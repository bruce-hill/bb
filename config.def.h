/*
 * User-defined key bindings.
 */
#include "keys.h"

#define PROG_FUZZY "fzf"
#define PIPE_SELECTION_TO " printf '%s\\n' \"$@\" | "
#define AND_PAUSE " && read -n1 -p '\n\e[2m...press any key to continue...\e[0m\e[?25l'"
#define SCROLLOFF 5

#define NORMAL_TERM     (1<<0)

#define MAX_REBINDINGS 8

struct {
    int keys[MAX_REBINDINGS];
    const char *command;
    const char *description;
    int flags;
} bindings[] = {
    ////////////////////////////////////////////////////////////////////////
    // User-defined custom scripts go here
    // Please note that these are sh scripts, not bash scripts, so bash-isms
    // won't work unless you make your script use `bash -c "<your script>"`
    ////////////////////////////////////////////////////////////////////////

    {{'?'},                  "bb -b | less -r", "Show the help menu", NORMAL_TERM},
    {{'M'},                  "man bb", "Show the bb manpage", NORMAL_TERM},
    {{'q', 'Q'},             "bb -c quit", "Quit"},
    {{'k', KEY_ARROW_UP},    "bb -c 'move:-1'", "Move up"},
    {{'j', KEY_ARROW_DOWN},  "bb -c 'move:+1'", "Move down"},
    {{'h', KEY_ARROW_LEFT},  "bb -c \"cd:..\"", "Go up a folder"},
    {{'l', KEY_ARROW_RIGHT}, "test -d \"$BBCURSOR\" && bb -c \"cd:$BBCURSOR\"", "Enter a folder"},
    {{' '},                  "bb -c \"toggle:$BBCURSOR\"", "Toggle selection"},
    {{'e'},                  "$EDITOR \"$@\"", "Edit file in $EDITOR", NORMAL_TERM},
    {{'\r'},
#ifdef __APPLE__
"if test -d \"$BBCURSOR\"; then bb -c \"cd:$BBCURSOR\";\n\
elif test -x \"$BBCURSOR\"; then \"$BBCURSOR\";\n\
    read -n1 -p '\n\e[2m...press any key to continue...\e[0m\e[?25l';\n\
elif file -bI \"$BBCURSOR\" | grep '^text/' >/dev/null; then $EDITOR \"$BBCURSOR\";\n\
else open \"$BBCURSOR\"; fi",
#else
"if test -d \"$BBCURSOR\"; then bb -c \"cd:$BBCURSOR\";\n\
elif test -x \"$BBCURSOR\"; then \"$BBCURSOR\";\n\
    read -n1 -p '\n\e[2m...press any key to continue...\e[0m\e[?25l';\n\
elif file -bi \"$BBCURSOR\" | grep '^text/' >/dev/null; then $EDITOR \"$BBCURSOR\";\n\
else xdg-open \"$BBCURSOR\"; fi",
#endif
        "Open file", NORMAL_TERM},
    {{'f'},                  "bb -c \"cursor:`fzf`\"", "Fuzzy search for file", NORMAL_TERM},
    {{'/'},                  "bb -c \"cursor:`ls -a|fzf`\"", "Fuzzy select file", NORMAL_TERM},
    {{'L'}, PIPE_SELECTION_TO "less", "List all selected files", NORMAL_TERM},
    {{'d'},                  "rm -rfi \"$@\"; bb -c 'deselect:*' refresh", "Delete files"},
    {{'D'},                  "rm -rf \"$@\"; bb -c 'deselect:*' refresh", "Delete files without confirmation"},
    {{'m'},                  "mv -i \"$@\" .; bb -c 'deselect:*' refresh", "Move files to current folder"},
    {{'c'},                  "cp -i \"$@\" .; bb -c refresh", "Copy files to current folder"},
    {{'C'},                  "for f; do cp \"$f\" \"$f.copy\"; done; bb -c refresh", "Clone files"},
    {{'n'},                  "read -p '\e[33;1mNew file:\e[0m \e[K\e[?25h' name && touch \"$name\"; bb -c refresh", "New file"},
    {{'N'},                  "read -p '\e[33;1mNew dir:\e[0m \e[K\e[?25h' name && mkdir \"$name\"; bb -c refresh", "New folder"},
    {{'|'},                  "read -p '\e[33;1m|>\e[0m \e[K\e[?25h' cmd && " PIPE_SELECTION_TO "$SHELL -c \"$cmd\"" AND_PAUSE " && bb -c refresh",
                             "Pipe selected files to a command"},
    {{':'},                  "read -p '\e[33;1m:>\e[0m \e[K\e[?25h' cmd && $SHELL -c \"$cmd\" -- \"$@\"" AND_PAUSE "&& bb -c refresh",
                             "Run a command"},
    {{'>'},                  "$SHELL", "Open a shell", NORMAL_TERM},
    {{'r'},                  "for f; do read -p \"Rename $f: \e[K\e[?25h\" renamed && mv \"$f\" \"$renamed\"; done; "
                             "bb -c 'deselect:*' refresh",
                             "Rename files"},
    {{'J'},                  "bb -c 'move:x+1'", "Spread selection down"},
    {{'K'},                  "bb -c 'move:x-1'", "Spread selection up"},
    {{'s'}, "read -n1 -p '\e[33mSort \e[1m(a)\e[22mlphabetic \e[1m(s)\e[22mize \e[1m(t)\e[22mime "
            "\e[1m(p)\e[22mermissions:\e[0m \e[K\e[?25h' sort "
            "&& bb -c \"sort:$sort\"", "Sort by..."},
    {{'g', KEY_HOME},        "bb -c move:0", "Go to first file"},
    {{'G', KEY_END},         "bb -c move:999999999", "Go to last file"},
    {{KEY_ESC},              "bb -c 'deselect:*'", "Clear selection"},
    {{KEY_F5, KEY_CTRL_R},   "bb -c refresh", "Refresh"},
    {{KEY_CTRL_A},           "bb -c 'select:*'", "Select all files in current folder"},
    {{KEY_PGDN},             "bb -c 'scroll:+100%'", "Page down"},
    {{KEY_PGUP},             "bb -c 'scroll:-100%'", "Page up"},
    {{KEY_CTRL_D},           "bb -c 'scroll:+50%'", "Half page down"},
    {{KEY_CTRL_U},           "bb -c 'scroll:-50%'", "Half page up"},
    {{KEY_MOUSE_WHEEL_DOWN}, "bb -c 'scroll:+3'", "Scroll down"},
    {{KEY_MOUSE_WHEEL_UP},   "bb -c 'scroll:-3'", "Scroll up"},
    {0},
};
