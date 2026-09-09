#pragma once

#include <gst/gst.h>
#include <gst/video/video-event.h>

namespace gwv3 {

inline bool send_immediate_keyframe_request(GstElement *sink, guint count) {
    if(!sink) {
        return false;
    }
    // Capture scheduling has already selected the due frame. Do not schedule
    // again in the encoder's potentially different running-time domain.
    return gst_element_send_event(sink, gst_video_event_new_upstream_force_key_unit(
                                            GST_CLOCK_TIME_NONE, TRUE, count));
}

}  // namespace gwv3
