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
#include "bterm.h"

#define BB_VERSION "0.14.0"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MAX_COLS 12
#define MAX_SORT (2*MAX_COLS)
#define HASH_SIZE 1024
#define HASH_MASK (HASH_SIZE - 1)
#define MAX(a,b) ((a) < (b) ? (b) : (a))
#define MIN(a,b) ((a) > (b) ? (b) : (a))
#define IS_SELECTED(p) (((p)->selected.atme) != NULL)
#define IS_VIEWED(p) ((p)->index >= 0)
#define LOWERCASE(c) ('A' <= (c) && (c) <= 'Z' ? ((c) + 'a' - 'A') : (c))
#define E_ISDIR(e) (S_ISDIR(S_ISLNK((e)->info.st_mode) ? (e)->linkedmode : (e)->info.st_mode))
#define ONSCREEN (termheight - 4)

#ifdef __APPLE__
#define mtime(s) (s).st_mtimespec
#define atime(s) (s).st_atimespec
#define ctime(s) (s).st_ctimespec
#else
#define mtime(s) (s).st_mtim
#define atime(s) (s).st_atim
#define ctime(s) (s).st_ctim
#endif

#define err(...) do { \
    cleanup(); \
    fprintf(stderr, __VA_ARGS__); \
    if (errno) fprintf(stderr, "\n%s", strerror(errno)); \
    fprintf(stderr, "\n"); \
    exit(EXIT_FAILURE); \
} while (0)

// Types
typedef enum {
    COL_NONE = 0,
    COL_NAME = 'n',
    COL_SIZE = 's',
    COL_PERM = 'p',
    COL_MTIME = 'm',
    COL_CTIME = 'c',
    COL_ATIME = 'a',
    COL_RANDOM = 'r',
    COL_SELECTED = '*',
} column_e;

/* entry_t uses intrusive linked lists.  This means entries can only belong to
 * one list at a time, in this case the list of selected entries. 'atme' is an
 * indirect pointer to either the 'next' field of the previous list member, or
 * the variable that points to the first list member. In other words,
 * item->next->atme == &item->next and firstitem->atme == &firstitem.
 */
typedef struct {
    struct entry_s *next, **atme;
} llnode_t;

typedef struct entry_s {
    llnode_t selected, hash;
    char *name, *linkname;
    struct stat info;
    mode_t linkedmode;
    int no_esc : 1;
    int link_no_esc : 1;
    int shufflepos;
    int index;
    char fullname[1];
    // ------- fullname must be last! --------------
} entry_t;

typedef struct bb_s {
    entry_t *hash[HASH_SIZE];
    entry_t **files;
    entry_t *firstselected;
    char path[PATH_MAX];
    int nfiles;
    int scroll, cursor;

    char *marks[128]; // Mapping from key to directory
    char sort[MAX_SORT+1];
    char columns[MAX_COLS+1];
    unsigned int dirty : 1;
    unsigned int show_dotdot : 1;
    unsigned int show_dot : 1;
    unsigned int show_dotfiles : 1;
    unsigned int interleave_dirs : 1;
} bb_t;

typedef enum { BB_OK = 0, BB_INVALID, BB_QUIT } bb_result_t;

// Functions
static void update_term_size(int sig);
static void init_term(void);
static void close_term(void);
static void cleanup_and_exit(int sig);
static void cleanup(void);
static void* memcheck(void *p);
static int run_cmd_on_selection(bb_t *bb, const char *cmd);
static int fputs_escaped(FILE *f, const char *str, const char *color);
static const char* color_of(mode_t mode);
static void set_sort(bb_t *bb, const char *sort);
static void render(bb_t *bb);
#ifdef __APPLE__
static int compare_files(void *v, const void *v1, const void *v2);
#else
static int compare_files(const void *v1, const void *v2, void *v);
#endif
static void clear_selection(bb_t *bb);
static void select_entry(bb_t *bb, entry_t *e);
static void deselect_entry(bb_t *bb, entry_t *e);
static void toggle_entry(bb_t *bb, entry_t *e);
static void set_cursor(bb_t *bb, int i);
static void set_scroll(bb_t *bb, int i);
static entry_t* load_entry(bb_t *bb, const char *path);
static void remove_entry(entry_t *e);
static void sort_files(bb_t *bb);
static void normalize_path(const char *root, const char *path, char *pbuf);
static int cd_to(bb_t *bb, const char *path);
static void populate_files(bb_t *bb, const char *path);
static bb_result_t execute_cmd(bb_t *bb, const char *cmd);
static void bb_browse(bb_t *bb, const char *path);
static void print_bindings(void);

// Config options
extern binding_t bindings[];
extern const char *startupcmds[];
extern const column_t columns[128];

// Constants
static const char *T_ENTER_BBMODE = T_OFF(T_SHOW_CURSOR ";" T_WRAP) T_ON(T_MOUSE_XY ";" T_MOUSE_CELL ";" T_MOUSE_SGR);
static const char *T_LEAVE_BBMODE = T_OFF(T_MOUSE_XY ";" T_MOUSE_CELL ";" T_MOUSE_SGR ";" T_ALT_SCREEN) T_ON(T_SHOW_CURSOR ";" T_WRAP);
static const char *T_LEAVE_BBMODE_PARTIAL = T_OFF(T_MOUSE_XY ";" T_MOUSE_CELL ";" T_MOUSE_SGR) T_ON(T_WRAP);

// Global variables
static struct termios orig_termios, bb_termios;
static FILE *tty_out = NULL, *tty_in = NULL;
static int termwidth, termheight;
static int mouse_x, mouse_y;
static char *cmdfilename = NULL;
static struct timespec lastclick = {0, 0};


/*
 * Hanlder for SIGWINCH events
 */
void update_term_size(int sig)
{
    (void)sig;
    struct winsize sz = {0};
    ioctl(fileno(tty_in), TIOCGWINSZ, &sz);
    termwidth = sz.ws_col;
    termheight = sz.ws_row;
}

/*
 * Initialize the terminal files for /dev/tty and set up some desired
 * attributes like passing Ctrl-c as a key instead of interrupting
 */
