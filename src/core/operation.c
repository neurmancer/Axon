#define _POSIX_C_SOURCE 200809L

#include "operation_internal.h"
#include "maintenance.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void axon_operation_result_clear(axon_operation_result_t *result)
{
    if(result != NULL) {
        alpm_list_free_inner(result->diagnostics, free);
        alpm_list_free(result->diagnostics);
        memset(result, 0, sizeof(*result));
    }
}

static void diagnostic(axon_operation_result_t *result, const char *format, ...)
{
    va_list args, copy;
    va_start(args, format);
    va_copy(copy, args);
    int length = vsnprintf(NULL, 0, format, args);
    va_end(args);
    char *text = length >= 0 ? malloc((size_t)length + 1) : NULL;
    if(text != NULL) {
        vsnprintf(text, (size_t)length + 1, format, copy);
    }
    va_end(copy);
    if(text == NULL || alpm_list_append(&result->diagnostics, text) == NULL) {
        free(text);
        if(result->status == AXON_OPERATION_OK) {
            result->status = AXON_OPERATION_MEMORY;
        }
    }
}

static void preflight_message(void *context, const char *text)
{
    diagnostic(context, "%s", text);
}

static void backend_problem(axon_operation_result_t *result, alpm_errno_t error,
                            axon_alpm_problem_t *problem)
{
    result->status = AXON_OPERATION_BACKEND;
    result->backend_error = error;
    diagnostic(result, "%s", alpm_strerror(error));
    if(problem == NULL) {
        return;
    }
    for(alpm_list_t *item = problem->details; item; item = item->next) {
        switch(error) {
            case ALPM_ERR_UNSATISFIED_DEPS: {
                alpm_depmissing_t *missing = item->data;
                char *dependency = alpm_dep_compute_string(missing->depend);
                diagnostic(result, "%s requires %s%s%s", missing->target,
                           dependency != NULL ? dependency : "(unavailable)",
                           missing->causingpkg != NULL ? "; caused by " : "",
                           missing->causingpkg != NULL ? missing->causingpkg : "");
                free(dependency);
                break;
            }
            case ALPM_ERR_CONFLICTING_DEPS: {
                alpm_conflict_t *conflict = item->data;
                diagnostic(result, "%s conflicts with %s", alpm_pkg_get_name(conflict->package1),
                           alpm_pkg_get_name(conflict->package2));
                break;
            }
            case ALPM_ERR_FILE_CONFLICTS: {
                alpm_fileconflict_t *conflict = item->data;
                diagnostic(result, "%s: conflicting file %s", conflict->target, conflict->file);
                break;
            }
            case ALPM_ERR_PKG_INVALID_ARCH:
            case ALPM_ERR_PKG_INVALID:
            case ALPM_ERR_PKG_INVALID_CHECKSUM:
            case ALPM_ERR_PKG_INVALID_SIG:
                diagnostic(result, "%s", (char *)item->data);
                break;
            default:
                break;
        }
    }
}

static int authorized(axon_operation_result_t *result)
{
    result->privilege_error = axon_privilege_check(1);
    if(result->privilege_error != AXON_PRIVILEGE_OK) {
        result->status = AXON_OPERATION_PRIVILEGE;
        diagnostic(result, "%s", axon_privilege_message(result->privilege_error));
        return(0);
    }
    return(1);
}

static void cancellation(axon_operation_t *operation, axon_operation_result_t *result)
{
    if(axon_operation_poll(operation) && operation->cancelled) {
        result->cancelled = 1;
        result->committed = 0;
        if(result->status == AXON_OPERATION_OK) {
            result->status = AXON_OPERATION_CANCELLED;
        }
    }
}

static void finish(axon_operation_t *operation, axon_operation_result_t *result)
{
    operation->busy = 1;
    operation->cleanup_result = result;
    if(operation->core != NULL) {
        alpm_errno_t error = axon_alpm_transaction_release(operation->core);
        if(error != ALPM_ERR_OK) {
            result->cleanup_error = error;
        }
        if(axon_alpm_release(&operation->core) != 0 && result->cleanup_error == ALPM_ERR_OK) {
            result->cleanup_error = ALPM_ERR_SYSTEM;
        }
    }
    operation->cleanup_result = NULL;
    operation->prepared = 0;
    operation->busy = 0;
    if(result->cleanup_error != ALPM_ERR_OK) {
        if(result->status == AXON_OPERATION_OK) {
            result->status = AXON_OPERATION_BACKEND;
        }
        diagnostic(result, "cleanup: %s", alpm_strerror(result->cleanup_error));
    }
    if(result->may_have_changes) {
        cancellation(operation, result);
    }
}

