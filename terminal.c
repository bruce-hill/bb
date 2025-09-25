//
// terminal.c
// Copyright 2020 Bruce Hill
// Released under the MIT License
//
// Implementations of some basic terminal stuff, like reading keys and some
// terminal escape sequences.
//

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "terminal.h"

typedef struct {
    int key;
    const char *name;
} keyname_t;

static keyname_t key_names[] = {
    {KEY_SPACE, "Space"},
    {KEY_BACKSPACE2, "Backspace"},
    {KEY_F1, "F1"},
    {KEY_F2, "F2"},
    {KEY_F3, "F3"},
    {KEY_F4, "F4"},
    {KEY_F5, "F5"},
    {KEY_F6, "F6"},
    {KEY_F7, "F7"},
    {KEY_F8, "F8"},
    {KEY_F9, "F9"},
    {KEY_F10, "F10"},
    {KEY_F11, "F11"},
    {KEY_F12, "F12"},
    {KEY_INSERT, "Insert"},
    {KEY_DELETE, "Delete"},
    {KEY_HOME, "Home"},
    {KEY_END, "End"},
    {KEY_PGUP, "PgUp"},
    {KEY_PGUP, "Page Up"},
    {KEY_PGDN, "PgDn"},
    {KEY_PGDN, "Page Down"},
    {KEY_ARROW_UP, "Up"},
    {KEY_ARROW_DOWN, "Down"},
    {KEY_ARROW_LEFT, "Left"},
    {KEY_ARROW_RIGHT, "Right"},
    {MOUSE_LEFT_PRESS, "Left press"},
    {MOUSE_RIGHT_PRESS, "Right press"},
    {MOUSE_MIDDLE_PRESS, "Middle press"},
    {MOUSE_LEFT_DRAG, "Left drag"},
    {MOUSE_RIGHT_DRAG, "Right drag"},
    {MOUSE_MIDDLE_DRAG, "Middle drag"},
    {MOUSE_LEFT_RELEASE, "Left click"},
    {MOUSE_RIGHT_RELEASE, "Right click"},
    {MOUSE_MIDDLE_RELEASE, "Middle click"},
    {MOUSE_LEFT_RELEASE, "Left up"},
    {MOUSE_RIGHT_RELEASE, "Right up"},
    {MOUSE_MIDDLE_RELEASE, "Middle up"},
    {MOUSE_LEFT_RELEASE, "Left release"},
    {MOUSE_RIGHT_RELEASE, "Right release"},
    {MOUSE_MIDDLE_RELEASE, "Middle release"},
    {MOUSE_LEFT_DOUBLE, "Double left click"},
    {MOUSE_RIGHT_DOUBLE, "Double right click"},
    {MOUSE_MIDDLE_DOUBLE, "Double middle click"},
    {MOUSE_WHEEL_RELEASE, "Mouse wheel up"},
    {MOUSE_WHEEL_PRESS, "Mouse wheel down"},
    {KEY_TAB, "Tab"},
    {KEY_ENTER, "Enter"},
    {KEY_ENTER, "Return"},
    {KEY_CTRL_A, "Ctrl-a"},
    {KEY_CTRL_B, "Ctrl-b"},
    {KEY_CTRL_C, "Ctrl-c"},
    {KEY_CTRL_D, "Ctrl-d"},
    {KEY_CTRL_E, "Ctrl-e"},
    {KEY_CTRL_F, "Ctrl-f"},
    {KEY_CTRL_G, "Ctrl-g"},
    {KEY_CTRL_H, "Ctrl-h"},
    {KEY_CTRL_I, "Ctrl-i"},
    {KEY_CTRL_J, "Ctrl-j"},
    {KEY_CTRL_K, "Ctrl-k"},
    {KEY_CTRL_L, "Ctrl-l"},
    {KEY_CTRL_M, "Ctrl-m"},
    {KEY_CTRL_N, "Ctrl-n"},
    {KEY_CTRL_O, "Ctrl-o"},
    {KEY_CTRL_P, "Ctrl-p"},
    {KEY_CTRL_Q, "Ctrl-q"},
    {KEY_CTRL_R, "Ctrl-r"},
    {KEY_CTRL_S, "Ctrl-s"},
    {KEY_CTRL_T, "Ctrl-t"},
    {KEY_CTRL_U, "Ctrl-u"},
    {KEY_CTRL_V, "Ctrl-v"},
    {KEY_CTRL_W, "Ctrl-w"},
    {KEY_CTRL_X, "Ctrl-x"},
    {KEY_CTRL_Y, "Ctrl-y"},
    {KEY_CTRL_Z, "Ctrl-z"},
    {KEY_ESC, "Esc"},
    {KEY_ESC, "Escape"},
    {KEY_CTRL_TILDE, "Ctrl-~"},
    {KEY_CTRL_BACKSLASH, "Ctrl-\\"},
    {KEY_CTRL_LSQ_BRACKET, "Ctrl-]"},
    {KEY_CTRL_RSQ_BRACKET, "Ctrl-]"},
    {KEY_CTRL_UNDERSCORE, "Ctrl-_"},
    {KEY_CTRL_SLASH, "Ctrl-/"},
    {KEY_CTRL_AT, "Ctrl-@"},
    {KEY_CTRL_CARET, "Ctrl-^"},
    {KEY_CTRL_BACKTICK, "Ctrl-`"},
    {KEY_CTRL_2, "Ctrl-2"},
    {KEY_CTRL_3, "Ctrl-3"},
    {KEY_CTRL_4, "Ctrl-4"},
    {KEY_CTRL_5, "Ctrl-5"},
    {KEY_CTRL_6, "Ctrl-6"},
    {KEY_CTRL_7, "Ctrl-7"},
    {KEY_CTRL_5, "Ctrl-8"},
    {KEY_CTRL_6, "Ctrl-9"},
    {':', "Colon"},
    {',', "Comma"},
};

