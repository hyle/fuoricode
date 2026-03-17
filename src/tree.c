#include "tree.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "text_io.h"

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

typedef struct {
    char* data;
    size_t len;
    size_t cap;
} TreePrefixBuffer;

static int add_size(size_t* total, size_t amount) {
    if (*total > SIZE_MAX - amount) {
        errno = EOVERFLOW;
        return -1;
    }
    *total += amount;
    return 0;
}

static int ensure_prefix_capacity(TreePrefixBuffer* prefix, size_t needed_len) {
    if (!prefix) {
        errno = EINVAL;
        return -1;
    }
    if (needed_len + 1 <= prefix->cap) {
        return 0;
    }

    size_t new_cap = (prefix->cap > 0) ? prefix->cap : 16;
    while (new_cap < needed_len + 1) {
        if (new_cap > SIZE_MAX / 2) {
            new_cap = needed_len + 1;
            break;
        }
        new_cap *= 2;
    }

    char* new_data = realloc(prefix->data, new_cap);
    if (!new_data) {
        return -1;
    }
    prefix->data = new_data;
    prefix->cap = new_cap;
    return 0;
}

static int append_prefix_segment(TreePrefixBuffer* prefix, const char* segment) {
    size_t segment_len = strlen(segment);
    size_t needed_len = prefix->len + segment_len;
    if (needed_len < prefix->len) {
        errno = EOVERFLOW;
        return -1;
    }
    if (ensure_prefix_capacity(prefix, needed_len) != 0) {
        return -1;
    }

    memcpy(prefix->data + prefix->len, segment, segment_len);
    prefix->len = needed_len;
    prefix->data[prefix->len] = '\0';
    return 0;
}

