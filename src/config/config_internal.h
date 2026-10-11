#ifndef AXON_CONFIG_INTERNAL
#   define AXON_CONFIG_INTERNAL

#include "pacman_config.h"

struct axon_config_source {
    const void *target;
    char *file;
    size_t line;
    struct axon_config_source *next;
};

struct axon_config {
    axon_config_options_t options;
    alpm_list_t *repositories;
    struct axon_config_source *sources;
    char *path;
    int local_mask;
    int remote_mask;
    size_t source_count;
};

axon_config_result_t axon_config_fail(const axon_config_t *config,
                                     const void *target, axon_config_error_t *error,
                                     axon_config_result_t code, const char *message);

#endif
