/*
 * Bruce's Browser (bb)
 * Copyright 2019 Bruce Hill
 * Released under the MIT license
 */
#include <dirent.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dir.h>
#include <sys/errno.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "keys.h"

#define MAX(a,b) ((a) < (b) ? (b) : (a))
#define MIN(a,b) ((a) > (b) ? (b) : (a))
#define MAX_PATH 4096
#define writez(fd, str) write(fd, str, strlen(str))
#define IS_SELECTED(p) ((p)->atme)

static const int SCROLLOFF = 5;

static struct termios orig_termios;
static int termfd;
static int width, height;
static int mouse_x, mouse_y;

typedef enum {
    SORT_ALPHA = 0,
    SORT_SIZE,
    SORT_BITS,
    SORT_DATE
} sortmethod_t;

typedef struct entry_s {
    struct entry_s *next, **atme;
    int visible : 1, d_isdir : 1;
    ino_t      d_ino;
    __uint16_t d_reclen;
    __uint8_t  d_type;
    __uint8_t  d_namlen;
    char *d_name;
    char d_fullname[0];
} entry_t;

typedef struct {
    char *path;
    entry_t *firstselected, **files;
    size_t nselected, nfiles;
    int scroll, cursor;
    struct timespec lastclick;
    int showhidden, sort_reverse;
    sortmethod_t sortmethod;
} bb_state_t;

static void err(const char *msg, ...);

static void update_term_size(void)
{
    struct winsize sz = {0};
    ioctl(termfd, TIOCGWINSZ, &sz);
    width = sz.ws_col;
    height = sz.ws_row;
}

static void init_term()
{
    termfd = open("/dev/tty", O_RDWR);
    tcgetattr(termfd, &orig_termios);
    struct termios tios;
    memcpy(&tios, &orig_termios, sizeof(tios));
    tios.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP
                           | INLCR | IGNCR | ICRNL | IXON);
    tios.c_oflag &= ~OPOST;
    tios.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    tios.c_cflag &= ~(CSIZE | PARENB);
    tios.c_cflag |= CS8;
    tios.c_cc[VMIN] = 0;
    tios.c_cc[VTIME] = 0;
    tcsetattr(termfd, TCSAFLUSH, &tios);
    // xterm-specific:
    writez(termfd, "\e[?1049h");
    update_term_size();
    // Initiate mouse tracking:
    writez(termfd, "\e[?1000h\e[?1002h\e[?1015h\e[?1006h");
    // hide cursor
    writez(termfd, "\e[?25l");
}

static void close_term()
{
    // xterm-specific:
    writez(termfd, "\e[?1049l");
    // Show cursor:
    writez(termfd, "\e[?25h");
    tcsetattr(termfd, TCSAFLUSH, &orig_termios);
    close(termfd);
}

static void err(const char *msg, ...)
{
    close_term();
    va_list args;
    va_start(args, msg);
    int len = fprintf(stderr, msg, args);
    va_end(args);
    if (errno)
        fprintf(stderr, "\n%s", strerror(errno));
    fprintf(stderr, "\n");
    _exit(1);
}

static pid_t run_cmd(int *child_out, int *child_in, const char *cmd, ...)
{
    int child_outfds[2], child_infds[2];
    pid_t child;
    if (child_out)
        pipe(child_outfds);
    if (child_in)
        pipe(child_infds);
    if ((child = fork())) {
        if (child == -1)
            err("Failed to fork");
        if (child_out) {
            *child_out = child_outfds[0];
            close(child_outfds[1]);
        }
        if (child_in) {
            *child_in = child_infds[1];
            close(child_infds[0]);
        }
    } else {
        if (child_out) {
            dup2(child_outfds[1], STDOUT_FILENO);
            close(child_outfds[0]);
        }
        if (child_in) {
            dup2(child_infds[0], STDIN_FILENO);
            close(child_infds[1]);
        }
        char *formatted_cmd;
        va_list args;
        va_start(args, cmd);
        int len = vasprintf(&formatted_cmd, cmd, args);
        va_end(args);
        if (formatted_cmd)
            execlp("sh", "sh", "-c", formatted_cmd);
        err("Failed to execute command %d: '%s'", len, formatted_cmd);
        _exit(0);
    }
    return child;
}

