#!/bin/sh
# This file contains the script that is run when bb launches
# See API.md for details on bb's command API.

# Load key bindings
# first check ~/.config/bb/bindings.bb, then /etc/xdg/bb/bindings.bb, then ./bindings.bb
[ ! -d "$XDG_DATA_HOME/bb" ] && mkdir -p "$XDG_DATA_HOME/bb"
if [ ! -e "$XDG_CONFIG_HOME/bb/bindings.bb" ] && [ ! -e "$sysconfdir/xdg/bb/bindings.bb" ]; then
    cat "./bindings.bb" 2>/dev/null | awk '/^#/ {next} /^[^ ]/ {printf "\0bind:"} {print $0} END {printf "\0"}' >> "$BBCMD"
else
    for path in "$sysconfdir/xdg/bb" "$XDG_CONFIG_HOME/bb"; do
        cat "$path/bindings.bb" 2>/dev/null
    done | awk '/^#/ {next} /^[^ ]/ {printf "\0bind:"} {print $0} END {printf "\0"}' >> "$BBCMD"
fi
if [ -e "$XDG_DATA_HOME/bb/state.sh" ]; then
    . "$XDG_DATA_HOME/bb/state.sh"
fi
