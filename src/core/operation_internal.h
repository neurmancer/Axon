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
    int busy;
    int committing;
    int transaction_started;
    int execution_started;
    int cancelled;
    int interrupting;
    int interrupted;
    axon_operation_callbacks_t callbacks;
    axon_operation_result_t *cleanup_result;
    axon_operation_result_t *commit_result;
    alpm_errno_t plan_error;
};

int axon_operation_poll(axon_operation_t *operation);
alpm_errno_t axon_operation_set_callbacks(axon_operation_t *operation);
void axon_operation_question(void *context, alpm_question_t *question);
axon_operation_status_t axon_operation_build_plan(axon_operation_t *operation);
void axon_operation_clear_plan(axon_operation_t *operation);
int axon_operation_held(const axon_operation_t *operation, const char *name);

#endif
