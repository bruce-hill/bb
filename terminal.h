//
// terminal.h
// Copyright 2020 Bruce Hill
// Released under the MIT License
//
// Definitions of some basic terminal stuff, like reading keys and some
// terminal escape sequences.
//

#ifndef FILE__TERMINAL_H
#define FILE__TERMINAL_H

#include <stdio.h>
#include <time.h>
#include <unistd.h>

// Maximum time in milliseconds between double clicks
#define DOUBLECLICK_THRESHOLD 200

typedef enum {
    // ASCII chars:
    KEY_CTRL_AT = 0x00, KEY_CTRL_A, KEY_CTRL_B, KEY_CTRL_C, KEY_CTRL_D,
    KEY_CTRL_E, KEY_CTRL_F, KEY_CTRL_G, KEY_CTRL_H, KEY_CTRL_I, KEY_CTRL_J,
    KEY_CTRL_K, KEY_CTRL_L, KEY_CTRL_M, KEY_CTRL_N, KEY_CTRL_O, KEY_CTRL_P,
    KEY_CTRL_Q, KEY_CTRL_R, KEY_CTRL_S, KEY_CTRL_T, KEY_CTRL_U, KEY_CTRL_V,
    KEY_CTRL_W, KEY_CTRL_X, KEY_CTRL_Y, KEY_CTRL_Z,
    KEY_CTRL_LSQ_BRACKET, KEY_CTRL_BACKSLASH, KEY_CTRL_RSQ_BRACKET,
    KEY_CTRL_CARET, KEY_CTRL_UNDERSCORE, KEY_SPACE,
    // Printable chars would be here
    KEY_BACKSPACE2 = 0x7F,

    // Non-ascii multi-byte keys:
    KEY_F0, KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8,
    KEY_F9, KEY_F10, KEY_F11, KEY_F12,
    KEY_INSERT, KEY_DELETE, KEY_HOME, KEY_END, KEY_PGUP, KEY_PGDN,
    KEY_ARROW_UP, KEY_ARROW_DOWN, KEY_ARROW_LEFT, KEY_ARROW_RIGHT,
    MOUSE_LEFT_PRESS, MOUSE_RIGHT_PRESS, MOUSE_MIDDLE_PRESS,
    MOUSE_LEFT_DRAG, MOUSE_RIGHT_DRAG, MOUSE_MIDDLE_DRAG,
    MOUSE_LEFT_RELEASE, MOUSE_RIGHT_RELEASE, MOUSE_MIDDLE_RELEASE,
    MOUSE_LEFT_DOUBLE, MOUSE_RIGHT_DOUBLE, MOUSE_MIDDLE_DOUBLE,
    MOUSE_WHEEL_RELEASE, MOUSE_WHEEL_PRESS,
} bkey_t;

#define MOD_BITSHIFT  9
#define MOD_META   (1 << (MOD_BITSHIFT + 0))
#define MOD_CTRL   (1 << (MOD_BITSHIFT + 1))
#define MOD_ALT    (1 << (MOD_BITSHIFT + 2))
#define MOD_SHIFT  (1 << (MOD_BITSHIFT + 3))

// Overlapping key codes:
#define KEY_CTRL_BACKTICK    0x00 /* clash with ^@ */
#define KEY_CTRL_2           0x00 /* clash with ^@ */
#define KEY_BACKSPACE        0x08 /* clash with ^H */
#define KEY_TAB              0x09 /* clash with ^I */
#define KEY_ENTER            0x0D /* clash with ^M */
#define KEY_ESC              0x1B /* clash with ^[ */
#define KEY_CTRL_3           0x1B /* clash with ^[ */
#define KEY_CTRL_4           0x1C /* clash with ^\ */
#define KEY_CTRL_5           0x1D /* clash with ^] */
#define KEY_CTRL_TILDE       0x1E /* clash with ^^ */
#define KEY_CTRL_6           0x1E /* clash with ^^ */
#define KEY_CTRL_7           0x1F /* clash with ^_ */
#define KEY_CTRL_SLASH       0x1F /* clash with ^_ */
#define KEY_SPACE            0x20
#define KEY_BACKSPACE2       0x7F
#define KEY_CTRL_8           0x7F /* clash with 'BACKSPACE2' */

// Terminal escape sequences:
#define T_WRAP        "7"
#define T_SHOW_CURSOR "25"
#define T_MOUSE_XY    "1000"
#define T_MOUSE_CELL  "1002"
#define T_MOUSE_SGR   "1006"
#define T_ALT_SCREEN  "1049"
#define T_ON(opt)  "\033[?" opt "h"
#define T_OFF(opt) "\033[?" opt "l"

#define move_cursor(f, x, y) fprintf((f), "\033[%d;%dH", (int)(y)+1, (int)(x)+1)
#define move_cursor_col(f, x) fprintf((f), "\033[%d`", (int)(x)+1)

int bgetkey(FILE *in, int *mouse_x, int *mouse_y);
char *bkeyname(int key, char *buf);
int bkeywithname(const char *name);

#endif
// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
