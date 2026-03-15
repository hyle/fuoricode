#!/bin/sh

set -eu

BIN=${BIN:-./fuori}
BIN_DIR=$(cd "$(dirname "$BIN")" && pwd)
BIN="$BIN_DIR/$(basename "$BIN")"

fail() {
    printf '%s\n' "$1" >&2
    exit 1
}

assert_contains() {
    file=$1
    pattern=$2
    if ! grep -F -- "$pattern" "$file" >/dev/null 2>&1; then
        fail "expected '$pattern' in $file"
    fi
}

assert_not_contains() {
    file=$1
    pattern=$2
    if grep -F -- "$pattern" "$file" >/dev/null 2>&1; then
        fail "did not expect '$pattern' in $file"
    fi
}

assert_file_equals() {
    file=$1
    expected=$2
    actual=$(cat "$file")
    if [ "$actual" != "$expected" ]; then
        fail "unexpected content in $file"
    fi
}

assert_missing() {
    path=$1
    if [ -e "$path" ]; then
        fail "did not expect $path to exist"
    fi
}

assert_no_temp_outputs() {
    dir=$1
    set -- "$dir"/.fuori.tmp.*
    if [ -e "$1" ]; then
        fail "did not expect temporary output artifacts in $dir"
    fi
}

TMPDIR=$(mktemp -d "${TMPDIR:-/tmp}/fuori-cli-test.XXXXXX")
trap 'rm -rf "$TMPDIR"' EXIT INT TERM

OUTSIDE="$TMPDIR/outside"
mkdir -p "$OUTSIDE"
cat >"$OUTSIDE/main.c" <<'EOF_OUTSIDE'
int main(void) { return 0; }
EOF_OUTSIDE

(cd "$OUTSIDE" && "$BIN" -o - >stdout.txt 2>stderr.txt)
assert_contains "$OUTSIDE/stdout.txt" "This document contains all the source code files from the current directory subtree using the local filesystem walker."
assert_not_contains "$OUTSIDE/stderr.txt" "Git file-selection modes require"
assert_not_contains "$OUTSIDE/stderr.txt" "git rev-parse failed"
assert_not_contains "$OUTSIDE/stderr.txt" "fatal: not a git repository"

cat >"$OUTSIDE/notes.md" <<'EOF_NOTES'
notes
EOF_NOTES
(cd "$OUTSIDE" && "$BIN" -o export.md >command_stdout.txt 2>command_stderr.txt)
assert_contains "$OUTSIDE/export.md" "## main\\.c"
assert_contains "$OUTSIDE/export.md" "## notes\\.md"
assert_not_contains "$OUTSIDE/export.md" "export.md"
assert_not_contains "$OUTSIDE/export.md" ".fuori.tmp."
assert_file_equals "$OUTSIDE/command_stdout.txt" ""
assert_not_contains "$OUTSIDE/command_stderr.txt" "Codebase exported to export.md successfully!"

(cd "$OUTSIDE" && "$BIN" -o - >redirected.md 2>redirect_stderr.txt)
assert_contains "$OUTSIDE/redirected.md" "## main\\.c"
assert_contains "$OUTSIDE/redirected.md" "## notes\\.md"
assert_not_contains "$OUTSIDE/redirected.md" "redirected.md"

VERBOSE_DIR="$TMPDIR/verbose"
mkdir -p "$VERBOSE_DIR"
cat >"$VERBOSE_DIR/.gitignore" <<'EOF_VERBOSE_IGNORE'
ignored.txt
EOF_VERBOSE_IGNORE
cat >"$VERBOSE_DIR/main.c" <<'EOF_VERBOSE_MAIN'
int main(void) { return 0; }
EOF_VERBOSE_MAIN
cat >"$VERBOSE_DIR/ignored.txt" <<'EOF_VERBOSE_IGNORED'
ignored
EOF_VERBOSE_IGNORED
printf '\000\001\002binary' >"$VERBOSE_DIR/binary.bin"
awk 'BEGIN { for (i = 0; i < 2048; i++) printf "x"; printf "\n" }' >"$VERBOSE_DIR/large.txt"
ln -s main.c "$VERBOSE_DIR/link.c"

