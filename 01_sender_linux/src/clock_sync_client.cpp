#include "gwv3_sender/clock_sync_client.hpp"
#include "gwv3_sender/network_utils.hpp"

#include "gwv3_common/protocol.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>

#include <arpa/inet.h>
#include <fcntl.h>
#include <json/json.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

namespace gwv3 {
namespace {

uint64_t now_us() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

uint64_t max_sync_age_us(int interval_ms) {
    return static_cast<uint64_t>(std::max(10000, interval_ms * 5)) * 1000ull;
}

uint64_t absolute_difference(uint64_t lhs, uint64_t rhs) {
    return lhs >= rhs ? lhs - rhs : rhs - lhs;
}

std::string json_to_string(const Json::Value &value) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, value);
}

bool parse_json_object(const std::string &payload, Json::Value &root) {
    Json::CharReaderBuilder builder;
    builder["collectComments"] = false;
    builder["failIfExtra"] = true;
    builder["strictRoot"] = true;
    std::string errors;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    return reader->parse(payload.data(), payload.data() + payload.size(), &root, &errors) && root.isObject();
}

int make_udp_socket() {
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if(fd < 0) {
        return -1;
    }
    const int descriptor_flags = fcntl(fd, F_GETFD, 0);
    if(descriptor_flags >= 0) {
        fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC);
    }
    const int flags = fcntl(fd, F_GETFL, 0);
    if(flags >= 0) {
        fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    }
    return fd;
}

bool json_uint64(const Json::Value &root, const char *key, uint64_t &out) {
    if(!root.isMember(key) || !root[key].isUInt64()) {
        return false;
    }
    out = root[key].asUInt64();
    return true;
}

bool json_string_equals(const Json::Value &root, const char *key, const std::string &expected) {
    return root.isMember(key) && root[key].isString() && root[key].asString() == expected;
}

}  // namespace

ClockSyncClient::ClockSyncClient(ClockSyncClientConfig config, std::string sender_id)
    : ClockSyncClient(config, std::move(sender_id), std::make_shared<ReceiverTarget>(config.receiver_ip)) {}

ClockSyncClient::ClockSyncClient(ClockSyncClientConfig config, std::string sender_id,
                                 std::shared_ptr<ReceiverTarget> receiver_target)
    : config_(std::move(config)), sender_id_(std::move(sender_id)), receiver_target_(std::move(receiver_target)) {
    config_.interval_ms = std::max(1, config_.interval_ms);
    config_.timeout_ms = std::max(1, config_.timeout_ms);
    config_.max_delay_us = std::max<int64_t>(1, config_.max_delay_us);
    config_.sample_window = std::max<size_t>(1, config_.sample_window);
}

ClockSyncClient::~ClockSyncClient() {
    stop();
}

void ClockSyncClient::set_log_callbacks(std::function<void(const std::string &)> info,
                                        std::function<void(const std::string &)> warn) {
    std::lock_guard<std::mutex> lock(log_mutex_);
    info_log_ = std::move(info);
    warn_log_ = std::move(warn);
}

bool ClockSyncClient::start() {
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
                log_warn(std::string("clock_sync thread stopped after exception: ") + e.what());
                running_ = false;
            }
            catch(...) {
                log_warn("clock_sync thread stopped after unknown exception");
                running_ = false;
            }
        });
    }
    catch(const std::exception &e) {
        running_ = false;
        log_warn(std::string("clock_sync thread start failed: ") + e.what());
        return false;
    }
    return true;
}

void ClockSyncClient::stop() {
    running_ = false;
    if(thread_.joinable()) {
        thread_.join();
    }
}

bool ClockSyncClient::healthy() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if(!state_.valid || state_.last_sync_us == 0 || last_sync_steady_ == std::chrono::steady_clock::time_point{}) {
        return false;
    }
    const auto max_age = std::chrono::microseconds(max_sync_age_us(config_.interval_ms));
    return std::chrono::steady_clock::now() - last_sync_steady_ <= max_age;
}

int64_t ClockSyncClient::offset_us() const {
    return state().offset_us;
}

int64_t ClockSyncClient::delay_us() const {
    return state().delay_us;
}

double ClockSyncClient::drift_ppm() const {
    return state().drift_ppm;
}

ClockSyncClientState ClockSyncClient::state() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return state_;
}

