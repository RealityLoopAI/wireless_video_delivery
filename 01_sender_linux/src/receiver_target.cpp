#include "gwv3_sender/receiver_target.hpp"

#include <chrono>
#include <utility>

namespace gwv3 {

ReceiverTarget::ReceiverTarget(std::string fallback_host) : fallback_host_(std::move(fallback_host)) {
    current_.host = fallback_host_;
    current_.generation = 1;
}

ReceiverTargetSnapshot ReceiverTarget::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_;
}

bool ReceiverTarget::update_discovered(const std::string &host, const std::string &receiver_id) {
    if(host.empty() || receiver_id.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return update_locked(host, receiver_id, true);
}

bool ReceiverTarget::restore_persisted(const std::string &host, const std::string &receiver_id) {
    if(host.empty() || receiver_id.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return update_locked(host, receiver_id, true);
}

bool ReceiverTarget::use_fallback() {
    std::lock_guard<std::mutex> lock(mutex_);
    return update_locked(fallback_host_, "", false);
}

void ReceiverTarget::mark_success(uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(generation == current_.generation) {
        last_success_ms_ = steady_now_ms();
    }
}

bool ReceiverTarget::success_recent(int timeout_ms) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if(last_success_ms_ == 0 || timeout_ms <= 0) {
        return false;
    }
    const auto now = steady_now_ms();
    return now >= last_success_ms_ && now - last_success_ms_ <= static_cast<uint64_t>(timeout_ms);
}

const std::string &ReceiverTarget::fallback_host() const {
    return fallback_host_;
}

uint64_t ReceiverTarget::steady_now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

bool ReceiverTarget::update_locked(const std::string &host, const std::string &receiver_id, bool discovered) {
    if(current_.host == host && current_.receiver_id == receiver_id && current_.discovered == discovered) {
        return false;
    }
    current_.host = host;
    current_.receiver_id = receiver_id;
    current_.discovered = discovered;
    ++current_.generation;
    last_success_ms_ = 0;
    return true;
}

}  // namespace gwv3
