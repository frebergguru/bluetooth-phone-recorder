/*
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "vcard.h"

#include <ctype.h>
#include <iconv.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decode a value in place: quoted-printable (if qp) and vCard escapes. */
static void decode_value(char *s, bool qp)
{
    char *w = s;
    for (char *r = s; *r; r++) {
        if (qp && *r == '=' && hexval(r[1]) >= 0 && hexval(r[2]) >= 0) {
            *w++ = (char)(hexval(r[1]) * 16 + hexval(r[2]));
            r += 2;
        } else if (!qp && *r == '\\' && r[1]) {
            r++;
            *w++ = (*r == 'n' || *r == 'N') ? ' ' : *r;
        } else {
            *w++ = *r;
        }
    }
    *w = 0;
}

static bool valid_utf8(const unsigned char *s)
{
    while (*s) {
        int extra = *s < 0x80 ? 0 : (*s & 0xE0) == 0xC0 ? 1 : (*s & 0xF0) == 0xE0 ? 2
                    : (*s & 0xF8) == 0xF0 ? 3 : -1;
        if (extra < 0) return false;
        s++;
        for (int i = 0; i < extra; i++, s++)
            if ((*s & 0xC0) != 0x80) return false;
    }
    return true;
}

/* Older phones send names in a legacy character set (CHARSET=ISO-8859-1 or
 * none at all). Convert to UTF-8 in place; anything not valid UTF-8
 * without a charset is taken as Latin-1. */
static void to_utf8(char *s, size_t cap, const char *params)
{
    char charset[32] = "";
    const char *cs = strstr(params, "CHARSET=");
    if (cs) {
        cs += 8;
        size_t j = 0;
        while (cs[j] && cs[j] != ';' && cs[j] != ',' && j + 1 < sizeof charset) {
            charset[j] = cs[j];
            j++;
        }
        charset[j] = 0;
    }
    bool utf8 = charset[0] ? strcasecmp(charset, "UTF-8") == 0 || strcasecmp(charset, "UTF8") == 0
                           : valid_utf8((const unsigned char *)s);
    if (utf8) return;
    iconv_t cd = iconv_open("UTF-8", charset[0] ? charset : "ISO-8859-1");
    if (cd == (iconv_t)-1) cd = iconv_open("UTF-8", "ISO-8859-1");
    if (cd == (iconv_t)-1) return;
    char out[512];
    char *in = s, *op = out;
    size_t il = strlen(s), ol = sizeof out - 1;
    if (iconv(cd, &in, &il, &op, &ol) != (size_t)-1) {
        *op = 0;
        snprintf(s, cap, "%s", out);
    }
    iconv_close(cd);
}

static void trim(char *s)
{
    size_t l = strlen(s);
    while (l && isspace((unsigned char)s[l - 1])) s[--l] = 0;
    size_t i = 0;
    while (s[i] && isspace((unsigned char)s[i])) i++;
    if (i) memmove(s, s + i, l - i + 1);
}

/* Name from N (family;given;middle;prefix;suffix) as "given middle family". */
static void name_from_n(const char *value, char *out, size_t len)
{
    char parts[5][128] = {{0}};
    int p = 0;
    size_t j = 0;
    for (const char *c = value; *c && p < 5; c++) {
        if (*c == ';') {
            parts[p][j] = 0;
            p++;
            j = 0;
        } else if (j + 1 < sizeof parts[0]) {
            parts[p][j++] = *c;
        }
    }
    out[0] = 0;
    const int order[] = {3, 1, 2, 0, 4}; /* prefix given middle family suffix */
    for (int k = 0; k < 5; k++) {
        char *v = parts[order[k]];
        trim(v);
        if (!*v) continue;
        size_t ol = strlen(out);
        snprintf(out + ol, len - ol, "%s%s", ol ? " " : "", v);
    }
}

static tel_type_t tel_type(const char *params)
{
    /* params is upper-cased already */
    if (strstr(params, "CELL") || strstr(params, "MOBILE")) return TEL_MOBILE;
    if (strstr(params, "HOME")) return TEL_HOME;
    if (strstr(params, "WORK")) return TEL_WORK;
    return TEL_OTHER;
}

static int cmp_contact(const void *a, const void *b)
{
    return strcasecmp(((const contact_t *)a)->name, ((const contact_t *)b)->name);
}

