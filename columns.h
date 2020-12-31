/*
 * columns.h - This file contains definitions for bb column-drawing code.
 */

#ifndef FILE_COLUMNS__H
#define FILE_COLUMNS__H

#include "entry.h"

#define TIME_FMT " %T %D "
#define SELECTED_INDICATOR " \033[31;7m \033[0m"
#define NOT_SELECTED_INDICATOR "  "

typedef struct {
    const char *name;
    void (*render)(entry_t*, const char*, char*, int);
    unsigned int stretchy : 1;
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

void col_mreltime(entry_t *entry, const char *color, char *buf, int width);
void col_areltime(entry_t *entry, const char *color, char *buf, int width);
void col_creltime(entry_t *entry, const char *color, char *buf, int width);
void col_mtime(entry_t *entry, const char *color, char *buf, int width);
void col_atime(entry_t *entry, const char *color, char *buf, int width);
void col_ctime(entry_t *entry, const char *color, char *buf, int width);
void col_selected(entry_t *entry, const char *color, char *buf, int width);
void col_perm(entry_t *entry, const char *color, char *buf, int width);
void col_random(entry_t *entry, const char *color, char *buf, int width);
void col_size(entry_t *entry, const char *color, char *buf, int width);
void col_name(entry_t *entry, const char *color, char *buf, int width);

#endif
