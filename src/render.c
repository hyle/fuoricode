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

typedef struct {
    size_t* starts;
    size_t* ends;
    size_t count;
} LineIndex;

static int count_fence_bytes(size_t* total, size_t count, const char* lang);
static int write_fence(FILE* out, size_t count, const char* lang);

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

static int sink_write_fence(RenderSink* sink, size_t count, const char* lang) {
    if (!sink) {
        errno = EINVAL;
        return -1;
    }
    if (sink->out) {
        return write_fence(sink->out, count, lang);
    }
    if (sink->total) {
        return count_fence_bytes(sink->total, count, lang);
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
    if (ctx->show_hunks) {
        char context_buf[32];
        if (format_size_value(ctx->hunk_context_lines, context_buf, sizeof(context_buf)) != 0 ||
            sink_write_text(sink, "\nHunks: on (context: ") != 0 ||
            sink_write_text(sink, context_buf) != 0 ||
            sink_write_char(sink, ')') != 0) {
            return -1;
        }
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

static int append_render_range(RenderEntryInfo* info,
                               size_t* capacity,
                               size_t start_line,
                               size_t end_line) {
    RenderLineRange* new_ranges;
    size_t new_capacity;

    if (!info || !capacity || start_line == 0 || end_line < start_line) {
        errno = EINVAL;
        return -1;
    }

    if (info->range_count > 0) {
        RenderLineRange* last = &info->ranges[info->range_count - 1];
        if (start_line <= last->end_line + 1) {
            if (end_line > last->end_line) {
                last->end_line = end_line;
            }
            return 0;
        }
    }

    if (info->range_count == *capacity) {
        new_capacity = (*capacity == 0) ? 4 : *capacity * 2;
        new_ranges = realloc(info->ranges, new_capacity * sizeof(*new_ranges));
        if (!new_ranges) {
            return -1;
        }
        info->ranges = new_ranges;
        *capacity = new_capacity;
    }

    info->ranges[info->range_count].start_line = start_line;
    info->ranges[info->range_count].end_line = end_line;
    info->range_count++;
    return 0;
}

static int compute_entry_render_ranges(const ExportEntry* entry,
                                       const GitFileHunks* hunks,
                                       size_t context_lines,
                                       RenderEntryInfo* info) {
    size_t capacity = 0;

    if (!entry || !hunks || !info) {
        errno = EINVAL;
        return -1;
    }

    info->total_lines = entry_line_count(entry);
    if (info->total_lines == 0) {
        return 0;
    }

    for (size_t i = 0; i < hunks->count; i++) {
        size_t changed_start = hunks->ranges[i].new_start;
        size_t changed_end = changed_start;
        size_t render_start;
        size_t render_end;

        if (changed_start == 0) {
            changed_start = 1;
        }
        if (changed_start > info->total_lines) {
            changed_start = info->total_lines;
        }

        if (hunks->ranges[i].new_count > 0) {
            if (hunks->ranges[i].new_start > SIZE_MAX - (hunks->ranges[i].new_count - 1)) {
                changed_end = SIZE_MAX;
            } else {
                changed_end = hunks->ranges[i].new_start + hunks->ranges[i].new_count - 1;
            }
            if (changed_end < changed_start) {
                changed_end = changed_start;
            }
            if (changed_end > info->total_lines) {
                changed_end = info->total_lines;
            }
        }

        render_start = (changed_start > context_lines) ? changed_start - context_lines : 1;
        render_end = changed_end;
        if (context_lines >= info->total_lines ||
            render_end > info->total_lines - context_lines) {
            render_end = info->total_lines;
        } else {
            render_end += context_lines;
        }

        if (append_render_range(info, &capacity, render_start, render_end) != 0) {
            return -1;
        }
    }

    return 0;
}

static int build_line_index(const ExportEntry* entry, LineIndex* index) {
    size_t line_count;
    size_t line_no = 0;
    size_t line_start = 0;

    if (!entry || !index) {
        errno = EINVAL;
        return -1;
    }

    memset(index, 0, sizeof(*index));
    line_count = entry_line_count(entry);
    if (line_count == 0) {
        return 0;
    }

    index->starts = malloc(line_count * sizeof(*index->starts));
    index->ends = malloc(line_count * sizeof(*index->ends));
    if (!index->starts || !index->ends) {
        free(index->starts);
        free(index->ends);
        index->starts = NULL;
        index->ends = NULL;
        return -1;
    }

    for (size_t i = 0; i < entry->buf_len; i++) {
        if (entry->buf[i] != '\n') {
            continue;
        }
        index->starts[line_no] = line_start;
        index->ends[line_no] = i + 1;
        line_no++;
        line_start = i + 1;
    }

    if (line_start < entry->buf_len || (entry->buf_len > 0 && entry->buf[entry->buf_len - 1] != '\n')) {
        index->starts[line_no] = line_start;
        index->ends[line_no] = entry->buf_len;
        line_no++;
    }

    index->count = line_no;
    return 0;
}

static void free_line_index(LineIndex* index) {
    if (!index) {
        return;
    }
    free(index->starts);
    free(index->ends);
    index->starts = NULL;
    index->ends = NULL;
    index->count = 0;
}

static int emit_entry_heading(RenderSink* sink, const ExportEntry* entry) {
    if (!sink || !entry) {
        errno = EINVAL;
        return -1;
    }

    if (sink_write_text(sink, "## ") != 0 ||
        emit_markdown_path(sink, entry->display_path) != 0 ||
        sink_write_text(sink, "\n\n") != 0) {
        return -1;
    }

    return 0;
}

static int emit_line_range(RenderSink* sink,
                           const ExportEntry* entry,
                           const LineIndex* index,
                           size_t start_line,
                           size_t end_line,
                           int show_line_numbers,
                           size_t line_number_width) {
    if (!sink || !entry || !index || start_line == 0 || end_line < start_line || end_line > index->count) {
        errno = EINVAL;
        return -1;
    }

    for (size_t line_no = start_line; line_no <= end_line; line_no++) {
        size_t offset = line_no - 1;
        size_t start = index->starts[offset];
        size_t end = index->ends[offset];

        if (show_line_numbers &&
            (emit_line_number_prefix(sink, line_no, line_number_width) != 0 ||
             sink_write_text(sink, " | ") != 0)) {
            return -1;
        }
        if (end > start &&
            sink_write_bytes(sink, entry->buf + start, end - start) != 0) {
            return -1;
        }
        if (end == start || entry->buf[end - 1] != '\n') {
            if (sink_write_char(sink, '\n') != 0) {
                return -1;
            }
        }
    }

    return 0;
}

static int emit_omission_marker(RenderSink* sink, size_t omitted_lines) {
    char count_buf[32];

    if (!sink || omitted_lines == 0) {
        errno = EINVAL;
        return -1;
    }
    if (format_size_value(omitted_lines, count_buf, sizeof(count_buf)) != 0 ||
        sink_write_text(sink, "... ") != 0 ||
        sink_write_text(sink, count_buf) != 0 ||
        sink_write_text(sink, " unchanged lines omitted ...\n\n") != 0) {
        return -1;
    }
    return 0;
}

static int emit_full_entry(RenderSink* sink,
                           const ExportEntry* entry,
                           const RenderEntryInfo* entry_info,
                           const ExportRenderContext* ctx) {
    LineIndex index = {0};
    size_t width = 0;
    int status = -1;

    if (!sink || !entry || !entry_info || !ctx) {
        errno = EINVAL;
        return -1;
    }

    if (emit_entry_heading(sink, entry) != 0 ||
        sink_write_fence(sink, entry_info->fence_length, entry->lang) != 0) {
        return -1;
    }

    if (build_line_index(entry, &index) != 0) {
        goto cleanup;
    }

    if (ctx->show_line_numbers) {
        width = decimal_digit_count(entry_info->total_lines);
    }
    if (index.count > 0 &&
        emit_line_range(sink,
                        entry,
                        &index,
                        1,
                        index.count,
                        ctx->show_line_numbers,
                        width) != 0) {
        goto cleanup;
    }

    if (sink_write_fence(sink, entry_info->fence_length, NULL) != 0 ||
        sink_write_text(sink, "\n\n") != 0) {
        goto cleanup;
    }

    status = 0;

cleanup:
    free_line_index(&index);
    return status;
}

static int emit_sliced_entry(RenderSink* sink,
                             const ExportEntry* entry,
                             const RenderEntryInfo* entry_info,
                             const ExportRenderContext* ctx) {
    LineIndex index = {0};
    size_t width = 0;
    size_t previous_end = 0;
    int status = -1;

    if (!sink || !entry || !entry_info || !ctx || entry_info->range_count == 0) {
        errno = EINVAL;
        return -1;
    }

    if (emit_entry_heading(sink, entry) != 0 ||
        build_line_index(entry, &index) != 0) {
        goto cleanup;
    }

    if (ctx->show_line_numbers) {
        width = decimal_digit_count(entry_info->total_lines);
    }

    for (size_t i = 0; i < entry_info->range_count; i++) {
        const RenderLineRange* range = &entry_info->ranges[i];
        if (range->start_line > previous_end + 1 &&
            emit_omission_marker(sink, range->start_line - previous_end - 1) != 0) {
            goto cleanup;
        }
        if (sink_write_fence(sink, entry_info->fence_length, entry->lang) != 0 ||
            emit_line_range(sink,
                            entry,
                            &index,
                            range->start_line,
                            range->end_line,
                            ctx->show_line_numbers,
                            width) != 0 ||
            sink_write_fence(sink, entry_info->fence_length, NULL) != 0 ||
            sink_write_text(sink, "\n\n") != 0) {
            goto cleanup;
        }
        previous_end = range->end_line;
    }

    if (entry_info->total_lines > previous_end &&
        emit_omission_marker(sink, entry_info->total_lines - previous_end) != 0) {
        goto cleanup;
    }

    status = 0;

cleanup:
    free_line_index(&index);
    return status;
}

static int emit_entry(RenderSink* sink,
                      const ExportEntry* entry,
                      const RenderEntryInfo* entry_info,
                      const ExportRenderContext* ctx) {
    if (!sink || !entry || !entry_info || !ctx) {
        errno = EINVAL;
        return -1;
    }

    switch (entry_info->mode) {
        case RENDER_ENTRY_FULL:
            return emit_full_entry(sink, entry, entry_info, ctx);
        case RENDER_ENTRY_SLICED:
            return emit_sliced_entry(sink, entry, entry_info, ctx);
        case RENDER_ENTRY_OMIT:
            return 0;
        default:
            errno = EINVAL;
            return -1;
    }
}

static int initialize_render_plan_info(const ExportPlan* plan, RenderPlanInfo* info) {
    if (!plan || !info) {
        errno = EINVAL;
        return -1;
    }

    memset(info, 0, sizeof(*info));
    if (plan->count == 0) {
        return 0;
    }

    info->entries = calloc(plan->count, sizeof(*info->entries));
    info->include_mask = calloc(plan->count, sizeof(*info->include_mask));
    if (!info->entries || !info->include_mask) {
        free_render_plan_info(info);
        return -1;
    }

    info->count = plan->count;
    for (size_t i = 0; i < plan->count; i++) {
        info->entries[i].fence_length = compute_fence_length(&plan->entries[i]);
        info->entries[i].total_lines = entry_line_count(&plan->entries[i]);
        info->entries[i].mode = RENDER_ENTRY_FULL;
        info->include_mask[i] = 1;
    }

    return 0;
}

static int collect_render_hunks(const ExportPlan* plan,
                                const ExportRenderContext* ctx,
                                GitFileHunks** hunks_out,
                                size_t* hunk_count_out) {
    if (!plan || !ctx || !hunks_out || !hunk_count_out) {
        errno = EINVAL;
        return -1;
    }
    if (ctx->selected_count != plan->count) {
        errno = EINVAL;
        return -1;
    }

    if (collect_git_hunks(ctx->mode,
                          ctx->diff_range,
                          ctx->selected_paths,
                          ctx->selected_count,
                          hunks_out,
                          hunk_count_out) != 0) {
        return -1;
    }
    if (*hunk_count_out != plan->count) {
        free_git_hunks(*hunks_out, *hunk_count_out);
        *hunks_out = NULL;
        *hunk_count_out = 0;
        errno = EINVAL;
        return -1;
    }

    return 0;
}

static int prepare_hunk_render_entry(const ExportEntry* entry,
                                     const SelectedPath* path,
                                     const GitFileHunks* file_hunks,
                                     size_t context_lines,
                                     RenderEntryInfo* entry_info,
                                     unsigned char* include_mask,
                                     size_t* visible_count) {
    if (!entry || !path || !file_hunks || !entry_info || !include_mask || !visible_count) {
        errno = EINVAL;
        return -1;
    }
    if (strcmp(entry->open_path, path->open_path) != 0) {
        errno = EINVAL;
        return -1;
    }

    if (path->change_type == SELECTED_PATH_CHANGE_ADDED) {
        entry_info->mode = RENDER_ENTRY_FULL;
        *include_mask = 1;
        (*visible_count)++;
        return 0;
    }

    if (compute_entry_render_ranges(entry, file_hunks, context_lines, entry_info) != 0) {
        return -1;
    }

    if (entry_info->range_count > 0) {
        entry_info->mode = RENDER_ENTRY_SLICED;
        *include_mask = 1;
        (*visible_count)++;
    } else {
        entry_info->mode = RENDER_ENTRY_OMIT;
        *include_mask = 0;
    }

    return 0;
}

static int prepare_hunk_render_plan(const ExportPlan* plan,
                                    const ExportRenderContext* ctx,
                                    RenderPlanInfo* info) {
    GitFileHunks* hunks = NULL;
    size_t hunk_count = 0;
    int status = -1;

    if (collect_render_hunks(plan, ctx, &hunks, &hunk_count) != 0) {
        return -1;
    }

    info->visible_count = 0;
    for (size_t i = 0; i < plan->count; i++) {
        if (prepare_hunk_render_entry(&plan->entries[i],
                                      &ctx->selected_paths[i],
                                      &hunks[i],
                                      ctx->hunk_context_lines,
                                      &info->entries[i],
                                      &info->include_mask[i],
                                      &info->visible_count) != 0) {
            goto cleanup;
        }
    }

    status = 0;

cleanup:
    free_git_hunks(hunks, hunk_count);
    return status;
}

int prepare_render_plan(const ExportPlan* plan,
                        const ExportRenderContext* ctx,
                        RenderPlanInfo* info) {
    if (!plan || !ctx || !info) {
        errno = EINVAL;
        return -1;
    }

    if (initialize_render_plan_info(plan, info) != 0) {
        return -1;
    }

    if (!ctx->show_hunks) {
        info->visible_count = plan->count;
        return 0;
    }

    if (prepare_hunk_render_plan(plan, ctx, info) != 0) {
        free_render_plan_info(info);
        return -1;
    }
    return 0;
}

void free_render_plan_info(RenderPlanInfo* info) {
    if (!info) {
        return;
    }

    if (info->entries) {
        for (size_t i = 0; i < info->count; i++) {
            free(info->entries[i].ranges);
        }
    }
    free(info->entries);
    free(info->include_mask);
    info->entries = NULL;
    info->include_mask = NULL;
    info->count = 0;
    info->visible_count = 0;
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

    if (emit_export_header(&sink, ctx) != 0 ||
        emit_change_context(&sink, ctx) != 0) {
        return -1;
    }

    if (ctx->show_tree &&
        count_project_tree_bytes_filtered(plan, info->include_mask, ctx->tree_depth, &total) != 0) {
        return -1;
    }

    for (size_t i = 0; i < plan->count; i++) {
        if (!info->include_mask[i]) {
            continue;
        }
        if (emit_entry(&sink, &plan->entries[i], &info->entries[i], ctx) != 0) {
            return -1;
        }
    }

    metrics->files_exported = info->visible_count;
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
        if (!info->include_mask[i]) {
            continue;
        }
        if (verbose) {
            fprintf(stderr, "Processing file: %s\n", plan->entries[i].display_path);
        }
#ifdef FUORI_TESTING
        if (maybe_inject_render_failure(i) != 0) {
            return -1;
        }
#endif
        if (emit_entry(&sink, &plan->entries[i], &info->entries[i], ctx) != 0) {
            return -1;
        }
    }
    return 0;
}
