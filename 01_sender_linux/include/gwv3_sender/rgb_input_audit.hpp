#pragma once

#include <cstddef>
#include <cstdint>

namespace gwv3::audit {

struct JpegEnvelope {
    bool soi = false;
    bool accepted_by_sender = false;
    size_t trimmed_size = 0;
    size_t first_eoi_end = 0;
    size_t last_eoi_end = 0;
};

// Diagnostic only: an EOI byte pattern inside a payload is not proof of a valid JPEG.
inline JpegEnvelope inspect_jpeg(const void *payload, size_t size) {
    JpegEnvelope result;
    if(!payload || size < 4) {
        return result;
    }
    const auto *data = static_cast<const uint8_t *>(payload);
    result.soi = data[0] == 0xff && data[1] == 0xd8;
    result.trimmed_size = size;
    while(result.trimmed_size && data[result.trimmed_size - 1] == 0) {
        --result.trimmed_size;
    }
    result.accepted_by_sender = result.soi && result.trimmed_size >= 4
                                && data[result.trimmed_size - 2] == 0xff && data[result.trimmed_size - 1] == 0xd9;
    for(size_t i = 1; i < size; ++i) {
        if(data[i - 1] == 0xff && data[i] == 0xd9) {
            if(!result.first_eoi_end) {
                result.first_eoi_end = i + 1;
            }
            result.last_eoi_end = i + 1;
        }
    }
    return result;
}

struct FrameSequence {
    bool seen = false;
    uint64_t last = 0;
    uint64_t missing = 0;
    uint64_t duplicates = 0;
    uint64_t resets = 0;

    uint64_t observe(uint64_t id) {
        uint64_t gap = 0;
        if(seen) {
            if(id > last) {
                gap = id - last - 1;
                missing += gap;
            }
            else if(id == last) {
                ++duplicates;
            }
            else {
                ++resets;
            }
        }
        last = id;
        seen = true;
        return gap;
    }
};

}  // namespace gwv3::audit
