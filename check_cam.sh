#!/usr/bin/env bash
# check_cam.sh — verify capture.py prerequisites before running as an AI agent
#
# Usage:  bash check_cam.sh
# Exits 0 if everything is ready, non-zero with a clear error message if not.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ── 1. Find a Python with cv2 ─────────────────────────────────────────────────
PYTHON=""
for candidate in \
    /opt/anaconda3/bin/python3 \
    /usr/local/bin/python3 \
    /opt/homebrew/bin/python3 \
    "$(command -v python3 2>/dev/null || true)"; do
  if [[ -z "$candidate" || ! -x "$candidate" ]]; then continue; fi
  if "$candidate" -c "import cv2" 2>/dev/null; then
    PYTHON="$candidate"
    break
  fi
done

if [[ -z "$PYTHON" ]]; then
  echo "ERROR: no Python with opencv-python found."
  echo "       Install it with one of:"
  echo "         /opt/anaconda3/bin/pip install opencv-python"
  echo "         brew install python && pip3 install opencv-python"
  exit 1
fi

echo "OK  Python  → $PYTHON  (cv2 $(${PYTHON} -c 'import cv2; print(cv2.__version__)'))"

# ── 2. Check camera access ────────────────────────────────────────────────────
RESULT=$("$PYTHON" "$SCRIPT_DIR/capture.py" --list 2>&1 || true)

if echo "$RESULT" | grep -q "not authorized"; then
  echo "ERROR: macOS has not granted Camera permission to this process."
  echo ""
  echo "  Which app is running this shell?"
  echo "    → Terminal.app : System Settings → Privacy & Security → Camera → Terminal  ✓"
  echo "    → iTerm2       : System Settings → Privacy & Security → Camera → iTerm2   ✓"
  echo "    → VS Code      : System Settings → Privacy & Security → Camera → Code     ✓"
  echo "    → Other agent  : grant Camera permission to that app, then retry."
  echo ""
  echo "  After granting permission, reopen a new terminal window and run this check again."
  exit 2
fi

if echo "$RESULT" | grep -q "no cameras found"; then
  echo "WARN: Python and Camera permission look OK, but no cameras are detected."
  echo "      Make sure the ESP32-P4 Nano is connected via USB-A and the firmware is running."
  exit 3
fi

echo "OK  Camera  → devices found:"
echo "$RESULT" | sed 's/^/    /'
echo ""
echo "Ready. Capture a frame with:"
echo "  $PYTHON $SCRIPT_DIR/capture.py -q -o /tmp/frame.jpg"
