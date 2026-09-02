std::string json_to_string(const Json::Value &value) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, value);
}

std::optional<Json::Value> parse_json_object(const std::string &payload) {
    Json::CharReaderBuilder builder;
    builder["collectComments"] = false;
    builder["failIfExtra"] = true;
    builder["strictRoot"] = true;
    Json::Value root;
    std::string errors;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if(!reader->parse(payload.data(), payload.data() + payload.size(), &root, &errors) || !root.isObject()) {
        return std::nullopt;
    }
    return root;
}

std::string json_string_or(const Json::Value &value, const char *key, const std::string &fallback = "") {
    return value.isMember(key) && value[key].isString() ? value[key].asString() : fallback;
}

bool json_bool_or(const Json::Value &value, const char *key, bool fallback) {
    return value.isMember(key) && value[key].isBool() ? value[key].asBool() : fallback;
}

int json_int_or(const Json::Value &value, const char *key, int fallback) {
    return value.isMember(key) && value[key].isInt() ? value[key].asInt() : fallback;
}

uint64_t json_uint64_or(const Json::Value &value, const char *key, uint64_t fallback) {
    if(!value.isMember(key)) {
        return fallback;
    }
    const auto &item = value[key];
    if(item.isUInt64()) {
        return item.asUInt64();
    }
    if(item.isInt64() && item.asInt64() >= 0) {
        return static_cast<uint64_t>(item.asInt64());
    }
    return fallback;
}

double elapsed_ms(std::chrono::steady_clock::time_point started, std::chrono::steady_clock::time_point ended) {
    return std::chrono::duration<double, std::milli>(ended - started).count();
}

double avg_ms(double total_ms, uint64_t count) {
    return count == 0 ? 0.0 : total_ms / static_cast<double>(count);
}

double rate_per_second(uint64_t count, double seconds) {
    return seconds <= 0.0 ? 0.0 : static_cast<double>(count) / seconds;
}

template <typename FrameT>
int64_t metadata_or(const std::shared_ptr<FrameT> &frame, OBFrameMetadataType type, int64_t fallback = -1) {
    try {
        if(frame && frame->hasMetadata(type)) {
            return frame->getMetadataValue(type);
        }
    }
    catch(const std::exception &) {
    }
    return fallback;
}

int32_t diagnostic_i32_or_unknown(int64_t value) {
    if(value < 0) {
        return -1;
    }
    if(value > INT32_MAX) {
        return INT32_MAX;
    }
    return static_cast<int32_t>(value);
}

std::string bool_text(bool value) {
    return value ? "true" : "false";
}

void log_property_set_result(CameraRuntime &camera, Logger &logger, const std::string &name, const std::string &value,
                             const std::string &readback = "") {
    std::ostringstream oss;
    oss << "color control set camera_id=" << camera.config.camera_id << " name=" << name << " value=" << value;
    if(!readback.empty()) {
        oss << " readback=" << readback;
    }
    logger.info(oss.str());
}

void log_property_unsupported(CameraRuntime &camera, Logger &logger, const std::string &name, OBPropertyID property_id) {
    std::ostringstream oss;
    oss << "color control unsupported camera_id=" << camera.config.camera_id << " name=" << name
        << " property_id=" << static_cast<int>(property_id);
    logger.warn(oss.str());
}

void set_bool_property_if_configured(CameraRuntime &camera, Logger &logger, const std::string &name, OBPropertyID property_id,
                                     const std::optional<bool> &value) {
    if(!value) {
        return;
    }
    try {
        if(!camera.device->isPropertySupported(property_id, OB_PERMISSION_WRITE)) {
            log_property_unsupported(camera, logger, name, property_id);
            return;
        }
        camera.device->setBoolProperty(property_id, *value);
        std::string readback;
        if(camera.device->isPropertySupported(property_id, OB_PERMISSION_READ)) {
            readback = bool_text(camera.device->getBoolProperty(property_id));
        }
        log_property_set_result(camera, logger, name, bool_text(*value), readback);
    }
    catch(const std::exception &e) {
        logger.warn("color control set failed camera_id=" + camera.config.camera_id + " name=" + name + " error=" + e.what());
    }
}

void set_int_property_if_configured(CameraRuntime &camera, Logger &logger, const std::string &name, OBPropertyID property_id,
                                    const std::optional<int> &value) {
    if(!value) {
        return;
    }
    try {
        if(!camera.device->isPropertySupported(property_id, OB_PERMISSION_WRITE)) {
            log_property_unsupported(camera, logger, name, property_id);
            return;
        }
        camera.device->setIntProperty(property_id, *value);
        std::string readback;
        if(camera.device->isPropertySupported(property_id, OB_PERMISSION_READ)) {
            readback = std::to_string(camera.device->getIntProperty(property_id));
        }
        log_property_set_result(camera, logger, name, std::to_string(*value), readback);
    }
    catch(const std::exception &e) {
        logger.warn("color control set failed camera_id=" + camera.config.camera_id + " name=" + name + " error=" + e.what());
    }
}

int software_rgb_rotation_degrees(const CameraConfig &config) {
    return config.rotation_degrees.value_or(0) == 180 ? 180 : 0;
}

void apply_stream_rotation(CameraRuntime &camera, Logger &logger) {
    if(!camera.config.rotation_degrees) {
        return;
    }

    struct RotationProperty {
        OBPropertyID id;
        const char *name;
        int desired_value;
        std::optional<int> previous_value;
    };

    const int configured_rotation = *camera.config.rotation_degrees;
    const int software_rgb_rotation = software_rgb_rotation_degrees(camera.config);
    std::vector<RotationProperty> properties{
        {OB_PROP_COLOR_ROTATE_INT, "color", software_rgb_rotation == 0 ? configured_rotation : 0, std::nullopt}};
    if(camera.config.depth_profile.enabled) {
        properties.push_back({OB_PROP_DEPTH_ROTATE_INT, "depth", configured_rotation, std::nullopt});
    }

    for(auto &property : properties) {
        if(!camera.device->isPropertySupported(property.id, OB_PERMISSION_WRITE)) {
            throw std::runtime_error(std::string("configured stream rotation is unsupported for ") + property.name
                                     + " camera_id=" + camera.config.camera_id);
        }
        if(camera.device->isPropertySupported(property.id, OB_PERMISSION_READ)) {
            property.previous_value = camera.device->getIntProperty(property.id);
        }
    }

    try {
        for(auto &property : properties) {
            camera.device->setIntProperty(property.id, property.desired_value);
        }
        for(const auto &property : properties) {
            if(camera.device->isPropertySupported(property.id, OB_PERMISSION_READ)) {
                const int readback = camera.device->getIntProperty(property.id);
                if(readback != property.desired_value) {
                    throw std::runtime_error(std::string(property.name) + " rotation readback=" + std::to_string(readback));
                }
            }
        }
    }
    catch(const std::exception &e) {
        for(auto property = properties.rbegin(); property != properties.rend(); ++property) {
            if(property->previous_value) {
                try {
                    camera.device->setIntProperty(property->id, *property->previous_value);
                }
                catch(...) {
                }
            }
        }
        throw std::runtime_error("cannot apply atomic RGB/Depth stream rotation camera_id=" + camera.config.camera_id
                                 + " error=" + e.what());
    }

    logger.info("stream rotation set camera_id=" + camera.config.camera_id
                + " degrees=" + std::to_string(configured_rotation)
                + " color=" + (software_rgb_rotation == 0 ? "hardware" : "software")
                + " depth=" + (camera.config.depth_profile.enabled ? "hardware" : "disabled"));
}

void apply_color_controls(CameraRuntime &camera, Logger &logger) {
    const auto &controls = camera.config.color_controls;
    if(!controls.auto_exposure && !controls.exposure && !controls.gain && !controls.auto_exposure_priority && !controls.max_exposure
       && !controls.max_gain && !controls.power_line_frequency && !controls.auto_white_balance && !controls.white_balance
       && !controls.brightness && !controls.contrast && !controls.saturation && !controls.gamma
       && !controls.backlight_compensation) {
        return;
    }

    if(controls.auto_exposure && !*controls.auto_exposure) {
        set_bool_property_if_configured(camera, logger, "auto_exposure", OB_PROP_COLOR_AUTO_EXPOSURE_BOOL, controls.auto_exposure);
    }
    set_int_property_if_configured(camera, logger, "auto_exposure_priority", OB_PROP_COLOR_AUTO_EXPOSURE_PRIORITY_INT,
                                   controls.auto_exposure_priority);
    set_int_property_if_configured(camera, logger, "max_exposure", OB_PROP_COLOR_AE_MAX_EXPOSURE_INT, controls.max_exposure);
    set_int_property_if_configured(camera, logger, "max_gain", GWV3_ORBBEC_MAX_GAIN_PROPERTY, controls.max_gain);
    set_int_property_if_configured(camera, logger, "power_line_frequency", OB_PROP_COLOR_POWER_LINE_FREQUENCY_INT,
                                   controls.power_line_frequency);
    set_int_property_if_configured(camera, logger, "exposure", OB_PROP_COLOR_EXPOSURE_INT, controls.exposure);
    set_int_property_if_configured(camera, logger, "gain", OB_PROP_COLOR_GAIN_INT, controls.gain);
    if(controls.auto_white_balance && !*controls.auto_white_balance) {
        set_bool_property_if_configured(camera, logger, "auto_white_balance", OB_PROP_COLOR_AUTO_WHITE_BALANCE_BOOL,
                                        controls.auto_white_balance);
    }
    set_int_property_if_configured(camera, logger, "white_balance", OB_PROP_COLOR_WHITE_BALANCE_INT, controls.white_balance);
    set_int_property_if_configured(camera, logger, "brightness", OB_PROP_COLOR_BRIGHTNESS_INT, controls.brightness);
    set_int_property_if_configured(camera, logger, "contrast", OB_PROP_COLOR_CONTRAST_INT, controls.contrast);
    set_int_property_if_configured(camera, logger, "saturation", OB_PROP_COLOR_SATURATION_INT, controls.saturation);
    set_int_property_if_configured(camera, logger, "gamma", OB_PROP_COLOR_GAMMA_INT, controls.gamma);
    set_int_property_if_configured(camera, logger, "backlight_compensation", OB_PROP_COLOR_BACKLIGHT_COMPENSATION_INT,
                                   controls.backlight_compensation);
    if(controls.auto_white_balance && *controls.auto_white_balance) {
        set_bool_property_if_configured(camera, logger, "auto_white_balance", OB_PROP_COLOR_AUTO_WHITE_BALANCE_BOOL,
                                        controls.auto_white_balance);
    }
    if(controls.auto_exposure && *controls.auto_exposure) {
        set_bool_property_if_configured(camera, logger, "auto_exposure", OB_PROP_COLOR_AUTO_EXPOSURE_BOOL, controls.auto_exposure);
    }
}

void update_color_metadata(CameraRuntime &camera, const std::shared_ptr<ob::ColorFrame> &color) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    camera.live.color_auto_exposure = metadata_or(color, OB_FRAME_METADATA_TYPE_AUTO_EXPOSURE, camera.live.color_auto_exposure);
    camera.live.color_exposure = metadata_or(color, OB_FRAME_METADATA_TYPE_EXPOSURE, camera.live.color_exposure);
    camera.live.color_gain = metadata_or(color, OB_FRAME_METADATA_TYPE_GAIN, camera.live.color_gain);
    camera.live.color_actual_fps = metadata_or(color, OB_FRAME_METADATA_TYPE_ACTUAL_FRAME_RATE, camera.live.color_actual_fps);
    camera.live.color_frame_rate = metadata_or(color, OB_FRAME_METADATA_TYPE_FRAME_RATE, camera.live.color_frame_rate);
    camera.live.color_exposure_priority = metadata_or(color, OB_FRAME_METADATA_TYPE_EXPOSURE_PRIORITY, camera.live.color_exposure_priority);
}

RgbFrameDiagnostics rgb_frame_diagnostics(CameraRuntime &camera, const std::shared_ptr<ob::ColorFrame> &color) {
    int64_t fallback_auto_exposure = -1;
    int64_t fallback_exposure = -1;
    int64_t fallback_gain = -1;
    int64_t fallback_actual_fps = -1;
    {
        std::lock_guard<std::mutex> lock(camera.mutex);
        fallback_auto_exposure = camera.live.color_auto_exposure;
        fallback_exposure = camera.live.color_exposure;
        fallback_gain = camera.live.color_gain;
        fallback_actual_fps = camera.live.color_actual_fps;
    }

    RgbFrameDiagnostics diagnostics;
    diagnostics.auto_exposure =
        diagnostic_i32_or_unknown(metadata_or(color, OB_FRAME_METADATA_TYPE_AUTO_EXPOSURE, fallback_auto_exposure));
    diagnostics.exposure_us = diagnostic_i32_or_unknown(metadata_or(color, OB_FRAME_METADATA_TYPE_EXPOSURE, fallback_exposure));
    diagnostics.gain = diagnostic_i32_or_unknown(metadata_or(color, OB_FRAME_METADATA_TYPE_GAIN, fallback_gain));
    diagnostics.actual_fps = diagnostic_i32_or_unknown(metadata_or(color, OB_FRAME_METADATA_TYPE_ACTUAL_FRAME_RATE, fallback_actual_fps));
    return diagnostics;
}

int media_outage_restart_samples() {
    const char *value = std::getenv("GEMINI_SENDER_MEDIA_OUTAGE_RESTART_SAMPLES");
    if(value == nullptr || value[0] == '\0') {
        return 0;
    }
    try {
        return std::max(0, std::stoi(value));
    }
    catch(const std::exception &) {
        return 0;
    }
}

double capture_outage_fps_floor() {
    return 5.0;
}

