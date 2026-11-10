#!/bin/bash
# Packages Phone Recorder as an AppImage: one file you download, make
# executable and run.
#
#   scripts/build-appimage.sh            # into dist/
#   OUT=dist/test scripts/build-appimage.sh
#
# Method (as in det-siste-speilet-2's tools/build-appimage.sh):
#  - a clean release build is installed into an AppDir
#  - the libraries the program links against are bundled, EXCEPT the ones
#    that must come from the host to talk to the host: the C library, the
#    graphics stack (GL/EGL/Vulkan/DRM), X11/XCB/Wayland, fonts, PipeWire,
#    PulseAudio/ALSA, D-Bus/systemd/udev. Bundling those is the classic way
#    to make an AppImage that runs nowhere.
#  - AppRun sets LD_LIBRARY_PATH and the data paths, so bundled copies win
#  - the check that matters: the program is actually started from the
#    AppDir under LD_DEBUG, and every bundled library must load from inside
#    it. Anything bundled that still loads from /usr/lib stops the build.
#  - every ELF file is stripped and checked (scripts/check-stripped.sh)
#  - the glibc version it needs is printed: glibc is only forward
#    compatible, so build on the oldest distribution you want to support.
#
# Inside an AppImage the kernel-driver adapter mode is not available (its
# helper needs a file capability, which a squashfs cannot carry), and the
# udev rule must still be installed once (see README). Direct USB works.
#
# Exit 2 means a prerequisite is missing (a skip, not a failure).
set -euo pipefail
cd "$(dirname "$0")/.."

OUT=${OUT:-dist}
BUILD=${BUILD:-build-appimage}
ARCH=${ARCH:-$(uname -m)}
export ARCH
version=$(sed -n "s/^ *version : '\([^']*\)'.*/\1/p" meson.build | head -1)
[ -n "$version" ] || { echo "cannot read the version from meson.build" >&2; exit 1; }
NAME=bt-phone-recorder
APPID=guru.freberg.BtPhoneRecorder

APPIMAGETOOL=${APPIMAGETOOL:-$(command -v appimagetool || true)}
for c in "$HOME/.local/bin/appimagetool-x86_64.AppImage" ./appimagetool-x86_64.AppImage; do
    [ -z "$APPIMAGETOOL" ] && [ -x "$c" ] && APPIMAGETOOL=$c
done
if [ -z "$APPIMAGETOOL" ]; then
    echo "appimagetool not found (Arch: appimagetool; or download it from" >&2
    echo "https://github.com/AppImage/appimagetool/releases and set APPIMAGETOOL)." >&2
    exit 2
fi
for t in strip objdump ldd glib-compile-schemas; do
    command -v "$t" >/dev/null || { echo "$t is missing" >&2; exit 2; }
done

# ---- clean release build ---------------------------------------------------
echo "== clean release build ($BUILD)"
rm -rf "$BUILD"
meson setup "$BUILD" --buildtype=release -Dstrip=true --prefix=/usr >/dev/null
ninja -C "$BUILD"
meson test -C "$BUILD" --print-errorlogs >/dev/null || {
    meson test -C "$BUILD" --print-errorlogs; exit 1; }

APPDIR="$PWD/$BUILD/AppDir"
DESTDIR="$APPDIR" meson install -C "$BUILD" --no-rebuild >/dev/null
BIN="$APPDIR/usr/bin/bt-phone-recorder"
# the udev rule is installed by scripts/install-udev-rule.sh, not by an
# AppImage; the helper cannot work without its capability
rm -rf "$APPDIR/usr/lib/udev" "$APPDIR/usr/libexec"

# ---- libraries -------------------------------------------------------------
# Never bundled: must match the host (see the header).
exclude_re='^(ld-linux.*|libc|libm|libdl|libpthread|librt|libresolv|libutil|libanl|libnsl|libgcc_s|libstdc\+\+|libGL.*|libEGL|libGLX.*|libGLdispatch|libOpenGL|libgbm|libdrm.*|libvulkan|libX11.*|libX[a-z]+|libxcb.*|libxkbcommon.*|libwayland-.*|libfontconfig|libfreetype|libharfbuzz|libexpat|libz|libpipewire-.*|libspa-.*|libpulse.*|libasound|libjack|libdbus-1|libsystemd|libudev|libcap|libgcrypt|libgpg-error|libselinux|libmount|libblkid|libuuid|libcom_err|libkrb5.*|libgssapi_krb5|libk5crypto|libkeyutils|libp11-kit|libgnutls|libnettle|libhogweed|libgmp|libidn2|libunistring|libtasn1|libffi)\.so'

