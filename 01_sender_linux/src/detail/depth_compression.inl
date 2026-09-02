bool h264_payload_has_idr(const std::vector<uint8_t> &payload) {
    for(size_t i = 0; i + 4 < payload.size(); ++i) {
        size_t nal_offset = 0;
        if(payload[i] == 0 && payload[i + 1] == 0 && payload[i + 2] == 1) {
            nal_offset = i + 3;
        }
        else if(i + 4 < payload.size() && payload[i] == 0 && payload[i + 1] == 0 && payload[i + 2] == 0 && payload[i + 3] == 1) {
            nal_offset = i + 4;
        }
        if(nal_offset > 0 && nal_offset < payload.size() && (payload[nal_offset] & 0x1fu) == 5u) {
            return true;
        }
    }
    return false;
}

bool h264_payload_has_vcl_nal(const std::vector<uint8_t> &payload) {
    for(size_t i = 0; i + 4 < payload.size(); ++i) {
        size_t nal_offset = 0;
        if(payload[i] == 0 && payload[i + 1] == 0 && payload[i + 2] == 1) {
            nal_offset = i + 3;
        }
        else if(i + 4 < payload.size() && payload[i] == 0 && payload[i + 1] == 0 && payload[i + 2] == 0 && payload[i + 3] == 1) {
            nal_offset = i + 4;
        }
        if(nal_offset > 0 && nal_offset < payload.size()) {
            const uint8_t nal_type = payload[nal_offset] & 0x1fu;
            if(nal_type >= 1 && nal_type <= 5) {
                return true;
            }
        }
    }
    return false;
}

std::vector<uint8_t> zlib_compress_payload(const void *data, size_t size) {
    const auto bound = compressBound(static_cast<uLong>(size));
    std::vector<uint8_t> out(bound);
    uLongf out_size = bound;
    const int rc = compress2(out.data(), &out_size, static_cast<const Bytef *>(data), static_cast<uLong>(size), Z_BEST_SPEED);
    if(rc != Z_OK) {
        throw std::runtime_error("zlib depth compression failed");
    }
    out.resize(static_cast<size_t>(out_size));
    return out;
}

struct Lz4CompressApi {
    using CompressBoundFn = int (*)(int);
    using CompressDefaultFn = int (*)(const char *, char *, int, int);

    void *handle = nullptr;
    CompressBoundFn compress_bound = nullptr;
    CompressDefaultFn compress_default = nullptr;
};

Lz4CompressApi &lz4_compress_api() {
    static Lz4CompressApi api;
    static std::once_flag once;
    std::call_once(once, [] {
        api.handle = dlopen("liblz4.so.1", RTLD_LAZY | RTLD_LOCAL);
        if(!api.handle) {
            throw std::runtime_error(std::string("cannot load liblz4.so.1: ") + dlerror());
        }
        api.compress_bound = reinterpret_cast<Lz4CompressApi::CompressBoundFn>(dlsym(api.handle, "LZ4_compressBound"));
        api.compress_default = reinterpret_cast<Lz4CompressApi::CompressDefaultFn>(dlsym(api.handle, "LZ4_compress_default"));
        if(!api.compress_bound || !api.compress_default) {
            throw std::runtime_error("liblz4.so.1 does not provide required compression symbols");
        }
    });
    return api;
}

std::vector<uint8_t> lz4_compress_payload(const uint8_t *data, size_t size) {
    if(data == nullptr || size == 0 || size > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("invalid lz4 compression input");
    }
    auto &api = lz4_compress_api();
    const int input_size = static_cast<int>(size);
    const int bound = api.compress_bound(input_size);
    if(bound <= 0) {
        throw std::runtime_error("lz4 compression bound failed");
    }
    std::vector<uint8_t> out(static_cast<size_t>(bound));
    const int compressed_size = api.compress_default(reinterpret_cast<const char *>(data),
                                                     reinterpret_cast<char *>(out.data()),
                                                     input_size,
                                                     bound);
    if(compressed_size <= 0) {
        throw std::runtime_error("lz4 depth compression failed");
    }
    out.resize(static_cast<size_t>(compressed_size));
    return out;
}

