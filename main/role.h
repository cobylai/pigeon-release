// Board role, resolved at boot from NVS. One firmware image runs on both sticks;
// this decides which is the Mac-side Nest and which is the target-side Pigeon,
// and therefore which USB personality the board enumerates as this session.
//
// UNSET is the factory default: the board comes up CDC-only and waits to be told
// its role (operator runs `pigeon-send set-nest`, or it hears a Nest over the
// radio and becomes the Pigeon). Once set, the role is remembered, so every
// later boot comes up in-role with no negotiation.

#pragma once

#include <stdbool.h>

typedef enum {
    ROLE_UNSET = 0,
    ROLE_NEST = 1,
    ROLE_PIGEON = 2,
} board_role_t;

// Read the persisted role (ROLE_UNSET if never written or NVS unavailable).
board_role_t role_load(void);

// Persist a role. Returns true on success.
bool role_store(board_role_t role);

// Persist a role and reboot into it. USB personality is fixed at enumeration, so
// a role change always takes effect via a clean restart. Does not return.
void role_apply(board_role_t role);

const char *role_name(board_role_t role);
