# Bluetooth Phone Recorder

Record phone calls over Bluetooth for podcasts. The computer connects to the
phone the way a car kit does (Bluetooth Hands-Free Profile), so any call on
the phone, including WhatsApp, Signal and other calling apps, comes through
the app. The caller, every microphone and the mix are recorded on separate,
sample-aligned tracks and are also available to OBS.

Written in C for Linux, with GTK 4 / libadwaita, PipeWire and its own
Bluetooth stack (BTstack). It does **not** use BlueZ or bluetoothd.

## What to expect about sound quality

Phone call audio over Bluetooth is limited by the Hands-Free Profile itself,
whatever the app records at:

| Codec   | Sample rate | When                                          |
|---------|-------------|-----------------------------------------------|
| CVSD    | 8 kHz       | old phones or adapters without eSCO           |
| mSBC    | 16 kHz      | "HD voice", most phones from the last decade  |
| LC3-SWB | 32 kHz      | newer phones with HFP 1.9                     |

The app picks the best codec that both the phone and the adapter support and
shows it during the call. Everything is recorded at 48 kHz, 32-bit float, so
nothing is lost after that point. Your own microphones are recorded at full
quality. The caller's own phone line (mobile network or VoIP) also limits
what reaches you.

## Features

- Own Bluetooth stack, so no BlueZ pairing or reconnection problems. Pairing
  with numeric comparison, remembers phones, reconnects automatically.
- Answer, decline, hang up, dial, redial, keypad tones, call waiting, caller
  ID, phone battery and signal. Move the call audio between phone and computer.
- Mixer with any number of inputs (microphones, other apps): gain, pan, mute,
  solo, low cut filter, noise gate, peak meters with numbers.
- **Mix-minus**: the caller hears all inputs except themselves.
- **"In call" switch per input**: decides only whether the caller hears that
  input. It never removes the input from OBS, the recording or the monitor.
- Monitor output to any headphones, optional echo cancelling for speakers.
- Multitrack recording: full mix (stereo) + caller + one track per input.
  WAV (32-bit float or 24-bit, RF64 above 4 GB), FLAC or Opus. Crash safe:
  headers are kept valid while recording.
- Markers (Ctrl+M or F9), saved as WAV cue points, an Audacity label file and
  a chapters file.
- Auto-record when a call starts, auto-stop when it ends.
- Free disk space and recording time left, dropout counter.
- OBS: see below. Optional obs-websocket control starts and stops OBS
  recording with the app and adds markers as OBS chapters.
- Accessible: screen reader labels, full keyboard control, text size setting,
  Norwegian Bokmål and English. See [ACCESSIBILITY.md](ACCESSIBILITY.md).

## OBS

While the app runs, these PipeWire sources always exist:

| Source                       | Content                                     |
|------------------------------|---------------------------------------------|
| Phone Recorder: Full mix     | stereo mix of everything                    |
| Phone Recorder: Caller       | the caller only                             |
| Phone Recorder: Mix-minus    | what the caller hears                       |
| Phone Recorder: *input name* | one per input, unaffected by mute and by "In call" |

In OBS add an **Audio Input Capture (PipeWire / PulseAudio)** source and
choose one. Put sources on separate OBS tracks to edit them separately.
The source names stay in English so OBS scenes keep working in any language.

## Bluetooth adapters

The app owns the adapter while it runs, so other Bluetooth devices on that
adapter disconnect until the app closes. **A separate USB dongle is
recommended.** The built-in adapter works too; the app asks before taking it
and gives it back on exit, also after a crash (a small guard process
re-attaches the kernel driver).

Two modes:

- **Direct USB** (default): the app drives the USB adapter itself with
  libusb, including firmware loading and call audio.
- **Kernel driver, raw access**: for chips whose firmware only the Linux
  kernel can load. The kernel driver loads the firmware, then hands the raw
  HCI channel to the app exclusively. bluetoothd is not involved. Needs the
  helper, see setup.

| Chip family                         | Mode        | Firmware          | Status          |
|-------------------------------------|-------------|-------------------|-----------------|
| CSR8510 (most cheap dongles)        | Direct USB  | none needed       | expected to work, not tested yet |
| Realtek RTL8761B/BU, 8821C, 8822, 8852A/B/C, 8851 | Direct USB | rtl_bt from linux-firmware | detection tested (RTL8852CE), call audio not tested yet |
| Broadcom/Cypress BCM20702, CYW20704 | Direct USB  | brcm .hcd files   | expected to work, not tested yet |
| Intel 8260, 8265, 9260, 9560, AX200, AX201 | Direct USB | intel ibt files | expected to work, not tested yet |
| Intel AX210 and newer               | Kernel driver | by the kernel   | control works; call audio depends on the kernel, not tested yet |
| MediaTek, Qualcomm, Marvell         | Kernel driver | by the kernel   | as above        |
| Other standard USB adapters         | Direct USB  | none              | tried as a standard controller |
| Serial (UART) modules, H4 or H5     | Serial      | none              | not tested yet  |

Firmware is read from the system's `/lib/firmware` and unpacked into
`~/.cache/bt-phone-recorder/firmware`. Nothing is shipped.

In kernel driver mode, call audio needs the kernel to switch the adapter's
audio channel, which not every kernel and chip does for raw access. If a call
connects but no audio arrives, the app says so; use Direct USB mode or another
adapter then.

If an adapter misbehaves, use **Menu, Save adapter report**, and turn on
**Preferences, Bluetooth, Save Bluetooth trace** to get a trace file
(PacketLogger format, opens in Wireshark).

## Building

Dependencies (Arch/Manjaro names): `gtk4 libadwaita pipewire libusb
libsndfile speexdsp zstd xz libsoup3 json-glib meson ninja`.

```sh
git clone --recurse-submodules <repo>
cd bluetooth-phone-recorder
meson setup build
ninja -C build
meson test -C build
```

`libsoup3` and `json-glib` are optional (`-Dobs_websocket=disabled`).

## Setup

1. Let your user drive USB Bluetooth adapters (once):
   `scripts/install-udev-rule.sh`
2. Only for the kernel driver mode: `scripts/setup-userchan-helper.sh`
   (gives the small helper program the single capability CAP_NET_ADMIN).
3. Start `build/src/ui/bt-phone-recorder`, choose the adapter, turn on
   pairing mode and pair from the phone's Bluetooth settings.

Command line: `--list-adapters`, `--adapter=usb:1-4.2`, `--trace`,
`--release-adapters` (gives adapters left without a driver back to the
system), `--verbose`.

## Files

- Settings: `~/.config/bt-phone-recorder/settings.ini`
- Pairings: `~/.local/share/bt-phone-recorder/`
- Log and traces: `~/.local/state/bt-phone-recorder/`
- Recordings: `~/Music/Podcast recordings/<date>_<time>/` (changeable)

## Releases

`scripts/release.sh` cleans the release build, runs the tests, installs into a
staging folder, refuses to continue if any shipped binary is not stripped
(`scripts/check-stripped.sh`), checks the version and writes
`dist/bt-phone-recorder-<version>-linux-<arch>.tar.xz`.

## Licence

Own code: BSD 3-Clause. BTstack, which the program includes, is free for
non-commercial use only, so the program as a whole is for non-commercial use.
See [NOTICE](NOTICE).
