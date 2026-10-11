#define _POSIX_C_SOURCE 200809L

#include "config_internal.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#define CONFIG_DEPTH 32
#define CONFIG_FILES 1024
#define CONFIG_LINE 65536
#define CONFIG_BYTES (16 * 1024 * 1024)

struct repository {
    axon_config_repo_t value;
    int mask;
};

struct parser {
    axon_config_t *config;
    axon_config_error_t *error;
    struct repository *repository;
    int section;
    const char *file;
    size_t line;
    size_t bytes;
    size_t files;
    size_t depth;
    struct { dev_t device; ino_t inode; } active[CONFIG_DEPTH];
};

static axon_config_result_t fail(struct parser *parser, axon_config_result_t code,
                                 const char *format, ...)
{
    if(parser->error != NULL) {
        va_list args;
        memset(parser->error, 0, sizeof(*parser->error));
        parser->error->code = code;
        parser->error->line = parser->line;
        snprintf(parser->error->file, sizeof(parser->error->file), "%s", parser->file);
        va_start(args, format);
        vsnprintf(parser->error->message, sizeof(parser->error->message), format, args);
        va_end(args);
    }

    return(code);
}

static axon_config_result_t io_error(struct parser *parser, const char *operation)
{
    int saved = errno;
    axon_config_result_t result = fail(parser, AXON_CONFIG_IO, "%s: %s",
                                       operation, strerror(saved));
    if(parser->error != NULL) {
        parser->error->system_error = saved;
    }

    return(result);
}

axon_config_result_t axon_config_fail(const axon_config_t *config,
                                     const void *target, axon_config_error_t *error,
                                     axon_config_result_t code, const char *message)
{
    struct parser parser = {.config = (axon_config_t *)config, .error = error,
                            .file = config != NULL ? config->path : ""};
    if(config != NULL) {
        for(struct axon_config_source *source = config->sources; source; source = source->next) {
            if(source->target == target) {
                parser.file = source->file;
                parser.line = source->line;
                break;
            }
        }
    }

    return(fail(&parser, code, "%s", message));
}

static axon_config_result_t source_add(struct parser *parser, const void *target)
{
    if(parser->config->source_count >= 65536) {
        return(fail(parser, AXON_CONFIG_LIMIT, "too many configuration values"));
    }

    struct axon_config_source *source = calloc(1, sizeof(*source));
    if(source == NULL) {
        return(fail(parser, AXON_CONFIG_MEMORY, "out of memory"));
    }

    source->file = strdup(parser->file);
    if(source->file == NULL) {
        free(source);
        return(fail(parser, AXON_CONFIG_MEMORY, "out of memory"));
    }

    source->target = target;
    source->line = parser->line;
    source->next = parser->config->sources;
    parser->config->sources = source;
    parser->config->source_count++;
    return(AXON_CONFIG_OK);
}

static char *trim(char *text)
{
    while(isspace((unsigned char)*text)) {
        text++;
    }

    size_t length = strlen(text);
    while(length > 0 && isspace((unsigned char)text[length - 1])) {
        text[--length] = '\0';
    }

    return(text);
}

static axon_config_result_t string_set(struct parser *parser, char **target,
                                       const char *value, int path)
{
    if(value[0] == '\0' || (path && value[0] != '/')) {
        return(fail(parser, AXON_CONFIG_VALUE, "expected %s", path ? "an absolute path" : "a value"));
    }

    if(*target != NULL) {
        return(AXON_CONFIG_OK);
    }

    *target = strdup(value);
    if(*target == NULL) {
        return(fail(parser, AXON_CONFIG_MEMORY, "out of memory"));
    }

    return(source_add(parser, target));
}

static axon_config_result_t list_add(struct parser *parser, alpm_list_t **list,
                                     const char *value)
{
    alpm_list_t *node = alpm_list_append_strdup(list, value);
    if(node == NULL) {
        return(fail(parser, AXON_CONFIG_MEMORY, "out of memory"));
    }

    return(source_add(parser, node));
}

