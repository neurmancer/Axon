#define _POSIX_C_SOURCE 200809L

#include "alpm_extra.h"
#include "pacman_config.h"
#include "privilege.h"
#include "mutation.h"
#include "maintenance.h"
#include "../ui/terminal.h"
#include "../ui/theme.h"

#include <getopt.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

static void usage(FILE *output)
{
    fputs("Usage: axon [--config PATH] -Q\n"
          "       axon [--config PATH] -Qi PACKAGE...\n"
          "       axon [--config PATH] -Ss PATTERN...\n"
          "       axon [--config PATH] -S PACKAGE...\n"
          "       axon [--config PATH] -U ARCHIVE...\n"
          "       axon [--config PATH] -R PACKAGE...\n"
          "       axon [--config PATH] -Sy | -Su | -Syu\n"
          "       axon --help\n\n"
          "  -Q, --query    List installed packages\n"
          "  -i, --info     Show installed-package details with -Q\n"
          "  -S, --sync     Select repository packages or queries\n"
          "  -s, --search   Search cached repositories with -S (regular expressions)\n"
          "  --config PATH Use PATH instead of /etc/pacman.conf\n"
          "  -U, --upgrade Install local package archives\n"
          "  -R, --remove  Remove installed packages\n"
          "  -y, --refresh Refresh repositories with -S (twice forces refresh)\n"
          "  -u, --sysupgrade Full system upgrade with -S\n"
          "  --allow-downgrade Permit a downgrade plan (confirmation still required)\n"
          "  --orphans       List unneeded dependency packages\n"
          "  --depends PKG... Inspect installed dependencies\n"
          "  --asexplicit PKG... Mark packages explicitly installed\n"
          "  --asdeps PKG... Mark packages installed as dependencies\n"
          "  --clean-cache   Review cache removal using CleanMethod\n"
          "  --check-db      Inspect database and recovery diagnostics\n"
          "  --ui            Open the interactive package browser\n"
          "Mutations require root and explicit confirmation.\n", output);
}

static const char *value(const char *text)
{
    return(text != NULL ? text : "None");
}

static int installed(axon_alpm_t *core)
{
    const alpm_list_t *packages = NULL;
    alpm_errno_t error = axon_alpm_local_packages(core, &packages);
    if(error != ALPM_ERR_OK) {
        fprintf(stderr, "axon: cannot list installed packages: %s\n", alpm_strerror(error));
        return(1);
    }

    for(const alpm_list_t *item = packages; item; item = item->next) {
        alpm_pkg_t *package = item->data;
        printf("%s%s %s%s\n", axon_color(stdout, AXON_BLUE), alpm_pkg_get_name(package),
               alpm_pkg_get_version(package), axon_color(stdout, AXON_RESET));
    }
    return(0);
}

static int information(axon_alpm_t *core, int count, char **names)
{
    int result = 0;
    for(int index = 0; index < count; index++) {
        alpm_pkg_t *package = NULL;
        alpm_errno_t error = axon_alpm_local_package(core, names[index], &package);
        if(error != ALPM_ERR_OK) {
            fprintf(stderr, "axon: %s: %s\n", names[index], alpm_strerror(error));
            result = 1;
            continue;
        }

        fputs(axon_color(stdout, AXON_BLUE), stdout);
        printf("Name           : %s\n"
               "Version        : %s\n"
               "Description    : %s\n"
               "Architecture   : %s\n"
               "URL            : %s\n"
               "Installed size : %jd bytes\n"
               "Install reason : %s\n\n",
               alpm_pkg_get_name(package), alpm_pkg_get_version(package),
               value(alpm_pkg_get_desc(package)), value(alpm_pkg_get_arch(package)),
               value(alpm_pkg_get_url(package)), (intmax_t)alpm_pkg_get_isize(package),
               alpm_pkg_get_reason(package) == ALPM_PKG_REASON_EXPLICIT
                   ? "Explicitly installed" : "Installed as a dependency");
        fputs(axon_color(stdout, AXON_RESET), stdout);
    }
    return(result);
}

