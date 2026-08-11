#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace gwv3 {

inline uint64_t sender_system_time_from_global(uint64_t global_us, int64_t receiver_minus_sender_us) {
    if(receiver_minus_sender_us >= 0) {
        const uint64_t offset = static_cast<uint64_t>(receiver_minus_sender_us);
        return global_us > offset ? global_us - offset : 1;
    }
    const uint64_t magnitude = static_cast<uint64_t>(-(receiver_minus_sender_us + 1)) + 1;
    return global_us <= std::numeric_limits<uint64_t>::max() - magnitude
               ? global_us + magnitude
               : std::numeric_limits<uint64_t>::max();
}

inline bool scheduled_keyframe_due(uint64_t target_sender_system_us,
                                   uint64_t frame_system_timestamp_us,
                                   uint64_t current_sender_system_us,
                                   uint64_t fallback_lateness_us = 100'000) {
    if(target_sender_system_us == 0 || frame_system_timestamp_us >= target_sender_system_us) {
        return true;
    }
    return current_sender_system_us >= target_sender_system_us
           && current_sender_system_us - target_sender_system_us >= fallback_lateness_us;
}

inline uint64_t merge_keyframe_target(uint64_t pending_target_us,
                                      uint64_t requested_target_us,
                                      bool low_priority_immediate_request) {
    if(pending_target_us == 0) {
        return 0;
    }
    if(requested_target_us == 0) {
        return low_priority_immediate_request ? pending_target_us : 0;
    }
    return std::min(pending_target_us, requested_target_us);
}

}  // namespace gwv3
