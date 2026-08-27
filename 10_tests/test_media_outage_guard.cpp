#include "gwv3_sender/media_outage_guard.hpp"

#include <iostream>

int main() {
    int failures = 0;
    const auto expect = [&](bool condition, const char *message) {
        if(!condition) {
            std::cerr << "FAILED: " << message << '\n';
            ++failures;
        }
    };

    uint32_t samples = 17;
    expect(!gwv3::media_outage_restart_due(samples, 0, true), "disabled guard must not restart");
    expect(samples == 0, "disabled guard must clear stale samples");

    expect(!gwv3::media_outage_restart_due(samples, 3, true), "first outage sample must not restart");
    expect(!gwv3::media_outage_restart_due(samples, 3, true), "second outage sample must not restart");
    expect(gwv3::media_outage_restart_due(samples, 3, true), "threshold outage sample must restart");
    expect(samples == 3, "counter must saturate at threshold");
    expect(gwv3::media_outage_restart_due(samples, 3, true), "continued outage remains due");
    expect(samples == 3, "continued outage must not overflow the counter");

    expect(!gwv3::media_outage_restart_due(samples, 3, false), "healthy sample must clear restart state");
    expect(samples == 0, "healthy sample must reset the counter");
    return failures == 0 ? 0 : 1;
}
