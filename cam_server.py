#!/usr/bin/env python3
"""
cam_server.py — Local HTTP camera server for OV5647 UVC Webcam (ESP32-P4)

Runs in a Terminal that already has macOS Camera permission.
Any process (Codex, Claude, scripts) can then fetch frames via curl —
no Camera permission needed on the caller side.

Usage:
  /opt/anaconda3/bin/python3 cam_server.py          # port 8765
  /opt/anaconda3/bin/python3 cam_server.py --port 9000
  /opt/anaconda3/bin/python3 cam_server.py -d 1     # force camera index

Endpoints:
  GET /frame.jpg        → capture frame, return as JPEG (Content-Type: image/jpeg)
  GET /frame.png        → capture frame, return as PNG
  GET /base64           → capture frame, return JSON {"image": "data:image/jpeg;base64,..."}
  GET /health           → {"status": "ok", "camera": <index>, "resolution": "WxH"}

Agent one-liners:
  curl -s http://localhost:8765/frame.jpg -o /tmp/frame.jpg
  curl -s http://localhost:8765/base64 | python3 -c "import sys,json; print(json.load(sys.stdin)['image'][:80])"

Requires: pip install opencv-python
"""

import cv2
import time
import threading
import argparse
import base64
import json
from http.server import BaseHTTPRequestHandler, HTTPServer

# ── Camera state ──────────────────────────────────────────────────────────────

_cap = None
_cap_lock = threading.Lock()
_cam_index = -1
WARMUP_FRAMES = 6
WARMUP_TIMEOUT = 4.0
MAX_DEVICE_INDEX = 6


def _open_camera(index: int):
    cap = cv2.VideoCapture(index, cv2.CAP_AVFOUNDATION)
    if not cap.isOpened():
        return None
    deadline = time.time() + WARMUP_TIMEOUT
    while time.time() < deadline:
        ret, frame = cap.read()
        if ret and frame is not None:
            return cap
        time.sleep(0.05)
    cap.release()
    return None


def init_camera(forced_index: int = -1):
    global _cap, _cam_index
    indices = [forced_index] if forced_index >= 0 else range(MAX_DEVICE_INDEX)
    for i in indices:
        cap = _open_camera(i)
        if cap is not None:
            _cap = cap
            _cam_index = i
            w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
            h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
            print(f"[cam_server] Camera [{i}] ready  {w}x{h}")
            return True
    return False


def capture_frame():
    """Thread-safe frame capture. Returns encoded JPEG bytes or None."""
    with _cap_lock:
        if _cap is None:
            return None, None
        # Drain stale frames so we get a fresh one
        for _ in range(WARMUP_FRAMES):
            _cap.read()
        ret, frame = _cap.read()
        if not ret or frame is None:
            return None, None
        w = int(_cap.get(cv2.CAP_PROP_FRAME_WIDTH))
        h = int(_cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    return frame, (w, h)


# ── HTTP handler ──────────────────────────────────────────────────────────────

class Handler(BaseHTTPRequestHandler):

    def log_message(self, fmt, *args):
        print(f"[cam_server] {self.address_string()} {fmt % args}")

    def _send(self, status: int, content_type: str, body: bytes):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split("?")[0]

        # ── /health ───────────────────────────────────────────────────────────
        if path == "/health":
            with _cap_lock:
                ok = _cap is not None
                w = int(_cap.get(cv2.CAP_PROP_FRAME_WIDTH)) if ok else 0
                h = int(_cap.get(cv2.CAP_PROP_FRAME_HEIGHT)) if ok else 0
            body = json.dumps({
                "status": "ok" if ok else "no_camera",
                "camera": _cam_index,
                "resolution": f"{w}x{h}" if ok else "n/a",
            }).encode()
            self._send(200 if ok else 503, "application/json", body)
            return

        # ── capture ───────────────────────────────────────────────────────────
        if path not in ("/frame.jpg", "/frame.png", "/base64"):
            self._send(404, "text/plain", b"Not found. Try /frame.jpg  /frame.png  /base64  /health\n")
            return

        frame, _ = capture_frame()
        if frame is None:
            self._send(503, "application/json",
                       json.dumps({"error": "camera not available"}).encode())
            return

        if path == "/frame.jpg":
            ret, buf = cv2.imencode(".jpg", frame, [cv2.IMWRITE_JPEG_QUALITY, 92])
            if not ret:
                self._send(500, "text/plain", b"encode error\n")
                return
            self._send(200, "image/jpeg", buf.tobytes())

        elif path == "/frame.png":
            ret, buf = cv2.imencode(".png", frame)
            if not ret:
                self._send(500, "text/plain", b"encode error\n")
                return
            self._send(200, "image/png", buf.tobytes())

        elif path == "/base64":
            ret, buf = cv2.imencode(".jpg", frame, [cv2.IMWRITE_JPEG_QUALITY, 92])
            if not ret:
                self._send(500, "text/plain", b"encode error\n")
                return
            b64 = base64.b64encode(buf).decode()
            body = json.dumps({"image": f"data:image/jpeg;base64,{b64}"}).encode()
            self._send(200, "application/json", body)


# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Local HTTP camera server — run in authorized Terminal, "
                    "call from any agent via curl."
    )
    parser.add_argument("--port", "-p", type=int, default=8765)
    parser.add_argument("--device", "-d", type=int, default=-1, metavar="INDEX",
                        help="Force camera index (default: auto-detect).")
    parser.add_argument("--host", default="127.0.0.1",
                        help="Bind address (default: 127.0.0.1 — localhost only).")
    args = parser.parse_args()

    print(f"[cam_server] Starting — opening camera ...")
    if not init_camera(args.device):
        print("[cam_server] ERROR: no camera found.")
        print("             Make sure Terminal has Camera permission and the")
        print("             ESP32-P4 is connected with firmware running.")
        raise SystemExit(1)

    server = HTTPServer((args.host, args.port), Handler)
    print(f"[cam_server] Listening on http://{args.host}:{args.port}")
    print(f"[cam_server] Endpoints:")
    print(f"             GET /frame.jpg  → JPEG image")
    print(f"             GET /frame.png  → PNG image")
    print(f"             GET /base64     → JSON with base64 data URI")
    print(f"             GET /health     → JSON status")
    print(f"[cam_server] Agent one-liner:")
    print(f'             curl -s http://localhost:{args.port}/frame.jpg -o /tmp/frame.jpg')
    print(f"[cam_server] Press Ctrl-C to stop.")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[cam_server] Stopped.")
        server.server_close()


if __name__ == "__main__":
    main()
