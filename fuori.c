#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <fnmatch.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <limits.h>
#include <stdint.h>
#include <strings.h>
#include <libgen.h>

#include "ignore.h"

#define MAX_FILE_SIZE (100 * 1024)  // 100KB limit
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#define MAX_PATH_LENGTH PATH_MAX
#define IGNORE_FILE ".gitignore"
#define DEFAULT_OUTPUT_FILE "_export.md"

// Application context structure to hold global state
typedef struct {
    int verbose;
    int no_clobber;
    int output_is_stdout;
    size_t max_file_size;
    const char* output_path;
    char** ignore_patterns;
    size_t ignore_count;
    struct stat temp_stat;
    struct stat final_stat;
    int have_temp;
    int have_final;
} AppContext;

typedef enum {
    FILE_SELECTION_RECURSIVE = 0,
    FILE_SELECTION_GIT_STAGED,
    FILE_SELECTION_GIT_UNSTAGED,
    FILE_SELECTION_GIT_DIFF
} FileSelectionMode;

typedef struct {
    char* open_path;
    char* display_path;
} SelectedPath;

// Function prototypes
int is_binary_file(const unsigned char* buffer, size_t bytes_read);
int should_exclude_file(const char* filepath,
                        const struct stat* st,
                        size_t max_file_size,
                        int ancestor_ignored,
                        char** patterns,
                        size_t count);
void sanitize_path(char* path);
int process_directory(const char* base_path,
                      FILE* output_file,
                      AppContext* ctx,
                      int ancestor_ignored);
// Return codes: 0 = exported, 1 = skipped (binary/empty), -1 = error.
int process_file(const char* filepath,
                 const char* display_path,
                 const struct stat* st,
                 size_t max_file_size,
                 FILE* output_file);
int process_selected_paths(const SelectedPath* paths,
                           size_t count,
                           FILE* output_file,
                           AppContext* ctx);
const char* get_language_identifier(const char* filepath, const unsigned char* buffer, size_t buffer_len);
const char* detect_shebang(const unsigned char* buffer, size_t buffer_len);
int write_fence(FILE* out, size_t count, const char* lang);
int write_text(FILE* out, const char* text);
int write_bytes(FILE* out, const void* data, size_t len);
int write_markdown_path(FILE* out, const char* path);
int write_export_header(FILE* out, FileSelectionMode mode);
int compare_names(const void* lhs, const void* rhs);
int compare_selected_paths(const void* lhs, const void* rhs);
void free_names(char** names, size_t count);
void free_selected_paths(SelectedPath* paths, size_t count);
int is_likely_utf8(const unsigned char* s, size_t n);
int make_temp_output_template(const char* output_path, char* tmpl, size_t tmpl_size);
int run_command_capture(const char* const argv[],
                        unsigned char** output,
                        size_t* output_len,
                        int* exit_status,
                        int* exec_errno);
int capture_git_line(const char* repo_root, const char* rev_parse_arg, char** line_out);
int collect_git_paths(FileSelectionMode mode,
                      const char* diff_range,
                      SelectedPath** paths_out,
                      size_t* count_out);

