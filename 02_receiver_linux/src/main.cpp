#include "gwv3_common/protocol.hpp"
#include "gwv3_receiver/clock_sync_manager.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdlib>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cmath>
#include <dlfcn.h>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
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

constexpr size_t kMediaHeaderBaseSize = kMediaHeaderV1Size;
constexpr size_t kMediaHeaderMaxSize = kMediaHeaderV2Size;
constexpr size_t kMaxReasonablePayload = 128ull * 1024ull * 1024ull;
constexpr size_t kMaxRgbPreviewPrefixBytes = 512ull * 1024ull;
constexpr uint32_t kRgbPreviewWidth = 320;
constexpr uint32_t kRgbMainPreviewWidth = 640;
constexpr uint32_t kRgbPreviewFps = 30;
constexpr uint32_t kRgbMainPreviewFps = 30;
constexpr int kRgbPreviewJpegQuality = 10;
constexpr bool kEnableRgbThumbnailPreview = true;
constexpr bool kEnableJpegMainPreview = true;
constexpr int kRgbPreviewPipeBytes = 1024 * 1024;
constexpr int kRgbPreviewWritePollMs = 2;
constexpr int kRgbPreviewWriteBudgetMs = 25;
constexpr uint64_t kCameraOnlineTimeoutUs = 5ull * 1000ull * 1000ull;
constexpr uint64_t kOfflineCameraPurgeUs = 30ull * 1000ull * 1000ull;
constexpr uint64_t kPreviewFreshUs = 5ull * 1000ull * 1000ull;
constexpr uint64_t kPreviewRequestKeepaliveUs = 2ull * 1000ull * 1000ull;
constexpr uint64_t kPreviewDecoderIdleStopUs = 5ull * 1000ull * 1000ull;
constexpr uint64_t kMainPreviewRequestKeepaliveUs = 15ull * 1000ull * 1000ull;
constexpr uint64_t kMainPreviewDecoderIdleStopUs = 30ull * 1000ull * 1000ull;
constexpr uint64_t kWebRgbPreviewControlIntervalUs = 500ull * 1000ull;
constexpr uint64_t kWebRgbPreviewKeyframeIntervalUs = 1ull * 1000ull * 1000ull;
constexpr int kWebRgbPreviewControlLeaseMs = 2500;
constexpr size_t kRgbH264StreamMaxPackets = 180;
constexpr size_t kRgbH264StreamMaxHeaderBytes = 512ull * 1024ull;
constexpr uint32_t kRecordFpsProbeFrames = 60;
constexpr uint64_t kRecordFpsProbeMaxWaitUs = 3'000'000ull;
constexpr double kMinRecordFps = 5.0;
constexpr double kMaxRecordFps = 60.0;
constexpr size_t kMaxPendingRgbRecordBytes = 8ull * 1024ull * 1024ull;
constexpr size_t kMaxPendingDepthRecordBytes = 64ull * 1024ull * 1024ull;
constexpr size_t kDefaultRecordQueueMaxBytes = 512ull * 1024ull * 1024ull;
constexpr uint64_t kRecordQueueWarnIntervalUs = 5ull * 1000ull * 1000ull;
constexpr int kMaxActiveMediaClients = 32;
constexpr int kMediaClientSocketTimeoutSec = 2;
constexpr int kMediaSocketReceiveBufferBytes = 16 * 1024 * 1024;
constexpr uint64_t kPreviewUdpAssemblyTimeoutUs = 1ull * 1000ull * 1000ull;
constexpr size_t kPreviewUdpMaxAssemblies = 4096;
constexpr uint64_t kAnnounceCacheSaveMinIntervalUs = 60ull * 1000ull * 1000ull;
constexpr uint64_t kRoutineStatusLogMinIntervalUs = 60ull * 1000ull * 1000ull;
constexpr const char *kRgbMp4RecordMuxFlags = "+frag_keyframe+empty_moov+default_base_moof";
constexpr const char *kRgbMp4FinalMuxFlags = "+faststart";

std::atomic<bool> g_running{true};

void handle_signal(int) {
    g_running = false;
}

