#include "gwv3_receiver/clock_sync_manager.hpp"

#include "gwv3_common/protocol.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

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

std::optional<std::string> json_string_field(const std::string &json, const std::string &key) {
    const std::regex pattern("\"" + key + "\"\\s*:\\s*\"([^\"]*)\"");
    std::smatch match;
    if(std::regex_search(json, match, pattern) && match.size() >= 2) {
        return match[1].str();
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
    thread_ = std::thread([this] { run_loop(); });
    return true;
}

void ClockSyncManager::stop() {
    running_ = false;
    if(thread_.joinable()) {
        thread_.join();
    }
}

void ClockSyncManager::update_from_sender_report(const std::string &sender_id,
                                                 int64_t offset_us,
                                                 int64_t delay_us,
                                                 double drift_ppm,
                                                 uint64_t last_sync_us) {
    if(sender_id.empty()) {
        return;
    }
    ClockModel model;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto &entry = clock_models_[sender_id];
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
    return static_cast<int64_t>(sender_timestamp_us) + model.offset_us;
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
        handle_datagram(fd, buffer.data(), static_cast<size_t>(got), peer);
    }
    close(fd);
}

void ClockSyncManager::handle_datagram(int fd, const char *data, size_t size, const sockaddr_in &peer) {
    const uint64_t t2 = now_us();
    const std::string json(data, size);
    if(json_string_field(json, "protocol_version").value_or("") != kProtocolVersion
       || json_string_field(json, "message_type").value_or("") != "clock_sync_probe") {
        return;
    }
    const auto sender_id = json_string_field(json, "sender_id").value_or("");
    const auto sequence = json_uint64_field(json, "sequence");
    const auto t1 = json_uint64_field(json, "t1_sender_send_us");
    if(sender_id.empty() || !sequence || !t1) {
        return;
    }

    const uint64_t t3 = now_us();
    std::ostringstream response;
    response << "{\"protocol_version\":\"" << kProtocolVersion << "\","
             << "\"message_type\":\"clock_sync_response\","
             << "\"sender_id\":\"" << json_escape(sender_id) << "\","
             << "\"sequence\":" << *sequence << ','
             << "\"t1_sender_send_us\":" << *t1 << ','
             << "\"t2_receiver_recv_us\":" << t2 << ','
             << "\"t3_receiver_send_us\":" << t3 << "}\n";
    const auto payload = response.str();
    const auto sent = sendto(fd, payload.data(), payload.size(), 0, reinterpret_cast<const sockaddr *>(&peer), sizeof(peer));
    if(sent < 0 || static_cast<size_t>(sent) != payload.size()) {
        log_warn("clock_sync response send failed sender_id=" + sender_id + ": " + std::strerror(errno));
        return;
    }
    log_info("clock_sync response sent sender_id=" + sender_id + " sequence=" + std::to_string(*sequence));
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
