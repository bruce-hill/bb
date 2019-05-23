/*
 * Bitty Browser (bb)
 * Copyright 2019 Bruce Hill
 * Released under the MIT license
 */
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
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

#include "config.h"
#include "keys.h"

#define MAX(a,b) ((a) < (b) ? (b) : (a))
#define MIN(a,b) ((a) > (b) ? (b) : (a))
#define MAX_PATH 4096
#define startswith(str, start) (strncmp(str, start, strlen(start)) == 0)
#define writez(fd, str) write(fd, str, strlen(str))
#define IS_SELECTED(p) ((p)->atme)

#define KEY_DELAY 50

static struct termios orig_termios;
static int termfd;
static int width, height;
static int mouse_x, mouse_y;

typedef enum {
    SORT_ALPHA = 0,
    SORT_SIZE,
    SORT_BITS,
    SORT_TIME
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

#define err(...) do { \
    close_term(); \
    fprintf(stderr, __VA_ARGS__); \
    _err(); \
} while (0)
static void _err();

static void update_term_size(void)
{
    struct winsize sz = {0};
    ioctl(termfd, TIOCGWINSZ, &sz);
    width = sz.ws_col;
    height = sz.ws_row;
}

static inline int clamped(int x, int low, int high)
{
    if (x < low) return low;
    if (x > high) return high;
    return x;
}

static void do_nothing(int _)
{
    // Used as SIGINT handler
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
    // Set the scrolling region
    char buf[16];
    sprintf(buf, "\e[3;%dr", height-1);
    writez(termfd, buf);
}

static void close_term()
{
    // xterm-specific:
    writez(termfd, "\e[?1049l");
    // Show cursor:
    writez(termfd, "\e[?25h");
    // Restore scrollable region
    char buf[16];
    sprintf(buf, "\e[1;%dr", height);
    writez(termfd, buf);

    tcsetattr(termfd, TCSAFLUSH, &orig_termios);
    close(termfd);
}

static void _err()
{
    if (errno)
        fprintf(stderr, "\n%s", strerror(errno));
    fprintf(stderr, "\n");
    exit(1);
}

static char bb_tmpfile[] = "/tmp/bb.XXXXXX";

static int run_cmd_on_selection(bb_state_t *state, const char *cmd)
{
    pid_t child;
    sig_t old_handler = signal(SIGINT, do_nothing);

    strcpy(bb_tmpfile, "/tmp/bb.XXXXXX");
    if (!mktemp(bb_tmpfile))
        err("Could not create temp file");

    if ((child = fork()) == 0) {
        // TODO: is there a max number of args? Should this be batched?
        char **const args = calloc(MAX(1, state->nselected) + 5, sizeof(char*));
        int i = 0;
        args[i++] = "sh";
        args[i++] = "-c";
        args[i++] = (char*)cmd;
        args[i++] = "--";
        entry_t *first = state->firstselected ? state->firstselected : state->files[state->cursor];
        for (entry_t *e = first; e; e = e->next) {
            args[i++] = e->d_name;
        }
        args[i] = NULL;

        char bb_depth_str[64] = {0}, bb_ipc_str[64] = {0};
        { // Set environment variable to track shell nesting
            char *depthstr = getenv("BB_DEPTH");
            int depth = depthstr ? atoi(depthstr) : 0;
            snprintf(bb_depth_str, sizeof(bb_depth_str), "%d", depth + 1);
            setenv("BB_DEPTH", bb_depth_str, 1);
            setenv("BBCMD", bb_tmpfile, 1);
            setenv("BBCURSOR", state->files[state->cursor]->d_name, 1);
            setenv("BBFULLCURSOR", state->files[state->cursor]->d_fullname, 1);
        }

        execvp("sh", args);
        err("Failed to execute command: '%s'", cmd);
        return -1;
    }

    if (child == -1)
        err("Failed to fork");

    int status;
    waitpid(child, &status, 0);
    signal(SIGINT, old_handler);
    return status;
}

