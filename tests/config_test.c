#define _XOPEN_SOURCE 700

#include "pacman_config.h"
#include "alpm_extra.h"

#include <assert.h>
#include <errno.h>
#include <ftw.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

static char workspace[] = "/tmp/axon-config-test-XXXXXX";

static void write_file(const char *path, const char *format, ...)
{
    FILE *file = fopen(path, "w");
    assert(file != NULL);
    va_list args;
    va_start(args, format);
    assert(vfprintf(file, format, args) >= 0);
    va_end(args);
    assert(fclose(file) == 0);
}

static axon_config_t *load(const char *path)
{
    axon_config_t *config = NULL;
    axon_config_error_t error;
    axon_config_result_t result = axon_config_load(path, &config, &error);
    if(result != AXON_CONFIG_OK) {
        fprintf(stderr, "%s:%zu: %s (%d)\n", error.file, error.line, error.message, result);
        abort();
    }
    assert(config != NULL && error.code == AXON_CONFIG_OK);
    return(config);
}

static void reject(const char *text, axon_config_result_t expected, size_t line)
{
    write_file("invalid.conf", "%s", text);
    axon_config_t *config = NULL;
    axon_config_error_t error;
    axon_config_result_t result = axon_config_load("invalid.conf", &config, &error);
    if(result != expected) {
        fprintf(stderr, "unexpected result %d instead of %d: %s\n%s\n", result,
                expected, error.message, text);
        abort();
    }
    assert(config == NULL && error.code == expected);
    assert(strcmp(error.file, "invalid.conf") == 0 && error.line == line);
    assert(error.message[0] != '\0');
}

static const char *string_at(const alpm_list_t *list, size_t index)
{
    while(index-- > 0) {
        assert(list != NULL);
        list = list->next;
    }
    assert(list != NULL);
    return(list->data);
}

static void defaults(void)
{
    write_file("defaults.conf", "# comment\r\n[options]\r\nIgnorePkg =\r\n");
    axon_config_t *config = load("defaults.conf");
    const axon_config_options_t *options = axon_config_options(config);
    assert(strcmp(options->root_dir, "/") == 0);
    assert(strcmp(options->db_path, "/var/lib/pacman/") == 0);
    assert(strcmp(options->log_file, "/var/log/pacman.log") == 0);
    assert(strcmp(string_at(options->cache_dirs, 0), "/var/cache/pacman/pkg/") == 0);
    assert(options->siglevel == (ALPM_SIG_PACKAGE | ALPM_SIG_DATABASE));
    assert(options->local_file_siglevel == options->siglevel);
    assert(options->remote_file_siglevel == options->siglevel);
    assert(options->parallel_downloads == 1 && options->clean_keep_installed);
    assert(options->architectures == NULL && options->ignore_packages == NULL);
    axon_config_t *original = config;
    axon_config_error_t error;
    assert(axon_config_load("defaults.conf", &config, &error) == AXON_CONFIG_ARGUMENT);
    assert(config == original);
    axon_config_free(&config);
    axon_config_free(&config);
    assert(config == NULL);

    write_file("root.conf", "[options]\nRootDir = %s/root/\n", workspace);
    config = load("root.conf");
    options = axon_config_options(config);
    char expected[512];
    snprintf(expected, sizeof(expected), "%s/root/var/lib/pacman/", workspace);
    assert(strcmp(options->db_path, expected) == 0);
    snprintf(expected, sizeof(expected), "%s/root/var/log/pacman.log", workspace);
    assert(strcmp(options->log_file, expected) == 0);
    assert(strcmp(options->gpg_dir, "/etc/pacman.d/gnupg/") == 0);
    axon_config_free(&config);
}

