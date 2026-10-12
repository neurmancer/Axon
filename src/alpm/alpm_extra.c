#include "alpm_extra.h"
#include "alpm_internal.h"
#include "privilege.h"

#include <stdlib.h>
#include <string.h>

static alpm_errno_t idle(axon_alpm_t *core)
{
    if(core == NULL) {
        return(ALPM_ERR_HANDLE_NULL);
    }

    if(core->transaction != AXON_TRANS_IDLE) {
        return(ALPM_ERR_TRANS_NOT_NULL);
    }

    return(ALPM_ERR_OK);
}

static alpm_errno_t building(axon_alpm_t *core)
{
    if(core == NULL) {
        return(ALPM_ERR_HANDLE_NULL);
    }

    if(core->transaction != AXON_TRANS_BUILDING) {
        return(ALPM_ERR_TRANS_NOT_INITIALIZED);
    }

    if(axon_privilege_check(1) != AXON_PRIVILEGE_OK) {
        return(ALPM_ERR_BADPERMS);
    }

    return(ALPM_ERR_OK);
}

static alpm_errno_t status(axon_alpm_t *core, int result)
{
    if(result < 0) {
        return(alpm_errno(core->handle));
    }

    return(ALPM_ERR_OK);
}

static alpm_db_t *find_repository(axon_alpm_t *core, const char *name)
{
    for(alpm_list_t *item = alpm_get_syncdbs(core->handle); item; item = item->next) {
        alpm_db_t *database = item->data;
        if(strcmp(alpm_db_get_name(database), name) == 0) {
            return(database);
        }
    }

    return(NULL);
}

alpm_errno_t axon_alpm_option(axon_alpm_t *core, axon_alpm_option_t option,
                            const char *value)
{
    alpm_errno_t error = idle(core);
    int result;

    if(error != ALPM_ERR_OK) {
        return(error);
    }

    if(value == NULL || value[0] == '\0') {
        return(ALPM_ERR_WRONG_ARGS);
    }

    switch(option) {
        case AXON_ALPM_ARCHITECTURE:
            result = alpm_option_add_architecture(core->handle, value);
            break;
        case AXON_ALPM_CACHE_DIR:
            result = alpm_option_add_cachedir(core->handle, value);
            break;
        case AXON_ALPM_HOOK_DIR:
            result = alpm_option_add_hookdir(core->handle, value);
            break;
        case AXON_ALPM_GPG_DIR:
            result = alpm_option_set_gpgdir(core->handle, value);
            break;
        case AXON_ALPM_LOG_FILE:
            result = alpm_option_set_logfile(core->handle, value);
            break;
        case AXON_ALPM_IGNORE_PACKAGE:
            result = alpm_option_add_ignorepkg(core->handle, value);
            break;
        case AXON_ALPM_IGNORE_GROUP:
            result = alpm_option_add_ignoregroup(core->handle, value);
            break;
        case AXON_ALPM_NO_UPGRADE:
            result = alpm_option_add_noupgrade(core->handle, value);
            break;
        case AXON_ALPM_NO_EXTRACT:
            result = alpm_option_add_noextract(core->handle, value);
            break;
        case AXON_ALPM_SANDBOX_USER:
            result = alpm_option_set_sandboxuser(core->handle, value);
            if(result > 0) {
                return(ALPM_ERR_WRONG_ARGS);
            }
            break;
        default:
            return(ALPM_ERR_WRONG_ARGS);
    }

    return(status(core, result));
}

alpm_errno_t axon_alpm_signatures(axon_alpm_t *core, int defaults,
                                int local_files, int remote_files)
{
    alpm_errno_t error = idle(core);
    if(error != ALPM_ERR_OK) {
        return(error);
    }

    if(alpm_option_set_default_siglevel(core->handle, defaults) < 0
            || alpm_option_set_local_file_siglevel(core->handle, local_files) < 0
            || alpm_option_set_remote_file_siglevel(core->handle, remote_files) < 0) {
        return(alpm_errno(core->handle));
    }

    return(ALPM_ERR_OK);
}

alpm_errno_t axon_alpm_downloads(axon_alpm_t *core, unsigned int parallel)
{
    alpm_errno_t error = idle(core);
    if(error != ALPM_ERR_OK) {
        return(error);
    }

    if(parallel == 0) {
        return(ALPM_ERR_WRONG_ARGS);
    }

    return(status(core, alpm_option_set_parallel_downloads(core->handle, parallel)));
}

