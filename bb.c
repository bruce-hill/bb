/*
 * Bitty Browser (bb)
 * Copyright 2019 Bruce Hill
 * Released under the MIT license
 *
 * This file contains the main source code of `bb`.
 */

#include "bb.h"

// Variables used within this file to track global state
static struct termios orig_termios, bb_termios;
static FILE *tty_out = NULL, *tty_in = NULL;
static int termwidth, termheight;
static char *cmdfilename = NULL;

/*
 * Use bb to browse a path.
 */
void bb_browse(bb_t *bb, const char *path)
{
    static long cmdpos = 0;
    int lastwidth = termwidth, lastheight = termheight;
    int check_cmds = 1;

    cd_to(bb, path);
    bb->scroll = 0;
    bb->cursor = 0;
    bb->dirty = 0;

    init_term();
    goto force_check_cmds;

  redraw:
    render(bb);
    bb->dirty = 0;

  next_input:
    if (termwidth != lastwidth || termheight != lastheight) {
        lastwidth = termwidth; lastheight = termheight;
        bb->dirty = 1;
        goto redraw;
    }

    if (check_cmds) {
        FILE *cmdfile;
      force_check_cmds:
        cmdfile = fopen(cmdfilename, "r");
        if (!cmdfile) {
            if (bb->dirty) goto redraw;
            goto get_keyboard_input;
        }

        if (cmdpos) 
            fseek(cmdfile, cmdpos, SEEK_SET);

        char *cmd = NULL;
        size_t space = 0;
        while (cmdfile && getdelim(&cmd, &space, '\0', cmdfile) >= 0) {
            cmdpos = ftell(cmdfile);
            if (!cmd[0]) continue;
            process_cmd(bb, cmd);
            if (bb->dirty) {
                free(cmd);
                fclose(cmdfile);
                goto redraw;
            }
            if (bb->should_quit) {
                free(cmd);
                fclose(cmdfile);
                goto quit;
            }
        }
        free(cmd);
        fclose(cmdfile);
        unlink(cmdfilename);
        cmdpos = 0;
        check_cmds = 0;
        goto redraw;
    }

    int key, mouse_x, mouse_y;
    static struct timespec lastclick = {0, 0};
  get_keyboard_input:
    key = bgetkey(tty_in, &mouse_x, &mouse_y);
    switch (key) {
        case KEY_MOUSE_LEFT: {
            struct timespec clicktime;
            clock_gettime(CLOCK_MONOTONIC, &clicktime);
            double dt_ms = 1e3*(double)(clicktime.tv_sec - lastclick.tv_sec);
            dt_ms += 1e-6*(double)(clicktime.tv_nsec - lastclick.tv_nsec);
            lastclick = clicktime;
            // Get column:
            char column[3] = "+?";
            for (int col = 0, x = 0; bb->columns[col]; col++) {
                if (col > 0) x += 1;
                x += columns[(int)bb->columns[col]].width;
                if (x >= mouse_x) {
                    column[1] = bb->columns[col];
                    break;
                }
            }
            if (mouse_y == 1) {
                char *pos;
                if ((pos = strstr(bb->sort, column)) && pos == bb->sort)
                    column[0] = '-';
                set_sort(bb, column);
                sort_files(bb);
                goto redraw;
            } else if (2 <= mouse_y && mouse_y <= termheight - 2) {
                int clicked = bb->scroll + (mouse_y - 2);
                if (clicked > bb->nfiles - 1) goto next_input;
                if (column[1] == COL_SELECTED) {
                    set_selected(bb, bb->files[clicked], !IS_SELECTED(bb->files[clicked]));
                    bb->dirty = 1;
                    goto redraw;
                }
                set_cursor(bb, clicked);
                if (dt_ms <= 200) {
                    key = KEY_MOUSE_DOUBLE_LEFT;
                    goto user_bindings;
                }
                goto redraw;
            }
            break;
        }

        case KEY_CTRL_C:
            cleanup_and_exit(SIGINT);

        case KEY_CTRL_Z:
            fputs(T_LEAVE_BBMODE, tty_out);
            restore_term(&orig_termios);
            raise(SIGTSTP);
            init_term();
            bb->dirty = 1;
            goto redraw;

        case -1:
            goto next_input;

        default: {
            // Search user-defined key bindings
            binding_t *binding;
          user_bindings:
            for (int i = 0; bindings[i].key != 0 && i < sizeof(bindings)/sizeof(bindings[0]); i++) {
                if (key == bindings[i].key) {
                    binding = &bindings[i];
                    goto run_binding;
                }
            }
            // Nothing matched
            goto next_input;

          run_binding:
            if (cmdpos != 0)
                err("Command file still open");
            if (is_simple_bbcmd(binding->script)) {
                process_cmd(bb, binding->script);
            } else {
                move_cursor(tty_out, 0, termheight-1);
                restore_term(&default_termios);
                run_script(bb, binding->script);
                init_term();
                check_cmds = 1;
            }
            if (bb->should_quit) goto quit;
            goto redraw;
        }
    }
    goto next_input;

  quit:
    if (tty_out) {
        fputs(T_LEAVE_BBMODE, tty_out);
        cleanup();
    }
}

int cd_to(bb_t *bb, const char *path)
{
    char pbuf[PATH_MAX], prev[PATH_MAX] = {0};
    strcpy(prev, bb->path);
    if (strcmp(path, "<selection>") == 0) {
        strcpy(pbuf, path);
    } else if (strcmp(path, "..") == 0 && strcmp(bb->path, "<selection>") == 0) {
        if (!bb->prev_path[0]) return -1;
        strcpy(pbuf, bb->prev_path);
        if (chdir(pbuf)) return -1;
    } else {
        normalize_path(bb->path, path, pbuf, 1);
        if (pbuf[strlen(pbuf)-1] != '/')
            strcat(pbuf, "/");
        if (chdir(pbuf)) return -1;
    }

    if (strcmp(bb->path, "<selection>") != 0) {
        strcpy(bb->prev_path, prev);
        setenv("BBPREVPATH", bb->prev_path, 1);
    }

    strcpy(bb->path, pbuf);
    populate_files(bb, 0);
    if (prev[0]) {
        entry_t *p = load_entry(bb, prev, 0);
        if (p) {
            if (IS_VIEWED(p)) set_cursor(bb, p->index);
            else try_free_entry(p);
        }
    }
    return 0;
}

/*
 * Close safely in a way that doesn't gunk up the terminal.
 */
