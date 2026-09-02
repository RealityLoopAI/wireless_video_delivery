class RgbPreviewDecoder {
public:
    RgbPreviewDecoder() = default;
    RgbPreviewDecoder(const RgbPreviewDecoder &) = delete;
    RgbPreviewDecoder &operator=(const RgbPreviewDecoder &) = delete;

    ~RgbPreviewDecoder() {
        stop();
    }

    bool start(const Config &cfg,
               const std::string &key,
               uint32_t width,
               uint32_t height,
               uint32_t target_width,
               uint32_t preview_fps,
               bool h264_full_range,
               Logger &logger) {
        stop();
        key_ = key;
        source_width_ = width;
        source_height_ = height;
        preview_width_ = target_width == 0 ? width : (width > 0 ? std::min<uint32_t>(width, target_width) : target_width);
        preview_height_ = scaled_height(width, height, preview_width_);

        int stdin_pipe[2] = {-1, -1};
        int stdout_pipe[2] = {-1, -1};
        if(!create_cloexec_pipe(stdin_pipe) || !create_cloexec_pipe(stdout_pipe)) {
            const int pipe_errno = errno;
            close_pipe(stdin_pipe);
            close_pipe(stdout_pipe);
            logger.warn("rgb preview decoder pipe creation failed: " + std::string(std::strerror(pipe_errno)));
            return false;
        }

        std::string scale = "fps=" + std::to_string(preview_fps);
        if(h264_full_range) {
            scale += ",setparams=range=full:colorspace=smpte170m";
        }
        if(target_width != 0) {
            scale += ",scale=" + std::to_string(target_width) + ":-2";
            if(h264_full_range) {
                scale += ":in_color_matrix=smpte170m:out_color_matrix=smpte170m:in_range=full:out_range=full";
            }
        }
        const std::string jpeg_quality = std::to_string(kRgbPreviewJpegQuality);
        std::vector<std::string> arguments = {
            cfg.ffmpeg_path, "-hide_banner", "-loglevel", "error", "-fflags", "nobuffer", "-flags", "low_delay",
            "-probesize", "32", "-analyzeduration", "0", "-avioflags", "direct", "-f", "h264", "-i", "pipe:0",
            "-vf", scale, "-q:v", jpeg_quality, "-f", "image2pipe", "-vcodec", "mjpeg", "pipe:1"};
        std::vector<char *> argv;
        argv.reserve(arguments.size() + 1);
        for(auto &argument : arguments) {
            argv.push_back(argument.data());
        }
        argv.push_back(nullptr);

        posix_spawn_file_actions_t actions;
        int spawn_rc = posix_spawn_file_actions_init(&actions);
        const bool actions_initialized = spawn_rc == 0;
        if(spawn_rc == 0) {
            spawn_rc = posix_spawn_file_actions_adddup2(&actions, stdin_pipe[0], STDIN_FILENO);
        }
        if(spawn_rc == 0) {
            spawn_rc = posix_spawn_file_actions_adddup2(&actions, stdout_pipe[1], STDOUT_FILENO);
        }
        if(spawn_rc == 0) {
            spawn_rc = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
        }
        if(spawn_rc == 0) {
            spawn_rc = posix_spawn_file_actions_addclose(&actions, stdin_pipe[1]);
        }
        if(spawn_rc == 0) {
            spawn_rc = posix_spawn_file_actions_addclose(&actions, stdout_pipe[0]);
        }
        if(spawn_rc == 0 && stdin_pipe[0] != STDIN_FILENO) {
            spawn_rc = posix_spawn_file_actions_addclose(&actions, stdin_pipe[0]);
        }
        if(spawn_rc == 0 && stdout_pipe[1] != STDOUT_FILENO) {
            spawn_rc = posix_spawn_file_actions_addclose(&actions, stdout_pipe[1]);
        }
        if(spawn_rc == 0) {
            // Do not let a long-lived preview decoder retain recording files.
            // On CIFS, inherited deleted descriptors become .__smb* files and
            // prevent the segment directory from being atomically published.
            spawn_rc = add_spawn_closefrom(&actions);
        }

        pid_t pid = -1;
        if(spawn_rc == 0) {
            spawn_rc = posix_spawnp(&pid, cfg.ffmpeg_path.c_str(), &actions, nullptr, argv.data(), environ);
        }
        if(actions_initialized) {
            posix_spawn_file_actions_destroy(&actions);
        }
        if(spawn_rc != 0) {
            close_pipe(stdin_pipe);
            close_pipe(stdout_pipe);
            logger.warn("rgb preview decoder spawn failed: " + std::string(std::strerror(spawn_rc)));
            return false;
        }

        close(stdin_pipe[0]);
        close(stdout_pipe[1]);
        stdin_fd_ = stdin_pipe[1];
        stdout_fd_ = stdout_pipe[0];
        set_pipe_size_if_supported(stdin_fd_, kRgbPreviewPipeBytes);
        set_fd_nonblocking(stdin_fd_);
        set_fd_nonblocking(stdout_fd_);
        pid_ = pid;
        running_ = true;
        const int reader_fd = stdout_fd_;
        reader_ = std::thread([this, reader_fd] { read_loop(reader_fd); });
        writer_ = std::thread([this] { write_loop(); });
        logger.info("rgb preview decoder started: " + key_ + " h264_full_range=" + (h264_full_range ? "true" : "false"));
        return true;
    }

    bool active() const {
        std::lock_guard<std::mutex> lock(process_mutex_);
        return running_ && stdin_fd_ >= 0;
    }

    void stop() {
        int stdin_fd = -1;
        int stdout_fd = -1;
        pid_t pid = -1;
        {
            std::lock_guard<std::mutex> lock(process_mutex_);
            running_ = false;
            stdin_fd = stdin_fd_;
            stdout_fd = stdout_fd_;
            pid = pid_;
            stdin_fd_ = -1;
            stdout_fd_ = -1;
            pid_ = -1;
        }
        {
            std::lock_guard<std::mutex> lock(write_queue_mutex_);
            write_queue_.clear();
            write_queue_bytes_ = 0;
        }
        write_queue_cv_.notify_all();
        if(stdin_fd >= 0) {
            close(stdin_fd);
        }
        if(pid > 0) {
            kill(pid, SIGTERM);
        }
        if(writer_.joinable()) {
            writer_.join();
        }
        if(pid > 0) {
            int status = 0;
            // Preview decoders are disposable. Keep the graceful-exit window
            // short so rapid target changes cannot build a long cleanup queue.
            for(int i = 0; i < 4; ++i) {
                const pid_t done = waitpid(pid, &status, WNOHANG);
                if(done == pid || (done < 0 && errno == ECHILD)) {
                    pid = -1;
                    break;
                }
                usleep(10 * 1000);
            }
            if(pid > 0) {
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
            }
        }
        // A blocking read on a pipe is not guaranteed to be interrupted when
        // another thread closes its descriptor. Reap the child first so the
        // writer end closes and the reader observes EOF before it is joined.
        if(reader_.joinable()) {
            reader_.join();
        }
        if(stdout_fd >= 0) {
            close(stdout_fd);
        }
    }

    bool write_packet(const std::vector<uint8_t> &payload) {
        if(payload.empty()) {
            return true;
        }
        if(!active()) {
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(write_queue_mutex_);
            if(!running_) {
                return false;
            }
            while(!write_queue_.empty()
                  && (write_queue_.size() >= kRgbPreviewDecoderMaxQueuedPackets
                      || write_queue_bytes_ + payload.size() > kRgbPreviewDecoderMaxQueuedBytes)) {
                write_queue_bytes_ -= write_queue_.front().size();
                write_queue_.pop_front();
            }
            if(payload.size() > kRgbPreviewDecoderMaxQueuedBytes) {
                write_queue_.clear();
                write_queue_bytes_ = 0;
                return false;
            }
            write_queue_bytes_ += payload.size();
            write_queue_.push_back(payload);
        }
        write_queue_cv_.notify_one();
        return true;
    }

    bool write_payload_to_process(const std::vector<uint8_t> &payload) {
        std::lock_guard<std::mutex> lock(process_mutex_);
        if(!running_ || stdin_fd_ < 0) {
            return false;
        }
        size_t offset = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kRgbPreviewWriteBudgetMs);
        while(offset < payload.size()) {
            const ssize_t written = write(stdin_fd_, payload.data() + offset, payload.size() - offset);
            if(written < 0 && errno == EINTR) {
                continue;
            }
            if(written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                if(std::chrono::steady_clock::now() < deadline && wait_fd_writable(stdin_fd_, kRgbPreviewWritePollMs)) {
                    continue;
                }
                break;
            }
            if(written <= 0) {
                break;
            }
            offset += static_cast<size_t>(written);
        }
        if(offset == payload.size()) {
            return true;
        }
        running_ = false;
        close(stdin_fd_);
        stdin_fd_ = -1;
        return false;
    }

    std::optional<std::vector<uint8_t>> latest_jpeg() const {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        if(latest_jpeg_.empty()) {
            return std::nullopt;
        }
        return latest_jpeg_;
    }

    bool has_frame() const {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        return !latest_jpeg_.empty();
    }

    uint32_t preview_width() const {
        return preview_width_;
    }

    uint32_t preview_height() const {
        return preview_height_;
    }

    uint64_t frame_us() const {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        return latest_frame_us_;
    }

