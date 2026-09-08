#include "gwv3_receiver/recording_tail_drain.hpp"
#include <stdexcept>

void check(bool value) { if(!value) throw std::runtime_error("tail drain assertion failed"); }

int main() {
    using namespace std::chrono;
    gwv3::RecordingTailDrain tail;
    const auto now = gwv3::RecordingTailDrain::Clock::now();
    tail.begin(1000000, true, true, now, milliseconds(100));
    tail.observe(true, 1000000, 1000000);
    check(!tail.rgb_done());
    tail.observe(true, 1000001, 1000010);
    check(tail.rgb_done() && !tail.complete());
    tail.observe(false, 2000000, 1000010);
    check(!tail.complete());
    tail.observe(false, 1000001, 1000010);
    check(tail.complete() && !tail.expired(now));
    tail.finish();
    check(!tail.active());
    tail.begin(3000000, true, false, now, milliseconds(100));
    check(!tail.complete() && tail.depth_done());
    check(tail.expired(now + milliseconds(100)));
    tail.observe(true, 3000001, 3000001);
    check(tail.complete());
}