alpm_errno_t axon_alpm_check_space(axon_alpm_t *core, int enabled)
{
    alpm_errno_t error = idle(core);
    if(error != ALPM_ERR_OK) {
        return(error);
    }

    return(status(core, alpm_option_set_checkspace(core->handle, enabled != 0)));
}

alpm_errno_t axon_alpm_callbacks(axon_alpm_t *core,
                               const axon_alpm_callbacks_t *callbacks)
{
    alpm_errno_t error = idle(core);
    if(error != ALPM_ERR_OK) {
        return(error);
    }

    if(callbacks == NULL) {
        return(ALPM_ERR_WRONG_ARGS);
    }

    void *context = callbacks->context;
    if(alpm_option_set_logcb(core->handle, callbacks->log, context) < 0
            || alpm_option_set_eventcb(core->handle, callbacks->event, context) < 0
            || alpm_option_set_questioncb(core->handle, callbacks->question, context) < 0
            || alpm_option_set_progresscb(core->handle, callbacks->progress, context) < 0
            || alpm_option_set_dlcb(core->handle, callbacks->download, context) < 0
            || alpm_option_set_fetchcb(core->handle, callbacks->fetch, context) < 0) {
        return(alpm_errno(core->handle));
    }

    return(ALPM_ERR_OK);
}

alpm_errno_t axon_alpm_repo_add(axon_alpm_t *core, const char *name,
                              int siglevel, const alpm_list_t *servers, int usage)
{
    alpm_errno_t error = idle(core);
    alpm_db_t *database;

    if(error != ALPM_ERR_OK) {
        return(error);
    }

    if(name == NULL || name[0] == '\0' || strcmp(name, ".") == 0
            || strcmp(name, "..") == 0 || strcmp(name, "local") == 0
            || strchr(name, '/') != NULL || (usage & ~ALPM_DB_USAGE_ALL) != 0) {
        return(ALPM_ERR_WRONG_ARGS);
    }

    for(const alpm_list_t *item = servers; item; item = item->next) {
        const char *server = item->data;
        if(server == NULL || server[0] == '\0') {
            return(ALPM_ERR_WRONG_ARGS);
        }
    }

    if(find_repository(core, name) != NULL) {
        return(ALPM_ERR_DB_NOT_NULL);
    }

    database = alpm_register_syncdb(core->handle, name, siglevel);
    if(database == NULL) {
        return(alpm_errno(core->handle));
    }

    if(alpm_db_set_usage(database, usage) < 0) {
        goto fail;
    }

    for(const alpm_list_t *item = servers; item; item = item->next) {
        if(alpm_db_add_server(database, item->data) < 0) {
            goto fail;
        }
    }

    return(ALPM_ERR_OK);

fail:
    error = alpm_errno(core->handle);
    alpm_db_unregister(database);
    return(error);
}

alpm_errno_t axon_alpm_repo_remove(axon_alpm_t *core, const char *name)
{
    alpm_errno_t error = idle(core);
    alpm_db_t *database;

    if(error != ALPM_ERR_OK) {
        return(error);
    }

    if(name == NULL || name[0] == '\0') {
        return(ALPM_ERR_WRONG_ARGS);
    }

    database = find_repository(core, name);
    if(database == NULL) {
        return(ALPM_ERR_DB_NOT_FOUND);
    }

    return(status(core, alpm_db_unregister(database)));
}

alpm_errno_t axon_alpm_repositories(axon_alpm_t *core,
                                  const alpm_list_t **repositories)
{
    if(repositories == NULL) {
        return(ALPM_ERR_WRONG_ARGS);
    }

    *repositories = NULL;
    if(core == NULL) {
        return(ALPM_ERR_HANDLE_NULL);
    }

    *repositories = alpm_get_syncdbs(core->handle);
    return(ALPM_ERR_OK);
}

alpm_errno_t axon_alpm_refresh(axon_alpm_t *core, int force, int *changed)
{
    alpm_errno_t error = idle(core);
    alpm_list_t *databases;
    int result;

    if(changed != NULL) {
        *changed = 0;
    }

    if(error != ALPM_ERR_OK) {
        return(error);
    }

    if(axon_privilege_check(1) != AXON_PRIVILEGE_OK) {
        return(ALPM_ERR_BADPERMS);
    }

    databases = alpm_get_syncdbs(core->handle);
    if(databases == NULL) {
        return(ALPM_ERR_DB_NULL);
    }

    result = alpm_db_update(core->handle, databases, force != 0);
    if(result >= 0 && changed != NULL) {
        *changed = (result == 0);
    }

    return(status(core, result));
}

