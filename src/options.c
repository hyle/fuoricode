#include "options.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int parse_positive_size_value(const char* value,
                                     const char* label,
                                     size_t max_value,
                                     size_t* out) {
    char* endptr;
    unsigned long long parsed;

    if (!value || !out) {
        errno = EINVAL;
        return -1;
    }

    errno = 0;
    parsed = strtoull(value, &endptr, 10);
    if (*value == '\0' || *endptr != '\0' || parsed == 0 ||
        errno == ERANGE || parsed > max_value) {
        fprintf(stderr, "Invalid %s value: %s\n", label, value);
        fprintf(stderr, "Use -h or --help for usage information\n");
        return -1;
    }

    *out = (size_t)parsed;
    return 0;
}

static void print_selection_mode_conflict(void) {
    fprintf(stderr, "--from-stdin, --staged, --unstaged, and --diff are mutually exclusive\n");
    fprintf(stderr, "Use -h or --help for usage information\n");
}

void init_cli_options(CliOptions* options) {
    if (!options) {
        return;
    }

    memset(options, 0, sizeof(*options));
    options->max_file_size = MAX_FILE_SIZE;
    options->show_tree = 1;
    options->tree_depth = SIZE_MAX;
    options->warn_tokens = DEFAULT_WARN_TOKENS;
    options->output_path = DEFAULT_OUTPUT_FILE;
    options->requested_mode = FILE_SELECTION_AUTO;
    options->resolved_mode = FILE_SELECTION_RECURSIVE;
}

void print_usage(const char* argv0) {
    printf("Usage: %s [OPTIONS]\n", argv0);
    printf("Export codebase to markdown file.\n");
    printf("Default mode uses Git's worktree view when available and falls back to a recursive filesystem walk.\n\n");
    printf("Options:\n");
    printf("  -h, --help          Show this help message\n");
    printf("  -V, --version       Show version information\n");
    printf("  -v, --verbose       Show progress information\n");
    printf("  -o, --output        Set output path (use '-' for stdout)\n");
    printf("      --staged        Export staged files from the current Git subtree\n");
    printf("      --unstaged      Export unstaged tracked files from the current Git subtree\n");
    printf("      --diff <r>      Export files changed by a git diff range (for example main...HEAD)\n");
    printf("      --from-stdin    Read paths from stdin instead of using Git or filesystem selection\n");
    printf("                      --from-stdin, --staged, --unstaged, and --diff are mutually exclusive\n");
    printf("  -0, --null          Use NUL as the input record delimiter instead of newline (requires --from-stdin)\n");
    printf("      --line-numbers  Prefix exported code lines with line numbers\n");
    printf("      --tree          Include a directory tree section (default)\n");
    printf("      --no-tree       Omit the directory tree section\n");
    printf("      --tree-depth    Limit tree rendering depth to N levels\n");
    printf("  -s <size_kb>        Set maximum file size limit in KB (default: 100)\n");
    printf("      --warn-tokens   Warn if estimated tokens exceed N (default: %d)\n",
           DEFAULT_WARN_TOKENS);
    printf("      --max-tokens    Fail if estimated tokens exceed N\n");
    printf("      --no-clobber    Fail if output file already exists\n");
    printf("      --no-git        Force recursive filesystem selection instead of auto Git detection\n");
    printf("      --no-default-ignore Disable built-in default ignore patterns in filesystem mode\n");
    printf("      --allow-sensitive Export files even if they match sensitive-file protection rules\n");
}

