#!/bin/bash
# Kiosk launcher (from tty1 autologin). Weston anchors the session; the UI is
# (re)started as its client. If the UI dies, only the UI restarts; if Weston
# dies, the session ends and getty respawns the whole thing.
export XDG_RUNTIME_DIR=/run/user/$(id -u)
[ -d "$XDG_RUNTIME_DIR" ] || { mkdir -p "$XDG_RUNTIME_DIR"; chmod 700 "$XDG_RUNTIME_DIR"; }

pkill -x weston 2>/dev/null
sleep 1
rm -f "$XDG_RUNTIME_DIR/wayland-av" "$XDG_RUNTIME_DIR/wayland-av.lock" 2>/dev/null

weston --backend=drm-backend.so --socket=wayland-av --idle-time=0 >/tmp/w.log 2>&1 &
WPID=$!

for _ in $(seq 1 60); do
    [ -S "$XDG_RUNTIME_DIR/wayland-av" ] && break
    sleep 0.25
done

export WAYLAND_DISPLAY=wayland-av

# keep the UI up; keep the session alive as long as Weston lives.
# Lite C++ dashboard: GStreamer (NVDEC) avatar video + cairo ring/telemetry overlay.
while kill -0 "$WPID" 2>/dev/null; do
    "$HOME/avatar-cpp/avatar_ui" "$HOME/avatar-studio/media/idle.mp4" >/tmp/ui.log 2>&1
    sleep 2
done
