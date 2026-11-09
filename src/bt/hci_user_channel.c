/*
 * SPDX-License-Identifier: BSD-3-Clause
 */
#define BTSTACK_FILE__ "hci_user_channel.c"

#include "hci_user_channel.h"

#include <errno.h>
#include <limits.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "btstack_config.h"
#include "btstack_debug.h"
#include "btstack_run_loop.h"
#include "hci.h"

#ifndef BPR_LIBEXECDIR
#define BPR_LIBEXECDIR "/usr/local/libexec"
#endif

extern char **environ;

static void (*packet_handler)(uint8_t packet_type, uint8_t *packet, uint16_t size);
static btstack_data_source_t data_source;
static uint8_t packet_with_pre[HCI_INCOMING_PRE_BUFFER_SIZE + HCI_INCOMING_PACKET_BUFFER_SIZE + 1];
static uint8_t *packet = &packet_with_pre[HCI_INCOMING_PRE_BUFFER_SIZE];
static int sock = -1;

static int find_helper(char *out, size_t len)
{
    /* 1) next to the executable (build tree), 2) installed libexec dir */
    char self[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n > 0) {
        self[n] = 0;
        char *slash = strrchr(self, '/');
        if (slash) {
            *slash = 0;
            snprintf(out, len, "%s/bpr-userchan-helper", self);
            if (access(out, X_OK) == 0) return 0;
        }
    }
    snprintf(out, len, "%s/bpr-userchan-helper", BPR_LIBEXECDIR);
    return access(out, X_OK) == 0 ? 0 : -1;
}

int hci_user_channel_open_socket(int hci_index, char *err, size_t err_len)
{
    char helper[PATH_MAX];
    if (find_helper(helper, sizeof helper) != 0) {
        snprintf(err, err_len, "bpr-userchan-helper not found");
        return -1;
    }
    int sp[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sp) != 0) {
        snprintf(err, err_len, "socketpair: %s", strerror(errno));
        return -1;
    }
    char idx[16], fdarg[16];
    snprintf(idx, sizeof idx, "%d", hci_index);
    snprintf(fdarg, sizeof fdarg, "%d", 3);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, sp[1], 3); /* dup2 clears CLOEXEC */
    char *argv[] = {helper, idx, fdarg, NULL};
    pid_t pid;
    int rc = posix_spawn(&pid, helper, &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(sp[1]);
    if (rc != 0) {
        close(sp[0]);
        snprintf(err, err_len, "cannot run helper: %s", strerror(rc));
        return -1;
    }

    /* receive the fd, or a text error */
    char buf[256] = {0};
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct iovec iov = {buf, sizeof buf - 1};
    struct msghdr msg = {0};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cbuf;
    msg.msg_controllen = sizeof cbuf;
    ssize_t r = recvmsg(sp[0], &msg, MSG_CMSG_CLOEXEC);
    close(sp[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    int fd = -1;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS)
            memcpy(&fd, CMSG_DATA(c), sizeof fd);
    }
    if (fd < 0) {
        snprintf(err, err_len, "%s", r > 0 ? buf : "helper failed (is it installed with setcap?)");
        return -1;
    }
    return fd;
}

void hci_user_channel_set_socket(int fd)
{
    sock = fd;
}

static void process(btstack_data_source_t *ds, btstack_data_source_callback_type_t type)
{
    (void)ds;
    if (type != DATA_SOURCE_CALLBACK_READ) return;
    ssize_t n = read(sock, packet, HCI_INCOMING_PACKET_BUFFER_SIZE + 1);
    if (n <= 1) {
        if (n < 0 && errno != EAGAIN) log_error("user channel read: %s", strerror(errno));
        return;
    }
    packet_handler(packet[0], &packet[1], (uint16_t)(n - 1));
}

static void transport_init(const void *config)
{
    (void)config;
}

static int transport_open(void)
{
    if (sock < 0) return -1;
    btstack_run_loop_set_data_source_fd(&data_source, sock);
    btstack_run_loop_set_data_source_handler(&data_source, &process);
    btstack_run_loop_add_data_source(&data_source);
    btstack_run_loop_enable_data_source_callbacks(&data_source, DATA_SOURCE_CALLBACK_READ);
    return 0;
}

static int transport_close(void)
{
    btstack_run_loop_remove_data_source(&data_source);
    if (sock >= 0) close(sock); /* the kernel gives the adapter back to the system */
    sock = -1;
    return 0;
}

static void register_handler(void (*handler)(uint8_t, uint8_t *, uint16_t))
{
    packet_handler = handler;
}

static int send_packet(uint8_t packet_type, uint8_t *pkt, int size)
{
    /* BTstack reserves a pre-buffer byte in front of outgoing packets */
    uint8_t *buf = pkt - 1;
    buf[0] = packet_type;
    ssize_t w = write(sock, buf, (size_t)size + 1);
    if (w < 0) {
        log_error("user channel write: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static const hci_transport_t transport = {
    .name = "HCI user channel",
    .init = &transport_init,
    .open = &transport_open,
    .close = &transport_close,
    .register_packet_handler = &register_handler,
    .send_packet = &send_packet,
};

const hci_transport_t *hci_user_channel_instance(void)
{
    return &transport;
}
