#define _XOPEN_SOURCE 700

#include "alpm_extra.h"
#include "privilege.h"

#include <archive.h>
#include <archive_entry.h>
#include <assert.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define EXPECT(call, expected) do { \
    alpm_errno_t actual = (call); \
    if(actual != (expected)) { \
        fprintf(stderr, "%s:%d: %s: %s (expected %s)\n", __FILE__, __LINE__, \
                #call, alpm_strerror(actual), alpm_strerror(expected)); \
        abort(); \
    } \
} while(0)

static unsigned int events;

axon_privilege_result_t __wrap_axon_privilege_check(int require_root)
{
    (void)require_root;
    return(AXON_PRIVILEGE_OK);
}

static void event_callback(void *context, alpm_event_t *event)
{
    unsigned int *count = context;
    assert(event != NULL);
    (*count)++;
}

static char *path_join(const char *base, const char *name)
{
    size_t length = strlen(base) + strlen(name) + 2;
    char *path = malloc(length);
    assert(path != NULL);
    assert(snprintf(path, length, "%s/%s", base, name) > 0);
    return(path);
}

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

static void make_archive(const char *path, const char *metadata_name,
                         const char *metadata, const char *payload)
{
    struct archive *archive = archive_write_new();
    assert(archive != NULL);
    assert(archive_write_set_format_pax_restricted(archive) == ARCHIVE_OK);
    assert(archive_write_open_filename(archive, path) == ARCHIVE_OK);
    archive_item(archive, metadata_name, metadata);
    if(payload != NULL) {
        archive_item(archive, "axon-test.txt", payload);
    }
    assert(archive_write_close(archive) == ARCHIVE_OK);
    assert(archive_write_free(archive) == ARCHIVE_OK);
}

static void make_package(const char *path, const char *name, const char *version,
                         const char *extra, const char *payload)
{
    char metadata[1024];
    int size = snprintf(metadata, sizeof(metadata),
                        "pkgname = %s\npkgver = %s\npkgdesc = Axon test package\n"
                        "builddate = 1700000000\nsize = 4\narch = any\n%s",
                        name, version, extra);
    assert(size > 0 && (size_t)size < sizeof(metadata));
    make_archive(path, ".PKGINFO", metadata, payload);
}

static void prepare_commit(axon_alpm_t *core)
{
    axon_alpm_problem_t problem = {0};
    const alpm_list_t *add = NULL;
    const alpm_list_t *remove = NULL;
    EXPECT(axon_alpm_transaction_prepare(core, &problem), ALPM_ERR_OK);
    EXPECT(axon_alpm_transaction_plan(core, &add, &remove), ALPM_ERR_OK);
    assert(add != NULL || remove != NULL);
    EXPECT(axon_alpm_transaction_commit(core, &problem), ALPM_ERR_OK);
    EXPECT(axon_alpm_transaction_commit(core, &problem), ALPM_ERR_TRANS_NOT_PREPARED);
    axon_alpm_problem_clear(&problem);
    EXPECT(axon_alpm_transaction_release(core), ALPM_ERR_OK);
}

static int remove_entry(const char *path, const struct stat *info, int type,
                        struct FTW *walk)
{
    (void)info;
    (void)type;
    (void)walk;
    return(remove(path));
}

