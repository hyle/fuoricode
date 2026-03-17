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

assert_occurrences() {
    file=$1
    pattern=$2
    expected=$3
    actual=$(grep -F -c -- "$pattern" "$file")
    if [ "$actual" -ne "$expected" ]; then
        fail "expected $expected occurrences of '$pattern' in $file, got $actual"
    fi
}

TMPDIR=$(mktemp -d "${TMPDIR:-/tmp}/fuori-cli-test.XXXXXX")
trap 'rm -rf "$TMPDIR"' EXIT INT TERM

(cd "$BIN_DIR" && "$BIN" --help >"$TMPDIR/help_stdout.txt" 2>"$TMPDIR/help_stderr.txt")
assert_contains "$TMPDIR/help_stdout.txt" "--allow-sensitive"
assert_contains "$TMPDIR/help_stdout.txt" "--line-numbers"
assert_contains "$TMPDIR/help_stdout.txt" "--no-default-ignore"
assert_file_equals "$TMPDIR/help_stderr.txt" ""

OUTSIDE="$TMPDIR/outside"
mkdir -p "$OUTSIDE"
cat >"$OUTSIDE/main.c" <<'EOF_OUTSIDE'
int main(void) { return 0; }
EOF_OUTSIDE

(cd "$OUTSIDE" && "$BIN" -o - >stdout.txt 2>stderr.txt)
assert_contains "$OUTSIDE/stdout.txt" "Repository: outside"
assert_contains "$OUTSIDE/stdout.txt" "Mode: recursive"
assert_contains "$OUTSIDE/stdout.txt" "Generated: "
assert_contains "$OUTSIDE/stdout.txt" "This document contains all the source code files from the current directory subtree using the local filesystem walker."
assert_not_contains "$OUTSIDE/stderr.txt" "Git file-selection modes require"
assert_not_contains "$OUTSIDE/stderr.txt" "git rev-parse failed"
assert_not_contains "$OUTSIDE/stderr.txt" "fatal: not a git repository"

cat >"$OUTSIDE/notes.md" <<'EOF_NOTES'
notes
EOF_NOTES
(cd "$OUTSIDE" && "$BIN" -o export.md >command_stdout.txt 2>command_stderr.txt)
assert_contains "$OUTSIDE/export.md" "## main.c"
assert_contains "$OUTSIDE/export.md" "## notes.md"
assert_not_contains "$OUTSIDE/export.md" "export.md"
assert_not_contains "$OUTSIDE/export.md" ".fuori.tmp."
assert_file_equals "$OUTSIDE/command_stdout.txt" ""
assert_not_contains "$OUTSIDE/command_stderr.txt" "Codebase exported to export.md successfully!"

(cd "$OUTSIDE" && "$BIN" -o - >redirected.md 2>redirect_stderr.txt)
assert_contains "$OUTSIDE/redirected.md" "## main.c"
assert_contains "$OUTSIDE/redirected.md" "## notes.md"
assert_not_contains "$OUTSIDE/redirected.md" "redirected.md"

LINE_NUMBERS_DIR="$TMPDIR/line_numbers"
mkdir -p "$LINE_NUMBERS_DIR"
cat >"$LINE_NUMBERS_DIR/main.c" <<'EOF_LINE_NUMBERS'
#include <stdio.h>
int main(void) { return 0; }
EOF_LINE_NUMBERS

(cd "$LINE_NUMBERS_DIR" && "$BIN" --no-git --no-tree --line-numbers -o - >line_numbers_stdout.txt 2>line_numbers_stderr.txt)
assert_contains "$LINE_NUMBERS_DIR/line_numbers_stdout.txt" "## main.c"
assert_contains "$LINE_NUMBERS_DIR/line_numbers_stdout.txt" "Line numbers: on"
assert_contains "$LINE_NUMBERS_DIR/line_numbers_stdout.txt" "1 | #include <stdio.h>"
assert_contains "$LINE_NUMBERS_DIR/line_numbers_stdout.txt" "2 | int main(void) { return 0; }"
assert_not_contains "$LINE_NUMBERS_DIR/line_numbers_stdout.txt" "3 |"
assert_not_contains "$LINE_NUMBERS_DIR/line_numbers_stderr.txt" "Warning:"

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
assert_contains "$VERBOSE_DIR/verbose_stderr.txt" "Skipped: binary/empty=1, too_large=1, ignored=1, symlink=1, sensitive=0"

