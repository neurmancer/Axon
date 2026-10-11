#ifndef AXON_ALPM_INTERNAL
#   define AXON_ALPM_INTERNAL

#include "alpm_core.h"

typedef enum {
    AXON_TRANS_IDLE,
    AXON_TRANS_BUILDING,
    AXON_TRANS_PREPARED,
    AXON_TRANS_FINISHED
} axon_trans_state_t;

struct axon_alpm {
    alpm_handle_t *handle;
    axon_trans_state_t transaction;
};

#endif
