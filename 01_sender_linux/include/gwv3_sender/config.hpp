#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace gwv3 {

struct VideoProfileConfig {
    int width = 0;
    int height = 0;
    int fps = 0;
    std::string format;
};

struct RgbEncodingConfig {
    std::string codec = "h264";
    std::string mode = "hardware";
    std::string gstreamer_encoder = "mpph264enc";
    int bitrate_bps = 2000000;
};

struct DepthTransportConfig {
    std::string compression = "none";
};

struct ColorControlsConfig {
    std::optional<bool> auto_exposure;
    std::optional<int> exposure;
    std::optional<int> gain;
    std::optional<int> auto_exposure_priority;
    std::optional<int> max_exposure;
    std::optional<int> max_gain;
    std::optional<int> power_line_frequency;
};

struct CameraConfig {
    std::string camera_id;
    std::string serial_number;
    std::string uid;
    int device_index = 0;
    bool validate_rgb_mjpeg = false;
    VideoProfileConfig rgb_profile;
    VideoProfileConfig depth_profile;
    RgbEncodingConfig rgb_encoding;
    DepthTransportConfig depth_transport;
    ColorControlsConfig color_controls;
};

struct ReceiverConfig {
    std::string ip;
    uint16_t media_port = 50010;
    uint16_t status_port = 50011;
};

struct TransportConfig {
    bool enabled = true;
    std::string status_protocol = "udp";
    std::string media_protocol = "tcp";
    int connect_timeout_ms = 250;
    int send_timeout_ms = 80;
    int send_buffer_bytes = 1048576;
    int reconnect_interval_ms = 1000;
};

struct PreviewConfig {
    bool enabled = true;
    int fps = 10;
};

struct LoggingConfig {
    std::string directory = "08_reports/sender_logs";
    size_t max_bytes = 10485760;
};

struct HotplugConfig {
    bool enabled = true;
};

struct AppConfig {
    std::string sender_id;
    std::string sender_version = "3.0.0";
    ReceiverConfig receiver;
    TransportConfig transport;
    PreviewConfig preview;
    LoggingConfig logging;
    HotplugConfig hotplug;
    int heartbeat_interval_ms = 1000;
    bool swap_depth_between_cameras = false;
    std::vector<CameraConfig> cameras;
};

AppConfig load_config(const std::string &path);
void validate_config(const AppConfig &config);
bool is_valid_protocol_id(const std::string &value);

}  // namespace gwv3
