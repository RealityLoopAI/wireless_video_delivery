Json::Value sender_hello(const AppConfig &config) {
    Json::Value msg = base_message(config, "sender_hello");
    msg["sender_version"] = config.sender_version;
    msg["host_name"] = hostname();
    msg["os"] = "linux";
    msg["arch"] = architecture();
    Json::Value capabilities;
    Json::Value rgb(Json::arrayValue);
    rgb.append("h264");
    capabilities["rgb_encoding"] = rgb;
    Json::Value depth(Json::arrayValue);
    depth.append("none");
    depth.append("zlib");
    depth.append("qdelta");
    depth.append("pq12zlib");
    depth.append("q8lz4");
    depth.append("pq8zlib");
    depth.append("pq8lz4");
    capabilities["depth_compression"] = depth;
    capabilities["local_preview"] = config.preview.enabled;
    capabilities["web_rgb_preview"] = config.web_rgb_preview.enabled;
    capabilities["web_rgb_preview_on_demand"] = config.web_rgb_preview.on_demand;
    capabilities["hotplug"] = config.hotplug.enabled;
    capabilities["clock_sync"] = config.clock_sync.enabled;
    capabilities["clock_sync_port"] = config.clock_sync.port;
    capabilities["receiver_discovery"] = config.receiver_discovery.enabled;
    capabilities["receiver_discovery_port"] = config.receiver_discovery.port;
    capabilities["media_protocol"] = config.transport.media_protocol;
    capabilities["status_protocol"] = config.transport.status_protocol;
    msg["capabilities"] = capabilities;
    return msg;
}

Json::Value camera_announce(const AppConfig &config, CameraRuntime &camera) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    Json::Value msg = base_message(config, "camera_announce");
    msg["camera_id"] = camera.config.camera_id;
    Json::Value device;
    device["vendor"] = "orbbec";
    device["model"] = camera.config.capture_backend == "v4l2" ? "UVC RGB" : "";
    device["serial_number"] = camera.device_serial;
    device["uid"] = camera.device_uid;
    device["paired_rgb_serial"] = camera.config.capture_backend == "v4l2" ? camera.device_serial : paired_rgb_serial_for_depth_uid(camera.device_uid);
    device["configured_model"] = camera.config.device_model;
    device["configured_serial_number"] = camera.config.serial_number;
    device["configured_uid"] = camera.config.uid;
    device["firmware_version"] = "";
    device["connection_type"] = camera.device_connection_type;
    if(camera.device) {
        auto info = camera.device->getDeviceInfo();
        device["model"] = info->name() ? info->name() : "";
        device["serial_number"] = info->serialNumber() ? info->serialNumber() : "";
        device["uid"] = info->uid() ? info->uid() : "";
        device["paired_rgb_serial"] = paired_rgb_serial_for_depth_uid(device["uid"].asString());
        device["firmware_version"] = info->firmwareVersion() ? info->firmwareVersion() : "";
        device["connection_type"] = info->connectionType() ? info->connectionType() : "";
    }
    msg["device"] = device;
    msg["rgb_profile"] = camera.color_profile ? profile_json(camera.color_profile, "encoded_video", camera.config.rgb_encoding.codec)
                                               : profile_json(camera.config.rgb_profile, "encoded_video", camera.config.rgb_encoding.codec);
    msg["depth_profile"] = camera.depth_profile ? profile_json(camera.depth_profile, "uint16", camera.config.depth_transport.compression, camera.depth_scale)
                                                 : profile_json(camera.config.depth_profile, "uint16", camera.config.depth_transport.compression,
                                                                camera.depth_scale);
    if(camera.config.rotation_degrees) {
        msg["rotation_degrees"] = *camera.config.rotation_degrees;
    }
    Json::Value web_preview;
    web_preview["enabled"] = config.web_rgb_preview.enabled;
    web_preview["on_demand"] = config.web_rgb_preview.on_demand;
    web_preview["max_width"] = config.web_rgb_preview.max_width;
    web_preview["max_height"] = config.web_rgb_preview.max_height;
    web_preview["fps"] = config.web_rgb_preview.fps;
    web_preview["bitrate_bps"] = config.web_rgb_preview.bitrate_bps;
    web_preview["width"] = Json::UInt(camera.web_preview_width);
    web_preview["height"] = Json::UInt(camera.web_preview_height);
    msg["web_rgb_preview"] = web_preview;
    if(camera.pipeline && camera.device) {
        msg["calibration"] = calibration_json(*camera.pipeline, camera.pipeline_config, camera.device, camera.color_profile,
                                                camera.depth_profile);
    }
    else {
        Json::Value calibration;
        calibration["available"] = false;
        calibration["reason"] = "not_available_for_v4l2_rgb_only";
        msg["calibration"] = calibration;
    }
    return msg;
}

Json::Value event_message(const AppConfig &config, const std::string &level, const std::string &code, const std::string &message,
                          const std::string &camera_id = "") {
    Json::Value msg = base_message(config, "event");
    if(!camera_id.empty()) {
        msg["camera_id"] = camera_id;
    }
    msg["level"] = level;
    msg["event_code"] = code;
    msg["message"] = message;
    return msg;
}

Json::Value camera_offline_message(const AppConfig &config, const std::string &camera_id, const std::string &reason) {
    Json::Value msg = base_message(config, "camera_offline");
    msg["camera_id"] = camera_id;
    msg["reason"] = reason.empty() ? "camera_offline" : reason;
    return msg;
}

