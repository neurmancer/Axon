#define _POSIX_C_SOURCE 200809L
#include "../ui/output.h"
#include "mutation.h"
#include "../ui/theme.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

static volatile sig_atomic_t interrupted;
static int input_failed;
static int progress_active;

static void finish_progress(void)
{
    if(progress_active) {
        fputc('\n', stderr);
        progress_active = 0;
    }
}

static void progress_bar(const char *label, int percent)
{
    struct winsize window;
    int width = 80;
    if(ioctl(STDERR_FILENO, TIOCGWINSZ, &window) == 0 && window.ws_col > 0) {
        width = window.ws_col;
    }
    int cells = width >= 60 ? 24 : width / 3;
    int name_width = width - cells - 5;
    if(name_width < 0) {
        return;
    }
    if(name_width > 60) {
        name_width = 60;
    }
    if(percent < 0) {
        percent = 0;
    }
    if(percent > 100) {
        percent = 100;
    }
    char *display = axon_display_text(label, name_width);
    if(display == NULL) {
        interrupted = SIGPIPE;
        return;
    }
    fprintf(stderr, "\r\033[K%s%s %s[", axon_color(stderr, AXON_BLUE), display, axon_color(stderr, AXON_PURPLE));
    free(display);
    fputs(axon_color(stderr, AXON_GREEN), stderr);
    for(int index = 0; index < cells; index++) {
        fputc(index < cells * percent / 100 ? '=' : '-', stderr);
    }
    fprintf(stderr, "%s]%s", axon_color(stderr, AXON_PURPLE), axon_color(stderr, AXON_RESET));
    progress_active = 1;
    if(percent == 100) {
        finish_progress();
    }
    if(fflush(stderr) != 0 || ferror(stderr)) {
        interrupted = SIGPIPE;
    }
}

static void interrupt(int number)
{
    interrupted = number;
}

static int answer(char *line, size_t size)
{
    if(interrupted || input_failed || axon_output_failed() || fflush(stdout) != 0 || ferror(stdout) || fflush(stderr) != 0) {
        return(0);
    }
    if(fgets(line, (int)size, stdin) == NULL || interrupted) {
        return(0);
    }
    char *newline = strchr(line, '\n');
    if(newline == NULL) {
        input_failed = 1;
        return(0);
    }
    *newline = '\0';
    return(1);
}

static int confirm(const char *prompt)
{
    char line[32];
    finish_progress();
    fprintf(stderr, "%s%s [y/N]%s ", axon_color(stderr, AXON_PURPLE), prompt, axon_color(stderr, AXON_RESET));
    return(answer(line, sizeof(line)) && (strcmp(line, "y") == 0 || strcmp(line, "yes") == 0
                                         || strcmp(line, "Y") == 0));
}

