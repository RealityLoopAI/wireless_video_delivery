#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace gwv3 {

// One instance per recording writer. NUT carries capture PTS through the
// existing ffmpeg pipe without decoding or changing the network protocol.
class TimestampedH264Writer {
public:
    using Sink = std::function<bool(const uint8_t *, size_t)>;
    TimestampedH264Writer(uint32_t width, uint32_t height, double fps, Sink sink);
    ~TimestampedH264Writer();
    TimestampedH264Writer(const TimestampedH264Writer &) = delete;
    TimestampedH264Writer &operator=(const TimestampedH264Writer &) = delete;

    void write(const uint8_t *data, size_t size, int64_t pts_us, bool keyframe);
    bool close() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace gwv3
