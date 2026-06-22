#include "gwv3_sender/transport.hpp"
#include "gwv3_common/protocol.hpp"

#include <cerrno>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <limits>
#include <string>
#include <stdexcept>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <unistd.h>

namespace gwv3 {

namespace {

sockaddr_in endpoint(const std::string &ip, uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if(inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        throw std::runtime_error("invalid receiver IPv4 address: " + ip);
    }
    return addr;
}

void set_nonblock(int fd, bool nonblock) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if(flags < 0) {
        return;
    }
    if(nonblock) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
    else {
        fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    }
}

void append_packet_slice(std::vector<uint8_t> &out, const MediaPacketView &packet, size_t offset, size_t size) {
    size_t remaining = size;
    size_t payload_offset = 0;
    if(offset < packet.header_size) {
        const size_t take = std::min(remaining, packet.header_size - offset);
        append_bytes(out, packet.header_data + offset, take);
        remaining -= take;
    }
    else {
        payload_offset = offset - packet.header_size;
    }
    if(remaining > 0) {
        append_bytes(out, packet.payload_data + payload_offset, remaining);
    }
}

}  // namespace

Transport::Transport(const AppConfig &config) : config_(config) {
    if(config_.transport.enabled) {
        status_udp_fd_ = make_udp_socket();
        if(rgb_preview_udp_enabled()) {
            preview_udp_fd_ = make_udp_socket();
        }
        if(config_.media_udp.enabled) {
            media_udp_fd_ = make_udp_socket();
        }
    }
}

Transport::~Transport() {
    if(status_udp_fd_ >= 0) {
        close(status_udp_fd_);
    }
    if(preview_udp_fd_ >= 0) {
        close(preview_udp_fd_);
    }
    if(media_udp_fd_ >= 0) {
        close(media_udp_fd_);
    }
    close_media_socket();
}

int Transport::make_udp_socket() {
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if(fd < 0) {
        throw std::runtime_error(std::string("cannot create UDP socket: ") + std::strerror(errno));
    }
    if(config_.transport.send_buffer_bytes > 0) {
        int requested = config_.transport.send_buffer_bytes;
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &requested, sizeof(requested));
    }
    return fd;
}

int Transport::make_tcp_socket() {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0) {
        set_error(std::string("cannot create TCP socket: ") + std::strerror(errno));
        return -1;
    }
    if(config_.transport.send_buffer_bytes > 0) {
        int requested = config_.transport.send_buffer_bytes;
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &requested, sizeof(requested));
    }
    int actual = 0;
    socklen_t actual_len = sizeof(actual);
    if(getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &actual, &actual_len) == 0) {
        media_send_buffer_bytes_ = actual;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}

bool Transport::send_status(const std::string &json_message) {
    if(!config_.transport.enabled) {
        return true;
    }
    return send_udp_status(json_message);
}

bool Transport::rgb_preview_udp_enabled() const {
    return config_.transport.enabled && config_.web_rgb_preview.enabled && config_.web_rgb_preview.udp_enabled;
}

bool Transport::media_udp_enabled(StreamType stream_type) const {
    if(!config_.transport.enabled || !config_.media_udp.enabled) {
        return false;
    }
    if(stream_type == StreamType::rgb) {
        return config_.media_udp.rgb_enabled;
    }
    if(stream_type == StreamType::depth_raw) {
        return config_.media_udp.depth_enabled;
    }
    return false;
}

bool Transport::send_media_udp(const MediaPacketView &packet) {
    if(!config_.media_udp.enabled) {
        set_error("media UDP is disabled");
        return false;
    }
    if(media_udp_fd_ < 0) {
        media_udp_fd_ = make_udp_socket();
    }
    if(packet.header_size > 0 && packet.header_data == nullptr) {
        set_error("media UDP send failed: packet header is null");
        return false;
    }
    if(packet.payload_size > 0 && packet.payload_data == nullptr) {
        set_error("media UDP send failed: packet payload is null");
        return false;
    }
    return send_udp_fragmented_packet(media_udp_fd_, config_.media_udp.port, config_.media_udp.mtu_bytes, media_udp_sequence_, packet,
                                      "media UDP");
}

bool Transport::send_rgb_preview_udp(const MediaPacketView &packet) {
    if(!rgb_preview_udp_enabled()) {
        set_error("rgb preview UDP is disabled");
        return false;
    }
    if(preview_udp_fd_ < 0) {
        preview_udp_fd_ = make_udp_socket();
    }
    if(packet.header_size > 0 && packet.header_data == nullptr) {
        set_error("rgb preview UDP send failed: packet header is null");
        return false;
    }
    if(packet.payload_size > 0 && packet.payload_data == nullptr) {
        set_error("rgb preview UDP send failed: packet payload is null");
        return false;
    }
    return send_udp_preview_packet(packet);
}

