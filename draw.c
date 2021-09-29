//
// draw.c - This file contains logic for drawing columns.
//

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>

#include "draw.h"
#include "terminal.h"
#include "types.h"
#include "utils.h"

column_t column_info[255] = {
    ['*'] = {.name = "*",          .render = col_selected},
    ['n'] = {.name = "Name",       .render = col_name, .stretchy = 1},
    ['s'] = {.name = " Size",      .render = col_size},
    ['p'] = {.name = "Perm",       .render = col_perm},
    ['m'] = {.name = " Modified",  .render = col_mreltime},
    ['M'] = {.name = "     Modified     ", .render = col_mtime},
    ['a'] = {.name = " Accessed",  .render = col_areltime},
    ['A'] = {.name = "     Accessed     ", .render = col_atime},
    ['c'] = {.name = " Created",   .render = col_creltime},
    ['C'] = {.name = "     Created      ",  .render = col_ctime},
    ['r'] = {.name = "Random",     .render = col_random},
};

//
// Left-pad a string with spaces.
//
static void lpad(char *buf, int width)
{
    int len = strlen(buf);
    if (len < width) {
        int pad = width - len;
        memmove(&buf[pad], buf, (size_t)len + 1);
        while (pad > 0) buf[--pad] = ' ';
    }
}

//
// Append a string to an existing string, but with escape sequences made explicit.
//
static char* stpcpy_escaped(char *buf, const char *str, const char *color)
{
    static const char *escapes = "       abtnvfr             e";
    for (const char *c = str; *c; ++c) {
        if (*c > 0 && *c <= '\x1b' && escapes[(int)*c] != ' ') { // "\n", etc.
            buf += sprintf(buf, "\033[31m\\%c%s", escapes[(int)*c], color);
        } else if (*c >= 0 && !(' ' <= *c && *c <= '~')) { // "\x02", etc.
            buf += sprintf(buf, "\033[31m\\x%02X%s", (unsigned int)*c, color);
        } else {
            *(buf++) = *c;
        }
    }
    *buf = '\0';
    return buf;
}

//
// Print a string, but replacing bytes like '\n' with a red-colored "\n".
// The color argument is what color to put back after the red.
// Returns the number of bytes that were escaped.
//
static int fputs_escaped(FILE *f, const char *str, const char *color)
{
    static const char *escapes = "       abtnvfr             e";
    int escaped = 0;
    for (const char *c = str; *c; ++c) {
        if (*c > 0 && *c <= '\x1b' && escapes[(int)*c] != ' ') { // "\n", etc.
            fprintf(f, "\033[31m\\%c%s", escapes[(int)*c], color);
            ++escaped;
        } else if (*c >= 0 && !(' ' <= *c && *c <= '~')) { // "\x02", etc.
            fprintf(f, "\033[31m\\x%02X%s", (unsigned int)*c, color);
            ++escaped;
        } else {
            fputc(*c, f);
        }
    }
    return escaped;
}

//
// Return a human-readable string representing how long ago a time was.
//
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
    int mag = 0;
    const char* units = "BKMGTPEZY";
    double bytes = (double)entry->info.st_size;
    while (bytes > 1024 && units[mag+1]) {
        bytes /= 1024;
        mag++;
    }
    // Add 1 extra digit of precision if it would be nonzero:
    sprintf(buf, "%5.*f%c ", ((int)(bytes*10.0 + 0.5)%10 >= 1) ? 1 : 0, bytes, units[mag]);
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

//
// Calculate the column widths.
//
int *get_column_widths(char columns[], int width)
{
    // TODO: maybe memoize
    static int colwidths[16] = {0};
    int space = width, nstretchy = 0;
    for (int c = 0; columns[c]; c++) {
        column_t col = column_info[(int)columns[c]];
        if (!col.name) continue;
        if (col.stretchy) {
            ++nstretchy;
        } else {
            colwidths[c] = strlen(col.name) + 1;
            space -= colwidths[c];
        }
        if (c > 0) --space;
    }
    for (int c = 0; columns[c]; c++)
        if (column_info[(int)columns[c]].stretchy)
            colwidths[c] = space / nstretchy;
    return colwidths;
}

//
// Draw the column header labels.
//
void draw_column_labels(FILE *out, char columns[], char *sort, int width)
{
    int *colwidths = get_column_widths(columns, width);
    fputs("\033[0;44;30m\033[K", out);
    int x = 0;
    for (int c = 0; columns[c]; c++) {
        column_t col = column_info[(int)columns[c]];
        if (!col.name) continue;
        const char *title = col.name;
        move_cursor_col(out, x);
        if (c > 0) {
            fputs("┃\033[K", out);
            x += 1;
        }
        const char *indicator = " ";
        if (columns[c] == sort[1])
            indicator = sort[0] == '-' ? RSORT_INDICATOR : SORT_INDICATOR;
        move_cursor_col(out, x);
        fputs(indicator, out);
        if (title) fputs(title, out);
        x += colwidths[c];
    }
    fputs(" \033[K\033[0m", out);
}

