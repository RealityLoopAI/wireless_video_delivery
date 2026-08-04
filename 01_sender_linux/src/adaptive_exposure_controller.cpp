#include "gwv3_sender/adaptive_exposure_controller.hpp"

#include <algorithm>
#include <utility>

namespace gwv3 {

AdaptiveExposureController::AdaptiveExposureController(AdaptiveExposureConfig config, int initial_exposure, int initial_gain)
    : config_(std::move(config)),
      exposure_(std::clamp(initial_exposure, config_.exposure_min, config_.exposure_max)),
      gain_(std::clamp(initial_gain, config_.gain_min, config_.gain_max)) {}

ExposureControlDecision AdaptiveExposureController::evaluate(const ExposureMeteringSample &sample) {
    ++sample_count_;
    ExposureControlDecision decision{false, exposure_, gain_, "stable"};
    const int upper_luma = config_.target_p95_luma + config_.luma_deadband;
    const int lower_luma = config_.target_p95_luma - config_.luma_deadband;
    const bool severe_highlights = sample.p99_luma >= config_.highlight_luma
                                   || sample.highlight_fraction > config_.max_highlight_fraction;
    const bool overexposed = severe_highlights || sample.p95_luma > upper_luma;

    if(overexposed) {
        consecutive_underexposed_ = 0;
        if(gain_ > config_.gain_min) {
            const int gain_step = severe_highlights ? gain_ - config_.gain_min : 4;
            decision.gain = std::max(config_.gain_min, gain_ - gain_step);
            decision.apply = decision.gain != gain_;
            decision.reason = severe_highlights ? "severe_highlights_reduce_gain" : "bright_reduce_gain";
            return decision;
        }
        if(exposure_ > config_.exposure_min) {
            const int excess = std::max(1, sample.p95_luma - upper_luma);
            const int exposure_step = severe_highlights ? std::clamp(excess / 2, 10, 30)
                                                        : std::clamp((excess + 1) / 2, 4, 10);
            decision.exposure = std::max(config_.exposure_min, exposure_ - exposure_step);
            decision.apply = decision.exposure != exposure_;
            decision.reason = severe_highlights ? "severe_highlights_reduce_exposure" : "bright_reduce_exposure";
            return decision;
        }
        decision.reason = "bright_at_minimum";
        return decision;
    }

    if(sample.p95_luma < lower_luma) {
        ++consecutive_underexposed_;
        if(consecutive_underexposed_ < config_.underexposed_samples) {
            decision.reason = "dark_hysteresis";
            return decision;
        }
        consecutive_underexposed_ = 0;
        if(exposure_ < config_.exposure_max) {
            const int deficit = config_.target_p95_luma - sample.p95_luma;
            const int exposure_step = std::clamp((deficit + 7) / 8, 2, 8);
            decision.exposure = std::min(config_.exposure_max, exposure_ + exposure_step);
            decision.apply = decision.exposure != exposure_;
            decision.reason = "dark_increase_exposure";
            return decision;
        }
        if(gain_ < config_.gain_max) {
            decision.gain = std::min(config_.gain_max, gain_ + 2);
            decision.apply = decision.gain != gain_;
            decision.reason = "dark_increase_gain";
            return decision;
        }
        decision.reason = "dark_at_maximum";
        return decision;
    }

    consecutive_underexposed_ = 0;
    return decision;
}

void AdaptiveExposureController::commit(const ExposureControlDecision &decision) {
    if(!decision.apply) {
        return;
    }
    exposure_ = std::clamp(decision.exposure, config_.exposure_min, config_.exposure_max);
    gain_ = std::clamp(decision.gain, config_.gain_min, config_.gain_max);
    ++adjustment_count_;
}

int AdaptiveExposureController::exposure() const { return exposure_; }

int AdaptiveExposureController::gain() const { return gain_; }

uint64_t AdaptiveExposureController::sample_count() const { return sample_count_; }

uint64_t AdaptiveExposureController::adjustment_count() const { return adjustment_count_; }

}  // namespace gwv3
