#define _POSIX_C_SOURCE 200809L

#include "operation_internal.h"

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
        result->status = AXON_OPERATION_MEMORY;
    }
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

static void question(void *context, alpm_question_t *question)
{
    axon_operation_t *operation = context;
    question->any.answer = 0;
    switch(question->type) {
        case ALPM_QUESTION_REPLACE_PKG:
            if(operation->allow_replacements) {
                question->replace.replace = 1;
                return;
            }
            break;
        case ALPM_QUESTION_INSTALL_IGNOREPKG:
            operation->ignored = 1;
            break;
        case ALPM_QUESTION_SELECT_PROVIDER:
            if(alpm_list_count(question->select_provider.providers) == 1) {
                question->select_provider.use_index = 0;
                return;
            }
            question->select_provider.use_index = -1;
            break;
        default:
            break;
    }
    operation->question = question->type;
}

static void finish(axon_operation_t *operation, axon_operation_result_t *result)
{
    if(operation->core != NULL) {
        alpm_errno_t error = axon_alpm_transaction_release(operation->core);
        if(error != ALPM_ERR_OK) {
            result->cleanup_error = error;
        }
        if(axon_alpm_release(&operation->core) != 0 && result->cleanup_error == ALPM_ERR_OK) {
            result->cleanup_error = ALPM_ERR_SYSTEM;
        }
    }
    operation->prepared = 0;
    if(result->cleanup_error != ALPM_ERR_OK) {
        if(result->status == AXON_OPERATION_OK) {
            result->status = AXON_OPERATION_BACKEND;
        }
        diagnostic(result, "cleanup: %s", alpm_strerror(result->cleanup_error));
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
    if((request->kind == AXON_OPERATION_UPGRADE) != (request->target_count == 0)) {
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
                || (target->repository != NULL && (request->kind != AXON_OPERATION_INSTALL
                                                   || target->repository[0] == '\0'))) {
            return(0);
        }
    }
    return(1);
}

static alpm_errno_t resolve(axon_alpm_t *core, const axon_operation_target_t *target,
                            alpm_pkg_t **package)
{
    if(target->repository != NULL) {
        return(axon_alpm_sync_package(core, target->repository, target->name, package));
    }
    const alpm_list_t *repositories = NULL;
    alpm_errno_t error = axon_alpm_repositories(core, &repositories);
    if(error != ALPM_ERR_OK) {
        return(error);
    }
    for(const alpm_list_t *item = repositories; item; item = item->next) {
        alpm_db_t *database = item->data;
        int usage;
        if(alpm_db_get_usage(database, &usage) < 0) {
            return(alpm_errno(alpm_db_get_handle(database)));
        }
        if(!(usage & ALPM_DB_USAGE_INSTALL)) {
            continue;
        }
        error = axon_alpm_sync_package(core, alpm_db_get_name(database), target->name, package);
        if(error != ALPM_ERR_PKG_NOT_FOUND) {
            return(error);
        }
    }
    return(ALPM_ERR_PKG_NOT_FOUND);
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
        : resolve(operation->core, target, &package);
    if(error != ALPM_ERR_OK) {
        return(error);
    }
    if(alpm_pkg_should_ignore(alpm_pkg_get_handle(package), package)) {
        error = ALPM_ERR_PKG_IGNORED;
    }

    else if(alpm_list_append_strdup(&operation->explicit_names, alpm_pkg_get_name(package)) == NULL) {
        error = ALPM_ERR_MEMORY;
    }

    else {
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
    axon_alpm_callbacks_t callbacks = {.question = question, .context = created};
    alpm_errno_t error = axon_alpm_callbacks(created->core, &callbacks);
    if(error == ALPM_ERR_OK) {
        error = axon_alpm_transaction_begin(created->core, request->flags);
    }
    if(error != ALPM_ERR_OK) {
        backend_problem(result, error, NULL);
        goto fail;
    }
    if(request->kind == AXON_OPERATION_UPGRADE) {
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

    else {
        for(size_t index = 0; index < request->target_count; index++) {
            error = queue(created, request->kind, &request->targets[index]);
            if(error != ALPM_ERR_OK) {
                diagnostic(result, "target: %s", request->targets[index].name);
                break;
            }
        }
    }
    axon_alpm_problem_t problem = {0};
    if(error == ALPM_ERR_OK && !created->question) {
        error = axon_alpm_transaction_prepare(created->core, &problem);
    }
    if(created->question) {
        result->status = created->ignored ? AXON_OPERATION_POLICY : AXON_OPERATION_QUESTION;
        result->backend_error = error;
        diagnostic(result, "unresolved libalpm question: %d", created->question);
    }

    else if(error != ALPM_ERR_OK) {
        backend_problem(result, error, &problem);
        if(error == ALPM_ERR_PKG_IGNORED) {
            result->status = AXON_OPERATION_POLICY;
        }
    }
    axon_alpm_problem_clear(&problem);
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
    *operation = created;
    return(AXON_OPERATION_OK);

fail:
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
    if(result == NULL) {
        return(AXON_OPERATION_ARGUMENT);
    }
    axon_operation_result_clear(result);
    if(operation == NULL || !operation->prepared) {
        result->status = AXON_OPERATION_STATE;
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
    axon_alpm_problem_t problem = {0};
    result->may_have_changes = operation->plan.count != 0;
    error = axon_alpm_transaction_commit(operation->core, &problem);
    if(error != ALPM_ERR_OK) {
        backend_problem(result, error, &problem);
    }

    else {
        result->committed = 1;
    }
    axon_alpm_problem_clear(&problem);
    if(operation->question) {
        result->status = AXON_OPERATION_QUESTION;
        diagnostic(result, "unresolved libalpm question: %d", operation->question);
    }
    finish(operation, result);
    return(result->status);
}

axon_operation_status_t axon_operation_cancel(axon_operation_t *operation,
    axon_operation_result_t *result)
{
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
    axon_operation_t operation = {0};
    if(axon_config_open(config, &operation.core, &result->config_error) != AXON_CONFIG_OK) {
        result->status = AXON_OPERATION_CONFIG;
        return(result->status);
    }
    result->may_have_changes = 1;
    alpm_errno_t error = axon_alpm_refresh(operation.core, force, &result->refreshed);
    if(error != ALPM_ERR_OK) {
        backend_problem(result, error, NULL);
    }
    finish(&operation, result);
    return(result->status);
}
