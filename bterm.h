/*
 * bterm.h
 * Copyright 2019 Bruce Hill
 * Released under the MIT License
 *
 * Definitions of some basic terminal stuff, like reading keys
 * and some terminal escape sequences.
 */

#ifndef FILE__BTERM_H
#define FILE__BTERM_H

#include <stdio.h>

#define KEY_F1               (0xFFFF-0)
#define KEY_F2               (0xFFFF-1)
#define KEY_F3               (0xFFFF-2)
#define KEY_F4               (0xFFFF-3)
#define KEY_F5               (0xFFFF-4)
#define KEY_F6               (0xFFFF-5)
#define KEY_F7               (0xFFFF-6)
#define KEY_F8               (0xFFFF-7)
#define KEY_F9               (0xFFFF-8)
#define KEY_F10              (0xFFFF-9)
#define KEY_F11              (0xFFFF-10)
#define KEY_F12              (0xFFFF-11)
#define KEY_INSERT           (0xFFFF-12)
#define KEY_DELETE           (0xFFFF-13)
#define KEY_HOME             (0xFFFF-14)
#define KEY_END              (0xFFFF-15)
#define KEY_PGUP             (0xFFFF-16)
#define KEY_PGDN             (0xFFFF-17)
#define KEY_ARROW_UP         (0xFFFF-18)
#define KEY_ARROW_DOWN       (0xFFFF-19)
#define KEY_ARROW_LEFT       (0xFFFF-20)
#define KEY_ARROW_RIGHT      (0xFFFF-21)
#define KEY_MOUSE_LEFT       (0xFFFF-22)
#define KEY_MOUSE_RIGHT      (0xFFFF-23)
#define KEY_MOUSE_MIDDLE     (0xFFFF-24)
#define KEY_MOUSE_RELEASE    (0xFFFF-25)
#define KEY_MOUSE_WHEEL_UP   (0xFFFF-26)
#define KEY_MOUSE_WHEEL_DOWN (0xFFFF-27)
#define KEY_MOUSE_DOUBLE_LEFT (0xFFFF-28)

/* These are all ASCII code points below SPACE character and a BACKSPACE key. */
#define KEY_CTRL_TILDE       0x00
#define KEY_CTRL_2           0x00 /* clash with 'CTRL_TILDE' */
#define KEY_CTRL_A           0x01
#define KEY_CTRL_B           0x02
#define KEY_CTRL_C           0x03
#define KEY_CTRL_D           0x04
#define KEY_CTRL_E           0x05
#define KEY_CTRL_F           0x06
#define KEY_CTRL_G           0x07
#define KEY_BACKSPACE        0x08
#define KEY_CTRL_H           0x08 /* clash with 'CTRL_BACKSPACE' */
#define KEY_TAB              0x09
#define KEY_CTRL_I           0x09 /* clash with 'TAB' */
#define KEY_CTRL_J           0x0A
#define KEY_CTRL_K           0x0B
#define KEY_CTRL_L           0x0C
#define KEY_ENTER            0x0D
#define KEY_CTRL_M           0x0D /* clash with 'ENTER' */
#define KEY_CTRL_N           0x0E
#define KEY_CTRL_O           0x0F
#define KEY_CTRL_P           0x10
#define KEY_CTRL_Q           0x11
#define KEY_CTRL_R           0x12
#define KEY_CTRL_S           0x13
#define KEY_CTRL_T           0x14
#define KEY_CTRL_U           0x15
#define KEY_CTRL_V           0x16
#define KEY_CTRL_W           0x17
#define KEY_CTRL_X           0x18
#define KEY_CTRL_Y           0x19
#define KEY_CTRL_Z           0x1A
#define KEY_ESC              0x1B
#define KEY_CTRL_LSQ_BRACKET 0x1B /* clash with 'ESC' */
#define KEY_CTRL_3           0x1B /* clash with 'ESC' */
#define KEY_CTRL_4           0x1C
#define KEY_CTRL_BACKSLASH   0x1C /* clash with 'CTRL_4' */
#define KEY_CTRL_5           0x1D
#define KEY_CTRL_RSQ_BRACKET 0x1D /* clash with 'CTRL_5' */
#define KEY_CTRL_6           0x1E
#define KEY_CTRL_7           0x1F
#define KEY_CTRL_SLASH       0x1F /* clash with 'CTRL_7' */
#define KEY_CTRL_UNDERSCORE  0x1F /* clash with 'CTRL_7' */
#define KEY_SPACE            0x20
#define KEY_BACKSPACE2       0x7F
#define KEY_CTRL_8           0x7F /* clash with 'BACKSPACE2' */


