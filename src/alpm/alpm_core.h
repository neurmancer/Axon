#ifndef AXON_ALPM_CORE
#   define AXON_ALPM_CORE

#include <alpm.h>

typedef struct axon_alpm axon_alpm_t;

alpm_errno_t axon_alpm_init(axon_alpm_t **core, const char *root,
                          const char *dbpath);
int axon_alpm_release(axon_alpm_t **core);
alpm_errno_t axon_alpm_local_packages(axon_alpm_t *core,
                                    const alpm_list_t **packages);
alpm_errno_t axon_alpm_local_package(axon_alpm_t *core, const char *name,
                                   alpm_pkg_t **package);

#endif
