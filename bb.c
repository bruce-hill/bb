/*
 * Bitty Browser (bb)
 * Copyright 2019 Bruce Hill
 * Released under the MIT license
 */
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
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
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "keys.h"

#define BB_VERSION "0.9.0"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MAX(a,b) ((a) < (b) ? (b) : (a))
#define MIN(a,b) ((a) > (b) ? (b) : (a))
#define IS_SELECTED(p) (((p)->atme) != NULL)

// Terminal escape sequences:
#define CSI           "\033["
#define T_WRAP        "7"
#define T_SHOW_CURSOR "25"
#define T_MOUSE_XY    "1000"
#define T_MOUSE_CELL  "1002"
#define T_MOUSE_SGR   "1006"
#define T_MOUSE_URXVT "1015"
#define T_ALT_SCREEN  "1049"
#define T_ON(opt)  CSI "?" opt "h"
#define T_OFF(opt) CSI "?" opt "l"

static const char *T_ENTER_BBMODE =  T_OFF(T_WRAP ";" T_MOUSE_URXVT) T_ON(T_MOUSE_XY ";" T_MOUSE_CELL ";" T_MOUSE_SGR);
static const char *T_LEAVE_BBMODE =  T_OFF(T_MOUSE_XY ";" T_MOUSE_CELL ";" T_MOUSE_SGR ";" T_ALT_SCREEN) T_ON(T_WRAP);
static const char *T_LEAVE_BBMODE_PARTIAL = T_OFF(T_MOUSE_XY ";" T_MOUSE_CELL ";" T_MOUSE_SGR) T_ON(T_WRAP);

#define move_cursor(f, x, y) fprintf((f), CSI "%d;%dH", (int)(y)+1, (int)(x)+1)

#define err(...) do { \
    close_term(); \
    fputs(T_OFF(T_ALT_SCREEN), stdout); \
    fflush(stdout); \
    fprintf(stderr, __VA_ARGS__); \
    if (errno) fprintf(stderr, "\n%s", strerror(errno)); \
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

/* entry_t uses intrusive linked lists.  This means entries can only belong to
 * one list at a time, in this case the list of selected entries. 'atme' is an
 * indirect pointer to either the 'next' field of the previous list member, or
 * the variable that points to the first list member. In other words,
 * item->next->atme == &item->next and firstitem->atme == &firstitem.
 */
typedef struct entry_s {
    struct entry_s *next, **atme;
    int visible;
    int d_isdir : 1, d_escname : 1, d_esclink : 1;
    ino_t      d_ino;
    __uint8_t  d_type;
    struct stat info;
    char *d_name, *d_linkname;
    char d_fullname[1];
} entry_t;

typedef struct bb_s {
    entry_t **files;
    entry_t *firstselected;
    char path[PATH_MAX];
    int nfiles;
    int scroll, cursor;
    int showhidden;
    char columns[16];
    char sort;
    char *marks[128]; // Mapping from key to directory
} bb_t;

// Functions
static void update_term_size(int sig);
static void init_term(void);
static void cleanup_and_exit(int sig);
static void close_term(void);
static void* memcheck(void *p);
static int unprintables(char *s);
static int run_cmd_on_selection(bb_t *bb, const char *cmd);
static void render(bb_t *bb, int lazy);
static int compare_files(void *r, const void *v1, const void *v2);
static int find_file(bb_t *bb, const char *name);
static void clear_selection(bb_t *bb);
static int select_file(bb_t *bb, entry_t *e);
static int deselect_file(bb_t *bb, entry_t *e);
static void set_cursor(bb_t *bb, int i);
static void set_scroll(bb_t *bb, int i);
static void populate_files(bb_t *bb, const char *path);
static void sort_files(bb_t *bb);
static void explore(bb_t *bb, const char *path);
static void print_bindings(int verbose);

// Config options
extern binding_t bindings[];
extern const char *startupcmds[];

// Global variables
static struct termios orig_termios, bb_termios;
static FILE *tty_out = NULL, *tty_in = NULL;
static int termwidth, termheight;
static int mouse_x, mouse_y;
static char *cmdfilename = NULL;
static const int colsizew = 7, coldatew = 19, colpermw = 4;
static int colnamew;
static struct timespec lastclick = {0, 0};


void update_term_size(int sig)
{
    (void)sig;
    struct winsize sz = {0};
    ioctl(fileno(tty_in), TIOCGWINSZ, &sz);
    termwidth = sz.ws_col;
    termheight = sz.ws_row;
}

