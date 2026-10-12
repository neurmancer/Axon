#define _XOPEN_SOURCE 700
#include "maintenance.h"

#include <archive.h>
#include <archive_entry.h>
#include <assert.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char workspace[] = "/tmp/axon-maintenance-XXXXXX";
static axon_operation_result_t result;
static int authorized = 1;
static volatile sig_atomic_t cancel;
static size_t previews;
static int messages;

axon_privilege_result_t __wrap_axon_privilege_check(int root)
{
    return(root && !authorized ? AXON_PRIVILEGE_ROOT_REQUIRED : AXON_PRIVILEGE_OK);
}

static void write_file(const char *path, const char *text)
{
    FILE *file = fopen(path, "w");
    assert(file != NULL && fputs(text, file) >= 0 && fclose(file) == 0);
}

static void archive_file(const char *path, const char *name, const char *text)
{
    struct archive *archive = archive_write_new();
    struct archive_entry *entry = archive_entry_new();
    assert(archive && entry);
    assert(archive_write_set_format_pax_restricted(archive) == ARCHIVE_OK);
    assert(archive_write_open_filename(archive, path) == ARCHIVE_OK);
    archive_entry_set_pathname(entry, name);
    archive_entry_set_size(entry, (la_int64_t)strlen(text));
    archive_entry_set_filetype(entry, AE_IFREG);
    archive_entry_set_perm(entry, 0644);
    archive_entry_set_uid(entry, getuid());
    archive_entry_set_gid(entry, getgid());
    assert(archive_write_header(archive, entry) == ARCHIVE_OK);
    assert(archive_write_data(archive, text, strlen(text)) == (la_ssize_t)strlen(text));
    archive_entry_free(entry);
    assert(archive_write_close(archive) == ARCHIVE_OK);
    assert(archive_write_free(archive) == ARCHIVE_OK);
}

static void package(const char *path, const char *name, const char *version)
{
    char text[1024];
    snprintf(text, sizeof(text), "pkgname = %s\npkgver = %s\narch = any\nsize = 1\npkgdesc = Maintenance fixture\n", name, version);
    archive_file(path, ".PKGINFO", text);
}

static void local(const char *name, int reason, const char *extra)
{
    char path[256], text[1024];
    snprintf(path, sizeof(path), "db/local/%s-1-1", name);
    assert(mkdir(path, 0700) == 0);
    snprintf(path, sizeof(path), "db/local/%s-1-1/desc", name);
    snprintf(text, sizeof(text), "%%NAME%%\n%s\n\n%%VERSION%%\n1-1\n\n%%REASON%%\n%d\n\n%%DESC%%\nFixture\n\n%s", name, reason, extra);
    write_file(path, text);
    snprintf(path, sizeof(path), "db/local/%s-1-1/files", name);
    write_file(path, "%FILES%\n\n");
}

static axon_config_t *configuration(const char *method)
{
    char text[4096];
    snprintf(text, sizeof(text), "[options]\nRootDir = %s/root\nDBPath = %s/db\n"
             "CacheDir = %s/cache\nCacheDir = %s/cache\nLogFile = %s/log\nGPGDir = %s/keys\n"
             "SigLevel = Never\nArchitecture = any\nCleanMethod = %s\n[test]\n",
             workspace, workspace, workspace, workspace, workspace, workspace, method);
    write_file("pacman.conf", text);
    axon_config_t *config = NULL;
    axon_config_error_t error;
    assert(axon_config_load("pacman.conf", &config, &error) == AXON_CONFIG_OK);
    return(config);
}

static axon_alpm_t *open_core(const axon_config_t *config)
{
    axon_alpm_t *core = NULL;
    axon_config_error_t error;
    assert(axon_config_open(config, &core, &error) == AXON_CONFIG_OK);
    return(core);
}

static void reason(const axon_config_t *config, const char *name, alpm_pkgreason_t expected)
{
    axon_alpm_t *core = open_core(config);
    alpm_pkg_t *package = NULL;
    assert(axon_alpm_local_package(core, name, &package) == ALPM_ERR_OK);
    assert(alpm_pkg_get_reason(package) == expected);
    assert(axon_alpm_release(&core) == 0);
}

