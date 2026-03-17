#ifndef GIT_PATHS_H
#define GIT_PATHS_H

#include <stddef.h>

#include "app.h"

typedef enum {
    SELECTED_PATH_CHANGE_NONE = 0,
    SELECTED_PATH_CHANGE_ADDED,
    SELECTED_PATH_CHANGE_MODIFIED,
    SELECTED_PATH_CHANGE_RENAMED
} SelectedPathChangeType;

typedef struct {
    char* open_path;
    char* display_path;
    char* repo_rel_path;
    SelectedPathChangeType change_type;
    char* previous_display_path;
    char* previous_repo_rel_path;
} SelectedPath;

typedef struct {
    size_t new_start;
    size_t new_count;
} GitHunkRange;

typedef struct {
    GitHunkRange* ranges;
    size_t count;
} GitFileHunks;

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
int collect_git_hunks(FileSelectionMode mode,
                      const char* diff_range,
                      const SelectedPath* paths,
                      size_t path_count,
                      GitFileHunks** hunks_out,
                      size_t* hunk_count_out);
int resolve_repository_name(FileSelectionMode mode, char* buffer, size_t buffer_size);
void free_git_hunks(GitFileHunks* hunks, size_t count);
void free_selected_paths(SelectedPath* paths, size_t count);

#endif
