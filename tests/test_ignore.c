#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ignore.h"

typedef struct {
    const char* name;
    const char* path;
    int is_dir;
    int initial_ignored;
    int expected;
    const char* patterns[8];
} IgnoreCase;

static int run_case(const IgnoreCase* test_case) {
    IgnorePattern* patterns = NULL;
    size_t count = 0;
    int fd = -1;
    FILE* file = NULL;
    char template[] = "/tmp/fuori-ignore-test.XXXXXX";
    int actual;
    int failed = 0;

    while (count < 8 && test_case->patterns[count] != NULL) {
        count++;
    }

    fd = mkstemp(template);
    if (fd < 0) {
        perror("mkstemp");
        return 1;
    }

    file = fdopen(fd, "w");
    if (!file) {
        perror("fdopen");
        close(fd);
        unlink(template);
        return 1;
    }
    fd = -1;

    for (size_t i = 0; i < count; i++) {
        if (fprintf(file, "%s\n", test_case->patterns[i]) < 0) {
            perror("fprintf");
            fclose(file);
            unlink(template);
            return 1;
        }
    }
    if (fclose(file) != 0) {
        perror("fclose");
        unlink(template);
        return 1;
    }

    if (load_ignore_patterns(template, &patterns, &count) != 0) {
        perror("load_ignore_patterns");
        unlink(template);
        return 1;
    }

    actual = resolve_ignore_state(test_case->path,
                                  patterns,
                                  count,
                                  test_case->is_dir,
                                  test_case->initial_ignored);
    if (actual != test_case->expected) {
        fprintf(stderr,
                "FAIL %-28s path=%s expected=%d actual=%d\n",
                test_case->name,
                test_case->path,
                test_case->expected,
                actual);
        failed = 1;
    }

    free_ignore_patterns(patterns, count);
    unlink(template);
    return failed;
}

int main(void) {
    const IgnoreCase cases[] = {
        {
            .name = "basename directory ignore",
            .path = "apps/web/node_modules",
            .is_dir = 1,
            .expected = 1,
            .patterns = {"node_modules/", NULL}
        },
        {
            .name = "recursive double-star directory",
            .path = "apps/web/node_modules",
            .is_dir = 1,
            .expected = 1,
            .patterns = {"**/node_modules/", NULL}
        },
        {
            .name = "recursive double-star file",
            .path = "pkg/cache/module.pyc",
            .is_dir = 0,
            .expected = 1,
            .patterns = {"**/*.pyc", NULL}
        },
        {
            .name = "double-star matches root file",
            .path = "module.pyc",
            .is_dir = 0,
            .expected = 1,
            .patterns = {"**/*.pyc", NULL}
        },
        {
            .name = "root anchored directory only",
            .path = "src/cache-dir",
            .is_dir = 1,
            .expected = 0,
            .patterns = {"/cache-dir/", NULL}
        },
        {
            .name = "root anchored root match",
            .path = "cache-dir",
            .is_dir = 1,
            .expected = 1,
            .patterns = {"/cache-dir/", NULL}
        },
        {
            .name = "slash pattern stays rooted",
            .path = "lib/src/main.c",
            .is_dir = 0,
            .expected = 0,
            .patterns = {"src/*.c", NULL}
        },
        {
            .name = "slash pattern root hit",
            .path = "src/main.c",
            .is_dir = 0,
            .expected = 1,
            .patterns = {"src/*.c", NULL}
        },
        {
            .name = "negation restores file",
            .path = "src/keep.pyc",
            .is_dir = 0,
            .expected = 0,
            .patterns = {"**/*.pyc", "!src/keep.pyc", NULL}
        },
        {
            .name = "ignored parent blocks basename negation",
            .path = "build/keep.txt",
            .is_dir = 0,
            .initial_ignored = 1,
            .expected = 1,
            .patterns = {"build/", "!keep.txt", NULL}
        }
    };

    int failures = 0;
    size_t count = sizeof(cases) / sizeof(cases[0]);
    for (size_t i = 0; i < count; i++) {
        failures += run_case(&cases[i]);
    }

    if (failures != 0) {
        return 1;
    }

    printf("ignore tests passed (%zu cases)\n", count);
    return 0;
}
