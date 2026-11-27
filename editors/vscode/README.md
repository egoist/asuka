# Asuka for Visual Studio Code

Support for the [Asuka language](../../docs/language/README.md): highlighting,
everything that needs the compiler from its language server, `asuka
lsp`, previews of components shown by `asuka dev`, and the debugger of
the apps `asuka dev` runs.

- **Diagnostics** as you type: the checker's errors, with what would fix
  them, for the package being edited and the packages that import it.
- **Completion**: `x.` (fields, methods, built-in methods, a package's
  names, a type's cases), `.name` where the type is known (`.accent`,
  `.title`, an enum's cases), an element's props, events (`on press`),
  slots and `bind` in its block, argument labels, names in scope,
  keywords, `import` paths.
- **Hover**: a name's declaration and its `///` documentation; an
  element's props and events.
- **Go to definition** and **type definition**, into imported packages,
  the standard library and the built-in elements.
- **Find references**, **rename** and highlights, across the workspace.
- **Formatting** (`asuka fmt`'s), on save by default.
- **Outline**, **workspace symbols**, **signature help** and **semantic
  highlighting**.
- **Previews**: **▶ Preview** above each `preview` block (a code lens)
  shows it on the devices of the `asuka dev` running that file's app, in
  place of the app; **Asuka: Show the App** goes back. `asuka dev` runs
  in a terminal, and the extension finds it by the app's directory; with
  none running, it says to start it. See
  [Previews](../../docs/development.md#previews).

## Install

The extension runs the `asuka` CLI, which must be installed (or built
from this repository: `go build -o ~/go/bin/asuka ./cmd/asuka`).

From this directory:

```sh
npm install
npx @vscode/vsce package     # asuka-0.1.0.vsix
code --install-extension asuka-0.1.0.vsix
```

Or, to work on the extension, open this directory in VS Code and press
F5 (Run Extension) after `npm install`.

## Debugging

The extension debugs the app `asuka dev` runs, on whatever it runs on
(the iOS simulator or an iPhone, Android, this computer; not the web):
breakpoints in `.asuka` files (conditions, hit counts, logpoints),
stopping on errors (uncaught, or all), stepping, the stack and its
variables, and evaluating expressions while paused (hovers, the Watch
view, the debug console).
`asuka dev` is the debug adapter: it listens on `127.0.0.1:8791` (or
the port it prints, or `--debug-port`'s). In **Run and Debug**, add a
configuration:

- **Asuka: Run and Debug** (`"request": "launch"`) runs `asuka dev` for
  the app (`app`, the workspace by default; `platform`, `device`,
  `args`) in a terminal, attaches once it runs, and stops it when the
  session ends.
- **Asuka: Attach to asuka dev** (`"request": "attach"`) attaches to an
  `asuka dev` already running, on `port` (8791).

With no `launch.json`, F5 in an `.asuka` file runs and debugs the
workspace's app on this computer. See [the
debugger](../../docs/development.md#the-debugger) for what it does.

## Settings

| Setting | Default | |
|---|---|---|
| `asuka.server.path` | `asuka` | The `asuka` command: a name on `PATH`, or a path (a relative one is from the workspace's first folder). |
| `asuka.server.args` | `[]` | More arguments for `asuka lsp`: `--verbose` logs to the Asuka output channel. |
| `asuka.trace.server` | `off` | `messages` or `verbose` traces LSP's messages in the Asuka output channel. |

Commands: **Asuka: Show Preview** (the preview at the cursor, as ▶
Preview does), **Asuka: Show the App**, **Asuka: Restart Language
Server** (after rebuilding `asuka`) and **Asuka: Show Language Server
Output**.

## Other editors

Zed has its own extension, [editors/zed](../zed), and
[editors/tree-sitter-asuka](../tree-sitter-asuka) is a tree-sitter
grammar for the highlighting of editors that use one.

Any editor with an LSP client can run `asuka lsp`: it speaks LSP 3.17
over standard input and output. For example, Neovim:

```lua
vim.filetype.add({ extension = { asuka = "asuka" } })
vim.lsp.config("asuka", { cmd = { "asuka", "lsp" }, filetypes = { "asuka" }, root_markers = { "asuka.toml", ".git" } })
vim.lsp.enable("asuka")
```

and Helix (`languages.toml`):

```toml
[language-server.asuka]
command = "asuka"
args = ["lsp"]

[[language]]
name = "asuka"
scope = "source.asuka"
file-types = ["asuka"]
roots = ["asuka.toml"]
comment-token = "//"
indent = { tab-width = 2, unit = "  " }
language-servers = ["asuka"]
```