static axon_config_result_t words_add(struct parser *parser, alpm_list_t **list,
                                      char *value, int architecture)
{
    char *save = NULL;
    for(char *word = strtok_r(value, " \t\r\n", &save); word; word = strtok_r(NULL, " \t\r\n", &save)) {
        struct utsname machine;
        if(architecture && strcmp(word, "auto") == 0) {
            if(uname(&machine) < 0) {
                return(io_error(parser, "uname"));
            }
            word = machine.machine;
        }

        axon_config_result_t result = list_add(parser, list, word);
        if(result != AXON_CONFIG_OK) {
            return(result);
        }
    }

    return(AXON_CONFIG_OK);
}

static axon_config_result_t signature(struct parser *parser, char *value,
                                      int *level, int *mask)
{
    char *save = NULL;
    if(value[0] == '\0') {
        return(fail(parser, AXON_CONFIG_VALUE, "empty signature policy"));
    }

    for(char *word = strtok_r(value, " \t", &save); word; word = strtok_r(NULL, " \t", &save)) {
        int package = 1;
        int database = 1;
        if(strncmp(word, "Package", 7) == 0) {
            database = 0;
            word += 7;
        }

        else if(strncmp(word, "Database", 8) == 0) {
            package = 0;
            word += 8;
        }

        int required = (package ? ALPM_SIG_PACKAGE : 0) | (database ? ALPM_SIG_DATABASE : 0);
        int optional = (package ? ALPM_SIG_PACKAGE_OPTIONAL : 0)
                       | (database ? ALPM_SIG_DATABASE_OPTIONAL : 0);
        int trust = (package ? ALPM_SIG_PACKAGE_MARGINAL_OK | ALPM_SIG_PACKAGE_UNKNOWN_OK : 0)
                    | (database ? ALPM_SIG_DATABASE_MARGINAL_OK | ALPM_SIG_DATABASE_UNKNOWN_OK : 0);
        int changed;
        int bits;
        if(strcmp(word, "Never") == 0) {
            changed = required | optional;
            bits = 0;
        }

        else if(strcmp(word, "Optional") == 0) {
            changed = required | optional;
            bits = required | optional;
        }

        else if(strcmp(word, "Required") == 0) {
            changed = required | optional;
            bits = required;
        }

        else if(strcmp(word, "TrustedOnly") == 0 || strcmp(word, "TrustAll") == 0) {
            changed = trust;
            bits = strcmp(word, "TrustAll") == 0 ? trust : 0;
        }

        else {
            return(fail(parser, AXON_CONFIG_VALUE, "unknown signature policy token '%s'", word));
        }

        *level = (*level & ~changed) | bits;
        *mask |= changed;
    }

    return(source_add(parser, level));
}

static axon_config_result_t section(struct parser *parser, char *line)
{
    size_t length = strlen(line);
    if(length < 3 || line[length - 1] != ']') {
        return(fail(parser, AXON_CONFIG_SYNTAX, "malformed section header"));
    }

    line[length - 1] = '\0';
    char *name = line + 1;
    if(strpbrk(name, "[]/ \t\r\n") != NULL || strcmp(name, ".") == 0
            || strcmp(name, "..") == 0 || strcmp(name, "local") == 0) {
        return(fail(parser, AXON_CONFIG_VALUE, "invalid or reserved section name '%s'", name));
    }

    parser->section = 1;
    if(strcmp(name, "options") == 0) {
        parser->repository = NULL;
        return(AXON_CONFIG_OK);
    }

    for(alpm_list_t *item = parser->config->repositories; item; item = item->next) {
        struct repository *repo = item->data;
        if(strcmp(repo->value.name, name) == 0) {
            return(fail(parser, AXON_CONFIG_VALUE, "duplicate repository '%s'", name));
        }
    }

    struct repository *repo = calloc(1, sizeof(*repo));
    if(repo == NULL) {
        return(fail(parser, AXON_CONFIG_MEMORY, "out of memory"));
    }

    repo->value.name = strdup(name);
    if(repo->value.name == NULL || alpm_list_append(&parser->config->repositories, repo) == NULL) {
        free(repo->value.name);
        free(repo);
        return(fail(parser, AXON_CONFIG_MEMORY, "out of memory"));
    }

    parser->repository = repo;
    return(source_add(parser, &repo->value));
}

static axon_config_result_t parse_file(struct parser *parser, const char *path);

