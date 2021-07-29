//
// bb.c
// Copyright 2020 Bruce Hill
// Released under the MIT license with the Commons Clause
//
// This file contains the main source code of `bb` the Bitty Browser.
//

#include <ctype.h>
#include <err.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/errno.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "draw.h"
#include "terminal.h"
#include "types.h"
#include "utils.h"

#ifndef BB_NAME
#define BB_NAME "bb"
#endif

#define BB_VERSION "0.31.0"
#define MAX_BINDINGS 1024
#define SCROLLOFF MIN(5, (winsize.ws_row-4)/2)
#define ONSCREEN (winsize.ws_row - 3)

// Functions
void bb_browse(bb_t *bb, int argc, char *argv[]);
static void check_cmdfile(bb_t *bb);
static void cleanup(void);
static void cleanup_and_raise(int sig);
static int compare_files(const void *v1, const void *v2);
__attribute__((format(printf,2,3)))
void flash_warn(bb_t *bb, const char *fmt, ...);
static void handle_next_key_binding(bb_t *bb);
static void init_term(void);
static int is_simple_bbcmd(const char *s);
static entry_t* load_entry(bb_t *bb, const char *path);
static int matches_cmd(const char *str, const char *cmd);
static char* normalize_path(const char *path, char *pbuf);
static int populate_files(bb_t *bb, const char *path);
static void print_bindings(int fd);
static void run_bbcmd(bb_t *bb, const char *cmd);
static void restore_term(const struct termios *term);
static int run_script(bb_t *bb, const char *cmd);
static void set_columns(bb_t *bb, const char *cols);
static void set_cursor(bb_t *bb, int i);
static void set_globs(bb_t *bb, const char *globs);
static void set_interleave(bb_t *bb, int interleave);
static void set_selected(bb_t *bb, entry_t *e, int selected);
static void set_scroll(bb_t *bb, int i);
static void set_sort(bb_t *bb, const char *sort);
static void set_title(bb_t *bb);
static void sort_files(bb_t *bb);
static char *trim(char *s);
static int try_free_entry(entry_t *e);
static void update_term_size(int sig);
static int wait_for_process(proc_t **proc);

// Constants
static const char *T_ENTER_BBMODE = T_OFF(T_SHOW_CURSOR ";" T_WRAP) T_ON(T_ALT_SCREEN ";" T_MOUSE_XY ";" T_MOUSE_CELL ";" T_MOUSE_SGR);
static const char *T_LEAVE_BBMODE = T_OFF(T_MOUSE_XY ";" T_MOUSE_CELL ";" T_MOUSE_SGR ";" T_ALT_SCREEN) T_ON(T_SHOW_CURSOR ";" T_WRAP);
static const char *T_LEAVE_BBMODE_PARTIAL = T_OFF(T_MOUSE_XY ";" T_MOUSE_CELL ";" T_MOUSE_SGR) T_ON(T_WRAP);
static const char *description_str = BB_NAME" - an itty bitty console TUI file browser\n";
static const char *usage_str = "Usage: "BB_NAME" (-h/--help | -v/--version | -s | -d | -0 | +command)* [[--] directory]\n";


// Variables used within this file to track global state
static binding_t bindings[MAX_BINDINGS];
static struct termios orig_termios, bb_termios;
static FILE *tty_out = NULL, *tty_in = NULL;
static struct winsize winsize = {0};
static char cmdfilename[PATH_MAX] = {0};
static bb_t *current_bb = NULL;

// Redirect stderr/stdout to these files during execution, and dump them on exit
typedef struct {
    int orig_fd, dup_fd, tmp_fd;
    const char *name;
    char filename[PATH_MAX];
} outbuf_t;
outbuf_t output_buffers[] = {
    {.name="stdout", .orig_fd=STDOUT_FILENO, .dup_fd=-1, .tmp_fd=-1},
    {.name="stderr", .orig_fd=STDERR_FILENO, .dup_fd=-1, .tmp_fd=-1},
};

