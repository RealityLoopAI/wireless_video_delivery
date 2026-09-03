struct Config {
    struct RecordingStagingConfig {
        bool enabled = false;
        std::string root;
        bool defer_player_compatible_finalize = true;
        std::string rgb_output_mode = "conventional_mp4";
        int idle_finalize_ms = 5000;
        std::string direct_publish_hidden_directory = ".gwv3_direct_inprogress";
    };

    struct PhotoCaptureConfig {
        bool enabled = false;
        std::string staging_root;
        std::string nas_subdirectory = "voice_photos";
        size_t max_jpeg_bytes = 8ull * 1024ull * 1024ull;
        size_t queue_max_items = 128;
    };

    struct TaskAudioConfig {
        bool enabled = false;
        int finalize_wait_ms = 3000;
        int poll_interval_ms = 25;
        std::string notify_host = "127.0.0.1";
        uint16_t notify_port = 50130;
        std::set<std::string> sender_ids;
    };

    struct NasAutoMountConfig {
        bool enabled = false;
        std::string status_path = "/run/gwv3/nas-mount-status.json";
        int status_max_age_ms = 10000;
        bool require_for_new_recording = true;
    };

    std::string status_bind_ip = "0.0.0.0";
    uint16_t status_port = 50011;
    std::string media_bind_ip = "0.0.0.0";
    uint16_t media_port = 50010;
    bool preview_enabled = true;
    bool media_udp_enabled = false;
    std::string media_udp_bind_ip = "0.0.0.0";
    uint16_t media_udp_port = 50013;
    bool preview_udp_enabled = false;
    std::string preview_udp_bind_ip = "0.0.0.0";
    uint16_t preview_udp_port = 50014;
    ReceiverDiscoveryServerConfig receiver_discovery;
    ClockSyncManagerConfig clock_sync;
    std::string admin_bind_ip = "127.0.0.1";
    uint16_t admin_port = 18080;
    std::string nas_root = "/home/fz/Desktop/nas";
    NasAutoMountConfig nas_auto_mount;
    RecordingStagingConfig recording_staging;
    PhotoCaptureConfig photo_capture;
    TaskAudioConfig task_audio;
    std::string log_directory = "08_reports/receiver_logs";
    std::string state_path = "06_configs/receiver_runtime_state.json";
    std::string ffmpeg_path = "ffmpeg";
    int segment_seconds = 300;
    int segment_keyframe_lead_ms = 500;
    int recording_start_lead_ms = 1000;
    int depth_fps = 30;
    bool write_debug_h264 = false;
    bool write_debug_depth_raw = false;
    std::set<std::string> rgb_h264_full_range_camera_keys;
    size_t max_payload_bytes = 32ull * 1024ull * 1024ull;
    size_t record_queue_max_bytes = kDefaultRecordQueueMaxBytes;
    size_t record_queue_total_max_bytes = 2ull * 1024ull * 1024ull * 1024ull;
    size_t record_finalize_max_pending_segments = kDefaultRecordFinalizeMaxPendingSegments;
    size_t record_finalize_workers = 1;
    uint64_t min_free_disk_bytes = 2ull * 1024ull * 1024ull * 1024ull;
    int min_free_disk_percent = 0;
    int warn_free_disk_percent = 0;
};

