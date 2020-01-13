/*
 * Bitty Browser (bb)
 * Copyright 2019 Bruce Hill
 * Released under the MIT license
 *
 * This file contains definitions and customization for `bb`.
 */
#include <dirent.h>
#include <fcntl.h>
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
#define BB_VERSION "0.20.4"

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

typedef struct {
    int width;
    const char *name;
} column_t;

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

    char sort[MAX_SORT+1];
    char columns[MAX_COLS+1];
    unsigned int show_dotdot : 1;
    unsigned int show_dot : 1;
    unsigned int show_dotfiles : 1;
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

#define MAX_BINDINGS 1024

static binding_t bindings[MAX_BINDINGS];

// Column widths and titles:
static const column_t columns[] = {
    ['*'] = {2,  "*"},
    ['a'] = {21, "      Accessed"},
    ['c'] = {21, "      Created"},
    ['m'] = {21, "      Modified"},
    ['n'] = {40, "Name"},
    ['p'] = {5,  "Permissions"},
    ['r'] = {2,  "Random"},
    ['s'] = {9,  "  Size"},
};

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
static void set_cursor(bb_t *bb, int i);
static void set_selected(bb_t *bb, entry_t *e, int selected);
static void set_scroll(bb_t *bb, int i);
static void set_sort(bb_t *bb, const char *sort);
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

// Shell functions
static const char *bbcmdfn = "bbcmd() {\n"
"    if test $# -eq 0; then cat >> $BBCMD; return; fi\n"
"    for arg; do\n"
"        shift;\n"
"        if expr \"$arg\" : \"^[^:]*:$\" >/dev/null; then\n"
"            if test $# -gt 0; then printf \"$arg%s\\0\" \"$@\" >> $BBCMD;\n"
"            else sed \"s/\\([^\\x00]\\+\\)/$arg\\1/g\" >> $BBCMD; fi;\n"
"            return;\n"
"        fi;\n"
"        printf \"%s\\0\" \"$arg\" >> $BBCMD;\n"
"    done;\n"
"}\n"
// This shell function overwrites the lines above the cursor instead of scrolling the screen
"unscroll() {\n"
"    input=\"$(cat)\"\n"
"    printf \"\\033[$(echo \"$input\" | wc -l)A\\033[J\" >/dev/tty\n"
"    echo \"$input\"\n"
"}\n"
"ask() {\n"
#ifdef ASK
ASK ";\n"
#else
"    [ $# -lt 2 ] && printf '\033[31;1mNot enough args to ask!\033[0m\n' && return 1;\n"
"    printf \"\033[1m%s\033[0m\" \"$2\" >/dev/tty;\n"
"    tput cvvis >/dev/tty;\n"
#ifdef __APPLE__
"    read -e $1 </dev/tty >/dev/tty;\n"
#else
"    read $1 </dev/tty >/dev/tty;\n"
#endif
#endif
"}\n"
"ask1() {\n"
#ifdef ASK1
ASK1 ";\n"
#else
"    tput civis >/dev/tty;\n"
"    printf \"\033[1m%s\033[0m\" \"$2\" >/dev/tty;\n"
"    stty -icanon -echo >/dev/tty 2>/dev/tty;\n"
#ifdef __APPLE__
"    read -n 1 $1 </dev/tty >/dev/tty;\n"
#else
"    eval \"$1=\\$(dd bs=1 count=1 2>/dev/null </dev/tty)\";\n"
#endif
"    stty icanon echo >/dev/tty 2>/dev/tty;\n"
"    tput cvvis >/dev/tty;\n"
#endif
"}\n"
"confirm() {\n"
#ifdef CONFIRM
CONFIRM ";\n"
#else
"    ask1 REPLY \"$1\033[0;1mIs that okay? [y/N] \" && [ \"$REPLY\" = 'y' ];\n"
#endif
"}\n"
"pause() {\n"
"    ask1 REPLY \"\033[0;2m Press any key to continue...\033[0m\";"
"}\n"
"pick() {\n"
#ifdef PICK
PICK ";\n"
#else
"    ask query \"$1\" && awk '{print length, $1}' | sort -n | cut -d' ' -f2- |\n"
"      grep -i -m1 \"$(echo \"$query\" | sed 's;.;[^/&]*[&];g')\";\n"
#endif
"}\n"
#ifdef SH
"alias sh="SH";\n"
#else
#define SH "sh"
#endif
;

static const char *runstartup = 
"[ ! \"$XDG_CONFIG_HOME\" ] && XDG_CONFIG_HOME=~/.config;\n"
"[ ! \"$sysconfdir\" ] && sysconfdir=/etc;\n"
"for path in \"$XDG_CONFIG_HOME/bb\" \"$sysconfdir/xdg/bb\" .; do\n"
"    if [ -e \"$path/bbstartup.sh\" ]; then\n"
"        . \"$path/bbstartup.sh\";\n"
"        break;\n"
"    fi;\n"
"done\n";

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