//
// Use bb to browse the filesystem.
//
void bb_browse(bb_t *bb, int argc, char *argv[])
{
    const char *initial_path;
    if (argc >= 3 && streq(argv[argc-2], "--")) {
        initial_path = argv[argc-1];
        argc -= 2;
    } else if (argc >= 2 && argv[argc-1][0] != '-' && argv[argc-1][0] != '+') {
        initial_path = argv[argc-1];
        argc -= 1;
    } else {
        initial_path = ".";
    }

    char full_initial_path[PATH_MAX];
    normalize_path(initial_path, full_initial_path);

    struct stat path_stat;
    const char *goto_file = NULL;
    nonnegative(stat(full_initial_path, &path_stat), "Could not find initial path: \"%s\"", initial_path);
    if (!S_ISDIR(path_stat.st_mode)) {
        char *slash = strrchr(full_initial_path, '/');
        *slash = '\0';
        goto_file = slash+1;
    }

    if (populate_files(bb, full_initial_path))
        errx(EXIT_FAILURE, "Could not find initial path: \"%s\"", full_initial_path);

    // Emergency fallback:
    bindings[0].key = KEY_CTRL_C;
    bindings[0].script = check_strdup("kill -INT $PPID");
    bindings[0].description = check_strdup("Kill the bb process");
    run_script(bb, "bbstartup");

    FILE *cmdfile = fopen(cmdfilename, "a");
    if (goto_file)
        fprintf(cmdfile, "%cgoto:%s", '\0', goto_file);
    for (int i = 0; i < argc; i++) {
        if (argv[i][0] == '+') {
            char *cmd = argv[i] + 1;
            char *colon = strchr(cmd, ':');
            if (colon && !colon[1]) {
                for (++i; i < argc; i++)
                    fprintf(cmdfile, "%c%s%s", '\0', cmd, argv[i]);
            } else {
                fprintf(cmdfile, "%c%s", '\0', cmd);
            }
        }
    }
    fclose(cmdfile);

    check_cmdfile(bb);
    while (!bb->should_quit) {
        render(tty_out, bb);
        handle_next_key_binding(bb);
    }
    run_script(bb, "bbshutdown");
    check_cmdfile(bb);
}

//
// Check the bb command file and run any and all commands that have been
// written to it.
//
static void check_cmdfile(bb_t *bb)
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
    delete(&cmd);
    fclose(cmdfile);
    unlink(cmdfilename);
}

//
// Clean up the terminal before going to the default signal handling behavior.
//
static void cleanup_and_raise(int sig)
{
    cleanup();
    int childsig = (sig == SIGTSTP || sig == SIGSTOP) ? sig : SIGHUP;
    if (current_bb) {
        for (proc_t *p = current_bb->running_procs; p; p = p->running.next) {
            kill(p->pid, childsig);
            LL_REMOVE(p, running);
        }
    }
    raise(sig);
    // This code will only ever be run if sig is SIGTSTP/SIGSTOP, otherwise, raise() won't return:
    init_term();
    struct sigaction sa = {.sa_handler = &cleanup_and_raise, .sa_flags = (int)(SA_NODEFER | SA_RESETHAND)};
    sigaction(sig, &sa, NULL);
}

//
// Reset the screen, delete the cmdfile, and print the stdout/stderr buffers
//
static void cleanup(void)
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
    FOREACH(outbuf_t*, ob, output_buffers) {
        if (ob->tmp_fd == -1) continue;
        fflush(ob->orig_fd == STDOUT_FILENO ? stdout : stderr);
        dup2(ob->dup_fd, ob->orig_fd);
        lseek(ob->tmp_fd, 0, SEEK_SET);
        char buf[256];
        for (ssize_t len; (len = read(ob->tmp_fd, buf, LEN(buf))) > 0; )
            write(ob->orig_fd, buf, len);
        close(ob->tmp_fd);
        ob->tmp_fd = ob->dup_fd = -1;
        unlink(ob->filename);
    }
}

//
// Used for sorting, this function compares files according to the sorting-related options,
// like bb->sort
//
static int compare_files(const void *v1, const void *v2)
{
#define COMPARE(a, b) if ((a) != (b)) { return sign*((a) < (b) ? 1 : -1); }
#define COMPARE_TIME(t1, t2) COMPARE((t1).tv_sec, (t2).tv_sec) COMPARE((t1).tv_nsec, (t2).tv_nsec)
    bb_t *bb = current_bb;
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
                // This sorting method is not identical to strverscmp(). Notably, bb's sort
                // will order: [0, 1, 9, 00, 01, 09, 10, 000, 010] instead of strverscmp()'s
                // order: [000, 00, 01, 010, 09, 0, 1, 9, 10]. I believe bb's sort is consistent
                // with how people want their files grouped: all files padded to n digits
                // will be grouped together, and files with the same padding will be sorted
                // ordinally. This version also does case-insensitivity by lowercasing words,
                // so the following characters come before all letters: [\]^_`
                const char *n1 = e1->name, *n2 = e2->name;
                while (*n1 && *n2) {
                    char c1 = tolower(*n1), c2 = tolower(*n2);
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
                COMPARE(tolower(*n2), tolower(*n1));
                break;
            }
            case COL_PERM: COMPARE((e1->info.st_mode & 0x3FF), (e2->info.st_mode & 0x3FF)); break;
            case COL_SIZE: COMPARE(e1->info.st_size, e2->info.st_size); break;
            case COL_MTIME: COMPARE_TIME(mtime(e1->info), mtime(e2->info)); break;
            case COL_CTIME: COMPARE_TIME(ctime(e1->info), ctime(e2->info)); break;
            case COL_ATIME: COMPARE_TIME(atime(e1->info), atime(e2->info)); break;
            case COL_RANDOM: COMPARE(e2->shufflepos, e1->shufflepos); break;
            default: break;
        }
    }
    return 0;
