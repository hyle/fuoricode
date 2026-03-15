#include "ignore.h"

#include <errno.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char* storage;
    char** items;
    size_t count;
} PathSegments;

static void free_compiled_segments(IgnorePattern* pattern) {
    if (!pattern) return;
    free(pattern->segment_items);
    free(pattern->segment_storage);
    pattern->segment_items = NULL;
    pattern->segment_storage = NULL;
    pattern->segment_count = 0;
}

static void free_pattern_fields(IgnorePattern* pattern) {
    if (!pattern) return;
    free(pattern->match_pattern);
    pattern->match_pattern = NULL;
    free_compiled_segments(pattern);
    pattern->negated = 0;
    pattern->dir_only = 0;
    pattern->root_anchored = 0;
    pattern->use_path_match = 0;
}

static void cleanup_patterns(IgnorePattern** patterns, size_t count) {
    if (!patterns || !*patterns) return;
    for (size_t i = 0; i < count; i++) {
        free_pattern_fields(&(*patterns)[i]);
    }
    free(*patterns);
    *patterns = NULL;
}

static void free_path_segments(PathSegments* segments) {
    if (!segments) return;
    free(segments->items);
    free(segments->storage);
    segments->items = NULL;
    segments->storage = NULL;
    segments->count = 0;
}

static int split_path_segments(const char* path, PathSegments* segments) {
    char* storage = NULL;
    char** items = NULL;
    size_t count = 0;
    size_t capacity = 0;
    char* cursor;

    if (!path || !segments) {
        errno = EINVAL;
        return -1;
    }

    storage = strdup(path);
    if (!storage) {
        return -1;
    }

    cursor = storage;
    while (*cursor != '\0') {
        while (*cursor == '/') {
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }

        if (count == capacity) {
            size_t new_capacity = (capacity == 0) ? 8 : capacity * 2;
            char** new_items = realloc(items, new_capacity * sizeof(*new_items));
            if (!new_items) {
                free(items);
                free(storage);
                return -1;
            }
            items = new_items;
            capacity = new_capacity;
        }

        items[count++] = cursor;
        while (*cursor != '\0' && *cursor != '/') {
            cursor++;
        }
        if (*cursor == '/') {
            *cursor++ = '\0';
        }
    }

    segments->storage = storage;
    segments->items = items;
    segments->count = count;
    return 0;
}

static int compile_segments(const char* path,
                            char** storage_out,
                            char*** items_out,
                            size_t* count_out) {
    PathSegments segments = {0};

    if (!storage_out || !items_out || !count_out) {
        errno = EINVAL;
        return -1;
    }

    if (split_path_segments(path, &segments) != 0) {
        return -1;
    }

    *storage_out = segments.storage;
    *items_out = segments.items;
    *count_out = segments.count;
    return 0;
}

static int is_double_star_segment(const char* segment) {
    return segment != NULL && strcmp(segment, "**") == 0;
}

static int match_segment_lists(char* const* pattern_items,
                               size_t pattern_count,
                               size_t pattern_index,
                               const PathSegments* path_segments,
                               size_t path_index) {
    while (pattern_index < pattern_count) {
        const char* pattern = pattern_items[pattern_index];

        if (is_double_star_segment(pattern)) {
            while (pattern_index + 1 < pattern_count &&
                   is_double_star_segment(pattern_items[pattern_index + 1])) {
                pattern_index++;
            }

            if (pattern_index + 1 == pattern_count) {
                return 1;
            }

            for (size_t i = path_index; i <= path_segments->count; i++) {
                if (match_segment_lists(pattern_items, pattern_count, pattern_index + 1, path_segments, i)) {
                    return 1;
                }
            }
            return 0;
        }

        if (path_index >= path_segments->count) {
            return 0;
        }

        if (fnmatch(pattern, path_segments->items[path_index], 0) != 0) {
            return 0;
        }

        pattern_index++;
        path_index++;
    }

    return path_index == path_segments->count;
}

