#!/bin/sh
# Starts a VNC X server, a window manager and noVNC, then runs PhotoC on that display.
# Browse to http://localhost:6080; the container stops when PhotoC exits.
set -eu

export DISPLAY=:1

Xvnc "$DISPLAY" -geometry "${VNC_GEOMETRY:-1920x1080}" -depth 24 -localhost -SecurityTypes None -AlwaysShared -rfbport 5901 &
for _ in $(seq 50); do
  [ -e /tmp/.X11-unix/X1 ] && break
  sleep 0.1
done

openbox &
websockify --web /usr/share/novnc 6080 localhost:5901 &

exec photoc "$@"
