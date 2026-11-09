#!/bin/bash
# Builds a release tarball. Enforced, not remembered:
#  - the release build folder and old archives for this version are removed first
#  - tests must pass
#  - every shipped ELF must be stripped (scripts/check-stripped.sh)
#  - the binary must report the expected version
#  - every shipped program must have been built by this run (fresh time stamp)
set -euo pipefail
cd "$(dirname "$0")/.."
version=$(sed -n "s/^ *version : '\([^']*\)'.*/\1/p" meson.build | head -1)
[ -n "$version" ] || { echo "cannot read version from meson.build" >&2; exit 1; }
arch=$(uname -m)
name="bt-phone-recorder-$version-linux-$arch"
build=build-release
stage="$build/stage"
mkdir -p dist

echo "== cleaning ($build, dist/$name.*)"
rm -rf "$build" "dist/$name" "dist/$name.tar.xz" "dist/$name.tar.xz.sha256"

stamp="dist/.release-start-$$"
touch "$stamp"
trap 'rm -f "$stamp"' EXIT

echo "== configuring and building $version"
meson setup "$build" --buildtype=release -Dstrip=true --prefix=/usr >/dev/null
ninja -C "$build"
meson test -C "$build" --print-errorlogs

echo "== staging"
DESTDIR="$PWD/$stage" meson install -C "$build" --no-rebuild >/dev/null

echo "== checking that everything shipped is stripped"
scripts/check-stripped.sh "$stage"

echo "== version check"
got=$("$stage/usr/bin/bt-phone-recorder" --version)
[ "$got" = "bt-phone-recorder $version" ] || { echo "version mismatch: $got" >&2; exit 1; }

echo "== freshness check"
for f in "$stage/usr/bin/bt-phone-recorder" "$stage/usr/libexec/bpr-userchan-helper"; do
    [ -f "$f" ] || { echo "missing: $f" >&2; exit 1; }
    [ "$f" -nt "$stamp" ] || { echo "not built by this run: $f" >&2; exit 1; }
done

cp -a "$stage" "dist/$name"
cp LICENSE NOTICE README.md ACCESSIBILITY.md "dist/$name/"
tar -C dist -cJf "dist/$name.tar.xz" "$name"
rm -rf "dist/$name"
(cd dist && sha256sum "$name.tar.xz" > "$name.tar.xz.sha256")
echo "== done: dist/$name.tar.xz"
