#!/bin/sh
# This file contains the script that is run when bb launches
# See API.md for details on bb's command API.

[ ! -d "$XDG_DATA_HOME/bb" ] && mkdir -p "$XDG_DATA_HOME/bb"

# Load key bindings
if [ "$BBPATH" ]; then
    cat "$BBPATH/bindings.bb" "$XDG_CONFIG_HOME/bb/bindings.bb"
else
    cat "$sysconfdir/xdg/bb/bindings.bb" "$XDG_CONFIG_HOME/bb/bindings.bb"
fi 2>/dev/null | awk '/^#/ {next} /^[^ ]/ {printf "\0bind:"} {print $0} END {printf "\0"}' >> "$BBCMD"

if [ -e "$XDG_DATA_HOME/bb/state.sh" ]; then
    . "$XDG_DATA_HOME/bb/state.sh"
fi
