/*
 * USB guard: gives the Bluetooth adapter back to the system, always.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * When the app drives a USB adapter itself, the kernel driver (btusb) is
 * detached. A tiny helper process, forked at start-up before any threads
 * exist, re-attaches the kernel driver when the app releases the adapter OR
 * when the app dies in any way (crash, kill -9, power button logout), because
 * it notices the pipe from the app closing.
 */
#ifndef BPR_USB_GUARD_H
#define BPR_USB_GUARD_H

#include <stdint.h>

/* Fork the guard. Must be called before any thread is created. */
int usb_guard_start(void);
/* Tell the guard which device is about to be taken. */
void usb_guard_take(uint8_t bus, const uint8_t *ports, int n_ports);
/* Tell the guard to give it back now. */
void usb_guard_release(void);

/* Re-attach the kernel driver to every Bluetooth USB device that has none
 * (repair tool, --release-adapters). Returns the number fixed. */
int usb_guard_reattach_all(void);

#endif
