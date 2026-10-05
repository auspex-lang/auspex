# The Auspex language

A tour of the language from top to bottom. For the SQL backends and their
capabilities, see [engines.md](engines.md); runnable versions of everything
below live in [examples/](../examples/).


### Procedures and functions

```auspex
proc greet(name string) -> int {
    print concat("Hello, ", name);
    return 0;
}

func square(x int) -> int {
    return x * x;
}
```

### Variables and types

```auspex
int count = 42;
float pi = 3.14;
string message = "hello";
bool active = true;
date today = current_date();
timestamp now = current_timestamp();
array<int> nums = [1, 2, 3];
map<string, int> ages = {"alice": 30, "bob": 25};
```

### Parameter modes

```auspex
proc swap(in out a int, in out b int) -> int {
    int tmp = a;
    a = b;
    b = tmp;
    return 0;
}
```

### Control flow

```auspex
int i = 0;
while i < 10 {
    print(int_to_string(i));
    i = i + 1;
}

for n in range(1, 5) {
    print(int_to_string(n));
}
```

### Embedded SQL

```auspex
create table users (
    id int primary key,
    name string,
    age int,
    active bool not null default true
);

insert into users values (1, "alice", 30, true);

for user in select id, name from users where age > 25 and active = true {
    print concat(int_to_string(user.id), concat(" ", user.name));
}
```

Column types are `int`, `float`, `string`, `bool`, `date` and `timestamp`. A
`bool` column takes the `true` and `false` literals and reads back into a `bool`
variable; see `examples/bool_columns.apx`. A `date` or `timestamp` column is
written as a string literal in canonical form — `"YYYY-MM-DD"`, or that plus
`" HH:MM:SS"`, which is what `to_date`, `current_date` and `current_timestamp`
produce — and reads back into a `date` or `timestamp` variable, so `to_char`
accepts it. Anything else is refused at write time. Because the text is fixed
width, `ORDER BY`, range comparisons and indexes are chronological; see
`examples/date_columns.apx`.

### SELECT INTO

```auspex
string name = "";
int age = 0;
SELECT name, age INTO name, age FROM users WHERE id = 1;
print concat(name, concat(" is ", int_to_string(age)));
```

Load an entire result set into an `array<row>`:

```auspex
array<row> users = [];
SELECT * INTO users FROM users;
print length(users);
print users[0].name;
```

### Cursors

```auspex
cursor c is select id, name from users where age > 25;
open c;
while c%found {
    int id;
    string name;
    fetch c into id, name;
    print concat(int_to_string(id), concat(" ", name));
}
close c;
```

### Triggers

Statement-level triggers fire once per statement; row-level triggers fire
per affected row with `:new` / `:old` row context. Trigger definitions
persist in the database and survive restarts, and they fire on dynamic SQL
(`execute_immediate`, `dbms_sql.execute`) too.

```auspex
// Statement level: audit every insert into orders.
trigger orders_audit before insert on orders {
    dbms_output.put_line("insert into orders");
}

// Row level: validate each new row before it is written.
trigger orders_check before insert on orders for each row {
    if :new.total < 0 {
        raise_application_error(-20001, "negative total");
    }
}

drop trigger orders_audit;
```

A trigger may not modify the table it fires on, whether directly, through a
proc it calls, through dynamic SQL, or through another table's trigger that
writes back. The statement fails with a runtime error such as
`Cannot modify table 'orders' while its trigger 'orders_check' is running`,
which `try`/`catch` can handle.

### Sequences

Persistent sequences work like Oracle's: `create_sequence`, `nextval`,
`currval`, and `drop_sequence`, with the current value stored in the
database catalog so they continue across process restarts.

```auspex
create_sequence("order_seq", 1000, 1);
insert into orders values (nextval("order_seq"), "alice", 42);
print int_to_string(currval("order_seq"));
```

### Views

The custom engine supports `create view` / `drop view`; a view stores its
`SELECT` and resolves recursively, so it composes with an outer `WHERE`,
`ORDER BY`, and `LIMIT`. Views are read-only.

```auspex
create view big_orders as select id, total from orders where total > 100;
for o in select id from big_orders order by total desc limit 5 {
    print int_to_string(o.id);
}
drop view big_orders;
```

### Indexes, constraints, and NULLs

The custom engine has page-based B-tree indexes (`create index idx on t (col)`,
`drop index idx`), column constraints (`primary key`, `unique`, `not null`,
`default`), and full three-valued NULL semantics with `IS [NOT] NULL`,
`coalesce`, and `nvl`. `WHERE` supports `and`/`or`/`not`, parentheses, `IN`,
and `LIKE`.

### The dbms_sql and utl_file packages

`dbms_sql` offers a cursor-style API for dynamic SQL with bind variables,
and `utl_file` wraps host files (append/seek/flush, mkdir/remove):

```auspex
int c = dbms_sql.open_cursor();
dbms_sql.parse(c, "insert into orders values (?1, ?2, ?3)");
dbms_sql.bind_variable(c, "1", 7);
dbms_sql.bind_variable(c, "2", "bob");
dbms_sql.bind_variable(c, "3", 19);
int n = dbms_sql.execute_cursor(c);
dbms_sql.close_cursor(c);

int f = utl_file.fopen("/tmp/log.txt", "a");
utl_file.put_line(f, "appended line");
utl_file.fclose(f);
```