int main(int argc, char* argv[]) {
    AppContext ctx = {0};
    FileSelectionMode selection_mode = FILE_SELECTION_RECURSIVE;
    const char* diff_range = NULL;
    SelectedPath* selected_paths = NULL;
    size_t selected_count = 0;
    int status = 1;
    int temp_created = 0;
    int output_needs_close = 0;
    FILE* output_file = NULL;
    char temp_output_path[MAX_PATH_LENGTH];
    ctx.max_file_size = MAX_FILE_SIZE;  // Default value
    ctx.output_path = DEFAULT_OUTPUT_FILE;
    temp_output_path[0] = '\0';

    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            ctx.verbose = 1;
        } else if (strcmp(argv[i], "-s") == 0) {
            if (i + 1 < argc) {
                char* endptr;
                errno = 0;
                unsigned long long size_kb = strtoull(argv[++i], &endptr, 10);
                if (*endptr == '\0' && size_kb > 0 &&
                    errno != ERANGE && size_kb <= (SIZE_MAX / 1024ULL)) {
                    ctx.max_file_size = (size_t)size_kb * 1024;
                } else {
                    fprintf(stderr, "Invalid size value: %s\n", argv[i]);
                    fprintf(stderr, "Use -h or --help for usage information\n");
                    return 1;
                }
            } else {
                fprintf(stderr, "Missing size value for -s option\n");
                fprintf(stderr, "Use -h or --help for usage information\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) {
            if (i + 1 < argc) {
                ctx.output_path = argv[++i];
                if (ctx.output_path[0] == '\0') {
                    fprintf(stderr, "Invalid output path: empty string\n");
                    return 1;
                }
                ctx.output_is_stdout = (strcmp(ctx.output_path, "-") == 0);
            } else {
                fprintf(stderr, "Missing path value for -o/--output option\n");
                fprintf(stderr, "Use -h or --help for usage information\n");
                return 1;
            }
        } else if (strncmp(argv[i], "--output=", 9) == 0) {
            ctx.output_path = argv[i] + 9;
            if (ctx.output_path[0] == '\0') {
                fprintf(stderr, "Invalid output path: empty string\n");
                return 1;
            }
            ctx.output_is_stdout = (strcmp(ctx.output_path, "-") == 0);
        } else if (strcmp(argv[i], "--no-clobber") == 0) {
            ctx.no_clobber = 1;
        } else if (strcmp(argv[i], "--staged") == 0) {
            if (selection_mode != FILE_SELECTION_RECURSIVE) {
                fprintf(stderr, "File-selection flags are mutually exclusive\n");
                fprintf(stderr, "Use -h or --help for usage information\n");
                return 1;
            }
            selection_mode = FILE_SELECTION_GIT_STAGED;
        } else if (strcmp(argv[i], "--unstaged") == 0) {
            if (selection_mode != FILE_SELECTION_RECURSIVE) {
                fprintf(stderr, "File-selection flags are mutually exclusive\n");
                fprintf(stderr, "Use -h or --help for usage information\n");
                return 1;
            }
            selection_mode = FILE_SELECTION_GIT_UNSTAGED;
        } else if (strcmp(argv[i], "--diff") == 0) {
            if (selection_mode != FILE_SELECTION_RECURSIVE) {
                fprintf(stderr, "File-selection flags are mutually exclusive\n");
                fprintf(stderr, "Use -h or --help for usage information\n");
                return 1;
            }
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing range value for --diff option\n");
                fprintf(stderr, "Use -h or --help for usage information\n");
                return 1;
            }
            diff_range = argv[++i];
            if (diff_range[0] == '\0') {
                fprintf(stderr, "Invalid diff range: empty string\n");
                return 1;
            }
            selection_mode = FILE_SELECTION_GIT_DIFF;
        } else if (strncmp(argv[i], "--diff=", 7) == 0) {
            if (selection_mode != FILE_SELECTION_RECURSIVE) {
                fprintf(stderr, "File-selection flags are mutually exclusive\n");
                fprintf(stderr, "Use -h or --help for usage information\n");
                return 1;
            }
            diff_range = argv[i] + 7;
            if (diff_range[0] == '\0') {
                fprintf(stderr, "Invalid diff range: empty string\n");
                return 1;
            }
            selection_mode = FILE_SELECTION_GIT_DIFF;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [OPTIONS]\n", argv[0]);
            printf("Export codebase to markdown file.\n\n");
            printf("Options:\n");
            printf("  -v, --verbose    Show progress information\n");
            printf("  -s <size_kb>     Set maximum file size limit in KB (default: 100)\n");
            printf("  -o, --output     Set output path (use '-' for stdout)\n");
            printf("      --staged     Export staged files from the current Git subtree\n");
            printf("      --unstaged   Export unstaged tracked files from the current Git subtree\n");
            printf("      --diff <r>   Export files changed by a git diff range (for example main...HEAD)\n");
            printf("                   Git file-selection flags are mutually exclusive\n");
            printf("      --no-clobber Fail if output file already exists\n");
            printf("  -h, --help       Show this help message\n");
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            fprintf(stderr, "Use -h or --help for usage information\n");
            return 1;
        }
    }

    // Load ignore patterns into memory
    if (load_ignore_patterns(IGNORE_FILE, &ctx.ignore_patterns, &ctx.ignore_count) != 0) {
        fprintf(stderr, "Error: Failed to initialize ignore patterns.\n");
        return 1;
    }

    if (selection_mode != FILE_SELECTION_RECURSIVE) {
        if (collect_git_paths(selection_mode, diff_range, &selected_paths, &selected_count) != 0) {
            goto cleanup;
        }
    }

    if (ctx.output_is_stdout) {
        output_file = stdout;
        if (fstat(fileno(output_file), &ctx.final_stat) == 0 &&
            S_ISREG(ctx.final_stat.st_mode)) {
            // When stdout is redirected to a file, exclude that inode from traversal
            // to avoid exporting the output back into itself.
            ctx.have_final = 1;
        }
    } else {
        if (ctx.no_clobber) {
            errno = 0;
            if (stat(ctx.output_path, &ctx.final_stat) == 0) {
                fprintf(stderr, "fuori: output file already exists: %s\n", ctx.output_path);
                goto cleanup;
            }
            if (errno != ENOENT) {
                perror("Error checking output path");
                goto cleanup;
            }
        }

        if (make_temp_output_template(ctx.output_path, temp_output_path, sizeof(temp_output_path)) != 0) {
            perror("Error creating temporary output path");
            goto cleanup;
        }

        // Write to a securely-created temporary file first for atomic operation.
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

        // Get stats for both temp and (optionally) final files
        if (fstat(fileno(output_file), &ctx.temp_stat) == -1) {
            perror("fstat on temporary output file");
            goto cleanup;
        }
        ctx.have_temp = 1;
        ctx.have_final = (stat(ctx.output_path, &ctx.final_stat) == 0);
    }

    // Write markdown header
    if (write_export_header(output_file, selection_mode) != 0) {
        perror("Error writing output header");
        goto cleanup;
    }

    if (selection_mode == FILE_SELECTION_RECURSIVE) {
        if (process_directory(".", output_file, &ctx, 0) != 0) {
            fprintf(stderr, "Error processing directory\n");
            goto cleanup;
        }
    } else {
        if (process_selected_paths(selected_paths, selected_count, output_file, &ctx) != 0) {
            fprintf(stderr, "Error processing selected files\n");
            goto cleanup;
        }
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
        // Atomically move temp file to final destination
        if (rename(temp_output_path, ctx.output_path) == -1) {
            perror("Error moving temporary file to final destination");
            goto cleanup;
        }
        temp_created = 0;
        printf("Codebase exported to %s successfully!\n", ctx.output_path);
    } else {
        fprintf(stderr, "Codebase exported to stdout successfully!\n");
    }

    status = 0;

