/*
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "adapters.h"

#include <dirent.h>
#include <libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "btstack_chipset_realtek.h"

/* Adapters that use a vendor-specific USB class instead of E0/01/01. */
static const struct {
    uint16_t vid, pid;
} vendor_class_devices[] = {
    {0x0a5c, 0x21e8}, /* Broadcom BCM20702A0, e.g. DeLOCK / Plugable */
    {0x0a5c, 0x21e6}, /* Broadcom BCM20702 */
    {0x0a5c, 0x21ec}, /* Broadcom BCM20702A1 */
    {0x0a5c, 0x640b}, /* Broadcom BCM20703 */
    {0x0b05, 0x17cb}, /* ASUS BT400 (BCM20702) */
    {0x04b4, 0xf901}, /* Cypress CYW20704A2 / Laird BT860 */
    {0x050d, 0x065a}, /* Belkin F8T065BF (BCM20702) */
    {0x0a12, 0x0001}, /* CSR8510 (normally E0/01/01, listed for safety) */
};

/* Realtek chips sold under another brand's USB ID. BTstack's firmware table
 * only knows Realtek's own IDs (vendor 0x0bda), so these are mapped to the
 * Realtek product ID of the same chip. IDs as in the Linux btusb driver. */
static const struct {
    uint16_t vid, pid, rtk_pid;
    const char *name;
} realtek_aliases[] = {
    {0x2357, 0x0604, 0x8771, "TP-Link UB500 (RTL8761BU)"},
    {0x0b05, 0x190e, 0x8771, "ASUS USB-BT500 (RTL8761BU)"},
    {0x2550, 0x8761, 0x8771, "RTL8761BU dongle"},
    {0x2b89, 0x8761, 0x8771, "UGREEN RTL8761BU dongle"},
};

static uint16_t realtek_alias_pid(uint16_t vid, uint16_t pid)
{
    for (size_t i = 0; i < sizeof realtek_aliases / sizeof realtek_aliases[0]; i++)
        if (realtek_aliases[i].vid == vid && realtek_aliases[i].pid == pid)
            return realtek_aliases[i].rtk_pid;
    return 0;
}

static bool intel_uses_tlv(uint16_t pid)
{
    switch (pid) {
    case 0x0032: /* AX210 */
    case 0x0033: /* AX211 / AX411 */
    case 0x0035: /* BE200 */
    case 0x0036: /* BE201 */
    case 0x0037:
    case 0x0038:
    case 0x0039:
        return true;
    default:
        return false;
    }
}

static bt_chip_family_t family_for(uint16_t vid, uint16_t pid)
{
    if (realtek_alias_pid(vid, pid)) return BT_CHIP_REALTEK;
    /* Realtek chips are sold under many vendor IDs: trust BTstack's table. */
    uint16_t n = btstack_chipset_realtek_get_num_usb_controllers();
    for (uint16_t i = 0; i < n; i++) {
        uint16_t v, p;
        btstack_chipset_realtek_get_vendor_product_id(i, &v, &p);
        if (v == vid && p == pid) return BT_CHIP_REALTEK;
    }
    switch (vid) {
    case 0x0bda: return BT_CHIP_REALTEK;
    case 0x8087: return BT_CHIP_INTEL;
    case 0x0a5c:
    case 0x04b4:
    case 0x050d:
    case 0x0b05: return BT_CHIP_BROADCOM;
    case 0x0a12: return BT_CHIP_CSR;
    case 0x0e8d: return BT_CHIP_MEDIATEK;
    case 0x0cf3: return BT_CHIP_QUALCOMM;
    case 0x1286: return BT_CHIP_MARVELL;
    default: break;
    }
    (void)pid;
    return BT_CHIP_GENERIC;
}

const char *bt_chip_family_name(bt_chip_family_t f)
{
    switch (f) {
    case BT_CHIP_REALTEK: return "Realtek";
    case BT_CHIP_INTEL: return "Intel";
    case BT_CHIP_BROADCOM: return "Broadcom/Cypress";
    case BT_CHIP_CSR: return "CSR/Qualcomm CSR";
    case BT_CHIP_MEDIATEK: return "MediaTek";
    case BT_CHIP_QUALCOMM: return "Qualcomm Atheros";
    case BT_CHIP_MARVELL: return "Marvell";
    case BT_CHIP_GENERIC:
    default: return "Standard";
    }
}

const char *bt_transport_name(bt_transport_t t)
{
    switch (t) {
    case BT_TRANSPORT_USB: return "usb";
    case BT_TRANSPORT_USERCHAN: return "userchan";
    case BT_TRANSPORT_UART: return "uart";
    default: return "?";
    }
}

