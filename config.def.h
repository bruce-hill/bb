/*
    BB Key Bindings

    User-defined key bindings go in config.h, which is created by running `make`
    (config.def.h is for keeping the defaults around, just in case)

    The basic structure is:
        <list of keys to bind>
        <program to run>
        <description> (for the help menu)
        <flags> (whether to run in a full terminal window or silently, etc.)

    When the scripts are run, the following values are provided as environment variables:

        $@ (the list of arguments): the full paths of the selected files
        $BBCURSOR: the full name of the file under the cursor
        $BB_DEPTH: the number of `bb` instances deep (in case you want to run a
            shell and have that shell print something special in the prompt)
        $BBCMD: a file to which `bb` commands can be written (used internally)

    In order to modify bb's internal state, you can call `bb +cmd`, where "cmd"
    is one of the following commands (or a unique prefix of one):

        .:[01]                    Whether to show "." in each directory
        ..:[01]                   Whether to show ".." in each directory
        cd:<path>                 Navigate to <path>
        columns:<columns>         Change which columns are visible, and in what order
        deselect:<filename>|*     Deselect <filename> or all ("*")
        dotfiles:[01]             Whether dotfiles are visible
        goto:<filename>           Move the cursor to <filename> (changing directory if needed)
        interleave:[01]           Whether or not directories should be interleaved with files in the display
        jump:<key>                Jump to the mark associated with <key>
        mark:<key>[=<path>]       Associate <key> with <path> (or current dir, if blank)
        move:<num*>               Move the cursor a numeric amount
        quit                      Quit bb
        refresh                   Refresh the file listing
        scroll:<num*>             Scroll the view a numeric amount
        select:<filename>|*       Select <filename> or all visible files in current directory ("*")
        sort:([+-]method)+        List of sortings (if equal on one, move to the next)
        spread:<num*>             Spread the selection state at the cursor
        toggle:<filename>         Toggle the selection status of <filename>

    Internally, bb will write the commands (NUL terminated) to $BBCMD, if
    $BBCMD is set, and read the file when file browsing resumes. These commands
    can also be passed to bb at startup, and will run immediately.
    E.g. `bb +col:n +sort:r .` will launch `bb` only showing the name column, randomly sorted.

    *Note: for numeric-based commands (like scroll), the number can be either
        an absolute value or a relative value (starting with '+' or '-'), and/or
        a percent (ending with '%'). Scrolling and moving, '%' means percent of
        screen height, and '%n' means percent of number of files (e.g. +50% means
        half a screen height down, and 100%n means the last file)

 */
#include "bterm.h"

// Configurable options:
#define KEY_DELAY 50
#define SCROLLOFF MIN(5, (termheight-4)/2)

#define CMDFILE_FORMAT "/tmp/bb.XXXXXX"

#define SORT_INDICATOR  "↓"
#define RSORT_INDICATOR "↑"
#define SELECTED_INDICATOR " \033[31;7m \033[0m"
#define NOT_SELECTED_INDICATOR "  "

#define TITLE_COLOR      "\033[37;1m"
#define NORMAL_COLOR     "\033[37m"
#define CURSOR_COLOR     "\033[43;30;1m"
#define LINK_COLOR       "\033[35m"
#define DIR_COLOR        "\033[34m"
#define EXECUTABLE_COLOR "\033[31m"

#define PAUSE " read -n1 -p '\033[2mPress any key to continue...\033[0m\033[?25l'"

#ifndef ASK
#ifdef ASKECHO
#define ASK(var, prompt, initial) var "=\"$(" ASKECHO(prompt, initial) ")\""
#else
#define ASK(var, prompt, initial) "read -p \"" prompt "\" " var
#endif
#endif

#ifndef ASKECHO
#define ASKECHO(prompt, initial) ASK("REPLY", prompt, initial) " && echo \"$REPLY\""
#endif

#ifndef PICK
#define PICK(prompt, initial) "true && " ASKECHO(prompt, initial)
#endif

#define NORMAL_TERM     (1<<0)

#define MAX_REBINDINGS 8

typedef struct {
    int keys[MAX_REBINDINGS+1];
    const char *command;
    const char *description;
    int flags;
} binding_t;

typedef struct {
    int width;
    const char *name;
} column_t;

