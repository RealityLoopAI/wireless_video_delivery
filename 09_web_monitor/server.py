import asyncio
import contextlib
import json
import os
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime
from pathlib import Path
from typing import Any

from fastapi import FastAPI, HTTPException, Query, Response as FastAPIResponse
from fastapi.responses import HTMLResponse, Response, StreamingResponse
from fastapi.staticfiles import StaticFiles


ADMIN_BASE = os.environ.get("GWV3_RECEIVER_ADMIN", "http://127.0.0.1:18080")
ADMIN_TIMEOUT_S = float(os.environ.get("GWV3_RECEIVER_ADMIN_TIMEOUT_S", "3"))
ADMIN_RECORD_STOP_TIMEOUT_S = float(os.environ.get("GWV3_RECEIVER_RECORD_STOP_TIMEOUT_S", "60"))
AUDIO_ADMIN_BASE = os.environ.get("GWV3_AUDIO_ARCHIVE_ADMIN", "http://127.0.0.1:18083")
AUDIO_ADMIN_TIMEOUT_S = float(os.environ.get("GWV3_AUDIO_ARCHIVE_TIMEOUT_S", "3"))
STATUS_CACHE_MAX_AGE_S = max(
    1.0, float(os.environ.get("GWV3_WEB_STATUS_CACHE_MAX_AGE_S", "15"))
)
MAX_PREVIEW_STREAM_CLIENTS = max(
    1, int(os.environ.get("GWV3_WEB_MAX_PREVIEW_STREAM_CLIENTS", os.environ.get("GWV3_WEB_MAX_STREAM_CLIENTS", "16")))
)
MAX_MAIN_STREAM_CLIENTS = max(1, int(os.environ.get("GWV3_WEB_MAX_MAIN_STREAM_CLIENTS", "2")))
PREVIEW_STREAM_SLOTS = threading.BoundedSemaphore(MAX_PREVIEW_STREAM_CLIENTS)
MAIN_STREAM_SLOTS = threading.BoundedSemaphore(MAX_MAIN_STREAM_CLIENTS)
STATUS_CACHE_LOCK = threading.Lock()
STATUS_CACHE: dict[str, Any] | None = None
STATUS_CACHE_MONOTONIC = 0.0
ROOT_DIR = Path(__file__).resolve().parent
STATIC_DIR = ROOT_DIR / "static"
INDEX_HTML = STATIC_DIR / "index.html"

app = FastAPI(title="Gemini Wireless Video v3 Monitor")
app.mount("/static", StaticFiles(directory=STATIC_DIR), name="static")


def _request(method: str, path: str, timeout_s: float = ADMIN_TIMEOUT_S) -> Any:
    url = ADMIN_BASE.rstrip("/") + path
    req = urllib.request.Request(url, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout_s) as resp:
            data = resp.read().decode("utf-8")
            return json.loads(data)
    except urllib.error.HTTPError as exc:
        exc.close()
        raise HTTPException(status_code=502, detail=f"receiver admin unavailable: {exc}") from exc
    except Exception as exc:
        raise HTTPException(status_code=502, detail=f"receiver admin unavailable: {exc}") from exc


def _audio_request(method: str, path: str) -> Any:
    url = AUDIO_ADMIN_BASE.rstrip("/") + path
    req = urllib.request.Request(url, method=method)
    try:
        with urllib.request.urlopen(req, timeout=AUDIO_ADMIN_TIMEOUT_S) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", errors="replace")
        raise HTTPException(status_code=exc.code, detail=detail or "audio archive request failed") from exc
    except Exception as exc:
        raise HTTPException(status_code=502, detail=f"audio archive unavailable: {exc}") from exc


@app.get("/", response_class=HTMLResponse)
def index() -> HTMLResponse:
    return HTMLResponse(
        content=INDEX_HTML.read_text(encoding="utf-8"),
        headers={"Cache-Control": "no-store"},
    )