void init_term(void)
{
    tty_in = fopen("/dev/tty", "r");
    tty_out = fopen("/dev/tty", "w");
    tcgetattr(fileno(tty_out), &orig_termios);
    memcpy(&bb_termios, &orig_termios, sizeof(bb_termios));
    cfmakeraw(&bb_termios);
    bb_termios.c_cc[VMIN] = 0;
    bb_termios.c_cc[VTIME] = 0;
    if (tcsetattr(fileno(tty_out), TCSAFLUSH, &bb_termios) == -1)
        err("Couldn't tcsetattr");
    update_term_size(0);
    signal(SIGWINCH, update_term_size);
    // Initiate mouse tracking and disable text wrapping:
    fputs(T_ENTER_BBMODE, tty_out);
}

/*
 * Close the /dev/tty terminals and restore some of the attributes.
 */
void close_term(void)
{
    if (tty_out) {
        tcsetattr(fileno(tty_out), TCSAFLUSH, &orig_termios);
        fputs(T_LEAVE_BBMODE_PARTIAL, tty_out);
        fflush(tty_out);
        fclose(tty_out);
        tty_out = NULL;
        fclose(tty_in);
        tty_in = NULL;
    }
    signal(SIGWINCH, SIG_DFL);
}

/*
 * Close safely in a way that doesn't gunk up the terminal.
 */
void cleanup_and_exit(int sig)
{
    static volatile sig_atomic_t error_in_progress = 0;
    if (error_in_progress)
        raise(sig);
    error_in_progress = 1;
    cleanup();
    signal(sig, SIG_DFL);
    raise(sig);
}

/*
 * Close the terminal, reset the screen, and delete the cmdfile
 */
void cleanup(void)
{
    if (cmdfilename) {
        unlink(cmdfilename);
        free(cmdfilename);
        cmdfilename = NULL;
    }
    if (tty_out)
        fputs(T_OFF(T_ALT_SCREEN), tty_out);
    close_term();
}

/*
 * Memory allocation failures are unrecoverable in bb, so this wrapper just
 * prints an error message and exits if that happens.
 */
void* memcheck(void *p)
{
    if (!p) err("Allocation failure");
    return p;
}

/*
 * Run a command with the selected files passed as sequential arguments to the
 * command (or pass the cursor file if none are selected).
 * Return the exit status of the command.
 */
