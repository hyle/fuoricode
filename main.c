#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app.h"
#include "collect.h"
#include "git_paths.h"
#include "ignore.h"
#include "render.h"

#ifndef VERSION
#define VERSION "dev"
#endif

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

static void print_usage(const char* argv0) {
    printf("Usage: %s [OPTIONS]\n", argv0);
    printf("Export codebase to markdown file.\n\n");
    printf("Options:\n");
    printf("  -V, --version       Show version information\n");
    printf("  -v, --verbose       Show progress information\n");
    printf("  -s <size_kb>        Set maximum file size limit in KB (default: 100)\n");
    printf("  -o, --output        Set output path (use '-' for stdout)\n");
    printf("      --staged        Export staged files from the current Git subtree\n");
    printf("      --unstaged      Export unstaged tracked files from the current Git subtree\n");
    printf("      --diff <r>      Export files changed by a git diff range (for example main...HEAD)\n");
    printf("                      Git file-selection flags are mutually exclusive\n");
    printf("      --tree          Include a directory tree section (default)\n");
    printf("      --no-tree       Omit the directory tree section\n");
    printf("      --tree-depth    Limit tree rendering depth to N levels\n");
    printf("      --warn-tokens   Warn if estimated tokens exceed N (default: %d)\n",
           DEFAULT_WARN_TOKENS);
    printf("      --max-tokens    Fail if estimated tokens exceed N\n");
    printf("      --no-clobber    Fail if output file already exists\n");
    printf("  -h, --help          Show this help message\n");
}

static int format_size_with_commas(size_t value, char* buffer, size_t buffer_size) {
    char reversed[64];
    size_t digits = 0;
    size_t used = 0;

    if (!buffer || buffer_size == 0) {
        errno = EINVAL;
        return -1;
    }

    do {
        if (digits >= sizeof(reversed) - 1) {
            errno = EOVERFLOW;
            return -1;
        }
        if (digits > 0 && digits % 3 == 0) {
            reversed[digits++] = ',';
        }
        reversed[digits++] = (char)('0' + (value % 10));
        value /= 10;
    } while (value > 0);

    if (digits + 1 > buffer_size) {
        errno = ENAMETOOLONG;
        return -1;
    }

    while (digits > 0) {
        buffer[used++] = reversed[--digits];
    }
    buffer[used] = '\0';
    return 0;
}

static void print_export_summary(const ExportMetrics* metrics) {
    char files_buf[32];
    char bytes_buf[32];
    char tokens_buf[32];

    if (!metrics) {
        return;
    }

    if (format_size_with_commas(metrics->files_exported, files_buf, sizeof(files_buf)) != 0 ||
        format_size_with_commas(metrics->bytes_written, bytes_buf, sizeof(bytes_buf)) != 0 ||
        format_size_with_commas(metrics->estimated_tokens, tokens_buf, sizeof(tokens_buf)) != 0) {
        fprintf(stderr, "Files exported: %zu\n", metrics->files_exported);
        fprintf(stderr, "Bytes written:  %zu\n", metrics->bytes_written);
        fprintf(stderr, "Est. tokens:    ~%zu  (approx, assuming BPE ~3.5 chars/token)\n",
                metrics->estimated_tokens);
        return;
    }

    fprintf(stderr, "Files exported: %s\n", files_buf);
    fprintf(stderr, "Bytes written:  %s\n", bytes_buf);
    fprintf(stderr, "Est. tokens:    ~%s  (approx, assuming BPE ~3.5 chars/token)\n", tokens_buf);
}

