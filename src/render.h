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

typedef struct {
    size_t* fence_lengths;
    size_t count;
} RenderPlanInfo;

typedef struct {
    FileSelectionMode mode;
    const char* repository;
    const char* generated_at;
    const SelectedPath* selected_paths;
    size_t selected_count;
    const char* diff_range;
    int show_line_numbers;
    int show_tree;
    size_t tree_depth;
} ExportRenderContext;

int prepare_render_plan(const ExportPlan* plan, RenderPlanInfo* info);
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
