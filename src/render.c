#include "render.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "text_io.h"
#include "tree.h"

#ifdef FUORI_TESTING
static int maybe_inject_render_failure(size_t index) {
    static int initialized = 0;
    static int enabled = 0;
    static size_t fail_index = 0;

    if (!initialized) {
        const char* value = getenv("FUORI_TEST_FAIL_RENDER_AT");
        initialized = 1;
        if (value && *value != '\0') {
            char* end = NULL;
            unsigned long parsed = strtoul(value, &end, 10);
            if (end != value && *end == '\0') {
                fail_index = (size_t)parsed;
                enabled = 1;
            }
        }
    }

    if (enabled && index == fail_index) {
        errno = EIO;
        return -1;
    }

    return 0;
}
#endif

static int add_size(size_t* total, size_t amount) {
    if (*total > SIZE_MAX - amount) {
        errno = EOVERFLOW;
        return -1;
    }
    *total += amount;
    return 0;
}

typedef struct {
    FILE* out;
    size_t* total;
} RenderSink;

static const char* export_description(FileSelectionMode mode) {
    switch (mode) {
        case FILE_SELECTION_GIT_WORKTREE:
            return "This document contains tracked files plus untracked, non-ignored files from the current Git subtree.\n\n";
        case FILE_SELECTION_GIT_STAGED:
            return "This document contains staged files selected from the current Git subtree.\n\n";
        case FILE_SELECTION_GIT_UNSTAGED:
            return "This document contains unstaged tracked files selected from the current Git subtree.\n\n";
        case FILE_SELECTION_GIT_DIFF:
            return "This document contains files selected from the current Git subtree by the requested Git diff range.\n\n";
        case FILE_SELECTION_STDIN:
            return "This document contains files selected from caller-supplied stdin paths.\n\n";
        case FILE_SELECTION_RECURSIVE:
            return "This document contains all the source code files from the current directory subtree using the local filesystem walker.\n\n";
        case FILE_SELECTION_AUTO:
        default:
            return "This document contains all the source code files from the current directory subtree.\n\n";
    }
}

static const char* export_mode_label(FileSelectionMode mode) {
    switch (mode) {
        case FILE_SELECTION_GIT_WORKTREE:
            return "worktree";
        case FILE_SELECTION_GIT_STAGED:
            return "staged";
        case FILE_SELECTION_GIT_UNSTAGED:
            return "unstaged";
        case FILE_SELECTION_GIT_DIFF:
            return "diff";
        case FILE_SELECTION_STDIN:
            return "stdin";
        case FILE_SELECTION_RECURSIVE:
            return "recursive";
        case FILE_SELECTION_AUTO:
        default:
            return "auto";
    }
}

static int should_render_change_context(FileSelectionMode mode) {
    return mode == FILE_SELECTION_GIT_STAGED ||
           mode == FILE_SELECTION_GIT_UNSTAGED ||
           mode == FILE_SELECTION_GIT_DIFF;
}

static const char* change_type_label(SelectedPathChangeType change_type) {
    switch (change_type) {
        case SELECTED_PATH_CHANGE_ADDED:
            return "A";
        case SELECTED_PATH_CHANGE_RENAMED:
            return "R";
        case SELECTED_PATH_CHANGE_MODIFIED:
        case SELECTED_PATH_CHANGE_NONE:
        default:
            return "M";
    }
}

static size_t estimate_tokens(size_t byte_count) {
    /* Approximate 1 token per 3.5 bytes using integer math to avoid floating point. */
    return (byte_count / 7) * 2 + ((byte_count % 7) * 2) / 7;
}

static int needs_markdown_escape(unsigned char c) {
    static const char markdown_meta[] = "\\`*[]";
    return strchr(markdown_meta, c) != NULL;
}

static int sink_write_text(RenderSink* sink, const char* text) {
    if (!sink || !text) {
        errno = EINVAL;
        return -1;
    }
    if (sink->out) {
        return fuori_write_text(sink->out, text);
    }
    if (sink->total) {
        return fuori_count_text_bytes(sink->total, text);
    }
    errno = EINVAL;
    return -1;
}

static int sink_write_char(RenderSink* sink, char c) {
    if (!sink) {
        errno = EINVAL;
        return -1;
    }
    if (sink->out) {
        return (fputc(c, sink->out) == EOF) ? -1 : 0;
    }
    if (sink->total) {
        return add_size(sink->total, 1);
    }
    errno = EINVAL;
    return -1;
}