int parse_cli_options(int argc, char* argv[], CliOptions* options) {
    int force_no_git = 0;

    if (!options) {
        errno = EINVAL;
        return -1;
    }

    init_cli_options(options);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-V") == 0 || strcmp(argv[i], "--version") == 0) {
            options->show_version = 1;
            return 0;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            options->verbose = 1;
        } else if (strcmp(argv[i], "-s") == 0) {
            if (i + 1 < argc) {
                size_t size_kb;
                if (parse_positive_size_value(argv[++i], "size", SIZE_MAX / 1024ULL, &size_kb) != 0) {
                    return -1;
                }
                options->max_file_size = size_kb * 1024ULL;
            } else {
                fprintf(stderr, "Missing size value for -s option\n");
                fprintf(stderr, "Use -h or --help for usage information\n");
                return -1;
            }
        } else if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) {
            if (i + 1 < argc) {
                options->output_path = argv[++i];
                if (options->output_path[0] == '\0') {
                    fprintf(stderr, "Invalid output path: empty string\n");
                    return -1;
                }
                options->output_is_stdout = (strcmp(options->output_path, "-") == 0);
            } else {
                fprintf(stderr, "Missing path value for -o/--output option\n");
                fprintf(stderr, "Use -h or --help for usage information\n");
                return -1;
            }
        } else if (strncmp(argv[i], "--output=", 9) == 0) {
            options->output_path = argv[i] + 9;
            if (options->output_path[0] == '\0') {
                fprintf(stderr, "Invalid output path: empty string\n");
                return -1;
            }
            options->output_is_stdout = (strcmp(options->output_path, "-") == 0);
        } else if (strcmp(argv[i], "--no-git") == 0) {
            force_no_git = 1;
        } else if (strcmp(argv[i], "--no-default-ignore") == 0) {
            options->no_default_ignore = 1;
        } else if (strcmp(argv[i], "--from-stdin") == 0) {
            if (options->requested_mode != FILE_SELECTION_AUTO) {
                print_selection_mode_conflict();
                return -1;
            }
            options->requested_mode = FILE_SELECTION_STDIN;
        } else if (strcmp(argv[i], "-0") == 0 || strcmp(argv[i], "--null") == 0) {
            options->stdin_null_delim = 1;
        } else if (strcmp(argv[i], "--no-clobber") == 0) {
            options->no_clobber = 1;
        } else if (strcmp(argv[i], "--allow-sensitive") == 0) {
            options->allow_sensitive = 1;
        } else if (strcmp(argv[i], "--tree") == 0) {
            options->show_tree = 1;
        } else if (strcmp(argv[i], "--no-tree") == 0) {
            options->show_tree = 0;
        } else if (strcmp(argv[i], "--line-numbers") == 0) {
            options->show_line_numbers = 1;
        } else if (strcmp(argv[i], "--tree-depth") == 0) {
            if (i + 1 < argc) {
                if (parse_positive_size_value(argv[++i], "tree depth", SIZE_MAX, &options->tree_depth) != 0) {
                    return -1;
                }
            } else {
                fprintf(stderr, "Missing value for --tree-depth option\n");
                fprintf(stderr, "Use -h or --help for usage information\n");
                return -1;
            }
        } else if (strncmp(argv[i], "--tree-depth=", 13) == 0) {
            if (parse_positive_size_value(argv[i] + 13, "tree depth", SIZE_MAX, &options->tree_depth) != 0) {
                return -1;
            }
        } else if (strcmp(argv[i], "--warn-tokens") == 0) {
            if (i + 1 < argc) {
                if (parse_positive_size_value(argv[++i], "warn-tokens", SIZE_MAX, &options->warn_tokens) != 0) {
                    return -1;
                }
            } else {
                fprintf(stderr, "Missing value for --warn-tokens option\n");
                fprintf(stderr, "Use -h or --help for usage information\n");
                return -1;
            }
        } else if (strncmp(argv[i], "--warn-tokens=", 14) == 0) {
            if (parse_positive_size_value(argv[i] + 14, "warn-tokens", SIZE_MAX, &options->warn_tokens) != 0) {
                return -1;
            }
        } else if (strcmp(argv[i], "--max-tokens") == 0) {
            if (i + 1 < argc) {
                if (parse_positive_size_value(argv[++i], "max-tokens", SIZE_MAX, &options->max_tokens) != 0) {
                    return -1;
                }
            } else {
                fprintf(stderr, "Missing value for --max-tokens option\n");
                fprintf(stderr, "Use -h or --help for usage information\n");
                return -1;
            }
        } else if (strncmp(argv[i], "--max-tokens=", 13) == 0) {
            if (parse_positive_size_value(argv[i] + 13, "max-tokens", SIZE_MAX, &options->max_tokens) != 0) {
                return -1;
            }
        } else if (strcmp(argv[i], "--staged") == 0) {
            if (options->requested_mode != FILE_SELECTION_AUTO) {
                print_selection_mode_conflict();
                return -1;
            }
            options->requested_mode = FILE_SELECTION_GIT_STAGED;
        } else if (strcmp(argv[i], "--unstaged") == 0) {
            if (options->requested_mode != FILE_SELECTION_AUTO) {
                print_selection_mode_conflict();
                return -1;
            }
            options->requested_mode = FILE_SELECTION_GIT_UNSTAGED;
        } else if (strcmp(argv[i], "--diff") == 0) {
            if (options->requested_mode != FILE_SELECTION_AUTO) {
                print_selection_mode_conflict();
                return -1;
            }
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing range value for --diff option\n");
                fprintf(stderr, "Use -h or --help for usage information\n");
                return -1;
            }
            options->diff_range = argv[++i];
            if (options->diff_range[0] == '\0') {
                fprintf(stderr, "Invalid diff range: empty string\n");
                return -1;
            }
            options->requested_mode = FILE_SELECTION_GIT_DIFF;
        } else if (strncmp(argv[i], "--diff=", 7) == 0) {
            if (options->requested_mode != FILE_SELECTION_AUTO) {
                print_selection_mode_conflict();
                return -1;
            }
            options->diff_range = argv[i] + 7;
            if (options->diff_range[0] == '\0') {
                fprintf(stderr, "Invalid diff range: empty string\n");
                return -1;
            }
            options->requested_mode = FILE_SELECTION_GIT_DIFF;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            options->show_help = 1;
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            fprintf(stderr, "Use -h or --help for usage information\n");
            return -1;
        }
    }

    if (force_no_git && options->requested_mode != FILE_SELECTION_AUTO) {
        fprintf(stderr, "--no-git cannot be combined with --from-stdin, --staged, --unstaged, or --diff\n");
        fprintf(stderr, "Use -h or --help for usage information\n");
        return -1;
    }

    if (options->stdin_null_delim && options->requested_mode != FILE_SELECTION_STDIN) {
        fprintf(stderr, "-0/--null requires --from-stdin\n");
        fprintf(stderr, "Use -h or --help for usage information\n");
        return -1;
    }

    if (force_no_git) {
        options->requested_mode = FILE_SELECTION_RECURSIVE;
    }

    return 0;
}

