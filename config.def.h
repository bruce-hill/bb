/*
 * User-defined key bindings.
 */
#include "keys.h"

#define PROG_FUZZY "fzf"
#define PIPE_SELECTION_TO " printf '%s\\n' \"$@\" | "
#define AND_PAUSE " && read -n1 -p '\n\e[2m...press any key to continue...\e[0m\e[?25l'"
#define SCROLLOFF 5

#define REFRESH         (1<<0)
#define CLEAR_SELECTION (1<<1)
#define ONSCREEN        (1<<2)

struct {
    int key;
    const char *command;
    int flags;
} bindings[] = {
    // User-defined custom scripts go here:
    {'L', PIPE_SELECTION_TO "less"},
    {'D', "rm -rf \"$@\"", CLEAR_SELECTION | REFRESH | ONSCREEN},
    {'d', "rm -rfi \"$@\"", CLEAR_SELECTION | REFRESH | ONSCREEN},
    {'m', "mv -i \"$@\" .", CLEAR_SELECTION | REFRESH | ONSCREEN},
    {'c', "cp -i \"$@\" .", CLEAR_SELECTION | REFRESH | ONSCREEN},
    {'C', "for f; do cp \"$f\" \"$f.copy\"; done", REFRESH | ONSCREEN},
    {'n', "read -p '\e[33;1mNew file:\e[0m ' name && touch \"$name\"", ONSCREEN | REFRESH},
    {'N', "read -p '\e[33;1mNew dir:\e[0m ' name && mkdir \"$name\"", ONSCREEN | REFRESH},
    {'|', "read -p \"\e[33;1m>\e[0m \" cmd && " PIPE_SELECTION_TO "$SHELL -c \"$cmd\"" AND_PAUSE, REFRESH},
    {'>', "$SHELL", REFRESH},
    {'r', "for f; do read -p \"Rename $f: \" renamed && mv \"$f\" \"$renamed\"; done",
        REFRESH | CLEAR_SELECTION | ONSCREEN},

    // Hard-coded behaviors (these are just placeholders for the help):
    {-1, "?\t\e[0;34mOpen help menu\e[0m"},
    {-1, "h,Left\t\e[0;34mNavigate up a directory\e[0m"},
    {-1, "j,Down\t\e[0;34mMove cursor down\e[0m"},
    {-1, "k,Up\t\e[0;34mMove cursor up\e[0m"},
    {-1, "l,Right,Enter\t\e[0;34mOpen file/dir\e[0m"},
    {-1, "Space\t\e[0;34mToggle selection\e[0m"},
    {-1, "J\t\e[0;34mMove selection state down\e[0m"},
    {-1, "K\t\e[0;34mMove selection state up\e[0m"},
    {-1, "q,Q\t\e[0;34mQuit\e[0m"},
    {-1, "g,Home\t\e[0;34mGo to first item\e[0m"},
    {-1, "G,End\t\e[0;34mGo to last item\e[0m"},
    {-1, "s\t\e[0;34mChange sorting\e[0m"},
    {-1, "f,/\t\e[0;34mFuzzy find\e[0m"},
    {-1, "Escape\t\e[0;34mClear selection\e[0m"},
    {-1, "F5,Ctrl-R\t\e[0;34mRefresh\e[0m"},
    {-1, "Ctrl-A\t\e[0;34mSelect all\e[0m"},
    {-1, "Ctrl-C\t\e[0;34mAbort and exit\e[0m"},
    {-1, "PgDn,Ctrl-D\t\e[0;34mPage down\e[0m"},
    {-1, "PgUp,Ctrl-U\t\e[0;34mPage up\e[0m"},
    {-1, "Ctrl-Z\t\e[0;34mSuspend\e[0m"},
    {0},
};
