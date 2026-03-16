# Design Notes

This document records the design boundaries of `fuori`.
It is intentionally short. If a detail belongs in user-facing docs or tests, it should not be repeated here unless it reflects a stable architectural choice.

## Primary Goal

`fuori` is a small, dependency-light Unix CLI that produces a single Markdown artifact for LLM context packing and code review.

Design bias:

- predictable CLI behavior
- easy-to-review C99/POSIX implementation
- minimal dependencies and simple build flow
- practical, trustworthy behavior over abstraction purity

## Non-Goals

`fuori` is not intended to become a context platform.

The project should continue to avoid:

- template engines and multiple output formats
- remote cloning or repository discovery features
- TUIs, MCP layers, or service-style integrations
- embedded parser/tokenizer stacks unless the project scope changes materially
- full Git ignore parity beyond what the current filesystem walker needs

## Selection Model

Selection and rendering are kept separate on purpose.

- Auto mode prefers Git's view of the current subtree and falls back quietly to recursive filesystem walking when Git is unavailable.
- `--no-git` forces filesystem selection.
- `--staged`, `--unstaged`, and `--diff` stay explicitly Git-dependent.
- `--from-stdin` only supplies candidate paths; it does not bypass normal export-time checks.
- Output ordering is deterministic rather than preserving caller input order.

Why:

- Git-backed default behavior is usually the most correct repository view.
- Filesystem fallback preserves portability and keeps the tool useful outside Git repos.
- Stdin remains the Unix escape hatch without introducing another export pipeline.

## Filtering and Safety

The collector should be conservative.

- Exportable files are biased toward UTF-8-like source text.
- Symlinks, binary or invalid-text files, oversized files, and self-output paths are skipped.
- Secret protection stays simple and default-on: high-signal filename and content checks, generic warnings, explicit `--allow-sensitive` override.
- File output remains atomic via temporary-file write plus `rename`.

The goal is not perfect classification. The goal is to avoid obviously bad exports while keeping the implementation understandable.

## Artifact Shape

The Markdown artifact should explain itself with a small amount of grounded metadata.

- The header carries factual export context such as repository, mode, generation time, and conditional review metadata like `Line numbers: on`.
- `Change Context` is reserved for Git delta modes and reflects the subset of selected files that actually survive export-time filtering.
- Line numbers are opt-in because they are useful for review but noisy for pure context packing.
- The project tree is optional and should reflect the final exported artifact, not the raw filesystem.

These are not presentation flourishes. They exist to help a reader interpret what the artifact represents.

## Rendering and Metrics

Markdown-specific concerns belong in the renderer, not in shared collection structures.

- Shared export data stays format-neutral.
- Renderer-owned preparation is acceptable when it prevents leaking Markdown details into collection types.
- Byte counting and output writing should share the same formatting path where practical so estimates do not drift from emitted output.
- Token budgeting is based on the final Markdown artifact, not raw source bytes.

This keeps size checks honest and makes review-oriented formatting features, such as line numbers, a rendering concern instead of a collection concern.

## Maintenance Rule

Keep this document brief and opinionated.

Update it when:

- a new feature changes the project's design boundaries
- a contributor might reasonably ask "why is it shaped this way?"

Do not update it for routine feature inventory, CLI examples, or behavior that is already obvious from tests and the README.
