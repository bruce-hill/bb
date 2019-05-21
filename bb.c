/*
 * Bruce's Browser (bb)
 * Copyright 2019 Bruce Hill
 * Released under the MIT license
 */
#include <dirent.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dir.h>
#include <sys/errno.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "keys.h"

#define MAX(a,b) ((a) < (b) ? (b) : (a))
#define MIN(a,b) ((a) > (b) ? (b) : (a))
#define MAX_PATH 4096
#define writez(fd, str) write(fd, str, strlen(str))
#define IS_SELECTED(p) ((p)->next || (p)->prev)

static const int SCROLLOFF = 5;

static struct termios orig_termios;
static int termfd;
static int width, height;
static int mouse_x, mouse_y;

typedef struct entry_s {
    struct entry_s *next, *prev;
    int visible : 1;
    ino_t      d_ino;
    __uint16_t d_reclen;
    __uint8_t  d_type;
    __uint8_t  d_namlen;
    char *d_name;
    char d_fullname[0];
} entry_t;

typedef struct {
    char *path;
    entry_t *firstselected, **files;
    size_t nselected, nfiles;
    int scroll, cursor;
    struct timespec lastclick;
} bb_state_t;

static void err(const char *msg, ...);

static void update_term_size(void)
{
    struct winsize sz = {0};
    ioctl(termfd, TIOCGWINSZ, &sz);
    width = sz.ws_col;
    height = sz.ws_row;
}

static void init_term()
{
    termfd = open("/dev/tty", O_RDWR);
    tcgetattr(termfd, &orig_termios);
    struct termios tios;
    memcpy(&tios, &orig_termios, sizeof(tios));
    tios.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP
                           | INLCR | IGNCR | ICRNL | IXON);
    tios.c_oflag &= ~OPOST;
    tios.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    tios.c_cflag &= ~(CSIZE | PARENB);
    tios.c_cflag |= CS8;
    tios.c_cc[VMIN] = 0;
    tios.c_cc[VTIME] = 0;
    tcsetattr(termfd, TCSAFLUSH, &tios);
    // xterm-specific:
    writez(termfd, "\e[?1049h");
    update_term_size();
    // Initiate mouse tracking:
    writez(termfd, "\e[?1000h\e[?1002h\e[?1015h\e[?1006h");
    // hide cursor
    writez(termfd, "\e[?25l");
}

static void close_term()
{
    // xterm-specific:
    writez(termfd, "\e[?1049l");
    // Show cursor:
    writez(termfd, "\e[?25h");
    tcsetattr(termfd, TCSAFLUSH, &orig_termios);
    close(termfd);
}

static void err(const char *msg, ...)
{
    close_term();
    va_list args;
    va_start(args, msg);
    int len = fprintf(stderr, msg, args);
    va_end(args);
    if (errno)
        fprintf(stderr, "\n%s", strerror(errno));
    fprintf(stderr, "\n");
    _exit(1);
}

static pid_t run_cmd(int *child_out, int *child_in, const char *cmd, ...)
{
    int child_outfds[2], child_infds[2];
    pid_t child;
    if (child_out)
        pipe(child_outfds);
    if (child_in)
        pipe(child_infds);
    if ((child = fork())) {
        if (child == -1)
            err("Failed to fork");
        if (child_out) {
            *child_out = child_outfds[0];
            close(child_outfds[1]);
        }
        if (child_in) {
            *child_in = child_infds[1];
            close(child_infds[0]);
        }
    } else {
        if (child_out) {
            dup2(child_outfds[1], STDOUT_FILENO);
            close(child_outfds[0]);
        }
        if (child_in) {
            dup2(child_infds[0], STDIN_FILENO);
            close(child_infds[1]);
        }
        char *formatted_cmd;
        va_list args;
        va_start(args, cmd);
        int len = vasprintf(&formatted_cmd, cmd, args);
        va_end(args);
        if (formatted_cmd)
            execlp("sh", "sh", "-c", formatted_cmd);
        err("Failed to execute command %d: '%s'", len, formatted_cmd);
        _exit(0);
    }
    return child;
}

static void term_move(int x, int y)
{
    static char buf[32] = {0};
    int len = snprintf(buf, sizeof(buf), "\e[%d;%dH", y+1, x+1);
    if (len > 0)
        write(termfd, buf, len);
}

