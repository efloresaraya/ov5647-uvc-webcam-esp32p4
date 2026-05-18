#!/usr/bin/env python3
"""
capture.py — Single-frame capture CLI for OV5647 UVC Webcam (ESP32-P4)

Designed for use as a tool by AI agents: outputs the saved file path to stdout
so the caller knows exactly where the image landed.

Usage:
  python3 capture.py                          # save frame_YYYYMMDD_HHMMSS.jpg
  python3 capture.py -o photo.png             # save to specific path
  python3 capture.py -o photo.png -n 3        # capture 3 frames (best of 3)
  python3 capture.py --base64                 # print JPEG as base64 to stdout
  python3 capture.py --base64 --png           # same but PNG
  python3 capture.py --list                   # list available cameras
  python3 capture.py -d 1                     # force camera index 1

Requires: pip install opencv-python
macOS: Terminal must have Camera permission →
       System Settings > Privacy & Security > Camera
"""

import cv2
import argparse
import base64
import sys
import time
import os
from datetime import datetime

# ── Camera detection ──────────────────────────────────────────────────────────

WARMUP_FRAMES = 6        # UVC devices need a few frames before color stabilises
WARMUP_TIMEOUT = 4.0     # seconds
MAX_DEVICE_INDEX = 6


def _try_open(index: int):
    """Return (cap, width, height) if camera index opens and delivers a frame."""
    cap = cv2.VideoCapture(index, cv2.CAP_AVFOUNDATION)
    if not cap.isOpened():
        return None, 0, 0
    deadline = time.time() + WARMUP_TIMEOUT
    while time.time() < deadline:
        ret, frame = cap.read()
        if ret and frame is not None:
            w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
            h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
            return cap, w, h
        time.sleep(0.05)
    cap.release()
    return None, 0, 0


def list_cameras():
    """Print all camera indices that open and deliver a frame."""
    found = False
    for i in range(MAX_DEVICE_INDEX):
        cap, w, h = _try_open(i)
        if cap is not None:
            print(f"  [{i}]  {w}x{h}")
            cap.release()
            found = True
    if not found:
        print("  (no cameras found)")


def find_camera():
    """Return (cap, index) for the first camera that opens and delivers a frame."""
    for i in range(MAX_DEVICE_INDEX):
        cap, w, h = _try_open(i)
        if cap is not None:
            return cap, i
    return None, -1


# ── Frame capture ─────────────────────────────────────────────────────────────

def capture_best(cap, n: int = 1):
    """
    Read up to n frames and return the last successful one.
    Extra frames let the auto-exposure settle after open.
    """
    frame = None
    for _ in range(max(n, WARMUP_FRAMES)):
        ret, f = cap.read()
        if ret and f is not None:
            frame = f
    return frame


# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Capture a frame from the OV5647 UVC Webcam (ESP32-P4 Nano).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("-o", "--output",
                        help="Output file path. Inferred from --png if omitted.")
    parser.add_argument("-n", "--count", type=int, default=1, metavar="N",
                        help="Read N extra frames so auto-exposure can settle (default 1).")
    parser.add_argument("-d", "--device", type=int, default=-1, metavar="INDEX",
                        help="Force camera index instead of auto-detect.")
    parser.add_argument("--base64", action="store_true",
                        help="Print the encoded image to stdout instead of saving a file.")
    parser.add_argument("--png", action="store_true",
                        help="Encode as PNG (default is JPEG).")
    parser.add_argument("--list", action="store_true",
                        help="List available cameras and exit.")
    parser.add_argument("--quiet", "-q", action="store_true",
                        help="Suppress informational messages (only output path or base64).")
    args = parser.parse_args()

    def log(msg):
        if not args.quiet:
            print(f"[capture] {msg}", file=sys.stderr)

    # ── List mode ────────────────────────────────────────────────────────────
    if args.list:
        print("Available cameras:")
        list_cameras()
        sys.exit(0)

    # ── Open camera ──────────────────────────────────────────────────────────
    if args.device >= 0:
        log(f"Opening camera index {args.device} ...")
        cap, w, h = _try_open(args.device)
        idx = args.device
    else:
        log("Auto-detecting camera ...")
        cap, idx = find_camera()

    if cap is None:
        print("ERROR: no camera found.", file=sys.stderr)
        print("       Make sure Terminal has Camera permission in", file=sys.stderr)
        print("       System Settings > Privacy & Security > Camera", file=sys.stderr)
        sys.exit(1)

    w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    log(f"Camera [{idx}] {w}x{h} — capturing ...")

    # ── Capture ───────────────────────────────────────────────────────────────
    frame = capture_best(cap, args.count)
    cap.release()

    if frame is None:
        print("ERROR: failed to read frame.", file=sys.stderr)
        sys.exit(1)

    ext = ".png" if args.png else ".jpg"
    encode_params = [] if args.png else [cv2.IMWRITE_JPEG_QUALITY, 92]
    ret, buf = cv2.imencode(ext, frame, encode_params)
    if not ret:
        print("ERROR: encoding failed.", file=sys.stderr)
        sys.exit(1)

    # ── Output ────────────────────────────────────────────────────────────────
    if args.base64:
        # Print MIME prefix so callers can embed directly in data URIs
        mime = "image/png" if args.png else "image/jpeg"
        b64 = base64.b64encode(buf).decode()
        print(f"data:{mime};base64,{b64}")
    else:
        if args.output:
            path = args.output
        else:
            ts = datetime.now().strftime("%Y%m%d_%H%M%S")
            path = f"frame_{ts}{ext}"

        # Ensure output directory exists
        out_dir = os.path.dirname(path)
        if out_dir:
            os.makedirs(out_dir, exist_ok=True)

        with open(path, "wb") as f:
            f.write(buf.tobytes())

        log(f"Saved {path}  ({os.path.getsize(path):,} bytes)")
        # Print path to stdout — the only stdout output when saving to file.
        # AI tools capture this line to know where the image is.
        print(path)


if __name__ == "__main__":
    main()
