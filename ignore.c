#include "ignore.h"

#include <errno.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void cleanup_patterns(char*** patterns, size_t count) {
    if (!patterns || !*patterns) return;
    for (size_t i = 0; i < count; i++) {
        free((*patterns)[i]);
    }
    free(*patterns);
    *patterns = NULL;
}

int is_ignored(const char* filepath, char** patterns, size_t count, int is_dir) {
    if (!patterns) return 0;

    const char* rel = (strncmp(filepath, "./", 2) == 0) ? filepath + 2 : filepath;
    const char* base = strrchr(rel, '/');
    base = (base) ? base + 1 : rel;

    char* scratch = NULL;
    size_t scratch_cap = 0;
    int ignored = 0;
    for (size_t i = 0; i < count; i++) {
        char* raw_pattern = patterns[i];
        size_t plen = strlen(raw_pattern);
        if (plen == 0) continue;

        int negated = 0;
        const char* pattern = raw_pattern;
        if (pattern[0] == '!') {
            negated = 1;
            pattern++;
            plen--;
            if (plen == 0) continue;
        }

        int pattern_is_dir_only = (pattern[plen - 1] == '/');
        if (pattern_is_dir_only && !is_dir) {
            continue;
        }

        const char* pat = pattern;
        if (pattern_is_dir_only) {
            if (plen > scratch_cap) {
                char* new_scratch = realloc(scratch, plen);
                if (!new_scratch) {
                    free(scratch);
                    // Fail closed: if we cannot evaluate ignore patterns, exclude the path.
                    return 1;
                }
                scratch = new_scratch;
                scratch_cap = plen;
            }
            memcpy(scratch, pattern, plen - 1);
            scratch[plen - 1] = '\0';
            pat = scratch;
        }
        int has_slash = (strchr(pat, '/') != NULL);
        if (has_slash) {
            const char* match_pat = pat;
            const char* match_target = rel;
            if (pat[0] == '/') {
                match_pat = pat + 1;
            }
            if (fnmatch(match_pat, match_target, FNM_PATHNAME) == 0) {
                ignored = !negated;
            }
        } else if (fnmatch(pat, base, FNM_PATHNAME) == 0) {
            ignored = !negated;
        }
    }

    free(scratch);
    return ignored;
}

int load_ignore_patterns(const char* ignore_file, char*** patterns, size_t* count) {
    const char* default_ignores[] = {
        ".git/",
        "node_modules/",
        "build/",
        "dist/",
        "bin/",
        ".env",
        ".venv/",
        "__pycache__/",
        ".DS_Store",
        "*.o",
        "*.a",
        "*.so",
        "*.exe",
        "*.dll",
        "*.log",
        NULL
    };

    size_t default_count = 0;
    while (default_ignores[default_count] != NULL) {
        default_count++;
    }

    size_t capacity = default_count + 16;
    *patterns = malloc(capacity * sizeof(char*));
    if (!*patterns) {
        return -1;
    }
    *count = 0;

    for (size_t i = 0; i < default_count; i++) {
        (*patterns)[*count] = strdup(default_ignores[i]);
        if (!(*patterns)[*count]) {
            cleanup_patterns(patterns, *count);
            return -1;
        }
        (*count)++;
    }

    FILE* file = fopen(ignore_file, "r");
    if (!file) {
        if (errno == ENOENT) {
            return 0;  // Defaults only
        }
        cleanup_patterns(patterns, *count);
        *count = 0;
        return -1;
    }

    char* line = NULL;
    size_t line_cap = 0;
    ssize_t line_len = 0;
    while ((line_len = getline(&line, &line_cap, file)) != -1) {
        while (line_len > 0 &&
               (line[line_len - 1] == '\n' || line[line_len - 1] == '\r' ||
                line[line_len - 1] == ' ' || line[line_len - 1] == '\t')) {
            line[--line_len] = '\0';
        }

        if (line_len == 0 || line[0] == '#') {
            continue;
        }

        if (*count >= capacity) {
            capacity *= 2;
            char** new_patterns = realloc(*patterns, capacity * sizeof(char*));
            if (!new_patterns) {
                free(line);
                fclose(file);
                cleanup_patterns(patterns, *count);
                *count = 0;
                return -1;
            }
            *patterns = new_patterns;
        }

        (*patterns)[*count] = strdup(line);
        if (!(*patterns)[*count]) {
            free(line);
            fclose(file);
            cleanup_patterns(patterns, *count);
            *count = 0;
            return -1;
        }

        (*count)++;
    }

    if (ferror(file)) {
        free(line);
        fclose(file);
        cleanup_patterns(patterns, *count);
        *count = 0;
        return -1;
    }

    free(line);
    if (fclose(file) != 0) {
        cleanup_patterns(patterns, *count);
        *count = 0;
        return -1;
    }
    return 0;
}

void free_ignore_patterns(char** patterns, size_t count) {
    if (!patterns) return;
    for (size_t i = 0; i < count; i++) {
        free(patterns[i]);
    }
    free(patterns);
}
