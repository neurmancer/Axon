#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>

int statvfs64(const char *path, struct statvfs64 *buffer)
{
    int (*original)(const char *, struct statvfs64 *);
    void *symbol = dlsym(RTLD_NEXT, "statvfs64");
    _Static_assert(sizeof(original) == sizeof(symbol), "function pointer size");
    memcpy(&original, &symbol, sizeof(original));
    if(original == NULL) {
        errno = ENOSYS;
        return(-1);
    }
    int result = original(path, buffer);
    if(result == 0 && getenv("AXON_TEST_DISK_FULL") != NULL) {
        buffer->f_bavail = 0;
        buffer->f_bfree = 0;
    }
    return(result);
}
