#define _XOPEN_SOURCE 700

#include "operation_internal.h"
#include "alpm_extra.h"
#include "alpm_internal.h"

#include <archive.h>
#include <archive_entry.h>
#include <assert.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int authorized = 1;
static axon_operation_result_t result;
static const int safe_flags = ALPM_TRANS_FLAG_NOHOOKS | ALPM_TRANS_FLAG_NOSCRIPTLET;

axon_privilege_result_t __wrap_axon_privilege_check(int require_root)
{
    return(require_root && !authorized ? AXON_PRIVILEGE_ROOT_REQUIRED : AXON_PRIVILEGE_OK);
}

#define EXPECT(call, expected) do { \
    axon_operation_status_t status = (call); \
    if(status != (expected)) { \
        fprintf(stderr, "%s:%d: %s returned %d, expected %d (alpm %s)\n", \
                __FILE__, __LINE__, #call, status, expected, alpm_strerror(result.backend_error)); \
        for(alpm_list_t *item = result.diagnostics; item; item = item->next) { \
            fprintf(stderr, "%s\n", (char *)item->data); \
        } \
        abort(); \
    } \
} while(0)

static int fail_cleanup;
static int commit_log;
static int cleanup_signal;
static int allocation_enabled;
static size_t allocation_count, allocation_failure;

void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
char *__real_strdup(const char *text);
alpm_list_t *__real_alpm_list_append(alpm_list_t **list, void *data);
alpm_list_t *__real_alpm_list_append_strdup(alpm_list_t **list, const char *text);

static int allocation_fails(void)
{
    return(allocation_enabled && ++allocation_count == allocation_failure);
}

void *__wrap_malloc(size_t size)
{
    return(allocation_fails() ? NULL : __real_malloc(size));
}

void *__wrap_calloc(size_t count, size_t size)
{
    return(allocation_fails() ? NULL : __real_calloc(count, size));
}

char *__wrap_strdup(const char *text)
{
    return(allocation_fails() ? NULL : __real_strdup(text));
}

alpm_list_t *__wrap_alpm_list_append(alpm_list_t **list, void *data)
{
    return(allocation_fails() ? NULL : __real_alpm_list_append(list, data));
}

alpm_list_t *__wrap_alpm_list_append_strdup(alpm_list_t **list, const char *text)
{
    return(allocation_fails() ? NULL : __real_alpm_list_append_strdup(list, text));
}

static void emit_log(axon_alpm_t *core, alpm_loglevel_t level, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    alpm_option_get_logcb(core->handle)(alpm_option_get_logcb_ctx(core->handle), level, format, args);
    va_end(args);
}

alpm_errno_t __real_axon_alpm_transaction_commit(axon_alpm_t *core, axon_alpm_problem_t *problem);
alpm_errno_t __wrap_axon_alpm_transaction_commit(axon_alpm_t *core, axon_alpm_problem_t *problem)
{
    alpm_errno_t error = __real_axon_alpm_transaction_commit(core, problem);
    if(commit_log && error == ALPM_ERR_OK) {
        emit_log(core, commit_log == 1 ? ALPM_LOG_WARNING : ALPM_LOG_ERROR,
                 "helper fixture exited with status %d\n", 7);
    }
    return(error);
}
alpm_errno_t __real_axon_alpm_transaction_release(axon_alpm_t *core);
alpm_errno_t __wrap_axon_alpm_transaction_release(axon_alpm_t *core)
{
    if(cleanup_signal) {
        cleanup_signal = 0;
        assert(raise(SIGUSR1) == 0);
    }
    alpm_errno_t error = __real_axon_alpm_transaction_release(core);
    if(fail_cleanup && error == ALPM_ERR_OK) {
        fail_cleanup = 0;
        return(ALPM_ERR_SYSTEM);
    }
    return(error);
}

static volatile sig_atomic_t stop;
static void stop_signal(int number)
{
    (void)number;
    stop = 1;
}

struct observer {
    axon_decision_t answer;
    size_t provider;
    int questions, events, progress, logs, downloads;
    alpm_event_type_t cancel_event;
    int cancel_download;
    axon_operation_t **operation;
};

static axon_decision_t decide(void *context, const alpm_question_t *question, size_t *provider)
{
    struct observer *observer = context;
    assert(question != NULL);
    observer->questions++;
    *provider = observer->provider;
    return(observer->answer);
}

static void observe_event(void *context, alpm_event_t *event)
{
    struct observer *observer = context;
    observer->events++;
    if(observer->operation != NULL && *observer->operation != NULL) {
        assert(axon_operation_free(observer->operation, &result) == AXON_OPERATION_STATE);
        assert(axon_operation_cancel(*observer->operation, &result) == AXON_OPERATION_STATE);
        assert(axon_operation_commit(*observer->operation, NULL, &result) == AXON_OPERATION_STATE);
    }
    if(event->type == observer->cancel_event) {
        assert(raise(SIGUSR1) == 0);
    }
}

static void observe_progress(void *context, alpm_progress_t progress, const char *package,
                             int percent, size_t count, size_t current)
{
    struct observer *observer = context;
    (void)progress;
    (void)package;
    (void)percent;
    (void)count;
    (void)current;
    observer->progress++;
}