SENSITIVE_NAME_DIR="$TMPDIR/sensitive_name"
mkdir -p "$SENSITIVE_NAME_DIR"
cat >"$SENSITIVE_NAME_DIR/main.c" <<'EOF_SENSITIVE_NAME_MAIN'
int main(void) { return 0; }
EOF_SENSITIVE_NAME_MAIN
cat >"$SENSITIVE_NAME_DIR/credentials.txt" <<'EOF_SENSITIVE_NAME_SECRET'
plain text but sensitive by filename
EOF_SENSITIVE_NAME_SECRET
(cd "$SENSITIVE_NAME_DIR" && "$BIN" --no-git -o - >sensitive_name_stdout.txt 2>"$TMPDIR/sensitive_name_stderr.txt")
assert_contains "$SENSITIVE_NAME_DIR/sensitive_name_stdout.txt" "## main.c"
assert_not_contains "$SENSITIVE_NAME_DIR/sensitive_name_stdout.txt" "credentials.txt"
assert_contains "$TMPDIR/sensitive_name_stderr.txt" "Warning: Skipping sensitive file ./credentials.txt"

(cd "$SENSITIVE_NAME_DIR" && "$BIN" --no-git --allow-sensitive -o - >sensitive_name_allow_stdout.txt 2>"$TMPDIR/sensitive_name_allow_stderr.txt")
assert_contains "$SENSITIVE_NAME_DIR/sensitive_name_allow_stdout.txt" "## credentials.txt"
assert_not_contains "$TMPDIR/sensitive_name_allow_stderr.txt" "Warning: Skipping sensitive file"

SENSITIVE_CONTENT_DIR="$TMPDIR/sensitive_content"
mkdir -p "$SENSITIVE_CONTENT_DIR"
cat >"$SENSITIVE_CONTENT_DIR/main.c" <<'EOF_SENSITIVE_CONTENT_MAIN'
int main(void) { return 0; }
EOF_SENSITIVE_CONTENT_MAIN
cat >"$SENSITIVE_CONTENT_DIR/notes.txt" <<'EOF_SENSITIVE_CONTENT_SECRET'
api_key=sk-abcdefghijklmnopqrstuvwxyz123456
EOF_SENSITIVE_CONTENT_SECRET
(cd "$SENSITIVE_CONTENT_DIR" && "$BIN" --no-git -o - >sensitive_content_stdout.txt 2>"$TMPDIR/sensitive_content_stderr.txt")
assert_contains "$SENSITIVE_CONTENT_DIR/sensitive_content_stdout.txt" "## main.c"
assert_not_contains "$SENSITIVE_CONTENT_DIR/sensitive_content_stdout.txt" "## notes.txt"
assert_contains "$TMPDIR/sensitive_content_stderr.txt" "Warning: Skipping sensitive file ./notes.txt"
assert_not_contains "$TMPDIR/sensitive_content_stderr.txt" "sk-abcdefghijklmnopqrstuvwxyz123456"

(cd "$SENSITIVE_CONTENT_DIR" && "$BIN" --no-git --allow-sensitive -o - >sensitive_content_allow_stdout.txt 2>"$TMPDIR/sensitive_content_allow_stderr.txt")
assert_contains "$SENSITIVE_CONTENT_DIR/sensitive_content_allow_stdout.txt" "## notes.txt"
assert_not_contains "$TMPDIR/sensitive_content_allow_stderr.txt" "Warning: Skipping sensitive file"

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
if [ "$(id -u)" -ne 0 ]; then
    chmod 000 "$PERM_DIR/private.txt"
    (cd "$PERM_DIR" && "$BIN" -o - >permission_stdout.txt 2>permission_stderr.txt)
    assert_contains "$PERM_DIR/permission_stdout.txt" "## main.c"
    assert_not_contains "$PERM_DIR/permission_stdout.txt" "private.txt"
    assert_contains "$PERM_DIR/permission_stderr.txt" "Warning: Failed to process file ./private.txt"
