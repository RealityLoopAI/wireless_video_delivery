#include "libobsensor/hpp/Context.hpp"
#include "libobsensor/hpp/Device.hpp"
#include "libobsensor/hpp/Error.hpp"
#include "libobsensor/hpp/Pipeline.hpp"
#include "libobsensor/hpp/StreamProfile.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace {

struct Options {
    int index = -1;
    std::string serial;
};

const char *safe(const char *value) {
    return value ? value : "";
}

void usage(const char *argv0) {
    std::cerr << "usage: " << argv0 << " [--index N | --serial SN]\n";
}

Options parse_args(int argc, char **argv) {
    Options options;
    for(int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto require_value = [&](const char *name) -> const char * {
            if(i + 1 >= argc) {
                throw std::runtime_error(std::string("missing value for ") + name);
            }
            return argv[++i];
        };
        if(arg == "--index") {
            options.index = std::stoi(require_value("--index"));
        }
        else if(arg == "--serial") {
            options.serial = require_value("--serial");
        }
        else if(arg == "--help" || arg == "-h") {
            usage(argv[0]);
            std::exit(0);
        }
        else {
            throw std::runtime_error("unknown argument: " + arg);
        }
    }
    return options;
}

std::string format_name(OBFormat format) {
    switch(format) {
    case OB_FORMAT_Y16:
        return "y16";
    case OB_FORMAT_MJPG:
        return "mjpg";
    case OB_FORMAT_RGB:
        return "rgb";
    case OB_FORMAT_BGR:
        return "bgr";
    case OB_FORMAT_YUYV:
        return "yuyv";
    case OB_FORMAT_UYVY:
        return "uyvy";
    case OB_FORMAT_Y8:
        return "y8";
    case OB_FORMAT_Y10:
        return "y10";
    case OB_FORMAT_Y11:
        return "y11";
    case OB_FORMAT_Y12:
        return "y12";
    case OB_FORMAT_Y14:
        return "y14";
    case OB_FORMAT_RLE:
        return "rle";
    default: {
        std::ostringstream oss;
        oss << "format_" << static_cast<int>(format);
        return oss.str();
    }
    }
}

std::shared_ptr<ob::Device> select_device(const std::shared_ptr<ob::DeviceList> &devices, const Options &options) {
    if(options.index >= 0) {
        return devices->getDevice(static_cast<uint32_t>(options.index));
    }
    if(!options.serial.empty()) {
        for(uint32_t i = 0; i < devices->deviceCount(); ++i) {
            auto device = devices->getDevice(i);
            const auto info = device->getDeviceInfo();
            if(options.serial == safe(info->serialNumber())) {
                return device;
            }
        }
        throw std::runtime_error("device serial not found: " + options.serial);
    }
    if(devices->deviceCount() == 0) {
        throw std::runtime_error("no Orbbec device found");
    }
    return devices->getDevice(0);
}

void print_int_property(const std::shared_ptr<ob::Device> &device, OBPropertyID property, const char *name) {
    const bool readable = device->isPropertySupported(property, OB_PERMISSION_READ);
    const bool writable = device->isPropertySupported(property, OB_PERMISSION_WRITE);
    if(!readable && !writable) {
        std::cout << "property " << name << " unsupported\n";
        return;
    }
    std::cout << "property " << name << " readable=" << (readable ? 1 : 0) << " writable=" << (writable ? 1 : 0);
    if(readable) {
        try {
            const auto range = device->getIntPropertyRange(property);
            std::cout << " cur=" << range.cur << " def=" << range.def << " min=" << range.min << " max=" << range.max
                      << " step=" << range.step;
        }
        catch(const ob::Error &e) {
            std::cout << " read_error=" << e.getMessage();
        }
    }
    std::cout << "\n";
}

void print_bool_property(const std::shared_ptr<ob::Device> &device, OBPropertyID property, const char *name) {
    const bool readable = device->isPropertySupported(property, OB_PERMISSION_READ);
    const bool writable = device->isPropertySupported(property, OB_PERMISSION_WRITE);
    if(!readable && !writable) {
        std::cout << "property " << name << " unsupported\n";
        return;
    }
    std::cout << "property " << name << " readable=" << (readable ? 1 : 0) << " writable=" << (writable ? 1 : 0);
    if(readable) {
        try {
            const auto range = device->getBoolPropertyRange(property);
            std::cout << " cur=" << (range.cur ? 1 : 0) << " def=" << (range.def ? 1 : 0);
        }
        catch(const ob::Error &e) {
            std::cout << " read_error=" << e.getMessage();
        }
    }
    std::cout << "\n";
}

void print_float_property(const std::shared_ptr<ob::Device> &device, OBPropertyID property, const char *name) {
    const bool readable = device->isPropertySupported(property, OB_PERMISSION_READ);
    const bool writable = device->isPropertySupported(property, OB_PERMISSION_WRITE);
    if(!readable && !writable) {
        std::cout << "property " << name << " unsupported\n";
        return;
    }
    std::cout << "property " << name << " readable=" << (readable ? 1 : 0) << " writable=" << (writable ? 1 : 0);
    if(readable) {
        try {
            const auto range = device->getFloatPropertyRange(property);
            std::cout << " cur=" << range.cur << " def=" << range.def << " min=" << range.min << " max=" << range.max
                      << " step=" << range.step;
        }
        catch(const ob::Error &e) {
            std::cout << " read_error=" << e.getMessage();
        }
    }
    std::cout << "\n";
}

