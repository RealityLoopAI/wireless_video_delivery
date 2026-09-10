#!/usr/bin/env python3
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

import test_receiver_hardening as h


def run(writer):
    with tempfile.TemporaryDirectory(prefix="gwv3_nut_") as tmp:
        root = Path(tmp)
        single = root / "single.h264"
        single.write_bytes(h.generate_h264_fixture(1))
        output = root / "output.nut"
        subprocess.run([writer, str(single), str(output), "timestamps"], check=True, timeout=10)
        probe = json.loads(subprocess.check_output(["ffprobe", "-v", "error", "-show_packets",
            "-show_entries", "packet=pts_time", "-of", "json", str(output)], timeout=10))
        expected = [.2, .233333, .266666, 1.266666, 1.299999]
        pts = [float(p["pts_time"]) for p in probe["packets"]]
        assert len(pts) == len(expected) and all(abs(a-b) < .000002 for a,b in zip(pts,expected)), pts
        multiple = root / "multiple.h264"
        multiple.write_bytes(h.generate_h264_fixture(4))
        subprocess.run([writer, str(multiple), str(root / "multiple.nut"), "multiple"], check=True, timeout=10)
        sequence = root / "sequence.nut"
        subprocess.run([writer, str(multiple), str(sequence), "sequence"], check=True, timeout=10)
        subprocess.run(["ffmpeg", "-v", "error", "-i", str(sequence), "-f", "null", "-"], check=True, timeout=10)
        bframes = root / "b.h264"
        subprocess.run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i", "testsrc2=size=64x48:rate=30",
            "-frames:v", "12", "-c:v", "libx264", "-bf", "2", "-x264-params", "b-adapt=0", "-f", "h264", str(bframes)],
            check=True, timeout=10)
        subprocess.run([writer, str(bframes), str(root / "b.nut"), "bframes"], check=True, timeout=10)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--writer", required=True)
    run(parser.parse_args().writer)
