#!/usr/bin/env python3
import argparse
import csv
import http.client
import json
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import tempfile
import threading
import time


MEDIA_MAGIC = 0x33565747
UDP_MAGIC = 0x31505547
HEADER_VERSION = 1
HEADER_SIZE = 94
STREAM_DEPTH = 2
STREAM_RGB = 1
PIXEL_ENCODED_VIDEO = 1
PIXEL_DEPTH_U16 = 2


def free_port(sock_type: int) -> int:
    with socket.socket(socket.AF_INET, sock_type) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def request(port: int, method: str, path: str, headers=None, timeout=5):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=timeout)
    connection.request(method, path, headers=headers or {})
    response = connection.getresponse()
    body = response.read()
    result = response.status, dict(response.getheaders()), body
    connection.close()
    return result


def wait_http(port: int, path: str = "/api/status", timeout: float = 10.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            if request(port, "GET", path, timeout=1)[0] in {200, 401}:
                return
        except OSError:
            pass
        time.sleep(0.05)
    raise RuntimeError(f"HTTP service on port {port} did not start")


def status_message(port: int, payload: dict) -> None:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        data = (json.dumps(payload, separators=(",", ":")) + "\n").encode()
        sock.sendto(data, ("127.0.0.1", port))


def depth_packet(sender_id: str, camera_id: str, frame_id: int, width: int, height: int, timestamp_us: int, pair_id: int = 0) -> bytes:
    sender = sender_id.encode("ascii")
    camera = camera_id.encode("ascii")
    codec = b"none"
    payload = struct.pack("<H", 1000 + frame_id % 100) * (width * height)
    header = struct.pack(
        "<IHHBBIHHHQQQQIIHQQ16s",
        MEDIA_MAGIC,
        HEADER_VERSION,
        HEADER_SIZE,
        STREAM_DEPTH,
        pair_id,
        1 << 3,
        len(sender),
        len(camera),
        len(codec),
        frame_id,
        frame_id * 33333,
        timestamp_us,
        0,
        width,
        height,
        PIXEL_DEPTH_U16,
        len(payload),
        len(payload),
        b"\0" * 16,
    )
    return header + sender + camera + codec + payload


def rgb_packet(sender_id: str, camera_id: str, frame_id: int, width: int, height: int, timestamp_us: int, payload: bytes) -> bytes:
    sender = sender_id.encode("ascii")
    camera = camera_id.encode("ascii")
    codec = b"h264"
    header = struct.pack(
        "<IHHBBIHHHQQQQIIHQQ16s",
        MEDIA_MAGIC,
        HEADER_VERSION,
        HEADER_SIZE,
        STREAM_RGB,
        0,
        (1 << 0) | (1 << 3),
        len(sender),
        len(camera),
        len(codec),
        frame_id,
        frame_id * 33333,
        timestamp_us,
        0,
        width,
        height,
        PIXEL_ENCODED_VIDEO,
        len(payload),
        len(payload),
        b"\0" * 16,
    )
    return header + sender + camera + codec + payload


def generate_h264_fixture(frame_count: int = 60) -> bytes:
    result = subprocess.run(
        [
            "ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", "testsrc2=size=64x48:rate=30",
            "-frames:v", str(frame_count), "-c:v", "libx264", "-preset", "ultrafast", "-tune", "zerolatency",
            "-x264-params", "repeat-headers=1:keyint=30", "-f", "h264", "pipe:1",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=20,
        check=False,
    )
    assert result.returncode == 0 and result.stdout, result.stderr.decode(errors="replace")
    return result.stdout


def create_ffmpeg_test_wrappers(root: Path) -> Path:
    wrapper_dir = root / "ffmpeg-bin"
    wrapper_dir.mkdir()
    real_ffmpeg = shutil.which("ffmpeg")
    real_ffprobe = shutil.which("ffprobe")
    assert real_ffmpeg and real_ffprobe
    ffmpeg_wrapper = wrapper_dir / "ffmpeg"
    ffmpeg_wrapper.write_text(
        "#!/usr/bin/env python3\n"
        "import os, sys, time\n"
        "if any('rgb_seekable.tmp.mp4' in arg for arg in sys.argv[1:]):\n"
        "    time.sleep(float(os.environ.get('GWV3_TEST_REMUX_DELAY_SEC', '0')))\n"
        f"os.execv({real_ffmpeg!r}, [{real_ffmpeg!r}, *sys.argv[1:]])\n",
        encoding="ascii",
    )
    ffprobe_wrapper = wrapper_dir / "ffprobe"
    ffprobe_wrapper.write_text(
        "#!/usr/bin/env python3\n"
        "import os, sys\n"
        f"os.execv({real_ffprobe!r}, [{real_ffprobe!r}, *sys.argv[1:]])\n",
        encoding="ascii",
    )
    ffmpeg_wrapper.chmod(0o755)
    ffprobe_wrapper.chmod(0o755)
    return ffmpeg_wrapper


def mp4_top_level_atoms(path: Path) -> set[bytes]:
    atoms = set()
    file_size = path.stat().st_size
    offset = 0
    with path.open("rb") as stream:
        while offset + 8 <= file_size:
            stream.seek(offset)
            header = stream.read(8)
            assert len(header) == 8, "truncated MP4 atom header"
            atom_size = int.from_bytes(header[:4], "big")
            atom_type = header[4:8]
            header_size = 8
            if atom_size == 1:
                extended_size = stream.read(8)
                assert len(extended_size) == 8, "truncated extended MP4 atom header"
                atom_size = int.from_bytes(extended_size, "big")
                header_size = 16
            elif atom_size == 0:
                atom_size = file_size - offset
            assert header_size <= atom_size <= file_size - offset, "invalid MP4 atom size"
            atoms.add(atom_type)
            offset += atom_size
    assert offset == file_size, "trailing bytes after final MP4 atom"
    return atoms


def compressed_depth_bomb_packet(sender_id: str, camera_id: str, timestamp_us: int) -> bytes:
    sender = sender_id.encode("ascii")
    camera = camera_id.encode("ascii")
    codec = b"pq8zlib"
    width, height = 64, 48
    payload = struct.pack("<IHHIHHI", 0x5A385150, 1, 4, width * height, 257, 0, 0)
    header = struct.pack(
        "<IHHBBIHHHQQQQIIHQQ16s",
        MEDIA_MAGIC, HEADER_VERSION, HEADER_SIZE, STREAM_DEPTH, 0, 1 << 3,
        len(sender), len(camera), len(codec), 1, 33333, timestamp_us, 0,
        width, height, PIXEL_DEPTH_U16, len(payload), width * height * 2, b"\0" * 16,
    )
    return header + sender + camera + codec + payload


def overlapping_plz4_packet(sender_id: str, camera_id: str, timestamp_us: int) -> bytes:
    sender = sender_id.encode("ascii")
    camera = camera_id.encode("ascii")
    codec = b"plz4"
    width, height = 64, 48
    raw_size = width * height * 2
    payload = struct.pack("<IHHII", 0x345A4C50, 1, 2, raw_size, 0)
    payload += struct.pack("<III", 0, raw_size // 2, 1)
    payload += struct.pack("<III", 0, raw_size // 2, 1)
    payload += b"xx"
    header = struct.pack(
        "<IHHBBIHHHQQQQIIHQQ16s",
        MEDIA_MAGIC, HEADER_VERSION, HEADER_SIZE, STREAM_DEPTH, 0, 1 << 3,
        len(sender), len(camera), len(codec), 2, 66666, timestamp_us, 0,
        width, height, PIXEL_DEPTH_U16, len(payload), raw_size, b"\0" * 16,
    )
    return header + sender + camera + codec + payload


def exercise_clock_sync(clock_port: int, status_port: int, admin_port: int) -> None:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.settimeout(2)
        t1 = int(time.time() * 1_000_000)
        message = {
            "protocol_version": "3.0",
            "message_type": "clock_sync_probe",
            "sender_id": "test-sender",
            "sequence": 1,
            "t1_sender_send_us": t1,
        }
        probe.sendto((json.dumps(message) + "\n").encode(), ("127.0.0.1", clock_port))
        response = json.loads(probe.recv(4096))
        t4 = int(time.time() * 1_000_000)
    offset = ((int(response["t2_receiver_recv_us"]) - t1) + (int(response["t3_receiver_send_us"]) - t4)) // 2
    delay = (t4 - t1) - (int(response["t3_receiver_send_us"]) - int(response["t2_receiver_recv_us"]))
    status_message(
        status_port,
        {
            "protocol_version": "3.0",
            "message_type": "clock_sync_report",
            "sender_id": "test-sender",
            "clock_sync_valid": True,
            "clock_offset_us": offset,
            "clock_delay_us": max(0, delay),
            "clock_drift_ppm": 0.5,
            "clock_last_sync_us": t4,
        },
    )
    deadline = time.monotonic() + 2
    while time.monotonic() < deadline:
        current = json.loads(request(admin_port, "GET", "/api/status")[2])
        models = [model for model in current.get("clock_sync", []) if model.get("sender_id") == "test-sender"]
        if models and models[0].get("clock_sync_valid"):
            return
        time.sleep(0.05)
    raise AssertionError("clock sync model was not accepted")


def send_incomplete_udp_assemblies(port: int, count: int = 300) -> None:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        for sequence in range(1, count + 1):
            header = struct.pack("<IHHIHHIIH6x", UDP_MAGIC, 1, 32, sequence, 0, 2, 64, 0, 1)
            sock.sendto(header + b"x", ("127.0.0.1", port))


def exercise_runtime_state_save_isolation(ports: dict, state_path: Path) -> None:
    fifo_path = Path(f"{state_path}.tmp")
    if fifo_path.exists():
        fifo_path.unlink()
    os.mkfifo(fifo_path)
    sender_id = "slow-state-save-test"
    camera_id = "cam01"
    reader_errors = []
    persisted_payloads = []

    def drain_fifo() -> None:
        try:
            with fifo_path.open("rb", buffering=0) as handle:
                persisted_payloads.append(handle.read())
        except Exception as exc:  # pragma: no cover - only reports cleanup failures
            reader_errors.append(exc)

    reader = None
    try:
        status_message(
            ports["status"],
            {
                "protocol_version": "3.0",
                "message_type": "camera_announce",
                "sender_id": sender_id,
                "camera_id": camera_id,
                "rgb_profile": {"width": 64, "height": 48, "fps": 30},
                "depth_profile": {"width": 64, "height": 48, "fps": 30, "depth_scale": 1},
            },
        )
        time.sleep(0.1)
        timestamp = int(time.time() * 1_000_000)
        with socket.create_connection(("127.0.0.1", ports["media"]), timeout=3) as media:
            media.sendall(depth_packet(sender_id, camera_id, 1, 64, 48, timestamp))

        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            current = json.loads(request(ports["admin"], "GET", "/api/status", timeout=1)[2])
            cameras = [camera for camera in current.get("cameras", []) if camera.get("sender_id") == sender_id]
            if cameras and int(cameras[0].get("depth_packets", 0)) >= 1:
                break
            time.sleep(0.02)
        else:
            raise AssertionError("runtime state persistence blocked media ingress")
    finally:
        reader = threading.Thread(target=drain_fifo, daemon=True)
        reader.start()
        reader.join(timeout=5)
        assert not reader.is_alive(), "runtime state FIFO writer did not drain"
        assert not reader_errors, reader_errors
        rename_deadline = time.monotonic() + 2
        while fifo_path.exists() and time.monotonic() < rename_deadline:
            time.sleep(0.01)
        assert not fifo_path.exists(), "runtime state FIFO was not atomically published"
        state_path.unlink(missing_ok=True)
        state_path.write_bytes(persisted_payloads[0] if persisted_payloads else b"{}\n")


def exercise_media_session_fencing(ports: dict) -> None:
    sender_id = "session-fence-test"
    camera_id = "cam01"
    status_message(
        ports["status"],
        {
            "protocol_version": "3.0",
            "message_type": "camera_announce",
            "sender_id": sender_id,
            "camera_id": camera_id,
            "rgb_profile": {"width": 64, "height": 48, "fps": 30},
            "depth_profile": {"width": 64, "height": 48, "fps": 30, "depth_scale": 1},
        },
    )
    baseline = json.loads(request(ports["admin"], "GET", "/api/status")[2])
    superseded_before = int(baseline.get("media_ingress_superseded_sessions", 0))
    timestamp = int(time.time() * 1_000_000)
    old_media = socket.create_connection(("127.0.0.1", ports["media"]), timeout=3)
    new_media = None
    try:
        old_media.sendall(depth_packet(sender_id, camera_id, 1, 64, 48, timestamp))
        time.sleep(0.05)
        new_media = socket.create_connection(("127.0.0.1", ports["media"]), timeout=3)
        new_media.sendall(depth_packet(sender_id, camera_id, 2, 64, 48, timestamp + 33333))

        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            current = json.loads(request(ports["admin"], "GET", "/api/status")[2])
            if int(current.get("media_ingress_superseded_sessions", 0)) > superseded_before:
                break
            time.sleep(0.02)
        else:
            raise AssertionError("new media session did not supersede the old route owner")

        old_media.settimeout(1)
        try:
            assert old_media.recv(1) == b"", "superseded media connection remained readable"
        except (ConnectionResetError, OSError):
            pass
        new_media.sendall(depth_packet(sender_id, camera_id, 3, 64, 48, timestamp + 66666))
    finally:
        old_media.close()
        if new_media is not None:
            new_media.close()


def direct_ffmpeg_children(parent_pid: int) -> list[int]:
    children_path = Path(f"/proc/{parent_pid}/task/{parent_pid}/children")
    if not children_path.exists():
        return []
    result = []
    for value in children_path.read_text(encoding="ascii").split():
        pid = int(value)
        try:
            command = Path(f"/proc/{pid}/comm").read_text(encoding="ascii").strip()
        except FileNotFoundError:
            continue
        if command == "ffmpeg":
            result.append(pid)
    return result


def exercise_preview_decoder_cleanup(ports: dict, receiver_pid: int) -> None:
    sender_id = "preview-cleanup-test"
    camera_ids = ["cam01", "cam02"]
    for camera_id in camera_ids:
        status_message(
            ports["status"],
            {
                "protocol_version": "3.0",
                "message_type": "camera_announce",
                "sender_id": sender_id,
                "camera_id": camera_id,
                "rgb_profile": {"width": 64, "height": 48, "fps": 30},
                "depth_profile": {"enabled": False},
            },
        )
    time.sleep(0.1)
    h264_fixture = generate_h264_fixture(30)
    timestamp = int(time.time() * 1_000_000)
    previous_camera = None
    with socket.create_connection(("127.0.0.1", ports["media"]), timeout=3) as media:
        for frame_id in range(24):
            camera_id = camera_ids[frame_id % len(camera_ids)]
            target_path = f"/api/preview/main-target?sender_id={sender_id}&camera_id={camera_id}"
            assert request(ports["admin"], "POST", target_path)[0] == 200
            if previous_camera is not None:
                media.sendall(
                    rgb_packet(sender_id, previous_camera, frame_id * 2, 64, 48, timestamp + frame_id * 33333, h264_fixture)
                )
            media.sendall(
                rgb_packet(sender_id, camera_id, frame_id * 2 + 1, 64, 48, timestamp + frame_id * 33333 + 1, h264_fixture)
            )
            preview_path = f"/api/preview/rgb-main?sender_id={sender_id}&camera_id={camera_id}"
            deadline = time.monotonic() + 2
            feed_index = 0
            while time.monotonic() < deadline:
                status, _, body = request(ports["admin"], "GET", preview_path)
                if status == 200 and body.startswith(b"\xff\xd8") and body.endswith(b"\xff\xd9"):
                    break
                feed_index += 1
                media.sendall(
                    rgb_packet(
                        sender_id,
                        camera_id,
                        10_000 + frame_id * 100 + feed_index,
                        64,
                        48,
                        timestamp + frame_id * 33333 + feed_index,
                        h264_fixture,
                    )
                )
                time.sleep(0.02)
            else:
                raise AssertionError(f"RGB main preview did not refresh for {camera_id}")
            previous_camera = camera_id

        other_camera = camera_ids[(camera_ids.index(previous_camera) + 1) % len(camera_ids)]
        target_path = f"/api/preview/main-target?sender_id={sender_id}&camera_id={other_camera}"
        assert request(ports["admin"], "POST", target_path)[0] == 200
        media.sendall(rgb_packet(sender_id, previous_camera, 1000, 64, 48, timestamp + 1_000_000, h264_fixture))

    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if len(direct_ffmpeg_children(receiver_pid)) <= 1:
            return
        time.sleep(0.05)
    raise AssertionError(f"preview FFmpeg children leaked: {direct_ffmpeg_children(receiver_pid)}")


def exercise_async_segment_rotation(ports: dict, nas_root: Path) -> None:
    sender_id = "rotation-test"
    camera_ids = ["cam01", "cam02", "cam03"]
    for camera_id in camera_ids:
        status_message(
            ports["status"],
            {
                "protocol_version": "3.0",
                "message_type": "camera_announce",
                "sender_id": sender_id,
                "camera_id": camera_id,
                "rgb_profile": {"width": 64, "height": 48, "fps": 30},
                "depth_profile": {"width": 64, "height": 48, "fps": 30, "depth_scale": 1},
            },
        )
    time.sleep(0.1)
    assert request(ports["admin"], "POST", "/api/record/start-all")[0] == 200

    h264_frame = generate_h264_fixture(1)
    first_timestamp = int(time.time() * 1_000_000)
    observed_directories = {camera_id: set() for camera_id in camera_ids}
    max_finalize_outstanding = 0
    max_finalize_active = 0
    max_record_queue_peak = 0
    frame_id = 0
    deadline = time.monotonic() + 3.4
    with socket.create_connection(("127.0.0.1", ports["media"]), timeout=3) as media:
        while time.monotonic() < deadline:
            timestamp = first_timestamp + frame_id * 33333
            for camera_id in camera_ids:
                media.sendall(rgb_packet(sender_id, camera_id, frame_id, 64, 48, timestamp, h264_frame))
                media.sendall(depth_packet(sender_id, camera_id, frame_id, 64, 48, timestamp, pair_id=frame_id & 0xFF))
            frame_id += 1
            current = json.loads(request(ports["admin"], "GET", "/api/status")[2])
            max_finalize_outstanding = max(
                max_finalize_outstanding, int(current.get("record_finalize_outstanding_segments", 0))
            )
            max_finalize_active = max(max_finalize_active, int(current.get("record_finalize_active_segments", 0)))
            for camera in current.get("cameras", []):
                if camera.get("sender_id") != sender_id:
                    continue
                camera_id = camera.get("camera_id")
                if camera.get("segment_dir"):
                    observed_directories[camera_id].add(camera["segment_dir"])
                max_record_queue_peak = max(max_record_queue_peak, int(camera.get("record_queue_peak_bytes", 0)))
            time.sleep(max(0.0, first_timestamp / 1_000_000 + frame_id / 30 - time.time()))

    assert all(len(directories) >= 2 for directories in observed_directories.values()), observed_directories
    assert max_finalize_outstanding >= 2, "slow finalization did not overlap active recording"
    assert max_finalize_active == 1, "more than one heavyweight segment finalizer ran concurrently"
    assert max_record_queue_peak < 4 * 1024 * 1024, f"record queue grew during background finalization: {max_record_queue_peak}"

    assert request(ports["admin"], "POST", "/api/record/stop-all", timeout=10)[0] == 200
    finalization_deadline = time.monotonic() + 30
    while time.monotonic() < finalization_deadline:
        current = json.loads(request(ports["admin"], "GET", "/api/status", timeout=2)[2])
        rotation_cameras = [camera for camera in current.get("cameras", []) if camera.get("sender_id") == sender_id]
        if (
            int(current.get("record_finalize_outstanding_segments", 0)) == 0
            and all(not camera.get("segment_active") and not camera.get("segment_finalizing") for camera in rotation_cameras)
        ):
            break
        time.sleep(0.1)
    else:
        raise AssertionError("background segment finalization did not drain")

    for camera_id in camera_ids:
        camera_root = nas_root / f"{sender_id}_{camera_id}"
        ready_files = list(camera_root.rglob("recording_ready.json"))
        assert len(ready_files) >= 2, f"finalized rotated segments missing for {camera_id}"


def assert_recording_output(nas_root: Path, minimum_rgb_duration: float = 1.0) -> None:
    rgb_files = list(nas_root.rglob("rgb.mp4"))
    assert rgb_files, "rgb.mp4 was not finalized"
    for rgb_file in rgb_files:
        probe = subprocess.run(
            ["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries", "stream=codec_name:format=duration",
             "-of", "json", str(rgb_file)],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=10,
            check=False,
        )
        assert probe.returncode == 0, probe.stderr
        metadata = json.loads(probe.stdout)
        assert metadata.get("streams", [{}])[0].get("codec_name") == "h264"
        assert float(metadata.get("format", {}).get("duration", 0)) > minimum_rgb_duration
        atoms = mp4_top_level_atoms(rgb_file)
        assert b"moov" in atoms and b"moof" not in atoms, "finalized RGB must be a conventional MP4"
        seek = subprocess.run(
            ["ffmpeg", "-v", "error", "-ss", "1", "-i", str(rgb_file), "-frames:v", "1", "-f", "null", "-"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=10,
            check=False,
        )
        assert seek.returncode == 0, "finalized rgb.mp4 is not seekable"
        assert not (rgb_file.parent / "rgb_debug.h264").exists(), "validated RGB recovery sidecar was not removed"
        assert not list(rgb_file.parent.glob("*rgb_seekable.tmp.mp4")), "RGB compatibility temporary file was not removed"

    depth_files = list(nas_root.rglob("depth.mkv"))
    assert depth_files, "depth.mkv was not finalized"
    for depth_file in depth_files:
        probe = subprocess.run(
            ["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries", "stream=codec_name", "-of", "csv=p=0", str(depth_file)],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=10,
            check=False,
        )
        assert probe.returncode == 0 and "ffv1" in probe.stdout, probe.stderr
        assert not list(depth_file.parent.glob("depth_part_*.mkv")), "finalized depth parts were not removed"

    frames_files = list(nas_root.rglob("frames.csv"))
    assert frames_files, "frames.csv missing"
    header = frames_files[0].read_text(encoding="utf-8").splitlines()[0].split(",")
    for field in (
        "global_timestamp_us",
        "pair_delta_us",
        "pair_delta_source",
        "pair_id_valid",
        "rgb_recorded",
        "rgb_video_frame_index",
    ):
        assert field in header, f"missing CSV field {field}"
    assert not list(nas_root.rglob("*frames.csv.inprogress")), "live frames.csv staging file was not removed"
    assert not list(nas_root.rglob("*frames.csv.finalizing")), "finalizing frames.csv was not published"
    ready_files = list(nas_root.rglob("*recording_ready.json"))
    assert ready_files, "recording ready marker missing"
    for ready_file in ready_files:
        assert json.loads(ready_file.read_text(encoding="utf-8")).get("ready") is True

    for frames_file in nas_root.rglob("*frames.csv"):
        last_global_timestamp = None
        with frames_file.open("r", newline="", encoding="utf-8-sig") as handle:
            for row in csv.DictReader(handle):
                if row.get("stream_type") != "rgb" or row.get("rgb_recorded") != "1":
                    continue
                global_timestamp = int(row["global_timestamp_us"])
                if last_global_timestamp is not None:
                    assert global_timestamp > last_global_timestamp, (
                        f"non-monotonic recorded RGB timestamp in {frames_file}: "
                        f"{global_timestamp} <= {last_global_timestamp}"
                    )
                last_global_timestamp = global_timestamp


def run(args) -> None:
    with tempfile.TemporaryDirectory(prefix="gwv3_receiver_test_") as temporary_text:
        temporary = Path(temporary_text)
        ffmpeg_wrapper = create_ffmpeg_test_wrappers(temporary) if shutil.which("ffmpeg") and shutil.which("ffprobe") else None
        ports = {
            "status": free_port(socket.SOCK_DGRAM),
            "media": free_port(socket.SOCK_STREAM),
            "media_udp": free_port(socket.SOCK_DGRAM),
            "clock": free_port(socket.SOCK_DGRAM),
            "admin": free_port(socket.SOCK_STREAM),
            "web": free_port(socket.SOCK_STREAM),
        }
        config = {
            "status_bind_ip": "127.0.0.1",
            "status_port": ports["status"],
            "media_bind_ip": "127.0.0.1",
            "media_port": ports["media"],
            "preview_enabled": True,
            "media_udp_enabled": True,
            "media_udp_bind_ip": "127.0.0.1",
            "media_udp_port": ports["media_udp"],
            "preview_udp_enabled": False,
            "clock_sync": {"enabled": True, "bind_ip": "127.0.0.1", "port": ports["clock"], "model_timeout_ms": 10000},
            "admin_bind_ip": "127.0.0.1",
            "admin_port": ports["admin"],
            "nas_root": str(temporary / "nas"),
            "log_directory": str(temporary / "logs"),
            "state_path": str(temporary / "state.json"),
            "ffmpeg_path": str(ffmpeg_wrapper) if ffmpeg_wrapper else "ffmpeg",
            "segment_seconds": 1,
            "depth_fps": 30,
            "max_payload_mb": 8,
            "record_queue_max_mb": 16,
            "record_finalize_max_pending_segments": 4,
        }
        config_path = temporary / "receiver.json"
        config_path.write_text(json.dumps(config), encoding="utf-8")
        receiver_log = (temporary / "receiver.log").open("wb")
        receiver_environment = os.environ.copy()
        receiver_environment["GWV3_TEST_REMUX_DELAY_SEC"] = "0.75"
        receiver = subprocess.Popen(
            [args.receiver, "--config", str(config_path)],
            env=receiver_environment,
            stdout=receiver_log,
            stderr=subprocess.STDOUT,
        )
        web = None
        idle_client = None
        try:
            wait_http(ports["admin"])

            exercise_runtime_state_save_isolation(ports, temporary / "state.json")

            duplicate_log = (temporary / "duplicate_receiver.log").open("wb")
            duplicate = subprocess.Popen(
                [args.receiver, "--config", str(config_path)],
                env=receiver_environment,
                stdout=duplicate_log,
                stderr=subprocess.STDOUT,
            )
            try:
                duplicate.wait(timeout=8)
                assert duplicate.returncode != 0, "a second receiver unexpectedly accepted the same listener ports"
            finally:
                if duplicate.poll() is None:
                    duplicate.kill()
                    duplicate.wait(timeout=3)
                duplicate_log.close()

            status_message(
                ports["status"],
                {"protocol_version": "3.0", "message_type": "camera_announce", "sender_id": "../escape", "camera_id": "cam01"},
            )
            status_message(
                ports["status"],
                {
                    "protocol_version": "3.0",
                    "message_type": "camera_announce",
                    "sender_id": "test-sender",
                    "camera_id": "cam01",
                    "rgb_profile": {"width": 10**100, "height": 480},
                    "depth_profile": {"width": 64, "height": 48, "fps": 30, "depth_scale": 1},
                },
            )
            time.sleep(0.1)
            admin_status = json.loads(request(ports["admin"], "GET", "/api/status")[2])
            source_hash = str(admin_status.get("build_source_hash", ""))
            assert len(source_hash) == 16 and all(ch in "0123456789abcdef" for ch in source_hash)
            assert receiver.poll() is None, "receiver crashed on malformed status JSON"
            assert all(camera.get("sender_id") != "../escape" for camera in admin_status.get("cameras", []))
            exercise_clock_sync(ports["clock"], ports["status"], ports["admin"])

            send_incomplete_udp_assemblies(ports["media_udp"])
            time.sleep(0.1)
            udp_status = json.loads(request(ports["admin"], "GET", "/api/status")[2])
            assert udp_status["media_udp_stats"]["active_assemblies"] <= 256
            exercise_media_session_fencing(ports)
            if shutil.which("ffmpeg"):
                exercise_preview_decoder_cleanup(ports, receiver.pid)

            assert request(ports["admin"], "POST", "/api/record/start-all")[0] == 200
            stop_one = json.loads(
                request(
                    ports["admin"],
                    "POST",
                    "/api/record/stop?sender_id=test-sender&camera_id=cam01",
                )[2]
            )
            assert stop_one.get("ok") is False, "single-camera stop must not race an active start-all recording"
            with socket.create_connection(("127.0.0.1", ports["media"]), timeout=3) as media:
                media.sendall(compressed_depth_bomb_packet("test-sender", "cam01", int(time.time() * 1_000_000)))
                media.sendall(overlapping_plz4_packet("test-sender", "cam01", int(time.time() * 1_000_000)))
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                current = json.loads(request(ports["admin"], "GET", "/api/status")[2])
                cameras = [camera for camera in current.get("cameras", []) if camera.get("sender_id") == "test-sender"]
                if cameras and int(cameras[0].get("record_write_errors", 0)) >= 2:
                    break
                time.sleep(0.05)
            else:
                raise AssertionError("malicious depth chunk layouts were not rejected")
            assert request(ports["admin"], "POST", "/api/record/stop-all", timeout=10)[0] == 200
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                current = json.loads(request(ports["admin"], "GET", "/api/status")[2])
                if all(not camera.get("segment_finalizing") for camera in current.get("cameras", [])):
                    break
                time.sleep(0.05)

            if shutil.which("ffmpeg") and shutil.which("ffprobe"):
                assert request(ports["admin"], "POST", "/api/record/start-all")[0] == 200
                start_timestamp = int(time.time() * 1_000_000)
                h264_fixture = generate_h264_fixture()
                with socket.create_connection(("127.0.0.1", ports["media"]), timeout=3) as media:
                    media.sendall(rgb_packet("test-sender", "cam01", 1, 64, 48, start_timestamp, h264_fixture))
                    for frame_id in range(70):
                        media.sendall(depth_packet("test-sender", "cam01", frame_id, 64, 48, start_timestamp + frame_id * 33333))
                deadline = time.monotonic() + 3
                while time.monotonic() < deadline:
                    staging_files = list((temporary / "nas").rglob("*frames.csv.inprogress"))
                    if staging_files:
                        break
                    time.sleep(0.05)
                else:
                    raise AssertionError("live frames.csv staging file was not created")
                for staging_file in staging_files:
                    published_file = Path(str(staging_file).removesuffix(".inprogress"))
                    assert not published_file.exists(), "frames.csv became visible before recording finalization"
                assert request(ports["admin"], "POST", "/api/record/stop-all", timeout=20)[0] == 200
                deadline = time.monotonic() + 20
                while time.monotonic() < deadline:
                    current = json.loads(request(ports["admin"], "GET", "/api/status")[2])
                    cameras = current.get("cameras", [])
                    if all(not camera.get("segment_active") and not camera.get("segment_finalizing") for camera in cameras):
                        break
                    time.sleep(0.1)
                else:
                    raise AssertionError("recording did not finalize")
                assert_recording_output(temporary / "nas")
                exercise_async_segment_rotation(ports, temporary / "nas")
                assert_recording_output(temporary / "nas", minimum_rgb_duration=0.0)
            else:
                print("recording finalization check skipped: ffmpeg/ffprobe unavailable")

            try:
                __import__("uvicorn")
            except ImportError:
                pass
            else:
                environment = os.environ.copy()
                environment.update(
                    {
                        "GWV3_RECEIVER_ADMIN": f"http://127.0.0.1:{ports['admin']}",
                    }
                )
                web = subprocess.Popen(
                    [os.sys.executable, "-m", "uvicorn", "server:app", "--app-dir", str(Path(args.source_root) / "09_web_monitor"),
                     "--host", "127.0.0.1", "--port", str(ports["web"]), "--no-access-log"],
                    env=environment,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )
                wait_http(ports["web"], "/")
                assert request(ports["web"], "GET", "/api/status")[0] == 200

            for index in range(40):
                status_message(
                    ports["status"],
                    {
                        "protocol_version": "3.0",
                        "message_type": "camera_announce",
                        "sender_id": "capacity-test",
                        "camera_id": f"cam{index:02d}",
                    },
                )
            time.sleep(0.2)
            assert receiver.poll() is None, "receiver crashed when the tracked-camera limit was exceeded"
            final_status = json.loads(request(ports["admin"], "GET", "/api/status")[2])
            assert int(final_status.get("record_queue_total_bytes", -1)) == 0

            idle_client = socket.create_connection(("127.0.0.1", ports["media"]), timeout=3)
            receiver.terminate()
            receiver.wait(timeout=8)
            assert receiver.returncode == 0, f"receiver shutdown failed: {receiver.returncode}"
        finally:
            if idle_client:
                idle_client.close()
            if web and web.poll() is None:
                web.terminate()
                web.wait(timeout=5)
            if receiver.poll() is None:
                receiver.kill()
                receiver.wait(timeout=5)
            receiver_log.close()
            if receiver.returncode not in {0, -15}:
                print((temporary / "receiver.log").read_text(encoding="utf-8", errors="replace"))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--receiver", required=True)
    parser.add_argument("--source-root", required=True)
    run(parser.parse_args())
    print("receiver hardening integration test passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
