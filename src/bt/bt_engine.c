/*
 * SPDX-License-Identifier: BSD-3-Clause
 */
#define BTSTACK_FILE__ "bt_engine.c"

#include "bt_engine.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "btstack.h"
#include "btstack_chipset_bcm.h"
#include "btstack_chipset_intel_firmware.h"
#include "btstack_chipset_realtek.h"
#include "btstack_config.h"
#include "btstack_lc3_google.h"
#include "btstack_run_loop_posix.h"
#include "btstack_tlv_posix.h"
#include "btstack_uart.h"
#include "classic/btstack_cvsd_plc.h"
#include "classic/btstack_link_key_db_tlv.h"
#include "classic/btstack_sbc.h"
#include "classic/btstack_sbc_bluedroid.h"
#include "classic/hfp_codec.h"
#include "hci_dump_posix_fs.h"
#include "hci_transport_h4.h"
#include "hci_transport_h5.h"
#include "hci_transport_usb.h"

#include "../core/log.h"
#include "../core/paths.h"
#include "firmware.h"
#include "hci_user_channel.h"
#include "usb_guard.h"

#define RFCOMM_CHANNEL 1
#define RECONNECT_MS 15000
#define SILENCE_CHECK_MS 2500
#define CMD_QUEUE_LEN 64
#define LC3_SWB_SAMPLES_PER_FRAME 240
#define LC3_SWB_OCTETS_PER_FRAME 58
#define MAX_SCO_SAMPLES 512

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static bt_config_t cfg;
static char cfg_local_name[64];
static char cfg_fw_dir[512];
static char cfg_trace[512];
static bt_event_cb event_cb;
static void *event_ud;
static pthread_t bt_thread;
static bool thread_started;
static atomic_bool stop_requested;
static bool shutting_down;

static const hci_transport_t *transport;
static const void *transport_config;
static hci_transport_config_uart_t uart_config;
static btstack_packet_callback_registration_t hci_cb_reg;
static uint8_t sdp_record[200];

static char tlv_path[600];
static btstack_tlv_posix_t tlv_ctx;
static const btstack_tlv_t *tlv_impl;
static bool tlv_open;

static hci_con_handle_t acl_handle = HCI_CON_HANDLE_INVALID;
static hci_con_handle_t sco_handle = HCI_CON_HANDLE_INVALID;
static bd_addr_t phone_addr;
static bool phone_connected;
static bool connecting;
static uint8_t codec = HFP_CODEC_CVSD;
static uint32_t sco_rx_packets;

static btstack_timer_source_t reconnect_timer;
static btstack_timer_source_t silence_timer;
static btstack_timer_source_t shutdown_timer;

/* controller report */
static struct {
    uint8_t hci_version, lmp_version;
    uint16_t hci_revision, lmp_subversion, manufacturer;
    char name[249];
    bool firmware_loaded;
} ctrl;

/* codecs */
static btstack_cvsd_plc_state_t cvsd_plc;
static const btstack_sbc_decoder_t *sbc_dec;
static btstack_sbc_decoder_bluedroid_t sbc_dec_ctx;
static const btstack_sbc_encoder_t *sbc_enc;
static btstack_sbc_encoder_bluedroid_t sbc_enc_ctx;
static const btstack_lc3_decoder_t *lc3_dec;
static btstack_lc3_decoder_google_t lc3_dec_ctx;
static btstack_lc3_encoder_google_t lc3_enc_ctx;
static hfp_h2_sync_t h2_sync;
static hfp_codec_t hfp_enc;

/* command queue */
typedef enum {
    CMD_CONNECT, CMD_DISCONNECT, CMD_DISCOVERABLE, CMD_PAIR_REPLY, CMD_PIN_REPLY, CMD_FORGET,
    CMD_LIST_PAIRED, CMD_ANSWER, CMD_REJECT, CMD_HANGUP, CMD_DIAL, CMD_REDIAL, CMD_DTMF,
    CMD_AUDIO_TO_COMPUTER, CMD_AUDIO_TO_PHONE, CMD_HOLD_ACCEPT, CMD_END_ACCEPT, CMD_QUERY_CALLS,
    CMD_REJECT_WAITING,
    CMD_SET_NAME, CMD_SHUTDOWN,
} cmd_type_t;

typedef struct {
    cmd_type_t type;
    char addr[18];
    char text[64];
    int flag;
} cmd_t;

static pthread_mutex_t cmd_lock = PTHREAD_MUTEX_INITIALIZER;
static cmd_t cmd_queue[CMD_QUEUE_LEN];
static int cmd_head, cmd_count;
static btstack_context_callback_registration_t cmd_reg;
static bool cmd_scheduled;
static atomic_bool run_loop_ready;

/* ------------------------------------------------------------------ */
/* Events                                                              */
/* ------------------------------------------------------------------ */

static void emit(bt_event_t *ev)
{
    if (event_cb) event_cb(ev, event_ud);
}

static void emit_simple(bt_event_type_t type, const char *text)
{
    bt_event_t ev = {.type = type};
    if (text) snprintf(ev.text, sizeof ev.text, "%s", text);
    emit(&ev);
}

static void emit_state(bt_state_t st, const char *text)
{
    bt_event_t ev = {.type = BT_EV_STATE, .state = st};
    if (text) snprintf(ev.text, sizeof ev.text, "%s", text);
    emit(&ev);
}

static void emit_addr(bt_event_type_t type, const bd_addr_t addr)
{
    bt_event_t ev = {.type = type};
    snprintf(ev.addr, sizeof ev.addr, "%s", bd_addr_to_str(addr));
    emit(&ev);
}

