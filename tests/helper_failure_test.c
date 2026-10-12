#define _GNU_SOURCE
#include "operation.h"

#include <archive.h>
#include <archive_entry.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "root_fixture.h"

static int contains(const axon_operation_result_t *result, const char *text)
{
    for(alpm_list_t *item = result->diagnostics; item != NULL; item = item->next) {
        if(strstr(item->data, text) != NULL) {
            return(1);
        }
    }
    return(0);
}

static void backend(int scenario)
{
    axon_config_t *config = NULL;
    axon_config_error_t error = {0};
    assert(axon_config_load("/etc/pacman.conf", &config, &error) == AXON_CONFIG_OK);
    axon_operation_target_t target = {.name = "/fixture.pkg.tar"};
    axon_operation_request_t request = {.kind = AXON_OPERATION_FILE, .targets = &target, .target_count = 1};
    axon_operation_t *operation = NULL;
    axon_operation_result_t result = {0}, cleanup = {0};
    assert(axon_operation_prepare(config, &request, &operation, &result) == AXON_OPERATION_OK);
    axon_operation_approval_t approval = {.confirm = 1, .plan_id = axon_operation_plan(operation)->id};
    axon_operation_status_t status = axon_operation_commit(operation, &approval, &result);
    assert(status == (scenario == 0 ? AXON_OPERATION_BACKEND : scenario == 5 ? AXON_OPERATION_OK : AXON_OPERATION_EXECUTION));
    assert(result.committed == (scenario != 0));
    assert(result.package_phase_completed == (scenario != 0));
    assert(result.completed_packages == (size_t)(scenario != 0));
    assert((result.execution_errors != 0) == (scenario != 5));
    assert(result.cleanup_error == ALPM_ERR_OK && !result.diagnostics_incomplete);
    assert(contains(&result, scenario == 2 ? "scriptlet-diagnostic" : "helper-diagnostic"));
    assert(axon_operation_free(&operation, &cleanup) == AXON_OPERATION_OK);
    assert(result.diagnostics != NULL);
    axon_operation_result_clear(&cleanup);
    axon_operation_result_clear(&result);
    axon_config_free(&config);
}

static void cli(int scenario)
{
    int input[2], output[2];
    assert(pipe(input) == 0 && pipe(output) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if(child == 0) {
        assert(dup2(input[0], STDIN_FILENO) >= 0);
        assert(dup2(output[1], STDOUT_FILENO) >= 0 && dup2(output[1], STDERR_FILENO) >= 0);
        close(input[0]);
        close(input[1]);
        close(output[0]);
        close(output[1]);
        execl("/axon", "axon", "-U", "/fixture.pkg.tar", (char *)NULL);
        _exit(127);
    }
    close(input[0]);
    close(output[1]);
    assert(write(input[1], "y\n", 2) == 2);
    close(input[1]);
    char text[65536];
    size_t used = 0;
    ssize_t count;
    while((count = read(output[0], text + used, sizeof(text) - used - 1)) > 0) {
        used += (size_t)count;
        assert(used < sizeof(text) - 1);
    }
    assert(count == 0);
    text[used] = '\0';
    close(output[0]);
    int status;
    assert(waitpid(child, &status, 0) == child);
    if(!WIFEXITED(status)) {
        fprintf(stderr, "CLI terminated (status %d):\n%s", status, text);
        abort();
    }
    if(WEXITSTATUS(status) != (scenario == 5 ? 0 : 1)) {
        fprintf(stderr, "%s", text);
        abort();
    }
    assert((strstr(text, "Transaction completed.") != NULL) == (scenario == 5));
    assert((strstr(text, "package processing completed (1 package operations)") != NULL)
        == (scenario != 0 && scenario != 5));
    assert(strstr(text, scenario == 2 ? "scriptlet-diagnostic" : "helper-diagnostic") != NULL);
}

static int remove_path(const char *path, const struct stat *info, int type, struct FTW *walk)
{
    (void)info;
    (void)type;
    (void)walk;
    return(remove(path));
}

static void expect_output(int terminal, char *text, size_t capacity, const char *expected)
{
    size_t used = 0;
    text[0] = '\0';
    while(strstr(text, expected) == NULL) {
        struct pollfd poller = {.fd = terminal, .events = POLLIN};
        assert(poll(&poller, 1, 5000) > 0);
        ssize_t count = read(terminal, text + used, capacity - used - 1);
        assert(count > 0);
        used += (size_t)count;
        assert(used < capacity - 1);
        text[used] = '\0';
    }
}

static void ui(int scenario)
{
    int terminal;
    struct winsize size = {.ws_row = 24, .ws_col = 100};
    pid_t child = forkpty(&terminal, NULL, NULL, &size);
    assert(child >= 0);
    if(child == 0) {
        assert(setenv("TERM", "xterm", 1) == 0);
        execl("/axon", "axon", "--ui", (char *)NULL);
        _exit(127);
    }
    char text[65536];
    expect_output(terminal, text, sizeof(text), "0 packages");
    assert(write(terminal, "f", 1) == 1);
    expect_output(terminal, text, sizeof(text), "Archive path:");
    const char *path = "/fixture.pkg.tar\n";
    assert(write(terminal, path, strlen(path)) == (ssize_t)strlen(path));
    expect_output(terminal, text, sizeof(text), "Commit this transaction?");
    assert(write(terminal, "y\n", 2) == 2);
    expect_output(terminal, text, sizeof(text), "Press Enter to return");
    assert((strstr(text, "Transaction completed.") != NULL) == (scenario == 5));
    assert((strstr(text, "package processing completed (1 package operations)") != NULL)
        == (scenario != 0 && scenario != 5));
    assert(strstr(text, scenario == 2 ? "scriptlet-diagnostic" : "helper-diagnostic") != NULL);
    assert(write(terminal, "\n", 1) == 1);
    expect_output(terminal, text, sizeof(text), scenario == 0 ? "0 packages" : "fixture");
    assert(write(terminal, "q", 1) == 1);
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status));
    assert(WEXITSTATUS(status) == (scenario == 5 ? 0 : 1));
    close(terminal);
}

