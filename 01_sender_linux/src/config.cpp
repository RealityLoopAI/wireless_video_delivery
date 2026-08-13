#include "gwv3_sender/config.hpp"

#include "gwv3_common/protocol.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <regex>
#include <sstream>
#include <set>
#include <stdexcept>
#include <vector>

#include <arpa/inet.h>
#include <unistd.h>

#include <json/json.h>

namespace gwv3 {

namespace {

std::string required_string(const Json::Value &node, const char *key) {
    if(!node.isMember(key) || !node[key].isString()) {
        throw std::runtime_error(std::string("missing or invalid string field: ") + key);
    }
    return node[key].asString();
}

std::string optional_string(const Json::Value &node, const char *key, const std::string &fallback) {
    if(!node.isMember(key)) {
        return fallback;
    }
    if(!node[key].isString()) {
        throw std::runtime_error(std::string("invalid string field: ") + key);
    }
    return node[key].asString();
}

int optional_int(const Json::Value &node, const char *key, int fallback) {
    if(!node.isMember(key)) {
        return fallback;
    }
    if(!node[key].isInt()) {
        throw std::runtime_error(std::string("invalid integer field: ") + key);
    }
    return node[key].asInt();
}

bool optional_bool(const Json::Value &node, const char *key, bool fallback) {
    if(!node.isMember(key)) {
        return fallback;
    }
    if(!node[key].isBool()) {
        throw std::runtime_error(std::string("invalid boolean field: ") + key);
    }
    return node[key].asBool();
}

double optional_double(const Json::Value &node, const char *key, double fallback) {
    if(!node.isMember(key)) {
        return fallback;
    }
    if(!node[key].isNumeric()) {
        throw std::runtime_error(std::string("invalid numeric field: ") + key);
    }
    return node[key].asDouble();
}

std::string trim_copy(std::string value) {
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

std::string read_first_line(const std::filesystem::path &path) {
    std::ifstream input(path);
    std::string line;
    if(input && std::getline(input, line)) {
        return trim_copy(line);
    }
    return "";
}

std::string sanitize_protocol_part(std::string value) {
    for(char &ch : value) {
        const auto c = static_cast<unsigned char>(ch);
        if(std::isalnum(c) || ch == '_' || ch == '-') {
            ch = static_cast<char>(std::tolower(c));
        }
        else {
            ch = '-';
        }
    }
    while(!value.empty() && value.front() == '-') {
        value.erase(value.begin());
    }
    while(!value.empty() && value.back() == '-') {
        value.pop_back();
    }
    return value.empty() ? "sender" : value;
}

bool is_valid_mac_address(const std::string &value) {
    static const std::regex pattern("^[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}$");
    if(!std::regex_match(value, pattern)) {
        return false;
    }

    std::string compact;
    for(char ch : value) {
        if(ch != ':') {
            compact.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
    }
    return compact != "000000000000";
}

std::string suffix_from_mac(std::string mac) {
    std::string compact;
    for(char ch : mac) {
        if(ch != ':') {
            compact.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
    }
    if(compact.size() <= 8) {
        return compact;
    }
    return compact.substr(compact.size() - 8);
}

std::string mac_for_interface(const std::string &interface_name) {
    const auto address = read_first_line(std::filesystem::path("/sys/class/net") / interface_name / "address");
    return is_valid_mac_address(address) ? address : "";
}

std::string first_available_mac() {
    std::vector<std::string> names;
    const std::filesystem::path net_dir("/sys/class/net");
    if(std::filesystem::exists(net_dir)) {
        for(const auto &entry : std::filesystem::directory_iterator(net_dir)) {
            names.push_back(entry.path().filename().string());
        }
    }
    std::sort(names.begin(), names.end());

    // The default route can move between Ethernet and Wi-Fi. Prefer the first
    // physical interface in stable name order so auto IDs do not move with it.
    for(const bool physical_only : {true, false}) {
        for(const auto &name : names) {
            if(name == "lo") {
                continue;
            }
            if(physical_only
               && !std::filesystem::exists(std::filesystem::path("/sys/class/net") / name / "device")) {
                continue;
            }
            const auto mac = mac_for_interface(name);
            if(!mac.empty()) {
                return mac;
            }
        }
    }
    return "";
}

std::string machine_id_suffix() {
    auto machine_id = read_first_line("/etc/machine-id");
    machine_id.erase(std::remove_if(machine_id.begin(), machine_id.end(), [](unsigned char c) { return !std::isxdigit(c); }),
                     machine_id.end());
    std::transform(machine_id.begin(), machine_id.end(), machine_id.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if(machine_id.empty()) {
        return "";
    }
    if(machine_id.size() <= 8) {
        return machine_id;
    }
    return machine_id.substr(machine_id.size() - 8);
}

std::string host_prefix() {
    char buffer[256] = {};
    if(gethostname(buffer, sizeof(buffer) - 1) == 0 && buffer[0] != '\0') {
        return sanitize_protocol_part(buffer);
    }
    return "sender";
}

std::string derive_auto_sender_id() {
    std::string suffix;
    const auto mac = first_available_mac();
    if(!mac.empty()) {
        suffix = suffix_from_mac(mac);
    }
    if(suffix.empty()) {
        suffix = machine_id_suffix();
    }
    if(suffix.empty()) {
        throw std::runtime_error("cannot derive auto sender_id: no usable network MAC or /etc/machine-id");
    }

    auto prefix = host_prefix();
    constexpr size_t max_id_len = 64;
    if(prefix.size() + 1 + suffix.size() > max_id_len) {
        prefix.resize(max_id_len - suffix.size() - 1);
    }
    return prefix + "-" + suffix;
}

std::optional<int> optional_int_value(const Json::Value &node, const char *key) {
    if(!node.isMember(key)) {
        return std::nullopt;
    }
    if(!node[key].isInt()) {
        throw std::runtime_error(std::string("invalid integer field: ") + key);
    }
    return node[key].asInt();
}

std::optional<bool> optional_bool_value(const Json::Value &node, const char *key) {
    if(!node.isMember(key)) {
        return std::nullopt;
    }
    if(!node[key].isBool()) {
        throw std::runtime_error(std::string("invalid boolean field: ") + key);
    }
    return node[key].asBool();
}

VideoProfileConfig load_profile(const Json::Value &node) {
    VideoProfileConfig profile;
    profile.enabled = optional_bool(node, "enabled", profile.enabled);
    profile.width = optional_int(node, "width", 0);
    profile.height = optional_int(node, "height", 0);
    profile.fps = optional_int(node, "fps", 0);
    profile.format = optional_string(node, "format", "");
    return profile;
}

ColorControlsConfig load_color_controls(const Json::Value &node) {
    ColorControlsConfig controls;
    if(node.isNull()) {
        return controls;
    }
    if(!node.isObject()) {
        throw std::runtime_error("invalid object field: color_controls");
    }
    controls.auto_exposure = optional_bool_value(node, "auto_exposure");
    controls.exposure = optional_int_value(node, "exposure");
    controls.gain = optional_int_value(node, "gain");
    controls.auto_exposure_priority = optional_int_value(node, "auto_exposure_priority");
    controls.max_exposure = optional_int_value(node, "max_exposure");
    controls.max_gain = optional_int_value(node, "max_gain");
    controls.power_line_frequency = optional_int_value(node, "power_line_frequency");
    controls.auto_white_balance = optional_bool_value(node, "auto_white_balance");
    controls.white_balance = optional_int_value(node, "white_balance");
    controls.brightness = optional_int_value(node, "brightness");
    controls.contrast = optional_int_value(node, "contrast");
    controls.saturation = optional_int_value(node, "saturation");
    controls.gamma = optional_int_value(node, "gamma");
    controls.backlight_compensation = optional_int_value(node, "backlight_compensation");
    return controls;
}

AdaptiveExposureConfig load_adaptive_exposure(const Json::Value &node) {
    AdaptiveExposureConfig config;
    if(node.isNull()) {
        return config;
    }
    if(!node.isObject()) {
        throw std::runtime_error("invalid object field: adaptive_exposure");
    }
    config.enabled = optional_bool(node, "enabled", config.enabled);
    config.control_mode = optional_string(node, "control_mode", config.control_mode);
    config.interval_ms = optional_int(node, "interval_ms", config.interval_ms);
    config.stable_interval_ms = optional_int(node, "stable_interval_ms", config.interval_ms);
    config.settle_ms = optional_int(node, "settle_ms", config.interval_ms);
    config.discard_frames_after_adjustment = optional_int(
        node, "discard_frames_after_adjustment", config.discard_frames_after_adjustment);
    config.direction_reversal_samples = optional_int(
        node, "direction_reversal_samples", config.direction_reversal_samples);
    config.max_exposure_step = optional_int(node, "max_exposure_step", config.max_exposure_step);
    config.max_recovery_exposure_step = optional_int(
        node, "max_recovery_exposure_step", config.max_recovery_exposure_step);
    config.max_gain_step = optional_int(node, "max_gain_step", config.max_gain_step);
    config.exposure_min = optional_int(node, "exposure_min", config.exposure_min);
    config.exposure_max = optional_int(node, "exposure_max", config.exposure_max);
    config.soft_highlight_exposure_floor =
        optional_int(node, "soft_highlight_exposure_floor", config.soft_highlight_exposure_floor);
    config.gain_min = optional_int(node, "gain_min", config.gain_min);
    config.gain_max = optional_int(node, "gain_max", config.gain_max);
    config.target_p50_luma = optional_int(node, "target_p50_luma", config.target_p50_luma);
    config.target_p95_luma = optional_int(node, "target_p95_luma", config.target_p95_luma);
    config.luma_deadband = optional_int(node, "luma_deadband", config.luma_deadband);
    config.soft_highlight_luma = optional_int(node, "soft_highlight_luma", config.soft_highlight_luma);
    config.highlight_luma = optional_int(node, "highlight_luma", config.highlight_luma);
    config.max_highlight_fraction =
        optional_double(node, "max_highlight_fraction", config.max_highlight_fraction);
    config.highlight_recovery_ratio =
        optional_double(node, "highlight_recovery_ratio", config.highlight_recovery_ratio);
    config.highlight_release_samples = optional_int(
        node, "highlight_release_samples", config.highlight_release_samples);
    config.underexposed_samples = optional_int(node, "underexposed_samples", config.underexposed_samples);
    config.roi_margin_percent = optional_int(node, "roi_margin_percent", config.roi_margin_percent);
    config.metering_window = optional_int(node, "metering_window", config.metering_window);
    config.pid_kp = optional_double(node, "pid_kp", config.pid_kp);
    config.pid_ki = optional_double(node, "pid_ki", config.pid_ki);
    config.pid_kd = optional_double(node, "pid_kd", config.pid_kd);
    config.pid_integral_limit = optional_double(node, "pid_integral_limit", config.pid_integral_limit);
    config.pid_derivative_alpha = optional_double(node, "pid_derivative_alpha", config.pid_derivative_alpha);
    return config;
}

uint16_t parse_port(const Json::Value &node, const char *key, uint16_t fallback) {
    int value = optional_int(node, key, fallback);
    if(value <= 0 || value > 65535) {
        throw std::runtime_error(std::string("invalid port field: ") + key);
    }
    return static_cast<uint16_t>(value);
}

}  // namespace

bool is_valid_protocol_id(const std::string &value) {
    static const std::regex pattern("^[A-Za-z0-9_-]{1,64}$");
    return std::regex_match(value, pattern);
}

std::optional<int> adaptive_exposure_max_for_model(const std::string &device_model) {
    std::string lower = device_model;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if(lower.find("gemini 305") != std::string::npos) {
        return 300;
    }
    if(lower.find("sv1301s_u3") != std::string::npos) {
        return 325;
    }
    return std::nullopt;
}

AppConfig load_config(const std::string &path) {
    std::ifstream input(path);
    if(!input) {
        throw std::runtime_error("cannot open config file: " + path);
    }

    const std::string json((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    Json::CharReaderBuilder builder;
    builder["collectComments"] = false;
    builder["failIfExtra"] = true;
    builder["strictRoot"] = true;
    Json::Value root;
    std::string errors;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if(!reader->parse(json.data(), json.data() + json.size(), &root, &errors) || !root.isObject()) {
        throw std::runtime_error("config JSON parse failed: " + errors);
    }

    AppConfig config;
    config.sender_id = required_string(root, "sender_id");
    if(config.sender_id == "auto") {
        config.sender_id = derive_auto_sender_id();
    }
    config.sender_version = optional_string(root, "sender_version", config.sender_version);
    config.heartbeat_interval_ms = optional_int(root, "heartbeat_interval_ms", config.heartbeat_interval_ms);
    config.swap_depth_between_cameras = optional_bool(root, "swap_depth_between_cameras", config.swap_depth_between_cameras);

    const auto &receiver = root["receiver"];
    config.receiver.ip = required_string(receiver, "ip");
    config.receiver.media_port = parse_port(receiver, "media_port", config.receiver.media_port);
    config.receiver.status_port = parse_port(receiver, "status_port", config.receiver.status_port);

    config.clock_sync.receiver_ip = config.receiver.ip;
    const auto &clock_sync = root["clock_sync"];
    if(!clock_sync.isNull()) {
        if(!clock_sync.isObject()) {
            throw std::runtime_error("invalid object field: clock_sync");
        }
        config.clock_sync.enabled = optional_bool(clock_sync, "enabled", config.clock_sync.enabled);
        config.clock_sync.receiver_ip = optional_string(clock_sync, "receiver_ip", config.clock_sync.receiver_ip);
        config.clock_sync.port = parse_port(clock_sync, "port", config.clock_sync.port);
        config.clock_sync.interval_ms = optional_int(clock_sync, "interval_ms", config.clock_sync.interval_ms);
        config.clock_sync.timeout_ms = optional_int(clock_sync, "timeout_ms", config.clock_sync.timeout_ms);
        config.clock_sync.max_delay_us = optional_int(clock_sync, "max_delay_us", static_cast<int>(config.clock_sync.max_delay_us));
        const int sample_window = optional_int(clock_sync, "sample_window", static_cast<int>(config.clock_sync.sample_window));
        if(sample_window <= 0) {
            throw std::runtime_error("clock_sync.sample_window must be positive");
        }
        config.clock_sync.sample_window = static_cast<size_t>(sample_window);
    }

    const auto &transport = root["transport"];
    if(!transport.isNull()) {
        config.transport.enabled = optional_bool(transport, "enabled", config.transport.enabled);
        config.transport.status_protocol = optional_string(transport, "status_protocol", config.transport.status_protocol);
        config.transport.media_protocol = optional_string(transport, "media_protocol", config.transport.media_protocol);
        config.transport.connect_timeout_ms = optional_int(transport, "connect_timeout_ms", config.transport.connect_timeout_ms);
        config.transport.send_timeout_ms = optional_int(transport, "send_timeout_ms", config.transport.send_timeout_ms);
        config.transport.send_buffer_bytes = optional_int(transport, "send_buffer_bytes", config.transport.send_buffer_bytes);
        config.transport.reconnect_interval_ms = optional_int(transport, "reconnect_interval_ms", config.transport.reconnect_interval_ms);
    }

    const auto &media_udp = root["media_udp"];
    if(!media_udp.isNull()) {
        if(!media_udp.isObject()) {
            throw std::runtime_error("invalid object field: media_udp");
        }
        config.media_udp.enabled = optional_bool(media_udp, "enabled", config.media_udp.enabled);
        config.media_udp.rgb_enabled = optional_bool(media_udp, "rgb_enabled", config.media_udp.rgb_enabled);
        config.media_udp.depth_enabled = optional_bool(media_udp, "depth_enabled", config.media_udp.depth_enabled);
        config.media_udp.port = parse_port(media_udp, "port", config.media_udp.port);
        config.media_udp.mtu_bytes = optional_int(media_udp, "mtu_bytes", config.media_udp.mtu_bytes);
    }

    const auto &preview = root["preview"];
    if(!preview.isNull()) {
        config.preview.enabled = optional_bool(preview, "enabled", config.preview.enabled);
        config.preview.fps = optional_int(preview, "fps", config.preview.fps);
        config.preview.aligned_rgb = optional_bool(preview, "aligned_rgb", config.preview.aligned_rgb);
    }

    const auto &web_rgb_preview = root["web_rgb_preview"];
    if(!web_rgb_preview.isNull()) {
        if(!web_rgb_preview.isObject()) {
            throw std::runtime_error("invalid object field: web_rgb_preview");
        }
        config.web_rgb_preview.enabled = optional_bool(web_rgb_preview, "enabled", config.web_rgb_preview.enabled);
        config.web_rgb_preview.on_demand = optional_bool(web_rgb_preview, "on_demand", config.web_rgb_preview.on_demand);
        config.web_rgb_preview.max_width = optional_int(web_rgb_preview, "max_width", config.web_rgb_preview.max_width);
        config.web_rgb_preview.max_height = optional_int(web_rgb_preview, "max_height", config.web_rgb_preview.max_height);
        config.web_rgb_preview.fps = optional_int(web_rgb_preview, "fps", config.web_rgb_preview.fps);
        config.web_rgb_preview.bitrate_bps = optional_int(web_rgb_preview, "bitrate_bps", config.web_rgb_preview.bitrate_bps);
        config.web_rgb_preview.udp_enabled = optional_bool(web_rgb_preview, "udp_enabled", config.web_rgb_preview.udp_enabled);
        config.web_rgb_preview.udp_port = parse_port(web_rgb_preview, "udp_port", config.web_rgb_preview.udp_port);
        config.web_rgb_preview.udp_mtu_bytes = optional_int(web_rgb_preview, "udp_mtu_bytes", config.web_rgb_preview.udp_mtu_bytes);
    }

    const auto &logging = root["logging"];
    if(!logging.isNull()) {
        config.logging.directory = optional_string(logging, "directory", config.logging.directory);
        config.logging.max_bytes = static_cast<size_t>(optional_int(logging, "max_bytes", static_cast<int>(config.logging.max_bytes)));
    }

    const auto &hotplug = root["hotplug"];
    if(!hotplug.isNull()) {
        if(!hotplug.isObject()) {
            throw std::runtime_error("invalid object field: hotplug");
        }
        config.hotplug.enabled = optional_bool(hotplug, "enabled", config.hotplug.enabled);
    }

    const auto &recording_buffer = root["recording_buffer"];
    if(!recording_buffer.isNull()) {
        if(!recording_buffer.isObject()) {
            throw std::runtime_error("invalid object field: recording_buffer");
        }
        config.recording_buffer.enabled = optional_bool(recording_buffer, "enabled", config.recording_buffer.enabled);
        config.recording_buffer.rgb_frames_per_slot =
            optional_int(recording_buffer, "rgb_frames_per_slot", config.recording_buffer.rgb_frames_per_slot);
        config.recording_buffer.depth_frames_per_slot =
            optional_int(recording_buffer, "depth_frames_per_slot", config.recording_buffer.depth_frames_per_slot);
        config.recording_buffer.depth_compression_frames_per_slot =
            optional_int(recording_buffer, "depth_compression_frames_per_slot", config.recording_buffer.depth_compression_frames_per_slot);
    }

    const auto &cameras = root["cameras"];
    if(!cameras.isArray()) {
        throw std::runtime_error("missing or invalid cameras array");
    }
    for(const auto &item : cameras) {
        CameraConfig camera;
        camera.camera_id = required_string(item, "camera_id");
        if(item.isMember("depth_camera_id")) {
            throw std::runtime_error("depth_camera_id is not supported; RGB and Depth are always sent with the same camera_id");
        }
        camera.capture_backend = optional_string(item, "capture_backend", camera.capture_backend);
        camera.device_model = trim_copy(optional_string(item, "device_model", ""));
        camera.serial_number = optional_string(item, "serial_number", "");
        camera.uid = optional_string(item, "uid", "");
        camera.video_device = optional_string(item, "video_device", "");
        camera.device_index = optional_int(item, "device_index", camera.device_index);
        camera.validate_rgb_mjpeg = optional_bool(item, "validate_rgb_mjpeg", camera.validate_rgb_mjpeg);
        camera.publish_warmup_ms = optional_int(item, "publish_warmup_ms", camera.publish_warmup_ms);
        camera.frame_aggregate_mode = optional_string(item, "frame_aggregate_mode", camera.frame_aggregate_mode);
        if(item.isMember("rotation_degrees")) {
            camera.rotation_degrees = optional_int(item, "rotation_degrees", 0);
        }
        camera.rgb_profile = load_profile(item["rgb_profile"]);
        camera.depth_profile = load_profile(item["depth_profile"]);

        const auto &encoding = item["rgb_encoding"];
        if(!encoding.isNull()) {
            camera.rgb_encoding.codec = optional_string(encoding, "codec", camera.rgb_encoding.codec);
            camera.rgb_encoding.mode = optional_string(encoding, "mode", camera.rgb_encoding.mode);
            camera.rgb_encoding.gstreamer_encoder = optional_string(encoding, "gstreamer_encoder", camera.rgb_encoding.gstreamer_encoder);
            camera.rgb_encoding.bitrate_bps = optional_int(encoding, "bitrate_bps", camera.rgb_encoding.bitrate_bps);
        }

        const auto &depth = item["depth_transport"];
        if(!depth.isNull()) {
            camera.depth_transport.compression = optional_string(depth, "compression", camera.depth_transport.compression);
            camera.depth_transport.quantization_step_mm =
                optional_double(depth, "quantization_step_mm", camera.depth_transport.quantization_step_mm);
        }

        camera.color_controls = load_color_controls(item["color_controls"]);
        camera.adaptive_exposure = load_adaptive_exposure(item["adaptive_exposure"]);
        config.cameras.push_back(camera);
    }

    validate_config(config);
    return config;
}

void validate_config(const AppConfig &config) {
    if(!is_valid_protocol_id(config.sender_id)) {
        throw std::runtime_error("sender_id must be 1-64 ASCII letters/digits/_/-");
    }
    in_addr receiver_addr{};
    if(inet_pton(AF_INET, config.receiver.ip.c_str(), &receiver_addr) != 1) {
        throw std::runtime_error("receiver.ip must be a valid IPv4 address");
    }
    if(config.heartbeat_interval_ms <= 0 || config.heartbeat_interval_ms > 60000) {
        throw std::runtime_error("heartbeat_interval_ms must be in range [1, 60000]");
    }
    if(config.clock_sync.enabled) {
        in_addr clock_receiver_addr{};
        if(inet_pton(AF_INET, config.clock_sync.receiver_ip.c_str(), &clock_receiver_addr) != 1) {
            throw std::runtime_error("clock_sync.receiver_ip must be a valid IPv4 address");
        }
        if(config.clock_sync.interval_ms <= 0 || config.clock_sync.interval_ms > 60000) {
            throw std::runtime_error("clock_sync.interval_ms must be in range [1, 60000]");
        }
        if(config.clock_sync.timeout_ms <= 0 || config.clock_sync.timeout_ms > 60000) {
            throw std::runtime_error("clock_sync.timeout_ms must be in range [1, 60000]");
        }
        if(config.clock_sync.max_delay_us <= 0 || config.clock_sync.max_delay_us > 10'000'000) {
            throw std::runtime_error("clock_sync.max_delay_us must be in range [1, 10000000]");
        }
        if(config.clock_sync.sample_window == 0 || config.clock_sync.sample_window > 1000) {
            throw std::runtime_error("clock_sync.sample_window must be in range [1, 1000]");
        }
    }
    if(config.cameras.empty() || config.cameras.size() > 4) {
        throw std::runtime_error("camera count must be in range [1, 4]");
    }
    if(config.transport.status_protocol != "udp") {
        throw std::runtime_error("only udp status_protocol is implemented in this sender build");
    }
    if(config.transport.media_protocol != "tcp" && config.transport.media_protocol != "udp") {
        throw std::runtime_error("transport.media_protocol must be tcp or udp");
    }
    if(config.transport.connect_timeout_ms <= 0 || config.transport.connect_timeout_ms > 60000) {
        throw std::runtime_error("transport.connect_timeout_ms must be in range [1, 60000]");
    }
    if(config.transport.send_timeout_ms <= 0 || config.transport.send_timeout_ms > 60000) {
        throw std::runtime_error("transport.send_timeout_ms must be in range [1, 60000]");
    }
    if(config.transport.send_buffer_bytes < 0 || config.transport.send_buffer_bytes > 256 * 1024 * 1024) {
        throw std::runtime_error("transport.send_buffer_bytes must be in range [0, 268435456]");
    }
    if(config.transport.reconnect_interval_ms <= 0 || config.transport.reconnect_interval_ms > 60000) {
        throw std::runtime_error("transport.reconnect_interval_ms must be in range [1, 60000]");
    }
    if(config.media_udp.mtu_bytes <= static_cast<int>(kPreviewUdpHeaderSize) || config.media_udp.mtu_bytes > 65000) {
        throw std::runtime_error("media_udp.mtu_bytes must be > 32 and <= 65000");
    }
    if(config.swap_depth_between_cameras && config.cameras.size() != 2) {
        throw std::runtime_error("swap_depth_between_cameras requires exactly two cameras");
    }
    if(config.web_rgb_preview.max_width <= 0 || config.web_rgb_preview.max_width > 16384) {
        throw std::runtime_error("web_rgb_preview.max_width must be in range [1, 16384]");
    }
    if(config.web_rgb_preview.max_height <= 0 || config.web_rgb_preview.max_height > 16384) {
        throw std::runtime_error("web_rgb_preview.max_height must be in range [1, 16384]");
    }
    if(config.web_rgb_preview.fps <= 0 || config.web_rgb_preview.fps > 120) {
        throw std::runtime_error("web_rgb_preview.fps must be in range [1, 120]");
    }
    if(config.web_rgb_preview.bitrate_bps <= 0 || config.web_rgb_preview.bitrate_bps > 200'000'000) {
        throw std::runtime_error("web_rgb_preview.bitrate_bps must be in range [1, 200000000]");
    }
    if(config.web_rgb_preview.udp_mtu_bytes <= static_cast<int>(kPreviewUdpHeaderSize) || config.web_rgb_preview.udp_mtu_bytes > 65000) {
        throw std::runtime_error("web_rgb_preview.udp_mtu_bytes must be > 32 and <= 65000");
    }
    if(config.clock_sync.enabled && config.web_rgb_preview.udp_enabled && config.clock_sync.port == config.web_rgb_preview.udp_port) {
        throw std::runtime_error("clock_sync.port conflicts with enabled web_rgb_preview.udp_port");
    }
    if(config.recording_buffer.rgb_frames_per_slot <= 0 || config.recording_buffer.rgb_frames_per_slot > 1800) {
        throw std::runtime_error("recording_buffer.rgb_frames_per_slot must be in range [1, 1800]");
    }
    if(config.recording_buffer.depth_frames_per_slot <= 0 || config.recording_buffer.depth_frames_per_slot > 1800) {
        throw std::runtime_error("recording_buffer.depth_frames_per_slot must be in range [1, 1800]");
    }
    if(config.recording_buffer.depth_compression_frames_per_slot <= 0
       || config.recording_buffer.depth_compression_frames_per_slot > 120) {
        throw std::runtime_error("recording_buffer.depth_compression_frames_per_slot must be in range [1, 120]");
    }
    if(config.preview.fps <= 0 || config.preview.fps > 120) {
        throw std::runtime_error("preview.fps must be in range [1, 120]");
    }
    if(config.logging.directory.empty() || config.logging.max_bytes > 1024ull * 1024ull * 1024ull) {
        throw std::runtime_error("logging.directory must be non-empty and logging.max_bytes must be <= 1GiB");
    }
    std::set<std::string> camera_ids;
    std::set<std::string> serial_numbers;
    std::set<std::string> uids;
    auto validate_profile = [](const VideoProfileConfig &profile, const std::string &name) {
        if(profile.width < 0 || profile.height < 0 || profile.fps < 0
           || profile.width > 16384 || profile.height > 16384 || profile.fps > 240) {
            throw std::runtime_error(name + " width/height/fps are outside supported limits");
        }
    };
    for(const auto &camera : config.cameras) {
        auto validate_nonnegative = [](const std::optional<int> &value, const char *name) {
            if(value && *value < 0) {
                throw std::runtime_error(std::string(name) + " must be non-negative");
            }
        };
        auto validate_range = [](const std::optional<int> &value, const char *name, int min_value, int max_value) {
            if(value && (*value < min_value || *value > max_value)) {
                throw std::runtime_error(std::string(name) + " must be in range [" + std::to_string(min_value) + ", "
                                         + std::to_string(max_value) + "]");
            }
        };
        if(!is_valid_protocol_id(camera.camera_id)) {
            throw std::runtime_error("camera_id must be 1-64 ASCII letters/digits/_/-");
        }
        if(!camera_ids.insert(camera.camera_id).second) {
            throw std::runtime_error("duplicate camera_id is not allowed: " + camera.camera_id);
        }
        if(config.cameras.size() > 1 && camera.serial_number.empty() && camera.uid.empty()) {
            throw std::runtime_error("multi-camera configs require serial_number or uid for every camera: " + camera.camera_id);
        }
        if(camera.device_model.size() > 128
           || std::any_of(camera.device_model.begin(), camera.device_model.end(), [](unsigned char ch) { return std::iscntrl(ch); })) {
            throw std::runtime_error("camera.device_model must be at most 128 characters without control characters");
        }
        if(camera.frame_aggregate_mode != "disable" && camera.frame_aggregate_mode != "any_situation"
           && camera.frame_aggregate_mode != "full_frame_require" && camera.frame_aggregate_mode != "color_frame_require") {
            throw std::runtime_error("frame_aggregate_mode must be disable, any_situation, full_frame_require, or color_frame_require");
        }
        if(!camera.serial_number.empty() && !serial_numbers.insert(camera.serial_number).second) {
            throw std::runtime_error("duplicate camera serial_number is not allowed: " + camera.serial_number);
        }
        if(!camera.uid.empty() && !uids.insert(camera.uid).second) {
            throw std::runtime_error("duplicate camera uid is not allowed: " + camera.uid);
        }
        if(camera.rgb_encoding.codec != "h264") {
            throw std::runtime_error("only h264 rgb_encoding.codec is implemented in this sender build");
        }
        if(camera.rgb_encoding.mode != "hardware") {
            throw std::runtime_error("only hardware rgb_encoding.mode is implemented in this sender build");
        }
        if(camera.rgb_encoding.gstreamer_encoder.empty()) {
            throw std::runtime_error("rgb_encoding.gstreamer_encoder must not be empty");
        }
        if(camera.rgb_encoding.bitrate_bps <= 0 || camera.rgb_encoding.bitrate_bps > 200'000'000) {
            throw std::runtime_error("rgb_encoding.bitrate_bps must be in range [1, 200000000]");
        }
        if(camera.depth_transport.compression != "none" && camera.depth_transport.compression != "zlib"
           && camera.depth_transport.compression != "qdelta" && camera.depth_transport.compression != "pq12zlib"
           && camera.depth_transport.compression != "q8lz4" && camera.depth_transport.compression != "pq8zlib"
           && camera.depth_transport.compression != "pq8lz4") {
            throw std::runtime_error(
                "only none/zlib/qdelta/pq12zlib/q8lz4/pq8zlib/pq8lz4 depth compression is implemented in this sender build");
        }
        if(!std::isfinite(camera.depth_transport.quantization_step_mm)
           || camera.depth_transport.quantization_step_mm <= 0.0 || camera.depth_transport.quantization_step_mm > 1000.0) {
            throw std::runtime_error("depth_transport.quantization_step_mm must be in range (0, 1000]");
        }
        if(camera.capture_backend != "orbbec_sdk" && camera.capture_backend != "v4l2") {
            throw std::runtime_error("camera.capture_backend must be orbbec_sdk or v4l2");
        }
        if(camera.capture_backend == "v4l2" && camera.video_device.empty() && camera.serial_number.empty()) {
            throw std::runtime_error("v4l2 camera requires video_device or serial_number");
        }
        if(camera.rotation_degrees && *camera.rotation_degrees != 0 && *camera.rotation_degrees != 90
           && *camera.rotation_degrees != 180 && *camera.rotation_degrees != 270) {
            throw std::runtime_error("camera.rotation_degrees must be one of 0, 90, 180, 270");
        }
        if(camera.capture_backend != "orbbec_sdk" && camera.rotation_degrees) {
            throw std::runtime_error("camera.rotation_degrees is only supported by the orbbec_sdk capture backend");
        }
        if(camera.publish_warmup_ms < -1 || camera.publish_warmup_ms > 30000) {
            throw std::runtime_error("camera.publish_warmup_ms must be -1 or between 0 and 30000");
        }
        validate_profile(camera.rgb_profile, "rgb_profile");
        if(camera.depth_profile.enabled) {
            validate_profile(camera.depth_profile, "depth_profile");
        }
        validate_nonnegative(camera.color_controls.exposure, "color_controls.exposure");
        validate_nonnegative(camera.color_controls.gain, "color_controls.gain");
        validate_nonnegative(camera.color_controls.auto_exposure_priority, "color_controls.auto_exposure_priority");
        validate_nonnegative(camera.color_controls.max_exposure, "color_controls.max_exposure");
        validate_nonnegative(camera.color_controls.max_gain, "color_controls.max_gain");
        validate_nonnegative(camera.color_controls.power_line_frequency, "color_controls.power_line_frequency");
        validate_nonnegative(camera.color_controls.white_balance, "color_controls.white_balance");
        validate_range(camera.color_controls.brightness, "color_controls.brightness", -64, 64);
        validate_nonnegative(camera.color_controls.contrast, "color_controls.contrast");
        validate_nonnegative(camera.color_controls.saturation, "color_controls.saturation");
        validate_nonnegative(camera.color_controls.gamma, "color_controls.gamma");
        validate_nonnegative(camera.color_controls.backlight_compensation, "color_controls.backlight_compensation");

        const auto &adaptive = camera.adaptive_exposure;
        if(adaptive.control_mode != "proportional" && adaptive.control_mode != "pid") {
            throw std::runtime_error("adaptive_exposure.control_mode must be proportional or pid");
        }
        if(adaptive.interval_ms < 33 || adaptive.interval_ms > 5000) {
            throw std::runtime_error("adaptive_exposure.interval_ms must be in range [33, 5000]");
        }
        if(adaptive.stable_interval_ms < adaptive.interval_ms || adaptive.stable_interval_ms > 5000) {
            throw std::runtime_error("adaptive_exposure.stable_interval_ms must be in range [interval_ms, 5000]");
        }
        if(adaptive.settle_ms < adaptive.interval_ms || adaptive.settle_ms > 5000) {
            throw std::runtime_error("adaptive_exposure.settle_ms must be in range [interval_ms, 5000]");
        }
        if(adaptive.discard_frames_after_adjustment < 0
           || adaptive.discard_frames_after_adjustment > 30) {
            throw std::runtime_error(
                "adaptive_exposure.discard_frames_after_adjustment must be in range [0, 30]");
        }
        if(adaptive.direction_reversal_samples < 1 || adaptive.direction_reversal_samples > 10) {
            throw std::runtime_error(
                "adaptive_exposure.direction_reversal_samples must be in range [1, 10]");
        }
        if(adaptive.max_exposure_step < 2 || adaptive.max_exposure_step > 1000) {
            throw std::runtime_error("adaptive_exposure.max_exposure_step must be in range [2, 1000]");
        }
        if(adaptive.max_recovery_exposure_step < 1
           || adaptive.max_recovery_exposure_step > adaptive.max_exposure_step) {
            throw std::runtime_error(
                "adaptive_exposure.max_recovery_exposure_step must be in range [1, max_exposure_step]");
        }
        if(adaptive.max_gain_step < 1 || adaptive.max_gain_step > 255) {
            throw std::runtime_error("adaptive_exposure.max_gain_step must be in range [1, 255]");
        }
        if(adaptive.exposure_min < 1 || adaptive.exposure_max < adaptive.exposure_min || adaptive.exposure_max > 10000) {
            throw std::runtime_error("adaptive_exposure exposure range must satisfy 1 <= min <= max <= 10000");
        }
        if(adaptive.soft_highlight_exposure_floor != -1
           && (adaptive.soft_highlight_exposure_floor < adaptive.exposure_min
               || adaptive.soft_highlight_exposure_floor > adaptive.exposure_max)) {
            throw std::runtime_error(
                "adaptive_exposure.soft_highlight_exposure_floor must be -1 or within the exposure range");
        }
        if(adaptive.gain_min < 0 || adaptive.gain_max < adaptive.gain_min || adaptive.gain_max > 255) {
            throw std::runtime_error("adaptive_exposure gain range must satisfy 0 <= min <= max <= 255");
        }
        if(adaptive.target_p95_luma < 1 || adaptive.target_p95_luma > 254
           || adaptive.luma_deadband < 1 || adaptive.luma_deadband > 64
           || (adaptive.soft_highlight_luma != -1
               && (adaptive.soft_highlight_luma < 1
                   || adaptive.soft_highlight_luma >= adaptive.highlight_luma))
           || adaptive.highlight_luma < 1 || adaptive.highlight_luma > 255
           || (adaptive.target_p50_luma != -1
               && (adaptive.target_p50_luma < 1 || adaptive.target_p50_luma > 254))
           || adaptive.target_p95_luma + adaptive.luma_deadband >= adaptive.highlight_luma
           || (adaptive.target_p50_luma != -1
               && adaptive.target_p50_luma + adaptive.luma_deadband >= adaptive.target_p95_luma)) {
            throw std::runtime_error("adaptive_exposure luma thresholds are invalid");
        }
        if(!std::isfinite(adaptive.max_highlight_fraction) || adaptive.max_highlight_fraction < 0.0
           || adaptive.max_highlight_fraction > 1.0) {
            throw std::runtime_error("adaptive_exposure.max_highlight_fraction must be in range [0, 1]");
        }
        if(!std::isfinite(adaptive.highlight_recovery_ratio) || adaptive.highlight_recovery_ratio < 0.0
           || adaptive.highlight_recovery_ratio >= 1.0) {
            throw std::runtime_error("adaptive_exposure.highlight_recovery_ratio must be in range [0, 1)");
        }
        if(adaptive.highlight_release_samples < 1 || adaptive.highlight_release_samples > 100) {
            throw std::runtime_error(
                "adaptive_exposure.highlight_release_samples must be in range [1, 100]");
        }
        if(adaptive.underexposed_samples < 1 || adaptive.underexposed_samples > 20) {
            throw std::runtime_error("adaptive_exposure.underexposed_samples must be in range [1, 20]");
        }
        if(adaptive.roi_margin_percent < 0 || adaptive.roi_margin_percent > 40) {
            throw std::runtime_error("adaptive_exposure.roi_margin_percent must be in range [0, 40]");
        }
        if(adaptive.metering_window < 1 || adaptive.metering_window > 9 || adaptive.metering_window % 2 == 0) {
            throw std::runtime_error("adaptive_exposure.metering_window must be an odd value in range [1, 9]");
        }
        if(!std::isfinite(adaptive.pid_kp) || adaptive.pid_kp < 0.0 || adaptive.pid_kp > 2.0
           || !std::isfinite(adaptive.pid_ki) || adaptive.pid_ki < 0.0 || adaptive.pid_ki > 2.0
           || !std::isfinite(adaptive.pid_kd) || adaptive.pid_kd < 0.0 || adaptive.pid_kd > 1.0
           || !std::isfinite(adaptive.pid_integral_limit) || adaptive.pid_integral_limit < 0.0
           || adaptive.pid_integral_limit > 2.0
           || !std::isfinite(adaptive.pid_derivative_alpha) || adaptive.pid_derivative_alpha <= 0.0
           || adaptive.pid_derivative_alpha > 1.0) {
            throw std::runtime_error("adaptive_exposure PID parameters are invalid");
        }
        if(adaptive.enabled) {
            if(camera.capture_backend != "orbbec_sdk") {
                throw std::runtime_error("adaptive_exposure requires camera.capture_backend=orbbec_sdk");
            }
            if(camera.rgb_profile.format != "mjpg") {
                throw std::runtime_error("adaptive_exposure requires camera.rgb_profile.format=mjpg");
            }
            const auto safe_exposure_max = adaptive_exposure_max_for_model(camera.device_model);
            if(!safe_exposure_max) {
                throw std::runtime_error("adaptive_exposure is unsupported for camera.device_model=" + camera.device_model);
            }
            if(adaptive.exposure_max > *safe_exposure_max) {
                throw std::runtime_error("adaptive_exposure.exposure_max exceeds validated 30fps limit "
                                         + std::to_string(*safe_exposure_max) + " for camera.device_model="
                                         + camera.device_model);
            }
            if(!camera.color_controls.auto_exposure || *camera.color_controls.auto_exposure) {
                throw std::runtime_error("adaptive_exposure requires color_controls.auto_exposure=false");
            }
            if(!camera.color_controls.exposure || !camera.color_controls.gain) {
                throw std::runtime_error("adaptive_exposure requires initial color_controls exposure and gain");
            }
            if(*camera.color_controls.exposure < adaptive.exposure_min
               || *camera.color_controls.exposure > adaptive.exposure_max
               || *camera.color_controls.gain < adaptive.gain_min
               || *camera.color_controls.gain > adaptive.gain_max) {
                throw std::runtime_error("adaptive_exposure initial exposure/gain must be inside configured ranges");
            }
        }
    }
}

}  // namespace gwv3
