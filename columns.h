/*
 * This file contains logic for drawing columns.
 */

static void lpad(char *buf, int width)
{
    int len = strlen(buf);
    if (len < width) {
        int pad = width - len;
        memmove(&buf[pad], buf, (size_t)len + 1);
        while (pad > 0) buf[--pad] = ' ';
    }
}

/*
 * Returns the color of a file listing, given its mode.
 */
static const char* color_of(mode_t mode)
{
    if (S_ISDIR(mode)) return DIR_COLOR;
    else if (S_ISLNK(mode)) return LINK_COLOR;
    else if (mode & (S_IXUSR | S_IXGRP | S_IXOTH))
        return EXECUTABLE_COLOR;
    else return NORMAL_COLOR;
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
    *(buf++) = '\0';
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

void col_mreltime(bb_t *bb, entry_t *entry, char *buf, int width) {
    (void)bb;
    timeago(buf, entry->info.st_mtime);
    lpad(buf, width);
}

void col_areltime(bb_t *bb, entry_t *entry, char *buf, int width) {
    (void)bb;
    timeago(buf, entry->info.st_atime);
    lpad(buf, width);
}

void col_creltime(bb_t *bb, entry_t *entry, char *buf, int width) {
    (void)bb;
    timeago(buf, entry->info.st_ctime);
    lpad(buf, width);
}

void col_mtime(bb_t *bb, entry_t *entry, char *buf, int width) {
    (void)bb;
    strftime(buf, (size_t)width, BB_TIME_FMT, localtime(&(entry->info.st_mtime)));
}

void col_atime(bb_t *bb, entry_t *entry, char *buf, int width) {
    (void)bb;
    strftime(buf, (size_t)width, BB_TIME_FMT, localtime(&(entry->info.st_atime)));
}

void col_ctime(bb_t *bb, entry_t *entry, char *buf, int width) {
    (void)bb;
    strftime(buf, (size_t)width, BB_TIME_FMT, localtime(&(entry->info.st_ctime)));
}

void col_selected(bb_t *bb, entry_t *entry, char *buf, int width) {
    (void)bb; (void)width;
    strcpy(buf, IS_SELECTED(entry) ? SELECTED_INDICATOR : NOT_SELECTED_INDICATOR);
}

void col_perm(bb_t *bb, entry_t *entry, char *buf, int width) {
    (void)bb; (void)width;
    sprintf(buf, " %03o", entry->info.st_mode & 0777);
}

void col_random(bb_t *bb, entry_t *entry, char *buf, int width)
{
    (void)width;
    double k = (double)entry->shufflepos/(double)bb->nfiles;
    int color = (int)(k*232 + (1.-k)*255);
    sprintf(buf, "\033[48;5;%dm  \033[0m%s", color,
            entry == bb->files[bb->cursor] ? CURSOR_COLOR : "\033[0m");
}

void col_size(bb_t *bb, entry_t *entry, char *buf, int width)
{
    (void)bb; (void)width;
    int j = 0;
    const char* units = "BKMGTPEZY";
    double bytes = (double)entry->info.st_size;
    while (bytes > 1024) {
        bytes /= 1024;
        j++;
    }
    sprintf(buf, " %6.*f%c ", j > 0 ? 1 : 0, bytes, units[j]);
}

void col_name(bb_t *bb, entry_t *entry, char *buf, int width)
{
    (void)width;
    char color[128];
    strcpy(color, color_of(entry->info.st_mode));
    if (entry == bb->files[bb->cursor]) strcpy(color, CURSOR_COLOR);
    buf = stpcpy(buf, color);

    int use_fullname =  strcmp(bb->path, "<selection>") == 0;
    char *name = use_fullname ? entry->fullname : entry->name;
    if (entry->no_esc) buf = stpcpy(buf, name);
    else buf = stpcpy_escaped(buf, name, color);

    if (E_ISDIR(entry)) buf = stpcpy(buf, "/");

    if (!entry->linkname) return;

    if (entry != bb->files[bb->cursor])
        buf = stpcpy(buf, "\033[37m");
    buf = stpcpy(buf, "\033[2m -> \033[3m");
    strcpy(color, color_of(entry->linkedmode));
    if (entry == bb->files[bb->cursor]) strcat(color, CURSOR_COLOR);
    strcat(color, "\033[3;2m");
    buf = stpcpy(buf, color);
    if (entry->link_no_esc) buf = stpcpy(buf, entry->linkname);
    else buf = stpcpy_escaped(buf, entry->linkname, color);

    if (S_ISDIR(entry->linkedmode))
        buf = stpcpy(buf, "/");

    buf = stpcpy(buf, "\033[22;23m");
}

typedef struct {
    const char *name;
    void (*render)(bb_t *, entry_t *, char *, int);
} column_t;

static column_t columns[255] = {
    ['*'] = {.name = "*", .render = col_selected},
    ['n'] = {.name = "Name                                             ", .render = col_name},
    ['s'] = {.name = "    Size", .render = col_size},
    ['p'] = {.name = "Perm", .render = col_perm},
    ['m'] = {.name = " Modified", .render = col_mreltime},
    ['M'] = {.name = "    Modified     ", .render = col_mtime},
    ['a'] = {.name = " Accessed", .render = col_areltime},
    ['A'] = {.name = "    Accessed     ", .render = col_atime},
    ['c'] = {.name = "  Created", .render = col_creltime},
    ['C'] = {.name = "     Created     ", .render = col_ctime},
    ['r'] = {.name = "R", .render = col_random},
};