Json::Value camera_heartbeat(const AppConfig &config,
                             CameraRuntime &camera,
                             std::chrono::steady_clock::time_point started,
                             const ClockSyncClient *clock_sync,
                             const ReceiverTarget *receiver_target = nullptr) {
    Json::Value msg = base_message(config, "heartbeat");
    const auto uptime = std::chrono::steady_clock::now() - started;
    msg["uptime_ms"] = Json::UInt64(std::chrono::duration_cast<std::chrono::milliseconds>(uptime).count());
    const auto clock_state = clock_sync ? clock_sync->state() : ClockSyncClientState{};
    const bool clock_valid = clock_sync && clock_sync->healthy() && clock_state.valid;
    msg["clock_sync_valid"] = clock_valid;
    msg["clock_offset_us"] = Json::Int64(clock_state.offset_us);
    msg["clock_delay_us"] = Json::Int64(clock_state.delay_us);
    msg["clock_drift_ppm"] = clock_state.drift_ppm;
    msg["clock_last_sync_us"] = Json::UInt64(clock_state.last_sync_us);
    msg["clock_samples"] = Json::UInt64(clock_state.sample_count);
    if(receiver_target != nullptr) {
        const auto target = receiver_target->snapshot();
        msg["receiver_target_host"] = target.host;
        msg["receiver_target_id"] = target.receiver_id;
        msg["receiver_target_discovered"] = target.discovered;
        msg["receiver_target_generation"] = Json::UInt64(target.generation);
    }
    std::lock_guard<std::mutex> lock(camera.mutex);
    const auto seconds = std::max(0.001, std::chrono::duration<double>(std::chrono::steady_clock::now() - camera.stats_started).count());
    msg["camera_id"] = camera.config.camera_id;
    msg["online"] = camera.online;
    msg["rgb_fps"] = static_cast<double>(camera.rgb_frames) / seconds;
    msg["depth_fps"] = static_cast<double>(camera.depth_frames) / seconds;
    msg["rgb_encoding"] = camera.config.rgb_encoding.codec;
    msg["depth_compression"] = camera.config.depth_transport.compression;
    msg["rgb_dropped_frames"] = Json::UInt64(camera.rgb_dropped);
    msg["rgb_transport_retry_drops"] = Json::UInt64(camera.rgb_transport_retry_drops);
    msg["rgb_send_failures_total"] = Json::UInt64(camera.rgb_send_failures_total);
    msg["rgb_preview_send_failures_total"] = Json::UInt64(camera.rgb_preview_send_failures_total);
    msg["depth_send_failures_total"] = Json::UInt64(camera.depth_send_failures_total);
    msg["rgb_corrupt_jpeg_frames"] = Json::UInt64(camera.rgb_corrupt_jpeg);
    msg["depth_dropped_frames"] = Json::UInt64(camera.depth_dropped);
    msg["hardware_encoder"] = camera.hardware_encoder;
    msg["rgb_measured_fps"] = camera.live.rgb_input_fps;
    msg["depth_measured_fps"] = camera.live.depth_input_fps;
    msg["rgb_sent_fps"] = camera.live.rgb_sent_fps;
    msg["depth_sent_fps"] = camera.live.depth_sent_fps;
    msg["rgb_mbps"] = camera.live.rgb_mbps;
    msg["rgb_preview_mbps"] = camera.live.rgb_preview_mbps;
    msg["rgb_preview_fps"] = camera.live.rgb_preview_sent_fps;
    msg["rgb_preview_width"] = Json::UInt(camera.web_preview_width);
    msg["rgb_preview_height"] = Json::UInt(camera.web_preview_height);
    msg["depth_mbps"] = camera.live.depth_mbps;
    msg["rgb_auto_exposure"] = Json::Int64(camera.live.color_auto_exposure);
    msg["rgb_exposure"] = Json::Int64(camera.live.color_exposure);
    msg["rgb_gain"] = Json::Int64(camera.live.color_gain);
    msg["rgb_metadata_actual_fps"] = Json::Int64(camera.live.color_actual_fps);
    msg["rgb_exposure_priority"] = Json::Int64(camera.live.color_exposure_priority);
    msg["adaptive_exposure_enabled"] = camera.live.adaptive_exposure_enabled;
    msg["adaptive_exposure_luma_p50"] = camera.live.adaptive_luma_p50;
    msg["adaptive_exposure_luma_p95"] = camera.live.adaptive_luma_p95;
    msg["adaptive_exposure_luma_p99"] = camera.live.adaptive_luma_p99;
    msg["adaptive_exposure_highlight_fraction"] = camera.live.adaptive_highlight_fraction;
    msg["adaptive_exposure_requested_exposure"] = camera.live.adaptive_requested_exposure;
    msg["adaptive_exposure_requested_gain"] = camera.live.adaptive_requested_gain;
    msg["adaptive_exposure_samples"] = Json::UInt64(camera.live.adaptive_samples);
    msg["adaptive_exposure_adjustments"] = Json::UInt64(camera.live.adaptive_adjustments);
    msg["adaptive_exposure_failures"] = Json::UInt64(camera.live.adaptive_failures);
    msg["adaptive_exposure_last_reason"] = camera.live.adaptive_last_reason;
    msg["publish_warmup_active"] = camera.publish_warmup_active;
    msg["publish_warmup_dropped_framesets"] = Json::UInt64(camera.publish_warmup_dropped_framesets);
    msg["last_error"] = camera.last_error;
    return msg;
}

template <typename Sender>
bool send_status(Sender &sender, Logger &logger, const Json::Value &message) {
    const auto payload = json_to_string(message);
    if(!sender.send_status(payload)) {
        logger.warn("status send failed: " + sender.last_error());
        return false;
    }
    return true;
}