// Terminal escape sequences:
#define CSI           "\033["
#define T_WRAP        "7"
#define T_SHOW_CURSOR "25"
#define T_MOUSE_XY    "1000"
#define T_MOUSE_CELL  "1002"
#define T_MOUSE_SGR   "1006"
#define T_ALT_SCREEN  "1049"
#define T_ON(opt)  CSI "?" opt "h"
#define T_OFF(opt) CSI "?" opt "l"

#define move_cursor(f, x, y) fprintf((f), CSI "%d;%dH", (int)(y)+1, (int)(x)+1)


#define ESC_DELAY 10

int bgetkey(FILE *in, int *mouse_x, int *mouse_y, int timeout);
const char *bkeyname(int key);

static inline int nextchar(int fd, int timeout)
{
    (void)timeout;
    char c;
    return read(fd, &c, 1) == 1 ? c : -1;
}

/*
 * Get one key of input from the given file. Returns -1 on failure.
 * If mouse_x or mouse_y are non-null and a mouse event occurs, they will be
 * set to the position of the mouse (0-indexed).
 */
int bgetkey(FILE *in, int *mouse_x, int *mouse_y, int timeout)
{
    int fd = fileno(in);
    int numcode = 0, super = 0;
    int c = nextchar(fd, timeout);
    if (c == '\x1b')
        goto escape;

    return c;

  escape:
    c = nextchar(fd, ESC_DELAY);
    // Actual escape key:
    if (c < 0)
        return KEY_ESC;

    switch (c) {
        case '\x1b': ++super; goto escape;
        case '[': goto CSI_start;
        case 'P': goto DCS;
        case 'O': goto SS3;
        default: return -1;
    }

  CSI_start:
    c = nextchar(fd, ESC_DELAY);
    if (c == -1)
        return -1;

    switch (c) {
        case 'A': return KEY_ARROW_UP;
        case 'B': return KEY_ARROW_DOWN;
        case 'C': return KEY_ARROW_RIGHT;
        case 'D': return KEY_ARROW_LEFT;
        case 'F': return KEY_END;
        case 'H': return KEY_HOME;
        case '~':
            switch (numcode) {
                case 3: return KEY_DELETE;
                case 5: return KEY_PGUP;
                case 6: return KEY_PGDN;
                case 15: return KEY_F5;
                case 17: return KEY_F6;
                case 18: return KEY_F7;
                case 19: return KEY_F8;
                case 20: return KEY_F9;
                case 21: return KEY_F10;
                case 23: return KEY_F11;
                case 24: return KEY_F12;
            }
            return -1;
        case '<': { // Mouse clicks
            int buttons = 0, x = 0, y = 0;
            char buf;
            while (read(fd, &buf, 1) == 1 && '0' <= buf && buf <= '9')
                buttons = buttons * 10 + (buf - '0');
            if (buf != ';') return -1;
            while (read(fd, &buf, 1) == 1 && '0' <= buf && buf <= '9')
                x = x * 10 + (buf - '0');
            if (buf != ';') return -1;
            while (read(fd, &buf, 1) == 1 && '0' <= buf && buf <= '9')
                y = y * 10 + (buf - '0');
            if (buf != 'm' && buf != 'M') return -1;

            if (mouse_x) *mouse_x = x - 1;
            if (mouse_y) *mouse_y = y - 1;

            if (buf == 'm')
                return KEY_MOUSE_RELEASE;
            switch (buttons) {
                case 64: return KEY_MOUSE_WHEEL_UP;
                case 65: return KEY_MOUSE_WHEEL_DOWN;
                case 0: return KEY_MOUSE_LEFT;
                case 1: return KEY_MOUSE_RIGHT;
                case 2: return KEY_MOUSE_MIDDLE;
                default: return -1;
            }
            break;
        }
        default:
            if ('0' <= c && c <= '9') {
                // Ps prefix
                numcode = 10*numcode + (c - '0');
                goto CSI_start;
            }
    }
    return -1;

  DCS:
    return -1;

  SS3:
    switch (nextchar(fd, ESC_DELAY)) {
        case 'P': return KEY_F1;
        case 'Q': return KEY_F2;
        case 'R': return KEY_F3;
        case 'S': return KEY_F4;
        default: break;
    }
    return -1;
}

