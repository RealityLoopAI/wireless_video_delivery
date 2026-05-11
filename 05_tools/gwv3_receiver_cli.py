#!/usr/bin/env python3
import argparse
import json
import urllib.parse
import urllib.request


DEFAULT_ADMIN = "http://127.0.0.1:18080"


def request(method: str, path: str, admin: str):
    req = urllib.request.Request(admin.rstrip("/") + path, method=method)
    with urllib.request.urlopen(req, timeout=5) as resp:
        return json.loads(resp.read().decode("utf-8"))


def main() -> int:
    parser = argparse.ArgumentParser(description="Gemini v3 receiver CLI")
    parser.add_argument("--admin", default=DEFAULT_ADMIN, help="receiver admin base URL")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("status")
    sub.add_parser("start-all")
    sub.add_parser("stop-all")
    start = sub.add_parser("start")
    start.add_argument("sender_id")
    start.add_argument("camera_id")
    stop = sub.add_parser("stop")
    stop.add_argument("sender_id")
    stop.add_argument("camera_id")
    args = parser.parse_args()

    if args.command == "status":
        data = request("GET", "/api/status", args.admin)
    elif args.command == "start-all":
        data = request("POST", "/api/record/start-all", args.admin)
    elif args.command == "stop-all":
        data = request("POST", "/api/record/stop-all", args.admin)
    elif args.command == "start":
        query = urllib.parse.urlencode({"sender_id": args.sender_id, "camera_id": args.camera_id})
        data = request("POST", f"/api/record/start?{query}", args.admin)
    elif args.command == "stop":
        query = urllib.parse.urlencode({"sender_id": args.sender_id, "camera_id": args.camera_id})
        data = request("POST", f"/api/record/stop?{query}", args.admin)
    else:
        raise RuntimeError(args.command)

    print(json.dumps(data, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
