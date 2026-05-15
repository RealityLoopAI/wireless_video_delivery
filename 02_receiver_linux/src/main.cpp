#include "gwv3_common/protocol.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <zlib.h>

namespace gwv3 {
namespace {

constexpr size_t kMediaHeaderSize = 94;
constexpr size_t kMaxReasonablePayload = 128ull * 1024ull * 1024ull;
constexpr size_t kMaxRgbPreviewPrefixBytes = 512ull * 1024ull;
constexpr uint32_t kRgbPreviewWidth = 640;
constexpr uint32_t kRgbPreviewFps = 30;
constexpr uint64_t kCameraOnlineTimeoutUs = 5ull * 1000ull * 1000ull;
constexpr uint64_t kOfflineCameraPurgeUs = 30ull * 1000ull * 1000ull;
constexpr uint64_t kPreviewFreshUs = 5ull * 1000ull * 1000ull;
constexpr uint32_t kRecordFpsProbeFrames = 60;
constexpr uint64_t kRecordFpsProbeMaxWaitUs = 3'000'000ull;
constexpr double kMinRecordFps = 5.0;
constexpr double kMaxRecordFps = 60.0;
constexpr size_t kMaxPendingRgbRecordBytes = 8ull * 1024ull * 1024ull;
constexpr size_t kMaxPendingDepthRecordBytes = 64ull * 1024ull * 1024ull;

std::atomic<bool> g_running{true};

void handle_signal(int) {
    g_running = false;
}

uint64_t now_us() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

std::string local_time_text(const char *format) {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&time, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, format);
    return oss.str();
}

std::string timestamp_text() {
    return local_time_text("%Y-%m-%d %H:%M:%S");
}

std::string date_dir() {
    return local_time_text("%Y-%m-%d");
}

std::string time_dir() {
    return local_time_text("%H%M%S");
}

std::string json_escape(const std::string &value) {
    std::ostringstream oss;
    for(unsigned char ch : value) {
        switch(ch) {
        case '\\':
            oss << "\\\\";
            break;
        case '"':
            oss << "\\\"";
            break;
        case '\b':
            oss << "\\b";
            break;
        case '\f':
            oss << "\\f";
            break;
        case '\n':
            oss << "\\n";
            break;
        case '\r':
            oss << "\\r";
            break;
        case '\t':
            oss << "\\t";
            break;
        default:
            if(ch < 0x20) {
                oss << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch);
            }
            else {
                oss << ch;
            }
        }
    }
    return oss.str();
}

std::string shell_quote(const std::string &value) {
    std::string out = "'";
    for(char ch : value) {
        if(ch == '\'') {
            out += "'\\''";
        }
        else {
            out += ch;
        }
    }
    out += "'";
    return out;
}

std::string trim_copy(std::string value) {
    auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

std::string url_decode(const std::string &value) {
    std::string out;
    for(size_t i = 0; i < value.size(); ++i) {
        if(value[i] == '%' && i + 2 < value.size()) {
            const auto hex = value.substr(i + 1, 2);
            char *end = nullptr;
            const long decoded = std::strtol(hex.c_str(), &end, 16);
            if(end && *end == '\0') {
                out.push_back(static_cast<char>(decoded));
                i += 2;
                continue;
            }
        }
        if(value[i] == '+') {
            out.push_back(' ');
        }
        else {
            out.push_back(value[i]);
        }
    }
    return out;
}

std::map<std::string, std::string> parse_query(const std::string &query) {
    std::map<std::string, std::string> result;
    size_t start = 0;
    while(start <= query.size()) {
        const size_t amp = query.find('&', start);
        const auto item = query.substr(start, amp == std::string::npos ? std::string::npos : amp - start);
        const size_t eq = item.find('=');
        if(eq != std::string::npos) {
            result[url_decode(item.substr(0, eq))] = url_decode(item.substr(eq + 1));
        }
        else if(!item.empty()) {
            result[url_decode(item)] = "";
        }
        if(amp == std::string::npos) {
            break;
        }
        start = amp + 1;
    }
    return result;
}

std::optional<std::string> json_string_field(const std::string &json, const std::string &key) {
    const std::regex pattern("\"" + key + "\"\\s*:\\s*\"([^\"]*)\"");
    std::smatch match;
    if(std::regex_search(json, match, pattern) && match.size() >= 2) {
        return match[1].str();
    }
    return std::nullopt;
}

std::optional<int> json_int_field(const std::string &json, const std::string &key) {
    const std::regex pattern("\"" + key + "\"\\s*:\\s*(-?[0-9]+)");
    std::smatch match;
    if(std::regex_search(json, match, pattern) && match.size() >= 2) {
        return std::stoi(match[1].str());
    }
    return std::nullopt;
}

std::optional<bool> json_bool_field(const std::string &json, const std::string &key) {
    const std::regex pattern("\"" + key + "\"\\s*:\\s*(true|false)");
    std::smatch match;
    if(std::regex_search(json, match, pattern) && match.size() >= 2) {
        return match[1].str() == "true";
    }
    return std::nullopt;
}

std::optional<std::string> json_object_field(const std::string &json, const std::string &key) {
    const std::string marker = "\"" + key + "\"";
    const size_t key_pos = json.find(marker);
    if(key_pos == std::string::npos) {
        return std::nullopt;
    }
    const size_t colon = json.find(':', key_pos + marker.size());
    if(colon == std::string::npos) {
        return std::nullopt;
    }
    const size_t start = json.find('{', colon + 1);
    if(start == std::string::npos) {
        return std::nullopt;
    }

    bool in_string = false;
    bool escaped = false;
    int depth = 0;
    for(size_t i = start; i < json.size(); ++i) {
        const char ch = json[i];
        if(in_string) {
            if(escaped) {
                escaped = false;
            }
            else if(ch == '\\') {
                escaped = true;
            }
            else if(ch == '"') {
                in_string = false;
            }
            continue;
        }
        if(ch == '"') {
            in_string = true;
        }
        else if(ch == '{') {
            ++depth;
        }
        else if(ch == '}') {
            --depth;
            if(depth == 0) {
                return json.substr(start, i - start + 1);
            }
        }
    }
    return std::nullopt;
}

std::optional<std::string> json_string_in_object(const std::string &json, const std::string &object_key, const std::string &field_key) {
    const auto object = json_object_field(json, object_key);
    if(!object) {
        return std::nullopt;
    }
    return json_string_field(*object, field_key);
}

std::optional<int> json_int_in_object(const std::string &json, const std::string &object_key, const std::string &field_key) {
    const auto object = json_object_field(json, object_key);
    if(!object) {
        return std::nullopt;
    }
    return json_int_field(*object, field_key);
}

std::string config_string(const std::string &json, const std::string &key, const std::string &fallback) {
    return json_string_field(json, key).value_or(fallback);
}

int config_int(const std::string &json, const std::string &key, int fallback) {
    return json_int_field(json, key).value_or(fallback);
}

bool config_bool(const std::string &json, const std::string &key, bool fallback) {
    return json_bool_field(json, key).value_or(fallback);
}

uint16_t config_port(const std::string &json, const std::string &key, uint16_t fallback) {
    const int value = config_int(json, key, fallback);
    if(value <= 0 || value > 65535) {
        throw std::runtime_error("invalid port in receiver config: " + key);
    }
    return static_cast<uint16_t>(value);
}

uint16_t read_le16(const uint8_t *data) {
    return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8u);
}

uint32_t read_le32(const uint8_t *data) {
    uint32_t value = 0;
    for(int i = 0; i < 4; ++i) {
        value |= static_cast<uint32_t>(data[i]) << (i * 8u);
    }
    return value;
}

uint64_t read_le64(const uint8_t *data) {
    uint64_t value = 0;
    for(int i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(data[i]) << (i * 8u);
    }
    return value;
}

bool read_exact(int fd, void *data, size_t size) {
    auto *ptr = static_cast<uint8_t *>(data);
    size_t offset = 0;
    while(offset < size && g_running) {
        const ssize_t got = recv(fd, ptr + offset, size - offset, 0);
        if(got == 0) {
            return false;
        }
        if(got < 0) {
            if(errno == EINTR) {
                continue;
            }
            return false;
        }
        offset += static_cast<size_t>(got);
    }
    return offset == size;
}

void set_socket_timeout(int fd, int seconds) {
    timeval tv{};
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

sockaddr_in make_bind_addr(const std::string &ip, uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if(ip.empty() || ip == "0.0.0.0") {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    }
    else if(inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        throw std::runtime_error("invalid bind ip: " + ip);
    }
    return addr;
}

std::string camera_key(const std::string &sender_id, const std::string &camera_id) {
    return sender_id + "_" + camera_id;
}

bool h264_payload_has_nal_type(const std::vector<uint8_t> &payload, uint8_t expected_type) {
    for(size_t i = 0; i + 4 < payload.size(); ++i) {
        size_t nal_offset = std::string::npos;
        if(payload[i] == 0 && payload[i + 1] == 0 && payload[i + 2] == 1) {
            nal_offset = i + 3;
        }
        else if(i + 4 < payload.size() && payload[i] == 0 && payload[i + 1] == 0 && payload[i + 2] == 0 && payload[i + 3] == 1) {
            nal_offset = i + 4;
        }
        if(nal_offset != std::string::npos && nal_offset < payload.size()) {
            const uint8_t nal_type = payload[nal_offset] & 0x1fu;
            if(nal_type == expected_type) {
                return true;
            }
        }
    }
    return false;
}

bool h264_payload_has_vcl_nal(const std::vector<uint8_t> &payload) {
    for(size_t i = 0; i + 4 < payload.size(); ++i) {
        size_t nal_offset = std::string::npos;
        if(payload[i] == 0 && payload[i + 1] == 0 && payload[i + 2] == 1) {
            nal_offset = i + 3;
        }
        else if(i + 4 < payload.size() && payload[i] == 0 && payload[i + 1] == 0 && payload[i + 2] == 0 && payload[i + 3] == 1) {
            nal_offset = i + 4;
        }
        if(nal_offset != std::string::npos && nal_offset < payload.size()) {
            const uint8_t nal_type = payload[nal_offset] & 0x1fu;
            if(nal_type >= 1 && nal_type <= 5) {
                return true;
            }
        }
    }
    return false;
}

struct PreviewImage {
    std::vector<uint8_t> bytes;
    uint32_t width = 0;
    uint32_t height = 0;
};

uint8_t clamp_color(double value) {
    if(value <= 0.0) {
        return 0;
    }
    if(value >= 255.0) {
        return 255;
    }
    return static_cast<uint8_t>(value);
}

void append_depth_color(std::vector<uint8_t> &out, uint16_t value, uint16_t max_value) {
    if(value == 0 || max_value == 0) {
        out.push_back(12);
        out.push_back(16);
        out.push_back(24);
        return;
    }
    const double t = std::min(1.0, static_cast<double>(value) / static_cast<double>(max_value));
    const auto channel = [t](double center) {
        return clamp_color(255.0 * std::max(0.0, std::min(1.0, 1.5 - std::abs(4.0 * t - center))));
    };
    out.push_back(channel(3.0));
    out.push_back(channel(2.0));
    out.push_back(channel(1.0));
}

void append_u16_le(std::vector<uint8_t> &out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xffu));
    out.push_back(static_cast<uint8_t>((value >> 8u) & 0xffu));
}