static void destroy(axon_operation_t *operation)
{
    axon_operation_clear_plan(operation);
    alpm_list_free_inner(operation->hold_packages, free);
    alpm_list_free(operation->hold_packages);
    alpm_list_free_inner(operation->explicit_names, free);
    alpm_list_free(operation->explicit_names);
    free(operation);
}

static int validate(const axon_operation_request_t *request)
{
    if(request == NULL || request->kind < AXON_OPERATION_INSTALL || request->kind > AXON_OPERATION_UPGRADE
            || (request->target_count != 0 && request->targets == NULL)) {
        return(0);
    }
    if(request->kind != AXON_OPERATION_UPGRADE && request->target_count == 0) {
        return(0);
    }
    int allowed = ALPM_TRANS_FLAG_NOHOOKS | ALPM_TRANS_FLAG_NOSCRIPTLET;
    if(request->kind == AXON_OPERATION_REMOVE) {
        allowed |= ALPM_TRANS_FLAG_CASCADE | ALPM_TRANS_FLAG_RECURSE | ALPM_TRANS_FLAG_RECURSEALL
                   | ALPM_TRANS_FLAG_NOSAVE | ALPM_TRANS_FLAG_UNNEEDED;
        if(request->allow_downgrade || request->allow_replacements) {
            return(0);
        }
        if((request->flags & ALPM_TRANS_FLAG_RECURSEALL) && !(request->flags & ALPM_TRANS_FLAG_RECURSE)) {
            return(0);
        }
    }

    else {
        allowed |= ALPM_TRANS_FLAG_NEEDED;
    }
    if(request->flags & ~allowed) {
        return(0);
    }
    for(size_t index = 0; index < request->target_count; index++) {
        const axon_operation_target_t *target = &request->targets[index];
        if(target->name == NULL || target->name[0] == '\0'
                || (target->repository != NULL && ((request->kind != AXON_OPERATION_INSTALL
                                                    && request->kind != AXON_OPERATION_UPGRADE)
                                                   || target->repository[0] == '\0'))) {
            return(0);
        }
    }
    return(1);
}

static alpm_errno_t resolve(axon_operation_t *operation, const axon_operation_target_t *target,
                            alpm_pkg_t **package)
{
    axon_alpm_t *core = operation->core;
    const alpm_list_t *repositories = NULL;
    alpm_errno_t error = axon_alpm_repositories(core, &repositories);
    if(error != ALPM_ERR_OK) {
        return(error);
    }
    alpm_depend_t *dependency = alpm_dep_from_string(target->name);
    if(dependency == NULL) {
        return(ALPM_ERR_MEMORY);
    }
    alpm_list_t *providers = NULL;
    int repository_found = target->repository == NULL;
    for(const alpm_list_t *item = repositories; item; item = item->next) {
        if(axon_operation_poll(operation)) {
            error = ALPM_ERR_TRANS_ABORT;
            goto done;
        }
        alpm_db_t *database = item->data;
        const char *name = alpm_db_get_name(database);
        if(target->repository != NULL && strcmp(target->repository, name) != 0) {
            continue;
        }
        repository_found = 1;
        int usage;
        if(alpm_db_get_usage(database, &usage) < 0) {
            error = alpm_errno(alpm_db_get_handle(database));
            goto done;
        }
        if(target->repository == NULL && !(usage & ALPM_DB_USAGE_INSTALL)) {
            continue;
        }
        error = axon_alpm_sync_package(core, name, dependency->name, package);
        if(error != ALPM_ERR_OK && error != ALPM_ERR_PKG_NOT_FOUND) {
            goto done;
        }
        alpm_list_t single = {.data = *package};
        if(*package != NULL && alpm_find_satisfier(&single, target->name) != NULL) {
            error = ALPM_ERR_OK;
            goto done;
        }
        *package = NULL;
        for(alpm_list_t *candidate = alpm_db_get_pkgcache(database); candidate; candidate = candidate->next) {
            if(axon_operation_poll(operation)) {
                error = ALPM_ERR_TRANS_ABORT;
                goto done;
            }
            single.data = candidate->data;
            if(alpm_find_satisfier(&single, target->name) != NULL
                    && alpm_pkg_find(providers, alpm_pkg_get_name(candidate->data)) == NULL) {
                if(alpm_list_append(&providers, candidate->data) == NULL) {
                    error = ALPM_ERR_MEMORY;
                    goto done;
                }
            }
        }
    }
    error = repository_found ? ALPM_ERR_PKG_NOT_FOUND : ALPM_ERR_DB_NOT_FOUND;
    if(providers != NULL) {
        for(alpm_list_t *item = providers; item; item = item->next) {
            alpm_pkg_t *installed = NULL;
            alpm_errno_t local = axon_alpm_local_package(core, alpm_pkg_get_name(item->data), &installed);
            if(local != ALPM_ERR_OK && local != ALPM_ERR_PKG_NOT_FOUND) {
                error = local;
                goto done;
            }
            if(installed != NULL) {
                *package = item->data;
                break;
            }
        }
        if(*package == NULL && providers->next == NULL) {
            *package = providers->data;
        }
        if(*package == NULL) {
            alpm_question_t question = {.select_provider = {
                .type = ALPM_QUESTION_SELECT_PROVIDER, .providers = providers, .depend = dependency}};
            axon_operation_question(operation, &question);
            if(question.select_provider.use_index >= 0) {
                *package = alpm_list_nth(providers, (size_t)question.select_provider.use_index)->data;
            }
            else {
                error = ALPM_ERR_TRANS_ABORT;
            }
        }
        if(*package != NULL) {
            error = ALPM_ERR_OK;
        }
    }
done:
    alpm_list_free(providers);
    alpm_dep_free(dependency);
    return(error);
}