int resolve_cli_selection(CliOptions* options,
                          SelectedPath** selected_paths_out,
                          size_t* selected_count_out) {
    GitPathResult git_result = GIT_PATHS_FALLBACK;

    if (!options || !selected_paths_out || !selected_count_out) {
        errno = EINVAL;
        return -1;
    }

    *selected_paths_out = NULL;
    *selected_count_out = 0;

    if (options->requested_mode == FILE_SELECTION_AUTO) {
        if (collect_git_paths(FILE_SELECTION_GIT_WORKTREE,
                              NULL,
                              1,
                              selected_paths_out,
                              selected_count_out,
                              &git_result) != 0) {
            return -1;
        }
        options->resolved_mode = (git_result == GIT_PATHS_COLLECTED) ? FILE_SELECTION_GIT_WORKTREE
                                                                     : FILE_SELECTION_RECURSIVE;
        return 0;
    }

    options->resolved_mode = options->requested_mode;
    if (options->resolved_mode == FILE_SELECTION_RECURSIVE) {
        return 0;
    }

    if (options->resolved_mode == FILE_SELECTION_STDIN) {
        return collect_stdin_paths(options->stdin_null_delim,
                                   selected_paths_out,
                                   selected_count_out);
    }

    return collect_git_paths(options->resolved_mode,
                             options->diff_range,
                             0,
                             selected_paths_out,
                             selected_count_out,
                             &git_result);
}
