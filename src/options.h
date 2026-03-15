#ifndef OPTIONS_H
#define OPTIONS_H

#include "app.h"
#include "git_paths.h"

typedef struct {
    int show_help;
    int show_version;
    int verbose;
    int no_clobber;
    int output_is_stdout;
    int show_tree;
    size_t max_file_size;
    size_t tree_depth;
    size_t warn_tokens;
    size_t max_tokens;
    const char* output_path;
    const char* diff_range;
    FileSelectionMode requested_mode;
    FileSelectionMode resolved_mode;
} CliOptions;

void init_cli_options(CliOptions* options);
void print_usage(const char* argv0);
int parse_cli_options(int argc, char* argv[], CliOptions* options);
int resolve_cli_selection(CliOptions* options,
                          SelectedPath** selected_paths_out,
                          size_t* selected_count_out);

#endif
