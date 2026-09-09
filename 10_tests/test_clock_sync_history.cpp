#include "gwv3_receiver/clock_sync_manager.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
void check(bool condition, const char *message) {
    if(!condition) throw std::runtime_error(message);
}

uint64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

void exercise_timeline() {
    constexpr uint64_t base = 1788919888952023;
    gwv3::ClockSyncTimeline timeline;
    check(!timeline.map(base).model.valid, "empty history must be invalid");
    gwv3::ClockModel model;
    model.valid = true;
    model.last_sync_us = base;
    model.offset_us = 14611;
    model.drift_ppm = -200;
    timeline.add_model(model);
    const auto original = timeline.map(base + 9000000);
    check(original.model.valid, "initial map");
    model.last_sync_us += 2000000;
    model.offset_us = -35389;
    model.drift_ppm = 200;
    timeline.add_model(model);
    check(timeline.map(base + 9000000).global_timestamp_us == original.global_timestamp_us,
          "late heartbeat rewrote an already published future prediction");
    const auto adjacent = timeline.map(base + 9033333);
    check(adjacent.global_timestamp_us > original.global_timestamp_us, "late heartbeat caused a step");
    check(timeline.map(base - 10000001).model.valid == false, "pre-history timestamp accepted");
    check(!timeline.map(base + 12000001).model.valid, "unbounded future projection accepted");
    check(!timeline.map(std::numeric_limits<uint64_t>::max()).model.valid, "overflow accepted");

    gwv3::ClockSyncTimeline interrupted;
    model.last_sync_us = base;
    model.offset_us = 90000;
    interrupted.add_model(model);
    const auto valid_end = interrupted.map(base + 10000000);
    const auto holdover = interrupted.map(base + 10033333);
    check(!holdover.model.valid, "expired clock was labelled synchronized");
    check(holdover.global_timestamp_us - valid_end.global_timestamp_us == 33333,
          "clock expiry stepped the timeline back to raw sender time");
    model.last_sync_us = base + 20000000;
    model.offset_us = -20000;
    interrupted.add_model(model);
    const auto repeated = interrupted.map(base + 10033333);
    check(!repeated.model.valid && repeated.global_timestamp_us == holdover.global_timestamp_us,
          "late RGB changed the validity or mapping of a Depth holdover frame");

    // Simulated eight hours, not an eight-hour hardware soak. Depth is current,
    // RGB trails by six minutes, reports alternate between the drift limits.
    gwv3::ClockSyncTimeline long_run;
    std::map<uint64_t, int64_t> published;
    int64_t last_delayed = 0;
    model.last_sync_us = base;
    model.offset_us = 14000;
    long_run.add_model(model);
    for(uint64_t seconds = 0; seconds < 8 * 3600; seconds += 2) {
        const uint64_t time = base + seconds * 1000000;
        model.last_sync_us = time;
        model.offset_us = 14000 + (seconds % 8 < 4 ? 766 : -766);
        model.drift_ppm = seconds % 4 ? -200 : 138.313;
        long_run.add_model(model);
        const auto current = long_run.map(time);
        check(current.model.valid, "current frame invalid in soak");
        published[time] = current.global_timestamp_us;
        if(seconds >= 360) {
            const auto historical = long_run.map(time - 360000000);
            check(historical.model.valid, "six-minute delayed frame invalid");
            check(historical.global_timestamp_us == published.at(time - 360000000),
                  "different media arrival order changed the clock mapping");
            check(historical.global_timestamp_us > last_delayed, "history regressed in soak");
            last_delayed = historical.global_timestamp_us;
            published.erase(time - 360000000);
        }
        for(uint64_t frame = 1; frame <= 59; ++frame) {
            const auto next = long_run.map(time + frame * 33333);
            const auto prev = long_run.map(time + (frame - 1) * 33333);
            check(next.model.valid && prev.model.valid, "frame invalid in soak");
            const auto delta = next.global_timestamp_us - prev.global_timestamp_us;
            check(delta >= 33298 && delta <= 33368, "clock mapping violated bounded slew");
        }
        check(long_run.size() <= 4096, "history memory grew without bound");
    }
    check(!long_run.map(base).model.valid, "evicted history was silently extrapolated");
    std::cout << "PASS eight-hour simulated clock timeline and history bounds\n";
}

class Probe {
public:
    Probe() {
        fd = socket(AF_INET, SOCK_DGRAM, 0);
        check(fd >= 0, "probe socket");
        timeval timeout{0, 100000};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        check(bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0, "bind probe");
        socklen_t size = sizeof(address);
        getsockname(fd, reinterpret_cast<sockaddr *>(&address), &size);
    }
    ~Probe() { close(fd); }
    int fd;
    sockaddr_in address{};
};
}

int main() {
    try {
        exercise_timeline();
        uint16_t port;
        {
            Probe reserve;
            port = ntohs(reserve.address.sin_port);
        }
        gwv3::ClockSyncManagerConfig config;
        config.bind_ip = "127.0.0.1";
        config.port = port;
        config.model_timeout_ms = 2000;
        gwv3::ClockSyncManager manager(config);
        check(manager.start(), "manager start");
        Probe probe;
        probe.address.sin_port = htons(port);
        const std::string message = "{\"protocol_version\":\"3.0\",\"message_type\":\"clock_sync_probe\","
                                    "\"sender_id\":\"history-test\",\"sequence\":1,\"t1_sender_send_us\":"
                                    + std::to_string(now_us()) + "}";
        char reply[4096];
        bool registered = false;
        for(int i = 0; i < 20 && !registered; ++i) {
            sendto(probe.fd, message.data(), message.size(), 0,
                   reinterpret_cast<sockaddr *>(&probe.address), sizeof(probe.address));
            registered = recv(probe.fd, reply, sizeof(reply), 0) > 0;
        }
        check(registered, "clock probe registration");
        const uint64_t capture = now_us() - 370000000;
        auto report = [&](uint64_t time, int64_t offset, double drift) {
            check(manager.update_from_sender_report("history-test", offset, 1000, drift, time, "127.0.0.1"),
                  "clock report rejected");
        };
        report(capture - 1000000, 14611, -200);
        const int64_t before = manager.get_global_timestamp_us("history-test", capture);
        report(capture + 369000000, 13845, 138.313);
        const int64_t after = manager.get_global_timestamp_us("history-test", capture + 33603);
        check(after > before, "delayed adjacent frames regressed after a drift report update");
        check(manager.get_global_timestamp_us("history-test", capture) == before,
              "new clock report rewrote the mapping of a historical capture timestamp");
        check(after - before >= 33500 && after - before <= 33700,
              "delayed frame interval was stretched by a receive-time clock model");
        const auto mapping = manager.map_timestamp("history-test", capture);
        check(mapping.model.last_sync_us == capture - 1000000 && mapping.model.offset_us == 14611,
              "frame metadata did not use the historical model snapshot");
        check(!manager.map_timestamp("unknown-sender", capture).model.valid, "unknown model accepted");
        std::this_thread::sleep_for(std::chrono::milliseconds(2100));
        check(!manager.get_model("history-test").valid, "live model did not expire");
        const auto historical = manager.map_timestamp("history-test", capture);
        check(historical.model.valid && historical.global_timestamp_us == before,
              "receive-time report expiry invalidated a previously valid capture");
        manager.stop();
        std::cout << "PASS historical capture timestamps survive drift changes\n";
    }
    catch(const std::exception &e) {
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
}
