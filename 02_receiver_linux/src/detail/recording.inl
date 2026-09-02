int run_shell_command(const std::string &command) {
    int error_code = 0;
    const pid_t pid = spawn_shell_process(command, -1, -1, -1, error_code);
    if(pid < 0) {
        errno = error_code;
        return -1;
    }
    return wait_child(pid);
}

std::optional<std::string> run_shell_capture(const std::string &command) {
    int output_pipe[2]{-1, -1};
    if(pipe2(output_pipe, O_CLOEXEC) != 0) {
        return std::nullopt;
    }
    ScopeExit close_pipe([&] {
        if(output_pipe[0] >= 0) {
            close(output_pipe[0]);
        }
        if(output_pipe[1] >= 0) {
            close(output_pipe[1]);
        }
    });
    const int dev_null = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if(dev_null < 0) {
        return std::nullopt;
    }
    ScopeExit close_dev_null([&] { close(dev_null); });
    int error_code = 0;
    const pid_t pid = spawn_shell_process(command, -1, output_pipe[1], dev_null, error_code);
    if(pid < 0) {
        return std::nullopt;
    }
    close(output_pipe[1]);
    output_pipe[1] = -1;
    std::string output;
    char buffer[512];
    for(;;) {
        const ssize_t count = read(output_pipe[0], buffer, sizeof(buffer));
        if(count > 0) {
            output.append(buffer, static_cast<size_t>(count));
            continue;
        }
        if(count < 0 && errno == EINTR) {
            continue;
        }
        if(count < 0) {
            (void)wait_child(pid);
            return std::nullopt;
        }
        break;
    }
    return wait_child(pid) == 0 ? std::optional<std::string>(std::move(output)) : std::nullopt;
}

class FfmpegPipe {
public:
    FfmpegPipe() = default;
    FfmpegPipe(const FfmpegPipe &) = delete;
    FfmpegPipe &operator=(const FfmpegPipe &) = delete;

    ~FfmpegPipe() {
        close();
    }

    bool open(const std::string &command, Logger &logger) {
        close();
        int input_pipe[2]{-1, -1};
        if(pipe2(input_pipe, O_CLOEXEC) != 0) {
            logger.error("failed to create ffmpeg input pipe: " + std::string(std::strerror(errno)));
            return false;
        }
        int error_code = 0;
        child_pid_ = spawn_shell_process(command, input_pipe[0], -1, -1, error_code);
        ::close(input_pipe[0]);
        if(child_pid_ < 0) {
            ::close(input_pipe[1]);
            logger.error("failed to start ffmpeg pipe: " + command + ": " + std::strerror(error_code));
            return false;
        }
        pipe_ = fdopen(input_pipe[1], "w");
        if(!pipe_) {
            ::close(input_pipe[1]);
            (void)wait_child(child_pid_);
            child_pid_ = -1;
            logger.error("failed to open ffmpeg input stream: " + std::string(std::strerror(errno)));
            return false;
        }
        if(setvbuf(pipe_, nullptr, _IONBF, 0) != 0) {
            logger.error("failed to configure unbuffered ffmpeg pipe: " + std::string(std::strerror(errno)));
            close();
            return false;
        }
        return true;
    }

    bool write(const uint8_t *data, size_t size, Logger &logger) {
        if(!pipe_) {
            return false;
        }
        if(size == 0) {
            return true;
        }
        const size_t written = fwrite(data, 1, size, pipe_);
        if(written != size) {
            logger.warn("ffmpeg pipe write failed");
            close();
            return false;
        }
        return true;
    }

    int close() {
        if(!pipe_ && child_pid_ < 0) {
            return 0;
        }
        if(pipe_) {
            fclose(pipe_);
            pipe_ = nullptr;
        }
        const pid_t pid = child_pid_;
        child_pid_ = -1;
        return pid >= 0 ? wait_child(pid) : 0;
    }

    bool active() const {
        return pipe_ != nullptr;
    }

private:
    FILE *pipe_ = nullptr;
    pid_t child_pid_ = -1;
};

std::string process_status_text(int status) {
    if(status == 0) {
        return "exit=0";
    }
    if(status == -1) {
        return std::string("waitpid failed: ") + std::strerror(errno);
    }
    if(WIFEXITED(status)) {
        return "exit=" + std::to_string(WEXITSTATUS(status));
    }
    if(WIFSIGNALED(status)) {
        return "signal=" + std::to_string(WTERMSIG(status));
    }
    return "status=" + std::to_string(status);
}

struct FrameInfo {
    bool valid = false;
    uint64_t frame_id = 0;
    uint64_t timestamp_us = 0;
    uint64_t system_timestamp_us = 0;
    uint64_t pair_id = 0;
    int32_t exposure_us = -1;
    int32_t gain = -1;
    int32_t auto_exposure = -1;
    int32_t actual_fps = -1;
};

struct PendingRgbPacketInfo {
    MediaPacket packet;
    uint64_t local_time_us = 0;
    size_t payload_size = 0;
    bool has_vcl = false;
};

size_t record_packet_queue_bytes(const MediaPacket &packet) {
    return sizeof(MediaPacket) + packet.sender_id.size() + packet.camera_id.size() + packet.codec_or_compression.size() + packet.payload.size();
}

struct RecordingWindow {
    uint64_t session_id = 0;
    uint64_t start_global_us = 0;
    uint64_t end_global_us = 0;
};

struct RecordingSegmentTimeline {
    uint64_t index = 0;
    uint64_t start_global_us = 0;
    uint64_t end_global_us = 0;
};

RecordingSegmentTimeline recording_segment_timeline(const RecordingWindow &window,
                                                     int segment_seconds,
                                                     uint64_t reference_global_us) {
    RecordingSegmentTimeline timeline;
    if(window.start_global_us == 0 || segment_seconds <= 0) {
        timeline.start_global_us = reference_global_us;
        return timeline;
    }
    const uint64_t duration_us = static_cast<uint64_t>(segment_seconds) * 1'000'000ull;
    if(reference_global_us > window.start_global_us) {
        timeline.index = (reference_global_us - window.start_global_us) / duration_us;
    }
    const uint64_t max_index = (std::numeric_limits<uint64_t>::max() - window.start_global_us) / duration_us;
    timeline.index = std::min(timeline.index, max_index);
    timeline.start_global_us = window.start_global_us + timeline.index * duration_us;
    timeline.end_global_us = timeline.start_global_us <= std::numeric_limits<uint64_t>::max() - duration_us
                                 ? timeline.start_global_us + duration_us
                                 : std::numeric_limits<uint64_t>::max();
    return timeline;
}

struct RecordJob {
    std::shared_ptr<const MediaPacket> packet;
    std::string sender_id;
    std::string camera_id;
    std::string camera_name;
    std::string storage_key;
    std::string file_prefix;
    std::string announce_json;
    RecordingWindow recording_window;
    uint64_t record_generation = 0;
    uint64_t media_session_id = 0;
    std::string media_ingress_key;
    size_t queue_bytes = 0;
    uint64_t enqueue_us = 0;
};

struct StreamRecordStats {
    uint64_t frames = 0;
    uint64_t first_local_us = 0;
    uint64_t last_local_us = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    std::string codec_or_compression;

    void add(const MediaPacket &packet, uint64_t local_us) {
        if(frames == 0) {
            first_local_us = local_us;
            width = packet.width;
            height = packet.height;
            codec_or_compression = packet.codec_or_compression;
        }
        last_local_us = local_us;
        if(width == 0) {
            width = packet.width;
        }
        if(height == 0) {
            height = packet.height;
        }
        if(codec_or_compression.empty()) {
            codec_or_compression = packet.codec_or_compression;
        }
        ++frames;
    }

    double actual_fps() const {
        if(frames >= 2 && last_local_us > first_local_us) {
            const double seconds = static_cast<double>(last_local_us - first_local_us) / 1'000'000.0;
            if(seconds > 0.0) {
                return static_cast<double>(frames - 1) / seconds;
            }
        }
        return 0.0;
    }

    void reset() {
        frames = 0;
        first_local_us = 0;
        last_local_us = 0;
        width = 0;
        height = 0;
        codec_or_compression.clear();
    }
};

std::string format_fps(double fps) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(3) << fps;
    std::string value = out.str();
    while(value.size() > 1 && value.back() == '0') {
        value.pop_back();
    }
    if(!value.empty() && value.back() == '.') {
        value.pop_back();
    }
    return value;
}

void write_optional_us(std::ostream &out, uint64_t value) {
    if(value > 0) {
        out << value;
    }
}

void write_optional_delta_us(std::ostream &out, uint64_t newer_us, uint64_t older_us) {
    if(newer_us > 0 && older_us > 0) {
        out << (newer_us >= older_us ? static_cast<int64_t>(newer_us - older_us)
                                      : -static_cast<int64_t>(older_us - newer_us));
    }
}

struct FpsProbe {
    uint64_t first_us = 0;
    uint64_t last_us = 0;
    uint32_t frames = 0;

    void add(uint64_t local_us) {
        constexpr uint64_t kMaxPlausibleFrameGapUs = 500'000;
        if(local_us == 0) {
            return;
        }
        if(last_us > 0 && (local_us <= last_us || local_us - last_us > kMaxPlausibleFrameGapUs)) {
            first_us = local_us;
            last_us = local_us;
            frames = 1;
            return;
        }
        if(first_us == 0) {
            first_us = local_us;
        }
        last_us = local_us;
        ++frames;
    }

    bool ready(uint64_t local_us) const {
        return frames >= kRecordFpsProbeFrames || (first_us > 0 && local_us > first_us && local_us - first_us >= kRecordFpsProbeMaxWaitUs);
    }

    double estimate(double fallback) const {
        if(frames >= 2 && last_us > first_us) {
            const double seconds = static_cast<double>(last_us - first_us) / 1'000'000.0;
            if(seconds > 0.0) {
                return std::clamp((static_cast<double>(frames - 1) / seconds), kMinRecordFps, kMaxRecordFps);
            }
        }
        return std::clamp(fallback, kMinRecordFps, kMaxRecordFps);
    }

    void reset() {
        first_us = 0;
        last_us = 0;
        frames = 0;
    }
};

struct CameraState;

class SegmentWriter {
public:
    SegmentWriter() = default;
    SegmentWriter(const SegmentWriter &) = delete;
    SegmentWriter &operator=(const SegmentWriter &) = delete;

    bool active() const {
        return active_;
    }

    const std::string &directory() const {
        return directory_;
    }

    uint64_t start_us() const {
        return start_us_;
    }

    uint64_t segment_index() const {
        return segment_timeline_.index;
    }

    uint64_t segment_window_start_global_us() const {
        return segment_timeline_.start_global_us;
    }

    uint64_t segment_window_end_global_us() const {
        return segment_timeline_.end_global_us;
    }

    void mark_end_us(uint64_t end_us) {
        if(end_us_ == 0) {
            end_us_ = end_us;
        }
    }

    void mark_recording_window_end_global_us(uint64_t end_global_us) {
        if(recording_window_.end_global_us == 0) {
            recording_window_.end_global_us = end_global_us;
        }
    }

    void start(const Config &cfg, const std::string &sender_id, const std::string &camera_id, const std::string &camera_name,
               const std::string &storage_key, const std::string &file_prefix, const std::string &announce_json,
               const RecordingWindow &recording_window, uint64_t segment_reference_global_us, Logger &logger) {
        close(cfg, sender_id, camera_id, announce_json, logger);
        ScopeExit rollback_guard([this] { reset_after_close(); });
        start_us_ = now_us();
        end_us_ = 0;
        recording_window_ = recording_window;
        segment_timeline_ = recording_segment_timeline(recording_window, cfg.segment_seconds,
                                                       segment_reference_global_us);
        camera_name_ = camera_name;
        storage_key_ = storage_key.empty() ? camera_key(sender_id, camera_id) : storage_key;
        file_prefix_ = file_prefix;
        const int announced_rgb_fps = json_int_in_object(announce_json, "rgb_profile", "fps").value_or(0);
        const int announced_depth_fps = json_int_in_object(announce_json, "depth_profile", "fps").value_or(0);
        rgb_nominal_fps_ = announced_rgb_fps >= kMinRecordFps && announced_rgb_fps <= kMaxRecordFps
                               ? static_cast<double>(announced_rgb_fps)
                               : 0.0;
        depth_nominal_fps_ = announced_depth_fps >= kMinRecordFps && announced_depth_fps <= kMaxRecordFps
                                 ? static_cast<double>(announced_depth_fps)
                                 : 0.0;
        rgb_expected_ = rgb_nominal_fps_ > 0.0
                        && json_int_in_object(announce_json, "rgb_profile", "width").value_or(0) > 0
                        && json_int_in_object(announce_json, "rgb_profile", "height").value_or(0) > 0;
        depth_expected_ = depth_nominal_fps_ > 0.0
                          && json_int_in_object(announce_json, "depth_profile", "width").value_or(0) > 0
                          && json_int_in_object(announce_json, "depth_profile", "height").value_or(0) > 0;
        rgb_h264_full_range_ = rgb_h264_full_range_for_camera(cfg, sender_id, camera_id);
        const auto recording_root = recording_write_root(cfg);
        const auto publish_root = std::filesystem::path(cfg.nas_root);
        std::error_code root_ec;
        std::filesystem::create_directories(recording_root, root_ec);
        if(root_ec) {
            throw std::runtime_error("cannot create recording root: " + recording_root.string() + ": " + root_ec.message());
        }
        std::filesystem::create_directories(publish_root, root_ec);
        if(root_ec) {
            throw std::runtime_error("cannot create recording publish root: " + publish_root.string()
                                     + ": " + root_ec.message());
        }
        if(!cfg.recording_staging.enabled && !paths_share_device(recording_root, publish_root)) {
            throw std::runtime_error("direct NAS hidden and publish roots must share one filesystem");
        }
        const auto space = std::filesystem::space(recording_root, root_ec);
        if(root_ec || space.available < cfg.min_free_disk_bytes) {
            throw std::runtime_error("insufficient free space under recording root: " + recording_root.string());
        }
        const uint64_t directory_time_us = segment_timeline_.start_global_us > 0
                                               ? segment_timeline_.start_global_us
                                               : start_us_;
        const auto relative_base = std::filesystem::path(storage_key_) / date_dir_from_us(directory_time_us)
                                   / time_dir_from_us(directory_time_us);
        const auto directory_base = recording_root / relative_base;
        const auto publish_base = publish_root / relative_base;
        auto directory = directory_base;
        auto publish_directory = publish_base;
        std::error_code ec;
        for(unsigned suffix = 1;; ++suffix) {
            ec.clear();
            const bool hidden_exists = std::filesystem::exists(directory, ec);
            if(ec) {
                throw std::runtime_error("cannot inspect recording directory: " + directory.string() + ": " + ec.message());
            }
            bool published_exists = false;
            if(!cfg.recording_staging.enabled) {
                published_exists = std::filesystem::exists(publish_directory, ec);
                if(ec) {
                    throw std::runtime_error("cannot inspect recording publish directory: "
                                             + publish_directory.string() + ": " + ec.message());
                }
            }
            if(!hidden_exists && !published_exists) {
                break;
            }
            std::ostringstream name;
            name << directory_base.filename().string() << '_' << std::setw(3) << std::setfill('0') << suffix;
            directory = directory_base.parent_path() / name.str();
            publish_directory = publish_base.parent_path() / name.str();
        }
        if(!std::filesystem::create_directories(directory, ec) || ec) {
            throw std::runtime_error("cannot create recording directory: " + directory.string() + ": " + ec.message());
        }
        directory_ = directory.string();
        recording_root_ = recording_root.lexically_normal().string();
        relative_directory_ = directory.lexically_relative(recording_root).generic_string();
        if(relative_directory_.empty() || relative_directory_ == ".." || relative_directory_.rfind("../", 0) == 0) {
            throw std::runtime_error("recording directory escaped configured root: " + directory_);
        }

        // Publish frames.csv only after media finalization and RGB index merging complete.
        // Consumers must never mistake the live packet journal for the final MP4 frame map.
        frames_csv_.open(file_path("frames.csv.inprogress"), std::ios::out | std::ios::trunc);
        if(!frames_csv_) {
            throw std::runtime_error("cannot open frames.csv staging file: " + file_path("frames.csv.inprogress").string());
        }
        frames_csv_ << "local_time_us,stream_type,rgb_frame_id,rgb_timestamp_us,depth_frame_id,depth_timestamp_us,pair_id,pair_delta_ms,width,height,payload_size,"
                       "packet_system_timestamp_us,rgb_system_timestamp_us,depth_system_timestamp_us,frame_id,timestamp_us,frame_system_timestamp_us,"
                       "rgb_exposure_us,rgb_gain,rgb_auto_exposure,rgb_actual_fps,rgb_frame_interval_us,codec_or_compression,"
                       "sender_capture_host_timestamp_us,sender_timing_bound_timestamp_us,sender_encode_start_timestamp_us,"
                       "sender_encode_done_timestamp_us,sender_packet_queued_timestamp_us,receiver_minus_frame_system_us,"
                       "sender_capture_to_timing_bound_us,sender_timing_bound_to_encode_start_us,sender_encode_duration_us,"
                       "sender_encode_done_to_packet_queued_us,sender_packet_queued_to_receiver_us,"
                       "sender_id,camera_id,sender_timestamp_us,sender_system_timestamp_us,receiver_receive_timestamp_us,"
                       "clock_sync_valid,sender_offset_us,sender_delay_us,sender_drift_ppm,global_timestamp_us,"
                       "rgb_depth_pair_valid,pair_delta_us,pair_delta_source,pair_id_valid\n";
        rgb_recorded_frames_csv_.open(file_path("rgb_recorded_frames.csv"), std::ios::out | std::ios::trunc);
        if(!rgb_recorded_frames_csv_) {
            throw std::runtime_error("cannot open RGB frame index CSV: " + file_path("rgb_recorded_frames.csv").string());
        }
        rgb_recorded_frames_csv_
            << "video_frame_index,local_time_us,frame_id,timestamp_us,frame_system_timestamp_us,width,height,payload_size,"
               "packet_system_timestamp_us,rgb_exposure_us,rgb_gain,rgb_auto_exposure,rgb_actual_fps,codec_or_compression,"
               "sender_capture_host_timestamp_us,sender_timing_bound_timestamp_us,sender_encode_start_timestamp_us,"
               "sender_encode_done_timestamp_us,sender_packet_queued_timestamp_us,receiver_minus_frame_system_us,"
               "sender_capture_to_timing_bound_us,sender_timing_bound_to_encode_start_us,sender_encode_duration_us,"
               "sender_encode_done_to_packet_queued_us,sender_packet_queued_to_receiver_us,"
               "sender_id,camera_id,sender_timestamp_us,sender_system_timestamp_us,receiver_receive_timestamp_us,"
               "clock_sync_valid,sender_offset_us,sender_delay_us,sender_drift_ppm,global_timestamp_us\n";

        if(cfg.write_debug_h264) {
            rgb_debug_path_ = file_path("rgb_debug.h264");
            rgb_debug_.open(rgb_debug_path_, std::ios::binary | std::ios::out | std::ios::trunc);
            if(!rgb_debug_) {
                throw std::runtime_error("cannot open RGB H264 debug file: " + rgb_debug_path_.string());
            }
        }
        if(cfg.write_debug_depth_raw) {
            depth_debug_.open(file_path("depth_debug.raw"), std::ios::binary | std::ios::out | std::ios::trunc);
            if(!depth_debug_) {
                throw std::runtime_error("cannot open depth debug file: " + file_path("depth_debug.raw").string());
            }
        }

        write_meta(cfg, sender_id, camera_id, announce_json, false);
        try {
            write_task_audio_manifest(cfg, sender_id, camera_id);
        }
        catch(const std::exception &e) {
            logger.warn(std::string("task audio manifest unavailable; video recording continues: ") + e.what());
        }
        active_ = true;
        rgb_pipe_failed_ = false;
        depth_pipe_failed_ = false;
        depth_part_index_ = 0;
        depth_part_paths_.clear();
        csv_rows_since_flush_ = 0;
        storage_check_packets_ = 0;
        storage_failed_ = false;
        rollback_guard.release();
        logger.info("recording segment started: " + directory_
                    + " global_segment_index=" + std::to_string(segment_timeline_.index)
                    + " segment_window_start_global_us=" + std::to_string(segment_timeline_.start_global_us)
                    + " segment_window_end_global_us=" + std::to_string(segment_timeline_.end_global_us));
    }

