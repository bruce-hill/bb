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
#define SCROLLOFF MIN(5, (termheight-4)/2)
#define CMDFILE_FORMAT "/tmp/bb.XXXXXX"

#define MAX(a,b) ((a) < (b) ? (b) : (a))
#define MIN(a,b) ((a) > (b) ? (b) : (a))
#define writez(fd, str) write(fd, str, strlen(str))
#define IS_SELECTED(p) (((p)->atme) != NULL)
// This uses the intrusive linked list offsets
//#define PREV_STATE(s) ((s) == firststate ? NULL : (bb_state_t*)((s)->atme - offsetof(bb_state_t, next)))
#define PREV_STATE(s) ((s) == firststate ? NULL : (bb_state_t*)((long)(s)->atme + (long)(s) - (long)(&(s)->atme)))
#define LLREMOVE(e) do { \
    (e)->next->atme = (e)->atme; \
    *((e)->atme) = (e)->next; \
    (e)->next = NULL; \
    (e)->atme = NULL; \
} while (0)

#define alt_screen() writez(termfd, "\033[?1049h")
#define default_screen() writez(termfd, "\033[?1049l")
#define hide_cursor() writez(termfd, "\033[?25l");
#define show_cursor() writez(termfd, "\033[?25h");

#define err(...) do { \
    if (termfd) { \
        default_screen(); \
        close_term(); \
    } \
    fprintf(stderr, __VA_ARGS__); \
    if (errno) \
        fprintf(stderr, "\n%s", strerror(errno)); \
    fprintf(stderr, "\n"); \
    cleanup_and_exit(1); \
} while (0)

// This bit toggles 'A' (0) vs 'a' (1)
#define SORT_DESCENDING 32
#define IS_REVERSED(method) (!((method) & SORT_DESCENDING))
#define DESCENDING(method) ((method) | SORT_DESCENDING)

// Types
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

/* Both entry_t and bb_state_t use intrusive linked lists.  This means they can
 * only belong to one list at a time, in this case the list of selected entries
 * and the list of states, respectively. 'atme' is an indirect pointer to
 * either the 'next' field of the previous list member, or the variable that
 * points to the first list member. In other words, item->next->atme == &item->next
 * and firstitem->atme == &firstitem.
 */
typedef struct entry_s {
    struct entry_s *next, **atme;
    int visible;
    int d_isdir : 1;
    ino_t      d_ino;
    __uint16_t d_reclen;
    __uint8_t  d_type;
    __uint16_t  d_namlen;
    char *d_name;
    char d_fullname[1];
} entry_t;

typedef struct bb_state_s {
    struct bb_state_s *next, **atme;
    entry_t **files;
    char path[MAX_PATH];
    char *to_select;
    int nfiles;
    int scroll, cursor;
    int showhidden;
    char columns[16];
    char sort;
} bb_state_t;

// Functions
static bb_state_t *new_state(bb_state_t *template);
static void delete_state(bb_state_t *s);
static void update_term_size(int sig);
static void init_term(void);
static void cleanup_and_exit(int sig);
static void close_term(void);
static void* memcheck(void *p);
static int run_cmd_on_selection(bb_state_t *s, const char *cmd);
static void term_move(int x, int y);
static int write_escaped(int fd, const char *str, size_t n, const char *reset_color);
static void render(bb_state_t *s, int lazy);
static int compare_files(void *r, const void *v1, const void *v2);
static int find_file(bb_state_t *s, const char *name);
static void write_selection(int fd, char sep);
static void clear_selection(void);
static int select_file(entry_t *e);
static int deselect_file(entry_t *e);
static void set_cursor(bb_state_t *state, int i);
static void set_scroll(bb_state_t *state, int i);
static void populate_files(bb_state_t *s, const char *path);
static void sort_files(bb_state_t *state);
static entry_t *explore(const char *path);
static void print_bindings(int verbose);

