/*
 * Bluetooth engine: runs BTstack in its own thread and acts as a Hands-Free
 * unit (like a car kit) towards one phone.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Thread model: BTstack is single threaded. Commands from the UI are queued
 * and executed on the BT thread. Events are delivered through a callback ON
 * THE BT THREAD; the UI must copy them and hand them to its own thread.
 */
#ifndef BPR_BT_ENGINE_H
#define BPR_BT_ENGINE_H

#include <stdbool.h>
#include <stdint.h>

#include "../audio/phone_link.h"
#include "adapters.h"

typedef enum {
    BT_STATE_OFF,
    BT_STATE_STARTING,
    BT_STATE_READY, /* adapter running, no phone */
    BT_STATE_FAILED,
} bt_state_t;

typedef enum {
    BT_EV_STATE,            /* state, text */
    BT_EV_CONTROLLER_INFO,  /* text: multi-line adapter report */
    BT_EV_PAIR_CONFIRM,     /* addr, number: show "does the phone show <number>?" */
    BT_EV_PIN_REQUEST,      /* addr: legacy pairing, ask for a PIN */
    BT_EV_PAIRED,           /* addr, status */
    BT_EV_REMOTE_NAME,      /* addr, text */
    BT_EV_PHONE_CONNECTED,  /* addr */
    BT_EV_PHONE_DISCONNECTED,
    BT_EV_INDICATOR,        /* text = indicator name, number = value */
    BT_EV_RING,
    BT_EV_CALLER_ID,        /* text = number, text2 = name */
    BT_EV_CALL_STATUS,      /* from +CLCC: number=index, value2=status, text=number */
    BT_EV_AUDIO_CONNECTED,  /* number = sample rate, text = codec name */
    BT_EV_AUDIO_DISCONNECTED,
    BT_EV_AUDIO_SILENT,     /* audio link up but no packets arrive (adapter problem) */
    BT_EV_OPERATOR,         /* text */
    BT_EV_SPEAKER_GAIN,     /* number 0..15 */
    BT_EV_PAIRED_LIST,      /* text = newline separated addresses */
    BT_EV_ERROR,            /* text */
} bt_event_type_t;

typedef struct {
    bt_event_type_t type;
    int state;
    char addr[18];
    int32_t number;
    int32_t value2;
    char text[512];
    char text2[128];
} bt_event_t;

typedef void (*bt_event_cb)(const bt_event_t *ev, void *ud);

typedef struct {
    bt_adapter_t adapter;
    const char *local_name;      /* shown on the phone */
    const char *firmware_dir;    /* extra firmware folder or NULL */
    const char *trace_path;      /* HCI trace (Wireshark/PacketLogger) or NULL */
    bool secure_connections;     /* turn off for old/buggy controllers */
    bool allow_super_wideband;   /* LC3-SWB 32 kHz */
    bool allow_wideband;         /* mSBC 16 kHz */
    bool disable_phone_nrec;     /* ask the phone to skip its echo/noise processing */
    char auto_connect_addr[18];  /* last phone, reconnect automatically */
    phone_link_t *link;
} bt_config_t;

/* Starts the BT thread. Only one start per process (BTstack is a singleton);
 * to switch adapters the app restarts itself. */
int bt_engine_start(const bt_config_t *cfg, bt_event_cb cb, void *ud);
/* Powers the controller off, closes the transport, gives the adapter back. */
void bt_engine_stop(void);

/* Commands (any thread). addr strings are "AA:BB:CC:DD:EE:FF". */
void bt_cmd_connect(const char *addr);
void bt_cmd_disconnect(void);
void bt_cmd_set_discoverable(bool on);
void bt_cmd_pair_reply(const char *addr, bool accept);
void bt_cmd_pin_reply(const char *addr, const char *pin); /* pin NULL = cancel */
void bt_cmd_forget(const char *addr);
void bt_cmd_list_paired(void);
void bt_cmd_answer(void);
void bt_cmd_reject(void);
void bt_cmd_hangup(void);
void bt_cmd_dial(const char *number);
void bt_cmd_redial(void);
void bt_cmd_dtmf(char digit);
void bt_cmd_audio_to_computer(void);
void bt_cmd_audio_to_phone(void);
void bt_cmd_hold_and_accept(void); /* AT+CHLD=2: hold active, accept waiting */
void bt_cmd_end_and_accept(void);  /* AT+CHLD=1 */
void bt_cmd_query_calls(void);

#endif