@app.get("/api/status")
def status(response: FastAPIResponse) -> Any:
    global STATUS_CACHE, STATUS_CACHE_MONOTONIC
    response.headers["Cache-Control"] = "no-store"
    try:
        current = _request("GET", "/api/status")
    except HTTPException as error:
        with STATUS_CACHE_LOCK:
            cache_age = time.monotonic() - STATUS_CACHE_MONOTONIC
            if STATUS_CACHE is None or cache_age > STATUS_CACHE_MAX_AGE_S:
                raise
            cached = dict(STATUS_CACHE)
        cached["receiver_admin_stale"] = True
        cached["receiver_admin_stale_age_ms"] = round(cache_age * 1000)
        cached["receiver_admin_error"] = str(error.detail)
        response.headers["X-GWV3-Receiver-Status"] = "stale"
        return cached
    if not isinstance(current, dict):
        raise HTTPException(status_code=502, detail="receiver admin returned invalid status")
    current = dict(current)
    current["receiver_admin_stale"] = False
    current["receiver_admin_stale_age_ms"] = 0
    with STATUS_CACHE_LOCK:
        STATUS_CACHE = dict(current)
        STATUS_CACHE_MONOTONIC = time.monotonic()
    response.headers["X-GWV3-Receiver-Status"] = "live"
    return current


