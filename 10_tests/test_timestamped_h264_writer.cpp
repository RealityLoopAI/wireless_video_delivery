#include "gwv3_receiver/timestamped_h264_writer.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}

static void require(bool condition, const char *message) {
    if(!condition) throw std::runtime_error(message);
}

int main(int argc, char **argv) {
    try {
        require(argc == 4, "expected fixture, output, mode");
        const std::string mode = argv[3];
        std::ifstream input(argv[1], std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
        require(!bytes.empty(), "fixture unavailable");
        std::ofstream output(argv[2], std::ios::binary);
        auto sink = [&](const uint8_t *data, size_t size) {
            output.write(reinterpret_cast<const char *>(data), static_cast<std::streamsize>(size));
            return bool(output);
        };
        gwv3::TimestampedH264Writer writer(64, 48, 30, sink);
        if(mode == "multiple") {
            bool rejected = false;
            try { writer.write(bytes.data(), bytes.size(), 0, false); }
            catch(const std::runtime_error &e) { rejected = std::string(e.what()).find("multiple H264") != std::string::npos; }
            require(rejected, "multiple pictures with one timestamp accepted");
        }
        else if(mode == "bframes" || mode == "sequence") {
            AVFormatContext *format = nullptr;
            require(avformat_open_input(&format, argv[1], nullptr, nullptr) >= 0, "cannot demux B-frame fixture");
            AVPacket *packet = av_packet_alloc();
            bool rejected = false;
            bool unexpected_error = false;
            int64_t pts = 0;
            while(av_read_frame(format, packet) >= 0) {
                try { writer.write(packet->data, packet->size, pts, false); }
                catch(const std::runtime_error &e) {
                    rejected = std::string(e.what()).find("B frames require") != std::string::npos;
                    unexpected_error = !rejected;
                    break;
                }
                pts += 33333;
                av_packet_unref(packet);
            }
            av_packet_free(&packet);
            avformat_close_input(&format);
            require(!unexpected_error, "unexpected H264 parser error");
            require(mode == "bframes" ? rejected : !rejected && pts > 0,
                    "unexpected H264 sequence acceptance result");
        }
        else {
            for(int64_t pts : {200000, 233333, 266666, 1266666, 1299999})
                writer.write(bytes.data(), bytes.size(), pts, false);
            for(int64_t pts : {-1, 1299999, 1200000}) {
                bool rejected = false;
                try { writer.write(bytes.data(), bytes.size(), pts, false); }
                catch(const std::invalid_argument &) { rejected = true; }
                require(rejected, "negative, duplicate or backward PTS accepted");
            }
            bool failed = false;
            try {
                gwv3::TimestampedH264Writer broken(64, 48, 30, [](const uint8_t *, size_t) { return false; });
                broken.write(bytes.data(), bytes.size(), 0, false);
            }
            catch(const std::runtime_error &) { failed = true; }
            require(failed, "output failure silently accepted");
        }
        require(writer.close(), "close failed");
        require(writer.close(), "second close failed");
        std::cout << "PASS timestamped writer " << mode << '\n';
        return 0;
    }
    catch(const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
