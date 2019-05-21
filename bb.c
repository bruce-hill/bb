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

static const int SCROLLOFF = 5;

static struct termios orig_termios;
static int termfd;
static int width, height;
static int mouse_x, mouse_y;

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
    writez(termfd, "\033[?1049h");
    update_term_size();
    // Initiate mouse tracking:
    writez(termfd, "\e[?1000h\e[?1002h\e[?1015h\e[?1006h");
}

static void close_term()
{
    // xterm-specific:
    writez(termfd, "\033[?1049l");
    tcsetattr(termfd, TCSAFLUSH, &orig_termios);
    close(termfd);
}

static void err(const char *msg, ...)
{
    close_term();
    va_list args;
    va_start(args, msg);
    fprintf(stderr, msg, args);
    va_end(args);
    if (errno)
        fprintf(stderr, "\n%s", strerror(errno));
    fprintf(stderr, "\n");
    _exit(1);
}

static pid_t run_cmd(int *readable_fd, int *writable_fd, const char *cmd, ...)
{
    int fd[2];
    pid_t child;
    pipe(fd);
    if ((child = fork())) {
        if (child == -1)
            err("Failed to fork");
        if (writable_fd) *writable_fd = fd[0];
        else close(fd[0]);
        if (readable_fd) *readable_fd = fd[1];
        else close(fd[1]);
    } else {
        close(fd[0]);
        if (writable_fd)
            dup2(fd[1], STDIN_FILENO);
        if (readable_fd)
            dup2(fd[0], STDOUT_FILENO);
        char *formatted_cmd;
        va_list args;
        va_start(args, cmd);
        vasprintf(&formatted_cmd, cmd, args);
        va_end(args);
        if (formatted_cmd)
            execlp("sh", "sh", "-c", formatted_cmd);
        err("Failed to execute command: %s", formatted_cmd);
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

static int term_write(const char *str, ...)
{
    char buf[1024] = {0};
    va_list args;
    va_start(args, str);
    int len = snprintf(buf, sizeof(buf), str, args);
    va_end(args);
    if (len > 0)
        write(termfd, buf, len);
    return len;

    /*
    char *formatted = NULL;
    va_list args;
    va_start(args, str);
    int ret = asprintf(&formatted, str, args);
    va_end(args);
    if (!formatted)
        err("failed to allocate for string format");

    write(termfd, formatted, ret);
    free(formatted);
    return ret;
    */
}

typedef struct {
    struct dirent entry;
    const char *path;
    int selected : 1;
} entry_t;

static void render(const char *path, entry_t *files, size_t nfiles, int cursor, int scroll, size_t nselected)
{
    writez(termfd, "\e[2J\e[0;1m"); // Clear, reset color + bold
    term_move(0,0);
    writez(termfd, path);
    writez(termfd, "\e[0m"); // Reset color

    char fullpath[MAX_PATH];
    size_t pathlen = strlen(path);
    strncpy(fullpath, path, pathlen + 1);
    for (int i = scroll; i < scroll + height - 3 && i < nfiles; i++) {
        int x = 0;
        int y = i - scroll + 1;
        term_move(x, y);

        // Selection box:
        if (files[i].selected)
            writez(termfd, "\e[43m \e[0m"); // Yellow BG
        else
            writez(termfd, " ");

        if (i != cursor && files[i].entry.d_type & DT_DIR) {
            writez(termfd, "\e[34m"); // Blue FG
        }
        if (i == cursor) {
            writez(termfd, "\e[7m"); // Reverse color
        }

        // Filesize:
        struct stat info;
        fullpath[pathlen] = '/';
        strncpy(fullpath + pathlen + 1, files[i].entry.d_name, files[i].entry.d_namlen);
        fullpath[pathlen + 1 + files[i].entry.d_namlen] = '\0';
        lstat(fullpath, &info);

        int j = 0;
        const char* units[] = {"B", "K", "M", "G", "T", "P", "E", "Z", "Y"};
        int bytes = info.st_size;
        while (bytes > 1024) {
            bytes /= 1024;
            j++;
        }
        //term_write("%10.*f%s  ", j, bytes, units[j]);

        // Date:
        char buf[64];
        strftime(buf, sizeof(buf), "%l:%M%p %b %e %Y", localtime(&(info.st_mtime)));
        writez(termfd, buf);
        writez(termfd, "  ");

        // Name:
        write(termfd, files[i].entry.d_name, files[i].entry.d_namlen);

        if (files[i].entry.d_type & DT_DIR) {
            writez(termfd, "/");
        }
        if (files[i].entry.d_type == DT_LNK) {
            char linkpath[MAX_PATH] = {0};
            ssize_t pathlen;
            if ((pathlen = readlink(files[i].entry.d_name, linkpath, sizeof(linkpath))) < 0)
                err("readlink() failed");
            writez(termfd, "\e[36m -> "); // Cyan FG
            write(termfd, linkpath, pathlen);
        }
        writez(termfd, "\e[0m"); // Reset color and attributes
    }

    term_move(0, height - 1);
    char buf[32] = {0};
    int len = snprintf(buf, sizeof(buf), "%lu selected", nselected);
    write(termfd, buf, len);
}

static int compare_alpha(const void *v1, const void *v2)
{
    const entry_t *f1 = (const entry_t*)v1, *f2 = (const entry_t*)v2;
    int diff;
    diff = (f1->entry.d_type & DT_DIR) - (f2->entry.d_type & DT_DIR);
    if (diff) return -diff;
    const char *p1 = f1->entry.d_name, *p2 = f2->entry.d_name;
    while (*p1 && *p2) {
        int diff = (*p1 - *p2);
        if ('0' <= *p1 && *p1 <= '9' && '0' <= *p2 && *p2 <= '9') {
            long n1 = strtol(p1, (char**)&p1, 10);
            long n2 = strtol(p2, (char**)&p2, 10);
            diff = ((p1 - f1->entry.d_name) - (p2 - f2->entry.d_name)) || (n1 - n2);
            if (diff) return diff;
        } else if (diff) {
            return diff;
        } else {
            ++p1, ++p2;
        }
    }
    return *p1 - *p2;
}

static void write_selection(int fd, entry_t *selected, size_t nselected, size_t ndeselected)
{
    for (int i = 0; i < nselected + ndeselected; i++) {
        if (!selected[i].selected)
            continue;

        const char *p = selected[i].path;
        while (*p) {
            const char *p2 = strchr(p, '\n');
            if (!p2) p2 = p + strlen(p) + 1;
            write(fd, p, p2 - p);
            if (*p2 == '\n')
                write(fd, "\\", 1);
            p = p2;
        }

        write(fd, "/", 1);

        p = selected[i].entry.d_name;
        while (*p) {
            const char *p2 = strchr(p, '\n');
            if (!p2) p2 = p + strlen(p) + 1;
            write(fd, p, p2 - p);
            if (*p2 == '\n')
                write(fd, "\\", 1);
            p = p2;
        }
        write(fd, "\n", 1);
    }
}

static int term_get_event()
{
    char c;
    if (read(termfd, &c, 1) != 1)
        return -1;
    switch (c) {
        case '\x1b':
            if (read(termfd, &c, 1) != 1)
                return -1;
            switch (c) {
                case '[':
                    if (read(termfd, &c, 1) != 1)
                        return -1;
                    switch (c) {
                        case 'H': return KEY_HOME;
                        case 'F': return KEY_END;
                        case 'M':
                            {
                                char buf[7] = {0};
                                if (read(termfd, buf, 6) != 6)
                                    return -1;
                                unsigned char buttons, x, y;
                                if (sscanf(buf, "%c%c%c", &buttons, &x, &y) != 3)
                                    return -1;

                                mouse_x = (int)x - 32, mouse_y = (int)y - 32;
                                switch (buttons) {
                                    case 0: return KEY_MOUSE_LEFT;
                                    case 1: return KEY_MOUSE_RIGHT;
                                    case 2: return KEY_MOUSE_MIDDLE;
                                    case 3: return KEY_MOUSE_RELEASE;
                                    default: return -1;
                                }
                            }
                            break;
                        default:
                            break;
                    }
                    return '\x1b';
                default:
                    return '\x1b';
            }
            break;
        default:
            return c;
    }
    return -1;
}

static void explore(const char *path)
{
    static entry_t *selected;
    static size_t nselected = 0, ndeselected = 0, selectedcapacity = 0;
    char to_select[MAX_PATH] = {0};
    char _path[MAX_PATH];
  tail_call:;
    DIR *dir = opendir(path);
    if (!dir)
        err("Couldn't open dir: %s", path);
    if (chdir(path) != 0)
        err("Couldn't chdir into %s", path);
    struct dirent *dp;

    size_t filecap = 0, nfiles = 0;
    entry_t *files = NULL;

    // Hash inode -> inode with linear probing
    size_t hashsize = 2 * nselected;
    ino_t *selecthash = calloc(hashsize, sizeof(ino_t));
    if (!selecthash)
        err("Failed to allocate %d spaces for selecthash", hashsize);
    for (int i = nselected + ndeselected - 1; i >= 0; i--) {
        if (!selected[i].selected) continue;
        ino_t inode = selected[i].entry.d_ino;
        int probe = ((int)inode) % hashsize;
        while (selecthash[probe])
            probe = (probe + 1) % hashsize;
        selecthash[probe] = inode;
    }

    while ((dp = readdir(dir)) != NULL) {
        if (dp->d_name[0] == '.' && dp->d_name[1] == '\0')
            continue;
        if (nfiles >= filecap) {
            filecap += 100;
            if ((files = realloc(files, sizeof(entry_t)*filecap)) == NULL)
                err("Alloc fail");
        }
        int selected = 0;
        if (nselected) {
            for (int probe = ((int)dp->d_ino) % hashsize; selecthash[probe]; probe = (probe + 1) % hashsize) {
                if (selecthash[probe] == dp->d_ino) {
                    selected = 1;
                    break;
                }
            }
        }
        entry_t file = {*dp, path, selected};
        files[nfiles++] = file;
    }
    free(selecthash);
    if (nfiles == 0) {
        err("No files found (not even '..')");
    }
    qsort(files, nfiles, sizeof(entry_t), compare_alpha);
    closedir(dir);

    int cursor = 0;
    int scroll = 0;

    if (to_select[0]) {
        for (int i = 0; i < nfiles; i++) {
            if (strcmp(to_select, files[i].entry.d_name) == 0) {
                cursor = i;
                break;
            }
        }
    }

    struct timespec lastclick;
    clock_gettime(CLOCK_MONOTONIC, &lastclick);
    int picked, scrolloff;

    while (1) {
      redraw:
        render(path, files, nfiles, cursor, scroll, nselected);
      skip_redraw:
        scrolloff = MIN(SCROLLOFF, height/2);
        //sleep(2);
        //if (1) goto done;
        switch (term_get_event()) {
            case KEY_MOUSE_LEFT: {
                struct timespec clicktime;
                clock_gettime(CLOCK_MONOTONIC, &clicktime);
                double dt_ms = 1e3*(double)(clicktime.tv_sec - lastclick.tv_sec);
                dt_ms += 1e-6*(double)(clicktime.tv_nsec - lastclick.tv_nsec);
                lastclick = clicktime;
                if (mouse_y > 0 && scroll + (mouse_y - 1) < nfiles) {
                    int clicked = scroll + (mouse_y - 1);
                    if (dt_ms > 200) {
                    // Single click
                        if (mouse_x == 0) {
                            // Toggle
                            picked = clicked;
                            goto toggle;
                        } else {
                            cursor = clicked;
                        }
                    } else {
                        // Double click
                        picked = clicked;
                        goto open_file;
                    }
                }
            }
            case KEY_ESC: case 'q': case 'Q':
                goto done;
            case KEY_CTRL_D:
                cursor = MIN(nfiles - 1, cursor + (height - 3) / 2);
                if (nfiles <= height - 3)
                    goto redraw;
                scroll += (height - 3)/2;
                if (scroll > nfiles - (height - 3))
                    scroll = nfiles - (height - 3);
                goto redraw;
            case KEY_CTRL_U:
                cursor = MAX(0, cursor - (height - 3) / 2);
                if (nfiles <= height - 3)
                    goto redraw;
                scroll -= (height - 3)/2;
                if (scroll < 0)
                    scroll = 0;
                goto redraw;
            case ' ': case '\r':
                picked = cursor;
              toggle:
                files[picked].selected ^= 1;
                if (files[picked].selected) {
                    if (nselected + ndeselected + 1 > selectedcapacity) {
                        selectedcapacity += 100;
                        selected = realloc(selected, selectedcapacity);
                    }
                    selected[nselected++] = files[picked];
                } else {
                    // Find and destroy
                    for (int i = nselected + ndeselected - 1; i >= 0; i--) {
                        if (!selected[i].selected) continue;
                        if (selected[i].entry.d_ino == files[picked].entry.d_ino) {
                            selected[i].selected = 0;
                            --nselected;
                            // Leave a hole to clean up later
                            if (i == nselected + ndeselected) {
                                goto redraw;
                            }
                            ++ndeselected;
                            goto found_it;
                        }
                    }
                    err("Didn't find selection");
                  found_it:
                    // Coalesce removals:
                    if (selectedcapacity > nselected + 100) {
                        entry_t *first = &selected[0];
                        entry_t *last = &selected[nselected + ndeselected - 1];
                        entry_t *p = first;
                        while (first != last) {
                            if (first->selected) {
                                *p = *first;
                                ++p;
                            }
                            ++first;
                        }
                        ndeselected = 0;

                        selectedcapacity = nselected + 100;
                        selected = realloc(selected, selectedcapacity);
                    }
                }
                goto redraw;
            case 'j':
                if (cursor >= nfiles - 1)
                    goto skip_redraw;
                ++cursor;
                if (cursor > scroll + height - 4 - scrolloff && scroll < nfiles - (height - 3)) {
                    ++scroll;
                }
                goto redraw;
            case 'k':
                if (cursor <= 0)
                    goto skip_redraw;
                --cursor;
                if (cursor < scroll + scrolloff && scroll > 0) {
                    --scroll;
                }
                goto redraw;
            case 'J':
                if (cursor < nfiles - 1) {
                    ++cursor;
                    files[cursor].selected = files[cursor - 1].selected;
                }
                goto redraw;
            case 'K':
                if (cursor > 0) {
                    --cursor;
                    files[cursor].selected = files[cursor + 1].selected;
                }
                goto redraw;
            case 'h':
                if (strcmp(path, "/") != 0) {
                    char *p = strrchr(path, '/');
                    if (p) strcpy(to_select, p+1);
                    else to_select[0] = '\0';
                    char tmp[MAX_PATH];
                    strcpy(tmp, path);
                    strcat(tmp, "/");
                    strcat(tmp, "..");
                    if (!realpath(tmp, _path))
                        err("realpath failed");
                    path = _path;
                    goto tail_call;
                }
                break;
            case 'l':
                picked = cursor;
              open_file:
                {
                    int is_dir = files[picked].entry.d_type & DT_DIR;
                    if (files[picked].entry.d_type == DT_LNK) {
                        char linkpath[MAX_PATH];
                        if (readlink(files[picked].entry.d_name, linkpath, sizeof(linkpath)) < 0)
                            err("readlink() failed");
                        DIR *dir = opendir(linkpath);
                        if (dir) {
                            is_dir = 1;
                            if (closedir(dir) < 0)
                                err("Failed to close directory: %s", linkpath);
                        }
                    }
                    if (is_dir) {
                        if (strcmp(files[picked].entry.d_name, "..") == 0) {
                            char *p = strrchr(path, '/');
                            if (p) strcpy(to_select, p+1);
                            else to_select[0] = '\0';
                        } else to_select[0] = '\0';
                        char tmp[MAX_PATH];
                        strcpy(tmp, path);
                        strcat(tmp, "/");
                        strcat(tmp, files[picked].entry.d_name);
                        if (!realpath(tmp, _path))
                            err("realpath failed");
                        path = _path;
                        goto tail_call;
                    } else {
                        char *name = files[picked].entry.d_name;
                        close_term();
                        pid_t child = run_cmd(NULL, NULL,
#ifdef __APPLE__
                                "if file -bI %s | grep '^text/'; then $EDITOR %s; else open %s; fi",
#else
                                "if file -bi %s | grep '^text/'; then $EDITOR %s; else xdg-open %s; fi",
#endif
                                name, name, name);
                        waitpid(child, NULL, 0);
                        init_term();
                        goto redraw;
                    }
                    break;
                }
            case 'm':
                if (nselected) {
                    int fd;
                    run_cmd(NULL, &fd, "xargs mv");
                    write_selection(fd, selected, nselected, ndeselected);
                    close(fd);
                }
                break;
            case 'd':
                if (nselected) {
                    int fd;
                    run_cmd(NULL, &fd, "xargs rm -rf");
                    write_selection(fd, selected, nselected, ndeselected);
                    close(fd);
                }
                break;
            case 'p':
                if (nselected) {
                    int fd;
                    run_cmd(NULL, &fd, "xargs cp");
                    write_selection(fd, selected, nselected, ndeselected);
                    close(fd);
                }
                break;
            default:
                goto skip_redraw;
        }
        goto skip_redraw;
    }
done:
    close_term();
    write_selection(STDOUT_FILENO, selected, nselected, ndeselected);
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
