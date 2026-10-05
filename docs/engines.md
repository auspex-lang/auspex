# The two SQL engines

Auspex speaks one dialect of embedded SQL but can run it on two very different
backends. You pick per invocation — there is no shared storage format between
them.

## SQLite backend (`--db <path>`, `.connect`)

The full-featured option. Auspex embeds the SQL text and hands it to SQLite,
so anything SQLite can express (JOINs, subqueries, window functions, …) works,
with `?var` bind parameters substituted safely. `:memory:` is accepted for
scratch work. In the REPL, `.connect <path>` switches sessions.

- Row-level triggers snapshot affected rows with `SELECT rowid, *` around the
  DML; a BEFORE row trigger therefore sees the row image around the completed
  statement rather than the exact pre-write image.
- `?var` parameters in the WHERE clause of a row-triggered UPDATE/DELETE are
  rejected with a clear runtime error.
- Package sources persist in `_auspex_packages`, triggers/procs in
  `_auspex_program_units`, sequences in `_auspex_sequences`. A database
  written by MyPL (tables named `_mypl_*`) has its tables renamed the first
  time Auspex opens it.

## Custom SQL engine (the default, no SQLite needed)

A small engine built into the binary: a file-backed catalog page plus row
pages, secondary indexes as page-based B+ trees, and a growing catalog format
(currently V5) that also stores constraints, views, and sequences. Without
`--db`, the CLI opens `auspex.db` in the working directory (or a MyPL-era
`mypl.db`, with a warning, if only that one exists).

Supported:

- DDL: `create table [if not exists]`, `drop table [if exists]`,
  `alter table … add/drop column`, `create/drop index`, `create/drop view`.
- DML: `insert`, `insert … select`, `update`, `delete`.
- Queries: `select` with `where` (`and`/`or`/`not`, parentheses, `IN`, `LIKE`,
  all six comparison operators), `order by`, `limit`.
- Three-valued NULL semantics: stored NULL cells, `IS [NOT] NULL`, aggregates
  skipping NULLs (`COUNT(*)` vs `COUNT(col)`).
- Constraints: `primary key`, `unique`, `not null`, `default`.
- Indexes accelerate `where` terms that compare an indexed column to a
  literal; candidates are re-checked against the full WHERE, so results always
  match a full scan.
- Views store their SELECT verbatim and resolve recursively (views over
  views work); they compose with an outer WHERE/ORDER BY/LIMIT and are
  read-only.
- Sequences persist in the catalog (`create_sequence`, `nextval`, `currval`,
  `drop_sequence`).

Known limitations (deliberate, documented):

- `?var` bind parameters are **not** substituted in static SQL — only literal
  SQL works without `--db`. The `dbms_sql` API (`bind_variable` substitutes
  `?N` into the text at execute time) works on both engines, as does
  `execute_immediate` with pre-built text.
- Index keys are bytewise and only the first 36 bytes are significant; key
  order is NULL < int < float < string. B-tree deletion removes the leaf
  entry without rebalancing — the SQL layer rebuilds indexes wholesale
  whenever the row chain is rewritten, so consistency is maintained at the
  cost of a rebuild.
- Views cannot be JOIN targets (JOIN sides must be base tables). Dropping a
  base table leaves its views behind; using them then fails at runtime.
- Aggregates and GROUP BY are limited compared to SQLite.
- Column types are `int`, `float`, `string`, `bool`, `date`, `timestamp`.

Set `AUSPEX_INDEX_DEBUG=1` (MyPL's `MYPL_INDEX_DEBUG` still works) to trace
index-assisted lookups on stderr.