void cleanup_and_exit(int sig)
{
    static volatile sig_atomic_t error_in_progress = 0;
    if (error_in_progress)
        raise(sig);
    error_in_progress = 1;
    cleanup();
    signal(sig, SIG_DFL);
    raise(sig);
}

/*
 * Close the terminal, reset the screen, and delete the cmdfile
 */
void cleanup(void)
{
    if (cmdfilename) {
        unlink(cmdfilename);
        free(cmdfilename);
        cmdfilename = NULL;
    }
    if (tty_out)
        fputs(T_OFF(T_ALT_SCREEN) T_ON(T_SHOW_CURSOR), tty_out);
    restore_term(&orig_termios);
}

/*
 * Returns the color of a file listing, given its mode.
 */
const char* color_of(mode_t mode)
{
    if (S_ISDIR(mode)) return DIR_COLOR;
    else if (S_ISLNK(mode)) return LINK_COLOR;
    else if (mode & (S_IXUSR | S_IXGRP | S_IXOTH))
        return EXECUTABLE_COLOR;
    else return NORMAL_COLOR;
}

/*
 * Used for sorting, this function compares files according to the sorting-related options,
 * like bb->sort
 */
#ifdef __APPLE__
int compare_files(void *v, const void *v1, const void *v2)
#else
int compare_files(const void *v1, const void *v2, void *v)
#endif
{
#define COMPARE(a, b) if ((a) != (b)) { return sign*((a) < (b) ? 1 : -1); }
#define COMPARE_TIME(t1, t2) COMPARE((t1).tv_sec, (t2).tv_sec) COMPARE((t1).tv_nsec, (t2).tv_nsec)
    bb_t *bb = (bb_t*)v;
    const entry_t *e1 = *((const entry_t**)v1), *e2 = *((const entry_t**)v2);

    int sign = 1;
    if (!bb->interleave_dirs) {
        COMPARE(E_ISDIR(e1), E_ISDIR(e2));
    }

    for (char *sort = bb->sort + 1; *sort; sort += 2) {
        sign = sort[-1] == '-' ? -1 : 1;
        switch (*sort) {
            case COL_SELECTED: COMPARE(IS_SELECTED(e1), IS_SELECTED(e2)); break;
            case COL_NAME: {
                /* This sorting method is not identical to strverscmp(). Notably, bb's sort
                 * will order: [0, 1, 9, 00, 01, 09, 10, 000, 010] instead of strverscmp()'s
                 * order: [000, 00, 01, 010, 09, 0, 1, 9, 10]. I believe bb's sort is consistent
                 * with how people want their files grouped: all files padded to n digits
                 * will be grouped together, and files with the same padding will be sorted
                 * ordinally. This version also does case-insensitivity by lowercasing words,
                 * so the following characters come before all letters: [\]^_`
                 */
                const char *n1 = e1->name, *n2 = e2->name;
                while (*n1 && *n2) {
                    char c1 = LOWERCASE(*n1), c2 = LOWERCASE(*n2);
                    if ('0' <= c1 && c1 <= '9' && '0' <= c2 && c2 <= '9') {
                        long i1 = strtol(n1, (char**)&n1, 10);
                        long i2 = strtol(n2, (char**)&n2, 10);
                        // Shorter numbers always go before longer. In practice, I assume
                        // filenames padded to the same number of digits should be grouped
                        // together, instead of
                        // [1.png, 0001.png, 2.png, 0002.png, 3.png], it makes more sense to have:
                        // [1.png, 2.png, 3.png, 0001.png, 0002.png]
                        COMPARE((n2 - e2->name), (n1 - e1->name));
                        COMPARE(i2, i1);
                    } else {
                        COMPARE(c2, c1);
                        ++n1; ++n2;
                    }
                }
                COMPARE(LOWERCASE(*n2), LOWERCASE(*n1));
                break;
            }
            case COL_PERM: COMPARE((e1->info.st_mode & 0x3FF), (e2->info.st_mode & 0x3FF)); break;
            case COL_SIZE: COMPARE(e1->info.st_size, e2->info.st_size); break;
            case COL_MTIME: COMPARE_TIME(mtime(e1->info), mtime(e2->info)); break;
            case COL_CTIME: COMPARE_TIME(ctime(e1->info), ctime(e2->info)); break;
            case COL_ATIME: COMPARE_TIME(atime(e1->info), atime(e2->info)); break;
            case COL_RANDOM: COMPARE(e1->shufflepos, e2->shufflepos); break;
        }
    }
    return 0;
#undef COMPARE
#undef COMPARE_TIME
}

/*
 * Print a string, but replacing bytes like '\n' with a red-colored "\n".
 * The color argument is what color to put back after the red.
 * Returns the number of bytes that were escaped.
 */
int fputs_escaped(FILE *f, const char *str, const char *color)
{
    static const char *escapes = "       abtnvfr             e";
    int escaped = 0;
    for (const char *c = str; *c; ++c) {
        if (*c > 0 && *c <= '\x1b' && escapes[(int)*c] != ' ') { // "\n", etc.
            fprintf(f, "\033[31m\\%c%s", escapes[(int)*c], color);
            ++escaped;
        } else if (*c >= 0 && !(' ' <= *c && *c <= '~')) { // "\x02", etc.
            fprintf(f, "\033[31m\\x%02X%s", *c, color);
            ++escaped;
        } else {
            fputc(*c, f);
        }
    }
    return escaped;
}

/*
 * Initialize the terminal files for /dev/tty and set up some desired
 * attributes like passing Ctrl-c as a key instead of interrupting
 */
void init_term(void)
{
    static int first_time = 1;
    tty_in = fopen("/dev/tty", "r");
    tty_out = fopen("/dev/tty", "w");
    if (first_time) {
        tcgetattr(fileno(tty_out), &orig_termios);
        first_time = 0;
    }
    memcpy(&bb_termios, &orig_termios, sizeof(bb_termios));
    cfmakeraw(&bb_termios);
    bb_termios.c_cc[VMIN] = 0;
    bb_termios.c_cc[VTIME] = 1;
    if (tcsetattr(fileno(tty_out), TCSAFLUSH, &bb_termios) == -1)
        err("Couldn't tcsetattr");
    update_term_size(0);
    signal(SIGWINCH, update_term_size);
    // Initiate mouse tracking and disable text wrapping:
    fputs(T_ENTER_BBMODE, tty_out);
}

/* 
 * Return whether or not 's' is a simple bb command that doesn't need
 * a full shell instance (e.g. "bb +cd:.." or "bb +move:+1").
 */
