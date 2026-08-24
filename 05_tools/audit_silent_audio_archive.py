#!/usr/bin/env python3
"""Mark historical all-silence audio segments without deleting evidence."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import time
from pathlib import Path
from typing import Any


def now_us() -> int:
    return time.time_ns() // 1000


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def atomic_json(path: Path, value: dict[str, Any]) -> None:
    temporary = path.with_name(path.name + ".tmp")
    with temporary.open("w", encoding="utf-8") as handle:
        json.dump(value, handle, ensure_ascii=False, indent=2, sort_keys=True)
        handle.write("\n")
        handle.flush()
        os.fsync(handle.fileno())
    os.replace(temporary, path)


def timing_received_packets(path: Path) -> int | None:
    if not path.exists():
        return None
    total = 0
    try:
        with path.open("r", encoding="utf-8-sig", newline="") as handle:
            for row in csv.DictReader(handle):
                total += int(row.get("received_packets") or 0)
    except (OSError, ValueError, csv.Error):
        return None
    return total


def inspect(meta_path: Path) -> tuple[bool, dict[str, Any], int | None]:
    try:
        meta = json.loads(meta_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return False, {}, None
    received = meta.get("received_packets")
    if received is None:
        received = timing_received_packets(meta_path.parent / "audio_timing.csv")
    try:
        received_value = int(received) if received is not None else None
    except (TypeError, ValueError):
        received_value = None
    return received_value == 0, meta, received_value


def update_ready(directory: Path, audited_us: int) -> None:
    ready_path = directory / "audio_ready.json"
    try:
        ready = json.loads(ready_path.read_text(encoding="utf-8")) if ready_path.exists() else {}
    except json.JSONDecodeError:
        ready = {}
    ready.update(
        {
            "schema_version": max(2, int(ready.get("schema_version", 1))),
            "ready": True,
            "audio_valid": False,
            "quality_status": "no_input",
            "quality_reason": "historical audit found zero received RTP packets",
            "historical_audited_receiver_us": audited_us,
            "historical_audio_file_retained": (directory / "audio.opus").exists(),
        }
    )
    files = {}
    for name in ("audio.opus", "audio_timing.csv", "audio_meta.json"):
        path = directory / name
        if path.exists() and path.is_file():
            files[name] = {"size": path.stat().st_size, "sha256": sha256_file(path)}
    ready["files"] = files
    atomic_json(ready_path, ready)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("--apply", action="store_true", help="write quality markers; default is dry-run")
    parser.add_argument("--manifest", type=Path)
    args = parser.parse_args()

    audited_us = now_us()
    records = []
    scanned = 0
    for meta_path in sorted(args.root.rglob("audio_meta.json")):
        scanned += 1
        no_input, meta, received = inspect(meta_path)
        if not no_input:
            continue
        record = {
            "directory": str(meta_path.parent),
            "sender_id": str(meta.get("sender_id", "")),
            "segment_id": str(meta.get("segment_id", "")),
            "received_packets": received,
            "audio_file_retained": (meta_path.parent / "audio.opus").exists(),
        }
        records.append(record)
        if not args.apply:
            continue
        meta.update(
            {
                "schema_version": max(2, int(meta.get("schema_version", 1))),
                "audio_valid": False,
                "quality_status": "no_input",
                "quality_reason": "historical audit found zero received RTP packets",
                "historical_audited_receiver_us": audited_us,
                "historical_audio_file_retained": record["audio_file_retained"],
            }
        )
        atomic_json(meta_path, meta)
        update_ready(meta_path.parent, audited_us)

    manifest = {
        "schema_version": 1,
        "root": str(args.root),
        "audit_receiver_us": audited_us,
        "applied": args.apply,
        "scanned_segments": scanned,
        "no_input_segments": len(records),
        "records": records,
    }
    manifest_path = args.manifest
    if manifest_path is None:
        manifest_path = args.root / "_audit" / f"no_input_audit_{audited_us}.json"
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    atomic_json(manifest_path, manifest)
    print(json.dumps({**manifest, "records": f"{len(records)} entries", "manifest": str(manifest_path)}, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
