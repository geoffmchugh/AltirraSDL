#!/bin/bash
# Launcher shipped with the temporary macOS leak diagnostic release.
# Source/documentation: MAC_OS_LEAK_DEBUG.md.

set -u

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
APP_PATH="$SCRIPT_DIR/AltirraSDL.app"
APP_BIN="$APP_PATH/Contents/MacOS/AltirraSDL"
MODE=${1:-baseline}
STAMP=$(date +%Y%m%d-%H%M%S)
LOG_PATH="$SCRIPT_DIR/AltirraSDL-memory-${MODE}-${STAMP}.log"

if [ ! -x "$APP_BIN" ]; then
    echo "AltirraSDL executable not found: $APP_BIN" >&2
    exit 1
fi

unset ALTIRRA_MAC_LEAK_NEW_FRAMES_ONLY
unset ALTIRRA_MAC_LEAK_DISABLE_UPLOAD
unset ALTIRRA_MAC_LEAK_DISABLE_PRESENT

case "$MODE" in
    baseline)
        ;;
    new-frames-only)
        export ALTIRRA_MAC_LEAK_NEW_FRAMES_ONLY=1
        ;;
    no-upload)
        export ALTIRRA_MAC_LEAK_DISABLE_UPLOAD=1
        ;;
    no-present)
        export ALTIRRA_MAC_LEAK_DISABLE_PRESENT=1
        ;;
    *)
        echo "Usage: $0 {baseline|new-frames-only|no-upload|no-present}" >&2
        exit 2
        ;;
esac

echo "AltirraSDL macOS memory diagnostic"
echo "Mode: $MODE"
echo "Log: $LOG_PATH"
echo "Reproduce the issue, then quit AltirraSDL normally."
echo "Please send the resulting log file to the developers."
echo

export NSUnbufferedIO=YES
"$APP_BIN" 2>&1 | tee "$LOG_PATH"