cleanup:
    if (output_needs_close && output_file) {
        fclose(output_file);
    }
    if (temp_created && temp_output_path[0] != '\0') {
        unlink(temp_output_path);
    }
    free_selected_paths(selected_paths, selected_count);
    free_ignore_patterns(ctx.ignore_patterns, ctx.ignore_count);
    return status;
}

int process_directory(const char* base_path,
                      FILE* output_file,
                      AppContext* ctx,
                      int ancestor_ignored) {
    DIR* dir = NULL;
    struct dirent* entry;
    char path[MAX_PATH_LENGTH];
    char** names = NULL;
    size_t name_count = 0;
    size_t name_capacity = 0;
    int status = 0;

    if (ctx->verbose) {
        fprintf(stderr, "Processing directory: %s\n", base_path);
    }

    dir = opendir(base_path);
    if (!dir) {
        perror("Error opening directory");
        return -1;
    }

    // Snapshot and sort directory entries for deterministic output ordering.
    while (1) {
        errno = 0;
        entry = readdir(dir);
        if (!entry) {
            if (errno != 0) {
                perror("Error reading directory entries");
                status = -1;
            }
            break;
        }

        // Skip current and parent directory entries
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        if (name_count == name_capacity) {
            size_t new_capacity = (name_capacity == 0) ? 32 : name_capacity * 2;
            char** new_names = realloc(names, new_capacity * sizeof(char*));
            if (!new_names) {
                perror("Error allocating directory entry list");
                status = -1;
                goto cleanup;
            }
            names = new_names;
            name_capacity = new_capacity;
        }

        names[name_count] = strdup(entry->d_name);
        if (!names[name_count]) {
            perror("Error duplicating directory entry name");
            status = -1;
            goto cleanup;
        }
        name_count++;
    }
    if (status != 0) {
        goto cleanup;
    }

    if (name_count > 1) {
        qsort(names, name_count, sizeof(char*), compare_names);
    }

    for (size_t i = 0; i < name_count; i++) {
        const char* name = names[i];

        // Construct full path
            int path_len = snprintf(path, sizeof(path), "%s/%s", base_path, name);
            if (path_len < 0 || (size_t)path_len >= sizeof(path)) {
                if (ctx->verbose) {
                    fprintf(stderr, "Skipping path that exceeds %zu bytes: %s/%s\n",
                            (size_t)MAX_PATH_LENGTH, base_path, name);
                }
                continue;
            }
        sanitize_path(path);

        struct stat st;
        if (lstat(path, &st) == -1) {
            perror("Error getting file status");
            continue;
        }

        // Skip symlinks to avoid cycles (standard git behavior often ignores symlinks or treats them as files,
        // avoiding them is safer for export)
        if (S_ISLNK(st.st_mode)) {
            continue;
        }

        // Skip temp output and final output files by device+inode
        if (S_ISREG(st.st_mode)) {
            if ((ctx->have_temp &&
                 st.st_dev == ctx->temp_stat.st_dev && st.st_ino == ctx->temp_stat.st_ino) ||
                (ctx->have_final &&
                 st.st_dev == ctx->final_stat.st_dev && st.st_ino == ctx->final_stat.st_ino)) {
                continue;
            }
        }

        if (S_ISDIR(st.st_mode)) {
            int dir_is_ignored = resolve_ignore_state(path,
                                                      ctx->ignore_patterns,
                                                      ctx->ignore_count,
                                                      1,
                                                      ancestor_ignored);
            if (dir_is_ignored) {
                if (ctx->verbose) {
                    fprintf(stderr, "Skipping ignored directory: %s\n", path);
                }
                continue;
            }
            // Recursively process subdirectory
            if (process_directory(path, output_file, ctx, dir_is_ignored) != 0) {
                status = -1;
                goto cleanup;
            }
        } else if (S_ISREG(st.st_mode)) {
            // Process regular file
            if (!should_exclude_file(path,
                                     &st,
                                     ctx->max_file_size,
                                     ancestor_ignored,
                                     ctx->ignore_patterns,
                                     ctx->ignore_count)) {
                if (ctx->verbose) {
                    fprintf(stderr, "Processing file: %s\n", path);
                }
                int result = process_file(path, path, &st, ctx->max_file_size, output_file);
                if (result == 1) {
                    if (ctx->verbose) {
                        fprintf(stderr, "Skipping binary/empty file: %s\n", path);
                    }
                } else if (result != 0) {
                    if (ferror(output_file)) {
                        perror("Error writing export output");
                        status = -1;
                        goto cleanup;
                    }
                    fprintf(stderr, "Warning: Failed to process file %s\n", path);
                }
            } else if (ctx->verbose) {
                fprintf(stderr, "Skipping file: %s\n", path);
            }
        }
    }

cleanup:
    if (dir && closedir(dir) != 0 && status == 0) {
        perror("Error closing directory");
        status = -1;
    }
    free_names(names, name_count);
    return status;
}

