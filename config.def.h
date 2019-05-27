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
        $BBCURSOR: the (short) name of the file under the cursor
        $BBFULLCURSOR: the full path name of the file under the cursor
        $BB_DEPTH: the number of `bb` instances deep (in case you want to run a
            shell and have that shell print something special in the prompt)
        $BBCMD: a file to which `bb` commands can be written (used internally)

    In order to modify bb's internal state, you can call `bb +cmd`, where "cmd"
    is one of the following commands (or a unique prefix of one):

        cd:<path>                Navigate to <path>
        columns:<columns>        Change which columns are visible, and in what order
        deselect:<filename>      Deselect <filename>
        dots[:yes|:no]           Toggle whether dotfiles are visible
        goto:<filename>          Move the cursor to <filename> (changing directory if needed)
        jump:<key>               Jump to the mark associated with <key>
        mark:<key>[=<path>]      Associate <key> with <path> (or current dir, if blank)
        move:<num*>              Move the cursor a numeric amount
        quit                     Quit bb
        refresh                  Refresh the file listing
        scroll:<num*>            Scroll the view a numeric amount
        select:<filename>        Select <filename>
        sort:<method>            Change the sorting method (uppercase means reverse)
        spread:<num*>            Spread the selection state at the cursor
        toggle:<filename>        Toggle the selection status of <filename>

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
#include "keys.h"

// Configurable options:
#define KEY_DELAY 50
#define SCROLLOFF MIN(5, (termheight-4)/2)

#define CMDFILE_FORMAT "/tmp/bb.XXXXXX"

#define SORT_INDICATOR  "↓"
#define RSORT_INDICATOR "↑"

#define NORMAL_COLOR  "\033[0m"
#define CURSOR_COLOR  "\033[0;30;43m"
#define LINKDIR_COLOR "\033[0;36m"
#define DIR_COLOR     "\033[0;34m"
#define LINK_COLOR    "\033[0;33m"

#define PIPE_SELECTION_TO " printf '%s\\n' \"$@\" | "
#define PAUSE " read -n1 -p '\033[2m...press any key to continue...\033[0m\033[?25l'"

#define NORMAL_TERM     (1<<0)
#define SHOW_CURSOR     (1<<1)

#define MAX_REBINDINGS 8

typedef struct {
    int keys[MAX_REBINDINGS+1];
    const char *command;
    const char *description;
    int flags;
} binding_t;

// These commands will run at startup (before command-line arguments)
extern const char *startupcmds[];
const char *startupcmds[] = {
    //////////////////////////////////////////////
    // User-defined startup commands can go here
    //////////////////////////////////////////////
    // Set some default marks:
    "+mark:0", "+mark:~=~", "+mark:h=~", "+mark:/=/", "+mark:c=~/.config",
    "+mark:l=~/.local",
    // Default column and sorting options:
    "+columns:smpn", "+sort:n",
    NULL, // NULL-terminated array
};

