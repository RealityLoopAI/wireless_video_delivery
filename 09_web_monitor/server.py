import json
import os
import secrets
import threading
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any

from fastapi import FastAPI, HTTPException, Query, Request, Response as FastAPIResponse
from fastapi.responses import HTMLResponse, JSONResponse, RedirectResponse, Response, StreamingResponse
from fastapi.staticfiles import StaticFiles


ADMIN_BASE = os.environ.get("GWV3_RECEIVER_ADMIN", "http://127.0.0.1:18080")
ADMIN_TIMEOUT_S = float(os.environ.get("GWV3_RECEIVER_ADMIN_TIMEOUT_S", "3"))
ADMIN_RECORD_STOP_TIMEOUT_S = float(os.environ.get("GWV3_RECEIVER_RECORD_STOP_TIMEOUT_S", "60"))
WEB_AUTH_TOKEN = os.environ.get("GWV3_WEB_AUTH_TOKEN", "")
AUTH_COOKIE = "gwv3_session"
MAX_STREAM_CLIENTS = max(1, int(os.environ.get("GWV3_WEB_MAX_STREAM_CLIENTS", "8")))
STREAM_SLOTS = threading.BoundedSemaphore(MAX_STREAM_CLIENTS)
if not WEB_AUTH_TOKEN and os.environ.get("GWV3_WEB_ALLOW_INSECURE", "") != "1":
    raise RuntimeError("GWV3_WEB_AUTH_TOKEN is required; set GWV3_WEB_ALLOW_INSECURE=1 only for local development")
ROOT_DIR = Path(__file__).resolve().parent
STATIC_DIR = ROOT_DIR / "static"
INDEX_HTML = STATIC_DIR / "index.html"

app = FastAPI(title="Gemini Wireless Video v3 Monitor")
app.mount("/static", StaticFiles(directory=STATIC_DIR), name="static")


def _authorized(request: Request) -> bool:
    if not WEB_AUTH_TOKEN:
        return True
    supplied = request.cookies.get(AUTH_COOKIE, "")
    return bool(supplied) and secrets.compare_digest(supplied, WEB_AUTH_TOKEN)


@app.middleware("http")
async def require_api_auth(request: Request, call_next):
    if request.url.path.startswith("/api/") and request.url.path not in {"/api/auth", "/api/logout"} and not _authorized(request):
        return JSONResponse(status_code=401, content={"detail": "authentication required"})
    return await call_next(request)


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
def index(request: Request) -> HTMLResponse:
    if not _authorized(request):
        return HTMLResponse(
            content="""<!doctype html><html lang=\"zh-CN\"><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>Receiver 登录</title><style>body{font-family:sans-serif;display:grid;place-items:center;min-height:100vh;margin:0;background:#111827;color:#f9fafb}form{display:flex;gap:8px;padding:24px;background:#1f2937;border:1px solid #374151;border-radius:8px}input,button{font:inherit;padding:10px 12px;border-radius:6px;border:1px solid #4b5563}input{background:#111827;color:#fff;min-width:280px}button{background:#2563eb;color:#fff;cursor:pointer}</style><form id=\"login\"><input id=\"token\" type=\"password\" autocomplete=\"current-password\" placeholder=\"访问令牌\" autofocus><button>登录</button></form><script>document.querySelector('#login').addEventListener('submit',async(e)=>{e.preventDefault();const token=document.querySelector('#token').value;const r=await fetch('/api/auth',{method:'POST',headers:{'X-GWV3-Token':token}});if(r.ok)location.reload();else alert('令牌错误');});</script></html>""",
            status_code=401,
            headers={"Cache-Control": "no-store"},
        )
    return HTMLResponse(
        content=INDEX_HTML.read_text(encoding="utf-8"),
        headers={"Cache-Control": "no-store"},
    )


@app.post("/api/auth")
def authenticate(request: Request, response: FastAPIResponse) -> Any:
    supplied = request.headers.get("X-GWV3-Token", "")
    if not WEB_AUTH_TOKEN or not supplied or not secrets.compare_digest(supplied, WEB_AUTH_TOKEN):
        raise HTTPException(status_code=401, detail="invalid token")
    response.set_cookie(AUTH_COOKIE, WEB_AUTH_TOKEN, max_age=30 * 24 * 3600, httponly=True, samesite="strict")
    return {"ok": True}


@app.post("/api/logout")
def logout() -> RedirectResponse:
    response = RedirectResponse(url="/", status_code=303)
    response.delete_cookie(AUTH_COOKIE)
    return response


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
def rgb_h264_frames(sender_id: str = Query(...), camera_id: str = Query(...)) -> StreamingResponse:
    query = urllib.parse.urlencode({"sender_id": sender_id, "camera_id": camera_id})
    url = ADMIN_BASE.rstrip("/") + f"/api/preview/rgb-h264-frames?{query}"

    if not STREAM_SLOTS.acquire(blocking=False):
        raise HTTPException(status_code=503, detail="preview stream capacity reached")

    def body():
        try:
            req = urllib.request.Request(url, method="GET")
            with urllib.request.urlopen(req, timeout=30) as resp:
                while True:
                    chunk = resp.read(64 * 1024)
                    if not chunk:
                        break
                    yield chunk
        finally:
            STREAM_SLOTS.release()

    return StreamingResponse(
        body(),
        media_type="application/octet-stream",
        headers={"Cache-Control": "no-store", "X-Accel-Buffering": "no"},
    )


@app.get("/api/preview/rgb-video")
def rgb_video(sender_id: str = Query(...), camera_id: str = Query(...)) -> StreamingResponse:
    raise HTTPException(status_code=410, detail="mp4 rgb preview fallback disabled; refresh the page to use jpeg fallback")