static int nextchar(int fd) {
    char c;
    return read(fd, &c, 1) == 1 ? c : -1;
}

static int nextnum(int fd, int c, int *n) {
    for (*n = 0; '0' <= c && c <= '9'; c = nextchar(fd))
        *n = 10 * (*n) + (c - '0');
    return c;
}

//
// Get one key of input from the given file. Returns -1 on failure.
// If mouse_x or mouse_y are non-null and a mouse event occurs, they will be
// set to the position of the mouse (0-indexed).
//
int bgetkey(FILE *in, int *mouse_x, int *mouse_y) {
    if (mouse_x) *mouse_x = -1;
    if (mouse_y) *mouse_y = -1;
    int fd = fileno(in);
    int numcode = 0, modifiers = 0;
    int c = nextchar(fd);
    if (c == '\x1b') goto escape;

    return c;

escape:
    c = nextchar(fd);
    // Actual escape key:
    if (c < 0) return KEY_ESC;

    switch (c) {
    case '\x1b': return KEY_ESC;
    case '[': c = nextchar(fd); goto CSI_start;
    case 'P': goto DCS;
    case 'O': goto SS3;
    default: return MOD_ALT | c;
    }

CSI_start:
    if (c == -1) return MOD_ALT | '[';

    switch (c) {
    case 'A': return modifiers | KEY_ARROW_UP;
    case 'B': return modifiers | KEY_ARROW_DOWN;
    case 'C': return modifiers | KEY_ARROW_RIGHT;
    case 'D': return modifiers | KEY_ARROW_LEFT;
    case 'F': return modifiers | KEY_END;
    case 'H': return modifiers | KEY_HOME;
    case 'J': return numcode == 2 ? (MOD_SHIFT | KEY_HOME) : -1;
    case 'K': return MOD_SHIFT | KEY_END;
    case 'M': return MOD_CTRL | KEY_DELETE;
    case 'P': return modifiers | (numcode == 1 ? KEY_F1 : KEY_DELETE);
    case 'Q': return numcode == 1 ? (modifiers | KEY_F2) : -1;
    case 'R': return numcode == 1 ? (modifiers | KEY_F3) : -1;
    case 'S': return numcode == 1 ? (modifiers | KEY_F4) : -1;
    case '~':
        switch (numcode) {
        case 1: return modifiers | KEY_HOME;
        case 2: return modifiers | KEY_INSERT;
        case 3: return modifiers | KEY_DELETE;
        case 4: return modifiers | KEY_END;
        case 5: return modifiers | KEY_PGUP;
        case 6: return modifiers | KEY_PGDN;
        case 7: return modifiers | KEY_HOME;
        case 8: return modifiers | KEY_END;
        case 10: return modifiers | KEY_F0;
        case 11: return modifiers | KEY_F1;
        case 12: return modifiers | KEY_F2;
        case 13: return modifiers | KEY_F3;
        case 14: return modifiers | KEY_F4;
        case 15: return modifiers | KEY_F5;
        case 17: return modifiers | KEY_F6;
        case 18: return modifiers | KEY_F7;
        case 19: return modifiers | KEY_F8;
        case 20: return modifiers | KEY_F9;
        case 21: return modifiers | KEY_F10;
        case 23: return modifiers | KEY_F11;
        case 24: return modifiers | KEY_F12;
        default: break;
        }
        return -1;
    case '<': { // Mouse clicks
        int buttons = 0, x = 0, y = 0;
        c = nextnum(fd, nextchar(fd), &buttons);
        if (c != ';') return -1;
        c = nextnum(fd, nextchar(fd), &x);
        if (c != ';') return -1;
        c = nextnum(fd, nextchar(fd), &y);
        if (c != 'm' && c != 'M') return -1;

        if (mouse_x) *mouse_x = x - 1;
        if (mouse_y) *mouse_y = y - 1;

        if (buttons & 4) modifiers |= MOD_SHIFT;
        if (buttons & 8) modifiers |= MOD_META;
        if (buttons & 16) modifiers |= MOD_CTRL;
        int key = -1;
        switch (buttons & ~(4 | 8 | 16)) {
        case 0: key = c == 'm' ? MOUSE_LEFT_RELEASE : MOUSE_LEFT_PRESS; break;
        case 1: key = c == 'm' ? MOUSE_MIDDLE_RELEASE : MOUSE_MIDDLE_PRESS; break;
        case 2: key = c == 'm' ? MOUSE_RIGHT_RELEASE : MOUSE_RIGHT_PRESS; break;
        case 32: key = MOUSE_LEFT_DRAG; break;
        case 33: key = MOUSE_MIDDLE_DRAG; break;
        case 34: key = MOUSE_RIGHT_DRAG; break;
        case 64: key = MOUSE_WHEEL_RELEASE; break;
        case 65: key = MOUSE_WHEEL_PRESS; break;
        default: return -1;
        }
        if (key == MOUSE_LEFT_RELEASE || key == MOUSE_RIGHT_RELEASE || key == MOUSE_MIDDLE_RELEASE) {
            static int lastclick = -1;
            static struct timespec lastclicktime = {0, 0};
            struct timespec clicktime;
            clock_gettime(CLOCK_MONOTONIC, &clicktime);
            if (key == lastclick) {
                double dt_ms = 1e3 * (double)(clicktime.tv_sec - lastclicktime.tv_sec)
                               + 1e-6 * (double)(clicktime.tv_nsec - lastclicktime.tv_nsec);
                if (dt_ms < DOUBLECLICK_THRESHOLD) {
                    switch (key) {
                    case MOUSE_LEFT_RELEASE: key = MOUSE_LEFT_DOUBLE; break;
                    case MOUSE_RIGHT_RELEASE: key = MOUSE_RIGHT_DOUBLE; break;
                    case MOUSE_MIDDLE_RELEASE: key = MOUSE_MIDDLE_DOUBLE; break;
                    default: break;
                    }
                }
            }
            lastclicktime = clicktime;
            lastclick = key;
        }
        return modifiers | key;
    }
    default:
        if ('0' <= c && c <= '9') {
            // Ps prefix
            c = nextnum(fd, c, &numcode);
            if (c == ';') {
                c = nextnum(fd, nextchar(fd), &modifiers);
                modifiers = (modifiers >> 1) << MOD_BITSHIFT;
            }
            goto CSI_start;
        }
    }
    return -1;

DCS:
    return -1;

SS3:
    switch (nextchar(fd)) {
    case 'P': return KEY_F1;
    case 'Q': return KEY_F2;
    case 'R': return KEY_F3;
    case 'S': return KEY_F4;
    default: break;
    }
    return -1;
}

