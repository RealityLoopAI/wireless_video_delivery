#include "gwv3_sender/rgb_transport_recovery.hpp"

#include <iostream>

int main() {
    using Recovery = gwv3::RgbTransportRecovery;
    int failures = 0;
    const auto expect = [&](bool condition, const char *message) {
        if(!condition) {
            std::cerr << "FAILED: " << message << '\n';
            ++failures;
        }
    };

    Recovery recovery;
    expect(recovery.before_send(false) == Recovery::SendDecision::send, "normal frame should send");
    expect(recovery.arm(), "first arm should report a transition");
    expect(!recovery.arm(), "repeated arm should preserve recovery state");
    expect(recovery.waiting(), "recovery should wait for a keyframe");

    expect(recovery.keyframe_request_due(1'000, 1'000'000), "first keyframe request should be due");
    expect(!recovery.keyframe_request_due(500'000, 1'000'000), "request should be rate limited");
    expect(recovery.keyframe_request_due(1'001'000, 1'000'000), "request should be retried after interval");

    expect(recovery.before_send(false) == Recovery::SendDecision::drop, "non-keyframe should drop during recovery");
    expect(recovery.before_send(false) == Recovery::SendDecision::drop, "second non-keyframe should drop during recovery");
    expect(recovery.dropped_frames() == 2, "drop count should be retained");

    expect(recovery.before_send(true) == Recovery::SendDecision::send_recovery_keyframe,
           "keyframe should be allowed during recovery");
    expect(recovery.waiting(), "allowing a keyframe must not complete recovery");
    expect(!recovery.complete_successful_send(false), "non-keyframe success must not complete recovery");
    expect(recovery.waiting(), "recovery should still be armed before keyframe send succeeds");

    const auto dropped = recovery.complete_successful_send(true);
    expect(dropped && *dropped == 2, "successful keyframe should complete recovery with drop count");
    expect(!recovery.waiting(), "successful keyframe should disarm recovery");
    expect(recovery.dropped_frames() == 0, "completion should reset drop count");

    recovery.arm();
    expect(recovery.before_send(true) == Recovery::SendDecision::send_recovery_keyframe,
           "keyframe should be retried after a new transport loss");
    recovery.arm();
    expect(recovery.waiting(), "failed keyframe send must leave recovery armed");
    expect(recovery.complete_successful_send(true).has_value(), "later successful keyframe should recover");
    return failures == 0 ? 0 : 1;
}
