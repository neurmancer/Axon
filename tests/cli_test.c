#define _XOPEN_SOURCE 700

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

static char *binary;
static char output[16384];

static void write_file(const char *path, const char *contents)
{
    FILE *file = fopen(path, "w");
    assert(file != NULL && fputs(contents, file) >= 0);
    assert(fclose(file) == 0);
}

static void run(char *const arguments[], int expected, const char *text)
{
    int descriptors[2];
    assert(pipe(descriptors) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if(child == 0) {
        close(descriptors[0]);
        assert(dup2(descriptors[1], STDOUT_FILENO) >= 0);
        assert(dup2(descriptors[1], STDERR_FILENO) >= 0);
        close(descriptors[1]);
        execv(binary, arguments);
        _exit(127);
    }

    close(descriptors[1]);
    size_t used = 0;
    ssize_t count;
    while((count = read(descriptors[0], output + used, sizeof(output) - used - 1)) > 0) {
        used += (size_t)count;
        assert(used < sizeof(output) - 1);
    }
    assert(count == 0);
    output[used] = '\0';
    close(descriptors[0]);
    int status;
    assert(waitpid(child, &status, 0) == child);
    if(!WIFEXITED(status) || WEXITSTATUS(status) != expected || strstr(output, text) == NULL) {
        for(size_t index = 0; arguments[index] != NULL; index++) {
            fprintf(stderr, "%s ", arguments[index]);
        }
        fputc('\n', stderr);
        fprintf(stderr, "CLI test failed: status %d, expected %d, wanted '%s'\n%s", status, expected, text, output);
        abort();
    }
    assert(access("db/db.lck", F_OK) != 0);
}

static void repository(void)
{
    const char *description = "%NAME%\nremote-fixture\n\n%VERSION%\n2-1\n\n"
                              "%DESC%\nRemote test package\n\n%ARCH%\nany\n\n"
                              "%DEPENDS%\nlocal-fixture>=1\n\n%CONFLICTS%\nold-fixture\n\n"
                              "%PROVIDES%\nvirtual-fixture=2\n\n%OPTDEPENDS%\nextra-fixture: optional feature\n\n";
    struct archive *archive = archive_write_new();
    struct archive_entry *entry = archive_entry_new();
    assert(archive != NULL && entry != NULL);
    assert(archive_write_set_format_pax_restricted(archive) == ARCHIVE_OK);
    assert(archive_write_open_filename(archive, "db/sync/fixture.db") == ARCHIVE_OK);
    archive_entry_set_pathname(entry, "remote-fixture-2-1/desc");
    archive_entry_set_filetype(entry, AE_IFREG);
    archive_entry_set_perm(entry, 0644);
    archive_entry_set_size(entry, (la_int64_t)strlen(description));
    assert(archive_write_header(archive, entry) == ARCHIVE_OK);
    assert(archive_write_data(archive, description, strlen(description)) == (la_ssize_t)strlen(description));
    archive_entry_free(entry);
    assert(archive_write_close(archive) == ARCHIVE_OK);
    assert(archive_write_free(archive) == ARCHIVE_OK);
}

static void package_archive(void)
{
    const char *paths[] = {".PKGINFO", "usr/share/archive-fixture"};
    const char *texts[] = {"pkgname = archive-fixture\npkgver = 3-1\narch = any\n"
        "pkgdesc = Archive metadata fixture\ndepend = local-fixture>=1\nconflict = old-fixture\n",
        "archive payload\n"};
    struct archive *archive = archive_write_new();
    assert(archive != NULL && archive_write_set_format_pax_restricted(archive) == ARCHIVE_OK);
    assert(archive_write_open_filename(archive, "fixture.pkg.tar") == ARCHIVE_OK);
    for(size_t index = 0; index < 2; index++) {
        struct archive_entry *entry = archive_entry_new();
        assert(entry != NULL);
        archive_entry_set_pathname(entry, paths[index]);
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0644);
        archive_entry_set_size(entry, (la_int64_t)strlen(texts[index]));
        assert(archive_write_header(archive, entry) == ARCHIVE_OK);
        assert(archive_write_data(archive, texts[index], strlen(texts[index])) == (la_ssize_t)strlen(texts[index]));
        archive_entry_free(entry);
    }
    assert(archive_write_close(archive) == ARCHIVE_OK && archive_write_free(archive) == ARCHIVE_OK);
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
    assert(previous != NULL);
    binary = malloc(strlen(previous) + 6);
    assert(binary != NULL);
    sprintf(binary, "%s/axon", previous);
    char workspace[] = "/tmp/axon-cli-test-XXXXXX";
    assert(mkdtemp(workspace) != NULL && chdir(workspace) == 0);
    assert(mkdir("db", 0700) == 0);
    assert(mkdir("db/local", 0700) == 0);
    assert(mkdir("db/local/local-fixture-1-1", 0700) == 0);
    assert(mkdir("db/sync", 0700) == 0);
    write_file("db/local/ALPM_DB_VERSION", "9\n");
    write_file("db/local/local-fixture-1-1/desc",
               "%NAME%\nlocal-fixture\n\n%VERSION%\n1-1\n\n"
               "%DESC%\nLocal test package\n\n%ARCH%\nany\n\n"
               "%SIZE%\n42\n\n%REASON%\n0\n\n");
    write_file("db/local/local-fixture-1-1/files", "%FILES%\n\n");
    repository();
    char config[2048];
    snprintf(config, sizeof(config), "[options]\nRootDir = %s\nDBPath = %s/db\n"
             "SigLevel = Never\n[fixture]\n", workspace, workspace);
    write_file("pacman.conf", config);
    char *help[] = {binary, "--config", "missing.conf", "--help", NULL};
    run(help, 0, "Usage:");
    char *list[] = {binary, "--config", "pacman.conf", "-Q", NULL};
    run(list, 0, "local-fixture 1-1\n");
    char *info[] = {binary, "--config=pacman.conf", "-Qi", "local-fixture", NULL};
    run(info, 0, "Local test package");
    char *mixed[] = {binary, "--config", "pacman.conf", "-Qi", "missing", "local-fixture", NULL};
    run(mixed, 1, "Local test package");
    char *missing[] = {binary, "--config", "pacman.conf", "-Qi", "missing", NULL};
    run(missing, 1, "axon: missing:");
    char *search[] = {binary, "--config", "pacman.conf", "-Ss", "Remote", "fixture", NULL};
    run(search, 0, "fixture/remote-fixture 2-1\n");
    char *unmatched[] = {binary, "--config", "pacman.conf", "-Ss", "absent", NULL};
    run(unmatched, 0, "");
    char *regex[] = {binary, "--config", "pacman.conf", "-Ss", "[", NULL};
    run(regex, 1, "repository search failed");
    char *no_config[] = {binary, "--config", "missing.conf", "-Q", NULL};
    run(no_config, 1, "missing.conf:0:");
    write_file("invalid.conf", "[options]\nSigLevel = invalid\n");
    char *invalid_config[] = {binary, "--config", "invalid.conf", "-Q", NULL};
    run(invalid_config, 1, "invalid.conf:2:");
    char *no_command[] = {binary, NULL};
    run(no_command, 2, "incompatible operation");
    char *unsafe[] = {binary, "-Sy", "package", NULL};
    run(unsafe, 2, "incompatible operation");
    char *remove[] = {binary, "-Ri", "package", NULL};
    run(remove, 2, "incompatible operation");
    char *incompatible[] = {binary, "-QS", NULL};
    run(incompatible, 2, "incompatible operation");
    char *no_name[] = {binary, "--config", "pacman.conf", "-Qi", NULL};
    run(no_name, 0, "local-fixture");
    char *no_path[] = {binary, "--config", NULL};
    run(no_path, 2, "missing option value");
    char *depends[] = {binary, "--config", "pacman.conf", "--depends", "local-fixture", NULL};
    run(depends, 0, "Dependencies:");
    char *missing_dependency[] = {binary, "--config", "pacman.conf", "--depends", "missing", NULL};
    run(missing_dependency, 1, "missing:");
    char *orphans[] = {binary, "--config", "pacman.conf", "--orphans", NULL};
    run(orphans, 0, "");
    char *check[] = {binary, "--config", "pacman.conf", "--check-db", NULL};
    run(check, 0, "no repair performed");
    char *combined[] = {binary, "--orphans", "-Q", NULL};
    run(combined, 2, "incompatible operation");
    char *missing_targets[] = {binary, "--asdeps", NULL};
    run(missing_targets, 2, "incompatible operation");
    char *multiple[] = {binary, "--orphans", "--check-db", NULL};
    run(multiple, 2, "choose one maintenance operation");
    if(getuid() != 0) {
        char *install[] = {binary, "--config", "pacman.conf", "-S", "remote-fixture", NULL};
        run(install, 1, "root");
        char *erase[] = {binary, "--config", "pacman.conf", "-R", "local-fixture", NULL};
        run(erase, 1, "root");
        char *refresh[] = {binary, "--config", "pacman.conf", "-Sy", NULL};
        run(refresh, 1, "root");
        char *reason[] = {binary, "--config", "pacman.conf", "--asdeps", "local-fixture", NULL};
        run(reason, 1, "root");
        char *clean[] = {binary, "--config", "pacman.conf", "--clean-cache", NULL};
        run(clean, 1, "root");
        char *upgrade_install[] = {binary, "--config", "pacman.conf", "-Syu", "fixture/remote-fixture", NULL};
        run(upgrade_install, 1, "root");
    }
    char *quiet[] = {binary, "--config", "pacman.conf", "-Qq", NULL};
    run(quiet, 0, "local-fixture\n");
    assert(strcmp(output, "local-fixture\n") == 0);
    char *quiet_search[] = {binary, "--config", "pacman.conf", "-Ssq", "Remote", NULL};
    run(quiet_search, 0, "remote-fixture\n");
    assert(strcmp(output, "remote-fixture\n") == 0);
    char *remote_info[] = {binary, "--config", "pacman.conf", "-Si", "remote-fixture", NULL};
    run(remote_info, 0, "Depends on     : local-fixture>=1\n");
    assert(strstr(output, "Conflicts with : old-fixture\n") && strstr(output, "Install reason : Not installed\n"));
    assert(strstr(output, "Provides       : virtual-fixture=2\n") && strstr(output, "extra-fixture: optional feature"));
    char *upgrades[] = {binary, "--config", "pacman.conf", "-Qu", NULL};
    run(upgrades, 1, "");
    assert(output[0] == '\0');
    assert(mkdir("db/local/remote-fixture-1-1", 0700) == 0);
    write_file("db/local/remote-fixture-1-1/desc", "%NAME%\nremote-fixture\n\n%VERSION%\n1-1\n\n%REASON%\n1\n\n");
    write_file("db/local/remote-fixture-1-1/files", "%FILES%\n\n");
    run(upgrades, 0, "remote-fixture 1-1 -> 2-1\n");
    assert(strcmp(output, "remote-fixture 1-1 -> 2-1\n") == 0);
    char *quiet_upgrades[] = {binary, "--config", "pacman.conf", "-Quq", NULL};
    run(quiet_upgrades, 0, "remote-fixture\n");
    assert(strcmp(output, "remote-fixture\n") == 0);
    char *explicit_upgrades[] = {binary, "--config", "pacman.conf", "-Queq", NULL};
    run(explicit_upgrades, 1, "");
    assert(output[0] == '\0');
    run(remote_info, 0, "Install reason : Installed as a dependency\n");
    package_archive();
    char *archive_info[] = {binary, "--config", "pacman.conf", "-Qip", "fixture.pkg.tar", NULL};
    run(archive_info, 0, "Archive metadata fixture");
    assert(strstr(output, "local-fixture>=1") && strstr(output, "old-fixture") && strstr(output, "Not applicable"));
    char *archive_files[] = {binary, "--config", "pacman.conf", "-Qlpq", "fixture.pkg.tar", NULL};
    run(archive_files, 0, "usr/share/archive-fixture\n");
    assert(strcmp(output, "usr/share/archive-fixture\n") == 0);
    char *archive_names[] = {binary, "--config", "pacman.conf", "-Qpq", "fixture.pkg.tar", NULL};
    run(archive_names, 0, "archive-fixture\n");
    assert(strcmp(output, "archive-fixture\n") == 0);
    char *archive_mixed[] = {binary, "--config", "pacman.conf", "-Qp", "pacman.conf", "fixture.pkg.tar", NULL};
    run(archive_mixed, 1, "archive-fixture 3-1\n");
    const char *invalid_queries[] = {"-Qpu", "-Qps", "-Qpk", "-Qpe", "-Qpo", "-Sp", "-Sq", "-Uq"};
    for(size_t index = 0; index < sizeof(invalid_queries) / sizeof(*invalid_queries); index++) {
        char *arguments[] = {binary, (char *)invalid_queries[index], "fixture.pkg.tar", NULL};
        run(arguments, 2, "incompatible operation");
    }
    char *archive_missing[] = {binary, "-Qp", NULL};
    run(archive_missing, 2, "incompatible operation");
    assert(unlink("db/sync/fixture.db") == 0);
    run(search, 1, "repository search failed");
    run(upgrades, 1, "upgrade query failed");
    assert(chdir(previous) == 0);
    assert(nftw(workspace, remove_entry, 16, FTW_DEPTH | FTW_PHYS) == 0);
    free(previous);
    free(binary);
    puts("CLI integration checks passed.");
    return(0);
}
