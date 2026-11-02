# Micro help text

Micro is a terminal-based text editor that aims to be easy to use and
intuitive, while also taking advantage of the full capabilities of modern
terminals. This is a rewrite of micro in C (C99 + the C standard library,
with a small platform layer for the terminal: POSIX, or the Windows console).

To open the command bar, press Ctrl-e. This enables a `>` prompt for typing
commands. From now on when the documentation says to run a command such as
`> help`, this means press Ctrl-e and type `help` (and press enter to execute
the command).

For a list of the default keybindings run `> help defaultkeys`.
For more information on keybindings see `> help keybindings`.

To toggle a short list of important keybindings, press Alt-g.

## Quick-start

Press Ctrl-q to quit, and Ctrl-s to save. Press Ctrl-e to start typing
commands and you can see which commands are available by pressing tab, or by
viewing the help topic `> help commands`.

Move the cursor around with the mouse or with the arrow keys. Enter text
simply by pressing character keys.

If the colorscheme doesn't look good, you can change it with
`> set colorscheme ...`. You can press tab to see the available colorschemes,
or see more information about colorschemes and syntax highlighting with
`> help colors`.

Press Ctrl-w to move between splits, and type `> vsplit filename` or
`> hsplit filename` to open a new split.

## Accessing more help

Micro has a built-in help system which can be accessed with the `> help`
command.

To view help for the various available topics, press Ctrl-e to access command
mode and type in `> help` followed by a topic. Typing just `> help` will open
this page.

Here are the available help topics:

* `defaultkeys`: a list of the default keybindings.
* `commands`: description of each available command.
* `options`: description of each option that can be set.
* `keybindings`: explains how to rebind keys and lists all bindable actions.
* `colors`: explains micro's colorschemes and syntax highlighting.

Press Ctrl-q to close the help split, or Ctrl-g to toggle it.

## About this build

Everything is implemented natively in C: the regular expression engine
(Go/RE2 syntax), the YAML syntax-file loader, all of micro's bundled syntax
definitions and colorschemes (embedded in the binary), multiple cursors,
splits and tabs, macros, undo/redo, crash-recovery backups and the behavior
of micro's default plugins (`autoclose`, `comment`, `ftoptions`, `status`).

Not available: Lua plugins and the plugin manager, the built-in terminal
emulator (`term`), the git diff gutter, and file encodings other than UTF-8.

On Windows, shell commands (`run`, `textfilter`, Ctrl-b) use `cmd.exe`, the
clipboard is the Windows clipboard, new files use CRLF line endings, and
your configuration lives in `%USERPROFILE%\.config\micro`.
