#define _GNU_SOURCE
#include "../ui/output.h"
#include "query.h"
#include "alpm_extra.h"
#include "alpm_internal.h"
#include "../core/maintenance.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>

static int selected(axon_alpm_t *core, const axon_query_t *query, alpm_pkg_t *package)
{
    if(query->sync && query->search) {
        int usage;
        if(alpm_db_get_usage(alpm_pkg_get_db(package), &usage) < 0) {
            return(-1);
        }
        if(!(usage & ALPM_DB_USAGE_SEARCH)) {
            return(0);
        }
    }
    if(query->explicit_only && alpm_pkg_get_reason(package) != ALPM_PKG_REASON_EXPLICIT) {
        return(0);
    }
    if(query->dependencies && alpm_pkg_get_reason(package) != ALPM_PKG_REASON_DEPEND) {
        return(0);
    }
    if(query->foreign || query->native) {
        alpm_pkg_t *remote = NULL;
        alpm_errno_t error = axon_alpm_sync_package(core, NULL, alpm_pkg_get_name(package), &remote);
        if(error != ALPM_ERR_OK && error != ALPM_ERR_PKG_NOT_FOUND) {
            return(-1);
        }
        if((query->foreign && remote != NULL) || (query->native && remote == NULL)) {
            return(0);
        }
    }
    return(1);
}

static const char *value(const char *text)
{
    return(text != NULL ? text : "None");
}

static void message(void *context, const char *text)
{
    (void)context;
    axon_fprintf(stderr, "axon: %s\n", text);
}

static int show(axon_alpm_t *core, const axon_query_t *query, alpm_pkg_t *package)
{
    const char *name = alpm_pkg_get_name(package);
    const char *version = alpm_pkg_get_version(package);
    if(query->check) {
        return(axon_maintenance_files(core, package, query->check > 1, query->checksums,
                                      message, NULL) != ALPM_ERR_OK);
    }
    if(query->files) {
        alpm_filelist_t *files = alpm_pkg_get_files(package);
        if(files == NULL) {
            return(1);
        }
        for(size_t index = 0; index < files->count; index++) {
            axon_fprintf(stdout, "%s %s%s\n", name, alpm_option_get_root(core->handle), files->files[index].name);
        }
    }
    else if(query->info) {
        axon_fprintf(stdout, "Name           : %s\nVersion        : %s\nDescription    : %s\n"
               "Architecture   : %s\nURL            : %s\nInstalled size : %jd bytes\n",
               name, version, value(alpm_pkg_get_desc(package)), value(alpm_pkg_get_arch(package)),
               value(alpm_pkg_get_url(package)), (intmax_t)alpm_pkg_get_isize(package));
        if(query->sync) {
            axon_fprintf(stdout, "Repository     : %s\n", alpm_db_get_name(alpm_pkg_get_db(package)));
        }
        else {
            axon_fprintf(stdout, "Install reason : %s\n", alpm_pkg_get_reason(package) == ALPM_PKG_REASON_EXPLICIT
                ? "Explicitly installed" : "Installed as a dependency");
        }
        putchar('\n');
    }
    else if(query->search) {
        axon_fprintf(stdout, "%s/%s %s", query->sync ? alpm_db_get_name(alpm_pkg_get_db(package)) : "local", name, version);
        alpm_list_t *groups = alpm_pkg_get_groups(package);
        if(groups != NULL) {
            fputs(" (", stdout);
            for(alpm_list_t *group = groups; group; group = group->next) {
                axon_fprintf(stdout, "%s%s", group == groups ? "" : " ", (char *)group->data);
            }
            putchar(')');
        }
        if(query->sync) {
            alpm_pkg_t *installed = NULL;
            if(axon_alpm_local_package(core, name, &installed) == ALPM_ERR_OK) {
                if(strcmp(version, alpm_pkg_get_version(installed)) == 0) {
                    fputs(" [installed]", stdout);
                }
                else {
                    axon_fprintf(stdout, " [installed: %s]", alpm_pkg_get_version(installed));
                }
            }
        }
        axon_fprintf(stdout, "\n    %s\n", value(alpm_pkg_get_desc(package)));
    }
    else {
        axon_fprintf(stdout, "%s %s\n", name, version);
    }
    return(0);
}