int main(void)
{
    char temporary[] = "/tmp/axon-alpm-test-XXXXXX";
    char *base = mkdtemp(temporary);
    assert(base != NULL);
    char *root = path_join(base, "root");
    char *dbpath = path_join(base, "db");
    char *cache = path_join(base, "cache");
    char *mirror = path_join(base, "mirror");
    char *local = path_join(dbpath, "local");
    char *lock = path_join(dbpath, "db.lck");
    char *installed_file = path_join(root, "axon-test.txt");
    char *first = path_join(base, "axon-fixture-1-1-any.pkg.tar");
    char *second = path_join(mirror, "axon-fixture-2-1-any.pkg.tar");
    char *broken = path_join(base, "axon-broken-1-1-any.pkg.tar");
    char *conflict = path_join(base, "axon-conflict-1-1-any.pkg.tar");
    char *wrong_arch = path_join(base, "axon-arch-1-1-other.pkg.tar");
    char *dependency_conflict = path_join(base, "axon-depconflict-1-1-any.pkg.tar");
    char *repo = path_join(mirror, "fixture.db");
    assert(mkdir(root, 0755) == 0);
    assert(mkdir(dbpath, 0755) == 0);
    assert(mkdir(local, 0755) == 0);
    assert(mkdir(cache, 0755) == 0);
    assert(mkdir(mirror, 0755) == 0);
    make_package(first, "axon-fixture", "1-1", "", "one\n");
    make_package(second, "axon-fixture", "2-1", "", "two\n");
    make_package(broken, "axon-broken", "1-1", "depend = axon-missing>=1\n", NULL);
    make_package(conflict, "axon-conflict", "1-1", "", "conflict\n");
    make_package(dependency_conflict, "axon-depconflict", "1-1",
                 "conflict = axon-fixture\n", NULL);
    make_archive(wrong_arch, ".PKGINFO",
                 "pkgname = axon-arch\npkgver = 1-1\narch = axon-other\n", NULL);

    char metadata[2048];
    char *checksum = alpm_compute_sha256sum(second);
    struct stat info;
    assert(checksum != NULL && stat(second, &info) == 0);
    int length = snprintf(metadata, sizeof(metadata),
                         "%%FILENAME%%\naxon-fixture-2-1-any.pkg.tar\n\n"
                         "%%NAME%%\naxon-fixture\n\n%%VERSION%%\n2-1\n\n"
                         "%%DESC%%\nAxon test package\n\n%%ARCH%%\nany\n\n"
                         "%%CSIZE%%\n%lld\n\n%%ISIZE%%\n4\n\n"
                         "%%SHA256SUM%%\n%s\n\n", (long long)info.st_size, checksum);
    assert(length > 0 && (size_t)length < sizeof(metadata));
    free(checksum);
    make_archive(repo, "axon-fixture-2-1/desc", metadata, NULL);

    axon_alpm_t *core = NULL;
    axon_alpm_problem_t problem = {0};
    axon_alpm_callbacks_t callbacks = {.event = event_callback, .context = &events};
    const alpm_list_t *borrowed = NULL;
    const alpm_list_t *removed = NULL;
    alpm_list_t *results = NULL;
    alpm_pkg_t *package = NULL;
    int changed = 7;
    int flags = ALPM_TRANS_FLAG_NOHOOKS | ALPM_TRANS_FLAG_NOSCRIPTLET;

    EXPECT(axon_alpm_init(&core, root, dbpath), ALPM_ERR_OK);
    EXPECT(axon_alpm_refresh(core, 0, &changed), ALPM_ERR_DB_NULL);
    assert(changed == 0);
    EXPECT(axon_alpm_option(core, AXON_ALPM_CACHE_DIR, cache), ALPM_ERR_OK);
    EXPECT(axon_alpm_option(core, AXON_ALPM_ARCHITECTURE, "x86_64"), ALPM_ERR_OK);
    EXPECT(axon_alpm_option(core, (axon_alpm_option_t)99, "x"), ALPM_ERR_WRONG_ARGS);
    EXPECT(axon_alpm_downloads(core, 0), ALPM_ERR_WRONG_ARGS);
    EXPECT(axon_alpm_downloads(core, 2), ALPM_ERR_OK);
    EXPECT(axon_alpm_check_space(core, 0), ALPM_ERR_OK);
    EXPECT(axon_alpm_signatures(core, 0, 0, 0), ALPM_ERR_OK);
    EXPECT(axon_alpm_callbacks(core, &callbacks), ALPM_ERR_OK);
    EXPECT(axon_alpm_search(core, 0, NULL, &results), ALPM_ERR_OK);
    assert(results == NULL);
    EXPECT(axon_alpm_load(core, first, &package), ALPM_ERR_OK);
    assert(strcmp(alpm_pkg_get_name(package), "axon-fixture") == 0);
    assert(alpm_pkg_free(package) == 0);
    package = NULL;
    EXPECT(axon_alpm_transaction_file(core, first), ALPM_ERR_TRANS_NOT_INITIALIZED);
    EXPECT(axon_alpm_transaction_commit(core, &problem), ALPM_ERR_TRANS_NOT_PREPARED);
    EXPECT(axon_alpm_transaction_begin(core, flags), ALPM_ERR_OK);
    assert(access(lock, F_OK) == 0);
    axon_alpm_t *competitor = NULL;
    EXPECT(axon_alpm_init(&competitor, root, dbpath), ALPM_ERR_OK);
    EXPECT(axon_alpm_transaction_begin(competitor, flags), ALPM_ERR_HANDLE_LOCK);
    assert(axon_alpm_release(&competitor) == 0);
    assert(access(lock, F_OK) == 0);
    EXPECT(axon_alpm_transaction_begin(core, flags), ALPM_ERR_TRANS_NOT_NULL);
    EXPECT(axon_alpm_refresh(core, 0, NULL), ALPM_ERR_TRANS_NOT_NULL);
    EXPECT(axon_alpm_transaction_file(core, first), ALPM_ERR_OK);
    EXPECT(axon_alpm_transaction_file(core, first), ALPM_ERR_TRANS_DUP_TARGET);
    prepare_commit(core);
    assert(access(lock, F_OK) != 0 && access(installed_file, F_OK) == 0);
    EXPECT(axon_alpm_local_package(core, "axon-fixture", &package), ALPM_ERR_OK);
    assert(strcmp(alpm_pkg_get_version(package), "1-1") == 0);
    EXPECT(axon_alpm_set_reason(core, "axon-fixture", ALPM_PKG_REASON_DEPEND), ALPM_ERR_OK);
    assert(alpm_pkg_get_reason(package) == ALPM_PKG_REASON_DEPEND);
    EXPECT(axon_alpm_set_reason(core, "axon-fixture", ALPM_PKG_REASON_EXPLICIT), ALPM_ERR_OK);

    EXPECT(axon_alpm_transaction_begin(core, flags), ALPM_ERR_OK);
    EXPECT(axon_alpm_transaction_file(core, wrong_arch), ALPM_ERR_OK);
    EXPECT(axon_alpm_transaction_prepare(core, &problem), ALPM_ERR_PKG_INVALID_ARCH);
    assert(problem.details != NULL);
    axon_alpm_problem_clear(&problem);
    EXPECT(axon_alpm_transaction_release(core), ALPM_ERR_OK);

    EXPECT(axon_alpm_transaction_begin(core, flags), ALPM_ERR_OK);
    EXPECT(axon_alpm_transaction_file(core, conflict), ALPM_ERR_OK);
    EXPECT(axon_alpm_transaction_prepare(core, &problem), ALPM_ERR_OK);
    EXPECT(axon_alpm_transaction_commit(core, &problem), ALPM_ERR_FILE_CONFLICTS);
    assert(problem.details != NULL);
    axon_alpm_problem_clear(&problem);
    EXPECT(axon_alpm_transaction_release(core), ALPM_ERR_OK);
    EXPECT(axon_alpm_local_package(core, "axon-conflict", &package), ALPM_ERR_PKG_NOT_FOUND);

    EXPECT(axon_alpm_transaction_begin(core, flags), ALPM_ERR_OK);
    EXPECT(axon_alpm_transaction_file(core, first), ALPM_ERR_OK);
    EXPECT(axon_alpm_transaction_file(core, dependency_conflict), ALPM_ERR_OK);
    EXPECT(axon_alpm_transaction_prepare(core, &problem), ALPM_ERR_CONFLICTING_DEPS);
    assert(problem.details != NULL);
    axon_alpm_problem_clear(&problem);
    EXPECT(axon_alpm_transaction_release(core), ALPM_ERR_OK);

    char *url = malloc(strlen(mirror) + 8);
    assert(url != NULL);
    sprintf(url, "file://%s", mirror);
    alpm_list_t server = {.data = url};
    EXPECT(axon_alpm_repo_add(core, "../bad", 0, &server, ALPM_DB_USAGE_ALL), ALPM_ERR_WRONG_ARGS);
    EXPECT(axon_alpm_repo_add(core, "fixture", 0, &server, ALPM_DB_USAGE_ALL), ALPM_ERR_OK);
    EXPECT(axon_alpm_repo_add(core, "fixture", 0, &server, ALPM_DB_USAGE_ALL), ALPM_ERR_DB_NOT_NULL);
    EXPECT(axon_alpm_repositories(core, &borrowed), ALPM_ERR_OK);
    assert(alpm_list_count(borrowed) == 1);
    EXPECT(axon_alpm_refresh(core, 1, &changed), ALPM_ERR_OK);
    assert(changed == 1 && access(lock, F_OK) != 0);
    EXPECT(axon_alpm_sync_package(core, NULL, "axon-fixture", &package), ALPM_ERR_OK);
    assert(strcmp(alpm_pkg_get_version(package), "2-1") == 0);
    EXPECT(axon_alpm_sync_package(core, "unknown", "axon-fixture", &package), ALPM_ERR_DB_NOT_FOUND);
    alpm_list_t pattern = {.data = "Axon test"};
    EXPECT(axon_alpm_search(core, 1, &pattern, &results), ALPM_ERR_OK);
    assert(alpm_list_count(results) == 1);
    alpm_list_free(results);
    results = NULL;
    pattern.data = "[";
    EXPECT(axon_alpm_search(core, 1, &pattern, &results), ALPM_ERR_INVALID_REGEX);
    assert(results == NULL);
    EXPECT(axon_alpm_upgrades(core, &results), ALPM_ERR_OK);
    assert(alpm_list_count(results) == 1);
    alpm_list_free(results);
    results = NULL;

    EXPECT(axon_alpm_transaction_begin(core, flags), ALPM_ERR_OK);
    EXPECT(axon_alpm_transaction_upgrade(core, 0), ALPM_ERR_OK);
    prepare_commit(core);
    EXPECT(axon_alpm_local_package(core, "axon-fixture", &package), ALPM_ERR_OK);
    assert(strcmp(alpm_pkg_get_version(package), "2-1") == 0);
    FILE *payload = fopen(installed_file, "r");
    char content[16];
    assert(payload != NULL && fgets(content, sizeof(content), payload) != NULL);
    assert(strcmp(content, "two\n") == 0);
    assert(fclose(payload) == 0);
    EXPECT(axon_alpm_upgrades(core, &results), ALPM_ERR_OK);
    assert(results == NULL);

    EXPECT(axon_alpm_transaction_begin(core, flags), ALPM_ERR_OK);
    EXPECT(axon_alpm_transaction_file(core, broken), ALPM_ERR_OK);
    EXPECT(axon_alpm_transaction_prepare(core, &problem), ALPM_ERR_UNSATISFIED_DEPS);
    assert(problem.details != NULL);
    alpm_depmissing_t *missing = problem.details->data;
    assert(strcmp(missing->depend->name, "axon-missing") == 0);
    axon_alpm_problem_clear(&problem);
    EXPECT(axon_alpm_transaction_release(core), ALPM_ERR_OK);
    EXPECT(axon_alpm_transaction_begin(core, flags), ALPM_ERR_OK);
    EXPECT(axon_alpm_transaction_install(core, "fixture", "axon-fixture"), ALPM_ERR_OK);
    EXPECT(axon_alpm_transaction_prepare(core, &problem), ALPM_ERR_OK);
    EXPECT(axon_alpm_transaction_remove(core, "axon-fixture"), ALPM_ERR_TRANS_NOT_INITIALIZED);
    EXPECT(axon_alpm_transaction_plan(core, &borrowed, &removed), ALPM_ERR_OK);
    assert(borrowed != NULL);
    EXPECT(axon_alpm_transaction_release(core), ALPM_ERR_OK);

    EXPECT(axon_alpm_transaction_begin(core, flags), ALPM_ERR_OK);
    EXPECT(axon_alpm_transaction_remove(core, "axon-fixture"), ALPM_ERR_OK);
    prepare_commit(core);
    assert(access(installed_file, F_OK) != 0);
    EXPECT(axon_alpm_local_package(core, "axon-fixture", &package), ALPM_ERR_PKG_NOT_FOUND);
    EXPECT(axon_alpm_repo_remove(core, "fixture"), ALPM_ERR_OK);
    EXPECT(axon_alpm_repo_remove(core, "fixture"), ALPM_ERR_DB_NOT_FOUND);
    EXPECT(axon_alpm_transaction_release(core), ALPM_ERR_OK);
    EXPECT(axon_alpm_transaction_begin(core, flags), ALPM_ERR_OK);
    assert(axon_alpm_release(&core) == 0);
    assert(core == NULL && access(lock, F_OK) != 0);
    assert(events > 0);

    free(root);
    free(dbpath);
    free(cache);
    free(mirror);
    free(local);
    free(lock);
    free(installed_file);
    free(first);
    free(second);
    free(broken);
    free(conflict);
    free(wrong_arch);
    free(dependency_conflict);
    free(repo);
    free(url);
    assert(nftw(base, remove_entry, 16, FTW_DEPTH | FTW_PHYS) == 0);
    puts("ALPM backend integration checks passed.");
    return(0);
}
