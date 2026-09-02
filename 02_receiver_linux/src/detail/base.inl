constexpr size_t kMediaHeaderBaseSize = kMediaHeaderV1Size;
constexpr size_t kMediaHeaderMaxSize = kMediaHeaderV2Size;
constexpr size_t kMaxReasonablePayload = 128ull * 1024ull * 1024ull;
constexpr size_t kMaxRgbPreviewPrefixBytes = 512ull * 1024ull;
constexpr uint32_t kRgbPreviewWidth = 320;
constexpr uint32_t kRgbMainPreviewWidth = 960;
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
constexpr size_t kRgbH264ClientMaxLagPackets = 12;
constexpr int kRgbH264ClientSendTimeoutMs = 150;
constexpr int kRgbH264ClientSendBufferBytes = 32 * 1024;
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
constexpr int kMediaListenBacklog = 128;
constexpr int kAdminListenBacklog = 64;
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
constexpr size_t kMaxCodecNameBytes = 128;
constexpr uint64_t kAnnounceCacheSaveMinIntervalUs = 60ull * 1000ull * 1000ull;
constexpr uint64_t kRoutineStatusLogMinIntervalUs = 60ull * 1000ull * 1000ull;
constexpr uint64_t kMaxGlobalTimestampReceiverSkewUs = 10ull * 60ull * 1000ull * 1000ull;
constexpr const char *kRgbMp4RecordMuxFlags = "+empty_moov+default_base_moof";
constexpr uint64_t kRgbMp4FragmentDurationUs = 1'000'000ull;
constexpr uint64_t kSegmentRotationKeyframeRetryUs = 1ull * 1000ull * 1000ull;
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
    case StreamType::rgb_snapshot:
        return "rgb_snapshot";
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

bool camera_announce_expects_rgb(const std::string &announce_json) {
    if(announce_json.empty()) {
        return true;
    }
    Json::Value root;
    if(!parse_json_object_strict(announce_json, root) || !root["rgb_profile"].isObject()) {
        return true;
    }
    const auto &rgb_profile = root["rgb_profile"];
    return !rgb_profile["enabled"].isBool() || rgb_profile["enabled"].asBool();
}

bool is_recovered_media_transport_error(const std::string &error) {
    return error.rfind("media TCP ", 0) == 0 || error.rfind("media transport ", 0) == 0
           || error == "unknown media transport error";
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

bool send_all_with_timeout(int fd, const void *data, size_t size, int timeout_ms) {
    const auto *bytes = static_cast<const uint8_t *>(data);
    size_t offset = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while(offset < size) {
        const ssize_t sent = send(fd, bytes + offset, size - offset, MSG_NOSIGNAL | MSG_DONTWAIT);
        if(sent > 0) {
            offset += static_cast<size_t>(sent);
            continue;
        }
        if(sent < 0 && errno == EINTR) {
            continue;
        }
        if(sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if(remaining.count() <= 0 || !wait_fd_writable(fd, static_cast<int>(remaining.count()))) {
                return false;
            }
            continue;
        }
        return false;
    }
    return true;
}

void configure_rgb_h264_client_socket(int fd) {
    const int enabled = 1;
    const int send_buffer_bytes = kRgbH264ClientSendBufferBytes;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &send_buffer_bytes, sizeof(send_buffer_bytes));
#ifdef TCP_NOTSENT_LOWAT
    const int notsent_lowat_bytes = kRgbH264ClientSendBufferBytes / 2;
    setsockopt(fd, IPPROTO_TCP, TCP_NOTSENT_LOWAT, &notsent_lowat_bytes, sizeof(notsent_lowat_bytes));
#endif
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

void fsync_directory_best_effort(const std::filesystem::path &path) {
    const int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if(fd < 0) {
        return;
    }
    while(fsync(fd) != 0 && errno == EINTR) {
    }
    close(fd);
}

void fsync_file_strict(const std::filesystem::path &path) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if(fd < 0) {
        throw std::runtime_error("cannot open file for fsync " + path.string() + ": " + std::strerror(errno));
    }
    ScopeExit close_file([&] { close(fd); });
    while(fsync(fd) != 0) {
        if(errno == EINTR) {
            continue;
        }
        throw std::runtime_error("cannot fsync file " + path.string() + ": " + std::strerror(errno));
    }
}

bool is_cifs_delete_pending_file(const std::filesystem::path &path) {
    return path.filename().string().rfind(".__smb", 0) == 0;
}