fi

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
assert_contains "$ODD_DIR/odd_stdout.txt" "## file with spaces.txt"
assert_contains "$ODD_DIR/odd_stdout.txt" "## café.py"
assert_contains "$ODD_DIR/odd_stdout.txt" "## weird &amp; &lt;name>.txt"

TREE_FENCE_DIR="$TMPDIR/tree_fence"
mkdir -p "$TREE_FENCE_DIR/src"
TREE_FENCE_FILE="$TREE_FENCE_DIR/src/"'```name.c'
cat >"$TREE_FENCE_FILE" <<'EOF_TREE_FENCE'
int tree_fence(void) { return 0; }
EOF_TREE_FENCE
(cd "$TREE_FENCE_DIR" && "$BIN" -o - >tree_fence_stdout.txt 2>tree_fence_stderr.txt)
assert_contains "$TREE_FENCE_DIR/tree_fence_stdout.txt" '````text'
assert_contains "$TREE_FENCE_DIR/tree_fence_stdout.txt" '└── ```name.c'

IGNORE_NEGATION_DIR="$TMPDIR/ignore_negation"
mkdir -p "$IGNORE_NEGATION_DIR/build"
cat >"$IGNORE_NEGATION_DIR/.gitignore" <<'EOF_IGNORE_NEGATION'
build/
!keep.txt
EOF_IGNORE_NEGATION
cat >"$IGNORE_NEGATION_DIR/build/keep.txt" <<'EOF_IGNORE_KEEP'
keep
EOF_IGNORE_KEEP
(cd "$IGNORE_NEGATION_DIR" && "$BIN" --no-git -o - >ignore_negation_stdout.txt 2>ignore_negation_stderr.txt)
assert_not_contains "$IGNORE_NEGATION_DIR/ignore_negation_stdout.txt" "build/keep\\.txt"

NO_DEFAULT_IGNORE_DIR="$TMPDIR/no_default_ignore"
mkdir -p "$NO_DEFAULT_IGNORE_DIR/build"
cat >"$NO_DEFAULT_IGNORE_DIR/.gitignore" <<'EOF_NO_DEFAULT_IGNORE'
local.txt
EOF_NO_DEFAULT_IGNORE
cat >"$NO_DEFAULT_IGNORE_DIR/main.c" <<'EOF_NO_DEFAULT_MAIN'
int main(void) { return 0; }
EOF_NO_DEFAULT_MAIN
cat >"$NO_DEFAULT_IGNORE_DIR/build/keep.txt" <<'EOF_NO_DEFAULT_BUILD'
built artifact but intentionally exported
EOF_NO_DEFAULT_BUILD
cat >"$NO_DEFAULT_IGNORE_DIR/local.txt" <<'EOF_NO_DEFAULT_LOCAL'
still ignored by local rule
EOF_NO_DEFAULT_LOCAL

(cd "$NO_DEFAULT_IGNORE_DIR" && "$BIN" --no-git --no-tree -o - >default_ignore_stdout.txt 2>default_ignore_stderr.txt)
assert_contains "$NO_DEFAULT_IGNORE_DIR/default_ignore_stdout.txt" "## main.c"
assert_not_contains "$NO_DEFAULT_IGNORE_DIR/default_ignore_stdout.txt" "build/keep.txt"
assert_not_contains "$NO_DEFAULT_IGNORE_DIR/default_ignore_stdout.txt" "## local.txt"

(cd "$NO_DEFAULT_IGNORE_DIR" && "$BIN" --no-git --no-default-ignore --no-tree -o - >no_default_ignore_stdout.txt 2>no_default_ignore_stderr.txt)
assert_contains "$NO_DEFAULT_IGNORE_DIR/no_default_ignore_stdout.txt" "## main.c"
assert_contains "$NO_DEFAULT_IGNORE_DIR/no_default_ignore_stdout.txt" "## build/keep.txt"
assert_not_contains "$NO_DEFAULT_IGNORE_DIR/no_default_ignore_stdout.txt" "## local.txt"

