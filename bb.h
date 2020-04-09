/*
 * Bitty Browser (bb)
 * Copyright 2019 Bruce Hill
 * Released under the MIT license
 *
 * This file contains definitions and customization for `bb`.
 */
#include <dirent.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <signal.h>
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

#include "bterm.h"

// Macros:
#define BB_VERSION "0.27.0"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define BB_TIME_FMT " %T %D "
#define MAX_COLS 12
#define MAX_SORT (2*MAX_COLS)
#define HASH_SIZE 1024
#define HASH_MASK (HASH_SIZE - 1)
#define MAX(a,b) ((a) < (b) ? (b) : (a))
#define MIN(a,b) ((a) > (b) ? (b) : (a))
#define IS_SELECTED(p) (((p)->selected.atme) != NULL)
#define IS_VIEWED(p) ((p)->index >= 0)
#define IS_LOADED(p) ((p)->hash.atme != NULL)
#define LOWERCASE(c) ('A' <= (c) && (c) <= 'Z' ? ((c) + 'a' - 'A') : (c))
#define E_ISDIR(e) (S_ISDIR(S_ISLNK((e)->info.st_mode) ? (e)->linkedmode : (e)->info.st_mode))
#define ONSCREEN (winsize.ws_row - 3)

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

#define warn(...) do { \
    move_cursor(tty_out, 0, winsize.ws_row-1); \
    fputs("\033[41;33;1m", tty_out); \
    fprintf(tty_out, __VA_ARGS__); \
    fputs(" Press any key to continue...\033[0m  ", tty_out); \
    fflush(tty_out); \
    while (bgetkey(tty_in, NULL, NULL) == -1) usleep(100); \
    dirty = 1; \
} while (0)

#define LL_PREPEND(head, node, name) do { \
    ((node)->name).atme = &(head); \
    ((node)->name).next = head; \
    if (head) ((head)->name).atme = &(((node)->name).next); \
    head = node; \
} while (0)

#define LL_REMOVE(node, name) do { \
    if (((node)->name).next) \
        ((__typeof__(node))(node)->name.next)->name.atme = ((node)->name).atme; \
    if (((node)->name).atme) \
        *(((node)->name).atme) = ((node)->name).next; \
    ((node)->name).atme = NULL; \
    ((node)->name).next = NULL; \
} while (0)

// Types:
typedef struct {
    int key;
    char *script;
    char *description;
} binding_t;

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
typedef struct entry_s {
    struct {
        struct entry_s *next, **atme;
    } selected, hash;
    char *name, *linkname;
    struct stat info;
    mode_t linkedmode;
    int no_esc : 1;
    int link_no_esc : 1;
    int shufflepos;
    int index;
    char fullname[1];
    // ------- fullname must be last! --------------
    // When entries are allocated, extra space on the end is reserved to fill
    // in fullname.
} entry_t;

typedef struct bb_s {
    entry_t *hash[HASH_SIZE];
    entry_t **files;
    entry_t *selected;
    char path[PATH_MAX];
    char prev_path[PATH_MAX];
    int nfiles, nselected;
    int scroll, cursor;

    char *globpats;
    char sort[MAX_SORT+1];
    char columns[MAX_COLS+1];
    unsigned int interleave_dirs : 1;
    unsigned int should_quit : 1;
} bb_t;

// For keeping track of child processes
typedef struct proc_s {
    pid_t pid;
    struct {
        struct proc_s *next, **atme;
    } running;
} proc_t;

// Configurable options:
#define SCROLLOFF   MIN(5, (winsize.ws_row-4)/2)
#define SORT_INDICATOR  "↓"
#define RSORT_INDICATOR "↑"
#define SELECTED_INDICATOR " \033[31;7m \033[0m"
#define NOT_SELECTED_INDICATOR "  "
// Colors (using ANSI escape sequences):
#define TITLE_COLOR      "\033[37;1m"
#define NORMAL_COLOR     "\033[37m"
#define CURSOR_COLOR     "\033[43;30;1m"
#define LINK_COLOR       "\033[35m"
#define DIR_COLOR        "\033[34m"
#define EXECUTABLE_COLOR "\033[31m"
#define SCROLLBAR_FG "\033[48;5;247m "
#define SCROLLBAR_BG "\033[48;5;239m "

