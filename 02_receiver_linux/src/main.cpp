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
#include <poll.h>
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
constexpr uint32_t kRgbMainPreviewWidth = 0; // 0 keeps the source RGB resolution for the selected main preview.
constexpr uint32_t kRgbPreviewFps = 10;
constexpr uint32_t kRgbMainPreviewFps = 15;
constexpr int kRgbPreviewPipeBytes = 1024 * 1024;
constexpr int kRgbPreviewWritePollMs = 2;
constexpr int kRgbPreviewWriteBudgetMs = 12;
constexpr uint64_t kCameraOnlineTimeoutUs = 5ull * 1000ull * 1000ull;
constexpr uint64_t kOfflineCameraPurgeUs = 30ull * 1000ull * 1000ull;
constexpr uint64_t kPreviewFreshUs = 5ull * 1000ull * 1000ull;
constexpr uint32_t kRecordFpsProbeFrames = 60;
constexpr uint64_t kRecordFpsProbeMaxWaitUs = 3'000'000ull;
constexpr double kMinRecordFps = 5.0;
constexpr double kMaxRecordFps = 60.0;
constexpr size_t kMaxPendingRgbRecordBytes = 8ull * 1024ull * 1024ull;
constexpr size_t kMaxPendingDepthRecordBytes = 64ull * 1024ull * 1024ull;
constexpr int kMaxActiveMediaClients = 32;
constexpr int kMediaClientSocketTimeoutSec = 2;
constexpr uint64_t kAnnounceCacheSaveMinIntervalUs = 60ull * 1000ull * 1000ull;
constexpr uint64_t kRoutineStatusLogMinIntervalUs = 60ull * 1000ull * 1000ull;

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

std::string local_time_text_from_us(uint64_t epoch_us, const char *format) {
    const auto seconds = static_cast<time_t>(epoch_us / 1'000'000ull);
    std::tm tm{};
    localtime_r(&seconds, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, format);
    return oss.str();
}

std::string date_dir_from_us(uint64_t epoch_us) {
    return local_time_text_from_us(epoch_us, "%Y-%m-%d");
}

