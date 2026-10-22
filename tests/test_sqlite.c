#include "test_harness.h"
#include <stdio.h>
#include <unistd.h>
#include <sqlite3.h>
#include "sql_engine.h"
#include "compiler.h"
#include "sqlite_driver.h"
#include "vm.h"

TEST(sqlite3_is_linked) {
    sqlite3* db = NULL;
    int rc = sqlite3_open(":memory:", &db);
    ASSERT_INT_EQ(SQLITE_OK, rc);
    sqlite3_close(db);
}

TEST(sqlite_driver_crud) {
    DBDriver driver;
    sqlite_driver_init(&driver);
    int rc = driver.open(&driver, ":memory:");
    ASSERT_INT_EQ(1, rc);
    rc = driver.exec(&driver, "CREATE TABLE t (id INTEGER, name TEXT)", NULL, 0);
    ASSERT_INT_EQ(1, rc);
    rc = driver.exec(&driver, "INSERT INTO t VALUES (1, 'alice')", NULL, 0);
    ASSERT_INT_EQ(1, rc);

    void* result = NULL;
    rc = driver.query(&driver, "SELECT id, name FROM t", NULL, 0, &result);
    ASSERT_INT_EQ(1, rc);

    void* row = NULL;
    rc = driver.result_next(&driver, result, &row);
    ASSERT_INT_EQ(1, rc);

    Value id;
    rc = driver.row_get_field(&driver, row, "id", &id);
    ASSERT_INT_EQ(1, rc);
    ASSERT_INT_EQ(VAL_INT, id.type);
    ASSERT_INT_EQ(1, id.as.as_int);

    Value name;
    rc = driver.row_get_field(&driver, row, "name", &name);
    ASSERT_INT_EQ(1, rc);
    ASSERT_INT_EQ(VAL_STRING, name.type);
    ASSERT_STRING_EQ("alice", name.as.as_string);

    driver.result_free(&driver, result);
    driver.close(&driver);
}

TEST(sqlite_driver_parameter_binding) {
    DBDriver driver;
    sqlite_driver_init(&driver);
    ASSERT_INT_EQ(1, driver.open(&driver, ":memory:"));
    ASSERT_INT_EQ(1, driver.exec(&driver, "CREATE TABLE t (id INTEGER, name TEXT)", NULL, 0));

    Value params[2];
    params[0] = value_int(42);
    params[1] = value_string(strdup("bob"));
    ASSERT_INT_EQ(1, driver.exec(&driver, "INSERT INTO t VALUES (?, ?)", params, 2));

    Value qparams[1];
    qparams[0] = value_int(42);
    void* result = NULL;
    ASSERT_INT_EQ(1, driver.query(&driver, "SELECT id, name FROM t WHERE id = ?", qparams, 1, &result));

    void* row = NULL;
    ASSERT_INT_EQ(1, driver.result_next(&driver, result, &row));

    Value name;
    ASSERT_INT_EQ(1, driver.row_get_field(&driver, row, "name", &name));
    ASSERT_STRING_EQ("bob", name.as.as_string);

    value_release(params[1]);
    driver.result_free(&driver, result);
    driver.close(&driver);
}

TEST(sqlite_end_to_end_ddl_and_dml) {
    DBDriver driver;
    sqlite_driver_init(&driver);
    ASSERT_INT_EQ(1, driver.open(&driver, ":memory:"));

    Chunk chunk;
    init_chunk(&chunk);
    char error[256];
    int ok = compile_with_context_and_path(
        "proc main() -> int { create table t (id int); insert into t values (1); return 0; }",
        &chunk, NULL, error, sizeof(error), NULL);
    ASSERT_INT_EQ(1, ok);

    VM* vm = vm_init();
    vm_set_driver(vm, &driver);
    ASSERT_INT_EQ(INTERPRET_OK, vm_interpret(vm, &chunk));

    vm_free(vm);
    free_chunk(&chunk);
    driver.close(&driver);
}

