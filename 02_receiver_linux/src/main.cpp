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
#include <functional>
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
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <zlib.h>
#include <json/json.h>

extern char **environ;

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
constexpr int kRgbPreviewReadPollMs = 100;
constexpr int kRgbPreviewWritePollMs = 2;
constexpr int kRgbPreviewWriteBudgetMs = 25;
constexpr size_t kRgbPreviewDecoderMaxQueuedPackets = 8;
constexpr size_t kRgbPreviewDecoderMaxQueuedBytes = 4ull * 1024ull * 1024ull;
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
constexpr uint64_t kRgbDepthPairValidMaxDeltaUs = 20ull * 1000ull;
constexpr size_t kRgbH264StreamMaxPackets = 180;
constexpr size_t kRgbH264StreamMaxHeaderBytes = 512ull * 1024ull;
constexpr uint32_t kRecordFpsProbeFrames = 60;
constexpr uint64_t kRecordFpsProbeMaxWaitUs = 3'000'000ull;
constexpr double kMinRecordFps = 5.0;
constexpr double kMaxRecordFps = 60.0;
constexpr size_t kMaxPendingRgbRecordBytes = 8ull * 1024ull * 1024ull;
constexpr size_t kMaxPendingDepthRecordBytes = 64ull * 1024ull * 1024ull;
constexpr size_t kDefaultRecordQueueMaxBytes = 512ull * 1024ull * 1024ull;
constexpr size_t kDefaultRecordFinalizeMaxPendingSegments = 8;
constexpr uint64_t kRecordQueueWarnIntervalUs = 5ull * 1000ull * 1000ull;
constexpr int kMaxActiveMediaClients = 32;
constexpr int kMaxActiveAdminClients = 32;
constexpr size_t kMaxTrackedCameras = 32;
constexpr size_t kMaxTrackedSenders = 32;
constexpr int kMediaClientSocketTimeoutSec = 2;
constexpr int kMediaSocketReceiveBufferBytes = 16 * 1024 * 1024;
constexpr uint64_t kPreviewUdpAssemblyTimeoutUs = 1ull * 1000ull * 1000ull;
constexpr size_t kPreviewUdpMaxAssemblies = 256;
constexpr size_t kPreviewUdpMaxAssemblyBytes = 256ull * 1024ull * 1024ull;
constexpr size_t kPreviewUdpMaxPacketBytes = 32ull * 1024ull * 1024ull;
constexpr uint16_t kPreviewUdpMaxChunks = 32768;
constexpr size_t kMaxDepthCompressionChunks = 256;
constexpr size_t kMaxDepthDecompressionWorkers = 8;
constexpr uint32_t kMaxMediaDimension = 16384;
constexpr size_t kMaxProtocolIdBytes = 64;
constexpr size_t kMaxCodecNameBytes = 32;
constexpr uint64_t kAnnounceCacheSaveMinIntervalUs = 60ull * 1000ull * 1000ull;
constexpr uint64_t kRoutineStatusLogMinIntervalUs = 60ull * 1000ull * 1000ull;
constexpr uint64_t kMaxGlobalTimestampReceiverSkewUs = 10ull * 60ull * 1000ull * 1000ull;
constexpr const char *kRgbMp4RecordMuxFlags = "+frag_keyframe+empty_moov+default_base_moof+global_sidx";
constexpr const char *kH264FullRangeMetadataBsf =
    "h264_metadata=video_full_range_flag=1:matrix_coefficients=6";

std::atomic<bool> g_running{true};

template <typename Function>
class ScopeExit {
public:
    explicit ScopeExit(Function function) : function_(std::move(function)) {}
    ~ScopeExit() noexcept {
        if(active_) {
            try {
                function_();
            }
            catch(...) {
            }
        }
    }

    void release() noexcept { active_ = false; }

    ScopeExit(const ScopeExit &) = delete;
    ScopeExit &operator=(const ScopeExit &) = delete;

private:
    Function function_;
    bool active_ = true;
};

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

bool parse_json_object_strict(const std::string &json, Json::Value &root) {
    Json::CharReaderBuilder builder;
    builder["collectComments"] = false;
    builder["failIfExtra"] = true;
    builder["strictRoot"] = true;
    std::string errors;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    return reader->parse(json.data(), json.data() + json.size(), &root, &errors) && root.isObject();
}

std::string json_string_value(const Json::Value &root, const char *key, const std::string &fallback = {}) {
    const auto &value = root[key];
    return value.isString() ? value.asString() : fallback;
}

std::optional<int64_t> json_int64_value(const Json::Value &root, const char *key) {
    const auto &value = root[key];
    if(value.isInt64()) {
        return value.asInt64();
    }
    if(value.isUInt64() && value.asUInt64() <= static_cast<Json::UInt64>(std::numeric_limits<int64_t>::max())) {
        return static_cast<int64_t>(value.asUInt64());
    }
    return std::nullopt;
}

std::optional<uint64_t> json_uint64_value(const Json::Value &root, const char *key) {
    const auto &value = root[key];
    if(value.isUInt64()) {
        return value.asUInt64();
    }
    if(value.isInt64() && value.asInt64() >= 0) {
        return static_cast<uint64_t>(value.asInt64());
    }
    return std::nullopt;
}

std::optional<double> json_double_value(const Json::Value &root, const char *key) {
    const auto &value = root[key];
    if(!value.isNumeric()) {
        return std::nullopt;
    }
    const double result = value.asDouble();
    return std::isfinite(result) ? std::optional<double>(result) : std::nullopt;
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
        try {
            return std::stoi(match[1].str());
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

bool create_cloexec_pipe(int fds[2]) {
#if defined(__linux__) && defined(O_CLOEXEC)
    if(pipe2(fds, O_CLOEXEC) == 0) {
        return true;
    }
    if(errno != ENOSYS && errno != EINVAL) {
        return false;
    }
#endif
    if(pipe(fds) != 0) {
        return false;
    }
    set_fd_cloexec(fds[0]);
    set_fd_cloexec(fds[1]);
    return true;
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

std::string socket_endpoint_ip(const std::string &endpoint) {
    const auto parsed = parse_socket_endpoint(endpoint);
    if(!parsed) {
        return {};
    }
    char ip[INET_ADDRSTRLEN] = {};
    return inet_ntop(AF_INET, &parsed->sin_addr, ip, sizeof(ip)) ? std::string(ip) : std::string{};
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
    set_fd_cloexec(fd);
    const auto sent = sendto(fd, payload.data(), payload.size(), 0, reinterpret_cast<const sockaddr *>(&*addr), sizeof(*addr));
    close(fd);
    return sent >= 0 && static_cast<size_t>(sent) == payload.size();
}

std::string camera_key(const std::string &sender_id, const std::string &camera_id) {
    return sender_id + "_" + camera_id;
}

bool is_valid_protocol_id(const std::string &value) {
    if(value.empty() || value.size() > kMaxProtocolIdBytes) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isalnum(ch) || ch == '_' || ch == '-';
    });
}

bool is_valid_codec_name(const std::string &value) {
    if(value.empty() || value.size() > kMaxCodecNameBytes) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isalnum(ch) || ch == '_' || ch == '-';
    });
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
    struct RecordingStagingConfig {
        bool enabled = false;
        std::string root;
        bool defer_player_compatible_finalize = true;
        int idle_finalize_ms = 5000;
    };

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
    RecordingStagingConfig recording_staging;
    std::string log_directory = "08_reports/receiver_logs";
    std::string state_path = "06_configs/receiver_runtime_state.json";
    std::string ffmpeg_path = "ffmpeg";
    int segment_seconds = 300;
    int depth_fps = 30;
    bool write_debug_h264 = false;
    bool write_debug_depth_raw = false;
    std::set<std::string> rgb_h264_full_range_camera_keys;
    size_t max_payload_bytes = 32ull * 1024ull * 1024ull;
    size_t record_queue_max_bytes = kDefaultRecordQueueMaxBytes;
    size_t record_queue_total_max_bytes = 2ull * 1024ull * 1024ull * 1024ull;
    size_t record_finalize_max_pending_segments = kDefaultRecordFinalizeMaxPendingSegments;
    uint64_t min_free_disk_bytes = 2ull * 1024ull * 1024ull * 1024ull;
};