private:
    static uint32_t scaled_height(uint32_t width, uint32_t height, uint32_t preview_width) {
        if(width == 0 || height == 0 || preview_width == 0) {
            return 0;
        }
        uint32_t scaled = static_cast<uint32_t>((static_cast<uint64_t>(height) * preview_width) / width);
        if(scaled % 2u != 0u) {
            ++scaled;
        }
        return scaled;
    }

    static void close_pipe(int fds[2]) {
        if(fds[0] >= 0) {
            close(fds[0]);
        }
        if(fds[1] >= 0) {
            close(fds[1]);
        }
    }

    void read_loop(int fd) {
        std::vector<uint8_t> buffer;
        buffer.reserve(512 * 1024);
        std::vector<uint8_t> chunk(32 * 1024);
        while(running_) {
            pollfd pfd{};
            pfd.fd = fd;
            pfd.events = POLLIN;
            int poll_rc = 0;
            do {
                poll_rc = poll(&pfd, 1, kRgbPreviewReadPollMs);
            } while(poll_rc < 0 && errno == EINTR && running_);
            if(!running_) {
                break;
            }
            if(poll_rc < 0 || (pfd.revents & (POLLERR | POLLNVAL)) != 0) {
                break;
            }
            if(poll_rc == 0 || (pfd.revents & (POLLIN | POLLHUP)) == 0) {
                continue;
            }
            while(running_) {
                const ssize_t got = read(fd, chunk.data(), chunk.size());
                if(got > 0) {
                    buffer.insert(buffer.end(), chunk.begin(), chunk.begin() + got);
                    consume_jpegs(buffer);
                    if(buffer.size() > 4ull * 1024ull * 1024ull) {
                        buffer.erase(buffer.begin(), buffer.end() - 1024);
                    }
                    continue;
                }
                if(got < 0 && errno == EINTR) {
                    continue;
                }
                if(got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    break;
                }
                return;
            }
        }
    }

    void write_loop() {
        while(true) {
            std::vector<uint8_t> payload;
            {
                std::unique_lock<std::mutex> lock(write_queue_mutex_);
                write_queue_cv_.wait(lock, [&] { return !running_ || !write_queue_.empty(); });
                if(write_queue_.empty()) {
                    if(!running_) {
                        return;
                    }
                    continue;
                }
                payload = std::move(write_queue_.front());
                write_queue_bytes_ -= payload.size();
                write_queue_.pop_front();
            }
            if(!write_payload_to_process(payload)) {
                {
                    std::lock_guard<std::mutex> lock(write_queue_mutex_);
                    write_queue_.clear();
                    write_queue_bytes_ = 0;
                    running_ = false;
                }
                write_queue_cv_.notify_all();
                return;
            }
        }
    }

    void consume_jpegs(std::vector<uint8_t> &buffer) {
        while(true) {
            const auto start = find_marker(buffer, 0xff, 0xd8, 0);
            if(!start) {
                buffer.clear();
                return;
            }
            if(*start > 0) {
                buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(*start));
            }
            const auto end = find_marker(buffer, 0xff, 0xd9, 2);
            if(!end) {
                return;
            }
            std::vector<uint8_t> jpeg(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(*end + 2));
            {
                std::lock_guard<std::mutex> lock(frame_mutex_);
                latest_jpeg_ = std::move(jpeg);
                latest_frame_us_ = now_us();
            }
            buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(*end + 2));
        }
    }

    std::string key_;
    uint32_t source_width_ = 0;
    uint32_t source_height_ = 0;
    uint32_t preview_width_ = 0;
    uint32_t preview_height_ = 0;
    mutable std::mutex process_mutex_;
    mutable std::mutex frame_mutex_;
    std::atomic<bool> running_{false};
    pid_t pid_ = -1;
    int stdin_fd_ = -1;
    int stdout_fd_ = -1;
    std::thread reader_;
    std::thread writer_;
    mutable std::mutex write_queue_mutex_;
    std::condition_variable write_queue_cv_;
    std::deque<std::vector<uint8_t>> write_queue_;
    size_t write_queue_bytes_ = 0;
    std::vector<uint8_t> latest_jpeg_;
    uint64_t latest_frame_us_ = 0;
};

struct MediaPacket {
    StreamType stream_type = StreamType::rgb;
    uint32_t flags = 0;
    std::string sender_id;
    std::string camera_id;
    std::string codec_or_compression;
    uint64_t frame_id = 0;
    uint64_t timestamp_us = 0;
    uint64_t system_timestamp_us = 0;
    uint64_t pair_id = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    PixelFormat pixel_format = PixelFormat::encoded_video;
    uint64_t payload_size = 0;
    uint64_t uncompressed_size = 0;
    int32_t rgb_exposure_us = -1;
    int32_t rgb_gain = -1;
    int32_t rgb_auto_exposure = -1;
    int32_t rgb_actual_fps = -1;
    uint64_t sender_capture_host_timestamp_us = 0;
    uint64_t sender_timing_bound_timestamp_us = 0;
    uint64_t sender_encode_start_timestamp_us = 0;
    uint64_t sender_encode_done_timestamp_us = 0;
    uint64_t sender_packet_queued_timestamp_us = 0;
    uint64_t receiver_receive_timestamp_us = 0;
    bool clock_sync_valid = false;
    int64_t sender_offset_us = 0;
    int64_t sender_delay_us = 0;
    double sender_drift_ppm = 0.0;
    uint64_t global_timestamp_us = 0;
    std::vector<uint8_t> payload;
};

struct MediaPacketReadBuffers {
    std::vector<uint8_t> header;
    std::vector<char> text;
};

void validate_media_packet_metadata(const MediaPacket &packet, size_t max_payload_bytes) {
    if(!is_valid_protocol_id(packet.sender_id) || !is_valid_protocol_id(packet.camera_id)) {
        throw std::runtime_error("invalid media sender_id/camera_id");
    }
    if(!is_valid_codec_name(packet.codec_or_compression)) {
        throw std::runtime_error("invalid media codec/compression name");
    }
    if(packet.stream_type != StreamType::rgb && packet.stream_type != StreamType::rgb_preview
       && packet.stream_type != StreamType::depth_raw && packet.stream_type != StreamType::rgb_snapshot) {
        throw std::runtime_error("invalid media stream type");
    }
    if(packet.width == 0 || packet.height == 0 || packet.width > kMaxMediaDimension || packet.height > kMaxMediaDimension) {
        throw std::runtime_error("invalid media dimensions");
    }
    if(packet.payload_size == 0 || packet.payload_size > max_payload_bytes) {
        throw std::runtime_error("invalid media payload size");
    }
    if(packet.uncompressed_size > kMaxReasonablePayload) {
        throw std::runtime_error("invalid media uncompressed size");
    }
    if(packet.stream_type == StreamType::depth_raw) {
        if(packet.pixel_format != PixelFormat::depth_u16) {
            throw std::runtime_error("invalid depth pixel format");
        }
        const uint64_t expected_raw_size = static_cast<uint64_t>(packet.width) * packet.height * sizeof(uint16_t);
        if(expected_raw_size > kMaxReasonablePayload || packet.uncompressed_size != expected_raw_size) {
            throw std::runtime_error("depth dimensions and uncompressed size disagree");
        }
        if(packet.codec_or_compression == "none" && packet.payload_size != expected_raw_size) {
            throw std::runtime_error("raw depth payload size disagrees with dimensions");
        }
        static const std::set<std::string> supported_depth_codecs = {
            "none", "zlib", "rvl", "qdelta", "lz4", "plz4", "pzlib", "q8lz4", "q8zlib", "pq12zlib", "pq8zlib", "pq8lz4"};
        if(supported_depth_codecs.count(packet.codec_or_compression) == 0) {
            throw std::runtime_error("unsupported depth compression");
        }
    }
    else if(packet.stream_type == StreamType::rgb_snapshot) {
        if(packet.pixel_format != PixelFormat::encoded_video || !rgb_snapshot_request_id(packet.codec_or_compression)
           || packet.uncompressed_size != packet.payload_size) {
            throw std::runtime_error("invalid RGB snapshot media format metadata");
        }
    }
    else {
        if(packet.pixel_format != PixelFormat::encoded_video || packet.codec_or_compression != "h264") {
            throw std::runtime_error("invalid RGB media format metadata");
        }
    }
}