TEST(sqlite_end_to_end_parameter_binding) {
    DBDriver driver;
    sqlite_driver_init(&driver);
    ASSERT_INT_EQ(1, driver.open(&driver, ":memory:"));

    Chunk chunk;
    init_chunk(&chunk);
    char error[256];
    int ok = compile_with_context_and_path(
        "proc main() -> int { string name = \"alice\"; create table t (id int, name string); insert into t values (1, ?name); return 0; }",
        &chunk, NULL, error, sizeof(error), NULL);
    ASSERT_INT_EQ(1, ok);

    VM* vm = vm_init();
    vm_set_driver(vm, &driver);
    ASSERT_INT_EQ(INTERPRET_OK, vm_interpret(vm, &chunk));

    void* result = NULL;
    ASSERT_INT_EQ(1, driver.query(&driver, "SELECT name FROM t WHERE id = 1", NULL, 0, &result));
    void* row = NULL;
    ASSERT_INT_EQ(1, driver.result_next(&driver, result, &row));
    Value name;
    ASSERT_INT_EQ(1, driver.row_get_field(&driver, row, "name", &name));
    ASSERT_INT_EQ(VAL_STRING, name.type);
    ASSERT_STRING_EQ("alice", name.as.as_string);
    value_release(name);
    driver.result_free(&driver, result);

    vm_free(vm);
    free_chunk(&chunk);
    driver.close(&driver);
}

TEST(sqlite_end_to_end_for_loop_parameter_binding) {
    DBDriver driver;
    sqlite_driver_init(&driver);
    ASSERT_INT_EQ(1, driver.open(&driver, ":memory:"));

    Chunk chunk;
    init_chunk(&chunk);
    char error[256];
    int ok = compile_with_context_and_path(
        "proc main() -> string { int id = 2; create table t (id int, name string); insert into t values (1, \"alice\"); insert into t values (2, \"bob\"); for row in SELECT name FROM t WHERE id = ?id { return row.name; } return \"none\"; }",
        &chunk, NULL, error, sizeof(error), NULL);
    ASSERT_INT_EQ(1, ok);

    VM* vm = vm_init();
    vm_set_driver(vm, &driver);
    ASSERT_INT_EQ(INTERPRET_OK, vm_interpret(vm, &chunk));

    Value result = vm_pop(vm);
    ASSERT_INT_EQ(VAL_STRING, result.type);
    ASSERT_STRING_EQ("bob", result.as.as_string);
    value_release(result);

    vm_free(vm);
    free_chunk(&chunk);
    driver.close(&driver);
}

TEST(sqlite_end_to_end_for_loop_string_parameter_binding) {
    DBDriver driver;
    sqlite_driver_init(&driver);
    ASSERT_INT_EQ(1, driver.open(&driver, ":memory:"));

    Chunk chunk;
    init_chunk(&chunk);
    char error[256];
    int ok = compile_with_context_and_path(
        "proc main() -> int { string name = \"bob\"; create table t (id int, name string); insert into t values (1, \"alice\"); insert into t values (2, \"bob\"); for row in SELECT id FROM t WHERE name = ?name { return row.id; } return -1; }",
        &chunk, NULL, error, sizeof(error), NULL);
    ASSERT_INT_EQ(1, ok);

    VM* vm = vm_init();
    vm_set_driver(vm, &driver);
    ASSERT_INT_EQ(INTERPRET_OK, vm_interpret(vm, &chunk));

    Value result = vm_pop(vm);
    ASSERT_INT_EQ(VAL_INT, result.type);
    ASSERT_INT_EQ(2, result.as.as_int);
    value_release(result);

    vm_free(vm);
    free_chunk(&chunk);
    driver.close(&driver);
}

TEST(sqlite_end_to_end_select_into_single_value) {
    DBDriver driver;
    sqlite_driver_init(&driver);
    ASSERT_INT_EQ(1, driver.open(&driver, ":memory:"));

    Chunk chunk;
    init_chunk(&chunk);
    char error[256];
    int ok = compile_with_context_and_path(
        "proc main() -> int { int my_id = 0; create table t (id int, name string); insert into t values (1, \"alice\"); SELECT id INTO my_id FROM t WHERE name = \"alice\"; return my_id; }",
        &chunk, NULL, error, sizeof(error), NULL);
    ASSERT_INT_EQ(1, ok);

    VM* vm = vm_init();
    vm_set_driver(vm, &driver);
    ASSERT_INT_EQ(INTERPRET_OK, vm_interpret(vm, &chunk));

    Value result = vm_pop(vm);
    ASSERT_INT_EQ(VAL_INT, result.type);
    ASSERT_INT_EQ(1, result.as.as_int);
    value_release(result);

    vm_free(vm);
    free_chunk(&chunk);
    driver.close(&driver);
}

