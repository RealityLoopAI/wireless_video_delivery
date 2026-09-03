struct CameraSummary {
    std::string camera_id;
    uint64_t rgb_frames = 0;
    uint64_t depth_frames = 0;
    bool hardware_encoder = false;
    float depth_scale = 0.0f;
    uint32_t disconnects = 0;
    std::string last_error;
};

CameraSummary snapshot_camera_summary(const CameraRuntime &camera) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    CameraSummary summary;
    summary.camera_id = camera.config.camera_id;
    summary.rgb_frames = camera.rgb_frames;
    summary.depth_frames = camera.depth_frames;
    summary.hardware_encoder = camera.hardware_encoder;
    summary.depth_scale = camera.depth_scale;
    summary.disconnects = camera.disconnects;
    summary.last_error = camera.last_error;
    return summary;
}

struct HotplugRetryCooldown {
    std::string device_key;
    std::chrono::steady_clock::time_point retry_after = std::chrono::steady_clock::now();
};

std::string hotplug_device_key(const OrbbecDeviceIdentity &identity) {
    if(!identity.uid.empty()) {
        return "uid:" + identity.uid;
    }
    if(!identity.serial.empty()) {
        return "serial:" + identity.serial;
    }
    if(!identity.paired_rgb_serial.empty()) {
        return "paired_rgb_serial:" + identity.paired_rgb_serial;
    }
    return "index:" + std::to_string(identity.index);
}

bool hotplug_retry_allowed(const std::vector<HotplugRetryCooldown> &cooldowns, const std::string &device_key,
                           std::chrono::steady_clock::time_point now) {
    for(const auto &cooldown : cooldowns) {
        if(cooldown.device_key == device_key && now < cooldown.retry_after) {
            return false;
        }
    }
    return true;
}

void set_hotplug_retry_cooldown(std::vector<HotplugRetryCooldown> &cooldowns, const std::string &device_key,
                                std::chrono::steady_clock::time_point retry_after) {
    for(auto &cooldown : cooldowns) {
        if(cooldown.device_key == device_key) {
            cooldown.retry_after = retry_after;
            return;
        }
    }
    HotplugRetryCooldown cooldown;
    cooldown.device_key = device_key;
    cooldown.retry_after = retry_after;
    cooldowns.push_back(std::move(cooldown));
}

std::optional<int> numeric_cam_suffix(const std::string &camera_id) {
    if(camera_id.size() <= 3 || camera_id.rfind("cam", 0) != 0) {
        return std::nullopt;
    }
    const auto suffix = camera_id.substr(3);
    if(!std::all_of(suffix.begin(), suffix.end(), [](char ch) { return ch >= '0' && ch <= '9'; })) {
        return std::nullopt;
    }
    try {
        return std::stoi(suffix);
    }
    catch(const std::exception &) {
        return std::nullopt;
    }
}

int initial_hotplug_camera_number(const AppConfig &config) {
    int next_number = 1;
    for(const auto &camera : config.cameras) {
        if(auto suffix = numeric_cam_suffix(camera.camera_id)) {
            next_number = std::max(next_number, *suffix + 1);
        }
    }
    return next_number;
}

std::string format_hotplug_camera_id(int camera_number) {
    std::ostringstream oss;
    oss << "cam" << std::setw(2) << std::setfill('0') << camera_number;
    return oss.str();
}

CameraConfig make_hotplug_camera_config(const AppConfig &config, const OrbbecDeviceIdentity &identity,
                                        const std::string &camera_id) {
    CameraConfig camera = config.cameras.front();
    camera.camera_id = camera_id;
    camera.rgb_rtp_output.enabled = false;
    camera.serial_number.clear();
    camera.uid.clear();
    camera.device_index = static_cast<int>(identity.index);
    if(!identity.uid.empty()) {
        camera.uid = identity.uid;
    }
    else if(!identity.paired_rgb_serial.empty()) {
        camera.serial_number = identity.paired_rgb_serial;
    }
    else if(!identity.serial.empty()) {
        camera.serial_number = identity.serial;
    }
    return camera;
}

size_t active_hotplug_camera_count(const std::vector<std::unique_ptr<CameraRuntime>> &cameras) {
    size_t active = 0;
    for(const auto &camera : cameras) {
        if(camera_counts_against_hotplug_limit(*camera)) {
            ++active;
        }
    }
    return active;
}