void append_u32_le(std::vector<uint8_t> &out, uint32_t value) {
    for(int i = 0; i < 4; ++i) {
        out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xffu));
    }
}

PreviewImage build_bmp_from_rgb_pixels(const std::vector<uint8_t> &rgb, uint32_t width, uint32_t height) {
    PreviewImage image;
    if(width == 0 || height == 0 || rgb.size() < static_cast<size_t>(width) * height * 3ull) {
        return image;
    }

    constexpr uint32_t file_header_size = 14;
    constexpr uint32_t dib_header_size = 40;
    const uint32_t row_stride = ((width * 3u + 3u) / 4u) * 4u;
    const uint32_t image_size = row_stride * height;
    const uint32_t file_size = file_header_size + dib_header_size + image_size;

    image.width = width;
    image.height = height;
    image.bytes.reserve(file_size);
    image.bytes.push_back('B');
    image.bytes.push_back('M');
    append_u32_le(image.bytes, file_size);
    append_u16_le(image.bytes, 0);
    append_u16_le(image.bytes, 0);
    append_u32_le(image.bytes, file_header_size + dib_header_size);
    append_u32_le(image.bytes, dib_header_size);
    append_u32_le(image.bytes, width);
    append_u32_le(image.bytes, height);
    append_u16_le(image.bytes, 1);
    append_u16_le(image.bytes, 24);
    append_u32_le(image.bytes, 0);
    append_u32_le(image.bytes, image_size);
    append_u32_le(image.bytes, 2835);
    append_u32_le(image.bytes, 2835);
    append_u32_le(image.bytes, 0);
    append_u32_le(image.bytes, 0);

    std::vector<uint8_t> padding(row_stride - width * 3u, 0);
    for(uint32_t row = 0; row < height; ++row) {
        const uint32_t y = height - 1u - row;
        for(uint32_t x = 0; x < width; ++x) {
            const size_t offset = (static_cast<size_t>(y) * width + x) * 3ull;
            image.bytes.push_back(rgb[offset + 2]);
            image.bytes.push_back(rgb[offset + 1]);
            image.bytes.push_back(rgb[offset + 0]);
        }
        image.bytes.insert(image.bytes.end(), padding.begin(), padding.end());
    }

    return image;
}

PreviewImage build_depth_preview_bmp(const std::vector<uint8_t> &payload, uint32_t width, uint32_t height) {
    PreviewImage image;
    if(width == 0 || height == 0 || payload.size() < static_cast<size_t>(width) * height * 2ull) {
        return image;
    }

    const uint32_t stride = std::max<uint32_t>(1, (width + 479u) / 480u);
    image.width = width / stride;
    image.height = height / stride;
    if(image.width == 0 || image.height == 0) {
        return {};
    }

    uint16_t max_value = 0;
    for(uint32_t y = 0; y < height; y += stride) {
        for(uint32_t x = 0; x < width; x += stride) {
            const size_t offset = (static_cast<size_t>(y) * width + x) * 2ull;
            const uint16_t value = static_cast<uint16_t>(payload[offset]) | (static_cast<uint16_t>(payload[offset + 1]) << 8u);
            max_value = std::max(max_value, value);
        }
    }

    std::vector<uint8_t> rgb;
    rgb.reserve(static_cast<size_t>(image.width) * image.height * 3ull);

    for(uint32_t y = 0; y < height && y / stride < image.height; y += stride) {
        for(uint32_t x = 0; x < width && x / stride < image.width; x += stride) {
            const size_t offset = (static_cast<size_t>(y) * width + x) * 2ull;
            const uint16_t value = static_cast<uint16_t>(payload[offset]) | (static_cast<uint16_t>(payload[offset + 1]) << 8u);
            append_depth_color(rgb, value, max_value);
        }
    }

    return build_bmp_from_rgb_pixels(rgb, image.width, image.height);
}

struct Config {
    std::string status_bind_ip = "0.0.0.0";
    uint16_t status_port = 50011;
    std::string media_bind_ip = "0.0.0.0";
    uint16_t media_port = 50010;
    std::string admin_bind_ip = "127.0.0.1";
    uint16_t admin_port = 18080;
    std::string nas_root = "/home/fz/Desktop/nas";
    std::string log_directory = "08_reports/receiver_logs";
    std::string ffmpeg_path = "ffmpeg";
    int segment_seconds = 300;
    int depth_fps = 30;
    bool write_debug_h264 = true;
    bool write_debug_depth_raw = true;
    size_t max_payload_bytes = kMaxReasonablePayload;
};

Config load_config(const std::string &path) {
    std::ifstream input(path);
    if(!input) {
        throw std::runtime_error("cannot open receiver config: " + path);
    }
    const std::string json((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());

    Config cfg;
    cfg.status_bind_ip = config_string(json, "status_bind_ip", cfg.status_bind_ip);
    cfg.status_port = config_port(json, "status_port", cfg.status_port);
    cfg.media_bind_ip = config_string(json, "media_bind_ip", cfg.media_bind_ip);
    cfg.media_port = config_port(json, "media_port", cfg.media_port);
    cfg.admin_bind_ip = config_string(json, "admin_bind_ip", cfg.admin_bind_ip);
    cfg.admin_port = config_port(json, "admin_port", cfg.admin_port);
    cfg.nas_root = config_string(json, "nas_root", cfg.nas_root);
    cfg.log_directory = config_string(json, "log_directory", cfg.log_directory);
    cfg.ffmpeg_path = config_string(json, "ffmpeg_path", cfg.ffmpeg_path);
    cfg.segment_seconds = config_int(json, "segment_seconds", cfg.segment_seconds);
    cfg.depth_fps = config_int(json, "depth_fps", cfg.depth_fps);
    cfg.write_debug_h264 = config_bool(json, "write_debug_h264", cfg.write_debug_h264);
    cfg.write_debug_depth_raw = config_bool(json, "write_debug_depth_raw", cfg.write_debug_depth_raw);
    cfg.max_payload_bytes = static_cast<size_t>(std::max(1, config_int(json, "max_payload_mb", 128))) * 1024ull * 1024ull;

    if(cfg.segment_seconds <= 0) {
        throw std::runtime_error("segment_seconds must be positive");
    }
    if(cfg.depth_fps <= 0) {
        throw std::runtime_error("depth_fps must be positive");
    }
    return cfg;
}

class Logger {
public:
    explicit Logger(std::string directory) : directory_(std::move(directory)) {
        std::filesystem::create_directories(directory_);
        log_path_ = directory_ + "/receiver.log";
        stream_.open(log_path_, std::ios::app);
        if(!stream_) {
            throw std::runtime_error("cannot open receiver log: " + log_path_);
        }
    }

    void info(const std::string &message) { write("INFO", message); }
    void warn(const std::string &message) { write("WARN", message); }
    void error(const std::string &message) { write("ERROR", message); }

private:
    void write(const std::string &level, const std::string &message) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto line = timestamp_text() + " [" + level + "] " + message;
        stream_ << line << '\n';
        stream_.flush();
        std::cout << line << std::endl;
    }

    std::string directory_;
    std::string log_path_;
    std::ofstream stream_;
    std::mutex mutex_;
};

ssize_t retrying_write(int fd, const uint8_t *data, size_t size) {
    while(true) {
        const ssize_t written = write(fd, data, size);
        if(written < 0 && errno == EINTR) {
            continue;
        }
        return written;
    }
}

std::optional<size_t> find_marker(const std::vector<uint8_t> &buffer, uint8_t a, uint8_t b, size_t start) {
    if(buffer.size() < 2 || start >= buffer.size() - 1) {
        return std::nullopt;
    }
    for(size_t i = start; i + 1 < buffer.size(); ++i) {
        if(buffer[i] == a && buffer[i + 1] == b) {
            return i;
        }
    }
    return std::nullopt;
}

class RgbPreviewDecoder {
public:
    RgbPreviewDecoder() = default;
    RgbPreviewDecoder(const RgbPreviewDecoder &) = delete;
    RgbPreviewDecoder &operator=(const RgbPreviewDecoder &) = delete;

    ~RgbPreviewDecoder() {
        stop();
    }