TEST(sqlite_end_to_end_select_into_multi_value) {
    DBDriver driver;
    sqlite_driver_init(&driver);
    ASSERT_INT_EQ(1, driver.open(&driver, ":memory:"));

    Chunk chunk;
    init_chunk(&chunk);
    char error[256];
    int ok = compile_with_context_and_path(
        "proc main() -> int { int my_id = 0; string my_name = \"\"; create table t (id int, name string); insert into t values (1, \"alice\"); SELECT id, name INTO my_id, my_name FROM t WHERE name = \"alice\"; return my_id; }",
        &chunk, NULL, error, sizeof(error), NULL);
    ASSERT_INT_EQ(1, ok);

    VM* vm = vm_init();
    vm_set_driver(vm, &driver);
    ASSERT_INT_EQ(INTERPRET_OK, vm_interpret(vm, &chunk));

    Value result = vm_pop(vm);
    ASSERT_INT_EQ(VAL_INT, result.type);
    ASSERT_INT_EQ(1, result.as.as_int);
    value_release(result);

    vm_free(vm);
    free_chunk(&chunk);
    driver.close(&driver);
}

TEST(sqlite_end_to_end_select_into_array_row) {
    DBDriver driver;
    sqlite_driver_init(&driver);
    ASSERT_INT_EQ(1, driver.open(&driver, ":memory:"));

    Chunk chunk;
    init_chunk(&chunk);
    char error[256];
    int ok = compile_with_context_and_path(
        "proc main() -> int { array<row> rows = []; create table t (id int, name string); insert into t values (1, \"alice\"); insert into t values (2, \"bob\"); SELECT * INTO rows FROM t; return length(rows) + rows[0].id; }",
        &chunk, NULL, error, sizeof(error), NULL);
    ASSERT_INT_EQ(1, ok);

    VM* vm = vm_init();
    vm_set_driver(vm, &driver);
    ASSERT_INT_EQ(INTERPRET_OK, vm_interpret(vm, &chunk));

    Value result = vm_pop(vm);
    ASSERT_INT_EQ(VAL_INT, result.type);
    ASSERT_INT_EQ(3, result.as.as_int);
    value_release(result);

    vm_free(vm);
    free_chunk(&chunk);
    driver.close(&driver);
}

TEST(sqlite_reports_source_line_on_sql_error) {
    DBDriver driver;
    sqlite_driver_init(&driver);
    ASSERT_INT_EQ(1, driver.open(&driver, ":memory:"));

    Chunk chunk;
    init_chunk(&chunk);
    char error[256];
    int ok = compile_with_context_and_path(
        "proc main() -> int {\n"
        "    int x = 0;\n"
        "    SELECT id INTO x FROM missing_table;\n"
        "    return x;\n"
        "}",
        &chunk, NULL, error, sizeof(error), NULL);
    ASSERT_INT_EQ(1, ok);

    VM* vm = vm_init();
    vm_set_driver(vm, &driver);
    ASSERT_INT_EQ(INTERPRET_RUNTIME_ERROR, vm_interpret(vm, &chunk));
    const char* msg = vm_get_error(vm);
    ASSERT_PTR_NOT_NULL(msg);
    ASSERT_INT_EQ(1, strstr(msg, "3:") != NULL);
    ASSERT_INT_EQ(1, strstr(msg, ": error:") != NULL);
    ASSERT_INT_EQ(1, strstr(msg, "SQL error") != NULL);

    vm_free(vm);
    free_chunk(&chunk);
    driver.close(&driver);
}

/* Implicit transactions: the driver opens a transaction at the first write and
   commits it on close (or flush_implicit), so N statements cost one fsync
   instead of N. A second connection observes what is committed. */
static int other_connection_count(const char* path, const char* sql) {
    sqlite3* db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) return -2;
    sqlite3_stmt* st = NULL;
    int n = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) {
        n = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return n;
}

static void implicit_db_path(char* buf, size_t size) {
    snprintf(buf, size, "/tmp/auspex_test_implicit_%d.db", (int)getpid());
    unlink(buf);
}

static const char* TABLE_COUNT_SQL =
    "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='t'";

TEST(sqlite_implicit_transaction_held_until_close) {
    char path[128];
    implicit_db_path(path, sizeof(path));
    DBDriver driver;
    sqlite_driver_init(&driver);
    ASSERT_INT_EQ(1, driver.open(&driver, path));
    ASSERT_INT_EQ(1, driver.exec(&driver, "CREATE TABLE t (id INTEGER)", NULL, 0));
    ASSERT_INT_EQ(1, driver.exec(&driver, "INSERT INTO t VALUES (1)", NULL, 0));
    ASSERT_INT_EQ(0, sqlite3_get_autocommit(((SQLiteImpl*)driver.impl)->db));
    ASSERT_INT_EQ(0, other_connection_count(path, TABLE_COUNT_SQL));
    driver.close(&driver);
    ASSERT_INT_EQ(1, other_connection_count(path, "SELECT count(*) FROM t"));
    unlink(path);
}

