#include "gwv3_sender/device_identity.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

namespace fs = std::filesystem;

namespace {

void write_value(const fs::path &path, const std::string &value) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path);
    output << value << '\n';
}

}  // namespace

int main() {
    const fs::path root = fs::temp_directory_path() / ("gwv3-device-identity-" + std::to_string(getpid()));
    const std::string interface_name = "gwv3test0";
    const fs::path interface = root / interface_name;

    write_value(interface / "perm_address", "AA:BB:CC:DD:EE:01");
    write_value(interface / "address", "02:00:00:00:00:01");
    auto identity = gwv3::read_device_identity(interface_name, root);
    assert(identity.wifi_interface == interface_name);
    assert(identity.wifi_permanent_mac == "aa:bb:cc:dd:ee:01");
    assert(identity.mac_is_permanent);
    assert(identity.mac_source == "sysfs_perm_address");

    fs::remove(interface / "perm_address");
    identity = gwv3::read_device_identity(interface_name, root);
    assert(identity.wifi_permanent_mac == "02:00:00:00:00:01");
    assert(!identity.mac_is_permanent);
    assert(identity.mac_source == "sysfs_current_address_fallback");

    write_value(interface / "address", "00:00:00:00:00:00");
    identity = gwv3::read_device_identity(interface_name, root);
    assert(identity.wifi_permanent_mac.empty());
    assert(!identity.mac_is_permanent);
    assert(identity.mac_source == "unavailable");

    const auto clock = gwv3::current_device_clock();
    assert(clock.system_time_us > 0);
    assert(clock.date.size() == 10);
    assert(clock.time_iso8601.size() >= 19);

    fs::remove_all(root);
    return 0;
}
