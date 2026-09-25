// Standalone, explicit-serial DEVICE_AE_REFERENCE setter for ExecStartPre.
// No pipeline or stream is created. All arguments are checked before SDK setup.
#include "libobsensor/hpp/Context.hpp"
#include "libobsensor/hpp/Device.hpp"
#include "libobsensor/hpp/Error.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

constexpr int kUsageError = 2;
constexpr int kPreconditionError = 3;
constexpr int kReadbackMismatch = 4;
constexpr int kSdkError = 5;
constexpr OBPropertyID kReferenceProperty = OB_PROP_DEVICE_AE_REFERENCE_INT;

struct Options {
    std::string serial;
    int32_t reference = -1;
};

void usage(std::ostream &out, const char *program) {
    out << "Usage: " << program << " --serial SN --reference 0|1\n"
        << "       " << program << " --help\n\n"
        << "Explicitly set DEVICE_AE_REFERENCE on exactly one serial-number match.\n"
        << "  0 = depth based (also the explicit rollback command)\n"
        << "  1 = color based\n"
        << "Print support, current value and range; write and verify readback.\n"
        << "No default serial/value, streaming, retries or automatic restoration.\n"
        << "No arguments or invalid arguments fail before SDK initialization.\n"
        << "Exit: 0 verified; 2 usage; 3 precondition; 4 readback mismatch;\n"
        << "      5 SDK or other runtime exception.\n";
}

Options parse_options(int argc, char **argv) {
    Options options;
    bool have_serial = false;
    bool have_reference = false;
    for(int i = 1; i < argc; ++i) {
        const std::string argument(argv[i]);
        if(argument != "--serial" && argument != "--reference") {
            throw std::invalid_argument("unknown argument: " + argument);
        }
        if(i + 1 >= argc) {
            throw std::invalid_argument("missing value for " + argument);
        }
        const std::string value(argv[++i]);
        if(argument == "--serial") {
            if(have_serial) {
                throw std::invalid_argument("duplicate --serial");
            }
            if(value.empty() || value.compare(0, 2, "--") == 0) {
                throw std::invalid_argument("--serial requires a nonempty serial number");
            }
            options.serial = value;
            have_serial = true;
        }
        else {
            if(have_reference) {
                throw std::invalid_argument("duplicate --reference");
            }
            if(value != "0" && value != "1") {
                throw std::invalid_argument("--reference must be exactly 0 or 1");
            }
            options.reference = value == "0" ? 0 : 1;
            have_reference = true;
        }
    }
    if(!have_serial || !have_reference) {
        throw std::invalid_argument("both --serial and --reference are required");
    }
    return options;
}

int configure(const Options &options) {
    ob::Context context;
    const auto devices = context.queryDeviceList();
    if(!devices) {
        std::cerr << "ERROR: SDK returned no device list\n";
        return kPreconditionError;
    }

    uint32_t matches = 0;
    uint32_t selected_index = 0;
    const uint32_t count = devices->deviceCount();
    for(uint32_t i = 0; i < count; ++i) {
        const char *serial = devices->serialNumber(i);
        if(serial && options.serial == serial) {
            selected_index = i;
            ++matches;
        }
    }
    if(matches != 1) {
        std::cerr << "ERROR: serial=" << options.serial << " matches=" << matches
                  << "; exactly one matching device is required\n";
        return kPreconditionError;
    }

    // Open only the exact metadata match, then confirm its identity before writing.
    const auto device = devices->getDevice(selected_index);
    if(!device) {
        std::cerr << "ERROR: SDK returned no device for the selected serial\n";
        return kPreconditionError;
    }
    const auto info = device->getDeviceInfo();
    const char *opened_serial = info ? info->serialNumber() : nullptr;
    if(!opened_serial || options.serial != opened_serial) {
        std::cerr << "ERROR: opened device serial does not match the explicit serial\n";
        return kPreconditionError;
    }
    std::cout << "serial=" << opened_serial << " requested_reference=" << options.reference << '\n';

    const bool readable = device->isPropertySupported(kReferenceProperty, OB_PERMISSION_READ);
    const bool writable = device->isPropertySupported(kReferenceProperty, OB_PERMISSION_WRITE);
    std::cout << "DEVICE_AE_REFERENCE support_read=" << readable << " support_write=" << writable << '\n';
    if(!readable) {
        std::cerr << "ERROR: DEVICE_AE_REFERENCE is not readable; verified setup is unavailable\n";
        return kPreconditionError;
    }

    const int32_t before = device->getIntProperty(kReferenceProperty);
    std::cout << "DEVICE_AE_REFERENCE current=" << before << '\n';
    const OBIntPropertyRange range = device->getIntPropertyRange(kReferenceProperty);
    std::cout << "DEVICE_AE_REFERENCE range_min=" << range.min << " range_max=" << range.max
              << " range_step=" << range.step << " range_default=" << range.def
              << " range_current=" << range.cur << '\n';
    if(!writable) {
        std::cerr << "ERROR: DEVICE_AE_REFERENCE is not writable\n";
        return kPreconditionError;
    }
    if(range.min > range.max || range.step <= 0) {
        std::cerr << "ERROR: invalid integer property range; refusing the write\n";
        return kPreconditionError;
    }
    const int64_t offset = static_cast<int64_t>(options.reference) - range.min;
    if(options.reference < range.min || options.reference > range.max || offset % range.step != 0) {
        std::cerr << "ERROR: requested reference is outside the supported integer range\n";
        return kPreconditionError;
    }

    std::cout << "DEVICE_AE_REFERENCE writing=" << options.reference << '\n';
    device->setIntProperty(kReferenceProperty, options.reference);
    const int32_t after = device->getIntProperty(kReferenceProperty);
    const bool verified = after == options.reference;
    std::cout << "DEVICE_AE_REFERENCE before=" << before << " requested=" << options.reference
              << " readback=" << after << " verified=" << verified << '\n';
    if(!verified) {
        std::cerr << "ERROR: readback did not match; no automatic restoration was attempted\n";
        return kReadbackMismatch;
    }
    return 0;
}

}  // namespace

int main(int argc, char **argv) {
    if(argc == 2 && std::string(argv[1]) == "--help") {
        usage(std::cout, argv[0]);
        return 0;
    }
    if(argc == 1) {
        usage(std::cerr, argv[0]);
        return kUsageError;
    }

    Options options;
    try {
        options = parse_options(argc, argv);
    }
    catch(const std::invalid_argument &error) {
        std::cerr << "ERROR: " << error.what() << '\n';
        usage(std::cerr, argv[0]);
        return kUsageError;
    }

    // Flush each diagnostic so systemd retains pre-write evidence on failures.
    std::cout << std::unitbuf;
    try {
        return configure(options);
    }
    catch(const ob::Error &error) {
        std::cerr << "ERROR: Orbbec SDK: " << error.what() << '\n';
        return kSdkError;
    }
    catch(const std::exception &error) {
        std::cerr << "ERROR: " << error.what() << '\n';
        return kSdkError;
    }
}
