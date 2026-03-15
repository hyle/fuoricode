# Contributing

Thanks for contributing to fuoricode.

## Development

Build locally:

```bash
make
```

Run:

```bash
./fuori --help
```

For behavior changes, also do a manual sanity check of the affected code path.

Repository layout:

- contributor-facing design notes live in `docs/design.md`
- production code lives in `src/`
- test assets live in `tests/`
- project-level docs and build files stay at the repo root

## Scope

Please keep changes aligned with the project's goals:

- small, dependency-light Unix CLI
- C99 / POSIX-focused implementation
- simple and predictable CLI behavior

## Before Opening A PR

- keep patches focused and small when possible
- update `README.md` if flags or behavior change
- update help text if CLI options change
- avoid unnecessary dependencies or build complexity
- include the manual verification you performed

## Style

- prefer clear, straightforward C
- preserve portability across supported Unix-like systems
- avoid unrelated refactors in feature or fix PRs

## Pull Requests

Please include:

- what changed
- why it changed
- any user-visible behavior changes
- example usage when relevant

## Releases

Tagged releases are built by GitHub Actions from tags matching `v*`.
If you change release packaging or workflow behavior, include that in the PR description.