    bool start(const Config &cfg, const std::string &key, uint32_t width, uint32_t height, Logger &logger) {
        stop();
        key_ = key;
        source_width_ = width;
        source_height_ = height;
        preview_width_ = width > 0 ? std::min<uint32_t>(width, kRgbPreviewWidth) : kRgbPreviewWidth;
        preview_height_ = scaled_height(width, height, preview_width_);

        int stdin_pipe[2] = {-1, -1};
        int stdout_pipe[2] = {-1, -1};
        if(pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0) {
            close_pipe(stdin_pipe);
            close_pipe(stdout_pipe);
            logger.warn("rgb preview decoder pipe creation failed: " + std::string(std::strerror(errno)));
            return false;
        }

        const pid_t pid = fork();
        if(pid < 0) {
            close_pipe(stdin_pipe);
            close_pipe(stdout_pipe);
            logger.warn("rgb preview decoder fork failed: " + std::string(std::strerror(errno)));
            return false;
        }

        if(pid == 0) {
            dup2(stdin_pipe[0], STDIN_FILENO);
            dup2(stdout_pipe[1], STDOUT_FILENO);
            const int dev_null = open("/dev/null", O_WRONLY);
            if(dev_null >= 0) {
                dup2(dev_null, STDERR_FILENO);
                close(dev_null);
            }
            close_pipe(stdin_pipe);
            close_pipe(stdout_pipe);
            const long max_fd = std::min<long>(sysconf(_SC_OPEN_MAX), 4096);
            for(int fd = 3; fd < max_fd; ++fd) {
                close(fd);
            }

            const std::string scale = "fps=" + std::to_string(kRgbPreviewFps) + ",scale=" + std::to_string(kRgbPreviewWidth) + ":-2";
            execlp(cfg.ffmpeg_path.c_str(), cfg.ffmpeg_path.c_str(), "-hide_banner", "-loglevel", "error", "-fflags", "nobuffer",
                   "-flags", "low_delay", "-probesize", "32", "-analyzeduration", "0", "-avioflags", "direct", "-f", "h264", "-i", "pipe:0",
                   "-vf", scale.c_str(), "-q:v", "3", "-f", "image2pipe", "-vcodec", "mjpeg", "pipe:1", static_cast<char *>(nullptr));
            _exit(127);
        }

        close(stdin_pipe[0]);
        close(stdout_pipe[1]);
        stdin_fd_ = stdin_pipe[1];
        stdout_fd_ = stdout_pipe[0];
        pid_ = pid;
        running_ = true;
        reader_ = std::thread([this] { read_loop(); });
        logger.info("rgb preview decoder started: " + key_);
        return true;
    }

    bool active() const {
        return running_ && stdin_fd_ >= 0;
    }

    void stop() {
        int stdin_fd = -1;
        {
            std::lock_guard<std::mutex> lock(process_mutex_);
            running_ = false;
            stdin_fd = stdin_fd_;
            stdin_fd_ = -1;
        }
        if(stdin_fd >= 0) {
            close(stdin_fd);
        }
        if(reader_.joinable()) {
            reader_.join();
        }
        if(stdout_fd_ >= 0) {
            close(stdout_fd_);
            stdout_fd_ = -1;
        }
        if(pid_ > 0) {
            int status = 0;
            waitpid(pid_, &status, 0);
            pid_ = -1;
        }
    }

    bool write_packet(const std::vector<uint8_t> &payload) {
        if(payload.empty()) {
            return true;
        }
        std::lock_guard<std::mutex> lock(process_mutex_);
        if(!running_ || stdin_fd_ < 0) {
            return false;
        }
        size_t offset = 0;
        while(offset < payload.size()) {
            const ssize_t written = retrying_write(stdin_fd_, payload.data() + offset, payload.size() - offset);
            if(written <= 0) {
                running_ = false;
                close(stdin_fd_);
                stdin_fd_ = -1;
                return false;
            }
            offset += static_cast<size_t>(written);
        }
        return true;
    }

    std::optional<std::vector<uint8_t>> latest_jpeg() const {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        if(latest_jpeg_.empty()) {
            return std::nullopt;
        }
        return latest_jpeg_;
    }

    bool has_frame() const {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        return !latest_jpeg_.empty();
    }

    uint32_t preview_width() const {
        return preview_width_;
    }

    uint32_t preview_height() const {
        return preview_height_;
    }

    uint64_t frame_us() const {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        return latest_frame_us_;
    }

private:
    static uint32_t scaled_height(uint32_t width, uint32_t height, uint32_t preview_width) {
        if(width == 0 || height == 0 || preview_width == 0) {
            return 0;
        }
        uint32_t scaled = static_cast<uint32_t>((static_cast<uint64_t>(height) * preview_width) / width);
        if(scaled % 2u != 0u) {
            ++scaled;
        }
        return scaled;
    }

    static void close_pipe(int fds[2]) {
        if(fds[0] >= 0) {
            close(fds[0]);
        }
        if(fds[1] >= 0) {
            close(fds[1]);
        }
    }

    void read_loop() {
        std::vector<uint8_t> buffer;
        buffer.reserve(512 * 1024);
        std::vector<uint8_t> chunk(32 * 1024);
        while(true) {
            const ssize_t got = read(stdout_fd_, chunk.data(), chunk.size());
            if(got <= 0) {
                break;
            }
            buffer.insert(buffer.end(), chunk.begin(), chunk.begin() + got);
            consume_jpegs(buffer);
            if(buffer.size() > 4ull * 1024ull * 1024ull) {
                buffer.erase(buffer.begin(), buffer.end() - 1024);
            }
        }
    }

    void consume_jpegs(std::vector<uint8_t> &buffer) {
        while(true) {
            const auto start = find_marker(buffer, 0xff, 0xd8, 0);
            if(!start) {
                buffer.clear();
                return;
            }
            if(*start > 0) {
                buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(*start));
            }
            const auto end = find_marker(buffer, 0xff, 0xd9, 2);
            if(!end) {
                return;
            }
            std::vector<uint8_t> jpeg(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(*end + 2));
            {
                std::lock_guard<std::mutex> lock(frame_mutex_);
                latest_jpeg_ = std::move(jpeg);
                latest_frame_us_ = now_us();
            }
            buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(*end + 2));
        }
    }

    std::string key_;
    uint32_t source_width_ = 0;
    uint32_t source_height_ = 0;
    uint32_t preview_width_ = 0;
    uint32_t preview_height_ = 0;
    mutable std::mutex process_mutex_;
    mutable std::mutex frame_mutex_;
    std::atomic<bool> running_{false};
    pid_t pid_ = -1;
    int stdin_fd_ = -1;
    int stdout_fd_ = -1;
    std::thread reader_;
    std::vector<uint8_t> latest_jpeg_;
    uint64_t latest_frame_us_ = 0;
};

struct MediaPacket {
    StreamType stream_type = StreamType::rgb;
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
    PixelFormat pixel_format = PixelFormat::encoded_video;
    uint64_t payload_size = 0;
    uint64_t uncompressed_size = 0;
    std::vector<uint8_t> payload;
};

MediaPacket read_media_packet(int fd, size_t max_payload_bytes) {
    uint8_t header[kMediaHeaderSize] = {};
    if(!read_exact(fd, header, sizeof(header))) {
        throw std::runtime_error("connection closed");
    }

    const uint32_t magic = read_le32(header + 0);
    const uint16_t header_version = read_le16(header + 4);
    const uint16_t header_size = read_le16(header + 6);
    if(magic != kMediaMagic || header_version != kMediaHeaderVersion || header_size != kMediaHeaderSize) {
        throw std::runtime_error("invalid media packet header");
    }

    const uint16_t sender_id_len = read_le16(header + 14);
    const uint16_t camera_id_len = read_le16(header + 16);
    const uint16_t codec_len = read_le16(header + 18);
    const uint64_t payload_size = read_le64(header + 62);
    if(payload_size > max_payload_bytes) {
        throw std::runtime_error("media payload too large");
    }

    MediaPacket packet;
    packet.stream_type = static_cast<StreamType>(header[8]);
    packet.flags = read_le32(header + 10);
    packet.frame_id = read_le64(header + 20);
    packet.timestamp_us = read_le64(header + 28);
    packet.system_timestamp_us = read_le64(header + 36);
    packet.pair_id = read_le64(header + 44);
    packet.width = read_le32(header + 52);
    packet.height = read_le32(header + 56);
    packet.pixel_format = static_cast<PixelFormat>(read_le16(header + 60));
    packet.payload_size = payload_size;
    packet.uncompressed_size = read_le64(header + 70);

    std::vector<char> text(sender_id_len + camera_id_len + codec_len);
    if(!text.empty() && !read_exact(fd, text.data(), text.size())) {
        throw std::runtime_error("connection closed while reading packet strings");
    }
    packet.sender_id.assign(text.data(), sender_id_len);
    packet.camera_id.assign(text.data() + sender_id_len, camera_id_len);
    packet.codec_or_compression.assign(text.data() + sender_id_len + camera_id_len, codec_len);

    packet.payload.resize(static_cast<size_t>(payload_size));
    if(payload_size > 0 && !read_exact(fd, packet.payload.data(), packet.payload.size())) {
        throw std::runtime_error("connection closed while reading payload");
    }
    return packet;
}

std::vector<uint8_t> zlib_decompress_payload(const MediaPacket &packet) {
    if(packet.uncompressed_size == 0 || packet.uncompressed_size > kMaxReasonablePayload) {
        throw std::runtime_error("invalid zlib uncompressed depth size");
    }
    std::vector<uint8_t> out(static_cast<size_t>(packet.uncompressed_size));
    uLongf out_size = static_cast<uLongf>(out.size());
    const int rc = uncompress(out.data(), &out_size, packet.payload.data(), static_cast<uLong>(packet.payload.size()));
    if(rc != Z_OK || out_size != packet.uncompressed_size) {
        throw std::runtime_error("zlib depth decompression failed");
    }
    return out;
}