static int sink_write_bytes(RenderSink* sink, const void* data, size_t len) {
    if (!sink || (!data && len > 0)) {
        errno = EINVAL;
        return -1;
    }
    if (sink->out) {
        return (fwrite(data, 1, len, sink->out) == len) ? 0 : -1;
    }
    if (sink->total) {
        return add_size(sink->total, len);
    }
    errno = EINVAL;
    return -1;
}

static int emit_markdown_path(RenderSink* sink, const char* path) {
    for (const unsigned char* p = (const unsigned char*)path; *p != '\0'; p++) {
        unsigned char c = *p;
        if (c == '&') {
            if (sink_write_text(sink, "&amp;") != 0) return -1;
            continue;
        }
        if (c == '<') {
            if (sink_write_text(sink, "&lt;") != 0) return -1;
            continue;
        }
        if (c == '\n') {
            if (sink_write_text(sink, "\\n") != 0) return -1;
            continue;
        }
        if (c == '\r') {
            if (sink_write_text(sink, "\\r") != 0) return -1;
            continue;
        }
        if (c == '\t') {
            if (sink_write_text(sink, "\\t") != 0) return -1;
            continue;
        }
        if ((c < 0x20 || c == 0x7f)) {
            char escaped[5];
            if (snprintf(escaped, sizeof(escaped), "\\x%02X", c) < 0 ||
                sink_write_text(sink, escaped) != 0) {
                return -1;
            }
            continue;
        }
        if (needs_markdown_escape(c) && sink_write_char(sink, '\\') != 0) {
            return -1;
        }
        if (sink_write_char(sink, (char)c) != 0) {
            return -1;
        }
    }
    return 0;
}

static int count_fence_bytes(size_t* total, size_t count, const char* lang) {
    if (add_size(total, count) != 0) return -1;
    if (lang && *lang && add_size(total, strlen(lang)) != 0) return -1;
    return add_size(total, 1);
}

static int write_fence(FILE* out, size_t count, const char* lang) {
    for (size_t i = 0; i < count; i++) {
        if (fputc('`', out) == EOF) return -1;
    }
    if (lang && *lang && fuori_write_text(out, lang) != 0) return -1;
    return (fputc('\n', out) == EOF) ? -1 : 0;
}

