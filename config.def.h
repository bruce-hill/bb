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
        align:<col-aligns>        Direction of column text alignment ('r' for right, 'c' for center, and 'l' for left)
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
    "+sort:+n", "+col:*smpn", "+..",
    NULL, // NULL-terminated array
};

#ifdef __APPLE__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdollar-in-identifier-extension"
#endif
extern binding_t bindings[];
binding_t bindings[] = {
    //////////////////////////////////////////////////////////////////////////
    // User-defined custom scripts can go here
    // Please note that these are sh scripts, not bash scripts, so bash-isms
    // won't work unless you make your script use `bash -c "<your script>"`
    //////////////////////////////////////////////////////////////////////////
    {{'?', KEY_F1},          "bb -b | $PAGER -r", "Show the help menu", NORMAL_TERM},
    {{'q', 'Q'},             "+quit", "Quit"},
    {{'k', KEY_ARROW_UP},    "+move:-1", "Move up"},
    {{'j', KEY_ARROW_DOWN},  "+move:+1", "Move down"},
    {{'h', KEY_ARROW_LEFT},  "+cd:..", "Go up a folder"},
    {{'l', KEY_ARROW_RIGHT}, "test -d \"$BBCURSOR\" && bb \"+cd:$BBCURSOR\"", "Enter a folder"},
    {{' ','v','V'},          "+toggle", "Toggle selection"},
    {{'e'},                  "$EDITOR \"$@\"", "Edit file in $EDITOR", NORMAL_TERM},
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
        "Open file", NORMAL_TERM},
    {{'f'},                  "bb \"+g:`fzf`\"", "Fuzzy search for file", NORMAL_TERM},
    {{'/'},                  "bb \"+g:`ls -a|fzf`\"", "Fuzzy select file", NORMAL_TERM},
    {{'L'}, PIPE_SELECTION_TO "$PAGER", "List all selected files", NORMAL_TERM},
    {{'d', KEY_DELETE},      "rm -rfi \"$@\"; bb '+d:*' +r", "Delete files", AT_CURSOR},
    {{'D'},                  "rm -rf \"$@\"; bb '+d:*' +r", "Delete files without confirmation"},
    {{'M'},                  "mv -i \"$@\" .; bb '+d:*' +r", "Move files to current folder"},
    {{'c'},                  "cp -i \"$@\" .; bb +r", "Copy files to current folder"},
    {{'C'},                  "for f; do cp \"$f\" \"$f.copy\"; done; bb +r", "Clone files"},
    {{'n'},                  "name=`bb '?New file: '` && touch \"$name\"; bb +r \"+goto:$name\"", "New file"},
    {{'N'},                  "name=`bb '?New dir: '` && mkdir \"$name\"; bb +r \"+goto:$name\"", "New folder"},
    {{'|'},                  "cmd=`bb '?|'` && " PIPE_SELECTION_TO "sh -c \"$cmd\" && " PAUSE "; bb +r",
                             "Pipe selected files to a command"},
    {{':'},                  "$SHELL -c \"`bb '?:'`\" -- \"$@\"; " PAUSE "; bb +refresh",
                             "Run a command"},
    {{'>'},                  "$SHELL", "Open a shell", NORMAL_TERM},
    {{'m'}, "read -n1 -p 'Mark: ' m && bb \"+mark:$m;$PWD\"", "Set mark"},
    {{'\''}, "read -n1 -p 'Jump: ' j && bb \"+jump:$j\"", "Jump to mark"},

    {{'r'}, QUOTE(
bb '+deselect:*' +refresh; 
for f; do 
    if renamed="$(dirname "$f")/$(bb '?Rename: ' "$(basename "$f")")" &&
        test "$f" != "$renamed" && mv -i "$f" "$renamed"; then 
        test $BBSELECTED && bb "+select:$renamed"; 
    elif test $BBSELECTED; then bb "+select:$f"; fi 
done)/*ENDQUOTE*/, "Rename files", AT_CURSOR},

    {{'R'}, QUOTE(
if patt="`bb '?Rename pattern: ' 's/'`"; then true; else bb +r; exit; fi;
if sed -E "$patt" </dev/null; then true; else read -p 'Press any key to continue...' -n1; bb +r; exit; fi;
bb '+deselect:*' +refresh; 
for f; do 
    renamed="`dirname "$f"`/`basename "$f" | sed -E "$patt"`"; 
    if test "$f" != "$renamed" && mv -i "$f" "$renamed"; then 
        test $BBSELECTED && bb "+select:$renamed"; 
    elif test $BBSELECTED; then bb "+select:$f"; fi 
done)/*ENDQUOTE*/, "Regex rename files", AT_CURSOR},
    
    // TODO debug:
    {{'P'},                  "patt=`bb '?Select pattern: '` && "
                             "for f; do echo \"$f\" | grep \"$patt\" >/dev/null 2>/dev/null && bb \"+sel:$f\"; done",
                             "Regex select files"},
    {{'J'},                  "+spread:+1", "Spread selection down"},
    {{'K'},                  "+spread:-1", "Spread selection up"},
    {{'b'},                  "bb \"+`bb '?bb +'`\"", "Run a bb command"},
    {{'s'}, "read -n1 -p 'Sort \033[1m(a)\033[22mlphabetic "
            "\033[1m(s)\033[22mize \033[1m(m)\033[22modification \033[1m(c)\033[22mcreation "
            "\033[1m(a)\033[22maccess \033[1m(r)\033[22mandom \033[1m(p)\033[22mermissions:\033[0m ' sort "
            "&& bb \"+sort:+$sort\"", "Sort by..."},
    {{'#'},                  "bb \"+col:`bb '?Set columns: '`\"", "Set columns"},
    {{'.'},                  "bb +dotfiles", "Toggle dotfiles"},
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
    {{0}}, // Array must be 0-terminated
};
#ifdef __APPLE__
#pragma clang diagnostic pop
#endif

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