static void message(void *context, const char *text)
{
    (void)context;
    assert(text && *text);
    messages++;
}

static int approve(void *context, const alpm_list_t *paths)
{
    const char *mode = context;
    previews = alpm_list_count(paths);
    assert(access("db/db.lck", F_OK) == 0);
    if(strcmp(mode, "decline") == 0) {
        return(0);
    }
    if(strcmp(mode, "cancel") == 0) {
        cancel = 1;
    }
    if(strcmp(mode, "privilege") == 0) {
        authorized = 0;
    }
    if(strcmp(mode, "replace") == 0) {
        assert(paths != NULL);
        assert(rename(paths->data, "original") == 0);
        write_file(paths->data, "replacement must survive");
    }
    return(1);
}

static void clean(axon_config_t *config, const char *mode, axon_operation_status_t expected)
{
    axon_operation_status_t status = axon_maintenance_clean(config, approve, (void *)mode, &cancel, &result);
    if(status != expected) {
        fprintf(stderr, "clean: status=%d expected=%d backend=%s\n", status, expected, alpm_strerror(result.backend_error));
        abort();
    }
    assert(access("db/db.lck", F_OK) != 0);
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
    char *previous = getcwd(NULL, 0);
    assert(previous && mkdtemp(workspace) && chdir(workspace) == 0);
    const char *directories[] = {"root", "db", "db/local", "db/sync", "cache", "keys"};
    for(size_t i = 0; i < sizeof(directories) / sizeof(directories[0]); i++) {
        assert(mkdir(directories[i], 0700) == 0);
    }
    write_file("db/local/ALPM_DB_VERSION", "9\n");
    local("installed", 0, "");
    local("orphan", 1, "");
    local("needed", 1, "");
    local("optional", 1, "");
    local("consumer", 0, "%DEPENDS%\nneeded\n\n%OPTDEPENDS%\noptional: useful\n\n");
    archive_file("db/sync/test.db", "current-2-1/desc", "%NAME%\ncurrent\n\n%VERSION%\n2-1\n\n%ARCH%\nany\n\n");
    axon_config_t *config = configuration("KeepInstalled KeepCurrent");
    axon_alpm_t *core = open_core(config);
    const char *names[] = {"installed", "orphan", "needed", "optional"};
    for(size_t i = 0; i < 4; i++) {
        alpm_pkg_t *pkg = NULL;
        assert(axon_alpm_local_package(core, names[i], &pkg) == ALPM_ERR_OK);
        assert(axon_package_orphan(pkg) == (i == 1));
    }
    assert(axon_maintenance_check(core, message, NULL) == ALPM_ERR_OK);
    write_file("db/db.lck", "someone else's lock\n");
    assert(axon_maintenance_check(core, message, NULL) == ALPM_ERR_HANDLE_LOCK && messages);
    assert(axon_alpm_release(&core) == 0 && access("db/db.lck", F_OK) == 0);
    assert(unlink("db/db.lck") == 0);
    const char *change[] = {"orphan", "missing"};
    assert(axon_maintenance_reason(config, 2, change, ALPM_PKG_REASON_EXPLICIT, &cancel, &result) == AXON_OPERATION_BACKEND);
    reason(config, "orphan", ALPM_PKG_REASON_DEPEND);
    authorized = 0;
    assert(axon_maintenance_reason(config, 1, change, ALPM_PKG_REASON_EXPLICIT, &cancel, &result) == AXON_OPERATION_PRIVILEGE);
    clean(config, "yes", AXON_OPERATION_PRIVILEGE);
    authorized = 1;
    cancel = 1;
    assert(axon_maintenance_reason(config, 1, change, ALPM_PKG_REASON_EXPLICIT, &cancel, &result) == AXON_OPERATION_CANCELLED);
    assert(!result.may_have_changes);
    cancel = 0;
    assert(axon_maintenance_reason(config, 1, change, ALPM_PKG_REASON_EXPLICIT, &cancel, &result) == AXON_OPERATION_OK);
    reason(config, "orphan", ALPM_PKG_REASON_EXPLICIT);
    assert(result.diagnostics && result.may_have_changes);
    assert(axon_maintenance_reason(config, 0, change, ALPM_PKG_REASON_EXPLICIT, &cancel, &result) == AXON_OPERATION_ARGUMENT);

    package("cache/installed.pkg.tar", "installed", "1-1");
    package("cache/old.pkg.tar", "installed", "0-1");
    package("cache/current.pkg.tar", "current", "2-1");
    write_file("cache/old.pkg.tar.sig", "signature");
    write_file("cache/unknown.pkg.tar", "invalid archive");
    write_file("cache/incomplete.pkg.tar.part", "partial");
    assert(symlink("old.pkg.tar", "cache/link.pkg.tar") == 0);
    clean(config, "decline", AXON_OPERATION_CANCELLED);
    assert(previews == 2 && !result.may_have_changes && access("cache/old.pkg.tar", F_OK) == 0);
    clean(config, "cancel", AXON_OPERATION_CANCELLED);
    assert(!result.may_have_changes);
    cancel = 0;
    clean(config, "privilege", AXON_OPERATION_PRIVILEGE);
    authorized = 1;
    clean(config, "yes", AXON_OPERATION_OK);
    assert(previews == 2 && access("cache/old.pkg.tar", F_OK) != 0 && access("cache/old.pkg.tar.sig", F_OK) != 0);
    assert(access("cache/installed.pkg.tar", F_OK) == 0 && access("cache/current.pkg.tar", F_OK) == 0);
    assert(access("cache/unknown.pkg.tar", F_OK) == 0 && access("cache/incomplete.pkg.tar.part", F_OK) == 0);
    struct stat info;
    assert(lstat("cache/link.pkg.tar", &info) == 0 && S_ISLNK(info.st_mode));
    clean(config, "yes", AXON_OPERATION_OK);
    assert(previews == 0);
    package("cache/race.pkg.tar", "race", "1-1");
    clean(config, "replace", AXON_OPERATION_BACKEND);
    assert(result.backend_error == ALPM_ERR_PKG_INVALID && !result.may_have_changes);
    assert(access("cache/race.pkg.tar", F_OK) == 0 && unlink("cache/race.pkg.tar") == 0 && unlink("original") == 0);
    assert(chmod("cache", 0777) == 0);
    clean(config, "yes", AXON_OPERATION_BACKEND);
    assert(result.backend_error == ALPM_ERR_BADPERMS);
    assert(chmod("cache", 0700) == 0);

    package("cache/widget.part-tools.pkg.tar", "widget.part-tools", "1-1");
    package("cache/widget.sig-tools.pkg.tar", "widget.sig-tools", "1-1");
    clean(config, "yes", AXON_OPERATION_OK);
    assert(previews == 2);
    assert(access("cache/widget.part-tools.pkg.tar", F_OK) != 0 && access("cache/widget.sig-tools.pkg.tar", F_OK) != 0);
    axon_config_free(&config);
    config = configuration("KeepInstalled");
    clean(config, "yes", AXON_OPERATION_OK);
    assert(previews == 1 && access("cache/current.pkg.tar", F_OK) != 0);
    axon_config_free(&config);
    config = configuration("KeepCurrent");
    clean(config, "yes", AXON_OPERATION_OK);
    assert(previews == 1 && access("cache/installed.pkg.tar", F_OK) != 0);
    write_file("db/sync/test.db", "not a database archive");
    core = open_core(config);
    assert(axon_maintenance_check(core, message, NULL) == ALPM_ERR_DB_INVALID);
    assert(axon_alpm_release(&core) == 0);
    clean(config, "yes", AXON_OPERATION_BACKEND);
    local("broken", 0, "%DEPENDS%\nnonexistent-dependency\n\n");
    archive_file("db/sync/test.db", "current-2-1/desc", "%NAME%\ncurrent\n\n%VERSION%\n2-1\n\n");
    core = open_core(config);
    assert(axon_maintenance_check(core, message, NULL) == ALPM_ERR_UNSATISFIED_DEPS);
    assert(axon_alpm_release(&core) == 0);
    axon_config_free(&config);
    axon_operation_result_clear(&result);
    assert(chdir(previous) == 0);
    free(previous);
    assert(nftw(workspace, remove_entry, 16, FTW_DEPTH | FTW_PHYS) == 0);
    puts("Maintenance policy, cache identity, cancellation, reasons, and database regressions passed.");
    return(0);
}
