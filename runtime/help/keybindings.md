# Keybindings

Micro has a plethora of hotkeys that make it easy and powerful to use and all
hotkeys are fully customizable to your liking.

Custom keybindings are stored internally in micro if changed with the `> bind`
command or can also be added in the file `~/.config/micro/bindings.json`.

## Rebinding keys

The bindings may be rebound using the `~/.config/micro/bindings.json` file.
Each key is bound to an action.

For example, to bind `Ctrl-y` to undo and `Ctrl-z` to redo, you could put the
following in the `bindings.json` file.

```json
{
    "Ctrl-y": "Undo",
    "Ctrl-z": "Redo"
}
```

Bindings for the command bar go in a `"command"` object, and buffer
bindings may also be grouped in a `"buffer"` object:

```json
{
    "buffer": { "Ctrl-y": "Undo" },
    "command": { "Ctrl-u": "SelectToStart" }
}
```

You can also chain commands when rebinding. For example, if you want `Alt-s`
to save and quit you can bind it like so:

```json
{
    "Alt-s": "Save,Quit"
}
```

Each action will return a success flag. Actions can be chained such that the
chain only continues when there are successes, or failures, or either. The
`,` separator will always chain to the next action. The `|` separator will
abort the chain if the action preceding it succeeds, and the `&` will abort
the chain if the action preceding it fails. For example, in the default
bindings, tab is bound as

```
"Tab": "Autocomplete|IndentSelection|InsertTab"
```

## Binding commands

You can also bind a key to execute a command in command mode (see
`help commands`). Simply prefix the binding with `command:`. For example:

```json
{
    "Alt-p": "command:pwd"
}
```

You can use `command-edit:` instead to open the command bar with the text
pre-filled so you can edit it before running it:

```json
{
    "Alt-p": "command-edit:help "
}
```

## Key names

Modifiers are written `Ctrl-`, `Alt-` and `Shift-` (the dashes are optional,
as in `CtrlShiftLeft`). Named keys:

```
Up Down Right Left Home End PageUp PageDown Insert Delete Backspace
OldBackspace Tab Backtab Enter Esc Space CtrlSpace F1..F64
MouseLeft MouseMiddle MouseRight MouseLeftDrag MouseLeftRelease
MouseWheelUp MouseWheelDown MouseWheelLeft MouseWheelRight
```

Any other single character can be bound, e.g. `Alt-{` or `Ctrl-/`.
Use the `raw` command to see what your terminal sends for a key.

## Bindable actions

```
CursorUp CursorDown CursorPageUp CursorPageDown CursorLeft CursorRight
CursorStart CursorEnd CursorToViewTop CursorToViewCenter CursorToViewBottom
SelectToStart SelectToEnd SelectUp SelectDown SelectLeft SelectRight
WordRight WordLeft SubWordRight SubWordLeft SelectWordRight SelectWordLeft
SelectSubWordRight SelectSubWordLeft DeleteWordRight DeleteWordLeft
DeleteSubWordRight DeleteSubWordLeft SelectLine SelectToStartOfLine
SelectToStartOfText SelectToStartOfTextToggle SelectToEndOfLine
ParagraphPrevious ParagraphNext SelectToParagraphPrevious
SelectToParagraphNext InsertNewline Backspace Delete InsertTab Save SaveAll
SaveAs Find FindLiteral FindNext FindPrevious DiffNext DiffPrevious Center
Undo Redo Copy CopyLine Cut CutLine Duplicate DuplicateLine DeleteLine
MoveLinesUp MoveLinesDown IndentSelection OutdentSelection Autocomplete
CycleAutocompleteBack OutdentLine IndentLine Paste PastePrimary SelectAll
OpenFile Start End PageUp PageDown SelectPageUp SelectPageDown HalfPageUp
HalfPageDown StartOfText StartOfTextToggle StartOfLine EndOfLine ToggleHelp
ToggleKeyMenu ToggleDiffGutter ToggleRuler ToggleHighlightSearch
UnhighlightSearch ResetSearch ClearStatus ShellMode CommandMode
ToggleOverwriteMode Escape Quit QuitAll ForceQuit AddTab PreviousTab NextTab
FirstTab LastTab NextSplit PreviousSplit FirstSplit LastSplit Unsplit VSplit
HSplit ToggleMacro PlayMacro Suspend ScrollUp ScrollDown SpawnMultiCursor
SpawnMultiCursorUp SpawnMultiCursorDown SpawnMultiCursorSelect
RemoveMultiCursor RemoveAllMultiCursors SkipMultiCursor SkipMultiCursorBack
JumpToMatchingBrace JumpLine Deselect ClearInfo None MousePress MouseDrag
MouseRelease MouseMultiCursor Comment
```

Command bar actions (for the `"command"` section): `HistoryUp`,
`HistoryDown`, `ExecuteCommand`, `AbortCommand`, `CommandComplete` and the
cursor/selection/clipboard actions above.

`lua:comment.comment` is accepted as an alias of `Comment` for compatibility
with existing micro configurations.
