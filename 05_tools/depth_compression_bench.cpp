#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <zlib.h>

namespace {

struct Options {
    int width = 1280;
    int height = 800;
    int fps = 30;
    int frames = 60;
    double quantization_mm = 10.0;
    float depth_scale = 1.0f;
    std::string input_path;
    std::vector<std::string> codecs{"pq8zlib", "pq8lz4", "q8lz4", "zlib"};
};

void usage(const char *argv0) {
    std::cerr << "usage: " << argv0
              << " [--width N] [--height N] [--fps N] [--frames N]"
                 " [--quant-mm N] [--depth-scale N] [--input raw_y16.bin]"
                 " [--codecs pq8zlib,pq8lz4,q8lz4,zlib,none]\n";
}

std::vector<std::string> split_csv(const std::string &value) {
    std::vector<std::string> out;
    std::stringstream ss(value);
    std::string item;
    while(std::getline(ss, item, ',')) {
        if(!item.empty()) {
            out.push_back(item);
        }
    }
    return out;
}

Options parse_args(int argc, char **argv) {
    Options options;
    for(int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto require_value = [&](const char *name) -> const char * {
            if(i + 1 >= argc) {
                throw std::runtime_error(std::string("missing value for ") + name);
            }
            return argv[++i];
        };
        if(arg == "--width") {
            options.width = std::stoi(require_value("--width"));
        }
        else if(arg == "--height") {
            options.height = std::stoi(require_value("--height"));
        }
        else if(arg == "--fps") {
            options.fps = std::stoi(require_value("--fps"));
        }
        else if(arg == "--frames") {
            options.frames = std::stoi(require_value("--frames"));
        }
        else if(arg == "--quant-mm") {
            options.quantization_mm = std::stod(require_value("--quant-mm"));
        }
        else if(arg == "--depth-scale") {
            options.depth_scale = std::stof(require_value("--depth-scale"));
        }
        else if(arg == "--input") {
            options.input_path = require_value("--input");
        }
        else if(arg == "--codecs") {
            options.codecs = split_csv(require_value("--codecs"));
        }
        else if(arg == "--help" || arg == "-h") {
            usage(argv[0]);
            std::exit(0);
        }
        else {
            throw std::runtime_error("unknown argument: " + arg);
        }
    }
    if(options.width <= 0 || options.height <= 0 || options.fps <= 0 || options.frames <= 0) {
        throw std::runtime_error("width/height/fps/frames must be positive");
    }
    if(options.quantization_mm <= 0.0 || options.depth_scale <= 0.0f) {
        throw std::runtime_error("quant-mm and depth-scale must be positive");
    }
    if(options.codecs.empty()) {
        throw std::runtime_error("at least one codec is required");
    }
    return options;
}

void append_u8(std::vector<uint8_t> &out, uint8_t value) {
    out.push_back(value);
}

void append_le16(std::vector<uint8_t> &out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xffu));
    out.push_back(static_cast<uint8_t>((value >> 8u) & 0xffu));
}

void append_le32(std::vector<uint8_t> &out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xffu));
    out.push_back(static_cast<uint8_t>((value >> 8u) & 0xffu));
    out.push_back(static_cast<uint8_t>((value >> 16u) & 0xffu));
    out.push_back(static_cast<uint8_t>((value >> 24u) & 0xffu));
}

void append_bytes(std::vector<uint8_t> &out, const uint8_t *data, size_t size) {
    out.insert(out.end(), data, data + size);
}

uint16_t raw_step_for_depth_scale(float depth_scale, double quantization_mm) {
    if(!std::isfinite(depth_scale) || depth_scale <= 0.0f || quantization_mm <= 0.0) {
        return 1;
    }
    const double raw_step = std::ceil(quantization_mm / static_cast<double>(depth_scale));
    return static_cast<uint16_t>(std::clamp<double>(raw_step, 1.0, static_cast<double>(std::numeric_limits<uint16_t>::max())));
}

std::vector<uint8_t> zlib_compress_payload(const uint8_t *data, size_t size) {
    const auto bound = compressBound(static_cast<uLong>(size));
    std::vector<uint8_t> out(bound);
    uLongf out_size = bound;
    const int rc = compress2(out.data(), &out_size, data, static_cast<uLong>(size), Z_BEST_SPEED);
    if(rc != Z_OK) {
        throw std::runtime_error("zlib compression failed");
    }
    out.resize(static_cast<size_t>(out_size));
    return out;
}

