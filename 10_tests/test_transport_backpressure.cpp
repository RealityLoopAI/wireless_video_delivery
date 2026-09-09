#include "gwv3_sender/transport.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <algorithm>
#include <chrono>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

static void require(bool value, const char *message) {
    if(!value) throw std::runtime_error(message);
}

struct Socket {
    int fd = -1;
    ~Socket() { if(fd >= 0) close(fd); }
};

template <typename T>
static auto pending(const T &transport, int) -> decltype(transport.media_retry_pending()) {
    return transport.media_retry_pending();
}
[[maybe_unused]] static bool pending(...) { return false; }

int main(int argc, char **argv) {
    try {
        const int stall_seconds = argc > 1 ? std::stoi(argv[1]) : 6;
        require(stall_seconds >= 1 && stall_seconds <= 50, "stall seconds must be 1..50");
        Socket listener{socket(AF_INET, SOCK_STREAM, 0)};
        require(listener.fd >= 0, "listen socket");
        const int receive_buffer = 65536;
        setsockopt(listener.fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(bind(listener.fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0, "bind");
        socklen_t length = sizeof(address);
        require(getsockname(listener.fd, reinterpret_cast<sockaddr *>(&address), &length) == 0, "getsockname");
        require(listen(listener.fd, 2) == 0, "listen");
        timeval timeout{15, 0};
        setsockopt(listener.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        gwv3::AppConfig config;
        config.receiver.ip = "127.0.0.1";
        config.receiver.media_port = ntohs(address.sin_port);
        config.transport.enabled = true;
        config.transport.media_protocol = "tcp";
        // A legacy large SO_SNDBUF must not hide both complete packets in the
        // kernel while the receiver is stalled.
        config.transport.send_buffer_bytes = 32 * 1024 * 1024;
        config.transport.send_timeout_ms = 100;
        config.recording_buffer.enabled = true;
        std::vector<uint8_t> header(94, 0);
        header[0] = 'G'; header[1] = 'W'; header[2] = 'V'; header[3] = '3';
        header[8] = 1;
        std::vector<uint8_t> payload(4 * 1024 * 1024);
        for(size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<uint8_t>(i * 31 + i / 251);
        std::vector<uint8_t> expected(header);
        expected.insert(expected.end(), payload.begin(), payload.end());
        // A second packet on the same connection proves the TCP boundary was
        // preserved: neither duplicated prefixes nor truncated tails are valid.
        expected.insert(expected.end(), header.begin(), header.end());
        expected.insert(expected.end(), payload.begin(), payload.end());

        std::exception_ptr server_error;
        std::thread server([&] {
            try {
                Socket peer{accept(listener.fd, nullptr, nullptr)};
                require(peer.fd >= 0, "accept");
                setsockopt(peer.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
                std::this_thread::sleep_for(std::chrono::seconds(stall_seconds));
                std::vector<uint8_t> received(expected.size());
                size_t offset = 0;
                while(offset < received.size()) {
                    const auto n = recv(peer.fd, received.data() + offset, received.size() - offset, 0);
                    require(n > 0, "connection lost during stalled packet");
                    offset += static_cast<size_t>(n);
                }
                require(received == expected, "resumed packet bytes differ");
            }
            catch(...) { server_error = std::current_exception(); }
        });
        std::exception_ptr client_error;
        size_t retries = 0;
        double max_call_ms = 0;
        try {
            gwv3::Transport transport(config);
            for(int index = 0; index < 2; ++index) {
                const gwv3::MediaPacketView packet{header.data(), header.size(), payload.data(), payload.size()};
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(stall_seconds) + 10s;
                while(true) {
                    const auto start = std::chrono::steady_clock::now();
                    const bool sent = transport.send_media(packet);
                    max_call_ms = std::max(max_call_ms, std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - start).count());
                    if(sent) break;
                    require(pending(transport, 0), "backpressure closed connection instead of retaining packet");
                    require(std::chrono::steady_clock::now() < deadline, "resume timed out");
                    ++retries;
                    std::this_thread::sleep_for(10ms);
                }
                require(!pending(transport, 0), "completed packet still pending");
            }
            require(retries > 0, "backpressure not exercised");
            require(max_call_ms < 1000, "send call blocked shutdown for too long");
        }
        catch(...) { client_error = std::current_exception(); }
        server.join();
        if(client_error) std::rethrow_exception(client_error);
        if(server_error) std::rethrow_exception(server_error);
        std::cout << stall_seconds << "-second stalled reader: bytes=" << expected.size()
                  << " retries=" << retries << " max_send_call_ms=" << max_call_ms
                  << " connections=1 byte_exact=true\n";
        return 0;
    }
    catch(const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
