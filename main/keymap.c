#include "keymap.h"

#define SH KEYMAP_SHIFT

// Indexed by ASCII value; 0 means "no keystroke for this character".
// Kept as explicit designated initializers rather than built by loops at init:
// a keymap is the kind of table that is worth being able to read straight down
// and check against a physical keyboard.
static const uint8_t ascii_hid[128] = {
    ['\t'] = 0x2B,
    ['\n'] = 0x28,
    ['\r'] = 0x28,
    [' '] = 0x2C,

    ['a'] = 0x04, ['b'] = 0x05, ['c'] = 0x06, ['d'] = 0x07, ['e'] = 0x08,
    ['f'] = 0x09, ['g'] = 0x0A, ['h'] = 0x0B, ['i'] = 0x0C, ['j'] = 0x0D,
    ['k'] = 0x0E, ['l'] = 0x0F, ['m'] = 0x10, ['n'] = 0x11, ['o'] = 0x12,
    ['p'] = 0x13, ['q'] = 0x14, ['r'] = 0x15, ['s'] = 0x16, ['t'] = 0x17,
    ['u'] = 0x18, ['v'] = 0x19, ['w'] = 0x1A, ['x'] = 0x1B, ['y'] = 0x1C,
    ['z'] = 0x1D,

    ['A'] = SH | 0x04, ['B'] = SH | 0x05, ['C'] = SH | 0x06, ['D'] = SH | 0x07,
    ['E'] = SH | 0x08, ['F'] = SH | 0x09, ['G'] = SH | 0x0A, ['H'] = SH | 0x0B,
    ['I'] = SH | 0x0C, ['J'] = SH | 0x0D, ['K'] = SH | 0x0E, ['L'] = SH | 0x0F,
    ['M'] = SH | 0x10, ['N'] = SH | 0x11, ['O'] = SH | 0x12, ['P'] = SH | 0x13,
    ['Q'] = SH | 0x14, ['R'] = SH | 0x15, ['S'] = SH | 0x16, ['T'] = SH | 0x17,
    ['U'] = SH | 0x18, ['V'] = SH | 0x19, ['W'] = SH | 0x1A, ['X'] = SH | 0x1B,
    ['Y'] = SH | 0x1C, ['Z'] = SH | 0x1D,

    // Digit row. '0' is 0x27, after '9' -- not before '1'.
    ['1'] = 0x1E, ['2'] = 0x1F, ['3'] = 0x20, ['4'] = 0x21, ['5'] = 0x22,
    ['6'] = 0x23, ['7'] = 0x24, ['8'] = 0x25, ['9'] = 0x26, ['0'] = 0x27,

    // Shifted digit row, same keys in the same order.
    ['!'] = SH | 0x1E, ['@'] = SH | 0x1F, ['#'] = SH | 0x20, ['$'] = SH | 0x21,
    ['%'] = SH | 0x22, ['^'] = SH | 0x23, ['&'] = SH | 0x24, ['*'] = SH | 0x25,
    ['('] = SH | 0x26, [')'] = SH | 0x27,

    ['-'] = 0x2D,  ['_'] = SH | 0x2D,
    ['='] = 0x2E,  ['+'] = SH | 0x2E,
    ['['] = 0x2F,  ['{'] = SH | 0x2F,
    [']'] = 0x30,  ['}'] = SH | 0x30,
    ['\\'] = 0x31, ['|'] = SH | 0x31,
    [';'] = 0x33,  [':'] = SH | 0x33,
    ['\''] = 0x34, ['"'] = SH | 0x34,
    ['`'] = 0x35,  ['~'] = SH | 0x35,
    [','] = 0x36,  ['<'] = SH | 0x36,
    ['.'] = 0x37,  ['>'] = SH | 0x37,
    ['/'] = 0x38,  ['?'] = SH | 0x38,
};

uint8_t keymap_lookup(char c) {
    uint8_t idx = (uint8_t)c;
    if (idx >= sizeof(ascii_hid)) {
        // Non-ASCII (UTF-8 continuation bytes included). No US-QWERTY
        // keystroke exists; the caller skips it rather than typing garbage.
        return 0;
    }
    return ascii_hid[idx];
}