static void render(bb_state_t *state)
{
    writez(termfd, "\e[2J\e[0;1m"); // Clear, reset color + bold
    term_move(0,0);
    writez(termfd, state->path);
    writez(termfd, "\e[0m"); // Reset color

    char fullpath[MAX_PATH];
    size_t pathlen = strlen(state->path);
    strncpy(fullpath, state->path, pathlen + 1);
    entry_t **files = state->files;
    for (int i = state->scroll; i < state->scroll + height - 3 && i < state->nfiles; i++) {
        entry_t *entry = files[i];

        int x = 0;
        int y = i - state->scroll + 1;
        term_move(x, y);

        // Selection box:
        if (IS_SELECTED(entry))
            writez(termfd, "\e[43m \e[0m"); // Yellow BG
        else
            writez(termfd, " ");

        if (i != state->cursor && entry->d_type & DT_DIR) {
            writez(termfd, "\e[34m"); // Blue FG
        }
        if (i == state->cursor) {
            writez(termfd, "\e[7m"); // Reverse color
        }

        struct stat info = {0};
        fullpath[pathlen] = '/';
        strncpy(fullpath + pathlen + 1, entry->d_name, entry->d_namlen);
        fullpath[pathlen + 1 + entry->d_namlen] = '\0';
        lstat(fullpath, &info);

        {
            // Filesize:
            int j = 0;
            const char* units = "BKMGTPEZY";
            double bytes = (double)info.st_size;
            while (bytes > 1024) {
                bytes /= 1024;
                j++;
            }
            char buf[16] = {0};
            sprintf(buf, "%6.*f%c  ", j > 0 ? 1 : 0, bytes, units[j]);
            writez(termfd, buf);
        }

        {
            // Date:
            char buf[64];
            strftime(buf, sizeof(buf), "%l:%M%p %b %e %Y", localtime(&(info.st_mtime)));
            writez(termfd, buf);
            writez(termfd, "  ");
        }

        // Name:
        write(termfd, entry->d_name, entry->d_namlen);

        if (entry->d_type & DT_DIR) {
            writez(termfd, "/");
        }
        if (entry->d_type == DT_LNK) {
            char linkpath[MAX_PATH] = {0};
            ssize_t pathlen;
            if ((pathlen = readlink(entry->d_name, linkpath, sizeof(linkpath))) < 0)
                err("readlink() failed");
            writez(termfd, "\e[36m -> "); // Cyan FG
            write(termfd, linkpath, pathlen);
        }
        writez(termfd, "\e[0m"); // Reset color and attributes
    }

    term_move(0, height - 1);
    char buf[32] = {0};
    int len = snprintf(buf, sizeof(buf), "%lu selected", state->nselected);
    write(termfd, buf, len);
}

static int compare_alpha(const void *v1, const void *v2)
{
    const entry_t *f1 = *((const entry_t**)v1), *f2 = *((const entry_t**)v2);
    int diff;
    diff = (f1->d_type & DT_DIR) - (f2->d_type & DT_DIR);
    if (diff) return -diff;
    const char *p1 = f1->d_name, *p2 = f2->d_name;
    while (*p1 && *p2) {
        int diff = (*p1 - *p2);
        if ('0' <= *p1 && *p1 <= '9' && '0' <= *p2 && *p2 <= '9') {
            long n1 = strtol(p1, (char**)&p1, 10);
            long n2 = strtol(p2, (char**)&p2, 10);
            diff = ((p1 - f1->d_name) - (p2 - f2->d_name)) || (n1 - n2);
            if (diff) return diff;
        } else if (diff) {
            return diff;
        } else {
            ++p1, ++p2;
        }
    }
    return *p1 - *p2;
}

static void write_selection(int fd, entry_t *firstselected)
{
    while (firstselected) {
        const char *p = firstselected->d_fullname;
        while (*p) {
            const char *p2 = strchr(p, '\n');
            if (!p2) p2 = p + strlen(p);
            write(fd, p, p2 - p);
            if (*p2 == '\n')
                write(fd, "\\", 1);
            p = p2;
        }
        write(fd, "\n", 1);
        firstselected = firstselected->next;
    }
}

