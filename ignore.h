#ifndef IGNORE_H
#define IGNORE_H

#include <stddef.h>

int is_ignored(const char* filepath, char** patterns, size_t count, int is_dir);
int load_ignore_patterns(const char* ignore_file, char*** patterns, size_t* count);
void free_ignore_patterns(char** patterns, size_t count);

#endif