static axon_decision_t question(void *context, const alpm_question_t *value, size_t *provider)
{
    (void)context;
    finish_progress();
    switch(value->type) {
        case ALPM_QUESTION_INSTALL_IGNOREPKG:
            axon_fprintf(stderr, "Ignored package: %s\n", alpm_pkg_get_name(value->install_ignorepkg.pkg));
            break;
        case ALPM_QUESTION_REPLACE_PKG:
            axon_fprintf(stderr, "Replace %s with %s/%s?\n", alpm_pkg_get_name(value->replace.oldpkg),
                    alpm_db_get_name(value->replace.newdb), alpm_pkg_get_name(value->replace.newpkg));
            break;
        case ALPM_QUESTION_CONFLICT_PKG:
            axon_fprintf(stderr, "%s conflicts with %s; remove %s?\n",
                    alpm_pkg_get_name(value->conflict.conflict->package1),
                    alpm_pkg_get_name(value->conflict.conflict->package2),
                    alpm_pkg_get_name(value->conflict.conflict->package2));
            break;
        case ALPM_QUESTION_CORRUPTED_PKG:
            axon_fprintf(stderr, "Delete invalid archive %s (%s)?\n", value->corrupted.filepath,
                    alpm_strerror(value->corrupted.reason));
            break;
        case ALPM_QUESTION_REMOVE_PKGS:
            fputs("Skip these unresolved targets:\n", stderr);
            for(alpm_list_t *item = value->remove_pkgs.packages; item; item = item->next) {
                axon_fprintf(stderr, "  %s\n", alpm_pkg_get_name(item->data));
            }
            break;
        case ALPM_QUESTION_IMPORT_KEY:
            axon_fprintf(stderr, "Import key %s (%s)?\n", value->import_key.fingerprint,
                    value->import_key.uid != NULL ? value->import_key.uid : "unknown identity");
            break;
        case ALPM_QUESTION_SELECT_PROVIDER: {
            size_t count = 0;
            for(alpm_list_t *item = value->select_provider.providers; item; item = item->next) {
                alpm_pkg_t *package = item->data;
                axon_fprintf(stderr, "  %zu) %s/%s\n", ++count,
                        alpm_db_get_name(alpm_pkg_get_db(package)), alpm_pkg_get_name(package));
            }
            fputs("Choose provider number (empty cancels): ", stderr);
            char line[32], *end;
            if(!answer(line, sizeof(line)) || line[0] < '0' || line[0] > '9') {
                return(AXON_DECISION_CANCEL);
            }
            errno = 0;
            unsigned long number = strtoul(line, &end, 10);
            if(errno || *end || number == 0 || number > count) {
                return(AXON_DECISION_UNANSWERED);
            }
            *provider = number - 1;
            return(AXON_DECISION_ACCEPT);
        }
        default:
            return(AXON_DECISION_UNANSWERED);
    }
    int yes = confirm("Allow this action?");
    if(interrupted || input_failed || feof(stdin) || ferror(stdin)) {
        return(AXON_DECISION_CANCEL);
    }
    return(yes ? AXON_DECISION_ACCEPT : AXON_DECISION_DECLINE);
}

static void log_message(void *context, alpm_loglevel_t level, const char *format, va_list args)
{
    (void)context;
    if(level & (ALPM_LOG_ERROR | ALPM_LOG_WARNING)) {
        finish_progress();
        if(axon_vfprintf(stderr, format, args) < 0) {
            interrupted = SIGPIPE;
        }
    }
}

static void event(void *context, alpm_event_t *value)
{
    (void)context;
    if(value->type == ALPM_EVENT_SCRIPTLET_INFO) {
        finish_progress();
        if(axon_fprintf(stderr, "%s\n", value->scriptlet_info.line) < 0) {
            interrupted = SIGPIPE;
        }
    }
}

static void progress(void *context, alpm_progress_t type, const char *package,
                     int percent, size_t total, size_t current)
{
    const axon_config_options_t *options = context;
    (void)type;
    if(!options->no_progress_bar && axon_terminal_output(stderr)) {
        char label[256];
        snprintf(label, sizeof(label), "[%zu/%zu] %s", current, total,
                 package != NULL && *package ? package : "Processing");
        progress_bar(label, percent);
    }
}

static void download(void *context, const char *name, alpm_download_event_type_t type, void *data)
{
    const axon_config_options_t *options = context;
    if(!options->no_progress_bar && axon_terminal_output(stderr) && type == ALPM_DOWNLOAD_PROGRESS) {
        const alpm_download_event_progress_t *value = data;
        char label[256];
        snprintf(label, sizeof(label), "Downloading %s", name);
        int percent = 0;
        if(value->total > 0 && value->downloaded > 0) {
            percent = value->downloaded >= value->total ? 100 : (int)(100.0L * value->downloaded / value->total);
        }
        progress_bar(label, percent);
    }
    if(type == ALPM_DOWNLOAD_COMPLETED) {
        finish_progress();
    }
}