static int search(axon_alpm_t *core, int count, char **terms)
{
    alpm_list_t *patterns = NULL;
    alpm_list_t *packages = NULL;
    for(int index = 0; index < count; index++) {
        if(alpm_list_append(&patterns, terms[index]) == NULL) {
            alpm_list_free(patterns);
            fputs("axon: out of memory\n", stderr);
            return(1);
        }
    }

    alpm_errno_t error = axon_alpm_search(core, 1, patterns, &packages);
    
    alpm_list_free(patterns);
    
    if(error != ALPM_ERR_OK) {
        fprintf(stderr, "axon: repository search failed: %s\n", alpm_strerror(error));
        return(1);
    }

    for(alpm_list_t *item = packages; item; item = item->next) {
        alpm_pkg_t *package = item->data;
        printf("%s%s/%s %s%s\n    %s%s%s\n", axon_color(stdout, AXON_PURPLE), alpm_db_get_name(alpm_pkg_get_db(package)),
               alpm_pkg_get_name(package), alpm_pkg_get_version(package),
               axon_color(stdout, AXON_RESET), axon_color(stdout, AXON_BLUE),
               value(alpm_pkg_get_desc(package)), axon_color(stdout, AXON_RESET));
    }
    
    alpm_list_free(packages);
    
    return(0);
}

