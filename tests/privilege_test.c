#define _GNU_SOURCE

#include "privilege.h"
#include "alpm_extra.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/auxv.h>
#include <sys/stat.h>
#include <unistd.h>

int __real_getresuid(uid_t *, uid_t *, uid_t *);
int __real_getresgid(gid_t *, gid_t *, gid_t *);
unsigned long __real_getauxval(unsigned long);

static int simulated;
static int syscall_failure;
static unsigned long secure;
static uid_t users[3];
static gid_t groups[3];

int __wrap_getresuid(uid_t *real, uid_t *effective, uid_t *saved)
{
    if(!simulated) {
        return(__real_getresuid(real, effective, saved));
    }
    if(syscall_failure == 1) {
        errno = EPERM;
        return(-1);
    }
    *real = users[0];
    *effective = users[1];
    *saved = users[2];
    return(0);
}

int __wrap_getresgid(gid_t *real, gid_t *effective, gid_t *saved)
{
    if(!simulated) {
        return(__real_getresgid(real, effective, saved));
    }
    if(syscall_failure == 2) {
        errno = EPERM;
        return(-1);
    }
    *real = groups[0];
    *effective = groups[1];
    *saved = groups[2];
    return(0);
}

unsigned long __wrap_getauxval(unsigned long type)
{
    if(!simulated) {
        return(__real_getauxval(type));
    }
    assert(type == AT_SECURE);
    if(syscall_failure == 3) {
        errno = ENOENT;
    }
    return(secure);
}

static void user(uid_t id)
{
    users[0] = users[1] = users[2] = id;
}

int main(void)
{
    assert(axon_privilege_check(0) == AXON_PRIVILEGE_OK);
    if(geteuid() != 0) {
        assert(axon_privilege_check(1) == AXON_PRIVILEGE_ROOT_REQUIRED);
    }
    char directory[] = "/tmp/axon-privilege-XXXXXX";
    assert(mkdtemp(directory) != NULL);
    char local[512], lock[512];
    snprintf(local, sizeof(local), "%s/local", directory);
    snprintf(lock, sizeof(lock), "%s/db.lck", directory);
    assert(mkdir(local, 0700) == 0);
    axon_alpm_t *core = NULL;
    assert(axon_alpm_init(&core, directory, directory) == ALPM_ERR_OK);
    if(geteuid() != 0) {
        assert(setenv("SUDO_UID", "0", 1) == 0);
        assert(setenv("USER", "root", 1) == 0);
        assert(axon_alpm_transaction_begin(core, 0) == ALPM_ERR_BADPERMS);
        assert(axon_alpm_refresh(core, 0, NULL) == ALPM_ERR_BADPERMS);
        assert(axon_alpm_set_reason(core, "missing", ALPM_PKG_REASON_EXPLICIT) == ALPM_ERR_BADPERMS);
        assert(access(lock, F_OK) != 0);
    }

    simulated = 1;
    user(1000);
    assert(axon_privilege_check(0) == AXON_PRIVILEGE_OK);
    assert(axon_privilege_check(1) == AXON_PRIVILEGE_ROOT_REQUIRED);
    users[1] = 0;
    assert(axon_privilege_check(0) == AXON_PRIVILEGE_UNSAFE_IDENTITY);
    user(1000);
    users[2] = 0;
    assert(axon_privilege_check(0) == AXON_PRIVILEGE_UNSAFE_IDENTITY);
    user(0);
    groups[1] = 42;
    assert(axon_privilege_check(0) == AXON_PRIVILEGE_UNSAFE_IDENTITY);
    groups[1] = 0;
    groups[2] = 42;
    assert(axon_privilege_check(0) == AXON_PRIVILEGE_UNSAFE_IDENTITY);
    groups[2] = 0;
    secure = 1;
    assert(axon_privilege_check(0) == AXON_PRIVILEGE_UNSAFE_IDENTITY);
    axon_alpm_t *rejected = NULL;
    assert(axon_alpm_init(&rejected, directory, directory) == ALPM_ERR_BADPERMS);
    assert(rejected == NULL);
    secure = 0;
    for(syscall_failure = 1; syscall_failure <= 3; syscall_failure++) {
        assert(axon_privilege_check(1) == AXON_PRIVILEGE_SYSTEM_ERROR);
    }
    syscall_failure = 0;
    assert(axon_privilege_check(1) == AXON_PRIVILEGE_OK);
    assert(axon_alpm_transaction_begin(core, ALPM_TRANS_FLAG_NOLOCK) == ALPM_ERR_WRONG_ARGS);
    assert(access(lock, F_OK) != 0);
    assert(axon_alpm_transaction_begin(core, 0) == ALPM_ERR_OK);
    assert(access(lock, F_OK) == 0);
    axon_alpm_problem_t problem = {0};
    user(1000);
    assert(axon_alpm_transaction_file(core, "/missing") == ALPM_ERR_BADPERMS);
    assert(axon_alpm_transaction_prepare(core, &problem) == ALPM_ERR_BADPERMS);
    user(0);
    assert(axon_alpm_transaction_prepare(core, &problem) == ALPM_ERR_OK);
    user(1000);
    assert(axon_alpm_transaction_commit(core, &problem) == ALPM_ERR_BADPERMS);
    assert(problem.code == ALPM_ERR_BADPERMS && problem.details == NULL);
    assert(axon_alpm_transaction_release(core) == ALPM_ERR_OK);
    assert(access(lock, F_OK) != 0);
    assert(axon_alpm_release(&core) == 0);
    char version[512];
    snprintf(version, sizeof(version), "%s/local/ALPM_DB_VERSION", directory);
    if(unlink(version) != 0) {
        assert(errno == ENOENT);
    }
    assert(rmdir(local) == 0);
    assert(rmdir(directory) == 0);
    puts("Privilege checks passed; no actual credentials were elevated.");
    return(0);
}
