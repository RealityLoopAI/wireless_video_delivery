#include "gwv3_common/protocol.hpp"
#include "gwv3_sender/adaptive_exposure_controller.hpp"
#include "gwv3_sender/clock_sync_client.hpp"
#include "gwv3_sender/config.hpp"
#include "gwv3_sender/gst_h264_encoder.hpp"
#include "gwv3_sender/logger.hpp"
#include "gwv3_sender/rgb_transport_recovery.hpp"
#include "gwv3_sender/transport.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cerrno>
#include <cctype>
#include <csetjmp>
#include <cstdio>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <vector>

#include <json/json.h>
#include <jpeglib.h>
#include <libobsensor/ObSensor.hpp>
#include <libobsensor/hpp/Error.hpp>
#include <libobsensor/h/Version.h>
#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <zlib.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace gwv3 {

namespace {

std::atomic<bool> g_running{true};
std::mutex g_camera_lifecycle_mutex;
constexpr uint16_t kDepthPreviewMinMm = 250;
constexpr uint16_t kDepthPreviewMaxMm = 2500;
constexpr size_t kMaxActiveCameras = 4;
constexpr uint64_t kAlignedRgbPreviewTargetDeltaUs = 15000;
constexpr uint64_t kAlignedRgbPreviewDisplayMaxDeltaUs = 33333;
constexpr size_t kMaxRgbPreviewQueueFrames = 8;
constexpr size_t kMaxRgbEncodeTimingFrames = 64;
constexpr int kAlignedRgbPreviewMinFps = 30;
constexpr auto kCameraAnnounceInterval = std::chrono::seconds(5);
constexpr auto kHotplugScanInterval = std::chrono::seconds(2);
constexpr auto kHotplugRetryCooldown = std::chrono::seconds(30);
constexpr auto kHotplugLimitEventInterval = std::chrono::seconds(30);
constexpr auto kWebRgbPreviewDefaultLease = std::chrono::milliseconds(2500);
constexpr auto kWebRgbPreviewMaxLease = std::chrono::seconds(10);
constexpr size_t kMediaSlotsPerCamera = 3;
constexpr size_t kDepthMediaQueuePerSlot = 4;
constexpr size_t kDepthCompressionQueuePerSlot = 4;
constexpr size_t kMediaQueueMaxBytesPerSlot = 256ull * 1024ull * 1024ull;
constexpr size_t kPreviewQueueMaxBytesPerSlot = 16ull * 1024ull * 1024ull;
constexpr size_t kDepthCompressionQueueMaxBytesPerSlot = 128ull * 1024ull * 1024ull;
constexpr uint32_t kJpegDualNoMainOutputFallbackFrames = 60;
constexpr uint64_t kRgbPtsMatchToleranceUs = 2000;
constexpr uint64_t kRgbEncoderOutputLagResetUs = 500000;
constexpr auto kWebRgbPreviewSuppressAfterEncoderLag = std::chrono::seconds(10);
constexpr auto kGracefulQueueDrainTimeout = std::chrono::seconds(10);
constexpr size_t kRgbSnapshotQueueMaxItems = 64;
constexpr size_t kRgbSnapshotQueueMaxBytes = 64ull * 1024ull * 1024ull;
constexpr size_t kRgbSnapshotMaxPendingPerCamera = 16;
constexpr int kRgbSnapshotJpegQuality = 95;
constexpr auto kRgbSnapshotRequestPollInterval = std::chrono::milliseconds(20);
constexpr auto kRgbSnapshotRetryInterval = std::chrono::seconds(1);
constexpr auto kRgbSnapshotRequestTimeout = std::chrono::seconds(30);
constexpr uint64_t kRgbRecoveryKeyframeRequestIntervalUs = 1'000'000;

const char *stream_type_name(StreamType stream_type) {
    switch(stream_type) {
    case StreamType::rgb:
        return "rgb";
    case StreamType::rgb_preview:
        return "rgb_preview";
    case StreamType::depth_raw:
        return "depth";
    case StreamType::rgb_snapshot:
        return "rgb_snapshot";
    }
    return "unknown";
}

void handle_signal(int) {
    g_running = false;
}

int even_dimension(int value) {
    value = std::max(2, value);
    return value % 2 == 0 ? value : value - 1;
}

struct WebRgbPreviewShape {
    int width = 0;
    int height = 0;
};

WebRgbPreviewShape resolve_web_rgb_preview_shape(const WebRgbPreviewConfig &preview, int source_width, int source_height) {
    if(!preview.enabled || source_width <= 0 || source_height <= 0) {
        return {};
    }
    const double scale_x = static_cast<double>(preview.max_width) / static_cast<double>(source_width);
    const double scale_y = static_cast<double>(preview.max_height) / static_cast<double>(source_height);
    const double scale = std::min(1.0, std::min(scale_x, scale_y));
    WebRgbPreviewShape shape;
    shape.width = even_dimension(static_cast<int>(std::round(static_cast<double>(source_width) * scale)));
    shape.height = even_dimension(static_cast<int>(std::round(static_cast<double>(source_height) * scale)));
    return shape;
}

bool jpeg_dual_encoder_experiment_enabled() {
    static const bool enabled = [] {
        const char *value = std::getenv("GEMINI_ENABLE_JPEG_TEE_ENCODER");
        if(!value) {
            return false;
        }
        const std::string text(value);
        return text == "1" || text == "true" || text == "TRUE" || text == "yes" || text == "YES";
    }();
    return enabled;
}

struct Args {
    std::string config_path = "06_configs/sender_orangepi5pro-01.json";
    int run_seconds = 0;
    bool validate_only = false;
    bool no_preview = false;
    bool no_local_preview = false;
    bool no_send = false;
};

struct CameraPerfStats {
    uint64_t wait_calls = 0;
    uint64_t wait_timeouts = 0;
    uint64_t rgb_input_frames = 0;
    uint64_t depth_input_frames = 0;
    uint64_t rgb_sent_packets = 0;
    uint64_t rgb_preview_sent_packets = 0;
    uint64_t depth_sent_frames = 0;
    uint64_t rgb_corrupt_jpeg_frames = 0;
    uint64_t rgb_send_failures = 0;
    uint64_t rgb_preview_send_failures = 0;
    uint64_t depth_send_failures = 0;
    uint64_t rgb_input_bytes = 0;
    uint64_t depth_input_bytes = 0;
    uint64_t rgb_bytes = 0;
    uint64_t rgb_preview_bytes = 0;
    uint64_t depth_bytes = 0;
    bool rgb_frame_id_seen = false;
    bool depth_frame_id_seen = false;
    uint64_t rgb_first_frame_id = 0;
    uint64_t rgb_last_frame_id = 0;
    uint64_t depth_first_frame_id = 0;
    uint64_t depth_last_frame_id = 0;
    double wait_ms = 0.0;
    double rgb_decode_ms = 0.0;
    double rgb_encode_ms = 0.0;
    double rgb_send_ms = 0.0;
    double rgb_preview_send_ms = 0.0;
    double depth_compress_ms = 0.0;
    double depth_send_ms = 0.0;
    double depth_preview_ms = 0.0;
    double preview_ms = 0.0;
    uint64_t rgb_timing_mismatch_drops = 0;
    uint64_t rgb_encoder_lag_resets = 0;
    std::chrono::steady_clock::time_point interval_started = std::chrono::steady_clock::now();

    void note_rgb_frame_id(uint64_t frame_id) {
        if(!rgb_frame_id_seen) {
            rgb_first_frame_id = frame_id;
            rgb_frame_id_seen = true;
        }
        rgb_last_frame_id = frame_id;
    }

    void note_depth_frame_id(uint64_t frame_id) {
        if(!depth_frame_id_seen) {
            depth_first_frame_id = frame_id;
            depth_frame_id_seen = true;
        }
        depth_last_frame_id = frame_id;
    }
};

struct CameraLiveStats {
    double rgb_input_fps = 0.0;
    double depth_input_fps = 0.0;
    double rgb_sent_fps = 0.0;
    double rgb_preview_sent_fps = 0.0;
    double depth_sent_fps = 0.0;
    double rgb_usb_mbps = 0.0;
    double depth_usb_mbps = 0.0;
    double rgb_mbps = 0.0;
    double rgb_preview_mbps = 0.0;
    double depth_mbps = 0.0;
    double rgb_encode_ms = 0.0;
    double rgb_preview_send_ms = 0.0;
    double depth_compress_ms = 0.0;
    int64_t color_auto_exposure = -1;
    int64_t color_exposure = -1;
    int64_t color_gain = -1;
    int64_t color_actual_fps = -1;
    int64_t color_frame_rate = -1;
    int64_t color_exposure_priority = -1;
    bool adaptive_exposure_enabled = false;
    int adaptive_luma_p50 = -1;
    int adaptive_luma_p95 = -1;
    int adaptive_luma_p99 = -1;
    double adaptive_highlight_fraction = 0.0;
    int adaptive_requested_exposure = -1;
    int adaptive_requested_gain = -1;
    uint64_t adaptive_samples = 0;
    uint64_t adaptive_adjustments = 0;
    uint64_t adaptive_failures = 0;
    std::string adaptive_last_reason;
};

struct RgbPreviewFrame {
    cv::Mat bgr;
    uint64_t frame_id = 0;
    uint64_t system_timestamp_us = 0;
};

struct RgbFrameDiagnostics {
    int32_t exposure_us = -1;
    int32_t gain = -1;
    int32_t auto_exposure = -1;
    int32_t actual_fps = -1;
};

struct RgbEncodeTiming {
    uint64_t frame_id = 0;
    uint64_t device_timestamp_us = 0;
    uint64_t system_timestamp_us = 0;
    uint64_t pair_id = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    RgbFrameDiagnostics diagnostics;
    uint64_t capture_host_timestamp_us = 0;
    uint64_t timing_bound_timestamp_us = 0;
    uint64_t encode_start_timestamp_us = 0;
    uint64_t encode_done_timestamp_us = 0;
    uint64_t packet_queued_timestamp_us = 0;
};

struct RgbSnapshotRequest {
    std::string request_id;
    std::string camera_id;
    std::string result_path;
    std::string trigger;
    uint64_t requested_at_unix_us = 0;
    uint64_t capture_not_before_unix_us = 0;
};

struct PendingRgbSnapshot {
    RgbSnapshotRequest request;
    std::chrono::steady_clock::time_point queued_at;
};

class V4L2MjpegCapture;
class ReliableSnapshotQueue;

struct CameraRuntime {
    mutable std::mutex mutex;
    CameraConfig config;
    std::string device_model;
    std::string device_serial;
    std::string device_uid;
    std::string device_connection_type;
    std::shared_ptr<ob::Device> device;
    std::unique_ptr<ob::Pipeline> pipeline;
    std::shared_ptr<ob::Config> pipeline_config;
    std::shared_ptr<V4L2MjpegCapture> v4l2_capture;
    std::shared_ptr<ob::VideoStreamProfile> color_profile;
    std::shared_ptr<ob::VideoStreamProfile> depth_profile;
    std::unique_ptr<GstH264Encoder> encoder;
    std::unique_ptr<GstH264Encoder> web_preview_encoder;
    std::unique_ptr<GstJpegDualH264Encoder> jpeg_dual_encoder;
    std::unique_ptr<AdaptiveExposureController> adaptive_exposure_controller;
    bool jpeg_dual_encoder_disabled = false;
    uint32_t jpeg_dual_no_main_output = 0;
    GstH264InputFormat encoder_input_format = GstH264InputFormat::Bgr;
    GstH264InputFormat web_preview_encoder_input_format = GstH264InputFormat::Bgr;
    uint32_t web_preview_width = 0;
    uint32_t web_preview_height = 0;
    bool announced = false;
    bool online = false;
    bool hotplug_dynamic = false;
    bool reconnect_enabled = true;
    bool hardware_encoder = false;
    uint32_t reconnect_attempts = 0;
    uint32_t disconnects = 0;
    std::deque<std::chrono::steady_clock::time_point> recent_disconnects;
    uint64_t rgb_frames = 0;
    uint64_t depth_frames = 0;
    RgbTransportRecovery rgb_transport_recovery;
    uint64_t rgb_transport_retry_drops = 0;
    uint64_t rgb_send_failures_total = 0;
    uint64_t rgb_preview_send_failures_total = 0;
    uint64_t depth_send_failures_total = 0;
    uint64_t force_rgb_keyframe_requests = 0;
    uint64_t force_rgb_keyframe_applied = 0;
    uint64_t force_rgb_keyframe_observed = 0;
    uint64_t force_rgb_keyframe_requested_at_us = 0;
    uint64_t rgb_corrupt_jpeg = 0;
    uint64_t rgb_dropped = 0;
    uint64_t rgb_timing_mismatch_drops = 0;
    uint64_t depth_dropped = 0;
    bool rgb_sent_timing_seen = false;
    uint64_t rgb_last_sent_frame_id = 0;
    uint64_t rgb_last_sent_system_timestamp_us = 0;
    uint64_t rgb_last_capture_system_timestamp_us = 0;
    uint64_t depth_last_capture_system_timestamp_us = 0;
    uint64_t capture_timestamp_fallbacks = 0;
    uint64_t next_pair_id = 1;
    uint32_t media_outage_samples = 0;
    uint32_t capture_stall_samples = 0;
    std::string last_error;
    float depth_scale = 0.0f;
    std::chrono::steady_clock::time_point stats_started = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point next_preview = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point next_web_rgb_preview = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point web_rgb_preview_requested_until = std::chrono::steady_clock::time_point::min();
    std::chrono::steady_clock::time_point next_depth_emit = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point next_reconnect = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point next_time_sync_log = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point next_jpeg_warning = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point next_media_warning = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point next_keyframe_guard_warning = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point next_rgb_timing_warning = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point next_rgb_encoder_lag_warning = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point last_rgb_frame_at = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point last_depth_frame_at = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point next_capture_stall_reconnect = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point web_rgb_preview_suppressed_until = std::chrono::steady_clock::time_point::min();
    std::chrono::steady_clock::time_point gemini305_manual_exposure_reapply_at = std::chrono::steady_clock::time_point::min();
    std::chrono::steady_clock::time_point next_adaptive_exposure_sample = std::chrono::steady_clock::time_point::min();
    std::chrono::steady_clock::time_point next_adaptive_exposure_warning = std::chrono::steady_clock::time_point::min();
    bool gemini305_manual_exposure_reapply_pending = false;
    bool publish_warmup_active = false;
    bool publish_warmup_exposure_verified = true;
    uint64_t publish_warmup_dropped_framesets = 0;
    std::chrono::steady_clock::time_point publish_not_before = std::chrono::steady_clock::time_point::min();
    std::chrono::steady_clock::time_point publish_warmup_deadline = std::chrono::steady_clock::time_point::min();
    std::string last_media_warning;
    cv::Mat latest_bgr;
    cv::Mat latest_depth_color;
    uint64_t latest_rgb_frame_id = 0;
    uint64_t latest_rgb_system_timestamp_us = 0;
    std::deque<RgbPreviewFrame> rgb_preview_queue;
    std::deque<RgbEncodeTiming> rgb_encode_timings;
    std::deque<RgbSnapshotRequest> rgb_snapshot_requests;
    std::map<std::string, PendingRgbSnapshot> pending_rgb_snapshots;
    ReliableSnapshotQueue *rgb_snapshot_queue = nullptr;
    CameraRuntime *depth_remap_target = nullptr;
    CameraPerfStats perf;
    CameraLiveStats live;
};

struct MediaPacketJob {
    CameraRuntime *camera = nullptr;
    StreamType stream_type = StreamType::rgb;
    std::vector<uint8_t> header;
    std::vector<uint8_t> owned_payload;
    std::shared_ptr<const void> payload_owner;
    const uint8_t *external_payload = nullptr;
    size_t external_payload_size = 0;
    bool rgb_keyframe = false;

    MediaPacketView view() const {
        const uint8_t *payload_data = owned_payload.empty() ? external_payload : owned_payload.data();
        const size_t payload_size = owned_payload.empty() ? external_payload_size : owned_payload.size();
        return MediaPacketView{header.data(), header.size(), payload_data, payload_size};
    }

    size_t total_size() const { return header.size() + (owned_payload.empty() ? external_payload_size : owned_payload.size()); }
};

struct ReliableSnapshotJob {
    MediaPacketJob media;
    MediaFrameMeta meta;
    std::string request_id;
    int rotation_degrees = 0;
    std::chrono::steady_clock::time_point first_queued_at = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point next_attempt_at = std::chrono::steady_clock::now();
    uint32_t attempts = 0;
};

class ReliableSnapshotQueue {
public:
    bool publish(ReliableSnapshotJob job) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(stopping_ || jobs_.size() >= kRgbSnapshotQueueMaxItems) {
            return false;
        }
        const size_t job_bytes = job.media.total_size();
        if(job_bytes == 0 || job_bytes > kRgbSnapshotQueueMaxBytes || bytes_ > kRgbSnapshotQueueMaxBytes - job_bytes) {
            return false;
        }
        cancelled_.erase(job.request_id);
        bytes_ += job_bytes;
        jobs_.push_back(std::move(job));
        cv_.notify_one();
        return true;
    }

    std::optional<ReliableSnapshotJob> wait_pop(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for(;;) {
            discard_cancelled_locked();
            if(stopping_) {
                return std::nullopt;
            }
            const auto now = std::chrono::steady_clock::now();
            auto ready = std::find_if(jobs_.begin(), jobs_.end(), [&](const auto &job) {
                return job.next_attempt_at <= now;
            });
            if(ready != jobs_.end()) {
                auto job = std::move(*ready);
                bytes_ -= std::min(bytes_, job.media.total_size());
                jobs_.erase(ready);
                return job;
            }
            auto wake_at = deadline;
            for(const auto &job : jobs_) {
                wake_at = std::min(wake_at, job.next_attempt_at);
            }
            if(cv_.wait_until(lock, wake_at) == std::cv_status::timeout
               && std::chrono::steady_clock::now() >= deadline) {
                return std::nullopt;
            }
        }
    }

    bool requeue(ReliableSnapshotJob job, std::chrono::steady_clock::time_point next_attempt_at) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(stopping_ || cancelled_.count(job.request_id) != 0) {
            cancelled_.erase(job.request_id);
            return false;
        }
        const size_t job_bytes = job.media.total_size();
        if(jobs_.size() >= kRgbSnapshotQueueMaxItems || job_bytes > kRgbSnapshotQueueMaxBytes
           || bytes_ > kRgbSnapshotQueueMaxBytes - job_bytes) {
            return false;
        }
        job.next_attempt_at = next_attempt_at;
        bytes_ += job_bytes;
        jobs_.push_back(std::move(job));
        cv_.notify_one();
        return true;
    }

    void cancel(const std::string &request_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        cancelled_.insert(request_id);
        discard_cancelled_locked();
        cv_.notify_all();
    }

    void stop() {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        jobs_.clear();
        bytes_ = 0;
        cv_.notify_all();
    }

private:
    void discard_cancelled_locked() {
        for(auto it = jobs_.begin(); it != jobs_.end();) {
            if(cancelled_.count(it->request_id) == 0) {
                ++it;
                continue;
            }
            bytes_ -= std::min(bytes_, it->media.total_size());
            cancelled_.erase(it->request_id);
            it = jobs_.erase(it);
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<ReliableSnapshotJob> jobs_;
    std::set<std::string> cancelled_;
    size_t bytes_ = 0;
    bool stopping_ = false;
};

class LatestMediaQueue {
public:
    explicit LatestMediaQueue(size_t slot_count, size_t rgb_frames_per_slot = 1, size_t depth_frames_per_slot = kDepthMediaQueuePerSlot)
        : slots_(slot_count),
          rgb_frames_per_slot_(std::max<size_t>(1, rgb_frames_per_slot)),
          depth_frames_per_slot_(std::max<size_t>(1, depth_frames_per_slot)) {}

    size_t append_slots(size_t slot_count) {
        std::lock_guard<std::mutex> lock(mutex_);
        const size_t first_slot = slots_.size();
        slots_.resize(first_slot + slot_count);
        return first_slot;
    }

    enum class PublishResult {
        queued,
        overwritten,
        rejected_occupied,
        rejected_stopping,
    };

    PublishResult publish(size_t slot_index, MediaPacketJob &&job, bool reject_if_occupied = false) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(stopping_ || slot_index >= slots_.size()) {
            return PublishResult::rejected_stopping;
        }
        auto &slot = slots_[slot_index];
        const bool occupied = !slot.jobs.empty();
        if(occupied && reject_if_occupied) {
            return PublishResult::rejected_occupied;
        }
        const size_t job_bytes = job.total_size();
        const size_t max_bytes = job.stream_type == StreamType::rgb_preview ? kPreviewQueueMaxBytesPerSlot
                                                                            : kMediaQueueMaxBytesPerSlot;
        if(job_bytes > max_bytes) {
            return PublishResult::rejected_occupied;
        }
        const auto discard_front = [&] {
            slot.bytes -= std::min(slot.bytes, slot.jobs.front().total_size());
            slot.jobs.pop_front();
        };
        bool dropped = false;
        if(job.stream_type == StreamType::rgb_preview) {
            dropped = occupied;
            slot.jobs.clear();
            slot.bytes = 0;
            slot.jobs.push_back(std::move(job));
        }
        else if(job.stream_type == StreamType::depth_raw) {
            while(!slot.jobs.empty()
                  && (slot.jobs.size() >= depth_frames_per_slot_ || job_bytes > max_bytes - std::min(slot.bytes, max_bytes))) {
                discard_front();
                dropped = true;
            }
            slot.jobs.push_back(std::move(job));
        }
        else {
            while(!slot.jobs.empty()
                  && (slot.jobs.size() >= rgb_frames_per_slot_ || job_bytes > max_bytes - std::min(slot.bytes, max_bytes))) {
                discard_front();
                dropped = true;
            }
            slot.jobs.push_back(std::move(job));
        }
        slot.bytes += job_bytes;
        cv_.notify_one();
        return dropped ? PublishResult::overwritten : PublishResult::queued;
    }

    std::optional<MediaPacketJob> wait_pop(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, timeout, [&] { return stopping_ || has_packet_locked(); });
        if(!has_packet_locked()) {
            return std::nullopt;
        }

        if(auto job = pop_next_locked([](StreamType stream_type) { return stream_type != StreamType::rgb_preview; })) {
            return job;
        }
        return pop_next_locked([](StreamType) { return true; });
    }

    void stop() {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        cv_.notify_all();
    }

    bool has_pending_primary() {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::any_of(slots_.begin(), slots_.end(), [](const auto &slot) {
            return !slot.jobs.empty() && slot.jobs.front().stream_type != StreamType::rgb_preview;
        });
    }

    bool empty() {
        std::lock_guard<std::mutex> lock(mutex_);
        return !has_packet_locked();
    }

private:
    struct Slot {
        std::deque<MediaPacketJob> jobs;
        size_t bytes = 0;
    };

    template <typename Predicate>
    std::optional<MediaPacketJob> pop_next_locked(Predicate predicate) {
        for(size_t offset = 0; offset < slots_.size(); ++offset) {
            const size_t index = (next_slot_ + offset) % slots_.size();
            if(!slots_[index].jobs.empty() && predicate(slots_[index].jobs.front().stream_type)) {
                auto job = std::move(slots_[index].jobs.front());
                slots_[index].jobs.pop_front();
                slots_[index].bytes -= std::min(slots_[index].bytes, job.total_size());
                next_slot_ = (index + 1) % slots_.size();
                return job;
            }
        }
        return std::nullopt;
    }

    bool has_packet_locked() const {
        return std::any_of(slots_.begin(), slots_.end(), [](const auto &slot) { return !slot.jobs.empty(); });
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<Slot> slots_;
    size_t rgb_frames_per_slot_ = 1;
    size_t depth_frames_per_slot_ = kDepthMediaQueuePerSlot;
    size_t next_slot_ = 0;
    bool stopping_ = false;
};

struct DepthCompressionJob {
    CameraRuntime *source_camera = nullptr;
    CameraRuntime *output_camera = nullptr;
    LatestMediaQueue *output_media_queue = nullptr;
    size_t media_slot_index = 0;
    MediaFrameMeta meta;
    float depth_scale = 1.0f;
    double quantization_step_mm = 10.0;
    std::shared_ptr<const void> raw_payload_owner;
    const uint8_t *raw_payload = nullptr;
    size_t raw_payload_size = 0;
};

struct DepthCompressionWorkItem {
    size_t slot_index = 0;
    DepthCompressionJob job;
};

class LatestDepthCompressionQueue {
public:
    explicit LatestDepthCompressionQueue(size_t slot_count, size_t frames_per_slot = kDepthCompressionQueuePerSlot)
        : slots_(slot_count), frames_per_slot_(std::max<size_t>(1, frames_per_slot)) {}

    size_t append_slots(size_t slot_count) {
        std::lock_guard<std::mutex> lock(mutex_);
        const size_t first_slot = slots_.size();
        slots_.resize(first_slot + slot_count);
        return first_slot;
    }

    bool publish(size_t slot_index, DepthCompressionJob &&job) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(stopping_ || slot_index >= slots_.size()) {
            return false;
        }
        auto &slot = slots_[slot_index];
        const size_t job_bytes = job.raw_payload_size;
        if(job_bytes == 0 || job_bytes > kDepthCompressionQueueMaxBytesPerSlot) {
            return true;
        }
        bool dropped = false;
        while(!slot.jobs.empty()
              && (slot.jobs.size() >= frames_per_slot_
                  || job_bytes > kDepthCompressionQueueMaxBytesPerSlot
                                     - std::min(slot.bytes, kDepthCompressionQueueMaxBytesPerSlot))) {
            slot.bytes -= std::min(slot.bytes, slot.jobs.front().raw_payload_size);
            slot.jobs.pop_front();
            dropped = true;
        }
        slot.bytes += job_bytes;
        slot.jobs.push_back(std::move(job));
        cv_.notify_one();
        return dropped;
    }

    std::optional<DepthCompressionWorkItem> wait_pop(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, timeout, [&] { return stopping_ || has_job_locked(); });
        if(!has_job_locked()) {
            return std::nullopt;
        }
        for(size_t offset = 0; offset < slots_.size(); ++offset) {
            const size_t index = (next_slot_ + offset) % slots_.size();
            if(!slots_[index].jobs.empty() && !slots_[index].in_flight) {
                auto job = std::move(slots_[index].jobs.front());
                slots_[index].jobs.pop_front();
                slots_[index].bytes -= std::min(slots_[index].bytes, job.raw_payload_size);
                slots_[index].in_flight = true;
                next_slot_ = (index + 1) % slots_.size();
                return DepthCompressionWorkItem{index, std::move(job)};
            }
        }
        return std::nullopt;
    }

    void complete(size_t slot_index) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(slot_index < slots_.size()) {
            slots_[slot_index].in_flight = false;
        }
        cv_.notify_one();
    }

    void stop() {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        cv_.notify_all();
    }

    bool empty() {
        std::lock_guard<std::mutex> lock(mutex_);
        return !has_job_locked();
    }

    bool stopping() {
        std::lock_guard<std::mutex> lock(mutex_);
        return stopping_;
    }

private:
    struct Slot {
        std::deque<DepthCompressionJob> jobs;
        size_t bytes = 0;
        bool in_flight = false;
    };

    bool has_job_locked() const {
        return std::any_of(slots_.begin(), slots_.end(), [](const auto &slot) { return !slot.jobs.empty() && !slot.in_flight; });
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<Slot> slots_;
    size_t frames_per_slot_ = kDepthCompressionQueuePerSlot;
    size_t next_slot_ = 0;
    bool stopping_ = false;
};

Args parse_args(int argc, char **argv) {
    Args args;
    for(int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if(arg == "--config" && i + 1 < argc) {
            args.config_path = argv[++i];
        }
        else if(arg == "--run-seconds" && i + 1 < argc) {
            args.run_seconds = std::stoi(argv[++i]);
        }
        else if(arg == "--validate-config") {
            args.validate_only = true;
        }
        else if(arg == "--no-preview") {
            args.no_preview = true;
        }
        else if(arg == "--no-local-preview") {
            args.no_local_preview = true;
        }
        else if(arg == "--no-send") {
            args.no_send = true;
        }
        else {
            throw std::runtime_error("unknown argument: " + arg);
        }
    }
    return args;
}

std::string json_to_string(const Json::Value &value) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, value);
}

std::optional<Json::Value> parse_json_object(const std::string &payload) {
    Json::CharReaderBuilder builder;
    builder["collectComments"] = false;
    builder["failIfExtra"] = true;
    builder["strictRoot"] = true;
    Json::Value root;
    std::string errors;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if(!reader->parse(payload.data(), payload.data() + payload.size(), &root, &errors) || !root.isObject()) {
        return std::nullopt;
    }
    return root;
}

std::string json_string_or(const Json::Value &value, const char *key, const std::string &fallback = "") {
    return value.isMember(key) && value[key].isString() ? value[key].asString() : fallback;
}

bool json_bool_or(const Json::Value &value, const char *key, bool fallback) {
    return value.isMember(key) && value[key].isBool() ? value[key].asBool() : fallback;
}

int json_int_or(const Json::Value &value, const char *key, int fallback) {
    return value.isMember(key) && value[key].isInt() ? value[key].asInt() : fallback;
}

uint64_t json_uint64_or(const Json::Value &value, const char *key, uint64_t fallback) {
    if(!value.isMember(key)) {
        return fallback;
    }
    const auto &item = value[key];
    if(item.isUInt64()) {
        return item.asUInt64();
    }
    if(item.isInt64() && item.asInt64() >= 0) {
        return static_cast<uint64_t>(item.asInt64());
    }
    return fallback;
}

double elapsed_ms(std::chrono::steady_clock::time_point started, std::chrono::steady_clock::time_point ended) {
    return std::chrono::duration<double, std::milli>(ended - started).count();
}

double avg_ms(double total_ms, uint64_t count) {
    return count == 0 ? 0.0 : total_ms / static_cast<double>(count);
}

double rate_per_second(uint64_t count, double seconds) {
    return seconds <= 0.0 ? 0.0 : static_cast<double>(count) / seconds;
}

template <typename FrameT>
int64_t metadata_or(const std::shared_ptr<FrameT> &frame, OBFrameMetadataType type, int64_t fallback = -1) {
    try {
        if(frame && frame->hasMetadata(type)) {
            return frame->getMetadataValue(type);
        }
    }
    catch(const std::exception &) {
    }
    return fallback;
}

int32_t diagnostic_i32_or_unknown(int64_t value) {
    if(value < 0) {
        return -1;
    }
    if(value > INT32_MAX) {
        return INT32_MAX;
    }
    return static_cast<int32_t>(value);
}

std::string bool_text(bool value) {
    return value ? "true" : "false";
}

void log_property_set_result(CameraRuntime &camera, Logger &logger, const std::string &name, const std::string &value,
                             const std::string &readback = "") {
    std::ostringstream oss;
    oss << "color control set camera_id=" << camera.config.camera_id << " name=" << name << " value=" << value;
    if(!readback.empty()) {
        oss << " readback=" << readback;
    }
    logger.info(oss.str());
}

void log_property_unsupported(CameraRuntime &camera, Logger &logger, const std::string &name, OBPropertyID property_id) {
    std::ostringstream oss;
    oss << "color control unsupported camera_id=" << camera.config.camera_id << " name=" << name
        << " property_id=" << static_cast<int>(property_id);
    logger.warn(oss.str());
}

void set_bool_property_if_configured(CameraRuntime &camera, Logger &logger, const std::string &name, OBPropertyID property_id,
                                     const std::optional<bool> &value) {
    if(!value) {
        return;
    }
    try {
        if(!camera.device->isPropertySupported(property_id, OB_PERMISSION_WRITE)) {
            log_property_unsupported(camera, logger, name, property_id);
            return;
        }
        camera.device->setBoolProperty(property_id, *value);
        std::string readback;
        if(camera.device->isPropertySupported(property_id, OB_PERMISSION_READ)) {
            readback = bool_text(camera.device->getBoolProperty(property_id));
        }
        log_property_set_result(camera, logger, name, bool_text(*value), readback);
    }
    catch(const std::exception &e) {
        logger.warn("color control set failed camera_id=" + camera.config.camera_id + " name=" + name + " error=" + e.what());
    }
}

void set_int_property_if_configured(CameraRuntime &camera, Logger &logger, const std::string &name, OBPropertyID property_id,
                                    const std::optional<int> &value) {
    if(!value) {
        return;
    }
    try {
        if(!camera.device->isPropertySupported(property_id, OB_PERMISSION_WRITE)) {
            log_property_unsupported(camera, logger, name, property_id);
            return;
        }
        camera.device->setIntProperty(property_id, *value);
        std::string readback;
        if(camera.device->isPropertySupported(property_id, OB_PERMISSION_READ)) {
            readback = std::to_string(camera.device->getIntProperty(property_id));
        }
        log_property_set_result(camera, logger, name, std::to_string(*value), readback);
    }
    catch(const std::exception &e) {
        logger.warn("color control set failed camera_id=" + camera.config.camera_id + " name=" + name + " error=" + e.what());
    }
}

int software_rgb_rotation_degrees(const CameraConfig &config) {
    return config.rotation_degrees.value_or(0) == 180 ? 180 : 0;
}

void apply_stream_rotation(CameraRuntime &camera, Logger &logger) {
    if(!camera.config.rotation_degrees) {
        return;
    }

    struct RotationProperty {
        OBPropertyID id;
        const char *name;
        int desired_value;
        std::optional<int> previous_value;
    };

    const int configured_rotation = *camera.config.rotation_degrees;
    const int software_rgb_rotation = software_rgb_rotation_degrees(camera.config);
    std::vector<RotationProperty> properties{
        {OB_PROP_COLOR_ROTATE_INT, "color", software_rgb_rotation == 0 ? configured_rotation : 0, std::nullopt}};
    if(camera.config.depth_profile.enabled) {
        properties.push_back({OB_PROP_DEPTH_ROTATE_INT, "depth", configured_rotation, std::nullopt});
    }

    for(auto &property : properties) {
        if(!camera.device->isPropertySupported(property.id, OB_PERMISSION_WRITE)) {
            throw std::runtime_error(std::string("configured stream rotation is unsupported for ") + property.name
                                     + " camera_id=" + camera.config.camera_id);
        }
        if(camera.device->isPropertySupported(property.id, OB_PERMISSION_READ)) {
            property.previous_value = camera.device->getIntProperty(property.id);
        }
    }

    try {
        for(auto &property : properties) {
            camera.device->setIntProperty(property.id, property.desired_value);
        }
        for(const auto &property : properties) {
            if(camera.device->isPropertySupported(property.id, OB_PERMISSION_READ)) {
                const int readback = camera.device->getIntProperty(property.id);
                if(readback != property.desired_value) {
                    throw std::runtime_error(std::string(property.name) + " rotation readback=" + std::to_string(readback));
                }
            }
        }
    }
    catch(const std::exception &e) {
        for(auto property = properties.rbegin(); property != properties.rend(); ++property) {
            if(property->previous_value) {
                try {
                    camera.device->setIntProperty(property->id, *property->previous_value);
                }
                catch(...) {
                }
            }
        }
        throw std::runtime_error("cannot apply atomic RGB/Depth stream rotation camera_id=" + camera.config.camera_id
                                 + " error=" + e.what());
    }

    logger.info("stream rotation set camera_id=" + camera.config.camera_id
                + " degrees=" + std::to_string(configured_rotation)
                + " color=" + (software_rgb_rotation == 0 ? "hardware" : "software")
                + " depth=" + (camera.config.depth_profile.enabled ? "hardware" : "disabled"));
}

void apply_color_controls(CameraRuntime &camera, Logger &logger) {
    const auto &controls = camera.config.color_controls;
    if(!controls.auto_exposure && !controls.exposure && !controls.gain && !controls.auto_exposure_priority && !controls.max_exposure
       && !controls.max_gain && !controls.power_line_frequency && !controls.auto_white_balance && !controls.white_balance
       && !controls.brightness && !controls.contrast && !controls.saturation && !controls.gamma
       && !controls.backlight_compensation) {
        return;
    }

    if(controls.auto_exposure && !*controls.auto_exposure) {
        set_bool_property_if_configured(camera, logger, "auto_exposure", OB_PROP_COLOR_AUTO_EXPOSURE_BOOL, controls.auto_exposure);
    }
    set_int_property_if_configured(camera, logger, "auto_exposure_priority", OB_PROP_COLOR_AUTO_EXPOSURE_PRIORITY_INT,
                                   controls.auto_exposure_priority);
    set_int_property_if_configured(camera, logger, "max_exposure", OB_PROP_COLOR_AE_MAX_EXPOSURE_INT, controls.max_exposure);
    set_int_property_if_configured(camera, logger, "max_gain", GWV3_ORBBEC_MAX_GAIN_PROPERTY, controls.max_gain);
    set_int_property_if_configured(camera, logger, "power_line_frequency", OB_PROP_COLOR_POWER_LINE_FREQUENCY_INT,
                                   controls.power_line_frequency);
    set_int_property_if_configured(camera, logger, "exposure", OB_PROP_COLOR_EXPOSURE_INT, controls.exposure);
    set_int_property_if_configured(camera, logger, "gain", OB_PROP_COLOR_GAIN_INT, controls.gain);
    if(controls.auto_white_balance && !*controls.auto_white_balance) {
        set_bool_property_if_configured(camera, logger, "auto_white_balance", OB_PROP_COLOR_AUTO_WHITE_BALANCE_BOOL,
                                        controls.auto_white_balance);
    }
    set_int_property_if_configured(camera, logger, "white_balance", OB_PROP_COLOR_WHITE_BALANCE_INT, controls.white_balance);
    set_int_property_if_configured(camera, logger, "brightness", OB_PROP_COLOR_BRIGHTNESS_INT, controls.brightness);
    set_int_property_if_configured(camera, logger, "contrast", OB_PROP_COLOR_CONTRAST_INT, controls.contrast);
    set_int_property_if_configured(camera, logger, "saturation", OB_PROP_COLOR_SATURATION_INT, controls.saturation);
    set_int_property_if_configured(camera, logger, "gamma", OB_PROP_COLOR_GAMMA_INT, controls.gamma);
    set_int_property_if_configured(camera, logger, "backlight_compensation", OB_PROP_COLOR_BACKLIGHT_COMPENSATION_INT,
                                   controls.backlight_compensation);
    if(controls.auto_white_balance && *controls.auto_white_balance) {
        set_bool_property_if_configured(camera, logger, "auto_white_balance", OB_PROP_COLOR_AUTO_WHITE_BALANCE_BOOL,
                                        controls.auto_white_balance);
    }
    if(controls.auto_exposure && *controls.auto_exposure) {
        set_bool_property_if_configured(camera, logger, "auto_exposure", OB_PROP_COLOR_AUTO_EXPOSURE_BOOL, controls.auto_exposure);
    }
}

void update_color_metadata(CameraRuntime &camera, const std::shared_ptr<ob::ColorFrame> &color) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    camera.live.color_auto_exposure = metadata_or(color, OB_FRAME_METADATA_TYPE_AUTO_EXPOSURE, camera.live.color_auto_exposure);
    camera.live.color_exposure = metadata_or(color, OB_FRAME_METADATA_TYPE_EXPOSURE, camera.live.color_exposure);
    camera.live.color_gain = metadata_or(color, OB_FRAME_METADATA_TYPE_GAIN, camera.live.color_gain);
    camera.live.color_actual_fps = metadata_or(color, OB_FRAME_METADATA_TYPE_ACTUAL_FRAME_RATE, camera.live.color_actual_fps);
    camera.live.color_frame_rate = metadata_or(color, OB_FRAME_METADATA_TYPE_FRAME_RATE, camera.live.color_frame_rate);
    camera.live.color_exposure_priority = metadata_or(color, OB_FRAME_METADATA_TYPE_EXPOSURE_PRIORITY, camera.live.color_exposure_priority);
}

RgbFrameDiagnostics rgb_frame_diagnostics(CameraRuntime &camera, const std::shared_ptr<ob::ColorFrame> &color) {
    int64_t fallback_auto_exposure = -1;
    int64_t fallback_exposure = -1;
    int64_t fallback_gain = -1;
    int64_t fallback_actual_fps = -1;
    {
        std::lock_guard<std::mutex> lock(camera.mutex);
        fallback_auto_exposure = camera.live.color_auto_exposure;
        fallback_exposure = camera.live.color_exposure;
        fallback_gain = camera.live.color_gain;
        fallback_actual_fps = camera.live.color_actual_fps;
    }

    RgbFrameDiagnostics diagnostics;
    diagnostics.auto_exposure =
        diagnostic_i32_or_unknown(metadata_or(color, OB_FRAME_METADATA_TYPE_AUTO_EXPOSURE, fallback_auto_exposure));
    diagnostics.exposure_us = diagnostic_i32_or_unknown(metadata_or(color, OB_FRAME_METADATA_TYPE_EXPOSURE, fallback_exposure));
    diagnostics.gain = diagnostic_i32_or_unknown(metadata_or(color, OB_FRAME_METADATA_TYPE_GAIN, fallback_gain));
    diagnostics.actual_fps = diagnostic_i32_or_unknown(metadata_or(color, OB_FRAME_METADATA_TYPE_ACTUAL_FRAME_RATE, fallback_actual_fps));
    return diagnostics;
}

int media_outage_restart_samples() {
    const char *value = std::getenv("GEMINI_SENDER_MEDIA_OUTAGE_RESTART_SAMPLES");
    if(value == nullptr || value[0] == '\0') {
        return 5;
    }
    try {
        return std::max(1, std::stoi(value));
    }
    catch(const std::exception &) {
        return 5;
    }
}

double capture_outage_fps_floor() {
    return 5.0;
}

int capture_stall_seconds() {
    const char *value = std::getenv("GEMINI_SENDER_CAPTURE_STALL_SECONDS");
    if(value == nullptr || value[0] == '\0') {
        return 8;
    }
    try {
        return std::max(2, std::stoi(value));
    }
    catch(const std::exception &) {
        return 8;
    }
}

int capture_stall_restart_samples() {
    const char *value = std::getenv("GEMINI_SENDER_CAPTURE_STALL_RESTART_SAMPLES");
    if(value == nullptr || value[0] == '\0') {
        return 3;
    }
    try {
        return std::max(1, std::stoi(value));
    }
    catch(const std::exception &) {
        return 3;
    }
}

int capture_stall_reconnect_cooldown_seconds() {
    const char *value = std::getenv("GEMINI_SENDER_CAPTURE_STALL_RECONNECT_COOLDOWN_SECONDS");
    if(value == nullptr || value[0] == '\0') {
        return 30;
    }
    try {
        return std::max(5, std::stoi(value));
    }
    catch(const std::exception &) {
        return 30;
    }
}

int camera_flap_restart_events() {
    const char *value = std::getenv("GEMINI_SENDER_CAMERA_FLAP_RESTART_EVENTS");
    if(value == nullptr || value[0] == '\0') {
        return 3;
    }
    try {
        return std::max(0, std::stoi(value));
    }
    catch(const std::exception &) {
        return 3;
    }
}

int camera_flap_window_seconds() {
    const char *value = std::getenv("GEMINI_SENDER_CAMERA_FLAP_WINDOW_SECONDS");
    if(value == nullptr || value[0] == '\0') {
        return 300;
    }
    try {
        return std::max(10, std::stoi(value));
    }
    catch(const std::exception &) {
        return 300;
    }
}

std::string read_text_file(const std::filesystem::path &path);

int camera_reconnect_process_restart_attempts() {
    const char *value = std::getenv("GEMINI_SENDER_CAMERA_RECONNECT_RESTART_ATTEMPTS");
    if(value == nullptr || value[0] == '\0') {
        return 12;
    }
    try {
        return std::max(0, std::stoi(value));
    }
    catch(const std::exception &) {
        return 12;
    }
}

int camera_reconnect_max_delay_seconds() {
    const char *value = std::getenv("GEMINI_SENDER_CAMERA_RECONNECT_MAX_DELAY_SECONDS");
    if(value == nullptr || value[0] == '\0') {
        return 30;
    }
    try {
        return std::max(1, std::stoi(value));
    }
    catch(const std::exception &) {
        return 30;
    }
}

bool is_no_orbbec_device_error(const std::string &error) {
    return error.find("no Orbbec device found") != std::string::npos;
}

bool system_orbbec_usb_present() {
    const std::filesystem::path root = "/sys/bus/usb/devices";
    std::error_code ec;
    if(!std::filesystem::exists(root, ec)) {
        return false;
    }
    for(const auto &entry : std::filesystem::directory_iterator(root, std::filesystem::directory_options::skip_permission_denied, ec)) {
        if(ec) {
            return false;
        }
        if(read_text_file(entry.path() / "idVendor") == "2bc5") {
            return true;
        }
    }
    return false;
}

void update_camera_reconnect_process_guard(CameraRuntime &camera, Logger &logger, uint32_t attempts, const std::string &error) {
    const int restart_attempts = camera_reconnect_process_restart_attempts();
    if(restart_attempts <= 0 || attempts < static_cast<uint32_t>(restart_attempts)) {
        return;
    }
    if(!is_no_orbbec_device_error(error) || !system_orbbec_usb_present()) {
        return;
    }

    logger.error("camera reconnect guard exiting sender for watchdog restart camera_id=" + camera.config.camera_id
                 + " attempts=" + std::to_string(attempts)
                 + " threshold=" + std::to_string(restart_attempts)
                 + " error=\"" + error + "\""
                 + " reason=orbbec_usb_present_but_sdk_enumeration_failed");
    g_running = false;
}

void update_media_outage_guard(CameraRuntime &camera, Logger &logger, const CameraPerfStats &perf) {
    const bool rgb_outage = camera.live.rgb_input_fps >= capture_outage_fps_floor() && perf.rgb_sent_packets == 0 && perf.rgb_send_failures > 0;
    const bool depth_outage = camera.live.depth_input_fps >= capture_outage_fps_floor()
                              && perf.depth_sent_frames == 0 && perf.depth_send_failures > 0;

    if(rgb_outage || depth_outage) {
        camera.media_outage_samples++;
    }
    else {
        camera.media_outage_samples = 0;
        return;
    }

    const int restart_samples = media_outage_restart_samples();
    if(camera.media_outage_samples < static_cast<uint32_t>(restart_samples)) {
        return;
    }

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    oss << "media outage guard stopping sender for watchdog restart camera_id=" << camera.config.camera_id
        << " samples=" << camera.media_outage_samples
        << " threshold=" << restart_samples
        << " rgb_input_fps=" << camera.live.rgb_input_fps
        << " depth_input_fps=" << camera.live.depth_input_fps
        << " rgb_sent_packets_s=" << camera.live.rgb_sent_fps
        << " depth_sent_fps=" << camera.live.depth_sent_fps
        << " rgb_send_failures=" << perf.rgb_send_failures
        << " depth_send_failures=" << perf.depth_send_failures;
    if(!camera.last_error.empty()) {
        oss << " last_error=" << camera.last_error;
    }
    logger.error(oss.str());
    g_running = false;
}

void update_capture_stall_guard(CameraRuntime &camera, Logger &logger, const CameraPerfStats &perf,
                                std::chrono::steady_clock::time_point now) {
    if(!camera.online) {
        camera.capture_stall_samples = 0;
        return;
    }

    const int stall_seconds = capture_stall_seconds();
    const auto stall_threshold = std::chrono::seconds(stall_seconds);
    const auto rgb_stale_for = now - camera.last_rgb_frame_at;
    const auto depth_stale_for = now - camera.last_depth_frame_at;
    const bool rgb_stalled = rgb_stale_for >= stall_threshold;
    const bool depth_stalled = camera.config.depth_profile.enabled && depth_stale_for >= stall_threshold;
    const bool worker_stuck = perf.wait_calls == 0 && (rgb_stalled || depth_stalled);
    if(!worker_stuck) {
        camera.capture_stall_samples = 0;
        return;
    }

    camera.capture_stall_samples++;
    const int restart_samples = capture_stall_restart_samples();
    if(camera.capture_stall_samples < static_cast<uint32_t>(restart_samples)) {
        return;
    }

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    oss << "capture worker stall guard exiting sender for watchdog restart camera_id=" << camera.config.camera_id
        << " samples=" << camera.capture_stall_samples
        << " threshold_samples=" << restart_samples
        << " stale_threshold_s=" << stall_seconds
        << " rgb_stale_s=" << std::chrono::duration<double>(rgb_stale_for).count()
        << " depth_stale_s=" << std::chrono::duration<double>(depth_stale_for).count()
        << " rgb_input_fps=" << camera.live.rgb_input_fps
        << " depth_input_fps=" << camera.live.depth_input_fps
        << " wait_calls=" << perf.wait_calls
        << " wait_timeouts=" << perf.wait_timeouts;
    if(!camera.last_error.empty()) {
        oss << " last_error=" << camera.last_error;
    }
    logger.error(oss.str());
    std::_Exit(75);
}

std::optional<std::string> capture_stream_stall_reason(CameraRuntime &camera, std::chrono::steady_clock::time_point now) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    if(!camera.online) {
        return std::nullopt;
    }
    if(now < camera.next_capture_stall_reconnect) {
        return std::nullopt;
    }

    const int stall_seconds = capture_stall_seconds();
    const auto stall_threshold = std::chrono::seconds(stall_seconds);
    const auto rgb_stale_for = now - camera.last_rgb_frame_at;
    const auto depth_stale_for = now - camera.last_depth_frame_at;
    const bool rgb_stalled = rgb_stale_for >= stall_threshold;
    const bool depth_stalled = camera.config.depth_profile.enabled && depth_stale_for >= stall_threshold;
    if(!rgb_stalled && !depth_stalled) {
        return std::nullopt;
    }

    const int cooldown_seconds = capture_stall_reconnect_cooldown_seconds();
    camera.next_capture_stall_reconnect = now + std::chrono::seconds(cooldown_seconds);

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    oss << "capture stream stalled stale_threshold_s=" << stall_seconds
        << " reconnect_cooldown_s=" << cooldown_seconds
        << " rgb_stale_s=" << std::chrono::duration<double>(rgb_stale_for).count()
        << " depth_stale_s=" << std::chrono::duration<double>(depth_stale_for).count();
    return oss.str();
}

void log_perf(CameraRuntime &camera, Logger &logger, std::chrono::steady_clock::time_point now) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    auto &perf = camera.perf;
    const double seconds = std::chrono::duration<double>(now - perf.interval_started).count();
    if(seconds <= 0.0) {
        return;
    }

    const uint64_t rgb_frame_id_delta =
        perf.rgb_frame_id_seen && perf.rgb_last_frame_id >= perf.rgb_first_frame_id ? perf.rgb_last_frame_id - perf.rgb_first_frame_id : 0;
    const uint64_t depth_frame_id_delta =
        perf.depth_frame_id_seen && perf.depth_last_frame_id >= perf.depth_first_frame_id ? perf.depth_last_frame_id - perf.depth_first_frame_id : 0;
    camera.live.rgb_input_fps = rate_per_second(perf.rgb_input_frames, seconds);
    camera.live.depth_input_fps = rate_per_second(perf.depth_input_frames, seconds);
    camera.live.rgb_sent_fps = rate_per_second(perf.rgb_sent_packets, seconds);
    camera.live.rgb_preview_sent_fps = rate_per_second(perf.rgb_preview_sent_packets, seconds);
    camera.live.depth_sent_fps = rate_per_second(perf.depth_sent_frames, seconds);
    camera.live.rgb_usb_mbps = seconds > 0.0 ? static_cast<double>(perf.rgb_input_bytes) * 8.0 / seconds / 1000000.0 : 0.0;
    camera.live.depth_usb_mbps = seconds > 0.0 ? static_cast<double>(perf.depth_input_bytes) * 8.0 / seconds / 1000000.0 : 0.0;
    camera.live.rgb_mbps = seconds > 0.0 ? static_cast<double>(perf.rgb_bytes) * 8.0 / seconds / 1000000.0 : 0.0;
    camera.live.rgb_preview_mbps = seconds > 0.0 ? static_cast<double>(perf.rgb_preview_bytes) * 8.0 / seconds / 1000000.0 : 0.0;
    camera.live.depth_mbps = seconds > 0.0 ? static_cast<double>(perf.depth_bytes) * 8.0 / seconds / 1000000.0 : 0.0;
    camera.live.rgb_encode_ms = avg_ms(perf.rgb_encode_ms, perf.rgb_input_frames);
    camera.live.rgb_preview_send_ms = avg_ms(perf.rgb_preview_send_ms, perf.rgb_preview_sent_packets + perf.rgb_preview_send_failures);
    camera.live.depth_compress_ms = avg_ms(perf.depth_compress_ms, perf.depth_input_frames);

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    oss << "perf camera_id=" << camera.config.camera_id << " interval_s=" << seconds
        << " rgb_input_fps=" << camera.live.rgb_input_fps
        << " depth_input_fps=" << camera.live.depth_input_fps
        << " rgb_sent_packets_s=" << camera.live.rgb_sent_fps
        << " rgb_preview_sent_packets_s=" << camera.live.rgb_preview_sent_fps
        << " depth_sent_fps=" << camera.live.depth_sent_fps
        << " rgb_frame_id_delta=" << rgb_frame_id_delta
        << " depth_frame_id_delta=" << depth_frame_id_delta
        << " rgb_usb_mbps=" << camera.live.rgb_usb_mbps
        << " depth_usb_mbps=" << camera.live.depth_usb_mbps
        << " rgb_mbps=" << camera.live.rgb_mbps
        << " rgb_preview_mbps=" << camera.live.rgb_preview_mbps
        << " depth_mbps=" << camera.live.depth_mbps
        << " wait_avg_ms=" << avg_ms(perf.wait_ms, perf.wait_calls)
        << " wait_timeouts=" << perf.wait_timeouts
        << " rgb_decode_avg_ms=" << avg_ms(perf.rgb_decode_ms, perf.rgb_input_frames)
        << " rgb_encode_avg_ms=" << camera.live.rgb_encode_ms
        << " rgb_send_avg_ms=" << avg_ms(perf.rgb_send_ms, perf.rgb_sent_packets + perf.rgb_send_failures)
        << " rgb_preview_send_avg_ms=" << camera.live.rgb_preview_send_ms
        << " depth_compress_avg_ms=" << camera.live.depth_compress_ms
        << " depth_send_avg_ms=" << avg_ms(perf.depth_send_ms, perf.depth_sent_frames + perf.depth_send_failures)
        << " depth_preview_avg_ms=" << avg_ms(perf.depth_preview_ms, perf.depth_input_frames)
        << " preview_avg_ms=" << avg_ms(perf.preview_ms, perf.wait_calls)
        << " rgb_ae=" << camera.live.color_auto_exposure
        << " rgb_exposure=" << camera.live.color_exposure
        << " rgb_gain=" << camera.live.color_gain
        << " rgb_meta_actual_fps=" << camera.live.color_actual_fps
        << " rgb_exposure_priority=" << camera.live.color_exposure_priority
        << " adaptive_ae=" << bool_text(camera.live.adaptive_exposure_enabled)
        << " adaptive_p50=" << camera.live.adaptive_luma_p50
        << " adaptive_p95=" << camera.live.adaptive_luma_p95
        << " adaptive_p99=" << camera.live.adaptive_luma_p99
        << " adaptive_highlights_pct=" << camera.live.adaptive_highlight_fraction * 100.0
        << " adaptive_exposure=" << camera.live.adaptive_requested_exposure
        << " adaptive_gain=" << camera.live.adaptive_requested_gain
        << " adaptive_adjustments=" << camera.live.adaptive_adjustments
        << " adaptive_failures=" << camera.live.adaptive_failures
        << " rgb_corrupt_jpeg_frames=" << perf.rgb_corrupt_jpeg_frames
        << " rgb_timing_mismatch_drops=" << perf.rgb_timing_mismatch_drops
        << " rgb_encoder_lag_resets=" << perf.rgb_encoder_lag_resets
        << " rgb_send_failures=" << perf.rgb_send_failures
        << " rgb_preview_send_failures=" << perf.rgb_preview_send_failures
        << " depth_send_failures=" << perf.depth_send_failures;
    logger.info(oss.str());
    update_capture_stall_guard(camera, logger, perf, now);
    update_media_outage_guard(camera, logger, perf);

    CameraPerfStats reset;
    reset.interval_started = now;
    perf = reset;
}

template <typename FrameT>
uint64_t frame_system_timestamp_us_or(const std::shared_ptr<FrameT> &frame, uint64_t fallback_us) {
    if(!frame) {
        return fallback_us;
    }
    const uint64_t timestamp = frame->systemTimeStampUs();
    return timestamp == 0 ? fallback_us : timestamp;
}

uint64_t normalize_capture_system_timestamp(CameraRuntime &camera,
                                            StreamType stream_type,
                                            uint64_t sdk_timestamp_us,
                                            uint64_t capture_host_us) {
    constexpr uint64_t kMaxSdkHostSkewUs = 500'000;
    const uint64_t skew = sdk_timestamp_us >= capture_host_us ? sdk_timestamp_us - capture_host_us : capture_host_us - sdk_timestamp_us;
    bool used_fallback = sdk_timestamp_us == 0 || skew > kMaxSdkHostSkewUs;
    uint64_t timestamp_us = used_fallback ? capture_host_us : sdk_timestamp_us;
    std::lock_guard<std::mutex> lock(camera.mutex);
    uint64_t &last = stream_type == StreamType::rgb ? camera.rgb_last_capture_system_timestamp_us
                                                    : camera.depth_last_capture_system_timestamp_us;
    if(last > 0 && timestamp_us <= last) {
        used_fallback = true;
        timestamp_us = capture_host_us > last ? capture_host_us : last + 1;
    }
    if(used_fallback) {
        camera.capture_timestamp_fallbacks++;
    }
    last = timestamp_us;
    return timestamp_us;
}

template <typename FrameT>
void append_frame_time_sync(std::ostringstream &oss, const std::string &prefix, const std::shared_ptr<FrameT> &frame, uint64_t host_now_us) {
    if(!frame) {
        oss << " " << prefix << "_present=0";
        return;
    }

    const uint64_t camera_device_timestamp_us = frame->timeStampUs();
    const uint64_t camera_system_timestamp_us = frame_system_timestamp_us_or(frame, host_now_us);
    const int64_t host_minus_camera_system_us =
        host_now_us >= camera_system_timestamp_us ? static_cast<int64_t>(host_now_us - camera_system_timestamp_us)
                                                  : -static_cast<int64_t>(camera_system_timestamp_us - host_now_us);

    oss << " " << prefix << "_present=1"
        << " " << prefix << "_frame_id=" << frame->index()
        << " " << prefix << "_canonical_timestamp_us=" << camera_system_timestamp_us
        << " " << prefix << "_device_timestamp_us=" << camera_device_timestamp_us
        << " " << prefix << "_system_timestamp_us=" << camera_system_timestamp_us
        << " " << prefix << "_host_minus_system_us=" << host_minus_camera_system_us;
}

void log_time_sync(CameraRuntime &camera, Logger &logger, const std::shared_ptr<ob::ColorFrame> &color,
                   const std::shared_ptr<ob::DepthFrame> &depth, const std::string &depth_output_camera_id,
                   std::chrono::steady_clock::time_point now) {
    if(now < camera.next_time_sync_log) {
        return;
    }

    const uint64_t host_now = now_us();
    std::ostringstream oss;
    oss << "time_sync camera_id=" << camera.config.camera_id << " host_now_us=" << host_now;
    append_frame_time_sync(oss, "rgb", color, host_now);
    append_frame_time_sync(oss, "depth", depth, host_now);
    if(depth && !depth_output_camera_id.empty() && depth_output_camera_id != camera.config.camera_id) {
        oss << " depth_output_camera_id=" << depth_output_camera_id;
    }
    logger.info(oss.str());

    camera.next_time_sync_log = now + std::chrono::seconds(5);
}

Json::Value base_message(const AppConfig &config, const std::string &type) {
    Json::Value msg;
    msg["protocol_version"] = kProtocolVersion;
    msg["message_type"] = type;
    msg["sender_id"] = config.sender_id;
    msg["build_commit"] = GWV3_GIT_COMMIT;
    msg["build_dirty"] = GWV3_GIT_DIRTY != 0;
    msg["build_source_hash"] = GWV3_SENDER_SOURCE_HASH;
    msg["orbbec_sdk_version"] = std::to_string(ob_get_major_version()) + "." + std::to_string(ob_get_minor_version()) + "."
                                   + std::to_string(ob_get_patch_version());
    msg["orbbec_sdk_version_number"] = ob_get_version();
    msg["timestamp_us"] = Json::UInt64(now_us());
    return msg;
}

Json::Value profile_json(const std::shared_ptr<ob::VideoStreamProfile> &profile, const std::string &pixel_format, const std::string &codec_or_compression,
                         float depth_scale = 0.0f) {
    Json::Value value;
    value["width"] = profile ? profile->width() : 0;
    value["height"] = profile ? profile->height() : 0;
    value["fps"] = profile ? profile->fps() : 0;
    value["pixel_format"] = pixel_format;
    if(pixel_format == "uint16") {
        value["compression"] = codec_or_compression;
        value["depth_scale"] = depth_scale;
    }
    else {
        value["codec"] = codec_or_compression;
    }
    return value;
}

Json::Value profile_json(const VideoProfileConfig &profile, const std::string &pixel_format, const std::string &codec_or_compression,
                         float depth_scale = 0.0f) {
    Json::Value value;
    value["width"] = profile.enabled ? profile.width : 0;
    value["height"] = profile.enabled ? profile.height : 0;
    value["fps"] = profile.enabled ? profile.fps : 0;
    value["pixel_format"] = pixel_format;
    if(pixel_format == "uint16") {
        value["compression"] = codec_or_compression;
        value["depth_scale"] = depth_scale;
    }
    else {
        value["codec"] = codec_or_compression;
    }
    return value;
}

Json::Value intrinsic_json(const OBCameraIntrinsic &intrinsic) {
    Json::Value value;
    value["fx"] = intrinsic.fx;
    value["fy"] = intrinsic.fy;
    value["cx"] = intrinsic.cx;
    value["cy"] = intrinsic.cy;
    value["width"] = intrinsic.width;
    value["height"] = intrinsic.height;
    return value;
}

Json::Value distortion_json(const OBCameraDistortion &distortion) {
    Json::Value value;
    value["k1"] = distortion.k1;
    value["k2"] = distortion.k2;
    value["k3"] = distortion.k3;
    value["k4"] = distortion.k4;
    value["k5"] = distortion.k5;
    value["k6"] = distortion.k6;
    value["p1"] = distortion.p1;
    value["p2"] = distortion.p2;
    return value;
}

Json::Value transform_json(const OBD2CTransform &transform) {
    Json::Value value;
    Json::Value rot(Json::arrayValue);
    Json::Value trans(Json::arrayValue);
    for(float item : transform.rot) {
        rot.append(item);
    }
    for(float item : transform.trans) {
        trans.append(item);
    }
    value["rot"] = rot;
    value["trans_mm"] = trans;
    return value;
}

Json::Value camera_param_data_json(const OBCameraParam &camera_param) {
    Json::Value data;
    data["depth_intrinsic"] = intrinsic_json(camera_param.depthIntrinsic);
    data["rgb_intrinsic"] = intrinsic_json(camera_param.rgbIntrinsic);
    data["depth_distortion"] = distortion_json(camera_param.depthDistortion);
    data["rgb_distortion"] = distortion_json(camera_param.rgbDistortion);
    data["d2c_transform"] = transform_json(camera_param.transform);
    data["is_mirrored"] = camera_param.isMirrored;
    return data;
}

bool intrinsic_valid(const OBCameraIntrinsic &intrinsic) {
    return intrinsic.fx > 0.0f && intrinsic.fy > 0.0f && intrinsic.width > 0 && intrinsic.height > 0;
}

bool transform_valid(const OBD2CTransform &transform) {
    float abs_sum = 0.0f;
    for(float item : transform.rot) {
        abs_sum += std::abs(item);
    }
    for(float item : transform.trans) {
        abs_sum += std::abs(item);
    }
    return abs_sum > 0.0f;
}

bool camera_param_complete(const OBCameraParam &camera_param) {
    return intrinsic_valid(camera_param.depthIntrinsic) && intrinsic_valid(camera_param.rgbIntrinsic) && transform_valid(camera_param.transform);
}

bool profile_dimensions_match(const OBCameraIntrinsic &intrinsic, const std::shared_ptr<ob::VideoStreamProfile> &profile) {
    return profile && intrinsic.width > 0 && intrinsic.height > 0 && static_cast<uint32_t>(intrinsic.width) == profile->width()
           && static_cast<uint32_t>(intrinsic.height) == profile->height();
}

bool camera_param_profiles_match(const OBCameraParam &camera_param,
                                 const std::shared_ptr<ob::VideoStreamProfile> &color_profile,
                                 const std::shared_ptr<ob::VideoStreamProfile> &depth_profile) {
    return camera_param_complete(camera_param) && profile_dimensions_match(camera_param.rgbIntrinsic, color_profile)
           && profile_dimensions_match(camera_param.depthIntrinsic, depth_profile);
}

OBCameraParam camera_param_from_calibration(const OBCalibrationParam &calibration) {
    OBCameraParam camera_param{};
    camera_param.depthIntrinsic = calibration.intrinsics[OB_SENSOR_DEPTH];
    camera_param.rgbIntrinsic = calibration.intrinsics[OB_SENSOR_COLOR];
    camera_param.depthDistortion = calibration.distortion[OB_SENSOR_DEPTH];
    camera_param.rgbDistortion = calibration.distortion[OB_SENSOR_COLOR];
    camera_param.transform = calibration.extrinsics[OB_SENSOR_DEPTH][OB_SENSOR_COLOR];
    camera_param.isMirrored = false;
    return camera_param;
}

void fill_missing_profile_calibration(OBCameraParam &camera_param, const std::shared_ptr<ob::VideoStreamProfile> &color_profile,
                                      const std::shared_ptr<ob::VideoStreamProfile> &depth_profile) {
    if(color_profile && !intrinsic_valid(camera_param.rgbIntrinsic)) {
        try {
            camera_param.rgbIntrinsic = color_profile->getIntrinsic();
            camera_param.rgbDistortion = color_profile->getDistortion();
        }
        catch(const std::exception &) {
        }
        catch(...) {
        }
    }
    if(depth_profile && !intrinsic_valid(camera_param.depthIntrinsic)) {
        try {
            camera_param.depthIntrinsic = depth_profile->getIntrinsic();
            camera_param.depthDistortion = depth_profile->getDistortion();
        }
        catch(const std::exception &) {
        }
        catch(...) {
        }
    }
}

std::optional<OBCameraParam> exact_profile_camera_param_from_device(const std::shared_ptr<ob::Device> &device,
                                                                    const std::shared_ptr<ob::VideoStreamProfile> &color_profile,
                                                                    const std::shared_ptr<ob::VideoStreamProfile> &depth_profile,
                                                                    Json::Value &raw_list_json) {
    if(!device) {
        return std::nullopt;
    }
    try {
        auto list = device->getCalibrationCameraParamList();
        const uint32_t count = list ? list->count() : 0;
        std::optional<OBCameraParam> exact;
        for(uint32_t i = 0; i < count; ++i) {
            const auto param = list->getCameraParam(i);
            Json::Value item = camera_param_data_json(param);
            item["index"] = i;
            raw_list_json.append(item);
            if(!exact && camera_param_profiles_match(param, color_profile, depth_profile)) {
                exact = param;
            }
        }
        return exact;
    }
    catch(const std::exception &) {
    }
    catch(...) {
    }
    return std::nullopt;
}

Json::Value calibration_json(ob::Pipeline &pipeline, const std::shared_ptr<ob::Config> &pipeline_config,
                             const std::shared_ptr<ob::Device> &device,
                             const std::shared_ptr<ob::VideoStreamProfile> &color_profile,
                             const std::shared_ptr<ob::VideoStreamProfile> &depth_profile) {
    Json::Value calibration;
    calibration["available"] = false;
    calibration["source"] = "orbbec_sdk";
    calibration["data"] = Json::objectValue;
    Json::Value raw_list(Json::arrayValue);
    try {
        std::optional<OBCameraParam> selected_param;
        std::string source_detail;
        auto select_if_exact = [&](OBCameraParam candidate, const std::string &detail, bool fill_missing) {
            if(fill_missing) {
                fill_missing_profile_calibration(candidate, color_profile, depth_profile);
            }
            if(!selected_param && camera_param_profiles_match(candidate, color_profile, depth_profile)) {
                selected_param = candidate;
                source_detail = detail;
            }
        };

        if(pipeline_config && color_profile && depth_profile) {
            try {
                select_if_exact(camera_param_from_calibration(pipeline.getCalibrationParam(pipeline_config)),
                                "pipeline_config_calibration", false);
            }
            catch(const std::exception &) {
            }
            catch(...) {
            }
        }

        if(!selected_param && color_profile && depth_profile) {
            try {
                select_if_exact(pipeline.getCameraParamWithProfile(color_profile->width(), color_profile->height(), depth_profile->width(),
                                                                   depth_profile->height()),
                                "pipeline_profile_calibration", true);
            }
            catch(const std::exception &) {
            }
            catch(...) {
            }
        }

        if(!selected_param) {
            const auto exact_list_param = exact_profile_camera_param_from_device(device, color_profile, depth_profile, raw_list);
            if(exact_list_param) {
                select_if_exact(*exact_list_param, "device_calibration_list_exact_profile", false);
            }
        }

        if(!selected_param) {
            try {
                select_if_exact(pipeline.getCameraParam(), "pipeline_default_exact_profile", true);
            }
            catch(const std::exception &) {
            }
            catch(...) {
            }
        }

        Json::Value data = selected_param ? camera_param_data_json(*selected_param) : Json::Value(Json::objectValue);
        if(!selected_param && !raw_list.empty()) {
            constexpr Json::ArrayIndex kMaxRawCalibrationDiagnostics = 4;
            Json::Value bounded_raw_list(Json::arrayValue);
            const auto keep_count = std::min(raw_list.size(), kMaxRawCalibrationDiagnostics);
            for(Json::ArrayIndex i = 0; i < keep_count; ++i) {
                bounded_raw_list.append(raw_list[i]);
            }
            data["raw_camera_param_count"] = raw_list.size();
            data["raw_camera_param_list"] = bounded_raw_list;
            data["raw_camera_param_list_truncated"] = raw_list.size() > keep_count;
        }
        calibration["available"] = selected_param.has_value();
        calibration["profile_aware"] = selected_param.has_value();
        if(selected_param) {
            calibration["source_detail"] = source_detail;
        }
        else {
            calibration["warning"] = "no_complete_calibration_for_active_profiles";
        }
        calibration["data"] = data;
    }
    catch(const std::exception &) {
        calibration["available"] = false;
    }
    catch(...) {
        calibration["available"] = false;
    }
    return calibration;
}

std::string ob_format_name(OBFormat format) {
    switch(format) {
    case OB_FORMAT_MJPG:
        return "mjpg";
    case OB_FORMAT_RGB:
        return "rgb";
    case OB_FORMAT_BGR:
        return "bgr";
    case OB_FORMAT_YUYV:
        return "yuyv";
    case OB_FORMAT_UYVY:
        return "uyvy";
    case OB_FORMAT_Y16:
        return "y16";
    case OB_FORMAT_Y11:
        return "y11";
    case OB_FORMAT_Y12:
        return "y12";
    case OB_FORMAT_H264:
        return "h264";
    case OB_FORMAT_H265:
        return "h265";
    default:
        return "unknown";
    }
}

OBFormat requested_format(const std::string &format, OBFormat fallback) {
    if(format == "mjpg") {
        return OB_FORMAT_MJPG;
    }
    if(format == "rgb") {
        return OB_FORMAT_RGB;
    }
    if(format == "bgr") {
        return OB_FORMAT_BGR;
    }
    if(format == "yuyv") {
        return OB_FORMAT_YUYV;
    }
    if(format == "uyvy") {
        return OB_FORMAT_UYVY;
    }
    if(format == "y16") {
        return OB_FORMAT_Y16;
    }
    if(format == "y11") {
        return OB_FORMAT_Y11;
    }
    if(format == "y12") {
        return OB_FORMAT_Y12;
    }
    if(format.empty() || format == "default") {
        return fallback;
    }
    return fallback;
}

std::shared_ptr<ob::VideoStreamProfile> select_profile(ob::Pipeline &pipeline, OBSensorType sensor_type, const VideoProfileConfig &requested,
                                                       OBFormat fallback_format, Logger &logger) {
    auto profiles = pipeline.getStreamProfileList(sensor_type);
    const int width = requested.width > 0 ? requested.width : OB_WIDTH_ANY;
    const int height = requested.height > 0 ? requested.height : OB_HEIGHT_ANY;
    const int fps = requested.fps > 0 ? requested.fps : OB_FPS_ANY;
    const auto format = requested_format(requested.format, fallback_format);
    try {
        return profiles->getVideoStreamProfile(width, height, format, fps);
    }
    catch(const ob::Error &e) {
        logger.warn(std::string("requested profile unavailable, using SDK default: ") + e.getMessage());
        return profiles->getProfile(OB_PROFILE_DEFAULT)->as<ob::VideoStreamProfile>();
    }
}

std::string trim_copy(std::string value) {
    while(!value.empty() && (value.back() == '\n' || value.back() == '\r' || value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }
    size_t first = 0;
    while(first < value.size() && (value[first] == ' ' || value[first] == '\t')) {
        ++first;
    }
    return first == 0 ? value : value.substr(first);
}

std::string read_text_file(const std::filesystem::path &path) {
    std::ifstream file(path);
    if(!file) {
        return "";
    }
    std::string value;
    std::getline(file, value);
    return trim_copy(value);
}

int checked_v4l2_ioctl(int fd, unsigned long request, void *arg) {
    int result = 0;
    do {
        result = ioctl(fd, request, arg);
    } while(result == -1 && errno == EINTR);
    return result;
}

uint64_t v4l2_buffer_system_timestamp_us(const v4l2_buffer &buffer, uint64_t fallback_system_us) {
    const uint64_t raw_us = static_cast<uint64_t>(buffer.timestamp.tv_sec) * 1'000'000ull
                            + static_cast<uint64_t>(buffer.timestamp.tv_usec);
    if(raw_us == 0) {
        return fallback_system_us;
    }
#ifdef V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC
    if((buffer.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC) {
        const auto steady_now = std::chrono::steady_clock::now().time_since_epoch();
        const uint64_t steady_now_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(steady_now).count());
        if(raw_us <= steady_now_us + 1'000'000ull) {
            const int64_t age_us = static_cast<int64_t>(steady_now_us) - static_cast<int64_t>(raw_us);
            const int64_t mapped = static_cast<int64_t>(fallback_system_us) - age_us;
            return mapped > 0 ? static_cast<uint64_t>(mapped) : fallback_system_us;
        }
    }
#endif
    constexpr uint64_t kEarliestPlausibleEpochUs = 1'577'836'800ull * 1'000'000ull;
    return raw_us >= kEarliestPlausibleEpochUs ? raw_us : fallback_system_us;
}

std::string v4l2_fourcc_string(uint32_t fourcc) {
    std::string value(4, ' ');
    value[0] = static_cast<char>(fourcc & 0xff);
    value[1] = static_cast<char>((fourcc >> 8) & 0xff);
    value[2] = static_cast<char>((fourcc >> 16) & 0xff);
    value[3] = static_cast<char>((fourcc >> 24) & 0xff);
    return value;
}

bool v4l2_device_supports_mjpeg(const std::string &path) {
    const int fd = open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if(fd < 0) {
        return false;
    }
    v4l2_capability cap{};
    const bool capture = checked_v4l2_ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0
                         && ((cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) || (cap.device_caps & V4L2_CAP_VIDEO_CAPTURE));
    bool mjpeg = false;
    if(capture) {
        v4l2_fmtdesc desc{};
        desc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        for(desc.index = 0; checked_v4l2_ioctl(fd, VIDIOC_ENUM_FMT, &desc) == 0; ++desc.index) {
            if(desc.pixelformat == V4L2_PIX_FMT_MJPEG || desc.pixelformat == V4L2_PIX_FMT_JPEG) {
                mjpeg = true;
                break;
            }
        }
    }
    close(fd);
    return capture && mjpeg;
}

std::string find_v4l2_device_by_serial(const std::string &serial) {
    if(serial.empty()) {
        return "";
    }
    const std::filesystem::path root = "/sys/class/video4linux";
    std::error_code ec;
    if(!std::filesystem::exists(root, ec)) {
        return "";
    }
    std::vector<std::string> candidates;
    for(const auto &entry : std::filesystem::directory_iterator(root, std::filesystem::directory_options::skip_permission_denied, ec)) {
        if(ec) {
            return "";
        }
        const auto name = entry.path().filename().string();
        if(name.rfind("video", 0) != 0) {
            continue;
        }
        if(read_text_file(entry.path() / "device" / ".." / "serial") != serial) {
            continue;
        }
        candidates.push_back("/dev/" + name);
    }
    std::sort(candidates.begin(), candidates.end(), [](const std::string &lhs, const std::string &rhs) {
        const auto number = [](const std::string &path) {
            const auto pos = path.find_last_not_of("0123456789");
            return pos == std::string::npos ? 0 : std::stoi(path.substr(pos + 1));
        };
        return number(lhs) < number(rhs);
    });
    for(const auto &candidate : candidates) {
        if(v4l2_device_supports_mjpeg(candidate)) {
            return candidate;
        }
    }
    return candidates.empty() ? "" : candidates.front();
}

class V4L2MjpegCapture {
public:
    struct Frame {
        const uint8_t *data = nullptr;
        size_t size = 0;
        uint64_t frame_id = 0;
        uint64_t system_timestamp_us = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        int buffer_index = -1;
    };

    ~V4L2MjpegCapture() { close_device(); }

    void open_device(const CameraConfig &config) {
        device_path_ = !config.video_device.empty() ? config.video_device : find_v4l2_device_by_serial(config.serial_number);
        if(device_path_.empty()) {
            throw std::runtime_error("v4l2 video device not found for serial=" + config.serial_number);
        }
        fd_ = open(device_path_.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if(fd_ < 0) {
            throw std::runtime_error("failed to open v4l2 device " + device_path_ + ": " + std::strerror(errno));
        }

        v4l2_capability cap{};
        if(checked_v4l2_ioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0) {
            throw std::runtime_error("VIDIOC_QUERYCAP failed for " + device_path_ + ": " + std::strerror(errno));
        }
        const auto capabilities = cap.device_caps ? cap.device_caps : cap.capabilities;
        if((capabilities & V4L2_CAP_VIDEO_CAPTURE) == 0 || (capabilities & V4L2_CAP_STREAMING) == 0) {
            throw std::runtime_error("v4l2 device does not support capture streaming: " + device_path_);
        }

        v4l2_format fmt{};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = static_cast<uint32_t>(config.rgb_profile.width);
        fmt.fmt.pix.height = static_cast<uint32_t>(config.rgb_profile.height);
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
        fmt.fmt.pix.field = V4L2_FIELD_ANY;
        if(checked_v4l2_ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
            throw std::runtime_error("VIDIOC_S_FMT MJPG failed for " + device_path_ + ": " + std::strerror(errno));
        }
        if(fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG && fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_JPEG) {
            throw std::runtime_error("v4l2 device returned non-MJPEG format " + v4l2_fourcc_string(fmt.fmt.pix.pixelformat)
                                     + " for " + device_path_);
        }
        width_ = fmt.fmt.pix.width;
        height_ = fmt.fmt.pix.height;
        if(width_ != static_cast<uint32_t>(config.rgb_profile.width) || height_ != static_cast<uint32_t>(config.rgb_profile.height)) {
            std::ostringstream oss;
            oss << "v4l2 device returned unexpected size " << width_ << "x" << height_ << " for " << device_path_;
            throw std::runtime_error(oss.str());
        }

        v4l2_streamparm parm{};
        parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        parm.parm.capture.timeperframe.numerator = 1;
        parm.parm.capture.timeperframe.denominator = static_cast<uint32_t>(std::max(1, config.rgb_profile.fps));
        if(checked_v4l2_ioctl(fd_, VIDIOC_S_PARM, &parm) < 0) {
            throw std::runtime_error("VIDIOC_S_PARM failed for " + device_path_ + ": " + std::strerror(errno));
        }
        fps_ = parm.parm.capture.timeperframe.numerator > 0
                   ? static_cast<int>(parm.parm.capture.timeperframe.denominator / parm.parm.capture.timeperframe.numerator)
                   : config.rgb_profile.fps;
        if(fps_ <= 0) {
            fps_ = config.rgb_profile.fps;
        }
        if(config.rgb_profile.fps > 0 && fps_ != config.rgb_profile.fps) {
            throw std::runtime_error("v4l2 device returned unexpected fps " + std::to_string(fps_) + " for " + device_path_);
        }
        apply_color_controls(config.color_controls);

        v4l2_requestbuffers req{};
        req.count = 4;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        if(checked_v4l2_ioctl(fd_, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
            throw std::runtime_error("VIDIOC_REQBUFS failed for " + device_path_ + ": " + std::strerror(errno));
        }
        buffers_.resize(req.count);
        for(uint32_t i = 0; i < req.count; ++i) {
            v4l2_buffer buf{};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;
            if(checked_v4l2_ioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) {
                throw std::runtime_error("VIDIOC_QUERYBUF failed for " + device_path_ + ": " + std::strerror(errno));
            }
            buffers_[i].length = buf.length;
            buffers_[i].start = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, buf.m.offset);
            if(buffers_[i].start == MAP_FAILED) {
                buffers_[i].start = nullptr;
                throw std::runtime_error("mmap failed for " + device_path_ + ": " + std::strerror(errno));
            }
            if(checked_v4l2_ioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
                throw std::runtime_error("VIDIOC_QBUF failed for " + device_path_ + ": " + std::strerror(errno));
            }
        }
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if(checked_v4l2_ioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
            throw std::runtime_error("VIDIOC_STREAMON failed for " + device_path_ + ": " + std::strerror(errno));
        }
        streaming_ = true;
    }

    bool wait_frame(Frame &frame, std::chrono::milliseconds timeout) {
        pollfd pfd{};
        pfd.fd = fd_;
        pfd.events = POLLIN;
        const int poll_result = poll(&pfd, 1, static_cast<int>(timeout.count()));
        if(poll_result == 0) {
            return false;
        }
        if(poll_result < 0) {
            if(errno == EINTR) {
                return false;
            }
            throw std::runtime_error("poll failed for " + device_path_ + ": " + std::strerror(errno));
        }
        if((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            throw std::runtime_error("v4l2 poll error on " + device_path_);
        }

        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if(checked_v4l2_ioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
            if(errno == EAGAIN) {
                return false;
            }
            throw std::runtime_error("VIDIOC_DQBUF failed for " + device_path_ + ": " + std::strerror(errno));
        }
        if(buf.index >= buffers_.size()) {
            throw std::runtime_error("v4l2 returned invalid buffer index for " + device_path_);
        }
        frame.data = static_cast<const uint8_t *>(buffers_[buf.index].start);
        frame.size = buf.bytesused;
        frame.frame_id = next_frame_id_++;
        frame.system_timestamp_us = v4l2_buffer_system_timestamp_us(buf, now_us());
        frame.width = width_;
        frame.height = height_;
        frame.buffer_index = static_cast<int>(buf.index);
        return true;
    }

    void release_frame(Frame &frame) {
        if(frame.buffer_index < 0 || fd_ < 0) {
            return;
        }
        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = static_cast<uint32_t>(frame.buffer_index);
        if(checked_v4l2_ioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
            frame.buffer_index = -1;
            frame.data = nullptr;
            frame.size = 0;
            throw std::runtime_error("VIDIOC_QBUF failed for " + device_path_ + ": " + std::strerror(errno));
        }
        frame.buffer_index = -1;
        frame.data = nullptr;
        frame.size = 0;
    }

    void close_device() {
        if(fd_ >= 0 && streaming_) {
            v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            checked_v4l2_ioctl(fd_, VIDIOC_STREAMOFF, &type);
            streaming_ = false;
        }
        for(auto &buffer : buffers_) {
            if(buffer.start && buffer.length > 0) {
                munmap(buffer.start, buffer.length);
                buffer.start = nullptr;
                buffer.length = 0;
            }
        }
        buffers_.clear();
        if(fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
    }

    const std::string &device_path() const { return device_path_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    int fps() const { return fps_; }

private:
    struct Buffer {
        void *start = nullptr;
        size_t length = 0;
    };

    void set_control(uint32_t id, int value) {
        v4l2_control control{};
        control.id = id;
        control.value = value;
        if(checked_v4l2_ioctl(fd_, VIDIOC_S_CTRL, &control) < 0) {
            throw std::runtime_error("VIDIOC_S_CTRL id=" + std::to_string(id) + " value=" + std::to_string(value)
                                     + " failed for " + device_path_ + ": " + std::strerror(errno));
        }
    }

    void set_control_if_configured(uint32_t id, const std::optional<int> &value) {
        if(value) {
            set_control(id, *value);
        }
    }

    void set_bool_control_if_configured(uint32_t id, const std::optional<bool> &value) {
        if(value) {
            set_control(id, *value ? 1 : 0);
        }
    }

    void apply_color_controls(const ColorControlsConfig &controls) {
        if(controls.auto_exposure && !*controls.auto_exposure) {
            set_control(V4L2_CID_EXPOSURE_AUTO, V4L2_EXPOSURE_MANUAL);
        }
        set_control_if_configured(V4L2_CID_EXPOSURE_AUTO_PRIORITY, controls.auto_exposure_priority);
        set_control_if_configured(V4L2_CID_POWER_LINE_FREQUENCY, controls.power_line_frequency);
        set_control_if_configured(V4L2_CID_EXPOSURE_ABSOLUTE, controls.exposure);
        set_control_if_configured(V4L2_CID_GAIN, controls.gain);
        if(controls.auto_white_balance && !*controls.auto_white_balance) {
            set_bool_control_if_configured(V4L2_CID_AUTO_WHITE_BALANCE, controls.auto_white_balance);
        }
        set_control_if_configured(V4L2_CID_WHITE_BALANCE_TEMPERATURE, controls.white_balance);
        set_control_if_configured(V4L2_CID_BRIGHTNESS, controls.brightness);
        set_control_if_configured(V4L2_CID_CONTRAST, controls.contrast);
        set_control_if_configured(V4L2_CID_SATURATION, controls.saturation);
        set_control_if_configured(V4L2_CID_GAMMA, controls.gamma);
        set_control_if_configured(V4L2_CID_BACKLIGHT_COMPENSATION, controls.backlight_compensation);
        if(controls.auto_white_balance && *controls.auto_white_balance) {
            set_bool_control_if_configured(V4L2_CID_AUTO_WHITE_BALANCE, controls.auto_white_balance);
        }
        if(controls.auto_exposure && *controls.auto_exposure) {
            set_control(V4L2_CID_EXPOSURE_AUTO, V4L2_EXPOSURE_APERTURE_PRIORITY);
        }
    }

    int fd_ = -1;
    bool streaming_ = false;
    std::string device_path_;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    int fps_ = 0;
    uint64_t next_frame_id_ = 0;
    std::vector<Buffer> buffers_;
};

std::string existing_usb_device_name(std::string uid) {
    const std::filesystem::path root = "/sys/bus/usb/devices";
    if(uid.empty()) {
        return "";
    }
    if(std::filesystem::exists(root / uid)) {
        return uid;
    }
    const auto last_dash = uid.rfind('-');
    if(last_dash != std::string::npos) {
        const auto shortened = uid.substr(0, last_dash);
        if(std::filesystem::exists(root / shortened)) {
            return shortened;
        }
    }
    return "";
}

bool usb_uid_matches(const std::string &configured_uid, const std::string &device_uid) {
    if(configured_uid.empty() || device_uid.empty()) {
        return false;
    }
    if(configured_uid == device_uid) {
        return true;
    }

    const auto configured_device = existing_usb_device_name(configured_uid);
    const auto actual_device = existing_usb_device_name(device_uid);
    return !configured_device.empty() && configured_device == actual_device;
}

bool split_usb_device_name(const std::string &device_name, int &bus, std::string &port_path) {
    const auto dash = device_name.find('-');
    if(dash == std::string::npos || dash == 0 || dash + 1 >= device_name.size()) {
        return false;
    }
    try {
        bus = std::stoi(device_name.substr(0, dash));
    }
    catch(const std::exception &) {
        return false;
    }
    port_path = device_name.substr(dash + 1);
    return true;
}

std::string previous_usb_port_path(std::string port_path) {
    const auto dot = port_path.rfind('.');
    const size_t last_port_begin = dot == std::string::npos ? 0 : dot + 1;
    try {
        const int last_port = std::stoi(port_path.substr(last_port_begin));
        if(last_port <= 1) {
            return "";
        }
        port_path.replace(last_port_begin, std::string::npos, std::to_string(last_port - 1));
    }
    catch(const std::exception &) {
        return "";
    }

    return port_path;
}

std::vector<std::string> paired_color_device_candidates(const std::string &depth_device_name) {
    int bus = 0;
    std::string port_path;
    if(!split_usb_device_name(depth_device_name, bus, port_path)) {
        return {};
    }

    const auto color_port_path = previous_usb_port_path(port_path);
    if(color_port_path.empty()) {
        return {};
    }

    std::vector<std::string> candidates;
    candidates.push_back(std::to_string(bus) + "-" + color_port_path);
    if(bus > 1) {
        candidates.push_back(std::to_string(bus - 1) + "-" + color_port_path);
    }
    return candidates;
}

std::string paired_rgb_serial_for_depth_uid(const std::string &uid) {
    const std::filesystem::path root = "/sys/bus/usb/devices";
    const auto depth_name = existing_usb_device_name(uid);
    if(depth_name.empty()) {
        return "";
    }
    for(const auto &color_name : paired_color_device_candidates(depth_name)) {
        const auto color_path = root / color_name;
        if(read_text_file(color_path / "idVendor") == "2bc5" && read_text_file(color_path / "idProduct") == "0511") {
            return read_text_file(color_path / "serial");
        }
    }
    return "";
}

struct OrbbecDeviceIdentity {
    uint32_t index = 0;
    std::string model;
    std::string serial;
    std::string uid;
    std::string paired_rgb_serial;
    std::string connection_type;
};

std::string device_list_model_or_empty(ob::DeviceList &devices, uint32_t index) {
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    const char *model = devices.name(index);
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
    return model ? model : "";
}

std::vector<OrbbecDeviceIdentity> enumerate_orbbec_devices(ob::Context &ctx) {
    std::vector<OrbbecDeviceIdentity> identities;
    auto devices = ctx.queryDeviceList();
    identities.reserve(devices->deviceCount());
    for(uint32_t i = 0; i < devices->deviceCount(); ++i) {
        OrbbecDeviceIdentity identity;
        identity.index = i;
        identity.model = device_list_model_or_empty(*devices, i);
        identity.serial = devices->serialNumber(i) ? devices->serialNumber(i) : "";
        identity.uid = devices->uid(i) ? devices->uid(i) : "";
        identity.paired_rgb_serial = paired_rgb_serial_for_depth_uid(identity.uid);
        identity.connection_type = devices->connectionType(i) ? devices->connectionType(i) : "";
        identities.push_back(std::move(identity));
    }
    return identities;
}

bool device_identity_matches_config(const OrbbecDeviceIdentity &identity, const CameraConfig &camera) {
    if(!camera.device_model.empty() && identity.model != camera.device_model) {
        return false;
    }
    if(!camera.uid.empty() && usb_uid_matches(camera.uid, identity.uid)) {
        return true;
    }
    if(!camera.serial_number.empty()
       && (identity.serial == camera.serial_number || identity.paired_rgb_serial == camera.serial_number)) {
        return true;
    }
    if(camera.uid.empty() && camera.serial_number.empty() && !camera.device_model.empty()) {
        return true;
    }
    if(camera.uid.empty() && camera.serial_number.empty() && camera.device_index >= 0
       && identity.index == static_cast<uint32_t>(camera.device_index)) {
        return true;
    }
    return false;
}

std::string device_identity_summary(const OrbbecDeviceIdentity &identity) {
    std::ostringstream oss;
    oss << "index=" << identity.index << " model=" << identity.model << " serial=" << identity.serial << " uid=" << identity.uid
        << " paired_rgb_serial=" << identity.paired_rgb_serial << " connection=" << identity.connection_type;
    return oss.str();
}

std::string device_info_serial_or_empty(const std::shared_ptr<ob::Device> &device) {
    if(!device) {
        return "";
    }
    try {
        auto info = device->getDeviceInfo();
        return info && info->serialNumber() ? info->serialNumber() : "";
    }
    catch(const std::exception &) {
    }
    catch(...) {
    }
    return "";
}

bool serial_matches_device(const std::string &configured_serial, const std::string &list_serial, const std::string &info_serial,
                           const std::string &paired_rgb_serial) {
    return !configured_serial.empty()
           && (configured_serial == list_serial || configured_serial == info_serial || configured_serial == paired_rgb_serial);
}

std::string safe_device_list_string(const std::shared_ptr<ob::DeviceList> &devices) {
    if(!devices) {
        return "none";
    }
    std::ostringstream oss;
    const uint32_t count = devices->deviceCount();
    if(count == 0) {
        return "none";
    }
    for(uint32_t i = 0; i < count; ++i) {
        if(i > 0) {
            oss << "; ";
        }
        const std::string model = device_list_model_or_empty(*devices, i);
        const std::string uid = devices->uid(i) ? devices->uid(i) : "";
        const std::string paired_rgb_serial = paired_rgb_serial_for_depth_uid(uid);
        std::string info_serial;
        try {
            info_serial = device_info_serial_or_empty(devices->getDevice(i));
        }
        catch(const std::exception &) {
        }
        catch(...) {
        }
        oss << "index=" << i << " model=" << model
            << " serial=" << (devices->serialNumber(i) ? devices->serialNumber(i) : "")
            << " info_serial=" << info_serial << " uid=" << uid << " paired_rgb_serial=" << paired_rgb_serial
            << " connection=" << (devices->connectionType(i) ? devices->connectionType(i) : "");
    }
    return oss.str();
}

std::shared_ptr<ob::Device> select_device(ob::Context &ctx, const CameraConfig &camera) {
    auto devices = ctx.queryDeviceList();
    if(devices->deviceCount() == 0) {
        throw std::runtime_error("no Orbbec device found");
    }
    const auto model_at = [&devices](uint32_t index) {
        return device_list_model_or_empty(*devices, index);
    };
    const auto require_model_match = [&](uint32_t index, const std::string &matched_by) {
        const std::string actual_model = model_at(index);
        if(!camera.device_model.empty() && actual_model != camera.device_model) {
            std::ostringstream oss;
            oss << "configured camera " << matched_by << " but device_model mismatched camera_id=" << camera.camera_id
                << " configured_model=" << camera.device_model << " actual_model=" << actual_model
                << " available=[" << safe_device_list_string(devices) << "]";
            throw std::runtime_error(oss.str());
        }
    };
    if(!camera.uid.empty()) {
        for(uint32_t i = 0; i < devices->deviceCount(); ++i) {
            const std::string serial = devices->serialNumber(i) ? devices->serialNumber(i) : "";
            const std::string uid = devices->uid(i) ? devices->uid(i) : "";
            const std::string paired_rgb_serial = paired_rgb_serial_for_depth_uid(uid);
            if(!usb_uid_matches(camera.uid, uid)) {
                continue;
            }
            require_model_match(i, "uid matched");
            auto device = devices->getDevice(i);
            const auto info_serial = device_info_serial_or_empty(device);
            if(camera.serial_number.empty() || serial_matches_device(camera.serial_number, serial, info_serial, paired_rgb_serial)) {
                return device;
            }
            std::ostringstream oss;
            oss << "configured camera uid matched but serial_number mismatched camera_id=" << camera.camera_id
                << " uid=" << camera.uid << " matched_uid=" << uid
                << " configured_serial=" << camera.serial_number
                << " device_serial=" << serial
                << " device_info_serial=" << info_serial
                << " paired_rgb_serial=" << paired_rgb_serial;
            throw std::runtime_error(oss.str());
        }

        std::ostringstream oss;
        oss << "configured camera not found camera_id=" << camera.camera_id << " uid=" << camera.uid;
        if(!camera.serial_number.empty()) {
            oss << " serial=" << camera.serial_number;
        }
        oss << " available=[" << safe_device_list_string(devices) << "]";
        throw std::runtime_error(oss.str());
    }

    if(!camera.serial_number.empty()) {
        for(uint32_t i = 0; i < devices->deviceCount(); ++i) {
            const std::string serial = devices->serialNumber(i) ? devices->serialNumber(i) : "";
            auto device = devices->getDevice(i);
            const auto info_serial = device_info_serial_or_empty(device);
            if(serial == camera.serial_number || info_serial == camera.serial_number) {
                require_model_match(i, "serial_number matched");
                return device;
            }
        }

        std::shared_ptr<ob::Device> paired_match;
        std::string paired_match_uid;
        for(uint32_t i = 0; i < devices->deviceCount(); ++i) {
            const std::string uid = devices->uid(i) ? devices->uid(i) : "";
            const std::string paired_rgb_serial = paired_rgb_serial_for_depth_uid(uid);
            if(paired_rgb_serial != camera.serial_number) {
                continue;
            }
            require_model_match(i, "paired RGB serial_number matched");
            if(paired_match) {
                std::ostringstream oss;
                oss << "configured serial_number is ambiguous camera_id=" << camera.camera_id
                    << " serial=" << camera.serial_number
                    << " first_uid=" << paired_match_uid
                    << " second_uid=" << uid
                    << " available=[" << safe_device_list_string(devices) << "]";
                throw std::runtime_error(oss.str());
            }
            paired_match = devices->getDevice(i);
            paired_match_uid = uid;
        }
        if(paired_match) {
            return paired_match;
        }

        std::ostringstream oss;
        oss << "configured camera not found camera_id=" << camera.camera_id << " serial=" << camera.serial_number;
        oss << " available=[" << safe_device_list_string(devices) << "]";
        throw std::runtime_error(oss.str());
    }

    if(!camera.device_model.empty()) {
        std::vector<uint32_t> model_matches;
        for(uint32_t i = 0; i < devices->deviceCount(); ++i) {
            if(model_at(i) == camera.device_model) {
                model_matches.push_back(i);
            }
        }
        if(model_matches.empty()) {
            std::ostringstream oss;
            oss << "configured camera model not found camera_id=" << camera.camera_id
                << " device_model=" << camera.device_model
                << " available=[" << safe_device_list_string(devices) << "]";
            throw std::runtime_error(oss.str());
        }
        if(model_matches.size() == 1) {
            return devices->getDevice(model_matches.front());
        }
        if(camera.device_index >= 0) {
            const auto configured_index = static_cast<uint32_t>(camera.device_index);
            if(std::find(model_matches.begin(), model_matches.end(), configured_index) != model_matches.end()) {
                return devices->getDevice(configured_index);
            }
        }
        std::ostringstream oss;
        oss << "configured camera model is ambiguous camera_id=" << camera.camera_id
            << " device_model=" << camera.device_model << " device_index=" << camera.device_index
            << " available=[" << safe_device_list_string(devices) << "]";
        throw std::runtime_error(oss.str());
    }

    if(camera.device_index < 0 || static_cast<uint32_t>(camera.device_index) >= devices->deviceCount()) {
        std::ostringstream oss;
        oss << "camera device_index out of range camera_id=" << camera.camera_id << " device_index=" << camera.device_index
            << " available_count=" << devices->deviceCount() << " available=[" << safe_device_list_string(devices) << "]";
        throw std::runtime_error(oss.str());
    }
    return devices->getDevice(static_cast<uint32_t>(camera.device_index));
}

cv::Mat mjpg_to_bgr(const std::shared_ptr<ob::ColorFrame> &frame, int flags) {
    cv::Mat raw(1, static_cast<int>(frame->dataSize()), CV_8UC1, frame->data());
    return cv::imdecode(raw, flags);
}

cv::Mat mjpg_buffer_to_bgr(const uint8_t *data, size_t size, int flags) {
    cv::Mat raw(1, static_cast<int>(size), CV_8UC1, const_cast<uint8_t *>(data));
    return cv::imdecode(raw, flags);
}

size_t mjpg_complete_jpeg_size(const void *payload, size_t payload_size) {
    if(!payload || payload_size < 4) {
        return 0;
    }
    const auto *data = static_cast<const uint8_t *>(payload);
    const size_t size = payload_size;
    if(data[0] != 0xff || data[1] != 0xd8) {
        return 0;
    }

    size_t end = size;
    while(end > 0 && data[end - 1] == 0x00) {
        --end;
    }
    return end >= 4 && data[end - 2] == 0xff && data[end - 1] == 0xd9 ? end : 0;
}

bool mjpg_has_complete_jpeg_data(const void *payload, size_t payload_size) {
    return mjpg_complete_jpeg_size(payload, payload_size) != 0;
}

bool mjpg_has_complete_jpeg(const std::shared_ptr<ob::ColorFrame> &frame) {
    return frame && mjpg_has_complete_jpeg_data(frame->data(), frame->dataSize());
}

struct JpegValidationError {
    jpeg_error_mgr pub;
    jmp_buf jump_buffer;
    bool warning = false;
    JSAMPLE *row_buffer = nullptr;
    char message[JMSG_LENGTH_MAX] = {};
};

void jpeg_validation_error_exit(j_common_ptr cinfo) {
    auto *error = reinterpret_cast<JpegValidationError *>(cinfo->err);
    (*cinfo->err->format_message)(cinfo, error->message);
    longjmp(error->jump_buffer, 1);
}

void jpeg_validation_emit_message(j_common_ptr cinfo, int msg_level) {
    if(msg_level >= 0) {
        return;
    }
    auto *error = reinterpret_cast<JpegValidationError *>(cinfo->err);
    error->warning = true;
    (*cinfo->err->format_message)(cinfo, error->message);
}

bool mjpg_decodes_cleanly_data(const void *payload, size_t payload_size, std::string &message) {
    if(!mjpg_has_complete_jpeg_data(payload, payload_size)) {
        message = "missing jpeg soi/eoi marker";
        return false;
    }

    jpeg_decompress_struct cinfo{};
    JpegValidationError error{};
    cinfo.err = jpeg_std_error(&error.pub);
    error.pub.error_exit = jpeg_validation_error_exit;
    error.pub.emit_message = jpeg_validation_emit_message;

    if(setjmp(error.jump_buffer) != 0) {
        if(error.row_buffer) {
            std::free(error.row_buffer);
            error.row_buffer = nullptr;
        }
        jpeg_destroy_decompress(&cinfo);
        message = error.message[0] ? error.message : "jpeg validation failed";
        return false;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, static_cast<const unsigned char *>(payload), static_cast<unsigned long>(payload_size));
    if(jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        message = "jpeg header validation failed";
        return false;
    }
    cinfo.scale_num = 1;
    cinfo.scale_denom = 8;
    jpeg_start_decompress(&cinfo);

    const size_t row_size = static_cast<size_t>(cinfo.output_width) * static_cast<size_t>(cinfo.output_components);
    error.row_buffer = static_cast<JSAMPLE *>(std::malloc(row_size));
    if(!error.row_buffer) {
        jpeg_destroy_decompress(&cinfo);
        message = "jpeg validation failed: row allocation failed";
        return false;
    }
    while(cinfo.output_scanline < cinfo.output_height) {
        JSAMPROW row_ptr = error.row_buffer;
        if(jpeg_read_scanlines(&cinfo, &row_ptr, 1) != 1) {
            break;
        }
    }
    std::free(error.row_buffer);
    error.row_buffer = nullptr;
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);

    if(error.warning) {
        message = error.message[0] ? error.message : "jpeg decoder warning";
        return false;
    }
    return true;
}

bool mjpg_decodes_cleanly(const std::shared_ptr<ob::ColorFrame> &frame, std::string &message) {
    if(!frame) {
        message = "missing color frame";
        return false;
    }
    return mjpg_decodes_cleanly_data(frame->data(), frame->dataSize(), message);
}

void mark_corrupt_rgb_jpeg_frame(CameraRuntime &camera, Logger &logger, const std::shared_ptr<ob::ColorFrame> &color,
                                 std::chrono::steady_clock::time_point frame_now, const std::string &reason) {
    bool should_log = false;
    {
        std::lock_guard<std::mutex> lock(camera.mutex);
        camera.perf.rgb_corrupt_jpeg_frames++;
        camera.rgb_corrupt_jpeg++;
        camera.rgb_dropped++;
        camera.last_error = "corrupt rgb mjpeg frame dropped";
        if(frame_now >= camera.next_jpeg_warning) {
            camera.next_jpeg_warning = frame_now + std::chrono::seconds(1);
            should_log = true;
        }
    }
    if(should_log) {
        std::ostringstream oss;
        oss << "corrupt rgb mjpeg frame dropped camera_id=" << camera.config.camera_id << " frame_id=" << color->index()
            << " size=" << color->dataSize() << " reason=\"" << reason << "\"";
        logger.warn(oss.str());
    }
}

void mark_corrupt_rgb_jpeg_frame(CameraRuntime &camera, Logger &logger, uint64_t frame_id, size_t frame_size,
                                 std::chrono::steady_clock::time_point frame_now, const std::string &reason) {
    bool should_log = false;
    {
        std::lock_guard<std::mutex> lock(camera.mutex);
        camera.perf.rgb_corrupt_jpeg_frames++;
        camera.rgb_corrupt_jpeg++;
        camera.rgb_dropped++;
        camera.last_error = "corrupt rgb mjpeg frame dropped";
        if(frame_now >= camera.next_jpeg_warning) {
            camera.next_jpeg_warning = frame_now + std::chrono::seconds(1);
            should_log = true;
        }
    }
    if(should_log) {
        std::ostringstream oss;
        oss << "corrupt rgb mjpeg frame dropped camera_id=" << camera.config.camera_id << " frame_id=" << frame_id
            << " size=" << frame_size << " reason=\"" << reason << "\"";
        logger.warn(oss.str());
    }
}

void record_queue_overwrite(CameraRuntime &camera, StreamType stream_type) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    if(stream_type == StreamType::rgb) {
        camera.rgb_dropped++;
    }
    else if(stream_type == StreamType::depth_raw) {
        camera.depth_dropped++;
    }
}

void record_media_send_success(CameraRuntime &camera, StreamType stream_type, size_t packet_size, double send_ms) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    if(stream_type == StreamType::rgb) {
        camera.perf.rgb_send_ms += send_ms;
        camera.perf.rgb_sent_packets++;
        camera.perf.rgb_bytes += packet_size;
    }
    else if(stream_type == StreamType::rgb_preview) {
        camera.perf.rgb_preview_send_ms += send_ms;
        camera.perf.rgb_preview_sent_packets++;
        camera.perf.rgb_preview_bytes += packet_size;
    }
    else if(stream_type == StreamType::depth_raw) {
        camera.perf.depth_send_ms += send_ms;
        camera.perf.depth_sent_frames++;
        camera.perf.depth_bytes += packet_size;
    }
}

void arm_rgb_keyframe_guard(CameraRuntime &camera, Logger &logger, const std::string &reason) {
    bool should_log = false;
    {
        std::lock_guard<std::mutex> lock(camera.mutex);
        should_log = camera.rgb_transport_recovery.arm();
        camera.last_error = reason.empty() ? "rgb transport loss; waiting for next keyframe" : reason;
    }
    if(should_log) {
        const std::string log_reason = reason.empty() ? "rgb transport loss" : reason;
        logger.warn(log_reason + " camera_id=" + camera.config.camera_id + "; dropping non-IDR frames until next IDR");
    }
}

void request_rgb_keyframe(CameraRuntime &camera, Logger &logger, const std::string &reason) {
    uint64_t request_id = 0;
    {
        std::lock_guard<std::mutex> lock(camera.mutex);
        request_id = ++camera.force_rgb_keyframe_requests;
        camera.force_rgb_keyframe_requested_at_us = now_us();
    }
    logger.info("rgb keyframe requested camera_id=" + camera.config.camera_id + " request_id=" + std::to_string(request_id)
                + (reason.empty() ? "" : " reason=" + reason));
}

bool consume_rgb_keyframe_request(CameraRuntime &camera, uint64_t &request_id) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    if(camera.force_rgb_keyframe_applied >= camera.force_rgb_keyframe_requests) {
        return false;
    }
    camera.force_rgb_keyframe_applied = camera.force_rgb_keyframe_requests;
    request_id = camera.force_rgb_keyframe_applied;
    return true;
}

void report_forced_rgb_keyframe(CameraRuntime &camera, Logger &logger) {
    uint64_t request_id = 0;
    uint64_t requested_at_us = 0;
    {
        std::lock_guard<std::mutex> lock(camera.mutex);
        if(camera.force_rgb_keyframe_observed >= camera.force_rgb_keyframe_requests) {
            return;
        }
        camera.force_rgb_keyframe_observed = camera.force_rgb_keyframe_requests;
        request_id = camera.force_rgb_keyframe_observed;
        requested_at_us = camera.force_rgb_keyframe_requested_at_us;
    }
    const uint64_t observed_at_us = now_us();
    const uint64_t latency_us = requested_at_us != 0 && observed_at_us >= requested_at_us ? observed_at_us - requested_at_us : 0;
    logger.info("rgb forced keyframe observed camera_id=" + camera.config.camera_id + " request_id=" + std::to_string(request_id)
                + " latency_us=" + std::to_string(latency_us));
}

void maybe_request_rgb_recovery_keyframe(CameraRuntime &camera, Logger &logger,
                                         std::chrono::steady_clock::time_point now) {
    const auto since_epoch = now.time_since_epoch();
    const auto monotonic_now_us =
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(since_epoch).count());
    bool request_due = false;
    {
        std::lock_guard<std::mutex> lock(camera.mutex);
        request_due = camera.rgb_transport_recovery.keyframe_request_due(monotonic_now_us,
                                                                          kRgbRecoveryKeyframeRequestIntervalUs);
    }
    if(request_due) {
        request_rgb_keyframe(camera, logger, "media_transport_recovery");
    }
}

RgbTransportRecovery::SendDecision decide_rgb_keyframe_send(CameraRuntime &camera, bool is_keyframe,
                                                            std::chrono::steady_clock::time_point now, Logger &logger) {
    bool log_drop = false;
    bool request_keyframe = false;
    uint64_t dropped = 0;
    RgbTransportRecovery::SendDecision decision = RgbTransportRecovery::SendDecision::send;
    {
        std::lock_guard<std::mutex> lock(camera.mutex);
        decision = camera.rgb_transport_recovery.before_send(is_keyframe);
        if(decision == RgbTransportRecovery::SendDecision::drop) {
            camera.rgb_dropped++;
            dropped = camera.rgb_transport_recovery.dropped_frames();
            if(now >= camera.next_keyframe_guard_warning) {
                camera.next_keyframe_guard_warning = now + std::chrono::seconds(1);
                log_drop = true;
            }
        }
        if(camera.rgb_transport_recovery.waiting()) {
            const auto monotonic_now_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                                     now.time_since_epoch())
                                                                     .count());
            request_keyframe = camera.rgb_transport_recovery.keyframe_request_due(
                monotonic_now_us, kRgbRecoveryKeyframeRequestIntervalUs);
        }
    }
    if(request_keyframe) {
        request_rgb_keyframe(camera, logger, "media_transport_recovery");
    }
    if(log_drop) {
        logger.warn("rgb keyframe guard dropping non-IDR camera_id=" + camera.config.camera_id + " dropped=" + std::to_string(dropped));
    }
    return decision;
}

void complete_rgb_keyframe_recovery(CameraRuntime &camera, Logger &logger, bool is_keyframe) {
    std::optional<uint64_t> dropped;
    {
        std::lock_guard<std::mutex> lock(camera.mutex);
        dropped = camera.rgb_transport_recovery.complete_successful_send(is_keyframe);
    }
    if(dropped) {
        logger.info("rgb keyframe guard recovered camera_id=" + camera.config.camera_id
                    + " dropped=" + std::to_string(*dropped));
    }
}

void record_media_send_failure(CameraRuntime &camera, Logger &logger, StreamType stream_type, double send_ms,
                               std::chrono::steady_clock::time_point frame_now, const std::string &reason) {
    const std::string message = reason.empty() ? "unknown media transport error" : reason;
    bool should_log = false;
    {
        std::lock_guard<std::mutex> lock(camera.mutex);
        if(stream_type == StreamType::rgb) {
            camera.perf.rgb_send_ms += send_ms;
            camera.perf.rgb_send_failures++;
            camera.rgb_send_failures_total++;
        }
        else if(stream_type == StreamType::rgb_preview) {
            camera.perf.rgb_preview_send_ms += send_ms;
            camera.perf.rgb_preview_send_failures++;
            camera.rgb_preview_send_failures_total++;
        }
        else if(stream_type == StreamType::depth_raw) {
            camera.perf.depth_send_ms += send_ms;
            camera.perf.depth_send_failures++;
            camera.depth_send_failures_total++;
        }
        camera.last_error = message;
        if(frame_now >= camera.next_media_warning || message != camera.last_media_warning) {
            camera.next_media_warning = frame_now + std::chrono::seconds(2);
            camera.last_media_warning = message;
            should_log = true;
        }
    }
    if(should_log) {
        logger.warn("media send failed camera_id=" + camera.config.camera_id
                    + " stream=" + stream_type_name(stream_type) + " error=" + message);
    }
}

void record_rgb_input(CameraRuntime &camera, const std::shared_ptr<ob::ColorFrame> &color) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    camera.perf.rgb_input_frames++;
    camera.perf.rgb_input_bytes += color->dataSize();
    camera.perf.note_rgb_frame_id(color->index());
    camera.last_rgb_frame_at = std::chrono::steady_clock::now();
}

void record_rgb_input(CameraRuntime &camera, size_t payload_size, uint64_t frame_id) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    camera.perf.rgb_input_frames++;
    camera.perf.rgb_input_bytes += payload_size;
    camera.perf.note_rgb_frame_id(frame_id);
    camera.last_rgb_frame_at = std::chrono::steady_clock::now();
}

void record_depth_input(CameraRuntime &camera, const std::shared_ptr<ob::DepthFrame> &depth) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    camera.perf.depth_input_frames++;
    camera.perf.depth_input_bytes += depth->dataSize();
    camera.perf.note_depth_frame_id(depth->index());
    camera.last_depth_frame_at = std::chrono::steady_clock::now();
}

void record_wait_result(CameraRuntime &camera, double wait_ms, bool timeout) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    camera.perf.wait_calls++;
    camera.perf.wait_ms += wait_ms;
    if(timeout) {
        camera.perf.wait_timeouts++;
    }
}

void record_rgb_decode_ms(CameraRuntime &camera, double decode_ms) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    camera.perf.rgb_decode_ms += decode_ms;
}

void record_rgb_encode_ms(CameraRuntime &camera, double encode_ms) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    camera.perf.rgb_encode_ms += encode_ms;
}

void record_depth_compress_ms(CameraRuntime &camera, double compress_ms) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    camera.perf.depth_compress_ms += compress_ms;
}

void record_depth_preview_ms(CameraRuntime &camera, double preview_ms) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    camera.perf.depth_preview_ms += preview_ms;
}

void record_preview_ms(CameraRuntime &camera, double preview_ms) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    camera.perf.preview_ms += preview_ms;
}

void record_rgb_frame_done(CameraRuntime &camera) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    camera.rgb_frames++;
}

void record_depth_frame_done(CameraRuntime &camera) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    camera.depth_frames++;
}

std::chrono::steady_clock::duration frame_interval_for_fps(int fps) {
    if(fps <= 0) {
        return std::chrono::steady_clock::duration::zero();
    }
    return std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(1.0 / static_cast<double>(fps)));
}

bool depth_emit_due(CameraRuntime &camera, std::chrono::steady_clock::time_point now) {
    (void)camera;
    (void)now;
    // The camera profile already controls capture FPS. Software pacing here
    // dropped valid depth frames when scheduler jitter made a frame arrive a
    // little before the next nominal 30 FPS tick.
    return true;
}

bool web_rgb_preview_emit_due(const AppConfig &config, CameraRuntime &camera, std::chrono::steady_clock::time_point now) {
    if(!config.web_rgb_preview.enabled) {
        return false;
    }
    const auto interval = frame_interval_for_fps(config.web_rgb_preview.fps);
    if(interval <= std::chrono::steady_clock::duration::zero()) {
        return true;
    }

    std::lock_guard<std::mutex> lock(camera.mutex);
    if(now < camera.web_rgb_preview_suppressed_until) {
        return false;
    }
    if(config.web_rgb_preview.on_demand && now > camera.web_rgb_preview_requested_until) {
        return false;
    }
    if(now < camera.next_web_rgb_preview) {
        return false;
    }
    camera.next_web_rgb_preview += interval;
    if(camera.next_web_rgb_preview <= now) {
        camera.next_web_rgb_preview = now + interval;
    }
    return true;
}

void set_latest_bgr(CameraRuntime &camera, const cv::Mat &bgr, uint64_t frame_id, uint64_t system_timestamp_us,
                    int rotation_degrees) {
    if(bgr.empty()) {
        return;
    }
    cv::Mat preview_bgr;
    if(rotation_degrees == 180) {
        cv::rotate(bgr, preview_bgr, cv::ROTATE_180);
    }
    else {
        preview_bgr = bgr.clone();
    }
    std::lock_guard<std::mutex> lock(camera.mutex);
    camera.latest_bgr = preview_bgr;
    camera.latest_rgb_frame_id = frame_id;
    camera.latest_rgb_system_timestamp_us = system_timestamp_us;
    camera.rgb_preview_queue.push_back(RgbPreviewFrame{preview_bgr, frame_id, system_timestamp_us});
    while(camera.rgb_preview_queue.size() > kMaxRgbPreviewQueueFrames) {
        camera.rgb_preview_queue.pop_front();
    }
}

void set_latest_depth_color(CameraRuntime &camera, const cv::Mat &depth_color) {
    if(depth_color.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(camera.mutex);
    camera.latest_depth_color = depth_color.clone();
}

bool camera_is_online(const CameraRuntime &camera) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    return camera.online;
}

bool camera_reconnect_enabled(const CameraRuntime &camera) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    return camera.reconnect_enabled;
}

bool camera_counts_against_hotplug_limit(const CameraRuntime &camera) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    return camera.online || camera.reconnect_enabled;
}

bool camera_matches_identity(const CameraRuntime &camera, const OrbbecDeviceIdentity &identity) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    if(camera.hotplug_dynamic && !camera.online && !camera.reconnect_enabled) {
        return false;
    }
    if(!camera.device_uid.empty() && camera.device_uid == identity.uid) {
        return true;
    }
    if(!camera.device_serial.empty()
       && (camera.device_serial == identity.serial || camera.device_serial == identity.paired_rgb_serial)) {
        return true;
    }
    return device_identity_matches_config(identity, camera.config);
}

void set_camera_reconnect_delay(CameraRuntime &camera, std::chrono::steady_clock::time_point next_reconnect) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    camera.next_reconnect = next_reconnect;
}

std::shared_ptr<ob::VideoStreamProfile> camera_color_profile(const CameraRuntime &camera) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    return camera.color_profile;
}

bool camera_announced(const CameraRuntime &camera) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    return camera.announced;
}

bool camera_online(const CameraRuntime &camera) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    return camera.online;
}

void set_camera_announced(CameraRuntime &camera, bool announced) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    camera.announced = announced;
}

GstH264InputFormat encoder_input_format_for(CameraRuntime &camera) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    return camera.encoder_input_format;
}

GstH264InputFormat web_preview_encoder_input_format_for(CameraRuntime &camera) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    return camera.web_preview_encoder_input_format;
}

void set_camera_last_error(CameraRuntime &camera, const std::string &error) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    camera.last_error = error;
}

void set_depth_scale_if_empty(CameraRuntime &camera, float depth_scale) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    if(camera.depth_scale == 0.0f) {
        camera.depth_scale = depth_scale;
    }
}

cv::Mat color_to_bgr(const std::shared_ptr<ob::ColorFrame> &frame) {
    const auto format = frame->format();
    if(format == OB_FORMAT_MJPG) {
        return mjpg_to_bgr(frame, cv::IMREAD_COLOR);
    }
    if(format == OB_FORMAT_RGB) {
        cv::Mat rgb(frame->height(), frame->width(), CV_8UC3, frame->data());
        cv::Mat bgr;
        cv::cvtColor(rgb, bgr, cv::COLOR_RGB2BGR);
        return bgr;
    }
    if(format == OB_FORMAT_BGR) {
        return cv::Mat(frame->height(), frame->width(), CV_8UC3, frame->data()).clone();
    }
    if(format == OB_FORMAT_YUYV) {
        cv::Mat yuyv(frame->height(), frame->width(), CV_8UC2, frame->data());
        cv::Mat bgr;
        cv::cvtColor(yuyv, bgr, cv::COLOR_YUV2BGR_YUYV);
        return bgr;
    }
    if(format == OB_FORMAT_UYVY) {
        cv::Mat uyvy(frame->height(), frame->width(), CV_8UC2, frame->data());
        cv::Mat bgr;
        cv::cvtColor(uyvy, bgr, cv::COLOR_YUV2BGR_UYVY);
        return bgr;
    }
    return {};
}

cv::Mat color_to_preview_bgr(const std::shared_ptr<ob::ColorFrame> &frame) {
    if(frame->format() == OB_FORMAT_MJPG) {
        return mjpg_to_bgr(frame, cv::IMREAD_REDUCED_COLOR_2);
    }
    return color_to_bgr(frame);
}

std::optional<ExposureMeteringSample> meter_mjpg_luma(const std::shared_ptr<ob::ColorFrame> &frame,
                                                      const AdaptiveExposureConfig &config,
                                                      std::string &error) {
    if(!frame || frame->format() != OB_FORMAT_MJPG || !frame->data() || frame->dataSize() == 0
       || frame->dataSize() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        error = "adaptive metering requires a valid MJPEG frame";
        return std::nullopt;
    }

    cv::Mat gray;
    try {
        cv::Mat encoded(1, static_cast<int>(frame->dataSize()), CV_8UC1, frame->data());
        gray = cv::imdecode(encoded, cv::IMREAD_REDUCED_GRAYSCALE_8);
    }
    catch(const cv::Exception &e) {
        error = e.what();
        return std::nullopt;
    }
    if(gray.empty() || gray.type() != CV_8UC1) {
        error = "adaptive metering JPEG decode returned no grayscale pixels";
        return std::nullopt;
    }

    const int margin_x = gray.cols * config.roi_margin_percent / 100;
    const int margin_y = gray.rows * config.roi_margin_percent / 100;
    const int roi_x0 = std::clamp(margin_x, 0, gray.cols - 1);
    const int roi_y0 = std::clamp(margin_y, 0, gray.rows - 1);
    const int roi_x1 = std::clamp(gray.cols - margin_x, roi_x0 + 1, gray.cols);
    const int roi_y1 = std::clamp(gray.rows - margin_y, roi_y0 + 1, gray.rows);

    uint64_t histogram[256] = {};
    uint64_t pixel_count = 0;
    uint64_t highlight_count = 0;
    for(int y = roi_y0; y < roi_y1; ++y) {
        const auto *row = gray.ptr<uint8_t>(y);
        for(int x = roi_x0; x < roi_x1; ++x) {
            const uint8_t value = row[x];
            ++histogram[value];
            ++pixel_count;
            if(value >= config.highlight_luma) {
                ++highlight_count;
            }
        }
    }
    if(pixel_count == 0) {
        error = "adaptive metering ROI is empty";
        return std::nullopt;
    }

    auto percentile = [&](int numerator, int denominator) {
        const uint64_t threshold = (pixel_count * static_cast<uint64_t>(numerator) + denominator - 1)
                                   / static_cast<uint64_t>(denominator);
        uint64_t cumulative = 0;
        for(int value = 0; value < 256; ++value) {
            cumulative += histogram[value];
            if(cumulative >= threshold) {
                return value;
            }
        }
        return 255;
    };

    ExposureMeteringSample sample;
    sample.p50_luma = percentile(50, 100);
    sample.p95_luma = percentile(95, 100);
    sample.p99_luma = percentile(99, 100);
    sample.highlight_fraction = static_cast<double>(highlight_count) / static_cast<double>(pixel_count);
    return sample;
}

bool apply_adaptive_exposure_decision(CameraRuntime &camera, const ExposureControlDecision &decision,
                                      Logger &logger, std::string &error) {
    if(!decision.apply || !camera.adaptive_exposure_controller) {
        return true;
    }

    std::shared_ptr<ob::Device> device;
    {
        std::lock_guard<std::mutex> lock(camera.mutex);
        device = camera.device;
    }
    if(!device) {
        error = "camera device is unavailable";
        return false;
    }

    auto set_and_verify = [&](OBPropertyID property_id, int desired, const char *name) {
        if(!device->isPropertySupported(property_id, OB_PERMISSION_WRITE)) {
            error = std::string(name) + " is not writable";
            return false;
        }
        device->setIntProperty(property_id, desired);
        if(device->isPropertySupported(property_id, OB_PERMISSION_READ)) {
            const int readback = device->getIntProperty(property_id);
            if(readback != desired) {
                error = std::string(name) + " readback=" + std::to_string(readback)
                        + " expected=" + std::to_string(desired);
                return false;
            }
        }
        return true;
    };

    try {
        const int old_exposure = camera.adaptive_exposure_controller->exposure();
        const int old_gain = camera.adaptive_exposure_controller->gain();
        if(decision.exposure != old_exposure
           && !set_and_verify(OB_PROP_COLOR_EXPOSURE_INT, decision.exposure, "exposure")) {
            return false;
        }
        if(decision.gain != old_gain && !set_and_verify(OB_PROP_COLOR_GAIN_INT, decision.gain, "gain")) {
            if(decision.exposure != old_exposure) {
                try {
                    device->setIntProperty(OB_PROP_COLOR_EXPOSURE_INT, old_exposure);
                }
                catch(...) {
                }
            }
            return false;
        }
        camera.adaptive_exposure_controller->commit(decision);
        logger.info("adaptive exposure adjusted camera_id=" + camera.config.camera_id
                    + " reason=" + decision.reason
                    + " exposure=" + std::to_string(decision.exposure)
                    + " gain=" + std::to_string(decision.gain));
        return true;
    }
    catch(const std::exception &e) {
        error = e.what();
        return false;
    }
}

void maybe_update_adaptive_exposure(CameraRuntime &camera, const std::shared_ptr<ob::ColorFrame> &color,
                                    std::chrono::steady_clock::time_point now, Logger &logger) {
    if(!camera.adaptive_exposure_controller) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(camera.mutex);
        if(now < camera.next_adaptive_exposure_sample) {
            return;
        }
        camera.next_adaptive_exposure_sample =
            now + std::chrono::milliseconds(camera.config.adaptive_exposure.interval_ms);
    }

    std::string error;
    const auto sample = meter_mjpg_luma(color, camera.config.adaptive_exposure, error);
    if(!sample) {
        bool should_log = false;
        {
            std::lock_guard<std::mutex> lock(camera.mutex);
            ++camera.live.adaptive_failures;
            camera.live.adaptive_last_reason = "metering_failed";
            if(now >= camera.next_adaptive_exposure_warning) {
                camera.next_adaptive_exposure_warning = now + std::chrono::seconds(10);
                should_log = true;
            }
        }
        if(should_log) {
            logger.warn("adaptive exposure metering failed camera_id=" + camera.config.camera_id + " error=" + error);
        }
        return;
    }

    const auto decision = camera.adaptive_exposure_controller->evaluate(*sample);
    const bool applied = apply_adaptive_exposure_decision(camera, decision, logger, error);
    bool should_log_failure = false;
    {
        std::lock_guard<std::mutex> lock(camera.mutex);
        camera.live.adaptive_luma_p50 = sample->p50_luma;
        camera.live.adaptive_luma_p95 = sample->p95_luma;
        camera.live.adaptive_luma_p99 = sample->p99_luma;
        camera.live.adaptive_highlight_fraction = sample->highlight_fraction;
        camera.live.adaptive_requested_exposure = camera.adaptive_exposure_controller->exposure();
        camera.live.adaptive_requested_gain = camera.adaptive_exposure_controller->gain();
        camera.live.adaptive_samples = camera.adaptive_exposure_controller->sample_count();
        camera.live.adaptive_adjustments = camera.adaptive_exposure_controller->adjustment_count();
        camera.live.adaptive_last_reason = applied ? decision.reason : "apply_failed";
        if(!applied) {
            ++camera.live.adaptive_failures;
            if(now >= camera.next_adaptive_exposure_warning) {
                camera.next_adaptive_exposure_warning = now + std::chrono::seconds(10);
                should_log_failure = true;
            }
        }
        else if(decision.apply) {
            camera.next_adaptive_exposure_sample =
                now + std::chrono::milliseconds(camera.config.adaptive_exposure.settle_ms);
        }
    }
    if(should_log_failure) {
        logger.warn("adaptive exposure apply failed camera_id=" + camera.config.camera_id + " error=" + error);
    }
}

void apply_software_rgb_rotation(cv::Mat &bgr, const CameraConfig &config) {
    if(bgr.empty() || software_rgb_rotation_degrees(config) == 0) {
        return;
    }
    cv::Mat rotated;
    cv::rotate(bgr, rotated, cv::ROTATE_180);
    bgr = std::move(rotated);
}

cv::Mat depth_to_color(const std::shared_ptr<ob::DepthFrame> &frame) {
    cv::Mat depth(frame->height(), frame->width(), CV_16UC1, frame->data());
    cv::Mat depth8;
    depth8.create(depth.rows, depth.cols, CV_8UC1);
    const auto denom = static_cast<float>(kDepthPreviewMaxMm - kDepthPreviewMinMm);
    for(int y = 0; y < depth.rows; ++y) {
        const auto *src = depth.ptr<uint16_t>(y);
        auto *dst = depth8.ptr<uint8_t>(y);
        for(int x = 0; x < depth.cols; ++x) {
            const uint16_t value = src[x];
            if(value == 0 || denom <= 0.0f) {
                dst[x] = 0;
                continue;
            }
            const auto clamped = static_cast<float>(std::clamp<uint16_t>(value, kDepthPreviewMinMm, kDepthPreviewMaxMm));
            dst[x] = static_cast<uint8_t>((clamped - static_cast<float>(kDepthPreviewMinMm)) * 255.0f / denom + 0.5f);
        }
    }
    cv::Mat color;
    cv::applyColorMap(depth8, color, cv::COLORMAP_JET);
    color.setTo(cv::Scalar(24, 16, 12), depth == 0);
    return color;
}

bool h264_payload_has_idr(const std::vector<uint8_t> &payload) {
    for(size_t i = 0; i + 4 < payload.size(); ++i) {
        size_t nal_offset = 0;
        if(payload[i] == 0 && payload[i + 1] == 0 && payload[i + 2] == 1) {
            nal_offset = i + 3;
        }
        else if(i + 4 < payload.size() && payload[i] == 0 && payload[i + 1] == 0 && payload[i + 2] == 0 && payload[i + 3] == 1) {
            nal_offset = i + 4;
        }
        if(nal_offset > 0 && nal_offset < payload.size() && (payload[nal_offset] & 0x1fu) == 5u) {
            return true;
        }
    }
    return false;
}

bool h264_payload_has_vcl_nal(const std::vector<uint8_t> &payload) {
    for(size_t i = 0; i + 4 < payload.size(); ++i) {
        size_t nal_offset = 0;
        if(payload[i] == 0 && payload[i + 1] == 0 && payload[i + 2] == 1) {
            nal_offset = i + 3;
        }
        else if(i + 4 < payload.size() && payload[i] == 0 && payload[i + 1] == 0 && payload[i + 2] == 0 && payload[i + 3] == 1) {
            nal_offset = i + 4;
        }
        if(nal_offset > 0 && nal_offset < payload.size()) {
            const uint8_t nal_type = payload[nal_offset] & 0x1fu;
            if(nal_type >= 1 && nal_type <= 5) {
                return true;
            }
        }
    }
    return false;
}

std::vector<uint8_t> zlib_compress_payload(const void *data, size_t size) {
    const auto bound = compressBound(static_cast<uLong>(size));
    std::vector<uint8_t> out(bound);
    uLongf out_size = bound;
    const int rc = compress2(out.data(), &out_size, static_cast<const Bytef *>(data), static_cast<uLong>(size), Z_BEST_SPEED);
    if(rc != Z_OK) {
        throw std::runtime_error("zlib depth compression failed");
    }
    out.resize(static_cast<size_t>(out_size));
    return out;
}

struct Lz4CompressApi {
    using CompressBoundFn = int (*)(int);
    using CompressDefaultFn = int (*)(const char *, char *, int, int);

    void *handle = nullptr;
    CompressBoundFn compress_bound = nullptr;
    CompressDefaultFn compress_default = nullptr;
};

Lz4CompressApi &lz4_compress_api() {
    static Lz4CompressApi api;
    static std::once_flag once;
    std::call_once(once, [] {
        api.handle = dlopen("liblz4.so.1", RTLD_LAZY | RTLD_LOCAL);
        if(!api.handle) {
            throw std::runtime_error(std::string("cannot load liblz4.so.1: ") + dlerror());
        }
        api.compress_bound = reinterpret_cast<Lz4CompressApi::CompressBoundFn>(dlsym(api.handle, "LZ4_compressBound"));
        api.compress_default = reinterpret_cast<Lz4CompressApi::CompressDefaultFn>(dlsym(api.handle, "LZ4_compress_default"));
        if(!api.compress_bound || !api.compress_default) {
            throw std::runtime_error("liblz4.so.1 does not provide required compression symbols");
        }
    });
    return api;
}

std::vector<uint8_t> lz4_compress_payload(const uint8_t *data, size_t size) {
    if(data == nullptr || size == 0 || size > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("invalid lz4 compression input");
    }
    auto &api = lz4_compress_api();
    const int input_size = static_cast<int>(size);
    const int bound = api.compress_bound(input_size);
    if(bound <= 0) {
        throw std::runtime_error("lz4 compression bound failed");
    }
    std::vector<uint8_t> out(static_cast<size_t>(bound));
    const int compressed_size = api.compress_default(reinterpret_cast<const char *>(data),
                                                     reinterpret_cast<char *>(out.data()),
                                                     input_size,
                                                     bound);
    if(compressed_size <= 0) {
        throw std::runtime_error("lz4 depth compression failed");
    }
    out.resize(static_cast<size_t>(compressed_size));
    return out;
}

void quantize_depth8_into(const uint16_t *samples, size_t sample_count, uint16_t raw_step, uint8_t *out) {
    if(samples == nullptr || out == nullptr || raw_step == 0) {
        throw std::runtime_error("invalid q8 depth quantization input");
    }
    for(size_t i = 0; i < sample_count; ++i) {
        const uint16_t value = samples[i];
        if(value == 0) {
            out[i] = 0;
            continue;
        }
        const uint32_t rounded = (static_cast<uint32_t>(value) + raw_step / 2u) / raw_step;
        out[i] = static_cast<uint8_t>(std::clamp<uint32_t>(rounded, 1u, 255u));
    }
}

void append_varuint(std::vector<uint8_t> &out, uint32_t value) {
    while(value >= 0x80u) {
        append_u8(out, static_cast<uint8_t>((value & 0x7fu) | 0x80u));
        value >>= 7u;
    }
    append_u8(out, static_cast<uint8_t>(value));
}

uint32_t zigzag_encode_i32(int32_t value) {
    return static_cast<uint32_t>((value << 1) ^ (value >> 31));
}

std::vector<uint8_t> qdelta_compress_payload(const void *data, size_t size, uint16_t raw_step = 1) {
    if(data == nullptr || size == 0 || size % sizeof(uint16_t) != 0 || raw_step == 0) {
        throw std::runtime_error("invalid qdelta depth input");
    }

    const auto *samples = static_cast<const uint16_t *>(data);
    const size_t sample_count = size / sizeof(uint16_t);
    std::vector<uint8_t> out;
    out.reserve(size / 2);
    append_u8(out, 'Q');
    append_u8(out, 'D');
    append_u8(out, 'L');
    append_u8(out, '1');
    append_le16(out, raw_step);
    append_le16(out, 0);

    size_t index = 0;
    int32_t previous = 0;
    while(index < sample_count) {
        if(samples[index] == 0) {
            size_t zeros = 0;
            while(index + zeros < sample_count && samples[index + zeros] == 0) {
                ++zeros;
            }
            append_u8(out, 0);
            append_varuint(out, static_cast<uint32_t>(zeros));
            index += zeros;
            previous = 0;
            continue;
        }

        const size_t run_start = index;
        uint8_t run = 0;
        while(index < sample_count && samples[index] != 0 && run < std::numeric_limits<uint8_t>::max()) {
            ++index;
            ++run;
        }
        append_u8(out, 1);
        append_u8(out, run);
        for(size_t i = 0; i < run; ++i) {
            const uint16_t value = samples[run_start + i];
            uint32_t quantized = (static_cast<uint32_t>(value) + raw_step / 2u) / raw_step;
            quantized = std::max<uint32_t>(1u, quantized);
            if(quantized > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
                throw std::runtime_error("qdelta depth sample out of range");
            }
            const int32_t current = static_cast<int32_t>(quantized);
            append_varuint(out, zigzag_encode_i32(current - previous));
            previous = current;
        }
    }
    return out;
}

uint16_t raw_step_for_depth_scale(float depth_scale, double quantization_mm = 1.0) {
    if(!std::isfinite(depth_scale) || depth_scale <= 0.0f || quantization_mm <= 0.0) {
        return 1;
    }
    const double raw_step = std::ceil(quantization_mm / static_cast<double>(depth_scale));
    return static_cast<uint16_t>(std::clamp<double>(raw_step, 1.0, static_cast<double>(std::numeric_limits<uint16_t>::max())));
}

uint16_t quantize_depth12(uint16_t value, uint16_t raw_step) {
    if(value == 0) {
        return 0;
    }
    const uint32_t rounded = (static_cast<uint32_t>(value) + raw_step / 2u) / raw_step;
    return static_cast<uint16_t>(std::clamp<uint32_t>(rounded, 1u, 4095u));
}

size_t depth_chunk_compression_worker_count(size_t chunk_count) {
    if(chunk_count <= 1) {
        return 1;
    }
    static const int configured = [] {
        const char *value = std::getenv("GEMINI_DEPTH_CHUNK_COMPRESSION_WORKERS");
        if(value == nullptr || *value == '\0') {
            return 0;
        }
        try {
            return std::stoi(value);
        }
        catch(const std::exception &) {
            return 0;
        }
    }();
    size_t worker_limit = 0;
    if(configured > 0) {
        worker_limit = static_cast<size_t>(configured);
    }
    else {
        worker_limit = 1;
    }
    return std::clamp<size_t>(worker_limit, 1, chunk_count);
}

std::vector<uint8_t> pack_depth12_payload(const uint16_t *samples, size_t sample_count, uint16_t raw_step) {
    std::vector<uint8_t> packed;
    packed.reserve(((sample_count + 1u) / 2u) * 3u);
    for(size_t i = 0; i < sample_count; i += 2) {
        const uint16_t a = quantize_depth12(samples[i], raw_step);
        const uint16_t b = (i + 1 < sample_count) ? quantize_depth12(samples[i + 1], raw_step) : 0;
        packed.push_back(static_cast<uint8_t>(a & 0xffu));
        packed.push_back(static_cast<uint8_t>(((a >> 8u) & 0x0fu) | ((b & 0x0fu) << 4u)));
        packed.push_back(static_cast<uint8_t>((b >> 4u) & 0xffu));
    }
    return packed;
}

std::vector<uint8_t> pq12zlib_compress_payload(const void *data, size_t size, float depth_scale, double quantization_step_mm) {
    if(data == nullptr || size == 0 || size % sizeof(uint16_t) != 0) {
        throw std::runtime_error("invalid pq12zlib depth input");
    }

    constexpr size_t kSamplesPerChunk = 64 * 1024;
    constexpr uint32_t kMagic = 0x5a323150u;  // bytes: P 1 2 Z
    constexpr uint16_t kVersion = 1;
    constexpr size_t kHeaderSize = 20;
    constexpr size_t kChunkEntrySize = 12;

    const auto *samples = static_cast<const uint16_t *>(data);
    const size_t sample_count = size / sizeof(uint16_t);
    if(sample_count > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error("pq12zlib depth frame too large");
    }
    const size_t chunk_count = (sample_count + kSamplesPerChunk - 1) / kSamplesPerChunk;
    if(chunk_count == 0 || chunk_count > std::numeric_limits<uint16_t>::max()) {
        throw std::runtime_error("pq12zlib depth chunk count out of range");
    }

    const uint16_t raw_step = raw_step_for_depth_scale(depth_scale, quantization_step_mm);
    struct CompressedChunk {
        uint32_t sample_offset = 0;
        uint32_t sample_count = 0;
        std::vector<uint8_t> payload;
    };

    std::vector<CompressedChunk> chunks;
    chunks.reserve(chunk_count);
    for(size_t offset = 0; offset < sample_count; offset += kSamplesPerChunk) {
        const size_t count = std::min(kSamplesPerChunk, sample_count - offset);
        auto packed = pack_depth12_payload(samples + offset, count, raw_step);
        const auto bound = compressBound(static_cast<uLong>(packed.size()));
        CompressedChunk chunk;
        chunk.sample_offset = static_cast<uint32_t>(offset);
        chunk.sample_count = static_cast<uint32_t>(count);
        chunk.payload.resize(bound);
        uLongf out_size = bound;
        const int rc = compress2(chunk.payload.data(), &out_size, packed.data(), static_cast<uLong>(packed.size()), Z_BEST_SPEED);
        if(rc != Z_OK) {
            throw std::runtime_error("pq12zlib depth compression failed");
        }
        chunk.payload.resize(static_cast<size_t>(out_size));
        chunks.push_back(std::move(chunk));
    }

    size_t payload_size = kHeaderSize + chunks.size() * kChunkEntrySize;
    for(const auto &chunk : chunks) {
        payload_size += chunk.payload.size();
    }

    std::vector<uint8_t> out;
    out.reserve(payload_size);
    append_le32(out, kMagic);
    append_le16(out, kVersion);
    append_le16(out, raw_step);
    append_le32(out, static_cast<uint32_t>(sample_count));
    append_le16(out, static_cast<uint16_t>(chunks.size()));
    for(size_t i = out.size(); i < kHeaderSize; ++i) {
        append_u8(out, 0);
    }
    for(const auto &chunk : chunks) {
        append_le32(out, chunk.sample_offset);
        append_le32(out, chunk.sample_count);
        append_le32(out, static_cast<uint32_t>(chunk.payload.size()));
    }
    for(const auto &chunk : chunks) {
        append_bytes(out, chunk.payload.data(), chunk.payload.size());
    }
    return out;
}

std::vector<uint8_t> q8lz4_compress_payload(const void *data, size_t size, uint16_t raw_step = 1) {
    if(data == nullptr || size == 0 || size % sizeof(uint16_t) != 0 || raw_step == 0) {
        throw std::runtime_error("invalid q8lz4 depth input");
    }

    constexpr uint32_t kMagic = 0x314c3851u;  // bytes: Q 8 L 1
    constexpr uint16_t kVersion = 1;

    const auto *samples = static_cast<const uint16_t *>(data);
    const size_t sample_count = size / sizeof(uint16_t);
    if(sample_count > static_cast<size_t>(std::numeric_limits<uint32_t>::max())
       || sample_count > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("q8lz4 depth frame too large");
    }

    std::vector<uint8_t> quantized(sample_count);
    quantize_depth8_into(samples, sample_count, raw_step, quantized.data());
    auto compressed = lz4_compress_payload(quantized.data(), quantized.size());
    if(compressed.size() > static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
        throw std::runtime_error("q8lz4 depth payload too large");
    }

    std::vector<uint8_t> out;
    out.reserve(16 + compressed.size());
    append_le32(out, kMagic);
    append_le16(out, kVersion);
    append_le16(out, raw_step);
    append_le32(out, static_cast<uint32_t>(sample_count));
    append_le32(out, static_cast<uint32_t>(compressed.size()));
    append_bytes(out, compressed.data(), compressed.size());
    return out;
}

std::vector<uint8_t> pq8zlib_compress_payload(const void *data, size_t size, uint16_t raw_step = 1) {
    if(data == nullptr || size == 0 || size % sizeof(uint16_t) != 0 || raw_step == 0) {
        throw std::runtime_error("invalid pq8zlib depth input");
    }

    constexpr size_t kSamplesPerChunk = 64 * 1024;
    constexpr uint32_t kMagic = 0x5a385150u;  // bytes: P Q 8 Z
    constexpr uint16_t kVersion = 1;
    constexpr size_t kHeaderSize = 20;
    constexpr size_t kChunkEntrySize = 12;

    const auto *samples = static_cast<const uint16_t *>(data);
    const size_t sample_count = size / sizeof(uint16_t);
    if(sample_count > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error("pq8zlib depth frame too large");
    }
    const size_t chunk_count = (sample_count + kSamplesPerChunk - 1) / kSamplesPerChunk;
    if(chunk_count == 0 || chunk_count > std::numeric_limits<uint16_t>::max()) {
        throw std::runtime_error("pq8zlib depth chunk count out of range");
    }

    struct CompressedChunk {
        uint32_t sample_offset = 0;
        uint32_t sample_count = 0;
        std::vector<uint8_t> payload;
    };

    std::vector<CompressedChunk> chunks(chunk_count);
    for(size_t chunk_index = 0; chunk_index < chunk_count; ++chunk_index) {
        const size_t offset = chunk_index * kSamplesPerChunk;
        const size_t count = std::min(kSamplesPerChunk, sample_count - offset);
        chunks[chunk_index].sample_offset = static_cast<uint32_t>(offset);
        chunks[chunk_index].sample_count = static_cast<uint32_t>(count);
    }

    std::atomic<size_t> next_chunk{0};
    auto compress_chunk_worker = [&] {
        while(true) {
            const size_t chunk_index = next_chunk.fetch_add(1);
            if(chunk_index >= chunks.size()) {
                break;
            }
            auto &chunk = chunks[chunk_index];
            const size_t offset = chunk.sample_offset;
            const size_t count = chunk.sample_count;
            std::vector<uint8_t> quantized(count);
            quantize_depth8_into(samples + offset, count, raw_step, quantized.data());

            const auto bound = compressBound(static_cast<uLong>(quantized.size()));
            chunk.payload.resize(bound);
            uLongf out_size = bound;
            const int rc = compress2(chunk.payload.data(), &out_size, quantized.data(), static_cast<uLong>(quantized.size()), Z_BEST_SPEED);
            if(rc != Z_OK) {
                throw std::runtime_error("pq8zlib depth compression failed");
            }
            chunk.payload.resize(static_cast<size_t>(out_size));
        }
    };
    const size_t worker_count = depth_chunk_compression_worker_count(chunks.size());
    std::vector<std::future<void>> futures;
    futures.reserve(worker_count > 0 ? worker_count - 1 : 0);
    for(size_t worker = 1; worker < worker_count; ++worker) {
        futures.emplace_back(std::async(std::launch::async, compress_chunk_worker));
    }
    compress_chunk_worker();
    for(auto &future : futures) {
        future.get();
    }

    size_t payload_size = kHeaderSize + chunks.size() * kChunkEntrySize;
    for(const auto &chunk : chunks) {
        payload_size += chunk.payload.size();
    }

    std::vector<uint8_t> out;
    out.reserve(payload_size);
    append_le32(out, kMagic);
    append_le16(out, kVersion);
    append_le16(out, raw_step);
    append_le32(out, static_cast<uint32_t>(sample_count));
    append_le16(out, static_cast<uint16_t>(chunks.size()));
    for(size_t i = out.size(); i < kHeaderSize; ++i) {
        append_u8(out, 0);
    }
    for(const auto &chunk : chunks) {
        append_le32(out, chunk.sample_offset);
        append_le32(out, chunk.sample_count);
        append_le32(out, static_cast<uint32_t>(chunk.payload.size()));
    }
    for(const auto &chunk : chunks) {
        append_bytes(out, chunk.payload.data(), chunk.payload.size());
    }
    return out;
}

std::vector<uint8_t> pq8lz4_compress_payload(const void *data, size_t size, uint16_t raw_step = 1) {
    if(data == nullptr || size == 0 || size % sizeof(uint16_t) != 0 || raw_step == 0) {
        throw std::runtime_error("invalid pq8lz4 depth input");
    }

    constexpr size_t kSamplesPerChunk = 64 * 1024;
    constexpr uint32_t kMagic = 0x4c385150u;  // bytes: P Q 8 L
    constexpr uint16_t kVersion = 1;
    constexpr size_t kHeaderSize = 20;
    constexpr size_t kChunkEntrySize = 12;

    const auto *samples = static_cast<const uint16_t *>(data);
    const size_t sample_count = size / sizeof(uint16_t);
    if(sample_count > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error("pq8lz4 depth frame too large");
    }
    const size_t chunk_count = (sample_count + kSamplesPerChunk - 1) / kSamplesPerChunk;
    if(chunk_count == 0 || chunk_count > std::numeric_limits<uint16_t>::max()) {
        throw std::runtime_error("pq8lz4 depth chunk count out of range");
    }

    struct CompressedChunk {
        uint32_t sample_offset = 0;
        uint32_t sample_count = 0;
        std::vector<uint8_t> payload;
    };

    std::vector<CompressedChunk> chunks(chunk_count);
    for(size_t chunk_index = 0; chunk_index < chunk_count; ++chunk_index) {
        const size_t offset = chunk_index * kSamplesPerChunk;
        const size_t count = std::min(kSamplesPerChunk, sample_count - offset);
        chunks[chunk_index].sample_offset = static_cast<uint32_t>(offset);
        chunks[chunk_index].sample_count = static_cast<uint32_t>(count);
    }

    std::atomic<size_t> next_chunk{0};
    auto compress_chunk_worker = [&] {
        while(true) {
            const size_t chunk_index = next_chunk.fetch_add(1);
            if(chunk_index >= chunks.size()) {
                break;
            }
            auto &chunk = chunks[chunk_index];
            const size_t offset = chunk.sample_offset;
            const size_t count = chunk.sample_count;
            std::vector<uint8_t> quantized(count);
            quantize_depth8_into(samples + offset, count, raw_step, quantized.data());

            chunk.payload = lz4_compress_payload(quantized.data(), quantized.size());
            if(chunk.payload.size() > static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
                throw std::runtime_error("pq8lz4 depth chunk too large");
            }
        }
    };
    const size_t worker_count = depth_chunk_compression_worker_count(chunks.size());
    std::vector<std::future<void>> futures;
    futures.reserve(worker_count > 0 ? worker_count - 1 : 0);
    for(size_t worker = 1; worker < worker_count; ++worker) {
        futures.emplace_back(std::async(std::launch::async, compress_chunk_worker));
    }
    compress_chunk_worker();
    for(auto &future : futures) {
        future.get();
    }

    size_t payload_size = kHeaderSize + chunks.size() * kChunkEntrySize;
    for(const auto &chunk : chunks) {
        payload_size += chunk.payload.size();
    }

    std::vector<uint8_t> out;
    out.reserve(payload_size);
    append_le32(out, kMagic);
    append_le16(out, kVersion);
    append_le16(out, raw_step);
    append_le32(out, static_cast<uint32_t>(sample_count));
    append_le16(out, static_cast<uint16_t>(chunks.size()));
    for(size_t i = out.size(); i < kHeaderSize; ++i) {
        append_u8(out, 0);
    }
    for(const auto &chunk : chunks) {
        append_le32(out, chunk.sample_offset);
        append_le32(out, chunk.sample_count);
        append_le32(out, static_cast<uint32_t>(chunk.payload.size()));
    }
    for(const auto &chunk : chunks) {
        append_bytes(out, chunk.payload.data(), chunk.payload.size());
    }
    return out;
}

std::vector<uint8_t> compress_depth_payload(const std::string &compression,
                                            const uint8_t *raw_payload,
                                            size_t raw_payload_size,
                                            float depth_scale,
                                            double quantization_step_mm) {
    if(raw_payload == nullptr || raw_payload_size == 0) {
        throw std::runtime_error("invalid depth compression input");
    }
    if(compression == "zlib") {
        return zlib_compress_payload(raw_payload, raw_payload_size);
    }
    if(compression == "qdelta") {
        return qdelta_compress_payload(raw_payload, raw_payload_size, 1);
    }
    if(compression == "pq12zlib") {
        return pq12zlib_compress_payload(raw_payload, raw_payload_size, depth_scale, quantization_step_mm);
    }
    if(compression == "q8lz4") {
        return q8lz4_compress_payload(raw_payload, raw_payload_size, raw_step_for_depth_scale(depth_scale, quantization_step_mm));
    }
    if(compression == "pq8zlib") {
        return pq8zlib_compress_payload(raw_payload, raw_payload_size, raw_step_for_depth_scale(depth_scale, quantization_step_mm));
    }
    if(compression == "pq8lz4") {
        return pq8lz4_compress_payload(raw_payload, raw_payload_size, raw_step_for_depth_scale(depth_scale, quantization_step_mm));
    }
    throw std::runtime_error("unsupported depth compression: " + compression);
}

bool depth_transport_uses_compression(const std::string &compression) {
    return compression != "none";
}

Json::Value sender_hello(const AppConfig &config) {
    Json::Value msg = base_message(config, "sender_hello");
    msg["sender_version"] = config.sender_version;
    msg["host_name"] = hostname();
    msg["os"] = "linux";
    msg["arch"] = architecture();
    Json::Value capabilities;
    Json::Value rgb(Json::arrayValue);
    rgb.append("h264");
    capabilities["rgb_encoding"] = rgb;
    Json::Value depth(Json::arrayValue);
    depth.append("none");
    depth.append("zlib");
    depth.append("qdelta");
    depth.append("pq12zlib");
    depth.append("q8lz4");
    depth.append("pq8zlib");
    depth.append("pq8lz4");
    capabilities["depth_compression"] = depth;
    capabilities["local_preview"] = config.preview.enabled;
    capabilities["web_rgb_preview"] = config.web_rgb_preview.enabled;
    capabilities["web_rgb_preview_on_demand"] = config.web_rgb_preview.on_demand;
    capabilities["hotplug"] = config.hotplug.enabled;
    capabilities["clock_sync"] = config.clock_sync.enabled;
    capabilities["clock_sync_port"] = config.clock_sync.port;
    capabilities["media_protocol"] = config.transport.media_protocol;
    capabilities["status_protocol"] = config.transport.status_protocol;
    msg["capabilities"] = capabilities;
    return msg;
}

Json::Value camera_announce(const AppConfig &config, CameraRuntime &camera) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    Json::Value msg = base_message(config, "camera_announce");
    msg["camera_id"] = camera.config.camera_id;
    Json::Value device;
    device["vendor"] = "orbbec";
    device["model"] = camera.config.capture_backend == "v4l2" ? "UVC RGB" : "";
    device["serial_number"] = camera.device_serial;
    device["uid"] = camera.device_uid;
    device["paired_rgb_serial"] = camera.config.capture_backend == "v4l2" ? camera.device_serial : paired_rgb_serial_for_depth_uid(camera.device_uid);
    device["configured_model"] = camera.config.device_model;
    device["configured_serial_number"] = camera.config.serial_number;
    device["configured_uid"] = camera.config.uid;
    device["firmware_version"] = "";
    device["connection_type"] = camera.device_connection_type;
    if(camera.device) {
        auto info = camera.device->getDeviceInfo();
        device["model"] = info->name() ? info->name() : "";
        device["serial_number"] = info->serialNumber() ? info->serialNumber() : "";
        device["uid"] = info->uid() ? info->uid() : "";
        device["paired_rgb_serial"] = paired_rgb_serial_for_depth_uid(device["uid"].asString());
        device["firmware_version"] = info->firmwareVersion() ? info->firmwareVersion() : "";
        device["connection_type"] = info->connectionType() ? info->connectionType() : "";
    }
    msg["device"] = device;
    msg["rgb_profile"] = camera.color_profile ? profile_json(camera.color_profile, "encoded_video", camera.config.rgb_encoding.codec)
                                               : profile_json(camera.config.rgb_profile, "encoded_video", camera.config.rgb_encoding.codec);
    msg["depth_profile"] = camera.depth_profile ? profile_json(camera.depth_profile, "uint16", camera.config.depth_transport.compression, camera.depth_scale)
                                                 : profile_json(camera.config.depth_profile, "uint16", camera.config.depth_transport.compression,
                                                                camera.depth_scale);
    if(camera.config.rotation_degrees) {
        msg["rotation_degrees"] = *camera.config.rotation_degrees;
    }
    Json::Value web_preview;
    web_preview["enabled"] = config.web_rgb_preview.enabled;
    web_preview["on_demand"] = config.web_rgb_preview.on_demand;
    web_preview["max_width"] = config.web_rgb_preview.max_width;
    web_preview["max_height"] = config.web_rgb_preview.max_height;
    web_preview["fps"] = config.web_rgb_preview.fps;
    web_preview["bitrate_bps"] = config.web_rgb_preview.bitrate_bps;
    web_preview["width"] = Json::UInt(camera.web_preview_width);
    web_preview["height"] = Json::UInt(camera.web_preview_height);
    msg["web_rgb_preview"] = web_preview;
    if(camera.pipeline && camera.device) {
        msg["calibration"] = calibration_json(*camera.pipeline, camera.pipeline_config, camera.device, camera.color_profile,
                                                camera.depth_profile);
    }
    else {
        Json::Value calibration;
        calibration["available"] = false;
        calibration["reason"] = "not_available_for_v4l2_rgb_only";
        msg["calibration"] = calibration;
    }
    return msg;
}

Json::Value event_message(const AppConfig &config, const std::string &level, const std::string &code, const std::string &message,
                          const std::string &camera_id = "") {
    Json::Value msg = base_message(config, "event");
    if(!camera_id.empty()) {
        msg["camera_id"] = camera_id;
    }
    msg["level"] = level;
    msg["event_code"] = code;
    msg["message"] = message;
    return msg;
}

Json::Value camera_offline_message(const AppConfig &config, const std::string &camera_id, const std::string &reason) {
    Json::Value msg = base_message(config, "camera_offline");
    msg["camera_id"] = camera_id;
    msg["reason"] = reason.empty() ? "camera_offline" : reason;
    return msg;
}

Json::Value camera_heartbeat(const AppConfig &config,
                             CameraRuntime &camera,
                             std::chrono::steady_clock::time_point started,
                             const ClockSyncClient *clock_sync) {
    Json::Value msg = base_message(config, "heartbeat");
    const auto uptime = std::chrono::steady_clock::now() - started;
    msg["uptime_ms"] = Json::UInt64(std::chrono::duration_cast<std::chrono::milliseconds>(uptime).count());
    const auto clock_state = clock_sync ? clock_sync->state() : ClockSyncClientState{};
    const bool clock_valid = clock_sync && clock_sync->healthy() && clock_state.valid;
    msg["clock_sync_valid"] = clock_valid;
    msg["clock_offset_us"] = Json::Int64(clock_state.offset_us);
    msg["clock_delay_us"] = Json::Int64(clock_state.delay_us);
    msg["clock_drift_ppm"] = clock_state.drift_ppm;
    msg["clock_last_sync_us"] = Json::UInt64(clock_state.last_sync_us);
    msg["clock_samples"] = Json::UInt64(clock_state.sample_count);
    std::lock_guard<std::mutex> lock(camera.mutex);
    const auto seconds = std::max(0.001, std::chrono::duration<double>(std::chrono::steady_clock::now() - camera.stats_started).count());
    msg["camera_id"] = camera.config.camera_id;
    msg["online"] = camera.online;
    msg["rgb_fps"] = static_cast<double>(camera.rgb_frames) / seconds;
    msg["depth_fps"] = static_cast<double>(camera.depth_frames) / seconds;
    msg["rgb_encoding"] = camera.config.rgb_encoding.codec;
    msg["depth_compression"] = camera.config.depth_transport.compression;
    msg["rgb_dropped_frames"] = Json::UInt64(camera.rgb_dropped);
    msg["rgb_transport_retry_drops"] = Json::UInt64(camera.rgb_transport_retry_drops);
    msg["rgb_send_failures_total"] = Json::UInt64(camera.rgb_send_failures_total);
    msg["rgb_preview_send_failures_total"] = Json::UInt64(camera.rgb_preview_send_failures_total);
    msg["depth_send_failures_total"] = Json::UInt64(camera.depth_send_failures_total);
    msg["rgb_corrupt_jpeg_frames"] = Json::UInt64(camera.rgb_corrupt_jpeg);
    msg["depth_dropped_frames"] = Json::UInt64(camera.depth_dropped);
    msg["hardware_encoder"] = camera.hardware_encoder;
    msg["rgb_measured_fps"] = camera.live.rgb_input_fps;
    msg["depth_measured_fps"] = camera.live.depth_input_fps;
    msg["rgb_sent_fps"] = camera.live.rgb_sent_fps;
    msg["depth_sent_fps"] = camera.live.depth_sent_fps;
    msg["rgb_mbps"] = camera.live.rgb_mbps;
    msg["rgb_preview_mbps"] = camera.live.rgb_preview_mbps;
    msg["rgb_preview_fps"] = camera.live.rgb_preview_sent_fps;
    msg["rgb_preview_width"] = Json::UInt(camera.web_preview_width);
    msg["rgb_preview_height"] = Json::UInt(camera.web_preview_height);
    msg["depth_mbps"] = camera.live.depth_mbps;
    msg["rgb_auto_exposure"] = Json::Int64(camera.live.color_auto_exposure);
    msg["rgb_exposure"] = Json::Int64(camera.live.color_exposure);
    msg["rgb_gain"] = Json::Int64(camera.live.color_gain);
    msg["rgb_metadata_actual_fps"] = Json::Int64(camera.live.color_actual_fps);
    msg["rgb_exposure_priority"] = Json::Int64(camera.live.color_exposure_priority);
    msg["adaptive_exposure_enabled"] = camera.live.adaptive_exposure_enabled;
    msg["adaptive_exposure_luma_p50"] = camera.live.adaptive_luma_p50;
    msg["adaptive_exposure_luma_p95"] = camera.live.adaptive_luma_p95;
    msg["adaptive_exposure_luma_p99"] = camera.live.adaptive_luma_p99;
    msg["adaptive_exposure_highlight_fraction"] = camera.live.adaptive_highlight_fraction;
    msg["adaptive_exposure_requested_exposure"] = camera.live.adaptive_requested_exposure;
    msg["adaptive_exposure_requested_gain"] = camera.live.adaptive_requested_gain;
    msg["adaptive_exposure_samples"] = Json::UInt64(camera.live.adaptive_samples);
    msg["adaptive_exposure_adjustments"] = Json::UInt64(camera.live.adaptive_adjustments);
    msg["adaptive_exposure_failures"] = Json::UInt64(camera.live.adaptive_failures);
    msg["adaptive_exposure_last_reason"] = camera.live.adaptive_last_reason;
    msg["publish_warmup_active"] = camera.publish_warmup_active;
    msg["publish_warmup_dropped_framesets"] = Json::UInt64(camera.publish_warmup_dropped_framesets);
    msg["last_error"] = camera.last_error;
    return msg;
}

template <typename Sender>
bool send_status(Sender &sender, Logger &logger, const Json::Value &message) {
    const auto payload = json_to_string(message);
    if(!sender.send_status(payload)) {
        logger.warn("status send failed: " + sender.last_error());
        return false;
    }
    return true;
}

void preview_frame(const CameraRuntime &camera, bool preview_enabled) {
    cv::Mat bgr;
    cv::Mat depth_color;
    std::string label;
    {
        std::lock_guard<std::mutex> lock(camera.mutex);
        bgr = camera.latest_bgr.clone();
        depth_color = camera.latest_depth_color.clone();
        label = camera.config.camera_id;
        if(!camera.device_uid.empty()) {
            label += " uid=" + camera.device_uid;
        }
        if(!camera.device_serial.empty()) {
            label += " serial=" + camera.device_serial;
        }
    }
    if(!preview_enabled || bgr.empty() || depth_color.empty()) {
        return;
    }
    const std::string window_name = "Gemini Sender " + camera.config.camera_id;
    static std::map<std::string, int> initialized_windows;
    if(initialized_windows.find(window_name) == initialized_windows.end()) {
        const int window_index = static_cast<int>(initialized_windows.size());
        initialized_windows[window_name] = window_index;
        cv::namedWindow(window_name, cv::WINDOW_NORMAL);
        cv::resizeWindow(window_name, 960, 360);
        cv::moveWindow(window_name, 80 + (window_index % 2) * 520, 80 + (window_index / 2) * 420);
    }
    cv::Mat rgb_small;
    cv::Mat depth_small;
    cv::resize(bgr, rgb_small, cv::Size(480, 360));
    cv::resize(depth_color, depth_small, cv::Size(480, 360));
    cv::Mat wall;
    cv::hconcat(rgb_small, depth_small, wall);
    cv::putText(wall, "RGB " + label, cv::Point(12, 28), cv::FONT_HERSHEY_SIMPLEX, 0.55, cv::Scalar(255, 255, 255), 2,
                cv::LINE_AA);
    cv::putText(wall, "DEPTH " + label, cv::Point(492, 28), cv::FONT_HERSHEY_SIMPLEX, 0.55, cv::Scalar(255, 255, 255), 2,
                cv::LINE_AA);
    cv::imshow(window_name, wall);
    cv::waitKey(1);
}

struct AlignedRgbPreviewPair {
    RgbPreviewFrame cam01;
    RgbPreviewFrame cam02;
    std::string cam01_label;
    std::string cam02_label;
    int64_t cam01_minus_cam02_us = 0;
};

std::string camera_preview_label_locked(const CameraRuntime &camera) {
    std::string label = camera.config.camera_id;
    if(!camera.device_uid.empty()) {
        label += " uid=" + camera.device_uid;
    }
    if(!camera.device_serial.empty()) {
        label += " serial=" + camera.device_serial;
    }
    return label;
}

uint64_t abs_diff_us(uint64_t lhs, uint64_t rhs) {
    return lhs >= rhs ? lhs - rhs : rhs - lhs;
}

struct RgbEncodeTimingResolution {
    RgbEncodeTiming timing;
    bool used_fifo_fallback = false;
    bool non_vcl = false;
    bool queue_empty = false;
    uint64_t pts_delta_us = 0;
};

void remember_rgb_encode_timing(CameraRuntime &camera, const RgbEncodeTiming &timing) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    camera.rgb_encode_timings.push_back(timing);
    while(camera.rgb_encode_timings.size() > kMaxRgbEncodeTimingFrames) {
        camera.rgb_encode_timings.pop_front();
    }
}

RgbEncodeTimingResolution resolve_rgb_encode_timing(CameraRuntime &camera, const EncodedH264Frame &encoded,
                                                    const RgbEncodeTiming &fallback, bool encoded_has_vcl) {
    if(!encoded_has_vcl) {
        RgbEncodeTimingResolution resolution;
        resolution.timing = fallback;
        resolution.non_vcl = true;
        return resolution;
    }

    std::lock_guard<std::mutex> lock(camera.mutex);
    if(camera.rgb_encode_timings.empty()) {
        RgbEncodeTimingResolution resolution;
        resolution.timing = fallback;
        resolution.queue_empty = true;
        return resolution;
    }

    RgbEncodeTimingResolution resolution;
    if(encoded.has_pts) {
        auto best = camera.rgb_encode_timings.end();
        uint64_t best_delta_us = std::numeric_limits<uint64_t>::max();
        for(auto it = camera.rgb_encode_timings.begin(); it != camera.rgb_encode_timings.end(); ++it) {
            const uint64_t delta_us = abs_diff_us(it->system_timestamp_us, encoded.pts_us);
            if(delta_us < best_delta_us) {
                best_delta_us = delta_us;
                best = it;
                if(delta_us == 0) {
                    break;
                }
            }
        }
        if(best != camera.rgb_encode_timings.end() && best_delta_us <= kRgbPtsMatchToleranceUs) {
            resolution.timing = *best;
            resolution.pts_delta_us = best_delta_us;
            camera.rgb_encode_timings.erase(camera.rgb_encode_timings.begin(), std::next(best));
            return resolution;
        }
    }

    resolution.timing = camera.rgb_encode_timings.front();
    resolution.used_fifo_fallback = true;
    if(encoded.has_pts) {
        resolution.pts_delta_us = abs_diff_us(resolution.timing.system_timestamp_us, encoded.pts_us);
    }
    camera.rgb_encode_timings.pop_front();
    return resolution;
}

void maybe_log_rgb_timing_resolution(CameraRuntime &camera, Logger &logger, const EncodedH264Frame &,
                                     const RgbEncodeTimingResolution &resolution, std::chrono::steady_clock::time_point now) {
    if(!resolution.queue_empty) {
        return;
    }
    bool should_log = false;
    {
        std::lock_guard<std::mutex> lock(camera.mutex);
        if(now >= camera.next_rgb_timing_warning) {
            camera.next_rgb_timing_warning = now + std::chrono::seconds(2);
            should_log = true;
        }
    }
    if(!should_log) {
        return;
    }

    std::ostringstream oss;
    oss << "rgb encoder timing fallback camera_id=" << camera.config.camera_id;
    if(resolution.queue_empty) {
        oss << " reason=empty_timing_queue";
    }
    oss << " selected_frame_id=" << resolution.timing.frame_id
        << " selected_system_timestamp_us=" << resolution.timing.system_timestamp_us;
    logger.warn(oss.str());
}

bool rgb_encoded_timing_is_monotonic(CameraRuntime &camera, Logger &logger, const RgbEncodeTiming &timing,
                                     std::chrono::steady_clock::time_point now) {
    bool regression = false;
    bool should_log = false;
    uint64_t last_frame_id = 0;
    uint64_t last_system_timestamp_us = 0;
    {
        std::lock_guard<std::mutex> lock(camera.mutex);
        if(camera.rgb_sent_timing_seen) {
            const bool frame_id_regressed = timing.frame_id <= camera.rgb_last_sent_frame_id;
            const bool system_timestamp_regressed =
                timing.system_timestamp_us > 0 && camera.rgb_last_sent_system_timestamp_us > 0
                && timing.system_timestamp_us <= camera.rgb_last_sent_system_timestamp_us;
            regression = frame_id_regressed || system_timestamp_regressed;
            if(regression) {
                last_frame_id = camera.rgb_last_sent_frame_id;
                last_system_timestamp_us = camera.rgb_last_sent_system_timestamp_us;
                camera.perf.rgb_timing_mismatch_drops++;
                camera.rgb_timing_mismatch_drops++;
                camera.rgb_dropped++;
                camera.last_error = "rgb encoded timing regression dropped";
                if(now >= camera.next_rgb_timing_warning) {
                    camera.next_rgb_timing_warning = now + std::chrono::seconds(1);
                    should_log = true;
                }
            }
        }
    }
    if(!regression) {
        return true;
    }
    if(should_log) {
        std::ostringstream oss;
        oss << "rgb encoded timing regression dropped camera_id=" << camera.config.camera_id
            << " frame_id=" << timing.frame_id
            << " last_frame_id=" << last_frame_id
            << " system_timestamp_us=" << timing.system_timestamp_us
            << " last_system_timestamp_us=" << last_system_timestamp_us;
        logger.warn(oss.str());
    }
    arm_rgb_keyframe_guard(camera, logger, "rgb encoded timing regression; waiting for next keyframe");
    return false;
}

void mark_rgb_encoded_timing_queued(CameraRuntime &camera, const RgbEncodeTiming &timing) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    camera.rgb_sent_timing_seen = true;
    camera.rgb_last_sent_frame_id = timing.frame_id;
    camera.rgb_last_sent_system_timestamp_us = timing.system_timestamp_us;
}

bool rgb_encoder_output_lag_too_large(CameraRuntime &camera, Logger &logger, const RgbEncodeTiming &timing,
                                      uint64_t encode_done_timestamp_us, std::chrono::steady_clock::time_point now) {
    if(timing.system_timestamp_us == 0 || encode_done_timestamp_us <= timing.system_timestamp_us) {
        return false;
    }
    const uint64_t lag_us = encode_done_timestamp_us - timing.system_timestamp_us;
    if(lag_us <= kRgbEncoderOutputLagResetUs) {
        return false;
    }

    std::unique_ptr<GstH264Encoder> old_encoder;
    std::unique_ptr<GstH264Encoder> old_preview_encoder;
    std::unique_ptr<GstJpegDualH264Encoder> old_dual_encoder;
    bool should_log = false;
    {
        std::lock_guard<std::mutex> lock(camera.mutex);
        old_encoder = std::move(camera.encoder);
        old_preview_encoder = std::move(camera.web_preview_encoder);
        old_dual_encoder = std::move(camera.jpeg_dual_encoder);
        camera.jpeg_dual_encoder_disabled = true;
        camera.jpeg_dual_no_main_output = 0;
        camera.web_preview_width = 0;
        camera.web_preview_height = 0;
        camera.web_rgb_preview_requested_until = std::chrono::steady_clock::time_point::min();
        camera.web_rgb_preview_suppressed_until = now + kWebRgbPreviewSuppressAfterEncoderLag;
        camera.rgb_encode_timings.clear();
        camera.rgb_sent_timing_seen = false;
        camera.rgb_last_sent_frame_id = 0;
        camera.rgb_last_sent_system_timestamp_us = 0;
        camera.rgb_dropped++;
        camera.perf.rgb_encoder_lag_resets++;
        camera.last_error = "rgb encoder output lag reset";
        if(now >= camera.next_rgb_encoder_lag_warning) {
            camera.next_rgb_encoder_lag_warning = now + std::chrono::seconds(2);
            should_log = true;
        }
    }

    if(should_log) {
        std::ostringstream oss;
        oss << "rgb encoder output lag reset camera_id=" << camera.config.camera_id
            << " frame_id=" << timing.frame_id
            << " lag_us=" << lag_us
            << " threshold_us=" << kRgbEncoderOutputLagResetUs
            << " system_timestamp_us=" << timing.system_timestamp_us
            << " encode_done_timestamp_us=" << encode_done_timestamp_us
            << " web_preview_suppressed_ms="
            << std::chrono::duration_cast<std::chrono::milliseconds>(kWebRgbPreviewSuppressAfterEncoderLag).count();
        logger.warn(oss.str());
    }
    return true;
}

void set_rgb_pipeline_diagnostics(MediaFrameMeta &meta, const RgbEncodeTiming &timing) {
    meta.flags |= has_pipeline_diagnostics;
    meta.sender_capture_host_timestamp_us = timing.capture_host_timestamp_us;
    meta.sender_timing_bound_timestamp_us = timing.timing_bound_timestamp_us;
    meta.sender_encode_start_timestamp_us = timing.encode_start_timestamp_us;
    meta.sender_encode_done_timestamp_us = timing.encode_done_timestamp_us;
    meta.sender_packet_queued_timestamp_us = timing.packet_queued_timestamp_us;
}

std::optional<AlignedRgbPreviewPair> pop_aligned_rgb_preview_pair(CameraRuntime &cam01, CameraRuntime &cam02) {
    std::scoped_lock lock(cam01.mutex, cam02.mutex);
    while(!cam01.rgb_preview_queue.empty() && !cam02.rgb_preview_queue.empty()) {
        const auto &left = cam01.rgb_preview_queue.front();
        const auto &right = cam02.rgb_preview_queue.front();
        const auto delta_us = abs_diff_us(left.system_timestamp_us, right.system_timestamp_us);
        if(delta_us <= kAlignedRgbPreviewDisplayMaxDeltaUs) {
            AlignedRgbPreviewPair pair;
            pair.cam01 = left;
            pair.cam02 = right;
            pair.cam01_label = camera_preview_label_locked(cam01);
            pair.cam02_label = camera_preview_label_locked(cam02);
            pair.cam01_minus_cam02_us =
                left.system_timestamp_us >= right.system_timestamp_us
                    ? static_cast<int64_t>(left.system_timestamp_us - right.system_timestamp_us)
                    : -static_cast<int64_t>(right.system_timestamp_us - left.system_timestamp_us);
            cam01.rgb_preview_queue.pop_front();
            cam02.rgb_preview_queue.pop_front();
            return pair;
        }
        if(left.system_timestamp_us < right.system_timestamp_us) {
            cam01.rgb_preview_queue.pop_front();
        }
        else {
            cam02.rgb_preview_queue.pop_front();
        }
    }
    return std::nullopt;
}

void put_text_with_outline(cv::Mat &image, const std::string &text, const cv::Point &origin, double scale,
                           const cv::Scalar &color) {
    cv::putText(image, text, origin, cv::FONT_HERSHEY_SIMPLEX, scale, cv::Scalar(0, 0, 0), 4, cv::LINE_AA);
    cv::putText(image, text, origin, cv::FONT_HERSHEY_SIMPLEX, scale, color, 1, cv::LINE_AA);
}

void draw_rgb_preview_overlay(cv::Mat &panel, const std::string &label, const RgbPreviewFrame &frame, int64_t delta_us,
                              const std::string &delta_label) {
    const cv::Scalar text_color(255, 255, 255);
    const cv::Scalar delta_color(std::abs(delta_us) <= static_cast<int64_t>(kAlignedRgbPreviewTargetDeltaUs) ? cv::Scalar(80, 255, 120)
                                                                                                             : cv::Scalar(40, 220, 255));
    std::ostringstream frame_line;
    frame_line << label << " RGB frame=" << frame.frame_id;
    std::ostringstream ts_line;
    ts_line << "system_ts_us=" << frame.system_timestamp_us;
    std::ostringstream delta_line;
    delta_line << std::fixed << std::setprecision(2) << delta_label << "=" << static_cast<double>(delta_us) / 1000.0 << "ms";

    put_text_with_outline(panel, frame_line.str(), cv::Point(12, 28), 0.55, text_color);
    put_text_with_outline(panel, ts_line.str(), cv::Point(12, 54), 0.48, text_color);
    put_text_with_outline(panel, delta_line.str(), cv::Point(12, 80), 0.52, delta_color);
}

std::filesystem::path default_rgb_snapshot_request_dir() {
    const char *value = std::getenv("GEMINI_RGB_SNAPSHOT_REQUEST_DIR");
    return value && *value ? std::filesystem::path(value) : std::filesystem::path("/tmp/gemini_rgb_snapshot_requests");
}

std::filesystem::path default_rgb_snapshot_result_dir() {
    const char *value = std::getenv("GEMINI_RGB_SNAPSHOT_RESULT_DIR");
    return value && *value ? std::filesystem::path(value) : std::filesystem::path("/tmp/gemini_rgb_snapshot_results");
}

bool is_safe_rgb_snapshot_request_id(const std::string &value) {
    return !value.empty() && value.size() <= 96
           && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
                  return std::isalnum(ch) || ch == '_' || ch == '-' || ch == '.';
              });
}

std::filesystem::path safe_rgb_snapshot_result_path(const std::string &request_id, const std::string &requested_path) {
    const auto result_dir = default_rgb_snapshot_result_dir().lexically_normal();
    const auto fallback = result_dir / (request_id + ".json");
    if(requested_path.empty()) {
        return fallback;
    }
    const auto candidate = std::filesystem::path(requested_path).lexically_normal();
    if(candidate.filename() != request_id + ".json" || candidate.parent_path() != result_dir) {
        return fallback;
    }
    return candidate;
}

void write_rgb_snapshot_result(const AppConfig &config,
                               const std::string &camera_id,
                               const RgbSnapshotRequest &request,
                               bool ok,
                               const std::string &status,
                               const std::string &image_path,
                               const std::string &error,
                               Logger &logger) {
    if(request.result_path.empty()) {
        return;
    }

    Json::Value root(Json::objectValue);
    root["message_type"] = "rgb_snapshot_result";
    root["request_id"] = request.request_id;
    root["sender_id"] = config.sender_id;
    root["camera_id"] = camera_id;
    root["ok"] = ok;
    root["status"] = status;
    root["image_path"] = image_path;
    root["error"] = error;
    root["trigger"] = request.trigger;
    root["requested_at_unix_us"] = Json::UInt64(request.requested_at_unix_us);
    root["completed_at_unix_us"] = Json::UInt64(now_us());

    const std::filesystem::path result_path = request.result_path;
    std::error_code ec;
    std::filesystem::create_directories(result_path.parent_path(), ec);
    if(ec) {
        logger.warn("rgb snapshot result directory create failed path=" + result_path.parent_path().string()
                    + " error=" + ec.message());
        return;
    }

    const auto temporary = result_path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::out | std::ios::trunc);
        if(!output) {
            logger.warn("rgb snapshot result write failed path=" + temporary);
            return;
        }
        output << json_to_string(root) << '\n';
        output.flush();
        if(!output) {
            logger.warn("rgb snapshot result flush failed path=" + temporary);
            return;
        }
    }
    std::filesystem::rename(temporary, result_path, ec);
    if(ec) {
        logger.warn("rgb snapshot result rename failed path=" + result_path.string() + " error=" + ec.message());
        std::filesystem::remove(temporary, ec);
    }
}

void remove_file_quietly(const std::filesystem::path &path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

CameraRuntime *choose_rgb_snapshot_camera(const std::vector<std::unique_ptr<CameraRuntime>> &cameras,
                                          const std::string &camera_id) {
    if(!camera_id.empty() && camera_id != "*") {
        for(const auto &camera : cameras) {
            if(camera && camera->config.camera_id == camera_id) {
                return camera.get();
            }
        }
        return nullptr;
    }
    for(const auto &camera : cameras) {
        if(camera && camera->config.camera_id == "cam01") {
            return camera.get();
        }
    }
    return cameras.empty() ? nullptr : cameras.front().get();
}

void process_rgb_snapshot_request_file(const AppConfig &config,
                                       const std::vector<std::unique_ptr<CameraRuntime>> &cameras,
                                       Logger &logger,
                                       const std::filesystem::path &path) {
    const auto root = parse_json_object(read_text_file(path));
    if(!root) {
        logger.warn("rgb snapshot request ignored invalid_json path=" + path.string());
        remove_file_quietly(path);
        return;
    }
    const std::string message_type = json_string_or(*root, "message_type");
    if(!message_type.empty() && message_type != "rgb_snapshot_request" && message_type != "snapshot_request") {
        logger.warn("rgb snapshot request ignored unknown_type path=" + path.string() + " message_type=" + message_type);
        remove_file_quietly(path);
        return;
    }
    const std::string target_sender = json_string_or(*root, "sender_id");
    if(!target_sender.empty() && target_sender != "*" && target_sender != config.sender_id) {
        remove_file_quietly(path);
        return;
    }

    RgbSnapshotRequest request;
    request.request_id = json_string_or(*root, "request_id", path.stem().string());
    request.camera_id = json_string_or(*root, "camera_id", "cam01");
    request.trigger = json_string_or(*root, "trigger", "local_request");
    request.requested_at_unix_us = json_uint64_or(*root, "requested_at_unix_us", now_us());
    request.capture_not_before_unix_us = json_uint64_or(*root, "capture_not_before_unix_us", 0);
    if(!is_safe_rgb_snapshot_request_id(request.request_id)) {
        logger.warn("rgb snapshot request ignored invalid request_id path=" + path.string());
        remove_file_quietly(path);
        return;
    }
    if(request.capture_not_before_unix_us > now_us() + 30ull * 1000ull * 1000ull) {
        logger.warn("rgb snapshot request ignored because capture schedule is too far in the future request_id="
                    + request.request_id);
        remove_file_quietly(path);
        return;
    }
    request.result_path =
        safe_rgb_snapshot_result_path(request.request_id, json_string_or(*root, "result_path")).string();

    CameraRuntime *camera = choose_rgb_snapshot_camera(cameras, request.camera_id);
    if(camera == nullptr) {
        logger.warn("rgb snapshot request matched no camera request_id=" + request.request_id
                    + " camera_id=" + request.camera_id);
        write_rgb_snapshot_result(config, request.camera_id, request, false, "error", "",
                                  "snapshot request matched no camera", logger);
        remove_file_quietly(path);
        return;
    }

    bool queued = false;
    std::string queue_error;
    {
        std::lock_guard<std::mutex> lock(camera->mutex);
        const bool duplicate = camera->pending_rgb_snapshots.count(request.request_id) != 0
                               || std::any_of(camera->rgb_snapshot_requests.begin(), camera->rgb_snapshot_requests.end(),
                                              [&](const auto &item) { return item.request_id == request.request_id; });
        if(duplicate) {
            remove_file_quietly(path);
            return;
        }
        else if(camera->rgb_snapshot_requests.size() + camera->pending_rgb_snapshots.size()
                >= kRgbSnapshotMaxPendingPerCamera) {
            queue_error = "snapshot request queue is full";
        }
        else {
            request.camera_id = camera->config.camera_id;
            const auto insert_at = std::upper_bound(
                camera->rgb_snapshot_requests.begin(), camera->rgb_snapshot_requests.end(), request,
                [](const RgbSnapshotRequest &left, const RgbSnapshotRequest &right) {
                    if(left.capture_not_before_unix_us != right.capture_not_before_unix_us) {
                        return left.capture_not_before_unix_us < right.capture_not_before_unix_us;
                    }
                    return left.request_id < right.request_id;
                });
            camera->rgb_snapshot_requests.insert(insert_at, request);
            queued = true;
        }
    }
    if(queued) {
        logger.info("rgb snapshot request queued request_id=" + request.request_id
                    + " camera_id=" + camera->config.camera_id
                    + " capture_not_before_unix_us=" + std::to_string(request.capture_not_before_unix_us));
    }
    else {
        logger.warn("rgb snapshot request rejected request_id=" + request.request_id + " error=" + queue_error);
        write_rgb_snapshot_result(config, camera->config.camera_id, request, false, "error", "", queue_error, logger);
    }
    remove_file_quietly(path);
}

void poll_rgb_snapshot_requests(const AppConfig &config,
                                const std::vector<std::unique_ptr<CameraRuntime>> &cameras,
                                Logger &logger,
                                std::chrono::steady_clock::time_point now,
                                std::chrono::steady_clock::time_point &next_poll) {
    if(now < next_poll) {
        return;
    }
    next_poll = now + kRgbSnapshotRequestPollInterval;

    const auto request_dir = default_rgb_snapshot_request_dir();
    std::error_code ec;
    std::filesystem::create_directories(request_dir, ec);
    if(ec) {
        logger.warn("rgb snapshot request directory unavailable path=" + request_dir.string() + " error=" + ec.message());
        return;
    }
    std::filesystem::directory_iterator it(request_dir, ec);
    if(ec) {
        logger.warn("rgb snapshot request directory scan failed path=" + request_dir.string() + " error=" + ec.message());
        return;
    }
    size_t processed = 0;
    for(const std::filesystem::directory_iterator end; it != end && processed < 16; it.increment(ec)) {
        if(ec) {
            logger.warn("rgb snapshot request directory iteration failed path=" + request_dir.string()
                        + " error=" + ec.message());
            return;
        }
        if(!it->is_regular_file(ec) || it->path().extension() != ".json") {
            continue;
        }
        process_rgb_snapshot_request_file(config, cameras, logger, it->path());
        ++processed;
    }
}

void expire_rgb_snapshot_requests(const AppConfig &config,
                                  const std::vector<std::unique_ptr<CameraRuntime>> &cameras,
                                  Logger &logger,
                                  std::chrono::steady_clock::time_point now) {
    for(const auto &camera : cameras) {
        if(!camera) {
            continue;
        }
        std::vector<RgbSnapshotRequest> expired;
        ReliableSnapshotQueue *queue = nullptr;
        {
            std::lock_guard<std::mutex> lock(camera->mutex);
            queue = camera->rgb_snapshot_queue;
            for(auto it = camera->pending_rgb_snapshots.begin(); it != camera->pending_rgb_snapshots.end();) {
                if(now - it->second.queued_at < kRgbSnapshotRequestTimeout) {
                    ++it;
                    continue;
                }
                expired.push_back(it->second.request);
                it = camera->pending_rgb_snapshots.erase(it);
            }
        }
        for(const auto &request : expired) {
            if(queue) {
                queue->cancel(request.request_id);
            }
            logger.warn("rgb snapshot request timed out request_id=" + request.request_id
                        + " camera_id=" + camera->config.camera_id);
            write_rgb_snapshot_result(config, camera->config.camera_id, request, false, "timeout", "",
                                      "receiver capture acknowledgement timeout", logger);
        }
    }
}

CameraRuntime *find_camera_by_id(const std::vector<std::unique_ptr<CameraRuntime>> &cameras, const std::string &camera_id) {
    for(const auto &camera : cameras) {
        if(camera && camera->config.camera_id == camera_id) {
            return camera.get();
        }
    }
    return nullptr;
}

void set_web_rgb_preview_active(CameraRuntime &camera, bool active, int lease_ms, Logger &logger) {
    const auto now = std::chrono::steady_clock::now();
    bool became_active = false;
    bool became_inactive = false;
    {
        std::lock_guard<std::mutex> lock(camera.mutex);
        const bool was_active = now <= camera.web_rgb_preview_requested_until;
        if(active) {
            const auto clamped_lease =
                std::chrono::milliseconds(std::clamp(lease_ms, 250, static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                                                         kWebRgbPreviewMaxLease)
                                                                                         .count())));
            camera.web_rgb_preview_requested_until = now + clamped_lease;
            if(camera.next_web_rgb_preview < now) {
                camera.next_web_rgb_preview = now;
            }
            became_active = !was_active;
        }
        else {
            camera.web_rgb_preview_requested_until = std::chrono::steady_clock::time_point::min();
            became_inactive = was_active;
        }
    }
    if(became_active) {
        logger.info("web rgb preview requested camera_id=" + camera.config.camera_id);
    }
    else if(became_inactive) {
        logger.info("web rgb preview request cleared camera_id=" + camera.config.camera_id);
    }
}

void handle_receiver_control_message(const AppConfig &config,
                                     const std::vector<std::unique_ptr<CameraRuntime>> &cameras,
                                     Logger &logger,
                                     const std::string &payload) {
    const auto root = parse_json_object(trim_copy(payload));
    if(!root) {
        return;
    }
    if(json_string_or(*root, "message_type") != "control") {
        return;
    }
    const std::string control = json_string_or(*root, "control");
    const std::string target_sender = json_string_or(*root, "sender_id");
    const std::string target_camera = json_string_or(*root, "camera_id");
    const std::string reason = json_string_or(*root, "reason");
    if(!target_sender.empty() && target_sender != "*" && target_sender != config.sender_id) {
        return;
    }

    if(control == "rgb_snapshot_result") {
        const std::string request_id = json_string_or(*root, "request_id");
        if(!is_safe_rgb_snapshot_request_id(request_id)) {
            return;
        }
        const bool ok = json_bool_or(*root, "ok", false);
        const std::string status = json_string_or(*root, "status", ok ? "captured" : "error");
        const std::string image_path = json_string_or(*root, "image_path");
        const std::string error = json_string_or(*root, "error");
        for(const auto &camera : cameras) {
            if(!camera || (!target_camera.empty() && target_camera != "*" && camera->config.camera_id != target_camera)) {
                continue;
            }
            std::optional<RgbSnapshotRequest> request;
            ReliableSnapshotQueue *queue = nullptr;
            {
                std::lock_guard<std::mutex> lock(camera->mutex);
                const auto pending = camera->pending_rgb_snapshots.find(request_id);
                if(pending == camera->pending_rgb_snapshots.end()) {
                    continue;
                }
                request = pending->second.request;
                camera->pending_rgb_snapshots.erase(pending);
                queue = camera->rgb_snapshot_queue;
            }
            if(queue) {
                queue->cancel(request_id);
            }
            logger.info("rgb snapshot result received request_id=" + request_id
                        + " camera_id=" + camera->config.camera_id
                        + " status=" + status
                        + " ok=" + (ok ? "true" : "false"));
            write_rgb_snapshot_result(config, camera->config.camera_id, *request, ok, status, image_path, error, logger);
            return;
        }
        return;
    }

    size_t matched = 0;
    const bool all_cameras = target_camera.empty() || target_camera == "*";
    if(control == "set_web_rgb_preview_active") {
        const bool active = json_bool_or(*root, "active", true);
        const int lease_ms = json_int_or(*root, "lease_ms", static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                             kWebRgbPreviewDefaultLease)
                                                             .count()));
        for(const auto &camera : cameras) {
            if(!camera) {
                continue;
            }
            if(all_cameras || camera->config.camera_id == target_camera) {
                set_web_rgb_preview_active(*camera, active, lease_ms, logger);
                ++matched;
            }
        }
        if(matched == 0) {
            logger.warn("web preview control matched no camera sender_id=" + config.sender_id + " camera_id=" + target_camera);
        }
        return;
    }

    if(control != "force_rgb_keyframe") {
        logger.warn("unknown receiver control ignored: " + control);
        return;
    }

    for(const auto &camera : cameras) {
        if(!camera) {
            continue;
        }
        if(all_cameras || camera->config.camera_id == target_camera) {
            request_rgb_keyframe(*camera, logger, reason.empty() ? "receiver_control" : reason);
            ++matched;
        }
    }
    if(matched == 0) {
        logger.warn("force_rgb_keyframe control matched no camera sender_id=" + config.sender_id + " camera_id=" + target_camera);
    }
}

template <typename Sender>
void process_receiver_controls(Sender &transport,
                               const AppConfig &config,
                               const std::vector<std::unique_ptr<CameraRuntime>> &cameras,
                               Logger &logger,
                               std::mutex &transport_mutex) {
    for(int i = 0; i < 16; ++i) {
        std::optional<std::string> payload;
        {
            std::lock_guard<std::mutex> lock(transport_mutex);
            payload = transport.receive_status_control(0);
        }
        if(!payload) {
            return;
        }
        handle_receiver_control_message(config, cameras, logger, *payload);
    }
}

bool aligned_rgb_preview_configured(const AppConfig &config) {
    if(!config.preview.aligned_rgb) {
        return false;
    }
    bool has_cam01 = false;
    bool has_cam02 = false;
    for(const auto &camera : config.cameras) {
        has_cam01 = has_cam01 || camera.camera_id == "cam01";
        has_cam02 = has_cam02 || camera.camera_id == "cam02";
    }
    return has_cam01 && has_cam02;
}

bool preview_aligned_rgb_pair(const std::vector<std::unique_ptr<CameraRuntime>> &cameras) {
    auto *cam01 = find_camera_by_id(cameras, "cam01");
    auto *cam02 = find_camera_by_id(cameras, "cam02");
    if(cam01 == nullptr || cam02 == nullptr) {
        return false;
    }

    static bool window_initialized = false;
    const auto pair = pop_aligned_rgb_preview_pair(*cam01, *cam02);
    if(!pair) {
        if(window_initialized) {
            cv::waitKey(1);
        }
        return false;
    }

    cv::Mat left;
    cv::Mat right;
    cv::resize(pair->cam01.bgr, left, cv::Size(640, 360));
    cv::resize(pair->cam02.bgr, right, cv::Size(640, 360));
    draw_rgb_preview_overlay(left, pair->cam01_label, pair->cam01, pair->cam01_minus_cam02_us, "cam01-cam02");
    draw_rgb_preview_overlay(right, pair->cam02_label, pair->cam02, -pair->cam01_minus_cam02_us, "cam02-cam01");

    cv::Mat wall;
    cv::hconcat(left, right, wall);
    cv::line(wall, cv::Point(640, 0), cv::Point(640, wall.rows), cv::Scalar(255, 255, 255), 1, cv::LINE_AA);
    const std::string window_name = "Gemini Sender RGB Aligned cam01 cam02";
    if(!window_initialized) {
        cv::namedWindow(window_name, cv::WINDOW_NORMAL);
        cv::resizeWindow(window_name, 1280, 360);
        cv::moveWindow(window_name, 80, 80);
        window_initialized = true;
    }
    cv::imshow(window_name, wall);
    cv::waitKey(1);
    return true;
}

std::string ob_error_text(const ob::Error &error) {
    const char *message = error.getMessage();
    return message ? message : "Orbbec SDK error";
}

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool camera_model_is_gemini305(const std::string &device_model) {
    return lower_copy(device_model).find("gemini 305") != std::string::npos;
}

int camera_publish_warmup_ms(const CameraConfig &config, const std::string &device_model) {
    if(config.publish_warmup_ms >= 0) {
        return config.publish_warmup_ms;
    }
    return camera_model_is_gemini305(device_model) ? 3000 : 0;
}

bool gemini305_manual_exposure_requested(const CameraConfig &config, const std::string &device_model) {
    const auto &controls = config.color_controls;
    return controls.auto_exposure && !*controls.auto_exposure
           && camera_model_is_gemini305(device_model);
}

bool ensure_gemini305_manual_exposure(const CameraConfig &config, const std::string &device_model,
                                      const std::string &device_serial, Logger &logger, bool force_reapply = false) {
    const auto &controls = config.color_controls;
    if(!gemini305_manual_exposure_requested(config, device_model)) {
        return true;
    }

    const auto video_device = find_v4l2_device_by_serial(device_serial);
    if(video_device.empty()) {
        logger.warn("Gemini305 manual exposure verification skipped camera_id=" + config.camera_id
                    + " reason=v4l2_device_not_found serial=" + device_serial);
        return false;
    }

    const int fd = open(video_device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if(fd < 0) {
        logger.warn("Gemini305 manual exposure verification skipped camera_id=" + config.camera_id
                    + " device=" + video_device + " error=" + std::strerror(errno));
        return false;
    }

    int control_errno = 0;
    auto get_control = [&](uint32_t id, int &value) {
        v4l2_control control{};
        control.id = id;
        if(checked_v4l2_ioctl(fd, VIDIOC_G_CTRL, &control) < 0) {
            control_errno = errno;
            return false;
        }
        value = control.value;
        return true;
    };
    auto set_control = [&](uint32_t id, int value) {
        v4l2_control control{};
        control.id = id;
        control.value = value;
        if(checked_v4l2_ioctl(fd, VIDIOC_S_CTRL, &control) < 0) {
            control_errno = errno;
            return false;
        }
        return true;
    };

    int exposure_mode = -1;
    const bool mode_readable = get_control(V4L2_CID_EXPOSURE_AUTO, exposure_mode);
    const bool applied_fallback = mode_readable && exposure_mode != V4L2_EXPOSURE_MANUAL;
    bool success = mode_readable;
    if(mode_readable && (applied_fallback || force_reapply)) {
        success = set_control(V4L2_CID_EXPOSURE_AUTO, V4L2_EXPOSURE_MANUAL);
        if(success && controls.exposure) {
            success = set_control(V4L2_CID_EXPOSURE_ABSOLUTE, *controls.exposure);
        }
        if(success && controls.gain) {
            success = set_control(V4L2_CID_GAIN, *controls.gain);
        }
    }

    int verified_mode = -1;
    const bool verified = success && get_control(V4L2_CID_EXPOSURE_AUTO, verified_mode)
                          && verified_mode == V4L2_EXPOSURE_MANUAL;
    close(fd);

    if(!verified) {
        logger.warn("Gemini305 manual exposure verification failed camera_id=" + config.camera_id
                    + " device=" + video_device + " sdk_mode=" + std::to_string(exposure_mode)
                    + " verified_mode=" + std::to_string(verified_mode) + " error="
                    + (control_errno ? std::strerror(control_errno) : "unexpected_mode"));
        return false;
    }

    logger.info("Gemini305 manual exposure verified camera_id=" + config.camera_id
                + " device=" + video_device + " mode=manual exposure="
                + (controls.exposure ? std::to_string(*controls.exposure) : "unchanged") + " gain="
                + (controls.gain ? std::to_string(*controls.gain) : "unchanged")
                + " fallback_applied=" + (applied_fallback ? "true" : "false")
                + " force_reapply=" + (force_reapply ? "true" : "false"));
    return true;
}

bool is_frame_sync_unsupported_error(const std::string &error) {
    const auto lower = lower_copy(error);
    return lower.find("frame sync") != std::string::npos
           && (lower.find("not support") != std::string::npos || lower.find("does not support") != std::string::npos
               || lower.find("unsupported") != std::string::npos);
}

OBFrameAggregateOutputMode frame_aggregate_mode_from_config(const std::string &mode) {
    if(mode == "full_frame_require") {
        return OB_FRAME_AGGREGATE_OUTPUT_FULL_FRAME_REQUIRE;
    }
    if(mode == "color_frame_require") {
        return OB_FRAME_AGGREGATE_OUTPUT_COLOR_FRAME_REQUIRE;
    }
    if(mode == "any_situation") {
        return OB_FRAME_AGGREGATE_OUTPUT_ANY_SITUATION;
    }
    return OB_FRAME_AGGREGATE_OUTPUT_DISABLE;
}

const char *frame_aggregate_mode_name(OBFrameAggregateOutputMode mode) {
    switch(mode) {
    case OB_FRAME_AGGREGATE_OUTPUT_FULL_FRAME_REQUIRE:
        return "full_frame_require";
    case OB_FRAME_AGGREGATE_OUTPUT_COLOR_FRAME_REQUIRE:
        return "color_frame_require";
    case OB_FRAME_AGGREGATE_OUTPUT_ANY_SITUATION:
        return "any_situation";
    case OB_FRAME_AGGREGATE_OUTPUT_DISABLE:
        return "disable";
    }
    return "unknown";
}

std::chrono::milliseconds reconnect_delay(uint32_t attempts) {
    const auto max_delay = static_cast<uint32_t>(std::max(1, camera_reconnect_max_delay_seconds()));
    return std::chrono::seconds(std::min<uint32_t>(max_delay, std::max<uint32_t>(1, attempts)));
}

void stop_camera(CameraRuntime &camera, Logger &logger) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    if(camera.pipeline) {
        try {
            camera.pipeline->stop();
        }
        catch(const ob::Error &e) {
            logger.warn("camera stop failed camera_id=" + camera.config.camera_id + " error=" + ob_error_text(e));
        }
        catch(const std::exception &e) {
            logger.warn("camera stop failed camera_id=" + camera.config.camera_id + " error=" + e.what());
        }
    }
    camera.encoder.reset();
    camera.web_preview_encoder.reset();
    camera.jpeg_dual_encoder.reset();
    camera.adaptive_exposure_controller.reset();
    camera.v4l2_capture.reset();
    camera.jpeg_dual_encoder_disabled = false;
    camera.jpeg_dual_no_main_output = 0;
    camera.web_preview_width = 0;
    camera.web_preview_height = 0;
    camera.pipeline.reset();
    camera.pipeline_config.reset();
    camera.color_profile.reset();
    camera.depth_profile.reset();
    camera.device.reset();
    camera.device_serial.clear();
    camera.device_uid.clear();
    camera.device_connection_type.clear();
    camera.online = false;
    camera.announced = false;
    camera.hardware_encoder = false;
    camera.live.adaptive_exposure_enabled = false;
    camera.rgb_sent_timing_seen = false;
    camera.rgb_last_sent_frame_id = 0;
    camera.rgb_last_sent_system_timestamp_us = 0;
    camera.rgb_transport_recovery.reset();
    camera.rgb_encode_timings.clear();
    camera.latest_bgr.release();
    camera.latest_depth_color.release();
}

void start_v4l2_camera_runtime(CameraRuntime &runtime, Logger &logger) {
    auto capture = std::make_shared<V4L2MjpegCapture>();
    capture->open_device(runtime.config);

    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(runtime.mutex);
        runtime.v4l2_capture = capture;
        runtime.device_model = runtime.config.device_model;
        runtime.device_serial = runtime.config.serial_number;
        runtime.device_uid = capture->device_path();
        runtime.device_connection_type = "v4l2";
        runtime.device.reset();
        runtime.pipeline.reset();
        runtime.pipeline_config.reset();
        runtime.color_profile.reset();
        runtime.depth_profile.reset();
        runtime.encoder.reset();
        runtime.web_preview_encoder.reset();
        runtime.jpeg_dual_encoder.reset();
        runtime.adaptive_exposure_controller.reset();
        runtime.jpeg_dual_encoder_disabled = false;
        runtime.jpeg_dual_no_main_output = 0;
        runtime.web_preview_width = 0;
        runtime.web_preview_height = 0;
        runtime.online = true;
        runtime.announced = false;
        runtime.hardware_encoder = false;
        runtime.depth_scale = 0.0f;
        runtime.last_error.clear();
        runtime.live = CameraLiveStats{};
        runtime.perf = CameraPerfStats{};
        runtime.perf.interval_started = now;
        runtime.next_preview = now;
        runtime.next_depth_emit = now;
        runtime.next_time_sync_log = now;
        runtime.next_jpeg_warning = now;
        runtime.next_media_warning = now;
        runtime.next_rgb_timing_warning = now;
        runtime.last_rgb_frame_at = now;
        runtime.last_depth_frame_at = now;
        runtime.next_capture_stall_reconnect = now;
        runtime.gemini305_manual_exposure_reapply_pending = false;
        runtime.gemini305_manual_exposure_reapply_at = std::chrono::steady_clock::time_point::min();
        runtime.capture_stall_samples = 0;
        runtime.last_media_warning.clear();
        runtime.rgb_sent_timing_seen = false;
        runtime.rgb_last_sent_frame_id = 0;
        runtime.rgb_last_sent_system_timestamp_us = 0;
        runtime.rgb_transport_recovery.reset();
        runtime.rgb_encode_timings.clear();
        runtime.latest_bgr.release();
        runtime.latest_depth_color.release();
    }

    std::ostringstream oss;
    oss << "v4l2 camera started camera_id=" << runtime.config.camera_id
        << " video_device=" << capture->device_path()
        << " color=" << capture->width() << "x" << capture->height() << "@" << capture->fps()
        << " format=mjpg configured_serial=" << runtime.config.serial_number
        << " depth=disabled";
    logger.info(oss.str());
}

void start_camera_runtime(CameraRuntime &runtime, Logger &logger) {
    if(runtime.config.capture_backend == "v4l2") {
        start_v4l2_camera_runtime(runtime, logger);
        return;
    }

    ob::Context::setLoggerSeverity(OB_LOG_SEVERITY_WARN);
    auto context = std::make_shared<ob::Context>();

    auto device = select_device(*context, runtime.config);
    auto device_info = device->getDeviceInfo();
    const std::string device_serial = device_info->serialNumber() ? device_info->serialNumber() : "";
    const std::string device_model = device_info->name() ? device_info->name() : "";
    const std::string device_uid = device_info->uid() ? device_info->uid() : "";
    const std::string device_connection_type = device_info->connectionType() ? device_info->connectionType() : "";

    CameraRuntime control_runtime;
    control_runtime.config = runtime.config;
    control_runtime.device = device;
    apply_stream_rotation(control_runtime, logger);

    auto start_pipeline = [&](OBFrameAggregateOutputMode aggregate_mode) {
        auto candidate_pipeline = std::make_unique<ob::Pipeline>(device);
        auto candidate_config = std::make_shared<ob::Config>();
        auto candidate_color_profile =
            select_profile(*candidate_pipeline, OB_SENSOR_COLOR, runtime.config.rgb_profile, OB_FORMAT_MJPG, logger);
        std::shared_ptr<ob::VideoStreamProfile> candidate_depth_profile;
        if(runtime.config.depth_profile.enabled) {
            candidate_depth_profile =
                select_profile(*candidate_pipeline, OB_SENSOR_DEPTH, runtime.config.depth_profile, OB_FORMAT_Y16, logger);
        }
        candidate_config->enableStream(candidate_color_profile);
        if(candidate_depth_profile) {
            candidate_config->enableStream(candidate_depth_profile);
        }
        if(aggregate_mode != OB_FRAME_AGGREGATE_OUTPUT_DISABLE && candidate_depth_profile) {
            candidate_config->setFrameAggregateOutputMode(aggregate_mode);
        }
        candidate_pipeline->start(candidate_config);
        return std::make_tuple(std::move(candidate_pipeline), std::move(candidate_config), std::move(candidate_color_profile),
                               std::move(candidate_depth_profile));
    };

    std::unique_ptr<ob::Pipeline> pipeline;
    std::shared_ptr<ob::Config> pipeline_config;
    std::shared_ptr<ob::VideoStreamProfile> color_profile;
    std::shared_ptr<ob::VideoStreamProfile> depth_profile;
    OBFrameAggregateOutputMode aggregate_mode = frame_aggregate_mode_from_config(runtime.config.frame_aggregate_mode);
    try {
        std::tie(pipeline, pipeline_config, color_profile, depth_profile) = start_pipeline(aggregate_mode);
    }
    catch(const ob::Error &e) {
        const auto error = ob_error_text(e);
        if(aggregate_mode == OB_FRAME_AGGREGATE_OUTPUT_DISABLE || !is_frame_sync_unsupported_error(error)) {
            throw;
        }
        aggregate_mode = OB_FRAME_AGGREGATE_OUTPUT_DISABLE;
        logger.warn("camera frame aggregate unsupported, retrying with aggregate disabled camera_id=" + runtime.config.camera_id
                    + " error=" + error);
        std::tie(pipeline, pipeline_config, color_profile, depth_profile) = start_pipeline(aggregate_mode);
    }

    apply_color_controls(control_runtime, logger);
    const bool gemini305_manual_exposure = gemini305_manual_exposure_requested(runtime.config, device_model);
    (void)ensure_gemini305_manual_exposure(runtime.config, device_model, device_serial, logger);

    const auto color_width = color_profile->width();
    const auto color_height = color_profile->height();
    const auto color_fps = color_profile->fps();
    const auto color_format = ob_format_name(color_profile->format());
    const auto depth_width = depth_profile ? depth_profile->width() : 0;
    const auto depth_height = depth_profile ? depth_profile->height() : 0;
    const auto depth_fps = depth_profile ? depth_profile->fps() : 0;
    const auto depth_format = depth_profile ? ob_format_name(depth_profile->format()) : "disabled";
    const auto now = std::chrono::steady_clock::now();
    const int publish_warmup_ms = camera_publish_warmup_ms(runtime.config, device_model);
    std::unique_ptr<AdaptiveExposureController> adaptive_exposure_controller;
    if(runtime.config.adaptive_exposure.enabled) {
        if(adaptive_exposure_max_for_model(device_model)) {
            adaptive_exposure_controller = std::make_unique<AdaptiveExposureController>(
                runtime.config.adaptive_exposure, *runtime.config.color_controls.exposure, *runtime.config.color_controls.gain);
        }
        else {
            logger.warn("adaptive exposure disabled camera_id=" + runtime.config.camera_id
                        + " reason=unsupported_device_model device_model=" + device_model);
        }
    }
    {
        std::lock_guard<std::mutex> lock(runtime.mutex);
        runtime.device = std::move(device);
        runtime.device_model = device_model;
        runtime.device_serial = device_serial;
        runtime.device_uid = device_uid;
        runtime.device_connection_type = device_connection_type;
        runtime.pipeline = std::move(pipeline);
        runtime.pipeline_config = std::move(pipeline_config);
        runtime.color_profile = std::move(color_profile);
        runtime.depth_profile = std::move(depth_profile);
        runtime.encoder.reset();
        runtime.web_preview_encoder.reset();
        runtime.jpeg_dual_encoder.reset();
        runtime.adaptive_exposure_controller = std::move(adaptive_exposure_controller);
        runtime.jpeg_dual_encoder_disabled = false;
        runtime.jpeg_dual_no_main_output = 0;
        runtime.web_preview_width = 0;
        runtime.web_preview_height = 0;
        runtime.online = true;
        runtime.announced = false;
        runtime.hardware_encoder = false;
        runtime.depth_scale = 0.0f;
        runtime.last_error.clear();
        runtime.live = CameraLiveStats{};
        runtime.live.adaptive_exposure_enabled = runtime.adaptive_exposure_controller != nullptr;
        if(runtime.adaptive_exposure_controller) {
            runtime.live.adaptive_requested_exposure = runtime.adaptive_exposure_controller->exposure();
            runtime.live.adaptive_requested_gain = runtime.adaptive_exposure_controller->gain();
            runtime.live.adaptive_last_reason = "initialized";
        }
        runtime.perf = CameraPerfStats{};
        runtime.perf.interval_started = now;
        runtime.next_preview = now;
        runtime.next_depth_emit = now;
        runtime.next_time_sync_log = now;
        runtime.next_jpeg_warning = now;
        runtime.next_media_warning = now;
        runtime.next_keyframe_guard_warning = now;
        runtime.next_rgb_timing_warning = now;
        runtime.last_rgb_frame_at = now;
        runtime.last_depth_frame_at = now;
        runtime.next_capture_stall_reconnect = now;
        runtime.gemini305_manual_exposure_reapply_pending = gemini305_manual_exposure;
        runtime.next_adaptive_exposure_sample = now;
        runtime.next_adaptive_exposure_warning = now;
        runtime.gemini305_manual_exposure_reapply_at = runtime.gemini305_manual_exposure_reapply_pending
                                                          ? now + std::chrono::seconds(2)
                                                          : std::chrono::steady_clock::time_point::min();
        runtime.publish_warmup_active = publish_warmup_ms > 0;
        runtime.publish_warmup_exposure_verified = !gemini305_manual_exposure;
        runtime.publish_warmup_dropped_framesets = 0;
        runtime.publish_not_before = runtime.publish_warmup_active
                                         ? now + std::chrono::milliseconds(publish_warmup_ms)
                                         : std::chrono::steady_clock::time_point::min();
        runtime.publish_warmup_deadline = runtime.publish_warmup_active
                                             ? runtime.publish_not_before + std::chrono::seconds(5)
                                             : std::chrono::steady_clock::time_point::min();
        runtime.capture_stall_samples = 0;
        runtime.last_media_warning.clear();
        runtime.rgb_sent_timing_seen = false;
        runtime.rgb_last_sent_frame_id = 0;
        runtime.rgb_last_sent_system_timestamp_us = 0;
        runtime.rgb_transport_recovery.reset();
        runtime.rgb_encode_timings.clear();
        runtime.latest_bgr.release();
        runtime.latest_depth_color.release();
    }

    std::ostringstream oss;
    oss << "camera started camera_id=" << runtime.config.camera_id << " color=" << color_width << "x"
        << color_height << "@" << color_fps << " format=" << color_format
        << " depth=" << depth_width << "x" << depth_height << "@" << depth_fps
        << " format=" << depth_format
        << " configured_model=" << runtime.config.device_model
        << " device_model=" << device_model
        << " configured_serial=" << runtime.config.serial_number
        << " configured_uid=" << runtime.config.uid
        << " device_serial=" << device_serial
        << " device_uid=" << device_uid
        << " paired_rgb_serial=" << paired_rgb_serial_for_depth_uid(device_uid)
        << " connection=" << device_connection_type
        << " aggregate_mode=" << frame_aggregate_mode_name(aggregate_mode)
        << " publish_warmup_ms=" << publish_warmup_ms
        << " exposure_verification=" << (gemini305_manual_exposure ? "required" : "not_required")
        << " adaptive_exposure=" << (runtime.config.adaptive_exposure.enabled ? "enabled" : "disabled");
    logger.info(oss.str());
}

std::vector<std::unique_ptr<CameraRuntime>> start_cameras(const AppConfig &config, Logger &logger) {
    std::vector<std::unique_ptr<CameraRuntime>> cameras;
    const auto now = std::chrono::steady_clock::now();
    size_t index = 0;

    for(const auto &camera_config : config.cameras) {
        auto runtime = std::make_unique<CameraRuntime>();
        runtime->config = camera_config;
        runtime->online = false;
        runtime->last_error = "camera startup pending";
        runtime->next_reconnect = now + std::chrono::milliseconds(static_cast<int>(index) * 1500);
        logger.info("camera startup scheduled camera_id=" + runtime->config.camera_id
                    + " delay_ms=" + std::to_string(static_cast<int>(index) * 1500));
        cameras.push_back(std::move(runtime));
        ++index;
    }
    return cameras;
}

template <typename Sender>
bool send_status_locked(Sender &sender, Logger &logger, std::mutex &transport_mutex, const Json::Value &message) {
    std::lock_guard<std::mutex> lock(transport_mutex);
    return send_status(sender, logger, message);
}

MediaPacketJob make_owned_media_job(CameraRuntime &camera, StreamType stream_type, const MediaFrameMeta &meta,
                                    std::vector<uint8_t> &&payload) {
    MediaPacketJob job;
    job.camera = &camera;
    job.stream_type = stream_type;
    job.header = build_media_header(meta);
    job.owned_payload = std::move(payload);
    job.rgb_keyframe = stream_type == StreamType::rgb && (meta.flags & key_frame) != 0u;
    return job;
}

MediaPacketJob make_external_media_job(CameraRuntime &camera, StreamType stream_type, const MediaFrameMeta &meta, const void *payload,
                                       size_t payload_size, std::shared_ptr<const void> payload_owner) {
    MediaPacketJob job;
    job.camera = &camera;
    job.stream_type = stream_type;
    job.header = build_media_header(meta);
    job.external_payload = static_cast<const uint8_t *>(payload);
    job.external_payload_size = payload_size;
    job.payload_owner = std::move(payload_owner);
    job.rgb_keyframe = stream_type == StreamType::rgb && (meta.flags & key_frame) != 0u;
    return job;
}

std::optional<RgbSnapshotRequest> take_next_rgb_snapshot_request(CameraRuntime &camera, uint64_t capture_host_timestamp_us) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    if(camera.rgb_snapshot_requests.empty()) {
        return std::nullopt;
    }
    if(camera.rgb_snapshot_requests.front().capture_not_before_unix_us > capture_host_timestamp_us) {
        return std::nullopt;
    }
    RgbSnapshotRequest request = std::move(camera.rgb_snapshot_requests.front());
    camera.rgb_snapshot_requests.pop_front();
    return request;
}

void reject_next_rgb_snapshot_request(const AppConfig &config, CameraRuntime &camera, const std::string &error, Logger &logger) {
    auto request = take_next_rgb_snapshot_request(camera, now_us());
    if(!request) {
        return;
    }
    logger.warn("rgb snapshot request failed request_id=" + request->request_id
                + " camera_id=" + camera.config.camera_id + " error=" + error);
    write_rgb_snapshot_result(config, camera.config.camera_id, *request, false, "error", "", error, logger);
}

bool rotate_rgb_snapshot_mjpeg_180(const std::vector<uint8_t> &jpeg,
                                   std::vector<uint8_t> &rotated_jpeg,
                                   std::string &error) {
    try {
        if(jpeg.empty() || jpeg.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
            error = "invalid MJPEG size";
            return false;
        }
        cv::Mat encoded(1, static_cast<int>(jpeg.size()), CV_8UC1, const_cast<uint8_t *>(jpeg.data()));
        cv::Mat decoded = cv::imdecode(encoded, cv::IMREAD_COLOR);
        if(decoded.empty()) {
            error = "MJPEG decode failed";
            return false;
        }
        cv::Mat rotated;
        cv::rotate(decoded, rotated, cv::ROTATE_180);
        const std::vector<int> parameters{cv::IMWRITE_JPEG_QUALITY, kRgbSnapshotJpegQuality};
        if(!cv::imencode(".jpg", rotated, rotated_jpeg, parameters)
           || !mjpg_has_complete_jpeg_data(rotated_jpeg.data(), rotated_jpeg.size())) {
            error = "rotated JPEG encode failed";
            return false;
        }
        return true;
    }
    catch(const cv::Exception &e) {
        error = e.what();
        return false;
    }
    catch(const std::exception &e) {
        error = e.what();
        return false;
    }
}

void publish_rgb_snapshot_frame(const AppConfig &config,
                                CameraRuntime &camera,
                                const uint8_t *jpeg_data,
                                size_t jpeg_size,
                                const RgbEncodeTiming &timing,
                                Logger &logger) {
    const uint64_t capture_host_timestamp_us =
        timing.capture_host_timestamp_us > 0 ? timing.capture_host_timestamp_us : now_us();
    auto request = take_next_rgb_snapshot_request(camera, capture_host_timestamp_us);
    if(!request) {
        return;
    }
    const size_t complete_jpeg_size = mjpg_complete_jpeg_size(jpeg_data, jpeg_size);
    if(complete_jpeg_size == 0) {
        const std::string error = "captured RGB frame is not a complete MJPEG image";
        logger.warn("rgb snapshot request failed request_id=" + request->request_id
                    + " camera_id=" + camera.config.camera_id + " error=" + error);
        write_rgb_snapshot_result(config, camera.config.camera_id, *request, false, "error", "", error, logger);
        return;
    }
    jpeg_size = complete_jpeg_size;

    ReliableSnapshotQueue *queue = nullptr;
    {
        std::lock_guard<std::mutex> lock(camera.mutex);
        queue = camera.rgb_snapshot_queue;
        if(queue) {
            camera.pending_rgb_snapshots.emplace(
                request->request_id, PendingRgbSnapshot{*request, std::chrono::steady_clock::now()});
        }
    }
    if(queue == nullptr) {
        write_rgb_snapshot_result(config, camera.config.camera_id, *request, false, "error", "",
                                  "snapshot media transport is unavailable", logger);
        return;
    }

    MediaFrameMeta meta;
    meta.stream_type = StreamType::rgb_snapshot;
    meta.flags = has_system_timestamp | has_rgb_diagnostics | has_pipeline_diagnostics;
    meta.sender_id = config.sender_id;
    meta.camera_id = camera.config.camera_id;
    meta.codec_or_compression = std::string(kRgbSnapshotCodecPrefix) + request->request_id;
    meta.frame_id = timing.frame_id;
    meta.timestamp_us = timing.device_timestamp_us;
    meta.system_timestamp_us = timing.system_timestamp_us;
    meta.pair_id = timing.pair_id;
    meta.width = timing.width;
    meta.height = timing.height;
    meta.pixel_format = PixelFormat::encoded_video;
    meta.payload_size = jpeg_size;
    meta.uncompressed_size = jpeg_size;
    meta.rgb_exposure_us = timing.diagnostics.exposure_us;
    meta.rgb_gain = timing.diagnostics.gain;
    meta.rgb_auto_exposure = timing.diagnostics.auto_exposure;
    meta.rgb_actual_fps = timing.diagnostics.actual_fps;
    meta.sender_capture_host_timestamp_us = timing.capture_host_timestamp_us;
    meta.sender_timing_bound_timestamp_us = timing.timing_bound_timestamp_us;
    meta.sender_packet_queued_timestamp_us = now_us();
    const int snapshot_rotation_degrees = software_rgb_rotation_degrees(camera.config);
    if(snapshot_rotation_degrees == 180) {
        meta.flags |= snapshot_orientation_applied;
    }

    std::vector<uint8_t> jpeg(jpeg_data, jpeg_data + jpeg_size);
    ReliableSnapshotJob job;
    job.media = make_owned_media_job(camera, StreamType::rgb_snapshot, meta, std::move(jpeg));
    job.meta = meta;
    job.request_id = request->request_id;
    job.rotation_degrees = snapshot_rotation_degrees;
    job.first_queued_at = std::chrono::steady_clock::now();
    job.next_attempt_at = job.first_queued_at;
    if(!queue->publish(std::move(job))) {
        {
            std::lock_guard<std::mutex> lock(camera.mutex);
            camera.pending_rgb_snapshots.erase(request->request_id);
        }
        const std::string error = "snapshot reliable media queue is full";
        logger.warn("rgb snapshot request failed request_id=" + request->request_id
                    + " camera_id=" + camera.config.camera_id + " error=" + error);
        write_rgb_snapshot_result(config, camera.config.camera_id, *request, false, "error", "", error, logger);
        return;
    }
    logger.info("rgb snapshot frame queued request_id=" + request->request_id
                + " camera_id=" + camera.config.camera_id
                + " frame_id=" + std::to_string(timing.frame_id)
                + " system_timestamp_us=" + std::to_string(timing.system_timestamp_us)
                + " jpeg_bytes=" + std::to_string(jpeg_size));
}

bool publish_media_job(LatestMediaQueue &media_queue, size_t slot_index, CameraRuntime &camera, StreamType stream_type,
                       MediaPacketJob &&job, Logger &logger, bool reject_if_occupied = false, bool packet_is_keyframe = false) {
    const auto result = media_queue.publish(slot_index, std::move(job), reject_if_occupied);
    if(result == LatestMediaQueue::PublishResult::queued) {
        return true;
    }
    if(result == LatestMediaQueue::PublishResult::overwritten || result == LatestMediaQueue::PublishResult::rejected_occupied) {
        record_queue_overwrite(camera, stream_type);
        if(stream_type == StreamType::rgb && !(result == LatestMediaQueue::PublishResult::overwritten && packet_is_keyframe)) {
            arm_rgb_keyframe_guard(camera, logger, "rgb queue overwrite; waiting for next keyframe");
        }
    }
    return result == LatestMediaQueue::PublishResult::overwritten;
}

template <typename Sender>
void mark_camera_disconnected(const AppConfig &config, CameraRuntime &camera, Sender &transport, Logger &logger,
                              std::mutex &transport_mutex, const std::string &error) {
    const bool should_reconnect = camera_reconnect_enabled(camera);
    const auto disconnected_at = std::chrono::steady_clock::now();
    const int flap_restart_events = camera_flap_restart_events();
    const int flap_window_seconds = camera_flap_window_seconds();
    size_t recent_disconnects = 0;
    bool restart_for_flapping = false;
    {
        std::lock_guard<std::mutex> lock(camera.mutex);
        camera.disconnects++;
        camera.reconnect_attempts = 0;
        camera.last_error = error;
        const auto oldest_allowed = disconnected_at - std::chrono::seconds(flap_window_seconds);
        while(!camera.recent_disconnects.empty() && camera.recent_disconnects.front() < oldest_allowed) {
            camera.recent_disconnects.pop_front();
        }
        camera.recent_disconnects.push_back(disconnected_at);
        recent_disconnects = camera.recent_disconnects.size();
        restart_for_flapping = flap_restart_events > 0 && recent_disconnects >= static_cast<size_t>(flap_restart_events);
    }
    logger.error("camera disconnected camera_id=" + camera.config.camera_id + " error=" + error);
    if(restart_for_flapping) {
        logger.error("camera flap guard exiting sender for watchdog restart camera_id=" + camera.config.camera_id
                     + " events=" + std::to_string(recent_disconnects)
                     + " threshold=" + std::to_string(flap_restart_events)
                     + " window_s=" + std::to_string(flap_window_seconds));
        std::_Exit(75);
    }
    send_status_locked(transport, logger, transport_mutex, camera_offline_message(config, camera.config.camera_id, error));
    stop_camera(camera, logger);
    if(should_reconnect) {
        set_camera_reconnect_delay(camera, std::chrono::steady_clock::now() + std::chrono::seconds(1));
    }
    else {
        logger.info("hotplug camera retired camera_id=" + camera.config.camera_id + " reason=" + error);
    }
}

template <typename Sender>
void retry_camera_reconnect(const AppConfig &config, CameraRuntime &camera, Sender &transport, Logger &logger,
                            std::mutex &transport_mutex, std::chrono::steady_clock::time_point now) {
    uint32_t attempt = 0;
    {
        std::lock_guard<std::mutex> lock(camera.mutex);
        if(now < camera.next_reconnect) {
            return;
        }
        camera.reconnect_attempts++;
        attempt = camera.reconnect_attempts;
    }

    logger.warn("camera reconnect attempt camera_id=" + camera.config.camera_id + " attempt=" + std::to_string(attempt));
    try {
        start_camera_runtime(camera, logger);
        {
            std::lock_guard<std::mutex> lock(camera.mutex);
            camera.reconnect_attempts = 0;
        }
        logger.info("camera reconnected camera_id=" + camera.config.camera_id);
        send_status_locked(transport, logger, transport_mutex,
                           event_message(config, "info", "camera_reconnected", "camera pipeline restarted", camera.config.camera_id));
    }
    catch(const ob::Error &e) {
        stop_camera(camera, logger);
        const std::string error = ob_error_text(e);
        uint32_t attempts = 0;
        {
            std::lock_guard<std::mutex> lock(camera.mutex);
            camera.last_error = error;
            attempts = camera.reconnect_attempts;
            camera.next_reconnect = now + reconnect_delay(camera.reconnect_attempts);
        }
        logger.warn("camera reconnect failed camera_id=" + camera.config.camera_id + " attempt=" + std::to_string(attempts)
                    + " error=" + error);
        send_status_locked(transport, logger, transport_mutex, camera_offline_message(config, camera.config.camera_id, error));
        update_camera_reconnect_process_guard(camera, logger, attempts, error);
    }
    catch(const std::exception &e) {
        stop_camera(camera, logger);
        const std::string error = e.what();
        uint32_t attempts = 0;
        {
            std::lock_guard<std::mutex> lock(camera.mutex);
            camera.last_error = error;
            attempts = camera.reconnect_attempts;
            camera.next_reconnect = now + reconnect_delay(camera.reconnect_attempts);
        }
        logger.warn("camera reconnect failed camera_id=" + camera.config.camera_id + " attempt=" + std::to_string(attempts)
                    + " error=" + error);
        send_status_locked(transport, logger, transport_mutex, camera_offline_message(config, camera.config.camera_id, error));
        update_camera_reconnect_process_guard(camera, logger, attempts, error);
    }
}

template <typename Sender>
void media_sender_loop(LatestMediaQueue &media_queue, Sender &transport, Logger &logger, std::mutex &transport_mutex,
                       const std::atomic<bool> *path_running = nullptr) {
    auto next_idle_close_log = std::chrono::steady_clock::now();
    std::optional<MediaPacketJob> retry_job;
    std::optional<std::chrono::steady_clock::time_point> drain_deadline;
    while(true) {
        const bool producers_active = path_running == nullptr ? g_running.load() : path_running->load();
        if(!producers_active && !drain_deadline) {
            drain_deadline = std::chrono::steady_clock::now() + kGracefulQueueDrainTimeout;
        }
        if(!producers_active && !retry_job && media_queue.empty()) {
            break;
        }
        if(drain_deadline && std::chrono::steady_clock::now() >= *drain_deadline) {
            logger.warn("media sender queue drain timed out; remaining packets discarded");
            break;
        }
        std::optional<MediaPacketJob> job;
        if(retry_job) {
            job.emplace(std::move(*retry_job));
            retry_job.reset();
        }
        else {
            job = media_queue.wait_pop(std::chrono::milliseconds(100));
        }
        if(!job || !job->camera) {
            bool closed = false;
            {
                std::lock_guard<std::mutex> lock(transport_mutex);
                closed = transport.close_if_media_peer_closed();
            }
            if(closed) {
                const auto now = std::chrono::steady_clock::now();
                if(now >= next_idle_close_log) {
                    logger.info("media TCP idle peer close detected; socket will reconnect on next packet");
                    next_idle_close_log = now + std::chrono::seconds(5);
                }
            }
            continue;
        }

        if(job->stream_type == StreamType::rgb
           && decide_rgb_keyframe_send(*job->camera, job->rgb_keyframe, std::chrono::steady_clock::now(), logger)
                  == RgbTransportRecovery::SendDecision::drop) {
            continue;
        }

        const auto send_started = std::chrono::steady_clock::now();
        bool sent = false;
        std::string error;
        const auto packet = job->view();
        try {
            std::lock_guard<std::mutex> lock(transport_mutex);
            sent = transport.send_media(packet);
            if(!sent) {
                error = transport.last_error();
            }
        }
        catch(const std::exception &e) {
            error = e.what();
        }
        catch(...) {
            error = "unknown media transport exception";
        }
        const auto send_ended = std::chrono::steady_clock::now();
        const double send_ms = elapsed_ms(send_started, send_ended);
        if(sent) {
            record_media_send_success(*job->camera, job->stream_type, job->total_size(), send_ms);
            if(job->stream_type == StreamType::rgb) {
                complete_rgb_keyframe_recovery(*job->camera, logger, job->rgb_keyframe);
            }
        }
        else {
            record_media_send_failure(*job->camera, logger, job->stream_type, send_ms, send_ended, error);
            if(job->stream_type == StreamType::rgb) {
                arm_rgb_keyframe_guard(*job->camera, logger, error);
                maybe_request_rgb_recovery_keyframe(*job->camera, logger, send_ended);
                std::lock_guard<std::mutex> lock(job->camera->mutex);
                job->camera->rgb_dropped++;
                job->camera->rgb_transport_retry_drops++;
            }
            if(job->stream_type == StreamType::depth_raw
               && (!drain_deadline || std::chrono::steady_clock::now() < *drain_deadline)) {
                retry_job = std::move(*job);
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
    }
}

void fail_pending_rgb_snapshot(const AppConfig &config,
                               CameraRuntime &camera,
                               const std::string &request_id,
                               const std::string &error,
                               Logger &logger) {
    std::optional<RgbSnapshotRequest> request;
    {
        std::lock_guard<std::mutex> lock(camera.mutex);
        const auto pending = camera.pending_rgb_snapshots.find(request_id);
        if(pending == camera.pending_rgb_snapshots.end()) {
            return;
        }
        request = pending->second.request;
        camera.pending_rgb_snapshots.erase(pending);
    }
    logger.warn("rgb snapshot transport failed request_id=" + request_id
                + " camera_id=" + camera.config.camera_id + " error=" + error);
    write_rgb_snapshot_result(config, camera.config.camera_id, *request, false, "error", "", error, logger);
}

template <typename Sender>
void rgb_snapshot_sender_loop(const AppConfig &config,
                              ReliableSnapshotQueue &queue,
                              Sender &transport,
                              Logger &logger,
                              std::atomic<bool> &running) {
    while(running.load() && g_running.load()) {
        auto job = queue.wait_pop(std::chrono::milliseconds(100));
        if(!job) {
            continue;
        }
        if(!job->media.camera) {
            continue;
        }
        const auto now = std::chrono::steady_clock::now();
        if(now - job->first_queued_at >= kRgbSnapshotRequestTimeout) {
            fail_pending_rgb_snapshot(config, *job->media.camera, job->request_id,
                                      "receiver capture acknowledgement timeout", logger);
            continue;
        }
        if(job->rotation_degrees == 180) {
            const auto rotation_started = std::chrono::steady_clock::now();
            std::vector<uint8_t> rotated_jpeg;
            std::string rotation_error;
            if(!rotate_rgb_snapshot_mjpeg_180(job->media.owned_payload, rotated_jpeg, rotation_error)) {
                fail_pending_rgb_snapshot(config, *job->media.camera, job->request_id,
                                          "snapshot rotation failed: " + rotation_error, logger);
                continue;
            }
            job->media.owned_payload = std::move(rotated_jpeg);
            job->meta.payload_size = job->media.owned_payload.size();
            job->meta.uncompressed_size = job->media.owned_payload.size();
            job->media.header = build_media_header(job->meta);
            job->rotation_degrees = 0;
            logger.info("rgb snapshot orientation applied request_id=" + job->request_id
                        + " camera_id=" + job->media.camera->config.camera_id
                        + " degrees=180"
                        + " jpeg_bytes=" + std::to_string(job->media.owned_payload.size())
                        + " elapsed_ms=" + std::to_string(
                              elapsed_ms(rotation_started, std::chrono::steady_clock::now())));
        }

        bool sent = false;
        std::string error;
        try {
            sent = transport.send_media(job->media.view());
            if(!sent) {
                error = transport.last_error();
            }
        }
        catch(const std::exception &e) {
            error = e.what();
        }
        catch(...) {
            error = "unknown snapshot media transport exception";
        }
        ++job->attempts;
        if(sent && job->attempts == 1) {
            logger.info("rgb snapshot media sent request_id=" + job->request_id
                        + " camera_id=" + job->media.camera->config.camera_id
                        + " bytes=" + std::to_string(job->media.total_size()));
        }
        else if(!sent && (job->attempts <= 3 || job->attempts % 10 == 0)) {
            logger.warn("rgb snapshot media retry request_id=" + job->request_id
                        + " camera_id=" + job->media.camera->config.camera_id
                        + " attempt=" + std::to_string(job->attempts)
                        + " error=" + (error.empty() ? "send failed" : error));
        }

        CameraRuntime *camera = job->media.camera;
        const std::string request_id = job->request_id;
        if(!queue.requeue(std::move(*job), std::chrono::steady_clock::now() + kRgbSnapshotRetryInterval)
           && running.load() && g_running.load()) {
            fail_pending_rgb_snapshot(config, *camera, request_id, "snapshot retry queue unavailable", logger);
        }
    }
}

void depth_compression_loop(LatestDepthCompressionQueue &depth_queue, Logger &logger, size_t worker_index) {
    std::optional<std::chrono::steady_clock::time_point> drain_deadline;
    while(true) {
        const bool producers_active = !depth_queue.stopping();
        if(!producers_active && !drain_deadline) {
            drain_deadline = std::chrono::steady_clock::now() + kGracefulQueueDrainTimeout;
        }
        if(!producers_active && depth_queue.empty()) {
            break;
        }
        if(drain_deadline && std::chrono::steady_clock::now() >= *drain_deadline) {
            logger.warn("depth compression queue drain timed out worker=" + std::to_string(worker_index));
            break;
        }
        auto work = depth_queue.wait_pop(std::chrono::milliseconds(100));
        if(!work) {
            continue;
        }
        auto &job = work->job;
        if(!job.source_camera || !job.output_camera || !job.output_media_queue) {
            depth_queue.complete(work->slot_index);
            continue;
        }

        try {
            const auto compress_started = std::chrono::steady_clock::now();
            auto compressed_depth =
                compress_depth_payload(job.meta.codec_or_compression, job.raw_payload, job.raw_payload_size, job.depth_scale,
                                       job.quantization_step_mm);
            record_depth_compress_ms(*job.source_camera, elapsed_ms(compress_started, std::chrono::steady_clock::now()));
            job.meta.payload_size = compressed_depth.size();
            job.meta.uncompressed_size = job.raw_payload_size;
            auto media_job = make_owned_media_job(*job.output_camera, StreamType::depth_raw, job.meta, std::move(compressed_depth));
            publish_media_job(*job.output_media_queue, job.media_slot_index, *job.output_camera, StreamType::depth_raw, std::move(media_job),
                              logger);
            record_depth_frame_done(*job.output_camera);
        }
        catch(const std::exception &e) {
            set_camera_last_error(*job.source_camera, e.what());
            logger.warn("depth compression failed worker=" + std::to_string(worker_index)
                        + " camera_id=" + job.source_camera->config.camera_id + " error=" + e.what());
        }
        catch(...) {
            set_camera_last_error(*job.source_camera, "unknown depth compression exception");
            logger.warn("depth compression failed worker=" + std::to_string(worker_index)
                        + " camera_id=" + job.source_camera->config.camera_id + " error=unknown exception");
        }
        depth_queue.complete(work->slot_index);
    }
}

void publish_rgb_encoded_units(const AppConfig &config, CameraRuntime &camera, LatestMediaQueue &main_media_queue, size_t rgb_slot,
                               std::vector<EncodedH264Frame> &encoded_units, RgbEncodeTiming submitted_timing,
                               uint64_t encode_done_timestamp_us, std::chrono::steady_clock::time_point frame_now, Logger &logger) {
    for(auto &encoded : encoded_units) {
        const bool encoded_has_vcl = h264_payload_has_vcl_nal(encoded.data);
        const bool is_key_frame = h264_payload_has_idr(encoded.data);
        if(is_key_frame) {
            report_forced_rgb_keyframe(camera, logger);
        }
        const auto send_decision = decide_rgb_keyframe_send(camera, is_key_frame, frame_now, logger);
        if(send_decision == RgbTransportRecovery::SendDecision::drop) {
            continue;
        }
        const auto timing_resolution = resolve_rgb_encode_timing(camera, encoded, submitted_timing, encoded_has_vcl);
        maybe_log_rgb_timing_resolution(camera, logger, encoded, timing_resolution, frame_now);
        auto timing = timing_resolution.timing;
        if(timing.encode_start_timestamp_us == 0) {
            timing.encode_start_timestamp_us = submitted_timing.encode_start_timestamp_us;
        }
        timing.encode_done_timestamp_us = encode_done_timestamp_us;
        if(encoded_has_vcl && rgb_encoder_output_lag_too_large(camera, logger, timing, encode_done_timestamp_us, frame_now)) {
            return;
        }
        if(encoded_has_vcl && !rgb_encoded_timing_is_monotonic(camera, logger, timing, frame_now)) {
            continue;
        }
        MediaFrameMeta meta;
        meta.stream_type = StreamType::rgb;
        meta.flags = has_system_timestamp | has_rgb_diagnostics | (is_key_frame ? key_frame : 0u);
        if(send_decision == RgbTransportRecovery::SendDecision::send_recovery_keyframe) {
            meta.flags |= dropped_before;
        }
        meta.sender_id = config.sender_id;
        meta.camera_id = camera.config.camera_id;
        meta.codec_or_compression = "h264";
        meta.frame_id = timing.frame_id;
        meta.timestamp_us = timing.device_timestamp_us;
        meta.system_timestamp_us = timing.system_timestamp_us;
        meta.pair_id = timing.pair_id;
        meta.width = timing.width;
        meta.height = timing.height;
        meta.pixel_format = PixelFormat::encoded_video;
        meta.payload_size = encoded.data.size();
        meta.uncompressed_size = encoded.data.size();
        meta.rgb_exposure_us = timing.diagnostics.exposure_us;
        meta.rgb_gain = timing.diagnostics.gain;
        meta.rgb_auto_exposure = timing.diagnostics.auto_exposure;
        meta.rgb_actual_fps = timing.diagnostics.actual_fps;
        timing.packet_queued_timestamp_us = now_us();
        set_rgb_pipeline_diagnostics(meta, timing);
        const bool reject_if_occupied = !config.recording_buffer.enabled && !is_key_frame;
        auto job = make_owned_media_job(camera, StreamType::rgb, meta, std::move(encoded.data));
        if(publish_media_job(main_media_queue, rgb_slot, camera, StreamType::rgb, std::move(job), logger, reject_if_occupied, is_key_frame)
           && encoded_has_vcl) {
            mark_rgb_encoded_timing_queued(camera, timing);
        }
    }
}

void publish_rgb_preview_units(const AppConfig &config, CameraRuntime &camera, LatestMediaQueue &preview_media_queue, size_t rgb_preview_slot,
                               std::vector<EncodedH264Frame> &preview_units, RgbEncodeTiming preview_timing, uint32_t preview_width,
                               uint32_t preview_height, Logger &logger) {
    for(auto &encoded : preview_units) {
        MediaFrameMeta meta;
        const bool is_key_frame = h264_payload_has_idr(encoded.data);
        meta.stream_type = StreamType::rgb_preview;
        meta.flags = has_system_timestamp | has_rgb_diagnostics | (is_key_frame ? key_frame : 0u);
        meta.sender_id = config.sender_id;
        meta.camera_id = camera.config.camera_id;
        meta.codec_or_compression = "h264";
        meta.frame_id = preview_timing.frame_id;
        meta.timestamp_us = preview_timing.device_timestamp_us;
        meta.system_timestamp_us = preview_timing.system_timestamp_us;
        meta.pair_id = preview_timing.pair_id;
        meta.width = preview_width;
        meta.height = preview_height;
        meta.pixel_format = PixelFormat::encoded_video;
        meta.payload_size = encoded.data.size();
        meta.uncompressed_size = encoded.data.size();
        meta.rgb_exposure_us = preview_timing.diagnostics.exposure_us;
        meta.rgb_gain = preview_timing.diagnostics.gain;
        meta.rgb_auto_exposure = preview_timing.diagnostics.auto_exposure;
        meta.rgb_actual_fps = preview_timing.diagnostics.actual_fps;
        preview_timing.packet_queued_timestamp_us = now_us();
        set_rgb_pipeline_diagnostics(meta, preview_timing);
        auto job = make_owned_media_job(camera, StreamType::rgb_preview, meta, std::move(encoded.data));
        publish_media_job(preview_media_queue, rgb_preview_slot, camera, StreamType::rgb_preview, std::move(job), logger);
    }
}

size_t depth_compression_worker_count(size_t camera_count) {
    if(camera_count <= 1) {
        return 1;
    }
    const unsigned int hardware_threads = std::thread::hardware_concurrency();
    const size_t hardware_limit = hardware_threads == 0 ? camera_count : static_cast<size_t>(hardware_threads);
    return std::max<size_t>(1, std::min(camera_count, hardware_limit));
}

std::mutex g_encoder_create_mutex;

CameraRuntime *depth_target_camera(const AppConfig &config, CameraRuntime &source) {
    if(!config.swap_depth_between_cameras || !camera_is_online(source) || source.depth_remap_target == nullptr
       || !camera_is_online(*source.depth_remap_target)) {
        return &source;
    }
    return source.depth_remap_target;
}

void configure_depth_remap_targets(const AppConfig &config, const std::vector<std::unique_ptr<CameraRuntime>> &cameras, Logger &logger) {
    if(!config.swap_depth_between_cameras) {
        return;
    }
    if(cameras.size() != 2) {
        logger.warn("depth stream remap requested but disabled at runtime because camera_count=" + std::to_string(cameras.size()));
        return;
    }
    cameras[0]->depth_remap_target = cameras[1].get();
    cameras[1]->depth_remap_target = cameras[0].get();
    logger.warn("depth stream remap enabled: source " + cameras[0]->config.camera_id + " depth is sent/previewed as "
                + cameras[1]->config.camera_id + "; source " + cameras[1]->config.camera_id + " depth is sent/previewed as "
                + cameras[0]->config.camera_id);
}

template <typename Sender>
void v4l2_camera_worker_loop(const AppConfig &config, CameraRuntime &camera, size_t primary_slot_base, size_t preview_slot_base,
                             LatestMediaQueue &rgb_media_queue, LatestMediaQueue &preview_media_queue, Sender &transport, Logger &logger,
                             std::mutex &transport_mutex, std::chrono::milliseconds preview_interval) {
    const size_t rgb_slot = primary_slot_base;
    const size_t rgb_preview_slot = preview_slot_base + 2;

    while(g_running) {
        const auto loop_now = std::chrono::steady_clock::now();
        if(!camera_is_online(camera)) {
            if(camera_reconnect_enabled(camera)) {
                retry_camera_reconnect(config, camera, transport, logger, transport_mutex, loop_now);
            }
            else {
                logger.info("v4l2 camera worker exiting camera_id=" + camera.config.camera_id + " reason=reconnect_disabled");
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        std::shared_ptr<V4L2MjpegCapture> capture;
        {
            std::lock_guard<std::mutex> lock(camera.mutex);
            capture = camera.v4l2_capture;
        }
        if(!capture) {
            mark_camera_disconnected(config, camera, transport, logger, transport_mutex, "v4l2 capture is not open");
            continue;
        }

        V4L2MjpegCapture::Frame frame;
        try {
            const auto wait_started = std::chrono::steady_clock::now();
            const bool has_frame = capture->wait_frame(frame, std::chrono::milliseconds(100));
            const auto wait_ended = std::chrono::steady_clock::now();
            record_wait_result(camera, elapsed_ms(wait_started, wait_ended), !has_frame);
            if(!has_frame) {
                if(auto reason = capture_stream_stall_reason(camera, wait_ended)) {
                    mark_camera_disconnected(config, camera, transport, logger, transport_mutex, *reason);
                }
                continue;
            }
        }
        catch(const std::exception &e) {
            mark_camera_disconnected(config, camera, transport, logger, transport_mutex, e.what());
            continue;
        }

        try {
            const auto frame_now = std::chrono::steady_clock::now();
            bool preview_due = false;
            {
                std::lock_guard<std::mutex> lock(camera.mutex);
                preview_due = config.preview.enabled && frame_now >= camera.next_preview;
            }
            const bool web_preview_due = web_rgb_preview_emit_due(config, camera, frame_now);
            const bool web_preview_can_queue = web_preview_due && !rgb_media_queue.has_pending_primary();

            record_rgb_input(camera, frame.size, frame.frame_id);
            RgbFrameDiagnostics diagnostics;
            RgbEncodeTiming rgb_capture_timing{frame.frame_id,
                                               frame.system_timestamp_us,
                                               frame.system_timestamp_us,
                                               0,
                                               frame.width,
                                               frame.height,
                                               diagnostics};
            rgb_capture_timing.capture_host_timestamp_us = frame.system_timestamp_us;
            rgb_capture_timing.timing_bound_timestamp_us = now_us();

            bool rgb_usable = true;
            if(!mjpg_has_complete_jpeg_data(frame.data, frame.size)) {
                rgb_usable = false;
                mark_corrupt_rgb_jpeg_frame(camera, logger, frame.frame_id, frame.size, frame_now, "missing jpeg soi/eoi marker");
            }
            else if(camera.config.validate_rgb_mjpeg) {
                std::string jpeg_validation_message;
                if(!mjpg_decodes_cleanly_data(frame.data, frame.size, jpeg_validation_message)) {
                    rgb_usable = false;
                    mark_corrupt_rgb_jpeg_frame(camera, logger, frame.frame_id, frame.size, frame_now, jpeg_validation_message);
                }
            }

            cv::Mat bgr;
            if(rgb_usable) {
                publish_rgb_snapshot_frame(config, camera, frame.data, frame.size, rgb_capture_timing, logger);
                if(preview_due) {
                    const auto decode_started = std::chrono::steady_clock::now();
                    auto preview_bgr = mjpg_buffer_to_bgr(frame.data, frame.size, cv::IMREAD_REDUCED_COLOR_2);
                    record_rgb_decode_ms(camera, elapsed_ms(decode_started, std::chrono::steady_clock::now()));
                    set_latest_bgr(camera, preview_bgr, frame.frame_id, frame.system_timestamp_us,
                                   software_rgb_rotation_degrees(camera.config));
                }

                if(!camera.encoder) {
                    auto input_format = GstH264InputFormat::Jpeg;
                    {
                        std::lock_guard<std::mutex> encoder_lock(g_encoder_create_mutex);
                        camera.encoder = std::make_unique<GstH264Encoder>(frame.width, frame.height, camera.config.rgb_profile.fps,
                                                                            camera.config.rgb_encoding.bitrate_bps,
                                                                            camera.config.rgb_encoding.gstreamer_encoder, input_format);
                        if(!camera.encoder->ok()) {
                            logger.warn("v4l2 jpeg rgb path unavailable, falling back to BGR encode path camera_id="
                                        + camera.config.camera_id + " error=" + camera.encoder->error());
                            input_format = GstH264InputFormat::Bgr;
                            camera.encoder = std::make_unique<GstH264Encoder>(frame.width, frame.height, camera.config.rgb_profile.fps,
                                                                                camera.config.rgb_encoding.bitrate_bps,
                                                                                camera.config.rgb_encoding.gstreamer_encoder, input_format);
                        }
                    }
                    {
                        std::lock_guard<std::mutex> lock(camera.mutex);
                        camera.encoder_input_format = input_format;
                        camera.hardware_encoder = camera.encoder->ok();
                        if(!camera.hardware_encoder) {
                            camera.last_error = camera.encoder->error();
                        }
                    }
                    if(!camera.encoder->ok()) {
                        const auto error = camera.encoder->error();
                        logger.error("encoder_init_failed: " + error);
                        send_status_locked(transport, logger, transport_mutex,
                                           event_message(config, "error", "encoder_init_failed", error, camera.config.camera_id));
                    }
                    else {
                        logger.info("v4l2 rgb encoder ready camera_id=" + camera.config.camera_id
                                    + " source=" + std::to_string(frame.width) + "x" + std::to_string(frame.height)
                                    + " fps=" + std::to_string(camera.config.rgb_profile.fps)
                                    + " input=" + std::string(input_format == GstH264InputFormat::Jpeg ? "mjpg" : "bgr"));
                    }
                }

                if(web_preview_due && !camera.web_preview_encoder && !camera.encoder && false) {
                    // Unreachable; kept intentionally empty so V4L2 preview never blocks main encoder creation.
                }
                if(web_preview_due && !camera.web_preview_encoder && camera.encoder) {
                    const auto shape = resolve_web_rgb_preview_shape(config.web_rgb_preview, frame.width, frame.height);
                    if(shape.width > 0 && shape.height > 0) {
                        auto input_format = GstH264InputFormat::Jpeg;
                        {
                            std::lock_guard<std::mutex> encoder_lock(g_encoder_create_mutex);
                            camera.web_preview_encoder = std::make_unique<GstH264Encoder>(
                                frame.width, frame.height, config.web_rgb_preview.fps, config.web_rgb_preview.bitrate_bps,
                                camera.config.rgb_encoding.gstreamer_encoder, input_format, shape.width, shape.height);
                            if(!camera.web_preview_encoder->ok()) {
                                logger.warn("v4l2 jpeg web rgb preview path unavailable, falling back to BGR encode path camera_id="
                                            + camera.config.camera_id + " error=" + camera.web_preview_encoder->error());
                                input_format = GstH264InputFormat::Bgr;
                                camera.web_preview_encoder = std::make_unique<GstH264Encoder>(
                                    frame.width, frame.height, config.web_rgb_preview.fps, config.web_rgb_preview.bitrate_bps,
                                    camera.config.rgb_encoding.gstreamer_encoder, input_format, shape.width, shape.height);
                            }
                        }
                        {
                            std::lock_guard<std::mutex> lock(camera.mutex);
                            camera.web_preview_encoder_input_format = input_format;
                            if(camera.web_preview_encoder->ok()) {
                                camera.web_preview_width = static_cast<uint32_t>(camera.web_preview_encoder->output_width());
                                camera.web_preview_height = static_cast<uint32_t>(camera.web_preview_encoder->output_height());
                            }
                            else {
                                camera.web_preview_width = 0;
                                camera.web_preview_height = 0;
                                camera.last_error = camera.web_preview_encoder->error();
                            }
                        }
                    }
                }

                if(camera.encoder && camera.encoder->ok()) {
                    const auto input_format = encoder_input_format_for(camera);
                    if(input_format == GstH264InputFormat::Bgr && bgr.empty()) {
                        const auto decode_started = std::chrono::steady_clock::now();
                        bgr = mjpg_buffer_to_bgr(frame.data, frame.size, cv::IMREAD_COLOR);
                        record_rgb_decode_ms(camera, elapsed_ms(decode_started, std::chrono::steady_clock::now()));
                    }
                    if(input_format == GstH264InputFormat::Bgr && bgr.empty()) {
                        set_camera_last_error(camera, "v4l2 rgb decode produced empty frame");
                        record_queue_overwrite(camera, StreamType::rgb);
                    }
                    else {
                        uint64_t force_keyframe_request_id = 0;
                        if(consume_rgb_keyframe_request(camera, force_keyframe_request_id)) {
                            camera.encoder->request_keyframe();
                            logger.info("rgb keyframe force event queued camera_id=" + camera.config.camera_id
                                        + " request_id=" + std::to_string(force_keyframe_request_id));
                        }
                        RgbEncodeTiming submitted_timing = rgb_capture_timing;
                        submitted_timing.encode_start_timestamp_us = now_us();
                        remember_rgb_encode_timing(camera, submitted_timing);
                        const auto encode_started = std::chrono::steady_clock::now();
                        auto encoded_units = input_format == GstH264InputFormat::Jpeg
                                                 ? camera.encoder->encode_jpeg(frame.data, frame.size, frame.system_timestamp_us)
                                                 : camera.encoder->encode_bgr(bgr, frame.system_timestamp_us);
                        const uint64_t encode_done_timestamp_us = now_us();
                        submitted_timing.encode_done_timestamp_us = encode_done_timestamp_us;
                        record_rgb_encode_ms(camera, elapsed_ms(encode_started, std::chrono::steady_clock::now()));
                        publish_rgb_encoded_units(config, camera, rgb_media_queue, rgb_slot, encoded_units, submitted_timing,
                                                  encode_done_timestamp_us, frame_now, logger);

                        if(web_preview_can_queue && camera.web_preview_encoder && camera.web_preview_encoder->ok()) {
                            const auto preview_input_format = web_preview_encoder_input_format_for(camera);
                            if(preview_input_format == GstH264InputFormat::Bgr && bgr.empty()) {
                                const auto decode_started = std::chrono::steady_clock::now();
                                bgr = mjpg_buffer_to_bgr(frame.data, frame.size, cv::IMREAD_COLOR);
                                record_rgb_decode_ms(camera, elapsed_ms(decode_started, std::chrono::steady_clock::now()));
                            }
                            if(preview_input_format == GstH264InputFormat::Bgr && bgr.empty()) {
                                set_camera_last_error(camera, "v4l2 rgb preview decode produced empty frame");
                            }
                            else {
                                RgbEncodeTiming preview_timing = rgb_capture_timing;
                                preview_timing.encode_start_timestamp_us = now_us();
                                auto preview_units = preview_input_format == GstH264InputFormat::Jpeg
                                                         ? camera.web_preview_encoder->encode_jpeg(frame.data, frame.size,
                                                                                                   frame.system_timestamp_us)
                                                         : camera.web_preview_encoder->encode_bgr(bgr, frame.system_timestamp_us);
                                preview_timing.encode_done_timestamp_us = now_us();
                                publish_rgb_preview_units(config, camera, preview_media_queue, rgb_preview_slot, preview_units, preview_timing,
                                                          static_cast<uint32_t>(camera.web_preview_encoder->output_width()),
                                                          static_cast<uint32_t>(camera.web_preview_encoder->output_height()), logger);
                            }
                        }
                    }
                }
            }
            record_rgb_frame_done(camera);

            if(auto reason = capture_stream_stall_reason(camera, frame_now)) {
                mark_camera_disconnected(config, camera, transport, logger, transport_mutex, *reason);
                capture->release_frame(frame);
                continue;
            }

            if(!camera_announced(camera)) {
                send_status_locked(transport, logger, transport_mutex, camera_announce(config, camera));
                set_camera_announced(camera, true);
            }
            bool time_sync_log_due = false;
            {
                std::lock_guard<std::mutex> lock(camera.mutex);
                time_sync_log_due = frame_now >= camera.next_time_sync_log;
                if(time_sync_log_due) {
                    camera.next_time_sync_log = frame_now + std::chrono::seconds(5);
                }
            }
            if(time_sync_log_due) {
                logger.info("time_sync camera_id=" + camera.config.camera_id
                            + " host_now_us=" + std::to_string(now_us())
                            + " rgb_present=1 rgb_frame_id=" + std::to_string(frame.frame_id)
                            + " rgb_canonical_timestamp_us=" + std::to_string(frame.system_timestamp_us)
                            + " rgb_device_timestamp_us=0"
                            + " rgb_system_timestamp_us=" + std::to_string(frame.system_timestamp_us)
                            + " depth_present=0");
            }
            if(preview_due) {
                std::lock_guard<std::mutex> lock(camera.mutex);
                camera.next_preview = frame_now + preview_interval;
            }
        }
        catch(const std::exception &e) {
            set_camera_last_error(camera, e.what());
            logger.error("v4l2 rgb processing failed camera_id=" + camera.config.camera_id + " error=" + e.what());
        }
        capture->release_frame(frame);
    }
}

template <typename Sender>
void camera_worker_loop(const AppConfig &config, CameraRuntime &camera, size_t primary_slot_base, size_t preview_slot_base,
                        size_t compression_slot_base, LatestMediaQueue &rgb_media_queue, LatestMediaQueue &depth_media_queue,
                        LatestMediaQueue &preview_media_queue,
                        LatestDepthCompressionQueue &depth_compression_queue, Sender &transport, Logger &logger,
                        std::mutex &transport_mutex, std::chrono::milliseconds preview_interval) {
    if(camera.config.capture_backend == "v4l2") {
        v4l2_camera_worker_loop(config, camera, primary_slot_base, preview_slot_base, rgb_media_queue, preview_media_queue, transport, logger,
                                transport_mutex, preview_interval);
        return;
    }

    const size_t rgb_slot = primary_slot_base;
    const size_t depth_slot = primary_slot_base + 1;
    const size_t rgb_preview_slot = preview_slot_base + 2;
    const size_t depth_compression_slot = compression_slot_base + 1;

    while(g_running) {
        const auto loop_now = std::chrono::steady_clock::now();
        if(!camera_is_online(camera)) {
            if(camera_reconnect_enabled(camera)) {
                retry_camera_reconnect(config, camera, transport, logger, transport_mutex, loop_now);
            }
            else {
                logger.info("camera worker exiting camera_id=" + camera.config.camera_id + " reason=reconnect_disabled");
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        std::shared_ptr<ob::FrameSet> frameset;
        std::shared_ptr<ob::ColorFrame> color;
        std::shared_ptr<ob::DepthFrame> depth;
        CameraRuntime *depth_output_camera = nullptr;
        try {
            const auto wait_started = std::chrono::steady_clock::now();
            frameset = camera.pipeline->waitForFrames(100);
            const auto wait_ended = std::chrono::steady_clock::now();
            record_wait_result(camera, elapsed_ms(wait_started, wait_ended), !frameset);
            if(!frameset) {
                if(auto reason = capture_stream_stall_reason(camera, wait_ended)) {
                    mark_camera_disconnected(config, camera, transport, logger, transport_mutex, *reason);
                }
                continue;
            }
            color = frameset->colorFrame();
            depth = frameset->depthFrame();
        }
        catch(const ob::Error &e) {
            mark_camera_disconnected(config, camera, transport, logger, transport_mutex, ob_error_text(e));
            continue;
        }
        catch(const std::exception &e) {
            mark_camera_disconnected(config, camera, transport, logger, transport_mutex, e.what());
            continue;
        }

        const auto frame_now = std::chrono::steady_clock::now();
        const uint64_t frame_host_now_us = now_us();
        bool reapply_gemini305_manual_exposure = false;
        std::string reapply_device_model;
        std::string reapply_device_serial;
        if(color) {
            std::lock_guard<std::mutex> lock(camera.mutex);
            if(camera.gemini305_manual_exposure_reapply_pending
               && frame_now >= camera.gemini305_manual_exposure_reapply_at) {
                camera.gemini305_manual_exposure_reapply_pending = false;
                reapply_gemini305_manual_exposure = true;
                reapply_device_model = camera.device_model;
                reapply_device_serial = camera.device_serial;
            }
        }
        if(reapply_gemini305_manual_exposure) {
            bool verified =
                ensure_gemini305_manual_exposure(camera.config, reapply_device_model, reapply_device_serial, logger, true);
            if(verified) {
                try {
                    apply_color_controls(camera, logger);
                    logger.info("Gemini305 SDK color controls reapplied after manual exposure verification camera_id="
                                + camera.config.camera_id);
                }
                catch(const std::exception &e) {
                    verified = false;
                    logger.warn("Gemini305 SDK color control reapply failed camera_id=" + camera.config.camera_id
                                + " error=" + e.what());
                }
            }
            std::lock_guard<std::mutex> lock(camera.mutex);
            camera.publish_warmup_exposure_verified = verified;
            camera.gemini305_manual_exposure_reapply_pending = !verified;
            camera.gemini305_manual_exposure_reapply_at = verified ? std::chrono::steady_clock::time_point::min()
                                                                   : frame_now + std::chrono::seconds(1);
        }
        depth_output_camera = depth ? depth_target_camera(config, camera) : nullptr;
        bool warmup_drop = false;
        bool warmup_completed = false;
        bool warmup_forced = false;
        uint64_t warmup_dropped = 0;
        {
            std::lock_guard<std::mutex> lock(camera.mutex);
            if(camera.publish_warmup_active) {
                const bool ready = frame_now >= camera.publish_not_before
                                   && camera.publish_warmup_exposure_verified
                                   && !camera.gemini305_manual_exposure_reapply_pending;
                warmup_forced = !ready && frame_now >= camera.publish_warmup_deadline;
                if(ready || warmup_forced) {
                    camera.publish_warmup_active = false;
                    warmup_completed = true;
                    warmup_dropped = camera.publish_warmup_dropped_framesets;
                }
                else {
                    camera.publish_warmup_dropped_framesets++;
                    warmup_dropped = camera.publish_warmup_dropped_framesets;
                    warmup_drop = true;
                }
            }
        }
        if(warmup_drop) {
            if(color) {
                record_rgb_input(camera, color);
                update_color_metadata(camera, color);
            }
            if(depth) {
                record_depth_input(camera, depth);
                set_depth_scale_if_empty(camera, depth->getValueScale());
                if(depth_output_camera) {
                    set_depth_scale_if_empty(*depth_output_camera, depth->getValueScale());
                }
            }
            continue;
        }
        if(warmup_completed) {
            request_rgb_keyframe(camera, logger, "capture_warmup_complete");
            const std::string message = "capture publish warmup completed dropped_framesets=" + std::to_string(warmup_dropped)
                                        + " forced=" + (warmup_forced ? "true" : "false");
            if(warmup_forced) {
                logger.warn(message + " camera_id=" + camera.config.camera_id);
            }
            else {
                logger.info(message + " camera_id=" + camera.config.camera_id);
            }
            send_status_locked(transport, logger, transport_mutex,
                               event_message(config, warmup_forced ? "warning" : "info", "capture_warmup_complete", message,
                                             camera.config.camera_id));
        }
        uint64_t frameset_pair_id = 0;
        if(color && depth && depth_output_camera == &camera) {
            std::lock_guard<std::mutex> lock(camera.mutex);
            frameset_pair_id = camera.next_pair_id++;
            if(camera.next_pair_id == 0) {
                camera.next_pair_id = 1;
            }
        }
        bool preview_due = false;
        {
            std::lock_guard<std::mutex> lock(camera.mutex);
            preview_due = config.preview.enabled && frame_now >= camera.next_preview;
        }
        const bool web_preview_due = web_rgb_preview_emit_due(config, camera, frame_now);

        cv::Mat bgr;
        if(color) {
            record_rgb_input(camera, color);
            update_color_metadata(camera, color);
            const uint64_t rgb_device_timestamp_us = color->timeStampUs();
            const uint64_t rgb_system_timestamp_us = normalize_capture_system_timestamp(
                camera, StreamType::rgb, frame_system_timestamp_us_or(color, frame_host_now_us), frame_host_now_us);
            const bool color_is_mjpg = color->format() == OB_FORMAT_MJPG;
            RgbEncodeTiming rgb_capture_timing{color->index(), rgb_device_timestamp_us, rgb_system_timestamp_us, frameset_pair_id,
                                               static_cast<uint32_t>(color->width()), static_cast<uint32_t>(color->height()),
                                               rgb_frame_diagnostics(camera, color)};
            rgb_capture_timing.capture_host_timestamp_us = frame_host_now_us;
            rgb_capture_timing.timing_bound_timestamp_us = now_us();
            bool rgb_usable = true;
            if(color_is_mjpg && !mjpg_has_complete_jpeg(color)) {
                rgb_usable = false;
                mark_corrupt_rgb_jpeg_frame(camera, logger, color, frame_now, "missing jpeg soi/eoi marker");
            }
            else if(color_is_mjpg && (preview_due || camera.config.validate_rgb_mjpeg)) {
                std::string jpeg_validation_message;
                if(!mjpg_decodes_cleanly(color, jpeg_validation_message)) {
                    rgb_usable = false;
                    mark_corrupt_rgb_jpeg_frame(camera, logger, color, frame_now, jpeg_validation_message);
                }
            }

            if(rgb_usable) {
                maybe_update_adaptive_exposure(camera, color, frame_now, logger);
                if(color_is_mjpg) {
                    publish_rgb_snapshot_frame(config, camera, static_cast<const uint8_t *>(color->data()), color->dataSize(),
                                               rgb_capture_timing, logger);
                }
                else {
                    reject_next_rgb_snapshot_request(config, camera,
                                                     "configured RGB profile does not provide original MJPEG frames", logger);
                }
                if(color_is_mjpg && preview_due) {
                    const auto decode_started = std::chrono::steady_clock::now();
                    auto preview_bgr = color_to_preview_bgr(color);
                    record_rgb_decode_ms(camera, elapsed_ms(decode_started, std::chrono::steady_clock::now()));
                    set_latest_bgr(camera, preview_bgr, color->index(), rgb_system_timestamp_us,
                                   software_rgb_rotation_degrees(camera.config));
                }
                else if(!color_is_mjpg) {
                    const auto decode_started = std::chrono::steady_clock::now();
                    bgr = color_to_bgr(color);
                    record_rgb_decode_ms(camera, elapsed_ms(decode_started, std::chrono::steady_clock::now()));
                    if(preview_due) {
                        set_latest_bgr(camera, bgr, color->index(), rgb_system_timestamp_us,
                                       software_rgb_rotation_degrees(camera.config));
                    }
                    apply_software_rgb_rotation(bgr, camera.config);
                }

                if(!camera.encoder && !camera.jpeg_dual_encoder && (color_is_mjpg || !bgr.empty())) {
                    auto input_format = color_is_mjpg && software_rgb_rotation_degrees(camera.config) == 0
                                            ? GstH264InputFormat::Jpeg
                                            : GstH264InputFormat::Bgr;
                    const auto color_profile = camera_color_profile(camera);
                    if(!color_profile) {
                        set_camera_last_error(camera, "missing rgb stream profile");
                    }
                    else {
                        if(color_is_mjpg && software_rgb_rotation_degrees(camera.config) == 0 && config.web_rgb_preview.enabled
                           && jpeg_dual_encoder_experiment_enabled()
                           && !camera.jpeg_dual_encoder_disabled) {
                            const auto shape = resolve_web_rgb_preview_shape(config.web_rgb_preview, color->width(), color->height());
                            if(shape.width > 0 && shape.height > 0) {
                                std::lock_guard<std::mutex> encoder_lock(g_encoder_create_mutex);
                                camera.jpeg_dual_encoder = std::make_unique<GstJpegDualH264Encoder>(
                                    color->width(), color->height(), color_profile->fps(), camera.config.rgb_encoding.bitrate_bps,
                                    camera.config.rgb_encoding.gstreamer_encoder, shape.width, shape.height, config.web_rgb_preview.fps,
                                    config.web_rgb_preview.bitrate_bps);
                                if(camera.jpeg_dual_encoder->ok()) {
                                    std::lock_guard<std::mutex> lock(camera.mutex);
                                    camera.encoder_input_format = GstH264InputFormat::Jpeg;
                                    camera.web_preview_encoder_input_format = GstH264InputFormat::Jpeg;
                                    camera.hardware_encoder = true;
                                    camera.web_preview_width = static_cast<uint32_t>(camera.jpeg_dual_encoder->preview_output_width());
                                    camera.web_preview_height = static_cast<uint32_t>(camera.jpeg_dual_encoder->preview_output_height());
                                    camera.jpeg_dual_no_main_output = 0;
                                    logger.info("jpeg tee rgb encoder ready camera_id=" + camera.config.camera_id
                                                + " source=" + std::to_string(color->width()) + "x" + std::to_string(color->height())
                                                + " preview=" + std::to_string(shape.width) + "x" + std::to_string(shape.height)
                                                + " fps=" + std::to_string(color_profile->fps())
                                                + " preview_fps=" + std::to_string(config.web_rgb_preview.fps));
                                }
                                else {
                                    logger.warn("jpeg tee rgb encoder unavailable, falling back to separate encoders camera_id="
                                                + camera.config.camera_id + " error=" + camera.jpeg_dual_encoder->error());
                                    camera.jpeg_dual_encoder.reset();
                                }
                            }
                        }
                    }
                    if(!camera.jpeg_dual_encoder && color_profile) {
                        {
                            std::lock_guard<std::mutex> encoder_lock(g_encoder_create_mutex);
                            camera.encoder = std::make_unique<GstH264Encoder>(color->width(), color->height(), color_profile->fps(),
                                                                                camera.config.rgb_encoding.bitrate_bps,
                                                                                camera.config.rgb_encoding.gstreamer_encoder, input_format);
                            if(!camera.encoder->ok() && color_is_mjpg) {
                                logger.warn("mppjpegdec rgb path unavailable, falling back to BGR encode path: " + camera.encoder->error());
                                input_format = GstH264InputFormat::Bgr;
                                camera.encoder = std::make_unique<GstH264Encoder>(color->width(), color->height(), color_profile->fps(),
                                                                                    camera.config.rgb_encoding.bitrate_bps,
                                                                                    camera.config.rgb_encoding.gstreamer_encoder, input_format);
                            }
                        }
                        {
                            std::lock_guard<std::mutex> lock(camera.mutex);
                            camera.encoder_input_format = input_format;
                            camera.hardware_encoder = camera.encoder->ok();
                            if(!camera.hardware_encoder) {
                                camera.last_error = camera.encoder->error();
                            }
                        }
                        if(!camera.encoder->ok()) {
                            const auto error = camera.encoder->error();
                            logger.error("encoder_init_failed: " + error);
                            send_status_locked(transport, logger, transport_mutex,
                                               event_message(config, "error", "encoder_init_failed", error, camera.config.camera_id));
                        }
                    }
                }

                if(web_preview_due && !camera.web_preview_encoder && !camera.jpeg_dual_encoder && (color_is_mjpg || !bgr.empty())) {
                    auto input_format = color_is_mjpg && software_rgb_rotation_degrees(camera.config) == 0
                                            ? GstH264InputFormat::Jpeg
                                            : GstH264InputFormat::Bgr;
                    const auto color_profile = camera_color_profile(camera);
                    if(color_profile) {
                        const auto shape = resolve_web_rgb_preview_shape(config.web_rgb_preview, color->width(), color->height());
                        if(shape.width > 0 && shape.height > 0) {
                            {
                                std::lock_guard<std::mutex> encoder_lock(g_encoder_create_mutex);
                                camera.web_preview_encoder = std::make_unique<GstH264Encoder>(
                                    color->width(), color->height(), config.web_rgb_preview.fps, config.web_rgb_preview.bitrate_bps,
                                    camera.config.rgb_encoding.gstreamer_encoder, input_format, shape.width, shape.height);
                                if(!camera.web_preview_encoder->ok() && color_is_mjpg) {
                                    logger.warn("mppjpegdec web rgb preview path unavailable, falling back to BGR encode path: "
                                                + camera.web_preview_encoder->error());
                                    input_format = GstH264InputFormat::Bgr;
                                    camera.web_preview_encoder = std::make_unique<GstH264Encoder>(
                                        color->width(), color->height(), config.web_rgb_preview.fps, config.web_rgb_preview.bitrate_bps,
                                        camera.config.rgb_encoding.gstreamer_encoder, input_format, shape.width, shape.height);
                                }
                            }
                            {
                                std::lock_guard<std::mutex> lock(camera.mutex);
                                camera.web_preview_encoder_input_format = input_format;
                                if(camera.web_preview_encoder->ok()) {
                                    camera.web_preview_width = static_cast<uint32_t>(camera.web_preview_encoder->output_width());
                                    camera.web_preview_height = static_cast<uint32_t>(camera.web_preview_encoder->output_height());
                                }
                                else {
                                    camera.web_preview_width = 0;
                                    camera.web_preview_height = 0;
                                    camera.last_error = camera.web_preview_encoder->error();
                                }
                            }
                            if(camera.web_preview_encoder->ok()) {
                                logger.info("web rgb preview encoder ready camera_id=" + camera.config.camera_id
                                            + " source=" + std::to_string(color->width()) + "x" + std::to_string(color->height())
                                            + " preview=" + std::to_string(shape.width) + "x" + std::to_string(shape.height)
                                            + " fps=" + std::to_string(config.web_rgb_preview.fps)
                                            + " bitrate_bps=" + std::to_string(config.web_rgb_preview.bitrate_bps));
                            }
                            else {
                                logger.warn("web rgb preview encoder init failed camera_id=" + camera.config.camera_id
                                            + " error=" + camera.web_preview_encoder->error());
                            }
                        }
                    }
                }

                const bool web_preview_can_queue = web_preview_due && !rgb_media_queue.has_pending_primary();
                if((camera.jpeg_dual_encoder && camera.jpeg_dual_encoder->ok()) || (camera.encoder && camera.encoder->ok())) {
                    try {
                        if(camera.jpeg_dual_encoder && camera.jpeg_dual_encoder->ok()) {
                            uint64_t force_keyframe_request_id = 0;
                            if(consume_rgb_keyframe_request(camera, force_keyframe_request_id)) {
                                camera.jpeg_dual_encoder->request_keyframe();
                                logger.info("rgb keyframe force event queued camera_id=" + camera.config.camera_id
                                            + " request_id=" + std::to_string(force_keyframe_request_id));
                            }
                            RgbEncodeTiming submitted_timing = rgb_capture_timing;
                            submitted_timing.encode_start_timestamp_us = now_us();
                            remember_rgb_encode_timing(camera, submitted_timing);
                            RgbEncodeTiming preview_timing = submitted_timing;
                            const auto encode_started = std::chrono::steady_clock::now();
                            auto encoded_units = camera.jpeg_dual_encoder->encode_jpeg(color->data(), color->dataSize(),
                                                                                       rgb_system_timestamp_us, web_preview_can_queue);
                            const uint64_t encode_done_timestamp_us = now_us();
                            submitted_timing.encode_done_timestamp_us = encode_done_timestamp_us;
                            preview_timing.encode_done_timestamp_us = encode_done_timestamp_us;
                            record_rgb_encode_ms(camera, elapsed_ms(encode_started, std::chrono::steady_clock::now()));
                            if(encoded_units.main.empty()) {
                                bool disable_dual_encoder = false;
                                {
                                    std::lock_guard<std::mutex> lock(camera.mutex);
                                    disable_dual_encoder = ++camera.jpeg_dual_no_main_output >= kJpegDualNoMainOutputFallbackFrames;
                                    if(disable_dual_encoder) {
                                        camera.jpeg_dual_encoder_disabled = true;
                                        camera.last_error = "jpeg tee encoder produced no main rgb output; falling back";
                                        camera.web_preview_width = 0;
                                        camera.web_preview_height = 0;
                                        camera.rgb_encode_timings.clear();
                                    }
                                }
                                if(disable_dual_encoder) {
                                    logger.warn("jpeg tee rgb encoder disabled camera_id=" + camera.config.camera_id
                                                + " reason=no_main_output fallback=separate_rgb_and_preview_encoders");
                                    camera.jpeg_dual_encoder.reset();
                                }
                            }
                            else {
                                std::lock_guard<std::mutex> lock(camera.mutex);
                                camera.jpeg_dual_no_main_output = 0;
                            }
                            publish_rgb_encoded_units(config, camera, rgb_media_queue, rgb_slot, encoded_units.main, submitted_timing,
                                                      encode_done_timestamp_us, frame_now, logger);
                            if(camera.jpeg_dual_encoder && web_preview_can_queue && !encoded_units.preview.empty()) {
                                publish_rgb_preview_units(config, camera, preview_media_queue, rgb_preview_slot, encoded_units.preview, preview_timing,
                                                          static_cast<uint32_t>(camera.jpeg_dual_encoder->preview_output_width()),
                                                          static_cast<uint32_t>(camera.jpeg_dual_encoder->preview_output_height()), logger);
                            }
                        }
                        else {
                            const auto input_format = encoder_input_format_for(camera);
                            if(input_format == GstH264InputFormat::Bgr && bgr.empty()) {
                                const auto decode_started = std::chrono::steady_clock::now();
                                bgr = color_to_bgr(color);
                                apply_software_rgb_rotation(bgr, camera.config);
                                record_rgb_decode_ms(camera, elapsed_ms(decode_started, std::chrono::steady_clock::now()));
                            }
                            if(input_format == GstH264InputFormat::Bgr && bgr.empty()) {
                                set_camera_last_error(camera, "rgb decode produced empty frame");
                                record_queue_overwrite(camera, StreamType::rgb);
                            }
                            else {
                                uint64_t force_keyframe_request_id = 0;
                                if(consume_rgb_keyframe_request(camera, force_keyframe_request_id)) {
                                    camera.encoder->request_keyframe();
                                    logger.info("rgb keyframe force event queued camera_id=" + camera.config.camera_id
                                                + " request_id=" + std::to_string(force_keyframe_request_id));
                                }
                                RgbEncodeTiming submitted_timing = rgb_capture_timing;
                                submitted_timing.encode_start_timestamp_us = now_us();
                                remember_rgb_encode_timing(camera, submitted_timing);
                                const auto encode_started = std::chrono::steady_clock::now();
                                auto encoded_units = input_format == GstH264InputFormat::Jpeg
                                                         ? camera.encoder->encode_jpeg(color->data(), color->dataSize(), rgb_system_timestamp_us)
                                                         : camera.encoder->encode_bgr(bgr, rgb_system_timestamp_us);
                                const uint64_t encode_done_timestamp_us = now_us();
                                submitted_timing.encode_done_timestamp_us = encode_done_timestamp_us;
                                record_rgb_encode_ms(camera, elapsed_ms(encode_started, std::chrono::steady_clock::now()));
                                publish_rgb_encoded_units(config, camera, rgb_media_queue, rgb_slot, encoded_units, submitted_timing,
                                                          encode_done_timestamp_us, frame_now, logger);
                                if(web_preview_can_queue && camera.web_preview_encoder && camera.web_preview_encoder->ok()) {
                                    const auto preview_input_format = web_preview_encoder_input_format_for(camera);
                                    if(preview_input_format == GstH264InputFormat::Bgr && bgr.empty()) {
                                        const auto decode_started = std::chrono::steady_clock::now();
                                        bgr = color_to_bgr(color);
                                        apply_software_rgb_rotation(bgr, camera.config);
                                        record_rgb_decode_ms(camera, elapsed_ms(decode_started, std::chrono::steady_clock::now()));
                                    }
                                    if(preview_input_format == GstH264InputFormat::Bgr && bgr.empty()) {
                                        set_camera_last_error(camera, "rgb preview decode produced empty frame");
                                    }
                                    else {
                                        RgbEncodeTiming preview_timing = rgb_capture_timing;
                                        preview_timing.encode_start_timestamp_us = now_us();
                                        auto preview_units = preview_input_format == GstH264InputFormat::Jpeg
                                                                 ? camera.web_preview_encoder->encode_jpeg(color->data(), color->dataSize(),
                                                                                                           rgb_system_timestamp_us)
                                                                 : camera.web_preview_encoder->encode_bgr(bgr, rgb_system_timestamp_us);
                                        preview_timing.encode_done_timestamp_us = now_us();
                                        publish_rgb_preview_units(config, camera, preview_media_queue, rgb_preview_slot, preview_units, preview_timing,
                                                                  static_cast<uint32_t>(camera.web_preview_encoder->output_width()),
                                                                  static_cast<uint32_t>(camera.web_preview_encoder->output_height()), logger);
                                    }
                                }
                            }
                        }
                    }
                    catch(const std::exception &e) {
                        set_camera_last_error(camera, e.what());
                        logger.error("rgb encode failed camera_id=" + camera.config.camera_id + " error=" + e.what());
                    }
                }
            }
            record_rgb_frame_done(camera);
        }

        if(depth) {
            auto *depth_target = depth_output_camera;
            record_depth_input(camera, depth);
            set_depth_scale_if_empty(camera, depth->getValueScale());
            set_depth_scale_if_empty(*depth_target, depth->getValueScale());
            const bool publish_depth = depth_emit_due(camera, frame_now);
            if(publish_depth) {
                const void *depth_payload = depth->data();
                size_t depth_payload_size = depth->dataSize();
                const uint64_t depth_device_timestamp_us = depth->timeStampUs();
                const uint64_t depth_system_timestamp_us = normalize_capture_system_timestamp(
                    camera, StreamType::depth_raw, frame_system_timestamp_us_or(depth, frame_host_now_us), frame_host_now_us);
                MediaFrameMeta meta;
                meta.stream_type = StreamType::depth_raw;
                meta.flags = has_system_timestamp;
                meta.sender_id = config.sender_id;
                meta.camera_id = depth_target->config.camera_id;
                meta.codec_or_compression = camera.config.depth_transport.compression;
                meta.frame_id = depth->index();
                meta.timestamp_us = depth_device_timestamp_us;
                meta.system_timestamp_us = depth_system_timestamp_us;
                meta.pair_id = frameset_pair_id;
                meta.width = depth->width();
                meta.height = depth->height();
                meta.pixel_format = PixelFormat::depth_u16;
                meta.payload_size = depth_payload_size;
                meta.uncompressed_size = depth->dataSize();
                if(depth_transport_uses_compression(camera.config.depth_transport.compression)) {
                    DepthCompressionJob compress_job;
                    compress_job.source_camera = &camera;
                    compress_job.output_camera = depth_target;
                    compress_job.output_media_queue = &depth_media_queue;
                    compress_job.media_slot_index = depth_slot;
                    compress_job.meta = meta;
                    {
                        std::lock_guard<std::mutex> lock(camera.mutex);
                        compress_job.depth_scale = camera.depth_scale > 0.0f ? camera.depth_scale : 1.0f;
                    }
                    compress_job.quantization_step_mm = camera.config.depth_transport.quantization_step_mm;
                    const auto *depth_bytes = static_cast<const uint8_t *>(depth->data());
                    compress_job.raw_payload = depth_bytes;
                    compress_job.raw_payload_size = depth->dataSize();
                    compress_job.raw_payload_owner = std::shared_ptr<const void>(depth, static_cast<const void *>(depth->data()));
                    if(depth_compression_queue.publish(depth_compression_slot, std::move(compress_job))) {
                        record_queue_overwrite(*depth_target, StreamType::depth_raw);
                    }
                }
                else {
                    MediaPacketJob job;
                    auto owner = std::shared_ptr<const void>(depth, static_cast<const void *>(depth->data()));
                    job = make_external_media_job(*depth_target, StreamType::depth_raw, meta, depth_payload, depth_payload_size, std::move(owner));
                    publish_media_job(depth_media_queue, depth_slot, *depth_target, StreamType::depth_raw, std::move(job), logger);
                    record_depth_frame_done(*depth_target);
                }

                if(preview_due) {
                    const auto depth_preview_started = std::chrono::steady_clock::now();
                    auto depth_color = depth_to_color(depth);
                    record_depth_preview_ms(camera, elapsed_ms(depth_preview_started, std::chrono::steady_clock::now()));
                    set_latest_depth_color(*depth_target, depth_color);
                }
            }
        }

        if(auto reason = capture_stream_stall_reason(camera, frame_now)) {
            mark_camera_disconnected(config, camera, transport, logger, transport_mutex, *reason);
            continue;
        }

        if(!camera_announced(camera) && color && (!camera.config.depth_profile.enabled || depth)) {
            send_status_locked(transport, logger, transport_mutex, camera_announce(config, camera));
            set_camera_announced(camera, true);
        }
        const std::string depth_output_camera_id = depth_output_camera ? depth_output_camera->config.camera_id : camera.config.camera_id;
        log_time_sync(camera, logger, color, depth, depth_output_camera_id, frame_now);
        if(preview_due) {
            std::lock_guard<std::mutex> lock(camera.mutex);
            camera.next_preview = frame_now + preview_interval;
        }
    }
}

template <typename Sender>
void camera_worker_thread_entry(const AppConfig &config, CameraRuntime &camera, size_t primary_slot_base, size_t preview_slot_base,
                                size_t compression_slot_base, LatestMediaQueue &rgb_media_queue, LatestMediaQueue &depth_media_queue,
                                LatestMediaQueue &preview_media_queue, LatestDepthCompressionQueue &depth_compression_queue,
                                Sender &transport, Logger &logger, std::mutex &transport_mutex,
                                std::chrono::milliseconds preview_interval) {
    while(g_running) {
        try {
            camera_worker_loop(config, camera, primary_slot_base, preview_slot_base, compression_slot_base,
                               rgb_media_queue, depth_media_queue, preview_media_queue, depth_compression_queue,
                               transport, logger, transport_mutex, preview_interval);
            return;
        }
        catch(const ob::Error &e) {
            try {
                mark_camera_disconnected(config, camera, transport, logger, transport_mutex, ob_error_text(e));
            }
            catch(const std::exception &cleanup_error) {
                logger.error("camera worker cleanup failed camera_id=" + camera.config.camera_id + " error=" + cleanup_error.what());
                return;
            }
        }
        catch(const std::exception &e) {
            try {
                mark_camera_disconnected(config, camera, transport, logger, transport_mutex, e.what());
            }
            catch(const std::exception &cleanup_error) {
                logger.error("camera worker cleanup failed camera_id=" + camera.config.camera_id + " error=" + cleanup_error.what());
                return;
            }
        }
        catch(...) {
            try {
                mark_camera_disconnected(config, camera, transport, logger, transport_mutex, "unknown camera worker exception");
            }
            catch(...) {
                return;
            }
        }
    }
}

struct CameraSummary {
    std::string camera_id;
    uint64_t rgb_frames = 0;
    uint64_t depth_frames = 0;
    bool hardware_encoder = false;
    float depth_scale = 0.0f;
    uint32_t disconnects = 0;
    std::string last_error;
};

CameraSummary snapshot_camera_summary(const CameraRuntime &camera) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    CameraSummary summary;
    summary.camera_id = camera.config.camera_id;
    summary.rgb_frames = camera.rgb_frames;
    summary.depth_frames = camera.depth_frames;
    summary.hardware_encoder = camera.hardware_encoder;
    summary.depth_scale = camera.depth_scale;
    summary.disconnects = camera.disconnects;
    summary.last_error = camera.last_error;
    return summary;
}

struct HotplugRetryCooldown {
    std::string device_key;
    std::chrono::steady_clock::time_point retry_after = std::chrono::steady_clock::now();
};

std::string hotplug_device_key(const OrbbecDeviceIdentity &identity) {
    if(!identity.uid.empty()) {
        return "uid:" + identity.uid;
    }
    if(!identity.serial.empty()) {
        return "serial:" + identity.serial;
    }
    if(!identity.paired_rgb_serial.empty()) {
        return "paired_rgb_serial:" + identity.paired_rgb_serial;
    }
    return "index:" + std::to_string(identity.index);
}

bool hotplug_retry_allowed(const std::vector<HotplugRetryCooldown> &cooldowns, const std::string &device_key,
                           std::chrono::steady_clock::time_point now) {
    for(const auto &cooldown : cooldowns) {
        if(cooldown.device_key == device_key && now < cooldown.retry_after) {
            return false;
        }
    }
    return true;
}

void set_hotplug_retry_cooldown(std::vector<HotplugRetryCooldown> &cooldowns, const std::string &device_key,
                                std::chrono::steady_clock::time_point retry_after) {
    for(auto &cooldown : cooldowns) {
        if(cooldown.device_key == device_key) {
            cooldown.retry_after = retry_after;
            return;
        }
    }
    HotplugRetryCooldown cooldown;
    cooldown.device_key = device_key;
    cooldown.retry_after = retry_after;
    cooldowns.push_back(std::move(cooldown));
}

std::optional<int> numeric_cam_suffix(const std::string &camera_id) {
    if(camera_id.size() <= 3 || camera_id.rfind("cam", 0) != 0) {
        return std::nullopt;
    }
    const auto suffix = camera_id.substr(3);
    if(!std::all_of(suffix.begin(), suffix.end(), [](char ch) { return ch >= '0' && ch <= '9'; })) {
        return std::nullopt;
    }
    try {
        return std::stoi(suffix);
    }
    catch(const std::exception &) {
        return std::nullopt;
    }
}

int initial_hotplug_camera_number(const AppConfig &config) {
    int next_number = 1;
    for(const auto &camera : config.cameras) {
        if(auto suffix = numeric_cam_suffix(camera.camera_id)) {
            next_number = std::max(next_number, *suffix + 1);
        }
    }
    return next_number;
}

std::string format_hotplug_camera_id(int camera_number) {
    std::ostringstream oss;
    oss << "cam" << std::setw(2) << std::setfill('0') << camera_number;
    return oss.str();
}

CameraConfig make_hotplug_camera_config(const AppConfig &config, const OrbbecDeviceIdentity &identity,
                                        const std::string &camera_id) {
    CameraConfig camera = config.cameras.front();
    camera.camera_id = camera_id;
    camera.serial_number.clear();
    camera.uid.clear();
    camera.device_index = static_cast<int>(identity.index);
    if(!identity.uid.empty()) {
        camera.uid = identity.uid;
    }
    else if(!identity.paired_rgb_serial.empty()) {
        camera.serial_number = identity.paired_rgb_serial;
    }
    else if(!identity.serial.empty()) {
        camera.serial_number = identity.serial;
    }
    return camera;
}

size_t active_hotplug_camera_count(const std::vector<std::unique_ptr<CameraRuntime>> &cameras) {
    size_t active = 0;
    for(const auto &camera : cameras) {
        if(camera_counts_against_hotplug_limit(*camera)) {
            ++active;
        }
    }
    return active;
}

bool device_already_claimed(const std::vector<std::unique_ptr<CameraRuntime>> &cameras, const OrbbecDeviceIdentity &identity) {
    for(const auto &camera : cameras) {
        if(camera_matches_identity(*camera, identity)) {
            return true;
        }
    }
    return false;
}

std::vector<OrbbecDeviceIdentity> query_hotplug_device_identities() {
    std::lock_guard<std::mutex> lifecycle_lock(g_camera_lifecycle_mutex);
    ob::Context::setLoggerSeverity(OB_LOG_SEVERITY_WARN);
    ob::Context context;
    return enumerate_orbbec_devices(context);
}

template <typename Sender>
struct MediaSenderPath {
    LatestMediaQueue queue;
    std::unique_ptr<Sender> transport;
    std::mutex mutex;
    std::thread thread;
    std::atomic<bool> running{true};

    MediaSenderPath(size_t slot_count, size_t rgb_frames_per_slot, size_t depth_frames_per_slot, std::unique_ptr<Sender> sender)
        : queue(slot_count, rgb_frames_per_slot, depth_frames_per_slot), transport(std::move(sender)) {}

    ~MediaSenderPath() {
        running = false;
        queue.stop();
        if(thread.joinable()) {
            thread.join();
        }
    }
};

class ThreadJoinGuard {
public:
    explicit ThreadJoinGuard(std::thread &thread) : thread_(thread) {}
    ~ThreadJoinGuard() {
        g_running = false;
        if(thread_.joinable()) {
            thread_.join();
        }
    }

    ThreadJoinGuard(const ThreadJoinGuard &) = delete;
    ThreadJoinGuard &operator=(const ThreadJoinGuard &) = delete;

private:
    std::thread &thread_;
};

class ThreadVectorJoinGuard {
public:
    explicit ThreadVectorJoinGuard(std::vector<std::thread> &threads, std::function<void()> before_join = {})
        : threads_(threads), before_join_(std::move(before_join)) {}
    ~ThreadVectorJoinGuard() {
        g_running = false;
        if(before_join_) {
            before_join_();
        }
        for(auto &thread : threads_) {
            if(thread.joinable()) {
                thread.join();
            }
        }
    }

    ThreadVectorJoinGuard(const ThreadVectorJoinGuard &) = delete;
    ThreadVectorJoinGuard &operator=(const ThreadVectorJoinGuard &) = delete;

private:
    std::vector<std::thread> &threads_;
    std::function<void()> before_join_;
};

template <typename Sender, typename MakeSender>
std::unique_ptr<MediaSenderPath<Sender>> start_media_sender_path(size_t slot_count, size_t rgb_frames_per_slot,
                                                                 size_t depth_frames_per_slot, MakeSender &make_sender, Logger &logger) {
    auto path = std::make_unique<MediaSenderPath<Sender>>(slot_count, rgb_frames_per_slot, depth_frames_per_slot, make_sender());
    auto *path_ptr = path.get();
    path_ptr->thread = std::thread([path_ptr, &logger] {
        try {
            media_sender_loop(path_ptr->queue, *path_ptr->transport, logger, path_ptr->mutex, &path_ptr->running);
        }
        catch(const std::exception &e) {
            logger.error(std::string("media sender path stopped unexpectedly: ") + e.what());
            g_running = false;
        }
        catch(...) {
            logger.error("media sender path stopped unexpectedly: unknown exception");
            g_running = false;
        }
    });
    return path;
}

template <typename StatusSender, typename MediaSender, typename MakeMediaSender>
void scan_hotplug_cameras(const AppConfig &config, std::vector<std::unique_ptr<CameraRuntime>> &cameras,
                          std::vector<std::thread> &camera_threads,
                          std::vector<std::unique_ptr<MediaSenderPath<MediaSender>>> &rgb_media_paths,
                          std::vector<std::unique_ptr<MediaSenderPath<MediaSender>>> &depth_media_paths,
                          LatestMediaQueue &preview_media_queue, LatestDepthCompressionQueue &depth_compression_queue,
                          ReliableSnapshotQueue &rgb_snapshot_queue,
                          MakeMediaSender &make_media_sender, StatusSender &transport, Logger &logger,
                          std::mutex &transport_mutex, std::chrono::milliseconds preview_interval,
                          int &next_hotplug_camera_number, std::vector<HotplugRetryCooldown> &retry_cooldowns,
                          std::chrono::steady_clock::time_point &next_limit_event) {
    std::vector<OrbbecDeviceIdentity> identities;
    try {
        identities = query_hotplug_device_identities();
    }
    catch(const ob::Error &e) {
        logger.warn("hotplug device scan failed: " + ob_error_text(e));
        return;
    }
    catch(const std::exception &e) {
        logger.warn(std::string("hotplug device scan failed: ") + e.what());
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    for(const auto &identity : identities) {
        if(device_already_claimed(cameras, identity)) {
            continue;
        }

        if(active_hotplug_camera_count(cameras) >= kMaxActiveCameras) {
            if(now >= next_limit_event) {
                const std::string message = "hotplug camera ignored because max active camera count is "
                                            + std::to_string(kMaxActiveCameras) + ": " + device_identity_summary(identity);
                logger.warn(message);
                send_status_locked(transport, logger, transport_mutex,
                                   event_message(config, "warning", "hotplug_camera_ignored", message));
                next_limit_event = now + kHotplugLimitEventInterval;
            }
            continue;
        }

        const auto device_key = hotplug_device_key(identity);
        if(!hotplug_retry_allowed(retry_cooldowns, device_key, now)) {
            continue;
        }

        const auto camera_id = format_hotplug_camera_id(next_hotplug_camera_number);
        auto runtime = std::make_unique<CameraRuntime>();
        runtime->config = make_hotplug_camera_config(config, identity, camera_id);
        runtime->hotplug_dynamic = true;
        runtime->reconnect_enabled = false;
        runtime->rgb_snapshot_queue = &rgb_snapshot_queue;

        try {
            start_camera_runtime(*runtime, logger);
        }
        catch(const ob::Error &e) {
            const std::string error = ob_error_text(e);
            stop_camera(*runtime, logger);
            set_hotplug_retry_cooldown(retry_cooldowns, device_key, now + kHotplugRetryCooldown);
            const std::string message = "hotplug camera start failed camera_id=" + camera_id + " device="
                                        + device_identity_summary(identity) + " error=" + error;
            logger.warn(message);
            send_status_locked(transport, logger, transport_mutex,
                               event_message(config, "warning", "hotplug_camera_start_failed", message));
            continue;
        }
        catch(const std::exception &e) {
            const std::string error = e.what();
            stop_camera(*runtime, logger);
            set_hotplug_retry_cooldown(retry_cooldowns, device_key, now + kHotplugRetryCooldown);
            const std::string message = "hotplug camera start failed camera_id=" + camera_id + " device="
                                        + device_identity_summary(identity) + " error=" + error;
            logger.warn(message);
            send_status_locked(transport, logger, transport_mutex,
                               event_message(config, "warning", "hotplug_camera_start_failed", message));
            continue;
        }

        const size_t rgb_frames_per_slot =
            config.recording_buffer.enabled ? static_cast<size_t>(config.recording_buffer.rgb_frames_per_slot) : 1;
        const size_t depth_frames_per_slot = config.recording_buffer.enabled
                                                 ? static_cast<size_t>(config.recording_buffer.depth_frames_per_slot)
                                                 : kDepthMediaQueuePerSlot;
        auto rgb_path =
            start_media_sender_path<MediaSender>(kMediaSlotsPerCamera, rgb_frames_per_slot, depth_frames_per_slot, make_media_sender, logger);
        auto depth_path =
            start_media_sender_path<MediaSender>(kMediaSlotsPerCamera, rgb_frames_per_slot, depth_frames_per_slot, make_media_sender, logger);
        auto *rgb_path_ptr = rgb_path.get();
        auto *depth_path_ptr = depth_path.get();
        rgb_media_paths.push_back(std::move(rgb_path));
        depth_media_paths.push_back(std::move(depth_path));
        const size_t primary_slot_base = 0;
        const size_t preview_slot_base = preview_media_queue.append_slots(kMediaSlotsPerCamera);
        const size_t compression_slot_base = depth_compression_queue.append_slots(kMediaSlotsPerCamera);
        CameraRuntime *camera_ptr = runtime.get();
        cameras.push_back(std::move(runtime));
        camera_threads.emplace_back([&, camera_ptr, rgb_path_ptr, depth_path_ptr, primary_slot_base, preview_slot_base, compression_slot_base] {
            camera_worker_thread_entry(config, *camera_ptr, primary_slot_base, preview_slot_base, compression_slot_base,
                                       rgb_path_ptr->queue, depth_path_ptr->queue, preview_media_queue, depth_compression_queue,
                                       transport, logger, transport_mutex, preview_interval);
        });
        logger.info("hotplug camera started camera_id=" + camera_id + " preview_slot_base=" + std::to_string(preview_slot_base)
                    + " compression_slot_base=" + std::to_string(compression_slot_base) + " device=" + device_identity_summary(identity));
        send_status_locked(transport, logger, transport_mutex,
                           event_message(config, "info", "camera_connected", "hotplug camera pipeline started", camera_id));
        ++next_hotplug_camera_number;
    }
}

template <typename StatusSender, typename MediaSender, typename MakeMediaSender, typename PreviewMediaSender>
void run_sender(AppConfig config, const Args &args, StatusSender &status_transport, MakeMediaSender &make_media_sender,
                PreviewMediaSender &preview_media_transport, Logger &logger) {
    const bool display_available = std::getenv("DISPLAY") != nullptr || std::getenv("WAYLAND_DISPLAY") != nullptr;
    if(args.no_preview || args.no_local_preview || !display_available) {
        config.preview.enabled = false;
    }
    if(args.no_preview) {
        config.web_rgb_preview.enabled = false;
    }
    if(!display_available && !args.no_preview && !args.no_local_preview) {
        logger.warn("DISPLAY/WAYLAND_DISPLAY not set; local preview disabled for this run");
    }
    int preview_fps = config.preview.fps > 0 ? config.preview.fps : 10;
    const bool aligned_rgb_preview_enabled = aligned_rgb_preview_configured(config);
    if(config.preview.enabled && !aligned_rgb_preview_enabled && !config.preview.aligned_rgb) {
        logger.info("aligned RGB preview disabled by config; using per-camera RGB/depth preview windows");
    }
    if(config.preview.enabled && aligned_rgb_preview_enabled && preview_fps < kAlignedRgbPreviewMinFps) {
        logger.info("aligned RGB preview enabled for cam01/cam02; raising local preview fps from " + std::to_string(preview_fps)
                    + " to " + std::to_string(kAlignedRgbPreviewMinFps)
                    + " target_delta_ms=" + std::to_string(kAlignedRgbPreviewTargetDeltaUs / 1000)
                    + " display_max_delta_ms=" + std::to_string(kAlignedRgbPreviewDisplayMaxDeltaUs / 1000));
        preview_fps = kAlignedRgbPreviewMinFps;
    }
    const auto preview_interval = std::chrono::milliseconds(std::max(1, 1000 / preview_fps));

    const auto started = std::chrono::steady_clock::now();
    std::unique_ptr<ClockSyncClient> clock_sync;
    if(!args.no_send && config.clock_sync.enabled) {
        ClockSyncClientConfig clock_config;
        clock_config.enabled = config.clock_sync.enabled;
        clock_config.receiver_ip = config.clock_sync.receiver_ip.empty() ? config.receiver.ip : config.clock_sync.receiver_ip;
        clock_config.port = config.clock_sync.port;
        clock_config.interval_ms = config.clock_sync.interval_ms;
        clock_config.timeout_ms = config.clock_sync.timeout_ms;
        clock_config.max_delay_us = config.clock_sync.max_delay_us;
        clock_config.sample_window = config.clock_sync.sample_window;
        clock_sync = std::make_unique<ClockSyncClient>(clock_config, config.sender_id);
        clock_sync->set_log_callbacks([&logger](const std::string &message) { logger.info(message); },
                                      [&logger](const std::string &message) { logger.warn(message); });
        clock_sync->start();
        logger.info("clock_sync client enabled receiver=" + clock_config.receiver_ip + ":" + std::to_string(clock_config.port)
                    + " interval_ms=" + std::to_string(clock_config.interval_ms)
                    + " timeout_ms=" + std::to_string(clock_config.timeout_ms));
    }
    auto cameras = start_cameras(config, logger);
    configure_depth_remap_targets(config, cameras, logger);
    ReliableSnapshotQueue rgb_snapshot_queue;
    for(auto &camera : cameras) {
        camera->rgb_snapshot_queue = &rgb_snapshot_queue;
    }
    auto rgb_snapshot_transport = make_media_sender();
    std::atomic<bool> rgb_snapshot_sender_running{true};
    std::thread rgb_snapshot_sender_thread([&] {
        try {
            rgb_snapshot_sender_loop(config, rgb_snapshot_queue, *rgb_snapshot_transport, logger,
                                     rgb_snapshot_sender_running);
        }
        catch(const std::exception &e) {
            logger.warn(std::string("rgb snapshot sender stopped unexpectedly: ") + e.what());
        }
        catch(...) {
            logger.warn("rgb snapshot sender stopped unexpectedly: unknown exception");
        }
    });
    ThreadJoinGuard rgb_snapshot_sender_thread_guard(rgb_snapshot_sender_thread);
    std::vector<std::unique_ptr<MediaSenderPath<MediaSender>>> rgb_media_paths;
    std::vector<std::unique_ptr<MediaSenderPath<MediaSender>>> depth_media_paths;
    rgb_media_paths.reserve(cameras.size());
    depth_media_paths.reserve(cameras.size());
    const size_t rgb_frames_per_slot =
        config.recording_buffer.enabled ? static_cast<size_t>(config.recording_buffer.rgb_frames_per_slot) : 1;
    const size_t depth_frames_per_slot =
        config.recording_buffer.enabled ? static_cast<size_t>(config.recording_buffer.depth_frames_per_slot) : kDepthMediaQueuePerSlot;
    const size_t depth_compression_frames_per_slot = config.recording_buffer.enabled
                                                        ? static_cast<size_t>(config.recording_buffer.depth_compression_frames_per_slot)
                                                        : kDepthCompressionQueuePerSlot;
    logger.info("recording buffer "
                + std::string(config.recording_buffer.enabled ? "enabled" : "disabled")
                + " rgb_frames_per_slot=" + std::to_string(rgb_frames_per_slot)
                + " depth_frames_per_slot=" + std::to_string(depth_frames_per_slot)
                + " depth_compression_frames_per_slot=" + std::to_string(depth_compression_frames_per_slot));

    LatestMediaQueue preview_media_queue(cameras.size() * kMediaSlotsPerCamera);
    LatestDepthCompressionQueue depth_compression_queue(cameras.size() * kMediaSlotsPerCamera, depth_compression_frames_per_slot);
    std::mutex status_transport_mutex;
    std::mutex preview_media_transport_mutex;

    send_status_locked(status_transport, logger, status_transport_mutex, sender_hello(config));
    for(const auto &camera : cameras) {
        bool online = false;
        std::string last_error;
        {
            std::lock_guard<std::mutex> lock(camera->mutex);
            online = camera->online;
            last_error = camera->last_error;
        }
        if(online) {
            send_status_locked(status_transport, logger, status_transport_mutex,
                               event_message(config, "info", "camera_connected", "camera pipeline started", camera->config.camera_id));
        }
        else {
            send_status_locked(status_transport, logger, status_transport_mutex,
                               camera_offline_message(config, camera->config.camera_id, last_error));
        }
    }

    for(size_t i = 0; i < cameras.size(); ++i) {
        rgb_media_paths.push_back(
            start_media_sender_path<MediaSender>(kMediaSlotsPerCamera, rgb_frames_per_slot, depth_frames_per_slot, make_media_sender, logger));
        depth_media_paths.push_back(
            start_media_sender_path<MediaSender>(kMediaSlotsPerCamera, rgb_frames_per_slot, depth_frames_per_slot, make_media_sender, logger));
    }
    std::thread preview_media_thread([&] {
        try {
            media_sender_loop(preview_media_queue, preview_media_transport, logger, preview_media_transport_mutex);
        }
        catch(const std::exception &e) {
            logger.warn(std::string("preview media sender stopped unexpectedly: ") + e.what());
        }
        catch(...) {
            logger.warn("preview media sender stopped unexpectedly: unknown exception");
        }
    });
    ThreadJoinGuard preview_media_thread_guard(preview_media_thread);
    const size_t depth_worker_count = depth_compression_worker_count(cameras.size());
    logger.info("depth compression workers=" + std::to_string(depth_worker_count));
    std::vector<std::thread> depth_compression_threads;
    ThreadVectorJoinGuard depth_compression_threads_guard(depth_compression_threads, [&] { depth_compression_queue.stop(); });
    depth_compression_threads.reserve(depth_worker_count);
    for(size_t worker = 0; worker < depth_worker_count; ++worker) {
        depth_compression_threads.emplace_back([&, worker] {
            try {
                depth_compression_loop(depth_compression_queue, logger, worker);
            }
            catch(const std::exception &e) {
                logger.error("depth compression worker stopped worker=" + std::to_string(worker) + " error=" + e.what());
                g_running = false;
            }
            catch(...) {
                logger.error("depth compression worker stopped worker=" + std::to_string(worker) + " error=unknown exception");
                g_running = false;
            }
        });
    }
    std::vector<std::thread> camera_threads;
    ThreadVectorJoinGuard camera_threads_guard(camera_threads);
    camera_threads.reserve(cameras.size());
    for(size_t i = 0; i < cameras.size(); ++i) {
        auto *rgb_path = rgb_media_paths[i].get();
        auto *depth_path = depth_media_paths[i].get();
        auto *camera = cameras[i].get();
        const size_t primary_slot_base = 0;
        const size_t preview_slot_base = i * kMediaSlotsPerCamera;
        const size_t compression_slot_base = i * kMediaSlotsPerCamera;
        camera_threads.emplace_back([&, camera, rgb_path, depth_path, primary_slot_base, preview_slot_base, compression_slot_base] {
            camera_worker_thread_entry(config, *camera, primary_slot_base, preview_slot_base, compression_slot_base,
                                       rgb_path->queue, depth_path->queue, preview_media_queue, depth_compression_queue,
                                       status_transport, logger, status_transport_mutex, preview_interval);
        });
    }

    logger.info("primary media uses per-camera separate " + config.transport.media_protocol
                + " senders for rgb and depth paths=" + std::to_string(cameras.size() * 2));

    if(config.web_rgb_preview.enabled) {
        logger.info(std::string("web rgb preview media uses separate ")
                    + (config.web_rgb_preview.udp_enabled ? "udp" : "tcp") + " sender");
    }

    auto next_heartbeat = std::chrono::steady_clock::now();
    auto next_camera_announce = std::chrono::steady_clock::now() + kCameraAnnounceInterval;
    auto next_perf_log = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    auto next_preview = std::chrono::steady_clock::now();
    auto next_hotplug_scan = std::chrono::steady_clock::now() + kHotplugScanInterval;
    auto next_hotplug_limit_event = std::chrono::steady_clock::now();
    auto next_rgb_snapshot_poll = std::chrono::steady_clock::now();
    auto next_rgb_snapshot_expiry = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    int next_hotplug_camera_number = initial_hotplug_camera_number(config);
    std::vector<HotplugRetryCooldown> hotplug_retry_cooldowns;
    if(!config.hotplug.enabled) {
        logger.info("hotplug camera scan disabled by config");
    }
    const auto stop_at = args.run_seconds > 0 ? started + std::chrono::seconds(args.run_seconds) : std::chrono::steady_clock::time_point::max();

    while(g_running && std::chrono::steady_clock::now() < stop_at) {
        const auto now = std::chrono::steady_clock::now();
        process_receiver_controls(status_transport, config, cameras, logger, status_transport_mutex);
        poll_rgb_snapshot_requests(config, cameras, logger, now, next_rgb_snapshot_poll);
        if(now >= next_rgb_snapshot_expiry) {
            expire_rgb_snapshot_requests(config, cameras, logger, now);
            next_rgb_snapshot_expiry = now + std::chrono::seconds(1);
        }
        if(now >= next_heartbeat) {
            for(auto &camera : cameras) {
                send_status_locked(status_transport, logger, status_transport_mutex,
                                   camera_heartbeat(config, *camera, started, clock_sync.get()));
            }
            next_heartbeat = now + std::chrono::milliseconds(config.heartbeat_interval_ms);
        }
        if(now >= next_camera_announce) {
            for(auto &camera : cameras) {
                if(camera_online(*camera)) {
                    send_status_locked(status_transport, logger, status_transport_mutex, camera_announce(config, *camera));
                    set_camera_announced(*camera, true);
                }
            }
            next_camera_announce = now + kCameraAnnounceInterval;
        }
        if(now >= next_perf_log) {
            for(auto &camera : cameras) {
                log_perf(*camera, logger, now);
            }
            next_perf_log = now + std::chrono::seconds(1);
        }
        if(config.preview.enabled && now >= next_preview) {
            const auto preview_started = std::chrono::steady_clock::now();
            bool aligned_preview_expected = false;
            bool aligned_preview_drawn = false;
            if(aligned_rgb_preview_enabled) {
                auto *cam01_preview = find_camera_by_id(cameras, "cam01");
                auto *cam02_preview = find_camera_by_id(cameras, "cam02");
                aligned_preview_expected =
                    cam01_preview != nullptr && cam02_preview != nullptr && camera_online(*cam01_preview) && camera_online(*cam02_preview);
                aligned_preview_drawn = preview_aligned_rgb_pair(cameras);
            }
            const double preview_ms = elapsed_ms(preview_started, std::chrono::steady_clock::now());
            if(aligned_preview_drawn) {
                for(auto &camera : cameras) {
                    if(camera && (camera->config.camera_id == "cam01" || camera->config.camera_id == "cam02")) {
                        record_preview_ms(*camera, preview_ms);
                    }
                }
            }
            else if(!aligned_preview_expected) {
                for(auto &camera : cameras) {
                    const auto fallback_preview_started = std::chrono::steady_clock::now();
                    preview_frame(*camera, true);
                    record_preview_ms(*camera, elapsed_ms(fallback_preview_started, std::chrono::steady_clock::now()));
                }
            }
            next_preview = now + preview_interval;
        }
        if(config.hotplug.enabled && now >= next_hotplug_scan) {
            scan_hotplug_cameras(config, cameras, camera_threads, rgb_media_paths, depth_media_paths, preview_media_queue,
                                 depth_compression_queue, rgb_snapshot_queue, make_media_sender, status_transport, logger, status_transport_mutex,
                                 preview_interval, next_hotplug_camera_number, hotplug_retry_cooldowns, next_hotplug_limit_event);
            next_hotplug_scan = now + kHotplugScanInterval;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    if(clock_sync) {
        clock_sync->stop();
    }
    g_running = false;
    for(auto &thread : camera_threads) {
        if(thread.joinable()) {
            thread.join();
        }
    }
    depth_compression_queue.stop();
    for(auto &thread : depth_compression_threads) {
        if(thread.joinable()) {
            thread.join();
        }
    }
    for(auto &path : rgb_media_paths) {
        path->running = false;
        path->queue.stop();
    }
    for(auto &path : depth_media_paths) {
        path->running = false;
        path->queue.stop();
    }
    rgb_snapshot_sender_running = false;
    rgb_snapshot_queue.stop();
    preview_media_queue.stop();
    for(auto &path : rgb_media_paths) {
        if(path->thread.joinable()) {
            path->thread.join();
        }
    }
    for(auto &path : depth_media_paths) {
        if(path->thread.joinable()) {
            path->thread.join();
        }
    }
    if(preview_media_thread.joinable()) {
        preview_media_thread.join();
    }
    if(rgb_snapshot_sender_thread.joinable()) {
        rgb_snapshot_sender_thread.join();
    }

    for(auto &camera : cameras) {
        const auto summary = snapshot_camera_summary(*camera);
        std::ostringstream oss;
        oss << "camera summary camera_id=" << summary.camera_id << " rgb_frames=" << summary.rgb_frames
            << " depth_frames=" << summary.depth_frames << " hardware_encoder=" << (summary.hardware_encoder ? "true" : "false")
            << " depth_scale=" << summary.depth_scale << " disconnects=" << summary.disconnects;
        if(!summary.last_error.empty()) {
            oss << " last_error=" << summary.last_error;
        }
        logger.info(oss.str());
        stop_camera(*camera, logger);
    }
}

}  // namespace

}  // namespace gwv3

int main(int argc, char **argv) {
    using namespace gwv3;
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    try {
        const Args args = parse_args(argc, argv);
        AppConfig config = load_config(args.config_path);
        Logger logger(config.logging.directory, config.logging.max_bytes);

        if(args.validate_only) {
            logger.info("config validation ok: " + args.config_path);
            return 0;
        }

        const std::string sdk_version = std::to_string(ob_get_major_version()) + "." + std::to_string(ob_get_minor_version()) + "."
                                        + std::to_string(ob_get_patch_version());
        logger.info("gemini sender starting, sender_id=" + config.sender_id + ", receiver=" + config.receiver.ip
                    + ", orbbec_sdk=" + sdk_version);
        if(args.no_send) {
            NullTransport transport;
            auto make_media_sender = [] {
                return std::make_unique<NullTransport>();
            };
            run_sender<NullTransport, NullTransport>(config, args, transport, make_media_sender, transport, logger);
        }
        else {
            Transport status_transport(config);
            Transport preview_transport(config);
            auto make_media_sender = [&config] {
                return std::make_unique<Transport>(config);
            };
            run_sender<Transport, Transport>(config, args, status_transport, make_media_sender, preview_transport, logger);
        }
        logger.info("gemini sender stopped");
        return 0;
    }
    catch(const ob::Error &e) {
        std::cerr << "Orbbec SDK error: " << e.getMessage() << std::endl;
        return 2;
    }
    catch(const std::exception &e) {
        std::cerr << "sender error: " << e.what() << std::endl;
        return 1;
    }
}
