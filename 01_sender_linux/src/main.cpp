#include "gwv3_common/protocol.hpp"
#include "gwv3_sender/config.hpp"
#include "gwv3_sender/gst_h264_encoder.hpp"
#include "gwv3_sender/logger.hpp"
#include "gwv3_sender/transport.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csetjmp>
#include <cstdio>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
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

namespace gwv3 {

namespace {

std::atomic<bool> g_running{true};
constexpr uint16_t kDepthPreviewMinMm = 250;
constexpr uint16_t kDepthPreviewMaxMm = 2500;

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

struct CameraRuntime {
    CameraConfig config;
    std::shared_ptr<ob::Device> device;
    std::unique_ptr<ob::Pipeline> pipeline;
    std::shared_ptr<ob::VideoStreamProfile> color_profile;
    std::shared_ptr<ob::VideoStreamProfile> depth_profile;
    std::unique_ptr<GstH264Encoder> encoder;
    GstH264InputFormat encoder_input_format = GstH264InputFormat::Bgr;
    bool announced = false;
    bool online = false;
    bool hardware_encoder = false;
    uint32_t reconnect_attempts = 0;
    uint32_t disconnects = 0;
    uint64_t rgb_frames = 0;
    uint64_t depth_frames = 0;
    uint64_t rgb_corrupt_jpeg = 0;
    uint64_t rgb_dropped = 0;
    uint64_t depth_dropped = 0;
    std::string last_error;
    float depth_scale = 0.0f;
    std::chrono::steady_clock::time_point stats_started = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point next_preview = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point next_reconnect = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point next_time_sync_log = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point next_jpeg_warning = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point next_media_warning = std::chrono::steady_clock::now();
    std::string last_media_warning;
    cv::Mat latest_bgr;
    cv::Mat latest_depth_color;
    CameraPerfStats perf;
    CameraLiveStats live;
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
    camera.live.color_auto_exposure = metadata_or(color, OB_FRAME_METADATA_TYPE_AUTO_EXPOSURE, camera.live.color_auto_exposure);
    camera.live.color_exposure = metadata_or(color, OB_FRAME_METADATA_TYPE_EXPOSURE, camera.live.color_exposure);
    camera.live.color_gain = metadata_or(color, OB_FRAME_METADATA_TYPE_GAIN, camera.live.color_gain);
    camera.live.color_actual_fps = metadata_or(color, OB_FRAME_METADATA_TYPE_ACTUAL_FRAME_RATE, camera.live.color_actual_fps);
    camera.live.color_frame_rate = metadata_or(color, OB_FRAME_METADATA_TYPE_FRAME_RATE, camera.live.color_frame_rate);
    camera.live.color_exposure_priority = metadata_or(color, OB_FRAME_METADATA_TYPE_EXPOSURE_PRIORITY, camera.live.color_exposure_priority);
}

void update_color_properties(CameraRuntime &camera) {
    camera.live.color_auto_exposure = bool_property_or(camera.device, OB_PROP_COLOR_AUTO_EXPOSURE_BOOL, camera.live.color_auto_exposure);
    camera.live.color_exposure = int_property_or(camera.device, OB_PROP_COLOR_EXPOSURE_INT, camera.live.color_exposure);
    camera.live.color_gain = int_property_or(camera.device, OB_PROP_COLOR_GAIN_INT, camera.live.color_gain);
    camera.live.color_exposure_priority = int_property_or(camera.device, OB_PROP_COLOR_AUTO_EXPOSURE_PRIORITY_INT, camera.live.color_exposure_priority);
}

void log_perf(CameraRuntime &camera, Logger &logger, std::chrono::steady_clock::time_point now) {
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
        << " rgb_send_failures=" << perf.rgb_send_failures
        << " depth_send_failures=" << perf.depth_send_failures;
    logger.info(oss.str());

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

Json::Value calibration_json(ob::Pipeline &pipeline) {
    Json::Value calibration;
    calibration["available"] = false;
    calibration["source"] = "orbbec_sdk";
    calibration["data"] = Json::objectValue;
    try {
        const auto camera_param = pipeline.getCameraParam();
        Json::Value data;
        data["depth_intrinsic"] = intrinsic_json(camera_param.depthIntrinsic);
        data["rgb_intrinsic"] = intrinsic_json(camera_param.rgbIntrinsic);
        data["depth_distortion"] = distortion_json(camera_param.depthDistortion);
        data["rgb_distortion"] = distortion_json(camera_param.rgbDistortion);
        data["d2c_transform"] = transform_json(camera_param.transform);
        data["is_mirrored"] = camera_param.isMirrored;
        calibration["available"] = true;
        calibration["data"] = data;
    }
    catch(const std::exception &) {
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
        oss << "index=" << i << " serial=" << (devices->serialNumber(i) ? devices->serialNumber(i) : "") << " uid=" << uid
            << " paired_rgb_serial=" << paired_rgb_serial
            << " connection=" << (devices->connectionType(i) ? devices->connectionType(i) : "");
    }
    return oss.str();
}

std::shared_ptr<ob::Device> select_device(ob::Context &ctx, const CameraConfig &camera) {
    auto devices = ctx.queryDeviceList();
    if(devices->deviceCount() == 0) {
        throw std::runtime_error("no Orbbec device found");
    }
    if(!camera.serial_number.empty() || !camera.uid.empty()) {
        for(uint32_t i = 0; i < devices->deviceCount(); ++i) {
            const std::string serial = devices->serialNumber(i) ? devices->serialNumber(i) : "";
            const std::string uid = devices->uid(i) ? devices->uid(i) : "";
            const std::string paired_rgb_serial = paired_rgb_serial_for_depth_uid(uid);
            if((!camera.serial_number.empty() && (serial == camera.serial_number || paired_rgb_serial == camera.serial_number))
               || (!camera.uid.empty() && uid == camera.uid)) {
                return devices->getDevice(i);
            }
        }

        std::ostringstream oss;
        oss << "configured camera not found camera_id=" << camera.camera_id;
        if(!camera.serial_number.empty()) {
            oss << " serial=" << camera.serial_number;
        }
        if(!camera.uid.empty()) {
            oss << " uid=" << camera.uid;
        }
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

void mark_corrupt_rgb_jpeg_frame(CameraRuntime &camera, Logger &logger, const std::shared_ptr<ob::ColorFrame> &color,
                                 std::chrono::steady_clock::time_point frame_now, const std::string &reason) {
    camera.perf.rgb_corrupt_jpeg_frames++;
    camera.rgb_corrupt_jpeg++;
    camera.rgb_dropped++;
    camera.last_error = "corrupt rgb mjpeg frame dropped";
    if(frame_now >= camera.next_jpeg_warning) {
        std::ostringstream oss;
        oss << "corrupt rgb mjpeg frame dropped camera_id=" << camera.config.camera_id << " frame_id=" << color->index()
            << " size=" << color->dataSize() << " reason=\"" << reason << "\"";
        logger.warn(oss.str());
        camera.next_jpeg_warning = frame_now + std::chrono::seconds(1);
    }
}

void mark_media_send_failure(CameraRuntime &camera, Logger &logger, const std::string &stream_type,
                             std::chrono::steady_clock::time_point frame_now, const std::string &reason) {
    const std::string message = reason.empty() ? "unknown media transport error" : reason;
    camera.last_error = message;
    if(frame_now >= camera.next_media_warning || message != camera.last_media_warning) {
        logger.warn("media send failed camera_id=" + camera.config.camera_id + " stream=" + stream_type + " error=" + message);
        camera.next_media_warning = frame_now + std::chrono::seconds(2);
        camera.last_media_warning = message;
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
    capabilities["media_protocol"] = config.transport.media_protocol;
    capabilities["status_protocol"] = config.transport.status_protocol;
    msg["capabilities"] = capabilities;
    return msg;
}

Json::Value camera_announce(const AppConfig &config, CameraRuntime &camera) {
    Json::Value msg = base_message(config, "camera_announce");
    msg["camera_id"] = camera.config.camera_id;
    auto info = camera.device->getDeviceInfo();
    Json::Value device;
    device["vendor"] = "orbbec";
    device["model"] = info->name() ? info->name() : "";
    device["serial_number"] = info->serialNumber() ? info->serialNumber() : "";
    device["uid"] = info->uid() ? info->uid() : "";
    device["firmware_version"] = info->firmwareVersion() ? info->firmwareVersion() : "";
    device["connection_type"] = info->connectionType() ? info->connectionType() : "";
    msg["device"] = device;
    msg["rgb_profile"] = profile_json(camera.color_profile, "encoded_video", camera.config.rgb_encoding.codec);
    msg["depth_profile"] = profile_json(camera.depth_profile, "uint16", camera.config.depth_transport.compression, camera.depth_scale);
    msg["calibration"] = calibration_json(*camera.pipeline);
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

Json::Value heartbeat(const AppConfig &config, const std::vector<std::unique_ptr<CameraRuntime>> &cameras,
                      std::chrono::steady_clock::time_point started) {
    Json::Value msg = base_message(config, "heartbeat");
    const auto uptime = std::chrono::steady_clock::now() - started;
    msg["uptime_ms"] = Json::UInt64(std::chrono::duration_cast<std::chrono::milliseconds>(uptime).count());
    Json::Value list(Json::arrayValue);
    for(const auto &camera : cameras) {
        const auto seconds = std::max(0.001, std::chrono::duration<double>(std::chrono::steady_clock::now() - camera->stats_started).count());
        Json::Value item;
        item["camera_id"] = camera->config.camera_id;
        item["online"] = camera->online;
        item["rgb_fps"] = static_cast<double>(camera->rgb_frames) / seconds;
        item["depth_fps"] = static_cast<double>(camera->depth_frames) / seconds;
        item["rgb_encoding"] = camera->config.rgb_encoding.codec;
        item["depth_compression"] = camera->config.depth_transport.compression;
        item["rgb_dropped_frames"] = Json::UInt64(camera->rgb_dropped);
        item["rgb_corrupt_jpeg_frames"] = Json::UInt64(camera->rgb_corrupt_jpeg);
        item["depth_dropped_frames"] = Json::UInt64(camera->depth_dropped);
        item["hardware_encoder"] = camera->hardware_encoder;
        item["rgb_measured_fps"] = camera->live.rgb_input_fps;
        item["depth_measured_fps"] = camera->live.depth_input_fps;
        item["rgb_mbps"] = camera->live.rgb_mbps;
        item["depth_mbps"] = camera->live.depth_mbps;
        item["rgb_auto_exposure"] = Json::Int64(camera->live.color_auto_exposure);
        item["rgb_exposure"] = Json::Int64(camera->live.color_exposure);
        item["rgb_gain"] = Json::Int64(camera->live.color_gain);
        item["rgb_metadata_actual_fps"] = Json::Int64(camera->live.color_actual_fps);
        item["rgb_exposure_priority"] = Json::Int64(camera->live.color_exposure_priority);
        item["last_error"] = camera->last_error;
        list.append(item);
    }
    msg["cameras"] = list;
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
    const auto &bgr = camera.latest_bgr;
    const auto &depth_color = camera.latest_depth_color;
    if(!preview_enabled || bgr.empty() || depth_color.empty()) {
        return;
    }
    const std::string window_name = "Gemini Sender " + camera.config.camera_id;
    static std::set<std::string> initialized_windows;
    if(initialized_windows.insert(window_name).second) {
        cv::namedWindow(window_name, cv::WINDOW_NORMAL);
        cv::resizeWindow(window_name, 960, 360);
        cv::moveWindow(window_name, 80, 80);
    }
    cv::Mat rgb_small;
    cv::Mat depth_small;
    cv::resize(bgr, rgb_small, cv::Size(480, 360));
    cv::resize(depth_color, depth_small, cv::Size(480, 360));
    cv::Mat wall;
    cv::hconcat(rgb_small, depth_small, wall);
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

void stop_camera(CameraRuntime &camera, Logger &logger) {
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
    camera.pipeline.reset();
    camera.color_profile.reset();
    camera.depth_profile.reset();
    camera.device.reset();
    camera.online = false;
    camera.announced = false;
    camera.hardware_encoder = false;
    camera.latest_bgr.release();
    camera.latest_depth_color.release();
}

void start_camera_runtime(CameraRuntime &runtime, Logger &logger) {
    ob::Context::setLoggerSeverity(OB_LOG_SEVERITY_WARN);
    auto context = std::make_shared<ob::Context>();

    runtime.device = select_device(*context, runtime.config);
    apply_color_controls(runtime, logger);
    runtime.pipeline = std::make_unique<ob::Pipeline>(runtime.device);

    auto stream_config = std::make_shared<ob::Config>();
    runtime.color_profile = select_profile(*runtime.pipeline, OB_SENSOR_COLOR, runtime.config.rgb_profile, OB_FORMAT_MJPG, logger);
    runtime.depth_profile = select_profile(*runtime.pipeline, OB_SENSOR_DEPTH, runtime.config.depth_profile, OB_FORMAT_Y16, logger);
    stream_config->enableStream(runtime.color_profile);
    stream_config->enableStream(runtime.depth_profile);
    runtime.pipeline->start(stream_config);
    update_color_properties(runtime);

    const auto now = std::chrono::steady_clock::now();
    runtime.encoder.reset();
    runtime.online = true;
    runtime.announced = false;
    runtime.hardware_encoder = false;
    runtime.depth_scale = 0.0f;
    runtime.last_error.clear();
    runtime.perf = CameraPerfStats{};
    runtime.perf.interval_started = now;
    runtime.next_preview = now;
    runtime.next_time_sync_log = now;
    runtime.next_jpeg_warning = now;
    runtime.next_media_warning = now;
    runtime.last_media_warning.clear();
    runtime.latest_bgr.release();
    runtime.latest_depth_color.release();

    std::ostringstream oss;
    oss << "camera started camera_id=" << runtime.config.camera_id << " color=" << runtime.color_profile->width() << "x"
        << runtime.color_profile->height() << "@" << runtime.color_profile->fps() << " format=" << ob_format_name(runtime.color_profile->format())
        << " depth=" << runtime.depth_profile->width() << "x" << runtime.depth_profile->height() << "@" << runtime.depth_profile->fps()
        << " format=" << ob_format_name(runtime.depth_profile->format());
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
void mark_camera_disconnected(const AppConfig &config, CameraRuntime &camera, Sender &transport, Logger &logger, const std::string &error) {
    camera.disconnects++;
    camera.reconnect_attempts = 0;
    camera.last_error = error;
    logger.error("camera disconnected camera_id=" + camera.config.camera_id + " error=" + error);
    send_status(transport, logger, camera_offline_message(config, camera.config.camera_id, error));
    stop_camera(camera, logger);
    camera.next_reconnect = std::chrono::steady_clock::now() + std::chrono::seconds(1);
}

template <typename Sender>
void retry_camera_reconnect(const AppConfig &config, CameraRuntime &camera, Sender &transport, Logger &logger,
                            std::chrono::steady_clock::time_point now) {
    if(now < camera.next_reconnect) {
        return;
    }

    camera.reconnect_attempts++;
    logger.warn("camera reconnect attempt camera_id=" + camera.config.camera_id + " attempt=" + std::to_string(camera.reconnect_attempts));
    try {
        start_camera_runtime(camera, logger);
        camera.reconnect_attempts = 0;
        logger.info("camera reconnected camera_id=" + camera.config.camera_id);
        send_status(transport, logger,
                    event_message(config, "info", "camera_reconnected", "camera pipeline restarted", camera.config.camera_id));
    }
    catch(const ob::Error &e) {
        stop_camera(camera, logger);
        camera.last_error = ob_error_text(e);
        logger.warn("camera reconnect failed camera_id=" + camera.config.camera_id + " attempt=" + std::to_string(camera.reconnect_attempts)
                    + " error=" + camera.last_error);
        send_status(transport, logger, camera_offline_message(config, camera.config.camera_id, camera.last_error));
        camera.next_reconnect = now + reconnect_delay(camera.reconnect_attempts);
    }
    catch(const std::exception &e) {
        stop_camera(camera, logger);
        camera.last_error = e.what();
        logger.warn("camera reconnect failed camera_id=" + camera.config.camera_id + " attempt=" + std::to_string(camera.reconnect_attempts)
                    + " error=" + camera.last_error);
        send_status(transport, logger, camera_offline_message(config, camera.config.camera_id, camera.last_error));
        camera.next_reconnect = now + reconnect_delay(camera.reconnect_attempts);
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
    send_status(transport, logger, sender_hello(config));
    for(const auto &camera : cameras) {
        if(camera->online) {
            send_status(transport, logger,
                        event_message(config, "info", "camera_connected", "camera pipeline started", camera->config.camera_id));
        }
        else {
            send_status(transport, logger, camera_offline_message(config, camera->config.camera_id, camera->last_error));
        }
    }

    auto next_heartbeat = std::chrono::steady_clock::now();
    auto next_perf_log = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    const auto stop_at = args.run_seconds > 0 ? started + std::chrono::seconds(args.run_seconds) : std::chrono::steady_clock::time_point::max();

    while(g_running && std::chrono::steady_clock::now() < stop_at) {
        for(auto &camera : cameras) {
            const auto loop_now = std::chrono::steady_clock::now();
            if(!camera->online) {
                retry_camera_reconnect(config, *camera, transport, logger, loop_now);
                continue;
            }

            std::shared_ptr<ob::FrameSet> frameset;
            std::shared_ptr<ob::ColorFrame> color;
            std::shared_ptr<ob::DepthFrame> depth;
            try {
                const auto wait_started = std::chrono::steady_clock::now();
                frameset = camera->pipeline->waitForFrames(100);
                const auto wait_ended = std::chrono::steady_clock::now();
                camera->perf.wait_calls++;
                camera->perf.wait_ms += elapsed_ms(wait_started, wait_ended);
                if(!frameset) {
                    camera->perf.wait_timeouts++;
                    continue;
                }
                color = frameset->colorFrame();
                depth = frameset->depthFrame();
            }
            catch(const ob::Error &e) {
                mark_camera_disconnected(config, *camera, transport, logger, ob_error_text(e));
                continue;
            }
            catch(const std::exception &e) {
                mark_camera_disconnected(config, *camera, transport, logger, e.what());
                continue;
            }
            const auto frame_now = std::chrono::steady_clock::now();
            const uint64_t frame_host_now_us = now_us();
            const bool preview_due = config.preview.enabled && frame_now >= camera->next_preview;

            cv::Mat bgr;
            if(color) {
                camera->perf.rgb_input_frames++;
                camera->perf.rgb_input_bytes += color->dataSize();
                camera->perf.note_rgb_frame_id(color->index());
                update_color_metadata(*camera, color);
                const bool color_is_mjpg = color->format() == OB_FORMAT_MJPG;
                bool rgb_usable = true;
                if(color_is_mjpg && !mjpg_has_complete_jpeg(color)) {
                    rgb_usable = false;
                    mark_corrupt_rgb_jpeg_frame(*camera, logger, color, frame_now, "missing jpeg soi/eoi marker");
                }
                else if(color_is_mjpg && (preview_due || camera->config.validate_rgb_mjpeg)) {
                    std::string jpeg_validation_message;
                    if(!mjpg_decodes_cleanly(color, jpeg_validation_message)) {
                        rgb_usable = false;
                        mark_corrupt_rgb_jpeg_frame(*camera, logger, color, frame_now, jpeg_validation_message);
                    }
                }

                if(rgb_usable) {
                    if(color_is_mjpg && preview_due) {
                        const auto decode_started = std::chrono::steady_clock::now();
                        auto preview_bgr = color_to_preview_bgr(color);
                        camera->perf.rgb_decode_ms += elapsed_ms(decode_started, std::chrono::steady_clock::now());
                        if(!preview_bgr.empty()) {
                            camera->latest_bgr = preview_bgr;
                        }
                    }
                    else if(!color_is_mjpg) {
                        const auto decode_started = std::chrono::steady_clock::now();
                        bgr = color_to_bgr(color);
                        camera->perf.rgb_decode_ms += elapsed_ms(decode_started, std::chrono::steady_clock::now());
                        if(preview_due && !bgr.empty()) {
                            camera->latest_bgr = bgr;
                        }
                    }
                    if(!camera->encoder && (color_is_mjpg || !bgr.empty())) {
                        camera->encoder_input_format = color_is_mjpg ? GstH264InputFormat::Jpeg : GstH264InputFormat::Bgr;
                        camera->encoder = std::make_unique<GstH264Encoder>(color->width(), color->height(), camera->color_profile->fps(),
                                                                            camera->config.rgb_encoding.bitrate_bps,
                                                                            camera->config.rgb_encoding.gstreamer_encoder,
                                                                            camera->encoder_input_format);
                        if(!camera->encoder->ok() && color_is_mjpg) {
                            logger.warn("mppjpegdec rgb path unavailable, falling back to BGR encode path: " + camera->encoder->error());
                            camera->encoder_input_format = GstH264InputFormat::Bgr;
                            camera->encoder = std::make_unique<GstH264Encoder>(color->width(), color->height(), camera->color_profile->fps(),
                                                                                camera->config.rgb_encoding.bitrate_bps,
                                                                                camera->config.rgb_encoding.gstreamer_encoder,
                                                                                camera->encoder_input_format);
                        }
                        camera->hardware_encoder = camera->encoder->ok();
                        if(!camera->hardware_encoder) {
                            camera->last_error = camera->encoder->error();
                            logger.error("encoder_init_failed: " + camera->last_error);
                            send_status(transport, logger,
                                        event_message(config, "error", "encoder_init_failed", camera->last_error, camera->config.camera_id));
                        }
                    }
                    if(camera->encoder && camera->encoder->ok()) {
                        try {
                            if(camera->encoder_input_format == GstH264InputFormat::Bgr && bgr.empty()) {
                                const auto decode_started = std::chrono::steady_clock::now();
                                bgr = color_to_bgr(color);
                                camera->perf.rgb_decode_ms += elapsed_ms(decode_started, std::chrono::steady_clock::now());
                            }
                            if(camera->encoder_input_format == GstH264InputFormat::Bgr && bgr.empty()) {
                                camera->last_error = "rgb decode produced empty frame";
                                camera->rgb_dropped++;
                            }
                            else {
                                const uint64_t rgb_system_timestamp_us = frame_system_timestamp_us_or(color, frame_host_now_us);
                                const auto encode_started = std::chrono::steady_clock::now();
                                const auto encoded_units = camera->encoder_input_format == GstH264InputFormat::Jpeg
                                                               ? camera->encoder->encode_jpeg(color->data(), color->dataSize(), color->timeStampUs())
                                                               : camera->encoder->encode_bgr(bgr, color->timeStampUs());
                                camera->perf.rgb_encode_ms += elapsed_ms(encode_started, std::chrono::steady_clock::now());
                                for(const auto &encoded : encoded_units) {
                                    MediaFrameMeta meta;
                                    meta.stream_type = StreamType::rgb;
                                    meta.flags = has_system_timestamp | (h264_payload_has_idr(encoded) ? key_frame : 0u);
                                    meta.sender_id = config.sender_id;
                                    meta.camera_id = camera->config.camera_id;
                                    meta.codec_or_compression = "h264";
                                    meta.frame_id = color->index();
                                    meta.timestamp_us = rgb_system_timestamp_us;
                                    meta.system_timestamp_us = rgb_system_timestamp_us;
                                    meta.width = color->width();
                                    meta.height = color->height();
                                    meta.pixel_format = PixelFormat::encoded_video;
                                    meta.payload_size = encoded.size();
                                    meta.uncompressed_size = encoded.size();
                                    const auto packet = build_media_packet(meta, encoded.data());
                                    const auto send_started = std::chrono::steady_clock::now();
                                    const bool sent = transport.send_media(packet);
                                    camera->perf.rgb_send_ms += elapsed_ms(send_started, std::chrono::steady_clock::now());
                                    if(sent) {
                                        camera->perf.rgb_sent_packets++;
                                        camera->perf.rgb_bytes += packet.size();
                                    }
                                    else {
                                        camera->perf.rgb_send_failures++;
                                        mark_media_send_failure(*camera, logger, "rgb", frame_now, transport.last_error());
                                    }
                                }
                            }
                        }
                        catch(const std::exception &e) {
                            camera->last_error = e.what();
                            logger.error("rgb encode failed: " + camera->last_error);
                        }
                    }
                }
                camera->rgb_frames++;
            }

            cv::Mat depth_color;
            if(depth) {
                camera->perf.depth_input_frames++;
                camera->perf.depth_input_bytes += depth->dataSize();
                camera->perf.note_depth_frame_id(depth->index());
                if(camera->depth_scale == 0.0f) {
                    camera->depth_scale = depth->getValueScale();
                }
                std::vector<uint8_t> compressed_depth;
                const void *depth_payload = depth->data();
                size_t depth_payload_size = depth->dataSize();
                if(camera->config.depth_transport.compression == "zlib") {
                    const auto compress_started = std::chrono::steady_clock::now();
                    compressed_depth = zlib_compress_payload(depth->data(), depth->dataSize());
                    camera->perf.depth_compress_ms += elapsed_ms(compress_started, std::chrono::steady_clock::now());
                    depth_payload = compressed_depth.data();
                    depth_payload_size = compressed_depth.size();
                }
                const uint64_t depth_system_timestamp_us = frame_system_timestamp_us_or(depth, frame_host_now_us);
                MediaFrameMeta meta;
                meta.stream_type = StreamType::depth_raw;
                meta.flags = has_system_timestamp;
                meta.sender_id = config.sender_id;
                meta.camera_id = camera->config.camera_id;
                meta.codec_or_compression = camera->config.depth_transport.compression;
                meta.frame_id = depth->index();
                meta.timestamp_us = depth_system_timestamp_us;
                meta.system_timestamp_us = depth_system_timestamp_us;
                meta.width = depth->width();
                meta.height = depth->height();
                meta.pixel_format = PixelFormat::depth_u16;
                meta.payload_size = depth_payload_size;
                meta.uncompressed_size = depth->dataSize();
                const auto packet = build_media_packet(meta, depth_payload);
                const auto send_started = std::chrono::steady_clock::now();
                const bool sent = transport.send_media(packet);
                camera->perf.depth_send_ms += elapsed_ms(send_started, std::chrono::steady_clock::now());
                if(sent) {
                    camera->perf.depth_sent_frames++;
                    camera->perf.depth_bytes += packet.size();
                }
                else {
                    camera->perf.depth_send_failures++;
                    mark_media_send_failure(*camera, logger, "depth", frame_now, transport.last_error());
                }
                camera->depth_frames++;
                if(preview_due) {
                    const auto depth_preview_started = std::chrono::steady_clock::now();
                    depth_color = depth_to_color(depth);
                    camera->perf.depth_preview_ms += elapsed_ms(depth_preview_started, std::chrono::steady_clock::now());
                    if(!depth_color.empty()) {
                        camera->latest_depth_color = depth_color;
                    }
                }
            }

            if(!camera->announced && depth && color) {
                send_status(transport, logger, camera_announce(config, *camera));
                camera->announced = true;
            }
            log_time_sync(*camera, logger, color, depth, frame_now);
            if(preview_due) {
                const auto preview_started = std::chrono::steady_clock::now();
                preview_frame(*camera, true);
                camera->perf.preview_ms += elapsed_ms(preview_started, std::chrono::steady_clock::now());
                camera->next_preview = frame_now + preview_interval;
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if(now >= next_heartbeat) {
            send_status(transport, logger, heartbeat(config, cameras, started));
            next_heartbeat = now + std::chrono::milliseconds(config.heartbeat_interval_ms);
        }
        if(now >= next_perf_log) {
            for(auto &camera : cameras) {
                update_color_properties(*camera);
                log_perf(*camera, logger, now);
            }
            next_perf_log = now + std::chrono::seconds(1);
        }
    }

    for(auto &camera : cameras) {
        std::ostringstream oss;
        oss << "camera summary camera_id=" << camera->config.camera_id << " rgb_frames=" << camera->rgb_frames
            << " depth_frames=" << camera->depth_frames << " hardware_encoder=" << (camera->hardware_encoder ? "true" : "false")
            << " depth_scale=" << camera->depth_scale << " disconnects=" << camera->disconnects;
        if(!camera->last_error.empty()) {
            oss << " last_error=" << camera->last_error;
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
