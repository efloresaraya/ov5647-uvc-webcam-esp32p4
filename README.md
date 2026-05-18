# OV5647 UVC Webcam — ESP32-P4 Nano

Plug-and-play USB webcam using the **ESP32-P4 Nano** and an **OV5647 MIPI CSI-2** camera module. No drivers required on macOS, Linux or Windows.

Connect the USB-A cable and the device appears as **"OV5647 UVC Webcam"** in any UVC-compatible app: QuickTime, OBS, Zoom, FaceTime, ffmpeg, OpenCV — everything.

```
MIPI RAW10 1280×960 (binning)
  → ESP32-P4 ISP  (Bayer demosaic + CCM + gamma)
  → RGB565 frame buffer
  → 2× CPU downscale
  → HW JPEG encoder
  → TinyUSB UVC isochronous
  → USB-A host
```

---

## Hardware

| Part | Details |
|------|---------|
| MCU board | ESP32-P4 Nano (chip rev v1.3) |
| Camera | OV5647 MIPI CSI-2 module (Pi Camera v1.3-compatible) |
| USB cable | USB-A → USB-A (connects to host) |
| Debug | USB-C → USB JTAG/serial (log output, independent of UVC) |

---

## Sensor mode — maximum FOV

| Config | Value |
|--------|-------|
| Sensor mode | RAW10 1280×960 @ 45 fps, 2:1 binning |
| Sensor window | x=24..2599, y=12..1943 (~99 % of full 2592×1944 array) |
| ISP output | RGB565 1280×960 |
| UVC output | MJPEG 640×480 @ 15 fps (2× downscale, no rotation needed) |
| Transfer | Isochronous (ISOC) |

The **1280×960 binning mode** captures nearly the entire physical sensor — the widest field of view achievable with this module. The 2:1 binning also improves low-light sensitivity compared to full-resolution modes.

---

## Quick start

```bash
git clone https://github.com/efloresaraya/ov5647-uvc-webcam-esp32p4.git
cd ov5647-uvc-webcam-esp32p4

# Build (ESP-IDF v6.1 required)
idf.py build

# Flash
idf.py -p /dev/cu.usbmodem* flash

# Live preview (requires opencv-python)
pip install opencv-python
python3 view_webcam.py
```

On macOS the first time Terminal needs **Camera permission**: System Settings → Privacy & Security → Camera → Terminal ✓

---

## AI capture CLI

`capture.py` is a single-frame capture tool designed to be called as a tool by AI agents (Claude, GPT-4o, Codex, etc.). It outputs only the saved file path to stdout — nothing else — so the caller can read the image directly.

### Prerequisites

**Step 1 — verify setup** (run once before using with any agent):

```bash
bash check_cam.sh
```

This checks for a Python with `cv2` and confirms macOS Camera permission for the current process.

**Step 2 — note the two common issues on macOS:**

| Issue | Symptom | Fix |
|-------|---------|-----|
| Wrong Python | `ModuleNotFoundError: cv2` | Use the Python that has OpenCV, e.g. `/opt/anaconda3/bin/python3` |
| No Camera permission | `OpenCV: not authorized to capture video` | System Settings → Privacy & Security → Camera → grant to the **app running the shell** (Terminal, iTerm2, VS Code, etc.) |

The system Python at `/usr/bin/python3` will not have `cv2`. Use whichever Python has OpenCV installed.

### Usage

```bash
# Save a frame (prints path to stdout)
/opt/anaconda3/bin/python3 capture.py -q -o /tmp/frame.jpg
# → /tmp/frame.jpg

# Return base64 data URI to stdout (no file written)
/opt/anaconda3/bin/python3 capture.py --base64 -q

# PNG instead of JPEG
/opt/anaconda3/bin/python3 capture.py --png -q -o /tmp/frame.png

# List cameras
/opt/anaconda3/bin/python3 capture.py --list

# Force camera index
/opt/anaconda3/bin/python3 capture.py -d 0 -q
```

### Using with Claude (MCP / tool use)

Claude can capture and analyze frames by running `capture.py` as a Bash tool:

```
run: /opt/anaconda3/bin/python3 capture.py -q -o /tmp/frame.jpg
→ /tmp/frame.jpg

read: /tmp/frame.jpg   (Claude's Read tool — multimodal)
→ Claude sees the image and can describe, measure, or reason about it
```

### Using with Codex or any agent that cannot get Camera permission

macOS grants Camera permission per **app** — not per process. If the agent's
shell process is not the authorized app, OpenCV returns `not authorized to
capture video` even with the right Python.

**Solution: `cam_server.py` — run once in your authorized Terminal.**

```bash
# In your Terminal (which already has Camera permission):
/opt/anaconda3/bin/python3 cam_server.py
# → Listening on http://127.0.0.1:8765
```

Now any agent calls `curl` — no Camera permission required:

