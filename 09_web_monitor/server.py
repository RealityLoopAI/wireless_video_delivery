import asyncio
import json
import os
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any

from fastapi import FastAPI, HTTPException, Query, Response as FastAPIResponse
from fastapi.responses import HTMLResponse, Response, StreamingResponse
from fastapi.staticfiles import StaticFiles


ADMIN_BASE = os.environ.get("GWV3_RECEIVER_ADMIN", "http://127.0.0.1:18080")
ADMIN_TIMEOUT_S = float(os.environ.get("GWV3_RECEIVER_ADMIN_TIMEOUT_S", "3"))
ADMIN_RECORD_STOP_TIMEOUT_S = float(os.environ.get("GWV3_RECEIVER_RECORD_STOP_TIMEOUT_S", "60"))
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


@app.get("/api/config")
def config() -> Any:
    return _request("GET", "/api/config")


@app.post("/api/record/start-all")
def start_all(file_prefix: str | None = Query(None)) -> Any:
    query = ""
    if file_prefix is not None:
        query = "?" + urllib.parse.urlencode({"file_prefix": file_prefix})
    return _request("POST", f"/api/record/start-all{query}")


@app.post("/api/record/stop-all")
def stop_all() -> Any:
    return _request("POST", "/api/record/stop-all", timeout_s=ADMIN_RECORD_STOP_TIMEOUT_S)


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


@app.get("/api/preview/rgb-video")
def rgb_video(sender_id: str = Query(...), camera_id: str = Query(...)) -> StreamingResponse:
    raise HTTPException(status_code=410, detail="mp4 rgb preview fallback disabled; refresh the page to use jpeg fallback")
