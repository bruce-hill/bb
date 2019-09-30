/*
    BB Configuration, Startup Commands, and Key Bindings

    User customization goes in config.h, which is created by running `make`
    (config.def.h is for keeping the defaults around, just in case)

    This file contains:
        - Global options, like which colors are used
        - Column formatting (width and title)

 */
#include "bterm.h"

// Types:
typedef struct {
    int key;
    char *script;
    char *description;
} binding_t;

typedef struct {
    int width;
    const char *name;
} column_t;

// Configurable options:
#define SCROLLOFF   MIN(5, (termheight-4)/2)
#define CMDFILE_FORMAT "/tmp/bb.XXXXXX"
#define SORT_INDICATOR  "↓"
#define RSORT_INDICATOR "↑"
#define SELECTED_INDICATOR " \033[31;7m \033[0m"
#define NOT_SELECTED_INDICATOR "  "
// Colors (using ANSI escape sequences):
#define TITLE_COLOR      "\033[37;1m"
#define NORMAL_COLOR     "\033[37m"
#define CURSOR_COLOR     "\033[43;30;1m"
#define LINK_COLOR       "\033[35m"
#define DIR_COLOR        "\033[34m"
#define EXECUTABLE_COLOR "\033[31m"

#ifndef SH
#define SH "sh"
#endif

// These commands will run at startup (before command-line arguments)
extern const column_t columns[128];
extern binding_t bindings[1024];

// Column widths and titles:
const column_t columns[128] = {
    ['*'] = {2,  "*"},
    ['a'] = {21, "      Accessed"},
    ['c'] = {21, "      Created"},
    ['m'] = {21, "      Modified"},
    ['n'] = {40, "Name"},
    ['p'] = {5,  "Permissions"},
    ['r'] = {2,  "Random"},
    ['s'] = {9,  "  Size"},
};

/******************************************************************************
 * These are all the key bindings for bb.
 * The format is: {{keys,...}, "<script>", "<description>"}
 *
 * Please note that these are sh scripts, not bash scripts, so bash-isms
 * won't work unless you make your script use `bash -c "<your bash script>"`
 *
 * If your editor is vim (and not neovim), you can replace `$EDITOR` below with
 * `vim -c 'set t_ti= t_te=' "$@"` to prevent momentarily seeing the shell
 * after editing.
 *****************************************************************************/
binding_t bindings[1024];

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
