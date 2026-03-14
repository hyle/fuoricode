# fuoricode

A C application that exports a codebase to a single Markdown file with UTF-8 encoding.

## Features

- **Context packing for LLMs**: export a codebase into a single Markdown artifact for AI assistants and LLM-based coding workflows
- Recursively scans the current directory for source code files
- Can export Git-selected files via `--staged`, `--unstaged`, or `--diff <range>`
- Respects exclusions defined in a `.gitignore` file
- Automatically detects and excludes binary files
- Excludes files larger than 100KB (configurable)
- Formats code in Markdown with appropriate language identifiers
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

Simply run the executable in any directory you want to export:

```bash
./fuori
```

By default, the application creates a file named `_export.md` in the current directory containing all source code files in markdown format.

### Command Line Options

```bash
./fuori [OPTIONS]
```

**Options:**
- `-v, --verbose`: Show progress information during processing
- `-s <size_kb>`: Set maximum file size limit in KB (default: 100)
- `-o, --output <path>`: Set output path (use `-` for stdout)
- `--staged`: Export staged files from the current Git subtree
- `--unstaged`: Export unstaged tracked files from the current Git subtree
- `--diff <range>` / `--diff=<range>`: Export files changed by a Git diff range
- `--no-clobber`: Fail if output file already exists
- `-h, --help`: Display help message

Git file-selection modes are mutually exclusive: use at most one of `--staged`, `--unstaged`, or `--diff`.

**Examples:**
```bash
# Basic usage
fuori

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

# Export unstaged tracked files only
fuori --unstaged

# Export files changed in the last 3 commits
fuori --diff HEAD~3..HEAD

# Export files changed on the current branch since it diverged from main
fuori --diff main...HEAD

# Show help
fuori --help
```

## .gitignore File

You can create a `.gitignore` file in the directory to specify files and patterns to exclude from the export.
This tool supports common gitignore-style rules (comments, `!` negation, trailing `/` for directories),
but does not implement recursive `**` glob semantics.

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

When you use `--staged`, `--unstaged`, or `--diff`, `fuori` asks Git for an explicit file list instead of recursively walking the tree.

- `--staged` uses staged files in `AMR` (`git diff --cached --name-only --diff-filter=AMR`)
- `--unstaged` uses tracked unstaged files in `AMR` (`git diff --name-only --diff-filter=AMR`)
- `--diff <range>` passes `<range>` directly to `git diff`, so both two-dot and three-dot ranges work, including examples like `HEAD~3..HEAD`, `main...HEAD`, and `v1.2.0..HEAD`

Additional semantics:

- Git file-selection modes are scoped to the current working directory subtree when run from a Git subdirectory
- Git-selected files bypass selection-time ignore rules such as `.gitignore`
- Git-selected files still go through normal export-time checks such as regular-file validation, symlink skipping, binary detection, size limits, and output-file self-exclusion
- `--unstaged` does not include untracked files
- Renamed files are exported under the current path reported by Git
- If Git selects no files, `fuori` still succeeds and writes only the normal export header

## Binary File Detection

The application automatically detects binary files by analyzing their content and excludes them from the export.
Empty files are skipped.
Symbolic links are skipped to avoid recursion cycles.

## Output Format

The output markdown file will contain:

1. A header with the file path
2. A code block with the file content
3. Appropriate language identifiers for syntax highlighting

Example:
````markdown
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
