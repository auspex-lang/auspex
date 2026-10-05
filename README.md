<p align="center">
  <img src="assets/hero.svg" alt="Auspex — a lightweight, open-source alternative to PL/SQL" width="860">
</p>

<p align="center">
  <a href="https://github.com/auspex-lang/auspex/actions/workflows/main.yml"><img src="https://github.com/auspex-lang/auspex/actions/workflows/main.yml/badge.svg" alt="Tests"></a>
  <a href="https://github.com/auspex-lang/auspex/actions/workflows/windows.yml"><img src="https://github.com/auspex-lang/auspex/actions/workflows/windows.yml/badge.svg" alt="Windows CI"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-green.svg" alt="MIT license"></a>
  <img src="https://img.shields.io/badge/platform-Linux%20%C2%B7%20macOS%20%C2%B7%20Windows-blue.svg" alt="Platforms: Linux, macOS, Windows">
</p>

Auspex (*AW-speks*, formerly MyPL) is a small scripting language with C-like
syntax and SQL in its veins.
It compiles to bytecode for a custom stack VM and runs against either its
built-in SQL engine or SQLite. Stored-procedure scripting without the weight
of an Oracle installation.

```auspex
proc add_todo(title string) -> int {
    insert into todos (title, done) values (?title, 0);
    return 0;
}

proc list_todos() -> int {
    for todo in select id, title from todos {
        print concat(int_to_string(todo.id), concat(": ", todo.title));
    }
    return 0;
}
```

## Quick start

```bash
git clone https://github.com/auspex-lang/auspex.git
cd auspex
make
./bin/auspex examples/todo.apx --db :memory:
```

## Highlights

- **SQL in the language**: inline DDL/DML with `?var` binding,
  `for row in select ...`, `SELECT ... INTO`, explicit cursors, statement-
  and row-level triggers (`before insert`, `for each row`, `:new`/`:old`),
  persistent sequences (`nextval`/`currval`), views (`create view`),
  B-tree indexes, constraints, and three-valued NULLs.
- **A real procedural language**: procedures and functions with
  `in`/`out`/`in out` modes, structs and object types with methods,
  `array<T>`/`map<K,V>` collections, `case`, exception handling,
  Packages with spec/body and persisted state, subtypes, `%TYPE`/`%ROWTYPE`.
- **Batteries included**: `dbms_output`, the `dbms_sql` dynamic-SQL cursor
  API (`dbms_sql.open_cursor`), `utl_file` for host files
  (`utl_file.fopen`), ~90 standard-library natives, regex support, and FFI
  to C libraries via `external_call`/`external_call_sig`.
  On all three platforms.
- **Two SQL engines**: the SQLite backend (`--db <path>`, `.connect`,
  `:memory:`) or the built-in Custom SQL engine with zero dependencies.
- **Hacker-friendly tooling**: a REPL with history and line editing on
  Linux, macOS, and Windows; Conditional compilation with `$if`/`$define`
  and `-DNAME` flags; file imports; a libFuzzer harness with a regression
  corpus; CI on three operating systems.

## Documentation

Full documentation lives in [`docs/`](docs/):

- [`docs/language.md`](docs/language.md) — the language, top to bottom
- [`docs/engines.md`](docs/engines.md) — the two SQL backends and their limits
- [`docs/examples.md`](docs/examples.md) — guided tour of the runnable examples
- `auspex.1` — man page (build, CLI, REPL)

## Build

Default build (needs the `sqlite3` dev library; on Ubuntu
`sudo apt-get install libsqlite3-dev`, on macOS `brew install sqlite3`):

```bash
make clean && make && make test
```

Standalone build without SQLite (custom engine only, `--db` disabled):

```bash
make clean && make USE_SQLITE=0 && make USE_SQLITE=0 test
```

On Windows, build in an [MSYS2](https://www.msys2.org/) MINGW64 shell:

```bash
pacman -S make mingw-w64-x86_64-gcc mingw-w64-x86_64-sqlite3 mingw-w64-x86_64-libsystre
make clean && make && make test
```

The Windows REPL uses the console API for history and in-line editing;
piped input falls back to plain line reading.

## Migrating from MyPL

Auspex was called MyPL up to v0.1.0. Everything MyPL wrote keeps working
through v0.3.x, with a one-line warning on stderr where a legacy name is used:

| MyPL | Auspex | Until v0.4.0 |
| --- | --- | --- |
| `mypl` binary | `auspex` | `make install` also installs a `mypl` symlink |
| `.mypl` sources | `.apx` | `.mypl` files still run (the extension is never checked) |
| `mypl.db` (+ `.packages`/`.programs`) | `auspex.db` | used when only `mypl.db` exists; rename all three files to switch |
| `_mypl_*` tables (SQLite) | `_auspex_*` | renamed automatically the first time the database is opened |
| `MYPL_INDEX_DEBUG` | `AUSPEX_INDEX_DEBUG` | still read |

## Contributing

Contributions and ideas are welcome! See
[`CONTRIBUTING.md`](CONTRIBUTING.md) for the workflow (failing-test-first,
clean rebuilds, sanitizer checks) and [`SECURITY.md`](SECURITY.md) for
reporting vulnerabilities.

## License

[MIT](LICENSE)
