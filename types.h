//
// types.h
// Copyright 2020 Bruce Hill
// Released under the MIT license with the Commons Clause
//
// This file contains definitions of different types.
//
#ifndef FILE_TYPES__H
#define FILE_TYPES__H

#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "terminal.h"

#define MAX_COLS 12
#define MAX_SORT (2*MAX_COLS)
#define HASH_SIZE 1024
#define HASH_MASK (HASH_SIZE - 1)

//
// Datastructure for file/directory entries.
// entry_t uses intrusive linked lists.  This means entries can only belong to
// one list at a time, in this case the list of selected entries. 'atme' is an
// indirect pointer to either the 'next' field of the previous list member, or
// the variable that points to the first list member. In other words,
// item->next->atme == &item->next and firstitem->atme == &firstitem.
//
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

// For keeping track of child processes:
typedef struct proc_s {
    pid_t pid;
    struct {
        struct proc_s *next, **atme;
    } running;
} proc_t;

// Structure for bb program state:
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

// Key bindings:
typedef struct {
    int key;
    char *script;
    char *description;
} binding_t;

#endif
// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
