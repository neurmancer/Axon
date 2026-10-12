#define _POSIX_C_SOURCE 200809L
#include "maintenance.h"
#include "alpm_internal.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int axon_package_orphan(alpm_pkg_t *package)
{
    if(alpm_pkg_get_reason(package) != ALPM_PKG_REASON_DEPEND) {
        return(0);
    }
    alpm_list_t *required = alpm_pkg_compute_requiredby(package);
    alpm_list_t *optional = alpm_pkg_compute_optionalfor(package);
    int orphan = required == NULL && optional == NULL;
    alpm_list_free_inner(required, free);
    alpm_list_free(required);
    alpm_list_free_inner(optional, free);
    alpm_list_free(optional);
    return(orphan);
}

alpm_errno_t axon_maintenance_check(axon_alpm_t *core,
    axon_maintenance_message_t message, void *context)
{
    if(core == NULL || message == NULL) {
        return(ALPM_ERR_WRONG_ARGS);
    }
    alpm_handle_t *handle = core->handle;
    struct stat info;
    const char *lock = alpm_option_get_lockfile(handle);
    if(lstat(lock, &info) == 0) {
        message(context, "Database lock exists. It may belong to a running package manager; verify that no transaction is active before manual recovery. Axon will not remove it.");
        return(ALPM_ERR_HANDLE_LOCK);
    }
    if(errno != ENOENT) {
        return(ALPM_ERR_SYSTEM);
    }
    alpm_db_t *local = alpm_get_localdb(handle);
    if(local == NULL || alpm_db_get_valid(local) < 0) {
        message(context, "Local database is invalid. Preserve it and the package-manager log before repairing it from backups or reinstalling affected packages.");
        return(ALPM_ERR_DB_INVALID);
    }
    alpm_list_t *packages = alpm_db_get_pkgcache(local);
    if(packages == NULL && alpm_errno(handle) != ALPM_ERR_OK) {
        return(alpm_errno(handle));
    }
    alpm_list_t *missing = alpm_checkdeps(handle, packages, NULL, packages, 0);
    alpm_errno_t error = ALPM_ERR_OK;
    for(alpm_list_t *item = missing; item; item = item->next) {
        alpm_depmissing_t *problem = item->data;
        char *dependency = alpm_dep_compute_string(problem->depend);
        char text[1024];
        snprintf(text, sizeof(text), "%s requires %s", problem->target,
                 dependency != NULL ? dependency : "an unavailable dependency");
        message(context, text);
        free(dependency);
        alpm_depmissing_free(problem);
        error = ALPM_ERR_UNSATISFIED_DEPS;
    }
    alpm_list_free(missing);
    alpm_list_t *conflicts = alpm_checkconflicts(handle, packages);
    for(alpm_list_t *item = conflicts; item; item = item->next) {
        alpm_conflict_t *conflict = item->data;
        char text[1024];
        snprintf(text, sizeof(text), "%s conflicts with %s",
                 alpm_pkg_get_name(conflict->package1), alpm_pkg_get_name(conflict->package2));
        message(context, text);
        alpm_conflict_free(conflict);
        error = ALPM_ERR_CONFLICTING_DEPS;
    }
    alpm_list_free(conflicts);
    for(alpm_list_t *item = alpm_get_syncdbs(handle); item; item = item->next) {
        alpm_db_t *database = item->data;
        if(alpm_db_get_valid(database) < 0
                || (alpm_db_get_pkgcache(database) == NULL && alpm_errno(handle) != ALPM_ERR_OK)) {
            char text[1024];
            snprintf(text, sizeof(text), "Repository %s is missing or invalid. Use a successful full refresh-and-upgrade to recover; do not install from a partial refresh.", alpm_db_get_name(database));
            message(context, text);
            error = ALPM_ERR_DB_INVALID;
        }
    }
    return(error);
}

static void changed(axon_operation_result_t *result, const char *action, const char *name)
{
    size_t size = strlen(action) + strlen(name) + 3;
    char *text = malloc(size);
    if(text == NULL) {
        result->status = AXON_OPERATION_MEMORY;
        return;
    }
    snprintf(text, size, "%s: %s", action, name);
    if(alpm_list_append(&result->diagnostics, text) == NULL) {
        free(text);
        result->status = AXON_OPERATION_MEMORY;
    }
}

