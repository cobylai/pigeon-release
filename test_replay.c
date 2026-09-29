// Host test for the link's anti-replay window (main/replay.c).
//
// This is the one piece of the Stage 4 security layer that can be exercised
// without a board, and it is the piece most likely to be subtly wrong: too
// strict and a legitimate retransmit wedges the link, too loose and a captured
// record can be re-injected as keystrokes.
//
//   cc -Wall -Wextra -Werror -o /tmp/test_replay test_replay.c main/replay.c -Imain && /tmp/test_replay

#include <stdio.h>

#include "replay.h"

static int failures;

static void check(int cond, const char *what) {
    if (!cond) {
        printf("FAIL: %s\n", what);
        failures++;
    }
}

int main(void) {
    replay_window_t w;

    // A fresh window accepts the first counter, whatever it is, and rejects 0.
    replay_reset(&w);
    check(!replay_accept(&w, 0), "counter 0 is refused");
    check(replay_accept(&w, 1), "first counter accepted");
    check(!replay_accept(&w, 1), "immediate duplicate refused");

    // Ordinary forward progress.
    for (uint32_t c = 2; c <= 100; c++) {
        check(replay_accept(&w, c), "sequential counters accepted");
    }
    check(!replay_accept(&w, 100), "the newest counter cannot be replayed");
    check(!replay_accept(&w, 50), "a mid-window counter cannot be replayed");

    // Out-of-order delivery inside the window is fine and each arrival counts
    // exactly once -- this is what keeps a delayed packet from being dropped.
    replay_reset(&w);
    check(replay_accept(&w, 10), "start at 10");
    check(replay_accept(&w, 12), "skip ahead to 12");
    check(replay_accept(&w, 11), "the gap fills late");
    check(!replay_accept(&w, 11), "but only once");
    check(!replay_accept(&w, 12), "and 12 is still spent");

    // The window edge. With top = 100 the oldest acceptable counter is
    // 100 - 63 = 37; 36 has fallen off and must be refused.
    replay_reset(&w);
    check(replay_accept(&w, 100), "top = 100");
    check(replay_accept(&w, 100 - (REPLAY_WINDOW - 1)), "oldest in-window counter accepted");
    check(!replay_accept(&w, 100 - REPLAY_WINDOW), "one past the window is refused");
    check(!replay_accept(&w, 1), "far older is refused");

    // A jump of exactly the window size must clear the bitmap rather than
    // shifting by 64, which would be undefined behaviour on a uint64_t.
    replay_reset(&w);
    check(replay_accept(&w, 1), "seed at 1");
    check(replay_accept(&w, 1 + REPLAY_WINDOW), "jump exactly one window");
    check(!replay_accept(&w, 1), "the seed is now out of window");
    check(replay_accept(&w, 1 + REPLAY_WINDOW - 1), "the new window's floor is open");

    // A huge jump (a peer that ran a long time) must not corrupt the window.
    replay_reset(&w);
    check(replay_accept(&w, 5), "seed at 5");
    check(replay_accept(&w, 4000000000u), "large forward jump accepted");
    check(!replay_accept(&w, 4000000000u), "and is not replayable");
    check(!replay_accept(&w, 5), "the old counter is long gone");

    // The retransmit case that motivated resealing every attempt: a data record
    // (ctr 7), a keepalive that overtakes it (ctr 8), then the retransmit under
    // a FRESH counter (ctr 9) must be accepted. Had the retransmit reused 7 it
    // would still pass here -- but only because 7 is inside the window; the
    // firmware must not rely on that.
    replay_reset(&w);
    check(replay_accept(&w, 7), "data record");
    check(replay_accept(&w, 8), "keepalive overtakes it");
    check(replay_accept(&w, 9), "resealed retransmit accepted");

    if (failures == 0) {
        printf("all replay-window tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
