#define _GNU_SOURCE
#include "operation.h"
#include "root_fixture.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <netinet/in.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static char workspace[] = "/tmp/axon-download-XXXXXX", driver[4096], preload[4096];
static unsigned int port, serial;
static pid_t server;
static char output[131072];

typedef struct {
    char path[4096];
    const char *name, *version;
    size_t size;
} package_t;

static void copy(const char *source, const char *target)
{
    FILE *input = fopen(source, "rb"), *destination = fopen(target, "wb");
    assert(input && destination);
    char buffer[16384];
    size_t count;
    while((count = fread(buffer, 1, sizeof(buffer), input)) > 0) {
        assert(fwrite(buffer, 1, count, destination) == count);
    }
    assert(!ferror(input) && fclose(input) == 0 && fclose(destination) == 0);
}

static int spawn(char *const args[], const char *input, int disk_full)
{
    int in[2], out[2];
    assert(pipe(in) == 0 && pipe(out) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if(child == 0) {
        assert(dup2(in[0], STDIN_FILENO) >= 0 && dup2(out[1], STDOUT_FILENO) >= 0 && dup2(out[1], STDERR_FILENO) >= 0);
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        setenv("LC_ALL", "C", 1);
        setenv("http_proxy", "", 1); setenv("https_proxy", "", 1);
        setenv("HTTP_PROXY", "", 1); setenv("HTTPS_PROXY", "", 1);
        setenv("ALL_PROXY", "", 1); setenv("NO_PROXY", "*", 1);
        if(disk_full) {
            setenv("LD_PRELOAD", preload, 1);
            setenv("AXON_TEST_DISK_FULL", "1", 1);
        }
        alarm(30);
        execvp(args[0], args);
        _exit(127);
    }
    close(in[0]); close(out[1]);
    if(input) {
        assert(write(in[1], input, strlen(input)) == (ssize_t)strlen(input));
    }
    close(in[1]);
    size_t used = 0;
    ssize_t count;
    while((count = read(out[0], output + used, sizeof(output) - used - 1)) > 0) {
        used += (size_t)count;
        assert(used < sizeof(output) - 1);
    }
    assert(count == 0);
    output[used] = '\0';
    close(out[0]);
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status));
    return(WEXITSTATUS(status));
}

static long value(const char *key)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "%s=", key);
    const char *line = output;
    while((line = strstr(line, pattern)) != NULL) {
        if(line == output || line[-1] == '\n') {
            return(strtol(line + strlen(pattern), NULL, 10));
        }
        line++;
    }
    return(-1);
}

static void run(const char *mode, const char *config, const char *target, int success, int full)
{
    char *args[] = {driver, (char *)mode, (char *)config, (char *)target, NULL};
    int status = spawn(args, NULL, full);
    if(status != 0 || (value("status") == 0) != success) {
        fprintf(stderr, "driver %s %s expected=%d exit=%d:\n%s", mode, config, success, status, output);
        abort();
    }
    char lock[4096];
    snprintf(lock, sizeof(lock), "%s", config);
    char *slash = strrchr(lock, '/');
    assert(slash);
    strcpy(slash, "/db/db.lck");
    assert(access(lock, F_OK) != 0);
    if(!success) {
        assert(value("committed") == 0);
    }
}

static void package(package_t *pkg, const char *name, const char *version, size_t size)
{
    pkg->name = name; pkg->version = version; pkg->size = size;
    snprintf(pkg->path, sizeof(pkg->path), "%s/mirror/%s-%s-any.pkg.tar", workspace, name, version);
    struct archive *archive = archive_write_new();
    assert(archive && archive_write_set_format_pax_restricted(archive) == ARCHIVE_OK);
    assert(archive_write_open_filename(archive, pkg->path) == ARCHIVE_OK);
    char text[1024], path[128];
    snprintf(text, sizeof(text), "pkgname = %s\npkgver = %s\npkgdesc = Test\narch = any\nsize = %zu\n", name, version, size);
    entry(archive, ".PKGINFO", text);
    char *payload = malloc(1048577);
    assert(payload);
    memset(payload, 'x', 1048576); payload[1048576] = '\0';
    snprintf(path, sizeof(path), "usr/share/%s", name);
    entry(archive, path, payload);
    free(payload);
    assert(archive_write_close(archive) == ARCHIVE_OK && archive_write_free(archive) == ARCHIVE_OK);
}

