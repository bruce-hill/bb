#!/bin/sh
# This file contains the script that is run when bb launches
# See API.md for details on bb's command API.

# Load key bindings
# first check ~/.config/bb/bindings.bb, then /etc/xdg/bb/bindings.bb, then ./bindings.bb
if [ ! -e "$XDG_CONFIG_HOME/bb/bindings.bb" ] && [ ! -e "$sysconfdir/xdg/bb/bindings.bb" ]; then
    cat "./bindings.bb" 2>/dev/null | sed -e '/^#/d' -e "s/^[^ ]/$(printf '\034')bind:\\0/" | tr '\034' '\0' >> "$BBCMD"
else
    for path in "$sysconfdir/xdg/bb" "$XDG_CONFIG_HOME/bb"; do
        cat "$path/bindings.bb" 2>/dev/null
    done | sed -e '/^#/d' -e "s/^[^ ]/$(printf '\034')bind:\\0/" | tr '\034' '\0' >> "$BBCMD"
fi
printf '\0' >> "$BBCMD"