static void emit_paired_list(void)
{
    bt_event_t ev = {.type = BT_EV_PAIRED_LIST};
    btstack_link_key_iterator_t it;
    if (gap_link_key_iterator_init(&it)) {
        bd_addr_t addr;
        link_key_t key;
        link_key_type_t type;
        size_t pos = 0;
        while (gap_link_key_iterator_get_next(&it, addr, key, &type)) {
            int n = snprintf(ev.text + pos, sizeof ev.text - pos, "%s\n", bd_addr_to_str(addr));
            if (n < 0 || (size_t)n >= sizeof ev.text - pos) break;
            pos += (size_t)n;
        }
        gap_link_key_iterator_done(&it);
    }
    emit(&ev);
}

static void emit_controller_info(void)
{
    bt_event_t ev = {.type = BT_EV_CONTROLLER_INFO};
    bd_addr_t local;
    gap_local_bd_addr(local);
    const bt_adapter_t *a = &cfg.adapter;
    snprintf(ev.text, sizeof ev.text,
             "Adapter: %s\n"
             "Transport: %s\n"
             "USB ID: %04x:%04x (%s)\n"
             "Chip family: %s\n"
             "Address: %s\n"
             "Controller name: %s\n"
             "Manufacturer ID: 0x%04x\n"
             "HCI version: %u, revision 0x%04x\n"
             "LMP version: %u, subversion 0x%04x\n"
             "eSCO (wideband audio): %s\n"
             "Secure Connections: %s\n",
             a->name, bt_transport_name(a->transport), a->vid, a->pid, a->sysfs_name,
             bt_chip_family_name(a->family), bd_addr_to_str(local), ctrl.name, ctrl.manufacturer,
             ctrl.hci_version, ctrl.hci_revision, ctrl.lmp_version, ctrl.lmp_subversion,
             hci_extended_sco_link_supported() ? "yes" : "no",
             gap_secure_connections_active() ? "yes" : "no");
    emit(&ev);
}

/* ------------------------------------------------------------------ */
/* SCO audio                                                           */
/* ------------------------------------------------------------------ */

static void msbc_pcm_cb(int16_t *data, int num_samples, int num_channels, int sample_rate,
                        void *context)
{
    (void)num_channels;
    (void)sample_rate;
    (void)context;
    phone_link_write_rx_s16(cfg.link, data, (size_t)num_samples);
}

static bool lc3_frame_cb(bool bad_frame, const uint8_t *frame_data, uint16_t frame_len)
{
    (void)frame_len;
    if (!bad_frame) frame_data += 2; /* skip H2 header */
    uint8_t bec = 0;
    int16_t samples[LC3_SWB_SAMPLES_PER_FRAME];
    lc3_dec->decode_signed_16(&lc3_dec_ctx, frame_data, bad_frame ? 1 : 0, samples, 1, &bec);
    phone_link_write_rx_s16(cfg.link, samples, LC3_SWB_SAMPLES_PER_FRAME);
    if (bad_frame || bec) atomic_fetch_add(&cfg.link->rx_bad_frames, 1);
    return !bad_frame && bec == 0;
}

static uint32_t codec_rate(uint8_t c)
{
    switch (c) {
    case HFP_CODEC_MSBC: return 16000;
    case HFP_CODEC_LC3_SWB: return 32000;
    default: return 8000;
    }
}

static const char *codec_name(uint8_t c)
{
    switch (c) {
    case HFP_CODEC_MSBC: return "mSBC";
    case HFP_CODEC_LC3_SWB: return "LC3-SWB";
    default: return "CVSD";
    }
}

static void sco_codec_init(uint8_t c)
{
    codec = c;
    switch (c) {
    case HFP_CODEC_MSBC:
        sbc_dec = btstack_sbc_decoder_bluedroid_init_instance(&sbc_dec_ctx);
        sbc_dec->configure(&sbc_dec_ctx, SBC_MODE_mSBC, &msbc_pcm_cb, NULL);
        sbc_enc = btstack_sbc_encoder_bluedroid_init_instance(&sbc_enc_ctx);
        hfp_codec_init_msbc_with_codec(&hfp_enc, sbc_enc, &sbc_enc_ctx);
        break;
    case HFP_CODEC_LC3_SWB: {
        hfp_enc.lc3_encoder_context = &lc3_enc_ctx;
        const btstack_lc3_encoder_t *enc = btstack_lc3_encoder_google_init_instance(&lc3_enc_ctx);
        hfp_codec_init_lc3_swb(&hfp_enc, enc, &lc3_enc_ctx);
        lc3_dec = btstack_lc3_decoder_google_init_instance(&lc3_dec_ctx);
        lc3_dec->configure(&lc3_dec_ctx, 32000, BTSTACK_LC3_FRAME_DURATION_7500US,
                           LC3_SWB_OCTETS_PER_FRAME);
        hfp_h2_sync_init(&h2_sync, &lc3_frame_cb);
        break;
    }
    case HFP_CODEC_CVSD:
    default:
        codec = HFP_CODEC_CVSD;
        btstack_cvsd_plc_init(&cvsd_plc);
        break;
    }
}

static void sco_receive(uint8_t *packet, uint16_t size)
{
    if (size < 3) return;
    sco_rx_packets++;
    uint8_t status = (packet[1] >> 4) & 3;
    switch (codec) {
    case HFP_CODEC_MSBC:
        if (status) atomic_fetch_add(&cfg.link->rx_bad_frames, 1);
        sbc_dec->decode_signed_16(&sbc_dec_ctx, status, packet + 3, size - 3);
        break;
    case HFP_CODEC_LC3_SWB:
        hfp_h2_sync_process(&h2_sync, status != 0, &packet[3], size - 3);
        break;
    default: {
        int n = (size - 3) / 2;
        if (n > MAX_SCO_SAMPLES) n = MAX_SCO_SAMPLES;
        int16_t in[MAX_SCO_SAMPLES], out[MAX_SCO_SAMPLES];
        for (int i = 0; i < n; i++) in[i] = (int16_t)little_endian_read_16(packet, 3 + i * 2);
        bool bad = status != 0;
        if (bad) atomic_fetch_add(&cfg.link->rx_bad_frames, 1);
        btstack_cvsd_plc_process_data(&cvsd_plc, bad, in, (uint16_t)n, out);
        phone_link_write_rx_s16(cfg.link, out, (size_t)n);
        break;
    }
    }
}