class DepthDecompressionExecutor {
public:
    DepthDecompressionExecutor() {
        const size_t hardware_workers = std::max<size_t>(1, std::thread::hardware_concurrency());
        const size_t worker_count = std::min(hardware_workers, kMaxDepthDecompressionWorkers);
        workers_.reserve(worker_count);
        for(size_t i = 0; i < worker_count; ++i) {
            try {
                workers_.emplace_back([this] { worker_loop(); });
            }
            catch(...) {
                break;
            }
        }
    }

    ~DepthDecompressionExecutor() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        cv_.notify_all();
        for(auto &worker : workers_) {
            if(worker.joinable()) {
                worker.join();
            }
        }
    }

    DepthDecompressionExecutor(const DepthDecompressionExecutor &) = delete;
    DepthDecompressionExecutor &operator=(const DepthDecompressionExecutor &) = delete;

    bool try_submit(std::function<void()> task) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(stopping_ || workers_.empty() || tasks_.size() >= kMaxQueuedTasks) {
            return false;
        }
        tasks_.push_back(std::move(task));
        cv_.notify_one();
        return true;
    }

private:
    void worker_loop() {
        for(;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [&] { return stopping_ || !tasks_.empty(); });
                if(tasks_.empty()) {
                    if(stopping_) {
                        return;
                    }
                    continue;
                }
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            try {
                task();
            }
            catch(...) {
            }
        }
    }

    static constexpr size_t kMaxQueuedTasks = 128;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> tasks_;
    std::vector<std::thread> workers_;
    bool stopping_ = false;
};

DepthDecompressionExecutor &depth_decompression_executor() {
    static DepthDecompressionExecutor executor;
    return executor;
}

template <typename Function>
void bounded_parallel_for(size_t count, Function &&function) {
    if(count == 0) {
        return;
    }
    const size_t hardware_workers = std::max<size_t>(1, std::thread::hardware_concurrency());
    const size_t worker_count = std::min({count, hardware_workers, kMaxDepthDecompressionWorkers});
    std::atomic<size_t> next{0};
    std::atomic<bool> failed{false};
    std::mutex error_mutex;
    std::exception_ptr error;
    auto worker = [&] {
        while(!failed.load(std::memory_order_relaxed)) {
            const size_t index = next.fetch_add(1, std::memory_order_relaxed);
            if(index >= count) {
                break;
            }
            try {
                function(index);
            }
            catch(...) {
                failed.store(true, std::memory_order_relaxed);
                std::lock_guard<std::mutex> lock(error_mutex);
                if(!error) {
                    error = std::current_exception();
                }
                break;
            }
        }
    };

    std::atomic<size_t> pending_workers{0};
    std::mutex completion_mutex;
    std::condition_variable completion_cv;
    for(size_t i = 1; i < worker_count; ++i) {
        pending_workers.fetch_add(1, std::memory_order_relaxed);
        bool queued = false;
        try {
            queued = depth_decompression_executor().try_submit([&] {
                worker();
                // Publish completion while holding the same mutex used by the
                // waiter. Otherwise notify_one() can land between the
                // predicate check and the wait, leaving a completed job asleep.
                {
                    std::lock_guard<std::mutex> lock(completion_mutex);
                    pending_workers.fetch_sub(1, std::memory_order_release);
                }
                completion_cv.notify_one();
            });
        }
        catch(...) {
            queued = false;
        }
        if(!queued) {
            {
                std::lock_guard<std::mutex> lock(completion_mutex);
                pending_workers.fetch_sub(1, std::memory_order_relaxed);
            }
            break;
        }
    }
    worker();
    {
        std::unique_lock<std::mutex> lock(completion_mutex);
        completion_cv.wait(lock, [&] { return pending_workers.load(std::memory_order_acquire) == 0; });
    }
    if(error) {
        std::rethrow_exception(error);
    }
}

void copy_media_packet_metadata(const MediaPacket &src, MediaPacket &dst) {
    dst.stream_type = src.stream_type;
    dst.flags = src.flags;
    dst.sender_id = src.sender_id;
    dst.camera_id = src.camera_id;
    dst.codec_or_compression = src.codec_or_compression;
    dst.frame_id = src.frame_id;
    dst.timestamp_us = src.timestamp_us;
    dst.system_timestamp_us = src.system_timestamp_us;
    dst.pair_id = src.pair_id;
    dst.width = src.width;
    dst.height = src.height;
    dst.pixel_format = src.pixel_format;
    dst.payload_size = src.payload_size;
    dst.uncompressed_size = src.uncompressed_size;
    dst.rgb_exposure_us = src.rgb_exposure_us;
    dst.rgb_gain = src.rgb_gain;
    dst.rgb_auto_exposure = src.rgb_auto_exposure;
    dst.rgb_actual_fps = src.rgb_actual_fps;
    dst.sender_capture_host_timestamp_us = src.sender_capture_host_timestamp_us;
    dst.sender_timing_bound_timestamp_us = src.sender_timing_bound_timestamp_us;
    dst.sender_encode_start_timestamp_us = src.sender_encode_start_timestamp_us;
    dst.sender_encode_done_timestamp_us = src.sender_encode_done_timestamp_us;
    dst.sender_packet_queued_timestamp_us = src.sender_packet_queued_timestamp_us;
    dst.receiver_receive_timestamp_us = src.receiver_receive_timestamp_us;
    dst.clock_sync_valid = src.clock_sync_valid;
    dst.sender_offset_us = src.sender_offset_us;
    dst.sender_delay_us = src.sender_delay_us;
    dst.sender_drift_ppm = src.sender_drift_ppm;
    dst.global_timestamp_us = src.global_timestamp_us;
    dst.payload.clear();
}

MediaPacket media_packet_metadata_only(const MediaPacket &packet) {
    MediaPacket copy;
    copy_media_packet_metadata(packet, copy);
    return copy;
}

void reset_media_packet_for_read(MediaPacket &packet) {
    packet.stream_type = StreamType::rgb;
    packet.flags = 0;
    packet.sender_id.clear();
    packet.camera_id.clear();
    packet.codec_or_compression.clear();
    packet.frame_id = 0;
    packet.timestamp_us = 0;
    packet.system_timestamp_us = 0;
    packet.pair_id = 0;
    packet.width = 0;
    packet.height = 0;
    packet.pixel_format = PixelFormat::encoded_video;
    packet.payload_size = 0;
    packet.uncompressed_size = 0;
    packet.rgb_exposure_us = -1;
    packet.rgb_gain = -1;
    packet.rgb_auto_exposure = -1;
    packet.rgb_actual_fps = -1;
    packet.sender_capture_host_timestamp_us = 0;
    packet.sender_timing_bound_timestamp_us = 0;
    packet.sender_encode_start_timestamp_us = 0;
    packet.sender_encode_done_timestamp_us = 0;
    packet.sender_packet_queued_timestamp_us = 0;
    packet.receiver_receive_timestamp_us = 0;
    packet.clock_sync_valid = false;
    packet.sender_offset_us = 0;
    packet.sender_delay_us = 0;
    packet.sender_drift_ppm = 0.0;
    packet.global_timestamp_us = 0;
    packet.payload.clear();
}

