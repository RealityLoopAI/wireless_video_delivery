#include "gwv3_sender/adaptive_exposure_controller.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace gwv3 {

AdaptiveExposureController::AdaptiveExposureController(AdaptiveExposureConfig config, int initial_exposure, int initial_gain)
    : config_(std::move(config)),
      exposure_(std::clamp(initial_exposure, config_.exposure_min, config_.exposure_max)),
      gain_(std::clamp(initial_gain, config_.gain_min, config_.gain_max)) {}

ExposureControlDecision AdaptiveExposureController::evaluate(const ExposureMeteringSample &sample) {
    ++sample_count_;
    ExposureControlDecision decision{false, exposure_, gain_, "stable"};
    const bool use_midtones = config_.target_p50_luma >= 0;
    const int target_luma = use_midtones ? config_.target_p50_luma : config_.target_p95_luma;
    const int measured_luma = use_midtones ? sample.p50_luma : sample.p95_luma;
    const int upper_luma = target_luma + config_.luma_deadband;
    const int lower_luma = target_luma - config_.luma_deadband;
    const int p95_ceiling = config_.target_p95_luma + config_.luma_deadband;
    const bool severe_highlights = sample.p99_luma >= config_.highlight_luma
                                   || sample.highlight_fraction > config_.max_highlight_fraction;
    const bool overexposed = severe_highlights || measured_luma > upper_luma;

    const auto proportional_exposure_step = [&](bool increase) {
        const double measured = static_cast<double>(std::max(1, measured_luma));
        const double ideal_exposure = static_cast<double>(exposure_) * static_cast<double>(target_luma) / measured;
        const double raw_delta = increase ? ideal_exposure - static_cast<double>(exposure_)
                                          : static_cast<double>(exposure_) - ideal_exposure;
        return std::clamp(static_cast<int>(std::lround(std::max(2.0, raw_delta))),
                          2, config_.max_exposure_step);
    };

    if(overexposed) {
        consecutive_underexposed_ = 0;
        if(gain_ > config_.gain_min) {
            const int gain_step = severe_highlights ? gain_ - config_.gain_min : 2;
            decision.gain = std::max(config_.gain_min, gain_ - gain_step);
            decision.apply = decision.gain != gain_;
            decision.reason = severe_highlights ? "severe_highlights_reduce_gain" : "bright_reduce_gain";
            return decision;
        }
        if(exposure_ > config_.exposure_min) {
            const int severe_step = std::clamp(
                config_.max_exposure_step + config_.max_exposure_step / 2, 10, 30);
            const int exposure_step = severe_highlights ? severe_step : proportional_exposure_step(false);
            decision.exposure = std::max(config_.exposure_min, exposure_ - exposure_step);
            decision.apply = decision.exposure != exposure_;
            decision.reason = severe_highlights ? "severe_highlights_reduce_exposure" : "bright_reduce_exposure";
            return decision;
        }
        decision.reason = "bright_at_minimum";
        return decision;
    }

    if(measured_luma < lower_luma) {
        if(use_midtones && sample.p95_luma >= p95_ceiling) {
            consecutive_underexposed_ = 0;
            decision.reason = "highlight_limited";
            return decision;
        }
        ++consecutive_underexposed_;
        if(consecutive_underexposed_ < config_.underexposed_samples) {
            decision.reason = "dark_hysteresis";
            return decision;
        }
        consecutive_underexposed_ = 0;
        if(exposure_ < config_.exposure_max) {
            const int exposure_step = proportional_exposure_step(true);
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