(cd "$VERBOSE_DIR" && "$BIN" -v -s 1 -o - >verbose_stdout.txt 2>verbose_stderr.txt)
assert_contains "$VERBOSE_DIR/verbose_stderr.txt" "Skipped: binary/empty=1, too_large=1, ignored=1, symlink=1"

(cd "$OUTSIDE" && "$BIN" -v -o verbose_export.md >verbose_file_stdout.txt 2>verbose_file_stderr.txt)
assert_file_equals "$OUTSIDE/verbose_file_stdout.txt" ""
assert_contains "$OUTSIDE/verbose_file_stderr.txt" "Codebase exported to verbose_export.md successfully!"

cat >"$OUTSIDE/existing.txt" <<'EOF_EXISTING'
keep me
EOF_EXISTING
if (cd "$OUTSIDE" && "$BIN" -o existing.txt --no-clobber >/dev/null 2>no_clobber_stderr.txt); then
    fail "expected --no-clobber to fail"
fi
assert_contains "$OUTSIDE/no_clobber_stderr.txt" "fuori: output file already exists: existing.txt"
assert_file_equals "$OUTSIDE/existing.txt" "keep me"

TOKEN_DIR="$TMPDIR/tokens"
mkdir -p "$TOKEN_DIR"
cat >"$TOKEN_DIR/big.txt" <<'EOF_BIG'
This file is intentionally long enough to exceed a tiny token budget.
EOF_BIG
if (cd "$TOKEN_DIR" && "$BIN" --max-tokens 1 -o blocked.md >/dev/null 2>max_tokens_new_stderr.txt); then
    fail "expected --max-tokens to fail for missing destination"
fi
assert_contains "$TOKEN_DIR/max_tokens_new_stderr.txt" "Error: estimated output is"
assert_missing "$TOKEN_DIR/blocked.md"

cat >"$TOKEN_DIR/blocked.md" <<'EOF_BLOCKED'
original content
EOF_BLOCKED
if (cd "$TOKEN_DIR" && "$BIN" --max-tokens 1 -o blocked.md >/dev/null 2>max_tokens_existing_stderr.txt); then
    fail "expected --max-tokens to fail for existing destination"
fi
assert_contains "$TOKEN_DIR/max_tokens_existing_stderr.txt" "Error: estimated output is"
assert_file_equals "$TOKEN_DIR/blocked.md" "original content"

FORMAT_DIR="$TMPDIR/formatting"
mkdir -p "$FORMAT_DIR"
awk 'BEGIN { for (i = 0; i < 130000; i++) printf "x"; printf "\n" }' >"$FORMAT_DIR/big.txt"
(cd "$FORMAT_DIR" && "$BIN" -s 200 -o - >format_stdout.txt 2>format_stderr.txt)
assert_contains "$FORMAT_DIR/format_stderr.txt" "Bytes written:  130,"
assert_not_contains "$FORMAT_DIR/format_stderr.txt" "Bytes written:  1,30,"

FAIL_DIR="$TMPDIR/write_failure"
mkdir -p "$FAIL_DIR/blocked"
cat >"$FAIL_DIR/main.c" <<'EOF_FAIL_MAIN'
int main(void) { return 0; }
EOF_FAIL_MAIN
if (cd "$FAIL_DIR" && "$BIN" -o blocked >/dev/null 2>write_failure_stderr.txt); then
    fail "expected rename failure when output path is a directory"
fi
assert_contains "$FAIL_DIR/write_failure_stderr.txt" "Error moving temporary file to final destination"
assert_no_temp_outputs "$FAIL_DIR"

RENDER_FAIL_DIR="$TMPDIR/render_failure"
mkdir -p "$RENDER_FAIL_DIR"
cat >"$RENDER_FAIL_DIR/a.c" <<'EOF_RENDER_A'
int a(void) { return 1; }
EOF_RENDER_A
cat >"$RENDER_FAIL_DIR/b.c" <<'EOF_RENDER_B'
int b(void) { return 2; }
EOF_RENDER_B
cat >"$RENDER_FAIL_DIR/export.md" <<'EOF_RENDER_EXISTING'
original artifact
EOF_RENDER_EXISTING
if (cd "$RENDER_FAIL_DIR" && FUORI_TEST_FAIL_RENDER_AT=1 "$BIN" -o export.md >/dev/null 2>render_failure_stderr.txt); then
    fail "expected render failure to abort export"