bt_transport_t bt_adapter_recommended_transport(const bt_adapter_t *a)
{
    if (a->transport == BT_TRANSPORT_UART) return BT_TRANSPORT_UART;
    if (a->usb_supported) return BT_TRANSPORT_USB;
    if (a->hci_index >= 0) return BT_TRANSPORT_USERCHAN;
    return BT_TRANSPORT_USB;
}

static bool read_line(const char *path, char *buf, size_t len)
{
    FILE *f = fopen(path, "r");
    if (!f) return false;
    bool ok = fgets(buf, (int)len, f) != NULL;
    fclose(f);
    if (ok) buf[strcspn(buf, "\r\n")] = 0;
    return ok;
}

static void sysfs_info(bt_adapter_t *a)
{
    char path[512], buf[256];
    char manufacturer[128] = "", product[128] = "";
    snprintf(path, sizeof path, "/sys/bus/usb/devices/%s/manufacturer", a->sysfs_name);
    if (read_line(path, buf, sizeof buf)) snprintf(manufacturer, sizeof manufacturer, "%s", buf);
    snprintf(path, sizeof path, "/sys/bus/usb/devices/%s/product", a->sysfs_name);
    if (read_line(path, buf, sizeof buf)) snprintf(product, sizeof product, "%s", buf);
    if (*product && *manufacturer && !strstr(product, manufacturer))
        snprintf(a->name, sizeof a->name, "%s %s", manufacturer, product);
    else if (*product)
        snprintf(a->name, sizeof a->name, "%s", product);
    else
        snprintf(a->name, sizeof a->name, "%s Bluetooth adapter", bt_chip_family_name(a->family));

    /* driver bound to interface 0 */
    a->driver[0] = 0;
    snprintf(path, sizeof path, "/sys/bus/usb/devices/%s:1.0/driver", a->sysfs_name);
    char link[512];
    ssize_t n = readlink(path, link, sizeof link - 1);
    if (n > 0) {
        link[n] = 0;
        const char *base = strrchr(link, '/');
        snprintf(a->driver, sizeof a->driver, "%s", base ? base + 1 : link);
    }

    /* hciN registered by the kernel for this device */
    a->hci_index = -1;
    snprintf(path, sizeof path, "/sys/bus/usb/devices/%s:1.0/bluetooth", a->sysfs_name);
    DIR *d = opendir(path);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            if (strncmp(de->d_name, "hci", 3) == 0) {
                a->hci_index = atoi(de->d_name + 3);
                break;
            }
        }
        closedir(d);
    }
}

static bool is_bluetooth(libusb_device *dev, const struct libusb_device_descriptor *dd)
{
    if (dd->bDeviceClass == 0xE0 && dd->bDeviceSubClass == 0x01 && dd->bDeviceProtocol == 0x01)
        return true;
    for (size_t i = 0; i < sizeof vendor_class_devices / sizeof vendor_class_devices[0]; i++)
        if (vendor_class_devices[i].vid == dd->idVendor &&
            vendor_class_devices[i].pid == dd->idProduct)
            return true;
    if (realtek_alias_pid(dd->idVendor, dd->idProduct)) return true;
    /* Composite devices: look at interface 0 */
    struct libusb_config_descriptor *cfg = NULL;
    if (libusb_get_active_config_descriptor(dev, &cfg) != 0 &&
        libusb_get_config_descriptor(dev, 0, &cfg) != 0)
        return false;
    bool bt = false;
    if (cfg->bNumInterfaces > 0 && cfg->interface[0].num_altsetting > 0) {
        const struct libusb_interface_descriptor *id = &cfg->interface[0].altsetting[0];
        bt = id->bInterfaceClass == 0xE0 && id->bInterfaceSubClass == 0x01 &&
             id->bInterfaceProtocol == 0x01;
    }
    libusb_free_config_descriptor(cfg);
    return bt;
}

