# BB's API

In `bb`, all interaction (more or less) occurs through binding keypresses
(and mouse events) to shell scripts. These shell scripts can perform external
actions (like moving files around) or internal actions (like changing the
directory `bb` is displaying). When a shell script runs, `bb` creates a
temporary file, and scripts may write commands to this file to modify `bb`'s
internal state.

## Helper Functions

`bb` is bundled with some helper scripts for performing common tasks. These
scripts are installed to `/etc/bb/`, which is added to `bb`'s `$PATH`
environment variable at runtime. `~/.config/bb/` is also added to the `$PATH`
with higher priority, so you can override any of these scripts by putting your
own version there.

- `bbstartup`: The script run when `bb` first launches. It calls `bbkeys` by
  default and sets up some configuration settings like which columns to display.
- `bbkeys`: The script called by `bb` to create all of `bb`'s key bindings.
  It's currently very hacky, but it amounts to a bunch of calls to `bbcmd
  bind:<key>:<script>`
- `bbshutdown`: The script run when `bb` exits. The default implementation saves
  the current configuration settings to `~/.local/share/bb/settings.sh`, which
  is run by `bbstartup` to restore the settings at launch.
- `bbask [-1] [prompt [initial]]`: Get user input in a standardized and
  customizable way and output it to `STDOUT`.
- `bbcmd <cmd>*`: Modify`bb`'s internal state (see BB Commands).
- `bbconfirm [prompt]`: Display a "Is this okay? [y/N]" prompt and exit with
  failure if the user does not press 'y'.
- `bbpause`: Display a "press any key to continue" message and wait for a keypress.
- `bbpick [prompt]`: Select one of `NULL`-delimited multiple inputs and print it.
- `bbtargets "$BBCMD" "$@"`: If `$BB` is not currently among `$@` (the
  selected files), this script prompts the user to ask whether they want to
  perform an action on the selected files, or on the cursor. The result is
  printed as `cursor` or `selected`.
- `bbunscroll`: Print text to the screen *above* the cursor instead of below it.

## Environment Variables

When `bb` runs scripts, following values are provided as environment variables:

- `$@` (the list of arguments): the full paths of the selected files
- `$BB`: the full path of the file under the cursor
- `$BBDEPTH`: the number of `bb` instances deep (in case you want to run a
  shell and have that shell print something special in the prompt)
- `$BBCMD`: a file to which `bb` commands can be written (used internally)
- `$BBGLOB`: the glob pattern `bb` is using to determine which files to show

## BB Internal State Commands

In order to modify bb's internal state, you can call `bbcmd <cmd>`, where "cmd"
is one of the following commands (or a unique prefix of one):

- `bind:<keys>:<script>`     Bind the given key presses to run the given script
- `cd:<path>`                Navigate to <path>
- `columns:<columns>`        Change which columns are visible, and in what order
- `deselect[:<filename>]`    Deselect <filename> (default: all selected files)
- `fg[:num]`                 Send a background process to the foreground (default: most recent process)
- `glob:<glob pattern>`      The glob pattern for which files to show (default: `*`)
- `goto:<filename>`          Move the cursor to <filename> (changing directory if needed)
- `help`                     Show the help menu
- `interleave[:01]`          Whether or not directories should be interleaved with files in the display (default: toggle)
- `move:<num*>`              Move the cursor a numeric amount
- `quit`                     Quit `bb`
- `refresh`                  Refresh the file listing
- `scroll:<num*>`            Scroll the view a numeric amount
- `select[:<filename>]`      Select <filename> (default: all visible files)
- `sort:([+-]method)+`       Set sorting method (+: normal, -: reverse, default: toggle), additional methods act as tiebreaker
- `spread:<num*>`            Spread the selection state at the cursor
- `toggle[:<filename>]`      Toggle the selection status of <filename> (default: all visible files)

For any of these commands (e.g. `select`), an empty parameter followed by
additional arguments (e.g. `bbcmd select: <file1> <file2> ...`) is equivalent to
repeating the command with each argument (e.g. `bbcmd select:<file1>
select:<file2> ...`).

Note: for numeric-based commands (like scroll), the number can be either an
absolute value or a relative value (starting with `+` or `-`), and/or a percent
(ending with `%`). Scrolling and moving, `%` means percent of screen height,
and `%n` means percent of number of files (e.g. `+50%` means half a screen
height down, and `100%n` means the last file)

## Globbing

`bb` uses glob patterns to determine which files are visible. By default, the
pattern is `*`, which means all files except those starting with a `.` (dot).
`bb`'s globs are POSIX-compliant and do not support bash-style extensions like
`**` for recursive matches. `bb` supports multiple, space-separated globs, so
for example, you can display all files including dotfiles with the glob: `.* *`
(by default, pressing the `.` key will toggle this behavior). The current `bb`
glob is available in `$BBGLOB`, which can be used in scripts if left unquoted.

## Final Notes

Internally, `bbcmd` writes the commands (`NULL` terminated) to a file whose path is
in`$BBCMD` and `bb` reads from that file when it resumes. These commands can also
be passed to `bb` at startup as command line arugments starting with `+`, and
will run immediately.  E.g. `bbcmd +'col:n' +'sort:+r' .` will launch `bb` only
showing the name column, randomly sorted.

`bb` also optimizes any scripts that only contain just a `bbcmd` command and no
shell variables, other commands, etc. (e.g. `bbcmd move:+1`) These
`bbcmd`-command-only scripts directly modify `bb`'s internal state without
spawning a shell, so they're much faster and avoid flickering the screen.
