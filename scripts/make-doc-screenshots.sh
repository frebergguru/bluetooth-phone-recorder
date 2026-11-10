#!/bin/bash
# Renders the screenshots for docs/images with demo data, off-screen.
#
# A virtual KWin draws the window, sealed off from the desktop you are using:
#  - its own D-Bus session (dbus-run-session), so it cannot register global
#    shortcuts or anything else in your session
#  - its own runtime, config, data, cache and state folders, so it cannot
#    write ~/.config (kwinrc, kwinoutputconfig.json, ...)
# The app runs inside that bubble with Bluetooth off (demo mode) and with
# audio streams that are never linked to real devices (BPR_NO_DEVICE_LINKS=1).
# Only PipeWire is shared, through PIPEWIRE_RUNTIME_DIR, because the app needs
# it to start; it only creates unlinked nodes there.
#
# Usage: scripts/make-doc-screenshots.sh            (English)
#        DOC_LANGUAGE=nb scripts/make-doc-screenshots.sh docs/images/nb
set -euo pipefail
cd "$(dirname "$0")/.."
bin="$PWD/build/src/ui/bt-phone-recorder"
[ -x "$bin" ] || { echo "build first: ninja -C build" >&2; exit 1; }
out="$PWD/${1:-docs/images}"
work="$PWD/scratchpad/docshots"
real_runtime="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
rm -rf "$work"
mkdir -p "$work"/{config/bt-phone-recorder,data,cache,state,run} "$out"
chmod 700 "$work/run"
cat > "$work/config/bt-phone-recorder/settings.ini" <<INI
[bluetooth]
name=Studio Phone Recorder
[recording]
folder=$work/recordings
[audio]
monitor=false
inputs=3
[input-1]
label=Host
send-to-call=true
[input-2]
label=Co-host
send-to-call=true
gain=-3
[input-3]
label=Music bed
send-to-call=false
low-cut=false
[accessibility]
language=${DOC_LANGUAGE:-en}
INI

export WORK="$work" BIN="$bin" OUT="$out" REAL_RUNTIME="$real_runtime"
# use the translations from this build, not an older installed copy
export BPR_LOCALEDIR="$PWD/build/po"
# The private bus itself starts inside the sealed folders, so anything it
# might start also lives there. Portals are disabled so nothing is started.
env -u WAYLAND_DISPLAY -u DISPLAY -u SESSION_MANAGER \
    XDG_RUNTIME_DIR="$work/run" XDG_CONFIG_HOME="$work/config" XDG_DATA_HOME="$work/data" \
    XDG_CACHE_HOME="$work/cache" XDG_STATE_HOME="$work/state" \
    GTK_USE_PORTAL=0 GDK_DEBUG=no-portals ADW_DISABLE_PORTAL=1 \
    dbus-run-session -- bash -c '
set -u
kwin_wayland --virtual --socket docs-wl --no-lockscreen --width 1600 --height 940 \
    >"$WORK/kwin.log" 2>&1 &
kpid=$!
for i in $(seq 1 100); do [ -S "$XDG_RUNTIME_DIR/docs-wl" ] && break; sleep 0.1; done
WAYLAND_DISPLAY=docs-wl GDK_BACKEND=wayland GTK_A11Y=none \
PIPEWIRE_RUNTIME_DIR="$REAL_RUNTIME" BPR_NO_DEVICE_LINKS=1 BPR_DOC_SHOTS="$OUT" \
    timeout 120 "$BIN" >"$WORK/app.log" 2>&1
echo "app exit: $?"
kill $kpid 2>/dev/null; wait $kpid 2>/dev/null
# stop anything else that is still attached to this private bus
for p in $(pgrep -u "$(id -u)"); do
    [ "$p" = "$$" ] && continue
    if tr "\0" "\n" < /proc/$p/environ 2>/dev/null | grep -qx "DBUS_SESSION_BUS_ADDRESS=$DBUS_SESSION_BUS_ADDRESS"; then
        echo "stopping leftover $p $(cat /proc/$p/comm 2>/dev/null)"; kill $p 2>/dev/null
    fi
done
' 2>&1 | grep -E "app exit|stopping leftover" || true
grep -E "docs:|error|warn" "$work/app.log" | grep -v "^\[info\] docs: saved" || true
ls -la "$out"