MediaPacket normalized_depth_packet(const MediaPacket &packet) {
    if(packet.stream_type != StreamType::depth_raw) {
        return packet;
    }
    if(packet.codec_or_compression == "none") {
        return packet;
    }
    if(packet.codec_or_compression == "zlib") {
        MediaPacket decoded = packet;
        decoded.payload = zlib_decompress_payload(packet);
        decoded.payload_size = decoded.payload.size();
        decoded.codec_or_compression = "none";
        return decoded;
    }
    throw std::runtime_error("unsupported depth compression: " + packet.codec_or_compression);
}

class FfmpegPipe {
public:
    FfmpegPipe() = default;
    FfmpegPipe(const FfmpegPipe &) = delete;
    FfmpegPipe &operator=(const FfmpegPipe &) = delete;

    ~FfmpegPipe() {
        close();
    }

    bool open(const std::string &command, Logger &logger) {
        close();
        command_ = command;
        pipe_ = popen(command.c_str(), "w");
        if(!pipe_) {
            logger.error("failed to start ffmpeg pipe: " + command + ": " + std::strerror(errno));
            return false;
        }
        return true;
    }

    bool write(const uint8_t *data, size_t size, Logger &logger) {
        if(!pipe_) {
            return false;
        }
        if(size == 0) {
            return true;
        }
        const size_t written = fwrite(data, 1, size, pipe_);
        if(written != size) {
            logger.warn("ffmpeg pipe write failed");
            close();
            return false;
        }
        return true;
    }

    int close() {
        if(!pipe_) {
            return 0;
        }
        const int rc = pclose(pipe_);
        pipe_ = nullptr;
        return rc;
    }

    bool active() const {
        return pipe_ != nullptr;
    }

private:
    FILE *pipe_ = nullptr;
    std::string command_;
};

struct FrameInfo {
    bool valid = false;
    uint64_t frame_id = 0;
    uint64_t timestamp_us = 0;
    uint64_t system_timestamp_us = 0;
};

struct StreamRecordStats {
    uint64_t frames = 0;
    uint64_t first_local_us = 0;
    uint64_t last_local_us = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    std::string codec_or_compression;

    void add(const MediaPacket &packet, uint64_t local_us) {
        if(frames == 0) {
            first_local_us = local_us;
            width = packet.width;
            height = packet.height;
            codec_or_compression = packet.codec_or_compression;
        }
        last_local_us = local_us;
        if(width == 0) {
            width = packet.width;
        }
        if(height == 0) {
            height = packet.height;
        }
        if(codec_or_compression.empty()) {
            codec_or_compression = packet.codec_or_compression;
        }
        ++frames;
    }

    double actual_fps() const {
        if(frames >= 2 && last_local_us > first_local_us) {
            const double seconds = static_cast<double>(last_local_us - first_local_us) / 1'000'000.0;
            if(seconds > 0.0) {
                return static_cast<double>(frames - 1) / seconds;
            }
        }
        return 0.0;
    }

    void reset() {
        frames = 0;
        first_local_us = 0;
        last_local_us = 0;
        width = 0;
        height = 0;
        codec_or_compression.clear();
    }
};

std::string format_fps(double fps) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(3) << fps;
    std::string value = out.str();
    while(value.size() > 1 && value.back() == '0') {
        value.pop_back();
    }
    if(!value.empty() && value.back() == '.') {
        value.pop_back();
    }
    return value;
}

struct FpsProbe {
    uint64_t first_us = 0;
    uint64_t last_us = 0;
    uint32_t frames = 0;

    void add(uint64_t local_us) {
        if(first_us == 0) {
            first_us = local_us;
        }
        last_us = local_us;
        ++frames;
    }

    bool ready(uint64_t local_us) const {
        return frames >= kRecordFpsProbeFrames || (first_us > 0 && local_us > first_us && local_us - first_us >= kRecordFpsProbeMaxWaitUs);
    }

    double estimate(double fallback) const {
        if(frames >= 2 && last_us > first_us) {
            const double seconds = static_cast<double>(last_us - first_us) / 1'000'000.0;
            if(seconds > 0.0) {
                return std::clamp((static_cast<double>(frames - 1) / seconds), kMinRecordFps, kMaxRecordFps);
            }
        }
        return std::clamp(fallback, kMinRecordFps, kMaxRecordFps);
    }

    void reset() {
        first_us = 0;
        last_us = 0;
        frames = 0;
    }
};

struct CameraState;

class SegmentWriter {
public:
    SegmentWriter() = default;
    SegmentWriter(const SegmentWriter &) = delete;
    SegmentWriter &operator=(const SegmentWriter &) = delete;

    bool active() const {
        return active_;
    }

    const std::string &directory() const {
        return directory_;
    }

    void start(const Config &cfg, const std::string &sender_id, const std::string &camera_id, const std::string &announce_json,
               Logger &logger) {
        close(cfg, sender_id, camera_id, announce_json, logger);
        start_us_ = now_us();
        start_steady_ = std::chrono::steady_clock::now();
        const auto key = camera_key(sender_id, camera_id);
        directory_ = (std::filesystem::path(cfg.nas_root) / key / date_dir() / time_dir()).string();
        std::filesystem::create_directories(directory_);

        frames_csv_.open(std::filesystem::path(directory_) / "frames.csv", std::ios::out | std::ios::trunc);
        frames_csv_ << "local_time_us,stream_type,rgb_frame_id,rgb_timestamp_us,depth_frame_id,depth_timestamp_us,pair_id,pair_delta_ms,width,height,payload_size,"
                       "packet_system_timestamp_us,rgb_system_timestamp_us,depth_system_timestamp_us,frame_id,timestamp_us,frame_system_timestamp_us,"
                       "codec_or_compression\n";

        if(cfg.write_debug_h264) {
            rgb_debug_.open(std::filesystem::path(directory_) / "rgb_debug.h264", std::ios::binary | std::ios::out | std::ios::trunc);
        }
        if(cfg.write_debug_depth_raw) {
            depth_debug_.open(std::filesystem::path(directory_) / "depth_debug.raw", std::ios::binary | std::ios::out | std::ios::trunc);
        }

        write_meta(cfg, sender_id, camera_id, announce_json, false);
        active_ = true;
        logger.info("recording segment started: " + directory_);
    }

    void close(const Config &cfg, const std::string &sender_id, const std::string &camera_id, const std::string &announce_json, Logger &logger) {
        if(!active_) {
            return;
        }
        flush_pending_media(cfg, logger);
        if(frames_csv_) {
            frames_csv_.flush();
            frames_csv_.close();
        }
        if(rgb_debug_) {
            rgb_debug_.close();
        }
        if(depth_debug_) {
            depth_debug_.close();
        }
        const int rgb_rc = rgb_pipe_.close();
        const int depth_rc = depth_pipe_.close();
        end_us_ = now_us();
        write_meta(cfg, sender_id, camera_id, announce_json, true);
        set_segment_mtime_to_start(logger);
        if(rgb_rc != 0) {
            logger.warn("rgb ffmpeg exited with non-zero status for segment: " + directory_);
        }
        if(depth_rc != 0) {
            logger.warn("depth ffmpeg exited with non-zero status for segment: " + directory_);
        }
        logger.info("recording segment closed: " + directory_);
        active_ = false;
        directory_.clear();
        depth_width_ = 0;
        depth_height_ = 0;
        last_rgb_ = {};
        last_depth_ = {};
        rgb_pending_.clear();
        rgb_pending_has_vcl_ = false;
        depth_pending_.clear();
        depth_pending_bytes_ = 0;
        rgb_fps_probe_.reset();
        depth_fps_probe_.reset();
        rgb_record_fps_ = 0.0;
        depth_record_fps_ = 0.0;
        rgb_stats_.reset();
        depth_stats_.reset();
    }

    bool should_rotate(const Config &cfg) const {
        return active_ && std::chrono::steady_clock::now() - start_steady_ >= std::chrono::seconds(cfg.segment_seconds);
    }

    void write_packet(const Config &cfg, const MediaPacket &packet, const std::string &sender_id, const std::string &camera_id,
                      const std::string &announce_json, Logger &logger) {
        if(!active_) {
            start(cfg, sender_id, camera_id, announce_json, logger);
        }
        if(should_rotate(cfg)) {
            close(cfg, sender_id, camera_id, announce_json, logger);
            start(cfg, sender_id, camera_id, announce_json, logger);
        }

        const uint64_t packet_local_us = now_us();
        const auto stream = packet.stream_type == StreamType::rgb ? std::string("rgb") : std::string("depth");
        if(packet.stream_type == StreamType::rgb) {
            if(rgb_debug_) {
                rgb_debug_.write(reinterpret_cast<const char *>(packet.payload.data()), static_cast<std::streamsize>(packet.payload.size()));
            }
            write_rgb_packet(cfg, packet, packet_local_us, logger);
            last_rgb_ = FrameInfo{true, packet.frame_id, packet.timestamp_us, packet.system_timestamp_us};
            rgb_stats_.add(packet, packet_local_us);
        }
        else if(packet.stream_type == StreamType::depth_raw) {
            if(depth_debug_) {
                depth_debug_.write(reinterpret_cast<const char *>(packet.payload.data()), static_cast<std::streamsize>(packet.payload.size()));
            }
            write_depth_packet(cfg, packet, packet_local_us, logger);
            last_depth_ = FrameInfo{true, packet.frame_id, packet.timestamp_us, packet.system_timestamp_us};
            depth_stats_.add(packet, packet_local_us);
        }

        if(frames_csv_) {
            frames_csv_ << packet_local_us << ',' << stream << ',';
            if(last_rgb_.valid) {
                frames_csv_ << last_rgb_.frame_id << ',' << last_rgb_.timestamp_us << ',';
            }
            else {
                frames_csv_ << ",,";
            }
            if(last_depth_.valid) {
                frames_csv_ << last_depth_.frame_id << ',' << last_depth_.timestamp_us << ',';
            }
            else {
                frames_csv_ << ",,";
            }
            frames_csv_ << packet.pair_id << ',';
            if(last_rgb_.valid && last_depth_.valid) {
                const auto delta = last_rgb_.timestamp_us > last_depth_.timestamp_us ? last_rgb_.timestamp_us - last_depth_.timestamp_us
                                                                                      : last_depth_.timestamp_us - last_rgb_.timestamp_us;
                frames_csv_ << static_cast<double>(delta) / 1000.0;
            }
            frames_csv_ << ',' << packet.width << ',' << packet.height << ',' << packet.payload_size << ',' << packet.system_timestamp_us << ',';
            if(last_rgb_.valid) {
                frames_csv_ << last_rgb_.system_timestamp_us;
            }
            frames_csv_ << ',';
            if(last_depth_.valid) {
                frames_csv_ << last_depth_.system_timestamp_us;
            }
            frames_csv_ << ',' << packet.frame_id << ',' << packet.timestamp_us << ',' << packet.system_timestamp_us << ','
                        << packet.codec_or_compression << '\n';
        }
    }

private:
    void ensure_depth_pipe(const Config &cfg, uint32_t width, uint32_t height, double fps, Logger &logger) {
        if(depth_pipe_.active()) {
            return;
        }
        depth_width_ = width;
        depth_height_ = height;
        const auto ffmpeg_log = shell_quote((std::filesystem::path(directory_) / "ffmpeg.log").string());
        const auto depth_mkv = shell_quote((std::filesystem::path(directory_) / "depth.mkv").string());
        std::ostringstream cmd;
        cmd << shell_quote(cfg.ffmpeg_path)
            << " -hide_banner -loglevel warning -y -f rawvideo -pixel_format gray16le -video_size " << width << "x" << height
            << " -framerate " << format_fps(fps) << " -i pipe:0 -c:v ffv1 -level 3 " << depth_mkv << " 2>>" << ffmpeg_log;
        depth_pipe_.open(cmd.str(), logger);
    }

