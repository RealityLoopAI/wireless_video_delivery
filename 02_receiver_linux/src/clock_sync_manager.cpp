#include "gwv3_receiver/clock_sync_manager.hpp"

#include "gwv3_common/protocol.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <json/json.h>

namespace gwv3 {
namespace {

uint64_t now_us() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
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

bool is_valid_protocol_id(const std::string &value) {
    if(value.empty() || value.size() > 64) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isalnum(ch) || ch == '_' || ch == '-';
    });
}

sockaddr_in make_bind_addr(const std::string &ip, uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if(ip.empty() || ip == "0.0.0.0") {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    }
    else if(inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        throw std::runtime_error("invalid clock sync bind ip: " + ip);
    }
    return addr;
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

void set_socket_timeout(int fd, int seconds) {
    timeval tv{};
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

}  // namespace

ClockSyncManager::ClockSyncManager(ClockSyncManagerConfig config) : config_(std::move(config)) {
    config_.model_timeout_ms = std::max(1, config_.model_timeout_ms);
}

ClockSyncManager::~ClockSyncManager() {
    stop();
}

void ClockSyncManager::set_log_callbacks(std::function<void(const std::string &)> info,
                                         std::function<void(const std::string &)> warn) {
    std::lock_guard<std::mutex> lock(log_mutex_);
    info_log_ = std::move(info);
    warn_log_ = std::move(warn);
}

bool ClockSyncManager::start() {
    if(!config_.enabled) {
        return true;
    }
    bool expected = false;
    if(!running_.compare_exchange_strong(expected, true)) {
        return true;
    }
    try {
        thread_ = std::thread([this] {
            try {
                run_loop();
            }
            catch(const std::exception &e) {
                log_warn(std::string("clock_sync listener stopped after exception: ") + e.what());
                running_ = false;
            }
            catch(...) {
                log_warn("clock_sync listener stopped after unknown exception");
                running_ = false;
            }
        });
    }
    catch(const std::exception &e) {
        running_ = false;
        log_warn(std::string("clock_sync listener thread start failed: ") + e.what());
        return false;
    }
    return true;
}

void ClockSyncManager::stop() {
    running_ = false;
    if(thread_.joinable()) {
        thread_.join();
    }
}

bool ClockSyncManager::update_from_sender_report(const std::string &sender_id,
                                                 int64_t offset_us,
                                                 int64_t delay_us,
                                                 double drift_ppm,
                                                 uint64_t last_sync_us,
                                                 const std::string &source_ip) {
    constexpr int64_t kMaxAcceptedOffsetUs = 24ll * 60ll * 60ll * 1000ll * 1000ll;
    in_addr source_addr{};
    if(!is_valid_protocol_id(sender_id) || inet_pton(AF_INET, source_ip.c_str(), &source_addr) != 1
       || delay_us < 0 || delay_us > 100000 || !std::isfinite(drift_ppm) || std::abs(drift_ppm) > 1000.0
       || offset_us < -kMaxAcceptedOffsetUs || offset_us > kMaxAcceptedOffsetUs
       || last_sync_us > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        return false;
    }
    ClockModel model;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto source = probe_source_ips_.find(sender_id);
        if(source == probe_source_ips_.end() || source->second != source_addr.s_addr) {
            return false;
        }
        auto &entry = clock_models_[sender_id];
        if(last_sync_us > 0 && entry.last_sync_us > 0 && last_sync_us < entry.last_sync_us) {
            return false;
        }
        entry.valid = last_sync_us > 0;
        entry.offset_us = offset_us;
        entry.delay_us = delay_us;
        entry.drift_ppm = drift_ppm;
        entry.last_sync_us = last_sync_us;
        entry.last_update_receiver_us = now_us();
        entry.sample_count++;
        model = entry;
    }

    if(model.sample_count == 1 || model.sample_count % 10 == 0) {
        std::ostringstream oss;
        oss << "clock_sync updated sender_id=" << sender_id
            << " offset_us=" << model.offset_us
            << " delay_us=" << model.delay_us
            << " drift_ppm=" << model.drift_ppm
            << " samples=" << model.sample_count
            << " valid=" << (model.valid ? "true" : "false");
        log_info(oss.str());
    }
    return true;
}

ClockModel ClockSyncManager::get_model(const std::string &sender_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = clock_models_.find(sender_id);
    if(it == clock_models_.end()) {
        return {};
    }
    return apply_timeout_locked(it->second, now_us());
}

int64_t ClockSyncManager::get_global_timestamp_us(const std::string &sender_id, uint64_t sender_timestamp_us) const {
    const auto model = get_model(sender_id);
    if(!model.valid) {
        return static_cast<int64_t>(sender_timestamp_us);
    }
    long double adjusted_offset = static_cast<long double>(model.offset_us);
    if(model.last_sync_us > 0 && std::isfinite(model.drift_ppm)) {
        const long double elapsed_us = static_cast<long double>(sender_timestamp_us)
                                       - static_cast<long double>(model.last_sync_us);
        adjusted_offset += elapsed_us * model.drift_ppm / 1'000'000.0L;
    }
    const long double global = static_cast<long double>(sender_timestamp_us) + adjusted_offset;
    if(global > static_cast<long double>(std::numeric_limits<int64_t>::max())) {
        return std::numeric_limits<int64_t>::max();
    }
    if(global < static_cast<long double>(std::numeric_limits<int64_t>::min())) {
        return std::numeric_limits<int64_t>::min();
    }
    return static_cast<int64_t>(std::llround(global));
}

