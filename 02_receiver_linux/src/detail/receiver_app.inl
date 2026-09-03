class ReceiverApp {
public:
    explicit ReceiverApp(Config config)
        : config_(std::move(config)),
          logger_(config_.log_directory),
          runtime_state_(load_runtime_state(config_.state_path)),
          clock_sync_manager_(config_.clock_sync),
          receiver_discovery_server_(config_.receiver_discovery) {
        logger_.info("receiver state loaded: " + config_.state_path);
    }

    ~ReceiverApp() {
        stop();
    }

    struct SegmentCloseTask {
        std::shared_ptr<CameraState> cam;
        std::string sender_id;
        std::string camera_id;
        std::string announce_json;
        uint64_t recording_end_global_us = 0;
    };

    struct SegmentFinalizeTask {
        std::shared_ptr<CameraState> cam;
        std::unique_ptr<SegmentWriter> segment;
        std::string sender_id;
        std::string camera_id;
        std::string announce_json;
        std::string reason;
        std::string directory;
        uint64_t queued_us = 0;
    };

    struct MediaIngressOwner {
        uint64_t session_id = 0;
        int fd = -1;
        std::string peer_endpoint;
    };

    struct SenderControlTarget {
        std::string sender_id;
        std::string camera_id;
        std::string endpoint;
        uint64_t target_global_us = 0;
    };

    struct RecordingActivation {
        std::vector<SenderControlTarget> keyframe_targets;
        uint64_t request_us = 0;
        bool activated = false;
    };

    struct PhotoCaptureJob {
        MediaPacket packet;
        std::string request_id;
        std::string status_endpoint;
        uint64_t queued_us = 0;
    };

    struct PhotoBurstPathState {
        std::filesystem::path directory;
        std::string filename_stem;
        uint32_t count = 0;
    };

    struct PreviewUdpAssembly {
        std::vector<uint8_t> bytes;
        std::vector<uint8_t> received;
        std::vector<uint32_t> chunk_offsets;
        std::vector<uint16_t> chunk_sizes;
        size_t received_count = 0;
        uint32_t total_size = 0;
        uint16_t chunk_count = 0;
        bool media_udp = false;
        uint64_t first_us = 0;
        uint64_t updated_us = 0;
    };

    struct ClientThread {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };

    void start() {
        bool expected = false;
        if(!started_.compare_exchange_strong(expected, true)) {
            return;
        }
        running_ = true;
        listener_start_failed_ = false;
        status_udp_ready_ = false;
        media_tcp_ready_ = false;
        media_udp_ready_ = false;
        preview_udp_ready_ = false;
        admin_ready_ = false;
        try {
            recover_direct_nas_segments();
            start_photo_capture_worker();
            start_segment_finalize_worker();
            start_decoder_cleanup_worker();
            start_recording_maintenance_worker();
            clock_sync_manager_.set_log_callbacks([this](const std::string &message) { logger_.info(message); },
                                                 [this](const std::string &message) { logger_.warn(message); });
            clock_sync_manager_.start();
            receiver_discovery_server_.set_log_callbacks(
                [this](const std::string &message) { logger_.info(message); },
                [this](const std::string &message) { logger_.warn(message); });
            receiver_discovery_server_.start();
            udp_thread_ = std::thread([this] { udp_loop(); });
            if(config_.media_udp_enabled) {
                media_udp_thread_ = std::thread([this] { media_udp_loop(); });
            }
            if(config_.preview_enabled && config_.preview_udp_enabled) {
                preview_udp_thread_ = std::thread([this] { preview_udp_loop(); });
            }
            tcp_thread_ = std::thread([this] { tcp_loop(); });
            admin_thread_ = std::thread([this] { admin_loop(); });
            const auto ready_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while(!required_listeners_ready() && !listener_start_failed_
                  && std::chrono::steady_clock::now() < ready_deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if(!required_listeners_ready()) {
                throw std::runtime_error(listener_start_failed_ ? "receiver listener startup failed"
                                                                : "receiver listener startup timed out");
            }
        }
        catch(...) {
            stop();
            throw;
        }
        logger_.info("receiver started: media tcp " + config_.media_bind_ip + ":" + std::to_string(config_.media_port) +
                     ", status udp " + config_.status_bind_ip + ":" + std::to_string(config_.status_port) +
                     ", clock sync udp "
                     + (config_.clock_sync.enabled ? config_.clock_sync.bind_ip : std::string("disabled")) + ":" +
                     std::to_string(config_.clock_sync.port) +
                     ", media udp " + (config_.media_udp_enabled ? config_.media_udp_bind_ip : std::string("disabled")) + ":" +
                     std::to_string(config_.media_udp_port) +
                     ", preview udp " +
                     (config_.preview_enabled && config_.preview_udp_enabled ? config_.preview_udp_bind_ip : std::string("disabled")) + ":" +
                     std::to_string(config_.preview_udp_port) + ", admin http " + config_.admin_bind_ip + ":" +
                     std::to_string(config_.admin_port) + ", preview " + (config_.preview_enabled ? "enabled" : "disabled"));
    }

    void stop() {
        if(!started_.exchange(false)) {
            return;
        }
        running_ = false;
        recording_maintenance_cv_.notify_all();
        if(recording_maintenance_thread_.joinable()) {
            recording_maintenance_thread_.join();
        }
        clock_sync_manager_.stop();
        receiver_discovery_server_.stop();
        shutdown_client_sockets();
        if(udp_thread_.joinable()) {
            udp_thread_.join();
        }
        if(media_udp_thread_.joinable()) {
            media_udp_thread_.join();
        }
        if(preview_udp_thread_.joinable()) {
            preview_udp_thread_.join();
        }
        if(tcp_thread_.joinable()) {
            tcp_thread_.join();
        }
        if(admin_thread_.joinable()) {
            admin_thread_.join();
        }
        shutdown_client_sockets();
        join_client_threads();
        stop_photo_capture_worker();
        wait_segment_close_futures();
        std::vector<SegmentCloseTask> close_tasks;
        std::vector<std::shared_ptr<CameraState>> camera_snapshot;
        const uint64_t shutdown_end_global_us = now_us();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            recording_all_ = false;
            recording_all_start_pending_ = false;
            for(auto &item : cameras_) {
                std::lock_guard<std::mutex> preview_lock(item.second->preview_mutex);
                cleanup_rgb_decoder_async(std::move(item.second->rgb_decoder));
                cleanup_rgb_decoder_async(std::move(item.second->main_rgb_decoder));
                item.second->recording_requested = false;
                item.second->recording_start_pending = false;
                set_record_accepting(item.second, false);
                camera_snapshot.push_back(item.second);
                close_tasks.push_back({item.second, item.second->sender_id, item.second->camera_id,
                                       item.second->last_announce_live ? item.second->last_announce_json : "",
                                       shutdown_end_global_us});
            }
        }
        stop_record_workers_sync(camera_snapshot);
        for(auto &task : close_tasks) {
            close_segment_task(task, "receiver stop");
        }
        wait_segment_finalize_idle();
        stop_segment_finalize_worker();
        stop_decoder_cleanup_worker();
        logger_.info("receiver stopped");
    }

    std::string status_json() {
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if(!lock.owns_lock()) {
            std::lock_guard<std::mutex> cache_lock(status_cache_mutex_);
            if(!status_cache_.empty()) {
                return status_cache_;
            }
            return "{\"running\":true,\"status_stale\":true,\"build_commit\":\"" + std::string(GWV3_GIT_COMMIT)
                   + "\",\"build_dirty\":" + std::string(GWV3_GIT_DIRTY != 0 ? "true" : "false")
                   + ",\"build_source_hash\":\"" + std::string(GWV3_RECEIVER_SOURCE_HASH) + "\",\"cameras\":[]}";
        }
        const auto now = now_us();
        refresh_camera_liveness_locked(now);
        const auto recording_start_block = recording_start_block_reason();
        std::error_code recording_space_error;
        const auto recording_space = std::filesystem::space(recording_write_root(config_), recording_space_error);
        const int recording_free_percent = recording_space_error || recording_space.capacity == 0
                                               ? -1
                                               : static_cast<int>(static_cast<long double>(recording_space.available)
                                                                  * 100.0L
                                                                  / static_cast<long double>(recording_space.capacity));
        const bool recording_space_warning =
            recording_free_percent >= 0 && config_.warn_free_disk_percent > 0
            && recording_free_percent < config_.warn_free_disk_percent;
        const bool recording_space_hard_limit =
            recording_space_error || !storage_space_meets_limits(recording_space, config_);
        UdpReassemblyStats media_udp_stats;
        UdpReassemblyStats preview_udp_stats;
        size_t active_media_udp_assemblies = 0;
        size_t active_preview_udp_assemblies = 0;
        {
            std::lock_guard<std::mutex> udp_lock(preview_udp_mutex_);
            media_udp_stats = media_udp_stats_;
            preview_udp_stats = preview_udp_stats_;
            for(const auto &assembly : preview_udp_assemblies_) {
                if(assembly.second.media_udp) {
                    ++active_media_udp_assemblies;
                }
                else {
                    ++active_preview_udp_assemblies;
                }
            }
        }
        std::ostringstream out;
        out << "{";
        out << "\"running\":true,";
        out << "\"build_commit\":\"" << json_escape(GWV3_GIT_COMMIT) << "\",";
        out << "\"build_dirty\":" << (GWV3_GIT_DIRTY != 0 ? "true" : "false") << ',';
        out << "\"build_source_hash\":\"" << GWV3_RECEIVER_SOURCE_HASH << "\",";
        out << "\"receiver_discovery\":{";
        out << "\"enabled\":" << (config_.receiver_discovery.enabled ? "true" : "false") << ',';
        out << "\"healthy\":" << (receiver_discovery_server_.healthy() ? "true" : "false") << ',';
        out << "\"receiver_id\":\"" << json_escape(receiver_discovery_server_.receiver_id()) << "\",";
        out << "\"port\":" << config_.receiver_discovery.port << ',';
        out << "\"requests_received\":" << receiver_discovery_server_.requests_received() << ',';
        out << "\"responses_sent\":" << receiver_discovery_server_.responses_sent() << ',';
        out << "\"last_request_us\":" << receiver_discovery_server_.last_request_us();
        out << "},";
        out << "\"recording_all\":" << (recording_all_ ? "true" : "false") << ',';
        const bool individual_recording_active = std::any_of(
            cameras_.begin(), cameras_.end(), [](const auto &item) {
                return item.second && item.second->recording_requested;
            });
        const std::string recording_state = recording_all_
                                                ? (recording_all_start_pending_ ? "starting" : "recording")
                                                : (individual_recording_active ? "recording"
                                                                               : (recording_faulted_ ? "faulted" : "idle"));
        out << "\"recording_state\":\"" << recording_state << "\",";
        out << "\"recording_start_pending\":" << (recording_all_start_pending_ ? "true" : "false") << ',';
        out << "\"recording_session_id\":" << recording_all_session_id_ << ',';
        out << "\"recording_start_us\":" << recording_all_start_us_ << ',';
        out << "\"recording_faulted\":" << (recording_faulted_ ? "true" : "false") << ',';
        out << "\"recording_fault_session_id\":" << recording_fault_session_id_ << ',';
        out << "\"recording_fault_us\":" << recording_fault_us_ << ',';
        out << "\"recording_fault_camera_key\":\"" << json_escape(recording_fault_camera_key_) << "\",";
        out << "\"recording_fault_reason\":\"" << json_escape(recording_fault_reason_) << "\",";
        out << "\"default_file_prefix\":\"" << json_escape(runtime_state_.default_file_prefix) << "\",";
        out << "\"file_prefix_scope\":\"per_camera\",";
        out << "\"nas_root\":\"" << json_escape(config_.nas_root) << "\",";
        out << "\"recording_storage\":{";
        out << "\"root\":\"" << json_escape(recording_write_root(config_).string()) << "\",";
        out << "\"available\":" << (!recording_space_error ? "true" : "false") << ',';
        out << "\"capacity_bytes\":" << (recording_space_error ? 0 : recording_space.capacity) << ',';
        out << "\"free_bytes\":" << (recording_space_error ? 0 : recording_space.available) << ',';
        out << "\"free_percent\":" << recording_free_percent << ',';
        out << "\"warning\":" << (recording_space_warning ? "true" : "false") << ',';
        out << "\"hard_limit\":" << (recording_space_hard_limit ? "true" : "false") << ',';
        out << "\"warn_free_percent\":" << config_.warn_free_disk_percent << ',';
        out << "\"min_free_percent\":" << config_.min_free_disk_percent << ',';
        out << "\"min_free_bytes\":" << config_.min_free_disk_bytes;
        out << "},";
        out << "\"nas_auto_mount\":{";
        out << "\"enabled\":" << (config_.nas_auto_mount.enabled ? "true" : "false") << ',';
        out << "\"ready\":" << (nas_ready_for_new_recording() ? "true" : "false") << ',';
        out << "\"required_for_new_recording\":"
            << (config_.nas_auto_mount.require_for_new_recording ? "true" : "false") << ',';
        out << "\"status_path\":\"" << json_escape(config_.nas_auto_mount.status_path) << "\"";
        out << "},";
        out << "\"recording_start_ready\":"
            << (recording_start_block.has_value() ? "false" : "true") << ',';
        out << "\"recording_start_block_reason\":\""
            << json_escape(recording_start_block.value_or("")) << "\",";
        out << "\"recording_staging\":{";
        out << "\"enabled\":" << (config_.recording_staging.enabled ? "true" : "false") << ',';
        out << "\"root\":\"" << json_escape(config_.recording_staging.root) << "\",";
        out << "\"defer_player_compatible_finalize\":"
            << (config_.recording_staging.defer_player_compatible_finalize ? "true" : "false") << ',';
        out << "\"rgb_output_mode\":\""
            << json_escape(config_.recording_staging.rgb_output_mode) << "\",";
        out << "\"idle_finalize_ms\":" << config_.recording_staging.idle_finalize_ms << ',';
        out << "\"direct_publish_hidden_directory\":\""
            << json_escape(config_.recording_staging.direct_publish_hidden_directory) << "\"},";
        out << "\"recording_staging_enabled\":" << (config_.recording_staging.enabled ? "true" : "false") << ',';
        out << "\"recording_write_root\":\""
            << json_escape(recording_write_root(config_).string()) << "\",";
        {
            std::lock_guard<std::mutex> photo_lock(photo_capture_mutex_);
            out << "\"photo_capture\":{";
            out << "\"enabled\":" << (config_.photo_capture.enabled ? "true" : "false") << ',';
            out << "\"available\":" << (photo_capture_available_ ? "true" : "false") << ',';
            out << "\"staging_root\":\"" << json_escape(config_.photo_capture.staging_root) << "\",";
            out << "\"nas_subdirectory\":\"" << json_escape(config_.photo_capture.nas_subdirectory) << "\",";
            out << "\"queue_items\":" << photo_capture_queue_.size() << ',';
            out << "\"queue_bytes\":" << photo_capture_queue_bytes_ << ',';
            out << "\"pending_request_ids\":" << photo_capture_pending_ids_.size() << ',';
            out << "\"enqueued\":" << photo_capture_enqueued_.load() << ',';
            out << "\"completed\":" << photo_capture_completed_.load() << ',';
            out << "\"duplicate_requests\":" << photo_capture_duplicate_requests_.load() << ',';
            out << "\"failures\":" << photo_capture_failures_.load() << "},";
        }
        {
            std::lock_guard<std::mutex> uploader_lock(uploader_status_mutex_);
            out << "\"recording_uploader\":"
                << (uploader_status_json_.empty() ? "{\"available\":false}" : uploader_status_json_) << ',';
        }
        const uint64_t finalize_last_completed_us = segment_finalize_last_completed_us_.load();
        const uint64_t uploader_metrics_refreshed_us = uploader_pending_metrics_refreshed_us_.load();
        const bool delivery_pending = segment_finalize_outstanding_status_.load() > 0
                                      || uploader_pending_segments_status_.load() > 0
                                      || (config_.recording_staging.enabled && finalize_last_completed_us > 0
                                          && uploader_metrics_refreshed_us < finalize_last_completed_us);
        out << "\"record_finalize_last_completed_us\":" << finalize_last_completed_us << ',';
        out << "\"recording_uploader_pending_metrics_refreshed_us\":"
            << uploader_metrics_refreshed_us << ',';
        out << "\"recording_delivery_pending\":" << (delivery_pending ? "true" : "false") << ',';
        out << "\"recording_delivery_ready\":"
            << (!recording_all_ && !delivery_pending ? "true" : "false") << ',';
        out << "\"media_port\":" << config_.media_port << ',';
        out << "\"preview_enabled\":" << (config_.preview_enabled ? "true" : "false") << ',';
        out << "\"media_udp_enabled\":" << (config_.media_udp_enabled ? "true" : "false") << ',';
        out << "\"media_udp_port\":" << config_.media_udp_port << ',';
        out << "\"preview_udp_enabled\":" << (config_.preview_enabled && config_.preview_udp_enabled ? "true" : "false") << ',';
        out << "\"preview_udp_port\":" << config_.preview_udp_port << ',';
        out << "\"active_media_clients\":" << active_media_clients_.load() << ',';
        out << "\"active_admin_clients\":" << active_admin_clients_.load() << ',';
        out << "\"media_ingress_superseded_sessions\":" << media_ingress_superseded_sessions_.load() << ',';
        out << "\"media_ingress_stale_packets\":" << media_ingress_stale_packets_.load() << ',';
        out << "\"record_queue_total_bytes\":" << total_record_queue_bytes_.load() << ',';
        out << "\"record_finalize_max_pending_segments\":" << config_.record_finalize_max_pending_segments << ',';
        out << "\"record_finalize_workers\":" << config_.record_finalize_workers << ',';
        out << "\"record_finalize_outstanding_segments\":" << segment_finalize_outstanding_status_.load() << ',';
        out << "\"record_finalize_queued_segments\":" << segment_finalize_queued_status_.load() << ',';
        out << "\"record_finalize_active_segments\":" << segment_finalize_active_status_.load() << ',';
        out << "\"record_finalize_completed_segments\":" << segment_finalize_completed_total_.load() << ',';
        out << "\"record_finalize_failed_segments\":" << segment_finalize_failures_total_.load() << ',';
        out << "\"media_udp_stats\":";
        append_udp_reassembly_stats_json(out, media_udp_stats, active_media_udp_assemblies);
        out << ',';
        out << "\"preview_udp_stats\":";
        append_udp_reassembly_stats_json(out, preview_udp_stats, active_preview_udp_assemblies);
        out << ',';
        out << "\"status_port\":" << config_.status_port << ',';
        out << "\"admin_port\":" << config_.admin_port << ',';
        out << "\"clock_sync_enabled\":" << (config_.clock_sync.enabled ? "true" : "false") << ',';
        out << "\"clock_sync_port\":" << config_.clock_sync.port << ',';
        out << "\"clock_sync\":[";
        const auto clock_models = clock_sync_manager_.models();
        for(size_t i = 0; i < clock_models.size(); ++i) {
            if(i > 0) {
                out << ',';
            }
            const auto &model = clock_models[i].second;
            out << "{";
            out << "\"sender_id\":\"" << json_escape(clock_models[i].first) << "\",";
            out << "\"clock_sync_valid\":" << (model.valid ? "true" : "false") << ',';
            out << "\"clock_report_stale\":" << (model.report_stale ? "true" : "false") << ',';
            out << "\"clock_offset_us\":" << model.offset_us << ',';
            out << "\"clock_delay_us\":" << model.delay_us << ',';
            out << "\"clock_drift_ppm\":" << model.drift_ppm << ',';
            out << "\"clock_last_sync_us\":" << model.last_sync_us << ',';
            out << "\"clock_last_update_receiver_us\":" << model.last_update_receiver_us << ',';
            out << "\"clock_last_probe_receiver_us\":" << model.last_probe_receiver_us << ',';
            out << "\"clock_probe_count\":" << model.probe_count << ',';
            out << "\"clock_samples\":" << model.sample_count;
            out << "}";
        }
        out << "],";
        out << "\"main_preview_camera_key\":\"" << json_escape(main_preview_key_) << "\",";
        out << "\"cameras\":[";
        bool first = true;
        for(const auto &item : cameras_) {
            auto &cam = *item.second;
            std::lock_guard<std::mutex> preview_lock(cam.preview_mutex);
            const auto last_seen = camera_last_seen_us(cam);
            const bool status_live = cam.online && is_recent_us(now, cam.last_status_us, kCameraOnlineTimeoutUs);
            const bool media_live = cam.online && is_recent_us(now, cam.last_media_us, kCameraOnlineTimeoutUs);
            const bool live = media_live;
            const bool preview_enabled = config_.preview_enabled;
            bool rgb_h264_preview_fresh = false;
            uint32_t rgb_h264_preview_width = 0;
            uint32_t rgb_h264_preview_height = 0;
            uint64_t rgb_h264_preview_us = 0;
            {
                std::lock_guard<std::mutex> stream_lock(cam.rgb_preview_stream.mutex);
                rgb_h264_preview_fresh = preview_enabled && media_live && is_recent_us(now, cam.rgb_preview_stream.last_us, kPreviewFreshUs);
                rgb_h264_preview_width = cam.rgb_preview_stream.width;
                rgb_h264_preview_height = cam.rgb_preview_stream.height;
                rgb_h264_preview_us = cam.rgb_preview_stream.last_us;
            }
            const bool rgb_preview_fresh = preview_enabled && media_live && is_recent_us(now, cam.rgb_preview_us, kPreviewFreshUs);
            const bool main_rgb_preview_fresh = preview_enabled && media_live && is_recent_us(now, cam.main_rgb_preview_us, kPreviewFreshUs);
            const bool depth_preview_fresh = preview_enabled && media_live && is_recent_us(now, cam.depth_preview_us, kPreviewFreshUs);
            const bool rgb_thumbnail_preview_available = rgb_preview_fresh && cam.rgb_decoder && cam.rgb_decoder->has_frame();
            const bool rgb_preview_report_available =
                preview_enabled && (rgb_h264_preview_fresh || rgb_thumbnail_preview_available);
            const bool main_rgb_native_preview_available =
                cam.key == main_preview_key_ && main_rgb_preview_fresh && cam.main_rgb_decoder && cam.main_rgb_decoder->has_frame();
            const bool main_rgb_preview_report_available =
                cam.key == main_preview_key_ && (rgb_h264_preview_fresh || main_rgb_native_preview_available || rgb_thumbnail_preview_available);
            const int announce_rgb_width_for_preview = json_int_in_object(cam.last_announce_json, "rgb_profile", "width").value_or(0);
            const int announce_rgb_height_for_preview = json_int_in_object(cam.last_announce_json, "rgb_profile", "height").value_or(0);
            const uint32_t rgb_report_width =
                rgb_h264_preview_fresh ? rgb_h264_preview_width
                                       : (cam.rgb_preview_width > 0 ? cam.rgb_preview_width : static_cast<uint32_t>(announce_rgb_width_for_preview));
            const uint32_t rgb_report_height =
                rgb_h264_preview_fresh ? rgb_h264_preview_height
                                       : (cam.rgb_preview_height > 0 ? cam.rgb_preview_height : static_cast<uint32_t>(announce_rgb_height_for_preview));
            const uint64_t rgb_report_us = rgb_preview_report_available ? (rgb_h264_preview_fresh ? rgb_h264_preview_us : cam.rgb_preview_us) : 0;
            const uint32_t main_rgb_report_width =
                !main_rgb_preview_report_available
                    ? 0
                    : (rgb_h264_preview_fresh ? rgb_h264_preview_width
                                              : (main_rgb_native_preview_available ? cam.main_rgb_preview_width : rgb_report_width));
            const uint32_t main_rgb_report_height =
                !main_rgb_preview_report_available
                    ? 0
                    : (rgb_h264_preview_fresh ? rgb_h264_preview_height
                                              : (main_rgb_native_preview_available ? cam.main_rgb_preview_height : rgb_report_height));
            const uint64_t main_rgb_report_us =
                !main_rgb_preview_report_available
                    ? 0
                    : (rgb_h264_preview_fresh ? rgb_h264_preview_us
                                              : (main_rgb_native_preview_available ? cam.main_rgb_preview_us : cam.rgb_preview_us));
            const auto calibration_json = json_object_field(cam.last_announce_json, "calibration").value_or("");
            const bool cached_calibration_available = json_bool_field(calibration_json, "available").value_or(false);
            const bool calibration_available = cam.last_announce_live && cached_calibration_available;
            const int announce_rgb_width = json_int_in_object(cam.last_announce_json, "rgb_profile", "width").value_or(0);
            const int announce_rgb_height = json_int_in_object(cam.last_announce_json, "rgb_profile", "height").value_or(0);
            const int announce_depth_width = json_int_in_object(cam.last_announce_json, "depth_profile", "width").value_or(0);
            const int announce_depth_height = json_int_in_object(cam.last_announce_json, "depth_profile", "height").value_or(0);
            const auto announce_timestamp_us = json_uint64_field(cam.last_announce_json, "timestamp_us").value_or(0);
            size_t record_queue_packets = 0;
            size_t record_queue_bytes = 0;
            size_t record_queue_peak_packets = 0;
            size_t record_queue_peak_bytes = 0;
            uint64_t record_queue_oldest_age_ms = 0;
            uint64_t record_prequeue_peak_delay_ms = 0;
            uint64_t record_queue_peak_wait_ms = 0;
            uint64_t record_enqueued_packets = 0;
            uint64_t record_dequeued_packets = 0;
            uint64_t record_backpressure_waits = 0;
            uint64_t record_oversize_packets = 0;
            uint64_t record_write_errors = 0;
            uint32_t record_active_writes = 0;
            bool record_worker_started = false;
            bool record_accepting = false;
            bool record_finalizing = false;
            bool record_storage_capacity_failed = false;
            {
                std::lock_guard<std::mutex> record_lock(cam.record_mutex);
                record_queue_packets = cam.record_queue.size();
                record_queue_bytes = cam.record_queue_bytes;
                record_queue_peak_packets = cam.record_queue_peak_packets;
                record_queue_peak_bytes = cam.record_queue_peak_bytes;
                if(!cam.record_queue.empty() && cam.record_queue.front().enqueue_us > 0
                   && now >= cam.record_queue.front().enqueue_us) {
                    record_queue_oldest_age_ms = (now - cam.record_queue.front().enqueue_us) / 1000ull;
                }
                record_prequeue_peak_delay_ms = cam.record_prequeue_peak_delay_us / 1000ull;
                record_queue_peak_wait_ms = cam.record_queue_peak_wait_us / 1000ull;
                record_enqueued_packets = cam.record_enqueued_packets;
                record_dequeued_packets = cam.record_dequeued_packets;
                record_backpressure_waits = cam.record_backpressure_waits;
                record_oversize_packets = cam.record_oversize_packets;
                record_write_errors = cam.record_write_errors;
                record_active_writes = cam.record_active_writes;
                record_worker_started = cam.record_worker_started;
                record_accepting = cam.record_accepting;
                record_finalizing = cam.record_finalizing;
                record_storage_capacity_failed = cam.record_storage_capacity_failed;
            }
            if(!first) {
                out << ',';
            }
            first = false;
            const auto clock_model = clock_sync_manager_.get_model(cam.sender_id);
            const size_t segment_finalize_pending = cam.segment_finalize_pending.load();
            const bool segment_finalize_active = cam.segment_finalize_active.load();
            out << "{";
            out << "\"sender_id\":\"" << json_escape(cam.sender_id) << "\",";
            out << "\"camera_id\":\"" << json_escape(cam.camera_id) << "\",";
            out << "\"sender_build_commit\":\"" << json_escape(cam.sender_build_commit) << "\",";
            out << "\"sender_build_source_hash\":\"" << json_escape(cam.sender_build_source_hash) << "\",";
            out << "\"sender_build_dirty\":" << (cam.sender_build_dirty ? "true" : "false") << ',';
            out << "\"camera_key\":\"" << json_escape(cam.key) << "\",";
            out << "\"camera_name\":\"" << json_escape(cam.camera_name) << "\",";
            out << "\"storage_key\":\"" << json_escape(cam.storage_key()) << "\",";
            out << "\"camera_file_prefix\":\"" << json_escape(cam.camera_file_prefix) << "\",";
            out << "\"sender_source_ip\":\"" << json_escape(socket_endpoint_ip(cam.status_endpoint)) << "\",";
            out << "\"online\":" << (cam.online ? "true" : "false") << ',';
            out << "\"status_live\":" << (status_live ? "true" : "false") << ',';
            out << "\"media_live\":" << (media_live ? "true" : "false") << ',';
            out << "\"live\":" << (live ? "true" : "false") << ',';
            out << "\"recording\":" << ((cam.recording_requested || recording_all_) ? "true" : "false") << ',';
            out << "\"recording_start_pending\":" << (cam.recording_start_pending ? "true" : "false") << ',';
            out << "\"recording_session_id\":" << cam.recording_window.session_id << ',';
            out << "\"recording_start_us\":" << cam.recording_start_us << ',';
            out << "\"recording_window_start_global_us\":" << cam.recording_window.start_global_us << ',';
            out << "\"recording_window_end_global_us\":" << cam.recording_window.end_global_us << ',';
            out << "\"file_prefix\":\"" << json_escape(cam.recording_file_prefix) << "\",";
            out << "\"segment_active\":" << (cam.segment_active ? "true" : "false") << ',';
            out << "\"segment_finalizing\":" << (cam.segment_finalizing ? "true" : "false") << ',';
            out << "\"segment_dir\":\"" << json_escape(cam.segment_dir) << "\",";
            out << "\"segment_start_us\":" << cam.segment_start_us << ',';
            out << "\"global_segment_index\":" << cam.global_segment_index << ',';
            out << "\"segment_window_start_global_us\":" << cam.segment_window_start_global_us << ',';
            out << "\"segment_window_end_global_us\":" << cam.segment_window_end_global_us << ',';
            out << "\"segment_finalize_pending\":" << segment_finalize_pending << ',';
            out << "\"segment_finalize_active\":" << (segment_finalize_active ? "true" : "false") << ',';
            out << "\"segment_finalize_completed\":" << cam.segment_finalize_completed.load() << ',';
            out << "\"segment_finalize_failures\":" << cam.segment_finalize_failures.load() << ',';
            out << "\"segment_finalize_last_duration_ms\":" << cam.segment_finalize_last_duration_ms.load() << ',';
            out << "\"segment_rotation_requested\":" << (cam.segment_rotation_requested.load() ? "true" : "false") << ',';
            out << "\"segment_rotation_keyframe_requested_us\":" << cam.segment_rotation_keyframe_requested_us.load() << ',';
            out << "\"segment_rotation_keyframe_requests\":" << cam.segment_rotation_keyframe_requests.load() << ',';
            out << "\"segment_prestart_depth_drops\":" << cam.segment_prestart_depth_drops.load() << ',';
            out << "\"segment_prestart_rgb_drops\":" << cam.segment_prestart_rgb_drops.load() << ',';
            out << "\"media_idle_finalizations\":" << cam.media_idle_finalizations.load() << ',';
            out << "\"last_media_session_id\":" << cam.last_media_session_id << ',';
            out << "\"rgb_ingress_session_id\":" << cam.rgb_ingress_session_id << ',';
            out << "\"rgb_ingress_waiting_for_idr\":"
                << (cam.rgb_ingress_waiting_for_idr ? "true" : "false") << ',';
            out << "\"rgb_ingress_keyframe_drops\":" << cam.rgb_ingress_keyframe_drops << ',';
            out << "\"rgb_ingress_recoveries\":" << cam.rgb_ingress_recoveries << ',';
            out << "\"rgb_ingress_keyframe_requests\":" << cam.rgb_ingress_keyframe_requests << ',';
            out << "\"sender_rgb_input_fps\":" << cam.sender_rgb_input_fps << ',';
            out << "\"sender_depth_input_fps\":" << cam.sender_depth_input_fps << ',';
            out << "\"sender_rgb_sent_fps\":" << cam.sender_rgb_sent_fps << ',';
            out << "\"sender_depth_sent_fps\":" << cam.sender_depth_sent_fps << ',';
            out << "\"sender_rgb_dropped_frames\":" << cam.sender_rgb_dropped_frames << ',';
            out << "\"sender_depth_dropped_frames\":" << cam.sender_depth_dropped_frames << ',';
            out << "\"sender_rgb_transport_retry_drops\":" << cam.sender_rgb_transport_retry_drops << ',';
            out << "\"sender_rgb_send_failures\":" << cam.sender_rgb_send_failures << ',';
            out << "\"sender_depth_send_failures\":" << cam.sender_depth_send_failures << ',';
            out << "\"sender_publish_warmup_active\":" << (cam.sender_publish_warmup_active ? "true" : "false") << ',';
            out << "\"sender_publish_warmup_drops\":" << cam.sender_publish_warmup_drops << ',';
            out << "\"record_queue_packets\":" << record_queue_packets << ',';
            out << "\"record_queue_bytes\":" << record_queue_bytes << ',';
            out << "\"record_queue_peak_packets\":" << record_queue_peak_packets << ',';
            out << "\"record_queue_peak_bytes\":" << record_queue_peak_bytes << ',';
            out << "\"record_queue_oldest_age_ms\":" << record_queue_oldest_age_ms << ',';
            out << "\"record_prequeue_peak_delay_ms\":" << record_prequeue_peak_delay_ms << ',';
            out << "\"record_queue_peak_wait_ms\":" << record_queue_peak_wait_ms << ',';
            out << "\"record_enqueued_packets\":" << record_enqueued_packets << ',';
            out << "\"record_dequeued_packets\":" << record_dequeued_packets << ',';
            out << "\"record_active_writes\":" << record_active_writes << ',';
            out << "\"record_backpressure_waits\":" << record_backpressure_waits << ',';
            out << "\"record_oversize_packets\":" << record_oversize_packets << ',';
            out << "\"record_write_errors\":" << record_write_errors << ',';
            out << "\"record_worker_started\":" << (record_worker_started ? "true" : "false") << ',';
            out << "\"record_accepting\":" << (record_accepting ? "true" : "false") << ',';
            out << "\"record_finalizing\":" << (record_finalizing ? "true" : "false") << ',';
            out << "\"record_storage_capacity_failed\":"
                << (record_storage_capacity_failed ? "true" : "false") << ',';
            out << "\"last_status_us\":" << cam.last_status_us << ',';
            out << "\"last_media_us\":" << cam.last_media_us << ',';
            out << "\"last_seen_us\":" << last_seen << ',';
            out << "\"status_age_ms\":" << age_ms_or_negative(now, cam.last_status_us) << ',';
            out << "\"media_age_ms\":" << age_ms_or_negative(now, cam.last_media_us) << ',';
            out << "\"clock_sync_valid\":" << (clock_model.valid ? "true" : "false") << ',';
            out << "\"clock_report_stale\":" << (clock_model.report_stale ? "true" : "false") << ',';
            out << "\"clock_offset_us\":" << clock_model.offset_us << ',';
            out << "\"clock_delay_us\":" << clock_model.delay_us << ',';
            out << "\"clock_drift_ppm\":" << clock_model.drift_ppm << ',';
            out << "\"clock_last_sync_us\":" << clock_model.last_sync_us << ',';
            out << "\"clock_last_probe_receiver_us\":" << clock_model.last_probe_receiver_us << ',';
            out << "\"rgb_packets\":" << cam.rgb_packets << ',';
            out << "\"depth_packets\":" << cam.depth_packets << ',';
            out << "\"rgb_bytes\":" << cam.rgb_bytes << ',';
            out << "\"depth_bytes\":" << cam.depth_bytes << ',';
            out << "\"rgb_h264_full_range\":"
                << (rgb_h264_full_range_for_camera(config_, cam.sender_id, cam.camera_id) ? "true" : "false") << ',';
            out << "\"rgb_preview_available\":" << (rgb_preview_report_available ? "true" : "false") << ',';
            out << "\"rgb_h264_preview_available\":" << (rgb_h264_preview_fresh ? "true" : "false") << ',';
            out << "\"rgb_h264_preview_width\":" << rgb_h264_preview_width << ',';
            out << "\"rgb_h264_preview_height\":" << rgb_h264_preview_height << ',';
            out << "\"rgb_h264_preview_us\":" << rgb_h264_preview_us << ',';
            out << "\"rgb_h264_preview_age_ms\":" << age_ms_or_negative(now, rgb_h264_preview_us) << ',';
            out << "\"rgb_jpeg_preview_available\":" << (rgb_thumbnail_preview_available ? "true" : "false") << ',';
            out << "\"rgb_preview_width\":" << rgb_report_width << ',';
            out << "\"rgb_preview_height\":" << rgb_report_height << ',';
            out << "\"rgb_preview_us\":" << rgb_report_us << ',';
            out << "\"rgb_preview_age_ms\":" << age_ms_or_negative(now, rgb_report_us) << ',';
            out << "\"main_rgb_preview_available\":" << (main_rgb_preview_report_available ? "true" : "false") << ',';
            out << "\"main_rgb_jpeg_preview_available\":" << (main_rgb_native_preview_available ? "true" : "false") << ',';
            out << "\"main_rgb_preview_width\":" << main_rgb_report_width << ',';
            out << "\"main_rgb_preview_height\":" << main_rgb_report_height << ',';
            out << "\"main_rgb_preview_us\":" << main_rgb_report_us << ',';
            out << "\"main_rgb_preview_age_ms\":" << age_ms_or_negative(now, main_rgb_report_us) << ',';
            out << "\"depth_preview_available\":" << (depth_preview_fresh && !cam.depth_preview_ppm.empty() ? "true" : "false") << ',';
            out << "\"depth_preview_width\":" << cam.depth_preview_width << ',';
            out << "\"depth_preview_height\":" << cam.depth_preview_height << ',';
            out << "\"depth_preview_us\":" << cam.depth_preview_us << ',';
            out << "\"depth_preview_age_ms\":" << age_ms_or_negative(now, cam.depth_preview_us) << ',';
            out << "\"calibration_available\":" << (calibration_available ? "true" : "false") << ',';
            out << "\"cached_calibration_available\":" << (cached_calibration_available ? "true" : "false") << ',';
            out << "\"announce_live\":" << (cam.last_announce_live ? "true" : "false") << ',';
            out << "\"announce_received_us\":" << cam.last_announce_received_us << ',';
            out << "\"announce_timestamp_us\":" << announce_timestamp_us << ',';
            out << "\"announce_rgb_width\":" << announce_rgb_width << ',';
            out << "\"announce_rgb_height\":" << announce_rgb_height << ',';
            out << "\"announce_depth_width\":" << announce_depth_width << ',';
            out << "\"announce_depth_height\":" << announce_depth_height << ',';
            out << "\"last_error\":\"" << json_escape(cam.last_error) << "\"";
            out << "}";
        }
        out << "]}";
        auto status = out.str();
        {
            std::lock_guard<std::mutex> cache_lock(status_cache_mutex_);
            status_cache_ = status;
        }
        return status;
    }

    std::string config_json() const {
        std::ostringstream out;
        out << "{";
        out << "\"build_commit\":\"" << json_escape(GWV3_GIT_COMMIT) << "\",";
        out << "\"build_dirty\":" << (GWV3_GIT_DIRTY != 0 ? "true" : "false") << ',';
        out << "\"build_source_hash\":\"" << GWV3_RECEIVER_SOURCE_HASH << "\",";
        out << "\"status_bind_ip\":\"" << json_escape(config_.status_bind_ip) << "\",";
        out << "\"status_port\":" << config_.status_port << ',';
        out << "\"media_bind_ip\":\"" << json_escape(config_.media_bind_ip) << "\",";
        out << "\"media_port\":" << config_.media_port << ',';
        out << "\"preview_enabled\":" << (config_.preview_enabled ? "true" : "false") << ',';
        out << "\"media_udp_enabled\":" << (config_.media_udp_enabled ? "true" : "false") << ',';
        out << "\"media_udp_bind_ip\":\"" << json_escape(config_.media_udp_bind_ip) << "\",";
        out << "\"media_udp_port\":" << config_.media_udp_port << ',';
        out << "\"preview_udp_enabled\":" << (config_.preview_udp_enabled ? "true" : "false") << ',';
        out << "\"preview_udp_bind_ip\":\"" << json_escape(config_.preview_udp_bind_ip) << "\",";
        out << "\"preview_udp_port\":" << config_.preview_udp_port << ',';
        out << "\"clock_sync_enabled\":" << (config_.clock_sync.enabled ? "true" : "false") << ',';
        out << "\"clock_sync_bind_ip\":\"" << json_escape(config_.clock_sync.bind_ip) << "\",";
        out << "\"clock_sync_port\":" << config_.clock_sync.port << ',';
        out << "\"clock_sync_model_timeout_ms\":" << config_.clock_sync.model_timeout_ms << ',';
        out << "\"rgb_h264_full_range_camera_keys\":[";
        bool first_full_range_key = true;
        for(const auto &key : config_.rgb_h264_full_range_camera_keys) {
            if(!first_full_range_key) {
                out << ',';
            }
            first_full_range_key = false;
            out << '"' << json_escape(key) << '"';
        }
        out << "],";
        out << "\"admin_bind_ip\":\"" << json_escape(config_.admin_bind_ip) << "\",";
        out << "\"admin_port\":" << config_.admin_port << ',';
        out << "\"nas_root\":\"" << json_escape(config_.nas_root) << "\",";
        out << "\"state_path\":\"" << json_escape(config_.state_path) << "\",";
        out << "\"default_file_prefix\":\"" << json_escape(runtime_state_.default_file_prefix) << "\",";
        out << "\"file_prefix_scope\":\"per_camera\",";
        out << "\"segment_seconds\":" << config_.segment_seconds << ',';
        out << "\"segment_keyframe_lead_ms\":" << config_.segment_keyframe_lead_ms << ',';
        out << "\"recording_start_lead_ms\":" << config_.recording_start_lead_ms << ',';
        out << "\"task_audio_enabled\":" << (config_.task_audio.enabled ? "true" : "false") << ',';
        out << "\"task_audio_finalize_wait_ms\":" << config_.task_audio.finalize_wait_ms << ',';
        out << "\"task_audio_notify_port\":" << config_.task_audio.notify_port << ',';
        out << "\"task_audio_sender_ids\":[";
        bool first_task_audio_sender = true;
        for(const auto &sender_id : config_.task_audio.sender_ids) {
            if(!first_task_audio_sender) {
                out << ',';
            }
            first_task_audio_sender = false;
            out << '"' << json_escape(sender_id) << '"';
        }
        out << "],";
        out << "\"max_payload_mb\":" << (config_.max_payload_bytes / (1024ull * 1024ull)) << ',';
        out << "\"record_queue_max_mb\":" << (config_.record_queue_max_bytes / (1024ull * 1024ull));
        out << ",\"record_queue_total_max_mb\":" << (config_.record_queue_total_max_bytes / (1024ull * 1024ull));
        out << ",\"record_finalize_max_pending_segments\":" << config_.record_finalize_max_pending_segments;
        out << ",\"record_finalize_workers\":" << config_.record_finalize_workers;
        out << "}";
        return out.str();
    }

    std::string effective_file_prefix_locked(const CameraState &cam) const {
        if(recording_all_ && recording_all_has_file_prefix_override_) {
            return recording_all_file_prefix_;
        }
        return cam.camera_file_prefix;
    }

    uint64_t next_recording_session_id_locked() {
        const uint64_t current_us = now_us();
        last_recording_session_id_ = std::max(current_us, last_recording_session_id_ + 1);
        return last_recording_session_id_;
    }

    static bool record_detach_in_progress(const std::shared_ptr<CameraState> &cam) {
        if(!cam) {
            return false;
        }
        std::lock_guard<std::mutex> record_lock(cam->record_mutex);
        return cam->record_finalizing;
    }

    RecordingActivation activate_pending_recordings_locked() {
        RecordingActivation activation;
        const uint64_t lead_us = static_cast<uint64_t>(config_.recording_start_lead_ms) * 1000ull;

        if(recording_all_) {
            const bool detach_pending = std::any_of(cameras_.begin(), cameras_.end(), [](const auto &item) {
                return item.second->recording_requested && record_detach_in_progress(item.second);
            });
            if(detach_pending) {
                recording_all_start_pending_ = true;
                for(auto &item : cameras_) {
                    bool accepting = false;
                    {
                        std::lock_guard<std::mutex> record_lock(item.second->record_mutex);
                        accepting = item.second->record_accepting;
                    }
                    if(item.second->recording_requested
                       && (!accepting || item.second->recording_start_us == 0
                           || item.second->recording_window.session_id == 0)) {
                        item.second->recording_start_pending = true;
                    }
                }
                return activation;
            }

            if(recording_all_start_us_ == 0) {
                recording_all_start_us_ = now_us() + lead_us;
                recording_all_session_id_ = next_recording_session_id_locked();
            }
            recording_all_start_pending_ = false;
            activation.request_us = recording_all_start_us_;
            for(auto &item : cameras_) {
                auto &cam = *item.second;
                if(!cam.recording_requested) {
                    continue;
                }
                bool accepting = false;
                {
                    std::lock_guard<std::mutex> record_lock(cam.record_mutex);
                    accepting = cam.record_accepting;
                }
                // A start-all request may promote one or more sender-scoped
                // recordings. Keep their existing segment timeline intact and
                // only activate cameras which were not already recording.
                if(accepting && !cam.recording_start_pending && cam.recording_start_us != 0
                   && cam.recording_window.session_id != 0) {
                    continue;
                }
                const bool newly_activated = !accepting || cam.recording_start_pending || cam.recording_start_us == 0;
                cam.recording_start_us = recording_all_start_us_;
                cam.recording_window = {recording_all_session_id_, recording_all_start_us_, 0};
                cam.recording_start_pending = false;
                if(cam.recording_file_prefix.empty()) {
                    cam.recording_file_prefix = effective_file_prefix_locked(cam);
                }
                set_record_accepting(item.second, true);
                if(newly_activated && cam.online && !cam.status_endpoint.empty()) {
                    activation.keyframe_targets.push_back({cam.sender_id, cam.camera_id, cam.status_endpoint});
                }
            }
            activation.activated = true;
            return activation;
        }

        for(auto &item : cameras_) {
            auto &cam = *item.second;
            if(!cam.recording_requested || record_detach_in_progress(item.second)) {
                continue;
            }
            bool accepting = false;
            {
                std::lock_guard<std::mutex> record_lock(cam.record_mutex);
                accepting = cam.record_accepting;
            }
            if(accepting) {
                continue;
            }
            if(cam.recording_start_us == 0 || cam.recording_window.session_id == 0) {
                cam.recording_start_us = now_us() + lead_us;
                cam.recording_window = {next_recording_session_id_locked(), cam.recording_start_us, 0};
            }
            cam.recording_start_pending = false;
            set_record_accepting(item.second, true);
            if(cam.online && !cam.status_endpoint.empty()) {
                activation.keyframe_targets.push_back({cam.sender_id, cam.camera_id, cam.status_endpoint});
            }
            activation.request_us = cam.recording_start_us;
            activation.activated = true;
        }
        return activation;
    }

    void send_force_rgb_keyframe_controls(const std::vector<SenderControlTarget> &targets,
                                          const std::string &reason,
                                          uint64_t request_us,
                                          uint64_t target_global_us = 0) {
        for(const auto &target : targets) {
            if(target.endpoint.empty()) {
                continue;
            }
            std::ostringstream payload;
            payload << "{\"message_type\":\"control\","
                    << "\"control\":\"force_rgb_keyframe\","
                    << "\"sender_id\":\"" << json_escape(target.sender_id) << "\","
                    << "\"camera_id\":\"" << json_escape(target.camera_id) << "\","
                    << "\"reason\":\"" << json_escape(reason) << "\","
                    << "\"request_us\":" << request_us;
            const uint64_t effective_target_global_us =
                target.target_global_us > 0 ? target.target_global_us : target_global_us;
            if(effective_target_global_us > 0) {
                payload << ",\"target_global_us\":" << effective_target_global_us;
            }
            payload << '}';
            if(send_udp_text_to_endpoint(target.endpoint, payload.str())) {
                logger_.info("force_rgb_keyframe control sent sender=" + target.sender_id + " camera=" + target.camera_id
                             + " endpoint=" + target.endpoint + " reason=" + reason
                             + (effective_target_global_us > 0
                                    ? " target_global_us=" + std::to_string(effective_target_global_us)
                                    : ""));
            }
            else {
                logger_.warn("force_rgb_keyframe control send failed sender=" + target.sender_id + " camera=" + target.camera_id
                             + " endpoint=" + target.endpoint);
            }
        }
    }

    std::optional<SenderControlTarget> maybe_web_rgb_preview_control_target_locked(CameraState &cam, uint64_t now) {
        const bool requested = is_recent_us(now, cam.rgb_stream_requested_until_us, 0)
                               || is_recent_us(now, cam.rgb_preview_requested_until_us, 0);
        if(!requested || cam.status_endpoint.empty()) {
            return std::nullopt;
        }
        if(cam.last_web_rgb_preview_control_us != 0 && now < cam.last_web_rgb_preview_control_us + kWebRgbPreviewControlIntervalUs) {
            return std::nullopt;
        }
        cam.last_web_rgb_preview_control_us = now;
        return SenderControlTarget{cam.sender_id, cam.camera_id, cam.status_endpoint};
    }

    std::optional<SenderControlTarget> maybe_web_rgb_preview_keyframe_target_locked(CameraState &cam, uint64_t now) {
        if(cam.status_endpoint.empty()) {
            return std::nullopt;
        }
        if(cam.last_web_rgb_preview_keyframe_us != 0 && now < cam.last_web_rgb_preview_keyframe_us + kWebRgbPreviewKeyframeIntervalUs) {
            return std::nullopt;
        }
        cam.last_web_rgb_preview_keyframe_us = now;
        return SenderControlTarget{cam.sender_id, cam.camera_id, cam.status_endpoint};
    }

    void send_web_rgb_preview_controls(const std::vector<SenderControlTarget> &targets, uint64_t request_us) {
        for(const auto &target : targets) {
            if(target.endpoint.empty()) {
                continue;
            }
            std::ostringstream payload;
            payload << "{\"message_type\":\"control\","
                    << "\"control\":\"set_web_rgb_preview_active\","
                    << "\"sender_id\":\"" << json_escape(target.sender_id) << "\","
                    << "\"camera_id\":\"" << json_escape(target.camera_id) << "\","
                    << "\"active\":true,"
                    << "\"lease_ms\":" << kWebRgbPreviewControlLeaseMs << ','
                    << "\"request_us\":" << request_us << "}";
            if(!send_udp_text_to_endpoint(target.endpoint, payload.str())) {
                logger_.warn("web rgb preview control send failed sender=" + target.sender_id + " camera=" + target.camera_id
                             + " endpoint=" + target.endpoint);
            }
        }
    }

    bool rotate_record_segment_async(const std::shared_ptr<CameraState> &cam,
                                     const RecordJob &job,
                                     const MediaPacket &packet,
                                     bool allow_timed_rotation) {
        if(!cam->segment || !cam->segment->active()) {
            return false;
        }
        const bool profile_changed = cam->segment->stream_profile_changed(packet);
        const bool timed_rotation = allow_timed_rotation
                                    && cam->segment->should_rotate_for_timestamp(packet.global_timestamp_us);
        if(!profile_changed && !timed_rotation) {
            return false;
        }
        if(timed_rotation && !profile_changed
           && (packet.stream_type != StreamType::rgb || !h264_payload_can_start_segment(packet.payload))) {
            return false;
        }

        if(!reserve_segment_finalize_slot(profile_changed)) {
            bool should_log = false;
            {
                std::lock_guard<std::mutex> record_lock(cam->record_mutex);
                const uint64_t current_us = now_us();
                should_log = cam->last_finalize_queue_full_log_us == 0
                             || current_us - cam->last_finalize_queue_full_log_us >= kRecordQueueWarnIntervalUs;
                if(should_log) {
                    cam->last_finalize_queue_full_log_us = current_us;
                }
            }
            if(should_log) {
                logger_.warn("segment rotation deferred because finalization queue is full camera=" + cam->key
                             + " outstanding=" + std::to_string(segment_finalize_outstanding_status_.load())
                             + " max=" + std::to_string(config_.record_finalize_max_pending_segments));
            }
            return false;
        }

        auto next_segment = std::make_unique<SegmentWriter>();
        try {
            next_segment->start(config_, job.sender_id, job.camera_id, job.camera_name, job.storage_key,
                                job.file_prefix, job.announce_json, job.recording_window,
                                packet.global_timestamp_us, logger_);
        }
        catch(...) {
            release_segment_finalize_slot();
            throw;
        }

        auto previous_segment = std::move(cam->segment);
        const std::string previous_directory = previous_segment->directory();
        previous_segment->mark_end_us(now_us());
        cam->segment = std::move(next_segment);
        cam->segment_rotation_requested.store(false);
        cam->segment_rotation_keyframe_requested_us.store(0);

        SegmentFinalizeTask task;
        task.cam = cam;
        task.segment = std::move(previous_segment);
        task.sender_id = job.sender_id;
        task.camera_id = job.camera_id;
        task.announce_json = job.announce_json;
        task.reason = profile_changed ? "media_profile_changed" : "segment_duration_elapsed";
        task.directory = previous_directory;
        if(!enqueue_reserved_segment_finalize(std::move(task))) {
            throw std::runtime_error("cannot enqueue detached segment finalization: " + previous_directory);
        }
        logger_.info("recording segment rotated asynchronously camera=" + cam->key + " old=" + previous_directory
                     + " new=" + cam->segment->directory() + " reason="
                     + (profile_changed ? "media_profile_changed" : "segment_duration_elapsed"));
        return true;
    }

    void write_record_job(const std::shared_ptr<CameraState> &cam, RecordJob job) {
        if(!job.packet) {
            return;
        }
        const MediaPacket &queued_packet = *job.packet;
        const MediaPacket *record_packet = &queued_packet;
        std::optional<MediaPacket> decoded_depth_packet;
        if(queued_packet.stream_type == StreamType::depth_raw && queued_packet.codec_or_compression != "none") {
            try {
                decoded_depth_packet = normalized_depth_packet(queued_packet);
                record_packet = &*decoded_depth_packet;
            }
            catch(const std::exception &e) {
                bool should_log = false;
                {
                    std::lock_guard<std::mutex> record_lock(cam->record_mutex);
                    cam->record_write_errors++;
                    const uint64_t current_us = now_us();
                    should_log = cam->last_record_write_error_log_us == 0
                                 || current_us - cam->last_record_write_error_log_us >= kRecordQueueWarnIntervalUs;
                    if(should_log) {
                        cam->last_record_write_error_log_us = current_us;
                    }
                }
                if(should_log) {
                    logger_.warn(std::string("depth record packet ignored camera=") + cam->key + " frame="
                                 + std::to_string(queued_packet.frame_id) + ": " + e.what());
                }
                return;
            }
        }

        bool dropped_before_segment_start = false;
        uint64_t prestart_drops = 0;
        {
            std::lock_guard<std::mutex> segment_lock(cam->segment_mutex);
            if(cam->segment && !cam->segment->active() && camera_announce_expects_rgb(job.announce_json)) {
                const bool before_recording_window = job.recording_window.start_global_us > 0
                                                     && queued_packet.global_timestamp_us
                                                            < job.recording_window.start_global_us;
                if(queued_packet.stream_type == StreamType::depth_raw) {
                    prestart_drops = cam->segment_prestart_depth_drops.fetch_add(1) + 1;
                    dropped_before_segment_start = true;
                }
                else if(queued_packet.stream_type == StreamType::rgb
                        && (before_recording_window
                            || !h264_payload_can_start_segment(queued_packet.payload))) {
                    prestart_drops = cam->segment_prestart_rgb_drops.fetch_add(1) + 1;
                    dropped_before_segment_start = true;
                }
            }
        }
        if(dropped_before_segment_start) {
            if(prestart_drops == 1) {
                logger_.info("recording waiting for decodable RGB segment start camera=" + cam->key
                             + " stream=" + std::string(stream_type_name(queued_packet.stream_type))
                             + "; unpaired prestart packets are ignored");
            }
            return;
        }

        bool allow_segment_rotate = true;
        {
            std::lock_guard<std::mutex> record_lock(cam->record_mutex);
            allow_segment_rotate = !cam->record_finalizing;
        }

        try {
            bool segment_active = false;
            std::string segment_dir;
            uint64_t segment_start_us = 0;
            uint64_t global_segment_index = 0;
            uint64_t segment_window_start_global_us = 0;
            uint64_t segment_window_end_global_us = 0;
            {
                std::lock_guard<std::mutex> segment_lock(cam->segment_mutex);
                rotate_record_segment_async(cam, job, *record_packet, allow_segment_rotate);
                cam->segment->write_packet(config_, *record_packet, job.sender_id, job.camera_id, job.camera_name, job.storage_key,
                                           job.file_prefix, job.announce_json, job.recording_window, logger_, false);
                segment_active = cam->segment->active();
                segment_dir = cam->segment->directory();
                segment_start_us = cam->segment->start_us();
                global_segment_index = cam->segment->segment_index();
                segment_window_start_global_us = cam->segment->segment_window_start_global_us();
                segment_window_end_global_us = cam->segment->segment_window_end_global_us();
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                cam->segment_active = segment_active;
                cam->segment_dir = std::move(segment_dir);
                cam->segment_start_us = segment_start_us;
                cam->global_segment_index = global_segment_index;
                cam->segment_window_start_global_us = segment_window_start_global_us;
                cam->segment_window_end_global_us = segment_window_end_global_us;
            }
        }
        catch(const std::exception &e) {
            const std::string write_error = e.what();
            const bool storage_capacity_failure =
                write_error.find("free space") != std::string::npos
                || write_error.find("storage previously failed") != std::string::npos;
            bool should_log = false;
            {
                std::lock_guard<std::mutex> record_lock(cam->record_mutex);
                cam->record_write_errors++;
                cam->record_accepting = false;
                cam->record_storage_capacity_failed =
                    cam->record_storage_capacity_failed || storage_capacity_failure;
                cam->record_generation++;
                const size_t discarded_queue_bytes = cam->record_queue_bytes;
                cam->record_queue.clear();
                cam->record_queue_bytes = 0;
                if(discarded_queue_bytes > 0) {
                    const size_t total_before = total_record_queue_bytes_.fetch_sub(discarded_queue_bytes);
                    if(total_before < discarded_queue_bytes) {
                        total_record_queue_bytes_.store(0);
                    }
                }
                const uint64_t current_us = now_us();
                should_log = cam->last_record_write_error_log_us == 0
                             || current_us - cam->last_record_write_error_log_us >= kRecordQueueWarnIntervalUs;
                if(should_log) {
                    cam->last_record_write_error_log_us = current_us;
                }
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                cam->last_error = std::string("recording_write_failed: ") + write_error;
            }
            cam->record_cv.notify_all();
            if(should_log) {
                logger_.warn(std::string("record packet write failed; recording input paused camera=") + cam->key
                             + " frame=" + std::to_string(queued_packet.frame_id) + ": " + e.what());
            }
            if(storage_capacity_failure) {
                abort_recording_after_storage_failure(cam->key, write_error);
            }
        }
    }

    void record_worker_loop(std::shared_ptr<CameraState> cam) {
        logger_.info("record queue worker started: " + cam->key);
        for(;;) {
            RecordJob job;
            {
                std::unique_lock<std::mutex> record_lock(cam->record_mutex);
                cam->record_cv.wait(record_lock, [&] { return cam->record_worker_stop || !cam->record_queue.empty(); });
                if(cam->record_queue.empty()) {
                    if(cam->record_worker_stop) {
                        break;
                    }
                    continue;
                }
                job = std::move(cam->record_queue.front());
                cam->record_queue.pop_front();
                if(job.queue_bytes <= cam->record_queue_bytes) {
                    cam->record_queue_bytes -= job.queue_bytes;
                }
                else {
                    cam->record_queue_bytes = 0;
                }
                const size_t total_before = total_record_queue_bytes_.fetch_sub(job.queue_bytes);
                if(total_before < job.queue_bytes) {
                    total_record_queue_bytes_.store(0);
                }
                cam->record_dequeued_packets++;
                cam->record_active_writes++;
                const uint64_t dequeue_us = now_us();
                if(job.enqueue_us > 0 && dequeue_us >= job.enqueue_us) {
                    cam->record_queue_peak_wait_us =
                        std::max(cam->record_queue_peak_wait_us, dequeue_us - job.enqueue_us);
                }
            }
            cam->record_cv.notify_all();

            write_record_job(cam, std::move(job));

            {
                std::lock_guard<std::mutex> record_lock(cam->record_mutex);
                if(cam->record_active_writes > 0) {
                    cam->record_active_writes--;
                }
            }
            cam->record_cv.notify_all();
        }
        logger_.info("record queue worker stopped: " + cam->key);
    }

    void start_record_worker_if_needed(const std::shared_ptr<CameraState> &cam) {
        std::lock_guard<std::mutex> record_lock(cam->record_mutex);
        if(cam->record_worker_started) {
            return;
        }
        cam->record_worker_stop = false;
        try {
            cam->record_worker = std::thread([this, cam] { record_worker_loop(cam); });
            cam->record_worker_started = true;
        }
        catch(...) {
            cam->record_worker_stop = false;
            cam->record_worker_started = false;
            throw;
        }
    }

    static void set_record_accepting(const std::shared_ptr<CameraState> &cam, bool accepting) {
        if(!cam) {
            return;
        }
        bool changed = false;
        {
            std::lock_guard<std::mutex> record_lock(cam->record_mutex);
            if(cam->record_accepting != accepting) {
                cam->record_accepting = accepting;
                cam->record_generation++;
                changed = true;
            }
        }
        if(changed) {
            cam->record_cv.notify_all();
        }
    }

    static void reset_record_session_metrics_locked(CameraState &cam) {
        cam.record_queue_peak_bytes = cam.record_queue_bytes;
        cam.record_queue_peak_packets = cam.record_queue.size();
        cam.record_prequeue_peak_delay_us = 0;
        cam.record_queue_peak_wait_us = 0;
        cam.record_enqueued_packets = 0;
        cam.record_dequeued_packets = 0;
        cam.record_backpressure_waits = 0;
        cam.record_oversize_packets = 0;
        cam.record_write_errors = 0;
    }

    static void set_record_finalizing(const std::shared_ptr<CameraState> &cam, bool finalizing) {
        if(!cam) {
            return;
        }
        {
            std::lock_guard<std::mutex> record_lock(cam->record_mutex);
            cam->record_finalizing = finalizing;
            if(!finalizing) {
                return;
            }
        }
        cam->record_cv.notify_all();
    }

    bool enqueue_record_job(const std::shared_ptr<CameraState> &cam, RecordJob job) {
        if(!cam || !job.packet || job.packet->stream_type == StreamType::rgb_preview) {
            return false;
        }
        start_record_worker_if_needed(cam);
        job.queue_bytes = record_packet_queue_bytes(*job.packet);
        job.enqueue_us = now_us();
        const uint64_t prequeue_delay_us = job.packet->receiver_receive_timestamp_us > 0
                                               && job.enqueue_us >= job.packet->receiver_receive_timestamp_us
                                           ? job.enqueue_us - job.packet->receiver_receive_timestamp_us
                                           : 0;
        const size_t max_bytes = std::max<size_t>(1, config_.record_queue_max_bytes);
        std::unique_lock<std::mutex> record_lock(cam->record_mutex);
        if(job.queue_bytes > max_bytes) {
            cam->record_oversize_packets++;
        }
        bool total_reserved = false;
        while(!cam->record_worker_stop) {
            if(!media_ingress_session_is_current(job.media_ingress_key, job.media_session_id)) {
                media_ingress_stale_packets_.fetch_add(1);
                return false;
            }
            if(!cam->record_accepting || cam->record_generation != job.record_generation) {
                return false;
            }
            const bool camera_has_room = cam->record_queue.empty()
                                         || (cam->record_queue_bytes <= max_bytes
                                             && job.queue_bytes <= max_bytes - cam->record_queue_bytes);
            if(camera_has_room) {
                size_t total = total_record_queue_bytes_.load();
                while(total <= config_.record_queue_total_max_bytes
                      && job.queue_bytes <= config_.record_queue_total_max_bytes - total) {
                    if(total_record_queue_bytes_.compare_exchange_weak(total, total + job.queue_bytes)) {
                        total_reserved = true;
                        break;
                    }
                }
                if(total_reserved) {
                    break;
                }
            }
            cam->record_backpressure_waits++;
            cam->record_cv.wait_for(record_lock, std::chrono::milliseconds(100));
        }
        std::unique_lock<std::mutex> ingress_lock;
        bool current_session = true;
        if(job.media_session_id != 0) {
            ingress_lock = std::unique_lock<std::mutex>(media_ingress_mutex_);
            const auto owner = media_ingress_owners_.find(job.media_ingress_key);
            current_session = owner != media_ingress_owners_.end() && owner->second.session_id == job.media_session_id;
        }
        if(cam->record_worker_stop || !cam->record_accepting || cam->record_generation != job.record_generation
           || !current_session) {
            if(total_reserved) {
                total_record_queue_bytes_.fetch_sub(job.queue_bytes);
            }
            if(!current_session) {
                media_ingress_stale_packets_.fetch_add(1);
            }
            return false;
        }
        const size_t queue_bytes = job.queue_bytes;
        try {
            cam->record_queue_bytes += queue_bytes;
            cam->record_queue.push_back(std::move(job));
        }
        catch(...) {
            cam->record_queue_bytes -= queue_bytes;
            total_record_queue_bytes_.fetch_sub(queue_bytes);
            throw;
        }
        cam->record_enqueued_packets++;
        cam->record_prequeue_peak_delay_us = std::max(cam->record_prequeue_peak_delay_us, prequeue_delay_us);
        cam->record_queue_peak_bytes = std::max(cam->record_queue_peak_bytes, cam->record_queue_bytes);
        cam->record_queue_peak_packets = std::max(cam->record_queue_peak_packets, cam->record_queue.size());
        record_lock.unlock();
        cam->record_cv.notify_one();
        return true;
    }

    void start_segment_finalize_worker() {
        {
            std::lock_guard<std::mutex> lock(segment_finalize_mutex_);
            if(segment_finalize_worker_running_) {
                return;
            }
            segment_finalize_worker_stop_ = false;
            segment_finalize_active_routes_.clear();
            segment_finalize_active_status_.store(0);
            segment_finalize_worker_running_ = true;
        }
        try {
            segment_finalize_workers_.reserve(config_.record_finalize_workers);
            for(size_t index = 0; index < config_.record_finalize_workers; ++index) {
                segment_finalize_workers_.emplace_back([this, index] { segment_finalize_worker_loop(index); });
            }
        }
        catch(...) {
            {
                std::lock_guard<std::mutex> lock(segment_finalize_mutex_);
                segment_finalize_worker_stop_ = true;
            }
            segment_finalize_cv_.notify_all();
            for(auto &worker : segment_finalize_workers_) {
                if(worker.joinable()) {
                    worker.join();
                }
            }
            segment_finalize_workers_.clear();
            {
                std::lock_guard<std::mutex> lock(segment_finalize_mutex_);
                segment_finalize_worker_running_ = false;
            }
            throw;
        }
    }

    bool reserve_segment_finalize_slot(bool wait_for_slot) {
        std::unique_lock<std::mutex> lock(segment_finalize_mutex_);
        const auto available = [this] {
            return segment_finalize_worker_stop_
                   || segment_finalize_outstanding_ < config_.record_finalize_max_pending_segments;
        };
        if(wait_for_slot) {
            segment_finalize_cv_.wait(lock, available);
        }
        else if(!available()) {
            return false;
        }
        if(segment_finalize_worker_stop_ || !segment_finalize_worker_running_) {
            return false;
        }
        ++segment_finalize_outstanding_;
        segment_finalize_outstanding_status_.store(segment_finalize_outstanding_);
        return true;
    }

    void release_segment_finalize_slot() {
        {
            std::lock_guard<std::mutex> lock(segment_finalize_mutex_);
            if(segment_finalize_outstanding_ > 0) {
                --segment_finalize_outstanding_;
            }
            segment_finalize_outstanding_status_.store(segment_finalize_outstanding_);
        }
        segment_finalize_cv_.notify_all();
    }

    bool enqueue_reserved_segment_finalize(SegmentFinalizeTask &&task) {
        task.queued_us = now_us();
        auto cam = task.cam;
        try {
            std::lock_guard<std::mutex> lock(segment_finalize_mutex_);
            segment_finalize_queue_.push_back(std::move(task));
            if(cam) {
                cam->segment_finalize_pending.fetch_add(1);
            }
            segment_finalize_queued_status_.store(segment_finalize_queue_.size());
        }
        catch(const std::exception &e) {
            logger_.error(std::string("cannot enqueue segment finalization: ") + e.what());
            release_segment_finalize_slot();
            return false;
        }
        segment_finalize_cv_.notify_one();
        return true;
    }

    void complete_segment_finalize_task(SegmentFinalizeTask &task, bool success, uint64_t duration_ms) {
        size_t pending_after = 0;
        if(task.cam) {
            task.cam->segment_finalize_active.store(false);
            task.cam->segment_finalize_last_duration_ms.store(duration_ms);
            if(success) {
                task.cam->segment_finalize_completed.fetch_add(1);
            }
            else {
                task.cam->segment_finalize_failures.fetch_add(1);
            }
            const size_t pending_before = task.cam->segment_finalize_pending.fetch_sub(1);
            pending_after = pending_before > 0 ? pending_before - 1 : 0;
            if(pending_before == 0) {
                task.cam->segment_finalize_pending.store(0);
            }
        }
        if(success) {
            segment_finalize_completed_total_.fetch_add(1);
            segment_finalize_last_completed_us_.store(now_us());
        }
        else {
            segment_finalize_failures_total_.fetch_add(1);
        }

        if(task.cam && !success) {
            std::lock_guard<std::mutex> lock(mutex_);
            task.cam->last_error = "recording_finalize_failed: " + task.directory;
        }

        {
            std::lock_guard<std::mutex> lock(segment_finalize_mutex_);
            if(segment_finalize_outstanding_ > 0) {
                --segment_finalize_outstanding_;
            }
            segment_finalize_outstanding_status_.store(segment_finalize_outstanding_);
            const std::string route = task.cam ? task.cam->key : camera_key(task.sender_id, task.camera_id);
            segment_finalize_active_routes_.erase(route);
            segment_finalize_active_status_.store(segment_finalize_active_routes_.size());
        }
        segment_finalize_cv_.notify_all();
        if(task.cam && pending_after == 0) {
            task.cam->record_cv.notify_all();
        }
    }

    void segment_finalize_worker_loop(size_t worker_index) {
        logger_.info("segment finalizer worker started worker=" + std::to_string(worker_index)
                     + " concurrency=" + std::to_string(config_.record_finalize_workers)
                     + " max_pending=" + std::to_string(config_.record_finalize_max_pending_segments));
        for(;;) {
            SegmentFinalizeTask task;
            {
                std::unique_lock<std::mutex> lock(segment_finalize_mutex_);
                segment_finalize_cv_.wait(lock, [this] {
                    if(segment_finalize_worker_stop_ && segment_finalize_queue_.empty()) {
                        return true;
                    }
                    return std::any_of(segment_finalize_queue_.begin(), segment_finalize_queue_.end(), [this](const auto &queued) {
                        const std::string route = queued.cam ? queued.cam->key
                                                             : camera_key(queued.sender_id, queued.camera_id);
                        return segment_finalize_active_routes_.count(route) == 0;
                    });
                });
                if(segment_finalize_queue_.empty()) {
                    if(segment_finalize_worker_stop_) {
                        break;
                    }
                    continue;
                }
                const auto available = std::find_if(
                    segment_finalize_queue_.begin(),
                    segment_finalize_queue_.end(),
                    [this](const auto &queued) {
                        const std::string route = queued.cam ? queued.cam->key
                                                             : camera_key(queued.sender_id, queued.camera_id);
                        return segment_finalize_active_routes_.count(route) == 0;
                    });
                if(available == segment_finalize_queue_.end()) {
                    continue;
                }
                task = std::move(*available);
                segment_finalize_queue_.erase(available);
                segment_finalize_queued_status_.store(segment_finalize_queue_.size());
                const std::string route = task.cam ? task.cam->key : camera_key(task.sender_id, task.camera_id);
                segment_finalize_active_routes_.insert(route);
                segment_finalize_active_status_.store(segment_finalize_active_routes_.size());
                if(task.cam) {
                    task.cam->segment_finalize_active.store(true);
                }
            }

            const uint64_t started_us = now_us();
            const uint64_t queue_wait_ms = task.queued_us > 0 && started_us >= task.queued_us
                                               ? (started_us - task.queued_us) / 1000ull
                                               : 0;
            logger_.info("segment finalization started camera="
                         + (task.cam ? task.cam->key : camera_key(task.sender_id, task.camera_id))
                         + " directory=" + task.directory + " reason=" + task.reason
                         + " queue_wait_ms=" + std::to_string(queue_wait_ms));
            bool success = true;
            try {
                if(task.segment) {
                    task.segment->close(config_, task.sender_id, task.camera_id, task.announce_json, logger_);
                }
            }
            catch(const std::exception &e) {
                success = false;
                logger_.warn("segment finalization failed camera="
                             + (task.cam ? task.cam->key : camera_key(task.sender_id, task.camera_id))
                             + " directory=" + task.directory + ": " + e.what());
            }
            const uint64_t completed_us = now_us();
            const uint64_t duration_ms = completed_us >= started_us ? (completed_us - started_us) / 1000ull : 0;
            logger_.info("segment finalization completed camera="
                         + (task.cam ? task.cam->key : camera_key(task.sender_id, task.camera_id))
                         + " directory=" + task.directory + " success=" + (success ? "true" : "false")
                         + " duration_ms=" + std::to_string(duration_ms));
            task.segment.reset();
            complete_segment_finalize_task(task, success, duration_ms);
        }
        logger_.info("segment finalizer worker stopped worker=" + std::to_string(worker_index));
    }

    void wait_segment_finalize_idle() {
        std::unique_lock<std::mutex> lock(segment_finalize_mutex_);
        segment_finalize_cv_.wait(lock, [this] { return segment_finalize_outstanding_ == 0; });
    }

    void stop_segment_finalize_worker() {
        wait_segment_finalize_idle();
        {
            std::lock_guard<std::mutex> lock(segment_finalize_mutex_);
            segment_finalize_worker_stop_ = true;
        }
        segment_finalize_cv_.notify_all();
        for(auto &worker : segment_finalize_workers_) {
            if(worker.joinable()) {
                worker.join();
            }
        }
        segment_finalize_workers_.clear();
        std::lock_guard<std::mutex> lock(segment_finalize_mutex_);
        segment_finalize_worker_running_ = false;
        segment_finalize_active_routes_.clear();
        segment_finalize_active_status_.store(0);
    }

    void refresh_recording_uploader_status() {
        if(!config_.recording_staging.enabled) {
            return;
        }
        const auto status_path = std::filesystem::path(config_.recording_staging.root) / ".gwv3_uploader_status.json";
        std::ifstream input(status_path);
        std::string status;
        if(input) {
            const std::string raw((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
            Json::Value root;
            if(parse_json_object_strict(raw, root)) {
                const uint64_t updated_us = json_uint64_value(root, "updated_us").value_or(0);
                uploader_pending_metrics_refreshed_us_.store(
                    json_uint64_value(root, "pending_metrics_refreshed_us").value_or(updated_us));
                uploader_pending_segments_status_.store(
                    json_uint64_value(root, "pending_segments").value_or(0));
                Json::StreamWriterBuilder builder;
                builder["indentation"] = "";
                status = Json::writeString(builder, root);
            }
        }
        if(status.empty()) {
            status = "{\"available\":false,\"last_error\":\"uploader status unavailable\"}";
        }
        std::lock_guard<std::mutex> lock(uploader_status_mutex_);
        uploader_status_json_ = std::move(status);
    }

    void recording_maintenance_loop() {
        logger_.info("recording maintenance worker started idle_finalize_ms="
                     + std::to_string(config_.recording_staging.idle_finalize_ms));
        auto next_uploader_status_refresh = std::chrono::steady_clock::time_point::min();
        while(running_) {
            const uint64_t current_us = now_us();
            std::vector<std::shared_ptr<CameraState>> camera_snapshot;
            std::vector<SegmentCloseTask> idle_close_tasks;
            std::vector<SegmentCloseTask> storage_close_tasks;
            std::vector<std::pair<std::shared_ptr<CameraState>, uint64_t>> rotation_keyframe_cameras;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                refresh_camera_liveness_locked(current_us);
                camera_snapshot.reserve(cameras_.size());
                for(auto &item : cameras_) {
                    auto &cam = item.second;
                    camera_snapshot.push_back(cam);
                    const uint64_t idle_limit_us = static_cast<uint64_t>(config_.recording_staging.idle_finalize_ms) * 1000ull;
                    const bool media_idle = cam->segment_active && cam->last_media_us > 0
                                            && current_us > cam->last_media_us
                                            && current_us - cam->last_media_us >= idle_limit_us;
                    bool storage_capacity_failed = false;
                    {
                        std::lock_guard<std::mutex> record_lock(cam->record_mutex);
                        storage_capacity_failed = cam->record_storage_capacity_failed;
                    }
                    if(storage_capacity_failed && cam->segment_active && !cam->segment_finalizing) {
                        cam->segment_finalizing = true;
                        cam->segment_rotation_requested.store(false);
                        cam->segment_rotation_keyframe_requested_us.store(0);
                        set_record_accepting(cam, false);
                        set_record_finalizing(cam, true);
                        storage_close_tasks.push_back({cam, cam->sender_id, cam->camera_id,
                                                       cam->last_announce_live ? cam->last_announce_json : "", 0});
                    }
                    else if(media_idle && !cam->segment_finalizing) {
                        cam->segment_finalizing = true;
                        cam->segment_rotation_requested.store(false);
                        cam->segment_rotation_keyframe_requested_us.store(0);
                        cam->media_idle_finalizations.fetch_add(1);
                        set_record_accepting(cam, false);
                        set_record_finalizing(cam, true);
                        idle_close_tasks.push_back({cam, cam->sender_id, cam->camera_id,
                                                    cam->last_announce_live ? cam->last_announce_json : "", 0});
                    }
                }
            }

            for(const auto &cam : camera_snapshot) {
                if(!cam) {
                    continue;
                }
                bool request_keyframe = false;
                uint64_t target_global_us = 0;
                {
                    std::lock_guard<std::mutex> segment_lock(cam->segment_mutex);
                    const uint64_t lead_us = static_cast<uint64_t>(config_.segment_keyframe_lead_ms) * 1000ull;
                    if(cam->segment && cam->segment->should_request_rotation_keyframe(current_us, lead_us)) {
                        cam->segment_rotation_requested.store(true);
                        target_global_us = cam->segment->segment_window_end_global_us();
                        const uint64_t previous_request_us = cam->segment_rotation_keyframe_requested_us.load();
                        if(previous_request_us == 0 || current_us >= previous_request_us + kSegmentRotationKeyframeRetryUs) {
                            cam->segment_rotation_keyframe_requested_us.store(current_us);
                            cam->segment_rotation_keyframe_requests.fetch_add(1);
                            request_keyframe = true;
                        }
                    }
                }
                if(request_keyframe) {
                    rotation_keyframe_cameras.emplace_back(cam, target_global_us);
                }
            }
            if(!rotation_keyframe_cameras.empty()) {
                std::vector<SenderControlTarget> targets;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    targets.reserve(rotation_keyframe_cameras.size());
                    for(const auto &[cam, target_global_us] : rotation_keyframe_cameras) {
                        if(cam && cam->recording_requested && cam->online && !cam->status_endpoint.empty()) {
                            targets.push_back({cam->sender_id, cam->camera_id, cam->status_endpoint, target_global_us});
                        }
                    }
                }
                if(!targets.empty()) {
                    send_force_rgb_keyframe_controls(targets, "segment_rotation", current_us);
                }
            }
            if(!idle_close_tasks.empty()) {
                logger_.warn("recording media idle timeout; finalizing segments count="
                             + std::to_string(idle_close_tasks.size()));
                close_segments_async(std::move(idle_close_tasks), "recording media idle timeout");
            }
            if(!storage_close_tasks.empty()) {
                logger_.warn("recording storage capacity failure; finalizing segments count="
                             + std::to_string(storage_close_tasks.size()));
                close_segments_async(std::move(storage_close_tasks), "recording storage capacity failure");
            }

            const auto steady_now = std::chrono::steady_clock::now();
            if(steady_now >= next_uploader_status_refresh) {
                refresh_recording_uploader_status();
                next_uploader_status_refresh = steady_now + std::chrono::seconds(1);
            }
            std::unique_lock<std::mutex> wait_lock(recording_maintenance_mutex_);
            recording_maintenance_cv_.wait_for(wait_lock, std::chrono::milliseconds(250), [this] { return !running_; });
        }
        logger_.info("recording maintenance worker stopped");
    }

    void start_recording_maintenance_worker() {
        recording_maintenance_thread_ = std::thread([this] { recording_maintenance_loop(); });
    }

    bool recording_storage_recovery_ready() const {
        constexpr uint64_t kRecoveryHeadroomBytes = 2ull * 1024ull * 1024ull * 1024ull;
        const auto recording_root = recording_write_root(config_);
        std::error_code ec;
        std::filesystem::create_directories(recording_root, ec);
        if(ec) {
            return false;
        }
        const auto space = std::filesystem::space(recording_root, ec);
        if(ec) {
            return false;
        }
        return storage_space_meets_limits(space, config_, kRecoveryHeadroomBytes);
    }

    bool nas_ready_for_new_recording() const {
        if(!config_.nas_auto_mount.enabled || !config_.nas_auto_mount.require_for_new_recording) {
            return true;
        }
        std::ifstream input(config_.nas_auto_mount.status_path);
        if(!input) {
            return false;
        }
        const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        Json::Value root;
        if(!parse_json_object_strict(text, root) || !root["ready"].isBool() || !root["updated_us"].isUInt64()
           || !root["mount_point"].isString() || !root["ready"].asBool()) {
            return false;
        }
        if(std::filesystem::path(root["mount_point"].asString()).lexically_normal()
           != std::filesystem::path(config_.nas_root).lexically_normal()) {
            return false;
        }
        const uint64_t updated_us = root["updated_us"].asUInt64();
        const uint64_t current_us = now_us();
        const uint64_t max_age_us = static_cast<uint64_t>(config_.nas_auto_mount.status_max_age_ms) * 1000ull;
        return updated_us <= current_us && current_us - updated_us <= max_age_us;
    }

    std::optional<std::string> recording_start_block_reason() const {
        if(!recording_storage_recovery_ready()) {
            return "recording storage does not have enough free-space headroom";
        }
        if(!nas_ready_for_new_recording()) {
            return "NAS is not mounted and writable; new recording is blocked";
        }
        return std::nullopt;
    }

    void abort_recording_after_storage_failure(const std::string &camera_key,
                                                const std::string &reason) {
        bool expected = false;
        if(!recording_fault_stop_requested_.compare_exchange_strong(expected, true)) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            recording_faulted_ = true;
            recording_fault_session_id_ = recording_all_session_id_;
            recording_fault_us_ = now_us();
            recording_fault_camera_key_ = camera_key;
            recording_fault_reason_ = reason;
        }
        logger_.warn("recording aborted after storage failure camera=" + camera_key
                     + " reason=" + reason);
        stop_all();
    }

    void wait_record_queue_idle(const std::shared_ptr<CameraState> &cam, const std::string &reason) {
        if(!cam) {
            return;
        }
        uint64_t next_log_us = now_us() + kRecordQueueWarnIntervalUs;
        for(;;) {
            std::unique_lock<std::mutex> record_lock(cam->record_mutex);
            if(cam->record_queue.empty() && cam->record_active_writes == 0) {
                return;
            }
            const bool timed_out = cam->record_cv.wait_for(record_lock, std::chrono::seconds(1)) == std::cv_status::timeout;
            const uint64_t current_us = now_us();
            if(timed_out && current_us >= next_log_us) {
                const size_t packets = cam->record_queue.size();
                const size_t bytes = cam->record_queue_bytes;
                const uint32_t active = cam->record_active_writes;
                record_lock.unlock();
                logger_.info("waiting record queue drain camera=" + cam->key + " reason=" + reason
                             + " packets=" + std::to_string(packets)
                             + " bytes=" + std::to_string(bytes)
                             + " active_writes=" + std::to_string(active));
                next_log_us = current_us + kRecordQueueWarnIntervalUs;
            }
        }
    }

    void stop_record_workers_sync(const std::vector<std::shared_ptr<CameraState>> &cameras) {
        for(const auto &cam : cameras) {
            if(!cam) {
                continue;
            }
            {
                std::lock_guard<std::mutex> record_lock(cam->record_mutex);
                if(cam->record_worker_started) {
                    cam->record_worker_stop = true;
                }
            }
            cam->record_cv.notify_all();
        }
        for(const auto &cam : cameras) {
            if(cam && cam->record_worker.joinable()) {
                cam->record_worker.join();
            }
        }
    }

    void close_segment_task(SegmentCloseTask &task, const std::string &reason) {
        if(!task.cam) {
            return;
        }
        wait_record_queue_idle(task.cam, reason);
        std::unique_ptr<SegmentWriter> replacement;
        std::unique_ptr<SegmentWriter> detached;
        std::string directory;
        bool finalize_slot_reserved = false;
        ScopeExit release_finalize_slot([this, &finalize_slot_reserved] {
            if(finalize_slot_reserved) {
                release_segment_finalize_slot();
            }
        });
        try {
            replacement = std::make_unique<SegmentWriter>();
            {
                std::lock_guard<std::mutex> segment_lock(task.cam->segment_mutex);
                if(task.cam->segment && task.cam->segment->active()) {
                    detached = std::move(task.cam->segment);
                    detached->mark_end_us(now_us());
                    if(task.recording_end_global_us > 0) {
                        detached->mark_recording_window_end_global_us(task.recording_end_global_us);
                    }
                    directory = detached->directory();
                    task.cam->segment = std::move(replacement);
                    task.cam->segment_rotation_requested.store(false);
                    task.cam->segment_rotation_keyframe_requested_us.store(0);
                }
            }
            RecordingActivation activation;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                task.cam->segment_active = false;
                task.cam->segment_finalizing = false;
                task.cam->segment_dir.clear();
                task.cam->segment_start_us = 0;
                task.cam->global_segment_index = 0;
                task.cam->segment_window_start_global_us = 0;
                task.cam->segment_window_end_global_us = 0;
                set_record_finalizing(task.cam, false);
                if(reason == "recording storage capacity failure") {
                    task.cam->recording_start_pending = true;
                }
                else {
                    activation = activate_pending_recordings_locked();
                }
            }
            if(!activation.keyframe_targets.empty()) {
                send_force_rgb_keyframe_controls(activation.keyframe_targets, "record_restart_after_detach",
                                                 activation.request_us, activation.request_us);
            }
            if(!detached) {
                return;
            }

            // Detaching is the only phase that blocks a new recording session. The
            // potentially slow container finalization below owns the old writer.
            if(!reserve_segment_finalize_slot(true)) {
                logger_.warn("segment finalizer unavailable; closing detached segment in close worker camera="
                             + task.cam->key + " directory=" + directory);
                detached->close(config_, task.sender_id, task.camera_id, task.announce_json, logger_);
                return;
            }
            finalize_slot_reserved = true;

            SegmentFinalizeTask finalize_task;
            finalize_task.cam = task.cam;
            finalize_task.segment = std::move(detached);
            finalize_task.sender_id = task.sender_id;
            finalize_task.camera_id = task.camera_id;
            finalize_task.announce_json = task.announce_json;
            finalize_task.reason = reason;
            finalize_task.directory = directory;
            if(!enqueue_reserved_segment_finalize(std::move(finalize_task))) {
                // enqueue_reserved_segment_finalize releases the reservation on failure.
                finalize_slot_reserved = false;
                throw std::runtime_error("cannot enqueue segment finalization: " + directory);
            }
            finalize_slot_reserved = false;
            logger_.info("recording segment queued for finalization camera=" + task.cam->key
                         + " directory=" + directory + " reason=" + reason);
        }
        catch(const std::exception &e) {
            logger_.warn("recording segment finalize failed camera=" + task.cam->key + ": " + e.what());
            RecordingActivation activation;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                task.cam->segment_finalizing = false;
                task.cam->last_error = std::string("recording_finalize_failed: ") + e.what();
                set_record_finalizing(task.cam, false);
                activation = activate_pending_recordings_locked();
            }
            if(!activation.keyframe_targets.empty()) {
                send_force_rgb_keyframe_controls(activation.keyframe_targets, "record_restart_after_detach_error",
                                                 activation.request_us, activation.request_us);
            }
        }
    }

    void reap_segment_close_futures() {
        std::lock_guard<std::mutex> lock(segment_close_futures_mutex_);
        auto it = segment_close_futures_.begin();
        while(it != segment_close_futures_.end()) {
            if(it->valid() && it->wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                it->get();
                it = segment_close_futures_.erase(it);
            }
            else {
                ++it;
            }
        }
    }

    void wait_segment_close_futures() {
        std::vector<std::future<void>> futures;
        {
            std::lock_guard<std::mutex> lock(segment_close_futures_mutex_);
            futures.swap(segment_close_futures_);
        }
        for(auto &future : futures) {
            if(future.valid()) {
                future.get();
            }
        }
    }

    void close_segments_async(std::vector<SegmentCloseTask> close_tasks, const std::string &done_log_message) {
        if(close_tasks.empty()) {
            return;
        }
        reap_segment_close_futures();
        auto tasks = std::make_shared<std::vector<SegmentCloseTask>>(std::move(close_tasks));
        try {
            auto future = std::async(std::launch::async, [this, tasks, done_log_message]() mutable {
                logger_.info(done_log_message + " finalization scheduling started");
                std::vector<std::future<void>> camera_futures;
                camera_futures.reserve(tasks->size());
                for(auto &task : *tasks) {
                    camera_futures.emplace_back(std::async(std::launch::async, [this, &task, &done_log_message] {
                        close_segment_task(task, done_log_message);
                    }));
                }
                for(auto &camera_future : camera_futures) {
                    camera_future.get();
                }
                logger_.info(done_log_message + " finalization queued");
            });
            try {
                std::lock_guard<std::mutex> lock(segment_close_futures_mutex_);
                segment_close_futures_.push_back(std::move(future));
            }
            catch(...) {
                if(future.valid()) {
                    future.get();
                }
                throw;
            }
        }
        catch(const std::exception &e) {
            logger_.warn(done_log_message + " async scheduling failed; queueing synchronously: " + e.what());
            for(auto &task : *tasks) {
                close_segment_task(task, done_log_message);
            }
            logger_.info(done_log_message + " finalization queued");
        }
    }

    void close_segments_sync(std::vector<SegmentCloseTask> close_tasks, const std::string &done_log_message) {
        if(close_tasks.empty()) {
            return;
        }
        for(auto &task : close_tasks) {
            close_segment_task(task, done_log_message);
        }
        logger_.info(done_log_message);
    }

    std::string start_all(const std::optional<std::string> &file_prefix_override) {
        if(file_prefix_override) {
            if(const auto error = storage_text_error("file_prefix", *file_prefix_override)) {
                return json_error(*error);
            }
        }
        const auto start_block_reason = recording_start_block_reason();
        RecordingActivation activation;
        uint64_t response_start_us = 0;
        uint64_t response_session_id = 0;
        bool response_pending = false;
        bool response_has_override = false;
        bool response_promoted = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            refresh_camera_liveness_locked(now_us());
            const bool already_recording = recording_all_;
            if(!already_recording && start_block_reason) {
                return json_error(*start_block_reason);
            }
            const bool individual_recording_active = !already_recording
                                                     && std::any_of(cameras_.begin(), cameras_.end(), [](const auto &item) {
                                                            return item.second->recording_requested;
                                                        });
            if(!already_recording) {
                recording_faulted_ = false;
                recording_fault_session_id_ = 0;
                recording_fault_us_ = 0;
                recording_fault_camera_key_.clear();
                recording_fault_reason_.clear();
                recording_fault_stop_requested_.store(false);
                recording_all_start_us_ = 0;
                recording_all_session_id_ = 0;
                recording_all_start_pending_ = true;
                recording_all_has_file_prefix_override_ = file_prefix_override.has_value();
                recording_all_file_prefix_ = file_prefix_override.value_or("");
            }
            recording_all_ = true;
            for(auto &item : cameras_) {
                const bool preserve_active_recording = individual_recording_active
                                                       && item.second->recording_requested;
                if(!already_recording && !preserve_active_recording) {
                    std::lock_guard<std::mutex> record_lock(item.second->record_mutex);
                    item.second->record_storage_capacity_failed = false;
                    reset_record_session_metrics_locked(*item.second);
                    if(item.second->last_error.rfind("recording_write_failed:", 0) == 0) {
                        item.second->last_error.clear();
                    }
                }
                if(!already_recording && !item.second->recording_requested && !item.second->segment_active) {
                    item.second->recording_start_us = 0;
                    item.second->recording_window = {};
                    item.second->recording_file_prefix = effective_file_prefix_locked(*item.second);
                    item.second->recording_start_pending = true;
                }
                item.second->recording_requested = true;
            }
            activation = activate_pending_recordings_locked();
            response_start_us = recording_all_start_us_;
            response_session_id = recording_all_session_id_;
            response_pending = recording_all_start_pending_;
            response_has_override = recording_all_has_file_prefix_override_;
            response_promoted = individual_recording_active;
            logger_.info(std::string("recording start-all requested pending=") + (response_pending ? "true" : "false")
                         + " promoted_individual=" + (individual_recording_active ? "true" : "false")
                         + " session_id=" + std::to_string(response_session_id)
                         + " start_global_us=" + std::to_string(response_start_us));
        }
        if(!activation.keyframe_targets.empty()) {
            send_force_rgb_keyframe_controls(activation.keyframe_targets, "record_start_all", activation.request_us,
                                             activation.request_us);
        }
        std::ostringstream out;
        out << "{\"ok\":true,\"recording_all\":true,\"recording_start_us\":" << response_start_us
            << ",\"recording_session_id\":" << response_session_id
            << ",\"start_pending\":" << (response_pending ? "true" : "false")
            << ",\"promoted_individual\":" << (response_promoted ? "true" : "false")
            << ",\"file_prefix_scope\":\"" << (response_has_override ? "override_all" : "per_camera") << "\"}";
        return out.str();
    }

    std::string stop_all() {
        std::vector<SegmentCloseTask> close_tasks;
        uint64_t recording_start_us = 0;
        uint64_t recording_session_id = 0;
        const uint64_t recording_end_global_us = now_us();
        bool finalizing = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            recording_start_us = recording_all_start_us_;
            recording_session_id = recording_all_session_id_;
            if(recording_start_us == 0) {
                for(const auto &item : cameras_) {
                    if(item.second->recording_start_us > 0 &&
                       (recording_start_us == 0 || item.second->recording_start_us < recording_start_us)) {
                        recording_start_us = item.second->recording_start_us;
                    }
                }
            }
            recording_all_ = false;
            recording_all_start_pending_ = false;
            recording_all_session_id_ = 0;
            recording_all_start_us_ = 0;
            recording_all_has_file_prefix_override_ = false;
            recording_all_file_prefix_.clear();
            for(auto &item : cameras_) {
                const bool already_finalizing = item.second->segment_finalizing || item.second->record_finalizing;
                const bool needs_close = item.second->recording_requested || item.second->segment_active;
                item.second->recording_requested = false;
                item.second->recording_start_pending = false;
                item.second->recording_window.end_global_us = recording_end_global_us;
                item.second->recording_start_us = 0;
                item.second->recording_file_prefix.clear();
                set_record_accepting(item.second, false);
                if(already_finalizing) {
                    finalizing = true;
                }
                else if(needs_close) {
                    item.second->segment_finalizing = true;
                    set_record_finalizing(item.second, true);
                    finalizing = true;
                    close_tasks.push_back({item.second, item.second->sender_id, item.second->camera_id,
                                           item.second->last_announce_live ? item.second->last_announce_json : "",
                                           recording_end_global_us});
                }
            }
            refresh_camera_liveness_locked(now_us());
        }
        logger_.info("recording stop-all requested");
        if(finalizing) {
            close_segments_async(std::move(close_tasks), "recording stop-all");
        }
        std::ostringstream out;
        out << "{\"ok\":true,\"recording_all\":false,\"recording_start_us\":" << recording_start_us
            << ",\"recording_session_id\":" << recording_session_id
            << ",\"recording_end_global_us\":" << recording_end_global_us
            << ",\"finalizing\":" << (finalizing ? "true" : "false")
            << ",\"finalized\":" << (finalizing ? "false" : "true") << "}";
        return out.str();
    }

    std::string start_sender(const std::string &sender_id) {
        if(sender_id.empty()) {
            return json_error("sender_id is required");
        }
        const auto start_block_reason = recording_start_block_reason();
        RecordingActivation activation;
        size_t camera_count = 0;
        size_t started_count = 0;
        uint64_t recording_start_us = 0;
        uint64_t recording_session_id = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const uint64_t request_us = now_us();
            refresh_camera_liveness_locked(request_us);
            std::vector<std::shared_ptr<CameraState>> sender_cameras;
            for(auto &item : cameras_) {
                auto &cam = item.second;
                if(cam->sender_id == sender_id && cam->online
                   && is_recent_us(request_us, cam->last_media_us, kCameraOnlineTimeoutUs)) {
                    sender_cameras.push_back(cam);
                }
            }
            if(sender_cameras.empty()) {
                return json_error("no live cameras found for sender_id");
            }
            camera_count = sender_cameras.size();
            const bool needs_start = std::any_of(sender_cameras.begin(), sender_cameras.end(), [](const auto &cam) {
                return !cam->recording_requested;
            });
            if(needs_start && start_block_reason) {
                return json_error(*start_block_reason);
            }
            if(needs_start) {
                recording_faulted_ = false;
                recording_fault_session_id_ = 0;
                recording_fault_us_ = 0;
                recording_fault_camera_key_.clear();
                recording_fault_reason_.clear();
                recording_fault_stop_requested_.store(false);
                recording_start_us = request_us
                                     + static_cast<uint64_t>(config_.recording_start_lead_ms) * 1000ull;
                recording_session_id = next_recording_session_id_locked();
            }
            const uint64_t new_recording_start_us = recording_start_us;
            const uint64_t new_recording_session_id = recording_session_id;
            for(const auto &cam : sender_cameras) {
                if(cam->recording_requested) {
                    if(!needs_start
                       && (recording_start_us == 0
                           || (cam->recording_start_us > 0 && cam->recording_start_us < recording_start_us))) {
                        recording_start_us = cam->recording_start_us;
                        recording_session_id = cam->recording_window.session_id;
                    }
                    continue;
                }
                {
                    std::lock_guard<std::mutex> record_lock(cam->record_mutex);
                    cam->record_storage_capacity_failed = false;
                    reset_record_session_metrics_locked(*cam);
                }
                if(cam->last_error.rfind("recording_write_failed:", 0) == 0) {
                    cam->last_error.clear();
                }
                cam->recording_start_us = new_recording_start_us;
                cam->recording_window = {new_recording_session_id, new_recording_start_us, 0};
                cam->recording_file_prefix = cam->camera_file_prefix;
                cam->recording_start_pending = true;
                cam->recording_requested = true;
                ++started_count;
            }
            activation = activate_pending_recordings_locked();
            logger_.info("recording start-sender requested sender=" + sender_id
                         + " cameras=" + std::to_string(camera_count)
                         + " started=" + std::to_string(started_count)
                         + " session_id=" + std::to_string(recording_session_id)
                         + " start_global_us=" + std::to_string(recording_start_us));
        }
        if(!activation.keyframe_targets.empty()) {
            send_force_rgb_keyframe_controls(activation.keyframe_targets, "record_start_sender",
                                             activation.request_us, activation.request_us);
        }
        std::ostringstream out;
        out << "{\"ok\":true,\"sender_id\":\"" << json_escape(sender_id)
            << "\",\"camera_count\":" << camera_count
            << ",\"started_count\":" << started_count
            << ",\"already_recording\":" << (started_count == 0 ? "true" : "false")
            << ",\"recording_start_us\":" << recording_start_us
            << ",\"recording_session_id\":" << recording_session_id << "}";
        return out.str();
    }

    std::string stop_sender(const std::string &sender_id) {
        if(sender_id.empty()) {
            return json_error("sender_id is required");
        }
        std::vector<SegmentCloseTask> close_tasks;
        const uint64_t recording_end_global_us = now_us();
        size_t camera_count = 0;
        size_t stopped_count = 0;
        bool finalizing = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            refresh_camera_liveness_locked(recording_end_global_us);
            std::vector<std::shared_ptr<CameraState>> sender_cameras;
            for(auto &item : cameras_) {
                if(item.second->sender_id == sender_id) {
                    sender_cameras.push_back(item.second);
                }
            }
            if(sender_cameras.empty()) {
                return json_error("sender_id not found");
            }
            camera_count = sender_cameras.size();

            // start-all materializes recording_requested on every camera. Leaving
            // global mode preserves all non-target cameras as individual recordings.
            if(recording_all_) {
                recording_all_ = false;
                recording_all_start_pending_ = false;
                recording_all_session_id_ = 0;
                recording_all_start_us_ = 0;
                recording_all_has_file_prefix_override_ = false;
                recording_all_file_prefix_.clear();
            }
            for(const auto &cam : sender_cameras) {
                const bool already_finalizing = cam->segment_finalizing || cam->record_finalizing;
                const bool needs_close = cam->recording_requested || cam->segment_active;
                if(!needs_close && !already_finalizing) {
                    continue;
                }
                ++stopped_count;
                finalizing = true;
                cam->recording_requested = false;
                cam->recording_start_pending = false;
                cam->recording_window.end_global_us = recording_end_global_us;
                cam->recording_start_us = 0;
                cam->recording_file_prefix.clear();
                set_record_accepting(cam, false);
                if(needs_close && !already_finalizing) {
                    cam->segment_finalizing = true;
                    set_record_finalizing(cam, true);
                    close_tasks.push_back({cam, cam->sender_id, cam->camera_id,
                                           cam->last_announce_live ? cam->last_announce_json : "",
                                           recording_end_global_us});
                }
            }
            logger_.info("recording stop-sender requested sender=" + sender_id
                         + " cameras=" + std::to_string(camera_count)
                         + " stopped=" + std::to_string(stopped_count));
        }
        if(!close_tasks.empty()) {
            close_segments_async(std::move(close_tasks), "recording stop-sender: " + sender_id);
        }
        std::ostringstream out;
        out << "{\"ok\":true,\"sender_id\":\"" << json_escape(sender_id)
            << "\",\"camera_count\":" << camera_count
            << ",\"stopped_count\":" << stopped_count
            << ",\"recording_end_global_us\":" << recording_end_global_us
            << ",\"finalizing\":" << (finalizing ? "true" : "false")
            << ",\"finalized\":" << (finalizing ? "false" : "true") << "}";
        return out.str();
    }

    std::string start_camera(const std::string &sender_id, const std::string &camera_id, const std::optional<std::string> &file_prefix_override) {
        if(sender_id.empty() || camera_id.empty()) {
            return "{\"ok\":false,\"error\":\"sender_id and camera_id are required\"}";
        }
        if(file_prefix_override) {
            if(const auto error = storage_text_error("file_prefix", *file_prefix_override)) {
                return json_error(*error);
            }
        }
        const auto start_block_reason = recording_start_block_reason();
        uint64_t response_start_us = 0;
        uint64_t response_session_id = 0;
        bool response_pending = false;
        std::string response_file_prefix;
        RecordingActivation activation;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto cam_ptr = ensure_camera_ptr_locked(sender_id, camera_id);
            auto &cam = *cam_ptr;
            const bool already_recording = cam.recording_requested || cam.segment_active;
            if(!already_recording && start_block_reason) {
                return json_error(*start_block_reason);
            }
            if(!already_recording && !recording_all_) {
                recording_faulted_ = false;
                recording_fault_session_id_ = 0;
                recording_fault_us_ = 0;
                recording_fault_camera_key_.clear();
                recording_fault_reason_.clear();
                recording_fault_stop_requested_.store(false);
                std::lock_guard<std::mutex> record_lock(cam.record_mutex);
                cam.record_storage_capacity_failed = false;
                reset_record_session_metrics_locked(cam);
                if(cam.last_error.rfind("recording_write_failed:", 0) == 0) {
                    cam.last_error.clear();
                }
            }
            if(!recording_all_ && !cam.recording_requested && !cam.segment_active) {
                cam.recording_start_us = 0;
                cam.recording_window = {};
                cam.recording_file_prefix = file_prefix_override.value_or(cam.camera_file_prefix);
                cam.recording_start_pending = true;
            }
            else if(cam.recording_start_us == 0) {
                cam.recording_start_us = recording_all_ ? recording_all_start_us_ : 0;
                cam.recording_file_prefix = recording_all_ ? effective_file_prefix_locked(cam) : file_prefix_override.value_or(cam.camera_file_prefix);
                cam.recording_start_pending = true;
            }
            cam.recording_requested = true;
            activation = activate_pending_recordings_locked();
            response_start_us = cam.recording_start_us;
            response_session_id = cam.recording_window.session_id;
            response_pending = cam.recording_start_pending;
            response_file_prefix = cam.recording_file_prefix;
            logger_.info("recording start requested: " + cam.key + " pending=" + (response_pending ? "true" : "false")
                         + " session_id=" + std::to_string(response_session_id)
                         + " start_global_us=" + std::to_string(response_start_us));
        }
        if(!activation.keyframe_targets.empty()) {
            send_force_rgb_keyframe_controls(activation.keyframe_targets, "record_start", activation.request_us,
                                             activation.request_us);
        }
        std::ostringstream out;
        out << "{\"ok\":true,\"recording_start_us\":" << response_start_us << ",\"file_prefix\":\""
            << json_escape(response_file_prefix) << "\",\"recording_session_id\":" << response_session_id
            << ",\"start_pending\":" << (response_pending ? "true" : "false") << "}";
        return out.str();
    }

    std::string stop_camera(const std::string &sender_id, const std::string &camera_id) {
        if(sender_id.empty() || camera_id.empty()) {
            return "{\"ok\":false,\"error\":\"sender_id and camera_id are required\"}";
        }
        std::shared_ptr<CameraState> cam;
        std::string announce_json;
        uint64_t recording_start_us = 0;
        uint64_t recording_session_id = 0;
        const uint64_t recording_end_global_us = now_us();
        bool finalizing = false;
        bool schedule_close = false;
        const auto key = camera_key(sender_id, camera_id);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = cameras_.find(key);
            if(it == cameras_.end()) {
                return "{\"ok\":false,\"error\":\"camera not found\"}";
            }
            if(recording_all_) {
                return json_error("cannot stop one camera while start-all recording is active; use stop-all");
            }
            cam = it->second;
            announce_json = cam->last_announce_live ? cam->last_announce_json : "";
            recording_start_us = cam->recording_start_us;
            recording_session_id = cam->recording_window.session_id;
            const bool already_finalizing = cam->segment_finalizing || cam->record_finalizing;
            const bool needs_close = cam->recording_requested || cam->segment_active;
            finalizing = already_finalizing || needs_close;
            schedule_close = needs_close && !already_finalizing;
            cam->recording_requested = false;
            cam->recording_start_pending = false;
            cam->recording_window.end_global_us = recording_end_global_us;
            if(schedule_close) {
                cam->segment_finalizing = true;
            }
            cam->recording_start_us = 0;
            cam->recording_file_prefix.clear();
            set_record_accepting(cam, false);
            if(schedule_close) {
                set_record_finalizing(cam, true);
            }
        }
        logger_.info("recording stop requested: " + key);
        std::vector<SegmentCloseTask> close_tasks;
        if(schedule_close) {
            close_tasks.push_back({cam, cam->sender_id, cam->camera_id, announce_json, recording_end_global_us});
            close_segments_async(std::move(close_tasks), "recording stop: " + key);
        }
        std::ostringstream out;
        out << "{\"ok\":true,\"recording_start_us\":" << recording_start_us
            << ",\"recording_session_id\":" << recording_session_id
            << ",\"recording_end_global_us\":" << recording_end_global_us
            << ",\"finalizing\":" << (finalizing ? "true" : "false")
            << ",\"finalized\":" << (finalizing ? "false" : "true") << "}";
        return out.str();
    }

    std::string set_camera_name(const std::string &sender_id, const std::string &camera_id, const std::string &camera_name) {
        if(sender_id.empty() || camera_id.empty()) {
            return "{\"ok\":false,\"error\":\"sender_id and camera_id are required\"}";
        }
        if(const auto error = storage_text_error("camera_name", camera_name)) {
            return json_error(*error);
        }
        const auto key = camera_key(sender_id, camera_id);
        RuntimeState state_snapshot;
        uint64_t state_revision = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(camera_name.empty()) {
                runtime_state_.camera_names.erase(key);
            }
            else {
                runtime_state_.camera_names[key] = camera_name;
            }
            auto it = cameras_.find(key);
            if(it != cameras_.end()) {
                it->second->camera_name = camera_name;
            }
            state_revision = ++runtime_state_revision_;
            state_snapshot = runtime_state_;
        }
        try {
            persist_runtime_state_snapshot(state_snapshot, state_revision);
        }
        catch(const std::exception &e) {
            logger_.error(e.what());
            return json_error(e.what());
        }
        logger_.info("camera name updated: " + key + " -> " + (camera_name.empty() ? key : camera_name));
        std::ostringstream out;
        out << "{\"ok\":true,\"sender_id\":\"" << json_escape(sender_id) << "\",\"camera_id\":\"" << json_escape(camera_id)
            << "\",\"camera_key\":\"" << json_escape(key) << "\",\"camera_name\":\"" << json_escape(camera_name)
            << "\",\"storage_key\":\"" << json_escape(camera_name.empty() ? key : camera_name) << "\"}";
        return out.str();
    }

    std::string set_camera_file_prefix(const std::string &sender_id, const std::string &camera_id, const std::string &file_prefix) {
        if(sender_id.empty() || camera_id.empty()) {
            return "{\"ok\":false,\"error\":\"sender_id and camera_id are required\"}";
        }
        if(const auto error = storage_text_error("file_prefix", file_prefix)) {
            return json_error(*error);
        }
        const auto key = camera_key(sender_id, camera_id);
        RuntimeState state_snapshot;
        uint64_t state_revision = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(file_prefix.empty()) {
                runtime_state_.camera_file_prefixes.erase(key);
            }
            else {
                runtime_state_.camera_file_prefixes[key] = file_prefix;
            }
            auto it = cameras_.find(key);
            if(it != cameras_.end()) {
                it->second->camera_file_prefix = file_prefix;
            }
            state_revision = ++runtime_state_revision_;
            state_snapshot = runtime_state_;
        }
        try {
            persist_runtime_state_snapshot(state_snapshot, state_revision);
        }
        catch(const std::exception &e) {
            logger_.error(e.what());
            return json_error(e.what());
        }
        logger_.info("camera file prefix updated: " + key + " -> " + file_prefix);
        std::ostringstream out;
        out << "{\"ok\":true,\"sender_id\":\"" << json_escape(sender_id) << "\",\"camera_id\":\"" << json_escape(camera_id)
            << "\",\"camera_key\":\"" << json_escape(key) << "\",\"camera_file_prefix\":\"" << json_escape(file_prefix) << "\"}";
        return out.str();
    }

    std::string set_default_file_prefix(const std::string &file_prefix) {
        if(const auto error = storage_text_error("file_prefix", file_prefix)) {
            return json_error(*error);
        }
        RuntimeState state_snapshot;
        uint64_t state_revision = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            runtime_state_.default_file_prefix = file_prefix;
            state_revision = ++runtime_state_revision_;
            state_snapshot = runtime_state_;
        }
        try {
            persist_runtime_state_snapshot(state_snapshot, state_revision);
        }
        catch(const std::exception &e) {
            logger_.error(e.what());
            return json_error(e.what());
        }
        logger_.info("default file prefix updated: " + file_prefix);
        return "{\"ok\":true,\"default_file_prefix\":\"" + json_escape(file_prefix) + "\"}";
    }

    std::optional<std::vector<uint8_t>> depth_preview(const std::string &sender_id, const std::string &camera_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!config_.preview_enabled) {
            return std::nullopt;
        }
        const auto now = now_us();
        refresh_camera_liveness_locked(now);
        const auto key = camera_key(sender_id, camera_id);
        auto it = cameras_.find(key);
        if(it == cameras_.end() || !it->second->online) {
            return std::nullopt;
        }
        it->second->depth_preview_requested_until_us = now + kPreviewRequestKeepaliveUs;
        std::lock_guard<std::mutex> preview_lock(it->second->preview_mutex);
        if(!is_recent_us(now, it->second->depth_preview_us, kPreviewFreshUs) || it->second->depth_preview_ppm.empty()) {
            return std::nullopt;
        }
        return it->second->depth_preview_ppm;
    }

    std::optional<std::vector<uint8_t>> rgb_preview(const std::string &sender_id, const std::string &camera_id) {
        std::optional<SenderControlTarget> keyframe_target;
        std::optional<std::vector<uint8_t>> jpeg;
        const auto request_us = now_us();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(!config_.preview_enabled) {
                return std::nullopt;
            }
            refresh_camera_liveness_locked(request_us);
            const auto key = camera_key(sender_id, camera_id);
            auto it = cameras_.find(key);
            if(it == cameras_.end() || !it->second->online) {
                return std::nullopt;
            }
            if(!kEnableRgbThumbnailPreview) {
                return std::nullopt;
            }
            auto &cam = *it->second;
            cam.rgb_preview_requested_until_us = request_us + kPreviewRequestKeepaliveUs;
            std::lock_guard<std::mutex> preview_lock(cam.preview_mutex);
            if(!is_recent_us(request_us, cam.rgb_preview_us, kPreviewFreshUs) || !cam.rgb_decoder) {
                keyframe_target = maybe_web_rgb_preview_keyframe_target_locked(cam, request_us);
            }
            if(is_recent_us(request_us, cam.rgb_preview_us, kPreviewFreshUs) && cam.rgb_decoder) {
                jpeg = cam.rgb_decoder->latest_jpeg();
            }
        }
        if(keyframe_target) {
            send_force_rgb_keyframe_controls({*keyframe_target}, "web_rgb_jpeg_preview", request_us);
        }
        return jpeg;
    }

    std::string set_main_preview_target(const std::string &sender_id, const std::string &camera_id) {
        if(!config_.preview_enabled) {
            return "{\"ok\":false,\"error\":\"preview disabled\"}";
        }
        if(sender_id.empty() || camera_id.empty()) {
            return "{\"ok\":false,\"error\":\"sender_id and camera_id are required\"}";
        }
        std::optional<SenderControlTarget> keyframe_target;
        const auto request_us = now_us();
        std::string selected_key;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto key = camera_key(sender_id, camera_id);
            auto it = cameras_.find(key);
            if(it == cameras_.end()) {
                return "{\"ok\":false,\"error\":\"camera not found\"}";
            }
            const bool target_changed = main_preview_key_ != key;
            if(target_changed) {
                for(auto &item : cameras_) {
                    if(item.first != key) {
                        std::lock_guard<std::mutex> preview_lock(item.second->preview_mutex);
                        cleanup_rgb_decoder_async(std::move(item.second->main_rgb_decoder));
                        item.second->main_rgb_preview_requested_until_us = 0;
                        item.second->main_rgb_preview_us = 0;
                        item.second->main_rgb_preview_width = 0;
                        item.second->main_rgb_preview_height = 0;
                    }
                }
            }
            main_preview_key_ = key;
            it->second->main_rgb_preview_requested_until_us = request_us + kMainPreviewRequestKeepaliveUs;
            std::lock_guard<std::mutex> preview_lock(it->second->preview_mutex);
            if(target_changed || !is_recent_us(request_us, it->second->main_rgb_preview_us, kPreviewFreshUs) || !it->second->main_rgb_decoder) {
                keyframe_target = maybe_web_rgb_preview_keyframe_target_locked(*it->second, request_us);
            }
            selected_key = main_preview_key_;
        }
        if(keyframe_target) {
            send_force_rgb_keyframe_controls({*keyframe_target}, "web_rgb_main_target", request_us);
        }
        std::ostringstream out;
        out << "{\"ok\":true,\"main_preview_camera_key\":\"" << json_escape(selected_key) << "\"}";
        return out.str();
    }

    std::optional<std::vector<uint8_t>> main_rgb_preview(const std::string &sender_id, const std::string &camera_id) {
        std::optional<SenderControlTarget> keyframe_target;
        std::optional<std::vector<uint8_t>> jpeg;
        const auto request_us = now_us();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(!config_.preview_enabled) {
                return std::nullopt;
            }
            refresh_camera_liveness_locked(request_us);
            const auto key = camera_key(sender_id, camera_id);
            auto it = cameras_.find(key);
            if(it == cameras_.end() || !it->second->online || !is_recent_us(request_us, it->second->last_media_us, kCameraOnlineTimeoutUs)) {
                return std::nullopt;
            }
            if(main_preview_key_.empty()) {
                main_preview_key_ = key;
            }
            auto &cam = *it->second;
            if(key == main_preview_key_) {
                cam.main_rgb_preview_requested_until_us = request_us + kMainPreviewRequestKeepaliveUs;
            }
            std::lock_guard<std::mutex> preview_lock(cam.preview_mutex);
            if(key == main_preview_key_) {
                if(!is_recent_us(request_us, cam.main_rgb_preview_us, kPreviewFreshUs) || !cam.main_rgb_decoder) {
                    keyframe_target = maybe_web_rgb_preview_keyframe_target_locked(cam, request_us);
                }
            }
            if(!kEnableJpegMainPreview) {
                return std::nullopt;
            }
            if(key == main_preview_key_ && is_recent_us(request_us, cam.main_rgb_preview_us, kPreviewFreshUs) && cam.main_rgb_decoder) {
                jpeg = cam.main_rgb_decoder->latest_jpeg();
            }
            if(!jpeg && is_recent_us(request_us, cam.rgb_preview_us, kPreviewFreshUs) && cam.rgb_decoder) {
                jpeg = cam.rgb_decoder->latest_jpeg();
            }
        }
        if(keyframe_target) {
            send_force_rgb_keyframe_controls({*keyframe_target}, "web_rgb_main_preview", request_us);
        }
        return jpeg;
    }

    bool stream_rgb_h264_preview(int fd, const std::string &sender_id, const std::string &camera_id) {
        configure_rgb_h264_client_socket(fd);
        std::shared_ptr<CameraState> cam;
        std::optional<SenderControlTarget> keyframe_target;
        const auto request_us = now_us();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(!config_.preview_enabled) {
                const std::string body = "{\"ok\":false,\"error\":\"preview disabled\"}";
                std::ostringstream response;
                response << "HTTP/1.1 404 Not Found\r\n";
                response << "Content-Type: application/json\r\n";
                response << "Cache-Control: no-store\r\n";
                response << "Content-Length: " << body.size() << "\r\n";
                response << "Connection: close\r\n\r\n";
                response << body;
                return send_all(fd, response.str());
            }
            refresh_camera_liveness_locked(request_us);
            const auto key = camera_key(sender_id, camera_id);
            auto it = cameras_.find(key);
            if(it == cameras_.end() || !it->second->online || !is_recent_us(request_us, it->second->last_media_us, kCameraOnlineTimeoutUs)) {
                const std::string body = "{\"ok\":false,\"error\":\"rgb h264 stream not found\"}";
                std::ostringstream response;
                response << "HTTP/1.1 404 Not Found\r\n";
                response << "Content-Type: application/json\r\n";
                response << "Cache-Control: no-store\r\n";
                response << "Content-Length: " << body.size() << "\r\n";
                response << "Connection: close\r\n\r\n";
                response << body;
                return send_all(fd, response.str());
            }
            it->second->rgb_stream_requested_until_us = request_us + kPreviewRequestKeepaliveUs;
            keyframe_target = maybe_web_rgb_preview_keyframe_target_locked(*it->second, request_us);
            cam = it->second;
        }
        if(keyframe_target) {
            send_force_rgb_keyframe_controls({*keyframe_target}, "web_rgb_h264_preview", request_us);
        }

        H264StreamBuffer *stream = &cam->rgb_stream;
        bool using_preview_stream = false;
        for(int attempt = 0; attempt < 10; ++attempt) {
            {
                std::lock_guard<std::mutex> stream_lock(cam->rgb_preview_stream.mutex);
                const auto now = now_us();
                using_preview_stream = is_recent_us(now, cam->rgb_preview_stream.last_us, kPreviewFreshUs);
                if(using_preview_stream) {
                    stream = &cam->rgb_preview_stream;
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        std::ostringstream response;
        response << "HTTP/1.1 200 OK\r\n";
        response << "Content-Type: video/h264\r\n";
        response << "Cache-Control: no-store\r\n";
        response << "Connection: close\r\n";
        response << "X-GWV3-Rgb-Stream: " << (using_preview_stream ? "preview" : "main") << "\r\n";
        response << "X-Accel-Buffering: no\r\n\r\n";
        if(!send_all(fd, response.str())) {
            return false;
        }

        bool started = false;
        uint64_t next_seq = 0;
        while(running_ && g_running) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                cam->rgb_stream_requested_until_us = now_us() + kPreviewRequestKeepaliveUs;
            }
            std::vector<uint8_t> header;
            std::vector<H264StreamPacket> packets;
            {
                std::unique_lock<std::mutex> stream_lock(stream->mutex);
                stream->cv.wait_for(stream_lock, std::chrono::milliseconds(1000));
                if(stream->packets.empty()) {
                    continue;
                }

                if(!started) {
                    size_t start_index = stream->packets.size();
                    for(size_t i = stream->packets.size(); i > 0; --i) {
                        if(stream->packets[i - 1].has_idr) {
                            start_index = i - 1;
                            break;
                        }
                    }
                    if(start_index == stream->packets.size()) {
                        continue;
                    }
                    header = stream->header_h264;
                    for(size_t i = start_index; i < stream->packets.size(); ++i) {
                        packets.push_back(stream->packets[i]);
                    }
                    next_seq = packets.empty() ? stream->next_seq : packets.back().seq + 1;
                    started = true;
                }
                else {
                    if(next_seq < stream->packets.front().seq) {
                        next_seq = stream->packets.front().seq;
                    }
                    for(const auto &packet : stream->packets) {
                        if(packet.seq >= next_seq) {
                            packets.push_back(packet);
                        }
                    }
                    if(!packets.empty()) {
                        next_seq = packets.back().seq + 1;
                    }
                }
            }

            if(!header.empty() && !send_all(fd, header.data(), header.size())) {
                return false;
            }
            for(const auto &packet : packets) {
                if(!send_all(fd, packet.payload.data(), packet.payload.size())) {
                    return false;
                }
            }
        }
        return true;
    }

    bool stream_rgb_h264_preview_frames(int fd,
                                        const std::string &sender_id,
                                        const std::string &camera_id,
                                        bool force_main_stream,
                                        bool include_global_timestamp) {
        configure_rgb_h264_client_socket(fd);
        std::shared_ptr<CameraState> cam;
        std::optional<SenderControlTarget> keyframe_target;
        const auto request_us = now_us();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(!config_.preview_enabled) {
                const std::string body = "{\"ok\":false,\"error\":\"preview disabled\"}";
                std::ostringstream response;
                response << "HTTP/1.1 404 Not Found\r\n";
                response << "Content-Type: application/json\r\n";
                response << "Cache-Control: no-store\r\n";
                response << "Content-Length: " << body.size() << "\r\n";
                response << "Connection: close\r\n\r\n";
                response << body;
                return send_all(fd, response.str());
            }
            refresh_camera_liveness_locked(request_us);
            const auto key = camera_key(sender_id, camera_id);
            auto it = cameras_.find(key);
            if(it == cameras_.end() || !it->second->online || !is_recent_us(request_us, it->second->last_media_us, kCameraOnlineTimeoutUs)) {
                const std::string body = "{\"ok\":false,\"error\":\"rgb h264 stream not found\"}";
                std::ostringstream response;
                response << "HTTP/1.1 404 Not Found\r\n";
                response << "Content-Type: application/json\r\n";
                response << "Cache-Control: no-store\r\n";
                response << "Content-Length: " << body.size() << "\r\n";
                response << "Connection: close\r\n\r\n";
                response << body;
                return send_all(fd, response.str());
            }
            if(force_main_stream) {
                it->second->rgb_main_stream_requested_until_us = request_us + kPreviewRequestKeepaliveUs;
            }
            else {
                it->second->rgb_stream_requested_until_us = request_us + kPreviewRequestKeepaliveUs;
            }
            keyframe_target = maybe_web_rgb_preview_keyframe_target_locked(*it->second, request_us);
            cam = it->second;
        }
        uint64_t main_request_seq = 1;
        uint64_t preview_request_seq = 1;
        {
            std::lock_guard<std::mutex> stream_lock(cam->rgb_stream.mutex);
            main_request_seq = cam->rgb_stream.next_seq;
        }
        {
            std::lock_guard<std::mutex> stream_lock(cam->rgb_preview_stream.mutex);
            preview_request_seq = cam->rgb_preview_stream.next_seq;
        }
        if(keyframe_target) {
            send_force_rgb_keyframe_controls({*keyframe_target}, "web_rgb_h264_frames", request_us);
        }

        H264StreamBuffer *stream = force_main_stream ? &cam->rgb_stream : nullptr;
        bool using_preview_stream = false;
        const auto preview_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
        while(!force_main_stream && running_ && g_running && std::chrono::steady_clock::now() < preview_deadline) {
            {
                std::unique_lock<std::mutex> stream_lock(cam->rgb_preview_stream.mutex);
                const auto now = now_us();
                if(is_recent_us(now, cam->rgb_preview_stream.last_us, kPreviewFreshUs) && !cam->rgb_preview_stream.packets.empty()) {
                    stream = &cam->rgb_preview_stream;
                    using_preview_stream = true;
                    break;
                }
                stream_lock.unlock();
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                cam->rgb_stream_requested_until_us = now_us() + kPreviewRequestKeepaliveUs;
            }
            std::unique_lock<std::mutex> wait_lock(cam->rgb_preview_stream.mutex);
            cam->rgb_preview_stream.cv.wait_for(wait_lock, std::chrono::milliseconds(50));
        }

        if(!stream) {
            const std::string body = "{\"ok\":false,\"error\":\"rgb preview stream unavailable\"}";
            std::ostringstream response;
            response << "HTTP/1.1 503 Service Unavailable\r\n";
            response << "Content-Type: application/json\r\n";
            response << "Cache-Control: no-store\r\n";
            response << "Content-Length: " << body.size() << "\r\n";
            response << "Connection: close\r\n\r\n";
            response << body;
            return send_all(fd, response.str());
        }

        std::ostringstream response;
        response << "HTTP/1.1 200 OK\r\n";
        response << "Content-Type: application/octet-stream\r\n";
        response << "Cache-Control: no-store\r\n";
        response << "Connection: close\r\n";
        response << "X-GWV3-Rgb-Stream: " << (using_preview_stream ? "preview" : "main") << "\r\n";
        response << "X-GWV3-Frame-Version: " << (include_global_timestamp ? 2 : 1) << "\r\n";
        response << "X-Accel-Buffering: no\r\n\r\n";
        if(!send_all(fd, response.str())) {
            return false;
        }

        uint64_t next_seq = using_preview_stream ? preview_request_seq : main_request_seq;
        bool waiting_for_keyframe = true;
        while(running_ && g_running) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if(force_main_stream) {
                    cam->rgb_main_stream_requested_until_us = now_us() + kPreviewRequestKeepaliveUs;
                }
                else {
                    cam->rgb_stream_requested_until_us = now_us() + kPreviewRequestKeepaliveUs;
                }
            }
            std::vector<uint8_t> header;
            std::vector<H264StreamPacket> packets;
            {
                std::unique_lock<std::mutex> stream_lock(stream->mutex);
                stream->cv.wait_for(stream_lock, std::chrono::milliseconds(1000));
                if(stream->packets.empty()) {
                    continue;
                }

                header = stream->header_h264;
                const uint64_t newest_next_seq = stream->next_seq;
                if(next_seq + kRgbH264ClientMaxLagPackets < newest_next_seq) {
                    next_seq = newest_next_seq;
                    waiting_for_keyframe = true;
                }
                if(next_seq < stream->packets.front().seq) {
                    next_seq = stream->packets.front().seq;
                    waiting_for_keyframe = true;
                }

                size_t start_index = stream->packets.size();
                if(waiting_for_keyframe) {
                    for(size_t i = stream->packets.size(); i > 0; --i) {
                        const auto &candidate = stream->packets[i - 1];
                        if(candidate.seq >= next_seq && candidate.has_idr) {
                            start_index = i - 1;
                            break;
                        }
                    }
                    if(start_index == stream->packets.size()) {
                        next_seq = newest_next_seq;
                        continue;
                    }
                    waiting_for_keyframe = false;
                }
                else {
                    for(size_t i = 0; i < stream->packets.size(); ++i) {
                        if(stream->packets[i].seq >= next_seq) {
                            start_index = i;
                            break;
                        }
                    }
                }
                for(size_t i = start_index; i < stream->packets.size(); ++i) {
                    packets.push_back(stream->packets[i]);
                }
                if(!packets.empty()) {
                    next_seq = packets.back().seq + 1;
                }
            }

            for(const auto &packet : packets) {
                uint32_t flags = packet.has_idr ? kH264PreviewFrameFlagKey : 0u;
                if(include_global_timestamp && packet.clock_sync_valid) {
                    flags |= kH264PreviewFrameFlagClockSyncValid;
                }
                const auto timestamp_us = packet.timestamp_us > 0 ? packet.timestamp_us : now_us();
                if(packet.has_idr && !header.empty()) {
                    std::vector<uint8_t> payload;
                    payload.reserve(header.size() + packet.payload.size());
                    payload.insert(payload.end(), header.begin(), header.end());
                    payload.insert(payload.end(), packet.payload.begin(), packet.payload.end());
                    flags |= kH264PreviewFrameFlagConfig;
                    if(!send_h264_preview_frame(fd, payload, flags, packet.width, packet.height, timestamp_us, packet.seq,
                                                include_global_timestamp, packet.global_timestamp_us)) {
                        return false;
                    }
                }
                else if(!send_h264_preview_frame(fd, packet.payload, flags, packet.width, packet.height, timestamp_us, packet.seq,
                                                 include_global_timestamp, packet.global_timestamp_us)) {
                    return false;
                }
            }
        }
        return true;
    }