static void term_move(int x, int y)
{
    static char buf[32] = {0};
    int len = snprintf(buf, sizeof(buf), "\e[%d;%dH", y+1, x+1);
    if (len > 0)
        write(termfd, buf, len);
}

static int write_escaped(int fd, const char *str, size_t n, const char *reset_color)
{
    // escapes['\n'] == 'n', etc.
    static const char *escapes = "       abtnvfr             e";
    char buf[5];
    int ret = 0;
    int backlog = 0;
    for (int i = 0; i < n; i++) {
        int escapelen = 0;
        if (str[i] <= '\x1b' && escapes[str[i]] != ' ')
            escapelen = sprintf(buf, "\\%c", escapes[str[i]]);
        else if (!(' ' <= str[i] && str[i] <= '~'))
            escapelen = sprintf(buf, "\\x%02X", str[i]);
        else {
            ++backlog;
            continue;
        }

        if (backlog > 0) {
            ret += write(fd, &str[i-backlog], backlog);
            backlog = 0;
        }
        ret += writez(fd, "\e[31m");
        ret += write(fd, buf, escapelen);
        ret += writez(fd, reset_color);
    }
    if (backlog > 0)
        ret += write(fd, &str[n-backlog], backlog);
    return ret;
}

static void render(bb_state_t *state, int lazy)
{
    static int lastcursor = -1, lastscroll = -1;

    if (!lazy) {
        term_move(0,0);
        writez(termfd, "\e[0;1m"); // reset color + bold
        write_escaped(termfd, state->path, strlen(state->path), "\e[0;1m");
        writez(termfd, "\e[K");

        term_move(0,1);
        { // Column labels
            char buf[] = "\e[32m    Size         Date           Perm  Name\e[0m";
            buf[8] = state->sortmethod == SORT_SIZE ? (state->sort_reverse ? '-' : '+') : ' ';
            buf[21] = state->sortmethod == SORT_TIME ? (state->sort_reverse ? '-' : '+') : ' ';
            buf[36] = state->sortmethod == SORT_BITS ? (state->sort_reverse ? '-' : '+') : ' ';
            buf[42] = state->sortmethod == SORT_ALPHA ? (state->sort_reverse ? '-' : '+') : ' ';
            writez(termfd, buf);
            writez(termfd, "\e[K");
        }
    }

    if (lazy) {
        char buf[16];
        if (lastscroll > state->scroll) {
            int n = sprintf(buf, "\e[%dT", lastscroll - state->scroll);
            write(termfd, buf, n);
        } else if (lastscroll < state->scroll) {
            int n = sprintf(buf, "\e[%dS", state->scroll - lastscroll);
            write(termfd, buf, n);
        }
    }

    entry_t **files = state->files;
    static const char *NORMAL_COLOR = "\e[0m";
    static const char *CURSOR_COLOR = "\e[0;30;47m";
    static const char *LINKDIR_COLOR = "\e[0;36m";
    static const char *DIR_COLOR = "\e[0;34m";
    static const char *LINK_COLOR = "\e[0;33m";
    for (int i = state->scroll; i < state->scroll + height - 3; i++) {
        if (lazy) {
            if (i == state->cursor || i == lastcursor)
                goto do_render;
            if (i < lastscroll || i >= lastscroll + height - 3)
                goto do_render;
            continue;
        }

      do_render:;
        int y = i - state->scroll + 2;
        term_move(0, y);
        if (i >= state->nfiles) {
            writez(termfd, "\e[K");
            continue;
        }

        entry_t *entry = files[i];
        struct stat info = {0};
        lstat(entry->d_fullname, &info);

        { // Selection box:
            if (IS_SELECTED(entry))
                writez(termfd, "\e[43m \e[0m");
            else
                writez(termfd, " ");
        }

        const char *color;
        if (i == state->cursor)
            color = CURSOR_COLOR;
        else if (entry->d_isdir && entry->d_type == DT_LNK)
            color = LINKDIR_COLOR;
        else if (entry->d_isdir)
            color = DIR_COLOR;
        else if (entry->d_type == DT_LNK)
            color = LINK_COLOR;
        else
            color = NORMAL_COLOR;
        writez(termfd, color);

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
            write_escaped(termfd, entry->d_name, entry->d_namlen, color);
            if (entry->d_isdir)
                writez(termfd, "/");

            if (entry->d_type == DT_LNK) {
                char linkpath[MAX_PATH+1] = {0};
                ssize_t pathlen;
                if ((pathlen = readlink(entry->d_name, linkpath, sizeof(linkpath))) < 0)
                    err("readlink() failed");
                writez(termfd, " -> ");
                write_escaped(termfd, linkpath, pathlen, color);
                if (entry->d_isdir)
                    writez(termfd, "/");
            }
        }
        writez(termfd, " \e[0m\e[K"); // Reset color and attributes
    }

    static const char *help = "Press '?' to see key bindings ";
    term_move(0, height - 1);
    if (state->nselected > 0) {
        char buf[32] = {0};
        int len = snprintf(buf, sizeof(buf), " %lu selected", state->nselected);
        if (strlen(help) + len < width + 1) {
            write(termfd, buf, len);
        }
    }
    writez(termfd, "\e[K");
    term_move(MAX(0, width - strlen(help)), height - 1);
    writez(termfd, help);
    lastcursor = state->cursor;
    lastscroll = state->scroll;
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

