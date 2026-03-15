# Design Notes

This document captures the main architectural decisions behind `fuori`.
It is intentionally short and focused on decisions contributors are likely to touch.

## Primary Goal

`fuori` is a small, dependency-light Unix CLI that exports a codebase into a single Markdown artifact for LLM and review workflows.

Design bias:

- predictable CLI behavior
- easy-to-review C99/POSIX implementation
- minimal dependencies and simple build flow
- practical behavior for real repositories over perfect abstraction purity

## Design Strengths

These implementation choices are worth preserving because they match the tool's scope well:

- Git integration is pragmatic: `fuori` relies on the system `git` binary instead of a heavier embedded Git library, while still falling back to the filesystem walker outside repositories.
- Subprocess handling is defensive: Git commands use careful fork/exec handling so `execvp` failures can be reported reliably rather than inferred indirectly.
- Content filtering is intentionally strict: the collector is biased toward exporting UTF-8-like source text and skipping inputs that are likely to pollute LLM context.
- Token estimation is artifact-based: warnings and hard limits are derived from the final rendered Markdown structure rather than just raw source bytes.
- Output handling is atomic: token-limit refusal happens before destination mutation, file output uses `mkstemp` plus `rename`, and temp/final output files are excluded from collection via inode/device checks.
- Git-backed and filesystem-backed selection stay cleanly separated: auto mode prefers Git and falls back quietly, while explicit Git modes remain hard Git-dependent and preserve subtree scoping.
- Rendering and metrics are kept consistent: fence sizing is precomputed once, metrics are based on the actual rendered structure, and tree bytes are counted separately but within the same accounting model.
- Hardening remains pragmatic rather than elaborate: `O_NOFOLLOW` is used when available, opened files are verified by device/inode, Git-selected paths are deduplicated, and output ordering is deterministic.

## File Selection Model

`fuori` distinguishes between requested selection mode and resolved selection mode.

- Default behavior starts in auto mode.
- Inside a Git repository, auto mode prefers Git's view of the current subtree.
- Outside Git, or when Git is unavailable, auto mode falls back silently to the recursive filesystem walker.
- `--no-git` forces the filesystem walker.
- Explicit Git modes such as `--staged`, `--unstaged`, and `--diff` remain hard Git-dependent modes.

Why:

- Git-backed default mode gives correct repository-aware behavior for most real projects.
- Filesystem fallback preserves portability for unpacked archives, non-Git directories, and simple local use.

## Ignore Behavior

There are two ignore paths by design:

- Git-backed selection uses Git as the source of truth for tracked files and untracked non-ignored files.
- Filesystem recursion uses the local ignore engine in `src/ignore.c`.

The local ignore engine exists to support:

- non-Git directories
- `--no-git`
- automatic fallback outside repositories

It supports common `.gitignore`-style matching, including recursive `**` globs, but it is not intended to reimplement Git's full layered ignore model.

## In-Memory Export Plan

Accepted files are read into memory and stored in the export plan before rendering.

Why this is intentional:

- the tool enforces a per-file size cap
- rendering and byte/token estimation both need accepted file contents
- caching avoids rereading files during later phases
- the implementation stays simple and deterministic

This is a good tradeoff for the current scope. Streaming should only be revisited if the project explicitly targets much larger exports or lower peak memory usage.

## Rendering and Metrics

The renderer emits Markdown, while tree helpers and renderer-local metadata preparation support that flow.

Important constraints:

- shared export structures stay format-neutral
- Markdown-specific metadata must not leak into shared collection types
- estimation and emission should consume the same prepared renderer data where possible

This is why fence-length preparation is renderer-owned rather than stored on `ExportEntry`.

## Output Safety Guarantees

The tool is designed to avoid mutating the destination unless export preconditions pass.

Notable guarantees:

- token-limit refusal happens before destination mutation
- `--no-clobber` preserves existing files
- final output and temporary output files are excluded from export collection
- normal file output writes via a temporary file and rename path

The CLI integration tests cover these guarantees and should be extended whenever output lifecycle behavior changes.

## Repository Layout

The repository uses a small-app layout:

- `src/` for production code
- `tests/` for test assets
- `docs/` for contributor-facing design notes
- root for project-level files and GitHub metadata

This keeps the GitHub root readable without introducing library-style structure that the project does not need.
