#include "collect.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "ignore.h"

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
    {NULL, NULL}
};

static int compare_names(const void* lhs, const void* rhs) {
    const char* const* left = lhs;
    const char* const* right = rhs;
    return strcmp(*left, *right);
}

static int compare_export_entries(const void* lhs, const void* rhs) {
    const ExportEntry* left = lhs;
    const ExportEntry* right = rhs;
    int display_cmp = strcmp(left->display_path, right->display_path);
    if (display_cmp != 0) {
        return display_cmp;
    }
    return strcmp(left->open_path, right->open_path);
}

static void free_names(char** names, size_t count) {
    if (!names) return;
    for (size_t i = 0; i < count; i++) {
        free(names[i]);
    }
    free(names);
}

void free_export_plan(ExportPlan* plan) {
    if (!plan || !plan->entries) return;
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

static void sanitize_path(char* path) {
    char* src = path;
    char* dst = path;

    while (*src) {
        *dst = *src;
        if (*src == '/') {
            while (*(src + 1) == '/') {
                src++;
            }
        }
        src++;
        dst++;
    }
    *dst = '\0';

    size_t len = strlen(path);
    if (len > 1 && path[len - 1] == '/') {
        path[len - 1] = '\0';
    }
}

static int is_likely_utf8(const unsigned char* s, size_t n) {
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

static int is_binary_file(const unsigned char* buffer, size_t bytes_read) {
    if (bytes_read == 0) return 0;

    for (size_t i = 0; i < bytes_read; i++) {
        if (buffer[i] == '\0') {
            return 1;
        }
    }

    if (!is_likely_utf8(buffer, bytes_read)) {
        return 1;
    }

    size_t ctrl = 0;
    for (size_t i = 0; i < bytes_read; i++) {
        unsigned char c = buffer[i];
        if (c < 0x20 && c != '\n' && c != '\r' && c != '\t') {
            ctrl++;
        }
    }
    return (ctrl * 100 / bytes_read) > 2;
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

static const char* detect_shebang(const unsigned char* buffer, size_t buffer_len) {
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

static const char* get_language_identifier(const char* filepath,
                                           const unsigned char* buffer,
                                           size_t buffer_len) {
    const char* base = filepath;
    const char* slash = strrchr(filepath, '/');
    if (slash) base = slash + 1;

    if (strcasecmp(base, "Dockerfile") == 0) return "dockerfile";
    if (strcasecmp(base, "Makefile") == 0 || strcasecmp(base, "GNUmakefile") == 0) return "makefile";

    const char* dot = strrchr(base, '.');
    if (!dot || dot == base) {
        return detect_shebang(buffer, buffer_len);
    }

    dot++;
    for (int i = 0; lang_map[i].extension != NULL; i++) {
        if (strcasecmp(dot, lang_map[i].extension) == 0) {
            return lang_map[i].language;
        }
    }

    return NULL;
}

static int should_exclude_file(const char* filepath,
                               const struct stat* st,
                               size_t max_file_size,
                               int ancestor_ignored,
                               char** patterns,
                               size_t count) {
    if (!S_ISREG(st->st_mode)) {
        return 1;
    }
    if (st->st_size < 0) {
        return 1;
    }
    if ((size_t)st->st_size > max_file_size) {
        return 1;
    }
    if (resolve_ignore_state(filepath, patterns, count, 0, ancestor_ignored)) {
        return 1;
    }
    return 0;
}

static int read_file_buffer(const char* filepath,
                            const struct stat* st,
                            size_t max_file_size,
                            unsigned char** buffer_out,
                            size_t* bytes_read_out) {
    int fd = -1;
    FILE* file = NULL;
    unsigned char* buffer = NULL;
    size_t buffer_size = 0;
    size_t buffer_capacity = 0;
    size_t extra_read = 0;
    size_t bytes_read = 0;
    unsigned char extra_chunk[4096];
    struct stat opened_st;

    *buffer_out = NULL;
    *bytes_read_out = 0;

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
        perror("Error opening file");
        return -1;
    }
    if (fstat(fd, &opened_st) == -1) {
        close(fd);
        perror("Error stating opened file");
        return -1;
    }
    if (!S_ISREG(opened_st.st_mode)) {
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

    *buffer_out = buffer;
    *bytes_read_out = bytes_read;
    return 0;
}

static int append_export_entry(ExportPlan* plan,
                               const char* open_path,
                               const char* display_path,
                               const struct stat* st,
                               unsigned char* buffer,
                               size_t buf_len,
                               const char* lang) {
    if (plan->count == plan->capacity) {
        size_t new_capacity = (plan->capacity == 0) ? 32 : plan->capacity * 2;
        ExportEntry* new_entries = realloc(plan->entries, new_capacity * sizeof(*new_entries));
        if (!new_entries) {
            perror("Error growing export plan");
            return -1;
        }
        plan->entries = new_entries;
        plan->capacity = new_capacity;
    }

    const char* normalized_display = display_path;
    if (normalized_display && strncmp(normalized_display, "./", 2) == 0) {
        normalized_display += 2;
    }

    ExportEntry* entry = &plan->entries[plan->count];
    entry->open_path = strdup(open_path);
    if (!entry->open_path) {
        perror("Error duplicating export path");
        return -1;
    }
    entry->display_path = strdup(normalized_display ? normalized_display : open_path);
    if (!entry->display_path) {
        perror("Error duplicating display path");
        free(entry->open_path);
        entry->open_path = NULL;
        return -1;
    }
    entry->st = *st;
    entry->buf = buffer;
    entry->buf_len = buf_len;
    entry->lang = lang;
    plan->count++;
    return 0;
}

static int collect_exportable_file(const char* open_path,
                                   const char* display_path,
                                   const struct stat* st,
                                   AppContext* ctx,
                                   int ancestor_ignored,
                                   int respect_ignore,
                                   ExportPlan* plan) {
    unsigned char* buffer = NULL;
    size_t bytes_read = 0;

    if (!S_ISREG(st->st_mode)) {
        return 0;
    }
    if ((ctx->have_temp &&
         st->st_dev == ctx->temp_stat.st_dev && st->st_ino == ctx->temp_stat.st_ino) ||
        (ctx->have_final &&
         st->st_dev == ctx->final_stat.st_dev && st->st_ino == ctx->final_stat.st_ino)) {
        return 0;
    }

    if (respect_ignore) {
        if (should_exclude_file(open_path,
                                st,
                                ctx->max_file_size,
                                ancestor_ignored,
                                ctx->ignore_patterns,
                                ctx->ignore_count)) {
            if (ctx->verbose) {
                fprintf(stderr, "Skipping file: %s\n", display_path);
            }
            return 0;
        }
    } else if (st->st_size < 0 || (size_t)st->st_size > ctx->max_file_size) {
        return 0;
    }

    /* Cache accepted file contents in memory to avoid re-reading at render time. */
    int read_result = read_file_buffer(open_path, st, ctx->max_file_size, &buffer, &bytes_read);
    if (read_result == 1) {
        return 0;
    }
    if (read_result != 0) {
        fprintf(stderr, "Warning: Failed to process file %s\n", display_path);
        return 0;
    }
    if (bytes_read == 0 || is_binary_file(buffer, bytes_read)) {
        if (ctx->verbose) {
            fprintf(stderr, "Skipping binary/empty file: %s\n", display_path);
        }
        free(buffer);
        return 0;
    }

    const char* lang = get_language_identifier(open_path, buffer, bytes_read);
    if (ctx->verbose) {
        fprintf(stderr, "Queued file: %s\n", display_path);
    }
    if (append_export_entry(plan, open_path, display_path, st, buffer, bytes_read, lang) != 0) {
        free(buffer);
        return -1;
    }
    return 0;
}

static int collect_recursive_paths(const char* base_path,
                                   AppContext* ctx,
                                   int ancestor_ignored,
                                   ExportPlan* plan) {
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
        if (S_ISLNK(st.st_mode)) {
            continue;
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
            if (collect_recursive_paths(path, ctx, dir_is_ignored, plan) != 0) {
                status = -1;
                goto cleanup;
            }
        } else if (S_ISREG(st.st_mode)) {
            if (collect_exportable_file(path, path, &st, ctx, ancestor_ignored, 1, plan) != 0) {
                status = -1;
                goto cleanup;
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

int collect_recursive_export_plan(AppContext* ctx, ExportPlan* plan) {
    if (collect_recursive_paths(".", ctx, 0, plan) != 0) {
        return -1;
    }
    if (plan->count > 1) {
        qsort(plan->entries, plan->count, sizeof(*plan->entries), compare_export_entries);
    }
    return 0;
}

int collect_selected_export_plan(const SelectedPath* selected_paths,
                                 size_t selected_count,
                                 AppContext* ctx,
                                 ExportPlan* plan) {
    for (size_t i = 0; i < selected_count; i++) {
        const SelectedPath* path = &selected_paths[i];
        struct stat st;

        if (lstat(path->open_path, &st) == -1) {
            if (errno == ENOENT) {
                continue;
            }
            perror("Error getting selected file status");
            continue;
        }
        if (!S_ISREG(st.st_mode)) {
            continue;
        }

        /* Symlinks are skipped inside collect_exportable_file via the S_ISREG guard above. */
        if (collect_exportable_file(path->open_path, path->display_path, &st, ctx, 0, 0, plan) != 0) {
            return -1;
        }
    }

    if (plan->count > 1) {
        qsort(plan->entries, plan->count, sizeof(*plan->entries), compare_export_entries);
    }
    return 0;
}
