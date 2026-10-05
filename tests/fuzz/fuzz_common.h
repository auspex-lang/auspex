#ifndef AUSPEX_FUZZ_COMMON_H
#define AUSPEX_FUZZ_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Entry point shared by libFuzzer and tests/fuzz/fuzz_replay.c. */
int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

/* The front end consumes NUL-terminated source, so fuzz input is copied into
 * a terminated buffer. An embedded NUL simply ends the source early. */
static inline char* fuzz_cstring(const uint8_t* data, size_t size) {
    char* source = malloc(size + 1);
    if (source == NULL) return NULL;
    if (size > 0) memcpy(source, data, size);
    source[size] = '\0';
    return source;
}

/* Invariant violations abort so libFuzzer and the replay driver both report
 * them as crashes. */
#define FUZZ_CHECK(cond, message)                                   \
    do {                                                            \
        if (!(cond)) {                                              \
            fprintf(stderr, "fuzz invariant failed: %s\n", message); \
            abort();                                                \
        }                                                           \
    } while (0)

#endif