static int compare_perm(void *r, const void *v1, const void *v2)
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

static entry_t *find_file(bb_state_t *state, const char *name)
{
    for (int i = 0; i < state->nfiles; i++) {
        entry_t *e = state->files[i];
        if (strcmp(name[0] == '/' ? e->d_fullname : e->d_name, name) == 0)
            return e;
    }
    return NULL;
}

static void write_selection(int fd, entry_t *firstselected, char sep)
{
    while (firstselected) {
        const char *p = firstselected->d_fullname;
        while (*p) {
            const char *p2 = strchr(p, '\n');
            if (!p2) p2 = p + strlen(p);
            write(fd, p, p2 - p);
            if (*p2 == '\n' && sep == '\n')
                write(fd, "\\", 1);
            p = p2;
        }
        write(fd, &sep, 1);
        firstselected = firstselected->next;
    }
}

static void clear_selection(bb_state_t *state)
{
    entry_t **tofree = calloc(state->nselected, sizeof(entry_t*));
    int i = 0;
    for (entry_t *e = state->firstselected; e; e = e->next) {
        if (!e->visible) tofree[i++] = e;
        *e->atme = NULL;
        e->atme = NULL;
    }
    while (i) free(tofree[--i]);
    free(tofree);
    state->nselected = 0;
}

static int select_file(bb_state_t *state, entry_t *e)
{
    if (IS_SELECTED(e)) return 0;
    if (strcmp(e->d_name, "..") == 0) return 0;
    if (state->firstselected)
        state->firstselected->atme = &e->next;
    e->next = state->firstselected;
    e->atme = &state->firstselected;
    state->firstselected = e;
    ++state->nselected;
    return 1;
}

static int deselect_file(bb_state_t *state, entry_t *e)
{
    if (!IS_SELECTED(e)) return 0;
    if (e->next) e->next->atme = e->atme;
    *(e->atme) = e->next;
    e->next = NULL, e->atme = NULL;
    --state->nselected;
    return 1;
}

