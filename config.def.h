/*
    BB Configuration, Startup Commands, and Key Bindings

    User customization goes in config.h, which is created by running `make`
    (config.def.h is for keeping the defaults around, just in case)

    This file contains:
        - Global options, like which colors are used
        - Column formatting (width and title)
        - Startup commands
        - User key bindings

    For startup commands and key bindings, the following values are provided as
    environment variables:

        $@ (the list of arguments): the full paths of the selected files, or if
            no files are selected, the full path of the file under the cursor
        $BBCURSOR: the full path of the file under the cursor
        $BBSELECTED: "1" if any files are selected, otherwise ""
        $BBDOTFILES: "1" if files beginning with "." are visible in bb, otherwise ""
        $BB_DEPTH: the number of `bb` instances deep (in case you want to run a
            shell and have that shell print something special in the prompt)
        $BBCMD: a file to which `bb` commands can be written (used internally)

    In order to modify bb's internal state, you can call `bb +cmd`, where "cmd"
    is one of the following commands (or a unique prefix of one):

        .:[01]                    Whether to show "." in each directory
        ..:[01]                   Whether to show ".." in each directory
        cd:<path>                 Navigate to <path>
        columns:<columns>         Change which columns are visible, and in what order
        deselect:<filename>       Deselect <filename>
        dotfiles:[01]             Whether dotfiles are visible
        goto:<filename>           Move the cursor to <filename> (changing directory if needed)
        interleave:[01]           Whether or not directories should be interleaved with files in the display
        move:<num*>               Move the cursor a numeric amount
        quit                      Quit bb
        refresh                   Refresh the file listing
        scroll:<num*>             Scroll the view a numeric amount
        select:<filename>         Select <filename>
        sort:([+-]method)+        Set sorting method (+: normal, -: reverse), additional methods act as tiebreaker
        spread:<num*>             Spread the selection state at the cursor
        toggle:<filename>         Toggle the selection status of <filename>

    Note: for numeric-based commands (like scroll), the number can be either
        an absolute value or a relative value (starting with '+' or '-'), and/or
        a percent (ending with '%'). Scrolling and moving, '%' means percent of
        screen height, and '%n' means percent of number of files (e.g. +50% means
        half a screen height down, and 100%n means the last file)

    Internally, bb will write the commands (NUL terminated) to $BBCMD, if
    $BBCMD is set, and read the file when file browsing resumes. These commands
    can also be passed to bb at startup, and will run immediately.
    E.g. `bb '+col:n' '+sort:+r' .` will launch `bb` only showing the name column, randomly sorted.

    As a shorthand and performance optimization, commands that don't rely on any
    shell variables or scripting can be written as "+move:+1" instead of "bb '+move:+1'",
    which is a bit faster because internally it avoids writing to and reading from
    the $BBCMD file.

 */
#include "bterm.h"

// Constants:
#define MAX_REBINDINGS 8