void read_media_packet_into(int fd, size_t max_payload_bytes, MediaPacketReadBuffers &buffers, MediaPacket &packet) {
    reset_media_packet_for_read(packet);
    buffers.header.resize(kMediaHeaderBaseSize);
    if(!read_exact(fd, buffers.header.data(), buffers.header.size())) {
        throw std::runtime_error("connection closed");
    }

    auto &header = buffers.header;
    const uint32_t magic = read_le32(header.data() + 0);
    const uint16_t header_version = read_le16(header.data() + 4);
    const uint16_t header_size = read_le16(header.data() + 6);
    if(magic != kMediaMagic || header_version < 1 || header_version > kMediaHeaderVersion || header_size < kMediaHeaderBaseSize
       || header_size > kMediaHeaderMaxSize) {
        throw std::runtime_error("invalid media packet header");
    }
    if(header_size > header.size()) {
        const size_t already_read = header.size();
        header.resize(header_size);
        if(!read_exact(fd, header.data() + already_read, header_size - already_read)) {
            throw std::runtime_error("connection closed");
        }
    }

    const uint16_t sender_id_len = read_le16(header.data() + 14);
    const uint16_t camera_id_len = read_le16(header.data() + 16);
    const uint16_t codec_len = read_le16(header.data() + 18);
    const uint64_t payload_size = read_le64(header.data() + 62);
    if(sender_id_len == 0 || sender_id_len > kMaxProtocolIdBytes
       || camera_id_len == 0 || camera_id_len > kMaxProtocolIdBytes
       || codec_len == 0 || codec_len > kMaxCodecNameBytes) {
        throw std::runtime_error("media packet string field length is invalid");
    }
    if(payload_size > max_payload_bytes) {
        throw std::runtime_error("media payload too large");
    }

    packet.stream_type = static_cast<StreamType>(header[8]);
    packet.flags = read_le32(header.data() + 10);
    packet.frame_id = read_le64(header.data() + 20);
    packet.timestamp_us = read_le64(header.data() + 28);
    packet.system_timestamp_us = read_le64(header.data() + 36);
    packet.pair_id = read_le64(header.data() + 44);
    packet.width = read_le32(header.data() + 52);
    packet.height = read_le32(header.data() + 56);
    packet.pixel_format = static_cast<PixelFormat>(read_le16(header.data() + 60));
    packet.payload_size = payload_size;
    packet.uncompressed_size = read_le64(header.data() + 70);
    if((packet.flags & has_rgb_diagnostics) != 0u) {
        packet.rgb_exposure_us = static_cast<int32_t>(read_le32(header.data() + 78));
        packet.rgb_gain = static_cast<int32_t>(read_le32(header.data() + 82));
        packet.rgb_auto_exposure = static_cast<int32_t>(read_le32(header.data() + 86));
        packet.rgb_actual_fps = static_cast<int32_t>(read_le32(header.data() + 90));
    }
    if(header_size >= kMediaHeaderV2Size && (packet.flags & has_pipeline_diagnostics) != 0u) {
        packet.sender_capture_host_timestamp_us = read_le64(header.data() + 94);
        packet.sender_timing_bound_timestamp_us = read_le64(header.data() + 102);
        packet.sender_encode_start_timestamp_us = read_le64(header.data() + 110);
        packet.sender_encode_done_timestamp_us = read_le64(header.data() + 118);
        packet.sender_packet_queued_timestamp_us = read_le64(header.data() + 126);
    }

    const size_t text_size = static_cast<size_t>(sender_id_len) + static_cast<size_t>(camera_id_len) + static_cast<size_t>(codec_len);
    buffers.text.resize(text_size);
    if(!buffers.text.empty() && !read_exact(fd, buffers.text.data(), buffers.text.size())) {
        throw std::runtime_error("connection closed while reading packet strings");
    }
    const char *text = buffers.text.empty() ? "" : buffers.text.data();
    packet.sender_id.assign(text, sender_id_len);
    packet.camera_id.assign(text + sender_id_len, camera_id_len);
    packet.codec_or_compression.assign(text + sender_id_len + camera_id_len, codec_len);

    validate_media_packet_metadata(packet, max_payload_bytes);

    packet.payload.resize(static_cast<size_t>(payload_size));
    if(payload_size > 0 && !read_exact(fd, packet.payload.data(), packet.payload.size())) {
        throw std::runtime_error("connection closed while reading payload");
    }
}

MediaPacket parse_media_packet_buffer(const uint8_t *data, size_t size, size_t max_payload_bytes) {
    if(size < kMediaHeaderBaseSize) {
        throw std::runtime_error("UDP media packet too small");
    }

    const uint32_t magic = read_le32(data + 0);
    const uint16_t header_version = read_le16(data + 4);
    const uint16_t header_size = read_le16(data + 6);
    if(magic != kMediaMagic || header_version < 1 || header_version > kMediaHeaderVersion || header_size < kMediaHeaderBaseSize
       || header_size > kMediaHeaderMaxSize || size < header_size) {
        throw std::runtime_error("invalid UDP media packet header");
    }

    const uint16_t sender_id_len = read_le16(data + 14);
    const uint16_t camera_id_len = read_le16(data + 16);
    const uint16_t codec_len = read_le16(data + 18);
    const uint64_t payload_size = read_le64(data + 62);
    if(sender_id_len == 0 || sender_id_len > kMaxProtocolIdBytes
       || camera_id_len == 0 || camera_id_len > kMaxProtocolIdBytes
       || codec_len == 0 || codec_len > kMaxCodecNameBytes) {
        throw std::runtime_error("UDP media packet string field length is invalid");
    }
    if(payload_size > max_payload_bytes) {
        throw std::runtime_error("UDP media payload too large");
    }
    const size_t text_size = static_cast<size_t>(sender_id_len) + static_cast<size_t>(camera_id_len) + static_cast<size_t>(codec_len);
    const size_t payload_offset = static_cast<size_t>(header_size) + text_size;
    if(payload_offset > size || payload_size != size - payload_offset) {
        throw std::runtime_error("truncated UDP media packet");
    }

    MediaPacket packet;
    packet.stream_type = static_cast<StreamType>(data[8]);
    packet.flags = read_le32(data + 10);
    packet.frame_id = read_le64(data + 20);
    packet.timestamp_us = read_le64(data + 28);
    packet.system_timestamp_us = read_le64(data + 36);
    packet.pair_id = read_le64(data + 44);
    packet.width = read_le32(data + 52);
    packet.height = read_le32(data + 56);
    packet.pixel_format = static_cast<PixelFormat>(read_le16(data + 60));
    packet.payload_size = payload_size;
    packet.uncompressed_size = read_le64(data + 70);
    if((packet.flags & has_rgb_diagnostics) != 0u) {
        packet.rgb_exposure_us = static_cast<int32_t>(read_le32(data + 78));
        packet.rgb_gain = static_cast<int32_t>(read_le32(data + 82));
        packet.rgb_auto_exposure = static_cast<int32_t>(read_le32(data + 86));
        packet.rgb_actual_fps = static_cast<int32_t>(read_le32(data + 90));
    }
    if(header_size >= kMediaHeaderV2Size && (packet.flags & has_pipeline_diagnostics) != 0u) {
        packet.sender_capture_host_timestamp_us = read_le64(data + 94);
        packet.sender_timing_bound_timestamp_us = read_le64(data + 102);
        packet.sender_encode_start_timestamp_us = read_le64(data + 110);
        packet.sender_encode_done_timestamp_us = read_le64(data + 118);
        packet.sender_packet_queued_timestamp_us = read_le64(data + 126);
    }

    const char *text = reinterpret_cast<const char *>(data + header_size);
    packet.sender_id.assign(text, sender_id_len);
    packet.camera_id.assign(text + sender_id_len, camera_id_len);
    packet.codec_or_compression.assign(text + sender_id_len + camera_id_len, codec_len);
    validate_media_packet_metadata(packet, max_payload_bytes);
    packet.payload.assign(data + payload_offset, data + payload_offset + static_cast<size_t>(payload_size));
    return packet;
}

std::vector<uint8_t> zlib_decompress_payload(const MediaPacket &packet) {
    if(packet.uncompressed_size == 0 || packet.uncompressed_size > kMaxReasonablePayload) {
        throw std::runtime_error("invalid zlib uncompressed depth size");
    }
    std::vector<uint8_t> out(static_cast<size_t>(packet.uncompressed_size));
    uLongf out_size = static_cast<uLongf>(out.size());
    const int rc = uncompress(out.data(), &out_size, packet.payload.data(), static_cast<uLong>(packet.payload.size()));
    if(rc != Z_OK || out_size != packet.uncompressed_size) {
        throw std::runtime_error("zlib depth decompression failed");
    }
    return out;
}

struct Lz4Api {
    using DecompressSafeFn = int (*)(const char *, char *, int, int);

    void *handle = nullptr;
    DecompressSafeFn decompress_safe = nullptr;
};

Lz4Api &lz4_api() {
    static Lz4Api api;
    static std::once_flag once;
    std::call_once(once, [] {
        api.handle = dlopen("liblz4.so.1", RTLD_LAZY | RTLD_LOCAL);
        if(!api.handle) {
            throw std::runtime_error(std::string("cannot load liblz4.so.1: ") + dlerror());
        }
        api.decompress_safe = reinterpret_cast<Lz4Api::DecompressSafeFn>(dlsym(api.handle, "LZ4_decompress_safe"));
        if(!api.decompress_safe) {
            throw std::runtime_error("liblz4.so.1 does not provide required decompression symbols");
        }
    });
    return api;
}

std::vector<uint8_t> lz4_decompress_payload(const MediaPacket &packet) {
    if(packet.uncompressed_size == 0 || packet.uncompressed_size > kMaxReasonablePayload
       || packet.payload.size() > static_cast<size_t>(std::numeric_limits<int>::max())
       || packet.uncompressed_size > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("invalid lz4 uncompressed depth size");
    }
    std::vector<uint8_t> out(static_cast<size_t>(packet.uncompressed_size));
    auto &api = lz4_api();
    const int decoded_size = api.decompress_safe(reinterpret_cast<const char *>(packet.payload.data()),
                                                 reinterpret_cast<char *>(out.data()),
                                                 static_cast<int>(packet.payload.size()),
                                                 static_cast<int>(out.size()));
    if(decoded_size != static_cast<int>(out.size())) {
        throw std::runtime_error("lz4 depth decompression failed");
    }
    return out;
}