bool Transport::send_media(const std::vector<uint8_t> &packet, MediaPriority priority) {
    return send_media(MediaPacketView{packet.data(), packet.size(), nullptr, 0}, priority);
}

bool Transport::send_media(const MediaPacketView &packet, MediaPriority priority) {
    if(!config_.transport.enabled) {
        return true;
    }
    if(packet.header_size > 0 && packet.header_data == nullptr) {
        set_error("media TCP send failed: packet header is null");
        return false;
    }
    if(packet.payload_size > 0 && packet.payload_data == nullptr) {
        set_error("media TCP send failed: packet payload is null");
        return false;
    }
    if(!ensure_media_tcp_connected()) {
        return false;
    }
    if(should_drop_before_write(media_tcp_fd_, packet.total_size(), priority)) {
        if(priority == MediaPriority::realtime) {
            consecutive_media_backpressure_drops_++;
            if(consecutive_media_backpressure_drops_ >= 30) {
                const std::string previous = last_error_;
                close_media_socket();
                last_media_connect_attempt_ = {};
                consecutive_media_backpressure_drops_ = 0;
                set_error(previous + "; reconnecting after repeated backpressure drops");
            }
        }
        return false;
    }
    const auto result = send_all(media_tcp_fd_, packet);
    if(result == SendResult::sent) {
        consecutive_media_backpressure_drops_ = 0;
        return true;
    }
    if(result == SendResult::failed) {
        close_media_socket();
        last_media_connect_attempt_ = std::chrono::steady_clock::now();
        consecutive_media_backpressure_drops_ = 0;
    }
    return false;
}

