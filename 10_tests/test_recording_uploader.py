#!/usr/bin/env python3
import argparse
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import time


def atoms(path: Path) -> set[bytes]:
    result: set[bytes] = set()
    size = path.stat().st_size
    offset = 0
    with path.open("rb") as handle:
        while offset + 8 <= size:
            handle.seek(offset)
            header = handle.read(8)
            assert len(header) == 8
            atom_size = int.from_bytes(header[:4], "big")
            atom_type = header[4:8]
            header_size = 8
            if atom_size == 1:
                atom_size = int.from_bytes(handle.read(8), "big")
                header_size = 16
            elif atom_size == 0:
                atom_size = size - offset
            assert header_size <= atom_size <= size - offset
            result.add(atom_type)
            offset += atom_size
    assert offset == size
    return result


def remove_closing_mfra(path: Path) -> None:
    size = path.stat().st_size
    offset = 0
    with path.open("r+b") as handle:
        while offset + 8 <= size:
            handle.seek(offset)
            header = handle.read(8)
            assert len(header) == 8
            atom_size = int.from_bytes(header[:4], "big")
            atom_type = header[4:8]
            header_size = 8
            if atom_size == 1:
                atom_size = int.from_bytes(handle.read(8), "big")
                header_size = 16
            elif atom_size == 0:
                atom_size = size - offset
            assert header_size <= atom_size <= size - offset
            if atom_type == b"mfra":
                assert offset + atom_size == size
                handle.truncate(offset)
                return
            offset += atom_size
    raise AssertionError("fixture has no closing mfra atom")


