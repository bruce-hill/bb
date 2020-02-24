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
static struct winsize winsize = {0};
static char cmdfilename[PATH_MAX] = {0};
static proc_t *running_procs = NULL;
static int dirty = 1;

/*
 * Use bb to browse the filesystem.
 */
void bb_browse(bb_t *bb, const char *initial_path)
{
    if (populate_files(bb, initial_path))
        err("Could not find initial path: \"%s\"", initial_path);
    run_script(bb, "bbstartup");
    check_cmdfile(bb);
    while (!bb->should_quit) {
        render(bb);
        handle_next_key_binding(bb);
    }
    run_script(bb, "bbshutdown");
}

/*
 * Check the bb command file and run any and all commands that have been
 * written to it.
 */
void check_cmdfile(bb_t *bb)
{
    FILE *cmdfile = fopen(cmdfilename, "r");
    if (!cmdfile) return;
    char *cmd = NULL;
    size_t space = 0;
    while (getdelim(&cmd, &space, '\0', cmdfile) >= 0) {
        if (!cmd[0]) continue;
        run_bbcmd(bb, cmd);
        if (bb->should_quit) break;
    }
    free(cmd);
    fclose(cmdfile);
    unlink(cmdfilename);
}

/*
 * Clean up the terminal before going to the default signal handling behavior.
 */
void cleanup_and_raise(int sig)
{
    cleanup();
    int childsig = (sig == SIGTSTP || sig == SIGSTOP) ? sig : SIGHUP;
    for (proc_t *p = running_procs; p; p = p->running.next)
        kill(p->pid, childsig);
    raise(sig);
    // This code will only ever be run if sig is SIGTSTP/SIGSTOP, otherwise, raise() won't return:
    init_term();
    struct sigaction sa = {.sa_handler = &cleanup_and_raise, .sa_flags = (int)(SA_NODEFER | SA_RESETHAND)};
    sigaction(sig, &sa, NULL);
}

/*
 * Reset the screen and delete the cmdfile
 */
