#include "gwv3_sender/transport.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
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
    return fd;
}

bool Transport::send_status(const std::string &json_message) {
    if(!config_.transport.enabled) {
        return true;
    }
    return send_udp_status(json_message);
}

bool Transport::send_media(const std::vector<uint8_t> &packet) {
    if(!config_.transport.enabled) {
        return true;
    }
    if(!ensure_media_tcp_connected()) {
        return false;
    }
    if(!send_all(media_tcp_fd_, packet.data(), packet.size())) {
        close_media_socket();
        return false;
    }
    return true;
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
    set_nonblock(fd, false);
    media_tcp_fd_ = fd;
    return true;
}

bool Transport::send_all(int fd, const uint8_t *data, size_t size) {
    size_t offset = 0;
    while(offset < size) {
        const ssize_t sent = send(fd, data + offset, size - offset, MSG_NOSIGNAL);
        if(sent <= 0) {
            set_error(std::string("media TCP send failed: ") + std::strerror(errno));
            return false;
        }
        offset += static_cast<size_t>(sent);
    }
    return true;
}

void Transport::close_media_socket() {
    if(media_tcp_fd_ >= 0) {
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

void Transport::set_error(const std::string &message) {
    last_error_ = message;
}

std::string Transport::last_error() const {
    return last_error_;
}

}  // namespace gwv3