void ClockSyncClient::run_loop() {
    if(!receiver_target_ || receiver_target_->snapshot().host.empty()) {
        log_warn("clock_sync disabled: receiver_ip is empty");
        running_ = false;
        return;
    }

    const int fd = make_udp_socket();
    if(fd < 0) {
        log_warn(std::string("clock_sync socket create failed: ") + std::strerror(errno));
        running_ = false;
        return;
    }

    uint64_t sequence = 0;
    auto next_warning = std::chrono::steady_clock::now();
    while(running_) {
        const bool ok = send_probe_and_wait_response(fd, ++sequence);
        if(!ok && std::chrono::steady_clock::now() >= next_warning) {
            const auto target = receiver_target_->snapshot();
            log_warn("clock_sync probe timeout or invalid response sender_id=" + sender_id_
                     + " receiver=" + target.host + ":" + std::to_string(config_.port));
            next_warning = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        }

        const auto sleep_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.interval_ms);
        while(running_ && std::chrono::steady_clock::now() < sleep_until) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    close(fd);
}

bool ClockSyncClient::send_probe_and_wait_response(int fd, uint64_t sequence) {
    const auto target = receiver_target_->snapshot();
    if(target.host.empty()) {
        return false;
    }
    if(target_generation_ != target.generation) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        state_ = ClockSyncClientState{};
        samples_.clear();
        last_sync_steady_ = {};
        target_generation_ = target.generation;
    }
    sockaddr_in addr{};
    try {
        addr = resolve_ipv4_endpoint(target.host, config_.port);
    }
    catch(const std::exception &e) {
        log_warn(e.what());
        return false;
    }

    const uint64_t t1 = now_us();
    Json::Value probe;
    probe["protocol_version"] = kProtocolVersion;
    probe["message_type"] = "clock_sync_probe";
    probe["sender_id"] = sender_id_;
    probe["sequence"] = Json::UInt64(sequence);
    probe["t1_sender_send_us"] = Json::UInt64(t1);
    auto payload = json_to_string(probe);
    payload.push_back('\n');

    const auto sent = sendto(fd, payload.data(), payload.size(), 0, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    if(sent < 0 || static_cast<size_t>(sent) != payload.size()) {
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.timeout_ms);
    std::array<char, 4096> buffer{};
    while(running_ && std::chrono::steady_clock::now() < deadline) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline - std::chrono::steady_clock::now());
        timeval timeout{};
        timeout.tv_sec = static_cast<long>(std::max<int64_t>(0, remaining.count()) / 1'000'000);
        timeout.tv_usec = static_cast<long>(std::max<int64_t>(0, remaining.count()) % 1'000'000);
        const int rc = select(fd + 1, &rfds, nullptr, nullptr, &timeout);
        if(rc < 0) {
            if(errno == EINTR) {
                continue;
            }
            return false;
        }
        if(rc == 0) {
            return false;
        }

        sockaddr_in peer{};
        socklen_t peer_len = sizeof(peer);
        const ssize_t got = recvfrom(fd, buffer.data(), buffer.size(), 0, reinterpret_cast<sockaddr *>(&peer), &peer_len);
        const uint64_t t4 = now_us();
        if(got <= 0) {
            continue;
        }
        if(peer.sin_family != AF_INET || peer.sin_addr.s_addr != addr.sin_addr.s_addr || peer.sin_port != addr.sin_port) {
            continue;
        }

        Json::Value response;
        if(!parse_json_object(std::string(buffer.data(), static_cast<size_t>(got)), response)) {
            continue;
        }
        if(!json_string_equals(response, "protocol_version", kProtocolVersion)
           || !json_string_equals(response, "message_type", "clock_sync_response")
           || !json_string_equals(response, "sender_id", sender_id_)) {
            continue;
        }

        uint64_t response_sequence = 0;
        uint64_t response_t1 = 0;
        uint64_t t2 = 0;
        uint64_t t3 = 0;
        if(!json_uint64(response, "sequence", response_sequence) || response_sequence != sequence
           || !json_uint64(response, "t1_sender_send_us", response_t1) || response_t1 != t1
           || !json_uint64(response, "t2_receiver_recv_us", t2)
           || !json_uint64(response, "t3_receiver_send_us", t3)) {
            continue;
        }

        constexpr uint64_t kMaxAcceptedClockSkewUs = 24ull * 60ull * 60ull * 1'000'000ull;
        if(t1 > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
           || t2 > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
           || t3 > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
           || t4 > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
           || t4 < t1 || t3 < t2
           || absolute_difference(t2, t1) > kMaxAcceptedClockSkewUs
           || absolute_difference(t3, t4) > kMaxAcceptedClockSkewUs) {
            continue;
        }
        const auto s_t1 = static_cast<int64_t>(t1);
        const auto r_t2 = static_cast<int64_t>(t2);
        const auto r_t3 = static_cast<int64_t>(t3);
        const auto s_t4 = static_cast<int64_t>(t4);
        const int64_t offset_us = ((r_t2 - s_t1) + (r_t3 - s_t4)) / 2;
        const int64_t delay_us = (s_t4 - s_t1) - (r_t3 - r_t2);
        if(receiver_target_->snapshot().generation != target.generation) {
            return false;
        }
        receiver_target_->mark_success(target.generation);
        return apply_sample(t4, offset_us, delay_us);
    }

    return false;
}