int bt_adapters_scan(bt_adapter_t *out, int max)
{
    libusb_context *ctx = NULL;
    if (libusb_init(&ctx) != 0) return 0;
    libusb_device **list = NULL;
    ssize_t cnt = libusb_get_device_list(ctx, &list);
    int n = 0;
    for (ssize_t i = 0; i < cnt && n < max; i++) {
        struct libusb_device_descriptor dd;
        if (libusb_get_device_descriptor(list[i], &dd) != 0) continue;
        if (!is_bluetooth(list[i], &dd)) continue;
        bt_adapter_t *a = &out[n];
        memset(a, 0, sizeof *a);
        a->transport = BT_TRANSPORT_USB;
        a->vid = dd.idVendor;
        a->pid = dd.idProduct;
        a->family = family_for(a->vid, a->pid);
        a->rtk_pid = realtek_alias_pid(a->vid, a->pid);
        if (!a->rtk_pid && a->vid == 0x0bda) a->rtk_pid = a->pid;
        a->bus = libusb_get_bus_number(list[i]);
        a->n_ports = libusb_get_port_numbers(list[i], a->ports, BT_MAX_PORTS);
        if (a->n_ports < 0) a->n_ports = 0;
        int pos = snprintf(a->sysfs_name, sizeof a->sysfs_name, "%u-", a->bus);
        for (int p = 0; p < a->n_ports && pos < (int)sizeof a->sysfs_name; p++)
            pos += snprintf(a->sysfs_name + pos, sizeof a->sysfs_name - (size_t)pos,
                            p ? ".%u" : "%u", a->ports[p]);
        switch (a->family) {
        case BT_CHIP_REALTEK:
            /* BTstack knows the firmware files for the chips in its table */
            a->usb_supported = true;
            break;
        case BT_CHIP_INTEL:
            /* BTstack loads firmware for 8260 .. AX200/AX201 ("legacy" boot
             * loader). AX210 and newer use the TLV boot loader, which only
             * the kernel driver handles. */
            a->usb_supported = !intel_uses_tlv(a->pid);
            break;
        case BT_CHIP_BROADCOM:
        case BT_CHIP_CSR:
        case BT_CHIP_GENERIC:
            a->usb_supported = true;
            break;
        case BT_CHIP_MEDIATEK:
        case BT_CHIP_QUALCOMM:
        case BT_CHIP_MARVELL:
        default:
            /* firmware loading only in the kernel: use the user channel */
            a->usb_supported = false;
            break;
        }
        sysfs_info(a);
        a->transport = bt_adapter_recommended_transport(a);
        n++;
    }
    libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    return n;
}

void bt_adapter_to_id(const bt_adapter_t *a, char *buf, int len)
{
    switch (a->transport) {
    case BT_TRANSPORT_UART:
        snprintf(buf, (size_t)len, "uart:%s:%u:%s%s", a->uart_path, a->uart_baud,
                 a->uart_h5 ? "h5" : "h4", a->uart_flow_control ? ":flow" : "");
        break;
    case BT_TRANSPORT_USERCHAN:
        snprintf(buf, (size_t)len, "userchan:%s", a->sysfs_name);
        break;
    case BT_TRANSPORT_USB:
    default:
        snprintf(buf, (size_t)len, "usb:%s", a->sysfs_name);
        break;
    }
}

bool bt_adapter_from_id(const char *id, bt_adapter_t *a)
{
    memset(a, 0, sizeof *a);
    a->hci_index = -1;
    if (strncmp(id, "uart:", 5) == 0) {
        /* uart:/dev/ttyUSB0:115200:h4[:flow] */
        char tmp[300];
        snprintf(tmp, sizeof tmp, "%s", id + 5);
        char *save = NULL;
        char *path = strtok_r(tmp, ":", &save);
        char *baud = strtok_r(NULL, ":", &save);
        char *proto = strtok_r(NULL, ":", &save);
        char *flow = strtok_r(NULL, ":", &save);
        if (!path) return false;
        a->transport = BT_TRANSPORT_UART;
        snprintf(a->uart_path, sizeof a->uart_path, "%s", path);
        a->uart_baud = baud ? (uint32_t)strtoul(baud, NULL, 10) : 115200;
        a->uart_h5 = proto && strcmp(proto, "h5") == 0;
        a->uart_flow_control = flow && strcmp(flow, "flow") == 0;
        snprintf(a->name, sizeof a->name, "UART %s", a->uart_path);
        return true;
    }
    bt_transport_t want;
    const char *sys;
    if (strncmp(id, "usb:", 4) == 0) {
        want = BT_TRANSPORT_USB;
        sys = id + 4;
    } else if (strncmp(id, "userchan:", 9) == 0) {
        want = BT_TRANSPORT_USERCHAN;
        sys = id + 9;
    } else {
        return false;
    }
    bt_adapter_t list[BT_MAX_ADAPTERS];
    int n = bt_adapters_scan(list, BT_MAX_ADAPTERS);
    for (int i = 0; i < n; i++) {
        if (strcmp(list[i].sysfs_name, sys) == 0) {
            *a = list[i];
            a->transport = want;
            return true;
        }
    }
    return false;
}
