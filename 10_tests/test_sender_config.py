#!/usr/bin/env python3
import argparse
import copy
import json
from pathlib import Path
import subprocess
import tempfile


def validate(sender: str, config: Path) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sender, "--config", str(config), "--validate-config"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=20,
        check=False,
    )


def expect_invalid(sender: str, directory: Path, name: str, config: dict) -> None:
    path = directory / f"{name}.json"
    path.write_text(json.dumps(config), encoding="utf-8")
    result = validate(sender, path)
    assert result.returncode != 0, f"invalid config {name} was accepted: {result.stdout}"


def run(args) -> None:
    root = Path(args.source_root)
    configs = sorted((root / "06_configs").glob("sender_*.json"))
    assert configs, "no sender configs found"
    for config in configs:
        result = validate(args.sender, config)
        assert result.returncode == 0, f"{config.name} failed validation:\n{result.stdout}"

    base = json.loads((root / "06_configs" / "sender_rk3588-01_one_camera.json").read_text(encoding="utf-8"))
    with tempfile.TemporaryDirectory(prefix="gwv3_sender_config_") as temporary_text:
        temporary = Path(temporary_text)

        invalid = copy.deepcopy(base)
        invalid["sender_id"] = "../escape"
        expect_invalid(args.sender, temporary, "unsafe_sender_id", invalid)

        invalid = copy.deepcopy(base)
        invalid["receiver"]["ip"] = "not-an-ip"
        expect_invalid(args.sender, temporary, "invalid_receiver_ip", invalid)

        invalid = copy.deepcopy(base)
        invalid["recording_buffer"] = {"enabled": True, "rgb_frames_per_slot": 10**9}
        expect_invalid(args.sender, temporary, "unbounded_queue", invalid)

        invalid = copy.deepcopy(base)
        invalid["cameras"][0]["rgb_profile"]["width"] = 10**9
        expect_invalid(args.sender, temporary, "oversized_profile", invalid)

        invalid = copy.deepcopy(base)
        invalid["cameras"][0]["device_model"] = "bad\nmodel"
        expect_invalid(args.sender, temporary, "invalid_device_model", invalid)

        trailing = temporary / "trailing.json"
        trailing.write_text(json.dumps(base) + "{}", encoding="utf-8")
        result = validate(args.sender, trailing)
        assert result.returncode != 0, "config with a trailing JSON document was accepted"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sender", required=True)
    parser.add_argument("--source-root", required=True)
    run(parser.parse_args())
    print("sender config validation test passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