void preview_frame(const CameraRuntime &camera, bool preview_enabled) {
    cv::Mat bgr;
    cv::Mat depth_color;
    std::string label;
    {
        std::lock_guard<std::mutex> lock(camera.mutex);
        bgr = camera.latest_bgr.clone();
        depth_color = camera.latest_depth_color.clone();
        label = camera.config.camera_id;
        if(!camera.device_uid.empty()) {
            label += " uid=" + camera.device_uid;
        }
        if(!camera.device_serial.empty()) {
            label += " serial=" + camera.device_serial;
        }
    }
    if(!preview_enabled || bgr.empty() || depth_color.empty()) {
        return;
    }
    const std::string window_name = "Gemini Sender " + camera.config.camera_id;
    static std::map<std::string, int> initialized_windows;
    if(initialized_windows.find(window_name) == initialized_windows.end()) {
        const int window_index = static_cast<int>(initialized_windows.size());
        initialized_windows[window_name] = window_index;
        cv::namedWindow(window_name, cv::WINDOW_NORMAL);
        cv::resizeWindow(window_name, 960, 360);
        cv::moveWindow(window_name, 80 + (window_index % 2) * 520, 80 + (window_index / 2) * 420);
    }
    cv::Mat rgb_small;
    cv::Mat depth_small;
    cv::resize(bgr, rgb_small, cv::Size(480, 360));
    cv::resize(depth_color, depth_small, cv::Size(480, 360));
    cv::Mat wall;
    cv::hconcat(rgb_small, depth_small, wall);
    cv::putText(wall, "RGB " + label, cv::Point(12, 28), cv::FONT_HERSHEY_SIMPLEX, 0.55, cv::Scalar(255, 255, 255), 2,
                cv::LINE_AA);
    cv::putText(wall, "DEPTH " + label, cv::Point(492, 28), cv::FONT_HERSHEY_SIMPLEX, 0.55, cv::Scalar(255, 255, 255), 2,
                cv::LINE_AA);
    cv::imshow(window_name, wall);
    cv::waitKey(1);
}

struct AlignedRgbPreviewPair {
    RgbPreviewFrame cam01;
    RgbPreviewFrame cam02;
    std::string cam01_label;
    std::string cam02_label;
    int64_t cam01_minus_cam02_us = 0;
};

std::string camera_preview_label_locked(const CameraRuntime &camera) {
    std::string label = camera.config.camera_id;
    if(!camera.device_uid.empty()) {
        label += " uid=" + camera.device_uid;
    }
    if(!camera.device_serial.empty()) {
        label += " serial=" + camera.device_serial;
    }
    return label;
}

uint64_t abs_diff_us(uint64_t lhs, uint64_t rhs) {
    return lhs >= rhs ? lhs - rhs : rhs - lhs;
}

struct RgbEncodeTimingResolution {
    RgbEncodeTiming timing;
    bool used_fifo_fallback = false;
    bool non_vcl = false;
    bool queue_empty = false;
    uint64_t pts_delta_us = 0;
};

void remember_rgb_encode_timing(CameraRuntime &camera, const RgbEncodeTiming &timing) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    camera.rgb_encode_timings.push_back(timing);
    while(camera.rgb_encode_timings.size() > kMaxRgbEncodeTimingFrames) {
        camera.rgb_encode_timings.pop_front();
    }
}

RgbEncodeTimingResolution resolve_rgb_encode_timing(CameraRuntime &camera, const EncodedH264Frame &encoded,
                                                    const RgbEncodeTiming &fallback, bool encoded_has_vcl) {
    if(!encoded_has_vcl) {
        RgbEncodeTimingResolution resolution;
        resolution.timing = fallback;
        resolution.non_vcl = true;
        return resolution;
    }

    std::lock_guard<std::mutex> lock(camera.mutex);
    if(camera.rgb_encode_timings.empty()) {
        RgbEncodeTimingResolution resolution;
        resolution.timing = fallback;
        resolution.queue_empty = true;
        return resolution;
    }

    RgbEncodeTimingResolution resolution;
    if(encoded.has_pts) {
        auto best = camera.rgb_encode_timings.end();
        uint64_t best_delta_us = std::numeric_limits<uint64_t>::max();
        for(auto it = camera.rgb_encode_timings.begin(); it != camera.rgb_encode_timings.end(); ++it) {
            const uint64_t delta_us = abs_diff_us(it->system_timestamp_us, encoded.pts_us);
            if(delta_us < best_delta_us) {
                best_delta_us = delta_us;
                best = it;
                if(delta_us == 0) {
                    break;
                }
            }
        }
        if(best != camera.rgb_encode_timings.end() && best_delta_us <= kRgbPtsMatchToleranceUs) {
            resolution.timing = *best;
            resolution.pts_delta_us = best_delta_us;
            camera.rgb_encode_timings.erase(camera.rgb_encode_timings.begin(), std::next(best));
            return resolution;
        }
    }

    resolution.timing = camera.rgb_encode_timings.front();
    resolution.used_fifo_fallback = true;
    if(encoded.has_pts) {
        resolution.pts_delta_us = abs_diff_us(resolution.timing.system_timestamp_us, encoded.pts_us);
    }
    camera.rgb_encode_timings.pop_front();
    return resolution;
}

void maybe_log_rgb_timing_resolution(CameraRuntime &camera, Logger &logger, const EncodedH264Frame &,
                                     const RgbEncodeTimingResolution &resolution, std::chrono::steady_clock::time_point now) {
    if(!resolution.queue_empty) {
        return;
    }
    bool should_log = false;
    {
        std::lock_guard<std::mutex> lock(camera.mutex);
        if(now >= camera.next_rgb_timing_warning) {
            camera.next_rgb_timing_warning = now + std::chrono::seconds(2);
            should_log = true;
        }
    }
    if(!should_log) {
        return;
    }

    std::ostringstream oss;
    oss << "rgb encoder timing fallback camera_id=" << camera.config.camera_id;
    if(resolution.queue_empty) {
        oss << " reason=empty_timing_queue";
    }
    oss << " selected_frame_id=" << resolution.timing.frame_id
        << " selected_system_timestamp_us=" << resolution.timing.system_timestamp_us;
    logger.warn(oss.str());
}

