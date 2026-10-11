#define _POSIX_C_SOURCE 200809L

#include "operation_internal.h"

#include <fnmatch.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static atomic_uint_fast64_t next_id = 0;

int axon_operation_held(const axon_operation_t *operation, const char *name)
{
    for(const alpm_list_t *item = operation->hold_packages; item; item = item->next) {
        if(fnmatch(item->data, name, 0) == 0) {
            return(1);
        }
    }
    return(0);
}

void axon_operation_clear_plan(axon_operation_t *operation)
{
    for(size_t index = 0; index < operation->plan.count; index++) {
        const axon_plan_entry_t *entry = &operation->plan.entries[index];
        free((char *)entry->name);
        free((char *)entry->old_version);
        free((char *)entry->new_version);
        free((char *)entry->repository);
        free((char *)entry->replaced_by);
    }
    free((axon_plan_entry_t *)operation->plan.entries);
    memset(&operation->plan, 0, sizeof(operation->plan));
}

static int sum(uint64_t *total, uint64_t amount)
{
    if(amount > INT64_MAX || *total > (uint64_t)INT64_MAX - amount) {
        return(-1);
    }
    *total += amount;
    return(0);
}

static axon_operation_status_t entry_add(axon_operation_t *operation,
    axon_plan_entry_t *entry, alpm_pkg_t *package, int removal,
    const alpm_list_t *additions)
{
    const char *name = alpm_pkg_get_name(package);
    alpm_pkg_t *old = NULL;
    entry->name = strdup(name);
    entry->reason = alpm_pkg_get_reason(package);
    entry->explicit_target = alpm_list_find_str(operation->explicit_names, name) != NULL;
    if(entry->name == NULL) {
        return(AXON_OPERATION_MEMORY);
    }

    if(removal) {
        entry->kind = AXON_CHANGE_REMOVE;
        entry->held = axon_operation_held(operation, name);
        operation->plan.has_protected_removals |= entry->held;
        old = package;
        alpm_list_t single = {.data = package};
        for(const alpm_list_t *item = additions; item; item = item->next) {
            alpm_pkg_t *replacement = item->data;
            for(alpm_list_t *dep = alpm_pkg_get_replaces(replacement); dep; dep = dep->next) {
                char *expression = alpm_dep_compute_string(dep->data);
                if(expression == NULL) {
                    return(AXON_OPERATION_MEMORY);
                }
                int matches = alpm_find_satisfier(&single, expression) != NULL;
                free(expression);
                if(matches) {
                    entry->replaced_by = strdup(alpm_pkg_get_name(replacement));
                    if(entry->replaced_by == NULL) {
                        return(AXON_OPERATION_MEMORY);
                    }
                    entry->kind = AXON_CHANGE_REPLACE;
                    break;
                }
            }
            if(entry->replaced_by != NULL) {
                break;
            }
        }
    }

    else {
        alpm_errno_t error = axon_alpm_local_package(operation->core, name, &old);
        if(error != ALPM_ERR_OK && error != ALPM_ERR_PKG_NOT_FOUND) {
            operation->plan_error = error;
            return(AXON_OPERATION_BACKEND);
        }
        entry->kind = AXON_CHANGE_INSTALL;
        entry->new_version = strdup(alpm_pkg_get_version(package));
        if(entry->new_version == NULL) {
            return(AXON_OPERATION_MEMORY);
        }
        alpm_db_t *database = alpm_pkg_get_db(package);
        if(database != NULL) {
            entry->repository = strdup(alpm_db_get_name(database));
            if(entry->repository == NULL) {
                return(AXON_OPERATION_MEMORY);
            }
        }
        off_t download = alpm_pkg_download_size(package);
        off_t size = alpm_pkg_get_isize(package);
        if(download < 0 || size < 0) {
            return(AXON_OPERATION_OVERFLOW);
        }
        entry->download_bytes = (uint64_t)download;
        entry->new_bytes = (uint64_t)size;
        if(old != NULL) {
            int comparison = alpm_pkg_vercmp(entry->new_version, alpm_pkg_get_version(old));
            entry->kind = comparison > 0 ? AXON_CHANGE_UPGRADE
                        : comparison < 0 ? AXON_CHANGE_DOWNGRADE : AXON_CHANGE_REINSTALL;
            entry->reason = alpm_pkg_get_reason(old);
            operation->plan.has_downgrades |= comparison < 0;
        }
    }

    if(old != NULL) {
        entry->old_version = strdup(alpm_pkg_get_version(old));
        if(entry->old_version == NULL) {
            return(AXON_OPERATION_MEMORY);
        }
        off_t size = alpm_pkg_get_isize(old);
        if(size < 0) {
            return(AXON_OPERATION_OVERFLOW);
        }
        entry->old_bytes = (uint64_t)size;
    }
    if(sum(&operation->plan.download_bytes, entry->download_bytes) < 0
            || sum(&operation->plan.installed_bytes, entry->new_bytes) < 0) {
        return(AXON_OPERATION_OVERFLOW);
    }
    if(!removal || alpm_pkg_find((alpm_list_t *)additions, name) == NULL) {
        if(sum(&operation->plan.removed_bytes, entry->old_bytes) < 0) {
            return(AXON_OPERATION_OVERFLOW);
        }
    }
    return(AXON_OPERATION_OK);
}

axon_operation_status_t axon_operation_build_plan(axon_operation_t *operation)
{
    const alpm_list_t *additions = NULL;
    const alpm_list_t *removals = NULL;
    operation->plan_error = axon_alpm_transaction_plan(operation->core, &additions, &removals);
    if(operation->plan_error != ALPM_ERR_OK) {
        return(AXON_OPERATION_BACKEND);
    }
    size_t adds = alpm_list_count(additions);
    size_t removes = alpm_list_count(removals);
    if(adds > SIZE_MAX - removes || adds + removes > SIZE_MAX / sizeof(axon_plan_entry_t)) {
        return(AXON_OPERATION_OVERFLOW);
    }
    axon_plan_entry_t *entries = NULL;
    if(adds + removes > 0) {
        entries = calloc(adds + removes, sizeof(*entries));
        if(entries == NULL) {
            return(AXON_OPERATION_MEMORY);
        }
    }
    operation->plan.entries = entries;
    operation->plan.count = adds + removes;
    size_t index = 0;
    const alpm_list_t *lists[] = {additions, removals};
    for(size_t list = 0; list < 2; list++) {
        for(const alpm_list_t *item = lists[list]; item; item = item->next) {
            axon_operation_status_t status = entry_add(operation, &entries[index++], item->data,
                                                       list == 1, additions);
            if(status != AXON_OPERATION_OK) {
                return(status);
            }
        }
    }
    operation->plan.installed_delta = (int64_t)operation->plan.installed_bytes
                                     - (int64_t)operation->plan.removed_bytes;
    uint_fast64_t id = atomic_load(&next_id);
    do {
        if(id >= UINT64_MAX) {
            return(AXON_OPERATION_OVERFLOW);
        }
    } while(!atomic_compare_exchange_weak(&next_id, &id, id + 1));
    operation->plan.id = id + 1;
    return(AXON_OPERATION_OK);
}