int capture_stall_seconds() {
    const char *value = std::getenv("GEMINI_SENDER_CAPTURE_STALL_SECONDS");
    if(value == nullptr || value[0] == '\0') {
        return 8;
    }
    try {
        return std::max(2, std::stoi(value));
    }
    catch(const std::exception &) {
        return 8;
    }
}

int capture_stall_restart_samples() {
    const char *value = std::getenv("GEMINI_SENDER_CAPTURE_STALL_RESTART_SAMPLES");
    if(value == nullptr || value[0] == '\0') {
        return 3;
    }
    try {
        return std::max(1, std::stoi(value));
    }
    catch(const std::exception &) {
        return 3;
    }
}

int capture_stall_reconnect_cooldown_seconds() {
    const char *value = std::getenv("GEMINI_SENDER_CAPTURE_STALL_RECONNECT_COOLDOWN_SECONDS");
    if(value == nullptr || value[0] == '\0') {
        return 30;
    }
    try {
        return std::max(5, std::stoi(value));
    }
    catch(const std::exception &) {
        return 30;
    }
}

int camera_flap_restart_events() {
    const char *value = std::getenv("GEMINI_SENDER_CAMERA_FLAP_RESTART_EVENTS");
    if(value == nullptr || value[0] == '\0') {
        return 3;
    }
    try {
        return std::max(0, std::stoi(value));
    }
    catch(const std::exception &) {
        return 3;
    }
}

int camera_flap_window_seconds() {
    const char *value = std::getenv("GEMINI_SENDER_CAMERA_FLAP_WINDOW_SECONDS");
    if(value == nullptr || value[0] == '\0') {
        return 300;
    }
    try {
        return std::max(10, std::stoi(value));
    }
    catch(const std::exception &) {
        return 300;
    }
}

std::string read_text_file(const std::filesystem::path &path);

int camera_reconnect_process_restart_attempts() {
    const char *value = std::getenv("GEMINI_SENDER_CAMERA_RECONNECT_RESTART_ATTEMPTS");
    if(value == nullptr || value[0] == '\0') {
        return 12;
    }
    try {
        return std::max(0, std::stoi(value));
    }
    catch(const std::exception &) {
        return 12;
    }
}

int camera_reconnect_max_delay_seconds() {
    const char *value = std::getenv("GEMINI_SENDER_CAMERA_RECONNECT_MAX_DELAY_SECONDS");
    if(value == nullptr || value[0] == '\0') {
        return 30;
    }
    try {
        return std::max(1, std::stoi(value));
    }
    catch(const std::exception &) {
        return 30;
    }
}

bool is_no_orbbec_device_error(const std::string &error) {
    return error.find("no Orbbec device found") != std::string::npos;
}

bool system_orbbec_usb_present() {
    const std::filesystem::path root = "/sys/bus/usb/devices";
    std::error_code ec;
    if(!std::filesystem::exists(root, ec)) {
        return false;
    }
    for(const auto &entry : std::filesystem::directory_iterator(root, std::filesystem::directory_options::skip_permission_denied, ec)) {
        if(ec) {
            return false;
        }
        if(read_text_file(entry.path() / "idVendor") == "2bc5") {
            return true;
        }
    }
    return false;
}

void update_camera_reconnect_process_guard(CameraRuntime &camera, Logger &logger, uint32_t attempts, const std::string &error) {
    const int restart_attempts = camera_reconnect_process_restart_attempts();
    if(restart_attempts <= 0 || attempts < static_cast<uint32_t>(restart_attempts)) {
        return;
    }
    if(!is_no_orbbec_device_error(error) || !system_orbbec_usb_present()) {
        return;
    }

    logger.error("camera reconnect guard exiting sender for watchdog restart camera_id=" + camera.config.camera_id
                 + " attempts=" + std::to_string(attempts)
                 + " threshold=" + std::to_string(restart_attempts)
                 + " error=\"" + error + "\""
                 + " reason=orbbec_usb_present_but_sdk_enumeration_failed");
    g_running = false;
}

void update_media_outage_guard(CameraRuntime &camera, Logger &logger, const CameraPerfStats &perf) {
    const bool rgb_outage = camera.live.rgb_input_fps >= capture_outage_fps_floor() && perf.rgb_sent_packets == 0 && perf.rgb_send_failures > 0;
    const bool depth_outage = camera.live.depth_input_fps >= capture_outage_fps_floor()
                              && perf.depth_sent_frames == 0 && perf.depth_send_failures > 0;

    const int restart_samples = media_outage_restart_samples();
    if(!media_outage_restart_due(camera.media_outage_samples, restart_samples, rgb_outage || depth_outage)) {
        return;
    }

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    oss << "media outage guard stopping sender for watchdog restart camera_id=" << camera.config.camera_id
        << " samples=" << camera.media_outage_samples
        << " threshold=" << restart_samples
        << " rgb_input_fps=" << camera.live.rgb_input_fps
        << " depth_input_fps=" << camera.live.depth_input_fps
        << " rgb_sent_packets_s=" << camera.live.rgb_sent_fps
        << " depth_sent_fps=" << camera.live.depth_sent_fps
        << " rgb_send_failures=" << perf.rgb_send_failures
        << " depth_send_failures=" << perf.depth_send_failures;
    if(!camera.last_error.empty()) {
        oss << " last_error=" << camera.last_error;
    }
    logger.error(oss.str());
    g_running = false;
}

void update_capture_stall_guard(CameraRuntime &camera, Logger &logger, const CameraPerfStats &perf,
                                std::chrono::steady_clock::time_point now) {
    if(!camera.online) {
        camera.capture_stall_samples = 0;
        return;
    }

    const int stall_seconds = capture_stall_seconds();
    const auto stall_threshold = std::chrono::seconds(stall_seconds);
    const auto rgb_stale_for = now - camera.last_rgb_frame_at;
    const auto depth_stale_for = now - camera.last_depth_frame_at;
    const bool rgb_stalled = rgb_stale_for >= stall_threshold;
    const bool depth_stalled = camera.config.depth_profile.enabled && depth_stale_for >= stall_threshold;
    const bool worker_stuck = perf.wait_calls == 0 && (rgb_stalled || depth_stalled);
    if(!worker_stuck) {
        camera.capture_stall_samples = 0;
        return;
    }

    camera.capture_stall_samples++;
    const int restart_samples = capture_stall_restart_samples();
    if(camera.capture_stall_samples < static_cast<uint32_t>(restart_samples)) {
        return;
    }

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    oss << "capture worker stall guard exiting sender for watchdog restart camera_id=" << camera.config.camera_id
        << " samples=" << camera.capture_stall_samples
        << " threshold_samples=" << restart_samples
        << " stale_threshold_s=" << stall_seconds
        << " rgb_stale_s=" << std::chrono::duration<double>(rgb_stale_for).count()
        << " depth_stale_s=" << std::chrono::duration<double>(depth_stale_for).count()
        << " rgb_input_fps=" << camera.live.rgb_input_fps
        << " depth_input_fps=" << camera.live.depth_input_fps
        << " wait_calls=" << perf.wait_calls
        << " wait_timeouts=" << perf.wait_timeouts;
    if(!camera.last_error.empty()) {
        oss << " last_error=" << camera.last_error;
    }
    logger.error(oss.str());
    std::_Exit(75);
}

std::optional<std::string> capture_stream_stall_reason(CameraRuntime &camera, std::chrono::steady_clock::time_point now) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    if(!camera.online) {
        return std::nullopt;
    }
    if(now < camera.next_capture_stall_reconnect) {
        return std::nullopt;
    }

    const int stall_seconds = capture_stall_seconds();
    const auto stall_threshold = std::chrono::seconds(stall_seconds);
    const auto rgb_stale_for = now - camera.last_rgb_frame_at;
    const auto depth_stale_for = now - camera.last_depth_frame_at;
    const bool rgb_stalled = rgb_stale_for >= stall_threshold;
    const bool depth_stalled = camera.config.depth_profile.enabled && depth_stale_for >= stall_threshold;
    if(!rgb_stalled && !depth_stalled) {
        return std::nullopt;
    }

    const int cooldown_seconds = capture_stall_reconnect_cooldown_seconds();
    camera.next_capture_stall_reconnect = now + std::chrono::seconds(cooldown_seconds);

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    oss << "capture stream stalled stale_threshold_s=" << stall_seconds
        << " reconnect_cooldown_s=" << cooldown_seconds
        << " rgb_stale_s=" << std::chrono::duration<double>(rgb_stale_for).count()
        << " depth_stale_s=" << std::chrono::duration<double>(depth_stale_for).count();
    return oss.str();
}

void log_perf(CameraRuntime &camera, Logger &logger, std::chrono::steady_clock::time_point now) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    auto &perf = camera.perf;
    const double seconds = std::chrono::duration<double>(now - perf.interval_started).count();
    if(seconds <= 0.0) {
        return;
    }

    const uint64_t rgb_frame_id_delta =
        perf.rgb_frame_id_seen && perf.rgb_last_frame_id >= perf.rgb_first_frame_id ? perf.rgb_last_frame_id - perf.rgb_first_frame_id : 0;
    const uint64_t depth_frame_id_delta =
        perf.depth_frame_id_seen && perf.depth_last_frame_id >= perf.depth_first_frame_id ? perf.depth_last_frame_id - perf.depth_first_frame_id : 0;
    camera.live.rgb_input_fps = rate_per_second(perf.rgb_input_frames, seconds);
    camera.live.depth_input_fps = rate_per_second(perf.depth_input_frames, seconds);
    camera.live.rgb_sent_fps = rate_per_second(perf.rgb_sent_packets, seconds);
    camera.live.rgb_preview_sent_fps = rate_per_second(perf.rgb_preview_sent_packets, seconds);
    camera.live.depth_sent_fps = rate_per_second(perf.depth_sent_frames, seconds);
    camera.live.rgb_usb_mbps = seconds > 0.0 ? static_cast<double>(perf.rgb_input_bytes) * 8.0 / seconds / 1000000.0 : 0.0;
    camera.live.depth_usb_mbps = seconds > 0.0 ? static_cast<double>(perf.depth_input_bytes) * 8.0 / seconds / 1000000.0 : 0.0;
    camera.live.rgb_mbps = seconds > 0.0 ? static_cast<double>(perf.rgb_bytes) * 8.0 / seconds / 1000000.0 : 0.0;
    camera.live.rgb_preview_mbps = seconds > 0.0 ? static_cast<double>(perf.rgb_preview_bytes) * 8.0 / seconds / 1000000.0 : 0.0;
    camera.live.depth_mbps = seconds > 0.0 ? static_cast<double>(perf.depth_bytes) * 8.0 / seconds / 1000000.0 : 0.0;
    camera.live.rgb_encode_ms = avg_ms(perf.rgb_encode_ms, perf.rgb_input_frames);
    camera.live.rgb_preview_send_ms = avg_ms(perf.rgb_preview_send_ms, perf.rgb_preview_sent_packets + perf.rgb_preview_send_failures);
    camera.live.depth_compress_ms = avg_ms(perf.depth_compress_ms, perf.depth_input_frames);

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    oss << "perf camera_id=" << camera.config.camera_id << " interval_s=" << seconds
        << " rgb_input_fps=" << camera.live.rgb_input_fps
        << " depth_input_fps=" << camera.live.depth_input_fps
        << " rgb_sent_packets_s=" << camera.live.rgb_sent_fps
        << " rgb_preview_sent_packets_s=" << camera.live.rgb_preview_sent_fps
        << " depth_sent_fps=" << camera.live.depth_sent_fps
        << " rgb_frame_id_delta=" << rgb_frame_id_delta
        << " depth_frame_id_delta=" << depth_frame_id_delta
        << " rgb_usb_mbps=" << camera.live.rgb_usb_mbps
        << " depth_usb_mbps=" << camera.live.depth_usb_mbps
        << " rgb_mbps=" << camera.live.rgb_mbps
        << " rgb_preview_mbps=" << camera.live.rgb_preview_mbps
        << " depth_mbps=" << camera.live.depth_mbps
        << " wait_avg_ms=" << avg_ms(perf.wait_ms, perf.wait_calls)
        << " wait_timeouts=" << perf.wait_timeouts
        << " rgb_decode_avg_ms=" << avg_ms(perf.rgb_decode_ms, perf.rgb_input_frames)
        << " rgb_encode_avg_ms=" << camera.live.rgb_encode_ms
        << " rgb_send_avg_ms=" << avg_ms(perf.rgb_send_ms, perf.rgb_sent_packets + perf.rgb_send_failures)
        << " rgb_preview_send_avg_ms=" << camera.live.rgb_preview_send_ms
        << " depth_compress_avg_ms=" << camera.live.depth_compress_ms
        << " depth_send_avg_ms=" << avg_ms(perf.depth_send_ms, perf.depth_sent_frames + perf.depth_send_failures)
        << " depth_preview_avg_ms=" << avg_ms(perf.depth_preview_ms, perf.depth_input_frames)
        << " preview_avg_ms=" << avg_ms(perf.preview_ms, perf.wait_calls)
        << " rgb_ae=" << camera.live.color_auto_exposure
        << " rgb_exposure=" << camera.live.color_exposure
        << " rgb_gain=" << camera.live.color_gain
        << " rgb_meta_actual_fps=" << camera.live.color_actual_fps
        << " rgb_exposure_priority=" << camera.live.color_exposure_priority
        << " adaptive_ae=" << bool_text(camera.live.adaptive_exposure_enabled)
        << " adaptive_p50=" << camera.live.adaptive_luma_p50
        << " adaptive_p95=" << camera.live.adaptive_luma_p95
        << " adaptive_p99=" << camera.live.adaptive_luma_p99
        << " adaptive_highlights_pct=" << camera.live.adaptive_highlight_fraction * 100.0
        << " adaptive_exposure=" << camera.live.adaptive_requested_exposure
        << " adaptive_gain=" << camera.live.adaptive_requested_gain
        << " adaptive_adjustments=" << camera.live.adaptive_adjustments
        << " adaptive_failures=" << camera.live.adaptive_failures
        << " rgb_corrupt_jpeg_frames=" << perf.rgb_corrupt_jpeg_frames
        << " rgb_timing_mismatch_drops=" << perf.rgb_timing_mismatch_drops
        << " rgb_encoder_lag_resets=" << perf.rgb_encoder_lag_resets
        << " rgb_send_failures=" << perf.rgb_send_failures
        << " rgb_preview_send_failures=" << perf.rgb_preview_send_failures
        << " depth_send_failures=" << perf.depth_send_failures;
    logger.info(oss.str());
    update_capture_stall_guard(camera, logger, perf, now);
    update_media_outage_guard(camera, logger, perf);

    CameraPerfStats reset;
    reset.interval_started = now;
    perf = reset;
}

