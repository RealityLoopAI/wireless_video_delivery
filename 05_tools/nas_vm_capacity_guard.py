#!/usr/bin/env python3
"""Host-side last resort: request a graceful guest shutdown before backing ENOSPC."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import time
import xml.etree.ElementTree as ET


def guard(volume, uuid, minimum, runner=subprocess.run, usage=shutil.disk_usage):
    if not os.path.ismount(volume):
        raise RuntimeError("backing volume is not mounted")
    free = usage(volume).free
    result = {"checked_us": time.time_ns() // 1000, "volume": str(volume),
              "free_bytes": free, "reserve_bytes": minimum, "vm_uuid": uuid,
              "action": "none"}
    if free >= minimum:
        return result
    def virsh(*args):
        return runner(["virsh", "-c", "qemu:///system", *args],
                      check=True, capture_output=True, text=True, timeout=15).stdout
    xml = ET.fromstring(virsh("dumpxml", uuid))
    disks = [Path(s.attrib["file"]).resolve() for s in xml.findall("./devices/disk/source")
             if "file" in s.attrib]
    if not any(Path(volume).resolve() in p.parents for p in disks):
        raise RuntimeError("VM has no file-backed disk on the configured volume")
    state = virsh("domstate", uuid).strip()
    result["vm_state"] = state
    if state == "running":
        virsh("shutdown", uuid)
        result["action"] = "graceful_shutdown_requested"
    else:
        result["action"] = "manual_recovery_required"
    # Never destroy, forcibly reset, or automatically resume a guest.
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--volume", type=Path, required=True)
    parser.add_argument("--vm-uuid", required=True)
    parser.add_argument("--reserve-gib", type=int, default=50)
    parser.add_argument("--status", type=Path, required=True)
    args = parser.parse_args()
    if args.reserve_gib <= 0:
        parser.error("reserve must be positive")
    result = guard(args.volume, args.vm_uuid, args.reserve_gib * 2**30)
    args.status.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.status.with_suffix(".tmp")
    temporary.write_text(json.dumps(result) + "\n")
    os.replace(temporary, args.status)
    print(json.dumps(result), flush=True)


if __name__ == "__main__":
    main()