`utl_file.get_line` returns the next line, of any length, without its line
ending. Past the last line it raises `no_data_found` (`sqlcode` 100), as
PL/SQL's `UTL_FILE` does, so a reader can loop until that is caught.

### Calling native libraries (external_call)

`external_call` invokes a C function from a shared library via
`dlopen`/`dlsym` (`LoadLibrary`/`GetProcAddress` on Windows). The native name selects the C return type; the argument's
C type follows its Auspex runtime type (`int`, `float`, or `string`):

```auspex
// double sqrt(double) from libm
float root = external_call_float("libm.so.6", "sqrt", 2.0);
// size_t strlen(const char*) from libc — int return
int n = external_call("libc.so.6", "strlen", "hello");
// const char* getenv(const char*) — string return (NULL becomes null)
string home = external_call_string("libc.so.6", "getenv", "HOME");
```

For more than one argument, `external_call_sig` takes a signature that reads
like the C prototype: the return type, then the argument types in
parentheses, with `i` for int, `d` for float (C `double`) and `s` for string
(`const char*`), and up to four arguments. The signature must be a string
literal; the compiler checks the arguments against it and gives the call its
return type. It has to match the C prototype exactly: `i` is a C `int`, so a
`long` or `size_t` parameter is not one.

```auspex
// double pow(double, double)
float p = external_call_sig("libm.so.6", "pow", "d(dd)", 2.0, 10.0);
// double ldexp(double, int)
float x = external_call_sig("libm.so.6", "ldexp", "d(di)", 1.5, 3);
// int strcmp(const char*, const char*)
int order = external_call_sig("libc.so.6", "strcmp", "i(ss)", "apple", "banana");
```

### Collections

```auspex
array<int> nums;
nums.extend(3);
nums[0] = 10;
nums[1] = 20;
nums[2] = 30;
nums.sort();
print int_to_string(nums[0]);  // 10

map<string, int> scores;
scores["ada"] = 95;
print int_to_string(scores["ada"]);
```

### Type attributes and subtypes

```auspex
// %TYPE copies a variable or column type
int x = 42;
x%type y = 7;

// Subtypes create aliases
subtype score is int;
score s = 100;

// %ROWTYPE creates a row/record matching a table
// (the table must already exist when the program is compiled)
users%rowtype u;
u.id = 2;
u.name = "bob";
print u.name;
```

### Exceptions

```auspex
proc maybe_fetch() -> int {
    int id;
    begin
        select id into id from users where name = "nobody";
    catch (err) {
        print concat("SQL error: ", err);
    }
    return 0;
}
```

### Packages

```auspex
package math_utils is
    func add(a int, b int) -> int;
end math_utils;

package body math_utils is
    func add(a int, b int) -> int {
        return a + b;
    }
end math_utils;
```

### Conditional compilation

Use line-oriented directives to include code for selected builds. Flags can be
defined in source with `$define` or supplied when running a file with `-DNAME`:

```auspex
proc main() -> int {
$if DEBUG $then
    print "debug logging enabled";
$else
    print "release mode";
$end
    return 0;
}
```

```bash
./bin/auspex -DDEBUG program.apx
./bin/auspex -DDEBUG -DTRACE program.apx
```

Command-line flags are boolean, may be repeated, and apply to the input file
and its imported modules. Use `$undefine NAME` within a source file to disable
a flag for the rest of that compilation unit.

One flag names the platform Auspex runs on: `PLATFORM_LINUX`, `PLATFORM_MACOS`
or `PLATFORM_WINDOWS`, plus `PLATFORM_POSIX` on Linux and macOS. They are
always defined, which lets a program choose, for instance, the library
`external_call` loads:

```auspex
$if PLATFORM_WINDOWS $then
    string libc = "msvcrt.dll";
$elsif PLATFORM_MACOS $then
    string libc = "/usr/lib/libSystem.B.dylib";
$else
    string libc = "libc.so.6";
$end
```



## REPL
Start an interactive session:

```bash
./bin/auspex
```

Useful commands:

```
> .connect :memory:
> create table todos (id integer primary key, title string, done int);
> .tables
> .schema todos
> .sql select * from todos;
> .exit
```

## Also in the box

The tour above covers the highlights; the rest of the language is a short
list:

- `struct` records and object types with methods
  (`examples/phases/phase10_object_types.apx`).
- `case` statements, `do ... while`, and numeric `for` loops.
- Anonymous `declare ... begin ... end;` blocks.
- `BULK COLLECT INTO` and `FORALL` for set-based fetching and DML.
- Table functions returning `array<row>`, iterable from SQL.
- Named predefined and user-defined exceptions, `raise`, `sqlcode`, `sqlerrm`.
- A standard library: `length`, `concat`, `split`, `join`, `replace`, `trim`,
  case conversions, `parse_int`, `range`, `sort`, `reverse`, `clamp`,
  `format`, `assert`, date/time and regex natives, and file I/O.

