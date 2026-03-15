#include "render.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

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

static size_t estimate_tokens(size_t byte_count) {
    return (byte_count / 7) * 2 + ((byte_count % 7) * 2) / 7;
}

static int write_text(FILE* out, const char* text) {
    return (fputs(text, out) == EOF) ? -1 : 0;
}

static int write_bytes(FILE* out, const void* data, size_t len) {
    return (fwrite(data, 1, len, out) == len) ? 0 : -1;
}

static int write_markdown_path(FILE* out, const char* path) {
    static const char markdown_meta[] = "\\`*_{}[]()#+-.!|>";

    for (const unsigned char* p = (const unsigned char*)path; *p != '\0'; p++) {
        unsigned char c = *p;
        if (c == '&') {
            if (write_text(out, "&amp;") != 0) return -1;
            continue;
        }
        if (c == '<') {
            if (write_text(out, "&lt;") != 0) return -1;
            continue;
        }
        if (c == '\n') {
            if (write_text(out, "\\n") != 0) return -1;
            continue;
        }
        if (c == '\r') {
            if (write_text(out, "\\r") != 0) return -1;
            continue;
        }
        if (c == '\t') {
            if (write_text(out, "\\t") != 0) return -1;
            continue;
        }
        if ((c < 0x20 || c == 0x7f)) {
            char escaped[5];
            if (snprintf(escaped, sizeof(escaped), "\\x%02X", c) < 0 ||
                write_text(out, escaped) != 0) {
                return -1;
            }
            continue;
        }
        if (strchr(markdown_meta, c) != NULL && fputc('\\', out) == EOF) {
            return -1;
        }
        if (fputc(c, out) == EOF) {
            return -1;
        }
    }
    return 0;
}

static int count_text_bytes(size_t* total, const char* text) {
    return add_size(total, strlen(text));
}

static int count_markdown_path_bytes(size_t* total, const char* path) {
    static const char markdown_meta[] = "\\`*_{}[]()#+-.!|>";

    for (const unsigned char* p = (const unsigned char*)path; *p != '\0'; p++) {
        unsigned char c = *p;
        if (c == '&') {
            if (add_size(total, strlen("&amp;")) != 0) return -1;
            continue;
        }
        if (c == '<') {
            if (add_size(total, strlen("&lt;")) != 0) return -1;
            continue;
        }
        if (c == '\n' || c == '\r' || c == '\t') {
            if (add_size(total, 2) != 0) return -1;
            continue;
        }
        if ((c < 0x20 || c == 0x7f)) {
            if (add_size(total, 4) != 0) return -1;
            continue;
        }
        if (strchr(markdown_meta, c) != NULL) {
            if (add_size(total, 2) != 0) return -1;
            continue;
        }
        if (add_size(total, 1) != 0) return -1;
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
    if (lang && *lang && write_text(out, lang) != 0) return -1;
    return (fputc('\n', out) == EOF) ? -1 : 0;
}

int write_export_header(FILE* out, FileSelectionMode mode) {
    if (write_text(out, "# Codebase Export\n\n") != 0) {
        return -1;
    }

    return write_text(out, export_description(mode));
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

static int count_entry_bytes(const ExportEntry* entry, size_t fence, size_t* total) {
    if (count_text_bytes(total, "## ") != 0 ||
        count_markdown_path_bytes(total, entry->display_path) != 0 ||
        count_text_bytes(total, "\n\n") != 0 ||
        count_fence_bytes(total, fence, entry->lang) != 0 ||
        add_size(total, entry->buf_len) != 0) {
        return -1;
    }
    if (entry->buf_len > 0 && entry->buf[entry->buf_len - 1] != '\n' &&
        add_size(total, 1) != 0) {
        return -1;
    }
    if (count_fence_bytes(total, fence, NULL) != 0 ||
        count_text_bytes(total, "\n\n") != 0) {
        return -1;
    }
    return 0;
}

int calculate_export_metrics(const ExportPlan* plan,
                             const RenderPlanInfo* info,
                             FileSelectionMode mode,
                             int show_tree,
                             size_t tree_depth,
                             ExportMetrics* metrics) {
    size_t total = 0;

    if (!plan || !info || !metrics || info->count != plan->count) {
        errno = EINVAL;
        return -1;
    }

    if (count_text_bytes(&total, "# Codebase Export\n\n") != 0 ||
        count_text_bytes(&total, export_description(mode)) != 0) {
        return -1;
    }

    if (show_tree && count_project_tree_bytes(plan, tree_depth, &total) != 0) {
        return -1;
    }

    for (size_t i = 0; i < plan->count; i++) {
        size_t fence = 0;
        if (get_fence_length(info, i, &fence) != 0 ||
            count_entry_bytes(&plan->entries[i], fence, &total) != 0) {
            return -1;
        }
    }

    metrics->files_exported = plan->count;
    metrics->bytes_written = total;
    metrics->estimated_tokens = estimate_tokens(total);
    return 0;
}

static int render_entry(FILE* out, const ExportEntry* entry, size_t fence) {
    if (write_text(out, "## ") != 0 ||
        write_markdown_path(out, entry->display_path) != 0 ||
        write_text(out, "\n\n") != 0) {
        return -1;
    }
    if (write_fence(out, fence, entry->lang) != 0) {
        return -1;
    }
    if (entry->buf_len > 0 && write_bytes(out, entry->buf, entry->buf_len) != 0) {
        return -1;
    }
    if (entry->buf_len > 0 && entry->buf[entry->buf_len - 1] != '\n' && write_text(out, "\n") != 0) {
        return -1;
    }
    if (write_fence(out, fence, NULL) != 0 ||
        write_text(out, "\n\n") != 0) {
        return -1;
    }
    return 0;
}

int render_export_plan(FILE* out, const ExportPlan* plan, const RenderPlanInfo* info, int verbose) {
    if (!plan || !info || info->count != plan->count) {
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
            render_entry(out, &plan->entries[i], fence) != 0) {
            return -1;
        }
    }
    return 0;
}
