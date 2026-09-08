#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace gwv3 {

struct DeviceIdentity {
    uint32_t version = 1;
    std::string host_name;
    std::string wifi_interface;
    std::string wifi_permanent_mac;
    bool mac_is_permanent = false;
    std::string mac_source;
};

struct DeviceClockSnapshot {
    uint64_t system_time_us = 0;
    std::string date;
    std::string time_iso8601;
    std::string timezone;
};

DeviceIdentity read_device_identity(
    const std::string &wifi_interface = "wlan0",
    const std::filesystem::path &sys_class_net_root = "/sys/class/net");

DeviceClockSnapshot current_device_clock();

}  // namespace gwv3
