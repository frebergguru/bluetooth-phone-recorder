/*
 * Minimal test helpers.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef BPR_TESTUTIL_H
#define BPR_TESTUTIL_H

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

static int test_failures;

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            test_failures++;                                                       \
        }                                                                          \
    } while (0)

#define TEST_DONE()                                                   \
    do {                                                              \
        if (test_failures) fprintf(stderr, "%d failure(s)\n", test_failures); \
        return test_failures ? EXIT_FAILURE : EXIT_SUCCESS;           \
    } while (0)

#endif
