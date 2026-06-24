#include "gwv3_sender/config.hpp"

#include "gwv3_common/protocol.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <set>
#include <stdexcept>
#include <vector>

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

std::string default_route_interface() {
    std::ifstream input("/proc/net/route");
    std::string line;
    std::string iface;
    std::string destination;
    std::getline(input, line);
    while(std::getline(input, line)) {
        std::istringstream row(line);
        row >> iface >> destination;
        if(destination == "00000000") {
            return iface;
        }
    }
    return "";
}

std::string first_available_mac() {
    const auto route_iface = default_route_interface();
    if(!route_iface.empty()) {
        const auto mac = mac_for_interface(route_iface);
        if(!mac.empty()) {
            return mac;
        }
    }

    std::vector<std::string> names;
    const std::filesystem::path net_dir("/sys/class/net");
    if(std::filesystem::exists(net_dir)) {
        for(const auto &entry : std::filesystem::directory_iterator(net_dir)) {
            names.push_back(entry.path().filename().string());
        }
    }
    std::sort(names.begin(), names.end());

    for(const auto &name : names) {
        if(name == "lo") {
            continue;
        }
        const auto mac = mac_for_interface(name);
        if(!mac.empty()) {
            return mac;
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
    return controls;
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

AppConfig load_config(const std::string &path) {
    std::ifstream input(path);
    if(!input) {
        throw std::runtime_error("cannot open config file: " + path);
    }

    Json::CharReaderBuilder builder;
    Json::Value root;
    std::string errors;
    if(!Json::parseFromStream(builder, input, &root, &errors)) {
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
        camera.serial_number = optional_string(item, "serial_number", "");
        camera.uid = optional_string(item, "uid", "");
        camera.device_index = optional_int(item, "device_index", camera.device_index);
        camera.validate_rgb_mjpeg = optional_bool(item, "validate_rgb_mjpeg", camera.validate_rgb_mjpeg);
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
        config.cameras.push_back(camera);
    }

    validate_config(config);
    return config;
}

void validate_config(const AppConfig &config) {
    if(!is_valid_protocol_id(config.sender_id)) {
        throw std::runtime_error("sender_id must be 1-64 ASCII letters/digits/_/-");
    }
    if(config.receiver.ip.empty()) {
        throw std::runtime_error("receiver.ip is required");
    }
    if(config.heartbeat_interval_ms <= 0) {
        throw std::runtime_error("heartbeat_interval_ms must be positive");
    }
    if(config.cameras.empty()) {
        throw std::runtime_error("at least one camera is required");
    }
    if(config.transport.status_protocol != "udp") {
        throw std::runtime_error("only udp status_protocol is implemented in this sender build");
    }
    if(config.transport.media_protocol != "tcp" && config.transport.media_protocol != "udp") {
        throw std::runtime_error("transport.media_protocol must be tcp or udp");
    }
    if(config.transport.connect_timeout_ms <= 0) {
        throw std::runtime_error("transport.connect_timeout_ms must be positive");
    }
    if(config.transport.send_timeout_ms <= 0) {
        throw std::runtime_error("transport.send_timeout_ms must be positive");
    }
    if(config.transport.send_buffer_bytes < 0) {
        throw std::runtime_error("transport.send_buffer_bytes must be non-negative");
    }
    if(config.transport.reconnect_interval_ms <= 0) {
        throw std::runtime_error("transport.reconnect_interval_ms must be positive");
    }
    if(config.media_udp.mtu_bytes <= static_cast<int>(kPreviewUdpHeaderSize) || config.media_udp.mtu_bytes > 65000) {
        throw std::runtime_error("media_udp.mtu_bytes must be > 32 and <= 65000");
    }
    if(config.swap_depth_between_cameras && config.cameras.size() != 2) {
        throw std::runtime_error("swap_depth_between_cameras requires exactly two cameras");
    }
    if(config.web_rgb_preview.max_width <= 0) {
        throw std::runtime_error("web_rgb_preview.max_width must be positive");
    }
    if(config.web_rgb_preview.max_height <= 0) {
        throw std::runtime_error("web_rgb_preview.max_height must be positive");
    }
    if(config.web_rgb_preview.fps <= 0) {
        throw std::runtime_error("web_rgb_preview.fps must be positive");
    }
    if(config.web_rgb_preview.bitrate_bps <= 0) {
        throw std::runtime_error("web_rgb_preview.bitrate_bps must be positive");
    }
    if(config.web_rgb_preview.udp_mtu_bytes <= static_cast<int>(kPreviewUdpHeaderSize) || config.web_rgb_preview.udp_mtu_bytes > 65000) {
        throw std::runtime_error("web_rgb_preview.udp_mtu_bytes must be > 32 and <= 65000");
    }
    if(config.recording_buffer.rgb_frames_per_slot <= 0) {
        throw std::runtime_error("recording_buffer.rgb_frames_per_slot must be positive");
    }
    if(config.recording_buffer.depth_frames_per_slot <= 0) {
        throw std::runtime_error("recording_buffer.depth_frames_per_slot must be positive");
    }
    if(config.recording_buffer.depth_compression_frames_per_slot <= 0) {
        throw std::runtime_error("recording_buffer.depth_compression_frames_per_slot must be positive");
    }
    std::set<std::string> camera_ids;
    std::set<std::string> serial_numbers;
    std::set<std::string> uids;
    auto validate_profile = [](const VideoProfileConfig &profile, const std::string &name) {
        if(profile.width < 0 || profile.height < 0 || profile.fps < 0) {
            throw std::runtime_error(name + " width/height/fps must be non-negative");
        }
    };
    for(const auto &camera : config.cameras) {
        auto validate_nonnegative = [](const std::optional<int> &value, const char *name) {
            if(value && *value < 0) {
                throw std::runtime_error(std::string(name) + " must be non-negative");
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
        if(camera.rgb_encoding.bitrate_bps <= 0) {
            throw std::runtime_error("rgb_encoding.bitrate_bps must be positive");
        }
        if(camera.depth_transport.compression != "none" && camera.depth_transport.compression != "zlib"
           && camera.depth_transport.compression != "qdelta" && camera.depth_transport.compression != "pq12zlib"
           && camera.depth_transport.compression != "q8lz4" && camera.depth_transport.compression != "pq8zlib"
           && camera.depth_transport.compression != "pq8lz4") {
            throw std::runtime_error(
                "only none/zlib/qdelta/pq12zlib/q8lz4/pq8zlib/pq8lz4 depth compression is implemented in this sender build");
        }
        if(camera.depth_transport.quantization_step_mm <= 0.0) {
            throw std::runtime_error("depth_transport.quantization_step_mm must be positive");
        }
        validate_profile(camera.rgb_profile, "rgb_profile");
        validate_profile(camera.depth_profile, "depth_profile");
        validate_nonnegative(camera.color_controls.exposure, "color_controls.exposure");
        validate_nonnegative(camera.color_controls.gain, "color_controls.gain");
        validate_nonnegative(camera.color_controls.auto_exposure_priority, "color_controls.auto_exposure_priority");
        validate_nonnegative(camera.color_controls.max_exposure, "color_controls.max_exposure");
        validate_nonnegative(camera.color_controls.max_gain, "color_controls.max_gain");
        validate_nonnegative(camera.color_controls.power_line_frequency, "color_controls.power_line_frequency");
    }
}

}  // namespace gwv3