// Types:
typedef struct {
    int keys[MAX_REBINDINGS+1];
    const char *script;
    const char *description;
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

// Some handy macros for common shell script behaviors:
#define PAUSE " read -n1 -p '\033[2mPress any key to continue...\033[0m\033[?25l' >/dev/tty </dev/tty"

// Bold text:
#define B(s) "\033[1m" s "\033[22m"

// Macro for getting user input:
#ifndef ASK
#define ASK(var, prompt, initial) "read -p \"" B(prompt) "\" " var " </dev/tty >/dev/tty"
#endif

// Macro for picking from a list of options:
#ifndef PICK
#define PICK(prompt, initial) " { "ASK("query", prompt, initial)" && awk '{print length, $1}' | sort -n | cut -d' ' -f2- | "\
    "grep -i -m1 \"$(echo \"$query\" | sed 's;.;[^/&]*[&];g')\"; } "
#endif

// Display a spinning indicator if command takes longer than 10ms:
#ifndef SPIN
#define SPIN(cmd) "{ { " cmd "; } & " \
    "pid=$!; "\
    "spinner='-\\|/'; "\
    "sleep 0.01; "\
    "while kill -0 $pid 2>/dev/null; do "\
    "    printf '%c\\033[D' \"$spinner\" >/dev/tty; "\
    "    spinner=\"$(echo $spinner | sed 's/\\(.\\)\\(.*\\)/\\2\\1/')\"; "\
    "    sleep 0.1; "\
    "done; "\
    "wait $pid; }"
#endif

#ifndef CONFIRM
#define CONFIRM(action, files) " { { echo \""B(action)"\"; printf '%s\\n' \""files"\"; } | more; " \
    ASK("REPLY", "Is that okay? [y/N] ", "")"; [ \"$REPLY\" = 'y' ]; } "
#endif

#define SECTION(name) {{0}, NULL, name}


// These commands will run at startup (before command-line arguments)
extern const char *startupcmds[];
extern const column_t columns[128];
extern binding_t bindings[];

// Column widths and titles:
const column_t columns[128] = {
    ['*'] = {2,  "*"},
    ['a'] = {21, "      Accessed"},
    ['c'] = {21, "      Created"},
    ['m'] = {21, "      Modified"},
    ['n'] = {40, "Name"},
    ['p'] = {5,  "Permissions"},
    ['r'] = {2,  "Random"},
    ['s'] = {9,  "Size"},
};

// This is a list of commands that runs when `bb` launches:
const char *startupcmds[] = {
    // Set some default marks:
    "mkdir -p ~/.config/bb/marks",
    "ln -sT ~/.config/bb/marks ~/.config/bb/marks/marks 2>/dev/null",
    "ln -sT ~ ~/.config/bb/marks/home 2>/dev/null",
    "ln -sT / ~/.config/bb/marks/root 2>/dev/null",
    "ln -sT ~/.config ~/.config/bb/marks/config 2>/dev/null",
    "ln -sT ~/.local ~/.config/bb/marks/local 2>/dev/null",
    // Default column and sorting options:
    "+sort:+n", "+col:*smpn", "+..",
    NULL, // NULL-terminated array
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
binding_t bindings[] = {
    SECTION("Key Bindings"),
    {{'?', KEY_F1}, "+help", B("Help")" menu"},
    {{'q', 'Q'}, "+quit", B("Quit")},

    SECTION("Navigation"),
    {{'j', KEY_ARROW_DOWN}, "+move:+1", B("Next")" file"},
    {{'k', KEY_ARROW_UP}, "+move:-1", B("Previous")" file"},
    {{'h', KEY_ARROW_LEFT}, "+cd:..", B("Parent")" directory"},
    {{'l', KEY_ARROW_RIGHT}, "[ -d \"$BBCURSOR\" ] && bb \"+cd:$BBCURSOR\"", B("Enter")" a directory"},
    {{KEY_CTRL_F}, "bb \"+goto:$(if [ $BBDOTFILES ]; then find -mindepth 1; else find -mindepth 1 ! -path '*/.*'; fi "
        "| "PICK("Find: ", "")")\"", B("Search")" for file"},
    {{'/'}, "bb \"+goto:$(if [ $BBDOTFILES ]; then find -mindepth 1 -maxdepth 1; else find -mindepth 1 -maxdepth 1 ! -path '*/.*'; fi "
        "| "PICK("Pick: ", "")")\"", B("Pick")" file"},
    {{KEY_CTRL_G}, ASK("goto", "Go to directory: ", "")" && bb +cd:\"$goto\"", B("Go to")" directory"},
    {{'m'}, ASK("mark", "Mark: ", "")" && ln -s \"$PWD\" ~/.config/bb/marks/\"$mark\"", B("Mark")" this directory"},
    {{'\''}, "mark=\"$(ls ~/.config/bb/marks | " PICK("Jump to: ", "") ")\" "
        "&& bb +cd:\"$(readlink -f ~/.config/bb/marks/\"$mark\")\"",
        "Go to a "B("marked")" directory"},
    {{'-', KEY_BACKSPACE, KEY_BACKSPACE2},
        "[ $BBPREVPATH ] && bb +cd:\"$BBPREVPATH\"", "Go to "B("previous")" directory"},
    {{';'}, "bb +cd:'<selection>'", "Show "B("selected files")},
    {{'0'}, "bb +cd:\"$BBINITIALPATH\"", "Go to "B("initial directory")},
    {{'g', KEY_HOME}, "+move:0", "Go to "B("first")" file"},
    {{'G', KEY_END}, "+move:100%n", "Go to "B("last")" file"},
    {{KEY_PGDN}, "+scroll:+100%", B("Page down")},
    {{KEY_PGUP}, "+scroll:-100%", B("Page up")},
    {{KEY_CTRL_D}, "+scroll:+50%", B("Half page down")},
    {{KEY_CTRL_U}, "+scroll:-50%", B("Half page up")},
    {{KEY_MOUSE_WHEEL_DOWN}, "+scroll:+3", B("Scroll down")},
    {{KEY_MOUSE_WHEEL_UP}, "+scroll:-3", B("Scroll up")},

    SECTION("File Selection"),
    {{' ','v','V'}, "+toggle", B("Toggle")" selection at cursor"},
    {{KEY_ESC}, "bb +deselect: \"$@\"", B("Clear")" selection"},
    {{KEY_CTRL_S}, ASK("savename", "Save selection as: ", "") " && printf '%s\\0' \"$@\" > ~/.config/bb/\"$savename\"",
        B("Save")" the selection"},
    {{KEY_CTRL_O}, "loadpath=\"$(find ~/.config/bb -maxdepth 1 -type f | " PICK("Load selection: ", "") ")\" "
        "&& [ -e \"$loadpath\" ] && bb +deselect:'*' "
        "&& while IFS= read -r -d $'\\0'; do bb +select:\"$REPLY\"; done < \"$loadpath\"",
        B("Open")" a saved selection"},
    {{'J'}, "+spread:+1", B("Spread")" selection down"},
    {{'K'}, "+spread:-1", B("Spread")" selection up"},
    {{KEY_CTRL_A},
        "if [ $BBDOTFILES ]; then find -mindepth 1 -maxdepth 1 -print0; "
        "else find -mindepth 1 -maxdepth 1 ! -path '*/.*' -print0; fi | bb +sel:",
        B("Select all")" files here"},

    SECTION("Actions"),
    {{'\r', KEY_MOUSE_DOUBLE_LEFT},
        "if [ -d \"$BBCURSOR\" ]; then bb \"+cd:$BBCURSOR\"; "
#ifdef __APPLE__
        "elif file -bI \"$BBCURSOR\" | grep -q '^\\(text/\\|inode/empty\\)'; then $EDITOR \"$BBCURSOR\"; "
        "else open \"$BBCURSOR\"; fi",
#else
        "elif file -bi \"$BBCURSOR\" | grep -q '^\\(text/\\|inode/empty\\)'; then $EDITOR \"$BBCURSOR\"; "
        "else xdg-open \"$BBCURSOR\"; fi",
#endif
        B("Open")" file/directory"},
    {{'e'}, "$EDITOR \"$@\" || "PAUSE, B("Edit")" file in $EDITOR"},
    {{'d', KEY_DELETE}, CONFIRM("The following will be deleted:", "$@") " && rm -rf \"$@\" && bb +refresh && bb +deselect: \"$@\"",
        B("Delete")" files"},
    {{KEY_CTRL_V}, "[ $BBSELECTED ] || exit; "
        "if " CONFIRM("The following will be moved here:", "$@") "; then "
        SPIN("mv -i \"$@\" . && bb +refresh && bb +deselect: \"$@\" && for f; do bb \"+sel:$(basename \"$f\")\"; done")" || "PAUSE
        "; fi",
        B("Move")" files here"},
    {{'c'}, CONFIRM("The following will be copied here:", "$@")
        " && for f; do if [ \"./$(basename \"$f\")\" -ef \"$f\" ]; then "
        SPIN("cp -ri \"$f\" \"$(basename \"$f\").copy\"")"; "
        "else "SPIN("cp -ri \"$f\" .")"; fi; done; bb +refresh",
        B("Copy")" the selected files here"},
    {{KEY_CTRL_N}, "type=\"$(printf '%s\\n' File Directory | "PICK("Create new: ", "")")\" "
        "&& "ASK("name", "New $type: ", "")" && "
        "{ if [ $type = File ]; then touch \"$name\"; else mkdir \"$name\"; fi "
        "&& bb \"+goto:$name\" +r || "PAUSE"; }", B("New")" file/directory"},
    {{'p'}, "$PAGER \"$@\"", B("Page")" through a file in $PAGER"},
    {{'|'}, ASK("cmd", "|", "") " && printf '%s\\n' \"$@\" | sh -c \"$BBSHELLFUNC$cmd\"; " PAUSE "; bb +r",
        B("Pipe")" selected files to a command"},
    {{':'}, ASK("cmd", ":", "")" && sh -c \"$BBSHELLFUNC$cmd\" -- \"$@\"; " PAUSE "; bb +refresh",
        B("Run")" a command"},
    {{'>'}, "tput rmcup >/dev/tty; $SHELL; bb +r", "Open a "B("shell")},
    {{'r', KEY_F2},
        "bb +refresh; "
        "for f; do "
        "    "ASK("newname", "Rename: ", "$(basename \"$f\")")" || break; "
        "    if r=\"$(dirname \"$f\")/$newname\"; then "
        "      if [ \"$r\" != \"$f\" ] && mv -i \"$f\" \"$r\"; then "
        "          [ $BBSELECTED ] && bb \"+deselect:$f\" \"+select:$r\"; "
        "      fi; "
        "    else break; "
        "    fi; "
        "done", B("Rename")" files"},
    {{'R'},
        "if "ASK("patt", "Rename pattern: ", "s/")"; then true; else exit; fi; "
        "if sed -E \"$patt\" </dev/null; then true; else " PAUSE "; exit; fi; "
        "bb +refresh; "
        "for f; do "
        "    renamed=\"$(dirname \"$f\")/$(basename \"$f\" | sed -E \"$patt\")\"; "
        "    if [ \"$f\" != \"$renamed\" ] && mv -i \"$f\" \"$renamed\" && [ $BBSELECTED ]; then "
        "        bb \"+deselect:$f\" \"+select:$renamed\"; "
        "    fi;"
        "done", B("Regex rename")" files"},

    SECTION("File Display"),
    {{'s'},
        "read -n1 -p \"" B("Sort (n)ame (s)ize (m)odification (c)reation (a)ccess (r)andom (p)ermissions: ") "\" sort"
        " && bb \"+sort:+$sort\" +refresh",
        B("Sort")" by..."},
    {{'#'}, ASK("columns", "Set columns (*)selected (a)ccessed (c)reated (m)odified (n)ame (p)ermissions (r)andom (s)ize: ", "")
        " && bb +col:\"$columns\"",
        "Set "B("columns")},
    {{'.'}, "+dotfiles", "Toggle "B("dotfile")" visibility"},
    {{KEY_F5, KEY_CTRL_R}, "+refresh", B("Refresh")},

    {{-1}} // Array must be -1-terminated
};

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