static void includes_and_policies(void)
{
    assert(mkdir("parts", 0700) == 0);
    write_file("parts/01.conf", "IgnorePkg = first\n[core]\nInclude = mirrors.conf\n"
               "SigLevel = DatabaseOptional\nUsage = Sync\nUsage = Search\n");
    write_file("parts/02.conf", "[extra]\nInclude = mirrors.conf\nSigLevel = PackageNever\n");
    write_file("mirrors.conf", "Server = https://example.org/$repo/os/$arch#fragment\n"
               "CacheServer = file:///cache/$repo/$arch\n");
    write_file("includes.conf", "[options]\nArchitecture = auto aarch64\n"
               "SigLevel = Required\nLocalFileSigLevel = Optional\n"
               "RemoteFileSigLevel = PackageTrustedOnly\nInclude = parts/*.conf\n"
               "Server = https://second.example/$repo/$arch\n[options]\n"
               "SigLevel = TrustAll\nIgnorePkg = second\n");
    axon_config_t *config = load("includes.conf");
    const axon_config_options_t *options = axon_config_options(config);
    struct utsname machine;
    assert(uname(&machine) == 0);
    assert(strcmp(string_at(options->architectures, 0), machine.machine) == 0);
    assert(strcmp(string_at(options->architectures, 1), "aarch64") == 0);
    assert(strcmp(string_at(options->ignore_packages, 0), "first") == 0);
    assert(strcmp(string_at(options->ignore_packages, 1), "second") == 0);
    assert((options->local_file_siglevel & ALPM_SIG_PACKAGE_OPTIONAL) != 0);
    assert((options->local_file_siglevel & ALPM_SIG_PACKAGE_UNKNOWN_OK) != 0);
    assert((options->remote_file_siglevel & ALPM_SIG_PACKAGE_UNKNOWN_OK) == 0);
    const alpm_list_t *repos = axon_config_repositories(config);
    assert(alpm_list_count(repos) == 2);
    const axon_config_repo_t *core = repos->data;
    const axon_config_repo_t *extra = repos->next->data;
    assert(strcmp(core->name, "core") == 0 && strcmp(extra->name, "extra") == 0);
    assert(core->usage == (ALPM_DB_USAGE_SYNC | ALPM_DB_USAGE_SEARCH));
    assert((core->siglevel & ALPM_SIG_DATABASE_OPTIONAL) != 0);
    assert((core->siglevel & ALPM_SIG_PACKAGE_UNKNOWN_OK) != 0);
    assert((extra->siglevel & (ALPM_SIG_PACKAGE | ALPM_SIG_PACKAGE_OPTIONAL)) == 0);
    assert(alpm_list_count(core->servers) == 1 && alpm_list_count(extra->servers) == 2);
    char expected[512];
    snprintf(expected, sizeof(expected), "https://example.org/core/os/%s#fragment", machine.machine);
    assert(strcmp(string_at(core->servers, 0), expected) == 0);
    snprintf(expected, sizeof(expected), "https://second.example/extra/%s", machine.machine);
    assert(strcmp(string_at(extra->servers, 1), expected) == 0);
    axon_config_free(&config);

    write_file("nested.conf", "[options]\nInclude = bad-child.conf\n");
    write_file("bad-child.conf", "# child\nSigLevel = Typo\n");
    axon_config_error_t error;
    assert(axon_config_load("nested.conf", &config, &error) == AXON_CONFIG_VALUE);
    assert(strcmp(error.file, "bad-child.conf") == 0 && error.line == 2);
    write_file("cycle.conf", "[options]\nInclude = alias.conf\n");
    assert(symlink("cycle.conf", "alias.conf") == 0);
    assert(axon_config_load("cycle.conf", &config, &error) == AXON_CONFIG_VALUE);
    assert(strstr(error.message, "cycle") != NULL);
    for(int index = 0; index < 34; index++) {
        char name[64];
        snprintf(name, sizeof(name), "depth-%d.conf", index);
        write_file(name, "Include = depth-%d.conf\n", index + 1);
    }
    assert(axon_config_load("depth-0.conf", &config, &error) == AXON_CONFIG_LIMIT);
}