template <typename FrameT>
uint64_t frame_system_timestamp_us_or(const std::shared_ptr<FrameT> &frame, uint64_t fallback_us) {
    if(!frame) {
        return fallback_us;
    }
    const uint64_t timestamp = frame->systemTimeStampUs();
    return timestamp == 0 ? fallback_us : timestamp;
}

uint64_t normalize_capture_system_timestamp(CameraRuntime &camera,
                                            StreamType stream_type,
                                            uint64_t sdk_timestamp_us,
                                            uint64_t capture_host_us) {
    constexpr uint64_t kMaxSdkHostSkewUs = 500'000;
    const uint64_t skew = sdk_timestamp_us >= capture_host_us ? sdk_timestamp_us - capture_host_us : capture_host_us - sdk_timestamp_us;
    bool used_fallback = sdk_timestamp_us == 0 || skew > kMaxSdkHostSkewUs;
    uint64_t timestamp_us = used_fallback ? capture_host_us : sdk_timestamp_us;
    std::lock_guard<std::mutex> lock(camera.mutex);
    uint64_t &last = stream_type == StreamType::rgb ? camera.rgb_last_capture_system_timestamp_us
                                                    : camera.depth_last_capture_system_timestamp_us;
    if(last > 0 && timestamp_us <= last) {
        used_fallback = true;
        timestamp_us = capture_host_us > last ? capture_host_us : last + 1;
    }
    if(used_fallback) {
        camera.capture_timestamp_fallbacks++;
    }
    last = timestamp_us;
    return timestamp_us;
}

template <typename FrameT>
void append_frame_time_sync(std::ostringstream &oss, const std::string &prefix, const std::shared_ptr<FrameT> &frame, uint64_t host_now_us) {
    if(!frame) {
        oss << " " << prefix << "_present=0";
        return;
    }

    const uint64_t camera_device_timestamp_us = frame->timeStampUs();
    const uint64_t camera_system_timestamp_us = frame_system_timestamp_us_or(frame, host_now_us);
    const int64_t host_minus_camera_system_us =
        host_now_us >= camera_system_timestamp_us ? static_cast<int64_t>(host_now_us - camera_system_timestamp_us)
                                                  : -static_cast<int64_t>(camera_system_timestamp_us - host_now_us);

    oss << " " << prefix << "_present=1"
        << " " << prefix << "_frame_id=" << frame->index()
        << " " << prefix << "_canonical_timestamp_us=" << camera_system_timestamp_us
        << " " << prefix << "_device_timestamp_us=" << camera_device_timestamp_us
        << " " << prefix << "_system_timestamp_us=" << camera_system_timestamp_us
        << " " << prefix << "_host_minus_system_us=" << host_minus_camera_system_us;
}

void log_time_sync(CameraRuntime &camera, Logger &logger, const std::shared_ptr<ob::ColorFrame> &color,
                   const std::shared_ptr<ob::DepthFrame> &depth, const std::string &depth_output_camera_id,
                   std::chrono::steady_clock::time_point now) {
    if(now < camera.next_time_sync_log) {
        return;
    }

    const uint64_t host_now = now_us();
    std::ostringstream oss;
    oss << "time_sync camera_id=" << camera.config.camera_id << " host_now_us=" << host_now;
    append_frame_time_sync(oss, "rgb", color, host_now);
    append_frame_time_sync(oss, "depth", depth, host_now);
    if(depth && !depth_output_camera_id.empty() && depth_output_camera_id != camera.config.camera_id) {
        oss << " depth_output_camera_id=" << depth_output_camera_id;
    }
    logger.info(oss.str());

    camera.next_time_sync_log = now + std::chrono::seconds(5);
}

Json::Value base_message(const AppConfig &config, const std::string &type) {
    Json::Value msg;
    msg["protocol_version"] = kProtocolVersion;
    msg["message_type"] = type;
    msg["sender_id"] = config.sender_id;
    msg["build_commit"] = GWV3_GIT_COMMIT;
    msg["build_dirty"] = GWV3_GIT_DIRTY != 0;
    msg["build_source_hash"] = GWV3_SENDER_SOURCE_HASH;
    msg["orbbec_sdk_version"] = std::to_string(ob_get_major_version()) + "." + std::to_string(ob_get_minor_version()) + "."
                                   + std::to_string(ob_get_patch_version());
    msg["orbbec_sdk_version_number"] = ob_get_version();
    msg["timestamp_us"] = Json::UInt64(now_us());
    return msg;
}

Json::Value profile_json(const std::shared_ptr<ob::VideoStreamProfile> &profile, const std::string &pixel_format, const std::string &codec_or_compression,
                         float depth_scale = 0.0f) {
    Json::Value value;
    value["width"] = profile ? profile->width() : 0;
    value["height"] = profile ? profile->height() : 0;
    value["fps"] = profile ? profile->fps() : 0;
    value["pixel_format"] = pixel_format;
    if(pixel_format == "uint16") {
        value["compression"] = codec_or_compression;
        value["depth_scale"] = depth_scale;
    }
    else {
        value["codec"] = codec_or_compression;
    }
    return value;
}

Json::Value profile_json(const VideoProfileConfig &profile, const std::string &pixel_format, const std::string &codec_or_compression,
                         float depth_scale = 0.0f) {
    Json::Value value;
    value["width"] = profile.enabled ? profile.width : 0;
    value["height"] = profile.enabled ? profile.height : 0;
    value["fps"] = profile.enabled ? profile.fps : 0;
    value["pixel_format"] = pixel_format;
    if(pixel_format == "uint16") {
        value["compression"] = codec_or_compression;
        value["depth_scale"] = depth_scale;
    }
    else {
        value["codec"] = codec_or_compression;
    }
    return value;
}

Json::Value intrinsic_json(const OBCameraIntrinsic &intrinsic) {
    Json::Value value;
    value["fx"] = intrinsic.fx;
    value["fy"] = intrinsic.fy;
    value["cx"] = intrinsic.cx;
    value["cy"] = intrinsic.cy;
    value["width"] = intrinsic.width;
    value["height"] = intrinsic.height;
    return value;
}

Json::Value distortion_json(const OBCameraDistortion &distortion) {
    Json::Value value;
    value["k1"] = distortion.k1;
    value["k2"] = distortion.k2;
    value["k3"] = distortion.k3;
    value["k4"] = distortion.k4;
    value["k5"] = distortion.k5;
    value["k6"] = distortion.k6;
    value["p1"] = distortion.p1;
    value["p2"] = distortion.p2;
    return value;
}

Json::Value transform_json(const OBD2CTransform &transform) {
    Json::Value value;
    Json::Value rot(Json::arrayValue);
    Json::Value trans(Json::arrayValue);
    for(float item : transform.rot) {
        rot.append(item);
    }
    for(float item : transform.trans) {
        trans.append(item);
    }
    value["rot"] = rot;
    value["trans_mm"] = trans;
    return value;
}

Json::Value camera_param_data_json(const OBCameraParam &camera_param) {
    Json::Value data;
    data["depth_intrinsic"] = intrinsic_json(camera_param.depthIntrinsic);
    data["rgb_intrinsic"] = intrinsic_json(camera_param.rgbIntrinsic);
    data["depth_distortion"] = distortion_json(camera_param.depthDistortion);
    data["rgb_distortion"] = distortion_json(camera_param.rgbDistortion);
    data["d2c_transform"] = transform_json(camera_param.transform);
    data["is_mirrored"] = camera_param.isMirrored;
    return data;
}

bool intrinsic_valid(const OBCameraIntrinsic &intrinsic) {
    return intrinsic.fx > 0.0f && intrinsic.fy > 0.0f && intrinsic.width > 0 && intrinsic.height > 0;
}

bool transform_valid(const OBD2CTransform &transform) {
    float abs_sum = 0.0f;
    for(float item : transform.rot) {
        abs_sum += std::abs(item);
    }
    for(float item : transform.trans) {
        abs_sum += std::abs(item);
    }
    return abs_sum > 0.0f;
}

bool camera_param_complete(const OBCameraParam &camera_param) {
    return intrinsic_valid(camera_param.depthIntrinsic) && intrinsic_valid(camera_param.rgbIntrinsic) && transform_valid(camera_param.transform);
}

bool profile_dimensions_match(const OBCameraIntrinsic &intrinsic, const std::shared_ptr<ob::VideoStreamProfile> &profile) {
    return profile && intrinsic.width > 0 && intrinsic.height > 0 && static_cast<uint32_t>(intrinsic.width) == profile->width()
           && static_cast<uint32_t>(intrinsic.height) == profile->height();
}

bool camera_param_profiles_match(const OBCameraParam &camera_param,
                                 const std::shared_ptr<ob::VideoStreamProfile> &color_profile,
                                 const std::shared_ptr<ob::VideoStreamProfile> &depth_profile) {
    return camera_param_complete(camera_param) && profile_dimensions_match(camera_param.rgbIntrinsic, color_profile)
           && profile_dimensions_match(camera_param.depthIntrinsic, depth_profile);
}

OBCameraParam camera_param_from_calibration(const OBCalibrationParam &calibration) {
    OBCameraParam camera_param{};
    camera_param.depthIntrinsic = calibration.intrinsics[OB_SENSOR_DEPTH];
    camera_param.rgbIntrinsic = calibration.intrinsics[OB_SENSOR_COLOR];
    camera_param.depthDistortion = calibration.distortion[OB_SENSOR_DEPTH];
    camera_param.rgbDistortion = calibration.distortion[OB_SENSOR_COLOR];
    camera_param.transform = calibration.extrinsics[OB_SENSOR_DEPTH][OB_SENSOR_COLOR];
    camera_param.isMirrored = false;
    return camera_param;
}

void fill_missing_profile_calibration(OBCameraParam &camera_param, const std::shared_ptr<ob::VideoStreamProfile> &color_profile,
                                      const std::shared_ptr<ob::VideoStreamProfile> &depth_profile) {
    if(color_profile && !intrinsic_valid(camera_param.rgbIntrinsic)) {
        try {
            camera_param.rgbIntrinsic = color_profile->getIntrinsic();
            camera_param.rgbDistortion = color_profile->getDistortion();
        }
        catch(const std::exception &) {
        }
        catch(...) {
        }
    }
    if(depth_profile && !intrinsic_valid(camera_param.depthIntrinsic)) {
        try {
            camera_param.depthIntrinsic = depth_profile->getIntrinsic();
            camera_param.depthDistortion = depth_profile->getDistortion();
        }
        catch(const std::exception &) {
        }
        catch(...) {
        }
    }
}

std::optional<OBCameraParam> exact_profile_camera_param_from_device(const std::shared_ptr<ob::Device> &device,
                                                                    const std::shared_ptr<ob::VideoStreamProfile> &color_profile,
                                                                    const std::shared_ptr<ob::VideoStreamProfile> &depth_profile,
                                                                    Json::Value &raw_list_json) {
    if(!device) {
        return std::nullopt;
    }
    try {
        auto list = device->getCalibrationCameraParamList();
        const uint32_t count = list ? list->count() : 0;
        std::optional<OBCameraParam> exact;
        for(uint32_t i = 0; i < count; ++i) {
            const auto param = list->getCameraParam(i);
            Json::Value item = camera_param_data_json(param);
            item["index"] = i;
            raw_list_json.append(item);
            if(!exact && camera_param_profiles_match(param, color_profile, depth_profile)) {
                exact = param;
            }
        }
        return exact;
    }
    catch(const std::exception &) {
    }
    catch(...) {
    }
    return std::nullopt;
}

