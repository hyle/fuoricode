#ifndef TREE_H
#define TREE_H

#include <stddef.h>
#include <stdio.h>

#include "collect.h"

int write_project_tree(FILE* out, const ExportPlan* plan, size_t max_depth);
int count_project_tree_bytes(const ExportPlan* plan, size_t max_depth, size_t* total);

#endif
