#include "gwv3_common/protocol.hpp"
#include "gwv3_sender/config.hpp"
#include "gwv3_sender/gst_h264_encoder.hpp"
#include "gwv3_sender/logger.hpp"
#include "gwv3_sender/transport.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
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
#include <map>
#include <iterator>
#include <limits>
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
#include <fcntl.h>
#include <linux/videodev2.h>
#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <zlib.h>

namespace gwv3 {

namespace {

std::atomic<bool> g_running{true};
std::mutex g_camera_lifecycle_mutex;
constexpr uint16_t kDepthPreviewMinMm = 250;
constexpr uint16_t kDepthPreviewMaxMm = 2500;
constexpr size_t kMaxActiveCameras = 4;
constexpr size_t kMaxRgbEncodeTimingFrames = 64;
constexpr auto kCameraAnnounceInterval = std::chrono::seconds(5);
constexpr auto kHotplugScanInterval = std::chrono::seconds(2);
constexpr auto kHotplugRetryCooldown = std::chrono::seconds(30);
constexpr auto kHotplugLimitEventInterval = std::chrono::seconds(30);
constexpr uint64_t kRgbEncodedPtsMatchToleranceUs = 1000;

void handle_signal(int) {
    g_running = false;
}

struct Args {
    std::string config_path = "06_configs/sender_orangepi5pro-01.json";
    int run_seconds = 0;
    bool validate_only = false;
    bool no_preview = false;
    bool no_send = false;
};

struct CameraPerfStats {
    uint64_t wait_calls = 0;
    uint64_t wait_timeouts = 0;
    uint64_t rgb_input_frames = 0;
    uint64_t depth_input_frames = 0;
    uint64_t rgb_sent_packets = 0;
    uint64_t depth_sent_frames = 0;
    uint64_t rgb_corrupt_jpeg_frames = 0;
    uint64_t rgb_send_failures = 0;
    uint64_t depth_send_failures = 0;
    uint64_t rgb_input_bytes = 0;
    uint64_t depth_input_bytes = 0;
    uint64_t rgb_bytes = 0;
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
    double depth_sent_fps = 0.0;
    double rgb_usb_mbps = 0.0;
    double depth_usb_mbps = 0.0;
    double rgb_mbps = 0.0;
    double depth_mbps = 0.0;
    double rgb_encode_ms = 0.0;
    double depth_compress_ms = 0.0;
    int64_t color_auto_exposure = -1;
    int64_t color_exposure = -1;
    int64_t color_gain = -1;
    int64_t color_actual_fps = -1;
    int64_t color_frame_rate = -1;
    int64_t color_exposure_priority = -1;
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

struct V4l2Frame {
    std::vector<uint8_t> data;
    uint64_t frame_id = 0;
    uint64_t device_timestamp_us = 0;
    uint64_t system_timestamp_us = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fourcc = 0;
};

int checked_ioctl(int fd, unsigned long request, void *arg) {
    int rc = 0;
    do {
        rc = ioctl(fd, request, arg);
    } while(rc == -1 && errno == EINTR);
    return rc;
}

std::string errno_text(const std::string &prefix) {
    return prefix + ": " + std::strerror(errno);
}

std::string fourcc_text(uint32_t fourcc) {
    std::string text(4, ' ');
    text[0] = static_cast<char>(fourcc & 0xffu);
    text[1] = static_cast<char>((fourcc >> 8u) & 0xffu);
    text[2] = static_cast<char>((fourcc >> 16u) & 0xffu);
    text[3] = static_cast<char>((fourcc >> 24u) & 0xffu);
    return text;
}

uint32_t v4l2_fourcc_for_format(const std::string &format) {
    if(format == "mjpg") {
        return V4L2_PIX_FMT_MJPEG;
    }
    if(format == "yuyv") {
        return V4L2_PIX_FMT_YUYV;
    }
    if(format == "z16" || format == "y16") {
        return v4l2_fourcc('Z', '1', '6', ' ');
    }
    throw std::runtime_error("unsupported v4l2 pixel format: " + format);
}

class V4l2Capture {
public:
    V4l2Capture(std::string device_path, int width, int height, int fps, uint32_t fourcc)
        : device_path_(std::move(device_path)), width_(width), height_(height), fps_(fps), fourcc_(fourcc) {
        open_device();
        configure_format();
        configure_streaming();
    }

    ~V4l2Capture() { close_device(); }

    V4l2Capture(const V4l2Capture &) = delete;
    V4l2Capture &operator=(const V4l2Capture &) = delete;

