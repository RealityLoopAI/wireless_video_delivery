#!/usr/bin/env python3
import argparse
import importlib.util
import json
from pathlib import Path
import tempfile
import zlib


def load_module(path: Path):
    spec = importlib.util.spec_from_file_location("gwv3_photo_uploader_under_test", path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def stage_job(staging_root: Path, request_id: str, relative_path: str, jpeg: bytes, *, valid_crc: bool = True) -> Path:
    job = staging_root / request_id
    job.mkdir(parents=True)
    (job / "photo.jpg").write_bytes(jpeg)
    marker = {
        "schema_version": 1,
        "ready": True,
        "request_id": request_id,
        "jpeg_file": "photo.jpg",
        "jpeg_size": len(jpeg),
        "jpeg_crc32": (zlib.crc32(jpeg) & 0xFFFFFFFF) if valid_crc else 0,
        "relative_path": relative_path,
    }
    (job / "photo_ready.json").write_text(json.dumps(marker), encoding="utf-8")
    return job


def run(args: argparse.Namespace) -> None:
    module = load_module(args.uploader)
    module.STOP_REQUESTED = False
    with tempfile.TemporaryDirectory(prefix="gwv3_photo_uploader_test_") as temporary_text:
        temporary = Path(temporary_text)
        staging_root = temporary / "staging" / ".gwv3_photo_queue"
        nas_root = temporary / "nas"
        staging_root.mkdir(parents=True)
        nas_root.mkdir()
        config_path = temporary / "receiver.json"
        config_path.write_text(
            json.dumps(
                {
                    "nas_root": str(nas_root),
                    "photo_capture": {
                        "enabled": True,
                        "staging_root": str(staging_root),
                        "upload_interval_ms": 50,
                    },
                }
            ),
            encoding="utf-8",
        )
        uploader = module.PhotoUploader(config_path, require_nas_mount=False)

        relative = "voice_photos/sender_cam01/2026-07-28/12-00-00/20260728_120000.jpg"
        jpeg_a = b"\xff\xd8original-mjpeg-a\xff\xd9"
        first_job = stage_job(staging_root, "request-a", relative, jpeg_a)
        assert uploader.run_once() == 1
        destination = nas_root / relative
        assert destination.read_bytes() == jpeg_a
        assert not first_job.exists()

        idempotent_job = stage_job(staging_root, "request-b", relative, jpeg_a)
        assert uploader.run_once() == 1
        assert destination.read_bytes() == jpeg_a
        assert not idempotent_job.exists()

        jpeg_b = b"\xff\xd8original-mjpeg-b\xff\xd9"
        collision_job = stage_job(staging_root, "request-c", relative, jpeg_b)
        assert uploader.run_once() == 1
        collision_destination = destination.with_name("20260728_120000_001.jpg")
        assert collision_destination.read_bytes() == jpeg_b
        assert not collision_job.exists()

        corrupt_job = stage_job(
            staging_root,
            "request-corrupt",
            "voice_photos/sender_cam01/2026-07-28/12-00-01/20260728_120001.jpg",
            jpeg_a,
            valid_crc=False,
        )
        traversal_job = stage_job(staging_root, "request-traversal", "../outside.jpg", jpeg_a)
        assert uploader.run_once() == 0
        assert corrupt_job.exists()
        assert traversal_job.exists()
        assert not (temporary / "outside.jpg").exists()
        assert uploader.failures >= 2

        unmounted_config = module.PhotoUploader(config_path, require_nas_mount=True)
        waiting_job = stage_job(
            staging_root,
            "request-waiting",
            "voice_photos/sender_cam01/2026-07-28/12-00-02/20260728_120002.jpg",
            jpeg_a,
        )
        assert unmounted_config.run_once() == 0
        assert waiting_job.exists()

    print("photo uploader tests passed")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--uploader",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "05_tools" / "photo_uploader.py",
    )
    return parser.parse_args()


if __name__ == "__main__":
    run(parse_args())
