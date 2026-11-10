/*
 * vCard reader: 2.1 with quoted-printable (Android), 3.0 with folding and
 * escapes, contacts without numbers skipped, number matching, base64 photos,
 * names always valid UTF-8, and cleaning numbers into dial strings.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <stdlib.h>
#include <string.h>

#include "core/vcard.h"
#include "testutil.h"

static const char *cards =
    "BEGIN:VCARD\r\n"
    "VERSION:2.1\r\n"
    "N;CHARSET=UTF-8;ENCODING=QUOTED-PRINTABLE:=C3=98stby;=C3=85se;;;\r\n"
    "FN;CHARSET=UTF-8;ENCODING=QUOTED-PRINTABLE:=C3=85se =C3=98stby\r\n"
    "TEL;CELL:+47 123 45 678\r\n"
    "TEL;HOME:22 33 44 55\r\n"
    "END:VCARD\r\n"
    "BEGIN:VCARD\r\n"
    "VERSION:3.0\r\n"
    "N:Nordmann;Kari;;;\r\n"
    "TEL;TYPE=WORK,VOICE:+4799887766\r\n"
    "NOTE:a long note that is\r\n"
    "  folded onto a second line\r\n"
    "END:VCARD\r\n"
    "BEGIN:VCARD\r\n"
    "VERSION:3.0\r\n"
    "FN:No Number\\, Inc.\r\n"
    "EMAIL:x@example.org\r\n"
    "END:VCARD\r\n"
    "BEGIN:VCARD\r\n"
    "VERSION:2.1\r\n"
    "FN;ENCODING=QUOTED-PRINTABLE;CHARSET=UTF-8:Bj=C3=B8rn=20=\r\n"
    "Bakke\r\n"
    "item1.TEL;TYPE=CELL:(555) 010-9999\r\n"
    "END:VCARD\r\n"
    "BEGIN:VCARD\r\n"
    "VERSION:3.0\r\n"
    "TEL:+1 555 0100\r\n"
    "END:VCARD\r\n"
    /* old phone: Latin-1, once declared and once undeclared */
    "BEGIN:VCARD\r\n"
    "VERSION:2.1\r\n"
    "FN;CHARSET=ISO-8859-1:S\xf8lvi Ring\r\n"
    "TEL:11223344\r\n"
    "END:VCARD\r\n"
    "BEGIN:VCARD\r\n"
    "VERSION:2.1\r\n"
    "N:Bl\xe5" "b\xe6r;Per\r\n"
    "TEL:55667788\r\n"
    "END:VCARD\r\n";

static bool utf8_ok(const unsigned char *s)
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

static const contact_t *find(const contact_t *l, int n, const char *name)
{
    for (int i = 0; i < n; i++)
        if (strcmp(l[i].name, name) == 0) return &l[i];
    return NULL;
}