static void sco_fill_payload(uint8_t *payload, uint16_t len)
{
    if (codec == HFP_CODEC_CVSD) {
        int16_t pcm[MAX_SCO_SAMPLES];
        uint16_t n = len / 2;
        if (n > MAX_SCO_SAMPLES) n = MAX_SCO_SAMPLES;
        phone_link_read_tx_s16(cfg.link, pcm, n);
        for (uint16_t i = 0; i < n; i++) little_endian_store_16(payload, i * 2, (uint16_t)pcm[i]);
        return;
    }
    while (len > 0) {
        if (hfp_codec_can_encode_audio_frame_now(&hfp_enc)) {
            int16_t pcm[LC3_SWB_SAMPLES_PER_FRAME];
            uint16_t n = hfp_codec_num_audio_samples_per_frame(&hfp_enc);
            if (n > LC3_SWB_SAMPLES_PER_FRAME) n = LC3_SWB_SAMPLES_PER_FRAME;
            phone_link_read_tx_s16(cfg.link, pcm, n);
            hfp_codec_encode_audio_frame(&hfp_enc, pcm);
        }
        uint16_t avail = hfp_codec_num_bytes_available(&hfp_enc);
        uint16_t chunk = avail < len ? avail : len;
        if (chunk == 0) {
            memset(payload, 0, len); /* nothing encoded yet: send silence, not old bytes */
            break;
        }
        hfp_codec_read_from_stream(&hfp_enc, payload, chunk);
        payload += chunk;
        len -= chunk;
    }
}

static void sco_send(hci_con_handle_t handle)
{
    if (handle == HCI_CON_HANDLE_INVALID || handle != sco_handle) return;
    int pkt_len = hci_get_sco_packet_length_for_connection(handle);
    if (pkt_len <= 3) return;
    uint16_t payload_len = (uint16_t)(pkt_len - 3);
    hci_reserve_packet_buffer();
    uint8_t *pkt = hci_get_outgoing_packet_buffer();
    sco_fill_payload(&pkt[3], payload_len);
    little_endian_store_16(pkt, 0, handle);
    pkt[2] = (uint8_t)payload_len;
    hci_send_sco_packet_buffer(pkt_len);
    hci_request_sco_can_send_now_event_for_con_handle(handle);
}

static void silence_check(btstack_timer_source_t *ts)
{
    (void)ts;
    if (sco_handle != HCI_CON_HANDLE_INVALID && sco_rx_packets == 0) {
        emit_simple(BT_EV_AUDIO_SILENT,
                    "The audio link is up, but no audio arrives from the adapter.");
        LOGW("bt", "SCO link up but no SCO packets received");
    }
}

/* ------------------------------------------------------------------ */
/* Reconnect                                                           */
/* ------------------------------------------------------------------ */

static void try_connect(const bd_addr_t addr)
{
    if (phone_connected || connecting) return;
    uint8_t st = hfp_hf_establish_service_level_connection((uint8_t *)addr);
    if (st == ERROR_CODE_SUCCESS) {
        connecting = true;
        memcpy(phone_addr, addr, 6);
    } else {
        LOGI("bt", "connect to %s not possible now (0x%02x)", bd_addr_to_str(addr), st);
    }
}

static void reconnect_tick(btstack_timer_source_t *ts)
{
    bd_addr_t addr;
    if (!shutting_down && !phone_connected && !connecting && cfg.auto_connect_addr[0] &&
        sscanf_bd_addr(cfg.auto_connect_addr, addr))
        try_connect(addr);
    btstack_run_loop_set_timer(ts, RECONNECT_MS);
    btstack_run_loop_add_timer(ts);
}

/* ------------------------------------------------------------------ */
/* HCI events                                                          */
/* ------------------------------------------------------------------ */

static void open_tlv(void)
{
    bd_addr_t local;
    gap_local_bd_addr(local);
    paths_mkdirs(paths_data_dir());
    snprintf(tlv_path, sizeof tlv_path, "%s/pairings-%s.tlv", paths_data_dir(),
             bd_addr_to_str_with_delimiter(local, '-'));
    tlv_impl = btstack_tlv_posix_init_instance(&tlv_ctx, tlv_path);
    btstack_tlv_set_instance(tlv_impl, &tlv_ctx);
    hci_set_link_key_db(btstack_link_key_db_tlv_get_instance(tlv_impl, &tlv_ctx));
    tlv_open = true;
}

static void setup_codecs(void)
{
    uint8_t codecs[3];
    uint8_t n = 0;
    codecs[n++] = HFP_CODEC_CVSD;
    if (hci_extended_sco_link_supported()) {
        if (cfg.allow_wideband) codecs[n++] = HFP_CODEC_MSBC;
        if (cfg.allow_super_wideband) codecs[n++] = HFP_CODEC_LC3_SWB;
    } else {
        LOGW("bt", "adapter has no eSCO: only narrowband (8 kHz) call audio possible");
    }
    hfp_hf_init_codecs(n, codecs);
}

static void on_working(void)
{
    open_tlv();
    setup_codecs();
    char msg[200];
    bd_addr_t local;
    gap_local_bd_addr(local);
    snprintf(msg, sizeof msg, "%s", bd_addr_to_str(local));
    emit_state(BT_STATE_READY, msg);
    emit_controller_info();
    emit_paired_list();
    btstack_run_loop_set_timer_handler(&reconnect_timer, reconnect_tick);
    btstack_run_loop_set_timer(&reconnect_timer, 500);
    btstack_run_loop_add_timer(&reconnect_timer);
}

