#define _GNU_SOURCE
#include "operation.h"
#include "root_fixture.h"

#include <assert.h>
#include <fcntl.h>
#include <ftw.h>
#include <grp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static char output[65536];
static int full_descriptor;

static void invoke(char *const args[], const char *input, int expected, const char *marker, int signum, int full)
{
    int in[2], out[2];
    assert(pipe(in) == 0 && pipe(out) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if(child == 0) {
        assert(dup2(in[0], STDIN_FILENO) >= 0 && dup2(full ? full_descriptor : out[1], STDOUT_FILENO) >= 0
            && dup2(out[1], STDERR_FILENO) >= 0);
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        alarm(20);
        execv(args[0], args);
        _exit(127);
    }
    close(in[0]); close(out[1]);
    if(input) { assert(write(in[1], input, strlen(input)) == (ssize_t)strlen(input)); }
    if(!marker) { close(in[1]); }
    size_t used = 0;
    int sent = 0;
    for(;;) {
        struct pollfd poller = {.fd = out[0], .events = POLLIN};
        assert(poll(&poller, 1, 10000) > 0);
        ssize_t size = read(out[0], output + used, sizeof(output) - used - 1);
        assert(size >= 0);
        if(size == 0) { break; }
        used += (size_t)size;
        assert(used < sizeof(output) - 1);
        output[used] = '\0';
        if(marker && !sent && strstr(output, marker)) {
            assert(kill(child, signum) == 0);
            close(in[1]);
            sent = 1;
        }
    }
    output[used] = '\0';
    if(marker) { assert(sent); }
    close(out[0]);
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status));
    if(WEXITSTATUS(status) != expected) {
        fprintf(stderr, "%s %s expected=%d actual=%d:\n%s", args[0], args[1], expected, WEXITSTATUS(status), output);
        abort();
    }
}

static void cli(const char *a, const char *b, const char *c, const char *input, int expected)
{
    char *args[] = {"/axon", (char *)a, (char *)b, (char *)c, NULL};
    invoke(args, input, expected, NULL, 0, 0);
    assert(access("/db/db.lck", F_OK) != 0);
}

static void backend(const char *mode, const char *target, int success)
{
    char *args[] = {"/driver", (char *)mode, "/etc/pacman.conf", (char *)target, NULL};
    invoke(args, NULL, 0, NULL, 0, 0);
    const char *status = strstr(output, "status=");
    assert(status && ((strtol(status + 7, NULL, 10) == 0) == success));
    assert(access("/db/db.lck", F_OK) != 0);
}

static void package(const char *name, const char *version, const char *script)
{
    char path[256], text[1024];
    snprintf(path, sizeof(path), "/repo/%s-%s.pkg.tar", name, version);
    struct archive *archive = archive_write_new();
    assert(archive && archive_write_set_format_pax_restricted(archive) == ARCHIVE_OK);
    assert(archive_write_open_filename(archive, path) == ARCHIVE_OK);
    snprintf(text, sizeof(text), "pkgname = %s\npkgver = %s\narch = any\nsize = 16\npkgdesc = Isolated test\n", name, version);
    entry(archive, ".PKGINFO", text);
    snprintf(path, sizeof(path), "usr/share/%s", name);
    entry(archive, path, version);
    if(script) { entry(archive, ".INSTALL", script); }
    assert(archive_write_close(archive) == ARCHIVE_OK && archive_write_free(archive) == ARCHIVE_OK);
}

static void repository(const char *version)
{
    package("fixture", version,
        "post_install() { echo post_install >> /scriptlet-results; }\n"
        "post_upgrade() { echo post_upgrade >> /scriptlet-results; }\n"
        "post_remove() { echo post_remove >> /scriptlet-results; }\n");
    char path[256], text[2048];
    snprintf(path, sizeof(path), "/repo/fixture-%s.pkg.tar", version);
    char *hash = alpm_compute_sha256sum(path);
    struct stat info;
    assert(hash && stat(path, &info) == 0);
    snprintf(text, sizeof(text), "%%NAME%%\nfixture\n\n%%VERSION%%\n%s\n\n%%FILENAME%%\nfixture-%s.pkg.tar\n\n"
             "%%ARCH%%\nany\n\n%%DESC%%\nFixture\n\n%%CSIZE%%\n%jd\n\n%%ISIZE%%\n16\n\n%%SHA256SUM%%\n%s\n\n",
             version, version, (intmax_t)info.st_size, hash);
    free(hash);
    struct archive *archive = archive_write_new();
    assert(archive && archive_write_set_format_pax_restricted(archive) == ARCHIVE_OK);
    assert(archive_write_open_filename(archive, "/repo/test.db") == ARCHIVE_OK);
    snprintf(path, sizeof(path), "fixture-%s/desc", version);
    entry(archive, path, text);
    assert(archive_write_close(archive) == ARCHIVE_OK && archive_write_free(archive) == ARCHIVE_OK);
}

static int contents(const char *path, const char *text)
{
    FILE *file = fopen(path, "r");
    assert(file);
    char data[4096];
    size_t count = fread(data, 1, sizeof(data) - 1, file);
    data[count] = '\0';
    assert(!ferror(file) && fclose(file) == 0);
    return(strstr(data, text) != NULL);
}

