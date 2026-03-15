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