STDIN_DIR="$TMPDIR/stdin_selection"
mkdir -p "$STDIN_DIR/ignored"
cat >"$STDIN_DIR/.gitignore" <<'EOF_STDIN_IGNORE'
ignored/
EOF_STDIN_IGNORE
cat >"$STDIN_DIR/alpha.c" <<'EOF_STDIN_ALPHA'
int alpha(void) { return 1; }
EOF_STDIN_ALPHA
cat >"$STDIN_DIR/beta.c" <<'EOF_STDIN_BETA'
int beta(void) { return 2; }
EOF_STDIN_BETA
cat >"$STDIN_DIR/file with spaces.txt" <<'EOF_STDIN_SPACES'
space path
EOF_STDIN_SPACES
cat >"$STDIN_DIR/ignored/keep.txt" <<'EOF_STDIN_IGNORED'
ignored but selected
EOF_STDIN_IGNORED

printf 'beta.c\nalpha.c\nmissing.c\n' | (cd "$STDIN_DIR" && "$BIN" --from-stdin --no-tree -o - >stdin_newline_stdout.txt 2>stdin_newline_stderr.txt)
assert_contains "$STDIN_DIR/stdin_newline_stdout.txt" "This document contains files selected from caller-supplied stdin paths."
assert_contains "$STDIN_DIR/stdin_newline_stdout.txt" "## alpha.c"
assert_contains "$STDIN_DIR/stdin_newline_stdout.txt" "## beta.c"
assert_not_contains "$STDIN_DIR/stdin_newline_stdout.txt" "missing\\.c"

printf 'alpha.c\0beta.c\0' | (cd "$STDIN_DIR" && "$BIN" --from-stdin -0 --no-tree -o - >stdin_null_stdout.txt 2>stdin_null_stderr.txt)
assert_contains "$STDIN_DIR/stdin_null_stdout.txt" "## alpha.c"
assert_contains "$STDIN_DIR/stdin_null_stdout.txt" "## beta.c"

: | (cd "$STDIN_DIR" && "$BIN" --from-stdin --no-tree -o - >stdin_empty_stdout.txt 2>stdin_empty_stderr.txt)
assert_contains "$STDIN_DIR/stdin_empty_stdout.txt" "# Codebase Export"
assert_contains "$STDIN_DIR/stdin_empty_stdout.txt" "This document contains files selected from caller-supplied stdin paths."
assert_not_contains "$STDIN_DIR/stdin_empty_stdout.txt" "## "

printf 'missing.c\n' | (cd "$STDIN_DIR" && "$BIN" --from-stdin --no-tree -o - >stdin_none_stdout.txt 2>stdin_none_stderr.txt)
assert_contains "$STDIN_DIR/stdin_none_stdout.txt" "# Codebase Export"
assert_not_contains "$STDIN_DIR/stdin_none_stdout.txt" "## "

if printf 'alpha.c\n' | (cd "$STDIN_DIR" && "$BIN" --from-stdin --staged >/dev/null 2>stdin_conflict_staged.txt); then
    fail "expected --from-stdin --staged to fail"
fi
assert_contains "$STDIN_DIR/stdin_conflict_staged.txt" "--from-stdin, --staged, --unstaged, and --diff are mutually exclusive"

if printf 'alpha.c\n' | (cd "$STDIN_DIR" && "$BIN" --from-stdin --diff HEAD >/dev/null 2>stdin_conflict_diff.txt); then
    fail "expected --from-stdin --diff to fail"
fi
assert_contains "$STDIN_DIR/stdin_conflict_diff.txt" "--from-stdin, --staged, --unstaged, and --diff are mutually exclusive"

if printf 'alpha.c\n' | (cd "$STDIN_DIR" && "$BIN" --from-stdin --no-git >/dev/null 2>stdin_conflict_no_git.txt); then
    fail "expected --from-stdin --no-git to fail"
fi
assert_contains "$STDIN_DIR/stdin_conflict_no_git.txt" "--no-git cannot be combined with --from-stdin, --staged, --unstaged, or --diff"

if printf 'alpha.c\n' | (cd "$STDIN_DIR" && "$BIN" --from-stdin --no-default-ignore >/dev/null 2>stdin_conflict_no_default_ignore.txt); then
    fail "expected --from-stdin --no-default-ignore to fail"
fi
assert_contains "$STDIN_DIR/stdin_conflict_no_default_ignore.txt" "--no-default-ignore can only be used with filesystem selection"