    void ensure_rgb_pipe(const Config &cfg, double fps, Logger &logger) {
        if(rgb_pipe_.active()) {
            return;
        }
        const auto ffmpeg_log = shell_quote((std::filesystem::path(directory_) / "ffmpeg.log").string());
        const auto rgb_mp4 = shell_quote((std::filesystem::path(directory_) / "rgb.mp4").string());
        const std::string rgb_cmd = shell_quote(cfg.ffmpeg_path) +
                                    " -hide_banner -loglevel warning -y -r " + format_fps(fps) + " -f h264 -i pipe:0 -c:v copy -movflags +faststart " +
                                    rgb_mp4 + " 2>>" + ffmpeg_log;
        rgb_pipe_.open(rgb_cmd, logger);
    }

    void write_rgb_packet(const Config &cfg, const MediaPacket &packet, uint64_t packet_local_us, Logger &logger) {
        if(rgb_pipe_.active()) {
            rgb_pipe_.write(packet.payload.data(), packet.payload.size(), logger);
            return;
        }

        if(rgb_pending_has_vcl_ && rgb_pending_.size() + packet.payload.size() > kMaxPendingRgbRecordBytes) {
            flush_rgb_pending(cfg, logger);
        }
        if(rgb_pipe_.active()) {
            rgb_pipe_.write(packet.payload.data(), packet.payload.size(), logger);
            return;
        }
        if(rgb_pending_.size() + packet.payload.size() > kMaxPendingRgbRecordBytes) {
            rgb_pending_.clear();
            rgb_pending_has_vcl_ = false;
            rgb_fps_probe_.reset();
        }
        rgb_pending_.insert(rgb_pending_.end(), packet.payload.begin(), packet.payload.end());

        if(h264_payload_has_vcl_nal(packet.payload)) {
            rgb_pending_has_vcl_ = true;
            const uint64_t fps_probe_us = packet.system_timestamp_us > 0 ? packet.system_timestamp_us : packet_local_us;
            rgb_fps_probe_.add(fps_probe_us);
        }

        const uint64_t ready_probe_us = packet.system_timestamp_us > 0 ? packet.system_timestamp_us : packet_local_us;
        if(rgb_pending_has_vcl_ && rgb_fps_probe_.ready(ready_probe_us)) {
            flush_rgb_pending(cfg, logger);
        }
    }

    void write_depth_packet(const Config &cfg, const MediaPacket &packet, uint64_t packet_local_us, Logger &logger) {
        if(depth_pipe_.active()) {
            depth_pipe_.write(packet.payload.data(), packet.payload.size(), logger);
            return;
        }
        if(depth_width_ == 0 || depth_height_ == 0) {
            depth_width_ = packet.width;
            depth_height_ = packet.height;
        }
        if(!depth_pending_.empty() && depth_pending_bytes_ + packet.payload.size() > kMaxPendingDepthRecordBytes) {
            flush_depth_pending(cfg, logger);
        }
        if(depth_pipe_.active()) {
            depth_pipe_.write(packet.payload.data(), packet.payload.size(), logger);
            return;
        }

        depth_pending_.push_back(packet.payload);
        depth_pending_bytes_ += packet.payload.size();
        const uint64_t fps_probe_us = packet.system_timestamp_us > 0 ? packet.system_timestamp_us : packet_local_us;
        depth_fps_probe_.add(fps_probe_us);
        if(depth_fps_probe_.ready(fps_probe_us)) {
            flush_depth_pending(cfg, logger);
        }
    }

    void flush_pending_media(const Config &cfg, Logger &logger) {
        flush_rgb_pending(cfg, logger);
        flush_depth_pending(cfg, logger);
    }

    void flush_rgb_pending(const Config &cfg, Logger &logger) {
        if(rgb_pipe_.active() || rgb_pending_.empty() || !rgb_pending_has_vcl_) {
            return;
        }
        rgb_record_fps_ = rgb_fps_probe_.estimate(30.0);
        ensure_rgb_pipe(cfg, rgb_record_fps_, logger);
        if(rgb_pipe_.active()) {
            logger.info("rgb record fps estimated: " + format_fps(rgb_record_fps_));
            rgb_pipe_.write(rgb_pending_.data(), rgb_pending_.size(), logger);
            rgb_pending_.clear();
        }
    }

    void flush_depth_pending(const Config &cfg, Logger &logger) {
        if(depth_pipe_.active() || depth_pending_.empty()) {
            return;
        }
        depth_record_fps_ = depth_fps_probe_.estimate(static_cast<double>(cfg.depth_fps));
        ensure_depth_pipe(cfg, depth_width_, depth_height_, depth_record_fps_, logger);
        if(depth_pipe_.active()) {
            logger.info("depth record fps estimated: " + format_fps(depth_record_fps_));
            for(const auto &payload : depth_pending_) {
                depth_pipe_.write(payload.data(), payload.size(), logger);
            }
            depth_pending_.clear();
            depth_pending_bytes_ = 0;
        }
    }

    void write_meta(const Config &cfg, const std::string &sender_id, const std::string &camera_id, const std::string &announce_json, bool closed) {
        if(directory_.empty()) {
            return;
        }
        const int rgb_requested_fps = json_int_in_object(announce_json, "rgb_profile", "fps").value_or(30);
        const int depth_requested_fps = json_int_in_object(announce_json, "depth_profile", "fps").value_or(cfg.depth_fps);
        const std::string rgb_codec = !rgb_stats_.codec_or_compression.empty()
                                          ? rgb_stats_.codec_or_compression
                                          : json_string_in_object(announce_json, "rgb_profile", "codec").value_or("h264");
        const uint32_t rgb_width = rgb_stats_.width > 0 ? rgb_stats_.width
                                                        : static_cast<uint32_t>(json_int_in_object(announce_json, "rgb_profile", "width").value_or(0));
        const uint32_t rgb_height = rgb_stats_.height > 0 ? rgb_stats_.height
                                                          : static_cast<uint32_t>(json_int_in_object(announce_json, "rgb_profile", "height").value_or(0));
        std::ofstream meta(std::filesystem::path(directory_) / "meta.json", std::ios::out | std::ios::trunc);
        meta << "{\n";
        meta << "  \"sender_id\": \"" << json_escape(sender_id) << "\",\n";
        meta << "  \"camera_id\": \"" << json_escape(camera_id) << "\",\n";
        meta << "  \"camera_key\": \"" << json_escape(camera_key(sender_id, camera_id)) << "\",\n";
        meta << "  \"segment_start_us\": " << start_us_ << ",\n";
        meta << "  \"segment_end_us\": " << (closed ? end_us_ : 0) << ",\n";
        meta << "  \"closed\": " << (closed ? "true" : "false") << ",\n";
        meta << "  \"rgb_file\": \"rgb.mp4\",\n";
        meta << "  \"rgb_debug_file\": \"rgb_debug.h264\",\n";
        meta << "  \"depth_file\": \"depth.mkv\",\n";
        meta << "  \"depth_debug_file\": \"depth_debug.raw\",\n";
        meta << "  \"frames_file\": \"frames.csv\",\n";
        meta << "  \"ffmpeg_log_file\": \"ffmpeg.log\",\n";
        meta << "  \"rgb_codec\": \"" << json_escape(rgb_codec) << "\",\n";
        meta << "  \"rgb_width\": " << rgb_width << ",\n";
        meta << "  \"rgb_height\": " << rgb_height << ",\n";
        meta << "  \"rgb_fps\": " << rgb_requested_fps << ",\n";
        meta << "  \"rgb_actual_fps\": " << format_fps(rgb_stats_.actual_fps()) << ",\n";
        meta << "  \"rgb_frames\": " << rgb_stats_.frames << ",\n";
        meta << "  \"depth_codec\": \"ffv1\",\n";
        meta << "  \"depth_pixel_format\": \"gray16le\",\n";
        meta << "  \"depth_dtype\": \"uint16le\",\n";
        meta << "  \"depth_fps\": " << depth_requested_fps << ",\n";
        meta << "  \"depth_actual_fps\": " << format_fps(depth_stats_.actual_fps()) << ",\n";
        meta << "  \"depth_frames\": " << depth_stats_.frames << ",\n";
        meta << "  \"depth_format\": \"ffv1_mkv_gray16le\",\n";
        meta << "  \"depth_width\": " << depth_width_ << ",\n";
        meta << "  \"depth_height\": " << depth_height_ << ",\n";
        meta << "  \"rgb_record_fps\": " << format_fps(rgb_record_fps_) << ",\n";
        meta << "  \"depth_record_fps\": " << format_fps(depth_record_fps_) << ",\n";
        meta << "  \"write_debug_h264\": " << (cfg.write_debug_h264 ? "true" : "false") << ",\n";
        meta << "  \"write_debug_depth_raw\": " << (cfg.write_debug_depth_raw ? "true" : "false") << ",\n";
        meta << "  \"camera_announce_raw\": \"" << json_escape(announce_json) << "\"\n";
        meta << "}\n";
    }