def run(args: argparse.Namespace) -> None:
    ffmpeg = shutil.which("ffmpeg")
    ffprobe = shutil.which("ffprobe")
    if not ffmpeg or not ffprobe:
        print("recording uploader fault-injection test skipped: ffmpeg/ffprobe unavailable")
        return

    with tempfile.TemporaryDirectory(prefix="gwv3_uploader_test_") as temporary_text:
        temporary = Path(temporary_text)
        staging_root = temporary / "staging"
        nas_root = temporary / "nas"
        segment = staging_root / "camera-a" / "2026-07-21" / "120000"
        segment.mkdir(parents=True)
        prefix = "test_Short_0048_"
        rgb_path = segment / f"{prefix}rgb.mp4"
        generated = subprocess.run(
            [
                ffmpeg,
                "-hide_banner",
                "-loglevel",
                "error",
                "-f",
                "lavfi",
                "-i",
                "testsrc2=size=128x80:rate=30",
                "-frames:v",
                "90",
                "-c:v",
                "libx264",
                "-preset",
                "ultrafast",
                "-movflags",
                "+frag_keyframe+empty_moov+default_base_moof",
                str(rgb_path),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=30,
            check=False,
        )
        assert generated.returncode == 0, generated.stderr.decode(errors="replace")
        assert b"moof" in atoms(rgb_path)
        assert b"mfra" in atoms(rgb_path)
        assert b"sidx" not in atoms(rgb_path)
        invalid_segment = temporary / "invalid_mfra"
        invalid_segment.mkdir()
        invalid_rgb = invalid_segment / "rgb.mp4"
        shutil.copy2(rgb_path, invalid_rgb)
        remove_closing_mfra(invalid_rgb)
        assert b"mfra" not in atoms(invalid_rgb)
        spec = importlib.util.spec_from_file_location("recording_uploader_under_test", args.uploader)
        assert spec is not None and spec.loader is not None
        uploader_module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(uploader_module)
        try:
            uploader_module.finalize_rgb(invalid_segment, invalid_rgb.name, ffmpeg)
        except RuntimeError as error:
            assert "no closing mfra atom" in str(error)
        else:
            raise AssertionError("fragmented MP4 without mfra was accepted")
        (segment / f"{prefix}frames.csv").write_text("local_time_us,stream_type\n1,rgb\n", encoding="ascii")
        (segment / f"{prefix}meta.json").write_text(
            json.dumps({"closed": True, "recording_ready_file": f"{prefix}recording_ready.json"}), encoding="ascii"
        )
        staged = {
            "schema": "gwv3_recording_staged_v1",
            "staged": True,
            "segment_start_us": 1784625600000000,
            "segment_end_us": 1784625603000000,
            "sender_id": "sender-a",
            "camera_id": "cam01",
            "relative_path": "camera-a/2026-07-21/120000",
            "frames_file": f"{prefix}frames.csv",
            "meta_file": f"{prefix}meta.json",
            "ready_file": f"{prefix}recording_ready.json",
            "rgb_file": f"{prefix}rgb.mp4",
            "depth_file": f"{prefix}depth.mkv",
        }
        (segment / f"{prefix}recording_staged.json").write_text(json.dumps(staged), encoding="ascii")

        config = {
            "nas_root": str(nas_root),
            "ffmpeg_path": ffmpeg,
            "recording_staging": {
                "enabled": True,
                "root": str(staging_root),
                "upload_interval_ms": 250,
                "delete_after_upload": True,
            },
        }
        config_path = temporary / "receiver.json"
        config_path.write_text(json.dumps(config), encoding="ascii")

        # A regular file at nas_root simulates an unavailable/broken mount.
        nas_root.write_text("offline", encoding="ascii")
        first = subprocess.run(
            [args.python, args.uploader, "--config", str(config_path), "--once"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=30,
            check=False,
        )
        assert first.returncode == 2, (first.stdout, first.stderr)
        assert segment.exists(), "failed upload removed the only local recording"
        assert (segment / f"{prefix}recording_ready.json").is_file(), "local finalization did not survive NAS failure"
        assert b"moof" not in atoms(rgb_path), "local staged MP4 was not converted to a conventional MP4"

        nas_root.unlink()
        nas_root.mkdir()
        (segment / "aaa_interruption_probe.bin").write_bytes(b"x" * (2 * 1024 * 1024))
        config["recording_staging"]["upload_bandwidth_limit_mbps"] = 1
        config_path.write_text(json.dumps(config), encoding="ascii")
        interrupted = subprocess.Popen(
            [args.python, args.uploader, "--config", str(config_path), "--once"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        deadline = time.monotonic() + 10
        active_status = {}
        while time.monotonic() < deadline:
            status_path = staging_root / ".gwv3_uploader_status.json"
            if status_path.is_file():
                active_status = json.loads(status_path.read_text(encoding="utf-8"))
            if (
                list(nas_root.rglob(".gwv3-uploading-*"))
                and active_status.get("active_phase") == "uploading"
                and int(active_status.get("active_bytes_total", 0)) > 0
            ):
                break
            assert interrupted.poll() is None, "uploader exited before interruption point"
            time.sleep(0.02)
        else:
            interrupted.kill()
            raise AssertionError("uploader did not enter the NAS copy phase")
        assert active_status["active_segment"] == str(segment)
        assert 0 <= float(active_status["active_progress_percent"]) <= 100
        assert int(active_status["active_elapsed_ms"]) >= 0
        interrupted.terminate()
        interrupted.wait(timeout=5)
        assert segment.exists(), "interrupted upload removed the local segment"

        config["recording_staging"]["upload_bandwidth_limit_mbps"] = 0
        config_path.write_text(json.dumps(config), encoding="ascii")
        second = subprocess.run(
            [args.python, args.uploader, "--config", str(config_path), "--once"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=30,
            check=False,
        )
        assert second.returncode == 0, (second.stdout, second.stderr)
        destination = nas_root / "camera-a" / "2026-07-21" / "120000"
        assert not segment.exists(), "successfully published local segment was not released"
        assert json.loads((destination / f"{prefix}recording_ready.json").read_text(encoding="utf-8"))["ready"] is True
        assert not list(nas_root.rglob(".gwv3-uploading-*")), "stale interrupted upload directory was not cleaned"
        assert b"moov" in atoms(destination / f"{prefix}rgb.mp4") and b"moof" not in atoms(destination / f"{prefix}rgb.mp4")
        probe = subprocess.run(
            [ffprobe, "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", str(destination / f"{prefix}rgb.mp4")],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=10,
            check=False,
        )
        assert probe.returncode == 0 and float(probe.stdout) > 2.0, probe.stderr
        status = json.loads((staging_root / ".gwv3_uploader_status.json").read_text(encoding="utf-8"))
        assert status["pending_segments"] == 0
        assert status["completed_segments"] == 1

        unsafe_segment = staging_root / "camera-unsafe" / "2026-07-21" / "120100"
        unsafe_segment.mkdir(parents=True)
        unsafe_marker = dict(staged)
        unsafe_marker["relative_path"] = "camera-unsafe/2026-07-21/120100"
        unsafe_marker["frames_file"] = "../escape.csv"
        (unsafe_segment / "recording_staged.json").write_text(json.dumps(unsafe_marker), encoding="ascii")
        unsafe = subprocess.run(
            [args.python, args.uploader, "--config", str(config_path), "--once"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=30,
            check=False,
        )
        assert unsafe.returncode == 2, (unsafe.stdout, unsafe.stderr)
        assert b"unsafe frames_file" in unsafe.stderr
        assert unsafe_segment.exists(), "rejected staging marker must remain available for inspection"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--uploader", required=True)
    parser.add_argument("--python", default="python3")
    return parser.parse_args()


if __name__ == "__main__":
    run(parse_args())