void init_term(void)
{
    tty_in = fopen("/dev/tty", "r");
    tty_out = fopen("/dev/tty", "w");
    tcgetattr(fileno(tty_out), &orig_termios);
    memcpy(&bb_termios, &orig_termios, sizeof(bb_termios));
    bb_termios.c_iflag &= ~(unsigned long)(
        IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    bb_termios.c_oflag &= (unsigned long)~OPOST;
    bb_termios.c_lflag &= (unsigned long)~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    bb_termios.c_cflag &= (unsigned long)~(CSIZE | PARENB);
    bb_termios.c_cflag |= (unsigned long)CS8;
    bb_termios.c_cc[VMIN] = 0;
    bb_termios.c_cc[VTIME] = 0;
    tcsetattr(fileno(tty_out), TCSAFLUSH, &bb_termios);
    update_term_size(0);
    signal(SIGWINCH, update_term_size);
    // Initiate mouse tracking and disable text wrapping:
    fputs(T_ENTER_BBMODE, tty_out);
}

void cleanup_and_exit(int sig)
{
    (void)sig;
    close_term();
    fputs(T_OFF(T_ALT_SCREEN), stdout);
    fflush(stdout);
    unlink(cmdfilename);
    exit(EXIT_FAILURE);
}

void close_term(void)
{
    if (tty_out) {
        tcsetattr(fileno(tty_out), TCSAFLUSH, &orig_termios);
        fclose(tty_out);
        tty_out = NULL;
        fclose(tty_in);
        tty_in = NULL;
    }
    fputs(T_LEAVE_BBMODE_PARTIAL, stdout);
    fflush(stdout);
    signal(SIGWINCH, SIG_IGN);
}

void* memcheck(void *p)
{
    if (!p) err("Allocation failure");
    return p;
}

int unprintables(char *s)
{
    int count = 0;
    for ( ; *s; ++s) count += *s < ' ';
    return count;
}

int run_cmd_on_selection(bb_t *bb, const char *cmd)
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
#ifdef __APPLE__
        args[i++] = "--";
#endif
        entry_t *first = bb->firstselected ? bb->firstselected : bb->files[bb->cursor];
        for (entry_t *e = first; e; e = e->next) {
            if (i >= space) {
                space += 100;
                args = memcheck(realloc(args, space*sizeof(char*)));
            }
            args[i++] = e->d_fullname;
        }
        args[i] = NULL;

        setenv("BBCURSOR", bb->files[bb->cursor]->d_name, 1);
        setenv("BBFULLCURSOR", bb->files[bb->cursor]->d_fullname, 1);

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

static void fputs_escaped(FILE *f, const char *str, const char *color)
{
    static const char *escapes = "       abtnvfr             e";
    for (const char *c = str; *c; ++c) {
        if (*c < 0) {
            fputc(*c, f);
        } else if (*c > 0 && *c <= '\x1b' && escapes[(int)*c] != ' ') { // "\n", etc.
            fprintf(f, "\033[31m\\%c%s", escapes[(int)*c], color);
        } else if (!(' ' <= *c && *c <= '~')) { // "\x02", etc.
            fprintf(f, "\033[31m\\x%02X%s", *c, color);
        } else {
            fputc(*c, f);
        }
    }
}


void render(bb_t *bb, int lazy)
{
    static int lastcursor = -1, lastscroll = -1;
    char buf[64];
    if (lastcursor == -1 || lastscroll == -1)
        lazy = 0;

    if (lazy) {
        // Use terminal scrolling:
        if (lastscroll > bb->scroll) {
            fprintf(tty_out, "\033[3;%dr\033[%dT\033[1;%dr", termheight-1, lastscroll - bb->scroll, termheight);
        } else if (lastscroll < bb->scroll) {
            fprintf(tty_out, "\033[3;%dr\033[%dS\033[1;%dr", termheight-1, bb->scroll - lastscroll, termheight);
        }
    }
    colnamew = termwidth - 1;
    int namecols = 0;
    for (char *col = bb->columns; *col; ++col) {
        switch (*col) {
            case 's': colnamew -= colsizew;
                      break;
            case 'm': case 'c': case 'a':
                      colnamew -= coldatew;
                      break;
            case 'p': colnamew -= colpermw;
                      break;
            case 'n': namecols++;
                      break;
        }
    }
    colnamew -= 3*strlen(bb->columns);
    colnamew /= namecols;

    if (!lazy) {
        // Path
        move_cursor(tty_out, 0, 0);
        const char *color = "\033[0;1;37m";
        fputs_escaped(tty_out, bb->path, color);
        fputs(" \033[K\033[0m", tty_out);

        // Columns
        move_cursor(tty_out, 0,1);
        fputs("\033[41m \033[0;44;30m", tty_out);
        for (char *col = bb->columns; *col; ++col) {
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
            if (col != bb->columns) fputs(" │ ", tty_out);
            if (DESCENDING(bb->sort) != *col) fputs(" ", tty_out);
            else if (IS_REVERSED(bb->sort)) fputs(RSORT_INDICATOR, tty_out);
            else fputs(SORT_INDICATOR, tty_out);
            for (ssize_t i = fputs(colname, tty_out); i < colwidth-1; i++)
                fputc(' ', tty_out);
        }
        fputs(" \033[K\033[0m", tty_out);
    }

    entry_t **files = bb->files;
    for (int i = bb->scroll; i < bb->scroll + termheight - 3; i++) {
        if (lazy) {
            if (i == bb->cursor || i == lastcursor)
                goto do_render;
            if (i < lastscroll || i >= lastscroll + termheight - 3)
                goto do_render;
            continue;
        }

        int y;
      do_render:
        y = i - bb->scroll + 2;
        move_cursor(tty_out, 0, y);
        if (i >= bb->nfiles) {
            fputs("\033[K", tty_out);
            continue;
        }

        entry_t *entry = files[i];
        { // Selection box:
            if (IS_SELECTED(entry))
                fputs("\033[41m \033[0m", tty_out);
            else
                fputs(" ", tty_out);
        }

        const char *color;
        if (i == bb->cursor)
            color = CURSOR_COLOR;
        else if (entry->d_isdir && entry->d_type == DT_LNK)
            color = LINKDIR_COLOR;
        else if (entry->d_isdir)
            color = DIR_COLOR;
        else if (entry->d_type == DT_LNK)
            color = LINK_COLOR;
        else
            color = NORMAL_COLOR;
        fputs(color, tty_out);

        int x = 1;
        for (char *col = bb->columns; *col; ++col) {
            fprintf(tty_out, "\033[%d;%dH\033[K", y+1, x+1);
            if (col != bb->columns) {
                if (i == bb->cursor) fputs(" │", tty_out);
                else fputs(" \033[37;2m│\033[22m", tty_out);
                fputs(color, tty_out);
                fputs(" ", tty_out);
                x += 3;
            }
            switch (*col) {
                case 's': {
                    int j = 0;
                    const char* units = "BKMGTPEZY";
                    double bytes = (double)entry->info.st_size;
                    while (bytes > 1024) {
                        bytes /= 1024;
                        j++;
                    }
                    fprintf(tty_out, "%6.*f%c", j > 0 ? 1 : 0, bytes, units[j]);
                    x += colsizew;
                    break;
                }

                case 'm':
                    strftime(buf, sizeof(buf), "%l:%M%p %b %e %Y", localtime(&(entry->info.st_mtime)));
                    fputs(buf, tty_out);
                    x += coldatew;
                    break;

                case 'c':
                    strftime(buf, sizeof(buf), "%l:%M%p %b %e %Y", localtime(&(entry->info.st_ctime)));
                    fputs(buf, tty_out);
                    x += coldatew;
                    break;

                case 'a':
                    strftime(buf, sizeof(buf), "%l:%M%p %b %e %Y", localtime(&(entry->info.st_atime)));
                    fputs(buf, tty_out);
                    x += coldatew;
                    break;

                case 'p':
                    buf[0] = ' ';
                    buf[1] = '0' + ((entry->info.st_mode >> 6) & 7);
                    buf[2] = '0' + ((entry->info.st_mode >> 3) & 7);
                    buf[3] = '0' + ((entry->info.st_mode >> 0) & 7);
                    buf[4] = '\0';
                    fputs(buf, tty_out);
                    x += colpermw;
                    break;

                case 'n': {
                    if (entry->d_escname)
                        fputs_escaped(tty_out, entry->d_name, color);
                    else
                        fputs(entry->d_name, tty_out);
                    if (entry->d_isdir) {
                        fputs("/", tty_out);
                    }

                    if (entry->d_linkname) {
                        fputs("\033[2m -> ", tty_out);
                        if (entry->d_esclink)
                            fputs_escaped(tty_out, entry->d_linkname, color);
                        else
                            fputs(entry->d_linkname, tty_out);
                        if (entry->d_isdir) {
                            fputs("/", tty_out);
                        }
                        fputs("\033[22m", tty_out);
                    }
                    x += colnamew;
                    break;
                }
            }
        }
        fputs(" \033[K\033[0m", tty_out); // Reset color and attributes
    }

    static const char *help = "Press '?' to see key bindings ";
    move_cursor(tty_out, 0, termheight - 1);
    fputs("\033[K", tty_out);
    move_cursor(tty_out, MAX(0, termwidth - (int)strlen(help)), termheight - 1);
    fputs(help, tty_out);
    lastcursor = bb->cursor;
    lastscroll = bb->scroll;
    fflush(tty_out);
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
                diff = (int)(p1 - f1->d_name) - (int)(p2 - f2->d_name);
                if (diff != 0)
                    return diff*sign;
                if (n1 != n2)
                    return (n1 > n2 ? 1 : -1)*sign;
            } else if (diff) {
                return diff*sign;
            } else {
                ++p1; ++p2;
            }
        }
        return (*p1 - *p2)*sign;
    }
    switch (sort) {
        case SORT_PERM:
            return -((f1->info.st_mode & 0x3FF) - (f2->info.st_mode & 0x3FF))*sign;
        case SORT_SIZE:
            return f1->info.st_size > f2->info.st_size ? -sign : sign;
        case SORT_MTIME:
            if (f1->info.st_mtimespec.tv_sec == f2->info.st_mtimespec.tv_sec)
                return f1->info.st_mtimespec.tv_nsec > f2->info.st_mtimespec.tv_nsec ? -sign : sign;
            else
                return f1->info.st_mtimespec.tv_sec > f2->info.st_mtimespec.tv_sec ? -sign : sign;
        case SORT_CTIME:
            if (f1->info.st_ctimespec.tv_sec == f2->info.st_ctimespec.tv_sec)
                return f1->info.st_ctimespec.tv_nsec > f2->info.st_ctimespec.tv_nsec ? -sign : sign;
            else
                return f1->info.st_ctimespec.tv_sec > f2->info.st_ctimespec.tv_sec ? -sign : sign;
        case SORT_ATIME:
            if (f1->info.st_atimespec.tv_sec == f2->info.st_atimespec.tv_sec)
                return f1->info.st_atimespec.tv_nsec > f2->info.st_atimespec.tv_nsec ? -sign : sign;
            else
                return f1->info.st_atimespec.tv_sec > f2->info.st_atimespec.tv_sec ? -sign : sign;
    }
    return 0;
}

