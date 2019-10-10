#!/bin/sh
# This file contains the script that is run when bb launches
# See API.md for details on bb's API.

# Default XDG values
[ ! "$XDG_CONFIG_HOME" ] && XDG_CONFIG_HOME=~/.config
[ ! "$sysconfdir" ] && sysconfdir=/etc

# Create some default marks:
mkdir -p "$XDG_CONFIG_HOME/bb/marks"
mark() {
    ln -sT "$2" "$XDG_CONFIG_HOME/bb/marks/$1" 2>/dev/null
}
mark home ~
mark root /
mark config "$XDG_CONFIG_HOME"
mark marks "$XDG_CONFIG_HOME/bb/marks"

# Load key bindings
# first check ~/.config/bb/bindings.bb, then /etc/xdg/bb/bindings.bb, then ./bindings.bb
for path in "$XDG_CONFIG_HOME/bb" "$sysconfdir/xdg/bb" "."; do
    cat "$path/bindings.bb" 2>/dev/null && break
done | sed -e '/^#/d' -e "s/^[^ ]/$(printf '\034')+bind:\\0/" | tr '\034' '\0' >> "$BBCMD"
printf '\0' >> "$BBCMD"
