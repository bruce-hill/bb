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
#define writez(fd, str) write(fd, str, strlen(str))
#define IS_SELECTED(p) (((p)->atme) != NULL)

#define alt_screen() writez(termfd, "\033[?1049h")
#define default_screen() writez(termfd, "\033[?1049l")
#define hide_cursor() writez(termfd, "\033[?25l");
#define show_cursor() writez(termfd, "\033[?25h");
#define queue_select(state, name) do {\
    char *__name = (name); \
    (state)->to_select = realloc((state)->to_select, strlen(__name)+1); \
    strcpy((state)->to_select, __name); \
} while (0)

#define err(...) do { \
    if (termfd) close_term(); \
    fprintf(stderr, __VA_ARGS__); \
    if (errno) \
        fprintf(stderr, "\n%s", strerror(errno)); \
    fprintf(stderr, "\n"); \
    cleanup_and_exit(1); \
} while (0)

extern binding_t bindings[];
static struct termios orig_termios;
static int termfd = 0;
static int width, height;
static int mouse_x, mouse_y;
static char *cmdfilename = NULL;

typedef enum {
    SORT_NONE = 0,
    SORT_NAME = 'n',
    SORT_SIZE = 's',
    SORT_PERM = 'p',
    SORT_MTIME = 'm',
    SORT_CTIME = 'c',
    SORT_ATIME = 'a',
    SORT_RANDOM = 'r',

    RSORT_NAME = 'N',
    RSORT_SIZE = 'S',
    RSORT_PERM = 'P',
    RSORT_MTIME = 'M',
    RSORT_CTIME = 'C',
    RSORT_ATIME = 'A',
    RSORT_RANDOM = 'R',
} sortmethod_t;

// This bit toggles 'A' (0) vs 'a' (1)
#define SORT_DESCENDING 32
#define IS_REVERSED(method) (!((method) & SORT_DESCENDING))
#define DESCENDING(method) ((method) | SORT_DESCENDING)

typedef struct entry_s {
    struct entry_s *next, **atme;
    int visible : 1, d_isdir : 1;
    ino_t      d_ino;
    __uint16_t d_reclen;
    __uint8_t  d_type;
    __uint16_t  d_namlen;
    char *d_name;
    char d_fullname[1];
} entry_t;

typedef struct {
    char path[MAX_PATH];
    char *to_select;
    entry_t *firstselected, **files;
    size_t nselected, nfiles;
    int scroll, cursor;
    int showhidden;
    struct timespec lastclick;
    char columns[16];
    char sort;
} bb_state_t;

static void update_term_size(int sig)
{
    (void)sig;
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
    writez(termfd, "\033[?1000h\033[?1002h\033[?1015h\033[?1006h");
}

static void cleanup_and_exit(int sig)
{
    (void)sig;
    if (termfd) {
        tcsetattr(termfd, TCSAFLUSH, &orig_termios);
        close(termfd);
        termfd = 0;
    }
    printf("\033[?1000l\033[?1002l\033[?1015l\033[?1006l\033[?1049l\033[?25h\n"); // Restore default screen and show cursor
    fflush(stdout);
    unlink(cmdfilename);
    exit(1);
}