int find_file(bb_t *bb, const char *name)
{
    for (int i = 0; i < bb->nfiles; i++) {
        entry_t *e = bb->files[i];
        if (strcmp(name[0] == '/' ? e->d_fullname : e->d_name, name) == 0)
            return i;
    }
    return -1;
}

void clear_selection(bb_t *bb)
{
    for (entry_t *next, *e = bb->firstselected; e; e = next) {
        next = e->next;
        if (!e->visible)
            free(e);
    }
    bb->firstselected = NULL;
}

int select_file(bb_t *bb, entry_t *e)
{
    if (IS_SELECTED(e)) return 0;
    if (strcmp(e->d_name, "..") == 0) return 0;
    if (bb->firstselected)
        bb->firstselected->atme = &e->next;
    e->next = bb->firstselected;
    e->atme = &bb->firstselected;
    bb->firstselected = e;
    return 1;
}

int deselect_file(bb_t *bb, entry_t *e)
{
    (void)bb;
    if (!IS_SELECTED(e)) return 0;
    if (e->next)
        e->next->atme = e->atme;
    *(e->atme) = e->next;
    e->next = NULL;
    e->atme = NULL;
    return 1;
}

void set_cursor(bb_t *bb, int newcur)
{
    if (newcur > bb->nfiles - 1) newcur = bb->nfiles - 1;
    if (newcur < 0) newcur = 0;
    bb->cursor = newcur;
    if (bb->nfiles <= termheight - 4)
        return;

    if (newcur < bb->scroll + SCROLLOFF)
        bb->scroll = newcur - SCROLLOFF;
    else if (newcur > bb->scroll + (termheight-4) - SCROLLOFF)
        bb->scroll = newcur - (termheight-4) + SCROLLOFF;
    int max_scroll = bb->nfiles - (termheight-4) - 1;
    if (max_scroll < 0) max_scroll = 0;
    if (bb->scroll > max_scroll) bb->scroll = max_scroll;
    if (bb->scroll < 0) bb->scroll = 0;
}