static void inside(void)
{
    uid_t real, effective, saved;
    gid_t group, effective_group, saved_group;
    assert(getresuid(&real, &effective, &saved) == 0 && real == 0 && effective == 0 && saved == 0);
    assert(getresgid(&group, &effective_group, &saved_group) == 0 && group == 0 && effective_group == 0 && saved_group == 0);
    write_text("/hooks/test.hook", "[Trigger]\nOperation = Install\nOperation = Upgrade\nOperation = Remove\n"
        "Type = Package\nTarget = fixture\n[Action]\nWhen = PostTransaction\nExec = /usr/bin/touch /hook-ran\n");
    repository("1-1"); backend("refresh", NULL, 1); backend("install", "fixture", 1);
    struct stat info;
    assert(stat("/usr/share/fixture", &info) == 0 && info.st_uid == 0 && info.st_gid == 0 && (info.st_mode & 0777) == 0644);
    assert(contents("/usr/share/fixture", "1-1") && contents("/scriptlet-results", "post_install") && unlink("/hook-ran") == 0);
    repository("2-1"); backend("upgrade", "unused", 1);
    assert(contents("/usr/share/fixture", "2-1") && contents("/scriptlet-results", "post_upgrade") && unlink("/hook-ran") == 0);
    backend("remove", "fixture", 1);
    assert(access("/usr/share/fixture", F_OK) != 0 && contents("/scriptlet-results", "post_remove") && unlink("/hook-ran") == 0);
    write_text("/hooks/fail.hook", "[Trigger]\nOperation = Install\nType = Package\nTarget = fixture\n"
        "[Action]\nWhen = PreTransaction\nAbortOnFail\nExec = /bin/sh -c 'exit 1'\n");
    backend("install", "fixture", 0);
    assert(access("/usr/share/fixture", F_OK) != 0 && unlink("/hooks/fail.hook") == 0);
    write_text("/repo/bad.pkg.tar", "broken archive"); backend("file", "/repo/bad.pkg.tar", 0);
    const char *invalid[][3] = {{"-Sy", "fixture", NULL}, {"-Syu", "--file", "fixture"}, {"-R", NULL, NULL},
        {"-U", NULL, NULL}, {"-QS", NULL, NULL}, {"-Ssi", "fixture", NULL}, {"-R", "--allow-downgrade", "fixture"},
        {"--noconfirm", "-S", "fixture"}};
    for(size_t index = 0; index < sizeof(invalid) / sizeof(*invalid); index++) {
        cli(invalid[index][0], invalid[index][1], invalid[index][2], NULL, 2);
    }
    cli("-Sy", NULL, NULL, "n\n", 1); cli("-Sy", NULL, NULL, "y\n", 0);
    const char *replies[] = {"n\n", "", "yes", "yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy\ny\n"};
    for(size_t index = 0; index < 4; index++) {
        cli("-S", "fixture", NULL, replies[index], 1);
        assert(strstr(output, "Prepared transaction") && access("/usr/share/fixture", F_OK) != 0);
    }
    cli("-S", "test/fixture", NULL, "y\n", 0);
    assert(strstr(output, "Transaction completed") && contents("/usr/share/fixture", "2-1"));
    cli("-R", "fixture", NULL, "n\n", 1);
    assert(access("/usr/share/fixture", F_OK) == 0);
    FILE *file = fopen("/etc/pacman.conf", "a");
    assert(file && fputs("[options]\nHoldPkg = fixture\n", file) >= 0 && fclose(file) == 0);
    cli("-R", "fixture", NULL, "n\ny\n", 1); assert(access("/usr/share/fixture", F_OK) == 0);
    cli("-R", "fixture", NULL, "y\ny\n", 0); assert(access("/usr/share/fixture", F_OK) != 0);
    cli("-U", "/repo/fixture-1-1.pkg.tar", NULL, "y\n", 0);
    assert(contents("/usr/share/fixture", "1-1"));
    cli("-Syu", "fixture", NULL, "y\ny\n", 0);
    assert(strstr(output, "upgrade fixture: 1-1 -> 2-1") && contents("/usr/share/fixture", "2-1"));
    cli("-U", "/repo/fixture-1-1.pkg.tar", NULL, "y\n", 1); assert(contents("/usr/share/fixture", "2-1"));
    cli("-U", "--allow-downgrade", "/repo/fixture-1-1.pkg.tar", "y\ny\n", 0);
    assert(contents("/usr/share/fixture", "1-1")); cli("-Su", NULL, NULL, "y\n", 0);
    assert(contents("/usr/share/fixture", "2-1")); cli("-Su", NULL, NULL, "", 0); assert(strstr(output, "Nothing to do"));
    cli("-S", "absent", NULL, "y\n", 1); assert(strstr(output, "target: absent"));
    cli("-U", "/repo/bad.pkg.tar", NULL, "y\n", 1);
    for(int number = 0; number < 2; number++) {
        int signum = number ? SIGTERM : SIGINT;
        char *args[] = {"/axon", "-R", "fixture", NULL};
        invoke(args, NULL, 128 + signum, "[y/N]", signum, 0);
        assert(access("/db/db.lck", F_OK) != 0 && access("/usr/share/fixture", F_OK) == 0);
    }
    package("cancel-a", "1-1", "post_install() { echo cancel-during-install; /usr/bin/sleep 1; }\n");
    package("cancel-b", "1-1", NULL);
    char *args[] = {"/axon", "-U", "/repo/cancel-a-1-1.pkg.tar", "/repo/cancel-b-1-1.pkg.tar", NULL};
    invoke(args, "y\n", 130, "cancel-during-install", SIGINT, 0);
    assert(strstr(output, "changes may have occurred") && access("/db/db.lck", F_OK) != 0);
    assert(access("/usr/share/cancel-a", F_OK) == 0 && access("/usr/share/cancel-b", F_OK) != 0);
    write_text("/db/db.lck", "another process\n");
    char *locked[] = {"/axon", "-S", "fixture", NULL}; invoke(locked, "y\n", 1, NULL, 0, 0);
    assert(strstr(output, "lock") && contents("/db/db.lck", "another process") && unlink("/db/db.lck") == 0);
    package("output-failure", "1-1", NULL);
    char *full[] = {"/axon", "-U", "/repo/output-failure-1-1.pkg.tar", NULL};
    invoke(full, "y\n", 1, NULL, 0, 1);
    assert(access("/usr/share/output-failure", F_OK) != 0 && access("/db/db.lck", F_OK) != 0);
}

