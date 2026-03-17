#include "git_paths.h"

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef enum {
    GIT_PROBE_READY = 0,
    GIT_PROBE_FALLBACK
} GitProbeResult;

#define GIT_SELECTION_ARGS_MAX 13
#define GIT_HUNK_ARGS_MAX 13

typedef struct {
    char* repo_root;
    char* prefix;
} GitRepoPaths;

static int compare_selected_paths(const void* lhs, const void* rhs) {
    const SelectedPath* left = lhs;
    const SelectedPath* right = rhs;
    int display_cmp = strcmp(left->display_path, right->display_path);
    if (display_cmp != 0) {
        return display_cmp;
    }
    int open_cmp = strcmp(left->open_path, right->open_path);
    if (open_cmp != 0) {
        return open_cmp;
    }
    const char* left_repo_rel = left->repo_rel_path ? left->repo_rel_path : "";
    const char* right_repo_rel = right->repo_rel_path ? right->repo_rel_path : "";
    int repo_rel_cmp = strcmp(left_repo_rel, right_repo_rel);
    if (repo_rel_cmp != 0) {
        return repo_rel_cmp;
    }
    if (left->change_type != right->change_type) {
        return (left->change_type < right->change_type) ? -1 : 1;
    }
    const char* left_previous = left->previous_display_path ? left->previous_display_path : "";
    const char* right_previous = right->previous_display_path ? right->previous_display_path : "";
    int previous_cmp = strcmp(left_previous, right_previous);
    if (previous_cmp != 0) {
        return previous_cmp;
    }
    return strcmp(left->previous_repo_rel_path ? left->previous_repo_rel_path : "",
                  right->previous_repo_rel_path ? right->previous_repo_rel_path : "");
}

void free_selected_paths(SelectedPath* paths, size_t count) {
    if (!paths) return;
    for (size_t i = 0; i < count; i++) {
        free(paths[i].open_path);
        free(paths[i].display_path);
        free(paths[i].repo_rel_path);
        free(paths[i].previous_display_path);
        free(paths[i].previous_repo_rel_path);
    }
    free(paths);
}

static int normalize_selected_paths(SelectedPath* paths, size_t* count) {
    if (!count) {
        errno = EINVAL;
        return -1;
    }

    if (!paths || *count == 0) {
        return 0;
    }

    if (*count > 1) {
        qsort(paths, *count, sizeof(*paths), compare_selected_paths);
        size_t unique_count = 1;
        for (size_t i = 1; i < *count; i++) {
            if (strcmp(paths[i - 1].open_path, paths[i].open_path) == 0 &&
                strcmp(paths[i - 1].display_path, paths[i].display_path) == 0 &&
                strcmp(paths[i - 1].repo_rel_path ? paths[i - 1].repo_rel_path : "",
                       paths[i].repo_rel_path ? paths[i].repo_rel_path : "") == 0 &&
                paths[i - 1].change_type == paths[i].change_type &&
                strcmp(paths[i - 1].previous_display_path ? paths[i - 1].previous_display_path : "",
                       paths[i].previous_display_path ? paths[i].previous_display_path : "") == 0 &&
                strcmp(paths[i - 1].previous_repo_rel_path ? paths[i - 1].previous_repo_rel_path : "",
                       paths[i].previous_repo_rel_path ? paths[i].previous_repo_rel_path : "") == 0) {
                free(paths[i].open_path);
                free(paths[i].display_path);
                free(paths[i].repo_rel_path);
                free(paths[i].previous_display_path);
                free(paths[i].previous_repo_rel_path);
                paths[i].open_path = NULL;
                paths[i].display_path = NULL;
                paths[i].repo_rel_path = NULL;
                paths[i].previous_display_path = NULL;
                paths[i].previous_repo_rel_path = NULL;
                continue;
            }
            if (unique_count != i) {
                paths[unique_count] = paths[i];
                paths[i].open_path = NULL;
                paths[i].display_path = NULL;
                paths[i].repo_rel_path = NULL;
                paths[i].previous_display_path = NULL;
                paths[i].previous_repo_rel_path = NULL;
            }
            unique_count++;
        }
        *count = unique_count;
    }

    return 0;
}

