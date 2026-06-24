#include "gwv3_common/protocol.hpp"
#include "gwv3_sender/config.hpp"
#include "gwv3_sender/gst_h264_encoder.hpp"
#include "gwv3_sender/logger.hpp"
#include "gwv3_sender/transport.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csetjmp>
#include <cstdio>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
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
#include <thread>
#include <vector>

#include <json/json.h>
#include <jpeglib.h>
#include <libobsensor/ObSensor.hpp>
#include <libobsensor/hpp/Error.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <zlib.h>

#include <dlfcn.h>

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
constexpr size_t kMediaSlotsPerCamera = 3;
constexpr size_t kDepthMediaQueuePerSlot = 4;
constexpr size_t kDepthCompressionQueuePerSlot = 4;

const char *stream_type_name(StreamType stream_type) {
    switch(stream_type) {
    case StreamType::rgb:
        return "rgb";
    case StreamType::rgb_preview:
        return "rgb_preview";
    case StreamType::depth_raw:
        return "depth";
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
    uint32_t width = 0;
    uint32_t height = 0;
    RgbFrameDiagnostics diagnostics;
    uint64_t capture_host_timestamp_us = 0;
    uint64_t timing_bound_timestamp_us = 0;
    uint64_t encode_start_timestamp_us = 0;
    uint64_t encode_done_timestamp_us = 0;
    uint64_t packet_queued_timestamp_us = 0;
};

struct CameraRuntime {
    mutable std::mutex mutex;
    CameraConfig config;
    std::string device_serial;
    std::string device_uid;
    std::string device_connection_type;
    std::shared_ptr<ob::Device> device;
    std::unique_ptr<ob::Pipeline> pipeline;
    std::shared_ptr<ob::VideoStreamProfile> color_profile;
    std::shared_ptr<ob::VideoStreamProfile> depth_profile;
    std::unique_ptr<GstH264Encoder> encoder;
    std::unique_ptr<GstH264Encoder> web_preview_encoder;
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
    uint64_t rgb_frames = 0;
    uint64_t depth_frames = 0;
    bool rgb_waiting_for_keyframe_after_transport_loss = false;
    uint64_t rgb_keyframe_guard_drops = 0;
    uint64_t force_rgb_keyframe_requests = 0;
    uint64_t force_rgb_keyframe_applied = 0;
    uint64_t rgb_corrupt_jpeg = 0;
    uint64_t rgb_dropped = 0;
    uint64_t rgb_timing_mismatch_drops = 0;
    uint64_t depth_dropped = 0;
    bool rgb_sent_timing_seen = false;
    uint64_t rgb_last_sent_frame_id = 0;
    uint64_t rgb_last_sent_system_timestamp_us = 0;
    uint32_t media_outage_samples = 0;
    uint32_t capture_stall_samples = 0;
    std::string last_error;
    float depth_scale = 0.0f;
    std::chrono::steady_clock::time_point stats_started = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point next_preview = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point next_web_rgb_preview = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point next_depth_emit = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point next_reconnect = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point next_time_sync_log = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point next_jpeg_warning = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point next_media_warning = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point next_keyframe_guard_warning = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point next_rgb_timing_warning = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point last_rgb_frame_at = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point last_depth_frame_at = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point next_capture_stall_reconnect = std::chrono::steady_clock::now();
    std::string last_media_warning;
    cv::Mat latest_bgr;
    cv::Mat latest_depth_color;
    uint64_t latest_rgb_frame_id = 0;
    uint64_t latest_rgb_system_timestamp_us = 0;
    std::deque<RgbPreviewFrame> rgb_preview_queue;
    std::deque<RgbEncodeTiming> rgb_encode_timings;
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

    MediaPacketView view() const {
        const uint8_t *payload_data = owned_payload.empty() ? external_payload : owned_payload.data();
        const size_t payload_size = owned_payload.empty() ? external_payload_size : owned_payload.size();
        return MediaPacketView{header.data(), header.size(), payload_data, payload_size};
    }

    size_t total_size() const { return header.size() + (owned_payload.empty() ? external_payload_size : owned_payload.size()); }
};

class LatestMediaQueue {
public:
    explicit LatestMediaQueue(size_t slot_count) : slots_(slot_count) {}

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
        bool dropped = false;
        if(job.stream_type == StreamType::depth_raw) {
            while(slot.jobs.size() >= kDepthMediaQueuePerSlot) {
                slot.jobs.pop_front();
                dropped = true;
            }
            slot.jobs.push_back(std::move(job));
        }
        else {
            dropped = occupied;
            slot.jobs.clear();
            slot.jobs.push_back(std::move(job));
        }
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

private:
    struct Slot {
        std::deque<MediaPacketJob> jobs;
    };

    template <typename Predicate>
    std::optional<MediaPacketJob> pop_next_locked(Predicate predicate) {
        for(size_t offset = 0; offset < slots_.size(); ++offset) {
            const size_t index = (next_slot_ + offset) % slots_.size();
            if(!slots_[index].jobs.empty() && predicate(slots_[index].jobs.front().stream_type)) {
                auto job = std::move(slots_[index].jobs.front());
                slots_[index].jobs.pop_front();
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
    size_t next_slot_ = 0;
    bool stopping_ = false;
};

struct DepthCompressionJob {
    CameraRuntime *source_camera = nullptr;
    CameraRuntime *output_camera = nullptr;
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
    explicit LatestDepthCompressionQueue(size_t slot_count) : slots_(slot_count) {}

    void append_slots(size_t slot_count) {
        std::lock_guard<std::mutex> lock(mutex_);
        slots_.resize(slots_.size() + slot_count);
    }

    bool publish(size_t slot_index, DepthCompressionJob &&job) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(stopping_ || slot_index >= slots_.size()) {
            return false;
        }
        auto &slot = slots_[slot_index];
        bool dropped = false;
        while(slot.jobs.size() >= kDepthCompressionQueuePerSlot) {
            slot.jobs.pop_front();
            dropped = true;
        }
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

private:
    struct Slot {
        std::deque<DepthCompressionJob> jobs;
        bool in_flight = false;
    };

    bool has_job_locked() const {
        return std::any_of(slots_.begin(), slots_.end(), [](const auto &slot) { return !slot.jobs.empty() && !slot.in_flight; });
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<Slot> slots_;
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
    Json::Value root;
    std::string errors;
    std::istringstream input(payload);
    if(!Json::parseFromStream(builder, input, &root, &errors) || !root.isObject()) {
        return std::nullopt;
    }
    return root;
}

std::string json_string_or(const Json::Value &value, const char *key, const std::string &fallback = "") {
    return value.isMember(key) && value[key].isString() ? value[key].asString() : fallback;
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

int64_t bool_property_or(const std::shared_ptr<ob::Device> &device, OBPropertyID property_id, int64_t fallback = -1) {
    try {
        if(device && device->isPropertySupported(property_id, OB_PERMISSION_READ)) {
            return device->getBoolProperty(property_id) ? 1 : 0;
        }
    }
    catch(const std::exception &) {
    }
    return fallback;
}

int64_t int_property_or(const std::shared_ptr<ob::Device> &device, OBPropertyID property_id, int64_t fallback = -1) {
    try {
        if(device && device->isPropertySupported(property_id, OB_PERMISSION_READ)) {
            return device->getIntProperty(property_id);
        }
    }
    catch(const std::exception &) {
    }
    return fallback;
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

void apply_color_controls(CameraRuntime &camera, Logger &logger) {
    const auto &controls = camera.config.color_controls;
    if(!controls.auto_exposure && !controls.exposure && !controls.gain && !controls.auto_exposure_priority && !controls.max_exposure
       && !controls.max_gain && !controls.power_line_frequency) {
        return;
    }

    if(controls.auto_exposure && !*controls.auto_exposure) {
        set_bool_property_if_configured(camera, logger, "auto_exposure", OB_PROP_COLOR_AUTO_EXPOSURE_BOOL, controls.auto_exposure);
    }
    set_int_property_if_configured(camera, logger, "auto_exposure_priority", OB_PROP_COLOR_AUTO_EXPOSURE_PRIORITY_INT,
                                   controls.auto_exposure_priority);
    set_int_property_if_configured(camera, logger, "max_exposure", OB_PROP_COLOR_AE_MAX_EXPOSURE_INT, controls.max_exposure);
    set_int_property_if_configured(camera, logger, "max_gain", OB_PROP_COLOR_MAXIMAL_GAIN_INT, controls.max_gain);
    set_int_property_if_configured(camera, logger, "power_line_frequency", OB_PROP_COLOR_POWER_LINE_FREQUENCY_INT,
                                   controls.power_line_frequency);
    set_int_property_if_configured(camera, logger, "exposure", OB_PROP_COLOR_EXPOSURE_INT, controls.exposure);
    set_int_property_if_configured(camera, logger, "gain", OB_PROP_COLOR_GAIN_INT, controls.gain);
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

void update_color_properties(CameraRuntime &camera) {
    if(!camera.device) {
        return;
    }
    camera.live.color_auto_exposure = bool_property_or(camera.device, OB_PROP_COLOR_AUTO_EXPOSURE_BOOL, camera.live.color_auto_exposure);
    camera.live.color_exposure = int_property_or(camera.device, OB_PROP_COLOR_EXPOSURE_INT, camera.live.color_exposure);
    camera.live.color_gain = int_property_or(camera.device, OB_PROP_COLOR_GAIN_INT, camera.live.color_gain);
    camera.live.color_exposure_priority = int_property_or(camera.device, OB_PROP_COLOR_AUTO_EXPOSURE_PRIORITY_INT, camera.live.color_exposure_priority);
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
    const bool depth_stalled = depth_stale_for >= stall_threshold;
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
    const bool depth_stalled = depth_stale_for >= stall_threshold;
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
    if(camera.online) {
        update_color_properties(camera);
    }
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
        << " rgb_corrupt_jpeg_frames=" << perf.rgb_corrupt_jpeg_frames
        << " rgb_timing_mismatch_drops=" << perf.rgb_timing_mismatch_drops
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
        for(uint32_t i = 0; i < count; ++i) {
            const auto param = list->getCameraParam(i);
            Json::Value item = camera_param_data_json(param);
            item["index"] = i;
            raw_list_json.append(item);
            if(profile_dimensions_match(param.rgbIntrinsic, color_profile) && profile_dimensions_match(param.depthIntrinsic, depth_profile)) {
                return param;
            }
        }
    }
    catch(const std::exception &) {
    }
    catch(...) {
    }
    return std::nullopt;
}

Json::Value calibration_json(ob::Pipeline &pipeline, const std::shared_ptr<ob::Device> &device,
                             const std::shared_ptr<ob::VideoStreamProfile> &color_profile,
                             const std::shared_ptr<ob::VideoStreamProfile> &depth_profile) {
    Json::Value calibration;
    calibration["available"] = false;
    calibration["source"] = "orbbec_sdk";
    calibration["data"] = Json::objectValue;
    Json::Value raw_list(Json::arrayValue);
    try {
        OBCameraParam camera_param{};
        bool has_profile_param = false;
        if(color_profile && depth_profile) {
            try {
                camera_param = pipeline.getCameraParamWithProfile(color_profile->width(), color_profile->height(), depth_profile->width(),
                                                                  depth_profile->height());
                has_profile_param = true;
            }
            catch(const std::exception &) {
            }
            catch(...) {
            }
        }
        fill_missing_profile_calibration(camera_param, color_profile, depth_profile);
        const auto exact_param = exact_profile_camera_param_from_device(device, color_profile, depth_profile, raw_list);
        if(exact_param && !camera_param_complete(camera_param)) {
            camera_param = *exact_param;
            has_profile_param = true;
            calibration["source_detail"] = "device_calibration_list_exact_profile";
        }
        if(!has_profile_param && !camera_param_complete(camera_param)) {
            camera_param = pipeline.getCameraParam();
            fill_missing_profile_calibration(camera_param, color_profile, depth_profile);
        }

        Json::Value data = camera_param_data_json(camera_param);
        if(!raw_list.empty()) {
            data["raw_camera_param_list"] = raw_list;
        }
        const bool depth_ok = intrinsic_valid(camera_param.depthIntrinsic);
        const bool rgb_ok = intrinsic_valid(camera_param.rgbIntrinsic);
        const bool transform_ok = transform_valid(camera_param.transform);
        calibration["available"] = depth_ok && rgb_ok && transform_ok;
        calibration["profile_aware"] = has_profile_param;
        if(!depth_ok || !rgb_ok || !transform_ok) {
            calibration["warning"] = "incomplete_calibration";
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
    std::string serial;
    std::string uid;
    std::string paired_rgb_serial;
    std::string connection_type;
};

std::vector<OrbbecDeviceIdentity> enumerate_orbbec_devices(ob::Context &ctx) {
    std::vector<OrbbecDeviceIdentity> identities;
    auto devices = ctx.queryDeviceList();
    identities.reserve(devices->deviceCount());
    for(uint32_t i = 0; i < devices->deviceCount(); ++i) {
        OrbbecDeviceIdentity identity;
        identity.index = i;
        identity.serial = devices->serialNumber(i) ? devices->serialNumber(i) : "";
        identity.uid = devices->uid(i) ? devices->uid(i) : "";
        identity.paired_rgb_serial = paired_rgb_serial_for_depth_uid(identity.uid);
        identity.connection_type = devices->connectionType(i) ? devices->connectionType(i) : "";
        identities.push_back(std::move(identity));
    }
    return identities;
}

bool device_identity_matches_config(const OrbbecDeviceIdentity &identity, const CameraConfig &camera) {
    if(!camera.uid.empty() && usb_uid_matches(camera.uid, identity.uid)) {
        return true;
    }
    if(!camera.serial_number.empty()
       && (identity.serial == camera.serial_number || identity.paired_rgb_serial == camera.serial_number)) {
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
    oss << "index=" << identity.index << " serial=" << identity.serial << " uid=" << identity.uid
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
        oss << "index=" << i << " serial=" << (devices->serialNumber(i) ? devices->serialNumber(i) : "")
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
    if(!camera.uid.empty()) {
        for(uint32_t i = 0; i < devices->deviceCount(); ++i) {
            const std::string serial = devices->serialNumber(i) ? devices->serialNumber(i) : "";
            const std::string uid = devices->uid(i) ? devices->uid(i) : "";
            const std::string paired_rgb_serial = paired_rgb_serial_for_depth_uid(uid);
            if(!usb_uid_matches(camera.uid, uid)) {
                continue;
            }
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

bool mjpg_has_complete_jpeg(const std::shared_ptr<ob::ColorFrame> &frame) {
    if(!frame || !frame->data() || frame->dataSize() < 4) {
        return false;
    }
    const auto *data = static_cast<const uint8_t *>(frame->data());
    const size_t size = frame->dataSize();
    if(data[0] != 0xff || data[1] != 0xd8) {
        return false;
    }

    size_t end = size;
    while(end > 0 && data[end - 1] == 0x00) {
        --end;
    }
    return end >= 4 && data[end - 2] == 0xff && data[end - 1] == 0xd9;
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

bool mjpg_decodes_cleanly(const std::shared_ptr<ob::ColorFrame> &frame, std::string &message) {
    if(!mjpg_has_complete_jpeg(frame)) {
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
    jpeg_mem_src(&cinfo, static_cast<const unsigned char *>(frame->data()), static_cast<unsigned long>(frame->dataSize()));
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
        if(!camera.rgb_waiting_for_keyframe_after_transport_loss) {
            should_log = true;
            camera.rgb_keyframe_guard_drops = 0;
        }
        camera.rgb_waiting_for_keyframe_after_transport_loss = true;
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

enum class RgbKeyframeDecision {
    send,
    drop,
    send_after_drop,
};

RgbKeyframeDecision decide_rgb_keyframe_send(CameraRuntime &camera, bool is_keyframe, std::chrono::steady_clock::time_point now,
                                             Logger &logger) {
    bool drop = false;
    bool log_drop = false;
    bool log_recovered = false;
    uint64_t dropped = 0;
    {
        std::lock_guard<std::mutex> lock(camera.mutex);
        if(!camera.rgb_waiting_for_keyframe_after_transport_loss) {
            return RgbKeyframeDecision::send;
        }
        if(is_keyframe) {
            log_recovered = true;
            dropped = camera.rgb_keyframe_guard_drops;
            camera.rgb_keyframe_guard_drops = 0;
            camera.rgb_waiting_for_keyframe_after_transport_loss = false;
        }
        else {
            drop = true;
            camera.rgb_dropped++;
            camera.rgb_keyframe_guard_drops++;
            dropped = camera.rgb_keyframe_guard_drops;
            if(now >= camera.next_keyframe_guard_warning) {
                camera.next_keyframe_guard_warning = now + std::chrono::seconds(1);
                log_drop = true;
            }
        }
    }
    if(log_drop) {
        logger.warn("rgb keyframe guard dropping non-IDR camera_id=" + camera.config.camera_id + " dropped=" + std::to_string(dropped));
    }
    if(log_recovered) {
        logger.info("rgb keyframe guard recovered camera_id=" + camera.config.camera_id + " dropped=" + std::to_string(dropped));
    }
    return drop ? RgbKeyframeDecision::drop : RgbKeyframeDecision::send_after_drop;
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
        }
        else if(stream_type == StreamType::rgb_preview) {
            camera.perf.rgb_preview_send_ms += send_ms;
            camera.perf.rgb_preview_send_failures++;
        }
        else if(stream_type == StreamType::depth_raw) {
            camera.perf.depth_send_ms += send_ms;
            camera.perf.depth_send_failures++;
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
    const auto interval = frame_interval_for_fps(camera.config.depth_profile.fps);
    if(interval <= std::chrono::steady_clock::duration::zero()) {
        return true;
    }

    std::lock_guard<std::mutex> lock(camera.mutex);
    if(now < camera.next_depth_emit) {
        return false;
    }
    camera.next_depth_emit += interval;
    if(camera.next_depth_emit <= now) {
        camera.next_depth_emit = now + interval;
    }
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
    if(now < camera.next_web_rgb_preview) {
        return false;
    }
    camera.next_web_rgb_preview += interval;
    if(camera.next_web_rgb_preview <= now) {
        camera.next_web_rgb_preview = now + interval;
    }
    return true;
}

void set_latest_bgr(CameraRuntime &camera, const cv::Mat &bgr, uint64_t frame_id, uint64_t system_timestamp_us) {
    if(bgr.empty()) {
        return;
    }
    const auto preview_bgr = bgr.clone();
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

    std::vector<CompressedChunk> chunks;
    chunks.reserve(chunk_count);
    for(size_t offset = 0; offset < sample_count; offset += kSamplesPerChunk) {
        const size_t count = std::min(kSamplesPerChunk, sample_count - offset);
        std::vector<uint8_t> quantized(count);
        quantize_depth8_into(samples + offset, count, raw_step, quantized.data());

        const auto bound = compressBound(static_cast<uLong>(quantized.size()));
        CompressedChunk chunk;
        chunk.sample_offset = static_cast<uint32_t>(offset);
        chunk.sample_count = static_cast<uint32_t>(count);
        chunk.payload.resize(bound);
        uLongf out_size = bound;
        const int rc = compress2(chunk.payload.data(), &out_size, quantized.data(), static_cast<uLong>(quantized.size()), Z_BEST_SPEED);
        if(rc != Z_OK) {
            throw std::runtime_error("pq8zlib depth compression failed");
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

    std::vector<CompressedChunk> chunks;
    chunks.reserve(chunk_count);
    for(size_t offset = 0; offset < sample_count; offset += kSamplesPerChunk) {
        const size_t count = std::min(kSamplesPerChunk, sample_count - offset);
        std::vector<uint8_t> quantized(count);
        quantize_depth8_into(samples + offset, count, raw_step, quantized.data());

        CompressedChunk chunk;
        chunk.sample_offset = static_cast<uint32_t>(offset);
        chunk.sample_count = static_cast<uint32_t>(count);
        chunk.payload = lz4_compress_payload(quantized.data(), quantized.size());
        if(chunk.payload.size() > static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
            throw std::runtime_error("pq8lz4 depth chunk too large");
        }
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
    capabilities["hotplug"] = config.hotplug.enabled;
    capabilities["media_protocol"] = config.transport.media_protocol;
    capabilities["status_protocol"] = config.transport.status_protocol;
    msg["capabilities"] = capabilities;
    return msg;
}

Json::Value camera_announce(const AppConfig &config, CameraRuntime &camera) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    Json::Value msg = base_message(config, "camera_announce");
    msg["camera_id"] = camera.config.camera_id;
    auto info = camera.device->getDeviceInfo();
    Json::Value device;
    device["vendor"] = "orbbec";
    device["model"] = info->name() ? info->name() : "";
    device["serial_number"] = info->serialNumber() ? info->serialNumber() : "";
    device["uid"] = info->uid() ? info->uid() : "";
    device["paired_rgb_serial"] = paired_rgb_serial_for_depth_uid(device["uid"].asString());
    device["configured_serial_number"] = camera.config.serial_number;
    device["configured_uid"] = camera.config.uid;
    device["firmware_version"] = info->firmwareVersion() ? info->firmwareVersion() : "";
    device["connection_type"] = info->connectionType() ? info->connectionType() : "";
    msg["device"] = device;
    msg["rgb_profile"] = profile_json(camera.color_profile, "encoded_video", camera.config.rgb_encoding.codec);
    msg["depth_profile"] = profile_json(camera.depth_profile, "uint16", camera.config.depth_transport.compression, camera.depth_scale);
    Json::Value web_preview;
    web_preview["enabled"] = config.web_rgb_preview.enabled;
    web_preview["max_width"] = config.web_rgb_preview.max_width;
    web_preview["max_height"] = config.web_rgb_preview.max_height;
    web_preview["fps"] = config.web_rgb_preview.fps;
    web_preview["bitrate_bps"] = config.web_rgb_preview.bitrate_bps;
    web_preview["width"] = Json::UInt(camera.web_preview_width);
    web_preview["height"] = Json::UInt(camera.web_preview_height);
    msg["web_rgb_preview"] = web_preview;
    msg["calibration"] = calibration_json(*camera.pipeline, camera.device, camera.color_profile, camera.depth_profile);
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

Json::Value camera_heartbeat(const AppConfig &config, CameraRuntime &camera, std::chrono::steady_clock::time_point started) {
    Json::Value msg = base_message(config, "heartbeat");
    const auto uptime = std::chrono::steady_clock::now() - started;
    msg["uptime_ms"] = Json::UInt64(std::chrono::duration_cast<std::chrono::milliseconds>(uptime).count());
    std::lock_guard<std::mutex> lock(camera.mutex);
    const auto seconds = std::max(0.001, std::chrono::duration<double>(std::chrono::steady_clock::now() - camera.stats_started).count());
    msg["camera_id"] = camera.config.camera_id;
    msg["online"] = camera.online;
    msg["rgb_fps"] = static_cast<double>(camera.rgb_frames) / seconds;
    msg["depth_fps"] = static_cast<double>(camera.depth_frames) / seconds;
    msg["rgb_encoding"] = camera.config.rgb_encoding.codec;
    msg["depth_compression"] = camera.config.depth_transport.compression;
    msg["rgb_dropped_frames"] = Json::UInt64(camera.rgb_dropped);
    msg["rgb_corrupt_jpeg_frames"] = Json::UInt64(camera.rgb_corrupt_jpeg);
    msg["depth_dropped_frames"] = Json::UInt64(camera.depth_dropped);
    msg["hardware_encoder"] = camera.hardware_encoder;
    msg["rgb_measured_fps"] = camera.live.rgb_input_fps;
    msg["depth_measured_fps"] = camera.live.depth_input_fps;
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

CameraRuntime *find_camera_by_id(const std::vector<std::unique_ptr<CameraRuntime>> &cameras, const std::string &camera_id) {
    for(const auto &camera : cameras) {
        if(camera && camera->config.camera_id == camera_id) {
            return camera.get();
        }
    }
    return nullptr;
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
    if(control != "force_rgb_keyframe") {
        logger.warn("unknown receiver control ignored: " + control);
        return;
    }

    size_t matched = 0;
    const bool all_cameras = target_camera.empty() || target_camera == "*";
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

std::chrono::milliseconds reconnect_delay(uint32_t attempts) {
    return std::chrono::seconds(std::min<uint32_t>(5, std::max<uint32_t>(1, attempts)));
}

void stop_camera(CameraRuntime &camera, Logger &logger) {
    std::lock_guard<std::mutex> lifecycle_lock(g_camera_lifecycle_mutex);
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
    camera.web_preview_width = 0;
    camera.web_preview_height = 0;
    camera.pipeline.reset();
    camera.color_profile.reset();
    camera.depth_profile.reset();
    camera.device.reset();
    camera.device_serial.clear();
    camera.device_uid.clear();
    camera.device_connection_type.clear();
    camera.online = false;
    camera.announced = false;
    camera.hardware_encoder = false;
    camera.rgb_sent_timing_seen = false;
    camera.rgb_last_sent_frame_id = 0;
    camera.rgb_last_sent_system_timestamp_us = 0;
    camera.rgb_encode_timings.clear();
    camera.latest_bgr.release();
    camera.latest_depth_color.release();
}

void start_camera_runtime(CameraRuntime &runtime, Logger &logger) {
    std::lock_guard<std::mutex> lifecycle_lock(g_camera_lifecycle_mutex);
    std::lock_guard<std::mutex> lock(runtime.mutex);
    ob::Context::setLoggerSeverity(OB_LOG_SEVERITY_WARN);
    auto context = std::make_shared<ob::Context>();

    runtime.device = select_device(*context, runtime.config);
    auto device_info = runtime.device->getDeviceInfo();
    runtime.device_serial = device_info->serialNumber() ? device_info->serialNumber() : "";
    runtime.device_uid = device_info->uid() ? device_info->uid() : "";
    runtime.device_connection_type = device_info->connectionType() ? device_info->connectionType() : "";
    apply_color_controls(runtime, logger);
    runtime.pipeline = std::make_unique<ob::Pipeline>(runtime.device);

    auto stream_config = std::make_shared<ob::Config>();
    runtime.color_profile = select_profile(*runtime.pipeline, OB_SENSOR_COLOR, runtime.config.rgb_profile, OB_FORMAT_MJPG, logger);
    runtime.depth_profile = select_profile(*runtime.pipeline, OB_SENSOR_DEPTH, runtime.config.depth_profile, OB_FORMAT_Y16, logger);
    stream_config->enableStream(runtime.color_profile);
    stream_config->enableStream(runtime.depth_profile);
    stream_config->setFrameAggregateOutputMode(OB_FRAME_AGGREGATE_OUTPUT_ANY_SITUATION);
    runtime.pipeline->start(stream_config);
    update_color_properties(runtime);

    const auto now = std::chrono::steady_clock::now();
    runtime.encoder.reset();
    runtime.web_preview_encoder.reset();
    runtime.web_preview_width = 0;
    runtime.web_preview_height = 0;
    runtime.online = true;
    runtime.announced = false;
    runtime.hardware_encoder = false;
    runtime.depth_scale = 0.0f;
    runtime.last_error.clear();
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
    if(runtime.next_capture_stall_reconnect < now) {
        runtime.next_capture_stall_reconnect = now;
    }
    runtime.capture_stall_samples = 0;
    runtime.last_media_warning.clear();
    runtime.rgb_sent_timing_seen = false;
    runtime.rgb_last_sent_frame_id = 0;
    runtime.rgb_last_sent_system_timestamp_us = 0;
    runtime.rgb_encode_timings.clear();
    runtime.latest_bgr.release();
    runtime.latest_depth_color.release();

    std::ostringstream oss;
    oss << "camera started camera_id=" << runtime.config.camera_id << " color=" << runtime.color_profile->width() << "x"
        << runtime.color_profile->height() << "@" << runtime.color_profile->fps() << " format=" << ob_format_name(runtime.color_profile->format())
        << " depth=" << runtime.depth_profile->width() << "x" << runtime.depth_profile->height() << "@" << runtime.depth_profile->fps()
        << " format=" << ob_format_name(runtime.depth_profile->format())
        << " configured_serial=" << runtime.config.serial_number
        << " configured_uid=" << runtime.config.uid
        << " device_serial=" << runtime.device_serial
        << " device_uid=" << runtime.device_uid
        << " paired_rgb_serial=" << paired_rgb_serial_for_depth_uid(runtime.device_uid)
        << " connection=" << runtime.device_connection_type;
    logger.info(oss.str());
}

std::vector<std::unique_ptr<CameraRuntime>> start_cameras(const AppConfig &config, Logger &logger) {
    std::vector<std::unique_ptr<CameraRuntime>> cameras;

    for(const auto &camera_config : config.cameras) {
        auto runtime = std::make_unique<CameraRuntime>();
        runtime->config = camera_config;
        try {
            start_camera_runtime(*runtime, logger);
        }
        catch(const ob::Error &e) {
            stop_camera(*runtime, logger);
            runtime->last_error = ob_error_text(e);
            runtime->next_reconnect = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            logger.warn("camera unavailable at startup camera_id=" + runtime->config.camera_id + " error=" + runtime->last_error
                        + "; continuing with remaining cameras");
        }
        catch(const std::exception &e) {
            stop_camera(*runtime, logger);
            runtime->last_error = e.what();
            runtime->next_reconnect = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            logger.warn("camera unavailable at startup camera_id=" + runtime->config.camera_id + " error=" + runtime->last_error
                        + "; continuing with remaining cameras");
        }
        cameras.push_back(std::move(runtime));
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
    return job;
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
    {
        std::lock_guard<std::mutex> lock(camera.mutex);
        camera.disconnects++;
        camera.reconnect_attempts = 0;
        camera.last_error = error;
    }
    logger.error("camera disconnected camera_id=" + camera.config.camera_id + " error=" + error);
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
    }
}

template <typename Sender>
void media_sender_loop(LatestMediaQueue &media_queue, Sender &transport, Logger &logger, std::mutex &transport_mutex) {
    while(g_running) {
        auto job = media_queue.wait_pop(std::chrono::milliseconds(100));
        if(!job || !job->camera) {
            continue;
        }

        const auto send_started = std::chrono::steady_clock::now();
        bool sent = false;
        std::string error;
        const auto packet = job->view();
        {
            std::lock_guard<std::mutex> lock(transport_mutex);
            sent = transport.send_media(packet);
            if(!sent) {
                error = transport.last_error();
            }
        }
        const auto send_ended = std::chrono::steady_clock::now();
        const double send_ms = elapsed_ms(send_started, send_ended);
        if(sent) {
            record_media_send_success(*job->camera, job->stream_type, job->total_size(), send_ms);
        }
        else {
            record_media_send_failure(*job->camera, logger, job->stream_type, send_ms, send_ended, error);
            if(job->stream_type == StreamType::rgb) {
                arm_rgb_keyframe_guard(*job->camera, logger, error);
            }
        }
    }
}

void depth_compression_loop(LatestDepthCompressionQueue &depth_queue, LatestMediaQueue &main_media_queue, Logger &logger, size_t worker_index) {
    while(g_running) {
        auto work = depth_queue.wait_pop(std::chrono::milliseconds(100));
        if(!work) {
            continue;
        }
        auto &job = work->job;
        if(!job.source_camera || !job.output_camera) {
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
            publish_media_job(main_media_queue, job.media_slot_index, *job.output_camera, StreamType::depth_raw, std::move(media_job), logger);
            record_depth_frame_done(*job.output_camera);
        }
        catch(const std::exception &e) {
            set_camera_last_error(*job.source_camera, e.what());
            logger.warn("depth compression failed worker=" + std::to_string(worker_index)
                        + " camera_id=" + job.source_camera->config.camera_id + " error=" + e.what());
        }
        depth_queue.complete(work->slot_index);
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
void camera_worker_loop(const AppConfig &config, CameraRuntime &camera, size_t slot_base, LatestMediaQueue &main_media_queue,
                        LatestMediaQueue &preview_media_queue, LatestDepthCompressionQueue &depth_compression_queue, Sender &transport, Logger &logger,
                        std::mutex &transport_mutex, std::chrono::milliseconds preview_interval) {
    const size_t rgb_slot = slot_base;
    const size_t depth_slot = slot_base + 1;
    const size_t rgb_preview_slot = slot_base + 2;

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
            const uint64_t rgb_system_timestamp_us = frame_system_timestamp_us_or(color, frame_host_now_us);
            const bool color_is_mjpg = color->format() == OB_FORMAT_MJPG;
            RgbEncodeTiming rgb_capture_timing{color->index(), rgb_device_timestamp_us, rgb_system_timestamp_us,
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
                if(color_is_mjpg && preview_due) {
                    const auto decode_started = std::chrono::steady_clock::now();
                    auto preview_bgr = color_to_preview_bgr(color);
                    record_rgb_decode_ms(camera, elapsed_ms(decode_started, std::chrono::steady_clock::now()));
                    set_latest_bgr(camera, preview_bgr, color->index(), rgb_system_timestamp_us);
                }
                else if(!color_is_mjpg) {
                    const auto decode_started = std::chrono::steady_clock::now();
                    bgr = color_to_bgr(color);
                    record_rgb_decode_ms(camera, elapsed_ms(decode_started, std::chrono::steady_clock::now()));
                    if(preview_due) {
                        set_latest_bgr(camera, bgr, color->index(), rgb_system_timestamp_us);
                    }
                }

                if(!camera.encoder && (color_is_mjpg || !bgr.empty())) {
                    auto input_format = color_is_mjpg ? GstH264InputFormat::Jpeg : GstH264InputFormat::Bgr;
                    const auto color_profile = camera_color_profile(camera);
                    if(!color_profile) {
                        set_camera_last_error(camera, "missing rgb stream profile");
                    }
                    else {
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

                if(web_preview_due && !camera.web_preview_encoder && (color_is_mjpg || !bgr.empty())) {
                    auto input_format = color_is_mjpg ? GstH264InputFormat::Jpeg : GstH264InputFormat::Bgr;
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

                const bool web_preview_can_queue = web_preview_due && !main_media_queue.has_pending_primary();
                if(camera.encoder && camera.encoder->ok()) {
                    try {
                        const auto input_format = encoder_input_format_for(camera);
                        if(input_format == GstH264InputFormat::Bgr && bgr.empty()) {
                            const auto decode_started = std::chrono::steady_clock::now();
                            bgr = color_to_bgr(color);
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
                            for(auto &encoded : encoded_units) {
                                const bool encoded_has_vcl = h264_payload_has_vcl_nal(encoded.data);
                                const bool is_key_frame = h264_payload_has_idr(encoded.data);
                                const auto send_decision = decide_rgb_keyframe_send(camera, is_key_frame, frame_now, logger);
                                if(send_decision == RgbKeyframeDecision::drop) {
                                    continue;
                                }
                                const auto timing_resolution = resolve_rgb_encode_timing(camera, encoded, submitted_timing, encoded_has_vcl);
                                maybe_log_rgb_timing_resolution(camera, logger, encoded, timing_resolution, frame_now);
                                auto timing = timing_resolution.timing;
                                if(timing.encode_start_timestamp_us == 0) {
                                    timing.encode_start_timestamp_us = submitted_timing.encode_start_timestamp_us;
                                }
                                timing.encode_done_timestamp_us = encode_done_timestamp_us;
                                if(encoded_has_vcl && !rgb_encoded_timing_is_monotonic(camera, logger, timing, frame_now)) {
                                    continue;
                                }
                                MediaFrameMeta meta;
                                meta.stream_type = StreamType::rgb;
                                meta.flags = has_system_timestamp | has_rgb_diagnostics | (is_key_frame ? key_frame : 0u);
                                if(send_decision == RgbKeyframeDecision::send_after_drop) {
                                    meta.flags |= dropped_before;
                                }
                                meta.sender_id = config.sender_id;
                                meta.camera_id = camera.config.camera_id;
                                meta.codec_or_compression = "h264";
                                meta.frame_id = timing.frame_id;
                                meta.timestamp_us = timing.device_timestamp_us;
                                meta.system_timestamp_us = timing.system_timestamp_us;
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
                                bool reject_if_occupied = !is_key_frame;
                                auto job = make_owned_media_job(camera, StreamType::rgb, meta, std::move(encoded.data));
                                if(publish_media_job(main_media_queue, rgb_slot, camera, StreamType::rgb, std::move(job), logger,
                                                     reject_if_occupied, is_key_frame)
                                   && encoded_has_vcl) {
                                    mark_rgb_encoded_timing_queued(camera, timing);
                                }
                            }
                            if(web_preview_can_queue && camera.web_preview_encoder && camera.web_preview_encoder->ok()) {
                                const auto preview_input_format = web_preview_encoder_input_format_for(camera);
                                if(preview_input_format == GstH264InputFormat::Bgr && bgr.empty()) {
                                    const auto decode_started = std::chrono::steady_clock::now();
                                    bgr = color_to_bgr(color);
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
                                        meta.width = static_cast<uint32_t>(camera.web_preview_encoder->output_width());
                                        meta.height = static_cast<uint32_t>(camera.web_preview_encoder->output_height());
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
            auto *depth_target = depth_target_camera(config, camera);
            depth_output_camera = depth_target;
            record_depth_input(camera, depth);
            set_depth_scale_if_empty(camera, depth->getValueScale());
            set_depth_scale_if_empty(*depth_target, depth->getValueScale());
            const bool publish_depth = depth_emit_due(camera, frame_now);
            if(publish_depth) {
                const void *depth_payload = depth->data();
                size_t depth_payload_size = depth->dataSize();
                const uint64_t depth_device_timestamp_us = depth->timeStampUs();
                const uint64_t depth_system_timestamp_us = frame_system_timestamp_us_or(depth, frame_host_now_us);
                MediaFrameMeta meta;
                meta.stream_type = StreamType::depth_raw;
                meta.flags = has_system_timestamp;
                meta.sender_id = config.sender_id;
                meta.camera_id = depth_target->config.camera_id;
                meta.codec_or_compression = camera.config.depth_transport.compression;
                meta.frame_id = depth->index();
                meta.timestamp_us = depth_device_timestamp_us;
                meta.system_timestamp_us = depth_system_timestamp_us;
                meta.width = depth->width();
                meta.height = depth->height();
                meta.pixel_format = PixelFormat::depth_u16;
                meta.payload_size = depth_payload_size;
                meta.uncompressed_size = depth->dataSize();
                if(depth_transport_uses_compression(camera.config.depth_transport.compression)) {
                    DepthCompressionJob compress_job;
                    compress_job.source_camera = &camera;
                    compress_job.output_camera = depth_target;
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
                    if(depth_compression_queue.publish(depth_slot, std::move(compress_job))) {
                        record_queue_overwrite(*depth_target, StreamType::depth_raw);
                    }
                }
                else {
                    MediaPacketJob job;
                    auto owner = std::shared_ptr<const void>(depth, static_cast<const void *>(depth->data()));
                    job = make_external_media_job(*depth_target, StreamType::depth_raw, meta, depth_payload, depth_payload_size, std::move(owner));
                    publish_media_job(main_media_queue, depth_slot, *depth_target, StreamType::depth_raw, std::move(job), logger);
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

        if(!camera_announced(camera) && depth && color) {
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
void scan_hotplug_cameras(const AppConfig &config, std::vector<std::unique_ptr<CameraRuntime>> &cameras,
                          std::vector<std::thread> &camera_threads, LatestMediaQueue &main_media_queue,
                          LatestMediaQueue &preview_media_queue, LatestDepthCompressionQueue &depth_compression_queue, Sender &transport, Logger &logger,
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

        const size_t slot_base = main_media_queue.append_slots(kMediaSlotsPerCamera);
        preview_media_queue.append_slots(kMediaSlotsPerCamera);
        depth_compression_queue.append_slots(kMediaSlotsPerCamera);
        CameraRuntime *camera_ptr = runtime.get();
        cameras.push_back(std::move(runtime));
        camera_threads.emplace_back([&, camera_ptr, slot_base] {
            camera_worker_loop(config, *camera_ptr, slot_base, main_media_queue, preview_media_queue, depth_compression_queue, transport, logger,
                               transport_mutex, preview_interval);
        });
        logger.info("hotplug camera started camera_id=" + camera_id + " slot_base=" + std::to_string(slot_base)
                    + " device=" + device_identity_summary(identity));
        send_status_locked(transport, logger, transport_mutex,
                           event_message(config, "info", "camera_connected", "hotplug camera pipeline started", camera_id));
        ++next_hotplug_camera_number;
    }
}

template <typename StatusSender, typename MainMediaSender, typename PreviewMediaSender>
void run_sender(AppConfig config, const Args &args, StatusSender &status_transport, MainMediaSender &main_media_transport,
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
    auto cameras = start_cameras(config, logger);
    configure_depth_remap_targets(config, cameras, logger);
    LatestMediaQueue main_media_queue(cameras.size() * kMediaSlotsPerCamera);
    LatestMediaQueue preview_media_queue(cameras.size() * kMediaSlotsPerCamera);
    LatestDepthCompressionQueue depth_compression_queue(cameras.size() * kMediaSlotsPerCamera);
    std::mutex main_transport_mutex;
    std::mutex preview_media_transport_mutex;

    send_status_locked(status_transport, logger, main_transport_mutex, sender_hello(config));
    for(const auto &camera : cameras) {
        bool online = false;
        std::string last_error;
        {
            std::lock_guard<std::mutex> lock(camera->mutex);
            online = camera->online;
            last_error = camera->last_error;
        }
        if(online) {
            send_status_locked(status_transport, logger, main_transport_mutex,
                               event_message(config, "info", "camera_connected", "camera pipeline started", camera->config.camera_id));
        }
        else {
            send_status_locked(status_transport, logger, main_transport_mutex,
                               camera_offline_message(config, camera->config.camera_id, last_error));
        }
    }

    std::thread main_media_thread([&] { media_sender_loop(main_media_queue, main_media_transport, logger, main_transport_mutex); });
    std::thread preview_media_thread([&] { media_sender_loop(preview_media_queue, preview_media_transport, logger, preview_media_transport_mutex); });
    const size_t depth_worker_count = depth_compression_worker_count(cameras.size());
    logger.info("depth compression workers=" + std::to_string(depth_worker_count));
    std::vector<std::thread> depth_compression_threads;
    depth_compression_threads.reserve(depth_worker_count);
    for(size_t worker = 0; worker < depth_worker_count; ++worker) {
        depth_compression_threads.emplace_back([&, worker] { depth_compression_loop(depth_compression_queue, main_media_queue, logger, worker); });
    }
    std::vector<std::thread> camera_threads;
    camera_threads.reserve(cameras.size());
    for(size_t i = 0; i < cameras.size(); ++i) {
        const size_t slot_base = i * kMediaSlotsPerCamera;
        camera_threads.emplace_back([&, i, slot_base] {
            camera_worker_loop(config, *cameras[i], slot_base, main_media_queue, preview_media_queue, depth_compression_queue,
                               status_transport, logger, main_transport_mutex, preview_interval);
        });
    }

    if(config.web_rgb_preview.enabled) {
        logger.info("web rgb preview media uses separate tcp sender");
    }

    auto next_heartbeat = std::chrono::steady_clock::now();
    auto next_camera_announce = std::chrono::steady_clock::now() + kCameraAnnounceInterval;
    auto next_perf_log = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    auto next_preview = std::chrono::steady_clock::now();
    auto next_hotplug_scan = std::chrono::steady_clock::now() + kHotplugScanInterval;
    auto next_hotplug_limit_event = std::chrono::steady_clock::now();
    int next_hotplug_camera_number = initial_hotplug_camera_number(config);
    std::vector<HotplugRetryCooldown> hotplug_retry_cooldowns;
    if(!config.hotplug.enabled) {
        logger.info("hotplug camera scan disabled by config");
    }
    const auto stop_at = args.run_seconds > 0 ? started + std::chrono::seconds(args.run_seconds) : std::chrono::steady_clock::time_point::max();

    while(g_running && std::chrono::steady_clock::now() < stop_at) {
        const auto now = std::chrono::steady_clock::now();
        process_receiver_controls(status_transport, config, cameras, logger, main_transport_mutex);
        if(now >= next_heartbeat) {
            for(auto &camera : cameras) {
                send_status_locked(status_transport, logger, main_transport_mutex, camera_heartbeat(config, *camera, started));
            }
            next_heartbeat = now + std::chrono::milliseconds(config.heartbeat_interval_ms);
        }
        if(now >= next_camera_announce) {
            for(auto &camera : cameras) {
                if(camera_online(*camera)) {
                    send_status_locked(status_transport, logger, main_transport_mutex, camera_announce(config, *camera));
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
            scan_hotplug_cameras(config, cameras, camera_threads, main_media_queue, preview_media_queue, depth_compression_queue,
                                 status_transport, logger, main_transport_mutex, preview_interval, next_hotplug_camera_number,
                                 hotplug_retry_cooldowns, next_hotplug_limit_event);
            next_hotplug_scan = now + kHotplugScanInterval;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    g_running = false;
    depth_compression_queue.stop();
    main_media_queue.stop();
    preview_media_queue.stop();
    for(auto &thread : camera_threads) {
        if(thread.joinable()) {
            thread.join();
        }
    }
    for(auto &thread : depth_compression_threads) {
        if(thread.joinable()) {
            thread.join();
        }
    }
    if(main_media_thread.joinable()) {
        main_media_thread.join();
    }
    if(preview_media_thread.joinable()) {
        preview_media_thread.join();
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

        logger.info("gemini sender starting, sender_id=" + config.sender_id + ", receiver=" + config.receiver.ip);
        if(args.no_send) {
            NullTransport transport;
            run_sender(config, args, transport, transport, transport, logger);
        }
        else {
            Transport transport(config);
            Transport preview_transport(config);
            run_sender(config, args, transport, transport, preview_transport, logger);
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