static void report(const axon_operation_result_t *result)
{
    finish_progress();
    if(result->status == AXON_OPERATION_CONFIG) {
        axon_fprintf(stderr, "axon: %s:%zu: %s\n", result->config_error.file,
                result->config_error.line, result->config_error.message);
    }
    for(alpm_list_t *item = result->status != AXON_OPERATION_OK ? result->diagnostics : NULL;
            item; item = item->next) {
        axon_fprintf(stderr, "axon: %s\n", (char *)item->data);
    }
    if(result->status != AXON_OPERATION_OK && result->diagnostics == NULL
            && result->status != AXON_OPERATION_CONFIG) {
        axon_fprintf(stderr, "axon: operation failed (status %d)\n", result->status);
    }
    if(result->backend_error == ALPM_ERR_HANDLE_LOCK) {
        fputs("axon: another package manager may be running; verify ownership of the database lock before any manual recovery\n", stderr);
    }
    if(result->backend_error == ALPM_ERR_DB_INVALID || result->backend_error == ALPM_ERR_DB_VERSION) {
        fputs("axon: inspect --check-db and preserve the database and transaction log before repairing it\n", stderr);
    }
    if(result->cancelled) {
        fputs("axon: operation cancelled\n", stderr);
    }
    if(result->diagnostics_incomplete) {
        fputs("axon: some transaction diagnostics could not be retained\n", stderr);
    }
    if(result->status != AXON_OPERATION_OK && result->package_phase_completed) {
        axon_fprintf(stderr, "axon: package processing completed (%zu package operations); transaction reported errors\n",
                result->completed_packages);
    }
    else if(result->status != AXON_OPERATION_OK && result->completed_packages != 0) {
        axon_fprintf(stderr, "axon: %zu package operations completed before failure\n", result->completed_packages);
    }
    if(result->status != AXON_OPERATION_OK && result->may_have_changes) {
        fputs("axon: changes may have occurred; no automatic rollback was performed\n", stderr);
    }
}

