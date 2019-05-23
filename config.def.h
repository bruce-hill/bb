/*
 * User-defined key bindings.
 */
#include "keys.h"

#define PROG_FUZZY "fzf"
#define PIPE_SELECTION_TO " printf '%s\\n' \"$@\" | "
#define AND_PAUSE " && read -n1 -p '\n\e[2m...press any key to continue...\e[0m\e[?25l'"
#define SCROLLOFF 5

#define NORMAL_TERM     (1<<0)

struct {
    int key;
    const char *command;
    int flags;
} bindings[] = {
    ////////////////////////////////////////////////////////////////////////
    // User-defined custom scripts go here
    // Please note that these are sh scripts, not bash scripts, so bash-isms
    // won't work unless you make your script use `bash -c "<your script>"`
    ////////////////////////////////////////////////////////////////////////


    {'1', "bb -c 'move:x+1'"},
    {'2', "bb -c 'move:x-1'"},
    {'3', "bb -c 'move:x+10'"},
    {'4', "bb -c 'move:x-10'"},

    {'e', "$EDITOR \"$@\"", NORMAL_TERM},
    {'L', PIPE_SELECTION_TO "less", NORMAL_TERM},
    {'D', "rm -rf \"$@\"; bb -c 'deselect:*' refresh"},
    {'d', "rm -rfi \"$@\"; bb -c 'deselect:*' refresh"},
    {'m', "mv -i \"$@\" .; bb -c 'deselect:*' refresh"},
    {'c', "cp -i \"$@\" .; bb -c refresh"},
    {'C', "for f; do cp \"$f\" \"$f.copy\"; done; bb -c refresh"},
    {'n', "read -p '\e[33;1mNew file:\e[0m \e[K\e[?25h' name && touch \"$name\"; bb -c refresh"},
    {'N', "read -p '\e[33;1mNew dir:\e[0m \e[K\e[?25h' name && mkdir \"$name\"; bb -c refresh"},
    {'|', "read -p '\e[33;1m|>\e[0m \e[K\e[?25h' cmd && " PIPE_SELECTION_TO "$SHELL -c \"$cmd\"" AND_PAUSE},
    {':', "read -p '\e[33;1m:>\e[0m \e[K\e[?25h' cmd && $SHELL -c \"$cmd\" -- \"$@\"" AND_PAUSE},
    {'>', "$SHELL", NORMAL_TERM},
    {'r', "for f; do read -p \"Rename $f: \e[K\e[?25h\" renamed && mv \"$f\" \"$renamed\"; done;"
          " bb -c 'deselect:*' refresh"},
    {'h', "bb -c \"cd:..\""},
    {KEY_ARROW_LEFT, "bb -c 'cd:..'"},
    {'j', "bb -c 'move:+1'"},
    {'J', "bb -c 'move:x+1'"},
    {KEY_ARROW_DOWN, "bb -c 'move:+1'"},
    {'k', "bb -c 'move:-1'"},
    {'K', "bb -c 'move:x-1'"},
    {KEY_ARROW_UP, "bb -c 'move:-1'"},
    {'l', "bb -c \"cd:$BBFULLCURSOR\""},
    {KEY_ARROW_RIGHT, "bb -c \"cd:$BBFULLCURSOR\""},
#ifdef __APPLE__
    {'\r', "if test -x \"$BBCURSOR\"; then \"$BBCURSOR\"; "
           "elif test -d \"$BBCURSOR\"; then bb -c \"cd:$BBFULLCURSOR\"; "
           "elif file -bI \"$BBCURSOR\" | grep '^text/' >/dev/null; then $EDITOR \"$BBCURSOR\"; "
           "else open \"$BBCURSOR\"; fi"},
#else
    {'\r', "if test -x \"$BBCURSOR\"; then \"$BBCURSOR\"; "
           "elif test -d \"$BBCURSOR\"; then bb -c \"cd:$BBFULLCURSOR\"; "
           "elif file -bi \"$BBCURSOR\" | grep '^text/' >/dev/null; then $EDITOR \"$BBCURSOR\"; "
           "else xdg-open \"$BBCURSOR\"; fi"},
#endif
    {' ', "bb -c \"toggle:$BBCURSOR\""},
    {'s', "read -n1 -p '\e[33mSort \e[1m(a)\e[22mlphabetic \e[1m(s)\e[22mize \e[1m(t)\e[22mime \e[1m(p)\e[22mermissions:\e[0m \e[K\e[?25h' sort "
          "&& bb -c \"sort:$sort\""},
    {'q', "bb -c quit"},
    {'Q', "bb -c quit"},
    {'g', "bb -c move:0"},
    {KEY_HOME, "bb -c move:0"},
    {'G', "bb -c move:9999999999"},
    {KEY_END, "bb -c move:999999999"},
    {'f', "bb -c \"cursor:`fzf`\"", NORMAL_TERM},
    {'/', "bb -c \"cursor:`ls -a|fzf`\"", NORMAL_TERM},
    {KEY_ESC, "bb -c 'deselect:*'"},
    {KEY_F5, "bb -c refresh"},
    {KEY_CTRL_R, "bb -c refresh"},
    {KEY_CTRL_A, "bb -c 'select:*'"},
    {KEY_PGDN, "bb -c 'scroll:+100%'"},
    {KEY_CTRL_D, "bb -c 'scroll:+50%'"},
    {KEY_PGUP, "bb -c 'scroll:-100%'"},
    {KEY_CTRL_U, "bb -c 'scroll:-50%'"},
    {KEY_MOUSE_WHEEL_DOWN, "bb -c 'scroll:+3'"},
    {KEY_MOUSE_WHEEL_UP, "bb -c 'scroll:-3'"},


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
