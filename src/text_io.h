#ifndef TEXT_IO_H
#define TEXT_IO_H

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static inline int fuori_write_text(FILE* out, const char* text) {
    return (fputs(text, out) == EOF) ? -1 : 0;
}

static inline int fuori_count_text_bytes(size_t* total, const char* text) {
    size_t text_len = strlen(text);
    if (*total > SIZE_MAX - text_len) {
        errno = EOVERFLOW;
        return -1;
    }
    *total += text_len;
    return 0;
}

#endif
