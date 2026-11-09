/*
 * SPDX-License-Identifier: BSD-3-Clause
 */
#define _GNU_SOURCE
#include "usb_guard.h"

#include <dirent.h>
#include <fcntl.h>
#include <libusb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

#include "adapters.h"

static int guard_fd = -1;

static int reattach(uint8_t bus, const uint8_t *ports, int n_ports)
{
    int fixed = 0;
    /* The device may be re-enumerating after a reset: retry for ~3 s. */
    for (int attempt = 0; attempt < 30 && fixed == 0; attempt++) {
        libusb_context *ctx = NULL;
        if (libusb_init(&ctx) != 0) return 0;
        libusb_device **list = NULL;
        ssize_t cnt = libusb_get_device_list(ctx, &list);
        bool found = false;
        for (ssize_t i = 0; i < cnt; i++) {
            uint8_t p[BT_MAX_PORTS];
            int np = libusb_get_port_numbers(list[i], p, BT_MAX_PORTS);
            if (libusb_get_bus_number(list[i]) != bus || np != n_ports ||
                memcmp(p, ports, (size_t)np) != 0)
                continue;
            found = true;
            libusb_device_handle *h = NULL;
            if (libusb_open(list[i], &h) != 0) break;
            for (int iface = 0; iface < 2; iface++) {
                if (libusb_kernel_driver_active(h, iface) == 0 &&
                    libusb_attach_kernel_driver(h, iface) == 0)
                    fixed++;
            }
            if (libusb_kernel_driver_active(h, 0) == 1 && fixed == 0) fixed = 1; /* already fine */
            libusb_close(h);
            break;
        }
        libusb_free_device_list(list, 1);
        libusb_exit(ctx);
        if (fixed == 0) {
            (void)found;
            struct timespec ts = {0, 100 * 1000 * 1000};
            nanosleep(&ts, NULL);
        }
    }
    return fixed;
}

static void guard_main(int rfd)
{
    /* Survive the terminal's Ctrl+C and hang-ups: we must outlive the app. */
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    prctl(PR_SET_NAME, "bpr-usb-guard", 0, 0, 0);

    uint8_t bus = 0, ports[BT_MAX_PORTS];
    int n_ports = 0;
    bool taken = false;
    FILE *in = fdopen(rfd, "r");
    char line[256];
    while (in && fgets(line, sizeof line, in)) {
        if (strncmp(line, "TAKE ", 5) == 0) {
            unsigned b;
            char path[128];
            if (sscanf(line + 5, "%u %127s", &b, path) >= 1) {
                bus = (uint8_t)b;
                n_ports = 0;
                char *save = NULL;
                for (char *tok = strtok_r(path, ".", &save); tok && n_ports < BT_MAX_PORTS;
                     tok = strtok_r(NULL, ".", &save))
                    ports[n_ports++] = (uint8_t)atoi(tok);
                taken = true;
            }
        } else if (strncmp(line, "RELEASE", 7) == 0) {
            if (taken) reattach(bus, ports, n_ports);
            taken = false;
        }
    }
    /* Pipe closed: the app is gone. */
    if (taken) reattach(bus, ports, n_ports);
    _exit(0);
}

int usb_guard_start(void)
{
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        close(fds[1]);
        /* close everything else we inherited */
        for (int fd = 3; fd < 1024; fd++)
            if (fd != fds[0]) close(fd);
        guard_main(fds[0]);
    }
    close(fds[0]);
    guard_fd = fds[1];
    return 0;
}

static void guard_send(const char *msg)
{
    if (guard_fd < 0) return;
    size_t len = strlen(msg);
    ssize_t w = write(guard_fd, msg, len);
    (void)w;
}

void usb_guard_take(uint8_t bus, const uint8_t *ports, int n_ports)
{
    char msg[128];
    int pos = snprintf(msg, sizeof msg, "TAKE %u ", bus);
    for (int i = 0; i < n_ports && pos < (int)sizeof msg - 8; i++)
        pos += snprintf(msg + pos, sizeof msg - (size_t)pos, i ? ".%u" : "%u", ports[i]);
    snprintf(msg + pos, sizeof msg - (size_t)pos, "\n");
    guard_send(msg);
}

void usb_guard_release(void)
{
    guard_send("RELEASE\n");
}

int usb_guard_reattach_all(void)
{
    bt_adapter_t list[BT_MAX_ADAPTERS];
    int n = bt_adapters_scan(list, BT_MAX_ADAPTERS);
    int fixed = 0;
    for (int i = 0; i < n; i++) {
        if (list[i].driver[0]) continue;
        if (reattach(list[i].bus, list[i].ports, list[i].n_ports) > 0) fixed++;
    }
    return fixed;
}
