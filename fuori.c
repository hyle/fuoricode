#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
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

// Function prototypes
int is_binary_file(const unsigned char* buffer, size_t bytes_read);
int should_exclude_file(const char* filepath,
                        const struct stat* st,
                        size_t max_file_size,
                        char** patterns,
                        size_t count);
void sanitize_path(char* path);
int process_directory(const char* base_path,
                      FILE* output_file,
                      AppContext* ctx);
// Return codes: 0 = exported, 1 = skipped (binary/empty), -1 = error.
int process_file(const char* filepath,
                 const struct stat* st,
                 size_t max_file_size,
                 FILE* output_file);
const char* get_language_identifier(const char* filepath, const unsigned char* buffer, size_t buffer_len);
const char* detect_shebang(const unsigned char* buffer, size_t buffer_len);
int write_fence(FILE* out, size_t count, const char* lang);
int write_text(FILE* out, const char* text);
int write_bytes(FILE* out, const void* data, size_t len);
int compare_names(const void* lhs, const void* rhs);
void free_names(char** names, size_t count);
int is_likely_utf8(const unsigned char* s, size_t n);
int make_temp_output_template(const char* output_path, char* tmpl, size_t tmpl_size);

int main(int argc, char* argv[]) {
    AppContext ctx = {0};
    ctx.max_file_size = MAX_FILE_SIZE;  // Default value
    ctx.output_path = DEFAULT_OUTPUT_FILE;

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
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [OPTIONS]\n", argv[0]);
            printf("Export codebase to markdown file.\n\n");
            printf("Options:\n");
            printf("  -v, --verbose    Show progress information\n");
            printf("  -s <size_kb>     Set maximum file size limit in KB (default: 100)\n");
            printf("  -o, --output     Set output path (use '-' for stdout)\n");
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

    int status = 1;
    int temp_created = 0;
    int output_needs_close = 0;
    FILE* output_file = NULL;
    char temp_output_path[MAX_PATH_LENGTH];
    temp_output_path[0] = '\0';

    if (ctx.output_is_stdout) {
        output_file = stdout;
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
    if (write_text(output_file, "# Codebase Export\n\n") != 0 ||
        write_text(output_file, "This document contains all the source code files from the codebase.\n\n") != 0) {
        perror("Error writing output header");
        goto cleanup;
    }

    // Process current directory
    if (process_directory(".", output_file, &ctx) != 0) {
        fprintf(stderr, "Error processing directory\n");
        goto cleanup;
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
    free_ignore_patterns(ctx.ignore_patterns, ctx.ignore_count);
    return status;
}

int process_directory(const char* base_path,
                      FILE* output_file,
                      AppContext* ctx) {
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

    qsort(names, name_count, sizeof(char*), compare_names);

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
            // Apply ignore file to directories before recursion
            // Pass 1 for is_dir
            if (is_ignored(path, ctx->ignore_patterns, ctx->ignore_count, 1)) {
                if (ctx->verbose) {
                    fprintf(stderr, "Skipping ignored directory: %s\n", path);
                }
                continue;
            }
            // Recursively process subdirectory
            if (process_directory(path, output_file, ctx) != 0) {
                status = -1;
                goto cleanup;
            }
        } else if (S_ISREG(st.st_mode)) {
            // Process regular file
            if (!should_exclude_file(path, &st, ctx->max_file_size, ctx->ignore_patterns, ctx->ignore_count)) {
                if (ctx->verbose) {
                    fprintf(stderr, "Processing file: %s\n", path);
                }
                int result = process_file(path, &st, ctx->max_file_size, output_file);
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
    const char* display = (strncmp(filepath, "./", 2) == 0) ? filepath + 2 : filepath;
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

    size_t sample_size = (bytes_read < 8192) ? bytes_read : 8192;
    if (is_binary_file(buffer, sample_size)) {
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
        write_text(output_file, display) != 0 ||
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

int should_exclude_file(const char* filepath,
                        const struct stat* st,
                        size_t max_file_size,
                        char** patterns,
                        size_t count) {
    // Skip non-regular files
    if (!S_ISREG(st->st_mode)) {
        return 1;
    }

    // Check file size
    if ((size_t)st->st_size > max_file_size) {
        return 1; // Exclude if too large
    }

    // Check if ignored by the ignore file
    // Pass 0 for is_dir
    if (is_ignored(filepath, patterns, count, 0)) {
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

    // If UTF-8, consider it text
    if (is_likely_utf8(buffer, bytes_read)) {
        return 0;
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

int compare_names(const void* lhs, const void* rhs) {
    const char* const* left = lhs;
    const char* const* right = rhs;
    return strcmp(*left, *right);
}

void free_names(char** names, size_t count) {
    if (!names) return;
    for (size_t i = 0; i < count; i++) {
        free(names[i]);
    }
    free(names);
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
    const char* dot = strrchr(filepath, '.');
    if (!dot || dot == filepath) {
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
        size_t need = 0;
        if ((c & 0xE0) == 0xC0) need = 1;
        else if ((c & 0xF0) == 0xE0) need = 2;
        else if ((c & 0xF8) == 0xF0) need = 3;
        else return 0;
        // If the sample ends in a partial multibyte sequence, treat it as likely text.
        if (i + need >= n) return 1;
        for (size_t k = 1; k <= need; k++) {
            if ((s[i+k] & 0xC0) != 0x80) return 0;
        }
        i += need + 1;
    }
    return 1;
}

const char* detect_shebang(const unsigned char* buffer, size_t buffer_len) {
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

    // Look for common interpreters
    if (strstr(line, "python")) return "python";
    if (strstr(line, "bash") || strstr(line, "sh")) return "bash";
    if (strstr(line, "node")) return "javascript";
    if (strstr(line, "ruby")) return "ruby";
    if (strstr(line, "perl")) return "perl";
    if (strstr(line, "lua")) return "lua";

    return NULL;
}