static void hci_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    (void)channel;
    if (packet_type == HCI_SCO_DATA_PACKET) {
        if (READ_SCO_CONNECTION_HANDLE(packet) == sco_handle) sco_receive(packet, size);
        return;
    }
    if (packet_type != HCI_EVENT_PACKET) return;

    bd_addr_t addr;
    switch (hci_event_packet_get_type(packet)) {
    case HCI_EVENT_TRANSPORT_USB_INFO: {
        uint16_t vid = hci_event_transport_usb_info_get_vendor_id(packet);
        uint16_t pid = hci_event_transport_usb_info_get_product_id(packet);
        LOGI("bt", "USB device %04x:%04x opened", vid, pid);
        if (cfg.adapter.family == BT_CHIP_REALTEK) {
            const char *dir = bt_firmware_prepare(BT_CHIP_REALTEK, cfg_fw_dir);
            if (dir) {
                btstack_chipset_realtek_set_firmware_folder_path(dir);
                btstack_chipset_realtek_set_config_folder_path(dir);
            }
            /* rebranded dongles report their own ID; use the Realtek one */
            uint16_t rtk = cfg.adapter.rtk_pid ? cfg.adapter.rtk_pid : pid;
            if (rtk != pid) LOGI("bt", "using Realtek ID %04x for %04x:%04x", rtk, vid, pid);
            btstack_chipset_realtek_set_product_id(rtk);
            hci_set_chipset(btstack_chipset_realtek_instance());
            hci_enable_custom_pre_init();
            ctrl.firmware_loaded = true;
        }
        break;
    }
    case BTSTACK_EVENT_STATE:
        switch (btstack_event_state_get_state(packet)) {
        case HCI_STATE_WORKING: on_working(); break;
        case HCI_STATE_OFF:
            if (tlv_open) {
                btstack_tlv_posix_deinit(&tlv_ctx);
                tlv_open = false;
            }
            /* hci_close() runs after the loop has exited (it emits this
             * event again, so it must not be called from here) */
            if (shutting_down) {
                btstack_run_loop_remove_timer(&shutdown_timer);
                btstack_run_loop_trigger_exit();
            }
            break;
        default: break;
        }
        break;
    case HCI_EVENT_COMMAND_COMPLETE: {
        uint16_t op = hci_event_command_complete_get_command_opcode(packet);
        const uint8_t *ret = hci_event_command_complete_get_return_parameters(packet);
        if (op == HCI_OPCODE_HCI_READ_LOCAL_VERSION_INFORMATION && ret[0] == 0 && size >= 14) {
            ctrl.hci_version = packet[6];
            ctrl.hci_revision = little_endian_read_16(packet, 7);
            ctrl.lmp_version = packet[9];
            ctrl.manufacturer = little_endian_read_16(packet, 10);
            ctrl.lmp_subversion = little_endian_read_16(packet, 12);
            if (ctrl.manufacturer == BLUETOOTH_COMPANY_ID_BROADCOM_CORPORATION &&
                cfg.adapter.family == BT_CHIP_BROADCOM &&
                cfg.adapter.transport != BT_TRANSPORT_USERCHAN) {
                const char *dir = bt_firmware_prepare(BT_CHIP_BROADCOM, cfg_fw_dir);
                if (dir) btstack_chipset_bcm_set_hcd_folder_path(dir);
                hci_set_chipset(btstack_chipset_bcm_instance());
            }
        } else if (op == HCI_OPCODE_HCI_READ_LOCAL_NAME && ret[0] == 0 && size >= 6 + 248) {
            memcpy(ctrl.name, &packet[6], 248);
            ctrl.name[248] = 0;
            if (ctrl.manufacturer == BLUETOOTH_COMPANY_ID_BROADCOM_CORPORATION &&
                cfg.adapter.family == BT_CHIP_BROADCOM &&
                cfg.adapter.transport != BT_TRANSPORT_USERCHAN) {
                btstack_chipset_bcm_set_device_name(ctrl.name);
                ctrl.firmware_loaded = true;
            }
        }
        break;
    }
    case HCI_EVENT_PIN_CODE_REQUEST:
        hci_event_pin_code_request_get_bd_addr(packet, addr);
        emit_addr(BT_EV_PIN_REQUEST, addr);
        break;
    case HCI_EVENT_USER_CONFIRMATION_REQUEST: {
        hci_event_user_confirmation_request_get_bd_addr(packet, addr);
        bt_event_t ev = {.type = BT_EV_PAIR_CONFIRM};
        snprintf(ev.addr, sizeof ev.addr, "%s", bd_addr_to_str(addr));
        ev.number = (int32_t)hci_event_user_confirmation_request_get_numeric_value(packet);
        emit(&ev);
        break;
    }
    case GAP_EVENT_PAIRING_COMPLETE: {
        gap_event_pairing_complete_get_bd_addr(packet, addr);
        bt_event_t ev = {.type = BT_EV_PAIRED};
        snprintf(ev.addr, sizeof ev.addr, "%s", bd_addr_to_str(addr));
        ev.number = gap_event_pairing_complete_get_status(packet);
        emit(&ev);
        emit_paired_list();
        break;
    }
    case HCI_EVENT_LINK_KEY_NOTIFICATION:
        /* BTstack stores the key while handling this event, after it already
         * reported "pairing complete": refresh the list now */
        emit_paired_list();
        break;
    case HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE:
        if (size >= 9 + 248 && packet[2] == 0) {
            reverse_bd_addr(&packet[3], addr);
            bt_event_t ev = {.type = BT_EV_REMOTE_NAME};
            snprintf(ev.addr, sizeof ev.addr, "%s", bd_addr_to_str(addr));
            char name[249];
            memcpy(name, &packet[9], 248);
            name[248] = 0;
            snprintf(ev.text, sizeof ev.text, "%s", name);
            emit(&ev);
        }
        break;
    case HCI_EVENT_SCO_CAN_SEND_NOW:
        sco_send(hci_event_sco_can_send_now_get_handle(packet));
        break;
    default: break;
    }
}

