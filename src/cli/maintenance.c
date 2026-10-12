#define _POSIX_C_SOURCE 200809L
#include "../ui/output.h"
#include "maintenance.h"
#include "../core/maintenance.h"
#include "../ui/theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile sig_atomic_t interrupted;

static void interrupt(int number)
{
    interrupted = number;
}

static int confirmation(void)
{
    char line[16];
    fprintf(stderr, "%sProceed? [y/N]%s ", axon_color(stderr, AXON_PURPLE), axon_color(stderr, AXON_RESET));
    if(interrupted || fflush(stdout) != 0 || fflush(stderr) != 0 || ferror(stdout)
            || fgets(line, sizeof(line), stdin) == NULL) {
        return(0);
    }
    return(!interrupted && (strcmp(line, "y\n") == 0 || strcmp(line, "yes\n") == 0));
}

static int cache_approval(void *context, const alpm_list_t *paths)
{
    (void)context;
    if(paths == NULL) {
        puts("No cache files eligible for removal.");
        return(1);
    }
    puts("Remove these cache files according to CleanMethod:");
    for(const alpm_list_t *item = paths; item; item = item->next) {
        axon_fprintf(stdout, "  %s\n", (const char *)item->data);
    }
    return(confirmation());
}

static void message(void *context, const char *text)
{
    (void)context;
    axon_fprintf(stderr, "axon: %s\n", text);
}

static void dependencies(const char *heading, alpm_list_t *list)
{
    axon_fprintf(stdout, "%s:", heading);
    for(alpm_list_t *item = list; item; item = item->next) {
        char *dependency = alpm_dep_compute_string(item->data);
        axon_fprintf(stdout, " %s", dependency != NULL ? dependency : "(unavailable)");
        free(dependency);
    }
    putchar('\n');
}