static int is_simple_bbcmd(const char *s)
{
    if (!s) return 0;
    while (*s == ' ') ++s;
    if (s[0] != '+' && strncmp(s, "bb +", 4) != 0)
        return 0;
    const char *special = ";$&<>|\n*?\\\"'";
    for (const char *p = special; *p; ++p) {
        if (strchr(s, *p))
            return 0;
    }
    return 1;
}

/*
 * Load a file's info into an entry_t and return it (if found).
 * The returned entry must be free()ed by the caller.
 * Warning: this does not deduplicate entries, and it's best if there aren't
 * duplicate entries hanging around.
 */
entry_t* load_entry(bb_t *bb, const char *path, int clear_dots)
{
    struct stat linkedstat, filestat;
    if (!path || !path[0]) return NULL;
    if (lstat(path, &filestat) == -1) return NULL;
    char pbuf[PATH_MAX];
    normalize_path(bb->path, path, pbuf, clear_dots);
    if (pbuf[strlen(pbuf)-1] == '/' && pbuf[1])
        pbuf[strlen(pbuf)-1] = '\0';

    // Check for pre-existing:
    for (entry_t *e = bb->hash[(int)filestat.st_ino & HASH_MASK]; e; e = e->hash.next) {
        if (e->info.st_ino == filestat.st_ino && e->info.st_dev == filestat.st_dev
            // Need to check filename in case of hard links
            && strcmp(pbuf, e->fullname) == 0)
            return e;
    }

    ssize_t linkpathlen = -1;
    char linkbuf[PATH_MAX];
    if (S_ISLNK(filestat.st_mode)) {
        linkpathlen = readlink(pbuf, linkbuf, sizeof(linkbuf));
        if (linkpathlen < 0) err("Couldn't read link: '%s'", pbuf);
        linkbuf[linkpathlen] = '\0';
        while (linkpathlen > 0 && linkbuf[linkpathlen-1] == '/') linkbuf[--linkpathlen] = '\0';
        if (stat(pbuf, &linkedstat) == -1) memset(&linkedstat, 0, sizeof(linkedstat));
    }
    size_t pathlen = strlen(pbuf);
    size_t entry_size = sizeof(entry_t) + (pathlen + 1) + (size_t)(linkpathlen + 1);
    entry_t *entry = memcheck(calloc(entry_size, 1));
    char *end = stpcpy(entry->fullname, pbuf);
    if (linkpathlen >= 0)
        entry->linkname = strcpy(end + 1, linkbuf);
    if (strcmp(entry->fullname, "/") == 0) {
        entry->name = entry->fullname;
    } else {
        entry->name = strrchr(entry->fullname, '/');
        if (!entry->name) err("No slash found in '%s' from '%s'", entry->fullname, path);
        ++entry->name;
    }
    if (S_ISLNK(filestat.st_mode))
        entry->linkedmode = linkedstat.st_mode;
    entry->info = filestat;
    if (bb->hash[(int)filestat.st_ino & HASH_MASK])
        bb->hash[(int)filestat.st_ino & HASH_MASK]->hash.atme = &entry->hash.next;
    entry->hash.next = bb->hash[(int)filestat.st_ino & HASH_MASK];
    entry->hash.atme = &bb->hash[(int)filestat.st_ino & HASH_MASK];
    entry->index = -1;
    bb->hash[(int)filestat.st_ino & HASH_MASK] = entry;
    return entry;
}

/*
 * Memory allocation failures are unrecoverable in bb, so this wrapper just
 * prints an error message and exits if that happens.
 */
void* memcheck(void *p)
{
    if (!p) err("Allocation failure");
    return p;
}

/*
 * Prepend `root` to relative paths, replace "~" with $HOME.
 * If clear_dots is 1, then also replace "/foo/." with "/foo" and replace "/foo/baz/.." with "/foo".
 * The normalized path is stored in `normalized`.
 */
void normalize_path(const char *root, const char *path, char *normalized, int clear_dots)
{
    if (path[0] == '~' && (path[1] == '\0' || path[1] == '/')) {
        char *home;
        if (!(home = getenv("HOME"))) return;
        strcpy(normalized, home);
        ++path;
    } else if (path[0] == '/') {
        normalized[0] = '\0';
    } else {
        strcpy(normalized, root);
        if (root[strlen(root)-1] != '/') err("No trailing slash on root");
    }
    strcat(normalized, path);

    if (clear_dots) {
        char *src = normalized, *dest = normalized;
        while (*src) {
            while (src[0] == '/' && src[1] == '.') {
                // Replace "foo/./?" with "foo/?"
                if (src[2] == '/' || src[2] == '\0') {
                    src += 2;
                } else if (src[2] == '.' && (src[3] == '/' || src[3] == '\0')) {
                    // Replace "foo/baz/../asdf" with "foo/asdf"
                    src += 3;
                    *dest = '\0';
                    if (dest[-1] == '/') dest[-1] = '\0';
                    while (dest > normalized && *dest != '/') dest--;
                } else break;
            }
            *(dest++) = *(src++);
        }
        *dest = '\0';
    }
}

/*
 * Remove all the files currently stored in bb->files and if `bb->path` is
 * non-NULL, update `bb` with a listing of the files in `path`
 */
void populate_files(bb_t *bb, int samedir)
{
    bb->dirty = 1;

    // Clear old files (if any)
    if (bb->files) {
        for (int i = 0; i < bb->nfiles; i++) {
            bb->files[i]->index = -1;
            try_free_entry(bb->files[i]);
            bb->files[i] = NULL;
        }
        free(bb->files);
        bb->files = NULL;
    }

    int old_scroll = bb->scroll, old_cursor = bb->cursor;
    bb->nfiles = 0;
    bb->cursor = 0;
    bb->scroll = 0;

    if (!bb->path[0])
        return;

    size_t space = 0;
    if (strcmp(bb->path, "<selection>") == 0) {
        for (entry_t *e = bb->firstselected; e; e = e->selected.next) {
            if ((size_t)bb->nfiles + 1 > space)
                bb->files = memcheck(realloc(bb->files, (space += 100)*sizeof(void*)));
            e->index = bb->nfiles;
            bb->files[bb->nfiles++] = e;
        }
    } else {
        DIR *dir = opendir(bb->path);
        if (!dir)
            err("Couldn't open dir: %s", bb->path);

        for (struct dirent *dp; (dp = readdir(dir)) != NULL; ) {
            if (dp->d_name[0] == '.') {
                if (dp->d_name[1] == '.' && dp->d_name[2] == '\0') {
                    if (!bb->show_dotdot || strcmp(bb->path, "/") == 0) continue;
                } else if (dp->d_name[1] == '\0') {
                    if (!bb->show_dot) continue;
                } else if (!bb->show_dotfiles) continue;
            }
            if ((size_t)bb->nfiles + 1 > space)
                bb->files = memcheck(realloc(bb->files, (space += 100)*sizeof(void*)));
            // Don't normalize path so we can get "." and ".."
            entry_t *entry = load_entry(bb, dp->d_name, 0);
            if (!entry) err("Failed to load entry: '%s'", dp->d_name);
            entry->index = bb->nfiles;
            bb->files[bb->nfiles++] = entry;
        }
        closedir(dir);
    }

    for (int i = 0; i < bb->nfiles; i++) {
        int j = rand() / (RAND_MAX / (i + 1)); // This is not optimal, but doesn't need to be
        bb->files[i]->shufflepos = bb->files[j]->shufflepos;
        bb->files[j]->shufflepos = i;
    }

    sort_files(bb);
    if (samedir) {
        set_cursor(bb, old_cursor);
        set_scroll(bb, old_scroll);
    }
}

