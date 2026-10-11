#define _POSIX_C_SOURCE 200809L

#include "pacman_config.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void __real_free(void *pointer);

static void *live[1024];
static size_t attempts;
static size_t failure;
static int enabled;

static int should_fail(void)
{
    if(enabled) {
        attempts++;
        return(failure != 0 && attempts == failure);
    }
    return(0);
}

static void *track(void *pointer)
{
    if(enabled && pointer != NULL) {
        for(size_t index = 0; index < sizeof(live) / sizeof(live[0]); index++) {
            if(live[index] == NULL) {
                live[index] = pointer;
                return(pointer);
            }
        }
        abort();
    }
    return(pointer);
}

void *__wrap_malloc(size_t size)
{
    return(should_fail() ? NULL : track(__real_malloc(size)));
}

void *__wrap_calloc(size_t count, size_t size)
{
    return(should_fail() ? NULL : track(__real_calloc(count, size)));
}

char *__wrap_strdup(const char *text)
{
    if(should_fail()) {
        return(NULL);
    }
    size_t length = strlen(text) + 1;
    char *copy = track(__real_malloc(length));
    if(copy != NULL) {
        memcpy(copy, text, length);
    }
    return(copy);
}

void __wrap_free(void *pointer)
{
    for(size_t index = 0; index < sizeof(live) / sizeof(live[0]); index++) {
        if(live[index] == pointer) {
            live[index] = NULL;
        }
    }
    __real_free(pointer);
}

static size_t run(const char *path, size_t fail_at)
{
    axon_config_t *config = NULL;
    axon_config_error_t error;
    attempts = 0;
    failure = fail_at;
    enabled = 1;
    axon_config_result_t result = axon_config_load(path, &config, &error);
    if(fail_at != 0) {
        assert(result == AXON_CONFIG_MEMORY);
        assert(config == NULL && error.code == AXON_CONFIG_MEMORY);
    }

    else {
        assert(result == AXON_CONFIG_OK && config != NULL);
    }
    axon_config_free(&config);
    enabled = 0;
    for(size_t index = 0; index < sizeof(live) / sizeof(live[0]); index++) {
        assert(live[index] == NULL);
    }
    return(attempts);
}

int main(void)
{
    char path[] = "/tmp/axon-config-alloc-XXXXXX";
    int descriptor = mkstemp(path);
    assert(descriptor >= 0);
    FILE *file = fdopen(descriptor, "w");
    assert(file != NULL);
    assert(fputs("[options]\nArchitecture = auto\nIgnorePkg = one two\n"
                 "SigLevel = Required DatabaseOptional\nLocalFileSigLevel = Optional\n"
                 "[core]\nServer = https://example/$repo/$arch\n"
                 "[extra]\nServer = file:///mirror/$repo/$arch\n", file) >= 0);
    assert(fclose(file) == 0);
    size_t count = run(path, 0);
    for(size_t index = 1; index <= count; index++) {
        run(path, index);
    }
    assert(unlink(path) == 0);
    printf("Configuration allocation failure checks passed (%zu allocation points).\n", count);
    return(0);
}
