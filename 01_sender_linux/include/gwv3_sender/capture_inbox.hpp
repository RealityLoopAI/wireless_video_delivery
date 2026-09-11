#pragma once

#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace gwv3 {

// SDK callbacks must not wait for encoding. Overflow is surfaced to the consumer.
template <typename T, size_t Capacity> class CaptureInbox {
    static_assert(Capacity > 0);
public:
    explicit CaptureInbox(bool accepting = true) : accepting_(accepting) {}

    void start() {
        std::lock_guard<std::mutex> lock(mutex_);
        accepting_ = true;
    }

    void push(T value) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!accepting_) return;
        if(queue_.size() == Capacity) {
            overflow_ = true;
            return;
        }
        queue_.push_back(std::move(value));
    }

    std::optional<T> pop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if(overflow_) {
            throw std::runtime_error("native capture SDK depth inbox overflow");
        }
        if(queue_.empty()) {
            return std::nullopt;
        }
        T value = std::move(queue_.front());
        queue_.pop_front();
        return value;
    }
private:
    std::mutex mutex_;
    std::deque<T> queue_;
    bool overflow_ = false;
    bool accepting_;
};
} // namespace gwv3