static void observe_log(void *context, alpm_loglevel_t level, const char *format, va_list args)
{
    struct observer *observer = context;
    (void)level;
    char message[256];
    assert(vsnprintf(message, sizeof(message), format, args) >= 0);
    observer->logs++;
}

static void observe_download(void *context, const char *name, alpm_download_event_type_t type, void *data)
{
    struct observer *observer = context;
    (void)type;
    (void)data;
    assert(name != NULL);
    observer->downloads++;
    if(observer->cancel_download) {
        stop = 1;
    }
}

static void decisions(void)
{
    struct observer observer = {0};
    axon_operation_t operation = {.callbacks = {.question = decide, .context = &observer}};
    const alpm_question_type_t types[] = {ALPM_QUESTION_INSTALL_IGNOREPKG,
        ALPM_QUESTION_REPLACE_PKG, ALPM_QUESTION_CONFLICT_PKG, ALPM_QUESTION_CORRUPTED_PKG,
        ALPM_QUESTION_REMOVE_PKGS, ALPM_QUESTION_IMPORT_KEY};
    for(size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        for(int answer = AXON_DECISION_UNANSWERED; answer <= AXON_DECISION_CANCEL; answer++) {
            operation.question = operation.cancelled = 0;
            observer.answer = answer;
            alpm_question_t question = {.type = types[i]};
            axon_operation_question(&operation, &question);
            assert(question.any.answer == (answer == AXON_DECISION_ACCEPT));
            assert(!!operation.question == (answer == AXON_DECISION_UNANSWERED));
            assert(operation.cancelled == (answer == AXON_DECISION_CANCEL));
        }
    }
    alpm_list_t *providers = NULL;
    assert(alpm_list_append(&providers, &observer) != NULL);
    assert(alpm_list_append(&providers, &operation) != NULL);
    for(size_t index = 0; index < 4; index++) {
        operation.question = operation.cancelled = 0;
        observer.answer = AXON_DECISION_ACCEPT;
        observer.provider = index == 3 ? SIZE_MAX : index;
        alpm_question_t question = {.select_provider = {.type = ALPM_QUESTION_SELECT_PROVIDER,
                                                       .providers = providers}};
        axon_operation_question(&operation, &question);
        assert(question.select_provider.use_index == (index < 2 ? (int)index : -1));
        assert(!!operation.question == (index >= 2));
    }
    alpm_list_free(providers);
}

struct package {
    const char *name;
    const char *version;
    int size;
    const char *dependency;
    const char *replaces;
    const char *group;
    const char *architecture;
    char path[512];
};

static void archive_item(struct archive *archive, const char *name, const char *text)
{
    struct archive_entry *entry = archive_entry_new();
    assert(entry != NULL);
    archive_entry_set_pathname(entry, name);
    archive_entry_set_filetype(entry, AE_IFREG);
    archive_entry_set_perm(entry, 0644);
    archive_entry_set_uid(entry, getuid());
    archive_entry_set_gid(entry, getgid());
    archive_entry_set_size(entry, (la_int64_t)strlen(text));
    assert(archive_write_header(archive, entry) == ARCHIVE_OK);
    assert(archive_write_data(archive, text, strlen(text)) == (la_ssize_t)strlen(text));
    archive_entry_free(entry);
}

static struct archive *archive_open(const char *path)
{
    struct archive *archive = archive_write_new();
    assert(archive != NULL);
    assert(archive_write_set_format_pax_restricted(archive) == ARCHIVE_OK);
    assert(archive_write_open_filename(archive, path) == ARCHIVE_OK);
    return(archive);
}

static void archive_close(struct archive *archive)
{
    assert(archive_write_close(archive) == ARCHIVE_OK);
    assert(archive_write_free(archive) == ARCHIVE_OK);
}

static void make_package(struct package *package, const char *directory)
{
    snprintf(package->path, sizeof(package->path), "%s/%s-%s-any.pkg.tar",
             directory, package->name, package->version);
    char metadata[2048];
    int length = snprintf(metadata, sizeof(metadata),
        "pkgname = %s\npkgver = %s\npkgdesc = Operation fixture\nsize = %d\narch = %s\n"
        "%s%s\n%s%s\n%s%s\n",
        package->name, package->version, package->size,
        package->architecture != NULL ? package->architecture : "any",
        package->dependency != NULL ? "depend = " : "", package->dependency != NULL ? package->dependency : "",
        package->replaces != NULL ? "replaces = " : "", package->replaces != NULL ? package->replaces : "",
        package->group != NULL ? "group = " : "", package->group != NULL ? package->group : "");
    assert(length > 0 && (size_t)length < sizeof(metadata));
    struct archive *archive = archive_open(package->path);
    archive_item(archive, ".PKGINFO", metadata);
    if(strcmp(package->name, "collision") == 0) {
        archive_item(archive, "busy.txt", "new content\n");
    }
    archive_close(archive);
}