Config load_config(const std::string &path) {
    std::ifstream input(path);
    if(!input) {
        throw std::runtime_error("cannot open receiver config: " + path);
    }
    const std::string json((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    Json::Value root;
    if(!parse_json_object_strict(json, root)) {
        throw std::runtime_error("invalid receiver JSON config: " + path);
    }

    const auto string_value = [](const Json::Value &object, const char *key, const std::string &fallback) {
        const auto &value = object[key];
        if(value.isNull()) {
            return fallback;
        }
        if(!value.isString()) {
            throw std::runtime_error(std::string("receiver config field must be a string: ") + key);
        }
        return value.asString();
    };
    const auto int_value = [](const Json::Value &object, const char *key, int fallback) {
        const auto &value = object[key];
        if(value.isNull()) {
            return fallback;
        }
        if(!value.isInt()) {
            throw std::runtime_error(std::string("receiver config field must be an integer: ") + key);
        }
        return value.asInt();
    };
    const auto bool_value = [](const Json::Value &object, const char *key, bool fallback) {
        const auto &value = object[key];
        if(value.isNull()) {
            return fallback;
        }
        if(!value.isBool()) {
            throw std::runtime_error(std::string("receiver config field must be a boolean: ") + key);
        }
        return value.asBool();
    };
    const auto string_set_value = [](const Json::Value &object, const char *key) {
        std::set<std::string> values;
        const auto &value = object[key];
        if(value.isNull()) {
            return values;
        }
        if(!value.isArray()) {
            throw std::runtime_error(std::string("receiver config field must be a string array: ") + key);
        }
        for(const auto &item : value) {
            if(!item.isString() || item.asString().empty()) {
                throw std::runtime_error(std::string("receiver config field contains an invalid camera key: ") + key);
            }
            values.insert(item.asString());
        }
        return values;
    };
    const auto port_value = [&](const Json::Value &object, const char *key, uint16_t fallback) {
        const int value = int_value(object, key, fallback);
        if(value <= 0 || value > 65535) {
            throw std::runtime_error(std::string("invalid receiver config port: ") + key);
        }
        return static_cast<uint16_t>(value);
    };

    Config cfg;
    cfg.status_bind_ip = string_value(root, "status_bind_ip", cfg.status_bind_ip);
    cfg.status_port = port_value(root, "status_port", cfg.status_port);
    cfg.media_bind_ip = string_value(root, "media_bind_ip", cfg.media_bind_ip);
    cfg.media_port = port_value(root, "media_port", cfg.media_port);
    cfg.preview_enabled = bool_value(root, "preview_enabled", cfg.preview_enabled);
    cfg.media_udp_enabled = bool_value(root, "media_udp_enabled", cfg.media_udp_enabled);
    cfg.media_udp_bind_ip = string_value(root, "media_udp_bind_ip", cfg.media_udp_bind_ip);
    cfg.media_udp_port = port_value(root, "media_udp_port", cfg.media_udp_port);
    cfg.preview_udp_enabled = bool_value(root, "preview_udp_enabled", cfg.preview_udp_enabled);
    cfg.preview_udp_bind_ip = string_value(root, "preview_udp_bind_ip", cfg.preview_udp_bind_ip);
    cfg.preview_udp_port = port_value(root, "preview_udp_port", cfg.preview_udp_port);
    Json::Value receiver_discovery(Json::objectValue);
    if(!root["receiver_discovery"].isNull()) {
        if(!root["receiver_discovery"].isObject()) {
            throw std::runtime_error("receiver config field must be an object: receiver_discovery");
        }
        receiver_discovery = root["receiver_discovery"];
    }
    cfg.receiver_discovery.enabled =
        bool_value(receiver_discovery, "enabled", cfg.receiver_discovery.enabled);
    cfg.receiver_discovery.bind_ip =
        string_value(receiver_discovery, "bind_ip", cfg.receiver_discovery.bind_ip);
    cfg.receiver_discovery.port =
        port_value(receiver_discovery, "port", cfg.receiver_discovery.port);
    cfg.receiver_discovery.receiver_id =
        string_value(receiver_discovery, "receiver_id", cfg.receiver_discovery.receiver_id);
    Json::Value clock_sync(Json::objectValue);
    if(!root["clock_sync"].isNull()) {
        if(!root["clock_sync"].isObject()) {
            throw std::runtime_error("receiver config field must be an object: clock_sync");
        }
        clock_sync = root["clock_sync"];
    }
    cfg.clock_sync.enabled = bool_value(clock_sync, "enabled", cfg.clock_sync.enabled);
    cfg.clock_sync.bind_ip = string_value(clock_sync, "bind_ip", cfg.clock_sync.bind_ip);
    cfg.clock_sync.port = port_value(clock_sync, "port", cfg.clock_sync.port);
    cfg.clock_sync.model_timeout_ms = int_value(clock_sync, "model_timeout_ms", cfg.clock_sync.model_timeout_ms);
    cfg.receiver_discovery.media_port = cfg.media_port;
    cfg.receiver_discovery.status_port = cfg.status_port;
    cfg.receiver_discovery.clock_sync_port = cfg.clock_sync.port;
    cfg.receiver_discovery.media_udp_port = cfg.media_udp_port;
    cfg.receiver_discovery.preview_udp_port = cfg.preview_udp_port;
    cfg.admin_bind_ip = string_value(root, "admin_bind_ip", cfg.admin_bind_ip);
    cfg.admin_port = port_value(root, "admin_port", cfg.admin_port);
    cfg.nas_root = string_value(root, "nas_root", cfg.nas_root);
    Json::Value nas_auto_mount(Json::objectValue);
    if(!root["nas_auto_mount"].isNull()) {
        if(!root["nas_auto_mount"].isObject()) {
            throw std::runtime_error("receiver config field must be an object: nas_auto_mount");
        }
        nas_auto_mount = root["nas_auto_mount"];
    }
    cfg.nas_auto_mount.enabled = bool_value(nas_auto_mount, "enabled", cfg.nas_auto_mount.enabled);
    cfg.nas_auto_mount.status_path =
        string_value(nas_auto_mount, "status_path", cfg.nas_auto_mount.status_path);
    cfg.nas_auto_mount.status_max_age_ms =
        int_value(nas_auto_mount, "status_max_age_ms", cfg.nas_auto_mount.status_max_age_ms);
    cfg.nas_auto_mount.require_for_new_recording =
        bool_value(nas_auto_mount, "require_for_new_recording", cfg.nas_auto_mount.require_for_new_recording);
    Json::Value recording_staging(Json::objectValue);
    if(!root["recording_staging"].isNull()) {
        if(!root["recording_staging"].isObject()) {
            throw std::runtime_error("receiver config field must be an object: recording_staging");
        }
        recording_staging = root["recording_staging"];
    }
    cfg.recording_staging.enabled = bool_value(recording_staging, "enabled", cfg.recording_staging.enabled);
    cfg.recording_staging.root = string_value(recording_staging, "root", cfg.recording_staging.root);
    cfg.recording_staging.defer_player_compatible_finalize =
        bool_value(recording_staging, "defer_player_compatible_finalize",
                   cfg.recording_staging.defer_player_compatible_finalize);
    cfg.recording_staging.rgb_output_mode =
        string_value(recording_staging, "rgb_output_mode", cfg.recording_staging.rgb_output_mode);
    cfg.recording_staging.idle_finalize_ms =
        int_value(recording_staging, "idle_finalize_ms", cfg.recording_staging.idle_finalize_ms);
    cfg.recording_staging.direct_publish_hidden_directory =
        string_value(recording_staging, "direct_publish_hidden_directory",
                     cfg.recording_staging.direct_publish_hidden_directory);
    Json::Value photo_capture(Json::objectValue);
    if(!root["photo_capture"].isNull()) {
        if(!root["photo_capture"].isObject()) {
            throw std::runtime_error("receiver config field must be an object: photo_capture");
        }
        photo_capture = root["photo_capture"];
    }
    cfg.photo_capture.enabled = bool_value(photo_capture, "enabled", cfg.photo_capture.enabled);
    cfg.photo_capture.staging_root = string_value(photo_capture, "staging_root", cfg.photo_capture.staging_root);
    cfg.photo_capture.nas_subdirectory =
        string_value(photo_capture, "nas_subdirectory", cfg.photo_capture.nas_subdirectory);
    const int photo_max_jpeg_mb = int_value(photo_capture, "max_jpeg_mb", 8);
    const int photo_queue_max_items = int_value(photo_capture, "queue_max_items", 128);
    if(photo_max_jpeg_mb <= 0 || photo_max_jpeg_mb > 64
       || photo_queue_max_items <= 0 || photo_queue_max_items > 4096) {
        throw std::runtime_error("photo_capture limits are out of range");
    }
    cfg.photo_capture.max_jpeg_bytes = static_cast<size_t>(photo_max_jpeg_mb) * 1024ull * 1024ull;
    cfg.photo_capture.queue_max_items = static_cast<size_t>(photo_queue_max_items);
    Json::Value task_audio(Json::objectValue);
    if(!root["task_audio"].isNull()) {
        if(!root["task_audio"].isObject()) {
            throw std::runtime_error("receiver config field must be an object: task_audio");
        }
        task_audio = root["task_audio"];
    }
    cfg.task_audio.enabled = bool_value(task_audio, "enabled", cfg.task_audio.enabled);
    cfg.task_audio.finalize_wait_ms = int_value(task_audio, "finalize_wait_ms", cfg.task_audio.finalize_wait_ms);
    cfg.task_audio.poll_interval_ms = int_value(task_audio, "poll_interval_ms", cfg.task_audio.poll_interval_ms);
    cfg.task_audio.notify_host = string_value(task_audio, "notify_host", cfg.task_audio.notify_host);
    cfg.task_audio.notify_port = port_value(task_audio, "notify_port", cfg.task_audio.notify_port);
    cfg.task_audio.sender_ids = string_set_value(task_audio, "sender_ids");
    cfg.log_directory = string_value(root, "log_directory", cfg.log_directory);
    cfg.state_path = string_value(root, "state_path", cfg.state_path);
    cfg.ffmpeg_path = string_value(root, "ffmpeg_path", cfg.ffmpeg_path);
    cfg.segment_seconds = int_value(root, "segment_seconds", cfg.segment_seconds);
    cfg.segment_keyframe_lead_ms =
        int_value(root, "segment_keyframe_lead_ms", cfg.segment_keyframe_lead_ms);
    cfg.recording_start_lead_ms = int_value(root, "recording_start_lead_ms", cfg.recording_start_lead_ms);
    cfg.depth_fps = int_value(root, "depth_fps", cfg.depth_fps);
    cfg.write_debug_h264 = bool_value(root, "write_debug_h264", cfg.write_debug_h264);
    cfg.write_debug_depth_raw = bool_value(root, "write_debug_depth_raw", cfg.write_debug_depth_raw);
    cfg.rgb_h264_full_range_camera_keys = string_set_value(root, "rgb_h264_full_range_camera_keys");
    const int max_payload_mb = int_value(root, "max_payload_mb", 32);
    const int record_queue_max_mb = int_value(root, "record_queue_max_mb", 512);
    const int min_free_disk_mb = int_value(root, "min_free_disk_mb", 2048);
    cfg.min_free_disk_percent = int_value(root, "min_free_disk_percent", cfg.min_free_disk_percent);
    cfg.warn_free_disk_percent = int_value(root, "warn_free_disk_percent", cfg.warn_free_disk_percent);
    const int record_queue_total_max_mb = int_value(root, "record_queue_total_max_mb", 2048);
    const int record_finalize_max_pending_segments =
        int_value(root, "record_finalize_max_pending_segments", static_cast<int>(kDefaultRecordFinalizeMaxPendingSegments));
    const int record_finalize_workers =
        int_value(root, "record_finalize_workers", static_cast<int>(cfg.record_finalize_workers));
    if(max_payload_mb <= 0 || max_payload_mb > 128 || record_queue_max_mb <= 0 || record_queue_max_mb > 4096
       || record_queue_total_max_mb <= 0 || record_queue_total_max_mb > 16384
       || record_finalize_max_pending_segments <= 0 || record_finalize_max_pending_segments > 128
       || record_finalize_workers <= 0 || record_finalize_workers > 32
       || min_free_disk_mb < 0 || min_free_disk_mb > 1024 * 1024
       || cfg.min_free_disk_percent < 0 || cfg.min_free_disk_percent > 100
       || cfg.warn_free_disk_percent < cfg.min_free_disk_percent || cfg.warn_free_disk_percent > 100) {
        throw std::runtime_error("receiver payload/record queue limits are out of range");
    }
    cfg.max_payload_bytes = static_cast<size_t>(max_payload_mb) * 1024ull * 1024ull;
    cfg.record_queue_max_bytes = static_cast<size_t>(record_queue_max_mb) * 1024ull * 1024ull;
    cfg.record_queue_total_max_bytes = static_cast<size_t>(record_queue_total_max_mb) * 1024ull * 1024ull;
    cfg.record_finalize_max_pending_segments = static_cast<size_t>(record_finalize_max_pending_segments);
    cfg.record_finalize_workers = static_cast<size_t>(record_finalize_workers);
    cfg.min_free_disk_bytes = static_cast<uint64_t>(min_free_disk_mb) * 1024ull * 1024ull;

    if(cfg.segment_seconds <= 0) {
        throw std::runtime_error("segment_seconds must be positive");
    }
    if(cfg.segment_keyframe_lead_ms < 0 || cfg.segment_keyframe_lead_ms > 5000) {
        throw std::runtime_error("segment_keyframe_lead_ms must be between 0 and 5000");
    }
    if(cfg.recording_start_lead_ms < 0 || cfg.recording_start_lead_ms > 10000) {
        throw std::runtime_error("recording_start_lead_ms must be between 0 and 10000");
    }
    if(cfg.depth_fps <= 0) {
        throw std::runtime_error("depth_fps must be positive");
    }
    if(cfg.clock_sync.model_timeout_ms <= 0) {
        throw std::runtime_error("clock_sync.model_timeout_ms must be positive");
    }
    if(cfg.nas_auto_mount.status_path.empty() || cfg.nas_auto_mount.status_max_age_ms < 1000
       || cfg.nas_auto_mount.status_max_age_ms > 300000) {
        throw std::runtime_error("nas_auto_mount status_path/timing is invalid");
    }
    if(cfg.recording_staging.enabled && cfg.recording_staging.root.empty()) {
        throw std::runtime_error("recording_staging.root must not be empty when staging is enabled");
    }
    if(cfg.recording_staging.idle_finalize_ms < 1000 || cfg.recording_staging.idle_finalize_ms > 300000) {
        throw std::runtime_error("recording_staging.idle_finalize_ms must be between 1000 and 300000");
    }
    if(cfg.recording_staging.rgb_output_mode != "conventional_mp4"
       && cfg.recording_staging.rgb_output_mode != "fragmented_mp4") {
        throw std::runtime_error(
            "recording_staging.rgb_output_mode must be conventional_mp4 or fragmented_mp4");
    }
    if(cfg.recording_staging.direct_publish_hidden_directory.empty()
       || !is_safe_storage_text(cfg.recording_staging.direct_publish_hidden_directory)
       || cfg.recording_staging.direct_publish_hidden_directory.front() != '.') {
        throw std::runtime_error(
            "recording_staging.direct_publish_hidden_directory must be one hidden safe directory name");
    }
    if(cfg.photo_capture.enabled && cfg.photo_capture.staging_root.empty()) {
        throw std::runtime_error("photo_capture.staging_root must not be empty when photo capture is enabled");
    }
    if(cfg.photo_capture.nas_subdirectory.empty() || !is_safe_storage_text(cfg.photo_capture.nas_subdirectory)) {
        throw std::runtime_error("photo_capture.nas_subdirectory must be one safe directory name");
    }
    if(cfg.task_audio.finalize_wait_ms < 0 || cfg.task_audio.finalize_wait_ms > 10000
       || cfg.task_audio.poll_interval_ms < 5 || cfg.task_audio.poll_interval_ms > 1000) {
        throw std::runtime_error("task_audio finalize timing is out of range");
    }
    if(cfg.task_audio.notify_host.empty()) {
        throw std::runtime_error("task_audio.notify_host must not be empty");
    }
    if(cfg.admin_bind_ip != "127.0.0.1") {
        throw std::runtime_error("admin_bind_ip must remain 127.0.0.1; expose only the authenticated Web proxy");
    }
    (void)make_bind_addr(cfg.status_bind_ip, cfg.status_port);
    (void)make_bind_addr(cfg.media_bind_ip, cfg.media_port);
    (void)make_bind_addr(cfg.media_udp_bind_ip, cfg.media_udp_port);
    (void)make_bind_addr(cfg.preview_udp_bind_ip, cfg.preview_udp_port);
    (void)make_bind_addr(cfg.receiver_discovery.bind_ip, cfg.receiver_discovery.port);
    (void)make_bind_addr(cfg.admin_bind_ip, cfg.admin_port);
    if(cfg.nas_root.empty() || cfg.log_directory.empty() || cfg.state_path.empty() || cfg.ffmpeg_path.empty()) {
        throw std::runtime_error("receiver path fields must not be empty");
    }
    for(const auto &key : cfg.rgb_h264_full_range_camera_keys) {
        if(key.size() > kMaxProtocolIdBytes * 2 + 1
           || !std::all_of(key.begin(), key.end(), [](unsigned char ch) {
                  return std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.';
              })) {
            throw std::runtime_error("invalid camera key in rgb_h264_full_range_camera_keys: " + key);
        }
    }
    std::set<uint16_t> udp_ports{cfg.status_port};
    const auto add_udp_port = [&](bool enabled, uint16_t port, const char *name) {
        if(enabled && !udp_ports.insert(port).second) {
            throw std::runtime_error(std::string("receiver UDP port conflict: ") + name);
        }
    };
    add_udp_port(cfg.clock_sync.enabled, cfg.clock_sync.port, "clock_sync.port");
    add_udp_port(cfg.receiver_discovery.enabled, cfg.receiver_discovery.port, "receiver_discovery.port");
    add_udp_port(cfg.media_udp_enabled, cfg.media_udp_port, "media_udp_port");
    add_udp_port(cfg.preview_enabled && cfg.preview_udp_enabled, cfg.preview_udp_port, "preview_udp_port");
    if(cfg.admin_port == cfg.media_port
       && (cfg.admin_bind_ip == cfg.media_bind_ip || cfg.admin_bind_ip == "0.0.0.0" || cfg.media_bind_ip == "0.0.0.0")) {
        throw std::runtime_error("admin_port conflicts with media_port on the same bind address");
    }
    return cfg;
}

std::filesystem::path direct_recording_root(const Config &cfg) {
    return std::filesystem::path(cfg.nas_root) / cfg.recording_staging.direct_publish_hidden_directory;
}

std::filesystem::path recording_write_root(const Config &cfg) {
    return cfg.recording_staging.enabled ? std::filesystem::path(cfg.recording_staging.root)
                                         : direct_recording_root(cfg);
}

bool storage_space_meets_limits(const std::filesystem::space_info &space, const Config &cfg,
                                uint64_t extra_headroom_bytes = 0) {
    if(cfg.min_free_disk_bytes > std::numeric_limits<uint64_t>::max() - extra_headroom_bytes
       || space.available < cfg.min_free_disk_bytes + extra_headroom_bytes) {
        return false;
    }
    if(cfg.min_free_disk_percent <= 0 || space.capacity == 0) {
        return true;
    }
    const long double free_percent = static_cast<long double>(space.available) * 100.0L
                                     / static_cast<long double>(space.capacity);
    return free_percent >= static_cast<long double>(cfg.min_free_disk_percent);
}

bool rgb_h264_full_range_for_camera(const Config &cfg, const std::string &sender_id, const std::string &camera_id) {
    return cfg.rgb_h264_full_range_camera_keys.count(camera_key(sender_id, camera_id)) != 0;
}

struct RuntimeState {
    std::string default_file_prefix;
    std::map<std::string, std::string> camera_names;
    std::map<std::string, std::string> camera_file_prefixes;
    std::map<std::string, std::string> camera_announces;
};

std::map<std::string, std::string> json_string_map_field(const Json::Value &root, const char *key) {
    std::map<std::string, std::string> result;
    const auto &object = root[key];
    if(!object.isObject()) {
        return result;
    }
    for(const auto &name : object.getMemberNames()) {
        if(result.size() >= kMaxTrackedCameras || name.size() > 160 || !is_safe_storage_text(name) || !object[name].isString()) {
            continue;
        }
        result[name] = object[name].asString();
    }
    return result;
}

std::map<std::string, std::string> json_object_map_field(const Json::Value &root, const char *key) {
    std::map<std::string, std::string> result;
    const auto &object = root[key];
    if(!object.isObject()) {
        return result;
    }
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    for(const auto &name : object.getMemberNames()) {
        if(result.size() >= kMaxTrackedCameras || name.size() > 160 || !is_safe_storage_text(name) || !object[name].isObject()) {
            continue;
        }
        result[name] = Json::writeString(builder, object[name]);
    }
    return result;
}

RuntimeState load_runtime_state(const std::string &path) {
    RuntimeState state;
    std::ifstream input(path);
    if(!input) {
        return state;
    }
    const std::string json((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    Json::Value root;
    if(!parse_json_object_strict(json, root)) {
        return state;
    }
    state.default_file_prefix = root["default_file_prefix"].isString() ? root["default_file_prefix"].asString() : std::string{};
    state.camera_names = json_string_map_field(root, "camera_names");
    state.camera_file_prefixes = json_string_map_field(root, "camera_file_prefixes");
    state.camera_announces = json_object_map_field(root, "camera_announces");
    if(!is_safe_storage_text(state.default_file_prefix)) {
        state.default_file_prefix.clear();
    }
    for(auto it = state.camera_names.begin(); it != state.camera_names.end();) {
        if(!is_safe_storage_text(it->second) || it->second.empty()) {
            it = state.camera_names.erase(it);
        }
        else {
            ++it;
        }
    }
    for(auto it = state.camera_file_prefixes.begin(); it != state.camera_file_prefixes.end();) {
        if(!is_safe_storage_text(it->second) || it->second.empty()) {
            it = state.camera_file_prefixes.erase(it);
        }
        else {
            ++it;
        }
    }
    return state;
}

void save_runtime_state_file(const std::string &path, const RuntimeState &state) {
    const auto state_path = std::filesystem::path(path);
    const auto parent = state_path.parent_path();
    if(!parent.empty()) {
        std::filesystem::create_directories(parent);
    }
    const auto tmp_path = state_path.string() + ".tmp";
    std::ofstream out(tmp_path, std::ios::out | std::ios::trunc);
    if(!out) {
        throw std::runtime_error("cannot write receiver state: " + tmp_path);
    }
    Json::Value root(Json::objectValue);
    root["default_file_prefix"] = state.default_file_prefix;
    root["camera_names"] = Json::Value(Json::objectValue);
    for(const auto &item : state.camera_names) {
        root["camera_names"][item.first] = item.second;
    }
    root["camera_file_prefixes"] = Json::Value(Json::objectValue);
    for(const auto &item : state.camera_file_prefixes) {
        root["camera_file_prefixes"][item.first] = item.second;
    }
    root["camera_announces"] = Json::Value(Json::objectValue);
    for(const auto &item : state.camera_announces) {
        Json::Value announce;
        if(parse_json_object_strict(item.second, announce)) {
            root["camera_announces"][item.first] = std::move(announce);
        }
    }
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "  ";
    out << Json::writeString(builder, root) << '\n';
    out.close();
    if(!out) {
        throw std::runtime_error("cannot finish receiver state: " + tmp_path);
    }
    std::filesystem::rename(tmp_path, state_path);
}

class Logger {
public:
    explicit Logger(std::string directory) : directory_(std::move(directory)) {
        std::filesystem::create_directories(directory_);
        log_path_ = directory_ + "/receiver.log";
        stream_.open(log_path_, std::ios::app);
        if(!stream_) {
            throw std::runtime_error("cannot open receiver log: " + log_path_);
        }
    }

    void info(const std::string &message) { write("INFO", message); }
    void warn(const std::string &message) { write("WARN", message); }
    void error(const std::string &message) { write("ERROR", message); }

private:
    void write(const std::string &level, const std::string &message) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto line = timestamp_text() + " [" + level + "] " + message;
        stream_ << line << '\n';
        const auto now = std::chrono::steady_clock::now();
        if(level != "INFO" || now - last_flush_ >= std::chrono::seconds(1)) {
            stream_.flush();
            last_flush_ = now;
        }
        // receiver.log is the authoritative operational log. Keep stdout free
        // of the duplicate per-packet stream; only exceptional lines reach the
        // service stderr log.
        if(level != "INFO") {
            std::cerr << line << '\n';
        }
    }

    std::string directory_;
    std::string log_path_;
    std::ofstream stream_;
    std::mutex mutex_;
    std::chrono::steady_clock::time_point last_flush_ =
        std::chrono::steady_clock::now();
};

std::optional<size_t> find_marker(const std::vector<uint8_t> &buffer, uint8_t a, uint8_t b, size_t start) {
    if(buffer.size() < 2 || start >= buffer.size() - 1) {
        return std::nullopt;
    }
    for(size_t i = start; i + 1 < buffer.size(); ++i) {
        if(buffer[i] == a && buffer[i + 1] == b) {
            return i;
        }
    }
    return std::nullopt;
}

int add_spawn_closefrom(posix_spawn_file_actions_t *actions);