Json::Value calibration_json(ob::Pipeline &pipeline, const std::shared_ptr<ob::Config> &pipeline_config,
                             const std::shared_ptr<ob::Device> &device,
                             const std::shared_ptr<ob::VideoStreamProfile> &color_profile,
                             const std::shared_ptr<ob::VideoStreamProfile> &depth_profile) {
    Json::Value calibration;
    calibration["available"] = false;
    calibration["source"] = "orbbec_sdk";
    calibration["data"] = Json::objectValue;
    Json::Value raw_list(Json::arrayValue);
    try {
        std::optional<OBCameraParam> selected_param;
        std::string source_detail;
        auto select_if_exact = [&](OBCameraParam candidate, const std::string &detail, bool fill_missing) {
            if(fill_missing) {
                fill_missing_profile_calibration(candidate, color_profile, depth_profile);
            }
            if(!selected_param && camera_param_profiles_match(candidate, color_profile, depth_profile)) {
                selected_param = candidate;
                source_detail = detail;
            }
        };

        if(pipeline_config && color_profile && depth_profile) {
            try {
                select_if_exact(camera_param_from_calibration(pipeline.getCalibrationParam(pipeline_config)),
                                "pipeline_config_calibration", false);
            }
            catch(const std::exception &) {
            }
            catch(...) {
            }
        }

        if(!selected_param && color_profile && depth_profile) {
            try {
                select_if_exact(pipeline.getCameraParamWithProfile(color_profile->width(), color_profile->height(), depth_profile->width(),
                                                                   depth_profile->height()),
                                "pipeline_profile_calibration", true);
            }
            catch(const std::exception &) {
            }
            catch(...) {
            }
        }

        if(!selected_param) {
            const auto exact_list_param = exact_profile_camera_param_from_device(device, color_profile, depth_profile, raw_list);
            if(exact_list_param) {
                select_if_exact(*exact_list_param, "device_calibration_list_exact_profile", false);
            }
        }

        if(!selected_param) {
            try {
                select_if_exact(pipeline.getCameraParam(), "pipeline_default_exact_profile", true);
            }
            catch(const std::exception &) {
            }
            catch(...) {
            }
        }

        Json::Value data = selected_param ? camera_param_data_json(*selected_param) : Json::Value(Json::objectValue);
        if(!selected_param && !raw_list.empty()) {
            constexpr Json::ArrayIndex kMaxRawCalibrationDiagnostics = 4;
            Json::Value bounded_raw_list(Json::arrayValue);
            const auto keep_count = std::min(raw_list.size(), kMaxRawCalibrationDiagnostics);
            for(Json::ArrayIndex i = 0; i < keep_count; ++i) {
                bounded_raw_list.append(raw_list[i]);
            }
            data["raw_camera_param_count"] = raw_list.size();
            data["raw_camera_param_list"] = bounded_raw_list;
            data["raw_camera_param_list_truncated"] = raw_list.size() > keep_count;
        }
        calibration["available"] = selected_param.has_value();
        calibration["profile_aware"] = selected_param.has_value();
        if(selected_param) {
            calibration["source_detail"] = source_detail;
        }
        else {
            calibration["warning"] = "no_complete_calibration_for_active_profiles";
        }
        calibration["data"] = data;
    }
    catch(const std::exception &) {
        calibration["available"] = false;
    }
    catch(...) {
        calibration["available"] = false;
    }
    return calibration;
}

std::string ob_format_name(OBFormat format) {
    switch(format) {
    case OB_FORMAT_MJPG:
        return "mjpg";
    case OB_FORMAT_RGB:
        return "rgb";
    case OB_FORMAT_BGR:
        return "bgr";
    case OB_FORMAT_YUYV:
        return "yuyv";
    case OB_FORMAT_UYVY:
        return "uyvy";
    case OB_FORMAT_Y16:
        return "y16";
    case OB_FORMAT_Y11:
        return "y11";
    case OB_FORMAT_Y12:
        return "y12";
    case OB_FORMAT_H264:
        return "h264";
    case OB_FORMAT_H265:
        return "h265";
    default:
        return "unknown";
    }
}

OBFormat requested_format(const std::string &format, OBFormat fallback) {
    if(format == "mjpg") {
        return OB_FORMAT_MJPG;
    }
    if(format == "rgb") {
        return OB_FORMAT_RGB;
    }
    if(format == "bgr") {
        return OB_FORMAT_BGR;
    }
    if(format == "yuyv") {
        return OB_FORMAT_YUYV;
    }
    if(format == "uyvy") {
        return OB_FORMAT_UYVY;
    }
    if(format == "y16") {
        return OB_FORMAT_Y16;
    }
    if(format == "y11") {
        return OB_FORMAT_Y11;
    }
    if(format == "y12") {
        return OB_FORMAT_Y12;
    }
    if(format.empty() || format == "default") {
        return fallback;
    }
    return fallback;
}

std::shared_ptr<ob::VideoStreamProfile> select_profile(ob::Pipeline &pipeline, OBSensorType sensor_type, const VideoProfileConfig &requested,
                                                       OBFormat fallback_format, Logger &logger) {
    auto profiles = pipeline.getStreamProfileList(sensor_type);
    const int width = requested.width > 0 ? requested.width : OB_WIDTH_ANY;
    const int height = requested.height > 0 ? requested.height : OB_HEIGHT_ANY;
    const int fps = requested.fps > 0 ? requested.fps : OB_FPS_ANY;
    const auto format = requested_format(requested.format, fallback_format);
    try {
        return profiles->getVideoStreamProfile(width, height, format, fps);
    }
    catch(const ob::Error &e) {
        logger.warn(std::string("requested profile unavailable, using SDK default: ") + e.getMessage());
        return profiles->getProfile(OB_PROFILE_DEFAULT)->as<ob::VideoStreamProfile>();
    }
}