static int run_command_capture(const char* const argv[],
                               int suppress_stderr,
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
        int null_fd = -1;
        close(stdout_pipe[0]);
        close(error_pipe[0]);

        if (dup2(stdout_pipe[1], STDOUT_FILENO) == -1) {
            child_errno = errno;
            write(error_pipe[1], &child_errno, sizeof(child_errno));
            _exit(127);
        }

        if (suppress_stderr) {
            null_fd = open("/dev/null", O_WRONLY);
            if (null_fd == -1 || dup2(null_fd, STDERR_FILENO) == -1) {
                child_errno = errno;
                write(error_pipe[1], &child_errno, sizeof(child_errno));
                if (null_fd != -1) {
                    close(null_fd);
                }
                _exit(127);
            }
            close(null_fd);
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
            size_t new_capacity = (capacity == 0) ? 65536 : capacity;
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

static int capture_git_line(const char* repo_root,
                            const char* rev_parse_arg,
                            int quiet_probe,
                            char** line_out,
                            GitProbeResult* probe_result) {
    const char* const argv[] = {"git", "-C", repo_root, "rev-parse", rev_parse_arg, NULL};
    unsigned char* output = NULL;
    size_t output_len = 0;
    int exit_status = 0;
    int exec_errno = 0;

    *line_out = NULL;
    if (probe_result) {
        *probe_result = GIT_PROBE_READY;
    }
    if (run_command_capture(argv, quiet_probe, &output, &output_len, &exit_status, &exec_errno) != 0) {
        perror("Error running git");
        return -1;
    }
    if (exec_errno != 0) {
        errno = exec_errno;
        if (quiet_probe && errno == ENOENT) {
            if (probe_result) {
                *probe_result = GIT_PROBE_FALLBACK;
            }
        } else {
            perror("Error executing git");
        }
        free(output);
        return -1;
    }
    if (!WIFEXITED(exit_status) || WEXITSTATUS(exit_status) != 0) {
        errno = 0;
        if (quiet_probe) {
            if (probe_result) {
                *probe_result = GIT_PROBE_FALLBACK;
            }
        } else {
            fprintf(stderr, "git rev-parse failed for %s\n", rev_parse_arg);
        }
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

static void free_git_repo_paths(GitRepoPaths* repo) {
    if (!repo) {
        return;
    }
    free(repo->repo_root);
    free(repo->prefix);
    repo->repo_root = NULL;
    repo->prefix = NULL;
}

static int capture_git_repo_root(int quiet_probe, char** repo_root_out, GitProbeResult* probe_result) {
    return capture_git_line(".", "--show-toplevel", quiet_probe, repo_root_out, probe_result);
}

static int load_git_repo_paths(int quiet_probe, GitRepoPaths* repo, GitProbeResult* probe_result) {
    if (!repo) {
        errno = EINVAL;
        return -1;
    }

    repo->repo_root = NULL;
    repo->prefix = NULL;

    if (capture_git_repo_root(quiet_probe, &repo->repo_root, probe_result) != 0) {
        return -1;
    }
    if (capture_git_line(".", "--show-prefix", quiet_probe, &repo->prefix, probe_result) != 0) {
        free_git_repo_paths(repo);
        return -1;
    }

    return 0;
}

static int copy_path_basename(const char* path, char* buffer, size_t buffer_size) {
    char path_copy[MAX_PATH_LENGTH];
    size_t path_len;
    char* base;

    if (!path || !buffer || buffer_size == 0) {
        errno = EINVAL;
        return -1;
    }

    path_len = strlen(path);
    if (path_len >= sizeof(path_copy)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(path_copy, path, path_len + 1);

    base = basename(path_copy);
    if (!base || base[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    if (snprintf(buffer, buffer_size, "%s", base) < 0 || strlen(base) >= buffer_size) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return 0;
}

static int append_selected_path(SelectedPath** paths,
                                size_t* count,
                                size_t* capacity,
                                const char* repo_root,
                                size_t repo_root_len,
                                const char* prefix,
                                size_t prefix_len,
                                const char* repo_rel,
                                size_t repo_rel_len,
                                SelectedPathChangeType change_type,
                                const char* previous_repo_rel) {
    const char* display_rel = repo_rel;
    const char* previous_display_rel = previous_repo_rel;
    if (prefix_len > 0 &&
        strncmp(repo_rel, prefix, prefix_len) == 0) {
        display_rel = repo_rel + prefix_len;
    }
    if (previous_repo_rel &&
        prefix_len > 0 &&
        strncmp(previous_repo_rel, prefix, prefix_len) == 0) {
        previous_display_rel = previous_repo_rel + prefix_len;
    }

    if (*count == *capacity) {
        size_t new_capacity = (*capacity == 0) ? 16 : *capacity * 2;
        SelectedPath* new_paths = realloc(*paths, new_capacity * sizeof(**paths));
        if (!new_paths) {
            return -1;
        }
        *paths = new_paths;
        *capacity = new_capacity;
    }

    size_t open_path_len = repo_root_len + 1 + repo_rel_len;
    (*paths)[*count].open_path = malloc(open_path_len + 1);
    if (!(*paths)[*count].open_path) {
        return -1;
    }
    memcpy((*paths)[*count].open_path, repo_root, repo_root_len);
    (*paths)[*count].open_path[repo_root_len] = '/';
    memcpy((*paths)[*count].open_path + repo_root_len + 1, repo_rel, repo_rel_len);
    (*paths)[*count].open_path[open_path_len] = '\0';

    (*paths)[*count].display_path = strdup(display_rel);
    if (!(*paths)[*count].display_path) {
        free((*paths)[*count].open_path);
        (*paths)[*count].open_path = NULL;
        return -1;
    }

    (*paths)[*count].repo_rel_path = strdup(repo_rel);
    if (!(*paths)[*count].repo_rel_path) {
        free((*paths)[*count].open_path);
        free((*paths)[*count].display_path);
        (*paths)[*count].open_path = NULL;
        (*paths)[*count].display_path = NULL;
        return -1;
    }

    (*paths)[*count].change_type = change_type;
    (*paths)[*count].previous_display_path = NULL;
    (*paths)[*count].previous_repo_rel_path = NULL;
    if (previous_display_rel) {
        (*paths)[*count].previous_display_path = strdup(previous_display_rel);
        if (!(*paths)[*count].previous_display_path) {
            free((*paths)[*count].open_path);
            free((*paths)[*count].display_path);
            free((*paths)[*count].repo_rel_path);
            (*paths)[*count].open_path = NULL;
            (*paths)[*count].display_path = NULL;
            (*paths)[*count].repo_rel_path = NULL;
            return -1;
        }
    }
    if (previous_repo_rel) {
        (*paths)[*count].previous_repo_rel_path = strdup(previous_repo_rel);
        if (!(*paths)[*count].previous_repo_rel_path) {
            free((*paths)[*count].open_path);
            free((*paths)[*count].display_path);
            free((*paths)[*count].repo_rel_path);
            free((*paths)[*count].previous_display_path);
            (*paths)[*count].open_path = NULL;
            (*paths)[*count].display_path = NULL;
            (*paths)[*count].repo_rel_path = NULL;
            (*paths)[*count].previous_display_path = NULL;
            return -1;
        }
    }

    (*count)++;
    return 0;
}

static int append_literal_selected_path(SelectedPath** paths,
                                        size_t* count,
                                        size_t* capacity,
                                        const char* path,
                                        size_t path_len) {
    if (*count == *capacity) {
        size_t new_capacity = (*capacity == 0) ? 16 : *capacity * 2;
        SelectedPath* new_paths = realloc(*paths, new_capacity * sizeof(**paths));
        if (!new_paths) {
            return -1;
        }
        *paths = new_paths;
        *capacity = new_capacity;
    }

    (*paths)[*count].open_path = malloc(path_len + 1);
    if (!(*paths)[*count].open_path) {
        return -1;
    }
    memcpy((*paths)[*count].open_path, path, path_len);
    (*paths)[*count].open_path[path_len] = '\0';

    (*paths)[*count].display_path = malloc(path_len + 1);
    if (!(*paths)[*count].display_path) {
        free((*paths)[*count].open_path);
        (*paths)[*count].open_path = NULL;
        return -1;
    }
    memcpy((*paths)[*count].display_path, path, path_len);
    (*paths)[*count].display_path[path_len] = '\0';
    (*paths)[*count].repo_rel_path = NULL;
    (*paths)[*count].change_type = SELECTED_PATH_CHANGE_NONE;
    (*paths)[*count].previous_display_path = NULL;
    (*paths)[*count].previous_repo_rel_path = NULL;

    (*count)++;
    return 0;
}

static int parse_name_only_output(const unsigned char* output,
                                  size_t output_len,
                                  const GitRepoPaths* repo,
                                  SelectedPath** paths_out,
                                  size_t* count_out) {
    SelectedPath* paths = NULL;
    size_t count = 0;
    size_t capacity = 0;
    size_t start = 0;
    size_t prefix_len = strlen(repo->prefix);
    size_t repo_root_len = strlen(repo->repo_root);

    while (start < output_len) {
        size_t end = start;
        while (end < output_len && output[end] != '\0') {
            end++;
        }
        if (end > start) {
            const char* repo_rel = (const char*)(output + start);
            size_t repo_rel_len = end - start;
            if (append_selected_path(&paths,
                                     &count,
                                     &capacity,
                                     repo->repo_root,
                                     repo_root_len,
                                     repo->prefix,
                                     prefix_len,
                                     repo_rel,
                                     repo_rel_len,
                                     SELECTED_PATH_CHANGE_NONE,
                                     NULL) != 0) {
                free_selected_paths(paths, count);
                return -1;
            }
        }
        start = end + 1;
    }

    if (normalize_selected_paths(paths, &count) != 0) {
        free_selected_paths(paths, count);
        return -1;
    }

    *paths_out = paths;
    *count_out = count;
    return 0;
}

static SelectedPathChangeType parse_change_type(const char* status) {
    if (!status || status[0] == '\0') {
        return SELECTED_PATH_CHANGE_NONE;
    }

    switch (status[0]) {
        case 'A':
            return SELECTED_PATH_CHANGE_ADDED;
        case 'R':
            return SELECTED_PATH_CHANGE_RENAMED;
        case 'M':
        default:
            return SELECTED_PATH_CHANGE_MODIFIED;
    }
}

static int next_nul_terminated_field(const unsigned char* output,
                                     size_t output_len,
                                     size_t* start,
                                     const char** field_out,
                                     size_t* field_len_out) {
    size_t end;

    if (!output || !start || !field_out || !field_len_out || *start > output_len) {
        errno = EINVAL;
        return -1;
    }
    if (*start == output_len) {
        *field_out = NULL;
        *field_len_out = 0;
        return 0;
    }

    end = *start;
    while (end < output_len && output[end] != '\0') {
        end++;
    }
    if (end == output_len) {
        errno = EINVAL;
        return -1;
    }

    *field_out = (const char*)(output + *start);
    *field_len_out = end - *start;
    *start = end + 1;
    return 1;
}

static int parse_name_status_output(const unsigned char* output,
                                    size_t output_len,
                                    const GitRepoPaths* repo,
                                    SelectedPath** paths_out,
                                    size_t* count_out) {
    SelectedPath* paths = NULL;
    size_t count = 0;
    size_t capacity = 0;
    size_t start = 0;
    size_t prefix_len = strlen(repo->prefix);
    size_t repo_root_len = strlen(repo->repo_root);

    while (start < output_len) {
        const char* status = NULL;
        const char* first_path = NULL;
        const char* second_path = NULL;
        size_t status_len = 0;
        size_t first_path_len = 0;
        size_t second_path_len = 0;
        int field_status = next_nul_terminated_field(output, output_len, &start, &status, &status_len);
        if (field_status <= 0) {
            break;
        }
        if (status_len == 0) {
            continue;
        }
        if (next_nul_terminated_field(output, output_len, &start, &first_path, &first_path_len) <= 0 ||
            first_path_len == 0) {
            free_selected_paths(paths, count);
            errno = EINVAL;
            return -1;
        }

        SelectedPathChangeType change_type = parse_change_type(status);
        if (change_type == SELECTED_PATH_CHANGE_RENAMED) {
            if (next_nul_terminated_field(output, output_len, &start, &second_path, &second_path_len) <= 0 ||
                second_path_len == 0) {
                free_selected_paths(paths, count);
                errno = EINVAL;
                return -1;
            }
            if (append_selected_path(&paths,
                                     &count,
                                     &capacity,
                                     repo->repo_root,
                                     repo_root_len,
                                     repo->prefix,
                                     prefix_len,
                                     second_path,
                                     second_path_len,
                                     change_type,
                                     first_path) != 0) {
                free_selected_paths(paths, count);
                return -1;
            }
            continue;
        }

        if (append_selected_path(&paths,
                                 &count,
                                 &capacity,
                                 repo->repo_root,
                                 repo_root_len,
                                 repo->prefix,
                                 prefix_len,
                                 first_path,
                                 first_path_len,
                                 change_type,
                                 NULL) != 0) {
            free_selected_paths(paths, count);
            return -1;
        }
    }

    if (normalize_selected_paths(paths, &count) != 0) {
        free_selected_paths(paths, count);
        return -1;
    }

    *paths_out = paths;
    *count_out = count;
    return 0;
}

static int parse_git_selected_paths(FileSelectionMode mode,
                                    const unsigned char* output,
                                    size_t output_len,
                                    const GitRepoPaths* repo,
                                    SelectedPath** paths_out,
                                    size_t* count_out) {
    if (!repo) {
        errno = EINVAL;
        return -1;
    }

    if (mode == FILE_SELECTION_GIT_WORKTREE) {
        return parse_name_only_output(output, output_len, repo, paths_out, count_out);
    }

    return parse_name_status_output(output, output_len, repo, paths_out, count_out);
}

static int append_arg(const char** args, size_t args_size, size_t* argc, const char* arg) {
    if (!args || !argc || !arg) {
        errno = EINVAL;
        return -1;
    }
    if (*argc + 1 >= args_size) {
        errno = EOVERFLOW;
        return -1;
    }
    args[(*argc)++] = arg;
    return 0;
}

static int build_git_selection_args(const GitRepoPaths* repo,
                                    FileSelectionMode mode,
                                    const char* diff_range,
                                    const char** args,
                                    size_t args_size) {
    size_t argc = 0;

    if (!repo || !args || args_size < 1) {
        errno = EINVAL;
        return -1;
    }

    if (append_arg(args, args_size, &argc, "git") != 0 ||
        append_arg(args, args_size, &argc, "-C") != 0 ||
        append_arg(args, args_size, &argc, repo->repo_root) != 0 ||
        append_arg(args, args_size, &argc, (mode == FILE_SELECTION_GIT_WORKTREE) ? "ls-files" : "diff") != 0) {
        return -1;
    }
    if (mode == FILE_SELECTION_GIT_WORKTREE) {
        if (append_arg(args, args_size, &argc, "--cached") != 0 ||
            append_arg(args, args_size, &argc, "--others") != 0 ||
            append_arg(args, args_size, &argc, "--exclude-standard") != 0) {
            return -1;
        }
    } else {
        if (mode == FILE_SELECTION_GIT_STAGED) {
            if (append_arg(args, args_size, &argc, "--cached") != 0) {
                return -1;
            }
        }
        if (append_arg(args, args_size, &argc, "--name-status") != 0 ||
            append_arg(args, args_size, &argc, "--diff-filter=AMR") != 0) {
            return -1;
        }
        if (mode == FILE_SELECTION_GIT_DIFF) {
            if (append_arg(args, args_size, &argc, diff_range) != 0) {
                return -1;
            }
        }
    }
    if (append_arg(args, args_size, &argc, "-z") != 0) {
        return -1;
    }
    if (repo->prefix[0] != '\0') {
        if (append_arg(args, args_size, &argc, "--") != 0 ||
            append_arg(args, args_size, &argc, repo->prefix) != 0) {
            return -1;
        }
    }
    args[argc] = NULL;
    return 0;
}

static int append_git_hunk_range(GitFileHunks* hunks, size_t* capacity, size_t new_start, size_t new_count) {
    GitHunkRange* new_ranges;
    size_t new_capacity;

    if (!hunks || !capacity) {
        errno = EINVAL;
        return -1;
    }
    if (hunks->count == *capacity) {
        new_capacity = (*capacity == 0) ? 4 : *capacity * 2;
        new_ranges = realloc(hunks->ranges, new_capacity * sizeof(*new_ranges));
        if (!new_ranges) {
            return -1;
        }
        hunks->ranges = new_ranges;
        *capacity = new_capacity;
    }

    hunks->ranges[hunks->count].new_start = new_start;
    hunks->ranges[hunks->count].new_count = new_count;
    hunks->count++;
    return 0;
}

static int parse_hunk_number(const char** cursor, const char* end, size_t* value_out) {
    size_t value = 0;
    const char* p;

    if (!cursor || !*cursor || !value_out) {
        errno = EINVAL;
        return -1;
    }

    p = *cursor;
    if (p >= end || *p < '0' || *p > '9') {
        errno = EINVAL;
        return -1;
    }

    while (p < end && *p >= '0' && *p <= '9') {
        unsigned digit = (unsigned)(*p - '0');
        if (value > (SIZE_MAX - digit) / 10) {
            errno = EOVERFLOW;
            return -1;
        }
        value = value * 10 + digit;
        p++;
    }

    *cursor = p;
    *value_out = value;
    return 0;
}

static int parse_unified_hunk_header(const char* line,
                                     size_t line_len,
                                     size_t* new_start_out,
                                     size_t* new_count_out) {
    const char* p;
    const char* end;
    size_t ignored = 0;
    size_t new_start = 0;
    size_t new_count = 1;

    if (!line || !new_start_out || !new_count_out) {
        errno = EINVAL;
        return -1;
    }
    if (line_len < 4 || line[0] != '@' || line[1] != '@') {
        return 0;
    }

    p = line + 2;
    end = line + line_len;
    while (p < end && *p == ' ') {
        p++;
    }
    if (p >= end || *p != '-') {
        errno = EINVAL;
        return -1;
    }
    p++;
    if (parse_hunk_number(&p, end, &ignored) != 0) {
        return -1;
    }
    if (p < end && *p == ',') {
        p++;
        if (parse_hunk_number(&p, end, &ignored) != 0) {
            return -1;
        }
    }
    while (p < end && *p == ' ') {
        p++;
    }
    if (p >= end || *p != '+') {
        errno = EINVAL;
        return -1;
    }
    p++;
    if (parse_hunk_number(&p, end, &new_start) != 0) {
        return -1;
    }
    if (p < end && *p == ',') {
        p++;
        if (parse_hunk_number(&p, end, &new_count) != 0) {
            return -1;
        }
    }
    while (p < end && *p == ' ') {
        p++;
    }
    if ((size_t)(end - p) < 2 || p[0] != '@' || p[1] != '@') {
        errno = EINVAL;
        return -1;
    }

    *new_start_out = new_start;
    *new_count_out = new_count;
    return 1;
}

static int parse_hunk_ranges_from_diff_output(const unsigned char* output,
                                              size_t output_len,
                                              GitFileHunks* hunks) {
    size_t start = 0;
    size_t capacity = 0;

    if (!hunks) {
        errno = EINVAL;
        return -1;
    }

    while (start < output_len) {
        size_t end = start;
        size_t new_start = 0;
        size_t new_count = 0;
        int header_status;

        while (end < output_len && output[end] != '\n') {
            end++;
        }
        if (end > start && output[end - 1] == '\r') {
            end--;
        }

        header_status = parse_unified_hunk_header((const char*)(output + start),
                                                  end - start,
                                                  &new_start,
                                                  &new_count);
        if (header_status < 0) {
            return -1;
        }
        if (header_status > 0 &&
            append_git_hunk_range(hunks, &capacity, new_start, new_count) != 0) {
            return -1;
        }

        start = (end < output_len && output[end] == '\n') ? end + 1 : output_len;
    }

    return 0;
}

static int derive_repo_root_from_selected_paths(const SelectedPath* paths,
                                                size_t path_count,
                                                char** repo_root_out) {
    for (size_t i = 0; i < path_count; i++) {
        const SelectedPath* path = &paths[i];
        size_t open_len;
        size_t repo_rel_len;
        size_t repo_root_len;
        char* repo_root;

        if (!path->open_path || !path->repo_rel_path) {
            continue;
        }

        open_len = strlen(path->open_path);
        repo_rel_len = strlen(path->repo_rel_path);
        if (open_len <= repo_rel_len ||
            strcmp(path->open_path + open_len - repo_rel_len, path->repo_rel_path) != 0 ||
            path->open_path[open_len - repo_rel_len - 1] != '/') {
            errno = EINVAL;
            return -1;
        }

        repo_root_len = open_len - repo_rel_len - 1;
        if (repo_root_len == 0) {
            repo_root = strdup("/");
        } else {
            repo_root = malloc(repo_root_len + 1);
            if (repo_root) {
                memcpy(repo_root, path->open_path, repo_root_len);
                repo_root[repo_root_len] = '\0';
            }
        }
        if (!repo_root) {
            return -1;
        }

        *repo_root_out = repo_root;
        return 0;
    }

    errno = EINVAL;
    return -1;
}

static int build_git_hunk_args(const char* repo_root,
                               FileSelectionMode mode,
                               const char* diff_range,
                               const SelectedPath* path,
                               const char* repo_rel_path,
                               const char** args,
                               size_t args_size) {
    size_t argc = 0;

    if (!repo_root || !path || !repo_rel_path || !args || args_size < 1) {
        errno = EINVAL;
        return -1;
    }

    if (append_arg(args, args_size, &argc, "git") != 0 ||
        append_arg(args, args_size, &argc, "-C") != 0 ||
        append_arg(args, args_size, &argc, repo_root) != 0 ||
        append_arg(args, args_size, &argc, "diff") != 0) {
        return -1;
    }
    if (mode == FILE_SELECTION_GIT_STAGED &&
        append_arg(args, args_size, &argc, "--cached") != 0) {
        return -1;
    }
    if (append_arg(args, args_size, &argc, "-U0") != 0 ||
        append_arg(args, args_size, &argc, "--no-color") != 0 ||
        append_arg(args, args_size, &argc, "--no-ext-diff") != 0) {
        return -1;
    }
    if (mode == FILE_SELECTION_GIT_DIFF &&
        append_arg(args, args_size, &argc, diff_range) != 0) {
        return -1;
    }
    if (append_arg(args, args_size, &argc, "--") != 0) {
        return -1;
    }
    if (path->change_type == SELECTED_PATH_CHANGE_RENAMED &&
        path->previous_repo_rel_path &&
        append_arg(args, args_size, &argc, path->previous_repo_rel_path) != 0) {
        return -1;
    }
    if (append_arg(args, args_size, &argc, repo_rel_path) != 0) {
        return -1;
    }

    args[argc] = NULL;
    return 0;
}

int collect_stdin_paths(int null_delim,
                        SelectedPath** paths_out,
                        size_t* count_out) {
    SelectedPath* paths = NULL;
    size_t count = 0;
    size_t capacity = 0;
    char* line = NULL;
    size_t line_cap = 0;
    int delim = null_delim ? '\0' : '\n';

    if (!paths_out || !count_out) {
        errno = EINVAL;
        return -1;
    }

    *paths_out = NULL;
    *count_out = 0;

    while (1) {
        ssize_t line_len = getdelim(&line, &line_cap, delim, stdin);
        if (line_len == -1) {
            if (feof(stdin)) {
                break;
            }
            free(line);
            free_selected_paths(paths, count);
            return -1;
        }

        size_t record_len = (size_t)line_len;
        if (record_len > 0 && line[record_len - 1] == delim) {
            record_len--;
        }
        if (!null_delim && record_len > 0 && line[record_len - 1] == '\r') {
            record_len--;
        }
        if (record_len == 0) {
            continue;
        }

        if (append_literal_selected_path(&paths, &count, &capacity, line, record_len) != 0) {
            free(line);
            free_selected_paths(paths, count);
            return -1;
        }
    }

    free(line);

    if (normalize_selected_paths(paths, &count) != 0) {
        free_selected_paths(paths, count);
        return -1;
    }

    *paths_out = paths;
    *count_out = count;
    return 0;
}

int collect_git_paths(FileSelectionMode mode,
                      const char* diff_range,
                      int quiet_probe,
                      SelectedPath** paths_out,
                      size_t* count_out,
                      GitPathResult* result_out) {
    GitRepoPaths repo = {0};
    const char* args[GIT_SELECTION_ARGS_MAX];
    unsigned char* output = NULL;
    size_t output_len = 0;
    int exit_status = 0;
    int exec_errno = 0;
    SelectedPath* paths = NULL;
    size_t parsed_count = 0;
    GitProbeResult probe_result = GIT_PROBE_READY;
    int status = -1;

    *paths_out = NULL;
    *count_out = 0;
    if (result_out) {
        *result_out = GIT_PATHS_FALLBACK;
    }

    errno = 0;
    if (load_git_repo_paths(quiet_probe, &repo, &probe_result) != 0) {
        if (quiet_probe && probe_result == GIT_PROBE_FALLBACK) {
            status = 0;
            goto cleanup;
        }
        if (errno == ENOENT) {
            fprintf(stderr, "Git file-selection modes require git to be installed\n");
        } else {
            fprintf(stderr, "Git file-selection modes require a Git repository\n");
        }
        goto cleanup;
    }
    if (build_git_selection_args(&repo, mode, diff_range, args, GIT_SELECTION_ARGS_MAX) != 0) {
        goto cleanup;
    }

    if (run_command_capture(args, 0, &output, &output_len, &exit_status, &exec_errno) != 0) {
        perror("Error running git");
        goto cleanup;
    }
    if (exec_errno != 0) {
        errno = exec_errno;
        perror("Error executing git");
        goto cleanup;
    }
    if (!WIFEXITED(exit_status) || WEXITSTATUS(exit_status) != 0) {
        fprintf(stderr, "git %s failed for the requested file-selection mode\n",
                (mode == FILE_SELECTION_GIT_WORKTREE) ? "ls-files" : "diff");
        goto cleanup;
    }

    if (parse_git_selected_paths(mode, output, output_len, &repo, &paths, &parsed_count) != 0) {
        goto cleanup;
    }

    *paths_out = paths;
    *count_out = parsed_count;
    paths = NULL;
    if (result_out) {
        *result_out = GIT_PATHS_COLLECTED;
    }
    status = 0;

cleanup:
    free(output);
    free_git_repo_paths(&repo);
    free_selected_paths(paths, parsed_count);
    return status;
}

int collect_git_hunks(FileSelectionMode mode,
                      const char* diff_range,
                      const SelectedPath* paths,
                      size_t path_count,
                      GitFileHunks** hunks_out,
                      size_t* hunk_count_out) {
    char* repo_root = NULL;
    GitFileHunks* hunks = NULL;
    int status = -1;

    if (!hunks_out || !hunk_count_out || (path_count > 0 && !paths)) {
        errno = EINVAL;
        return -1;
    }

    *hunks_out = NULL;
    *hunk_count_out = 0;

    if (mode != FILE_SELECTION_GIT_STAGED &&
        mode != FILE_SELECTION_GIT_UNSTAGED &&
        mode != FILE_SELECTION_GIT_DIFF) {
        errno = EINVAL;
        return -1;
    }

    if (path_count == 0) {
        return 0;
    }

    hunks = calloc(path_count, sizeof(*hunks));
    if (!hunks) {
        return -1;
    }

    if (derive_repo_root_from_selected_paths(paths, path_count, &repo_root) != 0) {
        goto cleanup;
    }

    for (size_t i = 0; i < path_count; i++) {
        const char* args[GIT_HUNK_ARGS_MAX];
        unsigned char* output = NULL;
        size_t output_len = 0;
        int exit_status = 0;
        int exec_errno = 0;

        if (paths[i].change_type == SELECTED_PATH_CHANGE_ADDED) {
            continue;
        }
        if (!paths[i].repo_rel_path) {
            errno = EINVAL;
            goto cleanup;
        }
        if (build_git_hunk_args(repo_root,
                                mode,
                                diff_range,
                                &paths[i],
                                paths[i].repo_rel_path,
                                args,
                                GIT_HUNK_ARGS_MAX) != 0) {
            goto cleanup;
        }
        if (run_command_capture(args, 0, &output, &output_len, &exit_status, &exec_errno) != 0) {
            perror("Error running git");
            free(output);
            goto cleanup;
        }
        if (exec_errno != 0) {
            errno = exec_errno;
            perror("Error executing git");
            free(output);
            goto cleanup;
        }
        if (!WIFEXITED(exit_status) || WEXITSTATUS(exit_status) != 0) {
            fprintf(stderr, "git diff failed while collecting hunks for %s\n", paths[i].display_path);
            free(output);
            goto cleanup;
        }
        if (parse_hunk_ranges_from_diff_output(output, output_len, &hunks[i]) != 0) {
            free(output);
            goto cleanup;
        }
        free(output);
    }

    *hunks_out = hunks;
    *hunk_count_out = path_count;
    hunks = NULL;
    status = 0;

cleanup:
    free(repo_root);
    free_git_hunks(hunks, path_count);
    return status;
}

int resolve_repository_name(FileSelectionMode mode, char* buffer, size_t buffer_size) {
    char cwd[MAX_PATH_LENGTH];
    char* repo_root = NULL;
    const char* source = NULL;

    if (!buffer || buffer_size == 0) {
        errno = EINVAL;
        return -1;
    }

    (void)mode;

    if (capture_git_repo_root(1, &repo_root, NULL) == 0) {
        source = repo_root;
    } else {
        if (!getcwd(cwd, sizeof(cwd))) {
            free(repo_root);
            return -1;
        }
        source = cwd;
    }

    if (copy_path_basename(source, buffer, buffer_size) != 0) {
        free(repo_root);
        return -1;
    }

    free(repo_root);
    return 0;
}

void free_git_hunks(GitFileHunks* hunks, size_t count) {
    if (!hunks) {
        return;
    }

    for (size_t i = 0; i < count; i++) {
        free(hunks[i].ranges);
    }
    free(hunks);
}
