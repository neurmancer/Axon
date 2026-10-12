#include "operation_internal.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

int axon_operation_poll(axon_operation_t *operation)
{
    if(operation->callbacks.cancel_requested != NULL
            && *operation->callbacks.cancel_requested) {
        operation->cancelled = 1;
    }
    if((operation->cancelled || operation->question) && operation->committing && operation->transaction_started
            && !operation->interrupting && !operation->interrupted) {
        operation->interrupting = 1;
        operation->interrupted = axon_alpm_transaction_interrupt(operation->core) == ALPM_ERR_OK;
        operation->interrupting = 0;
    }
    return(operation->cancelled || operation->question);
}

void axon_operation_question(void *context, alpm_question_t *question)
{
    axon_operation_t *operation = context;
    int provider = question->type == ALPM_QUESTION_SELECT_PROVIDER;
    question->any.answer = provider ? -1 : 0;
    if(axon_operation_poll(operation)) {
        return;
    }
    size_t index = SIZE_MAX;
    axon_decision_t decision = AXON_DECISION_UNANSWERED;
    if(operation->callbacks.question != NULL) {
        decision = operation->callbacks.question(operation->callbacks.context, question, &index);
    }
    else if(question->type == ALPM_QUESTION_REPLACE_PKG && operation->allow_replacements) {
        decision = AXON_DECISION_ACCEPT;
    }
    else if(provider && alpm_list_count(question->select_provider.providers) == 1) {
        decision = AXON_DECISION_ACCEPT;
        index = 0;
    }
    if(decision == AXON_DECISION_CANCEL) {
        operation->cancelled = 1;
    }
    if(axon_operation_poll(operation)) {
        return;
    }
    if(decision == AXON_DECISION_ACCEPT) {
        switch(question->type) {
            case ALPM_QUESTION_SELECT_PROVIDER:
                if(index > INT_MAX || index >= alpm_list_count(question->select_provider.providers)) {
                    break;
                }
                question->select_provider.use_index = (int)index;
                return;
            case ALPM_QUESTION_INSTALL_IGNOREPKG:
            case ALPM_QUESTION_REPLACE_PKG:
            case ALPM_QUESTION_CONFLICT_PKG:
            case ALPM_QUESTION_CORRUPTED_PKG:
            case ALPM_QUESTION_REMOVE_PKGS:
            case ALPM_QUESTION_IMPORT_KEY:
                question->any.answer = 1;
                return;
            default:
                break;
        }
    }
    else if(decision == AXON_DECISION_DECLINE) {
        return;
    }
    operation->question = question->type;
    operation->ignored = question->type == ALPM_QUESTION_INSTALL_IGNOREPKG;
    axon_operation_poll(operation);
}

static void event(void *context, alpm_event_t *value)
{
    axon_operation_t *operation = context;
    if(value->type == ALPM_EVENT_TRANSACTION_START) {
        operation->transaction_started = 1;
    }
    if(value->type == ALPM_EVENT_TRANSACTION_DONE) {
        operation->transaction_started = 0;
    }
    if(operation->callbacks.event != NULL) {
        operation->callbacks.event(operation->callbacks.context, value);
    }
    axon_operation_poll(operation);
}

static void progress(void *context, alpm_progress_t value, const char *package,
                     int percent, size_t total, size_t current)
{
    axon_operation_t *operation = context;
    if(operation->callbacks.progress != NULL) {
        operation->callbacks.progress(operation->callbacks.context, value, package, percent, total, current);
    }
    axon_operation_poll(operation);
}

static void download(void *context, const char *name, alpm_download_event_type_t value, void *data)
{
    axon_operation_t *operation = context;
    if(operation->callbacks.download != NULL) {
        operation->callbacks.download(operation->callbacks.context, name, value, data);
    }
    axon_operation_poll(operation);
}

static void log_message(void *context, alpm_loglevel_t level, const char *format, va_list args)
{
    axon_operation_t *operation = context;
    if(operation->cleanup_result != NULL && (level & (ALPM_LOG_ERROR | ALPM_LOG_WARNING))) {
        axon_operation_result_t *result = operation->cleanup_result;
        if(result->cleanup_error == ALPM_ERR_OK) {
            result->cleanup_error = ALPM_ERR_SYSTEM;
        }
        va_list copy;
        va_copy(copy, args);
        int length = vsnprintf(NULL, 0, format, copy);
        va_end(copy);
        char *message = length >= 0 ? malloc((size_t)length + 1) : NULL;
        if(message != NULL) {
            va_copy(copy, args);
            vsnprintf(message, (size_t)length + 1, format, copy);
            va_end(copy);
            if(alpm_list_append(&result->diagnostics, message) == NULL) {
                free(message);
            }
        }
    }
    if(operation->callbacks.log != NULL) {
        operation->callbacks.log(operation->callbacks.context, level, format, args);
    }
}

alpm_errno_t axon_operation_set_callbacks(axon_operation_t *operation)
{
    axon_alpm_callbacks_t callbacks = {.log = log_message, .event = event,
        .progress = progress, .download = download,
        .question = axon_operation_question, .context = operation};
    return(axon_alpm_callbacks(operation->core, &callbacks));
}
