/*
 * Minimal vCard reader for phone address books (vCard 2.1 and 3.0, as sent
 * over the Bluetooth phonebook profile). Reads names and phone numbers only.
 * Handles folded lines, quoted-printable (used by Android for non-ASCII
 * names in vCard 2.1), CHARSET parameters and backslash escapes.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef BPR_VCARD_H
#define BPR_VCARD_H

#include <stddef.h>

#define CONTACT_MAX_NUMBERS 6

typedef enum { TEL_OTHER, TEL_MOBILE, TEL_HOME, TEL_WORK } tel_type_t;

typedef struct {
    char name[128];
    int n_numbers;
    struct {
        char number[48];
        tel_type_t type;
    } numbers[CONTACT_MAX_NUMBERS];
} contact_t;

/* Parses all cards in data. Returns the number of contacts (with at least
 * one phone number) and a malloc'ed array in *out (free() it), sorted by
 * name. Returns 0 and *out = NULL when there are none. */
int vcard_parse(const char *data, size_t len, contact_t **out);

/* Digits of a phone number, with a leading '+' kept ("+47 123 45 678" ->
 * "+4712345678"). */
void phone_normalize(const char *in, char *out, size_t out_len);

/* Longest dial string the hands-free stack accepts (BTstack keeps it in a
 * 25-byte buffer and aborts on anything longer). */
#define PHONE_DIAL_MAX 24

/* Turns a number as written ("+47 (123) 45-678", "22 00 00 01 p 12") into
 * a dial string for the phone: digits, '*', '#', a leading '+' and ','
 * (pause; 'p' is written as ','). Spaces, '-', '.', '/' and brackets are
 * dropped. Returns the length, 0 if nothing is left to dial, -1 if the number holds
 * any other character (letters, "ext."), -2 if it is longer than
 * PHONE_DIAL_MAX or out_len - 1. A number is never altered in a way that
 * could reach someone else: anything unclear is refused. */
int phone_dial_string(const char *in, char *out, size_t out_len);

/* Finds the contact owning number. Numbers match when equal after
 * normalising, or when they share the last 8 digits (so "+47 12345678" and
 * "12345678" match). Returns NULL if none matches. */
const contact_t *contacts_lookup(const contact_t *list, int n, const char *number);

#endif