std::string trim_copy(std::string value) {
    while(!value.empty() && (value.back() == '\n' || value.back() == '\r' || value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }
    size_t first = 0;
    while(first < value.size() && (value[first] == ' ' || value[first] == '\t')) {
        ++first;
    }
    return first == 0 ? value : value.substr(first);
}

std::string read_text_file(const std::filesystem::path &path) {
    std::ifstream file(path);
    if(!file) {
        return "";
    }
    std::string value;
    std::getline(file, value);
    return trim_copy(value);
}

int checked_v4l2_ioctl(int fd, unsigned long request, void *arg) {
    int result = 0;
    do {
        result = ioctl(fd, request, arg);
    } while(result == -1 && errno == EINTR);
    return result;
}

uint64_t v4l2_buffer_system_timestamp_us(const v4l2_buffer &buffer, uint64_t fallback_system_us) {
    const uint64_t raw_us = static_cast<uint64_t>(buffer.timestamp.tv_sec) * 1'000'000ull
                            + static_cast<uint64_t>(buffer.timestamp.tv_usec);
    if(raw_us == 0) {
        return fallback_system_us;
    }
#ifdef V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC
    if((buffer.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC) {
        const auto steady_now = std::chrono::steady_clock::now().time_since_epoch();
        const uint64_t steady_now_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(steady_now).count());
        if(raw_us <= steady_now_us + 1'000'000ull) {
            const int64_t age_us = static_cast<int64_t>(steady_now_us) - static_cast<int64_t>(raw_us);
            const int64_t mapped = static_cast<int64_t>(fallback_system_us) - age_us;
            return mapped > 0 ? static_cast<uint64_t>(mapped) : fallback_system_us;
        }
    }
#endif
    constexpr uint64_t kEarliestPlausibleEpochUs = 1'577'836'800ull * 1'000'000ull;
    return raw_us >= kEarliestPlausibleEpochUs ? raw_us : fallback_system_us;
}

std::string v4l2_fourcc_string(uint32_t fourcc) {
    std::string value(4, ' ');
    value[0] = static_cast<char>(fourcc & 0xff);
    value[1] = static_cast<char>((fourcc >> 8) & 0xff);
    value[2] = static_cast<char>((fourcc >> 16) & 0xff);
    value[3] = static_cast<char>((fourcc >> 24) & 0xff);
    return value;
}

bool v4l2_device_supports_mjpeg(const std::string &path) {
    const int fd = open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if(fd < 0) {
        return false;
    }
    v4l2_capability cap{};
    const bool capture = checked_v4l2_ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0
                         && ((cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) || (cap.device_caps & V4L2_CAP_VIDEO_CAPTURE));
    bool mjpeg = false;
    if(capture) {
        v4l2_fmtdesc desc{};
        desc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        for(desc.index = 0; checked_v4l2_ioctl(fd, VIDIOC_ENUM_FMT, &desc) == 0; ++desc.index) {
            if(desc.pixelformat == V4L2_PIX_FMT_MJPEG || desc.pixelformat == V4L2_PIX_FMT_JPEG) {
                mjpeg = true;
                break;
            }
        }
    }
    close(fd);
    return capture && mjpeg;
}

std::string find_v4l2_device_by_serial(const std::string &serial) {
    if(serial.empty()) {
        return "";
    }
    const std::filesystem::path root = "/sys/class/video4linux";
    std::error_code ec;
    if(!std::filesystem::exists(root, ec)) {
        return "";
    }
    std::vector<std::string> candidates;
    for(const auto &entry : std::filesystem::directory_iterator(root, std::filesystem::directory_options::skip_permission_denied, ec)) {
        if(ec) {
            return "";
        }
        const auto name = entry.path().filename().string();
        if(name.rfind("video", 0) != 0) {
            continue;
        }
        if(read_text_file(entry.path() / "device" / ".." / "serial") != serial) {
            continue;
        }
        candidates.push_back("/dev/" + name);
    }
    std::sort(candidates.begin(), candidates.end(), [](const std::string &lhs, const std::string &rhs) {
        const auto number = [](const std::string &path) {
            const auto pos = path.find_last_not_of("0123456789");
            return pos == std::string::npos ? 0 : std::stoi(path.substr(pos + 1));
        };
        return number(lhs) < number(rhs);
    });
    for(const auto &candidate : candidates) {
        if(v4l2_device_supports_mjpeg(candidate)) {
            return candidate;
        }
    }
    return candidates.empty() ? "" : candidates.front();
}

class V4L2MjpegCapture {
public:
    struct Frame {
        const uint8_t *data = nullptr;
        size_t size = 0;
        uint64_t frame_id = 0;
        uint64_t system_timestamp_us = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        int buffer_index = -1;
    };

    ~V4L2MjpegCapture() { close_device(); }

    void open_device(const CameraConfig &config) {
        device_path_ = !config.video_device.empty() ? config.video_device : find_v4l2_device_by_serial(config.serial_number);
        if(device_path_.empty()) {
            throw std::runtime_error("v4l2 video device not found for serial=" + config.serial_number);
        }
        fd_ = open(device_path_.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if(fd_ < 0) {
            throw std::runtime_error("failed to open v4l2 device " + device_path_ + ": " + std::strerror(errno));
        }

        v4l2_capability cap{};
        if(checked_v4l2_ioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0) {
            throw std::runtime_error("VIDIOC_QUERYCAP failed for " + device_path_ + ": " + std::strerror(errno));
        }
        const auto capabilities = cap.device_caps ? cap.device_caps : cap.capabilities;
        if((capabilities & V4L2_CAP_VIDEO_CAPTURE) == 0 || (capabilities & V4L2_CAP_STREAMING) == 0) {
            throw std::runtime_error("v4l2 device does not support capture streaming: " + device_path_);
        }

        v4l2_format fmt{};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = static_cast<uint32_t>(config.rgb_profile.width);
        fmt.fmt.pix.height = static_cast<uint32_t>(config.rgb_profile.height);
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
        fmt.fmt.pix.field = V4L2_FIELD_ANY;
        if(checked_v4l2_ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
            throw std::runtime_error("VIDIOC_S_FMT MJPG failed for " + device_path_ + ": " + std::strerror(errno));
        }
        if(fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG && fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_JPEG) {
            throw std::runtime_error("v4l2 device returned non-MJPEG format " + v4l2_fourcc_string(fmt.fmt.pix.pixelformat)
                                     + " for " + device_path_);
        }
        width_ = fmt.fmt.pix.width;
        height_ = fmt.fmt.pix.height;
        if(width_ != static_cast<uint32_t>(config.rgb_profile.width) || height_ != static_cast<uint32_t>(config.rgb_profile.height)) {
            std::ostringstream oss;
            oss << "v4l2 device returned unexpected size " << width_ << "x" << height_ << " for " << device_path_;
            throw std::runtime_error(oss.str());
        }

        v4l2_streamparm parm{};
        parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        parm.parm.capture.timeperframe.numerator = 1;
        parm.parm.capture.timeperframe.denominator = static_cast<uint32_t>(std::max(1, config.rgb_profile.fps));
        if(checked_v4l2_ioctl(fd_, VIDIOC_S_PARM, &parm) < 0) {
            throw std::runtime_error("VIDIOC_S_PARM failed for " + device_path_ + ": " + std::strerror(errno));
        }
        fps_ = parm.parm.capture.timeperframe.numerator > 0
                   ? static_cast<int>(parm.parm.capture.timeperframe.denominator / parm.parm.capture.timeperframe.numerator)
                   : config.rgb_profile.fps;
        if(fps_ <= 0) {
            fps_ = config.rgb_profile.fps;
        }
        if(config.rgb_profile.fps > 0 && fps_ != config.rgb_profile.fps) {
            throw std::runtime_error("v4l2 device returned unexpected fps " + std::to_string(fps_) + " for " + device_path_);
        }
        apply_color_controls(config.color_controls);

        v4l2_requestbuffers req{};
        req.count = 4;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        if(checked_v4l2_ioctl(fd_, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
            throw std::runtime_error("VIDIOC_REQBUFS failed for " + device_path_ + ": " + std::strerror(errno));
        }
        buffers_.resize(req.count);
        for(uint32_t i = 0; i < req.count; ++i) {
            v4l2_buffer buf{};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;
            if(checked_v4l2_ioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) {
                throw std::runtime_error("VIDIOC_QUERYBUF failed for " + device_path_ + ": " + std::strerror(errno));
            }
            buffers_[i].length = buf.length;
            buffers_[i].start = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, buf.m.offset);
            if(buffers_[i].start == MAP_FAILED) {
                buffers_[i].start = nullptr;
                throw std::runtime_error("mmap failed for " + device_path_ + ": " + std::strerror(errno));
            }
            if(checked_v4l2_ioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
                throw std::runtime_error("VIDIOC_QBUF failed for " + device_path_ + ": " + std::strerror(errno));
            }
        }
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if(checked_v4l2_ioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
            throw std::runtime_error("VIDIOC_STREAMON failed for " + device_path_ + ": " + std::strerror(errno));
        }
        streaming_ = true;
    }

    bool wait_frame(Frame &frame, std::chrono::milliseconds timeout) {
        pollfd pfd{};
        pfd.fd = fd_;
        pfd.events = POLLIN;
        const int poll_result = poll(&pfd, 1, static_cast<int>(timeout.count()));
        if(poll_result == 0) {
            return false;
        }
        if(poll_result < 0) {
            if(errno == EINTR) {
                return false;
            }
            throw std::runtime_error("poll failed for " + device_path_ + ": " + std::strerror(errno));
        }
        if((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            throw std::runtime_error("v4l2 poll error on " + device_path_);
        }

        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if(checked_v4l2_ioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
            if(errno == EAGAIN) {
                return false;
            }
            throw std::runtime_error("VIDIOC_DQBUF failed for " + device_path_ + ": " + std::strerror(errno));
        }
        if(buf.index >= buffers_.size()) {
            throw std::runtime_error("v4l2 returned invalid buffer index for " + device_path_);
        }
        frame.data = static_cast<const uint8_t *>(buffers_[buf.index].start);
        frame.size = buf.bytesused;
        frame.frame_id = next_frame_id_++;
        frame.system_timestamp_us = v4l2_buffer_system_timestamp_us(buf, now_us());
        frame.width = width_;
        frame.height = height_;
        frame.buffer_index = static_cast<int>(buf.index);
        return true;
    }

    void release_frame(Frame &frame) {
        if(frame.buffer_index < 0 || fd_ < 0) {
            return;
        }
        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = static_cast<uint32_t>(frame.buffer_index);
        if(checked_v4l2_ioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
            frame.buffer_index = -1;
            frame.data = nullptr;
            frame.size = 0;
            throw std::runtime_error("VIDIOC_QBUF failed for " + device_path_ + ": " + std::strerror(errno));
        }
        frame.buffer_index = -1;
        frame.data = nullptr;
        frame.size = 0;
    }

    void close_device() {
        if(fd_ >= 0 && streaming_) {
            v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            checked_v4l2_ioctl(fd_, VIDIOC_STREAMOFF, &type);
            streaming_ = false;
        }
        for(auto &buffer : buffers_) {
            if(buffer.start && buffer.length > 0) {
                munmap(buffer.start, buffer.length);
                buffer.start = nullptr;
                buffer.length = 0;
            }
        }
        buffers_.clear();
        if(fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
    }

    const std::string &device_path() const { return device_path_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    int fps() const { return fps_; }

private:
    struct Buffer {
        void *start = nullptr;
        size_t length = 0;
    };

    void set_control(uint32_t id, int value) {
        v4l2_control control{};
        control.id = id;
        control.value = value;
        if(checked_v4l2_ioctl(fd_, VIDIOC_S_CTRL, &control) < 0) {
            throw std::runtime_error("VIDIOC_S_CTRL id=" + std::to_string(id) + " value=" + std::to_string(value)
                                     + " failed for " + device_path_ + ": " + std::strerror(errno));
        }
    }

    void set_control_if_configured(uint32_t id, const std::optional<int> &value) {
        if(value) {
            set_control(id, *value);
        }
    }

    void set_bool_control_if_configured(uint32_t id, const std::optional<bool> &value) {
        if(value) {
            set_control(id, *value ? 1 : 0);
        }
    }

    void apply_color_controls(const ColorControlsConfig &controls) {
        if(controls.auto_exposure && !*controls.auto_exposure) {
            set_control(V4L2_CID_EXPOSURE_AUTO, V4L2_EXPOSURE_MANUAL);
        }
        set_control_if_configured(V4L2_CID_EXPOSURE_AUTO_PRIORITY, controls.auto_exposure_priority);
        set_control_if_configured(V4L2_CID_POWER_LINE_FREQUENCY, controls.power_line_frequency);
        set_control_if_configured(V4L2_CID_EXPOSURE_ABSOLUTE, controls.exposure);
        set_control_if_configured(V4L2_CID_GAIN, controls.gain);
        if(controls.auto_white_balance && !*controls.auto_white_balance) {
            set_bool_control_if_configured(V4L2_CID_AUTO_WHITE_BALANCE, controls.auto_white_balance);
        }
        set_control_if_configured(V4L2_CID_WHITE_BALANCE_TEMPERATURE, controls.white_balance);
        set_control_if_configured(V4L2_CID_BRIGHTNESS, controls.brightness);
        set_control_if_configured(V4L2_CID_CONTRAST, controls.contrast);
        set_control_if_configured(V4L2_CID_SATURATION, controls.saturation);
        set_control_if_configured(V4L2_CID_GAMMA, controls.gamma);
        set_control_if_configured(V4L2_CID_BACKLIGHT_COMPENSATION, controls.backlight_compensation);
        if(controls.auto_white_balance && *controls.auto_white_balance) {
            set_bool_control_if_configured(V4L2_CID_AUTO_WHITE_BALANCE, controls.auto_white_balance);
        }
        if(controls.auto_exposure && *controls.auto_exposure) {
            set_control(V4L2_CID_EXPOSURE_AUTO, V4L2_EXPOSURE_APERTURE_PRIORITY);
        }
    }

    int fd_ = -1;
    bool streaming_ = false;
    std::string device_path_;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    int fps_ = 0;
    uint64_t next_frame_id_ = 0;
    std::vector<Buffer> buffers_;
};

std::string existing_usb_device_name(std::string uid) {
    const std::filesystem::path root = "/sys/bus/usb/devices";
    if(uid.empty()) {
        return "";
    }
    if(std::filesystem::exists(root / uid)) {
        return uid;
    }
    const auto last_dash = uid.rfind('-');
    if(last_dash != std::string::npos) {
        const auto shortened = uid.substr(0, last_dash);
        if(std::filesystem::exists(root / shortened)) {
            return shortened;
        }
    }
    return "";
}

bool usb_uid_matches(const std::string &configured_uid, const std::string &device_uid) {
    if(configured_uid.empty() || device_uid.empty()) {
        return false;
    }
    if(configured_uid == device_uid) {
        return true;
    }

    const auto configured_device = existing_usb_device_name(configured_uid);
    const auto actual_device = existing_usb_device_name(device_uid);
    return !configured_device.empty() && configured_device == actual_device;
}

bool split_usb_device_name(const std::string &device_name, int &bus, std::string &port_path) {
    const auto dash = device_name.find('-');
    if(dash == std::string::npos || dash == 0 || dash + 1 >= device_name.size()) {
        return false;
    }
    try {
        bus = std::stoi(device_name.substr(0, dash));
    }
    catch(const std::exception &) {
        return false;
    }
    port_path = device_name.substr(dash + 1);
    return true;
}

std::string previous_usb_port_path(std::string port_path) {
    const auto dot = port_path.rfind('.');
    const size_t last_port_begin = dot == std::string::npos ? 0 : dot + 1;
    try {
        const int last_port = std::stoi(port_path.substr(last_port_begin));
        if(last_port <= 1) {
            return "";
        }
        port_path.replace(last_port_begin, std::string::npos, std::to_string(last_port - 1));
    }
    catch(const std::exception &) {
        return "";
    }

    return port_path;
}

std::vector<std::string> paired_color_device_candidates(const std::string &depth_device_name) {
    int bus = 0;
    std::string port_path;
    if(!split_usb_device_name(depth_device_name, bus, port_path)) {
        return {};
    }

    const auto color_port_path = previous_usb_port_path(port_path);
    if(color_port_path.empty()) {
        return {};
    }

    std::vector<std::string> candidates;
    candidates.push_back(std::to_string(bus) + "-" + color_port_path);
    if(bus > 1) {
        candidates.push_back(std::to_string(bus - 1) + "-" + color_port_path);
    }
    return candidates;
}

std::string paired_rgb_serial_for_depth_uid(const std::string &uid) {
    const std::filesystem::path root = "/sys/bus/usb/devices";
    const auto depth_name = existing_usb_device_name(uid);
    if(depth_name.empty()) {
        return "";
    }
    for(const auto &color_name : paired_color_device_candidates(depth_name)) {
        const auto color_path = root / color_name;
        if(read_text_file(color_path / "idVendor") == "2bc5" && read_text_file(color_path / "idProduct") == "0511") {
            return read_text_file(color_path / "serial");
        }
    }
    return "";
}

struct OrbbecDeviceIdentity {
    uint32_t index = 0;
    std::string model;
    std::string serial;
    std::string uid;
    std::string paired_rgb_serial;
    std::string connection_type;
};

std::string device_list_model_or_empty(ob::DeviceList &devices, uint32_t index) {
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    const char *model = devices.name(index);
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
    return model ? model : "";
}

std::vector<OrbbecDeviceIdentity> enumerate_orbbec_devices(ob::Context &ctx) {
    std::vector<OrbbecDeviceIdentity> identities;
    auto devices = ctx.queryDeviceList();
    identities.reserve(devices->deviceCount());
    for(uint32_t i = 0; i < devices->deviceCount(); ++i) {
        OrbbecDeviceIdentity identity;
        identity.index = i;
        identity.model = device_list_model_or_empty(*devices, i);
        identity.serial = devices->serialNumber(i) ? devices->serialNumber(i) : "";
        identity.uid = devices->uid(i) ? devices->uid(i) : "";
        identity.paired_rgb_serial = paired_rgb_serial_for_depth_uid(identity.uid);
        identity.connection_type = devices->connectionType(i) ? devices->connectionType(i) : "";
        identities.push_back(std::move(identity));
    }
    return identities;
}

bool device_identity_matches_config(const OrbbecDeviceIdentity &identity, const CameraConfig &camera) {
    if(!camera.device_model.empty() && identity.model != camera.device_model) {
        return false;
    }
    if(!camera.uid.empty() && usb_uid_matches(camera.uid, identity.uid)) {
        return true;
    }
    if(!camera.serial_number.empty()
       && (identity.serial == camera.serial_number || identity.paired_rgb_serial == camera.serial_number)) {
        return true;
    }
    if(camera.uid.empty() && camera.serial_number.empty() && !camera.device_model.empty()) {
        return true;
    }
    if(camera.uid.empty() && camera.serial_number.empty() && camera.device_index >= 0
       && identity.index == static_cast<uint32_t>(camera.device_index)) {
        return true;
    }
    return false;
}

std::string device_identity_summary(const OrbbecDeviceIdentity &identity) {
    std::ostringstream oss;
    oss << "index=" << identity.index << " model=" << identity.model << " serial=" << identity.serial << " uid=" << identity.uid
        << " paired_rgb_serial=" << identity.paired_rgb_serial << " connection=" << identity.connection_type;
    return oss.str();
}

std::string device_info_serial_or_empty(const std::shared_ptr<ob::Device> &device) {
    if(!device) {
        return "";
    }
    try {
        auto info = device->getDeviceInfo();
        return info && info->serialNumber() ? info->serialNumber() : "";
    }
    catch(const std::exception &) {
    }
    catch(...) {
    }
    return "";
}

bool serial_matches_device(const std::string &configured_serial, const std::string &list_serial, const std::string &info_serial,
                           const std::string &paired_rgb_serial) {
    return !configured_serial.empty()
           && (configured_serial == list_serial || configured_serial == info_serial || configured_serial == paired_rgb_serial);
}

std::string safe_device_list_string(const std::shared_ptr<ob::DeviceList> &devices) {
    if(!devices) {
        return "none";
    }
    std::ostringstream oss;
    const uint32_t count = devices->deviceCount();
    if(count == 0) {
        return "none";
    }
    for(uint32_t i = 0; i < count; ++i) {
        if(i > 0) {
            oss << "; ";
        }
        const std::string model = device_list_model_or_empty(*devices, i);
        const std::string uid = devices->uid(i) ? devices->uid(i) : "";
        const std::string paired_rgb_serial = paired_rgb_serial_for_depth_uid(uid);
        std::string info_serial;
        try {
            info_serial = device_info_serial_or_empty(devices->getDevice(i));
        }
        catch(const std::exception &) {
        }
        catch(...) {
        }
        oss << "index=" << i << " model=" << model
            << " serial=" << (devices->serialNumber(i) ? devices->serialNumber(i) : "")
            << " info_serial=" << info_serial << " uid=" << uid << " paired_rgb_serial=" << paired_rgb_serial
            << " connection=" << (devices->connectionType(i) ? devices->connectionType(i) : "");
    }
    return oss.str();
}

std::shared_ptr<ob::Device> select_device(ob::Context &ctx, const CameraConfig &camera) {
    auto devices = ctx.queryDeviceList();
    if(devices->deviceCount() == 0) {
        throw std::runtime_error("no Orbbec device found");
    }
    const auto model_at = [&devices](uint32_t index) {
        return device_list_model_or_empty(*devices, index);
    };
    const auto require_model_match = [&](uint32_t index, const std::string &matched_by) {
        const std::string actual_model = model_at(index);
        if(!camera.device_model.empty() && actual_model != camera.device_model) {
            std::ostringstream oss;
            oss << "configured camera " << matched_by << " but device_model mismatched camera_id=" << camera.camera_id
                << " configured_model=" << camera.device_model << " actual_model=" << actual_model
                << " available=[" << safe_device_list_string(devices) << "]";
            throw std::runtime_error(oss.str());
        }
    };
    if(!camera.uid.empty()) {
        for(uint32_t i = 0; i < devices->deviceCount(); ++i) {
            const std::string serial = devices->serialNumber(i) ? devices->serialNumber(i) : "";
            const std::string uid = devices->uid(i) ? devices->uid(i) : "";
            const std::string paired_rgb_serial = paired_rgb_serial_for_depth_uid(uid);
            if(!usb_uid_matches(camera.uid, uid)) {
                continue;
            }
            require_model_match(i, "uid matched");
            auto device = devices->getDevice(i);
            const auto info_serial = device_info_serial_or_empty(device);
            if(camera.serial_number.empty() || serial_matches_device(camera.serial_number, serial, info_serial, paired_rgb_serial)) {
                return device;
            }
            std::ostringstream oss;
            oss << "configured camera uid matched but serial_number mismatched camera_id=" << camera.camera_id
                << " uid=" << camera.uid << " matched_uid=" << uid
                << " configured_serial=" << camera.serial_number
                << " device_serial=" << serial
                << " device_info_serial=" << info_serial
                << " paired_rgb_serial=" << paired_rgb_serial;
            throw std::runtime_error(oss.str());
        }

        std::ostringstream oss;
        oss << "configured camera not found camera_id=" << camera.camera_id << " uid=" << camera.uid;
        if(!camera.serial_number.empty()) {
            oss << " serial=" << camera.serial_number;
        }
        oss << " available=[" << safe_device_list_string(devices) << "]";
        throw std::runtime_error(oss.str());
    }

    if(!camera.serial_number.empty()) {
        for(uint32_t i = 0; i < devices->deviceCount(); ++i) {
            const std::string serial = devices->serialNumber(i) ? devices->serialNumber(i) : "";
            auto device = devices->getDevice(i);
            const auto info_serial = device_info_serial_or_empty(device);
            if(serial == camera.serial_number || info_serial == camera.serial_number) {
                require_model_match(i, "serial_number matched");
                return device;
            }
        }

        std::shared_ptr<ob::Device> paired_match;
        std::string paired_match_uid;
        for(uint32_t i = 0; i < devices->deviceCount(); ++i) {
            const std::string uid = devices->uid(i) ? devices->uid(i) : "";
            const std::string paired_rgb_serial = paired_rgb_serial_for_depth_uid(uid);
            if(paired_rgb_serial != camera.serial_number) {
                continue;
            }
            require_model_match(i, "paired RGB serial_number matched");
            if(paired_match) {
                std::ostringstream oss;
                oss << "configured serial_number is ambiguous camera_id=" << camera.camera_id
                    << " serial=" << camera.serial_number
                    << " first_uid=" << paired_match_uid
                    << " second_uid=" << uid
                    << " available=[" << safe_device_list_string(devices) << "]";
                throw std::runtime_error(oss.str());
            }
            paired_match = devices->getDevice(i);
            paired_match_uid = uid;
        }
        if(paired_match) {
            return paired_match;
        }

        std::ostringstream oss;
        oss << "configured camera not found camera_id=" << camera.camera_id << " serial=" << camera.serial_number;
        oss << " available=[" << safe_device_list_string(devices) << "]";
        throw std::runtime_error(oss.str());
    }

    if(!camera.device_model.empty()) {
        std::vector<uint32_t> model_matches;
        for(uint32_t i = 0; i < devices->deviceCount(); ++i) {
            if(model_at(i) == camera.device_model) {
                model_matches.push_back(i);
            }
        }
        if(model_matches.empty()) {
            std::ostringstream oss;
            oss << "configured camera model not found camera_id=" << camera.camera_id
                << " device_model=" << camera.device_model
                << " available=[" << safe_device_list_string(devices) << "]";
            throw std::runtime_error(oss.str());
        }
        if(model_matches.size() == 1) {
            return devices->getDevice(model_matches.front());
        }
        if(camera.device_index >= 0) {
            const auto configured_index = static_cast<uint32_t>(camera.device_index);
            if(std::find(model_matches.begin(), model_matches.end(), configured_index) != model_matches.end()) {
                return devices->getDevice(configured_index);
            }
        }
        std::ostringstream oss;
        oss << "configured camera model is ambiguous camera_id=" << camera.camera_id
            << " device_model=" << camera.device_model << " device_index=" << camera.device_index
            << " available=[" << safe_device_list_string(devices) << "]";
        throw std::runtime_error(oss.str());
    }

    if(camera.device_index < 0 || static_cast<uint32_t>(camera.device_index) >= devices->deviceCount()) {
        std::ostringstream oss;
        oss << "camera device_index out of range camera_id=" << camera.camera_id << " device_index=" << camera.device_index
            << " available_count=" << devices->deviceCount() << " available=[" << safe_device_list_string(devices) << "]";
        throw std::runtime_error(oss.str());
    }
    return devices->getDevice(static_cast<uint32_t>(camera.device_index));
}

cv::Mat mjpg_to_bgr(const std::shared_ptr<ob::ColorFrame> &frame, int flags) {
    cv::Mat raw(1, static_cast<int>(frame->dataSize()), CV_8UC1, frame->data());
    return cv::imdecode(raw, flags);
}

cv::Mat mjpg_buffer_to_bgr(const uint8_t *data, size_t size, int flags) {
    cv::Mat raw(1, static_cast<int>(size), CV_8UC1, const_cast<uint8_t *>(data));
    return cv::imdecode(raw, flags);
}

size_t mjpg_complete_jpeg_size(const void *payload, size_t payload_size) {
    if(!payload || payload_size < 4) {
        return 0;
    }
    const auto *data = static_cast<const uint8_t *>(payload);
    const size_t size = payload_size;
    if(data[0] != 0xff || data[1] != 0xd8) {
        return 0;
    }

    size_t end = size;
    while(end > 0 && data[end - 1] == 0x00) {
        --end;
    }
    return end >= 4 && data[end - 2] == 0xff && data[end - 1] == 0xd9 ? end : 0;
}

bool mjpg_has_complete_jpeg_data(const void *payload, size_t payload_size) {
    return mjpg_complete_jpeg_size(payload, payload_size) != 0;
}

bool mjpg_has_complete_jpeg(const std::shared_ptr<ob::ColorFrame> &frame) {
    return frame && mjpg_has_complete_jpeg_data(frame->data(), frame->dataSize());
}

struct JpegValidationError {
    jpeg_error_mgr pub;
    jmp_buf jump_buffer;
    bool warning = false;
    JSAMPLE *row_buffer = nullptr;
    char message[JMSG_LENGTH_MAX] = {};
};

void jpeg_validation_error_exit(j_common_ptr cinfo) {
    auto *error = reinterpret_cast<JpegValidationError *>(cinfo->err);
    (*cinfo->err->format_message)(cinfo, error->message);
    longjmp(error->jump_buffer, 1);
}

void jpeg_validation_emit_message(j_common_ptr cinfo, int msg_level) {
    if(msg_level >= 0) {
        return;
    }
    auto *error = reinterpret_cast<JpegValidationError *>(cinfo->err);
    error->warning = true;
    (*cinfo->err->format_message)(cinfo, error->message);
}

bool mjpg_decodes_cleanly_data(const void *payload, size_t payload_size, std::string &message) {
    if(!mjpg_has_complete_jpeg_data(payload, payload_size)) {
        message = "missing jpeg soi/eoi marker";
        return false;
    }

    jpeg_decompress_struct cinfo{};
    JpegValidationError error{};
    cinfo.err = jpeg_std_error(&error.pub);
    error.pub.error_exit = jpeg_validation_error_exit;
    error.pub.emit_message = jpeg_validation_emit_message;

    if(setjmp(error.jump_buffer) != 0) {
        if(error.row_buffer) {
            std::free(error.row_buffer);
            error.row_buffer = nullptr;
        }
        jpeg_destroy_decompress(&cinfo);
        message = error.message[0] ? error.message : "jpeg validation failed";
        return false;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, static_cast<const unsigned char *>(payload), static_cast<unsigned long>(payload_size));
    if(jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        message = "jpeg header validation failed";
        return false;
    }
    cinfo.scale_num = 1;
    cinfo.scale_denom = 8;
    jpeg_start_decompress(&cinfo);

    const size_t row_size = static_cast<size_t>(cinfo.output_width) * static_cast<size_t>(cinfo.output_components);
    error.row_buffer = static_cast<JSAMPLE *>(std::malloc(row_size));
    if(!error.row_buffer) {
        jpeg_destroy_decompress(&cinfo);
        message = "jpeg validation failed: row allocation failed";
        return false;
    }
    while(cinfo.output_scanline < cinfo.output_height) {
        JSAMPROW row_ptr = error.row_buffer;
        if(jpeg_read_scanlines(&cinfo, &row_ptr, 1) != 1) {
            break;
        }
    }
    std::free(error.row_buffer);
    error.row_buffer = nullptr;
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);

    if(error.warning) {
        message = error.message[0] ? error.message : "jpeg decoder warning";
        return false;
    }
    return true;
}

bool mjpg_decodes_cleanly(const std::shared_ptr<ob::ColorFrame> &frame, std::string &message) {
    if(!frame) {
        message = "missing color frame";
        return false;
    }
    return mjpg_decodes_cleanly_data(frame->data(), frame->dataSize(), message);
}

void mark_corrupt_rgb_jpeg_frame(CameraRuntime &camera, Logger &logger, const std::shared_ptr<ob::ColorFrame> &color,
                                 std::chrono::steady_clock::time_point frame_now, const std::string &reason) {
    bool should_log = false;
    {
        std::lock_guard<std::mutex> lock(camera.mutex);
        camera.perf.rgb_corrupt_jpeg_frames++;
        camera.rgb_corrupt_jpeg++;
        camera.rgb_dropped++;
        camera.last_error = "corrupt rgb mjpeg frame dropped";
        if(frame_now >= camera.next_jpeg_warning) {
            camera.next_jpeg_warning = frame_now + std::chrono::seconds(1);
            should_log = true;
        }
    }
    if(should_log) {
        std::ostringstream oss;
        oss << "corrupt rgb mjpeg frame dropped camera_id=" << camera.config.camera_id << " frame_id=" << color->index()
            << " size=" << color->dataSize() << " reason=\"" << reason << "\"";
        logger.warn(oss.str());
    }
}

void mark_corrupt_rgb_jpeg_frame(CameraRuntime &camera, Logger &logger, uint64_t frame_id, size_t frame_size,
                                 std::chrono::steady_clock::time_point frame_now, const std::string &reason) {
    bool should_log = false;
    {
        std::lock_guard<std::mutex> lock(camera.mutex);
        camera.perf.rgb_corrupt_jpeg_frames++;
        camera.rgb_corrupt_jpeg++;
        camera.rgb_dropped++;
        camera.last_error = "corrupt rgb mjpeg frame dropped";
        if(frame_now >= camera.next_jpeg_warning) {
            camera.next_jpeg_warning = frame_now + std::chrono::seconds(1);
            should_log = true;
        }
    }
    if(should_log) {
        std::ostringstream oss;
        oss << "corrupt rgb mjpeg frame dropped camera_id=" << camera.config.camera_id << " frame_id=" << frame_id
            << " size=" << frame_size << " reason=\"" << reason << "\"";
        logger.warn(oss.str());
    }
}

void record_queue_overwrite(CameraRuntime &camera, StreamType stream_type) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    if(stream_type == StreamType::rgb) {
        camera.rgb_dropped++;
    }
    else if(stream_type == StreamType::depth_raw) {
        camera.depth_dropped++;
    }
}

void record_media_send_success(CameraRuntime &camera, StreamType stream_type, size_t packet_size, double send_ms) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    if(stream_type == StreamType::rgb) {
        camera.perf.rgb_send_ms += send_ms;
        camera.perf.rgb_sent_packets++;
        camera.perf.rgb_bytes += packet_size;
    }
    else if(stream_type == StreamType::rgb_preview) {
        camera.perf.rgb_preview_send_ms += send_ms;
        camera.perf.rgb_preview_sent_packets++;
        camera.perf.rgb_preview_bytes += packet_size;
    }
    else if(stream_type == StreamType::depth_raw) {
        camera.perf.depth_send_ms += send_ms;
        camera.perf.depth_sent_frames++;
        camera.perf.depth_bytes += packet_size;
    }
}

void arm_rgb_keyframe_guard(CameraRuntime &camera, Logger &logger, const std::string &reason) {
    bool should_log = false;
    {
        std::lock_guard<std::mutex> lock(camera.mutex);
        should_log = camera.rgb_transport_recovery.arm();
        camera.last_error = reason.empty() ? "rgb transport loss; waiting for next keyframe" : reason;
    }
    if(should_log) {
        const std::string log_reason = reason.empty() ? "rgb transport loss" : reason;
        logger.warn(log_reason + " camera_id=" + camera.config.camera_id + "; dropping non-IDR frames until next IDR");
    }
}

void request_rgb_keyframe(CameraRuntime &camera, Logger &logger, const std::string &reason,
                          uint64_t target_sender_system_us = 0, uint64_t target_global_us = 0) {
    uint64_t request_id = 0;
    uint64_t effective_target_sender_system_us = target_sender_system_us;
    uint64_t effective_target_global_us = target_global_us;
    const bool low_priority_immediate_request = reason.rfind("web_rgb_", 0) == 0;
    {
        std::lock_guard<std::mutex> lock(camera.mutex);
        const bool already_pending = camera.force_rgb_keyframe_observed < camera.force_rgb_keyframe_requests;
        request_id = ++camera.force_rgb_keyframe_requests;
        camera.force_rgb_keyframe_requested_at_us = now_us();
        if(!already_pending) {
            camera.force_rgb_keyframe_target_sender_system_us = target_sender_system_us;
            camera.force_rgb_keyframe_target_global_us = target_global_us;
            camera.force_rgb_keyframe_last_event_us = 0;
        }
        else {
            const uint64_t merged_target = merge_keyframe_target(
                camera.force_rgb_keyframe_target_sender_system_us,
                target_sender_system_us,
                low_priority_immediate_request);
            if(merged_target != camera.force_rgb_keyframe_target_sender_system_us) {
                camera.force_rgb_keyframe_target_sender_system_us = merged_target;
                camera.force_rgb_keyframe_target_global_us = merged_target == target_sender_system_us
                                                                    ? target_global_us
                                                                    : 0;
            }
        }
        effective_target_sender_system_us = camera.force_rgb_keyframe_target_sender_system_us;
        effective_target_global_us = camera.force_rgb_keyframe_target_global_us;
    }
    logger.info("rgb keyframe requested camera_id=" + camera.config.camera_id + " request_id=" + std::to_string(request_id)
                + (reason.empty() ? "" : " reason=" + reason)
                + (effective_target_global_us > 0
                       ? " target_global_us=" + std::to_string(effective_target_global_us)
                       : "")
                + (effective_target_sender_system_us > 0
                       ? " target_sender_system_us=" + std::to_string(effective_target_sender_system_us)
                       : ""));
}

bool consume_rgb_keyframe_request(CameraRuntime &camera, uint64_t frame_system_timestamp_us,
                                  uint64_t &request_id) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    if(camera.force_rgb_keyframe_observed >= camera.force_rgb_keyframe_requests) {
        return false;
    }
    const uint64_t target_us = camera.force_rgb_keyframe_target_sender_system_us;
    const uint64_t current_us = now_us();
    if(!scheduled_keyframe_due(target_us, frame_system_timestamp_us, current_us)) {
        return false;
    }
    constexpr uint64_t kForceKeyframeRetryIntervalUs = 50'000;
    if(camera.force_rgb_keyframe_last_event_us > 0
       && current_us - camera.force_rgb_keyframe_last_event_us < kForceKeyframeRetryIntervalUs) {
        return false;
    }
    camera.force_rgb_keyframe_applied = camera.force_rgb_keyframe_requests;
    request_id = camera.force_rgb_keyframe_applied;
    camera.force_rgb_keyframe_last_event_us = current_us;
    ++camera.force_rgb_keyframe_events;
    return true;
}

void report_forced_rgb_keyframe(CameraRuntime &camera, Logger &logger) {
    uint64_t request_id = 0;
    uint64_t requested_at_us = 0;
    {
        std::lock_guard<std::mutex> lock(camera.mutex);
        if(camera.force_rgb_keyframe_applied == 0
           || camera.force_rgb_keyframe_observed >= camera.force_rgb_keyframe_applied
           || camera.force_rgb_keyframe_last_event_us == 0) {
            return;
        }
        camera.force_rgb_keyframe_observed = camera.force_rgb_keyframe_applied;
        request_id = camera.force_rgb_keyframe_observed;
        requested_at_us = camera.force_rgb_keyframe_requested_at_us;
        if(camera.force_rgb_keyframe_observed >= camera.force_rgb_keyframe_requests) {
            camera.force_rgb_keyframe_target_sender_system_us = 0;
            camera.force_rgb_keyframe_target_global_us = 0;
            camera.force_rgb_keyframe_last_event_us = 0;
        }
    }
    const uint64_t observed_at_us = now_us();
    const uint64_t latency_us = requested_at_us != 0 && observed_at_us >= requested_at_us ? observed_at_us - requested_at_us : 0;
    logger.info("rgb forced keyframe observed camera_id=" + camera.config.camera_id + " request_id=" + std::to_string(request_id)
                + " latency_us=" + std::to_string(latency_us));
}

void maybe_request_rgb_recovery_keyframe(CameraRuntime &camera, Logger &logger,
                                         std::chrono::steady_clock::time_point now) {
    const auto since_epoch = now.time_since_epoch();
    const auto monotonic_now_us =
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(since_epoch).count());
    bool request_due = false;
    {
        std::lock_guard<std::mutex> lock(camera.mutex);
        request_due = camera.rgb_transport_recovery.keyframe_request_due(monotonic_now_us,
                                                                          kRgbRecoveryKeyframeRequestIntervalUs);
    }
    if(request_due) {
        request_rgb_keyframe(camera, logger, "media_transport_recovery");
    }
}

