# bb - A bitty browser for command line file management

`bb` is a TUI console file browser that is:

- Extremely lightweight (currently around 1.2K lines of code)
- Highly interoperable with unix pipelines
- Highly customizable and hackable
- Without any build dependencies other than the C standard library (no ncurses)
- A good proof-of-concept for making a TUI without using any libraries

The core idea behind `bb` is that almost all actions are performed by piping
selected files to external scripts, rather than hard-coding actions. Unix
tools are very good at doing file management, the thing that `bb` adds is
immediate visual feedback and rapid navigation.

For example, normally on the command line, if you wanted to manually delete a
handful of files, you would first get a listing of the files with `ls`, then
type `rm` followed by typing out the names of the files (with some tab
autocompletion). With `bb`, you can just launch `bb`, see all the files
immediately, select the ones you want with a few keystrokes, and press `D` to
delete them (or `d` for deleting with confirmation). The `D` key's behavior
is defined in a single line of code in `config.h` to pipe the selected files
to `xargs -0 rm -rf`. That's it! If you want to add a mapping to upload files to
your server, you can just add a binding for `xargs scp user@example.com` or,
if you have some complicated one-time task, you can just hit `|` and type in
any arbitrary command and have the selected files piped to it.

## Zero Dependencies

There's a lot of TUI libraries out there like ncurses and termbox, but
essentially all they do is write ANSI escape sequences to the terminal. `bb`
does all of that by itself, just using basic calls to `write()`, with no
external libraries beyond the C standard library. Since `bb` only has to
support the terminal functionality that it uses itself, `bb`'s entire source
code is less than half the size of the source code for an extremely compact
library like termbox, and less than *half a percent* of the size of the source
code for ncurses. I hope anyone checking out this project can see it as a great
example of how you can build a full TUI without ncurses or any external
libraries as long as you're willing to hand-write a few escape sequences.

## Building

`make`
`sudo make install`

## Usage

Just run `bb` to launch the file browser. Press `?` for a full list of
available key bindings. In short: `h`/`j`/`k`/`l` or arrow keys for navigation,
`q` to quit, <space> to toggle selection, `d` to delete, `c` to copy, `m` to
move, `r` to rename, `n` to create a new file, `N` to create a new directory,
and `|` to pipe files to a command.

## Hacking

If you want to customize `bb`, you can add or change the key bindings by
editing `config.h` and recompiling. In [suckless](https://suckless.org/) style,
customizing means editing source code, and compilation is extremely fast.
Key character constants are in `keys.h` and the rest of the code is in `bb.c`.

## License

`bb` is released under the MIT license. See the `LICENSE` file for full details.
