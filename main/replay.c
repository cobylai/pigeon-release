#include "replay.h"

void replay_reset(replay_window_t *w) {
    w->top = 0;
    w->seen = 0;
    w->any = false;
}

bool replay_accept(replay_window_t *w, uint32_t ctr) {
    if (ctr == 0) {
        return false;  // counters start at 1; 0 means a malformed record
    }
    if (!w->any) {
        w->any = true;
        w->top = ctr;
        w->seen = 1;  // bit 0 marks top itself
        return true;
    }
    if (ctr > w->top) {
        uint32_t advance = ctr - w->top;
        // Shifting a uint64_t by 64 or more is undefined behaviour, so a jump
        // that clears the whole window has to be handled separately.
        w->seen = (advance >= REPLAY_WINDOW) ? 0 : (w->seen << advance);
        w->top = ctr;
        w->seen |= 1;
        return true;
    }
    uint32_t behind = w->top - ctr;
    if (behind >= REPLAY_WINDOW) {
        return false;  // older than the window: cannot prove it is not a replay
    }
    uint64_t bit = (uint64_t)1 << behind;
    if (w->seen & bit) {
        return false;  // already accepted this counter
    }
    w->seen |= bit;
    return true;
}