bool device_already_claimed(const std::vector<std::unique_ptr<CameraRuntime>> &cameras, const OrbbecDeviceIdentity &identity) {
    for(const auto &camera : cameras) {
        if(camera_matches_identity(*camera, identity)) {
            return true;
        }
    }
    return false;
}

std::vector<OrbbecDeviceIdentity> query_hotplug_device_identities() {
    std::lock_guard<std::mutex> lifecycle_lock(g_camera_lifecycle_mutex);
    ob::Context::setLoggerSeverity(OB_LOG_SEVERITY_WARN);
    ob::Context context;
    return enumerate_orbbec_devices(context);
}

template <typename Sender>
struct MediaSenderPath {
    LatestMediaQueue queue;
    std::unique_ptr<Sender> transport;
    std::mutex mutex;
    std::thread thread;
    std::atomic<bool> running{true};
    bool reliable_retry = false;

    MediaSenderPath(size_t slot_count, size_t rgb_frames_per_slot, size_t depth_frames_per_slot,
                    bool reliable_retry_enabled, std::unique_ptr<Sender> sender)
        : queue(slot_count, rgb_frames_per_slot, depth_frames_per_slot),
          transport(std::move(sender)),
          reliable_retry(reliable_retry_enabled) {}

    ~MediaSenderPath() {
        running = false;
        queue.stop();
        if(thread.joinable()) {
            thread.join();
        }
    }
};

class ThreadJoinGuard {
public:
    explicit ThreadJoinGuard(std::thread &thread) : thread_(thread) {}
    ~ThreadJoinGuard() {
        g_running = false;
        if(thread_.joinable()) {
            thread_.join();
        }
    }

    ThreadJoinGuard(const ThreadJoinGuard &) = delete;
    ThreadJoinGuard &operator=(const ThreadJoinGuard &) = delete;

private:
    std::thread &thread_;
};

class ThreadVectorJoinGuard {
public:
    explicit ThreadVectorJoinGuard(std::vector<std::thread> &threads, std::function<void()> before_join = {})
        : threads_(threads), before_join_(std::move(before_join)) {}
    ~ThreadVectorJoinGuard() {
        g_running = false;
        if(before_join_) {
            before_join_();
        }
        for(auto &thread : threads_) {
            if(thread.joinable()) {
                thread.join();
            }
        }
    }

    ThreadVectorJoinGuard(const ThreadVectorJoinGuard &) = delete;
    ThreadVectorJoinGuard &operator=(const ThreadVectorJoinGuard &) = delete;

private:
    std::vector<std::thread> &threads_;
    std::function<void()> before_join_;
};

template <typename Sender, typename MakeSender>
std::unique_ptr<MediaSenderPath<Sender>> start_media_sender_path(size_t slot_count, size_t rgb_frames_per_slot,
                                                                 size_t depth_frames_per_slot, bool reliable_retry,
                                                                 MakeSender &make_sender, Logger &logger) {
    auto path = std::make_unique<MediaSenderPath<Sender>>(
        slot_count, rgb_frames_per_slot, depth_frames_per_slot, reliable_retry, make_sender());
    auto *path_ptr = path.get();
    path_ptr->thread = std::thread([path_ptr, &logger] {
        try {
            media_sender_loop(path_ptr->queue, *path_ptr->transport, logger, path_ptr->mutex,
                              &path_ptr->running, path_ptr->reliable_retry);
        }
        catch(const std::exception &e) {
            logger.error(std::string("media sender path stopped unexpectedly: ") + e.what());
            g_running = false;
        }
        catch(...) {
            logger.error("media sender path stopped unexpectedly: unknown exception");
            g_running = false;
        }
    });
    return path;
}

