#include "gwv3_sender/network_utils.hpp"

#include <chrono>
#include <cctype>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

#include <arpa/inet.h>
#include <netdb.h>

namespace gwv3 {

namespace {

struct CachedAddress {
    in_addr address{};
    std::chrono::steady_clock::time_point expires_at{};
};

std::mutex cache_mutex;
std::unordered_map<std::string, CachedAddress> address_cache;

}  // namespace

bool is_valid_ipv4_or_hostname(const std::string &host) {
    if(host.empty() || host.size() > 253) {
        return false;
    }
    in_addr numeric{};
    if(inet_pton(AF_INET, host.c_str(), &numeric) == 1) {
        return true;
    }
    size_t label_start = 0;
    while(label_start < host.size()) {
        const size_t label_end = host.find('.', label_start);
        const size_t end = label_end == std::string::npos ? host.size() : label_end;
        const size_t length = end - label_start;
        if(length == 0 || length > 63 || host[label_start] == '-' || host[end - 1] == '-') {
            return false;
        }
        for(size_t index = label_start; index < end; ++index) {
            const unsigned char ch = static_cast<unsigned char>(host[index]);
            if(!std::isalnum(ch) && ch != '-') {
                return false;
            }
        }
        if(label_end == std::string::npos) {
            return true;
        }
        label_start = label_end + 1;
    }
    return false;
}

sockaddr_in resolve_ipv4_endpoint(const std::string &host, uint16_t port) {
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(port);
    if(inet_pton(AF_INET, host.c_str(), &endpoint.sin_addr) == 1) {
        return endpoint;
    }

    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        const auto found = address_cache.find(host);
        if(found != address_cache.end() && now < found->second.expires_at) {
            endpoint.sin_addr = found->second.address;
            return endpoint;
        }
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *results = nullptr;
    const int rc = getaddrinfo(host.c_str(), nullptr, &hints, &results);
    if(rc != 0 || results == nullptr) {
        if(results != nullptr) {
            freeaddrinfo(results);
        }
        throw std::runtime_error("cannot resolve receiver host " + host + ": " + gai_strerror(rc));
    }
    endpoint.sin_addr = reinterpret_cast<const sockaddr_in *>(results->ai_addr)->sin_addr;
    freeaddrinfo(results);
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        address_cache[host] = {endpoint.sin_addr, now + std::chrono::seconds(5)};
    }
    return endpoint;
}

}  // namespace gwv3
