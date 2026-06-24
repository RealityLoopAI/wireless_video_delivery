#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <gst/gst.h>
#include <opencv2/core.hpp>

namespace gwv3 {

enum class GstH264InputFormat {
    Bgr,
    Jpeg,
};

struct EncodedH264Frame {
    std::vector<uint8_t> data;
    uint64_t pts_us = 0;
    bool has_pts = false;
};

struct DualEncodedH264Frames {
    std::vector<EncodedH264Frame> main;
    std::vector<EncodedH264Frame> preview;
};

class GstH264Encoder {
public:
    GstH264Encoder(int width, int height, int fps, int bitrate_bps, const std::string &encoder_name,
                   GstH264InputFormat input_format = GstH264InputFormat::Bgr, int output_width = 0, int output_height = 0);
    ~GstH264Encoder();

    GstH264Encoder(const GstH264Encoder &) = delete;
    GstH264Encoder &operator=(const GstH264Encoder &) = delete;

    std::vector<EncodedH264Frame> encode_bgr(const cv::Mat &bgr, uint64_t timestamp_us);
    std::vector<EncodedH264Frame> encode_jpeg(const void *data, size_t size, uint64_t timestamp_us);
    void request_keyframe();
    bool ok() const { return ok_; }
    std::string error() const { return error_; }
    int output_width() const { return output_width_; }
    int output_height() const { return output_height_; }

private:
    std::vector<EncodedH264Frame> encode_bytes(const uint8_t *data, size_t size, uint64_t timestamp_us);
    void send_pending_keyframe_event(uint64_t timestamp_us);

    GstElement *pipeline_ = nullptr;
    GstElement *appsrc_ = nullptr;
    GstElement *encoder_ = nullptr;
    GstElement *appsink_ = nullptr;
    bool ok_ = false;
    std::string error_;
    uint64_t frame_index_ = 0;
    uint32_t force_keyframe_count_ = 0;
    bool force_keyframe_pending_ = false;
    int fps_ = 30;
    int output_width_ = 0;
    int output_height_ = 0;
};

class GstJpegDualH264Encoder {
public:
    GstJpegDualH264Encoder(int width, int height, int fps, int main_bitrate_bps, const std::string &encoder_name,
                           int preview_width, int preview_height, int preview_fps, int preview_bitrate_bps);
    ~GstJpegDualH264Encoder();

    GstJpegDualH264Encoder(const GstJpegDualH264Encoder &) = delete;
    GstJpegDualH264Encoder &operator=(const GstJpegDualH264Encoder &) = delete;

    DualEncodedH264Frames encode_jpeg(const void *data, size_t size, uint64_t timestamp_us, bool preview_active);
    void request_keyframe();
    bool ok() const { return ok_; }
    std::string error() const { return error_; }
    int output_width() const { return width_; }
    int output_height() const { return height_; }
    int preview_output_width() const { return preview_width_; }
    int preview_output_height() const { return preview_height_; }

private:
    std::vector<EncodedH264Frame> drain_sink(GstElement *sink, GstClockTime first_timeout);
    void send_pending_keyframe_event(uint64_t timestamp_us, GstElement *encoder, bool &pending, uint32_t &count);

    GstElement *pipeline_ = nullptr;
    GstElement *appsrc_ = nullptr;
    GstElement *main_encoder_ = nullptr;
    GstElement *preview_encoder_ = nullptr;
    GstElement *preview_valve_ = nullptr;
    GstElement *main_sink_ = nullptr;
    GstElement *preview_sink_ = nullptr;
    bool ok_ = false;
    std::string error_;
    uint64_t frame_index_ = 0;
    uint32_t force_main_keyframe_count_ = 0;
    uint32_t force_preview_keyframe_count_ = 0;
    bool force_main_keyframe_pending_ = false;
    bool preview_active_ = false;
    int fps_ = 30;
    int preview_fps_ = 30;
    int width_ = 0;
    int height_ = 0;
    int preview_width_ = 0;
    int preview_height_ = 0;
};

}  // namespace gwv3