```bash
# Codex / any agent:
curl -s http://localhost:8766/frame.jpg -o /tmp/frame.jpg

# Base64 (for vision APIs):
curl -s http://localhost:8766/base64

# Health check:
curl -s http://localhost:8766/health
```

Give Codex this context block:

```
Tool: OV5647 UVC Webcam

cam_server.py is already running in Terminal on http://localhost:8766.
Camera permission is handled by that process — no permission needed here.

Capture a frame to file:
  curl -s http://localhost:8766/frame.jpg -o /tmp/frame.jpg
  → file saved at /tmp/frame.jpg

Capture as base64 JSON:
  curl -s http://localhost:8766/base64
  → {"image": "data:image/jpeg;base64,..."}

Check server is alive:
  curl -s http://localhost:8766/health
  → {"status": "ok", "camera": 0, "resolution": "640x480"}
```

### Using with any vision API

```bash
B64=$(/ opt/anaconda3/bin/python3 capture.py --base64 -q)
# $B64 is a data:image/jpeg;base64,... string
# pass directly to OpenAI vision, Gemini, Anthropic, etc.
```

---

## ESP-IDF version

Tested with **ESP-IDF v6.1-dev** and ESP32-P4 target. The chip rev v1.3 quirks below apply to that release.

---

## Bug fixes and quirks — ESP32-P4 v1.3

This project took significant debugging to get working. Every fix below is either undocumented, subtly wrong in reference examples, or a macOS-specific issue that silently drops frames.

---

### FIX 1 — TinyUSB UVC probe/commit `wLength` (macOS compatibility)

**File**: `managed_components/espressif__tinyusb/src/class/video/video_device.c`

macOS sends the UVC VS_PROBE_CONTROL `SET_CUR` request with `wLength = 0x22` (34 bytes, UVC 1.1 layout) instead of `wLength = 0x30` (48 bytes, UVC 1.5). TinyUSB tries to read the full 48-byte struct into an uninitialized buffer — the leftover bytes cause `_update_streaming_parameters` to reject the negotiation silently.

**Symptom**: Device enumerates on macOS, AVFoundation opens the camera, zero frames arrive.

**Fix**: Clear the struct first, then accept `min(wLength, sizeof(struct))` bytes:

```c
// SET_CUR handler — PROBE and COMMIT
case VIDEO_REQUEST_SET_CUR:
  if (stage == CONTROL_STAGE_SETUP) {
    tu_memclr(&stm->probe_commit_payload,
              sizeof(video_probe_and_commit_control_t));
    TU_VERIFY(tud_control_xfer(rhport, request,
              &stm->probe_commit_payload,
              TU_MIN(request->wLength,
                     sizeof(video_probe_and_commit_control_t))),
              VIDEO_ERROR_UNKNOWN);
  }
```