void quantize_depth8_into(const uint16_t *samples, size_t sample_count, uint16_t raw_step, uint8_t *out) {
    if(samples == nullptr || out == nullptr || raw_step == 0) {
        throw std::runtime_error("invalid q8 depth quantization input");
    }
    for(size_t i = 0; i < sample_count; ++i) {
        const uint16_t value = samples[i];
        if(value == 0) {
            out[i] = 0;
            continue;
        }
        const uint32_t rounded = (static_cast<uint32_t>(value) + raw_step / 2u) / raw_step;
        out[i] = static_cast<uint8_t>(std::clamp<uint32_t>(rounded, 1u, 255u));
    }
}

void append_varuint(std::vector<uint8_t> &out, uint32_t value) {
    while(value >= 0x80u) {
        append_u8(out, static_cast<uint8_t>((value & 0x7fu) | 0x80u));
        value >>= 7u;
    }
    append_u8(out, static_cast<uint8_t>(value));
}

uint32_t zigzag_encode_i32(int32_t value) {
    return static_cast<uint32_t>((value << 1) ^ (value >> 31));
}

std::vector<uint8_t> qdelta_compress_payload(const void *data, size_t size, uint16_t raw_step = 1) {
    if(data == nullptr || size == 0 || size % sizeof(uint16_t) != 0 || raw_step == 0) {
        throw std::runtime_error("invalid qdelta depth input");
    }

    const auto *samples = static_cast<const uint16_t *>(data);
    const size_t sample_count = size / sizeof(uint16_t);
    std::vector<uint8_t> out;
    out.reserve(size / 2);
    append_u8(out, 'Q');
    append_u8(out, 'D');
    append_u8(out, 'L');
    append_u8(out, '1');
    append_le16(out, raw_step);
    append_le16(out, 0);

    size_t index = 0;
    int32_t previous = 0;
    while(index < sample_count) {
        if(samples[index] == 0) {
            size_t zeros = 0;
            while(index + zeros < sample_count && samples[index + zeros] == 0) {
                ++zeros;
            }
            append_u8(out, 0);
            append_varuint(out, static_cast<uint32_t>(zeros));
            index += zeros;
            previous = 0;
            continue;
        }

        const size_t run_start = index;
        uint8_t run = 0;
        while(index < sample_count && samples[index] != 0 && run < std::numeric_limits<uint8_t>::max()) {
            ++index;
            ++run;
        }
        append_u8(out, 1);
        append_u8(out, run);
        for(size_t i = 0; i < run; ++i) {
            const uint16_t value = samples[run_start + i];
            uint32_t quantized = (static_cast<uint32_t>(value) + raw_step / 2u) / raw_step;
            quantized = std::max<uint32_t>(1u, quantized);
            if(quantized > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
                throw std::runtime_error("qdelta depth sample out of range");
            }
            const int32_t current = static_cast<int32_t>(quantized);
            append_varuint(out, zigzag_encode_i32(current - previous));
            previous = current;
        }
    }
    return out;
}

uint16_t raw_step_for_depth_scale(float depth_scale, double quantization_mm = 1.0) {
    if(!std::isfinite(depth_scale) || depth_scale <= 0.0f || quantization_mm <= 0.0) {
        return 1;
    }
    const double raw_step = std::ceil(quantization_mm / static_cast<double>(depth_scale));
    return static_cast<uint16_t>(std::clamp<double>(raw_step, 1.0, static_cast<double>(std::numeric_limits<uint16_t>::max())));
}

uint16_t quantize_depth12(uint16_t value, uint16_t raw_step) {
    if(value == 0) {
        return 0;
    }
    const uint32_t rounded = (static_cast<uint32_t>(value) + raw_step / 2u) / raw_step;
    return static_cast<uint16_t>(std::clamp<uint32_t>(rounded, 1u, 4095u));
}