int run_cmd_on_selection(bb_t *bb, const char *cmd)
{
    pid_t child;
    void (*old_handler)(int) = signal(SIGINT, SIG_IGN);
    if ((child = fork()) == 0) {
        signal(SIGINT, SIG_DFL);
        // TODO: is there a max number of args? Should this be batched?
        size_t space = 32;
        char **args = memcheck(calloc(space, sizeof(char*)));
        size_t i = 0;
        args[i++] = "sh";
        args[i++] = "-c";
        args[i++] = (char*)cmd;
        args[i++] = "--"; // ensure files like "-i" are not interpreted as flags for sh
        entry_t *first = bb->firstselected ? bb->firstselected : (bb->nfiles ? bb->files[bb->cursor] : NULL);
        for (entry_t *e = first; e; e = e->selected.next) {
            if (i >= space)
                args = memcheck(realloc(args, (space += 100)*sizeof(char*)));
            args[i++] = e->fullname;
        }
        args[i] = NULL;

        setenv("BBSELECTED", bb->firstselected ? "1" : "", 1);
        setenv("BBCURSOR", bb->nfiles ? bb->files[bb->cursor]->fullname : "", 1);

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

/*
 * Print a string, but replacing bytes like '\n' with a red-colored "\n".
 * The color argument is what color to put back after the red.
 * Returns the number of bytes that were escaped.
 */
int fputs_escaped(FILE *f, const char *str, const char *color)
{
    static const char *escapes = "       abtnvfr             e";
    int escaped = 0;
    for (const char *c = str; *c; ++c) {
        if (*c > 0 && *c <= '\x1b' && escapes[(int)*c] != ' ') { // "\n", etc.
            fprintf(f, "\033[31m\\%c%s", escapes[(int)*c], color);
            ++escaped;
        } else if (*c >= 0 && !(' ' <= *c && *c <= '~')) { // "\x02", etc.
            fprintf(f, "\033[31m\\x%02X%s", *c, color);
            ++escaped;
        } else {
            fputc(*c, f);
        }
    }
    return escaped;
}

/*
 * Returns the color of a file listing, given its mode.
 */
const char* color_of(mode_t mode)
{
    if (S_ISDIR(mode)) return DIR_COLOR;
    else if (S_ISLNK(mode)) return LINK_COLOR;
    else if (mode & (S_IXUSR | S_IXGRP | S_IXOTH))
        return EXECUTABLE_COLOR;
    else return NORMAL_COLOR;
}

void set_sort(bb_t *bb, const char *sort)
{
    for (const char *s = sort; s[0] && s[1]; s += 2) {
        char *found;
        if ((found = strchr(bb->sort, s[1]))) {
            memmove(found-1, found+1, strlen(found+1)+1);
        }
    }
    size_t len = MIN(MAX_SORT, strlen(sort));
    memmove(bb->sort + len, bb->sort, MAX_SORT+1 - len);
    memmove(bb->sort, sort, len);
}

/*
 * Draw everything to the screen.
 * If bb->dirty is false, then use terminal scrolling to move the file listing
 * around and only update the files that have changed.
 */
void render(bb_t *bb)
{
    static int lastcursor = -1, lastscroll = -1;
    char buf[64];
    if (lastcursor == -1 || lastscroll == -1)
        bb->dirty = 1;

    if (!bb->dirty) {
        // Use terminal scrolling:
        if (lastscroll > bb->scroll) {
            fprintf(tty_out, "\033[3;%dr\033[%dT\033[1;%dr", termheight-1, lastscroll - bb->scroll, termheight);
        } else if (lastscroll < bb->scroll) {
            fprintf(tty_out, "\033[3;%dr\033[%dS\033[1;%dr", termheight-1, bb->scroll - lastscroll, termheight);
        }
    }

    if (bb->dirty) {
        // Path
        move_cursor(tty_out, 0, 0);
        const char *color = TITLE_COLOR;
        fputs(color, tty_out);
        fputs_escaped(tty_out, bb->path, color);
        fputs(" \033[K\033[0m", tty_out);

        static const char *help = "Press '?' to see key bindings ";
        move_cursor(tty_out, MAX(0, termwidth - (int)strlen(help)), 0);
        fputs(help, tty_out);
        fputs("\033[K\033[0m", tty_out);

        // Columns
        move_cursor(tty_out, 0, 1);
        fputs("\033[0;44;30m\033[K", tty_out);
        int x = 0;
        for (int col = 0; bb->columns[col]; col++) {
            const char *title = columns[(int)bb->columns[col]].name;
            if (!title) title = "";
            move_cursor(tty_out, x, 1);
            if (col > 0) {
                fputs("│\033[K", tty_out);
                x += 1;
            }
            const char *indicator = " ";
            if (bb->columns[col] == bb->sort[1])
                indicator = bb->sort[0] == '-' ? RSORT_INDICATOR : SORT_INDICATOR;
            move_cursor(tty_out, x, 1);
            fputs(indicator, tty_out);
            fputs(title, tty_out);
            x += columns[(int)bb->columns[col]].width;
        }
        fputs(" \033[K\033[0m", tty_out);
    }

    entry_t **files = bb->files;
    for (int i = bb->scroll; i < bb->scroll + termheight - 3; i++) {
        if (!bb->dirty) {
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

        if (i == bb->scroll && bb->nfiles == 0) {
            const char *s = "...no files here...";
            fprintf(tty_out, "\033[37;2m%s\033[0m\033[K\033[J", s);
            break;
        }

        if (i >= bb->nfiles) {
            fputs("\033[J", tty_out);
            break;
        }

        entry_t *entry = files[i];
        if (i == bb->cursor) fputs(CURSOR_COLOR, tty_out);

        int use_fullname =  strcmp(bb->path, "<selection>") == 0;
        int x = 0;
        for (int col = 0; bb->columns[col]; col++) {
            fprintf(tty_out, "\033[%d;%dH\033[K", y+1, x+1);
            if (col > 0) {
                if (i == bb->cursor) fputs("│", tty_out);
                else fputs("\033[37;2m│\033[22m", tty_out);
                fputs(i == bb->cursor ? CURSOR_COLOR : "\033[0m", tty_out);
                x += 1;
            }
            move_cursor(tty_out, x, y);
            switch (bb->columns[col]) {
                case COL_SELECTED:
                    fputs(IS_SELECTED(entry) ? SELECTED_INDICATOR : NOT_SELECTED_INDICATOR, tty_out);
                    fputs(i == bb->cursor ? CURSOR_COLOR : "\033[0m", tty_out);
                    break;

                case COL_RANDOM: {
                    double k = (double)entry->shufflepos/(double)bb->nfiles;
                    int color = (int)(k*232 + (1.-k)*255);
                    fprintf(tty_out, "\033[48;5;%dm  \033[0m%s", color,
                            i == bb->cursor ? CURSOR_COLOR : "\033[0m");
                    break;
                }

                case COL_SIZE: {
                    int j = 0;
                    const char* units = "BKMGTPEZY";
                    double bytes = (double)entry->info.st_size;
                    while (bytes > 1024) {
                        bytes /= 1024;
                        j++;
                    }
                    fprintf(tty_out, " %6.*f%c ", j > 0 ? 1 : 0, bytes, units[j]);
                    break;
                }

                case COL_MTIME:
                    strftime(buf, sizeof(buf), " %I:%M%p %b %e %Y ", localtime(&(entry->info.st_mtime)));
                    fputs(buf, tty_out);
                    break;

                case COL_CTIME:
                    strftime(buf, sizeof(buf), " %I:%M%p %b %e %Y ", localtime(&(entry->info.st_ctime)));
                    fputs(buf, tty_out);
                    break;

                case COL_ATIME:
                    strftime(buf, sizeof(buf), " %I:%M%p %b %e %Y ", localtime(&(entry->info.st_atime)));
                    fputs(buf, tty_out);
                    break;

                case COL_PERM:
                    fprintf(tty_out, " %03o", entry->info.st_mode & 0777);
                    break;

                case COL_NAME: {
                    char color[128];
                    strcpy(color, color_of(entry->info.st_mode));
                    if (i == bb->cursor) strcat(color, CURSOR_COLOR);
                    fputs(color, tty_out);

                    char *name = use_fullname ? entry->fullname : entry->name;
                    if (entry->no_esc) fputs(name, tty_out);
                    else entry->no_esc |= !fputs_escaped(tty_out, name, color);

                    if (E_ISDIR(entry)) fputs("/", tty_out);

                    if (entry->linkname) {
                        if (i != bb->cursor)
                            fputs("\033[37m", tty_out);
                        fputs("\033[2m -> \033[3m", tty_out);
                        strcpy(color, color_of(entry->linkedmode));
                        if (i == bb->cursor) strcat(color, CURSOR_COLOR);
                        strcat(color, "\033[3;2m");
                        fputs(color, tty_out);
                        if (entry->link_no_esc) fputs(entry->linkname, tty_out);
                        else entry->link_no_esc |= !fputs_escaped(tty_out, entry->linkname, color);

                        if (S_ISDIR(entry->linkedmode))
                            fputs("/", tty_out);

                        fputs("\033[22;23m", tty_out);
                    }
                    fputs(i == bb->cursor ? CURSOR_COLOR : "\033[0m", tty_out);
                    fputs("\033[K", tty_out);
                    break;
                }
                default: break;
            }
            x += columns[(int)bb->columns[col]].width;
        }
        fputs(" \033[K\033[0m", tty_out); // Reset color and attributes
    }

    if (bb->firstselected) {
        int n = 0;
        for (entry_t *s = bb->firstselected; s; s = s->selected.next) ++n;
        int x = termwidth - 14;
        for (int k = n; k; k /= 10) x--;
        move_cursor(tty_out, MAX(0, x), termheight - 1);
        fprintf(tty_out, "\033[41;30m %d Selected \033[0m", n);
    } else {
        move_cursor(tty_out, MAX(0, termwidth/2), termheight - 1);
        fputs("\033[0m\033[K", tty_out);
    }

    lastcursor = bb->cursor;
    lastscroll = bb->scroll;
    fflush(tty_out);
}

/*
 * Used for sorting, this function compares files according to the sorting-related options,
 * like bb->sort
 */
#ifdef __APPLE__
int compare_files(void *v, const void *v1, const void *v2)
#else
int compare_files(const void *v1, const void *v2, void *v)
#endif
{
#define COMPARE(a, b) if ((a) != (b)) { return sign*((a) < (b) ? 1 : -1); }
#define COMPARE_TIME(t1, t2) COMPARE((t1).tv_sec, (t2).tv_sec) COMPARE((t1).tv_nsec, (t2).tv_nsec)
    bb_t *bb = (bb_t*)v;
    const entry_t *e1 = *((const entry_t**)v1), *e2 = *((const entry_t**)v2);

    int sign = 1;
    if (!bb->interleave_dirs) {
        COMPARE(E_ISDIR(e1), E_ISDIR(e2));
    }

    for (char *sort = bb->sort + 1; *sort; sort += 2) {
        sign = sort[-1] == '-' ? -1 : 1;
        switch (*sort) {
            case COL_SELECTED: COMPARE(IS_SELECTED(e1), IS_SELECTED(e2)); break;
            case COL_NAME: {
                /* This sorting method is not identical to strverscmp(). Notably, bb's sort
                 * will order: [0, 1, 9, 00, 01, 09, 10, 000, 010] instead of strverscmp()'s
                 * order: [000, 00, 01, 010, 09, 0, 1, 9, 10]. I believe bb's sort is consistent
                 * with how people want their files grouped: all files padded to n digits
                 * will be grouped together, and files with the same padding will be sorted
                 * ordinally. This version also does case-insensitivity by lowercasing words,
                 * so the following characters come before all letters: [\]^_`
                 */
                const char *n1 = e1->name, *n2 = e2->name;
                while (*n1 && *n2) {
                    char c1 = LOWERCASE(*n1), c2 = LOWERCASE(*n2);
                    if ('0' <= c1 && c1 <= '9' && '0' <= c2 && c2 <= '9') {
                        long i1 = strtol(n1, (char**)&n1, 10);
                        long i2 = strtol(n2, (char**)&n2, 10);
                        // Shorter numbers always go before longer. In practice, I assume
                        // filenames padded to the same number of digits should be grouped
                        // together, instead of
                        // [1.png, 0001.png, 2.png, 0002.png, 3.png], it makes more sense to have:
                        // [1.png, 2.png, 3.png, 0001.png, 0002.png]
                        COMPARE((n2 - e2->name), (n1 - e1->name));
                        COMPARE(i2, i1);
                    } else {
                        COMPARE(c2, c1);
                        ++n1; ++n2;
                    }
                }
                COMPARE(LOWERCASE(*n2), LOWERCASE(*n1));
                break;
            }
            case COL_PERM: COMPARE((e1->info.st_mode & 0x3FF), (e2->info.st_mode & 0x3FF)); break;
            case COL_SIZE: COMPARE(e1->info.st_size, e2->info.st_size); break;
            case COL_MTIME: COMPARE_TIME(mtime(e1->info), mtime(e2->info)); break;
            case COL_CTIME: COMPARE_TIME(ctime(e1->info), ctime(e2->info)); break;
            case COL_ATIME: COMPARE_TIME(atime(e1->info), atime(e2->info)); break;
            case COL_RANDOM: COMPARE(e1->shufflepos, e2->shufflepos); break;
        }
    }
    return 0;
#undef COMPARE
#undef COMPARE_TIME
}

/*
 * Deselect all files
 */
void clear_selection(bb_t *bb)
{
    for (entry_t *next, *e = bb->firstselected; e; e = next) {
        next = e->selected.next;
        e->selected.atme = NULL;
        e->selected.next = NULL;
        if (!IS_VIEWED(e)) remove_entry(e);
    }
    bb->firstselected = NULL;
    bb->dirty = 1;
}

/*
 * Select a file
 */
void select_entry(bb_t *bb, entry_t *e)
{
    if (IS_SELECTED(e)) return;
    if (bb->nfiles > 0 && e != bb->files[bb->cursor])
        bb->dirty = 1;
    if (bb->firstselected)
        bb->firstselected->selected.atme = &e->selected.next;
    e->selected.next = bb->firstselected;
    e->selected.atme = &bb->firstselected;
    bb->firstselected = e;
}

/*
 * Deselect a file
 */
void deselect_entry(bb_t *bb, entry_t *e)
{
    (void)bb;
    if (IS_SELECTED(e)) {
        if (bb->nfiles > 0 && e != bb->files[bb->cursor])
            bb->dirty = 1;
        if (e->selected.next)
            e->selected.next->selected.atme = e->selected.atme;
        *(e->selected.atme) = e->selected.next;
        e->selected.next = NULL;
        e->selected.atme = NULL;
    }
    if (!IS_VIEWED(e)) remove_entry(e);
}

/*
 * Toggle a file's selection state
 */
void toggle_entry(bb_t *bb, entry_t *e)
{
    if (IS_SELECTED(e)) deselect_entry(bb, e);
    else select_entry(bb, e);
}

/*
 * Set bb's file cursor to the given index (and adjust the scroll as necessary)
 */
void set_cursor(bb_t *bb, int newcur)
{
    int oldcur = bb->cursor;
    if (newcur > bb->nfiles - 1) newcur = bb->nfiles - 1;
    if (newcur < 0) newcur = 0;
    bb->cursor = newcur;
    if (bb->nfiles <= ONSCREEN) {
        bb->scroll = 0;
        return;
    }

    if (oldcur < bb->cursor) {
        if (bb->scroll > bb->cursor)
            bb->scroll = MAX(0, bb->cursor);
        else if (bb->scroll < bb->cursor - ONSCREEN + SCROLLOFF)
            bb->scroll = MIN(bb->nfiles - 1 - ONSCREEN,
                             bb->scroll + (newcur - oldcur));
    } else {
        if (bb->scroll > bb->cursor - SCROLLOFF)
            bb->scroll = MAX(0, bb->scroll + (newcur - oldcur));//bb->cursor - SCROLLOFF);
        else if (bb->scroll < bb->cursor - ONSCREEN)
            bb->scroll = MIN(bb->cursor - ONSCREEN,
                             bb->nfiles - 1 - ONSCREEN);
    }
}

/*
 * Set bb's scroll to the given index (and adjust the cursor as necessary)
 */
void set_scroll(bb_t *bb, int newscroll)
{
    int delta = newscroll - bb->scroll;
    if (bb->nfiles <= ONSCREEN) {
        newscroll = 0;
    } else {
        if (newscroll > bb->nfiles - 1 - ONSCREEN)
            newscroll = bb->nfiles - 1 - ONSCREEN;
        if (newscroll < 0) newscroll = 0;
    }

    bb->scroll = newscroll;
    bb->cursor += delta;
    if (bb->cursor > bb->nfiles - 1) bb->cursor = bb->nfiles - 1;
    if (bb->cursor < 0) bb->cursor = 0;
}

/*
 * Load a file's info into an entry_t and return it (if found).
 * The returned entry must be free()ed by the caller.
 * Warning: this does not deduplicate entries, and it's best if there aren't
 * duplicate entries hanging around.
 */
entry_t* load_entry(bb_t *bb, const char *path)
{
    struct stat linkedstat, filestat;
    if (!path || !path[0]) return NULL;
    if (lstat(path, &filestat) == -1) return NULL;
    char pbuf[PATH_MAX];
    if (path[0] == '~' && (path[1] == '\0' || path[1] == '/')) {
        char *home;
        if (!(home = getenv("HOME")))
            return NULL;
        strcpy(pbuf, home);
        strcat(pbuf, path+1);
    } else if (path[0] == '/') {
        strcpy(pbuf, path);
    } else {
        strcpy(pbuf, bb->path);
        strcat(pbuf, path);
    }
    if (pbuf[strlen(pbuf)-1] == '/' && pbuf[1])
        pbuf[strlen(pbuf)-1] = '\0';

    // Check for pre-existing:
    for (entry_t *e = bb->hash[(int)filestat.st_ino & HASH_MASK]; e; e = e->hash.next) {
        if (e->info.st_ino == filestat.st_ino && e->info.st_dev == filestat.st_dev
            && strcmp(e->fullname, pbuf) == 0)
            return e;
    }

    ssize_t linkpathlen = -1;
    char linkbuf[PATH_MAX];
    if (S_ISLNK(filestat.st_mode)) {
        linkpathlen = readlink(pbuf, linkbuf, sizeof(linkbuf));
        if (linkpathlen < 0) err("Couldn't read link: '%s'", pbuf);
        linkbuf[linkpathlen] = 0;
        if (stat(pbuf, &linkedstat) == -1) memset(&linkedstat, 0, sizeof(linkedstat));
    }
    size_t pathlen = strlen(pbuf);
    size_t entry_size = sizeof(entry_t) + (pathlen + 1) + (size_t)(linkpathlen + 1);
    entry_t *entry = memcheck(calloc(entry_size, 1));
    char *end = stpcpy(entry->fullname, pbuf);
    if (linkpathlen >= 0)
        entry->linkname = strcpy(end + 1, linkbuf);
    if (strcmp(entry->fullname, "/") == 0) {
        entry->name = entry->fullname;
    } else {
        entry->name = strrchr(entry->fullname, '/');
        if (!entry->name) err("No slash found in '%s' from '%s'", entry->fullname, path);
        ++entry->name;
    }
    if (S_ISLNK(filestat.st_mode))
        entry->linkedmode = linkedstat.st_mode;
    entry->info = filestat;
    if (bb->hash[(int)filestat.st_ino & HASH_MASK])
        bb->hash[(int)filestat.st_ino & HASH_MASK]->hash.atme = &entry->hash.next;
    entry->hash.next = bb->hash[(int)filestat.st_ino & HASH_MASK];
    entry->hash.atme = &bb->hash[(int)filestat.st_ino & HASH_MASK];
    entry->index = -1;
    bb->hash[(int)filestat.st_ino & HASH_MASK] = entry;
    return entry;
}

void remove_entry(entry_t *e)
{
    if (IS_SELECTED(e)) err("Attempt to remove an entry while it is still selected.");
    if (IS_VIEWED(e)) err("Attempt to remove an entry while it is still being viewed.");
    if (e->hash.next)
        e->hash.next->hash.atme = e->hash.atme;
    *(e->hash.atme) = e->hash.next;
    e->hash.atme = NULL;
    e->hash.next = NULL;
    if (!IS_SELECTED(e)) free(e);
}

void sort_files(bb_t *bb)
{
#ifdef __APPLE__
    qsort_r(bb->files, (size_t)bb->nfiles, sizeof(entry_t*), bb, compare_files);
#else
    qsort_r(bb->files, (size_t)bb->nfiles, sizeof(entry_t*), compare_files, bb);
#endif
    for (int i = 0; i < bb->nfiles; i++)
        bb->files[i]->index = i;
    bb->dirty = 1;
}

/*
 * Prepend `root` to relative paths, replace "~" with $HOME, remove ".",
 * replace "/foo/baz/../" with "/foo/", and make sure there's a trailing
 * slash. The normalized path is stored in `normalized`.
 */
void normalize_path(const char *root, const char *path, char *normalized)
{
    if (path[0] == '~' && (path[1] == '\0' || path[1] == '/')) {
        char *home;
        if (!(home = getenv("HOME"))) return;
        strcpy(normalized, home);
        ++path;
    } else if (path[0] == '/') {
        normalized[0] = '\0';
    } else {
        strcpy(normalized, root);
    }
    strcat(normalized, path);

    if (normalized[strlen(normalized)-1] != '/')
        strcat(normalized, "/");

    char *src = normalized, *dest = normalized;
    while (*src) {
        if (strncmp(src, "/./", 3) == 0) {
            src += 2;
        } else if (strncmp(src, "/../", 4) == 0) {
            src += 3;
            while (dest > normalized && *(--dest) != '/')
                ;
        }
        *(dest++) = *(src++);
    }
    *dest = '\0';
}

int cd_to(bb_t *bb, const char *path)
{
    char pbuf[PATH_MAX], prev[PATH_MAX] = {0};
    strcpy(prev, bb->path);
    if (strcmp(path, "<selection>") == 0) {
        strcpy(pbuf, path);
        if (bb->marks['-']) free(bb->marks['-']);
        bb->marks['-'] = memcheck(strdup(bb->path));
    } else if (strcmp(path, "..") == 0 && strcmp(bb->path, "<selection>") == 0) {
        if (!bb->marks['-']) return -1;
        strcpy(pbuf, bb->marks['-']);
        if (chdir(pbuf)) return -1;
    } else {
        normalize_path(bb->path, path, pbuf);
        if (chdir(pbuf)) return -1;
    }

    if (strcmp(bb->path, "<selection>") != 0) {
        if (bb->marks['-']) free(bb->marks['-']);
        bb->marks['-'] = memcheck(strdup(bb->path));
    }

    populate_files(bb, pbuf);
    if (prev[0]) {
        entry_t *p = load_entry(bb, prev);
        if (p) {
            if (IS_VIEWED(p)) set_cursor(bb, p->index);
            else if (!IS_SELECTED(p)) remove_entry(p);
        }
    }
    return 0;
}

/*
 * Remove all the files currently stored in bb->files and if `path` is non-NULL,
 * update `bb` with a listing of the files in `path`
 */
void populate_files(bb_t *bb, const char *path)
{
    bb->dirty = 1;

    // Clear old files (if any)
    if (bb->files) {
        for (int i = 0; i < bb->nfiles; i++) {
            bb->files[i]->index = -1;
            if (!IS_SELECTED(bb->files[i]))
                remove_entry(bb->files[i]);
            bb->files[i] = NULL;
        }
        free(bb->files);
        bb->files = NULL;
    }

    int old_scroll = bb->scroll, old_cursor = bb->cursor;
    int samedir = path && strcmp(bb->path, path) == 0;
    bb->nfiles = 0;
    bb->cursor = 0;
    bb->scroll = 0;

    if (path == NULL || !path[0])
        return;

    size_t space = 0;
    if (strcmp(path, "<selection>") == 0) {
        for (entry_t *e = bb->firstselected; e; e = e->selected.next) {
            if ((size_t)bb->nfiles + 1 > space)
                bb->files = memcheck(realloc(bb->files, (space += 100)*sizeof(void*)));
            e->index = bb->nfiles;
            bb->files[bb->nfiles++] = e;
        }
    } else {
        DIR *dir = opendir(path);
        if (!dir)
            err("Couldn't open dir: %s", path);

        if (path[strlen(path)-1] != '/')
            err("No terminating slash on '%s'", path);

        char pathbuf[PATH_MAX];
        strcpy(pathbuf, path);
        size_t pathbuflen = strlen(pathbuf);
        for (struct dirent *dp; (dp = readdir(dir)) != NULL; ) {
            if (dp->d_name[0] == '.') {
                if (dp->d_name[1] == '.' && dp->d_name[2] == '\0') {
                    if (!bb->show_dotdot) continue;
                } else if (dp->d_name[1] == '\0') {
                    if (!bb->show_dot) continue;
                } else if (!bb->show_dotfiles) continue;
            }
            if ((size_t)bb->nfiles + 1 > space)
                bb->files = memcheck(realloc(bb->files, (space += 100)*sizeof(void*)));
            strcpy(&pathbuf[pathbuflen], dp->d_name);
            entry_t *entry = load_entry(bb, pathbuf);
            if (!entry) err("Failed to load entry: '%s'", dp->d_name);
            entry->index = bb->nfiles;
            bb->files[bb->nfiles++] = entry;
        }
        closedir(dir);
    }

    if (path != bb->path)
        strcpy(bb->path, path);

    for (int i = 0; i < bb->nfiles; i++) {
        int j = rand() / (RAND_MAX / (i + 1)); // This is not optimal, but doesn't need to be
        bb->files[i]->shufflepos = bb->files[j]->shufflepos;
        bb->files[j]->shufflepos = i;
    }

    sort_files(bb);
    if (samedir) {
        set_cursor(bb, old_cursor);
        set_scroll(bb, old_scroll);
    }
}

/*
 * Run a bb internal command (e.g. "+refresh") and return an indicator of what
 * needs to happen next.
 */
bb_result_t execute_cmd(bb_t *bb, const char *cmd)
{
    char *value = strchr(cmd, ':');
    if (value) ++value;
#define set_bool(target) do { if (!value) { target = !target; } else { target = value[0] == '1'; } } while (0)
    switch (cmd[0]) {
        case '.': { // +..:, +.:
            if (cmd[1] == '.') // +..:
                set_bool(bb->show_dotdot);
            else // +.:
                set_bool(bb->show_dot);
            populate_files(bb, bb->path);
            return BB_OK;
        }
        case 'c': { // +cd:, +columns:
            switch (cmd[1]) {
                case 'd': { // +cd:
                    if (!value) return BB_INVALID;
                    if (cd_to(bb, value)) return BB_INVALID;
                    return BB_OK;
                }
                case 'o': { // +columns:
                    if (!value) return BB_INVALID;
                    strncpy(bb->columns, value, MAX_COLS);
                    bb->dirty = 1;
                    return BB_OK;
                }
            }
            return BB_INVALID;
        }
        case 'd': { // +deselect:, +dotfiles:
            switch (cmd[1]) {
                case 'e': { // +deselect:
                    if (!value && !bb->nfiles) return BB_INVALID;
                    if (!value) value = bb->files[bb->cursor]->fullname;
                    if (strcmp(value, "*") == 0) {
                        clear_selection(bb);
                        return BB_OK;
                    } else {
                        entry_t *e = load_entry(bb, value);
                        if (e) {
                            deselect_entry(bb, e);
                            return BB_OK;
                        }
                        // Filename may no longer exist:
                        for (e = bb->firstselected; e; e = e->selected.next) {
                            if (strcmp(e->fullname, value) == 0) {
                                deselect_entry(bb, e);
                                break;
                            }
                        }
                        return BB_OK;
                    }
                }
                case 'o': { // +dotfiles:
                    set_bool(bb->show_dotfiles);
                    populate_files(bb, bb->path);
                    return BB_OK;
                }
            }
        }
        case 'g': { // +goto:
            if (!value) return BB_INVALID;
            entry_t *e = load_entry(bb, value);
            if (!e) return BB_INVALID;
            if (IS_VIEWED(e)) {
                set_cursor(bb, e->index);
                return BB_OK;
            }
            char pbuf[PATH_MAX];
            strcpy(pbuf, e->fullname);
            char *lastslash = strrchr(pbuf, '/');
            if (!lastslash) return BB_INVALID;
            *lastslash = '\0'; // Split in two
            cd_to(bb, pbuf);
            e = load_entry(bb, lastslash+1);
            if (!e) return BB_INVALID;
            if (IS_VIEWED(e)) {
                set_cursor(bb, e->index);
                return BB_OK;
            } else if (!IS_SELECTED(e))
                remove_entry(e);
            return BB_OK;
        }
        case 'i': { // +interleave
            set_bool(bb->interleave_dirs);
            sort_files(bb);
            return BB_OK;
        }
        case 'j': { // +jump:
            if (!value) return BB_INVALID;
            bb->dirty = 1;
            char key = value[0];
            if (bb->marks[(int)key]) {
                value = bb->marks[(int)key];
                if (!value) return BB_INVALID;
                if (cd_to(bb, value)) return BB_INVALID;
                return BB_OK;
            }
            return BB_INVALID;
        }
        case 'm': { // +move:, +mark:
            switch (cmd[1]) {
                case 'a': { // +mark:
                    if (!value) return BB_INVALID;
                    char key = value[0];
                    if (key < 0) return BB_INVALID;
                    value = strchr(value, '=');
                    if (!value) value = bb->path;
                    else ++value;
                    if (bb->marks[(int)key])
                        free(bb->marks[(int)key]);
                    bb->marks[(int)key] = memcheck(strdup(value));
                    return BB_OK;
                }
                default: { // +move:
                    int oldcur, isdelta, n;
                  move:
                    if (!value) return BB_INVALID;
                    if (!bb->nfiles) return BB_INVALID;
                    oldcur = bb->cursor;
                    isdelta = value[0] == '-' || value[0] == '+';
                    n = (int)strtol(value, &value, 10);
                    if (*value == '%')
                        n = (n * (value[1] == 'n' ? bb->nfiles : termheight)) / 100;
                    if (isdelta) set_cursor(bb, bb->cursor + n);
                    else set_cursor(bb, n);
                    if (cmd[0] == 's') { // +spread:
                        int sel = IS_SELECTED(bb->files[oldcur]);
                        for (int i = bb->cursor; i != oldcur; i += (oldcur > i ? 1 : -1)) {
                            if (sel != IS_SELECTED(bb->files[i]))
                                toggle_entry(bb, bb->files[i]);
                        }
                    }
                    return BB_OK;
                }
            }
        }
        case 'q': // +quit
            return BB_QUIT;
        case 'r': // +refresh
            populate_files(bb, bb->path);
            return BB_OK;
        case 's': // +scroll:, +select:, +sort:, +spread:
            switch (cmd[1]) {
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
                    return BB_OK;
                }

                case '\0': case 'e': // +select:
                    if (!value && !bb->nfiles) return BB_INVALID;
                    if (!value) value = bb->files[bb->cursor]->fullname;
                    if (strcmp(value, "*") == 0) {
                        for (int i = 0; i < bb->nfiles; i++) {
                            if (strcmp(bb->files[i]->name, ".")
                                && strcmp(bb->files[i]->name, ".."))
                                select_entry(bb, bb->files[i]);
                        }
                    } else {
                        entry_t *e = load_entry(bb, value);
                        if (e) select_entry(bb, e);
                    }
                    return BB_OK;

                case 'o': // +sort:
                    if (!value) return BB_INVALID;
                    set_sort(bb, value);
                    sort_files(bb);
                    return BB_OK;

                case 'p': // +spread:
                    goto move;
            }
        case 't': { // +toggle:
            if (!value && !bb->nfiles) return BB_INVALID;
            if (!value) value = bb->files[bb->cursor]->fullname;
            entry_t *e = load_entry(bb, value);
            if (e) toggle_entry(bb, e);
            return BB_OK;
        }
        default: err("UNKNOWN COMMAND: '%s'", cmd); break;
    }
    return BB_INVALID;
}

/*
 * Use bb to browse a path.
 */
void bb_browse(bb_t *bb, const char *path)
{
    static long cmdpos = 0;
    int lastwidth = termwidth, lastheight = termheight;
    int check_cmds = 1;

    bb->marks['-'] = memcheck(strdup(path));
    cd_to(bb, path);
    bb->scroll = 0;
    bb->cursor = 0;

    for (int i = 0; startupcmds[i]; i++) {
        if (startupcmds[i][0] == '+') {
            if (execute_cmd(bb, startupcmds[i] + 1) == BB_QUIT)
                goto quit;
        } else {
            run_cmd_on_selection(bb, startupcmds[i]);
            check_cmds = 1;
        }
    }

    init_term();
    fputs(T_ON(T_ALT_SCREEN), tty_out);
    goto force_check_cmds;

  redraw:
    render(bb);
    bb->dirty = 0;

  next_input:
    if (termwidth != lastwidth || termheight != lastheight) {
        lastwidth = termwidth; lastheight = termheight;
        bb->dirty = 1;
        goto redraw;
    }

    if (check_cmds) {
        FILE *cmdfile;
      force_check_cmds:
        cmdfile = fopen(cmdfilename, "r");
        if (!cmdfile) {
            if (bb->dirty) goto redraw;
            goto get_keyboard_input;
        }

        if (cmdpos) 
            fseek(cmdfile, cmdpos, SEEK_SET);

        char *cmd = NULL;
        size_t space = 0;
        while (cmdfile && getdelim(&cmd, &space, '\0', cmdfile) >= 0) {
            cmdpos = ftell(cmdfile);
            if (!cmd[0]) continue;
            if (execute_cmd(bb, cmd) == BB_QUIT) {
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
        goto redraw;
    }

    int key;
  get_keyboard_input:
    key = bgetkey(tty_in, &mouse_x, &mouse_y, KEY_DELAY);
    switch (key) {
        case KEY_MOUSE_LEFT: {
            struct timespec clicktime;
            clock_gettime(CLOCK_MONOTONIC, &clicktime);
            double dt_ms = 1e3*(double)(clicktime.tv_sec - lastclick.tv_sec);
            dt_ms += 1e-6*(double)(clicktime.tv_nsec - lastclick.tv_nsec);
            lastclick = clicktime;
            // Get column:
            char column[3] = "+?";
            for (int col = 0, x = 0; bb->columns[col]; col++) {
                if (col > 0) x += 1;
                x += columns[(int)bb->columns[col]].width;
                if (x >= mouse_x) {
                    column[1] = bb->columns[col];
                    break;
                }
            }
            if (mouse_y == 1) {
                char *pos;
                if ((pos = strstr(bb->sort, column)) && pos == bb->sort)
                    column[0] = '-';
                set_sort(bb, column);
                sort_files(bb);
                goto redraw;
            } else if (2 <= mouse_y && mouse_y <= termheight - 2) {
                int clicked = bb->scroll + (mouse_y - 2);
                if (clicked > bb->nfiles - 1) goto next_input;
                if (column[1] == COL_SELECTED) {
                    toggle_entry(bb, bb->files[clicked]);
                    bb->dirty = 1;
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

        case KEY_CTRL_Z:
            fputs(T_OFF(T_ALT_SCREEN), tty_out);
            close_term();
            raise(SIGTSTP);
            init_term();
            bb->dirty = 1;
            goto redraw;

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
                if (execute_cmd(bb, binding->command + 1) == BB_QUIT)
                    goto quit;
                goto redraw;
            }
            move_cursor(tty_out, 0, termheight-1);
            fputs(T_ON(T_SHOW_CURSOR), tty_out);
            close_term();
            run_cmd_on_selection(bb, binding->command);
            init_term();
            fputs(T_ON(T_ALT_SCREEN), tty_out);
            bb->dirty = 1;
            check_cmds = 1;
            goto redraw;
        }
    }
    goto next_input;

  quit:
    populate_files(bb, NULL);
    if (tty_out) {
        fputs(T_LEAVE_BBMODE, tty_out);
        cleanup();
    }
}

/*
 * Print the current key bindings
 */
void print_bindings(void)
{
    struct winsize sz = {0};
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &sz);
    int width = sz.ws_col;
    if (width == 0) width = 80;

    char buf[1024];
    char *kb = "Key Bindings";
    printf("\n\033[33;1;4m\033[%dG%s\033[0m\n\n", (width-(int)strlen(kb))/2, kb);
    for (int i = 0; bindings[i].keys[0]; i++) {
        char *p = buf;
        for (int j = 0; bindings[i].keys[j]; j++) {
            if (j > 0) *(p++) = ',';
            int key = bindings[i].keys[j];
            const char *name = bkeyname(key);
            if (name)
                p = stpcpy(p, name);
            else if (' ' <= key && key <= '~')
                p += sprintf(p, "%c", (char)key);
            else
                p += sprintf(p, "\033[31m\\x%02X", key);
        }
        *p = '\0';
        printf("\033[1m\033[%dG%s\033[0m", width/2 - 1 - (int)strlen(buf), buf);
        printf("\033[0m\033[%dG\033[34m%s\033[0m", width/2 + 1, bindings[i].description);
        printf("\033[0m\n");
    }
    printf("\n");
}

int main(int argc, char *argv[])
{
    char *initial_path = NULL, depthstr[64] = {0};
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

    int cmdfd = -1;
    if (initial_path) {
      has_initial_path:
        cmdfilename = memcheck(strdup(CMDFILE_FORMAT));
        if ((cmdfd = mkostemp(cmdfilename, O_APPEND)) == -1)
            err("Couldn't create tmpfile: '%s'", CMDFILE_FORMAT);
        // Set up environment variables
        char *curdepth = getenv("BB_DEPTH");
        int depth = curdepth ? atoi(curdepth) : 0;
        sprintf(depthstr, "%d", depth + 1);
        setenv("BB_DEPTH", depthstr, 1);
        setenv("BBCMD", cmdfilename, 1);
    } else if (cmd_args > 0) {
        char *parent_bbcmd = getenv("BBCMD");
        if (!parent_bbcmd || parent_bbcmd[0] == '\0') {
            initial_path = ".";
            goto has_initial_path;
        }
        cmdfd = open(parent_bbcmd, O_WRONLY | O_APPEND | O_CREAT, 0644);
        if (cmdfd == -1) err("Couldn't open cmdfile: '%s'\n", parent_bbcmd);
    }

    int i;
    for (i = 1; i < argc; i++) {
        if (argv[i][0] == '+') {
            write(cmdfd, argv[i]+1, strlen(argv[i]+1)+1);
            continue;
        }
        if (strcmp(argv[i], "--") == 0) break;
        if (strcmp(argv[i], "--help") == 0) {
          usage:
            printf("bb - an itty bitty console TUI file browser\n");
            printf("Usage: bb [-h/--help] [-s] [-b] [-d] [-0] (+command)* [path]\n");
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
                    case 'b': print_bindings();
                              return 0;
                }
            }
            continue;
        }
    }

    if (cmdfd != -1) {
        close(cmdfd);
        cmdfd = -1;
    }

    if (!initial_path)
        return 0;

    // Default values
    setenv("SHELL", "bash", 0);
    setenv("EDITOR", "nano", 0);
    setenv("PAGER", "less", 0);

    atexit(cleanup);
    signal(SIGTERM, cleanup_and_exit);
    signal(SIGINT, cleanup_and_exit);
    signal(SIGXCPU, cleanup_and_exit);
    signal(SIGXFSZ, cleanup_and_exit);
    signal(SIGVTALRM, cleanup_and_exit);
    signal(SIGPROF, cleanup_and_exit);
    signal(SIGSEGV, cleanup_and_exit);

    char path[PATH_MAX], curdir[PATH_MAX];
    getcwd(curdir, PATH_MAX);
    strcat(curdir, "/");
    normalize_path(curdir, initial_path, path);
    if (chdir(path)) err("Not a valid path: %s\n", path);

    bb_t *bb = memcheck(calloc(1, sizeof(bb_t)));
    bb->columns[0] = COL_NAME;
    strcpy(bb->sort, "+n");
    bb_browse(bb, path);

    if (bb->firstselected && print_selection) {
        for (entry_t *e = bb->firstselected; e; e = e->selected.next) {
            const char *p = e->fullname;
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
    if (print_dir)
        printf("%s\n", bb->path);

    clear_selection(bb);
    for (int m = 0; m < 128; m++)
        if (bb->marks[m]) free(bb->marks[m]);
    free(bb);
    if (cmdfilename) free(cmdfilename);

    return 0;
}

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
