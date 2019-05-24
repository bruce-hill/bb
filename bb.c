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

#define MAX_PATH 4096
#define KEY_DELAY 50
#define CMDFILE_FORMAT "/tmp/bb.XXXXXX"

#define MAX(a,b) ((a) < (b) ? (b) : (a))
#define MIN(a,b) ((a) > (b) ? (b) : (a))
#define startswith(str, start) (strncmp(str, start, strlen(start)) == 0)
#define writez(fd, str) write(fd, str, strlen(str))
#define IS_SELECTED(p) (((p)->atme) != NULL)

#define alt_screen() writez(termfd, "\e[?1049h")
#define default_screen() writez(termfd, "\e[?1049l")
#define hide_cursor() writez(termfd, "\e[?25l");
#define show_cursor() writez(termfd, "\e[?25h");
#define queue_select(state, name) do {\
    if ((state)->to_select) free(state->to_select); \
    (state)->to_select = memcheck(strdup(name)); \
} while (0)

#define err(...) do { \
    default_screen(); \
    show_cursor(); \
    close_term(); \
    fprintf(stderr, __VA_ARGS__); \
    if (errno) \
        fprintf(stderr, "\n%s", strerror(errno)); \
    fprintf(stderr, "\n"); \
    exit(1); \
} while (0)

static struct termios orig_termios;
static int termfd;
static int width, height;
static int mouse_x, mouse_y;

typedef enum {
    SORT_NONE = 0,
    SORT_NAME = 'n',
    SORT_SIZE = 's',
    SORT_PERM = 'p',
    SORT_MTIME = 'm',
    SORT_CTIME = 'c',
    SORT_ATIME = 'a',

    RSORT_NAME = 'N',
    RSORT_SIZE = 'S',
    RSORT_PERM = 'P',
    RSORT_MTIME = 'M',
    RSORT_CTIME = 'C',
    RSORT_ATIME = 'A',
} sortmethod_t;

// This bit toggles 'A' (0) vs 'a' (1)
#define SORT_DESCENDING 32
#define IS_REVERSED(method) (!((method) & SORT_DESCENDING))
#define COLUMN(method) ((method) & ~SORT_DESCENDING)

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
    char path[MAX_PATH];
    char *to_select;
    entry_t *firstselected, **files;
    size_t nselected, nfiles;
    int scroll, cursor;
    int showhidden;
    struct timespec lastclick;
    char cmdfilename[sizeof(CMDFILE_FORMAT)];
    char columns[16];
    char sort;
} bb_state_t;

static void update_term_size(int _)
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
    update_term_size(0);
    signal(SIGWINCH, update_term_size);
    // Initiate mouse tracking:
    writez(termfd, "\e[?1000h\e[?1002h\e[?1015h\e[?1006h");
}

static void close_term()
{
    signal(SIGWINCH, SIG_IGN);

    // Disable mouse tracking:
    writez(termfd, "\e[?1000l\e[?1002l\e[?1015l\e[?1006l");

    tcsetattr(termfd, TCSAFLUSH, &orig_termios);
    close(termfd);
}

static void* memcheck(void *p)
{
    if (!p) err("Allocation failure");
    return p;
}

static int run_cmd_on_selection(bb_state_t *state, const char *cmd)
{
    pid_t child;
    sig_t old_handler = signal(SIGINT, SIG_IGN);

    if (!state->cmdfilename[0])
        strcpy(state->cmdfilename, CMDFILE_FORMAT);
    if (!mktemp(state->cmdfilename))
        err("Could not create temp file");

    if ((child = fork()) == 0) {
        signal(SIGINT, SIG_DFL);
        // TODO: is there a max number of args? Should this be batched?
        char **const args = memcheck(calloc(MAX(1, state->nselected) + 5, sizeof(char*)));
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
            setenv("BBCMD", state->cmdfilename, 1);
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
    // Returns number of *visible* characters written, not including coloring
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
            ++ret;
            continue;
        }
        ret += escapelen;

        if (backlog > 0) {
            write(fd, &str[i-backlog], backlog);
            backlog = 0;
        }
        writez(fd, "\e[31m");
        write(fd, buf, escapelen);
        writez(fd, reset_color);
    }
    if (backlog > 0)
        write(fd, &str[n-backlog], backlog);
    return ret;
}

