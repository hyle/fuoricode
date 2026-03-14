#ifndef RENDER_H
#define RENDER_H

#include <stdio.h>

#include "app.h"
#include "collect.h"

int write_export_header(FILE* out, FileSelectionMode mode);
int write_project_tree(FILE* out, const ExportPlan* plan, size_t max_depth);
int render_export_plan(FILE* out, const ExportPlan* plan, int verbose);

#endif