struct Plz4ChunkEntry {
    uint32_t raw_offset = 0;
    uint32_t raw_size = 0;
    uint32_t compressed_offset = 0;
    uint32_t compressed_size = 0;
};

std::vector<uint8_t> plz4_decompress_payload(const MediaPacket &packet) {
    if(packet.payload.size() < 16) {
        throw std::runtime_error("invalid plz4 depth payload");
    }
    const uint32_t magic = read_le32(packet.payload.data());
    const uint16_t version = read_le16(packet.payload.data() + 4);
    const uint16_t chunk_count = read_le16(packet.payload.data() + 6);
    const uint32_t raw_total = read_le32(packet.payload.data() + 8);
    if(magic != 0x345a4c50u || version != 1 || chunk_count == 0 || chunk_count > kMaxDepthCompressionChunks || raw_total == 0
       || raw_total > kMaxReasonablePayload || packet.uncompressed_size != raw_total) {
        throw std::runtime_error("invalid plz4 depth header");
    }
    const size_t table_size = 16ull + static_cast<size_t>(chunk_count) * 12ull;
    if(table_size > packet.payload.size()) {
        throw std::runtime_error("truncated plz4 depth table");
    }

    std::vector<Plz4ChunkEntry> chunks;
    chunks.reserve(chunk_count);
    size_t compressed_offset = table_size;
    uint32_t expected_raw_offset = 0;
    for(uint16_t i = 0; i < chunk_count; ++i) {
        const uint8_t *entry = packet.payload.data() + 16ull + static_cast<size_t>(i) * 12ull;
        Plz4ChunkEntry chunk;
        chunk.raw_offset = read_le32(entry);
        chunk.raw_size = read_le32(entry + 4);
        chunk.compressed_size = read_le32(entry + 8);
        chunk.compressed_offset = static_cast<uint32_t>(compressed_offset);
        if(chunk.raw_offset != expected_raw_offset || chunk.raw_size == 0 || chunk.raw_offset > raw_total
           || chunk.raw_size > raw_total - chunk.raw_offset
           || chunk.compressed_size == 0 || compressed_offset > packet.payload.size()
           || chunk.compressed_size > packet.payload.size() - compressed_offset) {
            throw std::runtime_error("invalid plz4 depth chunk");
        }
        expected_raw_offset += chunk.raw_size;
        compressed_offset += chunk.compressed_size;
        chunks.push_back(chunk);
    }
    if(expected_raw_offset != raw_total || compressed_offset != packet.payload.size()) {
        throw std::runtime_error("plz4 depth payload has trailing bytes");
    }

    std::vector<uint8_t> out(raw_total);
    auto &api = lz4_api();
    bounded_parallel_for(chunks.size(), [&](size_t index) {
            const auto &chunk = chunks[index];
            const int rc = api.decompress_safe(
                reinterpret_cast<const char *>(packet.payload.data() + chunk.compressed_offset),
                reinterpret_cast<char *>(out.data() + chunk.raw_offset),
                static_cast<int>(chunk.compressed_size),
                static_cast<int>(chunk.raw_size));
            if(rc != static_cast<int>(chunk.raw_size)) {
                throw std::runtime_error("plz4 depth chunk decompression failed");
            }
        });
    return out;
}

std::vector<uint8_t> pzlib_decompress_payload(const MediaPacket &packet) {
    if(packet.payload.size() < 16) {
        throw std::runtime_error("invalid pzlib depth payload");
    }
    const uint32_t magic = read_le32(packet.payload.data());
    const uint16_t version = read_le16(packet.payload.data() + 4);
    const uint16_t chunk_count = read_le16(packet.payload.data() + 6);
    const uint32_t raw_total = read_le32(packet.payload.data() + 8);
    if(magic != 0x424c5a50u || version != 1 || chunk_count == 0 || chunk_count > kMaxDepthCompressionChunks || raw_total == 0
       || raw_total > kMaxReasonablePayload || packet.uncompressed_size != raw_total) {
        throw std::runtime_error("invalid pzlib depth header");
    }
    const size_t table_size = 16ull + static_cast<size_t>(chunk_count) * 12ull;
    if(table_size > packet.payload.size()) {
        throw std::runtime_error("truncated pzlib depth table");
    }

    std::vector<Plz4ChunkEntry> chunks;
    chunks.reserve(chunk_count);
    size_t compressed_offset = table_size;
    uint32_t expected_raw_offset = 0;
    for(uint16_t i = 0; i < chunk_count; ++i) {
        const uint8_t *entry = packet.payload.data() + 16ull + static_cast<size_t>(i) * 12ull;
        Plz4ChunkEntry chunk;
        chunk.raw_offset = read_le32(entry);
        chunk.raw_size = read_le32(entry + 4);
        chunk.compressed_size = read_le32(entry + 8);
        chunk.compressed_offset = static_cast<uint32_t>(compressed_offset);
        if(chunk.raw_offset != expected_raw_offset || chunk.raw_size == 0 || chunk.raw_offset > raw_total
           || chunk.raw_size > raw_total - chunk.raw_offset
           || chunk.compressed_size == 0 || compressed_offset > packet.payload.size()
           || chunk.compressed_size > packet.payload.size() - compressed_offset
           || chunk.raw_size > static_cast<uint32_t>(std::numeric_limits<uLongf>::max())) {
            throw std::runtime_error("invalid pzlib depth chunk");
        }
        expected_raw_offset += chunk.raw_size;
        compressed_offset += chunk.compressed_size;
        chunks.push_back(chunk);
    }
    if(expected_raw_offset != raw_total || compressed_offset != packet.payload.size()) {
        throw std::runtime_error("pzlib depth payload has trailing bytes");
    }

    std::vector<uint8_t> out(raw_total);
    bounded_parallel_for(chunks.size(), [&](size_t index) {
            const auto &chunk = chunks[index];
            uLongf out_size = static_cast<uLongf>(chunk.raw_size);
            const int rc =
                uncompress(out.data() + chunk.raw_offset, &out_size, packet.payload.data() + chunk.compressed_offset,
                           static_cast<uLong>(chunk.compressed_size));
            if(rc != Z_OK || out_size != chunk.raw_size) {
                throw std::runtime_error("pzlib depth chunk decompression failed");
            }
        });
    return out;
}

std::vector<uint8_t> q8_decompress_payload(const MediaPacket &packet, uint32_t magic, bool use_lz4) {
    if(packet.uncompressed_size == 0 || packet.uncompressed_size > kMaxReasonablePayload
       || packet.uncompressed_size % sizeof(uint16_t) != 0 || packet.payload.size() < 16) {
        throw std::runtime_error("invalid q8 depth payload");
    }
    const uint32_t packet_magic = read_le32(packet.payload.data());
    const uint16_t version = read_le16(packet.payload.data() + 4);
    const uint16_t raw_step = read_le16(packet.payload.data() + 6);
    const uint32_t sample_count = read_le32(packet.payload.data() + 8);
    const uint32_t compressed_size = read_le32(packet.payload.data() + 12);
    const size_t expected_sample_count = static_cast<size_t>(packet.uncompressed_size / sizeof(uint16_t));
    if(packet_magic != magic || version != 1 || raw_step == 0 || sample_count != expected_sample_count
       || sample_count > static_cast<uint32_t>(std::numeric_limits<int>::max()) || compressed_size == 0
       || static_cast<size_t>(compressed_size) != packet.payload.size() - 16
       || compressed_size > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("invalid q8 depth header");
    }

    std::vector<uint8_t> quantized(expected_sample_count);
    if(use_lz4) {
        auto &api = lz4_api();
        const int decoded_size = api.decompress_safe(reinterpret_cast<const char *>(packet.payload.data() + 16),
                                                     reinterpret_cast<char *>(quantized.data()),
                                                     static_cast<int>(compressed_size),
                                                     static_cast<int>(quantized.size()));
        if(decoded_size != static_cast<int>(quantized.size())) {
            throw std::runtime_error("q8 lz4 depth decompression failed");
        }
    }
    else {
        uLongf out_size = static_cast<uLongf>(quantized.size());
        const int rc = uncompress(quantized.data(), &out_size, packet.payload.data() + 16, static_cast<uLong>(compressed_size));
        if(rc != Z_OK || out_size != quantized.size()) {
            throw std::runtime_error("q8 zlib depth decompression failed");
        }
    }

    std::vector<uint8_t> out(static_cast<size_t>(packet.uncompressed_size), 0);
    for(size_t i = 0; i < quantized.size(); ++i) {
        const uint8_t index = quantized[i];
        const uint32_t value = index == 0 ? 0u : std::min<uint32_t>(static_cast<uint32_t>(index) * raw_step,
                                                                    std::numeric_limits<uint16_t>::max());
        const size_t offset = i * sizeof(uint16_t);
        out[offset] = static_cast<uint8_t>(value & 0xffu);
        out[offset + 1] = static_cast<uint8_t>((value >> 8u) & 0xffu);
    }
    return out;
}

