#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tree.h"

static unsigned char* dup_bytes(const char* text) {
    size_t len = strlen(text);
    unsigned char* buf = malloc(len);
    if (!buf) {
        return NULL;
    }
    memcpy(buf, text, len);
    return buf;
}

static void free_plan(ExportPlan* plan) {
    if (!plan || !plan->entries) {
        return;
    }
    for (size_t i = 0; i < plan->count; i++) {
        free(plan->entries[i].open_path);
        free(plan->entries[i].display_path);
        free(plan->entries[i].buf);
    }
    free(plan->entries);
    plan->entries = NULL;
    plan->count = 0;
    plan->capacity = 0;
}

static int build_plan(ExportPlan* plan) {
    static const char* const paths[] = {
        "alpha/deep/leaf.txt",
        "alpha/shallow.txt",
        "beta.txt",
        "tick/```name.txt"
    };

    memset(plan, 0, sizeof(*plan));
    plan->count = sizeof(paths) / sizeof(paths[0]);
    plan->capacity = plan->count;
    plan->entries = calloc(plan->count, sizeof(*plan->entries));
    if (!plan->entries) {
        return -1;
    }

    for (size_t i = 0; i < plan->count; i++) {
        plan->entries[i].open_path = strdup(paths[i]);
        plan->entries[i].display_path = strdup(paths[i]);
        plan->entries[i].buf = dup_bytes("x\n");
        plan->entries[i].buf_len = 2;
        if (!plan->entries[i].open_path ||
            !plan->entries[i].display_path ||
            !plan->entries[i].buf) {
            free_plan(plan);
            return -1;
        }
    }

    return 0;
}

static char* capture_tree_output(const ExportPlan* plan, size_t max_depth) {
    FILE* out = tmpfile();
    long size = 0;
    char* buffer = NULL;

    if (!out) {
        return NULL;
    }
    if (write_project_tree(out, plan, max_depth) != 0) {
        fclose(out);
        return NULL;
    }
    if (fflush(out) != 0 || fseek(out, 0, SEEK_END) != 0) {
        fclose(out);
        return NULL;
    }

    size = ftell(out);
    if (size < 0 || fseek(out, 0, SEEK_SET) != 0) {
        fclose(out);
        return NULL;
    }

    buffer = malloc((size_t)size + 1);
    if (!buffer) {
        fclose(out);
        return NULL;
    }

    if (size > 0 && fread(buffer, 1, (size_t)size, out) != (size_t)size) {
        free(buffer);
        fclose(out);
        return NULL;
    }
    buffer[size] = '\0';
    fclose(out);
    return buffer;
}

static int assert_contains(const char* haystack, const char* needle, const char* label) {
    if (!strstr(haystack, needle)) {
        fprintf(stderr, "FAIL missing %s: %s\n", label, needle);
        return 1;
    }
    return 0;
}

static int assert_not_contains(const char* haystack, const char* needle, const char* label) {
    if (strstr(haystack, needle)) {
        fprintf(stderr, "FAIL unexpected %s: %s\n", label, needle);
        return 1;
    }
    return 0;
}

int main(void) {
    ExportPlan plan;
    char* full_output = NULL;
    char* shallow_output = NULL;
    int failures = 0;

    if (build_plan(&plan) != 0) {
        perror("build_plan");
        return 1;
    }

    full_output = capture_tree_output(&plan, (size_t)-1);
    shallow_output = capture_tree_output(&plan, 1);
    if (!full_output || !shallow_output) {
        perror("capture_tree_output");
        free(full_output);
        free(shallow_output);
        free_plan(&plan);
        return 1;
    }

    failures += assert_contains(full_output, "├── alpha\n", "root alpha branch");
    failures += assert_contains(full_output, "│   ├── deep\n", "nested deep branch");
    failures += assert_contains(full_output, "│   └── shallow.txt\n", "sibling shallow branch");
    failures += assert_contains(full_output, "├── tick\n", "backtick directory branch");
    failures += assert_contains(full_output, "│   └── ```name.txt\n", "backtick filename branch");
    failures += assert_contains(full_output, "└── beta.txt\n", "root beta sibling");
    failures += assert_not_contains(full_output, "│       └── shallow.txt\n", "leaked deep prefix");
    failures += assert_contains(full_output, "````text\n", "expanded tree fence");

    failures += assert_contains(shallow_output, "├── alpha\n", "depth-limited alpha");
    failures += assert_contains(shallow_output, "├── tick\n", "depth-limited tick");
    failures += assert_contains(shallow_output, "└── beta.txt\n", "depth-limited beta");
    failures += assert_not_contains(shallow_output, "deep\n", "depth-limited deep omission");
    failures += assert_not_contains(shallow_output, "shallow.txt\n", "depth-limited shallow omission");
    failures += assert_contains(shallow_output, "````text\n", "depth-limited expanded tree fence");

    free(full_output);
    free(shallow_output);
    free_plan(&plan);

    if (failures != 0) {
        return 1;
    }

    printf("tree tests passed\n");
    return 0;
}
