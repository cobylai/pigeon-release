// replay: the sliding anti-replay window over the link's record counter.
//
// Split out of linkcrypt.c deliberately: it depends on nothing but stdint, so
// it can be compiled and tested on the host (see test_replay.c). It is also the
// subtlest logic in the security layer -- an off-by-one here either lets a
// captured record be re-injected or locks out the legitimate peer.
//
// The window tracks the highest counter accepted (`top`) plus a bitmap of which
// of the REPLAY_WINDOW counters at or below it have already been seen (bit 0 is
// `top` itself). A counter is accepted once and only once; anything older than
// the window is refused outright, since we can no longer prove it is not a
// replay.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define REPLAY_WINDOW 64

typedef struct {
    uint32_t top;   // highest counter accepted so far
    uint64_t seen;  // bitmap; bit n = counter (top - n) has been accepted
    bool any;       // false until the first counter is accepted
} replay_window_t;

// Arm an empty window. Call at the start of every session.
void replay_reset(replay_window_t *w);

// Check a received counter and, if fresh, record it. Returns false for a
// counter that is zero (counters start at 1), already seen, or older than the
// window. Only call this AFTER the record's tag verifies, or an attacker could
// advance the window with forged counters and lock out the real peer.
bool replay_accept(replay_window_t *w, uint32_t ctr);
