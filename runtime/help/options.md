# Options

Micro stores all of the user configuration in its configuration directory.

Micro uses `$MICRO_CONFIG_HOME` as the configuration directory. If this
environment variable is not set, it uses `$XDG_CONFIG_HOME/micro` instead. If
that environment variable is not set, it uses `~/.config/micro` as the
configuration directory. In the documentation, we use `~/.config/micro` to
refer to the configuration directory (even if it may in fact be somewhere
else if you have set either of the above environment variables).

Here are the available options:

* `autoclose`: automatically close brackets and quotes. Default: `true`
* `autoindent`: when creating a new line, use the same indentation as the
  previous line. Default: `true`
* `autosave`: automatically save the buffer every n seconds, where n is the
  value of the autosave option. `0` disables it. Default: `0`
* `autosu`: when a file is saved that the user doesn't have permission to
  modify, micro will ask if the user would like to use `sucmd`. If `autosu`
  is enabled, micro will automatically do this. Default: `false`
* `backup`: micro will automatically keep backups of all open buffers every
  8 seconds. Backups are stored in `~/.config/micro/backups` and are removed
  when the buffer is closed cleanly. If micro crashes, you are asked whether
  to recover the backup the next time you open the file. Default: `true`
* `backupdir`: the directory micro should place backups in. Default: `""`
* `basename`: in the infobar and tabbar, show only the basename of the file
  being edited rather than the full path. Default: `false`
* `clipboard`: specifies how micro should access the system clipboard.
  `external` uses an external tool (wl-copy/wl-paste, xclip, xsel, pbcopy/
  pbpaste) when available, `terminal` uses the OSC 52 escape sequence for
  copying, and `internal` uses an internal clipboard. Default: `external`
* `colorcolumn`: if this is not set to 0, it will display a column at the
  specified column. Default: `0`
* `colorscheme`: loads the colorscheme stored in
  `$(configDir)/colorschemes/option.micro` or a built-in one. Default: `default`
* `comment`: enable the comment toggling binding (Alt-/). Default: `true`
* `commenttype`: the comment format for the current buffer, e.g. `// %s`.
  Default: chosen by filetype.
* `cursorline`: highlight the line that the cursor is on. Default: `true`
* `detectlimit`: number of lines scanned for a syntax signature. Default: `100`
* `divchars`: specifies the "divider" characters used for vertical splits.
  Default: `"|-"`
* `divreverse`: colorschemes provide the color for the divider; this option
  reverses it. Default: `true`
* `eofnewline`: micro will automatically add a newline to the end of the
  file if one does not exist. Default: `true`
* `fileformat`: this determines what kind of line endings micro will use for
  the file. `unix` or `dos`. Detected automatically. Default: `unix`
* `filetype`: sets the filetype for the current buffer. Set this option to
  `off` to completely disable filetype detection. Default: `unknown`
* `ftoptions`: per-filetype indentation defaults (Go and Makefiles use tabs;
  Python, YAML, Nim and Fish use spaces). Default: `true`
* `helpsplit`: sets the split type to be used by the help command. `hsplit`
  or `vsplit`. Default: `hsplit`
* `hlsearch`: highlight all instances of the searched text after a
  successful search. Default: `false`
* `hltaberrors`: highlight tabs when spaces are expected. Default: `false`
* `hltrailingws`: highlight trailing whitespaces. Default: `false`
* `ignorecase`: perform case-insensitive searches. Default: `true`
* `incsearch`: enable incremental search in "Find" prompt. Default: `true`
* `indentchar`: sets the indentation character. Default: `" "`
* `infobar`: enables the line at the bottom of the editor where messages are
  printed. Default: `true`
* `keepautoindent`: when using autoindent, whitespace is deleted if you make
  a newline and don't insert anything; this option keeps it. Default: `false`
* `keymenu`: display the nano-style key menu at the bottom. Default: `false`
* `lockbindings`: prevent changing bindings. Default: `false`
* `matchbrace`: underline matching braces for '()', '{}', '[]'. Default: `true`
* `matchbraceleft`: also match the brace to the left of the cursor.
  Default: `true`
* `matchbracestyle`: `underline` or `highlight` (uses the `match-brace`
  color). Default: `underline`