fi
assert_contains "$RENDER_FAIL_DIR/render_failure_stderr.txt" "Error processing export files"
assert_file_equals "$RENDER_FAIL_DIR/export.md" "original artifact"
assert_no_temp_outputs "$RENDER_FAIL_DIR"

PERM_DIR="$TMPDIR/permissions"
mkdir -p "$PERM_DIR"
cat >"$PERM_DIR/main.c" <<'EOF_PERM_MAIN'
int main(void) { return 0; }
EOF_PERM_MAIN
cat >"$PERM_DIR/private.txt" <<'EOF_PERM_PRIVATE'
secret
EOF_PERM_PRIVATE
chmod 000 "$PERM_DIR/private.txt"
(cd "$PERM_DIR" && "$BIN" -o - >permission_stdout.txt 2>permission_stderr.txt)
assert_contains "$PERM_DIR/permission_stdout.txt" "## main\\.c"
assert_not_contains "$PERM_DIR/permission_stdout.txt" "private.txt"
assert_contains "$PERM_DIR/permission_stderr.txt" "Warning: Failed to process file ./private.txt"

ODD_DIR="$TMPDIR/odd_paths"
mkdir -p "$ODD_DIR"
cat >"$ODD_DIR/main.c" <<'EOF_ODD_MAIN'
int main(void) { return 0; }
EOF_ODD_MAIN
cat >"$ODD_DIR/file with spaces.txt" <<'EOF_ODD_SPACE'
spaces
EOF_ODD_SPACE
cat >"$ODD_DIR/café.py" <<'EOF_ODD_UTF8'
print("coffee")
EOF_ODD_UTF8
cat >"$ODD_DIR/weird & <name>.txt" <<'EOF_ODD_ESCAPED'
symbols
EOF_ODD_ESCAPED
(cd "$ODD_DIR" && "$BIN" -o - >odd_stdout.txt 2>"$TMPDIR/odd_paths_stderr.txt")
assert_contains "$ODD_DIR/odd_stdout.txt" "## file with spaces\\.txt"
assert_contains "$ODD_DIR/odd_stdout.txt" "## café\\.py"
assert_contains "$ODD_DIR/odd_stdout.txt" "## weird &amp; &lt;name\\>\\.txt"

REPO="$TMPDIR/repo"
mkdir -p "$REPO/sub"
(cd "$REPO" && git init -q)
cat >"$REPO/.gitignore" <<'EOF_IGNORE'
ignored.log
EOF_IGNORE
cat >"$REPO/sub/tracked.c" <<'EOF_TRACKED'
int tracked(void) { return 1; }
EOF_TRACKED
cat >"$REPO/root_only.c" <<'EOF_ROOT'
int root_only(void) { return 2; }
EOF_ROOT
(cd "$REPO" && git add .gitignore sub/tracked.c && \
    git add root_only.c && \
    git -c user.name='fuori tests' -c user.email='fuori@example.com' commit -qm init)
cat >"$REPO/sub/untracked.py" <<'EOF_UNTRACKED'
print("hello")
EOF_UNTRACKED
cat >"$REPO/sub/ignored.log" <<'EOF_IGNORED'
ignore me
EOF_IGNORED

(cd "$REPO/sub" && "$BIN" -o - >stdout.txt 2>stderr.txt)
assert_contains "$REPO/sub/stdout.txt" "This document contains tracked files plus untracked, non-ignored files from the current Git subtree."
assert_contains "$REPO/sub/stdout.txt" "├── tracked.c"
assert_contains "$REPO/sub/stdout.txt" "└── untracked.py"
assert_not_contains "$REPO/sub/stdout.txt" "ignored.log"
assert_not_contains "$REPO/sub/stdout.txt" ".gitignore"
assert_not_contains "$REPO/sub/stdout.txt" "root_only.c"

(cd "$REPO" && "$BIN" --no-git -o - >stdout_no_git.txt 2>stderr_no_git.txt)
assert_contains "$REPO/stdout_no_git.txt" "This document contains all the source code files from the current directory subtree using the local filesystem walker."

if (cd "$REPO" && "$BIN" --no-git --staged >/dev/null 2>stderr_invalid.txt); then
    fail "expected --no-git --staged to fail"
fi
assert_contains "$REPO/stderr_invalid.txt" "--no-git cannot be combined with --staged, --unstaged, or --diff"

printf 'cli tests passed\n'
