#include "gwv3_sender/transport.hpp"
#include "gwv3_sender/network_utils.hpp"

#include "gwv3_common/protocol.hpp"

#include <algorithm>
#include <cerrno>
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
#include <poll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <unistd.h>

namespace gwv3 {

namespace {

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

void write_le16(uint8_t *out, uint16_t value) {
    out[0] = static_cast<uint8_t>(value & 0xffu);
    out[1] = static_cast<uint8_t>((value >> 8u) & 0xffu);
}

void write_le32(uint8_t *out, uint32_t value) {
    out[0] = static_cast<uint8_t>(value & 0xffu);
    out[1] = static_cast<uint8_t>((value >> 8u) & 0xffu);
    out[2] = static_cast<uint8_t>((value >> 16u) & 0xffu);
    out[3] = static_cast<uint8_t>((value >> 24u) & 0xffu);
}

uint32_t read_le32(const uint8_t *data) {
    return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8u) | (static_cast<uint32_t>(data[2]) << 16u)
           | (static_cast<uint32_t>(data[3]) << 24u);
}

bool append_packet_slice_iovs(const MediaPacketView &packet, size_t offset, size_t size, std::array<iovec, 3> &iovs, int &iov_count) {
    size_t remaining_offset = offset;
    size_t remaining_size = size;
    if(remaining_offset < packet.header_size) {
        const size_t take = std::min(packet.header_size - remaining_offset, remaining_size);
        iovs[iov_count].iov_base = const_cast<uint8_t *>(packet.header_data + remaining_offset);
        iovs[iov_count].iov_len = take;
        ++iov_count;
        remaining_size -= take;
        remaining_offset = 0;
    }
    else {
        remaining_offset -= packet.header_size;
    }

    if(remaining_size > 0) {
        if(remaining_offset >= packet.payload_size) {
            return false;
        }
        const size_t take = std::min(packet.payload_size - remaining_offset, remaining_size);
        iovs[iov_count].iov_base = const_cast<uint8_t *>(packet.payload_data + remaining_offset);
        iovs[iov_count].iov_len = take;
        ++iov_count;
        remaining_size -= take;
    }
    return remaining_size == 0;
}

bool tcp_peer_closed(int fd) {
    if(fd < 0) {
        return true;
    }
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN | POLLERR | POLLHUP;
#ifdef POLLRDHUP
    pfd.events |= POLLRDHUP;
#endif
    const int rc = poll(&pfd, 1, 0);
    if(rc <= 0) {
        return false;
    }
    if((pfd.revents & (POLLERR | POLLHUP)) != 0) {
        return true;
    }
#ifdef POLLRDHUP
    if((pfd.revents & POLLRDHUP) != 0) {
        return true;
    }
#endif
    if((pfd.revents & POLLIN) != 0) {
        char byte = 0;
        const ssize_t got = recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
        if(got == 0) {
            return true;
        }
        if(got < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            return true;
        }
    }
    return false;
}

}  // namespace

Transport::Transport(const AppConfig &config)
    : Transport(config, std::make_shared<ReceiverTarget>(config.receiver.ip)) {}

Transport::Transport(const AppConfig &config, std::shared_ptr<ReceiverTarget> receiver_target)
    : config_(config), receiver_target_(std::move(receiver_target)) {
    if(!receiver_target_) {
        throw std::runtime_error("receiver target must not be null");
    }
    if(config_.transport.enabled) {
        status_udp_fd_ = make_udp_socket();
    }
}

Transport::~Transport() {
    if(status_udp_fd_ >= 0) {
        close(status_udp_fd_);
    }
    close_media_socket();
    close_udp_socket(media_udp_fd_);
    close_udp_socket(preview_udp_fd_);
}

int Transport::make_udp_socket() {
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if(fd < 0) {
        throw std::runtime_error(std::string("cannot create UDP socket: ") + std::strerror(errno));
    }
    const int descriptor_flags = fcntl(fd, F_GETFD, 0);
    if(descriptor_flags >= 0) {
        fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC);
    }
    return fd;
}

