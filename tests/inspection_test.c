#define _GNU_SOURCE
#include "maintenance.h"

#include <archive.h>
#include <archive_entry.h>
#include <assert.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static char binary[4096], workspace[] = "/tmp/axon-inspection-XXXXXX";
static char diagnostics[32768];

static void write_text(const char *path, const char *text)
{
    FILE *file = fopen(path, "w");
    assert(file && fputs(text, file) >= 0 && fclose(file) == 0);
}

static void message(void *context, const char *text)
{
    (void)context;
    assert(strlen(diagnostics) + strlen(text) + 2 < sizeof(diagnostics));
    strcat(diagnostics, text);
    strcat(diagnostics, "\n");
}

static void archive_entry_text(struct archive *archive, const char *path, const char *text)
{
    struct archive_entry *entry = archive_entry_new();
    assert(entry);
    archive_entry_set_pathname(entry, path);
    archive_entry_set_filetype(entry, AE_IFREG);
    archive_entry_set_perm(entry, 0644);
    archive_entry_set_size(entry, (la_int64_t)strlen(text));
    assert(archive_write_header(archive, entry) == ARCHIVE_OK);
    assert(archive_write_data(archive, text, strlen(text)) == (la_ssize_t)strlen(text));
    archive_entry_free(entry);
}

static void repository(int malformed)
{
    struct archive *archive = archive_write_new();
    assert(archive && archive_write_set_format_pax_restricted(archive) == ARCHIVE_OK);
    assert(archive_write_open_filename(archive, "db/sync/test.db") == ARCHIVE_OK);
    archive_entry_text(archive, "alpha-1-1/desc", malformed ? "%NAME%\nalpha\n\n%VERSION%\n"
        : "%NAME%\nalpha\n\n%VERSION%\n1-1\n\n%DESC%\nAlpha fixture\n\n%GROUPS%\ntools\n\n%ARCH%\nany\n\n");
    assert(archive_write_close(archive) == ARCHIVE_OK && archive_write_free(archive) == ARCHIVE_OK);
}

static int run(const char *program, const char *option, const char *target, const char *extra, char *output, size_t capacity)
{
    int pipes[2];
    assert(pipe(pipes) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if(child == 0) {
        close(pipes[0]);
        assert(dup2(pipes[1], STDOUT_FILENO) >= 0 && dup2(pipes[1], STDERR_FILENO) >= 0);
        close(pipes[1]);
        assert(setenv("LC_ALL", "C", 1) == 0 && setenv("NO_COLOR", "1", 1) == 0);
        char *args[] = {(char *)program, "--config", "pacman.conf", (char *)option,
            (char *)(target ? target : extra), target ? (char *)extra : NULL, NULL};
        execv(program, args);
        _exit(127);
    }
    close(pipes[1]);
    size_t used = 0;
    ssize_t count;
    while((count = read(pipes[0], output + used, capacity - used - 1)) > 0) {
        used += (size_t)count;
        assert(used < capacity - 1);
    }
    assert(count == 0);
    output[used] = '\0';
    close(pipes[0]);
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status));
    return(WEXITSTATUS(status));
}

static void compare(const char *option, const char *target)
{
    char actual[32768], expected[32768];
    int status = run(binary, option, target, NULL, actual, sizeof(actual));
    int reference = run("/usr/bin/pacman", option, target, NULL, expected, sizeof(expected));
    if(status != reference || strcmp(actual, expected) != 0) {
        fprintf(stderr, "comparison %s %s: axon=%d pacman=%d\nAxon:\n%sPacman:\n%s", option,
                target ? target : "", status, reference, actual, expected);
        abort();
    }
}

static void mtree(void)
{
    char *hash = alpm_compute_sha256sum("root/bin/tool");
    assert(hash);
    char text[4096];
    snprintf(text, sizeof(text), "#mtree\n./bin/tool type=file uid=%u gid=%u mode=644 size=4 sha256digest=%s\n"
             "./etc/config type=file uid=%u gid=%u mode=644 size=4 sha256digest=%s\n",
             getuid(), getgid(), hash, getuid(), getgid(), hash);
    write_text("db/local/alpha-1-1/mtree", text);
    free(hash);
}

static void check_status(const char *option, const char *target, int expected)
{
    char output[32768];
    int reference = run("/usr/bin/pacman", option, target, NULL, output, sizeof(output));
    if(reference != expected) {
        fprintf(stderr, "pacman %s %s: expected %d, got %d\n%s", option, target, expected, reference, output);
        abort();
    }
    int status = run(binary, option, target, NULL, output, sizeof(output));
    if(status != expected) {
        fprintf(stderr, "%s %s: expected %d, got %d\n%s", option, target, expected, status, output);
        abort();
    }
    if(expected == 0) {
        assert(!*output);
    }
    if(strcmp(option, "-Qkk") == 0) {
        assert(run(binary, option, target, "--checksums", output, sizeof(output)) == expected);
    }
}

