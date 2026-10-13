#define _GNU_SOURCE
#include "maintenance.h"
#include "alpm_internal.h"

#include <archive.h>
#include <archive_entry.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <unistd.h>

static void report(axon_maintenance_message_t message, void *context, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    char *text = NULL;
    if(vasprintf(&text, format, args) >= 0) {
        message(context, text);
    }
    else {
        message(context, "Inspection diagnostic unavailable: out of memory.");
    }
    free(text);
    va_end(args);
}

static int safe_name(const char *name)
{
    if(name == NULL || !*name || *name == '/') {
        return(0);
    }
    for(const char *part = name; *part;) {
        size_t length = strcspn(part, "/");
        if(length == 0 || (length == 1 && *part == '.') || (length == 2 && strncmp(part, "..", 2) == 0)) {
            return(0);
        }
        part += length;
        if(*part == '/') {
            part++;
        }
    }
    return(1);
}

static int open_rooted(int root, const char *name, int flags)
{
    struct open_how how = {.flags = (unsigned long long)flags, .resolve = RESOLVE_IN_ROOT | RESOLVE_NO_MAGICLINKS};
    return((int)syscall(SYS_openat2, root, name, &how, sizeof(how)));
}

static int backup(alpm_pkg_t *package, const char *name)
{
    for(alpm_list_t *item = alpm_pkg_get_backup(package); item; item = item->next) {
        alpm_backup_t *file = item->data;
        if(strcmp(file->name, name) == 0) {
            return(1);
        }
    }
    return(0);
}

alpm_errno_t axon_maintenance_writable(axon_alpm_t *core,
    const alpm_list_t *additions, const alpm_list_t *removals,
    axon_maintenance_message_t message, void *context)
{
    int root = open(alpm_option_get_root(core->handle), O_PATH | O_DIRECTORY | O_CLOEXEC);
    if(root < 0) {
        return(ALPM_ERR_SYSTEM);
    }
    alpm_errno_t result = ALPM_ERR_OK;
    const alpm_list_t *lists[] = {additions, removals};
    for(size_t list = 0; list < 2 && result == ALPM_ERR_OK; list++) {
        for(const alpm_list_t *item = lists[list]; item && result == ALPM_ERR_OK; item = item->next) {
            alpm_pkg_t *package = item->data;
            alpm_filelist_t *files = alpm_pkg_get_files(package);
            if(files == NULL) {
                result = ALPM_ERR_DB_INVALID;
                break;
            }
            for(size_t index = 0; index < files->count; index++) {
                const char *name = files->files[index].name;
                if(list == 0 && alpm_option_match_noextract(core->handle, name) == 0) {
                    continue;
                }
                if(!safe_name(name)) {
                    result = ALPM_ERR_PKG_INVALID;
                    break;
                }
                char *parent = strdup(name);
                if(parent == NULL) {
                    result = ALPM_ERR_MEMORY;
                    break;
                }
                int descriptor;
                while((descriptor = open_rooted(root, parent, O_PATH | O_NOFOLLOW | O_CLOEXEC)) < 0 && errno == ENOENT) {
                    char *slash = strrchr(parent, '/');
                    if(slash == NULL) {
                        strcpy(parent, ".");
                    }
                    else {
                        *slash = '\0';
                    }
                }
                struct statvfs filesystem;
                if(descriptor < 0 || fstatvfs(descriptor, &filesystem) < 0 || (filesystem.f_flag & ST_RDONLY)) {
                    report(message, context, "%s: %s%s: destination is read-only or inaccessible; no commit attempted",
                           alpm_pkg_get_name(package), alpm_option_get_root(core->handle), name);
                    result = ALPM_ERR_SYSTEM;
                }
                if(descriptor >= 0) {
                    close(descriptor);
                }
                free(parent);
                if(result != ALPM_ERR_OK) {
                    break;
                }
            }
        }
    }
    close(root);
    return(result);
}

