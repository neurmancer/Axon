#ifndef AXON_CLI_QUERY
#define AXON_CLI_QUERY
#include "alpm_core.h"

typedef struct {
    int sync, info, search, files, owns, groups;
    int foreign, native, explicit_only, dependencies, check, checksums;
} axon_query_t;
int axon_cli_query(axon_alpm_t *core, const axon_query_t *query, int count, char **targets);
#endif
