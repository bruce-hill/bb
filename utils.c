//
// utils.c
// Copyright 2021 Bruce Hill
// Released under the MIT license with the Commons Clause
//
// This file contains implementations of some convenience functions for more
// easily error checking.
//

#include <err.h>
#include <stdarg.h>
#include <stdlib.h>

//
// If the given argument is nonnegative, print the error message and exit with
// failure. Otherwise, return the given argument.
//
int check_nonnegative(int negative_err, const char *err_msg, ...)
{
    if (negative_err < 0) {
        va_list args;
        va_start(args, err_msg);
        verr(EXIT_FAILURE, err_msg, args);
        va_end(args);
    }
    return negative_err;
}

//
// If the given argument is NULL, print the error message and exit with
// failure. Otherwise return the given argument.
//
void *check_nonnull(void *p, const char *err_msg, ...)
{
    if (p == NULL) {
        va_list args;
        va_start(args, err_msg);
        verr(EXIT_FAILURE, err_msg, args);
        va_end(args);
    }
    return p;
}

//
// For a given pointer to a memory-allocated pointer, free its memory and set
// the pointer to NULL. (This is a safer alternative to free() that
// automatically NULLs out the pointer so it can't be used after freeing)
//
void delete(void *p)
{
    if (*(void**)p != NULL) {
        free(*(void**)p);
        *(void**)p = NULL;
    }
}

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
