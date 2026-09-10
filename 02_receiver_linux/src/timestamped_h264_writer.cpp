#include "gwv3_receiver/timestamped_h264_writer.hpp"

#include <cerrno>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
}

namespace gwv3 {
namespace {
void checked(int result, const char *operation) {
    if(result >= 0) return;
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(result, message, sizeof(message));
    throw std::runtime_error(std::string(operation) + ": " + message);
}
}

struct TimestampedH264Writer::Impl {
    Sink sink;
    AVFormatContext *format = nullptr;
    AVIOContext *io = nullptr;
    AVStream *stream = nullptr;
    AVPacket *packet = nullptr;
    AVCodecParserContext *parser = nullptr;
    AVCodecContext *codec = nullptr;
    int64_t last_pts = -1;
    int64_t frame_duration_us = 33333;
    bool header_written = false;
    bool failed = false;

    static int output(void *opaque, uint8_t *data, int size) noexcept {
        auto &self = *static_cast<Impl *>(opaque);
        try {
            if(self.sink(data, static_cast<size_t>(size))) return size;
        }
        catch(...) {}
        self.failed = true;
        return AVERROR(EIO);
    }

    bool close() noexcept {
        if(format && header_written) {
            if(av_write_trailer(format) < 0) failed = true;
            header_written = false;
        }
        if(io) {
            avio_flush(io);
            if(io->error < 0) failed = true;
        }
        if(format) format->pb = nullptr;
        avformat_free_context(format);
        format = nullptr;
        if(io) av_freep(&io->buffer);
        avio_context_free(&io);
        av_packet_free(&packet);
        if(parser) av_parser_close(parser);
        parser = nullptr;
        avcodec_free_context(&codec);
        return !failed;
    }
    ~Impl() { close(); }
};

TimestampedH264Writer::TimestampedH264Writer(uint32_t width, uint32_t height, double fps, Sink sink)
    : impl_(std::make_unique<Impl>()) {
    auto &s = *impl_;
    if(width == 0 || height == 0 || width > 16384 || height > 16384 || !std::isfinite(fps) || fps <= 0 || fps > 1000 || !sink) {
        throw std::invalid_argument("invalid timestamped H264 stream parameters");
    }
    s.sink = std::move(sink);
    s.frame_duration_us = static_cast<int64_t>(std::llround(1000000.0 / fps));
    checked(avformat_alloc_output_context2(&s.format, nullptr, "nut", nullptr), "allocate timestamped H264 muxer");
    s.stream = avformat_new_stream(s.format, nullptr);
    s.packet = av_packet_alloc();
    s.parser = av_parser_init(AV_CODEC_ID_H264);
    s.codec = avcodec_alloc_context3(nullptr);
    if(!s.stream || !s.packet || !s.parser || !s.codec) throw std::bad_alloc();
    s.parser->flags |= PARSER_FLAG_COMPLETE_FRAMES;
    s.codec->codec_id = AV_CODEC_ID_H264;
    s.stream->time_base = AVRational{1, 1000000};
    s.stream->avg_frame_rate = av_d2q(fps, 1000000);
    auto &parameters = *s.stream->codecpar;
    parameters.codec_type = AVMEDIA_TYPE_VIDEO;
    parameters.codec_id = AV_CODEC_ID_H264;
    parameters.width = static_cast<int>(width);
    parameters.height = static_cast<int>(height);
    auto *buffer = static_cast<uint8_t *>(av_malloc(32768));
    if(!buffer) throw std::bad_alloc();
    s.io = avio_alloc_context(buffer, 32768, 1, &s, nullptr, Impl::output, nullptr);
    if(!s.io) { av_free(buffer); throw std::bad_alloc(); }
    s.format->pb = s.io;
    s.format->flags |= AVFMT_FLAG_CUSTOM_IO | AVFMT_FLAG_FLUSH_PACKETS;
    checked(avformat_write_header(s.format, nullptr), "write timestamped H264 header");
    s.header_written = true;
}

TimestampedH264Writer::~TimestampedH264Writer() = default;

void TimestampedH264Writer::write(const uint8_t *data, size_t size, int64_t pts_us, bool keyframe) {
    auto &s = *impl_;
    if(!s.format || s.failed) throw std::runtime_error("timestamped H264 writer unavailable");
    if(!data || size == 0 || size > static_cast<size_t>(std::numeric_limits<int>::max()) || pts_us < 0 || pts_us <= s.last_pts) {
        throw std::invalid_argument("invalid or non-increasing RGB capture PTS");
    }
    av_packet_unref(s.packet);
    checked(av_new_packet(s.packet, static_cast<int>(size)), "allocate H264 packet");
    std::memcpy(s.packet->data, data, size);
    uint8_t *parsed = nullptr;
    int parsed_size = 0;
    // A streaming parser retains boundary state after EOF. Use a fresh boundary
    // scanner for each transport AU; the complete-frame parser below retains
    // SPS/PPS state for P pictures and B-frame detection across packets.
    auto *boundary = av_parser_init(AV_CODEC_ID_H264);
    if(!boundary) throw std::bad_alloc();
    const int consumed = av_parser_parse2(boundary, s.codec, &parsed, &parsed_size, s.packet->data, s.packet->size,
                                         pts_us, pts_us, -1);
    av_parser_close(boundary);
    checked(consumed, "check H264 access unit boundary");
    if(consumed != s.packet->size) {
        throw std::runtime_error("multiple H264 access units in one timestamped media packet");
    }
    checked(av_parser_parse2(s.parser, s.codec, &parsed, &parsed_size, s.packet->data, s.packet->size,
                            pts_us, pts_us, -1), "parse complete H264 access unit");
    if(parsed_size == 0 || s.parser->pict_type == AV_PICTURE_TYPE_NONE) {
        throw std::runtime_error("timestamped RGB packet is not a complete H264 picture");
    }
    if(s.parser->pict_type == AV_PICTURE_TYPE_B) {
        throw std::runtime_error("RGB B frames require explicit decode timestamps; refusing invented DTS");
    }
    s.packet->stream_index = s.stream->index;
    s.packet->pts = av_rescale_q(pts_us, AVRational{1, 1000000}, s.stream->time_base);
    s.packet->dts = s.packet->pts;
    s.packet->duration = av_rescale_q(s.frame_duration_us, AVRational{1, 1000000}, s.stream->time_base);
    if(keyframe || s.parser->key_frame == 1) s.packet->flags |= AV_PKT_FLAG_KEY;
    checked(av_write_frame(s.format, s.packet), "write timestamped H264 packet");
    avio_flush(s.io);
    checked(s.io->error, "flush timestamped H264 packet");
    s.last_pts = pts_us;
}

bool TimestampedH264Writer::close() noexcept { return impl_->close(); }
} // namespace gwv3
