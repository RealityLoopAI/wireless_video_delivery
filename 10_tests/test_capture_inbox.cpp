#include "gwv3_sender/capture_inbox.hpp"
#include <cassert>
#include <memory>
#include <thread>

int main() {
    gwv3::CaptureInbox<int, 2> starting(false);
    starting.push(7);
    assert(!starting.pop());
    starting.start();
    starting.push(8);
    assert(*starting.pop() == 8);
    gwv3::CaptureInbox<int, 2> queue;
    assert(!queue.pop());
    queue.push(1);
    queue.push(2);
    assert(*queue.pop() == 1);
    assert(*queue.pop() == 2);
    queue.push(3);
    queue.push(4);
    queue.push(5);
    bool failed = false;
    try { queue.pop(); } catch(const std::runtime_error &) { failed = true; }
    assert(failed);

    gwv3::CaptureInbox<int, 10000> concurrent;
    std::thread producer([&] { for(int i=0;i<10000;++i) concurrent.push(i); });
    for(int i=0;i<10000;) {
        auto item = concurrent.pop();
        if(item) { assert(*item == i); ++i; }
        else std::this_thread::yield();
    }
    producer.join();
    std::weak_ptr<int> weak;
    {
        gwv3::CaptureInbox<std::shared_ptr<int>, 2> owners;
        auto value = std::make_shared<int>(7);
        weak = value;
        owners.push(value);
        value.reset();
        assert(!weak.expired());
        auto held = owners.pop();
        assert(!weak.expired());
        held.reset();
        assert(weak.expired());
    }
}
