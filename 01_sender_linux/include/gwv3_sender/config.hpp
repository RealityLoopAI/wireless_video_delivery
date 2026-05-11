#pragma once

#include <cstdint>
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

struct CameraConfig {
    std::string camera_id;
    std::string serial_number;
    std::string uid;
    int device_index = 0;
    VideoProfileConfig rgb_profile;
    VideoProfileConfig depth_profile;
    RgbEncodingConfig rgb_encoding;
    DepthTransportConfig depth_transport;
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

struct AppConfig {
    std::string sender_id;
    std::string sender_version = "3.0.0";
    ReceiverConfig receiver;
    TransportConfig transport;
    PreviewConfig preview;
    LoggingConfig logging;
    int heartbeat_interval_ms = 1000;
    std::vector<CameraConfig> cameras;
};

AppConfig load_config(const std::string &path);
void validate_config(const AppConfig &config);
bool is_valid_protocol_id(const std::string &value);

}  // namespace gwv3
