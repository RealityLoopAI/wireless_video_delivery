std::string ob_error_text(const ob::Error &error) {
    const char *message = error.getMessage();
    return message ? message : "Orbbec SDK error";
}

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool camera_model_is_gemini305(const std::string &device_model) {
    return lower_copy(device_model).find("gemini 305") != std::string::npos;
}

int camera_publish_warmup_ms(const CameraConfig &config, const std::string &device_model) {
    if(config.publish_warmup_ms >= 0) {
        return config.publish_warmup_ms;
    }
    return camera_model_is_gemini305(device_model) ? 3000 : 0;
}

bool gemini305_manual_exposure_requested(const CameraConfig &config, const std::string &device_model) {
    const auto &controls = config.color_controls;
    return controls.auto_exposure && !*controls.auto_exposure
           && camera_model_is_gemini305(device_model);
}

bool ensure_gemini305_manual_exposure(const CameraConfig &config, const std::string &device_model,
                                      const std::string &device_serial, Logger &logger, bool force_reapply = false) {
    const auto &controls = config.color_controls;
    if(!gemini305_manual_exposure_requested(config, device_model)) {
        return true;
    }

    const auto video_device = find_v4l2_device_by_serial(device_serial);
    if(video_device.empty()) {
        logger.warn("Gemini305 manual exposure verification skipped camera_id=" + config.camera_id
                    + " reason=v4l2_device_not_found serial=" + device_serial);
        return false;
    }

    const int fd = open(video_device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if(fd < 0) {
        logger.warn("Gemini305 manual exposure verification skipped camera_id=" + config.camera_id
                    + " device=" + video_device + " error=" + std::strerror(errno));
        return false;
    }

    int control_errno = 0;
    auto get_control = [&](uint32_t id, int &value) {
        v4l2_control control{};
        control.id = id;
        if(checked_v4l2_ioctl(fd, VIDIOC_G_CTRL, &control) < 0) {
            control_errno = errno;
            return false;
        }
        value = control.value;
        return true;
    };
    auto set_control = [&](uint32_t id, int value) {
        v4l2_control control{};
        control.id = id;
        control.value = value;
        if(checked_v4l2_ioctl(fd, VIDIOC_S_CTRL, &control) < 0) {
            control_errno = errno;
            return false;
        }
        return true;
    };

    int exposure_mode = -1;
    const bool mode_readable = get_control(V4L2_CID_EXPOSURE_AUTO, exposure_mode);
    const bool applied_fallback = mode_readable && exposure_mode != V4L2_EXPOSURE_MANUAL;
    bool success = mode_readable;
    if(mode_readable && (applied_fallback || force_reapply)) {
        success = set_control(V4L2_CID_EXPOSURE_AUTO, V4L2_EXPOSURE_MANUAL);
        if(success && controls.exposure) {
            success = set_control(V4L2_CID_EXPOSURE_ABSOLUTE, *controls.exposure);
        }
        if(success && controls.gain) {
            success = set_control(V4L2_CID_GAIN, *controls.gain);
        }
    }

    int verified_mode = -1;
    const bool verified = success && get_control(V4L2_CID_EXPOSURE_AUTO, verified_mode)
                          && verified_mode == V4L2_EXPOSURE_MANUAL;
    close(fd);

    if(!verified) {
        logger.warn("Gemini305 manual exposure verification failed camera_id=" + config.camera_id
                    + " device=" + video_device + " sdk_mode=" + std::to_string(exposure_mode)
                    + " verified_mode=" + std::to_string(verified_mode) + " error="
                    + (control_errno ? std::strerror(control_errno) : "unexpected_mode"));
        return false;
    }

    logger.info("Gemini305 manual exposure verified camera_id=" + config.camera_id
                + " device=" + video_device + " mode=manual exposure="
                + (controls.exposure ? std::to_string(*controls.exposure) : "unchanged") + " gain="
                + (controls.gain ? std::to_string(*controls.gain) : "unchanged")
                + " fallback_applied=" + (applied_fallback ? "true" : "false")
                + " force_reapply=" + (force_reapply ? "true" : "false"));
    return true;
}

bool is_frame_sync_unsupported_error(const std::string &error) {
    const auto lower = lower_copy(error);
    return lower.find("frame sync") != std::string::npos
           && (lower.find("not support") != std::string::npos || lower.find("does not support") != std::string::npos
               || lower.find("unsupported") != std::string::npos);
}

OBFrameAggregateOutputMode frame_aggregate_mode_from_config(const std::string &mode) {
    if(mode == "full_frame_require") {
        return OB_FRAME_AGGREGATE_OUTPUT_FULL_FRAME_REQUIRE;
    }
    if(mode == "color_frame_require") {
        return OB_FRAME_AGGREGATE_OUTPUT_COLOR_FRAME_REQUIRE;
    }
    if(mode == "any_situation") {
        return OB_FRAME_AGGREGATE_OUTPUT_ANY_SITUATION;
    }
    return OB_FRAME_AGGREGATE_OUTPUT_DISABLE;
}

const char *frame_aggregate_mode_name(OBFrameAggregateOutputMode mode) {
    switch(mode) {
    case OB_FRAME_AGGREGATE_OUTPUT_FULL_FRAME_REQUIRE:
        return "full_frame_require";
    case OB_FRAME_AGGREGATE_OUTPUT_COLOR_FRAME_REQUIRE:
        return "color_frame_require";
    case OB_FRAME_AGGREGATE_OUTPUT_ANY_SITUATION:
        return "any_situation";
    case OB_FRAME_AGGREGATE_OUTPUT_DISABLE:
        return "disable";
    }
    return "unknown";
}

std::chrono::milliseconds reconnect_delay(uint32_t attempts) {
    const auto max_delay = static_cast<uint32_t>(std::max(1, camera_reconnect_max_delay_seconds()));
    return std::chrono::seconds(std::min<uint32_t>(max_delay, std::max<uint32_t>(1, attempts)));
}

void stop_camera(CameraRuntime &camera, Logger &logger) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    if(camera.pipeline) {
        try {
            camera.pipeline->stop();
        }
        catch(const ob::Error &e) {
            logger.warn("camera stop failed camera_id=" + camera.config.camera_id + " error=" + ob_error_text(e));
        }
        catch(const std::exception &e) {
            logger.warn("camera stop failed camera_id=" + camera.config.camera_id + " error=" + e.what());
        }
    }
    camera.encoder.reset();
    camera.rgb_rtp_sender.reset();
    camera.web_preview_encoder.reset();
    camera.jpeg_dual_encoder.reset();
    camera.adaptive_exposure_controller.reset();
    camera.adaptive_exposure_last_evaluation = std::chrono::steady_clock::time_point::min();
    camera.adaptive_exposure_discard_frames_remaining = 0;
    camera.v4l2_capture.reset();
    camera.jpeg_dual_encoder_disabled = false;
    camera.jpeg_dual_no_main_output = 0;
    camera.web_preview_width = 0;
    camera.web_preview_height = 0;
    camera.pipeline.reset();
    camera.pipeline_config.reset();
    camera.color_profile.reset();
    camera.depth_profile.reset();
    camera.device.reset();
    camera.device_serial.clear();
    camera.device_uid.clear();
    camera.device_connection_type.clear();
    camera.online = false;
    camera.announced = false;
    camera.hardware_encoder = false;
    camera.live.adaptive_exposure_enabled = false;
    camera.rgb_sent_timing_seen = false;
    camera.rgb_last_sent_frame_id = 0;
    camera.rgb_last_sent_system_timestamp_us = 0;
    camera.rgb_transport_recovery.reset();
    camera.rgb_encode_timings.clear();
    camera.latest_bgr.release();
    camera.latest_depth_color.release();
}

void start_v4l2_camera_runtime(CameraRuntime &runtime, Logger &logger) {
    auto capture = std::make_shared<V4L2MjpegCapture>();
    capture->open_device(runtime.config);

    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(runtime.mutex);
        runtime.v4l2_capture = capture;
        runtime.device_model = runtime.config.device_model;
        runtime.device_serial = runtime.config.serial_number;
        runtime.device_uid = capture->device_path();
        runtime.device_connection_type = "v4l2";
        runtime.device.reset();
        runtime.pipeline.reset();
        runtime.pipeline_config.reset();
        runtime.color_profile.reset();
        runtime.depth_profile.reset();
        runtime.encoder.reset();
        runtime.rgb_rtp_sender.reset();
        runtime.web_preview_encoder.reset();
        runtime.jpeg_dual_encoder.reset();
        runtime.adaptive_exposure_controller.reset();
        runtime.jpeg_dual_encoder_disabled = false;
        runtime.jpeg_dual_no_main_output = 0;
        runtime.web_preview_width = 0;
        runtime.web_preview_height = 0;
        runtime.online = true;
        runtime.announced = false;
        runtime.hardware_encoder = false;
        runtime.depth_scale = 0.0f;
        runtime.last_error.clear();
        runtime.live = CameraLiveStats{};
        runtime.perf = CameraPerfStats{};
        runtime.perf.interval_started = now;
        runtime.next_preview = now;
        runtime.next_depth_emit = now;
        runtime.next_time_sync_log = now;
        runtime.next_jpeg_warning = now;
        runtime.next_media_warning = now;
        runtime.next_rgb_timing_warning = now;
        runtime.last_rgb_frame_at = now;
        runtime.last_depth_frame_at = now;
        runtime.next_capture_stall_reconnect = now;
        runtime.gemini305_manual_exposure_reapply_pending = false;
        runtime.gemini305_manual_exposure_reapply_at = std::chrono::steady_clock::time_point::min();
        runtime.capture_stall_samples = 0;
        runtime.last_media_warning.clear();
        runtime.rgb_sent_timing_seen = false;
        runtime.rgb_last_sent_frame_id = 0;
        runtime.rgb_last_sent_system_timestamp_us = 0;
        runtime.rgb_transport_recovery.reset();
        runtime.rgb_encode_timings.clear();
        runtime.latest_bgr.release();
        runtime.latest_depth_color.release();
    }

    std::ostringstream oss;
    oss << "v4l2 camera started camera_id=" << runtime.config.camera_id
        << " video_device=" << capture->device_path()
        << " color=" << capture->width() << "x" << capture->height() << "@" << capture->fps()
        << " format=mjpg configured_serial=" << runtime.config.serial_number
        << " depth=disabled";
    logger.info(oss.str());
}