std::vector<uint8_t> q8lz4_decompress_payload(const MediaPacket &packet) {
    return q8_decompress_payload(packet, 0x314c3851u, true);  // bytes: Q 8 L 1
}

std::vector<uint8_t> q8zlib_decompress_payload(const MediaPacket &packet) {
    return q8_decompress_payload(packet, 0x315a3851u, false);  // bytes: Q 8 Z 1
}

struct PQ8ZlibChunkEntry {
    uint32_t sample_offset = 0;
    uint32_t sample_count = 0;
    uint32_t compressed_offset = 0;
    uint32_t compressed_size = 0;
};

size_t packed_depth12_size(uint32_t sample_count) {
    return ((static_cast<size_t>(sample_count) + 1u) / 2u) * 3u;
}

void unpack_depth12_into(const std::vector<uint8_t> &packed, uint16_t raw_step, std::vector<uint8_t> &out, uint32_t sample_offset,
                         uint32_t sample_count) {
    size_t packed_index = 0;
    for(uint32_t i = 0; i < sample_count; i += 2) {
        if(packed_index + 2 >= packed.size()) {
            throw std::runtime_error("truncated pq12zlib depth chunk");
        }
        const uint16_t a = static_cast<uint16_t>(packed[packed_index])
                           | (static_cast<uint16_t>(packed[packed_index + 1] & 0x0fu) << 8u);
        const uint16_t b = static_cast<uint16_t>((packed[packed_index + 1] >> 4u) & 0x0fu)
                           | (static_cast<uint16_t>(packed[packed_index + 2]) << 4u);
        packed_index += 3;
        const auto write_sample = [&](uint32_t local_index, uint16_t quantized) {
            const uint32_t value = quantized == 0 ? 0u : std::min<uint32_t>(static_cast<uint32_t>(quantized) * raw_step,
                                                                            std::numeric_limits<uint16_t>::max());
            const size_t offset = (static_cast<size_t>(sample_offset) + local_index) * sizeof(uint16_t);
            out[offset] = static_cast<uint8_t>(value & 0xffu);
            out[offset + 1] = static_cast<uint8_t>((value >> 8u) & 0xffu);
        };
        write_sample(i, a);
        if(i + 1 < sample_count) {
            write_sample(i + 1, b);
        }
    }
}

std::vector<uint8_t> pq12zlib_decompress_payload(const MediaPacket &packet) {
    if(packet.uncompressed_size == 0 || packet.uncompressed_size > kMaxReasonablePayload
       || packet.uncompressed_size % sizeof(uint16_t) != 0 || packet.payload.size() < 20) {
        throw std::runtime_error("invalid pq12zlib depth payload");
    }
    const uint32_t magic = read_le32(packet.payload.data());
    const uint16_t version = read_le16(packet.payload.data() + 4);
    const uint16_t raw_step = read_le16(packet.payload.data() + 6);
    const uint32_t sample_count = read_le32(packet.payload.data() + 8);
    const uint16_t chunk_count = read_le16(packet.payload.data() + 12);
    const size_t expected_sample_count = static_cast<size_t>(packet.uncompressed_size / sizeof(uint16_t));
    if(magic != 0x5a323150u || version != 1 || raw_step == 0 || sample_count != expected_sample_count || chunk_count == 0
       || chunk_count > kMaxDepthCompressionChunks) {
        throw std::runtime_error("invalid pq12zlib depth header");
    }
    const size_t table_size = 20ull + static_cast<size_t>(chunk_count) * 12ull;
    if(table_size > packet.payload.size()) {
        throw std::runtime_error("truncated pq12zlib depth table");
    }

    std::vector<PQ8ZlibChunkEntry> chunks;
    chunks.reserve(chunk_count);
    size_t compressed_offset = table_size;
    uint32_t expected_offset = 0;
    for(uint16_t i = 0; i < chunk_count; ++i) {
        const uint8_t *entry = packet.payload.data() + 20ull + static_cast<size_t>(i) * 12ull;
        PQ8ZlibChunkEntry chunk;
        chunk.sample_offset = read_le32(entry);
        chunk.sample_count = read_le32(entry + 4);
        chunk.compressed_size = read_le32(entry + 8);
        chunk.compressed_offset = static_cast<uint32_t>(compressed_offset);
        if(chunk.sample_offset != expected_offset || chunk.sample_count == 0 || chunk.sample_offset > sample_count
           || chunk.sample_count > sample_count - chunk.sample_offset || chunk.compressed_size == 0
           || compressed_offset > packet.payload.size() || chunk.compressed_size > packet.payload.size() - compressed_offset
           || packed_depth12_size(chunk.sample_count) > static_cast<size_t>(std::numeric_limits<uLongf>::max())) {
            throw std::runtime_error("invalid pq12zlib depth chunk");
        }
        expected_offset += chunk.sample_count;
        compressed_offset += chunk.compressed_size;
        chunks.push_back(chunk);
    }
    if(expected_offset != sample_count || compressed_offset != packet.payload.size()) {
        throw std::runtime_error("invalid pq12zlib depth layout");
    }

    std::vector<uint8_t> out(static_cast<size_t>(packet.uncompressed_size), 0);
    bounded_parallel_for(chunks.size(), [&](size_t index) {
            const auto &chunk = chunks[index];
            std::vector<uint8_t> packed(packed_depth12_size(chunk.sample_count));
            uLongf out_size = static_cast<uLongf>(packed.size());
            const int rc =
                uncompress(packed.data(), &out_size, packet.payload.data() + chunk.compressed_offset,
                           static_cast<uLong>(chunk.compressed_size));
            if(rc != Z_OK || out_size != packed.size()) {
                throw std::runtime_error("pq12zlib depth chunk decompression failed");
            }
            unpack_depth12_into(packed, raw_step, out, chunk.sample_offset, chunk.sample_count);
        });
    return out;
}

std::vector<uint8_t> pq8zlib_decompress_payload(const MediaPacket &packet) {
    if(packet.uncompressed_size == 0 || packet.uncompressed_size > kMaxReasonablePayload
       || packet.uncompressed_size % sizeof(uint16_t) != 0 || packet.payload.size() < 20) {
        throw std::runtime_error("invalid pq8zlib depth payload");
    }
    const uint32_t magic = read_le32(packet.payload.data());
    const uint16_t version = read_le16(packet.payload.data() + 4);
    const uint16_t raw_step = read_le16(packet.payload.data() + 6);
    const uint32_t sample_count = read_le32(packet.payload.data() + 8);
    const uint16_t chunk_count = read_le16(packet.payload.data() + 12);
    const size_t expected_sample_count = static_cast<size_t>(packet.uncompressed_size / sizeof(uint16_t));
    if(magic != 0x5a385150u || version != 1 || raw_step == 0 || sample_count != expected_sample_count || chunk_count == 0
       || chunk_count > kMaxDepthCompressionChunks) {
        throw std::runtime_error("invalid pq8zlib depth header");
    }
    const size_t table_size = 20ull + static_cast<size_t>(chunk_count) * 12ull;
    if(table_size > packet.payload.size()) {
        throw std::runtime_error("truncated pq8zlib depth table");
    }

    std::vector<PQ8ZlibChunkEntry> chunks;
    chunks.reserve(chunk_count);
    size_t compressed_offset = table_size;
    uint32_t expected_offset = 0;
    for(uint16_t i = 0; i < chunk_count; ++i) {
        const uint8_t *entry = packet.payload.data() + 20ull + static_cast<size_t>(i) * 12ull;
        PQ8ZlibChunkEntry chunk;
        chunk.sample_offset = read_le32(entry);
        chunk.sample_count = read_le32(entry + 4);
        chunk.compressed_size = read_le32(entry + 8);
        chunk.compressed_offset = static_cast<uint32_t>(compressed_offset);
        if(chunk.sample_offset != expected_offset || chunk.sample_count == 0 || chunk.sample_offset > sample_count
           || chunk.sample_count > sample_count - chunk.sample_offset || chunk.compressed_size == 0
           || compressed_offset > packet.payload.size() || chunk.compressed_size > packet.payload.size() - compressed_offset
           || chunk.sample_count > static_cast<uint32_t>(std::numeric_limits<uLongf>::max())) {
            throw std::runtime_error("invalid pq8zlib depth chunk");
        }
        expected_offset += chunk.sample_count;
        compressed_offset += chunk.compressed_size;
        chunks.push_back(chunk);
    }
    if(expected_offset != sample_count || compressed_offset != packet.payload.size()) {
        throw std::runtime_error("invalid pq8zlib depth layout");
    }

    std::vector<uint8_t> quantized(expected_sample_count);
    bounded_parallel_for(chunks.size(), [&](size_t index) {
            const auto &chunk = chunks[index];
            uLongf out_size = static_cast<uLongf>(chunk.sample_count);
            const int rc =
                uncompress(quantized.data() + chunk.sample_offset, &out_size, packet.payload.data() + chunk.compressed_offset,
                           static_cast<uLong>(chunk.compressed_size));
            if(rc != Z_OK || out_size != chunk.sample_count) {
                throw std::runtime_error("pq8zlib depth chunk decompression failed");
            }
        });

    std::vector<uint8_t> out(static_cast<size_t>(packet.uncompressed_size), 0);
    for(size_t i = 0; i < quantized.size(); ++i) {
        const uint8_t index = quantized[i];
        const uint32_t value = index == 0 ? 0u : std::min<uint32_t>(static_cast<uint32_t>(index) * raw_step,
                                                                    std::numeric_limits<uint16_t>::max());
        const size_t offset = i * sizeof(uint16_t);
        out[offset] = static_cast<uint8_t>(value & 0xffu);
        out[offset + 1] = static_cast<uint8_t>((value >> 8u) & 0xffu);
    }
    return out;
}

