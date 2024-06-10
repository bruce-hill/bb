//
// utils.h
// Copyright 2021 Bruce Hill
// Released under the MIT license with the Commons Clause
//
// This file contains some definitions of some utility macros and functions.
//

#ifndef FILE_UTILS__H
#define FILE_UTILS__H

#include <string.h>

#ifndef streq
#define streq(a,b) (strcmp(a,b)==0)
#endif

#define MAX(a,b) ((a) < (b) ? (b) : (a))
#define MIN(a,b) ((a) > (b) ? (b) : (a))

// Platform-dependent time strucutre accessors:
#ifdef __APPLE__
#define get_mtime(s) (s).st_mtimespec
#define get_atime(s) (s).st_atimespec
#define get_ctime(s) (s).st_ctimespec
#else
#define get_mtime(s) (s).st_mtim
#define get_atime(s) (s).st_atim
#define get_ctime(s) (s).st_ctim
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

#define LEN(a) (sizeof(a)/sizeof(a[0]))
#define FOREACH(type, var, array) for (type var = array; (var) < &(array)[LEN(array)]; var++)

#define S1(x) #x
#define S2(x) S1(x)
#define __LOCATION__ __FILE__ ":" S2(__LINE__)

// Error checking helper macros:
#define nonnegative(exp, ...) check_nonnegative(exp, __LOCATION__ ": `" #exp "` " __VA_ARGS__)
#define nonnull(exp, ...) check_nonnull(exp, __LOCATION__ ": `" #exp "` " __VA_ARGS__)
// Error-checking memory allocation helper macros:
#define new(t) check_nonnull(calloc(1, sizeof(t)), __LOCATION__ ": new(" #t ") failed")
#define new_bytes(n) check_nonnull(calloc(1, n), __LOCATION__ ": new_bytes(" #n ") failed")
#define grow(obj, new_count) check_nonnull(reallocarray(obj, (new_count), sizeof(obj[0])), __LOCATION__ ": grow(" #obj ", " #new_count ") failed")
#define check_strdup(s) check_nonnull(strdup(s), __LOCATION__ ": check_strdup(" #s ") failed")

int check_nonnegative(int negative_err, const char *err_msg, ...);
__attribute__((returns_nonnull))
void *check_nonnull(void *p, const char *err_msg, ...);
__attribute__((nonnull))
void delete(void *p);

#endif
// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1,\:0
