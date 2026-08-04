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
        require(decision.apply && decision.exposure == 130 && decision.gain == 28,
                "moderate brightness must reduce gain before exposure");
        controller.commit(decision);
        require(controller.gain() == 28 && controller.adjustment_count() == 1, "committed gain adjustment not tracked");
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

    std::cout << "adaptive exposure controller test passed\n";
    return 0;
}
