#pragma once

#include <cstdint>
#include <optional>

namespace gwv3 {

class RgbTransportRecovery {
public:
    enum class SendDecision {
        send,
        drop,
        send_recovery_keyframe,
    };

    bool arm() noexcept {
        const bool newly_armed = !waiting_for_keyframe_;
        if(newly_armed) {
            dropped_frames_ = 0;
            last_keyframe_request_us_ = 0;
        }
        waiting_for_keyframe_ = true;
        return newly_armed;
    }

    SendDecision before_send(bool is_keyframe) noexcept {
        if(!waiting_for_keyframe_) {
            return SendDecision::send;
        }
        if(is_keyframe) {
            return SendDecision::send_recovery_keyframe;
        }
        ++dropped_frames_;
        return SendDecision::drop;
    }

    bool keyframe_request_due(uint64_t monotonic_now_us, uint64_t interval_us) noexcept {
        if(!waiting_for_keyframe_) {
            return false;
        }
        if(last_keyframe_request_us_ != 0 && monotonic_now_us >= last_keyframe_request_us_
           && monotonic_now_us - last_keyframe_request_us_ < interval_us) {
            return false;
        }
        last_keyframe_request_us_ = monotonic_now_us;
        return true;
    }

    std::optional<uint64_t> complete_successful_send(bool is_keyframe) noexcept {
        if(!waiting_for_keyframe_ || !is_keyframe) {
            return std::nullopt;
        }
        const uint64_t dropped = dropped_frames_;
        reset();
        return dropped;
    }

    void reset() noexcept {
        waiting_for_keyframe_ = false;
        dropped_frames_ = 0;
        last_keyframe_request_us_ = 0;
    }

    bool waiting() const noexcept {
        return waiting_for_keyframe_;
    }

    uint64_t dropped_frames() const noexcept {
        return dropped_frames_;
    }

private:
    bool waiting_for_keyframe_ = false;
    uint64_t dropped_frames_ = 0;
    uint64_t last_keyframe_request_us_ = 0;
};

}  // namespace gwv3
