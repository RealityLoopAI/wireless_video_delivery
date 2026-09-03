#include "gwv3_sender/receiver_discovery_client.hpp"

#include "gwv3_sender/network_utils.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <regex>
#include <sstream>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <json/json.h>

namespace gwv3 {
namespace {

constexpr const char *kProtocolVersion = "3.0";

uint64_t system_now_us() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count());
}

uint64_t steady_now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

std::filesystem::path default_state_path() {
    if(const char *xdg = std::getenv("XDG_STATE_HOME"); xdg != nullptr && xdg[0] != '\0') {
        return std::filesystem::path(xdg) / "gwv3" / "receiver_target.json";
    }
    if(const char *home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        return std::filesystem::path(home) / ".local" / "state" / "gwv3" / "receiver_target.json";
    }
    return "/tmp/gwv3_receiver_target.json";
}

bool parse_json(const char *data, size_t size, Json::Value &root) {
    Json::CharReaderBuilder builder;
    builder["collectComments"] = false;
    builder["failIfExtra"] = true;
    builder["strictRoot"] = true;
    std::string errors;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    return reader->parse(data, data + size, &root, &errors) && root.isObject();
}

bool valid_id(const std::string &value) {
    static const std::regex pattern("^[A-Za-z0-9_-]{1,64}$");
    return std::regex_match(value, pattern);
}

std::vector<in_addr> broadcast_addresses() {
    std::vector<in_addr> addresses;
    in_addr limited{};
    limited.s_addr = htonl(INADDR_BROADCAST);
    addresses.push_back(limited);

    ifaddrs *interfaces = nullptr;
    if(getifaddrs(&interfaces) != 0) {
        return addresses;
    }
    for(auto *entry = interfaces; entry != nullptr; entry = entry->ifa_next) {
        if(entry->ifa_addr == nullptr || entry->ifa_addr->sa_family != AF_INET
           || (entry->ifa_flags & IFF_UP) == 0 || (entry->ifa_flags & IFF_LOOPBACK) != 0
           || (entry->ifa_flags & IFF_BROADCAST) == 0 || entry->ifa_broadaddr == nullptr) {
            continue;
        }
        const auto address = reinterpret_cast<const sockaddr_in *>(entry->ifa_broadaddr)->sin_addr;
        const auto duplicate = std::any_of(addresses.begin(), addresses.end(), [&](const in_addr &candidate) {
            return candidate.s_addr == address.s_addr;
        });
        if(!duplicate) {
            addresses.push_back(address);
        }
    }
    freeifaddrs(interfaces);
    return addresses;
}

std::string address_text(const in_addr &address) {
    std::array<char, INET_ADDRSTRLEN> buffer{};
    return inet_ntop(AF_INET, &address, buffer.data(), buffer.size()) != nullptr ? std::string(buffer.data()) : "";
}

void load_persisted_target(const std::filesystem::path &path, const std::shared_ptr<ReceiverTarget> &target) {
    std::ifstream input(path);
    if(!input) {
        return;
    }
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    Json::Value root;
    if(!parse_json(text.data(), text.size(), root)) {
        return;
    }
    const auto host = root["receiver_host"].asString();
    const auto receiver_id = root["receiver_id"].asString();
    if(is_valid_ipv4_or_hostname(host) && valid_id(receiver_id)) {
        target->restore_persisted(host, receiver_id);
    }
}

void save_persisted_target(const std::filesystem::path &path, const ReceiverTargetSnapshot &target) {
    if(!target.discovered || target.host.empty() || target.receiver_id.empty()) {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if(ec) {
        return;
    }
    Json::Value root(Json::objectValue);
    root["receiver_id"] = target.receiver_id;
    root["receiver_host"] = target.host;
    root["updated_us"] = Json::UInt64(system_now_us());
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "  ";
    const auto temporary = path.string() + ".tmp." + std::to_string(static_cast<unsigned long>(getpid()));
    {
        std::ofstream output(temporary, std::ios::trunc);
        if(!output) {
            return;
        }
        output << Json::writeString(builder, root) << '\n';
        output.flush();
        if(!output) {
            std::filesystem::remove(temporary, ec);
            return;
        }
    }
    std::filesystem::rename(temporary, path, ec);
    if(ec) {
        std::filesystem::remove(temporary, ec);
    }
}

}  // namespace