static int term_get_event()
{
    char c;
    if (read(termfd, &c, 1) != 1)
        return -1;

    if (c != '\x1b')
        return c;

    // Actual escape key:
    if (read(termfd, &c, 1) != 1)
        return KEY_ESC;

    switch (c) {
        case '[':
            if (read(termfd, &c, 1) != 1)
                return -1;
            switch (c) {
                case 'H': return KEY_HOME;
                case 'F': return KEY_END;
                case '<': // Mouse clicks
                    {
                        int buttons = 0, x = 0, y = 0;
                        char buf;
                        while (read(termfd, &buf, 1) == 1 && '0' <= buf && buf <= '9')
                            buttons = buttons * 10 + (buf - '0');
                        if (buf != ';') return -1;
                        while (read(termfd, &buf, 1) == 1 && '0' <= buf && buf <= '9')
                            x = x * 10 + (buf - '0');
                        if (buf != ';') return -1;
                        while (read(termfd, &buf, 1) == 1 && '0' <= buf && buf <= '9')
                            y = y * 10 + (buf - '0');
                        if (buf != 'm' && buf != 'M') return -1;

                        mouse_x = x - 1, mouse_y = y - 1;

                        if (buf == 'm')
                            return KEY_MOUSE_RELEASE;
                        switch (buttons) {
                            case 0: return KEY_MOUSE_LEFT;
                            case 1: return KEY_MOUSE_RIGHT;
                            case 2: return KEY_MOUSE_MIDDLE;
                            default: return -1;
                        }
                    }
                    break;
                default:
                    break;
            }
            return -1;
        default:
            return -1;
    }
    return -1;
}