def _build_device_info(status_data: dict[str, Any], sender_id: str | None, mac: str | None) -> dict[str, Any]:
    devices: dict[str, dict[str, Any]] = {}
    for camera in status_data.get("cameras", []):
        if not isinstance(camera, dict):
            continue
        current_sender_id = str(camera.get("sender_id", ""))
        if not current_sender_id:
            continue
        received_us = int(camera.get("device_info_received_us", 0) or 0)
        device = devices.get(current_sender_id)
        if device is None:
            device = {
                "sender_id": current_sender_id,
                "device_info_available": False,
                "device_info_version": 0,
                "host_name": "",
                "wifi_interface": "wlan0",
                "wifi_permanent_mac": "",
                "mac_is_permanent": False,
                "mac_source": "unavailable",
                "device_date": "",
                "device_time": "",
                "device_system_time_us": 0,
                "timezone": "",
                "device_info_received_us": 0,
                "online": False,
                "camera_ids": [],
                "source_ips": [],
            }
            devices[current_sender_id] = device
        device["online"] = bool(device["online"] or camera.get("online", False))
        camera_id = str(camera.get("camera_id", ""))
        if camera_id and camera_id not in device["camera_ids"]:
            device["camera_ids"].append(camera_id)
        source_ip = str(camera.get("sender_source_ip", ""))
        if source_ip and source_ip not in device["source_ips"]:
            device["source_ips"].append(source_ip)
        if received_us < int(device["device_info_received_us"]):
            continue
        version = int(camera.get("device_info_version", 0) or 0)
        device.update({
            "device_info_available": version > 0,
            "device_info_version": version,
            "host_name": str(camera.get("host_name", "")),
            "wifi_interface": str(camera.get("wifi_interface", "wlan0")),
            "wifi_permanent_mac": str(camera.get("wifi_permanent_mac", "")).lower(),
            "mac_is_permanent": bool(camera.get("mac_is_permanent", False)),
            "mac_source": str(camera.get("mac_source", "unavailable")),
            "device_date": str(camera.get("device_date", "")),
            "device_time": str(camera.get("device_time", "")),
            "device_system_time_us": int(camera.get("device_system_time_us", 0) or 0),
            "timezone": str(camera.get("timezone", "")),
            "device_info_received_us": received_us,
        })

    now_us = time.time_ns() // 1000
    result = []
    normalized_mac = mac.strip().lower() if mac else None
    for device in devices.values():
        if sender_id is not None and device["sender_id"] != sender_id:
            continue
        if normalized_mac is not None and device["wifi_permanent_mac"] != normalized_mac:
            continue
        received_us = int(device["device_info_received_us"])
        device["device_info_age_ms"] = max(0, (now_us - received_us) // 1000) if received_us > 0 else None
        device["camera_ids"].sort()
        device["source_ips"].sort()
        result.append(device)
    result.sort(key=lambda item: item["sender_id"])
    return {
        "ok": True,
        "protocol_version": "1.0",
        "generated_at": datetime.now().astimezone().isoformat(timespec="seconds"),
        "receiver_admin_stale": bool(status_data.get("receiver_admin_stale", False)),
        "device_count": len(result),
        "devices": result,
    }


@app.get("/api/device-info")
def device_info(
    response: FastAPIResponse,
    sender_id: str | None = Query(None, max_length=64),
    mac: str | None = Query(None, max_length=32),
) -> Any:
    response.headers["Cache-Control"] = "no-store"
    response.headers["Access-Control-Allow-Origin"] = "*"
    status_response = FastAPIResponse()
    return _build_device_info(status(status_response), sender_id, mac)


@app.get("/api/config")
def config() -> Any:
    return _request("GET", "/api/config")


@app.get("/api/audio/status")
def audio_status() -> Any:
    return _audio_request("GET", "/api/status")


@app.post("/api/audio/start-all")
def audio_start_all() -> Any:
    return _audio_request("POST", "/api/start-all")


@app.post("/api/audio/stop-all")
def audio_stop_all() -> Any:
    return _audio_request("POST", "/api/stop-all")


@app.post("/api/audio/start-sender")
def audio_start_sender(sender_id: str = Query(...)) -> Any:
    query = urllib.parse.urlencode({"sender_id": sender_id})
    return _audio_request("POST", f"/api/start-sender?{query}")


@app.post("/api/audio/stop-sender")
def audio_stop_sender(sender_id: str = Query(...)) -> Any:
    query = urllib.parse.urlencode({"sender_id": sender_id})
    return _audio_request("POST", f"/api/stop-sender?{query}")


@app.post("/api/record/start-all")
def start_all(file_prefix: str | None = Query(None)) -> Any:
    query = ""
    if file_prefix is not None:
        query = "?" + urllib.parse.urlencode({"file_prefix": file_prefix})
    return _request("POST", f"/api/record/start-all{query}")


@app.post("/api/record/stop-all")
def stop_all() -> Any:
    return _request("POST", "/api/record/stop-all", timeout_s=ADMIN_RECORD_STOP_TIMEOUT_S)


@app.post("/api/record/start-sender")
def start_sender(sender_id: str = Query(...)) -> Any:
    query = urllib.parse.urlencode({"sender_id": sender_id})
    return _request("POST", f"/api/record/start-sender?{query}")


@app.post("/api/record/stop-sender")
def stop_sender(sender_id: str = Query(...)) -> Any:
    query = urllib.parse.urlencode({"sender_id": sender_id})
    return _request(
        "POST",
        f"/api/record/stop-sender?{query}",
        timeout_s=ADMIN_RECORD_STOP_TIMEOUT_S,
    )


@app.post("/api/record/start")
def start_camera(sender_id: str = Query(...), camera_id: str = Query(...), file_prefix: str | None = Query(None)) -> Any:
    params = {"sender_id": sender_id, "camera_id": camera_id}
    if file_prefix is not None:
        params["file_prefix"] = file_prefix
    query = urllib.parse.urlencode(params)
    return _request("POST", f"/api/record/start?{query}")


@app.post("/api/record/stop")
def stop_camera(sender_id: str = Query(...), camera_id: str = Query(...)) -> Any:
    query = urllib.parse.urlencode({"sender_id": sender_id, "camera_id": camera_id})
    return _request("POST", f"/api/record/stop?{query}", timeout_s=ADMIN_RECORD_STOP_TIMEOUT_S)


@app.post("/api/camera/name")
def set_camera_name(sender_id: str = Query(...), camera_id: str = Query(...), camera_name: str = Query("")) -> Any:
    query = urllib.parse.urlencode({"sender_id": sender_id, "camera_id": camera_id, "camera_name": camera_name})
    return _request("POST", f"/api/camera/name?{query}")


@app.post("/api/camera/prefix")
def set_camera_prefix(sender_id: str = Query(...), camera_id: str = Query(...), prefix: str = Query("")) -> Any:
    query = urllib.parse.urlencode({"sender_id": sender_id, "camera_id": camera_id, "prefix": prefix})
    return _request("POST", f"/api/camera/prefix?{query}")


@app.post("/api/storage/prefix")
def set_storage_prefix(prefix: str = Query("")) -> Any:
    query = urllib.parse.urlencode({"prefix": prefix})
    return _request("POST", f"/api/storage/prefix?{query}")


@app.post("/api/preview/main-target")
def set_main_preview_target(sender_id: str = Query(...), camera_id: str = Query(...)) -> Any:
    query = urllib.parse.urlencode({"sender_id": sender_id, "camera_id": camera_id})
    return _request("POST", f"/api/preview/main-target?{query}")


@app.get("/api/preview/depth")
def depth_preview(sender_id: str = Query(...), camera_id: str = Query(...)) -> Response:
    query = urllib.parse.urlencode({"sender_id": sender_id, "camera_id": camera_id})
    url = ADMIN_BASE.rstrip("/") + f"/api/preview/depth?{query}"
    req = urllib.request.Request(url, method="GET")
    try:
        with urllib.request.urlopen(req, timeout=3) as resp:
            media_type = resp.headers.get_content_type() or "image/jpeg"
            return Response(content=resp.read(), media_type=media_type, headers={"Cache-Control": "no-store"})
    except urllib.error.HTTPError as exc:
        exc.close()
        raise HTTPException(status_code=404, detail=f"depth preview unavailable: {exc}") from exc
    except Exception as exc:
        raise HTTPException(status_code=404, detail=f"depth preview unavailable: {exc}") from exc


@app.get("/api/preview/rgb")
def rgb_preview(sender_id: str = Query(...), camera_id: str = Query(...)) -> Response:
    query = urllib.parse.urlencode({"sender_id": sender_id, "camera_id": camera_id})
    url = ADMIN_BASE.rstrip("/") + f"/api/preview/rgb?{query}"
    req = urllib.request.Request(url, method="GET")
    try:
        with urllib.request.urlopen(req, timeout=3) as resp:
            media_type = resp.headers.get_content_type() or "image/bmp"
            return Response(content=resp.read(), media_type=media_type, headers={"Cache-Control": "no-store"})
    except urllib.error.HTTPError as exc:
        exc.close()
        raise HTTPException(status_code=404, detail=f"rgb preview unavailable: {exc}") from exc
    except Exception as exc:
        raise HTTPException(status_code=404, detail=f"rgb preview unavailable: {exc}") from exc


@app.get("/api/preview/rgb-main")
def rgb_main_preview(sender_id: str = Query(...), camera_id: str = Query(...)) -> Response:
    query = urllib.parse.urlencode({"sender_id": sender_id, "camera_id": camera_id})
    url = ADMIN_BASE.rstrip("/") + f"/api/preview/rgb-main?{query}"
    req = urllib.request.Request(url, method="GET")
    try:
        with urllib.request.urlopen(req, timeout=3) as resp:
            media_type = resp.headers.get_content_type() or "image/jpeg"
            return Response(content=resp.read(), media_type=media_type, headers={"Cache-Control": "no-store"})
    except urllib.error.HTTPError as exc:
        exc.close()
        raise HTTPException(status_code=404, detail=f"main rgb preview unavailable: {exc}") from exc
    except Exception as exc:
        raise HTTPException(status_code=404, detail=f"main rgb preview unavailable: {exc}") from exc


@app.get("/api/preview/rgb-h264-frames")
async def rgb_h264_frames(
    sender_id: str = Query(...),
    camera_id: str = Query(...),
    quality: str = Query("preview", pattern="^(preview|main)$"),
    metadata: str = Query("legacy", pattern="^(legacy|global)$"),
) -> StreamingResponse:
    query = urllib.parse.urlencode(
        {"sender_id": sender_id, "camera_id": camera_id, "quality": quality, "metadata": metadata}
    )
    url = ADMIN_BASE.rstrip("/") + f"/api/preview/rgb-h264-frames?{query}"
    stream_slots = MAIN_STREAM_SLOTS if quality == "main" else PREVIEW_STREAM_SLOTS

    if not stream_slots.acquire(blocking=False):
        raise HTTPException(status_code=503, detail=f"{quality} stream capacity reached")

    parsed_url = urllib.parse.urlsplit(url)
    writer = None
    try:
        reader, writer = await asyncio.wait_for(
            asyncio.open_connection(parsed_url.hostname or "127.0.0.1", parsed_url.port or 80),
            timeout=3,
        )
        request_target = parsed_url.path or "/"
        if parsed_url.query:
            request_target += "?" + parsed_url.query
        request = (
            f"GET {request_target} HTTP/1.1\r\n"
            f"Host: {parsed_url.hostname or '127.0.0.1'}\r\n"
            "Connection: close\r\n\r\n"
        ).encode("ascii")
        writer.write(request)
        await writer.drain()
        header_bytes = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), timeout=30)
        header_lines = header_bytes[:-4].decode("iso-8859-1").split("\r\n")
        status_parts = header_lines[0].split(" ", 2)
        status_code = int(status_parts[1]) if len(status_parts) > 1 else 502
        upstream_headers = {}
        for line in header_lines[1:]:
            if ":" in line:
                name, value = line.split(":", 1)
                upstream_headers[name.strip().lower()] = value.strip()
        if status_code != 200:
            writer.close()
            try:
                await writer.wait_closed()
            except Exception:
                pass
            writer = None
            stream_slots.release()
            raise HTTPException(status_code=status_code, detail=f"{quality} stream unavailable")
    except HTTPException:
        raise
    except Exception as exc:
        if writer is not None:
            writer.close()
            try:
                await writer.wait_closed()
            except Exception:
                pass
        stream_slots.release()
        raise HTTPException(status_code=502, detail=f"{quality} stream unavailable: {exc}") from exc

    async def body():
        try:
            while True:
                chunk = await reader.read(64 * 1024)
                if not chunk:
                    break
                yield chunk
        finally:
            writer.close()
            stream_slots.release()

    actual_quality = upstream_headers.get("x-gwv3-rgb-stream", quality)
    frame_version = upstream_headers.get("x-gwv3-frame-version", "2" if metadata == "global" else "1")
    return StreamingResponse(
        body(),
        media_type="application/octet-stream",
        headers={
            "Cache-Control": "no-store",
            "X-Accel-Buffering": "no",
            "X-GWV3-Rgb-Quality-Requested": quality,
            "X-GWV3-Rgb-Stream": actual_quality,
            "X-GWV3-Frame-Version": frame_version,
        },
    )