static axon_config_result_t include(struct parser *parser, const char *pattern)
{
    glob_t matches = {0};
    int result = glob(pattern, GLOB_ERR, NULL, &matches);
    if(result != 0) {
        globfree(&matches);
        return(fail(parser, result == GLOB_NOSPACE ? AXON_CONFIG_MEMORY : AXON_CONFIG_IO,
                    "cannot expand Include '%s'%s", pattern,
                    result == GLOB_NOMATCH ? ": no files matched" : ""));
    }

    if(matches.gl_pathc > CONFIG_FILES - parser->files) {
        globfree(&matches);
        return(fail(parser, AXON_CONFIG_LIMIT, "too many included files"));
    }

    axon_config_result_t status = AXON_CONFIG_OK;
    for(size_t index = 0; index < matches.gl_pathc; index++) {
        status = parse_file(parser, matches.gl_pathv[index]);
        if(status != AXON_CONFIG_OK) {
            break;
        }
    }

    globfree(&matches);
    return(status);
}

static axon_config_result_t directive(struct parser *parser, char *key, char *value)
{
    axon_config_options_t *options = &parser->config->options;
    if(strcmp(key, "Include") == 0) {
        if(value == NULL || value[0] == '\0') {
            return(fail(parser, AXON_CONFIG_VALUE, "Include needs a path"));
        }
        return(include(parser, value));
    }

    if(!parser->section) {
        return(fail(parser, AXON_CONFIG_SYNTAX, "directive before any section"));
    }

    if(parser->repository != NULL) {
        struct repository *repo = parser->repository;
        if(value == NULL || value[0] == '\0') {
            return(fail(parser, AXON_CONFIG_VALUE, "repository directive '%s' needs a value", key));
        }

        if(strcmp(key, "Server") == 0 || strcmp(key, "CacheServer") == 0) {
            return(list_add(parser, strcmp(key, "Server") == 0
                            ? &repo->value.servers : &repo->value.cache_servers, value));
        }

        if(strcmp(key, "SigLevel") == 0) {
            return(signature(parser, value, &repo->value.siglevel, &repo->mask));
        }

        if(strcmp(key, "Usage") == 0) {
            char *save = NULL;
            for(char *word = strtok_r(value, " \t", &save); word; word = strtok_r(NULL, " \t", &save)) {
                if(strcmp(word, "All") == 0) {
                    repo->value.usage |= ALPM_DB_USAGE_ALL;
                }

                else if(strcmp(word, "Sync") == 0) {
                    repo->value.usage |= ALPM_DB_USAGE_SYNC;
                }

                else if(strcmp(word, "Search") == 0) {
                    repo->value.usage |= ALPM_DB_USAGE_SEARCH;
                }

                else if(strcmp(word, "Install") == 0) {
                    repo->value.usage |= ALPM_DB_USAGE_INSTALL;
                }

                else if(strcmp(word, "Upgrade") == 0) {
                    repo->value.usage |= ALPM_DB_USAGE_UPGRADE;
                }

                else {
                    return(fail(parser, AXON_CONFIG_VALUE, "unknown Usage token '%s'", word));
                }
            }
            return(source_add(parser, &repo->value.usage));
        }

        return(fail(parser, AXON_CONFIG_UNKNOWN, "unknown repository directive '%s'", key));
    }

    struct { const char *key; int *target; } flags[] = {
        {"UseSyslog", &options->use_syslog}, {"Color", &options->color},
        {"NoProgressBar", &options->no_progress_bar}, {"CheckSpace", &options->check_space},
        {"VerbosePkgLists", &options->verbose_package_lists}, {"ILoveCandy", &options->candy},
        {"DisableDownloadTimeout", &options->disable_download_timeout},
        {"DisableSandbox", &options->disable_sandbox},
        {"DisableSandboxFilesystem", &options->disable_sandbox_filesystem},
        {"DisableSandboxSyscalls", &options->disable_sandbox_syscalls}
    };
    for(size_t index = 0; index < sizeof(flags) / sizeof(flags[0]); index++) {
        if(strcmp(key, flags[index].key) == 0) {
            if(value != NULL) {
                return(fail(parser, AXON_CONFIG_VALUE, "'%s' does not take a value", key));
            }
            *flags[index].target = 1;
            return(source_add(parser, flags[index].target));
        }
    }

    if(value == NULL) {
        return(fail(parser, AXON_CONFIG_VALUE, "directive '%s' requires '=' and a value", key));
    }

    struct { const char *key; char **target; int path; } strings[] = {
        {"RootDir", &options->root_dir, 1}, {"DBPath", &options->db_path, 1},
        {"LogFile", &options->log_file, 1}, {"GPGDir", &options->gpg_dir, 1},
        {"DownloadUser", &options->download_user, 0}, {"XferCommand", &options->xfer_command, 0}
    };
    for(size_t index = 0; index < sizeof(strings) / sizeof(strings[0]); index++) {
        if(strcmp(key, strings[index].key) == 0) {
            if(strcmp(key, "XferCommand") == 0) {
                free(*strings[index].target);
                *strings[index].target = NULL;
            }
            return(string_set(parser, strings[index].target, value, strings[index].path));
        }
    }

    if(strcmp(key, "CacheDir") == 0 || strcmp(key, "HookDir") == 0) {
        char *save = NULL;
        if(value[0] == '\0') {
            return(fail(parser, AXON_CONFIG_VALUE, "'%s' requires an absolute path", key));
        }
        for(char *word = strtok_r(value, " \t", &save); word; word = strtok_r(NULL, " \t", &save)) {
            if(word[0] != '/') {
                return(fail(parser, AXON_CONFIG_VALUE, "'%s' requires absolute paths", key));
            }
            axon_config_result_t result = list_add(parser, strcmp(key, "CacheDir") == 0
                                                   ? &options->cache_dirs : &options->hook_dirs, word);
            if(result != AXON_CONFIG_OK) {
                return(result);
            }
        }
        return(AXON_CONFIG_OK);
    }

    struct { const char *key; alpm_list_t **target; } lists[] = {
        {"Architecture", &options->architectures}, {"HoldPkg", &options->hold_packages},
        {"IgnorePkg", &options->ignore_packages}, {"IgnoreGroup", &options->ignore_groups},
        {"NoUpgrade", &options->no_upgrade}, {"NoExtract", &options->no_extract}
    };
    for(size_t index = 0; index < sizeof(lists) / sizeof(lists[0]); index++) {
        if(strcmp(key, lists[index].key) == 0) {
            return(words_add(parser, lists[index].target, value, index == 0));
        }
    }

    if(strcmp(key, "SigLevel") == 0) {
        int mask = 0;
        return(signature(parser, value, &options->siglevel, &mask));
    }

    if(strcmp(key, "LocalFileSigLevel") == 0) {
        return(signature(parser, value, &options->local_file_siglevel, &parser->config->local_mask));
    }

    if(strcmp(key, "RemoteFileSigLevel") == 0) {
        return(signature(parser, value, &options->remote_file_siglevel, &parser->config->remote_mask));
    }

    if(strcmp(key, "ParallelDownloads") == 0) {
        unsigned int number = 0;
        if(value[0] == '\0') {
            return(fail(parser, AXON_CONFIG_VALUE, "ParallelDownloads needs a positive integer"));
        }
        for(const char *digit = value; *digit; digit++) {
            if(*digit < '0' || *digit > '9' || number > (UINT_MAX - (unsigned int)(*digit - '0')) / 10) {
                return(fail(parser, AXON_CONFIG_VALUE, "invalid ParallelDownloads value"));
            }
            number = number * 10 + (unsigned int)(*digit - '0');
        }
        if(number == 0) {
            return(fail(parser, AXON_CONFIG_VALUE, "ParallelDownloads must be positive"));
        }
        options->parallel_downloads = number;
        return(source_add(parser, &options->parallel_downloads));
    }

    if(strcmp(key, "CleanMethod") == 0) {
        char *save = NULL;
        if(value[0] == '\0') {
            return(fail(parser, AXON_CONFIG_VALUE, "empty CleanMethod"));
        }
        for(char *word = strtok_r(value, " \t", &save); word; word = strtok_r(NULL, " \t", &save)) {
            if(strcmp(word, "KeepInstalled") == 0) {
                options->clean_keep_installed = 1;
            }

            else if(strcmp(word, "KeepCurrent") == 0) {
                options->clean_keep_current = 1;
            }

            else {
                return(fail(parser, AXON_CONFIG_VALUE, "unknown CleanMethod token '%s'", word));
            }
        }
        return(AXON_CONFIG_OK);
    }

    return(fail(parser, AXON_CONFIG_UNKNOWN, "unknown options directive '%s'", key));
}

