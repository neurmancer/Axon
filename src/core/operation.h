#ifndef AXON_OPERATION
#   define AXON_OPERATION

#include "pacman_config.h"
#include "privilege.h"

#include <stdint.h>
#include <signal.h>

typedef struct axon_operation axon_operation_t;

typedef enum {
    AXON_OPERATION_INSTALL,
    AXON_OPERATION_FILE,
    AXON_OPERATION_REMOVE,
    AXON_OPERATION_UPGRADE
} axon_operation_kind_t;

typedef enum {
    AXON_OPERATION_OK,
    AXON_OPERATION_ARGUMENT,
    AXON_OPERATION_CONFIG,
    AXON_OPERATION_PRIVILEGE,
    AXON_OPERATION_BACKEND,
    AXON_OPERATION_MEMORY,
    AXON_OPERATION_POLICY,
    AXON_OPERATION_APPROVAL,
    AXON_OPERATION_STATE,
    AXON_OPERATION_QUESTION,
    AXON_OPERATION_CANCELLED,
    AXON_OPERATION_OVERFLOW,
    AXON_OPERATION_EXECUTION
} axon_operation_status_t;

typedef struct {
    const char *name;
    const char *repository;
} axon_operation_target_t;

typedef enum {
    AXON_DECISION_UNANSWERED,
    AXON_DECISION_ACCEPT,
    AXON_DECISION_DECLINE,
    AXON_DECISION_CANCEL
} axon_decision_t;

typedef struct {
    alpm_cb_log log;
    alpm_cb_event event;
    alpm_cb_progress progress;
    alpm_cb_download download;
    axon_decision_t (*question)(void *context, const alpm_question_t *question,
                               size_t *provider_index);
    void *context;
    const volatile sig_atomic_t *cancel_requested;
} axon_operation_callbacks_t;

typedef struct {
    axon_operation_kind_t kind;
    const axon_operation_target_t *targets;
    size_t target_count;
    int flags;
    int allow_downgrade;
    int allow_replacements;
    axon_operation_callbacks_t callbacks;
} axon_operation_request_t;

typedef enum {
    AXON_CHANGE_INSTALL,
    AXON_CHANGE_REMOVE,
    AXON_CHANGE_UPGRADE,
    AXON_CHANGE_DOWNGRADE,
    AXON_CHANGE_REINSTALL,
    AXON_CHANGE_REPLACE
} axon_change_kind_t;

typedef struct {
    axon_change_kind_t kind;
    const char *name;
    const char *old_version;
    const char *new_version;
    const char *repository;
    const char *replaced_by;
    alpm_pkgreason_t reason;
    uint64_t download_bytes;
    uint64_t old_bytes;
    uint64_t new_bytes;
    int explicit_target;
    int held;
} axon_plan_entry_t;

typedef struct {
    uint64_t id;
    const axon_plan_entry_t *entries;
    size_t count;
    uint64_t download_bytes;
    uint64_t installed_bytes;
    uint64_t removed_bytes;
    int64_t installed_delta;
    int flags;
    int has_downgrades;
    int has_protected_removals;
} axon_operation_plan_t;

typedef struct {
    uint64_t plan_id;
    int confirm;
    int approved_flags;
    int approve_downgrades;
    const char *const *protected_removals;
    size_t protected_removal_count;
} axon_operation_approval_t;

typedef struct {
    axon_operation_status_t status;
    alpm_errno_t backend_error;
    alpm_errno_t cleanup_error;
    axon_privilege_result_t privilege_error;
    axon_config_error_t config_error;
    alpm_list_t *diagnostics;
    int committed;
    int may_have_changes;
    int refreshed;
    int cancelled;
    size_t completed_packages;
    size_t execution_errors;
    int package_phase_completed;
    int diagnostics_incomplete;
} axon_operation_result_t;

axon_operation_status_t axon_operation_prepare(const axon_config_t *config,
    const axon_operation_request_t *request, axon_operation_t **operation,
    axon_operation_result_t *result);
axon_operation_status_t axon_operation_prepare_upgrade(const axon_config_t *config,
    int force, const axon_operation_request_t *request, axon_operation_t **operation,
    axon_operation_result_t *result);
const axon_operation_plan_t *axon_operation_plan(const axon_operation_t *operation);
axon_operation_status_t axon_operation_commit(axon_operation_t *operation,
    const axon_operation_approval_t *approval, axon_operation_result_t *result);
axon_operation_status_t axon_operation_cancel(axon_operation_t *operation,
    axon_operation_result_t *result);
axon_operation_status_t axon_operation_free(axon_operation_t **operation,
    axon_operation_result_t *result);
axon_operation_status_t axon_operation_refresh(const axon_config_t *config,
    int force, axon_operation_result_t *result);
axon_operation_status_t axon_operation_refresh_with_callbacks(const axon_config_t *config,
    int force, const axon_operation_callbacks_t *callbacks, axon_operation_result_t *result);
void axon_operation_result_clear(axon_operation_result_t *result);

#endif