// These commands will run at startup (before command-line arguments)
extern const char *startupcmds[];
extern const column_t columns[128];

const char *startupcmds[] = {
    //////////////////////////////////////////////
    // User-defined startup commands can go here
    //////////////////////////////////////////////
    // Set some default marks:
    "+mark:0", "+mark:~=~", "+mark:h=~", "+mark:/=/", "+mark:c=~/.config",
    "+mark:l=~/.local", "+mark:s=<selection>",
    // Default column and sorting options:
    "+sort:+n", "+col:*smpn", "+..",
    NULL, // NULL-terminated array
};

// Column widths:
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

extern binding_t bindings[];
#define B(s) "\033[1m" s "\033[22m"
binding_t bindings[] = {
    /*************************************************************************
     * User-defined custom scripts can go here
     * Format is: {{keys}, "script", "help text", flags}
     *
     * Please note that these are sh scripts, not bash scripts, so bash-isms
     * won't work unless you make your script use `bash -c "<your script>"`
     ************************************************************************/
    {{'?', KEY_F1}, "bb -b | $PAGER -r", B("Help")" menu", NORMAL_TERM},
    {{'q', 'Q'}, "+quit", B("Quit")},
    {{'j', KEY_ARROW_DOWN}, "+move:+1", B("Next")" file"},
    {{'k', KEY_ARROW_UP}, "+move:-1", B("Previous")" file"},
    {{'h', KEY_ARROW_LEFT}, "+cd:..", B("Parent")" directory"},
    {{'l', KEY_ARROW_RIGHT}, "test -d \"$BBCURSOR\" && bb \"+cd:$BBCURSOR\"", B("Enter")" a directory"},
    {{'\r', KEY_MOUSE_DOUBLE_LEFT},
#ifdef __APPLE__
        "if test -d \"$BBCURSOR\"; then bb \"+cd:$BBCURSOR\"; "
        "elif test -x \"$BBCURSOR\"; then \"$BBCURSOR\"; " PAUSE "; "
        "elif file -bI \"$BBCURSOR\" | grep '^\\(text/\\|inode/empty\\)' >/dev/null; then $EDITOR \"$BBCURSOR\"; "
        "else open \"$BBCURSOR\"; fi",
#else
        "if test -d \"$BBCURSOR\"; then bb \"+cd:$BBCURSOR\"; "
        "elif file -bi \"$BBCURSOR\" | grep '^\\(text/\\|inode/empty\\)' >/dev/null; then $EDITOR \"$BBCURSOR\"; "
        "else xdg-open \"$BBCURSOR\"; fi",
#endif
        B("Open")" file/directory", NORMAL_TERM},
    {{' ','v','V'}, "+toggle", B("Toggle")" selection"},
    {{KEY_ESC}, "+deselect:*", B("Clear")" selection"},
    {{'e'}, "$EDITOR \"$@\" || "PAUSE, B("Edit")" file in $EDITOR", NORMAL_TERM},
    {{KEY_CTRL_F}, "bb \"+g:`find | "PICK("Find: ", "")"`\"", B("Search")" for file"},
    {{'/'}, "bb \"+g:`ls -a | "PICK("Pick: ", "")"`\"", B("Pick")" file"},
    {{'d', KEY_DELETE}, "rm -rfi \"$@\" && bb '+deselect:*' +r ||" PAUSE, B("Delete")" files"},
    {{'D'}, "rm -rf \"$@\" && bb '+deselect:*' +r ||" PAUSE, B("Delete")" files (without confirmation)"},
    {{'M'}, "mv -i \"$@\" . && bb '+deselect:*' +r && for f; do bb \"+sel:`pwd`/`basename \"$f\"`\"; done || "PAUSE,
        B("Move")" files to current directory"},
    {{'c'}, "cp -ri \"$@\" . && bb +r || "PAUSE, B("Copy")" files to current directory"},
    {{'C'}, "bb '+de:*' && for f; do cp \"$f\" \"$f.copy\" && bb \"+sel:$f.copy\"; done && bb +r || "PAUSE, B("Clone")" files"},
    {{'n'}, ASK("name", "New file: ", "")" && touch \"$name\" && bb \"+goto:$name\" +r || "PAUSE, B("New file")},
    {{'N'}, ASK("name", "New dir: ", "")" && mkdir \"$name\" && bb \"+goto:$name\" +r || "PAUSE, B("New directory")},
    {{KEY_CTRL_G}, "bb \"+cd:`" ASKECHO("Go to directory: ", "") "`\"", B("Go to")" directory"},
    {{'|'}, ASK("cmd", "|", "") " && printf '%s\\n' \"$@\" | sh -c \"$cmd\"; " PAUSE "; bb +r",
        B("Pipe")" selected files to a command"},
    {{':'}, "sh -c \"`" ASKECHO(":", "") "`\" -- \"$@\"; " PAUSE "; bb +refresh",
        B("Run")" a command"},
    {{'>'}, "$SHELL; bb +r", "Open a "B("shell"), NORMAL_TERM},
    {{'m'}, "read -n1 -p 'Mark: ' m && bb \"+mark:$m;$PWD\"", "Set "B("mark")},
    {{'\''}, "read -n1 -p 'Jump: ' j && bb \"+jump:$j\"", B("Jump")" to mark"},
    {{'r'},
        "bb +refresh; "
        "for f; do "
        "    if r=\"$(dirname \"$f\")/$("ASKECHO("rename: ", "$(basename \"$f\")")")\"; then "
        "      if test \"$r\" != \"$f\" && mv -i \"$f\" \"$r\"; then "
        "          test $BBSELECTED && bb \"+deselect:$f\" \"+select:$r\"; "
        "      fi; "
        "    else break; "
        "    fi; "
        "done", B("Rename")" files"},
    {{'R'},
        "if "ASK("patt", "Rename pattern: ", "s/")"; then true; else exit; fi; "
        "if sed -E \"$patt\" </dev/null; then true; else " PAUSE "; exit; fi; "
        "bb +refresh; "
        "for f; do "
        "    renamed=\"`dirname \"$f\"`/`basename \"$f\" | sed -E \"$patt\"`\" || "PAUSE" && exit; "
        "    if test \"$f\" != \"$renamed\" && mv -i \"$f\" \"$renamed\"; then "
        "        test $BBSELECTED && bb \"+deselect:$f\" \"+select:$renamed\"; "
        "    fi; "
        "done", B("Regex rename")" files"},
    {{'P'},
        "patt=`ask 'Select pattern: '` && "
        "for f in *; do echo \"$f\" | grep \"$patt\" >/dev/null && bb \"+sel:$f\"; done",
        B("Regex select")" files"},
    {{'J'}, "+spread:+1", B("Spread")" selection down"},
    {{'K'}, "+spread:-1", B("Spread")" selection up"},
    {{'b'}, "bb \"+`"ASKECHO("bb +", "")"`\"", "Run a "B("bb command")},
    {{'s'},
        ("sort=\"$(printf '%s\\n' n s m c a r p | "
         PICK("Sort (n)ame (s)ize (m)odification (c)reation (a)ccess (r)andom (p)ermissions: ", "") ")\" "
         "&& bb \"+sort:+$sort\" +refresh"),
        B("Sort")" by..."},
    {{'#'}, "bb \"+col:`"ASKECHO("Set columns: ", "")"`\"", "Set "B("columns")},
    {{'.'}, "bb +dotfiles", "Toggle "B("dotfiles")},
    {{'g', KEY_HOME}, "+move:0", "Go to "B("first")" file"},
    {{'G', KEY_END}, "+move:100%n", "Go to "B("last")" file"},
    {{KEY_F5, KEY_CTRL_R}, "+refresh", B("Refresh")},
    {{KEY_CTRL_A}, "+select:*", B("Select all")" files in current directory"},
    {{KEY_PGDN}, "+scroll:+100%", B("Page down")},
    {{KEY_PGUP}, "+scroll:-100%", B("Page up")},
    {{KEY_CTRL_D}, "+scroll:+50%", B("Half page down")},
    {{KEY_CTRL_U}, "+scroll:-50%", B("Half page up")},
    {{KEY_MOUSE_WHEEL_DOWN}, "+scroll:+3", B("Scroll down")},
    {{KEY_MOUSE_WHEEL_UP}, "+scroll:-3", B("Scroll up")},
    {{0}}, // Array must be 0-terminated
};

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
