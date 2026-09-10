#include "gwv3_sender/rgb_input_audit.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>

void require(bool value) {
    if(!value) {
        std::cerr << "RGB input audit check failed\n";
        std::exit(1);
    }
}

int main() {
    using namespace gwv3::audit;
    require(!inspect_jpeg(nullptr, 100).accepted_by_sender);
    const std::array<uint8_t, 8> padded{0xff, 0xd8, 1, 2, 0xff, 0xd9, 0, 0};
    const auto valid = inspect_jpeg(padded.data(), padded.size());
    require(valid.accepted_by_sender && valid.trimmed_size == 6 && valid.last_eoi_end == 6);
    auto trailer = padded;
    trailer[7] = 12;
    const auto rejected = inspect_jpeg(trailer.data(), trailer.size());
    require(!rejected.accepted_by_sender && rejected.soi && rejected.first_eoi_end == 6);
    require(!inspect_jpeg(padded.data(), 5).accepted_by_sender);
    require(inspect_jpeg(padded.data(), 5).last_eoi_end == 0);
    auto no_soi = padded;
    no_soi[0] = 0;
    require(!inspect_jpeg(no_soi.data(), no_soi.size()).accepted_by_sender);
    const std::array<uint8_t, 4> zeros{};
    require(inspect_jpeg(zeros.data(), zeros.size()).trimmed_size == 0);
    std::array<uint8_t, 1024> truncated_padded{};
    truncated_padded[0] = 0xff;
    truncated_padded[1] = 0xd8;
    truncated_padded[20] = 0x42;
    const auto long_padding = inspect_jpeg(truncated_padded.data(), truncated_padded.size());
    require(long_padding.soi && !long_padding.accepted_by_sender && long_padding.last_eoi_end == 0);

    FrameSequence sequence;
    require(sequence.observe(1396) == 0);
    require(sequence.observe(1398) == 1);
    require(sequence.observe(1398) == 0);
    require(sequence.observe(0) == 0);
    require(sequence.observe(1) == 0);
    require(sequence.missing == 1 && sequence.duplicates == 1 && sequence.resets == 1);
    FrameSequence wrap;
    wrap.observe(std::numeric_limits<uint64_t>::max());
    wrap.observe(0);
    require(wrap.missing == 0 && wrap.resets == 1);
    std::cout << "RGB input audit tests passed\n";
}