static void malformed(void)
{
    reject("RootDir = /\n", AXON_CONFIG_SYNTAX, 1);
    reject("[options\n", AXON_CONFIG_SYNTAX, 1);
    reject("[options] trailing\n", AXON_CONFIG_SYNTAX, 1);
    reject("[]\n", AXON_CONFIG_SYNTAX, 1);
    reject("[local]\n", AXON_CONFIG_VALUE, 1);
    reject("[../escape]\n", AXON_CONFIG_VALUE, 1);
    reject("[core]\n[core]\n", AXON_CONFIG_VALUE, 2);
    reject("[options]\nSigLevell = Never\n", AXON_CONFIG_UNKNOWN, 2);
    reject("[options]\nSigLevel = Optional # comment\n", AXON_CONFIG_VALUE, 2);
    reject("[options]\nSigLevel =\n", AXON_CONFIG_VALUE, 2);
    reject("[options]\nDBPath = relative\n", AXON_CONFIG_VALUE, 2);
    reject("[options]\nCacheDir = /tmp relative\n", AXON_CONFIG_VALUE, 2);
    reject("[options]\nColor = yes\n", AXON_CONFIG_VALUE, 2);
    reject("[options]\nCleanMethod = Typo\n", AXON_CONFIG_VALUE, 2);
    reject("[options]\nInclude = missing-*\n", AXON_CONFIG_IO, 2);
    reject("[options]\n= value\n", AXON_CONFIG_SYNTAX, 2);
    reject("[core]\nServer\n", AXON_CONFIG_VALUE, 2);
    reject("[core]\nServer = https://example/$arch\n", AXON_CONFIG_VALUE, 2);
    reject("[core]\nServer = https://example/$unknown\n", AXON_CONFIG_VALUE, 2);
    reject("[core]\nServer = nonsense\n", AXON_CONFIG_VALUE, 2);
    reject("[core]\nServer = 1://bad\n", AXON_CONFIG_VALUE, 2);
    reject("[core]\nServer = https:///no-host\n", AXON_CONFIG_VALUE, 2);
    reject("[core]\nUsage = Everything\n", AXON_CONFIG_VALUE, 2);
    reject("[core]\nIgnorePkg = package\n", AXON_CONFIG_UNKNOWN, 2);
    const char *numbers[] = {"0", "-1", "+2", "3x", "4294967296", "99999999999999999999999", ""};
    for(size_t index = 0; index < sizeof(numbers) / sizeof(numbers[0]); index++) {
        char text[256];
        snprintf(text, sizeof(text), "[options]\nParallelDownloads = %s\n", numbers[index]);
        reject(text, AXON_CONFIG_VALUE, 2);
    }

    axon_config_t *config = NULL;
    axon_config_error_t error;
    FILE *file = fopen("nul.conf", "wb");
    assert(file != NULL);
    const char nul[] = "[options]\nSigLevel = Required\0Never\n";
    assert(fwrite(nul, 1, sizeof(nul) - 1, file) == sizeof(nul) - 1);
    assert(fclose(file) == 0);
    assert(axon_config_load("nul.conf", &config, &error) == AXON_CONFIG_SYNTAX);
    assert(error.line == 2);
    file = fopen("long.conf", "w");
    assert(file != NULL);
    for(size_t index = 0; index < 65537; index++) {
        assert(fputc('#', file) != EOF);
    }
    assert(fclose(file) == 0);
    assert(axon_config_load("long.conf", &config, &error) == AXON_CONFIG_LIMIT);
    assert(mkfifo("fifo.conf", 0600) == 0);
    assert(axon_config_load("fifo.conf", &config, &error) == AXON_CONFIG_IO);
    assert(axon_config_load("parts", &config, &error) == AXON_CONFIG_IO);
    assert(axon_config_load("does-not-exist", &config, &error) == AXON_CONFIG_IO);
    assert(error.system_error == ENOENT && config == NULL);
    assert(axon_config_load(NULL, &config, &error) == AXON_CONFIG_ARGUMENT);
    assert(axon_config_load("defaults.conf", NULL, NULL) == AXON_CONFIG_ARGUMENT);

    file = fopen("large.conf", "w");
    assert(file != NULL);
    char block[4096];
    memset(block, '#', sizeof(block));
    block[sizeof(block) - 1] = '\n';
    for(size_t index = 0; index < 4097; index++) {
        assert(fwrite(block, 1, sizeof(block), file) == sizeof(block));
    }
    assert(fclose(file) == 0);
    assert(axon_config_load("large.conf", &config, &error) == AXON_CONFIG_LIMIT);
    write_file("empty.conf", "# empty\n");
    file = fopen("many.conf", "w");
    assert(file != NULL);
    for(size_t index = 0; index < 1024; index++) {
        assert(fputs("Include = empty.conf\n", file) >= 0);
    }
    assert(fclose(file) == 0);
    assert(axon_config_load("many.conf", &config, &error) == AXON_CONFIG_LIMIT);
    file = fopen("values.conf", "w");
    assert(file != NULL && fputs("[options]\n", file) >= 0);
    for(size_t index = 0; index < 65537; index++) {
        assert(fputs("Color\n", file) >= 0);
    }
    assert(fclose(file) == 0);
    assert(axon_config_load("values.conf", &config, &error) == AXON_CONFIG_LIMIT);
}