/*
 * Print the current key bindings
 */
void print_bindings(int fd)
{
    char buf[1000], buf2[1024];
    for (int i = 0; bindings[i].key != 0 && i < sizeof(bindings)/sizeof(bindings[0]); i++) {
        if (bindings[i].key == -1) {
            const char *label = bindings[i].description;
            sprintf(buf, "\n\033[33;1;4m\033[%dG%s\033[0m\n", (termwidth-(int)strlen(label))/2, label);
            write(fd, buf, strlen(buf));
            continue;
        }
        int to_skip = -1;
        char *p = buf;
        for (int j = i; bindings[j].key && strcmp(bindings[j].description, bindings[i].description) == 0; j++) {
            if (j > i) p = stpcpy(p, ", ");
            ++to_skip;
            int key = bindings[j].key;
            const char *name = bkeyname(key);
            if (name)
                p = stpcpy(p, name);
            else if (' ' <= key && key <= '~')
                p += sprintf(p, "%c", (char)key);
            else
                p += sprintf(p, "\033[31m\\x%02X", key);
        }
        *p = '\0';
        sprintf(buf2, "\033[1m\033[%dG%s\033[0m", termwidth/2 - 1 - (int)strlen(buf), buf);
        write(fd, buf2, strlen(buf2));
        sprintf(buf2, "\033[1m\033[%dG\033[34m%s\033[0m", termwidth/2 + 1,
                bindings[i].description);
        write(fd, buf2, strlen(buf2));
        write(fd, "\033[0m\n", strlen("\033[0m\n"));
        i += to_skip;
    }
    write(fd, "\n", 1);
}

/*
 * Run a bb internal command (e.g. "+refresh") and return an indicator of what
 * needs to happen next.
 */
