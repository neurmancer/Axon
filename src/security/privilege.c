#define _GNU_SOURCE

#include "privilege.h"

#include <errno.h>
#include <sys/auxv.h>
#include <unistd.h>

axon_privilege_result_t axon_privilege_check(int require_root)
{
    uid_t real_user, effective_user, saved_user;
    gid_t real_group, effective_group, saved_group;

    if(getresuid(&real_user, &effective_user, &saved_user) < 0
            || getresgid(&real_group, &effective_group, &saved_group) < 0) {
        return(AXON_PRIVILEGE_SYSTEM_ERROR);
    }

    errno = 0;
    unsigned long secure = getauxval(AT_SECURE);
    if(errno != 0) {
        return(AXON_PRIVILEGE_SYSTEM_ERROR);
    }

    if(secure != 0 || real_user != effective_user || effective_user != saved_user
            || real_group != effective_group || effective_group != saved_group) {
        return(AXON_PRIVILEGE_UNSAFE_IDENTITY);
    }

    if(require_root && effective_user != 0) {
        return(AXON_PRIVILEGE_ROOT_REQUIRED);
    }

    return(AXON_PRIVILEGE_OK);
}

const char *axon_privilege_message(axon_privilege_result_t result)
{
    switch(result) {
        case AXON_PRIVILEGE_OK:
            return("authorized process identity");
        case AXON_PRIVILEGE_ROOT_REQUIRED:
            return("this operation requires root; launch through an administrator-authorized privilege tool");
        case AXON_PRIVILEGE_UNSAFE_IDENTITY:
            return("setuid/setgid, saved elevated credentials, and secure-execution launches are unsupported");
        case AXON_PRIVILEGE_SYSTEM_ERROR:
            return("cannot verify process credentials");
        default:
            return("unknown privilege error");
    }
}
