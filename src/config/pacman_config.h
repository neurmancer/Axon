#ifndef AXON_PACMAN_CONFIG
#   define AXON_PACMAN_CONFIG

#include "alpm_core.h"

#include <stddef.h>

typedef struct axon_config axon_config_t;

typedef enum {
    AXON_CONFIG_OK,
    AXON_CONFIG_ARGUMENT,
    AXON_CONFIG_MEMORY,
    AXON_CONFIG_IO,
    AXON_CONFIG_SYNTAX,
    AXON_CONFIG_VALUE,
    AXON_CONFIG_UNKNOWN,
    AXON_CONFIG_LIMIT,
    AXON_CONFIG_UNSUPPORTED,
    AXON_CONFIG_ALPM
} axon_config_result_t;

typedef struct {
    axon_config_result_t code;
    char file[4096];
    size_t line;
    char message[512];
    int system_error;
    alpm_errno_t alpm_error;
} axon_config_error_t;

typedef struct {
    char *root_dir;
    char *db_path;
    char *log_file;
    char *gpg_dir;
    char *download_user;
    char *xfer_command;
    alpm_list_t *cache_dirs;
    alpm_list_t *hook_dirs;
    alpm_list_t *architectures;
    alpm_list_t *hold_packages;
    alpm_list_t *ignore_packages;
    alpm_list_t *ignore_groups;
    alpm_list_t *no_upgrade;
    alpm_list_t *no_extract;
    unsigned int parallel_downloads;
    int siglevel;
    int local_file_siglevel;
    int remote_file_siglevel;
    int clean_keep_installed;
    int clean_keep_current;
    int use_syslog;
    int check_space;
    int disable_download_timeout;
    int disable_sandbox;
    int disable_sandbox_filesystem;
    int disable_sandbox_syscalls;
    int color;
    int no_progress_bar;
    int verbose_package_lists;
    int candy;
} axon_config_options_t;

typedef struct {
    char *name;
    alpm_list_t *servers;
    alpm_list_t *cache_servers;
    int siglevel;
    int usage;
} axon_config_repo_t;

axon_config_result_t axon_config_load(const char *path, axon_config_t **config,
                                     axon_config_error_t *error);
void axon_config_free(axon_config_t **config);
const axon_config_options_t *axon_config_options(const axon_config_t *config);
const alpm_list_t *axon_config_repositories(const axon_config_t *config);
axon_config_result_t axon_config_open(const axon_config_t *config,
                                     axon_alpm_t **core, axon_config_error_t *error);
axon_config_result_t axon_config_open_readonly(const axon_config_t *config,
                                     axon_alpm_t **core, axon_config_error_t *error);

#endif
