#define _XOPEN_SOURCE 700
#include "../src/ui/terminal.h"
#include "privilege.h"

#include <assert.h>
#include <errno.h>
#include <ftw.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static pid_t child;
static int terminal;
static int authorized = 1;
static char output[131072];

axon_privilege_result_t __wrap_axon_privilege_check(int require_root)
{
    return(require_root && !authorized ? AXON_PRIVILEGE_ROOT_REQUIRED : AXON_PRIVILEGE_OK);
}

static void timeout(int number)
{
    (void)number;
    if(child > 0) {
        kill(child, SIGKILL);
    }
    _exit(124);
}

static void write_file(const char *path, const char *text)
{
    FILE *file = fopen(path, "w");
    assert(file != NULL && fputs(text, file) >= 0 && fclose(file) == 0);
}

static void expect(const char *text)
{
    static int step;
    step++;
    size_t used = 0;
    output[0] = '\0';
    for(;;) {
        struct pollfd poller = {.fd = terminal, .events = POLLIN};
        int ready = poll(&poller, 1, 5000);
        if(ready <= 0) {
            fprintf(stderr, "UI step %d timed out waiting for '%s':\n%s\n", step, text, output);
            kill(child, SIGKILL);
            abort();
        }
        ssize_t size = read(terminal, output + used, sizeof(output) - used - 1);
        if(size <= 0) {
            fprintf(stderr, "UI exited waiting for '%s':\n%s\n", text, output);
            abort();
        }
        used += (size_t)size;
        output[used] = '\0';
        if(strstr(output, text) != NULL) {
            return;
        }
        assert(used < sizeof(output) - 1);
    }
}

static void send_keys(const char *keys)
{
    assert(write(terminal, keys, strlen(keys)) == (ssize_t)strlen(keys));
}

static void start(axon_config_t *config, unsigned short height, unsigned short width)
{
    struct winsize window = {.ws_row = height, .ws_col = width};
    fflush(NULL);
    child = forkpty(&terminal, NULL, NULL, &window);
    assert(child >= 0);
    if(child == 0) {
        assert(setenv("TERM", "xterm", 1) == 0);
        int status = axon_terminal(config);
        fflush(NULL);
        _exit(status);
    }
    alarm(30);
}

static void finish(int expected)
{
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == expected);
    close(terminal);
    child = 0;
    alarm(0);
    assert(access("db/db.lck", F_OK) != 0);
}

