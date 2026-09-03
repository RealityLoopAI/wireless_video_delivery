#include "gwv3_receiver/receiver_discovery_server.hpp"

#include "gwv3_common/protocol.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <fstream>
#include <memory>
#include <regex>
#include <stdexcept>
#include <utility>

#include <arpa/inet.h>
#include <fcntl.h>
#include <json/json.h>
#include <sys/socket.h>
#include <unistd.h>

namespace gwv3 {
namespace {

uint64_t now_us() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count());
}

std::string trim_copy(std::string value) {
    const auto non_space = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), non_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), non_space).base(), value.end());
    return value;
}

std::string sanitize_id(std::string value) {
    for(auto &ch : value) {
        const auto c = static_cast<unsigned char>(ch);
        ch = std::isalnum(c) || ch == '_' || ch == '-' ? static_cast<char>(std::tolower(c)) : '-';
    }
    while(!value.empty() && value.front() == '-') {
        value.erase(value.begin());
    }
    while(!value.empty() && value.back() == '-') {
        value.pop_back();
    }
    return value.empty() ? "receiver" : value;
}

std::string stable_receiver_id() {
    char hostname[128] = {};
    std::string prefix = gethostname(hostname, sizeof(hostname) - 1) == 0 ? sanitize_id(hostname) : "receiver";
    std::ifstream input("/etc/machine-id");
    std::string machine_id;
    std::getline(input, machine_id);
    machine_id = trim_copy(machine_id);
    machine_id.erase(std::remove_if(machine_id.begin(), machine_id.end(), [](unsigned char ch) {
                         return !std::isxdigit(ch);
                     }),
                     machine_id.end());
    if(machine_id.size() > 8) {
        machine_id = machine_id.substr(machine_id.size() - 8);
    }
    if(machine_id.empty()) {
        machine_id = "default";
    }
    const size_t max_prefix = 64 - 1 - machine_id.size();
    if(prefix.size() > max_prefix) {
        prefix.resize(max_prefix);
    }
    return prefix + "-" + machine_id;
}

bool valid_id(const std::string &value) {
    static const std::regex pattern("^[A-Za-z0-9_-]{1,64}$");
    return std::regex_match(value, pattern);
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

sockaddr_in bind_address(const std::string &ip, uint16_t port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if(inet_pton(AF_INET, ip.c_str(), &address.sin_addr) != 1) {
        throw std::runtime_error("invalid receiver discovery bind_ip: " + ip);
    }
    return address;
}

}  // namespace

ReceiverDiscoveryServer::ReceiverDiscoveryServer(ReceiverDiscoveryServerConfig config)
    : config_(std::move(config)),
      receiver_id_(config_.receiver_id.empty() || config_.receiver_id == "auto" ? stable_receiver_id()
                                                                                  : config_.receiver_id) {
    if(!valid_id(receiver_id_)) {
        throw std::runtime_error("receiver discovery receiver_id must be 1-64 ASCII letters/digits/_/-");
    }
}

ReceiverDiscoveryServer::~ReceiverDiscoveryServer() {
    stop();
}

bool ReceiverDiscoveryServer::start() {
    if(!config_.enabled) {
        return true;
    }
    bool expected = false;
    if(!running_.compare_exchange_strong(expected, true)) {
        return true;
    }
    try {
        thread_ = std::thread([this] { run(); });
    }
    catch(...) {
        running_ = false;
        throw;
    }
    return true;
}

void ReceiverDiscoveryServer::stop() {
    running_ = false;
    if(thread_.joinable()) {
        thread_.join();
    }
    healthy_ = false;
}

void ReceiverDiscoveryServer::set_log_callbacks(std::function<void(const std::string &)> info,
                                                std::function<void(const std::string &)> warn) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    info_callback_ = std::move(info);
    warn_callback_ = std::move(warn);
}

bool ReceiverDiscoveryServer::healthy() const {
    return !config_.enabled || healthy_.load();
}

std::string ReceiverDiscoveryServer::receiver_id() const {
    return receiver_id_;
}

uint64_t ReceiverDiscoveryServer::requests_received() const {
    return requests_received_.load();
}

uint64_t ReceiverDiscoveryServer::responses_sent() const {
    return responses_sent_.load();
}

uint64_t ReceiverDiscoveryServer::last_request_us() const {
    return last_request_us_.load();
}

void ReceiverDiscoveryServer::run() {
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
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    timeval timeout{};
    timeout.tv_sec = 0;
    timeout.tv_usec = 250000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    try {
        const auto address = bind_address(config_.bind_ip, config_.port);
        if(bind(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0) {
            throw std::runtime_error(std::string("bind failed: ") + std::strerror(errno));
        }
    }
    catch(const std::exception &error) {
        log_warn(std::string("receiver discovery unavailable: ") + error.what());
        close(fd);
        running_ = false;
        return;
    }

    healthy_ = true;
    log_info("receiver discovery listening receiver_id=" + receiver_id_ + " bind=" + config_.bind_ip
             + ":" + std::to_string(config_.port));
    std::array<char, 4096> buffer{};
    while(running_) {
        sockaddr_in peer{};
        socklen_t peer_size = sizeof(peer);
        const ssize_t got = recvfrom(fd, buffer.data(), buffer.size(), 0,
                                     reinterpret_cast<sockaddr *>(&peer), &peer_size);
        if(got < 0) {
            if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            log_warn(std::string("receiver discovery receive failed: ") + std::strerror(errno));
            continue;
        }
        if(got == 0 || static_cast<size_t>(got) == buffer.size()) {
            continue;
        }
        Json::Value request;
        if(!parse_json(buffer.data(), static_cast<size_t>(got), request)
           || request["protocol_version"].asString() != kProtocolVersion
           || request["message_type"].asString() != "receiver_discovery_request"
           || !request["sequence"].isUInt64() || !valid_id(request["sender_id"].asString())) {
            continue;
        }
        requests_received_.fetch_add(1);
        last_request_us_ = now_us();

        Json::Value response(Json::objectValue);
        response["protocol_version"] = kProtocolVersion;
        response["message_type"] = "receiver_discovery_response";
        response["receiver_id"] = receiver_id_;
        response["sequence"] = request["sequence"];
        response["media_port"] = config_.media_port;
        response["status_port"] = config_.status_port;
        response["clock_sync_port"] = config_.clock_sync_port;
        response["media_udp_port"] = config_.media_udp_port;
        response["preview_udp_port"] = config_.preview_udp_port;
        Json::StreamWriterBuilder writer;
        writer["indentation"] = "";
        const auto payload = Json::writeString(writer, response);
        const auto sent = sendto(fd, payload.data(), payload.size(), 0,
                                 reinterpret_cast<const sockaddr *>(&peer), peer_size);
        if(sent >= 0 && static_cast<size_t>(sent) == payload.size()) {
            responses_sent_.fetch_add(1);
        }
    }
    healthy_ = false;
    close(fd);
    log_info("receiver discovery stopped");
}

void ReceiverDiscoveryServer::log_info(const std::string &message) const {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    if(info_callback_) {
        info_callback_(message);
    }
}

void ReceiverDiscoveryServer::log_warn(const std::string &message) const {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    if(warn_callback_) {
        warn_callback_(message);
    }
}

}  // namespace gwv3