    void close(const Config &cfg, const std::string &sender_id, const std::string &camera_id, const std::string &announce_json, Logger &logger) {
        if(!active_) {
            return;
        }
        const auto close_started = std::chrono::steady_clock::now();
        ScopeExit reset_guard([this] { reset_after_close(); });
        try {
            request_task_audio_finalize(cfg, sender_id, camera_id);
        }
        catch(const std::exception &e) {
            logger.warn(std::string("task audio finalize request failed; video finalization continues: ") + e.what());
        }
        std::exception_ptr flush_error;
        try {
            flush_pending_media(cfg, logger);
        }
        catch(...) {
            flush_error = std::current_exception();
            logger.warn("recording pending media flush failed; continuing container finalization: " + directory_);
        }
        if(frames_csv_) {
            frames_csv_.flush();
            frames_csv_.close();
        }
        if(rgb_recorded_frames_csv_) {
            rgb_recorded_frames_csv_.flush();
            rgb_recorded_frames_csv_.close();
        }
        if(rgb_debug_) {
            rgb_debug_.close();
        }
        if(depth_debug_) {
            depth_debug_.close();
        }
        const int rgb_rc = rgb_pipe_.close();
        const int depth_rc = depth_pipe_.close();
        const auto pipes_closed = std::chrono::steady_clock::now();
        mark_end_us(now_us());
        if(rgb_rc != 0) {
            rgb_pipe_failed_ = true;
            logger.warn("rgb ffmpeg exited with non-zero status (" + process_status_text(rgb_rc) + ") for segment: " + directory_);
        }
        if(depth_rc != 0) {
            depth_pipe_failed_ = true;
            logger.warn("depth ffmpeg exited with non-zero status (" + process_status_text(depth_rc) + ") for segment: " + directory_);
        }
        merge_rgb_recorded_frames_into_frames(logger);
        const auto frames_merged = std::chrono::steady_clock::now();
        finalize_completed_media(cfg, logger);
        const auto media_validated = std::chrono::steady_clock::now();
        publish_finalized_frames(logger);
        try {
            wait_for_task_audio(cfg, sender_id, camera_id, logger);
        }
        catch(const std::exception &e) {
            logger.warn(std::string("task audio join failed; video finalization continues: ") + e.what());
        }
        write_meta(cfg, sender_id, camera_id, announce_json, true);
        if(cfg.recording_staging.enabled && cfg.recording_staging.defer_player_compatible_finalize) {
            write_recording_staged_marker(sender_id, camera_id);
        }
        else {
            write_recording_ready_marker(sender_id, camera_id);
        }
        const auto quality = recording_quality_summary();
        if(!quality.complete) {
            logger.warn("recording segment quality=" + quality.status + " directory=" + directory_
                        + " reason=" + quality.reason);
        }
        set_segment_mtime_to_start(logger);
        if(!cfg.recording_staging.enabled) {
            publish_direct_nas_segment(cfg, logger);
        }
        const auto close_finished = std::chrono::steady_clock::now();
        const auto elapsed_ms = [](auto begin, auto end) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count();
        };
        logger.info("recording segment close timing directory=" + directory_
                    + " pipe_close_ms=" + std::to_string(elapsed_ms(close_started, pipes_closed))
                    + " frames_merge_ms=" + std::to_string(elapsed_ms(pipes_closed, frames_merged))
                    + " media_validation_ms=" + std::to_string(elapsed_ms(frames_merged, media_validated))
                    + " publish_ms=" + std::to_string(elapsed_ms(media_validated, close_finished))
                    + " total_ms=" + std::to_string(elapsed_ms(close_started, close_finished)));
        logger.info("recording segment closed: " + directory_);
        if(flush_error) {
            std::rethrow_exception(flush_error);
        }
    }

    void reset_after_close() {
        if(frames_csv_) {
            frames_csv_.close();
        }
        if(rgb_recorded_frames_csv_) {
            rgb_recorded_frames_csv_.close();
        }
        if(rgb_debug_) {
            rgb_debug_.close();
        }
        if(depth_debug_) {
            depth_debug_.close();
        }
        rgb_pipe_.close();
        depth_pipe_.close();
        active_ = false;
        directory_.clear();
        recording_root_.clear();
        relative_directory_.clear();
        camera_name_.clear();
        storage_key_.clear();
        file_prefix_.clear();
        rgb_h264_full_range_ = false;
        depth_width_ = 0;
        depth_height_ = 0;
        last_rgb_ = {};
        last_depth_ = {};
        rgb_pending_.clear();
        rgb_pending_infos_.clear();
        rgb_pending_has_vcl_ = false;
        rgb_pending_has_decodable_start_ = false;
        rgb_recorded_frame_index_ = 0;
        rgb_debug_path_.clear();
        depth_pending_.clear();
        depth_pending_bytes_ = 0;
        last_rgb_frame_interval_us_.reset();
        rgb_fps_probe_.reset();
        depth_fps_probe_.reset();
        rgb_record_fps_ = 0.0;
        depth_record_fps_ = 0.0;
        rgb_nominal_fps_ = 0.0;
        depth_nominal_fps_ = 0.0;
        rgb_expected_ = false;
        depth_expected_ = false;
        rgb_stats_.reset();
        rgb_recorded_stats_.reset();
        depth_stats_.reset();
        rgb_pipe_failed_ = false;
        depth_pipe_failed_ = false;
        depth_part_index_ = 0;
        depth_part_paths_.clear();
        csv_rows_since_flush_ = 0;
        storage_check_packets_ = 0;
        storage_failed_ = false;
        recording_window_valid_rows_ = 0;
        recording_window_valid_rgb_frames_ = 0;
        recording_window_valid_depth_frames_ = 0;
        recording_window_first_valid_global_us_ = 0;
        recording_window_last_valid_global_us_ = 0;
        recording_window_first_valid_rgb_global_us_ = 0;
        recording_window_last_valid_rgb_global_us_ = 0;
        recording_window_first_valid_depth_global_us_ = 0;
        recording_window_last_valid_depth_global_us_ = 0;
        recording_window_rgb_max_gap_us_ = 0;
        recording_window_depth_max_gap_us_ = 0;
        recording_window_rgb_out_of_order_ = 0;
        recording_window_depth_out_of_order_ = 0;
        recording_window_ = {};
        segment_timeline_ = {};
    }

    bool should_rotate_for_timestamp(uint64_t global_timestamp_us) const {
        return active_ && segment_timeline_.end_global_us > 0
               && global_timestamp_us >= segment_timeline_.end_global_us;
    }

    bool should_request_rotation_keyframe(uint64_t receiver_time_us, uint64_t lead_us) const {
        if(!active_ || segment_timeline_.end_global_us == 0) {
            return false;
        }
        return receiver_time_us >= segment_timeline_.end_global_us
               || segment_timeline_.end_global_us - receiver_time_us <= lead_us;
    }

    bool stream_profile_changed(const MediaPacket &packet) const {
        const StreamRecordStats *stats = nullptr;
        if(packet.stream_type == StreamType::rgb) {
            stats = &rgb_stats_;
        }
        else if(packet.stream_type == StreamType::depth_raw) {
            stats = &depth_stats_;
        }
        return stats && stats->frames > 0
               && (stats->width != packet.width || stats->height != packet.height
                   || stats->codec_or_compression != packet.codec_or_compression);
    }

    void write_packet(const Config &cfg, const MediaPacket &packet, const std::string &sender_id, const std::string &camera_id,
                      const std::string &camera_name, const std::string &storage_key, const std::string &file_prefix,
                      const std::string &announce_json, const RecordingWindow &recording_window, Logger &logger,
                      bool allow_rotate = true) {
        if(!active_) {
            start(cfg, sender_id, camera_id, camera_name, storage_key, file_prefix, announce_json,
                  recording_window, packet.global_timestamp_us, logger);
        }
        if(storage_failed_) {
            throw std::runtime_error("recording storage previously failed: " + directory_);
        }
        if(++storage_check_packets_ >= 30) {
            storage_check_packets_ = 0;
            std::error_code ec;
            const auto space = std::filesystem::space(directory_, ec);
            if(ec || space.available < cfg.min_free_disk_bytes) {
                storage_failed_ = true;
                throw std::runtime_error("recording stopped because free space is below the configured reserve: " + directory_);
            }
        }
        if(allow_rotate && (stream_profile_changed(packet) || should_rotate_for_timestamp(packet.global_timestamp_us))) {
            if(stream_profile_changed(packet)) {
                logger.warn("media profile changed; rotating segment camera=" + camera_key(sender_id, camera_id));
            }
            close(cfg, sender_id, camera_id, announce_json, logger);
            start(cfg, sender_id, camera_id, camera_name, storage_key, file_prefix, announce_json,
                  recording_window, packet.global_timestamp_us, logger);
        }

        const uint64_t packet_local_us = now_us();
        const auto stream = std::string(stream_type_name(packet.stream_type));
        const bool rgb_packet_has_vcl = packet.stream_type == StreamType::rgb && h264_payload_has_vcl_nal(packet.payload);
        if(packet.stream_type == StreamType::rgb) {
            write_rgb_packet(cfg, packet, packet_local_us, logger);
            if(rgb_packet_has_vcl) {
                std::optional<int64_t> frame_interval_us;
                if(last_rgb_.valid && last_rgb_.system_timestamp_us > 0 && packet.system_timestamp_us > 0) {
                    frame_interval_us = packet.system_timestamp_us >= last_rgb_.system_timestamp_us
                                            ? static_cast<int64_t>(packet.system_timestamp_us - last_rgb_.system_timestamp_us)
                                            : -static_cast<int64_t>(last_rgb_.system_timestamp_us - packet.system_timestamp_us);
                }
                last_rgb_ = FrameInfo{true, packet.frame_id, packet.timestamp_us, packet.system_timestamp_us, packet.pair_id,
                                      packet.rgb_exposure_us, packet.rgb_gain, packet.rgb_auto_exposure, packet.rgb_actual_fps};
                last_rgb_frame_interval_us_ = frame_interval_us;
                rgb_stats_.add(packet, packet_local_us);
            }
        }
        else if(packet.stream_type == StreamType::depth_raw) {
            if(depth_debug_) {
                depth_debug_.write(reinterpret_cast<const char *>(packet.payload.data()), static_cast<std::streamsize>(packet.payload.size()));
            }
            write_depth_packet(cfg, packet, packet_local_us, logger);
            last_depth_ = FrameInfo{true, packet.frame_id, packet.timestamp_us, packet.system_timestamp_us, packet.pair_id};
            depth_stats_.add(packet, packet_local_us);
        }

        if(frames_csv_ && (packet.stream_type != StreamType::rgb || rgb_packet_has_vcl)) {
            frames_csv_ << packet_local_us << ',' << stream << ',';
            if(last_rgb_.valid) {
                frames_csv_ << last_rgb_.frame_id << ',' << last_rgb_.timestamp_us << ',';
            }
            else {
                frames_csv_ << ",,";
            }
            if(last_depth_.valid) {
                frames_csv_ << last_depth_.frame_id << ',' << last_depth_.timestamp_us << ',';
            }
            else {
                frames_csv_ << ",,";
            }
            frames_csv_ << packet.pair_id << ',';
            if(last_rgb_.valid && last_depth_.valid) {
                const bool use_system_pair_delta = last_rgb_.system_timestamp_us > 0 && last_depth_.system_timestamp_us > 0;
                const uint64_t rgb_pair_us = use_system_pair_delta ? last_rgb_.system_timestamp_us : last_rgb_.timestamp_us;
                const uint64_t depth_pair_us = use_system_pair_delta ? last_depth_.system_timestamp_us : last_depth_.timestamp_us;
                const auto delta = rgb_pair_us > depth_pair_us ? rgb_pair_us - depth_pair_us : depth_pair_us - rgb_pair_us;
                frames_csv_ << static_cast<double>(delta) / 1000.0;
            }
            frames_csv_ << ',' << packet.width << ',' << packet.height << ',' << packet.payload_size << ',' << packet.system_timestamp_us << ',';
            if(last_rgb_.valid) {
                frames_csv_ << last_rgb_.system_timestamp_us;
            }
            frames_csv_ << ',';
            if(last_depth_.valid) {
                frames_csv_ << last_depth_.system_timestamp_us;
            }
            frames_csv_ << ',' << packet.frame_id << ',' << packet.timestamp_us << ',' << packet.system_timestamp_us << ',';
            if(last_rgb_.valid && last_rgb_.exposure_us >= 0) {
                frames_csv_ << last_rgb_.exposure_us;
            }
            frames_csv_ << ',';
            if(last_rgb_.valid && last_rgb_.gain >= 0) {
                frames_csv_ << last_rgb_.gain;
            }
            frames_csv_ << ',';
            if(last_rgb_.valid && last_rgb_.auto_exposure >= 0) {
                frames_csv_ << last_rgb_.auto_exposure;
            }
            frames_csv_ << ',';
            if(last_rgb_.valid && last_rgb_.actual_fps >= 0) {
                frames_csv_ << last_rgb_.actual_fps;
            }
            frames_csv_ << ',';
            if(last_rgb_frame_interval_us_) {
                frames_csv_ << *last_rgb_frame_interval_us_;
            }
            frames_csv_ << ',' << packet.codec_or_compression;
            write_pipeline_diagnostics_columns(frames_csv_, packet, packet_local_us);
            write_clock_sync_columns(frames_csv_, packet);
            write_pair_quality_column(frames_csv_, last_rgb_, last_depth_);
            frames_csv_ << ',';
            if(last_rgb_.valid && last_depth_.valid) {
                const bool use_system = last_rgb_.system_timestamp_us > 0 && last_depth_.system_timestamp_us > 0;
                const uint64_t rgb_time = use_system ? last_rgb_.system_timestamp_us : last_rgb_.timestamp_us;
                const uint64_t depth_time = use_system ? last_depth_.system_timestamp_us : last_depth_.timestamp_us;
                const int64_t signed_delta = depth_time >= rgb_time ? static_cast<int64_t>(depth_time - rgb_time)
                                                                    : -static_cast<int64_t>(rgb_time - depth_time);
                frames_csv_ << signed_delta << ',' << (use_system ? "system_timestamp_us" : "device_timestamp_us");
            }
            else {
                frames_csv_ << ',';
            }
            const bool pair_id_valid = last_rgb_.valid && last_depth_.valid && last_rgb_.pair_id != 0
                                       && last_rgb_.pair_id == last_depth_.pair_id;
            frames_csv_ << ',' << (pair_id_valid ? 1 : 0);
            frames_csv_ << '\n';
            if(!frames_csv_) {
                throw std::runtime_error("frames.csv staging write failed: " + file_path("frames.csv.inprogress").string());
            }
            if(++csv_rows_since_flush_ >= 30) {
                frames_csv_.flush();
                rgb_recorded_frames_csv_.flush();
                if(!frames_csv_ || !rgb_recorded_frames_csv_) {
                    throw std::runtime_error("recording CSV flush failed: " + directory_);
                }
                csv_rows_since_flush_ = 0;
            }
        }
    }

