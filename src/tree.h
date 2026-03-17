#ifndef TREE_H
#define TREE_H

#include <stddef.h>
#include <stdio.h>

#include "collect.h"

int write_project_tree(FILE* out, const ExportPlan* plan, size_t max_depth);
int count_project_tree_bytes(const ExportPlan* plan, size_t max_depth, size_t* total);
int write_project_tree_filtered(FILE* out,
                                const ExportPlan* plan,
                                const unsigned char* include_mask,
                                size_t max_depth);
int count_project_tree_bytes_filtered(const ExportPlan* plan,
                                      const unsigned char* include_mask,
                                      size_t max_depth,
                                      size_t* total);

#endif
