#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <netinet/in.h>

namespace gwv3 {

struct ClockSyncManagerConfig {
    bool enabled = true;
    std::string bind_ip = "0.0.0.0";
    uint16_t port = 50012;
    int model_timeout_ms = 10000;
};

struct ClockSample {
    uint64_t local_receive_us = 0;
    uint64_t remote_sender_send_us = 0;
    int64_t offset_us = 0;
    int64_t delay_us = 0;
};

struct ClockModel {
    bool valid = false;
    bool report_stale = false;
    int64_t offset_us = 0;
    int64_t delay_us = 0;
    double drift_ppm = 0.0;
    uint64_t last_sync_us = 0;
    size_t sample_count = 0;
    uint64_t last_update_receiver_us = 0;
    uint64_t last_probe_receiver_us = 0;
    uint64_t probe_count = 0;
};

struct ClockTimestampMapping {
    ClockModel model;
    int64_t global_timestamp_us = 0;
    bool has_estimate = false;
};

// Caller owns synchronization. Published ranges are immutable, including when
// RGB and Depth reach the receiver at very different times.
class ClockSyncTimeline {
public:
    explicit ClockSyncTimeline(uint64_t extrapolation_limit_us = 10000000, size_t max_segments = 4096);
    void add_model(const ClockModel &model);
    ClockTimestampMapping map(uint64_t timestamp_us) const;
    size_t size() const { return segments_.size(); }

private:
    struct Segment {
        ClockModel model;
        uint64_t begin_us = 0;
        long double start_offset_us = 0;
        long double duration_us = 0;
        long double offset_at(uint64_t timestamp_us) const;
    };
    std::deque<Segment> segments_;
    uint64_t extrapolation_limit_us_;
    size_t max_segments_;
    uint64_t earliest_timestamp_us_ = 0;
    uint64_t last_report_us_ = 0;
    mutable uint64_t last_mapped_us_ = 0;
};

class ClockSyncManager {
public:
    explicit ClockSyncManager(ClockSyncManagerConfig config = {});
    ~ClockSyncManager();

    ClockSyncManager(const ClockSyncManager &) = delete;
    ClockSyncManager &operator=(const ClockSyncManager &) = delete;

    void set_log_callbacks(std::function<void(const std::string &)> info,
                           std::function<void(const std::string &)> warn);

    bool start();
    void stop();

    bool update_from_sender_report(const std::string &sender_id,
                                   int64_t offset_us,
                                   int64_t delay_us,
                                   double drift_ppm,
                                   uint64_t last_sync_us,
                                   const std::string &source_ip);

    ClockModel get_model(const std::string &sender_id) const;
    ClockTimestampMapping map_timestamp(const std::string &sender_id, uint64_t sender_timestamp_us) const;
    int64_t get_global_timestamp_us(const std::string &sender_id, uint64_t sender_timestamp_us) const;
    bool has_valid_model(const std::string &sender_id) const;
    std::vector<std::pair<std::string, ClockModel>> models() const;

private:
    void run_loop();
    void handle_datagram(int fd, const char *data, size_t size, const sockaddr_in &peer);
    ClockModel apply_timeout_locked(const ClockModel &model, uint64_t now) const;
    void log_info(const std::string &message) const;
    void log_warn(const std::string &message) const;

    ClockSyncManagerConfig config_;
    std::atomic<bool> running_{false};
    std::thread thread_;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, ClockModel> clock_models_;
    std::unordered_map<std::string, ClockSyncTimeline> clock_timelines_;
    std::unordered_map<std::string, uint32_t> probe_source_ips_;
    std::unordered_map<std::string, uint64_t> last_probe_log_us_;

    mutable std::mutex log_mutex_;
    std::function<void(const std::string &)> info_log_;
    std::function<void(const std::string &)> warn_log_;
};

}  // namespace gwv3