private:
    std::filesystem::path file_path(const std::string &basename) const {
        return std::filesystem::path(directory_) / prefixed_filename(file_prefix_, basename);
    }

    std::filesystem::path task_audio_path(const std::string &basename) const {
        return std::filesystem::path(directory_) / basename;
    }

    void write_atomic_text(const std::filesystem::path &path, const std::string &content) const {
        const auto temporary = path.string() + ".receiver.tmp";
        {
            std::ofstream out(temporary, std::ios::out | std::ios::trunc);
            if(!out) {
                throw std::runtime_error("cannot create task audio marker: " + temporary);
            }
            out << content;
            out.flush();
            out.close();
            if(!out) {
                throw std::runtime_error("cannot finish task audio marker: " + temporary);
            }
        }
        std::error_code ec;
        std::filesystem::rename(temporary, path, ec);
        if(ec) {
            std::filesystem::remove(temporary);
            throw std::runtime_error("cannot publish task audio marker " + path.string() + ": " + ec.message());
        }
    }

    uint64_t effective_segment_end_global_us() const {
        uint64_t end = end_us_;
        if(segment_timeline_.end_global_us > 0 && (end == 0 || segment_timeline_.end_global_us < end)) {
            end = segment_timeline_.end_global_us;
        }
        if(recording_window_.end_global_us > 0 && (end == 0 || recording_window_.end_global_us < end)) {
            end = recording_window_.end_global_us;
        }
        return end;
    }

    void write_task_audio_manifest(const Config &cfg, const std::string &sender_id,
                                   const std::string &camera_id) const {
        if(!cfg.task_audio.enabled || directory_.empty()
           || (!cfg.task_audio.sender_ids.empty() && cfg.task_audio.sender_ids.count(sender_id) == 0)) {
            return;
        }
        std::ostringstream out;
        out << "{\n"
            << "  \"schema_version\": 1,\n"
            << "  \"sender_id\": \"" << json_escape(sender_id) << "\",\n"
            << "  \"camera_id\": \"" << json_escape(camera_id) << "\",\n"
            << "  \"recording_session_id\": " << recording_window_.session_id << ",\n"
            << "  \"segment_window_start_global_us\": " << segment_timeline_.start_global_us << ",\n"
            << "  \"segment_window_end_global_us\": " << segment_timeline_.end_global_us << "\n"
            << "}\n";
        write_atomic_text(task_audio_path(".audio_task.json"), out.str());
        notify_task_audio(cfg, "video_task_audio_start", sender_id, camera_id,
                          segment_timeline_.end_global_us);
    }

    void request_task_audio_finalize(const Config &cfg, const std::string &sender_id,
                                     const std::string &camera_id) const {
        if(!cfg.task_audio.enabled || directory_.empty()
           || (!cfg.task_audio.sender_ids.empty() && cfg.task_audio.sender_ids.count(sender_id) == 0)) {
            return;
        }
        std::ostringstream out;
        out << "{\n"
            << "  \"schema_version\": 1,\n"
            << "  \"sender_id\": \"" << json_escape(sender_id) << "\",\n"
            << "  \"camera_id\": \"" << json_escape(camera_id) << "\",\n"
            << "  \"recording_session_id\": " << recording_window_.session_id << ",\n"
            << "  \"segment_end_global_us\": " << effective_segment_end_global_us() << ",\n"
            << "  \"requested_receiver_us\": " << now_us() << "\n"
            << "}\n";
        write_atomic_text(task_audio_path(".audio_finalize_request.json"), out.str());
        notify_task_audio(cfg, "video_task_audio_finalize", sender_id, camera_id,
                          effective_segment_end_global_us());
    }

    void notify_task_audio(const Config &cfg, const std::string &message_type,
                           const std::string &sender_id, const std::string &camera_id,
                           uint64_t end_global_us) const {
        const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if(fd < 0) {
            return;
        }
        ScopeExit close_socket([fd] { ::close(fd); });
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(cfg.task_audio.notify_port);
        if(inet_pton(AF_INET, cfg.task_audio.notify_host.c_str(), &address.sin_addr) != 1) {
            return;
        }
        std::ostringstream payload;
        payload << '{'
                << "\"protocol_version\":\"3.0\","
                << "\"message_type\":\"" << json_escape(message_type) << "\","
                << "\"sender_id\":\"" << json_escape(sender_id) << "\","
                << "\"camera_id\":\"" << json_escape(camera_id) << "\","
                << "\"recording_session_id\":" << recording_window_.session_id << ','
                << "\"segment_dir\":\"" << json_escape(directory_) << "\","
                << "\"segment_window_start_global_us\":" << segment_timeline_.start_global_us << ','
                << "\"segment_window_end_global_us\":" << segment_timeline_.end_global_us << ','
                << "\"segment_end_global_us\":" << end_global_us
                << '}';
        const auto text = payload.str();
        sendto(fd, text.data(), text.size(), MSG_DONTWAIT,
               reinterpret_cast<const sockaddr *>(&address), sizeof(address));
    }

    void write_no_input_task_audio(const std::string &sender_id, const std::string &camera_id,
                                   const std::string &reason) const {
        const uint64_t window_start_us = segment_timeline_.start_global_us;
        const uint64_t window_end_us = effective_segment_end_global_us();
        const uint64_t duration_us = window_end_us > window_start_us ? window_end_us - window_start_us : 0;
        const uint64_t expected_packets = (duration_us + 19'999) / 20'000;
        const auto timing = task_audio_path("audio_timing.csv");
        if(!std::filesystem::exists(timing)) {
            std::ofstream csv(timing, std::ios::out | std::ios::trunc);
            csv << "global_timestamp_us,window_start_global_us,window_end_global_us,expected_packets,"
                   "received_packets,silence_packets,quality_status\n";
            csv << window_start_us << ',' << window_start_us << ',' << window_end_us << ','
                << expected_packets << ",0," << expected_packets << ",no_input\n";
        }
        std::ostringstream meta;
        meta << "{\n"
             << "  \"schema_version\": 2,\n"
             << "  \"task_audio\": true,\n"
             << "  \"sender_id\": \"" << json_escape(sender_id) << "\",\n"
             << "  \"camera_id\": \"" << json_escape(camera_id) << "\",\n"
             << "  \"recording_session_id\": " << recording_window_.session_id << ",\n"
             << "  \"segment_window_start_global_us\": " << window_start_us << ",\n"
             << "  \"segment_window_end_global_us\": " << window_end_us << ",\n"
             << "  \"audio_valid\": false,\n"
             << "  \"quality_status\": \"no_input\",\n"
             << "  \"quality_reason\": \"" << json_escape(reason) << "\",\n"
             << "  \"expected_packets\": " << expected_packets << ",\n"
             << "  \"received_packets\": 0,\n"
             << "  \"silence_packets\": " << expected_packets << ",\n"
             << "  \"received_ratio\": 0.0,\n"
             << "  \"longest_no_input_ms\": " << (duration_us / 1000) << ",\n"
             << "  \"created_receiver_us\": " << now_us() << "\n"
             << "}\n";
        write_atomic_text(task_audio_path("audio_meta.json"), meta.str());
        std::ostringstream ready;
        ready << "{\n"
              << "  \"schema_version\": 2,\n"
              << "  \"ready\": true,\n"
              << "  \"task_audio\": true,\n"
              << "  \"sender_id\": \"" << json_escape(sender_id) << "\",\n"
              << "  \"camera_id\": \"" << json_escape(camera_id) << "\",\n"
              << "  \"recording_session_id\": " << recording_window_.session_id << ",\n"
              << "  \"audio_valid\": false,\n"
              << "  \"quality_status\": \"no_input\",\n"
              << "  \"quality_reason\": \"" << json_escape(reason) << "\",\n"
              << "  \"finalized_receiver_us\": " << now_us() << "\n"
              << "}\n";
        write_atomic_text(task_audio_path("audio_ready.json"), ready.str());
        std::error_code ec;
        std::filesystem::remove(task_audio_path(".audio_task.json"), ec);
        std::filesystem::remove(task_audio_path(".audio_finalize_request.json"), ec);
        std::filesystem::remove(task_audio_path(".audio.opus.inprogress"), ec);
        std::filesystem::remove(task_audio_path(".audio_timing.csv.inprogress"), ec);
    }

    void wait_for_task_audio(const Config &cfg, const std::string &sender_id,
                             const std::string &camera_id, Logger &logger) const {
        if(!cfg.task_audio.enabled || directory_.empty()) {
            return;
        }
        if(!cfg.task_audio.sender_ids.empty() && cfg.task_audio.sender_ids.count(sender_id) == 0) {
            write_no_input_task_audio(sender_id, camera_id, "sender has no configured audio input");
            logger.info("task audio marked no_input for sender without configured microphone directory=" + directory_);
            return;
        }
        const auto ready = task_audio_path("audio_ready.json");
        const auto deadline = std::chrono::steady_clock::now()
                              + std::chrono::milliseconds(cfg.task_audio.finalize_wait_ms);
        while(!std::filesystem::exists(ready) && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(cfg.task_audio.poll_interval_ms));
        }
        if(std::filesystem::exists(ready)) {
            logger.info("task audio joined recording segment directory=" + directory_);
            return;
        }
        write_no_input_task_audio(sender_id, camera_id, "audio finalize timeout or sender has no configured input");
        logger.warn("task audio unavailable after " + std::to_string(cfg.task_audio.finalize_wait_ms)
                    + " ms; video published with no_input marker directory=" + directory_);
    }

    static void write_pipeline_diagnostics_columns(std::ostream &csv, const MediaPacket &packet, uint64_t packet_local_us) {
        csv << ',';
        write_optional_us(csv, packet.sender_capture_host_timestamp_us);
        csv << ',';
        write_optional_us(csv, packet.sender_timing_bound_timestamp_us);
        csv << ',';
        write_optional_us(csv, packet.sender_encode_start_timestamp_us);
        csv << ',';
        write_optional_us(csv, packet.sender_encode_done_timestamp_us);
        csv << ',';
        write_optional_us(csv, packet.sender_packet_queued_timestamp_us);
        csv << ',';
        write_optional_delta_us(csv, packet_local_us, packet.system_timestamp_us);
        csv << ',';
        write_optional_delta_us(csv, packet.sender_timing_bound_timestamp_us, packet.sender_capture_host_timestamp_us);
        csv << ',';
        write_optional_delta_us(csv, packet.sender_encode_start_timestamp_us, packet.sender_timing_bound_timestamp_us);
        csv << ',';
        write_optional_delta_us(csv, packet.sender_encode_done_timestamp_us, packet.sender_encode_start_timestamp_us);
        csv << ',';
        write_optional_delta_us(csv, packet.sender_packet_queued_timestamp_us, packet.sender_encode_done_timestamp_us);
        csv << ',';
        write_optional_delta_us(csv, packet_local_us, packet.sender_packet_queued_timestamp_us);
    }

    static void write_clock_sync_columns(std::ostream &csv, const MediaPacket &packet) {
        csv << ',' << packet.sender_id
            << ',' << packet.camera_id
            << ',' << packet.timestamp_us
            << ',' << packet.system_timestamp_us
            << ',' << packet.receiver_receive_timestamp_us
            << ',' << (packet.clock_sync_valid ? 1 : 0)
            << ',' << packet.sender_offset_us
            << ',' << packet.sender_delay_us
            << ',' << packet.sender_drift_ppm
            << ',' << packet.global_timestamp_us;
    }

    static void write_pair_quality_column(std::ostream &csv, const FrameInfo &rgb, const FrameInfo &depth) {
        csv << ',';
        if(!rgb.valid || !depth.valid) {
            csv << 0;
            return;
        }
        if(rgb.pair_id != 0 && depth.pair_id != 0 && rgb.pair_id != depth.pair_id) {
            csv << 0;
            return;
        }
        const bool use_system_pair_delta = rgb.system_timestamp_us > 0 && depth.system_timestamp_us > 0;
        const uint64_t rgb_pair_us = use_system_pair_delta ? rgb.system_timestamp_us : rgb.timestamp_us;
        const uint64_t depth_pair_us = use_system_pair_delta ? depth.system_timestamp_us : depth.timestamp_us;
        const uint64_t delta = rgb_pair_us > depth_pair_us ? rgb_pair_us - depth_pair_us : depth_pair_us - rgb_pair_us;
        csv << (delta <= kRgbDepthPairValidMaxDeltaUs ? 1 : 0);
    }

    bool write_rgb_recovery_bytes(const uint8_t *data, size_t size, Logger &logger) {
        if(!rgb_debug_.is_open() || size == 0) {
            return false;
        }
        rgb_debug_.write(reinterpret_cast<const char *>(data), static_cast<std::streamsize>(size));
        if(!rgb_debug_) {
            logger.warn("RGB H264 recovery write failed: " + rgb_debug_path_.string());
            rgb_debug_.close();
            return false;
        }
        return true;
    }

    void write_rgb_recorded_frame(const MediaPacket &packet, uint64_t packet_local_us, size_t recorded_payload_size,
                                  bool payload_known_vcl = false) {
        if(!rgb_recorded_frames_csv_ || (!payload_known_vcl && !h264_payload_has_vcl_nal(packet.payload))) {
            return;
        }
        rgb_recorded_frames_csv_ << rgb_recorded_frame_index_++ << ',' << packet_local_us << ',' << packet.frame_id << ','
                                 << packet.timestamp_us << ',' << packet.system_timestamp_us << ',' << packet.width << ','
                                 << packet.height << ',' << recorded_payload_size << ',' << packet.system_timestamp_us << ',';
        if(packet.rgb_exposure_us >= 0) {
            rgb_recorded_frames_csv_ << packet.rgb_exposure_us;
        }
        rgb_recorded_frames_csv_ << ',';
        if(packet.rgb_gain >= 0) {
            rgb_recorded_frames_csv_ << packet.rgb_gain;
        }
        rgb_recorded_frames_csv_ << ',';
        if(packet.rgb_auto_exposure >= 0) {
            rgb_recorded_frames_csv_ << packet.rgb_auto_exposure;
        }
        rgb_recorded_frames_csv_ << ',';
        if(packet.rgb_actual_fps >= 0) {
            rgb_recorded_frames_csv_ << packet.rgb_actual_fps;
        }
        rgb_recorded_frames_csv_ << ',' << packet.codec_or_compression;
        write_pipeline_diagnostics_columns(rgb_recorded_frames_csv_, packet, packet_local_us);
        write_clock_sync_columns(rgb_recorded_frames_csv_, packet);
        rgb_recorded_frames_csv_ << '\n';
        if(!rgb_recorded_frames_csv_) {
            throw std::runtime_error("RGB frame index CSV write failed: " + file_path("rgb_recorded_frames.csv").string());
        }
        rgb_recorded_stats_.add(packet, packet_local_us);
    }

    void write_pending_rgb_recorded_frames() {
        for(const auto &info : rgb_pending_infos_) {
            if(info.has_vcl) {
                write_rgb_recorded_frame(info.packet, info.local_time_us, info.payload_size, true);
            }
        }
    }

    static std::vector<std::string> split_csv_line(const std::string &line) {
        std::vector<std::string> fields;
        std::string field;
        std::stringstream input(line);
        while(std::getline(input, field, ',')) {
            fields.push_back(field);
        }
        if(!line.empty() && line.back() == ',') {
            fields.emplace_back();
        }
        return fields;
    }

    static std::map<std::string, size_t> csv_header_index(const std::vector<std::string> &header) {
        std::map<std::string, size_t> index;
        for(size_t i = 0; i < header.size(); ++i) {
            index[header[i]] = i;
        }
        return index;
    }

    static std::string csv_value(const std::vector<std::string> &row, const std::map<std::string, size_t> &index,
                                 const std::string &name) {
        const auto found = index.find(name);
        if(found == index.end() || found->second >= row.size()) {
            return {};
        }
        return row[found->second];
    }

    static std::string rgb_record_key(const std::vector<std::string> &row, const std::map<std::string, size_t> &index) {
        const auto frame_id = csv_value(row, index, "frame_id");
        const auto timestamp_us = csv_value(row, index, "timestamp_us");
        const auto frame_system_timestamp_us = csv_value(row, index, "frame_system_timestamp_us");
        if(frame_id.empty() || timestamp_us.empty() || frame_system_timestamp_us.empty()) {
            return {};
        }
        return frame_id + "\t" + timestamp_us + "\t" + frame_system_timestamp_us;
    }

    void add_recording_window_summary(const std::string &stream_type, uint64_t global_us, bool valid) {
        if(!valid || global_us == 0) {
            return;
        }
        ++recording_window_valid_rows_;
        if(recording_window_first_valid_global_us_ == 0 || global_us < recording_window_first_valid_global_us_) {
            recording_window_first_valid_global_us_ = global_us;
        }
        recording_window_last_valid_global_us_ = std::max(recording_window_last_valid_global_us_, global_us);
        if(stream_type == "rgb") {
            ++recording_window_valid_rgb_frames_;
            if(recording_window_first_valid_rgb_global_us_ == 0 || global_us < recording_window_first_valid_rgb_global_us_) {
                recording_window_first_valid_rgb_global_us_ = global_us;
            }
            if(recording_window_last_valid_rgb_global_us_ > 0) {
                if(global_us < recording_window_last_valid_rgb_global_us_) {
                    ++recording_window_rgb_out_of_order_;
                }
                else {
                    recording_window_rgb_max_gap_us_ =
                        std::max(recording_window_rgb_max_gap_us_, global_us - recording_window_last_valid_rgb_global_us_);
                }
            }
            recording_window_last_valid_rgb_global_us_ = std::max(recording_window_last_valid_rgb_global_us_, global_us);
        }
        else if(stream_type == "depth" || stream_type == "depth_raw") {
            ++recording_window_valid_depth_frames_;
            if(recording_window_first_valid_depth_global_us_ == 0 || global_us < recording_window_first_valid_depth_global_us_) {
                recording_window_first_valid_depth_global_us_ = global_us;
            }
            if(recording_window_last_valid_depth_global_us_ > 0) {
                if(global_us < recording_window_last_valid_depth_global_us_) {
                    ++recording_window_depth_out_of_order_;
                }
                else {
                    recording_window_depth_max_gap_us_ =
                        std::max(recording_window_depth_max_gap_us_, global_us - recording_window_last_valid_depth_global_us_);
                }
            }
            recording_window_last_valid_depth_global_us_ = std::max(recording_window_last_valid_depth_global_us_, global_us);
        }
    }

    void merge_rgb_recorded_frames_into_frames(Logger &logger) {
        const auto frames_path = file_path("frames.csv.inprogress");
        const auto finalized_path = file_path("frames.csv.finalizing");
        const auto recorded_path = file_path("rgb_recorded_frames.csv");
        std::error_code ec;
        if(!std::filesystem::exists(frames_path, ec)) {
            return;
        }

        std::ifstream frames_in(frames_path);
        if(!frames_in) {
            logger.warn("failed to reopen frames.csv for RGB frame index merge: " + frames_path.string());
            return;
        }

        std::map<std::string, std::pair<std::string, std::string>> recorded_by_key;
        size_t duplicate_recorded_keys = 0;
        std::string first_duplicate_recorded_key;
        if(std::filesystem::exists(recorded_path, ec)) {
            std::ifstream recorded_in(recorded_path);
            if(recorded_in) {
                std::string recorded_header_line;
                if(std::getline(recorded_in, recorded_header_line)) {
                    const auto recorded_header = split_csv_line(recorded_header_line);
                    const auto recorded_index = csv_header_index(recorded_header);
                    std::string line;
                    while(std::getline(recorded_in, line)) {
                        const auto row = split_csv_line(line);
                        const std::string key = rgb_record_key(row, recorded_index);
                        if(!key.empty()) {
                            auto inserted = recorded_by_key.emplace(
                                key, std::make_pair(csv_value(row, recorded_index, "video_frame_index"),
                                                    csv_value(row, recorded_index, "payload_size")));
                            if(!inserted.second) {
                                duplicate_recorded_keys++;
                                if(first_duplicate_recorded_key.empty()) {
                                    first_duplicate_recorded_key = key;
                                }
                            }
                        }
                    }
                }
            }
            else {
                logger.warn("failed to reopen rgb_recorded_frames.csv for merge: " + recorded_path.string());
            }
        }
        if(duplicate_recorded_keys > 0) {
            logger.warn("duplicate RGB recorded frame keys ignored during merge path=" + recorded_path.string()
                        + " duplicates=" + std::to_string(duplicate_recorded_keys)
                        + " first_key=\"" + first_duplicate_recorded_key + "\"");
        }

        const auto tmp_path = finalized_path.string() + ".merge_tmp";
        std::ofstream merged(tmp_path, std::ios::out | std::ios::trunc);
        if(!merged) {
            logger.warn("failed to create merged frames.csv tmp: " + tmp_path);
            return;
        }

        std::string header_line;
        if(!std::getline(frames_in, header_line)) {
            logger.warn("frames.csv is empty during RGB frame index merge: " + frames_path.string());
            return;
        }
        const auto header = split_csv_line(header_line);
        const auto index = csv_header_index(header);
        merged << header_line
               << ",rgb_recorded,rgb_video_frame_index,rgb_recorded_payload_size,recording_session_id,"
                  "recording_window_start_global_us,recording_window_end_global_us,recording_window_valid,"
                  "global_segment_index,segment_window_start_global_us,segment_window_end_global_us,segment_window_valid\n";

        recording_window_valid_rows_ = 0;
        recording_window_valid_rgb_frames_ = 0;
        recording_window_valid_depth_frames_ = 0;
        recording_window_first_valid_global_us_ = 0;
        recording_window_last_valid_global_us_ = 0;
        recording_window_first_valid_rgb_global_us_ = 0;
        recording_window_last_valid_rgb_global_us_ = 0;
        recording_window_first_valid_depth_global_us_ = 0;
        recording_window_last_valid_depth_global_us_ = 0;
        recording_window_rgb_max_gap_us_ = 0;
        recording_window_depth_max_gap_us_ = 0;
        recording_window_rgb_out_of_order_ = 0;
        recording_window_depth_out_of_order_ = 0;

        const auto recording_window_state = [this, &index](const std::vector<std::string> &row) {
            bool valid = recording_window_.start_global_us == 0;
            uint64_t global_us = 0;
            const auto global_text = csv_value(row, index, "global_timestamp_us");
            if(!global_text.empty()) {
                try {
                    global_us = std::stoull(global_text);
                    valid = (recording_window_.start_global_us == 0 || global_us >= recording_window_.start_global_us)
                            && (recording_window_.end_global_us == 0 || global_us <= recording_window_.end_global_us);
                }
                catch(const std::exception &) {
                    valid = false;
                }
            }
            return std::make_pair(global_us, valid);
        };
        const auto append_recording_window = [this](std::ostream &out, const std::pair<uint64_t, bool> &state) {
            out << ',' << recording_window_.session_id << ',' << recording_window_.start_global_us << ','
                << recording_window_.end_global_us << ',' << (state.second ? 1 : 0) << ','
                << segment_timeline_.index << ',' << segment_timeline_.start_global_us << ','
                << segment_timeline_.end_global_us << ','
                << (state.first >= segment_timeline_.start_global_us
                            && (segment_timeline_.end_global_us == 0
                                || state.first < segment_timeline_.end_global_us)
                        ? 1
                        : 0);
        };

        std::string line;
        std::set<std::string> merged_rgb_keys;
        size_t duplicate_frame_rows = 0;
        size_t dropped_unrecorded_rgb_rows = 0;
        std::string first_duplicate_frame_key;
        while(std::getline(frames_in, line)) {
            const auto row = split_csv_line(line);
            const auto stream_type = csv_value(row, index, "stream_type");
            const bool is_rgb = stream_type == "rgb";
            const auto window_state = recording_window_state(row);
            if(is_rgb) {
                const auto key = rgb_record_key(row, index);
                const bool duplicate_frame_key = !key.empty() && !merged_rgb_keys.insert(key).second;
                if(duplicate_frame_key) {
                    duplicate_frame_rows++;
                    if(first_duplicate_frame_key.empty()) {
                        first_duplicate_frame_key = key;
                    }
                }
                const auto found = duplicate_frame_key ? recorded_by_key.end() : recorded_by_key.find(key);
                if(found != recorded_by_key.end()) {
                    merged << line << ",1," << found->second.first << ',' << found->second.second;
                    append_recording_window(merged, window_state);
                    merged << '\n';
                    add_recording_window_summary(stream_type, window_state.first, window_state.second);
                }
                else {
                    dropped_unrecorded_rgb_rows++;
                }
            }
            else {
                merged << line << ",,,";
                append_recording_window(merged, window_state);
                merged << '\n';
                add_recording_window_summary(stream_type, window_state.first, window_state.second);
            }
        }
        if(duplicate_frame_rows > 0) {
            logger.warn("duplicate RGB frame keys marked unrecorded during frames.csv merge path=" + frames_path.string()
                        + " duplicates=" + std::to_string(duplicate_frame_rows)
                        + " first_key=\"" + first_duplicate_frame_key + "\"");
        }
        if(dropped_unrecorded_rgb_rows > 0) {
            logger.warn("unrecorded RGB frame rows dropped during frames.csv merge path=" + frames_path.string()
                        + " dropped=" + std::to_string(dropped_unrecorded_rgb_rows));
        }
        merged.close();
        if(!merged) {
            logger.warn("failed to finish merged frames.csv tmp: " + tmp_path);
            std::filesystem::remove(tmp_path, ec);
            return;
        }
        std::filesystem::rename(tmp_path, finalized_path, ec);
        if(ec) {
            logger.warn("failed to stage finalized frames.csv with merged RGB frame indexes: " + ec.message());
            std::filesystem::remove(tmp_path, ec);
            return;
        }
        if(std::filesystem::exists(recorded_path, ec)) {
            std::filesystem::remove(recorded_path, ec);
            if(ec) {
                logger.warn("failed to remove merged rgb_recorded_frames.csv: " + ec.message());
            }
        }
    }

    void publish_finalized_frames(Logger &logger) {
        const auto staging_path = file_path("frames.csv.inprogress");
        const auto finalized_path = file_path("frames.csv.finalizing");
        const auto published_path = file_path("frames.csv");
        std::error_code ec;
        if(!std::filesystem::exists(finalized_path, ec) || ec) {
            throw std::runtime_error("finalized frames.csv is unavailable for publication: " + finalized_path.string());
        }
        std::filesystem::rename(finalized_path, published_path, ec);
        if(ec) {
            throw std::runtime_error("cannot publish finalized frames.csv: " + ec.message());
        }
        std::filesystem::remove(staging_path, ec);
        if(ec) {
            logger.warn("cannot remove frames.csv staging file after publication: " + ec.message());
        }
        logger.info("finalized frames.csv published atomically: " + published_path.string());
    }

    struct RecordingQualitySummary {
        std::string status = "partial";
        std::string reason = "recording window is unavailable";
        bool complete = false;
        uint64_t window_start_us = 0;
        uint64_t window_end_us = 0;
        uint64_t window_duration_us = 0;
        uint64_t rgb_first_lag_us = 0;
        uint64_t rgb_end_lag_us = 0;
        uint64_t depth_first_lag_us = 0;
        uint64_t depth_end_lag_us = 0;
        double rgb_coverage_ratio = 0.0;
        double depth_coverage_ratio = 0.0;
        uint64_t rgb_media_duration_us = 0;
        uint64_t depth_media_duration_us = 0;
        uint64_t rgb_depth_duration_delta_us = 0;
    };

    RecordingQualitySummary recording_quality_summary() const {
        RecordingQualitySummary quality;
        quality.window_start_us = segment_timeline_.start_global_us > 0
                                      ? segment_timeline_.start_global_us
                                      : recording_window_.start_global_us;
        quality.window_start_us = std::max(quality.window_start_us, recording_window_.start_global_us);
        quality.window_end_us = end_us_;
        if(segment_timeline_.end_global_us > 0
           && (quality.window_end_us == 0 || segment_timeline_.end_global_us < quality.window_end_us)) {
            quality.window_end_us = segment_timeline_.end_global_us;
        }
        if(recording_window_.end_global_us > 0
           && (quality.window_end_us == 0 || recording_window_.end_global_us < quality.window_end_us)) {
            quality.window_end_us = recording_window_.end_global_us;
        }
        if(quality.window_start_us == 0 || quality.window_end_us <= quality.window_start_us) {
            return quality;
        }
        quality.window_duration_us = quality.window_end_us - quality.window_start_us;

        const auto first_lag = [missing = quality.window_duration_us](uint64_t start, uint64_t frame) {
            return frame == 0 ? missing : (frame > start ? frame - start : 0);
        };
        const auto end_lag = [missing = quality.window_duration_us](uint64_t end, uint64_t frame) {
            return frame == 0 ? missing : (end > frame ? end - frame : 0);
        };
        quality.rgb_first_lag_us = first_lag(quality.window_start_us, recording_window_first_valid_rgb_global_us_);
        quality.rgb_end_lag_us = end_lag(quality.window_end_us, recording_window_last_valid_rgb_global_us_);
        quality.depth_first_lag_us = first_lag(quality.window_start_us, recording_window_first_valid_depth_global_us_);
        quality.depth_end_lag_us = end_lag(quality.window_end_us, recording_window_last_valid_depth_global_us_);
        const auto coverage = [duration = quality.window_duration_us](uint64_t frames, double fps) {
            if(duration == 0 || fps <= 0.0) {
                return 0.0;
            }
            return static_cast<double>(frames) * 1'000'000.0 / (static_cast<double>(duration) * fps);
        };
        quality.rgb_coverage_ratio = coverage(recording_window_valid_rgb_frames_, rgb_nominal_fps_);
        quality.depth_coverage_ratio = coverage(recording_window_valid_depth_frames_, depth_nominal_fps_);
        quality.rgb_media_duration_us = static_cast<uint64_t>(
            std::max(0.0, media_duration_seconds(rgb_recorded_stats_.frames > 0 ? rgb_recorded_stats_ : rgb_stats_))
            * 1'000'000.0);
        quality.depth_media_duration_us = static_cast<uint64_t>(
            std::max(0.0, media_duration_seconds(depth_stats_)) * 1'000'000.0);
        quality.rgb_depth_duration_delta_us = quality.rgb_media_duration_us >= quality.depth_media_duration_us
                                                  ? quality.rgb_media_duration_us - quality.depth_media_duration_us
                                                  : quality.depth_media_duration_us - quality.rgb_media_duration_us;

        constexpr double kMinimumCoverage = 0.98;
        constexpr uint64_t kMaximumEdgeLagUs = 500'000;
        constexpr uint64_t kMaximumFrameGapUs = 500'000;
        std::vector<std::string> failures;
        const auto inspect_stream = [&](const char *name,
                                        bool expected,
                                        uint64_t frames,
                                        double coverage_ratio,
                                        uint64_t first_frame_lag_us,
                                        uint64_t last_frame_lag_us,
                                        uint64_t max_gap_us,
                                        uint64_t out_of_order) {
            if(!expected) {
                return;
            }
            if(frames == 0) {
                failures.emplace_back(std::string(name) + " has no frames");
                return;
            }
            if(coverage_ratio < kMinimumCoverage) {
                failures.emplace_back(std::string(name) + " coverage below 98 percent");
            }
            if(first_frame_lag_us > kMaximumEdgeLagUs) {
                failures.emplace_back(std::string(name) + " first frame is late");
            }
            if(last_frame_lag_us > kMaximumEdgeLagUs) {
                failures.emplace_back(std::string(name) + " tail is missing");
            }
            if(max_gap_us > kMaximumFrameGapUs) {
                failures.emplace_back(std::string(name) + " contains a gap over 500 ms");
            }
            if(out_of_order > 0) {
                failures.emplace_back(std::string(name) + " timestamps moved backwards");
            }
        };
        inspect_stream("rgb", rgb_expected_, recording_window_valid_rgb_frames_, quality.rgb_coverage_ratio,
                       quality.rgb_first_lag_us, quality.rgb_end_lag_us, recording_window_rgb_max_gap_us_,
                       recording_window_rgb_out_of_order_);
        inspect_stream("depth", depth_expected_, recording_window_valid_depth_frames_, quality.depth_coverage_ratio,
                       quality.depth_first_lag_us, quality.depth_end_lag_us, recording_window_depth_max_gap_us_,
                       recording_window_depth_out_of_order_);
        constexpr uint64_t kMaximumRgbDepthDurationDeltaUs = 500'000;
        if(rgb_expected_ && depth_expected_ && quality.rgb_depth_duration_delta_us > kMaximumRgbDepthDurationDeltaUs) {
            failures.emplace_back("RGB/depth duration drift exceeds 500 ms");
        }
        if(!rgb_expected_ && !depth_expected_) {
            return quality;
        }
        quality.complete = failures.empty();
        quality.status = quality.complete ? "complete" : "partial";
        quality.reason = quality.complete ? "all expected streams passed coverage and continuity checks" : "";
        for(size_t i = 0; i < failures.size(); ++i) {
            if(i > 0) {
                quality.reason += "; ";
            }
            quality.reason += failures[i];
        }
        return quality;
    }

    void write_recording_quality_fields(std::ostream &out) const {
        const auto quality = recording_quality_summary();
        out << "  \"recording_quality_status\": \"" << quality.status << "\",\n";
        out << "  \"recording_complete\": " << (quality.complete ? "true" : "false") << ",\n";
        out << "  \"recording_quality_reason\": \"" << json_escape(quality.reason) << "\",\n";
        out << "  \"recording_quality_window_start_global_us\": " << quality.window_start_us << ",\n";
        out << "  \"recording_quality_window_end_global_us\": " << quality.window_end_us << ",\n";
        out << "  \"recording_quality_window_duration_us\": " << quality.window_duration_us << ",\n";
        out << "  \"rgb_stream_expected\": " << (rgb_expected_ ? "true" : "false") << ",\n";
        out << "  \"depth_stream_expected\": " << (depth_expected_ ? "true" : "false") << ",\n";
        out << "  \"rgb_coverage_ratio\": " << quality.rgb_coverage_ratio << ",\n";
        out << "  \"depth_coverage_ratio\": " << quality.depth_coverage_ratio << ",\n";
        out << "  \"rgb_media_duration_us\": " << quality.rgb_media_duration_us << ",\n";
        out << "  \"depth_media_duration_us\": " << quality.depth_media_duration_us << ",\n";
        out << "  \"rgb_depth_duration_delta_us\": " << quality.rgb_depth_duration_delta_us << ",\n";
        out << "  \"rgb_first_frame_lag_us\": " << quality.rgb_first_lag_us << ",\n";
        out << "  \"rgb_end_frame_lag_us\": " << quality.rgb_end_lag_us << ",\n";
        out << "  \"depth_first_frame_lag_us\": " << quality.depth_first_lag_us << ",\n";
        out << "  \"depth_end_frame_lag_us\": " << quality.depth_end_lag_us << ",\n";
        out << "  \"rgb_max_frame_gap_us\": " << recording_window_rgb_max_gap_us_ << ",\n";
        out << "  \"depth_max_frame_gap_us\": " << recording_window_depth_max_gap_us_ << ",\n";
        out << "  \"rgb_timestamp_out_of_order_count\": " << recording_window_rgb_out_of_order_ << ",\n";
        out << "  \"depth_timestamp_out_of_order_count\": " << recording_window_depth_out_of_order_ << ",\n";
    }

    void write_recording_ready_marker(const std::string &sender_id, const std::string &camera_id) const {
        const auto marker_path = file_path("recording_ready.json");
        const auto temporary_path = file_path("recording_ready.json.tmp");
        std::ofstream marker(temporary_path, std::ios::out | std::ios::trunc);
        if(!marker) {
            throw std::runtime_error("cannot create recording ready marker: " + temporary_path.string());
        }
        marker << "{\n";
        marker << "  \"schema\": \"gwv3_recording_ready_v1\",\n";
        marker << "  \"ready\": true,\n";
        marker << "  \"finalized_at_us\": " << now_us() << ",\n";
        marker << "  \"segment_start_us\": " << start_us_ << ",\n";
        marker << "  \"segment_end_us\": " << end_us_ << ",\n";
        marker << "  \"global_segment_index\": " << segment_timeline_.index << ",\n";
        marker << "  \"segment_window_start_global_us\": " << segment_timeline_.start_global_us << ",\n";
        marker << "  \"segment_window_end_global_us\": " << segment_timeline_.end_global_us << ",\n";
        marker << "  \"recording_session_id\": " << recording_window_.session_id << ",\n";
        marker << "  \"recording_window_start_global_us\": " << recording_window_.start_global_us << ",\n";
        marker << "  \"recording_window_end_global_us\": " << recording_window_.end_global_us << ",\n";
        marker << "  \"recording_window_first_valid_global_us\": " << recording_window_first_valid_global_us_ << ",\n";
        marker << "  \"recording_window_last_valid_global_us\": " << recording_window_last_valid_global_us_ << ",\n";
        marker << "  \"recording_window_valid_rows\": " << recording_window_valid_rows_ << ",\n";
        marker << "  \"recording_window_valid_rgb_frames\": " << recording_window_valid_rgb_frames_ << ",\n";
        marker << "  \"recording_window_valid_depth_frames\": " << recording_window_valid_depth_frames_ << ",\n";
        write_recording_quality_fields(marker);
        marker << "  \"sender_id\": \"" << json_escape(sender_id) << "\",\n";
        marker << "  \"camera_id\": \"" << json_escape(camera_id) << "\",\n";
        marker << "  \"relative_path\": \"" << json_escape(relative_directory_) << "\",\n";
        marker << "  \"frames_file\": \"" << json_escape(prefixed_filename(file_prefix_, "frames.csv")) << "\",\n";
        marker << "  \"meta_file\": \"" << json_escape(prefixed_filename(file_prefix_, "meta.json")) << "\",\n";
        marker << "  \"ready_file\": \"" << json_escape(prefixed_filename(file_prefix_, "recording_ready.json")) << "\",\n";
        marker << "  \"rgb_file\": \"" << json_escape(prefixed_filename(file_prefix_, "rgb.mp4")) << "\",\n";
        marker << "  \"depth_file\": \"" << json_escape(prefixed_filename(file_prefix_, "depth.mkv")) << "\",\n";
        marker << "  \"task_audio_ready_file\": \"audio_ready.json\",\n";
        marker << "  \"task_audio_meta_file\": \"audio_meta.json\",\n";
        marker << "  \"task_audio_timing_file\": \"audio_timing.csv\",\n";
        marker << "  \"task_audio_file\": \"audio.opus\",\n";
        marker << "  \"rgb_frame_index_mode\": \"frames_csv_rgb_recorded_columns\"\n";
        marker << "}\n";
        marker.close();
        if(!marker) {
            throw std::runtime_error("cannot finish recording ready marker: " + temporary_path.string());
        }
        std::error_code ec;
        std::filesystem::rename(temporary_path, marker_path, ec);
        if(ec) {
            throw std::runtime_error("cannot publish recording ready marker: " + ec.message());
        }
    }

    void write_recording_staged_marker(const std::string &sender_id, const std::string &camera_id) const {
        const auto marker_path = file_path("recording_staged.json");
        const auto temporary_path = file_path("recording_staged.json.tmp");
        std::ofstream marker(temporary_path, std::ios::out | std::ios::trunc);
        if(!marker) {
            throw std::runtime_error("cannot create recording staged marker: " + temporary_path.string());
        }
        marker << "{\n";
        marker << "  \"schema\": \"gwv3_recording_staged_v1\",\n";
        marker << "  \"staged\": true,\n";
        marker << "  \"staged_at_us\": " << now_us() << ",\n";
        marker << "  \"segment_start_us\": " << start_us_ << ",\n";
        marker << "  \"segment_end_us\": " << end_us_ << ",\n";
        marker << "  \"global_segment_index\": " << segment_timeline_.index << ",\n";
        marker << "  \"segment_window_start_global_us\": " << segment_timeline_.start_global_us << ",\n";
        marker << "  \"segment_window_end_global_us\": " << segment_timeline_.end_global_us << ",\n";
        marker << "  \"recording_session_id\": " << recording_window_.session_id << ",\n";
        marker << "  \"recording_window_start_global_us\": " << recording_window_.start_global_us << ",\n";
        marker << "  \"recording_window_end_global_us\": " << recording_window_.end_global_us << ",\n";
        marker << "  \"recording_window_first_valid_global_us\": " << recording_window_first_valid_global_us_ << ",\n";
        marker << "  \"recording_window_last_valid_global_us\": " << recording_window_last_valid_global_us_ << ",\n";
        marker << "  \"recording_window_valid_rows\": " << recording_window_valid_rows_ << ",\n";
        marker << "  \"recording_window_valid_rgb_frames\": " << recording_window_valid_rgb_frames_ << ",\n";
        marker << "  \"recording_window_valid_depth_frames\": " << recording_window_valid_depth_frames_ << ",\n";
        write_recording_quality_fields(marker);
        marker << "  \"sender_id\": \"" << json_escape(sender_id) << "\",\n";
        marker << "  \"camera_id\": \"" << json_escape(camera_id) << "\",\n";
        marker << "  \"relative_path\": \"" << json_escape(relative_directory_) << "\",\n";
        marker << "  \"recording_root\": \"" << json_escape(recording_root_) << "\",\n";
        marker << "  \"frames_file\": \"" << json_escape(prefixed_filename(file_prefix_, "frames.csv")) << "\",\n";
        marker << "  \"meta_file\": \"" << json_escape(prefixed_filename(file_prefix_, "meta.json")) << "\",\n";
        marker << "  \"ready_file\": \"" << json_escape(prefixed_filename(file_prefix_, "recording_ready.json")) << "\",\n";
        marker << "  \"rgb_file\": \"" << json_escape(prefixed_filename(file_prefix_, "rgb.mp4")) << "\",\n";
        marker << "  \"depth_file\": \"" << json_escape(prefixed_filename(file_prefix_, "depth.mkv")) << "\",\n";
        marker << "  \"task_audio_ready_file\": \"audio_ready.json\",\n";
        marker << "  \"task_audio_meta_file\": \"audio_meta.json\",\n";
        marker << "  \"task_audio_timing_file\": \"audio_timing.csv\",\n";
        marker << "  \"task_audio_file\": \"audio.opus\"\n";
        marker << "}\n";
        marker.close();
        if(!marker) {
            throw std::runtime_error("cannot finish recording staged marker: " + temporary_path.string());
        }
        std::error_code ec;
        std::filesystem::rename(temporary_path, marker_path, ec);
        if(ec) {
            throw std::runtime_error("cannot publish recording staged marker: " + ec.message());
        }
    }

    void trim_pending_rgb_infos_to_offset(size_t byte_offset) {
        size_t consumed = 0;
        size_t erase_count = 0;
        while(erase_count < rgb_pending_infos_.size()) {
            const size_t next = consumed + rgb_pending_infos_[erase_count].payload_size;
            if(next > byte_offset) {
                break;
            }
            consumed = next;
            ++erase_count;
        }
        if(erase_count > 0) {
            rgb_pending_infos_.erase(rgb_pending_infos_.begin(),
                                     rgb_pending_infos_.begin() + static_cast<std::ptrdiff_t>(erase_count));
        }
        if(!rgb_pending_infos_.empty() && byte_offset > consumed) {
            const size_t trimmed_from_first = byte_offset - consumed;
            auto &first = rgb_pending_infos_.front();
            if(trimmed_from_first < first.payload_size) {
                first.payload_size -= trimmed_from_first;
            }
        }
    }

    void ensure_depth_pipe(const Config &cfg, uint32_t width, uint32_t height, double fps, Logger &logger) {
        if(depth_pipe_.active()) {
            return;
        }
        depth_width_ = width;
        depth_height_ = height;
        const auto ffmpeg_log = shell_quote(file_path("ffmpeg.log").string());
        std::ostringstream part_name;
        part_name << "depth_part_" << std::setw(3) << std::setfill('0') << depth_part_index_ << ".mkv";
        const auto part_path = file_path(part_name.str());
        const auto depth_mkv = shell_quote(part_path.string());
        std::ostringstream cmd;
        cmd << shell_quote(cfg.ffmpeg_path)
            << " -hide_banner -loglevel warning -y -f rawvideo -pixel_format gray16le -video_size " << width << "x" << height
            << " -framerate " << format_fps(fps) << " -i pipe:0 -c:v ffv1 -level 3 " << depth_mkv << " 2>>" << ffmpeg_log;
        if(depth_pipe_.open(cmd.str(), logger)) {
            if(depth_part_paths_.empty() || depth_part_paths_.back() != part_path) {
                depth_part_paths_.push_back(part_path);
            }
        }
    }

    void ensure_rgb_pipe(const Config &cfg, double fps, Logger &logger) {
        if(rgb_pipe_.active() || rgb_pipe_failed_) {
            return;
        }
        const auto ffmpeg_log = shell_quote(file_path("ffmpeg.log").string());
        const auto rgb_mp4 = shell_quote(file_path("rgb.mp4").string());
        const std::string metadata_bsf = rgb_h264_full_range_
                                             ? " -bsf:v " + shell_quote(kH264FullRangeMetadataBsf)
                                             : "";
        const std::string rgb_cmd = shell_quote(cfg.ffmpeg_path) +
                                    " -hide_banner -loglevel warning -y -fflags +genpts -r " + format_fps(fps) +
                                    " -f h264 -i pipe:0 -c:v copy" + metadata_bsf + " -movflags " + kRgbMp4RecordMuxFlags +
                                    " -frag_duration " + std::to_string(kRgbMp4FragmentDurationUs) +
                                    " -flush_packets 1 " + rgb_mp4 +
                                    " 2>>" + ffmpeg_log;
        if(!rgb_pipe_.open(rgb_cmd, logger)) {
            rgb_pipe_failed_ = true;
        }
    }

    void write_rgb_packet(const Config &cfg, const MediaPacket &packet, uint64_t packet_local_us, Logger &logger) {
        if(rgb_pipe_failed_) {
            if(write_rgb_recovery_bytes(packet.payload.data(), packet.payload.size(), logger)) {
                write_rgb_recorded_frame(packet, packet_local_us, packet.payload.size());
            }
            return;
        }
        if(rgb_pipe_.active()) {
            const bool recovery_ok = write_rgb_recovery_bytes(packet.payload.data(), packet.payload.size(), logger);
            const bool pipe_ok = rgb_pipe_.write(packet.payload.data(), packet.payload.size(), logger);
            rgb_pipe_failed_ = !pipe_ok;
            if(pipe_ok || recovery_ok) {
                write_rgb_recorded_frame(packet, packet_local_us, packet.payload.size());
            }
            return;
        }

        if(rgb_pending_has_decodable_start_ && rgb_pending_.size() + packet.payload.size() > kMaxPendingRgbRecordBytes) {
            flush_rgb_pending(cfg, logger);
        }
        if(rgb_pipe_failed_) {
            if(write_rgb_recovery_bytes(packet.payload.data(), packet.payload.size(), logger)) {
                write_rgb_recorded_frame(packet, packet_local_us, packet.payload.size());
            }
            return;
        }
        if(rgb_pipe_.active()) {
            const bool recovery_ok = write_rgb_recovery_bytes(packet.payload.data(), packet.payload.size(), logger);
            const bool pipe_ok = rgb_pipe_.write(packet.payload.data(), packet.payload.size(), logger);
            rgb_pipe_failed_ = !pipe_ok;
            if(pipe_ok || recovery_ok) {
                write_rgb_recorded_frame(packet, packet_local_us, packet.payload.size());
            }
            return;
        }
        if(rgb_pending_.size() + packet.payload.size() > kMaxPendingRgbRecordBytes) {
            rgb_pending_.clear();
            rgb_pending_infos_.clear();
            rgb_pending_has_vcl_ = false;
            rgb_pending_has_decodable_start_ = false;
            rgb_fps_probe_.reset();
        }
        const bool packet_has_vcl = h264_payload_has_vcl_nal(packet.payload);
        rgb_pending_infos_.push_back(PendingRgbPacketInfo{media_packet_metadata_only(packet), packet_local_us, packet.payload.size(), packet_has_vcl});
        rgb_pending_.insert(rgb_pending_.end(), packet.payload.begin(), packet.payload.end());

        if(packet_has_vcl) {
            rgb_pending_has_vcl_ = true;
            const uint64_t fps_probe_us = packet.system_timestamp_us > 0 ? packet.system_timestamp_us : packet_local_us;
            rgb_fps_probe_.add(fps_probe_us);
        }
        if(!rgb_pending_has_decodable_start_) {
            if(const auto decodable_start = h264_decodable_start_offset(rgb_pending_)) {
                if(*decodable_start > 0) {
                    trim_pending_rgb_infos_to_offset(*decodable_start);
                    rgb_pending_.erase(rgb_pending_.begin(), rgb_pending_.begin() + static_cast<std::ptrdiff_t>(*decodable_start));
                }
                rgb_pending_has_decodable_start_ = true;
            }
        }

        const uint64_t ready_probe_us = packet.system_timestamp_us > 0 ? packet.system_timestamp_us : packet_local_us;
        if(rgb_pending_has_decodable_start_ && rgb_fps_probe_.ready(ready_probe_us)) {
            flush_rgb_pending(cfg, logger);
        }
    }

    void write_depth_packet(const Config &cfg, const MediaPacket &packet, uint64_t packet_local_us, Logger &logger) {
        if(depth_pipe_.active()) {
            if(!depth_pipe_.write(packet.payload.data(), packet.payload.size(), logger)) {
                depth_pipe_failed_ = true;
                ++depth_part_index_;
                ensure_depth_pipe(cfg, packet.width, packet.height, depth_record_fps_ > 0.0 ? depth_record_fps_ : cfg.depth_fps, logger);
                if(!depth_pipe_.active() || !depth_pipe_.write(packet.payload.data(), packet.payload.size(), logger)) {
                    throw std::runtime_error("depth ffmpeg recovery part write failed");
                }
            }
            return;
        }
        if(depth_width_ == 0 || depth_height_ == 0) {
            depth_width_ = packet.width;
            depth_height_ = packet.height;
        }
        if(!depth_pending_.empty() && depth_pending_bytes_ + packet.payload.size() > kMaxPendingDepthRecordBytes) {
            flush_depth_pending(cfg, logger);
        }
        if(depth_pipe_.active()) {
            if(!depth_pipe_.write(packet.payload.data(), packet.payload.size(), logger)) {
                depth_pipe_failed_ = true;
                ++depth_part_index_;
                ensure_depth_pipe(cfg, packet.width, packet.height, depth_record_fps_ > 0.0 ? depth_record_fps_ : cfg.depth_fps, logger);
                if(!depth_pipe_.active() || !depth_pipe_.write(packet.payload.data(), packet.payload.size(), logger)) {
                    throw std::runtime_error("depth ffmpeg recovery part write failed");
                }
            }
            return;
        }

        depth_pending_.push_back(packet.payload);
        depth_pending_bytes_ += packet.payload.size();
        const uint64_t fps_probe_us = packet.system_timestamp_us > 0 ? packet.system_timestamp_us : packet_local_us;
        depth_fps_probe_.add(fps_probe_us);
        if(depth_fps_probe_.ready(fps_probe_us)) {
            flush_depth_pending(cfg, logger);
        }
    }

    void flush_pending_media(const Config &cfg, Logger &logger) {
        flush_rgb_pending(cfg, logger);
        flush_depth_pending(cfg, logger);
    }

    void flush_rgb_pending(const Config &cfg, Logger &logger) {
        if(rgb_pipe_.active() || rgb_pending_.empty() || !rgb_pending_has_decodable_start_) {
            return;
        }
        rgb_record_fps_ = rgb_nominal_fps_ > 0.0 ? rgb_nominal_fps_ : rgb_fps_probe_.estimate(30.0);
        ensure_rgb_pipe(cfg, rgb_record_fps_, logger);
        if(rgb_pipe_failed_) {
            if(write_rgb_recovery_bytes(rgb_pending_.data(), rgb_pending_.size(), logger)) {
                write_pending_rgb_recorded_frames();
            }
            rgb_pending_.clear();
            rgb_pending_infos_.clear();
            return;
        }
        if(rgb_pipe_.active()) {
            logger.info("rgb record fps estimated: " + format_fps(rgb_record_fps_));
            const bool recovery_ok = write_rgb_recovery_bytes(rgb_pending_.data(), rgb_pending_.size(), logger);
            const bool pipe_ok = rgb_pipe_.write(rgb_pending_.data(), rgb_pending_.size(), logger);
            rgb_pipe_failed_ = !pipe_ok;
            if(pipe_ok || recovery_ok) {
                write_pending_rgb_recorded_frames();
            }
            rgb_pending_.clear();
            rgb_pending_infos_.clear();
        }
    }

    void flush_depth_pending(const Config &cfg, Logger &logger) {
        if(depth_pipe_.active() || depth_pending_.empty()) {
            return;
        }
        depth_record_fps_ = depth_nominal_fps_ > 0.0
                                ? depth_nominal_fps_
                                : depth_fps_probe_.estimate(static_cast<double>(cfg.depth_fps));
        ensure_depth_pipe(cfg, depth_width_, depth_height_, depth_record_fps_, logger);
        if(depth_pipe_.active()) {
            logger.info("depth record fps estimated: " + format_fps(depth_record_fps_));
            for(const auto &payload : depth_pending_) {
                if(!depth_pipe_.write(payload.data(), payload.size(), logger)) {
                    depth_pipe_failed_ = true;
                    ++depth_part_index_;
                    ensure_depth_pipe(cfg, depth_width_, depth_height_, depth_record_fps_, logger);
                    if(!depth_pipe_.active() || !depth_pipe_.write(payload.data(), payload.size(), logger)) {
                        throw std::runtime_error("depth ffmpeg pending recovery part write failed");
                    }
                }
            }
            depth_pending_.clear();
            depth_pending_bytes_ = 0;
        }
    }

    static double media_duration_seconds(const StreamRecordStats &stats) {
        if(stats.frames < 2 || stats.last_local_us <= stats.first_local_us) {
            return 0.0;
        }
        return static_cast<double>(stats.last_local_us - stats.first_local_us) / 1'000'000.0;
    }

    static double container_duration_seconds(uint64_t frames, double fps) {
        return frames > 0 && fps > 0.0 ? static_cast<double>(frames) / fps : 0.0;
    }

    static double media_retime_scale(double record_fps, const StreamRecordStats &stats) {
        const double actual_fps = stats.actual_fps();
        if(record_fps <= 0.0 || actual_fps <= 0.0 || stats.frames < 2) {
            return 1.0;
        }
        return record_fps / actual_fps;
    }

    static std::string ffprobe_path_from_ffmpeg(const std::string &ffmpeg_path) {
        const std::filesystem::path path(ffmpeg_path);
        if(path.filename() == "ffmpeg") {
            return (path.parent_path() / "ffprobe").string();
        }
        return "ffprobe";
    }

    static std::optional<double> probe_media_duration_seconds(const std::string &ffprobe_path, const std::filesystem::path &media_path) {
        const std::string cmd = shell_quote(ffprobe_path) +
                                " -v error -show_entries format=duration -of default=nk=1:nw=1 " + shell_quote(media_path.string()) +
                                " 2>/dev/null";
        const auto output = run_shell_capture(cmd);
        if(!output) {
            return std::nullopt;
        }
        try {
            const double duration = std::stod(trim_copy(*output));
            if(std::isfinite(duration) && duration > 0.0) {
                return duration;
            }
        }
        catch(...) {
        }
        return std::nullopt;
    }

    static bool file_size_nonzero(const std::filesystem::path &path) {
        std::error_code ec;
        return std::filesystem::exists(path, ec) && std::filesystem::is_regular_file(path, ec) &&
               std::filesystem::file_size(path, ec) > 0 && !ec;
    }

    struct Mp4TopLevelAtoms {
        bool moov = false;
        bool moof = false;
        bool sidx = false;
        bool mfra = false;
    };

    static uint32_t read_be_u32(const unsigned char *data) {
        return (static_cast<uint32_t>(data[0]) << 24u) | (static_cast<uint32_t>(data[1]) << 16u)
               | (static_cast<uint32_t>(data[2]) << 8u) | static_cast<uint32_t>(data[3]);
    }

    static uint64_t read_be_u64(const unsigned char *data) {
        uint64_t value = 0;
        for(size_t i = 0; i < 8; ++i) {
            value = (value << 8u) | static_cast<uint64_t>(data[i]);
        }
        return value;
    }

    static std::optional<Mp4TopLevelAtoms> inspect_mp4_top_level_atoms(const std::filesystem::path &path) {
        std::error_code ec;
        const auto file_size_value = std::filesystem::file_size(path, ec);
        if(ec || file_size_value < 8 || file_size_value > std::numeric_limits<uint64_t>::max()) {
            return std::nullopt;
        }
        const uint64_t file_size = static_cast<uint64_t>(file_size_value);
        std::ifstream input(path, std::ios::binary);
        if(!input) {
            return std::nullopt;
        }

        Mp4TopLevelAtoms atoms;
        uint64_t offset = 0;
        while(offset + 8 <= file_size) {
            if(offset > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max())) {
                return std::nullopt;
            }
            input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
            unsigned char header[16]{};
            input.read(reinterpret_cast<char *>(header), 8);
            if(input.gcount() != 8) {
                return std::nullopt;
            }

            uint64_t atom_size = read_be_u32(header);
            uint64_t header_size = 8;
            if(atom_size == 1) {
                input.read(reinterpret_cast<char *>(header + 8), 8);
                if(input.gcount() != 8) {
                    return std::nullopt;
                }
                atom_size = read_be_u64(header + 8);
                header_size = 16;
            }
            else if(atom_size == 0) {
                atom_size = file_size - offset;
            }
            if(atom_size < header_size || atom_size > file_size - offset) {
                return std::nullopt;
            }

            const std::string atom_type(reinterpret_cast<const char *>(header + 4), 4);
            atoms.moov = atoms.moov || atom_type == "moov";
            atoms.moof = atoms.moof || atom_type == "moof";
            atoms.sidx = atoms.sidx || atom_type == "sidx";
            atoms.mfra = atoms.mfra || atom_type == "mfra";
            offset += atom_size;
            if(offset == file_size) {
                break;
            }
        }
        if(offset != file_size) {
            return std::nullopt;
        }
        return atoms;
    }

    static bool fragmented_mp4_has_mfra_footer(std::ifstream &input, uint64_t file_size) {
        constexpr uint64_t kMfroAtomSize = 16;
        if(file_size < kMfroAtomSize
           || file_size > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max())) {
            return false;
        }

        unsigned char footer[kMfroAtomSize]{};
        input.seekg(static_cast<std::streamoff>(file_size - kMfroAtomSize), std::ios::beg);
        input.read(reinterpret_cast<char *>(footer), sizeof(footer));
        if(input.gcount() != static_cast<std::streamsize>(sizeof(footer))
           || read_be_u32(footer) != kMfroAtomSize
           || std::memcmp(footer + 4, "mfro", 4) != 0) {
            return false;
        }

        const uint64_t mfra_size = read_be_u32(footer + 12);
        if(mfra_size < 8 + kMfroAtomSize || mfra_size > file_size) {
            return false;
        }
        const uint64_t mfra_offset = file_size - mfra_size;
        unsigned char header[16]{};
        input.seekg(static_cast<std::streamoff>(mfra_offset), std::ios::beg);
        input.read(reinterpret_cast<char *>(header), 8);
        if(input.gcount() != 8 || std::memcmp(header + 4, "mfra", 4) != 0) {
            return false;
        }
        uint64_t header_size = 8;
        uint64_t atom_size = read_be_u32(header);
        if(atom_size == 1) {
            input.read(reinterpret_cast<char *>(header + 8), 8);
            if(input.gcount() != 8) {
                return false;
            }
            atom_size = read_be_u64(header + 8);
            header_size = 16;
        }
        return atom_size >= header_size && atom_size == mfra_size;
    }

    static bool fragmented_mp4_staging_sealed(const std::filesystem::path &path) {
        std::error_code ec;
        const auto file_size_value = std::filesystem::file_size(path, ec);
        if(ec || file_size_value < 8 || file_size_value > std::numeric_limits<uint64_t>::max()) {
            return false;
        }
        const uint64_t file_size = static_cast<uint64_t>(file_size_value);
        std::ifstream input(path, std::ios::binary);
        if(!input) {
            return false;
        }

        bool saw_moov = false;
        bool saw_moof = false;
        uint64_t offset = 0;
        constexpr size_t kMaxHeaderAtoms = 16;
        for(size_t i = 0; i < kMaxHeaderAtoms && offset + 8 <= file_size; ++i) {
            unsigned char header[16]{};
            input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
            input.read(reinterpret_cast<char *>(header), 8);
            if(input.gcount() != 8) {
                return false;
            }

            uint64_t atom_size = read_be_u32(header);
            uint64_t header_size = 8;
            if(atom_size == 1) {
                input.read(reinterpret_cast<char *>(header + 8), 8);
                if(input.gcount() != 8) {
                    return false;
                }
                atom_size = read_be_u64(header + 8);
                header_size = 16;
            }
            else if(atom_size == 0) {
                atom_size = file_size - offset;
            }
            if(atom_size < header_size || atom_size > file_size - offset) {
                return false;
            }

            const std::string atom_type(reinterpret_cast<const char *>(header + 4), 4);
            saw_moov = saw_moov || atom_type == "moov";
            saw_moof = saw_moof || atom_type == "moof";
            if(saw_moov && saw_moof) {
                break;
            }
            offset += atom_size;
        }

        // Receiver staging MP4 files are fragmented. A normal close writes an
        // mfro footer whose size points back to the final mfra atom. Checking
        // that envelope is constant-time; the uploader performs the full scan.
        return saw_moov && saw_moof && fragmented_mp4_has_mfra_footer(input, file_size);
    }

    static bool has_matroska_ebml_header(const std::filesystem::path &path) {
        std::ifstream input(path, std::ios::binary);
        unsigned char header[4]{};
        input.read(reinterpret_cast<char *>(header), sizeof(header));
        return input.gcount() == static_cast<std::streamsize>(sizeof(header))
               && header[0] == 0x1a && header[1] == 0x45 && header[2] == 0xdf && header[3] == 0xa3;
    }

    static bool media_file_structurally_complete(const std::filesystem::path &media_path) {
        if(!file_size_nonzero(media_path)) {
            return false;
        }
        const auto extension = media_path.extension().string();
        if(extension == ".mp4") {
            const auto atoms = inspect_mp4_top_level_atoms(media_path);
            return atoms && atoms->moov && (!atoms->moof || atoms->mfra);
        }
        if(extension == ".mkv") {
            return has_matroska_ebml_header(media_path);
        }
        return true;
    }

    static bool media_file_staging_sealed(const std::filesystem::path &media_path) {
        if(!file_size_nonzero(media_path)) {
            return false;
        }
        const auto extension = media_path.extension().string();
        if(extension == ".mp4") {
            return fragmented_mp4_staging_sealed(media_path);
        }
        if(extension == ".mkv") {
            return has_matroska_ebml_header(media_path);
        }
        return true;
    }

    static bool media_file_readable(const std::string &ffprobe_path, const std::filesystem::path &media_path) {
        if(!media_file_structurally_complete(media_path)) {
            return false;
        }
        if(media_path.extension().string() == ".mp4") {
            const auto atoms = inspect_mp4_top_level_atoms(media_path);
            if(atoms && atoms->moof) {
                // A normally closed fragmented MP4 has mfra at the end. Avoid
                // ffprobe here: without global_sidx it scans the entire file,
                // serializing multi-camera finalization for minutes.
                return true;
            }
        }
        return probe_media_duration_seconds(ffprobe_path, media_path).has_value();
    }

    static void append_retime_log(const std::filesystem::path &log_path, const std::string &message) {
        std::ofstream log(log_path, std::ios::out | std::ios::app);
        if(log) {
            log << timestamp_text() << " [retime] " << message << '\n';
        }
    }

    static void set_file_mtime_to_start(const std::filesystem::path &path, uint64_t start_us) {
        if(start_us == 0 || path.empty()) {
            return;
        }
        timespec times[2]{};
        times[0].tv_sec = static_cast<time_t>(start_us / 1'000'000ull);
        times[0].tv_nsec = static_cast<long>((start_us % 1'000'000ull) * 1000ull);
        times[1] = times[0];
        utimensat(AT_FDCWD, path.c_str(), times, 0);
    }

    static std::string ffconcat_quote(const std::string &value) {
        std::string out;
        out.reserve(value.size() + 8);
        for(char ch : value) {
            if(ch == '\\' || ch == '\'') {
                out.push_back('\\');
            }
            out.push_back(ch);
        }
        return out;
    }

    bool replace_with_valid_media(const std::filesystem::path &temporary,
                                  const std::filesystem::path &destination,
                                  const std::string &ffprobe_path,
                                  Logger &logger) const {
        if(!media_file_readable(ffprobe_path, temporary)) {
            logger.warn("recovered media validation failed: " + temporary.string());
            return false;
        }
        std::error_code ec;
        std::filesystem::rename(temporary, destination, ec);
        if(ec) {
            logger.warn("cannot replace recovered media " + destination.string() + ": " + ec.message());
            return false;
        }
        return true;
    }

    bool finalize_rgb_player_compatible(const Config &cfg, const std::string &ffprobe_path, Logger &logger) const {
        const auto destination = file_path("rgb.mp4");
        const auto source_atoms = inspect_mp4_top_level_atoms(destination);
        if(!source_atoms || !source_atoms->moov) {
            return false;
        }
        if(!source_atoms->moof) {
            return true;
        }

        const auto source_duration = probe_media_duration_seconds(ffprobe_path, destination);
        const auto temporary = file_path("rgb_seekable.tmp.mp4");
        const auto log_path = file_path("ffmpeg.log");
        std::error_code ec;
        std::filesystem::remove(temporary, ec);

        // Keep the crash-tolerant fragmented MP4 in place until a conventional
        // MP4 has been completely written and validated in the same directory.
        const std::string command = shell_quote(cfg.ffmpeg_path)
                                    + " -hide_banner -loglevel warning -y -i " + shell_quote(destination.string())
                                    + " -map 0:v:0 -c:v copy " + shell_quote(temporary.string())
                                    + " 2>>" + shell_quote(log_path.string());
        const int rc = run_shell_command(command);
        if(rc != 0) {
            append_retime_log(log_path, "rgb player-compatible remux failed: " + process_status_text(rc));
            std::filesystem::remove(temporary, ec);
            return false;
        }

        const auto temporary_atoms = inspect_mp4_top_level_atoms(temporary);
        const auto temporary_duration = probe_media_duration_seconds(ffprobe_path, temporary);
        bool valid = temporary_atoms && temporary_atoms->moov && !temporary_atoms->moof && temporary_duration.has_value();
        if(valid && source_duration) {
            const double tolerance = std::max(0.25, *source_duration * 0.001);
            valid = std::fabs(*temporary_duration - *source_duration) <= tolerance;
        }
        if(!valid) {
            append_retime_log(log_path, "rgb player-compatible remux validation failed");
            std::filesystem::remove(temporary, ec);
            return false;
        }

        std::filesystem::rename(temporary, destination, ec);
        if(ec) {
            append_retime_log(log_path, "rgb player-compatible publish failed: " + ec.message());
            std::filesystem::remove(temporary, ec);
            return false;
        }
        append_retime_log(log_path, "rgb player-compatible MP4 published atomically");
        logger.info("RGB recording finalized as conventional seekable MP4: " + destination.string());
        return true;
    }

    bool rebuild_rgb_from_recovery(const Config &cfg, const std::string &ffprobe_path, Logger &logger) const {
        if(!file_size_nonzero(rgb_debug_path_)) {
            return false;
        }
        const auto temporary = file_path("rgb_recovered.tmp.mp4");
        std::error_code ec;
        std::filesystem::remove(temporary, ec);
        const double fps = rgb_record_fps_ > 0.0 ? rgb_record_fps_ : 30.0;
        const std::string metadata_bsf = rgb_h264_full_range_
                                             ? " -bsf:v " + shell_quote(kH264FullRangeMetadataBsf)
                                             : "";
        const std::string command = shell_quote(cfg.ffmpeg_path)
                                    + " -hide_banner -loglevel warning -y -fflags +genpts -r " + format_fps(fps)
                                    + " -f h264 -i " + shell_quote(rgb_debug_path_.string())
                                    + " -c:v copy" + metadata_bsf + " -movflags " + kRgbMp4RecordMuxFlags
                                    + " -frag_duration " + std::to_string(kRgbMp4FragmentDurationUs) + " -flush_packets 1 "
                                    + shell_quote(temporary.string()) + " 2>>" + shell_quote(file_path("ffmpeg.log").string());
        if(run_shell_command(command) != 0) {
            logger.warn("RGB automatic recovery remux failed: " + directory_);
            std::filesystem::remove(temporary, ec);
            return false;
        }
        if(!replace_with_valid_media(temporary, file_path("rgb.mp4"), ffprobe_path, logger)) {
            std::filesystem::remove(temporary, ec);
            return false;
        }
        logger.info("RGB recording rebuilt from recovery stream: " + file_path("rgb.mp4").string());
        return true;
    }

    bool finalize_depth_parts(const Config &cfg, const std::string &ffprobe_path,
                              bool defer_full_validation, Logger &logger) const {
        std::vector<std::filesystem::path> parts;
        for(const auto &path : depth_part_paths_) {
            const bool valid = defer_full_validation
                                   ? media_file_staging_sealed(path)
                                   : media_file_readable(ffprobe_path, path);
            if(valid) {
                parts.push_back(path);
            }
            else if(file_size_nonzero(path)) {
                logger.warn("invalid depth part retained for offline inspection: " + path.string());
            }
        }
        if(parts.empty()) {
            return false;
        }

        const auto destination = file_path("depth.mkv");
        std::error_code ec;
        if(parts.size() == 1) {
            std::filesystem::rename(parts.front(), destination, ec);
            if(ec) {
                logger.warn("cannot finalize depth recording: " + ec.message());
                return false;
            }
            return true;
        }

        const auto list_path = file_path("depth_parts.concat.txt");
        const auto temporary = file_path("depth_recovered.tmp.mkv");
        std::ofstream list(list_path, std::ios::out | std::ios::trunc);
        if(!list) {
            logger.warn("cannot create depth concat list: " + list_path.string());
            return false;
        }
        for(const auto &part : parts) {
            list << "file '" << ffconcat_quote(part.string()) << "'\n";
        }
        list.close();
        if(!list) {
            logger.warn("cannot finish depth concat list: " + list_path.string());
            return false;
        }
        std::filesystem::remove(temporary, ec);
        const std::string command = shell_quote(cfg.ffmpeg_path)
                                    + " -hide_banner -loglevel warning -y -f concat -safe 0 -i " + shell_quote(list_path.string())
                                    + " -c copy " + shell_quote(temporary.string()) + " 2>>" + shell_quote(file_path("ffmpeg.log").string());
        const int rc = run_shell_command(command);
        std::filesystem::remove(list_path, ec);
        if(rc != 0 || !replace_with_valid_media(temporary, destination, ffprobe_path, logger)) {
            std::filesystem::remove(temporary, ec);
            logger.warn("depth part concatenation failed; part files retained: " + directory_);
            return false;
        }
        for(const auto &part : parts) {
            std::filesystem::remove(part, ec);
        }
        logger.info("depth recording finalized from " + std::to_string(parts.size()) + " parts: " + destination.string());
        return true;
    }

    void finalize_completed_media(const Config &cfg, Logger &logger) const {
        const auto ffprobe_path = ffprobe_path_from_ffmpeg(cfg.ffmpeg_path);
        const auto log_path = file_path("ffmpeg.log");
        bool checked_any_media = false;
        const bool defer_full_validation = cfg.recording_staging.enabled
                                           && cfg.recording_staging.defer_player_compatible_finalize;
        const bool preserve_fragmented_rgb =
            cfg.recording_staging.rgb_output_mode == "fragmented_mp4";

        const auto validate_media = [&](const std::filesystem::path &path, const std::string &stream_name) -> bool {
            if(!file_size_nonzero(path)) {
                append_retime_log(log_path, stream_name + " validation skipped: media file missing or empty");
                return false;
            }
            checked_any_media = true;
            const bool ok = defer_full_validation
                                ? media_file_staging_sealed(path)
                                : media_file_readable(ffprobe_path, path);
            append_retime_log(
                log_path,
                stream_name + (ok ? " final validation ok" : " final validation failed")
                    + (defer_full_validation ? " (full probe deferred)" : ""));
            set_file_mtime_to_start(path, start_us_);
            return ok;
        };

        bool rgb_ok = validate_media(file_path("rgb.mp4"), "rgb");
        if((rgb_pipe_failed_ || !rgb_ok) && rebuild_rgb_from_recovery(cfg, ffprobe_path, logger)) {
            rgb_ok = validate_media(file_path("rgb.mp4"), "rgb_recovered");
        }
        if(rgb_ok && !defer_full_validation && !preserve_fragmented_rgb) {
            rgb_ok = finalize_rgb_player_compatible(cfg, ffprobe_path, logger);
            if(rgb_ok) {
                rgb_ok = validate_media(file_path("rgb.mp4"), "rgb_compatible");
            }
        }
        else if(rgb_ok && defer_full_validation && !preserve_fragmented_rgb) {
            append_retime_log(log_path, "rgb player-compatible remux deferred to recording uploader");
        }
        else if(rgb_ok && preserve_fragmented_rgb) {
            append_retime_log(log_path, "rgb fragmented MP4 retained for final delivery");
        }
        if(rgb_ok && !cfg.write_debug_h264 && !rgb_debug_path_.empty()) {
            std::error_code ec;
            if(std::filesystem::exists(rgb_debug_path_, ec)) {
                std::filesystem::remove(rgb_debug_path_, ec);
                if(ec) {
                    append_retime_log(log_path, "rgb recovery h264 remove failed: " + ec.message());
                }
                else {
                    append_retime_log(log_path, "rgb recovery h264 removed after final validation");
                }
            }
        }
        else if(!rgb_ok && !rgb_debug_path_.empty()) {
            append_retime_log(log_path, "rgb recovery h264 kept for offline repair");
        }

        finalize_depth_parts(cfg, ffprobe_path, defer_full_validation, logger);
        validate_media(file_path("depth.mkv"), "depth");
        if(checked_any_media) {
            append_retime_log(log_path, "recording media finalization completed");
            set_file_mtime_to_start(log_path, start_us_);
        }
        if(!rgb_ok && (file_size_nonzero(file_path("rgb.mp4")) || file_size_nonzero(rgb_debug_path_))) {
            throw std::runtime_error("RGB recording could not be finalized: " + directory_);
        }
    }

    void write_meta(const Config &cfg, const std::string &sender_id, const std::string &camera_id, const std::string &announce_json, bool closed) {
        if(directory_.empty()) {
            return;
        }
        write_calibration(sender_id, camera_id, announce_json, closed);
        const int rgb_requested_fps = json_int_in_object(announce_json, "rgb_profile", "fps").value_or(30);
        const int depth_requested_fps = json_int_in_object(announce_json, "depth_profile", "fps").value_or(cfg.depth_fps);
        const auto &rgb_output_stats = rgb_recorded_stats_.frames > 0 ? rgb_recorded_stats_ : rgb_stats_;
        const std::string rgb_codec = !rgb_output_stats.codec_or_compression.empty()
                                          ? rgb_output_stats.codec_or_compression
                                          : json_string_in_object(announce_json, "rgb_profile", "codec").value_or("h264");
        const uint32_t rgb_width =
            rgb_output_stats.width > 0 ? rgb_output_stats.width
                                       : static_cast<uint32_t>(json_int_in_object(announce_json, "rgb_profile", "width").value_or(0));
        const uint32_t rgb_height =
            rgb_output_stats.height > 0 ? rgb_output_stats.height
                                        : static_cast<uint32_t>(json_int_in_object(announce_json, "rgb_profile", "height").value_or(0));
        std::ofstream meta(file_path("meta.json"), std::ios::out | std::ios::trunc);
        meta << "{\n";
        meta << "  \"sender_id\": \"" << json_escape(sender_id) << "\",\n";
        meta << "  \"camera_id\": \"" << json_escape(camera_id) << "\",\n";
        meta << "  \"camera_key\": \"" << json_escape(camera_key(sender_id, camera_id)) << "\",\n";
        meta << "  \"rgb_h264_full_range\": " << (rgb_h264_full_range_ ? "true" : "false") << ",\n";
        meta << "  \"camera_name\": \"" << json_escape(camera_name_) << "\",\n";
        meta << "  \"storage_key\": \"" << json_escape(storage_key_) << "\",\n";
        meta << "  \"file_prefix\": \"" << json_escape(file_prefix_) << "\",\n";
        meta << "  \"segment_start_us\": " << start_us_ << ",\n";
        meta << "  \"segment_end_us\": " << (closed ? end_us_ : 0) << ",\n";
        meta << "  \"global_segment_index\": " << segment_timeline_.index << ",\n";
        meta << "  \"segment_window_start_global_us\": " << segment_timeline_.start_global_us << ",\n";
        meta << "  \"segment_window_end_global_us\": " << segment_timeline_.end_global_us << ",\n";
        meta << "  \"segment_timeline_mode\": \"recording_session_global_timestamp_us\",\n";
        meta << "  \"recording_session_id\": " << recording_window_.session_id << ",\n";
        meta << "  \"recording_window_start_global_us\": " << recording_window_.start_global_us << ",\n";
        meta << "  \"recording_window_end_global_us\": "
             << (closed ? recording_window_.end_global_us : 0) << ",\n";
        meta << "  \"recording_window_first_valid_global_us\": "
             << (closed ? recording_window_first_valid_global_us_ : 0) << ",\n";
        meta << "  \"recording_window_last_valid_global_us\": "
             << (closed ? recording_window_last_valid_global_us_ : 0) << ",\n";
        meta << "  \"recording_window_first_valid_rgb_global_us\": "
             << (closed ? recording_window_first_valid_rgb_global_us_ : 0) << ",\n";
        meta << "  \"recording_window_last_valid_rgb_global_us\": "
             << (closed ? recording_window_last_valid_rgb_global_us_ : 0) << ",\n";
        meta << "  \"recording_window_first_valid_depth_global_us\": "
             << (closed ? recording_window_first_valid_depth_global_us_ : 0) << ",\n";
        meta << "  \"recording_window_last_valid_depth_global_us\": "
             << (closed ? recording_window_last_valid_depth_global_us_ : 0) << ",\n";
        meta << "  \"recording_window_valid_rows\": " << (closed ? recording_window_valid_rows_ : 0) << ",\n";
        meta << "  \"recording_window_valid_rgb_frames\": " << (closed ? recording_window_valid_rgb_frames_ : 0) << ",\n";
        meta << "  \"recording_window_valid_depth_frames\": " << (closed ? recording_window_valid_depth_frames_ : 0) << ",\n";
        if(closed) {
            write_recording_quality_fields(meta);
        }
        else {
            meta << "  \"recording_quality_status\": \"recording\",\n";
            meta << "  \"recording_complete\": false,\n";
        }
        meta << "  \"recording_window_mode\": \"global_timestamp_us\",\n";
        meta << "  \"closed\": " << (closed ? "true" : "false") << ",\n";
        meta << "  \"frames_publish_state\": \"" << (closed ? "finalized" : "recording") << "\",\n";
        meta << "  \"recording_storage_mode\": \""
             << (cfg.recording_staging.enabled ? "local_staging" : "direct_nas") << "\",\n";
        meta << "  \"recording_relative_path\": \"" << json_escape(relative_directory_) << "\",\n";
        meta << "  \"nas_publish_root\": \"" << json_escape(cfg.nas_root) << "\",\n";
        meta << "  \"rgb_output_mode\": \"" << json_escape(cfg.recording_staging.rgb_output_mode) << "\",\n";
        meta << "  \"player_compatible_finalize_deferred\": "
             << (cfg.recording_staging.enabled
                     && cfg.recording_staging.defer_player_compatible_finalize
                     && cfg.recording_staging.rgb_output_mode == "conventional_mp4"
                     ? "true"
                     : "false")
             << ",\n";
        meta << "  \"recording_ready_file\": \""
             << json_escape(prefixed_filename(file_prefix_, "recording_ready.json")) << "\",\n";
        meta << "  \"rgb_file\": \"" << json_escape(prefixed_filename(file_prefix_, "rgb.mp4")) << "\",\n";
        meta << "  \"rgb_debug_file\": \"" << json_escape(prefixed_filename(file_prefix_, "rgb_debug.h264")) << "\",\n";
        meta << "  \"rgb_frame_index_file\": \"" << json_escape(prefixed_filename(file_prefix_, "frames.csv")) << "\",\n";
        meta << "  \"rgb_frame_index_mode\": \"frames_csv_rgb_recorded_columns\",\n";
        meta << "  \"depth_file\": \"" << json_escape(prefixed_filename(file_prefix_, "depth.mkv")) << "\",\n";
        meta << "  \"task_audio_ready_file\": \"audio_ready.json\",\n";
        meta << "  \"task_audio_meta_file\": \"audio_meta.json\",\n";
        meta << "  \"task_audio_timing_file\": \"audio_timing.csv\",\n";
        meta << "  \"task_audio_file\": \"audio.opus\",\n";
        meta << "  \"depth_debug_file\": \"" << json_escape(prefixed_filename(file_prefix_, "depth_debug.raw")) << "\",\n";
        meta << "  \"frames_file\": \"" << json_escape(prefixed_filename(file_prefix_, "frames.csv")) << "\",\n";
        meta << "  \"calibration_file\": \"" << json_escape(prefixed_filename(file_prefix_, "calibration.json")) << "\",\n";
        meta << "  \"ffmpeg_log_file\": \"" << json_escape(prefixed_filename(file_prefix_, "ffmpeg.log")) << "\",\n";
        meta << "  \"rgb_codec\": \"" << json_escape(rgb_codec) << "\",\n";
        meta << "  \"rgb_width\": " << rgb_width << ",\n";
        meta << "  \"rgb_height\": " << rgb_height << ",\n";
        meta << "  \"rgb_fps\": " << rgb_requested_fps << ",\n";
        meta << "  \"rgb_actual_fps\": " << format_fps(rgb_output_stats.actual_fps()) << ",\n";
        meta << "  \"rgb_frames\": " << rgb_output_stats.frames << ",\n";
        meta << "  \"depth_codec\": \"ffv1\",\n";
        meta << "  \"depth_pixel_format\": \"gray16le\",\n";
        meta << "  \"depth_dtype\": \"uint16le\",\n";
        meta << "  \"depth_fps\": " << depth_requested_fps << ",\n";
        meta << "  \"depth_actual_fps\": " << format_fps(depth_stats_.actual_fps()) << ",\n";
        meta << "  \"depth_frames\": " << depth_stats_.frames << ",\n";
        meta << "  \"depth_format\": \"ffv1_mkv_gray16le\",\n";
        meta << "  \"depth_width\": " << depth_width_ << ",\n";
        meta << "  \"depth_height\": " << depth_height_ << ",\n";
        meta << "  \"rgb_record_fps\": " << format_fps(rgb_record_fps_) << ",\n";
        meta << "  \"rgb_playback_fps\": " << format_fps(rgb_output_stats.actual_fps()) << ",\n";
        meta << "  \"rgb_target_duration_sec\": " << format_fps(media_duration_seconds(rgb_output_stats)) << ",\n";
        meta << "  \"rgb_container_expected_duration_sec\": "
             << format_fps(container_duration_seconds(rgb_output_stats.frames, rgb_record_fps_)) << ",\n";
        meta << "  \"rgb_retime_scale\": " << format_fps(media_retime_scale(rgb_record_fps_, rgb_output_stats)) << ",\n";
        meta << "  \"depth_record_fps\": " << format_fps(depth_record_fps_) << ",\n";
        meta << "  \"depth_playback_fps\": " << format_fps(depth_stats_.actual_fps()) << ",\n";
        meta << "  \"depth_target_duration_sec\": " << format_fps(media_duration_seconds(depth_stats_)) << ",\n";
        meta << "  \"depth_container_expected_duration_sec\": "
             << format_fps(container_duration_seconds(depth_stats_.frames, depth_record_fps_)) << ",\n";
        meta << "  \"depth_retime_scale\": " << format_fps(media_retime_scale(depth_record_fps_, depth_stats_)) << ",\n";
        meta << "  \"write_debug_h264\": " << (cfg.write_debug_h264 ? "true" : "false") << ",\n";
        meta << "  \"write_debug_depth_raw\": " << (cfg.write_debug_depth_raw ? "true" : "false") << ",\n";
        meta << "  \"camera_announce_raw\": \"" << json_escape(announce_json) << "\"\n";
        meta << "}\n";
        meta.close();
        if(!meta) {
            throw std::runtime_error("cannot finish recording metadata: " + file_path("meta.json").string());
        }
    }

    void write_calibration(const std::string &sender_id, const std::string &camera_id, const std::string &announce_json, bool closed) const {
        if(directory_.empty()) {
            return;
        }
        const auto announce_timestamp_us = json_uint64_field(announce_json, "timestamp_us");
        const std::string device = json_object_or_empty(announce_json, "device");
        const std::string rgb_profile = json_object_or_empty(announce_json, "rgb_profile");
        const std::string depth_profile = json_object_or_empty(announce_json, "depth_profile");
        const std::string calibration = json_object_field(announce_json, "calibration")
                                            .value_or("{\"available\":false,\"source\":\"missing_camera_announce\",\"data\":{}}");

        std::ofstream out(file_path("calibration.json"), std::ios::out | std::ios::trunc);
        out << "{\n";
        out << "  \"schema\": \"gemini_calibration_v1\",\n";
        out << "  \"sender_id\": \"" << json_escape(sender_id) << "\",\n";
        out << "  \"camera_id\": \"" << json_escape(camera_id) << "\",\n";
        out << "  \"camera_key\": \"" << json_escape(camera_key(sender_id, camera_id)) << "\",\n";
        out << "  \"camera_name\": \"" << json_escape(camera_name_) << "\",\n";
        out << "  \"storage_key\": \"" << json_escape(storage_key_) << "\",\n";
        out << "  \"file_prefix\": \"" << json_escape(file_prefix_) << "\",\n";
        out << "  \"segment_start_us\": " << start_us_ << ",\n";
        out << "  \"segment_end_us\": " << (closed ? end_us_ : 0) << ",\n";
        out << "  \"global_segment_index\": " << segment_timeline_.index << ",\n";
        out << "  \"segment_window_start_global_us\": " << segment_timeline_.start_global_us << ",\n";
        out << "  \"segment_window_end_global_us\": " << segment_timeline_.end_global_us << ",\n";
        out << "  \"recording_session_id\": " << recording_window_.session_id << ",\n";
        out << "  \"recording_window_start_global_us\": " << recording_window_.start_global_us << ",\n";
        out << "  \"recording_window_end_global_us\": "
            << (closed ? recording_window_.end_global_us : 0) << ",\n";
        out << "  \"closed\": " << (closed ? "true" : "false") << ",\n";
        if(announce_timestamp_us) {
            out << "  \"announce_timestamp_us\": " << *announce_timestamp_us << ",\n";
        }
        else {
            out << "  \"announce_timestamp_us\": 0,\n";
        }
        out << "  \"alignment_mode\": \"raw_depth_to_rgb_offline\",\n";
        out << "  \"depth_master\": \"depth_raw\",\n";
        out << "  \"aligned_depth\": {\n";
        out << "    \"generated\": false,\n";
        out << "    \"target\": \"rgb\",\n";
        out << "    \"output_file\": \"" << json_escape(prefixed_filename(file_prefix_, "depth_aligned_to_rgb.mkv")) << "\",\n";
        out << "    \"method\": \"offline\"\n";
        out << "  },\n";
        out << "  \"device\": " << device << ",\n";
        out << "  \"rgb_profile\": " << rgb_profile << ",\n";
        out << "  \"depth_profile\": " << depth_profile << ",\n";
        out << "  \"calibration\": " << calibration << ",\n";
        out << "  \"camera_announce_raw\": \"" << json_escape(announce_json) << "\"\n";
        out << "}\n";
    }

    void publish_direct_nas_segment(const Config &cfg, Logger &logger) {
        const auto source = std::filesystem::path(directory_);
        const auto hidden_root = direct_recording_root(cfg).lexically_normal();
        const auto publish_root = std::filesystem::path(cfg.nas_root).lexically_normal();
        const auto relative = source.lexically_relative(hidden_root);
        if(relative.empty() || relative == ".." || relative.generic_string().rfind("../", 0) == 0) {
            throw std::runtime_error("direct NAS segment escaped hidden root: " + source.string());
        }
        const auto destination = publish_root / relative;
        std::error_code ec;
        if(std::filesystem::exists(destination, ec) || ec) {
            throw std::runtime_error(
                ec ? "cannot inspect direct NAS destination " + destination.string() + ": " + ec.message()
                   : "direct NAS destination already exists: " + destination.string());
        }
        std::filesystem::create_directories(destination.parent_path(), ec);
        if(ec) {
            throw std::runtime_error("cannot create direct NAS destination parent "
                                     + destination.parent_path().string() + ": " + ec.message());
        }

        fsync_segment_files_strict(source);
        fsync_directory_best_effort(source.parent_path());
        constexpr unsigned kDirectPublishRenameAttempts = 20;
        for(unsigned attempt = 0; attempt < kDirectPublishRenameAttempts; ++attempt) {
            ec.clear();
            std::filesystem::rename(source, destination, ec);
            if(!ec) {
                break;
            }
            const bool transient = ec == std::errc::permission_denied
                                   || ec == std::errc::device_or_resource_busy
                                   || ec == std::errc::resource_unavailable_try_again;
            if(!transient || attempt + 1 == kDirectPublishRenameAttempts) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if(ec) {
            throw std::runtime_error("cannot atomically publish direct NAS segment " + source.string()
                                     + " -> " + destination.string() + ": " + ec.message());
        }
        directory_ = destination.string();
        recording_root_ = publish_root.string();
        set_segment_mtime_to_start(logger);
        fsync_directory_best_effort(destination);
        fsync_directory_best_effort(destination.parent_path());
        fsync_directory_best_effort(source.parent_path());
        logger.info("direct NAS segment atomically published: " + destination.string());
    }

    void set_segment_mtime_to_start(Logger &logger) const {
        if(directory_.empty() || start_us_ == 0) {
            return;
        }

        std::vector<std::filesystem::path> paths;
        std::error_code ec;
        for(const auto &entry : std::filesystem::directory_iterator(directory_, ec)) {
            paths.push_back(entry.path());
        }
        if(ec) {
            logger.warn("cannot enumerate segment files for mtime update: " + directory_ + ": " + ec.message());
            return;
        }
        paths.emplace_back(directory_);
        const auto segment_dir = std::filesystem::path(directory_);
        const auto date_dir = segment_dir.parent_path();
        const auto camera_dir = date_dir.parent_path();
        if(!date_dir.empty()) {
            paths.emplace_back(date_dir);
        }
        if(!camera_dir.empty()) {
            paths.emplace_back(camera_dir);
        }

        const uint64_t mtime_us = segment_timeline_.start_global_us > 0
                                      ? segment_timeline_.start_global_us
                                      : start_us_;
        timespec times[2]{};
        times[0].tv_sec = static_cast<time_t>(mtime_us / 1'000'000ull);
        times[0].tv_nsec = static_cast<long>((mtime_us % 1'000'000ull) * 1000ull);
        times[1] = times[0];

        for(const auto &path : paths) {
            if(utimensat(AT_FDCWD, path.c_str(), times, 0) != 0) {
                logger.warn("cannot set segment mtime to start: " + path.string() + ": " + std::strerror(errno));
            }
        }

    }

    bool active_ = false;
    uint64_t start_us_ = 0;
    uint64_t end_us_ = 0;
    RecordingWindow recording_window_;
    RecordingSegmentTimeline segment_timeline_;
    std::string directory_;
    std::string recording_root_;
    std::string relative_directory_;
    std::string camera_name_;
    std::string storage_key_;
    std::string file_prefix_;
    bool rgb_h264_full_range_ = false;
    std::ofstream frames_csv_;
    std::ofstream rgb_recorded_frames_csv_;
    std::ofstream rgb_debug_;
    std::filesystem::path rgb_debug_path_;
    std::ofstream depth_debug_;
    FfmpegPipe rgb_pipe_;
    FfmpegPipe depth_pipe_;
    bool rgb_pipe_failed_ = false;
    bool depth_pipe_failed_ = false;
    unsigned depth_part_index_ = 0;
    std::vector<std::filesystem::path> depth_part_paths_;
    unsigned csv_rows_since_flush_ = 0;
    unsigned storage_check_packets_ = 0;
    bool storage_failed_ = false;
    uint64_t recording_window_valid_rows_ = 0;
    uint64_t recording_window_valid_rgb_frames_ = 0;
    uint64_t recording_window_valid_depth_frames_ = 0;
    uint64_t recording_window_first_valid_global_us_ = 0;
    uint64_t recording_window_last_valid_global_us_ = 0;
    uint64_t recording_window_first_valid_rgb_global_us_ = 0;
    uint64_t recording_window_last_valid_rgb_global_us_ = 0;
    uint64_t recording_window_first_valid_depth_global_us_ = 0;
    uint64_t recording_window_last_valid_depth_global_us_ = 0;
    uint64_t recording_window_rgb_max_gap_us_ = 0;
    uint64_t recording_window_depth_max_gap_us_ = 0;
    uint64_t recording_window_rgb_out_of_order_ = 0;
    uint64_t recording_window_depth_out_of_order_ = 0;
    std::vector<uint8_t> rgb_pending_;
    std::vector<PendingRgbPacketInfo> rgb_pending_infos_;
    bool rgb_pending_has_vcl_ = false;
    bool rgb_pending_has_decodable_start_ = false;
    uint64_t rgb_recorded_frame_index_ = 0;
    std::vector<std::vector<uint8_t>> depth_pending_;
    size_t depth_pending_bytes_ = 0;
    uint32_t depth_width_ = 0;
    uint32_t depth_height_ = 0;
    FpsProbe rgb_fps_probe_;
    FpsProbe depth_fps_probe_;
    double rgb_record_fps_ = 0.0;
    double depth_record_fps_ = 0.0;
    double rgb_nominal_fps_ = 0.0;
    double depth_nominal_fps_ = 0.0;
    bool rgb_expected_ = false;
    bool depth_expected_ = false;
    StreamRecordStats rgb_stats_;
    StreamRecordStats rgb_recorded_stats_;
    StreamRecordStats depth_stats_;
    FrameInfo last_rgb_;
    FrameInfo last_depth_;
    std::optional<int64_t> last_rgb_frame_interval_us_;
};

