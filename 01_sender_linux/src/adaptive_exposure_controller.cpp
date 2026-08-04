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
    const bool highlight_ceiling_exceeded = use_midtones && sample.p95_luma > p95_ceiling;
    const bool soft_highlights = use_midtones && config_.soft_highlight_luma >= 0
                                 && sample.p99_luma >= config_.soft_highlight_luma;
    const bool soft_highlight_limit = highlight_ceiling_exceeded || soft_highlights;
    const bool midtones_overexposed = measured_luma > upper_luma;
    const bool overexposed = severe_highlights || soft_highlight_limit || midtones_overexposed;

    const auto proportional_exposure_step = [&](int measured_luma_value, int target_luma_value, bool increase) {
        const double measured = static_cast<double>(std::max(1, measured_luma_value));
        const double ideal_exposure =
            static_cast<double>(exposure_) * static_cast<double>(target_luma_value) / measured;
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
            decision.reason = severe_highlights
                                  ? "severe_highlights_reduce_gain"
                                  : (soft_highlights
                                         ? "soft_highlights_reduce_gain"
                                         : (highlight_ceiling_exceeded ? "highlight_ceiling_reduce_gain"
                                                                       : "bright_reduce_gain"));
            return decision;
        }
        if(exposure_ > config_.exposure_min) {
            const int soft_highlight_floor = config_.soft_highlight_exposure_floor >= 0
                                                 ? config_.soft_highlight_exposure_floor
                                                 : config_.exposure_min;
            const int effective_floor = soft_highlight_limit && !severe_highlights && !midtones_overexposed
                                            ? soft_highlight_floor
                                            : config_.exposure_min;
            if(exposure_ <= effective_floor) {
                decision.reason = soft_highlights
                                      ? "soft_highlight_limited_at_floor"
                                      : (highlight_ceiling_exceeded ? "highlight_limited_at_floor" : "bright_at_minimum");
                return decision;
            }
            const int severe_step = std::clamp(
                config_.max_exposure_step + config_.max_exposure_step / 2, 10, 30);
            const int exposure_step = severe_highlights
                                          ? severe_step
                                          : proportional_exposure_step(
                                                soft_highlights ? sample.p99_luma
                                                                : (highlight_ceiling_exceeded ? sample.p95_luma
                                                                                              : measured_luma),
                                                soft_highlights
                                                    ? std::max(config_.target_p95_luma,
                                                               config_.soft_highlight_luma - config_.luma_deadband)
                                                    : (highlight_ceiling_exceeded ? config_.target_p95_luma
                                                                                  : target_luma),
                                                false);
            decision.exposure = std::max(effective_floor, exposure_ - exposure_step);
            decision.apply = decision.exposure != exposure_;
            decision.reason = severe_highlights
                                  ? "severe_highlights_reduce_exposure"
                                  : (soft_highlights
                                         ? "soft_highlights_reduce_exposure"
                                         : (highlight_ceiling_exceeded ? "highlight_ceiling_reduce_exposure"
                                                                       : "bright_reduce_exposure"));
            return decision;
        }
        decision.reason = "bright_at_minimum";
        return decision;
    }

    if(measured_luma < lower_luma) {
        const bool soft_highlights_near_limit = config_.soft_highlight_luma >= 0
                                                && sample.p99_luma
                                                       >= std::max(1, config_.soft_highlight_luma
                                                                         - config_.luma_deadband);
        if(use_midtones
           && (sample.p95_luma >= config_.target_p95_luma || soft_highlights_near_limit)) {
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
            int exposure_step = proportional_exposure_step(measured_luma, target_luma, true);
            if(use_midtones) {
                const double highlight_limited_exposure =
                    static_cast<double>(exposure_) * static_cast<double>(config_.target_p95_luma)
                    / static_cast<double>(std::max(1, sample.p95_luma));
                const int highlight_headroom = std::max(
                    0, static_cast<int>(std::lround(highlight_limited_exposure - static_cast<double>(exposure_))));
                exposure_step = std::min(exposure_step, highlight_headroom);
                if(exposure_step == 0) {
                    decision.reason = "highlight_limited";
                    return decision;
                }
            }
            decision.exposure = std::min(config_.exposure_max, exposure_ + exposure_step);
            decision.apply = decision.exposure != exposure_;
            decision.reason = "dark_increase_exposure";
            return decision;
        }
        if(gain_ < config_.gain_max) {
            if(use_midtones
               && sample.p95_luma > config_.target_p95_luma - config_.luma_deadband) {
                decision.reason = "highlight_limited";
                return decision;
            }
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