static void term_move(int x, int y)
{
    static char buf[32] = {0};
    int len = snprintf(buf, sizeof(buf), "\e[%d;%dH", y+1, x+1);
    if (len > 0)
        write(termfd, buf, len);
}

static void render(bb_state_t *state)
{
    writez(termfd, "\e[2J\e[0;1m"); // Clear, reset color + bold
    term_move(0,0);
    writez(termfd, state->path);

    term_move(0,1);
    { // Column labels
        char buf[] = "\e[32m    Size         Date           Bits  Name\e[0m";
        buf[8] = state->sortmethod == SORT_SIZE ? (state->sort_reverse ? '-' : '+') : ' ';
        buf[21] = state->sortmethod == SORT_DATE ? (state->sort_reverse ? '-' : '+') : ' ';
        buf[36] = state->sortmethod == SORT_BITS ? (state->sort_reverse ? '-' : '+') : ' ';
        buf[42] = state->sortmethod == SORT_ALPHA ? (state->sort_reverse ? '-' : '+') : ' ';
        writez(termfd, buf);
    }

    entry_t **files = state->files;
    for (int i = state->scroll; i < state->scroll + height - 3 && i < state->nfiles; i++) {
        entry_t *entry = files[i];
        struct stat info = {0};
        lstat(entry->d_fullname, &info);

        int x = 0;
        int y = i - state->scroll + 2;
        term_move(x, y);

        { // Selection box:
            if (IS_SELECTED(entry))
                writez(termfd, "\e[43m \e[0m");
            else
                writez(termfd, " ");

            if (i == state->cursor)
                writez(termfd, "\e[30;47m");
            else if (entry->d_isdir && entry->d_type == DT_LNK)
                writez(termfd, "\e[36m");
            else if (entry->d_isdir)
                writez(termfd, "\e[34m");
            else if (entry->d_type == DT_LNK)
                writez(termfd, "\e[33m");
        }

        { // Filesize:
            int j = 0;
            const char* units = "BKMGTPEZY";
            double bytes = (double)info.st_size;
            while (bytes > 1024) {
                bytes /= 1024;
                j++;
            }
            char buf[16] = {0};
            sprintf(buf, "%6.*f%c │", j > 0 ? 1 : 0, bytes, units[j]);
            writez(termfd, buf);
        }

        { // Date:
            char buf[64];
            strftime(buf, sizeof(buf), "%l:%M%p %b %e %Y", localtime(&(info.st_mtime)));
            writez(termfd, buf);
            //writez(termfd, "  ");
            writez(termfd, " │ ");
        }

        { // Permissions:
            char buf[] = {
                '0' + ((info.st_mode >> 6) & 7),
                '0' + ((info.st_mode >> 3) & 7),
                '0' + ((info.st_mode >> 0) & 7),
            };
            write(termfd, buf, 5);
            writez(termfd, " │ ");
        }

        { // Name:
            write(termfd, entry->d_name, entry->d_namlen);
            if (entry->d_isdir)
                writez(termfd, "/");

            if (entry->d_type == DT_LNK) {
                char linkpath[MAX_PATH+1] = {0};
                ssize_t pathlen;
                if ((pathlen = readlink(entry->d_name, linkpath, sizeof(linkpath))) < 0)
                    err("readlink() failed");
                //writez(termfd, "\e[36m -> "); // Cyan FG
                writez(termfd, " -> ");
                write(termfd, linkpath, pathlen);
                if (entry->d_isdir)
                    writez(termfd, "/");
            }
        }
        writez(termfd, " \e[0m"); // Reset color and attributes
    }

    term_move(0, height - 1);
    char buf[32] = {0};
    int len = snprintf(buf, sizeof(buf), "%lu selected", state->nselected);
    write(termfd, buf, len);
}

