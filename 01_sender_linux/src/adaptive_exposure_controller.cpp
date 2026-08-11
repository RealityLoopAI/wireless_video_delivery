#include "gwv3_sender/adaptive_exposure_controller.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace gwv3 {

AdaptiveExposureController::AdaptiveExposureController(AdaptiveExposureConfig config, int initial_exposure, int initial_gain)
    : config_(std::move(config)),
      exposure_(std::clamp(initial_exposure, config_.exposure_min, config_.exposure_max)),
      gain_(std::clamp(initial_gain, config_.gain_min, config_.gain_max)) {}

ExposureControlDecision AdaptiveExposureController::evaluate(const ExposureMeteringSample &sample, double dt_seconds) {
    ++sample_count_;
    metering_samples_.push_back(sample);
    while(metering_samples_.size() > static_cast<size_t>(config_.metering_window)) {
        metering_samples_.pop_front();
    }
    if(metering_samples_.size() < static_cast<size_t>(config_.metering_window)) {
        metering_ready_ = false;
        return {false, exposure_, gain_, "metering_warmup"};
    }

    const auto median_int = [&](auto member) {
        std::vector<int> values;
        values.reserve(metering_samples_.size());
        for(const auto &entry : metering_samples_) {
            values.push_back(entry.*member);
        }
        const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
        std::nth_element(values.begin(), middle, values.end());
        return *middle;
    };
    std::vector<double> highlight_fractions;
    highlight_fractions.reserve(metering_samples_.size());
    for(const auto &entry : metering_samples_) {
        highlight_fractions.push_back(entry.highlight_fraction);
    }
    const auto highlight_middle = highlight_fractions.begin()
                                  + static_cast<std::ptrdiff_t>(highlight_fractions.size() / 2);
    std::nth_element(highlight_fractions.begin(), highlight_middle, highlight_fractions.end());
    const ExposureMeteringSample filtered{
        median_int(&ExposureMeteringSample::p50_luma),
        median_int(&ExposureMeteringSample::p95_luma),
        median_int(&ExposureMeteringSample::p99_luma),
        *highlight_middle};
    metering_sample_ = filtered;
    metering_ready_ = true;

    const double highlight_recovery_fraction =
        config_.max_highlight_fraction * config_.highlight_recovery_ratio;
    if(filtered.highlight_fraction > config_.max_highlight_fraction) {
        highlight_limit_active_ = true;
        highlight_recovery_samples_ = 0;
    }
    else if(highlight_limit_active_) {
        const bool midtones_need_recovery = config_.target_p50_luma >= 0
                                             && filtered.p50_luma
                                                    < config_.target_p50_luma - config_.luma_deadband;
        const bool upper_tones_recovered = config_.target_p50_luma < 0
                                           || filtered.p95_luma
                                                  < config_.target_p95_luma - config_.luma_deadband
                                           || (midtones_need_recovery
                                               && filtered.highlight_fraction
                                                      <= config_.max_highlight_fraction);
        if(filtered.highlight_fraction <= highlight_recovery_fraction
           && upper_tones_recovered) {
            ++highlight_recovery_samples_;
            if(highlight_recovery_samples_ >= config_.highlight_release_samples) {
                highlight_limit_active_ = false;
                highlight_recovery_samples_ = 0;
            }
        }
        else {
            highlight_recovery_samples_ = 0;
        }
    }

    ExposureControlDecision decision;
    if(config_.control_mode == "pid") {
        if(dt_seconds <= 0.0 || !std::isfinite(dt_seconds)) {
            dt_seconds = static_cast<double>(config_.interval_ms) / 1000.0;
        }
        decision = evaluate_pid(filtered, std::clamp(dt_seconds, 0.001, 2.0));
    }
    else {
        decision = evaluate_proportional(filtered);
    }
    return filter_direction_reversal(std::move(decision));
}

ExposureControlDecision AdaptiveExposureController::filter_direction_reversal(
    ExposureControlDecision decision) {
    if(!decision.apply) {
        pending_reversal_direction_ = 0;
        pending_reversal_samples_ = 0;
        return decision;
    }

    const int direction = decision.exposure != exposure_
                              ? (decision.exposure > exposure_ ? 1 : -1)
                              : (decision.gain > gain_ ? 1 : -1);
    const bool urgent_highlight_reduction =
        direction < 0 && decision.reason.find("severe_highlights") != std::string::npos;
    if(urgent_highlight_reduction || last_adjustment_direction_ == 0
       || direction == last_adjustment_direction_ || config_.direction_reversal_samples <= 1) {
        pending_reversal_direction_ = 0;
        pending_reversal_samples_ = 0;
        return decision;
    }

    if(direction != pending_reversal_direction_) {
        pending_reversal_direction_ = direction;
        pending_reversal_samples_ = 1;
    }
    else {
        ++pending_reversal_samples_;
    }
    if(pending_reversal_samples_ < config_.direction_reversal_samples) {
        return {false, exposure_, gain_, "direction_reversal_hysteresis"};
    }

    pending_reversal_direction_ = 0;
    pending_reversal_samples_ = 0;
    return decision;
}

