#ifndef GIT_PATHS_H
#define GIT_PATHS_H

#include <stddef.h>

#include "app.h"

typedef struct {
    char* open_path;
    char* display_path;
} SelectedPath;

typedef enum {
    GIT_PATHS_COLLECTED = 0,
    GIT_PATHS_FALLBACK
} GitPathResult;

int collect_git_paths(FileSelectionMode mode,
                      const char* diff_range,
                      int quiet_probe,
                      SelectedPath** paths_out,
                      size_t* count_out,
                      GitPathResult* result_out);
int collect_stdin_paths(int null_delim,
                        SelectedPath** paths_out,
                        size_t* count_out);
int resolve_repository_name(FileSelectionMode mode, char* buffer, size_t buffer_size);
void free_selected_paths(SelectedPath* paths, size_t count);

#endif