if (cd "$STDIN_DIR" && "$BIN" -0 >/dev/null 2>stdin_null_without_mode_stderr.txt); then
    fail "expected -0 without --from-stdin to fail"
fi
assert_contains "$STDIN_DIR/stdin_null_without_mode_stderr.txt" "-0/--null requires --from-stdin"

if (cd "$STDIN_DIR" && "$BIN" --null >/dev/null 2>stdin_long_null_without_mode_stderr.txt); then
    fail "expected --null without --from-stdin to fail"
fi
assert_contains "$STDIN_DIR/stdin_long_null_without_mode_stderr.txt" "-0/--null requires --from-stdin"

printf 'alpha.c\nalpha.c\n' | (cd "$STDIN_DIR" && "$BIN" --from-stdin --no-tree -o - >stdin_dupe_stdout.txt 2>stdin_dupe_stderr.txt)
assert_occurrences "$STDIN_DIR/stdin_dupe_stdout.txt" "## alpha.c" 1

printf 'alpha.c' | (cd "$STDIN_DIR" && "$BIN" --from-stdin --no-tree -o - >stdin_unterminated_newline_stdout.txt 2>stdin_unterminated_newline_stderr.txt)
assert_contains "$STDIN_DIR/stdin_unterminated_newline_stdout.txt" "## alpha.c"

printf 'alpha.c\0beta.c' | (cd "$STDIN_DIR" && "$BIN" --from-stdin -0 --no-tree -o - >stdin_unterminated_null_stdout.txt 2>stdin_unterminated_null_stderr.txt)
assert_contains "$STDIN_DIR/stdin_unterminated_null_stdout.txt" "## alpha.c"
assert_contains "$STDIN_DIR/stdin_unterminated_null_stdout.txt" "## beta.c"

printf 'file with spaces.txt\n' | (cd "$STDIN_DIR" && "$BIN" --from-stdin --no-tree -o - >stdin_spaces_newline_stdout.txt 2>stdin_spaces_newline_stderr.txt)
assert_contains "$STDIN_DIR/stdin_spaces_newline_stdout.txt" "## file with spaces.txt"

printf 'file with spaces.txt\0' | (cd "$STDIN_DIR" && "$BIN" --from-stdin --null --no-tree -o - >stdin_spaces_null_stdout.txt 2>stdin_spaces_null_stderr.txt)
assert_contains "$STDIN_DIR/stdin_spaces_null_stdout.txt" "## file with spaces.txt"

printf 'beta.c\nalpha.c\n' | (cd "$STDIN_DIR" && "$BIN" --from-stdin --no-tree -o - >stdin_order_stdout.txt 2>stdin_order_stderr.txt)
first_heading=$(grep '^## ' "$STDIN_DIR/stdin_order_stdout.txt" | head -n 1)
if [ "$first_heading" != "## alpha.c" ]; then
    fail "expected sorted stdin output, got first heading '$first_heading'"
fi

printf 'ignored/keep.txt\n' | (cd "$STDIN_DIR" && "$BIN" --from-stdin --no-tree -o - >stdin_ignore_bypass_stdout.txt 2>stdin_ignore_bypass_stderr.txt)
assert_contains "$STDIN_DIR/stdin_ignore_bypass_stdout.txt" "## ignored/keep.txt"

NEGATE_DIR="$TMPDIR/negated_restore"
mkdir -p "$NEGATE_DIR/build"
cat >"$NEGATE_DIR/.gitignore" <<'EOF_NEGATE_IGNORE'
build/
!build/keep.txt
EOF_NEGATE_IGNORE
cat >"$NEGATE_DIR/build/keep.txt" <<'EOF_NEGATE_KEEP'
restored
EOF_NEGATE_KEEP
(cd "$NEGATE_DIR" && "$BIN" --no-git -o - >negated_stdout.txt 2>negated_stderr.txt)
assert_contains "$NEGATE_DIR/negated_stdout.txt" "## build/keep.txt"

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
assert_contains "$REPO/sub/stdout.txt" "Repository: repo"
assert_contains "$REPO/sub/stdout.txt" "Mode: worktree"
assert_contains "$REPO/sub/stdout.txt" "This document contains tracked files plus untracked, non-ignored files from the current Git subtree."
assert_not_contains "$REPO/sub/stdout.txt" "## Change Context"
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
assert_contains "$REPO/stderr_invalid.txt" "--no-git cannot be combined with --from-stdin, --staged, --unstaged, or --diff"

