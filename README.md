# fuoricode

A C application that exports a codebase to a single markdown file with UTF-8 encoding.

## Features

- Recursively scans the current directory for source code files
- Respects exclusions defined in a `.gitignore` file
- Automatically detects and excludes binary files
- Excludes files larger than 100KB (configurable)
- Formats code in markdown with appropriate language identifiers
- Outputs to `_export.md` by default (configurable, including stdout)

## Requirements

- C99 compiler (project is built with `-std=c99`)
- POSIX.1-2008 environment/APIs (project is built with `-D_POSIX_C_SOURCE=200809L`)
- Unix-like environment (Linux, macOS, etc.)

## Compilation

```bash
make
```

This will create an executable named `fuori`.

### Installation

```bash
# System-wide (often requires sudo)
make install PREFIX=/usr/local

# User-local install
make install PREFIX="$HOME/.local"
```

### Source Files

- `fuori.c`: Main program (CLI, traversal, export writer)
- `ignore.c` / `ignore.h`: Ignore pattern loading and matching

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
- `--no-clobber`: Fail if output file already exists
- `-h, --help`: Display help message

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