void start_camera_runtime(CameraRuntime &runtime, Logger &logger) {
    if(runtime.config.capture_backend == "v4l2") {
        start_v4l2_camera_runtime(runtime, logger);
        return;
    }

    ob::Context::setLoggerSeverity(OB_LOG_SEVERITY_WARN);
    auto context = std::make_shared<ob::Context>();

    auto device = select_device(*context, runtime.config);
    auto device_info = device->getDeviceInfo();
    const std::string device_serial = device_info->serialNumber() ? device_info->serialNumber() : "";
    const std::string device_model = device_info->name() ? device_info->name() : "";
    const std::string device_uid = device_info->uid() ? device_info->uid() : "";
    const std::string device_connection_type = device_info->connectionType() ? device_info->connectionType() : "";

    CameraRuntime control_runtime;
    control_runtime.config = runtime.config;
    control_runtime.device = device;
    apply_stream_rotation(control_runtime, logger);

    auto start_pipeline = [&](OBFrameAggregateOutputMode aggregate_mode) {
        auto candidate_pipeline = std::make_unique<ob::Pipeline>(device);
        auto candidate_config = std::make_shared<ob::Config>();
        auto candidate_color_profile =
            select_profile(*candidate_pipeline, OB_SENSOR_COLOR, runtime.config.rgb_profile, OB_FORMAT_MJPG, logger);
        std::shared_ptr<ob::VideoStreamProfile> candidate_depth_profile;
        if(runtime.config.depth_profile.enabled) {
            candidate_depth_profile =
                select_profile(*candidate_pipeline, OB_SENSOR_DEPTH, runtime.config.depth_profile, OB_FORMAT_Y16, logger);
        }
        candidate_config->enableStream(candidate_color_profile);
        if(candidate_depth_profile) {
            candidate_config->enableStream(candidate_depth_profile);
        }
        if(aggregate_mode != OB_FRAME_AGGREGATE_OUTPUT_DISABLE && candidate_depth_profile) {
            candidate_config->setFrameAggregateOutputMode(aggregate_mode);
        }
        candidate_pipeline->start(candidate_config);
        return std::make_tuple(std::move(candidate_pipeline), std::move(candidate_config), std::move(candidate_color_profile),
                               std::move(candidate_depth_profile));
    };

    std::unique_ptr<ob::Pipeline> pipeline;
    std::shared_ptr<ob::Config> pipeline_config;
    std::shared_ptr<ob::VideoStreamProfile> color_profile;
    std::shared_ptr<ob::VideoStreamProfile> depth_profile;
    OBFrameAggregateOutputMode aggregate_mode = frame_aggregate_mode_from_config(runtime.config.frame_aggregate_mode);
    try {
        std::tie(pipeline, pipeline_config, color_profile, depth_profile) = start_pipeline(aggregate_mode);
    }
    catch(const ob::Error &e) {
        const auto error = ob_error_text(e);
        if(aggregate_mode == OB_FRAME_AGGREGATE_OUTPUT_DISABLE || !is_frame_sync_unsupported_error(error)) {
            throw;
        }
        aggregate_mode = OB_FRAME_AGGREGATE_OUTPUT_DISABLE;
        logger.warn("camera frame aggregate unsupported, retrying with aggregate disabled camera_id=" + runtime.config.camera_id
                    + " error=" + error);
        std::tie(pipeline, pipeline_config, color_profile, depth_profile) = start_pipeline(aggregate_mode);
    }

    apply_color_controls(control_runtime, logger);
    const bool gemini305_manual_exposure = gemini305_manual_exposure_requested(runtime.config, device_model);
    (void)ensure_gemini305_manual_exposure(runtime.config, device_model, device_serial, logger);

    const auto color_width = color_profile->width();
    const auto color_height = color_profile->height();
    const auto color_fps = color_profile->fps();
    const auto color_format = ob_format_name(color_profile->format());
    const auto depth_width = depth_profile ? depth_profile->width() : 0;
    const auto depth_height = depth_profile ? depth_profile->height() : 0;
    const auto depth_fps = depth_profile ? depth_profile->fps() : 0;
    const auto depth_format = depth_profile ? ob_format_name(depth_profile->format()) : "disabled";
    const auto now = std::chrono::steady_clock::now();
    const int publish_warmup_ms = camera_publish_warmup_ms(runtime.config, device_model);
    std::unique_ptr<AdaptiveExposureController> adaptive_exposure_controller;
    if(runtime.config.adaptive_exposure.enabled) {
        if(adaptive_exposure_max_for_model(device_model)) {
            adaptive_exposure_controller = std::make_unique<AdaptiveExposureController>(
                runtime.config.adaptive_exposure, *runtime.config.color_controls.exposure, *runtime.config.color_controls.gain);
        }
        else {
            logger.warn("adaptive exposure disabled camera_id=" + runtime.config.camera_id
                        + " reason=unsupported_device_model device_model=" + device_model);
        }
    }
    {
        std::lock_guard<std::mutex> lock(runtime.mutex);
        runtime.device = std::move(device);
        runtime.device_model = device_model;
        runtime.device_serial = device_serial;
        runtime.device_uid = device_uid;
        runtime.device_connection_type = device_connection_type;
        runtime.pipeline = std::move(pipeline);
        runtime.pipeline_config = std::move(pipeline_config);
        runtime.color_profile = std::move(color_profile);
        runtime.depth_profile = std::move(depth_profile);
        runtime.encoder.reset();
        runtime.rgb_rtp_sender.reset();
        runtime.web_preview_encoder.reset();
        runtime.jpeg_dual_encoder.reset();
        runtime.adaptive_exposure_controller = std::move(adaptive_exposure_controller);
        runtime.jpeg_dual_encoder_disabled = false;
        runtime.jpeg_dual_no_main_output = 0;
        runtime.web_preview_width = 0;
        runtime.web_preview_height = 0;
        runtime.online = true;
        runtime.announced = false;
        runtime.hardware_encoder = false;
        runtime.depth_scale = 0.0f;
        runtime.last_error.clear();
        runtime.live = CameraLiveStats{};
        runtime.live.adaptive_exposure_enabled = runtime.adaptive_exposure_controller != nullptr;
        if(runtime.adaptive_exposure_controller) {
            runtime.live.adaptive_requested_exposure = runtime.adaptive_exposure_controller->exposure();
            runtime.live.adaptive_requested_gain = runtime.adaptive_exposure_controller->gain();
            runtime.live.adaptive_last_reason = "initialized";
        }
        runtime.perf = CameraPerfStats{};
        runtime.perf.interval_started = now;
        runtime.next_preview = now;
        runtime.next_depth_emit = now;
        runtime.next_time_sync_log = now;
        runtime.next_jpeg_warning = now;
        runtime.next_media_warning = now;
        runtime.next_keyframe_guard_warning = now;
        runtime.next_rgb_timing_warning = now;
        runtime.last_rgb_frame_at = now;
        runtime.last_depth_frame_at = now;
        runtime.next_capture_stall_reconnect = now;
        runtime.gemini305_manual_exposure_reapply_pending = gemini305_manual_exposure;
        runtime.next_adaptive_exposure_sample = now;
        runtime.adaptive_exposure_last_evaluation = std::chrono::steady_clock::time_point::min();
        runtime.next_adaptive_exposure_warning = now;
        runtime.adaptive_exposure_metadata_deadline = std::chrono::steady_clock::time_point::min();
        runtime.adaptive_exposure_waiting_for_metadata = false;
        runtime.adaptive_exposure_discard_frames_remaining = 0;
        runtime.gemini305_manual_exposure_reapply_at = runtime.gemini305_manual_exposure_reapply_pending
                                                          ? now + std::chrono::seconds(2)
                                                          : std::chrono::steady_clock::time_point::min();
        runtime.publish_warmup_active = publish_warmup_ms > 0;
        runtime.publish_warmup_exposure_verified = !gemini305_manual_exposure;
        runtime.publish_warmup_dropped_framesets = 0;
        runtime.publish_not_before = runtime.publish_warmup_active
                                         ? now + std::chrono::milliseconds(publish_warmup_ms)
                                         : std::chrono::steady_clock::time_point::min();
        runtime.publish_warmup_deadline = runtime.publish_warmup_active
                                             ? runtime.publish_not_before + std::chrono::seconds(5)
                                             : std::chrono::steady_clock::time_point::min();
        runtime.capture_stall_samples = 0;
        runtime.last_media_warning.clear();
        runtime.rgb_sent_timing_seen = false;
        runtime.rgb_last_sent_frame_id = 0;
        runtime.rgb_last_sent_system_timestamp_us = 0;
        runtime.rgb_transport_recovery.reset();
        runtime.rgb_encode_timings.clear();
        runtime.latest_bgr.release();
        runtime.latest_depth_color.release();
    }

    std::ostringstream oss;
    oss << "camera started camera_id=" << runtime.config.camera_id << " color=" << color_width << "x"
        << color_height << "@" << color_fps << " format=" << color_format
        << " depth=" << depth_width << "x" << depth_height << "@" << depth_fps
        << " format=" << depth_format
        << " configured_model=" << runtime.config.device_model
        << " device_model=" << device_model
        << " configured_serial=" << runtime.config.serial_number
        << " configured_uid=" << runtime.config.uid
        << " device_serial=" << device_serial
        << " device_uid=" << device_uid
        << " paired_rgb_serial=" << paired_rgb_serial_for_depth_uid(device_uid)
        << " connection=" << device_connection_type
        << " aggregate_mode=" << frame_aggregate_mode_name(aggregate_mode)
        << " publish_warmup_ms=" << publish_warmup_ms
        << " exposure_verification=" << (gemini305_manual_exposure ? "required" : "not_required")
        << " adaptive_exposure=" << (runtime.config.adaptive_exposure.enabled ? "enabled" : "disabled");
    logger.info(oss.str());
}

std::vector<std::unique_ptr<CameraRuntime>> start_cameras(const AppConfig &config, Logger &logger) {
    std::vector<std::unique_ptr<CameraRuntime>> cameras;
    const auto now = std::chrono::steady_clock::now();
    size_t index = 0;

    for(const auto &camera_config : config.cameras) {
        auto runtime = std::make_unique<CameraRuntime>();
        runtime->config = camera_config;
        runtime->online = false;
        runtime->last_error = "camera startup pending";
        runtime->next_reconnect = now + std::chrono::milliseconds(static_cast<int>(index) * 1500);
        logger.info("camera startup scheduled camera_id=" + runtime->config.camera_id
                    + " delay_ms=" + std::to_string(static_cast<int>(index) * 1500));
        cameras.push_back(std::move(runtime));
        ++index;
    }
    return cameras;
}

template <typename Sender>
bool send_status_locked(Sender &sender, Logger &logger, std::mutex &transport_mutex, const Json::Value &message) {
    std::lock_guard<std::mutex> lock(transport_mutex);
    return send_status(sender, logger, message);
}

MediaPacketJob make_owned_media_job(CameraRuntime &camera, StreamType stream_type, const MediaFrameMeta &meta,
                                    std::vector<uint8_t> &&payload) {
    MediaPacketJob job;
    job.camera = &camera;
    job.stream_type = stream_type;
    job.header = build_media_header(meta);
    job.owned_payload = std::move(payload);
    job.rgb_keyframe = stream_type == StreamType::rgb && (meta.flags & key_frame) != 0u;
    return job;
}

MediaPacketJob make_external_media_job(CameraRuntime &camera, StreamType stream_type, const MediaFrameMeta &meta, const void *payload,
                                       size_t payload_size, std::shared_ptr<const void> payload_owner) {
    MediaPacketJob job;
    job.camera = &camera;
    job.stream_type = stream_type;
    job.header = build_media_header(meta);
    job.external_payload = static_cast<const uint8_t *>(payload);
    job.external_payload_size = payload_size;
    job.payload_owner = std::move(payload_owner);
    job.rgb_keyframe = stream_type == StreamType::rgb && (meta.flags & key_frame) != 0u;
    return job;
}

std::optional<RgbSnapshotRequest> take_next_rgb_snapshot_request(CameraRuntime &camera, uint64_t capture_host_timestamp_us) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    if(camera.rgb_snapshot_requests.empty()) {
        return std::nullopt;
    }
    if(camera.rgb_snapshot_requests.front().capture_not_before_unix_us > capture_host_timestamp_us) {
        return std::nullopt;
    }
    RgbSnapshotRequest request = std::move(camera.rgb_snapshot_requests.front());
    camera.rgb_snapshot_requests.pop_front();
    return request;
}

void reject_next_rgb_snapshot_request(const AppConfig &config, CameraRuntime &camera, const std::string &error, Logger &logger) {
    auto request = take_next_rgb_snapshot_request(camera, now_us());
    if(!request) {
        return;
    }
    logger.warn("rgb snapshot request failed request_id=" + request->request_id
                + " camera_id=" + camera.config.camera_id + " error=" + error);
    write_rgb_snapshot_result(config, camera.config.camera_id, *request, false, "error", "", error, logger);
}

bool rotate_rgb_snapshot_mjpeg_180(const std::vector<uint8_t> &jpeg,
                                   std::vector<uint8_t> &rotated_jpeg,
                                   std::string &error) {
    try {
        if(jpeg.empty() || jpeg.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
            error = "invalid MJPEG size";
            return false;
        }
        cv::Mat encoded(1, static_cast<int>(jpeg.size()), CV_8UC1, const_cast<uint8_t *>(jpeg.data()));
        cv::Mat decoded = cv::imdecode(encoded, cv::IMREAD_COLOR);
        if(decoded.empty()) {
            error = "MJPEG decode failed";
            return false;
        }
        cv::Mat rotated;
        cv::rotate(decoded, rotated, cv::ROTATE_180);
        const std::vector<int> parameters{cv::IMWRITE_JPEG_QUALITY, kRgbSnapshotJpegQuality};
        if(!cv::imencode(".jpg", rotated, rotated_jpeg, parameters)
           || !mjpg_has_complete_jpeg_data(rotated_jpeg.data(), rotated_jpeg.size())) {
            error = "rotated JPEG encode failed";
            return false;
        }
        return true;
    }
    catch(const cv::Exception &e) {
        error = e.what();
        return false;
    }
    catch(const std::exception &e) {
        error = e.what();
        return false;
    }
}