static int remove_entry(const char *path, const struct stat *info, int type, struct FTW *walk)
{
    (void)info; (void)type; (void)walk;
    return(remove(path));
}

static void timeout(int number)
{
    (void)number;
    _exit(124);
}

int main(int argc, char **argv)
{
    assert(getcwd(project, sizeof(project)));
    if(argc == 1) {
        char temporary[] = "/tmp/axon-nonroot-XXXXXX", executable[4096], source[8192], config[4096], text[8192];
        assert(mkdtemp(temporary) && chmod(temporary, 0755) == 0 && chdir(temporary) == 0);
        snprintf(source, sizeof(source), "%s/tests/privileged_driver.out", project);
        copy_binary(source, "/driver");
        snprintf(config, sizeof(config), "%s/pacman.conf", temporary);
        snprintf(text, sizeof(text), "[options]\nRootDir = %s\nDBPath = %s/db\n", temporary, temporary);
        write_text(config, text); assert(chmod(config, 0644) == 0 && chmod("driver", 0755) == 0);
        snprintf(executable, sizeof(executable), "%s/driver", temporary);
        pid_t child = fork(); assert(child >= 0);
        if(child == 0) {
            if(getuid() == 0) { assert(setgroups(0, NULL) == 0 && setgid(65534) == 0 && setuid(65534) == 0); }
            char *args[] = {executable, "file", config, "/missing.pkg.tar", NULL};
            invoke(args, NULL, 0, NULL, 0, 0); assert(strstr(output, "status=3\n")); _exit(0);
        }
        int status;
        assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0 && access("db", F_OK) != 0);
        assert(chdir(project) == 0 && nftw(temporary, remove_entry, 32, FTW_DEPTH | FTW_PHYS) == 0);
        execlp("unshare", "unshare", "--user", "--map-root-user", "--mount", "--pid", "--fork", "--net", argv[0], "--inside", (char *)NULL);
        return(1);
    }
    assert(argc == 2 && strcmp(argv[1], "--inside") == 0 && getpid() == 1 && getuid() == 0);
    assert(signal(SIGALRM, timeout) != SIG_ERR); alarm(120);
    full_descriptor = open("/dev/full", O_WRONLY); assert(full_descriptor >= 0);
    char temporary[] = "/tmp/axon-privileged-XXXXXX";
    assert(mkdtemp(temporary) && chdir(temporary) == 0);
    fixture(5); assert(unlink("hooks/fixture.hook") == 0 && mkdir("repo", 0755) == 0);
    copy_binary("/usr/bin/touch", "/usr/bin/touch"); copy_binary("/usr/bin/sleep", "/usr/bin/sleep");
    char driver[8192]; snprintf(driver, sizeof(driver), "%s/tests/privileged_driver.out", project); copy_binary(driver, "/driver");
    FILE *config = fopen("etc/pacman.conf", "a"); assert(config && fputs("[test]\nServer = file:///repo\n", config) >= 0 && fclose(config) == 0);
    pid_t child = fork(); assert(child >= 0);
    if(child == 0) { assert(chroot(".") == 0 && chdir("/") == 0); inside(); _exit(0); }
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(umount("dev/pts") == 0 && umount("proc") == 0 && chdir(project) == 0 && nftw(temporary, remove_entry, 32, FTW_DEPTH | FTW_PHYS) == 0);
    close(full_descriptor);
    puts("C production privilege, ownership, scriptlet/hook, approval, downgrade, signal, output, and lock regressions passed.");
    return(0);
}
