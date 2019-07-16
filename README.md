# bb - An itty bitty browser for command line file management

`bb` (bitty browser) is a TUI console file browser that is:

- Extremely lightweight (under 2k lines of code)
- Highly interoperable with unix pipelines
- Highly customizable and hackable
- Free of any build dependencies other than the C standard library (no ncurses)
- A good proof-of-concept for making a TUI from scratch

## Building
No dependencies, just:

    make
    sudo make install

## Usage

Run `bb` to launch the file browser. `bb` also has the flags:

- `-d`: when `bb` exits successfully, print the directory `bb` was browsing
  (see the section on "Changing Directories with bb" in the FAQ below).
- `-s`: when `bb` exits successfully, print the files that were selected.
- `-0`: use NULL-terminated strings instead of newline-separated strings with
  the `-s` flag.

Within `bb`, press `?` for a full list of available key bindings. In short:
`h`/`j`/`k`/`l` or arrow keys for navigation, `q` to quit, `<space>` to toggle
selection, `d` to delete, `c` to copy, `M` to move, `r` to rename, `n` to
create a new file, `N` to create a new directory, `:` to run a command with the
selected files in `$@`, and `|` to pipe files to a command. Pressing `Ctrl-c`
will cause `bb` to exit with a failure status and without printing anything.

## bb's Philosophy
The core idea behind `bb` is that `bb` is a file **browser**, not a file
**manager**, which means `bb` uses shell scripts to perform all modifications
to the filesystem (passing selected files as arguments), rather than
reinventing the wheel by hard-coding operations like `rm`, `mv`, `cp`, `touch`,
and so on.  Shell scripts can be bound to keypresses in config.h (the user's
version of [the defaults in config.def.h](config.def.h)). For example, `D` is
bound to `rm -rf "$@"`, which means selecting `file_foo` and `dir_baz`, then
pressing `D` will cause `bb` to run the shell command `rm -rf file_foo dir_baz`.

## Customizing bb
`bb` comes with a bunch of pre-defined bindings for basic actions in
[config.def.h](config.def.h) (within `bb`, press `?` to see descriptions of the
bindings), but it's very easy to add new bindings for whatever custom scripts
you want to run, just add a new entry in `bindings` in config.h with the form
`{{keys...}, "<shell command>", "<description>"}` The description is shown when
pressing `?` within `bb`.

### User Input in Scripts
If you need user input in a binding, you can use the `ASK(var, prompt,
initial)` and `ASKECHO(prompt, initial)` macros, which internally use the
`read` shell function (`initial` is discarded) or the `ask` tool, if `USE_ASK`
is set to 1. This is used in a few key bindings by default, including `n` and
`:`.

### Fuzzy Finding
To select from a list of options with a fuzzy finder in a binding, you can use
the `PICK` macro. During the `make` process, you can use `PICKER=fzy`,
`PICKER=fzf`, `PICKER=dmenu`, or `PICKER=ask` to choose which fuzzy finder
program `bb` will use (and provide some default arguments). This is used in the
`/` and `Ctrl-f` key bindings by default.

### API
`bb` also exposes an API that allows shell scripts to modify `bb`'s internal
state. To do this, call `bb +<your command>` from within `bb`. For example, by
default, `j` is bound to `bb '+move:+1'`, which has the effect of moving `bb`'s
cursor down one item. More details about the API can be found in [the config
file](config.def.h).

## FAQ
### Using bb to Change Directory
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

### Launching bb with a Keyboard Shortcut
Using a keyboard shortcut to launch `bb` from the shell is something that is
handled by your shell. Here are some examples for binding `Ctrl-b` to launch
`bb` and change directory to `bb`'s directory (using the `bcd` function defined
above). For sh and bash, put this in your `~/.profile`:

    bind '"\C-b":"bcd\n"'

For fish, put this in your `~/.config/fish/functions/fish_user_key_bindings.fish`:

    bind \cB 'bcd; commandline -f repaint'

## License
`bb` is released under the MIT license. See the `LICENSE` file for full details.