static int compare_hash(int root, const char *name, struct archive_entry *entry)
{
    const unsigned char *digest = archive_entry_digest(entry, ARCHIVE_ENTRY_DIGEST_SHA256);
    int size = 32;
    int present = 0;
    for(int index = 0; digest != NULL && index < size; index++) {
        present |= digest[index];
    }
    if(!present) {
        digest = archive_entry_digest(entry, ARCHIVE_ENTRY_DIGEST_MD5);
        size = 16;
        for(int index = 0; digest != NULL && index < size; index++) {
            present |= digest[index];
        }
    }
    if(!present) {
        return(-2);
    }
    int descriptor = open_rooted(root, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    struct stat info;
    if(descriptor < 0) {
        return(-1);
    }
    if(fstat(descriptor, &info) < 0 || !S_ISREG(info.st_mode)) {
        close(descriptor);
        return(-1);
    }
    char path[64], expected[65];
    snprintf(path, sizeof(path), "/proc/self/fd/%d", descriptor);
    for(int index = 0; index < size; index++) {
        snprintf(expected + index * 2, 3, "%02x", digest[index]);
    }
    char *actual = size == 32 ? alpm_compute_sha256sum(path) : alpm_compute_md5sum(path);
    int result = actual == NULL ? -1 : strcmp(expected, actual) != 0;
    free(actual);
    close(descriptor);
    return(result);
}

alpm_errno_t axon_maintenance_files(axon_alpm_t *core, alpm_pkg_t *package,
    int metadata, int checksums, axon_maintenance_message_t message, void *context)
{
    if(core == NULL || package == NULL || message == NULL || (checksums && !metadata)) {
        return(ALPM_ERR_WRONG_ARGS);
    }
    const char *root_path = alpm_option_get_root(core->handle);
    const char *owner = alpm_pkg_get_name(package);
    int root = open(root_path, O_PATH | O_DIRECTORY | O_CLOEXEC);
    if(root < 0) {
        report(message, context, "%s: cannot inspect root %s: %s", owner, root_path, strerror(errno));
        return(ALPM_ERR_SYSTEM);
    }
    alpm_filelist_t *files = alpm_pkg_get_files(package);
    if(files == NULL) {
        close(root);
        return(ALPM_ERR_DB_INVALID);
    }
    int failed = 0;
    for(size_t index = 0; index < files->count; index++) {
        const char *name = files->files[index].name;
        if(!safe_name(name)) {
            report(message, context, "%s: unsafe database file path %s; preserve and repair package records", owner, name);
            failed = 1;
            continue;
        }
        int descriptor = open_rooted(root, name, O_PATH | O_NOFOLLOW | O_CLOEXEC);
        if(descriptor < 0) {
            if(errno == ENOENT && alpm_option_match_noextract(core->handle, name) == 0) {
                continue;
            }
            report(message, context, "%s: %s%s: %s; restore from backup or reinstall the owning package",
                   owner, root_path, name, strerror(errno));
            failed = 1;
        }
        else {
            close(descriptor);
        }
    }
    if(!metadata) {
        close(root);
        return(failed ? ALPM_ERR_PKG_INVALID : ALPM_ERR_OK);
    }
    struct archive *mtree = alpm_pkg_mtree_open(package);
    unsigned char *seen = calloc(files->count ? files->count : 1, 1);
    if(mtree == NULL || seen == NULL) {
        report(message, context, "%s: metadata records unavailable; preserve the database and reinstall from a trusted package", owner);
        if(mtree != NULL) {
            alpm_pkg_mtree_close(package, mtree);
        }
        free(seen);
        close(root);
        return(mtree == NULL ? ALPM_ERR_DB_INVALID : ALPM_ERR_MEMORY);
    }
    struct archive_entry *entry = NULL;
    int next;
    while((next = alpm_pkg_mtree_next(package, mtree, &entry)) == ARCHIVE_OK) {
        const char *name = archive_entry_pathname(entry);
        if(name == NULL) {
            failed = 1;
            continue;
        }
        if(strncmp(name, "./", 2) == 0) {
            name += 2;
        }
        if(strcmp(name, ".") == 0 || strcmp(name, ".PKGINFO") == 0
                || strcmp(name, ".BUILDINFO") == 0 || strcmp(name, ".INSTALL") == 0 || strcmp(name, ".CHANGELOG") == 0) {
            continue;
        }
        if(!safe_name(name)) {
            report(message, context, "%s: unsafe metadata path %s", owner, name);
            failed = 1;
            continue;
        }
        const char *record_name = name;
        for(size_t index = 0; index < files->count; index++) {
            const char *record = files->files[index].name;
            size_t size = strlen(record);
            if(size && record[size - 1] == '/') {
                size--;
            }
            if(strlen(name) == size && strncmp(name, record, size) == 0) {
                seen[index] = 1;
                record_name = record;
            }
        }
        int descriptor = open_rooted(root, name, O_PATH | O_NOFOLLOW | O_CLOEXEC);
        if(descriptor < 0 && errno == ENOENT
                && alpm_option_match_noextract(core->handle, record_name) == 0) {
            continue;
        }
        struct stat info;
        if(descriptor < 0 || fstat(descriptor, &info) < 0) {
            report(message, context, "%s: %s%s: cannot read metadata: %s", owner, root_path, name, strerror(errno));
            if(descriptor >= 0) {
                close(descriptor);
            }
            failed = 1;
            continue;
        }
        int changed = 0;
        if((info.st_mode & S_IFMT) != archive_entry_filetype(entry)
                || (info.st_mode & 07777) != archive_entry_perm(entry)
                || info.st_uid != archive_entry_uid(entry) || info.st_gid != archive_entry_gid(entry)) {
            report(message, context, "%s: %s%s: type, permissions, or ownership mismatch; review metadata and reinstall if unintended", owner, root_path, name);
            failed = 1;
        }
        if(S_ISREG(info.st_mode) && archive_entry_filetype(entry) == AE_IFREG) {
            changed = info.st_size != archive_entry_size(entry)
                || (archive_entry_mtime_is_set(entry) && info.st_mtime != archive_entry_mtime(entry));
            if(checksums) {
                int hash = compare_hash(root, name, entry);
                if(hash < 0) {
                    report(message, context, "%s: %s%s: %s", owner, root_path, name,
                        hash == -2 ? "checksum record unavailable" : "cannot read file for checksum verification");
                    failed = 1;
                }
                else if(hash) {
                    changed = 1;
                }
            }
        }
        if(S_ISLNK(info.st_mode) && archive_entry_filetype(entry) == AE_IFLNK) {
            char target[4096];
            ssize_t size = readlinkat(descriptor, "", target, sizeof(target));
            const char *expected = archive_entry_symlink(entry);
            if(size < 0 || expected == NULL || (size_t)size != strlen(expected)
                    || memcmp(target, expected, (size_t)size) != 0) {
                changed = 1;
            }
        }
        close(descriptor);
        if(changed) {
            int configuration = backup(package, name);
            report(message, context, "%s: %s%s: %s", owner, root_path, name,
                configuration ? "modified backup configuration; review and preserve local changes"
                : "file metadata/content mismatch; reinstall the owning package from a trusted source");
            failed |= !configuration;
        }
    }
    if(next != ARCHIVE_EOF) {
        report(message, context, "%s: malformed or truncated metadata records", owner);
        failed = 1;
    }
    for(size_t index = 0; index < files->count; index++) {
        if(!seen[index]) {
            report(message, context, "%s: %s%s: metadata record missing", owner, root_path, files->files[index].name);
            failed = 1;
        }
    }
    free(seen);
    if(alpm_pkg_mtree_close(package, mtree) != ARCHIVE_OK) {
        failed = 1;
    }
    close(root);
    return(failed ? ALPM_ERR_PKG_INVALID : ALPM_ERR_OK);
}

static char *read_record(int descriptor)
{
    struct stat info;
    if(fstat(descriptor, &info) < 0 || !S_ISREG(info.st_mode) || info.st_size < 0 || info.st_size > 16 * 1024 * 1024) {
        return(NULL);
    }
    size_t size = (size_t)info.st_size;
    char *text = calloc(size + 1, 1);
    if(text == NULL) {
        return(NULL);
    }
    size_t used = 0;
    while(used < size) {
        ssize_t count = read(descriptor, text + used, size - used);
        if(count <= 0) {
            free(text);
            return(NULL);
        }
        used += (size_t)count;
    }
    if(memchr(text, '\0', size) != NULL || (size && text[size - 1] != '\n')) {
        free(text);
        return(NULL);
    }
    return(text);
}

static char *field(const char *text, const char *key)
{
    const char *value = strstr(text, key);
    if(value == NULL || (value != text && value[-1] != '\n') || strstr(value + strlen(key), key) != NULL) {
        return(NULL);
    }
    value += strlen(key);
    size_t size = strcspn(value, "\n");
    if(!size || value[size] != '\n' || value[size + 1] != '\n') {
        return(NULL);
    }
    return(strndup(value, size));
}

static int description(const char *text, const char *directory)
{
    char *name = field(text, "%NAME%\n");
    char *version = field(text, "%VERSION%\n");
    char *expected = NULL;
    int valid = name != NULL && version != NULL && safe_name(name) && strchr(name, '/') == NULL
        && strchr(version, '/') == NULL && asprintf(&expected, "%s-%s", name, version) >= 0;
    valid = valid && strcmp(expected, directory) == 0;
    free(expected);
    free(name);
    free(version);
    return(valid);
}

static int file_records(const char *text)
{
    if(strncmp(text, "%FILES%\n", 8) != 0) {
        return(0);
    }
    char *copy = strdup(text);
    if(copy == NULL) {
        return(0);
    }
    int valid = 1, files = 0;
    char *save = NULL;
    for(char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if(*line == '%') {
            if(strcmp(line, "%FILES%") != 0 && strcmp(line, "%BACKUP%") != 0) {
                valid = 0;
                break;
            }
            files = 1;
            continue;
        }
        char *tab = strchr(line, '\t');
        if(tab != NULL) {
            *tab = '\0';
        }
        if(!files || !safe_name(line)) {
            valid = 0;
            break;
        }
    }
    free(copy);
    return(valid);
}

alpm_errno_t axon_maintenance_records(axon_alpm_t *core, int repositories,
    axon_maintenance_message_t message, void *context)
{
    if(core == NULL || message == NULL) {
        return(ALPM_ERR_WRONG_ARGS);
    }
    const char *dbpath = alpm_option_get_dbpath(core->handle);
    char *path = NULL;
    if(asprintf(&path, "%slocal", dbpath) < 0) {
        return(ALPM_ERR_MEMORY);
    }
    DIR *directory = opendir(path);
    if(directory == NULL) {
        report(message, context, "%s: cannot inspect database: %s", path, strerror(errno));
        free(path);
        return(ALPM_ERR_DB_OPEN);
    }
    int failed = 0;
    struct dirent *item;
    errno = 0;
    while((item = readdir(directory)) != NULL) {
        if(strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0 || strcmp(item->d_name, "ALPM_DB_VERSION") == 0) {
            continue;
        }
        int package = openat(dirfd(directory), item->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        int valid = package >= 0;
        const char *records[] = {"desc", "files"};
        for(size_t index = 0; valid && index < 2; index++) {
            int descriptor = openat(package, records[index], O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
            char *text = descriptor >= 0 ? read_record(descriptor) : NULL;
            valid = text != NULL && (index == 0 ? description(text, item->d_name) : file_records(text));
            if(!valid) {
                report(message, context, "%s/%s/%s: missing, unreadable, or malformed package record; preserve database and restore from backup or reinstall", path, item->d_name, records[index]);
            }
            free(text);
            if(descriptor >= 0) {
                close(descriptor);
            }
        }
        if(package >= 0) {
            close(package);
        }
        else {
            report(message, context, "%s/%s: cannot inspect package directory", path, item->d_name);
        }
        failed |= !valid;
        errno = 0;
    }
    failed |= errno != 0;
    closedir(directory);
    free(path);
    for(alpm_list_t *repo = repositories ? alpm_get_syncdbs(core->handle) : NULL; repo; repo = repo->next) {
        const char *name = alpm_db_get_name(repo->data);
        path = NULL;
        if(asprintf(&path, "%ssync/%s.db", dbpath, name) < 0) {
            return(ALPM_ERR_MEMORY);
        }
        struct archive *archive = archive_read_new();
        int valid = archive != NULL;
        if(valid) {
            archive_read_support_filter_all(archive);
            archive_read_support_format_tar(archive);
            valid = archive_read_open_filename(archive, path, 16384) == ARCHIVE_OK;
        }
        struct archive_entry *entry = NULL;
        int next = ARCHIVE_EOF;
        while(valid && (next = archive_read_next_header(archive, &entry)) == ARCHIVE_OK) {
            const char *record = archive_entry_pathname(entry);
            if(record == NULL || !safe_name(record)) {
                valid = 0;
                break;
            }
            if(archive_entry_filetype(entry) == AE_IFDIR) {
                continue;
            }
            const char *slash = strchr(record, '/');
            if(slash == NULL || strcmp(slash + 1, "desc") != 0 || archive_entry_filetype(entry) != AE_IFREG) {
                valid = 0;
                break;
            }
            la_int64_t size = archive_entry_size(entry);
            if(size <= 0 || size > 16 * 1024 * 1024) {
                valid = 0;
                break;
            }
            char *text = calloc((size_t)size + 1, 1);
            char *package = strndup(record, (size_t)(slash - record));
            valid = text != NULL && package != NULL
                && archive_read_data(archive, text, (size_t)size) == size
                && memchr(text, '\0', (size_t)size) == NULL && text[size - 1] == '\n'
                && description(text, package);
            free(text);
            free(package);
        }
        valid &= next == ARCHIVE_EOF;
        if(archive != NULL && archive_read_free(archive) != ARCHIVE_OK) {
            valid = 0;
        }
        if(!valid) {
            report(message, context, "%s: missing, unreadable, or malformed repository database; preserve it and perform a full refresh-and-upgrade", path);
            failed = 1;
        }
        free(path);
    }
    return(failed ? ALPM_ERR_DB_INVALID : ALPM_ERR_OK);
}