bool rgb_encoded_timing_is_monotonic(CameraRuntime &camera, Logger &logger, const RgbEncodeTiming &timing,
                                     std::chrono::steady_clock::time_point now) {
    bool regression = false;
    bool should_log = false;
    uint64_t last_frame_id = 0;
    uint64_t last_system_timestamp_us = 0;
    {
        std::lock_guard<std::mutex> lock(camera.mutex);
        if(camera.rgb_sent_timing_seen) {
            const bool frame_id_regressed = timing.frame_id <= camera.rgb_last_sent_frame_id;
            const bool system_timestamp_regressed =
                timing.system_timestamp_us > 0 && camera.rgb_last_sent_system_timestamp_us > 0
                && timing.system_timestamp_us <= camera.rgb_last_sent_system_timestamp_us;
            regression = frame_id_regressed || system_timestamp_regressed;
            if(regression) {
                last_frame_id = camera.rgb_last_sent_frame_id;
                last_system_timestamp_us = camera.rgb_last_sent_system_timestamp_us;
                camera.perf.rgb_timing_mismatch_drops++;
                camera.rgb_timing_mismatch_drops++;
                camera.rgb_dropped++;
                camera.last_error = "rgb encoded timing regression dropped";
                if(now >= camera.next_rgb_timing_warning) {
                    camera.next_rgb_timing_warning = now + std::chrono::seconds(1);
                    should_log = true;
                }
            }
        }
    }
    if(!regression) {
        return true;
    }
    if(should_log) {
        std::ostringstream oss;
        oss << "rgb encoded timing regression dropped camera_id=" << camera.config.camera_id
            << " frame_id=" << timing.frame_id
            << " last_frame_id=" << last_frame_id
            << " system_timestamp_us=" << timing.system_timestamp_us
            << " last_system_timestamp_us=" << last_system_timestamp_us;
        logger.warn(oss.str());
    }
    arm_rgb_keyframe_guard(camera, logger, "rgb encoded timing regression; waiting for next keyframe");
    return false;
}

void mark_rgb_encoded_timing_queued(CameraRuntime &camera, const RgbEncodeTiming &timing) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    camera.rgb_sent_timing_seen = true;
    camera.rgb_last_sent_frame_id = timing.frame_id;
    camera.rgb_last_sent_system_timestamp_us = timing.system_timestamp_us;
}

bool rgb_encoder_output_lag_too_large(CameraRuntime &camera, Logger &logger, const RgbEncodeTiming &timing,
                                      uint64_t encode_done_timestamp_us, std::chrono::steady_clock::time_point now) {
    if(timing.system_timestamp_us == 0 || encode_done_timestamp_us <= timing.system_timestamp_us) {
        return false;
    }
    const uint64_t lag_us = encode_done_timestamp_us - timing.system_timestamp_us;
    if(lag_us <= kRgbEncoderOutputLagResetUs) {
        return false;
    }

    std::unique_ptr<GstH264Encoder> old_encoder;
    std::unique_ptr<GstH264Encoder> old_preview_encoder;
    std::unique_ptr<GstJpegDualH264Encoder> old_dual_encoder;
    bool should_log = false;
    {
        std::lock_guard<std::mutex> lock(camera.mutex);
        old_encoder = std::move(camera.encoder);
        old_preview_encoder = std::move(camera.web_preview_encoder);
        old_dual_encoder = std::move(camera.jpeg_dual_encoder);
        camera.jpeg_dual_encoder_disabled = true;
        camera.jpeg_dual_no_main_output = 0;
        camera.web_preview_width = 0;
        camera.web_preview_height = 0;
        camera.web_rgb_preview_requested_until = std::chrono::steady_clock::time_point::min();
        camera.web_rgb_preview_suppressed_until = now + kWebRgbPreviewSuppressAfterEncoderLag;
        camera.rgb_encode_timings.clear();
        camera.rgb_sent_timing_seen = false;
        camera.rgb_last_sent_frame_id = 0;
        camera.rgb_last_sent_system_timestamp_us = 0;
        camera.rgb_dropped++;
        camera.perf.rgb_encoder_lag_resets++;
        camera.last_error = "rgb encoder output lag reset";
        if(now >= camera.next_rgb_encoder_lag_warning) {
            camera.next_rgb_encoder_lag_warning = now + std::chrono::seconds(2);
            should_log = true;
        }
    }

    if(should_log) {
        std::ostringstream oss;
        oss << "rgb encoder output lag reset camera_id=" << camera.config.camera_id
            << " frame_id=" << timing.frame_id
            << " lag_us=" << lag_us
            << " threshold_us=" << kRgbEncoderOutputLagResetUs
            << " system_timestamp_us=" << timing.system_timestamp_us
            << " encode_done_timestamp_us=" << encode_done_timestamp_us
            << " web_preview_suppressed_ms="
            << std::chrono::duration_cast<std::chrono::milliseconds>(kWebRgbPreviewSuppressAfterEncoderLag).count();
        logger.warn(oss.str());
    }
    return true;
}

void set_rgb_pipeline_diagnostics(MediaFrameMeta &meta, const RgbEncodeTiming &timing) {
    meta.flags |= has_pipeline_diagnostics;
    meta.sender_capture_host_timestamp_us = timing.capture_host_timestamp_us;
    meta.sender_timing_bound_timestamp_us = timing.timing_bound_timestamp_us;
    meta.sender_encode_start_timestamp_us = timing.encode_start_timestamp_us;
    meta.sender_encode_done_timestamp_us = timing.encode_done_timestamp_us;
    meta.sender_packet_queued_timestamp_us = timing.packet_queued_timestamp_us;
}

