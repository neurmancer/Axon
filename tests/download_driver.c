#define _POSIX_C_SOURCE 200809L
#include "operation.h"
#include "alpm_internal.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile sig_atomic_t cancelled;
static int cancel_download;
static int downloads;
static int questions;

#ifndef AXON_TEST_PRODUCTION
axon_privilege_result_t __wrap_axon_privilege_check(int require_root)
{
    (void)require_root;
    return(AXON_PRIVILEGE_OK);
}

#endif

static void download(void *context, const char *name, alpm_download_event_type_t event, void *data)
{
    (void)context;
    (void)name;
    if(event == ALPM_DOWNLOAD_PROGRESS) {
        alpm_download_event_progress_t *progress = data;
        downloads++;
        if(cancel_download && progress->downloaded > 0) {
            cancelled = 1;
        }
    }
}

static axon_decision_t question(void *context, const alpm_question_t *value, size_t *provider)
{
    (void)context;
    (void)value;
    (void)provider;
    questions++;
    return(AXON_DECISION_DECLINE);
}

static void log_message(void *context, alpm_loglevel_t level, const char *format, va_list args)
{
    (void)context;
    if(level & (ALPM_LOG_ERROR | ALPM_LOG_WARNING)) {
        vfprintf(stderr, format, args);
    }
}

int main(int argc, char **argv)
{
    assert(argc >= 3);
    axon_config_t *config = NULL;
    axon_config_error_t config_error = {0};
    assert(axon_config_load(argv[2], &config, &config_error) == AXON_CONFIG_OK);
    axon_operation_callbacks_t callbacks = {.download = download, .question = question,
        .log = log_message, .cancel_requested = &cancelled};
    axon_operation_result_t result = {0};
    axon_operation_t *operation = NULL;
    cancel_download = strstr(argv[1], "cancel") != NULL;
    if(strcmp(argv[1], "options") == 0) {
        axon_alpm_t *core = NULL;
        if(axon_config_open(config, &core, &config_error) != AXON_CONFIG_OK) {
            result.status = AXON_OPERATION_CONFIG;
            goto output;
        }
        const axon_config_options_t *options = axon_config_options(config);
        assert(alpm_option_get_checkspace(core->handle) == options->check_space);
        assert(alpm_option_get_disable_sandbox_filesystem(core->handle)
                == (options->disable_sandbox || options->disable_sandbox_filesystem));
        assert(alpm_option_get_disable_sandbox_syscalls(core->handle)
                == (options->disable_sandbox || options->disable_sandbox_syscalls));
        if(options->download_user != NULL) {
            assert(strcmp(alpm_option_get_sandboxuser(core->handle), options->download_user) == 0);
        }
        assert(axon_alpm_release(&core) == 0);
    }
    else if(strncmp(argv[1], "refresh", 7) == 0) {
        axon_operation_refresh_with_callbacks(config, 1, &callbacks, &result);
    }
    else {
        assert(argc == 4);
        axon_operation_target_t target = {.name = argv[3]};
        axon_operation_request_t request = {.kind = AXON_OPERATION_INSTALL,
            .targets = &target, .target_count = 1,
            .flags = ALPM_TRANS_FLAG_NOHOOKS | ALPM_TRANS_FLAG_NOSCRIPTLET,
            .callbacks = callbacks};
#ifdef AXON_TEST_PRODUCTION
        request.flags = 0;
#endif
        if(strcmp(argv[1], "remove") == 0) {
            request.kind = AXON_OPERATION_REMOVE;
        }
        if(strcmp(argv[1], "file") == 0) {
            request.kind = AXON_OPERATION_FILE;
        }
        if(strncmp(argv[1], "upgrade", 7) == 0) {
            request.kind = AXON_OPERATION_UPGRADE;
            request.targets = NULL;
            request.target_count = 0;
            axon_operation_prepare_upgrade(config, 1, &request, &operation, &result);
        }
        else if(strcmp(argv[1], "bad-upgrade") == 0) {
            axon_operation_prepare_upgrade(config, 1, &request, &operation, &result);
        }
        else {
            axon_operation_prepare(config, &request, &operation, &result);
        }
        if(result.status == AXON_OPERATION_OK && operation != NULL) {
            const axon_operation_plan_t *plan = axon_operation_plan(operation);
            printf("plan=%zu\n", plan->count);
            if(strcmp(argv[1], "upgrade-plan") != 0) {
                axon_operation_approval_t approval = {.plan_id = plan->id, .confirm = 1,
                    .approved_flags = plan->flags};
                axon_operation_commit(operation, &approval, &result);
            }
        }
    }
output:
    printf("status=%d\nbackend=%d\ncleanup=%d\ncommitted=%d\nchanged=%d\nrefreshed=%d\ncancelled=%d\ndownloads=%d\nquestions=%d\n",
        result.status, result.backend_error, result.cleanup_error, result.committed,
        result.may_have_changes, result.refreshed, result.cancelled, downloads, questions);
    for(alpm_list_t *item = result.diagnostics; item; item = item->next) {
        fprintf(stderr, "%s\n", (char *)item->data);
    }
    axon_operation_result_clear(&result);
    assert(axon_operation_free(&operation, &result) == AXON_OPERATION_OK);
    axon_operation_result_clear(&result);
    axon_config_free(&config);
    return(0);
}