// Return codes: 0 = exported, 1 = skipped (binary/empty), -1 = error.
int process_file(const char* filepath,
                 const char* display_path,
                 const struct stat* st,
                 size_t max_file_size,
                 FILE* output_file) {
    size_t max_run = 0;
    size_t current_run = 0;
    size_t fence = 3;
    const char* lang = NULL;
    int fd = -1;
    FILE* file = NULL;
    unsigned char* buffer = NULL;
    size_t buffer_size = 0;
    size_t buffer_capacity = 0;
    size_t extra_read = 0;
    unsigned char extra_chunk[4096];
    struct stat opened_st;

    // Normalize heading by removing leading "./"
    const char* display = display_path;
    if (!display) {
        display = (strncmp(filepath, "./", 2) == 0) ? filepath + 2 : filepath;
    } else if (strncmp(display, "./", 2) == 0) {
        display += 2;
    }
    if (st->st_size < 0) {
        errno = EINVAL;
        perror("Invalid file size");
        return -1;
    }

    int open_flags = O_RDONLY;
#ifdef O_NOFOLLOW
    open_flags |= O_NOFOLLOW;
#endif
    fd = open(filepath, open_flags);
    if (fd == -1) {
        free(buffer);
        perror("Error opening file");
        return -1;
    }
    if (fstat(fd, &opened_st) == -1) {
        free(buffer);
        close(fd);
        perror("Error stating opened file");
        return -1;
    }
    if (!S_ISREG(opened_st.st_mode)) {
        free(buffer);
        close(fd);
        errno = EINVAL;
        perror("Opened path is not a regular file");
        return -1;
    }
    if (opened_st.st_dev != st->st_dev || opened_st.st_ino != st->st_ino) {
        close(fd);
        errno = EAGAIN;
        perror("File changed while being processed");
        return -1;
    }
    if (opened_st.st_size < 0) {
        close(fd);
        errno = EINVAL;
        perror("Invalid opened file size");
        return -1;
    }
    if ((size_t)opened_st.st_size > max_file_size) {
        close(fd);
        return 1;
    }

    file = fdopen(fd, "rb");
    if (!file) {
        close(fd);
        perror("Error converting file descriptor to stream");
        return -1;
    }
    fd = -1;

    buffer_size = (size_t)opened_st.st_size;
    buffer_capacity = (buffer_size > 0) ? buffer_size : sizeof(extra_chunk);
    buffer = malloc(buffer_capacity);
    if (!buffer) {
        fclose(file);
        perror("Error allocating memory");
        return -1;
    }

    size_t bytes_read = 0;
    if (buffer_size > 0) {
        bytes_read = fread(buffer, 1, buffer_size, file);
        if (bytes_read < buffer_size && ferror(file)) {
            free(buffer);
            fclose(file);
            perror("Error reading file");
            return -1;
        }
    }

    while ((extra_read = fread(extra_chunk, 1, sizeof(extra_chunk), file)) > 0) {
        if (bytes_read + extra_read < bytes_read) {
            free(buffer);
            fclose(file);
            errno = EOVERFLOW;
            perror("File too large");
            return -1;
        }
        size_t needed = bytes_read + extra_read;
        if (needed > max_file_size) {
            free(buffer);
            fclose(file);
            return 1;
        }
        if (needed > buffer_capacity) {
            size_t new_capacity = buffer_capacity;
            while (new_capacity < needed) {
                if (new_capacity > SIZE_MAX / 2) {
                    new_capacity = needed;
                    break;
                }
                new_capacity *= 2;
            }

            unsigned char* new_buffer = realloc(buffer, new_capacity);
            if (!new_buffer) {
                free(buffer);
                fclose(file);
                perror("Error growing file buffer");
                return -1;
            }
            buffer = new_buffer;
            buffer_capacity = new_capacity;
        }
        memcpy(buffer + bytes_read, extra_chunk, extra_read);
        bytes_read += extra_read;
    }
    if (ferror(file)) {
        free(buffer);
        fclose(file);
        perror("Error reading file");
        return -1;
    }
    if (fclose(file) != 0) {
        free(buffer);
        perror("Error closing file");
        return -1;
    }

    if (is_binary_file(buffer, bytes_read)) {
        free(buffer);
        return 1;
    }
    if (bytes_read == 0) {
        free(buffer);
        return 1;
    }

    for (size_t i = 0; i < bytes_read; i++) {
        if (buffer[i] == '`') {
            current_run++;
            if (current_run > max_run) max_run = current_run;
        } else {
            current_run = 0;
        }
    }

    // Get language identifier for code block
    lang = get_language_identifier(filepath, buffer, bytes_read);
    fence = (max_run >= 3 ? max_run + 1 : 3);

    if (write_text(output_file, "## ") != 0 ||
        write_markdown_path(output_file, display) != 0 ||
        write_text(output_file, "\n\n") != 0) {
        free(buffer);
        perror("Error writing file heading");
        return -1;
    }

    // Write opening fence and the file contents.
    if (write_fence(output_file, fence, lang) != 0) {
        free(buffer);
        perror("Error writing opening code fence");
        return -1;
    }
    if (bytes_read > 0 && write_bytes(output_file, buffer, bytes_read) != 0) {
        free(buffer);
        perror("Error writing file contents to output");
        return -1;
    }
    if (bytes_read > 0 && buffer[bytes_read - 1] != '\n' && write_text(output_file, "\n") != 0) {
        free(buffer);
        perror("Error writing trailing newline");
        return -1;
    }
    free(buffer);

    // Write closing fence
    if (write_fence(output_file, fence, NULL) != 0 ||
        write_text(output_file, "\n\n") != 0) {
        perror("Error writing closing code fence");
        return -1;
    }
    return 0;
}