alpm_errno_t axon_alpm_sync_package(axon_alpm_t *core, const char *repository,
                                  const char *name, alpm_pkg_t **package)
{
    if(package == NULL) {
        return(ALPM_ERR_WRONG_ARGS);
    }

    *package = NULL;
    if(core == NULL) {
        return(ALPM_ERR_HANDLE_NULL);
    }

    if(name == NULL || name[0] == '\0' || (repository && repository[0] == '\0')) {
        return(ALPM_ERR_WRONG_ARGS);
    }

    if(repository != NULL && find_repository(core, repository) == NULL) {
        return(ALPM_ERR_DB_NOT_FOUND);
    }

    for(alpm_list_t *item = alpm_get_syncdbs(core->handle); item; item = item->next) {
        alpm_db_t *database = item->data;
        if(repository != NULL && strcmp(repository, alpm_db_get_name(database)) != 0) {
            continue;
        }

        if(alpm_db_get_valid(database) < 0) {
            return(alpm_errno(core->handle));
        }
        if(alpm_db_get_pkgcache(database) == NULL) {
            alpm_errno_t error = alpm_errno(core->handle);
            if(error != ALPM_ERR_OK) {
                return(error);
            }
        }

        *package = alpm_db_get_pkg(database, name);
        if(*package != NULL) {
            return(ALPM_ERR_OK);
        }

        alpm_errno_t error = alpm_errno(core->handle);
        if(error != ALPM_ERR_PKG_NOT_FOUND && error != ALPM_ERR_OK) {
            return(error);
        }
    }

    return(ALPM_ERR_PKG_NOT_FOUND);
}

alpm_errno_t axon_alpm_search(axon_alpm_t *core, int sync,
                            const alpm_list_t *patterns, alpm_list_t **packages)
{
    alpm_list_t *results = NULL;
    alpm_list_t local = {0};
    alpm_list_t *databases;

    if(packages == NULL || *packages != NULL) {
        return(ALPM_ERR_WRONG_ARGS);
    }

    if(core == NULL) {
        return(ALPM_ERR_HANDLE_NULL);
    }

    for(const alpm_list_t *item = patterns; item; item = item->next) {
        if(item->data == NULL) {
            return(ALPM_ERR_WRONG_ARGS);
        }
    }

    if(sync) {
        databases = alpm_get_syncdbs(core->handle);
    }

    else {
        local.data = alpm_get_localdb(core->handle);
        if(local.data == NULL) {
            return(alpm_errno(core->handle));
        }
        databases = &local;
    }

    for(alpm_list_t *item = databases; item; item = item->next) {
        alpm_list_t *matches = NULL;
        if(sync && alpm_db_get_valid(item->data) < 0) {
            alpm_errno_t error = alpm_errno(core->handle);
            alpm_list_free(results);
            return(error);
        }

        if(alpm_db_get_pkgcache(item->data) == NULL) {
            alpm_errno_t error = alpm_errno(core->handle);
            if(error != ALPM_ERR_OK) {
                alpm_list_free(results);
                return(error);
            }
        }

        if(patterns == NULL) {
            alpm_list_t *cache = alpm_db_get_pkgcache(item->data);
            matches = alpm_list_copy(cache);
            if(cache != NULL && matches == NULL) {
                alpm_list_free(results);
                return(ALPM_ERR_MEMORY);
            }
        }
        else if(alpm_db_search(item->data, patterns, &matches) < 0) {
            alpm_errno_t error = alpm_errno(core->handle);
            alpm_list_free(matches);
            alpm_list_free(results);
            return(error);
        }

        results = alpm_list_join(results, matches);
    }

    *packages = results;
    return(ALPM_ERR_OK);
}

alpm_errno_t axon_alpm_upgrades(axon_alpm_t *core, alpm_list_t **packages)
{
    const alpm_list_t *installed;
    alpm_list_t *results = NULL;
    alpm_errno_t error;

    if(packages == NULL || *packages != NULL) {
        return(ALPM_ERR_WRONG_ARGS);
    }

    error = axon_alpm_local_packages(core, &installed);
    if(error != ALPM_ERR_OK) {
        return(error);
    }

    for(alpm_list_t *item = alpm_get_syncdbs(core->handle); item; item = item->next) {
        if(alpm_db_get_valid(item->data) < 0) {
            return(alpm_errno(core->handle));
        }

        if(alpm_db_get_pkgcache(item->data) == NULL) {
            error = alpm_errno(core->handle);
            if(error != ALPM_ERR_OK) {
                return(error);
            }
        }
    }

    for(const alpm_list_t *item = installed; item; item = item->next) {
        alpm_pkg_t *package = alpm_sync_get_new_version(item->data,
                                                      alpm_get_syncdbs(core->handle));
        if(package != NULL && alpm_list_append(&results, package) == NULL) {
            alpm_list_free(results);
            return(ALPM_ERR_MEMORY);
        }
    }

    *packages = results;
    return(ALPM_ERR_OK);
}