// Global variables
extern binding_t bindings[];
static struct termios orig_termios;
static int termfd = 0;
static int termwidth, termheight;
static int mouse_x, mouse_y;
static char *cmdfilename = NULL;
static const int colsizew = 7, coldatew = 19, colpermw = 4;
static int colnamew;
static struct timespec lastclick = {0, 0};
static entry_t *firstselected = NULL;
static bb_state_t *firststate = NULL;


bb_state_t *new_state(bb_state_t *template)
{
    bb_state_t *s = memcheck(calloc(1, sizeof(bb_state_t)));
    if (template) {
        populate_files(s, template->path);
        s->cursor = template->cursor;
        s->scroll = template->scroll;
        s->showhidden = template->showhidden;
        strcpy(s->columns, template->columns);
        s->sort = template->sort;
        sort_files(s);
    } else {
        s->sort = 'n';
        strncpy(s->columns, "smpn", sizeof(s->columns));
    }
    return s;
}

void delete_state(bb_state_t *s)
{
    if (s->files) {
        for (int i = 0; i < s->nfiles; i++) {
            entry_t *e = s->files[i];
            --e->visible;
            if (!IS_SELECTED(e))
                free(e);
        }
        free(s->files);
    }
    if (s->to_select)
        free(s->to_select);

    LLREMOVE(s);
    memset(s, 'X', sizeof(bb_state_t)); // Just to be safe
    free(s);
    if (!firststate)
        cleanup_and_exit(0);
}

void update_term_size(int sig)
{
    (void)sig;
    struct winsize sz = {0};
    ioctl(termfd, TIOCGWINSZ, &sz);
    termwidth = sz.ws_col;
    termheight = sz.ws_row;
}

void init_term(void)
{
    termfd = open("/dev/tty", O_RDWR);
    tcgetattr(termfd, &orig_termios);
    struct termios tios;
    memcpy(&tios, &orig_termios, sizeof(tios));
    tios.c_iflag &= (unsigned long)~(IGNBRK | BRKINT | PARMRK | ISTRIP
                           | INLCR | IGNCR | ICRNL | IXON);
    tios.c_oflag &= (unsigned long)~OPOST;
    tios.c_lflag &= (unsigned long)~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    tios.c_cflag &= (unsigned long)~(CSIZE | PARENB);
    tios.c_cflag |= (unsigned long)CS8;
    tios.c_cc[VMIN] = 0;
    tios.c_cc[VTIME] = 0;
    tcsetattr(termfd, TCSAFLUSH, &tios);
    update_term_size(0);
    signal(SIGWINCH, update_term_size);
    // Initiate mouse tracking:
    writez(termfd, "\033[?1000h\033[?1002h\033[?1015h\033[?1006h");
}

void cleanup_and_exit(int sig)
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

void close_term(void)
{
    signal(SIGWINCH, SIG_IGN);

    // Disable mouse tracking:
    writez(termfd, "\033[?1000l\033[?1002l\033[?1015l\033[?1006l");

    tcsetattr(termfd, TCSAFLUSH, &orig_termios);
    close(termfd);
    termfd = 0;
}

void* memcheck(void *p)
{
    if (!p) err("Allocation failure");
    return p;
}