std::vector<uint8_t> pq8lz4_decompress_payload(const MediaPacket &packet) {
    if(packet.uncompressed_size == 0 || packet.uncompressed_size > kMaxReasonablePayload
       || packet.uncompressed_size % sizeof(uint16_t) != 0 || packet.payload.size() < 20) {
        throw std::runtime_error("invalid pq8lz4 depth payload");
    }
    const uint32_t magic = read_le32(packet.payload.data());
    const uint16_t version = read_le16(packet.payload.data() + 4);
    const uint16_t raw_step = read_le16(packet.payload.data() + 6);
    const uint32_t sample_count = read_le32(packet.payload.data() + 8);
    const uint16_t chunk_count = read_le16(packet.payload.data() + 12);
    const size_t expected_sample_count = static_cast<size_t>(packet.uncompressed_size / sizeof(uint16_t));
    if(magic != 0x4c385150u || version != 1 || raw_step == 0 || sample_count != expected_sample_count || chunk_count == 0
       || chunk_count > kMaxDepthCompressionChunks) {
        throw std::runtime_error("invalid pq8lz4 depth header");
    }
    const size_t table_size = 20ull + static_cast<size_t>(chunk_count) * 12ull;
    if(table_size > packet.payload.size()) {
        throw std::runtime_error("truncated pq8lz4 depth table");
    }

    std::vector<PQ8ZlibChunkEntry> chunks;
    chunks.reserve(chunk_count);
    size_t compressed_offset = table_size;
    uint32_t expected_offset = 0;
    for(uint16_t i = 0; i < chunk_count; ++i) {
        const uint8_t *entry = packet.payload.data() + 20ull + static_cast<size_t>(i) * 12ull;
        PQ8ZlibChunkEntry chunk;
        chunk.sample_offset = read_le32(entry);
        chunk.sample_count = read_le32(entry + 4);
        chunk.compressed_size = read_le32(entry + 8);
        chunk.compressed_offset = static_cast<uint32_t>(compressed_offset);
        if(chunk.sample_offset != expected_offset || chunk.sample_count == 0 || chunk.sample_offset > sample_count
           || chunk.sample_count > sample_count - chunk.sample_offset || chunk.compressed_size == 0
           || compressed_offset > packet.payload.size() || chunk.compressed_size > packet.payload.size() - compressed_offset
           || chunk.sample_count > static_cast<uint32_t>(std::numeric_limits<int>::max())
           || chunk.compressed_size > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
            throw std::runtime_error("invalid pq8lz4 depth chunk");
        }
        expected_offset += chunk.sample_count;
        compressed_offset += chunk.compressed_size;
        chunks.push_back(chunk);
    }
    if(expected_offset != sample_count || compressed_offset != packet.payload.size()) {
        throw std::runtime_error("invalid pq8lz4 depth layout");
    }

    std::vector<uint8_t> quantized(expected_sample_count);
    auto &api = lz4_api();
    bounded_parallel_for(chunks.size(), [&](size_t index) {
            const auto &chunk = chunks[index];
            const int decoded_size =
                api.decompress_safe(reinterpret_cast<const char *>(packet.payload.data() + chunk.compressed_offset),
                                    reinterpret_cast<char *>(quantized.data() + chunk.sample_offset),
                                    static_cast<int>(chunk.compressed_size),
                                    static_cast<int>(chunk.sample_count));
            if(decoded_size != static_cast<int>(chunk.sample_count)) {
                throw std::runtime_error("pq8lz4 depth chunk decompression failed");
            }
        });

    std::vector<uint8_t> out(static_cast<size_t>(packet.uncompressed_size), 0);
    for(size_t i = 0; i < quantized.size(); ++i) {
        const uint8_t index = quantized[i];
        const uint32_t value = index == 0 ? 0u : std::min<uint32_t>(static_cast<uint32_t>(index) * raw_step,
                                                                    std::numeric_limits<uint16_t>::max());
        const size_t offset = i * sizeof(uint16_t);
        out[offset] = static_cast<uint8_t>(value & 0xffu);
        out[offset + 1] = static_cast<uint8_t>((value >> 8u) & 0xffu);
    }
    return out;
}

class NibbleReader {
public:
    explicit NibbleReader(const std::vector<uint8_t> &data) : data_(data) {}

    uint8_t read() {
        if(byte_index_ >= data_.size()) {
            throw std::runtime_error("truncated rvl depth payload");
        }
        const uint8_t byte = data_[byte_index_];
        uint8_t nibble = 0;
        if(read_low_) {
            nibble = byte & 0x0fu;
            read_low_ = false;
        }
        else {
            nibble = static_cast<uint8_t>((byte >> 4u) & 0x0fu);
            read_low_ = true;
            ++byte_index_;
        }
        return nibble;
    }

private:
    const std::vector<uint8_t> &data_;
    size_t byte_index_ = 0;
    bool read_low_ = true;
};

uint32_t rvl_read_vle(NibbleReader &reader) {
    uint32_t value = 0;
    uint32_t shift = 0;
    while(true) {
        const uint8_t nibble = reader.read();
        value |= static_cast<uint32_t>(nibble & 0x7u) << shift;
        if((nibble & 0x8u) == 0) {
            return value;
        }
        shift += 3u;
        if(shift >= 32u) {
            throw std::runtime_error("invalid rvl depth payload");
        }
    }
}

void write_depth_u16le(std::vector<uint8_t> &out, size_t sample_index, uint16_t value) {
    const size_t offset = sample_index * sizeof(uint16_t);
    out[offset] = static_cast<uint8_t>(value & 0xffu);
    out[offset + 1] = static_cast<uint8_t>((value >> 8u) & 0xffu);
}

std::vector<uint8_t> rvl_decompress_payload(const MediaPacket &packet) {
    if(packet.uncompressed_size == 0 || packet.uncompressed_size > kMaxReasonablePayload
       || packet.uncompressed_size % sizeof(uint16_t) != 0) {
        throw std::runtime_error("invalid rvl uncompressed depth size");
    }
    std::vector<uint8_t> out(static_cast<size_t>(packet.uncompressed_size), 0);
    const size_t sample_count = out.size() / sizeof(uint16_t);
    NibbleReader reader(packet.payload);
    int32_t previous = 0;
    size_t index = 0;
    while(index < sample_count) {
        const uint32_t zeros = rvl_read_vle(reader);
        if(zeros > sample_count - index) {
            throw std::runtime_error("invalid rvl zero run");
        }
        index += zeros;

        const uint32_t nonzeros = rvl_read_vle(reader);
        if(nonzeros > sample_count - index) {
            throw std::runtime_error("invalid rvl nonzero run");
        }
        for(uint32_t i = 0; i < nonzeros; ++i) {
            const uint32_t zigzag = rvl_read_vle(reader);
            const int64_t delta = (zigzag & 1u) != 0u ? -static_cast<int64_t>((static_cast<uint64_t>(zigzag) + 1u) >> 1u)
                                                      : static_cast<int64_t>(zigzag >> 1u);
            const int64_t current = static_cast<int64_t>(previous) + delta;
            if(current < 0 || current > static_cast<int64_t>(std::numeric_limits<uint16_t>::max())) {
                throw std::runtime_error("invalid rvl depth sample");
            }
            write_depth_u16le(out, index, static_cast<uint16_t>(current));
            previous = static_cast<int32_t>(current);
            ++index;
        }
    }
    return out;
}

uint16_t read_u16_le_checked(const std::vector<uint8_t> &data, size_t offset) {
    if(offset + 1 >= data.size()) {
        throw std::runtime_error("truncated qdelta depth payload");
    }
    return static_cast<uint16_t>(static_cast<uint32_t>(data[offset]) | (static_cast<uint32_t>(data[offset + 1]) << 8u));
}

