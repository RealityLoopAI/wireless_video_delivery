struct H264StreamPacket {
    uint64_t seq = 0;
    bool has_idr = false;
    bool has_vcl = false;
    uint64_t timestamp_us = 0;
    uint64_t global_timestamp_us = 0;
    bool clock_sync_valid = false;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> payload;
};

struct H264StreamBuffer {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<H264StreamPacket> packets;
    std::vector<uint8_t> header_h264;
    uint64_t next_seq = 1;
    uint64_t last_us = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct UdpReassemblyStats {
    uint64_t datagrams = 0;
    uint64_t datagram_bytes = 0;
    uint64_t invalid_datagrams = 0;
    uint64_t valid_fragments = 0;
    uint64_t duplicate_fragments = 0;
    uint64_t assemblies_started = 0;
    uint64_t completed_packets = 0;
    uint64_t completed_bytes = 0;
    uint64_t completed_rgb_packets = 0;
    uint64_t completed_depth_packets = 0;
    uint64_t completed_preview_packets = 0;
    uint64_t parse_rejected_packets = 0;
    uint64_t stream_rejected_packets = 0;
    uint64_t expired_packets = 0;
    uint64_t expired_missing_fragments = 0;
    uint64_t evicted_packets = 0;
    uint64_t evicted_missing_fragments = 0;
    uint64_t max_active_assemblies = 0;
};

void append_udp_reassembly_stats_json(std::ostringstream &out, const UdpReassemblyStats &stats, size_t active_assemblies) {
    out << "{";
    out << "\"datagrams\":" << stats.datagrams << ',';
    out << "\"datagram_bytes\":" << stats.datagram_bytes << ',';
    out << "\"invalid_datagrams\":" << stats.invalid_datagrams << ',';
    out << "\"valid_fragments\":" << stats.valid_fragments << ',';
    out << "\"duplicate_fragments\":" << stats.duplicate_fragments << ',';
    out << "\"assemblies_started\":" << stats.assemblies_started << ',';
    out << "\"completed_packets\":" << stats.completed_packets << ',';
    out << "\"completed_bytes\":" << stats.completed_bytes << ',';
    out << "\"completed_rgb_packets\":" << stats.completed_rgb_packets << ',';
    out << "\"completed_depth_packets\":" << stats.completed_depth_packets << ',';
    out << "\"completed_preview_packets\":" << stats.completed_preview_packets << ',';
    out << "\"parse_rejected_packets\":" << stats.parse_rejected_packets << ',';
    out << "\"stream_rejected_packets\":" << stats.stream_rejected_packets << ',';
    out << "\"expired_packets\":" << stats.expired_packets << ',';
    out << "\"expired_missing_fragments\":" << stats.expired_missing_fragments << ',';
    out << "\"evicted_packets\":" << stats.evicted_packets << ',';
    out << "\"evicted_missing_fragments\":" << stats.evicted_missing_fragments << ',';
    out << "\"active_assemblies\":" << active_assemblies << ',';
    out << "\"max_active_assemblies\":" << stats.max_active_assemblies;
    out << "}";
}

constexpr uint32_t kH264PreviewFrameFlagKey = 1u << 0u;
constexpr uint32_t kH264PreviewFrameFlagConfig = 1u << 1u;
constexpr uint32_t kH264PreviewFrameFlagClockSyncValid = 1u << 2u;

std::vector<uint8_t> h264_preview_frame_header(uint32_t payload_size,
                                               uint32_t flags,
                                               uint32_t width,
                                               uint32_t height,
                                               uint64_t timestamp_us,
                                               uint64_t seq,
                                               bool include_global_timestamp,
                                               uint64_t global_timestamp_us) {
    const uint16_t version = include_global_timestamp ? 2 : 1;
    const uint16_t header_size = include_global_timestamp ? 48 : 40;
    std::vector<uint8_t> header;
    header.reserve(header_size);
    header.push_back('G');
    header.push_back('W');
    header.push_back('H');
    header.push_back('P');
    append_u16_le(header, version);
    append_u16_le(header, header_size);
    append_u32_le(header, payload_size);
    append_u32_le(header, flags);
    append_u32_le(header, width);
    append_u32_le(header, height);
    append_u64_le(header, timestamp_us);
    append_u64_le(header, seq);
    if(include_global_timestamp) {
        append_u64_le(header, global_timestamp_us);
    }
    return header;
}

bool send_h264_preview_frame(int fd,
                             const std::vector<uint8_t> &payload,
                             uint32_t flags,
                             uint32_t width,
                             uint32_t height,
                             uint64_t timestamp_us,
                             uint64_t seq,
                             bool include_global_timestamp,
                             uint64_t global_timestamp_us) {
    if(payload.empty() || payload.size() > std::numeric_limits<uint32_t>::max()) {
        return true;
    }
    const auto header = h264_preview_frame_header(static_cast<uint32_t>(payload.size()), flags, width, height,
                                                  timestamp_us, seq, include_global_timestamp, global_timestamp_us);
    return send_all_with_timeout(fd, header.data(), header.size(), kRgbH264ClientSendTimeoutMs)
           && send_all_with_timeout(fd, payload.data(), payload.size(), kRgbH264ClientSendTimeoutMs);
}

struct CameraState {
    explicit CameraState(std::string sender, std::string camera)
        : sender_id(std::move(sender)), camera_id(std::move(camera)), key(camera_key(sender_id, camera_id)) {}