bb_result_t process_cmd(bb_t *bb, const char *cmd)
{
    if (cmd[0] == '+') ++cmd;
    else if (strncmp(cmd, "bb +", 4) == 0) cmd = &cmd[4];
    const char *value = strchr(cmd, ':');
    if (value) ++value;
#define set_bool(target) do { if (!value) { target = !target; } else { target = value[0] == '1'; } } while (0)
    switch (cmd[0]) {
        case '.': { // +..:, +.:
            if (cmd[1] == '.') // +..:
                set_bool(bb->show_dotdot);
            else // +.:
                set_bool(bb->show_dot);
            populate_files(bb, 1);
            return BB_OK;
        }
        case 'b': { // +bind:<keys>:<script>
            if (!value || !value[0])
                return BB_INVALID;
            char *value_copy = memcheck(strdup(value));
            char *keys = trim(value_copy);
            if (!keys[0]) { free(value_copy); return BB_OK; }
            char *script = strchr(keys+1, ':');
            if (!script) { free(value_copy); return BB_INVALID; }
            *script = '\0';
            script = trim(script + 1);
            char *description;
            if (script[0] == '#') {
                description = trim(strsep(&script, "\n") + 1);
                if (!script) script = "";
                else script = trim(script);
            } else description = script;
            for (char *key; (key = strsep(&keys, ",")); ) {
                int is_section = strcmp(key, "Section") == 0;
                int keyval = strlen(key) == 1 ? key[0] : bkeywithname(key);
                if (keyval == -1 && !is_section) continue;
                for (int i = 0; i < sizeof(bindings)/sizeof(bindings[0]); i++) {
                    if (bindings[i].key && (bindings[i].key != keyval || is_section))
                        continue;
                    binding_t binding = {keyval, memcheck(strdup(script)),
                        memcheck(strdup(description))};
                    if (bindings[i].key == keyval) {
                        free(bindings[i].description);
                        free(bindings[i].script);
                        for (; i + 1 < sizeof(bindings)/sizeof(bindings[0]) && bindings[i+1].key; i++)
                            bindings[i] = bindings[i+1];
                    }
                    bindings[i] = binding;
                    break;
                }
            }
            free(value_copy);
            return BB_OK;
        }
        case 'c': { // +cd:, +columns:
            switch (cmd[1]) {
                case 'd': { // +cd:
                    if (!value) return BB_INVALID;
                    if (cd_to(bb, value)) return BB_INVALID;
                    return BB_OK;
                }
                case 'o': { // +columns:
                    if (!value) return BB_INVALID;
                    strncpy(bb->columns, value, MAX_COLS);
                    bb->dirty = 1;
                    return BB_OK;
                }
            }
            return BB_INVALID;
        }
        case 'd': { // +deselect:, +dotfiles:
            switch (cmd[1]) {
                case 'e': { // +deselect:
                    if (!value) {
                        while (bb->firstselected)
                            set_selected(bb, bb->firstselected, 0);
                        return BB_OK;
                    }
                    char pbuf[PATH_MAX];
                    normalize_path(bb->path, value, pbuf, 1);
                    entry_t *e = load_entry(bb, pbuf, 0);
                    if (e) {
                        set_selected(bb, e, 0);
                        return BB_OK;
                    }
                    // Filename may no longer exist:
                    for (e = bb->firstselected; e; e = e->selected.next) {
                        if (strcmp(e->fullname, pbuf) == 0) {
                            set_selected(bb, e, 0);
                            break;
                        }
                    }
                    return BB_OK;
                }
                case 'o': { // +dotfiles:
                    set_bool(bb->show_dotfiles);
                    populate_files(bb, 1);
                    return BB_OK;
                }
            }
        }
        case 'e': { // +execute:
            if (!value || !value[0]) return BB_INVALID;
            move_cursor(tty_out, 0, termheight-1);
            fputs(T_ON(T_SHOW_CURSOR), tty_out);
            restore_term(&default_termios);
            run_script(bb, value);
            init_term();
            return BB_OK;
        }
        case 'g': { // +goto:
            if (!value) return BB_INVALID;
            entry_t *e = load_entry(bb, value, 1);
            if (!e) return BB_INVALID;
            if (IS_VIEWED(e)) {
                set_cursor(bb, e->index);
                return BB_OK;
            }
            char pbuf[PATH_MAX];
            strcpy(pbuf, e->fullname);
            char *lastslash = strrchr(pbuf, '/');
            if (!lastslash) return BB_INVALID;
            *lastslash = '\0'; // Split in two
            cd_to(bb, pbuf);
            if (IS_VIEWED(e)) {
                set_cursor(bb, e->index);
                return BB_OK;
            } else try_free_entry(e);
            return BB_OK;
        }
        case 'h': { // +help
            restore_term(&default_termios);
            int fds[2];
            pipe(fds);
            pid_t child = fork();
            void (*old_handler)(int) = signal(SIGINT, SIG_IGN);
            if (child != 0) {
                close(fds[0]);
                print_bindings(fds[1]);
                close(fds[1]);
                waitpid(child, NULL, 0);
            } else {
                close(fds[1]);
                dup2(fds[0], STDIN_FILENO);
                char *args[] = {SH, "-c", "$PAGER -rX", NULL};
                execvp(SH, args);
            }
            init_term();
            signal(SIGINT, old_handler);
            bb->dirty = 1;
            return BB_OK;
        }
        case 'i': { // +interleave
            set_bool(bb->interleave_dirs);
            sort_files(bb);
            return BB_OK;
        }
        case 'm': { // +move:
            int oldcur, isdelta, n;
          move:
            if (!value) return BB_INVALID;
            if (!bb->nfiles) return BB_INVALID;
            oldcur = bb->cursor;
            isdelta = value[0] == '-' || value[0] == '+';
            n = (int)strtol(value, (char**)&value, 10);
            if (*value == '%')
                n = (n * (value[1] == 'n' ? bb->nfiles : termheight)) / 100;
            if (isdelta) set_cursor(bb, bb->cursor + n);
            else set_cursor(bb, n);
            if (cmd[0] == 's') { // +spread:
                int sel = IS_SELECTED(bb->files[oldcur]);
                for (int i = bb->cursor; i != oldcur; i += (oldcur > i ? 1 : -1))
                    set_selected(bb, bb->files[i], sel);
            }
            return BB_OK;
        }
        case 'q': // +quit
            bb->should_quit = 1;
            return BB_OK;
        case 'r': // +refresh
            populate_files(bb, 1);
            return BB_OK;
        case 's': // +scroll:, +select:, +sort:, +spread:
            switch (cmd[1]) {
                case 'c': { // scroll:
                    if (!value) return BB_INVALID;
                    // TODO: figure out the best version of this
                    int isdelta = value[0] == '+' || value[0] == '-';
                    int n = (int)strtol(value, (char**)&value, 10);
                    if (*value == '%')
                        n = (n * (value[1] == 'n' ? bb->nfiles : termheight)) / 100;
                    if (isdelta)
                        set_scroll(bb, bb->scroll + n);
                    else
                        set_scroll(bb, n);
                    return BB_OK;
                }

                case '\0': case 'e': { // +select:
                    if (!value && !bb->nfiles) return BB_INVALID;
                    if (!value) value = bb->files[bb->cursor]->fullname;
                    entry_t *e = load_entry(bb, value, 1);
                    if (e) set_selected(bb, e, 1);
                    return BB_OK;
                }
                case 'o': // +sort:
                    if (!value) return BB_INVALID;
                    set_sort(bb, value);
                    sort_files(bb);
                    return BB_OK;

                case 'p': // +spread:
                    goto move;
            }
        case 't': { // +toggle:
            if (!value && !bb->nfiles) return BB_INVALID;
            if (!value) value = bb->files[bb->cursor]->fullname;
            entry_t *e = load_entry(bb, value, 1);
            if (e) set_selected(bb, e, !IS_SELECTED(e));
            return BB_OK;
        }
        default: {
            fputs(T_LEAVE_BBMODE, tty_out);
            restore_term(&orig_termios);
            const char *msg = "Invalid bb command: ";
            write(STDERR_FILENO, msg, strlen(msg));
            write(STDERR_FILENO, cmd, strlen(cmd));
            write(STDERR_FILENO, "\n", 1);
            init_term();
            return BB_INVALID;
        }
    }
    return BB_INVALID;
}

/*
 * Draw everything to the screen.
 * If bb->dirty is false, then use terminal scrolling to move the file listing
 * around and only update the files that have changed.
 */
