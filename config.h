/*
 * User-defined key bindings.
 */
#define PROG_FUZZY "fzf"
#define PROG_DELETE "rm -rf"
#define SCROLLOFF 5

#define NO_FILES        (1<<0)
#define PROMPT          (1<<1)
#define CD_TO_RESULT    (1<<2)
#define REFRESH         (1<<3)
#define CLEAR_SELECTION (1<<4)
#define SILENT          (1<<5)

#define DEVNULL " >/dev/null 2>/dev/null"

struct {
    int key;
    const char *command;
    int flags;
    const char *prompt;
} bindings[] = {
    {'?', "less"},
    {'D', "xargs rm -rf" DEVNULL, CLEAR_SELECTION | REFRESH | SILENT},
    {'d', "xargs -I @ sh -c 'rm -rfi @ </dev/tty'", CLEAR_SELECTION | REFRESH},
    {'+', "xargs -n1 -I @ cp @ @.copy" DEVNULL, REFRESH | SILENT},
    {'m', "xargs -I @ mv @ ." DEVNULL, CLEAR_SELECTION | REFRESH | SILENT},
    {'p', "xargs -I @ cp @ ." DEVNULL, CLEAR_SELECTION | REFRESH | SILENT},
    {'n', "touch %s", SILENT | PROMPT | REFRESH | NO_FILES, "New file: "},
    {'|', "sh -c \"`read -p '> ' </dev/tty`\"", 0},//, PROMPT, "> "},
    //{'|', "%s", PROMPT, "> "},
    {'>', "%s", PROMPT | NO_FILES, "> "},
    {'r', "xargs -I @ -n1 sh -c 'mv \"@\" \"`printf \"\\033[1mRename \\033[0;33m%%s\\033[0m: \" \"@\" >&2 && head -n1 </dev/tty`\"'",
        REFRESH | CLEAR_SELECTION},
    {0},
};