static void repository(package_t *packages, size_t count)
{
    struct archive *archive = archive_write_new();
    assert(archive && archive_write_set_format_pax_restricted(archive) == ARCHIVE_OK);
    assert(archive_write_open_filename(archive, "mirror/test.db") == ARCHIVE_OK);
    for(size_t index = 0; index < count; index++) {
        package_t *pkg = &packages[index];
        struct stat info;
        assert(stat(pkg->path, &info) == 0);
        char *hash = alpm_compute_sha256sum(pkg->path);
        assert(hash);
        char text[2048], path[256];
        snprintf(text, sizeof(text), "%%NAME%%\n%s\n\n%%VERSION%%\n%s\n\n%%DESC%%\nTest\n\n%%ARCH%%\nany\n\n"
                 "%%FILENAME%%\n%s\n\n%%CSIZE%%\n%jd\n\n%%ISIZE%%\n%zu\n\n%%SHA256SUM%%\n%s\n\n",
                 pkg->name, pkg->version, strrchr(pkg->path, '/') + 1, (intmax_t)info.st_size, pkg->size, hash);
        snprintf(path, sizeof(path), "%s-%s/desc", pkg->name, pkg->version);
        entry(archive, path, text);
        free(hash);
    }
    assert(archive_write_close(archive) == ARCHIVE_OK && archive_write_free(archive) == ARCHIVE_OK);
}

static int send_all(int socket, const void *data, size_t size)
{
    const char *bytes = data;
    while(size) {
        ssize_t sent = send(socket, bytes, size, MSG_NOSIGNAL);
        if(sent <= 0) {
            return(0);
        }
        bytes += sent; size -= (size_t)sent;
    }
    return(1);
}

