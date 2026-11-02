# Colors and syntax highlighting

There are three different ways to color the screen in micro: 16 colors,
256 colors and truecolor (24-bit). If your terminal sets
`COLORTERM=truecolor` (or `24bit`), hex colors are used as-is; otherwise
they are mapped to the closest color of the 256-color palette. You can force
this with the `truecolor` option.

## Colorschemes

To change your colorscheme, press Ctrl-e in micro to bring up the command
prompt, and type:

```
set colorscheme twilight
```

(or whichever colorscheme you choose; press Tab to list them).

All of micro's bundled colorschemes are built in: atom-dark, bubblegum,
cmc-16, cmc-tc, darcula, default, dracula-tc, dukedark-tc, dukelight-tc,
dukeubuntu-tc, geany, gotham, gruvbox, gruvbox-tc, material-tc, monokai,
monokai-dark, one-dark, railscast, simple, solarized, solarized-tc,
sunny-day, twilight and zenburn.

## Creating a colorscheme

Put a file named `name.micro` in `~/.config/micro/colorschemes`. It consists
of lines like

```
color-link comment "bold yellow"
color-link statusline "#282828,#F8F8F2"
include "monokai"
```

The format of a style is `"[attributes] foreground,background"`. Colors can
be one of `black red green yellow blue magenta cyan white` (optionally
prefixed by `bright`), a number from 0 to 255, or a hex value `#RRGGBB`.
Attributes are `bold`, `italic`, `underline`, `reverse`, `dim`, `blink` and
`strikethrough`.

Group names fall back to their parent: `constant.string` uses `constant` if
it isn't defined. Besides the syntax groups (`comment`, `identifier`,
`constant`, `statement`, `symbol`, `preproc`, `type`, `special`,
`underlined`, `error`, `todo`, ...) these UI groups are used:

```
default selection cursor-line color-column line-number current-line-number
statusline statusline.inactive statusline.suggestions tabbar tabbar.active
hlsearch match-brace indent-char trailingws tab-error divider scrollbar
message error-message
```

## Syntax files

Syntax definitions use micro's YAML format and all of micro's syntax files
are built in. You can add or override definitions by placing `name.yaml` in
`~/.config/micro/syntax`:

```yaml
filetype: example

detect:
    filename: "\\.ex$"
    header: "^#!.*example"

rules:
    - statement: "\\b(if|else|while)\\b"
    - constant.string:
        start: "\""
        end: "\""
        skip: "\\\\."
        rules:
            - constant.specialChar: "\\\\."
    - include: "c"
```

Regular expressions use Go's RE2 syntax (no backreferences or lookaround).
