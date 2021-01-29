//
// utils.h
// Copyright 2020 Bruce Hill
// Released under the MIT license with the Commons Clause
//
// This file contains some definitions of some utility macros.
//

#ifndef FILE_UTILS__H
#define FILE_UTILS__H

#include <stdio.h>

#define MAX(a,b) ((a) < (b) ? (b) : (a))
#define MIN(a,b) ((a) > (b) ? (b) : (a))

#define ONSCREEN (winsize.ws_row - 3)

// Platform-dependent time strucutre accessors:
#ifdef __APPLE__
#define mtime(s) (s).st_mtimespec
#define atime(s) (s).st_atimespec
#define ctime(s) (s).st_ctimespec
#else
#define mtime(s) (s).st_mtim
#define atime(s) (s).st_atim
#define ctime(s) (s).st_ctim
#endif

// Error reporting macros:
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

// Entry macros
#define IS_SELECTED(e) (((e)->selected.atme) != NULL)
#define IS_VIEWED(e) ((e)->index >= 0)
#define IS_LOADED(e) ((e)->hash.atme != NULL)

#define E_ISDIR(e) (S_ISDIR(S_ISLNK((e)->info.st_mode) ? (e)->linkedmode : (e)->info.st_mode))

// Linked list macros
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


#endif
// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