private:
    void recover_direct_nas_segments() {
        if(config_.recording_staging.enabled) {
            return;
        }
        const auto hidden_root = direct_recording_root(config_).lexically_normal();
        const auto publish_root = std::filesystem::path(config_.nas_root).lexically_normal();
        std::error_code ec;
        std::filesystem::create_directories(hidden_root, ec);
        if(ec) {
            throw std::runtime_error("cannot create direct NAS hidden root " + hidden_root.string()
                                     + ": " + ec.message());
        }
        if(!paths_share_device(hidden_root, publish_root)) {
            throw std::runtime_error("direct NAS hidden and publish roots must share one filesystem");
        }

        std::set<std::filesystem::path> ready_segments;
        std::filesystem::recursive_directory_iterator iterator(
            hidden_root,
            std::filesystem::directory_options::skip_permission_denied,
            ec);
        const std::filesystem::recursive_directory_iterator end;
        while(!ec && iterator != end) {
            if(iterator->is_regular_file(ec)) {
                const auto name = iterator->path().filename().string();
                constexpr const char *suffix = "recording_ready.json";
                const size_t suffix_size = std::strlen(suffix);
                if(name.size() >= suffix_size
                   && name.compare(name.size() - suffix_size, suffix_size, suffix) == 0) {
                    std::ifstream marker(iterator->path());
                    const std::string raw((std::istreambuf_iterator<char>(marker)),
                                          std::istreambuf_iterator<char>());
                    Json::Value marker_root;
                    if(marker && parse_json_object_strict(raw, marker_root)
                       && marker_root["ready"].isBool() && marker_root["ready"].asBool()) {
                        ready_segments.insert(iterator->path().parent_path());
                    }
                    else {
                        logger_.warn("direct NAS recovery ignored invalid ready marker: "
                                     + iterator->path().string());
                    }
                }
            }
            iterator.increment(ec);
        }
        if(ec) {
            logger_.warn("direct NAS recovery scan incomplete root=" + hidden_root.string()
                         + " error=" + ec.message());
        }

        size_t recovered = 0;
        for(const auto &source : ready_segments) {
            const auto relative = source.lexically_relative(hidden_root);
            bool safe = !relative.empty() && relative != "..";
            for(const auto &part : relative) {
                safe = safe && is_safe_storage_text(part.string());
            }
            if(!safe) {
                logger_.warn("direct NAS recovery ignored unsafe path: " + source.string());
                continue;
            }
            const auto destination = publish_root / relative;
            ec.clear();
            if(std::filesystem::exists(destination, ec) || ec) {
                logger_.warn("direct NAS recovery retained hidden segment because destination exists: "
                             + source.string());
                continue;
            }
            std::filesystem::create_directories(destination.parent_path(), ec);
            if(ec) {
                logger_.warn("direct NAS recovery cannot create destination parent: " + ec.message());
                continue;
            }
            try {
                fsync_segment_files_strict(source);
            }
            catch(const std::exception &error) {
                logger_.warn("direct NAS recovery fsync failed source=" + source.string()
                             + " error=" + error.what());
                continue;
            }
            std::filesystem::rename(source, destination, ec);
            if(ec) {
                logger_.warn("direct NAS recovery rename failed source=" + source.string()
                             + " destination=" + destination.string() + " error=" + ec.message());
                continue;
            }
            fsync_directory_best_effort(destination.parent_path());
            fsync_directory_best_effort(source.parent_path());
            ++recovered;
            logger_.info("direct NAS recovery published segment: " + destination.string());
        }
        logger_.info("direct NAS recovery completed hidden_root=" + hidden_root.string()
                     + " recovered=" + std::to_string(recovered));
    }

    void persist_runtime_state_snapshot(const RuntimeState &snapshot, uint64_t revision) {
        std::lock_guard<std::mutex> save_lock(runtime_state_save_mutex_);
        if(revision <= runtime_state_save_revision_) {
            return;
        }
        runtime_state_save_revision_ = revision;
        save_runtime_state_file(config_.state_path, snapshot);
    }

    bool required_listeners_ready() const {
        return status_udp_ready_ && media_tcp_ready_ && admin_ready_
               && (!config_.media_udp_enabled || media_udp_ready_)
               && (!(config_.preview_enabled && config_.preview_udp_enabled) || preview_udp_ready_);
    }

    static bool is_safe_photo_relative_path(const std::filesystem::path &path) {
        if(path.empty() || path.is_absolute()) {
            return false;
        }
        for(const auto &part : path) {
            const auto text = part.string();
            if(text.empty() || !is_safe_storage_text(text)) {
                return false;
            }
        }
        return true;
    }

    void start_photo_capture_worker() {
        if(!config_.photo_capture.enabled) {
            logger_.info("receiver photo capture disabled by config");
            return;
        }
        std::error_code ec;
        std::filesystem::create_directories(config_.photo_capture.staging_root, ec);
        if(ec) {
            logger_.warn("receiver photo capture unavailable staging_root=" + config_.photo_capture.staging_root
                         + " error=" + ec.message());
            return;
        }
        fsync_directory_best_effort(std::filesystem::path(config_.photo_capture.staging_root).parent_path());
        {
            std::lock_guard<std::mutex> lock(photo_capture_mutex_);
            photo_capture_stop_ = false;
            photo_capture_available_ = true;
        }
        photo_capture_thread_ = std::thread([this] { photo_capture_worker_loop(); });
        logger_.info("receiver photo capture ready staging_root=" + config_.photo_capture.staging_root
                     + " nas_subdirectory=" + config_.photo_capture.nas_subdirectory
                     + " max_jpeg_bytes=" + std::to_string(config_.photo_capture.max_jpeg_bytes)
                     + " queue_max_items=" + std::to_string(config_.photo_capture.queue_max_items));
    }

    void stop_photo_capture_worker() {
        {
            std::lock_guard<std::mutex> lock(photo_capture_mutex_);
            photo_capture_available_ = false;
            photo_capture_stop_ = true;
        }
        photo_capture_cv_.notify_all();
        if(photo_capture_thread_.joinable()) {
            photo_capture_thread_.join();
        }
    }

    void send_rgb_snapshot_result(const PhotoCaptureJob &job,
                                  bool ok,
                                  const std::string &status,
                                  const std::string &image_path,
                                  const std::string &error) {
        Json::Value root(Json::objectValue);
        root["protocol_version"] = kProtocolVersion;
        root["message_type"] = "control";
        root["control"] = "rgb_snapshot_result";
        root["sender_id"] = job.packet.sender_id;
        root["camera_id"] = job.packet.camera_id;
        root["request_id"] = job.request_id;
        root["ok"] = ok;
        root["status"] = status;
        root["image_path"] = image_path;
        root["error"] = error;
        root["frame_id"] = Json::UInt64(job.packet.frame_id);
        root["frame_system_timestamp_us"] = Json::UInt64(job.packet.system_timestamp_us);
        root["orientation_applied_degrees"] =
            (job.packet.flags & snapshot_orientation_applied) != 0u ? 180 : 0;
        if(const auto burst = rgb_snapshot_burst_info(job.request_id)) {
            root["burst_id"] = burst->group_id;
            root["burst_index"] = burst->index;
            root["burst_count"] = burst->count;
        }
        root["receiver_captured_timestamp_us"] = Json::UInt64(now_us());
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        const auto payload = Json::writeString(builder, root);

        bool sent = false;
        for(int attempt = 0; attempt < 3; ++attempt) {
            sent = send_udp_text_to_endpoint(job.status_endpoint, payload) || sent;
            if(attempt < 2) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        if(!sent) {
            logger_.warn("rgb snapshot acknowledgement send failed request_id=" + job.request_id
                         + " endpoint=" + job.status_endpoint);
        }
    }

    void remember_completed_photo(const std::string &request_id, const std::string &image_path) {
        std::lock_guard<std::mutex> lock(photo_capture_mutex_);
        if(photo_completed_paths_.count(request_id) == 0) {
            photo_completed_order_.push_back(request_id);
        }
        photo_completed_paths_[request_id] = image_path;
        while(photo_completed_order_.size() > 1024) {
            photo_completed_paths_.erase(photo_completed_order_.front());
            photo_completed_order_.pop_front();
        }
    }

    std::optional<std::string> completed_photo_path(const std::string &request_id) {
        std::lock_guard<std::mutex> lock(photo_capture_mutex_);
        const auto found = photo_completed_paths_.find(request_id);
        return found == photo_completed_paths_.end() ? std::nullopt
                                                     : std::optional<std::string>(found->second);
    }

    std::optional<std::string> existing_staged_photo_path(const std::filesystem::path &job_directory) {
        const auto marker_path = job_directory / "photo_ready.json";
        const auto jpeg_path = job_directory / "photo.jpg";
        std::ifstream input(marker_path);
        if(!input || !std::filesystem::is_regular_file(jpeg_path)) {
            return std::nullopt;
        }
        const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        Json::Value root;
        if(!parse_json_object_strict(text, root) || !root["ready"].isBool() || !root["ready"].asBool()
           || !root["relative_path"].isString()) {
            return std::nullopt;
        }
        const std::filesystem::path relative_path = root["relative_path"].asString();
        if(!is_safe_photo_relative_path(relative_path)) {
            return std::nullopt;
        }
        return (std::filesystem::path(config_.nas_root) / relative_path).string();
    }

    std::filesystem::path reserve_photo_relative_path(const MediaPacket &packet,
                                                      uint64_t frame_time_us,
                                                      const std::string &request_id) {
        const std::string date = local_time_text_from_us(frame_time_us, "%Y-%m-%d");
        const std::string time = local_time_text_from_us(frame_time_us, "%H-%M-%S");
        const std::string stem = local_time_text_from_us(frame_time_us, "%Y%m%d_%H%M%S");
        const std::filesystem::path camera_directory =
            std::filesystem::path(config_.photo_capture.nas_subdirectory)
            / camera_key(packet.sender_id, packet.camera_id);

        std::lock_guard<std::mutex> lock(photo_capture_mutex_);
        if(const auto burst = rgb_snapshot_burst_info(request_id)) {
            const std::string burst_key =
                camera_key(packet.sender_id, packet.camera_id) + ":" + burst->group_id;
            auto state_it = photo_burst_paths_.find(burst_key);
            if(state_it == photo_burst_paths_.end()) {
                std::filesystem::path burst_directory;
                for(size_t directory_index = 0; directory_index < 1000; ++directory_index) {
                    std::ostringstream directory_name;
                    directory_name << time;
                    if(directory_index > 0) {
                        directory_name << '_' << std::setw(3) << std::setfill('0') << directory_index;
                    }
                    const auto candidate = camera_directory / date / directory_name.str();
                    if(photo_reserved_directories_.insert(candidate.generic_string()).second) {
                        burst_directory = candidate;
                        break;
                    }
                }
                if(burst_directory.empty()) {
                    throw std::runtime_error("cannot allocate unique photo burst directory");
                }
                state_it = photo_burst_paths_
                               .emplace(burst_key, PhotoBurstPathState{burst_directory, stem, burst->count})
                               .first;
                photo_burst_path_order_.push_back(burst_key);
                while(photo_burst_path_order_.size() > 1024) {
                    photo_burst_paths_.erase(photo_burst_path_order_.front());
                    photo_burst_path_order_.pop_front();
                }
            }
            if(state_it->second.count != burst->count) {
                throw std::runtime_error("photo burst count changed within one burst");
            }
            std::ostringstream filename;
            filename << state_it->second.filename_stem;
            if(burst->index > 1) {
                filename << '_' << std::setw(3) << std::setfill('0') << (burst->index - 1);
            }
            filename << ".jpg";
            const auto candidate = state_it->second.directory / filename.str();
            if(!photo_reserved_relative_paths_.insert(candidate.generic_string()).second) {
                throw std::runtime_error("duplicate photo burst index");
            }
            return candidate;
        }

        const std::filesystem::path directory = camera_directory / date / time;
        photo_reserved_directories_.insert(directory.generic_string());
        for(size_t index = 0; index < 1000; ++index) {
            std::ostringstream filename;
            filename << stem;
            if(index > 0) {
                filename << '_' << std::setw(3) << std::setfill('0') << index;
            }
            filename << ".jpg";
            const auto candidate = directory / filename.str();
            const auto key = candidate.generic_string();
            if(photo_reserved_relative_paths_.insert(key).second) {
                return candidate;
            }
        }
        throw std::runtime_error("cannot allocate unique photo path");
    }

    std::string stage_photo_capture(const PhotoCaptureJob &job) {
        if(auto completed = completed_photo_path(job.request_id)) {
            return *completed;
        }
        const std::filesystem::path staging_root = config_.photo_capture.staging_root;
        const auto final_directory = staging_root / job.request_id;
        if(std::filesystem::exists(final_directory)) {
            if(auto existing = existing_staged_photo_path(final_directory)) {
                remember_completed_photo(job.request_id, *existing);
                return *existing;
            }
            throw std::runtime_error("existing staged photo task is incomplete: " + final_directory.string());
        }

        constexpr uint64_t kEarliestPlausibleEpochUs = 1'577'836'800ull * 1'000'000ull;
        const uint64_t receiver_time_us =
            job.packet.receiver_receive_timestamp_us > 0 ? job.packet.receiver_receive_timestamp_us : now_us();
        const bool sender_time_plausible =
            (job.packet.flags & has_system_timestamp) != 0u
            && job.packet.system_timestamp_us >= kEarliestPlausibleEpochUs
            && job.packet.system_timestamp_us <= receiver_time_us + 24ull * 60ull * 60ull * 1'000'000ull;
        const uint64_t frame_time_us = sender_time_plausible ? job.packet.system_timestamp_us : receiver_time_us;
        const auto relative_path = reserve_photo_relative_path(job.packet, frame_time_us, job.request_id);
        if(!is_safe_photo_relative_path(relative_path)) {
            throw std::runtime_error("generated unsafe photo relative path");
        }

        const uint64_t temp_sequence = photo_temp_sequence_.fetch_add(1) + 1;
        const auto temporary_directory =
            staging_root / ("." + job.request_id + "." + std::to_string(getpid()) + "." + std::to_string(temp_sequence) + ".tmp");
        std::error_code ec;
        std::filesystem::remove_all(temporary_directory, ec);
        ec.clear();
        std::filesystem::create_directories(temporary_directory, ec);
        if(ec) {
            throw std::runtime_error("cannot create temporary photo staging directory: " + ec.message());
        }
        ScopeExit cleanup([&] {
            std::error_code cleanup_error;
            std::filesystem::remove_all(temporary_directory, cleanup_error);
        });

        const auto jpeg_path = temporary_directory / "photo.jpg";
        write_file_and_fsync(jpeg_path, job.packet.payload.data(), job.packet.payload.size());
        const uint32_t jpeg_crc = static_cast<uint32_t>(
            crc32(crc32(0L, Z_NULL, 0), job.packet.payload.data(), static_cast<uInt>(job.packet.payload.size())));

        Json::Value marker(Json::objectValue);
        marker["schema_version"] = 1;
        marker["ready"] = true;
        marker["request_id"] = job.request_id;
        marker["sender_id"] = job.packet.sender_id;
        marker["camera_id"] = job.packet.camera_id;
        marker["frame_id"] = Json::UInt64(job.packet.frame_id);
        marker["frame_timestamp_us"] = Json::UInt64(job.packet.timestamp_us);
        marker["frame_system_timestamp_us"] = Json::UInt64(job.packet.system_timestamp_us);
        marker["receiver_receive_timestamp_us"] = Json::UInt64(job.packet.receiver_receive_timestamp_us);
        marker["global_timestamp_us"] = Json::UInt64(job.packet.global_timestamp_us);
        marker["width"] = job.packet.width;
        marker["height"] = job.packet.height;
        marker["rgb_exposure_us"] = job.packet.rgb_exposure_us;
        marker["rgb_gain"] = job.packet.rgb_gain;
        marker["rgb_auto_exposure"] = job.packet.rgb_auto_exposure;
        const bool orientation_applied = (job.packet.flags & snapshot_orientation_applied) != 0u;
        marker["format"] = orientation_applied ? "jpeg_rotated_180" : "original_mjpeg";
        marker["orientation_applied_degrees"] = orientation_applied ? 180 : 0;
        if(const auto burst = rgb_snapshot_burst_info(job.request_id)) {
            marker["burst_id"] = burst->group_id;
            marker["burst_index"] = burst->index;
            marker["burst_count"] = burst->count;
        }
        marker["jpeg_file"] = "photo.jpg";
        marker["jpeg_size"] = Json::UInt64(job.packet.payload.size());
        marker["jpeg_crc32"] = Json::UInt64(jpeg_crc);
        marker["relative_path"] = relative_path.generic_string();
        marker["captured_at_unix_us"] = Json::UInt64(now_us());
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        const auto marker_text = Json::writeString(builder, marker) + "\n";
        write_text_file_and_fsync(temporary_directory / "photo_ready.json", marker_text);
        fsync_directory_best_effort(temporary_directory);

        std::filesystem::rename(temporary_directory, final_directory, ec);
        if(ec) {
            throw std::runtime_error("cannot publish staged photo task: " + ec.message());
        }
        cleanup.release();
        fsync_directory_best_effort(staging_root);

        const auto image_path = (std::filesystem::path(config_.nas_root) / relative_path).string();
        remember_completed_photo(job.request_id, image_path);
        return image_path;
    }

    void photo_capture_worker_loop() {
        for(;;) {
            PhotoCaptureJob job;
            {
                std::unique_lock<std::mutex> lock(photo_capture_mutex_);
                photo_capture_cv_.wait(lock, [&] { return photo_capture_stop_ || !photo_capture_queue_.empty(); });
                if(photo_capture_queue_.empty()) {
                    if(photo_capture_stop_) {
                        return;
                    }
                    continue;
                }
                job = std::move(photo_capture_queue_.front());
                photo_capture_queue_bytes_ -= std::min(photo_capture_queue_bytes_, job.packet.payload.size());
                photo_capture_queue_.pop_front();
            }

            try {
                const auto image_path = stage_photo_capture(job);
                const uint64_t completed_us = now_us();
                const uint64_t latency_us = job.queued_us > 0 && completed_us >= job.queued_us
                                                ? completed_us - job.queued_us
                                                : 0;
                photo_capture_completed_.fetch_add(1);
                logger_.info("rgb snapshot captured request_id=" + job.request_id
                             + " sender_id=" + job.packet.sender_id
                             + " camera_id=" + job.packet.camera_id
                             + " frame_id=" + std::to_string(job.packet.frame_id)
                             + " jpeg_bytes=" + std::to_string(job.packet.payload.size())
                             + " staging_latency_us=" + std::to_string(latency_us)
                             + " image_path=" + image_path);
                {
                    std::lock_guard<std::mutex> lock(photo_capture_mutex_);
                    photo_capture_pending_ids_.erase(job.request_id);
                }
                send_rgb_snapshot_result(job, true, "captured", image_path, "");
            }
            catch(const std::exception &e) {
                photo_capture_failures_.fetch_add(1);
                logger_.warn("rgb snapshot staging failed request_id=" + job.request_id + " error=" + e.what());
                {
                    std::lock_guard<std::mutex> lock(photo_capture_mutex_);
                    photo_capture_pending_ids_.erase(job.request_id);
                }
                send_rgb_snapshot_result(job, false, "error", "", e.what());
            }
        }
    }

    bool enqueue_photo_capture(MediaPacket packet, const std::string &request_id, const std::string &status_endpoint) {
        PhotoCaptureJob rejected;
        rejected.packet = media_packet_metadata_only(packet);
        rejected.request_id = request_id;
        rejected.status_endpoint = status_endpoint;
        if(!config_.photo_capture.enabled) {
            send_rgb_snapshot_result(rejected, false, "error", "", "receiver photo capture is disabled");
            return false;
        }
        const size_t received_payload_size = packet.payload.size();
        size_t jpeg_size = received_payload_size;
        while(jpeg_size > 0 && packet.payload[jpeg_size - 1] == 0x00) {
            --jpeg_size;
        }
        if(received_payload_size > config_.photo_capture.max_jpeg_bytes || jpeg_size < 4
           || packet.payload.front() != 0xff || packet.payload[1] != 0xd8
           || packet.payload[jpeg_size - 2] != 0xff || packet.payload[jpeg_size - 1] != 0xd9) {
            send_rgb_snapshot_result(rejected, false, "error", "", "invalid or oversized original MJPEG snapshot");
            return false;
        }
        if(jpeg_size != received_payload_size) {
            packet.payload.resize(jpeg_size);
        }

        const size_t payload_size = packet.payload.size();
        std::string rejection;
        std::optional<std::string> completed_path;
        bool duplicate_pending = false;
        {
            std::lock_guard<std::mutex> lock(photo_capture_mutex_);
            const size_t queue_max_bytes =
                config_.photo_capture.max_jpeg_bytes * std::min<size_t>(config_.photo_capture.queue_max_items, 32);
            if(const auto completed = photo_completed_paths_.find(request_id);
               completed != photo_completed_paths_.end()) {
                completed_path = completed->second;
                photo_capture_duplicate_requests_.fetch_add(1);
            }
            else if(photo_capture_pending_ids_.count(request_id) != 0) {
                duplicate_pending = true;
                photo_capture_duplicate_requests_.fetch_add(1);
            }
            else if(!photo_capture_available_ || photo_capture_stop_) {
                rejection = "receiver photo staging is unavailable";
            }
            else if(photo_capture_queue_.size() >= config_.photo_capture.queue_max_items
                    || payload_size > queue_max_bytes || photo_capture_queue_bytes_ > queue_max_bytes - payload_size) {
                rejection = "receiver photo staging queue is full";
            }
            else {
                PhotoCaptureJob job;
                job.packet = std::move(packet);
                job.request_id = request_id;
                job.status_endpoint = status_endpoint;
                job.queued_us = now_us();
                photo_capture_queue_bytes_ += payload_size;
                photo_capture_queue_.push_back(std::move(job));
                photo_capture_pending_ids_.insert(request_id);
                photo_capture_enqueued_.fetch_add(1);
            }
        }
        if(completed_path) {
            send_rgb_snapshot_result(rejected, true, "captured", *completed_path, "");
            return true;
        }
        if(duplicate_pending) {
            return true;
        }
        if(!rejection.empty()) {
            send_rgb_snapshot_result(rejected, false, "error", "", rejection);
            return false;
        }
        photo_capture_cv_.notify_one();
        return true;
    }

    void start_decoder_cleanup_worker() {
        std::lock_guard<std::mutex> lock(decoder_cleanup_mutex_);
        if(decoder_cleanup_running_) {
            return;
        }
        decoder_cleanup_running_ = true;
        decoder_cleanup_thread_ = std::thread([this] {
            for(;;) {
                std::unique_ptr<RgbPreviewDecoder> decoder;
                {
                    std::unique_lock<std::mutex> cleanup_lock(decoder_cleanup_mutex_);
                    decoder_cleanup_cv_.wait(cleanup_lock, [&] {
                        return !decoder_cleanup_running_ || !decoder_cleanup_queue_.empty();
                    });
                    if(decoder_cleanup_queue_.empty()) {
                        if(!decoder_cleanup_running_) {
                            return;
                        }
                        continue;
                    }
                    decoder = std::move(decoder_cleanup_queue_.front());
                    decoder_cleanup_queue_.pop_front();
                }
                decoder->stop();
            }
        });
    }

    void cleanup_rgb_decoder_async(std::unique_ptr<RgbPreviewDecoder> decoder) {
        if(!decoder) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(decoder_cleanup_mutex_);
            if(decoder_cleanup_running_) {
                decoder_cleanup_queue_.push_back(std::move(decoder));
                decoder_cleanup_cv_.notify_one();
                return;
            }
        }
        decoder->stop();
    }

    void stop_decoder_cleanup_worker() {
        {
            std::lock_guard<std::mutex> lock(decoder_cleanup_mutex_);
            decoder_cleanup_running_ = false;
        }
        decoder_cleanup_cv_.notify_all();
        if(decoder_cleanup_thread_.joinable()) {
            decoder_cleanup_thread_.join();
        }
    }

    void reap_completed_client_threads_locked() {
        for(auto it = client_threads_.begin(); it != client_threads_.end();) {
            if(it->done && it->done->load(std::memory_order_acquire)) {
                if(it->thread.joinable()) {
                    it->thread.join();
                }
                it = client_threads_.erase(it);
            }
            else {
                ++it;
            }
        }
    }

    void launch_client_thread(int fd, const std::string &label, std::function<void()> task) {
        std::lock_guard<std::mutex> lock(client_threads_mutex_);
        reap_completed_client_threads_locked();
        client_fds_.insert(fd);
        auto done = std::make_shared<std::atomic<bool>>(false);
        try {
            client_threads_.push_back(ClientThread{});
            auto &client = client_threads_.back();
            client.done = done;
            client.thread = std::thread([this, fd, label, task = std::move(task), done]() mutable {
                try {
                    task();
                }
                catch(const std::exception &e) {
                    logger_.warn(label + " client handler failed: " + e.what());
                }
                catch(...) {
                    logger_.warn(label + " client handler failed: unknown exception");
                }
                {
                    std::lock_guard<std::mutex> client_lock(client_threads_mutex_);
                    client_fds_.erase(fd);
                }
                shutdown(fd, SHUT_RDWR);
                close(fd);
                done->store(true, std::memory_order_release);
            });
        }
        catch(...) {
            if(!client_threads_.empty() && !client_threads_.back().thread.joinable()) {
                client_threads_.pop_back();
            }
            client_fds_.erase(fd);
            close(fd);
            throw;
        }
    }

    void shutdown_client_sockets() {
        std::lock_guard<std::mutex> lock(client_threads_mutex_);
        for(int fd : client_fds_) {
            shutdown(fd, SHUT_RDWR);
        }
    }

    void join_client_threads() {
        std::vector<ClientThread> threads;
        {
            std::lock_guard<std::mutex> lock(client_threads_mutex_);
            threads.swap(client_threads_);
        }
        for(auto &client : threads) {
            if(client.thread.joinable()) {
                client.thread.join();
            }
        }
    }

    static uint64_t camera_last_seen_us(const CameraState &cam) {
        return std::max(cam.last_status_us, cam.last_media_us);
    }

    static bool is_recent_us(uint64_t now, uint64_t timestamp_us, uint64_t timeout_us) {
        return timestamp_us > 0 && (timestamp_us >= now || now - timestamp_us <= timeout_us);
    }

    static bool is_older_than_us(uint64_t now, uint64_t timestamp_us, uint64_t timeout_us) {
        return timestamp_us == 0 || (timestamp_us < now && now - timestamp_us > timeout_us);
    }

    static int64_t age_ms_or_negative(uint64_t now, uint64_t timestamp_us) {
        if(timestamp_us == 0) {
            return -1;
        }
        if(timestamp_us >= now) {
            return 0;
        }
        return static_cast<int64_t>((now - timestamp_us) / 1000ull);
    }

    static std::string media_ingress_key(const MediaPacket &packet) {
        return packet.sender_id + '\x1f' + packet.camera_id + '\x1f' + stream_type_name(packet.stream_type);
    }

    bool claim_media_ingress(const MediaPacket &packet,
                             uint64_t session_id,
                             int fd,
                             const std::string &peer_endpoint) {
        if(session_id == 0) {
            return true;
        }

        const std::string key = media_ingress_key(packet);
        int superseded_fd = -1;
        uint64_t superseded_session = 0;
        std::string superseded_peer;
        bool stale = false;
        {
            std::lock_guard<std::mutex> lock(media_ingress_mutex_);
            auto [it, inserted] = media_ingress_owners_.try_emplace(
                key, MediaIngressOwner{session_id, fd, peer_endpoint});
            if(!inserted) {
                auto &owner = it->second;
                if(owner.session_id == session_id) {
                    owner.fd = fd;
                    owner.peer_endpoint = peer_endpoint;
                }
                else if(session_id < owner.session_id) {
                    stale = true;
                }
                else {
                    superseded_fd = owner.fd;
                    superseded_session = owner.session_id;
                    superseded_peer = owner.peer_endpoint;
                    owner = MediaIngressOwner{session_id, fd, peer_endpoint};
                }
            }
        }

        if(stale) {
            const uint64_t rejected = media_ingress_stale_packets_.fetch_add(1) + 1;
            if(rejected <= 10 || rejected % 100 == 0) {
                logger_.warn("stale media session packet rejected route=" + camera_key(packet.sender_id, packet.camera_id)
                             + " stream=" + stream_type_name(packet.stream_type)
                             + " session=" + std::to_string(session_id)
                             + " current_session_newer=true peer=" + peer_endpoint
                             + " rejected_total=" + std::to_string(rejected));
            }
            return false;
        }

        if(superseded_session != 0) {
            media_ingress_superseded_sessions_.fetch_add(1);
            logger_.warn("media session superseded route=" + camera_key(packet.sender_id, packet.camera_id)
                         + " stream=" + stream_type_name(packet.stream_type)
                         + " old_session=" + std::to_string(superseded_session)
                         + " new_session=" + std::to_string(session_id)
                         + " old_peer=" + superseded_peer + " new_peer=" + peer_endpoint);
            if(superseded_fd >= 0 && superseded_fd != fd) {
                shutdown(superseded_fd, SHUT_RDWR);
            }
        }
        return true;
    }

    void mark_media_ingress_session_closed(uint64_t session_id) {
        if(session_id == 0) {
            return;
        }
        std::lock_guard<std::mutex> lock(media_ingress_mutex_);
        for(auto &item : media_ingress_owners_) {
            if(item.second.session_id == session_id) {
                item.second.fd = -1;
            }
        }
    }

    bool media_ingress_session_is_current(const std::string &key, uint64_t session_id) {
        if(session_id == 0) {
            return true;
        }
        std::lock_guard<std::mutex> lock(media_ingress_mutex_);
        const auto owner = media_ingress_owners_.find(key);
        return owner != media_ingress_owners_.end() && owner->second.session_id == session_id;
    }

    bool bind_sender_source_locked(const std::string &sender_id, const std::string &peer_endpoint, uint64_t now) {
        const auto source_ip = socket_endpoint_ip(peer_endpoint);
        if(source_ip.empty()) {
            return false;
        }
        auto source = sender_source_ips_.find(sender_id);
        if(source == sender_source_ips_.end()) {
            if(sender_source_ips_.size() >= kMaxTrackedSenders) {
                return false;
            }
            sender_source_ips_[sender_id] = source_ip;
            return true;
        }
        if(source->second == source_ip) {
            return true;
        }
        const bool old_source_still_live = std::any_of(cameras_.begin(), cameras_.end(), [&](const auto &item) {
            return item.second->sender_id == sender_id
                   && is_recent_us(now, camera_last_seen_us(*item.second), kCameraOnlineTimeoutUs);
        });
        if(old_source_still_live) {
            return false;
        }
        logger_.warn("sender source IP changed after timeout sender_id=" + sender_id + " old=" + source->second + " new=" + source_ip);
        source->second = source_ip;
        return true;
    }

    void clear_camera_live_cache_locked(CameraState &cam) {
        std::lock_guard<std::mutex> preview_lock(cam.preview_mutex);
        cam.rgb_preview_prefix_h264.clear();
        cleanup_rgb_decoder_async(std::move(cam.rgb_decoder));
        cleanup_rgb_decoder_async(std::move(cam.main_rgb_decoder));
        {
            std::lock_guard<std::mutex> stream_lock(cam.rgb_stream.mutex);
            cam.rgb_stream.packets.clear();
            cam.rgb_stream.header_h264.clear();
            cam.rgb_stream.last_us = 0;
            cam.rgb_stream.width = 0;
            cam.rgb_stream.height = 0;
        }
        cam.rgb_stream.cv.notify_all();
        {
            std::lock_guard<std::mutex> stream_lock(cam.rgb_preview_stream.mutex);
            cam.rgb_preview_stream.packets.clear();
            cam.rgb_preview_stream.header_h264.clear();
            cam.rgb_preview_stream.last_us = 0;
            cam.rgb_preview_stream.width = 0;
            cam.rgb_preview_stream.height = 0;
        }
        cam.rgb_preview_stream.cv.notify_all();
        cam.rgb_preview_requested_until_us = 0;
        cam.rgb_stream_requested_until_us = 0;
        cam.rgb_main_stream_requested_until_us = 0;
        cam.rgb_preview_us = 0;
        cam.rgb_preview_width = 0;
        cam.rgb_preview_height = 0;
        cam.main_rgb_preview_requested_until_us = 0;
        cam.main_rgb_preview_us = 0;
        cam.main_rgb_preview_width = 0;
        cam.main_rgb_preview_height = 0;
        cam.depth_preview_ppm.clear();
        cam.depth_preview_requested_until_us = 0;
        cam.depth_preview_us = 0;
        cam.depth_preview_width = 0;
        cam.depth_preview_height = 0;
    }

    void refresh_camera_liveness_locked(uint64_t now) {
        for(auto it = cameras_.begin(); it != cameras_.end();) {
            auto &cam = *it->second;
            const auto last_seen = camera_last_seen_us(cam);
            if(is_older_than_us(now, last_seen, kCameraOnlineTimeoutUs)) {
                if(cam.online) {
                    cam.online = false;
                    cam.last_error = cam.last_error.empty() ? "receiver_timeout" : cam.last_error;
                    logger_.info("camera timed out: " + cam.key);
                }
                clear_camera_live_cache_locked(cam);
            }

            if(!cam.online && !cam.recording_requested && !cam.segment_active && !cam.record_worker_started &&
               is_older_than_us(now, last_seen, kOfflineCameraPurgeUs)) {
                logger_.info("camera purged: " + cam.key);
                it = cameras_.erase(it);
                continue;
            }
            ++it;
        }
    }

    void update_h264_stream_buffer_locked(H264StreamBuffer &stream, const MediaPacket &packet, bool has_idr, bool has_vcl) {
        if(packet.payload.empty()) {
            return;
        }

        {
            std::lock_guard<std::mutex> stream_lock(stream.mutex);
            const auto non_vcl_prefix = h264_non_vcl_prefix(packet.payload);
            if(!non_vcl_prefix.empty() && h264_payload_has_sps_and_pps(non_vcl_prefix)) {
                stream.header_h264 = non_vcl_prefix;
            }
            else if(!has_vcl) {
                if(stream.header_h264.size() + packet.payload.size() > kRgbH264StreamMaxHeaderBytes) {
                    stream.header_h264.clear();
                }
                stream.header_h264.insert(stream.header_h264.end(), packet.payload.begin(), packet.payload.end());
            }
            stream.packets.push_back(
                H264StreamPacket{stream.next_seq++, has_idr, has_vcl, packet.system_timestamp_us,
                                 packet.global_timestamp_us, packet.clock_sync_valid, packet.width, packet.height, packet.payload});
            while(stream.packets.size() > kRgbH264StreamMaxPackets) {
                stream.packets.pop_front();
            }
            stream.last_us = now_us();
            stream.width = packet.width;
            stream.height = packet.height;
        }
        stream.cv.notify_all();
    }

    std::shared_ptr<CameraState> ensure_camera_ptr_locked(const std::string &sender_id, const std::string &camera_id,
                                                          bool mark_online = true) {
        const auto key = camera_key(sender_id, camera_id);
        auto it = cameras_.find(key);
        if(it != cameras_.end() && (it->second->sender_id != sender_id || it->second->camera_id != camera_id)) {
            throw std::runtime_error("camera key collision for " + key);
        }
        if(it == cameras_.end()) {
            if(cameras_.size() >= kMaxTrackedCameras) {
                throw std::runtime_error("maximum tracked camera count reached");
            }
            auto state = std::make_shared<CameraState>(sender_id, camera_id);
            const auto name = runtime_state_.camera_names.find(key);
            if(name != runtime_state_.camera_names.end()) {
                state->camera_name = name->second;
            }
            const auto prefix = runtime_state_.camera_file_prefixes.find(key);
            if(prefix != runtime_state_.camera_file_prefixes.end()) {
                state->camera_file_prefix = prefix->second;
            }
            const auto announce = runtime_state_.camera_announces.find(key);
            if(announce != runtime_state_.camera_announces.end()) {
                state->last_announce_json = announce->second;
            }
            state->depth_scale = depth_scale_from_announce_or_camera(state->last_announce_json, sender_id, camera_id);
            state->recording_requested = recording_all_;
            if(recording_all_) {
                state->recording_start_us = recording_all_start_us_;
                state->recording_window = {recording_all_session_id_, recording_all_start_us_, 0};
                state->recording_file_prefix = effective_file_prefix_locked(*state);
                state->recording_start_pending = recording_all_start_pending_ || recording_all_start_us_ == 0;
                state->record_accepting = !state->recording_start_pending;
                state->record_generation = state->record_accepting ? 1 : 0;
            }
            it = cameras_.emplace(key, std::move(state)).first;
            logger_.info("camera discovered: " + key);
        }
        if(mark_online) {
            it->second->online = true;
        }
        return it->second;
    }

    CameraState &ensure_camera_locked(const std::string &sender_id, const std::string &camera_id) {
        return *ensure_camera_ptr_locked(sender_id, camera_id);
    }

    void handle_status_message(const std::string &payload, const std::string &peer_endpoint) {
        const auto json = trim_copy(payload);
        Json::Value root;
        if(!parse_json_object_strict(json, root)) {
            logger_.warn("malformed status JSON ignored from=" + peer_endpoint);
            return;
        }
        const auto type = json_string_value(root, "message_type", "unknown");
        const auto sender_id = json_string_value(root, "sender_id");
        const auto camera_id = json_string_value(root, "camera_id");
        if(json_string_value(root, "protocol_version") != kProtocolVersion) {
            logger_.warn("status protocol version mismatch ignored from=" + peer_endpoint);
            return;
        }
        static const std::set<std::string> supported_types = {
            "sender_hello", "heartbeat", "clock_sync_report", "camera_announce", "camera_offline", "event"};
        if(supported_types.count(type) == 0) {
            logger_.warn("unsupported status message ignored type=" + type + " from=" + peer_endpoint);
            return;
        }
        bool should_log_status = type != "heartbeat" && type != "sender_hello";

        if(!is_valid_protocol_id(sender_id) || (!camera_id.empty() && !is_valid_protocol_id(camera_id))) {
            logger_.warn("status packet with invalid sender_id/camera_id ignored from=" + peer_endpoint);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(!bind_sender_source_locked(sender_id, peer_endpoint, now_us())) {
                logger_.warn("status sender source mismatch ignored sender_id=" + sender_id + " from=" + peer_endpoint);
                return;
            }
        }

        if(!sender_id.empty() && (type == "heartbeat" || type == "clock_sync_report")) {
            const bool clock_valid = root["clock_sync_valid"].isBool() && root["clock_sync_valid"].asBool();
            const auto offset_us = json_int64_value(root, "clock_offset_us").value_or(0);
            const auto delay_us = json_int64_value(root, "clock_delay_us").value_or(0);
            const auto drift_ppm = json_double_value(root, "clock_drift_ppm").value_or(0.0);
            const auto last_sync_us = json_uint64_value(root, "clock_last_sync_us").value_or(0);
            // A single missed response must not erase the last accepted model. The
            // manager's report/probe timeout owns model expiry and bounded holdover.
            if(clock_valid && last_sync_us > 0) {
                clock_sync_manager_.update_from_sender_report(sender_id, offset_us, delay_us, drift_ppm, last_sync_us,
                                                              socket_endpoint_ip(peer_endpoint));
            }
        }

        std::optional<RuntimeState> state_snapshot;
        uint64_t state_revision = 0;
        if(!sender_id.empty() && !camera_id.empty()) {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto key = camera_key(sender_id, camera_id);
            const auto existing = cameras_.find(key);
            if(existing != cameras_.end()
               && (existing->second->sender_id != sender_id || existing->second->camera_id != camera_id)) {
                logger_.warn("status camera identity collision ignored key=" + key + " from=" + peer_endpoint);
                return;
            }
            const auto code = type == "event" ? json_string_value(root, "event_code", "event") : "";
            const bool heartbeat_online = type == "heartbeat" && root["online"].isBool() && root["online"].asBool();
            const bool marks_online = type == "camera_announce" || heartbeat_online || code == "camera_connected"
                                      || code == "camera_reconnected";
            auto &cam = *ensure_camera_ptr_locked(sender_id, camera_id, marks_online);
            const auto received_us = now_us();
            cam.last_status_us = received_us;
            cam.status_endpoint = peer_endpoint;
            cam.sender_build_commit = json_string_value(root, "build_commit", cam.sender_build_commit);
            cam.sender_build_source_hash = json_string_value(root, "build_source_hash", cam.sender_build_source_hash);
            if(root["build_dirty"].isBool()) {
                cam.sender_build_dirty = root["build_dirty"].asBool();
            }
            if(type == "heartbeat") {
                cam.sender_rgb_input_fps = json_double_value(root, "rgb_measured_fps").value_or(cam.sender_rgb_input_fps);
                cam.sender_depth_input_fps = json_double_value(root, "depth_measured_fps").value_or(cam.sender_depth_input_fps);
                cam.sender_rgb_sent_fps = json_double_value(root, "rgb_sent_fps").value_or(cam.sender_rgb_sent_fps);
                cam.sender_depth_sent_fps = json_double_value(root, "depth_sent_fps").value_or(cam.sender_depth_sent_fps);
                cam.sender_rgb_dropped_frames =
                    json_uint64_value(root, "rgb_dropped_frames").value_or(cam.sender_rgb_dropped_frames);
                cam.sender_depth_dropped_frames =
                    json_uint64_value(root, "depth_dropped_frames").value_or(cam.sender_depth_dropped_frames);
                cam.sender_rgb_transport_retry_drops =
                    json_uint64_value(root, "rgb_transport_retry_drops").value_or(cam.sender_rgb_transport_retry_drops);
                cam.sender_rgb_send_failures =
                    json_uint64_value(root, "rgb_send_failures_total").value_or(cam.sender_rgb_send_failures);
                cam.sender_depth_send_failures =
                    json_uint64_value(root, "depth_send_failures_total").value_or(cam.sender_depth_send_failures);
                if(root["publish_warmup_active"].isBool()) {
                    cam.sender_publish_warmup_active = root["publish_warmup_active"].asBool();
                }
                cam.sender_publish_warmup_drops =
                    json_uint64_value(root, "publish_warmup_dropped_framesets").value_or(cam.sender_publish_warmup_drops);
                const auto sender_error = json_string_value(root, "last_error");
                const bool preserve_recording_error = cam.last_error.rfind("recording_", 0) == 0;
                const bool media_live = is_recent_us(received_us, cam.last_media_us, kCameraOnlineTimeoutUs);
                const bool recovered_transport_error =
                    heartbeat_online && media_live && is_recovered_media_transport_error(sender_error);
                if(!sender_error.empty() && !recovered_transport_error && !preserve_recording_error) {
                    cam.last_error = sender_error;
                }
                else if(heartbeat_online && !preserve_recording_error
                        && (sender_error.empty() || recovered_transport_error)) {
                    cam.last_error.clear();
                }
            }
            if(type == "heartbeat" || type == "camera_announce" || type == "camera_offline") {
                should_log_status = cam.last_status_log_us == 0 ||
                                    received_us >= cam.last_status_log_us + kRoutineStatusLogMinIntervalUs;
                if(should_log_status) {
                    cam.last_status_log_us = received_us;
                }
            }
            if(type == "camera_announce") {
                cam.last_announce_json = json;
                cam.last_announce_live = true;
                cam.last_announce_received_us = received_us;
                if(cam.last_error.rfind("recording_", 0) != 0) {
                    cam.last_error.clear();
                }
                cam.depth_scale = depth_scale_from_announce_or_camera(cam.last_announce_json, sender_id, camera_id);
                const bool should_save_announce =
                    (runtime_state_.camera_announces.find(key) == runtime_state_.camera_announces.end() ||
                     runtime_state_.camera_announces[key] != json) &&
                    (cam.last_announce_cache_save_us == 0 ||
                     received_us >= cam.last_announce_cache_save_us + kAnnounceCacheSaveMinIntervalUs);
                if(should_save_announce) {
                    runtime_state_.camera_announces[key] = json;
                    cam.last_announce_cache_save_us = received_us;
                    state_revision = ++runtime_state_revision_;
                    state_snapshot = runtime_state_;
                }
            }
            else if(type == "camera_offline") {
                cam.online = false;
                cam.last_announce_live = false;
                cam.last_announce_received_us = 0;
                clear_camera_live_cache_locked(cam);
                cam.last_error = json_string_value(root, "reason", "camera_offline");
            }
            else if(type == "event") {
                const auto message = json_string_value(root, "message");
                const auto level = json_string_value(root, "level", "warning");
                if(level != "info") {
                    cam.last_error = code + (message.empty() ? "" : ": " + message);
                }
                else if((code == "camera_connected" || code == "camera_reconnected"
                         || code == "capture_warmup_complete")
                        && cam.last_error.rfind("recording_", 0) != 0) {
                    cam.last_error.clear();
                }
                if(code == "camera_unavailable" || code == "camera_disconnected") {
                    cam.online = false;
                    cam.last_announce_live = false;
                    cam.last_announce_received_us = 0;
                    clear_camera_live_cache_locked(cam);
                }
            }
        }

        if(state_snapshot) {
            try {
                persist_runtime_state_snapshot(*state_snapshot, state_revision);
            }
            catch(const std::exception &e) {
                logger_.error(e.what());
            }
        }

        if(should_log_status) {
            logger_.info("status " + type + " from=" + peer_endpoint + " sender=" + sender_id
                         + (camera_id.empty() ? "" : " camera=" + camera_id));
        }
    }

    bool feed_rgb_preview_decoder_locked(CameraState &cam,
                                         const MediaPacket &packet,
                                         bool has_idr,
                                         std::unique_ptr<RgbPreviewDecoder> &decoder,
                                         uint32_t target_width,
                                         uint32_t preview_fps,
                                         const std::string &decoder_key,
                                         uint32_t &preview_width,
                                         uint32_t &preview_height,
                                         uint64_t &preview_us) {
        if(decoder && !decoder->active()) {
            cleanup_rgb_decoder_async(std::move(decoder));
            preview_width = 0;
            preview_height = 0;
            preview_us = 0;
        }
        if(!decoder) {
            if(!has_idr) {
                return false;
            }
            decoder = std::make_unique<RgbPreviewDecoder>();
            if(!decoder->start(config_, decoder_key, packet.width, packet.height, target_width, preview_fps,
                               rgb_h264_full_range_for_camera(config_, cam.sender_id, cam.camera_id), logger_)) {
                cleanup_rgb_decoder_async(std::move(decoder));
                preview_width = 0;
                preview_height = 0;
                preview_us = 0;
                return false;
            }
            auto decoder_prefix = cam.rgb_preview_prefix_h264;
            const auto packet_prefix = h264_non_vcl_prefix(packet.payload);
            if(!packet_prefix.empty() && h264_payload_has_sps_and_pps(packet_prefix)) {
                decoder_prefix = packet_prefix;
            }
            if(!decoder_prefix.empty()) {
                if(!decoder->write_packet(decoder_prefix)) {
                    cleanup_rgb_decoder_async(std::move(decoder));
                    preview_width = 0;
                    preview_height = 0;
                    preview_us = 0;
                    return false;
                }
            }
        }

        if(!decoder->write_packet(packet.payload)) {
            cleanup_rgb_decoder_async(std::move(decoder));
            preview_width = 0;
            preview_height = 0;
            preview_us = 0;
            return false;
        }
        preview_width = decoder->preview_width();
        preview_height = decoder->preview_height();
        preview_us = decoder->frame_us();
        return true;
    }

    void update_rgb_preview_locked(CameraState &cam,
                                   const MediaPacket &packet,
                                   bool has_idr,
                                   bool has_vcl,
                                   bool thumbnail_requested,
                                   bool thumbnail_expired,
                                   bool main_requested,
                                   bool main_expired,
                                   bool is_main_camera) {
        if(!config_.preview_enabled) {
            cam.rgb_preview_prefix_h264.clear();
            cleanup_rgb_decoder_async(std::move(cam.rgb_decoder));
            cleanup_rgb_decoder_async(std::move(cam.main_rgb_decoder));
            cam.rgb_preview_requested_until_us = 0;
            cam.rgb_stream_requested_until_us = 0;
            cam.rgb_main_stream_requested_until_us = 0;
            cam.rgb_preview_us = 0;
            cam.rgb_preview_width = 0;
            cam.rgb_preview_height = 0;
            cam.main_rgb_preview_requested_until_us = 0;
            cam.main_rgb_preview_us = 0;
            cam.main_rgb_preview_width = 0;
            cam.main_rgb_preview_height = 0;
            return;
        }
        if(packet.payload.empty()) {
            return;
        }
        if(cam.rgb_preview_decoder_source != packet.stream_type) {
            cam.rgb_preview_decoder_source = packet.stream_type;
            cam.rgb_preview_prefix_h264.clear();
            cleanup_rgb_decoder_async(std::move(cam.rgb_decoder));
            cleanup_rgb_decoder_async(std::move(cam.main_rgb_decoder));
            cam.rgb_preview_width = 0;
            cam.rgb_preview_height = 0;
            cam.rgb_preview_us = 0;
            cam.main_rgb_preview_width = 0;
            cam.main_rgb_preview_height = 0;
            cam.main_rgb_preview_us = 0;
        }

        if(!has_vcl) {
            if(cam.rgb_preview_prefix_h264.size() + packet.payload.size() > kMaxRgbPreviewPrefixBytes) {
                cam.rgb_preview_prefix_h264.clear();
            }
            cam.rgb_preview_prefix_h264.insert(cam.rgb_preview_prefix_h264.end(), packet.payload.begin(), packet.payload.end());
            return;
        }

        if(thumbnail_requested) {
            feed_rgb_preview_decoder_locked(cam, packet, has_idr, cam.rgb_decoder, kRgbPreviewWidth, kRgbPreviewFps, cam.key, cam.rgb_preview_width,
                                            cam.rgb_preview_height, cam.rgb_preview_us);
        }
        else if(cam.rgb_decoder && thumbnail_expired) {
            cleanup_rgb_decoder_async(std::move(cam.rgb_decoder));
            cam.rgb_preview_width = 0;
            cam.rgb_preview_height = 0;
            cam.rgb_preview_us = 0;
        }

        if(kEnableJpegMainPreview && is_main_camera && main_requested) {
            feed_rgb_preview_decoder_locked(cam, packet, has_idr, cam.main_rgb_decoder, kRgbMainPreviewWidth, kRgbMainPreviewFps, cam.key + ":main",
                                            cam.main_rgb_preview_width, cam.main_rgb_preview_height, cam.main_rgb_preview_us);
        }
        else if(cam.main_rgb_decoder && (!is_main_camera || main_expired)) {
            cleanup_rgb_decoder_async(std::move(cam.main_rgb_decoder));
            cam.main_rgb_preview_width = 0;
            cam.main_rgb_preview_height = 0;
            cam.main_rgb_preview_us = 0;
        }
    }

    void handle_media_packet(MediaPacket packet,
                             const std::string &peer_endpoint,
                             uint64_t media_session_id = 0,
                             int media_fd = -1) {
        if(!is_valid_protocol_id(packet.sender_id) || !is_valid_protocol_id(packet.camera_id)) {
            logger_.warn("media packet with invalid sender_id/camera_id ignored");
            return;
        }
        const uint64_t packet_receive_us = now_us();
        packet.receiver_receive_timestamp_us = packet_receive_us;
        const auto clock_model = clock_sync_manager_.get_model(packet.sender_id);
        const bool sender_system_time_available = (packet.flags & has_system_timestamp) != 0u && packet.system_timestamp_us > 0;
        packet.clock_sync_valid = false;
        packet.sender_offset_us = clock_model.offset_us;
        packet.sender_delay_us = clock_model.delay_us;
        packet.sender_drift_ppm = clock_model.drift_ppm;
        // The offset model maps sender system time to receiver time.
        const uint64_t fallback_timestamp_us = sender_system_time_available ? packet.system_timestamp_us : packet.timestamp_us;
        packet.global_timestamp_us = fallback_timestamp_us;
        if(clock_model.valid && sender_system_time_available) {
            const int64_t candidate = clock_sync_manager_.get_global_timestamp_us(packet.sender_id, packet.system_timestamp_us);
            if(candidate > 0) {
                const uint64_t candidate_us = static_cast<uint64_t>(candidate);
                const uint64_t receiver_skew_us = candidate_us >= packet_receive_us ? candidate_us - packet_receive_us
                                                                                     : packet_receive_us - candidate_us;
                if(receiver_skew_us <= kMaxGlobalTimestampReceiverSkewUs) {
                    packet.clock_sync_valid = true;
                    packet.global_timestamp_us = candidate_us;
                }
            }
        }

        const bool rgb_stream_packet = packet.stream_type == StreamType::rgb || packet.stream_type == StreamType::rgb_preview;
        const bool rgb_has_idr = rgb_stream_packet &&
                                 (((packet.flags & key_frame) != 0u) || h264_payload_has_nal_type(packet.payload, 5));
        const bool rgb_has_vcl = rgb_stream_packet && h264_payload_has_vcl_nal(packet.payload);
        if(!claim_media_ingress(packet, media_session_id, media_fd, peer_endpoint)) {
            return;
        }
        if(packet.stream_type == StreamType::rgb_snapshot) {
            const auto request_id = rgb_snapshot_request_id(packet.codec_or_compression);
            if(!request_id) {
                logger_.warn("rgb snapshot packet ignored because request_id is invalid");
                return;
            }
            std::string status_endpoint;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if(!bind_sender_source_locked(packet.sender_id, peer_endpoint, packet_receive_us)) {
                    logger_.warn("rgb snapshot sender source mismatch ignored sender_id=" + packet.sender_id
                                 + " from=" + peer_endpoint);
                    return;
                }
                try {
                    auto snapshot_camera = ensure_camera_ptr_locked(packet.sender_id, packet.camera_id);
                    snapshot_camera->last_media_us = packet_receive_us;
                    snapshot_camera->last_media_session_id = media_session_id;
                    status_endpoint = snapshot_camera->status_endpoint;
                    if(status_endpoint.empty()) {
                        for(const auto &item : cameras_) {
                            if(item.second->sender_id == packet.sender_id && !item.second->status_endpoint.empty()) {
                                status_endpoint = item.second->status_endpoint;
                                break;
                            }
                        }
                    }
                }
                catch(const std::exception &e) {
                    logger_.warn(std::string("rgb snapshot identity rejected: ") + e.what());
                    return;
                }
            }
            enqueue_photo_capture(std::move(packet), *request_id, status_endpoint);
            return;
        }

        std::shared_ptr<CameraState> cam;
        bool build_depth_preview = false;
        uint64_t depth_preview_media_us = 0;
        double depth_preview_scale = fallback_depth_scale_for_camera(packet.sender_id, packet.camera_id);
        std::vector<SenderControlTarget> web_preview_control_targets;
        uint64_t web_preview_control_request_us = 0;
        H264StreamBuffer *rgb_stream_update = nullptr;
        bool update_rgb_preview = false;
        bool thumbnail_preview_requested = false;
        bool thumbnail_preview_expired = false;
        bool main_preview_requested = false;
        bool main_preview_expired = false;
        bool is_main_preview_camera = false;
        bool should_record = false;
        bool drop_rgb_until_idr = false;
        std::optional<SenderControlTarget> rgb_recovery_keyframe_target;
        std::string record_sender_id;
        std::string record_camera_id;
        std::string record_camera_name;
        std::string record_storage_key;
        std::string record_file_prefix;
        std::string record_announce_json;
        RecordingWindow record_window;
        uint64_t record_generation = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(!bind_sender_source_locked(packet.sender_id, peer_endpoint, packet_receive_us)) {
                logger_.warn("media sender source mismatch ignored sender_id=" + packet.sender_id + " from=" + peer_endpoint);
                return;
            }
            try {
                cam = ensure_camera_ptr_locked(packet.sender_id, packet.camera_id);
            }
            catch(const std::exception &e) {
                logger_.warn(std::string("media packet identity rejected: ") + e.what());
                return;
            }
            cam->last_media_us = packet_receive_us;
            cam->last_media_session_id = media_session_id;
            if(packet.stream_type == StreamType::rgb) {
                cam->rgb_packets++;
                cam->rgb_bytes += packet.payload_size;
                if(media_session_id != 0 && cam->rgb_ingress_session_id != media_session_id) {
                    cam->rgb_ingress_session_id = media_session_id;
                    cam->rgb_ingress_waiting_for_idr = !rgb_has_idr;
                    if(cam->rgb_ingress_waiting_for_idr && !cam->status_endpoint.empty()) {
                        ++cam->rgb_ingress_keyframe_requests;
                        rgb_recovery_keyframe_target =
                            SenderControlTarget{cam->sender_id, cam->camera_id, cam->status_endpoint};
                    }
                }
                if(cam->rgb_ingress_waiting_for_idr) {
                    if(rgb_has_idr) {
                        cam->rgb_ingress_waiting_for_idr = false;
                        ++cam->rgb_ingress_recoveries;
                        logger_.info("rgb ingress recovered on IDR route=" + cam->key
                                     + " session=" + std::to_string(media_session_id)
                                     + " dropped=" + std::to_string(cam->rgb_ingress_keyframe_drops));
                    }
                    else {
                        drop_rgb_until_idr = true;
                        ++cam->rgb_ingress_keyframe_drops;
                    }
                }
                if(config_.preview_enabled && !drop_rgb_until_idr) {
                    const auto media_now = cam->last_media_us;
                    const bool stream_requested = is_recent_us(media_now, cam->rgb_stream_requested_until_us, 0);
                    const bool main_stream_requested = is_recent_us(media_now, cam->rgb_main_stream_requested_until_us, 0);
                    const bool thumbnail_requested = is_recent_us(media_now, cam->rgb_preview_requested_until_us, 0);
                    if(stream_requested || main_stream_requested) {
                        rgb_stream_update = &cam->rgb_stream;
                    }
                    const bool preview_stream_fresh = is_recent_us(media_now, cam->last_rgb_preview_packet_us, kPreviewFreshUs);
                    update_rgb_preview = !preview_stream_fresh;
                    thumbnail_preview_requested = thumbnail_requested;
                    thumbnail_preview_expired = is_older_than_us(media_now, cam->rgb_preview_requested_until_us, kPreviewDecoderIdleStopUs);
                    is_main_preview_camera = cam->key == main_preview_key_;
                    main_preview_requested = is_main_preview_camera && is_recent_us(media_now, cam->main_rgb_preview_requested_until_us, 0);
                    main_preview_expired = is_older_than_us(media_now, cam->main_rgb_preview_requested_until_us, kMainPreviewDecoderIdleStopUs);
                    if(auto target = maybe_web_rgb_preview_control_target_locked(*cam, media_now)) {
                        web_preview_control_request_us = media_now;
                        web_preview_control_targets.push_back(*target);
                    }
                }
            }
            else if(packet.stream_type == StreamType::rgb_preview) {
                cam->last_rgb_preview_packet_us = cam->last_media_us;
                if(config_.preview_enabled) {
                    const auto media_now = cam->last_media_us;
                    const bool stream_requested = is_recent_us(media_now, cam->rgb_stream_requested_until_us, 0);
                    const bool thumbnail_requested = is_recent_us(media_now, cam->rgb_preview_requested_until_us, 0);
                    if(stream_requested) {
                        rgb_stream_update = &cam->rgb_preview_stream;
                    }
                    update_rgb_preview = true;
                    thumbnail_preview_requested = thumbnail_requested;
                    thumbnail_preview_expired = is_older_than_us(media_now, cam->rgb_preview_requested_until_us, kPreviewDecoderIdleStopUs);
                    is_main_preview_camera = cam->key == main_preview_key_;
                    main_preview_requested = is_main_preview_camera && is_recent_us(media_now, cam->main_rgb_preview_requested_until_us, 0);
                    main_preview_expired = is_older_than_us(media_now, cam->main_rgb_preview_requested_until_us, kMainPreviewDecoderIdleStopUs);
                    if(auto target = maybe_web_rgb_preview_control_target_locked(*cam, media_now)) {
                        web_preview_control_request_us = media_now;
                        web_preview_control_targets.push_back(*target);
                    }
                }
            }
            else if(packet.stream_type == StreamType::depth_raw) {
                cam->depth_packets++;
                cam->depth_bytes += packet.payload_size;
                build_depth_preview = config_.preview_enabled && is_recent_us(cam->last_media_us, cam->depth_preview_requested_until_us, 0);
                depth_preview_media_us = cam->last_media_us;
                depth_preview_scale = cam->depth_scale;
            }

            should_record = !drop_rgb_until_idr && (recording_all_ || cam->recording_requested)
                            && (packet.stream_type == StreamType::rgb || packet.stream_type == StreamType::depth_raw);
            if(should_record) {
                {
                    std::lock_guard<std::mutex> record_lock(cam->record_mutex);
                    if(!cam->record_accepting) {
                        should_record = false;
                    }
                    else {
                        record_generation = cam->record_generation;
                    }
                }
            }
            if(should_record) {
                if(cam->recording_start_us == 0) {
                    cam->recording_start_us = recording_all_ ? recording_all_start_us_ : now_us();
                    if(cam->recording_window.session_id == 0) {
                        cam->recording_window = {next_recording_session_id_locked(), cam->recording_start_us, 0};
                    }
                    cam->recording_file_prefix = effective_file_prefix_locked(*cam);
                }
                record_sender_id = cam->sender_id;
                record_camera_id = cam->camera_id;
                record_camera_name = cam->camera_name;
                record_storage_key = cam->storage_key();
                record_file_prefix = cam->recording_file_prefix;
                record_announce_json = cam->last_announce_live ? cam->last_announce_json : "";
                record_window = cam->recording_window;
            }
        }
        if(rgb_recovery_keyframe_target) {
            send_force_rgb_keyframe_controls({*rgb_recovery_keyframe_target},
                                             "rgb_ingress_session_recovery", packet_receive_us);
        }
        if(drop_rgb_until_idr) {
            return;
        }
        std::shared_ptr<MediaPacket> packet_owner;
        MediaPacket *processing_packet = &packet;
        if(should_record) {
            packet_owner = std::make_shared<MediaPacket>(std::move(packet));
            processing_packet = packet_owner.get();

            RecordJob job;
            job.packet = packet_owner;
            job.sender_id = std::move(record_sender_id);
            job.camera_id = std::move(record_camera_id);
            job.camera_name = std::move(record_camera_name);
            job.storage_key = std::move(record_storage_key);
            job.file_prefix = std::move(record_file_prefix);
            job.announce_json = std::move(record_announce_json);
            job.recording_window = record_window;
            job.record_generation = record_generation;
            job.media_session_id = media_session_id;
            job.media_ingress_key = media_ingress_key(*processing_packet);
            enqueue_record_job(cam, std::move(job));
        }
        const MediaPacket &media_packet = *processing_packet;

        if(rgb_stream_update) {
            update_h264_stream_buffer_locked(*rgb_stream_update, media_packet, rgb_has_idr, rgb_has_vcl);
        }
        if(update_rgb_preview) {
            std::lock_guard<std::mutex> preview_lock(cam->preview_mutex);
            update_rgb_preview_locked(*cam, media_packet, rgb_has_idr, rgb_has_vcl,
                                      thumbnail_preview_requested, thumbnail_preview_expired,
                                      main_preview_requested, main_preview_expired, is_main_preview_camera);
        }
        if(!web_preview_control_targets.empty()) {
            send_web_rgb_preview_controls(web_preview_control_targets, web_preview_control_request_us);
        }

        if(build_depth_preview) {
            std::optional<MediaPacket> preview_depth_packet;
            const MediaPacket *depth_packet = &media_packet;
            if(media_packet.stream_type == StreamType::depth_raw && media_packet.codec_or_compression != "none") {
                try {
                    preview_depth_packet = normalized_depth_packet(media_packet);
                    depth_packet = &*preview_depth_packet;
                }
                catch(const std::exception &e) {
                    logger_.warn(std::string("depth preview packet ignored camera=") + media_packet.sender_id + "_" + media_packet.camera_id
                                 + " frame=" + std::to_string(media_packet.frame_id) + ": " + e.what());
                    depth_packet = nullptr;
                }
            }
            PreviewImage preview;
            if(depth_packet) {
                preview = build_depth_preview_bmp(depth_packet->payload,
                                                  depth_packet->width,
                                                  depth_packet->height,
                                                  depth_preview_range_for_camera(depth_packet->sender_id, depth_packet->camera_id),
                                                  depth_preview_scale);
            }
            if(!preview.bytes.empty()) {
                std::lock_guard<std::mutex> lock(mutex_);
                if(config_.preview_enabled && cam->online &&
                   is_recent_us(now_us(), cam->depth_preview_requested_until_us, 0) &&
                   cam->last_media_us >= depth_preview_media_us) {
                    std::lock_guard<std::mutex> preview_lock(cam->preview_mutex);
                    cam->depth_preview_ppm = std::move(preview.bytes);
                    cam->depth_preview_width = preview.width;
                    cam->depth_preview_height = preview.height;
                    cam->depth_preview_us = depth_preview_media_us;
                }
            }
        }
    }

    UdpReassemblyStats &udp_stats_locked(bool media_udp) {
        return media_udp ? media_udp_stats_ : preview_udp_stats_;
    }

    void account_incomplete_udp_assembly_locked(const PreviewUdpAssembly &assembly, bool evicted) {
        auto &stats = udp_stats_locked(assembly.media_udp);
        const uint64_t missing = assembly.chunk_count > assembly.received_count
                                     ? static_cast<uint64_t>(assembly.chunk_count - assembly.received_count)
                                     : 0;
        if(evicted) {
            stats.evicted_packets++;
            stats.evicted_missing_fragments += missing;
        }
        else {
            stats.expired_packets++;
            stats.expired_missing_fragments += missing;
        }
    }

    void record_udp_invalid_datagram(bool media_udp) {
        std::lock_guard<std::mutex> lock(preview_udp_mutex_);
        udp_stats_locked(media_udp).invalid_datagrams++;
    }

    void record_udp_parse_rejected_packet(bool media_udp) {
        std::lock_guard<std::mutex> lock(preview_udp_mutex_);
        udp_stats_locked(media_udp).parse_rejected_packets++;
    }

    void record_udp_stream_result(bool media_udp, StreamType stream_type, bool accepted) {
        std::lock_guard<std::mutex> lock(preview_udp_mutex_);
        auto &stats = udp_stats_locked(media_udp);
        if(!accepted) {
            stats.stream_rejected_packets++;
            return;
        }
        switch(stream_type) {
        case StreamType::rgb:
            stats.completed_rgb_packets++;
            break;
        case StreamType::depth_raw:
            stats.completed_depth_packets++;
            break;
        case StreamType::rgb_preview:
            stats.completed_preview_packets++;
            break;
        case StreamType::rgb_snapshot:
            break;
        }
    }

    void cleanup_preview_udp_assemblies_locked(uint64_t now) {
        for(auto it = preview_udp_assemblies_.begin(); it != preview_udp_assemblies_.end();) {
            if(now > it->second.updated_us && now - it->second.updated_us > kPreviewUdpAssemblyTimeoutUs) {
                account_incomplete_udp_assembly_locked(it->second, false);
                it = preview_udp_assemblies_.erase(it);
            }
            else {
                ++it;
            }
        }
        size_t allocated_bytes = 0;
        for(const auto &item : preview_udp_assemblies_) {
            allocated_bytes += item.second.bytes.size();
        }
        while(preview_udp_assemblies_.size() > kPreviewUdpMaxAssemblies || allocated_bytes > kPreviewUdpMaxAssemblyBytes) {
            auto oldest = preview_udp_assemblies_.begin();
            for(auto it = preview_udp_assemblies_.begin(); it != preview_udp_assemblies_.end(); ++it) {
                if(it->second.updated_us < oldest->second.updated_us) {
                    oldest = it;
                }
            }
            allocated_bytes -= std::min(allocated_bytes, oldest->second.bytes.size());
            account_incomplete_udp_assembly_locked(oldest->second, true);
            preview_udp_assemblies_.erase(oldest);
        }
    }

    void reserve_udp_assembly_budget_locked(size_t requested_bytes) {
        size_t allocated_bytes = 0;
        for(const auto &item : preview_udp_assemblies_) {
            allocated_bytes += item.second.bytes.size();
        }
        while(!preview_udp_assemblies_.empty()
              && (preview_udp_assemblies_.size() >= kPreviewUdpMaxAssemblies
                  || allocated_bytes > kPreviewUdpMaxAssemblyBytes - requested_bytes)) {
            auto oldest = preview_udp_assemblies_.begin();
            for(auto it = preview_udp_assemblies_.begin(); it != preview_udp_assemblies_.end(); ++it) {
                if(it->second.updated_us < oldest->second.updated_us) {
                    oldest = it;
                }
            }
            allocated_bytes -= std::min(allocated_bytes, oldest->second.bytes.size());
            account_incomplete_udp_assembly_locked(oldest->second, true);
            preview_udp_assemblies_.erase(oldest);
        }
    }

    void handle_fragmented_udp_datagram(const uint8_t *data, size_t size, const std::string &peer_endpoint, bool media_udp) {
        {
            std::lock_guard<std::mutex> lock(preview_udp_mutex_);
            auto &stats = udp_stats_locked(media_udp);
            stats.datagrams++;
            stats.datagram_bytes += size;
        }
        if(size < kPreviewUdpHeaderSize) {
            record_udp_invalid_datagram(media_udp);
            return;
        }
        const uint32_t magic = read_le32(data + 0);
        const uint16_t version = read_le16(data + 4);
        const uint16_t header_size = read_le16(data + 6);
        if(magic != kPreviewUdpMagic || version != kPreviewUdpHeaderVersion || header_size != kPreviewUdpHeaderSize || size < header_size) {
            record_udp_invalid_datagram(media_udp);
            return;
        }

        const uint32_t sequence = read_le32(data + 8);
        const uint16_t chunk_index = read_le16(data + 12);
        const uint16_t chunk_count = read_le16(data + 14);
        const uint32_t total_size = read_le32(data + 16);
        const uint32_t chunk_offset = read_le32(data + 20);
        const uint16_t chunk_size = read_le16(data + 24);
        const size_t udp_packet_limit = std::min(config_.max_payload_bytes, kPreviewUdpMaxPacketBytes);
        if(chunk_count == 0 || chunk_count > kPreviewUdpMaxChunks || chunk_count > total_size || chunk_index >= chunk_count || total_size == 0
           || total_size > udp_packet_limit
           || chunk_size == 0 || chunk_offset > total_size || chunk_size > total_size - chunk_offset
           || size != header_size + chunk_size) {
            record_udp_invalid_datagram(media_udp);
            return;
        }

        std::vector<uint8_t> completed;
        const uint64_t now = now_us();
        const std::string key = std::string(media_udp ? "media#" : "preview#") + peer_endpoint + "#" + std::to_string(sequence);
        {
            std::lock_guard<std::mutex> lock(preview_udp_mutex_);
            cleanup_preview_udp_assemblies_locked(now);
            auto &stats = udp_stats_locked(media_udp);
            stats.valid_fragments++;
            auto existing = preview_udp_assemblies_.find(key);
            if(existing == preview_udp_assemblies_.end()) {
                reserve_udp_assembly_budget_locked(total_size);
                existing = preview_udp_assemblies_.emplace(key, PreviewUdpAssembly{}).first;
            }
            auto &assembly = existing->second;
            if(assembly.bytes.size() != total_size || assembly.chunk_count != chunk_count || assembly.media_udp != media_udp) {
                if(!assembly.bytes.empty()) {
                    account_incomplete_udp_assembly_locked(assembly, true);
                }
                assembly.bytes.assign(total_size, 0);
                assembly.received.assign(chunk_count, 0);
                assembly.chunk_offsets.assign(chunk_count, 0);
                assembly.chunk_sizes.assign(chunk_count, 0);
                assembly.received_count = 0;
                assembly.total_size = total_size;
                assembly.chunk_count = chunk_count;
                assembly.media_udp = media_udp;
                assembly.first_us = now;
                stats.assemblies_started++;
                size_t active_for_type = 0;
                for(const auto &item : preview_udp_assemblies_) {
                    if(item.second.media_udp == media_udp) {
                        ++active_for_type;
                    }
                }
                stats.max_active_assemblies = std::max<uint64_t>(stats.max_active_assemblies, active_for_type);
            }
            assembly.updated_us = now;
            if(!assembly.received[chunk_index]) {
                std::memcpy(assembly.bytes.data() + chunk_offset, data + header_size, chunk_size);
                assembly.received[chunk_index] = 1;
                assembly.chunk_offsets[chunk_index] = chunk_offset;
                assembly.chunk_sizes[chunk_index] = chunk_size;
                assembly.received_count++;
            }
            else {
                stats.duplicate_fragments++;
            }
            if(assembly.received_count == assembly.chunk_count) {
                uint64_t expected_offset = 0;
                bool layout_valid = true;
                for(size_t i = 0; i < assembly.chunk_count; ++i) {
                    if(assembly.chunk_offsets[i] != expected_offset || assembly.chunk_sizes[i] == 0) {
                        layout_valid = false;
                        break;
                    }
                    expected_offset += assembly.chunk_sizes[i];
                }
                if(!layout_valid || expected_offset != assembly.total_size) {
                    stats.invalid_datagrams++;
                    account_incomplete_udp_assembly_locked(assembly, true);
                    preview_udp_assemblies_.erase(key);
                    return;
                }
                stats.completed_packets++;
                stats.completed_bytes += assembly.total_size;
                completed = std::move(assembly.bytes);
                preview_udp_assemblies_.erase(key);
            }
        }

        if(completed.empty()) {
            return;
        }
        try {
            auto packet = parse_media_packet_buffer(completed.data(), completed.size(), config_.max_payload_bytes);
            const bool accepted = media_udp ? (packet.stream_type == StreamType::rgb || packet.stream_type == StreamType::depth_raw)
                                            : (packet.stream_type == StreamType::rgb_preview);
            record_udp_stream_result(media_udp, packet.stream_type, accepted);
            if(accepted) {
                handle_media_packet(std::move(packet), peer_endpoint);
            }
        }
        catch(const std::exception &e) {
            record_udp_parse_rejected_packet(media_udp);
            logger_.warn(std::string(media_udp ? "media UDP packet rejected from " : "preview UDP packet rejected from ")
                         + peer_endpoint + ": " + e.what());
        }
    }

    void udp_loop() {
        const int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if(fd < 0) {
            listener_start_failed_ = true;
            logger_.error(std::string("cannot create UDP socket: ") + std::strerror(errno));
            return;
        }
        set_fd_cloexec(fd);
        int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        set_socket_timeout(fd, 1);
        const auto addr = make_bind_addr(config_.status_bind_ip, config_.status_port);
        if(bind(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
            listener_start_failed_ = true;
            logger_.error(std::string("cannot bind status UDP: ") + std::strerror(errno));
            close(fd);
            return;
        }
        status_udp_ready_ = true;

        std::vector<char> buffer(65536);
        while(running_ && g_running) {
            sockaddr_in peer{};
            socklen_t peer_len = sizeof(peer);
            const ssize_t got = recvfrom(fd, buffer.data(), buffer.size() - 1, 0, reinterpret_cast<sockaddr *>(&peer), &peer_len);
            if(got < 0) {
                if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                    continue;
                }
                logger_.warn(std::string("UDP recv failed: ") + std::strerror(errno));
                continue;
            }
            buffer[static_cast<size_t>(got)] = '\0';
            try {
                handle_status_message(std::string(buffer.data(), static_cast<size_t>(got)), socket_endpoint(peer));
            }
            catch(const std::exception &e) {
                logger_.warn(std::string("status UDP packet rejected: ") + e.what());
            }
        }
        close(fd);
    }

    void preview_udp_loop() {
        const int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if(fd < 0) {
            listener_start_failed_ = true;
            logger_.error(std::string("cannot create preview UDP socket: ") + std::strerror(errno));
            return;
        }
        set_fd_cloexec(fd);
        int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        if(!set_socket_recv_buffer(fd, kMediaSocketReceiveBufferBytes)) {
            logger_.warn(std::string("cannot set preview UDP SO_RCVBUF: ") + std::strerror(errno));
        }
        set_socket_timeout(fd, 1);
        const auto addr = make_bind_addr(config_.preview_udp_bind_ip, config_.preview_udp_port);
        if(bind(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
            listener_start_failed_ = true;
            logger_.error(std::string("cannot bind preview UDP: ") + std::strerror(errno));
            close(fd);
            return;
        }
        preview_udp_ready_ = true;
        logger_.info("preview UDP listening on " + config_.preview_udp_bind_ip + ":" + std::to_string(config_.preview_udp_port));

        std::vector<uint8_t> buffer(65536);
        while(running_ && g_running) {
            sockaddr_in peer{};
            socklen_t peer_len = sizeof(peer);
            const ssize_t got = recvfrom(fd, buffer.data(), buffer.size(), 0, reinterpret_cast<sockaddr *>(&peer), &peer_len);
            if(got < 0) {
                if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                    continue;
                }
                logger_.warn(std::string("preview UDP recv failed: ") + std::strerror(errno));
                continue;
            }
            try {
                handle_fragmented_udp_datagram(buffer.data(), static_cast<size_t>(got), socket_endpoint(peer), false);
            }
            catch(const std::exception &e) {
                logger_.warn(std::string("preview UDP datagram rejected: ") + e.what());
            }
        }
        close(fd);
    }

    void media_udp_loop() {
        const int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if(fd < 0) {
            listener_start_failed_ = true;
            logger_.error(std::string("cannot create media UDP socket: ") + std::strerror(errno));
            return;
        }
        set_fd_cloexec(fd);
        int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        if(!set_socket_recv_buffer(fd, kMediaSocketReceiveBufferBytes)) {
            logger_.warn(std::string("cannot set media UDP SO_RCVBUF: ") + std::strerror(errno));
        }
        set_socket_timeout(fd, 1);
        const auto addr = make_bind_addr(config_.media_udp_bind_ip, config_.media_udp_port);
        if(bind(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
            listener_start_failed_ = true;
            logger_.error(std::string("cannot bind media UDP: ") + std::strerror(errno));
            close(fd);
            return;
        }
        media_udp_ready_ = true;
        logger_.info("media UDP listening on " + config_.media_udp_bind_ip + ":" + std::to_string(config_.media_udp_port));

        std::vector<uint8_t> buffer(65536);
        while(running_ && g_running) {
            sockaddr_in peer{};
            socklen_t peer_len = sizeof(peer);
            const ssize_t got = recvfrom(fd, buffer.data(), buffer.size(), 0, reinterpret_cast<sockaddr *>(&peer), &peer_len);
            if(got < 0) {
                if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                    continue;
                }
                logger_.warn(std::string("media UDP recv failed: ") + std::strerror(errno));
                continue;
            }
            try {
                handle_fragmented_udp_datagram(buffer.data(), static_cast<size_t>(got), socket_endpoint(peer), true);
            }
            catch(const std::exception &e) {
                logger_.warn(std::string("media UDP datagram rejected: ") + e.what());
            }
        }
    }

    void tcp_loop() {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        if(fd < 0) {
            listener_start_failed_ = true;
            logger_.error(std::string("cannot create TCP socket: ") + std::strerror(errno));
            return;
        }
        set_fd_cloexec(fd);
        int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        if(!set_socket_recv_buffer(fd, kMediaSocketReceiveBufferBytes)) {
            logger_.warn(std::string("cannot set media listen SO_RCVBUF: ") + std::strerror(errno));
        }
        set_socket_timeout(fd, 1);
        const auto addr = make_bind_addr(config_.media_bind_ip, config_.media_port);
        if(bind(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
            listener_start_failed_ = true;
            logger_.error(std::string("cannot bind media TCP: ") + std::strerror(errno));
            close(fd);
            return;
        }
        if(listen(fd, kMediaListenBacklog) != 0) {
            listener_start_failed_ = true;
            logger_.error(std::string("cannot listen media TCP: ") + std::strerror(errno));
            close(fd);
            return;
        }
        media_tcp_ready_ = true;

        while(running_ && g_running) {
            sockaddr_in peer{};
            socklen_t peer_len = sizeof(peer);
            const int client = accept(fd, reinterpret_cast<sockaddr *>(&peer), &peer_len);
            if(client < 0) {
                if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                    continue;
                }
                logger_.warn(std::string("media accept failed: ") + std::strerror(errno));
                continue;
            }
            set_fd_cloexec(client);
            if(!set_socket_recv_buffer(client, kMediaSocketReceiveBufferBytes)) {
                logger_.warn(std::string("cannot set media client SO_RCVBUF: ") + std::strerror(errno));
            }
            const auto peer_endpoint = socket_endpoint(peer);
            const int previous_clients = active_media_clients_.fetch_add(1);
            if(previous_clients >= kMaxActiveMediaClients) {
                active_media_clients_.fetch_sub(1);
                close(client);
                continue;
            }
            set_socket_timeout(client, kMediaClientSocketTimeoutSec);
            const uint64_t media_session_id = next_media_session_id_.fetch_add(1);
            try {
                launch_client_thread(client, "media", [this, client, peer_endpoint, media_session_id] {
                    try {
                        media_client_loop(client, peer_endpoint, media_session_id);
                    }
                    catch(...) {
                        active_media_clients_.fetch_sub(1);
                        throw;
                    }
                    active_media_clients_.fetch_sub(1);
                });
            }
            catch(const std::exception &e) {
                active_media_clients_.fetch_sub(1);
                logger_.warn(std::string("cannot start media client thread: ") + e.what());
            }
        }
        close(fd);
    }

    void media_client_loop(int fd, const std::string &peer_endpoint, uint64_t media_session_id) {
        ScopeExit unregister_session([this, media_session_id] { mark_media_ingress_session_closed(media_session_id); });
        logger_.info("media client connected from=" + peer_endpoint + " session=" + std::to_string(media_session_id));
        std::string last_sender;
        std::string last_camera;
        std::string last_stream;
        uint64_t last_frame_id = 0;
        MediaPacket packet;
        MediaPacketReadBuffers read_buffers;
        while(running_ && g_running) {
            try {
                read_media_packet_into(fd, config_.max_payload_bytes, read_buffers, packet);
                last_sender = packet.sender_id;
                last_camera = packet.camera_id;
                last_stream = stream_type_name(packet.stream_type);
                last_frame_id = packet.frame_id;
                handle_media_packet(std::move(packet), peer_endpoint, media_session_id, fd);
            }
            catch(const std::exception &e) {
                std::ostringstream msg;
                msg << "media client disconnected from=" << peer_endpoint << " session=" << media_session_id
                    << " last_sender=" << last_sender << " last_camera=" << last_camera
                    << " last_stream=" << last_stream << " last_frame=" << last_frame_id << " reason=" << e.what();
                if(std::string(e.what()) == "connection closed") {
                    logger_.info(msg.str());
                }
                else {
                    logger_.warn(msg.str());
                }
                break;
            }
        }
    }

    void admin_loop() {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        if(fd < 0) {
            listener_start_failed_ = true;
            logger_.error(std::string("cannot create admin socket: ") + std::strerror(errno));
            return;
        }
        set_fd_cloexec(fd);
        int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        set_socket_timeout(fd, 1);
        const auto addr = make_bind_addr(config_.admin_bind_ip, config_.admin_port);
        if(bind(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
            listener_start_failed_ = true;
            logger_.error(std::string("cannot bind admin HTTP: ") + std::strerror(errno));
            close(fd);
            return;
        }
        if(listen(fd, kAdminListenBacklog) != 0) {
            listener_start_failed_ = true;
            logger_.error(std::string("cannot listen admin HTTP: ") + std::strerror(errno));
            close(fd);
            return;
        }
        admin_ready_ = true;

        while(running_ && g_running) {
            const int client = accept(fd, nullptr, nullptr);
            if(client < 0) {
                if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                    continue;
                }
                logger_.warn(std::string("admin accept failed: ") + std::strerror(errno));
                continue;
            }
            set_fd_cloexec(client);
            set_socket_timeout(client, 2);
            const int previous_clients = active_admin_clients_.fetch_add(1);
            if(previous_clients >= kMaxActiveAdminClients) {
                active_admin_clients_.fetch_sub(1);
                close(client);
                continue;
            }
            try {
                launch_client_thread(client, "admin", [this, client] {
                    try {
                        handle_admin_client(client);
                    }
                    catch(...) {
                        active_admin_clients_.fetch_sub(1);
                        throw;
                    }
                    active_admin_clients_.fetch_sub(1);
                });
            }
            catch(const std::exception &e) {
                active_admin_clients_.fetch_sub(1);
                logger_.warn(std::string("cannot start admin client thread: ") + e.what());
            }
        }
        close(fd);
    }

    void handle_admin_client(int fd) {
        std::string request;
        char buffer[4096];
        while(request.find("\r\n\r\n") == std::string::npos && request.size() < 65536) {
            const ssize_t got = recv(fd, buffer, sizeof(buffer), 0);
            if(got <= 0) {
                return;
            }
            request.append(buffer, buffer + got);
        }

        std::istringstream input(request);
        std::string method;
        std::string target;
        std::string version;
        input >> method >> target >> version;
        std::string path = target;
        std::string query;
        const size_t qpos = target.find('?');
        if(qpos != std::string::npos) {
            path = target.substr(0, qpos);
            query = target.substr(qpos + 1);
        }
        const auto args = parse_query(query);

        int status = 200;
        std::string content_type = "application/json";
        std::string body;
        const bool invalid_sender_id = args.count("sender_id") && !is_valid_protocol_id(args.at("sender_id"));
        const bool invalid_camera_id = args.count("camera_id") && !is_valid_protocol_id(args.at("camera_id"));
        if(invalid_sender_id || invalid_camera_id) {
            status = 400;
            body = "{\"ok\":false,\"error\":\"invalid sender_id/camera_id\"}";
        }
        else if(method == "GET" && path == "/api/status") {
            body = status_json();
        }
        else if(method == "GET" && path == "/api/config") {
            body = config_json();
        }
        else if(method == "POST" && path == "/api/record/start-all") {
            body = start_all(args.count("file_prefix") ? std::optional<std::string>(args.at("file_prefix")) : std::nullopt);
        }
        else if(method == "POST" && path == "/api/record/stop-all") {
            body = stop_all();
        }
        else if(method == "POST" && path == "/api/record/start-sender") {
            body = start_sender(args.count("sender_id") ? args.at("sender_id") : "");
        }
        else if(method == "POST" && path == "/api/record/stop-sender") {
            body = stop_sender(args.count("sender_id") ? args.at("sender_id") : "");
        }
        else if(method == "POST" && path == "/api/record/start") {
            body = start_camera(args.count("sender_id") ? args.at("sender_id") : "", args.count("camera_id") ? args.at("camera_id") : "",
                                args.count("file_prefix") ? std::optional<std::string>(args.at("file_prefix")) : std::nullopt);
        }
        else if(method == "POST" && path == "/api/record/stop") {
            body = stop_camera(args.count("sender_id") ? args.at("sender_id") : "", args.count("camera_id") ? args.at("camera_id") : "");
        }
        else if(method == "POST" && path == "/api/camera/name") {
            body = set_camera_name(args.count("sender_id") ? args.at("sender_id") : "", args.count("camera_id") ? args.at("camera_id") : "",
                                   args.count("camera_name") ? args.at("camera_name") : "");
        }
        else if(method == "POST" && path == "/api/camera/prefix") {
            body = set_camera_file_prefix(args.count("sender_id") ? args.at("sender_id") : "", args.count("camera_id") ? args.at("camera_id") : "",
                                          args.count("prefix") ? args.at("prefix") : "");
        }
        else if(method == "POST" && path == "/api/storage/prefix") {
            body = set_default_file_prefix(args.count("prefix") ? args.at("prefix") : "");
        }
        else if(method == "POST" && path == "/api/preview/main-target") {
            body = set_main_preview_target(args.count("sender_id") ? args.at("sender_id") : "",
                                           args.count("camera_id") ? args.at("camera_id") : "");
        }
        else if(method == "GET" && path == "/api/preview/depth") {
            const auto preview = depth_preview(args.count("sender_id") ? args.at("sender_id") : "",
                                               args.count("camera_id") ? args.at("camera_id") : "");
            if(preview) {
                content_type = "image/bmp";
                body.assign(reinterpret_cast<const char *>(preview->data()), preview->size());
            }
            else {
                status = 404;
                body = "{\"ok\":false,\"error\":\"depth preview not found\"}";
            }
        }
        else if(method == "GET" && path == "/api/preview/rgb") {
            const auto preview = rgb_preview(args.count("sender_id") ? args.at("sender_id") : "",
                                             args.count("camera_id") ? args.at("camera_id") : "");
            if(preview) {
                content_type = "image/jpeg";
                body.assign(reinterpret_cast<const char *>(preview->data()), preview->size());
            }
            else {
                status = 404;
                body = "{\"ok\":false,\"error\":\"rgb preview not found\"}";
            }
        }
        else if(method == "GET" && path == "/api/preview/rgb-main") {
            const auto preview = main_rgb_preview(args.count("sender_id") ? args.at("sender_id") : "",
                                                  args.count("camera_id") ? args.at("camera_id") : "");
            if(preview) {
                content_type = "image/jpeg";
                body.assign(reinterpret_cast<const char *>(preview->data()), preview->size());
            }
            else {
                status = 404;
                body = "{\"ok\":false,\"error\":\"main rgb preview not found\"}";
            }
        }
        else if(method == "GET" && path == "/api/preview/rgb-h264") {
            stream_rgb_h264_preview(fd, args.count("sender_id") ? args.at("sender_id") : "",
                                    args.count("camera_id") ? args.at("camera_id") : "");
            return;
        }
        else if(method == "GET" && path == "/api/preview/rgb-h264-frames") {
            const bool force_main_stream = args.count("quality") && args.at("quality") == "main";
            const bool include_global_timestamp = args.count("metadata") && args.at("metadata") == "global";
            stream_rgb_h264_preview_frames(fd, args.count("sender_id") ? args.at("sender_id") : "",
                                           args.count("camera_id") ? args.at("camera_id") : "",
                                           force_main_stream,
                                           include_global_timestamp);
            return;
        }
        else {
            status = 404;
            body = "{\"ok\":false,\"error\":\"not found\"}";
        }

        std::ostringstream response;
        const char *reason = status == 200 ? "OK" : (status == 400 ? "Bad Request" : "Not Found");
        response << "HTTP/1.1 " << status << ' ' << reason << "\r\n";
        response << "Content-Type: " << content_type << "\r\n";
        response << "Cache-Control: no-store\r\n";
        response << "Content-Length: " << body.size() << "\r\n";
        response << "Connection: close\r\n\r\n";
        response << body;
        const auto text = response.str();
        send_all(fd, text);
    }

    Config config_;
    Logger logger_;
    RuntimeState runtime_state_;
    ClockSyncManager clock_sync_manager_;
    ReceiverDiscoveryServer receiver_discovery_server_;
    std::atomic<bool> started_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> listener_start_failed_{false};
    std::atomic<bool> status_udp_ready_{false};
    std::atomic<bool> media_tcp_ready_{false};
    std::atomic<bool> media_udp_ready_{false};
    std::atomic<bool> preview_udp_ready_{false};
    std::atomic<bool> admin_ready_{false};
    std::atomic<int> active_media_clients_{0};
    std::atomic<int> active_admin_clients_{0};
    std::atomic<size_t> total_record_queue_bytes_{0};
    std::atomic<uint64_t> next_media_session_id_{1};
    std::atomic<uint64_t> media_ingress_superseded_sessions_{0};
    std::atomic<uint64_t> media_ingress_stale_packets_{0};
    std::thread udp_thread_;
    std::thread media_udp_thread_;
    std::thread preview_udp_thread_;
    std::thread tcp_thread_;
    std::thread admin_thread_;
    std::thread recording_maintenance_thread_;
    std::vector<std::thread> segment_finalize_workers_;
    std::mutex mutex_;
    std::mutex segment_close_futures_mutex_;
    std::mutex segment_finalize_mutex_;
    std::mutex media_ingress_mutex_;
    std::mutex runtime_state_save_mutex_;
    std::mutex preview_udp_mutex_;
    std::mutex client_threads_mutex_;
    std::mutex decoder_cleanup_mutex_;
    std::mutex status_cache_mutex_;
    std::mutex recording_maintenance_mutex_;
    std::mutex uploader_status_mutex_;
    std::mutex photo_capture_mutex_;
    std::condition_variable decoder_cleanup_cv_;
    std::condition_variable segment_finalize_cv_;
    std::condition_variable recording_maintenance_cv_;
    std::condition_variable photo_capture_cv_;
    bool decoder_cleanup_running_ = false;
    bool segment_finalize_worker_running_ = false;
    bool segment_finalize_worker_stop_ = false;
    size_t segment_finalize_outstanding_ = 0;
    uint64_t runtime_state_revision_ = 0;
    uint64_t runtime_state_save_revision_ = 0;
    std::string main_preview_key_;
    bool recording_all_ = false;
    bool recording_all_start_pending_ = false;
    bool recording_faulted_ = false;
    uint64_t recording_all_session_id_ = 0;
    uint64_t recording_all_start_us_ = 0;
    uint64_t recording_fault_session_id_ = 0;
    uint64_t recording_fault_us_ = 0;
    uint64_t last_recording_session_id_ = 0;
    std::string recording_fault_camera_key_;
    std::string recording_fault_reason_;
    std::atomic<bool> recording_fault_stop_requested_{false};
    bool recording_all_has_file_prefix_override_ = false;
    std::string recording_all_file_prefix_;
    std::map<std::string, std::shared_ptr<CameraState>> cameras_;
    std::vector<std::future<void>> segment_close_futures_;
    std::deque<SegmentFinalizeTask> segment_finalize_queue_;
    std::set<std::string> segment_finalize_active_routes_;
    std::deque<PhotoCaptureJob> photo_capture_queue_;
    std::set<std::string> photo_capture_pending_ids_;
    std::set<std::string> photo_reserved_relative_paths_;
    std::set<std::string> photo_reserved_directories_;
    std::unordered_map<std::string, PhotoBurstPathState> photo_burst_paths_;
    std::deque<std::string> photo_burst_path_order_;
    std::unordered_map<std::string, std::string> photo_completed_paths_;
    std::deque<std::string> photo_completed_order_;
    size_t photo_capture_queue_bytes_ = 0;
    bool photo_capture_available_ = false;
    bool photo_capture_stop_ = false;
    std::atomic<uint64_t> photo_temp_sequence_{0};
    std::atomic<uint64_t> photo_capture_enqueued_{0};
    std::atomic<uint64_t> photo_capture_completed_{0};
    std::atomic<uint64_t> photo_capture_duplicate_requests_{0};
    std::atomic<uint64_t> photo_capture_failures_{0};
    std::atomic<size_t> segment_finalize_outstanding_status_{0};
    std::atomic<size_t> segment_finalize_queued_status_{0};
    std::atomic<size_t> segment_finalize_active_status_{0};
    std::atomic<uint64_t> segment_finalize_completed_total_{0};
    std::atomic<uint64_t> segment_finalize_failures_total_{0};
    std::atomic<uint64_t> segment_finalize_last_completed_us_{0};
    std::atomic<uint64_t> uploader_pending_metrics_refreshed_us_{0};
    std::atomic<uint64_t> uploader_pending_segments_status_{0};
    std::vector<ClientThread> client_threads_;
    std::deque<std::unique_ptr<RgbPreviewDecoder>> decoder_cleanup_queue_;
    std::set<int> client_fds_;
    std::string status_cache_;
    std::string uploader_status_json_;
    std::unordered_map<std::string, PreviewUdpAssembly> preview_udp_assemblies_;
    std::unordered_map<std::string, MediaIngressOwner> media_ingress_owners_;
    std::unordered_map<std::string, std::string> sender_source_ips_;
    UdpReassemblyStats media_udp_stats_;
    UdpReassemblyStats preview_udp_stats_;
    std::thread decoder_cleanup_thread_;
    std::thread photo_capture_thread_;
};
