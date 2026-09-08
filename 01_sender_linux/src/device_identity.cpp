#include "gwv3_sender/device_identity.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>

#include <linux/ethtool.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace gwv3 {
namespace {

std::string trim_copy(std::string value) {
    const auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

std::string read_text_file(const std::filesystem::path &path) {
    std::ifstream input(path);
    std::string value;
    if(input && std::getline(input, value)) {
        return trim_copy(value);
    }
    return {};
}

std::string normalize_mac(std::string value) {
    value = trim_copy(std::move(value));
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if(value.size() != 17) {
        return {};
    }
    bool all_zero = true;
    bool all_ff = true;
    for(size_t i = 0; i < value.size(); ++i) {
        if(i % 3 == 2) {
            if(value[i] != ':') {
                return {};
            }
            continue;
        }
        if(!std::isxdigit(static_cast<unsigned char>(value[i]))) {
            return {};
        }
        all_zero = all_zero && value[i] == '0';
        all_ff = all_ff && value[i] == 'f';
    }
    return all_zero || all_ff ? std::string{} : value;
}

std::string permanent_mac_from_ioctl(const std::string &interface_name) {
    if(interface_name.empty() || interface_name.size() >= IFNAMSIZ) {
        return {};
    }
    const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if(fd < 0) {
        return {};
    }

    constexpr size_t kMaxAddressBytes = 32;
    auto storage = std::make_unique<unsigned char[]>(sizeof(ethtool_perm_addr) + kMaxAddressBytes);
    std::fill_n(storage.get(), sizeof(ethtool_perm_addr) + kMaxAddressBytes, 0);
    auto *request = reinterpret_cast<ethtool_perm_addr *>(storage.get());
    request->cmd = ETHTOOL_GPERMADDR;
    request->size = kMaxAddressBytes;

    ifreq ifr{};
    std::copy(interface_name.begin(), interface_name.end(), ifr.ifr_name);
    ifr.ifr_data = reinterpret_cast<char *>(request);
    const int result = ioctl(fd, SIOCETHTOOL, &ifr);
    close(fd);
    if(result != 0 || request->size < 6) {
        return {};
    }

    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for(size_t i = 0; i < 6; ++i) {
        if(i > 0) {
            out << ':';
        }
        out << std::setw(2) << static_cast<unsigned int>(request->data[i]);
    }
    return normalize_mac(out.str());
}

std::string host_name() {
    std::array<char, 256> buffer{};
    return gethostname(buffer.data(), buffer.size() - 1) == 0 ? std::string(buffer.data()) : std::string{};
}

std::string detect_timezone() {
    if(const char *tz = std::getenv("TZ"); tz != nullptr && *tz != '\0') {
        return tz;
    }
    const auto configured = read_text_file("/etc/timezone");
    if(!configured.empty()) {
        return configured;
    }
    std::error_code ec;
    const auto target = std::filesystem::read_symlink("/etc/localtime", ec).generic_string();
    constexpr const char *prefix = "/usr/share/zoneinfo/";
    const auto pos = target.find(prefix);
    if(!ec && pos != std::string::npos) {
        return target.substr(pos + std::char_traits<char>::length(prefix));
    }
    return {};
}

std::string iso8601_time(const std::tm &local) {
    std::array<char, 64> buffer{};
    if(std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%S%z", &local) == 0) {
        return {};
    }
    std::string value(buffer.data());
    if(value.size() >= 5) {
        value.insert(value.size() - 2, ":");
    }
    return value;
}

}  // namespace

DeviceIdentity read_device_identity(const std::string &wifi_interface,
                                    const std::filesystem::path &sys_class_net_root) {
    DeviceIdentity identity;
    identity.host_name = host_name();
    identity.wifi_interface = wifi_interface;

    identity.wifi_permanent_mac = normalize_mac(
        read_text_file(sys_class_net_root / wifi_interface / "perm_address"));
    if(!identity.wifi_permanent_mac.empty()) {
        identity.mac_is_permanent = true;
        identity.mac_source = "sysfs_perm_address";
        return identity;
    }

    identity.wifi_permanent_mac = permanent_mac_from_ioctl(wifi_interface);
    if(!identity.wifi_permanent_mac.empty()) {
        identity.mac_is_permanent = true;
        identity.mac_source = "ethtool_perm_addr";
        return identity;
    }

    identity.wifi_permanent_mac = normalize_mac(
        read_text_file(sys_class_net_root / wifi_interface / "address"));
    if(!identity.wifi_permanent_mac.empty()) {
        identity.mac_source = "sysfs_current_address_fallback";
    }
    else {
        identity.mac_source = "unavailable";
    }
    return identity;
}

DeviceClockSnapshot current_device_clock() {
    DeviceClockSnapshot snapshot;
    const auto now = std::chrono::system_clock::now();
    snapshot.system_time_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count());
    const auto seconds = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    if(localtime_r(&seconds, &local) != nullptr) {
        std::array<char, 16> date{};
        if(std::strftime(date.data(), date.size(), "%Y-%m-%d", &local) > 0) {
            snapshot.date = date.data();
        }
        snapshot.time_iso8601 = iso8601_time(local);
    }
    static const std::string timezone = detect_timezone();
    snapshot.timezone = timezone;
    return snapshot;
}

}  // namespace gwv3