static int make_temp_output_template(const char* output_path, char* tmpl, size_t tmpl_size) {
    char path_copy[MAX_PATH_LENGTH];
    if (!output_path || !tmpl || tmpl_size == 0) {
        errno = EINVAL;
        return -1;
    }

    size_t path_len = strlen(output_path);
    if (path_len >= sizeof(path_copy)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(path_copy, output_path, path_len + 1);

    char* dir = dirname(path_copy);
    if (!dir || dir[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    int written;
    if (strcmp(dir, "/") == 0) {
        written = snprintf(tmpl, tmpl_size, "/.fuori.tmp.XXXXXX");
    } else {
        written = snprintf(tmpl, tmpl_size, "%s/.fuori.tmp.XXXXXX", dir);
    }
    if (written < 0 || (size_t)written >= tmpl_size) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return 0;
}

int main(int argc, char* argv[]) {
    AppContext ctx = {0};
    FileSelectionMode selection_mode = FILE_SELECTION_RECURSIVE;
    const char* diff_range = NULL;
    SelectedPath* selected_paths = NULL;
    size_t selected_count = 0;
    ExportPlan plan = {0};
    ExportMetrics metrics = {0};
    int status = 1;
    int temp_created = 0;
    int output_needs_close = 0;
    FILE* output_file = NULL;
    char temp_output_path[MAX_PATH_LENGTH];

    ctx.max_file_size = MAX_FILE_SIZE;
    ctx.show_tree = 1;
    ctx.tree_depth = SIZE_MAX;
    ctx.warn_tokens = DEFAULT_WARN_TOKENS;
    ctx.output_path = DEFAULT_OUTPUT_FILE;
    temp_output_path[0] = '\0';

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-V") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("fuori %s\n", VERSION);
            return 0;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            ctx.verbose = 1;
        } else if (strcmp(argv[i], "-s") == 0) {
            if (i + 1 < argc) {
                size_t size_kb;
                if (parse_positive_size_value(argv[++i], "size", SIZE_MAX / 1024ULL, &size_kb) != 0) {
                    return 1;
                }
                ctx.max_file_size = size_kb * 1024ULL;
            } else {
                fprintf(stderr, "Missing size value for -s option\n");
                fprintf(stderr, "Use -h or --help for usage information\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) {
            if (i + 1 < argc) {
                ctx.output_path = argv[++i];
                if (ctx.output_path[0] == '\0') {
                    fprintf(stderr, "Invalid output path: empty string\n");
                    return 1;
                }
                ctx.output_is_stdout = (strcmp(ctx.output_path, "-") == 0);
            } else {
                fprintf(stderr, "Missing path value for -o/--output option\n");
                fprintf(stderr, "Use -h or --help for usage information\n");
                return 1;
            }
        } else if (strncmp(argv[i], "--output=", 9) == 0) {
            ctx.output_path = argv[i] + 9;
            if (ctx.output_path[0] == '\0') {
                fprintf(stderr, "Invalid output path: empty string\n");
                return 1;
            }
            ctx.output_is_stdout = (strcmp(ctx.output_path, "-") == 0);
        } else if (strcmp(argv[i], "--no-clobber") == 0) {
            ctx.no_clobber = 1;
        } else if (strcmp(argv[i], "--tree") == 0) {
            ctx.show_tree = 1;
        } else if (strcmp(argv[i], "--no-tree") == 0) {
            ctx.show_tree = 0;
        } else if (strcmp(argv[i], "--tree-depth") == 0) {
            if (i + 1 < argc) {
                if (parse_positive_size_value(argv[++i], "tree depth", SIZE_MAX, &ctx.tree_depth) != 0) {
                    return 1;
                }
            } else {
                fprintf(stderr, "Missing value for --tree-depth option\n");
                fprintf(stderr, "Use -h or --help for usage information\n");
                return 1;
            }
        } else if (strncmp(argv[i], "--tree-depth=", 13) == 0) {
            if (parse_positive_size_value(argv[i] + 13, "tree depth", SIZE_MAX, &ctx.tree_depth) != 0) {
                return 1;
            }
        } else if (strcmp(argv[i], "--warn-tokens") == 0) {
            if (i + 1 < argc) {
                if (parse_positive_size_value(argv[++i], "warn-tokens", SIZE_MAX, &ctx.warn_tokens) != 0) {
                    return 1;
                }
            } else {
                fprintf(stderr, "Missing value for --warn-tokens option\n");
                fprintf(stderr, "Use -h or --help for usage information\n");
                return 1;
            }
        } else if (strncmp(argv[i], "--warn-tokens=", 14) == 0) {
            if (parse_positive_size_value(argv[i] + 14, "warn-tokens", SIZE_MAX, &ctx.warn_tokens) != 0) {
                return 1;
            }
        } else if (strcmp(argv[i], "--max-tokens") == 0) {
            if (i + 1 < argc) {
                if (parse_positive_size_value(argv[++i], "max-tokens", SIZE_MAX, &ctx.max_tokens) != 0) {
                    return 1;
                }
            } else {
                fprintf(stderr, "Missing value for --max-tokens option\n");
                fprintf(stderr, "Use -h or --help for usage information\n");
                return 1;
            }
        } else if (strncmp(argv[i], "--max-tokens=", 13) == 0) {
            if (parse_positive_size_value(argv[i] + 13, "max-tokens", SIZE_MAX, &ctx.max_tokens) != 0) {
                return 1;
            }
        } else if (strcmp(argv[i], "--staged") == 0) {
            if (selection_mode != FILE_SELECTION_RECURSIVE) {
                fprintf(stderr, "File-selection flags are mutually exclusive\n");
                fprintf(stderr, "Use -h or --help for usage information\n");
                return 1;
            }
            selection_mode = FILE_SELECTION_GIT_STAGED;
        } else if (strcmp(argv[i], "--unstaged") == 0) {
            if (selection_mode != FILE_SELECTION_RECURSIVE) {
                fprintf(stderr, "File-selection flags are mutually exclusive\n");
                fprintf(stderr, "Use -h or --help for usage information\n");
                return 1;
            }
            selection_mode = FILE_SELECTION_GIT_UNSTAGED;
        } else if (strcmp(argv[i], "--diff") == 0) {
            if (selection_mode != FILE_SELECTION_RECURSIVE) {
                fprintf(stderr, "File-selection flags are mutually exclusive\n");
                fprintf(stderr, "Use -h or --help for usage information\n");
                return 1;
            }
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing range value for --diff option\n");
                fprintf(stderr, "Use -h or --help for usage information\n");
                return 1;
            }
            diff_range = argv[++i];
            if (diff_range[0] == '\0') {
                fprintf(stderr, "Invalid diff range: empty string\n");
                return 1;
            }
            selection_mode = FILE_SELECTION_GIT_DIFF;
        } else if (strncmp(argv[i], "--diff=", 7) == 0) {
            if (selection_mode != FILE_SELECTION_RECURSIVE) {
                fprintf(stderr, "File-selection flags are mutually exclusive\n");
                fprintf(stderr, "Use -h or --help for usage information\n");
                return 1;
            }
            diff_range = argv[i] + 7;
            if (diff_range[0] == '\0') {
                fprintf(stderr, "Invalid diff range: empty string\n");
                return 1;
            }
            selection_mode = FILE_SELECTION_GIT_DIFF;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            fprintf(stderr, "Use -h or --help for usage information\n");
            return 1;
        }
    }

    if (load_ignore_patterns(IGNORE_FILE, &ctx.ignore_patterns, &ctx.ignore_count) != 0) {
        fprintf(stderr, "Error: Failed to initialize ignore patterns.\n");
        return 1;
    }

    if (selection_mode != FILE_SELECTION_RECURSIVE) {
        if (collect_git_paths(selection_mode, diff_range, &selected_paths, &selected_count) != 0) {
            goto cleanup;
        }
    }

    if (ctx.output_is_stdout) {
        if (fstat(fileno(stdout), &ctx.final_stat) == 0 &&
            S_ISREG(ctx.final_stat.st_mode)) {
            ctx.have_final = 1;
        }
    } else {
        errno = 0;
        if (stat(ctx.output_path, &ctx.final_stat) == 0) {
            ctx.have_final = 1;
            if (ctx.no_clobber) {
                fprintf(stderr, "fuori: output file already exists: %s\n", ctx.output_path);
                goto cleanup;
            }
        } else if (errno != ENOENT) {
            perror("Error checking output path");
            goto cleanup;
        }
    }

    if (selection_mode == FILE_SELECTION_RECURSIVE) {
        if (collect_recursive_export_plan(&ctx, &plan) != 0) {
            fprintf(stderr, "Error collecting directory entries\n");
            goto cleanup;
        }
    } else {
        if (collect_selected_export_plan(selected_paths, selected_count, &ctx, &plan) != 0) {
            fprintf(stderr, "Error collecting selected files\n");
            goto cleanup;
        }
    }

    if (calculate_export_metrics(&plan, selection_mode, ctx.show_tree, ctx.tree_depth, &metrics) != 0) {
        perror("Error calculating export metrics");
        goto cleanup;
    }

    if (ctx.max_tokens > 0 && metrics.estimated_tokens > ctx.max_tokens) {
        char limit_buf[32];
        char estimate_buf[32];

        if (format_size_with_commas(ctx.max_tokens, limit_buf, sizeof(limit_buf)) != 0 ||
            format_size_with_commas(metrics.estimated_tokens, estimate_buf, sizeof(estimate_buf)) != 0) {
            fprintf(stderr,
                    "Error: estimated output is ~%zu tokens, which exceeds --max-tokens %zu. "
                    "Consider using --staged or --diff to narrow scope.\n",
                    metrics.estimated_tokens,
                    ctx.max_tokens);
        } else {
            fprintf(stderr,
                    "Error: estimated output is ~%s tokens, which exceeds --max-tokens %s. "
                    "Consider using --staged or --diff to narrow scope.\n",
                    estimate_buf,
                    limit_buf);
        }
        goto cleanup;
    }

    if (metrics.estimated_tokens > ctx.warn_tokens) {
        char warn_buf[32];

        if (format_size_with_commas(ctx.warn_tokens, warn_buf, sizeof(warn_buf)) != 0) {
            fprintf(stderr,
                    "Warning: output may exceed %zu token context window. "
                    "Consider using --staged or --diff to narrow scope.\n",
                    ctx.warn_tokens);
        } else {
            fprintf(stderr,
                    "Warning: output may exceed %s token context window. "
                    "Consider using --staged or --diff to narrow scope.\n",
                    warn_buf);
        }
    }

    if (ctx.output_is_stdout) {
        output_file = stdout;
    } else {
        if (make_temp_output_template(ctx.output_path, temp_output_path, sizeof(temp_output_path)) != 0) {
            perror("Error creating temporary output path");
            goto cleanup;
        }

        int temp_fd = mkstemp(temp_output_path);
        if (temp_fd == -1) {
            perror("Error creating temporary output file");
            goto cleanup;
        }
        temp_created = 1;

        output_file = fdopen(temp_fd, "w");
        if (!output_file) {
            perror("Error opening temporary output stream");
            close(temp_fd);
            goto cleanup;
        }
        output_needs_close = 1;

        if (fstat(fileno(output_file), &ctx.temp_stat) == -1) {
            perror("fstat on temporary output file");
            goto cleanup;
        }
        ctx.have_temp = 1;
    }

    if (write_export_header(output_file, selection_mode) != 0) {
        perror("Error writing output header");
        goto cleanup;
    }

    if (ctx.show_tree) {
        if (write_project_tree(output_file, &plan, ctx.tree_depth) != 0) {
            perror("Error writing project tree");
            goto cleanup;
        }
    }

    if (render_export_plan(output_file, &plan, ctx.verbose) != 0) {
        fprintf(stderr, "Error processing export files\n");
        goto cleanup;
    }

    if (fflush(output_file) != 0) {
        perror("Error flushing output file");
        goto cleanup;
    }
    if (output_needs_close) {
        if (fclose(output_file) != 0) {
            output_file = NULL;
            perror("Error closing output file");
            goto cleanup;
        }
        output_file = NULL;
        output_needs_close = 0;
    }

    if (!ctx.output_is_stdout) {
        if (rename(temp_output_path, ctx.output_path) == -1) {
            perror("Error moving temporary file to final destination");
            goto cleanup;
        }
        temp_created = 0;
        printf("Codebase exported to %s successfully!\n", ctx.output_path);
    } else {
        fprintf(stderr, "Codebase exported to stdout successfully!\n");
    }

    print_export_summary(&metrics);

    status = 0;

cleanup:
    if (output_needs_close && output_file) {
        fclose(output_file);
    }
    if (temp_created && temp_output_path[0] != '\0') {
        unlink(temp_output_path);
    }
    free_export_plan(&plan);
    free_selected_paths(selected_paths, selected_count);
    free_ignore_patterns(ctx.ignore_patterns, ctx.ignore_count);
    return status;
}
