#include "gwv3_sender/gst_h264_encoder.hpp"

#include <opencv2/imgcodecs.hpp>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

namespace {
bool has_idr(const std::vector<uint8_t> &packet) {
    for(size_t i = 0; i + 3 < packet.size(); ++i) {
        if(packet[i] == 0 && packet[i + 1] == 0 && packet[i + 2] == 1
           && (packet[i + 3] & 31) == 5) return true;
    }
    return false;
}
}

int main(int argc, char **argv) {
    const std::string mode = argc > 1 ? argv[1] : "single";
    if(mode != "single" && mode != "dual" && mode != "bgr") return 2;
    try {
        const int width = argc > 2 ? std::stoi(argv[2]) : 1920;
        const int height = argc > 3 ? std::stoi(argv[3]) : 1080;
        if(width < 320 || width > 1920 || height < 240 || height > 1080) return 2;
        std::cout << "mode=" << mode << " input=" << width << "x" << height << std::endl;
        constexpr uint64_t base_us = 1788944000000000ull;
        constexpr uint64_t period_us = 33333;
        cv::Mat picture(height, width, CV_8UC3, cv::Scalar(80, 100, 120));
        std::vector<uint8_t> jpeg;
        if(!cv::imencode(".jpg", picture, jpeg)) return 1;
        std::unique_ptr<gwv3::GstH264Encoder> single;
        std::unique_ptr<gwv3::GstJpegDualH264Encoder> dual;
        if(mode == "dual") {
            dual = std::make_unique<gwv3::GstJpegDualH264Encoder>(
                width, height, 30, 12000000, "mpph264enc", 320, 240, 30, 100000);
            if(!dual->ok()) throw std::runtime_error(dual->error());
        } else {
            single = std::make_unique<gwv3::GstH264Encoder>(
                width, height, 30, 12000000, "mpph264enc",
                mode == "bgr" ? gwv3::GstH264InputFormat::Bgr : gwv3::GstH264InputFormat::Jpeg);
            if(!single->ok()) throw std::runtime_error(single->error());
        }
        int pending = -1, requests = 0, failures = 0;
        const auto start = std::chrono::steady_clock::now();
        // A single request misses the stale-pending bug in older MPP plugins.
        for(int i = 0; i < 500; ++i) {
            std::this_thread::sleep_until(start + std::chrono::microseconds(i * period_us));
            if(i >= 43 && (i - 43) % 47 == 0) {
                if(pending >= 0) ++failures;
                pending = i;
                ++requests;
                if(dual) dual->request_keyframe(); else single->request_keyframe();
            }
            const auto outputs = dual
                ? dual->encode_jpeg(jpeg.data(), jpeg.size(), base_us + i * period_us, true).main
                : mode == "bgr" ? single->encode_bgr(picture, base_us + i * period_us)
                                : single->encode_jpeg(jpeg.data(), jpeg.size(), base_us + i * period_us);
            for(const auto &out : outputs) {
                if(!out.has_pts || out.pts_us < base_us) return 1;
                if(!has_idr(out.data)) continue;
                const auto index = static_cast<int>((out.pts_us - base_us) / period_us);
                if(pending < 0 || index < pending) continue;
                std::cout << "request=" << pending << " idr_frame=" << index
                          << " observed_at=" << i << std::endl;
                if(index > pending + 3 || i > pending + 4) ++failures;
                pending = -1;
            }
        }
        if(pending >= 0) ++failures;
        std::cout << "requests=" << requests << " failures=" << failures << std::endl;
        return requests == 10 && failures == 0 ? 0 : 1;
    } catch(const std::exception &error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}
