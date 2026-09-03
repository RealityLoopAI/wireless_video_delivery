#include "gwv3_sender/receiver_target.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

namespace {

void expect(bool condition, const std::string &message) {
    if(!condition) {
        std::cerr << message << std::endl;
        std::exit(1);
    }
}

}  // namespace

int main() {
    auto target = std::make_shared<gwv3::ReceiverTarget>("192.168.66.196");
    const auto initial = target->snapshot();
    expect(initial.host == "192.168.66.196", "fallback host was not installed");
    expect(!initial.discovered, "fallback must not be marked discovered");

    expect(target->restore_persisted("192.168.1.196", "receiver-old"), "persisted target was not restored");
    const auto restored = target->snapshot();
    expect(restored.discovered && restored.receiver_id == "receiver-old", "persisted target metadata is wrong");
    target->mark_success(restored.generation);
    expect(target->success_recent(1000), "successful target was not marked recent");

    expect(target->update_discovered("192.168.1.210", "receiver-new"), "discovered target was not updated");
    const auto updated = target->snapshot();
    expect(updated.generation > restored.generation, "target generation did not advance");
    expect(!target->success_recent(1000), "new target inherited old success state");

    expect(target->use_fallback(), "fallback transition did not occur");
    const auto fallback = target->snapshot();
    expect(fallback.host == "192.168.66.196" && !fallback.discovered, "fallback transition is wrong");
    std::cout << "receiver target unit test passed" << std::endl;
    return 0;
}