static void explore(char *path, int print_dir, int print_selection, char sep)
{
    char realpath_buf[MAX_PATH+1];
    path = strdup(path);
    if (!path) err("allocation failure");
    char *original_path = strdup(path);
    if (!original_path) err("allocation failure");

    char to_select[MAX_PATH+1] = {0};
    bb_state_t state = {0};
    memset(&state, 0, sizeof(bb_state_t));

  tail_call:
    if (!realpath(path, realpath_buf))
        err("realpath failed on %s", path);
    path = realloc(path, strlen(realpath_buf));
    strcpy(path, realpath_buf);

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
            err("Failed to allocate %ld spaces for selecthash", hashsize);
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
    if (state.sortmethod == SORT_TIME) cmp = compare_date;
    if (state.sortmethod == SORT_BITS) cmp = compare_perm;
    qsort_r(&state.files[1], state.nfiles-1, sizeof(entry_t*), &state.sort_reverse, cmp);

    // Put the cursor on the first *real* file if one exists
    if (state.nfiles > 1)
        ++state.cursor;

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

    int picked, scrolloff, lazy = 0;
    while (1) {
      redraw:
        render(&state, lazy);
        lazy = 0;
      skip_redraw:
        scrolloff = MIN(SCROLLOFF, (height-4)/2);
        int key = term_getkey(termfd, &mouse_x, &mouse_y, KEY_DELAY);
        switch (key) {
            case KEY_MOUSE_LEFT: {
                struct timespec clicktime;
                clock_gettime(CLOCK_MONOTONIC, &clicktime);
                double dt_ms = 1e3*(double)(clicktime.tv_sec - state.lastclick.tv_sec);
                dt_ms += 1e-6*(double)(clicktime.tv_nsec - state.lastclick.tv_nsec);
                state.lastclick = clicktime;
                if (mouse_y == 1) {
                    //    Size         Date           Perm  Name
                    if (mouse_x <= 8)
                        goto sort_size;
                    else if (mouse_x <= 30)
                        goto sort_date;
                    else if (mouse_x <= 35)
                        goto sort_perm;
                    else
                        goto sort_alpha;
                } else if (mouse_y >= 2 && state.scroll + (mouse_y - 2) < state.nfiles) {
                    int clicked = state.scroll + (mouse_y - 2);
                    if (dt_ms > 200) {
                        lazy = 1;
                        // Single click
                        if (mouse_x == 0) {
                            if (IS_SELECTED(state.files[clicked]))
                                deselect_file(&state, state.files[clicked]);
                            else
                                select_file(&state, state.files[clicked]);
                            goto redraw;
                        } else {
                            state.cursor = clicked;
                            goto redraw;
                        }
                    } else {
                        // Double click
                        // TODO: hacky
                        state.cursor = clicked;
                        key = '\n';
                        goto user_bindings;
                    }
                }
                break;
            }

            case KEY_CTRL_C:
                close_term();
                if (print_dir)
                    printf("%s\n", original_path);
                exit(1);
                return;

            case KEY_CTRL_Z:
                close_term();
                raise(SIGTSTP);
                init_term();
                goto redraw;

            case KEY_MOUSE_WHEEL_DOWN:
                if (state.cursor >= state.nfiles - 1)
                    goto skip_redraw;
                ++state.cursor;
                lazy = 1;
                if (state.nfiles > height - 4)
                    ++state.scroll;
                goto redraw;

            case KEY_MOUSE_WHEEL_UP:
                if (state.cursor <= 0)
                    goto skip_redraw;
                --state.cursor;
                lazy = 1;
                if (state.nfiles > height - 4 && state.scroll > 0)
                    --state.scroll;
                goto redraw;

            case 'J':
                if (state.cursor < state.nfiles - 1) {
                    lazy = 1;
                    if (IS_SELECTED(state.files[state.cursor]))
                        select_file(&state, state.files[++state.cursor]);
                    else
                        deselect_file(&state, state.files[++state.cursor]);
                    goto redraw;
                }
                goto skip_redraw;

            case 'K':
                if (state.cursor > 0) {
                    lazy = 1;
                    if (IS_SELECTED(state.files[state.cursor]))
                        select_file(&state, state.files[--state.cursor]);
                    else
                        deselect_file(&state, state.files[--state.cursor]);
                    goto redraw;
                }
                goto skip_redraw;

            case 's':
                // Change sorting method:
                term_move(0, height-1);
                writez(termfd, "\e[K\e[1mSort by (a)lphabetic (s)ize (t)ime (p)ermissions:\e[0m \e[?25h");
              try_sort_again:
                switch (term_getkey(termfd, &mouse_x, &mouse_y, -1)) {
                    case 'a': case 'A':
                      sort_alpha:
                        if (state.sortmethod == SORT_ALPHA)
                            state.sort_reverse ^= 1;
                        else {
                            state.sortmethod = SORT_ALPHA;
                            state.sort_reverse = 0;
                        }
                        break;

                    case 'p': case 'P':
                      sort_perm:
                        if (state.sortmethod == SORT_BITS)
                            state.sort_reverse ^= 1;
                        else {
                            state.sortmethod = SORT_BITS;
                            state.sort_reverse = 0;
                        }
                        break;

                    case 's': case 'S':
                      sort_size:
                        if (state.sortmethod == SORT_SIZE)
                            state.sort_reverse ^= 1;
                        else {
                            state.sortmethod = SORT_SIZE;
                            state.sort_reverse = 0;
                        }
                        break;

                    case 't': case 'T':
                      sort_date:
                        if (state.sortmethod == SORT_TIME)
                            state.sort_reverse ^= 1;
                        else {
                            state.sortmethod = SORT_TIME;
                            state.sort_reverse = 0;
                        }
                        break;

                    case -1:
                        goto try_sort_again;

                    default:
                        writez(termfd, "\e[?25l");
                        goto redraw;
                }
                // Hide cursor again
                writez(termfd, "\e[?25l");
                goto sort_files;

            case '.':
                state.showhidden ^= 1;
                strcpy(to_select, state.files[state.cursor]->d_name);
                goto tail_call;

            case '?': {
                char tmpname[] = "/tmp/bb_help.XXXXXX";
                int fd = mkstemp(tmpname);
                writez(fd, "\n              \e[33;1mKey Bindings:\e[0m\n");
                for (int i = 0; bindings[i].key; i++) {
                    if (bindings[i].key > 0) continue;
                    writez(fd, "\e[1m");
                    char *tab = strchr(bindings[i].command, '\t');
                    const char *spaces = "                            ";
                    write(fd, spaces, 16 - (tab - bindings[i].command));
                    write(fd, bindings[i].command, tab - bindings[i].command);
                    write(fd, " ", 1);
                    writez(fd, tab + 1);
                    writez(fd, "\e[0m\n");
                }
                writez(fd, "\n    \e[33;1mScript Key Bindings:\e[0m\n");
                for (int i = 0; bindings[i].key; i++) {
                    if (bindings[i].key <= 0) continue;
                    writez(fd, "\e[1m    ");
                    char buf[16];
                    buf[0] = bindings[i].key;
                    if (' ' <= buf[0] && buf[0] <= '~')
                        write(fd, buf, 1);
                    else {
                        sprintf(buf, "\e[31m%04X", bindings[i].key);
                        writez(fd, buf);
                    }
                    writez(fd, "    \e[0m\t");
                    write_escaped(fd, bindings[i].command, strlen(bindings[i].command), "\e[0m");
                    writez(fd, "\e[0m\n");
                }
                writez(fd, "\n");
                close(fd);

                close_term();
                char cmd[256];
                sprintf(cmd, "less -r %s", tmpname);
                system(cmd);
                unlink(tmpname);
                init_term();
                goto redraw;
            }

            case -1:
                goto skip_redraw;

            default:
                // Search user-defined key bindings from config.h:
              user_bindings:
                for (int i = 0; bindings[i].key > 0; i++) {
                    if (key == bindings[i].key) {
                        term_move(0, height-1);
                        //writez(termfd, "\e[K");

                        struct termios cur_tios;
                        if (bindings[i].flags & NORMAL_TERM) {
                            close_term();
                        } else {
                            { // Restore scrolling region
                                char buf[16];
                                sprintf(buf, "\e[1;%dr", height);
                                writez(termfd, buf);
                            }
                            /*
                            tcgetattr(termfd, &cur_tios);
                            tcsetattr(termfd, TCSAFLUSH, &orig_termios);
                            writez(termfd, "\e[?25h"); // Show cursor
                            */
                        }

                        run_cmd_on_selection(&state, bindings[i].command);

                        if (bindings[i].flags & NORMAL_TERM) {
                            lazy = 0;
                            init_term();
                        } else {
                            lazy = 1;
                            /*
                            tcsetattr(termfd, TCSAFLUSH, &cur_tios);
                            writez(termfd, "\e[?25l"); // Hide cursor
                            */
                            { // Restore scrolling region
                                char buf[16];
                                sprintf(buf, "\e[3;%dr", height-1);
                                writez(termfd, buf);
                            }
                        }

                        // Scan for IPC requests
                        int needs_full_refresh = 0;
                        char newsort[3] = "";
                        FILE *tmpfile;
                        if ((tmpfile = fopen(bb_tmpfile, "r"))) {
                            char *line = NULL;
                            size_t capacity = 0;
                            ssize_t len;
                            while ((len = getline(&line, &capacity, tmpfile)) >= 0) {
                                if (len > 0 && line[len-1] == '\n') line[--len] = '\0';
                                if (strcmp(line, "refresh") == 0) {
                                    needs_full_refresh = 1;
                                } else if (strcmp(line, "quit") == 0) {
                                    goto done;
                                } else if (startswith(line, "sort:") && len >= 7) {
                                    newsort[0] = line[strlen("sort:")];
                                    newsort[1] = line[strlen("sort:") + 1];
                                } else if (startswith(line, "cd:")) {
                                    free(path);
                                    path = calloc(strlen(line+strlen("cd:")) + 1, sizeof(char));
                                    strcpy(path, line+strlen("cd:"));
                                    goto tail_call;
                                } else if (startswith(line, "toggle:")) {
                                    lazy = 0;
                                    entry_t *e = find_file(&state, line + strlen("select:"));
                                    if (e) {
                                        if (IS_SELECTED(e)) deselect_file(&state, e);
                                        else select_file(&state, e);
                                    }
                                } else if (startswith(line, "select:")) {
                                    char *name = line + strlen("select:");
                                    lazy = 0;
                                    if (strcmp(name, "*") == 0) {
                                        for (int i = 0; i < state.nfiles; i++)
                                            select_file(&state, state.files[i]);
                                    } else {
                                        entry_t *e = find_file(&state, name);
                                        if (e) select_file(&state, e);
                                    }
                                } else if (startswith(line, "deselect:")) {
                                    char *name = line + strlen("deselect:");
                                    lazy = 0;
                                    if (strcmp(name, "*") == 0) {
                                        clear_selection(&state);
                                    } else {
                                        entry_t *e = find_file(&state, name);
                                        if (e) select_file(&state, e);
                                    }
                                } else if (startswith(line, "cursor:")) {
                                    char *name = line + strlen("cursor:");
                                    for (int i = 0; i < state.nfiles; i++) {
                                        if (strcmp(name[0] == '/' ?
                                                    state.files[i]->d_fullname : state.files[i]->d_name,
                                                    name) == 0) {
                                            state.cursor = i;
                                            goto found_it;
                                        }
                                    }
                                    free(path);
                                    char *lastslash = strrchr(name, '/');
                                    if (!lastslash) goto found_it;
                                    size_t len = lastslash - name;
                                    path = calloc(len + 1, sizeof(char));
                                    memcpy(path, name, len);
                                    strcpy(to_select, lastslash+1);
                                    goto tail_call;
                                  found_it:;
                                } else if (startswith(line, "move:")) {
                                    char *value = line + strlen("move:");
                                    int oldcur = state.cursor;
                                    int isabs = value[0] != '-' && value[0] != '+';
                                    long delta = strtol(value, &value, 10);
                                    if (*value == '%') delta = (delta * height)/100;
                                    if (isabs) state.cursor = delta;
                                    else state.cursor += delta;

                                    state.cursor = clamped(state.cursor, 0, state.nfiles-1);
                                    delta = state.cursor - oldcur;

                                    if (state.nfiles > height-4) {
                                        if (delta > 0) {
                                            if (state.cursor >= state.scroll + (height-4) - scrolloff)
                                                state.scroll += delta;
                                        } else if (delta < 0) {
                                            if (state.cursor <= state.scroll + scrolloff)
                                                state.scroll += delta;
                                        }
                                        //int target = clamped(state.scroll, state.cursor - (height-4) + scrolloff, state.cursor - scrolloff);
                                        //state.scroll += (delta > 0 ? 1 : -1)*MIN(abs(target-state.scroll), abs((int)delta));
                                        //state.scroll = target;
                                        state.scroll = clamped(state.scroll, state.cursor - (height-4) + 1, state.cursor);
                                        state.scroll = clamped(state.scroll, 0, state.nfiles-1 - (height-4));
                                    }
                                } else if (startswith(line, "scroll:")) {
                                    char *value = line + strlen("scroll:");
                                    int oldscroll = state.scroll;
                                    int isabs = value[0] != '-' && value[0] != '+';
                                    long delta = strtol(value, &value, 10);
                                    if (*value == '%') delta = (delta * height)/100;

                                    if (state.nfiles > height-4) {
                                        if (isabs) state.scroll = delta;
                                        else state.scroll += delta;
                                        state.scroll = clamped(state.scroll, 0, state.nfiles-1 - (height-4));
                                    }

                                    state.cursor = clamped(state.cursor + delta, 0, state.nfiles-1);
                                }
                            }
                            if (line) free(line);
                            fclose(tmpfile);
                            unlink(bb_tmpfile);
                        }

                        sortmethod_t oldmethod = state.sortmethod;
                        int oldreverse = state.sort_reverse;
                        if (newsort[0] == '+')
                            state.sort_reverse = 0;
                        else if (newsort[0] == '+')
                            state.sort_reverse = 1;
                        switch (newsort[1]) {
                            case 'a': state.sortmethod = SORT_ALPHA; break;
                            case 's': state.sortmethod = SORT_SIZE; break;
                            case 't': state.sortmethod = SORT_TIME; break;
                            case 'p': state.sortmethod = SORT_BITS; break;
                            default: break;
                        }

                        if (needs_full_refresh) {
                            strcpy(to_select, state.files[state.cursor]->d_name);
                            goto tail_call;
                        }

                        if (state.sortmethod != oldmethod || oldreverse != state.sort_reverse)
                            goto sort_files;

                        goto redraw;
                    }
                }
                goto skip_redraw;
        }
        goto skip_redraw;
    }
done:
    close_term();
    if (print_dir)
        printf("%s\n", state.path);
    if (print_selection)
        write_selection(STDOUT_FILENO, state.firstselected, sep);
    return;
}

