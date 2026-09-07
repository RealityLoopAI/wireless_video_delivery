#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
import json
import tempfile
from pathlib import Path


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    path = root / "05_tools/chrony_receiver_watch.py"
    spec = importlib.util.spec_from_file_location("chrony_receiver_watch_test", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    assert module.valid_host("192.168.1.196")
    assert module.valid_host("gwv3-receiver.local")
    assert not module.valid_host("receiver; reboot")
    assert not module.valid_host("-invalid.local")
    with tempfile.TemporaryDirectory(prefix="gwv3-chrony-watch-test-") as temporary_text:
        state = Path(temporary_text) / "receiver_target.json"
        state.write_text(json.dumps({"receiver_host": "192.168.1.197"}), encoding="utf-8")
        assert module.discovered_host(state) == "192.168.1.197"
        state.write_text("not-json", encoding="utf-8")
        assert module.discovered_host(state) == ""
    print("Chrony receiver watch unit test passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
