#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "gwv3_sender/config.hpp"

namespace gwv3 {

class Transport {
public:
    explicit Transport(const AppConfig &config);
    ~Transport();

    bool send_status(const std::string &json_message);
    bool send_media(const std::vector<uint8_t> &packet);
    std::string last_error() const;

private:
    int make_udp_socket();
    int make_tcp_socket();
    bool send_udp_status(const std::string &json_message);
    bool ensure_media_tcp_connected();
    bool send_all(int fd, const uint8_t *data, size_t size);
    void close_media_socket();
    bool can_retry_media_connect() const;
    void set_error(const std::string &message);

    AppConfig config_;
    int status_udp_fd_ = -1;
    int media_tcp_fd_ = -1;
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
