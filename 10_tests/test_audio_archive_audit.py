#!/usr/bin/env python3
import json
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "05_tools" / "audit_silent_audio_archive.py"


def main():
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp) / "audio"
        silent = root / "sender-a" / "2026-08-24" / "120000"
        valid = root / "sender-a" / "2026-08-24" / "121500"
        silent.mkdir(parents=True)
        valid.mkdir(parents=True)
        for directory, received in ((silent, 0), (valid, 100)):
            (directory / "audio.opus").write_bytes(b"retained-evidence")
            (directory / "audio_timing.csv").write_text(
                f"received_packets\n{received}\n", encoding="utf-8"
            )
            (directory / "audio_meta.json").write_text(
                json.dumps({"schema_version": 1, "sender_id": "sender-a", "received_packets": received}),
                encoding="utf-8",
            )
            (directory / "audio_ready.json").write_text("{}\n", encoding="utf-8")

        manifest = root / "audit.json"
        subprocess.run(
            ["python3", str(SCRIPT), str(root), "--apply", "--manifest", str(manifest)],
            check=True,
            stdout=subprocess.PIPE,
        )
        silent_meta = json.loads((silent / "audio_meta.json").read_text(encoding="utf-8"))
        valid_meta = json.loads((valid / "audio_meta.json").read_text(encoding="utf-8"))
        assert silent_meta["quality_status"] == "no_input"
        assert silent_meta["audio_valid"] is False
        assert (silent / "audio.opus").exists()
        assert "quality_status" not in valid_meta
        audit = json.loads(manifest.read_text(encoding="utf-8"))
        assert audit["scanned_segments"] == 2
        assert audit["no_input_segments"] == 1
    print("audio archive audit test passed")


if __name__ == "__main__":
    main()
