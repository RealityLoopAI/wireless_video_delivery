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
    std::unique_ptr<GstH264RtpSender> rgb_rtp_sender;
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
    uint64_t force_rgb_keyframe_last_event_us = 0;
    uint64_t force_rgb_keyframe_events = 0;
    uint64_t force_rgb_keyframe_target_sender_system_us = 0;
    uint64_t force_rgb_keyframe_target_global_us = 0;
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
    std::chrono::steady_clock::time_point next_rgb_rtp_warning = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point last_rgb_frame_at = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point last_depth_frame_at = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point next_capture_stall_reconnect = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point web_rgb_preview_suppressed_until = std::chrono::steady_clock::time_point::min();
    std::chrono::steady_clock::time_point gemini305_manual_exposure_reapply_at = std::chrono::steady_clock::time_point::min();
    std::chrono::steady_clock::time_point next_adaptive_exposure_sample = std::chrono::steady_clock::time_point::min();
    std::chrono::steady_clock::time_point adaptive_exposure_last_evaluation =
        std::chrono::steady_clock::time_point::min();
    std::chrono::steady_clock::time_point next_adaptive_exposure_warning = std::chrono::steady_clock::time_point::min();
    std::chrono::steady_clock::time_point adaptive_exposure_metadata_deadline =
        std::chrono::steady_clock::time_point::min();
    bool adaptive_exposure_waiting_for_metadata = false;
    int adaptive_exposure_discard_frames_remaining = 0;
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

