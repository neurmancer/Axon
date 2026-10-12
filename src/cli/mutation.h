#ifndef AXON_CLI_MUTATION
#define AXON_CLI_MUTATION
#include "operation.h"

int axon_cli_mutate(const axon_config_t *config, axon_operation_kind_t kind,
                    int count, char **names, int refresh, int refresh_only,
                    int allow_downgrade, int flags);
#endif