std::optional<AlignedRgbPreviewPair> pop_aligned_rgb_preview_pair(CameraRuntime &cam01, CameraRuntime &cam02) {
    std::scoped_lock lock(cam01.mutex, cam02.mutex);
    while(!cam01.rgb_preview_queue.empty() && !cam02.rgb_preview_queue.empty()) {
        const auto &left = cam01.rgb_preview_queue.front();
        const auto &right = cam02.rgb_preview_queue.front();
        const auto delta_us = abs_diff_us(left.system_timestamp_us, right.system_timestamp_us);
        if(delta_us <= kAlignedRgbPreviewDisplayMaxDeltaUs) {
            AlignedRgbPreviewPair pair;
            pair.cam01 = left;
            pair.cam02 = right;
            pair.cam01_label = camera_preview_label_locked(cam01);
            pair.cam02_label = camera_preview_label_locked(cam02);
            pair.cam01_minus_cam02_us =
                left.system_timestamp_us >= right.system_timestamp_us
                    ? static_cast<int64_t>(left.system_timestamp_us - right.system_timestamp_us)
                    : -static_cast<int64_t>(right.system_timestamp_us - left.system_timestamp_us);
            cam01.rgb_preview_queue.pop_front();
            cam02.rgb_preview_queue.pop_front();
            return pair;
        }
        if(left.system_timestamp_us < right.system_timestamp_us) {
            cam01.rgb_preview_queue.pop_front();
        }
        else {
            cam02.rgb_preview_queue.pop_front();
        }
    }
    return std::nullopt;
}

void put_text_with_outline(cv::Mat &image, const std::string &text, const cv::Point &origin, double scale,
                           const cv::Scalar &color) {
    cv::putText(image, text, origin, cv::FONT_HERSHEY_SIMPLEX, scale, cv::Scalar(0, 0, 0), 4, cv::LINE_AA);
    cv::putText(image, text, origin, cv::FONT_HERSHEY_SIMPLEX, scale, color, 1, cv::LINE_AA);
}

void draw_rgb_preview_overlay(cv::Mat &panel, const std::string &label, const RgbPreviewFrame &frame, int64_t delta_us,
                              const std::string &delta_label) {
    const cv::Scalar text_color(255, 255, 255);
    const cv::Scalar delta_color(std::abs(delta_us) <= static_cast<int64_t>(kAlignedRgbPreviewTargetDeltaUs) ? cv::Scalar(80, 255, 120)
                                                                                                             : cv::Scalar(40, 220, 255));
    std::ostringstream frame_line;
    frame_line << label << " RGB frame=" << frame.frame_id;
    std::ostringstream ts_line;
    ts_line << "system_ts_us=" << frame.system_timestamp_us;
    std::ostringstream delta_line;
    delta_line << std::fixed << std::setprecision(2) << delta_label << "=" << static_cast<double>(delta_us) / 1000.0 << "ms";

    put_text_with_outline(panel, frame_line.str(), cv::Point(12, 28), 0.55, text_color);
    put_text_with_outline(panel, ts_line.str(), cv::Point(12, 54), 0.48, text_color);
    put_text_with_outline(panel, delta_line.str(), cv::Point(12, 80), 0.52, delta_color);
}

std::filesystem::path default_rgb_snapshot_request_dir() {
    const char *value = std::getenv("GEMINI_RGB_SNAPSHOT_REQUEST_DIR");
    return value && *value ? std::filesystem::path(value) : std::filesystem::path("/tmp/gemini_rgb_snapshot_requests");
}

std::filesystem::path default_rgb_snapshot_result_dir() {
    const char *value = std::getenv("GEMINI_RGB_SNAPSHOT_RESULT_DIR");
    return value && *value ? std::filesystem::path(value) : std::filesystem::path("/tmp/gemini_rgb_snapshot_results");
}

bool is_safe_rgb_snapshot_request_id(const std::string &value) {
    return !value.empty() && value.size() <= 96
           && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
                  return std::isalnum(ch) || ch == '_' || ch == '-' || ch == '.';
              });
}

std::filesystem::path safe_rgb_snapshot_result_path(const std::string &request_id, const std::string &requested_path) {
    const auto result_dir = default_rgb_snapshot_result_dir().lexically_normal();
    const auto fallback = result_dir / (request_id + ".json");
    if(requested_path.empty()) {
        return fallback;
    }
    const auto candidate = std::filesystem::path(requested_path).lexically_normal();
    if(candidate.filename() != request_id + ".json" || candidate.parent_path() != result_dir) {
        return fallback;
    }
    return candidate;
}

void write_rgb_snapshot_result(const AppConfig &config,
                               const std::string &camera_id,
                               const RgbSnapshotRequest &request,
                               bool ok,
                               const std::string &status,
                               const std::string &image_path,
                               const std::string &error,
                               Logger &logger) {
    if(request.result_path.empty()) {
        return;
    }

    Json::Value root(Json::objectValue);
    root["message_type"] = "rgb_snapshot_result";
    root["request_id"] = request.request_id;
    root["sender_id"] = config.sender_id;
    root["camera_id"] = camera_id;
    root["ok"] = ok;
    root["status"] = status;
    root["image_path"] = image_path;
    root["error"] = error;
    root["trigger"] = request.trigger;
    root["requested_at_unix_us"] = Json::UInt64(request.requested_at_unix_us);
    root["completed_at_unix_us"] = Json::UInt64(now_us());

    const std::filesystem::path result_path = request.result_path;
    std::error_code ec;
    std::filesystem::create_directories(result_path.parent_path(), ec);
    if(ec) {
        logger.warn("rgb snapshot result directory create failed path=" + result_path.parent_path().string()
                    + " error=" + ec.message());
        return;
    }

    const auto temporary = result_path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::out | std::ios::trunc);
        if(!output) {
            logger.warn("rgb snapshot result write failed path=" + temporary);
            return;
        }
        output << json_to_string(root) << '\n';
        output.flush();
        if(!output) {
            logger.warn("rgb snapshot result flush failed path=" + temporary);
            return;
        }
    }
    std::filesystem::rename(temporary, result_path, ec);
    if(ec) {
        logger.warn("rgb snapshot result rename failed path=" + result_path.string() + " error=" + ec.message());
        std::filesystem::remove(temporary, ec);
    }
}