/*
 * Return the name of a key, if one exists and is different from the key itself
 * (i.e. bkeyname('c') == NULL, bkeyname(' ') == "Space")
 */
const char *bkeyname(int key)
{
    // TODO: currently only the keys I'm using are named
    switch (key) {
        case KEY_F1: return "F1";
        case KEY_F2: return "F2";
        case KEY_F3: return "F3";
        case KEY_F4: return "F4";
        case KEY_F5: return "F5";
        case KEY_F6: return "F6";
        case KEY_F7: return "F7";
        case KEY_F8: return "F8";
        case KEY_F9: return "F9";
        case KEY_F10: return "F10";
        case KEY_F11: return "F11";
        case KEY_F12: return "F12";
        case KEY_INSERT: return "Insert";
        case KEY_DELETE: return "Delete";
        case KEY_HOME: return "Home";
        case KEY_END: return "End";
        case KEY_PGUP: return "PgUp";
        case KEY_PGDN: return "PgDn";
        case KEY_ARROW_UP: return "Up";
        case KEY_ARROW_DOWN: return "Down";
        case KEY_ARROW_LEFT: return "Left";
        case KEY_ARROW_RIGHT: return "Right";
        case KEY_MOUSE_LEFT: return "Left click";
        case KEY_MOUSE_RIGHT: return "Right click";
        case KEY_MOUSE_MIDDLE: return "Middle click";
        case KEY_MOUSE_RELEASE: return "Mouse release";
        case KEY_MOUSE_WHEEL_UP: return "Mouse wheel up";
        case KEY_MOUSE_WHEEL_DOWN: return "Mouse wheel down";
        case KEY_MOUSE_DOUBLE_LEFT: return "Double left click";
        case KEY_CTRL_TILDE: return "Ctrl-[2~]";
        case KEY_CTRL_A: return "Ctrl-a";
        case KEY_CTRL_B: return "Ctrl-b";
        case KEY_CTRL_C: return "Ctrl-c";
        case KEY_CTRL_D: return "Ctrl-d";
        case KEY_CTRL_E: return "Ctrl-e";
        case KEY_CTRL_F: return "Ctrl-f";
        case KEY_CTRL_G: return "Ctrl-g";
        case KEY_CTRL_H: return "Ctrl-h";
        case KEY_TAB: return "Tab";
        case KEY_CTRL_J: return "Ctrl-j";
        case KEY_CTRL_K: return "Ctrl-k";
        case KEY_CTRL_L: return "Ctrl-l";
        case KEY_ENTER: return "Enter";
        case KEY_CTRL_N: return "Ctrl-n";
        case KEY_CTRL_O: return "Ctrl-o";
        case KEY_CTRL_P: return "Ctrl-p";
        case KEY_CTRL_Q: return "Ctrl-q";
        case KEY_CTRL_R: return "Ctrl-r";
        case KEY_CTRL_S: return "Ctrl-s";
        case KEY_CTRL_T: return "Ctrl-t";
        case KEY_CTRL_U: return "Ctrl-u";
        case KEY_CTRL_V: return "Ctrl-v";
        case KEY_CTRL_W: return "Ctrl-w";
        case KEY_CTRL_X: return "Ctrl-x";
        case KEY_CTRL_Y: return "Ctrl-y";
        case KEY_CTRL_Z: return "Ctrl-z";
        case KEY_ESC: return "Esc";
        case KEY_CTRL_BACKSLASH: return "Ctrl-[4\\]";
        case KEY_CTRL_RSQ_BRACKET: return "Ctrl-[5]]";
        case KEY_CTRL_6: return "Ctrl-6";
        case KEY_CTRL_SLASH: return "Ctrl-[7/_]";
        case KEY_SPACE: return "Space";
        case KEY_BACKSPACE2: return "Backspace";
    }
    return NULL;
}

#endif
// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
