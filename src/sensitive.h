#ifndef SENSITIVE_H
#define SENSITIVE_H

#include <stddef.h>

int fuori_is_sensitive_filename(const char* filepath);
int fuori_contains_sensitive_content(const unsigned char* buffer, size_t bytes_read);

#endif
