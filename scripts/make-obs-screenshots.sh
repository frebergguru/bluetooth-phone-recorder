#!/bin/bash
# Renders the OBS screenshots for docs/images/obs-*.png, off-screen, in the
# same sealed bubble as make-doc-screenshots.sh (own D-Bus session, own
# runtime/config/data/cache/state folders, virtual KWin).
#
# OBS gets a throwaway configuration with one scene holding three
# "Phone Recorder" audio sources and NO default Desktop Audio / Mic sources,
# so it never opens a real sound device; its sources only listen to the
# app's virtual sources. The app runs in demo mode with unlinked device
# streams (BPR_NO_DEVICE_LINKS=1). OBS's own remote control (obs-websocket,
# local port 4466, only inside this run) opens the source settings dialog.
# Spectacle, inside the bubble, takes the pictures.
set -euo pipefail
cd "$(dirname "$0")/.."
bin="$PWD/build/src/ui/bt-phone-recorder"
[ -x "$bin" ] || { echo "build first: ninja -C build" >&2; exit 1; }
command -v obs >/dev/null || { echo "OBS Studio is not installed" >&2; exit 1; }
out="$PWD/docs/images"
work="$PWD/scratchpad/obsshots"
real_runtime="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
rm -rf "$work"
mkdir -p "$work"/{config/bt-phone-recorder,data,cache,state,run,appshots} "$out"
chmod 700 "$work/run"

cat > "$work/config/bt-phone-recorder/settings.ini" <<INI
[bluetooth]
name=Studio Phone Recorder
[recording]
folder=$work/recordings
[audio]
monitor=false
inputs=2
[input-1]
label=Host
[input-2]
label=Co-host
INI

obs="$work/config/obs-studio"
mkdir -p "$obs/basic/profiles/Podcast" "$obs/basic/scenes" "$obs/plugin_config/obs-websocket"
for f in global.ini user.ini; do
cat > "$obs/$f" <<INI
[General]
FirstRun=true
LastVersion=$((32 << 24 | 9 << 16))
EnableAutoUpdates=false
ConfirmOnExit=false
[Basic]
Profile=Podcast
ProfileDir=Podcast
SceneCollection=Podcast
SceneCollectionFile=Podcast
[BasicWindow]
gridMode=false
INI
done
cat > "$obs/basic/profiles/Podcast/basic.ini" <<'INI'
[General]
Name=Podcast
[Video]
BaseCX=1920
BaseCY=1080
OutputCX=1920
OutputCY=1080
[Audio]
SampleRate=48000
ChannelSetup=Stereo
INI
cat > "$obs/plugin_config/obs-websocket/config.json" <<'JSON'
{"alerts_enabled": false, "auth_required": false, "first_load": false,
 "server_enabled": true, "server_password": "", "server_port": 4466}
JSON
python3 -I - "$obs/basic/scenes/Podcast.json" <<'PY'
import json, sys, uuid
srcs = [("Phone: full mix", "bpr.full_mix", 1),   # track 1
        ("Phone: caller", "bpr.caller", 2),       # track 2
        ("Phone: Host", "bpr.input_1", 4)]        # track 3
items, sources = [], []
for i, (name, dev, tracks) in enumerate(srcs):
    u = str(uuid.uuid4())
    sources.append({"id": "pulse_input_capture", "versioned_id": "pulse_input_capture",
                    "name": name, "uuid": u, "settings": {"device_id": dev},
                    "mixers": tracks, "volume": 1.0, "muted": False, "enabled": True,
                    "monitoring_type": 0, "flags": 0})
    items.append({"name": name, "source_uuid": u, "id": i + 1, "visible": True,
                  "locked": False, "pos": {"x": 0.0, "y": 0.0}, "scale": {"x": 1.0, "y": 1.0},
                  "align": 5, "bounds_type": 0, "bounds": {"x": 0.0, "y": 0.0}})
