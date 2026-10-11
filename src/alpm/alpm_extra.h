#ifndef AXON_ALPM_EXTRA
#   define AXON_ALPM_EXTRA

#include "alpm_core.h"

typedef enum {
    AXON_ALPM_ARCHITECTURE,
    AXON_ALPM_CACHE_DIR,
    AXON_ALPM_HOOK_DIR,
    AXON_ALPM_GPG_DIR,
    AXON_ALPM_LOG_FILE,
    AXON_ALPM_IGNORE_PACKAGE,
    AXON_ALPM_IGNORE_GROUP,
    AXON_ALPM_NO_UPGRADE,
    AXON_ALPM_NO_EXTRACT,
    AXON_ALPM_SANDBOX_USER
} axon_alpm_option_t;

typedef struct {
    alpm_cb_log log;
    alpm_cb_event event;
    alpm_cb_question question;
    alpm_cb_progress progress;
    alpm_cb_download download;
    alpm_cb_fetch fetch;
    void *context;
} axon_alpm_callbacks_t;

typedef struct {
    alpm_errno_t code;
    alpm_list_t *details;
} axon_alpm_problem_t;

alpm_errno_t axon_alpm_option(axon_alpm_t *core, axon_alpm_option_t option,
                            const char *value);
alpm_errno_t axon_alpm_signatures(axon_alpm_t *core, int defaults,
                                int local_files, int remote_files);
alpm_errno_t axon_alpm_downloads(axon_alpm_t *core, unsigned int parallel);
alpm_errno_t axon_alpm_check_space(axon_alpm_t *core, int enabled);
alpm_errno_t axon_alpm_callbacks(axon_alpm_t *core,
                               const axon_alpm_callbacks_t *callbacks);

alpm_errno_t axon_alpm_repo_add(axon_alpm_t *core, const char *name,
                              int siglevel, const alpm_list_t *servers, int usage);
alpm_errno_t axon_alpm_repo_remove(axon_alpm_t *core, const char *name);
alpm_errno_t axon_alpm_repositories(axon_alpm_t *core,
                                  const alpm_list_t **repositories);
alpm_errno_t axon_alpm_refresh(axon_alpm_t *core, int force, int *changed);
alpm_errno_t axon_alpm_sync_package(axon_alpm_t *core, const char *repository,
                                  const char *name, alpm_pkg_t **package);
alpm_errno_t axon_alpm_search(axon_alpm_t *core, int sync,
                            const alpm_list_t *patterns, alpm_list_t **packages);
alpm_errno_t axon_alpm_upgrades(axon_alpm_t *core, alpm_list_t **packages);
alpm_errno_t axon_alpm_load(axon_alpm_t *core, const char *path,
                          alpm_pkg_t **package);
alpm_errno_t axon_alpm_set_reason(axon_alpm_t *core, const char *name,
                                alpm_pkgreason_t reason);

alpm_errno_t axon_alpm_transaction_begin(axon_alpm_t *core, int flags);
alpm_errno_t axon_alpm_transaction_install(axon_alpm_t *core,
                                         const char *repository, const char *name);
alpm_errno_t axon_alpm_transaction_file(axon_alpm_t *core, const char *path);
alpm_errno_t axon_alpm_transaction_package(axon_alpm_t *core, alpm_pkg_t *package);
alpm_errno_t axon_alpm_transaction_remove(axon_alpm_t *core, const char *name);
alpm_errno_t axon_alpm_transaction_upgrade(axon_alpm_t *core, int downgrade);
alpm_errno_t axon_alpm_transaction_prepare(axon_alpm_t *core,
                                         axon_alpm_problem_t *problem);
alpm_errno_t axon_alpm_transaction_plan(axon_alpm_t *core,
                                      const alpm_list_t **add,
                                      const alpm_list_t **remove);
alpm_errno_t axon_alpm_transaction_commit(axon_alpm_t *core,
                                        axon_alpm_problem_t *problem);
alpm_errno_t axon_alpm_transaction_interrupt(axon_alpm_t *core);
alpm_errno_t axon_alpm_transaction_release(axon_alpm_t *core);
void axon_alpm_problem_clear(axon_alpm_problem_t *problem);

#endif
