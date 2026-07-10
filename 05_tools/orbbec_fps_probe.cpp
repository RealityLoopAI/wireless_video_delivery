#include "libobsensor/hpp/Context.hpp"
#include "libobsensor/hpp/Device.hpp"
#include "libobsensor/hpp/Error.hpp"
#include "libobsensor/hpp/Frame.hpp"
#include "libobsensor/hpp/Pipeline.hpp"
#include "libobsensor/hpp/StreamProfile.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace {

struct Options {
    std::string serial = "AY2MC31010W";
    std::string streams = "both";
    std::string aggregate = "any";
    int seconds = 12;
    int device_index = -1;
    int auto_exposure = -1;
    int auto_exposure_priority = -1;
    int exposure = -1;
    int gain = -1;
    int power_line_frequency = -1;
    int max_exposure = -1;
    int auto_white_balance = -1;
    int white_balance = -1;
    int brightness = -1000;
    int contrast = -1;
    int saturation = -1;
    int gamma = -1;
    int backlight_compensation = -1;
    int color_width = 1920;
    int color_height = 1080;
    int depth_width = 320;
    int depth_height = 200;
    int fps = 30;
    std::string color_format = "mjpg";
    std::string depth_format = "y16";
    std::string save_prefix;
    int save_after_frames = 30;
};