scene_uuid = str(uuid.uuid4())
sources.insert(0, {"id": "scene", "versioned_id": "scene", "name": "Podcast", "uuid": scene_uuid,
                   "settings": {"items": items, "id_counter": len(items), "custom_size": False},
                   "mixers": 0, "enabled": True})
coll = {"name": "Podcast", "current_scene": "Podcast", "current_program_scene": "Podcast",
        "scene_order": [{"name": "Podcast"}], "sources": sources, "groups": [],
        "transitions": [], "current_transition": "Fade", "transition_duration": 300,
        "quick_transitions": [], "saved_projectors": [], "preview_locked": False,
        "scaling_enabled": False, "modules": {}}
json.dump(coll, open(sys.argv[1], "w"), indent=2)
PY

export WORK="$work" BIN="$bin" OUT="$out" REAL_RUNTIME="$real_runtime" ROOT="$PWD"
env -u WAYLAND_DISPLAY -u DISPLAY -u SESSION_MANAGER \
    XDG_RUNTIME_DIR="$work/run" XDG_CONFIG_HOME="$work/config" XDG_DATA_HOME="$work/data" \
    XDG_CACHE_HOME="$work/cache" XDG_STATE_HOME="$work/state" \
    GTK_USE_PORTAL=0 GDK_DEBUG=no-portals ADW_DISABLE_PORTAL=1 \
    dbus-run-session -- bash -c '
set -u
kwin_wayland --virtual --socket obs-wl --no-lockscreen --width 1600 --height 940 \
    >"$WORK/kwin.log" 2>&1 &
kpid=$!
for i in $(seq 1 100); do [ -S "$XDG_RUNTIME_DIR/obs-wl" ] && break; sleep 0.1; done
export WAYLAND_DISPLAY=obs-wl QT_QPA_PLATFORM=wayland
# the app provides the virtual sources (demo mode, held for 70 s)
GDK_BACKEND=wayland GTK_A11Y=none PIPEWIRE_RUNTIME_DIR="$REAL_RUNTIME" \
BPR_NO_DEVICE_LINKS=1 BPR_DOC_SHOTS="$WORK/appshots" BPR_DOC_HOLD=70 \
    timeout 150 "$BIN" >"$WORK/app.log" 2>&1 &
apid=$!
sleep 6
PULSE_SERVER="unix:$REAL_RUNTIME/pulse/native" PIPEWIRE_RUNTIME_DIR="$REAL_RUNTIME" \
    timeout 120 obs --multi --disable-shutdown-check --disable-missing-files-check \
    >"$WORK/obs.log" 2>&1 &
opid=$!
sleep 14
spectacle -b -n -a -o "$OUT/obs-main-window.png" >"$WORK/spectacle.log" 2>&1
python3 -I "$ROOT/scripts/obs-ws-request.py" 4466 OpenInputPropertiesDialog \
    "{\"inputName\": \"Phone: caller\"}" >>"$WORK/ws.log" 2>&1
sleep 4
spectacle -b -n -a -o "$OUT/obs-source-properties.png" >>"$WORK/spectacle.log" 2>&1
kill $opid 2>/dev/null; wait $opid 2>/dev/null
kill $apid 2>/dev/null; wait $apid 2>/dev/null
kill $kpid 2>/dev/null; wait $kpid 2>/dev/null
for p in $(pgrep -u "$(id -u)"); do
    [ "$p" = "$$" ] && continue
    if tr "\\0" "\\n" < /proc/$p/environ 2>/dev/null | grep -qx "DBUS_SESSION_BUS_ADDRESS=$DBUS_SESSION_BUS_ADDRESS"; then
        echo "stopping leftover $p $(cat /proc/$p/comm 2>/dev/null)"; kill $p 2>/dev/null
    fi
done
echo done
' 2>&1 | grep -E "^done|stopping leftover" || true
cat "$work/ws.log" 2>/dev/null
ls -la "$out"/obs-*.png 2>/dev/null || echo "no OBS pictures"