std::string time_dir_from_us(uint64_t epoch_us) {
    return local_time_text_from_us(epoch_us, "%H%M%S");
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

std::optional<uint64_t> json_uint64_field(const std::string &json, const std::string &key) {
    const std::regex pattern("\"" + key + "\"\\s*:\\s*([0-9]+)");
    std::smatch match;
    if(std::regex_search(json, match, pattern) && match.size() >= 2) {
        try {
            return static_cast<uint64_t>(std::stoull(match[1].str()));
        }
        catch(const std::exception &) {
            return std::nullopt;
        }
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

std::string json_object_or_empty(const std::string &json, const std::string &key) {
    return json_object_field(json, key).value_or("{}");
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

void set_fd_cloexec(int fd) {
    if(fd < 0) {
        return;
    }
    const int flags = fcntl(fd, F_GETFD, 0);
    if(flags >= 0) {
        fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }
}

void set_fd_nonblocking(int fd) {
    if(fd < 0) {
        return;
    }
    const int flags = fcntl(fd, F_GETFL, 0);
    if(flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

void set_pipe_size_if_supported(int fd, int bytes) {
    if(fd < 0 || bytes <= 0) {
        return;
    }
#ifdef F_SETPIPE_SZ
    fcntl(fd, F_SETPIPE_SZ, bytes);
#else
    (void)bytes;
#endif
}

bool wait_fd_writable(int fd, int timeout_ms) {
    if(fd < 0) {
        return false;
    }
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLOUT;
    int rc = 0;
    do {
        rc = poll(&pfd, 1, timeout_ms);
    } while(rc < 0 && errno == EINTR);
    return rc > 0 && (pfd.revents & POLLOUT) != 0;
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

std::string socket_endpoint(const sockaddr_in &addr) {
    char ip[INET_ADDRSTRLEN] = {};
    if(inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip)) == nullptr) {
        std::strncpy(ip, "unknown", sizeof(ip) - 1);
    }
    std::ostringstream out;
    out << ip << ':' << ntohs(addr.sin_port);
    return out.str();
}

std::string camera_key(const std::string &sender_id, const std::string &camera_id) {
    return sender_id + "_" + camera_id;
}

bool is_safe_storage_text(const std::string &value) {
    if(value == "." || value == "..") {
        return false;
    }
    for(unsigned char ch : value) {
        if(ch < 0x20 || ch == 0x7f) {
            return false;
        }
        if(ch >= 0x80) {
            continue;
        }
        if(std::isalnum(ch) || ch == '_' || ch == '-' || ch == '.') {
            continue;
        }
        return false;
    }
    return true;
}

std::optional<std::string> storage_text_error(const std::string &field, const std::string &value) {
    if(is_safe_storage_text(value)) {
        return std::nullopt;
    }
    return field + " only allows Chinese/letters/digits/_/-. and must not contain path or control characters";
}

std::string prefixed_filename(const std::string &prefix, const std::string &basename) {
    return prefix + basename;
}

std::string json_error(const std::string &error) {
    return "{\"ok\":false,\"error\":\"" + json_escape(error) + "\"}";
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

std::optional<size_t> h264_decodable_start_offset(const std::vector<uint8_t> &payload) {
    size_t candidate_start = std::string::npos;
    bool has_sps = false;
    bool has_pps = false;
    for(size_t i = 0; i + 4 < payload.size(); ++i) {
        size_t nal_offset = std::string::npos;
        size_t start_offset = i;
        if(payload[i] == 0 && payload[i + 1] == 0 && payload[i + 2] == 1) {
            nal_offset = i + 3;
        }
        else if(i + 4 < payload.size() && payload[i] == 0 && payload[i + 1] == 0 && payload[i + 2] == 0 && payload[i + 3] == 1) {
            nal_offset = i + 4;
        }
        if(nal_offset == std::string::npos || nal_offset >= payload.size()) {
            continue;
        }

        const uint8_t nal_type = payload[nal_offset] & 0x1fu;
        if(nal_type == 7) {
            candidate_start = start_offset;
            has_sps = true;
            has_pps = false;
        }
        else if(nal_type == 8 && has_sps) {
            has_pps = true;
        }
        else if(nal_type == 5 && has_sps && has_pps && candidate_start != std::string::npos) {
            return candidate_start;
        }
        else if(nal_type >= 1 && nal_type <= 5) {
            candidate_start = std::string::npos;
            has_sps = false;
            has_pps = false;
        }
    }
    return std::nullopt;
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

constexpr uint16_t kDepthPreviewMinMm = 250;
constexpr uint16_t kDepthPreviewMaxMm = 2500;

uint8_t clamp_color(double value) {
    if(value <= 0.0) {
        return 0;
    }
    if(value >= 255.0) {
        return 255;
    }
    return static_cast<uint8_t>(value);
}

void append_depth_color(std::vector<uint8_t> &out, uint16_t value) {
    if(value == 0 || kDepthPreviewMaxMm <= kDepthPreviewMinMm) {
        out.push_back(12);
        out.push_back(16);
        out.push_back(24);
        return;
    }
    const double clamped = std::clamp(static_cast<double>(value), static_cast<double>(kDepthPreviewMinMm),
                                      static_cast<double>(kDepthPreviewMaxMm));
    const double t = (clamped - static_cast<double>(kDepthPreviewMinMm))
                     / static_cast<double>(kDepthPreviewMaxMm - kDepthPreviewMinMm);
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

    std::vector<uint8_t> rgb;
    rgb.reserve(static_cast<size_t>(image.width) * image.height * 3ull);

    for(uint32_t y = 0; y < height && y / stride < image.height; y += stride) {
        for(uint32_t x = 0; x < width && x / stride < image.width; x += stride) {
            const size_t offset = (static_cast<size_t>(y) * width + x) * 2ull;
            const uint16_t value = static_cast<uint16_t>(payload[offset]) | (static_cast<uint16_t>(payload[offset + 1]) << 8u);
            append_depth_color(rgb, value);
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
    std::string state_path = "06_configs/receiver_runtime_state.json";
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
    cfg.state_path = config_string(json, "state_path", cfg.state_path);
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

struct RuntimeState {
    std::string default_file_prefix;
    std::map<std::string, std::string> camera_names;
    std::map<std::string, std::string> camera_file_prefixes;
    std::map<std::string, std::string> camera_announces;
};

std::map<std::string, std::string> json_string_map_field(const std::string &json, const std::string &key) {
    std::map<std::string, std::string> result;
    const auto object = json_object_field(json, key);
    if(!object) {
        return result;
    }
    const std::regex pair_pattern("\"([^\"]+)\"\\s*:\\s*\"([^\"]*)\"");
    for(auto it = std::sregex_iterator(object->begin(), object->end(), pair_pattern); it != std::sregex_iterator(); ++it) {
        if(it->size() >= 3) {
            result[(*it)[1].str()] = (*it)[2].str();
        }
    }
    return result;
}

std::map<std::string, std::string> json_object_map_field(const std::string &json, const std::string &key) {
    std::map<std::string, std::string> result;
    const auto object = json_object_field(json, key);
    if(!object) {
        return result;
    }

    size_t pos = 0;
    while(pos < object->size()) {
        const size_t key_start = object->find('"', pos);
        if(key_start == std::string::npos) {
            break;
        }
        bool escaped = false;
        size_t key_end = std::string::npos;
        for(size_t i = key_start + 1; i < object->size(); ++i) {
            const char ch = (*object)[i];
            if(escaped) {
                escaped = false;
            }
            else if(ch == '\\') {
                escaped = true;
            }
            else if(ch == '"') {
                key_end = i;
                break;
            }
        }
        if(key_end == std::string::npos) {
            break;
        }
        const std::string item_key = object->substr(key_start + 1, key_end - key_start - 1);
        const size_t colon = object->find(':', key_end + 1);
        if(colon == std::string::npos) {
            break;
        }
        const size_t value_start = object->find('{', colon + 1);
        if(value_start == std::string::npos) {
            break;
        }

        bool in_string = false;
        escaped = false;
        int depth = 0;
        for(size_t i = value_start; i < object->size(); ++i) {
            const char ch = (*object)[i];
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
                    result[item_key] = object->substr(value_start, i - value_start + 1);
                    pos = i + 1;
                    break;
                }
            }
        }
        if(depth != 0) {
            break;
        }
    }
    return result;
}

RuntimeState load_runtime_state(const std::string &path) {
    RuntimeState state;
    std::ifstream input(path);
    if(!input) {
        return state;
    }
    const std::string json((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    state.default_file_prefix = json_string_field(json, "default_file_prefix").value_or("");
    state.camera_names = json_string_map_field(json, "camera_names");
    state.camera_file_prefixes = json_string_map_field(json, "camera_file_prefixes");
    state.camera_announces = json_object_map_field(json, "camera_announces");
    if(!is_safe_storage_text(state.default_file_prefix)) {
        state.default_file_prefix.clear();
    }
    for(auto it = state.camera_names.begin(); it != state.camera_names.end();) {
        if(!is_safe_storage_text(it->second) || it->second.empty()) {
            it = state.camera_names.erase(it);
        }
        else {
            ++it;
        }
    }
    for(auto it = state.camera_file_prefixes.begin(); it != state.camera_file_prefixes.end();) {
        if(!is_safe_storage_text(it->second) || it->second.empty()) {
            it = state.camera_file_prefixes.erase(it);
        }
        else {
            ++it;
        }
    }
    return state;
}

void save_runtime_state_file(const std::string &path, const RuntimeState &state) {
    const auto state_path = std::filesystem::path(path);
    const auto parent = state_path.parent_path();
    if(!parent.empty()) {
        std::filesystem::create_directories(parent);
    }
    const auto tmp_path = state_path.string() + ".tmp";
    std::ofstream out(tmp_path, std::ios::out | std::ios::trunc);
    if(!out) {
        throw std::runtime_error("cannot write receiver state: " + tmp_path);
    }
    out << "{\n";
    out << "  \"default_file_prefix\": \"" << json_escape(state.default_file_prefix) << "\",\n";
    out << "  \"camera_names\": {\n";
    bool first = true;
    for(const auto &item : state.camera_names) {
        if(!first) {
            out << ",\n";
        }
        first = false;
        out << "    \"" << json_escape(item.first) << "\": \"" << json_escape(item.second) << "\"";
    }
    out << "\n  },\n";
    out << "  \"camera_file_prefixes\": {\n";
    first = true;
    for(const auto &item : state.camera_file_prefixes) {
        if(!first) {
            out << ",\n";
        }
        first = false;
        out << "    \"" << json_escape(item.first) << "\": \"" << json_escape(item.second) << "\"";
    }
    out << "\n  },\n";
    out << "  \"camera_announces\": {\n";
    first = true;
    for(const auto &item : state.camera_announces) {
        if(!first) {
            out << ",\n";
        }
        first = false;
        out << "    \"" << json_escape(item.first) << "\": " << item.second;
    }
    out << "\n  }\n";
    out << "}\n";
    out.close();
    std::filesystem::rename(tmp_path, state_path);
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

    bool start(const Config &cfg,
               const std::string &key,
               uint32_t width,
               uint32_t height,
               uint32_t target_width,
               uint32_t preview_fps,
               Logger &logger) {
        stop();
        key_ = key;
        source_width_ = width;
        source_height_ = height;
        preview_width_ = target_width == 0 ? width : (width > 0 ? std::min<uint32_t>(width, target_width) : target_width);
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

            const std::string scale = target_width == 0 ? "fps=" + std::to_string(preview_fps)
                                                        : "fps=" + std::to_string(preview_fps) + ",scale=" + std::to_string(target_width) + ":-2";
            execlp(cfg.ffmpeg_path.c_str(), cfg.ffmpeg_path.c_str(), "-hide_banner", "-loglevel", "error", "-fflags", "nobuffer",
                   "-flags", "low_delay", "-probesize", "32", "-analyzeduration", "0", "-avioflags", "direct", "-f", "h264", "-i", "pipe:0",
                   "-vf", scale.c_str(), "-q:v", "3", "-f", "image2pipe", "-vcodec", "mjpeg", "pipe:1", static_cast<char *>(nullptr));
            _exit(127);
        }

        close(stdin_pipe[0]);
        close(stdout_pipe[1]);
        stdin_fd_ = stdin_pipe[1];
        stdout_fd_ = stdout_pipe[0];
        set_pipe_size_if_supported(stdin_fd_, kRgbPreviewPipeBytes);
        set_fd_nonblocking(stdin_fd_);
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
        int stdout_fd = -1;
        pid_t pid = -1;
        {
            std::lock_guard<std::mutex> lock(process_mutex_);
            running_ = false;
            stdin_fd = stdin_fd_;
            stdout_fd = stdout_fd_;
            pid = pid_;
            stdin_fd_ = -1;
            stdout_fd_ = -1;
            pid_ = -1;
        }
        if(stdin_fd >= 0) {
            close(stdin_fd);
        }
        if(pid > 0) {
            kill(pid, SIGTERM);
        }
        if(stdout_fd >= 0) {
            close(stdout_fd);
        }
        if(reader_.joinable()) {
            reader_.join();
        }
        if(pid > 0) {
            int status = 0;
            for(int i = 0; i < 10; ++i) {
                const pid_t done = waitpid(pid, &status, WNOHANG);
                if(done == pid || (done < 0 && errno == ECHILD)) {
                    return;
                }
                usleep(50 * 1000);
            }
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
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
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kRgbPreviewWriteBudgetMs);
        while(offset < payload.size()) {
            const ssize_t written = write(stdin_fd_, payload.data() + offset, payload.size() - offset);
            if(written < 0 && errno == EINTR) {
                continue;
            }
            if(written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                if(std::chrono::steady_clock::now() < deadline && wait_fd_writable(stdin_fd_, kRgbPreviewWritePollMs)) {
                    continue;
                }
                break;
            }
            if(written <= 0) {
                break;
            }
            offset += static_cast<size_t>(written);
        }
        if(offset == payload.size()) {
            return true;
        }
        running_ = false;
        close(stdin_fd_);
        stdin_fd_ = -1;
        return false;
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

void cleanup_rgb_decoder_async(std::unique_ptr<RgbPreviewDecoder> decoder) {
    if(!decoder) {
        return;
    }
    std::thread([decoder = std::move(decoder)]() mutable {
        decoder->stop();
        decoder.reset();
    }).detach();
}

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

    uint64_t start_us() const {
        return start_us_;
    }

    void start(const Config &cfg, const std::string &sender_id, const std::string &camera_id, const std::string &camera_name,
               const std::string &storage_key, const std::string &file_prefix, const std::string &announce_json, Logger &logger) {
        close(cfg, sender_id, camera_id, announce_json, logger);
        start_us_ = now_us();
        start_steady_ = std::chrono::steady_clock::now();
        camera_name_ = camera_name;
        storage_key_ = storage_key.empty() ? camera_key(sender_id, camera_id) : storage_key;
        file_prefix_ = file_prefix;
        directory_ = (std::filesystem::path(cfg.nas_root) / storage_key_ / date_dir_from_us(start_us_) / time_dir_from_us(start_us_)).string();
        std::filesystem::create_directories(directory_);

        frames_csv_.open(file_path("frames.csv"), std::ios::out | std::ios::trunc);
        frames_csv_ << "local_time_us,stream_type,rgb_frame_id,rgb_timestamp_us,depth_frame_id,depth_timestamp_us,pair_id,pair_delta_ms,width,height,payload_size,"
                       "packet_system_timestamp_us,rgb_system_timestamp_us,depth_system_timestamp_us,frame_id,timestamp_us,frame_system_timestamp_us,"
                       "codec_or_compression\n";

        if(cfg.write_debug_h264) {
            rgb_debug_.open(file_path("rgb_debug.h264"), std::ios::binary | std::ios::out | std::ios::trunc);
        }
        if(cfg.write_debug_depth_raw) {
            depth_debug_.open(file_path("depth_debug.raw"), std::ios::binary | std::ios::out | std::ios::trunc);
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
        schedule_retime_completed_media(cfg);
        if(rgb_rc != 0) {
            logger.warn("rgb ffmpeg exited with non-zero status for segment: " + directory_);
        }
        if(depth_rc != 0) {
            logger.warn("depth ffmpeg exited with non-zero status for segment: " + directory_);
        }
        logger.info("recording segment closed: " + directory_);
        active_ = false;
        directory_.clear();
        camera_name_.clear();
        storage_key_.clear();
        file_prefix_.clear();
        depth_width_ = 0;
        depth_height_ = 0;
        last_rgb_ = {};
        last_depth_ = {};
        rgb_pending_.clear();
        rgb_pending_has_vcl_ = false;
        rgb_pending_has_decodable_start_ = false;
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
                      const std::string &camera_name, const std::string &storage_key, const std::string &file_prefix,
                      const std::string &announce_json, Logger &logger) {
        if(!active_) {
            start(cfg, sender_id, camera_id, camera_name, storage_key, file_prefix, announce_json, logger);
        }
        if(should_rotate(cfg)) {
            close(cfg, sender_id, camera_id, announce_json, logger);
            start(cfg, sender_id, camera_id, camera_name, storage_key, file_prefix, announce_json, logger);
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
    std::filesystem::path file_path(const std::string &basename) const {
        return std::filesystem::path(directory_) / prefixed_filename(file_prefix_, basename);
    }

    void ensure_depth_pipe(const Config &cfg, uint32_t width, uint32_t height, double fps, Logger &logger) {
        if(depth_pipe_.active()) {
            return;
        }
        depth_width_ = width;
        depth_height_ = height;
        const auto ffmpeg_log = shell_quote(file_path("ffmpeg.log").string());
        const auto depth_mkv = shell_quote(file_path("depth.mkv").string());
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
        const auto ffmpeg_log = shell_quote(file_path("ffmpeg.log").string());
        const auto rgb_mp4 = shell_quote(file_path("rgb.mp4").string());
        const std::string rgb_cmd = shell_quote(cfg.ffmpeg_path) +
                                    " -hide_banner -loglevel warning -y -fflags +genpts -r " + format_fps(fps) +
                                    " -f h264 -i pipe:0 -c:v copy -movflags +faststart " + rgb_mp4 +
                                    " 2>>" + ffmpeg_log;
        rgb_pipe_.open(rgb_cmd, logger);
    }

    void write_rgb_packet(const Config &cfg, const MediaPacket &packet, uint64_t packet_local_us, Logger &logger) {
        if(rgb_pipe_.active()) {
            rgb_pipe_.write(packet.payload.data(), packet.payload.size(), logger);
            return;
        }

        if(rgb_pending_has_decodable_start_ && rgb_pending_.size() + packet.payload.size() > kMaxPendingRgbRecordBytes) {
            flush_rgb_pending(cfg, logger);
        }
        if(rgb_pipe_.active()) {
            rgb_pipe_.write(packet.payload.data(), packet.payload.size(), logger);
            return;
        }
        if(rgb_pending_.size() + packet.payload.size() > kMaxPendingRgbRecordBytes) {
            rgb_pending_.clear();
            rgb_pending_has_vcl_ = false;
            rgb_pending_has_decodable_start_ = false;
            rgb_fps_probe_.reset();
        }
        rgb_pending_.insert(rgb_pending_.end(), packet.payload.begin(), packet.payload.end());

        if(h264_payload_has_vcl_nal(packet.payload)) {
            rgb_pending_has_vcl_ = true;
            const uint64_t fps_probe_us = packet.system_timestamp_us > 0 ? packet.system_timestamp_us : packet_local_us;
            rgb_fps_probe_.add(fps_probe_us);
        }
        if(!rgb_pending_has_decodable_start_) {
            if(const auto decodable_start = h264_decodable_start_offset(rgb_pending_)) {
                if(*decodable_start > 0) {
                    rgb_pending_.erase(rgb_pending_.begin(), rgb_pending_.begin() + static_cast<std::ptrdiff_t>(*decodable_start));
                }
                rgb_pending_has_decodable_start_ = true;
            }
        }

        const uint64_t ready_probe_us = packet.system_timestamp_us > 0 ? packet.system_timestamp_us : packet_local_us;
        if(rgb_pending_has_decodable_start_ && rgb_fps_probe_.ready(ready_probe_us)) {
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
        if(rgb_pipe_.active() || rgb_pending_.empty() || !rgb_pending_has_decodable_start_) {
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

    struct RetimingEntry {
        std::filesystem::path media_path;
        std::string stream_name;
        double target_duration_seconds = 0.0;
        double fallback_scale = 1.0;
    };

    struct RetimingTask {
        std::string ffmpeg_path;
        std::string ffprobe_path;
        std::filesystem::path log_path;
        uint64_t start_us = 0;
        std::vector<RetimingEntry> entries;
    };

    static double media_duration_seconds(const StreamRecordStats &stats) {
        if(stats.frames < 2 || stats.last_local_us <= stats.first_local_us) {
            return 0.0;
        }
        return static_cast<double>(stats.last_local_us - stats.first_local_us) / 1'000'000.0;
    }

    static double media_retime_scale(double record_fps, const StreamRecordStats &stats) {
        const double actual_fps = stats.actual_fps();
        if(record_fps <= 0.0 || actual_fps <= 0.0 || stats.frames < 2) {
            return 1.0;
        }
        return record_fps / actual_fps;
    }

    static bool should_retime_media(double scale) {
        return std::isfinite(scale) && scale >= 0.8 && scale <= 1.25 && std::fabs(scale - 1.0) >= 0.001;
    }

    static std::string format_scale(double scale) {
        std::ostringstream out;
        out << std::fixed << std::setprecision(9) << scale;
        return out.str();
    }

    static std::string ffprobe_path_from_ffmpeg(const std::string &ffmpeg_path) {
        const std::filesystem::path path(ffmpeg_path);
        if(path.filename() == "ffmpeg") {
            return (path.parent_path() / "ffprobe").string();
        }
        return "ffprobe";
    }

    static std::optional<double> probe_media_duration_seconds(const std::string &ffprobe_path, const std::filesystem::path &media_path) {
        const std::string cmd = shell_quote(ffprobe_path) +
                                " -v error -show_entries format=duration -of default=nk=1:nw=1 " + shell_quote(media_path.string()) +
                                " 2>/dev/null";
        FILE *pipe = popen(cmd.c_str(), "r");
        if(!pipe) {
            return std::nullopt;
        }
        std::string output;
        char buffer[128];
        while(fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            output += buffer;
        }
        const int rc = pclose(pipe);
        if(rc != 0) {
            return std::nullopt;
        }
        try {
            const double duration = std::stod(trim_copy(output));
            if(std::isfinite(duration) && duration > 0.0) {
                return duration;
            }
        }
        catch(...) {
        }
        return std::nullopt;
    }

    static void append_retime_log(const std::filesystem::path &log_path, const std::string &message) {
        std::ofstream log(log_path, std::ios::out | std::ios::app);
        if(log) {
            log << timestamp_text() << " [retime] " << message << '\n';
        }
    }

    static void set_file_mtime_to_start(const std::filesystem::path &path, uint64_t start_us) {
        if(start_us == 0 || path.empty()) {
            return;
        }
        timespec times[2]{};
        times[0].tv_sec = static_cast<time_t>(start_us / 1'000'000ull);
        times[0].tv_nsec = static_cast<long>((start_us % 1'000'000ull) * 1000ull);
        times[1] = times[0];
        utimensat(AT_FDCWD, path.c_str(), times, 0);
    }

    static bool retime_media_file(const RetimingTask &task, const RetimingEntry &entry) {
        if(!std::filesystem::exists(entry.media_path)) {
            append_retime_log(task.log_path, entry.stream_name + " retime skipped: media file missing");
            return false;
        }

        double scale = entry.fallback_scale;
        if(const auto current_duration = probe_media_duration_seconds(task.ffprobe_path, entry.media_path)) {
            if(entry.target_duration_seconds > 0.0) {
                scale = entry.target_duration_seconds / *current_duration;
            }
            append_retime_log(task.log_path, entry.stream_name + " duration current=" + format_scale(*current_duration) +
                                                 " target=" + format_scale(entry.target_duration_seconds) +
                                                 " scale=" + format_scale(scale));
        }
        else {
            append_retime_log(task.log_path, entry.stream_name + " duration probe failed; fallback scale=" + format_scale(scale));
        }
        if(!should_retime_media(scale)) {
            append_retime_log(task.log_path, entry.stream_name + " retime skipped: scale near 1 or outside 0.8..1.25");
            return false;
        }

        const auto parent = entry.media_path.parent_path();
        const auto tmp_path = parent / (entry.media_path.stem().string() + ".retime_tmp" + entry.media_path.extension().string());
        std::error_code ec;
        std::filesystem::remove(tmp_path, ec);

        append_retime_log(task.log_path, entry.stream_name + " retime start scale=" + format_scale(scale));
        const std::string cmd = shell_quote(task.ffmpeg_path) + " -hide_banner -loglevel warning -y -itsscale " + format_scale(scale) +
                                " -i " + shell_quote(entry.media_path.string()) + " -map 0 -c copy -avoid_negative_ts make_zero " +
                                shell_quote(tmp_path.string()) + " 2>>" + shell_quote(task.log_path.string());
        const int rc = std::system(cmd.c_str());
        if(rc != 0 || !std::filesystem::exists(tmp_path) || std::filesystem::file_size(tmp_path, ec) == 0) {
            append_retime_log(task.log_path, entry.stream_name + " retime failed");
            std::filesystem::remove(tmp_path, ec);
            return false;
        }

        std::filesystem::rename(tmp_path, entry.media_path, ec);
        if(ec) {
            append_retime_log(task.log_path, entry.stream_name + " retime replace failed: " + ec.message());
            std::filesystem::remove(tmp_path, ec);
            return false;
        }
        set_file_mtime_to_start(entry.media_path, task.start_us);
        append_retime_log(task.log_path, entry.stream_name + " retime done");
        return true;
    }

    static void run_retime_task(RetimingTask task) {
        for(const auto &entry : task.entries) {
            retime_media_file(task, entry);
        }
        set_file_mtime_to_start(task.log_path, task.start_us);
    }

    void schedule_retime_completed_media(const Config &cfg) const {
        RetimingTask task;
        task.ffmpeg_path = cfg.ffmpeg_path;
        task.ffprobe_path = ffprobe_path_from_ffmpeg(cfg.ffmpeg_path);
        task.log_path = file_path("ffmpeg.log");
        task.start_us = start_us_;

        const double rgb_scale = media_retime_scale(rgb_record_fps_, rgb_stats_);
        const double rgb_duration = media_duration_seconds(rgb_stats_);
        if(rgb_duration > 0.0) {
            task.entries.push_back(RetimingEntry{file_path("rgb.mp4"), "rgb", rgb_duration, rgb_scale});
        }
        const double depth_scale = media_retime_scale(depth_record_fps_, depth_stats_);
        const double depth_duration = media_duration_seconds(depth_stats_);
        if(depth_duration > 0.0) {
            task.entries.push_back(RetimingEntry{file_path("depth.mkv"), "depth", depth_duration, depth_scale});
        }
        if(task.entries.empty()) {
            return;
        }
        std::thread([task = std::move(task)]() mutable { run_retime_task(std::move(task)); }).detach();
    }

    void write_meta(const Config &cfg, const std::string &sender_id, const std::string &camera_id, const std::string &announce_json, bool closed) {
        if(directory_.empty()) {
            return;
        }
        write_calibration(sender_id, camera_id, announce_json, closed);
        const int rgb_requested_fps = json_int_in_object(announce_json, "rgb_profile", "fps").value_or(30);
        const int depth_requested_fps = json_int_in_object(announce_json, "depth_profile", "fps").value_or(cfg.depth_fps);
        const std::string rgb_codec = !rgb_stats_.codec_or_compression.empty()
                                          ? rgb_stats_.codec_or_compression
                                          : json_string_in_object(announce_json, "rgb_profile", "codec").value_or("h264");
        const uint32_t rgb_width = rgb_stats_.width > 0 ? rgb_stats_.width
                                                        : static_cast<uint32_t>(json_int_in_object(announce_json, "rgb_profile", "width").value_or(0));
        const uint32_t rgb_height = rgb_stats_.height > 0 ? rgb_stats_.height
                                                          : static_cast<uint32_t>(json_int_in_object(announce_json, "rgb_profile", "height").value_or(0));
        std::ofstream meta(file_path("meta.json"), std::ios::out | std::ios::trunc);
        meta << "{\n";
        meta << "  \"sender_id\": \"" << json_escape(sender_id) << "\",\n";
        meta << "  \"camera_id\": \"" << json_escape(camera_id) << "\",\n";
        meta << "  \"camera_key\": \"" << json_escape(camera_key(sender_id, camera_id)) << "\",\n";
        meta << "  \"camera_name\": \"" << json_escape(camera_name_) << "\",\n";
        meta << "  \"storage_key\": \"" << json_escape(storage_key_) << "\",\n";
        meta << "  \"file_prefix\": \"" << json_escape(file_prefix_) << "\",\n";
        meta << "  \"segment_start_us\": " << start_us_ << ",\n";
        meta << "  \"segment_end_us\": " << (closed ? end_us_ : 0) << ",\n";
        meta << "  \"closed\": " << (closed ? "true" : "false") << ",\n";
        meta << "  \"rgb_file\": \"" << json_escape(prefixed_filename(file_prefix_, "rgb.mp4")) << "\",\n";
        meta << "  \"rgb_debug_file\": \"" << json_escape(prefixed_filename(file_prefix_, "rgb_debug.h264")) << "\",\n";
        meta << "  \"depth_file\": \"" << json_escape(prefixed_filename(file_prefix_, "depth.mkv")) << "\",\n";
        meta << "  \"depth_debug_file\": \"" << json_escape(prefixed_filename(file_prefix_, "depth_debug.raw")) << "\",\n";
        meta << "  \"frames_file\": \"" << json_escape(prefixed_filename(file_prefix_, "frames.csv")) << "\",\n";
        meta << "  \"calibration_file\": \"" << json_escape(prefixed_filename(file_prefix_, "calibration.json")) << "\",\n";
        meta << "  \"ffmpeg_log_file\": \"" << json_escape(prefixed_filename(file_prefix_, "ffmpeg.log")) << "\",\n";
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
        meta << "  \"rgb_playback_fps\": " << format_fps(rgb_stats_.actual_fps()) << ",\n";
        meta << "  \"rgb_target_duration_sec\": " << format_fps(media_duration_seconds(rgb_stats_)) << ",\n";
        meta << "  \"rgb_retime_scale\": " << format_fps(media_retime_scale(rgb_record_fps_, rgb_stats_)) << ",\n";
        meta << "  \"depth_record_fps\": " << format_fps(depth_record_fps_) << ",\n";
        meta << "  \"depth_playback_fps\": " << format_fps(depth_stats_.actual_fps()) << ",\n";
        meta << "  \"depth_target_duration_sec\": " << format_fps(media_duration_seconds(depth_stats_)) << ",\n";
        meta << "  \"depth_retime_scale\": " << format_fps(media_retime_scale(depth_record_fps_, depth_stats_)) << ",\n";
        meta << "  \"write_debug_h264\": " << (cfg.write_debug_h264 ? "true" : "false") << ",\n";
        meta << "  \"write_debug_depth_raw\": " << (cfg.write_debug_depth_raw ? "true" : "false") << ",\n";
        meta << "  \"camera_announce_raw\": \"" << json_escape(announce_json) << "\"\n";
        meta << "}\n";
    }

    void write_calibration(const std::string &sender_id, const std::string &camera_id, const std::string &announce_json, bool closed) const {
        if(directory_.empty()) {
            return;
        }
        const auto announce_timestamp_us = json_uint64_field(announce_json, "timestamp_us");
        const std::string device = json_object_or_empty(announce_json, "device");
        const std::string rgb_profile = json_object_or_empty(announce_json, "rgb_profile");
        const std::string depth_profile = json_object_or_empty(announce_json, "depth_profile");
        const std::string calibration = json_object_field(announce_json, "calibration")
                                            .value_or("{\"available\":false,\"source\":\"missing_camera_announce\",\"data\":{}}");

        std::ofstream out(file_path("calibration.json"), std::ios::out | std::ios::trunc);
        out << "{\n";
        out << "  \"schema\": \"gemini_calibration_v1\",\n";
        out << "  \"sender_id\": \"" << json_escape(sender_id) << "\",\n";
        out << "  \"camera_id\": \"" << json_escape(camera_id) << "\",\n";
        out << "  \"camera_key\": \"" << json_escape(camera_key(sender_id, camera_id)) << "\",\n";
        out << "  \"camera_name\": \"" << json_escape(camera_name_) << "\",\n";
        out << "  \"storage_key\": \"" << json_escape(storage_key_) << "\",\n";
        out << "  \"file_prefix\": \"" << json_escape(file_prefix_) << "\",\n";
        out << "  \"segment_start_us\": " << start_us_ << ",\n";
        out << "  \"segment_end_us\": " << (closed ? end_us_ : 0) << ",\n";
        out << "  \"closed\": " << (closed ? "true" : "false") << ",\n";
        if(announce_timestamp_us) {
            out << "  \"announce_timestamp_us\": " << *announce_timestamp_us << ",\n";
        }
        else {
            out << "  \"announce_timestamp_us\": 0,\n";
        }
        out << "  \"alignment_mode\": \"raw_depth_to_rgb_offline\",\n";
        out << "  \"depth_master\": \"depth_raw\",\n";
        out << "  \"aligned_depth\": {\n";
        out << "    \"generated\": false,\n";
        out << "    \"target\": \"rgb\",\n";
        out << "    \"output_file\": \"" << json_escape(prefixed_filename(file_prefix_, "depth_aligned_to_rgb.mkv")) << "\",\n";
        out << "    \"method\": \"offline\"\n";
        out << "  },\n";
        out << "  \"device\": " << device << ",\n";
        out << "  \"rgb_profile\": " << rgb_profile << ",\n";
        out << "  \"depth_profile\": " << depth_profile << ",\n";
        out << "  \"calibration\": " << calibration << ",\n";
        out << "  \"camera_announce_raw\": \"" << json_escape(announce_json) << "\"\n";
        out << "}\n";
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
        const auto segment_dir = std::filesystem::path(directory_);
        const auto date_dir = segment_dir.parent_path();
        const auto camera_dir = date_dir.parent_path();
        if(!date_dir.empty()) {
            paths.emplace_back(date_dir);
        }
        if(!camera_dir.empty()) {
            paths.emplace_back(camera_dir);
        }

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
    std::string camera_name_;
    std::string storage_key_;
    std::string file_prefix_;
    std::ofstream frames_csv_;
    std::ofstream rgb_debug_;
    std::ofstream depth_debug_;
    FfmpegPipe rgb_pipe_;
    FfmpegPipe depth_pipe_;
    std::vector<uint8_t> rgb_pending_;
    bool rgb_pending_has_vcl_ = false;
    bool rgb_pending_has_decodable_start_ = false;
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
    std::string camera_name;
    std::string camera_file_prefix;
    uint64_t recording_start_us = 0;
    std::string recording_file_prefix;
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
    uint64_t main_rgb_preview_us = 0;
    uint32_t main_rgb_preview_width = 0;
    uint32_t main_rgb_preview_height = 0;
    std::unique_ptr<RgbPreviewDecoder> main_rgb_decoder;
    uint64_t depth_preview_us = 0;
    uint32_t depth_preview_width = 0;
    uint32_t depth_preview_height = 0;
    std::vector<uint8_t> depth_preview_ppm;
    std::string last_error;
    std::string last_announce_json;
    bool last_announce_live = false;
    uint64_t last_announce_received_us = 0;
    uint64_t last_announce_cache_save_us = 0;
    uint64_t last_status_log_us = 0;
    std::mutex segment_mutex;
    bool segment_active = false;
    bool segment_finalizing = false;
    std::string segment_dir;
    SegmentWriter segment;

    std::string storage_key() const {
        return camera_name.empty() ? key : camera_name;
    }
};

class ReceiverApp {
public:
    explicit ReceiverApp(Config config) : config_(std::move(config)), logger_(config_.log_directory), runtime_state_(load_runtime_state(config_.state_path)) {
        logger_.info("receiver state loaded: " + config_.state_path);
    }

    struct SegmentCloseTask {
        std::shared_ptr<CameraState> cam;
        std::string sender_id;
        std::string camera_id;
        std::string announce_json;
    };

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
        std::vector<SegmentCloseTask> close_tasks;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for(auto &item : cameras_) {
                cleanup_rgb_decoder_async(std::move(item.second->rgb_decoder));
                cleanup_rgb_decoder_async(std::move(item.second->main_rgb_decoder));
                close_tasks.push_back({item.second, item.second->sender_id, item.second->camera_id,
                                       item.second->last_announce_live ? item.second->last_announce_json : ""});
            }
        }
        for(auto &task : close_tasks) {
            std::lock_guard<std::mutex> segment_lock(task.cam->segment_mutex);
            task.cam->segment.close(config_, task.sender_id, task.camera_id, task.announce_json, logger_);
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
        out << "\"recording_start_us\":" << recording_all_start_us_ << ',';
        out << "\"default_file_prefix\":\"" << json_escape(runtime_state_.default_file_prefix) << "\",";
        out << "\"file_prefix_scope\":\"per_camera\",";
        out << "\"nas_root\":\"" << json_escape(config_.nas_root) << "\",";
        out << "\"media_port\":" << config_.media_port << ',';
        out << "\"active_media_clients\":" << active_media_clients_.load() << ',';
        out << "\"status_port\":" << config_.status_port << ',';
        out << "\"admin_port\":" << config_.admin_port << ',';
        out << "\"main_preview_camera_key\":\"" << json_escape(main_preview_key_) << "\",";
        out << "\"cameras\":[";
        bool first = true;
        for(const auto &item : cameras_) {
            const auto &cam = *item.second;
            const auto last_seen = camera_last_seen_us(cam);
            const bool status_live = cam.online && is_recent_us(now, cam.last_status_us, kCameraOnlineTimeoutUs);
            const bool media_live = cam.online && is_recent_us(now, cam.last_media_us, kCameraOnlineTimeoutUs);
            const bool live = media_live;
            const bool rgb_preview_fresh = media_live && is_recent_us(now, cam.rgb_preview_us, kPreviewFreshUs);
            const bool main_rgb_preview_fresh = media_live && is_recent_us(now, cam.main_rgb_preview_us, kPreviewFreshUs);
            const bool depth_preview_fresh = media_live && is_recent_us(now, cam.depth_preview_us, kPreviewFreshUs);
            const auto calibration_json = json_object_field(cam.last_announce_json, "calibration").value_or("");
            const bool cached_calibration_available = json_bool_field(calibration_json, "available").value_or(false);
            const bool calibration_available = cam.last_announce_live && cached_calibration_available;
            const int announce_rgb_width = json_int_in_object(cam.last_announce_json, "rgb_profile", "width").value_or(0);
            const int announce_rgb_height = json_int_in_object(cam.last_announce_json, "rgb_profile", "height").value_or(0);
            const int announce_depth_width = json_int_in_object(cam.last_announce_json, "depth_profile", "width").value_or(0);
            const int announce_depth_height = json_int_in_object(cam.last_announce_json, "depth_profile", "height").value_or(0);
            const auto announce_timestamp_us = json_uint64_field(cam.last_announce_json, "timestamp_us").value_or(0);
            if(!first) {
                out << ',';
            }
            first = false;
            out << "{";
            out << "\"sender_id\":\"" << json_escape(cam.sender_id) << "\",";
            out << "\"camera_id\":\"" << json_escape(cam.camera_id) << "\",";
            out << "\"camera_key\":\"" << json_escape(cam.key) << "\",";
            out << "\"camera_name\":\"" << json_escape(cam.camera_name) << "\",";
            out << "\"storage_key\":\"" << json_escape(cam.storage_key()) << "\",";
            out << "\"camera_file_prefix\":\"" << json_escape(cam.camera_file_prefix) << "\",";
            out << "\"online\":" << (cam.online ? "true" : "false") << ',';
            out << "\"status_live\":" << (status_live ? "true" : "false") << ',';
            out << "\"media_live\":" << (media_live ? "true" : "false") << ',';
            out << "\"live\":" << (live ? "true" : "false") << ',';
            out << "\"recording\":" << ((cam.recording_requested || recording_all_) ? "true" : "false") << ',';
            out << "\"recording_start_us\":" << cam.recording_start_us << ',';
            out << "\"file_prefix\":\"" << json_escape(cam.recording_file_prefix) << "\",";
            out << "\"segment_active\":" << (cam.segment_active ? "true" : "false") << ',';
            out << "\"segment_finalizing\":" << (cam.segment_finalizing ? "true" : "false") << ',';
            out << "\"segment_dir\":\"" << json_escape(cam.segment_dir) << "\",";
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
            out << "\"main_rgb_preview_available\":"
                << (cam.key == main_preview_key_ && main_rgb_preview_fresh && cam.main_rgb_decoder && cam.main_rgb_decoder->has_frame() ? "true" : "false")
                << ',';
            out << "\"main_rgb_preview_width\":" << cam.main_rgb_preview_width << ',';
            out << "\"main_rgb_preview_height\":" << cam.main_rgb_preview_height << ',';
            out << "\"main_rgb_preview_us\":" << cam.main_rgb_preview_us << ',';
            out << "\"main_rgb_preview_age_ms\":" << age_ms_or_negative(now, cam.main_rgb_preview_us) << ',';
            out << "\"depth_preview_available\":" << (depth_preview_fresh && !cam.depth_preview_ppm.empty() ? "true" : "false") << ',';
            out << "\"depth_preview_width\":" << cam.depth_preview_width << ',';
            out << "\"depth_preview_height\":" << cam.depth_preview_height << ',';
            out << "\"depth_preview_us\":" << cam.depth_preview_us << ',';
            out << "\"depth_preview_age_ms\":" << age_ms_or_negative(now, cam.depth_preview_us) << ',';
            out << "\"calibration_available\":" << (calibration_available ? "true" : "false") << ',';
            out << "\"cached_calibration_available\":" << (cached_calibration_available ? "true" : "false") << ',';
            out << "\"announce_live\":" << (cam.last_announce_live ? "true" : "false") << ',';
            out << "\"announce_received_us\":" << cam.last_announce_received_us << ',';
            out << "\"announce_timestamp_us\":" << announce_timestamp_us << ',';
            out << "\"announce_rgb_width\":" << announce_rgb_width << ',';
            out << "\"announce_rgb_height\":" << announce_rgb_height << ',';
            out << "\"announce_depth_width\":" << announce_depth_width << ',';
            out << "\"announce_depth_height\":" << announce_depth_height << ',';
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
        out << "\"state_path\":\"" << json_escape(config_.state_path) << "\",";
        out << "\"default_file_prefix\":\"" << json_escape(runtime_state_.default_file_prefix) << "\",";
        out << "\"file_prefix_scope\":\"per_camera\",";
        out << "\"segment_seconds\":" << config_.segment_seconds;
        out << "}";
        return out.str();
    }

    std::string effective_file_prefix_locked(const CameraState &cam) const {
        if(recording_all_ && recording_all_has_file_prefix_override_) {
            return recording_all_file_prefix_;
        }
        return cam.camera_file_prefix;
    }

    void close_segments_async(std::vector<SegmentCloseTask> close_tasks, const std::string &done_log_message) {
        if(close_tasks.empty()) {
            return;
        }
        std::thread([this, close_tasks = std::move(close_tasks), done_log_message] {
            for(auto &task : close_tasks) {
                bool segment_active = false;
                std::string segment_dir;
                {
                    std::lock_guard<std::mutex> segment_lock(task.cam->segment_mutex);
                    task.cam->segment.close(config_, task.sender_id, task.camera_id, task.announce_json, logger_);
                    segment_active = task.cam->segment.active();
                    segment_dir = task.cam->segment.directory();
                }
                std::lock_guard<std::mutex> lock(mutex_);
                task.cam->segment_active = segment_active;
                task.cam->segment_finalizing = false;
                task.cam->segment_dir = std::move(segment_dir);
            }
            logger_.info(done_log_message);
        }).detach();
    }

    std::string start_all(const std::optional<std::string> &file_prefix_override) {
        if(file_prefix_override) {
            if(const auto error = storage_text_error("file_prefix", *file_prefix_override)) {
                return json_error(*error);
            }
        }
        std::lock_guard<std::mutex> lock(mutex_);
        refresh_camera_liveness_locked(now_us());
        const bool already_recording = recording_all_;
        if(!already_recording) {
            recording_all_start_us_ = now_us();
            recording_all_has_file_prefix_override_ = file_prefix_override.has_value();
            recording_all_file_prefix_ = file_prefix_override.value_or("");
        }
        recording_all_ = true;
        for(auto &item : cameras_) {
            if(!item.second->recording_requested && !item.second->segment_active) {
                item.second->recording_start_us = recording_all_start_us_;
                item.second->recording_file_prefix = effective_file_prefix_locked(*item.second);
            }
            item.second->recording_requested = true;
        }
        logger_.info("recording start-all requested");
        std::ostringstream out;
        out << "{\"ok\":true,\"recording_all\":true,\"recording_start_us\":" << recording_all_start_us_
            << ",\"file_prefix_scope\":\"" << (recording_all_has_file_prefix_override_ ? "override_all" : "per_camera") << "\"}";
        return out.str();
    }

    std::string stop_all() {
        std::vector<SegmentCloseTask> close_tasks;
        uint64_t recording_start_us = 0;
        bool finalizing = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            recording_start_us = recording_all_start_us_;
            if(recording_start_us == 0) {
                for(const auto &item : cameras_) {
                    if(item.second->recording_start_us > 0 &&
                       (recording_start_us == 0 || item.second->recording_start_us < recording_start_us)) {
                        recording_start_us = item.second->recording_start_us;
                    }
                }
            }
            recording_all_ = false;
            recording_all_start_us_ = 0;
            recording_all_has_file_prefix_override_ = false;
            recording_all_file_prefix_.clear();
            for(auto &item : cameras_) {
                item.second->recording_requested = false;
                item.second->recording_start_us = 0;
                item.second->recording_file_prefix.clear();
                if(item.second->segment_active) {
                    item.second->segment_finalizing = true;
                    finalizing = true;
                }
                close_tasks.push_back({item.second, item.second->sender_id, item.second->camera_id,
                                       item.second->last_announce_live ? item.second->last_announce_json : ""});
            }
            refresh_camera_liveness_locked(now_us());
        }
        logger_.info("recording stop-all requested");
        close_segments_async(std::move(close_tasks), "recording stop-all finalized");
        std::ostringstream out;
        out << "{\"ok\":true,\"recording_all\":false,\"recording_start_us\":" << recording_start_us << ",\"finalizing\":"
            << (finalizing ? "true" : "false") << "}";
        return out.str();
    }

    std::string start_camera(const std::string &sender_id, const std::string &camera_id, const std::optional<std::string> &file_prefix_override) {
        if(sender_id.empty() || camera_id.empty()) {
            return "{\"ok\":false,\"error\":\"sender_id and camera_id are required\"}";
        }
        if(file_prefix_override) {
            if(const auto error = storage_text_error("file_prefix", *file_prefix_override)) {
                return json_error(*error);
            }
        }
        std::lock_guard<std::mutex> lock(mutex_);
        auto &cam = ensure_camera_locked(sender_id, camera_id);
        if(!recording_all_ && !cam.recording_requested && !cam.segment_active) {
            cam.recording_start_us = now_us();
            cam.recording_file_prefix = file_prefix_override.value_or(cam.camera_file_prefix);
        }
        else if(cam.recording_start_us == 0) {
            cam.recording_start_us = recording_all_ ? recording_all_start_us_ : now_us();
            cam.recording_file_prefix = recording_all_ ? effective_file_prefix_locked(cam) : file_prefix_override.value_or(cam.camera_file_prefix);
        }
        cam.recording_requested = true;
        logger_.info("recording start requested: " + cam.key);
        std::ostringstream out;
        out << "{\"ok\":true,\"recording_start_us\":" << cam.recording_start_us << ",\"file_prefix\":\""
            << json_escape(cam.recording_file_prefix) << "\"}";
        return out.str();
    }

    std::string stop_camera(const std::string &sender_id, const std::string &camera_id) {
        if(sender_id.empty() || camera_id.empty()) {
            return "{\"ok\":false,\"error\":\"sender_id and camera_id are required\"}";
        }
        std::shared_ptr<CameraState> cam;
        std::string announce_json;
        uint64_t recording_start_us = 0;
        const auto key = camera_key(sender_id, camera_id);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = cameras_.find(key);
            if(it == cameras_.end()) {
                return "{\"ok\":false,\"error\":\"camera not found\"}";
            }
            cam = it->second;
            announce_json = cam->last_announce_live ? cam->last_announce_json : "";
            recording_start_us = cam->recording_start_us;
            cam->recording_requested = false;
            if(cam->segment_active) {
                cam->segment_finalizing = true;
            }
            if(recording_all_) {
                cam->recording_start_us = recording_all_start_us_;
                cam->recording_file_prefix = effective_file_prefix_locked(*cam);
            }
            else {
                cam->recording_start_us = 0;
                cam->recording_file_prefix.clear();
            }
        }
        logger_.info("recording stop requested: " + key);
        std::vector<SegmentCloseTask> close_tasks;
        close_tasks.push_back({cam, cam->sender_id, cam->camera_id, announce_json});
        close_segments_async(std::move(close_tasks), "recording stop finalized: " + key);
        std::ostringstream out;
        out << "{\"ok\":true,\"recording_start_us\":" << recording_start_us << ",\"finalizing\":true}";
        return out.str();
    }

    std::string set_camera_name(const std::string &sender_id, const std::string &camera_id, const std::string &camera_name) {
        if(sender_id.empty() || camera_id.empty()) {
            return "{\"ok\":false,\"error\":\"sender_id and camera_id are required\"}";
        }
        if(const auto error = storage_text_error("camera_name", camera_name)) {
            return json_error(*error);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        const auto key = camera_key(sender_id, camera_id);
        if(camera_name.empty()) {
            runtime_state_.camera_names.erase(key);
        }
        else {
            runtime_state_.camera_names[key] = camera_name;
        }
        auto it = cameras_.find(key);
        if(it != cameras_.end()) {
            it->second->camera_name = camera_name;
        }
        try {
            save_runtime_state_file(config_.state_path, runtime_state_);
        }
        catch(const std::exception &e) {
            logger_.error(e.what());
            return json_error(e.what());
        }
        logger_.info("camera name updated: " + key + " -> " + (camera_name.empty() ? key : camera_name));
        std::ostringstream out;
        out << "{\"ok\":true,\"sender_id\":\"" << json_escape(sender_id) << "\",\"camera_id\":\"" << json_escape(camera_id)
            << "\",\"camera_key\":\"" << json_escape(key) << "\",\"camera_name\":\"" << json_escape(camera_name)
            << "\",\"storage_key\":\"" << json_escape(camera_name.empty() ? key : camera_name) << "\"}";
        return out.str();
    }

    std::string set_camera_file_prefix(const std::string &sender_id, const std::string &camera_id, const std::string &file_prefix) {
        if(sender_id.empty() || camera_id.empty()) {
            return "{\"ok\":false,\"error\":\"sender_id and camera_id are required\"}";
        }
        if(const auto error = storage_text_error("file_prefix", file_prefix)) {
            return json_error(*error);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        const auto key = camera_key(sender_id, camera_id);
        if(file_prefix.empty()) {
            runtime_state_.camera_file_prefixes.erase(key);
        }
        else {
            runtime_state_.camera_file_prefixes[key] = file_prefix;
        }
        auto it = cameras_.find(key);
        if(it != cameras_.end()) {
            it->second->camera_file_prefix = file_prefix;
        }
        try {
            save_runtime_state_file(config_.state_path, runtime_state_);
        }
        catch(const std::exception &e) {
            logger_.error(e.what());
            return json_error(e.what());
        }
        logger_.info("camera file prefix updated: " + key + " -> " + file_prefix);
        std::ostringstream out;
        out << "{\"ok\":true,\"sender_id\":\"" << json_escape(sender_id) << "\",\"camera_id\":\"" << json_escape(camera_id)
            << "\",\"camera_key\":\"" << json_escape(key) << "\",\"camera_file_prefix\":\"" << json_escape(file_prefix) << "\"}";
        return out.str();
    }

    std::string set_default_file_prefix(const std::string &file_prefix) {
        if(const auto error = storage_text_error("file_prefix", file_prefix)) {
            return json_error(*error);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        runtime_state_.default_file_prefix = file_prefix;
        try {
            save_runtime_state_file(config_.state_path, runtime_state_);
        }
        catch(const std::exception &e) {
            logger_.error(e.what());
            return json_error(e.what());
        }
        logger_.info("default file prefix updated: " + file_prefix);
        return "{\"ok\":true,\"default_file_prefix\":\"" + json_escape(file_prefix) + "\"}";
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

    std::string set_main_preview_target(const std::string &sender_id, const std::string &camera_id) {
        if(sender_id.empty() || camera_id.empty()) {
            return "{\"ok\":false,\"error\":\"sender_id and camera_id are required\"}";
        }
        std::lock_guard<std::mutex> lock(mutex_);
        const auto key = camera_key(sender_id, camera_id);
        auto it = cameras_.find(key);
        if(it == cameras_.end()) {
            return "{\"ok\":false,\"error\":\"camera not found\"}";
        }
        if(main_preview_key_ != key) {
            for(auto &item : cameras_) {
                if(item.first != key) {
                    cleanup_rgb_decoder_async(std::move(item.second->main_rgb_decoder));
                    item.second->main_rgb_preview_us = 0;
                    item.second->main_rgb_preview_width = 0;
                    item.second->main_rgb_preview_height = 0;
                }
            }
        }
        main_preview_key_ = key;
        std::ostringstream out;
        out << "{\"ok\":true,\"main_preview_camera_key\":\"" << json_escape(main_preview_key_) << "\"}";
        return out.str();
    }

    std::optional<std::vector<uint8_t>> main_rgb_preview(const std::string &sender_id, const std::string &camera_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto now = now_us();
        refresh_camera_liveness_locked(now);
        const auto key = camera_key(sender_id, camera_id);
        auto it = cameras_.find(key);
        if(it == cameras_.end() || !it->second->online || !is_recent_us(now, it->second->last_media_us, kCameraOnlineTimeoutUs)) {
            return std::nullopt;
        }
        if(main_preview_key_.empty()) {
            main_preview_key_ = key;
        }
        auto &cam = *it->second;
        if(key == main_preview_key_ && is_recent_us(now, cam.main_rgb_preview_us, kPreviewFreshUs) && cam.main_rgb_decoder) {
            const auto jpeg = cam.main_rgb_decoder->latest_jpeg();
            if(jpeg) {
                return jpeg;
            }
        }
        if(is_recent_us(now, cam.rgb_preview_us, kPreviewFreshUs) && cam.rgb_decoder) {
            return cam.rgb_decoder->latest_jpeg();
        }
        return std::nullopt;
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
        cleanup_rgb_decoder_async(std::move(cam.rgb_decoder));
        cleanup_rgb_decoder_async(std::move(cam.main_rgb_decoder));
        cam.rgb_preview_us = 0;
        cam.rgb_preview_width = 0;
        cam.rgb_preview_height = 0;
        cam.main_rgb_preview_us = 0;
        cam.main_rgb_preview_width = 0;
        cam.main_rgb_preview_height = 0;
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

            if(!cam.online && !cam.recording_requested && !cam.segment_active &&
               is_older_than_us(now, last_seen, kOfflineCameraPurgeUs)) {
                cleanup_rgb_decoder_async(std::move(cam.rgb_decoder));
                cleanup_rgb_decoder_async(std::move(cam.main_rgb_decoder));
                logger_.info("camera purged: " + cam.key);
                it = cameras_.erase(it);
                continue;
            }
            ++it;
        }
    }

    std::shared_ptr<CameraState> ensure_camera_ptr_locked(const std::string &sender_id, const std::string &camera_id,
                                                          bool mark_online = true) {
        const auto key = camera_key(sender_id, camera_id);
        auto it = cameras_.find(key);
        if(it == cameras_.end()) {
            auto state = std::make_shared<CameraState>(sender_id, camera_id);
            const auto name = runtime_state_.camera_names.find(key);
            if(name != runtime_state_.camera_names.end()) {
                state->camera_name = name->second;
            }
            const auto prefix = runtime_state_.camera_file_prefixes.find(key);
            if(prefix != runtime_state_.camera_file_prefixes.end()) {
                state->camera_file_prefix = prefix->second;
            }
            const auto announce = runtime_state_.camera_announces.find(key);
            if(announce != runtime_state_.camera_announces.end()) {
                state->last_announce_json = announce->second;
            }
            state->recording_requested = recording_all_;
            if(recording_all_) {
                state->recording_start_us = recording_all_start_us_;
                state->recording_file_prefix = effective_file_prefix_locked(*state);
            }
            it = cameras_.emplace(key, std::move(state)).first;
            logger_.info("camera discovered: " + key);
        }
        if(mark_online) {
            it->second->online = true;
        }
        return it->second;
    }

    CameraState &ensure_camera_locked(const std::string &sender_id, const std::string &camera_id) {
        return *ensure_camera_ptr_locked(sender_id, camera_id);
    }

    void handle_status_message(const std::string &payload, const std::string &peer_endpoint) {
        const auto json = trim_copy(payload);
        const auto type = json_string_field(json, "message_type").value_or("unknown");
        const auto sender_id = json_string_field(json, "sender_id").value_or("");
        const auto camera_id = json_string_field(json, "camera_id").value_or("");
        bool should_log_status = type != "heartbeat" && type != "sender_hello";

        if(!sender_id.empty() && !camera_id.empty()) {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto key = camera_key(sender_id, camera_id);
            const auto code = type == "event" ? json_string_field(json, "event_code").value_or("event") : "";
            const bool marks_online = type == "camera_announce" || code == "camera_connected" || code == "camera_reconnected";
            auto &cam = *ensure_camera_ptr_locked(sender_id, camera_id, marks_online);
            const auto received_us = now_us();
            cam.last_status_us = received_us;
            if(type == "heartbeat" || type == "camera_announce" || type == "camera_offline") {
                should_log_status = cam.last_status_log_us == 0 ||
                                    received_us >= cam.last_status_log_us + kRoutineStatusLogMinIntervalUs;
                if(should_log_status) {
                    cam.last_status_log_us = received_us;
                }
            }
            if(type == "camera_announce") {
                cam.last_announce_json = json;
                cam.last_announce_live = true;
                cam.last_announce_received_us = received_us;
                const bool should_save_announce =
                    (runtime_state_.camera_announces.find(key) == runtime_state_.camera_announces.end() ||
                     runtime_state_.camera_announces[key] != json) &&
                    (cam.last_announce_cache_save_us == 0 ||
                     received_us >= cam.last_announce_cache_save_us + kAnnounceCacheSaveMinIntervalUs);
                if(should_save_announce) {
                    runtime_state_.camera_announces[key] = json;
                    cam.last_announce_cache_save_us = received_us;
                    try {
                        save_runtime_state_file(config_.state_path, runtime_state_);
                    }
                    catch(const std::exception &e) {
                        logger_.error(e.what());
                    }
                }
            }
            else if(type == "camera_offline") {
                cam.online = false;
                cam.last_announce_live = false;
                cam.last_announce_received_us = 0;
                clear_camera_live_cache_locked(cam);
                cam.last_error = json_string_field(json, "reason").value_or("camera_offline");
            }
            else if(type == "event") {
                const auto message = json_string_field(json, "message").value_or("");
                cam.last_error = code + (message.empty() ? "" : ": " + message);
                if(code == "camera_unavailable" || code == "camera_disconnected") {
                    cam.online = false;
                    cam.last_announce_live = false;
                    cam.last_announce_received_us = 0;
                    clear_camera_live_cache_locked(cam);
                }
            }
        }

        if(should_log_status) {
            logger_.info("status " + type + " from=" + peer_endpoint + " sender=" + sender_id
                         + (camera_id.empty() ? "" : " camera=" + camera_id));
        }
    }

    bool feed_rgb_preview_decoder_locked(CameraState &cam,
                                         const MediaPacket &packet,
                                         bool has_idr,
                                         std::unique_ptr<RgbPreviewDecoder> &decoder,
                                         uint32_t target_width,
                                         uint32_t preview_fps,
                                         const std::string &decoder_key,
                                         uint32_t &preview_width,
                                         uint32_t &preview_height,
                                         uint64_t &preview_us) {
        if(has_idr && (!decoder || !decoder->active())) {
            if(!decoder) {
                decoder = std::make_unique<RgbPreviewDecoder>();
            }
            decoder->start(config_, decoder_key, packet.width, packet.height, target_width, preview_fps, logger_);
            if(!cam.rgb_preview_prefix_h264.empty()) {
                if(!decoder->write_packet(cam.rgb_preview_prefix_h264)) {
                    cleanup_rgb_decoder_async(std::move(decoder));
                    preview_width = 0;
                    preview_height = 0;
                    preview_us = 0;
                    return false;
                }
            }
        }
        else if(!decoder || !decoder->active()) {
            return false;
        }

        if(!decoder->write_packet(packet.payload)) {
            cleanup_rgb_decoder_async(std::move(decoder));
            preview_width = 0;
            preview_height = 0;
            preview_us = 0;
            return false;
        }
        preview_width = decoder->preview_width();
        preview_height = decoder->preview_height();
        preview_us = decoder->frame_us();
        return true;
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

        feed_rgb_preview_decoder_locked(cam, packet, has_idr, cam.rgb_decoder, kRgbPreviewWidth, kRgbPreviewFps, cam.key, cam.rgb_preview_width,
                                        cam.rgb_preview_height, cam.rgb_preview_us);
        if(cam.key == main_preview_key_) {
            feed_rgb_preview_decoder_locked(cam, packet, has_idr, cam.main_rgb_decoder, kRgbMainPreviewWidth, kRgbMainPreviewFps, cam.key + ":main",
                                            cam.main_rgb_preview_width, cam.main_rgb_preview_height, cam.main_rgb_preview_us);
        }
        else if(cam.main_rgb_decoder) {
            cleanup_rgb_decoder_async(std::move(cam.main_rgb_decoder));
            cam.main_rgb_preview_width = 0;
            cam.main_rgb_preview_height = 0;
            cam.main_rgb_preview_us = 0;
        }
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

        std::shared_ptr<CameraState> cam;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cam = ensure_camera_ptr_locked(packet.sender_id, packet.camera_id);
            cam->last_media_us = now_us();
            if(packet.stream_type == StreamType::rgb) {
                cam->rgb_packets++;
                cam->rgb_bytes += packet.payload_size;
                update_rgb_preview_locked(*cam, packet, rgb_has_idr, rgb_has_vcl);
            }
            else if(packet.stream_type == StreamType::depth_raw) {
                cam->depth_packets++;
                cam->depth_bytes += packet.payload_size;
                if(!preview.bytes.empty()) {
                    cam->depth_preview_ppm = std::move(preview.bytes);
                    cam->depth_preview_width = preview.width;
                    cam->depth_preview_height = preview.height;
                    cam->depth_preview_us = cam->last_media_us;
                }
            }
        }

        std::unique_lock<std::mutex> segment_lock(cam->segment_mutex, std::try_to_lock);
        if(!segment_lock.owns_lock()) {
            return;
        }
        bool should_record = false;
        std::string sender_id;
        std::string camera_id;
        std::string camera_name;
        std::string storage_key;
        std::string file_prefix;
        std::string announce_json;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            should_record = recording_all_ || cam->recording_requested;
            if(should_record) {
                if(cam->recording_start_us == 0) {
                    cam->recording_start_us = recording_all_ ? recording_all_start_us_ : now_us();
                    cam->recording_file_prefix = effective_file_prefix_locked(*cam);
                }
                sender_id = cam->sender_id;
                camera_id = cam->camera_id;
                camera_name = cam->camera_name;
                storage_key = cam->storage_key();
                file_prefix = cam->recording_file_prefix;
                announce_json = cam->last_announce_live ? cam->last_announce_json : "";
            }
        }
        if(!should_record) {
            return;
        }
        cam->segment.write_packet(config_, decoded_packet, sender_id, camera_id, camera_name, storage_key, file_prefix, announce_json, logger_);
        const bool segment_active = cam->segment.active();
        std::string segment_dir = cam->segment.directory();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cam->segment_active = segment_active;
            cam->segment_dir = std::move(segment_dir);
        }
    }

    void udp_loop() {
        const int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if(fd < 0) {
            logger_.error(std::string("cannot create UDP socket: ") + std::strerror(errno));
            return;
        }
        set_fd_cloexec(fd);
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
            handle_status_message(std::string(buffer.data(), static_cast<size_t>(got)), socket_endpoint(peer));
        }
        close(fd);
    }

    void tcp_loop() {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        if(fd < 0) {
            logger_.error(std::string("cannot create TCP socket: ") + std::strerror(errno));
            return;
        }
        set_fd_cloexec(fd);
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
            set_fd_cloexec(client);
            const auto peer_endpoint = socket_endpoint(peer);
            const int previous_clients = active_media_clients_.fetch_add(1);
            if(previous_clients >= kMaxActiveMediaClients) {
                active_media_clients_.fetch_sub(1);
                close(client);
                continue;
            }
            set_socket_timeout(client, kMediaClientSocketTimeoutSec);
            std::thread([this, client, peer_endpoint] {
                media_client_loop(client, peer_endpoint);
                active_media_clients_.fetch_sub(1);
            }).detach();
        }
        close(fd);
    }

    void media_client_loop(int fd, const std::string &peer_endpoint) {
        logger_.info("media client connected from=" + peer_endpoint);
        std::string last_sender;
        std::string last_camera;
        std::string last_stream;
        uint64_t last_frame_id = 0;
        while(running_ && g_running) {
            try {
                auto packet = read_media_packet(fd, config_.max_payload_bytes);
                last_sender = packet.sender_id;
                last_camera = packet.camera_id;
                last_stream = packet.stream_type == StreamType::rgb ? "rgb" : "depth";
                last_frame_id = packet.frame_id;
                handle_media_packet(packet);
            }
            catch(const std::exception &e) {
                std::ostringstream msg;
                msg << "media client disconnected from=" << peer_endpoint << " last_sender=" << last_sender << " last_camera=" << last_camera
                    << " last_stream=" << last_stream << " last_frame=" << last_frame_id << " reason=" << e.what();
                if(std::string(e.what()) == "connection closed") {
                    logger_.info(msg.str());
                }
                else {
                    logger_.warn(msg.str());
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
        set_fd_cloexec(fd);
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
            set_fd_cloexec(client);
            set_socket_timeout(client, 2);
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
            body = start_all(args.count("file_prefix") ? std::optional<std::string>(args.at("file_prefix")) : std::nullopt);
        }
        else if(method == "POST" && path == "/api/record/stop-all") {
            body = stop_all();
        }
        else if(method == "POST" && path == "/api/record/start") {
            body = start_camera(args.count("sender_id") ? args.at("sender_id") : "", args.count("camera_id") ? args.at("camera_id") : "",
                                args.count("file_prefix") ? std::optional<std::string>(args.at("file_prefix")) : std::nullopt);
        }
        else if(method == "POST" && path == "/api/record/stop") {
            body = stop_camera(args.count("sender_id") ? args.at("sender_id") : "", args.count("camera_id") ? args.at("camera_id") : "");
        }
        else if(method == "POST" && path == "/api/camera/name") {
            body = set_camera_name(args.count("sender_id") ? args.at("sender_id") : "", args.count("camera_id") ? args.at("camera_id") : "",
                                   args.count("camera_name") ? args.at("camera_name") : "");
        }
        else if(method == "POST" && path == "/api/camera/prefix") {
            body = set_camera_file_prefix(args.count("sender_id") ? args.at("sender_id") : "", args.count("camera_id") ? args.at("camera_id") : "",
                                          args.count("prefix") ? args.at("prefix") : "");
        }
        else if(method == "POST" && path == "/api/storage/prefix") {
            body = set_default_file_prefix(args.count("prefix") ? args.at("prefix") : "");
        }
        else if(method == "POST" && path == "/api/preview/main-target") {
            body = set_main_preview_target(args.count("sender_id") ? args.at("sender_id") : "",
                                           args.count("camera_id") ? args.at("camera_id") : "");
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
        else if(method == "GET" && path == "/api/preview/rgb-main") {
            const auto preview = main_rgb_preview(args.count("sender_id") ? args.at("sender_id") : "",
                                                  args.count("camera_id") ? args.at("camera_id") : "");
            if(preview) {
                content_type = "image/jpeg";
                body.assign(reinterpret_cast<const char *>(preview->data()), preview->size());
            }
            else {
                status = 404;
                body = "{\"ok\":false,\"error\":\"main rgb preview not found\"}";
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
    RuntimeState runtime_state_;
    std::atomic<bool> running_{false};
    std::atomic<int> active_media_clients_{0};
    std::thread udp_thread_;
    std::thread tcp_thread_;
    std::thread admin_thread_;
    std::mutex mutex_;
    std::string main_preview_key_;
    bool recording_all_ = false;
    uint64_t recording_all_start_us_ = 0;
    bool recording_all_has_file_prefix_override_ = false;
    std::string recording_all_file_prefix_;
    std::map<std::string, std::shared_ptr<CameraState>> cameras_;
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