static alpm_errno_t queue(axon_operation_t *operation, axon_operation_kind_t kind,
                          const axon_operation_target_t *target)
{
    if(kind == AXON_OPERATION_REMOVE) {
        if(alpm_list_append_strdup(&operation->explicit_names, target->name) == NULL) {
            return(ALPM_ERR_MEMORY);
        }
        return(axon_alpm_transaction_remove(operation->core, target->name));
    }
    alpm_pkg_t *package = NULL;
    alpm_errno_t error = kind == AXON_OPERATION_FILE
        ? axon_alpm_load(operation->core, target->name, &package)
        : resolve(operation, target, &package);
    if(error == ALPM_ERR_PKG_NOT_FOUND && kind == AXON_OPERATION_INSTALL) {
        const alpm_list_t *repositories = NULL;
        error = axon_alpm_repositories(operation->core, &repositories);
        int found = 0;
        for(const alpm_list_t *item = repositories; error == ALPM_ERR_OK && item; item = item->next) {
            alpm_db_t *database = item->data;
            const char *name = alpm_db_get_name(database);
            int usage;
            if(target->repository != NULL && strcmp(target->repository, name) != 0) {
                continue;
            }
            if(alpm_db_get_usage(database, &usage) < 0) {
                return(alpm_errno(alpm_db_get_handle(database)));
            }
            if(target->repository == NULL && !(usage & ALPM_DB_USAGE_INSTALL)) {
                continue;
            }
            alpm_group_t *group = alpm_db_get_group(database, target->name);
            if(group == NULL) {
                continue;
            }
            found = 1;
            for(alpm_list_t *member = group->packages; member; member = member->next) {
                if(axon_operation_poll(operation)) {
                    return(ALPM_ERR_TRANS_ABORT);
                }
                axon_operation_target_t expanded = {.name = alpm_pkg_get_name(member->data), .repository = name};
                error = queue(operation, kind, &expanded);
                if(error != ALPM_ERR_OK) {
                    break;
                }
            }
        }
        return(error != ALPM_ERR_OK ? error : found ? ALPM_ERR_OK : ALPM_ERR_PKG_NOT_FOUND);
    }
    if(error != ALPM_ERR_OK) {
        return(error);
    }
    if(alpm_list_find_str(operation->explicit_names, alpm_pkg_get_name(package)) != NULL) {
        alpm_pkg_t *queued = alpm_pkg_find(alpm_trans_get_add(alpm_pkg_get_handle(package)),
                                         alpm_pkg_get_name(package));
        if(queued != NULL && alpm_pkg_vercmp(alpm_pkg_get_version(queued), alpm_pkg_get_version(package)) != 0) {
            error = ALPM_ERR_TRANS_DUP_TARGET;
        }
        if(kind == AXON_OPERATION_FILE) {
            alpm_pkg_free(package);
        }
        return(error);
    }
    if(alpm_pkg_should_ignore(alpm_pkg_get_handle(package), package)) {
        alpm_question_t value = {.install_ignorepkg = {
            .type = ALPM_QUESTION_INSTALL_IGNOREPKG, .pkg = package}};
        axon_operation_question(operation, &value);
        if(!value.install_ignorepkg.install) {
            error = ALPM_ERR_PKG_IGNORED;
        }
    }

    if(error == ALPM_ERR_OK && alpm_list_append_strdup(&operation->explicit_names, alpm_pkg_get_name(package)) == NULL) {
        error = ALPM_ERR_MEMORY;
    }

    else if(error == ALPM_ERR_OK) {
        error = axon_alpm_transaction_package(operation->core, package);
    }
    if(error != ALPM_ERR_OK && kind == AXON_OPERATION_FILE) {
        alpm_pkg_free(package);
    }
    return(error);
}