RgbTransportRecovery::SendDecision decide_rgb_keyframe_send(CameraRuntime &camera, bool is_keyframe,
                                                            std::chrono::steady_clock::time_point now, Logger &logger) {
    bool log_drop = false;
    bool request_keyframe = false;
    uint64_t dropped = 0;
    RgbTransportRecovery::SendDecision decision = RgbTransportRecovery::SendDecision::send;
    {
        std::lock_guard<std::mutex> lock(camera.mutex);
        decision = camera.rgb_transport_recovery.before_send(is_keyframe);
        if(decision == RgbTransportRecovery::SendDecision::drop) {
            camera.rgb_dropped++;
            dropped = camera.rgb_transport_recovery.dropped_frames();
            if(now >= camera.next_keyframe_guard_warning) {
                camera.next_keyframe_guard_warning = now + std::chrono::seconds(1);
                log_drop = true;
            }
        }
        if(camera.rgb_transport_recovery.waiting()) {
            const auto monotonic_now_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                                     now.time_since_epoch())
                                                                     .count());
            request_keyframe = camera.rgb_transport_recovery.keyframe_request_due(
                monotonic_now_us, kRgbRecoveryKeyframeRequestIntervalUs);
        }
    }
    if(request_keyframe) {
        request_rgb_keyframe(camera, logger, "media_transport_recovery");
    }
    if(log_drop) {
        logger.warn("rgb keyframe guard dropping non-IDR camera_id=" + camera.config.camera_id + " dropped=" + std::to_string(dropped));
    }
    return decision;
}

