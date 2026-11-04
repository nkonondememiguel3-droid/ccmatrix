#!/bin/sh
# Start ccmatrix full-screen when the GNOME desktop has been idle.
# Works on Wayland and X11: it asks GNOME's own idle monitor (Mutter)
# how long it has been since the last key press or mouse movement.
#
# Usage: ccmatrix-gnome-idle.sh [idle-seconds]      (default 300)
# Set CCMATRIX_TERMINAL to use another terminal, e.g.
#   CCMATRIX_TERMINAL="kgx --fullscreen --" (GNOME Console)

IDLE_SECONDS=${1:-300}
TERMINAL_CMD=${CCMATRIX_TERMINAL:-gnome-terminal --full-screen --}
CHECK_EVERY=5
launched=0

idle_ms() {
  # Prints e.g. "(uint64 12345,)"; keep just the number.
  gdbus call --session --dest org.gnome.Mutter.IdleMonitor \
    --object-path /org/gnome/Mutter/IdleMonitor/Core \
    --method org.gnome.Mutter.IdleMonitor.GetIdletime 2>/dev/null |
    sed -n 's/.*uint64 \([0-9][0-9]*\).*/\1/p'
}

while :; do
  ms=$(idle_ms)
  if [ -n "$ms" ]; then
    if [ "$ms" -ge $((IDLE_SECONDS * 1000)) ]; then
      # Launch once per idle period, and never twice at the same time.
      if [ "$launched" -eq 0 ] && ! pgrep -x ccmatrix >/dev/null; then
        $TERMINAL_CMD ccmatrix -s &
        launched=1
      fi
    else
      launched=0 # the user is back; arm for the next idle period
    fi
  fi
  sleep "$CHECK_EVERY"
done