alpm_errno_t axon_alpm_load(axon_alpm_t *core, const char *path,
                          alpm_pkg_t **package)
{
    if(package == NULL || *package != NULL) {
        return(ALPM_ERR_WRONG_ARGS);
    }

    if(core == NULL) {
        return(ALPM_ERR_HANDLE_NULL);
    }

    if(path == NULL || path[0] == '\0') {
        return(ALPM_ERR_WRONG_ARGS);
    }

    return(status(core, alpm_pkg_load(core->handle, path, 1,
                  alpm_option_get_local_file_siglevel(core->handle), package)));
}

alpm_errno_t axon_alpm_set_reason(axon_alpm_t *core, const char *name,
                                alpm_pkgreason_t reason)
{
    alpm_errno_t error = idle(core);
    alpm_pkg_t *package;

    if(error != ALPM_ERR_OK) {
        return(error);
    }

    if(reason != ALPM_PKG_REASON_EXPLICIT && reason != ALPM_PKG_REASON_DEPEND) {
        return(ALPM_ERR_WRONG_ARGS);
    }

    if(axon_privilege_check(1) != AXON_PRIVILEGE_OK) {
        return(ALPM_ERR_BADPERMS);
    }

    error = axon_alpm_local_package(core, name, &package);
    if(error != ALPM_ERR_OK) {
        return(error);
    }

    error = axon_alpm_transaction_begin(core, 0);
    if(error != ALPM_ERR_OK) {
        return(error);
    }

    error = status(core, alpm_pkg_set_reason(package, reason));
    alpm_errno_t released = axon_alpm_transaction_release(core);
    return(error == ALPM_ERR_OK ? released : error);
}

alpm_errno_t axon_alpm_transaction_begin(axon_alpm_t *core, int flags)
{
    alpm_errno_t error = idle(core);
    if(error != ALPM_ERR_OK) {
        return(error);
    }

    if(axon_privilege_check(1) != AXON_PRIVILEGE_OK) {
        return(ALPM_ERR_BADPERMS);
    }

    if(flags & ALPM_TRANS_FLAG_NOLOCK) {
        return(ALPM_ERR_WRONG_ARGS);
    }

    error = status(core, alpm_trans_init(core->handle, flags));
    if(error == ALPM_ERR_OK) {
        core->transaction = AXON_TRANS_BUILDING;
    }

    return(error);
}

alpm_errno_t axon_alpm_transaction_install(axon_alpm_t *core,
                                         const char *repository, const char *name)
{
    alpm_errno_t error = building(core);
    alpm_pkg_t *package;

    if(error != ALPM_ERR_OK) {
        return(error);
    }

    error = axon_alpm_sync_package(core, repository, name, &package);
    if(error != ALPM_ERR_OK) {
        return(error);
    }

    return(status(core, alpm_add_pkg(core->handle, package)));
}

alpm_errno_t axon_alpm_transaction_package(axon_alpm_t *core, alpm_pkg_t *package)
{
    alpm_errno_t error = building(core);
    if(error != ALPM_ERR_OK) {
        return(error);
    }
    if(package == NULL || alpm_pkg_get_handle(package) != core->handle
            || alpm_pkg_get_origin(package) == ALPM_PKG_FROM_LOCALDB) {
        return(ALPM_ERR_WRONG_ARGS);
    }
    int loaded = alpm_pkg_get_origin(package) == ALPM_PKG_FROM_FILE;
    error = status(core, alpm_add_pkg(core->handle, package));
    if(error == ALPM_ERR_OK && loaded
            && alpm_list_find_ptr(alpm_trans_get_add(core->handle), package) == NULL) {
        alpm_pkg_free(package);
    }
    return(error);
}

alpm_errno_t axon_alpm_transaction_file(axon_alpm_t *core, const char *path)
{
    alpm_errno_t error = building(core);
    alpm_pkg_t *package = NULL;

    if(error != ALPM_ERR_OK) {
        return(error);
    }

    error = axon_alpm_load(core, path, &package);
    if(error != ALPM_ERR_OK) {
        return(error);
    }

    error = axon_alpm_transaction_package(core, package);
    if(error != ALPM_ERR_OK) {
        alpm_pkg_free(package);
    }

    return(error);
}