static void maintenance_action(axon_config_t *config, const char *key,
                               const char *answer, const char *message, int expected)
{
    start(config, 24, 100);
    expect("fixture");
    send_keys(key);
    if(answer != NULL) {
        expect("Proceed?");
        send_keys(answer);
    }
    expect("Press Enter to return");
    assert(strstr(output, message));
    send_keys("\n");
    expect("fixture");
    send_keys("o");
    expect("Press Enter to return");
    send_keys("\n");
    expect("fixture");
    send_keys("q");
    finish(expected);
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
    struct sigaction action = {.sa_handler = timeout};
    sigemptyset(&action.sa_mask);
    assert(sigaction(SIGALRM, &action, NULL) == 0);
    char *previous = getcwd(NULL, 0);
    char workspace[] = "/tmp/axon-ui-XXXXXX";
    assert(previous && mkdtemp(workspace) && chdir(workspace) == 0);
    assert(mkdir("db", 0700) == 0 && mkdir("db/local", 0700) == 0);
    assert(mkdir("root", 0700) == 0 && mkdir("cache", 0700) == 0);
    assert(mkdir("db/local/fixture-1-1", 0700) == 0);
    write_file("db/local/ALPM_DB_VERSION", "9\n");
    write_file("db/local/fixture-1-1/desc", "%NAME%\nfixture\n\n%VERSION%\n1-1\n\n%REASON%\n1\n\n%DESC%\nUI regression package\n\n");
    char text[2048];
    snprintf(text, sizeof(text), "[options]\nRootDir = %s/root\nDBPath = %s/db\nCacheDir = %s/cache\n"
             "LogFile = %s/log\nGPGDir = %s/keys\nSigLevel = Never\n", workspace, workspace, workspace, workspace, workspace);
    write_file("pacman.conf", text);
    axon_config_t *config = NULL;
    axon_config_error_t error;
    assert(axon_config_load("pacman.conf", &config, &error) == AXON_CONFIG_OK);
    start(config, 24, 100);
    expect("fixture");
    send_keys("/");
    expect("Filter:");
    send_keys("absent\n");
    expect("Enter details");
    send_keys("/");
    expect("Filter:");
    send_keys("\n");
    expect("fixture");
    send_keys("\n");
    expect("Press Enter to return");
    assert(strstr(output, "UI regression package"));
    assert(strstr(output, "Depends on") && strstr(output, "Conflicts with"));
    assert(strstr(output, "Installed as a dependency"));
    send_keys("\n");
    expect("fixture");
    send_keys("r");
    expect("Commit this transaction?");
    send_keys("n\n");
    expect("Press Enter to return");
    assert(strstr(output, "cancelled before commit"));
    send_keys("\n");
    expect("fixture");
    send_keys("e");
    expect("Proceed?");
    send_keys("y\n");
    expect("Press Enter to return");
    assert(strstr(output, "Installation reason changed"));
    send_keys("\n");
    expect("fixture");
    send_keys("q");
    finish(1);
    axon_alpm_t *core = NULL;
    alpm_pkg_t *package = NULL;
    assert(axon_config_open(config, &core, &error) == AXON_CONFIG_OK);
    assert(axon_alpm_local_package(core, "fixture", &package) == ALPM_ERR_OK);
    assert(alpm_pkg_get_reason(package) == ALPM_PKG_REASON_EXPLICIT);
    assert(axon_alpm_release(&core) == 0);

    authorized = 0;
    maintenance_action(config, "e", NULL, "requires root", 1);
    maintenance_action(config, "b", NULL, "requires root", 1);
    maintenance_action(config, "c", NULL, "requires root", 1);
    maintenance_action(config, "d", NULL, "malformed package record", 1);
    write_file("db/local/fixture-1-1/files", "%FILES%\n\n");
    maintenance_action(config, "d", NULL, "no repair performed", 0);
    authorized = 1;
    maintenance_action(config, "e", "n\n", "cancelled", 1);
    maintenance_action(config, "b", "n\n", "cancelled", 1);
    maintenance_action(config, "b", "y\n", "Installation reason changed", 0);
    maintenance_action(config, "e", "y\n", "Installation reason changed", 0);
    maintenance_action(config, "c", NULL, "No cache files eligible", 0);
    authorized = 0;
    start(config, 24, 100);
    expect("fixture");
    send_keys("r");
    expect("Press Enter to return");
    assert(strstr(output, "root"));
    send_keys("\n");
    expect("fixture");
    send_keys("q");
    finish(1);
    start(config, 5, 30);
    expect("Resize terminal");
    struct winsize size = {.ws_row = 24, .ws_col = 100};
    assert(ioctl(terminal, TIOCSWINSZ, &size) == 0);
    expect("fixture");
    send_keys("q");
    finish(0);
    write_file("db/local/fixture-1-1/desc", "%NAME%\nfixture\n\n%VERSION%\n1-1\n\n%DESC%\n"
               "界é description \033]52;c;injected\a\n\n");
    assert(setenv("NO_COLOR", "1", 1) == 0);
    start(config, 24, 100);
    expect("fixture");
    send_keys("\n");
    expect("Press Enter to return");
    assert(strstr(output, "\\x1b]52;c;injected\\x07"));
    assert(!strstr(output, "\033]52;") && !strstr(output, "38;2;") && !strstr(output, "[34m"));
    send_keys("\n");
    expect("fixture");
    send_keys("q");
    finish(0);
    assert(unsetenv("NO_COLOR") == 0);
    assert(nftw("db/local/fixture-1-1", remove_entry, 16, FTW_DEPTH | FTW_PHYS) == 0);
    start(config, 24, 100);
    expect("0 packages");
    send_keys("jk \t");
    expect("Repositories");
    send_keys("q");
    finish(0);
    axon_config_free(&config);
    assert(chdir(previous) == 0);
    free(previous);
    assert(nftw(workspace, remove_entry, 16, FTW_DEPTH | FTW_PHYS) == 0);
    puts("Terminal UI browsing, filtering, confirmation, privilege, resize, and empty-list regressions passed.");
    return(0);
}
