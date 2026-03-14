#include "git_paths.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int compare_selected_paths(const void* lhs, const void* rhs) {
    const SelectedPath* left = lhs;
    const SelectedPath* right = rhs;
    int display_cmp = strcmp(left->display_path, right->display_path);
    if (display_cmp != 0) {
        return display_cmp;
    }
    return strcmp(left->open_path, right->open_path);
}

void free_selected_paths(SelectedPath* paths, size_t count) {
    if (!paths) return;
    for (size_t i = 0; i < count; i++) {
        free(paths[i].open_path);
        free(paths[i].display_path);
    }
    free(paths);
}

static int run_command_capture(const char* const argv[],
                               unsigned char** output,
                               size_t* output_len,
                               int* exit_status,
                               int* exec_errno) {
    int stdout_pipe[2];
    int error_pipe[2];
    pid_t pid;
    unsigned char* buffer = NULL;
    size_t used = 0;
    size_t capacity = 0;
    int status = -1;

    *output = NULL;
    *output_len = 0;
    *exit_status = -1;
    *exec_errno = 0;

    if (pipe(stdout_pipe) == -1) {
        return -1;
    }
    if (pipe(error_pipe) == -1) {
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        return -1;
    }
    if (fcntl(error_pipe[1], F_SETFD, FD_CLOEXEC) == -1) {
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        close(error_pipe[0]);
        close(error_pipe[1]);
        return -1;
    }

    pid = fork();
    if (pid == -1) {
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        close(error_pipe[0]);
        close(error_pipe[1]);
        return -1;
    }

    if (pid == 0) {
        int child_errno;
        close(stdout_pipe[0]);
        close(error_pipe[0]);

        if (dup2(stdout_pipe[1], STDOUT_FILENO) == -1) {
            child_errno = errno;
            write(error_pipe[1], &child_errno, sizeof(child_errno));
            _exit(127);
        }

        close(stdout_pipe[1]);
        execvp(argv[0], (char* const*)argv);

        child_errno = errno;
        write(error_pipe[1], &child_errno, sizeof(child_errno));
        close(error_pipe[1]);
        _exit(127);
    }

    close(stdout_pipe[1]);
    close(error_pipe[1]);

    while (1) {
        unsigned char chunk[4096];
        ssize_t read_len = read(stdout_pipe[0], chunk, sizeof(chunk));
        if (read_len == 0) {
            break;
        }
        if (read_len < 0) {
            if (errno == EINTR) {
                continue;
            }
            goto cleanup;
        }
        if (used + (size_t)read_len < used) {
            errno = EOVERFLOW;
            goto cleanup;
        }
        if (used + (size_t)read_len > capacity) {
            size_t new_capacity = (capacity == 0) ? 4096 : capacity;
            while (new_capacity < used + (size_t)read_len) {
                if (new_capacity > SIZE_MAX / 2) {
                    new_capacity = used + (size_t)read_len;
                    break;
                }
                new_capacity *= 2;
            }
            unsigned char* new_buffer = realloc(buffer, new_capacity);
            if (!new_buffer) {
                goto cleanup;
            }
            buffer = new_buffer;
            capacity = new_capacity;
        }
        memcpy(buffer + used, chunk, (size_t)read_len);
        used += (size_t)read_len;
    }

    while (1) {
        ssize_t error_len = read(error_pipe[0], exec_errno, sizeof(*exec_errno));
        if (error_len == 0) {
            break;
        }
        if (error_len < 0) {
            if (errno == EINTR) {
                continue;
            }
            goto cleanup;
        }
        break;
    }

    while (waitpid(pid, exit_status, 0) == -1) {
        if (errno != EINTR) {
            goto cleanup;
        }
    }

    *output = buffer;
    *output_len = used;
    status = 0;
    buffer = NULL;

cleanup:
    free(buffer);
    close(stdout_pipe[0]);
    close(error_pipe[0]);
    return status;
}

static int capture_git_line(const char* repo_root, const char* rev_parse_arg, char** line_out) {
    const char* const argv[] = {"git", "-C", repo_root, "rev-parse", rev_parse_arg, NULL};
    unsigned char* output = NULL;
    size_t output_len = 0;
    int exit_status = 0;
    int exec_errno = 0;

    *line_out = NULL;
    if (run_command_capture(argv, &output, &output_len, &exit_status, &exec_errno) != 0) {
        perror("Error running git");
        return -1;
    }
    if (exec_errno != 0) {
        errno = exec_errno;
        perror("Error executing git");
        free(output);
        return -1;
    }
    if (!WIFEXITED(exit_status) || WEXITSTATUS(exit_status) != 0) {
        errno = 0;
        fprintf(stderr, "git rev-parse failed for %s\n", rev_parse_arg);
        free(output);
        return -1;
    }

    while (output_len > 0 &&
           (output[output_len - 1] == '\n' || output[output_len - 1] == '\r')) {
        output_len--;
    }

    char* line = malloc(output_len + 1);
    if (!line) {
        free(output);
        return -1;
    }
    memcpy(line, output, output_len);
    line[output_len] = '\0';

    free(output);
    *line_out = line;
    return 0;
}

