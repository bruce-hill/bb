/*
 * User-defined key bindings.
 */
#define PROG_FUZZY "fzf"
#define PROG_DELETE "rm -rf"
#define SCROLLOFF 5

#define NO_FILES        (1<<0)
#define CD_TO_RESULT    (1<<1)
#define REFRESH         (1<<2)
#define CLEAR_SELECTION (1<<3)
#define ONSCREEN        (1<<4)

#define DEVNULL " >/dev/null"

struct {
    int key;
    const char *command;
    int flags;
} bindings[] = {
    {'?', "less"},
    {'D', "xargs rm -rf" DEVNULL, CLEAR_SELECTION | REFRESH | ONSCREEN},
    {'d', "xargs -I @ sh -c 'rm -rfi @ </dev/tty'", CLEAR_SELECTION | REFRESH | ONSCREEN},
    {'+', "xargs -n1 -I @ cp @ @.copy" DEVNULL, REFRESH | ONSCREEN},
    {'m', "xargs -I @ mv -i @ . </dev/tty" DEVNULL, CLEAR_SELECTION | REFRESH | ONSCREEN},
    {'p', "xargs -I @ cp -i @ . </dev/tty" DEVNULL, CLEAR_SELECTION | REFRESH | ONSCREEN},
    {'n', "touch \"`printf '\\033[33;1mNew file:\\033[0m ' >/dev/tty && head -n1 /dev/tty`\"", ONSCREEN | REFRESH | NO_FILES},
    {'|', "sh -c \"`printf '> ' >/dev/tty && head -n1 /dev/tty`\"", REFRESH},
    {'>', "sh -c \"`printf '> ' >/dev/tty && head -n1 /dev/tty`\"", NO_FILES | REFRESH},
    {'r', "xargs -I @ -n1 sh -c 'mv \"@\" \"`printf \"\e[1mRename \e[0;33m%%s\e[0m: \" \"@\" >&2 && head -n1 </dev/tty`\"'",
        REFRESH | CLEAR_SELECTION | ONSCREEN},
    {'r', "xargs -I @ -n1 sh -c 'mv \"@\" \"`printf \"\\033[1mRename \\033[0;33m%%s\\033[0m: \" \"@\" >&2 && head -n1 </dev/tty`\"'",
        REFRESH | CLEAR_SELECTION | ONSCREEN},
    {0},
};
