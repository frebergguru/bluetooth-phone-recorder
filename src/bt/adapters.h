/*
 * Bluetooth adapter discovery.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Finds USB Bluetooth controllers without opening them (descriptors via
 * libusb, names and driver binding via sysfs), so listing never disturbs an
 * adapter the system is using.
 */
#ifndef BPR_ADAPTERS_H
#define BPR_ADAPTERS_H

#include <stdbool.h>
#include <stdint.h>

#define BT_MAX_ADAPTERS 16
#define BT_MAX_PORTS 7

typedef enum {
    BT_TRANSPORT_USB,      /* app drives the USB device directly (libusb) */
    BT_TRANSPORT_USERCHAN, /* kernel driver loads firmware, app gets raw HCI */
    BT_TRANSPORT_UART,     /* serial adapter, H4 or H5 */
} bt_transport_t;

typedef enum {
    BT_CHIP_GENERIC,
    BT_CHIP_REALTEK,
    BT_CHIP_INTEL,
    BT_CHIP_BROADCOM,
    BT_CHIP_CSR,
    BT_CHIP_MEDIATEK,
    BT_CHIP_QUALCOMM,
    BT_CHIP_MARVELL,
} bt_chip_family_t;

typedef struct {
    bt_transport_t transport;
    bt_chip_family_t family;
    uint16_t vid, pid;
    uint8_t bus;
    uint8_t ports[BT_MAX_PORTS];
    int n_ports;
    char sysfs_name[64];  /* e.g. "1-4.2" */
    char name[160];       /* human readable */
    char driver[32];      /* kernel driver bound to interface 0, "" if none */
    int hci_index;        /* hciN while the kernel owns it, else -1 */
    bool usb_supported;   /* the app can load this chip's firmware itself */
    /* UART only */
    char uart_path[256];
    uint32_t uart_baud;
    bool uart_h5;
    bool uart_flow_control;
} bt_adapter_t;

/* Returns the number of adapters found. */
int bt_adapters_scan(bt_adapter_t *out, int max);

const char *bt_chip_family_name(bt_chip_family_t f);
const char *bt_transport_name(bt_transport_t t);

/* Recommended transport for an adapter (USB if the app can load firmware,
 * otherwise the kernel user channel). */
bt_transport_t bt_adapter_recommended_transport(const bt_adapter_t *a);

/* "usb:1-4.2", "userchan:0", "uart:/dev/ttyUSB0" style identifiers used in
 * the settings file and on the command line. */
void bt_adapter_to_id(const bt_adapter_t *a, char *buf, int len);
/* Fill a from an id; USB ids are matched against a fresh scan. */
bool bt_adapter_from_id(const char *id, bt_adapter_t *a);

#endif
