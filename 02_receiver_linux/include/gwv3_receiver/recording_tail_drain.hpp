#pragma once

#include <chrono>
#include <cstdint>

namespace gwv3 {

// Serialized by the owning receiver camera lock. Progress is per ordered media
// stream; heartbeat/preview arrival and an empty disk queue are not watermarks.
class RecordingTailDrain {
public:
    using Clock = std::chrono::steady_clock;

    void begin(uint64_t end_us, bool rgb, bool depth, Clock::time_point now,
               std::chrono::milliseconds timeout) {
        end_us_ = end_us;
        rgb_done_ = !rgb;
        depth_done_ = !depth;
        deadline_ = now + timeout;
        active_ = true;
    }

    void observe(bool rgb, uint64_t stamp_us, uint64_t receive_us) {
        if(!active_ || stamp_us <= end_us_) {
            return;
        }
        if(stamp_us > receive_us && stamp_us - receive_us > 500000) {
            return;
        }
        (rgb ? rgb_done_ : depth_done_) = true;
    }

    bool active() const { return active_; }
    bool complete() const { return rgb_done_ && depth_done_; }
    bool expired(Clock::time_point now) const { return now >= deadline_; }
    bool rgb_done() const { return rgb_done_; }
    bool depth_done() const { return depth_done_; }
    uint64_t end_us() const { return end_us_; }
    void finish() { active_ = false; }

private:
    bool active_ = false;
    bool rgb_done_ = false;
    bool depth_done_ = false;
    uint64_t end_us_ = 0;
    Clock::time_point deadline_{};
};

}  // namespace gwv3
