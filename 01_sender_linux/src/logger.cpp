#include "gwv3_sender/logger.hpp"

#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <sys/utsname.h>
#include <unistd.h>

namespace gwv3 {

namespace fs = std::filesystem;

namespace {

std::string timestamp_text() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&time, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

}  // namespace

Logger::Logger(std::string directory, size_t max_bytes)
    : log_path_(std::move(directory) + "/sender.log"), max_bytes_(max_bytes) {
    fs::create_directories(fs::path(log_path_).parent_path());
    stream_.open(log_path_, std::ios::app);
    if(!stream_) {
        throw std::runtime_error("cannot open sender log: " + log_path_);
    }
}

void Logger::info(const std::string &message) {
    write("INFO", message);
}

void Logger::warn(const std::string &message) {
    write("WARN", message);
}

void Logger::error(const std::string &message) {
    write("ERROR", message);
}

void Logger::write(const std::string &level, const std::string &message) {
    std::lock_guard<std::mutex> lock(mutex_);
    rotate_if_needed();
    const auto line = timestamp_text() + " [" + level + "] " + message;
    stream_ << line << '\n';
    stream_.flush();
    std::cout << line << std::endl;
}

void Logger::rotate_if_needed() {
    if(max_bytes_ == 0 || !fs::exists(log_path_) || fs::file_size(log_path_) < max_bytes_) {
        return;
    }
    stream_.close();
    const auto rotated = log_path_ + ".1";
    std::error_code ec;
    fs::remove(rotated, ec);
    fs::rename(log_path_, rotated, ec);
    stream_.open(log_path_, std::ios::app);
}

uint64_t now_us() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

std::string hostname() {
    char buffer[256] = {};
    if(gethostname(buffer, sizeof(buffer) - 1) == 0) {
        return buffer;
    }
    return {};
}

std::string architecture() {
    utsname uts{};
    if(uname(&uts) == 0) {
        return uts.machine;
    }
    return {};
}

}  // namespace gwv3