static int begin(const axon_config_t *config, axon_alpm_t **core, axon_operation_result_t *result)
{
    axon_operation_result_clear(result);
    result->privilege_error = axon_privilege_check(1);
    if(result->privilege_error != AXON_PRIVILEGE_OK) {
        result->status = AXON_OPERATION_PRIVILEGE;
        return(0);
    }
    if(axon_config_open(config, core, &result->config_error) != AXON_CONFIG_OK) {
        result->status = AXON_OPERATION_CONFIG;
        return(0);
    }
    result->backend_error = axon_alpm_transaction_begin(*core, 0);
    if(result->backend_error != ALPM_ERR_OK) {
        result->status = AXON_OPERATION_BACKEND;
        return(0);
    }
    return(1);
}

static void cleanup_log(void *context, alpm_loglevel_t level, const char *format, va_list args)
{
    axon_operation_result_t *result = context;
    (void)format;
    (void)args;
    if(level & (ALPM_LOG_ERROR | ALPM_LOG_WARNING)) {
        result->cleanup_error = ALPM_ERR_SYSTEM;
    }
}

static axon_operation_status_t finish(axon_alpm_t **core, axon_operation_result_t *result)
{
    if(*core != NULL) {
        alpm_option_set_logcb((*core)->handle, cleanup_log, result);
        alpm_errno_t released = axon_alpm_transaction_release(*core);
        if(released != ALPM_ERR_OK) {
            result->cleanup_error = released;
        }
        if(axon_alpm_release(core) != 0 && result->cleanup_error == ALPM_ERR_OK) {
            result->cleanup_error = ALPM_ERR_SYSTEM;
        }
    }
    if(result->status == AXON_OPERATION_OK
            && (result->backend_error != ALPM_ERR_OK || result->cleanup_error != ALPM_ERR_OK)) {
        result->status = AXON_OPERATION_BACKEND;
    }
    return(result->status);
}

axon_operation_status_t axon_maintenance_reason(const axon_config_t *config,
    size_t count, const char *const *names, alpm_pkgreason_t reason,
    const volatile sig_atomic_t *cancel, axon_operation_result_t *result)
{
    if(result == NULL) {
        return(AXON_OPERATION_ARGUMENT);
    }
    axon_operation_result_clear(result);
    if(config == NULL || count == 0 || names == NULL
            || (reason != ALPM_PKG_REASON_EXPLICIT && reason != ALPM_PKG_REASON_DEPEND)) {
        result->status = AXON_OPERATION_ARGUMENT;
        return(result->status);
    }
    axon_alpm_t *core = NULL;
    if(!begin(config, &core, result)) {
        return(finish(&core, result));
    }
    alpm_db_t *local = alpm_get_localdb(core->handle);
    for(size_t index = 0; index < count; index++) {
        if(names[index] == NULL || alpm_db_get_pkg(local, names[index]) == NULL) {
            result->backend_error = ALPM_ERR_PKG_NOT_FOUND;
            changed(result, "Installed package not found", names[index] != NULL ? names[index] : "(null)");
            return(finish(&core, result));
        }
    }
    for(size_t index = 0; index < count; index++) {
        if(cancel != NULL && *cancel) {
            result->status = AXON_OPERATION_CANCELLED;
            result->cancelled = 1;
            break;
        }
        result->privilege_error = axon_privilege_check(1);
        if(result->privilege_error != AXON_PRIVILEGE_OK) {
            result->status = AXON_OPERATION_PRIVILEGE;
            break;
        }
        result->may_have_changes = 1;
        if(alpm_pkg_set_reason(alpm_db_get_pkg(local, names[index]), reason) < 0) {
            result->backend_error = alpm_errno(core->handle);
            break;
        }
        changed(result, "Installation reason changed", names[index]);
        if(result->status != AXON_OPERATION_OK) {
            break;
        }
    }
    return(finish(&core, result));
}

typedef struct {
    int directory;
    char *name;
    struct stat identity;
} candidate_t;

static int retain(axon_alpm_t *core, alpm_pkg_t *package, const axon_config_options_t *options)
{
    const char *name = alpm_pkg_get_name(package);
    const char *version = alpm_pkg_get_version(package);
    alpm_pkg_t *installed = alpm_db_get_pkg(alpm_get_localdb(core->handle), name);
    if(options->clean_keep_installed && installed != NULL
            && alpm_pkg_vercmp(version, alpm_pkg_get_version(installed)) == 0) {
        return(1);
    }
    if(options->clean_keep_current) {
        for(alpm_list_t *item = alpm_get_syncdbs(core->handle); item; item = item->next) {
            alpm_pkg_t *current = alpm_db_get_pkg(item->data, name);
            if(current != NULL && alpm_pkg_vercmp(version, alpm_pkg_get_version(current)) == 0) {
                return(1);
            }
        }
    }
    return(0);
}

