#pragma once

#include "gwv3_sender/config.hpp"

#include <cstdint>
#include <deque>
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

    ExposureControlDecision evaluate(const ExposureMeteringSample &sample, double dt_seconds = 0.0);
    void commit(const ExposureControlDecision &decision);

    int exposure() const;
    int gain() const;
    ExposureMeteringSample metering_sample() const;
    bool metering_ready() const;
    uint64_t sample_count() const;
    uint64_t adjustment_count() const;

private:
    ExposureControlDecision evaluate_proportional(const ExposureMeteringSample &sample);
    ExposureControlDecision evaluate_pid(const ExposureMeteringSample &sample, double dt_seconds);
    ExposureControlDecision filter_direction_reversal(ExposureControlDecision decision);

    AdaptiveExposureConfig config_;
    int exposure_ = 0;
    int gain_ = 0;
    int consecutive_underexposed_ = 0;
    double pid_integral_ = 0.0;
    double pid_previous_error_ = 0.0;
    double pid_filtered_derivative_ = 0.0;
    bool pid_has_previous_error_ = false;
    bool highlight_limit_active_ = false;
    int highlight_recovery_samples_ = 0;
    int last_adjustment_direction_ = 0;
    int pending_reversal_direction_ = 0;
    int pending_reversal_samples_ = 0;
    std::deque<ExposureMeteringSample> metering_samples_;
    ExposureMeteringSample metering_sample_;
    bool metering_ready_ = false;
    uint64_t sample_count_ = 0;
    uint64_t adjustment_count_ = 0;
};

}  // namespace gwv3
