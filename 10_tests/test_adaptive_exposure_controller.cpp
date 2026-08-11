#include "gwv3_sender/adaptive_exposure_controller.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(bool condition, const std::string &message) {
    if(!condition) {
        std::cerr << "test failure: " << message << '\n';
        std::exit(1);
    }
}

gwv3::AdaptiveExposureConfig test_config() {
    gwv3::AdaptiveExposureConfig config;
    config.enabled = true;
    config.exposure_min = 80;
    config.exposure_max = 200;
    config.gain_min = 16;
    config.gain_max = 32;
    config.target_p95_luma = 200;
    config.luma_deadband = 8;
    config.highlight_luma = 245;
    config.max_highlight_fraction = 0.0025;
    config.underexposed_samples = 3;
    return config;
}

gwv3::ExposureMeteringSample sample(int p95, int p99, double highlight_fraction = 0.0) {
    return {p95 / 2, p95, p99, highlight_fraction};
}

gwv3::ExposureMeteringSample mixed_sample(int p50, int p95, int p99, double highlight_fraction = 0.0) {
    return {p50, p95, p99, highlight_fraction};
}

}  // namespace

int main() {
    using gwv3::AdaptiveExposureController;

    {
        AdaptiveExposureController controller(test_config(), 130, 16);
        const auto decision = controller.evaluate(sample(200, 210));
        require(!decision.apply, "in-band sample must not change exposure");
        require(decision.reason == "stable", "in-band sample should be stable");
    }

    {
        AdaptiveExposureController controller(test_config(), 130, 32);
        auto decision = controller.evaluate(sample(220, 230));
        require(decision.apply && decision.exposure == 130 && decision.gain == 30,
                "moderate brightness must reduce gain before exposure");
        controller.commit(decision);
        require(controller.gain() == 30 && controller.adjustment_count() == 1, "committed gain adjustment not tracked");
    }

    {
        AdaptiveExposureController controller(test_config(), 130, 32);
        auto decision = controller.evaluate(sample(240, 250, 0.02));
        require(decision.apply && decision.gain == 16, "severe highlights must remove optional gain immediately");
        controller.commit(decision);
        decision = controller.evaluate(sample(240, 250, 0.02));
        require(decision.apply && decision.exposure < 130, "severe highlights at minimum gain must reduce exposure");
    }

    {
        AdaptiveExposureController controller(test_config(), 130, 16);
        require(!controller.evaluate(sample(150, 160)).apply, "first dark sample must be held by hysteresis");
        require(!controller.evaluate(sample(150, 160)).apply, "second dark sample must be held by hysteresis");
        auto decision = controller.evaluate(sample(150, 160));
        require(decision.apply && decision.exposure > 130 && decision.gain == 16,
                "sustained darkness must increase exposure first");
    }

    {
        AdaptiveExposureController controller(test_config(), 200, 16);
        controller.evaluate(sample(150, 160));
        controller.evaluate(sample(150, 160));
        const auto decision = controller.evaluate(sample(150, 160));
        require(decision.apply && decision.exposure == 200 && decision.gain == 18,
                "gain may increase only after exposure reaches its cap");
    }

    {
        AdaptiveExposureController controller(test_config(), 80, 16);
        const auto decision = controller.evaluate(sample(250, 255, 0.5));
        require(!decision.apply && decision.reason == "bright_at_minimum", "minimum limits must be respected");
    }

    {
        auto config = test_config();
        config.target_p50_luma = 105;
        config.target_p95_luma = 225;
        AdaptiveExposureController controller(config, 130, 16);
        const auto decision = controller.evaluate(mixed_sample(105, 220, 230));
        require(!decision.apply && decision.reason == "stable", "balanced mixed scene must stay stable");
    }

    {
        auto config = test_config();
        config.target_p50_luma = 105;
        config.target_p95_luma = 225;
        AdaptiveExposureController controller(config, 130, 16);
        controller.evaluate(mixed_sample(75, 220, 230));
        controller.evaluate(mixed_sample(75, 220, 230));
        const auto decision = controller.evaluate(mixed_sample(75, 220, 230));
        require(decision.apply && decision.exposure > 130 && decision.exposure <= 133,
                "dark midtones must increase only within available highlight headroom");
    }

    {
        auto config = test_config();
        config.target_p50_luma = 105;
        config.target_p95_luma = 225;
        AdaptiveExposureController controller(config, 130, 16);
        const auto decision = controller.evaluate(mixed_sample(75, 235, 240));
        require(decision.apply && decision.exposure < 130
                    && decision.reason == "highlight_ceiling_reduce_exposure",
                "upper tones above the ceiling must actively reduce exposure");
    }

    {
        auto config = test_config();
        config.target_p50_luma = 105;
        config.target_p95_luma = 225;
        config.soft_highlight_exposure_floor = 130;
        AdaptiveExposureController controller(config, 130, 16);
        const auto decision = controller.evaluate(mixed_sample(75, 235, 240));
        require(!decision.apply && decision.exposure == 130
                    && decision.reason == "highlight_limited_at_floor",
                "soft highlight protection must preserve the configured midtone exposure floor");
    }

    {
        auto config = test_config();
        config.target_p50_luma = 105;
        config.target_p95_luma = 225;
        config.soft_highlight_luma = 235;
        config.soft_highlight_exposure_floor = 100;
        AdaptiveExposureController controller(config, 180, 20);
        auto decision = controller.evaluate(mixed_sample(100, 220, 235));
        require(decision.apply && decision.exposure == 180 && decision.gain == 18
                    && decision.reason == "soft_highlights_reduce_gain",
                "P99 soft highlights must reduce gain before exposure");

        controller.commit(decision);
        controller = AdaptiveExposureController(config, 180, 16);
        decision = controller.evaluate(mixed_sample(100, 220, 235));
        require(decision.apply && decision.exposure < 180
                    && decision.reason == "soft_highlights_reduce_exposure",
                "P99 soft highlights must reduce exposure after gain reaches its floor");
    }

    {
        auto config = test_config();
        config.target_p50_luma = 135;
        config.target_p95_luma = 225;
        config.soft_highlight_luma = 235;
        config.underexposed_samples = 1;
        AdaptiveExposureController controller(config, 142, 16);
        const auto decision = controller.evaluate(mixed_sample(66, 149, 235));
        require(decision.apply && decision.exposure > 142
                    && decision.reason == "dark_increase_exposure",
                "an isolated bright tail must not keep the rest of a dark scene underexposed");
    }

    {
        auto config = test_config();
        config.target_p50_luma = 105;
        config.target_p95_luma = 225;
        config.soft_highlight_luma = 235;
        config.soft_highlight_exposure_floor = 130;
        AdaptiveExposureController controller(config, 130, 16);
        const auto decision = controller.evaluate(mixed_sample(75, 220, 235));
        require(!decision.apply && decision.reason == "soft_highlight_limited_at_floor",
                "P99 soft highlight control must preserve its configured exposure floor");
    }

    {
        auto config = test_config();
        config.target_p50_luma = 105;
        config.target_p95_luma = 225;
        config.soft_highlight_luma = 235;
        AdaptiveExposureController controller(config, 130, 16);
        const auto decision = controller.evaluate(mixed_sample(75, 220, 230));
        require(!decision.apply && decision.reason == "highlight_limited",
                "P99 recovery hysteresis must block re-amplification when upper tones are also near the limit");
    }

    {
        auto config = test_config();
        config.target_p50_luma = 105;
        config.target_p95_luma = 225;
        AdaptiveExposureController controller(config, 130, 20);
        const auto decision = controller.evaluate(mixed_sample(75, 235, 240));
        require(decision.apply && decision.exposure == 130 && decision.gain == 18
                    && decision.reason == "highlight_ceiling_reduce_gain",
                "upper tones above the ceiling must reduce gain before exposure");
    }

    {
        auto config = test_config();
        config.target_p50_luma = 105;
        config.target_p95_luma = 225;
        AdaptiveExposureController controller(config, 130, 16);
        const auto decision = controller.evaluate(mixed_sample(75, 228, 235));
        require(!decision.apply && decision.reason == "highlight_limited",
                "upper-tone hysteresis must prevent immediate re-amplification");
    }

    {
        auto config = test_config();
        config.target_p50_luma = 105;
        config.target_p95_luma = 225;
        AdaptiveExposureController controller(config, 130, 20);
        const auto decision = controller.evaluate(mixed_sample(125, 220, 230));
        require(decision.apply && decision.gain == 18, "ordinary gain changes must use a symmetric linear step");
    }

    {
        auto config = test_config();
        config.underexposed_samples = 1;
        AdaptiveExposureController small_error(config, 130, 16);
        AdaptiveExposureController large_error(config, 130, 16);
        const auto small = small_error.evaluate(sample(190, 200));
        const auto large = large_error.evaluate(sample(150, 160));
        require(small.apply && large.apply && large.exposure - 130 > small.exposure - 130,
                "exposure response must scale with the measured luma error");
        require(large.exposure - 130 == config.max_exposure_step,
                "large exposure error must be bounded by the configured linear slew");
    }

    {
        auto config = test_config();
        config.underexposed_samples = 1;
        config.max_exposure_step = 12;
        AdaptiveExposureController controller(config, 120, 16);
        const auto decision = controller.evaluate(sample(100, 110));
        require(decision.apply && decision.exposure == 132,
                "dark response must use the configured maximum exposure slew");
    }

    {
        auto config = test_config();
        config.underexposed_samples = 1;
        config.max_gain_step = 4;
        AdaptiveExposureController controller(config, config.exposure_max, 16);
        const auto decision = controller.evaluate(sample(100, 110));
        require(decision.apply && decision.gain == 20,
                "gain response must use the configured maximum gain slew");
    }

    {
        auto config = test_config();
        config.exposure_min = 1;
        config.exposure_max = 20;
        config.gain_min = 0;
        config.gain_max = 16;
        config.target_p50_luma = 120;
        config.target_p95_luma = 210;
        config.highlight_luma = 235;
        config.underexposed_samples = 1;
        AdaptiveExposureController controller(config, 3, 0);
        const auto decision = controller.evaluate(mixed_sample(130, 235, 235, 0.01));
        require(decision.apply && decision.exposure == 2,
                "low exposure control must retain the device's single-step precision");
    }

    {
        auto config = test_config();
        config.target_p50_luma = 120;
        config.target_p95_luma = 210;
        config.highlight_luma = 235;
        config.max_exposure_step = 170;
        config.exposure_min = 1;
        config.gain_min = 0;
        AdaptiveExposureController controller(config, 200, 0);
        const auto decision = controller.evaluate(mixed_sample(210, 235, 235, 0.4));
        require(decision.apply && decision.exposure <= 115,
                "large-area clipping must use the strictest luma constraint for immediate rollback");
    }

    {
        auto config = test_config();
        config.control_mode = "pid";
        config.target_p50_luma = 120;
        config.target_p95_luma = 210;
        config.underexposed_samples = 1;
        config.max_exposure_step = 80;
        config.pid_kp = 0.6;
        config.pid_ki = 0.0;
        config.pid_kd = 0.0;
        AdaptiveExposureController controller(config, 100, 16);
        const auto decision = controller.evaluate(mixed_sample(60, 100, 120), 0.2);
        require(decision.apply && decision.exposure > 100 && decision.exposure < 180,
                "PID mode must increase exposure proportionally without jumping directly to the estimate");
    }

    {
        auto config = test_config();
        config.control_mode = "pid";
        config.target_p50_luma = 120;
        config.target_p95_luma = 210;
        config.highlight_luma = 235;
        config.exposure_min = 1;
        config.max_exposure_step = 170;
        config.gain_min = 0;
        config.pid_kp = 0.6;
        config.pid_ki = 0.0;
        config.pid_kd = 0.0;
        AdaptiveExposureController controller(config, 200, 0);
        const auto decision = controller.evaluate(mixed_sample(210, 235, 235, 0.4), 0.2);
        require(decision.apply && decision.exposure < 160,
                "PID mode must retain immediate large-area clipping protection");
    }

    {
        auto config = test_config();
        config.control_mode = "pid";
        config.target_p50_luma = 120;
        config.target_p95_luma = 210;
        config.highlight_luma = 235;
        config.exposure_min = 1;
        config.exposure_max = 300;
        config.max_exposure_step = 170;
        config.gain_min = 0;
        config.max_highlight_fraction = 0.02;
        config.pid_kp = 0.6;
        config.pid_ki = 0.0;
        config.pid_kd = 0.0;
        AdaptiveExposureController controller(config, 300, 0);
        const auto decision = controller.evaluate(mixed_sample(80, 225, 250, 0.20), 0.2);
        require(!decision.apply && decision.reason == "dark_hysteresis",
                "large clipped area must not darken an already underexposed foreground");
    }

    {
        auto config = test_config();
        config.control_mode = "pid";
        config.target_p50_luma = 120;
        config.target_p95_luma = 210;
        config.gain_min = 0;
        config.gain_max = 80;
        config.max_gain_step = 16;
        config.max_highlight_fraction = 0.02;
        AdaptiveExposureController controller(config, 200, 80);
        const auto decision = controller.evaluate(mixed_sample(174, 235, 250, 0.073), 0.2);
        require(decision.apply && decision.exposure == 200 && decision.gain == 48,
                "PID gain rollback must scale with clipping area without dropping straight to zero");
    }

    {
        auto config = test_config();
        config.control_mode = "pid";
        config.target_p50_luma = 120;
        config.target_p95_luma = 210;
        config.gain_min = 0;
        config.gain_max = 80;
        config.max_gain_step = 16;
        config.max_highlight_fraction = 0.03;
        AdaptiveExposureController controller(config, 200, 48);
        const auto decision = controller.evaluate(mixed_sample(130, 222, 235, 0.031), 0.2);
        require(decision.apply && decision.gain == 32,
                "a marginal clipping excess must roll gain back by one step only");
    }

    {
        auto config = test_config();
        config.control_mode = "pid";
        config.target_p50_luma = 120;
        config.target_p95_luma = 210;
        AdaptiveExposureController controller(config, 130, 16);
        const auto decision = controller.evaluate(mixed_sample(120, 200, 205), 0.2);
        require(!decision.apply && decision.reason == "stable",
                "PID mode must not integrate while measurements are inside the deadband");
    }

    {
        auto config = test_config();
        config.control_mode = "pid";
        config.target_p50_luma = 120;
        config.target_p95_luma = 210;
        config.soft_highlight_luma = 225;
        config.highlight_luma = 235;
        config.max_highlight_fraction = 0.03;
        AdaptiveExposureController controller(config, 200, 32);
        const auto decision = controller.evaluate(mixed_sample(120, 208, 235, 0.027), 0.2);
        require(!decision.apply && decision.reason == "stable",
                "PID soft highlights must hold a balanced exposure instead of forcing oscillation");
    }

    {
        auto config = test_config();
        config.control_mode = "pid";
        config.target_p50_luma = 120;
        config.exposure_max = 200;
        config.gain_min = 0;
        config.gain_max = 80;
        config.max_gain_step = 16;
        AdaptiveExposureController controller(config, 200, 32);
        const auto decision = controller.evaluate(mixed_sample(140, 200, 220), 0.2);
        require(decision.apply && decision.gain < 32 && decision.gain > 16,
                "PID gain rollback near the target must use a fine step");
    }

    {
        auto config = test_config();
        config.control_mode = "pid";
        config.target_p50_luma = 120;
        config.exposure_max = 200;
        config.gain_min = 0;
        config.gain_max = 80;
        config.max_gain_step = 16;
        config.underexposed_samples = 1;
        AdaptiveExposureController controller(config, 200, 16);
        const auto decision = controller.evaluate(mixed_sample(100, 180, 220), 0.2);
        require(decision.apply && decision.gain > 16 && decision.gain < 32,
                "PID gain recovery near the target must use a fine step");
    }

    {
        auto config = test_config();
        config.control_mode = "pid";
        config.target_p50_luma = 120;
        config.target_p95_luma = 210;
        config.highlight_luma = 235;
        config.max_highlight_fraction = 0.12;
        config.underexposed_samples = 1;
        config.pid_ki = 0.0;
        AdaptiveExposureController controller(config, 100, config.gain_min);
        const auto decision = controller.evaluate(mixed_sample(70, 245, 255, 0.04), 0.2);
        require(decision.apply && decision.exposure > 100
                    && decision.reason == "pid_dark_increase_exposure",
                "a small local highlight must not darken or freeze an underexposed scene");
    }

    {
        auto config = test_config();
        config.control_mode = "pid";
        config.target_p50_luma = 120;
        config.target_p95_luma = 210;
        config.highlight_luma = 235;
        config.max_highlight_fraction = 0.12;
        config.pid_ki = 0.0;
        AdaptiveExposureController controller(config, 160, config.gain_min);
        const auto decision = controller.evaluate(mixed_sample(120, 245, 255, 0.04), 0.2);
        require(!decision.apply && decision.reason == "stable",
                "a small local highlight must not change a correctly exposed scene");
    }

    {
        auto config = test_config();
        config.control_mode = "pid";
        config.target_p50_luma = 120;
        config.target_p95_luma = 210;
        config.highlight_luma = 235;
        config.max_highlight_fraction = 0.12;
        config.exposure_min = 1;
        config.gain_min = 0;
        config.pid_ki = 0.0;
        AdaptiveExposureController controller(config, 200, 0);
        const auto decision = controller.evaluate(mixed_sample(180, 245, 255, 0.20), 0.2);
        require(decision.apply && decision.exposure < 200
                    && decision.reason == "pid_severe_highlights_reduce_exposure",
                "large-area clipping must still reduce global exposure");
    }

    {
        auto config = test_config();
        config.control_mode = "pid";
        config.target_p50_luma = 120;
        config.target_p95_luma = 210;
        config.highlight_luma = 235;
        config.max_highlight_fraction = 0.12;
        config.exposure_min = 1;
        config.gain_min = 0;
        config.pid_ki = 0.0;
        AdaptiveExposureController controller(config, 200, 0);
        const auto decision = controller.evaluate(mixed_sample(80, 250, 255, 0.20), 0.2);
        require(decision.apply && decision.exposure < 200
                    && decision.reason == "pid_severe_highlights_reduce_exposure",
                "broad clipping must be reduced even when the scene median is dark");
    }

    {
        auto config = test_config();
        config.control_mode = "pid";
        config.target_p50_luma = 100;
        config.target_p95_luma = 210;
        config.highlight_luma = 235;
        config.exposure_min = 1;
        config.exposure_max = 300;
        config.gain_min = 0;
        config.max_highlight_fraction = 0.12;
        config.highlight_recovery_ratio = 0.7;
        config.highlight_release_samples = 4;
        config.max_exposure_step = 80;
        config.max_recovery_exposure_step = 5;
        config.underexposed_samples = 1;
        config.pid_ki = 0.0;
        AdaptiveExposureController controller(config, 200, 0);

        auto decision = controller.evaluate(mixed_sample(140, 255, 255, 0.20), 0.2);
        require(decision.apply && decision.exposure < 200,
                "broad clipping must enter highlight protection");
        controller.commit(decision);

        const int reduced_exposure = controller.exposure();
        decision = controller.evaluate(mixed_sample(60, 190, 220, 0.0), 0.2);
        require(decision.apply && decision.exposure > reduced_exposure
                    && decision.exposure <= reduced_exposure + config.max_recovery_exposure_step
                    && decision.reason == "pid_highlight_recovery_exposure",
                "post-highlight recovery must use the configured slow-release slew");
        controller.commit(decision);

        decision = controller.evaluate(mixed_sample(60, 205, 225, 0.0), 0.2);
        require(decision.apply && decision.exposure > controller.exposure()
                    && decision.exposure <= controller.exposure() + config.max_recovery_exposure_step
                    && decision.reason == "pid_highlight_recovery_exposure",
                "dark midtones must keep recovering even while a local upper tone stays bright");
    }

    {
        auto config = test_config();
        config.control_mode = "pid";
        config.target_p50_luma = 100;
        config.target_p95_luma = 210;
        config.highlight_luma = 235;
        config.exposure_min = 1;
        config.exposure_max = 300;
        config.gain_min = 0;
        config.max_highlight_fraction = 0.16;
        config.highlight_release_samples = 4;
        config.max_recovery_exposure_step = 40;
        config.underexposed_samples = 1;
        config.pid_ki = 0.0;
        AdaptiveExposureController controller(config, 100, 0);

        auto decision = controller.evaluate(mixed_sample(140, 255, 255, 0.30), 0.2);
        require(decision.apply && decision.exposure < 100,
                "local-highlight recovery test must first enter protection");
        controller.commit(decision);

        const int reduced_exposure = controller.exposure();
        decision = controller.evaluate(mixed_sample(76, 235, 235, 0.17), 0.2);
        require(decision.apply && decision.exposure > reduced_exposure
                    && decision.exposure <= reduced_exposure + config.max_recovery_exposure_step
                    && decision.reason == "pid_highlight_recovery_exposure",
                "a marginal local-highlight excess must not freeze a dark scene");
    }

    {
        auto config = test_config();
        config.control_mode = "pid";
        config.target_p50_luma = 100;
        config.target_p95_luma = 210;
        config.highlight_luma = 235;
        config.exposure_min = 1;
        config.exposure_max = 300;
        config.gain_min = 0;
        config.max_highlight_fraction = 0.12;
        config.highlight_recovery_ratio = 0.7;
        config.highlight_release_samples = 3;
        config.max_exposure_step = 80;
        config.max_recovery_exposure_step = 5;
        config.underexposed_samples = 1;
        config.pid_ki = 0.0;
        AdaptiveExposureController controller(config, 200, 0);

        auto decision = controller.evaluate(mixed_sample(140, 255, 255, 0.20), 0.2);
        require(decision.apply, "highlight release test must enter protection");
        controller.commit(decision);
        require(!controller.evaluate(mixed_sample(100, 180, 210, 0.0), 0.2).apply,
                "first clean sample must retain highlight protection");
        require(!controller.evaluate(mixed_sample(100, 180, 210, 0.0), 0.2).apply,
                "second clean sample must retain highlight protection");
        require(!controller.evaluate(mixed_sample(100, 180, 210, 0.0), 0.2).apply,
                "release confirmation sample may remain stable");

        const int before_dark_scene = controller.exposure();
        decision = controller.evaluate(mixed_sample(50, 100, 120, 0.0), 0.2);
        require(decision.apply && decision.exposure > before_dark_scene + config.max_recovery_exposure_step
                    && decision.reason == "pid_dark_increase_exposure",
                "confirmed scene change must restore the normal fast dark-scene response");
    }

    {
        auto config = test_config();
        config.control_mode = "pid";
        config.target_p50_luma = 120;
        config.target_p95_luma = 210;
        config.exposure_min = 1;
        config.gain_min = 0;
        config.max_highlight_fraction = 0.02;
        config.highlight_recovery_ratio = 0.7;
        config.underexposed_samples = 1;
        AdaptiveExposureController controller(config, 20, 0);

        auto decision = controller.evaluate(mixed_sample(120, 199, 255, 0.024), 0.2);
        require(decision.apply && decision.exposure < 20,
                "PID mode must reduce exposure after entering the highlight limit");
        controller.commit(decision);

        decision = controller.evaluate(mixed_sample(105, 195, 235, 0.015), 0.2);
        require(decision.apply && decision.exposure > controller.exposure(),
                "highlight protection must recover toward the lower midtone boundary");

        controller.commit(decision);
        decision = controller.evaluate(mixed_sample(20, 185, 230, 0.010), 0.2);
        require(decision.apply && decision.exposure > controller.exposure(),
                "exposure recovery must resume after highlights leave the hysteresis band");
    }

    {
        auto config = test_config();
        config.metering_window = 3;
        AdaptiveExposureController controller(config, 130, 16);
        require(controller.evaluate(sample(200, 210)).reason == "metering_warmup",
                "metering window must wait for enough post-control samples");
        require(controller.evaluate(sample(245, 255, 0.5)).reason == "metering_warmup",
                "metering window must not react to an isolated sample before it is full");
        const auto decision = controller.evaluate(sample(200, 210));
        require(!decision.apply && decision.reason == "stable",
                "median metering must reject a single flicker outlier");
    }

    {
        auto config = test_config();
        config.control_mode = "pid";
        config.target_p50_luma = 120;
        config.target_p95_luma = 210;
        config.exposure_min = 1;
        config.exposure_max = 300;
        config.gain_min = 0;
        config.underexposed_samples = 1;
        config.direction_reversal_samples = 2;
        config.pid_ki = 0.0;
        AdaptiveExposureController controller(config, 100, 0);

        auto decision = controller.evaluate(mixed_sample(60, 120, 150), 0.2);
        require(decision.apply && decision.exposure > 100,
                "initial dark response must be applied immediately");
        controller.commit(decision);

        decision = controller.evaluate(mixed_sample(150, 190, 220), 0.2);
        require(!decision.apply && decision.reason == "direction_reversal_hysteresis",
                "the first ordinary direction reversal must be held");
        decision = controller.evaluate(mixed_sample(150, 190, 220), 0.2);
        require(decision.apply && decision.exposure < controller.exposure(),
                "a confirmed direction reversal must be applied");
    }

    {
        auto config = test_config();
        config.control_mode = "pid";
        config.target_p50_luma = 120;
        config.target_p95_luma = 210;
        config.exposure_min = 1;
        config.exposure_max = 300;
        config.gain_min = 0;
        config.max_highlight_fraction = 0.25;
        config.underexposed_samples = 1;
        config.direction_reversal_samples = 2;
        config.pid_ki = 0.0;
        AdaptiveExposureController controller(config, 100, 0);

        auto decision = controller.evaluate(mixed_sample(60, 120, 150), 0.2);
        require(decision.apply, "initial dark response must be available");
        controller.commit(decision);
        decision = controller.evaluate(mixed_sample(180, 245, 255, 0.5), 0.2);
        require(decision.apply && decision.exposure < controller.exposure()
                    && decision.reason == "pid_severe_highlights_reduce_exposure",
                "confirmed large-area clipping must bypass direction reversal hysteresis");
    }

    {
        auto config = test_config();
        config.control_mode = "pid";
        config.target_p50_luma = 100;
        config.target_p95_luma = 210;
        config.exposure_min = 1;
        config.exposure_max = 300;
        config.gain_min = 0;
        config.underexposed_samples = 2;
        config.direction_reversal_samples = 3;
        config.pid_ki = 0.0;
        AdaptiveExposureController controller(config, 100, 0);

        auto decision = controller.evaluate(mixed_sample(150, 190, 220), 0.2);
        require(decision.apply && decision.exposure < 100,
                "deadlock regression setup must establish a downward adjustment");
        controller.commit(decision);

        decision = controller.evaluate(mixed_sample(50, 100, 120), 0.2);
        require(!decision.apply && decision.reason == "dark_hysteresis",
                "first dark sample must still be filtered");
        decision = controller.evaluate(mixed_sample(50, 100, 120), 0.2);
        require(!decision.apply && decision.reason == "direction_reversal_hysteresis",
                "first upward reversal request must be held");
        decision = controller.evaluate(mixed_sample(50, 100, 120), 0.2);
        require(!decision.apply && decision.reason == "direction_reversal_hysteresis",
                "second upward reversal request must remain pending");
        decision = controller.evaluate(mixed_sample(50, 100, 120), 0.2);
        require(decision.apply && decision.exposure > controller.exposure(),
                "dark hysteresis and direction hysteresis must not deadlock recovery");
    }

    std::cout << "adaptive exposure controller test passed\n";
    return 0;
}
