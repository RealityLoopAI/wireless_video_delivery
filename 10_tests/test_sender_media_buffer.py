#!/usr/bin/env python3
"""Exercise the production queue without camera SDK or media threads."""
from pathlib import Path
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[1]
    source = (root / "01_sender_linux/src/detail/runtime.inl").read_text()
    queue = source.split("class LatestMediaQueue {", 1)[1].split("struct DepthCompressionJob", 1)[0]
    # Only the surrounding media/SDK types are stubbed; queue logic is unchanged.
    harness = r'''
#include <algorithm>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <vector>
enum class StreamType {rgb, depth_raw, rgb_preview};
constexpr size_t kDepthMediaQueuePerSlot = 4;
constexpr size_t kMediaQueueMaxBytesPerSlot = 256ull * 1024 * 1024;
constexpr size_t kPreviewQueueMaxBytesPerSlot = 16ull * 1024 * 1024;
struct MediaPacketJob {
    uint64_t frame_id;
    StreamType stream_type;
    size_t bytes;
    size_t total_size() const {return bytes;}
};
'''
    harness += "class LatestMediaQueue {" + queue
    harness += r'''
int main() {
    using R = LatestMediaQueue::PublishResult;
    for (auto stream : {StreamType::rgb, StreamType::depth_raw}) {
        LatestMediaQueue q(1, 1800, 1800);
        // A synthetic 45-second pause at 30 fps exceeds the old 900-frame limit.
        for (uint64_t i = 0; i < 1350; ++i)
            assert(q.publish(0, {i, stream, 50000}) == R::queued);
        for (uint64_t i = 0; i < 1350; ++i) {
            auto item = q.wait_pop(std::chrono::milliseconds(0));
            assert(item && item->frame_id == i);
        }
        assert(q.empty());
        for (uint64_t i = 0; i < 1800; ++i)
            assert(q.publish(0, {i, stream, 50000}) == R::queued);
        assert(q.publish(0, {1800, stream, 50000}) == R::overwritten);
        assert(q.wait_pop(std::chrono::milliseconds(0))->frame_id == 1);
    }
    LatestMediaQueue bytes(1, 1800, 1800);
    for (uint64_t i = 0; i < 4; ++i)
        assert(bytes.publish(0, {i, StreamType::rgb, 64ull*1024*1024}) == R::queued);
    assert(bytes.publish(0, {4, StreamType::rgb, 64ull*1024*1024}) == R::overwritten);
    assert(bytes.wait_pop(std::chrono::milliseconds(0))->frame_id == 1);
    LatestMediaQueue preview(1, 1800, 1800);
    assert(preview.publish(0, {0, StreamType::rgb_preview, 100}) == R::queued);
    assert(preview.publish(0, {1, StreamType::rgb_preview, 100}) == R::overwritten);
    assert(preview.wait_pop(std::chrono::milliseconds(0))->frame_id == 1);
    preview.stop();
    assert(preview.publish(0, {2, StreamType::rgb_preview, 100}) == R::rejected_stopping);
}
'''
    with tempfile.TemporaryDirectory(prefix="gwv3_buffer_test_") as directory:
        cpp = Path(directory) / "queue.cpp"
        binary = Path(directory) / "queue_test"
        cpp.write_text(harness)
        subprocess.run(["c++", "-std=c++17", "-pthread", str(cpp), "-o", str(binary)], check=True, timeout=60)
        subprocess.run([str(binary)], check=True, timeout=10)
    print("PASS: 45s logical backlog, 1800-frame cap, byte cap, preview isolation, stop")


if __name__ == "__main__":
    main()