alpm_errno_t axon_alpm_transaction_remove(axon_alpm_t *core, const char *name)
{
    alpm_errno_t error = building(core);
    alpm_pkg_t *package;

    if(error != ALPM_ERR_OK) {
        return(error);
    }

    error = axon_alpm_local_package(core, name, &package);
    if(error != ALPM_ERR_OK) {
        return(error);
    }

    return(status(core, alpm_remove_pkg(core->handle, package)));
}

alpm_errno_t axon_alpm_transaction_upgrade(axon_alpm_t *core, int downgrade)
{
    alpm_errno_t error = building(core);
    if(error != ALPM_ERR_OK) {
        return(error);
    }

    return(status(core, alpm_sync_sysupgrade(core->handle, downgrade != 0)));
}

alpm_errno_t axon_alpm_transaction_prepare(axon_alpm_t *core,
                                         axon_alpm_problem_t *problem)
{
    alpm_errno_t error = building(core);
    if(problem == NULL || problem->details != NULL) {
        return(ALPM_ERR_WRONG_ARGS);
    }

    if(error == ALPM_ERR_OK) {
        error = status(core, alpm_trans_prepare(core->handle, &problem->details));
        if(error == ALPM_ERR_OK) {
            core->transaction = AXON_TRANS_PREPARED;
        }

        else {
            core->transaction = AXON_TRANS_FINISHED;
        }
    }

    problem->code = error;
    return(error);
}

alpm_errno_t axon_alpm_transaction_plan(axon_alpm_t *core,
                                      const alpm_list_t **add,
                                      const alpm_list_t **remove)
{
    if(add == NULL || remove == NULL || add == remove) {
        return(ALPM_ERR_WRONG_ARGS);
    }

    *add = NULL;
    *remove = NULL;
    if(core == NULL) {
        return(ALPM_ERR_HANDLE_NULL);
    }

    if(core->transaction != AXON_TRANS_PREPARED) {
        return(ALPM_ERR_TRANS_NOT_PREPARED);
    }

    *add = alpm_trans_get_add(core->handle);
    *remove = alpm_trans_get_remove(core->handle);
    return(ALPM_ERR_OK);
}

alpm_errno_t axon_alpm_transaction_commit(axon_alpm_t *core,
                                        axon_alpm_problem_t *problem)
{
    if(problem == NULL || problem->details != NULL) {
        return(ALPM_ERR_WRONG_ARGS);
    }

    if(core == NULL) {
        problem->code = ALPM_ERR_HANDLE_NULL;
    }

    else if(core->transaction != AXON_TRANS_PREPARED) {
        problem->code = ALPM_ERR_TRANS_NOT_PREPARED;
    }

    else if(axon_privilege_check(1) != AXON_PRIVILEGE_OK) {
        problem->code = ALPM_ERR_BADPERMS;
    }

    else {
        problem->code = status(core, alpm_trans_commit(core->handle, &problem->details));
        core->transaction = AXON_TRANS_FINISHED;
    }

    return(problem->code);
}

alpm_errno_t axon_alpm_transaction_interrupt(axon_alpm_t *core)
{
    if(core == NULL) {
        return(ALPM_ERR_HANDLE_NULL);
    }

    return(status(core, alpm_trans_interrupt(core->handle)));
}

alpm_errno_t axon_alpm_transaction_release(axon_alpm_t *core)
{
    alpm_errno_t error;
    if(core == NULL) {
        return(ALPM_ERR_HANDLE_NULL);
    }

    if(core->transaction == AXON_TRANS_IDLE) {
        return(ALPM_ERR_OK);
    }

    error = status(core, alpm_trans_release(core->handle));
    core->transaction = AXON_TRANS_IDLE;
    return(error);
}

void axon_alpm_problem_clear(axon_alpm_problem_t *problem)
{
    if(problem == NULL) {
        return;
    }

    for(alpm_list_t *item = problem->details; item; item = item->next) {
        switch(problem->code) {
            case ALPM_ERR_UNSATISFIED_DEPS:
                alpm_depmissing_free(item->data);
                break;
            case ALPM_ERR_CONFLICTING_DEPS:
                alpm_conflict_free(item->data);
                break;
            case ALPM_ERR_FILE_CONFLICTS:
                alpm_fileconflict_free(item->data);
                break;
            default:
                free(item->data);
                break;
        }
    }

    alpm_list_free(problem->details);
    problem->details = NULL;
    problem->code = ALPM_ERR_OK;
}
