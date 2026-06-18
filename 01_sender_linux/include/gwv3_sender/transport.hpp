#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "gwv3_sender/config.hpp"

namespace gwv3 {

class Transport {
public:
    enum class MediaPriority {
        realtime,
        bulk,
    };

    explicit Transport(const AppConfig &config);
    ~Transport();

    bool send_status(const std::string &json_message);
    bool send_media(const std::vector<uint8_t> &packet, MediaPriority priority = MediaPriority::realtime);
    std::string last_error() const;

private:
    enum class SendResult {
        sent,
        dropped_backpressure,
        failed,
    };

    int make_udp_socket();
    int make_tcp_socket();
    bool send_udp_status(const std::string &json_message);
    bool ensure_media_tcp_connected();
    SendResult send_all(int fd, const uint8_t *data, size_t size);
    void close_media_socket();
    bool can_retry_media_connect() const;
    void set_error(const std::string &message);
    bool should_drop_before_write(int fd, size_t packet_size, MediaPriority priority);
    size_t media_send_capacity_bytes() const;

    AppConfig config_;
    int status_udp_fd_ = -1;
    int media_tcp_fd_ = -1;
    int media_send_buffer_bytes_ = 0;
    uint32_t consecutive_media_backpressure_drops_ = 0;
    std::chrono::steady_clock::time_point last_media_connect_attempt_{};
    std::string last_error_;
};

class NullTransport {
public:
    bool send_status(const std::string &) { return true; }
    bool send_media(const std::vector<uint8_t> &) { return true; }
    std::string last_error() const { return {}; }
};

}  // namespace gwv3