static const int sizewidth = 7, datewidth = 19, permwidth = 4;
static int namewidth;
static void render(bb_state_t *state, int lazy)
{
    static int lastcursor = -1, lastscroll = -1;
    char buf[64];
    if (lastcursor == -1 || lastscroll == -1)
        lazy = 0;

    if (lazy) {
        // Use terminal scrolling:
        if (lastscroll > state->scroll) {
            int n = sprintf(buf, "\e[3;%dr\e[%dT\e[1;%dr", height-1, lastscroll - state->scroll, height);
            write(termfd, buf, n);
        } else if (lastscroll < state->scroll) {
            int n = sprintf(buf, "\e[3;%dr\e[%dS\e[1;%dr", height-1, state->scroll - lastscroll, height);
            write(termfd, buf, n);
        }
    }
    namewidth = width - 1;
    for (char *col = state->columns; *col; ++col) {
        switch (*col) {
            case 's':
                namewidth -= sizewidth + 2;
                break;
            case 'm': case 'c': case 'a':
                namewidth -= datewidth + 2;
                break;
            case 'p':
                namewidth -= permwidth + 2;
                break;
        }
    }

    if (!lazy) {
        // Path
        term_move(0,0);
        writez(termfd, "\e[0;1;30;47m ");
        write_escaped(termfd, state->path, strlen(state->path), "\e[0;1;30;47m");
        writez(termfd, "\e[K\e[0m");

        term_move(0,1);
        writez(termfd, " ");
        for (char *col = state->columns; *col; ++col) {
            if (col != state->columns) writez(termfd, " │");
            writez(termfd, COLUMN(state->sort) == *col ? (IS_REVERSED(state->sort) ? "▲" : "▼") : " ");
            switch (*col) {
                case 's':
                    for (int i = writez(termfd, " Size"); i < sizewidth-1; i++)
                        write(termfd, " ", 1);
                    break;
                case 'p':
                    for (int i = writez(termfd, "Per"); i < permwidth-1; i++)
                        write(termfd, " ", 1);
                    break;
                case 'm':
                    for (int i = writez(termfd, "Modified"); i < datewidth-1; i++)
                        write(termfd, " ", 1);
                    break;
                case 'a':
                    for (int i = writez(termfd, "Accessed"); i < datewidth-1; i++)
                        write(termfd, " ", 1);
                    break;
                case 'c':
                    for (int i = writez(termfd, "Created"); i < datewidth-1; i++)
                        write(termfd, " ", 1);
                    break;
                case 'n':
                    for (int i = writez(termfd, "Name"); i < namewidth; i++)
                        write(termfd, " ", 1);
                    break;
            }
        }
    }

    if (state->nselected > 0) {
        int len = snprintf(buf, sizeof(buf), "%lu selected ", state->nselected);
        if (strlen(state->path) + 1 + len < width) {
            term_move(width-len, 0);
            writez(termfd, "\e[0;1;30;47m");
            write(termfd, buf, len);
            writez(termfd, "\e[0m");
        }
    }

    entry_t **files = state->files;
    static const char *NORMAL_COLOR = "\e[0m";
    static const char *CURSOR_COLOR = "\e[0;30;43m";
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

        int first = 1;
        for (char *col = state->columns; *col; ++col) {
            if (col != state->columns) writez(termfd, " │");
            switch (*col) {
                case 's': {
                    int j = 0;
                    const char* units = "BKMGTPEZY";
                    double bytes = (double)info.st_size;
                    while (bytes > 1024) {
                        bytes /= 1024;
                        j++;
                    }
                    sprintf(buf, "%6.*f%c", j > 0 ? 1 : 0, bytes, units[j]);
                    writez(termfd, buf);
                    break;
                }

                case 'm':
                    strftime(buf, sizeof(buf), "%l:%M%p %b %e %Y", localtime(&(info.st_mtime)));
                    writez(termfd, buf);
                    break;

                case 'c':
                    strftime(buf, sizeof(buf), "%l:%M%p %b %e %Y", localtime(&(info.st_ctime)));
                    writez(termfd, buf);
                    break;

                case 'a':
                    strftime(buf, sizeof(buf), "%l:%M%p %b %e %Y", localtime(&(info.st_atime)));
                    writez(termfd, buf);
                    break;

                case 'p':
                    buf[0] = ' ';
                    buf[1] = '0' + ((info.st_mode >> 6) & 7);
                    buf[2] = '0' + ((info.st_mode >> 3) & 7);
                    buf[3] = '0' + ((info.st_mode >> 0) & 7);
                    write(termfd, buf, 4);
                    break;

                case 'n': {
                    int wrote = write_escaped(termfd, entry->d_name, entry->d_namlen, color);
                    if (entry->d_isdir) {
                        writez(termfd, "/");
                        ++wrote;
                    }

                    if (entry->d_type == DT_LNK) {
                        char linkpath[MAX_PATH+1] = {0};
                        ssize_t pathlen;
                        if ((pathlen = readlink(entry->d_name, linkpath, sizeof(linkpath))) < 0)
                            err("readlink() failed");
                        writez(termfd, "\e[2m -> ");
                        wrote += 4;
                        wrote += write_escaped(termfd, linkpath, pathlen, color);
                        if (entry->d_isdir) {
                            writez(termfd, "/");
                            ++wrote;
                        }
                    }
                    for (int i = wrote; i < namewidth-1; i++)
                        write(termfd, " ", 1);
                    break;
                }
            }
        }
        writez(termfd, " \e[0m\e[K"); // Reset color and attributes
    }

    static const char *help = "Press '?' to see key bindings ";
    term_move(0, height - 1);
    writez(termfd, "\e[K");
    term_move(MAX(0, width - strlen(help)), height - 1);
    writez(termfd, help);
    lastcursor = state->cursor;
    lastscroll = state->scroll;
}