int vcard_parse(const char *data, size_t len, contact_t **out)
{
    *out = NULL;
    if (!data || !len) return 0;

    /* unfold: copy to a buffer, joining continuation lines (vCard 3.0 starts
     * them with a space or tab; vCard 2.1 quoted-printable ends with '=') */
    char *buf = malloc(len + 1);
    if (!buf) return 0;
    size_t w = 0;
    for (size_t i = 0; i < len; i++) {
        char c = data[i];
        if (c == '\r') continue;
        if (c == '\n') {
            if (i + 1 < len && (data[i + 1] == ' ' || data[i + 1] == '\t')) {
                i++; /* folded line: drop the newline and the single space */
                continue;
            }
            if (w > 0 && buf[w - 1] == '=') { /* QP soft line break */
                w--;
                continue;
            }
        }
        buf[w++] = c;
    }
    buf[w] = 0;

    int cap = 64, n = 0;
    contact_t *list = malloc((size_t)cap * sizeof *list);
    if (!list) {
        free(buf);
        return 0;
    }
    contact_t cur;
    bool in_card = false;
    char fn[128] = "", nname[128] = "";

    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = 0;
        char *key = line, *value = colon + 1;
        /* property name and parameters, upper-cased for matching */
        char upper[256];
        snprintf(upper, sizeof upper, "%s", key);
        for (char *u = upper; *u; u++) *u = (char)toupper((unsigned char)*u);
        char *semi = strchr(upper, ';');
        const char *params = semi ? semi + 1 : "";
        if (semi) *semi = 0;
        /* strip a "group." prefix (item1.TEL) */
        char *prop = strrchr(upper, '.') ? strrchr(upper, '.') + 1 : upper;
        bool qp = strstr(params, "QUOTED-PRINTABLE") != NULL;

        if (strcmp(prop, "BEGIN") == 0 && strncasecmp(value, "VCARD", 5) == 0) {
            memset(&cur, 0, sizeof cur);
            fn[0] = nname[0] = 0;
            in_card = true;
        } else if (!in_card) {
            continue;
        } else if (strcmp(prop, "FN") == 0) {
            /* decode in a separate buffer: UTF-8 can be longer than the
             * legacy encoding it was converted from */
            char tmp[512];
            snprintf(tmp, sizeof tmp, "%s", value);
            decode_value(tmp, qp);
            to_utf8(tmp, sizeof tmp, params);
            snprintf(fn, sizeof fn, "%s", tmp);
            trim(fn);
        } else if (strcmp(prop, "N") == 0) {
            char tmp[512];
            snprintf(tmp, sizeof tmp, "%s", value);
            decode_value(tmp, qp);
            to_utf8(tmp, sizeof tmp, params);
            name_from_n(tmp, nname, sizeof nname);
        } else if (strcmp(prop, "TEL") == 0 && cur.n_numbers < CONTACT_MAX_NUMBERS) {
            decode_value(value, qp);
            trim(value);
            if (*value) {
                snprintf(cur.numbers[cur.n_numbers].number, sizeof cur.numbers[0].number, "%s", value);
                cur.numbers[cur.n_numbers].type = tel_type(params);
                cur.n_numbers++;
            }
        } else if (strcmp(prop, "END") == 0 && strncasecmp(value, "VCARD", 5) == 0) {
            in_card = false;
            if (cur.n_numbers == 0) continue; /* nothing to call */
            snprintf(cur.name, sizeof cur.name, "%s", fn[0] ? fn : nname[0] ? nname : cur.numbers[0].number);
            if (n == cap) {
                cap *= 2;
                contact_t *g = realloc(list, (size_t)cap * sizeof *list);
                if (!g) break;
                list = g;
            }
            list[n++] = cur;
        }
    }
    free(buf);
    if (n == 0) {
        free(list);
        return 0;
    }
    qsort(list, (size_t)n, sizeof *list, cmp_contact);
    *out = list;
    return n;
}

void phone_normalize(const char *in, char *out, size_t out_len)
{
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 1 < out_len; i++) {
        if (isdigit((unsigned char)in[i]))
            out[j++] = in[i];
        else if (in[i] == '+' && j == 0)
            out[j++] = '+';
    }
    out[j] = 0;
}

static const char *last_digits(const char *norm, size_t k)
{
    size_t l = strlen(norm);
    return l > k ? norm + l - k : norm;
}

const contact_t *contacts_lookup(const contact_t *list, int n, const char *number)
{
    if (!number || !*number) return NULL;
    char want[64];
    phone_normalize(number, want, sizeof want);
    size_t wl = strlen(want);
    if (wl < 3) return NULL;
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < list[i].n_numbers; k++) {
            char have[64];
            phone_normalize(list[i].numbers[k].number, have, sizeof have);
            if (strcmp(have, want) == 0) return &list[i];
            /* same last 8 digits (country code written or not) */
            if (wl >= 8 && strlen(have) >= 8 &&
                strcmp(last_digits(have, 8), last_digits(want, 8)) == 0)
                return &list[i];
        }
    }
    return NULL;
}