static int compare_alpha(void *r, const void *v1, const void *v2)
{
    int sign = *((int *)r) ? -1 : 1;
    const entry_t *f1 = *((const entry_t**)v1), *f2 = *((const entry_t**)v2);
    int diff = -(f1->d_isdir - f2->d_isdir);
    if (diff) return -diff; // Always sort dirs before files
    const char *p1 = f1->d_name, *p2 = f2->d_name;
    while (*p1 && *p2) {
        char c1 = *p1, c2 = *p2;
        if ('A' <= c1 && 'Z' <= c1) c1 = c1 - 'A' + 'a';
        if ('A' <= c2 && 'Z' <= c2) c2 = c2 - 'A' + 'a';
        int diff = (c1 - c2);
        if ('0' <= c1 && c1 <= '9' && '0' <= c2 && c2 <= '9') {
            long n1 = strtol(p1, (char**)&p1, 10);
            long n2 = strtol(p2, (char**)&p2, 10);
            diff = ((p1 - f1->d_name) - (p2 - f2->d_name)) || (n1 - n2);
            if (diff) return diff*sign;
        } else if (diff) {
            return diff*sign;
        } else {
            ++p1, ++p2;
        }
    }
    return (*p1 - *p2)*sign;
}

static int compare_bits(void *r, const void *v1, const void *v2)
{
    int sign = *((int *)r) ? -1 : 1;
    const entry_t *f1 = *((const entry_t**)v1), *f2 = *((const entry_t**)v2);
    int diff = -(f1->d_isdir - f2->d_isdir);
    if (diff) return -diff; // Always sort dirs before files
    struct stat info1, info2;
    lstat(f1->d_fullname, &info1);
    lstat(f2->d_fullname, &info2);
    return -((info1.st_mode & 0x3FF) - (info2.st_mode & 0x3FF))*sign;
}

static int compare_size(void *r, const void *v1, const void *v2)
{
    int sign = *((int *)r) ? -1 : 1;
    const entry_t *f1 = *((const entry_t**)v1), *f2 = *((const entry_t**)v2);
    int diff = -(f1->d_isdir - f2->d_isdir);
    if (diff) return -diff; // Always sort dirs before files
    struct stat info1, info2;
    lstat(f1->d_fullname, &info1);
    lstat(f2->d_fullname, &info2);
    return -(info1.st_size - info2.st_size)*sign;
}

static int compare_date(void *r, const void *v1, const void *v2)
{
    int sign = *((int *)r) ? -1 : 1;
    const entry_t *f1 = *((const entry_t**)v1), *f2 = *((const entry_t**)v2);
    int diff = -(f1->d_isdir - f2->d_isdir);
    if (diff) return -diff; // Always sort dirs before files
    struct stat info1, info2;
    lstat(f1->d_fullname, &info1);
    lstat(f2->d_fullname, &info2);
    if (info1.st_mtimespec.tv_sec == info2.st_mtimespec.tv_sec)
        return -(info1.st_mtimespec.tv_nsec - info2.st_mtimespec.tv_nsec)*sign;
    else
        return -(info1.st_mtimespec.tv_sec - info2.st_mtimespec.tv_sec)*sign;
}

static void write_selection(int fd, entry_t *firstselected)
{
    while (firstselected) {
        const char *p = firstselected->d_fullname;
        while (*p) {
            const char *p2 = strchr(p, '\n');
            if (!p2) p2 = p + strlen(p);
            write(fd, p, p2 - p);
            if (*p2 == '\n')
                write(fd, "\\", 1);
            p = p2;
        }
        write(fd, "\n", 1);
        firstselected = firstselected->next;
    }
}

