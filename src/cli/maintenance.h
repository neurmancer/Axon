#ifndef AXON_CLI_MAINTENANCE
#define AXON_CLI_MAINTENANCE
#include "pacman_config.h"

enum { AXON_ORPHANS = 300, AXON_DEPENDENCIES, AXON_REASON_EXPLICIT,
       AXON_REASON_DEPENDENCY, AXON_CLEAN_CACHE, AXON_CHECK_DATABASE, AXON_UI };
int axon_cli_maintenance(const axon_config_t *config, int action, int count, char **names);
#endif