bool Transport::send_udp_status(const std::string &json_message) {
    auto addr = endpoint(config_.receiver.ip, config_.receiver.status_port);
    std::string payload = json_message;
    payload.push_back('\n');
    const auto sent = sendto(status_udp_fd_, payload.data(), payload.size(), 0, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    if(sent < 0 || static_cast<size_t>(sent) != payload.size()) {
        set_error(std::string("status UDP send failed: ") + std::strerror(errno));
        return false;
    }
    return true;
}

bool Transport::send_udp_preview_packet(const MediaPacketView &packet) {
    return send_udp_fragmented_packet(preview_udp_fd_, config_.web_rgb_preview.udp_port, config_.web_rgb_preview.udp_mtu_bytes,
                                      preview_udp_sequence_, packet, "rgb preview UDP");
}

bool Transport::send_udp_fragmented_packet(int fd, uint16_t port, int mtu_bytes, uint32_t &sequence, const MediaPacketView &packet,
                                           const char *label) {
    const size_t total_size = packet.total_size();
    if(total_size == 0 || total_size > std::numeric_limits<uint32_t>::max()) {
        set_error(std::string(label) + " packet size is invalid: " + std::to_string(total_size));
        return false;
    }

    const size_t mtu = static_cast<size_t>(std::max(mtu_bytes, static_cast<int>(kPreviewUdpHeaderSize + 256)));
    const size_t chunk_capacity = mtu - kPreviewUdpHeaderSize;
    const size_t chunk_count_size = (total_size + chunk_capacity - 1) / chunk_capacity;
    if(chunk_count_size == 0 || chunk_count_size > std::numeric_limits<uint16_t>::max()) {
        set_error(std::string(label) + " packet requires too many chunks: " + std::to_string(chunk_count_size));
        return false;
    }

    auto addr = endpoint(config_.receiver.ip, port);
    uint32_t packet_sequence = ++sequence;
    if(packet_sequence == 0) {
        packet_sequence = ++sequence;
    }

    const auto chunk_count = static_cast<uint16_t>(chunk_count_size);
    for(uint16_t chunk_index = 0; chunk_index < chunk_count; ++chunk_index) {
        const size_t chunk_offset = static_cast<size_t>(chunk_index) * chunk_capacity;
        const size_t chunk_size = std::min(chunk_capacity, total_size - chunk_offset);

        std::vector<uint8_t> datagram;
        datagram.reserve(kPreviewUdpHeaderSize + chunk_size);
        append_le32(datagram, kPreviewUdpMagic);
        append_le16(datagram, kPreviewUdpHeaderVersion);
        append_le16(datagram, kPreviewUdpHeaderSize);
        append_le32(datagram, packet_sequence);
        append_le16(datagram, chunk_index);
        append_le16(datagram, chunk_count);
        append_le32(datagram, static_cast<uint32_t>(total_size));
        append_le32(datagram, static_cast<uint32_t>(chunk_offset));
        append_le16(datagram, static_cast<uint16_t>(chunk_size));
        append_le16(datagram, 0);
        append_le32(datagram, 0);
        append_packet_slice(datagram, packet, chunk_offset, chunk_size);

        const auto sent =
            sendto(fd, datagram.data(), datagram.size(), 0, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
        if(sent < 0 || static_cast<size_t>(sent) != datagram.size()) {
            set_error(std::string(label) + " send failed: " + std::strerror(errno));
            return false;
        }
    }
    return true;
}

bool Transport::ensure_media_tcp_connected() {
    if(media_tcp_fd_ >= 0) {
        return true;
    }
    if(!can_retry_media_connect()) {
        return false;
    }
    last_media_connect_attempt_ = std::chrono::steady_clock::now();

    const int fd = make_tcp_socket();
    if(fd < 0) {
        return false;
    }
    auto addr = endpoint(config_.receiver.ip, config_.receiver.media_port);
    set_nonblock(fd, true);
    int rc = connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    if(rc < 0 && errno != EINPROGRESS) {
        set_error(std::string("media TCP connect failed: ") + std::strerror(errno));
        close(fd);
        return false;
    }

    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(fd, &wfds);
    timeval timeout{};
    timeout.tv_sec = config_.transport.connect_timeout_ms / 1000;
    timeout.tv_usec = (config_.transport.connect_timeout_ms % 1000) * 1000;
    rc = select(fd + 1, nullptr, &wfds, nullptr, &timeout);
    if(rc <= 0) {
        set_error(rc == 0 ? "media TCP connect timed out" : std::string("media TCP select failed: ") + std::strerror(errno));
        close(fd);
        return false;
    }

    int err = 0;
    socklen_t len = sizeof(err);
    if(getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
        set_error(std::string("media TCP connect failed: ") + std::strerror(err == 0 ? errno : err));
        close(fd);
        return false;
    }
    media_tcp_fd_ = fd;
    return true;
}

Transport::SendResult Transport::send_all(int fd, const uint8_t *data, size_t size) {
    size_t offset = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.transport.send_timeout_ms);
    while(offset < size) {
        const ssize_t sent = send(fd, data + offset, size - offset, MSG_NOSIGNAL);
        if(sent > 0) {
            offset += static_cast<size_t>(sent);
            continue;
        }
        if(sent == 0) {
            set_error("media TCP send failed: peer closed connection");
            return SendResult::failed;
        }
        if(errno == EINTR) {
            continue;
        }
        if(errno == EAGAIN || errno == EWOULDBLOCK) {
            const auto now = std::chrono::steady_clock::now();
            if(now >= deadline) {
                if(offset == 0) {
                    set_error("media TCP packet dropped under backpressure before write");
                    return SendResult::dropped_backpressure;
                }
                set_error("media TCP send timed out after partial write under backpressure");
                return SendResult::failed;
            }

            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(fd, &wfds);
            const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline - now);
            timeval timeout{};
            timeout.tv_sec = static_cast<long>(remaining.count() / 1000000);
            timeout.tv_usec = static_cast<long>(remaining.count() % 1000000);
            const int rc = select(fd + 1, nullptr, &wfds, nullptr, &timeout);
            if(rc > 0) {
                continue;
            }
            if(rc < 0 && errno == EINTR) {
                continue;
            }
            if(rc == 0 && offset == 0) {
                set_error("media TCP packet dropped under backpressure before write");
                return SendResult::dropped_backpressure;
            }
            set_error(rc == 0 ? "media TCP send timed out after partial write under backpressure"
                              : std::string("media TCP send select failed: ") + std::strerror(errno));
            return SendResult::failed;
        }
        else {
            set_error(std::string("media TCP send failed: ") + std::strerror(errno));
            return SendResult::failed;
        }
    }
    return SendResult::sent;
}