void print_depth_work_modes(const std::shared_ptr<ob::Device> &device) {
    const bool supported = device->isPropertySupported(OB_STRUCT_CURRENT_DEPTH_ALG_MODE, OB_PERMISSION_READ_WRITE);
    std::cout << "depth_work_mode supported=" << (supported ? 1 : 0) << "\n";
    if(!supported) {
        return;
    }
    try {
        const auto current = device->getCurrentDepthWorkMode();
        std::cout << "depth_work_mode current=" << current.name << "\n";
        const auto modes = device->getDepthWorkModeList();
        std::cout << "depth_work_mode count=" << modes->count() << "\n";
        for(uint32_t i = 0; i < modes->count(); ++i) {
            const auto mode = (*modes)[i];
            std::cout << "depth_work_mode[" << i << "] name=" << mode.name;
            if(std::strcmp(mode.name, current.name) == 0) {
                std::cout << " current=1";
            }
            std::cout << "\n";
        }
    }
    catch(const ob::Error &e) {
        std::cout << "depth_work_mode error=" << e.getMessage() << "\n";
    }
}

void print_depth_profiles(const std::shared_ptr<ob::Device> &device) {
    ob::Pipeline pipeline(device);
    const auto profiles = pipeline.getStreamProfileList(OB_SENSOR_DEPTH);
    std::vector<std::tuple<uint32_t, uint32_t, uint32_t, std::string>> rows;
    rows.reserve(profiles->count());
    for(uint32_t i = 0; i < profiles->count(); ++i) {
        try {
            const auto profile = profiles->getProfile(i)->as<ob::VideoStreamProfile>();
            rows.emplace_back(profile->width(), profile->height(), profile->fps(), format_name(profile->format()));
        }
        catch(const std::exception &e) {
            std::cout << "depth_profile[" << i << "] error=" << e.what() << "\n";
        }
    }
    std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) {
        const auto area_a = std::get<0>(a) * std::get<1>(a);
        const auto area_b = std::get<0>(b) * std::get<1>(b);
        if(area_a != area_b) {
            return area_a > area_b;
        }
        if(std::get<2>(a) != std::get<2>(b)) {
            return std::get<2>(a) > std::get<2>(b);
        }
        return a < b;
    });
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());

    std::cout << "depth_profile count=" << rows.size() << "\n";
    for(size_t i = 0; i < rows.size(); ++i) {
        std::cout << "depth_profile[" << i << "] " << std::get<0>(rows[i]) << "x" << std::get<1>(rows[i]) << "@"
                  << std::get<2>(rows[i]) << " " << std::get<3>(rows[i]) << "\n";
    }
}

}  // namespace

int main(int argc, char **argv) {
    try {
        const auto options = parse_args(argc, argv);
        ob::Context::setLoggerSeverity(OB_LOG_SEVERITY_WARN);
        auto context = std::make_shared<ob::Context>();
        auto devices = context->queryDeviceList();
        std::cout << "devices=" << devices->deviceCount() << "\n";
        for(uint32_t i = 0; i < devices->deviceCount(); ++i) {
            std::cout << "device[" << i << "] sn=" << safe(devices->serialNumber(i)) << " uid=" << safe(devices->uid(i))
                      << " conn=" << safe(devices->connectionType(i)) << "\n";
        }

        auto device = select_device(devices, options);
        auto info = device->getDeviceInfo();
        std::cout << "selected sn=" << safe(info->serialNumber()) << " name=" << safe(info->name()) << " fw="
                  << safe(info->firmwareVersion()) << " hw=" << safe(info->hardwareVersion()) << " asic=" << safe(info->asicName())
                  << " conn=" << safe(info->connectionType()) << " uid=" << safe(info->uid()) << "\n";

        print_depth_work_modes(device);
        print_int_property(device, OB_PROP_DEPTH_PRECISION_LEVEL_INT, "depth_precision_level");
        print_float_property(device, OB_PROP_DEPTH_UNIT_FLEXIBLE_ADJUSTMENT_FLOAT, "depth_unit_flexible_adjustment");
        print_bool_property(device, OB_PROP_DEPTH_AUTO_EXPOSURE_BOOL, "depth_auto_exposure");
        print_int_property(device, OB_PROP_DEPTH_EXPOSURE_INT, "depth_exposure");
        print_int_property(device, OB_PROP_DEPTH_GAIN_INT, "depth_gain");
        print_bool_property(device, OB_PROP_DEPTH_SOFT_FILTER_BOOL, "depth_soft_filter");
        print_bool_property(device, OB_PROP_DEPTH_POSTFILTER_BOOL, "depth_postfilter");
        print_bool_property(device, OB_PROP_DEPTH_HOLEFILTER_BOOL, "depth_holefilter");
        print_depth_profiles(device);
        return 0;
    }
    catch(const ob::Error &e) {
        std::cerr << "Orbbec error function=" << e.getName() << " args=" << e.getArgs() << " message=" << e.getMessage()
                  << " type=" << e.getExceptionType() << "\n";
    }
    catch(const std::exception &e) {
        std::cerr << "error: " << e.what() << "\n";
    }
    return 1;
}
