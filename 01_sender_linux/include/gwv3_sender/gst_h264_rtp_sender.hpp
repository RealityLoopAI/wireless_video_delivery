#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <gst/gst.h>

#include "gwv3_sender/config.hpp"

namespace gwv3 {

class GstH264RtpSender {
public:
    GstH264RtpSender(const RgbRtpOutputConfig &config, int fps);
    ~GstH264RtpSender();

    GstH264RtpSender(const GstH264RtpSender &) = delete;
    GstH264RtpSender &operator=(const GstH264RtpSender &) = delete;

    bool push(const uint8_t *data, size_t size, uint64_t timestamp_us);
    bool ok() const { return ok_; }
    std::string error() const { return error_; }

private:
    bool poll_bus_error();

    GstElement *pipeline_ = nullptr;
    GstElement *appsrc_ = nullptr;
    GstBus *bus_ = nullptr;
    bool ok_ = false;
    std::string error_;
    uint64_t frame_index_ = 0;
    int fps_ = 30;
};

}  // namespace gwv3
