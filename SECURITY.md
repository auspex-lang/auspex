# Security Policy

## Reporting a vulnerability

Please do **not** open a public issue for security reports. Instead:

1. Open a [GitHub security advisory](https://github.com/auspex-lang/auspex/security/advisories/new),
   or
2. contact the maintainer privately via GitHub direct message.

Include a minimal reproducer and the affected commit/version. You should
receive an initial response within a few days.

## Scope

Auspex is a local, single-user scripting prototype. Security considerations
that matter for this project:

- The VM executes bytecode from `.apx` sources; treat any interpreter
  that can be fed untrusted input as out of scope for hardening guarantees.
- SQL is passed through to the active database driver — use `?var`
  parameter binding instead of string concatenation for untrusted data.
- File-I/O natives (`read_file`, `write_file`, `utl_file`, etc.) access
  the host filesystem with the running user's permissions.
- `external_call` loads and executes native code from shared libraries —
  only call libraries you trust.

## Supported versions

Only the latest `main` branch receives security fixes. There are no
backported release branches at this time.