static int format_size_value(size_t value, char* buffer, size_t buffer_size) {
    if (!buffer || buffer_size == 0) {
        errno = EINVAL;
        return -1;
    }
    if (snprintf(buffer, buffer_size, "%zu", value) < 0 || strlen(buffer) >= buffer_size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static size_t decimal_digit_count(size_t value) {
    size_t digits = 1;

    while (value >= 10) {
        value /= 10;
        digits++;
    }

    return digits;
}

static size_t entry_line_count(const ExportEntry* entry) {
    size_t count = 0;

    if (!entry || entry->buf_len == 0) {
        return 0;
    }

    for (size_t i = 0; i < entry->buf_len; i++) {
        if (entry->buf[i] == '\n') {
            count++;
        }
    }

    if (entry->buf[entry->buf_len - 1] != '\n') {
        count++;
    }

    return count;
}

static int emit_line_number_prefix(RenderSink* sink, size_t line_no, size_t width) {
    char number_buf[32];
    size_t digits = decimal_digit_count(line_no);

    if (format_size_value(line_no, number_buf, sizeof(number_buf)) != 0) {
        return -1;
    }
    while (digits < width) {
        if (sink_write_char(sink, ' ') != 0) {
            return -1;
        }
        digits++;
    }
    return sink_write_text(sink, number_buf);
}

static int emit_export_header(RenderSink* sink, const ExportRenderContext* ctx) {
    if (!sink || !ctx || !ctx->repository || !ctx->generated_at) {
        errno = EINVAL;
        return -1;
    }

    if (sink_write_text(sink, "# Codebase Export\n\n") != 0 ||
        sink_write_text(sink, "Repository: ") != 0 ||
        sink_write_text(sink, ctx->repository) != 0 ||
        sink_write_text(sink, "\nMode: ") != 0 ||
        sink_write_text(sink, export_mode_label(ctx->mode)) != 0 ||
        sink_write_text(sink, "\nGenerated: ") != 0 ||
        sink_write_text(sink, ctx->generated_at) != 0) {
        return -1;
    }

    if (ctx->show_line_numbers &&
        (sink_write_text(sink, "\nLine numbers: on") != 0)) {
        return -1;
    }

    if (sink_write_text(sink, "\n\n") != 0 ||
        sink_write_text(sink, export_description(ctx->mode)) != 0) {
        return -1;
    }

    return 0;
}

static int emit_change_context(RenderSink* sink, const ExportRenderContext* ctx) {
    char count_buf[32];

    if (!sink || !ctx) {
        errno = EINVAL;
        return -1;
    }
    if (!should_render_change_context(ctx->mode)) {
        return 0;
    }
    if (format_size_value(ctx->selected_count, count_buf, sizeof(count_buf)) != 0) {
        return -1;
    }

    if (sink_write_text(sink, "## Change Context\n\n") != 0 ||
        sink_write_text(sink, "Files changed: ") != 0 ||
        sink_write_text(sink, count_buf) != 0 ||
        sink_write_text(sink, "\n") != 0) {
        return -1;
    }
    if (ctx->mode == FILE_SELECTION_GIT_DIFF &&
        ctx->diff_range &&
        *ctx->diff_range != '\0' &&
        (sink_write_text(sink, "Diff range: ") != 0 ||
         sink_write_text(sink, ctx->diff_range) != 0 ||
         sink_write_text(sink, "\n") != 0)) {
        return -1;
    }
    if (sink_write_text(sink, "\n") != 0) {
        return -1;
    }

    for (size_t i = 0; i < ctx->selected_count; i++) {
        const SelectedPath* path = &ctx->selected_paths[i];
        if (sink_write_text(sink, "- ") != 0 ||
            sink_write_text(sink, change_type_label(path->change_type)) != 0 ||
            sink_write_text(sink, " ") != 0) {
            return -1;
        }
        if (path->change_type == SELECTED_PATH_CHANGE_RENAMED &&
            path->previous_display_path &&
            *path->previous_display_path != '\0' &&
            (emit_markdown_path(sink, path->previous_display_path) != 0 ||
             sink_write_text(sink, " -> ") != 0)) {
            return -1;
        }
        if (emit_markdown_path(sink, path->display_path) != 0 ||
            sink_write_text(sink, "\n") != 0) {
            return -1;
        }
    }

    return sink_write_text(sink, "\n");
}

int write_export_header(FILE* out, const ExportRenderContext* ctx) {
    RenderSink sink = {.out = out, .total = NULL};
    return emit_export_header(&sink, ctx);
}

int write_change_context(FILE* out, const ExportRenderContext* ctx) {
    RenderSink sink = {.out = out, .total = NULL};
    return emit_change_context(&sink, ctx);
}

static size_t compute_fence_length(const ExportEntry* entry) {
    size_t max_run = 0;
    size_t current_run = 0;

    for (size_t i = 0; i < entry->buf_len; i++) {
        if (entry->buf[i] == '`') {
            current_run++;
            if (current_run > max_run) {
                max_run = current_run;
            }
        } else {
            current_run = 0;
        }
    }

    return (max_run >= 3) ? max_run + 1 : 3;
}

int prepare_render_plan(const ExportPlan* plan, RenderPlanInfo* info) {
    if (!plan || !info) {
        errno = EINVAL;
        return -1;
    }

    info->fence_lengths = NULL;
    info->count = 0;

    if (plan->count == 0) {
        return 0;
    }

    info->fence_lengths = malloc(plan->count * sizeof(*info->fence_lengths));
    if (!info->fence_lengths) {
        return -1;
    }

    info->count = plan->count;
    for (size_t i = 0; i < plan->count; i++) {
        info->fence_lengths[i] = compute_fence_length(&plan->entries[i]);
    }

    return 0;
}

void free_render_plan_info(RenderPlanInfo* info) {
    if (!info) {
        return;
    }

    free(info->fence_lengths);
    info->fence_lengths = NULL;
    info->count = 0;
}

static int get_fence_length(const RenderPlanInfo* info, size_t index, size_t* fence_out) {
    if (!info || !fence_out || index >= info->count) {
        errno = EINVAL;
        return -1;
    }

    *fence_out = info->fence_lengths[index];
    return 0;
}

static int emit_entry_body(RenderSink* sink, const ExportEntry* entry, int show_line_numbers) {
    size_t line_count = 0;
    size_t width = 0;
    size_t line_no = 1;
    size_t line_start = 0;

    if (!sink || !entry) {
        errno = EINVAL;
        return -1;
    }

    if (!show_line_numbers) {
        if (entry->buf_len > 0 && sink_write_bytes(sink, entry->buf, entry->buf_len) != 0) {
            return -1;
        }
        if (entry->buf_len > 0 &&
            entry->buf[entry->buf_len - 1] != '\n' &&
            sink_write_char(sink, '\n') != 0) {
            return -1;
        }
        return 0;
    }

    line_count = entry_line_count(entry);
    if (line_count == 0) {
        return 0;
    }
    width = decimal_digit_count(line_count);

    for (size_t i = 0; i < entry->buf_len; i++) {
        if (entry->buf[i] != '\n') {
            continue;
        }
        if (emit_line_number_prefix(sink, line_no, width) != 0 ||
            sink_write_text(sink, " | ") != 0 ||
            sink_write_bytes(sink, entry->buf + line_start, (i - line_start) + 1) != 0) {
            return -1;
        }
        line_no++;
        line_start = i + 1;
    }

    if (line_start < entry->buf_len) {
        if (emit_line_number_prefix(sink, line_no, width) != 0 ||
            sink_write_text(sink, " | ") != 0 ||
            sink_write_bytes(sink, entry->buf + line_start, entry->buf_len - line_start) != 0 ||
            sink_write_char(sink, '\n') != 0) {
            return -1;
        }
    }

    return 0;
}

static int emit_entry(RenderSink* sink,
                      const ExportEntry* entry,
                      size_t fence,
                      const ExportRenderContext* ctx) {
    if (!sink || !entry || !ctx) {
        errno = EINVAL;
        return -1;
    }

    if (sink_write_text(sink, "## ") != 0 ||
        emit_markdown_path(sink, entry->display_path) != 0 ||
        sink_write_text(sink, "\n\n") != 0 ||
        (sink->out ? write_fence(sink->out, fence, entry->lang)
                   : count_fence_bytes(sink->total, fence, entry->lang)) != 0 ||
        emit_entry_body(sink, entry, ctx->show_line_numbers) != 0 ||
        (sink->out ? write_fence(sink->out, fence, NULL)
                   : count_fence_bytes(sink->total, fence, NULL)) != 0 ||
        sink_write_text(sink, "\n\n") != 0) {
        return -1;
    }

    return 0;
}

int calculate_export_metrics(const ExportPlan* plan,
                             const RenderPlanInfo* info,
                             const ExportRenderContext* ctx,
                             ExportMetrics* metrics) {
    size_t total = 0;
    RenderSink sink = {.out = NULL, .total = &total};

    if (!plan || !info || !ctx || !metrics || info->count != plan->count) {
        errno = EINVAL;
        return -1;
    }

    if (emit_export_header(&sink, ctx) != 0) {
        return -1;
    }
    if (emit_change_context(&sink, ctx) != 0) {
        return -1;
    }

    if (ctx->show_tree && count_project_tree_bytes(plan, ctx->tree_depth, &total) != 0) {
        return -1;
    }

    for (size_t i = 0; i < plan->count; i++) {
        size_t fence = 0;
        if (get_fence_length(info, i, &fence) != 0 ||
            emit_entry(&sink, &plan->entries[i], fence, ctx) != 0) {
            return -1;
        }
    }

    metrics->files_exported = plan->count;
    metrics->bytes_written = total;
    metrics->estimated_tokens = estimate_tokens(total);
    return 0;
}

int render_export_plan(FILE* out,
                       const ExportPlan* plan,
                       const RenderPlanInfo* info,
                       const ExportRenderContext* ctx,
                       int verbose) {
    RenderSink sink = {.out = out, .total = NULL};

    if (!plan || !info || !ctx || info->count != plan->count) {
        errno = EINVAL;
        return -1;
    }

    for (size_t i = 0; i < plan->count; i++) {
        size_t fence = 0;
        if (verbose) {
            fprintf(stderr, "Processing file: %s\n", plan->entries[i].display_path);
        }
#ifdef FUORI_TESTING
        if (maybe_inject_render_failure(i) != 0) {
            return -1;
        }
#endif
        if (get_fence_length(info, i, &fence) != 0 ||
            emit_entry(&sink, &plan->entries[i], fence, ctx) != 0) {
            return -1;
        }
    }
    return 0;
}