static void close_term()
{
    signal(SIGWINCH, SIG_IGN);

    // Disable mouse tracking:
    writez(termfd, "\033[?1000l\033[?1002l\033[?1015l\033[?1006l");

    tcsetattr(termfd, TCSAFLUSH, &orig_termios);
    close(termfd);
    termfd = 0;
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

    if ((child = fork()) == 0) {
        signal(SIGINT, SIG_DFL);
        // TODO: is there a max number of args? Should this be batched?
        char **args = memcheck(calloc(MAX(1, state->nselected) + 5, sizeof(char*)));
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

        char bb_depth_str[64] = {0};
        { // Set environment variable to track shell nesting
            char *depthstr = getenv("BB_DEPTH");
            int depth = depthstr ? atoi(depthstr) : 0;
            snprintf(bb_depth_str, sizeof(bb_depth_str), "%d", depth + 1);
            setenv("BB_DEPTH", bb_depth_str, 1);
            setenv("BBCMD", cmdfilename, 1);
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
    int len = snprintf(buf, sizeof(buf), "\033[%d;%dH", y+1, x+1);
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
    for (size_t i = 0; i < n; i++) {
        int escapelen = 0;
        if (str[i] <= '\x1b' && escapes[(int)str[i]] != ' ')
            escapelen = sprintf(buf, "\\%c", escapes[(int)str[i]]);
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
        writez(fd, "\033[31m");
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
            int n = sprintf(buf, "\033[3;%dr\033[%dT\033[1;%dr", height-1, lastscroll - state->scroll, height);
            write(termfd, buf, n);
        } else if (lastscroll < state->scroll) {
            int n = sprintf(buf, "\033[3;%dr\033[%dS\033[1;%dr", height-1, state->scroll - lastscroll, height);
            write(termfd, buf, n);
        }
    }
    namewidth = width - 1;
    for (char *col = state->columns; *col; ++col) {
        switch (*col) {
            case 's':
                namewidth -= sizewidth + 3;
                break;
            case 'm': case 'c': case 'a':
                namewidth -= datewidth + 3;
                break;
            case 'p':
                namewidth -= permwidth + 3;
                break;
        }
    }

    if (!lazy) {
        // Path
        term_move(0,0);
        writez(termfd, "\033[0;2;37m ");
        write_escaped(termfd, state->path, strlen(state->path), "\033[0;2;37m");
        writez(termfd, "\033[K\033[0m");

        // Columns
        term_move(0,1);
        writez(termfd, " \033[0;44;30m");
        for (char *col = state->columns; *col; ++col) {
            const char *colname;
            int colwidth = 0;
            switch (*col) {
                case 's':
                    colname = "  Size"; colwidth = sizewidth;
                    break;
                case 'p':
                    colname = "Per"; colwidth = permwidth;
                    break;
                case 'm':
                    colname = "     Modified"; colwidth = datewidth;
                    break;
                case 'a':
                    colname = "     Accessed"; colwidth = datewidth;
                    break;
                case 'c':
                    colname = "     Created"; colwidth = datewidth;
                    break;
                case 'n':
                    colname = "Name"; colwidth = namewidth;
                    break;
                default:
                    continue;
            }
            if (col != state->columns) writez(termfd, " │ ");
            writez(termfd, DESCENDING(state->sort) == *col ? (IS_REVERSED(state->sort) ? "▲" : "▼") : " ");
            for (ssize_t i = writez(termfd, colname); i < colwidth-1; i++)
                write(termfd, " ", 1);
        }
        writez(termfd, "\033[0m");
    }

    if (state->nselected > 0) {
        int len = snprintf(buf, sizeof(buf), "%lu selected ", state->nselected);
        if (strlen(state->path) + 1 + len < width) {
            term_move(width-len, 0);
            writez(termfd, "\033[0;1;30;47m");
            write(termfd, buf, len);
            writez(termfd, "\033[0m");
        }
    }

    entry_t **files = state->files;
    static const char *NORMAL_COLOR = "\033[0m";
    static const char *CURSOR_COLOR = "\033[0;30;43m";
    static const char *LINKDIR_COLOR = "\033[0;36m";
    static const char *DIR_COLOR = "\033[0;34m";
    static const char *LINK_COLOR = "\033[0;33m";
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
            writez(termfd, "\033[K");
            continue;
        }

        entry_t *entry = files[i];
        struct stat info = {0};
        lstat(entry->d_fullname, &info);

        { // Selection box:
            if (IS_SELECTED(entry))
                writez(termfd, "\033[41m \033[0m");
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

        for (char *col = state->columns; *col; ++col) {
            if (col != state->columns) writez(termfd, " │ ");
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
                    ssize_t wrote = write(termfd, " ", 1);
                    wrote += write_escaped(termfd, entry->d_name, entry->d_namlen, color);
                    if (entry->d_isdir) {
                        writez(termfd, "/");
                        ++wrote;
                    }

                    if (entry->d_type == DT_LNK) {
                        char linkpath[MAX_PATH+1] = {0};
                        ssize_t pathlen;
                        if ((pathlen = readlink(entry->d_name, linkpath, sizeof(linkpath))) < 0)
                            err("readlink() failed");
                        writez(termfd, "\033[2m -> ");
                        wrote += 4;
                        wrote += write_escaped(termfd, linkpath, pathlen, color);
                        if (entry->d_isdir) {
                            writez(termfd, "/");
                            ++wrote;
                        }
                    }
                    while (wrote++ < namewidth - 1)
                        write(termfd, " ", 1);
                    break;
                }
            }
        }
        writez(termfd, " \033[0m\033[K"); // Reset color and attributes
    }

    static const char *help = "Press '?' to see key bindings ";
    term_move(0, height - 1);
    writez(termfd, "\033[K");
    term_move(MAX(0, width - (int)strlen(help)), height - 1);
    writez(termfd, help);
    lastcursor = state->cursor;
    lastscroll = state->scroll;
}