async def _open_admin_stream(path: str) -> tuple[asyncio.StreamReader, asyncio.StreamWriter]:
    parsed_url = urllib.parse.urlsplit(ADMIN_BASE.rstrip("/") + path)
    writer = None
    try:
        reader, writer = await asyncio.wait_for(
            asyncio.open_connection(parsed_url.hostname or "127.0.0.1", parsed_url.port or 80),
            timeout=3,
        )
        target = parsed_url.path or "/"
        if parsed_url.query:
            target += "?" + parsed_url.query
        writer.write(
            (
                f"GET {target} HTTP/1.1\r\n"
                f"Host: {parsed_url.hostname or '127.0.0.1'}\r\n"
                "Connection: close\r\n\r\n"
            ).encode("ascii")
        )
        await writer.drain()
        header = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), timeout=5)
        status_line = header.split(b"\r\n", 1)[0].decode("iso-8859-1")
        status_parts = status_line.split(" ", 2)
        status_code = int(status_parts[1]) if len(status_parts) > 1 else 502
        if status_code != 200:
            raise HTTPException(status_code=503, detail="RGB preview stream is warming up")
        return reader, writer
    except Exception:
        if writer is not None:
            writer.close()
            with contextlib.suppress(Exception):
                await writer.wait_closed()
        raise