TEST(sqlite_implicit_transaction_not_opened_by_reads) {
    DBDriver driver;
    sqlite_driver_init(&driver);
    ASSERT_INT_EQ(1, driver.open(&driver, ":memory:"));
    void* result = NULL;
    ASSERT_INT_EQ(1, driver.query(&driver, "SELECT 1", NULL, 0, &result));
    driver.result_free(&driver, result);
    ASSERT_INT_EQ(1, sqlite3_get_autocommit(((SQLiteImpl*)driver.impl)->db));
    driver.close(&driver);
}

TEST(sqlite_implicit_transaction_flush_commits) {
    char path[128];
    implicit_db_path(path, sizeof(path));
    DBDriver driver;
    sqlite_driver_init(&driver);
    ASSERT_INT_EQ(1, driver.open(&driver, path));
    ASSERT_INT_EQ(1, driver.flush_implicit(&driver)); /* nothing open: no-op */
    ASSERT_INT_EQ(1, driver.exec(&driver, "CREATE TABLE t (id INTEGER)", NULL, 0));
    ASSERT_INT_EQ(1, driver.flush_implicit(&driver));
    ASSERT_INT_EQ(1, sqlite3_get_autocommit(((SQLiteImpl*)driver.impl)->db));
    ASSERT_INT_EQ(1, other_connection_count(path, TABLE_COUNT_SQL));
    driver.close(&driver);
    unlink(path);
}

/* Transaction control sees what autocommit used to leave: BEGIN starts after
   earlier writes are committed, so ROLLBACK undoes only what followed it. */
TEST(sqlite_explicit_transaction_starts_after_implicit_writes) {
    char path[128];
    implicit_db_path(path, sizeof(path));
    DBDriver driver;
    sqlite_driver_init(&driver);
    ASSERT_INT_EQ(1, driver.open(&driver, path));
    ASSERT_INT_EQ(1, driver.exec(&driver, "CREATE TABLE t (id INTEGER)", NULL, 0));
    ASSERT_INT_EQ(1, driver.exec(&driver, "INSERT INTO t VALUES (1)", NULL, 0));
    ASSERT_INT_EQ(1, driver.begin(&driver));
    ASSERT_INT_EQ(1, other_connection_count(path, "SELECT count(*) FROM t"));
    ASSERT_INT_EQ(1, driver.exec(&driver, "INSERT INTO t VALUES (2)", NULL, 0));
    ASSERT_INT_EQ(1, driver.rollback(&driver));
    driver.close(&driver);
    ASSERT_INT_EQ(1, other_connection_count(path, "SELECT count(*) FROM t"));
    unlink(path);
}

/* Without an explicit BEGIN, ROLLBACK still fails as it did under autocommit
   rather than silently discarding the implicit transaction's writes. */
TEST(sqlite_rollback_without_begin_keeps_implicit_writes) {
    char path[128];
    implicit_db_path(path, sizeof(path));
    DBDriver driver;
    sqlite_driver_init(&driver);
    ASSERT_INT_EQ(1, driver.open(&driver, path));
    ASSERT_INT_EQ(1, driver.exec(&driver, "CREATE TABLE t (id INTEGER)", NULL, 0));
    ASSERT_INT_EQ(1, driver.exec(&driver, "INSERT INTO t VALUES (1)", NULL, 0));
    ASSERT_INT_EQ(0, driver.rollback(&driver));
    driver.close(&driver);
    ASSERT_INT_EQ(1, other_connection_count(path, "SELECT count(*) FROM t"));
    unlink(path);
}

int main(void) {
    RUN_TEST(sqlite3_is_linked);
    RUN_TEST(sqlite_driver_crud);
    RUN_TEST(sqlite_driver_parameter_binding);
    RUN_TEST(sqlite_end_to_end_ddl_and_dml);
    RUN_TEST(sqlite_end_to_end_parameter_binding);
    RUN_TEST(sqlite_end_to_end_for_loop_parameter_binding);
    RUN_TEST(sqlite_end_to_end_for_loop_string_parameter_binding);
    RUN_TEST(sqlite_end_to_end_select_into_single_value);
    RUN_TEST(sqlite_end_to_end_select_into_multi_value);
    RUN_TEST(sqlite_end_to_end_select_into_array_row);
    RUN_TEST(sqlite_reports_source_line_on_sql_error);
    RUN_TEST(sqlite_implicit_transaction_held_until_close);
    RUN_TEST(sqlite_implicit_transaction_not_opened_by_reads);
    RUN_TEST(sqlite_implicit_transaction_flush_commits);
    RUN_TEST(sqlite_explicit_transaction_starts_after_implicit_writes);
    RUN_TEST(sqlite_rollback_without_begin_keeps_implicit_writes);
    TEST_SUMMARY();
}
