#!/bin/bash
# Installs the udev rule that lets the logged-in user drive USB Bluetooth
# adapters directly, so Phone Recorder needs no root rights.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
sudo install -m 0644 "$here/data/70-bt-phone-recorder.rules" /etc/udev/rules.d/70-bt-phone-recorder.rules
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=usb --action=change
echo "Done. Unplug and replug USB dongles if they still are not accessible."
