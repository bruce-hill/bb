# bb - An itty bitty browser for command line file management

`bb` (bitty browser) is a console file browser with a text user interface that is:

- Extremely lightweight (under 2k lines of code)
- Highly interoperable with unix pipelines
- Highly customizable and hackable
- Free of any build dependencies other than the C standard library (no ncurses)
- A good proof-of-concept for making a TUI from scratch

![BB Preview Video](https://bitbucket.org/spilt/bb/downloads/bb.gif)

## Building

No dependencies besides `make` and a C compiler, just:

    make
    sudo make install

To run `bb`, it's expected that you have some basic unix tools:
`basename`, `cat`, `cp`, `echo`, `find`, `grep`, `mkdir`, `more`, `mv`,
`printf`, `read`, `rm`, `sed`, `sh`, `sleep`, `touch`, `tput`, `tr`, `wait`.

## Usage

Run `bb` to launch the file browser. `bb` also has the flags:

- `-d`: when `bb` exits successfully, print the directory `bb` was browsing
  (see the section on "Changing Directories with bb" in the FAQ below).
- `-s`: when `bb` exits successfully, print the files that were selected.
- `-0`: use NULL-terminated strings instead of newline-separated strings with
  the `-s` flag.
- `-h`: print usage
- `-v`: print version

Within `bb`, press `?` for a full list of available key bindings. In short:
`h`/`j`/`k`/`l` or arrow keys for navigation, `q` to quit, `<space>` to toggle
selection, `d` to delete, `c` to copy, `Ctrl-v` to move, `r` to rename,
`Ctrl-n` to create a new file or directory, `:` to run a command with the
selected files in `$@`, and `|` to pipe the selected files to a command.
Pressing `Ctrl-c` will cause `bb` to exit with a failure status and without
printing anything.

## bb's Philosophy

The core idea behind `bb` is that `bb` is a file **browser**, not a file
**manager**, which means `bb` uses shell scripts to perform all modifications
to the filesystem (passing selected files as arguments), rather than
reinventing the wheel by hard-coding operations like `rm`, `mv`, `cp`, `touch`,
and so on.  Shell scripts can be bound to keypresses in
`~/.config/bb/bindings.bb`. For example, `p` is bound to `$PAGER "$@"`, which
means selecting `file1` and `file2`, then pressing `p` will cause `bb` to run
the shell command `$PAGER file1 file2`.

## Customizing bb

`bb` comes with a bunch of pre-defined bindings for basic actions in
[bindings.bb](bindings.bb) (installed to `/etc/xdg/bb/bindings.bb`) (within
`bb`, press `?` to see descriptions of the bindings), but it's very easy to add
new bindings for whatever custom scripts you want to run, just `mkdir -p
~/.config/bb && cp -n /etc/xdg/bb/bindings.bb ~/.config/bb/` and edit
`~/.config/bb/bindings.bb` to have your new bindings. You can also create
bindings at runtime by hitting `Ctrl-b` (in case you want to set up an easy way
to repeat some custom workflow).

### API

`bb` also exposes an API that allows shell scripts to modify `bb`'s internal
state. To do this, call `bb +<your command>` from within `bb`. For example, by
default, `j` is bound to `bb +move:+1`, which has the effect of moving `bb`'s
cursor down one item. More details about the API can be found in [the API
documentation](API.md).

## FAQ

### Using bb to Change Directory

Applications cannot change the shell's working directory on their own, but you
can define a shell function that uses the shell's builtin `cd` function on the
output of `bb -d` (print directory on exit). For bash (sh, zsh, etc.), you can
put the following function in your `~/.profile` (or `~/.bashrc`, `~/.zshrc`,
etc.):

    function bcd() { cd "$(bb -d "$@")"; }

For [fish](https://fishshell.com/) (v3.0.0+), you can put this in your
`~/.config/fish/functions/`:

    function bcd; cd (bb -d $argv); end

In both versions, the directory will not change if `bb` exits with failure
(e.g. by pressing `Ctrl-c`).

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
