#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "app.h"
#include "collect.h"
#include "ignore.h"
#include "options.h"
#include "render.h"
#include "tree.h"

#ifndef VERSION
#define VERSION "dev"
#endif

static int format_size_with_commas(size_t value, char* buffer, size_t buffer_size) {
    char digits_buf[64];
    size_t digit_count = 0;
    size_t comma_count = 0;
    size_t total_len = 0;
    size_t used = 0;

    if (!buffer || buffer_size == 0) {
        errno = EINVAL;
        return -1;
    }

    do {
        if (digit_count >= sizeof(digits_buf) - 1) {
            errno = EOVERFLOW;
            return -1;
        }
        digits_buf[digit_count++] = (char)('0' + (value % 10));
        value /= 10;
    } while (value > 0);

    comma_count = (digit_count > 0) ? (digit_count - 1) / 3 : 0;
    total_len = digit_count + comma_count;
    if (total_len + 1 > buffer_size) {
        errno = ENAMETOOLONG;
        return -1;
    }

    for (size_t i = digit_count; i > 0; i--) {
        buffer[used++] = digits_buf[i - 1];
        if (i > 1 && (i - 1) % 3 == 0) {
            buffer[used++] = ',';
        }
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

static void print_verbose_skip_summary(const AppContext* ctx) {
    char binary_buf[32];
    char large_buf[32];
    char ignored_buf[32];
    char symlink_buf[32];
    char sensitive_buf[32];

    if (!ctx || !ctx->verbose) {
        return;
    }

    if (format_size_with_commas(ctx->skipped_binary, binary_buf, sizeof(binary_buf)) != 0 ||
        format_size_with_commas(ctx->skipped_too_large, large_buf, sizeof(large_buf)) != 0 ||
        format_size_with_commas(ctx->skipped_ignored, ignored_buf, sizeof(ignored_buf)) != 0 ||
        format_size_with_commas(ctx->skipped_symlink, symlink_buf, sizeof(symlink_buf)) != 0 ||
        format_size_with_commas(ctx->skipped_sensitive, sensitive_buf, sizeof(sensitive_buf)) != 0) {
        fprintf(stderr,
                "Skipped: binary/empty=%zu, too_large=%zu, ignored=%zu, symlink=%zu, sensitive=%zu\n",
                ctx->skipped_binary,
                ctx->skipped_too_large,
                ctx->skipped_ignored,
                ctx->skipped_symlink,
                ctx->skipped_sensitive);
        return;
    }

    fprintf(stderr,
            "Skipped: binary/empty=%s, too_large=%s, ignored=%s, symlink=%s, sensitive=%s\n",
            binary_buf,
            large_buf,
            ignored_buf,
            symlink_buf,
            sensitive_buf);
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

static int format_generated_timestamp(char* buffer, size_t buffer_size) {
    time_t now;
    struct tm utc_tm;

    if (!buffer || buffer_size == 0) {
        errno = EINVAL;
        return -1;
    }

    now = time(NULL);
    if (now == (time_t)-1) {
        return -1;
    }
    if (!gmtime_r(&now, &utc_tm)) {
        return -1;
    }
    if (strftime(buffer, buffer_size, "%Y-%m-%dT%H:%M:%SZ", &utc_tm) == 0) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return 0;
}

static const char* normalize_display_path(const char* path) {
    if (path && strncmp(path, "./", 2) == 0) {
        return path + 2;
    }
    return path;
}

static int export_plan_contains_selected_path(const ExportPlan* plan, const SelectedPath* selected_path) {
    const char* selected_display;

    if (!plan || !selected_path) {
        return 0;
    }
    selected_display = normalize_display_path(selected_path->display_path);

    for (size_t i = 0; i < plan->count; i++) {
        const ExportEntry* entry = &plan->entries[i];
        if (strcmp(entry->open_path, selected_path->open_path) == 0 &&
            strcmp(entry->display_path, selected_display ? selected_display : selected_path->open_path) == 0) {
            return 1;
        }
    }

    return 0;
}

static void compact_selected_paths_to_export_plan(SelectedPath* selected_paths,
                                                  size_t* selected_count,
                                                  const ExportPlan* plan) {
    size_t write_index = 0;

    if (!selected_count || !selected_paths || !plan) {
        return;
    }

    for (size_t read_index = 0; read_index < *selected_count; read_index++) {
        SelectedPath* path = &selected_paths[read_index];
        if (export_plan_contains_selected_path(plan, path)) {
            if (write_index != read_index) {
                selected_paths[write_index] = *path;
                path->open_path = NULL;
                path->display_path = NULL;
                path->previous_display_path = NULL;
            }
            write_index++;
            continue;
        }

        free(path->open_path);
        free(path->display_path);
        free(path->previous_display_path);
        path->open_path = NULL;
        path->display_path = NULL;
        path->previous_display_path = NULL;
    }

    *selected_count = write_index;
}

int main(int argc, char* argv[]) {
    CliOptions options;
    AppContext ctx = {0};
    SelectedPath* selected_paths = NULL;
    size_t selected_count = 0;
    ExportPlan plan = {0};
    RenderPlanInfo render_info = {0};
    ExportRenderContext render_ctx = {0};
    ExportMetrics metrics = {0};
    int status = 1;
    int temp_created = 0;
    int output_needs_close = 0;
    FILE* output_file = NULL;
    char temp_output_path[MAX_PATH_LENGTH];
    char repository_name[MAX_PATH_LENGTH];
    char generated_at[32];
    temp_output_path[0] = '\0';
    if (parse_cli_options(argc, argv, &options) != 0) {
        return 1;
    }
    if (options.show_version) {
        printf("fuori %s\n", VERSION);
        return 0;
    }
    if (options.show_help) {
        print_usage(argv[0]);
        return 0;
    }
    if (resolve_cli_selection(&options, &selected_paths, &selected_count) != 0) {
        goto cleanup;
    }

    ctx.verbose = options.verbose;
    ctx.no_clobber = options.no_clobber;
    ctx.output_is_stdout = options.output_is_stdout;
    ctx.show_tree = options.show_tree;
    ctx.allow_sensitive = options.allow_sensitive;
    ctx.max_file_size = options.max_file_size;
    ctx.tree_depth = options.tree_depth;
    ctx.warn_tokens = options.warn_tokens;
    ctx.max_tokens = options.max_tokens;
    ctx.output_path = options.output_path;

    if (options.resolved_mode == FILE_SELECTION_RECURSIVE) {
        if (load_ignore_patterns(IGNORE_FILE,
                                 !options.no_default_ignore,
                                 &ctx.ignore_patterns,
                                 &ctx.ignore_count) != 0) {
            fprintf(stderr, "Error: Failed to initialize ignore patterns.\n");
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

    if (options.resolved_mode == FILE_SELECTION_RECURSIVE) {
        if (collect_recursive_export_plan(&ctx, &plan) != 0) {
            fprintf(stderr, "Error collecting directory entries\n");
            goto cleanup;
        }
    } else {
        if (collect_selected_export_plan(selected_paths, selected_count, &ctx, &plan) != 0) {
            fprintf(stderr, "Error collecting selected files\n");
            goto cleanup;
        }
        compact_selected_paths_to_export_plan(selected_paths, &selected_count, &plan);
    }

    if (prepare_render_plan(&plan, &render_info) != 0) {
        perror("Error preparing render plan");
        goto cleanup;
    }

    if (resolve_repository_name(options.resolved_mode, repository_name, sizeof(repository_name)) != 0) {
        perror("Error resolving repository name");
        goto cleanup;
    }

    if (format_generated_timestamp(generated_at, sizeof(generated_at)) != 0) {
        perror("Error formatting export timestamp");
        goto cleanup;
    }

    render_ctx.mode = options.resolved_mode;
    render_ctx.repository = repository_name;
    render_ctx.generated_at = generated_at;
    render_ctx.selected_paths = selected_paths;
    render_ctx.selected_count = selected_count;
    render_ctx.diff_range = options.diff_range;
    render_ctx.show_line_numbers = options.show_line_numbers;
    render_ctx.show_tree = ctx.show_tree;
    render_ctx.tree_depth = ctx.tree_depth;

    if (calculate_export_metrics(&plan, &render_info, &render_ctx, &metrics) != 0) {
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

    if (write_export_header(output_file, &render_ctx) != 0) {
        perror("Error writing output header");
        goto cleanup;
    }

    if (write_change_context(output_file, &render_ctx) != 0) {
        perror("Error writing change context");
        goto cleanup;
    }

    if (render_ctx.show_tree) {
        if (write_project_tree(output_file, &plan, render_ctx.tree_depth) != 0) {
            perror("Error writing project tree");
            goto cleanup;
        }
    }

    errno = 0;
    if (render_export_plan(output_file, &plan, &render_info, &render_ctx, ctx.verbose) != 0) {
        if (errno != 0) {
            perror("Error processing export files");
        } else {
            fprintf(stderr, "Error processing export files\n");
        }
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
        if (ctx.verbose) {
            fprintf(stderr, "Codebase exported to %s successfully!\n", ctx.output_path);
        }
    } else if (ctx.verbose) {
        fprintf(stderr, "Codebase exported to stdout successfully!\n");
    }

    print_export_summary(&metrics);
    print_verbose_skip_summary(&ctx);

    status = 0;

cleanup:
    if (output_needs_close && output_file) {
        fclose(output_file);
    }
    if (temp_created && temp_output_path[0] != '\0') {
        unlink(temp_output_path);
    }
    free_export_plan(&plan);
    free_render_plan_info(&render_info);
    free_selected_paths(selected_paths, selected_count);
    free_ignore_patterns(ctx.ignore_patterns, ctx.ignore_count);
    return status;
}
