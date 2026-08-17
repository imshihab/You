# You

A small, dependency-free C++ CLI tool for creating, inspecting, and deleting
files and directories from the command line. It started as a personal idea for
a Node.js utility, but was eventually written from scratch in C++ and has
nothing to do with the npm package of the same name.

> Single-file build. No external libraries. Cross-platform (Linux, macOS,
> Windows).

---

## Features

- **Create** files or directories with a single short command
- **Create multiple** files or directories in one invocation
- **Brace expansion**: `you store-{a,b,name}.js` creates three files
- **`cd:` chain**: `you cd:src components/Button.tsx index.ts` first creates
  `src/`, then operates inside it; the last `cd:` wins
- **`.youconfig`** lookup: arguments like `$(home)/note.txt` resolve to
  `/home/note.txt` when `home: /home` is in a `.youconfig` file (walked up
  from cwd); if not found, you're prompted to fall back to cwd
- **Delete** with `-d`, `-rf` (file only), `-rd` (dir only)
- **Inspect** files or directories with `-i` (formatted like Node's
  `console.table`, just for visual familiarity)
- **Rename** with `-rn old=new`
- **Move** with `-mv old=new`
- **Copy** with `-c old=new` (file or directory tree)
- **Trash** with `-trash target` — moves target into `./.trash/<timestamp>/`
  and writes a `meta.json` sidecar with the original absolute path so it
  can be restored later
- **Tree view** with `-t[=N]` (default depth 3, skips `node_modules` and
  `.git`; each entry is preceded by a Nerd Font icon (folder, language, image,
  archive, etc.), directories are colored bold blue and get the folder glyph
  in the same color. Both icons and colors are gated on stdout being a TTY —
  set `NO_COLOR` or pipe the output to disable them.)
- **Print working directory** with `-pwd`
- **Open / list** with `-o` — for a directory, prints a text listing; for a
  file, dumps its contents (portable substitute for "open in file explorer")
- **Run commands** with `you run:Folder="cmd1 &&& cmd2"` — chdir into the
  folder (creating it if needed) and run each command in sequence via the
  system shell
- **Interactive settings** with `you --setting` — prompts for editor, trash
  path, tree depth, confirm-on-create and writes a `setting.json`
- **Touch** (update mtime) a file if it already exists
- **Cross-platform** file time handling (Windows `FILETIME`, macOS birth time,
  Linux ctime fallback)
- **MIME-type lookup** for ~70 common file extensions
- **Built-in version detection** by reading `package.json` next to the binary
  (falls back to `1.0.0`)

---

## Build

You only need a C++17 (or newer) compiler.

### Linux / macOS

```bash
g++ -std=c++17 -O2 You.cpp -o you
```

### Install (Linux)

Build, install to `~/.local/bin/`, make it executable, and add it to your
`PATH` (assumes Zsh):

```bash
g++ -std=c++17 -O2 -o you You.cpp
mv you ~/.local/bin/
chmod +x ~/.local/bin/you
echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.zshrc
source ~/.zshrc
```

### Windows (MSVC)

```bat
cl /std:c++17 /O2 /EHsc You.cpp /Fe:you.exe
```

### Windows (MinGW)

```bash
g++ -std=c++17 -O2 You.cpp -o you.exe
```

Optionally copy the resulting binary somewhere on your `PATH` (e.g.
`/usr/local/bin/you` on Unix or `%USERPROFILE%\bin\you.exe` on Windows).

---

## Usage

```text
you [fileName / directoryName] [flag]
```

### Flags

| Flag                | Description                          |
| ------------------- | ------------------------------------ |
| `--help`, `-h`, `--h` | Show the usage message            |
| `--version`, `-v`, `--v` | Display the version           |
| `-d`                | Delete a file or directory           |
| `-rf`               | Delete a file (errors on directory)  |
| `-rd`               | Delete a directory (errors on file) |
| `-i`                | Get information about a file or directory |
| `-o target`         | Show a text listing of a directory (or dump a file) |
| `-t[=N]`            | Tree view of the cwd (default depth 3); skips `node_modules` and `.git`; directories get a trailing `/`, colored bold blue on a TTY. Each entry is preceded by a Nerd Font icon (folder, language, image, archive, ...). `NO_COLOR` or a non-TTY stdout disables both icons and color. |
| `-pwd`              | Print the absolute current working directory |
| `--setting`         | Interactive prompts that write `setting.json` |
| `-rn old=new`       | Rename a file or directory           |
| `-mv old=new`       | Move a file or directory             |
| `-c  old=new`       | Copy a file or directory tree        |
| `-trash target`     | Move target into `./.trash/<timestamp>/` with a `meta.json` sidecar |

### Prefixes (creation only)

