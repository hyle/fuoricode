#include "render.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define TREE_BRANCH "\xE2\x94\x9C\xE2\x94\x80\xE2\x94\x80 "
#define TREE_LAST "\xE2\x94\x94\xE2\x94\x80\xE2\x94\x80 "
#define TREE_PIPE "\xE2\x94\x82   "
#define TREE_SPACE "    "

typedef struct TreeNode {
    char* name;
    int is_dir;
    struct TreeNode** children;
    size_t child_count;
    size_t child_capacity;
} TreeNode;

static int write_text(FILE* out, const char* text) {
    return (fputs(text, out) == EOF) ? -1 : 0;
}

static int write_bytes(FILE* out, const void* data, size_t len) {
    return (fwrite(data, 1, len, out) == len) ? 0 : -1;
}

static int write_visible_text(FILE* out, const char* text) {
    for (const unsigned char* p = (const unsigned char*)text; *p != '\0'; p++) {
        unsigned char c = *p;
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
        if (fputc(c, out) == EOF) {
            return -1;
        }
    }
    return 0;
}

static int write_markdown_path(FILE* out, const char* path) {
    static const char markdown_meta[] = "\\`*_{}[]()#+-.!|>";

    for (const unsigned char* p = (const unsigned char*)path; *p != '\0'; p++) {
        unsigned char c = *p;
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

    switch (mode) {
        case FILE_SELECTION_GIT_STAGED:
            return write_text(out,
                              "This document contains staged files selected from the current Git subtree.\n\n");
        case FILE_SELECTION_GIT_UNSTAGED:
            return write_text(out,
                              "This document contains unstaged tracked files selected from the current Git subtree.\n\n");
        case FILE_SELECTION_GIT_DIFF:
            return write_text(out,
                              "This document contains files selected from the current Git subtree by the requested Git diff range.\n\n");
        case FILE_SELECTION_RECURSIVE:
        default:
            return write_text(out,
                              "This document contains all the source code files from the current directory subtree.\n\n");
    }
}

static TreeNode* tree_node_create(const char* name, int is_dir) {
    TreeNode* node = calloc(1, sizeof(*node));
    if (!node) {
        return NULL;
    }

    node->name = strdup(name ? name : "");
    if (!node->name) {
        free(node);
        return NULL;
    }
    node->is_dir = is_dir;
    return node;
}

static void free_tree(TreeNode* node) {
    if (!node) return;
    for (size_t i = 0; i < node->child_count; i++) {
        free_tree(node->children[i]);
    }
    free(node->children);
    free(node->name);
    free(node);
}

static int compare_tree_nodes(const void* lhs, const void* rhs) {
    const TreeNode* const* left = lhs;
    const TreeNode* const* right = rhs;
    if ((*left)->is_dir != (*right)->is_dir) {
        return (*right)->is_dir - (*left)->is_dir;
    }
    return strcmp((*left)->name, (*right)->name);
}

static int tree_add_path(TreeNode* root, const char* display_path) {
    char* path_copy = strdup(display_path);
    if (!path_copy) {
        return -1;
    }

    TreeNode* current = root;
    char* saveptr = NULL;
    char* token = strtok_r(path_copy, "/", &saveptr);
    while (token) {
        char* next = strtok_r(NULL, "/", &saveptr);
        int is_dir = (next != NULL);
        TreeNode* child = NULL;

        for (size_t i = 0; i < current->child_count; i++) {
            if (strcmp(current->children[i]->name, token) == 0) {
                child = current->children[i];
                break;
            }
        }

        if (!child) {
            if (current->child_count == current->child_capacity) {
                size_t new_capacity = (current->child_capacity == 0) ? 8 : current->child_capacity * 2;
                TreeNode** new_children = realloc(current->children, new_capacity * sizeof(*new_children));
                if (!new_children) {
                    free(path_copy);
                    return -1;
                }
                current->children = new_children;
                current->child_capacity = new_capacity;
            }

            child = tree_node_create(token, is_dir);
            if (!child) {
                free(path_copy);
                return -1;
            }
            current->children[current->child_count++] = child;
        } else if (is_dir) {
            child->is_dir = 1;
        }

        current = child;
        token = next;
    }

    free(path_copy);
    return 0;
}

static void sort_tree(TreeNode* node) {
    if (!node) return;
    if (node->child_count > 1) {
        qsort(node->children, node->child_count, sizeof(*node->children), compare_tree_nodes);
    }
    for (size_t i = 0; i < node->child_count; i++) {
        sort_tree(node->children[i]);
    }
}

static int write_tree_children(FILE* out,
                               const TreeNode* node,
                               const char* prefix,
                               size_t depth,
                               size_t max_depth) {
    for (size_t i = 0; i < node->child_count; i++) {
        const TreeNode* child = node->children[i];
        int is_last = (i + 1 == node->child_count);

        if (write_text(out, prefix) != 0 ||
            write_text(out, is_last ? TREE_LAST : TREE_BRANCH) != 0 ||
            write_visible_text(out, child->name) != 0 ||
            write_text(out, "\n") != 0) {
            return -1;
        }

        if (child->is_dir &&
            child->child_count > 0 &&
            (max_depth == SIZE_MAX || depth < max_depth)) {
            const char* segment = is_last ? TREE_SPACE : TREE_PIPE;
            size_t prefix_len = strlen(prefix);
            size_t segment_len = strlen(segment);
            char* next_prefix = malloc(prefix_len + segment_len + 1);
            if (!next_prefix) {
                return -1;
            }
            memcpy(next_prefix, prefix, prefix_len);
            memcpy(next_prefix + prefix_len, segment, segment_len + 1);

            int result = write_tree_children(out, child, next_prefix, depth + 1, max_depth);
            free(next_prefix);
            if (result != 0) {
                return -1;
            }
        }
    }

    return 0;
}

int write_project_tree(FILE* out, const ExportPlan* plan, size_t max_depth) {
    TreeNode* root = tree_node_create("", 1);
    if (!root) {
        return -1;
    }

    for (size_t i = 0; i < plan->count; i++) {
        if (tree_add_path(root, plan->entries[i].display_path) != 0) {
            free_tree(root);
            return -1;
        }
    }
    sort_tree(root);

    if (write_text(out, "## Project Tree\n\n```text\n") != 0) {
        free_tree(root);
        return -1;
    }

    if (root->child_count == 0) {
        if (write_text(out, "(no exported files)\n") != 0) {
            free_tree(root);
            return -1;
        }
    } else if (write_tree_children(out, root, "", 1, max_depth) != 0) {
        free_tree(root);
        return -1;
    }

    free_tree(root);
    return write_text(out, "```\n\n");
}

static int render_entry(FILE* out, const ExportEntry* entry) {
    size_t max_run = 0;
    size_t current_run = 0;
    size_t fence = 3;

    for (size_t i = 0; i < entry->buf_len; i++) {
        if (entry->buf[i] == '`') {
            current_run++;
            if (current_run > max_run) max_run = current_run;
        } else {
            current_run = 0;
        }
    }
    fence = (max_run >= 3 ? max_run + 1 : 3);

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

int render_export_plan(FILE* out, const ExportPlan* plan, int verbose) {
    for (size_t i = 0; i < plan->count; i++) {
        if (verbose) {
            fprintf(stderr, "Processing file: %s\n", plan->entries[i].display_path);
        }
        if (render_entry(out, &plan->entries[i]) != 0) {
            if (ferror(out)) {
                perror("Error writing export output");
                return -1;
            }
            fprintf(stderr, "Warning: Failed to render file %s\n", plan->entries[i].display_path);
        }
    }
    return 0;
}
