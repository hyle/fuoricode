#ifndef GIT_PATHS_H
#define GIT_PATHS_H

#include <stddef.h>

#include "app.h"

typedef struct {
    char* open_path;
    char* display_path;
} SelectedPath;

int collect_git_paths(FileSelectionMode mode,
                      const char* diff_range,
                      SelectedPath** paths_out,
                      size_t* count_out);
void free_selected_paths(SelectedPath* paths, size_t count);

#endif