int Transport::make_tcp_socket() {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0) {
        set_error(std::string("cannot create TCP socket: ") + std::strerror(errno));
        return -1;
    }
    const int descriptor_flags = fcntl(fd, F_GETFD, 0);
    if(descriptor_flags >= 0) {
        fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC);
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

std::optional<std::string> Transport::receive_status_control(int timeout_ms) {
    if(!config_.transport.enabled || status_udp_fd_ < 0) {
        return std::nullopt;
    }

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(status_udp_fd_, &rfds);
    timeval timeout{};
    timeout.tv_sec = std::max(0, timeout_ms) / 1000;
    timeout.tv_usec = (std::max(0, timeout_ms) % 1000) * 1000;
    const int rc = select(status_udp_fd_ + 1, &rfds, nullptr, nullptr, &timeout);
    if(rc <= 0) {
        return std::nullopt;
    }

    std::array<char, 4096> buffer{};
    sockaddr_in peer{};
    socklen_t peer_len = sizeof(peer);
    const ssize_t got = recvfrom(status_udp_fd_, buffer.data(), buffer.size(), 0, reinterpret_cast<sockaddr *>(&peer), &peer_len);
    if(got <= 0 || static_cast<size_t>(got) == buffer.size()) {
        return std::nullopt;
    }
    sockaddr_in expected_peer{};
    const auto target = receiver_target_->snapshot();
    try {
        expected_peer = resolve_ipv4_endpoint(target.host, config_.receiver.status_port);
    }
    catch(const std::exception &e) {
        set_error(e.what());
        return std::nullopt;
    }
    if(peer.sin_family != AF_INET || peer.sin_addr.s_addr != expected_peer.sin_addr.s_addr) {
        return std::nullopt;
    }
    return std::string(buffer.data(), static_cast<size_t>(got));
}

bool Transport::send_media(const std::vector<uint8_t> &packet) {
    return send_media(MediaPacketView{packet.data(), packet.size(), nullptr, 0});
}

