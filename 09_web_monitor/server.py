import json
import os
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any

from fastapi import FastAPI, HTTPException, Query, Response as FastAPIResponse
from fastapi.responses import HTMLResponse, Response
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
    except Exception as exc:
        raise HTTPException(status_code=502, detail=f"receiver admin unavailable: {exc}") from exc


@app.get("/", response_class=HTMLResponse)
def index() -> str:
    return INDEX_HTML.read_text(encoding="utf-8")


@app.get("/api/status")
def status(response: FastAPIResponse) -> Any:
    response.headers["Cache-Control"] = "no-store"
    return _request("GET", "/api/status")


@app.get("/api/config")
def config() -> Any:
    return _request("GET", "/api/config")


@app.post("/api/record/start-all")
def start_all() -> Any:
    return _request("POST", "/api/record/start-all")


@app.post("/api/record/stop-all")
def stop_all() -> Any:
    return _request("POST", "/api/record/stop-all")


@app.post("/api/record/start")
def start_camera(sender_id: str = Query(...), camera_id: str = Query(...)) -> Any:
    query = urllib.parse.urlencode({"sender_id": sender_id, "camera_id": camera_id})
    return _request("POST", f"/api/record/start?{query}")


@app.post("/api/record/stop")
def stop_camera(sender_id: str = Query(...), camera_id: str = Query(...)) -> Any:
    query = urllib.parse.urlencode({"sender_id": sender_id, "camera_id": camera_id})
    return _request("POST", f"/api/record/stop?{query}")


@app.get("/api/preview/depth")
def depth_preview(sender_id: str = Query(...), camera_id: str = Query(...)) -> Response:
    query = urllib.parse.urlencode({"sender_id": sender_id, "camera_id": camera_id})
    url = ADMIN_BASE.rstrip("/") + f"/api/preview/depth?{query}"
    req = urllib.request.Request(url, method="GET")
    try:
        with urllib.request.urlopen(req, timeout=3) as resp:
            media_type = resp.headers.get_content_type() or "image/jpeg"
            return Response(content=resp.read(), media_type=media_type, headers={"Cache-Control": "no-store"})
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
    except Exception as exc:
        raise HTTPException(status_code=404, detail=f"rgb preview unavailable: {exc}") from exc
