/*
 * User-defined key bindings.
 */
#include "keys.h"

#define PROG_FUZZY "fzf"
#define SCROLLOFF 5

#define NO_FILES        (1<<0)
#define NULL_SEP        (1<<1)
#define REFRESH         (1<<2)
#define CLEAR_SELECTION (1<<3)
#define ONSCREEN        (1<<4)

struct {
    int key;
    const char *command;
    int flags;
} bindings[] = {
    // User-defined custom scripts go here:
    {'L', "less"},
    {'D', "xargs -0 rm -rf", CLEAR_SELECTION | REFRESH | ONSCREEN | NULL_SEP},
    {'d', "xargs -0 -I @ sh -c 'rm -rfi @ </dev/tty'", CLEAR_SELECTION | REFRESH | ONSCREEN | NULL_SEP},
    {'m', "xargs -0 -I @ mv -i @ . </dev/tty", CLEAR_SELECTION | REFRESH | ONSCREEN | NULL_SEP},
    {'c', "xargs -0 -I @ cp -i @ . </dev/tty", CLEAR_SELECTION | REFRESH | ONSCREEN | NULL_SEP},
    {'C', "xargs -0 -n1 -I @ cp @ @.copy", REFRESH | ONSCREEN | NULL_SEP},
    {'n', "touch \"`printf '\\033[33;1mNew file:\\033[0m ' >/dev/tty && head -n1 /dev/tty`\"", ONSCREEN | REFRESH | NO_FILES},
    {'N', "mkdir \"`printf '\\033[33;1mNew dir:\\033[0m ' >/dev/tty && head -n1 /dev/tty`\"", ONSCREEN | REFRESH | NO_FILES},
    {'|', "sh -c \"`printf '> ' >/dev/tty && head -n1 /dev/tty`\"", REFRESH},
    {'>', "sh -c \"`printf '> ' >/dev/tty && head -n1 /dev/tty`\"", NO_FILES | REFRESH},
    {'r', "xargs -0 -I @ -n1 sh -c 'mv \"@\" \"`printf \"\e[1mRename \e[1;33m%%s\e[0m: \" \"@\" >&2 && head -n1 </dev/tty`\"'",
        REFRESH | CLEAR_SELECTION | ONSCREEN | NULL_SEP},

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