int collect_git_paths(FileSelectionMode mode,
                      const char* diff_range,
                      SelectedPath** paths_out,
                      size_t* count_out) {
    char* repo_root = NULL;
    char* prefix = NULL;
    const char* diff_filter = "--diff-filter=AMR";
    const char* const* argv = NULL;
    const char* args[12];
    unsigned char* output = NULL;
    size_t output_len = 0;
    int exit_status = 0;
    int exec_errno = 0;
    SelectedPath* paths = NULL;
    size_t count = 0;
    size_t capacity = 0;
    int status = -1;

    *paths_out = NULL;
    *count_out = 0;

    errno = 0;
    if (capture_git_line(".", "--show-toplevel", &repo_root) != 0) {
        if (errno == ENOENT) {
            fprintf(stderr, "Git file-selection modes require git to be installed\n");
        } else {
            fprintf(stderr, "Git file-selection modes require a Git repository\n");
        }
        goto cleanup;
    }
    if (capture_git_line(".", "--show-prefix", &prefix) != 0) {
        goto cleanup;
    }

    size_t argc = 0;
    args[argc++] = "git";
    args[argc++] = "-C";
    args[argc++] = repo_root;
    args[argc++] = "diff";
    if (mode == FILE_SELECTION_GIT_STAGED) {
        args[argc++] = "--cached";
    }
    args[argc++] = "--name-only";
    args[argc++] = diff_filter;
    args[argc++] = "-z";
    if (mode == FILE_SELECTION_GIT_DIFF) {
        args[argc++] = diff_range;
    }
    if (prefix[0] != '\0') {
        args[argc++] = "--";
        args[argc++] = prefix;
    }
    args[argc] = NULL;
    argv = args;

    if (run_command_capture(argv, &output, &output_len, &exit_status, &exec_errno) != 0) {
        perror("Error running git");
        goto cleanup;
    }
    if (exec_errno != 0) {
        errno = exec_errno;
        perror("Error executing git");
        goto cleanup;
    }
    if (!WIFEXITED(exit_status) || WEXITSTATUS(exit_status) != 0) {
        fprintf(stderr, "git diff failed for the requested file-selection mode\n");
        goto cleanup;
    }

    size_t start = 0;
    size_t prefix_len = strlen(prefix);
    size_t repo_root_len = strlen(repo_root);
    while (start < output_len) {
        size_t end = start;
        while (end < output_len && output[end] != '\0') {
            end++;
        }
        if (end > start) {
            size_t repo_rel_len = end - start;
            const char* repo_rel = (const char*)(output + start);
            const char* display_rel = repo_rel;

            if (prefix_len > 0 &&
                strncmp(repo_rel, prefix, prefix_len) == 0) {
                display_rel = repo_rel + prefix_len;
            }

            if (count == capacity) {
                size_t new_capacity = (capacity == 0) ? 16 : capacity * 2;
                SelectedPath* new_paths = realloc(paths, new_capacity * sizeof(*paths));
                if (!new_paths) {
                    goto cleanup;
                }
                paths = new_paths;
                capacity = new_capacity;
            }

            size_t open_path_len = repo_root_len + 1 + repo_rel_len;
            paths[count].open_path = malloc(open_path_len + 1);
            if (!paths[count].open_path) {
                goto cleanup;
            }
            memcpy(paths[count].open_path, repo_root, repo_root_len);
            paths[count].open_path[repo_root_len] = '/';
            memcpy(paths[count].open_path + repo_root_len + 1, repo_rel, repo_rel_len);
            paths[count].open_path[open_path_len] = '\0';

            paths[count].display_path = strdup(display_rel);
            if (!paths[count].display_path) {
                free(paths[count].open_path);
                paths[count].open_path = NULL;
                goto cleanup;
            }
            count++;
        }
        start = end + 1;
    }

    if (count > 1) {
        qsort(paths, count, sizeof(*paths), compare_selected_paths);
        size_t unique_count = 1;
        for (size_t i = 1; i < count; i++) {
            if (strcmp(paths[i - 1].open_path, paths[i].open_path) == 0 &&
                strcmp(paths[i - 1].display_path, paths[i].display_path) == 0) {
                free(paths[i].open_path);
                free(paths[i].display_path);
                paths[i].open_path = NULL;
                paths[i].display_path = NULL;
                continue;
            }
            if (unique_count != i) {
                paths[unique_count] = paths[i];
                paths[i].open_path = NULL;
                paths[i].display_path = NULL;
            }
            unique_count++;
        }
        count = unique_count;
    }

    *paths_out = paths;
    *count_out = count;
    paths = NULL;
    status = 0;

cleanup:
    free(output);
    free(repo_root);
    free(prefix);
    free_selected_paths(paths, count);
    return status;
}