void render(bb_t *bb)
{
    static int lastcursor = -1, lastscroll = -1;
    char buf[64];
    if (lastcursor == -1 || lastscroll == -1)
        bb->dirty = 1;

    if (!bb->dirty) {
        // Use terminal scrolling:
        if (lastscroll > bb->scroll) {
            fprintf(tty_out, "\033[3;%dr\033[%dT\033[1;%dr", termheight-1, lastscroll - bb->scroll, termheight);
        } else if (lastscroll < bb->scroll) {
            fprintf(tty_out, "\033[3;%dr\033[%dS\033[1;%dr", termheight-1, bb->scroll - lastscroll, termheight);
        }
    }

    if (bb->dirty) {
        // Path
        move_cursor(tty_out, 0, 0);
        const char *color = TITLE_COLOR;
        fputs(color, tty_out);
        fputs_escaped(tty_out, bb->path, color);
        fputs(" \033[K\033[0m", tty_out);

        static const char *help = "Press '?' to see key bindings ";
        move_cursor(tty_out, MAX(0, termwidth - (int)strlen(help)), 0);
        fputs(help, tty_out);
        fputs("\033[K\033[0m", tty_out);

        // Columns
        move_cursor(tty_out, 0, 1);
        fputs("\033[0;44;30m\033[K", tty_out);
        int x = 0;
        for (int col = 0; bb->columns[col]; col++) {
            const char *title = columns[(int)bb->columns[col]].name;
            if (!title) title = "";
            move_cursor(tty_out, x, 1);
            if (col > 0) {
                fputs("│\033[K", tty_out);
                x += 1;
            }
            const char *indicator = " ";
            if (bb->columns[col] == bb->sort[1])
                indicator = bb->sort[0] == '-' ? RSORT_INDICATOR : SORT_INDICATOR;
            move_cursor(tty_out, x, 1);
            fputs(indicator, tty_out);
            fputs(title, tty_out);
            x += columns[(int)bb->columns[col]].width;
        }
        fputs(" \033[K\033[0m", tty_out);
    }

    entry_t **files = bb->files;
    for (int i = bb->scroll; i < bb->scroll + ONSCREEN; i++) {
        if (!bb->dirty) {
            if (i == bb->cursor || i == lastcursor)
                goto do_render;
            if (i < lastscroll || i >= lastscroll + ONSCREEN)
                goto do_render;
            continue;
        }

        int y;
      do_render:
        y = i - bb->scroll + 2;
        move_cursor(tty_out, 0, y);

        if (i == bb->scroll && bb->nfiles == 0) {
            const char *s = "...no files here...";
            fprintf(tty_out, "\033[37;2m%s\033[0m\033[K\033[J", s);
            break;
        }

        if (i >= bb->nfiles) {
            fputs("\033[J", tty_out);
            break;
        }

        entry_t *entry = files[i];
        if (i == bb->cursor) fputs(CURSOR_COLOR, tty_out);

        int use_fullname =  strcmp(bb->path, "<selection>") == 0;
        int x = 0;
        for (int col = 0; bb->columns[col]; col++) {
            fprintf(tty_out, "\033[%d;%dH\033[K", y+1, x+1);
            if (col > 0) {
                if (i == bb->cursor) fputs("│", tty_out);
                else fputs("\033[37;2m│\033[22m", tty_out);
                fputs(i == bb->cursor ? CURSOR_COLOR : "\033[0m", tty_out);
                x += 1;
            }
            move_cursor(tty_out, x, y);
            switch (bb->columns[col]) {
                case COL_SELECTED:
                    fputs(IS_SELECTED(entry) ? SELECTED_INDICATOR : NOT_SELECTED_INDICATOR, tty_out);
                    fputs(i == bb->cursor ? CURSOR_COLOR : "\033[0m", tty_out);
                    break;

                case COL_RANDOM: {
                    double k = (double)entry->shufflepos/(double)bb->nfiles;
                    int color = (int)(k*232 + (1.-k)*255);
                    fprintf(tty_out, "\033[48;5;%dm  \033[0m%s", color,
                            i == bb->cursor ? CURSOR_COLOR : "\033[0m");
                    break;
                }

                case COL_SIZE: {
                    int j = 0;
                    const char* units = "BKMGTPEZY";
                    double bytes = (double)entry->info.st_size;
                    while (bytes > 1024) {
                        bytes /= 1024;
                        j++;
                    }
                    fprintf(tty_out, " %6.*f%c ", j > 0 ? 1 : 0, bytes, units[j]);
                    break;
                }

                case COL_MTIME:
                    strftime(buf, sizeof(buf), " %I:%M%p %b %e %Y ", localtime(&(entry->info.st_mtime)));
                    fputs(buf, tty_out);
                    break;

                case COL_CTIME:
                    strftime(buf, sizeof(buf), " %I:%M%p %b %e %Y ", localtime(&(entry->info.st_ctime)));
                    fputs(buf, tty_out);
                    break;

                case COL_ATIME:
                    strftime(buf, sizeof(buf), " %I:%M%p %b %e %Y ", localtime(&(entry->info.st_atime)));
                    fputs(buf, tty_out);
                    break;

                case COL_PERM:
                    fprintf(tty_out, " %03o", entry->info.st_mode & 0777);
                    break;

                case COL_NAME: {
                    char color[128];
                    strcpy(color, color_of(entry->info.st_mode));
                    if (i == bb->cursor) strcat(color, CURSOR_COLOR);
                    fputs(color, tty_out);

                    char *name = use_fullname ? entry->fullname : entry->name;
                    if (entry->no_esc) fputs(name, tty_out);
                    else entry->no_esc |= !fputs_escaped(tty_out, name, color);

                    if (E_ISDIR(entry)) fputs("/", tty_out);

                    if (entry->linkname) {
                        if (i != bb->cursor)
                            fputs("\033[37m", tty_out);
                        fputs("\033[2m -> \033[3m", tty_out);
                        strcpy(color, color_of(entry->linkedmode));
                        if (i == bb->cursor) strcat(color, CURSOR_COLOR);
                        strcat(color, "\033[3;2m");
                        fputs(color, tty_out);
                        if (entry->link_no_esc) fputs(entry->linkname, tty_out);
                        else entry->link_no_esc |= !fputs_escaped(tty_out, entry->linkname, color);

                        if (S_ISDIR(entry->linkedmode))
                            fputs("/", tty_out);

                        fputs("\033[22;23m", tty_out);
                    }
                    fputs(i == bb->cursor ? CURSOR_COLOR : "\033[0m", tty_out);
                    fputs("\033[K", tty_out);
                    break;
                }
                default: break;
            }
            x += columns[(int)bb->columns[col]].width;
        }
        fputs(" \033[K\033[0m", tty_out); // Reset color and attributes
    }

    if (bb->firstselected) {
        int n = 0;
        for (entry_t *s = bb->firstselected; s; s = s->selected.next) ++n;
        int x = termwidth - 14;
        for (int k = n; k; k /= 10) x--;
        move_cursor(tty_out, MAX(0, x), termheight - 1);
        fprintf(tty_out, "\033[41;30m %d Selected \033[0m", n);
    } else {
        move_cursor(tty_out, termwidth/2, termheight - 1);
        fputs("\033[0m\033[K", tty_out);
    }

    lastcursor = bb->cursor;
    lastscroll = bb->scroll;
    fflush(tty_out);
}

/*
 * Close the /dev/tty terminals and restore some of the attributes.
 */
void restore_term(const struct termios *term)
{
    if (tty_out) {
        tcsetattr(fileno(tty_out), TCSAFLUSH, term);
        fputs(T_LEAVE_BBMODE_PARTIAL, tty_out);
        fflush(tty_out);
        fclose(tty_out);
        tty_out = NULL;
        fclose(tty_in);
        tty_in = NULL;
    }
    signal(SIGWINCH, SIG_DFL);
}

