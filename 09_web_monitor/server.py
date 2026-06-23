import json
import os
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any

from fastapi import FastAPI, HTTPException, Query, Response as FastAPIResponse
from fastapi.responses import HTMLResponse, Response, StreamingResponse
from fastapi.staticfiles import StaticFiles


ADMIN_BASE = os.environ.get("GWV3_RECEIVER_ADMIN", "http://127.0.0.1:18080")
ROOT_DIR = Path(__file__).resolve().parent
STATIC_DIR = ROOT_DIR / "static"
INDEX_HTML = STATIC_DIR / "index.html"

app = FastAPI(title="Gemini Wireless Video v3 Monitor")
app.mount("/static", StaticFiles(directory=STATIC_DIR), name="static")


def _request(method: str, path: str) -> Any:
    url = ADMIN_BASE.rstrip("/") + path
    req = urllib.request.Request(url, method=method)
    try:
        with urllib.request.urlopen(req, timeout=3) as resp:
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
    response.headers["Cache-Control"] = "no-store"
    return _request("GET", "/api/status")


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
    return _request("POST", "/api/record/stop-all")


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
    return _request("POST", f"/api/record/stop?{query}")


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
def rgb_h264_frames(sender_id: str = Query(...), camera_id: str = Query(...)) -> StreamingResponse:
    query = urllib.parse.urlencode({"sender_id": sender_id, "camera_id": camera_id})
    url = ADMIN_BASE.rstrip("/") + f"/api/preview/rgb-h264-frames?{query}"

    def body():
        req = urllib.request.Request(url, method="GET")
        with urllib.request.urlopen(req, timeout=30) as resp:
            while True:
                chunk = resp.read(64 * 1024)
                if not chunk:
                    break
                yield chunk

    return StreamingResponse(
        body(),
        media_type="application/octet-stream",
        headers={"Cache-Control": "no-store", "X-Accel-Buffering": "no"},
    )


@app.get("/api/preview/rgb-video")
def rgb_video(sender_id: str = Query(...), camera_id: str = Query(...)) -> StreamingResponse:
    raise HTTPException(status_code=410, detail="mp4 rgb preview fallback disabled; refresh the page to use jpeg fallback")