size_t depth_chunk_compression_worker_count(size_t chunk_count) {
    if(chunk_count <= 1) {
        return 1;
    }
    static const int configured = [] {
        const char *value = std::getenv("GEMINI_DEPTH_CHUNK_COMPRESSION_WORKERS");
        if(value == nullptr || *value == '\0') {
            return 0;
        }
        try {
            return std::stoi(value);
        }
        catch(const std::exception &) {
            return 0;
        }
    }();
    size_t worker_limit = 0;
    if(configured > 0) {
        worker_limit = static_cast<size_t>(configured);
    }
    else {
        worker_limit = 1;
    }
    return std::clamp<size_t>(worker_limit, 1, chunk_count);
}

std::vector<uint8_t> pack_depth12_payload(const uint16_t *samples, size_t sample_count, uint16_t raw_step) {
    std::vector<uint8_t> packed;
    packed.reserve(((sample_count + 1u) / 2u) * 3u);
    for(size_t i = 0; i < sample_count; i += 2) {
        const uint16_t a = quantize_depth12(samples[i], raw_step);
        const uint16_t b = (i + 1 < sample_count) ? quantize_depth12(samples[i + 1], raw_step) : 0;
        packed.push_back(static_cast<uint8_t>(a & 0xffu));
        packed.push_back(static_cast<uint8_t>(((a >> 8u) & 0x0fu) | ((b & 0x0fu) << 4u)));
        packed.push_back(static_cast<uint8_t>((b >> 4u) & 0xffu));
    }
    return packed;
}

std::vector<uint8_t> pq12zlib_compress_payload(const void *data, size_t size, float depth_scale, double quantization_step_mm) {
    if(data == nullptr || size == 0 || size % sizeof(uint16_t) != 0) {
        throw std::runtime_error("invalid pq12zlib depth input");
    }

    constexpr size_t kSamplesPerChunk = 64 * 1024;
    constexpr uint32_t kMagic = 0x5a323150u;  // bytes: P 1 2 Z
    constexpr uint16_t kVersion = 1;
    constexpr size_t kHeaderSize = 20;
    constexpr size_t kChunkEntrySize = 12;

    const auto *samples = static_cast<const uint16_t *>(data);
    const size_t sample_count = size / sizeof(uint16_t);
    if(sample_count > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error("pq12zlib depth frame too large");
    }
    const size_t chunk_count = (sample_count + kSamplesPerChunk - 1) / kSamplesPerChunk;
    if(chunk_count == 0 || chunk_count > std::numeric_limits<uint16_t>::max()) {
        throw std::runtime_error("pq12zlib depth chunk count out of range");
    }

    const uint16_t raw_step = raw_step_for_depth_scale(depth_scale, quantization_step_mm);
    struct CompressedChunk {
        uint32_t sample_offset = 0;
        uint32_t sample_count = 0;
        std::vector<uint8_t> payload;
    };

    std::vector<CompressedChunk> chunks;
    chunks.reserve(chunk_count);
    for(size_t offset = 0; offset < sample_count; offset += kSamplesPerChunk) {
        const size_t count = std::min(kSamplesPerChunk, sample_count - offset);
        auto packed = pack_depth12_payload(samples + offset, count, raw_step);
        const auto bound = compressBound(static_cast<uLong>(packed.size()));
        CompressedChunk chunk;
        chunk.sample_offset = static_cast<uint32_t>(offset);
        chunk.sample_count = static_cast<uint32_t>(count);
        chunk.payload.resize(bound);
        uLongf out_size = bound;
        const int rc = compress2(chunk.payload.data(), &out_size, packed.data(), static_cast<uLong>(packed.size()), Z_BEST_SPEED);
        if(rc != Z_OK) {
            throw std::runtime_error("pq12zlib depth compression failed");
        }
        chunk.payload.resize(static_cast<size_t>(out_size));
        chunks.push_back(std::move(chunk));
    }

    size_t payload_size = kHeaderSize + chunks.size() * kChunkEntrySize;
    for(const auto &chunk : chunks) {
        payload_size += chunk.payload.size();
    }

    std::vector<uint8_t> out;
    out.reserve(payload_size);
    append_le32(out, kMagic);
    append_le16(out, kVersion);
    append_le16(out, raw_step);
    append_le32(out, static_cast<uint32_t>(sample_count));
    append_le16(out, static_cast<uint16_t>(chunks.size()));
    for(size_t i = out.size(); i < kHeaderSize; ++i) {
        append_u8(out, 0);
    }
    for(const auto &chunk : chunks) {
        append_le32(out, chunk.sample_offset);
        append_le32(out, chunk.sample_count);
        append_le32(out, static_cast<uint32_t>(chunk.payload.size()));
    }
    for(const auto &chunk : chunks) {
        append_bytes(out, chunk.payload.data(), chunk.payload.size());
    }
    return out;
}

