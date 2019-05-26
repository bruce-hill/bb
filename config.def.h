/*
 * User-defined key bindings.
 */
#include "keys.h"

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

extern const char *startupcmds[];
const char *startupcmds[] = {
    "+mark:0",
    "+mark:~;~",
    "+mark:h;~",
    "+mark:/;/",
    "+mark:c;~/.config",
    "+mark:l;~/.local",
    "+columns:smpn",
    "+sort:n",
    NULL,
};

extern binding_t bindings[];
binding_t bindings[] = {
    ////////////////////////////////////////////////////////////////////////
    // User-defined custom scripts go here
    // Please note that these are sh scripts, not bash scripts, so bash-isms
    // won't work unless you make your script use `bash -c "<your script>"`
    ////////////////////////////////////////////////////////////////////////
    {{'?'},                  "bb -b | less -r", "Show the help menu", NORMAL_TERM},
    {{KEY_CTRL_H},           "", "Figure out what key does"},
    {{'q', 'Q'},             "bb +q", "Quit"},
    {{'k', KEY_ARROW_UP},    "+m:-1", "Move up"},
    {{'j', KEY_ARROW_DOWN},  "+m:+1", "Move down"},
    {{'h', KEY_ARROW_LEFT},  "bb \"+cd:..\"", "Go up a folder"},
    {{'l', KEY_ARROW_RIGHT}, "test -d \"$BBCURSOR\" && bb \"+cd:$BBCURSOR\"", "Enter a folder"},
    {{' ','v','V'},          "bb \"+t:$BBCURSOR\"", "Toggle selection"},
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
elif test -x \"$BBCURSOR\"; then \"$BBCURSOR\";\n\
    read -n1 -p '\n\033[2m...press any key to continue...\033[0m';\n\
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
    {{'n'},                  "read -p 'New file: ' name && touch \"$name\"; bb +r", "New file", SHOW_CURSOR},
    {{'N'},                  "read -p 'New dir: ' name && mkdir \"$name\"; bb +r", "New folder", SHOW_CURSOR},
    {{'|'},                  "read -p '| ' cmd && " PIPE_SELECTION_TO "sh -c \"$cmd\" && " PAUSE "; bb +r",
                             "Pipe selected files to a command", SHOW_CURSOR},
    {{':'},                  "read -p ': ' cmd && sh -c \"$cmd\" -- \"$@\"; " PAUSE "; bb +r",
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
    {{'J'},                  "bb '+spread:+1'", "Spread selection down"},
    {{'K'},                  "bb '+spread:-1'", "Spread selection up"},
    {{'s'}, "read -n1 -p 'Sort \033[1m(a)\033[22mlphabetic "
            "\033[1m(s)\033[22mize \033[1m(m)\033[22modification \033[1m(c)\033[22mcreation "
            "\033[1m(a)\033[22maccess \033[1m(r)\033[22mandom \033[1m(p)\033[22mermissions:\033[0m ' sort "
            "&& bb \"+sort:$sort\"", "Sort by..."},
    {{'#'},                  "read -p 'Set columns: ' cols && bb \"+cols:$cols\"", "Set columns"},
    {{'.'},                  "bb +dots", "Toggle dotfiles"},
    {{'g', KEY_HOME},        "bb +m:0", "Go to first file"},
    {{'G', KEY_END},         "bb +m:100%n", "Go to last file"},
    {{KEY_ESC},              "bb '+d:*'", "Clear selection"},
    {{KEY_F5, KEY_CTRL_R},   "bb +r", "Refresh"},
    {{KEY_CTRL_A},           "bb '+select:*'", "Select all files in current folder"},
    {{KEY_PGDN},             "bb '+scroll:+100%'", "Page down"},
    {{KEY_PGUP},             "bb '+scroll:-100%'", "Page up"},
    {{KEY_CTRL_D},           "bb '+scroll:+50%'", "Half page down"},
    {{KEY_CTRL_U},           "bb '+scroll:-50%'", "Half page up"},
    {{KEY_MOUSE_WHEEL_DOWN}, "bb '+scroll:+3'", "Scroll down"},
    {{KEY_MOUSE_WHEEL_UP},   "bb '+scroll:-3'", "Scroll up"},
    {0},
};