void cleanup(void)
{
    if (cmdfilename[0]) {
        unlink(cmdfilename);
        cmdfilename[0] = '\0';
    }
    if (tty_out) {
        fputs(T_LEAVE_BBMODE, tty_out);
        fflush(tty_out);
        tcsetattr(fileno(tty_out), TCSANOW, &orig_termios);
    }
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
            case COL_RANDOM: COMPARE(e2->shufflepos, e1->shufflepos); break;
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
 * Wait until the user has pressed a key with an associated key binding and run
 * that binding.
 */
void handle_next_key_binding(bb_t *bb)
{
    int key, mouse_x, mouse_y;
    binding_t *binding;
    do {
        do {
            key = bgetkey(tty_in, &mouse_x, &mouse_y);
            if (key == -1 && dirty) return;
        } while (key == -1);

        binding = NULL;
        for (size_t i = 0; bindings[i].script && i < sizeof(bindings)/sizeof(bindings[0]); i++) {
            if (key == bindings[i].key) {
                binding = &bindings[i];
                break;
            }
        }
    } while (!binding);

    char bbmousecol[2] = {0, 0}, bbclicked[PATH_MAX];
    if (mouse_x != -1 && mouse_y != -1) {
        // Get bb column:
        for (int col = 0, x = 0; bb->columns[col]; col++, x++) {
            if (!columns[(int)bb->columns[col]].name) continue;
            x += (int)strlen(columns[(int)bb->columns[col]].name);
            if (x >= mouse_x) {
                bbmousecol[0] = bb->columns[col];
                break;
            }
        }
        if (mouse_y == 1) {
            strcpy(bbclicked, "<column label>");
        } else if (2 <= mouse_y && mouse_y <= winsize.ws_row - 2
                   && bb->scroll + (mouse_y - 2) <= bb->nfiles - 1) {
            strcpy(bbclicked, bb->files[bb->scroll + (mouse_y - 2)]->fullname);
        } else {
            bbclicked[0] = '\0';
        }
        setenv("BBMOUSECOL", bbmousecol, 1);
        setenv("BBCLICKED", bbclicked, 1);
    }

    if (is_simple_bbcmd(binding->script)) {
        run_bbcmd(bb, binding->script);
    } else {
        move_cursor(tty_out, 0, winsize.ws_row-1);
        fputs("\033[K", tty_out);
        restore_term(&default_termios);
        run_script(bb, binding->script);
        init_term();
        set_title(bb);
        check_cmdfile(bb);
    }
    if (mouse_x != -1 && mouse_y != -1) {
        setenv("BBMOUSECOL", "", 1);
        setenv("BBCLICKED", "", 1);
    }
}

/*
 * Initialize the terminal files for /dev/tty and set up some desired
 * attributes like passing Ctrl-c as a key instead of interrupting
 */
void init_term(void)
{
    if (tcsetattr(fileno(tty_out), TCSANOW, &bb_termios) == -1)
        err("Couldn't tcsetattr");
    update_term_size(0);
    // Initiate mouse tracking and disable text wrapping:
    fputs(T_ENTER_BBMODE, tty_out);
    fflush(tty_out);
}

/* 
 * Return whether or not 's' is a simple bb command that doesn't need
 * a full shell instance (e.g. "bbcmd cd:.." or "bbcmd move:+1").
 */
static int is_simple_bbcmd(const char *s)
{
    if (!s) return 0;
    while (*s == ' ') ++s;
    if (strncmp(s, "bbcmd ", strlen("bbcmd ")) != 0)
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
entry_t* load_entry(bb_t *bb, const char *path)
{
    struct stat linkedstat, filestat;
    if (!path || !path[0]) return NULL;
    if (lstat(path, &filestat) == -1) return NULL;
    char pbuf[PATH_MAX];
    if (path[0] == '/')
        strcpy(pbuf, path);
    else
        sprintf(pbuf, "%s%s", bb->path, path);
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
        entry->name = &entry->fullname[strlen(bb->path)];
    }
    if (S_ISLNK(filestat.st_mode))
        entry->linkedmode = linkedstat.st_mode;
    entry->info = filestat;
    LL_PREPEND(bb->hash[(int)filestat.st_ino & HASH_MASK], entry, hash);
    entry->index = -1;
    bb->hash[(int)filestat.st_ino & HASH_MASK] = entry;
    return entry;
}

/*
 * Return whether a string matches a command
 * e.g. matches_cmd("sel:x", "select:") == 1, matches_cmd("q", "quit") == 1
 */
static inline int matches_cmd(const char *str, const char *cmd)
{
    if ((strchr(cmd, ':') == NULL) != (strchr(str, ':') == NULL))
        return 0;
    while (*str == *cmd && *cmd && *cmd != ':') ++str, ++cmd;
    return *str == '\0' || *str == ':';
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
 * The normalized path is stored in `normalized`.
 */
char *normalize_path(const char *root, const char *path, char *normalized)
{
    char pbuf[PATH_MAX] = {0};
    if (path[0] == '~' && (path[1] == '\0' || path[1] == '/')) {
        char *home;
        if (!(home = getenv("HOME"))) return NULL;
        strcpy(pbuf, home);
        ++path;
    } else if (path[0] != '/') {
        strcpy(pbuf, root);
        if (root[strlen(root)-1] != '/') strcat(pbuf, "/");
    }
    strcat(pbuf, path);
    if (realpath(pbuf, normalized) == NULL) {
        strcpy(normalized, pbuf); // TODO: normalize better?
        return NULL;
    }
    return normalized;
}

/*
 * Remove all the files currently stored in bb->files and if `bb->path` is
 * non-NULL, update `bb` with a listing of the files in `path`
 */
int populate_files(bb_t *bb, const char *path)
{
    int samedir = path && strcmp(bb->path, path) == 0;
    int old_scroll = bb->scroll;
    int old_cursor = bb->cursor;
    char old_selected[PATH_MAX] = "";
    if (samedir && bb->nfiles > 0) strcpy(old_selected, bb->files[bb->cursor]->fullname);

    char pbuf[PATH_MAX] = {0}, prev[PATH_MAX] = {0};
    strcpy(prev, bb->path);
    if (path == NULL) {
        pbuf[0] = '\0';
    } else if (strcmp(path, "<selection>") == 0) {
        strcpy(pbuf, path);
    } else if (strcmp(path, "..") == 0 && strcmp(bb->path, "<selection>") == 0) {
        if (!bb->prev_path[0]) return -1;
        strcpy(pbuf, bb->prev_path);
        if (chdir(pbuf)) {
            warn("Could not cd to: \"%s\"", pbuf);
            return -1;
        }
    } else {
        if (!normalize_path(bb->path, path, pbuf))
            warn("Could not normalize path: \"%s\"", path);
        if (pbuf[strlen(pbuf)-1] != '/')
            strcat(pbuf, "/");
        if (chdir(pbuf)) {
            warn("Could not cd to: \"%s\"", pbuf);
            return -1;
        }
    }

    if (strcmp(bb->path, "<selection>") != 0) {
        strcpy(bb->prev_path, prev);
        setenv("BBPREVPATH", bb->prev_path, 1);
    }

    dirty = 1;
    strcpy(bb->path, pbuf);
    set_title(bb);

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
    bb->nfiles = 0;
    bb->cursor = 0;
    bb->scroll = 0;

    if (!bb->path[0])
        return 0;

    size_t space = 0;
    if (strcmp(bb->path, "<selection>") == 0) {
        for (entry_t *e = bb->selected; e; e = e->selected.next) {
            if ((size_t)bb->nfiles + 1 > space)
                bb->files = memcheck(realloc(bb->files, (space += 100)*sizeof(void*)));
            e->index = bb->nfiles;
            bb->files[bb->nfiles++] = e;
        }
    } else {
        glob_t globbuf = {0};
        char *pat, *tmpglob = memcheck(strdup(bb->globpats));
        while ((pat = strsep(&tmpglob, " ")) != NULL)
            glob(pat, GLOB_NOSORT|GLOB_APPEND, NULL, &globbuf);
        free(tmpglob);
        for (size_t i = 0; i < globbuf.gl_pathc; i++) {
            if ((size_t)bb->nfiles + 1 > space)
                bb->files = memcheck(realloc(bb->files, (space += 100)*sizeof(void*)));
            // Don't normalize path so we can get "." and ".."
            entry_t *entry = load_entry(bb, globbuf.gl_pathv[i]);
            if (!entry) err("Failed to load entry: '%s'", globbuf.gl_pathv[i]);
            entry->index = bb->nfiles;
            bb->files[bb->nfiles++] = entry;
        }
        globfree(&globbuf);
    }

    for (int i = 0; i < bb->nfiles; i++) {
        int j = rand() / (RAND_MAX / (i + 1)); // This is not optimal, but doesn't need to be
        bb->files[i]->shufflepos = bb->files[j]->shufflepos;
        bb->files[j]->shufflepos = i;
    }

    sort_files(bb);
    if (samedir) {
        set_scroll(bb, old_scroll);
        bb->cursor = old_cursor > bb->nfiles-1 ? bb->nfiles-1 : old_cursor;
        if (old_selected[0]) {
            entry_t *e = load_entry(bb, old_selected);
            if (e) set_cursor(bb, e->index);
        }
    } else {
        entry_t *p = load_entry(bb, prev);
        if (p) {
            if (IS_VIEWED(p)) set_cursor(bb, p->index);
            else try_free_entry(p);
        }
    }
    return 0;
}

/*
 * Print the current key bindings
 */
void print_bindings(int fd)
{
    char buf[1000], buf2[1024];
    for (size_t i = 0; bindings[i].script && i < sizeof(bindings)/sizeof(bindings[0]); i++) {
        if (bindings[i].key == -1) {
            const char *label = bindings[i].description;
            sprintf(buf, "\n\033[33;1;4m\033[%dG%s\033[0m\n", (winsize.ws_col-(int)strlen(label))/2, label);
            write(fd, buf, strlen(buf));
            continue;
        }
        size_t shared = 0;
        char *p = buf;
        for (size_t j = i; bindings[j].script && strcmp(bindings[j].description, bindings[i].description) == 0; j++) {
            if (j > i) p = stpcpy(p, ", ");
            ++shared;
            int key = bindings[j].key;
            p = bkeyname(key, p);
        }
        *p = '\0';
        sprintf(buf2, "\033[1m\033[%dG%s\033[0m", winsize.ws_col/2 - 1 - (int)strlen(buf), buf);
        write(fd, buf2, strlen(buf2));
        sprintf(buf2, "\033[1m\033[%dG\033[34m%s\033[0m", winsize.ws_col/2 + 1,
                bindings[i].description);
        write(fd, buf2, strlen(buf2));
        write(fd, "\033[0m\n", strlen("\033[0m\n"));
        i += shared - 1;
    }
    write(fd, "\n", 1);
}

/*
 * Run a bb internal command (e.g. "+refresh") and return an indicator of what
 * needs to happen next.
 */
void run_bbcmd(bb_t *bb, const char *cmd)
{
    if (strncmp(cmd, "bbcmd ", strlen("bbcmd ")) == 0) cmd = &cmd[strlen("bbcmd ")];
    const char *value = strchr(cmd, ':');
    if (value) ++value;
#define set_bool(target) do { if (!value) { target = !target; } else { target = value[0] == '1'; } } while (0)
    if (matches_cmd(cmd, "bind:")) { // +bind:<keys>:<script>
        char *value_copy = memcheck(strdup(value));
        char *keys = trim(value_copy);
        if (!keys[0]) { free(value_copy); return; }
        char *script = strchr(keys+1, ':');
        if (!script) {
            free(value_copy);
            warn("No script provided.");
            return;
        }
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
            int keyval = bkeywithname(key);
            if (keyval == -1 && !is_section) continue;
            for (size_t i = 0; i < sizeof(bindings)/sizeof(bindings[0]); i++) {
                if (bindings[i].script && (bindings[i].key != keyval || is_section))
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
    } else if (matches_cmd(cmd, "cd:")) { // +cd:
        if (populate_files(bb, value))
            warn("Could not open directory: \"%s\"", value);
    } else if (matches_cmd(cmd, "columns:")) { // +columns:
        set_columns(bb, value);
    } else if (matches_cmd(cmd, "deselect")) { // +deselect
        while (bb->selected)
            set_selected(bb, bb->selected, 0);
    } else if (matches_cmd(cmd, "deselect:")) { // +deselect:<file>
        char pbuf[PATH_MAX];
        normalize_path(bb->path, value, pbuf);
        entry_t *e = load_entry(bb, pbuf);
        if (e) {
            set_selected(bb, e, 0);
            return;
        }
        // Filename may no longer exist:
        for (e = bb->selected; e; e = e->selected.next) {
            if (strcmp(e->fullname, pbuf) == 0) {
                set_selected(bb, e, 0);
                return;
            }
        }
    } else if (matches_cmd(cmd, "fg:") || matches_cmd(cmd, "fg")) { // +fg:
        int nprocs = 0;
        for (proc_t *p = running_procs; p; p = p->running.next) ++nprocs;
        int fg = value ? nprocs - (int)strtol(value, NULL, 10) : 0;
        proc_t *child = NULL;
        for (proc_t *p = running_procs; p && !child; p = p->running.next) {
            if (fg-- == 0) child = p;
        }
        if (!child) return;
        move_cursor(tty_out, 0, winsize.ws_row-1);
        fputs("\033[K", tty_out);
        restore_term(&default_termios);
        signal(SIGTTOU, SIG_IGN);
        if (tcsetpgrp(fileno(tty_out), child->pid))
            err("Couldn't set pgrp");
        kill(-(child->pid), SIGCONT);
        wait_for_process(&child);
        signal(SIGTTOU, SIG_DFL);
        init_term();
        set_title(bb);
        dirty = 1;
    } else if (matches_cmd(cmd, "glob:")) { // +glob:
        set_globs(bb, value[0] ? value : "*");
        populate_files(bb, bb->path);
    } else if (matches_cmd(cmd, "goto:")) { // +goto:
        entry_t *e = load_entry(bb, value);
        if (!e) {
            warn("Could not find file to go to: \"%s\"", value);
            return;
        }
        if (IS_VIEWED(e)) {
            set_cursor(bb, e->index);
            return;
        }
        char pbuf[PATH_MAX];
        strcpy(pbuf, e->fullname);
        char *lastslash = strrchr(pbuf, '/');
        if (!lastslash) err("No slash found in filename: %s", pbuf);
        *lastslash = '\0'; // Split in two
        populate_files(bb, pbuf);
        if (IS_VIEWED(e))
            set_cursor(bb, e->index);
        else try_free_entry(e);
    } else if (matches_cmd(cmd, "help")) { // +help
        char filename[PATH_MAX];
        sprintf(filename, "%s/bbhelp.XXXXXX", getenv("TMPDIR"));
        int fd = mkstemp(filename);
        if (fd == -1) err("Couldn't create temporary help file at %s", filename);
        print_bindings(fd);
        close(fd);
        char script[512] = "less -rKX < ";
        strcat(script, filename);
        run_script(bb, script);
        if (unlink(filename) == -1) err("Couldn't delete temporary help file: '%s'", filename);
    } else if (matches_cmd(cmd, "interleave:") || matches_cmd(cmd, "interleave")) { // +interleave
        set_bool(bb->interleave_dirs);
        set_interleave(bb, bb->interleave_dirs);
        sort_files(bb);
    } else if (matches_cmd(cmd, "move:")) { // +move:
        int oldcur, isdelta, n;
      move:
        if (bb->nfiles == 0) return;
        oldcur = bb->cursor;
        isdelta = value[0] == '-' || value[0] == '+';
        n = (int)strtol(value, (char**)&value, 10);
        if (*value == '%')
            n = (n * (value[1] == 'n' ? bb->nfiles : winsize.ws_row)) / 100;
        if (isdelta) set_cursor(bb, bb->cursor + n);
        else set_cursor(bb, n);
        if (matches_cmd(cmd, "spread:")) { // +spread:
            int sel = IS_SELECTED(bb->files[oldcur]);
            for (int i = bb->cursor; i != oldcur; i += (oldcur > i ? 1 : -1))
                set_selected(bb, bb->files[i], sel);
        }
    } else if (matches_cmd(cmd, "quit")) { // +quit
        bb->should_quit = 1;
    } else if (matches_cmd(cmd, "refresh")) { // +refresh
        populate_files(bb, bb->path);
    } else if (matches_cmd(cmd, "scroll:")) { // +scroll:
        // TODO: figure out the best version of this
        int isdelta = value[0] == '+' || value[0] == '-';
        int n = (int)strtol(value, (char**)&value, 10);
        if (*value == '%')
            n = (n * (value[1] == 'n' ? bb->nfiles : winsize.ws_row)) / 100;
        if (isdelta)
            set_scroll(bb, bb->scroll + n);
        else
            set_scroll(bb, n);
    } else if (matches_cmd(cmd, "select")) { // +select
        for (int i = 0; i < bb->nfiles; i++)
            set_selected(bb, bb->files[i], 1);
    } else if (matches_cmd(cmd, "select:")) { // +select:<file>
        entry_t *e = load_entry(bb, value);
        if (e) set_selected(bb, e, 1);
        else warn("Could not find file to select: \"%s\"", value);
    } else if (matches_cmd(cmd, "sort:")) { // +sort:
        set_sort(bb, value);
        sort_files(bb);
    } else if (matches_cmd(cmd, "spread:")) { // +spread:
        goto move;
    } else if (matches_cmd(cmd, "toggle")) { // +toggle
        for (int i = 0; i < bb->nfiles; i++)
            set_selected(bb, bb->files[i], !IS_SELECTED(bb->files[i]));
    } else if (matches_cmd(cmd, "toggle:")) { // +toggle:<file>
        entry_t *e = load_entry(bb, value);
        if (e) set_selected(bb, e, !IS_SELECTED(e));
        else warn("Could not find file to toggle: \"%s\"", value);
    } else {
        warn("Invalid bb command: %s", cmd);
    }
}

/*
 * Draw everything to the screen.
 * If `dirty` is false, then use terminal scrolling to move the file listing
 * around and only update the files that have changed.
 */
void render(bb_t *bb)
{
    static int lastcursor = -1, lastscroll = -1;

    if (!dirty) {
        // Use terminal scrolling:
        if (lastscroll > bb->scroll) {
            fprintf(tty_out, "\033[3;%dr\033[%dT\033[1;%dr", winsize.ws_row-1, lastscroll - bb->scroll, winsize.ws_row);
        } else if (lastscroll < bb->scroll) {
            fprintf(tty_out, "\033[3;%dr\033[%dS\033[1;%dr", winsize.ws_row-1, bb->scroll - lastscroll, winsize.ws_row);
        }
    }

    int colwidths[MAX_COLS] = {0};
    int space = winsize.ws_col - 1, nstretchy = 0;
    for (int c = 0; bb->columns[c]; c++) {
        column_t col = columns[(int)bb->columns[c]];
        if (!col.name) continue;
        if (col.stretchy) {
            ++nstretchy;
        } else {
            colwidths[c] = strlen(col.name) + 1;
            space -= colwidths[c];
        }
        if (c > 0) --space;
    }
    for (int c = 0; bb->columns[c]; c++)
        if (columns[(int)bb->columns[c]].stretchy)
            colwidths[c] = space / nstretchy;

    if (dirty) {
        // Path
        move_cursor(tty_out, 0, 0);
        const char *color = TITLE_COLOR;
        fputs(color, tty_out);

        char *home = getenv("HOME");
        if (home && strncmp(bb->path, home, strlen(home)) == 0) {
            fputs("~", tty_out);
            fputs_escaped(tty_out, bb->path + strlen(home), color);
        } else {
            fputs_escaped(tty_out, bb->path, color);
        }
        fprintf(tty_out, "\033[0;2m[%s]", bb->globpats);
        fputs(" \033[K\033[0m", tty_out);

        static const char *help = "Press '?' to see key bindings ";
        move_cursor(tty_out, MAX(0, winsize.ws_col - (int)strlen(help)), 0);
        fputs(help, tty_out);
        fputs("\033[K\033[0m", tty_out);

        // Columns
        move_cursor(tty_out, 0, 1);
        fputs("\033[0;44;30m\033[K", tty_out);
        int x = 0;
        for (int c = 0; bb->columns[c]; c++) {
            column_t col = columns[(int)bb->columns[c]];
            if (!col.name) continue;
            const char *title = col.name;
            if (!title) title = "";
            move_cursor(tty_out, x, 1);
            if (c > 0) {
                fputs("┃\033[K", tty_out);
                x += 1;
            }
            const char *indicator = " ";
            if (bb->columns[c] == bb->sort[1])
                indicator = bb->sort[0] == '-' ? RSORT_INDICATOR : SORT_INDICATOR;
            move_cursor(tty_out, x, 1);
            fputs(indicator, tty_out);
            fputs(title, tty_out);
            x += colwidths[c];
        }
        fputs(" \033[K\033[0m", tty_out);
    }

    entry_t **files = bb->files;
    for (int i = bb->scroll; i < bb->scroll + ONSCREEN; i++) {
        if (!dirty) {
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
        const char *color = i == bb->cursor ?
            CURSOR_COLOR : color_of(bb->files[i]->info.st_mode);

        int x = 0;
        for (int c = 0; bb->columns[c]; c++) {
            column_t col = columns[(int)bb->columns[c]];
            if (!col.name) continue;
            move_cursor(tty_out, x, y);
            fputs(color, tty_out);
            if (c > 0) {
                if (i == bb->cursor) fprintf(tty_out, "\033[2m┃\033[22m");
                else fprintf(tty_out, "\033[37;2m┃\033[22m%s", color);
                x += 1;
            }
            char buf[PATH_MAX * 2] = {0};
            col.render(entry, color, buf, colwidths[c]);
            fputs(buf, tty_out);
            fprintf(tty_out, "\033[0m%s\033[K", color);
            x += colwidths[c];
        }
        fputs("\033[0m", tty_out);
    }

    // Scrollbar:
    if (bb->nfiles > ONSCREEN) {
        int height = (ONSCREEN*ONSCREEN + (bb->nfiles-1))/bb->nfiles;
        int start = 2 + (bb->scroll*ONSCREEN)/bb->nfiles;
        for (int i = 2; i < 2 + ONSCREEN; i++) {
            move_cursor(tty_out, winsize.ws_col-1, i);
            fprintf(tty_out, "%s\033[0m",
                (i >= start && i < start + height) ?  SCROLLBAR_FG : SCROLLBAR_BG);
        }
    }

    move_cursor(tty_out, winsize.ws_col/2, winsize.ws_row - 1);
    fputs("\033[0m\033[K", tty_out);
    int x = winsize.ws_col;
    if (bb->selected) { // Number of selected files
        int n = 0;
        for (entry_t *s = bb->selected; s; s = s->selected.next) ++n;
        x -= 14;
        for (int k = n; k; k /= 10) x--;
        move_cursor(tty_out, MAX(0, x), winsize.ws_row - 1);
        fprintf(tty_out, "\033[41;30m %d Selected \033[0m", n);
    }
    int nprocs = 0;
    for (proc_t *p = running_procs; p; p = p->running.next) ++nprocs;
    if (nprocs > 0) { // Number of suspended processes
        x -= 13;
        for (int k = nprocs; k; k /= 10) x--;
        move_cursor(tty_out, MAX(0, x), winsize.ws_row - 1);
        fprintf(tty_out, "\033[44;30m %d Suspended \033[0m", nprocs);
    }
    move_cursor(tty_out, winsize.ws_col/2, winsize.ws_row - 1);

    lastcursor = bb->cursor;
    lastscroll = bb->scroll;
    fflush(tty_out);
    dirty = 0;
}

/*
 * Close the /dev/tty terminals and restore some of the attributes.
 */
void restore_term(const struct termios *term)
{
    tcsetattr(fileno(tty_out), TCSANOW, term);
    fputs(T_LEAVE_BBMODE_PARTIAL, tty_out);
    fflush(tty_out);
}

/*
 * Run a shell script with the selected files passed as sequential arguments to
 * the script (or pass the cursor file if none are selected).
 * Return the exit status of the script.
 */
int run_script(bb_t *bb, const char *cmd)
{
    proc_t *proc = memcheck(calloc(1, sizeof(proc_t)));
    signal(SIGTTOU, SIG_IGN);
    if ((proc->pid = fork()) == 0) {
        pid_t pgrp = getpid();
        (void)setpgid(0, pgrp);
        if (tcsetpgrp(STDIN_FILENO, pgrp))
            err("Couldn't set pgrp");
        char **args = memcheck(calloc(4 + (size_t)bb->nselected + 1, sizeof(char*)));
        int i = 0;
        args[i++] = "sh";
        args[i++] = "-c";
        args[i++] = (char*)cmd;
        args[i++] = "--"; // ensure files like "-i" are not interpreted as flags for sh
        // bb->selected is in most-recent order, so populate args in reverse to make sure
        // that $1 is the first selected, etc.
        i += bb->nselected;
        for (entry_t *e = bb->selected; e; e = e->selected.next)
            args[--i] = e->fullname;

        setenv("BBCURSOR", bb->nfiles ? bb->files[bb->cursor]->fullname : "", 1);

        dup2(fileno(tty_out), STDOUT_FILENO);
        dup2(fileno(tty_in), STDIN_FILENO);
        execvp(args[0], args);
        err("Failed to execute command: '%s'", cmd);
        return -1;
    }

    if (proc->pid == -1)
        err("Failed to fork");

    (void)setpgid(proc->pid, proc->pid);
    LL_PREPEND(running_procs, proc, running);
    int status = wait_for_process(&proc);
    dirty = 1;
    return status;
}

/*
 * Set the columns displayed by bb.
 */
void set_columns(bb_t *bb, const char *cols)
{
    strncpy(bb->columns, cols, MAX_COLS);
    setenv("BBCOLUMNS", bb->columns, 1);
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
 * Set the glob pattern(s) used by bb. Patterns are ' ' delimited
 */
void set_globs(bb_t *bb, const char *globs)
{
    free(bb->globpats);
    bb->globpats = memcheck(strdup(globs));
    setenv("BBGLOB", bb->globpats, 1);
}


/*
 * Set whether or not bb should interleave directories and files.
 */
void set_interleave(bb_t *bb, int interleave)
{
    bb->interleave_dirs = interleave;
    if (interleave) setenv("BBINTERLEAVE", "interleave", 1);
    else unsetenv("BBINTERLEAVE");
    dirty = 1;
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
        dirty = 1;

    if (selected) {
        LL_PREPEND(bb->selected, e, selected);
        ++bb->nselected;
    } else {
        LL_REMOVE(e, selected);
        try_free_entry(e);
        --bb->nselected;
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
    setenv("BBSORT", bb->sort, 1);
}

/*
 * Set the xwindow title property to: bb - current path
 */
void set_title(bb_t *bb)
{
    char *home = getenv("HOME");
    if (home && strncmp(bb->path, home, strlen(home)) == 0)
        fprintf(tty_out, "\033]2;bb: ~%s\007", bb->path + strlen(home));
    else
        fprintf(tty_out, "\033]2;bb: %s\007", bb->path);
}

/*
 * If the given entry is not viewed or selected, remove it from the
 * hash, free it, and return 1.
 */
int try_free_entry(entry_t *e)
{
    if (IS_SELECTED(e) || IS_VIEWED(e) || !IS_LOADED(e)) return 0;
    LL_REMOVE(e, hash);
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
    dirty = 1;
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
    struct winsize oldsize = winsize;
    ioctl(STDIN_FILENO, TIOCGWINSZ, &winsize);
    dirty |= (oldsize.ws_col != winsize.ws_col || oldsize.ws_row != winsize.ws_row);
}

/*
 * Wait for a process to either suspend or exit and return the status.
 */
int wait_for_process(proc_t **proc)
{
    tcsetpgrp(fileno(tty_out), (*proc)->pid);
    int status;
    while (*proc) {
        if (waitpid((*proc)->pid, &status, WUNTRACED) < 0)
            continue;
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            LL_REMOVE((*proc), running);
            free(*proc);
            *proc = NULL;
        } else if (WIFSTOPPED(status))
            break;
    }
    if (tcsetpgrp(fileno(tty_out), getpid()))
        err("Failed to set pgrp");
    signal(SIGTTOU, SIG_DFL);
    return status;
}

int main(int argc, char *argv[])
{
    char *initial_path;
    if (argc >= 3 && strcmp(argv[argc-2], "--") == 0) {
        initial_path = argv[argc-1];
        argc -= 2;
    } else if (argc >= 2 && argv[argc-1][0] != '-' && argv[argc-1][0] != '+') {
        initial_path = argv[argc-1];
        argc -= 1;
    } else {
        initial_path = ".";
    }

    char sep = '\n';
    int print_dir = 0, print_selection = 0;
    for (int i = 1; i < argc; i++) {
        // Commands are processed below, after flags have been parsed
        if (argv[i][0] == '+') {
            char *colon = strchr(argv[i], ':');
            if (colon && !colon[1])
                break;
        } else if (strcmp(argv[i], "--help") == 0) {
          help:
            printf("%s%s", description_str, usage_str);
            return 0;
        } else if (strcmp(argv[i], "--version") == 0) {
          version:
            printf("bb " BB_VERSION "\n");
            return 0;
        } else if (argv[i][0] == '-' && argv[i][1] != '-') {
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
        } else {
            printf("Unknown command line argument: \"%s\"\n%s", argv[i], usage_str);
            return 1;
        }
    }

    struct sigaction sa_winch = {.sa_handler = &update_term_size};
    sigaction(SIGWINCH, &sa_winch, NULL);
    update_term_size(0);
    // Wait 10ms at a time for terminal to initialize if necessary
    while (winsize.ws_row == 0)
        usleep(10000);

    // Set up environment variables
    // Default values
    setenv("TMPDIR", "/tmp", 0);
    sprintf(cmdfilename, "%s/bb.XXXXXX", getenv("TMPDIR"));
    int cmdfd;
    if ((cmdfd = mkostemp(cmdfilename, O_APPEND)) == -1)
        err("Couldn't create bb command file: '%s'", cmdfilename);
    setenv("BBCMD", cmdfilename, 1);
    char xdg_config_home[PATH_MAX], xdg_data_home[PATH_MAX];
    sprintf(xdg_config_home, "%s/.config", getenv("HOME"));
    setenv("XDG_CONFIG_HOME", xdg_config_home, 0);
    sprintf(xdg_data_home, "%s/.local/share", getenv("HOME"));
    setenv("XDG_DATA_HOME", xdg_data_home, 0);
    setenv("sysconfdir", "/etc", 0);

    char *newpath;
    static char bbpath[PATH_MAX];
    // Hacky fix to allow `bb` to be run out of its build directory:
    if (strncmp(argv[0], "./", 2) == 0) {
        if (realpath(argv[0], bbpath) == NULL)
            err("Could not resolve path: %s", bbpath);
        char *slash = strrchr(bbpath, '/');
        if (!slash) err("No slash found in real path: %s", bbpath);
        *slash = '\0';
        setenv("BBPATH", bbpath, 1);
    }
    if (getenv("BBPATH")) {
        if (asprintf(&newpath, "%s/bb:%s/scripts:%s",
                     getenv("XDG_CONFIG_HOME"), getenv("BBPATH"), getenv("PATH")) < 0)
            err("Could not allocate memory for PATH");
    } else {
        if (asprintf(&newpath, "%s/bb:%s/xdg/bb:%s",
                     getenv("XDG_CONFIG_HOME"), getenv("sysconfdir"), getenv("PATH")) < 0)
            err("Could not allocate memory for PATH");
    }
    setenv("PATH", newpath, 1);

    setenv("SHELL", "bash", 0);
    setenv("EDITOR", "nano", 0);
    char *curdepth = getenv("BBDEPTH");
    int depth = curdepth ? atoi(curdepth) : 0;
    char depthstr[16];
    sprintf(depthstr, "%d", depth + 1);
    setenv("BBDEPTH", depthstr, 1);
    char full_initial_path[PATH_MAX];
    getcwd(full_initial_path, PATH_MAX);
    normalize_path(full_initial_path, initial_path, full_initial_path);
    struct stat path_stat;
    if (stat(full_initial_path, &path_stat) != 0)
        err("Could not find initial path: \"%s\"", initial_path);
    if (S_ISDIR(path_stat.st_mode)) {
        if (strcmp(full_initial_path, "/") != 0) strcat(full_initial_path, "/");
    } else {
        write(cmdfd, "goto:", 5);
        write(cmdfd, full_initial_path, strlen(full_initial_path) + 1); // Include null byte
        char *slash = strrchr(full_initial_path, '/');
        *slash = '\0';
    }
    setenv("BBINITIALPATH", full_initial_path, 1);

    write(cmdfd, "\0", 1);
    for (int i = 0; i < argc; i++) {
        if (argv[i][0] == '+') {
            char *cmd = argv[i] + 1;
            char *colon = strchr(cmd, ':');
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
    close(cmdfd); cmdfd = -1;

    tty_in = fopen("/dev/tty", "r");
    tty_out = fopen("/dev/tty", "w");
    tcgetattr(fileno(tty_out), &orig_termios);
    memcpy(&bb_termios, &orig_termios, sizeof(bb_termios));
    cfmakeraw(&bb_termios);
    bb_termios.c_cc[VMIN] = 0;
    bb_termios.c_cc[VTIME] = 1;
    atexit(cleanup);
    int signals[] = {SIGTERM, SIGINT, SIGXCPU, SIGXFSZ, SIGVTALRM, SIGPROF, SIGSEGV, SIGTSTP};
    struct sigaction sa = {.sa_handler = &cleanup_and_raise, .sa_flags = (int)(SA_NODEFER | SA_RESETHAND)};
    for (size_t i = 0; i < sizeof(signals)/sizeof(signals[0]); i++)
        sigaction(signals[i], &sa, NULL);

    bb_t bb = {
        .columns = "*smpn",
        .sort = "+n",
    };
    set_globs(&bb, "*");
    init_term();
    bb_browse(&bb, full_initial_path);
    fputs(T_LEAVE_BBMODE, tty_out);
    cleanup();

    if (bb.selected && print_selection) {
        for (entry_t *e = bb.selected; e; e = e->selected.next) {
            write(STDOUT_FILENO, e->fullname, strlen(e->fullname));
            write(STDOUT_FILENO, &sep, 1);
        }
        fflush(stdout);
    }

    if (print_dir)
        printf("%s\n", bb.path);

    // Cleanup:
    populate_files(&bb, NULL);
    while (bb.selected)
        set_selected(&bb, bb.selected, 0);
    free(bb.globpats);
    return 0;
}

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
