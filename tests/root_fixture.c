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

char project[4096];

static void parents(char *path)
{
    for(char *cursor = path + 1; *cursor; cursor++) {
        if(*cursor == '/') {
            *cursor = '\0';
            assert(mkdir(path, 0755) == 0 || errno == EEXIST);
            *cursor = '/';
        }
    }
}

static void copy_file(const char *source, const char *destination)
{
    char path[8192];
    assert(snprintf(path, sizeof(path), ".%s", destination) < (int)sizeof(path));
    parents(path);
    int input = open(source, O_RDONLY);
    int output = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    assert(input >= 0 && output >= 0);
    char buffer[16384];
    ssize_t count;
    while((count = read(input, buffer, sizeof(buffer))) > 0) {
        assert(write(output, buffer, (size_t)count) == count);
    }
    assert(count == 0 && close(input) == 0 && close(output) == 0);
}

void copy_binary(const char *source, const char *destination)
{
    copy_file(source, destination);
    int pipes[2];
    assert(pipe(pipes) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if(child == 0) {
        assert(dup2(pipes[1], STDOUT_FILENO) >= 0);
        close(pipes[0]);
        close(pipes[1]);
        execlp("ldd", "ldd", source, (char *)NULL);
        _exit(127);
    }
    close(pipes[1]);
    FILE *stream = fdopen(pipes[0], "r");
    assert(stream != NULL);
    char *line = NULL;
    size_t capacity = 0;
    while(getline(&line, &capacity, stream) >= 0) {
        char *path = strchr(line, '/');
        if(path != NULL) {
            path[strcspn(path, " \t\n")] = '\0';
            copy_file(path, path);
        }
    }
    free(line);
    assert(fclose(stream) == 0);
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

void write_text(const char *path, const char *text)
{
    FILE *file = fopen(path, "w");
    assert(file != NULL && fputs(text, file) >= 0 && fclose(file) == 0);
}

void entry(struct archive *archive, const char *name, const char *text)
{
    struct archive_entry *item = archive_entry_new();
    assert(item != NULL);
    archive_entry_set_pathname(item, name);
    archive_entry_set_filetype(item, AE_IFREG);
    archive_entry_set_perm(item, 0644);
    archive_entry_set_size(item, (la_int64_t)strlen(text));
    assert(archive_write_header(archive, item) == ARCHIVE_OK);
    assert(archive_write_data(archive, text, strlen(text)) == (la_ssize_t)strlen(text));
    archive_entry_free(item);
}

void fixture(int scenario)
{
    const char *directories[] = {"etc", "db", "db/local", "cache", "hooks", "tmp", "keys", "dev", "dev/pts", "proc"};
    for(size_t index = 0; index < sizeof(directories) / sizeof(*directories); index++) {
        assert(mkdir(directories[index], 0755) == 0);
    }
    assert(mount("devpts", "dev/pts", "devpts", MS_NOSUID | MS_NOEXEC,
                 "newinstance,ptmxmode=0666,mode=0620") == 0);
    assert(mount("proc", "proc", "proc", MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) == 0);
    assert(symlink("pts/ptmx", "dev/ptmx") == 0);
    copy_binary("/bin/sh", "/bin/sh");
    copy_binary("/usr/bin/bash", "/usr/bin/bash");
    const char *terminfo = access("/usr/share/terminfo/x/xterm", R_OK) == 0
        ? "/usr/share/terminfo/x/xterm" : "/usr/share/terminfo/78/xterm";
    copy_file(terminfo, "/usr/share/terminfo/x/xterm");
    char binary[8192];
    snprintf(binary, sizeof(binary), "%s/axon", project);
    copy_binary(binary, "/axon");
    snprintf(binary, sizeof(binary), "%s/tests/helper_failure_test.out", project);
    copy_binary(binary, "/helper");
    write_text("etc/pacman.conf", "[options]\nRootDir = /\nDBPath = /db\nCacheDir = /cache\n"
        "LogFile = /transaction.log\nGPGDir = /keys\nHookDir = /hooks\nArchitecture = any\nSigLevel = Never\n");
    struct archive *archive = archive_write_new();
    assert(archive != NULL && archive_write_set_format_pax_restricted(archive) == ARCHIVE_OK);
    assert(archive_write_open_filename(archive, "fixture.pkg.tar") == ARCHIVE_OK);
    entry(archive, ".PKGINFO", "pkgname = fixture\npkgver = 1-1\narch = any\nsize = 7\npkgdesc = failure fixture\n");
    entry(archive, "payload", "changed");
    if(scenario == 2) {
        entry(archive, ".INSTALL", "post_install() { echo scriptlet-diagnostic; return 7; }\n");
    }
    assert(archive_write_close(archive) == ARCHIVE_OK && archive_write_free(archive) == ARCHIVE_OK);
    if(scenario == 0 || scenario == 1 || scenario == 3 || scenario == 5) {
        char hook[1024];
        snprintf(hook, sizeof(hook), "[Trigger]\nOperation = Install\nType = Package\nTarget = fixture\n"
            "[Action]\nWhen = %s\n%sExec = /helper %s\n",
            scenario == 0 ? "PreTransaction" : "PostTransaction",
            scenario == 0 ? "AbortOnFail\n" : "",
            scenario == 3 ? "--signal" : scenario == 5 ? "--success" : "--fail");
        write_text("hooks/fixture.hook", hook);
    }
    if(scenario == 4) {
        copy_binary(binary, "/usr/bin/ldconfig");
        write_text("etc/ld.so.conf", "");
    }
}