static int term_get_event()
{
    char c;
    if (read(termfd, &c, 1) != 1)
        return -1;

    if (c != '\x1b')
        return c;

    // Actual escape key:
    if (read(termfd, &c, 1) != 1)
        return KEY_ESC;

    switch (c) {
        case '[':
            if (read(termfd, &c, 1) != 1)
                return -1;
            switch (c) {
                case 'H': return KEY_HOME;
                case 'F': return KEY_END;
                case '<': // Mouse clicks
                    {
                        int buttons = 0, x = 0, y = 0;
                        char buf;
                        while (read(termfd, &buf, 1) == 1 && '0' <= buf && buf <= '9')
                            buttons = buttons * 10 + (buf - '0');
                        if (buf != ';') return -1;
                        while (read(termfd, &buf, 1) == 1 && '0' <= buf && buf <= '9')
                            x = x * 10 + (buf - '0');
                        if (buf != ';') return -1;
                        while (read(termfd, &buf, 1) == 1 && '0' <= buf && buf <= '9')
                            y = y * 10 + (buf - '0');
                        if (buf != 'm' && buf != 'M') return -1;

                        mouse_x = x - 1, mouse_y = y - 1;

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
                    }
                    break;
                default:
                    break;
            }
            return -1;
        default:
            return -1;
    }
    return -1;
}

static char *input(const char *prompt, const char *starter)
{
    size_t len = 0, capacity = MAX(100, starter ? strlen(starter)+1 : 0);
    char *reply = calloc(capacity, 1);
    if (!reply) err("allocation failure");
    if (starter)
        len = strcpy(reply, starter) - reply;

    // Show cursor:
    writez(termfd, "\e[?25h");

    while (1) {
      redraw:
        term_move(0, height-1);
        writez(termfd, "\e[K\e[33m");
        writez(termfd, prompt);
        writez(termfd, "\e[0m");
        write(termfd, reply, len);

      skip_redraw:;
        int c = term_get_event();
        switch (c) {
            case KEY_BACKSPACE: case KEY_BACKSPACE2:
                if (len > 0) {
                    reply[--len] = '\0';
                    goto redraw;
                }
                goto skip_redraw;
            case KEY_CTRL_U:
                if (len > 0) {
                    len = 0;
                    reply[0] = '\0';
                    goto redraw;
                }
                goto skip_redraw;
            case KEY_CTRL_C: case KEY_ESC:
                free(reply);
                reply = NULL;
                goto done;
            case '\r':
                goto done;
            default:
                if (' ' <= c && c <= '~') {
                    if (len + 1 >= capacity) {
                        capacity += 100;
                        reply = realloc(reply, capacity);
                        if (!reply)
                            err("allocation failure");
                    }
                    reply[len++] = c;
                    reply[len] = '\0';
                    goto redraw;
                }
                goto skip_redraw;
        }
    }
  done:
    // Hide cursor:
    writez(termfd, "\e[?25l");
    return reply;
}