#define MAX_BINDINGS 1024

static binding_t bindings[MAX_BINDINGS];

#include "columns.h"

// Functions
void bb_browse(bb_t *bb, const char *initial_path);
static void check_cmdfile(bb_t *bb);
static void cleanup(void);
static void cleanup_and_raise(int sig);
static const char* color_of(mode_t mode);
#ifdef __APPLE__
static int compare_files(void *v, const void *v1, const void *v2);
#else
static int compare_files(const void *v1, const void *v2, void *v);
#endif
static int fputs_escaped(FILE *f, const char *str, const char *color);
static void handle_next_key_binding(bb_t *bb);
static void init_term(void);
static int is_simple_bbcmd(const char *s);
static entry_t* load_entry(bb_t *bb, const char *path);
static inline int matches_cmd(const char *str, const char *cmd);
static void* memcheck(void *p);
static char* normalize_path(const char *root, const char *path, char *pbuf);
static int populate_files(bb_t *bb, const char *path);
static void print_bindings(int fd);
static void run_bbcmd(bb_t *bb, const char *cmd);
static void render(bb_t *bb);
static void restore_term(const struct termios *term);
static int run_script(bb_t *bb, const char *cmd);
static void set_columns(bb_t *bb, const char *cols);
static void set_cursor(bb_t *bb, int i);
static void set_globs(bb_t *bb, const char *globs);
static void set_interleave(bb_t *bb, int interleave);
static void set_selected(bb_t *bb, entry_t *e, int selected);
static void set_scroll(bb_t *bb, int i);
static void set_sort(bb_t *bb, const char *sort);
static void set_title(bb_t *bb);
static void sort_files(bb_t *bb);
static char *trim(char *s);
static int try_free_entry(entry_t *e);
static void update_term_size(int sig);
static int wait_for_process(proc_t **proc);

// Constants
static const char *T_ENTER_BBMODE = T_OFF(T_SHOW_CURSOR ";" T_WRAP) T_ON(T_ALT_SCREEN ";" T_MOUSE_XY ";" T_MOUSE_CELL ";" T_MOUSE_SGR);
static const char *T_LEAVE_BBMODE = T_OFF(T_MOUSE_XY ";" T_MOUSE_CELL ";" T_MOUSE_SGR ";" T_ALT_SCREEN) T_ON(T_SHOW_CURSOR ";" T_WRAP);
static const char *T_LEAVE_BBMODE_PARTIAL = T_OFF(T_MOUSE_XY ";" T_MOUSE_CELL ";" T_MOUSE_SGR) T_ON(T_WRAP);
static const struct termios default_termios = {
    .c_iflag = ICRNL,
    .c_oflag = OPOST | ONLCR | NL0 | CR0 | TAB0 | BS0 | VT0 | FF0,
    .c_lflag = ISIG | ICANON | IEXTEN | ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE,
    .c_cflag = CS8 | CREAD,
    .c_cc[VINTR] = '',
    .c_cc[VQUIT] = '',
    .c_cc[VERASE] = 127,
    .c_cc[VKILL] = '',
    .c_cc[VEOF] = '',
    .c_cc[VSTART] = '',
    .c_cc[VSTOP] = '',
    .c_cc[VSUSP] = '',
    .c_cc[VREPRINT] = '',
    .c_cc[VWERASE] = '',
    .c_cc[VLNEXT] = '',
    .c_cc[VDISCARD] = '',
    .c_cc[VMIN] = 1,
    .c_cc[VTIME] = 0,
};

static const char *description_str = "bb - an itty bitty console TUI file browser\n";
static const char *usage_str = "Usage: bb (-h/--help | -v/--version | -s | -d | -0 | +command)* [[--] directory]\n";

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