void set_scroll(bb_t *bb, int newscroll)
{
    newscroll = MIN(newscroll, bb->nfiles - (termheight-4) - 1);
    newscroll = MAX(newscroll, 0);
    bb->scroll = newscroll;

    int delta = newscroll - bb->scroll;
    int oldcur = bb->cursor;
    if (bb->nfiles < termheight - 4) {
        newscroll = 0;
    } else {
        if (bb->cursor > newscroll + (termheight-4) - SCROLLOFF && bb->scroll < bb->nfiles - (termheight-4) - 1)
            bb->cursor = newscroll + (termheight-4) - SCROLLOFF;
        else if (bb->cursor < newscroll + SCROLLOFF && bb->scroll > 0)
            bb->cursor = newscroll + SCROLLOFF;
    }
    bb->scroll = newscroll;
    if (abs(bb->cursor - oldcur) < abs(delta))
        bb->cursor += delta - (bb->cursor - oldcur);
    if (bb->cursor > bb->nfiles - 1) bb->cursor = bb->nfiles - 1;
    if (bb->cursor < 0) bb->cursor = 0;
}

void populate_files(bb_t *bb, const char *path)
{
    ino_t old_inode = 0;
    // Clear old files (if any)
    if (bb->files) {
        old_inode = bb->files[bb->cursor]->d_ino;
        for (int i = 0; i < bb->nfiles; i++) {
            entry_t *e = bb->files[i];
            --e->visible;
            if (!IS_SELECTED(e))
                free(e);
        }
        free(bb->files);
        bb->files = NULL;
    }
    int old_cursor = bb->cursor;
    int old_scroll = bb->scroll;
    bb->nfiles = 0;
    bb->cursor = 0;

    if (path == NULL)
        return;

    int samedir = strcmp(path, bb->path) == 0;
    if (!samedir)
        strcpy(bb->path, path);

    // Hash inode -> entry_t with linear probing
    int nselected = 0;
    for (entry_t *p = bb->firstselected; p; p = p->next) ++nselected;
    int hashsize = 2 * nselected;
    entry_t **selecthash = NULL;
    if (nselected > 0) {
        selecthash = memcheck(calloc((size_t)hashsize, sizeof(entry_t*)));
        for (entry_t *p = bb->firstselected; p; p = p->next) {
            int probe = ((int)p->d_ino) % hashsize;
            while (selecthash[probe])
                probe = (probe + 1) % hashsize;
            selecthash[probe] = p;
        }
    }

    DIR *dir = opendir(bb->path);
    if (!dir)
        err("Couldn't open dir: %s", bb->path);
    size_t pathlen = strlen(bb->path);
    size_t filecap = 0;
    char linkbuf[PATH_MAX+1];
    for (struct dirent *dp; (dp = readdir(dir)) != NULL; ) {
        if (dp->d_name[0] == '.' && dp->d_name[1] == '\0')
            continue;
        if (!bb->showhidden && dp->d_name[0] == '.' && !(dp->d_name[1] == '.' && dp->d_name[2] == '\0'))
            continue;
        if ((size_t)bb->nfiles >= filecap) {
            filecap += 100;
            bb->files = memcheck(realloc(bb->files, filecap*sizeof(entry_t*)));
        }

        // Hashed lookup from selected:
        if (nselected > 0) {
            for (int probe = ((int)dp->d_ino) % hashsize; selecthash[probe]; probe = (probe + 1) % hashsize) {
                if (selecthash[probe]->d_ino == dp->d_ino) {
                    ++selecthash[probe]->visible;
                    bb->files[bb->nfiles++] = selecthash[probe];
                    goto next_file;
                }
            }
        }

        ssize_t linkpathlen = -1;
        linkbuf[0] = 0;
        int d_esclink = 0;
        if (dp->d_type == DT_LNK) {
            linkpathlen = readlink(dp->d_name, linkbuf, sizeof(linkbuf));
            linkbuf[linkpathlen] = 0;
            if (linkpathlen > 0)
                d_esclink = unprintables(linkbuf) > 0;
        }

        entry_t *entry = memcheck(calloc(sizeof(entry_t) + pathlen + strlen(dp->d_name) + 2 + (size_t)(linkpathlen + 1), 1));
        if (pathlen > PATH_MAX) err("Path is too big");
        strncpy(entry->d_fullname, bb->path, pathlen);
        entry->d_fullname[pathlen] = '/';
        entry->d_name = &entry->d_fullname[pathlen + 1];
        strcpy(entry->d_name, dp->d_name);
        if (linkpathlen >= 0) {
            entry->d_linkname = entry->d_name + strlen(dp->d_name) + 2;
            strncpy(entry->d_linkname, linkbuf, linkpathlen+1);
        }
        entry->d_ino = dp->d_ino;
        entry->d_type = dp->d_type;
        entry->d_isdir = dp->d_type == DT_DIR;
        ++entry->visible;
        if (!entry->d_isdir && entry->d_type == DT_LNK) {
            struct stat statbuf;
            if (stat(entry->d_fullname, &statbuf) == 0)
                entry->d_isdir = S_ISDIR(statbuf.st_mode);
        }
        entry->next = NULL; entry->atme = NULL;
        entry->d_escname = unprintables(entry->d_name) > 0;
        entry->d_esclink = d_esclink;
        lstat(entry->d_fullname, &entry->info);
        bb->files[bb->nfiles++] = entry;
      next_file:
        continue;
    }
    closedir(dir);
    free(selecthash);
    if (bb->nfiles == 0) err("No files found (not even '..')");

    sort_files(bb);
    if (samedir) {
        if (old_inode) {
            for (int i = 0; i < bb->nfiles; i++) {
                if (bb->files[i]->d_ino == old_inode) {
                    set_scroll(bb, old_scroll);
                    set_cursor(bb, i);
                    return;
                }
            }
        }
        set_cursor(bb, old_cursor);
        set_scroll(bb, old_scroll);
    } else {
        set_cursor(bb, 0);
    }
}

