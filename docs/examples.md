# Examples

Runnable programs in [examples/](../examples/), one per topic. Everything
below (and 35 phase walkthroughs in `examples/phases/`) runs as a CI smoke
test via `make examples-test`.

### Todo list (`examples/todo.apx`)

A minimal CRUD example.

```bash
./bin/auspex examples/todo.apx --db todos.db
```

### Phase feature walkthroughs (`examples/phases/`)

Thirty-five runnable demos, one per shipped feature (exceptions, cursors,
packages, collections, triggers, indexes, views, sequences, FFI, conditional
compilation, and more), numbered by the milestone that added them.

### Data migration (`examples/migration.apx`)

Migrates messy legacy data into a clean schema, normalizing names and
classifying ages along the way.

```bash
./bin/auspex examples/migration.apx --db :memory:
```

Output:

```
Migrated customers: 4
Sample rows:
1: ALICE SMITH (adult)
2: BOB JONES (adult)
3: CHARLIE BROWN (minor)
4: DIANA PRINCE (young adult)
```

### Sales report (`examples/report.apx`)

Aggregates order data into a formatted CLI report with revenue totals,
product breakdowns, and top customers.

```bash
./bin/auspex examples/report.apx --db :memory:
```

### Inventory service (`examples/inventory.apx`)

A small catalog-backed service that lists stock, flags low-stock items, and
processes sales with quantity validation.

```bash
./bin/auspex examples/inventory.apx --db :memory:
```

