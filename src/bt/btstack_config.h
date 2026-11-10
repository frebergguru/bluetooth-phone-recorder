/*
 * BTstack configuration for Bluetooth Phone Recorder.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Classic Bluetooth only: we act as a Hands-Free unit (HFP HF) towards a phone.
 */
#ifndef BTSTACK_CONFIG_H
#define BTSTACK_CONFIG_H

/* Port related features */
#define HAVE_ASSERT
#define HAVE_MALLOC
#define HAVE_POSIX_FILE_IO
#define HAVE_POSIX_TIME

/* BTstack features */
#define ENABLE_CLASSIC
#define ENABLE_SCO_OVER_HCI
#define ENABLE_HFP_WIDE_BAND_SPEECH
#define ENABLE_HFP_SUPER_WIDE_BAND_SPEECH
#define ENABLE_LOG_ERROR
#define ENABLE_LOG_INFO
#define ENABLE_PRINTF_HEXDUMP
#define ENABLE_SDP_DES_DUMP
#define ENABLE_L2CAP_ENHANCED_RETRANSMISSION_MODE
/* Phonebook access (PBAP) over OBEX; newer phones use OBEX over L2CAP */
#define ENABLE_GOEP_L2CAP
/* H5 (three-wire UART) needs SLIP framing */
#define ENABLE_H5

/* Buffers and sizes */
#define HCI_ACL_PAYLOAD_SIZE (1691 + 4)
#define HCI_INCOMING_PRE_BUFFER_SIZE 14
#define NVM_NUM_LINK_KEYS 32
#define NVM_NUM_DEVICE_DB_ENTRIES 16

#endif