static axon_config_result_t parse_line(struct parser *parser, char *buffer)
{
    char *line = trim(buffer);
    if(line[0] == '\0' || line[0] == '#') {
        return(AXON_CONFIG_OK);
    }

    if(line[0] == '[') {
        return(section(parser, line));
    }

    char *value = strchr(line, '=');
    if(value != NULL) {
        *value++ = '\0';
        value = trim(value);
        line = trim(line);
    }

    if(line[0] == '\0') {
        return(fail(parser, AXON_CONFIG_SYNTAX, "empty directive name"));
    }

    return(directive(parser, line, value));
}

static axon_config_result_t parse_file(struct parser *parser, const char *path)
{
    if(parser->depth >= CONFIG_DEPTH || parser->files >= CONFIG_FILES) {
        return(fail(parser, AXON_CONFIG_LIMIT, "Include depth or file count limit exceeded"));
    }

    const char *parent_file = parser->file;
    size_t parent_line = parser->line;
    parser->file = path;
    parser->line = 0;
    axon_config_result_t result = AXON_CONFIG_OK;
    int descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    FILE *file = NULL;
    char *buffer = NULL;
    struct stat info;
    if(descriptor < 0) {
        result = io_error(parser, "cannot open configuration");
        goto done;
    }

    if(fstat(descriptor, &info) < 0) {
        result = io_error(parser, "cannot stat configuration");
        goto done;
    }

    if(!S_ISREG(info.st_mode)) {
        result = fail(parser, AXON_CONFIG_IO, "configuration must be a regular file");
        goto done;
    }

    for(size_t index = 0; index < parser->depth; index++) {
        if(parser->active[index].device == info.st_dev && parser->active[index].inode == info.st_ino) {
            result = fail(parser, AXON_CONFIG_VALUE, "Include cycle detected");
            goto done;
        }
    }

    file = fdopen(descriptor, "r");
    if(file == NULL) {
        result = io_error(parser, "cannot read configuration");
        goto done;
    }
    descriptor = -1;
    buffer = malloc(CONFIG_LINE + 1);
    if(buffer == NULL) {
        result = fail(parser, AXON_CONFIG_MEMORY, "out of memory");
        goto done;
    }

    parser->active[parser->depth].device = info.st_dev;
    parser->active[parser->depth].inode = info.st_ino;
    parser->depth++;
    parser->files++;
    for(;;) {
        size_t used = 0;
        int character;
        parser->line++;
        while((character = fgetc(file)) != EOF && character != '\n') {
            if(++parser->bytes > CONFIG_BYTES || used >= CONFIG_LINE) {
                result = fail(parser, AXON_CONFIG_LIMIT, "configuration byte or line limit exceeded");
                break;
            }
            if(character == '\0') {
                result = fail(parser, AXON_CONFIG_SYNTAX, "NUL byte in configuration");
                break;
            }
            buffer[used++] = (char)character;
        }

        if(result != AXON_CONFIG_OK) {
            break;
        }
        if(character == '\n' && ++parser->bytes > CONFIG_BYTES) {
            result = fail(parser, AXON_CONFIG_LIMIT, "configuration byte limit exceeded");
            break;
        }
        if(ferror(file)) {
            result = io_error(parser, "error reading configuration");
            break;
        }
        if(character == EOF && used == 0) {
            break;
        }
        buffer[used] = '\0';
        result = parse_line(parser, buffer);
        if(result != AXON_CONFIG_OK || character == EOF) {
            break;
        }
    }
    parser->depth--;

done:
    free(buffer);
    if(file != NULL) {
        if(fclose(file) != 0 && result == AXON_CONFIG_OK) {
            result = io_error(parser, "cannot close configuration");
        }
    }
    if(descriptor >= 0) {
        close(descriptor);
    }
    parser->file = parent_file;
    parser->line = parent_line;
    return(result);
}