@app.get("/api/preview/rgb-video")
async def rgb_video(sender_id: str = Query(...), camera_id: str = Query(...)) -> StreamingResponse:
    if not PREVIEW_STREAM_SLOTS.acquire(blocking=False):
        raise HTTPException(status_code=503, detail="preview stream capacity reached")

    upstream_writer = None
    actual_quality = "preview"
    try:
        for attempt in range(2):
            query = urllib.parse.urlencode(
                {"sender_id": sender_id, "camera_id": camera_id, "quality": "preview", "metadata": "legacy"}
            )
            try:
                reader, upstream_writer = await _open_admin_stream(f"/api/preview/rgb-h264-frames?{query}")
                break
            except Exception:
                if attempt == 0:
                    await asyncio.sleep(0.15)
        else:
            actual_quality = "main"
            query = urllib.parse.urlencode(
                {"sender_id": sender_id, "camera_id": camera_id, "quality": "main", "metadata": "legacy"}
            )
            reader, upstream_writer = await _open_admin_stream(f"/api/preview/rgb-h264-frames?{query}")
        proc = await asyncio.create_subprocess_exec(
            "ffmpeg",
            "-hide_banner",
            "-loglevel",
            "error",
            "-fflags",
            "+genpts+nobuffer",
            "-flags",
            "low_delay",
            "-probesize",
            "32",
            "-analyzeduration",
            "0",
            "-r",
            "30",
            "-f",
            "h264",
            "-i",
            "pipe:0",
            "-an",
            "-c:v",
            "copy",
            "-movflags",
            "frag_every_frame+empty_moov+default_base_moof+omit_tfhd_offset",
            "-flush_packets",
            "1",
            "-f",
            "mp4",
            "pipe:1",
            stdin=asyncio.subprocess.PIPE,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.DEVNULL,
        )
    except Exception:
        if upstream_writer is not None:
            upstream_writer.close()
            with contextlib.suppress(Exception):
                await upstream_writer.wait_closed()
        PREVIEW_STREAM_SLOTS.release()
        raise

    async def feed_h264() -> None:
        pending = bytearray()
        try:
            while True:
                chunk = await reader.read(64 * 1024)
                if not chunk:
                    break
                pending.extend(chunk)
                while len(pending) >= 12:
                    if pending[:4] != b"GWHP":
                        raise RuntimeError("invalid RGB preview frame magic")
                    header_size = int.from_bytes(pending[6:8], "little")
                    payload_size = int.from_bytes(pending[8:12], "little")
                    if header_size < 40 or header_size > 64 or payload_size > 16 * 1024 * 1024:
                        raise RuntimeError("invalid RGB preview frame size")
                    total_size = header_size + payload_size
                    if len(pending) < total_size:
                        break
                    if proc.stdin is None:
                        return
                    proc.stdin.write(bytes(pending[header_size:total_size]))
                    del pending[:total_size]
                    await proc.stdin.drain()
        finally:
            if proc.stdin is not None:
                proc.stdin.close()

    async def stop_process() -> None:
        upstream_writer.close()
        with contextlib.suppress(Exception):
            await upstream_writer.wait_closed()
        if proc.stdin is not None:
            proc.stdin.close()
            with contextlib.suppress(Exception):
                await proc.stdin.wait_closed()
        if proc.returncode is None:
            with contextlib.suppress(ProcessLookupError):
                proc.terminate()
            try:
                await asyncio.wait_for(proc.wait(), timeout=2)
            except asyncio.TimeoutError:
                with contextlib.suppress(ProcessLookupError):
                    proc.kill()
                await proc.wait()
        else:
            await proc.wait()

    async def body():
        feeder = asyncio.create_task(feed_h264())
        try:
            assert proc.stdout is not None
            while True:
                chunk = await proc.stdout.read(64 * 1024)
                if not chunk:
                    break
                yield chunk
        finally:
            feeder.cancel()
            with contextlib.suppress(asyncio.CancelledError, Exception):
                await feeder
            try:
                await stop_process()
            finally:
                PREVIEW_STREAM_SLOTS.release()

    return StreamingResponse(
        body(),
        media_type="video/mp4",
        headers={
            "Cache-Control": "no-store",
            "X-Accel-Buffering": "no",
            "X-GWV3-Rgb-Stream": actual_quality,
        },
    )