if (cd "$REPO" && "$BIN" --staged --no-default-ignore >/dev/null 2>stderr_no_default_ignore_invalid.txt); then
    fail "expected --staged --no-default-ignore to fail"
fi
assert_contains "$REPO/stderr_no_default_ignore_invalid.txt" "--no-default-ignore can only be used with filesystem selection"

STAGED_REPO="$TMPDIR/staged_repo"
mkdir -p "$STAGED_REPO"
(cd "$STAGED_REPO" && git init -q)
cat >"$STAGED_REPO/alpha.c" <<'EOF_STAGED_ALPHA_BASE'
int alpha(void) { return 1; }
EOF_STAGED_ALPHA_BASE
cat >"$STAGED_REPO/beta.c" <<'EOF_STAGED_BETA_BASE'
int beta(void) { return 2; }
EOF_STAGED_BETA_BASE
cat >"$STAGED_REPO/old_name.c" <<'EOF_STAGED_OLD_BASE'
int old_name(void) { return 3; }
EOF_STAGED_OLD_BASE
(cd "$STAGED_REPO" && git add alpha.c beta.c old_name.c && \
    git -c user.name='fuori tests' -c user.email='fuori@example.com' commit -qm base)
cat >"$STAGED_REPO/alpha.c" <<'EOF_STAGED_ALPHA_MOD'
int alpha(void) { return 10; }
EOF_STAGED_ALPHA_MOD
(cd "$STAGED_REPO" && git add alpha.c)
(cd "$STAGED_REPO" && git mv old_name.c new_name.c)
cat >"$STAGED_REPO/added.c" <<'EOF_STAGED_ADDED'
int added(void) { return 4; }
EOF_STAGED_ADDED
(cd "$STAGED_REPO" && git add added.c)
cat >"$STAGED_REPO/beta.c" <<'EOF_STAGED_BETA_MOD'
int beta(void) { return 20; }
EOF_STAGED_BETA_MOD

(cd "$STAGED_REPO" && "$BIN" --staged --no-tree -o - >staged_stdout.txt 2>staged_stderr.txt)
assert_contains "$STAGED_REPO/staged_stdout.txt" "Mode: staged"
assert_contains "$STAGED_REPO/staged_stdout.txt" "## Change Context"
assert_contains "$STAGED_REPO/staged_stdout.txt" "Files changed: 3"
assert_contains "$STAGED_REPO/staged_stdout.txt" "- A added.c"
assert_contains "$STAGED_REPO/staged_stdout.txt" "- M alpha.c"
assert_contains "$STAGED_REPO/staged_stdout.txt" "- R old_name.c -> new_name.c"
assert_contains "$STAGED_REPO/staged_stdout.txt" "## added.c"
assert_contains "$STAGED_REPO/staged_stdout.txt" "## alpha.c"
assert_contains "$STAGED_REPO/staged_stdout.txt" "## new_name.c"

(cd "$STAGED_REPO" && "$BIN" --unstaged --no-tree -o - >unstaged_stdout.txt 2>unstaged_stderr.txt)
assert_contains "$STAGED_REPO/unstaged_stdout.txt" "Mode: unstaged"
assert_contains "$STAGED_REPO/unstaged_stdout.txt" "## Change Context"
assert_contains "$STAGED_REPO/unstaged_stdout.txt" "Files changed: 1"
assert_contains "$STAGED_REPO/unstaged_stdout.txt" "- M beta.c"
assert_contains "$STAGED_REPO/unstaged_stdout.txt" "## beta.c"
assert_not_contains "$STAGED_REPO/unstaged_stdout.txt" "- A added.c"