bool ClockSyncManager::has_valid_model(const std::string &sender_id) const {
    return get_model(sender_id).valid;
}

std::vector<std::pair<std::string, ClockModel>> ClockSyncManager::models() const {
    std::vector<std::pair<std::string, ClockModel>> result;
    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t now = now_us();
    result.reserve(clock_models_.size());
    for(const auto &item : clock_models_) {
        result.emplace_back(item.first, apply_timeout_locked(item.second, now));
    }
    std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    return result;
}

void ClockSyncManager::run_loop() {
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if(fd < 0) {
        log_warn(std::string("cannot create clock sync UDP socket: ") + std::strerror(errno));
        running_ = false;
        return;
    }
    set_fd_cloexec(fd);
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    set_socket_timeout(fd, 1);

    try {
        const auto addr = make_bind_addr(config_.bind_ip, config_.port);
        if(bind(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
            log_warn(std::string("cannot bind clock sync UDP: ") + std::strerror(errno));
            close(fd);
            running_ = false;
            return;
        }
    }
    catch(const std::exception &e) {
        log_warn(e.what());
        close(fd);
        running_ = false;
        return;
    }

    log_info("clock_sync listening on " + config_.bind_ip + ":" + std::to_string(config_.port));

    std::array<char, 4096> buffer{};
    while(running_) {
        sockaddr_in peer{};
        socklen_t peer_len = sizeof(peer);
        const ssize_t got = recvfrom(fd, buffer.data(), buffer.size(), 0, reinterpret_cast<sockaddr *>(&peer), &peer_len);
        if(got < 0) {
            if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            log_warn(std::string("clock sync UDP recv failed: ") + std::strerror(errno));
            continue;
        }
        try {
            handle_datagram(fd, buffer.data(), static_cast<size_t>(got), peer);
        }
        catch(const std::exception &e) {
            log_warn(std::string("clock sync datagram rejected: ") + e.what());
        }
    }
    close(fd);
}

void ClockSyncManager::handle_datagram(int fd, const char *data, size_t size, const sockaddr_in &peer) {
    if(size == 0 || size > 4096) {
        return;
    }
    const uint64_t t2 = now_us();
    Json::CharReaderBuilder builder;
    builder["collectComments"] = false;
    builder["failIfExtra"] = true;
    builder["strictRoot"] = true;
    Json::Value root;
    std::string errors;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if(!reader->parse(data, data + size, &root, &errors) || !root.isObject()
       || !root["protocol_version"].isString() || root["protocol_version"].asString() != kProtocolVersion
       || !root["message_type"].isString() || root["message_type"].asString() != "clock_sync_probe") {
        return;
    }
    const auto sender_id = root["sender_id"].isString() ? root["sender_id"].asString() : std::string{};
    if(!is_valid_protocol_id(sender_id) || !root["sequence"].isUInt64() || !root["t1_sender_send_us"].isUInt64()) {
        return;
    }
    const uint64_t sequence = root["sequence"].asUInt64();
    const uint64_t t1 = root["t1_sender_send_us"].asUInt64();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        probe_source_ips_[sender_id] = peer.sin_addr.s_addr;
    }

    const uint64_t t3 = now_us();
    std::ostringstream response;
    response << "{\"protocol_version\":\"" << kProtocolVersion << "\","
             << "\"message_type\":\"clock_sync_response\","
             << "\"sender_id\":\"" << json_escape(sender_id) << "\","
             << "\"sequence\":" << sequence << ','
             << "\"t1_sender_send_us\":" << t1 << ','
             << "\"t2_receiver_recv_us\":" << t2 << ','
             << "\"t3_receiver_send_us\":" << t3 << "}\n";
    const auto payload = response.str();
    const auto sent = sendto(fd, payload.data(), payload.size(), 0, reinterpret_cast<const sockaddr *>(&peer), sizeof(peer));
    if(sent < 0 || static_cast<size_t>(sent) != payload.size()) {
        log_warn("clock_sync response send failed sender_id=" + sender_id + ": " + std::strerror(errno));
        return;
    }
    log_info("clock_sync response sent sender_id=" + sender_id + " sequence=" + std::to_string(sequence));
}

ClockModel ClockSyncManager::apply_timeout_locked(const ClockModel &model, uint64_t now) const {
    ClockModel out = model;
    const uint64_t timeout_us = static_cast<uint64_t>(config_.model_timeout_ms) * 1000ull;
    if(out.valid && out.last_update_receiver_us > 0 && out.last_update_receiver_us < now
       && now - out.last_update_receiver_us > timeout_us) {
        out.valid = false;
    }
    return out;
}

void ClockSyncManager::log_info(const std::string &message) const {
    std::function<void(const std::string &)> callback;
    {
        std::lock_guard<std::mutex> lock(log_mutex_);
        callback = info_log_;
    }
    if(callback) {
        callback(message);
    }
}

void ClockSyncManager::log_warn(const std::string &message) const {
    std::function<void(const std::string &)> callback;
    {
        std::lock_guard<std::mutex> lock(log_mutex_);
        callback = warn_log_;
    }
    if(callback) {
        callback(message);
    }
}

}  // namespace gwv3