std::vector<uint8_t> q8lz4_compress_payload(const void *data, size_t size, uint16_t raw_step = 1) {
    if(data == nullptr || size == 0 || size % sizeof(uint16_t) != 0 || raw_step == 0) {
        throw std::runtime_error("invalid q8lz4 depth input");
    }

    constexpr uint32_t kMagic = 0x314c3851u;  // bytes: Q 8 L 1
    constexpr uint16_t kVersion = 1;

    const auto *samples = static_cast<const uint16_t *>(data);
    const size_t sample_count = size / sizeof(uint16_t);
    if(sample_count > static_cast<size_t>(std::numeric_limits<uint32_t>::max())
       || sample_count > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("q8lz4 depth frame too large");
    }

    std::vector<uint8_t> quantized(sample_count);
    quantize_depth8_into(samples, sample_count, raw_step, quantized.data());
    auto compressed = lz4_compress_payload(quantized.data(), quantized.size());
    if(compressed.size() > static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
        throw std::runtime_error("q8lz4 depth payload too large");
    }

    std::vector<uint8_t> out;
    out.reserve(16 + compressed.size());
    append_le32(out, kMagic);
    append_le16(out, kVersion);
    append_le16(out, raw_step);
    append_le32(out, static_cast<uint32_t>(sample_count));
    append_le32(out, static_cast<uint32_t>(compressed.size()));
    append_bytes(out, compressed.data(), compressed.size());
    return out;
}

