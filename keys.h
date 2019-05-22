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


int term_getkey(int fd, int *mouse_x, int *mouse_y)
{
    char c;
    if (read(fd, &c, 1) != 1)
        return -1;

    if (c == '\x1b')
        goto escape;

    return c;

  escape:
    // Actual escape key:
    if (read(fd, &c, 1) != 1)
        return KEY_ESC;

    switch (c) {
        case '[': goto CSI;
        default: return -1;
    }

  CSI:
    if (read(fd, &c, 1) != 1)
        return -1;

    switch (c) {
        case 'H': return KEY_HOME;
        case 'F': return KEY_END;
        case 'A': return KEY_ARROW_UP;
        case 'B': return KEY_ARROW_DOWN;
        case 'C': return KEY_ARROW_RIGHT;
        case 'D': return KEY_ARROW_LEFT;
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

            *mouse_x = x - 1, *mouse_y = y - 1;

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
    }

    return -1;
}
