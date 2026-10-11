#ifndef AXON_OPERATION_INTERNAL
#   define AXON_OPERATION_INTERNAL

#include "operation.h"
#include "alpm_extra.h"

struct axon_operation {
    axon_alpm_t *core;
    axon_operation_plan_t plan;
    alpm_list_t *hold_packages;
    alpm_list_t *explicit_names;
    int allow_downgrade;
    int allow_replacements;
    int question;
    int ignored;
    int prepared;
    alpm_errno_t plan_error;
};

axon_operation_status_t axon_operation_build_plan(axon_operation_t *operation);
void axon_operation_clear_plan(axon_operation_t *operation);
int axon_operation_held(const axon_operation_t *operation, const char *name);

#endif
