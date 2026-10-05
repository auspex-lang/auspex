# Contributing to Auspex

Thanks for taking the time to contribute! This document explains how to get
started, what we expect from patches, and how the workflow looks.

## Ground rules

- **Language:** C99 (`-std=c99 -D_GNU_SOURCE`). No C++.
- **Dependencies:** SQLite is the only optional dependency
  (`USE_SQLITE=0` builds a standalone binary). No external libraries beyond
  libc/libm and, optionally, sqlite3.
- **Style:** `snake_case` for functions and variables, `PascalCase` for
  structs/enums, `ALL_CAPS` for macros and constants. Match the surrounding
  file — the codebase is uniform on purpose.
- **Warnings:** The CI `warnings-as-errors` job builds with `-Werror`. Keep
  the tree warning-free under `-Wall -Wextra`.
- **Commit attribution:** Commit with your GitHub-linked email — ideally
  your `ID+username@users.noreply.github.com` address (GitHub → Settings →
  Emails → "Keep my email addresses private"). The
  `attribution-check` workflow rejects PRs whose commits use unknown author
  emails or AI-tool co-author trailers, because every stray email shows up
  as a phantom contributor on the repo home page. Set it once with:
  `git config --global user.email "ID+username@users.noreply.github.com"`.

## Getting set up

```bash
git clone https://github.com/auspex-lang/auspex.git
cd auspex
make clean && make && make test
```

For a SQLite-free build:

```bash
make clean && make USE_SQLITE=0 && make USE_SQLITE=0 test
```

## Before you submit a patch

1. **Write a failing test first.** Every feature and bugfix starts as a
   failing test in `tests/test_*.c` using the harness in
   `tests/test_harness.h`. Existing tests must stay green — `make test`
   must exit 0.
2. **New test files:** add the file to both the compile and run sections of
   the `Makefile` `test` target. Guard with the `USE_SQLITE` ifdef only if
   the tests genuinely require SQLite.
3. **Rebuild from clean.** The Makefile has no header dependencies: after
   editing anything in `include/`, run `make clean && make` or stale objects
   will silently produce ABI mismatches.
4. **Sanitizers.** If you touched memory management (most things do — the
   runtime uses reference counting), check the sanitizer build:

   ```bash
   make clean && make CFLAGS="-Wall -Wextra -std=c99 -D_GNU_SOURCE -Iinclude -DUSE_SQLITE -fsanitize=address,undefined -fno-omit-frame-pointer" LDFLAGS="-lm -lsqlite3 -fsanitize=address,undefined"
   ASAN_OPTIONS=detect_leaks=0 make test
   ```

   Run `act -j test`, `act -j warnings-as-errors`, `act -j asan-ubsan` if you
   have `act` locally.
5. **Examples.** New features should come with a runnable example in
   `examples/` — keep the existing ones working. `make test` runs every
   example through `tests/run_examples.sh` (also `make examples-test`) and
   requires exit 0. If an example needs arguments, another example run first,
   SQLite or Linux, say so with `// smoke:` comment lines (`args --db :memory:`,
   `setup other.apx`, `requires sqlite`, `requires linux`, `skip <reason>`).
6. **AGENTS.md** is a gitignored handoff document: update it when you change
   architecture, conventions, or known limitations.

## Fuzzing

`tests/fuzz` has libFuzzer targets for the lexer (`fuzz_lexer.c`), the parser
(`fuzz_parser.c`) and the conditional-compilation preprocessor
(`fuzz_preprocessor.c`; the first input byte selects which `-D` flags are
set). They check for crashes, sanitizer errors and a few invariants, e.g. the
preprocessor keeping the source length and every newline in place.

```bash
make fuzz                 # build bin/fuzz_* with clang (FUZZ_CC=...)
make fuzz-run FUZZ_TIME=300   # add FUZZ_LEAKS=0 to silence leak reports
```

New corpus entries and crash inputs land in `build/fuzz/`. To keep a crash
fixed, minimize it (`bin/fuzz_parser -minimize_crash=1 -runs=10000 <input>`),
commit it under `tests/fuzz/regressions/<target>/` and add a unit test.
`make test` replays the seed corpus and every regression input with your
normal compiler via `make fuzz-replay`, so no clang is needed there. The
`Fuzz` workflow runs each target for 60 s on PRs and 10 min weekly.

## What not to commit

The following are gitignored and must never be committed: `AGENTS.md`,
`docs/`, `ARCHITECTURE.md`, `bin/`, `build/`, `*.db`, IDE files, and
graphify output. Check `.gitignore` before `git add -A`.

## Workflow

1. **Issues first.** Look for an open issue describing the work
   (roadmap phases are tracked as issues and on the
   [Auspex Roadmap](https://github.com/users/LPuehringerStudent/projects/9)
   project). If none exists, open one.
2. **Branch per issue.** `git checkout -b issue-<number>-short-title`.
3. **One logical change per PR.** Keep diffs focused; no drive-by
   reformatting.
4. **PRs** run CI on Ubuntu and macOS (gcc and clang) and on Windows
   (MinGW-w64 under MSYS2). Wait for green before merging; prefer squash
   merge for feature branches.

## Code of conduct

This project follows the [Contributor Covenant](CODE_OF_CONDUCT.md). Be
respectful and constructive in issues, reviews, and discussions.
