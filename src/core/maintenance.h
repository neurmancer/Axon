#ifndef AXON_MAINTENANCE
#define AXON_MAINTENANCE

#include "operation.h"
#include "alpm_extra.h"

typedef int (*axon_cache_approval_t)(void *context, const alpm_list_t *paths);
typedef void (*axon_maintenance_message_t)(void *context, const char *message);

int axon_package_orphan(alpm_pkg_t *package);
alpm_errno_t axon_maintenance_check(axon_alpm_t *core,
    axon_maintenance_message_t message, void *context);
alpm_errno_t axon_maintenance_records(axon_alpm_t *core, int repositories,
    axon_maintenance_message_t message, void *context);
alpm_errno_t axon_maintenance_files(axon_alpm_t *core, alpm_pkg_t *package,
    int metadata, int checksums, axon_maintenance_message_t message, void *context);
axon_operation_status_t axon_maintenance_reason(const axon_config_t *config,
    size_t count, const char *const *names, alpm_pkgreason_t reason,
    const volatile sig_atomic_t *cancel, axon_operation_result_t *result);
axon_operation_status_t axon_maintenance_clean(const axon_config_t *config,
    axon_cache_approval_t approve, void *context, const volatile sig_atomic_t *cancel,
    axon_operation_result_t *result);
#endif
