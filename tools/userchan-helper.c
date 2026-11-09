/*
 * bpr-userchan-helper: opens the HCI user channel for one adapter and passes
 * the socket to Bluetooth Phone Recorder. Needs CAP_NET_ADMIN:
 *     sudo setcap cap_net_admin+ep bpr-userchan-helper
 * It does nothing else: takes an adapter index, brings that adapter down,
 * binds the user channel and sends the descriptor over the given socket.
 *
 * Kernel ABI constants are defined here so no BlueZ headers are needed.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define BPR_AF_BLUETOOTH 31
#define BPR_BTPROTO_HCI 1
#define BPR_HCI_CHANNEL_USER 1
#define BPR_HCIDEVDOWN _IOW('H', 202, int)

struct bpr_sockaddr_hci {
    sa_family_t hci_family;
    unsigned short hci_dev;
    unsigned short hci_channel;
};

static void reply_error(int out, const char *fmt, int idx, int e)
{
    char msg[200];
    int n = snprintf(msg, sizeof msg, fmt, idx, strerror(e));
    if (n > 0) {
        ssize_t w = write(out, msg, (size_t)n);
        (void)w;
    }
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <hci-index> <socket-fd>\n", argv[0]);
        return 2;
    }
    char *end = NULL;
    long idx = strtol(argv[1], &end, 10);
    if (!end || *end || idx < 0 || idx > 64) return 2;
    long out = strtol(argv[2], &end, 10);
    if (!end || *end || out < 3 || out > 1024) return 2;

    int ctl = socket(BPR_AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BPR_BTPROTO_HCI);
    if (ctl < 0) {
        reply_error((int)out, "hci%d: no Bluetooth socket support: %s", (int)idx, errno);
        return 1;
    }

    /* bluetoothd may power the adapter up again right away: retry a few times */
    int fd = -1, last_err = 0;
    for (int attempt = 0; attempt < 10 && fd < 0; attempt++) {
        if (ioctl(ctl, BPR_HCIDEVDOWN, (int)idx) < 0 && errno != EALREADY) last_err = errno;
        int s = socket(BPR_AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, BPR_BTPROTO_HCI);
        if (s < 0) {
            last_err = errno;
            break;
        }
        struct bpr_sockaddr_hci addr = {0};
        addr.hci_family = BPR_AF_BLUETOOTH;
        addr.hci_dev = (unsigned short)idx;
        addr.hci_channel = BPR_HCI_CHANNEL_USER;
        if (bind(s, (struct sockaddr *)&addr, sizeof addr) == 0) {
            fd = s;
            break;
        }
        last_err = errno;
        close(s);
        struct timespec ts = {0, 100 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    close(ctl);
    if (fd < 0) {
        reply_error((int)out, "hci%d: cannot open user channel: %s", (int)idx, last_err);
        return 1;
    }

    char ok = 'k';
    struct iovec iov = {&ok, 1};
    char cbuf[CMSG_SPACE(sizeof(int))];
    memset(cbuf, 0, sizeof cbuf);
    struct msghdr msg = {0};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cbuf;
    msg.msg_controllen = sizeof cbuf;
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof fd);
    if (sendmsg((int)out, &msg, 0) < 0) return 1;
    return 0;
}