/*
 * Run a shell script with the selected files passed as sequential arguments to
 * the script (or pass the cursor file if none are selected).
 * Return the exit status of the script.
 */
int run_script(bb_t *bb, const char *cmd)
{
    char *fullcmd = calloc(strlen(cmd) + strlen(bbcmdfn) + 1, sizeof(char));
    strcpy(fullcmd, bbcmdfn);
    strcat(fullcmd, cmd);

    pid_t child;
    void (*old_handler)(int) = signal(SIGINT, SIG_IGN);
    if ((child = fork()) == 0) {
        signal(SIGINT, SIG_DFL);
        // TODO: is there a max number of args? Should this be batched?
        size_t space = 32;
        char **args = memcheck(calloc(space, sizeof(char*)));
        size_t i = 0;
        args[i++] = SH;
        args[i++] = "-c";
        args[i++] = fullcmd;
        args[i++] = "--"; // ensure files like "-i" are not interpreted as flags for sh
        for (entry_t *e = bb->firstselected; e; e = e->selected.next) {
            if (i >= space)
                args = memcheck(realloc(args, (space += 100)*sizeof(char*)));
            args[i++] = e->fullname;
        }
        args[i] = NULL;

        setenv("BBDOTFILES", bb->show_dotfiles ? "1" : "", 1);
        setenv("BBCURSOR", bb->nfiles ? bb->files[bb->cursor]->fullname : "", 1);
        setenv("BBSHELLFUNC", bbcmdfn, 1);

        int ttyout, ttyin;
        ttyout = open("/dev/tty", O_RDWR);
        ttyin = open("/dev/tty", O_RDONLY);
        dup2(ttyout, STDOUT_FILENO);
        dup2(ttyin, STDIN_FILENO);
        execvp(SH, args);
        err("Failed to execute command: '%s'", cmd);
        return -1;
    }

    if (child == -1)
        err("Failed to fork");

    int status;
    waitpid(child, &status, 0);
    signal(SIGINT, old_handler);

    bb->dirty = 1;
    return status;
}

/*
 * Set bb's file cursor to the given index (and adjust the scroll as necessary)
 */
void set_cursor(bb_t *bb, int newcur)
{
    int oldcur = bb->cursor;
    if (newcur > bb->nfiles - 1) newcur = bb->nfiles - 1;
    if (newcur < 0) newcur = 0;
    bb->cursor = newcur;
    if (bb->nfiles <= ONSCREEN) {
        bb->scroll = 0;
        return;
    }

    if (oldcur < bb->cursor) {
        if (bb->scroll > bb->cursor)
            bb->scroll = MAX(0, bb->cursor);
        else if (bb->scroll < bb->cursor - ONSCREEN + 1 + SCROLLOFF)
            bb->scroll = MIN(bb->nfiles - 1 - ONSCREEN + 1,
                             bb->scroll + (newcur - oldcur));
    } else {
        if (bb->scroll > bb->cursor - SCROLLOFF)
            bb->scroll = MAX(0, bb->scroll + (newcur - oldcur));//bb->cursor - SCROLLOFF);
        else if (bb->scroll < bb->cursor - ONSCREEN + 1)
            bb->scroll = MIN(bb->cursor - ONSCREEN + 1,
                             bb->nfiles - 1 - ONSCREEN + 1);
    }
}

/*
 * Set bb's scroll to the given index (and adjust the cursor as necessary)
 */
void set_scroll(bb_t *bb, int newscroll)
{
    int delta = newscroll - bb->scroll;
    if (bb->nfiles <= ONSCREEN) {
        newscroll = 0;
    } else {
        if (newscroll > bb->nfiles - 1 - ONSCREEN + 1)
            newscroll = bb->nfiles - 1 - ONSCREEN + 1;
        if (newscroll < 0) newscroll = 0;
    }

    bb->scroll = newscroll;
    bb->cursor += delta;
    if (bb->cursor > bb->nfiles - 1) bb->cursor = bb->nfiles - 1;
    if (bb->cursor < 0) bb->cursor = 0;
}

/*
 * Select or deselect a file.
 */
void set_selected(bb_t *bb, entry_t *e, int selected)
{
    if (IS_SELECTED(e) == selected) return;

    if (bb->nfiles > 0 && e != bb->files[bb->cursor])
        bb->dirty = 1;

    if (selected) {
        if (bb->firstselected)
            bb->firstselected->selected.atme = &e->selected.next;
        e->selected.next = bb->firstselected;
        e->selected.atme = &bb->firstselected;
        bb->firstselected = e;
    } else {
        if (bb->nfiles > 0 && e != bb->files[bb->cursor])
            bb->dirty = 1;
        if (e->selected.next)
            e->selected.next->selected.atme = e->selected.atme;
        *(e->selected.atme) = e->selected.next;
        e->selected.next = NULL;
        e->selected.atme = NULL;
        try_free_entry(e);
    }
}

/*
 * Set the sorting method used by bb to display files.
 */
void set_sort(bb_t *bb, const char *sort)
{
    char sortbuf[strlen(sort)+1];
    strcpy(sortbuf, sort);
    for (char *s = sortbuf; s[0] && s[1]; s += 2) {
        char *found;
        if ((found = strchr(bb->sort, s[1]))) {
            if (*s == '~')
                *s = found[-1] == '+' && found == &bb->sort[1] ? '-' : '+';
            memmove(found-1, found+1, strlen(found+1)+1);
        } else if (*s == '~')
            *s = '+';
    }
    size_t len = MIN(MAX_SORT, strlen(sort));
    memmove(bb->sort + len, bb->sort, MAX_SORT+1 - len);
    memmove(bb->sort, sortbuf, len);
}

/*
 * If the given entry is not viewed or selected, remove it from the
 * hash, free it, and return 1.
 */
int try_free_entry(entry_t *e)
{
    if (IS_SELECTED(e) || IS_VIEWED(e)) return 0;
    if (e->hash.next)
        e->hash.next->hash.atme = e->hash.atme;
    *(e->hash.atme) = e->hash.next;
    e->hash.atme = NULL;
    e->hash.next = NULL;
    free(e);
    return 1;
}

/*
 * Sort the files in bb according to bb's settings.
 */
void sort_files(bb_t *bb)
{
#ifdef __APPLE__
    qsort_r(bb->files, (size_t)bb->nfiles, sizeof(entry_t*), bb, compare_files);
#else
    qsort_r(bb->files, (size_t)bb->nfiles, sizeof(entry_t*), compare_files, bb);
#endif
    for (int i = 0; i < bb->nfiles; i++)
        bb->files[i]->index = i;
    bb->dirty = 1;
}