    void set_segment_mtime_to_start(Logger &logger) const {
        if(directory_.empty() || start_us_ == 0) {
            return;
        }

        std::vector<std::filesystem::path> paths;
        std::error_code ec;
        for(const auto &entry : std::filesystem::directory_iterator(directory_, ec)) {
            paths.push_back(entry.path());
        }
        if(ec) {
            logger.warn("cannot enumerate segment files for mtime update: " + directory_ + ": " + ec.message());
            return;
        }
        paths.emplace_back(directory_);

        timespec times[2]{};
        times[0].tv_sec = static_cast<time_t>(start_us_ / 1'000'000ull);
        times[0].tv_nsec = static_cast<long>((start_us_ % 1'000'000ull) * 1000ull);
        times[1] = times[0];

        for(const auto &path : paths) {
            if(utimensat(AT_FDCWD, path.c_str(), times, 0) != 0) {
                logger.warn("cannot set segment mtime to start: " + path.string() + ": " + std::strerror(errno));
            }
        }

        std::string cmd = "touch -d " + shell_quote("@" + std::to_string(start_us_ / 1'000'000ull)) + " --";
        for(const auto &path : paths) {
            cmd += " " + shell_quote(path.string());
        }
        const int rc = std::system(cmd.c_str());
        if(rc != 0) {
            logger.warn("touch command failed while setting segment mtime to start: " + directory_);
        }

        const auto delayed_paths = paths;
        const auto delayed_start_us = start_us_;
        std::thread([delayed_paths, delayed_start_us] {
            std::this_thread::sleep_for(std::chrono::seconds(2));

            timespec delayed_times[2]{};
            delayed_times[0].tv_sec = static_cast<time_t>(delayed_start_us / 1'000'000ull);
            delayed_times[0].tv_nsec = static_cast<long>((delayed_start_us % 1'000'000ull) * 1000ull);
            delayed_times[1] = delayed_times[0];
            for(const auto &path : delayed_paths) {
                utimensat(AT_FDCWD, path.c_str(), delayed_times, 0);
            }

            std::string delayed_cmd = "touch -d " + shell_quote("@" + std::to_string(delayed_start_us / 1'000'000ull)) + " --";
            for(const auto &path : delayed_paths) {
                delayed_cmd += " " + shell_quote(path.string());
            }
            std::system(delayed_cmd.c_str());
        }).detach();
    }

    bool active_ = false;
    uint64_t start_us_ = 0;
    uint64_t end_us_ = 0;
    std::chrono::steady_clock::time_point start_steady_{};
    std::string directory_;
    std::ofstream frames_csv_;
    std::ofstream rgb_debug_;
    std::ofstream depth_debug_;
    FfmpegPipe rgb_pipe_;
    FfmpegPipe depth_pipe_;
    std::vector<uint8_t> rgb_pending_;
    bool rgb_pending_has_vcl_ = false;
    std::vector<std::vector<uint8_t>> depth_pending_;
    size_t depth_pending_bytes_ = 0;
    uint32_t depth_width_ = 0;
    uint32_t depth_height_ = 0;
    FpsProbe rgb_fps_probe_;
    FpsProbe depth_fps_probe_;
    double rgb_record_fps_ = 0.0;
    double depth_record_fps_ = 0.0;
    StreamRecordStats rgb_stats_;
    StreamRecordStats depth_stats_;
    FrameInfo last_rgb_;
    FrameInfo last_depth_;
};

struct CameraState {
    explicit CameraState(std::string sender, std::string camera)
        : sender_id(std::move(sender)), camera_id(std::move(camera)), key(camera_key(sender_id, camera_id)) {}

    std::string sender_id;
    std::string camera_id;
    std::string key;
    bool online = true;
    bool recording_requested = false;
    uint64_t last_status_us = 0;
    uint64_t last_media_us = 0;
    uint64_t rgb_packets = 0;
    uint64_t depth_packets = 0;
    uint64_t rgb_bytes = 0;
    uint64_t depth_bytes = 0;
    uint64_t rgb_preview_us = 0;
    uint32_t rgb_preview_width = 0;
    uint32_t rgb_preview_height = 0;
    std::vector<uint8_t> rgb_preview_prefix_h264;
    std::unique_ptr<RgbPreviewDecoder> rgb_decoder;
    uint64_t depth_preview_us = 0;
    uint32_t depth_preview_width = 0;
    uint32_t depth_preview_height = 0;
    std::vector<uint8_t> depth_preview_ppm;
    std::string last_error;
    std::string last_announce_json;
    SegmentWriter segment;
};

class ReceiverApp {
public:
    explicit ReceiverApp(Config config) : config_(std::move(config)), logger_(config_.log_directory) {}

    void start() {
        running_ = true;
        udp_thread_ = std::thread([this] { udp_loop(); });
        tcp_thread_ = std::thread([this] { tcp_loop(); });
        admin_thread_ = std::thread([this] { admin_loop(); });
        logger_.info("receiver started: media tcp " + config_.media_bind_ip + ":" + std::to_string(config_.media_port) +
                     ", status udp " + config_.status_bind_ip + ":" + std::to_string(config_.status_port) + ", admin http " +
                     config_.admin_bind_ip + ":" + std::to_string(config_.admin_port));
    }