int run_cmd_on_selection(bb_state_t *s, const char *cmd)
{
    pid_t child;
    sig_t old_handler = signal(SIGINT, SIG_IGN);

    if ((child = fork()) == 0) {
        signal(SIGINT, SIG_DFL);
        // TODO: is there a max number of args? Should this be batched?
        size_t space = 32;
        char **args = memcheck(calloc(space, sizeof(char*)));
        size_t i = 0;
        args[i++] = "sh";
        args[i++] = "-c";
        args[i++] = (char*)cmd;
        args[i++] = "--";
        entry_t *first = firstselected ? firstselected : s->files[s->cursor];
        for (entry_t *e = first; e; e = e->next) {
            if (i >= space) {
                space += 100;
                args = memcheck(realloc(args, space*sizeof(char*)));
            }
            args[i++] = e->d_name;
        }
        args[i] = NULL;

        setenv("BBCURSOR", s->files[s->cursor]->d_name, 1);
        setenv("BBFULLCURSOR", s->files[s->cursor]->d_fullname, 1);

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

void term_move(int x, int y)
{
    static char buf[32] = {0};
    int len = snprintf(buf, sizeof(buf), "\033[%d;%dH", y+1, x+1);
    if (len > 0)
        write(termfd, buf, (size_t)len);
}

int write_escaped(int fd, const char *str, size_t n, const char *reset_color)
{
    // Returns number of *visible* characters written, not including coloring
    // escapes['\n'] == 'n', etc.
    static const char *escapes = "       abtnvfr             e";
    char buf[5];
    int ret = 0;
    size_t backlog = 0;
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
        write(fd, buf, (size_t)escapelen);
        writez(fd, reset_color);
    }
    if (backlog > 0)
        write(fd, &str[n-backlog], backlog);
    return ret;
}

void render(bb_state_t *s, int lazy)
{
    static int lastcursor = -1, lastscroll = -1;
    char buf[64];
    if (lastcursor == -1 || lastscroll == -1)
        lazy = 0;

    if (lazy) {
        // Use terminal scrolling:
        if (lastscroll > s->scroll) {
            int n = sprintf(buf, "\033[3;%dr\033[%dT\033[1;%dr", termheight-1, lastscroll - s->scroll, termheight);
            write(termfd, buf, (size_t)n);
        } else if (lastscroll < s->scroll) {
            int n = sprintf(buf, "\033[3;%dr\033[%dS\033[1;%dr", termheight-1, s->scroll - lastscroll, termheight);
            write(termfd, buf, (size_t)n);
        }
    }
    colnamew = termwidth - 1;
    for (char *col = s->columns; *col; ++col) {
        switch (*col) {
            case 's':
                colnamew -= colsizew + 3;
                break;
            case 'm': case 'c': case 'a':
                colnamew -= coldatew + 3;
                break;
            case 'p':
                colnamew -= colpermw + 3;
                break;
        }
    }

    if (!lazy) {
        // Path
        term_move(0,0);
        char tabnum[] = "1)";
        for (bb_state_t *si = firststate; si; si = si->next) {
            const char *color = si == s ? "\033[0;1;37m" : "\033[0;2;37m";
            writez(termfd, color);
            write(termfd, tabnum, 2);
            ++tabnum[0];
            // TODO error check
            char *title = si == s ? si->path : strrchr(si->path, '/') + 1;
            write_escaped(termfd, title, strlen(title), color);
            writez(termfd, " ");
        }
        writez(termfd, "\033[K\033[0m");

        // Columns
        term_move(0,1);
        writez(termfd, " \033[0;44;30m");
        for (char *col = s->columns; *col; ++col) {
            const char *colname;
            int colwidth = 0;
            switch (*col) {
                case 's':
                    colname = "  Size"; colwidth = colsizew;
                    break;
                case 'p':
                    colname = "Per"; colwidth = colpermw;
                    break;
                case 'm':
                    colname = "     Modified"; colwidth = coldatew;
                    break;
                case 'a':
                    colname = "     Accessed"; colwidth = coldatew;
                    break;
                case 'c':
                    colname = "     Created"; colwidth = coldatew;
                    break;
                case 'n':
                    colname = "Name"; colwidth = colnamew;
                    break;
                default:
                    continue;
            }
            if (col != s->columns) writez(termfd, " │ ");
            writez(termfd, DESCENDING(s->sort) == *col ? (IS_REVERSED(s->sort) ? "▲" : "▼") : " ");
            for (ssize_t i = writez(termfd, colname); i < colwidth-1; i++)
                write(termfd, " ", 1);
        }
        writez(termfd, "\033[0m");
    }

    entry_t **files = s->files;
    static const char *NORMAL_COLOR = "\033[0m";
    static const char *CURSOR_COLOR = "\033[0;30;43m";
    static const char *LINKDIR_COLOR = "\033[0;36m";
    static const char *DIR_COLOR = "\033[0;34m";
    static const char *LINK_COLOR = "\033[0;33m";
    for (int i = s->scroll; i < s->scroll + termheight - 3; i++) {
        if (lazy) {
            if (i == s->cursor || i == lastcursor)
                goto do_render;
            if (i < lastscroll || i >= lastscroll + termheight - 3)
                goto do_render;
            continue;
        }

      do_render:;
        int y = i - s->scroll + 2;
        term_move(0, y);
        if (i >= s->nfiles) {
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
        if (i == s->cursor)
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

        for (char *col = s->columns; *col; ++col) {
            if (col != s->columns) writez(termfd, " │ ");
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
                        wrote += write_escaped(termfd, linkpath, (size_t)pathlen, color);
                        if (entry->d_isdir) {
                            writez(termfd, "/");
                            ++wrote;
                        }
                    }
                    while (wrote++ < colnamew - 1)
                        write(termfd, " ", 1);
                    break;
                }
            }
        }
        writez(termfd, " \033[0m\033[K"); // Reset color and attributes
    }

    static const char *help = "Press '?' to see key bindings ";
    term_move(0, termheight - 1);
    writez(termfd, "\033[K");
    term_move(MAX(0, termwidth - (int)strlen(help)), termheight - 1);
    writez(termfd, help);
    lastcursor = s->cursor;
    lastscroll = s->scroll;
    // TODO: show selection and dotfile setting and anything else?
}