/*
 * Trim trailing whitespace by inserting '\0' and return a pointer to after the
 * first non-whitespace char
 */
static char *trim(char *s)
{
    if (!s) return NULL;
    while (*s == ' ' || *s == '\n') ++s;
    char *end;
    for (end = &s[strlen(s)-1]; end >= s && (*end == ' ' || *end == '\n'); end--)
        *end = '\0';
    return s;
}

/*
 * Hanlder for SIGWINCH events
 */
void update_term_size(int sig)
{
    (void)sig;
    struct winsize sz = {0};
    ioctl(fileno(tty_in), TIOCGWINSZ, &sz);
    termwidth = sz.ws_col;
    termheight = sz.ws_row;
}

int main(int argc, char *argv[])
{
    char *initial_path = NULL, depthstr[64] = {0};
    char sep = '\n';
    int print_dir = 0, print_selection = 0;

    for (int i = 1; i < argc; i++) {
        // Commands are processed below, after flags have been parsed
        if (argv[i][0] == '+') {
            char *colon = strchr(argv[i], ':');
            if (colon && !colon[1])
                break;
        } else if (strcmp(argv[i], "--") == 0) {
            if (i + 1 < argc) initial_path = argv[i+1];
            if (i + 2 < argc) {
                printf("Extra arguments after %s\n%s", argv[i+1], usage_str);
                return 1;
            }
            break;
        } else if (strcmp(argv[i], "--help") == 0) {
          help:
            printf("%s%s", description_str, usage_str);
            return 0;
        } else if (strcmp(argv[i], "--version") == 0) {
          version:
            printf("bb " BB_VERSION "\n");
            return 0;
        } else if (argv[i][0] == '-' && argv[i][1] == '-') {
            if (argv[i][2] == '\0') break;
            printf("Unknown command line argument: %s\n%s", argv[i], usage_str);
            return 1;
        } else if (argv[i][0] == '-') {
            for (char *c = &argv[i][1]; *c; c++) {
                switch (*c) {
                    case 'h': goto help;
                    case 'v': goto version;
                    case 'd': print_dir = 1; break;
                    case '0': sep = '\0'; break;
                    case 's': print_selection = 1; break;
                    default: printf("Unknown command line argument: -%c\n%s", *c, usage_str);
                             return 1;
                }
            }
        } else if (!initial_path) {
            initial_path = argv[i];
        } else {
            printf("Unknown command line argument: %s\n%s", argv[i], usage_str);
            return 1;
        }
    }
    if (!initial_path) initial_path = ".";

    cmdfilename = memcheck(strdup(CMDFILE_FORMAT));
    int cmdfd;
    if ((cmdfd = mkostemp(cmdfilename, O_APPEND)) == -1)
        err("Couldn't create tmpfile: '%s'", CMDFILE_FORMAT);
    // Set up environment variables
    char *curdepth = getenv("BB_DEPTH");
    int depth = curdepth ? atoi(curdepth) : 0;
    sprintf(depthstr, "%d", depth + 1);
    setenv("BB_DEPTH", depthstr, 1);
    setenv("BBCMD", cmdfilename, 1);

    if (!initial_path)
        return 0;

    // Default values
    setenv("SHELL", "bash", 0);
    setenv("EDITOR", "nano", 0);
    setenv("PAGER", "less", 0);

    atexit(cleanup);
    signal(SIGTERM, cleanup_and_exit);
    signal(SIGINT, cleanup_and_exit);
    signal(SIGXCPU, cleanup_and_exit);
    signal(SIGXFSZ, cleanup_and_exit);
    signal(SIGVTALRM, cleanup_and_exit);
    signal(SIGPROF, cleanup_and_exit);
    signal(SIGSEGV, cleanup_and_exit);

    char path[PATH_MAX], curdir[PATH_MAX];
    getcwd(curdir, PATH_MAX);
    strcat(curdir, "/");
    normalize_path(curdir, initial_path, path, 1);
    if (chdir(path)) err("Not a valid path: %s\n", path);
    setenv("BBINITIALPATH", path, 1);

    bb_t *bb = memcheck(calloc(1, sizeof(bb_t)));
    strcpy(bb->columns, "*smpn");
    strcpy(bb->sort, "+n");
    cd_to(bb, path);
    run_script(bb, runstartup);
    write(cmdfd, "", 1);
    for (int i = 0; i < argc; i++) {
        if (argv[i][0] == '+') {
            char *colon = strchr(argv[i], ':');
            char *cmd = argv[i];
            if (colon && !colon[1]) {
                for (++i; i < argc; i++) {
                    write(cmdfd, cmd, strlen(cmd));
                    write(cmdfd, argv[i], strlen(argv[i])+1); // Include null byte
                }
            } else {
                write(cmdfd, cmd, strlen(cmd)+1); // Include null byte
            }
        }
    }

    // Check whether anything was piped in to begin with,
    // and if so, treat it as commands
    struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
    if (poll(&pfd, 1, 0) == 1 && pfd.revents & POLLIN) {
        ssize_t len;
        char buf[1024];
        while ((len = read(STDIN_FILENO, buf, sizeof(buf))) > 0)
            write(cmdfd, buf, (size_t)len);
        write(cmdfd, "\0", 1);
    }

    if (cmdfd != -1) {
        close(cmdfd);
        cmdfd = -1;
    }

    bb_browse(bb, path);

    if (bb->firstselected && print_selection) {
        for (entry_t *e = bb->firstselected; e; e = e->selected.next) {
            const char *p = e->fullname;
            while (*p) {
                const char *p2 = strchr(p, '\n');
                if (!p2) p2 = p + strlen(p);
                write(STDOUT_FILENO, p, (size_t)(p2 - p));
                if (*p2 == '\n' && sep == '\n')
                    write(STDOUT_FILENO, "\\", 1);
                p = p2;
            }
            write(STDOUT_FILENO, &sep, 1);
        }
        fflush(stdout);
    }
    if (print_dir)
        printf("%s\n", bb->path);

    // Cleanup:
    bb->path[0] = '\0';
    populate_files(bb, 0);
    for (entry_t *next, *e = bb->firstselected; e; e = next) {
        next = e->selected.next;
        e->selected.atme = NULL;
        e->selected.next = NULL;
        try_free_entry(e);
    }
    bb->firstselected = NULL;
    free(bb);
    if (cmdfilename) free(cmdfilename);

    return 0;
}

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
