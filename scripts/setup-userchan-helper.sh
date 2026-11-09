#!/bin/bash
# Gives bpr-userchan-helper the single capability it needs (CAP_NET_ADMIN) to
# open the kernel's raw HCI user channel. Only needed for the "Kernel driver"
# adapter mode (MediaTek, Intel AX210 and newer, Qualcomm and similar chips).
# Usage: scripts/setup-userchan-helper.sh [path/to/bpr-userchan-helper]
set -euo pipefail
helper="${1:-$(cd "$(dirname "$0")/.." && pwd)/build/tools/bpr-userchan-helper}"
[ -x "$helper" ] || { echo "helper not found: $helper" >&2; exit 1; }
sudo setcap cap_net_admin+ep "$helper"
getcap "$helper"