bool ClockSyncClient::apply_sample(uint64_t sender_receive_us, int64_t offset_us, int64_t delay_us) {
    if(delay_us < 0 || delay_us > config_.max_delay_us) {
        return false;
    }

    ClockSyncClientState snapshot;
    bool accepted_large_reset = false;
    int64_t previous_offset_us = 0;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if(state_.valid && std::llabs(offset_us - state_.offset_us) > 50000) {
            const uint64_t max_age_us = max_sync_age_us(config_.interval_ms);
            const bool stale = state_.last_sync_us == 0 || sender_receive_us <= state_.last_sync_us
                               || sender_receive_us - state_.last_sync_us > max_age_us;
            if(!stale) {
                return false;
            }
            accepted_large_reset = true;
            previous_offset_us = state_.offset_us;
            state_ = ClockSyncClientState{};
            samples_.clear();
            last_sync_steady_ = {};
        }

        samples_.push_back(Sample{sender_receive_us, offset_us, delay_us});
        while(samples_.size() > config_.sample_window) {
            samples_.erase(samples_.begin());
        }
        auto selected = samples_;
        std::sort(selected.begin(), selected.end(), [](const Sample &lhs, const Sample &rhs) { return lhs.delay_us < rhs.delay_us; });
        const size_t selected_count = std::min(selected.size(), std::max<size_t>(1, (selected.size() + 1) / 2));
        selected.resize(selected_count);
        std::vector<int64_t> selected_offsets;
        selected_offsets.reserve(selected.size());
        for(const auto &sample : selected) {
            selected_offsets.push_back(sample.offset_us);
        }
        std::sort(selected_offsets.begin(), selected_offsets.end());
        const int64_t filtered_offset = selected_offsets[selected_offsets.size() / 2];

        if(!state_.valid) {
            state_.offset_us = filtered_offset;
        }
        else {
            state_.offset_us = static_cast<int64_t>(std::llround(static_cast<double>(state_.offset_us) * 0.8
                                                                 + static_cast<double>(filtered_offset) * 0.2));
        }
        state_.valid = true;
        state_.delay_us = selected.front().delay_us;
        state_.last_sync_us = sender_receive_us;
        last_sync_steady_ = std::chrono::steady_clock::now();
        state_.sample_count = samples_.size();
        if(selected.size() >= 3) {
            long double mean_time = 0.0;
            long double mean_offset = 0.0;
            for(const auto &sample : selected) {
                mean_time += sample.sender_receive_us;
                mean_offset += sample.offset_us;
            }
            mean_time /= selected.size();
            mean_offset /= selected.size();
            long double covariance = 0.0;
            long double variance = 0.0;
            for(const auto &sample : selected) {
                const long double x = static_cast<long double>(sample.sender_receive_us) - mean_time;
                const long double y = static_cast<long double>(sample.offset_us) - mean_offset;
                covariance += x * y;
                variance += x * x;
            }
            if(variance > 0.0) {
                state_.drift_ppm = std::clamp(static_cast<double>(covariance / variance * 1'000'000.0L), -200.0, 200.0);
            }
        }
        snapshot = state_;
    }

    if(accepted_large_reset) {
        log_warn("clock_sync accepting large offset reset sender_id=" + sender_id_
                 + " old_offset_us=" + std::to_string(previous_offset_us)
                 + " new_offset_us=" + std::to_string(offset_us));
    }

    static thread_local auto next_info = std::chrono::steady_clock::now();
    if(std::chrono::steady_clock::now() >= next_info) {
        std::ostringstream oss;
        oss << "clock_sync sender_id=" << sender_id_
            << " offset_us=" << snapshot.offset_us
            << " delay_us=" << snapshot.delay_us
            << " drift_ppm=" << snapshot.drift_ppm
            << " samples=" << snapshot.sample_count
            << " healthy=" << (snapshot.valid ? "true" : "false");
        log_info(oss.str());
        next_info = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    }
    return true;
}

void ClockSyncClient::log_info(const std::string &message) const {
    std::function<void(const std::string &)> callback;
    {
        std::lock_guard<std::mutex> lock(log_mutex_);
        callback = info_log_;
    }
    if(callback) {
        callback(message);
    }
}

void ClockSyncClient::log_warn(const std::string &message) const {
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