static int compare_files(void *r, const void *v1, const void *v2)
{
    char sort = *((char *)r);
    int sign = IS_REVERSED(sort) ? -1 : 1;
    const entry_t *f1 = *((const entry_t**)v1), *f2 = *((const entry_t**)v2);
    int diff = -(f1->d_isdir - f2->d_isdir);
    if (diff) return -diff; // Always sort dirs before files
    if (sort == SORT_NONE) return 0;
    if (COLUMN(sort) == SORT_NAME) {
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
    struct stat info1, info2;
    lstat(f1->d_fullname, &info1);
    lstat(f2->d_fullname, &info2);
    switch (COLUMN(sort)) {
        case SORT_PERM:
            return -((info1.st_mode & 0x3FF) - (info2.st_mode & 0x3FF))*sign;
        case SORT_SIZE:
            return -(info1.st_size - info2.st_size)*sign;
        case SORT_MTIME:
            if (info1.st_mtimespec.tv_sec == info2.st_mtimespec.tv_sec)
                return -(info1.st_mtimespec.tv_nsec - info2.st_mtimespec.tv_nsec)*sign;
            else
                return -(info1.st_mtimespec.tv_sec - info2.st_mtimespec.tv_sec)*sign;
        case SORT_CTIME:
            if (info1.st_ctimespec.tv_sec == info2.st_ctimespec.tv_sec)
                return -(info1.st_ctimespec.tv_nsec - info2.st_ctimespec.tv_nsec)*sign;
            else
                return -(info1.st_ctimespec.tv_sec - info2.st_ctimespec.tv_sec)*sign;
        case SORT_ATIME:
            if (info1.st_atimespec.tv_sec == info2.st_atimespec.tv_sec)
                return -(info1.st_atimespec.tv_nsec - info2.st_atimespec.tv_nsec)*sign;
            else
                return -(info1.st_atimespec.tv_sec - info2.st_atimespec.tv_sec)*sign;
    }
    return 0;
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
    entry_t **tofree = memcheck(calloc(state->nselected, sizeof(entry_t*)));
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

static void populate_files(bb_state_t *state)
{
    // Clear old files (if any)
    if (state->files) {
        for (int i = 0; i < state->nfiles; i++) {
            entry_t *e = state->files[i];
            e->visible = 0;
            if (!IS_SELECTED(e))
                free(e);
        }
        free(state->files);
    }
    state->files = NULL;
    state->nfiles = 0;

    // Hash inode -> entry_t with linear probing
    size_t hashsize = 2 * state->nselected;
    entry_t **selecthash = memcheck(calloc(hashsize, sizeof(entry_t*)));
    for (entry_t *p = state->firstselected; p; p = p->next) {
        int probe = ((int)p->d_ino) % hashsize;
        while (selecthash[probe])
            probe = (probe + 1) % hashsize;
        selecthash[probe] = p;
    }

    DIR *dir = opendir(state->path);
    if (!dir)
        err("Couldn't open dir: %s", state->path);
    struct dirent *dp;
    size_t pathlen = strlen(state->path);
    size_t filecap = 0;
    while ((dp = readdir(dir)) != NULL) {
        if (dp->d_name[0] == '.' && dp->d_name[1] == '\0')
            continue;
        if (!state->showhidden && dp->d_name[0] == '.' && !(dp->d_name[1] == '.' && dp->d_name[2] == '\0'))
            continue;
        // Hashed lookup from selected:
        if (state->nselected > 0) {
            for (int probe = ((int)dp->d_ino) % hashsize; selecthash[probe]; probe = (probe + 1) % hashsize) {
                if (selecthash[probe]->d_ino == dp->d_ino) {
                    selecthash[probe]->visible = 1;
                    state->files[state->nfiles++] = selecthash[probe];
                    goto next_file;
                }
            }
        }
        entry_t *entry = memcheck(malloc(sizeof(entry_t) + pathlen + dp->d_namlen + 2));
        strncpy(entry->d_fullname, state->path, pathlen);
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

        if (state->nfiles >= filecap) {
            filecap += 100;
            state->files = memcheck(realloc(state->files, filecap*sizeof(entry_t*)));
        }
        state->files[state->nfiles++] = entry;
      next_file:;
    }
    closedir(dir);
    free(selecthash);
    if (state->nfiles == 0) err("No files found (not even '..')");
}

static int explore(bb_state_t *state)
{
    if (chdir(state->path) != 0)
        err("Couldn't chdir into '%s'", state->path);
  refresh:
    if (!getwd(state->path))
        err("Couldn't get working directory");

    populate_files(state);
    
    state->cursor = 0;
    state->scroll = 0;

  sort_files:
    qsort_r(&state->files[1], state->nfiles-1, sizeof(entry_t*), &state->sort, compare_files);

    // Put the cursor on the first *real* file if one exists
    if (state->cursor == 0 && state->nfiles > 1)
        ++state->cursor;

    if (state->to_select) {
        for (int i = 0; i < state->nfiles; i++) {
            if (strcmp(state->to_select, state->files[i]->d_name) == 0) {
                state->cursor = i;
                if (state->nfiles > height - 4)
                    state->scroll = MAX(0, i - MIN(SCROLLOFF, (height-4)/2));
                break;
            }
        }
        free(state->to_select);
        state->to_select = NULL;
    }

    int lastwidth = width, lastheight = height;
    int scrolloff = MIN(SCROLLOFF, (height-4)/2);
    int lazy = 0;

  redraw:
    render(state, lazy);
    lazy = 1;

  next_input:
    if (width != lastwidth || height != lastheight) {
        scrolloff = MIN(SCROLLOFF, (height-4)/2);
        lastwidth = width, lastheight = height;
        lazy = 0;
        goto redraw;
    }

  scan_cmdfile:
    if (state->cmdfilename[0]) {
        // Scan for IPC requests
        FILE *cmdfile;
        if (!(cmdfile = fopen(state->cmdfilename, "r")))
            goto redraw;

        char *line = NULL;
        size_t capacity = 0;
        ssize_t len;
#define cleanup_cmdfile() do { fclose(cmdfile); if (line) free(line); } while (0)
        while ((len = getdelim(&line, &capacity, '\0', cmdfile)) >= 0) {
            if (!line[0]) continue;
            char *value = strchr(line, ':');
            if (value) ++value;
            if (strcmp(line, "refresh") == 0) {
                cleanup_cmdfile();
                queue_select(state, state->files[state->cursor]->d_name);
                goto refresh;
            } else if (strcmp(line, "quit") == 0) {
                cleanup_cmdfile();
                return 1;
            } else if (startswith(line, "sort:")) {
                state->sort = *value;
                queue_select(state, state->files[state->cursor]->d_name);
                cleanup_cmdfile();
                goto sort_files;
            } else if (startswith(line, "cd:")) {
                if (chdir(value) == 0) {
                    strcpy(state->path, value);
                    getwd(state->path);
                    queue_select(state, state->files[state->cursor]->d_name);
                    cleanup_cmdfile();
                    goto refresh;
                }
            } else if (startswith(line, "toggle:")) {
                lazy = 0;
                entry_t *e = find_file(state, line + strlen("select:"));
                if (e) {
                    if (IS_SELECTED(e)) deselect_file(state, e);
                    else select_file(state, e);
                }
            } else if (startswith(line, "select:")) {
                lazy = 0;
                if (strcmp(value, "*") == 0) {
                    for (int i = 0; i < state->nfiles; i++)
                        select_file(state, state->files[i]);
                } else {
                    entry_t *e = find_file(state, value);
                    if (e) select_file(state, e);
                }
            } else if (startswith(line, "deselect:")) {
                lazy = 0;
                if (strcmp(value, "*") == 0) {
                    clear_selection(state);
                } else {
                    entry_t *e = find_file(state, value);
                    if (e) select_file(state, e);
                }
            } else if (startswith(line, "cursor:")) {
                for (int i = 0; i < state->nfiles; i++) {
                    if (strcmp(value[0] == '/' ?
                                state->files[i]->d_fullname : state->files[i]->d_name,
                                value) == 0) {
                        state->cursor = i;
                        goto next_line;
                    }
                }
                char *lastslash = strrchr(value, '/');
                if (!lastslash) goto next_line;
                *lastslash = '\0'; // Split in two
                if (chdir(value) != 0) goto next_line;
                strcpy(state->path, value);
                if (lastslash[1])
                    queue_select(state, lastslash+1);
                cleanup_cmdfile();
                goto refresh;
            } else if (startswith(line, "move:")) {
                int expand_sel = 0;
                if (*value == 'x') {
                    expand_sel = 1;
                    ++value;
                }
                int oldcur = state->cursor;
                int isabs = value[0] != '-' && value[0] != '+';
                long delta = strtol(value, &value, 10);
                if (*value == '%') delta = (delta * height)/100;
                if (isabs) state->cursor = delta;
                else state->cursor += delta;

                state->cursor = clamped(state->cursor, 0, state->nfiles-1);
                delta = state->cursor - oldcur;

                if (state->nfiles > height-4) {
                    if (delta > 0) {
                        if (state->cursor >= state->scroll + (height-4) - scrolloff)
                            state->scroll += delta;
                    } else if (delta < 0) {
                        if (state->cursor <= state->scroll + scrolloff)
                            state->scroll += delta;
                    }
                    //int target = clamped(state->scroll, state->cursor - (height-4) + scrolloff, state->cursor - scrolloff);
                    //state->scroll += (delta > 0 ? 1 : -1)*MIN(abs(target-state->scroll), abs((int)delta));
                    //state->scroll = target;
                    state->scroll = clamped(state->scroll, state->cursor - (height-4) + 1, state->cursor);
                    state->scroll = clamped(state->scroll, 0, state->nfiles-1 - (height-4));
                }
                if (expand_sel) {
                    int sel = IS_SELECTED(state->files[oldcur]);
                    for (int i = state->cursor; i != oldcur; i += (oldcur > i ? 1 : -1)) {
                        if (sel && !IS_SELECTED(state->files[i]))
                            select_file(state, state->files[i]);
                        else if (!sel && IS_SELECTED(state->files[i]))
                            deselect_file(state, state->files[i]);
                    }
                    lazy &= abs(oldcur - state->cursor) <= 1;
                }
            } else if (startswith(line, "scroll:")) {
                int oldscroll = state->scroll;
                int isabs = value[0] != '-' && value[0] != '+';
                long delta = strtol(value, &value, 10);
                if (*value == '%') delta = (delta * height)/100;

                int fudge = state->cursor - clamped(state->cursor, state->scroll + scrolloff, state->scroll + (height-4) - scrolloff);
                if (state->nfiles > height-4) {
                    if (isabs) state->scroll = delta;
                    else state->scroll += delta;
                    state->scroll = clamped(state->scroll, 0, state->nfiles-1 - (height-4));
                }

                state->cursor = clamped(state->cursor, state->scroll, state->scroll + (height-4));
                /*
                //if (!isabs && abs(state->scroll - oldscroll) == abs(delta)) {
                state->cursor = clamped(state->cursor, state->scroll + scrolloff, state->scroll + (height-4) - scrolloff);
                if (fudge && fudge < 0 != delta < 0)
                    state->cursor += fudge;
                    */
                state->cursor = clamped(state->cursor, 0, state->nfiles-1);
            }
          next_line:;
        }

        cleanup_cmdfile();
        if (unlink(state->cmdfilename))
            err("Failed to delete cmdfile %s", state->cmdfilename);
        state->cmdfilename[0] = '\0';
        goto redraw;
    }


    int key = term_getkey(termfd, &mouse_x, &mouse_y, KEY_DELAY);
    switch (key) {
        case KEY_MOUSE_LEFT: {
            struct timespec clicktime;
            clock_gettime(CLOCK_MONOTONIC, &clicktime);
            double dt_ms = 1e3*(double)(clicktime.tv_sec - state->lastclick.tv_sec);
            dt_ms += 1e-6*(double)(clicktime.tv_nsec - state->lastclick.tv_nsec);
            state->lastclick = clicktime;
            if (mouse_y == 1) {
                // Sort column:
                int x = 1;
                for (char *col = state->columns; *col; ++col) {
                    if (col != state->columns) x += 3;
                    switch (*col) {
                        case 's': x += sizewidth; break;
                        case 'p': x += permwidth; break;
                        case 'm': case 'a': case 'c':
                            x += datewidth; break;
                        case 'n': x += namewidth; break;
                    }
                    if (x >= mouse_x) {
                        if (COLUMN(state->sort) == *col)
                            state->sort ^= SORT_DESCENDING;
                        else
                            state->sort = *col;
                        goto sort_files;
                    }
                }
                goto next_input;
            } else if (mouse_y >= 2 && state->scroll + (mouse_y - 2) < state->nfiles) {
                int clicked = state->scroll + (mouse_y - 2);
                if (mouse_x == 0) {
                    if (IS_SELECTED(state->files[clicked]))
                        deselect_file(state, state->files[clicked]);
                    else
                        select_file(state, state->files[clicked]);
                    goto redraw;
                }
                state->cursor = clicked;
                if (dt_ms <= 200)
                    key = KEY_MOUSE_DOUBLE_LEFT;
                goto user_bindings;
            }
            break;
        }

        case KEY_CTRL_C:
            default_screen();
            show_cursor();
            close_term();
            return 1;

        case KEY_CTRL_Z:
            default_screen();
            show_cursor();
            close_term();
            raise(SIGTSTP);
            init_term();
            alt_screen();
            hide_cursor();
            goto redraw;

        case '.':
            state->showhidden ^= 1;
            queue_select(state, state->files[state->cursor]->d_name);
            goto refresh;

        case KEY_CTRL_H: {
            term_move(0,height-1);
            writez(termfd, "\e[K\e[33;1mPress any key...\e[0m");
            while ((key = term_getkey(termfd, &mouse_x, &mouse_y, 1000)) == -1)
                ;
            term_move(0,height-1);
            writez(termfd, "\e[K\e[1m<\e[33m");
            const char *name = keyname(key);
            char buf[32] = {key};
            if (name) writez(termfd, name);
            else if (' ' <= key && key <= '~')
                write(termfd, buf, 1);
            else {
                sprintf(buf, "\e[31m\\x%02X", key);
                writez(termfd, buf);
            }

            writez(termfd, "\e[0;1m> is bound to: \e[34;1m");
            for (int i = 0; bindings[i].keys[0] > 0; i++) {
                for (int j = 0; bindings[i].keys[j]; j++) {
                    if (key == bindings[i].keys[j]) {
                        writez(termfd, bindings[i].description);
                        writez(termfd, "\e[0m");
                        goto next_input;
                    }
                }
            }
            writez(termfd, "--- nothing ---\e[0m");
            goto next_input;
        }

        case -1:
            goto next_input;

        default:
            // Search user-defined key bindings from config.h:
          user_bindings:;
            binding_t *binding;
            for (int i = 0; bindings[i].keys[0] > 0; i++) {
                for (int j = 0; bindings[i].keys[j]; j++) {
                    if (key == bindings[i].keys[j]) {
                        binding = &bindings[i];
                        goto run_binding;
                    }
                }
            }
            goto redraw;

          run_binding:
            term_move(0, height-1);
            //writez(termfd, "\e[K");
            if (binding->flags & NORMAL_TERM) {
                default_screen();
                show_cursor();
            }
            close_term();
            run_cmd_on_selection(state, binding->command);
            init_term();
            if (binding->flags & NORMAL_TERM) {
                lazy = 0;
                alt_screen();
            }
            hide_cursor();
            goto scan_cmdfile;
    }
    goto next_input;

    return 0;
}

static void print_bindings(int verbose)
{
    struct winsize sz = {0};
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &sz);
    int width = sz.ws_col;
    int height = sz.ws_row;
    if (width == 0) width = 80;

    char buf[1024];
    char *kb = "Key Bindings";
    printf("\n\e[33;1;4m\e[%dG%s\e[0m\n\n", (width-(int)strlen(kb))/2, kb);
    for (int i = 0; bindings[i].keys[0]; i++) {
        char *p = buf;
        for (int j = 0; bindings[i].keys[j]; j++) {
            if (j > 0) *(p++) = ',';
            int key = bindings[i].keys[j];
            const char *name = keyname(key);
            if (name)
                p = strcpy(p, name);
            else if (' ' <= key && key <= '~')
                p += sprintf(p, "%c", (char)key);
            else
                p += sprintf(p, "\e[31m\\x%02X", key);
        }
        *p = '\0';
        printf("\e[1m\e[%dG%s\e[0m", width/2 - 1 - (int)strlen(buf), buf);
        printf("\e[0m\e[%dG\e[34;1m%s\e[0m", width/2 + 1, bindings[i].description);
        if (verbose) {
            printf("\n\e[%dG\e[0;32m", MAX(1, (width - (int)strlen(bindings[i].command))/2));
            fflush(stdout);
            write_escaped(STDOUT_FILENO, bindings[i].command, strlen(bindings[i].command), "\e[0;32m");
        }
        printf("\e[0m\n");
    }
    printf("\n");
}

int main(int argc, char *argv[])
{
    int ret = 0;
    bb_state_t state = {0};
    memset(&state, 0, sizeof(bb_state_t));
    clock_gettime(CLOCK_MONOTONIC, &state.lastclick);
    state.sort = 'n';
    strncpy(state.columns, "smpn", sizeof(state.columns));

    char *initial_path = NULL;
    char sep = '\n';
    int print_dir = 0, print_selection = 0;
    int cmd_args = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '+') {
            ++cmd_args;
            continue;
        }
        if (strcmp(argv[i], "--") == 0) {
            if (i + 1 < argc) initial_path = argv[i+1];
            break;
        }
        if (argv[i][0] != '-' && !initial_path)
            initial_path = argv[i];
    }
    
    char *cmdfilename = NULL;
    FILE *cmdfile = NULL;
    if (cmd_args > 0) {
        if (initial_path) {
            strcpy(state.cmdfilename, CMDFILE_FORMAT);
            cmdfilename = state.cmdfilename;
        } else {
            cmdfilename = getenv("BBCMD");
            if (!cmdfilename || !*cmdfilename) {
                fprintf(stderr, "Commands only work from inside bb\n");
                ret = 1;
                goto done;
            }
        }
        FILE *f = fopen(cmdfilename, "w");
        if (!f) {
            fprintf(stderr, "Could not open command file: %s\n", cmdfilename);
            ret = 1;
            goto done;
        }
    }

    int i;
    for (i = 1; i < argc; i++) {
        if (argv[i][0] == '+') {
            for (i = i+1; i < argc; i++) {
                fprintf(cmdfile, "%s", argv[i]+1);
                fputc('\0', cmdfile);
            }
            continue;
        }
        if (strcmp(argv[i], "--") == 0) break;
        if (strcmp(argv[i], "--help") == 0) {
          usage:
            printf("bb - an itty bitty console TUI file browser\n");
            printf("Usage: bb [-h/--help] [-s] [-b] [-0] [path]\n");
            goto done;
        }
        if (strcmp(argv[i], "--columns") == 0 && i + 1 < argc) {
            strncpy(state.columns, argv[++i], sizeof(state.columns));
            continue;
        }
        if (strcmp(argv[i], "--sort") == 0 && i + 1 < argc) {
            state.sort = *argv[++i];
            continue;
        }
        if (argv[i][0] == '-' && argv[i][1] == '-') {
            if (argv[i][2] == '\0') break;
            continue;
        }
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
                    case 'b':
                        print_bindings(0);
                        goto done;
                    case 'B':
                        print_bindings(1);
                        goto done;
                }
            }
            continue;
        }
    }
    if (cmdfile) {
        fclose(cmdfile);
        cmdfile = NULL;
    }

    if (!initial_path) initial_path = ".";
    strcpy(state.path, initial_path);

    init_term();
    alt_screen();
    hide_cursor();
    explore(&state);
    default_screen();
    show_cursor();
    close_term();

    if (ret == 0) {
        if (print_dir)
            printf("%s\n", state.path);
        if (print_selection)
            write_selection(STDOUT_FILENO, state.firstselected, sep);
    } else if (print_dir) {
        printf("%s\n", initial_path);
    }

  done:
    if (cmdfile) fclose(cmdfile);
    return ret;
}
