# This file defines the key bindings for bb
# The format is: <key>(,<key>)*:[ ]*#<description>(\n[ ]+script)+
Section: BB Commands
?,F1: # Show Help menu
    bb +help
q,Q: # Quit
    bb +quit
Ctrl-c: # Send interrupt signal
    kill -INT $PPID
Ctrl-z: # Suspend
    kill -TSTP $PPID
Ctrl-\: # Quit and generate core dump
    kill -QUIT $PPID

Section: File Navigation
j,Down: # Next file
    bb +move:+1
k,Up: # Previous file
    bb +move:-1
h,Left: # Parent directory
    bb +cd:..
l,Right: # Enter directory
    [ -d "$BBCURSOR" ] && bb +cd:"$BBCURSOR"
Ctrl-f: # Search for file
    file="$(
        if [ $BBDOTFILES ]; then
            find -mindepth 1 -print0;
        else find -mindepth 1 ! -path '*/.*' -print0;
        fi | pick "Find: "
    )" && bb +goto:"$file"
/: # Pick a file
    file="$( (printf '%s\0' *; [ $BBDOTFILES ] && printf '%s\0' .[!.]* ..?*) | pick "Pick: ")" &&
        bb +goto:"$file"
Ctrl-g: # Go to directory
    ask goto "Go to directory: " && bb +cd:"$goto"
m: # Mark this directory
    ask mark "Mark: " && ln -s "$PWD" ~/.config/bb/marks/"$mark"
': # Go to a marked directory
    mark="$(basename -az ~/.config/bb/marks/* | pick "Jump to: ")" &&
        bb +cd:"$(readlink -f ~/.config/bb/marks/"$mark")"
-,Backspace: # Go to previous directory
    [ $BBPREVPATH ] && bb +cd:"$BBPREVPATH"
;: # Show selected files
    bb +cd:'<selection>'
0: # Go to intitial directory
    bb +cd:"$BBINITIALPATH"
g,Home: # Go to first file
    bb +move:0
G,End: # Go to last file
    bb +move:100%n
PgDn: # Page down
    bb +scroll:+100%
PgUp: # Page up
    bb +scroll:-100%
Ctrl-d: # Half page down
    bb +scroll:+50%
Ctrl-u: # Half page up
    bb +scroll:-50%
Mouse wheel down: # Scroll down
    bb +scroll:+3
Mouse wheel up: # Scroll up
    bb +scroll:-3

Section: File Selection
v,V,Space: # Toggle selection at cursor
    bb +toggle
Escape: # Clear selection
    bb +deselect
Ctrl-s: # Save the selection
    [ $# -gt 0 ] && ask savename "Save selection as: " && printf '%s\0' "$@" > ~/.config/bb/"$savename"
Ctrl-o: # Open a saved selection
    loadpath="$(printf '%s\0' ~/.config/bb/* | pick "Load selection: ")" &&
        [ -e "$loadpath" ] && bb +deselect &&
        while IFS= read -r -d $'\0'; do bb +select:"$REPLY"; done < "$loadpath"
J: # Spread selection down
    bb +spread:+1
K: # Spread selection up
    bb +spread:-1
Shift-Home: # Spread the selection to the top
    bb +spread:0
Shift-End: # Spread the selection to the bottom
    bb +spread:100%n
Ctrl-a: # Select all files here
    if [ $BBDOTFILES ]; then bb +sel: * .[!.]* ..?*
    else bb +sel: *; fi

Section: File Actions
Left click: # Move cursor to file
    if [ "$BBCLICKED" = "<column label>" ]; then
        bb +sort:"~$BBMOUSECOL"
    elif [ "$BBCLICKED" -a "$BBMOUSECOL" = "*" ]; then
        bb +toggle:"$BBCLICKED"
    elif [ "$BBCLICKED" ]; then
        bb +goto:"$BBCLICKED"
    fi
Enter,Double left click: # Open file/directory
    if [ "$(uname)" = "Darwin" ]; then
        if [ -d "$BBCURSOR" ]; then bb +cd:"$BBCURSOR";
        elif file -bI "$BBCURSOR" | grep -q '^\(text/\|inode/empty\)'; then $EDITOR "$BBCURSOR";
        else open "$BBCURSOR"; fi
    else
        if [ -d "$BBCURSOR" ]; then bb +cd:"$BBCURSOR";
        elif file -bi "$BBCURSOR" | grep -q '^\(text/\|inode/x-empty\)'; then $EDITOR "$BBCURSOR";
        else xdg-open "$BBCURSOR"; fi
    fi
e: # Edit file in $EDITOR
    $EDITOR "$BBCURSOR" || pause
d,Delete: # Delete a file
    printf "\033[1mDeleting \033[33m$BBCURSOR\033[0;1m...\033[0m " && confirm &&
        rm -rf "$BBCURSOR" && bb +deselect:"$BBCURSOR" +refresh
D: # Delete all selected files
    [ $# -gt 0 ] && printf "\033[1mDeleting the following:\n\033[33m$(printf '  %s\n' "$@")\033[0m" | more &&
        confirm && rm -rf "$@" && bb +deselect +refresh
Ctrl-v: # Move files here
    printf "\033[1mMoving the following to here:\n\033[33m$(printf '  %s\n' "$@")\033[0m" | more &&
        confirm && printf "\033[1G\033[KMoving..." && mv -i "$@" . && printf "done." &&
        bb +deselect +refresh && for f; do bb +sel:"$(basename "$f")"; done
c: # Copy a file
    printf "\033[1mCreating copy of \033[33m$BBCURSOR\033[0;1m...\033[0m " &&
        confirm && cp -ri "$BBCURSOR" "$BBCURSOR.copy" && bb +refresh
C: # Copy all selected files here
    [ $# -gt 0 ] && printf "\033[1mCopying the following to here:\n\033[33m$(printf '  %s\n' "$@")\033[0m" | more &&
        confirm && printf "\033[1G\033[KCopying..." &&
        for f; do if [ "./$(basename "$f")" -ef "$f" ]; then
            cp -ri "$f" "$f.copy" || break;
        else cp -ri "$f" . || break; fi; done; printf 'done.' && bb +refresh
Ctrl-n: # New file/directory
    case "$(printf '%s\0' File Directory | pick "Create new: ")" in
        File)
            ask name "New File: " || exit
            touch "$name"
            ;;
        Directory)
            ask name "New Directory: " || exit
            mkdir "$name"
            ;;
        *) exit
            ;;
    esac && bb +goto:"$name" +refresh || pause