const char *stream_type_name(StreamType stream_type) {
    switch(stream_type) {
    case StreamType::rgb:
        return "rgb";
    case StreamType::rgb_preview:
        return "rgb_preview";
    case StreamType::depth_raw:
        return "depth";
    }
    return "unknown";
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

std::optional<int64_t> json_int64_field(const std::string &json, const std::string &key) {
    const std::regex pattern("\"" + key + "\"\\s*:\\s*(-?[0-9]+)");
    std::smatch match;
    if(std::regex_search(json, match, pattern) && match.size() >= 2) {
        try {
            return std::stoll(match[1].str());
        }
        catch(const std::exception &) {
            return std::nullopt;
        }
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

std::optional<double> json_double_field(const std::string &json, const std::string &key) {
    const std::regex pattern("\"" + key + "\"\\s*:\\s*(-?(?:[0-9]+(?:\\.[0-9]*)?|\\.[0-9]+)(?:[eE][+-]?[0-9]+)?)");
    std::smatch match;
    if(std::regex_search(json, match, pattern) && match.size() >= 2) {
        try {
            return std::stod(match[1].str());
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

std::optional<double> json_double_in_object(const std::string &json, const std::string &object_key, const std::string &field_key) {
    const auto object = json_object_field(json, object_key);
    if(!object) {
        return std::nullopt;
    }
    return json_double_field(*object, field_key);
}

std::optional<bool> json_bool_in_object(const std::string &json, const std::string &object_key, const std::string &field_key) {
    const auto object = json_object_field(json, object_key);
    if(!object) {
        return std::nullopt;
    }
    return json_bool_field(*object, field_key);
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

bool set_socket_recv_buffer(int fd, int bytes) {
    if(bytes <= 0) {
        return true;
    }
    const int requested = bytes;
    return setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &requested, sizeof(requested)) == 0;
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

bool send_all(int fd, const void *data, size_t size) {
    const auto *bytes = static_cast<const uint8_t *>(data);
    size_t offset = 0;
    while(offset < size) {
        const ssize_t sent = send(fd, bytes + offset, size - offset, MSG_NOSIGNAL);
        if(sent < 0 && errno == EINTR) {
            continue;
        }
        if(sent <= 0) {
            return false;
        }
        offset += static_cast<size_t>(sent);
    }
    return true;
}

bool send_all(int fd, const std::string &text) {
    return send_all(fd, text.data(), text.size());
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

std::optional<sockaddr_in> parse_socket_endpoint(const std::string &endpoint) {
    const auto colon = endpoint.rfind(':');
    if(colon == std::string::npos || colon == 0 || colon + 1 >= endpoint.size()) {
        return std::nullopt;
    }
    const auto ip_text = endpoint.substr(0, colon);
    const auto port_text = endpoint.substr(colon + 1);
    int port = 0;
    try {
        port = std::stoi(port_text);
    }
    catch(const std::exception &) {
        return std::nullopt;
    }
    if(port <= 0 || port > 65535) {
        return std::nullopt;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if(inet_pton(AF_INET, ip_text.c_str(), &addr.sin_addr) != 1) {
        return std::nullopt;
    }
    return addr;
}

bool send_udp_text_to_endpoint(const std::string &endpoint, const std::string &payload) {
    const auto addr = parse_socket_endpoint(endpoint);
    if(!addr) {
        return false;
    }
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if(fd < 0) {
        return false;
    }
    const auto sent = sendto(fd, payload.data(), payload.size(), 0, reinterpret_cast<const sockaddr *>(&*addr), sizeof(*addr));
    close(fd);
    return sent >= 0 && static_cast<size_t>(sent) == payload.size();
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

std::optional<size_t> h264_first_vcl_start_offset(const std::vector<uint8_t> &payload) {
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
        if(nal_type >= 1 && nal_type <= 5) {
            return start_offset;
        }
    }
    return std::nullopt;
}

bool h264_payload_has_sps_and_pps(const std::vector<uint8_t> &payload) {
    return h264_payload_has_nal_type(payload, 7) && h264_payload_has_nal_type(payload, 8);
}

std::vector<uint8_t> h264_non_vcl_prefix(const std::vector<uint8_t> &payload) {
    const auto first_vcl = h264_first_vcl_start_offset(payload);
    if(!first_vcl || *first_vcl == 0) {
        return {};
    }
    return std::vector<uint8_t>(payload.begin(), payload.begin() + static_cast<std::ptrdiff_t>(*first_vcl));
}

struct PreviewImage {
    std::vector<uint8_t> bytes;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct DepthPreviewRange {
    double min_mm = 250.0;
    double max_mm = 2500.0;
};

constexpr DepthPreviewRange kDefaultDepthPreviewRange{250, 2500};
constexpr DepthPreviewRange kGemini305DepthPreviewRange{40, 1000};

DepthPreviewRange depth_preview_range_for_camera(const std::string &sender_id, const std::string &camera_id) {
    if(camera_id == "cam01" && (sender_id == "raspberrypi-01" || sender_id == "orangepi5pro-d12a4719")) {
        return kGemini305DepthPreviewRange;
    }
    return kDefaultDepthPreviewRange;
}

double fallback_depth_scale_for_camera(const std::string &sender_id, const std::string &camera_id) {
    if(camera_id == "cam01" && (sender_id == "raspberrypi-01" || sender_id == "orangepi5pro-d12a4719")) {
        return 0.1;
    }
    return 1.0;
}

double depth_scale_from_announce_or_camera(const std::string &announce_json,
                                           const std::string &sender_id,
                                           const std::string &camera_id) {
    const double fallback = fallback_depth_scale_for_camera(sender_id, camera_id);
    const auto depth_scale = json_double_in_object(announce_json, "depth_profile", "depth_scale");
    if(depth_scale && std::isfinite(*depth_scale) && *depth_scale > 0.0 && *depth_scale <= 1000.0) {
        return *depth_scale;
    }
    return fallback;
}

uint8_t clamp_color(double value) {
    if(value <= 0.0) {
        return 0;
    }
    if(value >= 255.0) {
        return 255;
    }
    return static_cast<uint8_t>(value);
}

void append_depth_color(std::vector<uint8_t> &out, uint16_t raw_value, const DepthPreviewRange &range, double depth_scale) {
    if(raw_value == 0 || raw_value == std::numeric_limits<uint16_t>::max() || depth_scale <= 0.0 || range.max_mm <= range.min_mm) {
        out.push_back(12);
        out.push_back(16);
        out.push_back(24);
        return;
    }
    const double value_mm = static_cast<double>(raw_value) * depth_scale;
    const double clamped = std::clamp(value_mm, range.min_mm, range.max_mm);
    const double t = (clamped - range.min_mm) / (range.max_mm - range.min_mm);
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

void append_u64_le(std::vector<uint8_t> &out, uint64_t value) {
    for(int i = 0; i < 8; ++i) {
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

PreviewImage build_depth_preview_bmp(const std::vector<uint8_t> &payload,
                                     uint32_t width,
                                     uint32_t height,
                                     const DepthPreviewRange &range,
                                     double depth_scale) {
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
            append_depth_color(rgb, value, range, depth_scale);
        }
    }

    return build_bmp_from_rgb_pixels(rgb, image.width, image.height);
}

struct Config {
    std::string status_bind_ip = "0.0.0.0";
    uint16_t status_port = 50011;
    std::string media_bind_ip = "0.0.0.0";
    uint16_t media_port = 50010;
    bool preview_enabled = true;
    bool media_udp_enabled = false;
    std::string media_udp_bind_ip = "0.0.0.0";
    uint16_t media_udp_port = 50013;
    bool preview_udp_enabled = false;
    std::string preview_udp_bind_ip = "0.0.0.0";
    uint16_t preview_udp_port = 50012;
    ClockSyncManagerConfig clock_sync;
    std::string admin_bind_ip = "127.0.0.1";
    uint16_t admin_port = 18080;
    std::string nas_root = "/home/fz/Desktop/nas";
    std::string log_directory = "08_reports/receiver_logs";
    std::string state_path = "06_configs/receiver_runtime_state.json";
    std::string ffmpeg_path = "ffmpeg";
    int segment_seconds = 300;
    int depth_fps = 30;
    bool write_debug_h264 = false;
    bool write_debug_depth_raw = false;
    size_t max_payload_bytes = kMaxReasonablePayload;
    size_t record_queue_max_bytes = kDefaultRecordQueueMaxBytes;
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
    cfg.preview_enabled = config_bool(json, "preview_enabled", cfg.preview_enabled);
    cfg.media_udp_enabled = config_bool(json, "media_udp_enabled", cfg.media_udp_enabled);
    cfg.media_udp_bind_ip = config_string(json, "media_udp_bind_ip", cfg.media_udp_bind_ip);
    cfg.media_udp_port = config_port(json, "media_udp_port", cfg.media_udp_port);
    cfg.preview_udp_enabled = config_bool(json, "preview_udp_enabled", cfg.preview_udp_enabled);
    cfg.preview_udp_bind_ip = config_string(json, "preview_udp_bind_ip", cfg.preview_udp_bind_ip);
    cfg.preview_udp_port = config_port(json, "preview_udp_port", cfg.preview_udp_port);
    cfg.clock_sync.enabled = json_bool_in_object(json, "clock_sync", "enabled").value_or(cfg.clock_sync.enabled);
    cfg.clock_sync.bind_ip = json_string_in_object(json, "clock_sync", "bind_ip").value_or(cfg.clock_sync.bind_ip);
    const int clock_sync_port = json_int_in_object(json, "clock_sync", "port").value_or(cfg.clock_sync.port);
    if(clock_sync_port <= 0 || clock_sync_port > 65535) {
        throw std::runtime_error("invalid port in receiver config: clock_sync.port");
    }
    cfg.clock_sync.port = static_cast<uint16_t>(clock_sync_port);
    cfg.clock_sync.model_timeout_ms = json_int_in_object(json, "clock_sync", "model_timeout_ms").value_or(cfg.clock_sync.model_timeout_ms);
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
    cfg.record_queue_max_bytes = static_cast<size_t>(std::max(1, config_int(json, "record_queue_max_mb", 512))) * 1024ull * 1024ull;

    if(cfg.segment_seconds <= 0) {
        throw std::runtime_error("segment_seconds must be positive");
    }
    if(cfg.depth_fps <= 0) {
        throw std::runtime_error("depth_fps must be positive");
    }
    if(cfg.clock_sync.model_timeout_ms <= 0) {
        throw std::runtime_error("clock_sync.model_timeout_ms must be positive");
    }
    if(cfg.clock_sync.enabled && cfg.preview_udp_enabled && cfg.clock_sync.port == cfg.preview_udp_port) {
        throw std::runtime_error("clock_sync.port conflicts with enabled preview_udp_port");
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
            const std::string jpeg_quality = std::to_string(kRgbPreviewJpegQuality);
            execlp(cfg.ffmpeg_path.c_str(), cfg.ffmpeg_path.c_str(), "-hide_banner", "-loglevel", "error", "-fflags", "nobuffer",
                   "-flags", "low_delay", "-probesize", "32", "-analyzeduration", "0", "-avioflags", "direct", "-f", "h264", "-i", "pipe:0",
                   "-vf", scale.c_str(), "-q:v", jpeg_quality.c_str(), "-f", "image2pipe", "-vcodec", "mjpeg", "pipe:1",
                   static_cast<char *>(nullptr));
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
    int32_t rgb_exposure_us = -1;
    int32_t rgb_gain = -1;
    int32_t rgb_auto_exposure = -1;
    int32_t rgb_actual_fps = -1;
    uint64_t sender_capture_host_timestamp_us = 0;
    uint64_t sender_timing_bound_timestamp_us = 0;
    uint64_t sender_encode_start_timestamp_us = 0;
    uint64_t sender_encode_done_timestamp_us = 0;
    uint64_t sender_packet_queued_timestamp_us = 0;
    uint64_t receiver_receive_timestamp_us = 0;
    bool clock_sync_valid = false;
    int64_t sender_offset_us = 0;
    int64_t sender_delay_us = 0;
    double sender_drift_ppm = 0.0;
    uint64_t global_timestamp_us = 0;
    std::vector<uint8_t> payload;
};

struct MediaPacketReadBuffers {
    std::vector<uint8_t> header;
    std::vector<char> text;
};

void copy_media_packet_metadata(const MediaPacket &src, MediaPacket &dst) {
    dst.stream_type = src.stream_type;
    dst.flags = src.flags;
    dst.sender_id = src.sender_id;
    dst.camera_id = src.camera_id;
    dst.codec_or_compression = src.codec_or_compression;
    dst.frame_id = src.frame_id;
    dst.timestamp_us = src.timestamp_us;
    dst.system_timestamp_us = src.system_timestamp_us;
    dst.pair_id = src.pair_id;
    dst.width = src.width;
    dst.height = src.height;
    dst.pixel_format = src.pixel_format;
    dst.payload_size = src.payload_size;
    dst.uncompressed_size = src.uncompressed_size;
    dst.rgb_exposure_us = src.rgb_exposure_us;
    dst.rgb_gain = src.rgb_gain;
    dst.rgb_auto_exposure = src.rgb_auto_exposure;
    dst.rgb_actual_fps = src.rgb_actual_fps;
    dst.sender_capture_host_timestamp_us = src.sender_capture_host_timestamp_us;
    dst.sender_timing_bound_timestamp_us = src.sender_timing_bound_timestamp_us;
    dst.sender_encode_start_timestamp_us = src.sender_encode_start_timestamp_us;
    dst.sender_encode_done_timestamp_us = src.sender_encode_done_timestamp_us;
    dst.sender_packet_queued_timestamp_us = src.sender_packet_queued_timestamp_us;
    dst.receiver_receive_timestamp_us = src.receiver_receive_timestamp_us;
    dst.clock_sync_valid = src.clock_sync_valid;
    dst.sender_offset_us = src.sender_offset_us;
    dst.sender_delay_us = src.sender_delay_us;
    dst.sender_drift_ppm = src.sender_drift_ppm;
    dst.global_timestamp_us = src.global_timestamp_us;
    dst.payload.clear();
}

MediaPacket media_packet_metadata_only(const MediaPacket &packet) {
    MediaPacket copy;
    copy_media_packet_metadata(packet, copy);
    return copy;
}

void reset_media_packet_for_read(MediaPacket &packet) {
    packet.stream_type = StreamType::rgb;
    packet.flags = 0;
    packet.sender_id.clear();
    packet.camera_id.clear();
    packet.codec_or_compression.clear();
    packet.frame_id = 0;
    packet.timestamp_us = 0;
    packet.system_timestamp_us = 0;
    packet.pair_id = 0;
    packet.width = 0;
    packet.height = 0;
    packet.pixel_format = PixelFormat::encoded_video;
    packet.payload_size = 0;
    packet.uncompressed_size = 0;
    packet.rgb_exposure_us = -1;
    packet.rgb_gain = -1;
    packet.rgb_auto_exposure = -1;
    packet.rgb_actual_fps = -1;
    packet.sender_capture_host_timestamp_us = 0;
    packet.sender_timing_bound_timestamp_us = 0;
    packet.sender_encode_start_timestamp_us = 0;
    packet.sender_encode_done_timestamp_us = 0;
    packet.sender_packet_queued_timestamp_us = 0;
    packet.receiver_receive_timestamp_us = 0;
    packet.clock_sync_valid = false;
    packet.sender_offset_us = 0;
    packet.sender_delay_us = 0;
    packet.sender_drift_ppm = 0.0;
    packet.global_timestamp_us = 0;
    packet.payload.clear();
}

void read_media_packet_into(int fd, size_t max_payload_bytes, MediaPacketReadBuffers &buffers, MediaPacket &packet) {
    reset_media_packet_for_read(packet);
    buffers.header.resize(kMediaHeaderBaseSize);
    if(!read_exact(fd, buffers.header.data(), buffers.header.size())) {
        throw std::runtime_error("connection closed");
    }

    auto &header = buffers.header;
    const uint32_t magic = read_le32(header.data() + 0);
    const uint16_t header_version = read_le16(header.data() + 4);
    const uint16_t header_size = read_le16(header.data() + 6);
    if(magic != kMediaMagic || header_version < 1 || header_version > kMediaHeaderVersion || header_size < kMediaHeaderBaseSize
       || header_size > kMediaHeaderMaxSize) {
        throw std::runtime_error("invalid media packet header");
    }
    if(header_size > header.size()) {
        const size_t already_read = header.size();
        header.resize(header_size);
        if(!read_exact(fd, header.data() + already_read, header_size - already_read)) {
            throw std::runtime_error("connection closed");
        }
    }

    const uint16_t sender_id_len = read_le16(header.data() + 14);
    const uint16_t camera_id_len = read_le16(header.data() + 16);
    const uint16_t codec_len = read_le16(header.data() + 18);
    const uint64_t payload_size = read_le64(header.data() + 62);
    if(payload_size > max_payload_bytes) {
        throw std::runtime_error("media payload too large");
    }

    packet.stream_type = static_cast<StreamType>(header[8]);
    packet.flags = read_le32(header.data() + 10);
    packet.frame_id = read_le64(header.data() + 20);
    packet.timestamp_us = read_le64(header.data() + 28);
    packet.system_timestamp_us = read_le64(header.data() + 36);
    packet.pair_id = read_le64(header.data() + 44);
    packet.width = read_le32(header.data() + 52);
    packet.height = read_le32(header.data() + 56);
    packet.pixel_format = static_cast<PixelFormat>(read_le16(header.data() + 60));
    packet.payload_size = payload_size;
    packet.uncompressed_size = read_le64(header.data() + 70);
    if((packet.flags & has_rgb_diagnostics) != 0u) {
        packet.rgb_exposure_us = static_cast<int32_t>(read_le32(header.data() + 78));
        packet.rgb_gain = static_cast<int32_t>(read_le32(header.data() + 82));
        packet.rgb_auto_exposure = static_cast<int32_t>(read_le32(header.data() + 86));
        packet.rgb_actual_fps = static_cast<int32_t>(read_le32(header.data() + 90));
    }
    if(header_size >= kMediaHeaderV2Size && (packet.flags & has_pipeline_diagnostics) != 0u) {
        packet.sender_capture_host_timestamp_us = read_le64(header.data() + 94);
        packet.sender_timing_bound_timestamp_us = read_le64(header.data() + 102);
        packet.sender_encode_start_timestamp_us = read_le64(header.data() + 110);
        packet.sender_encode_done_timestamp_us = read_le64(header.data() + 118);
        packet.sender_packet_queued_timestamp_us = read_le64(header.data() + 126);
    }

    const size_t text_size = static_cast<size_t>(sender_id_len) + static_cast<size_t>(camera_id_len) + static_cast<size_t>(codec_len);
    buffers.text.resize(text_size);
    if(!buffers.text.empty() && !read_exact(fd, buffers.text.data(), buffers.text.size())) {
        throw std::runtime_error("connection closed while reading packet strings");
    }
    const char *text = buffers.text.empty() ? "" : buffers.text.data();
    packet.sender_id.assign(text, sender_id_len);
    packet.camera_id.assign(text + sender_id_len, camera_id_len);
    packet.codec_or_compression.assign(text + sender_id_len + camera_id_len, codec_len);

    packet.payload.resize(static_cast<size_t>(payload_size));
    if(payload_size > 0 && !read_exact(fd, packet.payload.data(), packet.payload.size())) {
        throw std::runtime_error("connection closed while reading payload");
    }
}

MediaPacket parse_media_packet_buffer(const uint8_t *data, size_t size, size_t max_payload_bytes) {
    if(size < kMediaHeaderBaseSize) {
        throw std::runtime_error("UDP media packet too small");
    }

    const uint32_t magic = read_le32(data + 0);
    const uint16_t header_version = read_le16(data + 4);
    const uint16_t header_size = read_le16(data + 6);
    if(magic != kMediaMagic || header_version < 1 || header_version > kMediaHeaderVersion || header_size < kMediaHeaderBaseSize
       || header_size > kMediaHeaderMaxSize || size < header_size) {
        throw std::runtime_error("invalid UDP media packet header");
    }

    const uint16_t sender_id_len = read_le16(data + 14);
    const uint16_t camera_id_len = read_le16(data + 16);
    const uint16_t codec_len = read_le16(data + 18);
    const uint64_t payload_size = read_le64(data + 62);
    if(payload_size > max_payload_bytes) {
        throw std::runtime_error("UDP media payload too large");
    }
    const size_t text_size = static_cast<size_t>(sender_id_len) + static_cast<size_t>(camera_id_len) + static_cast<size_t>(codec_len);
    const size_t payload_offset = static_cast<size_t>(header_size) + text_size;
    if(payload_offset > size || payload_size > size - payload_offset) {
        throw std::runtime_error("truncated UDP media packet");
    }

    MediaPacket packet;
    packet.stream_type = static_cast<StreamType>(data[8]);
    packet.flags = read_le32(data + 10);
    packet.frame_id = read_le64(data + 20);
    packet.timestamp_us = read_le64(data + 28);
    packet.system_timestamp_us = read_le64(data + 36);
    packet.pair_id = read_le64(data + 44);
    packet.width = read_le32(data + 52);
    packet.height = read_le32(data + 56);
    packet.pixel_format = static_cast<PixelFormat>(read_le16(data + 60));
    packet.payload_size = payload_size;
    packet.uncompressed_size = read_le64(data + 70);
    if((packet.flags & has_rgb_diagnostics) != 0u) {
        packet.rgb_exposure_us = static_cast<int32_t>(read_le32(data + 78));
        packet.rgb_gain = static_cast<int32_t>(read_le32(data + 82));
        packet.rgb_auto_exposure = static_cast<int32_t>(read_le32(data + 86));
        packet.rgb_actual_fps = static_cast<int32_t>(read_le32(data + 90));
    }
    if(header_size >= kMediaHeaderV2Size && (packet.flags & has_pipeline_diagnostics) != 0u) {
        packet.sender_capture_host_timestamp_us = read_le64(data + 94);
        packet.sender_timing_bound_timestamp_us = read_le64(data + 102);
        packet.sender_encode_start_timestamp_us = read_le64(data + 110);
        packet.sender_encode_done_timestamp_us = read_le64(data + 118);
        packet.sender_packet_queued_timestamp_us = read_le64(data + 126);
    }

    const char *text = reinterpret_cast<const char *>(data + header_size);
    packet.sender_id.assign(text, sender_id_len);
    packet.camera_id.assign(text + sender_id_len, camera_id_len);
    packet.codec_or_compression.assign(text + sender_id_len + camera_id_len, codec_len);
    packet.payload.assign(data + payload_offset, data + payload_offset + static_cast<size_t>(payload_size));
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

struct Lz4Api {
    using DecompressSafeFn = int (*)(const char *, char *, int, int);

    void *handle = nullptr;
    DecompressSafeFn decompress_safe = nullptr;
};

Lz4Api &lz4_api() {
    static Lz4Api api;
    static std::once_flag once;
    std::call_once(once, [] {
        api.handle = dlopen("liblz4.so.1", RTLD_LAZY | RTLD_LOCAL);
        if(!api.handle) {
            throw std::runtime_error(std::string("cannot load liblz4.so.1: ") + dlerror());
        }
        api.decompress_safe = reinterpret_cast<Lz4Api::DecompressSafeFn>(dlsym(api.handle, "LZ4_decompress_safe"));
        if(!api.decompress_safe) {
            throw std::runtime_error("liblz4.so.1 does not provide required decompression symbols");
        }
    });
    return api;
}

std::vector<uint8_t> lz4_decompress_payload(const MediaPacket &packet) {
    if(packet.uncompressed_size == 0 || packet.uncompressed_size > kMaxReasonablePayload
       || packet.payload.size() > static_cast<size_t>(std::numeric_limits<int>::max())
       || packet.uncompressed_size > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("invalid lz4 uncompressed depth size");
    }
    std::vector<uint8_t> out(static_cast<size_t>(packet.uncompressed_size));
    auto &api = lz4_api();
    const int decoded_size = api.decompress_safe(reinterpret_cast<const char *>(packet.payload.data()),
                                                 reinterpret_cast<char *>(out.data()),
                                                 static_cast<int>(packet.payload.size()),
                                                 static_cast<int>(out.size()));
    if(decoded_size != static_cast<int>(out.size())) {
        throw std::runtime_error("lz4 depth decompression failed");
    }
    return out;
}

struct Plz4ChunkEntry {
    uint32_t raw_offset = 0;
    uint32_t raw_size = 0;
    uint32_t compressed_offset = 0;
    uint32_t compressed_size = 0;
};

std::vector<uint8_t> plz4_decompress_payload(const MediaPacket &packet) {
    if(packet.payload.size() < 16) {
        throw std::runtime_error("invalid plz4 depth payload");
    }
    const uint32_t magic = read_le32(packet.payload.data());
    const uint16_t version = read_le16(packet.payload.data() + 4);
    const uint16_t chunk_count = read_le16(packet.payload.data() + 6);
    const uint32_t raw_total = read_le32(packet.payload.data() + 8);
    if(magic != 0x345a4c50u || version != 1 || chunk_count == 0 || raw_total == 0
       || raw_total > kMaxReasonablePayload || packet.uncompressed_size != raw_total) {
        throw std::runtime_error("invalid plz4 depth header");
    }
    const size_t table_size = 16ull + static_cast<size_t>(chunk_count) * 12ull;
    if(table_size > packet.payload.size()) {
        throw std::runtime_error("truncated plz4 depth table");
    }

    std::vector<Plz4ChunkEntry> chunks;
    chunks.reserve(chunk_count);
    size_t compressed_offset = table_size;
    for(uint16_t i = 0; i < chunk_count; ++i) {
        const uint8_t *entry = packet.payload.data() + 16ull + static_cast<size_t>(i) * 12ull;
        Plz4ChunkEntry chunk;
        chunk.raw_offset = read_le32(entry);
        chunk.raw_size = read_le32(entry + 4);
        chunk.compressed_size = read_le32(entry + 8);
        chunk.compressed_offset = static_cast<uint32_t>(compressed_offset);
        if(chunk.raw_size == 0 || chunk.raw_offset > raw_total || chunk.raw_size > raw_total - chunk.raw_offset
           || chunk.compressed_size == 0 || compressed_offset > packet.payload.size()
           || chunk.compressed_size > packet.payload.size() - compressed_offset) {
            throw std::runtime_error("invalid plz4 depth chunk");
        }
        compressed_offset += chunk.compressed_size;
        chunks.push_back(chunk);
    }
    if(compressed_offset != packet.payload.size()) {
        throw std::runtime_error("plz4 depth payload has trailing bytes");
    }

    std::vector<uint8_t> out(raw_total);
    auto &api = lz4_api();
    std::vector<std::future<void>> futures;
    futures.reserve(chunks.size());
    for(const auto &chunk : chunks) {
        futures.emplace_back(std::async(std::launch::async, [&packet, &out, &api, chunk] {
            const int rc = api.decompress_safe(
                reinterpret_cast<const char *>(packet.payload.data() + chunk.compressed_offset),
                reinterpret_cast<char *>(out.data() + chunk.raw_offset),
                static_cast<int>(chunk.compressed_size),
                static_cast<int>(chunk.raw_size));
            if(rc != static_cast<int>(chunk.raw_size)) {
                throw std::runtime_error("plz4 depth chunk decompression failed");
            }
        }));
    }
    for(auto &future : futures) {
        future.get();
    }
    return out;
}

std::vector<uint8_t> pzlib_decompress_payload(const MediaPacket &packet) {
    if(packet.payload.size() < 16) {
        throw std::runtime_error("invalid pzlib depth payload");
    }
    const uint32_t magic = read_le32(packet.payload.data());
    const uint16_t version = read_le16(packet.payload.data() + 4);
    const uint16_t chunk_count = read_le16(packet.payload.data() + 6);
    const uint32_t raw_total = read_le32(packet.payload.data() + 8);
    if(magic != 0x424c5a50u || version != 1 || chunk_count == 0 || raw_total == 0
       || raw_total > kMaxReasonablePayload || packet.uncompressed_size != raw_total) {
        throw std::runtime_error("invalid pzlib depth header");
    }
    const size_t table_size = 16ull + static_cast<size_t>(chunk_count) * 12ull;
    if(table_size > packet.payload.size()) {
        throw std::runtime_error("truncated pzlib depth table");
    }

    std::vector<Plz4ChunkEntry> chunks;
    chunks.reserve(chunk_count);
    size_t compressed_offset = table_size;
    for(uint16_t i = 0; i < chunk_count; ++i) {
        const uint8_t *entry = packet.payload.data() + 16ull + static_cast<size_t>(i) * 12ull;
        Plz4ChunkEntry chunk;
        chunk.raw_offset = read_le32(entry);
        chunk.raw_size = read_le32(entry + 4);
        chunk.compressed_size = read_le32(entry + 8);
        chunk.compressed_offset = static_cast<uint32_t>(compressed_offset);
        if(chunk.raw_size == 0 || chunk.raw_offset > raw_total || chunk.raw_size > raw_total - chunk.raw_offset
           || chunk.compressed_size == 0 || compressed_offset > packet.payload.size()
           || chunk.compressed_size > packet.payload.size() - compressed_offset
           || chunk.raw_size > static_cast<uint32_t>(std::numeric_limits<uLongf>::max())) {
            throw std::runtime_error("invalid pzlib depth chunk");
        }
        compressed_offset += chunk.compressed_size;
        chunks.push_back(chunk);
    }
    if(compressed_offset != packet.payload.size()) {
        throw std::runtime_error("pzlib depth payload has trailing bytes");
    }

    std::vector<uint8_t> out(raw_total);
    std::vector<std::future<void>> futures;
    futures.reserve(chunks.size());
    for(const auto &chunk : chunks) {
        futures.emplace_back(std::async(std::launch::async, [&packet, &out, chunk] {
            uLongf out_size = static_cast<uLongf>(chunk.raw_size);
            const int rc =
                uncompress(out.data() + chunk.raw_offset, &out_size, packet.payload.data() + chunk.compressed_offset,
                           static_cast<uLong>(chunk.compressed_size));
            if(rc != Z_OK || out_size != chunk.raw_size) {
                throw std::runtime_error("pzlib depth chunk decompression failed");
            }
        }));
    }
    for(auto &future : futures) {
        future.get();
    }
    return out;
}

std::vector<uint8_t> q8_decompress_payload(const MediaPacket &packet, uint32_t magic, bool use_lz4) {
    if(packet.uncompressed_size == 0 || packet.uncompressed_size > kMaxReasonablePayload
       || packet.uncompressed_size % sizeof(uint16_t) != 0 || packet.payload.size() < 16) {
        throw std::runtime_error("invalid q8 depth payload");
    }
    const uint32_t packet_magic = read_le32(packet.payload.data());
    const uint16_t version = read_le16(packet.payload.data() + 4);
    const uint16_t raw_step = read_le16(packet.payload.data() + 6);
    const uint32_t sample_count = read_le32(packet.payload.data() + 8);
    const uint32_t compressed_size = read_le32(packet.payload.data() + 12);
    const size_t expected_sample_count = static_cast<size_t>(packet.uncompressed_size / sizeof(uint16_t));
    if(packet_magic != magic || version != 1 || raw_step == 0 || sample_count != expected_sample_count
       || sample_count > static_cast<uint32_t>(std::numeric_limits<int>::max()) || compressed_size == 0
       || static_cast<size_t>(compressed_size) != packet.payload.size() - 16
       || compressed_size > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("invalid q8 depth header");
    }

    std::vector<uint8_t> quantized(expected_sample_count);
    if(use_lz4) {
        auto &api = lz4_api();
        const int decoded_size = api.decompress_safe(reinterpret_cast<const char *>(packet.payload.data() + 16),
                                                     reinterpret_cast<char *>(quantized.data()),
                                                     static_cast<int>(compressed_size),
                                                     static_cast<int>(quantized.size()));
        if(decoded_size != static_cast<int>(quantized.size())) {
            throw std::runtime_error("q8 lz4 depth decompression failed");
        }
    }
    else {
        uLongf out_size = static_cast<uLongf>(quantized.size());
        const int rc = uncompress(quantized.data(), &out_size, packet.payload.data() + 16, static_cast<uLong>(compressed_size));
        if(rc != Z_OK || out_size != quantized.size()) {
            throw std::runtime_error("q8 zlib depth decompression failed");
        }
    }

    std::vector<uint8_t> out(static_cast<size_t>(packet.uncompressed_size), 0);
    for(size_t i = 0; i < quantized.size(); ++i) {
        const uint8_t index = quantized[i];
        const uint32_t value = index == 0 ? 0u : std::min<uint32_t>(static_cast<uint32_t>(index) * raw_step,
                                                                    std::numeric_limits<uint16_t>::max());
        const size_t offset = i * sizeof(uint16_t);
        out[offset] = static_cast<uint8_t>(value & 0xffu);
        out[offset + 1] = static_cast<uint8_t>((value >> 8u) & 0xffu);
    }
    return out;
}

std::vector<uint8_t> q8lz4_decompress_payload(const MediaPacket &packet) {
    return q8_decompress_payload(packet, 0x314c3851u, true);  // bytes: Q 8 L 1
}

std::vector<uint8_t> q8zlib_decompress_payload(const MediaPacket &packet) {
    return q8_decompress_payload(packet, 0x315a3851u, false);  // bytes: Q 8 Z 1
}

struct PQ8ZlibChunkEntry {
    uint32_t sample_offset = 0;
    uint32_t sample_count = 0;
    uint32_t compressed_offset = 0;
    uint32_t compressed_size = 0;
};

size_t packed_depth12_size(uint32_t sample_count) {
    return ((static_cast<size_t>(sample_count) + 1u) / 2u) * 3u;
}

void unpack_depth12_into(const std::vector<uint8_t> &packed, uint16_t raw_step, std::vector<uint8_t> &out, uint32_t sample_offset,
                         uint32_t sample_count) {
    size_t packed_index = 0;
    for(uint32_t i = 0; i < sample_count; i += 2) {
        if(packed_index + 2 >= packed.size()) {
            throw std::runtime_error("truncated pq12zlib depth chunk");
        }
        const uint16_t a = static_cast<uint16_t>(packed[packed_index])
                           | (static_cast<uint16_t>(packed[packed_index + 1] & 0x0fu) << 8u);
        const uint16_t b = static_cast<uint16_t>((packed[packed_index + 1] >> 4u) & 0x0fu)
                           | (static_cast<uint16_t>(packed[packed_index + 2]) << 4u);
        packed_index += 3;
        const auto write_sample = [&](uint32_t local_index, uint16_t quantized) {
            const uint32_t value = quantized == 0 ? 0u : std::min<uint32_t>(static_cast<uint32_t>(quantized) * raw_step,
                                                                            std::numeric_limits<uint16_t>::max());
            const size_t offset = (static_cast<size_t>(sample_offset) + local_index) * sizeof(uint16_t);
            out[offset] = static_cast<uint8_t>(value & 0xffu);
            out[offset + 1] = static_cast<uint8_t>((value >> 8u) & 0xffu);
        };
        write_sample(i, a);
        if(i + 1 < sample_count) {
            write_sample(i + 1, b);
        }
    }
}

std::vector<uint8_t> pq12zlib_decompress_payload(const MediaPacket &packet) {
    if(packet.uncompressed_size == 0 || packet.uncompressed_size > kMaxReasonablePayload
       || packet.uncompressed_size % sizeof(uint16_t) != 0 || packet.payload.size() < 20) {
        throw std::runtime_error("invalid pq12zlib depth payload");
    }
    const uint32_t magic = read_le32(packet.payload.data());
    const uint16_t version = read_le16(packet.payload.data() + 4);
    const uint16_t raw_step = read_le16(packet.payload.data() + 6);
    const uint32_t sample_count = read_le32(packet.payload.data() + 8);
    const uint16_t chunk_count = read_le16(packet.payload.data() + 12);
    const size_t expected_sample_count = static_cast<size_t>(packet.uncompressed_size / sizeof(uint16_t));
    if(magic != 0x5a323150u || version != 1 || raw_step == 0 || sample_count != expected_sample_count || chunk_count == 0) {
        throw std::runtime_error("invalid pq12zlib depth header");
    }
    const size_t table_size = 20ull + static_cast<size_t>(chunk_count) * 12ull;
    if(table_size > packet.payload.size()) {
        throw std::runtime_error("truncated pq12zlib depth table");
    }

    std::vector<PQ8ZlibChunkEntry> chunks;
    chunks.reserve(chunk_count);
    size_t compressed_offset = table_size;
    uint32_t expected_offset = 0;
    for(uint16_t i = 0; i < chunk_count; ++i) {
        const uint8_t *entry = packet.payload.data() + 20ull + static_cast<size_t>(i) * 12ull;
        PQ8ZlibChunkEntry chunk;
        chunk.sample_offset = read_le32(entry);
        chunk.sample_count = read_le32(entry + 4);
        chunk.compressed_size = read_le32(entry + 8);
        chunk.compressed_offset = static_cast<uint32_t>(compressed_offset);
        if(chunk.sample_offset != expected_offset || chunk.sample_count == 0 || chunk.sample_offset > sample_count
           || chunk.sample_count > sample_count - chunk.sample_offset || chunk.compressed_size == 0
           || compressed_offset > packet.payload.size() || chunk.compressed_size > packet.payload.size() - compressed_offset
           || packed_depth12_size(chunk.sample_count) > static_cast<size_t>(std::numeric_limits<uLongf>::max())) {
            throw std::runtime_error("invalid pq12zlib depth chunk");
        }
        expected_offset += chunk.sample_count;
        compressed_offset += chunk.compressed_size;
        chunks.push_back(chunk);
    }
    if(expected_offset != sample_count || compressed_offset != packet.payload.size()) {
        throw std::runtime_error("invalid pq12zlib depth layout");
    }

    std::vector<uint8_t> out(static_cast<size_t>(packet.uncompressed_size), 0);
    std::vector<std::future<void>> futures;
    futures.reserve(chunks.size());
    for(const auto &chunk : chunks) {
        futures.emplace_back(std::async(std::launch::async, [&packet, &out, raw_step, chunk] {
            std::vector<uint8_t> packed(packed_depth12_size(chunk.sample_count));
            uLongf out_size = static_cast<uLongf>(packed.size());
            const int rc =
                uncompress(packed.data(), &out_size, packet.payload.data() + chunk.compressed_offset,
                           static_cast<uLong>(chunk.compressed_size));
            if(rc != Z_OK || out_size != packed.size()) {
                throw std::runtime_error("pq12zlib depth chunk decompression failed");
            }
            unpack_depth12_into(packed, raw_step, out, chunk.sample_offset, chunk.sample_count);
        }));
    }
    for(auto &future : futures) {
        future.get();
    }
    return out;
}

std::vector<uint8_t> pq8zlib_decompress_payload(const MediaPacket &packet) {
    if(packet.uncompressed_size == 0 || packet.uncompressed_size > kMaxReasonablePayload
       || packet.uncompressed_size % sizeof(uint16_t) != 0 || packet.payload.size() < 20) {
        throw std::runtime_error("invalid pq8zlib depth payload");
    }
    const uint32_t magic = read_le32(packet.payload.data());
    const uint16_t version = read_le16(packet.payload.data() + 4);
    const uint16_t raw_step = read_le16(packet.payload.data() + 6);
    const uint32_t sample_count = read_le32(packet.payload.data() + 8);
    const uint16_t chunk_count = read_le16(packet.payload.data() + 12);
    const size_t expected_sample_count = static_cast<size_t>(packet.uncompressed_size / sizeof(uint16_t));
    if(magic != 0x5a385150u || version != 1 || raw_step == 0 || sample_count != expected_sample_count || chunk_count == 0) {
        throw std::runtime_error("invalid pq8zlib depth header");
    }
    const size_t table_size = 20ull + static_cast<size_t>(chunk_count) * 12ull;
    if(table_size > packet.payload.size()) {
        throw std::runtime_error("truncated pq8zlib depth table");
    }

    std::vector<PQ8ZlibChunkEntry> chunks;
    chunks.reserve(chunk_count);
    size_t compressed_offset = table_size;
    uint32_t expected_offset = 0;
    for(uint16_t i = 0; i < chunk_count; ++i) {
        const uint8_t *entry = packet.payload.data() + 20ull + static_cast<size_t>(i) * 12ull;
        PQ8ZlibChunkEntry chunk;
        chunk.sample_offset = read_le32(entry);
        chunk.sample_count = read_le32(entry + 4);
        chunk.compressed_size = read_le32(entry + 8);
        chunk.compressed_offset = static_cast<uint32_t>(compressed_offset);
        if(chunk.sample_offset != expected_offset || chunk.sample_count == 0 || chunk.sample_offset > sample_count
           || chunk.sample_count > sample_count - chunk.sample_offset || chunk.compressed_size == 0
           || compressed_offset > packet.payload.size() || chunk.compressed_size > packet.payload.size() - compressed_offset
           || chunk.sample_count > static_cast<uint32_t>(std::numeric_limits<uLongf>::max())) {
            throw std::runtime_error("invalid pq8zlib depth chunk");
        }
        expected_offset += chunk.sample_count;
        compressed_offset += chunk.compressed_size;
        chunks.push_back(chunk);
    }
    if(expected_offset != sample_count || compressed_offset != packet.payload.size()) {
        throw std::runtime_error("invalid pq8zlib depth layout");
    }

    std::vector<uint8_t> quantized(expected_sample_count);
    std::vector<std::future<void>> futures;
    futures.reserve(chunks.size());
    for(const auto &chunk : chunks) {
        futures.emplace_back(std::async(std::launch::async, [&packet, &quantized, chunk] {
            uLongf out_size = static_cast<uLongf>(chunk.sample_count);
            const int rc =
                uncompress(quantized.data() + chunk.sample_offset, &out_size, packet.payload.data() + chunk.compressed_offset,
                           static_cast<uLong>(chunk.compressed_size));
            if(rc != Z_OK || out_size != chunk.sample_count) {
                throw std::runtime_error("pq8zlib depth chunk decompression failed");
            }
        }));
    }
    for(auto &future : futures) {
        future.get();
    }

    std::vector<uint8_t> out(static_cast<size_t>(packet.uncompressed_size), 0);
    for(size_t i = 0; i < quantized.size(); ++i) {
        const uint8_t index = quantized[i];
        const uint32_t value = index == 0 ? 0u : std::min<uint32_t>(static_cast<uint32_t>(index) * raw_step,
                                                                    std::numeric_limits<uint16_t>::max());
        const size_t offset = i * sizeof(uint16_t);
        out[offset] = static_cast<uint8_t>(value & 0xffu);
        out[offset + 1] = static_cast<uint8_t>((value >> 8u) & 0xffu);
    }
    return out;
}

std::vector<uint8_t> pq8lz4_decompress_payload(const MediaPacket &packet) {
    if(packet.uncompressed_size == 0 || packet.uncompressed_size > kMaxReasonablePayload
       || packet.uncompressed_size % sizeof(uint16_t) != 0 || packet.payload.size() < 20) {
        throw std::runtime_error("invalid pq8lz4 depth payload");
    }
    const uint32_t magic = read_le32(packet.payload.data());
    const uint16_t version = read_le16(packet.payload.data() + 4);
    const uint16_t raw_step = read_le16(packet.payload.data() + 6);
    const uint32_t sample_count = read_le32(packet.payload.data() + 8);
    const uint16_t chunk_count = read_le16(packet.payload.data() + 12);
    const size_t expected_sample_count = static_cast<size_t>(packet.uncompressed_size / sizeof(uint16_t));
    if(magic != 0x4c385150u || version != 1 || raw_step == 0 || sample_count != expected_sample_count || chunk_count == 0) {
        throw std::runtime_error("invalid pq8lz4 depth header");
    }
    const size_t table_size = 20ull + static_cast<size_t>(chunk_count) * 12ull;
    if(table_size > packet.payload.size()) {
        throw std::runtime_error("truncated pq8lz4 depth table");
    }

    std::vector<PQ8ZlibChunkEntry> chunks;
    chunks.reserve(chunk_count);
    size_t compressed_offset = table_size;
    uint32_t expected_offset = 0;
    for(uint16_t i = 0; i < chunk_count; ++i) {
        const uint8_t *entry = packet.payload.data() + 20ull + static_cast<size_t>(i) * 12ull;
        PQ8ZlibChunkEntry chunk;
        chunk.sample_offset = read_le32(entry);
        chunk.sample_count = read_le32(entry + 4);
        chunk.compressed_size = read_le32(entry + 8);
        chunk.compressed_offset = static_cast<uint32_t>(compressed_offset);
        if(chunk.sample_offset != expected_offset || chunk.sample_count == 0 || chunk.sample_offset > sample_count
           || chunk.sample_count > sample_count - chunk.sample_offset || chunk.compressed_size == 0
           || compressed_offset > packet.payload.size() || chunk.compressed_size > packet.payload.size() - compressed_offset
           || chunk.sample_count > static_cast<uint32_t>(std::numeric_limits<int>::max())
           || chunk.compressed_size > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
            throw std::runtime_error("invalid pq8lz4 depth chunk");
        }
        expected_offset += chunk.sample_count;
        compressed_offset += chunk.compressed_size;
        chunks.push_back(chunk);
    }
    if(expected_offset != sample_count || compressed_offset != packet.payload.size()) {
        throw std::runtime_error("invalid pq8lz4 depth layout");
    }

    std::vector<uint8_t> quantized(expected_sample_count);
    auto &api = lz4_api();
    std::vector<std::future<void>> futures;
    futures.reserve(chunks.size());
    for(const auto &chunk : chunks) {
        futures.emplace_back(std::async(std::launch::async, [&packet, &quantized, &api, chunk] {
            const int decoded_size =
                api.decompress_safe(reinterpret_cast<const char *>(packet.payload.data() + chunk.compressed_offset),
                                    reinterpret_cast<char *>(quantized.data() + chunk.sample_offset),
                                    static_cast<int>(chunk.compressed_size),
                                    static_cast<int>(chunk.sample_count));
            if(decoded_size != static_cast<int>(chunk.sample_count)) {
                throw std::runtime_error("pq8lz4 depth chunk decompression failed");
            }
        }));
    }
    for(auto &future : futures) {
        future.get();
    }

    std::vector<uint8_t> out(static_cast<size_t>(packet.uncompressed_size), 0);
    for(size_t i = 0; i < quantized.size(); ++i) {
        const uint8_t index = quantized[i];
        const uint32_t value = index == 0 ? 0u : std::min<uint32_t>(static_cast<uint32_t>(index) * raw_step,
                                                                    std::numeric_limits<uint16_t>::max());
        const size_t offset = i * sizeof(uint16_t);
        out[offset] = static_cast<uint8_t>(value & 0xffu);
        out[offset + 1] = static_cast<uint8_t>((value >> 8u) & 0xffu);
    }
    return out;
}

class NibbleReader {
public:
    explicit NibbleReader(const std::vector<uint8_t> &data) : data_(data) {}

    uint8_t read() {
        if(byte_index_ >= data_.size()) {
            throw std::runtime_error("truncated rvl depth payload");
        }
        const uint8_t byte = data_[byte_index_];
        uint8_t nibble = 0;
        if(read_low_) {
            nibble = byte & 0x0fu;
            read_low_ = false;
        }
        else {
            nibble = static_cast<uint8_t>((byte >> 4u) & 0x0fu);
            read_low_ = true;
            ++byte_index_;
        }
        return nibble;
    }

private:
    const std::vector<uint8_t> &data_;
    size_t byte_index_ = 0;
    bool read_low_ = true;
};

uint32_t rvl_read_vle(NibbleReader &reader) {
    uint32_t value = 0;
    uint32_t shift = 0;
    while(true) {
        const uint8_t nibble = reader.read();
        value |= static_cast<uint32_t>(nibble & 0x7u) << shift;
        if((nibble & 0x8u) == 0) {
            return value;
        }
        shift += 3u;
        if(shift >= 32u) {
            throw std::runtime_error("invalid rvl depth payload");
        }
    }
}

void write_depth_u16le(std::vector<uint8_t> &out, size_t sample_index, uint16_t value) {
    const size_t offset = sample_index * sizeof(uint16_t);
    out[offset] = static_cast<uint8_t>(value & 0xffu);
    out[offset + 1] = static_cast<uint8_t>((value >> 8u) & 0xffu);
}

std::vector<uint8_t> rvl_decompress_payload(const MediaPacket &packet) {
    if(packet.uncompressed_size == 0 || packet.uncompressed_size > kMaxReasonablePayload
       || packet.uncompressed_size % sizeof(uint16_t) != 0) {
        throw std::runtime_error("invalid rvl uncompressed depth size");
    }
    std::vector<uint8_t> out(static_cast<size_t>(packet.uncompressed_size), 0);
    const size_t sample_count = out.size() / sizeof(uint16_t);
    NibbleReader reader(packet.payload);
    int32_t previous = 0;
    size_t index = 0;
    while(index < sample_count) {
        const uint32_t zeros = rvl_read_vle(reader);
        if(zeros > sample_count - index) {
            throw std::runtime_error("invalid rvl zero run");
        }
        index += zeros;

        const uint32_t nonzeros = rvl_read_vle(reader);
        if(nonzeros > sample_count - index) {
            throw std::runtime_error("invalid rvl nonzero run");
        }
        for(uint32_t i = 0; i < nonzeros; ++i) {
            const uint32_t zigzag = rvl_read_vle(reader);
            const int32_t delta = (zigzag & 1u) != 0u ? -static_cast<int32_t>((zigzag + 1u) >> 1u)
                                                      : static_cast<int32_t>(zigzag >> 1u);
            const int32_t current = previous + delta;
            if(current < 0 || current > static_cast<int32_t>(std::numeric_limits<uint16_t>::max())) {
                throw std::runtime_error("invalid rvl depth sample");
            }
            write_depth_u16le(out, index, static_cast<uint16_t>(current));
            previous = current;
            ++index;
        }
    }
    return out;
}

uint16_t read_u16_le_checked(const std::vector<uint8_t> &data, size_t offset) {
    if(offset + 1 >= data.size()) {
        throw std::runtime_error("truncated qdelta depth payload");
    }
    return static_cast<uint16_t>(static_cast<uint32_t>(data[offset]) | (static_cast<uint32_t>(data[offset + 1]) << 8u));
}

uint32_t read_varuint_checked(const std::vector<uint8_t> &data, size_t &offset) {
    uint32_t value = 0;
    uint32_t shift = 0;
    while(offset < data.size()) {
        const uint8_t byte = data[offset++];
        value |= static_cast<uint32_t>(byte & 0x7fu) << shift;
        if((byte & 0x80u) == 0) {
            return value;
        }
        shift += 7u;
        if(shift >= 32u) {
            throw std::runtime_error("invalid qdelta varuint");
        }
    }
    throw std::runtime_error("truncated qdelta varuint");
}

int32_t zigzag_decode_i32(uint32_t value) {
    return static_cast<int32_t>((value >> 1u) ^ (~(value & 1u) + 1u));
}

std::vector<uint8_t> qdelta_decompress_payload(const MediaPacket &packet) {
    if(packet.uncompressed_size == 0 || packet.uncompressed_size > kMaxReasonablePayload
       || packet.uncompressed_size % sizeof(uint16_t) != 0) {
        throw std::runtime_error("invalid qdelta uncompressed depth size");
    }
    if(packet.payload.size() < 8 || packet.payload[0] != 'Q' || packet.payload[1] != 'D' || packet.payload[2] != 'L'
       || packet.payload[3] != '1') {
        throw std::runtime_error("invalid qdelta header");
    }
    const uint32_t raw_step = std::max<uint32_t>(1, read_u16_le_checked(packet.payload, 4));
    std::vector<uint8_t> out(static_cast<size_t>(packet.uncompressed_size), 0);
    const size_t sample_count = out.size() / sizeof(uint16_t);
    size_t offset = 8;
    size_t index = 0;
    int32_t previous = 0;
    while(index < sample_count) {
        if(offset >= packet.payload.size()) {
            throw std::runtime_error("truncated qdelta depth payload");
        }
        const uint8_t token = packet.payload[offset++];
        if(token == 0) {
            const uint32_t zeros = read_varuint_checked(packet.payload, offset);
            if(zeros > sample_count - index) {
                throw std::runtime_error("invalid qdelta zero run");
            }
            index += zeros;
            previous = 0;
            continue;
        }
        if(token != 1) {
            throw std::runtime_error("invalid qdelta token");
        }
        if(offset >= packet.payload.size()) {
            throw std::runtime_error("truncated qdelta run");
        }
        const uint32_t nonzeros = packet.payload[offset++];
        if(nonzeros == 0 || nonzeros > sample_count - index) {
            throw std::runtime_error("invalid qdelta nonzero run");
        }
        for(uint32_t i = 0; i < nonzeros; ++i) {
            const int32_t delta = zigzag_decode_i32(read_varuint_checked(packet.payload, offset));
            const int32_t quantized = previous + delta;
            if(quantized < 0) {
                throw std::runtime_error("invalid qdelta depth sample");
            }
            const uint32_t raw_value = std::min<uint32_t>(static_cast<uint32_t>(quantized) * raw_step, std::numeric_limits<uint16_t>::max());
            write_depth_u16le(out, index, static_cast<uint16_t>(raw_value));
            previous = quantized;
            ++index;
        }
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
        MediaPacket decoded = media_packet_metadata_only(packet);
        decoded.payload = zlib_decompress_payload(packet);
        decoded.payload_size = decoded.payload.size();
        decoded.codec_or_compression = "none";
        return decoded;
    }
    if(packet.codec_or_compression == "rvl") {
        MediaPacket decoded = media_packet_metadata_only(packet);
        decoded.payload = rvl_decompress_payload(packet);
        decoded.payload_size = decoded.payload.size();
        decoded.codec_or_compression = "none";
        return decoded;
    }
    if(packet.codec_or_compression == "qdelta") {
        MediaPacket decoded = media_packet_metadata_only(packet);
        decoded.payload = qdelta_decompress_payload(packet);
        decoded.payload_size = decoded.payload.size();
        decoded.codec_or_compression = "none";
        return decoded;
    }
    if(packet.codec_or_compression == "lz4") {
        MediaPacket decoded = media_packet_metadata_only(packet);
        decoded.payload = lz4_decompress_payload(packet);
        decoded.payload_size = decoded.payload.size();
        decoded.codec_or_compression = "none";
        return decoded;
    }
    if(packet.codec_or_compression == "plz4") {
        MediaPacket decoded = media_packet_metadata_only(packet);
        decoded.payload = plz4_decompress_payload(packet);
        decoded.payload_size = decoded.payload.size();
        decoded.codec_or_compression = "none";
        return decoded;
    }
    if(packet.codec_or_compression == "pzlib") {
        MediaPacket decoded = media_packet_metadata_only(packet);
        decoded.payload = pzlib_decompress_payload(packet);
        decoded.payload_size = decoded.payload.size();
        decoded.codec_or_compression = "none";
        return decoded;
    }
    if(packet.codec_or_compression == "q8lz4") {
        MediaPacket decoded = media_packet_metadata_only(packet);
        decoded.payload = q8lz4_decompress_payload(packet);
        decoded.payload_size = decoded.payload.size();
        decoded.codec_or_compression = "none";
        return decoded;
    }
    if(packet.codec_or_compression == "q8zlib") {
        MediaPacket decoded = media_packet_metadata_only(packet);
        decoded.payload = q8zlib_decompress_payload(packet);
        decoded.payload_size = decoded.payload.size();
        decoded.codec_or_compression = "none";
        return decoded;
    }
    if(packet.codec_or_compression == "pq12zlib") {
        MediaPacket decoded = media_packet_metadata_only(packet);
        decoded.payload = pq12zlib_decompress_payload(packet);
        decoded.payload_size = decoded.payload.size();
        decoded.codec_or_compression = "none";
        return decoded;
    }
    if(packet.codec_or_compression == "pq8zlib") {
        MediaPacket decoded = media_packet_metadata_only(packet);
        decoded.payload = pq8zlib_decompress_payload(packet);
        decoded.payload_size = decoded.payload.size();
        decoded.codec_or_compression = "none";
        return decoded;
    }
    if(packet.codec_or_compression == "pq8lz4") {
        MediaPacket decoded = media_packet_metadata_only(packet);
        decoded.payload = pq8lz4_decompress_payload(packet);
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

std::string process_status_text(int status) {
    if(status == 0) {
        return "exit=0";
    }
    if(status == -1) {
        return std::string("pclose failed: ") + std::strerror(errno);
    }
    if(WIFEXITED(status)) {
        return "exit=" + std::to_string(WEXITSTATUS(status));
    }
    if(WIFSIGNALED(status)) {
        return "signal=" + std::to_string(WTERMSIG(status));
    }
    return "status=" + std::to_string(status);
}

struct FrameInfo {
    bool valid = false;
    uint64_t frame_id = 0;
    uint64_t timestamp_us = 0;
    uint64_t system_timestamp_us = 0;
    int32_t exposure_us = -1;
    int32_t gain = -1;
    int32_t auto_exposure = -1;
    int32_t actual_fps = -1;
};

struct PendingRgbPacketInfo {
    MediaPacket packet;
    uint64_t local_time_us = 0;
    size_t payload_size = 0;
    bool has_vcl = false;
};

size_t record_packet_queue_bytes(const MediaPacket &packet) {
    return sizeof(MediaPacket) + packet.sender_id.size() + packet.camera_id.size() + packet.codec_or_compression.size() + packet.payload.size();
}

struct RecordJob {
    MediaPacket packet;
    std::string sender_id;
    std::string camera_id;
    std::string camera_name;
    std::string storage_key;
    std::string file_prefix;
    std::string announce_json;
    size_t queue_bytes = 0;
    uint64_t enqueue_us = 0;
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

void write_optional_us(std::ostream &out, uint64_t value) {
    if(value > 0) {
        out << value;
    }
}

void write_optional_delta_us(std::ostream &out, uint64_t newer_us, uint64_t older_us) {
    if(newer_us > 0 && older_us > 0) {
        out << (newer_us >= older_us ? static_cast<int64_t>(newer_us - older_us)
                                      : -static_cast<int64_t>(older_us - newer_us));
    }
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
                       "rgb_exposure_us,rgb_gain,rgb_auto_exposure,rgb_actual_fps,rgb_frame_interval_us,codec_or_compression,"
                       "sender_capture_host_timestamp_us,sender_timing_bound_timestamp_us,sender_encode_start_timestamp_us,"
                       "sender_encode_done_timestamp_us,sender_packet_queued_timestamp_us,receiver_minus_frame_system_us,"
                       "sender_capture_to_timing_bound_us,sender_timing_bound_to_encode_start_us,sender_encode_duration_us,"
                       "sender_encode_done_to_packet_queued_us,sender_packet_queued_to_receiver_us,"
                       "sender_id,camera_id,sender_timestamp_us,sender_system_timestamp_us,receiver_receive_timestamp_us,"
                       "clock_sync_valid,sender_offset_us,sender_delay_us,sender_drift_ppm,global_timestamp_us\n";
        rgb_recorded_frames_csv_.open(file_path("rgb_recorded_frames.csv"), std::ios::out | std::ios::trunc);
        rgb_recorded_frames_csv_
            << "video_frame_index,local_time_us,frame_id,timestamp_us,frame_system_timestamp_us,width,height,payload_size,"
               "packet_system_timestamp_us,rgb_exposure_us,rgb_gain,rgb_auto_exposure,rgb_actual_fps,codec_or_compression,"
               "sender_capture_host_timestamp_us,sender_timing_bound_timestamp_us,sender_encode_start_timestamp_us,"
               "sender_encode_done_timestamp_us,sender_packet_queued_timestamp_us,receiver_minus_frame_system_us,"
               "sender_capture_to_timing_bound_us,sender_timing_bound_to_encode_start_us,sender_encode_duration_us,"
               "sender_encode_done_to_packet_queued_us,sender_packet_queued_to_receiver_us,"
               "sender_id,camera_id,sender_timestamp_us,sender_system_timestamp_us,receiver_receive_timestamp_us,"
               "clock_sync_valid,sender_offset_us,sender_delay_us,sender_drift_ppm,global_timestamp_us\n";

        rgb_debug_path_ = file_path("rgb_debug.h264");
        rgb_debug_.open(rgb_debug_path_, std::ios::binary | std::ios::out | std::ios::trunc);
        if(!rgb_debug_) {
            logger.warn("failed to open RGB H264 recovery file: " + rgb_debug_path_.string());
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
        if(rgb_recorded_frames_csv_) {
            rgb_recorded_frames_csv_.flush();
            rgb_recorded_frames_csv_.close();
        }
        merge_rgb_recorded_frames_into_frames(logger);
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
            logger.warn("rgb ffmpeg exited with non-zero status (" + process_status_text(rgb_rc) + ") for segment: " + directory_);
        }
        if(depth_rc != 0) {
            logger.warn("depth ffmpeg exited with non-zero status (" + process_status_text(depth_rc) + ") for segment: " + directory_);
        }
        finalize_completed_media(cfg);
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
        rgb_pending_infos_.clear();
        rgb_pending_has_vcl_ = false;
        rgb_pending_has_decodable_start_ = false;
        rgb_recorded_frame_index_ = 0;
        rgb_debug_path_.clear();
        depth_pending_.clear();
        depth_pending_bytes_ = 0;
        last_rgb_frame_interval_us_.reset();
        rgb_fps_probe_.reset();
        depth_fps_probe_.reset();
        rgb_record_fps_ = 0.0;
        depth_record_fps_ = 0.0;
        rgb_stats_.reset();
        rgb_recorded_stats_.reset();
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
        const auto stream = std::string(stream_type_name(packet.stream_type));
        const bool rgb_packet_has_vcl = packet.stream_type == StreamType::rgb && h264_payload_has_vcl_nal(packet.payload);
        if(packet.stream_type == StreamType::rgb) {
            write_rgb_packet(cfg, packet, packet_local_us, logger);
            if(rgb_packet_has_vcl) {
                std::optional<int64_t> frame_interval_us;
                if(last_rgb_.valid && last_rgb_.system_timestamp_us > 0 && packet.system_timestamp_us > 0) {
                    frame_interval_us = packet.system_timestamp_us >= last_rgb_.system_timestamp_us
                                            ? static_cast<int64_t>(packet.system_timestamp_us - last_rgb_.system_timestamp_us)
                                            : -static_cast<int64_t>(last_rgb_.system_timestamp_us - packet.system_timestamp_us);
                }
                last_rgb_ = FrameInfo{true, packet.frame_id, packet.timestamp_us, packet.system_timestamp_us, packet.rgb_exposure_us,
                                      packet.rgb_gain, packet.rgb_auto_exposure, packet.rgb_actual_fps};
                last_rgb_frame_interval_us_ = frame_interval_us;
                rgb_stats_.add(packet, packet_local_us);
            }
        }
        else if(packet.stream_type == StreamType::depth_raw) {
            if(depth_debug_) {
                depth_debug_.write(reinterpret_cast<const char *>(packet.payload.data()), static_cast<std::streamsize>(packet.payload.size()));
            }
            write_depth_packet(cfg, packet, packet_local_us, logger);
            last_depth_ = FrameInfo{true, packet.frame_id, packet.timestamp_us, packet.system_timestamp_us};
            depth_stats_.add(packet, packet_local_us);
        }

        if(frames_csv_ && (packet.stream_type != StreamType::rgb || rgb_packet_has_vcl)) {
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
                const bool use_system_pair_delta = last_rgb_.system_timestamp_us > 0 && last_depth_.system_timestamp_us > 0;
                const uint64_t rgb_pair_us = use_system_pair_delta ? last_rgb_.system_timestamp_us : last_rgb_.timestamp_us;
                const uint64_t depth_pair_us = use_system_pair_delta ? last_depth_.system_timestamp_us : last_depth_.timestamp_us;
                const auto delta = rgb_pair_us > depth_pair_us ? rgb_pair_us - depth_pair_us : depth_pair_us - rgb_pair_us;
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
            frames_csv_ << ',' << packet.frame_id << ',' << packet.timestamp_us << ',' << packet.system_timestamp_us << ',';
            if(last_rgb_.valid && last_rgb_.exposure_us >= 0) {
                frames_csv_ << last_rgb_.exposure_us;
            }
            frames_csv_ << ',';
            if(last_rgb_.valid && last_rgb_.gain >= 0) {
                frames_csv_ << last_rgb_.gain;
            }
            frames_csv_ << ',';
            if(last_rgb_.valid && last_rgb_.auto_exposure >= 0) {
                frames_csv_ << last_rgb_.auto_exposure;
            }
            frames_csv_ << ',';
            if(last_rgb_.valid && last_rgb_.actual_fps >= 0) {
                frames_csv_ << last_rgb_.actual_fps;
            }
            frames_csv_ << ',';
            if(last_rgb_frame_interval_us_) {
                frames_csv_ << *last_rgb_frame_interval_us_;
            }
            frames_csv_ << ',' << packet.codec_or_compression;
            write_pipeline_diagnostics_columns(frames_csv_, packet, packet_local_us);
            write_clock_sync_columns(frames_csv_, packet);
            frames_csv_ << '\n';
        }
    }

private:
    std::filesystem::path file_path(const std::string &basename) const {
        return std::filesystem::path(directory_) / prefixed_filename(file_prefix_, basename);
    }

    static void write_pipeline_diagnostics_columns(std::ostream &csv, const MediaPacket &packet, uint64_t packet_local_us) {
        csv << ',';
        write_optional_us(csv, packet.sender_capture_host_timestamp_us);
        csv << ',';
        write_optional_us(csv, packet.sender_timing_bound_timestamp_us);
        csv << ',';
        write_optional_us(csv, packet.sender_encode_start_timestamp_us);
        csv << ',';
        write_optional_us(csv, packet.sender_encode_done_timestamp_us);
        csv << ',';
        write_optional_us(csv, packet.sender_packet_queued_timestamp_us);
        csv << ',';
        write_optional_delta_us(csv, packet_local_us, packet.system_timestamp_us);
        csv << ',';
        write_optional_delta_us(csv, packet.sender_timing_bound_timestamp_us, packet.sender_capture_host_timestamp_us);
        csv << ',';
        write_optional_delta_us(csv, packet.sender_encode_start_timestamp_us, packet.sender_timing_bound_timestamp_us);
        csv << ',';
        write_optional_delta_us(csv, packet.sender_encode_done_timestamp_us, packet.sender_encode_start_timestamp_us);
        csv << ',';
        write_optional_delta_us(csv, packet.sender_packet_queued_timestamp_us, packet.sender_encode_done_timestamp_us);
        csv << ',';
        write_optional_delta_us(csv, packet_local_us, packet.sender_packet_queued_timestamp_us);
    }

    static void write_clock_sync_columns(std::ostream &csv, const MediaPacket &packet) {
        csv << ',' << packet.sender_id
            << ',' << packet.camera_id
            << ',' << packet.timestamp_us
            << ',' << packet.system_timestamp_us
            << ',' << packet.receiver_receive_timestamp_us
            << ',' << (packet.clock_sync_valid ? 1 : 0)
            << ',' << packet.sender_offset_us
            << ',' << packet.sender_delay_us
            << ',' << packet.sender_drift_ppm
            << ',' << packet.global_timestamp_us;
    }

    bool write_rgb_recovery_bytes(const uint8_t *data, size_t size, Logger &logger) {
        if(!rgb_debug_ || size == 0) {
            return false;
        }
        rgb_debug_.write(reinterpret_cast<const char *>(data), static_cast<std::streamsize>(size));
        if(!rgb_debug_) {
            logger.warn("RGB H264 recovery write failed: " + rgb_debug_path_.string());
            rgb_debug_.close();
            return false;
        }
        return true;
    }

    void write_rgb_recorded_frame(const MediaPacket &packet, uint64_t packet_local_us, size_t recorded_payload_size,
                                  bool payload_known_vcl = false) {
        if(!rgb_recorded_frames_csv_ || (!payload_known_vcl && !h264_payload_has_vcl_nal(packet.payload))) {
            return;
        }
        rgb_recorded_frames_csv_ << rgb_recorded_frame_index_++ << ',' << packet_local_us << ',' << packet.frame_id << ','
                                 << packet.timestamp_us << ',' << packet.system_timestamp_us << ',' << packet.width << ','
                                 << packet.height << ',' << recorded_payload_size << ',' << packet.system_timestamp_us << ',';
        if(packet.rgb_exposure_us >= 0) {
            rgb_recorded_frames_csv_ << packet.rgb_exposure_us;
        }
        rgb_recorded_frames_csv_ << ',';
        if(packet.rgb_gain >= 0) {
            rgb_recorded_frames_csv_ << packet.rgb_gain;
        }
        rgb_recorded_frames_csv_ << ',';
        if(packet.rgb_auto_exposure >= 0) {
            rgb_recorded_frames_csv_ << packet.rgb_auto_exposure;
        }
        rgb_recorded_frames_csv_ << ',';
        if(packet.rgb_actual_fps >= 0) {
            rgb_recorded_frames_csv_ << packet.rgb_actual_fps;
        }
        rgb_recorded_frames_csv_ << ',' << packet.codec_or_compression;
        write_pipeline_diagnostics_columns(rgb_recorded_frames_csv_, packet, packet_local_us);
        write_clock_sync_columns(rgb_recorded_frames_csv_, packet);
        rgb_recorded_frames_csv_ << '\n';
        rgb_recorded_stats_.add(packet, packet_local_us);
    }

    void write_pending_rgb_recorded_frames() {
        for(const auto &info : rgb_pending_infos_) {
            if(info.has_vcl) {
                write_rgb_recorded_frame(info.packet, info.local_time_us, info.payload_size, true);
            }
        }
    }

    static std::vector<std::string> split_csv_line(const std::string &line) {
        std::vector<std::string> fields;
        std::string field;
        std::stringstream input(line);
        while(std::getline(input, field, ',')) {
            fields.push_back(field);
        }
        if(!line.empty() && line.back() == ',') {
            fields.emplace_back();
        }
        return fields;
    }

    static std::map<std::string, size_t> csv_header_index(const std::vector<std::string> &header) {
        std::map<std::string, size_t> index;
        for(size_t i = 0; i < header.size(); ++i) {
            index[header[i]] = i;
        }
        return index;
    }

    static std::string csv_value(const std::vector<std::string> &row, const std::map<std::string, size_t> &index,
                                 const std::string &name) {
        const auto found = index.find(name);
        if(found == index.end() || found->second >= row.size()) {
            return {};
        }
        return row[found->second];
    }

    static std::string rgb_record_key(const std::vector<std::string> &row, const std::map<std::string, size_t> &index) {
        const auto frame_id = csv_value(row, index, "frame_id");
        const auto timestamp_us = csv_value(row, index, "timestamp_us");
        const auto frame_system_timestamp_us = csv_value(row, index, "frame_system_timestamp_us");
        if(frame_id.empty() || timestamp_us.empty() || frame_system_timestamp_us.empty()) {
            return {};
        }
        return frame_id + "\t" + timestamp_us + "\t" + frame_system_timestamp_us;
    }

    void merge_rgb_recorded_frames_into_frames(Logger &logger) {
        const auto frames_path = file_path("frames.csv");
        const auto recorded_path = file_path("rgb_recorded_frames.csv");
        std::error_code ec;
        if(!std::filesystem::exists(frames_path, ec)) {
            return;
        }

        std::ifstream frames_in(frames_path);
        if(!frames_in) {
            logger.warn("failed to reopen frames.csv for RGB frame index merge: " + frames_path.string());
            return;
        }

        std::map<std::string, std::pair<std::string, std::string>> recorded_by_key;
        size_t duplicate_recorded_keys = 0;
        std::string first_duplicate_recorded_key;
        if(std::filesystem::exists(recorded_path, ec)) {
            std::ifstream recorded_in(recorded_path);
            if(recorded_in) {
                std::string recorded_header_line;
                if(std::getline(recorded_in, recorded_header_line)) {
                    const auto recorded_header = split_csv_line(recorded_header_line);
                    const auto recorded_index = csv_header_index(recorded_header);
                    std::string line;
                    while(std::getline(recorded_in, line)) {
                        const auto row = split_csv_line(line);
                        const std::string key = rgb_record_key(row, recorded_index);
                        if(!key.empty()) {
                            auto inserted = recorded_by_key.emplace(
                                key, std::make_pair(csv_value(row, recorded_index, "video_frame_index"),
                                                    csv_value(row, recorded_index, "payload_size")));
                            if(!inserted.second) {
                                duplicate_recorded_keys++;
                                if(first_duplicate_recorded_key.empty()) {
                                    first_duplicate_recorded_key = key;
                                }
                            }
                        }
                    }
                }
            }
            else {
                logger.warn("failed to reopen rgb_recorded_frames.csv for merge: " + recorded_path.string());
            }
        }
        if(duplicate_recorded_keys > 0) {
            logger.warn("duplicate RGB recorded frame keys ignored during merge path=" + recorded_path.string()
                        + " duplicates=" + std::to_string(duplicate_recorded_keys)
                        + " first_key=\"" + first_duplicate_recorded_key + "\"");
        }

        const auto tmp_path = frames_path.string() + ".merge_tmp";
        std::ofstream merged(tmp_path, std::ios::out | std::ios::trunc);
        if(!merged) {
            logger.warn("failed to create merged frames.csv tmp: " + tmp_path);
            return;
        }

        std::string header_line;
        if(!std::getline(frames_in, header_line)) {
            logger.warn("frames.csv is empty during RGB frame index merge: " + frames_path.string());
            return;
        }
        const auto header = split_csv_line(header_line);
        const auto index = csv_header_index(header);
        merged << header_line << ",rgb_recorded,rgb_video_frame_index,rgb_recorded_payload_size\n";

        std::string line;
        std::set<std::string> merged_rgb_keys;
        size_t duplicate_frame_rows = 0;
        size_t dropped_unrecorded_rgb_rows = 0;
        std::string first_duplicate_frame_key;
        while(std::getline(frames_in, line)) {
            const auto row = split_csv_line(line);
            const bool is_rgb = csv_value(row, index, "stream_type") == "rgb";
            if(is_rgb) {
                const auto key = rgb_record_key(row, index);
                const bool duplicate_frame_key = !key.empty() && !merged_rgb_keys.insert(key).second;
                if(duplicate_frame_key) {
                    duplicate_frame_rows++;
                    if(first_duplicate_frame_key.empty()) {
                        first_duplicate_frame_key = key;
                    }
                }
                const auto found = duplicate_frame_key ? recorded_by_key.end() : recorded_by_key.find(key);
                if(found != recorded_by_key.end()) {
                    merged << line << ",1," << found->second.first << ',' << found->second.second << '\n';
                }
                else {
                    dropped_unrecorded_rgb_rows++;
                }
            }
            else {
                merged << line << ",,,\n";
            }
        }
        if(duplicate_frame_rows > 0) {
            logger.warn("duplicate RGB frame keys marked unrecorded during frames.csv merge path=" + frames_path.string()
                        + " duplicates=" + std::to_string(duplicate_frame_rows)
                        + " first_key=\"" + first_duplicate_frame_key + "\"");
        }
        if(dropped_unrecorded_rgb_rows > 0) {
            logger.warn("unrecorded RGB frame rows dropped during frames.csv merge path=" + frames_path.string()
                        + " dropped=" + std::to_string(dropped_unrecorded_rgb_rows));
        }
        merged.close();
        if(!merged) {
            logger.warn("failed to finish merged frames.csv tmp: " + tmp_path);
            std::filesystem::remove(tmp_path, ec);
            return;
        }
        std::filesystem::rename(tmp_path, frames_path, ec);
        if(ec) {
            logger.warn("failed to replace frames.csv with merged RGB frame index version: " + ec.message());
            std::filesystem::remove(tmp_path, ec);
            return;
        }
        if(std::filesystem::exists(recorded_path, ec)) {
            std::filesystem::remove(recorded_path, ec);
            if(ec) {
                logger.warn("failed to remove merged rgb_recorded_frames.csv: " + ec.message());
            }
        }
    }

    void trim_pending_rgb_infos_to_offset(size_t byte_offset) {
        size_t consumed = 0;
        size_t erase_count = 0;
        while(erase_count < rgb_pending_infos_.size()) {
            const size_t next = consumed + rgb_pending_infos_[erase_count].payload_size;
            if(next > byte_offset) {
                break;
            }
            consumed = next;
            ++erase_count;
        }
        if(erase_count > 0) {
            rgb_pending_infos_.erase(rgb_pending_infos_.begin(),
                                     rgb_pending_infos_.begin() + static_cast<std::ptrdiff_t>(erase_count));
        }
        if(!rgb_pending_infos_.empty() && byte_offset > consumed) {
            const size_t trimmed_from_first = byte_offset - consumed;
            auto &first = rgb_pending_infos_.front();
            if(trimmed_from_first < first.payload_size) {
                first.payload_size -= trimmed_from_first;
            }
        }
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
                                    " -f h264 -i pipe:0 -c:v copy -movflags " + kRgbMp4RecordMuxFlags +
                                    " -flush_packets 1 " + rgb_mp4 +
                                    " 2>>" + ffmpeg_log;
        rgb_pipe_.open(rgb_cmd, logger);
    }

    void write_rgb_packet(const Config &cfg, const MediaPacket &packet, uint64_t packet_local_us, Logger &logger) {
        if(rgb_pipe_.active()) {
            const bool recovery_ok = write_rgb_recovery_bytes(packet.payload.data(), packet.payload.size(), logger);
            const bool pipe_ok = rgb_pipe_.write(packet.payload.data(), packet.payload.size(), logger);
            if(pipe_ok || recovery_ok) {
                write_rgb_recorded_frame(packet, packet_local_us, packet.payload.size());
            }
            return;
        }

        if(rgb_pending_has_decodable_start_ && rgb_pending_.size() + packet.payload.size() > kMaxPendingRgbRecordBytes) {
            flush_rgb_pending(cfg, logger);
        }
        if(rgb_pipe_.active()) {
            const bool recovery_ok = write_rgb_recovery_bytes(packet.payload.data(), packet.payload.size(), logger);
            const bool pipe_ok = rgb_pipe_.write(packet.payload.data(), packet.payload.size(), logger);
            if(pipe_ok || recovery_ok) {
                write_rgb_recorded_frame(packet, packet_local_us, packet.payload.size());
            }
            return;
        }
        if(rgb_pending_.size() + packet.payload.size() > kMaxPendingRgbRecordBytes) {
            rgb_pending_.clear();
            rgb_pending_infos_.clear();
            rgb_pending_has_vcl_ = false;
            rgb_pending_has_decodable_start_ = false;
            rgb_fps_probe_.reset();
        }
        const bool packet_has_vcl = h264_payload_has_vcl_nal(packet.payload);
        rgb_pending_infos_.push_back(PendingRgbPacketInfo{media_packet_metadata_only(packet), packet_local_us, packet.payload.size(), packet_has_vcl});
        rgb_pending_.insert(rgb_pending_.end(), packet.payload.begin(), packet.payload.end());

        if(packet_has_vcl) {
            rgb_pending_has_vcl_ = true;
            const uint64_t fps_probe_us = packet.system_timestamp_us > 0 ? packet.system_timestamp_us : packet_local_us;
            rgb_fps_probe_.add(fps_probe_us);
        }
        if(!rgb_pending_has_decodable_start_) {
            if(const auto decodable_start = h264_decodable_start_offset(rgb_pending_)) {
                if(*decodable_start > 0) {
                    trim_pending_rgb_infos_to_offset(*decodable_start);
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
            const bool recovery_ok = write_rgb_recovery_bytes(rgb_pending_.data(), rgb_pending_.size(), logger);
            const bool pipe_ok = rgb_pipe_.write(rgb_pending_.data(), rgb_pending_.size(), logger);
            if(pipe_ok || recovery_ok) {
                write_pending_rgb_recorded_frames();
            }
            rgb_pending_.clear();
            rgb_pending_infos_.clear();
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
        std::filesystem::path raw_h264_path;
        double raw_h264_fps = 30.0;
        bool keep_raw_h264 = false;
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

    static bool is_mp4_file(const std::filesystem::path &path) {
        return path.extension().string() == ".mp4";
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

    static bool file_size_nonzero(const std::filesystem::path &path) {
        std::error_code ec;
        return std::filesystem::exists(path, ec) && std::filesystem::is_regular_file(path, ec) &&
               std::filesystem::file_size(path, ec) > 0 && !ec;
    }

    static bool mp4_has_moov_atom(const std::filesystem::path &path) {
        std::ifstream input(path, std::ios::binary);
        if(!input) {
            return false;
        }
        constexpr size_t kChunkSize = 64 * 1024;
        std::vector<char> buffer(kChunkSize + 3);
        size_t carry = 0;
        while(input) {
            input.read(buffer.data() + static_cast<std::streamoff>(carry), static_cast<std::streamsize>(kChunkSize));
            const auto read_count = input.gcount();
            const size_t total = carry + static_cast<size_t>(std::max<std::streamsize>(read_count, 0));
            if(total >= 4) {
                const char *begin = buffer.data();
                const char *end = buffer.data() + total;
                constexpr char kMoovAtom[] = {'m', 'o', 'o', 'v'};
                if(std::search(begin, end, std::begin(kMoovAtom), std::end(kMoovAtom)) != end) {
                    return true;
                }
                carry = std::min<size_t>(3, total);
                std::memmove(buffer.data(), end - static_cast<std::ptrdiff_t>(carry), carry);
            }
            if(read_count <= 0) {
                break;
            }
        }
        return false;
    }

    static bool media_file_readable(const std::string &ffprobe_path, const std::filesystem::path &media_path) {
        if(!file_size_nonzero(media_path)) {
            return false;
        }
        if(media_path.extension().string() == ".mp4" && !mp4_has_moov_atom(media_path)) {
            return false;
        }
        return probe_media_duration_seconds(ffprobe_path, media_path).has_value();
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

    static std::filesystem::path local_retime_tmp_path(const std::filesystem::path &media_path, const std::string &tag) {
        const auto tmp_dir = std::filesystem::temp_directory_path() / "gwv3_receiver_media_tmp";
        std::error_code ec;
        std::filesystem::create_directories(tmp_dir, ec);
        const auto stem = media_path.stem().string();
        const auto extension = media_path.extension().string();
        for(int attempt = 0; attempt < 100; ++attempt) {
            const auto name = stem + "." + tag + "." + std::to_string(static_cast<long long>(::getpid())) + "." +
                              std::to_string(static_cast<unsigned long long>(now_us())) + "." + std::to_string(attempt) + extension;
            auto candidate = tmp_dir / name;
            if(!std::filesystem::exists(candidate, ec)) {
                return candidate;
            }
        }
        return tmp_dir / (stem + "." + tag + "." + std::to_string(static_cast<unsigned long long>(now_us())) + extension);
    }

    static bool replace_media_from_local_tmp(const RetimingTask &task, const RetimingEntry &entry,
                                             const std::filesystem::path &local_tmp_path, const std::string &operation) {
        if(!media_file_readable(task.ffprobe_path, local_tmp_path)) {
            append_retime_log(task.log_path, entry.stream_name + " " + operation + " local tmp validation failed");
            return false;
        }

        const auto parent = entry.media_path.parent_path();
        const auto replace_tmp_path = parent / (entry.media_path.stem().string() + "." + operation + "_replace_tmp" +
                                                entry.media_path.extension().string());
        std::error_code ec;
        std::filesystem::remove(replace_tmp_path, ec);
        std::filesystem::copy_file(local_tmp_path, replace_tmp_path, std::filesystem::copy_options::overwrite_existing, ec);
        if(ec || !media_file_readable(task.ffprobe_path, replace_tmp_path)) {
            append_retime_log(task.log_path, entry.stream_name + " " + operation + " staging copy failed");
            std::filesystem::remove(replace_tmp_path, ec);
            return false;
        }

        std::filesystem::rename(replace_tmp_path, entry.media_path, ec);
        if(ec) {
            append_retime_log(task.log_path, entry.stream_name + " " + operation + " replace failed: " + ec.message());
            std::filesystem::remove(replace_tmp_path, ec);
            return false;
        }

        set_file_mtime_to_start(entry.media_path, task.start_us);
        if(!media_file_readable(task.ffprobe_path, entry.media_path)) {
            append_retime_log(task.log_path, entry.stream_name + " " + operation + " final validation failed");
            return false;
        }
        return true;
    }

    static bool repair_mp4_from_h264(const RetimingTask &task, const RetimingEntry &entry) {
        if(entry.raw_h264_path.empty() || !std::filesystem::exists(entry.raw_h264_path)) {
            append_retime_log(task.log_path, entry.stream_name + " repair skipped: raw h264 missing");
            return false;
        }
        const auto tmp_path = local_retime_tmp_path(entry.media_path, "repair");
        std::error_code ec;
        std::filesystem::remove(tmp_path, ec);

        append_retime_log(task.log_path, entry.stream_name + " repair start from " + entry.raw_h264_path.filename().string());
        const std::string cmd = shell_quote(task.ffmpeg_path) + " -hide_banner -loglevel warning -y -fflags +genpts -r " +
                                format_fps(entry.raw_h264_fps) + " -f h264 -i " + shell_quote(entry.raw_h264_path.string()) +
                                " -c:v copy -movflags " + kRgbMp4FinalMuxFlags + " " + shell_quote(tmp_path.string()) +
                                " 2>>" + shell_quote(task.log_path.string());
        const int rc = std::system(cmd.c_str());
        if(rc != 0 || !file_size_nonzero(tmp_path)) {
            append_retime_log(task.log_path, entry.stream_name + " repair failed");
            std::filesystem::remove(tmp_path, ec);
            return false;
        }

        const bool replaced = replace_media_from_local_tmp(task, entry, tmp_path, "repair");
        std::filesystem::remove(tmp_path, ec);
        if(!replaced) {
            return false;
        }
        append_retime_log(task.log_path, entry.stream_name + " repair done");
        return true;
    }

    static bool retime_media_file(const RetimingTask &task, const RetimingEntry &entry) {
        if(!std::filesystem::exists(entry.media_path)) {
            append_retime_log(task.log_path, entry.stream_name + " retime skipped: media file missing");
            return false;
        }

        double scale = entry.fallback_scale;
        if(!media_file_readable(task.ffprobe_path, entry.media_path)) {
            append_retime_log(task.log_path, entry.stream_name + " initial validation failed");
            if(entry.stream_name != "rgb" || !repair_mp4_from_h264(task, entry)) {
                return false;
            }
        }
        auto current_duration = probe_media_duration_seconds(task.ffprobe_path, entry.media_path);
        if(!current_duration && entry.stream_name == "rgb" && repair_mp4_from_h264(task, entry)) {
            current_duration = probe_media_duration_seconds(task.ffprobe_path, entry.media_path);
        }
        if(current_duration) {
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
        const bool should_retime = should_retime_media(scale);
        const bool should_finalize_mp4 = is_mp4_file(entry.media_path);
        if(!should_retime && !should_finalize_mp4) {
            append_retime_log(task.log_path, entry.stream_name + " retime skipped: scale near 1 or outside 0.8..1.25");
            if(!media_file_readable(task.ffprobe_path, entry.media_path)) {
                append_retime_log(task.log_path, entry.stream_name + " skipped retime final validation failed");
                return entry.stream_name == "rgb" && repair_mp4_from_h264(task, entry);
            }
            append_retime_log(task.log_path, entry.stream_name + " final validation ok");
            return true;
        }

        const auto tmp_path = local_retime_tmp_path(entry.media_path, "retime");
        std::error_code ec;
        std::filesystem::remove(tmp_path, ec);

        append_retime_log(task.log_path,
                          entry.stream_name + (should_retime ? " retime start scale=" + format_scale(scale) : " finalize mp4 start"));
        std::string cmd = shell_quote(task.ffmpeg_path) + " -hide_banner -loglevel warning -y ";
        if(should_retime) {
            cmd += "-itsscale " + format_scale(scale) + " ";
        }
        cmd += "-i " + shell_quote(entry.media_path.string()) + " -map 0 -c copy -avoid_negative_ts make_zero";
        if(should_finalize_mp4) {
            cmd += std::string(" -movflags ") + kRgbMp4FinalMuxFlags;
        }
        cmd += " " + shell_quote(tmp_path.string()) + " 2>>" + shell_quote(task.log_path.string());
        const int rc = std::system(cmd.c_str());
        if(rc != 0 || !file_size_nonzero(tmp_path)) {
            append_retime_log(task.log_path, entry.stream_name + " retime failed");
            std::filesystem::remove(tmp_path, ec);
            return entry.stream_name == "rgb" && repair_mp4_from_h264(task, entry);
        }

        const bool replaced = replace_media_from_local_tmp(task, entry, tmp_path, "retime");
        std::filesystem::remove(tmp_path, ec);
        if(!replaced) {
            append_retime_log(task.log_path, entry.stream_name + " retime replace failed");
            return entry.stream_name == "rgb" && repair_mp4_from_h264(task, entry);
        }
        append_retime_log(task.log_path, entry.stream_name + (should_retime ? " retime done" : " finalize mp4 done"));
        if(!media_file_readable(task.ffprobe_path, entry.media_path)) {
            append_retime_log(task.log_path, entry.stream_name + " retime final validation failed");
            return entry.stream_name == "rgb" && repair_mp4_from_h264(task, entry);
        }
        append_retime_log(task.log_path, entry.stream_name + " final validation ok");
        return true;
    }

    static void run_retime_task(RetimingTask task) {
        for(const auto &entry : task.entries) {
            const bool ok = retime_media_file(task, entry);
            if(ok && entry.stream_name == "rgb" && !entry.keep_raw_h264 && !entry.raw_h264_path.empty()) {
                std::error_code ec;
                if(std::filesystem::exists(entry.raw_h264_path, ec)) {
                    std::filesystem::remove(entry.raw_h264_path, ec);
                    if(ec) {
                        append_retime_log(task.log_path, "rgb recovery h264 remove failed: " + ec.message());
                    }
                    else {
                        append_retime_log(task.log_path, "rgb recovery h264 removed after final validation");
                    }
                }
            }
        }
        set_file_mtime_to_start(task.log_path, task.start_us);
    }

    void finalize_completed_media(const Config &cfg) const {
        RetimingTask task;
        task.ffmpeg_path = cfg.ffmpeg_path;
        task.ffprobe_path = ffprobe_path_from_ffmpeg(cfg.ffmpeg_path);
        task.log_path = file_path("ffmpeg.log");
        task.start_us = start_us_;

        const auto &rgb_output_stats = rgb_recorded_stats_.frames > 0 ? rgb_recorded_stats_ : rgb_stats_;
        const double rgb_scale = media_retime_scale(rgb_record_fps_, rgb_output_stats);
        const double rgb_duration = media_duration_seconds(rgb_output_stats);
        if(rgb_duration > 0.0) {
            task.entries.push_back(
                RetimingEntry{file_path("rgb.mp4"), "rgb", rgb_duration, rgb_scale, file_path("rgb_debug.h264"), rgb_record_fps_, cfg.write_debug_h264});
        }
        const double depth_scale = media_retime_scale(depth_record_fps_, depth_stats_);
        const double depth_duration = media_duration_seconds(depth_stats_);
        if(depth_duration > 0.0) {
            task.entries.push_back(RetimingEntry{file_path("depth.mkv"), "depth", depth_duration, depth_scale, {}, 0.0, false});
        }
        if(task.entries.empty()) {
            return;
        }
        run_retime_task(std::move(task));
    }

    void write_meta(const Config &cfg, const std::string &sender_id, const std::string &camera_id, const std::string &announce_json, bool closed) {
        if(directory_.empty()) {
            return;
        }
        write_calibration(sender_id, camera_id, announce_json, closed);
        const int rgb_requested_fps = json_int_in_object(announce_json, "rgb_profile", "fps").value_or(30);
        const int depth_requested_fps = json_int_in_object(announce_json, "depth_profile", "fps").value_or(cfg.depth_fps);
        const auto &rgb_output_stats = rgb_recorded_stats_.frames > 0 ? rgb_recorded_stats_ : rgb_stats_;
        const std::string rgb_codec = !rgb_output_stats.codec_or_compression.empty()
                                          ? rgb_output_stats.codec_or_compression
                                          : json_string_in_object(announce_json, "rgb_profile", "codec").value_or("h264");
        const uint32_t rgb_width =
            rgb_output_stats.width > 0 ? rgb_output_stats.width
                                       : static_cast<uint32_t>(json_int_in_object(announce_json, "rgb_profile", "width").value_or(0));
        const uint32_t rgb_height =
            rgb_output_stats.height > 0 ? rgb_output_stats.height
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
        meta << "  \"rgb_frame_index_file\": \"" << json_escape(prefixed_filename(file_prefix_, "frames.csv")) << "\",\n";
        meta << "  \"rgb_frame_index_mode\": \"frames_csv_rgb_recorded_columns\",\n";
        meta << "  \"depth_file\": \"" << json_escape(prefixed_filename(file_prefix_, "depth.mkv")) << "\",\n";
        meta << "  \"depth_debug_file\": \"" << json_escape(prefixed_filename(file_prefix_, "depth_debug.raw")) << "\",\n";
        meta << "  \"frames_file\": \"" << json_escape(prefixed_filename(file_prefix_, "frames.csv")) << "\",\n";
        meta << "  \"calibration_file\": \"" << json_escape(prefixed_filename(file_prefix_, "calibration.json")) << "\",\n";
        meta << "  \"ffmpeg_log_file\": \"" << json_escape(prefixed_filename(file_prefix_, "ffmpeg.log")) << "\",\n";
        meta << "  \"rgb_codec\": \"" << json_escape(rgb_codec) << "\",\n";
        meta << "  \"rgb_width\": " << rgb_width << ",\n";
        meta << "  \"rgb_height\": " << rgb_height << ",\n";
        meta << "  \"rgb_fps\": " << rgb_requested_fps << ",\n";
        meta << "  \"rgb_actual_fps\": " << format_fps(rgb_output_stats.actual_fps()) << ",\n";
        meta << "  \"rgb_frames\": " << rgb_output_stats.frames << ",\n";
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
        meta << "  \"rgb_playback_fps\": " << format_fps(rgb_output_stats.actual_fps()) << ",\n";
        meta << "  \"rgb_target_duration_sec\": " << format_fps(media_duration_seconds(rgb_output_stats)) << ",\n";
        meta << "  \"rgb_retime_scale\": " << format_fps(media_retime_scale(rgb_record_fps_, rgb_output_stats)) << ",\n";
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

        std::string cmd = "touch -c -d " + shell_quote("@" + std::to_string(start_us_ / 1'000'000ull)) + " --";
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

            std::string delayed_cmd = "touch -c -d " + shell_quote("@" + std::to_string(delayed_start_us / 1'000'000ull)) + " --";
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
    std::ofstream rgb_recorded_frames_csv_;
    std::ofstream rgb_debug_;
    std::filesystem::path rgb_debug_path_;
    std::ofstream depth_debug_;
    FfmpegPipe rgb_pipe_;
    FfmpegPipe depth_pipe_;
    std::vector<uint8_t> rgb_pending_;
    std::vector<PendingRgbPacketInfo> rgb_pending_infos_;
    bool rgb_pending_has_vcl_ = false;
    bool rgb_pending_has_decodable_start_ = false;
    uint64_t rgb_recorded_frame_index_ = 0;
    std::vector<std::vector<uint8_t>> depth_pending_;
    size_t depth_pending_bytes_ = 0;
    uint32_t depth_width_ = 0;
    uint32_t depth_height_ = 0;
    FpsProbe rgb_fps_probe_;
    FpsProbe depth_fps_probe_;
    double rgb_record_fps_ = 0.0;
    double depth_record_fps_ = 0.0;
    StreamRecordStats rgb_stats_;
    StreamRecordStats rgb_recorded_stats_;
    StreamRecordStats depth_stats_;
    FrameInfo last_rgb_;
    FrameInfo last_depth_;
    std::optional<int64_t> last_rgb_frame_interval_us_;
};

struct H264StreamPacket {
    uint64_t seq = 0;
    bool has_idr = false;
    bool has_vcl = false;
    uint64_t timestamp_us = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> payload;
};

struct H264StreamBuffer {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<H264StreamPacket> packets;
    std::vector<uint8_t> header_h264;
    uint64_t next_seq = 1;
    uint64_t last_us = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct UdpReassemblyStats {
    uint64_t datagrams = 0;
    uint64_t datagram_bytes = 0;
    uint64_t invalid_datagrams = 0;
    uint64_t valid_fragments = 0;
    uint64_t duplicate_fragments = 0;
    uint64_t assemblies_started = 0;
    uint64_t completed_packets = 0;
    uint64_t completed_bytes = 0;
    uint64_t completed_rgb_packets = 0;
    uint64_t completed_depth_packets = 0;
    uint64_t completed_preview_packets = 0;
    uint64_t parse_rejected_packets = 0;
    uint64_t stream_rejected_packets = 0;
    uint64_t expired_packets = 0;
    uint64_t expired_missing_fragments = 0;
    uint64_t evicted_packets = 0;
    uint64_t evicted_missing_fragments = 0;
    uint64_t max_active_assemblies = 0;
};

void append_udp_reassembly_stats_json(std::ostringstream &out, const UdpReassemblyStats &stats, size_t active_assemblies) {
    out << "{";
    out << "\"datagrams\":" << stats.datagrams << ',';
    out << "\"datagram_bytes\":" << stats.datagram_bytes << ',';
    out << "\"invalid_datagrams\":" << stats.invalid_datagrams << ',';
    out << "\"valid_fragments\":" << stats.valid_fragments << ',';
    out << "\"duplicate_fragments\":" << stats.duplicate_fragments << ',';
    out << "\"assemblies_started\":" << stats.assemblies_started << ',';
    out << "\"completed_packets\":" << stats.completed_packets << ',';
    out << "\"completed_bytes\":" << stats.completed_bytes << ',';
    out << "\"completed_rgb_packets\":" << stats.completed_rgb_packets << ',';
    out << "\"completed_depth_packets\":" << stats.completed_depth_packets << ',';
    out << "\"completed_preview_packets\":" << stats.completed_preview_packets << ',';
    out << "\"parse_rejected_packets\":" << stats.parse_rejected_packets << ',';
    out << "\"stream_rejected_packets\":" << stats.stream_rejected_packets << ',';
    out << "\"expired_packets\":" << stats.expired_packets << ',';
    out << "\"expired_missing_fragments\":" << stats.expired_missing_fragments << ',';
    out << "\"evicted_packets\":" << stats.evicted_packets << ',';
    out << "\"evicted_missing_fragments\":" << stats.evicted_missing_fragments << ',';
    out << "\"active_assemblies\":" << active_assemblies << ',';
    out << "\"max_active_assemblies\":" << stats.max_active_assemblies;
    out << "}";
}

constexpr uint32_t kH264PreviewFrameFlagKey = 1u << 0u;
constexpr uint32_t kH264PreviewFrameFlagConfig = 1u << 1u;

std::vector<uint8_t> h264_preview_frame_header(uint32_t payload_size,
                                               uint32_t flags,
                                               uint32_t width,
                                               uint32_t height,
                                               uint64_t timestamp_us,
                                               uint64_t seq) {
    std::vector<uint8_t> header;
    header.reserve(40);
    header.push_back('G');
    header.push_back('W');
    header.push_back('H');
    header.push_back('P');
    append_u16_le(header, 1);
    append_u16_le(header, 40);
    append_u32_le(header, payload_size);
    append_u32_le(header, flags);
    append_u32_le(header, width);
    append_u32_le(header, height);
    append_u64_le(header, timestamp_us);
    append_u64_le(header, seq);
    return header;
}

bool send_h264_preview_frame(int fd,
                             const std::vector<uint8_t> &payload,
                             uint32_t flags,
                             uint32_t width,
                             uint32_t height,
                             uint64_t timestamp_us,
                             uint64_t seq) {
    if(payload.empty() || payload.size() > std::numeric_limits<uint32_t>::max()) {
        return true;
    }
    const auto header = h264_preview_frame_header(static_cast<uint32_t>(payload.size()), flags, width, height, timestamp_us, seq);
    return send_all(fd, header.data(), header.size()) && send_all(fd, payload.data(), payload.size());
}

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
    std::string status_endpoint;
    uint64_t last_media_us = 0;
    uint64_t rgb_packets = 0;
    uint64_t depth_packets = 0;
    uint64_t rgb_bytes = 0;
    uint64_t depth_bytes = 0;
    uint64_t rgb_preview_us = 0;
    uint32_t rgb_preview_width = 0;
    uint32_t rgb_preview_height = 0;
    StreamType rgb_preview_decoder_source = StreamType::rgb;
    std::vector<uint8_t> rgb_preview_prefix_h264;
    std::unique_ptr<RgbPreviewDecoder> rgb_decoder;
    uint64_t rgb_preview_requested_until_us = 0;
    uint64_t last_web_rgb_preview_control_us = 0;
    uint64_t last_web_rgb_preview_keyframe_us = 0;
    uint64_t last_rgb_preview_packet_us = 0;
    uint64_t rgb_stream_requested_until_us = 0;
    H264StreamBuffer rgb_stream;
    H264StreamBuffer rgb_preview_stream;
    uint64_t main_rgb_preview_us = 0;
    uint32_t main_rgb_preview_width = 0;
    uint32_t main_rgb_preview_height = 0;
    std::unique_ptr<RgbPreviewDecoder> main_rgb_decoder;
    uint64_t main_rgb_preview_requested_until_us = 0;
    uint64_t depth_preview_us = 0;
    uint32_t depth_preview_width = 0;
    uint32_t depth_preview_height = 0;
    uint64_t depth_preview_requested_until_us = 0;
    std::vector<uint8_t> depth_preview_ppm;
    double depth_scale = 1.0;
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
    std::mutex record_mutex;
    std::condition_variable record_cv;
    std::deque<RecordJob> record_queue;
    size_t record_queue_bytes = 0;
    size_t record_queue_peak_bytes = 0;
    size_t record_queue_peak_packets = 0;
    uint64_t record_enqueued_packets = 0;
    uint64_t record_dequeued_packets = 0;
    uint64_t record_backpressure_waits = 0;
    uint64_t record_oversize_packets = 0;
    uint64_t record_write_errors = 0;
    uint32_t record_active_writes = 0;
    bool record_worker_started = false;
    bool record_worker_stop = false;
    std::thread record_worker;

    std::string storage_key() const {
        return camera_name.empty() ? key : camera_name;
    }
};

class ReceiverApp {
public:
    explicit ReceiverApp(Config config)
        : config_(std::move(config)),
          logger_(config_.log_directory),
          runtime_state_(load_runtime_state(config_.state_path)),
          clock_sync_manager_(config_.clock_sync) {
        logger_.info("receiver state loaded: " + config_.state_path);
    }

    struct SegmentCloseTask {
        std::shared_ptr<CameraState> cam;
        std::string sender_id;
        std::string camera_id;
        std::string announce_json;
    };

    struct SenderControlTarget {
        std::string sender_id;
        std::string camera_id;
        std::string endpoint;
    };

    struct PreviewUdpAssembly {
        std::vector<uint8_t> bytes;
        std::vector<uint8_t> received;
        size_t received_count = 0;
        uint32_t total_size = 0;
        uint16_t chunk_count = 0;
        bool media_udp = false;
        uint64_t first_us = 0;
        uint64_t updated_us = 0;
    };

    void start() {
        running_ = true;
        clock_sync_manager_.set_log_callbacks([this](const std::string &message) { logger_.info(message); },
                                             [this](const std::string &message) { logger_.warn(message); });
        clock_sync_manager_.start();
        udp_thread_ = std::thread([this] { udp_loop(); });
        if(config_.media_udp_enabled) {
            media_udp_thread_ = std::thread([this] { media_udp_loop(); });
        }
        if(config_.preview_enabled && config_.preview_udp_enabled) {
            preview_udp_thread_ = std::thread([this] { preview_udp_loop(); });
        }
        tcp_thread_ = std::thread([this] { tcp_loop(); });
        admin_thread_ = std::thread([this] { admin_loop(); });
        logger_.info("receiver started: media tcp " + config_.media_bind_ip + ":" + std::to_string(config_.media_port) +
                     ", status udp " + config_.status_bind_ip + ":" + std::to_string(config_.status_port) +
                     ", clock sync udp "
                     + (config_.clock_sync.enabled ? config_.clock_sync.bind_ip : std::string("disabled")) + ":" +
                     std::to_string(config_.clock_sync.port) +
                     ", media udp " + (config_.media_udp_enabled ? config_.media_udp_bind_ip : std::string("disabled")) + ":" +
                     std::to_string(config_.media_udp_port) +
                     ", preview udp " +
                     (config_.preview_enabled && config_.preview_udp_enabled ? config_.preview_udp_bind_ip : std::string("disabled")) + ":" +
                     std::to_string(config_.preview_udp_port) + ", admin http " + config_.admin_bind_ip + ":" +
                     std::to_string(config_.admin_port) + ", preview " + (config_.preview_enabled ? "enabled" : "disabled"));
    }

    void stop() {
        running_ = false;
        clock_sync_manager_.stop();
        if(udp_thread_.joinable()) {
            udp_thread_.join();
        }
        if(media_udp_thread_.joinable()) {
            media_udp_thread_.join();
        }
        if(preview_udp_thread_.joinable()) {
            preview_udp_thread_.join();
        }
        if(tcp_thread_.joinable()) {
            tcp_thread_.join();
        }
        if(admin_thread_.joinable()) {
            admin_thread_.join();
        }
        std::vector<SegmentCloseTask> close_tasks;
        std::vector<std::shared_ptr<CameraState>> camera_snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for(auto &item : cameras_) {
                cleanup_rgb_decoder_async(std::move(item.second->rgb_decoder));
                cleanup_rgb_decoder_async(std::move(item.second->main_rgb_decoder));
                camera_snapshot.push_back(item.second);
                close_tasks.push_back({item.second, item.second->sender_id, item.second->camera_id,
                                       item.second->last_announce_live ? item.second->last_announce_json : ""});
            }
        }
        stop_record_workers_sync(camera_snapshot);
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
        UdpReassemblyStats media_udp_stats;
        UdpReassemblyStats preview_udp_stats;
        size_t active_media_udp_assemblies = 0;
        size_t active_preview_udp_assemblies = 0;
        {
            std::lock_guard<std::mutex> udp_lock(preview_udp_mutex_);
            media_udp_stats = media_udp_stats_;
            preview_udp_stats = preview_udp_stats_;
            for(const auto &assembly : preview_udp_assemblies_) {
                if(assembly.second.media_udp) {
                    ++active_media_udp_assemblies;
                }
                else {
                    ++active_preview_udp_assemblies;
                }
            }
        }
        std::ostringstream out;
        out << "{";
        out << "\"running\":true,";
        out << "\"recording_all\":" << (recording_all_ ? "true" : "false") << ',';
        out << "\"recording_start_us\":" << recording_all_start_us_ << ',';
        out << "\"default_file_prefix\":\"" << json_escape(runtime_state_.default_file_prefix) << "\",";
        out << "\"file_prefix_scope\":\"per_camera\",";
        out << "\"nas_root\":\"" << json_escape(config_.nas_root) << "\",";
        out << "\"media_port\":" << config_.media_port << ',';
        out << "\"preview_enabled\":" << (config_.preview_enabled ? "true" : "false") << ',';
        out << "\"media_udp_enabled\":" << (config_.media_udp_enabled ? "true" : "false") << ',';
        out << "\"media_udp_port\":" << config_.media_udp_port << ',';
        out << "\"preview_udp_enabled\":" << (config_.preview_enabled && config_.preview_udp_enabled ? "true" : "false") << ',';
        out << "\"preview_udp_port\":" << config_.preview_udp_port << ',';
        out << "\"active_media_clients\":" << active_media_clients_.load() << ',';
        out << "\"media_udp_stats\":";
        append_udp_reassembly_stats_json(out, media_udp_stats, active_media_udp_assemblies);
        out << ',';
        out << "\"preview_udp_stats\":";
        append_udp_reassembly_stats_json(out, preview_udp_stats, active_preview_udp_assemblies);
        out << ',';
        out << "\"status_port\":" << config_.status_port << ',';
        out << "\"admin_port\":" << config_.admin_port << ',';
        out << "\"clock_sync_enabled\":" << (config_.clock_sync.enabled ? "true" : "false") << ',';
        out << "\"clock_sync_port\":" << config_.clock_sync.port << ',';
        out << "\"clock_sync\":[";
        const auto clock_models = clock_sync_manager_.models();
        for(size_t i = 0; i < clock_models.size(); ++i) {
            if(i > 0) {
                out << ',';
            }
            const auto &model = clock_models[i].second;
            out << "{";
            out << "\"sender_id\":\"" << json_escape(clock_models[i].first) << "\",";
            out << "\"clock_sync_valid\":" << (model.valid ? "true" : "false") << ',';
            out << "\"clock_offset_us\":" << model.offset_us << ',';
            out << "\"clock_delay_us\":" << model.delay_us << ',';
            out << "\"clock_drift_ppm\":" << model.drift_ppm << ',';
            out << "\"clock_last_sync_us\":" << model.last_sync_us << ',';
            out << "\"clock_last_update_receiver_us\":" << model.last_update_receiver_us << ',';
            out << "\"clock_samples\":" << model.sample_count;
            out << "}";
        }
        out << "],";
        out << "\"main_preview_camera_key\":\"" << json_escape(main_preview_key_) << "\",";
        out << "\"cameras\":[";
        bool first = true;
        for(const auto &item : cameras_) {
            auto &cam = *item.second;
            const auto last_seen = camera_last_seen_us(cam);
            const bool status_live = cam.online && is_recent_us(now, cam.last_status_us, kCameraOnlineTimeoutUs);
            const bool media_live = cam.online && is_recent_us(now, cam.last_media_us, kCameraOnlineTimeoutUs);
            const bool live = media_live;
            const bool preview_enabled = config_.preview_enabled;
            bool rgb_h264_preview_fresh = false;
            uint32_t rgb_h264_preview_width = 0;
            uint32_t rgb_h264_preview_height = 0;
            uint64_t rgb_h264_preview_us = 0;
            {
                std::lock_guard<std::mutex> stream_lock(cam.rgb_preview_stream.mutex);
                rgb_h264_preview_fresh = preview_enabled && media_live && is_recent_us(now, cam.rgb_preview_stream.last_us, kPreviewFreshUs);
                rgb_h264_preview_width = cam.rgb_preview_stream.width;
                rgb_h264_preview_height = cam.rgb_preview_stream.height;
                rgb_h264_preview_us = cam.rgb_preview_stream.last_us;
            }
            const bool rgb_preview_fresh = preview_enabled && media_live && is_recent_us(now, cam.rgb_preview_us, kPreviewFreshUs);
            const bool main_rgb_preview_fresh = preview_enabled && media_live && is_recent_us(now, cam.main_rgb_preview_us, kPreviewFreshUs);
            const bool depth_preview_fresh = preview_enabled && media_live && is_recent_us(now, cam.depth_preview_us, kPreviewFreshUs);
            const bool rgb_thumbnail_preview_available = rgb_preview_fresh && cam.rgb_decoder && cam.rgb_decoder->has_frame();
            const bool rgb_preview_report_available =
                preview_enabled && (rgb_h264_preview_fresh || rgb_thumbnail_preview_available);
            const bool main_rgb_native_preview_available =
                cam.key == main_preview_key_ && main_rgb_preview_fresh && cam.main_rgb_decoder && cam.main_rgb_decoder->has_frame();
            const bool main_rgb_preview_report_available =
                cam.key == main_preview_key_ && (rgb_h264_preview_fresh || main_rgb_native_preview_available || rgb_thumbnail_preview_available);
            const int announce_rgb_width_for_preview = json_int_in_object(cam.last_announce_json, "rgb_profile", "width").value_or(0);
            const int announce_rgb_height_for_preview = json_int_in_object(cam.last_announce_json, "rgb_profile", "height").value_or(0);
            const uint32_t rgb_report_width =
                rgb_h264_preview_fresh ? rgb_h264_preview_width
                                       : (cam.rgb_preview_width > 0 ? cam.rgb_preview_width : static_cast<uint32_t>(announce_rgb_width_for_preview));
            const uint32_t rgb_report_height =
                rgb_h264_preview_fresh ? rgb_h264_preview_height
                                       : (cam.rgb_preview_height > 0 ? cam.rgb_preview_height : static_cast<uint32_t>(announce_rgb_height_for_preview));
            const uint64_t rgb_report_us = rgb_preview_report_available ? (rgb_h264_preview_fresh ? rgb_h264_preview_us : cam.rgb_preview_us) : 0;
            const uint32_t main_rgb_report_width =
                !main_rgb_preview_report_available
                    ? 0
                    : (rgb_h264_preview_fresh ? rgb_h264_preview_width
                                              : (main_rgb_native_preview_available ? cam.main_rgb_preview_width : rgb_report_width));
            const uint32_t main_rgb_report_height =
                !main_rgb_preview_report_available
                    ? 0
                    : (rgb_h264_preview_fresh ? rgb_h264_preview_height
                                              : (main_rgb_native_preview_available ? cam.main_rgb_preview_height : rgb_report_height));
            const uint64_t main_rgb_report_us =
                !main_rgb_preview_report_available
                    ? 0
                    : (rgb_h264_preview_fresh ? rgb_h264_preview_us
                                              : (main_rgb_native_preview_available ? cam.main_rgb_preview_us : cam.rgb_preview_us));
            const auto calibration_json = json_object_field(cam.last_announce_json, "calibration").value_or("");
            const bool cached_calibration_available = json_bool_field(calibration_json, "available").value_or(false);
            const bool calibration_available = cam.last_announce_live && cached_calibration_available;
            const int announce_rgb_width = json_int_in_object(cam.last_announce_json, "rgb_profile", "width").value_or(0);
            const int announce_rgb_height = json_int_in_object(cam.last_announce_json, "rgb_profile", "height").value_or(0);
            const int announce_depth_width = json_int_in_object(cam.last_announce_json, "depth_profile", "width").value_or(0);
            const int announce_depth_height = json_int_in_object(cam.last_announce_json, "depth_profile", "height").value_or(0);
            const auto announce_timestamp_us = json_uint64_field(cam.last_announce_json, "timestamp_us").value_or(0);
            size_t record_queue_packets = 0;
            size_t record_queue_bytes = 0;
            size_t record_queue_peak_packets = 0;
            size_t record_queue_peak_bytes = 0;
            uint64_t record_enqueued_packets = 0;
            uint64_t record_dequeued_packets = 0;
            uint64_t record_backpressure_waits = 0;
            uint64_t record_oversize_packets = 0;
            uint64_t record_write_errors = 0;
            uint32_t record_active_writes = 0;
            bool record_worker_started = false;
            {
                std::lock_guard<std::mutex> record_lock(cam.record_mutex);
                record_queue_packets = cam.record_queue.size();
                record_queue_bytes = cam.record_queue_bytes;
                record_queue_peak_packets = cam.record_queue_peak_packets;
                record_queue_peak_bytes = cam.record_queue_peak_bytes;
                record_enqueued_packets = cam.record_enqueued_packets;
                record_dequeued_packets = cam.record_dequeued_packets;
                record_backpressure_waits = cam.record_backpressure_waits;
                record_oversize_packets = cam.record_oversize_packets;
                record_write_errors = cam.record_write_errors;
                record_active_writes = cam.record_active_writes;
                record_worker_started = cam.record_worker_started;
            }
            if(!first) {
                out << ',';
            }
            first = false;
            const auto clock_model = clock_sync_manager_.get_model(cam.sender_id);
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
            out << "\"record_queue_packets\":" << record_queue_packets << ',';
            out << "\"record_queue_bytes\":" << record_queue_bytes << ',';
            out << "\"record_queue_peak_packets\":" << record_queue_peak_packets << ',';
            out << "\"record_queue_peak_bytes\":" << record_queue_peak_bytes << ',';
            out << "\"record_enqueued_packets\":" << record_enqueued_packets << ',';
            out << "\"record_dequeued_packets\":" << record_dequeued_packets << ',';
            out << "\"record_active_writes\":" << record_active_writes << ',';
            out << "\"record_backpressure_waits\":" << record_backpressure_waits << ',';
            out << "\"record_oversize_packets\":" << record_oversize_packets << ',';
            out << "\"record_write_errors\":" << record_write_errors << ',';
            out << "\"record_worker_started\":" << (record_worker_started ? "true" : "false") << ',';
            out << "\"last_status_us\":" << cam.last_status_us << ',';
            out << "\"last_media_us\":" << cam.last_media_us << ',';
            out << "\"last_seen_us\":" << last_seen << ',';
            out << "\"status_age_ms\":" << age_ms_or_negative(now, cam.last_status_us) << ',';
            out << "\"media_age_ms\":" << age_ms_or_negative(now, cam.last_media_us) << ',';
            out << "\"clock_sync_valid\":" << (clock_model.valid ? "true" : "false") << ',';
            out << "\"clock_offset_us\":" << clock_model.offset_us << ',';
            out << "\"clock_delay_us\":" << clock_model.delay_us << ',';
            out << "\"clock_drift_ppm\":" << clock_model.drift_ppm << ',';
            out << "\"clock_last_sync_us\":" << clock_model.last_sync_us << ',';
            out << "\"rgb_packets\":" << cam.rgb_packets << ',';
            out << "\"depth_packets\":" << cam.depth_packets << ',';
            out << "\"rgb_bytes\":" << cam.rgb_bytes << ',';
            out << "\"depth_bytes\":" << cam.depth_bytes << ',';
            out << "\"rgb_preview_available\":" << (rgb_preview_report_available ? "true" : "false") << ',';
            out << "\"rgb_h264_preview_available\":" << (rgb_h264_preview_fresh ? "true" : "false") << ',';
            out << "\"rgb_h264_preview_width\":" << rgb_h264_preview_width << ',';
            out << "\"rgb_h264_preview_height\":" << rgb_h264_preview_height << ',';
            out << "\"rgb_h264_preview_us\":" << rgb_h264_preview_us << ',';
            out << "\"rgb_h264_preview_age_ms\":" << age_ms_or_negative(now, rgb_h264_preview_us) << ',';
            out << "\"rgb_jpeg_preview_available\":" << (rgb_thumbnail_preview_available ? "true" : "false") << ',';
            out << "\"rgb_preview_width\":" << rgb_report_width << ',';
            out << "\"rgb_preview_height\":" << rgb_report_height << ',';
            out << "\"rgb_preview_us\":" << rgb_report_us << ',';
            out << "\"rgb_preview_age_ms\":" << age_ms_or_negative(now, rgb_report_us) << ',';
            out << "\"main_rgb_preview_available\":" << (main_rgb_preview_report_available ? "true" : "false") << ',';
            out << "\"main_rgb_jpeg_preview_available\":" << (main_rgb_native_preview_available ? "true" : "false") << ',';
            out << "\"main_rgb_preview_width\":" << main_rgb_report_width << ',';
            out << "\"main_rgb_preview_height\":" << main_rgb_report_height << ',';
            out << "\"main_rgb_preview_us\":" << main_rgb_report_us << ',';
            out << "\"main_rgb_preview_age_ms\":" << age_ms_or_negative(now, main_rgb_report_us) << ',';
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
        out << "\"preview_enabled\":" << (config_.preview_enabled ? "true" : "false") << ',';
        out << "\"media_udp_enabled\":" << (config_.media_udp_enabled ? "true" : "false") << ',';
        out << "\"media_udp_bind_ip\":\"" << json_escape(config_.media_udp_bind_ip) << "\",";
        out << "\"media_udp_port\":" << config_.media_udp_port << ',';
        out << "\"preview_udp_enabled\":" << (config_.preview_udp_enabled ? "true" : "false") << ',';
        out << "\"preview_udp_bind_ip\":\"" << json_escape(config_.preview_udp_bind_ip) << "\",";
        out << "\"preview_udp_port\":" << config_.preview_udp_port << ',';
        out << "\"clock_sync_enabled\":" << (config_.clock_sync.enabled ? "true" : "false") << ',';
        out << "\"clock_sync_bind_ip\":\"" << json_escape(config_.clock_sync.bind_ip) << "\",";
        out << "\"clock_sync_port\":" << config_.clock_sync.port << ',';
        out << "\"clock_sync_model_timeout_ms\":" << config_.clock_sync.model_timeout_ms << ',';
        out << "\"admin_bind_ip\":\"" << json_escape(config_.admin_bind_ip) << "\",";
        out << "\"admin_port\":" << config_.admin_port << ',';
        out << "\"nas_root\":\"" << json_escape(config_.nas_root) << "\",";
        out << "\"state_path\":\"" << json_escape(config_.state_path) << "\",";
        out << "\"default_file_prefix\":\"" << json_escape(runtime_state_.default_file_prefix) << "\",";
        out << "\"file_prefix_scope\":\"per_camera\",";
        out << "\"segment_seconds\":" << config_.segment_seconds << ',';
        out << "\"max_payload_mb\":" << (config_.max_payload_bytes / (1024ull * 1024ull)) << ',';
        out << "\"record_queue_max_mb\":" << (config_.record_queue_max_bytes / (1024ull * 1024ull));
        out << "}";
        return out.str();
    }

    std::string effective_file_prefix_locked(const CameraState &cam) const {
        if(recording_all_ && recording_all_has_file_prefix_override_) {
            return recording_all_file_prefix_;
        }
        return cam.camera_file_prefix;
    }

    void send_force_rgb_keyframe_controls(const std::vector<SenderControlTarget> &targets,
                                          const std::string &reason,
                                          uint64_t request_us) {
        for(const auto &target : targets) {
            if(target.endpoint.empty()) {
                continue;
            }
            std::ostringstream payload;
            payload << "{\"message_type\":\"control\","
                    << "\"control\":\"force_rgb_keyframe\","
                    << "\"sender_id\":\"" << json_escape(target.sender_id) << "\","
                    << "\"camera_id\":\"" << json_escape(target.camera_id) << "\","
                    << "\"reason\":\"" << json_escape(reason) << "\","
                    << "\"request_us\":" << request_us << "}";
            if(send_udp_text_to_endpoint(target.endpoint, payload.str())) {
                logger_.info("force_rgb_keyframe control sent sender=" + target.sender_id + " camera=" + target.camera_id
                             + " endpoint=" + target.endpoint + " reason=" + reason);
            }
            else {
                logger_.warn("force_rgb_keyframe control send failed sender=" + target.sender_id + " camera=" + target.camera_id
                             + " endpoint=" + target.endpoint);
            }
        }
    }

    std::optional<SenderControlTarget> maybe_web_rgb_preview_control_target_locked(CameraState &cam, uint64_t now) {
        const bool requested = is_recent_us(now, cam.rgb_stream_requested_until_us, 0)
                               || is_recent_us(now, cam.rgb_preview_requested_until_us, 0)
                               || (cam.key == main_preview_key_ && is_recent_us(now, cam.main_rgb_preview_requested_until_us, 0));
        if(!requested || cam.status_endpoint.empty()) {
            return std::nullopt;
        }
        if(cam.last_web_rgb_preview_control_us != 0 && now < cam.last_web_rgb_preview_control_us + kWebRgbPreviewControlIntervalUs) {
            return std::nullopt;
        }
        cam.last_web_rgb_preview_control_us = now;
        return SenderControlTarget{cam.sender_id, cam.camera_id, cam.status_endpoint};
    }

    std::optional<SenderControlTarget> maybe_web_rgb_preview_keyframe_target_locked(CameraState &cam, uint64_t now) {
        if(cam.status_endpoint.empty()) {
            return std::nullopt;
        }
        if(cam.last_web_rgb_preview_keyframe_us != 0 && now < cam.last_web_rgb_preview_keyframe_us + kWebRgbPreviewKeyframeIntervalUs) {
            return std::nullopt;
        }
        cam.last_web_rgb_preview_keyframe_us = now;
        return SenderControlTarget{cam.sender_id, cam.camera_id, cam.status_endpoint};
    }

    void send_web_rgb_preview_controls(const std::vector<SenderControlTarget> &targets, uint64_t request_us) {
        for(const auto &target : targets) {
            if(target.endpoint.empty()) {
                continue;
            }
            std::ostringstream payload;
            payload << "{\"message_type\":\"control\","
                    << "\"control\":\"set_web_rgb_preview_active\","
                    << "\"sender_id\":\"" << json_escape(target.sender_id) << "\","
                    << "\"camera_id\":\"" << json_escape(target.camera_id) << "\","
                    << "\"active\":true,"
                    << "\"lease_ms\":" << kWebRgbPreviewControlLeaseMs << ','
                    << "\"request_us\":" << request_us << "}";
            if(!send_udp_text_to_endpoint(target.endpoint, payload.str())) {
                logger_.warn("web rgb preview control send failed sender=" + target.sender_id + " camera=" + target.camera_id
                             + " endpoint=" + target.endpoint);
            }
        }
    }

    void write_record_job(const std::shared_ptr<CameraState> &cam, RecordJob job) {
        const MediaPacket *record_packet = &job.packet;
        std::optional<MediaPacket> decoded_depth_packet;
        if(job.packet.stream_type == StreamType::depth_raw && job.packet.codec_or_compression != "none") {
            try {
                decoded_depth_packet = normalized_depth_packet(job.packet);
                record_packet = &*decoded_depth_packet;
            }
            catch(const std::exception &e) {
                {
                    std::lock_guard<std::mutex> record_lock(cam->record_mutex);
                    cam->record_write_errors++;
                }
                logger_.warn(std::string("depth record packet ignored camera=") + cam->key + " frame=" + std::to_string(job.packet.frame_id)
                             + ": " + e.what());
                return;
            }
        }

        try {
            bool segment_active = false;
            std::string segment_dir;
            {
                std::lock_guard<std::mutex> segment_lock(cam->segment_mutex);
                cam->segment.write_packet(config_, *record_packet, job.sender_id, job.camera_id, job.camera_name, job.storage_key,
                                          job.file_prefix, job.announce_json, logger_);
                segment_active = cam->segment.active();
                segment_dir = cam->segment.directory();
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                cam->segment_active = segment_active;
                cam->segment_dir = std::move(segment_dir);
            }
        }
        catch(const std::exception &e) {
            {
                std::lock_guard<std::mutex> record_lock(cam->record_mutex);
                cam->record_write_errors++;
            }
            logger_.warn(std::string("record packet write failed camera=") + cam->key + " frame=" + std::to_string(job.packet.frame_id)
                         + ": " + e.what());
        }
    }

    void record_worker_loop(std::shared_ptr<CameraState> cam) {
        logger_.info("record queue worker started: " + cam->key);
        for(;;) {
            RecordJob job;
            {
                std::unique_lock<std::mutex> record_lock(cam->record_mutex);
                cam->record_cv.wait(record_lock, [&] { return cam->record_worker_stop || !cam->record_queue.empty(); });
                if(cam->record_queue.empty()) {
                    if(cam->record_worker_stop) {
                        break;
                    }
                    continue;
                }
                job = std::move(cam->record_queue.front());
                cam->record_queue.pop_front();
                if(job.queue_bytes <= cam->record_queue_bytes) {
                    cam->record_queue_bytes -= job.queue_bytes;
                }
                else {
                    cam->record_queue_bytes = 0;
                }
                cam->record_dequeued_packets++;
                cam->record_active_writes++;
            }
            cam->record_cv.notify_all();

            write_record_job(cam, std::move(job));

            {
                std::lock_guard<std::mutex> record_lock(cam->record_mutex);
                if(cam->record_active_writes > 0) {
                    cam->record_active_writes--;
                }
            }
            cam->record_cv.notify_all();
        }
        logger_.info("record queue worker stopped: " + cam->key);
    }

    void start_record_worker_if_needed(const std::shared_ptr<CameraState> &cam) {
        bool should_start = false;
        {
            std::lock_guard<std::mutex> record_lock(cam->record_mutex);
            if(!cam->record_worker_started) {
                cam->record_worker_started = true;
                cam->record_worker_stop = false;
                should_start = true;
            }
        }
        if(should_start) {
            cam->record_worker = std::thread([this, cam] { record_worker_loop(cam); });
        }
    }

    bool enqueue_record_job(const std::shared_ptr<CameraState> &cam, RecordJob job) {
        if(!cam || job.packet.stream_type == StreamType::rgb_preview) {
            return false;
        }
        start_record_worker_if_needed(cam);
        job.queue_bytes = record_packet_queue_bytes(job.packet);
        job.enqueue_us = now_us();
        const size_t max_bytes = std::max<size_t>(1, config_.record_queue_max_bytes);
        std::unique_lock<std::mutex> record_lock(cam->record_mutex);
        if(job.queue_bytes > max_bytes) {
            cam->record_oversize_packets++;
        }
        while(!cam->record_worker_stop && !cam->record_queue.empty() && cam->record_queue_bytes + job.queue_bytes > max_bytes) {
            cam->record_backpressure_waits++;
            cam->record_cv.wait_for(record_lock, std::chrono::milliseconds(100));
        }
        if(cam->record_worker_stop) {
            return false;
        }
        cam->record_queue_bytes += job.queue_bytes;
        cam->record_queue.push_back(std::move(job));
        cam->record_enqueued_packets++;
        cam->record_queue_peak_bytes = std::max(cam->record_queue_peak_bytes, cam->record_queue_bytes);
        cam->record_queue_peak_packets = std::max(cam->record_queue_peak_packets, cam->record_queue.size());
        record_lock.unlock();
        cam->record_cv.notify_one();
        return true;
    }

    void wait_record_queue_idle(const std::shared_ptr<CameraState> &cam, const std::string &reason) {
        if(!cam) {
            return;
        }
        uint64_t next_log_us = now_us() + kRecordQueueWarnIntervalUs;
        for(;;) {
            std::unique_lock<std::mutex> record_lock(cam->record_mutex);
            if(cam->record_queue.empty() && cam->record_active_writes == 0) {
                return;
            }
            const bool timed_out = cam->record_cv.wait_for(record_lock, std::chrono::seconds(1)) == std::cv_status::timeout;
            const uint64_t current_us = now_us();
            if(timed_out && current_us >= next_log_us) {
                const size_t packets = cam->record_queue.size();
                const size_t bytes = cam->record_queue_bytes;
                const uint32_t active = cam->record_active_writes;
                record_lock.unlock();
                logger_.info("waiting record queue drain camera=" + cam->key + " reason=" + reason
                             + " packets=" + std::to_string(packets)
                             + " bytes=" + std::to_string(bytes)
                             + " active_writes=" + std::to_string(active));
                next_log_us = current_us + kRecordQueueWarnIntervalUs;
            }
        }
    }

    void stop_record_workers_sync(const std::vector<std::shared_ptr<CameraState>> &cameras) {
        for(const auto &cam : cameras) {
            if(!cam) {
                continue;
            }
            {
                std::lock_guard<std::mutex> record_lock(cam->record_mutex);
                if(cam->record_worker_started) {
                    cam->record_worker_stop = true;
                }
            }
            cam->record_cv.notify_all();
        }
        for(const auto &cam : cameras) {
            if(cam && cam->record_worker.joinable()) {
                cam->record_worker.join();
            }
        }
    }

    void close_segments_async(std::vector<SegmentCloseTask> close_tasks, const std::string &done_log_message) {
        if(close_tasks.empty()) {
            return;
        }
        std::thread([this, close_tasks = std::move(close_tasks), done_log_message] {
            for(auto &task : close_tasks) {
                wait_record_queue_idle(task.cam, done_log_message);
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

    void close_segments_sync(std::vector<SegmentCloseTask> close_tasks, const std::string &done_log_message) {
        if(close_tasks.empty()) {
            return;
        }
        for(auto &task : close_tasks) {
            wait_record_queue_idle(task.cam, done_log_message);
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
    }

    std::string start_all(const std::optional<std::string> &file_prefix_override) {
        if(file_prefix_override) {
            if(const auto error = storage_text_error("file_prefix", *file_prefix_override)) {
                return json_error(*error);
            }
        }
        uint64_t request_us = 0;
        uint64_t response_start_us = 0;
        bool response_has_override = false;
        bool control_needed = false;
        std::vector<SenderControlTarget> control_targets;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            refresh_camera_liveness_locked(now_us());
            const bool already_recording = recording_all_;
            if(!already_recording) {
                recording_all_start_us_ = now_us();
                recording_all_has_file_prefix_override_ = file_prefix_override.has_value();
                recording_all_file_prefix_ = file_prefix_override.value_or("");
                request_us = recording_all_start_us_;
                control_needed = true;
            }
            recording_all_ = true;
            for(auto &item : cameras_) {
                if(!item.second->recording_requested && !item.second->segment_active) {
                    item.second->recording_start_us = recording_all_start_us_;
                    item.second->recording_file_prefix = effective_file_prefix_locked(*item.second);
                }
                item.second->recording_requested = true;
                if(control_needed && item.second->online && !item.second->status_endpoint.empty()) {
                    control_targets.push_back({item.second->sender_id, item.second->camera_id, item.second->status_endpoint});
                }
            }
            response_start_us = recording_all_start_us_;
            response_has_override = recording_all_has_file_prefix_override_;
            logger_.info("recording start-all requested");
        }
        if(control_needed) {
            send_force_rgb_keyframe_controls(control_targets, "record_start_all", request_us);
        }
        std::ostringstream out;
        out << "{\"ok\":true,\"recording_all\":true,\"recording_start_us\":" << response_start_us
            << ",\"file_prefix_scope\":\"" << (response_has_override ? "override_all" : "per_camera") << "\"}";
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
        close_segments_sync(std::move(close_tasks), "recording stop-all finalized");
        std::ostringstream out;
        out << "{\"ok\":true,\"recording_all\":false,\"recording_start_us\":" << recording_start_us
            << ",\"finalizing\":false,\"finalized\":" << (finalizing ? "true" : "false") << "}";
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
        uint64_t response_start_us = 0;
        std::string response_file_prefix;
        std::vector<SenderControlTarget> control_targets;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto &cam = ensure_camera_locked(sender_id, camera_id);
            const bool was_recording = recording_all_ || cam.recording_requested || cam.segment_active;
            if(!recording_all_ && !cam.recording_requested && !cam.segment_active) {
                cam.recording_start_us = now_us();
                cam.recording_file_prefix = file_prefix_override.value_or(cam.camera_file_prefix);
            }
            else if(cam.recording_start_us == 0) {
                cam.recording_start_us = recording_all_ ? recording_all_start_us_ : now_us();
                cam.recording_file_prefix = recording_all_ ? effective_file_prefix_locked(cam) : file_prefix_override.value_or(cam.camera_file_prefix);
            }
            cam.recording_requested = true;
            response_start_us = cam.recording_start_us;
            response_file_prefix = cam.recording_file_prefix;
            if(!was_recording && cam.online && !cam.status_endpoint.empty()) {
                control_targets.push_back({cam.sender_id, cam.camera_id, cam.status_endpoint});
            }
            logger_.info("recording start requested: " + cam.key);
        }
        if(!control_targets.empty()) {
            send_force_rgb_keyframe_controls(control_targets, "record_start", response_start_us);
        }
        std::ostringstream out;
        out << "{\"ok\":true,\"recording_start_us\":" << response_start_us << ",\"file_prefix\":\""
            << json_escape(response_file_prefix) << "\"}";
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
        close_segments_sync(std::move(close_tasks), "recording stop finalized: " + key);
        std::ostringstream out;
        out << "{\"ok\":true,\"recording_start_us\":" << recording_start_us << ",\"finalizing\":false,\"finalized\":true}";
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
        if(!config_.preview_enabled) {
            return std::nullopt;
        }
        const auto now = now_us();
        refresh_camera_liveness_locked(now);
        const auto key = camera_key(sender_id, camera_id);
        auto it = cameras_.find(key);
        if(it == cameras_.end() || !it->second->online) {
            return std::nullopt;
        }
        it->second->depth_preview_requested_until_us = now + kPreviewRequestKeepaliveUs;
        if(!is_recent_us(now, it->second->depth_preview_us, kPreviewFreshUs) || it->second->depth_preview_ppm.empty()) {
            return std::nullopt;
        }
        return it->second->depth_preview_ppm;
    }

    std::optional<std::vector<uint8_t>> rgb_preview(const std::string &sender_id, const std::string &camera_id) {
        std::optional<SenderControlTarget> keyframe_target;
        std::optional<std::vector<uint8_t>> jpeg;
        const auto request_us = now_us();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(!config_.preview_enabled) {
                return std::nullopt;
            }
            refresh_camera_liveness_locked(request_us);
            const auto key = camera_key(sender_id, camera_id);
            auto it = cameras_.find(key);
            if(it == cameras_.end() || !it->second->online) {
                return std::nullopt;
            }
            if(!kEnableRgbThumbnailPreview) {
                return std::nullopt;
            }
            auto &cam = *it->second;
            cam.rgb_preview_requested_until_us = request_us + kPreviewRequestKeepaliveUs;
            if(!is_recent_us(request_us, cam.rgb_preview_us, kPreviewFreshUs) || !cam.rgb_decoder) {
                keyframe_target = maybe_web_rgb_preview_keyframe_target_locked(cam, request_us);
            }
            if(is_recent_us(request_us, cam.rgb_preview_us, kPreviewFreshUs) && cam.rgb_decoder) {
                jpeg = cam.rgb_decoder->latest_jpeg();
            }
        }
        if(keyframe_target) {
            send_force_rgb_keyframe_controls({*keyframe_target}, "web_rgb_jpeg_preview", request_us);
        }
        return jpeg;
    }

    std::string set_main_preview_target(const std::string &sender_id, const std::string &camera_id) {
        if(!config_.preview_enabled) {
            return "{\"ok\":false,\"error\":\"preview disabled\"}";
        }
        if(sender_id.empty() || camera_id.empty()) {
            return "{\"ok\":false,\"error\":\"sender_id and camera_id are required\"}";
        }
        std::optional<SenderControlTarget> keyframe_target;
        const auto request_us = now_us();
        std::string selected_key;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto key = camera_key(sender_id, camera_id);
            auto it = cameras_.find(key);
            if(it == cameras_.end()) {
                return "{\"ok\":false,\"error\":\"camera not found\"}";
            }
            const bool target_changed = main_preview_key_ != key;
            if(target_changed) {
                for(auto &item : cameras_) {
                    if(item.first != key) {
                        cleanup_rgb_decoder_async(std::move(item.second->main_rgb_decoder));
                        item.second->main_rgb_preview_requested_until_us = 0;
                        item.second->main_rgb_preview_us = 0;
                        item.second->main_rgb_preview_width = 0;
                        item.second->main_rgb_preview_height = 0;
                    }
                }
            }
            main_preview_key_ = key;
            it->second->main_rgb_preview_requested_until_us = request_us + kMainPreviewRequestKeepaliveUs;
            if(target_changed || !is_recent_us(request_us, it->second->main_rgb_preview_us, kPreviewFreshUs) || !it->second->main_rgb_decoder) {
                keyframe_target = maybe_web_rgb_preview_keyframe_target_locked(*it->second, request_us);
            }
            selected_key = main_preview_key_;
        }
        if(keyframe_target) {
            send_force_rgb_keyframe_controls({*keyframe_target}, "web_rgb_main_target", request_us);
        }
        std::ostringstream out;
        out << "{\"ok\":true,\"main_preview_camera_key\":\"" << json_escape(selected_key) << "\"}";
        return out.str();
    }

    std::optional<std::vector<uint8_t>> main_rgb_preview(const std::string &sender_id, const std::string &camera_id) {
        std::optional<SenderControlTarget> keyframe_target;
        std::optional<std::vector<uint8_t>> jpeg;
        const auto request_us = now_us();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(!config_.preview_enabled) {
                return std::nullopt;
            }
            refresh_camera_liveness_locked(request_us);
            const auto key = camera_key(sender_id, camera_id);
            auto it = cameras_.find(key);
            if(it == cameras_.end() || !it->second->online || !is_recent_us(request_us, it->second->last_media_us, kCameraOnlineTimeoutUs)) {
                return std::nullopt;
            }
            if(main_preview_key_.empty()) {
                main_preview_key_ = key;
            }
            auto &cam = *it->second;
            if(key == main_preview_key_) {
                cam.main_rgb_preview_requested_until_us = request_us + kMainPreviewRequestKeepaliveUs;
                if(!is_recent_us(request_us, cam.main_rgb_preview_us, kPreviewFreshUs) || !cam.main_rgb_decoder) {
                    keyframe_target = maybe_web_rgb_preview_keyframe_target_locked(cam, request_us);
                }
            }
            if(!kEnableJpegMainPreview) {
                return std::nullopt;
            }
            if(key == main_preview_key_ && is_recent_us(request_us, cam.main_rgb_preview_us, kPreviewFreshUs) && cam.main_rgb_decoder) {
                jpeg = cam.main_rgb_decoder->latest_jpeg();
            }
            if(!jpeg && is_recent_us(request_us, cam.rgb_preview_us, kPreviewFreshUs) && cam.rgb_decoder) {
                jpeg = cam.rgb_decoder->latest_jpeg();
            }
        }
        if(keyframe_target) {
            send_force_rgb_keyframe_controls({*keyframe_target}, "web_rgb_main_preview", request_us);
        }
        return jpeg;
    }

    bool stream_rgb_h264_preview(int fd, const std::string &sender_id, const std::string &camera_id) {
        std::shared_ptr<CameraState> cam;
        std::optional<SenderControlTarget> keyframe_target;
        const auto request_us = now_us();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(!config_.preview_enabled) {
                const std::string body = "{\"ok\":false,\"error\":\"preview disabled\"}";
                std::ostringstream response;
                response << "HTTP/1.1 404 Not Found\r\n";
                response << "Content-Type: application/json\r\n";
                response << "Cache-Control: no-store\r\n";
                response << "Content-Length: " << body.size() << "\r\n";
                response << "Connection: close\r\n\r\n";
                response << body;
                return send_all(fd, response.str());
            }
            refresh_camera_liveness_locked(request_us);
            const auto key = camera_key(sender_id, camera_id);
            auto it = cameras_.find(key);
            if(it == cameras_.end() || !it->second->online || !is_recent_us(request_us, it->second->last_media_us, kCameraOnlineTimeoutUs)) {
                const std::string body = "{\"ok\":false,\"error\":\"rgb h264 stream not found\"}";
                std::ostringstream response;
                response << "HTTP/1.1 404 Not Found\r\n";
                response << "Content-Type: application/json\r\n";
                response << "Cache-Control: no-store\r\n";
                response << "Content-Length: " << body.size() << "\r\n";
                response << "Connection: close\r\n\r\n";
                response << body;
                return send_all(fd, response.str());
            }
            it->second->rgb_stream_requested_until_us = request_us + kPreviewRequestKeepaliveUs;
            keyframe_target = maybe_web_rgb_preview_keyframe_target_locked(*it->second, request_us);
            cam = it->second;
        }
        if(keyframe_target) {
            send_force_rgb_keyframe_controls({*keyframe_target}, "web_rgb_h264_preview", request_us);
        }

        H264StreamBuffer *stream = &cam->rgb_stream;
        bool using_preview_stream = false;
        for(int attempt = 0; attempt < 10; ++attempt) {
            {
                std::lock_guard<std::mutex> stream_lock(cam->rgb_preview_stream.mutex);
                const auto now = now_us();
                using_preview_stream = is_recent_us(now, cam->rgb_preview_stream.last_us, kPreviewFreshUs);
                if(using_preview_stream) {
                    stream = &cam->rgb_preview_stream;
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        std::ostringstream response;
        response << "HTTP/1.1 200 OK\r\n";
        response << "Content-Type: video/h264\r\n";
        response << "Cache-Control: no-store\r\n";
        response << "Connection: close\r\n";
        response << "X-GWV3-Rgb-Stream: " << (using_preview_stream ? "preview" : "main") << "\r\n";
        response << "X-Accel-Buffering: no\r\n\r\n";
        if(!send_all(fd, response.str())) {
            return false;
        }

        bool started = false;
        uint64_t next_seq = 0;
        while(running_ && g_running) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                cam->rgb_stream_requested_until_us = now_us() + kPreviewRequestKeepaliveUs;
            }
            std::vector<uint8_t> header;
            std::vector<H264StreamPacket> packets;
            {
                std::unique_lock<std::mutex> stream_lock(stream->mutex);
                stream->cv.wait_for(stream_lock, std::chrono::milliseconds(1000));
                if(stream->packets.empty()) {
                    continue;
                }

                if(!started) {
                    size_t start_index = stream->packets.size();
                    for(size_t i = stream->packets.size(); i > 0; --i) {
                        if(stream->packets[i - 1].has_idr) {
                            start_index = i - 1;
                            break;
                        }
                    }
                    if(start_index == stream->packets.size()) {
                        continue;
                    }
                    header = stream->header_h264;
                    for(size_t i = start_index; i < stream->packets.size(); ++i) {
                        packets.push_back(stream->packets[i]);
                    }
                    next_seq = packets.empty() ? stream->next_seq : packets.back().seq + 1;
                    started = true;
                }
                else {
                    if(next_seq < stream->packets.front().seq) {
                        next_seq = stream->packets.front().seq;
                    }
                    for(const auto &packet : stream->packets) {
                        if(packet.seq >= next_seq) {
                            packets.push_back(packet);
                        }
                    }
                    if(!packets.empty()) {
                        next_seq = packets.back().seq + 1;
                    }
                }
            }

            if(!header.empty() && !send_all(fd, header.data(), header.size())) {
                return false;
            }
            for(const auto &packet : packets) {
                if(!send_all(fd, packet.payload.data(), packet.payload.size())) {
                    return false;
                }
            }
        }
        return true;
    }

    bool stream_rgb_h264_preview_frames(int fd, const std::string &sender_id, const std::string &camera_id) {
        std::shared_ptr<CameraState> cam;
        std::optional<SenderControlTarget> keyframe_target;
        const auto request_us = now_us();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(!config_.preview_enabled) {
                const std::string body = "{\"ok\":false,\"error\":\"preview disabled\"}";
                std::ostringstream response;
                response << "HTTP/1.1 404 Not Found\r\n";
                response << "Content-Type: application/json\r\n";
                response << "Cache-Control: no-store\r\n";
                response << "Content-Length: " << body.size() << "\r\n";
                response << "Connection: close\r\n\r\n";
                response << body;
                return send_all(fd, response.str());
            }
            refresh_camera_liveness_locked(request_us);
            const auto key = camera_key(sender_id, camera_id);
            auto it = cameras_.find(key);
            if(it == cameras_.end() || !it->second->online || !is_recent_us(request_us, it->second->last_media_us, kCameraOnlineTimeoutUs)) {
                const std::string body = "{\"ok\":false,\"error\":\"rgb h264 stream not found\"}";
                std::ostringstream response;
                response << "HTTP/1.1 404 Not Found\r\n";
                response << "Content-Type: application/json\r\n";
                response << "Cache-Control: no-store\r\n";
                response << "Content-Length: " << body.size() << "\r\n";
                response << "Connection: close\r\n\r\n";
                response << body;
                return send_all(fd, response.str());
            }
            it->second->rgb_stream_requested_until_us = request_us + kPreviewRequestKeepaliveUs;
            keyframe_target = maybe_web_rgb_preview_keyframe_target_locked(*it->second, request_us);
            cam = it->second;
        }
        if(keyframe_target) {
            send_force_rgb_keyframe_controls({*keyframe_target}, "web_rgb_h264_frames", request_us);
        }

        H264StreamBuffer *stream = &cam->rgb_stream;
        bool using_preview_stream = false;
        const auto preview_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        while(running_ && g_running && std::chrono::steady_clock::now() < preview_deadline) {
            {
                std::unique_lock<std::mutex> stream_lock(cam->rgb_preview_stream.mutex);
                const auto now = now_us();
                if(is_recent_us(now, cam->rgb_preview_stream.last_us, kPreviewFreshUs) && !cam->rgb_preview_stream.packets.empty()) {
                    stream = &cam->rgb_preview_stream;
                    using_preview_stream = true;
                    break;
                }
                stream_lock.unlock();
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                cam->rgb_stream_requested_until_us = now_us() + kPreviewRequestKeepaliveUs;
            }
            std::unique_lock<std::mutex> wait_lock(cam->rgb_preview_stream.mutex);
            cam->rgb_preview_stream.cv.wait_for(wait_lock, std::chrono::milliseconds(50));
        }

        std::ostringstream response;
        response << "HTTP/1.1 200 OK\r\n";
        response << "Content-Type: application/octet-stream\r\n";
        response << "Cache-Control: no-store\r\n";
        response << "Connection: close\r\n";
        response << "X-GWV3-Rgb-Stream: " << (using_preview_stream ? "preview" : "main") << "\r\n";
        response << "X-Accel-Buffering: no\r\n\r\n";
        if(!send_all(fd, response.str())) {
            return false;
        }

        bool started = false;
        uint64_t next_seq = 0;
        while(running_ && g_running) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                cam->rgb_stream_requested_until_us = now_us() + kPreviewRequestKeepaliveUs;
            }
            std::vector<uint8_t> header;
            std::vector<H264StreamPacket> packets;
            {
                std::unique_lock<std::mutex> stream_lock(stream->mutex);
                stream->cv.wait_for(stream_lock, std::chrono::milliseconds(1000));
                if(stream->packets.empty()) {
                    continue;
                }

                header = stream->header_h264;
                if(!started) {
                    size_t start_index = stream->packets.size();
                    for(size_t i = stream->packets.size(); i > 0; --i) {
                        if(stream->packets[i - 1].has_idr) {
                            start_index = i - 1;
                            break;
                        }
                    }
                    if(start_index == stream->packets.size()) {
                        continue;
                    }
                    for(size_t i = start_index; i < stream->packets.size(); ++i) {
                        packets.push_back(stream->packets[i]);
                    }
                    next_seq = packets.empty() ? stream->next_seq : packets.back().seq + 1;
                    started = true;
                }
                else {
                    if(next_seq < stream->packets.front().seq) {
                        next_seq = stream->packets.front().seq;
                    }
                    for(const auto &packet : stream->packets) {
                        if(packet.seq >= next_seq) {
                            packets.push_back(packet);
                        }
                    }
                    if(!packets.empty()) {
                        next_seq = packets.back().seq + 1;
                    }
                }
            }

            for(const auto &packet : packets) {
                uint32_t flags = packet.has_idr ? kH264PreviewFrameFlagKey : 0u;
                const auto timestamp_us = packet.timestamp_us > 0 ? packet.timestamp_us : now_us();
                if(packet.has_idr && !header.empty()) {
                    std::vector<uint8_t> payload;
                    payload.reserve(header.size() + packet.payload.size());
                    payload.insert(payload.end(), header.begin(), header.end());
                    payload.insert(payload.end(), packet.payload.begin(), packet.payload.end());
                    flags |= kH264PreviewFrameFlagConfig;
                    if(!send_h264_preview_frame(fd, payload, flags, packet.width, packet.height, timestamp_us, packet.seq)) {
                        return false;
                    }
                }
                else if(!send_h264_preview_frame(fd, packet.payload, flags, packet.width, packet.height, timestamp_us, packet.seq)) {
                    return false;
                }
            }
        }
        return true;
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
        {
            std::lock_guard<std::mutex> stream_lock(cam.rgb_stream.mutex);
            cam.rgb_stream.packets.clear();
            cam.rgb_stream.header_h264.clear();
            cam.rgb_stream.last_us = 0;
            cam.rgb_stream.width = 0;
            cam.rgb_stream.height = 0;
        }
        cam.rgb_stream.cv.notify_all();
        {
            std::lock_guard<std::mutex> stream_lock(cam.rgb_preview_stream.mutex);
            cam.rgb_preview_stream.packets.clear();
            cam.rgb_preview_stream.header_h264.clear();
            cam.rgb_preview_stream.last_us = 0;
            cam.rgb_preview_stream.width = 0;
            cam.rgb_preview_stream.height = 0;
        }
        cam.rgb_preview_stream.cv.notify_all();
        cam.rgb_preview_requested_until_us = 0;
        cam.rgb_stream_requested_until_us = 0;
        cam.rgb_preview_us = 0;
        cam.rgb_preview_width = 0;
        cam.rgb_preview_height = 0;
        cam.main_rgb_preview_requested_until_us = 0;
        cam.main_rgb_preview_us = 0;
        cam.main_rgb_preview_width = 0;
        cam.main_rgb_preview_height = 0;
        cam.depth_preview_ppm.clear();
        cam.depth_preview_requested_until_us = 0;
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

            if(!cam.online && !cam.recording_requested && !cam.segment_active && !cam.record_worker_started &&
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

    void update_h264_stream_buffer_locked(H264StreamBuffer &stream, const MediaPacket &packet, bool has_idr, bool has_vcl) {
        if(packet.payload.empty()) {
            return;
        }

        {
            std::lock_guard<std::mutex> stream_lock(stream.mutex);
            const auto non_vcl_prefix = h264_non_vcl_prefix(packet.payload);
            if(!non_vcl_prefix.empty() && h264_payload_has_sps_and_pps(non_vcl_prefix)) {
                stream.header_h264 = non_vcl_prefix;
            }
            else if(!has_vcl) {
                if(stream.header_h264.size() + packet.payload.size() > kRgbH264StreamMaxHeaderBytes) {
                    stream.header_h264.clear();
                }
                stream.header_h264.insert(stream.header_h264.end(), packet.payload.begin(), packet.payload.end());
            }
            stream.packets.push_back(
                H264StreamPacket{stream.next_seq++, has_idr, has_vcl, packet.system_timestamp_us, packet.width, packet.height, packet.payload});
            while(stream.packets.size() > kRgbH264StreamMaxPackets) {
                stream.packets.pop_front();
            }
            stream.last_us = now_us();
            stream.width = packet.width;
            stream.height = packet.height;
        }
        stream.cv.notify_all();
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
            state->depth_scale = depth_scale_from_announce_or_camera(state->last_announce_json, sender_id, camera_id);
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

        if(!sender_id.empty() && (type == "heartbeat" || type == "clock_sync_report")) {
            const bool clock_valid = json_bool_field(json, "clock_sync_valid").value_or(false);
            const auto offset_us = json_int64_field(json, "clock_offset_us").value_or(0);
            const auto delay_us = json_int64_field(json, "clock_delay_us").value_or(0);
            const auto drift_ppm = json_double_field(json, "clock_drift_ppm").value_or(0.0);
            const auto last_sync_us = clock_valid ? json_uint64_field(json, "clock_last_sync_us").value_or(0) : 0;
            clock_sync_manager_.update_from_sender_report(sender_id, offset_us, delay_us, drift_ppm, last_sync_us);
        }

        if(!sender_id.empty() && !camera_id.empty()) {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto key = camera_key(sender_id, camera_id);
            const auto code = type == "event" ? json_string_field(json, "event_code").value_or("event") : "";
            const bool marks_online = type == "camera_announce" || code == "camera_connected" || code == "camera_reconnected";
            auto &cam = *ensure_camera_ptr_locked(sender_id, camera_id, marks_online);
            const auto received_us = now_us();
            cam.last_status_us = received_us;
            cam.status_endpoint = peer_endpoint;
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
                cam.depth_scale = depth_scale_from_announce_or_camera(cam.last_announce_json, sender_id, camera_id);
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
            auto decoder_prefix = cam.rgb_preview_prefix_h264;
            const auto packet_prefix = h264_non_vcl_prefix(packet.payload);
            if(!packet_prefix.empty() && h264_payload_has_sps_and_pps(packet_prefix)) {
                decoder_prefix = packet_prefix;
            }
            if(!decoder_prefix.empty()) {
                if(!decoder->write_packet(decoder_prefix)) {
                    if(!decoder->has_frame()) {
                        cleanup_rgb_decoder_async(std::move(decoder));
                        preview_width = 0;
                        preview_height = 0;
                        preview_us = 0;
                    }
                    return false;
                }
            }
        }
        else if(!decoder || !decoder->active()) {
            return false;
        }

        if(!decoder->write_packet(packet.payload)) {
            if(!decoder->has_frame()) {
                cleanup_rgb_decoder_async(std::move(decoder));
                preview_width = 0;
                preview_height = 0;
                preview_us = 0;
            }
            return false;
        }
        preview_width = decoder->preview_width();
        preview_height = decoder->preview_height();
        preview_us = decoder->frame_us();
        return true;
    }

    void update_rgb_preview_locked(CameraState &cam, const MediaPacket &packet, bool has_idr, bool has_vcl) {
        if(!config_.preview_enabled) {
            cam.rgb_preview_prefix_h264.clear();
            cleanup_rgb_decoder_async(std::move(cam.rgb_decoder));
            cleanup_rgb_decoder_async(std::move(cam.main_rgb_decoder));
            cam.rgb_preview_requested_until_us = 0;
            cam.rgb_stream_requested_until_us = 0;
            cam.rgb_preview_us = 0;
            cam.rgb_preview_width = 0;
            cam.rgb_preview_height = 0;
            cam.main_rgb_preview_requested_until_us = 0;
            cam.main_rgb_preview_us = 0;
            cam.main_rgb_preview_width = 0;
            cam.main_rgb_preview_height = 0;
            return;
        }
        if(packet.payload.empty()) {
            return;
        }
        if(cam.rgb_preview_decoder_source != packet.stream_type) {
            cam.rgb_preview_decoder_source = packet.stream_type;
            cam.rgb_preview_prefix_h264.clear();
            cleanup_rgb_decoder_async(std::move(cam.rgb_decoder));
            cleanup_rgb_decoder_async(std::move(cam.main_rgb_decoder));
            cam.rgb_preview_width = 0;
            cam.rgb_preview_height = 0;
            cam.rgb_preview_us = 0;
            cam.main_rgb_preview_width = 0;
            cam.main_rgb_preview_height = 0;
            cam.main_rgb_preview_us = 0;
        }

        if(!has_vcl) {
            if(cam.rgb_preview_prefix_h264.size() + packet.payload.size() > kMaxRgbPreviewPrefixBytes) {
                cam.rgb_preview_prefix_h264.clear();
            }
            cam.rgb_preview_prefix_h264.insert(cam.rgb_preview_prefix_h264.end(), packet.payload.begin(), packet.payload.end());
            return;
        }

        const auto now = now_us();
        if(is_recent_us(now, cam.rgb_preview_requested_until_us, 0)) {
            feed_rgb_preview_decoder_locked(cam, packet, has_idr, cam.rgb_decoder, kRgbPreviewWidth, kRgbPreviewFps, cam.key, cam.rgb_preview_width,
                                            cam.rgb_preview_height, cam.rgb_preview_us);
        }
        else if(cam.rgb_decoder && is_older_than_us(now, cam.rgb_preview_requested_until_us, kPreviewDecoderIdleStopUs)) {
            cleanup_rgb_decoder_async(std::move(cam.rgb_decoder));
            cam.rgb_preview_width = 0;
            cam.rgb_preview_height = 0;
            cam.rgb_preview_us = 0;
        }

        if(kEnableJpegMainPreview && cam.key == main_preview_key_ && is_recent_us(now, cam.main_rgb_preview_requested_until_us, 0)) {
            feed_rgb_preview_decoder_locked(cam, packet, has_idr, cam.main_rgb_decoder, kRgbMainPreviewWidth, kRgbMainPreviewFps, cam.key + ":main",
                                            cam.main_rgb_preview_width, cam.main_rgb_preview_height, cam.main_rgb_preview_us);
        }
        else if(cam.main_rgb_decoder &&
                (cam.key != main_preview_key_ || is_older_than_us(now, cam.main_rgb_preview_requested_until_us, kMainPreviewDecoderIdleStopUs))) {
            cleanup_rgb_decoder_async(std::move(cam.main_rgb_decoder));
            cam.main_rgb_preview_width = 0;
            cam.main_rgb_preview_height = 0;
            cam.main_rgb_preview_us = 0;
        }
    }

    void handle_media_packet(MediaPacket packet) {
        if(packet.sender_id.empty() || packet.camera_id.empty()) {
            logger_.warn("media packet with empty sender_id/camera_id ignored");
            return;
        }
        const uint64_t packet_receive_us = now_us();
        packet.receiver_receive_timestamp_us = packet_receive_us;
        const auto clock_model = clock_sync_manager_.get_model(packet.sender_id);
        packet.clock_sync_valid = clock_model.valid;
        packet.sender_offset_us = clock_model.offset_us;
        packet.sender_delay_us = clock_model.delay_us;
        packet.sender_drift_ppm = clock_model.drift_ppm;
        // The offset model maps sender system time to receiver time.
        const uint64_t sender_clock_timestamp_us = packet.system_timestamp_us > 0 ? packet.system_timestamp_us : packet.timestamp_us;
        const int64_t global_timestamp_us = clock_model.valid
                                                ? static_cast<int64_t>(sender_clock_timestamp_us) + clock_model.offset_us
                                                : static_cast<int64_t>(sender_clock_timestamp_us);
        packet.global_timestamp_us = global_timestamp_us > 0 ? static_cast<uint64_t>(global_timestamp_us) : 0;

        const bool rgb_stream_packet = packet.stream_type == StreamType::rgb || packet.stream_type == StreamType::rgb_preview;
        const bool rgb_has_idr = rgb_stream_packet &&
                                 (((packet.flags & key_frame) != 0u) || h264_payload_has_nal_type(packet.payload, 5));
        const bool rgb_has_vcl = rgb_stream_packet && h264_payload_has_vcl_nal(packet.payload);

        std::shared_ptr<CameraState> cam;
        bool build_depth_preview = false;
        uint64_t depth_preview_media_us = 0;
        double depth_preview_scale = fallback_depth_scale_for_camera(packet.sender_id, packet.camera_id);
        std::vector<SenderControlTarget> web_preview_control_targets;
        uint64_t web_preview_control_request_us = 0;
        bool should_record = false;
        std::string record_sender_id;
        std::string record_camera_id;
        std::string record_camera_name;
        std::string record_storage_key;
        std::string record_file_prefix;
        std::string record_announce_json;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cam = ensure_camera_ptr_locked(packet.sender_id, packet.camera_id);
            cam->last_media_us = packet_receive_us;
            if(packet.stream_type == StreamType::rgb) {
                cam->rgb_packets++;
                cam->rgb_bytes += packet.payload_size;
                if(config_.preview_enabled) {
                    const auto media_now = cam->last_media_us;
                    const bool stream_requested = is_recent_us(media_now, cam->rgb_stream_requested_until_us, 0);
                    const bool jpeg_requested =
                        is_recent_us(media_now, cam->rgb_preview_requested_until_us, 0) ||
                        (cam->key == main_preview_key_ && is_recent_us(media_now, cam->main_rgb_preview_requested_until_us, 0));
                    const bool decoder_active = static_cast<bool>(cam->rgb_decoder) || static_cast<bool>(cam->main_rgb_decoder);
                    if(stream_requested) {
                        update_h264_stream_buffer_locked(cam->rgb_stream, packet, rgb_has_idr, rgb_has_vcl);
                    }
                    const bool preview_stream_fresh = is_recent_us(media_now, cam->last_rgb_preview_packet_us, kPreviewFreshUs);
                    if(!preview_stream_fresh && (jpeg_requested || decoder_active)) {
                        update_rgb_preview_locked(*cam, packet, rgb_has_idr, rgb_has_vcl);
                    }
                    if(auto target = maybe_web_rgb_preview_control_target_locked(*cam, media_now)) {
                        web_preview_control_request_us = media_now;
                        web_preview_control_targets.push_back(*target);
                    }
                }
            }
            else if(packet.stream_type == StreamType::rgb_preview) {
                cam->last_rgb_preview_packet_us = cam->last_media_us;
                if(config_.preview_enabled) {
                    const auto media_now = cam->last_media_us;
                    const bool stream_requested = is_recent_us(media_now, cam->rgb_stream_requested_until_us, 0);
                    const bool jpeg_requested =
                        is_recent_us(media_now, cam->rgb_preview_requested_until_us, 0) ||
                        (cam->key == main_preview_key_ && is_recent_us(media_now, cam->main_rgb_preview_requested_until_us, 0));
                    const bool decoder_active = static_cast<bool>(cam->rgb_decoder) || static_cast<bool>(cam->main_rgb_decoder);
                    if(stream_requested) {
                        update_h264_stream_buffer_locked(cam->rgb_preview_stream, packet, rgb_has_idr, rgb_has_vcl);
                    }
                    if(jpeg_requested || decoder_active) {
                        update_rgb_preview_locked(*cam, packet, rgb_has_idr, rgb_has_vcl);
                        cam->rgb_preview_width = packet.width;
                        cam->rgb_preview_height = packet.height;
                        cam->rgb_preview_us = cam->last_media_us;
                    }
                    if(auto target = maybe_web_rgb_preview_control_target_locked(*cam, media_now)) {
                        web_preview_control_request_us = media_now;
                        web_preview_control_targets.push_back(*target);
                    }
                }
            }
            else if(packet.stream_type == StreamType::depth_raw) {
                cam->depth_packets++;
                cam->depth_bytes += packet.payload_size;
                build_depth_preview = config_.preview_enabled && is_recent_us(cam->last_media_us, cam->depth_preview_requested_until_us, 0);
                depth_preview_media_us = cam->last_media_us;
                depth_preview_scale = cam->depth_scale;
            }

            should_record = (recording_all_ || cam->recording_requested) && packet.stream_type != StreamType::rgb_preview;
            if(should_record) {
                if(cam->recording_start_us == 0) {
                    cam->recording_start_us = recording_all_ ? recording_all_start_us_ : now_us();
                    cam->recording_file_prefix = effective_file_prefix_locked(*cam);
                }
                record_sender_id = cam->sender_id;
                record_camera_id = cam->camera_id;
                record_camera_name = cam->camera_name;
                record_storage_key = cam->storage_key();
                record_file_prefix = cam->recording_file_prefix;
                record_announce_json = cam->last_announce_live ? cam->last_announce_json : "";
            }
        }
        if(!web_preview_control_targets.empty()) {
            send_web_rgb_preview_controls(web_preview_control_targets, web_preview_control_request_us);
        }

        if(build_depth_preview) {
            std::optional<MediaPacket> preview_depth_packet;
            const MediaPacket *depth_packet = &packet;
            if(packet.stream_type == StreamType::depth_raw && packet.codec_or_compression != "none") {
                try {
                    preview_depth_packet = normalized_depth_packet(packet);
                    depth_packet = &*preview_depth_packet;
                }
                catch(const std::exception &e) {
                    logger_.warn(std::string("depth preview packet ignored camera=") + packet.sender_id + "_" + packet.camera_id
                                 + " frame=" + std::to_string(packet.frame_id) + ": " + e.what());
                    depth_packet = nullptr;
                }
            }
            PreviewImage preview;
            if(depth_packet) {
                preview = build_depth_preview_bmp(depth_packet->payload,
                                                  depth_packet->width,
                                                  depth_packet->height,
                                                  depth_preview_range_for_camera(depth_packet->sender_id, depth_packet->camera_id),
                                                  depth_preview_scale);
            }
            if(!preview.bytes.empty()) {
                std::lock_guard<std::mutex> lock(mutex_);
                if(config_.preview_enabled && cam->online &&
                   is_recent_us(now_us(), cam->depth_preview_requested_until_us, 0) &&
                   cam->last_media_us >= depth_preview_media_us) {
                    cam->depth_preview_ppm = std::move(preview.bytes);
                    cam->depth_preview_width = preview.width;
                    cam->depth_preview_height = preview.height;
                    cam->depth_preview_us = depth_preview_media_us;
                }
            }
        }

        if(should_record) {
            RecordJob job;
            job.packet = std::move(packet);
            job.sender_id = std::move(record_sender_id);
            job.camera_id = std::move(record_camera_id);
            job.camera_name = std::move(record_camera_name);
            job.storage_key = std::move(record_storage_key);
            job.file_prefix = std::move(record_file_prefix);
            job.announce_json = std::move(record_announce_json);
            enqueue_record_job(cam, std::move(job));
        }
    }

    UdpReassemblyStats &udp_stats_locked(bool media_udp) {
        return media_udp ? media_udp_stats_ : preview_udp_stats_;
    }

    void account_incomplete_udp_assembly_locked(const PreviewUdpAssembly &assembly, bool evicted) {
        auto &stats = udp_stats_locked(assembly.media_udp);
        const uint64_t missing = assembly.chunk_count > assembly.received_count
                                     ? static_cast<uint64_t>(assembly.chunk_count - assembly.received_count)
                                     : 0;
        if(evicted) {
            stats.evicted_packets++;
            stats.evicted_missing_fragments += missing;
        }
        else {
            stats.expired_packets++;
            stats.expired_missing_fragments += missing;
        }
    }

    void record_udp_invalid_datagram(bool media_udp) {
        std::lock_guard<std::mutex> lock(preview_udp_mutex_);
        udp_stats_locked(media_udp).invalid_datagrams++;
    }

    void record_udp_parse_rejected_packet(bool media_udp) {
        std::lock_guard<std::mutex> lock(preview_udp_mutex_);
        udp_stats_locked(media_udp).parse_rejected_packets++;
    }

    void record_udp_stream_result(bool media_udp, StreamType stream_type, bool accepted) {
        std::lock_guard<std::mutex> lock(preview_udp_mutex_);
        auto &stats = udp_stats_locked(media_udp);
        if(!accepted) {
            stats.stream_rejected_packets++;
            return;
        }
        switch(stream_type) {
        case StreamType::rgb:
            stats.completed_rgb_packets++;
            break;
        case StreamType::depth_raw:
            stats.completed_depth_packets++;
            break;
        case StreamType::rgb_preview:
            stats.completed_preview_packets++;
            break;
        }
    }

    void cleanup_preview_udp_assemblies_locked(uint64_t now) {
        for(auto it = preview_udp_assemblies_.begin(); it != preview_udp_assemblies_.end();) {
            if(now > it->second.updated_us && now - it->second.updated_us > kPreviewUdpAssemblyTimeoutUs) {
                account_incomplete_udp_assembly_locked(it->second, false);
                it = preview_udp_assemblies_.erase(it);
            }
            else {
                ++it;
            }
        }
        if(preview_udp_assemblies_.size() <= kPreviewUdpMaxAssemblies) {
            return;
        }
        while(preview_udp_assemblies_.size() > kPreviewUdpMaxAssemblies / 2) {
            auto oldest = preview_udp_assemblies_.begin();
            for(auto it = preview_udp_assemblies_.begin(); it != preview_udp_assemblies_.end(); ++it) {
                if(it->second.updated_us < oldest->second.updated_us) {
                    oldest = it;
                }
            }
            account_incomplete_udp_assembly_locked(oldest->second, true);
            preview_udp_assemblies_.erase(oldest);
        }
    }

    void handle_fragmented_udp_datagram(const uint8_t *data, size_t size, const std::string &peer_endpoint, bool media_udp) {
        {
            std::lock_guard<std::mutex> lock(preview_udp_mutex_);
            auto &stats = udp_stats_locked(media_udp);
            stats.datagrams++;
            stats.datagram_bytes += size;
        }
        if(size < kPreviewUdpHeaderSize) {
            record_udp_invalid_datagram(media_udp);
            return;
        }
        const uint32_t magic = read_le32(data + 0);
        const uint16_t version = read_le16(data + 4);
        const uint16_t header_size = read_le16(data + 6);
        if(magic != kPreviewUdpMagic || version != kPreviewUdpHeaderVersion || header_size != kPreviewUdpHeaderSize || size < header_size) {
            record_udp_invalid_datagram(media_udp);
            return;
        }

        const uint32_t sequence = read_le32(data + 8);
        const uint16_t chunk_index = read_le16(data + 12);
        const uint16_t chunk_count = read_le16(data + 14);
        const uint32_t total_size = read_le32(data + 16);
        const uint32_t chunk_offset = read_le32(data + 20);
        const uint16_t chunk_size = read_le16(data + 24);
        if(chunk_count == 0 || chunk_index >= chunk_count || total_size == 0 || total_size > config_.max_payload_bytes
           || chunk_offset > total_size || chunk_size > total_size - chunk_offset || size < header_size + chunk_size) {
            record_udp_invalid_datagram(media_udp);
            return;
        }

        std::vector<uint8_t> completed;
        const uint64_t now = now_us();
        const std::string key = peer_endpoint + "#" + std::to_string(sequence);
        {
            std::lock_guard<std::mutex> lock(preview_udp_mutex_);
            cleanup_preview_udp_assemblies_locked(now);
            auto &stats = udp_stats_locked(media_udp);
            stats.valid_fragments++;
            auto &assembly = preview_udp_assemblies_[key];
            if(assembly.bytes.size() != total_size || assembly.chunk_count != chunk_count || assembly.media_udp != media_udp) {
                if(!assembly.bytes.empty()) {
                    account_incomplete_udp_assembly_locked(assembly, true);
                }
                assembly.bytes.assign(total_size, 0);
                assembly.received.assign(chunk_count, 0);
                assembly.received_count = 0;
                assembly.total_size = total_size;
                assembly.chunk_count = chunk_count;
                assembly.media_udp = media_udp;
                assembly.first_us = now;
                stats.assemblies_started++;
                size_t active_for_type = 0;
                for(const auto &item : preview_udp_assemblies_) {
                    if(item.second.media_udp == media_udp) {
                        ++active_for_type;
                    }
                }
                stats.max_active_assemblies = std::max<uint64_t>(stats.max_active_assemblies, active_for_type);
            }
            assembly.updated_us = now;
            if(!assembly.received[chunk_index]) {
                std::memcpy(assembly.bytes.data() + chunk_offset, data + header_size, chunk_size);
                assembly.received[chunk_index] = 1;
                assembly.received_count++;
            }
            else {
                stats.duplicate_fragments++;
            }
            if(assembly.received_count == assembly.chunk_count) {
                stats.completed_packets++;
                stats.completed_bytes += assembly.total_size;
                completed = std::move(assembly.bytes);
                preview_udp_assemblies_.erase(key);
            }
        }

        if(completed.empty()) {
            return;
        }
        try {
            auto packet = parse_media_packet_buffer(completed.data(), completed.size(), config_.max_payload_bytes);
            const bool accepted = media_udp ? (packet.stream_type == StreamType::rgb || packet.stream_type == StreamType::depth_raw)
                                            : (packet.stream_type == StreamType::rgb_preview);
            record_udp_stream_result(media_udp, packet.stream_type, accepted);
            if(accepted) {
                handle_media_packet(std::move(packet));
            }
        }
        catch(const std::exception &e) {
            record_udp_parse_rejected_packet(media_udp);
            logger_.warn(std::string(media_udp ? "media UDP packet rejected from " : "preview UDP packet rejected from ")
                         + peer_endpoint + ": " + e.what());
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

    void preview_udp_loop() {
        const int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if(fd < 0) {
            logger_.error(std::string("cannot create preview UDP socket: ") + std::strerror(errno));
            return;
        }
        set_fd_cloexec(fd);
        int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        if(!set_socket_recv_buffer(fd, kMediaSocketReceiveBufferBytes)) {
            logger_.warn(std::string("cannot set preview UDP SO_RCVBUF: ") + std::strerror(errno));
        }
        set_socket_timeout(fd, 1);
        const auto addr = make_bind_addr(config_.preview_udp_bind_ip, config_.preview_udp_port);
        if(bind(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
            logger_.error(std::string("cannot bind preview UDP: ") + std::strerror(errno));
            close(fd);
            return;
        }
        logger_.info("preview UDP listening on " + config_.preview_udp_bind_ip + ":" + std::to_string(config_.preview_udp_port));

        std::vector<uint8_t> buffer(65536);
        while(running_ && g_running) {
            sockaddr_in peer{};
            socklen_t peer_len = sizeof(peer);
            const ssize_t got = recvfrom(fd, buffer.data(), buffer.size(), 0, reinterpret_cast<sockaddr *>(&peer), &peer_len);
            if(got < 0) {
                if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                    continue;
                }
                logger_.warn(std::string("preview UDP recv failed: ") + std::strerror(errno));
                continue;
            }
            handle_fragmented_udp_datagram(buffer.data(), static_cast<size_t>(got), socket_endpoint(peer), false);
        }
        close(fd);
    }

    void media_udp_loop() {
        const int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if(fd < 0) {
            logger_.error(std::string("cannot create media UDP socket: ") + std::strerror(errno));
            return;
        }
        set_fd_cloexec(fd);
        int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        if(!set_socket_recv_buffer(fd, kMediaSocketReceiveBufferBytes)) {
            logger_.warn(std::string("cannot set media UDP SO_RCVBUF: ") + std::strerror(errno));
        }
        set_socket_timeout(fd, 1);
        const auto addr = make_bind_addr(config_.media_udp_bind_ip, config_.media_udp_port);
        if(bind(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
            logger_.error(std::string("cannot bind media UDP: ") + std::strerror(errno));
            close(fd);
            return;
        }
        logger_.info("media UDP listening on " + config_.media_udp_bind_ip + ":" + std::to_string(config_.media_udp_port));

        std::vector<uint8_t> buffer(65536);
        while(running_ && g_running) {
            sockaddr_in peer{};
            socklen_t peer_len = sizeof(peer);
            const ssize_t got = recvfrom(fd, buffer.data(), buffer.size(), 0, reinterpret_cast<sockaddr *>(&peer), &peer_len);
            if(got < 0) {
                if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                    continue;
                }
                logger_.warn(std::string("media UDP recv failed: ") + std::strerror(errno));
                continue;
            }
            handle_fragmented_udp_datagram(buffer.data(), static_cast<size_t>(got), socket_endpoint(peer), true);
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
        if(!set_socket_recv_buffer(fd, kMediaSocketReceiveBufferBytes)) {
            logger_.warn(std::string("cannot set media listen SO_RCVBUF: ") + std::strerror(errno));
        }
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
            if(!set_socket_recv_buffer(client, kMediaSocketReceiveBufferBytes)) {
                logger_.warn(std::string("cannot set media client SO_RCVBUF: ") + std::strerror(errno));
            }
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
        MediaPacket packet;
        MediaPacketReadBuffers read_buffers;
        while(running_ && g_running) {
            try {
                read_media_packet_into(fd, config_.max_payload_bytes, read_buffers, packet);
                last_sender = packet.sender_id;
                last_camera = packet.camera_id;
                last_stream = stream_type_name(packet.stream_type);
                last_frame_id = packet.frame_id;
                handle_media_packet(std::move(packet));
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
            std::thread([this, client] {
                handle_admin_client(client);
                close(client);
            }).detach();
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
        else if(method == "GET" && path == "/api/preview/rgb-h264") {
            stream_rgb_h264_preview(fd, args.count("sender_id") ? args.at("sender_id") : "",
                                    args.count("camera_id") ? args.at("camera_id") : "");
            return;
        }
        else if(method == "GET" && path == "/api/preview/rgb-h264-frames") {
            stream_rgb_h264_preview_frames(fd, args.count("sender_id") ? args.at("sender_id") : "",
                                           args.count("camera_id") ? args.at("camera_id") : "");
            return;
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
        send_all(fd, text);
    }

    Config config_;
    Logger logger_;
    RuntimeState runtime_state_;
    ClockSyncManager clock_sync_manager_;
    std::atomic<bool> running_{false};
    std::atomic<int> active_media_clients_{0};
    std::thread udp_thread_;
    std::thread media_udp_thread_;
    std::thread preview_udp_thread_;
    std::thread tcp_thread_;
    std::thread admin_thread_;
    std::mutex mutex_;
    std::mutex preview_udp_mutex_;
    std::string main_preview_key_;
    bool recording_all_ = false;
    uint64_t recording_all_start_us_ = 0;
    bool recording_all_has_file_prefix_override_ = false;
    std::string recording_all_file_prefix_;
    std::map<std::string, std::shared_ptr<CameraState>> cameras_;
    std::unordered_map<std::string, PreviewUdpAssembly> preview_udp_assemblies_;
    UdpReassemblyStats media_udp_stats_;
    UdpReassemblyStats preview_udp_stats_;
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