static void explore(char *path)
{
    char *tmp = path;
    path = malloc(strlen(tmp) + 1);
    strcpy(path, tmp);
    tmp = NULL;
    char to_select[MAX_PATH] = {0};
    bb_state_t state = {0};
    memset(&state, 0, sizeof(bb_state_t));

  tail_call:

    state.path = path;

    DIR *dir = opendir(state.path);
    if (!dir)
        err("Couldn't open dir: %s", state.path);
    if (chdir(state.path) != 0)
        err("Couldn't chdir into %s", state.path);
    struct dirent *dp;

    // Hash inode -> entry_t with linear probing
    size_t hashsize = 2 * state.nselected;
    entry_t **selecthash = calloc(hashsize, sizeof(entry_t*));
    if (!selecthash)
        err("Failed to allocate %d spaces for selecthash", hashsize);
    for (entry_t *p = state.firstselected; p; p = p->next) {
        int probe = ((int)p->d_ino) % hashsize;
        while (selecthash[probe])
            probe = (probe + 1) % hashsize;
        selecthash[probe] = p;
    }

    if (state.files) {
        for (int i = 0; i < state.nfiles; i++) {
            entry_t *e = state.files[i];
            e->visible = 0;
            if (!IS_SELECTED(e))
                free(e);
        }
        free(state.files);
        state.files = NULL;
    }
    size_t pathlen = strlen(state.path);
    size_t filecap = 0;
    state.nfiles = 0;
    while ((dp = readdir(dir)) != NULL) {
        if (dp->d_name[0] == '.' && dp->d_name[1] == '\0')
            continue;
        // Hashed lookup from selected:
        if (state.firstselected) {
            for (int probe = ((int)dp->d_ino) % hashsize; selecthash[probe]; probe = (probe + 1) % hashsize) {
                if (selecthash[probe]->d_ino == dp->d_ino) {
                    selecthash[probe]->visible = 1;
                    state.files[state.nfiles++] = selecthash[probe];
                    goto next_file;
                }
            }
        }
        entry_t *entry = malloc(sizeof(entry_t) + pathlen + dp->d_namlen + 2);
        strncpy(entry->d_fullname, state.path, pathlen);
        entry->d_fullname[pathlen] = '/';
        entry->d_name = &entry->d_fullname[pathlen + 1];
        strncpy(entry->d_name, dp->d_name, dp->d_namlen + 1);
        entry->d_ino = dp->d_ino;
        entry->d_reclen = dp->d_reclen;
        entry->d_type = dp->d_type;
        entry->d_namlen = dp->d_namlen;
        entry->next = NULL, entry->prev = NULL;

        if (state.nfiles >= filecap) {
            filecap += 100;
            state.files = realloc(state.files, filecap*sizeof(entry_t*));
        }
        state.files[state.nfiles++] = entry;
      next_file:;
    }
    free(selecthash);
    if (state.nfiles == 0) err("No files found (not even '..')");
    qsort(state.files, state.nfiles, sizeof(entry_t*), compare_alpha);
    closedir(dir);

    state.cursor = 0;
    state.scroll = 0;

    if (to_select[0]) {
        for (int i = 0; i < state.nfiles; i++) {
            if (strcmp(to_select, state.files[i]->d_name) == 0) {
                state.cursor = i;
                break;
            }
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &state.lastclick);
    int picked, scrolloff;

    while (1) {
      redraw:
        render(&state);
      skip_redraw:
        scrolloff = MIN(SCROLLOFF, height/2);
        //sleep(2);
        //if (1) goto done;
        int key = term_get_event();
        switch (key) {
            case KEY_MOUSE_LEFT: {
                struct timespec clicktime;
                clock_gettime(CLOCK_MONOTONIC, &clicktime);
                double dt_ms = 1e3*(double)(clicktime.tv_sec - state.lastclick.tv_sec);
                dt_ms += 1e-6*(double)(clicktime.tv_nsec - state.lastclick.tv_nsec);
                state.lastclick = clicktime;
                if (mouse_y > 0 && state.scroll + (mouse_y - 1) < state.nfiles) {
                    int clicked = state.scroll + (mouse_y - 1);
                    if (dt_ms > 200) {
                    // Single click
                        if (mouse_x == 0) {
                            // Toggle
                            picked = clicked;
                            goto toggle;
                        } else {
                            state.cursor = clicked;
                            goto redraw;
                        }
                    } else {
                        // Double click
                        picked = clicked;
                        goto open_file;
                    }
                }
                break;
            }

            case 'q': case 'Q': case KEY_CTRL_C:
                goto done;

            case KEY_CTRL_D:
                state.cursor = MIN(state.nfiles - 1, state.cursor + (height - 3) / 2);
                if (state.nfiles <= height - 3)
                    goto redraw;
                state.scroll += (height - 3)/2;
                if (state.scroll > state.nfiles - (height - 3))
                    state.scroll = state.nfiles - (height - 3);
                goto redraw;

            case KEY_CTRL_U:
                state.cursor = MAX(0, state.cursor - (height - 3) / 2);
                if (state.nfiles <= height - 3)
                    goto redraw;
                state.scroll -= (height - 3)/2;
                if (state.scroll < 0)
                    state.scroll = 0;
                goto redraw;

            case ' ':
                picked = state.cursor;
              toggle:
                if (IS_SELECTED(state.files[picked])) {
                  toggle_off:;
                    entry_t *e = state.files[picked];
                    if (state.firstselected == e)
                        state.firstselected = e->next;
                    if (e->prev) e->prev->next = e->next;
                    if (e->next) e->next->prev = e->prev;
                    e->next = NULL;
                    e->prev = NULL;
                    --state.nselected;
                } else {
                  toggle_on:;
                    entry_t *e = state.files[picked];
                    if (state.firstselected) {
                        e->next = state.firstselected;
                        state.firstselected->prev = e;
                    }
                    state.firstselected = e;
                    ++state.nselected;
                }
                goto redraw;

            case KEY_ESC:
                for (entry_t *e = state.firstselected; e; e = e->next) {
                    e->next = NULL;
                    e->prev = NULL;
                    if (!e->visible) free(e);
                }
                state.firstselected = NULL;
                state.nselected = 0;
                goto redraw;

            case 'j':
                if (state.cursor >= state.nfiles - 1)
                    goto skip_redraw;
                ++state.cursor;
                if (state.cursor > state.scroll + height - 4 - scrolloff && state.scroll < state.nfiles - (height - 3)) {
                    ++state.scroll;
                }
                goto redraw;

            case 'k':
                if (state.cursor <= 0)
                    goto skip_redraw;
                --state.cursor;
                if (state.cursor < state.scroll + scrolloff && state.scroll > 0) {
                    --state.scroll;
                }
                goto redraw;

            case 'J':
                if (state.cursor < state.nfiles - 1) {
                    if (IS_SELECTED(state.files[state.cursor])) {
                        picked = ++state.cursor;
                        goto toggle_on;
                    } else {
                        picked = ++state.cursor;
                        goto toggle_off;
                    }
                }
                goto skip_redraw;

            case 'K':
                if (state.cursor > 0) {
                    if (IS_SELECTED(state.files[state.cursor])) {
                        picked = --state.cursor;
                        goto toggle_on;
                    } else {
                        picked = --state.cursor;
                        goto toggle_off;
                    }
                }
                goto skip_redraw;

            case 'h':
                if (strcmp(state.path, "/") != 0) {
                    char *p = strrchr(state.path, '/');
                    if (p) strcpy(to_select, p+1);
                    else to_select[0] = '\0';
                    char tmp[MAX_PATH], tmp2[MAX_PATH];
                    strcpy(tmp, state.path);
                    strcat(tmp, "/");
                    strcat(tmp, "..");
                    if (!realpath(tmp, tmp2))
                        err("realpath failed");
                    free(path);
                    path = malloc(strlen(tmp2) + 1);
                    strcpy(path, tmp2);
                    goto tail_call;
                }
                break;

            case 'l': case '\r':
                picked = state.cursor;
              open_file:
                {
                    int is_dir = state.files[picked]->d_type & DT_DIR;
                    if (state.files[picked]->d_type == DT_LNK) {
                        char linkpath[MAX_PATH];
                        if (readlink(state.files[picked]->d_name, linkpath, sizeof(linkpath)) < 0)
                            err("readlink() failed");
                        DIR *dir = opendir(linkpath);
                        if (dir) {
                            is_dir = 1;
                            if (closedir(dir) < 0)
                                err("Failed to close directory: %s", linkpath);
                        }
                    }
                    if (is_dir) {
                        if (strcmp(state.files[picked]->d_name, "..") == 0) {
                            char *p = strrchr(state.path, '/');
                            if (p) strcpy(to_select, p+1);
                            else to_select[0] = '\0';
                        } else to_select[0] = '\0';

                        char tmp[MAX_PATH], tmp2[MAX_PATH];
                        strcpy(tmp, state.path);
                        strcat(tmp, "/");
                        strcat(tmp, state.files[picked]->d_name);
                        if (!realpath(tmp, tmp2))
                            err("realpath failed");
                        free(path);
                        path = malloc(strlen(tmp2) + 1);
                        strcpy(path, tmp2);
                        goto tail_call;
                    } else {
                        char *name = state.files[picked]->d_name;
                        close_term();
                        pid_t child = run_cmd(NULL, NULL,
#ifdef __APPLE__
                                "if file -bI %s | grep '^text/' >/dev/null; then $EDITOR %s; else open %s; fi",
#else
                                "if file -bi %s | grep '^text/' >/dev/null; then $EDITOR %s; else xdg-open %s; fi",
#endif
                                name, name, name);
                        waitpid(child, NULL, 0);
                        init_term();
                        goto redraw;
                    }
                    break;
                }

            case 'm':
                if (state.nselected) {
                    int fd;
                    run_cmd(NULL, &fd, "xargs -I {} mv {} .");
                    write_selection(fd, state.firstselected);
                    close(fd);
                }
                break;

            case 'd':
                if (state.nselected) {
                    int fd;
                    run_cmd(NULL, &fd, "xargs rm -rf");
                    write_selection(fd, state.firstselected);
                    close(fd);
                }
                break;

            case 'p':
                if (state.nselected) {
                    int fd;
                    run_cmd(NULL, &fd, "xargs -I {} cp {} .");
                    write_selection(fd, state.firstselected);
                    close(fd);
                }
                break;

            case 'x':
                if (state.nselected) {
                    close_term();

                    char *buf = NULL;
                    size_t bufsize = 0;
                    printf("> ");
                    fflush(stdout);
                    getline(&buf, &bufsize, stdin);

                    int fd;
                    pid_t child = run_cmd(NULL, &fd, "xargs %s", buf);
                    write_selection(fd, state.firstselected);
                    close(fd);
                    waitpid(child, NULL, 0);

                    printf("press enter to continue...");
                    fflush(stdout);
                    getline(&buf, &bufsize, stdin);
                    free(buf);

                    init_term();
                    goto redraw;
                }
                break;

            default:
                goto skip_redraw;
        }
        goto skip_redraw;
    }
done:
    close_term();
    write_selection(STDOUT_FILENO, state.firstselected);
    return;
}

int main(int argc, char *argv[])
{
    init_term();
    char path[MAX_PATH];
    if (!realpath(argc > 1 ? argv[1] : ".", path))
        err("realpath failed");
    explore(path);
done:
    return 0;
}