#undef COMPARE
#undef COMPARE_TIME
}

//
// Flash a warning message at the bottom of the screen.
//
void flash_warn(bb_t *bb, const char *fmt, ...) {
    move_cursor(tty_out, 0, winsize.ws_row-1);
    fputs("\033[41;33;1m", tty_out);
    va_list args;
    va_start(args, fmt);
    vfprintf(tty_out, fmt, args);
    va_end(args);
    fputs(" Press any key to continue...\033[0m  ", tty_out);
    fflush(tty_out);
    while (bgetkey(tty_in, NULL, NULL) == -1) usleep(100);
    bb->dirty = 1;
}

//
// Wait until the user has pressed a key with an associated key binding and run
// that binding.
//
static void handle_next_key_binding(bb_t *bb)
{
    int key, mouse_x, mouse_y;
    binding_t *binding;
    do {
        do {
            key = bgetkey(tty_in, &mouse_x, &mouse_y);
            if (key == -1 && bb->dirty) return;
        } while (key == -1);

        binding = NULL;
        FOREACH(binding_t*, b, bindings) {
            if (key == b->key) {
                binding = b;
                break;
            }
        }
    } while (!binding);

    char bbmousecol[2] = {0, 0}, bbclicked[PATH_MAX];
    if (mouse_x != -1 && mouse_y != -1) {
        int *colwidths = get_column_widths(bb->columns, winsize.ws_col-1);
        // Get bb column:
        for (int col = 0, x = 0; bb->columns[col]; col++, x++) {
            x += colwidths[col];
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
        restore_term(&orig_termios);
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

//
// Initialize the terminal files for /dev/tty and set up some desired
// attributes like passing Ctrl-c as a key instead of interrupting
//
static void init_term(void)
{
    nonnegative(tcsetattr(fileno(tty_out), TCSANOW, &bb_termios));
    update_term_size(0);
    // Initiate mouse tracking and disable text wrapping:
    fputs(T_ENTER_BBMODE, tty_out);
    fflush(tty_out);
}

//
// Return whether or not 's' is a simple bb command that doesn't need
// a full shell instance (e.g. "bbcmd cd:.." or "bbcmd move:+1").
//
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

//
// Load a file's info into an entry_t and return it (if found).
// The returned entry must be free()ed by the caller.
// Warning: this does not deduplicate entries, and it's best if there aren't
// duplicate entries hanging around.
//
static entry_t* load_entry(bb_t *bb, const char *path)
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
            && streq(pbuf, e->fullname))
            return e;
    }

    ssize_t linkpathlen = -1;
    char linkbuf[PATH_MAX];
    if (S_ISLNK(filestat.st_mode)) {
        linkpathlen = nonnegative(readlink(pbuf, linkbuf, sizeof(linkbuf)), "Couldn't read link: '%s'", pbuf);
        linkbuf[linkpathlen] = '\0';
        while (linkpathlen > 0 && linkbuf[linkpathlen-1] == '/') linkbuf[--linkpathlen] = '\0';
        if (stat(pbuf, &linkedstat) == -1) memset(&linkedstat, 0, sizeof(linkedstat));
    }
    size_t pathlen = strlen(pbuf);
    size_t entry_size = sizeof(entry_t) + (pathlen + 1) + (size_t)(linkpathlen + 1);
    entry_t *entry = new_bytes(entry_size);
    char *end = stpcpy(entry->fullname, pbuf);
    if (linkpathlen >= 0)
        entry->linkname = strcpy(end + 1, linkbuf);
    if (streq(entry->fullname, "/")) {
        entry->name = entry->fullname;
    } else {
        entry->name = strrchr(entry->fullname, '/') + 1; // Last path component
    }
    if (S_ISLNK(filestat.st_mode))
        entry->linkedmode = linkedstat.st_mode;
    entry->info = filestat;
    LL_PREPEND(bb->hash[(int)filestat.st_ino & HASH_MASK], entry, hash);
    entry->index = -1;
    bb->hash[(int)filestat.st_ino & HASH_MASK] = entry;
    return entry;
}

