/*
 * Bitty Browser (bb)
 * Copyright 2019 Bruce Hill
 * Released under the MIT license
 *
 * This file contains definitions and customization for `bb`.
 */
#ifndef FILE_BB__H
#define FILE_BB__H

#include "bterm.h"
#include "entry.h"

// Macros:
#define BB_VERSION "0.27.0"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MAX_COLS 12
#define MAX_SORT (2*MAX_COLS)
#define HASH_SIZE 1024
#define HASH_MASK (HASH_SIZE - 1)
#define MAX_BINDINGS 1024

// Configurable options:
#define SCROLLOFF   MIN(5, (winsize.ws_row-4)/2)
// Colors (using ANSI escape sequences):
#define TITLE_COLOR      "\033[37;1m"
#define NORMAL_COLOR     "\033[37m"
#define CURSOR_COLOR     "\033[43;30;1m"
#define LINK_COLOR       "\033[35m"
#define DIR_COLOR        "\033[34m"
#define EXECUTABLE_COLOR "\033[31m"
#define SCROLLBAR_FG "\033[48;5;247m "
#define SCROLLBAR_BG "\033[48;5;239m "

#define MAX(a,b) ((a) < (b) ? (b) : (a))
#define MIN(a,b) ((a) > (b) ? (b) : (a))
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
    bb->dirty = 1; \
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

// For keeping track of child processes
typedef struct proc_s {
    pid_t pid;
    struct {
        struct proc_s *next, **atme;
    } running;
} proc_t;

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
    unsigned int dirty : 1;
    proc_t *running_procs;
} bb_t;

// Hack to get TinyCC (TCC) compilation to work:
// https://lists.nongnu.org/archive/html/tinycc-devel/2018-07/msg00000.html
#ifdef __TINYC__
void * __dso_handle __attribute((visibility("hidden"))) = &__dso_handle;
#endif

#endif
// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