int process_selected_paths(const SelectedPath* paths,
                           size_t count,
                           FILE* output_file,
                           AppContext* ctx) {
    for (size_t i = 0; i < count; i++) {
        const SelectedPath* path = &paths[i];
        struct stat st;

        if (ctx->verbose) {
            fprintf(stderr, "Processing selected file: %s\n", path->display_path);
        }

        if (lstat(path->open_path, &st) == -1) {
            if (errno == ENOENT) {
                continue;
            }
            perror("Error getting selected file status");
            continue;
        }

        if (!S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
            continue;
        }

        if ((ctx->have_temp &&
             st.st_dev == ctx->temp_stat.st_dev && st.st_ino == ctx->temp_stat.st_ino) ||
            (ctx->have_final &&
             st.st_dev == ctx->final_stat.st_dev && st.st_ino == ctx->final_stat.st_ino)) {
            continue;
        }

        if (st.st_size < 0 || (size_t)st.st_size > ctx->max_file_size) {
            continue;
        }

        int result = process_file(path->open_path,
                                  path->display_path,
                                  &st,
                                  ctx->max_file_size,
                                  output_file);
        if (result == 1) {
            if (ctx->verbose) {
                fprintf(stderr, "Skipping binary/empty file: %s\n", path->display_path);
            }
        } else if (result != 0) {
            if (ferror(output_file)) {
                perror("Error writing export output");
                return -1;
            }
            fprintf(stderr, "Warning: Failed to process file %s\n", path->display_path);
        }
    }

    return 0;
}

