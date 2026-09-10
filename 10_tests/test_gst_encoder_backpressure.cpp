#include "gwv3_sender/gst_h264_encoder.hpp"

#include <gst/app/gstappsink.h>
#include <opencv2/imgcodecs.hpp>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>

namespace gwv3 {
struct GstEncoderTestAccess {
    static GstElement *pipeline(GstH264Encoder &e) { return e.pipeline_; }
    static GstElement *pipeline(GstJpegDualH264Encoder &e) { return e.pipeline_; }
    static GstElement *sink(GstH264Encoder &e) { return e.appsink_; }
    static GstElement *sink(GstJpegDualH264Encoder &e) { return e.main_sink_; }
};
}

// Hold the encoder input while several frames arrive, without relying on CPU load.
struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool open = false;
    GstPad *pad;
    gulong id;
    explicit Gate(GstElement *pipeline, const char *name) {
        auto *element = gst_bin_get_by_name(GST_BIN(pipeline), name);
        if(!element) throw std::runtime_error("test encoder not found");
        pad = gst_element_get_static_pad(element, "sink");
        gst_object_unref(element);
        id = gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, [](GstPad *, GstPadProbeInfo *, gpointer p) {
            auto &gate = *static_cast<Gate *>(p);
            std::unique_lock<std::mutex> lock(gate.mutex);
            gate.entered = true;
            gate.cv.notify_all();
            gate.cv.wait(lock, [&] { return gate.open; });
            return GST_PAD_PROBE_OK;
        }, this, nullptr);
    }
    void release() {
        std::lock_guard<std::mutex> lock(mutex);
        open = true;
        cv.notify_all();
    }
    ~Gate() {
        release();
        GST_PAD_STREAM_LOCK(pad);
        gst_pad_remove_probe(pad, id);
        GST_PAD_STREAM_UNLOCK(pad);
        gst_object_unref(pad);
    }
    void wait_entered() {
        std::unique_lock<std::mutex> lock(mutex);
        if(!cv.wait_for(lock, std::chrono::seconds(2), [&] { return entered; }))
            throw std::runtime_error("test gate never received input");
    }
};

int main(int argc, char **argv) {
    gst_init(nullptr, nullptr);
    const std::string codec = argc > 1 ? argv[1] : "x264enc";
    const std::string mode = argc > 2 ? argv[2] : "bgr";
    const std::string scenario = argc > 3 ? argv[3] : "pressure";
    auto *factory = gst_element_factory_find(codec.c_str());
    if(!factory) return 77;
    gst_object_unref(factory);
    try {
        cv::Mat picture(240, 320, CV_8UC3, cv::Scalar(80, 100, 120));
        std::vector<uint8_t> jpeg;
        if(!cv::imencode(".jpg", picture, jpeg)) return 1;
        jpeg.resize(jpeg.size() + 4096, 0);  // Camera SDKs may pad complete JPEGs.
        std::unique_ptr<gwv3::GstH264Encoder> single;
        std::unique_ptr<gwv3::GstJpegDualH264Encoder> dual;
        GstElement *pipeline, *sink;
        if(mode == "dual") {
            dual = std::make_unique<gwv3::GstJpegDualH264Encoder>(320, 240, 30, 1000000, codec, 160, 120, 30, 200000);
            if(!dual->ok()) throw std::runtime_error(dual->error());
            pipeline = gwv3::GstEncoderTestAccess::pipeline(*dual);
            sink = gwv3::GstEncoderTestAccess::sink(*dual);
        } else {
            single = std::make_unique<gwv3::GstH264Encoder>(320, 240, 30, 1000000, codec,
                mode == "bgr" ? gwv3::GstH264InputFormat::Bgr : gwv3::GstH264InputFormat::Jpeg,
                0, 0, scenario == "preview" ? gwv3::GstH264QueuePolicy::Preview : gwv3::GstH264QueuePolicy::Recording);
            if(!single->ok()) throw std::runtime_error(single->error());
            pipeline = gwv3::GstEncoderTestAccess::pipeline(*single);
            sink = gwv3::GstEncoderTestAccess::sink(*single);
        }
        std::unique_ptr<Gate> gate;
        if(scenario != "timing") gate = std::make_unique<Gate>(pipeline,
            mode == "dual" ? (scenario == "preview" ? "preview_enc" : "main_enc") : "enc");
        constexpr uint64_t base = 1789000000000000ull;
        const int count = scenario == "timing" || scenario == "timeout" ? 64 : 8;
        std::set<uint64_t> submitted;
        std::set<uint64_t> received;
        bool timed_out = false;
        for(int i = 0; i < count; ++i) {
            const auto timestamp = base + i * 33333ull
                + (scenario == "timing" ? (i % 4) * 2000ull + (i >= 20 ? 100000ull : 0) : 0);
            const auto start = std::chrono::steady_clock::now();
            try {
                auto outputs = dual ? dual->encode_jpeg(jpeg.data(), jpeg.size(), timestamp, true).main
                                : mode == "bgr" ? single->encode_bgr(picture, timestamp)
                                                : single->encode_jpeg(jpeg.data(), jpeg.size(), timestamp);
                submitted.insert(timestamp);
                for(const auto &frame : outputs) {
                    if(!frame.has_pts || !received.insert(frame.pts_us).second)
                        throw std::runtime_error("missing or duplicate output PTS");
                }
            } catch(const std::runtime_error &error) {
                if(scenario != "timeout" || std::string(error.what()).find("backpressure timeout") == std::string::npos) throw;
                const auto elapsed = std::chrono::steady_clock::now() - start;
                if(elapsed > std::chrono::seconds(1)) throw std::runtime_error("admission wait was not bounded");
                timed_out = true;
                break;
            }
            if(i == 0 && gate) gate->wait_entered();
        }
        if(gate) gate->release();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while(received.size() < submitted.size() && std::chrono::steady_clock::now() < deadline) {
            auto *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 100 * GST_MSECOND);
            if(!sample) continue;
            auto *buffer = gst_sample_get_buffer(sample);
            const bool valid = buffer && GST_BUFFER_PTS_IS_VALID(buffer)
                && received.insert(GST_BUFFER_PTS(buffer) / GST_USECOND).second;
            gst_sample_unref(sample);
            if(!valid) throw std::runtime_error("missing or duplicate drained output PTS");
        }
        size_t missing = 0;
        for(auto timestamp : submitted) missing += !received.count(timestamp);
        for(auto timestamp : received) {
            if(!submitted.count(timestamp)) std::cout << "unexpected_pts_delta_us=" << timestamp - base << std::endl;
        }
        std::cout << "codec=" << codec << " mode=" << mode << " scenario=" << scenario << " submitted=" << submitted.size()
                  << " received=" << received.size() << " missing=" << missing << " timed_out=" << timed_out << std::endl;
        if(scenario == "preview" && !dual) return missing > 0 && !received.empty() ? 0 : 1;
        if(scenario == "timeout" && !timed_out) return 1;
        return missing == 0 && received.size() == submitted.size() ? 0 : 1;
    } catch(const std::exception &e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }
}