void sort_files(bb_t *bb)
{
    ino_t cursor_inode = bb->files[bb->cursor]->d_ino;
    qsort_r(&bb->files[1], (size_t)(bb->nfiles-1), sizeof(entry_t*), &bb->sort, compare_files);
    if (DESCENDING(bb->sort) == SORT_RANDOM) {
        entry_t **files = &bb->files[1];
        int ndirs = 0, nents = bb->nfiles - 1;
        for (int i = 0; i < nents; i++) {
            if (bb->files[i]->d_isdir) ++ndirs;
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
    for (int i = 0; i < bb->nfiles; i++) {
        if (bb->files[i]->d_ino == cursor_inode) {
            set_cursor(bb, i);
            break;
        }
    }
}

static enum { BB_NOP = 0, BB_INVALID, BB_REFRESH, BB_DIRTY, BB_QUIT }
execute_cmd(bb_t *bb, const char *cmd)
{
    char *value = strchr(cmd, ':');
    if (value) ++value;
    switch (cmd[0]) {
        case 'r': { // refresh
            populate_files(bb, bb->path);
            return BB_REFRESH;
        }
        case 'q': // quit
            return BB_QUIT;
        case 's': // sort:, select:, scroll:, spread:
            switch (cmd[1]) {
                case 'o': // sort:
                    if (!value) return BB_INVALID;
                    switch (*value) {
                        case 'n': case 'N': case 's': case 'S':
                        case 'p': case 'P': case 'm': case 'M':
                        case 'c': case 'C': case 'a': case 'A':
                        case 'r': case 'R':
                            bb->sort = *value;
                            sort_files(bb);
                            return BB_REFRESH;
                    }
                    return BB_INVALID;
                case 'c': { // scroll:
                    if (!value) return BB_INVALID;
                    // TODO: figure out the best version of this
                    int isdelta = value[0] == '+' || value[0] == '-';
                    int n = (int)strtol(value, &value, 10);
                    if (*value == '%')
                        n = (n * (value[1] == 'n' ? bb->nfiles : termheight)) / 100;
                    if (isdelta)
                        set_scroll(bb, bb->scroll + n);
                    else
                        set_scroll(bb, n);
                    return BB_NOP;
                }

                case 'p': // spread:
                    goto move;

                case '\0': case 'e': // select:
                    if (!value) value = bb->files[bb->cursor]->d_name;
                    if (strcmp(value, "*") == 0) {
                        for (int i = 0; i < bb->nfiles; i++)
                            select_file(bb, bb->files[i]);
                    } else {
                        int f = find_file(bb, value);
                        if (f >= 0) select_file(bb, bb->files[f]);
                    }
                    return BB_DIRTY;
            }
        case 'c':
            switch (cmd[1]) {
                case 'd': { // cd:
                    char pbuf[PATH_MAX];
                  cd:
                    if (!value) return BB_INVALID;
                    if (value[0] == '~') {
                        char *home;
                        if (!(home = getenv("HOME")))
                            return BB_INVALID;
                        strcpy(pbuf, home);
                        strcat(pbuf, value+1);
                        value = pbuf;
                    }
                    char *rpbuf = realpath(value, NULL);
                    if (!rpbuf) return BB_INVALID;
                    if (strcmp(rpbuf, bb->path) == 0) {
                        free(rpbuf);
                        return BB_NOP;
                    }
                    if (chdir(rpbuf)) {
                        free(rpbuf);
                        return BB_INVALID;
                    }
                    char *oldpath = memcheck(strdup(bb->path));
                    populate_files(bb, rpbuf);
                    free(rpbuf);
                    if (strcmp(value, "..") == 0) {
                        int f = find_file(bb, oldpath);
                        if (f >= 0) set_cursor(bb, f);
                    }
                    free(oldpath);
                    return BB_REFRESH;
                }
                case 'o': // cols:
                    if (!value) return BB_INVALID;
                    for (int i = 0; i < (int)sizeof(bb->columns)-1 && value[i]; i++) {
                        bb->columns[i] = value[i];
                        bb->columns[i+1] = '\0';
                    }
                    return BB_REFRESH;
            }
        case 't': { // toggle:
            if (!value) value = bb->files[bb->cursor]->d_name;
            int f = find_file(bb, value);
            if (f < 0) return BB_INVALID;
            entry_t *e = bb->files[f];
            if (IS_SELECTED(e)) deselect_file(bb, e);
            else select_file(bb, e);
            return f == bb->cursor ? BB_NOP : BB_DIRTY;
        }
        case 'd': { // deselect:, dots:
            if (cmd[1] == 'o') { // dots:
                int requested = value ? (value[0] == 'y') : bb->showhidden ^ 1;
                if (requested == bb->showhidden)
                    return BB_INVALID;
                bb->showhidden = requested;
                populate_files(bb, bb->path);
                return BB_REFRESH;
            } else if (value) { // deselect:
                if (!value) value = bb->files[bb->cursor]->d_name;
                if (strcmp(value, "*") == 0) {
                    clear_selection(bb);
                    return BB_DIRTY;
                } else {
                    int f = find_file(bb, value);
                    if (f >= 0)
                        select_file(bb, bb->files[f]);
                    return f == bb->cursor ? BB_NOP : BB_DIRTY;
                }
            }
            return BB_INVALID;
        }
        case 'g': { // goto:
            if (!value) return BB_INVALID;
            int f = find_file(bb, value);
            if (f >= 0) {
                set_cursor(bb, f);
                return BB_NOP;
            }
            char *path = memcheck(strdup(value));
            char *lastslash = strrchr(path, '/');
            if (!lastslash) return BB_INVALID;
            *lastslash = '\0'; // Split in two
            char *real = realpath(path, NULL);
            if (!real || chdir(real))
                err("Not a valid path: %s\n", path);
            populate_files(bb, real);
            free(real); // estate
            if (lastslash[1]) {
                f = find_file(bb, lastslash + 1);
                if (f >= 0) set_cursor(bb, f);
            }
            return BB_REFRESH;
        }
        case 'm': { // move:, mark:
            switch (cmd[1]) {
                case 'a': { // mark:
                    if (!value) return BB_INVALID;
                    char key = value[0];
                    if (key < 0) return BB_INVALID;
                    value = strchr(value, '=');
                    if (!value) value = bb->path;
                    else ++value;
                    if (bb->marks[(int)key])
                        free(bb->marks[(int)key]);
                    bb->marks[(int)key] = memcheck(strdup(value));
                    return BB_NOP;
                }
                default: { // move:
                    int oldcur, isdelta, n;
                  move:
                    if (!value) return BB_INVALID;
                    oldcur = bb->cursor;
                    isdelta = value[0] == '-' || value[0] == '+';
                    n = (int)strtol(value, &value, 10);
                    if (*value == '%')
                        n = (n * (value[1] == 'n' ? bb->nfiles : termheight)) / 100;
                    if (isdelta) set_cursor(bb, bb->cursor + n);
                    else set_cursor(bb, n);
                    if (cmd[0] == 's') { // spread:
                        int sel = IS_SELECTED(bb->files[oldcur]);
                        for (int i = bb->cursor; i != oldcur; i += (oldcur > i ? 1 : -1)) {
                            if (sel && !IS_SELECTED(bb->files[i]))
                                select_file(bb, bb->files[i]);
                            else if (!sel && IS_SELECTED(bb->files[i]))
                                deselect_file(bb, bb->files[i]);
                        }
                        if (abs(oldcur - bb->cursor) > 1)
                            return BB_DIRTY;
                    }
                    return BB_NOP;
                }
            }
        }
        case 'j': { // jump:
            if (!value) return BB_INVALID;
            char key = value[0];
            if (bb->marks[(int)key]) {
                value = bb->marks[(int)key];
                goto cd;
            }
            return BB_INVALID;
        }
        default: break;
    }
    return BB_INVALID;
}

void explore(bb_t *bb, const char *path)
{
    static long cmdpos = 0;
    int lastwidth = termwidth, lastheight = termheight;
    int lazy = 0, check_cmds = 1;

    init_term();
    fputs(T_ON(T_ALT_SCREEN), tty_out);

    {
        char *real = realpath(path, NULL);
        if (!real || chdir(real))
            err("Not a valid path: %s\n", path);
        populate_files(bb, real);
        free(real); // estate
    }

    for (int i = 0; startupcmds[i]; i++) {
        if (startupcmds[i][0] == '+') {
            if (execute_cmd(bb, startupcmds[i] + 1) == BB_QUIT)
                goto quit;
        } else {
            run_cmd_on_selection(bb, startupcmds[i]);
            check_cmds = 1;
        }
    }

  refresh:
    lazy = 0;

  redraw:
    render(bb, lazy);
    lazy = 1;

  next_input:
    if (termwidth != lastwidth || termheight != lastheight) {
        lastwidth = termwidth; lastheight = termheight;
        lazy = 0;
        goto redraw;
    }

    if (check_cmds) {
        FILE *cmdfile = fopen(cmdfilename, "r");
        if (!cmdfile) {
            if (!lazy) goto redraw;
            goto get_keyboard_input;
        }

        if (cmdpos) 
            fseek(cmdfile, cmdpos, SEEK_SET);

        char *cmd = NULL;
        size_t cap = 0;
        while (cmdfile && getdelim(&cmd, &cap, '\0', cmdfile) >= 0) {
            cmdpos = ftell(cmdfile);
            if (!cmd[0]) continue;
            switch (execute_cmd(bb, cmd)) {
                case BB_INVALID:
                    break;
                case BB_DIRTY:
                    lazy = 0;
                case BB_NOP:
                    goto redraw;
                case BB_REFRESH:
                    free(cmd);
                    fclose(cmdfile);
                    goto refresh;
                case BB_QUIT:
                    free(cmd);
                    fclose(cmdfile);
                    goto quit;
            }
        }
        free(cmd);
        fclose(cmdfile);
        unlink(cmdfilename);
        cmdpos = 0;
        check_cmds = 0;
        //if (!lazy) goto redraw;
    }

    int key;
  get_keyboard_input:
    key = term_getkey(fileno(tty_in), &mouse_x, &mouse_y, KEY_DELAY);
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
                for (char *col = bb->columns; *col; ++col) {
                    if (col != bb->columns) x += 3;
                    switch (*col) {
                        case 's': x += colsizew;
                                  break;
                        case 'p': x += colpermw;
                                  break;
                        case 'm': case 'a': case 'c':
                                  x += coldatew;
                                  break;
                        case 'n': x += colnamew;
                                  break;
                    }
                    if (x >= mouse_x) {
                        if (DESCENDING(bb->sort) == *col)
                            bb->sort ^= SORT_DESCENDING;
                        else
                            bb->sort = *col;
                        sort_files(bb);
                        goto refresh;
                    }
                }
                goto next_input;
            } else if (mouse_y >= 2 && bb->scroll + (mouse_y - 2) < bb->nfiles) {
                int clicked = bb->scroll + (mouse_y - 2);
                if (mouse_x == 0) {
                    if (IS_SELECTED(bb->files[clicked]))
                        deselect_file(bb, bb->files[clicked]);
                    else
                        select_file(bb, bb->files[clicked]);
                    goto redraw;
                }
                set_cursor(bb, clicked);
                if (dt_ms <= 200) {
                    key = KEY_MOUSE_DOUBLE_LEFT;
                    goto user_bindings;
                }
                goto redraw;
            }
            break;
        }

        case KEY_CTRL_C:
            cleanup_and_exit(SIGINT);
            goto quit; // Unreachable

        case KEY_CTRL_Z:
            close_term();
            fputs(T_OFF(T_ALT_SCREEN), stdout);
            fflush(stdout);
            raise(SIGTSTP);
            init_term();
            fputs(T_ON(T_ALT_SCREEN), tty_out);
            lazy = 0;
            goto redraw;

        case KEY_CTRL_H: {
            move_cursor(tty_out, 0,termheight-1);
            fputs("\033[K\033[33;1mPress any key...\033[0m", tty_out);
            while ((key = term_getkey(fileno(tty_in), &mouse_x, &mouse_y, 1000)) == -1)
                ;
            move_cursor(tty_out, 0,termheight-1);
            fputs("\033[K\033[1m<\033[33m", tty_out);
            const char *name = keyname(key);
            if (name) fputs(name, tty_out);
            else if (' ' <= key && key <= '~')
                fputc((char)key, tty_out);
            else
                fprintf(tty_out, "\033[31m\\x%02X", key);

            fputs("\033[0;1m> is bound to: \033[34;1m", tty_out);
            for (int i = 0; bindings[i].keys[0] > 0; i++) {
                for (int j = 0; bindings[i].keys[j]; j++) {
                    if (key == bindings[i].keys[j]) {
                        fputs(bindings[i].description, tty_out);
                        fputs("\033[0m", tty_out);
                        goto next_input;
                    }
                }
            }
            fputs("--- nothing ---\033[0m", tty_out);
            goto next_input;
        }

        case -1:
            goto next_input;

        default: {
            // Search user-defined key bindings from config.h:
            binding_t *binding;
          user_bindings:
            for (int i = 0; bindings[i].keys[0] > 0; i++) {
                for (int j = 0; bindings[i].keys[j]; j++) {
                    if (key == bindings[i].keys[j]) {
                        // Move to front optimization:
                        if (i > 2) {
                            binding_t tmp;
                            tmp = bindings[0];
                            bindings[0] = bindings[i];
                            bindings[i] = tmp;
                            i = 0;
                        }
                        binding = &bindings[i];
                        goto run_binding;
                    }
                }
            }
            // Nothing matched
            goto next_input;

          run_binding:
            if (cmdpos != 0)
                err("Command file still open");
            if (binding->command[0] == '+') {
                switch (execute_cmd(bb, binding->command + 1)) {
                    case BB_INVALID: break;
                    case BB_DIRTY: lazy = 0;
                    case BB_NOP: goto redraw;
                    case BB_REFRESH: goto refresh;
                    case BB_QUIT: goto quit;
                }
                goto get_keyboard_input;
            }
            move_cursor(tty_out, 0, termheight-1);
            close_term();
            if (binding->flags & NORMAL_TERM) {
                fputs(T_OFF(T_ALT_SCREEN), stdout);
                fflush(stdout);
            }
            if (binding->flags & SHOW_CURSOR)
                fputs(T_ON(T_SHOW_CURSOR), stdout);
            run_cmd_on_selection(bb, binding->command);
            init_term();
            if (binding->flags & NORMAL_TERM)
                fputs(T_ON(T_ALT_SCREEN), tty_out);
            if (binding->flags & NORMAL_TERM)
                lazy = 0;
            check_cmds = 1;
            goto redraw;
        }
    }
    goto next_input;

  quit:
    populate_files(bb, NULL);
    fputs(T_LEAVE_BBMODE, tty_out);
    close_term();
    fputs(T_OFF(T_ALT_SCREEN), stdout);
    fflush(stdout);
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
                p = stpcpy(p, name);
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
            fputs_escaped(stdout, bindings[i].command, "\033[0;32m");
            fflush(stdout);
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
      has_initial_path:
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
            initial_path = ".";
            goto has_initial_path;
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
            return 0;
        }
        if (strcmp(argv[i], "--version") == 0) {
          version:
            printf("bb " BB_VERSION "\n");
            return 0;
        }
        if (argv[i][0] == '-' && argv[i][1] == '-') {
            if (argv[i][2] == '\0') break;
            continue;
        }
        if (argv[i][0] == '-') {
            for (char *c = &argv[i][1]; *c; c++) {
                switch (*c) {
                    case 'h':goto usage;
                    case 'v': goto version;
                    case 'd': print_dir = 1;
                              break;
                    case '0': sep = '\0';
                              break;
                    case 's': print_selection = 1;
                              break;
                    case 'b': print_bindings(0);
                              return 0;
                    case 'B': print_bindings(1);
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

    bb_t *bb = memcheck(calloc(1, sizeof(bb_t)));
    strcpy(bb->columns, "n");
    bb->sort = 'n';
    explore(bb, real);
    free(real);

    if (bb->firstselected && print_selection) {
        for (entry_t *e = bb->firstselected; e; e = e->next) {
            const char *p = e->d_fullname;
            while (*p) {
                const char *p2 = strchr(p, '\n');
                if (!p2) p2 = p + strlen(p);
                write(STDOUT_FILENO, p, (size_t)(p2 - p));
                if (*p2 == '\n' && sep == '\n')
                    write(STDOUT_FILENO, "\\", 1);
                p = p2;
            }
            write(STDOUT_FILENO, &sep, 1);
        }
        fflush(stdout);
    }
    if (print_dir) {
        printf("%s\n", initial_path);
    }
    for (int m = 0; m < 128; m++)
        if (bb->marks[m]) free(bb->marks[m]);
    free(bb);

    return 0;
}

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
