#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <gst/gst.h>
#include <opencv2/core.hpp>

namespace gwv3 {

class GstH264Encoder {
public:
    GstH264Encoder(int width, int height, int fps, int bitrate_bps, const std::string &encoder_name);
    ~GstH264Encoder();

    GstH264Encoder(const GstH264Encoder &) = delete;
    GstH264Encoder &operator=(const GstH264Encoder &) = delete;

    std::vector<std::vector<uint8_t>> encode_bgr(const cv::Mat &bgr, uint64_t timestamp_us);
    bool ok() const { return ok_; }
    std::string error() const { return error_; }

private:
    GstElement *pipeline_ = nullptr;
    GstElement *appsrc_ = nullptr;
    GstElement *appsink_ = nullptr;
    bool ok_ = false;
    std::string error_;
    uint64_t frame_index_ = 0;
    int fps_ = 30;
};

}  // namespace gwv3