SENSITIVE_STAGED_REPO="$TMPDIR/sensitive_staged_repo"
mkdir -p "$SENSITIVE_STAGED_REPO"
(cd "$SENSITIVE_STAGED_REPO" && git init -q)
cat >"$SENSITIVE_STAGED_REPO/safe.c" <<'EOF_SENSITIVE_STAGED_BASE'
int safe(void) { return 1; }
EOF_SENSITIVE_STAGED_BASE
(cd "$SENSITIVE_STAGED_REPO" && git add safe.c && \
    git -c user.name='fuori tests' -c user.email='fuori@example.com' commit -qm base)
cat >"$SENSITIVE_STAGED_REPO/safe.c" <<'EOF_SENSITIVE_STAGED_MOD'
int safe(void) { return 2; }
EOF_SENSITIVE_STAGED_MOD
cat >"$SENSITIVE_STAGED_REPO/credentials.txt" <<'EOF_SENSITIVE_STAGED_SECRET'
do not export me
EOF_SENSITIVE_STAGED_SECRET
(cd "$SENSITIVE_STAGED_REPO" && git add safe.c credentials.txt)

(cd "$SENSITIVE_STAGED_REPO" && "$BIN" --staged --no-tree -o - >sensitive_staged_stdout.txt 2>sensitive_staged_stderr.txt)
assert_contains "$SENSITIVE_STAGED_REPO/sensitive_staged_stdout.txt" "## Change Context"
assert_contains "$SENSITIVE_STAGED_REPO/sensitive_staged_stdout.txt" "Files changed: 1"
assert_contains "$SENSITIVE_STAGED_REPO/sensitive_staged_stdout.txt" "- M safe.c"
assert_contains "$SENSITIVE_STAGED_REPO/sensitive_staged_stdout.txt" "## safe.c"
assert_not_contains "$SENSITIVE_STAGED_REPO/sensitive_staged_stdout.txt" "credentials.txt"
assert_contains "$SENSITIVE_STAGED_REPO/sensitive_staged_stderr.txt" "Warning: Skipping sensitive file credentials.txt"

DIFF_REPO="$TMPDIR/diff_repo"
mkdir -p "$DIFF_REPO"
(cd "$DIFF_REPO" && git init -q)
cat >"$DIFF_REPO/shared.c" <<'EOF_DIFF_SHARED_BASE'
int shared(void) { return 1; }
EOF_DIFF_SHARED_BASE
cat >"$DIFF_REPO/old_name.c" <<'EOF_DIFF_OLD_BASE'
int old_name(void) { return 2; }
EOF_DIFF_OLD_BASE
(cd "$DIFF_REPO" && git add shared.c old_name.c && \
    git -c user.name='fuori tests' -c user.email='fuori@example.com' commit -qm base)
(cd "$DIFF_REPO" && git mv old_name.c renamed.c)
cat >"$DIFF_REPO/shared.c" <<'EOF_DIFF_SHARED_MOD'
int shared(void) { return 10; }
EOF_DIFF_SHARED_MOD
cat >"$DIFF_REPO/fresh.c" <<'EOF_DIFF_FRESH'
int fresh(void) { return 3; }
EOF_DIFF_FRESH
(cd "$DIFF_REPO" && git add renamed.c shared.c fresh.c && \
    git -c user.name='fuori tests' -c user.email='fuori@example.com' commit -qm update)

(cd "$DIFF_REPO" && "$BIN" --diff HEAD~1..HEAD --no-tree -o - >diff_stdout.txt 2>diff_stderr.txt)
assert_contains "$DIFF_REPO/diff_stdout.txt" "Mode: diff"
assert_contains "$DIFF_REPO/diff_stdout.txt" "## Change Context"
assert_contains "$DIFF_REPO/diff_stdout.txt" "Files changed: 3"
assert_contains "$DIFF_REPO/diff_stdout.txt" "Diff range: HEAD~1..HEAD"
assert_contains "$DIFF_REPO/diff_stdout.txt" "- A fresh.c"
assert_contains "$DIFF_REPO/diff_stdout.txt" "- M shared.c"
assert_contains "$DIFF_REPO/diff_stdout.txt" "- R old_name.c -> renamed.c"
assert_contains "$DIFF_REPO/diff_stdout.txt" "## fresh.c"
assert_contains "$DIFF_REPO/diff_stdout.txt" "## renamed.c"
assert_contains "$DIFF_REPO/diff_stdout.txt" "## shared.c"

printf 'cli tests passed\n'
