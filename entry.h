/*
 * entry.h - Define types for file entries.
 */
#ifndef FILE_ENTRY__H
#define FILE_ENTRY__H

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#define IS_SELECTED(p) (((p)->selected.atme) != NULL)
#define IS_VIEWED(p) ((p)->index >= 0)
#define IS_LOADED(p) ((p)->hash.atme != NULL)

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

#endif