Config load_config(const std::string &path) {
    std::ifstream input(path);
    if(!input) {
        throw std::runtime_error("cannot open receiver config: " + path);
    }
    const std::string json((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    Json::Value root;
    if(!parse_json_object_strict(json, root)) {
        throw std::runtime_error("invalid receiver JSON config: " + path);
    }

    const auto string_value = [](const Json::Value &object, const char *key, const std::string &fallback) {
        const auto &value = object[key];
        if(value.isNull()) {
            return fallback;
        }
        if(!value.isString()) {
            throw std::runtime_error(std::string("receiver config field must be a string: ") + key);
        }
        return value.asString();
    };
    const auto int_value = [](const Json::Value &object, const char *key, int fallback) {
        const auto &value = object[key];
        if(value.isNull()) {
            return fallback;
        }
        if(!value.isInt()) {
            throw std::runtime_error(std::string("receiver config field must be an integer: ") + key);
        }
        return value.asInt();
    };
    const auto bool_value = [](const Json::Value &object, const char *key, bool fallback) {
        const auto &value = object[key];
        if(value.isNull()) {
            return fallback;
        }
        if(!value.isBool()) {
            throw std::runtime_error(std::string("receiver config field must be a boolean: ") + key);
        }
        return value.asBool();
    };
    const auto string_set_value = [](const Json::Value &object, const char *key) {
        std::set<std::string> values;
        const auto &value = object[key];
        if(value.isNull()) {
            return values;
        }
        if(!value.isArray()) {
            throw std::runtime_error(std::string("receiver config field must be a string array: ") + key);
        }
        for(const auto &item : value) {
            if(!item.isString() || item.asString().empty()) {
                throw std::runtime_error(std::string("receiver config field contains an invalid camera key: ") + key);
            }
            values.insert(item.asString());
        }
        return values;
    };
    const auto port_value = [&](const Json::Value &object, const char *key, uint16_t fallback) {
        const int value = int_value(object, key, fallback);
        if(value <= 0 || value > 65535) {
            throw std::runtime_error(std::string("invalid receiver config port: ") + key);
        }
        return static_cast<uint16_t>(value);
    };

    Config cfg;
    cfg.status_bind_ip = string_value(root, "status_bind_ip", cfg.status_bind_ip);
    cfg.status_port = port_value(root, "status_port", cfg.status_port);
    cfg.media_bind_ip = string_value(root, "media_bind_ip", cfg.media_bind_ip);
    cfg.media_port = port_value(root, "media_port", cfg.media_port);
    cfg.preview_enabled = bool_value(root, "preview_enabled", cfg.preview_enabled);
    cfg.media_udp_enabled = bool_value(root, "media_udp_enabled", cfg.media_udp_enabled);
    cfg.media_udp_bind_ip = string_value(root, "media_udp_bind_ip", cfg.media_udp_bind_ip);
    cfg.media_udp_port = port_value(root, "media_udp_port", cfg.media_udp_port);
    cfg.preview_udp_enabled = bool_value(root, "preview_udp_enabled", cfg.preview_udp_enabled);
    cfg.preview_udp_bind_ip = string_value(root, "preview_udp_bind_ip", cfg.preview_udp_bind_ip);
    cfg.preview_udp_port = port_value(root, "preview_udp_port", cfg.preview_udp_port);
    Json::Value clock_sync(Json::objectValue);
    if(!root["clock_sync"].isNull()) {
        if(!root["clock_sync"].isObject()) {
            throw std::runtime_error("receiver config field must be an object: clock_sync");
        }
        clock_sync = root["clock_sync"];
    }
    cfg.clock_sync.enabled = bool_value(clock_sync, "enabled", cfg.clock_sync.enabled);
    cfg.clock_sync.bind_ip = string_value(clock_sync, "bind_ip", cfg.clock_sync.bind_ip);
    cfg.clock_sync.port = port_value(clock_sync, "port", cfg.clock_sync.port);
    cfg.clock_sync.model_timeout_ms = int_value(clock_sync, "model_timeout_ms", cfg.clock_sync.model_timeout_ms);
    cfg.admin_bind_ip = string_value(root, "admin_bind_ip", cfg.admin_bind_ip);
    cfg.admin_port = port_value(root, "admin_port", cfg.admin_port);
    cfg.nas_root = string_value(root, "nas_root", cfg.nas_root);
    Json::Value recording_staging(Json::objectValue);
    if(!root["recording_staging"].isNull()) {
        if(!root["recording_staging"].isObject()) {
            throw std::runtime_error("receiver config field must be an object: recording_staging");
        }
        recording_staging = root["recording_staging"];
    }
    cfg.recording_staging.enabled = bool_value(recording_staging, "enabled", cfg.recording_staging.enabled);
    cfg.recording_staging.root = string_value(recording_staging, "root", cfg.recording_staging.root);
    cfg.recording_staging.defer_player_compatible_finalize =
        bool_value(recording_staging, "defer_player_compatible_finalize",
                   cfg.recording_staging.defer_player_compatible_finalize);
    cfg.recording_staging.idle_finalize_ms =
        int_value(recording_staging, "idle_finalize_ms", cfg.recording_staging.idle_finalize_ms);
    cfg.log_directory = string_value(root, "log_directory", cfg.log_directory);
    cfg.state_path = string_value(root, "state_path", cfg.state_path);
    cfg.ffmpeg_path = string_value(root, "ffmpeg_path", cfg.ffmpeg_path);
    cfg.segment_seconds = int_value(root, "segment_seconds", cfg.segment_seconds);
    cfg.depth_fps = int_value(root, "depth_fps", cfg.depth_fps);
    cfg.write_debug_h264 = bool_value(root, "write_debug_h264", cfg.write_debug_h264);
    cfg.write_debug_depth_raw = bool_value(root, "write_debug_depth_raw", cfg.write_debug_depth_raw);
    cfg.rgb_h264_full_range_camera_keys = string_set_value(root, "rgb_h264_full_range_camera_keys");
    const int max_payload_mb = int_value(root, "max_payload_mb", 32);
    const int record_queue_max_mb = int_value(root, "record_queue_max_mb", 512);
    const int min_free_disk_mb = int_value(root, "min_free_disk_mb", 2048);
    const int record_queue_total_max_mb = int_value(root, "record_queue_total_max_mb", 2048);
    const int record_finalize_max_pending_segments =
        int_value(root, "record_finalize_max_pending_segments", static_cast<int>(kDefaultRecordFinalizeMaxPendingSegments));
    if(max_payload_mb <= 0 || max_payload_mb > 128 || record_queue_max_mb <= 0 || record_queue_max_mb > 4096
       || record_queue_total_max_mb <= 0 || record_queue_total_max_mb > 16384
       || record_finalize_max_pending_segments <= 0 || record_finalize_max_pending_segments > 128
       || min_free_disk_mb < 0 || min_free_disk_mb > 1024 * 1024) {
        throw std::runtime_error("receiver payload/record queue limits are out of range");
    }
    cfg.max_payload_bytes = static_cast<size_t>(max_payload_mb) * 1024ull * 1024ull;
    cfg.record_queue_max_bytes = static_cast<size_t>(record_queue_max_mb) * 1024ull * 1024ull;
    cfg.record_queue_total_max_bytes = static_cast<size_t>(record_queue_total_max_mb) * 1024ull * 1024ull;
    cfg.record_finalize_max_pending_segments = static_cast<size_t>(record_finalize_max_pending_segments);
    cfg.min_free_disk_bytes = static_cast<uint64_t>(min_free_disk_mb) * 1024ull * 1024ull;

    if(cfg.segment_seconds <= 0) {
        throw std::runtime_error("segment_seconds must be positive");
    }
    if(cfg.depth_fps <= 0) {
        throw std::runtime_error("depth_fps must be positive");
    }
    if(cfg.clock_sync.model_timeout_ms <= 0) {
        throw std::runtime_error("clock_sync.model_timeout_ms must be positive");
    }
    if(cfg.recording_staging.enabled && cfg.recording_staging.root.empty()) {
        throw std::runtime_error("recording_staging.root must not be empty when staging is enabled");
    }
    if(cfg.recording_staging.idle_finalize_ms < 1000 || cfg.recording_staging.idle_finalize_ms > 300000) {
        throw std::runtime_error("recording_staging.idle_finalize_ms must be between 1000 and 300000");
    }
    if(cfg.admin_bind_ip != "127.0.0.1") {
        throw std::runtime_error("admin_bind_ip must remain 127.0.0.1; expose only the authenticated Web proxy");
    }
    (void)make_bind_addr(cfg.status_bind_ip, cfg.status_port);
    (void)make_bind_addr(cfg.media_bind_ip, cfg.media_port);
    (void)make_bind_addr(cfg.media_udp_bind_ip, cfg.media_udp_port);
    (void)make_bind_addr(cfg.preview_udp_bind_ip, cfg.preview_udp_port);
    (void)make_bind_addr(cfg.admin_bind_ip, cfg.admin_port);
    if(cfg.nas_root.empty() || cfg.log_directory.empty() || cfg.state_path.empty() || cfg.ffmpeg_path.empty()) {
        throw std::runtime_error("receiver path fields must not be empty");
    }
    for(const auto &key : cfg.rgb_h264_full_range_camera_keys) {
        if(key.size() > kMaxProtocolIdBytes * 2 + 1
           || !std::all_of(key.begin(), key.end(), [](unsigned char ch) {
                  return std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.';
              })) {
            throw std::runtime_error("invalid camera key in rgb_h264_full_range_camera_keys: " + key);
        }
    }
    std::set<uint16_t> udp_ports{cfg.status_port};
    const auto add_udp_port = [&](bool enabled, uint16_t port, const char *name) {
        if(enabled && !udp_ports.insert(port).second) {
            throw std::runtime_error(std::string("receiver UDP port conflict: ") + name);
        }
    };
    add_udp_port(cfg.clock_sync.enabled, cfg.clock_sync.port, "clock_sync.port");
    add_udp_port(cfg.media_udp_enabled, cfg.media_udp_port, "media_udp_port");
    add_udp_port(cfg.preview_enabled && cfg.preview_udp_enabled, cfg.preview_udp_port, "preview_udp_port");
    if(cfg.admin_port == cfg.media_port
       && (cfg.admin_bind_ip == cfg.media_bind_ip || cfg.admin_bind_ip == "0.0.0.0" || cfg.media_bind_ip == "0.0.0.0")) {
        throw std::runtime_error("admin_port conflicts with media_port on the same bind address");
    }
    return cfg;
}

bool rgb_h264_full_range_for_camera(const Config &cfg, const std::string &sender_id, const std::string &camera_id) {
    return cfg.rgb_h264_full_range_camera_keys.count(camera_key(sender_id, camera_id)) != 0;
}

struct RuntimeState {
    std::string default_file_prefix;
    std::map<std::string, std::string> camera_names;
    std::map<std::string, std::string> camera_file_prefixes;
    std::map<std::string, std::string> camera_announces;
};

std::map<std::string, std::string> json_string_map_field(const Json::Value &root, const char *key) {
    std::map<std::string, std::string> result;
    const auto &object = root[key];
    if(!object.isObject()) {
        return result;
    }
    for(const auto &name : object.getMemberNames()) {
        if(result.size() >= kMaxTrackedCameras || name.size() > 160 || !is_safe_storage_text(name) || !object[name].isString()) {
            continue;
        }
        result[name] = object[name].asString();
    }
    return result;
}

std::map<std::string, std::string> json_object_map_field(const Json::Value &root, const char *key) {
    std::map<std::string, std::string> result;
    const auto &object = root[key];
    if(!object.isObject()) {
        return result;
    }
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    for(const auto &name : object.getMemberNames()) {
        if(result.size() >= kMaxTrackedCameras || name.size() > 160 || !is_safe_storage_text(name) || !object[name].isObject()) {
            continue;
        }
        result[name] = Json::writeString(builder, object[name]);
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
    Json::Value root;
    if(!parse_json_object_strict(json, root)) {
        return state;
    }
    state.default_file_prefix = root["default_file_prefix"].isString() ? root["default_file_prefix"].asString() : std::string{};
    state.camera_names = json_string_map_field(root, "camera_names");
    state.camera_file_prefixes = json_string_map_field(root, "camera_file_prefixes");
    state.camera_announces = json_object_map_field(root, "camera_announces");
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
    Json::Value root(Json::objectValue);
    root["default_file_prefix"] = state.default_file_prefix;
    root["camera_names"] = Json::Value(Json::objectValue);
    for(const auto &item : state.camera_names) {
        root["camera_names"][item.first] = item.second;
    }
    root["camera_file_prefixes"] = Json::Value(Json::objectValue);
    for(const auto &item : state.camera_file_prefixes) {
        root["camera_file_prefixes"][item.first] = item.second;
    }
    root["camera_announces"] = Json::Value(Json::objectValue);
    for(const auto &item : state.camera_announces) {
        Json::Value announce;
        if(parse_json_object_strict(item.second, announce)) {
            root["camera_announces"][item.first] = std::move(announce);
        }
    }
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "  ";
    out << Json::writeString(builder, root) << '\n';
    out.close();
    if(!out) {
        throw std::runtime_error("cannot finish receiver state: " + tmp_path);
    }
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
               bool h264_full_range,
               Logger &logger) {
        stop();
        key_ = key;
        source_width_ = width;
        source_height_ = height;
        preview_width_ = target_width == 0 ? width : (width > 0 ? std::min<uint32_t>(width, target_width) : target_width);
        preview_height_ = scaled_height(width, height, preview_width_);

        int stdin_pipe[2] = {-1, -1};
        int stdout_pipe[2] = {-1, -1};
        if(!create_cloexec_pipe(stdin_pipe) || !create_cloexec_pipe(stdout_pipe)) {
            const int pipe_errno = errno;
            close_pipe(stdin_pipe);
            close_pipe(stdout_pipe);
            logger.warn("rgb preview decoder pipe creation failed: " + std::string(std::strerror(pipe_errno)));
            return false;
        }

        std::string scale = "fps=" + std::to_string(preview_fps);
        if(h264_full_range) {
            scale += ",setparams=range=full:colorspace=smpte170m";
        }
        if(target_width != 0) {
            scale += ",scale=" + std::to_string(target_width) + ":-2";
            if(h264_full_range) {
                scale += ":in_color_matrix=smpte170m:out_color_matrix=smpte170m:in_range=full:out_range=full";
            }
        }
        const std::string jpeg_quality = std::to_string(kRgbPreviewJpegQuality);
        std::vector<std::string> arguments = {
            cfg.ffmpeg_path, "-hide_banner", "-loglevel", "error", "-fflags", "nobuffer", "-flags", "low_delay",
            "-probesize", "32", "-analyzeduration", "0", "-avioflags", "direct", "-f", "h264", "-i", "pipe:0",
            "-vf", scale, "-q:v", jpeg_quality, "-f", "image2pipe", "-vcodec", "mjpeg", "pipe:1"};
        std::vector<char *> argv;
        argv.reserve(arguments.size() + 1);
        for(auto &argument : arguments) {
            argv.push_back(argument.data());
        }
        argv.push_back(nullptr);

        posix_spawn_file_actions_t actions;
        int spawn_rc = posix_spawn_file_actions_init(&actions);
        const bool actions_initialized = spawn_rc == 0;
        if(spawn_rc == 0) {
            spawn_rc = posix_spawn_file_actions_adddup2(&actions, stdin_pipe[0], STDIN_FILENO);
        }
        if(spawn_rc == 0) {
            spawn_rc = posix_spawn_file_actions_adddup2(&actions, stdout_pipe[1], STDOUT_FILENO);
        }
        if(spawn_rc == 0) {
            spawn_rc = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
        }
        if(spawn_rc == 0) {
            spawn_rc = posix_spawn_file_actions_addclose(&actions, stdin_pipe[1]);
        }
        if(spawn_rc == 0) {
            spawn_rc = posix_spawn_file_actions_addclose(&actions, stdout_pipe[0]);
        }
        if(spawn_rc == 0 && stdin_pipe[0] != STDIN_FILENO) {
            spawn_rc = posix_spawn_file_actions_addclose(&actions, stdin_pipe[0]);
        }
        if(spawn_rc == 0 && stdout_pipe[1] != STDOUT_FILENO) {
            spawn_rc = posix_spawn_file_actions_addclose(&actions, stdout_pipe[1]);
        }

        pid_t pid = -1;
        if(spawn_rc == 0) {
            spawn_rc = posix_spawnp(&pid, cfg.ffmpeg_path.c_str(), &actions, nullptr, argv.data(), environ);
        }
        if(actions_initialized) {
            posix_spawn_file_actions_destroy(&actions);
        }
        if(spawn_rc != 0) {
            close_pipe(stdin_pipe);
            close_pipe(stdout_pipe);
            logger.warn("rgb preview decoder spawn failed: " + std::string(std::strerror(spawn_rc)));
            return false;
        }

        close(stdin_pipe[0]);
        close(stdout_pipe[1]);
        stdin_fd_ = stdin_pipe[1];
        stdout_fd_ = stdout_pipe[0];
        set_pipe_size_if_supported(stdin_fd_, kRgbPreviewPipeBytes);
        set_fd_nonblocking(stdin_fd_);
        set_fd_nonblocking(stdout_fd_);
        pid_ = pid;
        running_ = true;
        const int reader_fd = stdout_fd_;
        reader_ = std::thread([this, reader_fd] { read_loop(reader_fd); });
        writer_ = std::thread([this] { write_loop(); });
        logger.info("rgb preview decoder started: " + key_ + " h264_full_range=" + (h264_full_range ? "true" : "false"));
        return true;
    }

    bool active() const {
        std::lock_guard<std::mutex> lock(process_mutex_);
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
        {
            std::lock_guard<std::mutex> lock(write_queue_mutex_);
            write_queue_.clear();
            write_queue_bytes_ = 0;
        }
        write_queue_cv_.notify_all();
        if(stdin_fd >= 0) {
            close(stdin_fd);
        }
        if(pid > 0) {
            kill(pid, SIGTERM);
        }
        if(writer_.joinable()) {
            writer_.join();
        }
        if(pid > 0) {
            int status = 0;
            for(int i = 0; i < 10; ++i) {
                const pid_t done = waitpid(pid, &status, WNOHANG);
                if(done == pid || (done < 0 && errno == ECHILD)) {
                    pid = -1;
                    break;
                }
                usleep(50 * 1000);
            }
            if(pid > 0) {
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
            }
        }
        // A blocking read on a pipe is not guaranteed to be interrupted when
        // another thread closes its descriptor. Reap the child first so the
        // writer end closes and the reader observes EOF before it is joined.
        if(reader_.joinable()) {
            reader_.join();
        }
        if(stdout_fd >= 0) {
            close(stdout_fd);
        }
    }

    bool write_packet(const std::vector<uint8_t> &payload) {
        if(payload.empty()) {
            return true;
        }
        if(!active()) {
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(write_queue_mutex_);
            if(!running_) {
                return false;
            }
            while(!write_queue_.empty()
                  && (write_queue_.size() >= kRgbPreviewDecoderMaxQueuedPackets
                      || write_queue_bytes_ + payload.size() > kRgbPreviewDecoderMaxQueuedBytes)) {
                write_queue_bytes_ -= write_queue_.front().size();
                write_queue_.pop_front();
            }
            if(payload.size() > kRgbPreviewDecoderMaxQueuedBytes) {
                write_queue_.clear();
                write_queue_bytes_ = 0;
                return false;
            }
            write_queue_bytes_ += payload.size();
            write_queue_.push_back(payload);
        }
        write_queue_cv_.notify_one();
        return true;
    }

    bool write_payload_to_process(const std::vector<uint8_t> &payload) {
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

    void read_loop(int fd) {
        std::vector<uint8_t> buffer;
        buffer.reserve(512 * 1024);
        std::vector<uint8_t> chunk(32 * 1024);
        while(running_) {
            pollfd pfd{};
            pfd.fd = fd;
            pfd.events = POLLIN;
            int poll_rc = 0;
            do {
                poll_rc = poll(&pfd, 1, kRgbPreviewReadPollMs);
            } while(poll_rc < 0 && errno == EINTR && running_);
            if(!running_) {
                break;
            }
            if(poll_rc < 0 || (pfd.revents & (POLLERR | POLLNVAL)) != 0) {
                break;
            }
            if(poll_rc == 0 || (pfd.revents & (POLLIN | POLLHUP)) == 0) {
                continue;
            }
            while(running_) {
                const ssize_t got = read(fd, chunk.data(), chunk.size());
                if(got > 0) {
                    buffer.insert(buffer.end(), chunk.begin(), chunk.begin() + got);
                    consume_jpegs(buffer);
                    if(buffer.size() > 4ull * 1024ull * 1024ull) {
                        buffer.erase(buffer.begin(), buffer.end() - 1024);
                    }
                    continue;
                }
                if(got < 0 && errno == EINTR) {
                    continue;
                }
                if(got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    break;
                }
                return;
            }
        }
    }

    void write_loop() {
        while(true) {
            std::vector<uint8_t> payload;
            {
                std::unique_lock<std::mutex> lock(write_queue_mutex_);
                write_queue_cv_.wait(lock, [&] { return !running_ || !write_queue_.empty(); });
                if(write_queue_.empty()) {
                    if(!running_) {
                        return;
                    }
                    continue;
                }
                payload = std::move(write_queue_.front());
                write_queue_bytes_ -= payload.size();
                write_queue_.pop_front();
            }
            if(!write_payload_to_process(payload)) {
                {
                    std::lock_guard<std::mutex> lock(write_queue_mutex_);
                    write_queue_.clear();
                    write_queue_bytes_ = 0;
                    running_ = false;
                }
                write_queue_cv_.notify_all();
                return;
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
    std::thread writer_;
    mutable std::mutex write_queue_mutex_;
    std::condition_variable write_queue_cv_;
    std::deque<std::vector<uint8_t>> write_queue_;
    size_t write_queue_bytes_ = 0;
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

void validate_media_packet_metadata(const MediaPacket &packet, size_t max_payload_bytes) {
    if(!is_valid_protocol_id(packet.sender_id) || !is_valid_protocol_id(packet.camera_id)) {
        throw std::runtime_error("invalid media sender_id/camera_id");
    }
    if(!is_valid_codec_name(packet.codec_or_compression)) {
        throw std::runtime_error("invalid media codec/compression name");
    }
    if(packet.stream_type != StreamType::rgb && packet.stream_type != StreamType::rgb_preview
       && packet.stream_type != StreamType::depth_raw) {
        throw std::runtime_error("invalid media stream type");
    }
    if(packet.width == 0 || packet.height == 0 || packet.width > kMaxMediaDimension || packet.height > kMaxMediaDimension) {
        throw std::runtime_error("invalid media dimensions");
    }
    if(packet.payload_size == 0 || packet.payload_size > max_payload_bytes) {
        throw std::runtime_error("invalid media payload size");
    }
    if(packet.uncompressed_size > kMaxReasonablePayload) {
        throw std::runtime_error("invalid media uncompressed size");
    }
    if(packet.stream_type == StreamType::depth_raw) {
        if(packet.pixel_format != PixelFormat::depth_u16) {
            throw std::runtime_error("invalid depth pixel format");
        }
        const uint64_t expected_raw_size = static_cast<uint64_t>(packet.width) * packet.height * sizeof(uint16_t);
        if(expected_raw_size > kMaxReasonablePayload || packet.uncompressed_size != expected_raw_size) {
            throw std::runtime_error("depth dimensions and uncompressed size disagree");
        }
        if(packet.codec_or_compression == "none" && packet.payload_size != expected_raw_size) {
            throw std::runtime_error("raw depth payload size disagrees with dimensions");
        }
        static const std::set<std::string> supported_depth_codecs = {
            "none", "zlib", "rvl", "qdelta", "lz4", "plz4", "pzlib", "q8lz4", "q8zlib", "pq12zlib", "pq8zlib", "pq8lz4"};
        if(supported_depth_codecs.count(packet.codec_or_compression) == 0) {
            throw std::runtime_error("unsupported depth compression");
        }
    }
    else {
        if(packet.pixel_format != PixelFormat::encoded_video || packet.codec_or_compression != "h264") {
            throw std::runtime_error("invalid RGB media format metadata");
        }
    }
}

class DepthDecompressionExecutor {
public:
    DepthDecompressionExecutor() {
        const size_t hardware_workers = std::max<size_t>(1, std::thread::hardware_concurrency());
        const size_t worker_count = std::min(hardware_workers, kMaxDepthDecompressionWorkers);
        workers_.reserve(worker_count);
        for(size_t i = 0; i < worker_count; ++i) {
            try {
                workers_.emplace_back([this] { worker_loop(); });
            }
            catch(...) {
                break;
            }
        }
    }

    ~DepthDecompressionExecutor() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        cv_.notify_all();
        for(auto &worker : workers_) {
            if(worker.joinable()) {
                worker.join();
            }
        }
    }

    DepthDecompressionExecutor(const DepthDecompressionExecutor &) = delete;
    DepthDecompressionExecutor &operator=(const DepthDecompressionExecutor &) = delete;

    bool try_submit(std::function<void()> task) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(stopping_ || workers_.empty() || tasks_.size() >= kMaxQueuedTasks) {
            return false;
        }
        tasks_.push_back(std::move(task));
        cv_.notify_one();
        return true;
    }

private:
    void worker_loop() {
        for(;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [&] { return stopping_ || !tasks_.empty(); });
                if(tasks_.empty()) {
                    if(stopping_) {
                        return;
                    }
                    continue;
                }
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            try {
                task();
            }
            catch(...) {
            }
        }
    }

    static constexpr size_t kMaxQueuedTasks = 128;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> tasks_;
    std::vector<std::thread> workers_;
    bool stopping_ = false;
};

DepthDecompressionExecutor &depth_decompression_executor() {
    static DepthDecompressionExecutor executor;
    return executor;
}

template <typename Function>
void bounded_parallel_for(size_t count, Function &&function) {
    if(count == 0) {
        return;
    }
    const size_t hardware_workers = std::max<size_t>(1, std::thread::hardware_concurrency());
    const size_t worker_count = std::min({count, hardware_workers, kMaxDepthDecompressionWorkers});
    std::atomic<size_t> next{0};
    std::atomic<bool> failed{false};
    std::mutex error_mutex;
    std::exception_ptr error;
    auto worker = [&] {
        while(!failed.load(std::memory_order_relaxed)) {
            const size_t index = next.fetch_add(1, std::memory_order_relaxed);
            if(index >= count) {
                break;
            }
            try {
                function(index);
            }
            catch(...) {
                failed.store(true, std::memory_order_relaxed);
                std::lock_guard<std::mutex> lock(error_mutex);
                if(!error) {
                    error = std::current_exception();
                }
                break;
            }
        }
    };

    std::atomic<size_t> pending_workers{0};
    std::mutex completion_mutex;
    std::condition_variable completion_cv;
    for(size_t i = 1; i < worker_count; ++i) {
        pending_workers.fetch_add(1, std::memory_order_relaxed);
        bool queued = false;
        try {
            queued = depth_decompression_executor().try_submit([&] {
                worker();
                // Publish completion while holding the same mutex used by the
                // waiter. Otherwise notify_one() can land between the
                // predicate check and the wait, leaving a completed job asleep.
                {
                    std::lock_guard<std::mutex> lock(completion_mutex);
                    pending_workers.fetch_sub(1, std::memory_order_release);
                }
                completion_cv.notify_one();
            });
        }
        catch(...) {
            queued = false;
        }
        if(!queued) {
            {
                std::lock_guard<std::mutex> lock(completion_mutex);
                pending_workers.fetch_sub(1, std::memory_order_relaxed);
            }
            break;
        }
    }
    worker();
    {
        std::unique_lock<std::mutex> lock(completion_mutex);
        completion_cv.wait(lock, [&] { return pending_workers.load(std::memory_order_acquire) == 0; });
    }
    if(error) {
        std::rethrow_exception(error);
    }
}

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
    if(sender_id_len == 0 || sender_id_len > kMaxProtocolIdBytes
       || camera_id_len == 0 || camera_id_len > kMaxProtocolIdBytes
       || codec_len == 0 || codec_len > kMaxCodecNameBytes) {
        throw std::runtime_error("media packet string field length is invalid");
    }
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

    validate_media_packet_metadata(packet, max_payload_bytes);

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
    if(sender_id_len == 0 || sender_id_len > kMaxProtocolIdBytes
       || camera_id_len == 0 || camera_id_len > kMaxProtocolIdBytes
       || codec_len == 0 || codec_len > kMaxCodecNameBytes) {
        throw std::runtime_error("UDP media packet string field length is invalid");
    }
    if(payload_size > max_payload_bytes) {
        throw std::runtime_error("UDP media payload too large");
    }
    const size_t text_size = static_cast<size_t>(sender_id_len) + static_cast<size_t>(camera_id_len) + static_cast<size_t>(codec_len);
    const size_t payload_offset = static_cast<size_t>(header_size) + text_size;
    if(payload_offset > size || payload_size != size - payload_offset) {
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
    validate_media_packet_metadata(packet, max_payload_bytes);
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
    if(magic != 0x345a4c50u || version != 1 || chunk_count == 0 || chunk_count > kMaxDepthCompressionChunks || raw_total == 0
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
    uint32_t expected_raw_offset = 0;
    for(uint16_t i = 0; i < chunk_count; ++i) {
        const uint8_t *entry = packet.payload.data() + 16ull + static_cast<size_t>(i) * 12ull;
        Plz4ChunkEntry chunk;
        chunk.raw_offset = read_le32(entry);
        chunk.raw_size = read_le32(entry + 4);
        chunk.compressed_size = read_le32(entry + 8);
        chunk.compressed_offset = static_cast<uint32_t>(compressed_offset);
        if(chunk.raw_offset != expected_raw_offset || chunk.raw_size == 0 || chunk.raw_offset > raw_total
           || chunk.raw_size > raw_total - chunk.raw_offset
           || chunk.compressed_size == 0 || compressed_offset > packet.payload.size()
           || chunk.compressed_size > packet.payload.size() - compressed_offset) {
            throw std::runtime_error("invalid plz4 depth chunk");
        }
        expected_raw_offset += chunk.raw_size;
        compressed_offset += chunk.compressed_size;
        chunks.push_back(chunk);
    }
    if(expected_raw_offset != raw_total || compressed_offset != packet.payload.size()) {
        throw std::runtime_error("plz4 depth payload has trailing bytes");
    }

    std::vector<uint8_t> out(raw_total);
    auto &api = lz4_api();
    bounded_parallel_for(chunks.size(), [&](size_t index) {
            const auto &chunk = chunks[index];
            const int rc = api.decompress_safe(
                reinterpret_cast<const char *>(packet.payload.data() + chunk.compressed_offset),
                reinterpret_cast<char *>(out.data() + chunk.raw_offset),
                static_cast<int>(chunk.compressed_size),
                static_cast<int>(chunk.raw_size));
            if(rc != static_cast<int>(chunk.raw_size)) {
                throw std::runtime_error("plz4 depth chunk decompression failed");
            }
        });
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
    if(magic != 0x424c5a50u || version != 1 || chunk_count == 0 || chunk_count > kMaxDepthCompressionChunks || raw_total == 0
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
    uint32_t expected_raw_offset = 0;
    for(uint16_t i = 0; i < chunk_count; ++i) {
        const uint8_t *entry = packet.payload.data() + 16ull + static_cast<size_t>(i) * 12ull;
        Plz4ChunkEntry chunk;
        chunk.raw_offset = read_le32(entry);
        chunk.raw_size = read_le32(entry + 4);
        chunk.compressed_size = read_le32(entry + 8);
        chunk.compressed_offset = static_cast<uint32_t>(compressed_offset);
        if(chunk.raw_offset != expected_raw_offset || chunk.raw_size == 0 || chunk.raw_offset > raw_total
           || chunk.raw_size > raw_total - chunk.raw_offset
           || chunk.compressed_size == 0 || compressed_offset > packet.payload.size()
           || chunk.compressed_size > packet.payload.size() - compressed_offset
           || chunk.raw_size > static_cast<uint32_t>(std::numeric_limits<uLongf>::max())) {
            throw std::runtime_error("invalid pzlib depth chunk");
        }
        expected_raw_offset += chunk.raw_size;
        compressed_offset += chunk.compressed_size;
        chunks.push_back(chunk);
    }
    if(expected_raw_offset != raw_total || compressed_offset != packet.payload.size()) {
        throw std::runtime_error("pzlib depth payload has trailing bytes");
    }

    std::vector<uint8_t> out(raw_total);
    bounded_parallel_for(chunks.size(), [&](size_t index) {
            const auto &chunk = chunks[index];
            uLongf out_size = static_cast<uLongf>(chunk.raw_size);
            const int rc =
                uncompress(out.data() + chunk.raw_offset, &out_size, packet.payload.data() + chunk.compressed_offset,
                           static_cast<uLong>(chunk.compressed_size));
            if(rc != Z_OK || out_size != chunk.raw_size) {
                throw std::runtime_error("pzlib depth chunk decompression failed");
            }
        });
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
    if(magic != 0x5a323150u || version != 1 || raw_step == 0 || sample_count != expected_sample_count || chunk_count == 0
       || chunk_count > kMaxDepthCompressionChunks) {
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
    bounded_parallel_for(chunks.size(), [&](size_t index) {
            const auto &chunk = chunks[index];
            std::vector<uint8_t> packed(packed_depth12_size(chunk.sample_count));
            uLongf out_size = static_cast<uLongf>(packed.size());
            const int rc =
                uncompress(packed.data(), &out_size, packet.payload.data() + chunk.compressed_offset,
                           static_cast<uLong>(chunk.compressed_size));
            if(rc != Z_OK || out_size != packed.size()) {
                throw std::runtime_error("pq12zlib depth chunk decompression failed");
            }
            unpack_depth12_into(packed, raw_step, out, chunk.sample_offset, chunk.sample_count);
        });
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
    if(magic != 0x5a385150u || version != 1 || raw_step == 0 || sample_count != expected_sample_count || chunk_count == 0
       || chunk_count > kMaxDepthCompressionChunks) {
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
    bounded_parallel_for(chunks.size(), [&](size_t index) {
            const auto &chunk = chunks[index];
            uLongf out_size = static_cast<uLongf>(chunk.sample_count);
            const int rc =
                uncompress(quantized.data() + chunk.sample_offset, &out_size, packet.payload.data() + chunk.compressed_offset,
                           static_cast<uLong>(chunk.compressed_size));
            if(rc != Z_OK || out_size != chunk.sample_count) {
                throw std::runtime_error("pq8zlib depth chunk decompression failed");
            }
        });

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
    if(magic != 0x4c385150u || version != 1 || raw_step == 0 || sample_count != expected_sample_count || chunk_count == 0
       || chunk_count > kMaxDepthCompressionChunks) {
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
    bounded_parallel_for(chunks.size(), [&](size_t index) {
            const auto &chunk = chunks[index];
            const int decoded_size =
                api.decompress_safe(reinterpret_cast<const char *>(packet.payload.data() + chunk.compressed_offset),
                                    reinterpret_cast<char *>(quantized.data() + chunk.sample_offset),
                                    static_cast<int>(chunk.compressed_size),
                                    static_cast<int>(chunk.sample_count));
            if(decoded_size != static_cast<int>(chunk.sample_count)) {
                throw std::runtime_error("pq8lz4 depth chunk decompression failed");
            }
        });

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
            const int64_t delta = (zigzag & 1u) != 0u ? -static_cast<int64_t>((static_cast<uint64_t>(zigzag) + 1u) >> 1u)
                                                      : static_cast<int64_t>(zigzag >> 1u);
            const int64_t current = static_cast<int64_t>(previous) + delta;
            if(current < 0 || current > static_cast<int64_t>(std::numeric_limits<uint16_t>::max())) {
                throw std::runtime_error("invalid rvl depth sample");
            }
            write_depth_u16le(out, index, static_cast<uint16_t>(current));
            previous = static_cast<int32_t>(current);
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

int64_t zigzag_decode_i64(uint32_t value) {
    return static_cast<int64_t>(value >> 1u) ^ -static_cast<int64_t>(value & 1u);
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
    int64_t previous = 0;
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
            const int64_t delta = zigzag_decode_i64(read_varuint_checked(packet.payload, offset));
            const int64_t quantized = previous + delta;
            if(quantized < 0 || quantized > static_cast<int64_t>(std::numeric_limits<uint32_t>::max())) {
                throw std::runtime_error("invalid qdelta depth sample");
            }
            const uint64_t scaled = static_cast<uint64_t>(quantized) * raw_step;
            const uint32_t raw_value = static_cast<uint32_t>(std::min<uint64_t>(scaled, std::numeric_limits<uint16_t>::max()));
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

int wait_child(pid_t pid) {
    int status = 0;
    while(waitpid(pid, &status, 0) < 0) {
        if(errno == EINTR) {
            continue;
        }
        return -1;
    }
    return status;
}

int add_spawn_closefrom(posix_spawn_file_actions_t *actions) {
#if defined(__GLIBC__)
    return posix_spawn_file_actions_addclosefrom_np(actions, 3);
#else
    const long limit = std::min<long>(sysconf(_SC_OPEN_MAX), 65536);
    for(int fd = 3; fd < limit; ++fd) {
        const int rc = posix_spawn_file_actions_addclose(actions, fd);
        if(rc != 0) {
            return rc;
        }
    }
    return 0;
#endif
}

pid_t spawn_shell_process(const std::string &command, int child_stdin, int child_stdout, int child_stderr, int &error_code) {
    posix_spawn_file_actions_t actions;
    error_code = posix_spawn_file_actions_init(&actions);
    if(error_code != 0) {
        return -1;
    }
    ScopeExit destroy_actions([&actions] { posix_spawn_file_actions_destroy(&actions); });
    const auto add_dup = [&](int source, int destination) {
        if(source < 0 || source == destination) {
            return 0;
        }
        return posix_spawn_file_actions_adddup2(&actions, source, destination);
    };
    if((error_code = add_dup(child_stdin, STDIN_FILENO)) != 0
       || (error_code = add_dup(child_stdout, STDOUT_FILENO)) != 0
       || (error_code = add_dup(child_stderr, STDERR_FILENO)) != 0
       || (error_code = add_spawn_closefrom(&actions)) != 0) {
        return -1;
    }
    const char *argv[] = {"/bin/sh", "-c", command.c_str(), nullptr};
    pid_t pid = -1;
    error_code = posix_spawn(&pid, argv[0], &actions, nullptr, const_cast<char *const *>(argv), environ);
    return error_code == 0 ? pid : -1;
}

int run_shell_command(const std::string &command) {
    int error_code = 0;
    const pid_t pid = spawn_shell_process(command, -1, -1, -1, error_code);
    if(pid < 0) {
        errno = error_code;
        return -1;
    }
    return wait_child(pid);
}

std::optional<std::string> run_shell_capture(const std::string &command) {
    int output_pipe[2]{-1, -1};
    if(pipe2(output_pipe, O_CLOEXEC) != 0) {
        return std::nullopt;
    }
    ScopeExit close_pipe([&] {
        if(output_pipe[0] >= 0) {
            close(output_pipe[0]);
        }
        if(output_pipe[1] >= 0) {
            close(output_pipe[1]);
        }
    });
    const int dev_null = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if(dev_null < 0) {
        return std::nullopt;
    }
    ScopeExit close_dev_null([&] { close(dev_null); });
    int error_code = 0;
    const pid_t pid = spawn_shell_process(command, -1, output_pipe[1], dev_null, error_code);
    if(pid < 0) {
        return std::nullopt;
    }
    close(output_pipe[1]);
    output_pipe[1] = -1;
    std::string output;
    char buffer[512];
    for(;;) {
        const ssize_t count = read(output_pipe[0], buffer, sizeof(buffer));
        if(count > 0) {
            output.append(buffer, static_cast<size_t>(count));
            continue;
        }
        if(count < 0 && errno == EINTR) {
            continue;
        }
        if(count < 0) {
            (void)wait_child(pid);
            return std::nullopt;
        }
        break;
    }
    return wait_child(pid) == 0 ? std::optional<std::string>(std::move(output)) : std::nullopt;
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
        int input_pipe[2]{-1, -1};
        if(pipe2(input_pipe, O_CLOEXEC) != 0) {
            logger.error("failed to create ffmpeg input pipe: " + std::string(std::strerror(errno)));
            return false;
        }
        int error_code = 0;
        child_pid_ = spawn_shell_process(command, input_pipe[0], -1, -1, error_code);
        ::close(input_pipe[0]);
        if(child_pid_ < 0) {
            ::close(input_pipe[1]);
            logger.error("failed to start ffmpeg pipe: " + command + ": " + std::strerror(error_code));
            return false;
        }
        pipe_ = fdopen(input_pipe[1], "w");
        if(!pipe_) {
            ::close(input_pipe[1]);
            (void)wait_child(child_pid_);
            child_pid_ = -1;
            logger.error("failed to open ffmpeg input stream: " + std::string(std::strerror(errno)));
            return false;
        }
        if(setvbuf(pipe_, nullptr, _IONBF, 0) != 0) {
            logger.error("failed to configure unbuffered ffmpeg pipe: " + std::string(std::strerror(errno)));
            close();
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
        if(!pipe_ && child_pid_ < 0) {
            return 0;
        }
        if(pipe_) {
            fclose(pipe_);
            pipe_ = nullptr;
        }
        const pid_t pid = child_pid_;
        child_pid_ = -1;
        return pid >= 0 ? wait_child(pid) : 0;
    }

    bool active() const {
        return pipe_ != nullptr;
    }

private:
    FILE *pipe_ = nullptr;
    pid_t child_pid_ = -1;
};

std::string process_status_text(int status) {
    if(status == 0) {
        return "exit=0";
    }
    if(status == -1) {
        return std::string("waitpid failed: ") + std::strerror(errno);
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
    uint64_t pair_id = 0;
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
    std::shared_ptr<const MediaPacket> packet;
    std::string sender_id;
    std::string camera_id;
    std::string camera_name;
    std::string storage_key;
    std::string file_prefix;
    std::string announce_json;
    uint64_t record_generation = 0;
    uint64_t media_session_id = 0;
    std::string media_ingress_key;
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
        constexpr uint64_t kMaxPlausibleFrameGapUs = 500'000;
        if(local_us == 0) {
            return;
        }
        if(last_us > 0 && (local_us <= last_us || local_us - last_us > kMaxPlausibleFrameGapUs)) {
            first_us = local_us;
            last_us = local_us;
            frames = 1;
            return;
        }
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

    void mark_end_us(uint64_t end_us) {
        if(end_us_ == 0) {
            end_us_ = end_us;
        }
    }

    void start(const Config &cfg, const std::string &sender_id, const std::string &camera_id, const std::string &camera_name,
               const std::string &storage_key, const std::string &file_prefix, const std::string &announce_json, Logger &logger) {
        close(cfg, sender_id, camera_id, announce_json, logger);
        ScopeExit rollback_guard([this] { reset_after_close(); });
        start_us_ = now_us();
        end_us_ = 0;
        start_steady_ = std::chrono::steady_clock::now();
        camera_name_ = camera_name;
        storage_key_ = storage_key.empty() ? camera_key(sender_id, camera_id) : storage_key;
        file_prefix_ = file_prefix;
        rgb_h264_full_range_ = rgb_h264_full_range_for_camera(cfg, sender_id, camera_id);
        const std::string &recording_root = cfg.recording_staging.enabled ? cfg.recording_staging.root : cfg.nas_root;
        std::error_code root_ec;
        std::filesystem::create_directories(recording_root, root_ec);
        if(root_ec) {
            throw std::runtime_error("cannot create recording root: " + recording_root + ": " + root_ec.message());
        }
        const auto space = std::filesystem::space(recording_root, root_ec);
        if(root_ec || space.available < cfg.min_free_disk_bytes) {
            throw std::runtime_error("insufficient free space under recording root: " + recording_root);
        }
        const auto directory_base =
            std::filesystem::path(recording_root) / storage_key_ / date_dir_from_us(start_us_) / time_dir_from_us(start_us_);
        auto directory = directory_base;
        std::error_code ec;
        for(unsigned suffix = 1; std::filesystem::exists(directory, ec); ++suffix) {
            if(ec) {
                throw std::runtime_error("cannot inspect recording directory: " + directory.string() + ": " + ec.message());
            }
            std::ostringstream name;
            name << directory_base.filename().string() << '_' << std::setw(3) << std::setfill('0') << suffix;
            directory = directory_base.parent_path() / name.str();
        }
        if(!std::filesystem::create_directories(directory, ec) || ec) {
            throw std::runtime_error("cannot create recording directory: " + directory.string() + ": " + ec.message());
        }
        directory_ = directory.string();
        recording_root_ = std::filesystem::path(recording_root).lexically_normal().string();
        relative_directory_ = directory.lexically_relative(std::filesystem::path(recording_root)).generic_string();
        if(relative_directory_.empty() || relative_directory_ == ".." || relative_directory_.rfind("../", 0) == 0) {
            throw std::runtime_error("recording directory escaped configured root: " + directory_);
        }

        // Publish frames.csv only after media finalization and RGB index merging complete.
        // Consumers must never mistake the live packet journal for the final MP4 frame map.
        frames_csv_.open(file_path("frames.csv.inprogress"), std::ios::out | std::ios::trunc);
        if(!frames_csv_) {
            throw std::runtime_error("cannot open frames.csv staging file: " + file_path("frames.csv.inprogress").string());
        }
        frames_csv_ << "local_time_us,stream_type,rgb_frame_id,rgb_timestamp_us,depth_frame_id,depth_timestamp_us,pair_id,pair_delta_ms,width,height,payload_size,"
                       "packet_system_timestamp_us,rgb_system_timestamp_us,depth_system_timestamp_us,frame_id,timestamp_us,frame_system_timestamp_us,"
                       "rgb_exposure_us,rgb_gain,rgb_auto_exposure,rgb_actual_fps,rgb_frame_interval_us,codec_or_compression,"
                       "sender_capture_host_timestamp_us,sender_timing_bound_timestamp_us,sender_encode_start_timestamp_us,"
                       "sender_encode_done_timestamp_us,sender_packet_queued_timestamp_us,receiver_minus_frame_system_us,"
                       "sender_capture_to_timing_bound_us,sender_timing_bound_to_encode_start_us,sender_encode_duration_us,"
                       "sender_encode_done_to_packet_queued_us,sender_packet_queued_to_receiver_us,"
                       "sender_id,camera_id,sender_timestamp_us,sender_system_timestamp_us,receiver_receive_timestamp_us,"
                       "clock_sync_valid,sender_offset_us,sender_delay_us,sender_drift_ppm,global_timestamp_us,"
                       "rgb_depth_pair_valid,pair_delta_us,pair_delta_source,pair_id_valid\n";
        rgb_recorded_frames_csv_.open(file_path("rgb_recorded_frames.csv"), std::ios::out | std::ios::trunc);
        if(!rgb_recorded_frames_csv_) {
            throw std::runtime_error("cannot open RGB frame index CSV: " + file_path("rgb_recorded_frames.csv").string());
        }
        rgb_recorded_frames_csv_
            << "video_frame_index,local_time_us,frame_id,timestamp_us,frame_system_timestamp_us,width,height,payload_size,"
               "packet_system_timestamp_us,rgb_exposure_us,rgb_gain,rgb_auto_exposure,rgb_actual_fps,codec_or_compression,"
               "sender_capture_host_timestamp_us,sender_timing_bound_timestamp_us,sender_encode_start_timestamp_us,"
               "sender_encode_done_timestamp_us,sender_packet_queued_timestamp_us,receiver_minus_frame_system_us,"
               "sender_capture_to_timing_bound_us,sender_timing_bound_to_encode_start_us,sender_encode_duration_us,"
               "sender_encode_done_to_packet_queued_us,sender_packet_queued_to_receiver_us,"
               "sender_id,camera_id,sender_timestamp_us,sender_system_timestamp_us,receiver_receive_timestamp_us,"
               "clock_sync_valid,sender_offset_us,sender_delay_us,sender_drift_ppm,global_timestamp_us\n";

        if(cfg.write_debug_h264) {
            rgb_debug_path_ = file_path("rgb_debug.h264");
            rgb_debug_.open(rgb_debug_path_, std::ios::binary | std::ios::out | std::ios::trunc);
            if(!rgb_debug_) {
                throw std::runtime_error("cannot open RGB H264 debug file: " + rgb_debug_path_.string());
            }
        }
        if(cfg.write_debug_depth_raw) {
            depth_debug_.open(file_path("depth_debug.raw"), std::ios::binary | std::ios::out | std::ios::trunc);
            if(!depth_debug_) {
                throw std::runtime_error("cannot open depth debug file: " + file_path("depth_debug.raw").string());
            }
        }

        write_meta(cfg, sender_id, camera_id, announce_json, false);
        active_ = true;
        rgb_pipe_failed_ = false;
        depth_pipe_failed_ = false;
        depth_part_index_ = 0;
        depth_part_paths_.clear();
        csv_rows_since_flush_ = 0;
        storage_check_packets_ = 0;
        storage_failed_ = false;
        rollback_guard.release();
        logger.info("recording segment started: " + directory_);
    }

    void close(const Config &cfg, const std::string &sender_id, const std::string &camera_id, const std::string &announce_json, Logger &logger) {
        if(!active_) {
            return;
        }
        ScopeExit reset_guard([this] { reset_after_close(); });
        std::exception_ptr flush_error;
        try {
            flush_pending_media(cfg, logger);
        }
        catch(...) {
            flush_error = std::current_exception();
            logger.warn("recording pending media flush failed; continuing container finalization: " + directory_);
        }
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
        mark_end_us(now_us());
        if(rgb_rc != 0) {
            rgb_pipe_failed_ = true;
            logger.warn("rgb ffmpeg exited with non-zero status (" + process_status_text(rgb_rc) + ") for segment: " + directory_);
        }
        if(depth_rc != 0) {
            depth_pipe_failed_ = true;
            logger.warn("depth ffmpeg exited with non-zero status (" + process_status_text(depth_rc) + ") for segment: " + directory_);
        }
        finalize_completed_media(cfg, logger);
        publish_finalized_frames(logger);
        write_meta(cfg, sender_id, camera_id, announce_json, true);
        if(cfg.recording_staging.enabled && cfg.recording_staging.defer_player_compatible_finalize) {
            write_recording_staged_marker(sender_id, camera_id);
        }
        else {
            write_recording_ready_marker(sender_id, camera_id);
        }
        set_segment_mtime_to_start(logger);
        logger.info("recording segment closed: " + directory_);
        if(flush_error) {
            std::rethrow_exception(flush_error);
        }
    }

    void reset_after_close() {
        if(frames_csv_) {
            frames_csv_.close();
        }
        if(rgb_recorded_frames_csv_) {
            rgb_recorded_frames_csv_.close();
        }
        if(rgb_debug_) {
            rgb_debug_.close();
        }
        if(depth_debug_) {
            depth_debug_.close();
        }
        rgb_pipe_.close();
        depth_pipe_.close();
        active_ = false;
        directory_.clear();
        recording_root_.clear();
        relative_directory_.clear();
        camera_name_.clear();
        storage_key_.clear();
        file_prefix_.clear();
        rgb_h264_full_range_ = false;
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
        rgb_pipe_failed_ = false;
        depth_pipe_failed_ = false;
        depth_part_index_ = 0;
        depth_part_paths_.clear();
        csv_rows_since_flush_ = 0;
        storage_check_packets_ = 0;
        storage_failed_ = false;
    }

    bool should_rotate(const Config &cfg) const {
        return active_ && std::chrono::steady_clock::now() - start_steady_ >= std::chrono::seconds(cfg.segment_seconds);
    }

    bool stream_profile_changed(const MediaPacket &packet) const {
        const StreamRecordStats *stats = nullptr;
        if(packet.stream_type == StreamType::rgb) {
            stats = &rgb_stats_;
        }
        else if(packet.stream_type == StreamType::depth_raw) {
            stats = &depth_stats_;
        }
        return stats && stats->frames > 0
               && (stats->width != packet.width || stats->height != packet.height
                   || stats->codec_or_compression != packet.codec_or_compression);
    }

    void write_packet(const Config &cfg, const MediaPacket &packet, const std::string &sender_id, const std::string &camera_id,
                      const std::string &camera_name, const std::string &storage_key, const std::string &file_prefix,
                      const std::string &announce_json, Logger &logger, bool allow_rotate = true) {
        if(!active_) {
            start(cfg, sender_id, camera_id, camera_name, storage_key, file_prefix, announce_json, logger);
        }
        if(storage_failed_) {
            throw std::runtime_error("recording storage previously failed: " + directory_);
        }
        if(++storage_check_packets_ >= 30) {
            storage_check_packets_ = 0;
            std::error_code ec;
            const auto space = std::filesystem::space(directory_, ec);
            if(ec || space.available < cfg.min_free_disk_bytes) {
                storage_failed_ = true;
                throw std::runtime_error("recording stopped because free space is below the configured reserve: " + directory_);
            }
        }
        if(allow_rotate && (stream_profile_changed(packet) || should_rotate(cfg))) {
            if(stream_profile_changed(packet)) {
                logger.warn("media profile changed; rotating segment camera=" + camera_key(sender_id, camera_id));
            }
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
                last_rgb_ = FrameInfo{true, packet.frame_id, packet.timestamp_us, packet.system_timestamp_us, packet.pair_id,
                                      packet.rgb_exposure_us, packet.rgb_gain, packet.rgb_auto_exposure, packet.rgb_actual_fps};
                last_rgb_frame_interval_us_ = frame_interval_us;
                rgb_stats_.add(packet, packet_local_us);
            }
        }
        else if(packet.stream_type == StreamType::depth_raw) {
            if(depth_debug_) {
                depth_debug_.write(reinterpret_cast<const char *>(packet.payload.data()), static_cast<std::streamsize>(packet.payload.size()));
            }
            write_depth_packet(cfg, packet, packet_local_us, logger);
            last_depth_ = FrameInfo{true, packet.frame_id, packet.timestamp_us, packet.system_timestamp_us, packet.pair_id};
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
            write_pair_quality_column(frames_csv_, last_rgb_, last_depth_);
            frames_csv_ << ',';
            if(last_rgb_.valid && last_depth_.valid) {
                const bool use_system = last_rgb_.system_timestamp_us > 0 && last_depth_.system_timestamp_us > 0;
                const uint64_t rgb_time = use_system ? last_rgb_.system_timestamp_us : last_rgb_.timestamp_us;
                const uint64_t depth_time = use_system ? last_depth_.system_timestamp_us : last_depth_.timestamp_us;
                const int64_t signed_delta = depth_time >= rgb_time ? static_cast<int64_t>(depth_time - rgb_time)
                                                                    : -static_cast<int64_t>(rgb_time - depth_time);
                frames_csv_ << signed_delta << ',' << (use_system ? "system_timestamp_us" : "device_timestamp_us");
            }
            else {
                frames_csv_ << ',';
            }
            const bool pair_id_valid = last_rgb_.valid && last_depth_.valid && last_rgb_.pair_id != 0
                                       && last_rgb_.pair_id == last_depth_.pair_id;
            frames_csv_ << ',' << (pair_id_valid ? 1 : 0);
            frames_csv_ << '\n';
            if(!frames_csv_) {
                throw std::runtime_error("frames.csv staging write failed: " + file_path("frames.csv.inprogress").string());
            }
            if(++csv_rows_since_flush_ >= 30) {
                frames_csv_.flush();
                rgb_recorded_frames_csv_.flush();
                if(!frames_csv_ || !rgb_recorded_frames_csv_) {
                    throw std::runtime_error("recording CSV flush failed: " + directory_);
                }
                csv_rows_since_flush_ = 0;
            }
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

    static void write_pair_quality_column(std::ostream &csv, const FrameInfo &rgb, const FrameInfo &depth) {
        csv << ',';
        if(!rgb.valid || !depth.valid) {
            csv << 0;
            return;
        }
        if(rgb.pair_id != 0 && depth.pair_id != 0 && rgb.pair_id != depth.pair_id) {
            csv << 0;
            return;
        }
        const bool use_system_pair_delta = rgb.system_timestamp_us > 0 && depth.system_timestamp_us > 0;
        const uint64_t rgb_pair_us = use_system_pair_delta ? rgb.system_timestamp_us : rgb.timestamp_us;
        const uint64_t depth_pair_us = use_system_pair_delta ? depth.system_timestamp_us : depth.timestamp_us;
        const uint64_t delta = rgb_pair_us > depth_pair_us ? rgb_pair_us - depth_pair_us : depth_pair_us - rgb_pair_us;
        csv << (delta <= kRgbDepthPairValidMaxDeltaUs ? 1 : 0);
    }

    bool write_rgb_recovery_bytes(const uint8_t *data, size_t size, Logger &logger) {
        if(!rgb_debug_.is_open() || size == 0) {
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
        if(!rgb_recorded_frames_csv_) {
            throw std::runtime_error("RGB frame index CSV write failed: " + file_path("rgb_recorded_frames.csv").string());
        }
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
        const auto frames_path = file_path("frames.csv.inprogress");
        const auto finalized_path = file_path("frames.csv.finalizing");
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

        const auto tmp_path = finalized_path.string() + ".merge_tmp";
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
        std::filesystem::rename(tmp_path, finalized_path, ec);
        if(ec) {
            logger.warn("failed to stage finalized frames.csv with merged RGB frame indexes: " + ec.message());
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

    void publish_finalized_frames(Logger &logger) {
        const auto staging_path = file_path("frames.csv.inprogress");
        const auto finalized_path = file_path("frames.csv.finalizing");
        const auto published_path = file_path("frames.csv");
        std::error_code ec;
        if(!std::filesystem::exists(finalized_path, ec) || ec) {
            throw std::runtime_error("finalized frames.csv is unavailable for publication: " + finalized_path.string());
        }
        std::filesystem::rename(finalized_path, published_path, ec);
        if(ec) {
            throw std::runtime_error("cannot publish finalized frames.csv: " + ec.message());
        }
        std::filesystem::remove(staging_path, ec);
        if(ec) {
            logger.warn("cannot remove frames.csv staging file after publication: " + ec.message());
        }
        logger.info("finalized frames.csv published atomically: " + published_path.string());
    }

    void write_recording_ready_marker(const std::string &sender_id, const std::string &camera_id) const {
        const auto marker_path = file_path("recording_ready.json");
        const auto temporary_path = file_path("recording_ready.json.tmp");
        std::ofstream marker(temporary_path, std::ios::out | std::ios::trunc);
        if(!marker) {
            throw std::runtime_error("cannot create recording ready marker: " + temporary_path.string());
        }
        marker << "{\n";
        marker << "  \"schema\": \"gwv3_recording_ready_v1\",\n";
        marker << "  \"ready\": true,\n";
        marker << "  \"finalized_at_us\": " << now_us() << ",\n";
        marker << "  \"segment_start_us\": " << start_us_ << ",\n";
        marker << "  \"segment_end_us\": " << end_us_ << ",\n";
        marker << "  \"sender_id\": \"" << json_escape(sender_id) << "\",\n";
        marker << "  \"camera_id\": \"" << json_escape(camera_id) << "\",\n";
        marker << "  \"relative_path\": \"" << json_escape(relative_directory_) << "\",\n";
        marker << "  \"frames_file\": \"" << json_escape(prefixed_filename(file_prefix_, "frames.csv")) << "\",\n";
        marker << "  \"meta_file\": \"" << json_escape(prefixed_filename(file_prefix_, "meta.json")) << "\",\n";
        marker << "  \"ready_file\": \"" << json_escape(prefixed_filename(file_prefix_, "recording_ready.json")) << "\",\n";
        marker << "  \"rgb_file\": \"" << json_escape(prefixed_filename(file_prefix_, "rgb.mp4")) << "\",\n";
        marker << "  \"depth_file\": \"" << json_escape(prefixed_filename(file_prefix_, "depth.mkv")) << "\",\n";
        marker << "  \"rgb_frame_index_mode\": \"frames_csv_rgb_recorded_columns\"\n";
        marker << "}\n";
        marker.close();
        if(!marker) {
            throw std::runtime_error("cannot finish recording ready marker: " + temporary_path.string());
        }
        std::error_code ec;
        std::filesystem::rename(temporary_path, marker_path, ec);
        if(ec) {
            throw std::runtime_error("cannot publish recording ready marker: " + ec.message());
        }
    }

    void write_recording_staged_marker(const std::string &sender_id, const std::string &camera_id) const {
        const auto marker_path = file_path("recording_staged.json");
        const auto temporary_path = file_path("recording_staged.json.tmp");
        std::ofstream marker(temporary_path, std::ios::out | std::ios::trunc);
        if(!marker) {
            throw std::runtime_error("cannot create recording staged marker: " + temporary_path.string());
        }
        marker << "{\n";
        marker << "  \"schema\": \"gwv3_recording_staged_v1\",\n";
        marker << "  \"staged\": true,\n";
        marker << "  \"staged_at_us\": " << now_us() << ",\n";
        marker << "  \"segment_start_us\": " << start_us_ << ",\n";
        marker << "  \"segment_end_us\": " << end_us_ << ",\n";
        marker << "  \"sender_id\": \"" << json_escape(sender_id) << "\",\n";
        marker << "  \"camera_id\": \"" << json_escape(camera_id) << "\",\n";
        marker << "  \"relative_path\": \"" << json_escape(relative_directory_) << "\",\n";
        marker << "  \"recording_root\": \"" << json_escape(recording_root_) << "\",\n";
        marker << "  \"frames_file\": \"" << json_escape(prefixed_filename(file_prefix_, "frames.csv")) << "\",\n";
        marker << "  \"meta_file\": \"" << json_escape(prefixed_filename(file_prefix_, "meta.json")) << "\",\n";
        marker << "  \"ready_file\": \"" << json_escape(prefixed_filename(file_prefix_, "recording_ready.json")) << "\",\n";
        marker << "  \"rgb_file\": \"" << json_escape(prefixed_filename(file_prefix_, "rgb.mp4")) << "\",\n";
        marker << "  \"depth_file\": \"" << json_escape(prefixed_filename(file_prefix_, "depth.mkv")) << "\"\n";
        marker << "}\n";
        marker.close();
        if(!marker) {
            throw std::runtime_error("cannot finish recording staged marker: " + temporary_path.string());
        }
        std::error_code ec;
        std::filesystem::rename(temporary_path, marker_path, ec);
        if(ec) {
            throw std::runtime_error("cannot publish recording staged marker: " + ec.message());
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
        std::ostringstream part_name;
        part_name << "depth_part_" << std::setw(3) << std::setfill('0') << depth_part_index_ << ".mkv";
        const auto part_path = file_path(part_name.str());
        const auto depth_mkv = shell_quote(part_path.string());
        std::ostringstream cmd;
        cmd << shell_quote(cfg.ffmpeg_path)
            << " -hide_banner -loglevel warning -y -f rawvideo -pixel_format gray16le -video_size " << width << "x" << height
            << " -framerate " << format_fps(fps) << " -i pipe:0 -c:v ffv1 -level 3 " << depth_mkv << " 2>>" << ffmpeg_log;
        if(depth_pipe_.open(cmd.str(), logger)) {
            if(depth_part_paths_.empty() || depth_part_paths_.back() != part_path) {
                depth_part_paths_.push_back(part_path);
            }
        }
    }

    void ensure_rgb_pipe(const Config &cfg, double fps, Logger &logger) {
        if(rgb_pipe_.active() || rgb_pipe_failed_) {
            return;
        }
        const auto ffmpeg_log = shell_quote(file_path("ffmpeg.log").string());
        const auto rgb_mp4 = shell_quote(file_path("rgb.mp4").string());
        const std::string metadata_bsf = rgb_h264_full_range_
                                             ? " -bsf:v " + shell_quote(kH264FullRangeMetadataBsf)
                                             : "";
        const std::string rgb_cmd = shell_quote(cfg.ffmpeg_path) +
                                    " -hide_banner -loglevel warning -y -fflags +genpts -r " + format_fps(fps) +
                                    " -f h264 -i pipe:0 -c:v copy" + metadata_bsf + " -movflags " + kRgbMp4RecordMuxFlags +
                                    " -flush_packets 1 " + rgb_mp4 +
                                    " 2>>" + ffmpeg_log;
        if(!rgb_pipe_.open(rgb_cmd, logger)) {
            rgb_pipe_failed_ = true;
        }
    }

    void write_rgb_packet(const Config &cfg, const MediaPacket &packet, uint64_t packet_local_us, Logger &logger) {
        if(rgb_pipe_failed_) {
            if(write_rgb_recovery_bytes(packet.payload.data(), packet.payload.size(), logger)) {
                write_rgb_recorded_frame(packet, packet_local_us, packet.payload.size());
            }
            return;
        }
        if(rgb_pipe_.active()) {
            const bool recovery_ok = write_rgb_recovery_bytes(packet.payload.data(), packet.payload.size(), logger);
            const bool pipe_ok = rgb_pipe_.write(packet.payload.data(), packet.payload.size(), logger);
            rgb_pipe_failed_ = !pipe_ok;
            if(pipe_ok || recovery_ok) {
                write_rgb_recorded_frame(packet, packet_local_us, packet.payload.size());
            }
            return;
        }

        if(rgb_pending_has_decodable_start_ && rgb_pending_.size() + packet.payload.size() > kMaxPendingRgbRecordBytes) {
            flush_rgb_pending(cfg, logger);
        }
        if(rgb_pipe_failed_) {
            if(write_rgb_recovery_bytes(packet.payload.data(), packet.payload.size(), logger)) {
                write_rgb_recorded_frame(packet, packet_local_us, packet.payload.size());
            }
            return;
        }
        if(rgb_pipe_.active()) {
            const bool recovery_ok = write_rgb_recovery_bytes(packet.payload.data(), packet.payload.size(), logger);
            const bool pipe_ok = rgb_pipe_.write(packet.payload.data(), packet.payload.size(), logger);
            rgb_pipe_failed_ = !pipe_ok;
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
            if(!depth_pipe_.write(packet.payload.data(), packet.payload.size(), logger)) {
                depth_pipe_failed_ = true;
                ++depth_part_index_;
                ensure_depth_pipe(cfg, packet.width, packet.height, depth_record_fps_ > 0.0 ? depth_record_fps_ : cfg.depth_fps, logger);
                if(!depth_pipe_.active() || !depth_pipe_.write(packet.payload.data(), packet.payload.size(), logger)) {
                    throw std::runtime_error("depth ffmpeg recovery part write failed");
                }
            }
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
            if(!depth_pipe_.write(packet.payload.data(), packet.payload.size(), logger)) {
                depth_pipe_failed_ = true;
                ++depth_part_index_;
                ensure_depth_pipe(cfg, packet.width, packet.height, depth_record_fps_ > 0.0 ? depth_record_fps_ : cfg.depth_fps, logger);
                if(!depth_pipe_.active() || !depth_pipe_.write(packet.payload.data(), packet.payload.size(), logger)) {
                    throw std::runtime_error("depth ffmpeg recovery part write failed");
                }
            }
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
        if(rgb_pipe_failed_) {
            if(write_rgb_recovery_bytes(rgb_pending_.data(), rgb_pending_.size(), logger)) {
                write_pending_rgb_recorded_frames();
            }
            rgb_pending_.clear();
            rgb_pending_infos_.clear();
            return;
        }
        if(rgb_pipe_.active()) {
            logger.info("rgb record fps estimated: " + format_fps(rgb_record_fps_));
            const bool recovery_ok = write_rgb_recovery_bytes(rgb_pending_.data(), rgb_pending_.size(), logger);
            const bool pipe_ok = rgb_pipe_.write(rgb_pending_.data(), rgb_pending_.size(), logger);
            rgb_pipe_failed_ = !pipe_ok;
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
                if(!depth_pipe_.write(payload.data(), payload.size(), logger)) {
                    depth_pipe_failed_ = true;
                    ++depth_part_index_;
                    ensure_depth_pipe(cfg, depth_width_, depth_height_, depth_record_fps_, logger);
                    if(!depth_pipe_.active() || !depth_pipe_.write(payload.data(), payload.size(), logger)) {
                        throw std::runtime_error("depth ffmpeg pending recovery part write failed");
                    }
                }
            }
            depth_pending_.clear();
            depth_pending_bytes_ = 0;
        }
    }

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
        const auto output = run_shell_capture(cmd);
        if(!output) {
            return std::nullopt;
        }
        try {
            const double duration = std::stod(trim_copy(*output));
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

    struct Mp4TopLevelAtoms {
        bool moov = false;
        bool moof = false;
        bool sidx = false;
        bool mfra = false;
    };

    static uint32_t read_be_u32(const unsigned char *data) {
        return (static_cast<uint32_t>(data[0]) << 24u) | (static_cast<uint32_t>(data[1]) << 16u)
               | (static_cast<uint32_t>(data[2]) << 8u) | static_cast<uint32_t>(data[3]);
    }

    static uint64_t read_be_u64(const unsigned char *data) {
        uint64_t value = 0;
        for(size_t i = 0; i < 8; ++i) {
            value = (value << 8u) | static_cast<uint64_t>(data[i]);
        }
        return value;
    }

    static std::optional<Mp4TopLevelAtoms> inspect_mp4_top_level_atoms(const std::filesystem::path &path) {
        std::error_code ec;
        const auto file_size_value = std::filesystem::file_size(path, ec);
        if(ec || file_size_value < 8 || file_size_value > std::numeric_limits<uint64_t>::max()) {
            return std::nullopt;
        }
        const uint64_t file_size = static_cast<uint64_t>(file_size_value);
        std::ifstream input(path, std::ios::binary);
        if(!input) {
            return std::nullopt;
        }

        Mp4TopLevelAtoms atoms;
        uint64_t offset = 0;
        while(offset + 8 <= file_size) {
            if(offset > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max())) {
                return std::nullopt;
            }
            input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
            unsigned char header[16]{};
            input.read(reinterpret_cast<char *>(header), 8);
            if(input.gcount() != 8) {
                return std::nullopt;
            }

            uint64_t atom_size = read_be_u32(header);
            uint64_t header_size = 8;
            if(atom_size == 1) {
                input.read(reinterpret_cast<char *>(header + 8), 8);
                if(input.gcount() != 8) {
                    return std::nullopt;
                }
                atom_size = read_be_u64(header + 8);
                header_size = 16;
            }
            else if(atom_size == 0) {
                atom_size = file_size - offset;
            }
            if(atom_size < header_size || atom_size > file_size - offset) {
                return std::nullopt;
            }

            const std::string atom_type(reinterpret_cast<const char *>(header + 4), 4);
            atoms.moov = atoms.moov || atom_type == "moov";
            atoms.moof = atoms.moof || atom_type == "moof";
            atoms.sidx = atoms.sidx || atom_type == "sidx";
            atoms.mfra = atoms.mfra || atom_type == "mfra";
            offset += atom_size;
            if(offset == file_size) {
                break;
            }
        }
        if(offset != file_size) {
            return std::nullopt;
        }
        return atoms;
    }

    static bool media_file_readable(const std::string &ffprobe_path, const std::filesystem::path &media_path) {
        if(!file_size_nonzero(media_path)) {
            return false;
        }
        if(media_path.extension().string() == ".mp4") {
            const auto atoms = inspect_mp4_top_level_atoms(media_path);
            if(!atoms || !atoms->moov) {
                return false;
            }
            if(atoms->moof && (!atoms->sidx || !atoms->mfra)) {
                return false;
            }
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

    static std::string ffconcat_quote(const std::string &value) {
        std::string out;
        out.reserve(value.size() + 8);
        for(char ch : value) {
            if(ch == '\\' || ch == '\'') {
                out.push_back('\\');
            }
            out.push_back(ch);
        }
        return out;
    }

    bool replace_with_valid_media(const std::filesystem::path &temporary,
                                  const std::filesystem::path &destination,
                                  const std::string &ffprobe_path,
                                  Logger &logger) const {
        if(!media_file_readable(ffprobe_path, temporary)) {
            logger.warn("recovered media validation failed: " + temporary.string());
            return false;
        }
        std::error_code ec;
        std::filesystem::rename(temporary, destination, ec);
        if(ec) {
            logger.warn("cannot replace recovered media " + destination.string() + ": " + ec.message());
            return false;
        }
        return true;
    }

    bool finalize_rgb_player_compatible(const Config &cfg, const std::string &ffprobe_path, Logger &logger) const {
        const auto destination = file_path("rgb.mp4");
        const auto source_atoms = inspect_mp4_top_level_atoms(destination);
        if(!source_atoms || !source_atoms->moov) {
            return false;
        }
        if(!source_atoms->moof) {
            return true;
        }

        const auto source_duration = probe_media_duration_seconds(ffprobe_path, destination);
        const auto temporary = file_path("rgb_seekable.tmp.mp4");
        const auto log_path = file_path("ffmpeg.log");
        std::error_code ec;
        std::filesystem::remove(temporary, ec);

        // Keep the crash-tolerant fragmented MP4 in place until a conventional
        // MP4 has been completely written and validated in the same directory.
        const std::string command = shell_quote(cfg.ffmpeg_path)
                                    + " -hide_banner -loglevel warning -y -i " + shell_quote(destination.string())
                                    + " -map 0:v:0 -c:v copy " + shell_quote(temporary.string())
                                    + " 2>>" + shell_quote(log_path.string());
        const int rc = run_shell_command(command);
        if(rc != 0) {
            append_retime_log(log_path, "rgb player-compatible remux failed: " + process_status_text(rc));
            std::filesystem::remove(temporary, ec);
            return false;
        }

        const auto temporary_atoms = inspect_mp4_top_level_atoms(temporary);
        const auto temporary_duration = probe_media_duration_seconds(ffprobe_path, temporary);
        bool valid = temporary_atoms && temporary_atoms->moov && !temporary_atoms->moof && temporary_duration.has_value();
        if(valid && source_duration) {
            const double tolerance = std::max(0.25, *source_duration * 0.001);
            valid = std::fabs(*temporary_duration - *source_duration) <= tolerance;
        }
        if(!valid) {
            append_retime_log(log_path, "rgb player-compatible remux validation failed");
            std::filesystem::remove(temporary, ec);
            return false;
        }

        std::filesystem::rename(temporary, destination, ec);
        if(ec) {
            append_retime_log(log_path, "rgb player-compatible publish failed: " + ec.message());
            std::filesystem::remove(temporary, ec);
            return false;
        }
        append_retime_log(log_path, "rgb player-compatible MP4 published atomically");
        logger.info("RGB recording finalized as conventional seekable MP4: " + destination.string());
        return true;
    }

    bool rebuild_rgb_from_recovery(const Config &cfg, const std::string &ffprobe_path, Logger &logger) const {
        if(!file_size_nonzero(rgb_debug_path_)) {
            return false;
        }
        const auto temporary = file_path("rgb_recovered.tmp.mp4");
        std::error_code ec;
        std::filesystem::remove(temporary, ec);
        const double fps = rgb_record_fps_ > 0.0 ? rgb_record_fps_ : 30.0;
        const std::string metadata_bsf = rgb_h264_full_range_
                                             ? " -bsf:v " + shell_quote(kH264FullRangeMetadataBsf)
                                             : "";
        const std::string command = shell_quote(cfg.ffmpeg_path)
                                    + " -hide_banner -loglevel warning -y -fflags +genpts -r " + format_fps(fps)
                                    + " -f h264 -i " + shell_quote(rgb_debug_path_.string())
                                    + " -c:v copy" + metadata_bsf + " -movflags " + kRgbMp4RecordMuxFlags + " -flush_packets 1 "
                                    + shell_quote(temporary.string()) + " 2>>" + shell_quote(file_path("ffmpeg.log").string());
        if(run_shell_command(command) != 0) {
            logger.warn("RGB automatic recovery remux failed: " + directory_);
            std::filesystem::remove(temporary, ec);
            return false;
        }
        if(!replace_with_valid_media(temporary, file_path("rgb.mp4"), ffprobe_path, logger)) {
            std::filesystem::remove(temporary, ec);
            return false;
        }
        logger.info("RGB recording rebuilt from recovery stream: " + file_path("rgb.mp4").string());
        return true;
    }

    bool finalize_depth_parts(const Config &cfg, const std::string &ffprobe_path, Logger &logger) const {
        std::vector<std::filesystem::path> parts;
        for(const auto &path : depth_part_paths_) {
            if(file_size_nonzero(path) && media_file_readable(ffprobe_path, path)) {
                parts.push_back(path);
            }
            else if(file_size_nonzero(path)) {
                logger.warn("invalid depth part retained for offline inspection: " + path.string());
            }
        }
        if(parts.empty()) {
            return false;
        }

        const auto destination = file_path("depth.mkv");
        std::error_code ec;
        if(parts.size() == 1) {
            std::filesystem::rename(parts.front(), destination, ec);
            if(ec) {
                logger.warn("cannot finalize depth recording: " + ec.message());
                return false;
            }
            return true;
        }

        const auto list_path = file_path("depth_parts.concat.txt");
        const auto temporary = file_path("depth_recovered.tmp.mkv");
        std::ofstream list(list_path, std::ios::out | std::ios::trunc);
        if(!list) {
            logger.warn("cannot create depth concat list: " + list_path.string());
            return false;
        }
        for(const auto &part : parts) {
            list << "file '" << ffconcat_quote(part.string()) << "'\n";
        }
        list.close();
        if(!list) {
            logger.warn("cannot finish depth concat list: " + list_path.string());
            return false;
        }
        std::filesystem::remove(temporary, ec);
        const std::string command = shell_quote(cfg.ffmpeg_path)
                                    + " -hide_banner -loglevel warning -y -f concat -safe 0 -i " + shell_quote(list_path.string())
                                    + " -c copy " + shell_quote(temporary.string()) + " 2>>" + shell_quote(file_path("ffmpeg.log").string());
        const int rc = run_shell_command(command);
        std::filesystem::remove(list_path, ec);
        if(rc != 0 || !replace_with_valid_media(temporary, destination, ffprobe_path, logger)) {
            std::filesystem::remove(temporary, ec);
            logger.warn("depth part concatenation failed; part files retained: " + directory_);
            return false;
        }
        for(const auto &part : parts) {
            std::filesystem::remove(part, ec);
        }
        logger.info("depth recording finalized from " + std::to_string(parts.size()) + " parts: " + destination.string());
        return true;
    }

    void finalize_completed_media(const Config &cfg, Logger &logger) const {
        const auto ffprobe_path = ffprobe_path_from_ffmpeg(cfg.ffmpeg_path);
        const auto log_path = file_path("ffmpeg.log");
        bool checked_any_media = false;

        const auto validate_media = [&](const std::filesystem::path &path, const std::string &stream_name) -> bool {
            if(!file_size_nonzero(path)) {
                append_retime_log(log_path, stream_name + " validation skipped: media file missing or empty");
                return false;
            }
            checked_any_media = true;
            const bool ok = media_file_readable(ffprobe_path, path);
            append_retime_log(log_path, stream_name + (ok ? " final validation ok" : " final validation failed"));
            set_file_mtime_to_start(path, start_us_);
            return ok;
        };

        const bool defer_player_compatible = cfg.recording_staging.enabled
                                             && cfg.recording_staging.defer_player_compatible_finalize;
        bool rgb_ok = validate_media(file_path("rgb.mp4"), "rgb");
        if((rgb_pipe_failed_ || !rgb_ok) && rebuild_rgb_from_recovery(cfg, ffprobe_path, logger)) {
            rgb_ok = validate_media(file_path("rgb.mp4"), "rgb_recovered");
        }
        if(rgb_ok && !defer_player_compatible) {
            rgb_ok = finalize_rgb_player_compatible(cfg, ffprobe_path, logger);
            if(rgb_ok) {
                rgb_ok = validate_media(file_path("rgb.mp4"), "rgb_compatible");
            }
        }
        else if(rgb_ok && defer_player_compatible) {
            append_retime_log(log_path, "rgb player-compatible remux deferred to recording uploader");
        }
        if(rgb_ok && !cfg.write_debug_h264 && !rgb_debug_path_.empty()) {
            std::error_code ec;
            if(std::filesystem::exists(rgb_debug_path_, ec)) {
                std::filesystem::remove(rgb_debug_path_, ec);
                if(ec) {
                    append_retime_log(log_path, "rgb recovery h264 remove failed: " + ec.message());
                }
                else {
                    append_retime_log(log_path, "rgb recovery h264 removed after final validation");
                }
            }
        }
        else if(!rgb_ok && !rgb_debug_path_.empty()) {
            append_retime_log(log_path, "rgb recovery h264 kept for offline repair");
        }

        finalize_depth_parts(cfg, ffprobe_path, logger);
        validate_media(file_path("depth.mkv"), "depth");
        if(checked_any_media) {
            append_retime_log(log_path, "recording media finalization completed");
            set_file_mtime_to_start(log_path, start_us_);
        }
        if(!rgb_ok && (file_size_nonzero(file_path("rgb.mp4")) || file_size_nonzero(rgb_debug_path_))) {
            throw std::runtime_error("RGB recording could not be finalized: " + directory_);
        }
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
        meta << "  \"rgb_h264_full_range\": " << (rgb_h264_full_range_ ? "true" : "false") << ",\n";
        meta << "  \"camera_name\": \"" << json_escape(camera_name_) << "\",\n";
        meta << "  \"storage_key\": \"" << json_escape(storage_key_) << "\",\n";
        meta << "  \"file_prefix\": \"" << json_escape(file_prefix_) << "\",\n";
        meta << "  \"segment_start_us\": " << start_us_ << ",\n";
        meta << "  \"segment_end_us\": " << (closed ? end_us_ : 0) << ",\n";
        meta << "  \"closed\": " << (closed ? "true" : "false") << ",\n";
        meta << "  \"frames_publish_state\": \"" << (closed ? "finalized" : "recording") << "\",\n";
        meta << "  \"recording_storage_mode\": \""
             << (cfg.recording_staging.enabled ? "local_staging" : "direct_nas") << "\",\n";
        meta << "  \"recording_relative_path\": \"" << json_escape(relative_directory_) << "\",\n";
        meta << "  \"nas_publish_root\": \"" << json_escape(cfg.nas_root) << "\",\n";
        meta << "  \"player_compatible_finalize_deferred\": "
             << (cfg.recording_staging.enabled && cfg.recording_staging.defer_player_compatible_finalize ? "true" : "false")
             << ",\n";
        meta << "  \"recording_ready_file\": \""
             << json_escape(prefixed_filename(file_prefix_, "recording_ready.json")) << "\",\n";
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
        meta.close();
        if(!meta) {
            throw std::runtime_error("cannot finish recording metadata: " + file_path("meta.json").string());
        }
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

    }

    bool active_ = false;
    uint64_t start_us_ = 0;
    uint64_t end_us_ = 0;
    std::chrono::steady_clock::time_point start_steady_{};
    std::string directory_;
    std::string recording_root_;
    std::string relative_directory_;
    std::string camera_name_;
    std::string storage_key_;
    std::string file_prefix_;
    bool rgb_h264_full_range_ = false;
    std::ofstream frames_csv_;
    std::ofstream rgb_recorded_frames_csv_;
    std::ofstream rgb_debug_;
    std::filesystem::path rgb_debug_path_;
    std::ofstream depth_debug_;
    FfmpegPipe rgb_pipe_;
    FfmpegPipe depth_pipe_;
    bool rgb_pipe_failed_ = false;
    bool depth_pipe_failed_ = false;
    unsigned depth_part_index_ = 0;
    std::vector<std::filesystem::path> depth_part_paths_;
    unsigned csv_rows_since_flush_ = 0;
    unsigned storage_check_packets_ = 0;
    bool storage_failed_ = false;
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
    std::mutex preview_mutex;
    uint64_t recording_start_us = 0;
    std::string recording_file_prefix;
    bool online = true;
    bool recording_requested = false;
    uint64_t last_status_us = 0;
    std::string status_endpoint;
    uint64_t last_media_us = 0;
    uint64_t last_media_session_id = 0;
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
    std::string sender_build_commit;
    std::string sender_build_source_hash;
    bool sender_build_dirty = false;
    double sender_rgb_input_fps = 0.0;
    double sender_depth_input_fps = 0.0;
    double sender_rgb_sent_fps = 0.0;
    double sender_depth_sent_fps = 0.0;
    uint64_t sender_rgb_dropped_frames = 0;
    uint64_t sender_depth_dropped_frames = 0;
    uint64_t sender_rgb_transport_retry_drops = 0;
    uint64_t sender_rgb_send_failures = 0;
    uint64_t sender_depth_send_failures = 0;
    bool sender_publish_warmup_active = false;
    uint64_t sender_publish_warmup_drops = 0;
    std::string last_announce_json;
    bool last_announce_live = false;
    uint64_t last_announce_received_us = 0;
    uint64_t last_announce_cache_save_us = 0;
    uint64_t last_status_log_us = 0;
    std::mutex segment_mutex;
    bool segment_active = false;
    bool segment_finalizing = false;
    std::string segment_dir;
    uint64_t segment_start_us = 0;
    std::atomic<bool> segment_rotation_requested{false};
    std::atomic<uint64_t> media_idle_finalizations{0};
    std::unique_ptr<SegmentWriter> segment = std::make_unique<SegmentWriter>();
    std::atomic<size_t> segment_finalize_pending{0};
    std::atomic<bool> segment_finalize_active{false};
    std::atomic<uint64_t> segment_finalize_completed{0};
    std::atomic<uint64_t> segment_finalize_failures{0};
    std::atomic<uint64_t> segment_finalize_last_duration_ms{0};
    std::mutex record_mutex;
    std::condition_variable record_cv;
    std::deque<RecordJob> record_queue;
    bool record_accepting = false;
    bool record_finalizing = false;
    uint64_t record_generation = 0;
    size_t record_queue_bytes = 0;
    size_t record_queue_peak_bytes = 0;
    size_t record_queue_peak_packets = 0;
    uint64_t record_prequeue_peak_delay_us = 0;
    uint64_t record_queue_peak_wait_us = 0;
    uint64_t record_enqueued_packets = 0;
    uint64_t record_dequeued_packets = 0;
    uint64_t record_backpressure_waits = 0;
    uint64_t record_oversize_packets = 0;
    uint64_t record_write_errors = 0;
    uint64_t last_record_write_error_log_us = 0;
    uint64_t last_finalize_queue_full_log_us = 0;
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

    ~ReceiverApp() {
        stop();
    }

    struct SegmentCloseTask {
        std::shared_ptr<CameraState> cam;
        std::string sender_id;
        std::string camera_id;
        std::string announce_json;
        bool resume_recording = false;
    };

    struct SegmentFinalizeTask {
        std::shared_ptr<CameraState> cam;
        std::unique_ptr<SegmentWriter> segment;
        std::string sender_id;
        std::string camera_id;
        std::string announce_json;
        std::string reason;
        std::string directory;
        uint64_t queued_us = 0;
        bool completes_stop = false;
        bool resume_recording = false;
    };

    struct MediaIngressOwner {
        uint64_t session_id = 0;
        int fd = -1;
        std::string peer_endpoint;
    };

    struct SenderControlTarget {
        std::string sender_id;
        std::string camera_id;
        std::string endpoint;
    };

    struct PreviewUdpAssembly {
        std::vector<uint8_t> bytes;
        std::vector<uint8_t> received;
        std::vector<uint32_t> chunk_offsets;
        std::vector<uint16_t> chunk_sizes;
        size_t received_count = 0;
        uint32_t total_size = 0;
        uint16_t chunk_count = 0;
        bool media_udp = false;
        uint64_t first_us = 0;
        uint64_t updated_us = 0;
    };

    struct ClientThread {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };

    void start() {
        bool expected = false;
        if(!started_.compare_exchange_strong(expected, true)) {
            return;
        }
        running_ = true;
        listener_start_failed_ = false;
        status_udp_ready_ = false;
        media_tcp_ready_ = false;
        media_udp_ready_ = false;
        preview_udp_ready_ = false;
        admin_ready_ = false;
        try {
            start_segment_finalize_worker();
            start_decoder_cleanup_worker();
            start_recording_maintenance_worker();
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
            const auto ready_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while(!required_listeners_ready() && !listener_start_failed_
                  && std::chrono::steady_clock::now() < ready_deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if(!required_listeners_ready()) {
                throw std::runtime_error(listener_start_failed_ ? "receiver listener startup failed"
                                                                : "receiver listener startup timed out");
            }
        }
        catch(...) {
            stop();
            throw;
        }
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
        if(!started_.exchange(false)) {
            return;
        }
        running_ = false;
        recording_maintenance_cv_.notify_all();
        if(recording_maintenance_thread_.joinable()) {
            recording_maintenance_thread_.join();
        }
        clock_sync_manager_.stop();
        shutdown_client_sockets();
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
        shutdown_client_sockets();
        join_client_threads();
        wait_segment_close_futures();
        std::vector<SegmentCloseTask> close_tasks;
        std::vector<std::shared_ptr<CameraState>> camera_snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for(auto &item : cameras_) {
                std::lock_guard<std::mutex> preview_lock(item.second->preview_mutex);
                cleanup_rgb_decoder_async(std::move(item.second->rgb_decoder));
                cleanup_rgb_decoder_async(std::move(item.second->main_rgb_decoder));
                camera_snapshot.push_back(item.second);
                close_tasks.push_back({item.second, item.second->sender_id, item.second->camera_id,
                                       item.second->last_announce_live ? item.second->last_announce_json : ""});
            }
        }
        stop_record_workers_sync(camera_snapshot);
        for(auto &task : close_tasks) {
            close_segment_task(task, "receiver stop");
        }
        wait_segment_finalize_idle();
        stop_segment_finalize_worker();
        stop_decoder_cleanup_worker();
        logger_.info("receiver stopped");
    }

    std::string status_json() {
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if(!lock.owns_lock()) {
            std::lock_guard<std::mutex> cache_lock(status_cache_mutex_);
            if(!status_cache_.empty()) {
                return status_cache_;
            }
            return "{\"running\":true,\"status_stale\":true,\"build_commit\":\"" + std::string(GWV3_GIT_COMMIT)
                   + "\",\"build_dirty\":" + std::string(GWV3_GIT_DIRTY != 0 ? "true" : "false")
                   + ",\"build_source_hash\":\"" + std::string(GWV3_RECEIVER_SOURCE_HASH) + "\",\"cameras\":[]}";
        }
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
        out << "\"build_commit\":\"" << json_escape(GWV3_GIT_COMMIT) << "\",";
        out << "\"build_dirty\":" << (GWV3_GIT_DIRTY != 0 ? "true" : "false") << ',';
        out << "\"build_source_hash\":\"" << GWV3_RECEIVER_SOURCE_HASH << "\",";
        out << "\"recording_all\":" << (recording_all_ ? "true" : "false") << ',';
        out << "\"recording_start_us\":" << recording_all_start_us_ << ',';
        out << "\"default_file_prefix\":\"" << json_escape(runtime_state_.default_file_prefix) << "\",";
        out << "\"file_prefix_scope\":\"per_camera\",";
        out << "\"nas_root\":\"" << json_escape(config_.nas_root) << "\",";
        out << "\"recording_staging\":{";
        out << "\"enabled\":" << (config_.recording_staging.enabled ? "true" : "false") << ',';
        out << "\"root\":\"" << json_escape(config_.recording_staging.root) << "\",";
        out << "\"defer_player_compatible_finalize\":"
            << (config_.recording_staging.defer_player_compatible_finalize ? "true" : "false") << ',';
        out << "\"idle_finalize_ms\":" << config_.recording_staging.idle_finalize_ms << "},";
        out << "\"recording_staging_enabled\":" << (config_.recording_staging.enabled ? "true" : "false") << ',';
        out << "\"recording_write_root\":\""
            << json_escape(config_.recording_staging.enabled ? config_.recording_staging.root : config_.nas_root) << "\",";
        {
            std::lock_guard<std::mutex> uploader_lock(uploader_status_mutex_);
            out << "\"recording_uploader\":"
                << (uploader_status_json_.empty() ? "{\"available\":false}" : uploader_status_json_) << ',';
        }
        out << "\"media_port\":" << config_.media_port << ',';
        out << "\"preview_enabled\":" << (config_.preview_enabled ? "true" : "false") << ',';
        out << "\"media_udp_enabled\":" << (config_.media_udp_enabled ? "true" : "false") << ',';
        out << "\"media_udp_port\":" << config_.media_udp_port << ',';
        out << "\"preview_udp_enabled\":" << (config_.preview_enabled && config_.preview_udp_enabled ? "true" : "false") << ',';
        out << "\"preview_udp_port\":" << config_.preview_udp_port << ',';
        out << "\"active_media_clients\":" << active_media_clients_.load() << ',';
        out << "\"active_admin_clients\":" << active_admin_clients_.load() << ',';
        out << "\"media_ingress_superseded_sessions\":" << media_ingress_superseded_sessions_.load() << ',';
        out << "\"media_ingress_stale_packets\":" << media_ingress_stale_packets_.load() << ',';
        out << "\"record_queue_total_bytes\":" << total_record_queue_bytes_.load() << ',';
        out << "\"record_finalize_max_pending_segments\":" << config_.record_finalize_max_pending_segments << ',';
        out << "\"record_finalize_outstanding_segments\":" << segment_finalize_outstanding_status_.load() << ',';
        out << "\"record_finalize_queued_segments\":" << segment_finalize_queued_status_.load() << ',';
        out << "\"record_finalize_active_segments\":" << segment_finalize_active_status_.load() << ',';
        out << "\"record_finalize_completed_segments\":" << segment_finalize_completed_total_.load() << ',';
        out << "\"record_finalize_failed_segments\":" << segment_finalize_failures_total_.load() << ',';
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
            std::lock_guard<std::mutex> preview_lock(cam.preview_mutex);
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
            uint64_t record_queue_oldest_age_ms = 0;
            uint64_t record_prequeue_peak_delay_ms = 0;
            uint64_t record_queue_peak_wait_ms = 0;
            uint64_t record_enqueued_packets = 0;
            uint64_t record_dequeued_packets = 0;
            uint64_t record_backpressure_waits = 0;
            uint64_t record_oversize_packets = 0;
            uint64_t record_write_errors = 0;
            uint32_t record_active_writes = 0;
            bool record_worker_started = false;
            bool record_accepting = false;
            bool record_finalizing = false;
            {
                std::lock_guard<std::mutex> record_lock(cam.record_mutex);
                record_queue_packets = cam.record_queue.size();
                record_queue_bytes = cam.record_queue_bytes;
                record_queue_peak_packets = cam.record_queue_peak_packets;
                record_queue_peak_bytes = cam.record_queue_peak_bytes;
                if(!cam.record_queue.empty() && cam.record_queue.front().enqueue_us > 0
                   && now >= cam.record_queue.front().enqueue_us) {
                    record_queue_oldest_age_ms = (now - cam.record_queue.front().enqueue_us) / 1000ull;
                }
                record_prequeue_peak_delay_ms = cam.record_prequeue_peak_delay_us / 1000ull;
                record_queue_peak_wait_ms = cam.record_queue_peak_wait_us / 1000ull;
                record_enqueued_packets = cam.record_enqueued_packets;
                record_dequeued_packets = cam.record_dequeued_packets;
                record_backpressure_waits = cam.record_backpressure_waits;
                record_oversize_packets = cam.record_oversize_packets;
                record_write_errors = cam.record_write_errors;
                record_active_writes = cam.record_active_writes;
                record_worker_started = cam.record_worker_started;
                record_accepting = cam.record_accepting;
                record_finalizing = cam.record_finalizing;
            }
            if(!first) {
                out << ',';
            }
            first = false;
            const auto clock_model = clock_sync_manager_.get_model(cam.sender_id);
            const size_t segment_finalize_pending = cam.segment_finalize_pending.load();
            const bool segment_finalize_active = cam.segment_finalize_active.load();
            out << "{";
            out << "\"sender_id\":\"" << json_escape(cam.sender_id) << "\",";
            out << "\"camera_id\":\"" << json_escape(cam.camera_id) << "\",";
            out << "\"sender_build_commit\":\"" << json_escape(cam.sender_build_commit) << "\",";
            out << "\"sender_build_source_hash\":\"" << json_escape(cam.sender_build_source_hash) << "\",";
            out << "\"sender_build_dirty\":" << (cam.sender_build_dirty ? "true" : "false") << ',';
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
            out << "\"segment_start_us\":" << cam.segment_start_us << ',';
            out << "\"segment_finalize_pending\":" << segment_finalize_pending << ',';
            out << "\"segment_finalize_active\":" << (segment_finalize_active ? "true" : "false") << ',';
            out << "\"segment_finalize_completed\":" << cam.segment_finalize_completed.load() << ',';
            out << "\"segment_finalize_failures\":" << cam.segment_finalize_failures.load() << ',';
            out << "\"segment_finalize_last_duration_ms\":" << cam.segment_finalize_last_duration_ms.load() << ',';
            out << "\"segment_rotation_requested\":" << (cam.segment_rotation_requested.load() ? "true" : "false") << ',';
            out << "\"media_idle_finalizations\":" << cam.media_idle_finalizations.load() << ',';
            out << "\"last_media_session_id\":" << cam.last_media_session_id << ',';
            out << "\"sender_rgb_input_fps\":" << cam.sender_rgb_input_fps << ',';
            out << "\"sender_depth_input_fps\":" << cam.sender_depth_input_fps << ',';
            out << "\"sender_rgb_sent_fps\":" << cam.sender_rgb_sent_fps << ',';
            out << "\"sender_depth_sent_fps\":" << cam.sender_depth_sent_fps << ',';
            out << "\"sender_rgb_dropped_frames\":" << cam.sender_rgb_dropped_frames << ',';
            out << "\"sender_depth_dropped_frames\":" << cam.sender_depth_dropped_frames << ',';
            out << "\"sender_rgb_transport_retry_drops\":" << cam.sender_rgb_transport_retry_drops << ',';
            out << "\"sender_rgb_send_failures\":" << cam.sender_rgb_send_failures << ',';
            out << "\"sender_depth_send_failures\":" << cam.sender_depth_send_failures << ',';
            out << "\"sender_publish_warmup_active\":" << (cam.sender_publish_warmup_active ? "true" : "false") << ',';
            out << "\"sender_publish_warmup_drops\":" << cam.sender_publish_warmup_drops << ',';
            out << "\"record_queue_packets\":" << record_queue_packets << ',';
            out << "\"record_queue_bytes\":" << record_queue_bytes << ',';
            out << "\"record_queue_peak_packets\":" << record_queue_peak_packets << ',';
            out << "\"record_queue_peak_bytes\":" << record_queue_peak_bytes << ',';
            out << "\"record_queue_oldest_age_ms\":" << record_queue_oldest_age_ms << ',';
            out << "\"record_prequeue_peak_delay_ms\":" << record_prequeue_peak_delay_ms << ',';
            out << "\"record_queue_peak_wait_ms\":" << record_queue_peak_wait_ms << ',';
            out << "\"record_enqueued_packets\":" << record_enqueued_packets << ',';
            out << "\"record_dequeued_packets\":" << record_dequeued_packets << ',';
            out << "\"record_active_writes\":" << record_active_writes << ',';
            out << "\"record_backpressure_waits\":" << record_backpressure_waits << ',';
            out << "\"record_oversize_packets\":" << record_oversize_packets << ',';
            out << "\"record_write_errors\":" << record_write_errors << ',';
            out << "\"record_worker_started\":" << (record_worker_started ? "true" : "false") << ',';
            out << "\"record_accepting\":" << (record_accepting ? "true" : "false") << ',';
            out << "\"record_finalizing\":" << (record_finalizing ? "true" : "false") << ',';
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
            out << "\"rgb_h264_full_range\":"
                << (rgb_h264_full_range_for_camera(config_, cam.sender_id, cam.camera_id) ? "true" : "false") << ',';
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
        auto status = out.str();
        {
            std::lock_guard<std::mutex> cache_lock(status_cache_mutex_);
            status_cache_ = status;
        }
        return status;
    }

    std::string config_json() const {
        std::ostringstream out;
        out << "{";
        out << "\"build_commit\":\"" << json_escape(GWV3_GIT_COMMIT) << "\",";
        out << "\"build_dirty\":" << (GWV3_GIT_DIRTY != 0 ? "true" : "false") << ',';
        out << "\"build_source_hash\":\"" << GWV3_RECEIVER_SOURCE_HASH << "\",";
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
        out << "\"rgb_h264_full_range_camera_keys\":[";
        bool first_full_range_key = true;
        for(const auto &key : config_.rgb_h264_full_range_camera_keys) {
            if(!first_full_range_key) {
                out << ',';
            }
            first_full_range_key = false;
            out << '"' << json_escape(key) << '"';
        }
        out << "],";
        out << "\"admin_bind_ip\":\"" << json_escape(config_.admin_bind_ip) << "\",";
        out << "\"admin_port\":" << config_.admin_port << ',';
        out << "\"nas_root\":\"" << json_escape(config_.nas_root) << "\",";
        out << "\"state_path\":\"" << json_escape(config_.state_path) << "\",";
        out << "\"default_file_prefix\":\"" << json_escape(runtime_state_.default_file_prefix) << "\",";
        out << "\"file_prefix_scope\":\"per_camera\",";
        out << "\"segment_seconds\":" << config_.segment_seconds << ',';
        out << "\"max_payload_mb\":" << (config_.max_payload_bytes / (1024ull * 1024ull)) << ',';
        out << "\"record_queue_max_mb\":" << (config_.record_queue_max_bytes / (1024ull * 1024ull));
        out << ",\"record_queue_total_max_mb\":" << (config_.record_queue_total_max_bytes / (1024ull * 1024ull));
        out << ",\"record_finalize_max_pending_segments\":" << config_.record_finalize_max_pending_segments;
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

    bool rotate_record_segment_async(const std::shared_ptr<CameraState> &cam,
                                     const RecordJob &job,
                                     const MediaPacket &packet,
                                     bool allow_timed_rotation) {
        if(!cam->segment || !cam->segment->active()) {
            return false;
        }
        const bool profile_changed = cam->segment->stream_profile_changed(packet);
        const bool timed_rotation = allow_timed_rotation
                                    && (cam->segment_rotation_requested.load() || cam->segment->should_rotate(config_));
        if(!profile_changed && !timed_rotation) {
            return false;
        }

        if(!reserve_segment_finalize_slot(profile_changed)) {
            bool should_log = false;
            {
                std::lock_guard<std::mutex> record_lock(cam->record_mutex);
                const uint64_t current_us = now_us();
                should_log = cam->last_finalize_queue_full_log_us == 0
                             || current_us - cam->last_finalize_queue_full_log_us >= kRecordQueueWarnIntervalUs;
                if(should_log) {
                    cam->last_finalize_queue_full_log_us = current_us;
                }
            }
            if(should_log) {
                logger_.warn("segment rotation deferred because finalization queue is full camera=" + cam->key
                             + " outstanding=" + std::to_string(segment_finalize_outstanding_status_.load())
                             + " max=" + std::to_string(config_.record_finalize_max_pending_segments));
            }
            return false;
        }

        auto next_segment = std::make_unique<SegmentWriter>();
        try {
            next_segment->start(config_, job.sender_id, job.camera_id, job.camera_name, job.storage_key,
                                job.file_prefix, job.announce_json, logger_);
        }
        catch(...) {
            release_segment_finalize_slot();
            throw;
        }

        auto previous_segment = std::move(cam->segment);
        const std::string previous_directory = previous_segment->directory();
        previous_segment->mark_end_us(now_us());
        cam->segment = std::move(next_segment);
        cam->segment_rotation_requested.store(false);

        SegmentFinalizeTask task;
        task.cam = cam;
        task.segment = std::move(previous_segment);
        task.sender_id = job.sender_id;
        task.camera_id = job.camera_id;
        task.announce_json = job.announce_json;
        task.reason = profile_changed ? "media_profile_changed" : "segment_duration_elapsed";
        task.directory = previous_directory;
        if(!enqueue_reserved_segment_finalize(std::move(task))) {
            throw std::runtime_error("cannot enqueue detached segment finalization: " + previous_directory);
        }
        logger_.info("recording segment rotated asynchronously camera=" + cam->key + " old=" + previous_directory
                     + " new=" + cam->segment->directory() + " reason="
                     + (profile_changed ? "media_profile_changed" : "segment_duration_elapsed"));
        return true;
    }

    void write_record_job(const std::shared_ptr<CameraState> &cam, RecordJob job) {
        if(!job.packet) {
            return;
        }
        const MediaPacket &queued_packet = *job.packet;
        const MediaPacket *record_packet = &queued_packet;
        std::optional<MediaPacket> decoded_depth_packet;
        if(queued_packet.stream_type == StreamType::depth_raw && queued_packet.codec_or_compression != "none") {
            try {
                decoded_depth_packet = normalized_depth_packet(queued_packet);
                record_packet = &*decoded_depth_packet;
            }
            catch(const std::exception &e) {
                bool should_log = false;
                {
                    std::lock_guard<std::mutex> record_lock(cam->record_mutex);
                    cam->record_write_errors++;
                    const uint64_t current_us = now_us();
                    should_log = cam->last_record_write_error_log_us == 0
                                 || current_us - cam->last_record_write_error_log_us >= kRecordQueueWarnIntervalUs;
                    if(should_log) {
                        cam->last_record_write_error_log_us = current_us;
                    }
                }
                if(should_log) {
                    logger_.warn(std::string("depth record packet ignored camera=") + cam->key + " frame="
                                 + std::to_string(queued_packet.frame_id) + ": " + e.what());
                }
                return;
            }
        }

        bool allow_segment_rotate = true;
        {
            std::lock_guard<std::mutex> record_lock(cam->record_mutex);
            allow_segment_rotate = !cam->record_finalizing;
        }

        try {
            bool segment_active = false;
            std::string segment_dir;
            uint64_t segment_start_us = 0;
            {
                std::lock_guard<std::mutex> segment_lock(cam->segment_mutex);
                rotate_record_segment_async(cam, job, *record_packet, allow_segment_rotate);
                cam->segment->write_packet(config_, *record_packet, job.sender_id, job.camera_id, job.camera_name, job.storage_key,
                                           job.file_prefix, job.announce_json, logger_, false);
                segment_active = cam->segment->active();
                segment_dir = cam->segment->directory();
                segment_start_us = cam->segment->start_us();
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                cam->segment_active = segment_active;
                cam->segment_dir = std::move(segment_dir);
                cam->segment_start_us = segment_start_us;
            }
        }
        catch(const std::exception &e) {
            bool should_log = false;
            {
                std::lock_guard<std::mutex> record_lock(cam->record_mutex);
                cam->record_write_errors++;
                cam->record_accepting = false;
                cam->record_generation++;
                const size_t discarded_queue_bytes = cam->record_queue_bytes;
                cam->record_queue.clear();
                cam->record_queue_bytes = 0;
                if(discarded_queue_bytes > 0) {
                    const size_t total_before = total_record_queue_bytes_.fetch_sub(discarded_queue_bytes);
                    if(total_before < discarded_queue_bytes) {
                        total_record_queue_bytes_.store(0);
                    }
                }
                const uint64_t current_us = now_us();
                should_log = cam->last_record_write_error_log_us == 0
                             || current_us - cam->last_record_write_error_log_us >= kRecordQueueWarnIntervalUs;
                if(should_log) {
                    cam->last_record_write_error_log_us = current_us;
                }
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                cam->last_error = std::string("recording_write_failed: ") + e.what();
            }
            cam->record_cv.notify_all();
            if(should_log) {
                logger_.warn(std::string("record packet write failed; recording input paused camera=") + cam->key
                             + " frame=" + std::to_string(queued_packet.frame_id) + ": " + e.what());
            }
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
                const size_t total_before = total_record_queue_bytes_.fetch_sub(job.queue_bytes);
                if(total_before < job.queue_bytes) {
                    total_record_queue_bytes_.store(0);
                }
                cam->record_dequeued_packets++;
                cam->record_active_writes++;
                const uint64_t dequeue_us = now_us();
                if(job.enqueue_us > 0 && dequeue_us >= job.enqueue_us) {
                    cam->record_queue_peak_wait_us =
                        std::max(cam->record_queue_peak_wait_us, dequeue_us - job.enqueue_us);
                }
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
        std::lock_guard<std::mutex> record_lock(cam->record_mutex);
        if(cam->record_worker_started) {
            return;
        }
        cam->record_worker_stop = false;
        try {
            cam->record_worker = std::thread([this, cam] { record_worker_loop(cam); });
            cam->record_worker_started = true;
        }
        catch(...) {
            cam->record_worker_stop = false;
            cam->record_worker_started = false;
            throw;
        }
    }

    static void set_record_accepting(const std::shared_ptr<CameraState> &cam, bool accepting) {
        if(!cam) {
            return;
        }
        bool changed = false;
        {
            std::lock_guard<std::mutex> record_lock(cam->record_mutex);
            if(cam->record_accepting != accepting) {
                cam->record_accepting = accepting;
                cam->record_generation++;
                changed = true;
            }
        }
        if(changed) {
            cam->record_cv.notify_all();
        }
    }

    static void set_record_finalizing(const std::shared_ptr<CameraState> &cam, bool finalizing) {
        if(!cam) {
            return;
        }
        {
            std::lock_guard<std::mutex> record_lock(cam->record_mutex);
            cam->record_finalizing = finalizing;
            if(!finalizing) {
                return;
            }
        }
        cam->record_cv.notify_all();
    }

    bool enqueue_record_job(const std::shared_ptr<CameraState> &cam, RecordJob job) {
        if(!cam || !job.packet || job.packet->stream_type == StreamType::rgb_preview) {
            return false;
        }
        start_record_worker_if_needed(cam);
        job.queue_bytes = record_packet_queue_bytes(*job.packet);
        job.enqueue_us = now_us();
        const uint64_t prequeue_delay_us = job.packet->receiver_receive_timestamp_us > 0
                                               && job.enqueue_us >= job.packet->receiver_receive_timestamp_us
                                           ? job.enqueue_us - job.packet->receiver_receive_timestamp_us
                                           : 0;
        const size_t max_bytes = std::max<size_t>(1, config_.record_queue_max_bytes);
        std::unique_lock<std::mutex> record_lock(cam->record_mutex);
        if(job.queue_bytes > max_bytes) {
            cam->record_oversize_packets++;
        }
        bool total_reserved = false;
        while(!cam->record_worker_stop) {
            if(!media_ingress_session_is_current(job.media_ingress_key, job.media_session_id)) {
                media_ingress_stale_packets_.fetch_add(1);
                return false;
            }
            if(!cam->record_accepting || cam->record_generation != job.record_generation) {
                return false;
            }
            const bool camera_has_room = cam->record_queue.empty()
                                         || (cam->record_queue_bytes <= max_bytes
                                             && job.queue_bytes <= max_bytes - cam->record_queue_bytes);
            if(camera_has_room) {
                size_t total = total_record_queue_bytes_.load();
                while(total <= config_.record_queue_total_max_bytes
                      && job.queue_bytes <= config_.record_queue_total_max_bytes - total) {
                    if(total_record_queue_bytes_.compare_exchange_weak(total, total + job.queue_bytes)) {
                        total_reserved = true;
                        break;
                    }
                }
                if(total_reserved) {
                    break;
                }
            }
            cam->record_backpressure_waits++;
            cam->record_cv.wait_for(record_lock, std::chrono::milliseconds(100));
        }
        std::unique_lock<std::mutex> ingress_lock;
        bool current_session = true;
        if(job.media_session_id != 0) {
            ingress_lock = std::unique_lock<std::mutex>(media_ingress_mutex_);
            const auto owner = media_ingress_owners_.find(job.media_ingress_key);
            current_session = owner != media_ingress_owners_.end() && owner->second.session_id == job.media_session_id;
        }
        if(cam->record_worker_stop || !cam->record_accepting || cam->record_generation != job.record_generation
           || !current_session) {
            if(total_reserved) {
                total_record_queue_bytes_.fetch_sub(job.queue_bytes);
            }
            if(!current_session) {
                media_ingress_stale_packets_.fetch_add(1);
            }
            return false;
        }
        const size_t queue_bytes = job.queue_bytes;
        try {
            cam->record_queue_bytes += queue_bytes;
            cam->record_queue.push_back(std::move(job));
        }
        catch(...) {
            cam->record_queue_bytes -= queue_bytes;
            total_record_queue_bytes_.fetch_sub(queue_bytes);
            throw;
        }
        cam->record_enqueued_packets++;
        cam->record_prequeue_peak_delay_us = std::max(cam->record_prequeue_peak_delay_us, prequeue_delay_us);
        cam->record_queue_peak_bytes = std::max(cam->record_queue_peak_bytes, cam->record_queue_bytes);
        cam->record_queue_peak_packets = std::max(cam->record_queue_peak_packets, cam->record_queue.size());
        record_lock.unlock();
        cam->record_cv.notify_one();
        return true;
    }

    void start_segment_finalize_worker() {
        std::lock_guard<std::mutex> lock(segment_finalize_mutex_);
        if(segment_finalize_worker_running_) {
            return;
        }
        segment_finalize_worker_stop_ = false;
        segment_finalize_worker_ = std::thread([this] { segment_finalize_worker_loop(); });
        segment_finalize_worker_running_ = true;
    }

    bool reserve_segment_finalize_slot(bool wait_for_slot) {
        std::unique_lock<std::mutex> lock(segment_finalize_mutex_);
        const auto available = [this] {
            return segment_finalize_worker_stop_
                   || segment_finalize_outstanding_ < config_.record_finalize_max_pending_segments;
        };
        if(wait_for_slot) {
            segment_finalize_cv_.wait(lock, available);
        }
        else if(!available()) {
            return false;
        }
        if(segment_finalize_worker_stop_ || !segment_finalize_worker_running_) {
            return false;
        }
        ++segment_finalize_outstanding_;
        segment_finalize_outstanding_status_.store(segment_finalize_outstanding_);
        return true;
    }

    void release_segment_finalize_slot() {
        {
            std::lock_guard<std::mutex> lock(segment_finalize_mutex_);
            if(segment_finalize_outstanding_ > 0) {
                --segment_finalize_outstanding_;
            }
            segment_finalize_outstanding_status_.store(segment_finalize_outstanding_);
        }
        segment_finalize_cv_.notify_all();
    }

    bool enqueue_reserved_segment_finalize(SegmentFinalizeTask &&task) {
        task.queued_us = now_us();
        auto cam = task.cam;
        try {
            std::lock_guard<std::mutex> lock(segment_finalize_mutex_);
            segment_finalize_queue_.push_back(std::move(task));
            if(cam) {
                cam->segment_finalize_pending.fetch_add(1);
            }
            segment_finalize_queued_status_.store(segment_finalize_queue_.size());
        }
        catch(const std::exception &e) {
            logger_.error(std::string("cannot enqueue segment finalization: ") + e.what());
            release_segment_finalize_slot();
            return false;
        }
        segment_finalize_cv_.notify_one();
        return true;
    }

    void complete_segment_finalize_task(SegmentFinalizeTask &task, bool success, uint64_t duration_ms) {
        size_t pending_after = 0;
        if(task.cam) {
            task.cam->segment_finalize_active.store(false);
            task.cam->segment_finalize_last_duration_ms.store(duration_ms);
            if(success) {
                task.cam->segment_finalize_completed.fetch_add(1);
            }
            else {
                task.cam->segment_finalize_failures.fetch_add(1);
            }
            const size_t pending_before = task.cam->segment_finalize_pending.fetch_sub(1);
            pending_after = pending_before > 0 ? pending_before - 1 : 0;
            if(pending_before == 0) {
                task.cam->segment_finalize_pending.store(0);
            }
        }
        if(success) {
            segment_finalize_completed_total_.fetch_add(1);
        }
        else {
            segment_finalize_failures_total_.fetch_add(1);
        }

        if(task.cam && task.completes_stop) {
            std::lock_guard<std::mutex> lock(mutex_);
            task.cam->segment_finalizing = false;
            if(!success) {
                task.cam->last_error = "recording_finalize_failed: " + task.directory;
            }
            set_record_finalizing(task.cam, false);
            if(task.resume_recording && (recording_all_ || task.cam->recording_requested)) {
                set_record_accepting(task.cam, true);
            }
        }
        else if(task.cam && !success) {
            std::lock_guard<std::mutex> lock(mutex_);
            task.cam->last_error = "recording_finalize_failed: " + task.directory;
        }

        {
            std::lock_guard<std::mutex> lock(segment_finalize_mutex_);
            if(segment_finalize_outstanding_ > 0) {
                --segment_finalize_outstanding_;
            }
            segment_finalize_outstanding_status_.store(segment_finalize_outstanding_);
            segment_finalize_active_status_.store(0);
        }
        segment_finalize_cv_.notify_all();
        if(task.cam && pending_after == 0) {
            task.cam->record_cv.notify_all();
        }
    }

    void segment_finalize_worker_loop() {
        logger_.info("segment finalizer worker started concurrency=1 max_pending="
                     + std::to_string(config_.record_finalize_max_pending_segments));
        for(;;) {
            SegmentFinalizeTask task;
            {
                std::unique_lock<std::mutex> lock(segment_finalize_mutex_);
                segment_finalize_cv_.wait(lock, [this] {
                    return segment_finalize_worker_stop_ || !segment_finalize_queue_.empty();
                });
                if(segment_finalize_queue_.empty()) {
                    if(segment_finalize_worker_stop_) {
                        break;
                    }
                    continue;
                }
                task = std::move(segment_finalize_queue_.front());
                segment_finalize_queue_.pop_front();
                segment_finalize_queued_status_.store(segment_finalize_queue_.size());
                segment_finalize_active_status_.store(1);
                if(task.cam) {
                    task.cam->segment_finalize_active.store(true);
                }
            }

            const uint64_t started_us = now_us();
            const uint64_t queue_wait_ms = task.queued_us > 0 && started_us >= task.queued_us
                                               ? (started_us - task.queued_us) / 1000ull
                                               : 0;
            logger_.info("segment finalization started camera="
                         + (task.cam ? task.cam->key : camera_key(task.sender_id, task.camera_id))
                         + " directory=" + task.directory + " reason=" + task.reason
                         + " queue_wait_ms=" + std::to_string(queue_wait_ms));
            bool success = true;
            try {
                if(task.segment) {
                    task.segment->close(config_, task.sender_id, task.camera_id, task.announce_json, logger_);
                }
            }
            catch(const std::exception &e) {
                success = false;
                logger_.warn("segment finalization failed camera="
                             + (task.cam ? task.cam->key : camera_key(task.sender_id, task.camera_id))
                             + " directory=" + task.directory + ": " + e.what());
            }
            const uint64_t completed_us = now_us();
            const uint64_t duration_ms = completed_us >= started_us ? (completed_us - started_us) / 1000ull : 0;
            logger_.info("segment finalization completed camera="
                         + (task.cam ? task.cam->key : camera_key(task.sender_id, task.camera_id))
                         + " directory=" + task.directory + " success=" + (success ? "true" : "false")
                         + " duration_ms=" + std::to_string(duration_ms));
            task.segment.reset();
            complete_segment_finalize_task(task, success, duration_ms);
        }
        logger_.info("segment finalizer worker stopped");
    }

    void wait_segment_finalize_idle() {
        std::unique_lock<std::mutex> lock(segment_finalize_mutex_);
        segment_finalize_cv_.wait(lock, [this] { return segment_finalize_outstanding_ == 0; });
    }

    void stop_segment_finalize_worker() {
        wait_segment_finalize_idle();
        {
            std::lock_guard<std::mutex> lock(segment_finalize_mutex_);
            segment_finalize_worker_stop_ = true;
        }
        segment_finalize_cv_.notify_all();
        if(segment_finalize_worker_.joinable()) {
            segment_finalize_worker_.join();
        }
        std::lock_guard<std::mutex> lock(segment_finalize_mutex_);
        segment_finalize_worker_running_ = false;
    }

    void refresh_recording_uploader_status() {
        if(!config_.recording_staging.enabled) {
            return;
        }
        const auto status_path = std::filesystem::path(config_.recording_staging.root) / ".gwv3_uploader_status.json";
        std::ifstream input(status_path);
        std::string status;
        if(input) {
            const std::string raw((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
            Json::Value root;
            if(parse_json_object_strict(raw, root)) {
                Json::StreamWriterBuilder builder;
                builder["indentation"] = "";
                status = Json::writeString(builder, root);
            }
        }
        if(status.empty()) {
            status = "{\"available\":false,\"last_error\":\"uploader status unavailable\"}";
        }
        std::lock_guard<std::mutex> lock(uploader_status_mutex_);
        uploader_status_json_ = std::move(status);
    }

    void recording_maintenance_loop() {
        logger_.info("recording maintenance worker started idle_finalize_ms="
                     + std::to_string(config_.recording_staging.idle_finalize_ms));
        auto next_uploader_status_refresh = std::chrono::steady_clock::time_point::min();
        while(running_) {
            const uint64_t current_us = now_us();
            std::vector<std::shared_ptr<CameraState>> camera_snapshot;
            std::vector<SegmentCloseTask> idle_close_tasks;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                refresh_camera_liveness_locked(current_us);
                camera_snapshot.reserve(cameras_.size());
                for(auto &item : cameras_) {
                    auto &cam = item.second;
                    camera_snapshot.push_back(cam);
                    const uint64_t idle_limit_us = static_cast<uint64_t>(config_.recording_staging.idle_finalize_ms) * 1000ull;
                    const bool media_idle = cam->segment_active && cam->last_media_us > 0
                                            && current_us > cam->last_media_us
                                            && current_us - cam->last_media_us >= idle_limit_us;
                    if(media_idle && !cam->segment_finalizing) {
                        const bool resume_recording = recording_all_ || cam->recording_requested;
                        cam->segment_finalizing = true;
                        cam->segment_rotation_requested.store(false);
                        cam->media_idle_finalizations.fetch_add(1);
                        set_record_accepting(cam, false);
                        set_record_finalizing(cam, true);
                        idle_close_tasks.push_back({cam, cam->sender_id, cam->camera_id,
                                                    cam->last_announce_live ? cam->last_announce_json : "",
                                                    resume_recording});
                    }
                }
            }

            for(const auto &cam : camera_snapshot) {
                if(!cam) {
                    continue;
                }
                std::lock_guard<std::mutex> segment_lock(cam->segment_mutex);
                if(cam->segment && cam->segment->should_rotate(config_)) {
                    cam->segment_rotation_requested.store(true);
                }
            }
            if(!idle_close_tasks.empty()) {
                logger_.warn("recording media idle timeout; finalizing segments count="
                             + std::to_string(idle_close_tasks.size()));
                close_segments_async(std::move(idle_close_tasks), "recording media idle timeout");
            }

            const auto steady_now = std::chrono::steady_clock::now();
            if(steady_now >= next_uploader_status_refresh) {
                refresh_recording_uploader_status();
                next_uploader_status_refresh = steady_now + std::chrono::seconds(1);
            }
            std::unique_lock<std::mutex> wait_lock(recording_maintenance_mutex_);
            recording_maintenance_cv_.wait_for(wait_lock, std::chrono::milliseconds(250), [this] { return !running_; });
        }
        logger_.info("recording maintenance worker stopped");
    }

    void start_recording_maintenance_worker() {
        recording_maintenance_thread_ = std::thread([this] { recording_maintenance_loop(); });
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

    void close_segment_task(SegmentCloseTask &task, const std::string &reason) {
        if(!task.cam) {
            return;
        }
        wait_record_queue_idle(task.cam, reason);
        if(!reserve_segment_finalize_slot(true)) {
            std::lock_guard<std::mutex> lock(mutex_);
            task.cam->segment_finalizing = false;
            task.cam->last_error = "recording_finalize_failed: finalizer is unavailable";
            set_record_finalizing(task.cam, false);
            if(task.resume_recording && (recording_all_ || task.cam->recording_requested)) {
                set_record_accepting(task.cam, true);
            }
            return;
        }
        bool slot_reserved = true;
        try {
            auto replacement = std::make_unique<SegmentWriter>();
            std::unique_ptr<SegmentWriter> detached;
            std::string directory;
            {
                std::lock_guard<std::mutex> segment_lock(task.cam->segment_mutex);
                if(task.cam->segment && task.cam->segment->active()) {
                    detached = std::move(task.cam->segment);
                    detached->mark_end_us(now_us());
                    directory = detached->directory();
                    task.cam->segment = std::move(replacement);
                    task.cam->segment_rotation_requested.store(false);
                }
            }
            if(!detached) {
                release_segment_finalize_slot();
                slot_reserved = false;
                std::lock_guard<std::mutex> lock(mutex_);
                task.cam->segment_active = false;
                task.cam->segment_finalizing = false;
                task.cam->segment_dir.clear();
                task.cam->segment_start_us = 0;
                set_record_finalizing(task.cam, false);
                if(task.resume_recording && (recording_all_ || task.cam->recording_requested)) {
                    set_record_accepting(task.cam, true);
                }
                return;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                task.cam->segment_active = false;
                task.cam->segment_dir.clear();
                task.cam->segment_start_us = 0;
            }

            SegmentFinalizeTask finalize_task;
            finalize_task.cam = task.cam;
            finalize_task.segment = std::move(detached);
            finalize_task.sender_id = task.sender_id;
            finalize_task.camera_id = task.camera_id;
            finalize_task.announce_json = task.announce_json;
            finalize_task.reason = reason;
            finalize_task.directory = directory;
            finalize_task.completes_stop = true;
            finalize_task.resume_recording = task.resume_recording;
            if(!enqueue_reserved_segment_finalize(std::move(finalize_task))) {
                slot_reserved = false;
                throw std::runtime_error("cannot enqueue segment finalization: " + directory);
            }
            slot_reserved = false;
            logger_.info("recording segment queued for finalization camera=" + task.cam->key
                         + " directory=" + directory + " reason=" + reason);
        }
        catch(const std::exception &e) {
            if(slot_reserved) {
                release_segment_finalize_slot();
            }
            logger_.warn("recording segment finalize failed camera=" + task.cam->key + ": " + e.what());
            std::lock_guard<std::mutex> lock(mutex_);
            task.cam->segment_finalizing = false;
            task.cam->last_error = std::string("recording_finalize_failed: ") + e.what();
            set_record_finalizing(task.cam, false);
            if(task.resume_recording && (recording_all_ || task.cam->recording_requested)) {
                set_record_accepting(task.cam, true);
            }
        }
    }

    void reap_segment_close_futures() {
        std::lock_guard<std::mutex> lock(segment_close_futures_mutex_);
        auto it = segment_close_futures_.begin();
        while(it != segment_close_futures_.end()) {
            if(it->valid() && it->wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                it->get();
                it = segment_close_futures_.erase(it);
            }
            else {
                ++it;
            }
        }
    }

    void wait_segment_close_futures() {
        std::vector<std::future<void>> futures;
        {
            std::lock_guard<std::mutex> lock(segment_close_futures_mutex_);
            futures.swap(segment_close_futures_);
        }
        for(auto &future : futures) {
            if(future.valid()) {
                future.get();
            }
        }
    }

    void close_segments_async(std::vector<SegmentCloseTask> close_tasks, const std::string &done_log_message) {
        if(close_tasks.empty()) {
            return;
        }
        reap_segment_close_futures();
        auto tasks = std::make_shared<std::vector<SegmentCloseTask>>(std::move(close_tasks));
        try {
            auto future = std::async(std::launch::async, [this, tasks, done_log_message]() mutable {
                logger_.info(done_log_message + " finalization scheduling started");
                std::vector<std::future<void>> camera_futures;
                camera_futures.reserve(tasks->size());
                for(auto &task : *tasks) {
                    camera_futures.emplace_back(std::async(std::launch::async, [this, &task, &done_log_message] {
                        close_segment_task(task, done_log_message);
                    }));
                }
                for(auto &camera_future : camera_futures) {
                    camera_future.get();
                }
                logger_.info(done_log_message + " finalization queued");
            });
            try {
                std::lock_guard<std::mutex> lock(segment_close_futures_mutex_);
                segment_close_futures_.push_back(std::move(future));
            }
            catch(...) {
                if(future.valid()) {
                    future.get();
                }
                throw;
            }
        }
        catch(const std::exception &e) {
            logger_.warn(done_log_message + " async scheduling failed; queueing synchronously: " + e.what());
            for(auto &task : *tasks) {
                close_segment_task(task, done_log_message);
            }
            logger_.info(done_log_message + " finalization queued");
        }
    }

    void close_segments_sync(std::vector<SegmentCloseTask> close_tasks, const std::string &done_log_message) {
        if(close_tasks.empty()) {
            return;
        }
        for(auto &task : close_tasks) {
            close_segment_task(task, done_log_message);
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
            const bool any_finalizing = std::any_of(cameras_.begin(), cameras_.end(), [](const auto &item) {
                return item.second->segment_finalizing || item.second->record_finalizing;
            });
            if(any_finalizing) {
                return json_error("recording segments are still finalizing; retry after finalizing=false");
            }
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
                set_record_accepting(item.second, true);
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
                const bool already_finalizing = item.second->segment_finalizing || item.second->record_finalizing;
                const bool needs_close = item.second->recording_requested || item.second->segment_active;
                item.second->recording_requested = false;
                item.second->recording_start_us = 0;
                item.second->recording_file_prefix.clear();
                set_record_accepting(item.second, false);
                if(already_finalizing) {
                    finalizing = true;
                }
                else if(needs_close) {
                    item.second->segment_finalizing = true;
                    set_record_finalizing(item.second, true);
                    finalizing = true;
                    close_tasks.push_back({item.second, item.second->sender_id, item.second->camera_id,
                                           item.second->last_announce_live ? item.second->last_announce_json : ""});
                }
            }
            refresh_camera_liveness_locked(now_us());
        }
        logger_.info("recording stop-all requested");
        if(finalizing) {
            close_segments_async(std::move(close_tasks), "recording stop-all");
        }
        std::ostringstream out;
        out << "{\"ok\":true,\"recording_all\":false,\"recording_start_us\":" << recording_start_us
            << ",\"finalizing\":" << (finalizing ? "true" : "false")
            << ",\"finalized\":" << (finalizing ? "false" : "true") << "}";
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
            auto cam_ptr = ensure_camera_ptr_locked(sender_id, camera_id);
            auto &cam = *cam_ptr;
            if(cam.segment_finalizing || cam.record_finalizing) {
                return json_error("recording segment is still finalizing; retry after finalizing=false");
            }
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
            set_record_accepting(cam_ptr, true);
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
        bool finalizing = false;
        const auto key = camera_key(sender_id, camera_id);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = cameras_.find(key);
            if(it == cameras_.end()) {
                return "{\"ok\":false,\"error\":\"camera not found\"}";
            }
            if(recording_all_) {
                return json_error("cannot stop one camera while start-all recording is active; use stop-all");
            }
            cam = it->second;
            announce_json = cam->last_announce_live ? cam->last_announce_json : "";
            recording_start_us = cam->recording_start_us;
            if(cam->segment_finalizing || cam->record_finalizing) {
                return "{\"ok\":true,\"finalizing\":true,\"finalized\":false}";
            }
            finalizing = cam->recording_requested || cam->segment_active;
            cam->recording_requested = false;
            if(finalizing) {
                cam->segment_finalizing = true;
            }
            cam->recording_start_us = 0;
            cam->recording_file_prefix.clear();
            set_record_accepting(cam, false);
            if(finalizing) {
                set_record_finalizing(cam, true);
            }
        }
        logger_.info("recording stop requested: " + key);
        std::vector<SegmentCloseTask> close_tasks;
        close_tasks.push_back({cam, cam->sender_id, cam->camera_id, announce_json});
        if(finalizing) {
            close_segments_async(std::move(close_tasks), "recording stop: " + key);
        }
        std::ostringstream out;
        out << "{\"ok\":true,\"recording_start_us\":" << recording_start_us
            << ",\"finalizing\":" << (finalizing ? "true" : "false")
            << ",\"finalized\":" << (finalizing ? "false" : "true") << "}";
        return out.str();
    }

    std::string set_camera_name(const std::string &sender_id, const std::string &camera_id, const std::string &camera_name) {
        if(sender_id.empty() || camera_id.empty()) {
            return "{\"ok\":false,\"error\":\"sender_id and camera_id are required\"}";
        }
        if(const auto error = storage_text_error("camera_name", camera_name)) {
            return json_error(*error);
        }
        const auto key = camera_key(sender_id, camera_id);
        RuntimeState state_snapshot;
        uint64_t state_revision = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
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
            state_revision = ++runtime_state_revision_;
            state_snapshot = runtime_state_;
        }
        try {
            persist_runtime_state_snapshot(state_snapshot, state_revision);
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
        const auto key = camera_key(sender_id, camera_id);
        RuntimeState state_snapshot;
        uint64_t state_revision = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
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
            state_revision = ++runtime_state_revision_;
            state_snapshot = runtime_state_;
        }
        try {
            persist_runtime_state_snapshot(state_snapshot, state_revision);
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
        RuntimeState state_snapshot;
        uint64_t state_revision = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            runtime_state_.default_file_prefix = file_prefix;
            state_revision = ++runtime_state_revision_;
            state_snapshot = runtime_state_;
        }
        try {
            persist_runtime_state_snapshot(state_snapshot, state_revision);
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
        std::lock_guard<std::mutex> preview_lock(it->second->preview_mutex);
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
            std::lock_guard<std::mutex> preview_lock(cam.preview_mutex);
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
                        std::lock_guard<std::mutex> preview_lock(item.second->preview_mutex);
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
            std::lock_guard<std::mutex> preview_lock(it->second->preview_mutex);
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
            }
            std::lock_guard<std::mutex> preview_lock(cam.preview_mutex);
            if(key == main_preview_key_) {
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
    void persist_runtime_state_snapshot(const RuntimeState &snapshot, uint64_t revision) {
        std::lock_guard<std::mutex> save_lock(runtime_state_save_mutex_);
        if(revision <= runtime_state_save_revision_) {
            return;
        }
        runtime_state_save_revision_ = revision;
        save_runtime_state_file(config_.state_path, snapshot);
    }

    bool required_listeners_ready() const {
        return status_udp_ready_ && media_tcp_ready_ && admin_ready_
               && (!config_.media_udp_enabled || media_udp_ready_)
               && (!(config_.preview_enabled && config_.preview_udp_enabled) || preview_udp_ready_);
    }

    void start_decoder_cleanup_worker() {
        std::lock_guard<std::mutex> lock(decoder_cleanup_mutex_);
        if(decoder_cleanup_running_) {
            return;
        }
        decoder_cleanup_running_ = true;
        decoder_cleanup_thread_ = std::thread([this] {
            for(;;) {
                std::unique_ptr<RgbPreviewDecoder> decoder;
                {
                    std::unique_lock<std::mutex> cleanup_lock(decoder_cleanup_mutex_);
                    decoder_cleanup_cv_.wait(cleanup_lock, [&] {
                        return !decoder_cleanup_running_ || !decoder_cleanup_queue_.empty();
                    });
                    if(decoder_cleanup_queue_.empty()) {
                        if(!decoder_cleanup_running_) {
                            return;
                        }
                        continue;
                    }
                    decoder = std::move(decoder_cleanup_queue_.front());
                    decoder_cleanup_queue_.pop_front();
                }
                decoder->stop();
            }
        });
    }

    void cleanup_rgb_decoder_async(std::unique_ptr<RgbPreviewDecoder> decoder) {
        if(!decoder) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(decoder_cleanup_mutex_);
            if(decoder_cleanup_running_) {
                decoder_cleanup_queue_.push_back(std::move(decoder));
                decoder_cleanup_cv_.notify_one();
                return;
            }
        }
        decoder->stop();
    }

    void stop_decoder_cleanup_worker() {
        {
            std::lock_guard<std::mutex> lock(decoder_cleanup_mutex_);
            decoder_cleanup_running_ = false;
        }
        decoder_cleanup_cv_.notify_all();
        if(decoder_cleanup_thread_.joinable()) {
            decoder_cleanup_thread_.join();
        }
    }

    void reap_completed_client_threads_locked() {
        for(auto it = client_threads_.begin(); it != client_threads_.end();) {
            if(it->done && it->done->load(std::memory_order_acquire)) {
                if(it->thread.joinable()) {
                    it->thread.join();
                }
                it = client_threads_.erase(it);
            }
            else {
                ++it;
            }
        }
    }

    void launch_client_thread(int fd, const std::string &label, std::function<void()> task) {
        std::lock_guard<std::mutex> lock(client_threads_mutex_);
        reap_completed_client_threads_locked();
        client_fds_.insert(fd);
        auto done = std::make_shared<std::atomic<bool>>(false);
        try {
            client_threads_.push_back(ClientThread{});
            auto &client = client_threads_.back();
            client.done = done;
            client.thread = std::thread([this, fd, label, task = std::move(task), done]() mutable {
                try {
                    task();
                }
                catch(const std::exception &e) {
                    logger_.warn(label + " client handler failed: " + e.what());
                }
                catch(...) {
                    logger_.warn(label + " client handler failed: unknown exception");
                }
                {
                    std::lock_guard<std::mutex> client_lock(client_threads_mutex_);
                    client_fds_.erase(fd);
                }
                shutdown(fd, SHUT_RDWR);
                close(fd);
                done->store(true, std::memory_order_release);
            });
        }
        catch(...) {
            if(!client_threads_.empty() && !client_threads_.back().thread.joinable()) {
                client_threads_.pop_back();
            }
            client_fds_.erase(fd);
            close(fd);
            throw;
        }
    }

    void shutdown_client_sockets() {
        std::lock_guard<std::mutex> lock(client_threads_mutex_);
        for(int fd : client_fds_) {
            shutdown(fd, SHUT_RDWR);
        }
    }

    void join_client_threads() {
        std::vector<ClientThread> threads;
        {
            std::lock_guard<std::mutex> lock(client_threads_mutex_);
            threads.swap(client_threads_);
        }
        for(auto &client : threads) {
            if(client.thread.joinable()) {
                client.thread.join();
            }
        }
    }

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

    static std::string media_ingress_key(const MediaPacket &packet) {
        return packet.sender_id + '\x1f' + packet.camera_id + '\x1f' + stream_type_name(packet.stream_type);
    }

    bool claim_media_ingress(const MediaPacket &packet,
                             uint64_t session_id,
                             int fd,
                             const std::string &peer_endpoint) {
        if(session_id == 0) {
            return true;
        }

        const std::string key = media_ingress_key(packet);
        int superseded_fd = -1;
        uint64_t superseded_session = 0;
        std::string superseded_peer;
        bool stale = false;
        {
            std::lock_guard<std::mutex> lock(media_ingress_mutex_);
            auto [it, inserted] = media_ingress_owners_.try_emplace(
                key, MediaIngressOwner{session_id, fd, peer_endpoint});
            if(!inserted) {
                auto &owner = it->second;
                if(owner.session_id == session_id) {
                    owner.fd = fd;
                    owner.peer_endpoint = peer_endpoint;
                }
                else if(session_id < owner.session_id) {
                    stale = true;
                }
                else {
                    superseded_fd = owner.fd;
                    superseded_session = owner.session_id;
                    superseded_peer = owner.peer_endpoint;
                    owner = MediaIngressOwner{session_id, fd, peer_endpoint};
                }
            }
        }

        if(stale) {
            const uint64_t rejected = media_ingress_stale_packets_.fetch_add(1) + 1;
            if(rejected <= 10 || rejected % 100 == 0) {
                logger_.warn("stale media session packet rejected route=" + camera_key(packet.sender_id, packet.camera_id)
                             + " stream=" + stream_type_name(packet.stream_type)
                             + " session=" + std::to_string(session_id)
                             + " current_session_newer=true peer=" + peer_endpoint
                             + " rejected_total=" + std::to_string(rejected));
            }
            return false;
        }

        if(superseded_session != 0) {
            media_ingress_superseded_sessions_.fetch_add(1);
            logger_.warn("media session superseded route=" + camera_key(packet.sender_id, packet.camera_id)
                         + " stream=" + stream_type_name(packet.stream_type)
                         + " old_session=" + std::to_string(superseded_session)
                         + " new_session=" + std::to_string(session_id)
                         + " old_peer=" + superseded_peer + " new_peer=" + peer_endpoint);
            if(superseded_fd >= 0 && superseded_fd != fd) {
                shutdown(superseded_fd, SHUT_RDWR);
            }
        }
        return true;
    }

    void mark_media_ingress_session_closed(uint64_t session_id) {
        if(session_id == 0) {
            return;
        }
        std::lock_guard<std::mutex> lock(media_ingress_mutex_);
        for(auto &item : media_ingress_owners_) {
            if(item.second.session_id == session_id) {
                item.second.fd = -1;
            }
        }
    }

    bool media_ingress_session_is_current(const std::string &key, uint64_t session_id) {
        if(session_id == 0) {
            return true;
        }
        std::lock_guard<std::mutex> lock(media_ingress_mutex_);
        const auto owner = media_ingress_owners_.find(key);
        return owner != media_ingress_owners_.end() && owner->second.session_id == session_id;
    }

    bool bind_sender_source_locked(const std::string &sender_id, const std::string &peer_endpoint, uint64_t now) {
        const auto source_ip = socket_endpoint_ip(peer_endpoint);
        if(source_ip.empty()) {
            return false;
        }
        auto source = sender_source_ips_.find(sender_id);
        if(source == sender_source_ips_.end()) {
            if(sender_source_ips_.size() >= kMaxTrackedSenders) {
                return false;
            }
            sender_source_ips_[sender_id] = source_ip;
            return true;
        }
        if(source->second == source_ip) {
            return true;
        }
        const bool old_source_still_live = std::any_of(cameras_.begin(), cameras_.end(), [&](const auto &item) {
            return item.second->sender_id == sender_id
                   && is_recent_us(now, camera_last_seen_us(*item.second), kCameraOnlineTimeoutUs);
        });
        if(old_source_still_live) {
            return false;
        }
        logger_.warn("sender source IP changed after timeout sender_id=" + sender_id + " old=" + source->second + " new=" + source_ip);
        source->second = source_ip;
        return true;
    }

    void clear_camera_live_cache_locked(CameraState &cam) {
        std::lock_guard<std::mutex> preview_lock(cam.preview_mutex);
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
        if(it != cameras_.end() && (it->second->sender_id != sender_id || it->second->camera_id != camera_id)) {
            throw std::runtime_error("camera key collision for " + key);
        }
        if(it == cameras_.end()) {
            if(cameras_.size() >= kMaxTrackedCameras) {
                throw std::runtime_error("maximum tracked camera count reached");
            }
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
                state->record_accepting = true;
                state->record_generation = 1;
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
        Json::Value root;
        if(!parse_json_object_strict(json, root)) {
            logger_.warn("malformed status JSON ignored from=" + peer_endpoint);
            return;
        }
        const auto type = json_string_value(root, "message_type", "unknown");
        const auto sender_id = json_string_value(root, "sender_id");
        const auto camera_id = json_string_value(root, "camera_id");
        if(json_string_value(root, "protocol_version") != kProtocolVersion) {
            logger_.warn("status protocol version mismatch ignored from=" + peer_endpoint);
            return;
        }
        static const std::set<std::string> supported_types = {
            "sender_hello", "heartbeat", "clock_sync_report", "camera_announce", "camera_offline", "event"};
        if(supported_types.count(type) == 0) {
            logger_.warn("unsupported status message ignored type=" + type + " from=" + peer_endpoint);
            return;
        }
        bool should_log_status = type != "heartbeat" && type != "sender_hello";

        if(!is_valid_protocol_id(sender_id) || (!camera_id.empty() && !is_valid_protocol_id(camera_id))) {
            logger_.warn("status packet with invalid sender_id/camera_id ignored from=" + peer_endpoint);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(!bind_sender_source_locked(sender_id, peer_endpoint, now_us())) {
                logger_.warn("status sender source mismatch ignored sender_id=" + sender_id + " from=" + peer_endpoint);
                return;
            }
        }

        if(!sender_id.empty() && (type == "heartbeat" || type == "clock_sync_report")) {
            const bool clock_valid = root["clock_sync_valid"].isBool() && root["clock_sync_valid"].asBool();
            const auto offset_us = json_int64_value(root, "clock_offset_us").value_or(0);
            const auto delay_us = json_int64_value(root, "clock_delay_us").value_or(0);
            const auto drift_ppm = json_double_value(root, "clock_drift_ppm").value_or(0.0);
            const auto last_sync_us = clock_valid ? json_uint64_value(root, "clock_last_sync_us").value_or(0) : 0;
            clock_sync_manager_.update_from_sender_report(sender_id, offset_us, delay_us, drift_ppm, last_sync_us,
                                                          socket_endpoint_ip(peer_endpoint));
        }

        std::optional<RuntimeState> state_snapshot;
        uint64_t state_revision = 0;
        if(!sender_id.empty() && !camera_id.empty()) {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto key = camera_key(sender_id, camera_id);
            const auto existing = cameras_.find(key);
            if(existing != cameras_.end()
               && (existing->second->sender_id != sender_id || existing->second->camera_id != camera_id)) {
                logger_.warn("status camera identity collision ignored key=" + key + " from=" + peer_endpoint);
                return;
            }
            const auto code = type == "event" ? json_string_value(root, "event_code", "event") : "";
            const bool marks_online = type == "camera_announce" || code == "camera_connected" || code == "camera_reconnected";
            auto &cam = *ensure_camera_ptr_locked(sender_id, camera_id, marks_online);
            const auto received_us = now_us();
            cam.last_status_us = received_us;
            cam.status_endpoint = peer_endpoint;
            cam.sender_build_commit = json_string_value(root, "build_commit", cam.sender_build_commit);
            cam.sender_build_source_hash = json_string_value(root, "build_source_hash", cam.sender_build_source_hash);
            if(root["build_dirty"].isBool()) {
                cam.sender_build_dirty = root["build_dirty"].asBool();
            }
            if(type == "heartbeat") {
                cam.sender_rgb_input_fps = json_double_value(root, "rgb_measured_fps").value_or(cam.sender_rgb_input_fps);
                cam.sender_depth_input_fps = json_double_value(root, "depth_measured_fps").value_or(cam.sender_depth_input_fps);
                cam.sender_rgb_sent_fps = json_double_value(root, "rgb_sent_fps").value_or(cam.sender_rgb_sent_fps);
                cam.sender_depth_sent_fps = json_double_value(root, "depth_sent_fps").value_or(cam.sender_depth_sent_fps);
                cam.sender_rgb_dropped_frames =
                    json_uint64_value(root, "rgb_dropped_frames").value_or(cam.sender_rgb_dropped_frames);
                cam.sender_depth_dropped_frames =
                    json_uint64_value(root, "depth_dropped_frames").value_or(cam.sender_depth_dropped_frames);
                cam.sender_rgb_transport_retry_drops =
                    json_uint64_value(root, "rgb_transport_retry_drops").value_or(cam.sender_rgb_transport_retry_drops);
                cam.sender_rgb_send_failures =
                    json_uint64_value(root, "rgb_send_failures_total").value_or(cam.sender_rgb_send_failures);
                cam.sender_depth_send_failures =
                    json_uint64_value(root, "depth_send_failures_total").value_or(cam.sender_depth_send_failures);
                if(root["publish_warmup_active"].isBool()) {
                    cam.sender_publish_warmup_active = root["publish_warmup_active"].asBool();
                }
                cam.sender_publish_warmup_drops =
                    json_uint64_value(root, "publish_warmup_dropped_framesets").value_or(cam.sender_publish_warmup_drops);
            }
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
                    state_revision = ++runtime_state_revision_;
                    state_snapshot = runtime_state_;
                }
            }
            else if(type == "camera_offline") {
                cam.online = false;
                cam.last_announce_live = false;
                cam.last_announce_received_us = 0;
                clear_camera_live_cache_locked(cam);
                cam.last_error = json_string_value(root, "reason", "camera_offline");
            }
            else if(type == "event") {
                const auto message = json_string_value(root, "message");
                cam.last_error = code + (message.empty() ? "" : ": " + message);
                if(code == "camera_unavailable" || code == "camera_disconnected") {
                    cam.online = false;
                    cam.last_announce_live = false;
                    cam.last_announce_received_us = 0;
                    clear_camera_live_cache_locked(cam);
                }
            }
        }

        if(state_snapshot) {
            try {
                persist_runtime_state_snapshot(*state_snapshot, state_revision);
            }
            catch(const std::exception &e) {
                logger_.error(e.what());
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
        if(decoder && !decoder->active()) {
            cleanup_rgb_decoder_async(std::move(decoder));
            preview_width = 0;
            preview_height = 0;
            preview_us = 0;
        }
        if(!decoder) {
            if(!has_idr) {
                return false;
            }
            decoder = std::make_unique<RgbPreviewDecoder>();
            if(!decoder->start(config_, decoder_key, packet.width, packet.height, target_width, preview_fps,
                               rgb_h264_full_range_for_camera(config_, cam.sender_id, cam.camera_id), logger_)) {
                cleanup_rgb_decoder_async(std::move(decoder));
                preview_width = 0;
                preview_height = 0;
                preview_us = 0;
                return false;
            }
            auto decoder_prefix = cam.rgb_preview_prefix_h264;
            const auto packet_prefix = h264_non_vcl_prefix(packet.payload);
            if(!packet_prefix.empty() && h264_payload_has_sps_and_pps(packet_prefix)) {
                decoder_prefix = packet_prefix;
            }
            if(!decoder_prefix.empty()) {
                if(!decoder->write_packet(decoder_prefix)) {
                    cleanup_rgb_decoder_async(std::move(decoder));
                    preview_width = 0;
                    preview_height = 0;
                    preview_us = 0;
                    return false;
                }
            }
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

    void update_rgb_preview_locked(CameraState &cam,
                                   const MediaPacket &packet,
                                   bool has_idr,
                                   bool has_vcl,
                                   bool thumbnail_requested,
                                   bool thumbnail_expired,
                                   bool main_requested,
                                   bool main_expired,
                                   bool is_main_camera) {
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

        if(thumbnail_requested) {
            feed_rgb_preview_decoder_locked(cam, packet, has_idr, cam.rgb_decoder, kRgbPreviewWidth, kRgbPreviewFps, cam.key, cam.rgb_preview_width,
                                            cam.rgb_preview_height, cam.rgb_preview_us);
        }
        else if(cam.rgb_decoder && thumbnail_expired) {
            cleanup_rgb_decoder_async(std::move(cam.rgb_decoder));
            cam.rgb_preview_width = 0;
            cam.rgb_preview_height = 0;
            cam.rgb_preview_us = 0;
        }

        if(kEnableJpegMainPreview && is_main_camera && main_requested) {
            feed_rgb_preview_decoder_locked(cam, packet, has_idr, cam.main_rgb_decoder, kRgbMainPreviewWidth, kRgbMainPreviewFps, cam.key + ":main",
                                            cam.main_rgb_preview_width, cam.main_rgb_preview_height, cam.main_rgb_preview_us);
        }
        else if(cam.main_rgb_decoder && (!is_main_camera || main_expired)) {
            cleanup_rgb_decoder_async(std::move(cam.main_rgb_decoder));
            cam.main_rgb_preview_width = 0;
            cam.main_rgb_preview_height = 0;
            cam.main_rgb_preview_us = 0;
        }
    }

    void handle_media_packet(MediaPacket packet,
                             const std::string &peer_endpoint,
                             uint64_t media_session_id = 0,
                             int media_fd = -1) {
        if(!is_valid_protocol_id(packet.sender_id) || !is_valid_protocol_id(packet.camera_id)) {
            logger_.warn("media packet with invalid sender_id/camera_id ignored");
            return;
        }
        const uint64_t packet_receive_us = now_us();
        packet.receiver_receive_timestamp_us = packet_receive_us;
        const auto clock_model = clock_sync_manager_.get_model(packet.sender_id);
        const bool sender_system_time_available = (packet.flags & has_system_timestamp) != 0u && packet.system_timestamp_us > 0;
        packet.clock_sync_valid = false;
        packet.sender_offset_us = clock_model.offset_us;
        packet.sender_delay_us = clock_model.delay_us;
        packet.sender_drift_ppm = clock_model.drift_ppm;
        // The offset model maps sender system time to receiver time.
        const uint64_t fallback_timestamp_us = sender_system_time_available ? packet.system_timestamp_us : packet.timestamp_us;
        packet.global_timestamp_us = fallback_timestamp_us;
        if(clock_model.valid && sender_system_time_available) {
            const int64_t candidate = clock_sync_manager_.get_global_timestamp_us(packet.sender_id, packet.system_timestamp_us);
            if(candidate > 0) {
                const uint64_t candidate_us = static_cast<uint64_t>(candidate);
                const uint64_t receiver_skew_us = candidate_us >= packet_receive_us ? candidate_us - packet_receive_us
                                                                                     : packet_receive_us - candidate_us;
                if(receiver_skew_us <= kMaxGlobalTimestampReceiverSkewUs) {
                    packet.clock_sync_valid = true;
                    packet.global_timestamp_us = candidate_us;
                }
            }
        }

        const bool rgb_stream_packet = packet.stream_type == StreamType::rgb || packet.stream_type == StreamType::rgb_preview;
        const bool rgb_has_idr = rgb_stream_packet &&
                                 (((packet.flags & key_frame) != 0u) || h264_payload_has_nal_type(packet.payload, 5));
        const bool rgb_has_vcl = rgb_stream_packet && h264_payload_has_vcl_nal(packet.payload);
        if(!claim_media_ingress(packet, media_session_id, media_fd, peer_endpoint)) {
            return;
        }

        std::shared_ptr<CameraState> cam;
        bool build_depth_preview = false;
        uint64_t depth_preview_media_us = 0;
        double depth_preview_scale = fallback_depth_scale_for_camera(packet.sender_id, packet.camera_id);
        std::vector<SenderControlTarget> web_preview_control_targets;
        uint64_t web_preview_control_request_us = 0;
        H264StreamBuffer *rgb_stream_update = nullptr;
        bool update_rgb_preview = false;
        bool thumbnail_preview_requested = false;
        bool thumbnail_preview_expired = false;
        bool main_preview_requested = false;
        bool main_preview_expired = false;
        bool is_main_preview_camera = false;
        bool should_record = false;
        std::string record_sender_id;
        std::string record_camera_id;
        std::string record_camera_name;
        std::string record_storage_key;
        std::string record_file_prefix;
        std::string record_announce_json;
        uint64_t record_generation = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(!bind_sender_source_locked(packet.sender_id, peer_endpoint, packet_receive_us)) {
                logger_.warn("media sender source mismatch ignored sender_id=" + packet.sender_id + " from=" + peer_endpoint);
                return;
            }
            try {
                cam = ensure_camera_ptr_locked(packet.sender_id, packet.camera_id);
            }
            catch(const std::exception &e) {
                logger_.warn(std::string("media packet identity rejected: ") + e.what());
                return;
            }
            cam->last_media_us = packet_receive_us;
            cam->last_media_session_id = media_session_id;
            if(packet.stream_type == StreamType::rgb) {
                cam->rgb_packets++;
                cam->rgb_bytes += packet.payload_size;
                if(config_.preview_enabled) {
                    const auto media_now = cam->last_media_us;
                    const bool stream_requested = is_recent_us(media_now, cam->rgb_stream_requested_until_us, 0);
                    const bool thumbnail_requested = is_recent_us(media_now, cam->rgb_preview_requested_until_us, 0);
                    if(stream_requested) {
                        rgb_stream_update = &cam->rgb_stream;
                    }
                    const bool preview_stream_fresh = is_recent_us(media_now, cam->last_rgb_preview_packet_us, kPreviewFreshUs);
                    update_rgb_preview = !preview_stream_fresh;
                    thumbnail_preview_requested = thumbnail_requested;
                    thumbnail_preview_expired = is_older_than_us(media_now, cam->rgb_preview_requested_until_us, kPreviewDecoderIdleStopUs);
                    is_main_preview_camera = cam->key == main_preview_key_;
                    main_preview_requested = is_main_preview_camera && is_recent_us(media_now, cam->main_rgb_preview_requested_until_us, 0);
                    main_preview_expired = is_older_than_us(media_now, cam->main_rgb_preview_requested_until_us, kMainPreviewDecoderIdleStopUs);
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
                    const bool thumbnail_requested = is_recent_us(media_now, cam->rgb_preview_requested_until_us, 0);
                    if(stream_requested) {
                        rgb_stream_update = &cam->rgb_preview_stream;
                    }
                    update_rgb_preview = true;
                    thumbnail_preview_requested = thumbnail_requested;
                    thumbnail_preview_expired = is_older_than_us(media_now, cam->rgb_preview_requested_until_us, kPreviewDecoderIdleStopUs);
                    is_main_preview_camera = cam->key == main_preview_key_;
                    main_preview_requested = is_main_preview_camera && is_recent_us(media_now, cam->main_rgb_preview_requested_until_us, 0);
                    main_preview_expired = is_older_than_us(media_now, cam->main_rgb_preview_requested_until_us, kMainPreviewDecoderIdleStopUs);
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
                {
                    std::lock_guard<std::mutex> record_lock(cam->record_mutex);
                    if(!cam->record_accepting) {
                        should_record = false;
                    }
                    else {
                        record_generation = cam->record_generation;
                    }
                }
            }
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
        std::shared_ptr<MediaPacket> packet_owner;
        MediaPacket *processing_packet = &packet;
        if(should_record) {
            packet_owner = std::make_shared<MediaPacket>(std::move(packet));
            processing_packet = packet_owner.get();

            RecordJob job;
            job.packet = packet_owner;
            job.sender_id = std::move(record_sender_id);
            job.camera_id = std::move(record_camera_id);
            job.camera_name = std::move(record_camera_name);
            job.storage_key = std::move(record_storage_key);
            job.file_prefix = std::move(record_file_prefix);
            job.announce_json = std::move(record_announce_json);
            job.record_generation = record_generation;
            job.media_session_id = media_session_id;
            job.media_ingress_key = media_ingress_key(*processing_packet);
            enqueue_record_job(cam, std::move(job));
        }
        const MediaPacket &media_packet = *processing_packet;

        if(rgb_stream_update) {
            update_h264_stream_buffer_locked(*rgb_stream_update, media_packet, rgb_has_idr, rgb_has_vcl);
        }
        if(update_rgb_preview) {
            std::lock_guard<std::mutex> preview_lock(cam->preview_mutex);
            update_rgb_preview_locked(*cam, media_packet, rgb_has_idr, rgb_has_vcl,
                                      thumbnail_preview_requested, thumbnail_preview_expired,
                                      main_preview_requested, main_preview_expired, is_main_preview_camera);
        }
        if(!web_preview_control_targets.empty()) {
            send_web_rgb_preview_controls(web_preview_control_targets, web_preview_control_request_us);
        }

        if(build_depth_preview) {
            std::optional<MediaPacket> preview_depth_packet;
            const MediaPacket *depth_packet = &media_packet;
            if(media_packet.stream_type == StreamType::depth_raw && media_packet.codec_or_compression != "none") {
                try {
                    preview_depth_packet = normalized_depth_packet(media_packet);
                    depth_packet = &*preview_depth_packet;
                }
                catch(const std::exception &e) {
                    logger_.warn(std::string("depth preview packet ignored camera=") + media_packet.sender_id + "_" + media_packet.camera_id
                                 + " frame=" + std::to_string(media_packet.frame_id) + ": " + e.what());
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
                    std::lock_guard<std::mutex> preview_lock(cam->preview_mutex);
                    cam->depth_preview_ppm = std::move(preview.bytes);
                    cam->depth_preview_width = preview.width;
                    cam->depth_preview_height = preview.height;
                    cam->depth_preview_us = depth_preview_media_us;
                }
            }
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
        size_t allocated_bytes = 0;
        for(const auto &item : preview_udp_assemblies_) {
            allocated_bytes += item.second.bytes.size();
        }
        while(preview_udp_assemblies_.size() > kPreviewUdpMaxAssemblies || allocated_bytes > kPreviewUdpMaxAssemblyBytes) {
            auto oldest = preview_udp_assemblies_.begin();
            for(auto it = preview_udp_assemblies_.begin(); it != preview_udp_assemblies_.end(); ++it) {
                if(it->second.updated_us < oldest->second.updated_us) {
                    oldest = it;
                }
            }
            allocated_bytes -= std::min(allocated_bytes, oldest->second.bytes.size());
            account_incomplete_udp_assembly_locked(oldest->second, true);
            preview_udp_assemblies_.erase(oldest);
        }
    }

    void reserve_udp_assembly_budget_locked(size_t requested_bytes) {
        size_t allocated_bytes = 0;
        for(const auto &item : preview_udp_assemblies_) {
            allocated_bytes += item.second.bytes.size();
        }
        while(!preview_udp_assemblies_.empty()
              && (preview_udp_assemblies_.size() >= kPreviewUdpMaxAssemblies
                  || allocated_bytes > kPreviewUdpMaxAssemblyBytes - requested_bytes)) {
            auto oldest = preview_udp_assemblies_.begin();
            for(auto it = preview_udp_assemblies_.begin(); it != preview_udp_assemblies_.end(); ++it) {
                if(it->second.updated_us < oldest->second.updated_us) {
                    oldest = it;
                }
            }
            allocated_bytes -= std::min(allocated_bytes, oldest->second.bytes.size());
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
        const size_t udp_packet_limit = std::min(config_.max_payload_bytes, kPreviewUdpMaxPacketBytes);
        if(chunk_count == 0 || chunk_count > kPreviewUdpMaxChunks || chunk_count > total_size || chunk_index >= chunk_count || total_size == 0
           || total_size > udp_packet_limit
           || chunk_size == 0 || chunk_offset > total_size || chunk_size > total_size - chunk_offset
           || size != header_size + chunk_size) {
            record_udp_invalid_datagram(media_udp);
            return;
        }

        std::vector<uint8_t> completed;
        const uint64_t now = now_us();
        const std::string key = std::string(media_udp ? "media#" : "preview#") + peer_endpoint + "#" + std::to_string(sequence);
        {
            std::lock_guard<std::mutex> lock(preview_udp_mutex_);
            cleanup_preview_udp_assemblies_locked(now);
            auto &stats = udp_stats_locked(media_udp);
            stats.valid_fragments++;
            auto existing = preview_udp_assemblies_.find(key);
            if(existing == preview_udp_assemblies_.end()) {
                reserve_udp_assembly_budget_locked(total_size);
                existing = preview_udp_assemblies_.emplace(key, PreviewUdpAssembly{}).first;
            }
            auto &assembly = existing->second;
            if(assembly.bytes.size() != total_size || assembly.chunk_count != chunk_count || assembly.media_udp != media_udp) {
                if(!assembly.bytes.empty()) {
                    account_incomplete_udp_assembly_locked(assembly, true);
                }
                assembly.bytes.assign(total_size, 0);
                assembly.received.assign(chunk_count, 0);
                assembly.chunk_offsets.assign(chunk_count, 0);
                assembly.chunk_sizes.assign(chunk_count, 0);
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
                assembly.chunk_offsets[chunk_index] = chunk_offset;
                assembly.chunk_sizes[chunk_index] = chunk_size;
                assembly.received_count++;
            }
            else {
                stats.duplicate_fragments++;
            }
            if(assembly.received_count == assembly.chunk_count) {
                uint64_t expected_offset = 0;
                bool layout_valid = true;
                for(size_t i = 0; i < assembly.chunk_count; ++i) {
                    if(assembly.chunk_offsets[i] != expected_offset || assembly.chunk_sizes[i] == 0) {
                        layout_valid = false;
                        break;
                    }
                    expected_offset += assembly.chunk_sizes[i];
                }
                if(!layout_valid || expected_offset != assembly.total_size) {
                    stats.invalid_datagrams++;
                    account_incomplete_udp_assembly_locked(assembly, true);
                    preview_udp_assemblies_.erase(key);
                    return;
                }
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
                handle_media_packet(std::move(packet), peer_endpoint);
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
            listener_start_failed_ = true;
            logger_.error(std::string("cannot create UDP socket: ") + std::strerror(errno));
            return;
        }
        set_fd_cloexec(fd);
        int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        set_socket_timeout(fd, 1);
        const auto addr = make_bind_addr(config_.status_bind_ip, config_.status_port);
        if(bind(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
            listener_start_failed_ = true;
            logger_.error(std::string("cannot bind status UDP: ") + std::strerror(errno));
            close(fd);
            return;
        }
        status_udp_ready_ = true;

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
            try {
                handle_status_message(std::string(buffer.data(), static_cast<size_t>(got)), socket_endpoint(peer));
            }
            catch(const std::exception &e) {
                logger_.warn(std::string("status UDP packet rejected: ") + e.what());
            }
        }
        close(fd);
    }

    void preview_udp_loop() {
        const int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if(fd < 0) {
            listener_start_failed_ = true;
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
            listener_start_failed_ = true;
            logger_.error(std::string("cannot bind preview UDP: ") + std::strerror(errno));
            close(fd);
            return;
        }
        preview_udp_ready_ = true;
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
            try {
                handle_fragmented_udp_datagram(buffer.data(), static_cast<size_t>(got), socket_endpoint(peer), false);
            }
            catch(const std::exception &e) {
                logger_.warn(std::string("preview UDP datagram rejected: ") + e.what());
            }
        }
        close(fd);
    }

    void media_udp_loop() {
        const int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if(fd < 0) {
            listener_start_failed_ = true;
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
            listener_start_failed_ = true;
            logger_.error(std::string("cannot bind media UDP: ") + std::strerror(errno));
            close(fd);
            return;
        }
        media_udp_ready_ = true;
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
            try {
                handle_fragmented_udp_datagram(buffer.data(), static_cast<size_t>(got), socket_endpoint(peer), true);
            }
            catch(const std::exception &e) {
                logger_.warn(std::string("media UDP datagram rejected: ") + e.what());
            }
        }
    }

    void tcp_loop() {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        if(fd < 0) {
            listener_start_failed_ = true;
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
            listener_start_failed_ = true;
            logger_.error(std::string("cannot bind media TCP: ") + std::strerror(errno));
            close(fd);
            return;
        }
        if(listen(fd, 16) != 0) {
            listener_start_failed_ = true;
            logger_.error(std::string("cannot listen media TCP: ") + std::strerror(errno));
            close(fd);
            return;
        }
        media_tcp_ready_ = true;

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
            const uint64_t media_session_id = next_media_session_id_.fetch_add(1);
            try {
                launch_client_thread(client, "media", [this, client, peer_endpoint, media_session_id] {
                    try {
                        media_client_loop(client, peer_endpoint, media_session_id);
                    }
                    catch(...) {
                        active_media_clients_.fetch_sub(1);
                        throw;
                    }
                    active_media_clients_.fetch_sub(1);
                });
            }
            catch(const std::exception &e) {
                active_media_clients_.fetch_sub(1);
                logger_.warn(std::string("cannot start media client thread: ") + e.what());
            }
        }
        close(fd);
    }

    void media_client_loop(int fd, const std::string &peer_endpoint, uint64_t media_session_id) {
        ScopeExit unregister_session([this, media_session_id] { mark_media_ingress_session_closed(media_session_id); });
        logger_.info("media client connected from=" + peer_endpoint + " session=" + std::to_string(media_session_id));
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
                handle_media_packet(std::move(packet), peer_endpoint, media_session_id, fd);
            }
            catch(const std::exception &e) {
                std::ostringstream msg;
                msg << "media client disconnected from=" << peer_endpoint << " session=" << media_session_id
                    << " last_sender=" << last_sender << " last_camera=" << last_camera
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
    }

    void admin_loop() {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        if(fd < 0) {
            listener_start_failed_ = true;
            logger_.error(std::string("cannot create admin socket: ") + std::strerror(errno));
            return;
        }
        set_fd_cloexec(fd);
        int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        set_socket_timeout(fd, 1);
        const auto addr = make_bind_addr(config_.admin_bind_ip, config_.admin_port);
        if(bind(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
            listener_start_failed_ = true;
            logger_.error(std::string("cannot bind admin HTTP: ") + std::strerror(errno));
            close(fd);
            return;
        }
        if(listen(fd, 16) != 0) {
            listener_start_failed_ = true;
            logger_.error(std::string("cannot listen admin HTTP: ") + std::strerror(errno));
            close(fd);
            return;
        }
        admin_ready_ = true;

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
            const int previous_clients = active_admin_clients_.fetch_add(1);
            if(previous_clients >= kMaxActiveAdminClients) {
                active_admin_clients_.fetch_sub(1);
                close(client);
                continue;
            }
            try {
                launch_client_thread(client, "admin", [this, client] {
                    try {
                        handle_admin_client(client);
                    }
                    catch(...) {
                        active_admin_clients_.fetch_sub(1);
                        throw;
                    }
                    active_admin_clients_.fetch_sub(1);
                });
            }
            catch(const std::exception &e) {
                active_admin_clients_.fetch_sub(1);
                logger_.warn(std::string("cannot start admin client thread: ") + e.what());
            }
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
        const bool invalid_sender_id = args.count("sender_id") && !is_valid_protocol_id(args.at("sender_id"));
        const bool invalid_camera_id = args.count("camera_id") && !is_valid_protocol_id(args.at("camera_id"));
        if(invalid_sender_id || invalid_camera_id) {
            status = 400;
            body = "{\"ok\":false,\"error\":\"invalid sender_id/camera_id\"}";
        }
        else if(method == "GET" && path == "/api/status") {
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
        const char *reason = status == 200 ? "OK" : (status == 400 ? "Bad Request" : "Not Found");
        response << "HTTP/1.1 " << status << ' ' << reason << "\r\n";
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
    std::atomic<bool> started_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> listener_start_failed_{false};
    std::atomic<bool> status_udp_ready_{false};
    std::atomic<bool> media_tcp_ready_{false};
    std::atomic<bool> media_udp_ready_{false};
    std::atomic<bool> preview_udp_ready_{false};
    std::atomic<bool> admin_ready_{false};
    std::atomic<int> active_media_clients_{0};
    std::atomic<int> active_admin_clients_{0};
    std::atomic<size_t> total_record_queue_bytes_{0};
    std::atomic<uint64_t> next_media_session_id_{1};
    std::atomic<uint64_t> media_ingress_superseded_sessions_{0};
    std::atomic<uint64_t> media_ingress_stale_packets_{0};
    std::thread udp_thread_;
    std::thread media_udp_thread_;
    std::thread preview_udp_thread_;
    std::thread tcp_thread_;
    std::thread admin_thread_;
    std::thread recording_maintenance_thread_;
    std::thread segment_finalize_worker_;
    std::mutex mutex_;
    std::mutex segment_close_futures_mutex_;
    std::mutex segment_finalize_mutex_;
    std::mutex media_ingress_mutex_;
    std::mutex runtime_state_save_mutex_;
    std::mutex preview_udp_mutex_;
    std::mutex client_threads_mutex_;
    std::mutex decoder_cleanup_mutex_;
    std::mutex status_cache_mutex_;
    std::mutex recording_maintenance_mutex_;
    std::mutex uploader_status_mutex_;
    std::condition_variable decoder_cleanup_cv_;
    std::condition_variable segment_finalize_cv_;
    std::condition_variable recording_maintenance_cv_;
    bool decoder_cleanup_running_ = false;
    bool segment_finalize_worker_running_ = false;
    bool segment_finalize_worker_stop_ = false;
    size_t segment_finalize_outstanding_ = 0;
    uint64_t runtime_state_revision_ = 0;
    uint64_t runtime_state_save_revision_ = 0;
    std::string main_preview_key_;
    bool recording_all_ = false;
    uint64_t recording_all_start_us_ = 0;
    bool recording_all_has_file_prefix_override_ = false;
    std::string recording_all_file_prefix_;
    std::map<std::string, std::shared_ptr<CameraState>> cameras_;
    std::vector<std::future<void>> segment_close_futures_;
    std::deque<SegmentFinalizeTask> segment_finalize_queue_;
    std::atomic<size_t> segment_finalize_outstanding_status_{0};
    std::atomic<size_t> segment_finalize_queued_status_{0};
    std::atomic<size_t> segment_finalize_active_status_{0};
    std::atomic<uint64_t> segment_finalize_completed_total_{0};
    std::atomic<uint64_t> segment_finalize_failures_total_{0};
    std::vector<ClientThread> client_threads_;
    std::deque<std::unique_ptr<RgbPreviewDecoder>> decoder_cleanup_queue_;
    std::set<int> client_fds_;
    std::string status_cache_;
    std::string uploader_status_json_;
    std::unordered_map<std::string, PreviewUdpAssembly> preview_udp_assemblies_;
    std::unordered_map<std::string, MediaIngressOwner> media_ingress_owners_;
    std::unordered_map<std::string, std::string> sender_source_ips_;
    UdpReassemblyStats media_udp_stats_;
    UdpReassemblyStats preview_udp_stats_;
    std::thread decoder_cleanup_thread_;
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