    std::optional<V4l2Frame> read_frame(int timeout_ms) {
        pollfd pfd{};
        pfd.fd = fd_;
        pfd.events = POLLIN;
        const int poll_rc = poll(&pfd, 1, timeout_ms);
        if(poll_rc == 0) {
            return std::nullopt;
        }
        if(poll_rc < 0) {
            if(errno == EINTR) {
                return std::nullopt;
            }
            throw std::runtime_error(errno_text("poll failed for " + device_path_));
        }
        if((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            throw std::runtime_error("v4l2 device signaled error: " + device_path_);
        }

        v4l2_buffer buffer{};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        if(checked_ioctl(fd_, VIDIOC_DQBUF, &buffer) == -1) {
            if(errno == EAGAIN) {
                return std::nullopt;
            }
            throw std::runtime_error(errno_text("VIDIOC_DQBUF failed for " + device_path_));
        }
        if(buffer.index >= buffers_.size()) {
            throw std::runtime_error("v4l2 returned invalid buffer index for " + device_path_);
        }

        V4l2Frame frame;
        frame.frame_id = buffer.sequence;
        frame.device_timestamp_us = static_cast<uint64_t>(buffer.timestamp.tv_sec) * 1000000ull
                                    + static_cast<uint64_t>(buffer.timestamp.tv_usec);
        frame.system_timestamp_us = now_us();
        if(frame.device_timestamp_us == 0) {
            frame.device_timestamp_us = frame.system_timestamp_us;
        }
        frame.width = static_cast<uint32_t>(width_);
        frame.height = static_cast<uint32_t>(height_);
        frame.fourcc = fourcc_;
        const size_t used = buffer.bytesused > 0 ? buffer.bytesused : buffers_[buffer.index].length;
        const auto *src = static_cast<const uint8_t *>(buffers_[buffer.index].start);
        frame.data.assign(src, src + used);

        if(checked_ioctl(fd_, VIDIOC_QBUF, &buffer) == -1) {
            throw std::runtime_error(errno_text("VIDIOC_QBUF failed for " + device_path_));
        }
        return frame;
    }

    bool set_control(uint32_t control_id, int32_t value) {
        v4l2_queryctrl query{};
        query.id = control_id;
        if(checked_ioctl(fd_, VIDIOC_QUERYCTRL, &query) == -1 || (query.flags & V4L2_CTRL_FLAG_DISABLED) != 0) {
            return false;
        }
        v4l2_control control{};
        control.id = control_id;
        control.value = value;
        return checked_ioctl(fd_, VIDIOC_S_CTRL, &control) == 0;
    }

    std::optional<int32_t> get_control(uint32_t control_id) const {
        v4l2_control control{};
        control.id = control_id;
        if(checked_ioctl(fd_, VIDIOC_G_CTRL, &control) == -1) {
            return std::nullopt;
        }
        return control.value;
    }

    const std::string &device_path() const { return device_path_; }

private:
    struct Buffer {
        void *start = nullptr;
        size_t length = 0;
    };

    void open_device() {
        fd_ = open(device_path_.c_str(), O_RDWR | O_NONBLOCK);
        if(fd_ < 0) {
            throw std::runtime_error(errno_text("failed to open " + device_path_));
        }

        v4l2_capability capability{};
        if(checked_ioctl(fd_, VIDIOC_QUERYCAP, &capability) == -1) {
            throw std::runtime_error(errno_text("VIDIOC_QUERYCAP failed for " + device_path_));
        }
        const uint32_t caps = capability.device_caps != 0 ? capability.device_caps : capability.capabilities;
        if((caps & V4L2_CAP_VIDEO_CAPTURE) == 0 || (caps & V4L2_CAP_STREAMING) == 0) {
            throw std::runtime_error("v4l2 device does not support capture streaming: " + device_path_);
        }
    }

    void configure_format() {
        v4l2_format format{};
        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        format.fmt.pix.width = static_cast<uint32_t>(width_);
        format.fmt.pix.height = static_cast<uint32_t>(height_);
        format.fmt.pix.pixelformat = fourcc_;
        format.fmt.pix.field = V4L2_FIELD_NONE;
        if(checked_ioctl(fd_, VIDIOC_S_FMT, &format) == -1) {
            throw std::runtime_error(errno_text("VIDIOC_S_FMT failed for " + device_path_));
        }
        if(format.fmt.pix.width != static_cast<uint32_t>(width_) || format.fmt.pix.height != static_cast<uint32_t>(height_)
           || format.fmt.pix.pixelformat != fourcc_) {
            std::ostringstream oss;
            oss << "v4l2 device did not accept requested profile " << device_path_
                << " requested=" << width_ << "x" << height_ << " " << fourcc_text(fourcc_)
                << " actual=" << format.fmt.pix.width << "x" << format.fmt.pix.height << " "
                << fourcc_text(format.fmt.pix.pixelformat);
            throw std::runtime_error(oss.str());
        }

        v4l2_streamparm streamparm{};
        streamparm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if(checked_ioctl(fd_, VIDIOC_G_PARM, &streamparm) == 0
           && (streamparm.parm.capture.capability & V4L2_CAP_TIMEPERFRAME) != 0) {
            streamparm.parm.capture.timeperframe.numerator = 1;
            streamparm.parm.capture.timeperframe.denominator = static_cast<uint32_t>(fps_);
            checked_ioctl(fd_, VIDIOC_S_PARM, &streamparm);
        }
    }

    void configure_streaming() {
        v4l2_requestbuffers request{};
        request.count = 4;
        request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        request.memory = V4L2_MEMORY_MMAP;
        if(checked_ioctl(fd_, VIDIOC_REQBUFS, &request) == -1) {
            throw std::runtime_error(errno_text("VIDIOC_REQBUFS failed for " + device_path_));
        }
        if(request.count < 2) {
            throw std::runtime_error("v4l2 device returned too few mmap buffers: " + device_path_);
        }

        buffers_.resize(request.count);
        for(uint32_t i = 0; i < request.count; ++i) {
            v4l2_buffer buffer{};
            buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buffer.memory = V4L2_MEMORY_MMAP;
            buffer.index = i;
            if(checked_ioctl(fd_, VIDIOC_QUERYBUF, &buffer) == -1) {
                throw std::runtime_error(errno_text("VIDIOC_QUERYBUF failed for " + device_path_));
            }
            buffers_[i].length = buffer.length;
            buffers_[i].start = mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, buffer.m.offset);
            if(buffers_[i].start == MAP_FAILED) {
                buffers_[i].start = nullptr;
                throw std::runtime_error(errno_text("mmap failed for " + device_path_));
            }
            if(checked_ioctl(fd_, VIDIOC_QBUF, &buffer) == -1) {
                throw std::runtime_error(errno_text("initial VIDIOC_QBUF failed for " + device_path_));
            }
        }

        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if(checked_ioctl(fd_, VIDIOC_STREAMON, &type) == -1) {
            throw std::runtime_error(errno_text("VIDIOC_STREAMON failed for " + device_path_));
        }
        streaming_ = true;
    }