static void backend(void)
{
    assert(mkdir("root", 0700) == 0);
    assert(mkdir("db", 0700) == 0);
    assert(mkdir("cache", 0700) == 0);
    write_file("backend.conf", "[options]\nRootDir = %s/root\nDBPath = %s/db\n"
               "CacheDir = %s/cache\nGPGDir = %s/keyring\nLogFile = %s/actions.log\n"
               "Architecture = x86_64\nIgnorePkg = held*\nNoExtract = usr/share/doc/*\n"
               "HoldPkg = essential\nCheckSpace\nUseSyslog\nDisableDownloadTimeout\n"
               "DisableSandbox\nParallelDownloads = 3\nSigLevel = Required DatabaseOptional\n"
               "LocalFileSigLevel = Optional\n[fixture]\nServer = file://%s/mirror/$repo/$arch\n"
               "CacheServer = file://%s/cache\nUsage = Sync Search\n",
               workspace, workspace, workspace, workspace, workspace, workspace, workspace);
    axon_config_t *config = load("backend.conf");
    axon_alpm_t *core = NULL;
    axon_config_error_t error;
    if(axon_config_open(config, &core, &error) != AXON_CONFIG_OK) {
        fprintf(stderr, "%s:%zu: %s\n", error.file, error.line, error.message);
        abort();
    }
    const alpm_list_t *repositories = NULL;
    assert(axon_alpm_repositories(core, &repositories) == ALPM_ERR_OK);
    assert(alpm_list_count(repositories) == 1);
    alpm_db_t *database = repositories->data;
    alpm_handle_t *handle = alpm_db_get_handle(database);
    const axon_config_options_t *options = axon_config_options(config);
    assert(alpm_option_get_default_siglevel(handle) == options->siglevel);
    assert(alpm_option_get_local_file_siglevel(handle) == options->local_file_siglevel);
    assert(alpm_option_get_checkspace(handle) == 1);
    assert(alpm_option_get_usesyslog(handle) == 1);
    assert(alpm_option_get_disable_dl_timeout(handle) == 1);
    assert(alpm_option_get_disable_sandbox_filesystem(handle) == 1);
    assert(alpm_option_get_disable_sandbox_syscalls(handle) == 1);
    assert(alpm_option_get_parallel_downloads(handle) == 3);
    assert(strcmp(string_at(alpm_option_get_ignorepkgs(handle), 0), "held*") == 0);
    assert(strcmp(string_at(alpm_option_get_noextracts(handle), 0), "usr/share/doc/*") == 0);
    assert(alpm_list_count(alpm_db_get_cache_servers(database)) == 1);
    int usage = 0;
    assert(alpm_db_get_usage(database, &usage) == 0);
    assert(usage == (ALPM_DB_USAGE_SYNC | ALPM_DB_USAGE_SEARCH));
    assert(axon_config_open(config, &core, &error) == AXON_CONFIG_ARGUMENT);
    axon_config_free(&config);
    assert(strcmp(alpm_db_get_name(database), "fixture") == 0);
    assert(axon_alpm_release(&core) == 0);

    write_file("unsupported.conf", "[options]\nXferCommand = touch /tmp/axon-should-never-execute\n");
    config = load("unsupported.conf");
    assert(axon_config_open(config, &core, &error) == AXON_CONFIG_UNSUPPORTED);
    assert(core == NULL && error.line == 2);
    axon_config_free(&config);
    write_file("bad-root.conf", "[options]\nRootDir = %s/missing-root\n", workspace);
    config = load("bad-root.conf");
    assert(axon_config_open(config, &core, &error) == AXON_CONFIG_ALPM);
    assert(core == NULL && error.alpm_error != ALPM_ERR_OK);
    assert(error.line == 2);
    axon_config_free(&config);
}

static void signature_order(void)
{
    write_file("signature.conf", "[options]\nSigLevel = Optional TrustAll\n"
               "SigLevel = PackageRequired DatabaseTrustedOnly\n"
               "LocalFileSigLevel = PackageNever\nLocalFileSigLevel = PackageOptional PackageTrustedOnly\n"
               "[core]\nSigLevel = Never\nSigLevel = DatabaseRequired DatabaseTrustAll\n");
    axon_config_t *config = load("signature.conf");
    const axon_config_options_t *options = axon_config_options(config);
    assert(options->siglevel == (ALPM_SIG_PACKAGE | ALPM_SIG_PACKAGE_MARGINAL_OK
                                | ALPM_SIG_PACKAGE_UNKNOWN_OK | ALPM_SIG_DATABASE
                                | ALPM_SIG_DATABASE_OPTIONAL));
    assert(options->local_file_siglevel == (ALPM_SIG_PACKAGE | ALPM_SIG_PACKAGE_OPTIONAL
                                           | ALPM_SIG_DATABASE | ALPM_SIG_DATABASE_OPTIONAL));
    const axon_config_repo_t *repo = axon_config_repositories(config)->data;
    assert(repo->siglevel == (ALPM_SIG_DATABASE | ALPM_SIG_DATABASE_MARGINAL_OK
                             | ALPM_SIG_DATABASE_UNKNOWN_OK | ALPM_SIG_PACKAGE_MARGINAL_OK
                             | ALPM_SIG_PACKAGE_UNKNOWN_OK));
    axon_config_free(&config);
}