int main(void)
{
    contact_t *list = NULL;
    int n = vcard_parse(cards, strlen(cards), &list);
    CHECK(n == 6); /* the card without a number is skipped */

    const contact_t *ase = find(list, n, "\xc3\x85se \xc3\x98stby"); /* Åse Østby */
    CHECK(ase != NULL);
    if (ase) {
        CHECK(ase->n_numbers == 2);
        CHECK(ase->numbers[0].type == TEL_MOBILE);
        CHECK(ase->numbers[1].type == TEL_HOME);
    }
    const contact_t *kari = find(list, n, "Kari Nordmann"); /* name built from N */
    CHECK(kari != NULL);
    if (kari) CHECK(kari->numbers[0].type == TEL_WORK);
    CHECK(find(list, n, "Bj\xc3\xb8rn Bakke") != NULL); /* QP soft line break */
    CHECK(find(list, n, "+1 555 0100") != NULL);         /* no name: number used */
    CHECK(find(list, n, "S\xc3\xb8lvi Ring") != NULL);        /* ISO-8859-1 declared */
    CHECK(find(list, n, "Per Bl\xc3\xa5" "b\xc3\xa6r") != NULL); /* Latin-1, undeclared */

    /* sorted by name */
    for (int i = 1; i < n; i++) CHECK(strcasecmp(list[i - 1].name, list[i].name) <= 0);

    /* number matching */
    char norm[64];
    phone_normalize("+47 (123) 45-678", norm, sizeof norm);
    CHECK(strcmp(norm, "+4712345678") == 0);
    CHECK(contacts_lookup(list, n, "+4712345678") == ase);
    CHECK(contacts_lookup(list, n, "12345678") == ase); /* without country code */
    CHECK(contacts_lookup(list, n, "004799887766") == kari);
    CHECK(contacts_lookup(list, n, "5550109999") != NULL);
    CHECK(contacts_lookup(list, n, "11111111") == NULL);
    CHECK(contacts_lookup(list, n, "") == NULL);

    free(list);

    /* A base64 PHOTO ending in '=' is not a quoted-printable soft line
     * break: the END:VCARD after it must still close the card. */
    static const char *photo =
        "BEGIN:VCARD\r\n"
        "VERSION:3.0\r\n"
        "FN:Photo Person\r\n"
        "TEL:+4740000001\r\n"
        "PHOTO;ENCODING=b;TYPE=JPEG:AAAA\r\n"
        " BBB=\r\n"
        "END:VCARD\r\n"
        "BEGIN:VCARD\r\n"
        "VERSION:3.0\r\n"
        "FN:Next Person\r\n"
        "TEL:+4740000002\r\n"
        "END:VCARD\r\n";
    n = vcard_parse(photo, strlen(photo), &list);
    CHECK(n == 2);
    CHECK(find(list, n, "Photo Person") != NULL);
    CHECK(find(list, n, "Next Person") != NULL);
    free(list);

    /* A name longer than the field is cut between characters, never inside
     * one (GTK and GKeyFile reject broken UTF-8). */
    char longcard[1024];
    char longname[400] = "";
    for (int i = 0; i < 100; i++) strcat(longname, "\xc3\x98"); /* 100 x U+00D8, 200 bytes */
    snprintf(longcard, sizeof longcard,
             "BEGIN:VCARD\nVERSION:3.0\nFN:AB%s\nTEL:123456\nEND:VCARD\n"
             "BEGIN:VCARD\nVERSION:3.0\nN:B%s;C;;;\nTEL:654321\nEND:VCARD\n",
             longname, longname);
    n = vcard_parse(longcard, strlen(longcard), &list);
    CHECK(n == 2);
    for (int i = 0; i < n; i++) {
        const unsigned char *u = (const unsigned char *)list[i].name;
        size_t l = strlen(list[i].name);
        CHECK(l > 100 && l < sizeof list[i].name);
        CHECK(utf8_ok(u));
    }
    free(list);

    /* a value declared UTF-8 that is not: still valid text afterwards */
    static const char *broken = "BEGIN:VCARD\nVERSION:3.0\nFN;CHARSET=UTF-8:Ab\xff\xfe\nTEL:777\nEND:VCARD\n";
    n = vcard_parse(broken, strlen(broken), &list);
    CHECK(n == 1 && utf8_ok((const unsigned char *)list[0].name) && strncmp(list[0].name, "Ab", 2) == 0);
    free(list);

    /* dial strings */
    char d[PHONE_DIAL_MAX + 1];
    CHECK(phone_dial_string("+47 (123) 45-678", d, sizeof d) == 11 && strcmp(d, "+4712345678") == 0);
    CHECK(phone_dial_string("22 00 00 01 p 12", d, sizeof d) > 0 && strcmp(d, "22000001,12") == 0);
    CHECK(phone_dial_string("*#06#", d, sizeof d) == 5);
    CHECK(phone_dial_string("12345678 ext. 12", d, sizeof d) == -1 && d[0] == 0);
    CHECK(phone_dial_string("123\rAT+CHUP", d, sizeof d) == -1);
    CHECK(phone_dial_string("12+34", d, sizeof d) == -1); /* '+' only at the start */
    CHECK(phone_dial_string("  - ", d, sizeof d) == 0);
    CHECK(phone_dial_string("+", d, sizeof d) == 0);
    CHECK(phone_dial_string("1234567890123456789012345", d, sizeof d) == -2); /* 25 digits */
    CHECK(phone_dial_string("123456789012345678901234", d, sizeof d) == 24);

    contact_t *none = (contact_t *)1;
    CHECK(vcard_parse("", 0, &none) == 0 && none == NULL);
    TEST_DONE();
}
