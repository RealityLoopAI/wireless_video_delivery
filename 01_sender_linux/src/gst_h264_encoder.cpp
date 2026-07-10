#include "gwv3_sender/gst_h264_encoder.hpp"

#include <algorithm>
#include <mutex>
#include <stdexcept>

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/video/video-event.h>

namespace gwv3 {

namespace {

bool gst_element_supports_property(const std::string &element_name, const char *property_name) {
    GstElement *element = gst_element_factory_make(element_name.c_str(), nullptr);
    if(!element) {
        return false;
    }
    const bool supported = g_object_class_find_property(G_OBJECT_GET_CLASS(element), property_name) != nullptr;
    gst_object_unref(element);
    return supported;
}

}  // namespace

std::string h264_encoder_stage(const std::string &encoder_name, int bitrate_bps, int fps) {
    if(encoder_name == "x264enc") {
        const int bitrate_kbps = std::max(1, (bitrate_bps + 999) / 1000);
        return encoder_name + " name=enc bitrate=" + std::to_string(bitrate_kbps)
               + " key-int-max=" + std::to_string(fps)
               + " speed-preset=ultrafast tune=zerolatency byte-stream=true";
    }

    std::string stage = encoder_name + " name=enc bps=" + std::to_string(bitrate_bps) + " gop=" + std::to_string(fps) + " header-mode=1";
    if(encoder_name == "mpph264enc" && gst_element_supports_property(encoder_name, "max-pending")) {
        stage += " max-pending=2";
    }
    return stage;
}

std::string jpeg_decoder_stage(const std::string &encoder_name) {
    if(encoder_name == "x264enc") {
        return "! jpegparse ! jpegdec ! videoconvert ";
    }
    return "! jpegparse ! mppjpegdec fast-mode=true format=NV12 ";
}

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
            + jpeg_decoder_stage(encoder_name)
            + scale_stage +
            "! " + h264_encoder_stage(encoder_name, bitrate_bps, fps) + " "
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
            "! " + h264_encoder_stage(encoder_name, bitrate_bps, fps) + " "
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
    encoder_ = gst_bin_get_by_name(GST_BIN(pipeline_), "enc");
    appsink_ = gst_bin_get_by_name(GST_BIN(pipeline_), "sink");
    if(!appsrc_ || !encoder_ || !appsink_) {
        error_ = "failed to get appsrc/encoder/appsink";
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
    if(encoder_) {
        gst_object_unref(encoder_);
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

void GstH264Encoder::request_keyframe() {
    force_keyframe_pending_ = true;
}

void GstH264Encoder::send_pending_keyframe_event(uint64_t timestamp_us) {
    if(!force_keyframe_pending_ || !encoder_) {
        return;
    }
    force_keyframe_pending_ = false;
    (void)timestamp_us;
    GstEvent *event = gst_video_event_new_upstream_force_key_unit(GST_CLOCK_TIME_NONE,
                                                                  TRUE,
                                                                  force_keyframe_count_++);
    if(event && !gst_element_send_event(encoder_, event)) {
        // Ownership is transferred to gst_element_send_event even when it returns false.
    }
}

std::vector<EncodedH264Frame> GstH264Encoder::encode_bytes(const uint8_t *data, size_t size, uint64_t timestamp_us) {
    if(!ok_) {
        throw std::runtime_error("gstreamer encoder is not ready: " + error_);
    }

    send_pending_keyframe_event(timestamp_us);

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

GstJpegDualH264Encoder::GstJpegDualH264Encoder(int width, int height, int fps, int main_bitrate_bps, const std::string &encoder_name,
                                               int preview_width, int preview_height, int preview_fps, int preview_bitrate_bps)
    : fps_(fps),
      preview_fps_(preview_fps > 0 ? preview_fps : fps),
      width_(width),
      height_(height),
      preview_width_(preview_width),
      preview_height_(preview_height) {
    static std::once_flag gst_init_once;
    std::call_once(gst_init_once, [] { gst_init(nullptr, nullptr); });

    if(width_ <= 0 || height_ <= 0 || preview_width_ <= 0 || preview_height_ <= 0) {
        error_ = "invalid dual encoder dimensions";
        return;
    }

    std::string main_encoder_stage = h264_encoder_stage(encoder_name, main_bitrate_bps, fps_);
    std::string preview_encoder_stage = h264_encoder_stage(encoder_name, preview_bitrate_bps, preview_fps_);
    const auto replace_name = [](std::string &stage, const std::string &name) {
        const std::string from = " name=enc ";
        const auto pos = stage.find(from);
        if(pos != std::string::npos) {
            stage.replace(pos, from.size(), " name=" + name + " ");
        }
    };
    replace_name(main_encoder_stage, "main_enc");
    replace_name(preview_encoder_stage, "preview_enc");

    const std::string source_caps = "video/x-raw,format=NV12,width=" + std::to_string(width_) + ",height=" + std::to_string(height_)
                                    + ",framerate=" + std::to_string(fps_) + "/1 ";
    const std::string preview_caps = "video/x-raw,format=NV12,width=" + std::to_string(preview_width_)
                                     + ",height=" + std::to_string(preview_height_) + ",framerate=" + std::to_string(preview_fps_) + "/1 ";

    const std::string pipeline_text =
        "appsrc name=src is-live=true block=false leaky-type=downstream max-buffers=2 max-bytes=0 format=time do-timestamp=false "
        "caps=image/jpeg,framerate=" + std::to_string(fps_) + "/1 "
        "! queue max-size-buffers=2 leaky=downstream "
        + jpeg_decoder_stage(encoder_name)
        + "! " + source_caps
        + "! tee name=t allow-not-linked=true "
        "t. ! queue max-size-buffers=2 leaky=downstream "
        "! " + main_encoder_stage + " "
        "! h264parse "
        "! video/x-h264,stream-format=byte-stream,alignment=au "
        "! appsink name=main_sink emit-signals=false sync=false max-buffers=8 drop=true "
        "t. ! queue max-size-buffers=2 leaky=downstream "
        "! valve name=preview_valve drop=true drop-mode=transform-to-gap "
        "! videoscale "
        "! " + preview_caps
        + "! " + preview_encoder_stage + " "
        "! h264parse "
        "! video/x-h264,stream-format=byte-stream,alignment=au "
        "! appsink name=preview_sink emit-signals=false sync=false max-buffers=8 drop=true";

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
    main_encoder_ = gst_bin_get_by_name(GST_BIN(pipeline_), "main_enc");
    preview_encoder_ = gst_bin_get_by_name(GST_BIN(pipeline_), "preview_enc");
    preview_valve_ = gst_bin_get_by_name(GST_BIN(pipeline_), "preview_valve");
    main_sink_ = gst_bin_get_by_name(GST_BIN(pipeline_), "main_sink");
    preview_sink_ = gst_bin_get_by_name(GST_BIN(pipeline_), "preview_sink");
    if(!appsrc_ || !main_encoder_ || !preview_encoder_ || !preview_valve_ || !main_sink_ || !preview_sink_) {
        error_ = "failed to get dual encoder elements";
        return;
    }

    const auto state = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
    if(state == GST_STATE_CHANGE_FAILURE) {
        error_ = "failed to set dual gstreamer pipeline to PLAYING";
        return;
    }
    ok_ = true;
}

GstJpegDualH264Encoder::~GstJpegDualH264Encoder() {
    if(pipeline_) {
        gst_element_set_state(pipeline_, GST_STATE_NULL);
    }
    if(appsrc_) {
        gst_object_unref(appsrc_);
    }
    if(main_encoder_) {
        gst_object_unref(main_encoder_);
    }
    if(preview_encoder_) {
        gst_object_unref(preview_encoder_);
    }
    if(preview_valve_) {
        gst_object_unref(preview_valve_);
    }
    if(main_sink_) {
        gst_object_unref(main_sink_);
    }
    if(preview_sink_) {
        gst_object_unref(preview_sink_);
    }
    if(pipeline_) {
        gst_object_unref(pipeline_);
    }
}

void GstJpegDualH264Encoder::request_keyframe() {
    force_main_keyframe_pending_ = true;
}

void GstJpegDualH264Encoder::send_pending_keyframe_event(uint64_t timestamp_us, GstElement *encoder, bool &pending, uint32_t &count) {
    if(!pending || !encoder) {
        return;
    }
    pending = false;
    (void)timestamp_us;
    GstEvent *event = gst_video_event_new_upstream_force_key_unit(GST_CLOCK_TIME_NONE,
                                                                  TRUE,
                                                                  count++);
    if(event && !gst_element_send_event(encoder, event)) {
    }
}

std::vector<EncodedH264Frame> GstJpegDualH264Encoder::drain_sink(GstElement *sink, GstClockTime first_timeout) {
    std::vector<EncodedH264Frame> outputs;
    GstClockTime timeout = first_timeout;
    while(sink) {
        GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), timeout);
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

DualEncodedH264Frames GstJpegDualH264Encoder::encode_jpeg(const void *data, size_t size, uint64_t timestamp_us, bool preview_active) {
    if(!ok_) {
        throw std::runtime_error("dual gstreamer encoder is not ready: " + error_);
    }
    if(!data || size == 0) {
        return {};
    }

    if(preview_valve_) {
        g_object_set(G_OBJECT(preview_valve_), "drop", preview_active ? FALSE : TRUE, nullptr);
    }
    if(preview_active && !preview_active_) {
        bool preview_keyframe_pending = true;
        send_pending_keyframe_event(timestamp_us, preview_encoder_, preview_keyframe_pending, force_preview_keyframe_count_);
    }
    preview_active_ = preview_active;
    send_pending_keyframe_event(timestamp_us, main_encoder_, force_main_keyframe_pending_, force_main_keyframe_count_);

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

    DualEncodedH264Frames outputs;
    outputs.main = drain_sink(main_sink_, 20 * GST_MSECOND);
    outputs.preview = drain_sink(preview_sink_, preview_active ? 5 * GST_MSECOND : 0);
    return outputs;
}

}  // namespace gwv3
