#pragma once

#include "gwv3_sender/config.hpp"
#include "gwv3_sender/receiver_target.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace gwv3 {

class ReceiverDiscoveryClient {
public:
    ReceiverDiscoveryClient(ReceiverDiscoveryConfig config, std::string sender_id,
                            std::shared_ptr<ReceiverTarget> target);
    ~ReceiverDiscoveryClient();

    ReceiverDiscoveryClient(const ReceiverDiscoveryClient &) = delete;
    ReceiverDiscoveryClient &operator=(const ReceiverDiscoveryClient &) = delete;

    bool start();
    void stop();
    void set_log_callbacks(std::function<void(const std::string &)> info,
                           std::function<void(const std::string &)> warn);

    bool healthy() const;
    uint64_t last_response_us() const;

private:
    void run();
    void log_info(const std::string &message) const;
    void log_warn(const std::string &message) const;

    ReceiverDiscoveryConfig config_;
    std::string sender_id_;
    std::shared_ptr<ReceiverTarget> target_;
    std::atomic<bool> running_{false};
    std::atomic<bool> healthy_{false};
    std::atomic<uint64_t> last_response_us_{0};
    std::thread thread_;
    mutable std::mutex callback_mutex_;
    std::function<void(const std::string &)> info_callback_;
    std::function<void(const std::string &)> warn_callback_;
};

}  // namespace gwv3
