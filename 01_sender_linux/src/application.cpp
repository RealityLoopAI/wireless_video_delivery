#include "gwv3_sender/application.hpp"

#include "gwv3_common/protocol.hpp"
#include "gwv3_sender/adaptive_exposure_controller.hpp"
#include "gwv3_sender/clock_sync_client.hpp"
#include "gwv3_sender/config.hpp"
#include "gwv3_sender/gst_h264_encoder.hpp"
#include "gwv3_sender/gst_h264_rtp_sender.hpp"
#include "gwv3_sender/logger.hpp"
#include "gwv3_sender/media_outage_guard.hpp"
#include "gwv3_sender/rgb_transport_recovery.hpp"
#include "gwv3_sender/scheduled_keyframe.hpp"
#include "gwv3_sender/transport.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cerrno>
#include <cctype>
#include <csetjmp>
#include <cstdio>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <vector>

#include <json/json.h>
#include <jpeglib.h>
#include <libobsensor/ObSensor.hpp>
#include <libobsensor/hpp/Error.hpp>
#include <libobsensor/h/Version.h>
#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <zlib.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace gwv3 {
namespace {

#include "detail/runtime.inl"
#include "detail/camera_support.inl"
#include "detail/depth_compression.inl"
#include "detail/preview_control.inl"
#include "detail/camera_capture.inl"
#include "detail/hotplug_and_run.inl"

}  // namespace

int run_sender_application(int argc, char **argv) {
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    try {
        const Args args = parse_args(argc, argv);
        AppConfig config = load_config(args.config_path);
        Logger logger(config.logging.directory, config.logging.max_bytes);

        if(args.validate_only) {
            logger.info("config validation ok: " + args.config_path);
            return 0;
        }

        const std::string sdk_version = std::to_string(ob_get_major_version()) + "." + std::to_string(ob_get_minor_version()) + "."
                                        + std::to_string(ob_get_patch_version());
        logger.info("gemini sender starting, sender_id=" + config.sender_id + ", receiver=" + config.receiver.ip
                    + ", orbbec_sdk=" + sdk_version);
        if(args.no_send) {
            NullTransport transport;
            auto make_media_sender = [] {
                return std::make_unique<NullTransport>();
            };
            run_sender<NullTransport, NullTransport>(config, args, transport, make_media_sender, transport, logger);
        }
        else {
            Transport status_transport(config);
            Transport preview_transport(config);
            auto make_media_sender = [&config] {
                return std::make_unique<Transport>(config);
            };
            run_sender<Transport, Transport>(config, args, status_transport, make_media_sender, preview_transport, logger);
        }
        logger.info("gemini sender stopped");
        return 0;
    }
    catch(const ob::Error &e) {
        std::cerr << "Orbbec SDK error: " << e.getMessage() << std::endl;
        return 2;
    }
    catch(const std::exception &e) {
        std::cerr << "sender error: " << e.what() << std::endl;
        return 1;
    }
}

}  // namespace gwv3