static void explore(char *path, int print_dir, int print_selection)
{
    char *tmp = path;
    path = malloc(strlen(tmp) + 1);
    strcpy(path, tmp);
    tmp = NULL;
    char to_select[MAX_PATH+1] = {0};
    bb_state_t state = {0};
    memset(&state, 0, sizeof(bb_state_t));

  tail_call:

    if (state.files) {
        for (int i = 0; i < state.nfiles; i++) {
            entry_t *e = state.files[i];
            e->visible = 0;
            if (!IS_SELECTED(e))
                free(e);
        }
        free(state.files);
        state.files = NULL;
    }
    state.nfiles = 0;
    state.path = path;

    { // Populate the file list:
        // Hash inode -> entry_t with linear probing
        size_t hashsize = 2 * state.nselected;
        entry_t **selecthash = calloc(hashsize, sizeof(entry_t*));
        if (!selecthash)
            err("Failed to allocate %d spaces for selecthash", hashsize);
        for (entry_t *p = state.firstselected; p; p = p->next) {
            int probe = ((int)p->d_ino) % hashsize;
            while (selecthash[probe])
                probe = (probe + 1) % hashsize;
            selecthash[probe] = p;
        }

        DIR *dir = opendir(state.path);
        if (!dir)
            err("Couldn't open dir: %s", state.path);
        if (chdir(state.path) != 0)
            err("Couldn't chdir into %s", state.path);
        struct dirent *dp;
        size_t pathlen = strlen(state.path);
        size_t filecap = 0;
        while ((dp = readdir(dir)) != NULL) {
            if (dp->d_name[0] == '.' && dp->d_name[1] == '\0')
                continue;
            if (!state.showhidden && dp->d_name[0] == '.' && !(dp->d_name[1] == '.' && dp->d_name[2] == '\0'))
                continue;
            // Hashed lookup from selected:
            if (state.nselected > 0) {
                for (int probe = ((int)dp->d_ino) % hashsize; selecthash[probe]; probe = (probe + 1) % hashsize) {
                    if (selecthash[probe]->d_ino == dp->d_ino) {
                        selecthash[probe]->visible = 1;
                        state.files[state.nfiles++] = selecthash[probe];
                        goto next_file;
                    }
                }
            }
            entry_t *entry = malloc(sizeof(entry_t) + pathlen + dp->d_namlen + 2);
            strncpy(entry->d_fullname, state.path, pathlen);
            entry->d_fullname[pathlen] = '/';
            entry->d_name = &entry->d_fullname[pathlen + 1];
            strncpy(entry->d_name, dp->d_name, dp->d_namlen + 1);
            entry->d_ino = dp->d_ino;
            entry->d_reclen = dp->d_reclen;
            entry->d_type = dp->d_type;
            entry->d_isdir = dp->d_type == DT_DIR;
            entry->visible = 1;
            if (!entry->d_isdir && entry->d_type == DT_LNK) {
                struct stat statbuf;
                if (stat(entry->d_fullname, &statbuf) == 0)
                    entry->d_isdir = S_ISDIR(statbuf.st_mode);
            }
            entry->d_namlen = dp->d_namlen;
            entry->next = NULL, entry->atme = NULL;

            if (state.nfiles >= filecap) {
                filecap += 100;
                state.files = realloc(state.files, filecap*sizeof(entry_t*));
                if (!state.files) err("allocation failure");
            }
            state.files[state.nfiles++] = entry;
          next_file:;
        }
        closedir(dir);
        free(selecthash);
        if (state.nfiles == 0) err("No files found (not even '..')");
    }

    state.cursor = 0;
    state.scroll = 0;
    clock_gettime(CLOCK_MONOTONIC, &state.lastclick);

    int (*cmp)(void*, const void*, const void*);

  sort_files:
    cmp = compare_alpha;
    if (state.sortmethod == SORT_SIZE) cmp = compare_size;
    if (state.sortmethod == SORT_DATE) cmp = compare_date;
    if (state.sortmethod == SORT_BITS) cmp = compare_bits;
    qsort_r(&state.files[1], state.nfiles-1, sizeof(entry_t*), &state.sort_reverse, cmp);

    if (to_select[0]) {
        for (int i = 0; i < state.nfiles; i++) {
            if (strcmp(to_select, state.files[i]->d_name) == 0) {
                state.cursor = i;
                if (state.nfiles > height - 4)
                    state.scroll = MAX(0, i - MIN(SCROLLOFF, (height-4)/2));
                break;
            }
        }
        to_select[0] = '\0';
    }

    int picked, scrolloff;
    while (1) {
      redraw:
        render(&state);
      skip_redraw:
        scrolloff = MIN(SCROLLOFF, (height-4)/2);
        int key = term_get_event();
        switch (key) {
            case KEY_MOUSE_LEFT: {
                struct timespec clicktime;
                clock_gettime(CLOCK_MONOTONIC, &clicktime);
                double dt_ms = 1e3*(double)(clicktime.tv_sec - state.lastclick.tv_sec);
                dt_ms += 1e-6*(double)(clicktime.tv_nsec - state.lastclick.tv_nsec);
                state.lastclick = clicktime;
                if (mouse_y == 1) {
                    //    Size         Date           Bits  Name
                    if (mouse_x <= 8)
                        goto sort_size;
                    else if (mouse_x <= 30)
                        goto sort_date;
                    else if (mouse_x <= 35)
                        goto sort_bits;
                    else
                        goto sort_alpha;
                } else if (mouse_y >= 2 && state.scroll + (mouse_y - 2) < state.nfiles) {
                    int clicked = state.scroll + (mouse_y - 2);
                    if (dt_ms > 200) {
                    // Single click
                        if (mouse_x == 0) {
                            // Toggle
                            picked = clicked;
                            goto toggle;
                        } else {
                            state.cursor = clicked;
                            goto redraw;
                        }
                    } else {
                        // Double click
                        picked = clicked;
                        goto open_file;
                    }
                }
                break;
            }

            case 'q': case 'Q': case KEY_CTRL_C:
                goto done;

            case KEY_MOUSE_WHEEL_DOWN:
                if (state.cursor >= state.nfiles - 1)
                    goto skip_redraw;
                ++state.cursor;
                if (state.nfiles <= height - 4)
                    goto redraw;
                ++state.scroll;
                goto redraw;

            case KEY_MOUSE_WHEEL_UP:
                if (state.cursor <= 0)
                    goto skip_redraw;
                --state.cursor;
                if (state.nfiles <= height - 4)
                    goto redraw;
                --state.scroll;
                if (state.scroll < 0)
                    state.scroll = 0;
                goto redraw;

            case KEY_CTRL_D:
                if (state.cursor == state.nfiles - 1)
                    goto skip_redraw;
                state.cursor = MIN(state.nfiles - 1, state.cursor + (height - 4) / 2);
                if (state.nfiles <= height - 4)
                    goto redraw;
                state.scroll += (height - 4)/2;
                if (state.scroll > state.nfiles - (height - 4))
                    state.scroll = state.nfiles - (height - 4);
                goto redraw;

            case KEY_CTRL_U:
                state.cursor = MAX(0, state.cursor - (height - 4) / 2);
                if (state.nfiles <= height - 4)
                    goto redraw;
                state.scroll -= (height - 4)/2;
                if (state.scroll < 0)
                    state.scroll = 0;
                goto redraw;

            case 'g':
                state.cursor = 0;
                state.scroll = 0;
                goto redraw;

            case 'G':
                state.cursor = state.nfiles - 1;
                if (state.nfiles > height - 4)
                    state.scroll = state.nfiles - (height - 4);
                goto redraw;

            case ' ':
                picked = state.cursor;
              toggle:
                if (strcmp(state.files[picked]->d_name, "..") == 0)
                    goto skip_redraw;
                if (IS_SELECTED(state.files[picked])) {
                  toggle_off:;
                    entry_t *e = state.files[picked];
                    if (e->next) e->next->atme = e->atme;
                    *(e->atme) = e->next;
                    e->next = NULL, e->atme = NULL;
                    --state.nselected;
                } else {
                  toggle_on:;
                    entry_t *e = state.files[picked];
                    if (state.firstselected)
                        state.firstselected->atme = &e->next;
                    e->next = state.firstselected;
                    e->atme = &state.firstselected;
                    state.firstselected = e;
                    ++state.nselected;
                }
                goto redraw;

            case KEY_CTRL_A:
                for (int i = 0; i < state.nfiles; i++) {
                    entry_t *e = state.files[i];
                    if (e->atme) continue;
                    if (strcmp(e->d_name, "..") == 0)
                        continue;
                    if (state.firstselected)
                        state.firstselected->atme = &e->next;
                    e->next = state.firstselected;
                    e->atme = &state.firstselected;
                    state.firstselected = e;
                    ++state.nselected;
                }
                goto redraw;

            case KEY_ESC: {
                entry_t **tofree = calloc(state.nselected, sizeof(entry_t*));
                int i = 0;
                for (entry_t *e = state.firstselected; e; e = e->next) {
                    if (!e->visible) tofree[i++] = e;
                    *e->atme = NULL;
                    e->atme = NULL;
                }
                if (state.firstselected) err("???");
                while (i) free(tofree[--i]);
                free(tofree);
                state.nselected = 0;
                goto redraw;
            }

            case 'j':
                if (state.cursor >= state.nfiles - 1)
                    goto skip_redraw;
                ++state.cursor;
                if (state.cursor > state.scroll + height - 4 - 1 - scrolloff && state.scroll < state.nfiles - (height - 4)) {
                    ++state.scroll;
                }
                goto redraw;

            case 'k':
                if (state.cursor <= 0)
                    goto skip_redraw;
                --state.cursor;
                if (state.cursor < state.scroll + scrolloff && state.scroll > 0) {
                    --state.scroll;
                }
                goto redraw;

            case 'J':
                if (state.cursor < state.nfiles - 1) {
                    if (IS_SELECTED(state.files[state.cursor])) {
                        picked = ++state.cursor;
                        if (!IS_SELECTED(state.files[picked]))
                            goto toggle_on;
                    } else {
                        picked = ++state.cursor;
                        if (IS_SELECTED(state.files[picked]))
                            goto toggle_off;
                    }
                    goto redraw;
                }
                goto skip_redraw;

            case 'K':
                if (state.cursor > 0) {
                    if (IS_SELECTED(state.files[state.cursor])) {
                        picked = --state.cursor;
                        if (!IS_SELECTED(state.files[picked]))
                            goto toggle_on;
                    } else {
                        picked = --state.cursor;
                        if (IS_SELECTED(state.files[picked]))
                            goto toggle_off;
                    }
                    goto redraw;
                }
                goto skip_redraw;

            case 'h':
                picked = 0;
                goto open_file;

            case 'a':
              sort_alpha:
                if (state.sortmethod == SORT_ALPHA)
                    state.sort_reverse ^= 1;
                else {
                    state.sortmethod = SORT_ALPHA;
                    state.sort_reverse = 0;
                }
                goto sort_files;

            case 'b':
              sort_bits:
                if (state.sortmethod == SORT_BITS)
                    state.sort_reverse ^= 1;
                else {
                    state.sortmethod = SORT_BITS;
                    state.sort_reverse = 0;
                }
                goto sort_files;

            case 's':
              sort_size:
                if (state.sortmethod == SORT_SIZE)
                    state.sort_reverse ^= 1;
                else {
                    state.sortmethod = SORT_SIZE;
                    state.sort_reverse = 0;
                }
                goto sort_files;

            case 'd':
              sort_date:
                if (state.sortmethod == SORT_DATE)
                    state.sort_reverse ^= 1;
                else {
                    state.sortmethod = SORT_DATE;
                    state.sort_reverse = 0;
                }
                goto sort_files;

            case '.':
                state.showhidden ^= 1;
                goto tail_call;

            case 'l': case '\r':
                picked = state.cursor;
              open_file: {
                if (state.files[picked]->d_isdir) {
                    if (strcmp(state.files[picked]->d_name, "..") == 0) {
                        char *p = strrchr(state.path, '/');
                        if (p) strcpy(to_select, p+1);
                        else to_select[0] = '\0';
                    } else to_select[0] = '\0';

                    char tmp[MAX_PATH+1];
                    if (!realpath(state.files[picked]->d_fullname, tmp))
                        err("realpath failed");
                    free(path);
                    path = calloc(strlen(tmp) + 1, sizeof(char));
                    strcpy(path, tmp);
                    goto tail_call;
                } else {
                    char *name = state.files[picked]->d_name;
                    close_term();
                    pid_t child = run_cmd(NULL, NULL,
#ifdef __APPLE__
                            "if file -bI %s | grep '^text/' >/dev/null; then $EDITOR %s; else open %s; fi",
#else
                            "if file -bi %s | grep '^text/' >/dev/null; then $EDITOR %s; else xdg-open %s; fi",
#endif
                            name, name, name);
                    waitpid(child, NULL, 0);
                    init_term();
                    goto redraw;
                }
                break;
            }

            case 'm':
                if (state.nselected) {
                    int fd;
                    run_cmd(NULL, &fd, "xargs -I {} mv {} .");
                    write_selection(fd, state.firstselected);
                    close(fd);
                }
                break;

            case 'D': {
                int fd;
                run_cmd(NULL, &fd, "xargs rm -rf");
                if (state.nselected > 0) {
                    write_selection(fd, state.firstselected);
                    for (entry_t *e = state.firstselected; e; e = e->next) {
                        e->next = NULL;
                        *(e->atme) = NULL;
                        e->atme = NULL;
                        if (!e->visible) free(e);
                    }
                    state.nselected = 0;
                } else {
                    writez(fd, state.files[state.cursor]->d_fullname);
                    write(fd, "\n", 1);
                }
                close(fd);
                goto tail_call;
            }

            case 'p':
                if (state.nselected) {
                    int fd;
                    run_cmd(NULL, &fd, "xargs -I {} cp {} .");
                    write_selection(fd, state.firstselected);
                    close(fd);
                } else if (strcmp(state.files[state.cursor]->d_name, "..") != 0) {
                    run_cmd(NULL, NULL, "cp %s %s.copy", state.files[state.cursor]->d_name);
                }
                goto tail_call;

            case 'n': {
                char *name = input("new file: ", "foo");
                if (!name) goto redraw;
                run_cmd(NULL, NULL, "touch %s", name);
                goto tail_call;
            }

            case 'f': case '/': {
                close_term();
                int fd;
                if (state.showhidden)
                    run_cmd(&fd, NULL, "find | cut -d/ -f2- | fzf");
                else
                    run_cmd(&fd, NULL, "find -not -name '\\.*' -not -path '*/\\.*' | cut -d/ -f2- | fzf");
                int len = 0, space = MAX_PATH, consumed;
                while ((consumed = read(fd, &to_select[len], space)) > 0) {
                    if (consumed < 0) err("Error reading selection");
                    to_select[len + consumed] = '\0';
                    char *nl = strchr(&to_select[len], '\n');
                    if (nl) {
                        *nl = '\0';
                        break;
                    }
                    len += consumed;
                    space -= consumed;
                }
                close(fd);
                init_term();

                if (to_select[0] == '\0')
                    goto redraw;

                char *pathend = strrchr(to_select, '/');
                if (pathend) {
                    char *newpath = calloc(strlen(path) + 1 + (pathend - to_select), 1);
                    strcpy(newpath, path);
                    strcat(newpath, "/");
                    strncat(newpath, to_select, pathend - to_select);
                    free(path);
                    path = newpath;
                    strcpy(to_select, pathend + 1);
                }
                goto tail_call;
            }

            case '|': {
                char *cmd = input("> ", NULL);
                if (!cmd)
                    goto redraw;
                // TODO: avoid having this spam the terminal history
                close_term();
                int fd;
                pid_t child = run_cmd(NULL, &fd, cmd);
                free(cmd);
                if (state.nselected > 0) {
                    write_selection(fd, state.firstselected);
                } else {
                    write(fd, state.files[state.cursor]->d_name, state.files[state.cursor]->d_namlen);
                    write(fd, "\n", 1);
                }
                close(fd);
                waitpid(child, NULL, 0);

                printf("\npress any key to continue...\n");
                fflush(stdout);
                getchar();

                init_term();
                goto tail_call;
            }

            default:
                goto skip_redraw;
        }
        goto skip_redraw;
    }
done:
    close_term();
    if (print_dir)
        printf("%s\n", state.path);
    if (print_selection)
        write_selection(STDOUT_FILENO, state.firstselected);
    return;
}

int main(int argc, char *argv[])
{
    init_term();
    char _realpath[MAX_PATH+1];
    char *path = ".";
    int print_dir = 0, print_selection = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0) {
            print_dir = 1;
        } else if (strcmp(argv[i], "-s") == 0) {
            print_selection = 1;
        } else {
            path = argv[i];
            break;
        }
    }
    if (!realpath(path, _realpath))
        err("realpath failed");
    explore(_realpath, print_dir, print_selection);
done:
    return 0;
}