struct Lz4Api {
    using CompressBoundFn = int (*)(int);
    using CompressDefaultFn = int (*)(const char *, char *, int, int);

    void *handle = nullptr;
    CompressBoundFn compress_bound = nullptr;
    CompressDefaultFn compress_default = nullptr;
};

Lz4Api &lz4_api() {
    static Lz4Api api;
    static bool initialized = false;
    if(!initialized) {
        api.handle = dlopen("liblz4.so.1", RTLD_LAZY | RTLD_LOCAL);
        if(!api.handle) {
            throw std::runtime_error(std::string("cannot load liblz4.so.1: ") + dlerror());
        }
        api.compress_bound = reinterpret_cast<Lz4Api::CompressBoundFn>(dlsym(api.handle, "LZ4_compressBound"));
        api.compress_default = reinterpret_cast<Lz4Api::CompressDefaultFn>(dlsym(api.handle, "LZ4_compress_default"));
        if(!api.compress_bound || !api.compress_default) {
            throw std::runtime_error("liblz4.so.1 missing compression symbols");
        }
        initialized = true;
    }
    return api;
}

std::vector<uint8_t> lz4_compress_payload(const uint8_t *data, size_t size) {
    if(size == 0 || size > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("invalid lz4 input size");
    }
    auto &api = lz4_api();
    const int input_size = static_cast<int>(size);
    const int bound = api.compress_bound(input_size);
    if(bound <= 0) {
        throw std::runtime_error("LZ4_compressBound failed");
    }
    std::vector<uint8_t> out(static_cast<size_t>(bound));
    const int compressed = api.compress_default(reinterpret_cast<const char *>(data),
                                                reinterpret_cast<char *>(out.data()),
                                                input_size,
                                                bound);
    if(compressed <= 0) {
        throw std::runtime_error("LZ4_compress_default failed");
    }
    out.resize(static_cast<size_t>(compressed));
    return out;
}

