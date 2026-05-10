# uno

`uno` is a small terminal text editor written in a single file (`uno.c`) with
raw-terminal input handling, double-buffered rendering, mouse support, and
language-aware syntax highlighting.

## Build

Compile with `gcc`:

```sh
gcc -O2 -Wall -Wextra -o uno uno.c
```

## Run

```sh
./uno <FILENAME>
```

Behavior:

- If `<FILENAME>` exists, it opens for read/write editing.
- If `<FILENAME>` does not exist, `uno` starts with an empty buffer and creates
  the file on first save (`Ctrl+S`).

## Keyboard and mouse controls

- Printable keys: insert at cursor.
- `Enter`: insert newline.
- `Backspace`: delete one character before cursor.
- `Left` / `Right`: move cursor by character.
- `Up` / `Down`: move cursor by line (preserves preferred column when possible).
- `Ctrl+S`: save file.
- `Ctrl+Q`: quit.
- Mouse wheel: scroll viewport.
- Left click in text area: move cursor to clicked line/column.
- Bracketed paste (`ESC[200~` ... `ESC[201~`): paste blocks safely as text.

## Display and editor model

- Top status bar centered as `Editing <filename>`.
- Scrollable text viewport below status bar.
- Line-number gutter on the left.
- Cursor shown by inverting the current cell colors.
- Automatic cursor visibility management while navigating.
- Terminal resize (`SIGWINCH`) support with full redraw invalidation.
- Bottom status bar (same colors as heading bar) with:
  - left: transient messages (`Pasted clipboard`, `Saved to file ...`) shown for 3 seconds
  - right: cursor + newline mode in `line, col | STYLE` format (e.g. `121, 10 | UNIX`)
- Quit-confirm UI when unsaved changes exist:
  - prompt line appears below the status bar in text-area colors
  - accepts `yes`, `no`, or `esc`

## Syntax highlighting

Highlighting is computed during rendering (the text buffer itself is not
modified) and now uses a consistent palette:

- **Green + bold**: text in double/single quotes (`"..."`, `'...'`)
- **Yellow + bold**: function-like identifiers (token followed by `(`)
- **Dark blue + bold**: datatypes and core keywords
- **Gray + bold**: comments (distinct from editor background)
- **Light blue + bold**: operators and punctuation
- **Amber + bold**: numeric literals

Language coverage:

- **General programming files** (`.c`, `.cpp`, `.h`, `.js`, `.ts`, `.java`,
  `.go`, `.rs`, etc.): generic code-oriented tokenization.
- **Python/Shell** (`.py`, `.sh`, `.bash`, `.zsh`, `.fish`): `#` comments plus
  generic token classes.
- **SQL** (`.sql`): `--` and `/* ... */` comments plus generic token classes.
- **HTML/XML/SVG** (`.html`, `.htm`, `.xml`, `.svg`, ...): basic highlighting
  for tags/keywords, attributes/strings, operators, and `<!-- ... -->` comments.
- **CSS-family** (`.css`, `.scss`, `.less`): basic highlighting for
  declarations/keywords, function-like calls, operators, strings, and comments.
- **Markdown** (`.md`, `.markdown`, `.mdx`): basic highlighting for headings,
  fence markers, inline code spans, operators, and HTML-style comments.

## Internals (from `uno.c`)

- Raw terminal mode via `termios`.
- Mouse tracking enabled with ANSI mouse reporting.
- Double-buffer renderer (`render_front`/`render_back`) that diffs cells and
  emits only changed regions.
- Batched ANSI output into one write buffer per frame for reduced flicker.
- Signal handling:
  - `SIGINT` to exit cleanly
  - `SIGWINCH` to resize/re-render
- On exit, cleanup restores terminal state (cursor, paste mode, mouse mode,
  colors, and screen clear/reset sequence).

## Requirements and limitations

Requirements:

- Unix-like terminal with ANSI escape support.
- A TTY input device (raw mode requires terminal stdin).

Current limitations:

- No undo/redo.
- No search/replace.
- No selection/multi-cursor.
- No multi-file buffers/tabs.
- No syntax-aware editing operations beyond basic highlighting.