static int candidate(alpm_list_t **entries, alpm_list_t **paths, int directory,
                     const char *base, const char *name, const struct stat *expected)
{
    struct stat info;
    if(fstatat(directory, name, &info, AT_SYMLINK_NOFOLLOW) < 0) {
        return(errno == ENOENT && expected == NULL ? 0 : -1);
    }
    if(!S_ISREG(info.st_mode)) {
        errno = ESTALE;
        return(expected == NULL ? 0 : -1);
    }
    if(expected != NULL && (info.st_dev != expected->st_dev || info.st_ino != expected->st_ino
            || info.st_size != expected->st_size || info.st_mtim.tv_sec != expected->st_mtim.tv_sec
            || info.st_mtim.tv_nsec != expected->st_mtim.tv_nsec)) {
        errno = ESTALE;
        return(-1);
    }
    candidate_t *entry = calloc(1, sizeof(*entry));
    char *path = malloc(strlen(base) + strlen(name) + 2);
    if(entry == NULL || path == NULL) {
        free(entry);
        free(path);
        return(-1);
    }
    entry->directory = directory;
    entry->name = strdup(name);
    entry->identity = info;
    sprintf(path, "%s/%s", base, name);
    if(entry->directory < 0 || entry->name == NULL || alpm_list_append(entries, entry) == NULL) {
        free(entry->name);
        free(entry);
        free(path);
        return(-1);
    }
    if(alpm_list_append(paths, path) == NULL) {
        free(path);
        return(-1);
    }
    return(0);
}