static void compatibility_repository(void)
{
    assert(mkdir("repo", 0755) == 0);
    struct archive *database = archive_write_new();
    assert(database && archive_write_set_format_pax_restricted(database) == ARCHIVE_OK);
    assert(archive_write_open_filename(database, "repo/test.db") == ARCHIVE_OK);
    const char *names[] = {"app", "dep", "other"};
    for(size_t index = 0; index < 3; index++) {
        const char *name = names[index];
        char path[128], text[1024];
        snprintf(path, sizeof(path), "repo/%s.pkg.tar", name);
        struct archive *package = archive_write_new();
        assert(package && archive_write_set_format_pax_restricted(package) == ARCHIVE_OK);
        assert(archive_write_open_filename(package, path) == ARCHIVE_OK);
        snprintf(text, sizeof(text), "pkgname = %s\npkgver = 1-1\narch = any\nsize = 7\npkgdesc = compatibility\n%s%s",
                 name, index != 1 ? "depend = dep\ngroup = tools\n" : "",
                 index == 2 ? "backup = etc/other.conf\n" : "");
        entry(package, ".PKGINFO", text);
        char payload[128];
        snprintf(payload, sizeof(payload), "usr/share/%s", name);
        entry(package, payload, "payload");
        if(index == 2) {
            entry(package, "etc/other.conf", "original");
        }
        assert(archive_write_close(package) == ARCHIVE_OK && archive_write_free(package) == ARCHIVE_OK);
        struct stat info;
        assert(stat(path, &info) == 0);
        char *hash = alpm_compute_sha256sum(path);
        assert(hash);
        snprintf(text, sizeof(text), "%%NAME%%\n%s\n\n%%VERSION%%\n1-1\n\n%%ARCH%%\nany\n\n"
                 "%%DESC%%\ncompatibility\n\n%%FILENAME%%\n%s.pkg.tar\n\n%%CSIZE%%\n%jd\n\n"
                 "%%ISIZE%%\n7\n\n%%SHA256SUM%%\n%s\n\n%s", name, name, (intmax_t)info.st_size, hash,
                 index != 1 ? "%DEPENDS%\ndep\n\n%GROUPS%\ntools\n\n" : "");
        free(hash);
        snprintf(path, sizeof(path), "%s-1-1/desc", name);
        entry(database, path, text);
    }
    assert(archive_write_close(database) == ARCHIVE_OK && archive_write_free(database) == ARCHIVE_OK);
    FILE *config = fopen("etc/pacman.conf", "a");
    assert(config && fputs("[test]\nServer = file:///repo\n", config) >= 0 && fclose(config) == 0);
}

