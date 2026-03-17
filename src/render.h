#ifndef RENDER_H
#define RENDER_H

#include <stdio.h>

#include "app.h"
#include "collect.h"
#include "git_paths.h"

typedef struct {
    size_t files_exported;
    size_t bytes_written;
    size_t estimated_tokens;
} ExportMetrics;

typedef enum {
    RENDER_ENTRY_FULL = 0,
    RENDER_ENTRY_SLICED,
    RENDER_ENTRY_OMIT
} RenderEntryMode;

typedef struct {
    size_t start_line;
    size_t end_line;
} RenderLineRange;

typedef struct {
    RenderEntryMode mode;
    size_t fence_length;
    size_t total_lines;
    RenderLineRange* ranges;
    size_t range_count;
} RenderEntryInfo;

typedef struct {
    RenderEntryInfo* entries;
    unsigned char* include_mask;
    size_t count;
    size_t visible_count;
} RenderPlanInfo;

typedef struct {
    FileSelectionMode mode;
    const char* repository;
    const char* generated_at;
    const SelectedPath* selected_paths;
    size_t selected_count;
    const char* diff_range;
    int show_line_numbers;
    int show_hunks;
    int show_tree;
    size_t hunk_context_lines;
    size_t tree_depth;
} ExportRenderContext;

int prepare_render_plan(const ExportPlan* plan,
                        const ExportRenderContext* ctx,
                        RenderPlanInfo* info);
void free_render_plan_info(RenderPlanInfo* info);
int calculate_export_metrics(const ExportPlan* plan,
                             const RenderPlanInfo* info,
                             const ExportRenderContext* ctx,
                             ExportMetrics* metrics);
int write_export_header(FILE* out, const ExportRenderContext* ctx);
int write_change_context(FILE* out, const ExportRenderContext* ctx);
int render_export_plan(FILE* out,
                       const ExportPlan* plan,
                       const RenderPlanInfo* info,
                       const ExportRenderContext* ctx,
                       int verbose);

#endif