void remove_file_quietly(const std::filesystem::path &path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

CameraRuntime *choose_rgb_snapshot_camera(const std::vector<std::unique_ptr<CameraRuntime>> &cameras,
                                          const std::string &camera_id) {
    if(!camera_id.empty() && camera_id != "*") {
        for(const auto &camera : cameras) {
            if(camera && camera->config.camera_id == camera_id) {
                return camera.get();
            }
        }
        return nullptr;
    }
    for(const auto &camera : cameras) {
        if(camera && camera->config.camera_id == "cam01") {
            return camera.get();
        }
    }
    return cameras.empty() ? nullptr : cameras.front().get();
}

void process_rgb_snapshot_request_file(const AppConfig &config,
                                       const std::vector<std::unique_ptr<CameraRuntime>> &cameras,
                                       Logger &logger,
                                       const std::filesystem::path &path) {
    const auto root = parse_json_object(read_text_file(path));
    if(!root) {
        logger.warn("rgb snapshot request ignored invalid_json path=" + path.string());
        remove_file_quietly(path);
        return;
    }
    const std::string message_type = json_string_or(*root, "message_type");
    if(!message_type.empty() && message_type != "rgb_snapshot_request" && message_type != "snapshot_request") {
        logger.warn("rgb snapshot request ignored unknown_type path=" + path.string() + " message_type=" + message_type);
        remove_file_quietly(path);
        return;
    }
    const std::string target_sender = json_string_or(*root, "sender_id");
    if(!target_sender.empty() && target_sender != "*" && target_sender != config.sender_id) {
        remove_file_quietly(path);
        return;
    }

    RgbSnapshotRequest request;
    request.request_id = json_string_or(*root, "request_id", path.stem().string());
    request.camera_id = json_string_or(*root, "camera_id", "cam01");
    request.trigger = json_string_or(*root, "trigger", "local_request");
    request.requested_at_unix_us = json_uint64_or(*root, "requested_at_unix_us", now_us());
    request.capture_not_before_unix_us = json_uint64_or(*root, "capture_not_before_unix_us", 0);
    if(!is_safe_rgb_snapshot_request_id(request.request_id)) {
        logger.warn("rgb snapshot request ignored invalid request_id path=" + path.string());
        remove_file_quietly(path);
        return;
    }
    request.result_path =
        safe_rgb_snapshot_result_path(request.request_id, json_string_or(*root, "result_path")).string();
    const uint64_t current_us = now_us();
    const uint64_t request_timeout_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(kRgbSnapshotRequestTimeout).count());
    if(request.requested_at_unix_us > current_us + request_timeout_us) {
        logger.warn("rgb snapshot request ignored because request time is too far in the future request_id="
                    + request.request_id);
        write_rgb_snapshot_result(config, request.camera_id, request, false, "error", "",
                                  "snapshot request time is too far in the future", logger);
        remove_file_quietly(path);
        return;
    }
    if(current_us > request.requested_at_unix_us
       && current_us - request.requested_at_unix_us > request_timeout_us) {
        logger.warn("rgb snapshot request expired before sender queue request_id=" + request.request_id);
        write_rgb_snapshot_result(config, request.camera_id, request, false, "timeout", "",
                                  "snapshot request expired before sender queue", logger);
        remove_file_quietly(path);
        return;
    }
    if(request.capture_not_before_unix_us > current_us + request_timeout_us) {
        logger.warn("rgb snapshot request ignored because capture schedule is too far in the future request_id="
                    + request.request_id);
        write_rgb_snapshot_result(config, request.camera_id, request, false, "error", "",
                                  "snapshot capture schedule is too far in the future", logger);
        remove_file_quietly(path);
        return;
    }

    CameraRuntime *camera = choose_rgb_snapshot_camera(cameras, request.camera_id);
    if(camera == nullptr) {
        logger.warn("rgb snapshot request matched no camera request_id=" + request.request_id
                    + " camera_id=" + request.camera_id);
        write_rgb_snapshot_result(config, request.camera_id, request, false, "error", "",
                                  "snapshot request matched no camera", logger);
        remove_file_quietly(path);
        return;
    }

    bool queued = false;
    std::string queue_error;
    {
        std::lock_guard<std::mutex> lock(camera->mutex);
        const bool duplicate = camera->pending_rgb_snapshots.count(request.request_id) != 0
                               || std::any_of(camera->rgb_snapshot_requests.begin(), camera->rgb_snapshot_requests.end(),
                                              [&](const auto &item) { return item.request_id == request.request_id; });
        if(duplicate) {
            remove_file_quietly(path);
            return;
        }
        else if(camera->rgb_snapshot_requests.size() + camera->pending_rgb_snapshots.size()
                >= kRgbSnapshotMaxPendingPerCamera) {
            queue_error = "snapshot request queue is full";
        }
        else {
            request.camera_id = camera->config.camera_id;
            const auto insert_at = std::upper_bound(
                camera->rgb_snapshot_requests.begin(), camera->rgb_snapshot_requests.end(), request,
                [](const RgbSnapshotRequest &left, const RgbSnapshotRequest &right) {
                    if(left.capture_not_before_unix_us != right.capture_not_before_unix_us) {
                        return left.capture_not_before_unix_us < right.capture_not_before_unix_us;
                    }
                    return left.request_id < right.request_id;
                });
            camera->rgb_snapshot_requests.insert(insert_at, request);
            queued = true;
        }
    }
    if(queued) {
        logger.info("rgb snapshot request queued request_id=" + request.request_id
                    + " camera_id=" + camera->config.camera_id
                    + " capture_not_before_unix_us=" + std::to_string(request.capture_not_before_unix_us));
    }
    else {
        logger.warn("rgb snapshot request rejected request_id=" + request.request_id + " error=" + queue_error);
        write_rgb_snapshot_result(config, camera->config.camera_id, request, false, "error", "", queue_error, logger);
    }
    remove_file_quietly(path);
}

