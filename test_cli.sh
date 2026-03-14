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

REPO="$TMPDIR/repo"
mkdir -p "$REPO/sub"
(cd "$REPO" && git init -q)
cat >"$REPO/.gitignore" <<'EOF_IGNORE'
ignored.log
EOF_IGNORE
cat >"$REPO/sub/tracked.c" <<'EOF_TRACKED'
int tracked(void) { return 1; }
EOF_TRACKED
(cd "$REPO" && git add .gitignore sub/tracked.c && \
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

(cd "$REPO" && "$BIN" --no-git -o - >stdout_no_git.txt 2>stderr_no_git.txt)
assert_contains "$REPO/stdout_no_git.txt" "This document contains all the source code files from the current directory subtree using the local filesystem walker."

if (cd "$REPO" && "$BIN" --no-git --staged >/dev/null 2>stderr_invalid.txt); then
    fail "expected --no-git --staged to fail"
fi
assert_contains "$REPO/stderr_invalid.txt" "--no-git cannot be combined with --staged, --unstaged, or --diff"

printf 'cli tests passed\n'