void complete_rgb_keyframe_recovery(CameraRuntime &camera, Logger &logger, bool is_keyframe) {
    std::optional<uint64_t> dropped;
    {
        std::lock_guard<std::mutex> lock(camera.mutex);
        dropped = camera.rgb_transport_recovery.complete_successful_send(is_keyframe);
    }
    if(dropped) {
        logger.info("rgb keyframe guard recovered camera_id=" + camera.config.camera_id
                    + " dropped=" + std::to_string(*dropped));
    }
}

void record_media_send_failure(CameraRuntime &camera, Logger &logger, StreamType stream_type, double send_ms,
                               std::chrono::steady_clock::time_point frame_now, const std::string &reason) {
    const std::string message = reason.empty() ? "unknown media transport error" : reason;
    bool should_log = false;
    {
        std::lock_guard<std::mutex> lock(camera.mutex);
        if(stream_type == StreamType::rgb) {
            camera.perf.rgb_send_ms += send_ms;
            camera.perf.rgb_send_failures++;
            camera.rgb_send_failures_total++;
        }
        else if(stream_type == StreamType::rgb_preview) {
            camera.perf.rgb_preview_send_ms += send_ms;
            camera.perf.rgb_preview_send_failures++;
            camera.rgb_preview_send_failures_total++;
        }
        else if(stream_type == StreamType::depth_raw) {
            camera.perf.depth_send_ms += send_ms;
            camera.perf.depth_send_failures++;
            camera.depth_send_failures_total++;
        }
        camera.last_error = message;
        if(frame_now >= camera.next_media_warning || message != camera.last_media_warning) {
            camera.next_media_warning = frame_now + std::chrono::seconds(2);
            camera.last_media_warning = message;
            should_log = true;
        }
    }
    if(should_log) {
        logger.warn("media send failed camera_id=" + camera.config.camera_id
                    + " stream=" + stream_type_name(stream_type) + " error=" + message);
    }
}

void record_rgb_input(CameraRuntime &camera, const std::shared_ptr<ob::ColorFrame> &color) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    camera.perf.rgb_input_frames++;
    camera.perf.rgb_input_bytes += color->dataSize();
    camera.perf.note_rgb_frame_id(color->index());
    camera.last_rgb_frame_at = std::chrono::steady_clock::now();
}

void record_rgb_input(CameraRuntime &camera, size_t payload_size, uint64_t frame_id) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    camera.perf.rgb_input_frames++;
    camera.perf.rgb_input_bytes += payload_size;
    camera.perf.note_rgb_frame_id(frame_id);
    camera.last_rgb_frame_at = std::chrono::steady_clock::now();
}

void record_depth_input(CameraRuntime &camera, const std::shared_ptr<ob::DepthFrame> &depth) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    camera.perf.depth_input_frames++;
    camera.perf.depth_input_bytes += depth->dataSize();
    camera.perf.note_depth_frame_id(depth->index());
    camera.last_depth_frame_at = std::chrono::steady_clock::now();
}

void record_wait_result(CameraRuntime &camera, double wait_ms, bool timeout) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    camera.perf.wait_calls++;
    camera.perf.wait_ms += wait_ms;
    if(timeout) {
        camera.perf.wait_timeouts++;
    }
}

void record_rgb_decode_ms(CameraRuntime &camera, double decode_ms) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    camera.perf.rgb_decode_ms += decode_ms;
}

void record_rgb_encode_ms(CameraRuntime &camera, double encode_ms) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    camera.perf.rgb_encode_ms += encode_ms;
}

void record_depth_compress_ms(CameraRuntime &camera, double compress_ms) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    camera.perf.depth_compress_ms += compress_ms;
}

void record_depth_preview_ms(CameraRuntime &camera, double preview_ms) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    camera.perf.depth_preview_ms += preview_ms;
}

void record_preview_ms(CameraRuntime &camera, double preview_ms) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    camera.perf.preview_ms += preview_ms;
}

void record_rgb_frame_done(CameraRuntime &camera) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    camera.rgb_frames++;
}

void record_depth_frame_done(CameraRuntime &camera) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    camera.depth_frames++;
}

std::chrono::steady_clock::duration frame_interval_for_fps(int fps) {
    if(fps <= 0) {
        return std::chrono::steady_clock::duration::zero();
    }
    return std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(1.0 / static_cast<double>(fps)));
}

bool depth_emit_due(CameraRuntime &camera, std::chrono::steady_clock::time_point now) {
    (void)camera;
    (void)now;
    // The camera profile already controls capture FPS. Software pacing here
    // dropped valid depth frames when scheduler jitter made a frame arrive a
    // little before the next nominal 30 FPS tick.
    return true;
}

bool web_rgb_preview_emit_due(const AppConfig &config, CameraRuntime &camera, std::chrono::steady_clock::time_point now) {
    if(!config.web_rgb_preview.enabled) {
        return false;
    }
    std::lock_guard<std::mutex> lock(camera.mutex);
    if(now < camera.web_rgb_preview_suppressed_until) {
        return false;
    }
    if(config.web_rgb_preview.on_demand && now > camera.web_rgb_preview_requested_until) {
        return false;
    }
    // Capture already paces frames. Reapplying the same nominal rate drops
    // frames whenever camera jitter puts the next frame just before the tick.
    if(camera.config.rgb_profile.fps > 0 && config.web_rgb_preview.fps >= camera.config.rgb_profile.fps) {
        camera.next_web_rgb_preview = now;
        return true;
    }
    const auto interval = frame_interval_for_fps(config.web_rgb_preview.fps);
    if(interval <= std::chrono::steady_clock::duration::zero()) {
        return true;
    }
    if(now < camera.next_web_rgb_preview) {
        return false;
    }
    camera.next_web_rgb_preview += interval;
    if(camera.next_web_rgb_preview <= now) {
        camera.next_web_rgb_preview = now + interval;
    }
    return true;
}

void set_latest_bgr(CameraRuntime &camera, const cv::Mat &bgr, uint64_t frame_id, uint64_t system_timestamp_us,
                    int rotation_degrees) {
    if(bgr.empty()) {
        return;
    }
    cv::Mat preview_bgr;
    if(rotation_degrees == 180) {
        cv::rotate(bgr, preview_bgr, cv::ROTATE_180);
    }
    else {
        preview_bgr = bgr.clone();
    }
    std::lock_guard<std::mutex> lock(camera.mutex);
    camera.latest_bgr = preview_bgr;
    camera.latest_rgb_frame_id = frame_id;
    camera.latest_rgb_system_timestamp_us = system_timestamp_us;
    camera.rgb_preview_queue.push_back(RgbPreviewFrame{preview_bgr, frame_id, system_timestamp_us});
    while(camera.rgb_preview_queue.size() > kMaxRgbPreviewQueueFrames) {
        camera.rgb_preview_queue.pop_front();
    }
}

void set_latest_depth_color(CameraRuntime &camera, const cv::Mat &depth_color) {
    if(depth_color.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(camera.mutex);
    camera.latest_depth_color = depth_color.clone();
}

bool camera_is_online(const CameraRuntime &camera) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    return camera.online;
}

bool camera_reconnect_enabled(const CameraRuntime &camera) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    return camera.reconnect_enabled;
}

bool camera_counts_against_hotplug_limit(const CameraRuntime &camera) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    return camera.online || camera.reconnect_enabled;
}

bool camera_matches_identity(const CameraRuntime &camera, const OrbbecDeviceIdentity &identity) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    if(camera.hotplug_dynamic && !camera.online && !camera.reconnect_enabled) {
        return false;
    }
    if(!camera.device_uid.empty() && camera.device_uid == identity.uid) {
        return true;
    }
    if(!camera.device_serial.empty()
       && (camera.device_serial == identity.serial || camera.device_serial == identity.paired_rgb_serial)) {
        return true;
    }
    return device_identity_matches_config(identity, camera.config);
}

void set_camera_reconnect_delay(CameraRuntime &camera, std::chrono::steady_clock::time_point next_reconnect) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    camera.next_reconnect = next_reconnect;
}

std::shared_ptr<ob::VideoStreamProfile> camera_color_profile(const CameraRuntime &camera) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    return camera.color_profile;
}

bool camera_announced(const CameraRuntime &camera) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    return camera.announced;
}

bool camera_online(const CameraRuntime &camera) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    return camera.online;
}

void set_camera_announced(CameraRuntime &camera, bool announced) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    camera.announced = announced;
}

GstH264InputFormat encoder_input_format_for(CameraRuntime &camera) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    return camera.encoder_input_format;
}

GstH264InputFormat web_preview_encoder_input_format_for(CameraRuntime &camera) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    return camera.web_preview_encoder_input_format;
}

void set_camera_last_error(CameraRuntime &camera, const std::string &error) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    camera.last_error = error;
}

void set_depth_scale_if_empty(CameraRuntime &camera, float depth_scale) {
    std::lock_guard<std::mutex> lock(camera.mutex);
    if(camera.depth_scale == 0.0f) {
        camera.depth_scale = depth_scale;
    }
}