/* ------------------------------------------------------------------ */
/* HFP events                                                          */
/* ------------------------------------------------------------------ */

static void hfp_handler(uint8_t packet_type, uint16_t channel, uint8_t *event, uint16_t size)
{
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET || hci_event_packet_get_type(event) != HCI_EVENT_HFP_META)
        return;
    uint8_t status;
    bd_addr_t addr;
    switch (hci_event_hfp_meta_get_subevent_code(event)) {
    case HFP_SUBEVENT_SERVICE_LEVEL_CONNECTION_ESTABLISHED:
        connecting = false;
        status = hfp_subevent_service_level_connection_established_get_status(event);
        if (status != ERROR_CODE_SUCCESS) {
            LOGI("bt", "service level connection failed: 0x%02x", status);
            break;
        }
        acl_handle = hfp_subevent_service_level_connection_established_get_acl_handle(event);
        hfp_subevent_service_level_connection_established_get_bd_addr(event, addr);
        memcpy(phone_addr, addr, 6);
        phone_connected = true;
        snprintf(cfg.auto_connect_addr, sizeof cfg.auto_connect_addr, "%s", bd_addr_to_str(addr));
        emit_addr(BT_EV_PHONE_CONNECTED, addr);
        emit_paired_list();
        gap_discoverable_control(0);
        gap_remote_name_request(addr, 0, 0);
        hfp_hf_activate_calling_line_notification(acl_handle);
        hfp_hf_activate_call_waiting_notification(acl_handle); /* +CCWA: who is waiting */
        if (cfg.disable_phone_nrec) hfp_hf_deactivate_echo_canceling_and_noise_reduction(acl_handle);
        break;
    case HFP_SUBEVENT_SERVICE_LEVEL_CONNECTION_RELEASED:
        if (sco_handle != HCI_CON_HANDLE_INVALID) {
            /* no "audio released" came first (link loss): end the audio too,
             * so the mixer stops waiting for caller audio */
            btstack_run_loop_remove_timer(&silence_timer);
            sco_handle = HCI_CON_HANDLE_INVALID;
            phone_link_end(cfg.link);
            emit_simple(BT_EV_AUDIO_DISCONNECTED, NULL);
        }
        acl_handle = HCI_CON_HANDLE_INVALID;
        phone_connected = false;
        connecting = false;
        emit_simple(BT_EV_PHONE_DISCONNECTED, NULL);
        break;
    case HFP_SUBEVENT_AUDIO_CONNECTION_ESTABLISHED: {
        status = hfp_subevent_audio_connection_established_get_status(event);
        if (status != ERROR_CODE_SUCCESS) {
            char msg[100];
            snprintf(msg, sizeof msg, "Audio connection failed (0x%02x)", status);
            emit_simple(BT_EV_ERROR, msg);
            break;
        }
        sco_handle = hfp_subevent_audio_connection_established_get_sco_handle(event);
        uint8_t c = hfp_subevent_audio_connection_established_get_negotiated_codec(event);
        sco_codec_init(c);
        sco_rx_packets = 0;
        phone_link_begin(cfg.link, codec_rate(codec));
        hci_request_sco_can_send_now_event_for_con_handle(sco_handle);
        bt_event_t ev = {.type = BT_EV_AUDIO_CONNECTED, .number = (int32_t)codec_rate(codec)};
        snprintf(ev.text, sizeof ev.text, "%s", codec_name(codec));
        emit(&ev);
        btstack_run_loop_set_timer_handler(&silence_timer, silence_check);
        btstack_run_loop_set_timer(&silence_timer, SILENCE_CHECK_MS);
        btstack_run_loop_add_timer(&silence_timer);
        break;
    }
    case HFP_SUBEVENT_AUDIO_CONNECTION_RELEASED:
        btstack_run_loop_remove_timer(&silence_timer);
        sco_handle = HCI_CON_HANDLE_INVALID;
        phone_link_end(cfg.link);
        emit_simple(BT_EV_AUDIO_DISCONNECTED, NULL);
        break;
    case HFP_SUBEVENT_AG_INDICATOR_STATUS_CHANGED: {
        bt_event_t ev = {.type = BT_EV_INDICATOR};
        snprintf(ev.text, sizeof ev.text, "%s",
                 (const char *)hfp_subevent_ag_indicator_status_changed_get_indicator_name(event));
        ev.number = hfp_subevent_ag_indicator_status_changed_get_indicator_status(event);
        emit(&ev);
        break;
    }
    case HFP_SUBEVENT_RING:
        emit_simple(BT_EV_RING, NULL);
        break;
    case HFP_SUBEVENT_CALLING_LINE_IDENTIFICATION_NOTIFICATION: {
        bt_event_t ev = {.type = BT_EV_CALLER_ID};
        snprintf(ev.text, sizeof ev.text, "%s",
                 (const char *)hfp_subevent_calling_line_identification_notification_get_number(event));
        snprintf(ev.text2, sizeof ev.text2, "%s",
                 (const char *)hfp_subevent_calling_line_identification_notification_get_alpha(event));
        emit(&ev);
        break;
    }
    case HFP_SUBEVENT_CALL_WAITING_NOTIFICATION: {
        bt_event_t ev = {.type = BT_EV_CALL_WAITING};
        snprintf(ev.text, sizeof ev.text, "%s",
                 (const char *)hfp_subevent_call_waiting_notification_get_number(event));
        snprintf(ev.text2, sizeof ev.text2, "%s",
                 (const char *)hfp_subevent_call_waiting_notification_get_alpha(event));
        emit(&ev);
        break;
    }
    case HFP_SUBEVENT_ENHANCED_CALL_STATUS: {
        bt_event_t ev = {.type = BT_EV_CALL_STATUS};
        ev.number = hfp_subevent_enhanced_call_status_get_clcc_idx(event);
        ev.value2 = hfp_subevent_enhanced_call_status_get_clcc_status(event);
        snprintf(ev.text, sizeof ev.text, "%s",
                 (const char *)hfp_subevent_enhanced_call_status_get_bnip_number(event));
        emit(&ev);
        break;
    }
    case HFP_SUBEVENT_NETWORK_OPERATOR_CHANGED: {
        bt_event_t ev = {.type = BT_EV_OPERATOR};
        snprintf(ev.text, sizeof ev.text, "%s",
                 (const char *)hfp_subevent_network_operator_changed_get_network_operator_name(event));
        emit(&ev);
        break;
    }
    case HFP_SUBEVENT_SPEAKER_VOLUME: {
        bt_event_t ev = {.type = BT_EV_SPEAKER_GAIN,
                         .number = hfp_subevent_speaker_volume_get_gain(event)};
        emit(&ev);
        break;
    }
    case HFP_SUBEVENT_EXTENDED_AUDIO_GATEWAY_ERROR: {
        char msg[80];
        snprintf(msg, sizeof msg, "Phone reported error %u",
                 hfp_subevent_extended_audio_gateway_error_get_error(event));
        emit_simple(BT_EV_ERROR, msg);
        break;
    }
    default: break;
    }
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

