#!/usr/bin/env python3
"""
view_webcam.py — Live viewer for OV5647 UVC Webcam
Usage: python3 view_webcam.py [camera_index]

Opens the OV5647 UVC Webcam (or any camera index) in a window using OpenCV.
Press 'q' or ESC to quit, 's' to save a snapshot.

Requires: pip3 install opencv-python
macOS: Terminal must have Camera permission in
       System Settings > Privacy & Security > Camera
"""

import cv2
import sys
import time
import os

WINDOW_TITLE = "OV5647 UVC Webcam — press Q to quit, S to snapshot"

def find_uvc_camera():
    """Try each camera index; return the first one that opens and delivers a frame."""
    for i in range(4):
        cap = cv2.VideoCapture(i, cv2.CAP_AVFOUNDATION)
        if not cap.isOpened():
            continue
        w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
        h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
        print(f"[uvc] Camera index {i} opened: {w}x{h} — waiting for first frame...")
        # UVC devices need a few frames to warm up — retry for up to 3 s
        deadline = time.time() + 3.0
        while time.time() < deadline:
            ret, frame = cap.read()
            if ret and frame is not None:
                print(f"[uvc] Got first frame from camera {i}")
                return cap, i
            time.sleep(0.05)
        print(f"[uvc] Camera {i} opened but no frames arrived — skipping")
        cap.release()
    return None, -1


def main():
    cam_index = int(sys.argv[1]) if len(sys.argv) > 1 else -1

    if cam_index >= 0:
        print(f"[uvc] Opening camera index {cam_index} ...")
        cap = cv2.VideoCapture(cam_index, cv2.CAP_AVFOUNDATION)
        if not cap.isOpened():
            print(f"[uvc] ERROR: camera {cam_index} could not be opened.")
            print("      Make sure Terminal has Camera permission in")
            print("      System Settings > Privacy & Security > Camera")
            sys.exit(1)
    else:
        print("[uvc] Auto-detecting camera ...")
        cap, cam_index = find_uvc_camera()
        if cap is None:
            print("[uvc] ERROR: no camera found.")
            print("      Make sure Terminal has Camera permission in")
            print("      System Settings > Privacy & Security > Camera")
            sys.exit(1)

    print(f"[uvc] Streaming — {WINDOW_TITLE}")

    frame_count = 0
    t0 = time.time()
    fps_display = 0.0

    while True:
        ret, frame = cap.read()
        if not ret or frame is None:
            print("[uvc] Frame capture failed — retrying...")
            time.sleep(0.1)
            continue

        frame_count += 1
        elapsed = time.time() - t0
        if elapsed >= 1.0:
            fps_display = frame_count / elapsed
            frame_count = 0
            t0 = time.time()

        # Overlay FPS
        cv2.putText(frame, f"{fps_display:.1f} fps  {frame.shape[1]}x{frame.shape[0]}",
                    (10, 28), cv2.FONT_HERSHEY_SIMPLEX, 0.8, (0, 255, 0), 2)

        cv2.imshow(WINDOW_TITLE, frame)

        key = cv2.waitKey(1) & 0xFF
        if key in (ord('q'), 27):  # q or ESC
            break
        elif key == ord('s'):
            fname = f"snapshot_{int(time.time())}.jpg"
            cv2.imwrite(fname, frame)
            print(f"[uvc] Snapshot saved: {fname}")

    cap.release()
    cv2.destroyAllWindows()
    print("[uvc] Stream closed.")


if __name__ == "__main__":
    main()