void quantize_depth8_into(const uint16_t *samples, size_t sample_count, uint16_t raw_step, uint8_t *out) {
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

size_t depth_chunk_compression_worker_count(size_t chunk_count) {
    if(chunk_count <= 1) {
        return 1;
    }
    size_t worker_limit = 1;
    if(const char *value = std::getenv("GEMINI_DEPTH_CHUNK_COMPRESSION_WORKERS")) {
        try {
            const int configured = std::stoi(value);
            if(configured > 0) {
                worker_limit = static_cast<size_t>(configured);
            }
        }
        catch(const std::exception &) {
            worker_limit = 1;
        }
    }
    return std::clamp<size_t>(worker_limit, 1, chunk_count);
}

std::vector<uint8_t> q8lz4_compress_payload(const uint8_t *data, size_t size, uint16_t raw_step) {
    if(size % sizeof(uint16_t) != 0 || raw_step == 0) {
        throw std::runtime_error("invalid q8lz4 input");
    }
    constexpr uint32_t kMagic = 0x314c3851u;
    constexpr uint16_t kVersion = 1;
    const auto *samples = reinterpret_cast<const uint16_t *>(data);
    const size_t sample_count = size / sizeof(uint16_t);
    std::vector<uint8_t> quantized(sample_count);
    quantize_depth8_into(samples, sample_count, raw_step, quantized.data());
    auto compressed = lz4_compress_payload(quantized.data(), quantized.size());

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

template <typename CompressFn>
std::vector<uint8_t> pq8_chunked_compress_payload(const uint8_t *data,
                                                  size_t size,
                                                  uint16_t raw_step,
                                                  uint32_t magic,
                                                  CompressFn compress_fn) {
    if(size % sizeof(uint16_t) != 0 || raw_step == 0) {
        throw std::runtime_error("invalid pq8 input");
    }
    constexpr size_t kSamplesPerChunk = 64 * 1024;
    constexpr uint16_t kVersion = 1;
    constexpr size_t kHeaderSize = 20;
    constexpr size_t kChunkEntrySize = 12;
    const auto *samples = reinterpret_cast<const uint16_t *>(data);
    const size_t sample_count = size / sizeof(uint16_t);
    const size_t chunk_count = (sample_count + kSamplesPerChunk - 1) / kSamplesPerChunk;
    if(sample_count > std::numeric_limits<uint32_t>::max() || chunk_count == 0 || chunk_count > std::numeric_limits<uint16_t>::max()) {
        throw std::runtime_error("pq8 frame too large");
    }

    struct Chunk {
        uint32_t sample_offset = 0;
        uint32_t sample_count = 0;
        std::vector<uint8_t> payload;
    };

    std::vector<Chunk> chunks(chunk_count);
    for(size_t chunk_index = 0; chunk_index < chunk_count; ++chunk_index) {
        const size_t offset = chunk_index * kSamplesPerChunk;
        const size_t count = std::min(kSamplesPerChunk, sample_count - offset);
        chunks[chunk_index].sample_offset = static_cast<uint32_t>(offset);
        chunks[chunk_index].sample_count = static_cast<uint32_t>(count);
    }

    std::atomic<size_t> next_chunk{0};
    auto worker = [&] {
        while(true) {
            const size_t chunk_index = next_chunk.fetch_add(1);
            if(chunk_index >= chunks.size()) {
                break;
            }
            auto &chunk = chunks[chunk_index];
            std::vector<uint8_t> quantized(chunk.sample_count);
            quantize_depth8_into(samples + chunk.sample_offset, chunk.sample_count, raw_step, quantized.data());
            chunk.payload = compress_fn(quantized.data(), quantized.size());
        }
    };

    const size_t worker_count = depth_chunk_compression_worker_count(chunks.size());
    std::vector<std::future<void>> futures;
    futures.reserve(worker_count > 0 ? worker_count - 1 : 0);
    for(size_t worker_index = 1; worker_index < worker_count; ++worker_index) {
        futures.emplace_back(std::async(std::launch::async, worker));
    }
    worker();
    for(auto &future : futures) {
        future.get();
    }

    size_t payload_size = kHeaderSize + chunks.size() * kChunkEntrySize;
    for(const auto &chunk : chunks) {
        payload_size += chunk.payload.size();
    }

    std::vector<uint8_t> out;
    out.reserve(payload_size);
    append_le32(out, magic);
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

std::vector<uint8_t> compress_depth_payload(const std::string &codec, const uint8_t *data, size_t size, uint16_t raw_step) {
    if(codec == "none") {
        return std::vector<uint8_t>(data, data + size);
    }
    if(codec == "zlib") {
        return zlib_compress_payload(data, size);
    }
    if(codec == "q8lz4") {
        return q8lz4_compress_payload(data, size, raw_step);
    }
    if(codec == "pq8zlib") {
        return pq8_chunked_compress_payload(data, size, raw_step, 0x5a385150u, zlib_compress_payload);
    }
    if(codec == "pq8lz4") {
        return pq8_chunked_compress_payload(data, size, raw_step, 0x4c385150u, lz4_compress_payload);
    }
    throw std::runtime_error("unsupported codec: " + codec);
}

std::vector<uint8_t> read_file(const std::string &path) {
    std::ifstream input(path, std::ios::binary);
    if(!input) {
        throw std::runtime_error("cannot open input: " + path);
    }
    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    input.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    if(!data.empty()) {
        input.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    return data;
}

uint32_t xorshift32(uint32_t &state) {
    state ^= state << 13u;
    state ^= state >> 17u;
    state ^= state << 5u;
    return state;
}

std::vector<uint8_t> synthetic_frame(int width, int height, int frame_index) {
    std::vector<uint8_t> bytes(static_cast<size_t>(width) * static_cast<size_t>(height) * sizeof(uint16_t));
    auto *out = reinterpret_cast<uint16_t *>(bytes.data());
    uint32_t rng = 0x12345678u + static_cast<uint32_t>(frame_index * 2654435761u);
    for(int y = 0; y < height; ++y) {
        for(int x = 0; x < width; ++x) {
            const int noise = static_cast<int>(xorshift32(rng) % 13u) - 6;
            int value = 600 + (x * 2) + (y * 3) + frame_index * 5 + noise;
            if(((x / 96) + (y / 80) + frame_index) % 23 == 0) {
                value = 0;
            }
            out[static_cast<size_t>(y) * width + x] = static_cast<uint16_t>(std::clamp(value, 0, 8000));
        }
    }
    return bytes;
}

std::vector<std::vector<uint8_t>> load_frames(const Options &options) {
    const size_t frame_bytes = static_cast<size_t>(options.width) * static_cast<size_t>(options.height) * sizeof(uint16_t);
    if(!options.input_path.empty()) {
        auto input = read_file(options.input_path);
        const size_t available_frames = input.size() / frame_bytes;
        if(available_frames == 0) {
            throw std::runtime_error("input does not contain a full frame");
        }
        const size_t frame_count = std::min<size_t>(static_cast<size_t>(options.frames), available_frames);
        std::vector<std::vector<uint8_t>> frames;
        frames.reserve(frame_count);
        for(size_t i = 0; i < frame_count; ++i) {
            const auto begin = input.begin() + static_cast<std::ptrdiff_t>(i * frame_bytes);
            frames.emplace_back(begin, begin + static_cast<std::ptrdiff_t>(frame_bytes));
        }
        return frames;
    }

    std::vector<std::vector<uint8_t>> frames;
    frames.reserve(static_cast<size_t>(options.frames));
    for(int i = 0; i < options.frames; ++i) {
        frames.push_back(synthetic_frame(options.width, options.height, i));
    }
    return frames;
}

double percentile(std::vector<double> values, double p) {
    if(values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const double index = std::clamp(p, 0.0, 1.0) * static_cast<double>(values.size() - 1);
    return values[static_cast<size_t>(std::llround(index))];
}

}  // namespace

int main(int argc, char **argv) {
    try {
        const auto options = parse_args(argc, argv);
        const auto frames = load_frames(options);
        const size_t raw_frame_bytes = static_cast<size_t>(options.width) * static_cast<size_t>(options.height) * sizeof(uint16_t);
        const uint16_t raw_step = raw_step_for_depth_scale(options.depth_scale, options.quantization_mm);

        std::cout << "codec,frames,width,height,fps,raw_step,quant_mm,avg_ms,p50_ms,p95_ms,max_ms,avg_bytes,ratio,mbps_at_fps,errors\n";
        for(const auto &codec : options.codecs) {
            std::vector<double> timings_ms;
            timings_ms.reserve(frames.size());
            uint64_t total_bytes = 0;
            size_t errors = 0;
            for(const auto &frame : frames) {
                try {
                    const auto started = std::chrono::steady_clock::now();
                    const auto payload = compress_depth_payload(codec, frame.data(), frame.size(), raw_step);
                    const auto ended = std::chrono::steady_clock::now();
                    timings_ms.push_back(std::chrono::duration<double, std::milli>(ended - started).count());
                    total_bytes += payload.size();
                }
                catch(const std::exception &e) {
                    if(errors == 0) {
                        std::cerr << "codec " << codec << " first_error=" << e.what() << "\n";
                    }
                    ++errors;
                }
            }
            const double avg_ms =
                timings_ms.empty() ? 0.0 : std::accumulate(timings_ms.begin(), timings_ms.end(), 0.0) / static_cast<double>(timings_ms.size());
            const double p50_ms = percentile(timings_ms, 0.50);
            const double p95_ms = percentile(timings_ms, 0.95);
            const double max_ms = timings_ms.empty() ? 0.0 : *std::max_element(timings_ms.begin(), timings_ms.end());
            const double avg_bytes = timings_ms.empty() ? 0.0 : static_cast<double>(total_bytes) / static_cast<double>(timings_ms.size());
            const double ratio = raw_frame_bytes == 0 ? 0.0 : avg_bytes / static_cast<double>(raw_frame_bytes);
            const double mbps = avg_bytes * 8.0 * static_cast<double>(options.fps) / 1000000.0;
            std::cout << codec << ',' << timings_ms.size() << ',' << options.width << ',' << options.height << ',' << options.fps << ','
                      << raw_step << ',' << std::fixed << std::setprecision(3) << options.quantization_mm << ',' << avg_ms << ','
                      << p50_ms << ',' << p95_ms << ',' << max_ms << ',' << avg_bytes << ',' << ratio << ',' << mbps << ',' << errors
                      << "\n";
        }
    }
    catch(const std::exception &e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