static int maintenance(const axon_config_t *config, int action, int count, char **names)
{
    if(action == AXON_REASON_EXPLICIT || action == AXON_REASON_DEPENDENCY || action == AXON_CLEAN_CACHE) {
        axon_operation_result_t result = {0};
        if(action == AXON_CLEAN_CACHE) {
            axon_maintenance_clean(config, cache_approval, NULL, &interrupted, &result);
        }

        else {
            axon_fprintf(stdout, "Mark packages as %s:\n", action == AXON_REASON_EXPLICIT ? "explicitly installed" : "dependencies");
            for(int index = 0; index < count; index++) {
                axon_fprintf(stdout, "  %s\n", names[index]);
            }
            if(!confirmation()) {
                fputs("axon: cancelled\n", stderr);
                return(1);
            }
            axon_maintenance_reason(config, (size_t)count, (const char *const *)names,
                action == AXON_REASON_EXPLICIT ? ALPM_PKG_REASON_EXPLICIT : ALPM_PKG_REASON_DEPEND, &interrupted, &result);
        }
        for(alpm_list_t *item = result.diagnostics; item; item = item->next) {
            message(NULL, item->data);
        }
        int status = result.status == AXON_OPERATION_OK ? 0 : 1;
        if(status) {
            axon_fprintf(stderr, "axon: maintenance failed (status %d): %s\n", result.status,
                    result.backend_error != ALPM_ERR_OK ? alpm_strerror(result.backend_error) : "action not completed");
            if(result.status == AXON_OPERATION_CONFIG) {
                message(NULL, result.config_error.message);
            }
            if(result.cleanup_error != ALPM_ERR_OK) {
                axon_fprintf(stderr, "axon: cleanup: %s\n", alpm_strerror(result.cleanup_error));
            }
            if(result.may_have_changes) {
                message(NULL, "Some changes may have completed. Review the reported files/packages before retrying; no rollback was performed.");
            }
        }
        axon_operation_result_clear(&result);
        return(status);
    }
    axon_alpm_t *core = NULL;
    axon_config_error_t config_error;
    if((action == AXON_CHECK_DATABASE ? axon_config_open_readonly(config, &core, &config_error)
                                     : axon_config_open(config, &core, &config_error)) != AXON_CONFIG_OK) {
        message(NULL, config_error.message);
        return(1);
    }
    alpm_errno_t error = ALPM_ERR_OK;
    if(action == AXON_CHECK_DATABASE) {
        error = axon_maintenance_check(core, message, NULL);
        if(error == ALPM_ERR_OK) {
            puts("Database and dependency checks completed; no repair performed.");
        }
    }

    else if(action == AXON_ORPHANS) {
        const alpm_list_t *packages = NULL;
        error = axon_alpm_local_packages(core, &packages);
        for(const alpm_list_t *item = packages; item; item = item->next) {
            alpm_pkg_t *package = item->data;
            if(axon_package_orphan(package)) {
                axon_fprintf(stdout, "%s %s\n", alpm_pkg_get_name(package), alpm_pkg_get_version(package));
            }
        }
    }

    else {
        for(int index = 0; index < count; index++) {
            alpm_pkg_t *package = NULL;
            alpm_errno_t found = axon_alpm_local_package(core, names[index], &package);
            if(found != ALPM_ERR_OK) {
                axon_fprintf(stderr, "axon: %s: %s\n", names[index], alpm_strerror(found));
                error = found;
                continue;
            }
            axon_fprintf(stdout, "%s %s\n", alpm_pkg_get_name(package), alpm_pkg_get_version(package));
            dependencies("Dependencies", alpm_pkg_get_depends(package));
            dependencies("Optional dependencies", alpm_pkg_get_optdepends(package));
            alpm_list_t *required = alpm_pkg_compute_requiredby(package);
            fputs("Required by:", stdout);
            for(alpm_list_t *item = required; item; item = item->next) {
                axon_fprintf(stdout, " %s", (char *)item->data);
            }
            putchar('\n');
            alpm_list_free_inner(required, free);
            alpm_list_free(required);
        }
    }
    if(error != ALPM_ERR_OK) {
        message(NULL, alpm_strerror(error));
    }
    int released = axon_alpm_release(&core);
    return(error == ALPM_ERR_OK && released == 0 ? 0 : 1);
}

int axon_cli_maintenance(const axon_config_t *config, int action, int count, char **names)
{
    if(action == AXON_CLEAN_CACHE || action == AXON_REASON_EXPLICIT || action == AXON_REASON_DEPENDENCY) {
        axon_privilege_result_t privilege = axon_privilege_check(1);
        if(privilege != AXON_PRIVILEGE_OK) {
            axon_fprintf(stderr, "axon: %s\n", axon_privilege_message(privilege));
            return(1);
        }
    }
    struct sigaction handler = {.sa_handler = interrupt}, previous_int, previous_term, previous_pipe;
    sigemptyset(&handler.sa_mask);
    interrupted = 0;
    if(sigaction(SIGINT, &handler, &previous_int) < 0) {
        return(1);
    }
    if(sigaction(SIGTERM, &handler, &previous_term) < 0) {
        sigaction(SIGINT, &previous_int, NULL);
        return(1);
    }
    handler.sa_handler = SIG_IGN;
    if(sigaction(SIGPIPE, &handler, &previous_pipe) < 0) {
        sigaction(SIGINT, &previous_int, NULL);
        sigaction(SIGTERM, &previous_term, NULL);
        return(1);
    }
    int status = maintenance(config, action, count, names);
    if(axon_output_failed() || fflush(stdout) != 0 || ferror(stdout) || fflush(stderr) != 0 || ferror(stderr)) {
        status = 1;
    }
    if(interrupted) {
        status = 128 + interrupted;
    }
    sigaction(SIGINT, &previous_int, NULL);
    sigaction(SIGTERM, &previous_term, NULL);
    sigaction(SIGPIPE, &previous_pipe, NULL);
    return(status);
}
