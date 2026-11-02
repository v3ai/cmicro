# Command bar

The command bar is opened by pressing Ctrl-e. It is a single-line buffer,
meaning that all keybindings from a normal buffer are supported (as well
as mouse and selection).

When running a command, you can use extra syntax that micro will expand
before running the command. To use an argument with a space in it, put it in
quotes. The command bar also has tab completion (commands, option names and
values, help topics, colorschemes, filetypes, action names and file names)
and history (Up / Down).

# Commands

* `bind 'key' 'action'`: creates a keybinding from key to action. See the
  `keybindings` documentation for more information about binding keys.
  This command will modify `bindings.json` and overwrite any bindings to
  `key` that already exist.

* `unbind 'key'`: binds `key` to its default action.

* `help 'topic'`: opens the corresponding help topic. If no topic is
  provided opens the default help screen.

* `save 'filename'`: saves the current buffer. If the filename is provided
  it will 'save as' the filename.

* `quit`: quits micro.

* `goto 'line[:col]'`: goes to the given absolute line (and optionally
  column) number. A negative number can be passed to go inward from the end
  of the file. Example: -5 goes to the 5th-last line in the file.

* `jump 'line[:col]'`: goes to the given relative number from the current
  line (and optionally absolute column) number. Example: -5 jumps 5 lines up.

* `replace 'search' 'value' ['flags']`: This will replace `search` with
  `value`. The `flags` are optional:
  * `-a`: replace all occurrences at once
  * `-l`: do a literal search instead of a regex search

  The `search` and `value` arguments must be quoted if they contain spaces.
  Regex submatches can be used in `value` with `$1`, `${1}` and so on. If
  the cursor has a selection, `replace -a` only works within it. Without
  `-a`, micro asks `Perform replacement (y,n,esc)` for each match.

* `replaceall 'search' 'value'`: this will replace all occurrences of
  `search` with `value` without user confirmation. See `replace` for details.

* `set 'option' 'value'`: sets the option to value. See the `options` help
  topic for a list of options you can set. This will modify your
  `settings.json` with the new value.

* `setlocal 'option' 'value'`: sets the option to value locally (only in the
  current buffer). This will *not* modify `settings.json`.

* `toggle 'option'` / `togglelocal 'option'`: toggles a boolean option.

* `reset 'option'`: resets the given option to its default value.

* `show 'option'`: shows the current value of the given option.

* `showkey 'key'`: shows the action(s) bound to a given key.

* `run 'sh-command'`: runs the given shell command (`sh` on Unix, `cmd.exe`
  on Windows). The command's output will be displayed in one line when it
  finishes running.

* `vsplit 'filename'`: opens a vertical split with `filename`. If no
  filename is provided, a vertical split is opened with an empty buffer.

* `hsplit 'filename'`: same as `vsplit` but opens a horizontal split.

* `tab 'filename'`: opens the given file in a new tab.

* `tabmove '[-+]n'`: moves the active tab to another slot. `n` is an
  integer (starting at 1). If `n` is prefixed with `-` or `+` the move is
  relative.

* `tabswitch 'tab'`: switches to the specified tab, by name or number.

* `textfilter 'sh-command'`: filters the current selection through a shell
  command as standard input and replaces the selection with its output.

* `log`: opens a log of messages and debug statements.

* `reload`: reloads the colorscheme.

* `reopen`: reopens the current file from disk.

* `cd 'path'`: changes the working directory to the given `path`.

* `pwd`: prints the current working directory.

* `open 'filename'`: opens a file in the current buffer.

* `retab`: replaces all leading tabs with spaces or leading spaces with tabs
  depending on the value of `tabstospaces`.

* `raw`: shows the raw escape sequences of key presses (Ctrl-q to stop).

* `comment`: toggles a comment on the current line or selected lines.

* `memusage`: shows the memory used by the current buffer's text.

The following micro commands are not available in this build because they
depend on Lua or on a terminal emulator: `eval`, `plugin`, `term`.

The following commands are provided by the default plugins in micro and are
built into this build: `comment` (Alt-/), autoclosing brackets and quotes,
and per-filetype indentation defaults (`ftoptions`).