int compare_files(void *r, const void *v1, const void *v2)
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

int find_file(bb_state_t *s, const char *name)
{
    for (int i = 0; i < s->nfiles; i++) {
        entry_t *e = s->files[i];
        if (strcmp(name[0] == '/' ? e->d_fullname : e->d_name, name) == 0)
            return i;
    }
    return -1;
}

void write_selection(int fd, char sep)
{
    for (entry_t *e = firstselected; e; e = e->next) {
        const char *p = e->d_fullname;
        while (*p) {
            const char *p2 = strchr(p, '\n');
            if (!p2) p2 = p + strlen(p);
            write(fd, p, (size_t)(p2 - p));
            if (*p2 == '\n' && sep == '\n')
                write(fd, "\\", 1);
            p = p2;
        }
        write(fd, &sep, 1);
    }
}

void clear_selection(void)
{
    for (entry_t *next, *e = firstselected; e; e = next) {
        next = e->next;
        if (!e->visible)
            free(e);
    }
    firstselected = NULL;
}

int select_file(entry_t *e)
{
    if (IS_SELECTED(e)) return 0;
    if (strcmp(e->d_name, "..") == 0) return 0;
    if (firstselected)
        firstselected->atme = &e->next;
    e->next = firstselected;
    e->atme = &firstselected;
    firstselected = e;
    return 1;
}

int deselect_file(entry_t *e)
{
    if (!IS_SELECTED(e)) return 0;
    LLREMOVE(e);
    return 1;
}

void set_cursor(bb_state_t *state, int newcur)
{
    if (newcur > state->nfiles - 1) newcur = state->nfiles - 1;
    if (newcur < 0) newcur = 0;
    state->cursor = newcur;
    if (state->nfiles <= termheight - 4)
        return;

    if (newcur < state->scroll + SCROLLOFF)
        state->scroll = newcur - SCROLLOFF;
    else if (newcur > state->scroll + (termheight-4) - SCROLLOFF)
        state->scroll = newcur - (termheight-4) + SCROLLOFF;
    int max_scroll = state->nfiles - (termheight-4) - 1;
    if (max_scroll < 0) max_scroll = 0;
    if (state->scroll > max_scroll) state->scroll = max_scroll;
    if (state->scroll < 0) state->scroll = 0;
}

