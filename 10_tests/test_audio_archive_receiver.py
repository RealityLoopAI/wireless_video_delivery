#!/usr/bin/env python3
import importlib.util
import json
import struct
import sys
import tempfile
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
    test_verified_atomic_nas_publish(module)
    test_local_finalize_is_separate_from_publish(module)
    test_config_defaults_and_uniqueness(module)
    print("audio archive receiver tests passed")


if __name__ == "__main__":
    main()
