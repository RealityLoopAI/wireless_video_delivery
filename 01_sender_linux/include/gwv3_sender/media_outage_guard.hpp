#pragma once

#include <cstdint>

namespace gwv3 {

inline bool media_outage_restart_due(uint32_t &consecutive_samples,
                                     int restart_samples,
                                     bool media_outage) noexcept {
    if(restart_samples <= 0) {
        consecutive_samples = 0;
        return false;
    }
    if(!media_outage) {
        consecutive_samples = 0;
        return false;
    }
    if(consecutive_samples < static_cast<uint32_t>(restart_samples)) {
        ++consecutive_samples;
    }
    return consecutive_samples >= static_cast<uint32_t>(restart_samples);
}

} // namespace gwv3
