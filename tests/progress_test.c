#define _POSIX_C_SOURCE 200809L
#include "../src/cli/mutation.c"

#include <assert.h>
#include <locale.h>
#include <pty.h>
#include <sys/wait.h>

static char output[8192];

static void capture(int monochrome, int redirected)
{
    int reader, writer, descriptors[2];
    struct winsize window = {.ws_row = 24, .ws_col = 80};
    if(redirected) {
        assert(pipe(descriptors) == 0);
        reader = descriptors[0];
        writer = descriptors[1];
    }
    else {
        assert(openpty(&reader, &writer, NULL, NULL, &window) == 0);
    }
    pid_t child = fork();
    assert(child >= 0);
    if(child == 0) {
        close(reader);
        assert(dup2(writer, STDERR_FILENO) >= 0);
        close(writer);
        assert(setenv("TERM", "xterm", 1) == 0);
        if(monochrome) { assert(setenv("NO_COLOR", "1", 1) == 0); }
        else { assert(unsetenv("NO_COLOR") == 0); }
        axon_config_options_t options = {0};
        alpm_download_event_progress_t transfer = {.total = 0, .downloaded = 100};
        download(&options, "界é\033]52;c;bad\a", ALPM_DOWNLOAD_PROGRESS, &transfer);
        assert(!interrupted);
        transfer.total = 200;
        download(&options, "second", ALPM_DOWNLOAD_PROGRESS, &transfer);
        transfer.downloaded = 200;
        download(&options, "first", ALPM_DOWNLOAD_PROGRESS, &transfer);
        download(&options, "second", ALPM_DOWNLOAD_COMPLETED, NULL);
        progress(&options, ALPM_PROGRESS_ADD_START, "package", 50, 2, 1);
        finish_progress();
        assert(fflush(stderr) == 0);
        _exit(0);
    }
    close(writer);
    size_t used = 0;
    ssize_t count;
    while((count = read(reader, output + used, sizeof(output) - used - 1)) > 0) {
        used += (size_t)count;
        assert(used < sizeof(output) - 1);
    }
    assert(count == 0 || errno == EIO);
    output[used] = '\0';
    close(reader);
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

int main(void)
{
    assert(setlocale(LC_CTYPE, "C.UTF-8"));
    capture(0, 0);
    assert(strstr(output, "\\x1b]52;c;bad\\x07") && !strstr(output, "\033]52;"));
    assert(strstr(output, AXON_BLUE) && strstr(output, AXON_PURPLE) && strstr(output, AXON_GREEN));
    const char *second = strstr(output, "Downloading second");
    const char *bar = strstr(output, "------------------------");
    assert(second && bar && bar < second);
    assert(strstr(second, "============------------") && strstr(output, "[1/2] package"));
    capture(1, 0);
    assert(!strstr(output, "38;2;") && strstr(output, "Downloading second"));
    capture(0, 1);
    assert(!*output);
    int saved = dup(STDERR_FILENO);
    assert(saved >= 0 && freopen("/dev/full", "w", stderr));
    assert(setvbuf(stderr, NULL, _IONBF, 0) == 0);
    progress_bar("failure", 50);
    assert(dup2(saved, STDERR_FILENO) >= 0);
    close(saved);
    clearerr(stderr);
    assert(interrupted == SIGPIPE);
    puts("Progress escaping, unknown/concurrent totals, palette, monochrome, redirection and output cancellation passed.");
    return(0);
}
