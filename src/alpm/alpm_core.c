#include "alpm_internal.h"
#include "privilege.h"

#include <stdlib.h>

alpm_errno_t axon_alpm_init(axon_alpm_t **core, const char *root,
                          const char *dbpath)
{
    axon_alpm_t *context;
    alpm_errno_t error = ALPM_ERR_OK;

    if(core == NULL) {
        return(ALPM_ERR_WRONG_ARGS);
    }

    if(*core != NULL) {
        return(ALPM_ERR_HANDLE_NOT_NULL);
    }

    if(axon_privilege_check(0) != AXON_PRIVILEGE_OK) {
        return(ALPM_ERR_BADPERMS);
    }

    if(root == NULL) {
        root = "/";
    }

    if(dbpath == NULL) {
        dbpath = "/var/lib/pacman";
    }

    if(root[0] != '/' || dbpath[0] != '/') {
        return(ALPM_ERR_WRONG_ARGS);
    }

    context = calloc(1, sizeof(*context));
    if(context == NULL) {
        return(ALPM_ERR_MEMORY);
    }

    context->handle = alpm_initialize(root, dbpath, &error);
    if(context->handle == NULL) {
        free(context);
        return(error);
    }

    *core = context;
    return(ALPM_ERR_OK);
}

int axon_alpm_release(axon_alpm_t **core)
{
    int result;
    int transaction_result = 0;

    if(core == NULL) {
        return(-1);
    }

    if(*core == NULL) {
        return(0);
    }

    if((*core)->transaction != AXON_TRANS_IDLE) {
        transaction_result = alpm_trans_release((*core)->handle);
    }

    result = alpm_release((*core)->handle);
    free(*core);
    *core = NULL;
    return(result != 0 ? result : transaction_result);
}

alpm_errno_t axon_alpm_local_packages(axon_alpm_t *core,
                                    const alpm_list_t **packages)
{
    alpm_db_t *database;

    if(packages == NULL) {
        return(ALPM_ERR_WRONG_ARGS);
    }

    *packages = NULL;
    if(core == NULL) {
        return(ALPM_ERR_HANDLE_NULL);
    }

    database = alpm_get_localdb(core->handle);
    if(database == NULL) {
        return(alpm_errno(core->handle));
    }

    *packages = alpm_db_get_pkgcache(database);
    if(*packages == NULL) {
        return(alpm_errno(core->handle));
    }

    return(ALPM_ERR_OK);
}

alpm_errno_t axon_alpm_local_package(axon_alpm_t *core, const char *name,
                                   alpm_pkg_t **package)
{
    alpm_db_t *database;

    if(package == NULL) {
        return(ALPM_ERR_WRONG_ARGS);
    }

    *package = NULL;
    if(core == NULL) {
        return(ALPM_ERR_HANDLE_NULL);
    }

    if(name == NULL || name[0] == '\0') {
        return(ALPM_ERR_WRONG_ARGS);
    }

    database = alpm_get_localdb(core->handle);
    if(database == NULL) {
        return(alpm_errno(core->handle));
    }

    *package = alpm_db_get_pkg(database, name);
    if(*package == NULL) {
        return(alpm_errno(core->handle));
    }

    return(ALPM_ERR_OK);
}
