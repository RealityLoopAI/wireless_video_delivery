#include "gwv3_sender/receiver_discovery_client.hpp"
#include "gwv3_sender/receiver_target.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <json/json.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

void expect(bool condition, const std::string &message) {
    if(!condition) {
        std::cerr << message << std::endl;
        std::exit(1);
    }
}

}  // namespace

int main() {
    const int server_fd = socket(AF_INET, SOCK_DGRAM, 0);
    expect(server_fd >= 0, "cannot create fake discovery server");
    timeval timeout{};
    timeout.tv_sec = 0;
    timeout.tv_usec = 100000;
    setsockopt(server_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    sockaddr_in server_address{};
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    server_address.sin_port = 0;
    expect(bind(server_fd, reinterpret_cast<const sockaddr *>(&server_address), sizeof(server_address)) == 0,
           "cannot bind fake discovery server");
    socklen_t address_size = sizeof(server_address);
    expect(getsockname(server_fd, reinterpret_cast<sockaddr *>(&server_address), &address_size) == 0,
           "cannot read fake discovery server address");

    std::atomic<bool> server_running{true};
    std::thread server([&] {
        while(server_running) {
            char buffer[4096] = {};
            sockaddr_in peer{};
            socklen_t peer_size = sizeof(peer);
            const ssize_t got = recvfrom(server_fd, buffer, sizeof(buffer), 0,
                                         reinterpret_cast<sockaddr *>(&peer), &peer_size);
            if(got <= 0) {
                continue;
            }
            Json::CharReaderBuilder reader_builder;
            Json::Value request;
            std::string errors;
            std::unique_ptr<Json::CharReader> reader(reader_builder.newCharReader());
            if(!reader->parse(buffer, buffer + got, &request, &errors)
               || request["message_type"].asString() != "receiver_discovery_request") {
                continue;
            }
            Json::Value response(Json::objectValue);
            response["protocol_version"] = "3.0";
            response["message_type"] = "receiver_discovery_response";
            response["receiver_id"] = "receiver-test";
            response["sequence"] = request["sequence"];
            Json::StreamWriterBuilder writer;
            writer["indentation"] = "";
            const auto payload = Json::writeString(writer, response);
            sendto(server_fd, payload.data(), payload.size(), 0,
                   reinterpret_cast<const sockaddr *>(&peer), peer_size);
        }
    });

    const auto state_path = std::filesystem::temp_directory_path()
                            / ("gwv3_receiver_discovery_" + std::to_string(getpid()) + ".json");
    std::filesystem::remove(state_path);
    auto target = std::make_shared<gwv3::ReceiverTarget>("127.0.0.1");
    gwv3::ReceiverDiscoveryConfig config;
    config.enabled = true;
    config.port = ntohs(server_address.sin_port);
    config.interval_ms = 250;
    config.response_window_ms = 100;
    config.sticky_timeout_ms = 500;
    config.state_path = state_path.string();
    gwv3::ReceiverDiscoveryClient client(config, "sender-test", target);
    expect(client.start(), "receiver discovery client did not start");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while(std::chrono::steady_clock::now() < deadline && target->snapshot().receiver_id != "receiver-test") {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const auto selected = target->snapshot();
    expect(selected.receiver_id == "receiver-test" && selected.host == "127.0.0.1" && selected.discovered,
           "receiver discovery client did not select the response source");
    expect(client.healthy(), "receiver discovery client did not become healthy");
    client.stop();

    auto restored_target = std::make_shared<gwv3::ReceiverTarget>("192.0.2.1");
    gwv3::ReceiverDiscoveryClient restored_client(config, "sender-test", restored_target);
    expect(restored_client.start(), "restored discovery client did not start");
    expect(restored_target->snapshot().receiver_id == "receiver-test", "last receiver was not restored from state");
    restored_client.stop();

    server_running = false;
    server.join();
    close(server_fd);
    std::filesystem::remove(state_path);
    std::cout << "receiver discovery client integration test passed" << std::endl;
    return 0;
}