static void make_repository(const char *path, struct package *packages, size_t count)
{
    struct archive *archive = archive_open(path);
    for(size_t index = 0; index < count; index++) {
        struct package *package = &packages[index];
        char *hash = alpm_compute_sha256sum(package->path);
        struct stat info;
        assert(hash != NULL && stat(package->path, &info) == 0);
        char metadata[4096];
        int length = snprintf(metadata, sizeof(metadata),
            "%%NAME%%\n%s\n\n%%VERSION%%\n%s\n\n%%DESC%%\nOperation fixture\n\n"
            "%%FILENAME%%\n%s-%s-any.pkg.tar\n\n%%ARCH%%\nany\n\n"
            "%%CSIZE%%\n%lld\n\n%%ISIZE%%\n%d\n\n%%SHA256SUM%%\n%s\n\n"
            "%s%s\n\n%s%s\n\n%s%s\n\n",
            package->name, package->version, package->name, package->version,
            (long long)info.st_size, package->size, hash,
            package->dependency != NULL ? "%DEPENDS%\n" : "", package->dependency != NULL ? package->dependency : "",
            package->replaces != NULL ? "%REPLACES%\n" : "", package->replaces != NULL ? package->replaces : "",
            package->group != NULL ? "%GROUPS%\n" : "", package->group != NULL ? package->group : "");
        assert(length > 0 && (size_t)length < sizeof(metadata));
        free(hash);
        char entry[256];
        snprintf(entry, sizeof(entry), "%s-%s/desc", package->name, package->version);
        archive_item(archive, entry, metadata);
    }
    archive_close(archive);
}

static const axon_plan_entry_t *entry(const axon_operation_plan_t *plan, const char *name)
{
    for(size_t index = 0; index < plan->count; index++) {
        if(strcmp(plan->entries[index].name, name) == 0) {
            return(&plan->entries[index]);
        }
    }
    return(NULL);
}

static axon_operation_approval_t approval(axon_operation_t *operation)
{
    const axon_operation_plan_t *plan = axon_operation_plan(operation);
    axon_operation_approval_t approved = {.plan_id = plan->id, .confirm = 1, .approved_flags = plan->flags};
    return(approved);
}

static void release(axon_operation_t **operation)
{
    EXPECT(axon_operation_free(operation, &result), AXON_OPERATION_OK);
    assert(*operation == NULL && access("db/db.lck", F_OK) != 0);
}

static void installed(const axon_config_t *config, const char *name, const char *version,
                       alpm_pkgreason_t reason)
{
    axon_alpm_t *core = NULL;
    axon_config_error_t error;
    assert(axon_config_open(config, &core, &error) == AXON_CONFIG_OK);
    alpm_pkg_t *package = NULL;
    assert(axon_alpm_local_package(core, name, &package) == ALPM_ERR_OK);
    assert(strcmp(alpm_pkg_get_version(package), version) == 0);
    assert(alpm_pkg_get_reason(package) == reason);
    assert(axon_alpm_release(&core) == 0);
}

static int remove_entry(const char *path, const struct stat *info, int type, struct FTW *walk)
{
    (void)info;
    (void)type;
    (void)walk;
    return(remove(path));
}