static void shutdown_timeout(btstack_timer_source_t *ts)
{
    (void)ts;
    LOGW("bt", "controller did not power off in time, closing anyway");
    btstack_run_loop_trigger_exit();
}

static void exec_cmd(const cmd_t *c)
{
    bd_addr_t addr;
    bool has_addr = c->addr[0] && sscanf_bd_addr(c->addr, addr);
    bool slc = acl_handle != HCI_CON_HANDLE_INVALID;
    switch (c->type) {
    case CMD_CONNECT:
        if (has_addr) {
            snprintf(cfg.auto_connect_addr, sizeof cfg.auto_connect_addr, "%s", c->addr);
            try_connect(addr);
        }
        break;
    case CMD_DISCONNECT:
        cfg.auto_connect_addr[0] = 0; /* do not reconnect behind the user's back */
        if (slc) hfp_hf_release_service_level_connection(acl_handle);
        break;
    case CMD_DISCOVERABLE: gap_discoverable_control(c->flag ? 1 : 0); break;
    case CMD_SET_NAME:
        /* BTstack keeps the pointer, so the name lives in a static buffer */
        snprintf(cfg_local_name, sizeof cfg_local_name, "%s", c->text);
        gap_set_local_name(cfg_local_name);
        LOGI("bt", "local name set to \"%s\"", cfg_local_name);
        break;
    case CMD_PAIR_REPLY:
        if (!has_addr) break;
        if (c->flag)
            gap_ssp_confirmation_response(addr);
        else
            gap_ssp_confirmation_negative(addr);
        break;
    case CMD_PIN_REPLY:
        if (!has_addr) break;
        if (c->flag)
            gap_pin_code_response(addr, c->text);
        else
            gap_pin_code_negative(addr);
        break;
    case CMD_FORGET:
        if (has_addr) {
            if (phone_connected && bd_addr_cmp(addr, phone_addr) == 0 && slc)
                hfp_hf_release_service_level_connection(acl_handle);
            gap_drop_link_key_for_bd_addr(addr);
            if (strcmp(cfg.auto_connect_addr, c->addr) == 0) cfg.auto_connect_addr[0] = 0;
            emit_paired_list();
        }
        break;
    case CMD_LIST_PAIRED: emit_paired_list(); break;
    case CMD_ANSWER: if (slc) hfp_hf_answer_incoming_call(acl_handle); break;
    case CMD_REJECT: if (slc) hfp_hf_reject_incoming_call(acl_handle); break;
    case CMD_HANGUP: if (slc) hfp_hf_terminate_call(acl_handle); break;
    case CMD_DIAL: if (slc && c->text[0]) hfp_hf_dial_number(acl_handle, (char *)c->text); break;
    case CMD_REDIAL: if (slc) hfp_hf_redial_last_number(acl_handle); break;
    case CMD_DTMF: if (slc) hfp_hf_send_dtmf_code(acl_handle, c->text[0]); break;
    case CMD_AUDIO_TO_COMPUTER: if (slc) hfp_hf_establish_audio_connection(acl_handle); break;
    case CMD_AUDIO_TO_PHONE: if (slc) hfp_hf_release_audio_connection(acl_handle); break;
    case CMD_HOLD_ACCEPT: if (slc) hfp_hf_swap_calls(acl_handle); break;
    case CMD_END_ACCEPT: if (slc) hfp_hf_end_active_and_accept_other(acl_handle); break;
    case CMD_REJECT_WAITING: if (slc) hfp_hf_user_busy(acl_handle); break;
    case CMD_QUERY_CALLS: if (slc) hfp_hf_query_current_call_status(acl_handle); break;
    case CMD_SHUTDOWN:
        shutting_down = true;
        btstack_run_loop_remove_timer(&reconnect_timer);
        btstack_run_loop_remove_timer(&silence_timer);
        if (hci_get_state() == HCI_STATE_OFF) {
            btstack_run_loop_trigger_exit();
        } else {
            /* if the controller never confirms power-off, leave anyway */
            btstack_run_loop_set_timer_handler(&shutdown_timer, shutdown_timeout);
            btstack_run_loop_set_timer(&shutdown_timer, 3000);
            btstack_run_loop_add_timer(&shutdown_timer);
            hci_power_control(HCI_POWER_OFF);
        }
        break;
    }
}

static void drain_cmds(void *ctx)
{
    (void)ctx;
    for (;;) {
        cmd_t c;
        pthread_mutex_lock(&cmd_lock);
        if (cmd_count == 0) {
            cmd_scheduled = false;
            pthread_mutex_unlock(&cmd_lock);
            return;
        }
        c = cmd_queue[cmd_head];
        cmd_head = (cmd_head + 1) % CMD_QUEUE_LEN;
        cmd_count--;
        pthread_mutex_unlock(&cmd_lock);
        exec_cmd(&c);
    }
}