axon_operation_status_t axon_operation_prepare(const axon_config_t *config,
    const axon_operation_request_t *request, axon_operation_t **operation,
    axon_operation_result_t *result)
{
    if(result == NULL) {
        return(AXON_OPERATION_ARGUMENT);
    }
    axon_operation_result_clear(result);
    if(config == NULL || operation == NULL || *operation != NULL || !validate(request)) {
        result->status = AXON_OPERATION_ARGUMENT;
        return(result->status);
    }
    if(!authorized(result)) {
        return(result->status);
    }
    axon_operation_t *created = calloc(1, sizeof(*created));
    if(created == NULL) {
        result->status = AXON_OPERATION_MEMORY;
        return(result->status);
    }
    created->callbacks = request->callbacks;
    created->busy = 1;
    cancellation(created, result);
    if(result->cancelled) {
        goto fail;
    }
    created->allow_downgrade = request->allow_downgrade;
    created->allow_replacements = request->allow_replacements;
    created->plan.flags = request->flags;
    const axon_config_options_t *options = axon_config_options(config);
    for(const alpm_list_t *item = options->hold_packages; item; item = item->next) {
        if(alpm_list_append_strdup(&created->hold_packages, item->data) == NULL) {
            result->status = AXON_OPERATION_MEMORY;
            goto fail;
        }
    }
    if(axon_config_open(config, &created->core, &result->config_error) != AXON_CONFIG_OK) {
        result->status = AXON_OPERATION_CONFIG;
        goto fail;
    }
    alpm_errno_t error = axon_operation_set_callbacks(created);
    if(error == ALPM_ERR_OK) {
        error = axon_alpm_transaction_begin(created->core, request->flags);
    }
    if(error != ALPM_ERR_OK) {
        backend_problem(result, error, NULL);
        goto fail;
    }
    for(size_t index = 0; index < request->target_count; index++) {
        if(axon_operation_poll(created)) {
            break;
        }
        error = queue(created, request->kind == AXON_OPERATION_UPGRADE
            ? AXON_OPERATION_INSTALL : request->kind, &request->targets[index]);
        if(error != ALPM_ERR_OK) {
            diagnostic(result, "target: %s", request->targets[index].name);
            break;
        }
    }
    if(error == ALPM_ERR_OK && !axon_operation_poll(created) && request->kind == AXON_OPERATION_UPGRADE) {
        const alpm_list_t *repositories = NULL;
        axon_alpm_repositories(created->core, &repositories);
        if(repositories == NULL) {
            backend_problem(result, ALPM_ERR_DB_NULL, NULL);
            goto fail;
        }
        for(const alpm_list_t *item = repositories; item; item = item->next) {
            alpm_db_t *database = item->data;
            if(alpm_db_get_pkgcache(database) == NULL) {
                error = alpm_errno(alpm_db_get_handle(database));
                if(error != ALPM_ERR_OK) {
                    backend_problem(result, error, NULL);
                    goto fail;
                }
            }
        }
        error = axon_alpm_transaction_upgrade(created->core, request->allow_downgrade);
    }

    axon_alpm_problem_t problem = {0};
    if(error == ALPM_ERR_OK && !axon_operation_poll(created)) {
        error = axon_alpm_transaction_prepare(created->core, &problem);
    }
    if(created->question) {
        result->status = created->ignored ? AXON_OPERATION_POLICY : AXON_OPERATION_QUESTION;
        result->backend_error = error;
        diagnostic(result, "unresolved libalpm question: %d", created->question);
    }

    else if(error == ALPM_ERR_TRANS_ABORT && created->cancelled) {
        result->status = AXON_OPERATION_CANCELLED;
    }

    else if(error != ALPM_ERR_OK) {
        backend_problem(result, error, &problem);
        if(error == ALPM_ERR_PKG_IGNORED) {
            result->status = AXON_OPERATION_POLICY;
        }
    }
    axon_alpm_problem_clear(&problem);
    cancellation(created, result);
    if(result->status != AXON_OPERATION_OK) {
        goto fail;
    }
    result->status = axon_operation_build_plan(created);
    if(result->status != AXON_OPERATION_OK) {
        result->backend_error = created->plan_error;
        diagnostic(result, "cannot build prepared plan");
        goto fail;
    }
    if(created->plan.has_downgrades && !created->allow_downgrade) {
        result->status = AXON_OPERATION_POLICY;
        diagnostic(result, "downgrade requires an explicit request");
        goto fail;
    }
    created->prepared = 1;
    created->busy = 0;
    *operation = created;
    return(AXON_OPERATION_OK);

fail:
    cancellation(created, result);
    finish(created, result);
    destroy(created);
    return(result->status);
}