int main(int argc, char *argv[])
{
    char *path = ".";
    char sep = '\n';
    int print_dir = 0, print_selection = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
          usage:
            printf("bb - an itty bitty console TUI file browser\n");
            printf("Usage: bb [-h/--help] [-s] [-b] [-0] [path]\n");
            return 0;
        }
        if (strcmp(argv[i], "-c") == 0) {
            char *bb_cmdfile = getenv("BBCMD");
            if (!bb_cmdfile)
                err("Can only execute bb commands from inside bb");
            FILE *f = fopen(bb_cmdfile, "w");
            for (i = i+1; i < argc; i++) {
                fprintf(f, "%s\n", argv[i]);
            }
            fclose(f);
            return 0;
        }
        if (argv[i][0] == '-' && argv[i][1] == '-')
            continue;
        if (argv[i][0] == '-') {
            for (char *c = &argv[i][1]; *c; c++) {
                switch (*c) {
                    case 'h': goto usage;
                    case 'd':
                        print_dir = 1;
                        break;
                    case '0':
                        sep = '\0';
                        break;
                    case 's':
                        print_selection = 1;
                        break;
                }
            }
            continue;
        }
        if (argv[i][0]) {
            path = argv[i];
            break;
        }
    }
    init_term();
    explore(path, print_dir, print_selection, sep);
    return 0;
}