std::vector<uint8_t> pq8zlib_compress_payload(const void *data, size_t size, uint16_t raw_step = 1) {
    if(data == nullptr || size == 0 || size % sizeof(uint16_t) != 0 || raw_step == 0) {
        throw std::runtime_error("invalid pq8zlib depth input");
    }

    constexpr size_t kSamplesPerChunk = 64 * 1024;
    constexpr uint32_t kMagic = 0x5a385150u;  // bytes: P Q 8 Z
    constexpr uint16_t kVersion = 1;
    constexpr size_t kHeaderSize = 20;
    constexpr size_t kChunkEntrySize = 12;

    const auto *samples = static_cast<const uint16_t *>(data);
    const size_t sample_count = size / sizeof(uint16_t);
    if(sample_count > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error("pq8zlib depth frame too large");
    }
    const size_t chunk_count = (sample_count + kSamplesPerChunk - 1) / kSamplesPerChunk;
    if(chunk_count == 0 || chunk_count > std::numeric_limits<uint16_t>::max()) {
        throw std::runtime_error("pq8zlib depth chunk count out of range");
    }

    struct CompressedChunk {
        uint32_t sample_offset = 0;
        uint32_t sample_count = 0;
        std::vector<uint8_t> payload;
    };

    std::vector<CompressedChunk> chunks(chunk_count);
    for(size_t chunk_index = 0; chunk_index < chunk_count; ++chunk_index) {
        const size_t offset = chunk_index * kSamplesPerChunk;
        const size_t count = std::min(kSamplesPerChunk, sample_count - offset);
        chunks[chunk_index].sample_offset = static_cast<uint32_t>(offset);
        chunks[chunk_index].sample_count = static_cast<uint32_t>(count);
    }

    std::atomic<size_t> next_chunk{0};
    auto compress_chunk_worker = [&] {
        while(true) {
            const size_t chunk_index = next_chunk.fetch_add(1);
            if(chunk_index >= chunks.size()) {
                break;
            }
            auto &chunk = chunks[chunk_index];
            const size_t offset = chunk.sample_offset;
            const size_t count = chunk.sample_count;
            std::vector<uint8_t> quantized(count);
            quantize_depth8_into(samples + offset, count, raw_step, quantized.data());

            const auto bound = compressBound(static_cast<uLong>(quantized.size()));
            chunk.payload.resize(bound);
            uLongf out_size = bound;
            const int rc = compress2(chunk.payload.data(), &out_size, quantized.data(), static_cast<uLong>(quantized.size()), Z_BEST_SPEED);
            if(rc != Z_OK) {
                throw std::runtime_error("pq8zlib depth compression failed");
            }
            chunk.payload.resize(static_cast<size_t>(out_size));
        }
    };
    const size_t worker_count = depth_chunk_compression_worker_count(chunks.size());
    std::vector<std::future<void>> futures;
    futures.reserve(worker_count > 0 ? worker_count - 1 : 0);
    for(size_t worker = 1; worker < worker_count; ++worker) {
        futures.emplace_back(std::async(std::launch::async, compress_chunk_worker));
    }
    compress_chunk_worker();
    for(auto &future : futures) {
        future.get();
    }

    size_t payload_size = kHeaderSize + chunks.size() * kChunkEntrySize;
    for(const auto &chunk : chunks) {
        payload_size += chunk.payload.size();
    }

    std::vector<uint8_t> out;
    out.reserve(payload_size);
    append_le32(out, kMagic);
    append_le16(out, kVersion);
    append_le16(out, raw_step);
    append_le32(out, static_cast<uint32_t>(sample_count));
    append_le16(out, static_cast<uint16_t>(chunks.size()));
    for(size_t i = out.size(); i < kHeaderSize; ++i) {
        append_u8(out, 0);
    }
    for(const auto &chunk : chunks) {
        append_le32(out, chunk.sample_offset);
        append_le32(out, chunk.sample_count);
        append_le32(out, static_cast<uint32_t>(chunk.payload.size()));
    }
    for(const auto &chunk : chunks) {
        append_bytes(out, chunk.payload.data(), chunk.payload.size());
    }
    return out;
}

