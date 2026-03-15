#ifndef COLLECT_H
#define COLLECT_H

#include <stddef.h>
#include <sys/stat.h>

#include "app.h"
#include "git_paths.h"

typedef struct {
    char* open_path;
    char* display_path;
    struct stat st;
    unsigned char* buf;
    size_t buf_len;
    const char* lang;  // Points to a static literal; not heap-owned.
} ExportEntry;

typedef struct {
    ExportEntry* entries;
    size_t count;
    size_t capacity;
} ExportPlan;

int collect_recursive_export_plan(AppContext* ctx, ExportPlan* plan);
int collect_selected_export_plan(const SelectedPath* selected_paths,
                                 size_t selected_count,
                                 AppContext* ctx,
                                 ExportPlan* plan);
void free_export_plan(ExportPlan* plan);

#endif
