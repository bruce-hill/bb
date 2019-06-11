# bb - An itty bitty browser for command line file management

`bb` (bitty browser) is a TUI console file browser that is:

- Extremely lightweight (under 2k lines of code)
- Highly interoperable with unix pipelines
- Highly customizable and hackable
- Without any build dependencies other than the C standard library (no ncurses)
- A good proof-of-concept for making a TUI from scratch

## Building
No dependencies, just:

`make`
`sudo make install`

## Usage

Run `bb` to launch the file browser. `bb` also has the flags:

- `-d`: when `bb` exits successfully, print the directory `bb` was browsing.
- `-s`: when `bb` exits successfully, print the files that were selected.
- `-0`: use NULL-terminated strings instead of newline-separated strings with
  the `-s` flag.

Within `bb`, press `?` for a full list of available key bindings. In short:
`h`/`j`/`k`/`l` or arrow keys for navigation, `q` to quit, `<space>` to toggle
selection, `d` to delete, `c` to copy, `M` to move, `r` to rename, `n` to
create a new file, `N` to create a new directory, `:` to run a command with the
selected files in `$@`, and `|` to pipe files to a command. Pressing `Ctrl-c`
will cause `bb` to exit with a failure status and without printing anything.

## Using bb to Change Directory
Applications cannot change the shell's working directory on their own, but you
can define a shell function that uses the shell's builtin `cd` function on the
output of `bb -d` (print directory on exit). For bash (sh, zsh, etc.), you can
put the following function in your `~/.profile` (or `~/.bashrc`, `~/.zshrc`,
etc.):

    function bcd() { cd "$(bb -d "$@" || pwd)"; }

For [fish](https://fishshell.com/) (v3.0.0+), you can put this in your
`~/.config/fish/functions/`:

    function bcd; cd (bb -d $argv || pwd); end

In both versions, `|| pwd` means the directory will not change if `bb` exits
with failure (e.g. by pressing `Ctrl-c`).

## Launching bb with a Keyboard Shortcut
Using a keyboard shortcut to launch `bb` from the shell is something that is
handled by your shell. Here are some examples for binding `Ctrl-b` to launch
`bb` and change directory to `bb`'s directory (using the `bcd` function defined
above). For sh and bash, put this in your `~/.profile`:

    bind '"\C-b":"bcd\n"'

For fish, put this in your `~/.config/fish/functions/fish_user_key_bindings.fish`:

    bind \cB 'bcd; commandline -f repaint'

# bb's Philosophy
The core idea behind `bb` is that `bb` is a file **browser**, not a file
**manager**, which means `bb` uses shell scripts to perform all modifications
to the filesystem (passing selected files as arguments), rather than
reinventing the wheel by hard-coding operations like `rm`, `mv`, `cp`, `touch`,
and so on.  Shell scripts can be bound to keypresses in config.h (the user's
version of [the defaults in config.def.h](config.def.h)). For example, `D` is
bound to `rm -rf "$@"`, which means selecting `file_foo` and `dir_baz`, then
pressing `D` will cause `bb` to run the shell command `rm -rf file_foo dir_baz`.

`bb` comes with a bunch of pre-defined bindings for basic actions in
[config.def.h](config.def.h) (within `bb`, press `?` to see descriptions of the
bindings), but it's very easy to add new bindings for whatever custom scripts
you want to run, just add a new entry in `bindings` in config.h with the form
`{{keys...}, "<shell command>", "<description>"}` The description is shown when
pressing `?` within `bb`.

## User Input in Scripts
If you need user input in a script, you can just use the `read` shell function
like so: `read -p "Archive: " name && zip "$name" "$@"` However, `read` doesn't
support a lot of functionality (e.g. using the arrow keys), so I would recommnd
using [ask](https://bitbucket.org/spilt/ask) instead. If you have `ask`
isntalled, making `bb` will automatically detect it and the default key
bindings will use it instead of `read`.

## API
`bb` also exposes an API so that programs can modify `bb`'s internal state.
For example, by default, `f` is bound to `bb "+goto:$(fzf)"`, which has the
effect of moving `bb`'s cursor to whatever `fzf` (a fuzzy finder) prints out.
More details about the API can be found in [the config file](config.def.h).

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


## Hacking

If you want to customize `bb`, you can add or change the key bindings by
editing `config.h` and recompiling. In [suckless](https://suckless.org/) style,
customizing means editing source code, and compilation is extremely fast.
Key character constants are in `keys.h` and the rest of the code is in `bb.c`.

## License

`bb` is released under the MIT license. See the `LICENSE` file for full details.
