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
    char output[16384];
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
                              "%DESC%\nRemote test package\n\n%ARCH%\nany\n\n";
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
    run(no_command, 2, "choose -Q");
    char *unsafe[] = {binary, "-S", "package", NULL};
    run(unsafe, 2, "choose -Q");
    char *remove[] = {binary, "-R", "package", NULL};
    run(remove, 2, "unknown option");
    char *incompatible[] = {binary, "-QS", NULL};
    run(incompatible, 2, "choose -Q");
    char *no_name[] = {binary, "-Qi", NULL};
    run(no_name, 2, "choose -Q");
    char *no_path[] = {binary, "--config", NULL};
    run(no_path, 2, "missing option value");
    assert(unlink("db/sync/fixture.db") == 0);
    run(search, 1, "repository search failed");
    assert(chdir(previous) == 0);
    assert(nftw(workspace, remove_entry, 16, FTW_DEPTH | FTW_PHYS) == 0);
    free(previous);
    free(binary);
    puts("CLI integration checks passed.");
    return(0);
}