int axon_cli_mutate(const axon_config_t *config, axon_operation_kind_t kind,
                    int count, char **names, int refresh, int refresh_only,
                    int allow_downgrade, int flags)
{
    axon_privilege_result_t privilege = axon_privilege_check(1);
    if(privilege != AXON_PRIVILEGE_OK) {
        axon_fprintf(stderr, "axon: %s\n", axon_privilege_message(privilege));
        return(1);
    }
    struct sigaction action = {.sa_handler = interrupt}, previous_int, previous_term, previous_pipe;
    sigemptyset(&action.sa_mask);
    interrupted = 0;
    input_failed = 0;
    progress_active = 0;
    if(sigaction(SIGINT, &action, &previous_int) < 0) {
        return(1);
    }
    if(sigaction(SIGTERM, &action, &previous_term) < 0) {
        sigaction(SIGINT, &previous_int, NULL);
        return(1);
    }
    action.sa_handler = SIG_IGN;
    if(sigaction(SIGPIPE, &action, &previous_pipe) < 0) {
        sigaction(SIGINT, &previous_int, NULL);
        sigaction(SIGTERM, &previous_term, NULL);
        return(1);
    }
    int exit_status = 1;
    axon_operation_t *operation = NULL;
    axon_operation_result_t result = {0}, cleanup = {0};
    axon_operation_target_t *targets = count ? calloc((size_t)count, sizeof(*targets)) : NULL;
    char **qualified = count ? calloc((size_t)count, sizeof(*qualified)) : NULL;
    const char **protected = NULL;
    if(count && (targets == NULL || qualified == NULL)) {
        fputs("axon: out of memory\n", stderr);
        goto done;
    }
    for(int index = 0; index < count; index++) {
        targets[index].name = names[index];
        if((kind == AXON_OPERATION_INSTALL || kind == AXON_OPERATION_UPGRADE)
                && strchr(names[index], '/') != NULL) {
            qualified[index] = strdup(names[index]);
            if(qualified[index] == NULL) {
                fputs("axon: out of memory\n", stderr);
                goto done;
            }
            char *slash = strchr(qualified[index], '/');
            *slash = '\0';
            targets[index].repository = qualified[index];
            targets[index].name = slash + 1;
        }
    }
    axon_operation_callbacks_t callbacks = {.question = question, .log = log_message,
        .event = event, .progress = progress, .download = download,
        .context = (void *)axon_config_options(config), .cancel_requested = &interrupted};
    axon_operation_request_t request = {.kind = kind, .targets = targets,
        .target_count = (size_t)count, .flags = flags, .allow_downgrade = allow_downgrade, .callbacks = callbacks};
    if(refresh && !confirm("Refresh repository databases?")) {
        goto declined;
    }
    if(refresh_only) {
        axon_operation_refresh_with_callbacks(config, refresh > 1, &callbacks, &result);
    }
    else if(refresh) {
        axon_operation_prepare_upgrade(config, refresh > 1, &request, &operation, &result);
    }
    else {
        axon_operation_prepare(config, &request, &operation, &result);
    }
    if(result.status != AXON_OPERATION_OK) {
        report(&result);
        goto done;
    }
    if(refresh_only) {
        puts("Repository refresh completed.");
        exit_status = 0;
        goto done;
    }
    const axon_operation_plan_t *plan = axon_operation_plan(operation);
    static const char *changes[] = {"install", "remove", "upgrade", "downgrade", "reinstall", "replace"};
    fprintf(stdout, "%sPrepared transaction: %zu package changes%s\n", axon_color(stdout, AXON_PURPLE),
           plan->count, axon_color(stdout, AXON_RESET));
    for(size_t index = 0; index < plan->count; index++) {
        const axon_plan_entry_t *entry = &plan->entries[index];
        fputs(axon_color(stdout, AXON_BLUE), stdout);
        axon_fprintf(stdout, "  %s %s: %s -> %s%s\n", changes[entry->kind], entry->name,
               entry->old_version != NULL ? entry->old_version : "-",
               entry->new_version != NULL ? entry->new_version : "-", entry->held ? " [protected removal]" : "");
        fputs(axon_color(stdout, AXON_RESET), stdout);
    }
    axon_fprintf(stdout, "Download: %" PRIu64 " bytes; installed size change: %" PRId64 " bytes\n",
           plan->download_bytes, plan->installed_delta);
    if(axon_output_failed() || fflush(stdout) != 0 || ferror(stdout) || fflush(stderr) != 0 || ferror(stderr)) {
        goto done;
    }
    if(plan->count == 0) {
        puts("Nothing to do.");
        exit_status = 0;
        goto done;
    }
    protected = calloc(plan->count, sizeof(*protected));
    if(protected == NULL) {
        fputs("axon: out of memory\n", stderr);
        goto done;
    }
    axon_operation_approval_t approval = {.plan_id = plan->id, .confirm = 1,
        .approved_flags = plan->flags, .protected_removals = protected};
    for(size_t index = 0; index < plan->count; index++) {
        if(plan->entries[index].held) {
            axon_fprintf(stderr, "Protected package: %s\n", plan->entries[index].name);
            if(!confirm("Approve removal of this exact package?")) {
                goto declined;
            }
            protected[approval.protected_removal_count++] = plan->entries[index].name;
        }
    }
    if(plan->has_downgrades) {
        if(!confirm("Approve the listed downgrades?")) {
            goto declined;
        }
        approval.approve_downgrades = 1;
    }
    if(!confirm("Commit this transaction?")) {
        goto declined;
    }
    axon_operation_commit(operation, &approval, &result);
    report(&result);
    if(result.status == AXON_OPERATION_OK) {
        fprintf(stdout, "%sTransaction completed.%s\n", axon_color(stdout, AXON_GREEN), axon_color(stdout, AXON_RESET));
        exit_status = 0;
    }
    goto done;

declined:
    fputs("axon: cancelled before commit\n", stderr);
done:
    finish_progress();
    if(axon_operation_free(&operation, &cleanup) != AXON_OPERATION_OK) {
        report(&cleanup);
        exit_status = 1;
    }
    axon_operation_result_clear(&cleanup);
    axon_operation_result_clear(&result);
    free(protected);
    for(int index = 0; qualified != NULL && index < count; index++) {
        free(qualified[index]);
    }
    free(qualified);
    free(targets);
    if(axon_output_failed() || fflush(stdout) != 0 || ferror(stdout) || fflush(stderr) != 0 || ferror(stderr)) {
        fputs("axon: failed to write output\n", stderr);
        exit_status = 1;
    }
    if(interrupted) {
        exit_status = 128 + interrupted;
    }
    sigaction(SIGINT, &previous_int, NULL);
    sigaction(SIGTERM, &previous_term, NULL);
    sigaction(SIGPIPE, &previous_pipe, NULL);
    return(exit_status);
}