static int ownership(axon_alpm_t *core, int count, char **targets)
{
    const alpm_list_t *packages = NULL;
    if(axon_alpm_local_packages(core, &packages) != ALPM_ERR_OK) {
        return(1);
    }
    int status = 0;
    const char *root = alpm_option_get_root(core->handle);
    for(int index = 0; index < count; index++) {
        char *copy = strdup(targets[index]);
        if(copy == NULL) {
            return(1);
        }
        size_t length = strlen(copy);
        while(length > 1 && copy[length - 1] == '/') {
            copy[--length] = '\0';
        }
        char *slash = strrchr(copy, '/');
        const char *name = slash != NULL ? slash + 1 : copy;
        char *directory;
        if(slash != NULL) {
            *slash = '\0';
            directory = realpath(copy[0] ? copy : "/", NULL);
        }
        else {
            directory = realpath(".", NULL);
        }
        char *absolute = NULL;
        if(directory != NULL && asprintf(&absolute, "%s%s%s", directory,
                strcmp(directory, "/") == 0 ? "" : "/", name) < 0) {
            absolute = NULL;
        }
        int found = 0;
        if(absolute != NULL && strncmp(absolute, root, strlen(root)) == 0) {
            const char *relative = absolute + strlen(root);
            for(const alpm_list_t *item = packages; item; item = item->next) {
                alpm_pkg_t *package = item->data;
                alpm_filelist_t *files = alpm_pkg_get_files(package);
                if(files == NULL) {
                    status = 1;
                    continue;
                }
                for(size_t file = 0; file < files->count; file++) {
                    const char *record = files->files[file].name;
                    size_t size = strlen(record);
                    if(size && record[size - 1] == '/') {
                        size--;
                    }
                    if(strlen(relative) == size && strncmp(relative, record, size) == 0) {
                        axon_fprintf(stdout, "%s is owned by %s %s\n", absolute,
                               alpm_pkg_get_name(package), alpm_pkg_get_version(package));
                        found = 1;
                        break;
                    }
                }
            }
        }
        if(!found) {
            axon_fprintf(stderr, "axon: no package owns %s\n", targets[index]);
            status = 1;
        }
        free(directory);
        free(absolute);
        free(copy);
    }
    return(status);
}

int axon_cli_query(axon_alpm_t *core, const axon_query_t *query, int count, char **targets)
{
    if(query->check && axon_maintenance_records(core, 0, message, NULL) != ALPM_ERR_OK) {
        return(1);
    }
    if(query->owns) {
        return(ownership(core, count, targets));
    }
    alpm_list_t *patterns = NULL, *packages = NULL;
    if(query->search) {
        for(int index = 0; index < count; index++) {
            if(alpm_list_append(&patterns, targets[index]) == NULL) {
                alpm_list_free(patterns);
                return(1);
            }
        }
    }
    alpm_errno_t error = axon_alpm_search(core, query->sync, patterns, &packages);
    alpm_list_free(patterns);
    if(error != ALPM_ERR_OK) {
        axon_fprintf(stderr, "axon: %s search failed: %s\n", query->sync ? "repository" : "local", alpm_strerror(error));
        return(1);
    }
    int status = 0;
    int passes = !query->search && count ? count : 1;
    alpm_list_t *seen_groups = NULL;
    for(int index = 0; index < passes; index++) {
        int found = 0;
        for(alpm_list_t *item = packages; item; item = item->next) {
            alpm_pkg_t *package = item->data;
            int match = selected(core, query, package);
            if(match < 0) {
                status = 1;
                continue;
            }
            if(!match) {
                continue;
            }
            if(query->groups) {
                for(alpm_list_t *group = alpm_pkg_get_groups(package); group; group = group->next) {
                    if(count && strcmp(group->data, targets[index]) != 0) {
                        continue;
                    }
                    found = 1;
                    if(query->sync && !count && query->groups == 1) {
                        if(alpm_list_find_str(seen_groups, group->data) == NULL) {
                            axon_fprintf(stdout, "%s\n", (char *)group->data);
                            if(alpm_list_append(&seen_groups, group->data) == NULL) {
                                status = 1;
                            }
                        }
                    }
                    else {
                        axon_fprintf(stdout, "%s %s\n", (char *)group->data, alpm_pkg_get_name(package));
                    }
                }
                continue;
            }
            if(!query->search && count) {
                const char *name = targets[index];
                const char *slash = query->sync ? strchr(name, '/') : NULL;
                if(slash != NULL) {
                    const char *repository = alpm_db_get_name(alpm_pkg_get_db(package));
                    if(strlen(repository) != (size_t)(slash - name) || strncmp(repository, name, (size_t)(slash - name)) != 0) {
                        continue;
                    }
                    name = slash + 1;
                }
                if(strcmp(name, alpm_pkg_get_name(package)) != 0) {
                    continue;
                }
            }
            found = 1;
            status |= show(core, query, package);
            if(query->sync && query->info && count) {
                break;
            }
        }
        if(!found && count && !query->search) {
            axon_fprintf(stderr, "axon: %s: package or group not found\n", targets[index]);
            status = 1;
        }
    }
    alpm_list_free(seen_groups);
    alpm_list_free(packages);
    return(status);
}
