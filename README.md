# fuoricode

Export the right slice of a codebase into a single Markdown artifact that is easy to hand to an LLM.

```bash
fuori --staged -o review.md
```

Built for the boring but important job of turning a real working tree into clean, reviewable context without manual copy-paste.

## When to use it

- Prepare context for an LLM without pasting files by hand
- Review staged changes as one artifact before a commit or PR
- Snapshot a subtree for debugging, discussion, or handoff

## How it works

1. Select files from Git or the filesystem, depending on the mode
2. Filter out unsafe or non-exportable content such as binaries, oversized files, symlinks, and generated output targets
3. Render one Markdown artifact with a project tree, per-file headings, and fenced code blocks

## Features

- **Context packing for LLMs**: export a codebase into a single Markdown artifact for AI assistants
- Auto-detects Git worktrees by default and falls back to recursive filesystem scanning elsewhere
- Git-aware file selection via the default worktree mode, `--staged`, `--unstaged`, or `--diff <range>`
- Includes a project tree that matches the exported artifact
- Respects Git ignore rules in Git-backed modes and local ignore rules in filesystem mode
- Automatically detects and excludes binary files
- Excludes files larger than 100KB (configurable)
- Formats code in Markdown with appropriate language identifiers
- Estimates final artifact token usage with a conservative 3.5 chars/token heuristic
- Outputs to `_export.md` by default (configurable, including stdout)
- Simple C implementation that is easy to inspect and review
- Zero dependencies, plain C99 + POSIX, compiles in seconds on almost anything
- Tiny footprint, fast execution even on large projects

## Requirements

- A C compiler supporting C99 (project is built with `-std=c99`)
- POSIX.1-2008 environment/APIs (project is built with `-D_POSIX_C_SOURCE=200809L`)
- Unix-like environment (Linux, macOS, WSL, etc.)

## Install

### Homebrew

```bash
brew tap hyle/tap
brew install fuoricode
```

This installs the `fuori` command, with `fuoricode` also available as an alias.

### From source

```bash
# System-wide (often requires sudo)
make install PREFIX=/usr/local

# User-local install
make install PREFIX="$HOME/.local"
```

## Usage

Run `fuori` in any directory you want to export:

```bash
fuori
```

By default, the application creates a file named `_export.md` in the current directory containing all source code files in markdown format. Inside a Git repository, it prefers Git's view of the current subtree (tracked files plus untracked non-ignored files); outside a repository, or when you pass `--no-git`, it falls back to the recursive filesystem walker.

Contributor-oriented design notes live in [`docs/design.md`](docs/design.md).

### Command Line Options

```bash
fuori [OPTIONS]
```

**Options:**
- `-h, --help`: Display help message
- `-V, --version`: Show version information
- `-v, --verbose`: Show progress information during processing
- `-o, --output <path>`: Set output path (use `-` for stdout)
- `--no-clobber`: Fail if output file already exists
- `--no-git`: Force recursive filesystem selection instead of the default auto-Git behavior
- `--staged`: Export staged files from the current Git subtree
- `--unstaged`: Export unstaged tracked files from the current Git subtree
- `--diff <range>` / `--diff=<range>`: Export files changed by a Git diff range
- `--tree`: Include the project tree section (default)
- `--no-tree`: Omit the project tree section
- `--tree-depth <n>` / `--tree-depth=<n>`: Limit tree rendering depth to `n` levels
- `-s <size_kb>`: Set maximum file size limit in KB (default: 100)
- `--warn-tokens <n>` / `--warn-tokens=<n>`: Warn if the estimated token count exceeds `n` (default: 200000)
- `--max-tokens <n>` / `--max-tokens=<n>`: Fail before writing output if the estimated token count exceeds `n`

Git file-selection modes are mutually exclusive: use at most one of `--staged`, `--unstaged`, or `--diff`. `--no-git` cannot be combined with those explicit Git selection flags.

**Examples:**
```bash
# Basic usage
fuori

# Show help
fuori --help

# Force filesystem recursion even inside a Git repository
fuori --no-git

# Show version
fuori --version

# With verbose output
fuori -v

# With custom file size limit
fuori -s 50

# Write to a custom output path
fuori -o exports/codebase.md

# Write to stdout (useful in pipelines)
fuori -o - > codebase.md

# Prevent overwriting an existing output file
fuori -o codebase.md --no-clobber

# Export staged files only
fuori --staged

# Export without the tree section
fuori --no-tree

# Export with a shallow tree
fuori --tree-depth 2

# Export unstaged tracked files only
fuori --unstaged

# Export files changed in the last 3 commits
fuori --diff HEAD~3..HEAD

# Export files changed on the current branch since it diverged from main
fuori --diff main...HEAD

# Warn earlier for smaller context windows
fuori --warn-tokens 100000

# Refuse to write exports above a hard token budget
fuori --max-tokens 180000
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

Example:
````markdown
# Codebase Export

This document contains all the source code files from the current directory subtree.

## Project Tree

```text
├── src
│   └── main.c
└── Makefile
```

Example file contents excerpt (the `Makefile` section is omitted for brevity):

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