void publish_rgb_snapshot_frame(const AppConfig &config,
                                CameraRuntime &camera,
                                const uint8_t *jpeg_data,
                                size_t jpeg_size,
                                const RgbEncodeTiming &timing,
                                Logger &logger) {
    const uint64_t capture_host_timestamp_us =
        timing.capture_host_timestamp_us > 0 ? timing.capture_host_timestamp_us : now_us();
    auto request = take_next_rgb_snapshot_request(camera, capture_host_timestamp_us);
    if(!request) {
        return;
    }
    const size_t complete_jpeg_size = mjpg_complete_jpeg_size(jpeg_data, jpeg_size);
    if(complete_jpeg_size == 0) {
        const std::string error = "captured RGB frame is not a complete MJPEG image";
        logger.warn("rgb snapshot request failed request_id=" + request->request_id
                    + " camera_id=" + camera.config.camera_id + " error=" + error);
        write_rgb_snapshot_result(config, camera.config.camera_id, *request, false, "error", "", error, logger);
        return;
    }
    jpeg_size = complete_jpeg_size;

    ReliableSnapshotQueue *queue = nullptr;
    {
        std::lock_guard<std::mutex> lock(camera.mutex);
        queue = camera.rgb_snapshot_queue;
        if(queue) {
            camera.pending_rgb_snapshots.emplace(
                request->request_id, PendingRgbSnapshot{*request, std::chrono::steady_clock::now()});
        }
    }
    if(queue == nullptr) {
        write_rgb_snapshot_result(config, camera.config.camera_id, *request, false, "error", "",
                                  "snapshot media transport is unavailable", logger);
        return;
    }

    MediaFrameMeta meta;
    meta.stream_type = StreamType::rgb_snapshot;
    meta.flags = has_system_timestamp | has_rgb_diagnostics | has_pipeline_diagnostics;
    meta.sender_id = config.sender_id;
    meta.camera_id = camera.config.camera_id;
    meta.codec_or_compression = std::string(kRgbSnapshotCodecPrefix) + request->request_id;
    meta.frame_id = timing.frame_id;
    meta.timestamp_us = timing.device_timestamp_us;
    meta.system_timestamp_us = timing.system_timestamp_us;
    meta.pair_id = timing.pair_id;
    meta.width = timing.width;
    meta.height = timing.height;
    meta.pixel_format = PixelFormat::encoded_video;
    meta.payload_size = jpeg_size;
    meta.uncompressed_size = jpeg_size;
    meta.rgb_exposure_us = timing.diagnostics.exposure_us;
    meta.rgb_gain = timing.diagnostics.gain;
    meta.rgb_auto_exposure = timing.diagnostics.auto_exposure;
    meta.rgb_actual_fps = timing.diagnostics.actual_fps;
    meta.sender_capture_host_timestamp_us = timing.capture_host_timestamp_us;
    meta.sender_timing_bound_timestamp_us = timing.timing_bound_timestamp_us;
    meta.sender_packet_queued_timestamp_us = now_us();
    const int snapshot_rotation_degrees = software_rgb_rotation_degrees(camera.config);
    if(snapshot_rotation_degrees == 180) {
        meta.flags |= snapshot_orientation_applied;
    }

    std::vector<uint8_t> jpeg(jpeg_data, jpeg_data + jpeg_size);
    ReliableSnapshotJob job;
    job.media = make_owned_media_job(camera, StreamType::rgb_snapshot, meta, std::move(jpeg));
    job.meta = meta;
    job.request_id = request->request_id;
    job.rotation_degrees = snapshot_rotation_degrees;
    job.first_queued_at = std::chrono::steady_clock::now();
    job.next_attempt_at = job.first_queued_at;
    if(!queue->publish(std::move(job))) {
        {
            std::lock_guard<std::mutex> lock(camera.mutex);
            camera.pending_rgb_snapshots.erase(request->request_id);
        }
        const std::string error = "snapshot reliable media queue is full";
        logger.warn("rgb snapshot request failed request_id=" + request->request_id
                    + " camera_id=" + camera.config.camera_id + " error=" + error);
        write_rgb_snapshot_result(config, camera.config.camera_id, *request, false, "error", "", error, logger);
        return;
    }
    logger.info("rgb snapshot frame queued request_id=" + request->request_id
                + " camera_id=" + camera.config.camera_id
                + " frame_id=" + std::to_string(timing.frame_id)
                + " system_timestamp_us=" + std::to_string(timing.system_timestamp_us)
                + " jpeg_bytes=" + std::to_string(jpeg_size));
}

bool publish_media_job(LatestMediaQueue &media_queue, size_t slot_index, CameraRuntime &camera, StreamType stream_type,
                       MediaPacketJob &&job, Logger &logger, bool reject_if_occupied = false, bool packet_is_keyframe = false) {
    const auto result = media_queue.publish(slot_index, std::move(job), reject_if_occupied);
    if(result == LatestMediaQueue::PublishResult::queued) {
        return true;
    }
    if(result == LatestMediaQueue::PublishResult::overwritten || result == LatestMediaQueue::PublishResult::rejected_occupied) {
        record_queue_overwrite(camera, stream_type);
        if(stream_type == StreamType::rgb && !(result == LatestMediaQueue::PublishResult::overwritten && packet_is_keyframe)) {
            arm_rgb_keyframe_guard(camera, logger, "rgb queue overwrite; waiting for next keyframe");
        }
    }
    return result == LatestMediaQueue::PublishResult::overwritten;
}

template <typename Sender>
void mark_camera_disconnected(const AppConfig &config, CameraRuntime &camera, Sender &transport, Logger &logger,
                              std::mutex &transport_mutex, const std::string &error) {
    const bool should_reconnect = camera_reconnect_enabled(camera);
    const auto disconnected_at = std::chrono::steady_clock::now();
    const int flap_restart_events = camera_flap_restart_events();
    const int flap_window_seconds = camera_flap_window_seconds();
    size_t recent_disconnects = 0;
    bool restart_for_flapping = false;
    {
        std::lock_guard<std::mutex> lock(camera.mutex);
        camera.disconnects++;
        camera.reconnect_attempts = 0;
        camera.last_error = error;
        const auto oldest_allowed = disconnected_at - std::chrono::seconds(flap_window_seconds);
        while(!camera.recent_disconnects.empty() && camera.recent_disconnects.front() < oldest_allowed) {
            camera.recent_disconnects.pop_front();
        }
        camera.recent_disconnects.push_back(disconnected_at);
        recent_disconnects = camera.recent_disconnects.size();
        restart_for_flapping = flap_restart_events > 0 && recent_disconnects >= static_cast<size_t>(flap_restart_events);
    }
    logger.error("camera disconnected camera_id=" + camera.config.camera_id + " error=" + error);
    if(restart_for_flapping) {
        logger.error("camera flap guard exiting sender for watchdog restart camera_id=" + camera.config.camera_id
                     + " events=" + std::to_string(recent_disconnects)
                     + " threshold=" + std::to_string(flap_restart_events)
                     + " window_s=" + std::to_string(flap_window_seconds));
        std::_Exit(75);
    }
    send_status_locked(transport, logger, transport_mutex, camera_offline_message(config, camera.config.camera_id, error));
    stop_camera(camera, logger);
    if(should_reconnect) {
        set_camera_reconnect_delay(camera, std::chrono::steady_clock::now() + std::chrono::seconds(1));
    }
    else {
        logger.info("hotplug camera retired camera_id=" + camera.config.camera_id + " reason=" + error);
    }
}

template <typename Sender>
void retry_camera_reconnect(const AppConfig &config, CameraRuntime &camera, Sender &transport, Logger &logger,
                            std::mutex &transport_mutex, std::chrono::steady_clock::time_point now) {
    uint32_t attempt = 0;
    {
        std::lock_guard<std::mutex> lock(camera.mutex);
        if(now < camera.next_reconnect) {
            return;
        }
        camera.reconnect_attempts++;
        attempt = camera.reconnect_attempts;
    }

    logger.warn("camera reconnect attempt camera_id=" + camera.config.camera_id + " attempt=" + std::to_string(attempt));
    try {
        start_camera_runtime(camera, logger);
        {
            std::lock_guard<std::mutex> lock(camera.mutex);
            camera.reconnect_attempts = 0;
        }
        logger.info("camera reconnected camera_id=" + camera.config.camera_id);
        send_status_locked(transport, logger, transport_mutex,
                           event_message(config, "info", "camera_reconnected", "camera pipeline restarted", camera.config.camera_id));
    }
    catch(const ob::Error &e) {
        stop_camera(camera, logger);
        const std::string error = ob_error_text(e);
        uint32_t attempts = 0;
        {
            std::lock_guard<std::mutex> lock(camera.mutex);
            camera.last_error = error;
            attempts = camera.reconnect_attempts;
            camera.next_reconnect = now + reconnect_delay(camera.reconnect_attempts);
        }
        logger.warn("camera reconnect failed camera_id=" + camera.config.camera_id + " attempt=" + std::to_string(attempts)
                    + " error=" + error);
        send_status_locked(transport, logger, transport_mutex, camera_offline_message(config, camera.config.camera_id, error));
        update_camera_reconnect_process_guard(camera, logger, attempts, error);
    }
    catch(const std::exception &e) {
        stop_camera(camera, logger);
        const std::string error = e.what();
        uint32_t attempts = 0;
        {
            std::lock_guard<std::mutex> lock(camera.mutex);
            camera.last_error = error;
            attempts = camera.reconnect_attempts;
            camera.next_reconnect = now + reconnect_delay(camera.reconnect_attempts);
        }
        logger.warn("camera reconnect failed camera_id=" + camera.config.camera_id + " attempt=" + std::to_string(attempts)
                    + " error=" + error);
        send_status_locked(transport, logger, transport_mutex, camera_offline_message(config, camera.config.camera_id, error));
        update_camera_reconnect_process_guard(camera, logger, attempts, error);
    }
}

