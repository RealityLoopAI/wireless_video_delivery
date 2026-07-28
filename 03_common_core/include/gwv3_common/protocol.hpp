#pragma once

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace gwv3 {

constexpr const char *kProtocolVersion = "3.0";
constexpr uint16_t kMediaHeaderVersion = 2;
constexpr uint16_t kMediaHeaderV1Size = 94;
constexpr uint16_t kMediaHeaderV2Size = 134;
constexpr uint32_t kMediaMagic = 0x33565747;  // bytes: G W V 3
constexpr uint32_t kPreviewUdpMagic = 0x31505547;  // bytes: G U P 1
constexpr uint16_t kPreviewUdpHeaderVersion = 1;
constexpr uint16_t kPreviewUdpHeaderSize = 32;

enum class StreamType : uint8_t {
    rgb = 1,
    depth_raw = 2,
    rgb_preview = 3,
    rgb_snapshot = 4,
};

constexpr const char *kRgbSnapshotCodecPrefix = "mjpeg;request_id=";

enum class PixelFormat : uint16_t {
    encoded_video = 1,
    depth_u16 = 2,
};

enum MediaFlags : uint32_t {
    key_frame = 1u << 0,
    dropped_before = 1u << 1,
    end_of_segment_hint = 1u << 2,
    has_system_timestamp = 1u << 3,
    has_rgb_diagnostics = 1u << 4,
    has_pipeline_diagnostics = 1u << 5,
};

struct MediaFrameMeta {
    StreamType stream_type;
    uint32_t flags = 0;
    std::string sender_id;
    std::string camera_id;
    std::string codec_or_compression;
    uint64_t frame_id = 0;
    uint64_t timestamp_us = 0;
    uint64_t system_timestamp_us = 0;
    uint64_t pair_id = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    PixelFormat pixel_format;
    uint64_t payload_size = 0;
    uint64_t uncompressed_size = 0;
    int32_t rgb_exposure_us = -1;
    int32_t rgb_gain = -1;
    int32_t rgb_auto_exposure = -1;
    int32_t rgb_actual_fps = -1;
    uint64_t sender_capture_host_timestamp_us = 0;
    uint64_t sender_timing_bound_timestamp_us = 0;
    uint64_t sender_encode_start_timestamp_us = 0;
    uint64_t sender_encode_done_timestamp_us = 0;
    uint64_t sender_packet_queued_timestamp_us = 0;
};

inline void append_u8(std::vector<uint8_t> &out, uint8_t value) {
    out.push_back(value);
}

inline void append_le16(std::vector<uint8_t> &out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xffu));
    out.push_back(static_cast<uint8_t>((value >> 8u) & 0xffu));
}

inline void append_le32(std::vector<uint8_t> &out, uint32_t value) {
    for(int i = 0; i < 4; ++i) {
        out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xffu));
    }
}

inline void append_le64(std::vector<uint8_t> &out, uint64_t value) {
    for(int i = 0; i < 8; ++i) {
        out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xffu));
    }
}

inline void append_bytes(std::vector<uint8_t> &out, const void *data, size_t size) {
    const auto *begin = static_cast<const uint8_t *>(data);
    out.insert(out.end(), begin, begin + size);
}

inline std::vector<uint8_t> build_media_header(const MediaFrameMeta &meta) {
    if(meta.sender_id.size() > UINT16_MAX || meta.camera_id.size() > UINT16_MAX || meta.codec_or_compression.size() > UINT16_MAX) {
        throw std::runtime_error("media packet string field is too long");
    }

    constexpr uint16_t fixed_header_size = kMediaHeaderV2Size;
    std::vector<uint8_t> out;
    out.reserve(fixed_header_size + meta.sender_id.size() + meta.camera_id.size() + meta.codec_or_compression.size());

    append_le32(out, kMediaMagic);
    append_le16(out, kMediaHeaderVersion);
    append_le16(out, fixed_header_size);
    append_u8(out, static_cast<uint8_t>(meta.stream_type));
    append_u8(out, 0);  // reserved for fixed-header alignment/versioning
    append_le32(out, meta.flags);
    append_le16(out, static_cast<uint16_t>(meta.sender_id.size()));
    append_le16(out, static_cast<uint16_t>(meta.camera_id.size()));
    append_le16(out, static_cast<uint16_t>(meta.codec_or_compression.size()));
    append_le64(out, meta.frame_id);
    append_le64(out, meta.timestamp_us);
    append_le64(out, meta.system_timestamp_us);
    append_le64(out, meta.pair_id);
    append_le32(out, meta.width);
    append_le32(out, meta.height);
    append_le16(out, static_cast<uint16_t>(meta.pixel_format));
    append_le64(out, meta.payload_size);
    append_le64(out, meta.uncompressed_size);
    append_le32(out, static_cast<uint32_t>(meta.rgb_exposure_us));
    append_le32(out, static_cast<uint32_t>(meta.rgb_gain));
    append_le32(out, static_cast<uint32_t>(meta.rgb_auto_exposure));
    append_le32(out, static_cast<uint32_t>(meta.rgb_actual_fps));
    append_le64(out, meta.sender_capture_host_timestamp_us);
    append_le64(out, meta.sender_timing_bound_timestamp_us);
    append_le64(out, meta.sender_encode_start_timestamp_us);
    append_le64(out, meta.sender_encode_done_timestamp_us);
    append_le64(out, meta.sender_packet_queued_timestamp_us);

    append_bytes(out, meta.sender_id.data(), meta.sender_id.size());
    append_bytes(out, meta.camera_id.data(), meta.camera_id.size());
    append_bytes(out, meta.codec_or_compression.data(), meta.codec_or_compression.size());
    return out;
}

inline std::vector<uint8_t> build_media_packet(const MediaFrameMeta &meta, const void *payload) {
    if(meta.payload_size > 0 && payload == nullptr) {
        throw std::runtime_error("media packet payload is null");
    }

    auto out = build_media_header(meta);
    out.reserve(out.size() + static_cast<size_t>(meta.payload_size));
    append_bytes(out, payload, static_cast<size_t>(meta.payload_size));
    return out;
}

}  // namespace gwv3