int main(void)
{
    decisions();
    struct sigaction action = {.sa_handler = stop_signal};
    sigemptyset(&action.sa_mask);
    assert(sigaction(SIGUSR1, &action, NULL) == 0);
    char *previous = getcwd(NULL, 0);
    char temporary[] = "/tmp/axon-operation-XXXXXX";
    assert(previous != NULL && mkdtemp(temporary) != NULL && chdir(temporary) == 0);
    const char *directories[] = {"db", "db/local", "root", "cache", "first", "second", "files"};
    for(size_t index = 0; index < sizeof(directories) / sizeof(directories[0]); index++) {
        assert(mkdir(directories[index], 0700) == 0);
    }
    struct package locals[] = {
        {.name = "essential", .version = "1-1", .size = 10},
        {.name = "consumer", .version = "1-1", .size = 20, .dependency = "essential>=1"},
        {.name = "legacy", .version = "1-1", .size = 7},
        {.name = "versioned", .version = "2-1", .size = 9},
        {.name = "versioned", .version = "1-1", .size = 8},
        {.name = "badarch", .version = "1-1", .size = 1, .architecture = "wrong"},
        {.name = "collision", .version = "1-1", .size = 12},
        {.name = "cancel-first", .version = "1-1", .size = 1},
        {.name = "cancel-second", .version = "1-1", .size = 1}
    };
    struct package packages[] = {
        {.name = "essential", .version = "2-1", .size = 12, .dependency = "helper>=1"},
        {.name = "helper", .version = "1-1", .size = 5, .group = "tools"},
        {.name = "application", .version = "1-1", .size = 15, .dependency = "helper>=1", .group = "tools"},
        {.name = "substitute", .version = "1-1", .size = 11, .replaces = "legacy"},
        {.name = "ignored", .version = "1-1", .size = 3, .group = "ignored-group"}
    };
    struct package lower_priority = {.name = "essential", .version = "3-1", .size = 14};
    for(size_t index = 0; index < sizeof(locals) / sizeof(locals[0]); index++) {
        make_package(&locals[index], "files");
    }
    for(size_t index = 0; index < sizeof(packages) / sizeof(packages[0]); index++) {
        make_package(&packages[index], "first");
    }
    make_package(&lower_priority, "second");
    make_repository("first/first.db", packages, sizeof(packages) / sizeof(packages[0]));
    make_repository("second/second.db", &lower_priority, 1);
    FILE *file = fopen("pacman.conf", "w");
    assert(file != NULL);
    fprintf(file, "[options]\nRootDir = %s/root\nDBPath = %s/db\nCacheDir = %s/cache\n"
            "LogFile = %s/log\nGPGDir = %s/keyring\nArchitecture = x86_64\nSigLevel = Never\n"
            "HoldPkg = ess* cons* leg*\nIgnoreGroup = ignored-group\n"
            "[first]\nServer = file://%s/first\n[second]\nServer = file://%s/second\n",
            temporary, temporary, temporary, temporary, temporary, temporary, temporary);
    assert(fclose(file) == 0);
    axon_config_t *config = NULL;
    axon_config_error_t config_error;
    assert(axon_config_load("pacman.conf", &config, &config_error) == AXON_CONFIG_OK);
    axon_operation_t *operation = NULL;
    axon_operation_target_t targets[4];
    for(size_t index = 0; index < 4; index++) {
        targets[index] = (axon_operation_target_t){.name = locals[index].path};
    }
    axon_operation_request_t request = {.kind = AXON_OPERATION_FILE, .targets = targets,
                                        .target_count = 4, .flags = safe_flags};
    authorized = 0;
    EXPECT(axon_operation_prepare(config, &request, &operation, &result), AXON_OPERATION_PRIVILEGE);
    EXPECT(axon_operation_refresh(config, 0, &result), AXON_OPERATION_PRIVILEGE);
    assert(operation == NULL && access("db/db.lck", F_OK) != 0);
    authorized = 1;
    allocation_count = allocation_failure = 0;
    allocation_enabled = 1;
    EXPECT(axon_operation_prepare(config, &request, &operation, &result), AXON_OPERATION_OK);
    allocation_enabled = 0;
    size_t prepare_allocations = allocation_count;
    release(&operation);
    for(size_t index = 1; index <= prepare_allocations; index++) {
        allocation_count = 0;
        allocation_failure = index;
        allocation_enabled = 1;
        axon_operation_status_t status = axon_operation_prepare(config, &request, &operation, &result);
        allocation_enabled = 0;
        assert(status != AXON_OPERATION_OK && operation == NULL && !result.may_have_changes);
        assert(access("db/db.lck", F_OK) != 0);
    }
    printf("Operation prepare allocation failures passed (%zu points).\n", prepare_allocations);
    EXPECT(axon_operation_prepare(config, &request, &operation, &result), AXON_OPERATION_OK);
    const axon_operation_plan_t *plan = axon_operation_plan(operation);
    assert(plan->count == 4 && plan->installed_delta == 46 && plan->download_bytes == 0);
    axon_operation_approval_t approved = approval(operation);
    approved.plan_id++;
    EXPECT(axon_operation_commit(operation, &approved, &result), AXON_OPERATION_APPROVAL);
    approved = approval(operation);
    approved.approved_flags = 0;
    EXPECT(axon_operation_commit(operation, &approved, &result), AXON_OPERATION_APPROVAL);
    approved = approval(operation);
    EXPECT(axon_operation_commit(operation, &approved, &result), AXON_OPERATION_OK);
    assert(result.committed && result.may_have_changes && access("db/db.lck", F_OK) != 0);
    assert(strcmp(plan->entries[0].new_version, "1-1") == 0);
    EXPECT(axon_operation_commit(operation, &approved, &result), AXON_OPERATION_STATE);
    release(&operation);

    targets[0] = (axon_operation_target_t){.name = locals[0].path};
    request = (axon_operation_request_t){.kind = AXON_OPERATION_FILE, .targets = targets,
        .target_count = 1, .flags = safe_flags};
    for(int mode = 0; mode < 3; mode++) {
        EXPECT(axon_operation_prepare(config, &request, &operation, &result), AXON_OPERATION_OK);
        approved = approval(operation);
        commit_log = mode;
        EXPECT(axon_operation_commit(operation, &approved, &result),
               mode == 2 ? AXON_OPERATION_EXECUTION : AXON_OPERATION_OK);
        assert(result.committed && result.package_phase_completed && result.completed_packages == 1);
        assert(result.execution_errors == (size_t)(mode == 2) && !result.diagnostics_incomplete);
        if(mode != 0) {
            int found = 0;
            for(alpm_list_t *item = result.diagnostics; item != NULL; item = item->next) {
                found |= strstr(item->data, "helper fixture exited with status 7") != NULL;
            }
            assert(found);
        }
        release(&operation);
    }
    commit_log = 0;

    size_t commit_allocations = 0;
    for(size_t index = 0; index <= commit_allocations; index++) {
        EXPECT(axon_operation_prepare(config, &request, &operation, &result), AXON_OPERATION_OK);
        approved = approval(operation);
        commit_log = 2;
        allocation_count = 0;
        allocation_failure = index;
        allocation_enabled = 1;
        EXPECT(axon_operation_commit(operation, &approved, &result), AXON_OPERATION_EXECUTION);
        allocation_enabled = 0;
        if(index == 0) {
            commit_allocations = allocation_count;
        }
        else {
            assert(result.diagnostics_incomplete);
        }
        assert(result.completed_packages == 1 && result.package_phase_completed && result.committed);
        release(&operation);
    }
    commit_log = 0;

    targets[0] = (axon_operation_target_t){.name = "essential"};
    request = (axon_operation_request_t){.kind = AXON_OPERATION_REMOVE, .targets = targets,
                                         .target_count = 1, .flags = safe_flags};
    EXPECT(axon_operation_prepare(config, &request, &operation, &result), AXON_OPERATION_BACKEND);
    assert(result.backend_error == ALPM_ERR_UNSATISFIED_DEPS && result.diagnostics != NULL);
    assert(operation == NULL && access("db/db.lck", F_OK) != 0);
    request.flags |= ALPM_TRANS_FLAG_CASCADE;
    EXPECT(axon_operation_prepare(config, &request, &operation, &result), AXON_OPERATION_OK);
    plan = axon_operation_plan(operation);
    assert(plan->count == 2 && plan->installed_delta == -30);
    assert(entry(plan, "essential")->held && entry(plan, "consumer")->held);
    assert(!entry(plan, "consumer")->explicit_target);
    approved = approval(operation);
    EXPECT(axon_operation_commit(operation, &approved, &result), AXON_OPERATION_POLICY);
    const char *protected[] = {"ess*", "consumer"};
    approved.protected_removals = protected;
    approved.protected_removal_count = 2;
    EXPECT(axon_operation_commit(operation, &approved, &result), AXON_OPERATION_POLICY);
    protected[0] = "essential";
    uint64_t old_id = approved.plan_id;
    EXPECT(axon_operation_cancel(operation, &result), AXON_OPERATION_OK);
    release(&operation);
    EXPECT(axon_operation_prepare(config, &request, &operation, &result), AXON_OPERATION_OK);
    assert(axon_operation_plan(operation)->id != old_id);
    EXPECT(axon_operation_commit(operation, &approved, &result), AXON_OPERATION_APPROVAL);
    release(&operation);
    installed(config, "essential", "1-1", ALPM_PKG_REASON_EXPLICIT);

    EXPECT(axon_operation_refresh(config, 1, &result), AXON_OPERATION_OK);
    assert(result.refreshed);
    axon_operation_target_t group_targets[] = {{.name = "tools"}, {.name = "helper"}};
    axon_operation_request_t group_request = {.kind = AXON_OPERATION_INSTALL,
        .targets = group_targets, .target_count = 2, .flags = safe_flags};
    EXPECT(axon_operation_prepare(config, &group_request, &operation, &result), AXON_OPERATION_OK);
    assert(axon_operation_plan(operation)->count == 2);
    assert(entry(axon_operation_plan(operation), "helper")->explicit_target);
    assert(entry(axon_operation_plan(operation), "application")->explicit_target);
    release(&operation);
    file = fopen("pacman.conf", "a");
    assert(file != NULL && fputs("[options]\nIgnorePkg = essential\n", file) >= 0);
    assert(fclose(file) == 0);
    axon_config_t *ignored_config = NULL;
    assert(axon_config_load("pacman.conf", &ignored_config, &config_error) == AXON_CONFIG_OK);
    request = (axon_operation_request_t){.kind = AXON_OPERATION_UPGRADE, .flags = safe_flags,
                                         .allow_replacements = 1};
    EXPECT(axon_operation_prepare(ignored_config, &request, &operation, &result), AXON_OPERATION_OK);
    assert(entry(axon_operation_plan(operation), "essential") == NULL);
    release(&operation);
    request = (axon_operation_request_t){.kind = AXON_OPERATION_INSTALL, .targets = targets,
                                         .target_count = 1, .flags = safe_flags};
    EXPECT(axon_operation_prepare(ignored_config, &request, &operation, &result), AXON_OPERATION_POLICY);
    assert(operation == NULL && result.backend_error == ALPM_ERR_PKG_IGNORED);
    axon_config_free(&ignored_config);
    file = fopen("pacman.conf", "a");
    assert(file != NULL && fputs("SigLevel = Required\n", file) >= 0);
    assert(fclose(file) == 0);
    axon_config_t *signed_config = NULL;
    assert(axon_config_load("pacman.conf", &signed_config, &config_error) == AXON_CONFIG_OK);
    targets[0] = (axon_operation_target_t){.name = locals[4].path};
    request.kind = AXON_OPERATION_FILE;
    EXPECT(axon_operation_prepare(signed_config, &request, &operation, &result), AXON_OPERATION_BACKEND);
    assert(operation == NULL && result.backend_error == ALPM_ERR_PKG_MISSING_SIG);
    axon_config_free(&signed_config);
    targets[0] = (axon_operation_target_t){.name = "application"};
    request = (axon_operation_request_t){.kind = AXON_OPERATION_INSTALL, .targets = targets,
                                         .target_count = 1, .flags = safe_flags};
    EXPECT(axon_operation_prepare(config, &request, &operation, &result), AXON_OPERATION_OK);
    plan = axon_operation_plan(operation);
    assert(plan->count == 2 && plan->installed_delta == 20 && plan->download_bytes > 0);
    assert(entry(plan, "helper")->reason == ALPM_PKG_REASON_DEPEND);
    assert(!entry(plan, "helper")->explicit_target);
    approved = approval(operation);
    authorized = 0;
    EXPECT(axon_operation_commit(operation, &approved, &result), AXON_OPERATION_PRIVILEGE);
    authorized = 1;
    EXPECT(axon_operation_commit(operation, &approved, &result), AXON_OPERATION_OK);
    release(&operation);
    installed(config, "helper", "1-1", ALPM_PKG_REASON_DEPEND);
    targets[0].name = "ignored";
    EXPECT(axon_operation_prepare(config, &request, &operation, &result), AXON_OPERATION_POLICY);
    assert(result.backend_error == ALPM_ERR_PKG_IGNORED && operation == NULL);

    request = (axon_operation_request_t){.kind = AXON_OPERATION_UPGRADE, .flags = safe_flags};
    EXPECT(axon_operation_prepare(config, &request, &operation, &result), AXON_OPERATION_QUESTION);
    {
        struct observer replacement = {.answer = AXON_DECISION_DECLINE};
        request.callbacks = (axon_operation_callbacks_t){.question = decide, .context = &replacement};
        EXPECT(axon_operation_prepare(config, &request, &operation, &result), AXON_OPERATION_OK);
        assert(replacement.questions && entry(axon_operation_plan(operation), "legacy") == NULL);
        release(&operation);
        replacement.answer = AXON_DECISION_UNANSWERED;
        EXPECT(axon_operation_prepare(config, &request, &operation, &result), AXON_OPERATION_QUESTION);
        replacement.answer = AXON_DECISION_CANCEL;
        EXPECT(axon_operation_prepare(config, &request, &operation, &result), AXON_OPERATION_CANCELLED);
        assert(result.cancelled && operation == NULL);
        replacement.answer = AXON_DECISION_ACCEPT;
        EXPECT(axon_operation_prepare(config, &request, &operation, &result), AXON_OPERATION_OK);
        assert(entry(axon_operation_plan(operation), "legacy") != NULL);
        release(&operation);
        request.callbacks = (axon_operation_callbacks_t){0};
    }
    request.allow_replacements = 1;
    EXPECT(axon_operation_prepare(config, &request, &operation, &result), AXON_OPERATION_OK);
    plan = axon_operation_plan(operation);
    assert(entry(plan, "essential")->kind == AXON_CHANGE_UPGRADE);
    assert(strcmp(entry(plan, "essential")->new_version, "2-1") == 0);
    assert(strcmp(entry(plan, "essential")->old_version, "1-1") == 0);
    assert(entry(plan, "legacy")->kind == AXON_CHANGE_REPLACE && entry(plan, "legacy")->held);
    assert(strcmp(entry(plan, "legacy")->replaced_by, "substitute") == 0);
    assert(plan->installed_delta == 6);
    approved = approval(operation);
    EXPECT(axon_operation_commit(operation, &approved, &result), AXON_OPERATION_POLICY);
    const char *legacy[] = {"legacy"};
    approved.protected_removals = legacy;
    approved.protected_removal_count = 1;
    EXPECT(axon_operation_commit(operation, &approved, &result), AXON_OPERATION_OK);
    release(&operation);
    installed(config, "essential", "2-1", ALPM_PKG_REASON_EXPLICIT);
    EXPECT(axon_operation_prepare(config, &request, &operation, &result), AXON_OPERATION_OK);
    assert(axon_operation_plan(operation)->count == 0);
    approved = approval(operation);
    EXPECT(axon_operation_commit(operation, &approved, &result), AXON_OPERATION_OK);
    assert(!result.may_have_changes);
    release(&operation);

    targets[0] = (axon_operation_target_t){.name = "essential"};
    request = (axon_operation_request_t){.kind = AXON_OPERATION_INSTALL, .targets = targets,
                                         .target_count = 1, .flags = safe_flags | ALPM_TRANS_FLAG_NEEDED};
    EXPECT(axon_operation_prepare(config, &request, &operation, &result), AXON_OPERATION_OK);
    assert(axon_operation_plan(operation)->count == 0);
    release(&operation);
    targets[0].repository = "second";
    EXPECT(axon_operation_prepare(config, &request, &operation, &result), AXON_OPERATION_OK);
    assert(strcmp(entry(axon_operation_plan(operation), "essential")->new_version, "3-1") == 0);
    release(&operation);

    targets[0] = (axon_operation_target_t){.name = locals[4].path};
    request = (axon_operation_request_t){.kind = AXON_OPERATION_FILE, .targets = targets,
                                         .target_count = 1, .flags = safe_flags};
    EXPECT(axon_operation_prepare(config, &request, &operation, &result), AXON_OPERATION_POLICY);
    request.allow_downgrade = 1;
    EXPECT(axon_operation_prepare(config, &request, &operation, &result), AXON_OPERATION_OK);
    assert(entry(axon_operation_plan(operation), "versioned")->kind == AXON_CHANGE_DOWNGRADE);
    approved = approval(operation);
    EXPECT(axon_operation_commit(operation, &approved, &result), AXON_OPERATION_APPROVAL);
    approved.approve_downgrades = 1;
    EXPECT(axon_operation_commit(operation, &approved, &result), AXON_OPERATION_OK);
    release(&operation);
    installed(config, "versioned", "1-1", ALPM_PKG_REASON_EXPLICIT);
    targets[0].name = locals[5].path;
    EXPECT(axon_operation_prepare(config, &request, &operation, &result), AXON_OPERATION_BACKEND);
    assert(result.backend_error == ALPM_ERR_PKG_INVALID_ARCH);
    request.flags |= ALPM_TRANS_FLAG_NODEPS;
    EXPECT(axon_operation_prepare(config, &request, &operation, &result), AXON_OPERATION_ARGUMENT);
    assert(access("db/db.lck", F_OK) != 0);

    file = fopen("root/busy.txt", "w");
    assert(file != NULL && fputs("existing content\n", file) >= 0);
    assert(fclose(file) == 0);
    targets[0] = (axon_operation_target_t){.name = locals[6].path};
    request = (axon_operation_request_t){.kind = AXON_OPERATION_FILE, .targets = targets,
                                         .target_count = 1, .flags = safe_flags};
    EXPECT(axon_operation_prepare(config, &request, &operation, &result), AXON_OPERATION_OK);
    approved = approval(operation);
    EXPECT(axon_operation_commit(operation, &approved, &result), AXON_OPERATION_BACKEND);
    assert(result.backend_error == ALPM_ERR_FILE_CONFLICTS && !result.committed && result.may_have_changes);
    assert(result.diagnostics != NULL && access("db/db.lck", F_OK) != 0);
    release(&operation);

    struct observer observer = {.answer = AXON_DECISION_ACCEPT, .operation = &operation};
    axon_operation_callbacks_t callbacks = {.question = decide, .event = observe_event,
        .progress = observe_progress, .log = observe_log, .download = observe_download,
        .context = &observer, .cancel_requested = &stop};
    targets[0] = (axon_operation_target_t){.name = "ignored"};
    request = (axon_operation_request_t){.kind = AXON_OPERATION_INSTALL, .targets = targets,
        .target_count = 1, .flags = safe_flags, .callbacks = callbacks};
    observer.answer = AXON_DECISION_DECLINE;
    EXPECT(axon_operation_prepare(config, &request, &operation, &result), AXON_OPERATION_POLICY);
    observer.answer = AXON_DECISION_UNANSWERED;
    EXPECT(axon_operation_prepare(config, &request, &operation, &result), AXON_OPERATION_POLICY);
    observer.answer = AXON_DECISION_ACCEPT;
    EXPECT(axon_operation_prepare(config, &request, &operation, &result), AXON_OPERATION_OK);
    assert(observer.questions == 3 && observer.events && observer.logs);
    approved = approval(operation);
    observer.cancel_download = 1;
    EXPECT(axon_operation_commit(operation, &approved, &result), AXON_OPERATION_CANCELLED);
    assert(result.cancelled && !result.committed && observer.downloads);
    release(&operation);
    stop = observer.cancel_download = 0;
    axon_alpm_t *check = NULL;
    alpm_pkg_t *package = NULL;
    assert(axon_config_open(config, &check, &config_error) == AXON_CONFIG_OK);
    assert(axon_alpm_local_package(check, "ignored", &package) == ALPM_ERR_PKG_NOT_FOUND);
    assert(axon_alpm_release(&check) == 0);

    observer.cancel_download = 1;
    EXPECT(axon_operation_refresh_with_callbacks(config, 1, &callbacks, &result), AXON_OPERATION_CANCELLED);
    assert(result.cancelled && result.may_have_changes);
    stop = observer.cancel_download = 0;

    targets[0] = (axon_operation_target_t){.name = locals[7].path};
    targets[1] = (axon_operation_target_t){.name = locals[8].path};
    request.kind = AXON_OPERATION_FILE;
    request.target_count = 2;
    stop = 1;
    EXPECT(axon_operation_prepare(config, &request, &operation, &result), AXON_OPERATION_CANCELLED);
    assert(!result.may_have_changes && operation == NULL);
    EXPECT(axon_operation_refresh_with_callbacks(config, 1, &callbacks, &result), AXON_OPERATION_CANCELLED);
    assert(!result.may_have_changes);
    stop = 0;
    observer.cancel_event = ALPM_EVENT_RESOLVEDEPS_START;
    EXPECT(axon_operation_prepare(config, &request, &operation, &result), AXON_OPERATION_CANCELLED);
    assert(operation == NULL && result.cancelled && access("db/db.lck", F_OK) != 0);
    stop = 0;
    observer.cancel_event = 0;
    EXPECT(axon_operation_prepare(config, &request, &operation, &result), AXON_OPERATION_OK);
    approved = approval(operation);
    stop = 1;
    EXPECT(axon_operation_commit(operation, &approved, &result), AXON_OPERATION_CANCELLED);
    assert(!result.may_have_changes);
    release(&operation);
    stop = 0;
    EXPECT(axon_operation_prepare(config, &request, &operation, &result), AXON_OPERATION_OK);
    approved = approval(operation);
    observer.cancel_event = ALPM_EVENT_PACKAGE_OPERATION_DONE;
    EXPECT(axon_operation_commit(operation, &approved, &result), AXON_OPERATION_CANCELLED);
    assert(result.cancelled && result.may_have_changes && !result.committed && observer.progress);
    assert(result.completed_packages == 1);
    release(&operation);
    stop = 0;
    observer.cancel_event = 0;
    installed(config, "cancel-first", "1-1", ALPM_PKG_REASON_EXPLICIT);
    assert(axon_config_open(config, &check, &config_error) == AXON_CONFIG_OK);
    assert(axon_alpm_local_package(check, "cancel-second", &package) == ALPM_ERR_PKG_NOT_FOUND);
    assert(axon_alpm_release(&check) == 0);

    request.target_count = 1;
    EXPECT(axon_operation_prepare(config, &request, &operation, &result), AXON_OPERATION_OK);
    approved = approval(operation);
    cleanup_signal = 1;
    EXPECT(axon_operation_commit(operation, &approved, &result), AXON_OPERATION_CANCELLED);
    assert(result.cancelled && result.package_phase_completed && result.completed_packages == 1);
    assert(result.cleanup_error == ALPM_ERR_OK && access("db/db.lck", F_OK) != 0);
    stop = 0;
    release(&operation);
    request.target_count = 2;

    EXPECT(axon_operation_prepare(config, &request, &operation, &result), AXON_OPERATION_OK);
    pid_t child = fork();
    assert(child >= 0);
    if(child == 0) {
        axon_operation_t *competitor = NULL;
        axon_operation_result_t competing = {0};
        assert(axon_operation_prepare(config, &request, &competitor, &competing) == AXON_OPERATION_BACKEND);
        assert(competing.backend_error == ALPM_ERR_HANDLE_LOCK && competitor == NULL);
        assert(access("db/db.lck", F_OK) == 0);
        axon_operation_result_clear(&competing);
        _exit(0);
    }
    int child_status;
    assert(waitpid(child, &child_status, 0) == child && WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    assert(access("db/db.lck", F_OK) == 0);
    fail_cleanup = 1;
    EXPECT(axon_operation_cancel(operation, &result), AXON_OPERATION_BACKEND);
    assert(result.cleanup_error == ALPM_ERR_SYSTEM && result.backend_error == ALPM_ERR_OK);
    release(&operation);

    EXPECT(axon_operation_prepare(config, &request, &operation, &result), AXON_OPERATION_OK);
    assert(unlink("db/db.lck") == 0 && mkdir("db/db.lck", 0700) == 0);
    EXPECT(axon_operation_cancel(operation, &result), AXON_OPERATION_BACKEND);
    assert(result.cleanup_error == ALPM_ERR_SYSTEM && result.diagnostics != NULL);
    assert(access("db/db.lck", F_OK) == 0);
    assert(rmdir("db/db.lck") == 0);
    release(&operation);
    if(getuid() != 0) {
        EXPECT(axon_operation_prepare(config, &request, &operation, &result), AXON_OPERATION_OK);
        assert(chmod("db", 0500) == 0);
        EXPECT(axon_operation_cancel(operation, &result), AXON_OPERATION_BACKEND);
        assert(result.cleanup_error == ALPM_ERR_SYSTEM && access("db/db.lck", F_OK) == 0);
        assert(chmod("db", 0700) == 0 && unlink("db/db.lck") == 0);
        release(&operation);
    }

    targets[0].name = locals[5].path;
    request.target_count = 1;
    fail_cleanup = 1;
    EXPECT(axon_operation_prepare(config, &request, &operation, &result), AXON_OPERATION_BACKEND);
    assert(result.backend_error == ALPM_ERR_PKG_INVALID_ARCH && result.cleanup_error == ALPM_ERR_SYSTEM);
    assert(operation == NULL && access("db/db.lck", F_OK) != 0);
    targets[0].name = locals[6].path;
    EXPECT(axon_operation_prepare(config, &request, &operation, &result), AXON_OPERATION_OK);
    approved = approval(operation);
    fail_cleanup = 1;
    EXPECT(axon_operation_commit(operation, &approved, &result), AXON_OPERATION_BACKEND);
    assert(result.backend_error == ALPM_ERR_FILE_CONFLICTS && result.cleanup_error == ALPM_ERR_SYSTEM);
    release(&operation);

    targets[0] = (axon_operation_target_t){.name = "essential"};
    request = (axon_operation_request_t){.kind = AXON_OPERATION_REMOVE, .targets = targets,
                                         .target_count = 1, .flags = safe_flags | ALPM_TRANS_FLAG_CASCADE};
    EXPECT(axon_operation_prepare(config, &request, &operation, &result), AXON_OPERATION_OK);
    approved = approval(operation);
    approved.protected_removals = protected;
    approved.protected_removal_count = 2;
    axon_config_free(&config);
    EXPECT(axon_operation_commit(operation, &approved, &result), AXON_OPERATION_OK);
    release(&operation);
    axon_operation_result_clear(&result);
    assert(chdir(previous) == 0);
    free(previous);
    assert(nftw(temporary, remove_entry, 16, FTW_DEPTH | FTW_PHYS) == 0);
    puts("Operation workflows, plans, and package policy checks passed.");
    return(0);
}
