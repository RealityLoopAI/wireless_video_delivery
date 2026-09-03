#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace gwv3 {

struct VideoProfileConfig {
    bool enabled = true;
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

struct RgbRtpOutputConfig {
    bool enabled = false;
    std::string host;
    uint16_t port = 5600;
    int payload_type = 96;
    int mtu_bytes = 1200;
};

struct DepthTransportConfig {
    std::string compression = "none";
    double quantization_step_mm = 10.0;
};

struct ColorControlsConfig {
    std::optional<bool> auto_exposure;
    std::optional<int> exposure;
    std::optional<int> gain;
    std::optional<int> auto_exposure_priority;
    std::optional<int> max_exposure;
    std::optional<int> max_gain;
    std::optional<int> power_line_frequency;
    std::optional<bool> auto_white_balance;
    std::optional<int> white_balance;
    std::optional<int> brightness;
    std::optional<int> contrast;
    std::optional<int> saturation;
    std::optional<int> gamma;
    std::optional<int> backlight_compensation;
};

struct AdaptiveExposureConfig {
    bool enabled = false;
    std::string control_mode = "proportional";
    int interval_ms = 500;
    int stable_interval_ms = 500;
    int settle_ms = 500;
    int discard_frames_after_adjustment = 0;
    int direction_reversal_samples = 1;
    int max_exposure_step = 16;
    int max_recovery_exposure_step = 16;
    int max_gain_step = 2;
    int exposure_min = 80;
    int exposure_max = 200;
    int soft_highlight_exposure_floor = -1;
    int gain_min = 16;
    int gain_max = 32;
    int target_p50_luma = -1;
    int target_p95_luma = 200;
    int luma_deadband = 8;
    int soft_highlight_luma = -1;
    int highlight_luma = 245;
    double max_highlight_fraction = 0.0025;
    double highlight_recovery_ratio = 0.7;
    int highlight_release_samples = 1;
    int underexposed_samples = 3;
    int roi_margin_percent = 10;
    int metering_window = 1;
    double pid_kp = 0.6;
    double pid_ki = 0.15;
    double pid_kd = 0.0;
    double pid_integral_limit = 0.35;
    double pid_derivative_alpha = 0.2;
};

struct CameraConfig {
    std::string camera_id;
    std::string capture_backend = "orbbec_sdk";
    std::string device_model;
    std::string serial_number;
    std::string uid;
    std::string video_device;
    int device_index = 0;
    bool validate_rgb_mjpeg = false;
    int publish_warmup_ms = -1;
    std::string frame_aggregate_mode = "disable";
    std::optional<int> rotation_degrees;
    VideoProfileConfig rgb_profile;
    VideoProfileConfig depth_profile;
    RgbEncodingConfig rgb_encoding;
    RgbRtpOutputConfig rgb_rtp_output;
    DepthTransportConfig depth_transport;
    ColorControlsConfig color_controls;
    AdaptiveExposureConfig adaptive_exposure;
};

struct ReceiverConfig {
    std::string ip;
    uint16_t media_port = 50010;
    uint16_t status_port = 50011;
};

struct ReceiverDiscoveryConfig {
    bool enabled = true;
    uint16_t port = 50009;
    int interval_ms = 1000;
    int response_window_ms = 250;
    int sticky_timeout_ms = 10000;
    std::string state_path;
};

struct ClockSyncConfig {
    bool enabled = true;
    std::string receiver_ip;
    uint16_t port = 50012;
    int interval_ms = 2000;
    int timeout_ms = 100;
    int64_t max_delay_us = 100000;
    size_t sample_window = 10;
};

struct TransportConfig {
    bool enabled = true;
    std::string status_protocol = "udp";
    std::string media_protocol = "tcp";
    int connect_timeout_ms = 1500;
    int send_timeout_ms = 80;
    int send_buffer_bytes = 1048576;
    int reconnect_interval_ms = 1000;
};

struct MediaUdpConfig {
    bool enabled = false;
    bool rgb_enabled = false;
    bool depth_enabled = false;
    uint16_t port = 50013;
    int mtu_bytes = 1200;
};

struct PreviewConfig {
    bool enabled = true;
    int fps = 10;
    bool aligned_rgb = true;
};

struct WebRgbPreviewConfig {
    bool enabled = true;
    bool on_demand = true;
    int max_width = 1920;
    int max_height = 1080;
    int fps = 30;
    int bitrate_bps = 1200000;
    bool udp_enabled = false;
    uint16_t udp_port = 50012;
    int udp_mtu_bytes = 1200;
};

struct LoggingConfig {
    std::string directory = "08_reports/sender_logs";
    size_t max_bytes = 10485760;
};

struct HotplugConfig {
    bool enabled = true;
};

struct RecordingBufferConfig {
    bool enabled = false;
    int rgb_frames_per_slot = 1;
    int depth_frames_per_slot = 4;
    int depth_compression_frames_per_slot = 4;
};

struct AppConfig {
    std::string sender_id;
    std::string sender_version = "3.0.0";
    ReceiverConfig receiver;
    ReceiverDiscoveryConfig receiver_discovery;
    ClockSyncConfig clock_sync;
    TransportConfig transport;
    MediaUdpConfig media_udp;
    PreviewConfig preview;
    WebRgbPreviewConfig web_rgb_preview;
    LoggingConfig logging;
    HotplugConfig hotplug;
    RecordingBufferConfig recording_buffer;
    int heartbeat_interval_ms = 1000;
    bool swap_depth_between_cameras = false;
    std::vector<CameraConfig> cameras;
};

AppConfig load_config(const std::string &path);
void validate_config(const AppConfig &config);
bool is_valid_protocol_id(const std::string &value);
std::optional<int> adaptive_exposure_max_for_model(const std::string &device_model);

}  // namespace gwv3
