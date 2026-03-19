# Fuoricode

A command-line tool that exports codebases into single Markdown artifacts optimized for LLM context and code review workflows.

```bash
fuori --staged -o review.md
```

Turns your messy working tree into clean, auditable context. Zero copy-paste, zero noise.

## When to use it

- Pack codebase context for an LLM without pasting files by hand
- Review staged changes as one artifact before a commit or PR
- Snapshot a subtree for debugging, discussion, or handoff

## How it works

1. Select files from Git, the filesystem, or stdin depending on the mode
2. Strip out anything non-exportable such as binaries, empty files, oversized files, symlinks
3. Render one Markdown artifact with a project tree, per-file headings, and fenced code blocks

## Features

- LLM context packing: export a codebase into one Markdown artifact, ready for any AI assistant
- Auto-detects Git worktrees by default and falls back to recursive filesystem scanning elsewhere
- Git-aware file selection via the default worktree mode, `--staged`, `--unstaged`, or `--diff <range>`
- Caller-supplied file selection via `--from-stdin`, with optional NUL-delimited parsing via `-0` / `--null`
- Respects `.gitignore` rules in Git-backed modes and local ignore rules in filesystem mode
- Includes a project tree that matches the exported artifact
- Binary file auto-detection and exclusion
- Configurable file size cap (default: 100 KB)
- Formats code in Markdown with appropriate language identifiers
- Estimates final artifact token usage with a chars/token heuristic
- Warns or hard-fails when estimated tokens exceed a configurable threshold
- Outputs to `_export.md` by default (configurable, including stdout)
- Simple C implementation that is easy to inspect and review
- Zero dependencies, plain C99 + POSIX, compiles in seconds on almost anything

Design notes live in [`docs/design.md`](docs/design.md).