const axon_operation_plan_t *axon_operation_plan(const axon_operation_t *operation)
{
    return(operation != NULL ? &operation->plan : NULL);
}

static int name_approved(const axon_operation_approval_t *approval, const char *name)
{
    for(size_t index = 0; index < approval->protected_removal_count; index++) {
        if(strcmp(approval->protected_removals[index], name) == 0) {
            return(1);
        }
    }
    return(0);
}

axon_operation_status_t axon_operation_commit(axon_operation_t *operation,
    const axon_operation_approval_t *approval, axon_operation_result_t *result)
{
    if(operation != NULL && operation->busy) {
        return(AXON_OPERATION_STATE);
    }
    if(result == NULL) {
        return(AXON_OPERATION_ARGUMENT);
    }
    axon_operation_result_clear(result);
    if(operation == NULL || !operation->prepared) {
        result->status = AXON_OPERATION_STATE;
        return(result->status);
    }
    cancellation(operation, result);
    if(result->cancelled) {
        finish(operation, result);
        return(result->status);
    }
    if(approval == NULL || !approval->confirm || approval->plan_id != operation->plan.id
            || approval->approved_flags != operation->plan.flags
            || (operation->plan.has_downgrades && !approval->approve_downgrades)) {
        result->status = AXON_OPERATION_APPROVAL;
        return(result->status);
    }
    if(approval->protected_removal_count != 0 && approval->protected_removals == NULL) {
        result->status = AXON_OPERATION_ARGUMENT;
        return(result->status);
    }
    for(size_t index = 0; index < approval->protected_removal_count; index++) {
        if(approval->protected_removals[index] == NULL) {
            result->status = AXON_OPERATION_ARGUMENT;
            return(result->status);
        }
    }
    const alpm_list_t *additions = NULL;
    const alpm_list_t *removals = NULL;
    alpm_errno_t error = axon_alpm_transaction_plan(operation->core, &additions, &removals);
    if(error != ALPM_ERR_OK) {
        backend_problem(result, error, NULL);
        return(result->status);
    }
    for(const alpm_list_t *item = removals; item; item = item->next) {
        const char *name = alpm_pkg_get_name(item->data);
        if(axon_operation_held(operation, name) && !name_approved(approval, name)) {
            result->status = AXON_OPERATION_POLICY;
            diagnostic(result, "protected removal requires exact-name approval: %s", name);
        }
    }
    if(result->status != AXON_OPERATION_OK) {
        return(result->status);
    }
    if(!authorized(result)) {
        return(result->status);
    }
    if(operation->plan.count == 0) {
        result->committed = 1;
        finish(operation, result);
        return(result->status);
    }
    error = axon_maintenance_writable(operation->core, additions, removals, preflight_message, result);
    if(error != ALPM_ERR_OK) {
        backend_problem(result, error, NULL);
        finish(operation, result);
        return(result->status);
    }
    axon_alpm_problem_t problem = {0};
    result->may_have_changes = operation->plan.count != 0;
    operation->busy = 1;
    operation->committing = 1;
    operation->commit_result = result;
    error = axon_alpm_transaction_commit(operation->core, &problem);
    operation->commit_result = NULL;
    operation->committing = 0;
    if(error != ALPM_ERR_OK) {
        backend_problem(result, error, &problem);
    }

    else {
        result->committed = 1;
        if(result->execution_errors != 0) {
            result->status = AXON_OPERATION_EXECUTION;
        }
        else if(result->diagnostics_incomplete) {
            result->status = AXON_OPERATION_MEMORY;
        }
    }
    axon_alpm_problem_clear(&problem);
    if(operation->question) {
        result->committed = 0;
        result->status = AXON_OPERATION_QUESTION;
        diagnostic(result, "unresolved libalpm question: %d", operation->question);
    }
    cancellation(operation, result);
    finish(operation, result);
    return(result->status);
}

