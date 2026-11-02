# Default Keys

Below are simple charts of the default hotkeys and their functions. For more
information about binding custom hotkeys or changing default bindings, please
run `> help keybindings`.

### Power user

| Key       | Description of function                                          |
|---------- |----------------------------------------------------------------- |
| Ctrl-e    | Open a command prompt for running commands (see `> help commands`) |
| Tab       | In command prompt, autocomplete the current command or argument  |
| Ctrl-b    | Run a shell command (this will close micro while it is running)  |

### Navigation

| Key                         | Description of function                       |
|---------------------------- |---------------------------------------------- |
| Arrows                      | Move the cursor around                        |
| Shift-arrows                | Move and select text                          |
| Alt(Ctrl on Mac)-LeftArrow  | Move to the beginning of the current line     |
| Alt(Ctrl on Mac)-RightArrow | Move to the end of the current line           |
| Home                        | Move to the start of text on the current line |
| End                         | Move to the end of the current line           |
| Ctrl(Alt on Mac)-LeftArrow  | Move cursor one word left                     |
| Ctrl(Alt on Mac)-RightArrow | Move cursor one word right                    |
| Alt-{                       | Move cursor to previous empty line            |
| Alt-}                       | Move cursor to next empty line                |
| PageUp                      | Move cursor up one page                       |
| PageDown                    | Move cursor down one page                     |
| Ctrl-Home or Ctrl-UpArrow   | Move cursor to start of document              |
| Ctrl-End or Ctrl-DownArrow  | Move cursor to end of document                |
| Ctrl-l                      | Jump to a line in the file (prompts)          |
| Ctrl-w                      | Cycle between splits in the current tab       |
| Ctrl-t                      | Open a new tab                                |
| Alt-, and Alt-.             | Previous / next tab                           |
| Ctrl-PageUp / Ctrl-PageDown | Previous / next tab                           |

### Find Operations

| Key       | Description of function                   |
|---------- |------------------------------------------ |
| Ctrl-f    | Find (opens prompt)                       |
| Alt-F     | Find literal (no regex)                   |
| Ctrl-n    | Find next instance of current search      |
| Ctrl-p    | Find previous instance of current search  |

### File Operations

| Key       | Description of function                                          |
|---------- |----------------------------------------------------------------- |
| Ctrl-q    | Close current file (quits micro if this is the last file open)   |
| Ctrl-o    | Open a file (prompts for filename)                               |
| Ctrl-s    | Save current file                                                |

### Text operations

| Key                               | Description of function                   |
|---------------------------------- |------------------------------------------ |
| Ctrl(Alt on Mac)-Shift-RightArrow | Select word right                         |
| Ctrl(Alt on Mac)-Shift-LeftArrow  | Select word left                          |
| Alt(Ctrl on Mac)-Shift-LeftArrow  | Select to start of current line           |
| Alt(Ctrl on Mac)-Shift-RightArrow | Select to end of current line             |
| Shift-Home                        | Select to start of current line           |
| Shift-End                         | Select to end of current line             |
| Ctrl-Shift-UpArrow                | Select to start of file                   |
| Ctrl-Shift-DownArrow              | Select to end of file                     |
| Ctrl-x                            | Cut selected text (or the current line)   |
| Ctrl-c                            | Copy selected text (or the current line)  |
| Ctrl-v                            | Paste                                     |
| Ctrl-k                            | Cut current line                          |
| Ctrl-d                            | Duplicate current line                    |
| Ctrl-z                            | Undo                                      |
| Ctrl-y                            | Redo                                      |
| Alt-UpArrow                       | Move current line or selected lines up    |
| Alt-DownArrow                     | Move current line or selected lines down  |
| Alt-Backspace or Alt-Ctrl-h       | Delete a word left                        |
| Ctrl-a                            | Select all                                |
| Tab                               | Indent selection / autocomplete a word    |
| Shift-Tab                         | Unindent selection                        |
| Alt-/ or Ctrl-_                   | Toggle comment on line(s)                 |

### Macros

| Key       | Description of function                                            |
|---------- |------------------------------------------------------------------- |
| Ctrl-u    | Toggle macro recording (press Ctrl-u to start, Ctrl-u again to stop) |
| Ctrl-j    | Run latest recorded macro                                          |

### Multiple cursors

| Key              | Description of function                                    |
|----------------- |----------------------------------------------------------- |
| Alt-n            | Create new multiple cursor from selection (selects word)   |
| Alt-Shift-Up     | Spawn a new cursor on the line above the current one       |
| Alt-Shift-Down   | Spawn a new cursor on the line below the current one       |
| Alt-m            | Spawn a cursor at the start of each line of the selection  |
| Alt-p            | Remove latest multiple cursor                              |
| Alt-c            | Remove all multiple cursors (cancel)                       |
| Alt-x            | Skip multiple cursor selection                             |
| Ctrl-MouseLeft   | Place a multiple cursor at any location                    |

### Other

| Key       | Description of function                                          |
|---------- |----------------------------------------------------------------- |
| Ctrl-g    | Open help file                                                   |
| Ctrl-r    | Toggle the line number ruler                                     |
| Alt-g     | Toggle key menu                                                  |
| Insert    | Toggle overwrite mode                                            |

### Emacs style actions

| Key       | Description of function   |
|---------- |-------------------------- |
| Alt-f     | Next word                 |
| Alt-b     | Previous word             |
| Alt-a     | Move to start of line     |
| Alt-e     | Move to end of line       |

### Function keys

| Key       | Description of function    |
|---------- |--------------------------- |
| F2        | Save                       |
| F3        | Find                       |
| F4        | Quit                       |
| F7        | Find                       |
| F10       | Quit                       |
| Esc       | Escape (deselect, clear message, remove cursors, unhighlight search) |