int should_exclude_file(const char* filepath,
                        const struct stat* st,
                        size_t max_file_size,
                        int ancestor_ignored,
                        char** patterns,
                        size_t count) {
    // Skip non-regular files
    if (!S_ISREG(st->st_mode)) {
        return 1;
    }

    // Check file size
    if (st->st_size < 0) {
        return 1;
    }
    if ((size_t)st->st_size > max_file_size) {
        return 1; // Exclude if too large
    }

    // Check if ignored by the ignore file
    // Pass 0 for is_dir
    if (resolve_ignore_state(filepath, patterns, count, 0, ancestor_ignored)) {
        return 1;
    }

    return 0; // Don't exclude
}

int make_temp_output_template(const char* output_path, char* tmpl, size_t tmpl_size) {
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

int is_binary_file(const unsigned char* buffer, size_t bytes_read) {
    if (bytes_read == 0) return 0; // Empty file is not binary

    // Check for null bytes (common in binary files)
    for (size_t i = 0; i < bytes_read; i++) {
        if (buffer[i] == '\0') {
            return 1; // Binary file
        }
    }

    // Only export files whose bytes are valid UTF-8 to preserve UTF-8 output.
    if (!is_likely_utf8(buffer, bytes_read)) {
        return 1;
    }

    // Heuristic on control chars (excluding \t, \n, \r)
    size_t ctrl = 0;
    for (size_t i = 0; i < bytes_read; i++) {
        unsigned char c = buffer[i];
        if (c < 0x20 && c != '\n' && c != '\r' && c != '\t') {
            ctrl++;
        }
    }
    return (ctrl * 100 / bytes_read) > 2; // Tweak threshold as desired
}

void sanitize_path(char* path) {
    // Remove double slashes
    char* src = path;
    char* dst = path;

    while (*src) {
        *dst = *src;
        if (*src == '/') {
            // Skip multiple slashes
            while (*(src + 1) == '/') {
                src++;
            }
        }
        src++;
        dst++;
    }
    *dst = '\0';

    // Remove trailing slash if not root
    size_t len = strlen(path);
    if (len > 1 && path[len - 1] == '/') {
        path[len - 1] = '\0';
    }
}

int write_text(FILE* out, const char* text) {
    return (fputs(text, out) == EOF) ? -1 : 0;
}

int write_bytes(FILE* out, const void* data, size_t len) {
    return (fwrite(data, 1, len, out) == len) ? 0 : -1;
}

int write_markdown_path(FILE* out, const char* path) {
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

int compare_names(const void* lhs, const void* rhs) {
    const char* const* left = lhs;
    const char* const* right = rhs;
    return strcmp(*left, *right);
}

int compare_selected_paths(const void* lhs, const void* rhs) {
    const SelectedPath* left = lhs;
    const SelectedPath* right = rhs;
    int display_cmp = strcmp(left->display_path, right->display_path);
    if (display_cmp != 0) {
        return display_cmp;
    }
    return strcmp(left->open_path, right->open_path);
}

void free_names(char** names, size_t count) {
    if (!names) return;
    for (size_t i = 0; i < count; i++) {
        free(names[i]);
    }
    free(names);
}

void free_selected_paths(SelectedPath* paths, size_t count) {
    if (!paths) return;
    for (size_t i = 0; i < count; i++) {
        free(paths[i].open_path);
        free(paths[i].display_path);
    }
    free(paths);
}

int run_command_capture(const char* const argv[],
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

int capture_git_line(const char* repo_root, const char* rev_parse_arg, char** line_out) {
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
        args[argc++] = (char*)diff_range;
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

            size_t open_path_len = strlen(repo_root) + 1 + repo_rel_len;
            paths[count].open_path = malloc(open_path_len + 1);
            if (!paths[count].open_path) {
                goto cleanup;
            }
            memcpy(paths[count].open_path, repo_root, strlen(repo_root));
            paths[count].open_path[strlen(repo_root)] = '/';
            memcpy(paths[count].open_path + strlen(repo_root) + 1, repo_rel, repo_rel_len);
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

typedef struct {
    const char* const extension;
    const char* const language;
} LanguageMapping;

static const LanguageMapping lang_map[] = {
    {"c", "c"}, {"h", "c"},
    {"cpp", "cpp"}, {"cc", "cpp"}, {"cxx", "cpp"}, {"hpp", "cpp"},
    {"py", "python"},
    {"js", "javascript"}, {"jsx", "javascript"},
    {"ts", "typescript"}, {"tsx", "typescript"},
    {"html", "html"},
    {"css", "css"},
    {"java", "java"},
    {"php", "php"},
    {"sql", "sql"},
    {"xml", "xml"},
    {"json", "json"},
    {"md", "markdown"},
    {"sh", "bash"},
    {"yml", "yaml"}, {"yaml", "yaml"},
    {"go", "go"},
    {"rs", "rust"},
    {"kt", "kotlin"},
    {"cs", "csharp"},
    {"rb", "ruby"},
    {"lua", "lua"},
    {"toml", "toml"},
    {"ini", "ini"},
    {"dockerfile", "dockerfile"},
    {"makefile", "makefile"},
    {"cmake", "cmake"},
    {"swift", "swift"},
    {"m", "objective-c"}, {"mm", "objective-c"},
    {"ps1", "powershell"},
    {"bat", "batch"},
    {"r", "r"},
    {"scala", "scala"},
    {"proto", "protobuf"},
    {NULL, NULL} // Sentinel value to mark the end
};

const char* get_language_identifier(const char* filepath, const unsigned char* buffer, size_t buffer_len) {
    // Handle well-known dotless filenames first
    const char* base = filepath;
    const char* slash = strrchr(filepath, '/');
    if (slash) base = slash + 1;

    if (strcasecmp(base, "Dockerfile") == 0) return "dockerfile";
    if (strcasecmp(base, "Makefile") == 0 || strcasecmp(base, "GNUmakefile") == 0) return "makefile";

    // Extract file extension
    const char* dot = strrchr(base, '.');
    if (!dot || dot == base) {
        // No extension found, try shebang detection
        return detect_shebang(buffer, buffer_len);
    }

    dot++; // Skip the dot

    // Search through the language mapping table
    for (int i = 0; lang_map[i].extension != NULL; i++) {
        if (strcasecmp(dot, lang_map[i].extension) == 0) {
            return lang_map[i].language;
        }
    }

    return NULL;
}

int write_fence(FILE* out, size_t count, const char* lang) {
    for (size_t i = 0; i < count; i++) {
        if (fputc('`', out) == EOF) return -1;
    }
    if (lang && *lang && write_text(out, lang) != 0) return -1;
    return (fputc('\n', out) == EOF) ? -1 : 0;
}

int is_likely_utf8(const unsigned char* s, size_t n) {
    size_t i = 0;
    while (i < n) {
        unsigned char c = s[i];
        if (c < 0x80) {
            i++;
            continue;
        }
        if ((c & 0xE0) == 0xC0) {
            if (i + 1 >= n) return 0;
            if ((s[i + 1] & 0xC0) != 0x80) return 0;
            if (c < 0xC2) return 0;
            i += 2;
            continue;
        }
        if ((c & 0xF0) == 0xE0) {
            unsigned char b1;
            unsigned char b2;
            if (i + 2 >= n) return 0;
            b1 = s[i + 1];
            b2 = s[i + 2];
            if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80) return 0;
            if (c == 0xE0 && b1 < 0xA0) return 0;
            if (c == 0xED && b1 >= 0xA0) return 0;
            i += 3;
            continue;
        }
        if ((c & 0xF8) == 0xF0) {
            unsigned char b1;
            unsigned char b2;
            unsigned char b3;
            if (i + 3 >= n) return 0;
            b1 = s[i + 1];
            b2 = s[i + 2];
            b3 = s[i + 3];
            if ((b1 & 0xC0) != 0x80 ||
                (b2 & 0xC0) != 0x80 ||
                (b3 & 0xC0) != 0x80) {
                return 0;
            }
            if (c == 0xF0 && b1 < 0x90) return 0;
            if (c > 0xF4) return 0;
            if (c == 0xF4 && b1 >= 0x90) return 0;
            i += 4;
            continue;
        }
        return 0;
    }
    return 1;
}

static const char* classify_shebang_interpreter(const char* name) {
    if (!name || *name == '\0') return NULL;

    if (strcmp(name, "python") == 0 || strcmp(name, "python3") == 0) return "python";
    if (strcmp(name, "bash") == 0 || strcmp(name, "sh") == 0 || strcmp(name, "zsh") == 0) return "bash";
    if (strcmp(name, "node") == 0 || strcmp(name, "nodejs") == 0) return "javascript";
    if (strcmp(name, "ruby") == 0) return "ruby";
    if (strcmp(name, "perl") == 0) return "perl";
    if (strcmp(name, "lua") == 0) return "lua";
    if (strcmp(name, "pwsh") == 0 || strcmp(name, "powershell") == 0) return "powershell";

    return NULL;
}

const char* detect_shebang(const unsigned char* buffer, size_t buffer_len) {
    const char* interpreter = NULL;
    if (!buffer || buffer_len < 2) return NULL;
    if (buffer[0] != '#' || buffer[1] != '!') return NULL;

    size_t line_len = 0;
    while (line_len < buffer_len && line_len < 255 &&
           buffer[line_len] != '\n' && buffer[line_len] != '\r') {
        line_len++;
    }

    char line[256];
    memcpy(line, buffer, line_len);
    line[line_len] = '\0';

    char* p = line + 2;
    while (*p == ' ' || *p == '\t') {
        p++;
    }

    if (*p == '\0') {
        return NULL;
    }

    char* first = p;
    while (*p != '\0' && *p != ' ' && *p != '\t') {
        p++;
    }
    if (*p != '\0') {
        *p++ = '\0';
    }

    char* first_base = strrchr(first, '/');
    first_base = first_base ? first_base + 1 : first;

    if (strcmp(first_base, "env") == 0) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        while (*p == '-') {
            while (*p != '\0' && *p != ' ' && *p != '\t') {
                p++;
            }
            while (*p == ' ' || *p == '\t') {
                p++;
            }
        }
        if (*p == '\0') {
            return NULL;
        }

        char* second = p;
        while (*p != '\0' && *p != ' ' && *p != '\t') {
            p++;
        }
        *p = '\0';

        interpreter = second;
    } else {
        interpreter = first_base;
    }

    char* interp_base = strrchr(interpreter, '/');
    interp_base = interp_base ? interp_base + 1 : (char*)interpreter;
    return classify_shebang_interpreter(interp_base);
}
