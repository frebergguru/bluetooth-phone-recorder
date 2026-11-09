/*
 * Firmware cache. Distributions ship Bluetooth firmware in /lib/firmware,
 * often compressed (.zst or .xz). BTstack reads plain files, so the files a
 * chip family needs are unpacked once into ~/.cache/bt-phone-recorder/firmware.
 * The app never ships firmware itself.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef BPR_FIRMWARE_H
#define BPR_FIRMWARE_H

#include "adapters.h"

/* Returns the folder holding usable firmware for the family (static
 * string), or NULL if the family needs none. override_dir, if set, is used
 * as an extra search location before the system folders. */
const char *bt_firmware_prepare(bt_chip_family_t family, const char *override_dir);

/* Unpack one compressed or plain file to dst. Returns 0 on success.
 * Exposed for tests. */
int bt_firmware_unpack_file(const char *src, const char *dst);

#endif