void set_scroll(bb_state_t *state, int newscroll)
{
    newscroll = MIN(newscroll, state->nfiles - (termheight-4) - 1);
    newscroll = MAX(newscroll, 0);
    state->scroll = newscroll;

    int delta = newscroll - state->scroll;
    int oldcur = state->cursor;
    if (state->nfiles < termheight - 4) {
        newscroll = 0;
    } else {
        if (state->cursor > newscroll + (termheight-4) - SCROLLOFF && state->scroll < state->nfiles - (termheight-4) - 1)
            state->cursor = newscroll + (termheight-4) - SCROLLOFF;
        else if (state->cursor < newscroll + SCROLLOFF && state->scroll > 0)
            state->cursor = newscroll + SCROLLOFF;
    }
    state->scroll = newscroll;
    if (abs(state->cursor - oldcur) < abs(delta))
        state->cursor += delta - (state->cursor - oldcur);
    if (state->cursor > state->nfiles - 1) state->cursor = state->nfiles - 1;
    if (state->cursor < 0) state->cursor = 0;
}

void populate_files(bb_state_t *s, const char *path)
{
    if (!path) err("No path given");
    ino_t old_inode = 0;
    // Clear old files (if any)
    if (s->files) {
        old_inode = s->files[s->cursor]->d_ino;
        for (int i = 0; i < s->nfiles; i++) {
            entry_t *e = s->files[i];
            --e->visible;
            if (!IS_SELECTED(e))
                free(e);
        }
        free(s->files);
        s->files = NULL;
    }
    s->nfiles = 0;
    s->cursor = 0;
    if (strcmp(path, s->path) != 0) {
        s->scroll = 0;
        strcpy(s->path, path);
    }

    // Hash inode -> entry_t with linear probing
    int nselected = 0;
    for (entry_t *p = firstselected; p; p = p->next) ++nselected;
    int hashsize = 2 * nselected;
    entry_t **selecthash = NULL;
    if (nselected > 0) {
        selecthash = memcheck(calloc((size_t)hashsize, sizeof(entry_t*)));
        for (entry_t *p = firstselected; p; p = p->next) {
            int probe = ((int)p->d_ino) % hashsize;
            while (selecthash[probe])
                probe = (probe + 1) % hashsize;
            selecthash[probe] = p;
        }
    }

    DIR *dir = opendir(s->path);
    if (!dir)
        err("Couldn't open dir: %s", s->path);
    size_t pathlen = strlen(s->path);
    size_t filecap = 0;
    for (struct dirent *dp; (dp = readdir(dir)) != NULL; ) {
        if (dp->d_name[0] == '.' && dp->d_name[1] == '\0')
            continue;
        if (!s->showhidden && dp->d_name[0] == '.' && !(dp->d_name[1] == '.' && dp->d_name[2] == '\0'))
            continue;
        if ((size_t)s->nfiles >= filecap) {
            filecap += 100;
            s->files = memcheck(realloc(s->files, filecap*sizeof(entry_t*)));
        }

        // Hashed lookup from selected:
        if (nselected > 0) {
            for (int probe = ((int)dp->d_ino) % hashsize; selecthash[probe]; probe = (probe + 1) % hashsize) {
                if (selecthash[probe]->d_ino == dp->d_ino) {
                    ++selecthash[probe]->visible;
                    s->files[s->nfiles++] = selecthash[probe];
                    goto next_file;
                }
            }
        }

        entry_t *entry = memcheck(malloc(sizeof(entry_t) + pathlen + dp->d_namlen + 2));
        if (pathlen > MAX_PATH) err("Path is too big");
        strncpy(entry->d_fullname, s->path, pathlen);
        entry->d_fullname[pathlen] = '/';
        entry->d_name = &entry->d_fullname[pathlen + 1];
        strncpy(entry->d_name, dp->d_name, dp->d_namlen + 1);
        entry->d_ino = dp->d_ino;
        entry->d_reclen = dp->d_reclen;
        entry->d_type = dp->d_type;
        entry->d_isdir = dp->d_type == DT_DIR;
        ++entry->visible;
        if (!entry->d_isdir && entry->d_type == DT_LNK) {
            struct stat statbuf;
            if (stat(entry->d_fullname, &statbuf) == 0)
                entry->d_isdir = S_ISDIR(statbuf.st_mode);
        }
        entry->d_namlen = dp->d_namlen;
        entry->next = NULL; entry->atme = NULL;
        s->files[s->nfiles++] = entry;
      next_file:;
    }
    closedir(dir);
    free(selecthash);
    if (s->nfiles == 0) err("No files found (not even '..')");

    if (old_inode) {
        for (int i = 0; i < s->nfiles; i++) {
            if (s->files[i]->d_ino == old_inode) {
                set_cursor(s, i);
                break;
            }
        }
    }
}