axon_operation_status_t axon_operation_cancel(axon_operation_t *operation,
    axon_operation_result_t *result)
{
    if(operation != NULL && operation->busy) {
        return(AXON_OPERATION_STATE);
    }
    if(result == NULL) {
        return(AXON_OPERATION_ARGUMENT);
    }
    axon_operation_result_clear(result);
    if(operation == NULL) {
        result->status = AXON_OPERATION_ARGUMENT;
    }

    else {
        finish(operation, result);
    }
    return(result->status);
}

axon_operation_status_t axon_operation_free(axon_operation_t **operation,
    axon_operation_result_t *result)
{
    if(operation != NULL && *operation != NULL && (*operation)->busy) {
        return(AXON_OPERATION_STATE);
    }
    if(result == NULL) {
        return(AXON_OPERATION_ARGUMENT);
    }
    axon_operation_result_clear(result);
    if(operation == NULL) {
        result->status = AXON_OPERATION_ARGUMENT;
    }

    else if(*operation != NULL) {
        finish(*operation, result);
        destroy(*operation);
        *operation = NULL;
    }
    return(result->status);
}

axon_operation_status_t axon_operation_refresh(const axon_config_t *config,
    int force, axon_operation_result_t *result)
{
    return(axon_operation_refresh_with_callbacks(config, force, NULL, result));
}

axon_operation_status_t axon_operation_refresh_with_callbacks(const axon_config_t *config,
    int force, const axon_operation_callbacks_t *callbacks, axon_operation_result_t *result)
{
    if(result == NULL) {
        return(AXON_OPERATION_ARGUMENT);
    }
    axon_operation_result_clear(result);
    if(config == NULL) {
        result->status = AXON_OPERATION_ARGUMENT;
        return(result->status);
    }
    if(!authorized(result)) {
        return(result->status);
    }
    axon_operation_t operation = {.busy = 1};
    if(callbacks != NULL) {
        operation.callbacks = *callbacks;
    }
    cancellation(&operation, result);
    if(result->cancelled) {
        return(result->status);
    }
    if(axon_config_open(config, &operation.core, &result->config_error) != AXON_CONFIG_OK) {
        result->status = AXON_OPERATION_CONFIG;
        return(result->status);
    }
    result->may_have_changes = 1;
    alpm_errno_t error = axon_operation_set_callbacks(&operation);
    if(error == ALPM_ERR_OK) {
        error = axon_alpm_refresh(operation.core, force, &result->refreshed);
    }
    if(error != ALPM_ERR_OK) {
        backend_problem(result, error, NULL);
    }
    if(operation.question) {
        result->status = AXON_OPERATION_QUESTION;
        diagnostic(result, "unresolved libalpm question: %d", operation.question);
    }
    cancellation(&operation, result);
    finish(&operation, result);
    return(result->status);
}

axon_operation_status_t axon_operation_prepare_upgrade(const axon_config_t *config,
    int force, const axon_operation_request_t *request, axon_operation_t **operation,
    axon_operation_result_t *result)
{
    if(result == NULL) {
        return(AXON_OPERATION_ARGUMENT);
    }
    axon_operation_result_clear(result);
    if(config == NULL || operation == NULL || *operation != NULL || !validate(request)
            || request->kind != AXON_OPERATION_UPGRADE) {
        result->status = AXON_OPERATION_ARGUMENT;
        return(result->status);
    }
    if(axon_operation_refresh_with_callbacks(config, force, &request->callbacks, result)
            != AXON_OPERATION_OK) {
        return(result->status);
    }
    int refreshed = result->refreshed;
    int changed = result->may_have_changes;
    axon_operation_prepare(config, request, operation, result);
    result->refreshed = refreshed;
    result->may_have_changes |= changed;
    return(result->status);
}