static char *root_path(const char *root, const char *suffix)
{
    size_t length = strlen(root);
    while(length > 0 && root[length - 1] == '/') {
        length--;
    }
    char *result = malloc(length + strlen(suffix) + 1);
    if(result != NULL) {
        memcpy(result, root, length);
        strcpy(result + length, suffix);
    }
    return(result);
}

static axon_config_result_t expand_server(struct parser *parser, alpm_list_t *node,
                                          const char *repository)
{
    const char *value = node->data;
    alpm_list_t *architectures = parser->config->options.architectures;
    const char *arch = architectures != NULL ? architectures->data : NULL;
    size_t size = 1;
    for(const char *cursor = value; *cursor; cursor++) {
        if(strncmp(cursor, "$repo", 5) == 0) {
            size += strlen(repository);
            cursor += 4;
        }

        else if(strncmp(cursor, "$arch", 5) == 0) {
            if(arch == NULL) {
                return(axon_config_fail(parser->config, node, parser->error,
                                        AXON_CONFIG_VALUE, "$arch used without Architecture"));
            }
            size += strlen(arch);
            cursor += 4;
        }

        else {
            size++;
        }
        if(size > CONFIG_LINE) {
            return(axon_config_fail(parser->config, node, parser->error,
                                    AXON_CONFIG_LIMIT, "expanded Server is too long"));
        }
    }

    char *expanded = malloc(size);
    if(expanded == NULL) {
        return(fail(parser, AXON_CONFIG_MEMORY, "out of memory"));
    }
    char *output = expanded;
    for(const char *cursor = value; *cursor; cursor++) {
        const char *replacement = NULL;
        if(strncmp(cursor, "$repo", 5) == 0) {
            replacement = repository;
        }

        else if(strncmp(cursor, "$arch", 5) == 0) {
            replacement = arch;
        }
        if(replacement != NULL) {
            size_t length = strlen(replacement);
            memcpy(output, replacement, length);
            output += length;
            cursor += 4;
        }

        else {
            *output++ = *cursor;
        }
    }
    *output = '\0';
    char *scheme = strstr(expanded, "://");
    int valid_scheme = isalpha((unsigned char)expanded[0]);
    if(scheme != NULL) {
        for(char *character = expanded; character < scheme; character++) {
            if(!isalnum((unsigned char)*character) && strchr("+.-", *character) == NULL) {
                valid_scheme = 0;
            }
        }
    }
    if(scheme == NULL || !valid_scheme || scheme[3] == '\0'
            || (strncmp(expanded, "file://", 7) != 0 && scheme[3] == '/')
            || strpbrk(expanded, " \t\r\n$") != NULL) {
        free(expanded);
        return(axon_config_fail(parser->config, node, parser->error,
                                AXON_CONFIG_VALUE, "invalid Server URL or unresolved variable"));
    }
    free(node->data);
    node->data = expanded;
    return(AXON_CONFIG_OK);
}

