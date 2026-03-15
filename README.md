# fuoricode

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

By default, it writes `_export.md` to the current directory. Inside a Git repo, it uses Git's view of the working tree (tracked + untracked non-ignored files). Outside a repo, or with `--no-git`, it falls back to the recursive filesystem walking.

Design notes for contributors live in [`docs/design.md`](docs/design.md).

### Options

```bash
fuori [OPTIONS]
```

**Options:**

| Flag                    | Description                                 |
| ----------------------- | ------------------------------------------- |
| `-h`, `--help`          | Display help                                |
| `-V`, `--version`       | Show version                                |
| `-v`, `--verbose`       | Show progress during export                 |
| `-o`, `--output <path>` | Output path (`-` for stdout)                |
| `--no-clobber`          | Fail if output already exists               |
| `--no-git`              | Force filesystem selection                  |
| `--from-stdin`          | Read paths from stdin instead of Git        |
| `--staged`              | Export staged files                         |
| `--unstaged`            | Export unstaged tracked files               |
| `--diff <range>`        | Export files changed in a diff range        |
| `--tree` / `--no-tree`  | Include/omit the project tree (default: on) |
| `--tree-depth <n>`      | Limit tree render depth                     |
| `-s <size_kb>`          | Max file size in KB (default: 100)          |
| `--warn-tokens <n>`     | Warn above token threshold (default: 200k)  |
| `--max-tokens <n>`      | Hard-fail above token threshold             |

Git selection flags (`--staged`, `--unstaged`, `--diff`) and `--from-stdin` are mutually exclusive; `--no-git` cannot be combined with them.

**Examples:**
```bash
fuori                              # Export current working tree
fuori --staged -o review.md        # Staged changes to a named file
fuori --diff HEAD~3..HEAD          # Files changed in the last 3 commits
fuori --diff main...HEAD           # Changes since branching from main
fuori -o - > codebase.md           # Pipe to stdout
fuori --no-tree                    # Skip the project tree section
fuori --tree-depth 2               # Shallow tree
fuori -s 50                        # 50 KB file size cap
fuori --warn-tokens 100000         # Earlier token warning
fuori --max-tokens 180000          # Hard token budget
fuori -o out.md --no-clobber       # Refuse to overwrite
```

## .gitignore File

You can create a `.gitignore` file in the directory to specify files and patterns to exclude from the export.
These rules apply to the recursive filesystem walker, including `--no-git` mode and automatic fallback outside Git repositories.
This tool supports common gitignore-style rules, including comments, `!` negation, trailing `/` for directories,
root-anchored `/` patterns, and recursive `**` path globs such as `**/node_modules/` and `**/*.pyc`.

```
# Ignore build directories
build/
dist/

# Ignore specific file types
*.log
*.tmp

# Ignore node_modules directory
node_modules/
```

## File Size Limit

Files larger than the specified size limit (default 100KB) are automatically excluded from the export to prevent including large binary or data files. You can change this limit using the `-s` option.

## Git File Selection

When you run `fuori` without file-selection flags inside a Git repository, it asks Git for the current subtree's tracked files plus untracked non-ignored files by running `git ls-files -z --cached --others --exclude-standard`. If Git is unavailable or the current directory is not inside a repository, `fuori` silently falls back to the recursive filesystem walker.

Use `--no-git` to force the recursive filesystem walker even inside a Git repository.

When you use `--staged`, `--unstaged`, or `--diff`, `fuori` asks Git for an explicit file list instead of recursively walking the tree.

- Default mode uses tracked files plus untracked non-ignored files (`git ls-files -z --cached --others --exclude-standard`)
- `--staged` uses staged files in `AMR` (`git diff --cached --name-only --diff-filter=AMR`)
- `--unstaged` uses tracked unstaged files in `AMR` (`git diff --name-only --diff-filter=AMR`)
- `--diff <range>` passes `<range>` directly to `git diff`, so both two-dot and three-dot ranges work, including examples like `HEAD~3..HEAD`, `main...HEAD`, and `v1.2.0..HEAD`

Additional semantics:

- The default Git-backed mode and explicit Git file-selection modes are scoped to the current working directory subtree when run from a Git subdirectory
- Git-selected files bypass selection-time ignore rules such as `.gitignore`
- Git-selected files still go through normal export-time checks such as regular-file validation, symlink skipping, binary detection, size limits, and output-file self-exclusion
- `--unstaged` does not include untracked files
- Renamed files are exported under the current path reported by Git
- If Git selects no files, `fuori` still succeeds and writes only the normal export header

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
- they still go through normal export-time checks such as regular-file validation, symlink skipping, binary detection, size limits, and output-file self-exclusion
- `--from-stdin` is mutually exclusive with `--staged`, `--unstaged`, `--diff`, and `--no-git`
- `-0` / `--null` requires `--from-stdin`
- empty stdin is a successful no-op export that still emits the normal header
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

## Output Format

The output markdown file will contain:

1. A preamble describing the export mode
2. A project tree section that reflects the exported artifact (enabled by default)
3. A header with the file path
4. A code block with the file content
5. Appropriate language identifiers for syntax highlighting
6. A `stderr` summary of files, bytes, and estimated tokens after successful completion

Example file contents excerpt (the `Makefile` section is omitted for brevity):
````markdown
# Codebase Export

This document contains all the source code files from the current directory subtree.

## Project Tree

```text
├── src
│   └── main.c
└── Makefile
```

## src/main.c

```c
#include <stdio.h>

int main() {
    printf("Hello, World!\n");
    return 0;
}
```
````

## Cleaning Up

To remove the compiled executable:

```bash
make clean
```
