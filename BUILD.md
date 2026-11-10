# Building Bluetooth Phone Recorder

Everything you need to build, test, install and clean the project from
scratch. Commands are for Arch/Manjaro; package names on other distributions
are similar.

## 1. Install the tools and libraries (once)

```sh
sudo pacman -S --needed base-devel git meson ninja pkgconf patch \
    gtk4 libadwaita pipewire libusb libsndfile speexdsp zstd xz \
    libsoup3 json-glib gettext
```

`libsoup3` and `json-glib` are only needed for OBS remote control. Without
them, configure with `-Dobs_websocket=disabled` (step 3).

Minimum versions checked by the build: GTK 4.18, libadwaita 1.7,
PipeWire 1.4, libusb 1.0.27, libsndfile 1.2, speexdsp 1.2.1.

## 2. Get the source

```sh
git clone --recurse-submodules <repository-url> bluetooth-phone-recorder
cd bluetooth-phone-recorder
```

Already cloned without submodules, or after pulling a new BTstack version:

```sh
git submodule update --init --recursive
```

BTstack (the Bluetooth stack) lives in `third_party/btstack` and is pinned to
a release. Never edit it there: fixes go into `patches/`, and the build
applies them to copies in the build folder. If a patch no longer applies
after a BTstack update, the build stops with an error.

## 3. Configure and build

```sh
meson setup build
ninja -C build
```

The results:

| File                                | What it is                           |
|-------------------------------------|--------------------------------------|
| `build/src/ui/bt-phone-recorder`    | the app                              |
| `build/tools/bpr-userchan-helper`   | helper for the "kernel driver" adapter mode |
| `build/tools/bpr-bt-test`           | headless Bluetooth hardware test (developers) |

Useful configure options (add to `meson setup`, or change later with
`meson configure build -D...`):

| Option                          | Meaning                                 |
|---------------------------------|-----------------------------------------|
| `--buildtype=release`           | optimised build without debug checks    |
| `-Dstrip=true`                  | strip binaries on install               |
| `-Dobs_websocket=disabled`      | build without OBS remote control        |
| `--prefix=/usr`                 | install location (default `/usr/local`) |

## 4. Run the tests

```sh
meson test -C build --print-errorlogs
```

The tests need no Bluetooth adapter, phone or sound card. They cover the
ring buffer, the mixer (including the rule that the "In call" switch only
changes what the caller hears), clock drift correction, the recorder,
firmware unpacking, the address book (vCard) reader with the cleaning of
numbers before they are dialled, and a check that the clock drift resampler
never allocates memory while audio runs.

## 5. Run without installing

```sh
./build/src/ui/bt-phone-recorder
```

Before the first run, let your user drive USB Bluetooth adapters (once,
asks for your password):

```sh
scripts/install-udev-rule.sh
```

Only for the "kernel driver" adapter mode (MediaTek, Intel AX210 and
newer, Qualcomm and similar chips):

```sh
scripts/setup-userchan-helper.sh
```

## 6. Install and uninstall

```sh
sudo meson install -C build          # installs into /usr/local by default
sudo ninja -C build uninstall        # removes exactly what was installed
```

After changing code, rebuild and install again with the same two
commands (`ninja -C build`, then `sudo meson install -C build`). The
installed helper loses its capability when it is replaced, so run
`scripts/setup-userchan-helper.sh /usr/local/libexec/bpr-userchan-helper`
again if you use the kernel driver mode.

## 7. Release build

```sh
scripts/release.sh
```

It removes `build-release/` and any old archive for the same version,
builds with `--buildtype=release -Dstrip=true`, runs the tests, installs
into a staging folder, stops if any shipped binary is not stripped, checks
the version and writes:

```
dist/bt-phone-recorder-<version>-linux-<arch>.tar.xz
dist/bt-phone-recorder-<version>-linux-<arch>.tar.xz.sha256
```

The version comes from `project(... version : ...)` in `meson.build`.

## 8. AppImage

One file that runs on most Linux distributions without installing:

```sh
scripts/build-appimage.sh
```

It makes a clean release build, bundles the libraries the app needs (but not
the ones that must come from the host: the C library, graphics, X11/Wayland,
fonts, PipeWire and PulseAudio), runs the program from the bundle to prove
every bundled library loads from inside it, strips everything and writes:

```
dist/bt-phone-recorder-<version>-<arch>.AppImage
dist/bt-phone-recorder-<version>-<arch>.AppImage.sha256
```

Needs `appimagetool` (Arch: `appimagetool`). The script prints the glibc
version the AppImage needs: glibc only works forwards, so build on the oldest
distribution you want to support.

Using it:

```sh
chmod +x bt-phone-recorder-*.AppImage
./bt-phone-recorder-*.AppImage
```

The udev rule must still be installed once (`scripts/install-udev-rule.sh`
from the source, or copy `data/70-bt-phone-recorder.rules` to
`/etc/udev/rules.d/`). The "kernel driver" adapter mode is not available
from an AppImage, because its helper needs a file capability that an AppImage
cannot carry; direct USB works.

## 9. Cleaning

Pick the level you need:

| Goal                                        | Command                          |
|---------------------------------------------|----------------------------------|
| Remove compiled objects, keep configuration | `ninja -C build clean`           |
| Start completely fresh (recommended before a build someone will run) | `rm -rf build build-release build-appimage dist` |
| Reset the BTstack submodule to the pinned release | `git submodule update --init --force third_party/btstack` |

After `rm -rf build`, configure again with `meson setup build` (step 3).

### Full clean rebuild from scratch

```sh
cd bluetooth-phone-recorder
rm -rf build build-release build-appimage dist
git submodule update --init --recursive
meson setup build
ninja -C build
meson test -C build --print-errorlogs
```

### Files the app creates outside the project

These are not removed by cleaning the build. Delete them only if you want
to reset the app itself:

| Path                                    | Contents                          |
|-----------------------------------------|-----------------------------------|
| `~/.config/bt-phone-recorder/`          | settings, remembered phone names  |
| `~/.local/share/bt-phone-recorder/`     | pairing keys (phones must pair again if deleted), downloaded address books, favorites, call queue and call log |
| `~/.local/state/bt-phone-recorder/`     | log file and Bluetooth traces     |
| `~/.cache/bt-phone-recorder/`           | unpacked Bluetooth firmware (recreated automatically) |

Your recordings (by default `~/Music/Podcast recordings/`) are never touched.

## Troubleshooting the build

- **`Dependency "..." not found`**: install the package from step 1.
- **`patch` failed**: BTstack was updated and a file in `patches/` no longer
  fits. Update the patch, or go back to the pinned release with
  `git submodule update --init --force third_party/btstack`.
- **`third_party/btstack` is empty**: run `git submodule update --init --recursive`.
- **The adapter is left without a driver** (rare, e.g. after a power cut while
  the app ran): `./build/src/ui/bt-phone-recorder --release-adapters`.
