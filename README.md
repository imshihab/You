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
- **Delete** files or directories with `-d`
- **Inspect** files or directories with `-i` (formatted like Node's
  `console.table`, just for visual familiarity)
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
| `-i`                | Get information about a file or directory |

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

# Explicitly mark a path as a directory (trailing "/")
# Only matters at creation time; the slash is stripped before any -d / -i.
you FolderName/

# Delete a file
you index.js -d

# Delete a directory (and its contents)
you index -d

# Show info for a file (size, mime, times, ...)
you index.js -i

# Show info for a directory
you index -i
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
