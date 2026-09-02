#include "gwv3_receiver/application.hpp"

#include "gwv3_common/protocol.hpp"
#include "gwv3_receiver/clock_sync_manager.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdlib>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cmath>
#include <dlfcn.h>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <zlib.h>
#include <json/json.h>

extern char **environ;

namespace gwv3 {
namespace {

#include "detail/base.inl"
#include "detail/config_state.inl"
#include "detail/media_decode.inl"
#include "detail/recording.inl"
#include "detail/stream_state.inl"
#include "detail/receiver_app.inl"
#include "detail/cli.inl"

}  // namespace

int run_receiver_application(int argc, char **argv) {
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
    std::signal(SIGPIPE, SIG_IGN);

    try {
        const auto args = parse_args(argc, argv);
        auto config = load_config(args.config_path);
        ReceiverApp app(config);
        app.start();
        while(g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        app.stop();
        return 0;
    }
    catch(const std::exception &e) {
        std::cerr << "receiver error: " << e.what() << std::endl;
        return 1;
    }
}

}  // namespace gwv3