Transport::SendResult Transport::send_all(int fd, const MediaPacketView &packet) {
    const size_t total_size = packet.total_size();
    size_t offset = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.transport.send_timeout_ms);

    while(offset < total_size) {
        std::array<iovec, 2> iovs{};
        int iov_count = 0;
        size_t remaining_offset = offset;
        if(remaining_offset < packet.header_size) {
            iovs[iov_count].iov_base = const_cast<uint8_t *>(packet.header_data + remaining_offset);
            iovs[iov_count].iov_len = packet.header_size - remaining_offset;
            ++iov_count;
            remaining_offset = 0;
        }
        else {
            remaining_offset -= packet.header_size;
        }
        if(packet.payload_size > remaining_offset) {
            iovs[iov_count].iov_base = const_cast<uint8_t *>(packet.payload_data + remaining_offset);
            iovs[iov_count].iov_len = packet.payload_size - remaining_offset;
            ++iov_count;
        }

        msghdr message{};
        message.msg_iov = iovs.data();
        message.msg_iovlen = static_cast<size_t>(iov_count);
        const ssize_t sent = sendmsg(fd, &message, MSG_NOSIGNAL);
        if(sent > 0) {
            offset += static_cast<size_t>(sent);
            continue;
        }
        if(sent == 0) {
            set_error("media TCP send failed: peer closed connection");
            return SendResult::failed;
        }
        if(errno == EINTR) {
            continue;
        }
        if(errno == EAGAIN || errno == EWOULDBLOCK) {
            const auto now = std::chrono::steady_clock::now();
            if(now >= deadline) {
                if(offset == 0) {
                    set_error("media TCP packet dropped under backpressure before write");
                    return SendResult::dropped_backpressure;
                }
                set_error("media TCP send timed out after partial write under backpressure");
                return SendResult::failed;
            }

            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(fd, &wfds);
            const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline - now);
            timeval timeout{};
            timeout.tv_sec = static_cast<long>(remaining.count() / 1000000);
            timeout.tv_usec = static_cast<long>(remaining.count() % 1000000);
            const int rc = select(fd + 1, nullptr, &wfds, nullptr, &timeout);
            if(rc > 0) {
                continue;
            }
            if(rc < 0 && errno == EINTR) {
                continue;
            }
            if(rc == 0 && offset == 0) {
                set_error("media TCP packet dropped under backpressure before write");
                return SendResult::dropped_backpressure;
            }
            set_error(rc == 0 ? "media TCP send timed out after partial write under backpressure"
                              : std::string("media TCP send select failed: ") + std::strerror(errno));
            return SendResult::failed;
        }

        set_error(std::string("media TCP send failed: ") + std::strerror(errno));
        return SendResult::failed;
    }
    return SendResult::sent;
}

void Transport::close_media_socket() {
    if(media_tcp_fd_ >= 0) {
        linger reset_linger{};
        reset_linger.l_onoff = 1;
        reset_linger.l_linger = 0;
        setsockopt(media_tcp_fd_, SOL_SOCKET, SO_LINGER, &reset_linger, sizeof(reset_linger));
        close(media_tcp_fd_);
        media_tcp_fd_ = -1;
    }
}

bool Transport::can_retry_media_connect() const {
    if(last_media_connect_attempt_.time_since_epoch().count() == 0) {
        return true;
    }
    const auto elapsed = std::chrono::steady_clock::now() - last_media_connect_attempt_;
    return elapsed >= std::chrono::milliseconds(config_.transport.reconnect_interval_ms);
}

bool Transport::should_drop_before_write(int fd, size_t packet_size, MediaPriority priority) {
#ifdef TIOCOUTQ
    int pending = 0;
    if(ioctl(fd, TIOCOUTQ, &pending) != 0 || pending <= 0) {
        return false;
    }
    const size_t capacity = media_send_capacity_bytes();
    if(capacity == 0) {
        return false;
    }
    const size_t queued = static_cast<size_t>(pending);
    size_t limit = capacity;
    if(priority == MediaPriority::bulk) {
        const size_t reserve = std::max<size_t>(256 * 1024, capacity / 4);
        limit = capacity > reserve ? capacity - reserve : capacity;
    }
    const bool drop = priority == MediaPriority::bulk ? queued + packet_size >= limit : queued >= limit;
    if(drop) {
        const std::string kind = priority == MediaPriority::bulk ? "bulk media TCP packet dropped under backpressure "
                                                                 : "media TCP packet dropped under backpressure ";
        set_error(kind + "pending_bytes=" + std::to_string(queued) + " packet_bytes=" + std::to_string(packet_size)
                  + " limit_bytes=" + std::to_string(limit) + " capacity_bytes=" + std::to_string(capacity));
        return true;
    }
#else
    (void)fd;
    (void)packet_size;
    (void)priority;
#endif
    return false;
}

size_t Transport::media_send_capacity_bytes() const {
    if(media_send_buffer_bytes_ > 0) {
        // Linux reports SO_SNDBUF doubled for bookkeeping; user payload capacity is roughly half.
        return static_cast<size_t>(media_send_buffer_bytes_) / 2;
    }
    return config_.transport.send_buffer_bytes > 0 ? static_cast<size_t>(config_.transport.send_buffer_bytes) : 0;
}

void Transport::set_error(const std::string &message) {
    last_error_ = message;
}

std::string Transport::last_error() const {
    return last_error_;
}

}  // namespace gwv3
