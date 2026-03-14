#ifndef RENDER_H
#define RENDER_H

#include <stdio.h>

#include "app.h"
#include "collect.h"

typedef struct {
    size_t files_exported;
    size_t bytes_written;
    size_t estimated_tokens;
} ExportMetrics;

int calculate_export_metrics(const ExportPlan* plan,
                             FileSelectionMode mode,
                             int show_tree,
                             size_t tree_depth,
                             ExportMetrics* metrics);
int write_export_header(FILE* out, FileSelectionMode mode);
int write_project_tree(FILE* out, const ExportPlan* plan, size_t max_depth);
int render_export_plan(FILE* out, const ExportPlan* plan, int verbose);

#endif