p: # Page through a file with `less`
    less -XK "$BBCURSOR"
r,F2: # Rename a file
    ask newname "Rename $(printf "\033[33m%s\033[39m" "$(basename "$BBCURSOR")"): " "$(basename "$BBCURSOR")" || exit
    r="$(dirname "$BBCURSOR")/$newname" || exit
    [ "$r" = "$BBCURSOR" ] && exit
    [ -e "$r" ] && printf "\033[31;1m$r already exists! It will be overwritten.\033[0m " &&
        confirm && { rm -rf "$r" || { pause; exit; }; }
    mv "$BBCURSOR" "$r" && bb +refresh &&
        while [ $# -gt 0 ]; do  "$1" = "$BBCURSOR"  && bb +deselect:"$BBCURSOR" +select:"$r"; shift; done &&
        bb +goto:"$r" || { pause; exit; }
R: # Rename all selected files
    for f; do
        ask newname "Rename $(printf "\033[33m%s\033[39m" "$(basename "$f")"): " "$(basename "$f")" || break;
        r="$(dirname "$f")/$newname";
        [ "$r" = "$f" ] && continue;
        [ -e "$r" ] && printf "\033[31;1m$r already exists! It will be overwritten.\033[0m "
            && confirm && { rm -rf "$r" || { pause; exit; }; }
        mv "$f" "$r" || { pause; exit; }
        bb +deselect:"$f" +select:"$r";
        [ "$f" = "$BBCURSOR" ] && bb +goto:"$r";
    done;
    bb +refresh
Ctrl-r: # Regex rename files
    command -v rename >/dev/null ||
        { printf '\033[31;1mThe `rename` command is not installed. Please install it to use this key binding.\033[0m\n'; pause; exit; };
    ask patt "Replace pattern: " && ask rep "Replacement: " &&
        printf "\033[1mRenaming:\n\033[33m$(if [ $# -gt 0 ]; then rename -nv "$patt" "$rep" "$@"; else rename -nv "$patt" "$rep" *; fi)\033[0m" | more &&
        confirm &&
        if [ $# -gt 0 ]; then rename -i "$patt" "$rep" "$@"; else rename -i "$patt" "$rep" *; fi;
    bb +deselect +refresh

Section: Shell Commands
:: # Run a command
    ask cmd ':' && sh -c "$BBSHELLFUNC$cmd" -- "$@"; bb +r; pause
|: # Pipe selected files to a command
    ask cmd '|' && printf '%s\n' "$@" | sh -c "$BBSHELLFUNC$cmd"; bb +r; pause
>: # Open a shell
    tput rmcup; tput cvvis; $SHELL; bb +r
f: # Resume suspended process
    bb +fg

Section: Viewing Options
s: # Sort by...
    ask1 sort "Sort (n)ame (s)ize (m)odification (c)reation (a)ccess (r)andom (p)ermissions: " &&
        bb +sort:"~$sort"
---,#: # Set columns
    ask columns "Set columns (*)selected (a)ccessed (c)reated (m)odified (n)ame (p)ermissions (r)andom (s)ize: " &&
        bb +col:"$columns"
.: # Toggle dotfile visibility
    bb +dotfiles
i: # Toggle interleaving files and directories
    bb +interleave
F5: # Refresh view
    bb +refresh
Ctrl-b: # Bind a key to a script
    ask1 key "Press key to bind..." && echo && ask script "Bind script: " &&
        bb +bind:"$key":"{ $script; } || pause" || pause

Section: User Bindings
