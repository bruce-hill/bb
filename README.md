# bb - An itty bitty browser for command line file management

`bb` (bitty browser) is a console file browser with a text user interface that is:

- Extremely lightweight (under 2k lines of code)
- Highly interoperable with unix pipelines
- Highly customizable and hackable
- Free of any build dependencies other than the C standard library (no ncurses)
- A good proof-of-concept for making a TUI from scratch

![BB Preview Video](https://bitbucket.org/spilt/bb/downloads/bb.gif)


## Building

`bb` has no build dependencies besides `make` and a C compiler, just:

    make
    sudo make install

To run `bb`, it's expected that you have some basic unix tools: `cat`, `cp`,
`echo`, `find`, `kill`, `less`, `ln`, `mkdir`, `more`, `mv`, `printf`, `read`,
`rm`, `sed`, `sh`, `tput`, `tr`.


## Usage

Run `bb` to launch the file browser. `bb` also has the flags:

- `-d`: when `bb` exits successfully, print the directory `bb` was browsing.
- `-s`: when `bb` exits successfully, print the files that were selected.
- `-0`: use NULL-terminated strings instead of newline-separated strings with
  the `-s` flag.
- `-h`: print usage
- `-v`: print version

Within `bb`, press `?` for a full list of available key bindings. In short:
`h`/`j`/`k`/`l` (or arrow keys) for navigation, `q` to quit, `Enter` to open a
file, `<space>` to toggle selection, `d` to delete, `C` to copy, `Ctrl-v` to
move, `r` to rename, `Ctrl-n` to create a new file or directory, `:` to run a
command with the selected files in `$@`, and `|` to pipe the selected files to
a command.  Pressing `Ctrl-c` will cause `bb` to exit with a failure status and
without printing anything.

More information about usage can also be found by running `man bb` after
installing.


## bb's Philosophy

The core idea behind `bb` is that `bb` is a file **browser**, not a file
**manager**, which means `bb` uses shell scripts to perform all modifications
to the filesystem (passing selected files as arguments), rather than
reinventing the wheel by hard-coding operations like `rm`, `mv`, `cp`, `touch`,
and so on.  Shell scripts can be bound to keypresses in
`~/.config/bb/bbkeys`. For example, `D` is bound to a script that prints a
confirmation message, then runs `rm -rf "$@" && bbcmd deselect refresh`,
which means selecting `file1` and `file2`, then pressing `D` will cause `bb` to
run the shell command `rm -rf file1 file2` and then tell `bb` to deselect all
(now deleted) files and refresh.


## Customizing bb

When `bb` launches, it first updates `bb`'s `$PATH` environment variable to
include, in order, `~/.config/bb/` and `/etc/bb/`. Then, `bb` will run the
command `bbstartup` (the default implementation is found at
[scripts/bbstartup](scripts/bbstartup), along with other default `bb` commands).
`bbstartup` will call `bbkeys` and may also set up configuration options like
which columns to display and what sort order to use. All of these behaviors can
be customized by creating custom local versions of these files in `~/.config/bb/`.
The default versions can be found in `/etc/bb/`.

You can also create temporary bindings at runtime by hitting `Ctrl-b`, pressing
the key you want to bind, and then entering in a script to run (in case you
want to set up an easy way to repeat some custom workflow).


### API

`bb` also exposes an API that allows shell scripts to modify `bb`'s internal
state. To do this, call `bbcmd <your command>` from within `bb`. For example, by
default, `j` is bound to `bbcmd move:+1`, which has the effect of moving `bb`'s
cursor down one item. More details about the API can be found in [the API
documentation](API.md) or by running `man bbcmd` after installing.


## License

`bb` is provided under the MIT license with the [Commons Clause](https://commonsclause.com/)
(you can't sell this software without the developer's permission, but you're
otherwise free to use, modify, and redistribute it free of charge).
See [LICENSE](LICENSE) for details.