ReceiverDiscoveryClient::ReceiverDiscoveryClient(ReceiverDiscoveryConfig config, std::string sender_id,
                                                 std::shared_ptr<ReceiverTarget> target)
    : config_(std::move(config)), sender_id_(std::move(sender_id)), target_(std::move(target)) {}

ReceiverDiscoveryClient::~ReceiverDiscoveryClient() {
    stop();
}

bool ReceiverDiscoveryClient::start() {
    if(!config_.enabled || !target_) {
        return false;
    }
    bool expected = false;
    if(!running_.compare_exchange_strong(expected, true)) {
        return true;
    }
    const auto state_path = config_.state_path.empty() ? default_state_path() : std::filesystem::path(config_.state_path);
    load_persisted_target(state_path, target_);
    try {
        thread_ = std::thread([this] { run(); });
    }
    catch(...) {
        running_ = false;
        throw;
    }
    return true;
}

void ReceiverDiscoveryClient::stop() {
    running_ = false;
    if(thread_.joinable()) {
        thread_.join();
    }
    healthy_ = false;
}

void ReceiverDiscoveryClient::set_log_callbacks(std::function<void(const std::string &)> info,
                                                std::function<void(const std::string &)> warn) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    info_callback_ = std::move(info);
    warn_callback_ = std::move(warn);
}

bool ReceiverDiscoveryClient::healthy() const {
    return healthy_.load();
}

uint64_t ReceiverDiscoveryClient::last_response_us() const {
    return last_response_us_.load();
}