template <typename StatusSender, typename MediaSender, typename MakeMediaSender>
void scan_hotplug_cameras(const AppConfig &config, std::vector<std::unique_ptr<CameraRuntime>> &cameras,
                          std::vector<std::thread> &camera_threads,
                          std::vector<std::unique_ptr<MediaSenderPath<MediaSender>>> &rgb_media_paths,
                          std::vector<std::unique_ptr<MediaSenderPath<MediaSender>>> &depth_media_paths,
                          LatestMediaQueue &preview_media_queue, LatestDepthCompressionQueue &depth_compression_queue,
                          ReliableSnapshotQueue &rgb_snapshot_queue,
                          MakeMediaSender &make_media_sender, StatusSender &transport, Logger &logger,
                          std::mutex &transport_mutex, std::chrono::milliseconds preview_interval,
                          int &next_hotplug_camera_number, std::vector<HotplugRetryCooldown> &retry_cooldowns,
                          std::chrono::steady_clock::time_point &next_limit_event) {
    std::vector<OrbbecDeviceIdentity> identities;
    try {
        identities = query_hotplug_device_identities();
    }
    catch(const ob::Error &e) {
        logger.warn("hotplug device scan failed: " + ob_error_text(e));
        return;
    }
    catch(const std::exception &e) {
        logger.warn(std::string("hotplug device scan failed: ") + e.what());
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    for(const auto &identity : identities) {
        if(device_already_claimed(cameras, identity)) {
            continue;
        }

        if(active_hotplug_camera_count(cameras) >= kMaxActiveCameras) {
            if(now >= next_limit_event) {
                const std::string message = "hotplug camera ignored because max active camera count is "
                                            + std::to_string(kMaxActiveCameras) + ": " + device_identity_summary(identity);
                logger.warn(message);
                send_status_locked(transport, logger, transport_mutex,
                                   event_message(config, "warning", "hotplug_camera_ignored", message));
                next_limit_event = now + kHotplugLimitEventInterval;
            }
            continue;
        }

        const auto device_key = hotplug_device_key(identity);
        if(!hotplug_retry_allowed(retry_cooldowns, device_key, now)) {
            continue;
        }

        const auto camera_id = format_hotplug_camera_id(next_hotplug_camera_number);
        auto runtime = std::make_unique<CameraRuntime>();
        runtime->config = make_hotplug_camera_config(config, identity, camera_id);
        runtime->hotplug_dynamic = true;
        runtime->reconnect_enabled = false;
        runtime->rgb_snapshot_queue = &rgb_snapshot_queue;

        try {
            start_camera_runtime(*runtime, logger);
        }
        catch(const ob::Error &e) {
            const std::string error = ob_error_text(e);
            stop_camera(*runtime, logger);
            set_hotplug_retry_cooldown(retry_cooldowns, device_key, now + kHotplugRetryCooldown);
            const std::string message = "hotplug camera start failed camera_id=" + camera_id + " device="
                                        + device_identity_summary(identity) + " error=" + error;
            logger.warn(message);
            send_status_locked(transport, logger, transport_mutex,
                               event_message(config, "warning", "hotplug_camera_start_failed", message));
            continue;
        }
        catch(const std::exception &e) {
            const std::string error = e.what();
            stop_camera(*runtime, logger);
            set_hotplug_retry_cooldown(retry_cooldowns, device_key, now + kHotplugRetryCooldown);
            const std::string message = "hotplug camera start failed camera_id=" + camera_id + " device="
                                        + device_identity_summary(identity) + " error=" + error;
            logger.warn(message);
            send_status_locked(transport, logger, transport_mutex,
                               event_message(config, "warning", "hotplug_camera_start_failed", message));
            continue;
        }

        const size_t rgb_frames_per_slot =
            config.recording_buffer.enabled ? static_cast<size_t>(config.recording_buffer.rgb_frames_per_slot) : 1;
        const size_t depth_frames_per_slot = config.recording_buffer.enabled
                                                 ? static_cast<size_t>(config.recording_buffer.depth_frames_per_slot)
                                                 : kDepthMediaQueuePerSlot;
        auto rgb_path =
            start_media_sender_path<MediaSender>(kMediaSlotsPerCamera, rgb_frames_per_slot, depth_frames_per_slot,
                                                 config.recording_buffer.enabled, make_media_sender, logger);
        auto depth_path =
            start_media_sender_path<MediaSender>(kMediaSlotsPerCamera, rgb_frames_per_slot, depth_frames_per_slot,
                                                 config.recording_buffer.enabled, make_media_sender, logger);
        auto *rgb_path_ptr = rgb_path.get();
        auto *depth_path_ptr = depth_path.get();
        rgb_media_paths.push_back(std::move(rgb_path));
        depth_media_paths.push_back(std::move(depth_path));
        const size_t primary_slot_base = 0;
        const size_t preview_slot_base = preview_media_queue.append_slots(kMediaSlotsPerCamera);
        const size_t compression_slot_base = depth_compression_queue.append_slots(kMediaSlotsPerCamera);
        CameraRuntime *camera_ptr = runtime.get();
        cameras.push_back(std::move(runtime));
        camera_threads.emplace_back([&, camera_ptr, rgb_path_ptr, depth_path_ptr, primary_slot_base, preview_slot_base, compression_slot_base] {
            camera_worker_thread_entry(config, *camera_ptr, primary_slot_base, preview_slot_base, compression_slot_base,
                                       rgb_path_ptr->queue, depth_path_ptr->queue, preview_media_queue, depth_compression_queue,
                                       transport, logger, transport_mutex, preview_interval);
        });
        logger.info("hotplug camera started camera_id=" + camera_id + " preview_slot_base=" + std::to_string(preview_slot_base)
                    + " compression_slot_base=" + std::to_string(compression_slot_base) + " device=" + device_identity_summary(identity));
        send_status_locked(transport, logger, transport_mutex,
                           event_message(config, "info", "camera_connected", "hotplug camera pipeline started", camera_id));
        ++next_hotplug_camera_number;
    }
}

template <typename StatusSender, typename MediaSender, typename MakeMediaSender, typename PreviewMediaSender>
void run_sender(AppConfig config, const Args &args, StatusSender &status_transport, MakeMediaSender &make_media_sender,
                PreviewMediaSender &preview_media_transport, Logger &logger,
                std::shared_ptr<ReceiverTarget> receiver_target = nullptr) {
    if(args.no_send) {
        for(auto &camera : config.cameras) {
            camera.rgb_rtp_output.enabled = false;
        }
    }
    const bool display_available = std::getenv("DISPLAY") != nullptr || std::getenv("WAYLAND_DISPLAY") != nullptr;
    if(args.no_preview || args.no_local_preview || !display_available) {
        config.preview.enabled = false;
    }
    if(args.no_preview) {
        config.web_rgb_preview.enabled = false;
    }
    if(!display_available && !args.no_preview && !args.no_local_preview) {
        logger.warn("DISPLAY/WAYLAND_DISPLAY not set; local preview disabled for this run");
    }
    int preview_fps = config.preview.fps > 0 ? config.preview.fps : 10;
    const bool aligned_rgb_preview_enabled = aligned_rgb_preview_configured(config);
    if(config.preview.enabled && !aligned_rgb_preview_enabled && !config.preview.aligned_rgb) {
        logger.info("aligned RGB preview disabled by config; using per-camera RGB/depth preview windows");
    }
    if(config.preview.enabled && aligned_rgb_preview_enabled && preview_fps < kAlignedRgbPreviewMinFps) {
        logger.info("aligned RGB preview enabled for cam01/cam02; raising local preview fps from " + std::to_string(preview_fps)
                    + " to " + std::to_string(kAlignedRgbPreviewMinFps)
                    + " target_delta_ms=" + std::to_string(kAlignedRgbPreviewTargetDeltaUs / 1000)
                    + " display_max_delta_ms=" + std::to_string(kAlignedRgbPreviewDisplayMaxDeltaUs / 1000));
        preview_fps = kAlignedRgbPreviewMinFps;
    }
    const auto preview_interval = std::chrono::milliseconds(std::max(1, 1000 / preview_fps));

    const auto started = std::chrono::steady_clock::now();
    std::unique_ptr<ClockSyncClient> clock_sync;
    if(!args.no_send && config.clock_sync.enabled) {
        ClockSyncClientConfig clock_config;
        clock_config.enabled = config.clock_sync.enabled;
        clock_config.receiver_ip = config.clock_sync.receiver_ip.empty() ? config.receiver.ip : config.clock_sync.receiver_ip;
        clock_config.port = config.clock_sync.port;
        clock_config.interval_ms = config.clock_sync.interval_ms;
        clock_config.timeout_ms = config.clock_sync.timeout_ms;
        clock_config.max_delay_us = config.clock_sync.max_delay_us;
        clock_config.sample_window = config.clock_sync.sample_window;
        clock_sync = receiver_target ? std::make_unique<ClockSyncClient>(clock_config, config.sender_id, receiver_target)
                                     : std::make_unique<ClockSyncClient>(clock_config, config.sender_id);
        clock_sync->set_log_callbacks([&logger](const std::string &message) { logger.info(message); },
                                      [&logger](const std::string &message) { logger.warn(message); });
        clock_sync->start();
        logger.info("clock_sync client enabled receiver=" + clock_config.receiver_ip + ":" + std::to_string(clock_config.port)
                    + " interval_ms=" + std::to_string(clock_config.interval_ms)
                    + " timeout_ms=" + std::to_string(clock_config.timeout_ms));
    }
    auto cameras = start_cameras(config, logger);
    for(const auto &camera : cameras) {
        const auto &rtp = camera->config.rgb_rtp_output;
        if(rtp.enabled) {
            logger.info("rgb RTP output enabled camera_id=" + camera->config.camera_id
                        + " target=" + rtp.host + ":" + std::to_string(rtp.port)
                        + " payload_type=" + std::to_string(rtp.payload_type)
                        + " mtu=" + std::to_string(rtp.mtu_bytes));
        }
    }
    configure_depth_remap_targets(config, cameras, logger);
    ReliableSnapshotQueue rgb_snapshot_queue;
    for(auto &camera : cameras) {
        camera->rgb_snapshot_queue = &rgb_snapshot_queue;
    }
    auto rgb_snapshot_transport = make_media_sender();
    std::atomic<bool> rgb_snapshot_sender_running{true};
    std::thread rgb_snapshot_sender_thread([&] {
        try {
            rgb_snapshot_sender_loop(config, rgb_snapshot_queue, *rgb_snapshot_transport, logger,
                                     rgb_snapshot_sender_running);
        }
        catch(const std::exception &e) {
            logger.warn(std::string("rgb snapshot sender stopped unexpectedly: ") + e.what());
        }
        catch(...) {
            logger.warn("rgb snapshot sender stopped unexpectedly: unknown exception");
        }
    });
    ThreadJoinGuard rgb_snapshot_sender_thread_guard(rgb_snapshot_sender_thread);
    std::vector<std::unique_ptr<MediaSenderPath<MediaSender>>> rgb_media_paths;
    std::vector<std::unique_ptr<MediaSenderPath<MediaSender>>> depth_media_paths;
    rgb_media_paths.reserve(cameras.size());
    depth_media_paths.reserve(cameras.size());
    const size_t rgb_frames_per_slot =
        config.recording_buffer.enabled ? static_cast<size_t>(config.recording_buffer.rgb_frames_per_slot) : 1;
    const size_t depth_frames_per_slot =
        config.recording_buffer.enabled ? static_cast<size_t>(config.recording_buffer.depth_frames_per_slot) : kDepthMediaQueuePerSlot;
    const size_t depth_compression_frames_per_slot = config.recording_buffer.enabled
                                                        ? static_cast<size_t>(config.recording_buffer.depth_compression_frames_per_slot)
                                                        : kDepthCompressionQueuePerSlot;
    logger.info("recording buffer "
                + std::string(config.recording_buffer.enabled ? "enabled" : "disabled")
                + " rgb_frames_per_slot=" + std::to_string(rgb_frames_per_slot)
                + " depth_frames_per_slot=" + std::to_string(depth_frames_per_slot)
                + " depth_compression_frames_per_slot=" + std::to_string(depth_compression_frames_per_slot));

    LatestMediaQueue preview_media_queue(cameras.size() * kMediaSlotsPerCamera);
    LatestDepthCompressionQueue depth_compression_queue(cameras.size() * kMediaSlotsPerCamera, depth_compression_frames_per_slot);
    std::mutex status_transport_mutex;
    std::mutex preview_media_transport_mutex;

    send_status_locked(status_transport, logger, status_transport_mutex, sender_hello(config));
    for(const auto &camera : cameras) {
        bool online = false;
        std::string last_error;
        {
            std::lock_guard<std::mutex> lock(camera->mutex);
            online = camera->online;
            last_error = camera->last_error;
        }
        if(online) {
            send_status_locked(status_transport, logger, status_transport_mutex,
                               event_message(config, "info", "camera_connected", "camera pipeline started", camera->config.camera_id));
        }
        else {
            send_status_locked(status_transport, logger, status_transport_mutex,
                               camera_offline_message(config, camera->config.camera_id, last_error));
        }
    }

    for(size_t i = 0; i < cameras.size(); ++i) {
        rgb_media_paths.push_back(
            start_media_sender_path<MediaSender>(kMediaSlotsPerCamera, rgb_frames_per_slot, depth_frames_per_slot,
                                                 config.recording_buffer.enabled, make_media_sender, logger));
        depth_media_paths.push_back(
            start_media_sender_path<MediaSender>(kMediaSlotsPerCamera, rgb_frames_per_slot, depth_frames_per_slot,
                                                 config.recording_buffer.enabled, make_media_sender, logger));
    }
    std::thread preview_media_thread([&] {
        try {
            media_sender_loop(preview_media_queue, preview_media_transport, logger, preview_media_transport_mutex);
        }
        catch(const std::exception &e) {
            logger.warn(std::string("preview media sender stopped unexpectedly: ") + e.what());
        }
        catch(...) {
            logger.warn("preview media sender stopped unexpectedly: unknown exception");
        }
    });
    ThreadJoinGuard preview_media_thread_guard(preview_media_thread);
    const size_t depth_worker_count = depth_compression_worker_count(cameras.size());
    logger.info("depth compression workers=" + std::to_string(depth_worker_count));
    std::vector<std::thread> depth_compression_threads;
    ThreadVectorJoinGuard depth_compression_threads_guard(depth_compression_threads, [&] { depth_compression_queue.stop(); });
    depth_compression_threads.reserve(depth_worker_count);
    for(size_t worker = 0; worker < depth_worker_count; ++worker) {
        depth_compression_threads.emplace_back([&, worker] {
            try {
                depth_compression_loop(depth_compression_queue, logger, worker);
            }
            catch(const std::exception &e) {
                logger.error("depth compression worker stopped worker=" + std::to_string(worker) + " error=" + e.what());
                g_running = false;
            }
            catch(...) {
                logger.error("depth compression worker stopped worker=" + std::to_string(worker) + " error=unknown exception");
                g_running = false;
            }
        });
    }
    std::vector<std::thread> camera_threads;
    ThreadVectorJoinGuard camera_threads_guard(camera_threads);
    camera_threads.reserve(cameras.size());
    for(size_t i = 0; i < cameras.size(); ++i) {
        auto *rgb_path = rgb_media_paths[i].get();
        auto *depth_path = depth_media_paths[i].get();
        auto *camera = cameras[i].get();
        const size_t primary_slot_base = 0;
        const size_t preview_slot_base = i * kMediaSlotsPerCamera;
        const size_t compression_slot_base = i * kMediaSlotsPerCamera;
        camera_threads.emplace_back([&, camera, rgb_path, depth_path, primary_slot_base, preview_slot_base, compression_slot_base] {
            camera_worker_thread_entry(config, *camera, primary_slot_base, preview_slot_base, compression_slot_base,
                                       rgb_path->queue, depth_path->queue, preview_media_queue, depth_compression_queue,
                                       status_transport, logger, status_transport_mutex, preview_interval);
        });
    }

    logger.info("primary media uses per-camera separate " + config.transport.media_protocol
                + " senders for rgb and depth paths=" + std::to_string(cameras.size() * 2));

    if(config.web_rgb_preview.enabled) {
        logger.info(std::string("web rgb preview media uses separate ")
                    + (config.web_rgb_preview.udp_enabled ? "udp" : "tcp") + " sender");
    }

    auto next_heartbeat = std::chrono::steady_clock::now();
    auto next_camera_announce = std::chrono::steady_clock::now() + kCameraAnnounceInterval;
    auto next_perf_log = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    auto next_preview = std::chrono::steady_clock::now();
    auto next_hotplug_scan = std::chrono::steady_clock::now() + kHotplugScanInterval;
    auto next_hotplug_limit_event = std::chrono::steady_clock::now();
    auto next_rgb_snapshot_poll = std::chrono::steady_clock::now();
    auto next_rgb_snapshot_expiry = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    int next_hotplug_camera_number = initial_hotplug_camera_number(config);
    std::vector<HotplugRetryCooldown> hotplug_retry_cooldowns;
    if(!config.hotplug.enabled) {
        logger.info("hotplug camera scan disabled by config");
    }
    const auto stop_at = args.run_seconds > 0 ? started + std::chrono::seconds(args.run_seconds) : std::chrono::steady_clock::time_point::max();

    while(g_running && std::chrono::steady_clock::now() < stop_at) {
        const auto now = std::chrono::steady_clock::now();
        process_receiver_controls(status_transport, config, cameras, logger, status_transport_mutex,
                                  clock_sync.get());
        poll_rgb_snapshot_requests(config, cameras, logger, now, next_rgb_snapshot_poll);
        if(now >= next_rgb_snapshot_expiry) {
            expire_rgb_snapshot_requests(config, cameras, logger, now);
            next_rgb_snapshot_expiry = now + std::chrono::seconds(1);
        }
        if(now >= next_heartbeat) {
            for(auto &camera : cameras) {
                send_status_locked(status_transport, logger, status_transport_mutex,
                                   camera_heartbeat(config, *camera, started, clock_sync.get(), receiver_target.get()));
            }
            next_heartbeat = now + std::chrono::milliseconds(config.heartbeat_interval_ms);
        }
        if(now >= next_camera_announce) {
            for(auto &camera : cameras) {
                if(camera_online(*camera)) {
                    send_status_locked(status_transport, logger, status_transport_mutex, camera_announce(config, *camera));
                    set_camera_announced(*camera, true);
                }
            }
            next_camera_announce = now + kCameraAnnounceInterval;
        }
        if(now >= next_perf_log) {
            for(auto &camera : cameras) {
                log_perf(*camera, logger, now);
            }
            next_perf_log = now + std::chrono::seconds(1);
        }
        if(config.preview.enabled && now >= next_preview) {
            const auto preview_started = std::chrono::steady_clock::now();
            bool aligned_preview_expected = false;
            bool aligned_preview_drawn = false;
            if(aligned_rgb_preview_enabled) {
                auto *cam01_preview = find_camera_by_id(cameras, "cam01");
                auto *cam02_preview = find_camera_by_id(cameras, "cam02");
                aligned_preview_expected =
                    cam01_preview != nullptr && cam02_preview != nullptr && camera_online(*cam01_preview) && camera_online(*cam02_preview);
                aligned_preview_drawn = preview_aligned_rgb_pair(cameras);
            }
            const double preview_ms = elapsed_ms(preview_started, std::chrono::steady_clock::now());
            if(aligned_preview_drawn) {
                for(auto &camera : cameras) {
                    if(camera && (camera->config.camera_id == "cam01" || camera->config.camera_id == "cam02")) {
                        record_preview_ms(*camera, preview_ms);
                    }
                }
            }
            else if(!aligned_preview_expected) {
                for(auto &camera : cameras) {
                    const auto fallback_preview_started = std::chrono::steady_clock::now();
                    preview_frame(*camera, true);
                    record_preview_ms(*camera, elapsed_ms(fallback_preview_started, std::chrono::steady_clock::now()));
                }
            }
            next_preview = now + preview_interval;
        }
        if(config.hotplug.enabled && now >= next_hotplug_scan) {
            scan_hotplug_cameras(config, cameras, camera_threads, rgb_media_paths, depth_media_paths, preview_media_queue,
                                 depth_compression_queue, rgb_snapshot_queue, make_media_sender, status_transport, logger, status_transport_mutex,
                                 preview_interval, next_hotplug_camera_number, hotplug_retry_cooldowns, next_hotplug_limit_event);
            next_hotplug_scan = now + kHotplugScanInterval;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    if(clock_sync) {
        clock_sync->stop();
    }
    g_running = false;
    for(auto &thread : camera_threads) {
        if(thread.joinable()) {
            thread.join();
        }
    }
    depth_compression_queue.stop();
    for(auto &thread : depth_compression_threads) {
        if(thread.joinable()) {
            thread.join();
        }
    }
    for(auto &path : rgb_media_paths) {
        path->running = false;
        path->queue.stop();
    }
    for(auto &path : depth_media_paths) {
        path->running = false;
        path->queue.stop();
    }
    rgb_snapshot_sender_running = false;
    rgb_snapshot_queue.stop();
    preview_media_queue.stop();
    for(auto &path : rgb_media_paths) {
        if(path->thread.joinable()) {
            path->thread.join();
        }
    }
    for(auto &path : depth_media_paths) {
        if(path->thread.joinable()) {
            path->thread.join();
        }
    }
    if(preview_media_thread.joinable()) {
        preview_media_thread.join();
    }
    if(rgb_snapshot_sender_thread.joinable()) {
        rgb_snapshot_sender_thread.join();
    }

    for(auto &camera : cameras) {
        const auto summary = snapshot_camera_summary(*camera);
        std::ostringstream oss;
        oss << "camera summary camera_id=" << summary.camera_id << " rgb_frames=" << summary.rgb_frames
            << " depth_frames=" << summary.depth_frames << " hardware_encoder=" << (summary.hardware_encoder ? "true" : "false")
            << " depth_scale=" << summary.depth_scale << " disconnects=" << summary.disconnects;
        if(!summary.last_error.empty()) {
            oss << " last_error=" << summary.last_error;
        }
        logger.info(oss.str());
        stop_camera(*camera, logger);
    }
}
