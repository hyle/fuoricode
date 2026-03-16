#ifndef IGNORE_H
#define IGNORE_H

#include <stddef.h>

typedef struct IgnorePattern {
    char* match_pattern;
    char* segment_storage;
    char** segment_items;
    size_t segment_count;
    int negated;
    int dir_only;
    int root_anchored;
    int use_path_match;
} IgnorePattern;

int is_ignored(const char* filepath, const IgnorePattern* patterns, size_t count, int is_dir);
int resolve_ignore_state(const char* filepath,
                         const IgnorePattern* patterns,
                         size_t count,
                         int is_dir,
                         int initial_ignored);
int ignored_directory_may_have_included_descendants(const char* dirpath,
                                                    const IgnorePattern* patterns,
                                                    size_t count);
int load_ignore_patterns(const char* ignore_file,
                         int include_default_ignores,
                         IgnorePattern** patterns,
                         size_t* count);
void free_ignore_patterns(IgnorePattern* patterns, size_t count);

#endif