static void noextract(void)
{
    assert(unlink("root/bin/tool") == 0);
    const char *patterns[] = {"bin/*", "bin/* !bin/tool", "bin/* !bin/tool bin/tool", "etc/*"};
    const int expected[] = {0, 1, 0, 1};
    char text[4096];
    for(size_t index = 0; index < sizeof(patterns) / sizeof(*patterns); index++) {
        snprintf(text, sizeof(text), "[options]\nRootDir = %s/root\nDBPath = %s/db\n"
                 "SigLevel = Never\nNoExtract = %s\n", workspace, workspace, patterns[index]);
        write_text("pacman.conf", text);
        check_status("-Qk", "alpha", expected[index]);
        check_status("-Qkk", "alpha", expected[index]);
    }
    snprintf(text, sizeof(text), "[options]\nRootDir = %s/root\nDBPath = %s/db\n"
             "SigLevel = Never\nNoExtract = bin/* omitted/\n", workspace, workspace);
    write_text("pacman.conf", text);
    write_text("root/bin/tool", "modified excluded file");
    assert(chmod("root/bin/tool", 0644) == 0);
    check_status("-Qk", "alpha", 0);
    check_status("-Qkk", "alpha", 1);
    write_text("root/bin/tool", "good");
    if(getuid() != 0) {
        char output[32768];
        assert(chmod("root/bin", 0000) == 0);
        assert(run(binary, "-Qk", "alpha", NULL, output, sizeof(output)) == 1);
        assert(strstr(output, "Permission denied"));
        assert(run(binary, "-Qkk", "alpha", "--checksums", output, sizeof(output)) == 1);
        assert(strstr(output, "Permission denied"));
        assert(chmod("root/bin", 0700) == 0);
    }
    write_text("db/local/beta-1-1/files", "%FILES%\nomitted/\n\n");
    snprintf(text, sizeof(text), "#mtree\n./omitted type=dir uid=%u gid=%u mode=755\n", getuid(), getgid());
    write_text("db/local/beta-1-1/mtree", text);
    check_status("-Qk", "beta", 0);
    char output[32768];
    assert(run(binary, "-Qkk", "beta", "--checksums", output, sizeof(output)) == 0 && !*output);
    write_text("db/local/beta-1-1/files", "%FILES%\n\n");
    assert(unlink("db/local/beta-1-1/mtree") == 0);
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
    assert(access("/usr/bin/pacman", X_OK) == 0);
    char *previous = getcwd(NULL, 0);
    assert(previous && snprintf(binary, sizeof(binary), "%s/axon", previous) < (int)sizeof(binary));
    assert(mkdtemp(workspace) && chdir(workspace) == 0);
    const char *directories[] = {"root", "root/bin", "root/etc", "db", "db/local", "db/sync",
        "db/local/alpha-1-1", "db/local/beta-1-1"};
    for(size_t index = 0; index < sizeof(directories) / sizeof(*directories); index++) {
        assert(mkdir(directories[index], 0700) == 0);
    }
    write_text("db/local/ALPM_DB_VERSION", "9\n");
    const char *alpha = "%NAME%\nalpha\n\n%VERSION%\n1-1\n\n%DESC%\nAlpha fixture\n\n%ARCH%\nany\n\n%REASON%\n0\n\n%GROUPS%\ntools\n\n";
    write_text("db/local/alpha-1-1/desc", alpha);
    write_text("db/local/beta-1-1/desc", "%NAME%\nbeta\n\n%VERSION%\n1-1\n\n%DESC%\nBeta fixture\n\n%ARCH%\nany\n\n%REASON%\n1\n\n%GROUPS%\ntools\n\n");
    write_text("root/bin/tool", "good");
    write_text("root/etc/config", "good");
    assert(chmod("root/bin/tool", 0644) == 0 && chmod("root/etc/config", 0644) == 0);
    char *hash = alpm_compute_md5sum("root/etc/config");
    char text[4096];
    snprintf(text, sizeof(text), "%%FILES%%\nbin/tool\netc/config\n\n%%BACKUP%%\netc/config\t%s\n\n", hash);
    free(hash);
    write_text("db/local/alpha-1-1/files", text);
    write_text("db/local/beta-1-1/files", "%FILES%\n\n");
    mtree();
    repository(0);
    snprintf(text, sizeof(text), "[options]\nRootDir = %s/root\nDBPath = %s/db\nSigLevel = Never\n[test]\n", workspace, workspace);
    write_text("pacman.conf", text);
    const char *options[] = {"-Q", "-Qm", "-Qn", "-Qe", "-Qd", "-Qmd", "-Qg", "-Sg", "-Sgg", "-Ql", "-Qs"};
    for(size_t index = 0; index < sizeof(options) / sizeof(*options); index++) {
        compare(options[index], strcmp(options[index], "-Qmd") == 0 ? "beta" : NULL);
    }
    compare("-Qg", "tools");
    compare("-Sg", "tools");
    compare("-Q", "alpha");
    compare("-Qs", "Alpha");
    compare("-Qo", "root/bin/tool");
    compare("-Qq", NULL);
    compare("-Qsq", "Alpha");
    compare("-Qoq", "root/bin/tool");
    compare("-Qlq", "alpha");
    compare("-Qgq", "tools");
    compare("-Sgq", "tools");
    compare("-Qu", NULL);
    compare("-Quq", NULL);
    noextract();
    write_text("pacman.conf", text);
    char output[32768];
    compare("-Ss", "Alpha");
    compare("-Ssq", "Alpha");
    char limited[4096], reference[32768];
    snprintf(limited, sizeof(limited), "[options]\nRootDir = %s/root\nDBPath = %s/db\nSigLevel = Never\n[test]\nUsage = Install\n", workspace, workspace);
    write_text("pacman.conf", limited);
    assert(run(binary, "-Ss", "Alpha", NULL, output, sizeof(output)) == 0);
    assert(run("/usr/bin/pacman", "-Ss", "Alpha", NULL, reference, sizeof(reference)) == 1);
    assert(!*output && !*reference);
    assert(run(binary, "-Ss", NULL, NULL, output, sizeof(output)) == 0 && !*output);
    write_text("pacman.conf", text);
    assert(run(binary, "-Si", "test/alpha", NULL, output, sizeof(output)) == 0 && strstr(output, "Alpha fixture"));
    assert(run("/usr/bin/pacman", "-Si", "test/alpha", NULL, output, sizeof(output)) == 0 && strstr(output, "Alpha fixture"));
    assert(run(binary, "-Qkk", "alpha", "--checksums", output, sizeof(output)) == 0);
    axon_config_t *config = NULL;
    axon_config_error_t error;
    assert(axon_config_load("pacman.conf", &config, &error) == AXON_CONFIG_OK);
    axon_alpm_t *core = NULL;
    assert(axon_config_open_readonly(config, &core, &error) == AXON_CONFIG_OK);
    alpm_pkg_t *package = NULL;
    assert(axon_alpm_local_package(core, "alpha", &package) == ALPM_ERR_OK);
    assert(axon_maintenance_check(core, message, NULL) == ALPM_ERR_OK);
    write_text("root/etc/config", "user configuration");
    assert(axon_maintenance_files(core, package, 1, 1, message, NULL) == ALPM_ERR_OK);
    assert(strstr(diagnostics, "modified backup configuration") && strstr(diagnostics, "alpha:") && strstr(diagnostics, "root/etc/config"));
    write_text("root/bin/tool", "evil");
    diagnostics[0] = '\0';
    assert(axon_maintenance_files(core, package, 1, 1, message, NULL) != ALPM_ERR_OK);
    assert(strstr(diagnostics, "content mismatch") && strstr(diagnostics, "root/bin/tool"));
    assert(unlink("root/bin/tool") == 0);
    assert(axon_maintenance_files(core, package, 0, 0, message, NULL) != ALPM_ERR_OK);
    assert(strstr(diagnostics, "No such file"));
    write_text("root/bin/tool", "good");
    assert(chmod("root/bin/tool", 0600) == 0);
    assert(axon_maintenance_files(core, package, 1, 0, message, NULL) != ALPM_ERR_OK);
    assert(strstr(diagnostics, "permissions"));
    assert(chmod("root/bin/tool", 0644) == 0);
    if(getuid() != 0) {
        assert(chmod("root/bin/tool", 0000) == 0);
        assert(axon_maintenance_files(core, package, 1, 1, message, NULL) != ALPM_ERR_OK);
        assert(chmod("root/bin/tool", 0644) == 0);
    }
    write_text("db/db.lck", "stale or active: never remove\n");
    assert(axon_maintenance_check(core, message, NULL) == ALPM_ERR_HANDLE_LOCK);
    assert(access("db/db.lck", F_OK) == 0 && unlink("db/db.lck") == 0);
    write_text("db/local/alpha-1-1/desc", "%NAME%\nalpha\n\n%VERSION%\n");
    assert(axon_maintenance_check(core, message, NULL) == ALPM_ERR_DB_INVALID);
    write_text("db/local/alpha-1-1/desc", alpha);
    write_text("db/local/beta-1-1/files", "%FILES%\n../../outside\n\n");
    assert(axon_maintenance_check(core, message, NULL) == ALPM_ERR_DB_INVALID);
    write_text("db/local/beta-1-1/files", "%FILES%\n\n");
    repository(1);
    assert(axon_maintenance_check(core, message, NULL) == ALPM_ERR_DB_INVALID);
    repository(0);
    assert(truncate("db/sync/test.db", 100) == 0);
    assert(axon_maintenance_check(core, message, NULL) == ALPM_ERR_DB_INVALID);
    repository(0);
    assert(axon_alpm_release(&core) == 0);
    assert(unlink("db/local/ALPM_DB_VERSION") == 0);
    assert(axon_config_open_readonly(config, &core, &error) != AXON_CONFIG_OK && core == NULL);
    assert(access("db/local/ALPM_DB_VERSION", F_OK) != 0);
    axon_config_free(&config);
    assert(chdir(previous) == 0 && nftw(workspace, remove_entry, 32, FTW_DEPTH | FTW_PHYS) == 0);
    free(previous);
    puts("Pacman query comparisons and read-only corruption/file inspection checks passed.");
    return(0);
}
