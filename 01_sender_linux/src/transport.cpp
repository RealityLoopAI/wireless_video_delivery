#include "gwv3_sender/transport.hpp"

#include <cerrno>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <stdexcept>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
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

}  // namespace

Transport::Transport(const AppConfig &config) : config_(config) {
    if(config_.transport.enabled) {
        status_udp_fd_ = make_udp_socket();
    }
}

Transport::~Transport() {
    if(status_udp_fd_ >= 0) {
        close(status_udp_fd_);
    }
    close_media_socket();
}

int Transport::make_udp_socket() {
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if(fd < 0) {
        throw std::runtime_error(std::string("cannot create UDP socket: ") + std::strerror(errno));
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

bool Transport::send_media(const std::vector<uint8_t> &packet, MediaPriority priority) {
    if(!config_.transport.enabled) {
        return true;
    }
    if(!ensure_media_tcp_connected()) {
        return false;
    }
    if(should_drop_before_write(media_tcp_fd_, packet.size(), priority)) {
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
    const auto result = send_all(media_tcp_fd_, packet.data(), packet.size());
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