void poll_rgb_snapshot_requests(const AppConfig &config,
                                const std::vector<std::unique_ptr<CameraRuntime>> &cameras,
                                Logger &logger,
                                std::chrono::steady_clock::time_point now,
                                std::chrono::steady_clock::time_point &next_poll) {
    if(now < next_poll) {
        return;
    }
    next_poll = now + kRgbSnapshotRequestPollInterval;

    const auto request_dir = default_rgb_snapshot_request_dir();
    std::error_code ec;
    std::filesystem::create_directories(request_dir, ec);
    if(ec) {
        logger.warn("rgb snapshot request directory unavailable path=" + request_dir.string() + " error=" + ec.message());
        return;
    }
    std::filesystem::directory_iterator it(request_dir, ec);
    if(ec) {
        logger.warn("rgb snapshot request directory scan failed path=" + request_dir.string() + " error=" + ec.message());
        return;
    }
    size_t processed = 0;
    for(const std::filesystem::directory_iterator end; it != end && processed < 16; it.increment(ec)) {
        if(ec) {
            logger.warn("rgb snapshot request directory iteration failed path=" + request_dir.string()
                        + " error=" + ec.message());
            return;
        }
        if(!it->is_regular_file(ec) || it->path().extension() != ".json") {
            continue;
        }
        process_rgb_snapshot_request_file(config, cameras, logger, it->path());
        ++processed;
    }
}

void expire_rgb_snapshot_requests(const AppConfig &config,
                                  const std::vector<std::unique_ptr<CameraRuntime>> &cameras,
                                  Logger &logger,
                                  std::chrono::steady_clock::time_point now) {
    for(const auto &camera : cameras) {
        if(!camera) {
            continue;
        }
        std::vector<RgbSnapshotRequest> expired;
        ReliableSnapshotQueue *queue = nullptr;
        {
            std::lock_guard<std::mutex> lock(camera->mutex);
            queue = camera->rgb_snapshot_queue;
            for(auto it = camera->pending_rgb_snapshots.begin(); it != camera->pending_rgb_snapshots.end();) {
                if(now - it->second.queued_at < kRgbSnapshotRequestTimeout) {
                    ++it;
                    continue;
                }
                expired.push_back(it->second.request);
                it = camera->pending_rgb_snapshots.erase(it);
            }
        }
        for(const auto &request : expired) {
            if(queue) {
                queue->cancel(request.request_id);
            }
            logger.warn("rgb snapshot request timed out request_id=" + request.request_id
                        + " camera_id=" + camera->config.camera_id);
            write_rgb_snapshot_result(config, camera->config.camera_id, request, false, "timeout", "",
                                      "receiver capture acknowledgement timeout", logger);
        }
    }
}

CameraRuntime *find_camera_by_id(const std::vector<std::unique_ptr<CameraRuntime>> &cameras, const std::string &camera_id) {
    for(const auto &camera : cameras) {
        if(camera && camera->config.camera_id == camera_id) {
            return camera.get();
        }
    }
    return nullptr;
}

void set_web_rgb_preview_active(CameraRuntime &camera, bool active, int lease_ms, Logger &logger) {
    const auto now = std::chrono::steady_clock::now();
    bool became_active = false;
    bool became_inactive = false;
    {
        std::lock_guard<std::mutex> lock(camera.mutex);
        const bool was_active = now <= camera.web_rgb_preview_requested_until;
        if(active) {
            const auto clamped_lease =
                std::chrono::milliseconds(std::clamp(lease_ms, 250, static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                                                         kWebRgbPreviewMaxLease)
                                                                                         .count())));
            camera.web_rgb_preview_requested_until = now + clamped_lease;
            if(camera.next_web_rgb_preview < now) {
                camera.next_web_rgb_preview = now;
            }
            became_active = !was_active;
        }
        else {
            camera.web_rgb_preview_requested_until = std::chrono::steady_clock::time_point::min();
            became_inactive = was_active;
        }
    }
    if(became_active) {
        logger.info("web rgb preview requested camera_id=" + camera.config.camera_id);
    }
    else if(became_inactive) {
        logger.info("web rgb preview request cleared camera_id=" + camera.config.camera_id);
    }
}

