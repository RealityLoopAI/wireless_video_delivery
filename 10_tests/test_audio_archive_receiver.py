#!/usr/bin/env python3
import importlib.util
import json
import socket
import struct
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "05_tools" / "audio_archive_receiver.py"


def load_module():
    spec = importlib.util.spec_from_file_location("audio_archive_receiver", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def test_rtp_parser(module):
    packet = struct.pack("!BBHII", 0x80, 111, 65535, 0xFFFFFF00, 99) + b"payload"
    parsed = module.parse_rtp(packet, 123456)
    assert parsed is not None
    assert parsed.sequence == 65535
    assert parsed.timestamp == 0xFFFFFF00
    assert parsed.ssrc == 99
    assert parsed.payload_type == 111
    assert parsed.payload == b"payload"
    assert module.parse_rtp(b"short", 0) is None
    rebuilt = module.make_rtp(1, 960, 99, 111, module.OPUS_SILENCE_20_MS)
    assert module.parse_rtp(rebuilt, 1).payload == module.OPUS_SILENCE_20_MS
    assert module.signed_rtp_delta(100, 0xFFFFFFF0) == 116


def test_global_timestamp_mapping(module):
    registry = module.TimingRegistry("http://unused", 10000)
    registry.update_anchor(
        "sender-a",
        {
            "stream_instance_id": "stream-a",
            "rtp_timestamp": 48000,
            "sender_system_timestamp_us": 1_000_000,
            "sender_send_timestamp_us": 1_020_000,
            "sample_rate": 48000,
            "ssrc": 7,
        },
        module.now_us(),
    )
    with registry._lock:
        registry._models["sender-a"] = module.ClockModel(
            valid=True,
            offset_us=2500,
            delay_us=900,
            drift_ppm=1.25,
            receiver_update_us=module.now_us(),
        )
    packet = module.RtpPacket(1, 48960, 7, 111, False, b"opus", module.now_us())
    mapped = registry.map_packet("sender-a", packet)
    assert mapped.sender_system_us == 1_020_000
    assert mapped.global_us == 1_022_500
    assert mapped.clock_valid is True
    assert mapped.offset_us == 2500


def test_native_ogg_opus_muxer(module):
    with tempfile.TemporaryDirectory() as temp:
        path = Path(temp) / "audio.opus"
        writer = module.OggOpusWriter(path, 48000)
        for _ in range(200):
            writer.write_packet(module.OPUS_SILENCE_20_MS, 960)
        writer.close()
        content = path.read_bytes()
        assert len(content) > 1000
        offset = 0
        page_count = 0
        last_header_type = 0
        while offset < len(content):
            assert content[offset : offset + 4] == b"OggS"
            last_header_type = content[offset + 5]
            segments = content[offset + 26]
            body_size = sum(content[offset + 27 : offset + 27 + segments])
            page_size = 27 + segments + body_size
            page = bytearray(content[offset : offset + page_size])
            expected_crc = struct.unpack_from("<I", page, 22)[0]
            struct.pack_into("<I", page, 22, 0)
            assert module.ogg_crc(page) == expected_crc
            offset += page_size
            page_count += 1
        assert page_count >= 6
        assert last_header_type & 0x04


def test_audio_quality_gate(module):
    assert module.classify_audio_quality(1000, 995, 10)[:2] == ("complete", True)
    assert module.classify_audio_quality(1000, 980, 10)[:2] == ("partial", False)
    assert module.classify_audio_quality(1000, 999, 51)[:2] == ("partial", False)
    assert module.classify_audio_quality(1000, 0, 1000)[:2] == ("no_input", False)


def test_task_audio_no_input_has_no_false_audio_file(module):
    with tempfile.TemporaryDirectory() as temp:
        directory = Path(temp) / "video-segment"
        spec = module.TaskAudioSpec("sender-a", "cam01", directory, 7, 1_000_000, 1_100_000)
        segment = module.TaskAudioSegment(spec, 48000, 111, 9)
        for slot in range(segment.start_slot, segment.end_slot):
            segment.write_slot(slot * 20_000, 0, 0, None)
        ready = segment.close_to_video_directory("test")
        assert ready["quality_status"] == "no_input"
        assert ready["audio_valid"] is False
        assert not (directory / "audio.opus").exists()
        assert (directory / "audio_timing.csv").exists()
        assert (directory / "audio_meta.json").exists()
        assert (directory / "audio_ready.json").exists()


def test_task_audio_reuses_opus_and_reports_quality(module):
    with tempfile.TemporaryDirectory() as temp:
        directory = Path(temp) / "video-segment"
        spec = module.TaskAudioSpec("sender-a", "cam01", directory, 8, 2_000_000, 4_000_000)
        segment = module.TaskAudioSegment(spec, 48000, 111, 9)
        for index, slot in enumerate(range(segment.start_slot, segment.end_slot)):
            packet = None if index == 25 else module.RtpPacket(
                index,
                index * 960,
                9,
                111,
                False,
                module.OPUS_SILENCE_20_MS,
                slot * 20_000,
                global_us=slot * 20_000,
                clock_valid=True,
            )
            segment.write_slot(slot * 20_000, index, index * 960, packet)
        ready = segment.close_to_video_directory("test")
        assert ready["quality_status"] == "complete"
        assert ready["received_packets"] == 99
        assert ready["expected_packets"] == 100
        assert (directory / "audio.opus").exists()
        meta = json.loads((directory / "audio_meta.json").read_text(encoding="utf-8"))
        assert meta["encoded_packet_reuse"] is True
        assert meta["silence_packets"] == 1


def test_task_audio_real_silent_rtp_is_retained(module):
    with tempfile.TemporaryDirectory() as temp:
        directory = Path(temp) / "silent-audio-segment"
        spec = module.TaskAudioSpec("sender-a", "cam01", directory, 9, 5_000_000, 5_200_000)
        segment = module.TaskAudioSegment(spec, 48000, 111, 9)
        for index, slot in enumerate(range(segment.start_slot, segment.end_slot)):
            packet = module.RtpPacket(
                index,
                index * 960,
                9,
                111,
                False,
                module.OPUS_SILENCE_20_MS,
                slot * 20_000,
                global_us=slot * 20_000,
                clock_valid=True,
            )
            segment.write_slot(slot * 20_000, index, index * 960, packet)
        ready = segment.close_to_video_directory("test-silent-input")
        assert ready["quality_status"] == "complete"
        assert ready["received_packets"] == ready["expected_packets"] == 10
        assert (directory / "audio.opus").exists()
        meta = json.loads((directory / "audio_meta.json").read_text(encoding="utf-8"))
        assert meta["silence_packets"] == 0


def free_port(socket_type):
    with socket.socket(socket.AF_INET, socket_type) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def test_task_audio_udp_integration(module):
    class StatusHandler(BaseHTTPRequestHandler):
        def do_GET(self):
            body = json.dumps({"clock_sync": [], "cameras": []}).encode()
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *_args):
            return

    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        receiver = ThreadingHTTPServer(("127.0.0.1", 0), StatusHandler)
        receiver_thread = threading.Thread(target=receiver.serve_forever, daemon=True)
        receiver_thread.start()
        rtp_port = free_port(socket.SOCK_DGRAM)
        timing_port = free_port(socket.SOCK_DGRAM)
        admin_port = free_port(socket.SOCK_STREAM)
        config_path = root / "audio.json"
        video_root = root / "video"
        (root / "nas").mkdir()
        config_path.write_text(
            json.dumps(
                {
                    "bind_ip": "127.0.0.1",
                    "timing_port": timing_port,
                    "admin_bind_ip": "127.0.0.1",
                    "admin_port": admin_port,
                    "receiver_admin_url": f"http://127.0.0.1:{receiver.server_port}",
                    "staging_root": str(root / "staging"),
                    "nas_root": str(root / "nas"),
                    "nas_require_mount": False,
                    "nas_low_space_warning_mb": 0,
                    "min_free_disk_mb": 0,
                    "task_poll_interval_ms": 50,
                    "task_status_fallback_seconds": 0.5,
                    "task_allowed_roots": [str(video_root)],
                    "streams": [{"sender_id": "sender-a", "port": rtp_port, "ssrc": 9}],
                }
            ),
            encoding="utf-8",
        )
        service = module.AudioArchiveService(module.load_config(config_path))
        service.start()
        service.set_all_enabled(False)
        try:
            start_us = ((module.now_us() + 500_000 + 19_999) // 20_000) * 20_000
            end_us = start_us + 1_000_000
            directory = video_root / "sender-a_cam01" / "segment"
            directory.mkdir(parents=True)
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sender:
                anchor = {
                    "message_type": "audio_timing_anchor",
                    "sender_id": "sender-a",
                    "stream_instance_id": "test-stream",
                    "rtp_timestamp": 0,
                    "sender_system_timestamp_us": start_us,
                    "sender_send_timestamp_us": start_us + 20_000,
                    "sample_rate": 48000,
                    "ssrc": 9,
                }
                sender.sendto(json.dumps(anchor).encode(), ("127.0.0.1", timing_port))
                task = {
                    "message_type": "video_task_audio_start",
                    "sender_id": "sender-a",
                    "camera_id": "cam01",
                    "recording_session_id": 99,
                    "segment_dir": str(directory),
                    "segment_window_start_global_us": start_us,
                    "segment_window_end_global_us": end_us,
                }
                sender.sendto(json.dumps(task).encode(), ("127.0.0.1", timing_port))
                time.sleep(0.1)
                for index in range(50):
                    sender.sendto(
                        module.make_rtp(index, index * 960, 9, 111, module.OPUS_SILENCE_20_MS),
                        ("127.0.0.1", rtp_port),
                    )
                task["message_type"] = "video_task_audio_finalize"
                task["segment_end_global_us"] = end_us
                sender.sendto(json.dumps(task).encode(), ("127.0.0.1", timing_port))
            deadline = time.monotonic() + 4.0
            while time.monotonic() < deadline and not (directory / "audio_ready.json").exists():
                time.sleep(0.05)
            ready = json.loads((directory / "audio_ready.json").read_text(encoding="utf-8"))
            assert ready["quality_status"] == "complete"
            assert ready["received_packets"] == 50
            assert (directory / "audio.opus").exists()
        finally:
            service.stop()
            receiver.shutdown()
            receiver.server_close()
            receiver_thread.join(timeout=1.0)


def test_verified_atomic_nas_publish(module):
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        staging = root / "staging"
        nas = root / "nas"
        staged = staging / "segments" / "sender-a" / "2026-08-20" / "120000.segment.staged"
        staged.mkdir(parents=True)
        nas.mkdir()
        files = {}
        for name, content in {
            "audio.opus": b"opus-data",
            "audio_timing.csv": b"header\nrow\n",
            "audio_meta.json": b"{}\n",
        }.items():
            path = staged / name
            path.write_bytes(content)
            files[name] = {"size": len(content), "sha256": module.sha256_file(path)}
        (staged / "audio_staged.json").write_text(
            json.dumps({"segment_id": "segment", "files": files}),
            encoding="utf-8",
        )

        class FakeApp:
            staging_root = staging
            nas_root = nas
            uploader_wakeup = __import__("threading").Event()
            config = {
                "nas_require_mount": False,
                "nas_low_space_warning_mb": 0,
                "nas_subdirectory": "audio",
                "upload_interval_seconds": 0.1,
            }

            def refresh_storage_state(self):
                return None

        uploader = module.AudioUploader(FakeApp())
        uploader._publish(staged)
        published = nas / "audio" / "sender-a" / "2026-08-20" / "120000"
        assert not staged.exists()
        assert (published / "audio.opus").read_bytes() == b"opus-data"
        ready = json.loads((published / "audio_ready.json").read_text(encoding="utf-8"))
        assert ready["files"] == files
        assert not (nas / ".gwv3_audio_uploading" / "sender-a" / "2026-08-20" / "120000.segment").exists()


def test_local_finalize_is_separate_from_publish(module):
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        pending = root / "staging" / "segments" / "sender-a" / "2026-08-20" / "120000.segment.finalize"
        pending.mkdir(parents=True)
        (pending / "audio.opus").write_bytes(b"valid-opus-placeholder")
        (pending / "audio_timing.csv").write_text("global_timestamp_us\n1\n", encoding="utf-8")
        (pending / "audio_meta.json").write_text(
            json.dumps({"segment_id": "segment"}),
            encoding="utf-8",
        )

        class FakeApp:
            staging_root = root / "staging"
            nas_root = root / "nas"
            uploader_wakeup = __import__("threading").Event()
            config = {"upload_interval_seconds": 1}

        staged = module.AudioUploader(FakeApp())._stage(pending)
        assert staged.name.endswith(".staged")
        marker = json.loads((staged / "audio_staged.json").read_text(encoding="utf-8"))
        assert set(marker["files"]) == {"audio.opus", "audio_timing.csv", "audio_meta.json"}


def test_restart_fragments_keep_longest_at_canonical_path(module):
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        staging_root = root / "staging"
        nas_root = root / "nas"
        nas_root.mkdir()
        staging_path = staging_root
        nas_path = nas_root

        class FakeApp:
            staging_root = staging_path
            nas_root = nas_path
            uploader_wakeup = __import__("threading").Event()
            config = {
                "nas_require_mount": False,
                "nas_low_space_warning_mb": 0,
                "nas_subdirectory": "audio",
                "upload_interval_seconds": 1,
            }

        def make_staged(segment_id, duration_us, content):
            directory = (
                staging_root
                / "segments"
                / "sender-a"
                / "2026-08-20"
                / f"120000.{segment_id}.staged"
            )
            directory.mkdir(parents=True)
            (directory / "audio.opus").write_bytes(content)
            (directory / "audio_timing.csv").write_text("global_timestamp_us\n1\n", encoding="utf-8")
            (directory / "audio_meta.json").write_text(
                json.dumps({"segment_id": segment_id, "audio_duration_us": duration_us}),
                encoding="utf-8",
            )
            files = {}
            for name in ("audio.opus", "audio_timing.csv", "audio_meta.json"):
                path = directory / name
                files[name] = {"size": path.stat().st_size, "sha256": module.sha256_file(path)}
            (directory / "audio_staged.json").write_text(
                json.dumps({"segment_id": segment_id, "files": files}),
                encoding="utf-8",
            )
            return directory

        uploader = module.AudioUploader(FakeApp())
        uploader._publish(make_staged("shortseg", 1_000_000, b"short"))
        uploader._publish(make_staged("longseg", 4_000_000, b"long"))
        canonical = nas_root / "audio" / "sender-a" / "2026-08-20" / "120000"
        assert (canonical / "audio.opus").read_bytes() == b"long"
        partials = list(canonical.parent.glob("120000-partial-shortseg*"))
        assert len(partials) == 1
        assert (partials[0] / "audio.opus").read_bytes() == b"short"
        uploader._publish(make_staged("tinyseg", 500_000, b"tiny"))
        assert (canonical / "audio.opus").read_bytes() == b"long"
        assert len(list(canonical.parent.glob("120000-partial-tinyseg*"))) == 1


def test_config_defaults_and_uniqueness(module):
    with tempfile.TemporaryDirectory() as temp:
        path = Path(temp) / "audio.json"
        path.write_text(
            json.dumps({"streams": [{"sender_id": "a", "port": 50030, "ssrc": 1}]}),
            encoding="utf-8",
        )
        config = module.load_config(path)
        assert config["segment_seconds"] == 900
        assert config["timing_port"] == 50130
        path.write_text(
            json.dumps(
                {
                    "streams": [
                        {"sender_id": "a", "port": 50030, "ssrc": 1},
                        {"sender_id": "b", "port": 50030, "ssrc": 2},
                    ]
                }
            ),
            encoding="utf-8",
        )
        try:
            module.load_config(path)
        except ValueError:
            pass
        else:
            raise AssertionError("duplicate stream port accepted")


def main():
    module = load_module()
    test_rtp_parser(module)
    test_global_timestamp_mapping(module)
    test_native_ogg_opus_muxer(module)
    test_audio_quality_gate(module)
    test_task_audio_no_input_has_no_false_audio_file(module)
    test_task_audio_reuses_opus_and_reports_quality(module)
    test_task_audio_real_silent_rtp_is_retained(module)
    test_task_audio_udp_integration(module)
    test_verified_atomic_nas_publish(module)
    test_local_finalize_is_separate_from_publish(module)
    test_restart_fragments_keep_longest_at_canonical_path(module)
    test_config_defaults_and_uniqueness(module)
    print("audio archive receiver tests passed")


if __name__ == "__main__":
    main()
