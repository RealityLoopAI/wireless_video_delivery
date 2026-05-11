#pragma once

#include <fstream>
#include <mutex>
#include <string>

namespace gwv3 {

class Logger {
public:
    Logger(std::string directory, size_t max_bytes);

    void info(const std::string &message);
    void warn(const std::string &message);
    void error(const std::string &message);

private:
    void write(const std::string &level, const std::string &message);
    void rotate_if_needed();

    std::string log_path_;
    size_t max_bytes_;
    std::ofstream stream_;
    std::mutex mutex_;
};

uint64_t now_us();
std::string hostname();
std::string architecture();

}  // namespace gwv3