//
// Populate `buf` with the name of a key.
//
char *bkeyname(int key, char *buf) {
    if (key & MOD_META) buf = stpcpy(buf, "Super-");
    if (key & MOD_CTRL) buf = stpcpy(buf, "Ctrl-");
    if (key & MOD_ALT) buf = stpcpy(buf, "Alt-");
    if (key & MOD_SHIFT) buf = stpcpy(buf, "Shift-");
    key &= ~(MOD_META | MOD_CTRL | MOD_ALT | MOD_SHIFT);
    for (size_t i = 0; i < sizeof(key_names) / sizeof(key_names[0]); i++) {
        if (key_names[i].key == key) {
            return stpcpy(buf, key_names[i].name);
        }
    }
    if (' ' < key && key <= '~') return buf + sprintf(buf, "%c", key);
    else return buf + sprintf(buf, "\\x%02X", (unsigned int)key);
}

//
// Return the key with the given name, if one exists, otherwise -1.
// (i.e. bkeywithname("Space") == ' ', bkeywithname("x") == 'x', bkeywithname("F1") == KEY_F1, bkeywithname("???") ==
// -1)
//
int bkeywithname(const char *name) {
    int modifiers = 0;
    static const struct {
        const char *prefix;
        int modifier;
    } modnames[] = {{"Super-", MOD_META}, {"Ctrl-", MOD_CTRL}, {"Alt-", MOD_ALT}, {"Shift-", MOD_SHIFT}};
check_names:
    for (size_t i = 0; i < sizeof(key_names) / sizeof(key_names[0]); i++) {
        if (strcmp(key_names[i].name, name) == 0) return modifiers | key_names[i].key;
    }
    for (size_t i = 0; i < sizeof(modnames) / sizeof(modnames[0]); i++) {
        if (strncmp(name, modnames[i].prefix, strlen(modnames[i].prefix)) == 0) {
            modifiers |= modnames[i].modifier;
            name += strlen(modnames[i].prefix);
            goto check_names;
        }
    }
    return strlen(name) == 1 ? name[0] : -1;
}
// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1,\:0