void ReceiverDiscoveryClient::run() {
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if(fd < 0) {
        running_ = false;
        log_warn(std::string("receiver discovery socket failed: ") + std::strerror(errno));
        return;
    }
    const int descriptor_flags = fcntl(fd, F_GETFD, 0);
    if(descriptor_flags >= 0) {
        fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC);
    }
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = 0;
    if(bind(fd, reinterpret_cast<const sockaddr *>(&local), sizeof(local)) != 0) {
        log_warn(std::string("receiver discovery bind failed: ") + std::strerror(errno));
        close(fd);
        running_ = false;
        return;
    }

    uint64_t sequence = 0;
    uint64_t last_current_seen_ms = target_->snapshot().receiver_id.empty() ? 0 : steady_now_ms();
    const auto state_path = config_.state_path.empty() ? default_state_path() : std::filesystem::path(config_.state_path);
    log_info("receiver discovery started port=" + std::to_string(config_.port)
             + " fallback=" + target_->fallback_host());

    while(running_) {
        const auto cycle_started = std::chrono::steady_clock::now();
        const auto current = target_->snapshot();
        Json::Value request(Json::objectValue);
        request["protocol_version"] = kProtocolVersion;
        request["message_type"] = "receiver_discovery_request";
        request["sender_id"] = sender_id_;
        request["sequence"] = Json::UInt64(++sequence);
        request["preferred_receiver_id"] = current.receiver_id;
        Json::StreamWriterBuilder writer;
        writer["indentation"] = "";
        const auto payload = Json::writeString(writer, request);

        for(const auto &broadcast : broadcast_addresses()) {
            sockaddr_in destination{};
            destination.sin_family = AF_INET;
            destination.sin_addr = broadcast;
            destination.sin_port = htons(config_.port);
            (void)sendto(fd, payload.data(), payload.size(), 0,
                         reinterpret_cast<const sockaddr *>(&destination), sizeof(destination));
        }
        try {
            const auto fallback = resolve_ipv4_endpoint(target_->fallback_host(), config_.port);
            (void)sendto(fd, payload.data(), payload.size(), 0,
                         reinterpret_cast<const sockaddr *>(&fallback), sizeof(fallback));
        }
        catch(const std::exception &) {
        }

        std::map<std::string, std::string> candidates;
        const auto response_deadline = cycle_started + std::chrono::milliseconds(config_.response_window_ms);
        while(running_ && std::chrono::steady_clock::now() < response_deadline) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                response_deadline - std::chrono::steady_clock::now());
            pollfd pfd{};
            pfd.fd = fd;
            pfd.events = POLLIN;
            const int rc = poll(&pfd, 1, std::max(1, static_cast<int>(remaining.count())));
            if(rc <= 0) {
                if(rc < 0 && errno == EINTR) {
                    continue;
                }
                break;
            }
            std::array<char, 4096> buffer{};
            sockaddr_in peer{};
            socklen_t peer_size = sizeof(peer);
            const ssize_t got = recvfrom(fd, buffer.data(), buffer.size(), 0,
                                         reinterpret_cast<sockaddr *>(&peer), &peer_size);
            if(got <= 0 || static_cast<size_t>(got) == buffer.size()) {
                continue;
            }
            Json::Value response;
            if(!parse_json(buffer.data(), static_cast<size_t>(got), response)
               || response["protocol_version"].asString() != kProtocolVersion
               || response["message_type"].asString() != "receiver_discovery_response"
               || response["sequence"].asUInt64() != sequence) {
                continue;
            }
            const auto receiver_id = response["receiver_id"].asString();
            const auto host = address_text(peer.sin_addr);
            if(!valid_id(receiver_id) || host.empty()) {
                continue;
            }
            candidates[receiver_id] = host;
            last_response_us_ = system_now_us();
        }

        const auto now_ms = steady_now_ms();
        const auto before = target_->snapshot();
        auto selected = candidates.end();
        if(!before.receiver_id.empty()) {
            selected = candidates.find(before.receiver_id);
        }
        if(selected == candidates.end()) {
            selected = std::find_if(candidates.begin(), candidates.end(), [&](const auto &candidate) {
                return candidate.second == before.host;
            });
        }
        if(selected != candidates.end()) {
            last_current_seen_ms = now_ms;
            healthy_ = true;
            if(target_->update_discovered(selected->second, selected->first)) {
                save_persisted_target(state_path, target_->snapshot());
                log_info("receiver discovery target updated receiver_id=" + selected->first
                         + " host=" + selected->second);
            }
        }
        else {
            const bool current_recent = target_->success_recent(config_.sticky_timeout_ms)
                                        || (now_ms >= last_current_seen_ms
                                            && now_ms - last_current_seen_ms
                                                   <= static_cast<uint64_t>(config_.sticky_timeout_ms));
            if(!candidates.empty() && !current_recent) {
                selected = candidates.begin();
                last_current_seen_ms = now_ms;
                healthy_ = true;
                if(target_->update_discovered(selected->second, selected->first)) {
                    save_persisted_target(state_path, target_->snapshot());
                    log_info("receiver discovery switched receiver_id=" + selected->first
                             + " host=" + selected->second);
                }
            }
            else if(candidates.empty() && !current_recent) {
                healthy_ = false;
                if(target_->use_fallback()) {
                    log_warn("receiver discovery timed out; using configured fallback=" + target_->fallback_host());
                }
            }
        }

        const auto next_cycle = cycle_started + std::chrono::milliseconds(config_.interval_ms);
        while(running_ && std::chrono::steady_clock::now() < next_cycle) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    close(fd);
    log_info("receiver discovery stopped");
}

void ReceiverDiscoveryClient::log_info(const std::string &message) const {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    if(info_callback_) {
        info_callback_(message);
    }
}

void ReceiverDiscoveryClient::log_warn(const std::string &message) const {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    if(warn_callback_) {
        warn_callback_(message);
    }
}

}  // namespace gwv3
