#define _GNU_SOURCE
#include "config_internal.h"
#include "alpm_extra.h"
#include "alpm_internal.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

axon_config_result_t axon_config_open_readonly(const axon_config_t *config,
                                     axon_alpm_t **core, axon_config_error_t *error)
{
    if(config == NULL || core == NULL || *core != NULL) {
        return(axon_config_fail(config, NULL, error, AXON_CONFIG_ARGUMENT,
                                "pass a parsed configuration and an initially NULL context"));
    }
    int directory = open(config->options.db_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int version = directory < 0 ? -1 : openat(directory, "local/ALPM_DB_VERSION", O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    struct stat info;
    int valid = version >= 0 && fstat(version, &info) == 0 && S_ISREG(info.st_mode);
    if(version >= 0) {
        close(version);
    }
    if(directory >= 0) {
        close(directory);
    }
    if(!valid) {
        return(axon_config_fail(config, &config->options.db_path, error, AXON_CONFIG_IO,
            "database or local/ALPM_DB_VERSION is missing or unreadable; inspection will not initialize or repair it"));
    }
    return(axon_config_open(config, core, error));
}

static axon_config_result_t backend_error(const axon_config_t *config,
                                          const void *target, axon_config_error_t *error,
                                          alpm_errno_t code, const char *operation)
{
    char message[512];
    snprintf(message, sizeof(message), "%s: %s", operation, alpm_strerror(code));
    axon_config_result_t result = axon_config_fail(config, target, error,
                                                  AXON_CONFIG_ALPM, message);
    if(error != NULL) {
        error->alpm_error = code;
    }
    return(result);
}

axon_config_result_t axon_config_open(const axon_config_t *config,
                                     axon_alpm_t **core, axon_config_error_t *error)
{
    if(error != NULL) {
        memset(error, 0, sizeof(*error));
    }
    if(config == NULL || core == NULL || *core != NULL) {
        return(axon_config_fail(config, NULL, error, AXON_CONFIG_ARGUMENT,
                                "pass a parsed configuration and an initially NULL context"));
    }

    const axon_config_options_t *options = &config->options;
    if(options->xfer_command != NULL) {
        return(axon_config_fail(config, &options->xfer_command, error,
                                AXON_CONFIG_UNSUPPORTED,
                                "XferCommand is parsed but external command downloads are not implemented"));
    }

    axon_alpm_t *opened = NULL;
    alpm_errno_t code = axon_alpm_init(&opened, options->root_dir, options->db_path);
    const void *target = &options->db_path;
    const char *operation = "initialize root/database";
    if(code != ALPM_ERR_OK) {
        struct stat root_info;
        if(stat(options->root_dir, &root_info) < 0 || !S_ISDIR(root_info.st_mode)) {
            target = &options->root_dir;
        }
        return(backend_error(config, target, error, code, operation));
    }

    struct { axon_alpm_option_t option; char *const *value; } strings[] = {
        {AXON_ALPM_GPG_DIR, &options->gpg_dir},
        {AXON_ALPM_LOG_FILE, &options->log_file},
        {AXON_ALPM_SANDBOX_USER, &options->download_user}
    };
    for(size_t index = 0; index < sizeof(strings) / sizeof(strings[0]); index++) {
        if(*strings[index].value != NULL) {
            target = strings[index].value;
            operation = "configure path or download user";
            code = axon_alpm_option(opened, strings[index].option, *strings[index].value);
            if(code != ALPM_ERR_OK) {
                goto fail;
            }
        }
    }

    struct { axon_alpm_option_t option; const alpm_list_t *values; } lists[] = {
        {AXON_ALPM_CACHE_DIR, options->cache_dirs},
        {AXON_ALPM_HOOK_DIR, options->hook_dirs},
        {AXON_ALPM_ARCHITECTURE, options->architectures},
        {AXON_ALPM_IGNORE_PACKAGE, options->ignore_packages},
        {AXON_ALPM_IGNORE_GROUP, options->ignore_groups},
        {AXON_ALPM_NO_UPGRADE, options->no_upgrade},
        {AXON_ALPM_NO_EXTRACT, options->no_extract}
    };
    for(size_t index = 0; index < sizeof(lists) / sizeof(lists[0]); index++) {
        for(const alpm_list_t *item = lists[index].values; item; item = item->next) {
            target = item;
            operation = "configure list option";
            code = axon_alpm_option(opened, lists[index].option, item->data);
            if(code != ALPM_ERR_OK) {
                goto fail;
            }
        }
    }

    struct { int (*set)(alpm_handle_t *, int); const int *value; } integers[] = {
        {alpm_option_set_default_siglevel, &options->siglevel},
        {alpm_option_set_local_file_siglevel, &options->local_file_siglevel},
        {alpm_option_set_remote_file_siglevel, &options->remote_file_siglevel},
        {alpm_option_set_checkspace, &options->check_space},
        {alpm_option_set_usesyslog, &options->use_syslog}
    };
    for(size_t index = 0; index < sizeof(integers) / sizeof(integers[0]); index++) {
        target = integers[index].value;
        operation = "configure signature or boolean option";
        if(integers[index].set(opened->handle, *integers[index].value) < 0) {
            code = alpm_errno(opened->handle);
            goto fail;
        }
    }

    target = &options->parallel_downloads;
    operation = "configure parallel downloads";
    code = axon_alpm_downloads(opened, options->parallel_downloads);
    if(code != ALPM_ERR_OK) {
        goto fail;
    }

    struct { int (*set)(alpm_handle_t *, unsigned short); const int *value; } toggles[] = {
        {alpm_option_set_disable_dl_timeout, &options->disable_download_timeout},
        {alpm_option_set_disable_sandbox_filesystem, &options->disable_sandbox_filesystem},
        {alpm_option_set_disable_sandbox_syscalls, &options->disable_sandbox_syscalls}
    };
    for(size_t index = 0; index < sizeof(toggles) / sizeof(toggles[0]); index++) {
        target = toggles[index].value;
        operation = "configure download timeout or sandbox";
        int enabled = *toggles[index].value;
        if(index >= 1 && options->disable_sandbox) {
            enabled = 1;
        }
        if(toggles[index].set(opened->handle, (unsigned short)enabled) < 0) {
            code = alpm_errno(opened->handle);
            goto fail;
        }
    }

    for(const alpm_list_t *item = config->repositories; item; item = item->next) {
        const axon_config_repo_t *repo = item->data;
        target = repo;
        operation = "register repository";
        code = axon_alpm_repo_add(opened, repo->name, repo->siglevel, repo->servers, repo->usage);
        if(code != ALPM_ERR_OK) {
            goto fail;
        }
        alpm_db_t *database = alpm_list_last(alpm_get_syncdbs(opened->handle))->data;
        for(const alpm_list_t *server = repo->cache_servers; server; server = server->next) {
            target = server;
            operation = "configure cache server";
            if(alpm_db_add_cache_server(database, server->data) < 0) {
                code = alpm_errno(opened->handle);
                goto fail;
            }
        }
    }

    *core = opened;
    return(AXON_CONFIG_OK);

fail:
    axon_alpm_release(&opened);
    return(backend_error(config, target, error, code, operation));
}
