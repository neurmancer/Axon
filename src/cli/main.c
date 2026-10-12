#define _POSIX_C_SOURCE 200809L

#include "alpm_extra.h"
#include "pacman_config.h"
#include "privilege.h"
#include "mutation.h"
#include "query.h"
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
          "  -i, --info     Show package details with -Q or -S\n"
          "  -S, --sync     Select repository packages or queries\n"
          "  -s, --search   Search local (-Q) or cached repository (-S) packages\n"
          "  -l, --list     List installed package files (-Q)\n"
          "  -o, --owns     Find installed owners of paths (-Q)\n"
          "  -g, --groups   Inspect groups (-Q or -S); -S also installs groups\n"
          "  -m/-n         Filter foreign/native packages (-Q)\n"
          "  -e/-d         Filter explicit/dependency packages (-Q)\n"
          "  -k/-kk        Check installed file presence/metadata (-Q)\n"
          "  --checksums   Verify recorded hashes with -Qkk\n"
          "  --needed      Skip current versions with -S or -U\n"
          "  -Rs/-Rss      Recursively remove dependencies (twice includes explicit)\n"
          "  -Rc/-Rn/-Ru   Cascade / omit backups / remove only unneeded targets\n"
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
    axon_query_t view = {0};
    int needed = 0, cascade = 0, nosave = 0;
    
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
        {"list", no_argument, NULL, 'l'},
        {"owns", no_argument, NULL, 'o'},
        {"groups", no_argument, NULL, 'g'},
        {"foreign", no_argument, NULL, 'm'},
        {"native", no_argument, NULL, 'n'},
        {"explicit", no_argument, NULL, 'e'},
        {"deps", no_argument, NULL, 'd'},
        {"check", no_argument, NULL, 'k'},
        {"checksums", no_argument, NULL, 259},
        {"needed", no_argument, NULL, 258},
        {"cascade", no_argument, NULL, 'c'},
        {"recursive", no_argument, NULL, 's'},
        {"nosave", no_argument, NULL, 'n'},
        {"unneeded", no_argument, NULL, 'u'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    opterr = 0;
    
    while((option = getopt_long(argc, argv, "QSishRUyulogmnedkc", options, NULL)) != -1) {
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
                searching++;
                break;
            case 'l': view.files = 1; break;
            case 'o': view.owns = 1; break;
            case 'g': view.groups++; break;
            case 'm': view.foreign = 1; break;
            case 'n': view.native = 1; break;
            case 'e': view.explicit_only = 1; break;
            case 'd': view.dependencies = 1; break;
            case 'k': view.check++; break;
            case 'c': cascade = 1; break;
            case 258: needed = 1; break;
            case 259: view.checksums = 1; break;
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
    
    if(remove) {
        nosave = view.native;
        view.native = 0;
    }
    view.sync = sync;
    view.info = info;
    view.search = !remove && searching;
    int modifiers = info + !!view.search + view.files + view.owns + !!view.groups + !!view.check;
    int filters = view.foreign || view.native || view.explicit_only || view.dependencies;
    int mutation = remove || local || (sync && modifiers == 0);
    int invalid = path[0] == '\0';
    if(maintenance) {
        int targets = maintenance == AXON_DEPENDENCIES || maintenance == AXON_REASON_EXPLICIT
            || maintenance == AXON_REASON_DEPENDENCY;
        invalid |= query || sync || remove || local || modifiers || filters || refresh || upgrade
            || downgrade || needed || cascade || view.checksums || (targets ? count == 0 : count != 0);
        mutation = maintenance == AXON_REASON_EXPLICIT || maintenance == AXON_REASON_DEPENDENCY
            || maintenance == AXON_CLEAN_CACHE;
    }
    else {
        invalid |= query + sync + remove + local != 1 || modifiers > 1;
        invalid |= (filters && !query) || (view.foreign && view.native)
            || (view.explicit_only && view.dependencies);
        invalid |= (view.files || view.owns || view.check) && !query;
        invalid |= view.owns && (count == 0 || filters);
        invalid |= (info || view.search || view.groups) && !(query || sync);
        invalid |= view.check > 2 || view.groups > 2 || (view.checksums && view.check != 2);
        invalid |= refresh > 2 || upgrade > 1 || (refresh && (!sync || modifiers))
            || (upgrade && !(remove || (sync && !modifiers)));
        invalid |= (remove || local || (sync && !modifiers && !refresh && !upgrade)) && !count;
        invalid |= sync && !modifiers && (refresh || upgrade) && count != 0;
        invalid |= downgrade && (!mutation || remove || (refresh && !upgrade));
        invalid |= needed && (!mutation || remove || (refresh && !upgrade));
        invalid |= cascade && !remove;
        invalid |= searching > (remove ? 2 : 1);
    }
    if(invalid) {
        fputs("axon: incompatible operation, options, or targets\n", stderr);
        usage(stderr);
        return(2);
    }
    int flags = needed ? ALPM_TRANS_FLAG_NEEDED : 0;
    if(remove) {
        flags |= searching ? ALPM_TRANS_FLAG_RECURSE : 0;
        flags |= searching > 1 ? ALPM_TRANS_FLAG_RECURSEALL : 0;
        flags |= cascade ? ALPM_TRANS_FLAG_CASCADE : 0;
        flags |= nosave ? ALPM_TRANS_FLAG_NOSAVE : 0;
        flags |= upgrade ? ALPM_TRANS_FLAG_UNNEEDED : 0;
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
                                 refresh && !upgrade, downgrade, flags);
        goto cleanup;
    }
    if((view.check ? axon_config_open_readonly(config, &core, &error)
                   : axon_config_open(config, &core, &error)) != AXON_CONFIG_OK) {
        fprintf(stderr, "axon: %s:%zu: %s\n", error.file, error.line, error.message);
        goto cleanup;
    }

    result = axon_cli_query(core, &view, count, argv + optind);

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