[![DeepWiki](https://img.shields.io/badge/DeepWiki-hyle%2Ffuoricode-blue.svg?logo=data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAACwAAAAyCAYAAAAnWDnqAAAAAXNSR0IArs4c6QAAA05JREFUaEPtmUtyEzEQhtWTQyQLHNak2AB7ZnyXZMEjXMGeK/AIi+QuHrMnbChYY7MIh8g01fJoopFb0uhhEqqcbWTp06/uv1saEDv4O3n3dV60RfP947Mm9/SQc0ICFQgzfc4CYZoTPAswgSJCCUJUnAAoRHOAUOcATwbmVLWdGoH//PB8mnKqScAhsD0kYP3j/Yt5LPQe2KvcXmGvRHcDnpxfL2zOYJ1mFwrryWTz0advv1Ut4CJgf5uhDuDj5eUcAUoahrdY/56ebRWeraTjMt/00Sh3UDtjgHtQNHwcRGOC98BJEAEymycmYcWwOprTgcB6VZ5JK5TAJ+fXGLBm3FDAmn6oPPjR4rKCAoJCal2eAiQp2x0vxTPB3ALO2CRkwmDy5WohzBDwSEFKRwPbknEggCPB/imwrycgxX2NzoMCHhPkDwqYMr9tRcP5qNrMZHkVnOjRMWwLCcr8ohBVb1OMjxLwGCvjTikrsBOiA6fNyCrm8V1rP93iVPpwaE+gO0SsWmPiXB+jikdf6SizrT5qKasx5j8ABbHpFTx+vFXp9EnYQmLx02h1QTTrl6eDqxLnGjporxl3NL3agEvXdT0WmEost648sQOYAeJS9Q7bfUVoMGnjo4AZdUMQku50McDcMWcBPvr0SzbTAFDfvJqwLzgxwATnCgnp4wDl6Aa+Ax283gghmj+vj7feE2KBBRMW3FzOpLOADl0Isb5587h/U4gGvkt5v60Z1VLG8BhYjbzRwyQZemwAd6cCR5/XFWLYZRIMpX39AR0tjaGGiGzLVyhse5C9RKC6ai42ppWPKiBagOvaYk8lO7DajerabOZP46Lby5wKjw1HCRx7p9sVMOWGzb/vA1hwiWc6jm3MvQDTogQkiqIhJV0nBQBTU+3okKCFDy9WwferkHjtxib7t3xIUQtHxnIwtx4mpg26/HfwVNVDb4oI9RHmx5WGelRVlrtiw43zboCLaxv46AZeB3IlTkwouebTr1y2NjSpHz68WNFjHvupy3q8TFn3Hos2IAk4Ju5dCo8B3wP7VPr/FGaKiG+T+v+TQqIrOqMTL1VdWV1DdmcbO8KXBz6esmYWYKPwDL5b5FA1a0hwapHiom0r/cKaoqr+27/XcrS5UwSMbQAAAABJRU5ErkJggg==)](https://deepwiki.com/hyle/fuoricode)

## Requirements

- A C compiler supporting C99
- POSIX.1-2008 environment (Linux, macOS, WSL, etc.)

## Install

### Homebrew

```bash
brew tap hyle/tap
brew install fuoricode
```

Installs `fuori`, with `fuoricode` available as an alias.

### From source

```bash
# System-wide
make install PREFIX=/usr/local

# User-local
make install PREFIX="$HOME/.local"
```

## Usage

Run `fuori` in any directory:

```bash
fuori
```

By default it writes `_export.md` to the current directory. Inside a Git repo, it uses Git's view of the working tree (tracked + untracked non-ignored files). Outside a repo, or with `--no-git`, it falls back to the recursive filesystem walking.

### Options

```bash
fuori [OPTIONS]
```

**Options:**

| Flag | Description |
|---|---|
| `-h`, `--help` | Display help |
| `-V`, `--version` | Show version |
| `-v`, `--verbose` | Show progress during export |
| `-o`, `--output <path>` | Output path (`-` for stdout) |
| `--staged` | Export staged files |
| `--unstaged` | Export unstaged tracked files |
| `--diff <range>` | Export files changed in a diff range |
| `--from-stdin` | Read paths from stdin |
| `-0`, `--null` | Use NUL as the stdin delimiter (requires `--from-stdin`) |
| `--line-numbers` | Prefix exported code lines with line numbers |
| `--hunks [<n>]` | In Git delta modes, export only changed hunks plus context lines |
| `--unpacker` | Append an LLM-oriented unpacker appendix for full exports |
| `--tree` / `--no-tree` | Include/omit project tree (default: on) |
| `--tree-depth <n>` | Limit tree render depth |
| `-s <size_kb>` | Max file size in KB (default: 100) |
| `--warn-tokens <n>` | Warn above token threshold (default: 200k) |
| `--max-tokens <n>` | Hard-fail above token threshold |
| `--no-clobber` | Fail if output already exists |
| `--no-git` | Force filesystem selection |
| `--no-default-ignore` | Disable built-in default ignore patterns in filesystem mode |
| `--allow-sensitive` | Export files even if they match sensitive-file protection rules |

Git selection flags (`--staged`, `--unstaged`, `--diff`) and `--from-stdin` are mutually exclusive; `--no-git` cannot be combined with them.
`--no-default-ignore` only applies to filesystem selection.
`--hunks` only applies to `--staged`, `--unstaged`, and `--diff`.
`--unpacker` cannot be combined with `--hunks`.

**Examples:**

```bash
fuori                              # Export current working tree
fuori --unstaged                   # Export unstaged changes
fuori --staged -o review.md        # Staged changes to a named file
fuori --diff HEAD~3..HEAD          # Files changed in the last 3 commits
fuori --diff main...HEAD           # Changes since branching from main
fuori --staged --hunks             # Only changed hunks with default context
fuori --diff main...HEAD --hunks=8 # Wider hunk context for review
fuori --unpacker                   # Append an unpacker appendix for LLM reconstruction
fuori -o - > codebase.md           # Pipe to stdout
fuori --no-tree                    # Skip the project tree section
fuori --tree-depth 2               # Shallow tree
fuori --line-numbers --staged      # Add line numbers for review-oriented exports
fuori -s 50                        # 50 KB file size cap
fuori --warn-tokens 100000         # Earlier token warning
fuori --max-tokens 270000          # Hard token budget
fuori -o out.md --no-clobber       # Refuse to overwrite
fuori --no-git --no-default-ignore # Disable built-in filesystem ignore defaults
fuori --allow-sensitive            # Export files that secret protection would skip
```

## Ignore Rules

Filesystem mode always honors a local `.gitignore` file when present.
These rules apply in `--no-git` mode and during automatic fallback outside Git repositories.

Supported syntax:

- Comments (`#`)
- Negation (`!pattern`)
- Directory trailing slash (`dir/`)
- Root-anchored patterns (`/pattern`)
- Recursive globs (`**/node_modules/`, `**/*.pyc`)

In filesystem mode, `fuori` also applies a built-in default ignore list unless `--no-default-ignore` is set:

| Category | Patterns |
|---|---|
| VCS | `.git/` |
| Dependencies | `node_modules/`, `.venv/`, `__pycache__/` |
| Build output | `build/`, `dist/`, `bin/` |
| Compiled artifacts | `*.o`, `*.a`, `*.so`, `*.exe`, `*.dll` |
| Environment / OS | `.env`, `.DS_Store`, `*.log` |

Use `--no-default-ignore` to disable only the built-in defaults. Local `.gitignore` rules still apply.

To bypass ignore-based selection entirely, use `--from-stdin`.

## File Size Limit

Files larger than the specified size limit (default 100KB) are automatically excluded from the export to prevent including large binary or data files. You can change this limit using the `-s` option.

## Sensitive File Protection

By default, `fuori` skips files that look obviously sensitive and prints a warning to `stderr`.
This protection applies in every selection mode, including Git-backed modes and `--from-stdin`.

v1 checks:

- high-risk filenames such as `.env*`, `credentials*`, `secret*`, `id_rsa*`, `*.pem`, and `*.key`
- obvious content patterns such as private key blocks and a small set of API key prefixes

Sensitive files are omitted entirely from the Markdown output. In `--staged`, `--unstaged`, and `--diff` mode they are also omitted from `Change Context`.

Use `--allow-sensitive` to disable this protection for a run.

## Git File Selection

By default, `fuori` asks Git for tracked files plus untracked non-ignored files in the current subtree.
If Git is unavailable or the directory is not a repository, it silently falls back to the filesystem walker.
Use `--no-git` to force the filesystem walker explicitly.

| Mode | Git command |
|---|---|
| Default | `git ls-files -z --cached --others --exclude-standard` |
| `--staged` | `git diff --cached --name-status --diff-filter=AMR` |
| `--unstaged` | `git diff --name-status --diff-filter=AMR` |
| `--diff <range>` | `git diff --name-status --diff-filter=AMR <range>` (two-dot and three-dot ranges both work) |

Additional semantics:

- The default Git-backed mode and explicit Git file-selection modes are scoped to the current working directory subtree when run from a Git subdirectory
- Git-selected files bypass ignore rules at selection time
- Git-selected files still go through normal export-time checks such as regular-file validation, symlink skipping, binary detection, size limits, sensitive-file protection, and output-file self-exclusion
- `--unstaged` does not include untracked files
- Renamed files are exported under the current path reported by Git
- `--staged`, `--unstaged`, and `--diff` include a `Change Context` section with change status summaries
- `--hunks[=N]` narrows Git delta exports to changed hunks plus `N` lines of surrounding context (`3` by default)
- Added files still export as full files under `--hunks`
- Delta entries with no renderable changed-line ranges stay in `Change Context` but omit file bodies and tree entries under `--hunks`
- If Git selects no files, `fuori` still succeeds and writes an empty export

## Stdin File Selection

Use `--from-stdin` when another tool should decide which paths to export.

```bash
# newline-delimited (default)
fd -e c -e h | fuori --from-stdin

# NUL-delimited (safe for arbitrary filenames)
fd -e c --print0 | fuori --from-stdin -0
git ls-files -z -- src/ | fuori --from-stdin -0
```

Semantics:

- stdin-selected paths bypass selection-time ignore rules
- they still go through export-time checks such as regular-file validation, symlink skipping, binary detection, size limits, sensitive-file protection, and output-file self-exclusion
- `--from-stdin` is mutually exclusive with `--staged`, `--unstaged`, `--diff`, and `--no-git`
- `-0` / `--null` requires `--from-stdin`
- empty stdin is a successful no-op export that still emits the export preamble
- stdin input order is not preserved; paths are sorted and deduplicated before export
- display paths are caller-supplied, so absolute stdin paths render as absolute headings and relative stdin paths render as relative headings

## Token Estimates

After a successful export, `fuori` prints a compact summary to `stderr`:

```text
Files exported: 42
Bytes written:  183,421
Est. tokens:    ~52,400  (approx, assuming BPE ~3.5 chars/token)
```

The estimate is based on the final Markdown artifact, not just the raw source files, so headings, code fences, and the optional tree section are included in the byte count before estimating tokens.

By default, `fuori` warns when the estimate exceeds 200,000 tokens:

```text
Warning: output may exceed 200,000 token context window. Consider using --staged or --diff to narrow scope.
```

Use `--warn-tokens <n>` to change the warning threshold, or `--max-tokens <n>` to fail before writing any output when the estimated size would exceed a hard limit.

## Text File Detection

`fuori` exports UTF-8 text files and skips inputs that do not pass its text/binary detection path.
In practice, that means files with NUL bytes, invalid UTF-8, or too many control characters are excluded from the export.
Empty files are skipped.
Symbolic links are skipped to avoid recursion cycles.
UTF-16 and other non-UTF-8 text encodings are currently treated as non-exportable and skipped.
Sensitive files are skipped by default unless `--allow-sensitive` is set.

## Output Format

The output markdown file will contain:

1. A preamble with repository, mode, and generation timestamp metadata, plus `Line numbers: on` and `Hunks: on (context: N)` when enabled, and a short mode description
2. A `Change Context` section for `--staged`, `--unstaged`, and `--diff` exports
3. A project tree section that reflects the exported artifact (enabled by default)
4. A header with the file path
5. Either a full-file code block or one or more hunk slices separated by omission markers such as `... 84 unchanged lines omitted ...`
6. Optional line-number prefixes inside code blocks when `--line-numbers` is set; hunk exports keep original file line numbers
7. Appropriate language identifiers for syntax highlighting
8. An optional unpacker appendix with reconstruction instructions and an embedded Python helper when `--unpacker` is set
9. A `stderr` summary of files, bytes, and estimated tokens after successful completion

Example file contents excerpt (the `Makefile` section is omitted for brevity):
````markdown
# Codebase Export

Repository: my-project
Mode: recursive
Generated: 2026-03-16T12:34:56Z
Line numbers: on

This document contains all the source code files from the current directory subtree.

## Project Tree

```text
├── src
│   └── main.c
└── Makefile
```

## src/main.c

```
1 | #include <stdio.h>
2 | 
3 | int main() {
4 |     printf("Hello, World!\n");
5 |     return 0;
6 | }
```
````