void handle_receiver_control_message(const AppConfig &config,
                                     const std::vector<std::unique_ptr<CameraRuntime>> &cameras,
                                     Logger &logger,
                                     const std::string &payload,
                                     const ClockSyncClient *clock_sync) {
    const auto root = parse_json_object(trim_copy(payload));
    if(!root) {
        return;
    }
    if(json_string_or(*root, "message_type") != "control") {
        return;
    }
    const std::string control = json_string_or(*root, "control");
    const std::string target_sender = json_string_or(*root, "sender_id");
    const std::string target_camera = json_string_or(*root, "camera_id");
    const std::string reason = json_string_or(*root, "reason");
    if(!target_sender.empty() && target_sender != "*" && target_sender != config.sender_id) {
        return;
    }

    if(control == "rgb_snapshot_result") {
        const std::string request_id = json_string_or(*root, "request_id");
        if(!is_safe_rgb_snapshot_request_id(request_id)) {
            return;
        }
        const bool ok = json_bool_or(*root, "ok", false);
        const std::string status = json_string_or(*root, "status", ok ? "captured" : "error");
        const std::string image_path = json_string_or(*root, "image_path");
        const std::string error = json_string_or(*root, "error");
        for(const auto &camera : cameras) {
            if(!camera || (!target_camera.empty() && target_camera != "*" && camera->config.camera_id != target_camera)) {
                continue;
            }
            std::optional<RgbSnapshotRequest> request;
            ReliableSnapshotQueue *queue = nullptr;
            {
                std::lock_guard<std::mutex> lock(camera->mutex);
                const auto pending = camera->pending_rgb_snapshots.find(request_id);
                if(pending == camera->pending_rgb_snapshots.end()) {
                    continue;
                }
                request = pending->second.request;
                camera->pending_rgb_snapshots.erase(pending);
                queue = camera->rgb_snapshot_queue;
            }
            if(queue) {
                queue->cancel(request_id);
            }
            logger.info("rgb snapshot result received request_id=" + request_id
                        + " camera_id=" + camera->config.camera_id
                        + " status=" + status
                        + " ok=" + (ok ? "true" : "false"));
            write_rgb_snapshot_result(config, camera->config.camera_id, *request, ok, status, image_path, error, logger);
            return;
        }
        return;
    }

    size_t matched = 0;
    const bool all_cameras = target_camera.empty() || target_camera == "*";
    if(control == "set_web_rgb_preview_active") {
        const bool active = json_bool_or(*root, "active", true);
        const int lease_ms = json_int_or(*root, "lease_ms", static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                             kWebRgbPreviewDefaultLease)
                                                             .count()));
        for(const auto &camera : cameras) {
            if(!camera) {
                continue;
            }
            if(all_cameras || camera->config.camera_id == target_camera) {
                set_web_rgb_preview_active(*camera, active, lease_ms, logger);
                ++matched;
            }
        }
        if(matched == 0) {
            logger.warn("web preview control matched no camera sender_id=" + config.sender_id + " camera_id=" + target_camera);
        }
        return;
    }

    if(control != "force_rgb_keyframe") {
        logger.warn("unknown receiver control ignored: " + control);
        return;
    }

    const uint64_t target_global_us = json_uint64_or(*root, "target_global_us", 0);
    uint64_t target_sender_system_us = target_global_us;
    if(target_global_us > 0 && clock_sync) {
        const auto state = clock_sync->state();
        if(state.valid) {
            target_sender_system_us = sender_system_time_from_global(target_global_us, state.offset_us);
        }
    }

    for(const auto &camera : cameras) {
        if(!camera) {
            continue;
        }
        if(all_cameras || camera->config.camera_id == target_camera) {
            request_rgb_keyframe(*camera, logger, reason.empty() ? "receiver_control" : reason,
                                 target_sender_system_us, target_global_us);
            ++matched;
        }
    }
    if(matched == 0) {
        logger.warn("force_rgb_keyframe control matched no camera sender_id=" + config.sender_id + " camera_id=" + target_camera);
    }
}

template <typename Sender>
void process_receiver_controls(Sender &transport,
                               const AppConfig &config,
                               const std::vector<std::unique_ptr<CameraRuntime>> &cameras,
                               Logger &logger,
                               std::mutex &transport_mutex,
                               const ClockSyncClient *clock_sync) {
    for(int i = 0; i < 16; ++i) {
        std::optional<std::string> payload;
        {
            std::lock_guard<std::mutex> lock(transport_mutex);
            payload = transport.receive_status_control(0);
        }
        if(!payload) {
            return;
        }
        handle_receiver_control_message(config, cameras, logger, *payload, clock_sync);
    }
}

bool aligned_rgb_preview_configured(const AppConfig &config) {
    if(!config.preview.aligned_rgb) {
        return false;
    }
    bool has_cam01 = false;
    bool has_cam02 = false;
    for(const auto &camera : config.cameras) {
        has_cam01 = has_cam01 || camera.camera_id == "cam01";
        has_cam02 = has_cam02 || camera.camera_id == "cam02";
    }
    return has_cam01 && has_cam02;
}

bool preview_aligned_rgb_pair(const std::vector<std::unique_ptr<CameraRuntime>> &cameras) {
    auto *cam01 = find_camera_by_id(cameras, "cam01");
    auto *cam02 = find_camera_by_id(cameras, "cam02");
    if(cam01 == nullptr || cam02 == nullptr) {
        return false;
    }

    static bool window_initialized = false;
    const auto pair = pop_aligned_rgb_preview_pair(*cam01, *cam02);
    if(!pair) {
        if(window_initialized) {
            cv::waitKey(1);
        }
        return false;
    }

    cv::Mat left;
    cv::Mat right;
    cv::resize(pair->cam01.bgr, left, cv::Size(640, 360));
    cv::resize(pair->cam02.bgr, right, cv::Size(640, 360));
    draw_rgb_preview_overlay(left, pair->cam01_label, pair->cam01, pair->cam01_minus_cam02_us, "cam01-cam02");
    draw_rgb_preview_overlay(right, pair->cam02_label, pair->cam02, -pair->cam01_minus_cam02_us, "cam02-cam01");

    cv::Mat wall;
    cv::hconcat(left, right, wall);
    cv::line(wall, cv::Point(640, 0), cv::Point(640, wall.rows), cv::Scalar(255, 255, 255), 1, cv::LINE_AA);
    const std::string window_name = "Gemini Sender RGB Aligned cam01 cam02";
    if(!window_initialized) {
        cv::namedWindow(window_name, cv::WINDOW_NORMAL);
        cv::resizeWindow(window_name, 1280, 360);
        cv::moveWindow(window_name, 80, 80);
        window_initialized = true;
    }
    cv::imshow(window_name, wall);
    cv::waitKey(1);
    return true;
}
