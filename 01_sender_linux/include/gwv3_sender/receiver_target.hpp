#pragma once

#include <cstdint>
#include <mutex>
#include <string>

namespace gwv3 {

struct ReceiverTargetSnapshot {
    std::string host;
    std::string receiver_id;
    uint64_t generation = 0;
    bool discovered = false;
};

class ReceiverTarget {
public:
    explicit ReceiverTarget(std::string fallback_host);

    ReceiverTargetSnapshot snapshot() const;
    bool update_discovered(const std::string &host, const std::string &receiver_id);
    bool restore_persisted(const std::string &host, const std::string &receiver_id);
    bool use_fallback();
    void mark_success(uint64_t generation);
    bool success_recent(int timeout_ms) const;
    const std::string &fallback_host() const;

private:
    static uint64_t steady_now_ms();
    bool update_locked(const std::string &host, const std::string &receiver_id, bool discovered);

    const std::string fallback_host_;
    mutable std::mutex mutex_;
    ReceiverTargetSnapshot current_;
    uint64_t last_success_ms_ = 0;
};

}  // namespace gwv3
