#ifndef APP_H
#define APP_H

#include <stddef.h>
#include <sys/stat.h>

#define MAX_FILE_SIZE (100 * 1024)
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#define MAX_PATH_LENGTH PATH_MAX
#define IGNORE_FILE ".gitignore"
#define DEFAULT_OUTPUT_FILE "_export.md"
#define DEFAULT_WARN_TOKENS 200000

struct IgnorePattern;

typedef struct {
    int verbose;
    int no_clobber;
    int output_is_stdout;
    int show_tree;
    size_t max_file_size;
    size_t tree_depth;
    size_t warn_tokens;
    size_t max_tokens;
    const char* output_path;
    struct IgnorePattern* ignore_patterns;
    size_t ignore_count;
    struct stat temp_stat;
    struct stat final_stat;
    int have_temp;
    int have_final;
    size_t skipped_binary;
    size_t skipped_too_large;
    size_t skipped_ignored;
    size_t skipped_symlink;
} AppContext;

typedef enum {
    FILE_SELECTION_AUTO = 0,
    FILE_SELECTION_RECURSIVE,
    FILE_SELECTION_GIT_WORKTREE,
    FILE_SELECTION_GIT_STAGED,
    FILE_SELECTION_GIT_UNSTAGED,
    FILE_SELECTION_GIT_DIFF,
    FILE_SELECTION_STDIN
} FileSelectionMode;

#endif
