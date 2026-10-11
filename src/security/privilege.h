#ifndef AXON_PRIVILEGE
#   define AXON_PRIVILEGE

typedef enum {
    AXON_PRIVILEGE_OK,
    AXON_PRIVILEGE_ROOT_REQUIRED,
    AXON_PRIVILEGE_UNSAFE_IDENTITY,
    AXON_PRIVILEGE_SYSTEM_ERROR
} axon_privilege_result_t;

axon_privilege_result_t axon_privilege_check(int require_root);
const char *axon_privilege_message(axon_privilege_result_t result);

#endif
