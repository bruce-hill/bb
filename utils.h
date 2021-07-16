//
// utils.h
// Copyright 2020 Bruce Hill
// Released under the MIT license with the Commons Clause
//
// This file contains some definitions of some utility macros.
//

#ifndef FILE_UTILS__H
#define FILE_UTILS__H

#include <err.h>
#include <stdio.h>
#include <string.h>

#ifndef streq
#define streq(a,b) (strcmp(a,b)==0)
#endif

#define MAX(a,b) ((a) < (b) ? (b) : (a))
#define MIN(a,b) ((a) > (b) ? (b) : (a))

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
