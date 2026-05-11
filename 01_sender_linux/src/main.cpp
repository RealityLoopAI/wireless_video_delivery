#include "gwv3_common/protocol.hpp"
#include "gwv3_sender/config.hpp"
#include "gwv3_sender/gst_h264_encoder.hpp"
#include "gwv3_sender/logger.hpp"
#include "gwv3_sender/transport.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <thread>

#include <json/json.h>
#include <libobsensor/ObSensor.hpp>
#include <libobsensor/hpp/Error.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <zlib.h>

namespace gwv3 {

namespace {

std::atomic<bool> g_running{true};

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

struct CameraRuntime {
    CameraConfig config;
    std::shared_ptr<ob::Device> device;
    std::unique_ptr<ob::Pipeline> pipeline;
    std::shared_ptr<ob::VideoStreamProfile> color_profile;
    std::shared_ptr<ob::VideoStreamProfile> depth_profile;
    std::unique_ptr<GstH264Encoder> encoder;
    bool announced = false;
    bool hardware_encoder = false;
    uint64_t rgb_frames = 0;
    uint64_t depth_frames = 0;
    uint64_t rgb_dropped = 0;
    uint64_t depth_dropped = 0;
    std::string last_error;
    float depth_scale = 0.0f;
    std::chrono::steady_clock::time_point stats_started = std::chrono::steady_clock::now();
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

std::shared_ptr<ob::Device> select_device(ob::Context &ctx, const CameraConfig &camera) {
    auto devices = ctx.queryDeviceList();
    if(devices->deviceCount() == 0) {
        throw std::runtime_error("no Orbbec device found");
    }
    for(uint32_t i = 0; i < devices->deviceCount(); ++i) {
        auto device = devices->getDevice(i);
        auto info = device->getDeviceInfo();
        const std::string serial = info->serialNumber() ? info->serialNumber() : "";
        const std::string uid = info->uid() ? info->uid() : "";
        if((!camera.serial_number.empty() && serial == camera.serial_number) || (!camera.uid.empty() && uid == camera.uid)) {
            return device;
        }
    }
    if(camera.device_index < 0 || static_cast<uint32_t>(camera.device_index) >= devices->deviceCount()) {
        throw std::runtime_error("camera device_index out of range");
    }
    return devices->getDevice(static_cast<uint32_t>(camera.device_index));
}

cv::Mat color_to_bgr(const std::shared_ptr<ob::ColorFrame> &frame) {
    const auto format = frame->format();
    if(format == OB_FORMAT_MJPG) {
        cv::Mat raw(1, static_cast<int>(frame->dataSize()), CV_8UC1, frame->data());
        return cv::imdecode(raw, cv::IMREAD_COLOR);
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

cv::Mat depth_to_color(const std::shared_ptr<ob::DepthFrame> &frame) {
    cv::Mat depth(frame->height(), frame->width(), CV_16UC1, frame->data());
    cv::Mat depth8;
    cv::normalize(depth, depth8, 0, 255, cv::NORM_MINMAX, CV_8U);
    cv::Mat color;
    cv::applyColorMap(depth8, color, cv::COLORMAP_JET);
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
        item["online"] = true;
        item["rgb_fps"] = static_cast<double>(camera->rgb_frames) / seconds;
        item["depth_fps"] = static_cast<double>(camera->depth_frames) / seconds;
        item["rgb_encoding"] = camera->config.rgb_encoding.codec;
        item["depth_compression"] = camera->config.depth_transport.compression;
        item["rgb_dropped_frames"] = Json::UInt64(camera->rgb_dropped);
        item["depth_dropped_frames"] = Json::UInt64(camera->depth_dropped);
        item["hardware_encoder"] = camera->hardware_encoder;
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

void preview_frame(const std::string &camera_id, const cv::Mat &bgr, const cv::Mat &depth_color, bool preview_enabled) {
    if(!preview_enabled || bgr.empty() || depth_color.empty()) {
        return;
    }
    cv::Mat rgb_small;
    cv::Mat depth_small;
    cv::resize(bgr, rgb_small, cv::Size(480, 360));
    cv::resize(depth_color, depth_small, cv::Size(480, 360));
    cv::Mat wall;
    cv::hconcat(rgb_small, depth_small, wall);
    cv::imshow("Gemini Sender " + camera_id, wall);
    cv::waitKey(1);
}

std::vector<std::unique_ptr<CameraRuntime>> start_cameras(const AppConfig &config, Logger &logger) {
    std::vector<std::unique_ptr<CameraRuntime>> cameras;
    ob::Context::setLoggerSeverity(OB_LOG_SEVERITY_WARN);
    auto context = std::make_shared<ob::Context>();

    for(const auto &camera_config : config.cameras) {
        auto runtime = std::make_unique<CameraRuntime>();
        runtime->config = camera_config;
        runtime->device = select_device(*context, camera_config);
        runtime->pipeline = std::make_unique<ob::Pipeline>(runtime->device);

        auto stream_config = std::make_shared<ob::Config>();
        runtime->color_profile = select_profile(*runtime->pipeline, OB_SENSOR_COLOR, camera_config.rgb_profile, OB_FORMAT_MJPG, logger);
        runtime->depth_profile = select_profile(*runtime->pipeline, OB_SENSOR_DEPTH, camera_config.depth_profile, OB_FORMAT_Y16, logger);
        stream_config->enableStream(runtime->color_profile);
        stream_config->enableStream(runtime->depth_profile);
        runtime->pipeline->start(stream_config);

        std::ostringstream oss;
        oss << "camera started camera_id=" << camera_config.camera_id << " color=" << runtime->color_profile->width() << "x"
            << runtime->color_profile->height() << "@" << runtime->color_profile->fps() << " format=" << ob_format_name(runtime->color_profile->format())
            << " depth=" << runtime->depth_profile->width() << "x" << runtime->depth_profile->height() << "@" << runtime->depth_profile->fps()
            << " format=" << ob_format_name(runtime->depth_profile->format());
        logger.info(oss.str());

        cameras.push_back(std::move(runtime));
    }
    return cameras;
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

    const auto started = std::chrono::steady_clock::now();
    auto cameras = start_cameras(config, logger);
    send_status(transport, logger, sender_hello(config));
    send_status(transport, logger, event_message(config, "info", "camera_connected", "camera pipeline started", cameras.front()->config.camera_id));

    auto next_heartbeat = std::chrono::steady_clock::now();
    const auto stop_at = args.run_seconds > 0 ? started + std::chrono::seconds(args.run_seconds) : std::chrono::steady_clock::time_point::max();

    while(g_running && std::chrono::steady_clock::now() < stop_at) {
        for(auto &camera : cameras) {
            auto frameset = camera->pipeline->waitForFrames(100);
            if(!frameset) {
                continue;
            }
            auto color = frameset->colorFrame();
            auto depth = frameset->depthFrame();

            cv::Mat bgr;
            if(color) {
                bgr = color_to_bgr(color);
                if(!bgr.empty() && !camera->encoder) {
                    camera->encoder = std::make_unique<GstH264Encoder>(bgr.cols, bgr.rows, camera->color_profile->fps(),
                                                                        camera->config.rgb_encoding.bitrate_bps,
                                                                        camera->config.rgb_encoding.gstreamer_encoder);
                    camera->hardware_encoder = camera->encoder->ok();
                    if(!camera->hardware_encoder) {
                        camera->last_error = camera->encoder->error();
                        logger.error("encoder_init_failed: " + camera->last_error);
                        send_status(transport, logger,
                                    event_message(config, "error", "encoder_init_failed", camera->last_error, camera->config.camera_id));
                    }
                }
                if(camera->encoder && camera->encoder->ok() && !bgr.empty()) {
                    try {
                        const auto encoded_units = camera->encoder->encode_bgr(bgr, color->timeStampUs());
                        for(const auto &encoded : encoded_units) {
                            MediaFrameMeta meta;
                            meta.stream_type = StreamType::rgb;
                            meta.flags = has_system_timestamp | (h264_payload_has_idr(encoded) ? key_frame : 0u);
                            meta.sender_id = config.sender_id;
                            meta.camera_id = camera->config.camera_id;
                            meta.codec_or_compression = "h264";
                            meta.frame_id = color->index();
                            meta.timestamp_us = color->timeStampUs();
                            meta.system_timestamp_us = color->systemTimeStampUs();
                            meta.width = static_cast<uint32_t>(bgr.cols);
                            meta.height = static_cast<uint32_t>(bgr.rows);
                            meta.pixel_format = PixelFormat::encoded_video;
                            meta.payload_size = encoded.size();
                            meta.uncompressed_size = encoded.size();
                            const auto packet = build_media_packet(meta, encoded.data());
                            if(!transport.send_media(packet)) {
                                camera->last_error = transport.last_error();
                            }
                        }
                    }
                    catch(const std::exception &e) {
                        camera->last_error = e.what();
                        logger.error("rgb encode failed: " + camera->last_error);
                    }
                }
                camera->rgb_frames++;
            }

            cv::Mat depth_color;
            if(depth) {
                if(camera->depth_scale == 0.0f) {
                    camera->depth_scale = depth->getValueScale();
                }
                std::vector<uint8_t> compressed_depth;
                const void *depth_payload = depth->data();
                size_t depth_payload_size = depth->dataSize();
                if(camera->config.depth_transport.compression == "zlib") {
                    compressed_depth = zlib_compress_payload(depth->data(), depth->dataSize());
                    depth_payload = compressed_depth.data();
                    depth_payload_size = compressed_depth.size();
                }
                MediaFrameMeta meta;
                meta.stream_type = StreamType::depth_raw;
                meta.flags = has_system_timestamp;
                meta.sender_id = config.sender_id;
                meta.camera_id = camera->config.camera_id;
                meta.codec_or_compression = camera->config.depth_transport.compression;
                meta.frame_id = depth->index();
                meta.timestamp_us = depth->timeStampUs();
                meta.system_timestamp_us = depth->systemTimeStampUs();
                meta.width = depth->width();
                meta.height = depth->height();
                meta.pixel_format = PixelFormat::depth_u16;
                meta.payload_size = depth_payload_size;
                meta.uncompressed_size = depth->dataSize();
                const auto packet = build_media_packet(meta, depth_payload);
                if(!transport.send_media(packet)) {
                    camera->last_error = transport.last_error();
                }
                camera->depth_frames++;
                depth_color = depth_to_color(depth);
            }

            if(!camera->announced && depth && color) {
                send_status(transport, logger, camera_announce(config, *camera));
                camera->announced = true;
            }
            preview_frame(camera->config.camera_id, bgr, depth_color, config.preview.enabled);
        }

        const auto now = std::chrono::steady_clock::now();
        if(now >= next_heartbeat) {
            send_status(transport, logger, heartbeat(config, cameras, started));
            next_heartbeat = now + std::chrono::milliseconds(config.heartbeat_interval_ms);
        }
    }

    for(auto &camera : cameras) {
        if(camera->pipeline) {
            camera->pipeline->stop();
        }
        std::ostringstream oss;
        oss << "camera summary camera_id=" << camera->config.camera_id << " rgb_frames=" << camera->rgb_frames
            << " depth_frames=" << camera->depth_frames << " hardware_encoder=" << (camera->hardware_encoder ? "true" : "false")
            << " depth_scale=" << camera->depth_scale;
        if(!camera->last_error.empty()) {
            oss << " last_error=" << camera->last_error;
        }
        logger.info(oss.str());
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