| Prefix           | Description                                              |
| ---------------- | -------------------------------------------------------- |
| `name/`          | Trailing `/` => always create a directory                |
| `cd:Folder`      | Chdir into `Folder` for subsequent args; the last `cd:` wins |
| `$(name)/...`    | Resolve via `.youconfig` (walked up from cwd); fallback to cwd with a prompt |

### Examples

```bash
# Create a new file
you index.js

# Create a new directory
you index

# Create multiple files at once
you index.js route.js

# Create multiple directories at once
you index route

# Brace expansion: store-first.js, store-2nd.js, store-name.js
you store-{first,2nd,name}.js

# cd: chain: creates src/, then components/Button.tsx and index.ts inside it
you cd:src components/Button.tsx index.ts

# $(name) prefix: looks up "home" in .youconfig
you $(home)/note.txt

# Explicitly mark a path as a directory (trailing "/")
# Only matters at creation time; the slash is stripped before any -d / -i.
you FolderName/

# Delete a file
you index.js -d

# Delete a directory (and its contents)
you index -d

# Delete only files / only directories with stricter flags
you index.js -rf
you index     -rd

# Rename (note: -rn/-mv/-c take an 'old=new' spec)
you -rn old.txt=new.txt

# Move
you -mv src/a.ts=src/b/a.ts

# Copy
you -c file.txt=copy.txt

# Trash (move to ./.trash/<ts>/)
you secret -trash

# Show info for a file (size, mime, times, ...)
you index.js -i

# Show info for a directory
you index -i

# Text-mode listing of a directory
you src -o

# Tree view (depth 2)
you -t=2

# Print cwd
you -pwd

# Run commands inside a folder (created if missing)
you run:app="npm install &&& npm run dev"

# Interactive settings
you --setting
```

If a file is requested and already exists, its modification time is updated
("touch" semantics). If a directory is requested and already exists, the tool
is a no-op.

---

## Output of `-i`

`-i` prints a Unicode box-drawing table (styled like Node's
`console.table({...})` for visual familiarity):

```text
┌─────────────────┬──────────────────────────┐
│     (index)     │         Values           │
├─────────────────┼──────────────────────────┤
│    FileName     │       'index.js'         │
│    Extension    │         'js'             │
│    MimeType     │   'text/javascript'      │
│      Size       │        '0.00Kib'         │
│     Created     │  '8/2/2026, 2:30:15 PM'  │
│     Changed     │  '8/2/2026, 2:30:15 PM'  │
└─────────────────┴──────────────────────────┘
```

Directories are shown with `null` extension/size and the literal string
`'Directory'` in the `MimeType` column.

---

## Version Output

```text
You/1.0.0 C++: C++17
```

The version is read from the `version` field of a `package.json` located next
to the binary (or one directory up). If no `package.json` is found, it falls
back to `1.0.0`. The C++ standard reported is the one the binary was compiled
with.

---

## How it decides "file vs. directory"

By default, a path is treated as a **file** if its basename contains a `.`
extension (e.g. `index.js`, `route.ts`), and as a **directory** otherwise. A
few special cases:

- `.gitignore` -> treated as a directory-style name (no extension)
- `index`      -> directory
- `index.js`   -> file

A leading `.` (hidden file like `.env`) is also preserved as a file.

### Explicit directory marker (creation only)

A trailing `/` on the argument forces the path to be treated as a directory
**at creation time only**. The slash is stripped before the path is used, so it
is never part of the resulting filesystem entry, and it has no effect when
combined with `-d` or `-i`:

```bash
you FolderName/   # always creates a directory, even if the name has a dot
you Notes.txt/    # creates a directory named "Notes.txt" instead of a file
you Notes.txt/ -d # deletes the directory "Notes.txt" (no special meaning)
you Notes.txt/ -i # prints info for "Notes.txt"   (no special meaning)
```

---

## Project Layout

```
.
├── You.cpp        # Single-file implementation
└── README.md      # This file
```

---

## Implementation Notes

- **Header-only-ish**: Everything lives in `You.cpp` so there's nothing to
  install or configure.
- **Standard library only**: Uses `<filesystem>`, `<chrono>` (indirectly via
  `<ctime>`), `<unordered_map>`, etc. No third-party dependencies.
- **Cross-platform file times**:
  - Windows uses `GetFileAttributesExW` + `FILETIME`.
  - macOS uses `st_birthtimespec`.
  - Linux falls back to `st_ctime` (Linux's `stat` does not expose a portable
    birth time).
- **MIME table** is a hand-rolled `std::unordered_map`. It's intentionally
  not exhaustive; extend the table in `mimeTable()` as needed.

---

## Notes

- **No runtime dependencies**: Pure C++17, nothing to install besides a
  compiler.
- **No globbing or shell expansion**: Arguments are taken literally; wildcards
  like `*` are not interpreted.
- **Single executable**: One binary, no `package.json` required to run (it is
  only consulted for `-v` when present next to the binary).

---

## License

See `LICENSE` if present, or treat this as MIT-licensed unless stated otherwise.
