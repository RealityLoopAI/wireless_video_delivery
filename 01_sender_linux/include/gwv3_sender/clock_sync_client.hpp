#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "gwv3_sender/receiver_target.hpp"

namespace gwv3 {

struct ClockSyncClientConfig {
    bool enabled = true;
    std::string receiver_ip;
    uint16_t port = 50012;
    int interval_ms = 2000;
    int timeout_ms = 100;
    int64_t max_delay_us = 100000;
    size_t sample_window = 10;
};

struct ClockSyncClientState {
    bool valid = false;
    int64_t offset_us = 0;
    int64_t delay_us = 0;
    double drift_ppm = 0.0;
    uint64_t last_sync_us = 0;
    size_t sample_count = 0;
};

class ClockSyncClient {
public:
    ClockSyncClient(ClockSyncClientConfig config, std::string sender_id);
    ClockSyncClient(ClockSyncClientConfig config, std::string sender_id,
                    std::shared_ptr<ReceiverTarget> receiver_target);
    ~ClockSyncClient();

    ClockSyncClient(const ClockSyncClient &) = delete;
    ClockSyncClient &operator=(const ClockSyncClient &) = delete;

    void set_log_callbacks(std::function<void(const std::string &)> info,
                           std::function<void(const std::string &)> warn);

    bool start();
    void stop();

    bool healthy() const;
    int64_t offset_us() const;
    int64_t delay_us() const;
    double drift_ppm() const;
    ClockSyncClientState state() const;

private:
    struct Sample {
        uint64_t sender_receive_us = 0;
        int64_t offset_us = 0;
        int64_t delay_us = 0;
    };

    void run_loop();
    bool send_probe_and_wait_response(int fd, uint64_t sequence);
    bool apply_sample(uint64_t sender_receive_us, int64_t offset_us, int64_t delay_us);
    void log_info(const std::string &message) const;
    void log_warn(const std::string &message) const;

    ClockSyncClientConfig config_;
    std::string sender_id_;
    std::shared_ptr<ReceiverTarget> receiver_target_;
    uint64_t target_generation_ = 0;
    std::atomic<bool> running_{false};
    std::thread thread_;

    mutable std::mutex state_mutex_;
    ClockSyncClientState state_;
    std::vector<Sample> samples_;
    std::chrono::steady_clock::time_point last_sync_steady_{};

    mutable std::mutex log_mutex_;
    std::function<void(const std::string &)> info_log_;
    std::function<void(const std::string &)> warn_log_;
};

}  // namespace gwv3