static void post(cmd_t c)
{
    if (!thread_started) return;
    bool schedule = false;
    pthread_mutex_lock(&cmd_lock);
    if (cmd_count < CMD_QUEUE_LEN) {
        cmd_queue[(cmd_head + cmd_count) % CMD_QUEUE_LEN] = c;
        cmd_count++;
    }
    if (!cmd_scheduled && atomic_load(&run_loop_ready)) {
        cmd_scheduled = true;
        schedule = true;
    }
    pthread_mutex_unlock(&cmd_lock);
    if (schedule) {
        cmd_reg.callback = drain_cmds;
        cmd_reg.context = NULL;
        btstack_run_loop_execute_on_main_thread(&cmd_reg);
    }
}

static cmd_t mk(cmd_type_t t, const char *addr, const char *text, int flag)
{
    cmd_t c = {.type = t, .flag = flag};
    if (addr) snprintf(c.addr, sizeof c.addr, "%s", addr);
    if (text) snprintf(c.text, sizeof c.text, "%s", text);
    return c;
}

void bt_cmd_connect(const char *addr) { post(mk(CMD_CONNECT, addr, NULL, 0)); }
void bt_cmd_disconnect(void) { post(mk(CMD_DISCONNECT, NULL, NULL, 0)); }
void bt_cmd_set_discoverable(bool on) { post(mk(CMD_DISCOVERABLE, NULL, NULL, on)); }
void bt_cmd_set_name(const char *name) { post(mk(CMD_SET_NAME, NULL, name, 0)); }
void bt_cmd_pair_reply(const char *addr, bool accept) { post(mk(CMD_PAIR_REPLY, addr, NULL, accept)); }
void bt_cmd_pin_reply(const char *addr, const char *pin)
{
    post(mk(CMD_PIN_REPLY, addr, pin, pin != NULL));
}
void bt_cmd_forget(const char *addr) { post(mk(CMD_FORGET, addr, NULL, 0)); }
void bt_cmd_list_paired(void) { post(mk(CMD_LIST_PAIRED, NULL, NULL, 0)); }
void bt_cmd_answer(void) { post(mk(CMD_ANSWER, NULL, NULL, 0)); }
void bt_cmd_reject(void) { post(mk(CMD_REJECT, NULL, NULL, 0)); }
void bt_cmd_hangup(void) { post(mk(CMD_HANGUP, NULL, NULL, 0)); }
void bt_cmd_dial(const char *number) { post(mk(CMD_DIAL, NULL, number, 0)); }
void bt_cmd_redial(void) { post(mk(CMD_REDIAL, NULL, NULL, 0)); }
void bt_cmd_dtmf(char digit)
{
    char s[2] = {digit, 0};
    post(mk(CMD_DTMF, NULL, s, 0));
}
void bt_cmd_audio_to_computer(void) { post(mk(CMD_AUDIO_TO_COMPUTER, NULL, NULL, 0)); }
void bt_cmd_audio_to_phone(void) { post(mk(CMD_AUDIO_TO_PHONE, NULL, NULL, 0)); }
void bt_cmd_hold_and_accept(void) { post(mk(CMD_HOLD_ACCEPT, NULL, NULL, 0)); }
void bt_cmd_end_and_accept(void) { post(mk(CMD_END_ACCEPT, NULL, NULL, 0)); }
void bt_cmd_reject_waiting(void) { post(mk(CMD_REJECT_WAITING, NULL, NULL, 0)); }
void bt_cmd_query_calls(void) { post(mk(CMD_QUERY_CALLS, NULL, NULL, 0)); }

/* ------------------------------------------------------------------ */
/* Start-up                                                            */
/* ------------------------------------------------------------------ */

static void stack_init(void)
{
    hci_init(transport, transport_config);

    hci_cb_reg.callback = &hci_handler;
    hci_add_event_handler(&hci_cb_reg);
    hci_register_sco_packet_handler(&hci_handler);

    l2cap_init();
    rfcomm_init();
    sdp_init();

    uint16_t features = (1 << HFP_HFSF_ESCO_S4) | (1 << HFP_HFSF_CLI_PRESENTATION_CAPABILITY) |
                        (1 << HFP_HFSF_HF_INDICATORS) | (1 << HFP_HFSF_CODEC_NEGOTIATION) |
                        (1 << HFP_HFSF_ENHANCED_CALL_STATUS) | (1 << HFP_HFSF_EC_NR_FUNCTION) |
                        (1 << HFP_HFSF_THREE_WAY_CALLING) | (1 << HFP_HFSF_REMOTE_VOLUME_CONTROL);
    static uint16_t indicators[1] = {0x02}; /* battery level */
    static uint8_t codecs[3];
    uint8_t n_codecs = 0;
    codecs[n_codecs++] = HFP_CODEC_CVSD;
    if (cfg.allow_wideband) codecs[n_codecs++] = HFP_CODEC_MSBC;
    if (cfg.allow_super_wideband) codecs[n_codecs++] = HFP_CODEC_LC3_SWB;

    hfp_hf_init(RFCOMM_CHANNEL);
    hfp_hf_init_supported_features(features);
    hfp_hf_init_hf_indicators(1, indicators);
    hfp_hf_init_codecs(n_codecs, codecs);
    hfp_hf_register_packet_handler(hfp_handler);
    hfp_hf_apple_set_identification(BLUETOOTH_COMPANY_ID_BLUEKITCHEN_GMBH, 0x0001, "0001", 3);
    hfp_hf_apple_set_battery_level(9);
    hfp_hf_apple_set_docked_state(1);

    memset(sdp_record, 0, sizeof sdp_record);
    hfp_hf_create_sdp_record_with_codecs(sdp_record, sdp_create_service_record_handle(),
                                         RFCOMM_CHANNEL, cfg_local_name, features, n_codecs,
                                         codecs);
    sdp_register_service(sdp_record);

    gap_set_local_name(cfg_local_name);
    gap_discoverable_control(1);
    gap_set_class_of_device(0x200408); /* Audio, hands-free device */
    gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_ROLE_SWITCH |
                                         LM_LINK_POLICY_ENABLE_SNIFF_MODE);
    gap_set_allow_role_switch(true);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_DISPLAY_YES_NO);
    /* BTstack confirms numeric comparison by itself unless told not to: the
     * user must see and confirm the code */
    gap_ssp_set_auto_accept(0);
    gap_ssp_set_authentication_requirement(SSP_IO_AUTHREQ_MITM_PROTECTION_REQUIRED_GENERAL_BONDING);
    gap_secure_connections_enable(cfg.secure_connections);
    hci_set_sco_voice_setting(0x60); /* 16-bit linear PCM for CVSD */

    if (hci_power_control(HCI_POWER_ON) != 0) {
        char msg[400];
        snprintf(msg, sizeof msg, "could not open %s", cfg.adapter.name);
        emit_state(BT_STATE_FAILED, msg);
    }
}

