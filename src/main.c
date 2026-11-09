/*
 * Bluetooth Phone Recorder: entry point.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <getopt.h>
#include <glib/gi18n.h>
#include <locale.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bt/adapters.h"
#include "bt/usb_guard.h"
#include "config.h"
#include "core/log.h"
#include "core/paths.h"

int app_run(int argc, char **argv, const char *cli_adapter, bool cli_trace);

static void list_adapters(void)
{
    bt_adapter_t list[BT_MAX_ADAPTERS];
    int n = bt_adapters_scan(list, BT_MAX_ADAPTERS);
    if (n == 0) {
        printf("%s\n", _("No USB Bluetooth adapters found."));
        return;
    }
    for (int i = 0; i < n; i++) {
        char id[320];
        bt_adapter_to_id(&list[i], id, sizeof id);
        printf("%-22s %04x:%04x  %-18s driver=%-8s hci=%-3d %s\n", id, list[i].vid, list[i].pid,
               bt_chip_family_name(list[i].family), list[i].driver[0] ? list[i].driver : "-",
               list[i].hci_index, list[i].name);
    }
}

static void usage(const char *argv0)
{
    printf(_("Usage: %s [options]\n\n"
             "  --adapter=ID        use this adapter (see --list-adapters), for this run only\n"
             "  --list-adapters     list Bluetooth adapters and exit\n"
             "  --release-adapters  give adapters left without a driver back to the system\n"
             "  --trace             save a Bluetooth trace (PacketLogger format)\n"
             "  --verbose           more log output\n"
             "  --version           show the version\n"),
           argv0);
}

int main(int argc, char **argv)
{
    /* The USB guard must be forked before any thread exists. */
    usb_guard_start();
    /* A write to the guard's pipe after the guard died must report EPIPE,
     * not kill the app (and with it a running recording). */
    signal(SIGPIPE, SIG_IGN);

    setlocale(LC_ALL, "");
    bindtextdomain(GETTEXT_PACKAGE, BPR_LOCALEDIR);
    bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
    textdomain(GETTEXT_PACKAGE);

    static struct option opts[] = {
        {"adapter", required_argument, NULL, 'a'},
        {"list-adapters", no_argument, NULL, 'l'},
        {"release-adapters", no_argument, NULL, 'r'},
        {"trace", no_argument, NULL, 't'},
        {"verbose", no_argument, NULL, 'v'},
        {"version", no_argument, NULL, 'V'},
        {"help", no_argument, NULL, 'h'},
        {0, 0, 0, 0},
    };
    const char *adapter = NULL;
    bool trace = false;
    int c;
    opterr = 0;
    while ((c = getopt_long(argc, argv, "", opts, NULL)) != -1) {
        switch (c) {
        case 'a': adapter = optarg; break;
        case 'l': list_adapters(); return 0;
        case 'r': {
            int n = usb_guard_reattach_all();
            printf(_("%d adapter(s) given back to the system.\n"), n);
            return 0;
        }
        case 't': trace = true; break;
        case 'v': log_set_verbose(1); break;
        case 'V': printf("bt-phone-recorder %s\n", BPR_VERSION); return 0;
        case 'h': usage(argv[0]); return 0;
        default: break; /* leave GTK options alone */
        }
    }

    paths_mkdirs(paths_state_dir());
    char log_path[1024];
    snprintf(log_path, sizeof log_path, "%s/phone-recorder.log", paths_state_dir());
    log_open(log_path);
    LOGI("app", "Bluetooth Phone Recorder %s starting", BPR_VERSION);

    /* GTK must not see our own options */
    char *gtk_argv[2] = {argv[0], NULL};
    int status = app_run(1, gtk_argv, adapter, trace);
    log_close();

    if (status == 100) {
        /* restart requested (adapter switch): start fresh, the guard has
         * already been told to release the old adapter */
        char *args[] = {argv[0], NULL};
        execv("/proc/self/exe", args);
        perror("restart failed");
        return 1;
    }
    return status;
}
