#!/bin/bash
# Renders the screenshots for docs/images with demo data. Safe to run on a
# desktop: GTK's Broadway backend draws off-screen (no compositor, nothing
# appears), Bluetooth is not started, and audio streams are never linked to
# real devices (BPR_NO_DEVICE_LINKS=1). Settings live in a throwaway folder.
set -euo pipefail
cd "$(dirname "$0")/.."
bin=build/src/ui/bt-phone-recorder
[ -x "$bin" ] || { echo "build first: ninja -C build" >&2; exit 1; }
work="$PWD/scratchpad/docshots"
out="$PWD/docs/images"
rm -rf "$work"
mkdir -p "$work/config/bt-phone-recorder" "$out"
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
pan=0
INI
num=$((40 + RANDOM % 50))
display=":$num"
gtk4-broadwayd "$display" >"$work/broadway.log" 2>&1 &
bpid=$!
sleep 1
# Broadway only draws while a viewer is connected: attach a headless one
python3 -I scripts/broadway-viewer.py $((8080 + num)) &
vpid=$!
trap 'kill $vpid $bpid 2>/dev/null || true' EXIT
GDK_BACKEND=broadway BROADWAY_DISPLAY="$display" \
BPR_NO_DEVICE_LINKS=1 BPR_DOC_SHOTS="$out" \
XDG_CONFIG_HOME="$work/config" XDG_STATE_HOME="$work/state" \
XDG_DATA_HOME="$work/data" XDG_CACHE_HOME="$work/cache" \
LANGUAGE="${DOC_LANGUAGE:-en}" LC_ALL=C.UTF-8 \
timeout 90 "$bin" 2>&1 | tail -25 || true
ls -la "$out"