This is tracked as [TinyUSB PR #1820](https://github.com/hathach/tinyusb/pull/1820).

---

### FIX 2 — ISP demosaic must be explicitly enabled after `esp_isp_enable()`

**File**: `main/camera_pipeline.c`

On ESP32-P4 chip rev v1.3, calling `esp_isp_enable()` does **not** activate the demosaic block. Without it the ISP outputs flat/striped data that the JPEG encoder happily compresses — you get valid JPEG files full of garbage.

**Symptom**: Stream works, frames arrive, but the image is a solid color or horizontal stripes.

**Fix**: Configure and explicitly enable demosaic after ISP enable:

```c
ESP_ERROR_CHECK(esp_isp_enable(isp_proc));

// v1.3 quirk: demosaic is NOT started by esp_isp_enable()
esp_isp_demosaic_config_t demosaic_cfg = {
    .grad_ratio  = { .val = 0 },
    .padding_mode = ISP_DEMOSAIC_EDGE_PADDING_MODE_SRND_DATA,
};
ESP_ERROR_CHECK(esp_isp_demosaic_configure(isp_proc, &demosaic_cfg));
ESP_ERROR_CHECK(esp_isp_demosaic_enable(isp_proc));
```

---

### FIX 3 — DMA `received_size` is MIPI packed bytes, not RGB565 bytes

**File**: `main/camera_pipeline.c`

The ESP32-P4 v1.3 CSI driver sets `esp_cam_ctlr_trans_t.received_size` to the number of **MIPI input bytes** received from the sensor, not the RGB565 output bytes written by the ISP to the DMA buffer.

For RAW8 modes: 1 byte/pixel → `received_size = W × H`  
For RAW10 modes: 4 pixels packed in 5 bytes → `received_size = W × H × 5/4`

If your capture loop validates `received_size == W * H * 2` (RGB565) or even `== W * H` (RAW8 assumption) when running RAW10, **every frame is silently discarded** and the UVC task blocks forever.

**Symptom**: Device enumerates, macOS opens the stream at 640×480, zero frames arrive. No error logs.

**Fix**: Accept both possible counts and log the actual value:

```c
size_t rx          = s_finished_trans.received_size;
size_t raw8_bytes  = (size_t)CAM_HRES * CAM_VRES;          // W*H
size_t raw10_bytes = (size_t)CAM_HRES * CAM_VRES * 5 / 4;  // W*H*1.25

if (rx != raw8_bytes && rx != raw10_bytes) {
    ESP_LOGW(TAG, "DMA size unexpected: rx=%u", (unsigned)rx);
    continue; // skip partial frame
}
```

---

### FIX 4 — ISOC transfer required; BULK triggers AVFoundation C++ exception

**File**: `sdkconfig.defaults`

macOS AVFoundation throws an uncaught C++ exception when a UVC device advertises BULK (not isochronous) transfer. The stack trace appears in the system log:

```
cap.cpp:459 open VIDEOIO(AVFOUNDATION): raised unknown C++ exception
```

**Fix**: Always use isochronous in `sdkconfig.defaults`:

```
CONFIG_UVC_MODE_ISOC_CAM1=y
# CONFIG_UVC_MODE_BULK_CAM1 is not set
```

---

### FIX 5 — Ping-pong DMA buffers prevent frame tearing

**File**: `main/camera_pipeline.c`

The CSI DMA callback `on_get_new_trans` re-arms the DMA immediately after a frame completes — before the CPU finishes processing. With a single frame buffer, the ISP starts overwriting the buffer while the CPU is still reading it, causing a diagonal "rolling shutter" artifact across the image.

The artifact is worse with non-sequential (column-stride) memory access (as in rotation transforms) because the CPU takes longer to process the frame.

**Fix**: Allocate two frame buffers and alternate:

```c
static uint8_t *s_frame_bufs[2];
static volatile int s_write_idx = 0;

// In on_get_new_trans ISR callback:
s_write_idx ^= 1;
trans->buffer = s_frame_bufs[s_write_idx];
trans->buflen  = RGB_FRAME_BYTES;

// In capture task:
uint8_t *done_buf = s_finished_trans.buffer; // buffer that just completed
esp_cache_msync(done_buf, RGB_FRAME_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
// process done_buf — safe, DMA writes to the other buffer
```

---

### FIX 6 — Non-standard portrait resolutions crash AVFoundation

**File**: `sdkconfig.defaults`

If the UVC descriptor advertises only a non-standard portrait resolution (e.g., 400×640), macOS throws the same C++ exception as with BULK transfer:

```
cap.cpp:459 open VIDEOIO(AVFOUNDATION): raised unknown C++ exception
```

Standard landscape resolutions (640×480, 1280×720, 1920×1080, etc.) work fine.

**Fix**: Use VGA 640×480:

```
CONFIG_FRAMESIZE_VGA=y
CONFIG_UVC_CAM1_FRAMESIZE_WIDTH=640
CONFIG_UVC_CAM1_FRAMESIZE_HEIGT=480   # note: typo in Kconfig, not a mistake here
```

---

## Project structure

```
ov5647_uvc_webcam/
├── main/
│   ├── camera_pipeline.c   # MIPI CSI-2 → ISP → JPEG pipeline
│   ├── camera_pipeline.h
│   ├── uvc_stream.c        # TinyUSB UVC glue + fb_get_cb
│   ├── uvc_stream.h
│   ├── main.c
│   └── idf_component.yml   # component dependencies
├── sdkconfig.defaults       # all Kconfig overrides (sensor, USB, UVC)
├── CMakeLists.txt
├── app_config.json
├── view_webcam.py           # OpenCV live preview (interactive)
├── capture.py               # single-frame CLI for AI agents (needs Camera permission)
├── cam_server.py            # local HTTP server — agents call curl, no permission needed
└── check_cam.sh             # prerequisite check (Python + Camera permission)
```

---

## sdkconfig.defaults highlights

```ini
# Sensor: RAW10 1280x960 binning — widest FOV
CONFIG_CAMERA_OV5647_MIPI_RAW10_1280X960_BINNING_45FPS=y

# UVC: single frame VGA, isochronous
CONFIG_FRAMESIZE_VGA=y
CONFIG_UVC_MODE_ISOC_CAM1=y
CONFIG_UVC_CAM1_FRAMESIZE_WIDTH=640
CONFIG_UVC_CAM1_FRAMESIZE_HEIGT=480
CONFIG_UVC_CAM1_FRAMERATE=15

# USB descriptor
CONFIG_TUSB_VID=0x303A
CONFIG_TUSB_PID=0x8000
CONFIG_TUSB_MANUFACTURER="Enki ESP"
CONFIG_TUSB_PRODUCT="OV5647 UVC Webcam"

# PSRAM: 32 MB hex-mode @ 200 MHz
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_HEX=y
CONFIG_SPIRAM_SPEED_200M=y
```

---

## License

Apache 2.0 — see [LICENSE](LICENSE).
