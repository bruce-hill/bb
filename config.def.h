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
        deselect:<filename>       Deselect <filename>
        dotfiles:[01]             Whether dotfiles are visible
        goto:<filename>           Move the cursor to <filename> (changing directory if needed)
        interleave:[01]           Whether or not directories should be interleaved with files in the display
        jump:<key>                Jump to the mark associated with <key>
        mark:<key>[=<path>]       Associate <key> with <path> (or current dir, if blank)
        move:<num*>               Move the cursor a numeric amount
        quit                      Quit bb
        refresh                   Refresh the file listing
        scroll:<num*>             Scroll the view a numeric amount
        select:<filename>         Select <filename>
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

#define QUOTE(s) #s

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

#define PIPE_SELECTION_TO " printf '%s\\n' \"$@\" | "
#define PAUSE " read -n1 -p '\033[2mPress any key to continue...\033[0m\033[?25l'"

#define NORMAL_TERM     (1<<0)
#define AT_CURSOR       (1<<1)

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

#ifdef __APPLE__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdollar-in-identifier-extension"
#endif
extern binding_t bindings[];
#define EM(s) "\033[33;4m" s "\033[0;34m"
binding_t bindings[] = {
    //////////////////////////////////////////////////////////////////////////
    // User-defined custom scripts can go here
    // Please note that these are sh scripts, not bash scripts, so bash-isms
    // won't work unless you make your script use `bash -c "<your script>"`
    //////////////////////////////////////////////////////////////////////////
    {{'?', KEY_F1},          "bb -b | $PAGER -r", EM("Help")" menu", NORMAL_TERM},
    {{'q', 'Q'},             "+quit", EM("Quit")},
    {{'j', KEY_ARROW_DOWN},  "+move:+1", EM("Next")" file"},
    {{'k', KEY_ARROW_UP},    "+move:-1", EM("Previous")" file"},
    {{'h', KEY_ARROW_LEFT},  "+cd:..", EM("Parent")" directory"},
    {{'l', KEY_ARROW_RIGHT}, "test -d \"$BBCURSOR\" && bb \"+cd:$BBCURSOR\"", EM("Enter")" a directory"},
    {{'\r', KEY_MOUSE_DOUBLE_LEFT},
#ifdef __APPLE__
        QUOTE(
if test -d "$BBCURSOR"; then bb "+cd:$BBCURSOR";
elif test -x "$BBCURSOR"; then "$BBCURSOR"; read -p 'Press any key to continue...' -n1;
elif file -bI "$BBCURSOR" | grep '^\(text/\|inode/empty\)' >/dev/null; then $EDITOR "$BBCURSOR";
else open "$BBCURSOR"; fi
        )/*ENDQUOTE*/,
#else
        QUOTE(
if test -d "$BBCURSOR"; then bb "+cd:$BBCURSOR";
elif file -bi "$BBCURSOR" | grep '^\(text/\|inode/empty\)' >/dev/null; then $EDITOR "$BBCURSOR";
else xdg-open "$BBCURSOR"; fi
        )/*ENDQUOTE*/,
#endif
        EM("Open")" file/directory", NORMAL_TERM},
    {{' ','v','V'},          "+toggle", EM("Toggle")" selection"},
    {{KEY_ESC},              "+deselect:*", EM("Clear")" selection"},
    {{'e'},                  "$EDITOR \"$@\"", EM("Edit")" file in $EDITOR", NORMAL_TERM},
    {{KEY_CTRL_F},           "bb \"+g:`fzf`\"", EM("Fuzzy search")" for file", NORMAL_TERM},
    {{'/'},                  "bb \"+g:`ls -a|fzf`\"", EM("Fuzzy select")" file", NORMAL_TERM},
    {{'d', KEY_DELETE},      "rm -rfi \"$@\"; bb '+de:*' +r", EM("Delete")" files", AT_CURSOR},
    {{'D'},                  "rm -rf \"$@\"; bb '+de:*' +r", EM("Delete")" files (without confirmation)"},
    {{'M'},                  "mv -i \"$@\" .; bb '+de:*' +r; for f; do bb \"+sel:`pwd`/`basename \"$f\"`\"; done",
                             EM("Move")" files to current directory"},
    {{'c'},                  "cp -i \"$@\" .; bb +r", EM("Copy")" files to current directory"},
    {{'C'},                  "bb '+de:*'; for f; do cp \"$f\" \"$f.copy\" && bb \"+sel:$f.copy\"; done; bb +r", EM("Clone")" files"},
    {{'n'},                  "name=`bb '?New file: '` && touch \"$name\"; bb +r \"+goto:$name\"", EM("New file")},
    {{'N'},                  "name=`bb '?New dir: '` && mkdir \"$name\"; bb +r \"+goto:$name\"", EM("New directory")},
    {{'|'},                  "cmd=`bb '?|'` && " PIPE_SELECTION_TO "sh -c \"$cmd\" && " PAUSE "; bb +r",
                             EM("Pipe")" selected files to a command"},
    {{':'},                  "$SHELL -c \"`bb '?:'`\" -- \"$@\"; " PAUSE "; bb +refresh",
                             EM("Run")" a command"},
    {{'>'},                  "$SHELL", "Open a "EM("shell"), NORMAL_TERM},
    {{'m'}, "read -n1 -p 'Mark: ' m && bb \"+mark:$m;$PWD\"", "Set "EM("mark")},
    {{'\''}, "read -n1 -p 'Jump: ' j && bb \"+jump:$j\"", EM("Jump")" to mark"},

    {{'r'}, QUOTE(
bb '+deselect:*' +refresh; 
for f; do 
    if renamed="$(dirname "$f")/$(bb '?Rename: ' "$(basename "$f")")" &&
        test "$f" != "$renamed" && mv -i "$f" "$renamed"; then 
        test $BBSELECTED && bb "+select:$renamed"; 
    elif test $BBSELECTED; then bb "+select:$f"; fi 
done)/*ENDQUOTE*/, EM("Rename")" files", AT_CURSOR},

    {{'R'}, QUOTE(
if patt="`bb '?Rename pattern: ' 's/'`"; then true; else bb +r; exit; fi;
if sed -E "$patt" </dev/null; then true; else read -p 'Press any key to continue...' -n1; bb +r; exit; fi;
bb '+deselect:*' +refresh; 
for f; do 
    renamed="`dirname "$f"`/`basename "$f" | sed -E "$patt"`"; 
    if test "$f" != "$renamed" && mv -i "$f" "$renamed"; then 
        test $BBSELECTED && bb "+select:$renamed"; 
    elif test $BBSELECTED; then bb "+select:$f"; fi 
done)/*ENDQUOTE*/, EM("Regex rename")" files", AT_CURSOR},
    
    // TODO debug:
    {{'P'},                  "patt=`bb '?Select pattern: '` && "
                             "for f; do echo \"$f\" | grep \"$patt\" >/dev/null 2>/dev/null && bb \"+sel:$f\"; done",
                             EM("Regex select")" files"},
    {{'J'},                  "+spread:+1", EM("Spread")" selection down"},
    {{'K'},                  "+spread:-1", EM("Spread")" selection up"},
    {{'b'},                  "bb \"+`bb '?bb +'`\"", "Run a "EM("bb command")},
    {{'s'}, "read -n1 -p 'Sort \033[1m(a)\033[22mlphabetic "
            "\033[1m(s)\033[22mize \033[1m(m)\033[22modification \033[1m(c)\033[22mcreation "
            "\033[1m(a)\033[22maccess \033[1m(r)\033[22mandom \033[1m(p)\033[22mermissions:\033[0m ' sort "
            "&& bb \"+sort:+$sort\"", EM("Sort")" by..."},
    {{'#'},                  "bb \"+col:`bb '?Set columns: '`\"", "Set "EM("columns")},
    {{'.'},                  "bb +dotfiles", "Toggle "EM("dotfiles")},
    {{'g', KEY_HOME},        "+move:0", "Go to "EM("first")" file"},
    {{'G', KEY_END},         "+move:100%n", "Go to "EM("last")" file"},
    {{KEY_F5, KEY_CTRL_R},   "+refresh", EM("Refresh")},
    {{KEY_CTRL_A},           "+select:*", EM("Select all")" files in current directory"},
    {{KEY_PGDN},             "+scroll:+100%", EM("Page down")},
    {{KEY_PGUP},             "+scroll:-100%", EM("Page up")},
    {{KEY_CTRL_D},           "+scroll:+50%", EM("Half page down")},
    {{KEY_CTRL_U},           "+scroll:-50%", EM("Half page up")},
    {{KEY_MOUSE_WHEEL_DOWN}, "+scroll:+3", EM("Scroll down")},
    {{KEY_MOUSE_WHEEL_UP},   "+scroll:-3", EM("Scroll up")},
    {{0}}, // Array must be 0-terminated
};
#ifdef __APPLE__
#pragma clang diagnostic pop
#endif

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