    void close_device() {
        if(fd_ >= 0 && streaming_) {
            v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            checked_ioctl(fd_, VIDIOC_STREAMOFF, &type);
            streaming_ = false;
        }
        for(auto &buffer : buffers_) {
            if(buffer.start != nullptr) {
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

    std::string device_path_;
    int width_ = 0;
    int height_ = 0;
    int fps_ = 0;
    uint32_t fourcc_ = 0;
    int fd_ = -1;
    bool streaming_ = false;
    std::vector<Buffer> buffers_;
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
    std::unique_ptr<V4l2Capture> rgb_capture;
    std::unique_ptr<V4l2Capture> depth_capture;
    std::unique_ptr<GstH264Encoder> encoder;
    GstH264InputFormat encoder_input_format = GstH264InputFormat::Bgr;
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
    CameraRuntime *depth_remap_target = nullptr;
    std::deque<RgbEncodeTiming> rgb_encode_timings;
    CameraPerfStats perf;
    CameraLiveStats live;
};

struct MediaPacketJob {
    CameraRuntime *camera = nullptr;
    StreamType stream_type = StreamType::rgb;
    std::vector<uint8_t> packet;
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

    bool publish(size_t slot_index, MediaPacketJob &&job) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(stopping_ || slot_index >= slots_.size()) {
            return false;
        }
        const bool overwritten = slots_[slot_index].has_value();
        slots_[slot_index] = std::move(job);
        cv_.notify_one();
        return overwritten;
    }

    std::optional<MediaPacketJob> wait_pop(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, timeout, [&] { return stopping_ || has_packet_locked(); });
        if(!has_packet_locked()) {
            return std::nullopt;
        }

        for(size_t offset = 0; offset < slots_.size(); ++offset) {
            const size_t index = (next_slot_ + offset) % slots_.size();
            if(slots_[index]) {
                auto job = std::move(*slots_[index]);
                slots_[index].reset();
                next_slot_ = (index + 1) % slots_.size();
                return job;
            }
        }
        return std::nullopt;
    }

    void stop() {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        cv_.notify_all();
    }

private:
    bool has_packet_locked() const {
        return std::any_of(slots_.begin(), slots_.end(), [](const auto &slot) { return slot.has_value(); });
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::optional<MediaPacketJob>> slots_;
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
    const auto max_value = static_cast<int64_t>(std::numeric_limits<int32_t>::max());
    if(value > max_value) {
        return std::numeric_limits<int32_t>::max();
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

void set_v4l2_control_if_configured(CameraRuntime &camera, Logger &logger, const std::string &name, uint32_t control_id,
                                    const std::optional<int> &value) {
    if(!value || !camera.rgb_capture) {
        return;
    }
    if(!camera.rgb_capture->set_control(control_id, *value)) {
        logger.warn("v4l2 color control unsupported or failed camera_id=" + camera.config.camera_id + " name=" + name);
        return;
    }
    std::string readback;
    if(auto current = camera.rgb_capture->get_control(control_id)) {
        readback = std::to_string(*current);
    }
    log_property_set_result(camera, logger, name, std::to_string(*value), readback);
}

void apply_v4l2_color_controls(CameraRuntime &camera, Logger &logger) {
    const auto &controls = camera.config.color_controls;
    if(!camera.rgb_capture || (!controls.auto_exposure && !controls.exposure && !controls.gain && !controls.auto_exposure_priority
                               && !controls.power_line_frequency)) {
        return;
    }

    if(controls.auto_exposure && !*controls.auto_exposure) {
        const int value = V4L2_EXPOSURE_MANUAL;
        if(camera.rgb_capture->set_control(V4L2_CID_EXPOSURE_AUTO, value)) {
            log_property_set_result(camera, logger, "auto_exposure", "false", std::to_string(value));
        }
        else {
            logger.warn("v4l2 color control unsupported or failed camera_id=" + camera.config.camera_id + " name=auto_exposure");
        }
    }
    set_v4l2_control_if_configured(camera, logger, "auto_exposure_priority", V4L2_CID_EXPOSURE_AUTO_PRIORITY,
                                   controls.auto_exposure_priority);
    set_v4l2_control_if_configured(camera, logger, "power_line_frequency", V4L2_CID_POWER_LINE_FREQUENCY,
                                   controls.power_line_frequency);
    set_v4l2_control_if_configured(camera, logger, "exposure", V4L2_CID_EXPOSURE_ABSOLUTE, controls.exposure);
    set_v4l2_control_if_configured(camera, logger, "gain", V4L2_CID_GAIN, controls.gain);
    if(controls.auto_exposure && *controls.auto_exposure) {
        const int value = V4L2_EXPOSURE_AUTO;
        if(camera.rgb_capture->set_control(V4L2_CID_EXPOSURE_AUTO, value)) {
            log_property_set_result(camera, logger, "auto_exposure", "true", std::to_string(value));
        }
        else {
            logger.warn("v4l2 color control unsupported or failed camera_id=" + camera.config.camera_id + " name=auto_exposure");
        }
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

RgbFrameDiagnostics rgb_frame_diagnostics(CameraRuntime &camera) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    RgbFrameDiagnostics diagnostics;
    diagnostics.auto_exposure = diagnostic_i32_or_unknown(camera.live.color_auto_exposure);
    diagnostics.exposure_us = diagnostic_i32_or_unknown(camera.live.color_exposure);
    diagnostics.gain = diagnostic_i32_or_unknown(camera.live.color_gain);
    diagnostics.actual_fps = diagnostic_i32_or_unknown(camera.live.color_actual_fps);
    return diagnostics;
}

void update_color_properties(CameraRuntime &camera) {
    if(camera.config.capture_backend == "v4l2" && camera.rgb_capture) {
        if(auto exposure_auto = camera.rgb_capture->get_control(V4L2_CID_EXPOSURE_AUTO)) {
            camera.live.color_auto_exposure = *exposure_auto == V4L2_EXPOSURE_AUTO ? 1 : 0;
        }
        if(auto exposure = camera.rgb_capture->get_control(V4L2_CID_EXPOSURE_ABSOLUTE)) {
            camera.live.color_exposure = *exposure;
        }
        if(auto gain = camera.rgb_capture->get_control(V4L2_CID_GAIN)) {
            camera.live.color_gain = *gain;
        }
        if(auto priority = camera.rgb_capture->get_control(V4L2_CID_EXPOSURE_AUTO_PRIORITY)) {
            camera.live.color_exposure_priority = *priority;
        }
        camera.live.color_actual_fps = camera.config.rgb_profile.fps;
        camera.live.color_frame_rate = camera.config.rgb_profile.fps;
        return;
    }
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
    camera.live.depth_sent_fps = rate_per_second(perf.depth_sent_frames, seconds);
    camera.live.rgb_usb_mbps = seconds > 0.0 ? static_cast<double>(perf.rgb_input_bytes) * 8.0 / seconds / 1000000.0 : 0.0;
    camera.live.depth_usb_mbps = seconds > 0.0 ? static_cast<double>(perf.depth_input_bytes) * 8.0 / seconds / 1000000.0 : 0.0;
    camera.live.rgb_mbps = seconds > 0.0 ? static_cast<double>(perf.rgb_bytes) * 8.0 / seconds / 1000000.0 : 0.0;
    camera.live.depth_mbps = seconds > 0.0 ? static_cast<double>(perf.depth_bytes) * 8.0 / seconds / 1000000.0 : 0.0;
    camera.live.rgb_encode_ms = avg_ms(perf.rgb_encode_ms, perf.rgb_input_frames);
    camera.live.depth_compress_ms = avg_ms(perf.depth_compress_ms, perf.depth_input_frames);

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    oss << "perf camera_id=" << camera.config.camera_id << " interval_s=" << seconds
        << " rgb_input_fps=" << camera.live.rgb_input_fps
        << " depth_input_fps=" << camera.live.depth_input_fps
        << " rgb_sent_packets_s=" << camera.live.rgb_sent_fps
        << " depth_sent_fps=" << camera.live.depth_sent_fps
        << " rgb_frame_id_delta=" << rgb_frame_id_delta
        << " depth_frame_id_delta=" << depth_frame_id_delta
        << " rgb_usb_mbps=" << camera.live.rgb_usb_mbps
        << " depth_usb_mbps=" << camera.live.depth_usb_mbps
        << " rgb_mbps=" << camera.live.rgb_mbps
        << " depth_mbps=" << camera.live.depth_mbps
        << " wait_avg_ms=" << avg_ms(perf.wait_ms, perf.wait_calls)
        << " wait_timeouts=" << perf.wait_timeouts
        << " rgb_decode_avg_ms=" << avg_ms(perf.rgb_decode_ms, perf.rgb_input_frames)
        << " rgb_encode_avg_ms=" << camera.live.rgb_encode_ms
        << " rgb_send_avg_ms=" << avg_ms(perf.rgb_send_ms, perf.rgb_sent_packets + perf.rgb_send_failures)
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
uint64_t frame_device_timestamp_us_or(const std::shared_ptr<FrameT> &frame, uint64_t fallback_us) {
    if(!frame) {
        return fallback_us;
    }
    const uint64_t timestamp = frame->timeStampUs();
    return timestamp == 0 ? fallback_us : timestamp;
}

uint64_t abs_diff_us(uint64_t lhs, uint64_t rhs) {
    return lhs >= rhs ? lhs - rhs : rhs - lhs;
}

void arm_rgb_keyframe_guard(CameraRuntime &camera, Logger &logger, const std::string &reason);

struct RgbEncodeTimingResolution {
    RgbEncodeTiming timing;
    bool matched_pts = false;
    bool used_fifo_fallback = false;
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
                                                    const RgbEncodeTiming &fallback) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    if(camera.rgb_encode_timings.empty()) {
        RgbEncodeTimingResolution resolution;
        resolution.timing = fallback;
        resolution.queue_empty = true;
        return resolution;
    }

    if(encoded.has_pts) {
        auto best = camera.rgb_encode_timings.end();
        uint64_t best_delta_us = 0;
        for(auto it = camera.rgb_encode_timings.begin(); it != camera.rgb_encode_timings.end(); ++it) {
            const auto delta_us = abs_diff_us(it->system_timestamp_us, encoded.pts_us);
            if(best == camera.rgb_encode_timings.end() || delta_us < best_delta_us) {
                best = it;
                best_delta_us = delta_us;
            }
            if(delta_us == 0) {
                break;
            }
        }

        if(best != camera.rgb_encode_timings.end() && best_delta_us <= kRgbEncodedPtsMatchToleranceUs) {
            RgbEncodeTimingResolution resolution;
            resolution.timing = *best;
            resolution.matched_pts = true;
            resolution.pts_delta_us = best_delta_us;
            camera.rgb_encode_timings.erase(camera.rgb_encode_timings.begin(), std::next(best));
            return resolution;
        }
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

void maybe_log_rgb_timing_resolution(CameraRuntime &camera, Logger &logger, const EncodedH264Frame &encoded,
                                     const RgbEncodeTimingResolution &resolution, std::chrono::steady_clock::time_point now) {
    if(!resolution.used_fifo_fallback && !resolution.queue_empty) {
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
    else if(!encoded.has_pts) {
        oss << " reason=missing_encoded_pts";
    }
    else {
        oss << " reason=pts_mismatch pts_delta_us=" << resolution.pts_delta_us << " encoded_pts_us=" << encoded.pts_us;
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

void append_v4l2_frame_time_sync(std::ostringstream &oss, const std::string &prefix, const std::optional<V4l2Frame> &frame,
                                 uint64_t host_now_us) {
    if(!frame) {
        oss << " " << prefix << "_present=0";
        return;
    }
    const int64_t host_minus_system_us =
        host_now_us >= frame->system_timestamp_us ? static_cast<int64_t>(host_now_us - frame->system_timestamp_us)
                                                  : -static_cast<int64_t>(frame->system_timestamp_us - host_now_us);
    oss << " " << prefix << "_present=1"
        << " " << prefix << "_frame_id=" << frame->frame_id
        << " " << prefix << "_canonical_timestamp_us=" << frame->system_timestamp_us
        << " " << prefix << "_device_timestamp_us=" << frame->device_timestamp_us
        << " " << prefix << "_system_timestamp_us=" << frame->system_timestamp_us
        << " " << prefix << "_host_minus_system_us=" << host_minus_system_us;
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
                   const std::shared_ptr<ob::DepthFrame> &depth, std::chrono::steady_clock::time_point now) {
    if(now < camera.next_time_sync_log) {
        return;
    }

    const uint64_t host_now = now_us();
    std::ostringstream oss;
    oss << "time_sync camera_id=" << camera.config.camera_id << " host_now_us=" << host_now;
    append_frame_time_sync(oss, "rgb", color, host_now);
    append_frame_time_sync(oss, "depth", depth, host_now);
    logger.info(oss.str());

    camera.next_time_sync_log = now + std::chrono::seconds(5);
}

void log_time_sync(CameraRuntime &camera, Logger &logger, const std::optional<V4l2Frame> &color, const std::optional<V4l2Frame> &depth,
                   std::chrono::steady_clock::time_point now) {
    if(now < camera.next_time_sync_log) {
        return;
    }

    const uint64_t host_now = now_us();
    std::ostringstream oss;
    oss << "time_sync camera_id=" << camera.config.camera_id << " backend=v4l2 host_now_us=" << host_now;
    append_v4l2_frame_time_sync(oss, "rgb", color, host_now);
    append_v4l2_frame_time_sync(oss, "depth", depth, host_now);
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

Json::Value profile_json(const VideoProfileConfig &profile, const std::string &pixel_format, const std::string &codec_or_compression,
                         float depth_scale = 0.0f) {
    Json::Value value;
    value["width"] = profile.width;
    value["height"] = profile.height;
    value["fps"] = profile.fps;
    value["source_format"] = profile.format;
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

cv::Mat mjpg_to_bgr(const void *data, size_t size, int flags) {
    if(data == nullptr || size == 0) {
        return {};
    }
    cv::Mat raw(1, static_cast<int>(size), CV_8UC1, const_cast<void *>(data));
    return cv::imdecode(raw, flags);
}

cv::Mat mjpg_to_bgr(const std::shared_ptr<ob::ColorFrame> &frame, int flags) {
    return mjpg_to_bgr(frame ? frame->data() : nullptr, frame ? frame->dataSize() : 0, flags);
}

bool mjpg_has_complete_jpeg(const void *payload, size_t size) {
    if(payload == nullptr || size < 4) {
        return false;
    }
    const auto *data = static_cast<const uint8_t *>(payload);
    if(data[0] != 0xff || data[1] != 0xd8) {
        return false;
    }

    size_t end = size;
    while(end > 0 && data[end - 1] == 0x00) {
        --end;
    }
    return end >= 4 && data[end - 2] == 0xff && data[end - 1] == 0xd9;
}

bool mjpg_has_complete_jpeg(const std::shared_ptr<ob::ColorFrame> &frame) {
    return mjpg_has_complete_jpeg(frame ? frame->data() : nullptr, frame ? frame->dataSize() : 0);
}

struct JpegValidationError {
    jpeg_error_mgr pub;
    jmp_buf jump_buffer;
    bool warning = false;
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

bool mjpg_decodes_cleanly(const void *payload, size_t size, std::string &message) {
    if(!mjpg_has_complete_jpeg(payload, size)) {
        message = "missing jpeg soi/eoi marker";
        return false;
    }

    jpeg_decompress_struct cinfo{};
    JpegValidationError error{};
    cinfo.err = jpeg_std_error(&error.pub);
    error.pub.error_exit = jpeg_validation_error_exit;
    error.pub.emit_message = jpeg_validation_emit_message;

    if(setjmp(error.jump_buffer) != 0) {
        jpeg_destroy_decompress(&cinfo);
        message = error.message[0] ? error.message : "jpeg validation failed";
        return false;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, static_cast<const unsigned char *>(payload), static_cast<unsigned long>(size));
    if(jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        message = "jpeg header validation failed";
        return false;
    }
    cinfo.scale_num = 1;
    cinfo.scale_denom = 8;
    jpeg_start_decompress(&cinfo);

    std::vector<JSAMPLE> row(static_cast<size_t>(cinfo.output_width) * static_cast<size_t>(cinfo.output_components));
    while(cinfo.output_scanline < cinfo.output_height) {
        JSAMPROW row_ptr = row.data();
        if(jpeg_read_scanlines(&cinfo, &row_ptr, 1) != 1) {
            break;
        }
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);

    if(error.warning) {
        message = error.message[0] ? error.message : "jpeg decoder warning";
        return false;
    }
    return true;
}

bool mjpg_decodes_cleanly(const std::shared_ptr<ob::ColorFrame> &frame, std::string &message) {
    return mjpg_decodes_cleanly(frame ? frame->data() : nullptr, frame ? frame->dataSize() : 0, message);
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

void mark_corrupt_rgb_jpeg_frame(CameraRuntime &camera, Logger &logger, const std::shared_ptr<ob::ColorFrame> &color,
                                 std::chrono::steady_clock::time_point frame_now, const std::string &reason) {
    mark_corrupt_rgb_jpeg_frame(camera, logger, color ? color->index() : 0, color ? color->dataSize() : 0, frame_now, reason);
}

void record_queue_overwrite(CameraRuntime &camera, StreamType stream_type) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    if(stream_type == StreamType::rgb) {
        camera.rgb_dropped++;
    }
    else {
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
    else {
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

bool should_drop_rgb_until_keyframe(CameraRuntime &camera, bool is_keyframe, std::chrono::steady_clock::time_point now, Logger &logger) {
    bool drop = false;
    bool log_drop = false;
    bool log_recovered = false;
    uint64_t dropped = 0;
    {
        std::lock_guard<std::mutex> lock(camera.mutex);
        if(!camera.rgb_waiting_for_keyframe_after_transport_loss) {
            return false;
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
    return drop;
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
        else {
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
                    + " stream=" + (stream_type == StreamType::rgb ? "rgb" : "depth") + " error=" + message);
    }
}

void record_rgb_input(CameraRuntime &camera, const std::shared_ptr<ob::ColorFrame> &color) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    camera.perf.rgb_input_frames++;
    camera.perf.rgb_input_bytes += color->dataSize();
    camera.perf.note_rgb_frame_id(color->index());
    camera.last_rgb_frame_at = std::chrono::steady_clock::now();
}

void record_rgb_input(CameraRuntime &camera, size_t data_size, uint64_t frame_id) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    camera.perf.rgb_input_frames++;
    camera.perf.rgb_input_bytes += data_size;
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

void record_depth_input(CameraRuntime &camera, size_t data_size, uint64_t frame_id) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    camera.perf.depth_input_frames++;
    camera.perf.depth_input_bytes += data_size;
    camera.perf.note_depth_frame_id(frame_id);
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

void set_latest_bgr(CameraRuntime &camera, const cv::Mat &bgr) {
    if(bgr.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(camera.mutex);
    camera.latest_bgr = bgr.clone();
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

cv::Mat depth_u16_to_color(const void *data, int width, int height) {
    if(data == nullptr || width <= 0 || height <= 0) {
        return {};
    }
    cv::Mat depth(height, width, CV_16UC1, const_cast<void *>(data));
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

cv::Mat depth_to_color(const std::shared_ptr<ob::DepthFrame> &frame) {
    return depth_u16_to_color(frame ? frame->data() : nullptr, frame ? frame->width() : 0, frame ? frame->height() : 0);
}

bool h264_payload_has_idr(const std::vector<uint8_t> &payload) {
    for(size_t i = 0; i + 4 < payload.size(); ++i) {
        size_t nal_offset = 0;
        if(payload[i] == 0 && payload[i + 1] == 0 && payload[i + 2] == 1) {
            nal_offset = i + 3;
        }
        else if(i + 5 < payload.size() && payload[i] == 0 && payload[i + 1] == 0 && payload[i + 2] == 0 && payload[i + 3] == 1) {
            nal_offset = i + 4;
        }
        if(nal_offset > 0 && nal_offset < payload.size() && (payload[nal_offset] & 0x1fu) == 5u) {
            return true;
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
    capabilities["depth_compression"] = depth;
    capabilities["local_preview"] = config.preview.enabled;
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
    if(camera.config.capture_backend == "v4l2") {
        Json::Value device;
        device["vendor"] = "orbbec";
        device["model"] = camera.config.device_model.empty() ? "Orbbec UVC RGBD" : camera.config.device_model;
        device["serial_number"] = camera.device_serial;
        device["uid"] = camera.device_uid;
        device["firmware_version"] = "";
        device["connection_type"] = camera.device_connection_type;
        device["rgb_device_path"] = camera.config.rgb_device_path;
        device["depth_device_path"] = camera.config.depth_device_path;
        msg["device"] = device;
        msg["rgb_profile"] = profile_json(camera.config.rgb_profile, "encoded_video", camera.config.rgb_encoding.codec);
        msg["depth_profile"] =
            profile_json(camera.config.depth_profile, "uint16", camera.config.depth_transport.compression, camera.depth_scale);
        Json::Value calibration;
        calibration["available"] = false;
        calibration["source"] = "v4l2_uvc";
        calibration["warning"] = "calibration_not_available_from_uvc_backend";
        calibration["data"] = Json::objectValue;
        msg["calibration"] = calibration;
        return msg;
    }
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

std::string ob_error_text(const ob::Error &error) {
    const char *message = error.getMessage();
    return message ? message : "Orbbec SDK error";
}

std::chrono::milliseconds reconnect_delay(uint32_t attempts) {
    return std::chrono::seconds(std::min<uint32_t>(5, std::max<uint32_t>(1, attempts)));
}

void reset_runtime_after_camera_start(CameraRuntime &runtime, std::chrono::steady_clock::time_point now, float depth_scale) {
    runtime.encoder.reset();
    runtime.online = true;
    runtime.announced = false;
    runtime.hardware_encoder = false;
    runtime.depth_scale = depth_scale;
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
    camera.rgb_capture.reset();
    camera.depth_capture.reset();
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
    if(runtime.config.capture_backend == "v4l2") {
        runtime.device.reset();
        runtime.pipeline.reset();
        runtime.color_profile.reset();
        runtime.depth_profile.reset();
        runtime.rgb_capture = std::make_unique<V4l2Capture>(runtime.config.rgb_device_path, runtime.config.rgb_profile.width,
                                                            runtime.config.rgb_profile.height, runtime.config.rgb_profile.fps,
                                                            v4l2_fourcc_for_format(runtime.config.rgb_profile.format));
        runtime.depth_capture = std::make_unique<V4l2Capture>(runtime.config.depth_device_path, runtime.config.depth_profile.width,
                                                              runtime.config.depth_profile.height, runtime.config.depth_profile.fps,
                                                              v4l2_fourcc_for_format(runtime.config.depth_profile.format));
        runtime.device_serial = runtime.config.serial_number;
        runtime.device_uid = runtime.config.uid.empty() ? runtime.config.rgb_device_path + "+" + runtime.config.depth_device_path
                                                       : runtime.config.uid;
        runtime.device_connection_type = "usb-v4l2";
        apply_v4l2_color_controls(runtime, logger);
        update_color_properties(runtime);
        const auto now = std::chrono::steady_clock::now();
        reset_runtime_after_camera_start(runtime, now, runtime.config.depth_scale > 0.0f ? runtime.config.depth_scale : 1.0f);

        std::ostringstream oss;
        oss << "camera started camera_id=" << runtime.config.camera_id << " backend=v4l2"
            << " color=" << runtime.config.rgb_profile.width << "x" << runtime.config.rgb_profile.height << "@"
            << runtime.config.rgb_profile.fps << " format=" << runtime.config.rgb_profile.format
            << " rgb_device=" << runtime.config.rgb_device_path
            << " depth=" << runtime.config.depth_profile.width << "x" << runtime.config.depth_profile.height << "@"
            << runtime.config.depth_profile.fps << " format=" << runtime.config.depth_profile.format
            << " depth_device=" << runtime.config.depth_device_path
            << " configured_serial=" << runtime.config.serial_number
            << " configured_uid=" << runtime.config.uid
            << " connection=" << runtime.device_connection_type;
        logger.info(oss.str());
        return;
    }
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
    reset_runtime_after_camera_start(runtime, now, 0.0f);

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

void publish_media_packet(LatestMediaQueue &media_queue, size_t slot_index, CameraRuntime &camera, StreamType stream_type,
                          std::vector<uint8_t> &&packet) {
    MediaPacketJob job;
    job.camera = &camera;
    job.stream_type = stream_type;
    job.packet = std::move(packet);
    if(media_queue.publish(slot_index, std::move(job))) {
        record_queue_overwrite(camera, stream_type);
    }
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
        {
            std::lock_guard<std::mutex> lock(transport_mutex);
            sent = transport.send_media(job->packet);
            if(!sent) {
                error = transport.last_error();
            }
        }
        const auto send_ended = std::chrono::steady_clock::now();
        const double send_ms = elapsed_ms(send_started, send_ended);
        if(sent) {
            record_media_send_success(*job->camera, job->stream_type, job->packet.size(), send_ms);
        }
        else {
            record_media_send_failure(*job->camera, logger, job->stream_type, send_ms, send_ended, error);
            if(job->stream_type == StreamType::rgb) {
                arm_rgb_keyframe_guard(*job->camera, logger, error);
            }
        }
    }
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
void camera_worker_loop(const AppConfig &config, CameraRuntime &camera, size_t slot_base, LatestMediaQueue &media_queue,
                        Sender &transport, Logger &logger, std::mutex &transport_mutex, std::chrono::milliseconds preview_interval) {
    const size_t rgb_slot = slot_base;
    const size_t depth_slot = slot_base + 1;

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

        cv::Mat bgr;
        if(color) {
            record_rgb_input(camera, color);
            update_color_metadata(camera, color);
            const bool color_is_mjpg = color->format() == OB_FORMAT_MJPG;
            const uint64_t rgb_system_timestamp_us = frame_system_timestamp_us_or(color, frame_host_now_us);
            const uint64_t rgb_device_timestamp_us = frame_device_timestamp_us_or(color, rgb_system_timestamp_us);
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
                    set_latest_bgr(camera, preview_bgr);
                }
                else if(!color_is_mjpg) {
                    const auto decode_started = std::chrono::steady_clock::now();
                    bgr = color_to_bgr(color);
                    record_rgb_decode_ms(camera, elapsed_ms(decode_started, std::chrono::steady_clock::now()));
                    if(preview_due) {
                        set_latest_bgr(camera, bgr);
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
                            RgbEncodeTiming submitted_timing = rgb_capture_timing;
                            submitted_timing.encode_start_timestamp_us = now_us();
                            remember_rgb_encode_timing(camera, submitted_timing);
                            const auto encode_started = std::chrono::steady_clock::now();
                            const auto encoded_units = input_format == GstH264InputFormat::Jpeg
                                                           ? camera.encoder->encode_jpeg(color->data(), color->dataSize(), rgb_system_timestamp_us)
                                                           : camera.encoder->encode_bgr(bgr, rgb_system_timestamp_us);
                            const uint64_t encode_done_timestamp_us = now_us();
                            submitted_timing.encode_done_timestamp_us = encode_done_timestamp_us;
                            record_rgb_encode_ms(camera, elapsed_ms(encode_started, std::chrono::steady_clock::now()));
                            for(const auto &encoded : encoded_units) {
                                const bool is_key_frame = h264_payload_has_idr(encoded.data);
                                if(should_drop_rgb_until_keyframe(camera, is_key_frame, frame_now, logger)) {
                                    continue;
                                }
                                const auto timing_resolution = resolve_rgb_encode_timing(camera, encoded, submitted_timing);
                                maybe_log_rgb_timing_resolution(camera, logger, encoded, timing_resolution, frame_now);
                                auto timing = timing_resolution.timing;
                                if(timing.encode_start_timestamp_us == 0) {
                                    timing.encode_start_timestamp_us = submitted_timing.encode_start_timestamp_us;
                                }
                                timing.encode_done_timestamp_us = encode_done_timestamp_us;
                                if(!rgb_encoded_timing_is_monotonic(camera, logger, timing, frame_now)) {
                                    continue;
                                }
                                MediaFrameMeta meta;
                                meta.stream_type = StreamType::rgb;
                                meta.flags = has_system_timestamp | has_rgb_diagnostics | (is_key_frame ? key_frame : 0u);
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
                                auto packet = build_media_packet(meta, encoded.data.data());
                                publish_media_packet(media_queue, rgb_slot, camera, StreamType::rgb, std::move(packet));
                                mark_rgb_encoded_timing_queued(camera, timing);
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
            record_depth_input(camera, depth);
            set_depth_scale_if_empty(camera, depth->getValueScale());
            set_depth_scale_if_empty(*depth_target, depth->getValueScale());
            const bool publish_depth = depth_emit_due(camera, frame_now);
            if(publish_depth) {
                std::vector<uint8_t> compressed_depth;
                const void *depth_payload = depth->data();
                size_t depth_payload_size = depth->dataSize();
                if(camera.config.depth_transport.compression == "zlib") {
                    const auto compress_started = std::chrono::steady_clock::now();
                    compressed_depth = zlib_compress_payload(depth->data(), depth->dataSize());
                    record_depth_compress_ms(camera, elapsed_ms(compress_started, std::chrono::steady_clock::now()));
                    depth_payload = compressed_depth.data();
                    depth_payload_size = compressed_depth.size();
                }
                const uint64_t depth_system_timestamp_us = frame_system_timestamp_us_or(depth, frame_host_now_us);
                const uint64_t depth_device_timestamp_us = frame_device_timestamp_us_or(depth, depth_system_timestamp_us);
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
                auto packet = build_media_packet(meta, depth_payload);
                publish_media_packet(media_queue, depth_slot, *depth_target, StreamType::depth_raw, std::move(packet));
                record_depth_frame_done(*depth_target);

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
        log_time_sync(camera, logger, color, depth, frame_now);
        if(preview_due) {
            std::lock_guard<std::mutex> lock(camera.mutex);
            camera.next_preview = frame_now + preview_interval;
        }
    }
}

cv::Mat v4l2_frame_to_bgr(const V4l2Frame &frame) {
    if(frame.data.empty()) {
        return {};
    }
    if(frame.fourcc == V4L2_PIX_FMT_MJPEG) {
        return mjpg_to_bgr(frame.data.data(), frame.data.size(), cv::IMREAD_COLOR);
    }
    if(frame.fourcc == V4L2_PIX_FMT_YUYV) {
        cv::Mat yuyv(static_cast<int>(frame.height), static_cast<int>(frame.width), CV_8UC2,
                     const_cast<uint8_t *>(frame.data.data()));
        cv::Mat bgr;
        cv::cvtColor(yuyv, bgr, cv::COLOR_YUV2BGR_YUYV);
        return bgr;
    }
    return {};
}

template <typename Sender>
void v4l2_camera_worker_loop(const AppConfig &config, CameraRuntime &camera, size_t slot_base, LatestMediaQueue &media_queue,
                             Sender &transport, Logger &logger, std::mutex &transport_mutex, std::chrono::milliseconds preview_interval) {
    const size_t rgb_slot = slot_base;
    const size_t depth_slot = slot_base + 1;

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

        std::optional<V4l2Frame> color;
        std::optional<V4l2Frame> depth;
        try {
            const auto wait_started = std::chrono::steady_clock::now();
            if(!camera.rgb_capture || !camera.depth_capture) {
                throw std::runtime_error("v4l2 capture is not initialized");
            }
            color = camera.rgb_capture->read_frame(50);
            depth = camera.depth_capture->read_frame(color ? 0 : 10);
            if(!color) {
                color = camera.rgb_capture->read_frame(0);
            }
            if(!depth) {
                depth = camera.depth_capture->read_frame(0);
            }
            const auto wait_ended = std::chrono::steady_clock::now();
            record_wait_result(camera, elapsed_ms(wait_started, wait_ended), !color && !depth);
            if(!color && !depth) {
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

        const auto frame_now = std::chrono::steady_clock::now();
        bool preview_due = false;
        {
            std::lock_guard<std::mutex> lock(camera.mutex);
            preview_due = config.preview.enabled && frame_now >= camera.next_preview;
        }

        cv::Mat bgr;
        if(color) {
            record_rgb_input(camera, color->data.size(), color->frame_id);
            const bool color_is_mjpg = color->fourcc == V4L2_PIX_FMT_MJPEG;
            RgbEncodeTiming rgb_capture_timing{color->frame_id, color->device_timestamp_us, color->system_timestamp_us,
                                               color->width, color->height, RgbFrameDiagnostics{}};
            rgb_capture_timing.capture_host_timestamp_us = color->system_timestamp_us;
            rgb_capture_timing.timing_bound_timestamp_us = now_us();
            bool rgb_usable = true;
            if(color_is_mjpg && !mjpg_has_complete_jpeg(color->data.data(), color->data.size())) {
                rgb_usable = false;
                mark_corrupt_rgb_jpeg_frame(camera, logger, color->frame_id, color->data.size(), frame_now,
                                            "missing jpeg soi/eoi marker");
            }
            else if(color_is_mjpg && (preview_due || camera.config.validate_rgb_mjpeg)) {
                std::string jpeg_validation_message;
                if(!mjpg_decodes_cleanly(color->data.data(), color->data.size(), jpeg_validation_message)) {
                    rgb_usable = false;
                    mark_corrupt_rgb_jpeg_frame(camera, logger, color->frame_id, color->data.size(), frame_now, jpeg_validation_message);
                }
            }

            if(rgb_usable) {
                if(color_is_mjpg && preview_due) {
                    const auto decode_started = std::chrono::steady_clock::now();
                    auto preview_bgr = mjpg_to_bgr(color->data.data(), color->data.size(), cv::IMREAD_REDUCED_COLOR_2);
                    record_rgb_decode_ms(camera, elapsed_ms(decode_started, std::chrono::steady_clock::now()));
                    set_latest_bgr(camera, preview_bgr);
                }
                else if(!color_is_mjpg) {
                    const auto decode_started = std::chrono::steady_clock::now();
                    bgr = v4l2_frame_to_bgr(*color);
                    record_rgb_decode_ms(camera, elapsed_ms(decode_started, std::chrono::steady_clock::now()));
                    if(preview_due) {
                        set_latest_bgr(camera, bgr);
                    }
                }

                if(!camera.encoder && (color_is_mjpg || !bgr.empty())) {
                    auto input_format = color_is_mjpg ? GstH264InputFormat::Jpeg : GstH264InputFormat::Bgr;
                    {
                        std::lock_guard<std::mutex> encoder_lock(g_encoder_create_mutex);
                        camera.encoder = std::make_unique<GstH264Encoder>(static_cast<int>(color->width), static_cast<int>(color->height),
                                                                            camera.config.rgb_profile.fps,
                                                                            camera.config.rgb_encoding.bitrate_bps,
                                                                            camera.config.rgb_encoding.gstreamer_encoder, input_format);
                        if(!camera.encoder->ok() && color_is_mjpg) {
                            logger.warn("jpeg rgb path unavailable, falling back to BGR encode path: " + camera.encoder->error());
                            input_format = GstH264InputFormat::Bgr;
                            camera.encoder = std::make_unique<GstH264Encoder>(static_cast<int>(color->width),
                                                                                static_cast<int>(color->height),
                                                                                camera.config.rgb_profile.fps,
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

                if(camera.encoder && camera.encoder->ok()) {
                    try {
                        const auto input_format = encoder_input_format_for(camera);
                        if(input_format == GstH264InputFormat::Bgr && bgr.empty()) {
                            const auto decode_started = std::chrono::steady_clock::now();
                            bgr = v4l2_frame_to_bgr(*color);
                            record_rgb_decode_ms(camera, elapsed_ms(decode_started, std::chrono::steady_clock::now()));
                        }
                        if(input_format == GstH264InputFormat::Bgr && bgr.empty()) {
                            set_camera_last_error(camera, "rgb decode produced empty frame");
                            record_queue_overwrite(camera, StreamType::rgb);
                        }
                        else {
                            {
                                std::lock_guard<std::mutex> lock(camera.mutex);
                                update_color_properties(camera);
                            }
                            RgbEncodeTiming submitted_timing = rgb_capture_timing;
                            submitted_timing.diagnostics = rgb_frame_diagnostics(camera);
                            submitted_timing.encode_start_timestamp_us = now_us();
                            remember_rgb_encode_timing(camera, submitted_timing);
                            const auto encode_started = std::chrono::steady_clock::now();
                            const auto encoded_units = input_format == GstH264InputFormat::Jpeg
                                                           ? camera.encoder->encode_jpeg(color->data.data(), color->data.size(),
                                                                                         color->system_timestamp_us)
                                                           : camera.encoder->encode_bgr(bgr, color->system_timestamp_us);
                            const uint64_t encode_done_timestamp_us = now_us();
                            submitted_timing.encode_done_timestamp_us = encode_done_timestamp_us;
                            record_rgb_encode_ms(camera, elapsed_ms(encode_started, std::chrono::steady_clock::now()));
                            for(const auto &encoded : encoded_units) {
                                const bool is_key_frame = h264_payload_has_idr(encoded.data);
                                if(should_drop_rgb_until_keyframe(camera, is_key_frame, frame_now, logger)) {
                                    continue;
                                }
                                const auto timing_resolution = resolve_rgb_encode_timing(camera, encoded, submitted_timing);
                                maybe_log_rgb_timing_resolution(camera, logger, encoded, timing_resolution, frame_now);
                                auto timing = timing_resolution.timing;
                                if(timing.encode_start_timestamp_us == 0) {
                                    timing.encode_start_timestamp_us = submitted_timing.encode_start_timestamp_us;
                                }
                                timing.encode_done_timestamp_us = encode_done_timestamp_us;
                                if(!rgb_encoded_timing_is_monotonic(camera, logger, timing, frame_now)) {
                                    continue;
                                }
                                MediaFrameMeta meta;
                                meta.stream_type = StreamType::rgb;
                                meta.flags = has_system_timestamp | has_rgb_diagnostics | (is_key_frame ? key_frame : 0u);
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
                                auto packet = build_media_packet(meta, encoded.data.data());
                                publish_media_packet(media_queue, rgb_slot, camera, StreamType::rgb, std::move(packet));
                                mark_rgb_encoded_timing_queued(camera, timing);
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
            record_depth_input(camera, depth->data.size(), depth->frame_id);
            set_depth_scale_if_empty(camera, camera.config.depth_scale > 0.0f ? camera.config.depth_scale : 1.0f);
            const bool publish_depth = depth_emit_due(camera, frame_now);
            if(publish_depth) {
                std::vector<uint8_t> compressed_depth;
                const void *depth_payload = depth->data.data();
                size_t depth_payload_size = depth->data.size();
                if(camera.config.depth_transport.compression == "zlib") {
                    const auto compress_started = std::chrono::steady_clock::now();
                    compressed_depth = zlib_compress_payload(depth->data.data(), depth->data.size());
                    record_depth_compress_ms(camera, elapsed_ms(compress_started, std::chrono::steady_clock::now()));
                    depth_payload = compressed_depth.data();
                    depth_payload_size = compressed_depth.size();
                }
                MediaFrameMeta meta;
                meta.stream_type = StreamType::depth_raw;
                meta.flags = has_system_timestamp;
                meta.sender_id = config.sender_id;
                meta.camera_id = camera.config.camera_id;
                meta.codec_or_compression = camera.config.depth_transport.compression;
                meta.frame_id = depth->frame_id;
                meta.timestamp_us = depth->device_timestamp_us;
                meta.system_timestamp_us = depth->system_timestamp_us;
                meta.width = depth->width;
                meta.height = depth->height;
                meta.pixel_format = PixelFormat::depth_u16;
                meta.payload_size = depth_payload_size;
                meta.uncompressed_size = depth->data.size();
                auto packet = build_media_packet(meta, depth_payload);
                publish_media_packet(media_queue, depth_slot, camera, StreamType::depth_raw, std::move(packet));
                record_depth_frame_done(camera);

                if(preview_due) {
                    const auto depth_preview_started = std::chrono::steady_clock::now();
                    auto depth_color = depth_u16_to_color(depth->data.data(), static_cast<int>(depth->width), static_cast<int>(depth->height));
                    record_depth_preview_ms(camera, elapsed_ms(depth_preview_started, std::chrono::steady_clock::now()));
                    set_latest_depth_color(camera, depth_color);
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
        log_time_sync(camera, logger, color, depth, frame_now);
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
                          std::vector<std::thread> &camera_threads, LatestMediaQueue &media_queue, Sender &transport,
                          Logger &logger, std::mutex &transport_mutex, std::chrono::milliseconds preview_interval,
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

        const size_t slot_base = media_queue.append_slots(2);
        CameraRuntime *camera_ptr = runtime.get();
        cameras.push_back(std::move(runtime));
        camera_threads.emplace_back([&, camera_ptr, slot_base] {
            camera_worker_loop(config, *camera_ptr, slot_base, media_queue, transport, logger, transport_mutex, preview_interval);
        });
        logger.info("hotplug camera started camera_id=" + camera_id + " slot_base=" + std::to_string(slot_base)
                    + " device=" + device_identity_summary(identity));
        send_status_locked(transport, logger, transport_mutex,
                           event_message(config, "info", "camera_connected", "hotplug camera pipeline started", camera_id));
        ++next_hotplug_camera_number;
    }
}

template <typename Sender>
void run_sender(AppConfig config, const Args &args, Sender &transport, Logger &logger) {
    const bool display_available = std::getenv("DISPLAY") != nullptr || std::getenv("WAYLAND_DISPLAY") != nullptr;
    if(args.no_preview || !display_available) {
        config.preview.enabled = false;
    }
    if(!display_available && !args.no_preview) {
        logger.warn("DISPLAY/WAYLAND_DISPLAY not set; local preview disabled for this run");
    }
    const auto preview_interval =
        std::chrono::milliseconds(config.preview.fps > 0 ? std::max(1, 1000 / config.preview.fps) : 100);

    const auto started = std::chrono::steady_clock::now();
    auto cameras = start_cameras(config, logger);
    configure_depth_remap_targets(config, cameras, logger);
    LatestMediaQueue media_queue(cameras.size() * 2);
    std::mutex transport_mutex;

    send_status_locked(transport, logger, transport_mutex, sender_hello(config));
    for(const auto &camera : cameras) {
        bool online = false;
        std::string last_error;
        {
            std::lock_guard<std::mutex> lock(camera->mutex);
            online = camera->online;
            last_error = camera->last_error;
        }
        if(online) {
            send_status_locked(transport, logger, transport_mutex,
                               event_message(config, "info", "camera_connected", "camera pipeline started", camera->config.camera_id));
        }
        else {
            send_status_locked(transport, logger, transport_mutex,
                               camera_offline_message(config, camera->config.camera_id, last_error));
        }
    }

    std::thread media_thread([&] { media_sender_loop(media_queue, transport, logger, transport_mutex); });
    std::vector<std::thread> camera_threads;
    camera_threads.reserve(cameras.size());
    for(size_t i = 0; i < cameras.size(); ++i) {
        const size_t slot_base = i * 2;
        camera_threads.emplace_back([&, i, slot_base] {
            if(cameras[i]->config.capture_backend == "v4l2") {
                v4l2_camera_worker_loop(config, *cameras[i], slot_base, media_queue, transport, logger, transport_mutex, preview_interval);
            }
            else {
                camera_worker_loop(config, *cameras[i], slot_base, media_queue, transport, logger, transport_mutex, preview_interval);
            }
        });
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
        if(now >= next_heartbeat) {
            for(auto &camera : cameras) {
                send_status_locked(transport, logger, transport_mutex, camera_heartbeat(config, *camera, started));
            }
            next_heartbeat = now + std::chrono::milliseconds(config.heartbeat_interval_ms);
        }
        if(now >= next_camera_announce) {
            for(auto &camera : cameras) {
                if(camera_online(*camera)) {
                    send_status_locked(transport, logger, transport_mutex, camera_announce(config, *camera));
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
            for(auto &camera : cameras) {
                const auto preview_started = std::chrono::steady_clock::now();
                preview_frame(*camera, true);
                record_preview_ms(*camera, elapsed_ms(preview_started, std::chrono::steady_clock::now()));
            }
            next_preview = now + preview_interval;
        }
        if(config.hotplug.enabled && now >= next_hotplug_scan) {
            scan_hotplug_cameras(config, cameras, camera_threads, media_queue, transport, logger, transport_mutex, preview_interval,
                                 next_hotplug_camera_number, hotplug_retry_cooldowns, next_hotplug_limit_event);
            next_hotplug_scan = now + kHotplugScanInterval;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    g_running = false;
    media_queue.stop();
    for(auto &thread : camera_threads) {
        if(thread.joinable()) {
            thread.join();
        }
    }
    if(media_thread.joinable()) {
        media_thread.join();
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
            run_sender(config, args, transport, logger);
        }
        else {
            Transport transport(config);
            run_sender(config, args, transport, logger);
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