    std::string sender_id;
    std::string camera_id;
    std::string key;
    std::string camera_name;
    std::string camera_file_prefix;
    std::mutex preview_mutex;
    uint64_t recording_start_us = 0;
    RecordingWindow recording_window;
    std::string recording_file_prefix;
    bool online = true;
    bool recording_requested = false;
    bool recording_start_pending = false;
    uint64_t last_status_us = 0;
    std::string status_endpoint;
    uint64_t last_media_us = 0;
    uint64_t last_media_session_id = 0;
    uint64_t rgb_ingress_session_id = 0;
    bool rgb_ingress_waiting_for_idr = false;
    uint64_t rgb_ingress_keyframe_drops = 0;
    uint64_t rgb_ingress_recoveries = 0;
    uint64_t rgb_ingress_keyframe_requests = 0;
    uint64_t rgb_packets = 0;
    uint64_t depth_packets = 0;
    uint64_t rgb_bytes = 0;
    uint64_t depth_bytes = 0;
    uint64_t rgb_preview_us = 0;
    uint32_t rgb_preview_width = 0;
    uint32_t rgb_preview_height = 0;
    StreamType rgb_preview_decoder_source = StreamType::rgb;
    std::vector<uint8_t> rgb_preview_prefix_h264;
    std::unique_ptr<RgbPreviewDecoder> rgb_decoder;
    uint64_t rgb_preview_requested_until_us = 0;
    uint64_t last_web_rgb_preview_control_us = 0;
    uint64_t last_web_rgb_preview_keyframe_us = 0;
    uint64_t last_rgb_preview_packet_us = 0;
    uint64_t rgb_stream_requested_until_us = 0;
    uint64_t rgb_main_stream_requested_until_us = 0;
    H264StreamBuffer rgb_stream;
    H264StreamBuffer rgb_preview_stream;
    uint64_t main_rgb_preview_us = 0;
    uint32_t main_rgb_preview_width = 0;
    uint32_t main_rgb_preview_height = 0;
    std::unique_ptr<RgbPreviewDecoder> main_rgb_decoder;
    uint64_t main_rgb_preview_requested_until_us = 0;
    uint64_t depth_preview_us = 0;
    uint32_t depth_preview_width = 0;
    uint32_t depth_preview_height = 0;
    uint64_t depth_preview_requested_until_us = 0;
    std::vector<uint8_t> depth_preview_ppm;
    double depth_scale = 1.0;
    std::string last_error;
    std::string sender_build_commit;
    std::string sender_build_source_hash;
    bool sender_build_dirty = false;
    double sender_rgb_input_fps = 0.0;
    double sender_depth_input_fps = 0.0;
    double sender_rgb_sent_fps = 0.0;
    double sender_depth_sent_fps = 0.0;
    uint64_t sender_rgb_dropped_frames = 0;
    uint64_t sender_depth_dropped_frames = 0;
    uint64_t sender_rgb_transport_retry_drops = 0;
    uint64_t sender_rgb_send_failures = 0;
    uint64_t sender_depth_send_failures = 0;
    bool sender_publish_warmup_active = false;
    uint64_t sender_publish_warmup_drops = 0;
    std::string last_announce_json;
    bool last_announce_live = false;
    uint64_t last_announce_received_us = 0;
    uint64_t last_announce_cache_save_us = 0;
    uint64_t last_status_log_us = 0;
    std::mutex segment_mutex;
    bool segment_active = false;
    bool segment_finalizing = false;
    std::string segment_dir;
    uint64_t segment_start_us = 0;
    uint64_t global_segment_index = 0;
    uint64_t segment_window_start_global_us = 0;
    uint64_t segment_window_end_global_us = 0;
    std::atomic<bool> segment_rotation_requested{false};
    std::atomic<uint64_t> segment_rotation_keyframe_requested_us{0};
    std::atomic<uint64_t> segment_rotation_keyframe_requests{0};
    std::atomic<uint64_t> segment_prestart_depth_drops{0};
    std::atomic<uint64_t> segment_prestart_rgb_drops{0};
    std::atomic<uint64_t> media_idle_finalizations{0};
    std::unique_ptr<SegmentWriter> segment = std::make_unique<SegmentWriter>();
    std::atomic<size_t> segment_finalize_pending{0};
    std::atomic<bool> segment_finalize_active{false};
    std::atomic<uint64_t> segment_finalize_completed{0};
    std::atomic<uint64_t> segment_finalize_failures{0};
    std::atomic<uint64_t> segment_finalize_last_duration_ms{0};
    std::mutex record_mutex;
    std::condition_variable record_cv;
    std::deque<RecordJob> record_queue;
    bool record_accepting = false;
    bool record_finalizing = false;
    bool record_storage_capacity_failed = false;
    uint64_t record_generation = 0;
    size_t record_queue_bytes = 0;
    size_t record_queue_peak_bytes = 0;
    size_t record_queue_peak_packets = 0;
    uint64_t record_prequeue_peak_delay_us = 0;
    uint64_t record_queue_peak_wait_us = 0;
    uint64_t record_enqueued_packets = 0;
    uint64_t record_dequeued_packets = 0;
    uint64_t record_backpressure_waits = 0;
    uint64_t record_oversize_packets = 0;
    uint64_t record_write_errors = 0;
    uint64_t last_record_write_error_log_us = 0;
    uint64_t last_finalize_queue_full_log_us = 0;
    uint32_t record_active_writes = 0;
    bool record_worker_started = false;
    bool record_worker_stop = false;
    std::thread record_worker;

    std::string storage_key() const {
        return camera_name.empty() ? key : camera_name;
    }
};