axon_operation_status_t axon_maintenance_clean(const axon_config_t *config,
    axon_cache_approval_t approve, void *context, const volatile sig_atomic_t *cancel,
    axon_operation_result_t *result)
{
    if(result == NULL) {
        return(AXON_OPERATION_ARGUMENT);
    }
    axon_operation_result_clear(result);
    if(config == NULL || approve == NULL) {
        result->status = AXON_OPERATION_ARGUMENT;
        return(result->status);
    }
    axon_alpm_t *core = NULL;
    alpm_list_t *entries = NULL, *paths = NULL, *directories = NULL;
    if(!begin(config, &core, result)) {
        return(finish(&core, result));
    }
    const axon_config_options_t *options = axon_config_options(config);
    if(alpm_db_get_valid(alpm_get_localdb(core->handle)) < 0) {
        result->backend_error = ALPM_ERR_DB_INVALID;
        goto done;
    }
    alpm_db_t *local = alpm_get_localdb(core->handle);
    if(alpm_db_get_pkgcache(local) == NULL && alpm_errno(core->handle) != ALPM_ERR_OK) {
        result->backend_error = alpm_errno(core->handle);
        goto done;
    }
    if(options->clean_keep_current) {
        for(alpm_list_t *item = alpm_get_syncdbs(core->handle); item; item = item->next) {
            if(alpm_db_get_valid(item->data) < 0
                    || (alpm_db_get_pkgcache(item->data) == NULL && alpm_errno(core->handle) != ALPM_ERR_OK)) {
                result->backend_error = ALPM_ERR_DB_INVALID;
                goto done;
            }
        }
    }
    for(const alpm_list_t *item = options->cache_dirs; item; item = item->next) {
        const char *base = item->data;
        int fd = open(base, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if(fd < 0) {
            if(errno == ENOENT) {
                continue;
            }
            result->backend_error = ALPM_ERR_BADPERMS;
            changed(result, "Cache directory must be accessible, owned by the invoking root identity, and not writable by group or others", base);
            goto done;
        }
        struct stat directory_info;
        if(fstat(fd, &directory_info) < 0 || directory_info.st_uid != geteuid()
                || (directory_info.st_mode & (S_IWGRP | S_IWOTH))) {
            close(fd);
            result->backend_error = ALPM_ERR_BADPERMS;
            changed(result, "Cache directory must be accessible, owned by the invoking root identity, and not writable by group or others", base);
            goto done;
        }
        int duplicate = 0;
        for(alpm_list_t *opened = directories; opened; opened = opened->next) {
            struct stat previous;
            if(fstat(dirfd(opened->data), &previous) == 0 && previous.st_dev == directory_info.st_dev
                    && previous.st_ino == directory_info.st_ino) {
                duplicate = 1;
                break;
            }
        }
        if(duplicate) {
            close(fd);
            continue;
        }
        DIR *directory = fdopendir(fd);
        if(directory == NULL) {
            close(fd);
            result->backend_error = ALPM_ERR_SYSTEM;
            goto done;
        }
        if(alpm_list_append(&directories, directory) == NULL) {
            closedir(directory);
            result->backend_error = ALPM_ERR_MEMORY;
            goto done;
        }
        struct dirent *file;
        for(;;) {
            errno = 0;
            file = readdir(directory);
            if(file == NULL) {
                if(errno != 0) {
                    result->backend_error = ALPM_ERR_SYSTEM;
                }
                break;
            }
            if(cancel != NULL && *cancel) {
                result->status = AXON_OPERATION_CANCELLED;
                result->cancelled = 1;
                break;
            }
            size_t length = strlen(file->d_name);
            if(strstr(file->d_name, ".pkg.tar") == NULL
                    || (length >= 4 && strcmp(file->d_name + length - 4, ".sig") == 0)
                    || (length >= 5 && strcmp(file->d_name + length - 5, ".part") == 0)) {
                continue;
            }
            int archive = openat(fd, file->d_name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
            struct stat info;
            if(archive < 0) {
                continue;
            }
            if(fstat(archive, &info) < 0 || !S_ISREG(info.st_mode)) {
                close(archive);
                continue;
            }
            char descriptor[64];
            snprintf(descriptor, sizeof(descriptor), "/proc/self/fd/%d", archive);
            alpm_pkg_t *package = NULL;
            int loaded = alpm_pkg_load(core->handle, descriptor, 0, 0, &package);
            close(archive);
            if(loaded < 0) {
                continue;
            }
            int keep = retain(core, package, options);
            alpm_pkg_free(package);
            if(keep) {
                continue;
            }
            if(candidate(&entries, &paths, fd, base, file->d_name, &info) < 0) {
                result->backend_error = errno == ENOMEM ? ALPM_ERR_MEMORY : ALPM_ERR_SYSTEM;
                break;
            }
            size_t size = strlen(file->d_name) + 5;
            char *signature = malloc(size);
            if(signature == NULL) {
                result->backend_error = ALPM_ERR_MEMORY;
                break;
            }
            snprintf(signature, size, "%s.sig", file->d_name);
            int added = candidate(&entries, &paths, fd, base, signature, NULL);
            free(signature);
            if(added < 0) {
                result->backend_error = errno == ENOMEM ? ALPM_ERR_MEMORY : ALPM_ERR_SYSTEM;
                break;
            }
        }
        if(result->backend_error != ALPM_ERR_OK || result->cancelled) {
            goto done;
        }
    }
    if((cancel != NULL && *cancel) || !approve(context, paths)) {
        result->status = AXON_OPERATION_CANCELLED;
        result->cancelled = 1;
        goto done;
    }
    const alpm_list_t *path = paths;
    for(alpm_list_t *item = entries; item; item = item->next, path = path->next) {
        candidate_t *entry = item->data;
        struct stat info;
        if(cancel != NULL && *cancel) {
            result->status = AXON_OPERATION_CANCELLED;
            result->cancelled = 1;
            break;
        }
        result->privilege_error = axon_privilege_check(1);
        if(result->privilege_error != AXON_PRIVILEGE_OK) {
            result->status = AXON_OPERATION_PRIVILEGE;
            break;
        }
        if(fstatat(entry->directory, entry->name, &info, AT_SYMLINK_NOFOLLOW) < 0
                || !S_ISREG(info.st_mode) || info.st_dev != entry->identity.st_dev
                || info.st_ino != entry->identity.st_ino || info.st_size != entry->identity.st_size
                || info.st_mtim.tv_sec != entry->identity.st_mtim.tv_sec
                || info.st_mtim.tv_nsec != entry->identity.st_mtim.tv_nsec) {
            result->backend_error = ALPM_ERR_PKG_INVALID;
            break;
        }
        result->may_have_changes = 1;
        if(unlinkat(entry->directory, entry->name, 0) < 0) {
            result->backend_error = ALPM_ERR_SYSTEM;
            break;
        }
        changed(result, "Removed cache file", path->data);
        if(result->status != AXON_OPERATION_OK) {
            break;
        }
    }
done:
    for(alpm_list_t *item = entries; item; item = item->next) {
        candidate_t *entry = item->data;
        free(entry->name);
        free(entry);
    }
    alpm_list_free(entries);
    for(alpm_list_t *item = directories; item; item = item->next) {
        closedir(item->data);
    }
    alpm_list_free(directories);
    alpm_list_free_inner(paths, free);
    alpm_list_free(paths);
    return(finish(&core, result));
}