//
// Return whether a string matches a command
// e.g. matches_cmd("sel:x", "select:") == 1, matches_cmd("q", "quit") == 1
//
static int matches_cmd(const char *str, const char *cmd)
{
    if ((strchr(cmd, ':') == NULL) != (strchr(str, ':') == NULL))
        return 0;
    while (*str == *cmd && *cmd && *cmd != ':') ++str, ++cmd;
    return *str == '\0' || *str == ':';
}

//
// Prepend `./` to relative paths, replace "~" with $HOME.
// The normalized path is stored in `normalized`.
//
static char *normalize_path(const char *path, char *normalized)
{
    char pbuf[PATH_MAX] = {0};
    if (path[0] == '~' && (path[1] == '\0' || path[1] == '/')) {
        char *home;
        if (!(home = getenv("HOME"))) return NULL;
        strcpy(pbuf, home);
        ++path;
    } else if (path[0] != '/') {
        strcpy(pbuf, "./");
    }
    strcat(pbuf, path);
    if (realpath(pbuf, normalized) == NULL) {
        strcpy(normalized, pbuf); // TODO: normalize better?
        return NULL;
    }
    return normalized;
}

//
// Remove all the files currently stored in bb->files and if `bb->path` is
// non-NULL, update `bb` with a listing of the files in `path`
//
static int populate_files(bb_t *bb, const char *path)
{
    int clear_future_history = 0;
    if (path == NULL)
        ;
    else if (streq(path, "-")) {
        if (!bb->history)
            return -1;
        if (bb->history->prev)
            bb->history = bb->history->prev;
        path = bb->history->path;
    } else if (streq(path, "+")) {
        if (!bb->history)
            return -1;
        if (bb->history->next)
            bb->history = bb->history->next;
        path = bb->history->path;
    } else
        clear_future_history = 1;

    int samedir = path && streq(bb->path, path);
    int old_scroll = bb->scroll;
    int old_cursor = bb->cursor;
    char old_selected[PATH_MAX] = "";
    if (samedir && bb->nfiles > 0) strcpy(old_selected, bb->files[bb->cursor]->fullname);

    char pbuf[PATH_MAX] = {0}, prev[PATH_MAX] = {0};
    strcpy(prev, bb->path);
    if (path != NULL) {
        if (!normalize_path(path, pbuf))
            flash_warn(bb, "Could not normalize path: \"%s\"", path);
        if (pbuf[strlen(pbuf)-1] != '/')
            strcat(pbuf, "/");
        if (chdir(pbuf)) {
            flash_warn(bb, "Could not cd to: \"%s\"", pbuf);
            return -1;
        }
    }

    if (clear_future_history && !samedir) {
        for (bb_history_t *next, *h = bb->history ? bb->history->next : NULL; h; h = next) {
            next = h->next;
            delete(&h);
        }

        bb_history_t *h = new(bb_history_t);
        strcpy(h->path, pbuf);
        h->prev = bb->history;
        if (bb->history) bb->history->next = h;
        bb->history = h;
    }

    bb->dirty = 1;
    strcpy(bb->path, pbuf);
    set_title(bb);

    // Clear old files (if any)
    if (bb->files) {
        for (int i = 0; i < bb->nfiles; i++) {
            bb->files[i]->index = -1;
            try_free_entry(bb->files[i]);
            bb->files[i] = NULL;
        }
        delete(&bb->files);
    }
    bb->nfiles = 0;
    bb->cursor = 0;
    bb->scroll = 0;

    if (!bb->path[0])
        return 0;

    size_t space = 0;
    glob_t globbuf = {0};
    char *pat, *tmpglob = check_strdup(bb->globpats);
    while ((pat = strsep(&tmpglob, " ")) != NULL)
        glob(pat, GLOB_NOSORT|GLOB_APPEND, NULL, &globbuf);
    delete(&tmpglob);
    for (size_t i = 0; i < globbuf.gl_pathc; i++) {
        // Don't normalize path so we can get "." and ".."
        entry_t *entry = load_entry(bb, globbuf.gl_pathv[i]);
        if (!entry) {
            flash_warn(bb, "Failed to load entry: '%s'", globbuf.gl_pathv[i]);
            continue;
        }
        entry->index = bb->nfiles;
        if ((size_t)bb->nfiles + 1 > space)
            bb->files = grow(bb->files, space += 100);
        bb->files[bb->nfiles++] = entry;
    }
    globfree(&globbuf);

    // RNG is seeded with a hash of all the inodes in the current dir
    // This hash algorithm is based on Python's frozenset hashing
    unsigned long seed = (unsigned long)bb->nfiles * 1927868237UL;
    for (int i = 0; i < bb->nfiles; i++)
        seed ^= ((bb->files[i]->info.st_ino ^ 89869747UL) ^ (bb->files[i]->info.st_ino << 16)) * 3644798167UL;
    srand((unsigned int)seed);
    for (int i = 0; i < bb->nfiles; i++) {
        int j = rand() % (i+1); // This introduces some RNG bias, but it's not important here
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

//
// Print the current key bindings
//
static void print_bindings(int fd)
{
    FOREACH(binding_t*, b, bindings) {
        if (!b->description) break;
        if (b->key == -1) {
            const char *label = b->description;
            dprintf(fd, "\n\033[33;1;4m\033[%dG%s\033[0m\n", (winsize.ws_col-(int)strlen(label))/2, label);
            continue;
        }
        char buf[1000];
        char *p = buf;
        for (binding_t *next = b; next < &bindings[LEN(bindings)] && next->script && streq(b->description, next->description); next++) {
            if (next > b) p = stpcpy(p, ", ");
            p = bkeyname(next->key, p);
            b = next;
        }
        *p = '\0';
        dprintf(fd, "\033[1m\033[%dG%s\033[0m", winsize.ws_col/2 - 1 - (int)strlen(buf), buf);
        dprintf(fd, "\033[1m\033[%dG\033[34m%s\033[0m", winsize.ws_col/2 + 1, b->description);
        write(fd, "\033[0m\n", strlen("\033[0m\n"));
    }
    write(fd, "\n", 1);
}

//
// Run a bb internal command (e.g. "+refresh") and return an indicator of what
// needs to happen next.
//
static void run_bbcmd(bb_t *bb, const char *cmd)
{
    while (*cmd == ' ' || *cmd == '\n') ++cmd;
    if (strncmp(cmd, "bbcmd ", strlen("bbcmd ")) == 0) cmd = &cmd[strlen("bbcmd ")];
    const char *value = strchr(cmd, ':');
    if (value) ++value;
    if (matches_cmd(cmd, "bind:")) { // +bind:<keys>:<script>
        char *value_copy = check_strdup(value);
        char *keys = trim(value_copy);
        if (!keys[0]) { delete(&value_copy); return; }
        char *script = strchr(keys+1, ':');
        if (!script) {
            delete(&value_copy);
            flash_warn(bb, "No script provided.");
            return;
        }
        *script = '\0';
        script = trim(script + 1);
        char *description;
        if (script[0] == '#') {
            description = trim(strsep(&script, "\n") + 1);
            if (!script) script = (char*)"";
            else script = trim(script);
        } else description = script;
        for (char *key; (key = strsep(&keys, ",")); ) {
            int keyval;
            if (streq(key, "Section")) {
                keyval = -1;
            } else {
                keyval = bkeywithname(key);
                if (keyval == -1) continue;
                // Delete existing bindings for this key (if any):
                FOREACH(binding_t*, b, bindings) {
                    if (b->key == keyval) {
                        delete((char**)&b->description);
                        delete((char**)&b->script);
                        memmove(b, b+1, (size_t)(&bindings[LEN(bindings)] - (b+1)));
                        memset(&bindings[LEN(bindings)-1], 0, sizeof(binding_t));
                    }
                }
            }
            // Append binding:
            FOREACH(binding_t*, b, bindings) {
                if (b->script) continue;
                b->key = keyval;
                if (is_simple_bbcmd(script))
                    b->script = check_strdup(script);
                else
                    nonnegative(asprintf((char**)&b->script, "set -e\n%s", script), "Could not copy script");
                b->description = check_strdup(description);
                break;
            }
        }
        delete(&value_copy);
    } else if (matches_cmd(cmd, "cd:")) { // +cd:
        if (populate_files(bb, value))
            flash_warn(bb, "Could not open directory: \"%s\"", value);
    } else if (matches_cmd(cmd, "columns:")) { // +columns:
        set_columns(bb, value);
    } else if (matches_cmd(cmd, "deselect")) { // +deselect
        while (bb->selected)
            set_selected(bb, bb->selected, 0);
    } else if (matches_cmd(cmd, "deselect:")) { // +deselect:<file>
        char pbuf[PATH_MAX];
        normalize_path(value, pbuf);
        entry_t *e = load_entry(bb, pbuf);
        if (e) {
            set_selected(bb, e, 0);
            return;
        }
        // Filename may no longer exist:
        for (e = bb->selected; e; e = e->selected.next) {
            if (streq(e->fullname, pbuf)) {
                set_selected(bb, e, 0);
                return;
            }
        }
    } else if (matches_cmd(cmd, "fg:") || matches_cmd(cmd, "fg")) { // +fg:
        int nprocs = 0;
        for (proc_t *p = bb->running_procs; p; p = p->running.next) ++nprocs;
        int fg = value ? nprocs - (int)strtol(value, NULL, 10) : 0;
        proc_t *child = NULL;
        for (proc_t *p = bb->running_procs; p && !child; p = p->running.next) {
            if (fg-- == 0) child = p;
        }
        if (!child) return;
        move_cursor(tty_out, 0, winsize.ws_row-1);
        fputs("\033[K", tty_out);
        restore_term(&orig_termios);
        signal(SIGTTOU, SIG_IGN);
        nonnegative(tcsetpgrp(fileno(tty_out), child->pid));
        kill(-(child->pid), SIGCONT);
        wait_for_process(&child);
        signal(SIGTTOU, SIG_DFL);
        init_term();
        set_title(bb);
        bb->dirty = 1;
    } else if (matches_cmd(cmd, "glob:")) { // +glob:
        set_globs(bb, value[0] ? value : "*");
        populate_files(bb, bb->path);
    } else if (matches_cmd(cmd, "goto:") || matches_cmd(cmd, "goto")) { // +goto:
        if (!value && !bb->selected) return;
        entry_t *e = load_entry(bb, value ? value : bb->selected->fullname);
        if (!e) {
            flash_warn(bb, "Could not find file to go to: \"%s\"", value);
            return;
        }
        char pbuf[PATH_MAX];
        strcpy(pbuf, e->fullname);
        char *lastslash = strrchr(pbuf, '/');
        if (!lastslash) errx(EXIT_FAILURE, "No slash found in filename: %s", pbuf);
        *lastslash = '\0'; // Split in two
        try_free_entry(e);
        // Move to dir and reselect
        populate_files(bb, pbuf);
        e = load_entry(bb, lastslash+1);
        if (!e) {
            flash_warn(bb, "Could not find file again: \"%s\"", lastslash+1);
        }
        if (IS_VIEWED(e))
            set_cursor(bb, e->index);
        else try_free_entry(e);
    } else if (matches_cmd(cmd, "help")) { // +help
        char filename[PATH_MAX];
        sprintf(filename, "%s/"BB_NAME".help.XXXXXX", getenv("TMPDIR"));
        int fd = nonnegative(mkstemp(filename), "Couldn't create temporary help file at %s", filename);
        print_bindings(fd);
        close(fd);
        char script[512] = "less -rKX < ";
        strcat(script, filename);
        run_script(bb, script);
        nonnegative(unlink(filename), "Couldn't delete temporary help file: '%s'", filename);
    } else if (matches_cmd(cmd, "interleave:") || matches_cmd(cmd, "interleave")) { // +interleave
        bb->interleave_dirs = value ? (value[0] == '1') : !bb->interleave_dirs;
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
        else flash_warn(bb, "Could not find file to select: \"%s\"", value);
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
        else flash_warn(bb, "Could not find file to toggle: \"%s\"", value);
    } else {
        flash_warn(bb, "Invalid bb command: %s", cmd);
    }
}

//
// Close the /dev/tty terminals and restore some of the attributes.
//
static void restore_term(const struct termios *term)
{
    tcsetattr(fileno(tty_out), TCSANOW, term);
    fputs(T_LEAVE_BBMODE_PARTIAL, tty_out);
    fflush(tty_out);
}

//
// Run a shell script with the selected files passed as sequential arguments to
// the script (or pass the cursor file if none are selected).
// Return the exit status of the script.
//
static int run_script(bb_t *bb, const char *cmd)
{
    proc_t *proc = new(proc_t);
    signal(SIGTTOU, SIG_IGN);
    if ((proc->pid = nonnegative(fork())) == 0) {
        pid_t pgrp = getpid();
        (void)setpgid(0, pgrp);
        nonnegative(tcsetpgrp(STDIN_FILENO, pgrp));
        const char **args = new(char*[4 + (size_t)bb->nselected + 1]);
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

        setenv("BB", bb->nfiles ? bb->files[bb->cursor]->fullname : "", 1);

        dup2(fileno(tty_out), STDOUT_FILENO);
        dup2(fileno(tty_in), STDIN_FILENO);
        execvp(args[0], (char**)args);
        err(EXIT_FAILURE, "Failed to execute command: '%s'", cmd);
        return -1;
    }

    (void)setpgid(getpid(), getpid());
    LL_PREPEND(bb->running_procs, proc, running);
    int status = wait_for_process(&proc);
    bb->dirty = 1;
    return status;
}

//
// Set the columns displayed by bb.
//
static void set_columns(bb_t *bb, const char *cols)
{
    strncpy(bb->columns, cols, MAX_COLS);
    setenv("BBCOLUMNS", bb->columns, 1);
}

//
// Set bb's file cursor to the given index (and adjust the scroll as necessary)
//
static void set_cursor(bb_t *bb, int newcur)
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

//
// Set the glob pattern(s) used by bb. Patterns are ' ' delimited
//
static void set_globs(bb_t *bb, const char *globs)
{
    delete(&bb->globpats);
    bb->globpats = check_strdup(globs);
    setenv("BBGLOB", bb->globpats, 1);
}


//
// Set whether or not bb should interleave directories and files.
//
static void set_interleave(bb_t *bb, int interleave)
{
    bb->interleave_dirs = interleave;
    if (interleave) setenv("BBINTERLEAVE", "interleave", 1);
    else unsetenv("BBINTERLEAVE");
    bb->dirty = 1;
}

//
// Set bb's scroll to the given index (and adjust the cursor as necessary)
//
static void set_scroll(bb_t *bb, int newscroll)
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

//
// Select or deselect a file.
//
static void set_selected(bb_t *bb, entry_t *e, int selected)
{
    if (IS_SELECTED(e) == selected) return;

    if (bb->nfiles > 0 && e != bb->files[bb->cursor])
        bb->dirty = 1;

    if (selected) {
        LL_PREPEND(bb->selected, e, selected);
        ++bb->nselected;
    } else {
        LL_REMOVE(e, selected);
        try_free_entry(e);
        --bb->nselected;
    }
}

//
// Set the sorting method used by bb to display files.
//
static void set_sort(bb_t *bb, const char *sort)
{
    char sortbuf[MAX_SORT+1];
    strncpy(sortbuf, sort, MAX_SORT);
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

//
// Set the xwindow title property to: bb - current path
//
static void set_title(bb_t *bb)
{
    char *home = getenv("HOME");
    if (home && strncmp(bb->path, home, strlen(home)) == 0)
        fprintf(tty_out, "\033]2;"BB_NAME": ~%s\007", bb->path + strlen(home));
    else
        fprintf(tty_out, "\033]2;"BB_NAME": %s\007", bb->path);
}

//
// If the given entry is not viewed or selected, remove it from the
// hash, free it, and return 1.
//
static int try_free_entry(entry_t *e)
{
    if (IS_SELECTED(e) || IS_VIEWED(e) || !IS_LOADED(e)) return 0;
    LL_REMOVE(e, hash);
    delete(&e);
    return 1;
}

//
// Sort the files in bb according to bb's settings.
//
static void sort_files(bb_t *bb)
{
    qsort(bb->files, (size_t)bb->nfiles, sizeof(entry_t*), compare_files);
    for (int i = 0; i < bb->nfiles; i++)
        bb->files[i]->index = i;
    bb->dirty = 1;
}

//
// Trim trailing whitespace by inserting '\0' and return a pointer to after the
// first non-whitespace char
//
static char *trim(char *s)
{
    if (!s) return NULL;
    while (*s == ' ' || *s == '\n') ++s;
    char *end;
    for (end = &s[strlen(s)-1]; end >= s && (*end == ' ' || *end == '\n'); end--)
        *end = '\0';
    return s;
}

//
// Hanlder for SIGWINCH events
//
static void update_term_size(int sig)
{
    (void)sig;
    ioctl(STDIN_FILENO, TIOCGWINSZ, &winsize);
}

//
// Wait for a process to either suspend or exit and return the status.
//
static int wait_for_process(proc_t **proc)
{
    tcsetpgrp(fileno(tty_out), (*proc)->pid);
    int status;
    while (*proc) {
        if (waitpid((*proc)->pid, &status, WUNTRACED) < 0)
            continue;
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            LL_REMOVE((*proc), running);
            delete(proc);
        } else if (WIFSTOPPED(status))
            break;
    }
    nonnegative(tcsetpgrp(fileno(tty_out), getpid()));
    signal(SIGTTOU, SIG_DFL);
    return status;
}

int main(int argc, char *argv[])
{
    char sep = '\n';
    int print_dir = 0, print_selection = 0;
    for (int i = 1; i < argc; i++) {
        // Commands are processed below, after flags have been parsed
        if (argv[i][0] == '+') {
            char *colon = strchr(argv[i], ':');
            if (colon && !colon[1])
                break;
        } else if (streq(argv[i], "--")) {
            break;
        } else if (streq(argv[i], "--help")) {
          help:
            printf("%s%s", description_str, usage_str);
            return 0;
        } else if (streq(argv[i], "--version")) {
          version:
            printf(BB_NAME" "BB_VERSION"\n");
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
        } else if (i < argc-1) {
            printf("Unknown command line argument: \"%s\"\n%s", argv[i], usage_str);
            return 1;
        }
    }

    struct sigaction sa_winch = {.sa_handler = &update_term_size};
    sigaction(SIGWINCH, &sa_winch, NULL);
    update_term_size(0);
    // Wait 100us at a time for terminal to initialize if necessary
    while (winsize.ws_row == 0)
        usleep(100);

    // Set up environment variables
    // Default values
    setenv("TMPDIR", "/tmp", 0);
    sprintf(cmdfilename, "%s/"BB_NAME".cmd.XXXXXX", getenv("TMPDIR"));
    int cmdfd = nonnegative(
        mkostemp(cmdfilename, O_APPEND),
        "Couldn't create "BB_NAME" command file: '%s'", cmdfilename);
    close(cmdfd);
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
        nonnull(realpath(argv[0], bbpath), "Could not resolve path: %s", bbpath);
        char *slash = strrchr(bbpath, '/');
        if (!slash) errx(EXIT_FAILURE, "No slash found in real path: %s", bbpath);
        *slash = '\0';
        setenv("BBPATH", bbpath, 1);
    }
    if (getenv("BBPATH")) {
        nonnegative(
            asprintf(&newpath, "%s/"BB_NAME":%s/scripts:%s",
                     getenv("XDG_CONFIG_HOME"), getenv("BBPATH"), getenv("PATH")),
            "Could not allocate memory for PATH");
    } else {
        nonnegative(
            asprintf(&newpath, "%s/"BB_NAME":%s/"BB_NAME":%s",
                     getenv("XDG_CONFIG_HOME"), getenv("sysconfdir"), getenv("PATH")),
            "Could not allocate memory for PATH");
    }
    setenv("PATH", newpath, 1);

    setenv("SHELL", "bash", 0);
    setenv("EDITOR", "nano", 0);
    char *curdepth = getenv("BBDEPTH");
    int depth = curdepth ? atoi(curdepth) : 0;
    char depthstr[16];
    sprintf(depthstr, "%d", depth + 1);
    setenv("BBDEPTH", depthstr, 1);

    atexit(cleanup);

    FOREACH(outbuf_t*, ob, output_buffers) {
        sprintf(ob->filename, "%s/"BB_NAME".%s.XXXXXX", getenv("TMPDIR"), ob->name);
        ob->tmp_fd = nonnegative(mkostemp(ob->filename, O_RDWR), "Couldn't create error file");
        ob->dup_fd = nonnegative(dup(ob->orig_fd));
        nonnegative(dup2(ob->tmp_fd, ob->orig_fd), "Couldn't redirect error output");
    }

    tty_in = nonnull(fopen("/dev/tty", "r"));
    tty_out = nonnull(fopen("/dev/tty", "w"));
    nonnegative(tcgetattr(fileno(tty_out), &orig_termios));
    memcpy(&bb_termios, &orig_termios, sizeof(bb_termios));
    cfmakeraw(&bb_termios);
    bb_termios.c_cc[VMIN] = 0;
    bb_termios.c_cc[VTIME] = 1;

    int signals[] = {SIGTERM, SIGINT, SIGXCPU, SIGXFSZ, SIGVTALRM, SIGPROF, SIGSEGV, SIGTSTP};
    struct sigaction sa = {.sa_handler = &cleanup_and_raise, .sa_flags = (int)(SA_NODEFER | SA_RESETHAND)};
    for (size_t i = 0; i < LEN(signals); i++)
        sigaction(signals[i], &sa, NULL);

    bb_t bb = {
        .columns = "*smpn",
        .sort = "+n",
        .history = NULL,
    };
    current_bb = &bb;
    set_globs(&bb, "*");
    init_term();
    bb_browse(&bb, argc, argv);
    cleanup(); // Optional, but this allows us to write directly to stdout instead of the buffer

    if (bb.selected && print_selection) {
        for (entry_t *e = bb.selected; e; e = e->selected.next) {
            write(STDOUT_FILENO, e->fullname, strlen(e->fullname));
            write(STDOUT_FILENO, &sep, 1);
        }
    }

    if (print_dir)
        printf("%s\n", bb.path);

    // Cleanup:
    populate_files(&bb, NULL);
    while (bb.selected)
        set_selected(&bb, bb.selected, 0);
    delete(&bb.globpats);
    for (bb_history_t *next; bb.history; bb.history = next) {
        next = bb.history->next;
        delete(&bb.history);
    }
    return 0;
}

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