    void stop() {
        running_ = false;
        if(udp_thread_.joinable()) {
            udp_thread_.join();
        }
        if(tcp_thread_.joinable()) {
            tcp_thread_.join();
        }
        if(admin_thread_.joinable()) {
            admin_thread_.join();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        for(auto &item : cameras_) {
            if(item.second->rgb_decoder) {
                item.second->rgb_decoder->stop();
            }
            item.second->segment.close(config_, item.second->sender_id, item.second->camera_id, item.second->last_announce_json, logger_);
        }
        logger_.info("receiver stopped");
    }

    std::string status_json() {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto now = now_us();
        refresh_camera_liveness_locked(now);
        std::ostringstream out;
        out << "{";
        out << "\"running\":true,";
        out << "\"recording_all\":" << (recording_all_ ? "true" : "false") << ',';
        out << "\"nas_root\":\"" << json_escape(config_.nas_root) << "\",";
        out << "\"media_port\":" << config_.media_port << ',';
        out << "\"status_port\":" << config_.status_port << ',';
        out << "\"admin_port\":" << config_.admin_port << ',';
        out << "\"cameras\":[";
        bool first = true;
        for(const auto &item : cameras_) {
            const auto &cam = *item.second;
            const auto last_seen = camera_last_seen_us(cam);
            const bool live = cam.online && is_recent_us(now, last_seen, kCameraOnlineTimeoutUs);
            const bool rgb_preview_fresh = live && is_recent_us(now, cam.rgb_preview_us, kPreviewFreshUs);
            const bool depth_preview_fresh = live && is_recent_us(now, cam.depth_preview_us, kPreviewFreshUs);
            if(!first) {
                out << ',';
            }
            first = false;
            out << "{";
            out << "\"sender_id\":\"" << json_escape(cam.sender_id) << "\",";
            out << "\"camera_id\":\"" << json_escape(cam.camera_id) << "\",";
            out << "\"camera_key\":\"" << json_escape(cam.key) << "\",";
            out << "\"online\":" << (cam.online ? "true" : "false") << ',';
            out << "\"live\":" << (live ? "true" : "false") << ',';
            out << "\"recording\":" << ((cam.recording_requested || recording_all_) ? "true" : "false") << ',';
            out << "\"segment_active\":" << (cam.segment.active() ? "true" : "false") << ',';
            out << "\"segment_dir\":\"" << json_escape(cam.segment.directory()) << "\",";
            out << "\"last_status_us\":" << cam.last_status_us << ',';
            out << "\"last_media_us\":" << cam.last_media_us << ',';
            out << "\"last_seen_us\":" << last_seen << ',';
            out << "\"status_age_ms\":" << age_ms_or_negative(now, cam.last_status_us) << ',';
            out << "\"media_age_ms\":" << age_ms_or_negative(now, cam.last_media_us) << ',';
            out << "\"rgb_packets\":" << cam.rgb_packets << ',';
            out << "\"depth_packets\":" << cam.depth_packets << ',';
            out << "\"rgb_bytes\":" << cam.rgb_bytes << ',';
            out << "\"depth_bytes\":" << cam.depth_bytes << ',';
            out << "\"rgb_preview_available\":"
                << (rgb_preview_fresh && cam.rgb_decoder && cam.rgb_decoder->has_frame() ? "true" : "false") << ',';
            out << "\"rgb_preview_width\":" << cam.rgb_preview_width << ',';
            out << "\"rgb_preview_height\":" << cam.rgb_preview_height << ',';
            out << "\"rgb_preview_us\":" << cam.rgb_preview_us << ',';
            out << "\"rgb_preview_age_ms\":" << age_ms_or_negative(now, cam.rgb_preview_us) << ',';
            out << "\"depth_preview_available\":" << (depth_preview_fresh && !cam.depth_preview_ppm.empty() ? "true" : "false") << ',';
            out << "\"depth_preview_width\":" << cam.depth_preview_width << ',';
            out << "\"depth_preview_height\":" << cam.depth_preview_height << ',';
            out << "\"depth_preview_us\":" << cam.depth_preview_us << ',';
            out << "\"depth_preview_age_ms\":" << age_ms_or_negative(now, cam.depth_preview_us) << ',';
            out << "\"last_error\":\"" << json_escape(cam.last_error) << "\"";
            out << "}";
        }
        out << "]}";
        return out.str();
    }

    std::string config_json() const {
        std::ostringstream out;
        out << "{";
        out << "\"status_bind_ip\":\"" << json_escape(config_.status_bind_ip) << "\",";
        out << "\"status_port\":" << config_.status_port << ',';
        out << "\"media_bind_ip\":\"" << json_escape(config_.media_bind_ip) << "\",";
        out << "\"media_port\":" << config_.media_port << ',';
        out << "\"admin_bind_ip\":\"" << json_escape(config_.admin_bind_ip) << "\",";
        out << "\"admin_port\":" << config_.admin_port << ',';
        out << "\"nas_root\":\"" << json_escape(config_.nas_root) << "\",";
        out << "\"segment_seconds\":" << config_.segment_seconds;
        out << "}";
        return out.str();
    }

    std::string start_all() {
        std::lock_guard<std::mutex> lock(mutex_);
        refresh_camera_liveness_locked(now_us());
        recording_all_ = true;
        for(auto &item : cameras_) {
            item.second->recording_requested = true;
        }
        logger_.info("recording start-all requested");
        return "{\"ok\":true,\"recording_all\":true}";
    }

    std::string stop_all() {
        std::lock_guard<std::mutex> lock(mutex_);
        recording_all_ = false;
        for(auto &item : cameras_) {
            item.second->recording_requested = false;
            item.second->segment.close(config_, item.second->sender_id, item.second->camera_id, item.second->last_announce_json, logger_);
        }
        refresh_camera_liveness_locked(now_us());
        logger_.info("recording stop-all requested");
        return "{\"ok\":true,\"recording_all\":false}";
    }

    std::string start_camera(const std::string &sender_id, const std::string &camera_id) {
        if(sender_id.empty() || camera_id.empty()) {
            return "{\"ok\":false,\"error\":\"sender_id and camera_id are required\"}";
        }
        std::lock_guard<std::mutex> lock(mutex_);
        auto &cam = ensure_camera_locked(sender_id, camera_id);
        cam.recording_requested = true;
        logger_.info("recording start requested: " + cam.key);
        return "{\"ok\":true}";
    }

    std::string stop_camera(const std::string &sender_id, const std::string &camera_id) {
        if(sender_id.empty() || camera_id.empty()) {
            return "{\"ok\":false,\"error\":\"sender_id and camera_id are required\"}";
        }
        std::lock_guard<std::mutex> lock(mutex_);
        const auto key = camera_key(sender_id, camera_id);
        auto it = cameras_.find(key);
        if(it == cameras_.end()) {
            return "{\"ok\":false,\"error\":\"camera not found\"}";
        }
        it->second->recording_requested = false;
        it->second->segment.close(config_, it->second->sender_id, it->second->camera_id, it->second->last_announce_json, logger_);
        logger_.info("recording stop requested: " + key);
        return "{\"ok\":true}";
    }

    std::optional<std::vector<uint8_t>> depth_preview(const std::string &sender_id, const std::string &camera_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto now = now_us();
        refresh_camera_liveness_locked(now);
        const auto key = camera_key(sender_id, camera_id);
        auto it = cameras_.find(key);
        if(it == cameras_.end() || !it->second->online || !is_recent_us(now, it->second->depth_preview_us, kPreviewFreshUs) ||
           it->second->depth_preview_ppm.empty()) {
            return std::nullopt;
        }
        return it->second->depth_preview_ppm;
    }

    std::optional<std::vector<uint8_t>> rgb_preview(const std::string &sender_id, const std::string &camera_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto now = now_us();
        refresh_camera_liveness_locked(now);
        const auto key = camera_key(sender_id, camera_id);
        auto it = cameras_.find(key);
        if(it == cameras_.end() || !it->second->online || !is_recent_us(now, it->second->rgb_preview_us, kPreviewFreshUs) ||
           !it->second->rgb_decoder) {
            return std::nullopt;
        }
        return it->second->rgb_decoder->latest_jpeg();
    }

private:
    static uint64_t camera_last_seen_us(const CameraState &cam) {
        return std::max(cam.last_status_us, cam.last_media_us);
    }

    static bool is_recent_us(uint64_t now, uint64_t timestamp_us, uint64_t timeout_us) {
        return timestamp_us > 0 && (timestamp_us >= now || now - timestamp_us <= timeout_us);
    }

    static bool is_older_than_us(uint64_t now, uint64_t timestamp_us, uint64_t timeout_us) {
        return timestamp_us == 0 || (timestamp_us < now && now - timestamp_us > timeout_us);
    }

    static int64_t age_ms_or_negative(uint64_t now, uint64_t timestamp_us) {
        if(timestamp_us == 0) {
            return -1;
        }
        if(timestamp_us >= now) {
            return 0;
        }
        return static_cast<int64_t>((now - timestamp_us) / 1000ull);
    }

    void clear_camera_live_cache_locked(CameraState &cam) {
        cam.rgb_preview_prefix_h264.clear();
        if(cam.rgb_decoder) {
            cam.rgb_decoder->stop();
            cam.rgb_decoder.reset();
        }
        cam.rgb_preview_us = 0;
        cam.rgb_preview_width = 0;
        cam.rgb_preview_height = 0;
        cam.depth_preview_ppm.clear();
        cam.depth_preview_us = 0;
        cam.depth_preview_width = 0;
        cam.depth_preview_height = 0;
    }

    void refresh_camera_liveness_locked(uint64_t now) {
        for(auto it = cameras_.begin(); it != cameras_.end();) {
            auto &cam = *it->second;
            const auto last_seen = camera_last_seen_us(cam);
            if(is_older_than_us(now, last_seen, kCameraOnlineTimeoutUs)) {
                if(cam.online) {
                    cam.online = false;
                    cam.last_error = cam.last_error.empty() ? "receiver_timeout" : cam.last_error;
                    logger_.info("camera timed out: " + cam.key);
                }
                clear_camera_live_cache_locked(cam);
            }

            if(!cam.online && !cam.recording_requested && !cam.segment.active() &&
               is_older_than_us(now, last_seen, kOfflineCameraPurgeUs)) {
                if(cam.rgb_decoder) {
                    cam.rgb_decoder->stop();
                }
                logger_.info("camera purged: " + cam.key);
                it = cameras_.erase(it);
                continue;
            }
            ++it;
        }
    }

    CameraState &ensure_camera_locked(const std::string &sender_id, const std::string &camera_id) {
        const auto key = camera_key(sender_id, camera_id);
        auto it = cameras_.find(key);
        if(it == cameras_.end()) {
            auto state = std::make_unique<CameraState>(sender_id, camera_id);
            state->recording_requested = recording_all_;
            it = cameras_.emplace(key, std::move(state)).first;
            logger_.info("camera discovered: " + key);
        }
        it->second->online = true;
        return *it->second;
    }

    void handle_status_message(const std::string &payload) {
        const auto json = trim_copy(payload);
        const auto type = json_string_field(json, "message_type").value_or("unknown");
        const auto sender_id = json_string_field(json, "sender_id").value_or("");
        const auto camera_id = json_string_field(json, "camera_id").value_or("");

        if(!sender_id.empty() && !camera_id.empty()) {
            std::lock_guard<std::mutex> lock(mutex_);
            auto &cam = ensure_camera_locked(sender_id, camera_id);
            cam.last_status_us = now_us();
            if(type == "camera_announce") {
                cam.last_announce_json = json;
            }
            else if(type == "camera_offline") {
                cam.online = false;
                clear_camera_live_cache_locked(cam);
                cam.last_error = json_string_field(json, "reason").value_or("camera_offline");
            }
            else if(type == "event") {
                const auto code = json_string_field(json, "event_code").value_or("event");
                const auto message = json_string_field(json, "message").value_or("");
                cam.last_error = code + (message.empty() ? "" : ": " + message);
            }
        }

        logger_.info("status " + type + " sender=" + sender_id + (camera_id.empty() ? "" : " camera=" + camera_id));
    }

    void update_rgb_preview_locked(CameraState &cam, const MediaPacket &packet, bool has_idr, bool has_vcl) {
        if(packet.payload.empty()) {
            return;
        }

        if(!has_vcl) {
            if(cam.rgb_preview_prefix_h264.size() + packet.payload.size() > kMaxRgbPreviewPrefixBytes) {
                cam.rgb_preview_prefix_h264.clear();
            }
            cam.rgb_preview_prefix_h264.insert(cam.rgb_preview_prefix_h264.end(), packet.payload.begin(), packet.payload.end());
            return;
        }

        if(has_idr && (!cam.rgb_decoder || !cam.rgb_decoder->active())) {
            if(!cam.rgb_decoder) {
                cam.rgb_decoder = std::make_unique<RgbPreviewDecoder>();
            }
            cam.rgb_decoder->start(config_, cam.key, packet.width, packet.height, logger_);
            if(!cam.rgb_preview_prefix_h264.empty()) {
                cam.rgb_decoder->write_packet(cam.rgb_preview_prefix_h264);
            }
        }
        else if(!cam.rgb_decoder || !cam.rgb_decoder->active()) {
            return;
        }

        if(!cam.rgb_decoder->write_packet(packet.payload)) {
            cam.rgb_decoder.reset();
            return;
        }
        cam.rgb_preview_width = cam.rgb_decoder->preview_width();
        cam.rgb_preview_height = cam.rgb_decoder->preview_height();
        cam.rgb_preview_us = cam.rgb_decoder->frame_us();
    }

    void handle_media_packet(const MediaPacket &packet) {
        if(packet.sender_id.empty() || packet.camera_id.empty()) {
            logger_.warn("media packet with empty sender_id/camera_id ignored");
            return;
        }

        MediaPacket decoded_packet = packet;
        if(packet.stream_type == StreamType::depth_raw) {
            try {
                decoded_packet = normalized_depth_packet(packet);
            }
            catch(const std::exception &e) {
                logger_.warn(std::string("depth packet ignored: ") + e.what());
                return;
            }
        }

        PreviewImage preview;
        if(decoded_packet.stream_type == StreamType::depth_raw) {
            preview = build_depth_preview_bmp(decoded_packet.payload, decoded_packet.width, decoded_packet.height);
        }
        const bool rgb_has_idr = packet.stream_type == StreamType::rgb &&
                                 (((packet.flags & key_frame) != 0u) || h264_payload_has_nal_type(packet.payload, 5));
        const bool rgb_has_vcl = packet.stream_type == StreamType::rgb && h264_payload_has_vcl_nal(packet.payload);

        std::lock_guard<std::mutex> lock(mutex_);
        auto &cam = ensure_camera_locked(packet.sender_id, packet.camera_id);
        cam.last_media_us = now_us();
        if(packet.stream_type == StreamType::rgb) {
            cam.rgb_packets++;
            cam.rgb_bytes += packet.payload_size;
            update_rgb_preview_locked(cam, packet, rgb_has_idr, rgb_has_vcl);
        }
        else if(packet.stream_type == StreamType::depth_raw) {
            cam.depth_packets++;
            cam.depth_bytes += packet.payload_size;
            if(!preview.bytes.empty()) {
                cam.depth_preview_ppm = std::move(preview.bytes);
                cam.depth_preview_width = preview.width;
                cam.depth_preview_height = preview.height;
                cam.depth_preview_us = cam.last_media_us;
            }
        }

        if(recording_all_ || cam.recording_requested) {
            cam.segment.write_packet(config_, decoded_packet, cam.sender_id, cam.camera_id, cam.last_announce_json, logger_);
        }
    }

    void udp_loop() {
        const int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if(fd < 0) {
            logger_.error(std::string("cannot create UDP socket: ") + std::strerror(errno));
            return;
        }
        int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        set_socket_timeout(fd, 1);
        const auto addr = make_bind_addr(config_.status_bind_ip, config_.status_port);
        if(bind(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
            logger_.error(std::string("cannot bind status UDP: ") + std::strerror(errno));
            close(fd);
            return;
        }

        std::vector<char> buffer(65536);
        while(running_ && g_running) {
            sockaddr_in peer{};
            socklen_t peer_len = sizeof(peer);
            const ssize_t got = recvfrom(fd, buffer.data(), buffer.size() - 1, 0, reinterpret_cast<sockaddr *>(&peer), &peer_len);
            if(got < 0) {
                if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                    continue;
                }
                logger_.warn(std::string("UDP recv failed: ") + std::strerror(errno));
                continue;
            }
            buffer[static_cast<size_t>(got)] = '\0';
            handle_status_message(std::string(buffer.data(), static_cast<size_t>(got)));
        }
        close(fd);
    }

    void tcp_loop() {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        if(fd < 0) {
            logger_.error(std::string("cannot create TCP socket: ") + std::strerror(errno));
            return;
        }
        int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        set_socket_timeout(fd, 1);
        const auto addr = make_bind_addr(config_.media_bind_ip, config_.media_port);
        if(bind(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
            logger_.error(std::string("cannot bind media TCP: ") + std::strerror(errno));
            close(fd);
            return;
        }
        if(listen(fd, 16) != 0) {
            logger_.error(std::string("cannot listen media TCP: ") + std::strerror(errno));
            close(fd);
            return;
        }

        while(running_ && g_running) {
            sockaddr_in peer{};
            socklen_t peer_len = sizeof(peer);
            const int client = accept(fd, reinterpret_cast<sockaddr *>(&peer), &peer_len);
            if(client < 0) {
                if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                    continue;
                }
                logger_.warn(std::string("media accept failed: ") + std::strerror(errno));
                continue;
            }
            std::thread([this, client] { media_client_loop(client); }).detach();
        }
        close(fd);
    }

    void media_client_loop(int fd) {
        logger_.info("media client connected");
        while(running_ && g_running) {
            try {
                auto packet = read_media_packet(fd, config_.max_payload_bytes);
                handle_media_packet(packet);
            }
            catch(const std::exception &e) {
                if(std::string(e.what()) != "connection closed") {
                    logger_.warn(std::string("media client disconnected: ") + e.what());
                }
                break;
            }
        }
        close(fd);
    }

    void admin_loop() {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        if(fd < 0) {
            logger_.error(std::string("cannot create admin socket: ") + std::strerror(errno));
            return;
        }
        int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        set_socket_timeout(fd, 1);
        const auto addr = make_bind_addr(config_.admin_bind_ip, config_.admin_port);
        if(bind(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
            logger_.error(std::string("cannot bind admin HTTP: ") + std::strerror(errno));
            close(fd);
            return;
        }
        if(listen(fd, 16) != 0) {
            logger_.error(std::string("cannot listen admin HTTP: ") + std::strerror(errno));
            close(fd);
            return;
        }

        while(running_ && g_running) {
            const int client = accept(fd, nullptr, nullptr);
            if(client < 0) {
                if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                    continue;
                }
                logger_.warn(std::string("admin accept failed: ") + std::strerror(errno));
                continue;
            }
            handle_admin_client(client);
            close(client);
        }
        close(fd);
    }

    void handle_admin_client(int fd) {
        std::string request;
        char buffer[4096];
        while(request.find("\r\n\r\n") == std::string::npos && request.size() < 65536) {
            const ssize_t got = recv(fd, buffer, sizeof(buffer), 0);
            if(got <= 0) {
                return;
            }
            request.append(buffer, buffer + got);
        }

        std::istringstream input(request);
        std::string method;
        std::string target;
        std::string version;
        input >> method >> target >> version;
        std::string path = target;
        std::string query;
        const size_t qpos = target.find('?');
        if(qpos != std::string::npos) {
            path = target.substr(0, qpos);
            query = target.substr(qpos + 1);
        }
        const auto args = parse_query(query);

        int status = 200;
        std::string content_type = "application/json";
        std::string body;
        if(method == "GET" && path == "/api/status") {
            body = status_json();
        }
        else if(method == "GET" && path == "/api/config") {
            body = config_json();
        }
        else if(method == "POST" && path == "/api/record/start-all") {
            body = start_all();
        }
        else if(method == "POST" && path == "/api/record/stop-all") {
            body = stop_all();
        }
        else if(method == "POST" && path == "/api/record/start") {
            body = start_camera(args.count("sender_id") ? args.at("sender_id") : "", args.count("camera_id") ? args.at("camera_id") : "");
        }
        else if(method == "POST" && path == "/api/record/stop") {
            body = stop_camera(args.count("sender_id") ? args.at("sender_id") : "", args.count("camera_id") ? args.at("camera_id") : "");
        }
        else if(method == "GET" && path == "/api/preview/depth") {
            const auto preview = depth_preview(args.count("sender_id") ? args.at("sender_id") : "",
                                               args.count("camera_id") ? args.at("camera_id") : "");
            if(preview) {
                content_type = "image/bmp";
                body.assign(reinterpret_cast<const char *>(preview->data()), preview->size());
            }
            else {
                status = 404;
                body = "{\"ok\":false,\"error\":\"depth preview not found\"}";
            }
        }
        else if(method == "GET" && path == "/api/preview/rgb") {
            const auto preview = rgb_preview(args.count("sender_id") ? args.at("sender_id") : "",
                                             args.count("camera_id") ? args.at("camera_id") : "");
            if(preview) {
                content_type = "image/jpeg";
                body.assign(reinterpret_cast<const char *>(preview->data()), preview->size());
            }
            else {
                status = 404;
                body = "{\"ok\":false,\"error\":\"rgb preview not found\"}";
            }
        }
        else {
            status = 404;
            body = "{\"ok\":false,\"error\":\"not found\"}";
        }

        std::ostringstream response;
        response << "HTTP/1.1 " << status << (status == 200 ? " OK" : " Not Found") << "\r\n";
        response << "Content-Type: " << content_type << "\r\n";
        response << "Cache-Control: no-store\r\n";
        response << "Content-Length: " << body.size() << "\r\n";
        response << "Connection: close\r\n\r\n";
        response << body;
        const auto text = response.str();
        send(fd, text.data(), text.size(), MSG_NOSIGNAL);
    }

    Config config_;
    Logger logger_;
    std::atomic<bool> running_{false};
    std::thread udp_thread_;
    std::thread tcp_thread_;
    std::thread admin_thread_;
    std::mutex mutex_;
    bool recording_all_ = false;
    std::map<std::string, std::unique_ptr<CameraState>> cameras_;
};

struct Args {
    std::string config_path = "06_configs/receiver_ubuntu-01.json";
};

Args parse_args(int argc, char **argv) {
    Args args;
    for(int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if(arg == "--config" && i + 1 < argc) {
            args.config_path = argv[++i];
        }
        else {
            throw std::runtime_error("unknown argument: " + arg);
        }
    }
    return args;
}

}  // namespace
}  // namespace gwv3

int main(int argc, char **argv) {
    std::signal(SIGINT, gwv3::handle_signal);
    std::signal(SIGTERM, gwv3::handle_signal);
    std::signal(SIGPIPE, SIG_IGN);

    try {
        const auto args = gwv3::parse_args(argc, argv);
        auto config = gwv3::load_config(args.config_path);
        gwv3::ReceiverApp app(config);
        app.start();
        while(gwv3::g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        app.stop();
        return 0;
    }
    catch(const std::exception &e) {
        std::cerr << "receiver error: " << e.what() << std::endl;
        return 1;
    }
}