static int command(const char *program, const char *first, const char *second, const char *third)
{
    int input[2], output[2];
    assert(pipe(input) == 0 && pipe(output) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if(child == 0) {
        assert(dup2(input[0], STDIN_FILENO) >= 0);
        assert(dup2(output[1], STDOUT_FILENO) >= 0 && dup2(output[1], STDERR_FILENO) >= 0);
        close(input[0]);
        close(input[1]);
        close(output[0]);
        close(output[1]);
        char *args[8] = {(char *)program, (char *)first};
        size_t next = 2;
        if(second) {
            args[next++] = (char *)second;
        }
        if(third) {
            args[next++] = (char *)third;
        }
        if(strcmp(program, "/pacman") == 0) {
            args[next++] = "--noconfirm";
        }
        args[next] = NULL;
        execv(program, args);
        _exit(127);
    }
    close(input[0]);
    close(output[1]);
    assert(write(input[1], "y\ny\n", 4) == 4);
    close(input[1]);
    char text[32768];
    size_t used = 0;
    ssize_t size;
    while((size = read(output[0], text + used, sizeof(text) - used - 1)) > 0) {
        used += (size_t)size;
        assert(used < sizeof(text) - 1);
    }
    assert(size == 0);
    text[used] = '\0';
    close(output[0]);
    int status;
    assert(waitpid(child, &status, 0) == child);
    if(!WIFEXITED(status)) {
        fprintf(stderr, "%s %s terminated (status %d):\n%s", program, first, status, text);
        abort();
    }
    if(WEXITSTATUS(status) != 0 && !(strcmp(first, "-R") == 0 && second && strcmp(second, "dep") == 0)) {
        fprintf(stderr, "%s %s %s failed:\n%s", program, first, second ? second : "", text);
        abort();
    }
    return(WEXITSTATUS(status));
}

static void mutation_compatibility(void)
{
    const char *steps[][3] = {
        {"-Sy", NULL, NULL}, {"-S", "tools", NULL}, {"-S", "--needed", "tools"},
        {"-R", "dep", NULL}, {"-Ru", "dep", NULL}, {"-Rs", "app", NULL},
        {"-Rs", "other", NULL}, {"-S", "tools", NULL}, {"-S", "dep", NULL},
        {"-Rs", "app", NULL}, {"-Rss", "other", NULL}, {"-S", "tools", NULL},
        {"-Rc", "dep", NULL}, {"-S", "other", NULL}, {"-Rn", "other", NULL},
        {"-U", "/repo/other.pkg.tar", NULL}, {"-U", "--needed", "/repo/other.pkg.tar"},
        {"-R", "other", NULL}, {"-Syu", NULL, NULL}
    };
    char snapshots[2][8192] = {{0}};
    for(int reference = 0; reference < 2; reference++) {
        char temporary[] = "/tmp/axon-compatibility-XXXXXX";
        assert(mkdtemp(temporary) && chdir(temporary) == 0);
        fixture(5);
        assert(unlink("hooks/fixture.hook") == 0);
        copy_binary("/usr/bin/pacman", "/pacman");
        compatibility_repository();
        int output[2];
        assert(pipe(output) == 0);
        pid_t child = fork();
        assert(child >= 0);
        if(child == 0) {
            close(output[0]);
            FILE *snapshot = fdopen(output[1], "w");
            assert(snapshot && chroot(".") == 0 && chdir("/") == 0);
            assert(setenv("LC_ALL", "C", 1) == 0);
            for(size_t index = 0; index < sizeof(steps) / sizeof(*steps); index++) {
                if(index == 14 || index == 17) {
                    write_text("/etc/other.conf", "locally modified");
                }
                int status = command(reference ? "/pacman" : "/axon", steps[index][0], steps[index][1], steps[index][2]);
                fprintf(snapshot, "%zu status=%d backup=%d\n", index, status, access("/etc/other.conf.pacsave", F_OK) == 0);
                axon_config_t *config = NULL;
                axon_config_error_t error;
                axon_alpm_t *core = NULL;
                assert(axon_config_load("/etc/pacman.conf", &config, &error) == AXON_CONFIG_OK);
                assert(axon_config_open(config, &core, &error) == AXON_CONFIG_OK);
                const alpm_list_t *packages = NULL;
                assert(axon_alpm_local_packages(core, &packages) == ALPM_ERR_OK);
                for(const alpm_list_t *item = packages; item; item = item->next) {
                    alpm_pkg_t *package = item->data;
                    fprintf(snapshot, "%s %s reason=%d\n", alpm_pkg_get_name(package),
                            alpm_pkg_get_version(package), alpm_pkg_get_reason(package));
                }
                assert(axon_alpm_release(&core) == 0 && access("/db/db.lck", F_OK) != 0);
                axon_config_free(&config);
            }
            assert(fclose(snapshot) == 0);
            _exit(0);
        }
        close(output[1]);
        size_t used = 0;
        ssize_t size;
        while((size = read(output[0], snapshots[reference] + used, sizeof(snapshots[reference]) - used - 1)) > 0) {
            used += (size_t)size;
            assert(used < sizeof(snapshots[reference]) - 1);
        }
        assert(size == 0);
        close(output[0]);
        int status;
        assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
        assert(umount("dev/pts") == 0 && umount("proc") == 0);
        assert(chdir(project) == 0 && nftw(temporary, remove_path, 32, FTW_DEPTH | FTW_PHYS) == 0);
    }
    if(strcmp(snapshots[0], snapshots[1]) != 0) {
        fprintf(stderr, "Axon mutations:\n%sPacman mutations:\n%s", snapshots[0], snapshots[1]);
        abort();
    }
    puts("Pacman comparisons passed for groups, needed installs, removal modes, backups, and full upgrades.");
}

static void timeout(int number)
{
    (void)number;
    _exit(124);
}

static void filesystem_failures(void)
{
    for(int mode = 0; mode < 4; mode++) {
        char temporary[] = "/tmp/axon-filesystem-XXXXXX";
        assert(mkdtemp(temporary) != NULL && chdir(temporary) == 0);
        fixture(5);
        assert(unlink("hooks/fixture.hook") == 0 && mkdir("data", 0755) == 0);
        struct archive *archive = archive_write_new();
        assert(archive != NULL && archive_write_set_format_pax_restricted(archive) == ARCHIVE_OK);
        assert(archive_write_open_filename(archive, "fixture.pkg.tar") == ARCHIVE_OK);
        entry(archive, ".PKGINFO", "pkgname = fixture\npkgver = 1-1\narch = any\nsize = 8192\npkgdesc = filesystem fixture\n");
        char payload[8193];
        memset(payload, 'x', sizeof(payload) - 1);
        payload[sizeof(payload) - 1] = '\0';
        entry(archive, "data/payload", payload);
        assert(archive_write_close(archive) == ARCHIVE_OK && archive_write_free(archive) == ARCHIVE_OK);
        if(mode == 0) {
            assert(mount("tmpfs", "db", "tmpfs", MS_NOSUID | MS_NODEV, "size=1m") == 0);
            assert(mkdir("db/local", 0755) == 0);
            write_text("db/local/ALPM_DB_VERSION", "9\n");
            assert(mount(NULL, "db", NULL, MS_REMOUNT | MS_RDONLY | MS_NOSUID | MS_NODEV, NULL) == 0);
        }
        else {
            assert(mount("tmpfs", "data", "tmpfs", MS_NOSUID | MS_NODEV,
                         mode == 3 ? "size=1m,nr_inodes=1" : "size=4096") == 0);
            if(mode == 1) {
                assert(mount(NULL, "data", NULL, MS_REMOUNT | MS_RDONLY | MS_NOSUID | MS_NODEV, NULL) == 0);
            }
        }
        pid_t child = fork();
        assert(child >= 0);
        if(child == 0) {
            assert(chroot(".") == 0 && chdir("/") == 0);
            axon_config_t *config = NULL;
            axon_config_error_t error;
            assert(axon_config_load("/etc/pacman.conf", &config, &error) == AXON_CONFIG_OK);
            axon_operation_target_t target = {.name = "/fixture.pkg.tar"};
            axon_operation_request_t request = {.kind = AXON_OPERATION_FILE, .targets = &target, .target_count = 1};
            axon_operation_t *operation = NULL;
            axon_operation_result_t result = {0}, cleanup = {0};
            axon_operation_status_t status = axon_operation_prepare(config, &request, &operation, &result);
            if(mode == 0) {
                assert(status != AXON_OPERATION_OK && !result.may_have_changes && !result.completed_packages);
            }
            else {
                assert(status == AXON_OPERATION_OK);
                axon_operation_approval_t approval = {.confirm = 1, .plan_id = axon_operation_plan(operation)->id};
                status = axon_operation_commit(operation, &approval, &result);
                if(status == AXON_OPERATION_OK || (mode != 1 && !result.execution_errors)) {
                    fprintf(stderr, "filesystem mode %d: status=%d backend=%d changes=%d errors=%zu\n",
                            mode, status, result.backend_error, result.may_have_changes, result.execution_errors);
                    for(alpm_list_t *item = result.diagnostics; item; item = item->next) {
                        fprintf(stderr, "%s\n", (char *)item->data);
                    }
                }
                assert(status != AXON_OPERATION_OK);
                if(mode == 1) {
                    assert(!result.may_have_changes && !result.completed_packages);
                }
                else {
                    assert(result.may_have_changes && result.execution_errors);
                }
                assert(result.diagnostics != NULL);
                int descriptor = open("/data/payload", O_RDONLY);
                if(descriptor >= 0) {
                    char actual[8192];
                    ssize_t length = read(descriptor, actual, sizeof(actual));
                    assert(length != (ssize_t)sizeof(actual) || memcmp(actual, payload, sizeof(actual)) != 0);
                    close(descriptor);
                }
            }
            assert(axon_operation_free(&operation, &cleanup) == AXON_OPERATION_OK);
            assert(access("/db/db.lck", F_OK) != 0);
            axon_operation_result_clear(&result);
            axon_operation_result_clear(&cleanup);
            axon_config_free(&config);
            _exit(0);
        }
        int status;
        assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
        assert(umount(mode == 0 ? "db" : "data") == 0 && umount("dev/pts") == 0 && umount("proc") == 0);
        assert(chdir(project) == 0 && nftw(temporary, remove_path, 32, FTW_DEPTH | FTW_PHYS) == 0);
    }
    puts("Read-only database/root, exhausted blocks/inodes, and transaction cleanup checks passed.");
}

int main(int argc, char **argv)
{
    if(strstr(argv[0], "ldconfig") != NULL || (argc == 2 && strcmp(argv[1], "--signal") == 0)) {
        struct rlimit limit = {0};
        assert(setrlimit(RLIMIT_CORE, &limit) == 0);
        puts("helper-diagnostic: signal fixture");
        fflush(stdout);
        raise(SIGTERM);
        return(99);
    }
    if(argc == 2) {
        puts("helper-diagnostic: exit fixture");
        return(strcmp(argv[1], "--success") == 0 ? 0 : 7);
    }
    assert(getuid() == 0 && geteuid() == 0 && getpid() == 1);
    assert(getcwd(project, sizeof(project)) != NULL);
    assert(signal(SIGALRM, timeout) != SIG_ERR);
    alarm(180);
    for(int scenario = 0; scenario < 6; scenario++) {
        for(int frontend = 0; frontend < 3; frontend++) {
            char temporary[] = "/tmp/axon-helper-XXXXXX";
            assert(mkdtemp(temporary) != NULL && chdir(temporary) == 0);
            fixture(scenario);
            pid_t child = fork();
            assert(child >= 0);
            if(child == 0) {
                assert(chroot(".") == 0 && chdir("/") == 0);
                if(frontend == 2) {
                    ui(scenario);
                }
                else if(frontend == 1) {
                    cli(scenario);
                }
                else {
                    backend(scenario);
                }
                assert((access("/payload", F_OK) == 0) == (scenario != 0));
                assert(access("/db/db.lck", F_OK) != 0);
                _exit(0);
            }
            int status;
            assert(waitpid(child, &status, 0) == child);
            if(!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                fprintf(stderr, "helper fixture failed: scenario %d, frontend %d, root %s\n", scenario, frontend, temporary);
                return(1);
            }
            assert(umount("dev/pts") == 0 && umount("proc") == 0);
            assert(chdir(project) == 0 && nftw(temporary, remove_path, 32, FTW_DEPTH | FTW_PHYS) == 0);
        }
    }
    puts("Hook aborts, post-hook/scriptlet failures, helper signals, and success results passed.");
    filesystem_failures();
    mutation_compatibility();
    return(0);
}
