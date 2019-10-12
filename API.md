# BB's API

In `bb`, all interaction (more or less) occurs through binding keypresses
(and mouse events) to shell scripts. These shell scripts can perform external
actions (like moving files around) or internal actions (like changing the
directory `bb` is displaying). When a shell script runs, `bb` creates a
temporary file, and scripts may write commands to this file to modify `bb`'s
internal state.

## Helper Functions

- `bb`: used for modifying `bb`'s internal state (see BB Commands).
- `ask`: get user input in a standardized and customizable way. The first
  argument is a variable where the value is stored. The second argument is
  a prompt. A third optional argument can provide a default value (may be
  ignored).
- `ask1`: get a single character of user input. The first argument is a variable
  where the input will be stored and the second argument is a prompt.
- `pause`: Display a "press any key to continue" message and wait for a keypress.
- `confirm`: Display a "Is this okay? [y/N]" prompt and exit with failure if
  the user does not press 'y'.
- `spin`: Display a spinning icon while a slow command executes in the background.
  (e.g. `spin sleep 5`).

## Environment Variables

For startup commands and key bindings, the following values are provided as
environment variables:

- `$@` (the list of arguments): the full paths of the selected files
- `$BBCURSOR`: the full path of the file under the cursor
- `$BBDOTFILES`: "1" if files beginning with "." are visible in bb, otherwise ""
- `$BB_DEPTH`: the number of `bb` instances deep (in case you want to run a
  shell and have that shell print something special in the prompt)
- `$BBCMD`: a file to which `bb` commands can be written (used internally)

## BB Internal State Commands

In order to modify bb's internal state, you can call `bb +cmd`, where "cmd"
is one of the following commands (or a unique prefix of one):

- `.:[01]`                   Whether to show "." in each directory
- `..:[01]`                  Whether to show ".." in each directory
- `cd:<path>`                Navigate to <path>
- `columns:<columns>`        Change which columns are visible, and in what order
- `deselect:<filename>`      Deselect <filename>
- `dotfiles:[01]`            Whether dotfiles are visible
- `goto:<filename>`          Move the cursor to <filename> (changing directory if needed)
- `interleave:[01]`          Whether or not directories should be interleaved with files in the display
- `kill`                     Exit with failure
- `move:<num*>`              Move the cursor a numeric amount
- `quit`                     Quit bb
- `refresh`                  Refresh the file listing
- `scroll:<num*>`            Scroll the view a numeric amount
- `select:<filename>`        Select <filename>
- `sort:([+-]method)+`       Set sorting method (+: normal, -: reverse), additional methods act as tiebreaker
- `spread:<num*>`            Spread the selection state at the cursor
- `suspend`                  Suspend `bb` (SIGSTP signal)
- `toggle:<filename>`        Toggle the selection status of <filename>

For any of these commands (e.g. `+select`), an empty parameter followed by
additional arguments (e.g. `bb +select: <file1> <file2> ...`) is equivalent to
repeating the command with each argument (e.g. `bb +select:<file1>
+select:<file2> ...`).

Note: for numeric-based commands (like scroll), the number can be either an
absolute value or a relative value (starting with `+` or `-`), and/or a percent
(ending with `%`). Scrolling and moving, `%` means percent of screen height,
and `%n` means percent of number of files (e.g. `+50%` means half a screen
height down, and `100%n` means the last file)

## Final Notes

Internally, `bb` writes the commands (NUL terminated) to a file whose path is
in`$BBCMD` and reads from that file when `bb` resumes. These commands can also
be passed to bb at startup, and will run immediately.  E.g. `bb '+col:n'
'+sort:+r' .` will launch `bb` only showing the name column, randomly sorted.

`bb` also optimizes any scripts that only contain just a `bb` command and no
shell variables, other commands, etc. (e.g. `bb +move:+1`) These
`bb`-command-only scripts directly modify `bb`'s internal state without
spawning a shell, so they're much faster and avoid flickering the screen.
