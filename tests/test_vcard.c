/*
 * vCard reader: 2.1 with quoted-printable (Android), 3.0 with folding and
 * escapes, contacts without numbers skipped, number matching.
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
    contact_t *none = (contact_t *)1;
    CHECK(vcard_parse("", 0, &none) == 0 && none == NULL);
    TEST_DONE();
}