//
// Draw a row (one file).
//
void draw_row(FILE *out, char columns[], entry_t *entry, const char *color, int width)
{
    int *colwidths = get_column_widths(columns, width);
    fputs(color, out);
    int x = 0;
    for (int c = 0; columns[c]; c++) {
        column_t col = column_info[(int)columns[c]];
        if (!col.name) continue;
        move_cursor_col(out, x);
        if (c > 0) { // Separator |
            fprintf(out, "\033[37;2m┃\033[22m%s", color);
            x += 1;
        }
        char buf[PATH_MAX * 2] = {0};
        col.render(entry, color, buf, colwidths[c]);
        fprintf(out, "%s\033[K", buf);
        x += colwidths[c];
    }
    fputs("\033[0m", out);
}

//
// Draw everything to the screen.
// If `bb->dirty` is false, then use terminal scrolling to move the file
// listing around and only update the files that have changed.
//
void render(FILE *out, bb_t *bb)
{
    static int lastcursor = -1, lastscroll = -1;
    static struct winsize oldsize = {0};

    struct winsize winsize;
    ioctl(STDIN_FILENO, TIOCGWINSZ, &winsize);
    int onscreen  = winsize.ws_row - 3;

    bb->dirty |= (winsize.ws_row != oldsize.ws_row) || (winsize.ws_col != oldsize.ws_col);
    oldsize = winsize;

    if (!bb->dirty) {
        // Use terminal scrolling:
        if (lastscroll > bb->scroll) {
            fprintf(out, "\033[3;%dr\033[%dT\033[1;%dr", winsize.ws_row-1, lastscroll - bb->scroll, winsize.ws_row);
        } else if (lastscroll < bb->scroll) {
            fprintf(out, "\033[3;%dr\033[%dS\033[1;%dr", winsize.ws_row-1, bb->scroll - lastscroll, winsize.ws_row);
        }
    }

    if (bb->dirty) {
        // Path
        move_cursor(out, 0, 0);
        const char *color = TITLE_COLOR;
        fputs(color, out);

        char *home = getenv("HOME");
        if (home && strncmp(bb->path, home, strlen(home)) == 0) {
            fputs("~", out);
            fputs_escaped(out, bb->path + strlen(home), color);
        } else {
            fputs_escaped(out, bb->path, color);
        }
        fprintf(out, "\033[0;2m[%s]", bb->globpats);
        fputs(" \033[K\033[0m", out);

        static const char *help = "Press '?' to see key bindings ";
        move_cursor(out, MAX(0, winsize.ws_col - (int)strlen(help)), 0);
        fputs(help, out);
        fputs("\033[K\033[0m", out);

        // Columns
        move_cursor(out, 0, 1);
        fputs("\033[0;44;30m\033[K", out);
        draw_column_labels(out, bb->columns, bb->sort, winsize.ws_col-1);
    }

    if (bb->nfiles == 0) {
        move_cursor(out, 0, 2);
        fputs("\033[37;2m ...no files here... \033[0m\033[J", out);
    } else {
        entry_t **files = bb->files;
        for (int i = bb->scroll; i < bb->scroll + onscreen && i < bb->nfiles; i++) {
            if (!(bb->dirty || i == bb->cursor || i == lastcursor ||
                  i < lastscroll || i >= lastscroll + onscreen)) {
                continue;
            }

            entry_t *entry = files[i];
            const char *color = NORMAL_COLOR;
            if (i == bb->cursor) color = CURSOR_COLOR;
            else if (S_ISDIR(entry->info.st_mode)) color = DIR_COLOR;
            else if (S_ISLNK(entry->info.st_mode)) color = LINK_COLOR;
            else if (entry->info.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH))
                color = EXECUTABLE_COLOR;

            int x = 0, y = i - bb->scroll + 2;
            move_cursor(out, x, y);
            draw_row(out, bb->columns, entry, color, winsize.ws_col-1);
        }
        move_cursor(out, 0, MIN(bb->nfiles - bb->scroll, onscreen) + 2);
        fputs("\033[J", out);
    }

    // Scrollbar:
    if (bb->nfiles > onscreen) {
        int height = (onscreen*onscreen + (bb->nfiles-1))/bb->nfiles;
        int start = 2 + (bb->scroll*onscreen)/bb->nfiles;
        for (int i = 2; i < 2 + onscreen; i++) {
            move_cursor(out, winsize.ws_col-1, i);
            fprintf(out, "%s\033[0m",
                (i >= start && i < start + height) ?  SCROLLBAR_FG : SCROLLBAR_BG);
        }
    }

    // Bottom Line:
    move_cursor(out, winsize.ws_col/2, winsize.ws_row - 1);
    fputs("\033[0m\033[K", out);
    int x = winsize.ws_col;
    if (bb->selected) { // Number of selected files
        int n = 0;
        for (entry_t *s = bb->selected; s; s = s->selected.next) ++n;
        x -= 14;
        for (int k = n; k; k /= 10) x--;
        move_cursor(out, MAX(0, x), winsize.ws_row - 1);
        fprintf(out, "\033[41;30m %d Selected \033[0m", n);
    }
    int nprocs = 0;
    for (proc_t *p = bb->running_procs; p; p = p->running.next) ++nprocs;
    if (nprocs > 0) { // Number of suspended processes
        x -= 13;
        for (int k = nprocs; k; k /= 10) x--;
        move_cursor(out, MAX(0, x), winsize.ws_row - 1);
        fprintf(out, "\033[44;30m %d Suspended \033[0m", nprocs);
    }
    move_cursor(out, winsize.ws_col/2, winsize.ws_row - 1);

    lastcursor = bb->cursor;
    lastscroll = bb->scroll;
    fflush(out);
    bb->dirty = 0;
}
// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1,\:0