static int compile_ignore_pattern(const char* raw_pattern, IgnorePattern* pattern) {
    const char* source = raw_pattern;
    size_t length;
    size_t normalized_start;
    size_t normalized_len;

    if (!raw_pattern || !pattern) {
        errno = EINVAL;
        return -1;
    }

    memset(pattern, 0, sizeof(*pattern));

    if (source[0] == '!') {
        pattern->negated = 1;
        source++;
    }

    length = strlen(source);
    if (length == 0) {
        errno = EINVAL;
        return -1;
    }

    pattern->dir_only = (source[length - 1] == '/');
    pattern->root_anchored = (source[0] == '/');
    normalized_start = pattern->root_anchored ? 1 : 0;
    normalized_len = length - normalized_start - (pattern->dir_only ? 1 : 0);
    if (normalized_len == 0) {
        errno = EINVAL;
        return -1;
    }

    pattern->match_pattern = malloc(normalized_len + 1);
    if (!pattern->match_pattern) {
        return -1;
    }
    memcpy(pattern->match_pattern, source + normalized_start, normalized_len);
    pattern->match_pattern[normalized_len] = '\0';

    pattern->use_path_match = (pattern->root_anchored || strchr(pattern->match_pattern, '/') != NULL);
    if (!pattern->use_path_match) {
        return 0;
    }

    if (compile_segments(pattern->match_pattern,
                         &pattern->segment_storage,
                         &pattern->segment_items,
                         &pattern->segment_count) != 0) {
        free_pattern_fields(pattern);
        return -1;
    }

    return 0;
}

int resolve_ignore_state(const char* filepath,
                         const IgnorePattern* patterns,
                         size_t count,
                         int is_dir,
                         int initial_ignored) {
    if (!patterns) return 0;

    const char* rel = (strncmp(filepath, "./", 2) == 0) ? filepath + 2 : filepath;
    const char* base = strrchr(rel, '/');
    base = (base) ? base + 1 : rel;

    PathSegments rel_segments = {0};
    int rel_segments_ready = 0;
    int ignored = initial_ignored;
    for (size_t i = 0; i < count; i++) {
        const IgnorePattern* pattern = &patterns[i];

        if (pattern->dir_only && !is_dir) {
            continue;
        }

        if (!pattern->use_path_match) {
            if (fnmatch(pattern->match_pattern, base, 0) == 0) {
                ignored = !pattern->negated;
            }
            continue;
        }

        if (!rel_segments_ready) {
            if (split_path_segments(rel, &rel_segments) != 0) {
                return 1;
            }
            rel_segments_ready = 1;
        }

        if (match_segment_lists(pattern->segment_items,
                                pattern->segment_count,
                                0,
                                &rel_segments,
                                0)) {
            ignored = !pattern->negated;
        }
    }

    free_path_segments(&rel_segments);
    return ignored;
}

int is_ignored(const char* filepath, const IgnorePattern* patterns, size_t count, int is_dir) {
    return resolve_ignore_state(filepath, patterns, count, is_dir, 0);
}

int load_ignore_patterns(const char* ignore_file, IgnorePattern** patterns, size_t* count) {
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
    *patterns = calloc(capacity, sizeof(**patterns));
    if (!*patterns) {
        return -1;
    }
    *count = 0;

    for (size_t i = 0; i < default_count; i++) {
        if (compile_ignore_pattern(default_ignores[i], &(*patterns)[*count]) != 0) {
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
            IgnorePattern* new_patterns = realloc(*patterns, capacity * sizeof(*new_patterns));
            if (!new_patterns) {
                free(line);
                fclose(file);
                cleanup_patterns(patterns, *count);
                *count = 0;
                return -1;
            }
            *patterns = new_patterns;
        }

        if (compile_ignore_pattern(line, &(*patterns)[*count]) != 0) {
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

void free_ignore_patterns(IgnorePattern* patterns, size_t count) {
    if (!patterns) return;
    for (size_t i = 0; i < count; i++) {
        free_pattern_fields(&patterns[i]);
    }
    free(patterns);
}
