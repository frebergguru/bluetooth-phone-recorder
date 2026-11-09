/*
 * HCI "user channel" transport for BTstack.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The kernel driver keeps handling USB and firmware loading (needed for
 * chips like MediaTek or Intel AX210), but hands the raw HCI channel to us
 * exclusively. bluetoothd and the BlueZ D-Bus API are not involved at all.
 *
 * Opening the channel needs CAP_NET_ADMIN, so a tiny helper
 * (bpr-userchan-helper) opens it and passes the socket to the app.
 */
#ifndef BPR_HCI_USER_CHANNEL_H
#define BPR_HCI_USER_CHANNEL_H

#include <stddef.h>

#include "hci_transport.h"

/* Opens the channel via the helper. Returns the socket fd or -1 (err filled). */
int hci_user_channel_open_socket(int hci_index, char *err, size_t err_len);

/* The transport uses the socket given here (call before hci_init). */
void hci_user_channel_set_socket(int fd);
const hci_transport_t *hci_user_channel_instance(void);

#endif