static axon_config_result_t finish(struct parser *parser)
{
    axon_config_options_t *options = &parser->config->options;
    if(options->root_dir == NULL) {
        options->root_dir = strdup("/");
    }
    if(options->root_dir == NULL) {
        return(fail(parser, AXON_CONFIG_MEMORY, "out of memory"));
    }
    if(options->db_path == NULL) {
        options->db_path = root_path(options->root_dir, "/var/lib/pacman/");
    }
    if(options->log_file == NULL) {
        options->log_file = root_path(options->root_dir, "/var/log/pacman.log");
    }
    if(options->gpg_dir == NULL) {
        options->gpg_dir = strdup("/etc/pacman.d/gnupg/");
    }
    if(options->db_path == NULL || options->log_file == NULL || options->gpg_dir == NULL) {
        return(fail(parser, AXON_CONFIG_MEMORY, "out of memory"));
    }
    if(options->cache_dirs == NULL && alpm_list_append_strdup(&options->cache_dirs, "/var/cache/pacman/pkg/") == NULL) {
        return(fail(parser, AXON_CONFIG_MEMORY, "out of memory"));
    }
    if(options->hook_dirs == NULL && alpm_list_append_strdup(&options->hook_dirs, "/etc/pacman.d/hooks/") == NULL) {
        return(fail(parser, AXON_CONFIG_MEMORY, "out of memory"));
    }
    if(!options->clean_keep_installed && !options->clean_keep_current) {
        options->clean_keep_installed = 1;
    }
    options->local_file_siglevel |= options->siglevel & ~parser->config->local_mask;
    options->remote_file_siglevel |= options->siglevel & ~parser->config->remote_mask;
    for(alpm_list_t *item = parser->config->repositories; item; item = item->next) {
        struct repository *repo = item->data;
        repo->value.siglevel |= options->siglevel & ~repo->mask;
        if(repo->value.usage == 0) {
            repo->value.usage = ALPM_DB_USAGE_ALL;
        }
        alpm_list_t *lists[] = {repo->value.servers, repo->value.cache_servers};
        for(size_t index = 0; index < 2; index++) {
            for(alpm_list_t *node = lists[index]; node; node = node->next) {
                axon_config_result_t result = expand_server(parser, node, repo->value.name);
                if(result != AXON_CONFIG_OK) {
                    return(result);
                }
            }
        }
    }
    return(AXON_CONFIG_OK);
}