int main(int argc, char **argv)
{
    axon_privilege_result_t identity = axon_privilege_check(0);
    
    if(identity != AXON_PRIVILEGE_OK) {
        fprintf(stderr, "axon: %s\n", axon_privilege_message(identity));
        return(1);
    }

    const char *path = "/etc/pacman.conf";
    
    int query = 0;
    int sync = 0;
    int info = 0;
    int searching = 0;
    int help = 0;
    int remove = 0, local = 0, refresh = 0, upgrade = 0, downgrade = 0;
    int maintenance = 0;
    int option;
    
    static const struct option options[] = {
        {"config", required_argument, NULL, 256},
        {"query", no_argument, NULL, 'Q'},
        {"sync", no_argument, NULL, 'S'},
        {"info", no_argument, NULL, 'i'},
        {"search", no_argument, NULL, 's'},
        {"remove", no_argument, NULL, 'R'},
        {"upgrade", no_argument, NULL, 'U'},
        {"refresh", no_argument, NULL, 'y'},
        {"sysupgrade", no_argument, NULL, 'u'},
        {"allow-downgrade", no_argument, NULL, 257},
        {"orphans", no_argument, NULL, AXON_ORPHANS},
        {"depends", no_argument, NULL, AXON_DEPENDENCIES},
        {"asexplicit", no_argument, NULL, AXON_REASON_EXPLICIT},
        {"asdeps", no_argument, NULL, AXON_REASON_DEPENDENCY},
        {"clean-cache", no_argument, NULL, AXON_CLEAN_CACHE},
        {"check-db", no_argument, NULL, AXON_CHECK_DATABASE},
        {"ui", no_argument, NULL, AXON_UI},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    opterr = 0;
    
    while((option = getopt_long(argc, argv, "QSishRUyu", options, NULL)) != -1) {
        switch(option) {
            case 256:
                path = optarg;
                break;
            case 'Q':
                query = 1;
                break;
            case 'S':
                sync = 1;
                break;
            case 'i':
                info = 1;
                break;
            case 's':
                searching = 1;
                break;
            case 'R': remove = 1; break;
            case 'U': local = 1; break;
            case 'y': refresh++; break;
            case 'u': upgrade++; break;
            case 257: downgrade = 1; break;
            case AXON_ORPHANS:
            case AXON_DEPENDENCIES:
            case AXON_REASON_EXPLICIT:
            case AXON_REASON_DEPENDENCY:
            case AXON_CLEAN_CACHE:
            case AXON_CHECK_DATABASE:
            case AXON_UI:
                if(maintenance && maintenance != option) {
                    fputs("axon: choose one maintenance operation\n", stderr);
                    return(2);
                }
                maintenance = option;
                break;
            case 'h':
                help = 1;
                break;
            default:
                fputs("axon: unknown option or missing option value\n", stderr);
                usage(stderr);
                return(2);
        }
    }

    if(help) {
        usage(stdout);
        return(fflush(stdout) == 0 ? 0 : 1);
    }

    int count = argc - optind;
    
    int mutation = remove || local || (sync && !searching);
    if(maintenance) {
        int targets = maintenance == AXON_DEPENDENCIES || maintenance == AXON_REASON_EXPLICIT
            || maintenance == AXON_REASON_DEPENDENCY;
        if(query || sync || remove || local || info || searching || refresh || upgrade || downgrade
                || path[0] == '\0' || (targets ? count == 0 : count != 0)) {
            fputs("axon: maintenance operations cannot be combined; --depends, --asexplicit and --asdeps require package names\n", stderr);
            return(2);
        }
        mutation = maintenance == AXON_REASON_EXPLICIT || maintenance == AXON_REASON_DEPENDENCY
            || maintenance == AXON_CLEAN_CACHE;
    }

    else if(path[0] == '\0' || query + sync + remove + local != 1
            || (info && !query) || (searching && !sync)
            || ((refresh || upgrade) && (!sync || searching)) || refresh > 2 || upgrade > 1
            || (query && ((info && !count) || (!info && count)))
            || (searching && !count) || ((remove || local) && !count)
            || (sync && !searching && ((refresh || upgrade) ? count != 0 : count == 0))
            || (downgrade && (!mutation || remove || (refresh && !upgrade)))) {
        fputs("axon: choose -Q, -Qi, -Ss, -S PACKAGE..., -U ARCHIVE..., -R PACKAGE..., -Sy, -Su, or -Syu\n", stderr);
        usage(stderr);
        return(2);
    }
    if(mutation && (identity = axon_privilege_check(1)) != AXON_PRIVILEGE_OK) {
        fprintf(stderr, "axon: %s\n", axon_privilege_message(identity));
        return(1);
    }

    axon_config_t *config = NULL;
    axon_alpm_t *core = NULL;
    axon_config_error_t error;
    
    int result = 1;
    
    if(axon_config_load(path, &config, &error) != AXON_CONFIG_OK) {
        fprintf(stderr, "axon: %s:%zu: %s\n", error.file, error.line, error.message);
        goto cleanup;
    }

    if(maintenance) {
        result = maintenance == AXON_UI ? axon_terminal(config)
            : axon_cli_maintenance(config, maintenance, count, argv + optind);
        goto cleanup;
    }
    if(mutation) {
        axon_operation_kind_t kind = remove ? AXON_OPERATION_REMOVE : local ? AXON_OPERATION_FILE
            : upgrade ? AXON_OPERATION_UPGRADE : AXON_OPERATION_INSTALL;
        result = axon_cli_mutate(config, kind, count, argv + optind, refresh,
                                 refresh && !upgrade, downgrade);
        goto cleanup;
    }
    if(axon_config_open(config, &core, &error) != AXON_CONFIG_OK) {
        fprintf(stderr, "axon: %s:%zu: %s\n", error.file, error.line, error.message);
        goto cleanup;
    }

    if(info) {
        result = information(core, count, argv + optind);
    }

    else if(searching) {
        result = search(core, count, argv + optind);
    }

    else {
        result = installed(core);
    }

cleanup:
    if(axon_alpm_release(&core) != 0) {
        fputs("axon: failed to release package database\n", stderr);
        result = 1;
    }
    axon_config_free(&config);
    if(fflush(stdout) != 0 || ferror(stdout)) {
        fputs("axon: failed to write output\n", stderr);
        result = 1;
    }
    return(result);
}