std::vector<uint8_t> pq8lz4_compress_payload(const void *data, size_t size, uint16_t raw_step = 1) {
    if(data == nullptr || size == 0 || size % sizeof(uint16_t) != 0 || raw_step == 0) {
        throw std::runtime_error("invalid pq8lz4 depth input");
    }

    constexpr size_t kSamplesPerChunk = 64 * 1024;
    constexpr uint32_t kMagic = 0x4c385150u;  // bytes: P Q 8 L
    constexpr uint16_t kVersion = 1;
    constexpr size_t kHeaderSize = 20;
    constexpr size_t kChunkEntrySize = 12;

    const auto *samples = static_cast<const uint16_t *>(data);
    const size_t sample_count = size / sizeof(uint16_t);
    if(sample_count > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error("pq8lz4 depth frame too large");
    }
    const size_t chunk_count = (sample_count + kSamplesPerChunk - 1) / kSamplesPerChunk;
    if(chunk_count == 0 || chunk_count > std::numeric_limits<uint16_t>::max()) {
        throw std::runtime_error("pq8lz4 depth chunk count out of range");
    }

    struct CompressedChunk {
        uint32_t sample_offset = 0;
        uint32_t sample_count = 0;
        std::vector<uint8_t> payload;
    };

    std::vector<CompressedChunk> chunks(chunk_count);
    for(size_t chunk_index = 0; chunk_index < chunk_count; ++chunk_index) {
        const size_t offset = chunk_index * kSamplesPerChunk;
        const size_t count = std::min(kSamplesPerChunk, sample_count - offset);
        chunks[chunk_index].sample_offset = static_cast<uint32_t>(offset);
        chunks[chunk_index].sample_count = static_cast<uint32_t>(count);
    }

    std::atomic<size_t> next_chunk{0};
    auto compress_chunk_worker = [&] {
        while(true) {
            const size_t chunk_index = next_chunk.fetch_add(1);
            if(chunk_index >= chunks.size()) {
                break;
            }
            auto &chunk = chunks[chunk_index];
            const size_t offset = chunk.sample_offset;
            const size_t count = chunk.sample_count;
            std::vector<uint8_t> quantized(count);
            quantize_depth8_into(samples + offset, count, raw_step, quantized.data());

            chunk.payload = lz4_compress_payload(quantized.data(), quantized.size());
            if(chunk.payload.size() > static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
                throw std::runtime_error("pq8lz4 depth chunk too large");
            }
        }
    };
    const size_t worker_count = depth_chunk_compression_worker_count(chunks.size());
    std::vector<std::future<void>> futures;
    futures.reserve(worker_count > 0 ? worker_count - 1 : 0);
    for(size_t worker = 1; worker < worker_count; ++worker) {
        futures.emplace_back(std::async(std::launch::async, compress_chunk_worker));
    }
    compress_chunk_worker();
    for(auto &future : futures) {
        future.get();
    }

    size_t payload_size = kHeaderSize + chunks.size() * kChunkEntrySize;
    for(const auto &chunk : chunks) {
        payload_size += chunk.payload.size();
    }

    std::vector<uint8_t> out;
    out.reserve(payload_size);
    append_le32(out, kMagic);
    append_le16(out, kVersion);
    append_le16(out, raw_step);
    append_le32(out, static_cast<uint32_t>(sample_count));
    append_le16(out, static_cast<uint16_t>(chunks.size()));
    for(size_t i = out.size(); i < kHeaderSize; ++i) {
        append_u8(out, 0);
    }
    for(const auto &chunk : chunks) {
        append_le32(out, chunk.sample_offset);
        append_le32(out, chunk.sample_count);
        append_le32(out, static_cast<uint32_t>(chunk.payload.size()));
    }
    for(const auto &chunk : chunks) {
        append_bytes(out, chunk.payload.data(), chunk.payload.size());
    }
    return out;
}

std::vector<uint8_t> compress_depth_payload(const std::string &compression,
                                            const uint8_t *raw_payload,
                                            size_t raw_payload_size,
                                            float depth_scale,
                                            double quantization_step_mm) {
    if(raw_payload == nullptr || raw_payload_size == 0) {
        throw std::runtime_error("invalid depth compression input");
    }
    if(compression == "zlib") {
        return zlib_compress_payload(raw_payload, raw_payload_size);
    }
    if(compression == "qdelta") {
        return qdelta_compress_payload(raw_payload, raw_payload_size, 1);
    }
    if(compression == "pq12zlib") {
        return pq12zlib_compress_payload(raw_payload, raw_payload_size, depth_scale, quantization_step_mm);
    }
    if(compression == "q8lz4") {
        return q8lz4_compress_payload(raw_payload, raw_payload_size, raw_step_for_depth_scale(depth_scale, quantization_step_mm));
    }
    if(compression == "pq8zlib") {
        return pq8zlib_compress_payload(raw_payload, raw_payload_size, raw_step_for_depth_scale(depth_scale, quantization_step_mm));
    }
    if(compression == "pq8lz4") {
        return pq8lz4_compress_payload(raw_payload, raw_payload_size, raw_step_for_depth_scale(depth_scale, quantization_step_mm));
    }
    throw std::runtime_error("unsupported depth compression: " + compression);
}

bool depth_transport_uses_compression(const std::string &compression) {
    return compression != "none";
}

