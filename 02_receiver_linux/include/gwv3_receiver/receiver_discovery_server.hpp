#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace gwv3 {

struct ReceiverDiscoveryServerConfig {
    bool enabled = true;
    std::string bind_ip = "0.0.0.0";
    uint16_t port = 50009;
    std::string receiver_id = "auto";
    uint16_t media_port = 50010;
    uint16_t status_port = 50011;
    uint16_t clock_sync_port = 50012;
    uint16_t media_udp_port = 50013;
    uint16_t preview_udp_port = 50014;
};

class ReceiverDiscoveryServer {
public:
    explicit ReceiverDiscoveryServer(ReceiverDiscoveryServerConfig config);
    ~ReceiverDiscoveryServer();

    ReceiverDiscoveryServer(const ReceiverDiscoveryServer &) = delete;
    ReceiverDiscoveryServer &operator=(const ReceiverDiscoveryServer &) = delete;

    bool start();
    void stop();
    void set_log_callbacks(std::function<void(const std::string &)> info,
                           std::function<void(const std::string &)> warn);

    bool healthy() const;
    std::string receiver_id() const;
    uint64_t requests_received() const;
    uint64_t responses_sent() const;
    uint64_t last_request_us() const;

private:
    void run();
    void log_info(const std::string &message) const;
    void log_warn(const std::string &message) const;

    ReceiverDiscoveryServerConfig config_;
    std::string receiver_id_;
    std::atomic<bool> running_{false};
    std::atomic<bool> healthy_{false};
    std::atomic<uint64_t> requests_received_{0};
    std::atomic<uint64_t> responses_sent_{0};
    std::atomic<uint64_t> last_request_us_{0};
    std::thread thread_;
    mutable std::mutex callback_mutex_;
    std::function<void(const std::string &)> info_callback_;
    std::function<void(const std::string &)> warn_callback_;
};

}  // namespace gwv3
