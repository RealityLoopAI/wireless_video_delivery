#include "gwv3_sender/gst_h264_rtp_sender.hpp"

#include <algorithm>
#include <mutex>

#include <gst/app/gstappsrc.h>

namespace gwv3 {

GstH264RtpSender::GstH264RtpSender(const RgbRtpOutputConfig &config, int fps)
    : fps_(std::max(1, fps)) {
    static std::once_flag gst_init_once;
    std::call_once(gst_init_once, [] { gst_init(nullptr, nullptr); });

    const std::string pipeline_text =
        "appsrc name=src is-live=true block=false leaky-type=downstream max-buffers=2 max-bytes=0 "
        "format=time do-timestamp=false "
        "caps=video/x-h264,stream-format=byte-stream,alignment=au,framerate=" + std::to_string(fps_) + "/1 "
        "! queue max-size-buffers=2 max-size-bytes=0 max-size-time=0 leaky=downstream "
        "! h264parse "
        "! rtph264pay config-interval=1 pt=" + std::to_string(config.payload_type)
        + " mtu=" + std::to_string(config.mtu_bytes)
        + " ! udpsink host=" + config.host
        + " port=" + std::to_string(config.port)
        + " sync=false async=false qos=false";

    GError *error = nullptr;
    pipeline_ = gst_parse_launch(pipeline_text.c_str(), &error);
    if(error) {
        error_ = error->message;
        g_error_free(error);
        return;
    }
    if(!pipeline_) {
        error_ = "gst_parse_launch returned null";
        return;
    }

    appsrc_ = gst_bin_get_by_name(GST_BIN(pipeline_), "src");
    bus_ = gst_element_get_bus(pipeline_);
    if(!appsrc_ || !bus_) {
        error_ = "failed to get RTP appsrc or bus";
        return;
    }
    if(gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        error_ = "failed to set RTP pipeline to PLAYING";
        return;
    }
    ok_ = true;
}

GstH264RtpSender::~GstH264RtpSender() {
    if(pipeline_) {
        gst_element_set_state(pipeline_, GST_STATE_NULL);
    }
    if(bus_) {
        gst_object_unref(bus_);
    }
    if(appsrc_) {
        gst_object_unref(appsrc_);
    }
    if(pipeline_) {
        gst_object_unref(pipeline_);
    }
}

bool GstH264RtpSender::poll_bus_error() {
    if(!bus_) {
        return false;
    }
    GstMessage *message = gst_bus_pop_filtered(bus_, GST_MESSAGE_ERROR);
    if(!message) {
        return false;
    }
    GError *error = nullptr;
    gchar *debug = nullptr;
    gst_message_parse_error(message, &error, &debug);
    error_ = error && error->message ? error->message : "unknown RTP pipeline error";
    if(error) {
        g_error_free(error);
    }
    g_free(debug);
    gst_message_unref(message);
    ok_ = false;
    return true;
}

bool GstH264RtpSender::push(const uint8_t *data, size_t size, uint64_t timestamp_us) {
    if(!ok_ || !appsrc_ || !data || size == 0 || poll_bus_error()) {
        return false;
    }

    GstBuffer *buffer = gst_buffer_new_allocate(nullptr, size, nullptr);
    if(!buffer) {
        error_ = "failed to allocate RTP input buffer";
        return false;
    }
    gst_buffer_fill(buffer, 0, data, size);
    GST_BUFFER_PTS(buffer) = static_cast<GstClockTime>(timestamp_us) * GST_USECOND;
    GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
    GST_BUFFER_DURATION(buffer) = static_cast<GstClockTime>(GST_SECOND / static_cast<uint64_t>(fps_));
    GST_BUFFER_OFFSET(buffer) = frame_index_++;

    const GstFlowReturn flow = gst_app_src_push_buffer(GST_APP_SRC(appsrc_), buffer);
    if(flow != GST_FLOW_OK) {
        error_ = "gst_app_src_push_buffer failed with flow=" + std::to_string(static_cast<int>(flow));
        return false;
    }
    return !poll_bus_error();
}

}  // namespace gwv3