bool Transport::send_media(const MediaPacketView &packet) {
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
    int udp_mtu_bytes = 0;
    const char *udp_label = nullptr;
    if(auto udp_port = udp_port_for_packet(packet, udp_mtu_bytes, udp_label)) {
        int &udp_fd = std::strcmp(udp_label, "preview UDP") == 0 ? preview_udp_fd_ : media_udp_fd_;
        if(!ensure_udp_socket(udp_fd, udp_label)) {
            return false;
        }
        return send_fragmented_udp_packet(udp_fd, *udp_port, udp_mtu_bytes, packet, udp_label);
    }
    if(!ensure_media_tcp_connected()) {
        return false;
    }
    if(!config_.recording_buffer.enabled && should_drop_before_write(media_tcp_fd_, packet.total_size())) {
        consecutive_media_backpressure_drops_++;
        if(consecutive_media_backpressure_drops_ >= 30) {
            const std::string previous = last_error_;
            close_media_socket();
            last_media_connect_attempt_ = {};
            consecutive_media_backpressure_drops_ = 0;
            set_error(previous + "; reconnecting after repeated backpressure drops");
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

bool Transport::close_if_media_peer_closed() {
    if(media_tcp_fd_ < 0 || !tcp_peer_closed(media_tcp_fd_)) {
        return false;
    }
    close_media_socket();
    last_media_connect_attempt_ = {};
    set_error("media TCP peer closed while idle; socket closed for reconnect");
    return true;
}

bool Transport::send_udp_status(const std::string &json_message) {
    sockaddr_in addr{};
    const auto target = receiver_target_->snapshot();
    try {
        addr = resolve_ipv4_endpoint(target.host, config_.receiver.status_port);
    }
    catch(const std::exception &e) {
        set_error(e.what());
        return false;
    }
    std::string payload = json_message;
    payload.push_back('\n');
    const auto sent = sendto(status_udp_fd_, payload.data(), payload.size(), 0, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    if(sent < 0 || static_cast<size_t>(sent) != payload.size()) {
        set_error(std::string("status UDP send failed: ") + std::strerror(errno));
        return false;
    }
    return true;
}

std::optional<uint16_t> Transport::udp_port_for_packet(const MediaPacketView &packet, int &mtu_bytes, const char *&label) const {
    if(packet.header_size < 9 || packet.header_data == nullptr || read_le32(packet.header_data) != kMediaMagic) {
        return std::nullopt;
    }

    const auto stream_type = static_cast<StreamType>(packet.header_data[8]);
    if(stream_type == StreamType::rgb_preview) {
        if(config_.web_rgb_preview.udp_enabled) {
            mtu_bytes = config_.web_rgb_preview.udp_mtu_bytes;
            label = "preview UDP";
            return config_.web_rgb_preview.udp_port;
        }
        return std::nullopt;
    }

    const bool media_udp_protocol = config_.transport.media_protocol == "udp";
    const bool media_udp_enabled = config_.media_udp.enabled || media_udp_protocol;
    if(!media_udp_enabled) {
        return std::nullopt;
    }
    if(stream_type == StreamType::rgb && (media_udp_protocol || config_.media_udp.rgb_enabled)) {
        mtu_bytes = config_.media_udp.mtu_bytes;
        label = "media UDP";
        return config_.media_udp.port;
    }
    if(stream_type == StreamType::depth_raw && (media_udp_protocol || config_.media_udp.depth_enabled)) {
        mtu_bytes = config_.media_udp.mtu_bytes;
        label = "media UDP";
        return config_.media_udp.port;
    }
    return std::nullopt;
}

bool Transport::ensure_udp_socket(int &fd, const char *label) {
    if(fd >= 0) {
        return true;
    }
    try {
        fd = make_udp_socket();
    }
    catch(const std::exception &e) {
        set_error(std::string(label) + " socket create failed: " + e.what());
        return false;
    }
    if(fd < 0) {
        return false;
    }
    if(config_.transport.send_buffer_bytes > 0) {
        int requested = config_.transport.send_buffer_bytes;
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &requested, sizeof(requested));
    }
    set_nonblock(fd, true);
    (void)label;
    return true;
}

bool Transport::send_fragmented_udp_packet(int fd, uint16_t port, int mtu_bytes, const MediaPacketView &packet, const char *label) {
    if(mtu_bytes <= static_cast<int>(kPreviewUdpHeaderSize)) {
        set_error(std::string(label) + " send failed: mtu_bytes must exceed UDP fragment header size");
        return false;
    }
    const size_t total_size = packet.total_size();
    if(total_size == 0 || total_size > std::numeric_limits<uint32_t>::max()) {
        set_error(std::string(label) + " send failed: packet size is invalid");
        return false;
    }
    const size_t chunk_payload_size = static_cast<size_t>(mtu_bytes) - kPreviewUdpHeaderSize;
    const size_t chunk_count_size = (total_size + chunk_payload_size - 1u) / chunk_payload_size;
    if(chunk_count_size == 0 || chunk_count_size > std::numeric_limits<uint16_t>::max()) {
        set_error(std::string(label) + " send failed: packet requires too many UDP fragments");
        return false;
    }

    uint32_t &sequence_counter = std::strcmp(label, "preview UDP") == 0 ? preview_udp_sequence_ : media_udp_sequence_;
    const uint32_t sequence = ++sequence_counter == 0 ? ++sequence_counter : sequence_counter;
    const uint16_t chunk_count = static_cast<uint16_t>(chunk_count_size);
    sockaddr_in addr{};
    const auto target = receiver_target_->snapshot();
    try {
        addr = resolve_ipv4_endpoint(target.host, port);
    }
    catch(const std::exception &e) {
        set_error(std::string(label) + " send failed: " + e.what());
        return false;
    }

    for(uint16_t chunk_index = 0; chunk_index < chunk_count; ++chunk_index) {
        const size_t chunk_offset = static_cast<size_t>(chunk_index) * chunk_payload_size;
        const size_t remaining = total_size - chunk_offset;
        const size_t chunk_size = std::min(chunk_payload_size, remaining);
        if(chunk_size > std::numeric_limits<uint16_t>::max()) {
            set_error(std::string(label) + " send failed: UDP fragment is too large");
            return false;
        }

        std::array<uint8_t, kPreviewUdpHeaderSize> udp_header{};
        write_le32(udp_header.data() + 0, kPreviewUdpMagic);
        write_le16(udp_header.data() + 4, kPreviewUdpHeaderVersion);
        write_le16(udp_header.data() + 6, kPreviewUdpHeaderSize);
        write_le32(udp_header.data() + 8, sequence);
        write_le16(udp_header.data() + 12, chunk_index);
        write_le16(udp_header.data() + 14, chunk_count);
        write_le32(udp_header.data() + 16, static_cast<uint32_t>(total_size));
        write_le32(udp_header.data() + 20, static_cast<uint32_t>(chunk_offset));
        write_le16(udp_header.data() + 24, static_cast<uint16_t>(chunk_size));

        std::array<iovec, 3> iovs{};
        int iov_count = 0;
        iovs[iov_count].iov_base = udp_header.data();
        iovs[iov_count].iov_len = udp_header.size();
        ++iov_count;
        if(!append_packet_slice_iovs(packet, chunk_offset, chunk_size, iovs, iov_count)) {
            set_error(std::string(label) + " send failed: packet slice is invalid");
            return false;
        }

        msghdr message{};
        message.msg_name = const_cast<sockaddr *>(reinterpret_cast<const sockaddr *>(&addr));
        message.msg_namelen = sizeof(addr);
        message.msg_iov = iovs.data();
        message.msg_iovlen = static_cast<size_t>(iov_count);
        const ssize_t sent = sendmsg(fd, &message, MSG_NOSIGNAL);
        const size_t expected = kPreviewUdpHeaderSize + chunk_size;
        if(sent < 0 || static_cast<size_t>(sent) != expected) {
            set_error(std::string(label) + " send failed: " + std::strerror(errno));
            return false;
        }
    }
    return true;
}

bool Transport::ensure_media_tcp_connected() {
    const auto target = receiver_target_->snapshot();
    if(media_tcp_fd_ >= 0 && media_target_generation_ != target.generation) {
        close_media_socket();
        last_media_connect_attempt_ = {};
    }
    if(media_tcp_fd_ >= 0) {
        if(!tcp_peer_closed(media_tcp_fd_)) {
            receiver_target_->mark_success(target.generation);
            return true;
        }
        close_media_socket();
        last_media_connect_attempt_ = {};
    }
    if(!can_retry_media_connect()) {
        return false;
    }
    last_media_connect_attempt_ = std::chrono::steady_clock::now();

    const int fd = make_tcp_socket();
    if(fd < 0) {
        return false;
    }
    sockaddr_in addr{};
    try {
        addr = resolve_ipv4_endpoint(target.host, config_.receiver.media_port);
    }
    catch(const std::exception &e) {
        set_error(std::string("media TCP connect failed: ") + e.what());
        close(fd);
        return false;
    }
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
    media_target_generation_ = target.generation;
    receiver_target_->mark_success(target.generation);
    return true;
}

Transport::SendResult Transport::send_all(int fd, const uint8_t *data, size_t size) {
    size_t offset = 0;
    const int timeout_ms = config_.recording_buffer.enabled ? std::max(5000, config_.transport.send_timeout_ms)
                                                            : config_.transport.send_timeout_ms;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
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
    const int timeout_ms = config_.recording_buffer.enabled ? std::max(5000, config_.transport.send_timeout_ms)
                                                            : config_.transport.send_timeout_ms;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

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
        close(media_tcp_fd_);
        media_tcp_fd_ = -1;
    }
    media_target_generation_ = 0;
}

void Transport::close_udp_socket(int &fd) {
    if(fd >= 0) {
        close(fd);
        fd = -1;
    }
}

bool Transport::can_retry_media_connect() const {
    if(last_media_connect_attempt_.time_since_epoch().count() == 0) {
        return true;
    }
    const auto elapsed = std::chrono::steady_clock::now() - last_media_connect_attempt_;
    return elapsed >= std::chrono::milliseconds(config_.transport.reconnect_interval_ms);
}

bool Transport::should_drop_before_write(int fd, size_t packet_size) {
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
    if(queued >= capacity) {
        set_error("media TCP packet dropped under backpressure pending_bytes=" + std::to_string(queued)
                  + " packet_bytes=" + std::to_string(packet_size) + " capacity_bytes=" + std::to_string(capacity));
        return true;
    }
#else
    (void)fd;
    (void)packet_size;
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
