#include "gwv3_sender/scheduled_keyframe.hpp"

#include <cassert>
#include <cstdint>
#include <limits>

int main() {
    using gwv3::scheduled_keyframe_due;
    using gwv3::sender_system_time_from_global;
    using gwv3::merge_keyframe_target;

    assert(sender_system_time_from_global(1'000'000, 2'000) == 998'000);
    assert(sender_system_time_from_global(1'000'000, -2'000) == 1'002'000);
    assert(sender_system_time_from_global(100, 200) == 1);
    assert(sender_system_time_from_global(std::numeric_limits<uint64_t>::max() - 5, -10)
           == std::numeric_limits<uint64_t>::max());

    assert(scheduled_keyframe_due(0, 1, 1));
    assert(!scheduled_keyframe_due(1'000, 999, 1'099, 100));
    assert(scheduled_keyframe_due(1'000, 1'000, 900));
    assert(scheduled_keyframe_due(1'000, 999, 1'100, 100));
    assert(merge_keyframe_target(2'000, 0, true) == 2'000);
    assert(merge_keyframe_target(2'000, 0, false) == 0);
    assert(merge_keyframe_target(2'000, 2'500, false) == 2'000);
    assert(merge_keyframe_target(2'000, 1'500, false) == 1'500);
    assert(merge_keyframe_target(0, 2'000, false) == 0);
    return 0;
}