void fsync_segment_files_strict(const std::filesystem::path &directory) {
    std::error_code ec;
    for(const auto &entry : std::filesystem::directory_iterator(directory, ec)) {
        if(ec) {
            break;
        }
        // CIFS creates .__smb* placeholders when a deleted file is still open
        // by another process. They can disappear between enumeration and open
        // and are not part of the recording artifact set.
        if(is_cifs_delete_pending_file(entry.path())) {
            continue;
        }
        if(entry.is_regular_file(ec)) {
            if(ec) {
                break;
            }
            fsync_file_strict(entry.path());
        }
    }
    if(ec) {
        throw std::runtime_error("cannot enumerate segment for fsync " + directory.string() + ": " + ec.message());
    }
    fsync_directory_best_effort(directory);
}

bool paths_share_device(const std::filesystem::path &left, const std::filesystem::path &right) {
    struct stat left_stat{};
    struct stat right_stat{};
    return stat(left.c_str(), &left_stat) == 0 && stat(right.c_str(), &right_stat) == 0
           && left_stat.st_dev == right_stat.st_dev;
}

void write_file_and_fsync(const std::filesystem::path &path, const uint8_t *data, size_t size) {
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0640);
    if(fd < 0) {
        throw std::runtime_error("cannot open staged photo file " + path.string() + ": " + std::strerror(errno));
    }
    ScopeExit close_file([&] { close(fd); });
    size_t offset = 0;
    while(offset < size) {
        const ssize_t written = write(fd, data + offset, size - offset);
        if(written < 0) {
            if(errno == EINTR) {
                continue;
            }
            throw std::runtime_error("cannot write staged photo file " + path.string() + ": " + std::strerror(errno));
        }
        if(written == 0) {
            throw std::runtime_error("short write while staging photo " + path.string());
        }
        offset += static_cast<size_t>(written);
    }
    while(fsync(fd) != 0) {
        if(errno == EINTR) {
            continue;
        }
        throw std::runtime_error("cannot fsync staged photo file " + path.string() + ": " + std::strerror(errno));
    }
}

void write_text_file_and_fsync(const std::filesystem::path &path, const std::string &text) {
    write_file_and_fsync(path, reinterpret_cast<const uint8_t *>(text.data()), text.size());
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
        return std::isalnum(ch) || ch == '_' || ch == '-' || ch == '.' || ch == ';' || ch == '=';
    });
}

std::optional<std::string> rgb_snapshot_request_id(const std::string &codec) {
    const std::string prefix = kRgbSnapshotCodecPrefix;
    if(codec.rfind(prefix, 0) != 0) {
        return std::nullopt;
    }
    const std::string request_id = codec.substr(prefix.size());
    if(request_id.empty() || request_id.size() > 96
       || !std::all_of(request_id.begin(), request_id.end(), [](unsigned char ch) {
              return std::isalnum(ch) || ch == '_' || ch == '-' || ch == '.';
          })) {
        return std::nullopt;
    }
    return request_id;
}

struct RgbSnapshotBurstInfo {
    std::string group_id;
    uint32_t index = 0;
    uint32_t count = 0;
};

std::optional<RgbSnapshotBurstInfo> rgb_snapshot_burst_info(const std::string &request_id) {
    constexpr size_t kSuffixBytes = 7;
    if(request_id.size() <= kSuffixBytes) {
        return std::nullopt;
    }
    const size_t suffix = request_id.size() - kSuffixBytes;
    const auto is_digit = [&](size_t offset) {
        return std::isdigit(static_cast<unsigned char>(request_id[suffix + offset])) != 0;
    };
    if(request_id[suffix] != '_' || !is_digit(1) || !is_digit(2)
       || request_id[suffix + 3] != 'o' || request_id[suffix + 4] != 'f'
       || !is_digit(5) || !is_digit(6)) {
        return std::nullopt;
    }
    const uint32_t index = static_cast<uint32_t>((request_id[suffix + 1] - '0') * 10
                                                  + (request_id[suffix + 2] - '0'));
    const uint32_t count = static_cast<uint32_t>((request_id[suffix + 5] - '0') * 10
                                                  + (request_id[suffix + 6] - '0'));
    if(count < 2 || index == 0 || index > count) {
        return std::nullopt;
    }
    return RgbSnapshotBurstInfo{request_id.substr(0, suffix), index, count};
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

bool h264_payload_can_start_segment(const std::vector<uint8_t> &payload) {
    return h264_decodable_start_offset(payload).has_value();
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