ExposureControlDecision AdaptiveExposureController::evaluate_proportional(const ExposureMeteringSample &sample) {
    ExposureControlDecision decision{false, exposure_, gain_, "stable"};
    const bool use_midtones = config_.target_p50_luma >= 0;
    const int target_luma = use_midtones ? config_.target_p50_luma : config_.target_p95_luma;
    const int measured_luma = use_midtones ? sample.p50_luma : sample.p95_luma;
    const int upper_luma = target_luma + config_.luma_deadband;
    const int lower_luma = target_luma - config_.luma_deadband;
    const int p95_ceiling = config_.target_p95_luma + config_.luma_deadband;
    const bool severe_highlights = sample.highlight_fraction > config_.max_highlight_fraction;
    const bool highlight_ceiling_exceeded = use_midtones && sample.p95_luma > p95_ceiling;
    const bool upper_tones_near_target = sample.p95_luma
                                         >= config_.target_p95_luma - config_.luma_deadband;
    const bool soft_highlights = use_midtones && config_.soft_highlight_luma >= 0
                                 && sample.p99_luma >= config_.soft_highlight_luma
                                 && upper_tones_near_target;
    const bool soft_highlight_limit = highlight_ceiling_exceeded || soft_highlights;
    const bool midtones_overexposed = measured_luma > upper_luma;
    const bool overexposed = severe_highlights || soft_highlight_limit || midtones_overexposed;

    const auto proportional_exposure_step = [&](int measured_luma_value, int target_luma_value, bool increase) {
        const double measured = static_cast<double>(std::max(1, measured_luma_value));
        const double ideal_exposure =
            static_cast<double>(exposure_) * static_cast<double>(target_luma_value) / measured;
        const double raw_delta = increase ? ideal_exposure - static_cast<double>(exposure_)
                                          : static_cast<double>(exposure_) - ideal_exposure;
        return std::clamp(static_cast<int>(std::lround(std::max(1.0, raw_delta))),
                          1, config_.max_exposure_step);
    };

    if(overexposed) {
        consecutive_underexposed_ = 0;
        if(gain_ > config_.gain_min) {
            const int gain_step = severe_highlights ? gain_ - config_.gain_min : config_.max_gain_step;
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
            int exposure_step = severe_highlights
                                    ? proportional_exposure_step(
                                          std::max(sample.p99_luma, config_.highlight_luma),
                                          std::max(1, config_.highlight_luma - config_.luma_deadband),
                                          false)
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
            if(severe_highlights && use_midtones && midtones_overexposed) {
                exposure_step = std::max(
                    exposure_step,
                    proportional_exposure_step(measured_luma, target_luma, false));
            }
            if(severe_highlights && sample.p95_luma > config_.target_p95_luma) {
                exposure_step = std::max(
                    exposure_step,
                    proportional_exposure_step(sample.p95_luma, config_.target_p95_luma, false));
            }
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
        if(highlight_limit_active_) {
            consecutive_underexposed_ = 0;
            decision.reason = "highlight_hysteresis";
            return decision;
        }
        const bool soft_highlights_near_limit = config_.soft_highlight_luma >= 0
                                                && sample.p99_luma
                                                       >= std::max(1, config_.soft_highlight_luma
                                                                         - config_.luma_deadband)
                                                && upper_tones_near_target;
        if(use_midtones
           && (sample.p95_luma >= config_.target_p95_luma || soft_highlights_near_limit)) {
            consecutive_underexposed_ = 0;
            decision.reason = "highlight_limited";
            return decision;
        }
        consecutive_underexposed_ = std::min(
            consecutive_underexposed_ + 1, config_.underexposed_samples);
        if(consecutive_underexposed_ < config_.underexposed_samples) {
            decision.reason = "dark_hysteresis";
            return decision;
        }
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
            decision.gain = std::min(config_.gain_max, gain_ + config_.max_gain_step);
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

ExposureControlDecision AdaptiveExposureController::evaluate_pid(const ExposureMeteringSample &sample, double dt_seconds) {
    ExposureControlDecision decision{false, exposure_, gain_, "stable"};
    const bool use_midtones = config_.target_p50_luma >= 0;
    const int target_luma = use_midtones ? config_.target_p50_luma : config_.target_p95_luma;
    const int measured_luma = use_midtones ? sample.p50_luma : sample.p95_luma;
    const int upper_luma = target_luma + config_.luma_deadband;
    const int lower_luma = target_luma - config_.luma_deadband;
    const bool excessive_highlights = sample.highlight_fraction > config_.max_highlight_fraction;
    const bool midtones_overexposed = measured_luma > upper_luma;
    // P95/P99 can be dominated by a lamp, reflection, or sheet of white paper.
    // Let P50 control the scene until clipping occupies a configured portion of
    // the image; otherwise a local highlight would darken every other region.
    // Preserve a dark foreground unless clipping covers enough of the image to
    // push P95 into the clipping range; that case is no longer a local outlier.
    const double broad_clip_fraction = std::min(
        1.0, config_.max_highlight_fraction * 1.5);
    const bool broadly_clipped = use_midtones
                                 && sample.p95_luma >= config_.highlight_luma
                                 && sample.highlight_fraction >= broad_clip_fraction;
    const bool highlight_protection = excessive_highlights
                                      && (measured_luma > lower_luma || broadly_clipped);
    const bool overexposed = highlight_protection || midtones_overexposed;

    const auto log_error = [](int target, int measured) {
        return std::log(static_cast<double>(std::max(1, target))
                        / static_cast<double>(std::max(1, measured)));
    };
    const auto gain_step_for_error = [&](int target, int measured) {
        const double magnitude = std::abs(log_error(target, measured));
        const double fraction = std::clamp(magnitude / 0.5, 0.25, 1.0);
        return std::clamp(static_cast<int>(std::lround(config_.max_gain_step * fraction)),
                          1, config_.max_gain_step);
    };
    const auto reset_opposing_integral = [&](double error) {
        if((error < 0.0 && pid_integral_ > 0.0) || (error > 0.0 && pid_integral_ < 0.0)) {
            pid_integral_ = 0.0;
        }
    };
    const auto exposure_decision = [&](double error, int lower_bound, int upper_bound,
                                       const std::string &reason) {
        reset_opposing_integral(error);
        const double raw_derivative = pid_has_previous_error_
                                          ? (error - pid_previous_error_) / dt_seconds
                                          : 0.0;
        pid_filtered_derivative_ = config_.pid_derivative_alpha * raw_derivative
                                   + (1.0 - config_.pid_derivative_alpha) * pid_filtered_derivative_;
        pid_previous_error_ = error;
        pid_has_previous_error_ = true;

        const double integral_candidate = std::clamp(
            pid_integral_ + config_.pid_ki * error * dt_seconds,
            -config_.pid_integral_limit, config_.pid_integral_limit);
        const double control = config_.pid_kp * error + integral_candidate
                               + config_.pid_kd * pid_filtered_derivative_;
        const double scaled = static_cast<double>(exposure_) * std::exp(std::clamp(control, -2.0, 2.0));
        const int raw_target = static_cast<int>(std::lround(scaled));
        const int slew_limited = std::clamp(raw_target,
                                            exposure_ - config_.max_exposure_step,
                                            exposure_ + config_.max_exposure_step);
        const int target = std::clamp(slew_limited, lower_bound, upper_bound);
        const bool saturated = (error > 0.0 && target == upper_bound && raw_target > upper_bound)
                               || (error < 0.0 && target == lower_bound && raw_target < lower_bound);
        if(!saturated) {
            pid_integral_ = integral_candidate;
        }

        ExposureControlDecision result{target != exposure_, target, gain_, reason};
        if(!result.apply) {
            result.reason = "pid_quantized_hold";
        }
        return result;
    };

    if(overexposed) {
        consecutive_underexposed_ = 0;
        pid_integral_ = std::min(0.0, pid_integral_);
        if(gain_ > config_.gain_min) {
            const bool upper_tones_drive_reduction = highlight_protection
                                                     && sample.p95_luma > config_.target_p95_luma;
            const int gain_target = upper_tones_drive_reduction
                                        ? config_.target_p95_luma
                                        : (highlight_protection && !midtones_overexposed
                                               ? lower_luma
                                               : target_luma);
            const int gain_measurement = upper_tones_drive_reduction
                                             ? sample.p95_luma
                                             : measured_luma;
            int gain_step = gain_step_for_error(gain_target, gain_measurement);
            if(highlight_protection) {
                const double allowed_fraction = std::max(config_.max_highlight_fraction, 1e-6);
                const double severity = sample.highlight_fraction / allowed_fraction;
                const int step_multiplier = std::clamp(
                    static_cast<int>(std::lround(std::sqrt(std::max(1.0, severity)))), 1, 8);
                gain_step = std::max(gain_step,
                                     std::min(config_.max_gain_step * step_multiplier,
                                              gain_ - config_.gain_min));
            }
            decision.gain = std::max(config_.gain_min, gain_ - gain_step);
            decision.apply = decision.gain != gain_;
            decision.reason = highlight_protection ? "pid_severe_highlights_reduce_gain"
                                                    : "pid_bright_reduce_gain";
            return decision;
        }

        const int effective_floor = config_.exposure_min;
        if(exposure_ <= effective_floor) {
            decision.reason = "pid_bright_at_minimum";
            return decision;
        }

        double error = 0.0;
        if(midtones_overexposed) {
            error = std::min(error, log_error(target_luma, measured_luma));
        }
        if(highlight_protection) {
            if(measured_luma > lower_luma) {
                error = std::min(error, log_error(lower_luma, measured_luma));
            }
            if(sample.p95_luma > config_.target_p95_luma) {
                error = std::min(error, log_error(config_.target_p95_luma, sample.p95_luma));
            }
        }
        if(error >= 0.0) {
            error = -1.0 / static_cast<double>(std::max(1, exposure_));
        }
        return exposure_decision(error, effective_floor, config_.exposure_max,
                                 highlight_protection ? "pid_severe_highlights_reduce_exposure"
                                                      : "pid_bright_reduce_exposure");
    }

    if(measured_luma < lower_luma) {
        consecutive_underexposed_ = std::min(
            consecutive_underexposed_ + 1, config_.underexposed_samples);
        if(consecutive_underexposed_ < config_.underexposed_samples) {
            decision.reason = "dark_hysteresis";
            return decision;
        }

        const int recovery_target = highlight_limit_active_ ? lower_luma : target_luma;
        const double error = log_error(recovery_target, measured_luma);
        int recovery_exposure_ceiling = config_.exposure_max;
        if(highlight_limit_active_) {
            // A clipped local light source can keep P95 saturated even after the
            // rest of the scene becomes dark. Recover toward the lower midtone
            // boundary with a bounded slew instead of freezing on that P95.
            recovery_exposure_ceiling = std::min(
                recovery_exposure_ceiling, exposure_ + config_.max_recovery_exposure_step);
        }
        if(exposure_ < recovery_exposure_ceiling) {
            return exposure_decision(error, config_.exposure_min, recovery_exposure_ceiling,
                                     highlight_limit_active_ ? "pid_highlight_recovery_exposure"
                                                             : "pid_dark_increase_exposure");
        }
        pid_integral_ = std::max(0.0, pid_integral_);
        if(gain_ < config_.gain_max) {
            const int gain_step = gain_step_for_error(recovery_target, measured_luma);
            decision.gain = std::min(config_.gain_max, gain_ + gain_step);
            decision.apply = decision.gain != gain_;
            decision.reason = "pid_dark_increase_gain";
            return decision;
        }
        decision.reason = "pid_dark_at_maximum";
        return decision;
    }

    consecutive_underexposed_ = 0;
    pid_integral_ *= std::exp(-2.0 * dt_seconds);
    pid_previous_error_ = 0.0;
    pid_filtered_derivative_ *= std::exp(-4.0 * dt_seconds);
    pid_has_previous_error_ = true;
    return decision;
}

void AdaptiveExposureController::commit(const ExposureControlDecision &decision) {
    if(!decision.apply) {
        return;
    }
    const int next_exposure = std::clamp(decision.exposure, config_.exposure_min, config_.exposure_max);
    const int next_gain = std::clamp(decision.gain, config_.gain_min, config_.gain_max);
    last_adjustment_direction_ = next_exposure != exposure_
                                     ? (next_exposure > exposure_ ? 1 : -1)
                                     : (next_gain > gain_ ? 1 : -1);
    exposure_ = next_exposure;
    gain_ = next_gain;
    consecutive_underexposed_ = 0;
    metering_samples_.clear();
    metering_ready_ = false;
    ++adjustment_count_;
}

int AdaptiveExposureController::exposure() const { return exposure_; }

int AdaptiveExposureController::gain() const { return gain_; }

ExposureMeteringSample AdaptiveExposureController::metering_sample() const { return metering_sample_; }

bool AdaptiveExposureController::metering_ready() const { return metering_ready_; }

uint64_t AdaptiveExposureController::sample_count() const { return sample_count_; }

uint64_t AdaptiveExposureController::adjustment_count() const { return adjustment_count_; }

}  // namespace gwv3
