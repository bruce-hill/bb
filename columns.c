/*
 * columns.c - This file contains logic for drawing columns.
 */

#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/stat.h>

#include "columns.h"
#include "entry.h"

#define E_ISDIR(e) (S_ISDIR(S_ISLNK((e)->info.st_mode) ? (e)->linkedmode : (e)->info.st_mode))

static void lpad(char *buf, int width)
{
    int len = strlen(buf);
    if (len < width) {
        int pad = width - len;
        memmove(&buf[pad], buf, (size_t)len + 1);
        while (pad > 0) buf[--pad] = ' ';
    }
}

static char* stpcpy_escaped(char *buf, const char *str, const char *color)
{
    static const char *escapes = "       abtnvfr             e";
    for (const char *c = str; *c; ++c) {
        if (*c > 0 && *c <= '\x1b' && escapes[(int)*c] != ' ') { // "\n", etc.
            buf += sprintf(buf, "\033[31m\\%c%s", escapes[(int)*c], color);
        } else if (*c >= 0 && !(' ' <= *c && *c <= '~')) { // "\x02", etc.
            buf += sprintf(buf, "\033[31m\\x%02X%s", *c, color);
        } else {
            *(buf++) = *c;
        }
    }
    *buf = '\0';
    return buf;
}

static void timeago(char *buf, time_t t)
{
    const int SECOND = 1;
    const int MINUTE = 60 * SECOND;
    const int HOUR = 60 * MINUTE;
    const int DAY = 24 * HOUR;
    const int MONTH = 30 * DAY;
    const int YEAR = 365 * DAY;

    time_t now = time(0);
    double delta = difftime(now, t);

    if (delta < 1.5)
        sprintf(buf, "a second");
    else if (delta < 1 * MINUTE)
        sprintf(buf, "%d seconds", (int)delta);
    else if (delta < 2 * MINUTE)
        sprintf(buf, "a minute");
    else if (delta < 1 * HOUR)
        sprintf(buf, "%d minutes", (int)delta/MINUTE);
    else if (delta < 2 * HOUR)
        sprintf(buf, "an hour");
    else if (delta < 1 * DAY)
        sprintf(buf, "%d hours", (int)delta/HOUR);
    else if (delta < 2 * DAY)
        sprintf(buf, "yesterday");
    else if (delta < 1 * MONTH)
        sprintf(buf, "%d days", (int)delta/DAY);
    else if (delta < 2 * MONTH)
        sprintf(buf, "a month");
    else if (delta < 1 * YEAR)
        sprintf(buf, "%d months", (int)delta/MONTH);
    else if (delta < 2 * YEAR)
        sprintf(buf, "a year");
    else
        sprintf(buf, "%d years", (int)delta/YEAR);
}

void col_mreltime(entry_t *entry, const char *color, char *buf, int width) {
    (void)color;
    timeago(buf, entry->info.st_mtime);
    lpad(buf, width);
}

void col_areltime(entry_t *entry, const char *color, char *buf, int width) {
    (void)color;
    timeago(buf, entry->info.st_atime);
    lpad(buf, width);
}

void col_creltime(entry_t *entry, const char *color, char *buf, int width) {
    (void)color;
    timeago(buf, entry->info.st_ctime);
    lpad(buf, width);
}

void col_mtime(entry_t *entry, const char *color, char *buf, int width) {
    (void)color;
    strftime(buf, (size_t)width, TIME_FMT, localtime(&(entry->info.st_mtime)));
}

void col_atime(entry_t *entry, const char *color, char *buf, int width) {
    (void)color;
    strftime(buf, (size_t)width, TIME_FMT, localtime(&(entry->info.st_atime)));
}

void col_ctime(entry_t *entry, const char *color, char *buf, int width) {
    (void)color;
    strftime(buf, (size_t)width, TIME_FMT, localtime(&(entry->info.st_ctime)));
}

void col_selected(entry_t *entry, const char *color, char *buf, int width) {
    (void)width;
    buf = stpcpy(buf, IS_SELECTED(entry) ? SELECTED_INDICATOR : NOT_SELECTED_INDICATOR);
    buf = stpcpy(buf, color);
}

void col_perm(entry_t *entry, const char *color, char *buf, int width) {
    (void)color; (void)width;
    sprintf(buf, " %03o", entry->info.st_mode & 0777);
}

void col_random(entry_t *entry, const char *color, char *buf, int width)
{
    (void)color;
    sprintf(buf, "%*d", width, entry->shufflepos);
}

void col_size(entry_t *entry, const char *color, char *buf, int width)
{
    (void)color; (void)width;
    int j = 0;
    const char* units = "BKMGTPEZY";
    double bytes = (double)entry->info.st_size;
    while (bytes > 1024) {
        bytes /= 1024;
        j++;
    }
    sprintf(buf, " %6.*f%c ", j > 0 ? 1 : 0, bytes, units[j]);
}

void col_name(entry_t *entry, const char *color, char *buf, int width)
{
    (void)width;
    if (entry->no_esc) buf = stpcpy(buf, entry->name);
    else buf = stpcpy_escaped(buf, entry->name, color);

    if (E_ISDIR(entry)) buf = stpcpy(buf, "/");

    if (!entry->linkname) return;

    buf = stpcpy(buf, "\033[2m -> \033[3m");
    buf = stpcpy(buf, color);
    if (entry->link_no_esc) buf = stpcpy(buf, entry->linkname);
    else buf = stpcpy_escaped(buf, entry->linkname, color);

    if (S_ISDIR(entry->linkedmode))
        buf = stpcpy(buf, "/");

    buf = stpcpy(buf, "\033[22;23m");
}