void usage(const char *argv0) {
    std::cerr << "usage: " << argv0
              << " [--serial SN] [--streams color|both] [--aggregate any|full|color|disable]"
                 " [--seconds N] [--index N] [--auto-exposure 0|1] [--exposure N] [--gain N]"
                 " [--ae-priority 0|1] [--power-line 0|1|2] [--max-exposure N]"
                 " [--auto-white-balance 0|1] [--white-balance N] [--brightness N] [--contrast N]"
                 " [--saturation N] [--gamma N] [--backlight N]"
                 " [--color-width N] [--color-height N] [--depth-width N] [--depth-height N] [--fps N]"
                 " [--color-format mjpg|rgb|yuyv] [--depth-format y16|y12]"
                 " [--save-prefix PATH] [--save-after-frames N]\n";
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
        if(arg == "--serial") {
            options.serial = require_value("--serial");
        }
        else if(arg == "--streams") {
            options.streams = require_value("--streams");
        }
        else if(arg == "--aggregate") {
            options.aggregate = require_value("--aggregate");
        }
        else if(arg == "--seconds") {
            options.seconds = std::stoi(require_value("--seconds"));
        }
        else if(arg == "--index") {
            options.device_index = std::stoi(require_value("--index"));
        }
        else if(arg == "--ae-priority") {
            options.auto_exposure_priority = std::stoi(require_value("--ae-priority"));
        }
        else if(arg == "--auto-exposure") {
            options.auto_exposure = std::stoi(require_value("--auto-exposure"));
        }
        else if(arg == "--exposure") {
            options.exposure = std::stoi(require_value("--exposure"));
        }
        else if(arg == "--gain") {
            options.gain = std::stoi(require_value("--gain"));
        }
        else if(arg == "--power-line") {
            options.power_line_frequency = std::stoi(require_value("--power-line"));
        }
        else if(arg == "--max-exposure") {
            options.max_exposure = std::stoi(require_value("--max-exposure"));
        }
        else if(arg == "--auto-white-balance") {
            options.auto_white_balance = std::stoi(require_value("--auto-white-balance"));
        }
        else if(arg == "--white-balance") {
            options.white_balance = std::stoi(require_value("--white-balance"));
        }
        else if(arg == "--brightness") {
            options.brightness = std::stoi(require_value("--brightness"));
        }
        else if(arg == "--contrast") {
            options.contrast = std::stoi(require_value("--contrast"));
        }
        else if(arg == "--saturation") {
            options.saturation = std::stoi(require_value("--saturation"));
        }
        else if(arg == "--gamma") {
            options.gamma = std::stoi(require_value("--gamma"));
        }
        else if(arg == "--backlight") {
            options.backlight_compensation = std::stoi(require_value("--backlight"));
        }
        else if(arg == "--color-width") {
            options.color_width = std::stoi(require_value("--color-width"));
        }
        else if(arg == "--color-height") {
            options.color_height = std::stoi(require_value("--color-height"));
        }
        else if(arg == "--depth-width") {
            options.depth_width = std::stoi(require_value("--depth-width"));
        }
        else if(arg == "--depth-height") {
            options.depth_height = std::stoi(require_value("--depth-height"));
        }
        else if(arg == "--fps") {
            options.fps = std::stoi(require_value("--fps"));
        }
        else if(arg == "--color-format") {
            options.color_format = require_value("--color-format");
        }
        else if(arg == "--depth-format") {
            options.depth_format = require_value("--depth-format");
        }
        else if(arg == "--save-prefix") {
            options.save_prefix = require_value("--save-prefix");
        }
        else if(arg == "--save-after-frames") {
            options.save_after_frames = std::stoi(require_value("--save-after-frames"));
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

OBFrameAggregateOutputMode aggregate_mode(const std::string &value) {
    if(value == "any") {
        return OB_FRAME_AGGREGATE_OUTPUT_ANY_SITUATION;
    }
    if(value == "full") {
        return OB_FRAME_AGGREGATE_OUTPUT_FULL_FRAME_REQUIRE;
    }
    if(value == "color") {
        return OB_FRAME_AGGREGATE_OUTPUT_COLOR_FRAME_REQUIRE;
    }
    if(value == "disable") {
        return OB_FRAME_AGGREGATE_OUTPUT_DISABLE;
    }
    throw std::runtime_error("invalid aggregate mode: " + value);
}

OBFormat video_format(const std::string &value) {
    if(value == "mjpg") {
        return OB_FORMAT_MJPG;
    }
    if(value == "rgb") {
        return OB_FORMAT_RGB;
    }
    if(value == "yuyv") {
        return OB_FORMAT_YUYV;
    }
    if(value == "y16") {
        return OB_FORMAT_Y16;
    }
    if(value == "y12") {
        return OB_FORMAT_Y12;
    }
    throw std::runtime_error("invalid video format: " + value);
}

const char *safe(const char *value) {
    return value ? value : "";
}

void set_int_if_supported(const std::shared_ptr<ob::Device> &device, OBPropertyID property, const char *name, int value) {
    if(value < 0) {
        return;
    }
    if(!device->isPropertySupported(property, OB_PERMISSION_WRITE)) {
        std::cout << "property unsupported for write name=" << name << "\n";
        return;
    }
    device->setIntProperty(property, value);
    std::cout << "property set name=" << name << " value=" << value;
    if(device->isPropertySupported(property, OB_PERMISSION_READ)) {
        std::cout << " readback=" << device->getIntProperty(property);
    }
    std::cout << "\n";
}

void set_bool_if_supported(const std::shared_ptr<ob::Device> &device, OBPropertyID property, const char *name, int value) {
    if(value < 0) {
        return;
    }
    if(!device->isPropertySupported(property, OB_PERMISSION_WRITE)) {
        std::cout << "property unsupported for write name=" << name << "\n";
        return;
    }
    const bool bool_value = value != 0;
    device->setBoolProperty(property, bool_value);
    std::cout << "property set name=" << name << " value=" << (bool_value ? 1 : 0);
    if(device->isPropertySupported(property, OB_PERMISSION_READ)) {
        std::cout << " readback=" << (device->getBoolProperty(property) ? 1 : 0);
    }
    std::cout << "\n";
}

std::shared_ptr<ob::VideoStreamProfile> profile(ob::Pipeline &pipeline, OBSensorType sensor, int width, int height, OBFormat format, int fps) {
    auto profiles = pipeline.getStreamProfileList(sensor);
    try {
        return profiles->getVideoStreamProfile(width, height, format, fps);
    }
    catch(const ob::Error &e) {
        std::cout << "profile fallback sensor=" << sensor << " error=" << e.getMessage() << "\n";
        return profiles->getProfile(OB_PROFILE_DEFAULT)->as<ob::VideoStreamProfile>();
    }
}

std::shared_ptr<ob::Device> select_device(const std::shared_ptr<ob::DeviceList> &devices, const Options &options) {
    if(options.device_index >= 0) {
        return devices->getDevice(static_cast<uint32_t>(options.device_index));
    }
    for(uint32_t i = 0; i < devices->deviceCount(); ++i) {
        auto device = devices->getDevice(i);
        auto info = device->getDeviceInfo();
        const std::string serial = safe(info->serialNumber());
        std::cout << "opened index=" << i << " sn=" << serial << " uid=" << safe(info->uid()) << " conn=" << safe(info->connectionType())
                  << "\n";
        if(serial == options.serial) {
            return device;
        }
    }
    throw std::runtime_error("device serial not found: " + options.serial);
}

void save_color_snapshot(const std::shared_ptr<ob::ColorFrame> &color, const std::string &prefix) {
    if(!color || prefix.empty()) {
        return;
    }
    const std::string extension = color->format() == OB_FORMAT_MJPG ? ".jpg" : ".raw";
    const std::string path = prefix + "_frame" + std::to_string(color->index()) + "_" + std::to_string(color->width()) + "x"
                             + std::to_string(color->height()) + extension;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if(!out) {
        throw std::runtime_error("failed to open snapshot for write: " + path);
    }
    out.write(static_cast<const char *>(color->data()), static_cast<std::streamsize>(color->dataSize()));
    if(!out) {
        throw std::runtime_error("failed to write snapshot: " + path);
    }
    std::cout << "saved color snapshot path=" << path << " bytes=" << color->dataSize() << " format=" << color->format() << "\n";
}

}  // namespace

int main(int argc, char **argv) {
    try {
        const auto options = parse_args(argc, argv);
        if(options.streams != "color" && options.streams != "both") {
            throw std::runtime_error("invalid streams: " + options.streams);
        }

        ob::Context::setLoggerSeverity(OB_LOG_SEVERITY_WARN);
        auto context = std::make_shared<ob::Context>();
        auto devices = context->queryDeviceList();
        std::cout << "devices=" << devices->deviceCount() << "\n";
        for(uint32_t i = 0; i < devices->deviceCount(); ++i) {
            std::cout << "device index=" << i << " sn=" << safe(devices->serialNumber(i)) << " uid=" << safe(devices->uid(i))
                      << " conn=" << safe(devices->connectionType(i)) << "\n";
        }

        auto device = select_device(devices, options);
        auto info = device->getDeviceInfo();
        std::cout << "selected sn=" << safe(info->serialNumber()) << " name=" << safe(info->name()) << " fw=" << safe(info->firmwareVersion())
                  << " conn=" << safe(info->connectionType()) << " uid=" << safe(info->uid()) << "\n";

        ob::Pipeline pipeline(device);
        auto config = std::make_shared<ob::Config>();
        auto color_profile =
            profile(pipeline, OB_SENSOR_COLOR, options.color_width, options.color_height, video_format(options.color_format), options.fps);
        config->enableStream(color_profile);
        std::shared_ptr<ob::VideoStreamProfile> depth_profile;
        if(options.streams == "both") {
            depth_profile =
                profile(pipeline, OB_SENSOR_DEPTH, options.depth_width, options.depth_height, video_format(options.depth_format), options.fps);
            config->enableStream(depth_profile);
        }
        if(options.aggregate != "disable") {
            config->setFrameAggregateOutputMode(aggregate_mode(options.aggregate));
        }

        std::cout << "start streams=" << options.streams << " aggregate=" << options.aggregate << " color=" << color_profile->width() << "x"
                  << color_profile->height() << "@" << color_profile->fps() << " depth="
                  << (depth_profile ? std::to_string(depth_profile->width()) + "x" + std::to_string(depth_profile->height()) + "@"
                                          + std::to_string(depth_profile->fps())
                                    : "off")
                  << "\n";

        pipeline.start(config);

        if(options.auto_exposure == 0) {
            set_bool_if_supported(device, OB_PROP_COLOR_AUTO_EXPOSURE_BOOL, "auto_exposure", options.auto_exposure);
        }
        set_int_if_supported(device, OB_PROP_COLOR_AUTO_EXPOSURE_PRIORITY_INT, "auto_exposure_priority", options.auto_exposure_priority);
        set_int_if_supported(device, OB_PROP_COLOR_POWER_LINE_FREQUENCY_INT, "power_line_frequency", options.power_line_frequency);
        set_int_if_supported(device, OB_PROP_COLOR_AE_MAX_EXPOSURE_INT, "max_exposure", options.max_exposure);
        set_int_if_supported(device, OB_PROP_COLOR_EXPOSURE_INT, "exposure", options.exposure);
        set_int_if_supported(device, OB_PROP_COLOR_GAIN_INT, "gain", options.gain);
        if(options.auto_white_balance == 0) {
            set_bool_if_supported(device, OB_PROP_COLOR_AUTO_WHITE_BALANCE_BOOL, "auto_white_balance", options.auto_white_balance);
        }
        set_int_if_supported(device, OB_PROP_COLOR_WHITE_BALANCE_INT, "white_balance", options.white_balance);
        set_int_if_supported(device, OB_PROP_COLOR_BRIGHTNESS_INT, "brightness", options.brightness);
        set_int_if_supported(device, OB_PROP_COLOR_CONTRAST_INT, "contrast", options.contrast);
        set_int_if_supported(device, OB_PROP_COLOR_SATURATION_INT, "saturation", options.saturation);
        set_int_if_supported(device, OB_PROP_COLOR_GAMMA_INT, "gamma", options.gamma);
        set_int_if_supported(device, OB_PROP_COLOR_BACKLIGHT_COMPENSATION_INT, "backlight_compensation", options.backlight_compensation);
        if(options.auto_white_balance > 0) {
            set_bool_if_supported(device, OB_PROP_COLOR_AUTO_WHITE_BALANCE_BOOL, "auto_white_balance", options.auto_white_balance);
        }
        if(options.auto_exposure > 0) {
            set_bool_if_supported(device, OB_PROP_COLOR_AUTO_EXPOSURE_BOOL, "auto_exposure", options.auto_exposure);
        }

        uint64_t framesets = 0;
        uint64_t timeouts = 0;
        uint64_t color_count = 0;
        uint64_t depth_count = 0;
        uint64_t both_count = 0;
        uint64_t color_id_first = 0;
        uint64_t color_id_last = 0;
        uint64_t depth_id_first = 0;
        uint64_t depth_id_last = 0;
        const auto started = std::chrono::steady_clock::now();
        auto last = started;
        uint64_t last_color = 0;
        uint64_t last_depth = 0;
        uint64_t last_framesets = 0;
        bool snapshot_saved = false;
        while(std::chrono::steady_clock::now() - started < std::chrono::seconds(options.seconds)) {
            auto frameset = pipeline.waitForFrames(200);
            if(!frameset) {
                ++timeouts;
                continue;
            }
            ++framesets;
            auto color = frameset->colorFrame();
            auto depth = frameset->depthFrame();
            if(color) {
                if(color_count == 0) {
                    color_id_first = color->index();
                }
                color_id_last = color->index();
                ++color_count;
                if(!snapshot_saved && !options.save_prefix.empty() && color_count >= static_cast<uint64_t>(std::max(1, options.save_after_frames))) {
                    save_color_snapshot(color, options.save_prefix);
                    snapshot_saved = true;
                }
            }
            if(depth) {
                if(depth_count == 0) {
                    depth_id_first = depth->index();
                }
                depth_id_last = depth->index();
                ++depth_count;
            }
            if(color && depth) {
                ++both_count;
            }
            const auto now = std::chrono::steady_clock::now();
            if(now - last >= std::chrono::seconds(1)) {
                const double seconds = std::chrono::duration<double>(now - last).count();
                std::cout << "sample framesets_fps=" << (framesets - last_framesets) / seconds << " color_fps="
                          << (color_count - last_color) / seconds << " depth_fps=" << (depth_count - last_depth) / seconds
                          << " both=" << both_count << " timeouts=" << timeouts << "\n";
                last = now;
                last_color = color_count;
                last_depth = depth_count;
                last_framesets = framesets;
            }
        }
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        pipeline.stop();

        std::cout << "summary seconds=" << seconds << " framesets=" << framesets << " color=" << color_count << " depth=" << depth_count
                  << " both=" << both_count << " timeouts=" << timeouts << " color_fps=" << color_count / seconds
                  << " depth_fps=" << depth_count / seconds << " color_id_delta="
                  << (color_count > 0 ? color_id_last - color_id_first : 0) << " depth_id_delta="
                  << (depth_count > 0 ? depth_id_last - depth_id_first : 0) << "\n";
        return 0;
    }
    catch(const ob::Error &e) {
        std::cerr << "Orbbec SDK error: " << e.getMessage() << "\n";
        return 2;
    }
    catch(const std::exception &e) {
        std::cerr << "error: " << e.what() << "\n";
        usage(argv[0]);
        return 1;
    }
}