mkdir -p "$APPDIR/usr/lib"
bundled=()
resolve() { # print "soname path" for every library the file needs, recursively (ldd)
    ldd "$1" | sed -n 's/^\t*\([^ ]*\) => \(\/[^ ]*\) .*/\1 \2/p'
}
while read -r soname path; do
    case "$soname" in /*) continue ;; esac   # the dynamic loader itself
    if [[ "$soname" =~ $exclude_re ]]; then continue; fi
    real=$(readlink -f "$path")
    cp -n "$real" "$APPDIR/usr/lib/$(basename "$real")"
    chmod 644 "$APPDIR/usr/lib/$(basename "$real")"
    [ "$(basename "$real")" != "$soname" ] && ln -sf "$(basename "$real")" "$APPDIR/usr/lib/$soname"
    bundled+=("$soname")
done < <(resolve "$BIN")
echo "== bundled ${#bundled[@]} libraries"

# GIO modules of the host are built against the host's GLib, which may not be
# the bundled one: use none (the app needs no GVfs or dconf).
mkdir -p "$APPDIR/usr/lib/gio/modules"
# GTK's own settings schemas, so file dialogs work on hosts without GTK 4
mkdir -p "$APPDIR/usr/share/glib-2.0/schemas"
for s in /usr/share/glib-2.0/schemas/org.gtk.gtk4.*.xml; do
    [ -f "$s" ] && cp "$s" "$APPDIR/usr/share/glib-2.0/schemas/"
done
glib-compile-schemas "$APPDIR/usr/share/glib-2.0/schemas"

# ---- desktop integration -----------------------------------------------------
cp "$APPDIR/usr/share/applications/$APPID.desktop" "$APPDIR/$APPID.desktop"
cp "$APPDIR/usr/share/icons/hicolor/scalable/apps/$APPID.svg" "$APPDIR/$APPID.svg"
ln -sf "$APPID.svg" "$APPDIR/.DirIcon"
mkdir -p "$APPDIR/usr/share/doc/$NAME"
cp LICENSE NOTICE README.md ACCESSIBILITY.md BUILD.md "$APPDIR/usr/share/doc/$NAME/"
cp docs/pdf/*.pdf "$APPDIR/usr/share/doc/$NAME/" 2>/dev/null || true

cat > "$APPDIR/AppRun" <<'EOF'
#!/bin/sh
HERE=$(dirname "$(readlink -f "$0")")
export LD_LIBRARY_PATH="$HERE/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export GSETTINGS_SCHEMA_DIR="$HERE/usr/share/glib-2.0/schemas"
export GIO_MODULE_DIR="$HERE/usr/lib/gio/modules"
export XDG_DATA_DIRS="$HERE/usr/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
export BPR_LOCALEDIR="$HERE/usr/share/locale"
exec "$HERE/usr/bin/bt-phone-recorder" "$@"
EOF
chmod +x "$APPDIR/AppRun"

# ---- strip -------------------------------------------------------------------
while IFS= read -r -d '' f; do
    if [ "$(head -c 4 "$f" | od -An -c | tr -d ' ')" = "177ELF" ]; then
        strip --strip-all "$f"
    fi
done < <(find "$APPDIR" -type f -print0)
scripts/check-stripped.sh "$APPDIR"

# ---- check: bundled libraries really load from the AppDir --------------------
# --version exits before any window opens, but the loader has already mapped
# every linked library by then.
loaded=$(cd "$APPDIR" && LD_DEBUG=libs timeout 20 ./AppRun --version 2>&1 |
         sed -n 's/.*calling init: \(\/.*\.so[^ ]*\)$/\1/p' | sort -u)
[ -n "$loaded" ] || { echo "the program did not start from the AppDir" >&2; exit 1; }
outside=""
for soname in "${bundled[@]}"; do
    hit=$(printf '%s\n' "$loaded" | grep "/$soname\$\|/$(readlink "$APPDIR/usr/lib/$soname" 2>/dev/null || echo "$soname")\$" || true)
    if printf '%s\n' "$hit" | grep -qv "^$APPDIR/usr/lib/" && [ -n "$hit" ]; then
        outside="$outside $soname"
    fi
done
if [ -n "$outside" ]; then
    echo "these bundled libraries loaded from outside the AppDir:$outside" >&2
    exit 1
fi
inside=$(printf '%s\n' "$loaded" | grep -c "^$APPDIR/usr/lib/" || true)
echo "== check: $inside libraries load from inside the AppImage, none of the bundled ones from the system"
(cd "$APPDIR" && ./AppRun --version)

# ---- package -------------------------------------------------------------------
mkdir -p "$OUT"
APPIMAGE="$OUT/$NAME-$version-$ARCH.AppImage"
rm -f "$APPIMAGE"
if ! "$APPIMAGETOOL" --no-appstream "$APPDIR" "$APPIMAGE" >"$BUILD/appimagetool.log" 2>&1; then
    sed 's/^/  /' "$BUILD/appimagetool.log" >&2
    if grep -qi fuse "$BUILD/appimagetool.log"; then
        echo "appimagetool needs FUSE (Arch: fuse2) or run it with --appimage-extract" >&2
        exit 2
    fi
    echo "appimagetool failed" >&2
    exit 1
fi
chmod +x "$APPIMAGE"
(cd "$OUT" && sha256sum "$(basename "$APPIMAGE")" > "$(basename "$APPIMAGE").sha256")

floor=$(for f in "$BIN" "$APPDIR"/usr/lib/*.so*; do
            [ -f "$f" ] && objdump -T "$f" 2>/dev/null | sed -n 's/.*GLIBC_\([0-9][0-9.]*\).*/\1/p'
        done | sort -V | tail -1)
echo "== appimage: $APPIMAGE ($(du -h "$APPIMAGE" | cut -f1))"
echo "   needs glibc $floor or newer (built on this machine; build on an older"
echo "   distribution to reach older systems)"
