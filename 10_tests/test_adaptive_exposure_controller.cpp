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
        const auto decision = controller.evaluate(mixed_sample(75, 210, 230));
        require(!decision.apply && decision.reason == "highlight_limited",
                "P99 recovery hysteresis must block immediate re-amplification near the soft limit");
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

    std::cout << "adaptive exposure controller test passed\n";
    return 0;
}
