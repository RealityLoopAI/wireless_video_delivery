#include "gwv3_sender/gst_keyframe_request.hpp"

#include <iostream>

struct Observed {
    int events = 0;
    bool immediate = true;
    bool headers = true;
    guint count = 0;
};

int main() {
    gst_init(nullptr, nullptr);
    if(gwv3::send_immediate_keyframe_request(nullptr, 1)) return 1;
    GstElement *pipeline = gst_parse_launch("fakesrc ! fakesink name=sink", nullptr);
    if(!pipeline) return 1;
    GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    GstPad *pad = gst_element_get_static_pad(sink, "sink");
    Observed observed;
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_EVENT_UPSTREAM,
        [](GstPad *, GstPadProbeInfo *info, gpointer data) {
            auto &out = *static_cast<Observed *>(data);
            GstClockTime time = 0;
            gboolean headers = FALSE;
            guint count = 0;
            if(gst_video_event_parse_upstream_force_key_unit(
                   GST_PAD_PROBE_INFO_EVENT(info), &time, &headers, &count)) {
                ++out.events;
                out.immediate = out.immediate && time == GST_CLOCK_TIME_NONE;
                out.headers = out.headers && headers;
                out.count = count;
                return GST_PAD_PROBE_HANDLED;
            }
            return GST_PAD_PROBE_OK;
        }, &observed, nullptr);
    gst_element_set_state(pipeline, GST_STATE_PAUSED);
    gst_element_get_state(pipeline, nullptr, nullptr, GST_SECOND);
    const bool first = gwv3::send_immediate_keyframe_request(sink, 7);
    const bool second = gwv3::send_immediate_keyframe_request(sink, 8);
    const bool ok = first && second && observed.events == 2 && observed.immediate
                    && observed.headers && observed.count == 8;
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pad);
    gst_object_unref(sink);
    gst_object_unref(pipeline);
    if(!ok) std::cerr << "force-key-unit must be immediate, repeatable and include headers\n";
    return ok ? 0 : 1;
}