void sort_files(bb_state_t *state)
{
    ino_t cursor_inode = state->files[state->cursor]->d_ino;
    qsort_r(&state->files[1], (size_t)(state->nfiles-1), sizeof(entry_t*), &state->sort, compare_files);
    if (DESCENDING(state->sort) == SORT_RANDOM) {
        entry_t **files = &state->files[1];
        int ndirs = 0, nents = state->nfiles - 1;
        for (int i = 0; i < nents; i++) {
            if (state->files[i]->d_isdir) ++ndirs;
            else break;
        }
        for (int i = 0; i < ndirs - 1; i++) {
            int j = i + rand() / (RAND_MAX / (ndirs - 1 - i));
            entry_t *tmp = files[j];
            files[j] = files[i];
            files[i] = tmp;
        }
        for (int i = ndirs; i < nents - 1; i++) {
            int j = i + rand() / (RAND_MAX / (nents - 1 - i));
            entry_t *tmp = files[j];
            files[j] = files[i];
            files[i] = tmp;
        }
    }
    for (int i = 0; i < state->nfiles; i++) {
        if (state->files[i]->d_ino == cursor_inode) {
            set_cursor(state, i);
            break;
        }
    }
}

entry_t *explore(const char *path)
{
    static long cmdpos = 0;

    init_term();
    alt_screen();
    hide_cursor();

    bb_state_t *state = new_state(NULL);
    if (!firststate) {
        firststate = state;
        state->atme = &firststate;
    }
    {
        char *real = realpath(path, NULL);
        if (!real || chdir(real))
            err("Not a valid path: %s\n", path);
        populate_files(state, real);
        free(real); // estate
    }
    sort_files(state);

  refresh:;

    int lastwidth = termwidth, lastheight = termheight;
    int lazy = 0;

  redraw:
    render(state, lazy);
    lazy = 1;

  next_input:
    if (termwidth != lastwidth || termheight != lastheight) {
        lastwidth = termwidth; lastheight = termheight;
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
                    populate_files(state, state->path);
                    sort_files(state);
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
                                    sort_files(state);
                                    cleanup_cmd();
                                    lazy = 0;
                                    goto refresh;
                            }
                            goto next_cmd;
                        case 'c': { // scroll:
                            // TODO: figure out the best version of this
                            int isdelta = value[0] == '+' || value[0] == '-';
                            int n = (int)strtol(value, &value, 10);
                            if (*value == '%')
                                n = (n * (value[1] == 'n' ? state->nfiles : termheight)) / 100;
                            if (isdelta)
                                set_scroll(state, state->scroll + n);
                            else
                                set_scroll(state, n);
                            goto next_cmd;
                        }

                        case 'p': // spread:
                            goto move;

                        case '\0': case 'e': // select:
                            lazy = 0;
                            if (strcmp(value, "*") == 0) {
                                for (int i = 0; i < state->nfiles; i++)
                                    select_file(state->files[i]);
                            } else {
                                int f = find_file(state, value);
                                if (f >= 0) select_file(state->files[f]);
                            }
                            goto next_cmd;
                    }
                case 'c':
                    switch (cmd[1]) {
                        case 'd': { // cd:
                            char *rpbuf = realpath(value, NULL);
                            if (!rpbuf) continue;
                            if (strcmp(rpbuf, state->path) == 0) {
                                free(rpbuf);
                                goto next_cmd;
                            }
                            if (chdir(rpbuf)) {
                                free(rpbuf);
                                goto next_cmd;
                            }
                            char *oldpath = memcheck(strdup(state->path));
                            populate_files(state, rpbuf);
                            sort_files(state);
                            free(rpbuf);
                            if (strcmp(value, "..") == 0) {
                                int f = find_file(state, oldpath);
                                if (f >= 0) set_cursor(state, f);
                            }
                            free(oldpath);
                            cleanup_cmd();
                            goto refresh;
                        }
                        case 'o': // cols:
                            for (char *col = value, *dst = state->columns;
                                 *col && dst < &state->columns[sizeof(state->columns)-1]; col++) {
                                *(dst++) = DESCENDING(*col);
                                *dst = '\0';
                            }
                            break;
                        case 'l': { // closetab:
                            if (!value) value = "+0";
                            bb_state_t *from = (value[0] == '+' || value[0] == '-') ? state : firststate;
                            int n = (int)strtol(value, &value, 10);
                            bb_state_t *s;
                            if (n < 0) {
                                for (s = from; n++; s = PREV_STATE(s))
                                    if (!s) goto next_cmd;
                            } else {
                                for (s = from; n--; s = s->next)
                                    if (!s) goto next_cmd;
                            }
                            if (state == s) {
                                if (s->next)
                                    state = s->next;
                                else if (PREV_STATE(s))
                                    state = PREV_STATE(s);
                                if (chdir(state->path))
                                    err("Could not cd to '%s'", state->path);
                            }
                            delete_state(s);
                            goto next_cmd;
                        }
                    }
                case 'n': { // newtab
                    bb_state_t *ns = new_state(state);
                    if (state->next)
                        state->next->atme = &ns->next;
                    ns->next = state->next;
                    ns->atme = &state->next;
                    state->next = ns;
                    state = ns;
                    if (chdir(ns->path))
                        err("Could not cd to '%s'", ns->path);
                    goto refresh;
                }
                case 't': { // toggle:, tab:
                    if (cmd[1] == 'o') { // toggle:
                        lazy = 0;
                        int f = find_file(state, value);
                        if (f >= 0) {
                            entry_t *e = state->files[f];
                            if (IS_SELECTED(e)) deselect_file(e);
                            else select_file(e);
                        }
                        goto next_cmd;
                    } else if (cmd[1] == 'a') { // tab:
                        bb_state_t *from = (value[0] == '+' || value[0] == '-') ? state : firststate;
                        int n = (int)strtol(value, &value, 10);
                        bb_state_t *s = from;
                        if (n < 0) {
                            while (s && n++) s = PREV_STATE(s);
                        } else {
                            while (s && n--) s = s->next;
                        }
                        if (!s || s == state)
                            goto next_cmd;

                        state = s;
                        if (chdir(s->path))
                            err("Could not cd to '%s'", s->path);
                        goto refresh;
                    }
                }
                case 'd': { // deselect:, dots:
                    if (cmd[1] == 'o') { // dots:
                        int requested = value ? (value[0] == 'y') : state->showhidden ^ 1;
                        if (requested == state->showhidden)
                            goto next_cmd;
                        state->showhidden = requested;
                        populate_files(state, state->path);
                        sort_files(state);
                        goto refresh;
                    } else if (value) { // deselect:
                        lazy = 0;
                        if (strcmp(value, "*") == 0) {
                            clear_selection();
                        } else {
                            int f = find_file(state, value);
                            if (f >= 0)
                                select_file(state->files[f]);
                        }
                        goto next_cmd;
                    }
                }
                case 'g': { // goto:
                    int f = find_file(state, value);
                    if (f >= 0) {
                        set_cursor(state, f);
                        goto next_cmd;
                    }
                    char *lastslash = strrchr(value, '/');
                    if (!lastslash) goto next_cmd;
                    *lastslash = '\0'; // Split in two
                    char *real = realpath(path, NULL);
                    if (!real || chdir(real))
                        err("Not a valid path: %s\n", path);
                    populate_files(state, real);
                    free(real); // estate
                    if (lastslash[1]) {
                        f = find_file(state, value);
                        if (f >= 0) set_cursor(state, f);
                    }
                    cleanup_cmd();
                    goto refresh;
                }
                case 'm': { // move:
                  move:;
                    int oldcur = state->cursor;
                    int isdelta = value[0] == '-' || value[0] == '+';
                    int n = (int)strtol(value, &value, 10);
                    if (*value == '%')
                        n = (n * (value[1] == 'n' ? state->nfiles : termheight)) / 100;
                    if (isdelta) set_cursor(state, state->cursor + n);
                    else set_cursor(state, n);
                    if (cmd[0] == 's') { // spread:
                        int sel = IS_SELECTED(state->files[oldcur]);
                        for (int i = state->cursor; i != oldcur; i += (oldcur > i ? 1 : -1)) {
                            if (sel && !IS_SELECTED(state->files[i]))
                                select_file(state->files[i]);
                            else if (!sel && IS_SELECTED(state->files[i]))
                                deselect_file(state->files[i]);
                        }
                        lazy &= abs(oldcur - state->cursor) <= 1;
                    }
                    goto next_cmd;
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
            double dt_ms = 1e3*(double)(clicktime.tv_sec - lastclick.tv_sec);
            dt_ms += 1e-6*(double)(clicktime.tv_nsec - lastclick.tv_nsec);
            lastclick = clicktime;
            if (mouse_y == 1) {
                // Sort column:
                int x = 1;
                for (char *col = state->columns; *col; ++col) {
                    if (col != state->columns) x += 3;
                    switch (*col) {
                        case 's': x += colsizew; break;
                        case 'p': x += colpermw; break;
                        case 'm': case 'a': case 'c':
                            x += coldatew; break;
                        case 'n': x += colnamew; break;
                    }
                    if (x >= mouse_x) {
                        if (DESCENDING(state->sort) == *col)
                            state->sort ^= SORT_DESCENDING;
                        else
                            state->sort = *col;
                        sort_files(state);
                        goto refresh;
                    }
                }
                goto next_input;
            } else if (mouse_y >= 2 && state->scroll + (mouse_y - 2) < state->nfiles) {
                int clicked = state->scroll + (mouse_y - 2);
                if (mouse_x == 0) {
                    if (IS_SELECTED(state->files[clicked]))
                        deselect_file(state->files[clicked]);
                    else
                        select_file(state->files[clicked]);
                    goto redraw;
                }
                set_cursor(state, clicked);
                if (dt_ms <= 200)
                    key = KEY_MOUSE_DOUBLE_LEFT;
                goto user_bindings;
            }
            break;
        }

        case KEY_CTRL_C:
            cleanup_and_exit(SIGINT);
            goto quit; // Unreachable

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

        case KEY_CTRL_H: {
            term_move(0,termheight-1);
            writez(termfd, "\033[K\033[33;1mPress any key...\033[0m");
            while ((key = term_getkey(termfd, &mouse_x, &mouse_y, 1000)) == -1)
                ;
            term_move(0,termheight-1);
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
            term_move(0, termheight-1);
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

    default_screen();
    show_cursor();
    close_term();
    return firstselected;
}

void print_bindings(int verbose)
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
    char *initial_path = NULL, *depthstr;
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
        // Set up environment variables
        depthstr = getenv("BB_DEPTH");
        int depth = depthstr ? atoi(depthstr) : 0;
        if (asprintf(&depthstr, "%d", depth + 1) < 0)
            err("Allocation failure");
        setenv("BB_DEPTH", depthstr, 1);
        setenv("BBCMD", cmdfilename, 1);
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

    signal(SIGTERM, cleanup_and_exit);
    signal(SIGINT, cleanup_and_exit);
    signal(SIGXCPU, cleanup_and_exit);
    signal(SIGXFSZ, cleanup_and_exit);
    signal(SIGVTALRM, cleanup_and_exit);
    signal(SIGPROF, cleanup_and_exit);

    char *real = realpath(initial_path, NULL);
    if (!real || chdir(real)) err("Not a valid path: %s\n", initial_path);
    explore(real);
    free(real);

    if (firstselected && print_selection) {
        write_selection(STDOUT_FILENO, sep);
    }
    if (print_dir) {
        printf("%s\n", initial_path);
    }

    return 0;
}

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