* `mkparents`: if a file is opened on a path that does not exist, create the
  directories when saving. Default: `false`
* `mouse`: mouse support. Default: `true`
* `multiopen`: specifies how to layout multiple files opened at startup.
  `tab`, `vsplit` or `hsplit`. Default: `tab`
* `pageoverlap`: lines kept when paging up or down. Default: `2`
* `parsecursor`: if enabled, `file.txt:10:5` opens at line 10, column 5.
  Default: `false`
* `paste`: treat characters sent from the terminal in a single chunk as a
  paste event. Disables autoindent and autoclose. Default: `false`
* `permbackup`: keep backups permanently. Default: `false`
* `readonly`: when enabled, disallows edits to the buffer. Default: `false`
* `relativeruler`: make line numbers display relatively. Default: `false`
* `reload`: controls the reload behavior of the current buffer in case the
  file has changed on disk: `prompt`, `auto` or `disabled`. Default: `prompt`
* `rmtrailingws`: micro will remove trailing whitespace when saving.
  Default: `false`
* `ruler`: display line numbers. Default: `true`
* `savecursor`: remember where the cursor was last time the file was opened.
  Default: `false`
* `savehistory`: remember command history between sessions. Default: `true`
* `scrollbar`: display a scroll bar. Default: `false`
* `scrollbarchar`: the scrollbar character. Default: `"|"`
* `scrollmargin`: margin at which the view starts scrolling when the cursor
  approaches the edge of the view. Default: `3`
* `scrollspeed`: amount of lines to scroll for one scroll event. Default: `2`
* `showchars`: show tabs, spaces or trailing spaces with visible characters,
  e.g. `tab=>,space=.,trailing=~`. Default: `""`
* `smartpaste`: add leading whitespace when pasting multiple lines.
  Default: `true`
* `softwrap`: wrap lines that are too long to fit on the screen.
  Default: `false`
* `splitbottom`: when a horizontal split is created, create it below the
  current split. Default: `true`
* `splitright`: when a vertical split is created, create it to the right of
  the current split. Default: `true`
* `statusformatl` / `statusformatr`: format strings of the left and right
  side of the statusline. Available: `$(filename)`, `$(modified)`,
  `$(overwrite)`, `$(line)`, `$(col)`, `$(lines)`, `$(percentage)`,
  `$(opt:name)`, `$(bind:Action)`, `$(status.paste)`, `$(status.lines)`,
  `$(status.vcol)`, `$(status.bytes)`, `$(status.size)`.
* `statusline`: display the status line at the bottom of the screen.
  Default: `true`
* `sucmd`: specifies the super user command (`sudo`, `doas`). Default: `sudo`
* `syntax`: enables syntax highlighting. Default: `true`
* `tabalways`: always show the tab bar. Default: `false`
* `tabhighlight` / `tabreverse`: tab bar styling. Default: `false` / `true`
* `tabmovement`: navigate spaces at the beginning of lines as if they are
  tabs. Default: `false`
* `tabsize`: the size in spaces that a tab character should be displayed
  with. Default: `4`
* `tabstospaces`: use spaces instead of tabs. Default: `false`
* `truecolor`: `auto`, `on` or `off`. Use 24-bit colors when the terminal
  supports them (`COLORTERM=truecolor`); otherwise hex colors are mapped to
  the 256-color palette. Default: `auto`
* `wordwrap`: wrap long lines by words. Requires `softwrap`. Default: `false`

Accepted for compatibility but without effect in this build: `diffgutter`,
`encoding` (UTF-8 only), `fakecursor`, `fastdirty`, `saveundo`,
`useprimary`, `xterm`.

---

## Settings.json file

The settings.json file should go in your configuration directory and should
contain only options which have been modified from their default setting:

```json
{
    "colorscheme": "solarized",
    "tabsize": 2,
    "ft:python": {
        "tabsize": 4
    },
    "*.md": {
        "softwrap": true
    }
}
```

Keys of the form `ft:filetype` apply to buffers of that filetype; other
nested objects (or `glob:pattern`) apply to files whose path matches the
glob. Comments and trailing commas are allowed.