static int compare_files(void *r, const void *v1, const void *v2)
{
    char sort = *((char *)r);
    int sign = IS_REVERSED(sort) ? -1 : 1;
    sort = DESCENDING(sort);
    const entry_t *f1 = *((const entry_t**)v1), *f2 = *((const entry_t**)v2);
    int diff = -(f1->d_isdir - f2->d_isdir);
    if (diff) return -diff; // Always sort dirs before files
    if (sort == SORT_NONE || sort == SORT_RANDOM) return 0;
    if (sort == SORT_NAME) {
        const char *p1 = f1->d_name, *p2 = f2->d_name;
        while (*p1 && *p2) {
            char c1 = *p1, c2 = *p2;
            if ('A' <= c1 && 'Z' <= c1) c1 = c1 - 'A' + 'a';
            if ('A' <= c2 && 'Z' <= c2) c2 = c2 - 'A' + 'a';
            diff = (c1 - c2);
            if ('0' <= c1 && c1 <= '9' && '0' <= c2 && c2 <= '9') {
                long n1 = strtol(p1, (char**)&p1, 10);
                long n2 = strtol(p2, (char**)&p2, 10);
                diff = ((p1 - f1->d_name) - (p2 - f2->d_name)) || (n1 - n2);
                if (diff) return diff*sign;
            } else if (diff) {
                return diff*sign;
            } else {
                ++p1; ++p2;
            }
        }
        return (*p1 - *p2)*sign;
    }
    struct stat info1, info2;
    lstat(f1->d_fullname, &info1);
    lstat(f2->d_fullname, &info2);
    switch (sort) {
        case SORT_PERM:
            return -((info1.st_mode & 0x3FF) - (info2.st_mode & 0x3FF))*sign;
        case SORT_SIZE:
            return info1.st_size > info2.st_size ? -sign : sign;
        case SORT_MTIME:
            if (info1.st_mtimespec.tv_sec == info2.st_mtimespec.tv_sec)
                return info1.st_mtimespec.tv_nsec > info2.st_mtimespec.tv_nsec ? -sign : sign;
            else
                return info1.st_mtimespec.tv_sec > info2.st_mtimespec.tv_sec ? -sign : sign;
        case SORT_CTIME:
            if (info1.st_ctimespec.tv_sec == info2.st_ctimespec.tv_sec)
                return info1.st_ctimespec.tv_nsec > info2.st_ctimespec.tv_nsec ? -sign : sign;
            else
                return info1.st_ctimespec.tv_sec > info2.st_ctimespec.tv_sec ? -sign : sign;
        case SORT_ATIME:
            if (info1.st_atimespec.tv_sec == info2.st_atimespec.tv_sec)
                return info1.st_atimespec.tv_nsec > info2.st_atimespec.tv_nsec ? -sign : sign;
            else
                return info1.st_atimespec.tv_sec > info2.st_atimespec.tv_sec ? -sign : sign;
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
    e->next = NULL; e->atme = NULL;
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
        entry->next = NULL; entry->atme = NULL;

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
    long cmdpos = 0;
    if (chdir(state->path) != 0)
        err("Couldn't chdir into '%s'", state->path);
  refresh:;
    if (!getwd(state->path))
        err("Couldn't get working directory");

    populate_files(state);
    
    state->cursor = 0;
    state->scroll = 0;

  sort_files:
    qsort_r(&state->files[1], state->nfiles-1, sizeof(entry_t*), &state->sort, compare_files);
    if (DESCENDING(state->sort) == SORT_RANDOM) {
        entry_t **files = &state->files[1];
        size_t ndirs = 0, nents = state->nfiles - 1;
        for (int i = 0; i < nents; i++) {
            if (state->files[i]->d_isdir) ++ndirs;
            else break;
        }
        for (size_t i = 0; i < ndirs - 1; i++) {
            //size_t j = i + rand() / (RAND_MAX / (ndirs - i) + 1);
            size_t j = i + rand() / (RAND_MAX / (ndirs - 1 - i));
            entry_t *tmp = files[j];
            files[j] = files[i];
            files[i] = tmp;
        }
        for (size_t i = ndirs; i < nents - 1; i++) {
            //size_t j = i + rand() / (RAND_MAX / (nents - i) + 1);
            size_t j = i + rand() / (RAND_MAX / (nents - 1 - i));
            entry_t *tmp = files[j];
            files[j] = files[i];
            files[i] = tmp;
        }
    }

    // Put the cursor on the first *real* file if one exists
    if (state->cursor == 0 && state->nfiles > 1)
        ++state->cursor;

    if (state->to_select) {
        char *sel = state->to_select;
        for (int i = 0; i < state->nfiles; i++) {
            if (strcmp(sel, sel[0] == '/' ? state->files[i]->d_fullname : state->files[i]->d_name) == 0) {
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
        lastwidth = width; lastheight = height;
        lazy = 0;
        goto redraw;
    }

  scan_cmdfile:
    {
#define cleanup_cmd() do { if (cmdfile) fclose(cmdfile); if (cmd) free(cmd); } while (0)
        FILE *cmdfile = fopen(cmdfilename, "r");
        if (!cmdfile) {
            if (!lazy) goto redraw;
            goto get_user_input;
        }

        if (cmdpos) 
            fseek(cmdfile, cmdpos, SEEK_SET);
        
        int did_anything = 0;
        char *cmd = NULL;
        size_t cap = 0;
        while (getdelim(&cmd, &cap, '\0', cmdfile) >= 0) {
            cmdpos = ftell(cmdfile);
            if (!cmd[0]) continue;
            did_anything = 1;
            char *value = strchr(cmd, ':');
            if (value) ++value;
            switch (cmd[0]) {
                case 'r': // refresh
                    queue_select(state, state->files[state->cursor]->d_name);
                    cleanup_cmd();
                    goto refresh;
                case 'q': // quit
                    cleanup_cmd();
                    goto quit;
                case 's': // sort:, select:, scroll:, spread:
                    switch (cmd[1]) {
                        case 'o': // sort:
                            switch (*value) {
                                case 'n': case 'N': case 's': case 'S':
                                case 'p': case 'P': case 'm': case 'M':
                                case 'c': case 'C': case 'a': case 'A':
                                case 'r': case 'R':
                                    state->sort = *value;
                                    queue_select(state, state->files[state->cursor]->d_name);
                                    cleanup_cmd();
                                    goto sort_files;
                            }
                            goto next_cmd;
                        case 'c': { // scroll:
                            int isdelta = value[0] == '+' || value[0] == '-';
                            long n = strtol(value, &value, 10);
                            if (*value == '%')
                                n = (n * (value[1] == 'n' ? state->nfiles : height)) / 100;
                            if (isdelta) {
                                state->cursor += n;
                                if (state->nfiles > height-4)
                                    state->scroll += n;
                            } else {
                                state->cursor = (int)n;
                                if (state->nfiles > height-4)
                                    state->scroll = (int)n;
                            }
                            if (state->nfiles > height-4)
                                state->scroll = clamped(state->scroll, 0, state->nfiles-1 - (height-4));
                            state->cursor = clamped(state->cursor, 0, state->nfiles-1);
                            break;
                        }

                        case 'p':
                            goto move;

                        case '\0': case 'e': // select:
                            lazy = 0;
                            if (strcmp(value, "*") == 0) {
                                for (int i = 0; i < state->nfiles; i++)
                                    select_file(state, state->files[i]);
                            } else {
                                entry_t *e = find_file(state, value);
                                if (e) select_file(state, e);
                            }
                            break;
                    }
                case 'c':
                    switch (cmd[1]) {
                        case 'd': { // cd:
                            char *rpbuf = realpath(value, NULL);
                            if (!rpbuf) continue;
                            if (strcmp(rpbuf, state->path) == 0) {
                                free(rpbuf);
                                continue;
                            }
                            if (chdir(rpbuf) == 0) {
                                if (strcmp(value, "..") == 0)
                                    queue_select(state, state->path);
                                strcpy(state->path, rpbuf);
                                free(rpbuf);
                                cleanup_cmd();
                                goto refresh;
                            } else {
                                free(rpbuf);
                            }
                        }
                        case 'o': // cols:
                            for (char *col = value, *dst = state->columns;
                                 *col && dst < &state->columns[sizeof(state->columns)-1]; col++) {
                                *(dst++) = DESCENDING(*col);
                                *dst = '\0';
                            }
                    }
                case 't': { // toggle:
                    lazy = 0;
                    entry_t *e = find_file(state, value);
                    if (e) {
                        if (IS_SELECTED(e)) deselect_file(state, e);
                        else select_file(state, e);
                    }
                    break;
                }
                case 'd': { // deselect:
                    lazy = 0;
                    if (strcmp(value, "*") == 0) {
                        clear_selection(state);
                    } else {
                        entry_t *e = find_file(state, value);
                        if (e) select_file(state, e);
                    }
                }
                case 'g': { // goto:
                    for (int i = 0; i < state->nfiles; i++) {
                        if (strcmp(value[0] == '/' ?
                                    state->files[i]->d_fullname : state->files[i]->d_name,
                                    value) == 0) {
                            state->cursor = i;
                            goto next_cmd;
                        }
                    }
                    char *lastslash = strrchr(value, '/');
                    if (!lastslash) goto next_cmd;
                    *lastslash = '\0'; // Split in two
                    if (chdir(value) != 0) goto next_cmd;
                    strcpy(state->path, value);
                    if (lastslash[1])
                        queue_select(state, lastslash+1);
                    cleanup_cmd();
                    goto refresh;
                }
                case 'm': { // move:
                  move:;
                    int oldcur = state->cursor;
                    int isdelta = value[0] == '-' || value[0] == '+';
                    long n = strtol(value, &value, 10);
                    if (*value == '%')
                        n = (n * (value[1] == 'n' ? state->nfiles : height)) / 100;
                    if (isdelta) state->cursor += n;
                    else state->cursor = n;

                    state->cursor = clamped(state->cursor, 0, state->nfiles-1);
                    int delta = state->cursor - oldcur;

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
                    if (cmd[0] == 's') { // spread:
                        int sel = IS_SELECTED(state->files[oldcur]);
                        for (int i = state->cursor; i != oldcur; i += (oldcur > i ? 1 : -1)) {
                            if (sel && !IS_SELECTED(state->files[i]))
                                select_file(state, state->files[i]);
                            else if (!sel && IS_SELECTED(state->files[i]))
                                deselect_file(state, state->files[i]);
                        }
                        lazy &= abs(oldcur - state->cursor) <= 1;
                    }
                }
                default: break;
            }
          next_cmd:;
        }

        cleanup_cmd();
        unlink(cmdfilename);
        cmdpos = 0;
        if (did_anything || !lazy)
            goto redraw;
    }

    int key;
  get_user_input:
    key = term_getkey(termfd, &mouse_x, &mouse_y, KEY_DELAY);
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
                        if (DESCENDING(state->sort) == *col)
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
            cleanup_and_exit(SIGINT);
            return 1;

        case KEY_CTRL_Z:
            default_screen();
            show_cursor();
            close_term();
            raise(SIGTSTP);
            init_term();
            alt_screen();
            hide_cursor();
            lazy = 0;
            goto redraw;

        case '.':
            state->showhidden ^= 1;
            queue_select(state, state->files[state->cursor]->d_name);
            goto refresh;

        case KEY_CTRL_H: {
            term_move(0,height-1);
            writez(termfd, "\033[K\033[33;1mPress any key...\033[0m");
            while ((key = term_getkey(termfd, &mouse_x, &mouse_y, 1000)) == -1)
                ;
            term_move(0,height-1);
            writez(termfd, "\033[K\033[1m<\033[33m");
            const char *name = keyname(key);
            char buf[32] = {(char)key};
            if (name) writez(termfd, name);
            else if (' ' <= key && key <= '~')
                write(termfd, buf, 1);
            else {
                sprintf(buf, "\033[31m\\x%02X", key);
                writez(termfd, buf);
            }

            writez(termfd, "\033[0;1m> is bound to: \033[34;1m");
            for (int i = 0; bindings[i].keys[0] > 0; i++) {
                for (int j = 0; bindings[i].keys[j]; j++) {
                    if (key == bindings[i].keys[j]) {
                        writez(termfd, bindings[i].description);
                        writez(termfd, "\033[0m");
                        goto next_input;
                    }
                }
            }
            writez(termfd, "--- nothing ---\033[0m");
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
            if (cmdpos != 0)
                err("Command file still open");
            term_move(0, height-1);
            //writez(termfd, "\033[K");
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

  quit:
    return 0;
}

static void print_bindings(int verbose)
{
    struct winsize sz = {0};
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &sz);
    int _width = sz.ws_col;
    if (_width == 0) _width = 80;

    char buf[1024];
    char *kb = "Key Bindings";
    printf("\n\033[33;1;4m\033[%dG%s\033[0m\n\n", (_width-(int)strlen(kb))/2, kb);
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
                p += sprintf(p, "\033[31m\\x%02X", key);
        }
        *p = '\0';
        printf("\033[1m\033[%dG%s\033[0m", _width/2 - 1 - (int)strlen(buf), buf);
        printf("\033[0m\033[%dG\033[34;1m%s\033[0m", _width/2 + 1, bindings[i].description);
        if (verbose) {
            printf("\n\033[%dG\033[0;32m", MAX(1, (_width - (int)strlen(bindings[i].command))/2));
            fflush(stdout);
            write_escaped(STDOUT_FILENO, bindings[i].command, strlen(bindings[i].command), "\033[0;32m");
        }
        printf("\033[0m\n");
    }
    printf("\n");
}

int main(int argc, char *argv[])
{
    bb_state_t state;
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
    if (!initial_path && cmd_args == 0) initial_path = ".";

    FILE *cmdfile = NULL;
    if (initial_path) {
        cmdfilename = memcheck(strdup(CMDFILE_FORMAT));
        if (!mktemp(cmdfilename)) err("Couldn't create tmpfile\n");
        cmdfile = fopen(cmdfilename, "a");
        if (!cmdfile) err("Couldn't create cmdfile: '%s'\n", cmdfilename);
    } else if (cmd_args > 0) {
        char *parent_bbcmd = getenv("BBCMD");
        if (!parent_bbcmd || parent_bbcmd[0] == '\0') {
            err("Couldn't find command file\n");
        }
        cmdfile = fopen(parent_bbcmd, "a");
        if (!cmdfile) err("Couldn't open cmdfile: '%s'\n", parent_bbcmd);
    }

    int i;
    for (i = 1; i < argc; i++) {
        if (argv[i][0] == '+') {
            fwrite(argv[i]+1, sizeof(char), strlen(argv[i]+1)+1, cmdfile);
            continue;
        }
        if (strcmp(argv[i], "--") == 0) break;
        if (strcmp(argv[i], "--help") == 0) {
          usage:
            printf("bb - an itty bitty console TUI file browser\n");
            printf("Usage: bb [-h/--help] [-s] [-b] [-0] [path]\n");
            return 1;
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
                        return 0;
                    case 'B':
                        print_bindings(1);
                        return 0;
                }
            }
            continue;
        }
    }

    if (cmdfile)
        fclose(cmdfile);

    if (!initial_path) {
        return 0;
    }

    char *real = realpath(initial_path, NULL);
    if (!real) err("Not a valid file: %s\n", initial_path);
    strcpy(state.path, initial_path);

    signal(SIGTERM, cleanup_and_exit);
    signal(SIGINT, cleanup_and_exit);
    signal(SIGXCPU, cleanup_and_exit);
    signal(SIGXFSZ, cleanup_and_exit);
    signal(SIGVTALRM, cleanup_and_exit);
    signal(SIGPROF, cleanup_and_exit);

    init_term();
    alt_screen();
    hide_cursor();
    int ret = explore(&state);
    default_screen();
    show_cursor();
    close_term();

    unlink(cmdfilename);

    if (ret == 0) {
        if (print_dir)
            printf("%s\n", state.path);
        if (print_selection)
            write_selection(STDOUT_FILENO, state.firstselected, sep);
    } else if (print_dir) {
        printf("%s\n", initial_path);
    }

    return ret;
}

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