cv::Mat color_to_bgr(const std::shared_ptr<ob::ColorFrame> &frame) {
    const auto format = frame->format();
    if(format == OB_FORMAT_MJPG) {
        return mjpg_to_bgr(frame, cv::IMREAD_COLOR);
    }
    if(format == OB_FORMAT_RGB) {
        cv::Mat rgb(frame->height(), frame->width(), CV_8UC3, frame->data());
        cv::Mat bgr;
        cv::cvtColor(rgb, bgr, cv::COLOR_RGB2BGR);
        return bgr;
    }
    if(format == OB_FORMAT_BGR) {
        return cv::Mat(frame->height(), frame->width(), CV_8UC3, frame->data()).clone();
    }
    if(format == OB_FORMAT_YUYV) {
        cv::Mat yuyv(frame->height(), frame->width(), CV_8UC2, frame->data());
        cv::Mat bgr;
        cv::cvtColor(yuyv, bgr, cv::COLOR_YUV2BGR_YUYV);
        return bgr;
    }
    if(format == OB_FORMAT_UYVY) {
        cv::Mat uyvy(frame->height(), frame->width(), CV_8UC2, frame->data());
        cv::Mat bgr;
        cv::cvtColor(uyvy, bgr, cv::COLOR_YUV2BGR_UYVY);
        return bgr;
    }
    return {};
}

cv::Mat color_to_preview_bgr(const std::shared_ptr<ob::ColorFrame> &frame) {
    if(frame->format() == OB_FORMAT_MJPG) {
        return mjpg_to_bgr(frame, cv::IMREAD_REDUCED_COLOR_2);
    }
    return color_to_bgr(frame);
}

std::optional<ExposureMeteringSample> meter_mjpg_luma(const std::shared_ptr<ob::ColorFrame> &frame,
                                                      const AdaptiveExposureConfig &config,
                                                      std::string &error) {
    if(!frame || frame->format() != OB_FORMAT_MJPG || !frame->data() || frame->dataSize() == 0
       || frame->dataSize() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        error = "adaptive metering requires a valid MJPEG frame";
        return std::nullopt;
    }

    cv::Mat gray;
    try {
        cv::Mat encoded(1, static_cast<int>(frame->dataSize()), CV_8UC1, frame->data());
        gray = cv::imdecode(encoded, cv::IMREAD_REDUCED_GRAYSCALE_8);
    }
    catch(const cv::Exception &e) {
        error = e.what();
        return std::nullopt;
    }
    if(gray.empty() || gray.type() != CV_8UC1) {
        error = "adaptive metering JPEG decode returned no grayscale pixels";
        return std::nullopt;
    }

    const int margin_x = gray.cols * config.roi_margin_percent / 100;
    const int margin_y = gray.rows * config.roi_margin_percent / 100;
    const int roi_x0 = std::clamp(margin_x, 0, gray.cols - 1);
    const int roi_y0 = std::clamp(margin_y, 0, gray.rows - 1);
    const int roi_x1 = std::clamp(gray.cols - margin_x, roi_x0 + 1, gray.cols);
    const int roi_y1 = std::clamp(gray.rows - margin_y, roi_y0 + 1, gray.rows);

    uint64_t histogram[256] = {};
    uint64_t pixel_count = 0;
    uint64_t highlight_count = 0;
    for(int y = roi_y0; y < roi_y1; ++y) {
        const auto *row = gray.ptr<uint8_t>(y);
        for(int x = roi_x0; x < roi_x1; ++x) {
            const uint8_t value = row[x];
            ++histogram[value];
            ++pixel_count;
            if(value >= config.highlight_luma) {
                ++highlight_count;
            }
        }
    }
    if(pixel_count == 0) {
        error = "adaptive metering ROI is empty";
        return std::nullopt;
    }

    auto percentile = [&](int numerator, int denominator) {
        const uint64_t threshold = (pixel_count * static_cast<uint64_t>(numerator) + denominator - 1)
                                   / static_cast<uint64_t>(denominator);
        uint64_t cumulative = 0;
        for(int value = 0; value < 256; ++value) {
            cumulative += histogram[value];
            if(cumulative >= threshold) {
                return value;
            }
        }
        return 255;
    };

    ExposureMeteringSample sample;
    sample.p50_luma = percentile(50, 100);
    sample.p95_luma = percentile(95, 100);
    sample.p99_luma = percentile(99, 100);
    sample.highlight_fraction = static_cast<double>(highlight_count) / static_cast<double>(pixel_count);
    return sample;
}

bool apply_adaptive_exposure_decision(CameraRuntime &camera, const ExposureControlDecision &decision,
                                      Logger &logger, std::string &error) {
    if(!decision.apply || !camera.adaptive_exposure_controller) {
        return true;
    }

    std::shared_ptr<ob::Device> device;
    {
        std::lock_guard<std::mutex> lock(camera.mutex);
        device = camera.device;
    }
    if(!device) {
        error = "camera device is unavailable";
        return false;
    }

    auto set_and_verify = [&](OBPropertyID property_id, int desired, const char *name) {
        if(!device->isPropertySupported(property_id, OB_PERMISSION_WRITE)) {
            error = std::string(name) + " is not writable";
            return false;
        }
        device->setIntProperty(property_id, desired);
        if(device->isPropertySupported(property_id, OB_PERMISSION_READ)) {
            const int readback = device->getIntProperty(property_id);
            if(readback != desired) {
                error = std::string(name) + " readback=" + std::to_string(readback)
                        + " expected=" + std::to_string(desired);
                return false;
            }
        }
        return true;
    };

    try {
        const int old_exposure = camera.adaptive_exposure_controller->exposure();
        const int old_gain = camera.adaptive_exposure_controller->gain();
        if(decision.exposure != old_exposure
           && !set_and_verify(OB_PROP_COLOR_EXPOSURE_INT, decision.exposure, "exposure")) {
            return false;
        }
        if(decision.gain != old_gain && !set_and_verify(OB_PROP_COLOR_GAIN_INT, decision.gain, "gain")) {
            if(decision.exposure != old_exposure) {
                try {
                    device->setIntProperty(OB_PROP_COLOR_EXPOSURE_INT, old_exposure);
                }
                catch(...) {
                }
            }
            return false;
        }
        camera.adaptive_exposure_controller->commit(decision);
        logger.info("adaptive exposure adjusted camera_id=" + camera.config.camera_id
                    + " reason=" + decision.reason
                    + " exposure=" + std::to_string(decision.exposure)
                    + " gain=" + std::to_string(decision.gain));
        return true;
    }
    catch(const std::exception &e) {
        error = e.what();
        return false;
    }
}

void maybe_update_adaptive_exposure(CameraRuntime &camera, const std::shared_ptr<ob::ColorFrame> &color,
                                    std::chrono::steady_clock::time_point now, Logger &logger) {
    if(!camera.adaptive_exposure_controller) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(camera.mutex);
        if(camera.adaptive_exposure_discard_frames_remaining > 0) {
            --camera.adaptive_exposure_discard_frames_remaining;
            camera.live.adaptive_last_reason = "discarding_unsettled_frames";
            return;
        }
    }
    {
        std::lock_guard<std::mutex> lock(camera.mutex);
        if(now < camera.next_adaptive_exposure_sample) {
            return;
        }
        camera.next_adaptive_exposure_sample =
            now + std::chrono::milliseconds(camera.config.adaptive_exposure.interval_ms);
    }

    bool waiting_for_metadata = false;
    bool metadata_wait_timed_out = false;
    {
        std::lock_guard<std::mutex> lock(camera.mutex);
        if(camera.adaptive_exposure_waiting_for_metadata) {
            const int expected_exposure = camera.adaptive_exposure_controller->exposure();
            const int expected_gain = camera.adaptive_exposure_controller->gain();
            const bool metadata_available = camera.live.color_exposure >= 0 && camera.live.color_gain >= 0;
            const bool metadata_matches = !metadata_available
                                          || (camera.live.color_exposure == expected_exposure
                                              && camera.live.color_gain == expected_gain);
            if(metadata_matches) {
                camera.adaptive_exposure_waiting_for_metadata = false;
            }
            else if(now < camera.adaptive_exposure_metadata_deadline) {
                camera.live.adaptive_last_reason = "awaiting_applied_controls";
                waiting_for_metadata = true;
            }
            else {
                camera.adaptive_exposure_waiting_for_metadata = false;
                metadata_wait_timed_out = true;
            }
        }
    }
    if(waiting_for_metadata) {
        return;
    }
    if(metadata_wait_timed_out) {
        logger.warn("adaptive exposure metadata wait timed out camera_id=" + camera.config.camera_id);
    }

    std::string error;
    const auto sample = meter_mjpg_luma(color, camera.config.adaptive_exposure, error);
    if(!sample) {
        bool should_log = false;
        {
            std::lock_guard<std::mutex> lock(camera.mutex);
            ++camera.live.adaptive_failures;
            camera.live.adaptive_last_reason = "metering_failed";
            if(now >= camera.next_adaptive_exposure_warning) {
                camera.next_adaptive_exposure_warning = now + std::chrono::seconds(10);
                should_log = true;
            }
        }
        if(should_log) {
            logger.warn("adaptive exposure metering failed camera_id=" + camera.config.camera_id + " error=" + error);
        }
        return;
    }

    double evaluation_dt_seconds = static_cast<double>(camera.config.adaptive_exposure.interval_ms) / 1000.0;
    {
        std::lock_guard<std::mutex> lock(camera.mutex);
        if(camera.adaptive_exposure_last_evaluation != std::chrono::steady_clock::time_point::min()) {
            evaluation_dt_seconds =
                std::chrono::duration<double>(now - camera.adaptive_exposure_last_evaluation).count();
        }
        camera.adaptive_exposure_last_evaluation = now;
    }
    const auto decision = camera.adaptive_exposure_controller->evaluate(*sample, evaluation_dt_seconds);
    const auto control_sample = camera.adaptive_exposure_controller->metering_ready()
                                    ? camera.adaptive_exposure_controller->metering_sample()
                                    : *sample;
    const bool applied = apply_adaptive_exposure_decision(camera, decision, logger, error);
    bool should_log_failure = false;
    {
        std::lock_guard<std::mutex> lock(camera.mutex);
        camera.live.adaptive_luma_p50 = control_sample.p50_luma;
        camera.live.adaptive_luma_p95 = control_sample.p95_luma;
        camera.live.adaptive_luma_p99 = control_sample.p99_luma;
        camera.live.adaptive_highlight_fraction = control_sample.highlight_fraction;
        camera.live.adaptive_requested_exposure = camera.adaptive_exposure_controller->exposure();
        camera.live.adaptive_requested_gain = camera.adaptive_exposure_controller->gain();
        camera.live.adaptive_samples = camera.adaptive_exposure_controller->sample_count();
        camera.live.adaptive_adjustments = camera.adaptive_exposure_controller->adjustment_count();
        camera.live.adaptive_last_reason = applied ? decision.reason : "apply_failed";
        if(!applied) {
            ++camera.live.adaptive_failures;
            if(now >= camera.next_adaptive_exposure_warning) {
                camera.next_adaptive_exposure_warning = now + std::chrono::seconds(10);
                should_log_failure = true;
            }
        }
        else if(decision.apply) {
            camera.next_adaptive_exposure_sample =
                now + std::chrono::milliseconds(camera.config.adaptive_exposure.settle_ms);
            camera.adaptive_exposure_discard_frames_remaining =
                camera.config.adaptive_exposure.discard_frames_after_adjustment;
            camera.adaptive_exposure_waiting_for_metadata = true;
            camera.adaptive_exposure_metadata_deadline =
                now + std::chrono::milliseconds(std::max(2000, camera.config.adaptive_exposure.settle_ms * 4));
        }
        else if(decision.reason != "dark_hysteresis" && decision.reason != "metering_warmup") {
            camera.next_adaptive_exposure_sample =
                now + std::chrono::milliseconds(camera.config.adaptive_exposure.stable_interval_ms);
        }
    }
    if(should_log_failure) {
        logger.warn("adaptive exposure apply failed camera_id=" + camera.config.camera_id + " error=" + error);
    }
}

void apply_software_rgb_rotation(cv::Mat &bgr, const CameraConfig &config) {
    if(bgr.empty() || software_rgb_rotation_degrees(config) == 0) {
        return;
    }
    cv::Mat rotated;
    cv::rotate(bgr, rotated, cv::ROTATE_180);
    bgr = std::move(rotated);
}

cv::Mat depth_to_color(const std::shared_ptr<ob::DepthFrame> &frame) {
    cv::Mat depth(frame->height(), frame->width(), CV_16UC1, frame->data());
    cv::Mat depth8;
    depth8.create(depth.rows, depth.cols, CV_8UC1);
    const auto denom = static_cast<float>(kDepthPreviewMaxMm - kDepthPreviewMinMm);
    for(int y = 0; y < depth.rows; ++y) {
        const auto *src = depth.ptr<uint16_t>(y);
        auto *dst = depth8.ptr<uint8_t>(y);
        for(int x = 0; x < depth.cols; ++x) {
            const uint16_t value = src[x];
            if(value == 0 || denom <= 0.0f) {
                dst[x] = 0;
                continue;
            }
            const auto clamped = static_cast<float>(std::clamp<uint16_t>(value, kDepthPreviewMinMm, kDepthPreviewMaxMm));
            dst[x] = static_cast<uint8_t>((clamped - static_cast<float>(kDepthPreviewMinMm)) * 255.0f / denom + 0.5f);
        }
    }
    cv::Mat color;
    cv::applyColorMap(depth8, color, cv::COLORMAP_JET);
    color.setTo(cv::Scalar(24, 16, 12), depth == 0);
    return color;
}