axon_config_result_t axon_config_load(const char *path, axon_config_t **config,
                                     axon_config_error_t *error)
{
    if(error != NULL) {
        memset(error, 0, sizeof(*error));
    }
    if(config == NULL || *config != NULL || path == NULL || path[0] == '\0') {
        return(axon_config_fail(NULL, NULL, error, AXON_CONFIG_ARGUMENT,
                                "pass a configuration path and an initially NULL output"));
    }
    axon_config_t *parsed = calloc(1, sizeof(*parsed));
    if(parsed == NULL) {
        return(axon_config_fail(NULL, NULL, error, AXON_CONFIG_MEMORY, "out of memory"));
    }
    parsed->path = strdup(path);
    if(parsed->path == NULL) {
        axon_config_free(&parsed);
        return(axon_config_fail(NULL, NULL, error, AXON_CONFIG_MEMORY, "out of memory"));
    }
    parsed->options.parallel_downloads = 1;
    parsed->options.siglevel = ALPM_SIG_PACKAGE | ALPM_SIG_DATABASE;
    struct parser parser = {.config = parsed, .error = error, .file = path};
    axon_config_result_t result = parse_file(&parser, path);
    if(result == AXON_CONFIG_OK) {
        result = finish(&parser);
    }
    if(result != AXON_CONFIG_OK) {
        axon_config_free(&parsed);
        return(result);
    }
    *config = parsed;
    return(AXON_CONFIG_OK);
}

void axon_config_free(axon_config_t **config)
{
    if(config == NULL || *config == NULL) {
        return;
    }
    axon_config_t *current = *config;
    axon_config_options_t *options = &current->options;
    free(options->root_dir);
    free(options->db_path);
    free(options->log_file);
    free(options->gpg_dir);
    free(options->download_user);
    free(options->xfer_command);
    alpm_list_t *lists[] = {options->cache_dirs, options->hook_dirs, options->architectures,
                           options->hold_packages, options->ignore_packages, options->ignore_groups,
                           options->no_upgrade, options->no_extract};
    for(size_t index = 0; index < sizeof(lists) / sizeof(lists[0]); index++) {
        alpm_list_free_inner(lists[index], free);
        alpm_list_free(lists[index]);
    }
    for(alpm_list_t *item = current->repositories; item; item = item->next) {
        struct repository *repo = item->data;
        free(repo->value.name);
        alpm_list_free_inner(repo->value.servers, free);
        alpm_list_free(repo->value.servers);
        alpm_list_free_inner(repo->value.cache_servers, free);
        alpm_list_free(repo->value.cache_servers);
        free(repo);
    }
    alpm_list_free(current->repositories);
    while(current->sources != NULL) {
        struct axon_config_source *source = current->sources;
        current->sources = source->next;
        free(source->file);
        free(source);
    }
    free(current->path);
    free(current);
    *config = NULL;
}

const axon_config_options_t *axon_config_options(const axon_config_t *config)
{
    return(config != NULL ? &config->options : NULL);
}

const alpm_list_t *axon_config_repositories(const axon_config_t *config)
{
    return(config != NULL ? config->repositories : NULL);
}