static void intel_done(int result)
{
    if (result != 0) {
        emit_state(BT_STATE_FAILED,
                   "Intel firmware could not be loaded. Choose the \"kernel driver\" mode for "
                   "this adapter in Settings.");
        btstack_run_loop_trigger_exit();
        return;
    }
    ctrl.firmware_loaded = true;
    stack_init();
}

static void *bt_main(void *arg)
{
    (void)arg;
    btstack_memory_init();
    btstack_run_loop_init(btstack_run_loop_posix_get_instance());
    atomic_store(&run_loop_ready, true);

    if (cfg_trace[0]) {
        hci_dump_posix_fs_open(cfg_trace, HCI_DUMP_PACKETLOGGER);
        hci_dump_init(hci_dump_posix_fs_get_instance());
        LOGI("bt", "HCI trace: %s", cfg_trace);
    }

    bt_adapter_t *a = &cfg.adapter;
    emit_state(BT_STATE_STARTING, a->name);
    bool intel_fw = false;
    switch (a->transport) {
    case BT_TRANSPORT_USB:
        hci_transport_usb_set_bus_and_path(a->bus, a->n_ports, a->ports);
        hci_transport_usb_add_device(a->vid, a->pid);
        usb_guard_take(a->bus, a->ports, a->n_ports);
        transport = hci_transport_usb_instance();
        transport_config = NULL;
        if (a->family == BT_CHIP_INTEL) {
            const char *dir = bt_firmware_prepare(BT_CHIP_INTEL, cfg_fw_dir);
            if (dir) btstack_chipset_intel_set_firmware_path(dir);
            intel_fw = true;
        }
        break;
    case BT_TRANSPORT_USERCHAN: {
        char err[256];
        int fd = hci_user_channel_open_socket(a->hci_index, err, sizeof err);
        if (fd < 0) {
            atomic_store(&run_loop_ready, false);
            emit_state(BT_STATE_FAILED, err);
            return NULL;
        }
        hci_user_channel_set_socket(fd);
        transport = hci_user_channel_instance();
        transport_config = NULL;
        break;
    }
    case BT_TRANSPORT_UART:
        uart_config.type = HCI_TRANSPORT_CONFIG_UART;
        uart_config.baudrate_init = a->uart_baud ? a->uart_baud : 115200;
        uart_config.baudrate_main = 0;
        uart_config.flowcontrol = a->uart_flow_control ? 1 : 0;
        uart_config.device_name = a->uart_path;
        uart_config.parity = BTSTACK_UART_PARITY_OFF;
        transport = a->uart_h5 ? hci_transport_h5_instance(btstack_uart_posix_instance())
                               : hci_transport_h4_instance_for_uart(btstack_uart_posix_instance());
        transport_config = &uart_config;
        break;
    }

    if (intel_fw)
        btstack_chipset_intel_download_firmware(transport, &intel_done);
    else
        stack_init();

    btstack_run_loop_execute();

    shutting_down = true;
    hci_close(); /* closes the transport (USB, socket or serial port) */
    if (sco_handle != HCI_CON_HANDLE_INVALID) {
        sco_handle = HCI_CON_HANDLE_INVALID;
        phone_link_end(cfg.link);
    }
    atomic_store(&run_loop_ready, false);
    if (a->transport == BT_TRANSPORT_USB) usb_guard_release();
    if (cfg_trace[0]) hci_dump_posix_fs_close();
    emit_state(BT_STATE_OFF, NULL);
    return NULL;
}

int bt_engine_start(const bt_config_t *c, bt_event_cb cb, void *ud)
{
    if (thread_started) return -1;
    cfg = *c;
    snprintf(cfg_local_name, sizeof cfg_local_name, "%s",
             c->local_name && *c->local_name ? c->local_name : "Phone Recorder");
    snprintf(cfg_fw_dir, sizeof cfg_fw_dir, "%s", c->firmware_dir ? c->firmware_dir : "");
    snprintf(cfg_trace, sizeof cfg_trace, "%s", c->trace_path ? c->trace_path : "");
    cfg.local_name = cfg_local_name;
    cfg.firmware_dir = cfg_fw_dir;
    cfg.trace_path = cfg_trace;
    event_cb = cb;
    event_ud = ud;
    atomic_store(&stop_requested, false);
    if (pthread_create(&bt_thread, NULL, bt_main, NULL) != 0) return -1;
    thread_started = true;
    return 0;
}

void bt_engine_stop(void)
{
    if (!thread_started) return;
    if (atomic_exchange(&stop_requested, true)) return;
    /* wait until the run loop accepts commands, then ask it to shut down */
    for (int i = 0; i < 200 && !atomic_load(&run_loop_ready); i++) usleep(10000);
    if (atomic_load(&run_loop_ready)) post(mk(CMD_SHUTDOWN, NULL, NULL, 0));
    pthread_join(bt_thread, NULL);
    thread_started = false;
}
