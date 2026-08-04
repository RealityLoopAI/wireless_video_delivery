#pragma once

#include "gwv3_sender/config.hpp"

#include <cstdint>
#include <string>

namespace gwv3 {

struct ExposureMeteringSample {
    int p50_luma = 0;
    int p95_luma = 0;
    int p99_luma = 0;
    double highlight_fraction = 0.0;
};

struct ExposureControlDecision {
    bool apply = false;
    int exposure = 0;
    int gain = 0;
    std::string reason;
};

class AdaptiveExposureController {
public:
    AdaptiveExposureController(AdaptiveExposureConfig config, int initial_exposure, int initial_gain);

    ExposureControlDecision evaluate(const ExposureMeteringSample &sample);
    void commit(const ExposureControlDecision &decision);

    int exposure() const;
    int gain() const;
    uint64_t sample_count() const;
    uint64_t adjustment_count() const;

private:
    AdaptiveExposureConfig config_;
    int exposure_ = 0;
    int gain_ = 0;
    int consecutive_underexposed_ = 0;
    uint64_t sample_count_ = 0;
    uint64_t adjustment_count_ = 0;
};

}  // namespace gwv3