static int write_visible_text(FILE* out, const char* text) {
    for (const unsigned char* p = (const unsigned char*)text; *p != '\0'; p++) {
        unsigned char c = *p;
        if (c == '\n') {
            if (fuori_write_text(out, "\\n") != 0) return -1;
            continue;
        }
        if (c == '\r') {
            if (fuori_write_text(out, "\\r") != 0) return -1;
            continue;
        }
        if (c == '\t') {
            if (fuori_write_text(out, "\\t") != 0) return -1;
            continue;
        }
        if ((c < 0x20 || c == 0x7f)) {
            char escaped[5];
            if (snprintf(escaped, sizeof(escaped), "\\x%02X", c) < 0 ||
                fuori_write_text(out, escaped) != 0) {
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

static int count_visible_text_bytes(size_t* total, const char* text) {
    for (const unsigned char* p = (const unsigned char*)text; *p != '\0'; p++) {
        unsigned char c = *p;
        if (c == '\n' || c == '\r' || c == '\t') {
            if (add_size(total, 2) != 0) return -1;
            continue;
        }
        if ((c < 0x20 || c == 0x7f)) {
            if (add_size(total, 4) != 0) return -1;
            continue;
        }
        if (add_size(total, 1) != 0) return -1;
    }
    return 0;
}

static size_t max_backtick_run_in_text(const char* text) {
    size_t max_run = 0;
    size_t current_run = 0;

    if (!text) {
        return 0;
    }

    for (const unsigned char* p = (const unsigned char*)text; *p != '\0'; p++) {
        if (*p == '`') {
            current_run++;
            if (current_run > max_run) {
                max_run = current_run;
            }
        } else {
            current_run = 0;
        }
    }

    return max_run;
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
    if (display_path[0] == '/') {
        TreeNode* absolute_root = NULL;

        for (size_t i = 0; i < current->child_count; i++) {
            if (strcmp(current->children[i]->name, "/") == 0) {
                absolute_root = current->children[i];
                break;
            }
        }

        if (!absolute_root) {
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

            absolute_root = tree_node_create("/", 1);
            if (!absolute_root) {
                free(path_copy);
                return -1;
            }
            current->children[current->child_count++] = absolute_root;
        }

        current = absolute_root;
    }

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

static size_t max_backtick_run_in_tree(const TreeNode* node) {
    size_t max_run = max_backtick_run_in_text(node ? node->name : NULL);

    if (!node) {
        return 0;
    }

    for (size_t i = 0; i < node->child_count; i++) {
        size_t child_run = max_backtick_run_in_tree(node->children[i]);
        if (child_run > max_run) {
            max_run = child_run;
        }
    }

    return max_run;
}

static size_t compute_tree_fence_length(const TreeNode* node) {
    size_t max_run = max_backtick_run_in_tree(node);
    return (max_run >= 3) ? max_run + 1 : 3;
}

static int write_tree_open_fence(FILE* out, size_t fence_len) {
    for (size_t i = 0; i < fence_len; i++) {
        if (fputc('`', out) == EOF) {
            return -1;
        }
    }
    return fuori_write_text(out, "text\n");
}

static int write_tree_close_fence(FILE* out, size_t fence_len) {
    for (size_t i = 0; i < fence_len; i++) {
        if (fputc('`', out) == EOF) {
            return -1;
        }
    }
    return fuori_write_text(out, "\n\n");
}

static int count_tree_open_fence_bytes(size_t* total, size_t fence_len) {
    if (add_size(total, fence_len) != 0) {
        return -1;
    }
    return add_size(total, strlen("text\n"));
}

static int count_tree_close_fence_bytes(size_t* total, size_t fence_len) {
    if (add_size(total, fence_len) != 0) {
        return -1;
    }
    return add_size(total, 2);
}

static int write_tree_children(FILE* out,
                               const TreeNode* node,
                               TreePrefixBuffer* prefix,
                               size_t depth,
                               size_t max_depth) {
    for (size_t i = 0; i < node->child_count; i++) {
        const TreeNode* child = node->children[i];
        int is_last = (i + 1 == node->child_count);

        if (fuori_write_text(out, prefix->data ? prefix->data : "") != 0 ||
            fuori_write_text(out, is_last ? TREE_LAST : TREE_BRANCH) != 0 ||
            write_visible_text(out, child->name) != 0 ||
            fuori_write_text(out, "\n") != 0) {
            return -1;
        }

        if (child->is_dir &&
            child->child_count > 0 &&
            (max_depth == SIZE_MAX || depth < max_depth)) {
            const char* segment = is_last ? TREE_SPACE : TREE_PIPE;
            size_t saved_len = prefix->len;
            if (append_prefix_segment(prefix, segment) != 0) {
                return -1;
            }
            int result = write_tree_children(out, child, prefix, depth + 1, max_depth);
            prefix->len = saved_len;
            if (prefix->data) {
                prefix->data[prefix->len] = '\0';
            }
            if (result != 0) {
                return -1;
            }
        }
    }

    return 0;
}

static int count_tree_children_bytes(const TreeNode* node,
                                     size_t prefix_len,
                                     size_t depth,
                                     size_t max_depth,
                                     size_t* total) {
    for (size_t i = 0; i < node->child_count; i++) {
        const TreeNode* child = node->children[i];
        int is_last = (i + 1 == node->child_count);
        const char* branch = is_last ? TREE_LAST : TREE_BRANCH;

        if (add_size(total, prefix_len) != 0 ||
            add_size(total, strlen(branch)) != 0 ||
            count_visible_text_bytes(total, child->name) != 0 ||
            add_size(total, 1) != 0) {
            return -1;
        }

        if (child->is_dir &&
            child->child_count > 0 &&
            (max_depth == SIZE_MAX || depth < max_depth)) {
            const char* segment = is_last ? TREE_SPACE : TREE_PIPE;
            size_t next_prefix_len = prefix_len + strlen(segment);
            if (next_prefix_len < prefix_len) {
                errno = EOVERFLOW;
                return -1;
            }
            if (count_tree_children_bytes(child, next_prefix_len, depth + 1, max_depth, total) != 0) {
                return -1;
            }
        }
    }

    return 0;
}

int write_project_tree_filtered(FILE* out,
                                const ExportPlan* plan,
                                const unsigned char* include_mask,
                                size_t max_depth) {
    TreeNode* root = tree_node_create("", 1);
    TreePrefixBuffer prefix = {0};
    size_t fence_len = 3;
    int result = -1;
    if (!root) {
        return -1;
    }

    for (size_t i = 0; i < plan->count; i++) {
        if (include_mask && !include_mask[i]) {
            continue;
        }
        if (tree_add_path(root, plan->entries[i].display_path) != 0) {
            free_tree(root);
            return -1;
        }
    }
    sort_tree(root);
    fence_len = compute_tree_fence_length(root);

    if (fuori_write_text(out, "## Project Tree\n\n") != 0 ||
        write_tree_open_fence(out, fence_len) != 0) {
        free_tree(root);
        return -1;
    }

    if (root->child_count == 0) {
        if (fuori_write_text(out, "(no exported files)\n") != 0) {
            goto cleanup;
        }
    } else {
        if (ensure_prefix_capacity(&prefix, 0) != 0) {
            goto cleanup;
        }
        prefix.data[0] = '\0';
        if (write_tree_children(out, root, &prefix, 1, max_depth) != 0) {
            goto cleanup;
        }
    }

    result = write_tree_close_fence(out, fence_len);

cleanup:
    free(prefix.data);
    free_tree(root);
    return result;
}

int count_project_tree_bytes_filtered(const ExportPlan* plan,
                                      const unsigned char* include_mask,
                                      size_t max_depth,
                                      size_t* total) {
    TreeNode* root = tree_node_create("", 1);
    size_t fence_len = 3;
    if (!root) {
        return -1;
    }

    for (size_t i = 0; i < plan->count; i++) {
        if (include_mask && !include_mask[i]) {
            continue;
        }
        if (tree_add_path(root, plan->entries[i].display_path) != 0) {
            free_tree(root);
            return -1;
        }
    }
    sort_tree(root);
    fence_len = compute_tree_fence_length(root);

    if (fuori_count_text_bytes(total, "## Project Tree\n\n") != 0 ||
        count_tree_open_fence_bytes(total, fence_len) != 0) {
        free_tree(root);
        return -1;
    }

    if (root->child_count == 0) {
        if (fuori_count_text_bytes(total, "(no exported files)\n") != 0) {
            free_tree(root);
            return -1;
        }
    } else if (count_tree_children_bytes(root, 0, 1, max_depth, total) != 0) {
        free_tree(root);
        return -1;
    }

    free_tree(root);
    return count_tree_close_fence_bytes(total, fence_len);
}

int write_project_tree(FILE* out, const ExportPlan* plan, size_t max_depth) {
    return write_project_tree_filtered(out, plan, NULL, max_depth);
}

int count_project_tree_bytes(const ExportPlan* plan, size_t max_depth, size_t* total) {
    return count_project_tree_bytes_filtered(plan, NULL, max_depth, total);
}
