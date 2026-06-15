#include "gwv3_sender/gst_h264_encoder.hpp"

#include <mutex>
#include <stdexcept>

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>

namespace gwv3 {

GstH264Encoder::GstH264Encoder(int width, int height, int fps, int bitrate_bps, const std::string &encoder_name,
                               GstH264InputFormat input_format, int output_width, int output_height)
    : fps_(fps), output_width_(output_width > 0 ? output_width : width), output_height_(output_height > 0 ? output_height : height) {
    static std::once_flag gst_init_once;
    std::call_once(gst_init_once, [] { gst_init(nullptr, nullptr); });

    const bool scale_output = output_width_ != width || output_height_ != height;
    const std::string scale_stage =
        scale_output ? ("! videoscale ! video/x-raw,format=NV12,width=" + std::to_string(output_width_)
                        + ",height=" + std::to_string(output_height_) + ",framerate=" + std::to_string(fps) + "/1 ")
                     : ("! video/x-raw,format=NV12,width=" + std::to_string(width) + ",height=" + std::to_string(height)
                        + ",framerate=" + std::to_string(fps) + "/1 ");

    std::string pipeline_text;
    if(input_format == GstH264InputFormat::Jpeg) {
        pipeline_text =
            "appsrc name=src is-live=true block=true format=time do-timestamp=false "
            "caps=image/jpeg,framerate=" + std::to_string(fps) + "/1 "
            "! queue max-size-buffers=2 leaky=downstream "
            "! jpegparse "
            "! mppjpegdec fast-mode=true format=NV12 "
            + scale_stage +
            "! " + encoder_name + " bps=" + std::to_string(bitrate_bps) + " gop=" + std::to_string(fps) + " header-mode=1 "
            "! h264parse "
            "! video/x-h264,stream-format=byte-stream,alignment=au "
            "! appsink name=sink emit-signals=false sync=false max-buffers=8 drop=true";
    }
    else {
        pipeline_text =
            "appsrc name=src is-live=true block=true format=time do-timestamp=false "
            "caps=video/x-raw,format=BGR,width=" + std::to_string(width) + ",height=" + std::to_string(height) + ",framerate=" + std::to_string(fps) + "/1 "
            "! queue max-size-buffers=2 leaky=downstream "
            "! videoconvert "
            + scale_stage +
            "! " + encoder_name + " bps=" + std::to_string(bitrate_bps) + " gop=" + std::to_string(fps) + " header-mode=1 "
            "! h264parse "
            "! video/x-h264,stream-format=byte-stream,alignment=au "
            "! appsink name=sink emit-signals=false sync=false max-buffers=8 drop=true";
    }

    GError *error = nullptr;
    pipeline_ = gst_parse_launch(pipeline_text.c_str(), &error);
    if(error != nullptr) {
        error_ = error->message;
        g_error_free(error);
        return;
    }
    if(!pipeline_) {
        error_ = "gst_parse_launch returned null";
        return;
    }

    appsrc_ = gst_bin_get_by_name(GST_BIN(pipeline_), "src");
    appsink_ = gst_bin_get_by_name(GST_BIN(pipeline_), "sink");
    if(!appsrc_ || !appsink_) {
        error_ = "failed to get appsrc/appsink";
        return;
    }

    const auto state = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
    if(state == GST_STATE_CHANGE_FAILURE) {
        error_ = "failed to set gstreamer pipeline to PLAYING";
        return;
    }
    ok_ = true;
}

GstH264Encoder::~GstH264Encoder() {
    if(pipeline_) {
        gst_element_set_state(pipeline_, GST_STATE_NULL);
    }
    if(appsrc_) {
        gst_object_unref(appsrc_);
    }
    if(appsink_) {
        gst_object_unref(appsink_);
    }
    if(pipeline_) {
        gst_object_unref(pipeline_);
    }
}

std::vector<EncodedH264Frame> GstH264Encoder::encode_bgr(const cv::Mat &bgr, uint64_t timestamp_us) {
    if(!ok_) {
        throw std::runtime_error("gstreamer encoder is not ready: " + error_);
    }
    if(!bgr.isContinuous()) {
        return encode_bgr(bgr.clone(), timestamp_us);
    }

    const size_t size = bgr.total() * bgr.elemSize();
    return encode_bytes(bgr.data, size, timestamp_us);
}

std::vector<EncodedH264Frame> GstH264Encoder::encode_jpeg(const void *data, size_t size, uint64_t timestamp_us) {
    if(!data || size == 0) {
        return {};
    }
    return encode_bytes(static_cast<const uint8_t *>(data), size, timestamp_us);
}

std::vector<EncodedH264Frame> GstH264Encoder::encode_bytes(const uint8_t *data, size_t size, uint64_t timestamp_us) {
    if(!ok_) {
        throw std::runtime_error("gstreamer encoder is not ready: " + error_);
    }

    GstBuffer *buffer = gst_buffer_new_allocate(nullptr, size, nullptr);
    gst_buffer_fill(buffer, 0, data, size);
    GST_BUFFER_PTS(buffer) = static_cast<GstClockTime>(timestamp_us) * GST_USECOND;
    GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
    GST_BUFFER_DURATION(buffer) = static_cast<GstClockTime>(1000000000ull / static_cast<uint64_t>(fps_));
    GST_BUFFER_OFFSET(buffer) = frame_index_++;

    const GstFlowReturn flow = gst_app_src_push_buffer(GST_APP_SRC(appsrc_), buffer);
    if(flow != GST_FLOW_OK) {
        throw std::runtime_error("gst_app_src_push_buffer failed");
    }

    std::vector<EncodedH264Frame> outputs;
    GstClockTime timeout = 20 * GST_MSECOND;
    while(true) {
        GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(appsink_), timeout);
        if(!sample) {
            break;
        }
        GstBuffer *encoded = gst_sample_get_buffer(sample);
        GstMapInfo map{};
        if(encoded && gst_buffer_map(encoded, &map, GST_MAP_READ)) {
            EncodedH264Frame frame;
            frame.data.assign(map.data, map.data + map.size);
            const GstClockTime pts = GST_BUFFER_PTS(encoded);
            if(GST_CLOCK_TIME_IS_VALID(pts)) {
                frame.pts_us = static_cast<uint64_t>(pts / GST_USECOND);
                frame.has_pts = true;
            }
            outputs.push_back(std::move(frame));
            gst_buffer_unmap(encoded, &map);
        }
        gst_sample_unref(sample);
        timeout = 0;
    }
    return outputs;
}

}  // namespace gwv3