static char *pacman_query(const char *file, const char *repository, const char *key)
{
    int descriptors[2];
    assert(pipe(descriptors) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if(child == 0) {
        close(descriptors[0]);
        assert(dup2(descriptors[1], STDOUT_FILENO) >= 0);
        close(descriptors[1]);
        if(repository != NULL) {
            execl("/usr/bin/pacman-conf", "pacman-conf", "-c", file, "-r", repository, key, (char *)NULL);
        }

        else {
            execl("/usr/bin/pacman-conf", "pacman-conf", "-c", file, key, (char *)NULL);
        }
        _exit(127);
    }
    close(descriptors[1]);
    char *output = calloc(1, 8192);
    assert(output != NULL);
    size_t used = 0;
    ssize_t count;
    while((count = read(descriptors[0], output + used, 8191 - used)) > 0) {
        used += (size_t)count;
        assert(used < 8191);
    }
    assert(count == 0);
    close(descriptors[0]);
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    return(output);
}

static void compare_list(const char *file, const char *repository, const char *key,
                         const alpm_list_t *values)
{
    char *actual = pacman_query(file, repository, key);
    char *cursor = actual;
    for(const alpm_list_t *item = values; item; item = item->next) {
        const char *value = item->data;
        size_t length = strlen(value);
        assert(strncmp(cursor, value, length) == 0 && cursor[length] == '\n');
        cursor += length + 1;
    }
    assert(*cursor == '\0');
    free(actual);
}

static void differential(void)
{
    if(access("/usr/bin/pacman-conf", X_OK) != 0) {
        puts("pacman-conf unavailable; reference comparison skipped.");
        return;
    }
    axon_config_t *config = load("includes.conf");
    const axon_config_options_t *options = axon_config_options(config);
    compare_list("includes.conf", NULL, "Architecture", options->architectures);
    compare_list("includes.conf", NULL, "IgnorePkg", options->ignore_packages);
    for(const alpm_list_t *item = axon_config_repositories(config); item; item = item->next) {
        const axon_config_repo_t *repo = item->data;
        compare_list("includes.conf", repo->name, "Server", repo->servers);
        compare_list("includes.conf", repo->name, "CacheServer", repo->cache_servers);
    }
    char *signature = pacman_query("includes.conf", "core", "SigLevel");
    assert(strcmp(signature, "PackageRequired\nPackageTrustAll\nDatabaseOptional\nDatabaseTrustAll\n") == 0);
    free(signature);
    axon_config_free(&config);
    write_file("repeat.conf", "[options]\nRootDir = /tmp\nRootDir = /\n"
               "DBPath = /tmp\nDBPath = /\nCacheDir = /tmp/one /tmp/two\n"
               "HookDir = /tmp/hooks /tmp/hooks2\nParallelDownloads = 2\nParallelDownloads = 4\n"
               "XferCommand = first\nXferCommand = second\n");
    config = load("repeat.conf");
    options = axon_config_options(config);
    compare_list("repeat.conf", NULL, "CacheDir", options->cache_dirs);
    compare_list("repeat.conf", NULL, "HookDir", options->hook_dirs);
    assert(strcmp(options->root_dir, "/tmp") == 0 && strcmp(options->db_path, "/tmp") == 0);
    assert(options->parallel_downloads == 4 && strcmp(options->xfer_command, "second") == 0);
    axon_config_free(&config);
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
    assert(previous != NULL && mkdtemp(workspace) != NULL);
    assert(chdir(workspace) == 0);
    defaults();
    includes_and_policies();
    malformed();
    backend();
    signature_order();
    differential();
    assert(chdir(previous) == 0);
    free(previous);
    assert(nftw(workspace, remove_entry, 16, FTW_DEPTH | FTW_PHYS) == 0);
    puts("Configuration parser, backend setup, and compatibility checks passed.");
    return(0);
}
