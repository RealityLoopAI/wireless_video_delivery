#!/usr/bin/env python3

from __future__ import annotations

import csv
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "05_tools" / "sync_input_guard.py"
SPEC = importlib.util.spec_from_file_location("sync_input_guard", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
sync_input_guard = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = sync_input_guard
SPEC.loader.exec_module(sync_input_guard)


CSV_FIELDS = [
    "stream_type",
    "frame_id",
    "frame_system_timestamp_us",
    "global_timestamp_us",
    "rgb_recorded",
    "rgb_video_frame_index",
]


class SyncInputGuardTest(unittest.TestCase):
    def make_recording(
        self,
        root: Path,
        *,
        closed: bool = True,
        indexes: tuple[int, ...] = (0, 1, 2),
        timestamps: tuple[int, ...] = (1_000_000, 1_033_333, 1_066_666),
        ready_marker: bool = False,
    ) -> Path:
        root.mkdir()
        frames_name = "test_frames.csv"
        rgb_name = "test_rgb.mp4"
        meta = {
            "camera_key": "sender_cam01",
            "closed": closed,
            "frames_file": frames_name,
            "rgb_file": rgb_name,
            "rgb_frames": len(indexes),
        }
        if ready_marker:
            meta["frames_publish_state"] = "finalized"
            meta["recording_ready_file"] = "test_recording_ready.json"
            (root / "test_recording_ready.json").write_text(
                json.dumps({"ready": True}), encoding="utf-8"
            )
        (root / "test_meta.json").write_text(json.dumps(meta), encoding="utf-8")
        (root / rgb_name).write_bytes(b"mp4 fixture")
        with (root / frames_name).open("w", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(handle, fieldnames=CSV_FIELDS)
            writer.writeheader()
            for frame_id, (index, timestamp) in enumerate(zip(indexes, timestamps), start=100):
                writer.writerow(
                    {
                        "stream_type": "rgb",
                        "frame_id": frame_id,
                        "frame_system_timestamp_us": timestamp - 500,
                        "global_timestamp_us": timestamp,
                        "rgb_recorded": 1,
                        "rgb_video_frame_index": index,
                    }
                )
        return root

    def test_accepts_finalized_explicit_frame_map(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.make_recording(Path(temporary) / "recording", ready_marker=True)
            result = sync_input_guard.validate_recording(root)
            self.assertEqual(result.rgb_frames, 3)
            self.assertEqual(result.first_global_timestamp_us, 1_000_000)
            self.assertIsNotNone(result.ready_marker)

    def test_rejects_active_recording(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.make_recording(Path(temporary) / "recording", closed=False)
            with self.assertRaisesRegex(sync_input_guard.ValidationError, "not finalized"):
                sync_input_guard.validate_recording(root)

    def test_rejects_row_number_instead_of_video_index(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.make_recording(
                Path(temporary) / "recording", indexes=(20, 21, 22)
            )
            with self.assertRaisesRegex(sync_input_guard.ValidationError, "expected 0, got 20"):
                sync_input_guard.validate_recording(root)

    def test_rejects_non_monotonic_global_time(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.make_recording(
                Path(temporary) / "recording",
                timestamps=(1_000_000, 999_000, 1_066_666),
            )
            with self.assertRaisesRegex(sync_input_guard.ValidationError, "non-monotonic"):
                sync_input_guard.validate_recording(root)

    def test_requires_declared_ready_marker(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.make_recording(Path(temporary) / "recording")
            meta_path = root / "test_meta.json"
            meta = json.loads(meta_path.read_text(encoding="utf-8"))
            meta["recording_ready_file"] = "missing_ready.json"
            meta_path.write_text(json.dumps(meta), encoding="utf-8")
            with self.assertRaisesRegex(sync_input_guard.ValidationError, "ready marker"):
                sync_input_guard.validate_recording(root)


if __name__ == "__main__":
    unittest.main()
