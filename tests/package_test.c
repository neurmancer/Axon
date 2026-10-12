#define _XOPEN_SOURCE 700
#include <alpm.h>
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

static void run(char *const arguments[])
{
    pid_t child = fork();
    assert(child >= 0);
    if(child == 0) {
        execvp(arguments[0], arguments);
        _exit(127);
    }
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void inspect(const char *path, int source)
{
    struct archive *archive = archive_read_new();
    assert(archive != NULL);
    assert(archive_read_support_filter_all(archive) == ARCHIVE_OK);
    assert(archive_read_support_format_all(archive) == ARCHIVE_OK);
    assert(archive_read_open_filename(archive, path, 10240) == ARCHIVE_OK);
    struct archive_entry *entry;
    int code, executable = 0, metadata = 0, license = 0, manual = 0, makefile = 0;
    while((code = archive_read_next_header(archive, &entry)) == ARCHIVE_OK) {
        const char *name = archive_entry_pathname(entry);
        assert(name[0] != '/' && strstr(name, "../") == NULL && strstr(name, "/.git/") == NULL);
        assert(archive_entry_uid(entry) == 0 && archive_entry_gid(entry) == 0);
        assert(archive_entry_mtime(entry) == 0);
        if(source) {
            size_t size = strlen(name);
            assert(strncmp(name, "axon-0.1.0/", 11) == 0);
            assert(size < 2 || strcmp(name + size - 2, ".o") != 0);
            assert(size < 4 || strcmp(name + size - 4, ".out") != 0);
            makefile |= strcmp(name, "axon-0.1.0/Makefile") == 0;
            continue;
        }
        assert(strstr(name, "tests/") == NULL);
        if(strcmp(name, "usr/bin/axon") == 0) {
            assert(archive_entry_perm(entry) == 0755);
            executable++;
        }
        if(strcmp(name, ".PKGINFO") == 0) {
            char text[8192];
            la_ssize_t size = archive_read_data(archive, text, sizeof(text) - 1);
            assert(size > 0);
            text[size] = '\0';
            assert(strstr(text, "pkgname = axon\n") && strstr(text, "depend = ncurses\n")
                    && strstr(text, "depend = libalpm.so=16-"));
            metadata++;
        }
        manual += strcmp(name, "usr/share/man/man1/axon.1") == 0;
        license += strcmp(name, "usr/share/licenses/axon/LICENSE") == 0;
    }
    assert(code == ARCHIVE_EOF);
    assert(source ? makefile : executable == 1 && metadata == 1 && manual == 1 && license == 1);
    assert(archive_read_free(archive) == ARCHIVE_OK);
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
    char temporary[] = "/tmp/axon-package-XXXXXX";
    assert(mkdtemp(temporary));
    char destination[512], path[1024];
    snprintf(destination, sizeof(destination), "DESTDIR=%s", temporary);
    char *install[] = {"make", "install", destination, "PREFIX=/usr", "BINDIR=/usr/bin",
                       "MANDIR=/usr/share/man", "LICENSEDIR=/usr/share/licenses/axon", NULL};
    run(install);
    struct stat info;
    snprintf(path, sizeof(path), "%s/usr/bin/axon", temporary);
    assert(stat(path, &info) == 0 && (info.st_mode & 07777) == 0755);
    char *expected = alpm_compute_sha256sum("axon");
    char *actual = alpm_compute_sha256sum(path);
    assert(expected && actual && strcmp(expected, actual) == 0);
    free(expected);
    free(actual);
    snprintf(path, sizeof(path), "%s/keep", temporary);
    FILE *marker = fopen(path, "w");
    assert(marker && fclose(marker) == 0);
    install[1] = "uninstall";
    run(install);
    assert(access(path, F_OK) == 0);
    snprintf(path, sizeof(path), "%s/usr/bin/axon", temporary);
    assert(access(path, F_OK) != 0);
    char *package[] = {"make", "package", "source-dist", "VERSION=0.1.0", "PKGREL=1",
                       "CARCH=x86_64", "SOURCE_DATE_EPOCH=0", NULL};
    run(package);
    const char *archive = "build/axon-0.1.0-1-x86_64.pkg.tar.zst";
    inspect(archive, 0);
    inspect("build/axon-0.1.0.src.tar.zst", 1);
    expected = alpm_compute_sha256sum(archive);
    run(package);
    actual = alpm_compute_sha256sum(archive);
    assert(expected && actual && strcmp(expected, actual) == 0);
    free(expected);
    free(actual);
    snprintf(path, sizeof(path), "%s/db", temporary);
    assert(mkdir(path, 0700) == 0);
    alpm_errno_t error;
    alpm_handle_t *handle = alpm_initialize(temporary, path, &error);
    assert(handle != NULL);
    alpm_pkg_t *pkg = NULL;
    assert(alpm_pkg_load(handle, archive, 1, 0, &pkg) == 0);
    assert(strcmp(alpm_pkg_get_name(pkg), "axon") == 0 && strcmp(alpm_pkg_get_version(pkg), "0.1.0-1") == 0);
    assert(alpm_pkg_free(pkg) == 0 && alpm_release(handle) == 0);
    assert(nftw(temporary, remove_entry, 16, FTW_DEPTH | FTW_PHYS) == 0);
    puts("Staging, uninstall, package metadata, source archive, and reproducibility regressions passed.");
    return(0);
}