template <typename Sender>
void media_sender_loop(LatestMediaQueue &media_queue, Sender &transport, Logger &logger, std::mutex &transport_mutex,
                       const std::atomic<bool> *path_running = nullptr, bool reliable_retry = false) {
    auto next_idle_close_log = std::chrono::steady_clock::now();
    std::optional<MediaPacketJob> retry_job;
    std::optional<std::chrono::steady_clock::time_point> drain_deadline;
    while(true) {
        const bool producers_active = path_running == nullptr ? g_running.load() : path_running->load();
        if(!producers_active && !drain_deadline) {
            drain_deadline = std::chrono::steady_clock::now() + kGracefulQueueDrainTimeout;
        }
        if(!producers_active && !retry_job && media_queue.empty()) {
            break;
        }
        if(drain_deadline && std::chrono::steady_clock::now() >= *drain_deadline) {
            logger.warn("media sender queue drain timed out; remaining packets discarded");
            break;
        }
        std::optional<MediaPacketJob> job;
        if(retry_job) {
            job.emplace(std::move(*retry_job));
            retry_job.reset();
        }
        else {
            job = media_queue.wait_pop(std::chrono::milliseconds(100));
        }
        if(!job || !job->camera) {
            bool closed = false;
            {
                std::lock_guard<std::mutex> lock(transport_mutex);
                closed = transport.close_if_media_peer_closed();
            }
            if(closed) {
                const auto now = std::chrono::steady_clock::now();
                if(now >= next_idle_close_log) {
                    logger.info("media TCP idle peer close detected; socket will reconnect on next packet");
                    next_idle_close_log = now + std::chrono::seconds(5);
                }
            }
            continue;
        }

        if(job->stream_type == StreamType::rgb
           && decide_rgb_keyframe_send(*job->camera, job->rgb_keyframe, std::chrono::steady_clock::now(), logger)
                  == RgbTransportRecovery::SendDecision::drop) {
            continue;
        }

        const auto send_started = std::chrono::steady_clock::now();
        bool sent = false;
        std::string error;
        const auto packet = job->view();
        try {
            std::lock_guard<std::mutex> lock(transport_mutex);
            sent = transport.send_media(packet);
            if(!sent) {
                error = transport.last_error();
            }
        }
        catch(const std::exception &e) {
            error = e.what();
        }
        catch(...) {
            error = "unknown media transport exception";
        }
        const auto send_ended = std::chrono::steady_clock::now();
        const double send_ms = elapsed_ms(send_started, send_ended);
        if(sent) {
            record_media_send_success(*job->camera, job->stream_type, job->total_size(), send_ms);
            if(job->stream_type == StreamType::rgb) {
                complete_rgb_keyframe_recovery(*job->camera, logger, job->rgb_keyframe);
            }
        }
        else {
            record_media_send_failure(*job->camera, logger, job->stream_type, send_ms, send_ended, error);
            if(job->stream_type == StreamType::rgb) {
                arm_rgb_keyframe_guard(*job->camera, logger, error);
                maybe_request_rgb_recovery_keyframe(*job->camera, logger, send_ended);
                const bool retain_keyframe = reliable_retry && job->rgb_keyframe
                                             && (!drain_deadline
                                                 || std::chrono::steady_clock::now() < *drain_deadline);
                if(retain_keyframe) {
                    retry_job = std::move(*job);
                }
                else {
                    std::lock_guard<std::mutex> lock(job->camera->mutex);
                    job->camera->rgb_dropped++;
                    job->camera->rgb_transport_retry_drops++;
                }
            }
            if(job->stream_type == StreamType::depth_raw
               && (!drain_deadline || std::chrono::steady_clock::now() < *drain_deadline)) {
                retry_job = std::move(*job);
            }
            if(retry_job) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
    }
}

void fail_pending_rgb_snapshot(const AppConfig &config,
                               CameraRuntime &camera,
                               const std::string &request_id,
                               const std::string &error,
                               Logger &logger) {
    std::optional<RgbSnapshotRequest> request;
    {
        std::lock_guard<std::mutex> lock(camera.mutex);
        const auto pending = camera.pending_rgb_snapshots.find(request_id);
        if(pending == camera.pending_rgb_snapshots.end()) {
            return;
        }
        request = pending->second.request;
        camera.pending_rgb_snapshots.erase(pending);
    }
    logger.warn("rgb snapshot transport failed request_id=" + request_id
                + " camera_id=" + camera.config.camera_id + " error=" + error);
    write_rgb_snapshot_result(config, camera.config.camera_id, *request, false, "error", "", error, logger);
}

template <typename Sender>
void rgb_snapshot_sender_loop(const AppConfig &config,
                              ReliableSnapshotQueue &queue,
                              Sender &transport,
                              Logger &logger,
                              std::atomic<bool> &running) {
    while(running.load() && g_running.load()) {
        auto job = queue.wait_pop(std::chrono::milliseconds(100));
        if(!job) {
            continue;
        }
        if(!job->media.camera) {
            continue;
        }
        const auto now = std::chrono::steady_clock::now();
        if(now - job->first_queued_at >= kRgbSnapshotRequestTimeout) {
            fail_pending_rgb_snapshot(config, *job->media.camera, job->request_id,
                                      "receiver capture acknowledgement timeout", logger);
            continue;
        }
        if(job->rotation_degrees == 180) {
            const auto rotation_started = std::chrono::steady_clock::now();
            std::vector<uint8_t> rotated_jpeg;
            std::string rotation_error;
            if(!rotate_rgb_snapshot_mjpeg_180(job->media.owned_payload, rotated_jpeg, rotation_error)) {
                fail_pending_rgb_snapshot(config, *job->media.camera, job->request_id,
                                          "snapshot rotation failed: " + rotation_error, logger);
                continue;
            }
            job->media.owned_payload = std::move(rotated_jpeg);
            job->meta.payload_size = job->media.owned_payload.size();
            job->meta.uncompressed_size = job->media.owned_payload.size();
            job->media.header = build_media_header(job->meta);
            job->rotation_degrees = 0;
            logger.info("rgb snapshot orientation applied request_id=" + job->request_id
                        + " camera_id=" + job->media.camera->config.camera_id
                        + " degrees=180"
                        + " jpeg_bytes=" + std::to_string(job->media.owned_payload.size())
                        + " elapsed_ms=" + std::to_string(
                              elapsed_ms(rotation_started, std::chrono::steady_clock::now())));
        }

        bool sent = false;
        std::string error;
        try {
            sent = transport.send_media(job->media.view());
            if(!sent) {
                error = transport.last_error();
            }
        }
        catch(const std::exception &e) {
            error = e.what();
        }
        catch(...) {
            error = "unknown snapshot media transport exception";
        }
        ++job->attempts;
        if(sent && job->attempts == 1) {
            logger.info("rgb snapshot media sent request_id=" + job->request_id
                        + " camera_id=" + job->media.camera->config.camera_id
                        + " bytes=" + std::to_string(job->media.total_size()));
        }
        else if(!sent && (job->attempts <= 3 || job->attempts % 10 == 0)) {
            logger.warn("rgb snapshot media retry request_id=" + job->request_id
                        + " camera_id=" + job->media.camera->config.camera_id
                        + " attempt=" + std::to_string(job->attempts)
                        + " error=" + (error.empty() ? "send failed" : error));
        }

        CameraRuntime *camera = job->media.camera;
        const std::string request_id = job->request_id;
        if(!queue.requeue(std::move(*job), std::chrono::steady_clock::now() + kRgbSnapshotRetryInterval)
           && running.load() && g_running.load()) {
            fail_pending_rgb_snapshot(config, *camera, request_id, "snapshot retry queue unavailable", logger);
        }
    }
}

void depth_compression_loop(LatestDepthCompressionQueue &depth_queue, Logger &logger, size_t worker_index) {
    std::optional<std::chrono::steady_clock::time_point> drain_deadline;
    while(true) {
        const bool producers_active = !depth_queue.stopping();
        if(!producers_active && !drain_deadline) {
            drain_deadline = std::chrono::steady_clock::now() + kGracefulQueueDrainTimeout;
        }
        if(!producers_active && depth_queue.empty()) {
            break;
        }
        if(drain_deadline && std::chrono::steady_clock::now() >= *drain_deadline) {
            logger.warn("depth compression queue drain timed out worker=" + std::to_string(worker_index));
            break;
        }
        auto work = depth_queue.wait_pop(std::chrono::milliseconds(100));
        if(!work) {
            continue;
        }
        auto &job = work->job;
        if(!job.source_camera || !job.output_camera || !job.output_media_queue) {
            depth_queue.complete(work->slot_index);
            continue;
        }

        try {
            const auto compress_started = std::chrono::steady_clock::now();
            auto compressed_depth =
                compress_depth_payload(job.meta.codec_or_compression, job.raw_payload, job.raw_payload_size, job.depth_scale,
                                       job.quantization_step_mm);
            record_depth_compress_ms(*job.source_camera, elapsed_ms(compress_started, std::chrono::steady_clock::now()));
            job.meta.payload_size = compressed_depth.size();
            job.meta.uncompressed_size = job.raw_payload_size;
            auto media_job = make_owned_media_job(*job.output_camera, StreamType::depth_raw, job.meta, std::move(compressed_depth));
            publish_media_job(*job.output_media_queue, job.media_slot_index, *job.output_camera, StreamType::depth_raw, std::move(media_job),
                              logger);
            record_depth_frame_done(*job.output_camera);
        }
        catch(const std::exception &e) {
            set_camera_last_error(*job.source_camera, e.what());
            logger.warn("depth compression failed worker=" + std::to_string(worker_index)
                        + " camera_id=" + job.source_camera->config.camera_id + " error=" + e.what());
        }
        catch(...) {
            set_camera_last_error(*job.source_camera, "unknown depth compression exception");
            logger.warn("depth compression failed worker=" + std::to_string(worker_index)
                        + " camera_id=" + job.source_camera->config.camera_id + " error=unknown exception");
        }
        depth_queue.complete(work->slot_index);
    }
}

void publish_rgb_encoded_units(const AppConfig &config, CameraRuntime &camera, LatestMediaQueue &main_media_queue, size_t rgb_slot,
                               std::vector<EncodedH264Frame> &encoded_units, RgbEncodeTiming submitted_timing,
                               uint64_t encode_done_timestamp_us, std::chrono::steady_clock::time_point frame_now, Logger &logger) {
    for(auto &encoded : encoded_units) {
        const bool encoded_has_vcl = h264_payload_has_vcl_nal(encoded.data);
        const bool is_key_frame = h264_payload_has_idr(encoded.data);
        if(camera.config.rgb_rtp_output.enabled && !encoded.data.empty()) {
            if(!camera.rgb_rtp_sender) {
                camera.rgb_rtp_sender = std::make_unique<GstH264RtpSender>(camera.config.rgb_rtp_output,
                                                                           camera.config.rgb_profile.fps);
                if(camera.rgb_rtp_sender->ok()) {
                    logger.info("rgb RTP sender ready camera_id=" + camera.config.camera_id
                                + " target=" + camera.config.rgb_rtp_output.host + ":"
                                + std::to_string(camera.config.rgb_rtp_output.port));
                }
                else {
                    logger.warn("rgb RTP sender unavailable camera_id=" + camera.config.camera_id
                                + " error=" + camera.rgb_rtp_sender->error());
                }
            }
            const uint64_t rtp_timestamp_us = encoded.has_pts ? encoded.pts_us : submitted_timing.system_timestamp_us;
            if(!camera.rgb_rtp_sender->push(encoded.data.data(), encoded.data.size(), rtp_timestamp_us)) {
                const std::string error = camera.rgb_rtp_sender->error();
                camera.rgb_rtp_sender.reset();
                if(frame_now >= camera.next_rgb_rtp_warning) {
                    camera.next_rgb_rtp_warning = frame_now + std::chrono::seconds(2);
                    logger.warn("rgb RTP send failed; pipeline will be rebuilt camera_id="
                                + camera.config.camera_id + " error=" + error);
                }
            }
        }
        if(is_key_frame) {
            report_forced_rgb_keyframe(camera, logger);
        }
        const auto send_decision = decide_rgb_keyframe_send(camera, is_key_frame, frame_now, logger);
        if(send_decision == RgbTransportRecovery::SendDecision::drop) {
            continue;
        }
        const auto timing_resolution = resolve_rgb_encode_timing(camera, encoded, submitted_timing, encoded_has_vcl);
        maybe_log_rgb_timing_resolution(camera, logger, encoded, timing_resolution, frame_now);
        auto timing = timing_resolution.timing;
        if(timing.encode_start_timestamp_us == 0) {
            timing.encode_start_timestamp_us = submitted_timing.encode_start_timestamp_us;
        }
        timing.encode_done_timestamp_us = encode_done_timestamp_us;
        if(encoded_has_vcl && rgb_encoder_output_lag_too_large(camera, logger, timing, encode_done_timestamp_us, frame_now)) {
            return;
        }
        if(encoded_has_vcl && !rgb_encoded_timing_is_monotonic(camera, logger, timing, frame_now)) {
            continue;
        }
        MediaFrameMeta meta;
        meta.stream_type = StreamType::rgb;
        meta.flags = has_system_timestamp | has_rgb_diagnostics | (is_key_frame ? key_frame : 0u);
        if(send_decision == RgbTransportRecovery::SendDecision::send_recovery_keyframe) {
            meta.flags |= dropped_before;
        }
        meta.sender_id = config.sender_id;
        meta.camera_id = camera.config.camera_id;
        meta.codec_or_compression = "h264";
        meta.frame_id = timing.frame_id;
        meta.timestamp_us = timing.device_timestamp_us;
        meta.system_timestamp_us = timing.system_timestamp_us;
        meta.pair_id = timing.pair_id;
        meta.width = timing.width;
        meta.height = timing.height;
        meta.pixel_format = PixelFormat::encoded_video;
        meta.payload_size = encoded.data.size();
        meta.uncompressed_size = encoded.data.size();
        meta.rgb_exposure_us = timing.diagnostics.exposure_us;
        meta.rgb_gain = timing.diagnostics.gain;
        meta.rgb_auto_exposure = timing.diagnostics.auto_exposure;
        meta.rgb_actual_fps = timing.diagnostics.actual_fps;
        timing.packet_queued_timestamp_us = now_us();
        set_rgb_pipeline_diagnostics(meta, timing);
        const bool reject_if_occupied = !config.recording_buffer.enabled && !is_key_frame;
        auto job = make_owned_media_job(camera, StreamType::rgb, meta, std::move(encoded.data));
        if(publish_media_job(main_media_queue, rgb_slot, camera, StreamType::rgb, std::move(job), logger, reject_if_occupied, is_key_frame)
           && encoded_has_vcl) {
            mark_rgb_encoded_timing_queued(camera, timing);
        }
    }
}

void publish_rgb_preview_units(const AppConfig &config, CameraRuntime &camera, LatestMediaQueue &preview_media_queue, size_t rgb_preview_slot,
                               std::vector<EncodedH264Frame> &preview_units, RgbEncodeTiming preview_timing, uint32_t preview_width,
                               uint32_t preview_height, Logger &logger) {
    for(auto &encoded : preview_units) {
        MediaFrameMeta meta;
        const bool is_key_frame = h264_payload_has_idr(encoded.data);
        meta.stream_type = StreamType::rgb_preview;
        meta.flags = has_system_timestamp | has_rgb_diagnostics | (is_key_frame ? key_frame : 0u);
        meta.sender_id = config.sender_id;
        meta.camera_id = camera.config.camera_id;
        meta.codec_or_compression = "h264";
        meta.frame_id = preview_timing.frame_id;
        meta.timestamp_us = preview_timing.device_timestamp_us;
        meta.system_timestamp_us = preview_timing.system_timestamp_us;
        meta.pair_id = preview_timing.pair_id;
        meta.width = preview_width;
        meta.height = preview_height;
        meta.pixel_format = PixelFormat::encoded_video;
        meta.payload_size = encoded.data.size();
        meta.uncompressed_size = encoded.data.size();
        meta.rgb_exposure_us = preview_timing.diagnostics.exposure_us;
        meta.rgb_gain = preview_timing.diagnostics.gain;
        meta.rgb_auto_exposure = preview_timing.diagnostics.auto_exposure;
        meta.rgb_actual_fps = preview_timing.diagnostics.actual_fps;
        preview_timing.packet_queued_timestamp_us = now_us();
        set_rgb_pipeline_diagnostics(meta, preview_timing);
        auto job = make_owned_media_job(camera, StreamType::rgb_preview, meta, std::move(encoded.data));
        publish_media_job(preview_media_queue, rgb_preview_slot, camera, StreamType::rgb_preview, std::move(job), logger);
    }
}

size_t depth_compression_worker_count(size_t camera_count) {
    if(camera_count <= 1) {
        return 1;
    }
    const unsigned int hardware_threads = std::thread::hardware_concurrency();
    const size_t hardware_limit = hardware_threads == 0 ? camera_count : static_cast<size_t>(hardware_threads);
    return std::max<size_t>(1, std::min(camera_count, hardware_limit));
}

std::mutex g_encoder_create_mutex;

CameraRuntime *depth_target_camera(const AppConfig &config, CameraRuntime &source) {
    if(!config.swap_depth_between_cameras || !camera_is_online(source) || source.depth_remap_target == nullptr
       || !camera_is_online(*source.depth_remap_target)) {
        return &source;
    }
    return source.depth_remap_target;
}

void configure_depth_remap_targets(const AppConfig &config, const std::vector<std::unique_ptr<CameraRuntime>> &cameras, Logger &logger) {
    if(!config.swap_depth_between_cameras) {
        return;
    }
    if(cameras.size() != 2) {
        logger.warn("depth stream remap requested but disabled at runtime because camera_count=" + std::to_string(cameras.size()));
        return;
    }
    cameras[0]->depth_remap_target = cameras[1].get();
    cameras[1]->depth_remap_target = cameras[0].get();
    logger.warn("depth stream remap enabled: source " + cameras[0]->config.camera_id + " depth is sent/previewed as "
                + cameras[1]->config.camera_id + "; source " + cameras[1]->config.camera_id + " depth is sent/previewed as "
                + cameras[0]->config.camera_id);
}

template <typename Sender>
void v4l2_camera_worker_loop(const AppConfig &config, CameraRuntime &camera, size_t primary_slot_base, size_t preview_slot_base,
                             LatestMediaQueue &rgb_media_queue, LatestMediaQueue &preview_media_queue, Sender &transport, Logger &logger,
                             std::mutex &transport_mutex, std::chrono::milliseconds preview_interval) {
    const size_t rgb_slot = primary_slot_base;
    const size_t rgb_preview_slot = preview_slot_base + 2;

    while(g_running) {
        const auto loop_now = std::chrono::steady_clock::now();
        if(!camera_is_online(camera)) {
            if(camera_reconnect_enabled(camera)) {
                retry_camera_reconnect(config, camera, transport, logger, transport_mutex, loop_now);
            }
            else {
                logger.info("v4l2 camera worker exiting camera_id=" + camera.config.camera_id + " reason=reconnect_disabled");
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        std::shared_ptr<V4L2MjpegCapture> capture;
        {
            std::lock_guard<std::mutex> lock(camera.mutex);
            capture = camera.v4l2_capture;
        }
        if(!capture) {
            mark_camera_disconnected(config, camera, transport, logger, transport_mutex, "v4l2 capture is not open");
            continue;
        }

        V4L2MjpegCapture::Frame frame;
        try {
            const auto wait_started = std::chrono::steady_clock::now();
            const bool has_frame = capture->wait_frame(frame, std::chrono::milliseconds(100));
            const auto wait_ended = std::chrono::steady_clock::now();
            record_wait_result(camera, elapsed_ms(wait_started, wait_ended), !has_frame);
            if(!has_frame) {
                if(auto reason = capture_stream_stall_reason(camera, wait_ended)) {
                    mark_camera_disconnected(config, camera, transport, logger, transport_mutex, *reason);
                }
                continue;
            }
        }
        catch(const std::exception &e) {
            mark_camera_disconnected(config, camera, transport, logger, transport_mutex, e.what());
            continue;
        }

        try {
            const auto frame_now = std::chrono::steady_clock::now();
            bool preview_due = false;
            {
                std::lock_guard<std::mutex> lock(camera.mutex);
                preview_due = config.preview.enabled && frame_now >= camera.next_preview;
            }
            const bool web_preview_due = web_rgb_preview_emit_due(config, camera, frame_now);
            const bool web_preview_can_queue = web_preview_due;

            record_rgb_input(camera, frame.size, frame.frame_id);
            RgbFrameDiagnostics diagnostics;
            RgbEncodeTiming rgb_capture_timing{frame.frame_id,
                                               frame.system_timestamp_us,
                                               frame.system_timestamp_us,
                                               0,
                                               frame.width,
                                               frame.height,
                                               diagnostics};
            rgb_capture_timing.capture_host_timestamp_us = frame.system_timestamp_us;
            rgb_capture_timing.timing_bound_timestamp_us = now_us();

            bool rgb_usable = true;
            if(!mjpg_has_complete_jpeg_data(frame.data, frame.size)) {
                rgb_usable = false;
                mark_corrupt_rgb_jpeg_frame(camera, logger, frame.frame_id, frame.size, frame_now, "missing jpeg soi/eoi marker");
            }
            else if(camera.config.validate_rgb_mjpeg) {
                std::string jpeg_validation_message;
                if(!mjpg_decodes_cleanly_data(frame.data, frame.size, jpeg_validation_message)) {
                    rgb_usable = false;
                    mark_corrupt_rgb_jpeg_frame(camera, logger, frame.frame_id, frame.size, frame_now, jpeg_validation_message);
                }
            }

            cv::Mat bgr;
            if(rgb_usable) {
                publish_rgb_snapshot_frame(config, camera, frame.data, frame.size, rgb_capture_timing, logger);
                if(preview_due) {
                    const auto decode_started = std::chrono::steady_clock::now();
                    auto preview_bgr = mjpg_buffer_to_bgr(frame.data, frame.size, cv::IMREAD_REDUCED_COLOR_2);
                    record_rgb_decode_ms(camera, elapsed_ms(decode_started, std::chrono::steady_clock::now()));
                    set_latest_bgr(camera, preview_bgr, frame.frame_id, frame.system_timestamp_us,
                                   software_rgb_rotation_degrees(camera.config));
                }

                if(!camera.encoder) {
                    auto input_format = GstH264InputFormat::Jpeg;
                    {
                        std::lock_guard<std::mutex> encoder_lock(g_encoder_create_mutex);
                        camera.encoder = std::make_unique<GstH264Encoder>(frame.width, frame.height, camera.config.rgb_profile.fps,
                                                                            camera.config.rgb_encoding.bitrate_bps,
                                                                            camera.config.rgb_encoding.gstreamer_encoder, input_format);
                        if(!camera.encoder->ok()) {
                            logger.warn("v4l2 jpeg rgb path unavailable, falling back to BGR encode path camera_id="
                                        + camera.config.camera_id + " error=" + camera.encoder->error());
                            input_format = GstH264InputFormat::Bgr;
                            camera.encoder = std::make_unique<GstH264Encoder>(frame.width, frame.height, camera.config.rgb_profile.fps,
                                                                                camera.config.rgb_encoding.bitrate_bps,
                                                                                camera.config.rgb_encoding.gstreamer_encoder, input_format);
                        }
                    }
                    {
                        std::lock_guard<std::mutex> lock(camera.mutex);
                        camera.encoder_input_format = input_format;
                        camera.hardware_encoder = camera.encoder->ok();
                        if(!camera.hardware_encoder) {
                            camera.last_error = camera.encoder->error();
                        }
                    }
                    if(!camera.encoder->ok()) {
                        const auto error = camera.encoder->error();
                        logger.error("encoder_init_failed: " + error);
                        send_status_locked(transport, logger, transport_mutex,
                                           event_message(config, "error", "encoder_init_failed", error, camera.config.camera_id));
                    }
                    else {
                        logger.info("v4l2 rgb encoder ready camera_id=" + camera.config.camera_id
                                    + " source=" + std::to_string(frame.width) + "x" + std::to_string(frame.height)
                                    + " fps=" + std::to_string(camera.config.rgb_profile.fps)
                                    + " input=" + std::string(input_format == GstH264InputFormat::Jpeg ? "mjpg" : "bgr"));
                    }
                }

                if(web_preview_due && !camera.web_preview_encoder && !camera.encoder && false) {
                    // Unreachable; kept intentionally empty so V4L2 preview never blocks main encoder creation.
                }
                if(web_preview_due && !camera.web_preview_encoder && camera.encoder) {
                    const auto shape = resolve_web_rgb_preview_shape(config.web_rgb_preview, frame.width, frame.height);
                    if(shape.width > 0 && shape.height > 0) {
                        auto input_format = GstH264InputFormat::Jpeg;
                        {
                            std::lock_guard<std::mutex> encoder_lock(g_encoder_create_mutex);
                            camera.web_preview_encoder = std::make_unique<GstH264Encoder>(
                                frame.width, frame.height, config.web_rgb_preview.fps, config.web_rgb_preview.bitrate_bps,
                                camera.config.rgb_encoding.gstreamer_encoder, input_format, shape.width, shape.height);
                            if(!camera.web_preview_encoder->ok()) {
                                logger.warn("v4l2 jpeg web rgb preview path unavailable, falling back to BGR encode path camera_id="
                                            + camera.config.camera_id + " error=" + camera.web_preview_encoder->error());
                                input_format = GstH264InputFormat::Bgr;
                                camera.web_preview_encoder = std::make_unique<GstH264Encoder>(
                                    frame.width, frame.height, config.web_rgb_preview.fps, config.web_rgb_preview.bitrate_bps,
                                    camera.config.rgb_encoding.gstreamer_encoder, input_format, shape.width, shape.height);
                            }
                        }
                        {
                            std::lock_guard<std::mutex> lock(camera.mutex);
                            camera.web_preview_encoder_input_format = input_format;
                            if(camera.web_preview_encoder->ok()) {
                                camera.web_preview_width = static_cast<uint32_t>(camera.web_preview_encoder->output_width());
                                camera.web_preview_height = static_cast<uint32_t>(camera.web_preview_encoder->output_height());
                            }
                            else {
                                camera.web_preview_width = 0;
                                camera.web_preview_height = 0;
                                camera.last_error = camera.web_preview_encoder->error();
                            }
                        }
                    }
                }

                if(camera.encoder && camera.encoder->ok()) {
                    const auto input_format = encoder_input_format_for(camera);
                    if(input_format == GstH264InputFormat::Bgr && bgr.empty()) {
                        const auto decode_started = std::chrono::steady_clock::now();
                        bgr = mjpg_buffer_to_bgr(frame.data, frame.size, cv::IMREAD_COLOR);
                        record_rgb_decode_ms(camera, elapsed_ms(decode_started, std::chrono::steady_clock::now()));
                    }
                    if(input_format == GstH264InputFormat::Bgr && bgr.empty()) {
                        set_camera_last_error(camera, "v4l2 rgb decode produced empty frame");
                        record_queue_overwrite(camera, StreamType::rgb);
                    }
                    else {
                        uint64_t force_keyframe_request_id = 0;
                        if(consume_rgb_keyframe_request(camera, frame.system_timestamp_us,
                                                        force_keyframe_request_id)) {
                            camera.encoder->request_keyframe();
                            logger.info("rgb keyframe force event queued camera_id=" + camera.config.camera_id
                                        + " request_id=" + std::to_string(force_keyframe_request_id));
                        }
                        RgbEncodeTiming submitted_timing = rgb_capture_timing;
                        submitted_timing.encode_start_timestamp_us = now_us();
                        remember_rgb_encode_timing(camera, submitted_timing);
                        const auto encode_started = std::chrono::steady_clock::now();
                        auto encoded_units = input_format == GstH264InputFormat::Jpeg
                                                 ? camera.encoder->encode_jpeg(frame.data, frame.size, frame.system_timestamp_us)
                                                 : camera.encoder->encode_bgr(bgr, frame.system_timestamp_us);
                        const uint64_t encode_done_timestamp_us = now_us();
                        submitted_timing.encode_done_timestamp_us = encode_done_timestamp_us;
                        record_rgb_encode_ms(camera, elapsed_ms(encode_started, std::chrono::steady_clock::now()));
                        publish_rgb_encoded_units(config, camera, rgb_media_queue, rgb_slot, encoded_units, submitted_timing,
                                                  encode_done_timestamp_us, frame_now, logger);

                        if(web_preview_can_queue && camera.web_preview_encoder && camera.web_preview_encoder->ok()) {
                            const auto preview_input_format = web_preview_encoder_input_format_for(camera);
                            if(preview_input_format == GstH264InputFormat::Bgr && bgr.empty()) {
                                const auto decode_started = std::chrono::steady_clock::now();
                                bgr = mjpg_buffer_to_bgr(frame.data, frame.size, cv::IMREAD_COLOR);
                                record_rgb_decode_ms(camera, elapsed_ms(decode_started, std::chrono::steady_clock::now()));
                            }
                            if(preview_input_format == GstH264InputFormat::Bgr && bgr.empty()) {
                                set_camera_last_error(camera, "v4l2 rgb preview decode produced empty frame");
                            }
                            else {
                                RgbEncodeTiming preview_timing = rgb_capture_timing;
                                preview_timing.encode_start_timestamp_us = now_us();
                                auto preview_units = preview_input_format == GstH264InputFormat::Jpeg
                                                         ? camera.web_preview_encoder->encode_jpeg(frame.data, frame.size,
                                                                                                   frame.system_timestamp_us)
                                                         : camera.web_preview_encoder->encode_bgr(bgr, frame.system_timestamp_us);
                                preview_timing.encode_done_timestamp_us = now_us();
                                publish_rgb_preview_units(config, camera, preview_media_queue, rgb_preview_slot, preview_units, preview_timing,
                                                          static_cast<uint32_t>(camera.web_preview_encoder->output_width()),
                                                          static_cast<uint32_t>(camera.web_preview_encoder->output_height()), logger);
                            }
                        }
                    }
                }
            }
            record_rgb_frame_done(camera);

            if(auto reason = capture_stream_stall_reason(camera, frame_now)) {
                mark_camera_disconnected(config, camera, transport, logger, transport_mutex, *reason);
                capture->release_frame(frame);
                continue;
            }

            if(!camera_announced(camera)) {
                send_status_locked(transport, logger, transport_mutex, camera_announce(config, camera));
                set_camera_announced(camera, true);
            }
            bool time_sync_log_due = false;
            {
                std::lock_guard<std::mutex> lock(camera.mutex);
                time_sync_log_due = frame_now >= camera.next_time_sync_log;
                if(time_sync_log_due) {
                    camera.next_time_sync_log = frame_now + std::chrono::seconds(5);
                }
            }
            if(time_sync_log_due) {
                logger.info("time_sync camera_id=" + camera.config.camera_id
                            + " host_now_us=" + std::to_string(now_us())
                            + " rgb_present=1 rgb_frame_id=" + std::to_string(frame.frame_id)
                            + " rgb_canonical_timestamp_us=" + std::to_string(frame.system_timestamp_us)
                            + " rgb_device_timestamp_us=0"
                            + " rgb_system_timestamp_us=" + std::to_string(frame.system_timestamp_us)
                            + " depth_present=0");
            }
            if(preview_due) {
                std::lock_guard<std::mutex> lock(camera.mutex);
                camera.next_preview = frame_now + preview_interval;
            }
        }
        catch(const std::exception &e) {
            set_camera_last_error(camera, e.what());
            logger.error("v4l2 rgb processing failed camera_id=" + camera.config.camera_id + " error=" + e.what());
        }
        capture->release_frame(frame);
    }
}

template <typename Sender>
void camera_worker_loop(const AppConfig &config, CameraRuntime &camera, size_t primary_slot_base, size_t preview_slot_base,
                        size_t compression_slot_base, LatestMediaQueue &rgb_media_queue, LatestMediaQueue &depth_media_queue,
                        LatestMediaQueue &preview_media_queue,
                        LatestDepthCompressionQueue &depth_compression_queue, Sender &transport, Logger &logger,
                        std::mutex &transport_mutex, std::chrono::milliseconds preview_interval) {
    if(camera.config.capture_backend == "v4l2") {
        v4l2_camera_worker_loop(config, camera, primary_slot_base, preview_slot_base, rgb_media_queue, preview_media_queue, transport, logger,
                                transport_mutex, preview_interval);
        return;
    }

    const size_t rgb_slot = primary_slot_base;
    const size_t depth_slot = primary_slot_base + 1;
    const size_t rgb_preview_slot = preview_slot_base + 2;
    const size_t depth_compression_slot = compression_slot_base + 1;

    while(g_running) {
        const auto loop_now = std::chrono::steady_clock::now();
        if(!camera_is_online(camera)) {
            if(camera_reconnect_enabled(camera)) {
                retry_camera_reconnect(config, camera, transport, logger, transport_mutex, loop_now);
            }
            else {
                logger.info("camera worker exiting camera_id=" + camera.config.camera_id + " reason=reconnect_disabled");
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        std::shared_ptr<ob::FrameSet> frameset;
        std::shared_ptr<ob::ColorFrame> color;
        std::shared_ptr<ob::DepthFrame> depth;
        CameraRuntime *depth_output_camera = nullptr;
        try {
            const auto wait_started = std::chrono::steady_clock::now();
            frameset = camera.pipeline->waitForFrames(100);
            const auto wait_ended = std::chrono::steady_clock::now();
            record_wait_result(camera, elapsed_ms(wait_started, wait_ended), !frameset);
            if(!frameset) {
                if(auto reason = capture_stream_stall_reason(camera, wait_ended)) {
                    mark_camera_disconnected(config, camera, transport, logger, transport_mutex, *reason);
                }
                continue;
            }
            color = frameset->colorFrame();
            depth = frameset->depthFrame();
        }
        catch(const ob::Error &e) {
            mark_camera_disconnected(config, camera, transport, logger, transport_mutex, ob_error_text(e));
            continue;
        }
        catch(const std::exception &e) {
            mark_camera_disconnected(config, camera, transport, logger, transport_mutex, e.what());
            continue;
        }

        const auto frame_now = std::chrono::steady_clock::now();
        const uint64_t frame_host_now_us = now_us();
        bool reapply_gemini305_manual_exposure = false;
        std::string reapply_device_model;
        std::string reapply_device_serial;
        if(color) {
            std::lock_guard<std::mutex> lock(camera.mutex);
            if(camera.gemini305_manual_exposure_reapply_pending
               && frame_now >= camera.gemini305_manual_exposure_reapply_at) {
                camera.gemini305_manual_exposure_reapply_pending = false;
                reapply_gemini305_manual_exposure = true;
                reapply_device_model = camera.device_model;
                reapply_device_serial = camera.device_serial;
            }
        }
        if(reapply_gemini305_manual_exposure) {
            bool verified =
                ensure_gemini305_manual_exposure(camera.config, reapply_device_model, reapply_device_serial, logger, true);
            if(verified) {
                try {
                    apply_color_controls(camera, logger);
                    logger.info("Gemini305 SDK color controls reapplied after manual exposure verification camera_id="
                                + camera.config.camera_id);
                }
                catch(const std::exception &e) {
                    verified = false;
                    logger.warn("Gemini305 SDK color control reapply failed camera_id=" + camera.config.camera_id
                                + " error=" + e.what());
                }
            }
            std::lock_guard<std::mutex> lock(camera.mutex);
            camera.publish_warmup_exposure_verified = verified;
            camera.gemini305_manual_exposure_reapply_pending = !verified;
            camera.gemini305_manual_exposure_reapply_at = verified ? std::chrono::steady_clock::time_point::min()
                                                                   : frame_now + std::chrono::seconds(1);
        }
        depth_output_camera = depth ? depth_target_camera(config, camera) : nullptr;
        bool warmup_drop = false;
        bool warmup_completed = false;
        bool warmup_forced = false;
        uint64_t warmup_dropped = 0;
        {
            std::lock_guard<std::mutex> lock(camera.mutex);
            if(camera.publish_warmup_active) {
                const bool ready = frame_now >= camera.publish_not_before
                                   && camera.publish_warmup_exposure_verified
                                   && !camera.gemini305_manual_exposure_reapply_pending;
                warmup_forced = !ready && frame_now >= camera.publish_warmup_deadline;
                if(ready || warmup_forced) {
                    camera.publish_warmup_active = false;
                    warmup_completed = true;
                    warmup_dropped = camera.publish_warmup_dropped_framesets;
                }
                else {
                    camera.publish_warmup_dropped_framesets++;
                    warmup_dropped = camera.publish_warmup_dropped_framesets;
                    warmup_drop = true;
                }
            }
        }
        if(warmup_drop) {
            if(color) {
                record_rgb_input(camera, color);
                update_color_metadata(camera, color);
            }
            if(depth) {
                record_depth_input(camera, depth);
                set_depth_scale_if_empty(camera, depth->getValueScale());
                if(depth_output_camera) {
                    set_depth_scale_if_empty(*depth_output_camera, depth->getValueScale());
                }
            }
            continue;
        }
        if(warmup_completed) {
            request_rgb_keyframe(camera, logger, "capture_warmup_complete");
            const std::string message = "capture publish warmup completed dropped_framesets=" + std::to_string(warmup_dropped)
                                        + " forced=" + (warmup_forced ? "true" : "false");
            if(warmup_forced) {
                logger.warn(message + " camera_id=" + camera.config.camera_id);
            }
            else {
                logger.info(message + " camera_id=" + camera.config.camera_id);
            }
            send_status_locked(transport, logger, transport_mutex,
                               event_message(config, warmup_forced ? "warning" : "info", "capture_warmup_complete", message,
                                             camera.config.camera_id));
        }
        uint64_t frameset_pair_id = 0;
        if(color && depth && depth_output_camera == &camera) {
            std::lock_guard<std::mutex> lock(camera.mutex);
            frameset_pair_id = camera.next_pair_id++;
            if(camera.next_pair_id == 0) {
                camera.next_pair_id = 1;
            }
        }
        bool preview_due = false;
        {
            std::lock_guard<std::mutex> lock(camera.mutex);
            preview_due = config.preview.enabled && frame_now >= camera.next_preview;
        }
        const bool web_preview_due = web_rgb_preview_emit_due(config, camera, frame_now);

        cv::Mat bgr;
        if(color) {
            record_rgb_input(camera, color);
            update_color_metadata(camera, color);
            const uint64_t rgb_device_timestamp_us = color->timeStampUs();
            const uint64_t rgb_system_timestamp_us = normalize_capture_system_timestamp(
                camera, StreamType::rgb, frame_system_timestamp_us_or(color, frame_host_now_us), frame_host_now_us);
            const bool color_is_mjpg = color->format() == OB_FORMAT_MJPG;
            RgbEncodeTiming rgb_capture_timing{color->index(), rgb_device_timestamp_us, rgb_system_timestamp_us, frameset_pair_id,
                                               static_cast<uint32_t>(color->width()), static_cast<uint32_t>(color->height()),
                                               rgb_frame_diagnostics(camera, color)};
            rgb_capture_timing.capture_host_timestamp_us = frame_host_now_us;
            rgb_capture_timing.timing_bound_timestamp_us = now_us();
            bool rgb_usable = true;
            if(color_is_mjpg && !mjpg_has_complete_jpeg(color)) {
                rgb_usable = false;
                mark_corrupt_rgb_jpeg_frame(camera, logger, color, frame_now, "missing jpeg soi/eoi marker");
            }
            else if(color_is_mjpg && (preview_due || camera.config.validate_rgb_mjpeg)) {
                std::string jpeg_validation_message;
                if(!mjpg_decodes_cleanly(color, jpeg_validation_message)) {
                    rgb_usable = false;
                    mark_corrupt_rgb_jpeg_frame(camera, logger, color, frame_now, jpeg_validation_message);
                }
            }

            if(rgb_usable) {
                maybe_update_adaptive_exposure(camera, color, frame_now, logger);
                if(color_is_mjpg) {
                    publish_rgb_snapshot_frame(config, camera, static_cast<const uint8_t *>(color->data()), color->dataSize(),
                                               rgb_capture_timing, logger);
                }
                else {
                    reject_next_rgb_snapshot_request(config, camera,
                                                     "configured RGB profile does not provide original MJPEG frames", logger);
                }
                if(color_is_mjpg && preview_due) {
                    const auto decode_started = std::chrono::steady_clock::now();
                    auto preview_bgr = color_to_preview_bgr(color);
                    record_rgb_decode_ms(camera, elapsed_ms(decode_started, std::chrono::steady_clock::now()));
                    set_latest_bgr(camera, preview_bgr, color->index(), rgb_system_timestamp_us,
                                   software_rgb_rotation_degrees(camera.config));
                }
                else if(!color_is_mjpg) {
                    const auto decode_started = std::chrono::steady_clock::now();
                    bgr = color_to_bgr(color);
                    record_rgb_decode_ms(camera, elapsed_ms(decode_started, std::chrono::steady_clock::now()));
                    if(preview_due) {
                        set_latest_bgr(camera, bgr, color->index(), rgb_system_timestamp_us,
                                       software_rgb_rotation_degrees(camera.config));
                    }
                    apply_software_rgb_rotation(bgr, camera.config);
                }

                if(!camera.encoder && !camera.jpeg_dual_encoder && (color_is_mjpg || !bgr.empty())) {
                    auto input_format = color_is_mjpg && software_rgb_rotation_degrees(camera.config) == 0
                                            ? GstH264InputFormat::Jpeg
                                            : GstH264InputFormat::Bgr;
                    const auto color_profile = camera_color_profile(camera);
                    if(!color_profile) {
                        set_camera_last_error(camera, "missing rgb stream profile");
                    }
                    else {
                        if(color_is_mjpg && software_rgb_rotation_degrees(camera.config) == 0 && config.web_rgb_preview.enabled
                           && jpeg_dual_encoder_experiment_enabled()
                           && !camera.jpeg_dual_encoder_disabled) {
                            const auto shape = resolve_web_rgb_preview_shape(config.web_rgb_preview, color->width(), color->height());
                            if(shape.width > 0 && shape.height > 0) {
                                std::lock_guard<std::mutex> encoder_lock(g_encoder_create_mutex);
                                camera.jpeg_dual_encoder = std::make_unique<GstJpegDualH264Encoder>(
                                    color->width(), color->height(), color_profile->fps(), camera.config.rgb_encoding.bitrate_bps,
                                    camera.config.rgb_encoding.gstreamer_encoder, shape.width, shape.height, config.web_rgb_preview.fps,
                                    config.web_rgb_preview.bitrate_bps);
                                if(camera.jpeg_dual_encoder->ok()) {
                                    std::lock_guard<std::mutex> lock(camera.mutex);
                                    camera.encoder_input_format = GstH264InputFormat::Jpeg;
                                    camera.web_preview_encoder_input_format = GstH264InputFormat::Jpeg;
                                    camera.hardware_encoder = true;
                                    camera.web_preview_width = static_cast<uint32_t>(camera.jpeg_dual_encoder->preview_output_width());
                                    camera.web_preview_height = static_cast<uint32_t>(camera.jpeg_dual_encoder->preview_output_height());
                                    camera.jpeg_dual_no_main_output = 0;
                                    logger.info("jpeg tee rgb encoder ready camera_id=" + camera.config.camera_id
                                                + " source=" + std::to_string(color->width()) + "x" + std::to_string(color->height())
                                                + " preview=" + std::to_string(shape.width) + "x" + std::to_string(shape.height)
                                                + " fps=" + std::to_string(color_profile->fps())
                                                + " preview_fps=" + std::to_string(config.web_rgb_preview.fps));
                                }
                                else {
                                    logger.warn("jpeg tee rgb encoder unavailable, falling back to separate encoders camera_id="
                                                + camera.config.camera_id + " error=" + camera.jpeg_dual_encoder->error());
                                    camera.jpeg_dual_encoder.reset();
                                }
                            }
                        }
                    }
                    if(!camera.jpeg_dual_encoder && color_profile) {
                        {
                            std::lock_guard<std::mutex> encoder_lock(g_encoder_create_mutex);
                            camera.encoder = std::make_unique<GstH264Encoder>(color->width(), color->height(), color_profile->fps(),
                                                                                camera.config.rgb_encoding.bitrate_bps,
                                                                                camera.config.rgb_encoding.gstreamer_encoder, input_format);
                            if(!camera.encoder->ok() && color_is_mjpg) {
                                logger.warn("mppjpegdec rgb path unavailable, falling back to BGR encode path: " + camera.encoder->error());
                                input_format = GstH264InputFormat::Bgr;
                                camera.encoder = std::make_unique<GstH264Encoder>(color->width(), color->height(), color_profile->fps(),
                                                                                    camera.config.rgb_encoding.bitrate_bps,
                                                                                    camera.config.rgb_encoding.gstreamer_encoder, input_format);
                            }
                        }
                        {
                            std::lock_guard<std::mutex> lock(camera.mutex);
                            camera.encoder_input_format = input_format;
                            camera.hardware_encoder = camera.encoder->ok();
                            if(!camera.hardware_encoder) {
                                camera.last_error = camera.encoder->error();
                            }
                        }
                        if(!camera.encoder->ok()) {
                            const auto error = camera.encoder->error();
                            logger.error("encoder_init_failed: " + error);
                            send_status_locked(transport, logger, transport_mutex,
                                               event_message(config, "error", "encoder_init_failed", error, camera.config.camera_id));
                        }
                    }
                }

                if(web_preview_due && !camera.web_preview_encoder && !camera.jpeg_dual_encoder && (color_is_mjpg || !bgr.empty())) {
                    auto input_format = color_is_mjpg && software_rgb_rotation_degrees(camera.config) == 0
                                            ? GstH264InputFormat::Jpeg
                                            : GstH264InputFormat::Bgr;
                    const auto color_profile = camera_color_profile(camera);
                    if(color_profile) {
                        const auto shape = resolve_web_rgb_preview_shape(config.web_rgb_preview, color->width(), color->height());
                        if(shape.width > 0 && shape.height > 0) {
                            {
                                std::lock_guard<std::mutex> encoder_lock(g_encoder_create_mutex);
                                camera.web_preview_encoder = std::make_unique<GstH264Encoder>(
                                    color->width(), color->height(), config.web_rgb_preview.fps, config.web_rgb_preview.bitrate_bps,
                                    camera.config.rgb_encoding.gstreamer_encoder, input_format, shape.width, shape.height);
                                if(!camera.web_preview_encoder->ok() && color_is_mjpg) {
                                    logger.warn("mppjpegdec web rgb preview path unavailable, falling back to BGR encode path: "
                                                + camera.web_preview_encoder->error());
                                    input_format = GstH264InputFormat::Bgr;
                                    camera.web_preview_encoder = std::make_unique<GstH264Encoder>(
                                        color->width(), color->height(), config.web_rgb_preview.fps, config.web_rgb_preview.bitrate_bps,
                                        camera.config.rgb_encoding.gstreamer_encoder, input_format, shape.width, shape.height);
                                }
                            }
                            {
                                std::lock_guard<std::mutex> lock(camera.mutex);
                                camera.web_preview_encoder_input_format = input_format;
                                if(camera.web_preview_encoder->ok()) {
                                    camera.web_preview_width = static_cast<uint32_t>(camera.web_preview_encoder->output_width());
                                    camera.web_preview_height = static_cast<uint32_t>(camera.web_preview_encoder->output_height());
                                }
                                else {
                                    camera.web_preview_width = 0;
                                    camera.web_preview_height = 0;
                                    camera.last_error = camera.web_preview_encoder->error();
                                }
                            }
                            if(camera.web_preview_encoder->ok()) {
                                logger.info("web rgb preview encoder ready camera_id=" + camera.config.camera_id
                                            + " source=" + std::to_string(color->width()) + "x" + std::to_string(color->height())
                                            + " preview=" + std::to_string(shape.width) + "x" + std::to_string(shape.height)
                                            + " fps=" + std::to_string(config.web_rgb_preview.fps)
                                            + " bitrate_bps=" + std::to_string(config.web_rgb_preview.bitrate_bps));
                            }
                            else {
                                logger.warn("web rgb preview encoder init failed camera_id=" + camera.config.camera_id
                                            + " error=" + camera.web_preview_encoder->error());
                            }
                        }
                    }
                }

                const bool web_preview_can_queue = web_preview_due;
                if((camera.jpeg_dual_encoder && camera.jpeg_dual_encoder->ok()) || (camera.encoder && camera.encoder->ok())) {
                    try {
                        if(camera.jpeg_dual_encoder && camera.jpeg_dual_encoder->ok()) {
                            uint64_t force_keyframe_request_id = 0;
                            if(consume_rgb_keyframe_request(camera, rgb_system_timestamp_us,
                                                            force_keyframe_request_id)) {
                                camera.jpeg_dual_encoder->request_keyframe();
                                logger.info("rgb keyframe force event queued camera_id=" + camera.config.camera_id
                                            + " request_id=" + std::to_string(force_keyframe_request_id));
                            }
                            RgbEncodeTiming submitted_timing = rgb_capture_timing;
                            submitted_timing.encode_start_timestamp_us = now_us();
                            remember_rgb_encode_timing(camera, submitted_timing);
                            RgbEncodeTiming preview_timing = submitted_timing;
                            const auto encode_started = std::chrono::steady_clock::now();
                            auto encoded_units = camera.jpeg_dual_encoder->encode_jpeg(color->data(), color->dataSize(),
                                                                                       rgb_system_timestamp_us, web_preview_can_queue);
                            const uint64_t encode_done_timestamp_us = now_us();
                            submitted_timing.encode_done_timestamp_us = encode_done_timestamp_us;
                            preview_timing.encode_done_timestamp_us = encode_done_timestamp_us;
                            record_rgb_encode_ms(camera, elapsed_ms(encode_started, std::chrono::steady_clock::now()));
                            if(encoded_units.main.empty()) {
                                bool disable_dual_encoder = false;
                                {
                                    std::lock_guard<std::mutex> lock(camera.mutex);
                                    disable_dual_encoder = ++camera.jpeg_dual_no_main_output >= kJpegDualNoMainOutputFallbackFrames;
                                    if(disable_dual_encoder) {
                                        camera.jpeg_dual_encoder_disabled = true;
                                        camera.last_error = "jpeg tee encoder produced no main rgb output; falling back";
                                        camera.web_preview_width = 0;
                                        camera.web_preview_height = 0;
                                        camera.rgb_encode_timings.clear();
                                    }
                                }
                                if(disable_dual_encoder) {
                                    logger.warn("jpeg tee rgb encoder disabled camera_id=" + camera.config.camera_id
                                                + " reason=no_main_output fallback=separate_rgb_and_preview_encoders");
                                    camera.jpeg_dual_encoder.reset();
                                }
                            }
                            else {
                                std::lock_guard<std::mutex> lock(camera.mutex);
                                camera.jpeg_dual_no_main_output = 0;
                            }
                            publish_rgb_encoded_units(config, camera, rgb_media_queue, rgb_slot, encoded_units.main, submitted_timing,
                                                      encode_done_timestamp_us, frame_now, logger);
                            if(camera.jpeg_dual_encoder && web_preview_can_queue && !encoded_units.preview.empty()) {
                                publish_rgb_preview_units(config, camera, preview_media_queue, rgb_preview_slot, encoded_units.preview, preview_timing,
                                                          static_cast<uint32_t>(camera.jpeg_dual_encoder->preview_output_width()),
                                                          static_cast<uint32_t>(camera.jpeg_dual_encoder->preview_output_height()), logger);
                            }
                        }
                        else {
                            const auto input_format = encoder_input_format_for(camera);
                            if(input_format == GstH264InputFormat::Bgr && bgr.empty()) {
                                const auto decode_started = std::chrono::steady_clock::now();
                                bgr = color_to_bgr(color);
                                apply_software_rgb_rotation(bgr, camera.config);
                                record_rgb_decode_ms(camera, elapsed_ms(decode_started, std::chrono::steady_clock::now()));
                            }
                            if(input_format == GstH264InputFormat::Bgr && bgr.empty()) {
                                set_camera_last_error(camera, "rgb decode produced empty frame");
                                record_queue_overwrite(camera, StreamType::rgb);
                            }
                            else {
                                uint64_t force_keyframe_request_id = 0;
                                if(consume_rgb_keyframe_request(camera, rgb_system_timestamp_us,
                                                                force_keyframe_request_id)) {
                                    camera.encoder->request_keyframe();
                                    logger.info("rgb keyframe force event queued camera_id=" + camera.config.camera_id
                                                + " request_id=" + std::to_string(force_keyframe_request_id));
                                }
                                RgbEncodeTiming submitted_timing = rgb_capture_timing;
                                submitted_timing.encode_start_timestamp_us = now_us();
                                remember_rgb_encode_timing(camera, submitted_timing);
                                const auto encode_started = std::chrono::steady_clock::now();
                                auto encoded_units = input_format == GstH264InputFormat::Jpeg
                                                         ? camera.encoder->encode_jpeg(color->data(), color->dataSize(), rgb_system_timestamp_us)
                                                         : camera.encoder->encode_bgr(bgr, rgb_system_timestamp_us);
                                const uint64_t encode_done_timestamp_us = now_us();
                                submitted_timing.encode_done_timestamp_us = encode_done_timestamp_us;
                                record_rgb_encode_ms(camera, elapsed_ms(encode_started, std::chrono::steady_clock::now()));
                                publish_rgb_encoded_units(config, camera, rgb_media_queue, rgb_slot, encoded_units, submitted_timing,
                                                          encode_done_timestamp_us, frame_now, logger);
                                if(web_preview_can_queue && camera.web_preview_encoder && camera.web_preview_encoder->ok()) {
                                    const auto preview_input_format = web_preview_encoder_input_format_for(camera);
                                    if(preview_input_format == GstH264InputFormat::Bgr && bgr.empty()) {
                                        const auto decode_started = std::chrono::steady_clock::now();
                                        bgr = color_to_bgr(color);
                                        apply_software_rgb_rotation(bgr, camera.config);
                                        record_rgb_decode_ms(camera, elapsed_ms(decode_started, std::chrono::steady_clock::now()));
                                    }
                                    if(preview_input_format == GstH264InputFormat::Bgr && bgr.empty()) {
                                        set_camera_last_error(camera, "rgb preview decode produced empty frame");
                                    }
                                    else {
                                        RgbEncodeTiming preview_timing = rgb_capture_timing;
                                        preview_timing.encode_start_timestamp_us = now_us();
                                        auto preview_units = preview_input_format == GstH264InputFormat::Jpeg
                                                                 ? camera.web_preview_encoder->encode_jpeg(color->data(), color->dataSize(),
                                                                                                           rgb_system_timestamp_us)
                                                                 : camera.web_preview_encoder->encode_bgr(bgr, rgb_system_timestamp_us);
                                        preview_timing.encode_done_timestamp_us = now_us();
                                        publish_rgb_preview_units(config, camera, preview_media_queue, rgb_preview_slot, preview_units, preview_timing,
                                                                  static_cast<uint32_t>(camera.web_preview_encoder->output_width()),
                                                                  static_cast<uint32_t>(camera.web_preview_encoder->output_height()), logger);
                                    }
                                }
                            }
                        }
                    }
                    catch(const std::exception &e) {
                        set_camera_last_error(camera, e.what());
                        logger.error("rgb encode failed camera_id=" + camera.config.camera_id + " error=" + e.what());
                    }
                }
            }
            record_rgb_frame_done(camera);
        }

        if(depth) {
            auto *depth_target = depth_output_camera;
            record_depth_input(camera, depth);
            set_depth_scale_if_empty(camera, depth->getValueScale());
            set_depth_scale_if_empty(*depth_target, depth->getValueScale());
            const bool publish_depth = depth_emit_due(camera, frame_now);
            if(publish_depth) {
                const void *depth_payload = depth->data();
                size_t depth_payload_size = depth->dataSize();
                const uint64_t depth_device_timestamp_us = depth->timeStampUs();
                const uint64_t depth_system_timestamp_us = normalize_capture_system_timestamp(
                    camera, StreamType::depth_raw, frame_system_timestamp_us_or(depth, frame_host_now_us), frame_host_now_us);
                MediaFrameMeta meta;
                meta.stream_type = StreamType::depth_raw;
                meta.flags = has_system_timestamp;
                meta.sender_id = config.sender_id;
                meta.camera_id = depth_target->config.camera_id;
                meta.codec_or_compression = camera.config.depth_transport.compression;
                meta.frame_id = depth->index();
                meta.timestamp_us = depth_device_timestamp_us;
                meta.system_timestamp_us = depth_system_timestamp_us;
                meta.pair_id = frameset_pair_id;
                meta.width = depth->width();
                meta.height = depth->height();
                meta.pixel_format = PixelFormat::depth_u16;
                meta.payload_size = depth_payload_size;
                meta.uncompressed_size = depth->dataSize();
                if(depth_transport_uses_compression(camera.config.depth_transport.compression)) {
                    DepthCompressionJob compress_job;
                    compress_job.source_camera = &camera;
                    compress_job.output_camera = depth_target;
                    compress_job.output_media_queue = &depth_media_queue;
                    compress_job.media_slot_index = depth_slot;
                    compress_job.meta = meta;
                    {
                        std::lock_guard<std::mutex> lock(camera.mutex);
                        compress_job.depth_scale = camera.depth_scale > 0.0f ? camera.depth_scale : 1.0f;
                    }
                    compress_job.quantization_step_mm = camera.config.depth_transport.quantization_step_mm;
                    const auto *depth_bytes = static_cast<const uint8_t *>(depth->data());
                    compress_job.raw_payload = depth_bytes;
                    compress_job.raw_payload_size = depth->dataSize();
                    compress_job.raw_payload_owner = std::shared_ptr<const void>(depth, static_cast<const void *>(depth->data()));
                    if(depth_compression_queue.publish(depth_compression_slot, std::move(compress_job))) {
                        record_queue_overwrite(*depth_target, StreamType::depth_raw);
                    }
                }
                else {
                    MediaPacketJob job;
                    auto owner = std::shared_ptr<const void>(depth, static_cast<const void *>(depth->data()));
                    job = make_external_media_job(*depth_target, StreamType::depth_raw, meta, depth_payload, depth_payload_size, std::move(owner));
                    publish_media_job(depth_media_queue, depth_slot, *depth_target, StreamType::depth_raw, std::move(job), logger);
                    record_depth_frame_done(*depth_target);
                }

                if(preview_due) {
                    const auto depth_preview_started = std::chrono::steady_clock::now();
                    auto depth_color = depth_to_color(depth);
                    record_depth_preview_ms(camera, elapsed_ms(depth_preview_started, std::chrono::steady_clock::now()));
                    set_latest_depth_color(*depth_target, depth_color);
                }
            }
        }

        if(auto reason = capture_stream_stall_reason(camera, frame_now)) {
            mark_camera_disconnected(config, camera, transport, logger, transport_mutex, *reason);
            continue;
        }

        if(!camera_announced(camera) && color && (!camera.config.depth_profile.enabled || depth)) {
            send_status_locked(transport, logger, transport_mutex, camera_announce(config, camera));
            set_camera_announced(camera, true);
        }
        const std::string depth_output_camera_id = depth_output_camera ? depth_output_camera->config.camera_id : camera.config.camera_id;
        log_time_sync(camera, logger, color, depth, depth_output_camera_id, frame_now);
        if(preview_due) {
            std::lock_guard<std::mutex> lock(camera.mutex);
            camera.next_preview = frame_now + preview_interval;
        }
    }
}

template <typename Sender>
void camera_worker_thread_entry(const AppConfig &config, CameraRuntime &camera, size_t primary_slot_base, size_t preview_slot_base,
                                size_t compression_slot_base, LatestMediaQueue &rgb_media_queue, LatestMediaQueue &depth_media_queue,
                                LatestMediaQueue &preview_media_queue, LatestDepthCompressionQueue &depth_compression_queue,
                                Sender &transport, Logger &logger, std::mutex &transport_mutex,
                                std::chrono::milliseconds preview_interval) {
    while(g_running) {
        try {
            camera_worker_loop(config, camera, primary_slot_base, preview_slot_base, compression_slot_base,
                               rgb_media_queue, depth_media_queue, preview_media_queue, depth_compression_queue,
                               transport, logger, transport_mutex, preview_interval);
            return;
        }
        catch(const ob::Error &e) {
            try {
                mark_camera_disconnected(config, camera, transport, logger, transport_mutex, ob_error_text(e));
            }
            catch(const std::exception &cleanup_error) {
                logger.error("camera worker cleanup failed camera_id=" + camera.config.camera_id + " error=" + cleanup_error.what());
                return;
            }
        }
        catch(const std::exception &e) {
            try {
                mark_camera_disconnected(config, camera, transport, logger, transport_mutex, e.what());
            }
            catch(const std::exception &cleanup_error) {
                logger.error("camera worker cleanup failed camera_id=" + camera.config.camera_id + " error=" + cleanup_error.what());
                return;
            }
        }
        catch(...) {
            try {
                mark_camera_disconnected(config, camera, transport, logger, transport_mutex, "unknown camera worker exception");
            }
            catch(...) {
                return;
            }
        }
    }
}