uint32_t read_varuint_checked(const std::vector<uint8_t> &data, size_t &offset) {
    uint32_t value = 0;
    uint32_t shift = 0;
    while(offset < data.size()) {
        const uint8_t byte = data[offset++];
        value |= static_cast<uint32_t>(byte & 0x7fu) << shift;
        if((byte & 0x80u) == 0) {
            return value;
        }
        shift += 7u;
        if(shift >= 32u) {
            throw std::runtime_error("invalid qdelta varuint");
        }
    }
    throw std::runtime_error("truncated qdelta varuint");
}

int64_t zigzag_decode_i64(uint32_t value) {
    return static_cast<int64_t>(value >> 1u) ^ -static_cast<int64_t>(value & 1u);
}

std::vector<uint8_t> qdelta_decompress_payload(const MediaPacket &packet) {
    if(packet.uncompressed_size == 0 || packet.uncompressed_size > kMaxReasonablePayload
       || packet.uncompressed_size % sizeof(uint16_t) != 0) {
        throw std::runtime_error("invalid qdelta uncompressed depth size");
    }
    if(packet.payload.size() < 8 || packet.payload[0] != 'Q' || packet.payload[1] != 'D' || packet.payload[2] != 'L'
       || packet.payload[3] != '1') {
        throw std::runtime_error("invalid qdelta header");
    }
    const uint32_t raw_step = std::max<uint32_t>(1, read_u16_le_checked(packet.payload, 4));
    std::vector<uint8_t> out(static_cast<size_t>(packet.uncompressed_size), 0);
    const size_t sample_count = out.size() / sizeof(uint16_t);
    size_t offset = 8;
    size_t index = 0;
    int64_t previous = 0;
    while(index < sample_count) {
        if(offset >= packet.payload.size()) {
            throw std::runtime_error("truncated qdelta depth payload");
        }
        const uint8_t token = packet.payload[offset++];
        if(token == 0) {
            const uint32_t zeros = read_varuint_checked(packet.payload, offset);
            if(zeros > sample_count - index) {
                throw std::runtime_error("invalid qdelta zero run");
            }
            index += zeros;
            previous = 0;
            continue;
        }
        if(token != 1) {
            throw std::runtime_error("invalid qdelta token");
        }
        if(offset >= packet.payload.size()) {
            throw std::runtime_error("truncated qdelta run");
        }
        const uint32_t nonzeros = packet.payload[offset++];
        if(nonzeros == 0 || nonzeros > sample_count - index) {
            throw std::runtime_error("invalid qdelta nonzero run");
        }
        for(uint32_t i = 0; i < nonzeros; ++i) {
            const int64_t delta = zigzag_decode_i64(read_varuint_checked(packet.payload, offset));
            const int64_t quantized = previous + delta;
            if(quantized < 0 || quantized > static_cast<int64_t>(std::numeric_limits<uint32_t>::max())) {
                throw std::runtime_error("invalid qdelta depth sample");
            }
            const uint64_t scaled = static_cast<uint64_t>(quantized) * raw_step;
            const uint32_t raw_value = static_cast<uint32_t>(std::min<uint64_t>(scaled, std::numeric_limits<uint16_t>::max()));
            write_depth_u16le(out, index, static_cast<uint16_t>(raw_value));
            previous = quantized;
            ++index;
        }
    }
    return out;
}

MediaPacket normalized_depth_packet(const MediaPacket &packet) {
    if(packet.stream_type != StreamType::depth_raw) {
        return packet;
    }
    if(packet.codec_or_compression == "none") {
        return packet;
    }
    if(packet.codec_or_compression == "zlib") {
        MediaPacket decoded = media_packet_metadata_only(packet);
        decoded.payload = zlib_decompress_payload(packet);
        decoded.payload_size = decoded.payload.size();
        decoded.codec_or_compression = "none";
        return decoded;
    }
    if(packet.codec_or_compression == "rvl") {
        MediaPacket decoded = media_packet_metadata_only(packet);
        decoded.payload = rvl_decompress_payload(packet);
        decoded.payload_size = decoded.payload.size();
        decoded.codec_or_compression = "none";
        return decoded;
    }
    if(packet.codec_or_compression == "qdelta") {
        MediaPacket decoded = media_packet_metadata_only(packet);
        decoded.payload = qdelta_decompress_payload(packet);
        decoded.payload_size = decoded.payload.size();
        decoded.codec_or_compression = "none";
        return decoded;
    }
    if(packet.codec_or_compression == "lz4") {
        MediaPacket decoded = media_packet_metadata_only(packet);
        decoded.payload = lz4_decompress_payload(packet);
        decoded.payload_size = decoded.payload.size();
        decoded.codec_or_compression = "none";
        return decoded;
    }
    if(packet.codec_or_compression == "plz4") {
        MediaPacket decoded = media_packet_metadata_only(packet);
        decoded.payload = plz4_decompress_payload(packet);
        decoded.payload_size = decoded.payload.size();
        decoded.codec_or_compression = "none";
        return decoded;
    }
    if(packet.codec_or_compression == "pzlib") {
        MediaPacket decoded = media_packet_metadata_only(packet);
        decoded.payload = pzlib_decompress_payload(packet);
        decoded.payload_size = decoded.payload.size();
        decoded.codec_or_compression = "none";
        return decoded;
    }
    if(packet.codec_or_compression == "q8lz4") {
        MediaPacket decoded = media_packet_metadata_only(packet);
        decoded.payload = q8lz4_decompress_payload(packet);
        decoded.payload_size = decoded.payload.size();
        decoded.codec_or_compression = "none";
        return decoded;
    }
    if(packet.codec_or_compression == "q8zlib") {
        MediaPacket decoded = media_packet_metadata_only(packet);
        decoded.payload = q8zlib_decompress_payload(packet);
        decoded.payload_size = decoded.payload.size();
        decoded.codec_or_compression = "none";
        return decoded;
    }
    if(packet.codec_or_compression == "pq12zlib") {
        MediaPacket decoded = media_packet_metadata_only(packet);
        decoded.payload = pq12zlib_decompress_payload(packet);
        decoded.payload_size = decoded.payload.size();
        decoded.codec_or_compression = "none";
        return decoded;
    }
    if(packet.codec_or_compression == "pq8zlib") {
        MediaPacket decoded = media_packet_metadata_only(packet);
        decoded.payload = pq8zlib_decompress_payload(packet);
        decoded.payload_size = decoded.payload.size();
        decoded.codec_or_compression = "none";
        return decoded;
    }
    if(packet.codec_or_compression == "pq8lz4") {
        MediaPacket decoded = media_packet_metadata_only(packet);
        decoded.payload = pq8lz4_decompress_payload(packet);
        decoded.payload_size = decoded.payload.size();
        decoded.codec_or_compression = "none";
        return decoded;
    }
    throw std::runtime_error("unsupported depth compression: " + packet.codec_or_compression);
}

int wait_child(pid_t pid) {
    int status = 0;
    while(waitpid(pid, &status, 0) < 0) {
        if(errno == EINTR) {
            continue;
        }
        return -1;
    }
    return status;
}

int add_spawn_closefrom(posix_spawn_file_actions_t *actions) {
#if defined(__GLIBC__)
    return posix_spawn_file_actions_addclosefrom_np(actions, 3);
#else
    const long limit = std::min<long>(sysconf(_SC_OPEN_MAX), 65536);
    for(int fd = 3; fd < limit; ++fd) {
        const int rc = posix_spawn_file_actions_addclose(actions, fd);
        if(rc != 0) {
            return rc;
        }
    }
    return 0;
#endif
}

pid_t spawn_shell_process(const std::string &command, int child_stdin, int child_stdout, int child_stderr, int &error_code) {
    posix_spawn_file_actions_t actions;
    error_code = posix_spawn_file_actions_init(&actions);
    if(error_code != 0) {
        return -1;
    }
    ScopeExit destroy_actions([&actions] { posix_spawn_file_actions_destroy(&actions); });
    const auto add_dup = [&](int source, int destination) {
        if(source < 0 || source == destination) {
            return 0;
        }
        return posix_spawn_file_actions_adddup2(&actions, source, destination);
    };
    if((error_code = add_dup(child_stdin, STDIN_FILENO)) != 0
       || (error_code = add_dup(child_stdout, STDOUT_FILENO)) != 0
       || (error_code = add_dup(child_stderr, STDERR_FILENO)) != 0
       || (error_code = add_spawn_closefrom(&actions)) != 0) {
        return -1;
    }
    const char *argv[] = {"/bin/sh", "-c", command.c_str(), nullptr};
    pid_t pid = -1;
    error_code = posix_spawn(&pid, argv[0], &actions, nullptr, const_cast<char *const *>(argv), environ);
    return error_code == 0 ? pid : -1;
}