extern binding_t bindings[];
binding_t bindings[] = {
    //////////////////////////////////////////////////////////////////////////
    // User-defined custom scripts can go here
    // Please note that these are sh scripts, not bash scripts, so bash-isms
    // won't work unless you make your script use `bash -c "<your script>"`
    //////////////////////////////////////////////////////////////////////////
    {{'?'},                  "bb -b | less -r", "Show the help menu", NORMAL_TERM},
    {{KEY_CTRL_H},           "<placeholder>", "Figure out what key does"},
    {{'q', 'Q'},             "+quit", "Quit"},
    {{'k', KEY_ARROW_UP},    "+move:-1", "Move up"},
    {{'j', KEY_ARROW_DOWN},  "+move:+1", "Move down"},
    {{'h', KEY_ARROW_LEFT},  "+cd:..", "Go up a folder"},
    {{'l', KEY_ARROW_RIGHT}, "test -d \"$BBCURSOR\" && bb \"+cd:$BBCURSOR\"", "Enter a folder"},
    {{' ','v','V'},          "+toggle", "Toggle selection"},
    {{'e'},                  "$EDITOR \"$@\"", "Edit file in $EDITOR", NORMAL_TERM},
    {{'\r', KEY_MOUSE_DOUBLE_LEFT},
#ifdef __APPLE__
"if test -d \"$BBCURSOR\"; then bb \"+cd:$BBCURSOR\";\n\
elif test -x \"$BBCURSOR\"; then \"$BBCURSOR\";\n\
    read -n1 -p '\n\033[2m...press any key to continue...\033[0m';\n\
elif file -bI \"$BBCURSOR\" | grep '^\\(text/\\|inode/empty\\)' >/dev/null; then $EDITOR \"$BBCURSOR\";\n\
else open \"$BBCURSOR\"; fi",
#else
"if test -d \"$BBCURSOR\"; then bb \"+cd:$BBCURSOR\";\n\
elif file -bi \"$BBCURSOR\" | grep '^\\(text/\\|inode/empty\\)' >/dev/null; then $EDITOR \"$BBCURSOR\";\n\
else xdg-open \"$BBCURSOR\"; fi",
#endif
        "Open file", NORMAL_TERM},
    {{'f'},                  "bb \"+g:`fzf`\"", "Fuzzy search for file", NORMAL_TERM},
    {{'/'},                  "bb \"+g:`ls -a|fzf`\"", "Fuzzy select file", NORMAL_TERM},
    {{'L'}, PIPE_SELECTION_TO "less", "List all selected files", NORMAL_TERM},
    {{'d', KEY_DELETE},      "rm -rfi \"$@\"; bb '+d:*' +r", "Delete files", SHOW_CURSOR},
    {{'D'},                  "rm -rf \"$@\"; bb '+d:*' +r", "Delete files without confirmation"},
    {{'M'},                  "mv -i \"$@\" .; bb '+d:*' +r", "Move files to current folder", SHOW_CURSOR},
    {{'c'},                  "cp -i \"$@\" .; bb +r", "Copy files to current folder", SHOW_CURSOR},
    {{'C'},                  "for f; do cp \"$f\" \"$f.copy\"; done; bb +r", "Clone files"},
    {{'n'},                  "read -p 'New file: ' name && touch \"$name\"; bb +r \"+goto:$name\"", "New file", SHOW_CURSOR},
    {{'N'},                  "read -p 'New dir: ' name && mkdir \"$name\"; bb +r \"+goto:$name\"", "New folder", SHOW_CURSOR},
    {{'|'},                  "read -p '|' cmd && " PIPE_SELECTION_TO "sh -c \"$cmd\" && " PAUSE "; bb +r",
                             "Pipe selected files to a command", SHOW_CURSOR},
    {{':'},                  "read -p ':' cmd && sh -c \"$cmd\" -- \"$@\"; " PAUSE "; bb +r",
                             "Run a command", SHOW_CURSOR},
    {{'>'},                  "sh", "Open a shell", NORMAL_TERM},
    {{'m'}, "read -n1 -p 'Mark: ' m && bb \"+mark:$m;$PWD\"", "Set mark", SHOW_CURSOR},
    {{'\''}, "read -n1 -p 'Jump: ' j && bb \"+jump:$j\"", "Jump to mark", SHOW_CURSOR},
    {{'r'},                  "for f; do read -p \"Rename $f: \" renamed && mv \"$f\" \"$renamed\"; done; "
                             "bb '+d:*' +r",
                             "Rename files", SHOW_CURSOR},
    {{'R'},                  "read -p 'Rename pattern: ' patt && "
                             "for f; do mv -i \"$f\" \"`echo \"$f\" | sed \"$patt\"`\"; done &&" PAUSE,
                             "Regex rename files", SHOW_CURSOR},
    // TODO debug:
    {{'P'},                  "read -p 'Select pattern: ' patt && "
                             "for f; do echo \"$f\" | grep \"$patt\" >/dev/null 2>/dev/null && bb \"+sel:$f\"; done",
                             "Regex select files"},
    {{'J'},                  "+spread:+1", "Spread selection down"},
    {{'K'},                  "+spread:-1", "Spread selection up"},
    {{'s'}, "read -n1 -p 'Sort \033[1m(a)\033[22mlphabetic "
            "\033[1m(s)\033[22mize \033[1m(m)\033[22modification \033[1m(c)\033[22mcreation "
            "\033[1m(a)\033[22maccess \033[1m(r)\033[22mandom \033[1m(p)\033[22mermissions:\033[0m ' sort "
            "&& bb \"+sort:$sort\"", "Sort by..."},
    {{'#'},                  "read -p 'Set columns: ' cols && bb \"+cols:$cols\"", "Set columns"},
    {{'.'},                  "+dots", "Toggle dotfiles"},
    {{'g', KEY_HOME},        "+move:0", "Go to first file"},
    {{'G', KEY_END},         "+move:100%n", "Go to last file"},
    {{KEY_ESC},              "+deselect:*", "Clear selection"},
    {{KEY_F5, KEY_CTRL_R},   "+refresh", "Refresh"},
    {{KEY_CTRL_A},           "+select:*", "Select all files in current folder"},
    {{KEY_PGDN},             "+scroll:+100%", "Page down"},
    {{KEY_PGUP},             "+scroll:-100%", "Page up"},
    {{KEY_CTRL_D},           "+scroll:+50%", "Half page down"},
    {{KEY_CTRL_U},           "+scroll:-50%", "Half page up"},
    {{KEY_MOUSE_WHEEL_DOWN}, "+scroll:+3", "Scroll down"},
    {{KEY_MOUSE_WHEEL_UP},   "+scroll:-3", "Scroll up"},
    {0}, // Array must be 0-terminated
};