static void serve(int socket)
{
    char request[8192] = {0};
    size_t used = 0;
    while(strstr(request, "\r\n\r\n") == NULL) {
        ssize_t count = recv(socket, request + used, sizeof(request) - used - 1, 0);
        if(count <= 0) {
            return;
        }
        used += (size_t)count;
        if(used == sizeof(request) - 1) {
            return;
        }
    }
    char route[64], filename[256];
    if(sscanf(request, "GET /%63[^/]/%255s", route, filename) != 2 || strchr(filename, '/')) {
        return;
    }
    FILE *log = fopen("requests", "a");
    assert(log && fprintf(log, "/%s/%s\n", route, filename) > 0 && fclose(log) == 0);
    char path[512];
    snprintf(path, sizeof(path), "mirror/%s", filename);
    int descriptor = open(path, O_RDONLY);
    struct stat info;
    if(strcmp(route, "missing") == 0 || descriptor < 0 || fstat(descriptor, &info) < 0 || !S_ISREG(info.st_mode)) {
        const char *response = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send_all(socket, response, strlen(response));
        if(descriptor >= 0) { close(descriptor); }
        return;
    }
    off_t start = 0;
    const char *range = strstr(request, "Range: bytes=");
    if(range == NULL) { range = strstr(request, "range: bytes="); }
    if(range && strcmp(route, "ok") == 0) { start = strtoll(range + 13, NULL, 10); }
    char header[512];
    if(start < 0 || start >= info.st_size) {
        snprintf(header, sizeof(header), "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    }
    else if(start) {
        snprintf(header, sizeof(header), "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes %jd-%jd/%jd\r\nContent-Length: %jd\r\nConnection: close\r\n\r\n",
                 (intmax_t)start, (intmax_t)info.st_size - 1, (intmax_t)info.st_size, (intmax_t)(info.st_size - start));
    }
    else {
        snprintf(header, sizeof(header), "HTTP/1.1 200 OK\r\nContent-Length: %jd\r\nConnection: close\r\n\r\n", (intmax_t)info.st_size);
    }
    send_all(socket, header, strlen(header));
    if(start >= 0 && start < info.st_size) {
        assert(lseek(descriptor, start, SEEK_SET) == start);
        off_t remaining = strcmp(route, "broken") == 0 ? info.st_size / 2 : info.st_size - start;
        char buffer[16384];
        while(remaining > 0) {
            ssize_t count = read(descriptor, buffer, remaining < (off_t)sizeof(buffer) ? (size_t)remaining : sizeof(buffer));
            if(count <= 0) { break; }
            if(strcmp(route, "corrupt") == 0 && strstr(filename, ".pkg.tar")) { memset(buffer, '!', (size_t)count); }
            if(!send_all(socket, buffer, (size_t)count)) { break; }
            remaining -= count;
        }
    }
    close(descriptor);
}

static void start_server(void)
{
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    assert(listener >= 0);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0 && listen(listener, 16) == 0);
    socklen_t length = sizeof(address);
    assert(getsockname(listener, (struct sockaddr *)&address, &length) == 0);
    port = ntohs(address.sin_port);
    server = fork();
    assert(server >= 0);
    if(server == 0) {
        signal(SIGCHLD, SIG_IGN);
        for(;;) {
            int connection = accept(listener, NULL, NULL);
            if(connection < 0) { _exit(1); }
            pid_t worker = fork();
            if(worker == 0) {
                close(listener);
                alarm(10);
                serve(connection);
                close(connection);
                _exit(0);
            }
            assert(worker > 0);
            close(connection);
        }
    }
    close(listener);
}

static void configuration(char *path, const char *routes, const char *extra, const char *sig, const char *keyring, int second)
{
    char root[4096], directory[8192];
    snprintf(root, sizeof(root), "%s/%u", workspace, ++serial);
    assert(mkdir(root, 0700) == 0);
    const char *names[] = {"root", "db", "cache", "keys"};
    for(size_t index = 0; index < 4; index++) {
        snprintf(directory, sizeof(directory), "%s/%s", root, names[index]);
        assert(mkdir(directory, 0700) == 0);
    }
    snprintf(path, 4096, "%s/%u/pacman.conf", workspace, serial);
    FILE *file = fopen(path, "w");
    assert(file);
    fprintf(file, "[options]\nRootDir = %s/root\nDBPath = %s/db\nCacheDir = %s/cache\nLogFile = %s/log\n"
            "GPGDir = %s%s\nArchitecture = any\nSigLevel = %s\nParallelDownloads = 2\n%s\n[test]\n",
            root, root, root, root, keyring ? keyring : root, keyring ? "" : "/keys", sig, extra);
    char *copy = strdup(routes), *save = NULL;
    assert(copy);
    for(char *route = strtok_r(copy, " ", &save); route; route = strtok_r(NULL, " ", &save)) {
        fprintf(file, "Server = http://127.0.0.1:%u/%s\n", port, route);
    }
    free(copy);
    if(second) { fprintf(file, "[absent]\nServer = http://127.0.0.1:%u/missing\n", port); }
    assert(fclose(file) == 0);
}

static void replace(const char *path, const char *from, const char *to)
{
    FILE *file = fopen(path, "r");
    assert(file);
    char buffer[16384];
    size_t size = fread(buffer, 1, sizeof(buffer) - 1, file);
    buffer[size] = '\0';
    assert(!ferror(file) && fclose(file) == 0);
    file = fopen(path, "w");
    assert(file);
    const char *start = buffer, *match;
    while((match = strstr(start, from)) != NULL) {
        assert(fwrite(start, 1, (size_t)(match - start), file) == (size_t)(match - start));
        fputs(to, file);
        start = match + strlen(from);
    }
    assert(fputs(start, file) >= 0 && fclose(file) == 0);
}

static int exists(const char *config, const char *suffix)
{
    char path[8192];
    snprintf(path, sizeof(path), "%.*s/%s", (int)(strrchr(config, '/') - config), config, suffix);
    return(access(path, F_OK) == 0);
}

static void gpg(const char *home, const char *input, const char *a, const char *b, const char *c, const char *d)
{
    char *args[] = {"gpg", "--homedir", (char *)home, "--batch", "--yes", "--no-auto-key-retrieve",
        "--pinentry-mode", "loopback", "--passphrase", "", (char *)a, (char *)b, (char *)c, (char *)d, NULL};
    if(spawn(args, input, 0) != 0) { fprintf(stderr, "%s", output); abort(); }
}

static void signatures(package_t *pkg)
{
    char signing[4096], trusted[4096], untrusted[4096], path[4096], signature[8192];
    snprintf(signing, sizeof(signing), "%s/signing", workspace);
    snprintf(trusted, sizeof(trusted), "%s/trusted", workspace);
    snprintf(untrusted, sizeof(untrusted), "%s/untrusted", workspace);
    assert(mkdir(signing, 0700) == 0 && mkdir(trusted, 0700) == 0 && mkdir(untrusted, 0700) == 0);
    const char *homes[] = {signing, trusted, untrusted};
    for(size_t index = 0; index < 3; index++) {
        snprintf(signature, sizeof(signature), "%s/gpg.conf", homes[index]);
        write_text(signature, "no-auto-key-retrieve\n");
    }
    gpg(signing, NULL, "--quick-generate-key", "Axon Fixture <fixture@example.invalid>", "ed25519", "sign");
    gpg(signing, NULL, "--output", "public.key", "--export", NULL);
    gpg(signing, NULL, "--with-colons", "--list-keys", NULL, NULL);
    char *start = strstr(output, "fpr:");
    assert(start);
    for(int field = 0; field < 9; field++) { start = strchr(start, ':') + 1; }
    char fingerprint[128];
    size_t size = strcspn(start, ":");
    assert(size < sizeof(fingerprint));
    memcpy(fingerprint, start, size); fingerprint[size] = '\0';
    gpg(trusted, NULL, "--import", "public.key", NULL, NULL);
    gpg(untrusted, NULL, "--import", "public.key", NULL, NULL);
    char ownertrust[160];
    snprintf(ownertrust, sizeof(ownertrust), "%s:6:\n", fingerprint);
    gpg(trusted, ownertrust, "--import-ownertrust", NULL, NULL, NULL);
    gpg(signing, NULL, "--detach-sign", pkg->path, NULL, NULL);
    gpg(signing, NULL, "--detach-sign", "mirror/test.db", NULL, NULL);
    configuration(path, "ok", "", "Required", trusted, 0);
    run("refresh", path, NULL, 1, 0); run("install", path, "fixture", 1, 0);
    configuration(path, "ok", "", "Required", trusted, 0); run("file", path, pkg->path, 1, 0);
    gpg(trusted, NULL, "--delete-key", fingerprint, NULL, NULL);
    configuration(path, "ok", "", "Required", trusted, 0); run("file", path, pkg->path, 0, 0);
    gpg(trusted, NULL, "--import", "public.key", NULL, NULL);
    gpg(trusted, ownertrust, "--import-ownertrust", NULL, NULL, NULL);
    run("file", path, pkg->path, 1, 0);
    configuration(path, "ok", "", "Required", untrusted, 0);
    run("file", path, pkg->path, 0, 0); run("refresh", path, NULL, 0, 0);
    gpg(untrusted, ownertrust, "--import-ownertrust", NULL, NULL, NULL);
    run("refresh", path, NULL, 1, 0); run("install", path, "fixture", 1, 0);
    snprintf(ownertrust, sizeof(ownertrust), "%s:2:\n", fingerprint);
    gpg(untrusted, ownertrust, "--import-ownertrust", NULL, NULL, NULL);
    configuration(path, "ok", "", "PackageRequired DatabaseNever", untrusted, 0);
    run("refresh", path, NULL, 1, 0); run("install", path, "fixture", 0, 0);
    configuration(path, "ok", "", "Required", NULL, 0);
    run("refresh", path, NULL, 0, 0);
    snprintf(signature, sizeof(signature), "%s.sig", pkg->path);
    copy(signature, "valid.sig");
    package_t other;
    package(&other, "different", "1-1", 1048576);
    gpg(signing, NULL, "--detach-sign", other.path, NULL, NULL);
    char other_signature[8192];
    snprintf(other_signature, sizeof(other_signature), "%s.sig", other.path);
    for(int mode = 0; mode < 3; mode++) {
        if(mode == 0) { copy(other_signature, signature); }
        else if(mode == 1) { assert(unlink(signature) == 0); }
        else { write_text(signature, "invalid signature"); }
        configuration(path, "ok", "", "Required", trusted, 0); run("file", path, pkg->path, 0, 0);
        configuration(path, "ok", "", "Required", trusted, 0);
        run("refresh", path, NULL, 1, 0); run("install", path, "fixture", 0, 0);
    }
    copy("valid.sig", signature);
    assert(unlink("mirror/test.db.sig") == 0);
    configuration(path, "ok", "", "Required", trusted, 0); run("refresh", path, NULL, 0, 0);
    write_text("mirror/test.db.sig", "invalid signature");
    configuration(path, "ok", "", "Required", trusted, 0); run("refresh", path, NULL, 0, 0);
    assert(unlink("mirror/test.db.sig") == 0);
    gpg(signing, NULL, "--detach-sign", "mirror/test.db", NULL, NULL);
    run("refresh", path, NULL, 1, 0);
    assert(unlink("mirror/test.db.sig") == 0);
    for(size_t index = 0; index < 3; index++) {
        char *args[] = {"gpgconf", "--homedir", (char *)homes[index], "--kill", "all", NULL};
        assert(spawn(args, NULL, 0) == 0);
    }
}

static int remove_entry(const char *path, const struct stat *info, int type, struct FTW *walk)
{
    (void)info; (void)type; (void)walk;
    return(remove(path));
}

static void timeout(int number)
{
    (void)number;
    if(server > 0) { kill(server, SIGTERM); }
    _exit(124);
}

int main(void)
{
    char *previous = getcwd(NULL, 0);
    assert(previous);
    snprintf(driver, sizeof(driver), "%s/tests/download_driver.out", previous);
    snprintf(preload, sizeof(preload), "%s/tests/disk_full.so", previous);
    assert(mkdtemp(workspace) && chdir(workspace) == 0 && mkdir("mirror", 0700) == 0);
    signal(SIGPIPE, SIG_IGN); signal(SIGALRM, timeout); alarm(180);
    start_server();
    package_t pkg;
    package(&pkg, "fixture", "1-1", 1048576); repository(&pkg, 1);
    char path[4096];
    configuration(path, "missing ok", "", "Never", NULL, 0);
    run("refresh", path, NULL, 1, 0); run("install", path, "fixture", 1, 0);
    assert(exists(path, "root/usr/share/fixture"));
    configuration(path, "broken ok", "", "Never", NULL, 0);
    run("refresh", path, NULL, 1, 0); run("install", path, "fixture", 1, 0);
    configuration(path, "missing", "", "Never", NULL, 0); run("refresh", path, NULL, 0, 0);
    configuration(path, "ok", "", "Never", NULL, 1); run("upgrade-plan", path, "unused", 0, 0);
    assert(value("changed") && value("plan") < 0 && exists(path, "db/sync/test.db"));
    struct stat before, after;
    assert(stat("requests", &before) == 0);
    run("bad-upgrade", path, "fixture", 0, 0);
    assert(stat("requests", &after) == 0 && before.st_size == after.st_size);
    const char *routes[] = {"broken", "corrupt"};
    for(size_t index = 0; index < 2; index++) {
        configuration(path, routes[index], "", "Never", NULL, 0);
        if(index == 0) {
            run("refresh", path, NULL, 0, 0); replace(path, "/broken", "/ok");
            run("refresh", path, NULL, 1, 0); replace(path, "/ok", "/broken");
        }
        else { run("refresh", path, NULL, 1, 0); }
        run("install", path, "fixture", 0, 0);
        assert(!exists(path, "root/usr/share/fixture"));
    }
    configuration(path, "ok", "", "Never", NULL, 0); run("refresh", path, NULL, 1, 0);
    run("install-cancel", path, "fixture", 0, 0);
    assert(value("cancelled") && value("downloads") && !exists(path, "root/usr/share/fixture"));
    configuration(path, "ok", "", "Never", NULL, 0); run("refresh-cancel", path, NULL, 0, 0); assert(value("cancelled"));
    char user[256];
    struct passwd *identity = getpwuid(getuid());
    assert(identity);
    snprintf(user, sizeof(user), "DownloadUser = %s", identity->pw_name);
    const char *extras[] = {"", "DisableSandbox", "DisableSandboxFilesystem", "DisableSandboxSyscalls", user};
    for(size_t index = 0; index < 5; index++) {
        configuration(path, "ok", extras[index], "Never", NULL, 0);
        run("options", path, NULL, 1, 0); run("refresh", path, NULL, 1, 0);
        if(index == 4) { run("install-cancel", path, "fixture", 0, 0); assert(value("cancelled")); }
    }
    configuration(path, "ok", "DownloadUser = axon-nonexistent-test-user", "Never", NULL, 0);
    run("options", path, NULL, 0, 0); run("refresh", path, NULL, 0, 0);
    package_t huge;
    package(&huge, "huge", "1-1", 1048576); repository(&huge, 1);
    configuration(path, "ok", "CheckSpace", "Never", NULL, 0); run("refresh", path, NULL, 1, 0);
    run("install", path, "huge", 0, 1); assert(strstr(output, "space") && !exists(path, "root/usr/share/huge"));
    repository(&pkg, 1);
    if(getuid() != 0) {
        configuration(path, "ok", "", "Never", NULL, 0); run("refresh", path, NULL, 1, 0);
        char cache[8192], extra[8192];
        snprintf(cache, sizeof(cache), "%.*s/cache", (int)(strrchr(path, '/') - path), path);
        assert(chmod(cache, 0500) == 0);
        snprintf(extra, sizeof(extra), "%.*s/cache2", (int)(strrchr(path, '/') - path), path);
        assert(mkdir(extra, 0700) == 0);
        char directive[16384];
        snprintf(directive, sizeof(directive), "CacheDir = %s\n[test]", extra);
        replace(path, "[test]", directive); run("install", path, "fixture", 1, 0);
        assert(exists(path, "cache2/fixture-1-1-any.pkg.tar") && chmod(cache, 0700) == 0);
        configuration(path, "ok", "", "Never", NULL, 0); run("refresh", path, NULL, 1, 0);
        snprintf(cache, sizeof(cache), "%.*s/db/sync", (int)(strrchr(path, '/') - path), path);
        assert(chmod(cache, 0500) == 0); run("refresh", path, NULL, 0, 0); assert(chmod(cache, 0700) == 0);
    }
    signatures(&pkg);
    package_t upgrades[2];
    package(&upgrades[0], "first", "1-1", 1048576); package(&upgrades[1], "second", "1-1", 1048576);
    configuration(path, "ok", "", "Never", NULL, 0);
    run("file", path, upgrades[0].path, 1, 0); run("file", path, upgrades[1].path, 1, 0);
    package(&upgrades[0], "first", "2-1", 1048576); package(&upgrades[1], "second", "2-1", 1048576);
    repository(upgrades, 2); run("upgrade-plan", path, "unused", 1, 0);
    assert(value("plan") == 2 && value("refreshed") && !value("committed")); run("upgrade", path, "unused", 1, 0);
    assert(kill(server, SIGTERM) == 0 && waitpid(server, NULL, 0) == server); server = 0;
    assert(chdir(previous) == 0 && nftw(workspace, remove_entry, 32, FTW_DEPTH | FTW_PHYS) == 0);
    free(previous);
    puts("C HTTP, concurrent downloads, mirrors, interruption, signatures, keyring recovery, sandbox, space, and upgrade regressions passed.");
    return(0);
}
