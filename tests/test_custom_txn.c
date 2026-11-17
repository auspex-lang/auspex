/* #113 phase 2: begin / commit / rollback and the implicit transaction on
   the custom engine. Mirrors the SQLite driver's implicit-transaction tests
   in test_sqlite.c; needs no SQLite. */
#include "test_harness.h"
#include "sql_engine.h"
#include "compiler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void db_path(char* buf, size_t size, const char* tag) {
    snprintf(buf, size, "/tmp/auspex_test_custom_txn_%s_%d.db", tag, (int)getpid());
    unlink(buf);
}

static void remove_db(const char* path) {
    char side[512];
    unlink(path);
    snprintf(side, sizeof(side), "%s.packages", path);
    unlink(side);
    snprintf(side, sizeof(side), "%s.programs", path);
    unlink(side);
}

/* The file as it is on disk right now: what a second process, or a crash,
   would see. */
static int snapshot_file(const char* path, const char* copy) {
    FILE* in = fopen(path, "rb");
    if (in == NULL) return 0;
    fseek(in, 0, SEEK_END);
    long size = ftell(in);
    fseek(in, 0, SEEK_SET);
    char* data = malloc(size > 0 ? (size_t)size : 1);
    int ok = data != NULL && fread(data, 1, (size_t)size, in) == (size_t)size;
    fclose(in);
    if (ok) {
        FILE* out = fopen(copy, "wb");
        ok = out != NULL && fwrite(data, 1, (size_t)size, out) == (size_t)size;
        if (out != NULL) fclose(out);
    }
    free(data);
    return ok;
}

static DBDriver open_driver(const char* path) {
    DBDriver driver;
    custom_driver_init(&driver);
    if (!driver.open(&driver, path)) {
        fprintf(stderr, "could not open %s\n", path);
    }
    return driver;
}

static int exec(DBDriver* driver, const char* sql) {
    return driver->exec(driver, sql, NULL, 0);
}

/* First column of the first row as an int, or -1 when the query fails or
   returns no rows. */
static int query_int(DBDriver* driver, const char* sql) {
    void* result = NULL;
    if (!driver->query(driver, sql, NULL, 0, &result)) return -1;
    void* row = NULL;
    int n = -1;
    if (driver->result_next(driver, result, &row)) {
        Value v;
        if (driver->row_get_column(driver, row, 0, &v) && v.type == VAL_INT) {
            n = (int)v.as.as_int;
        }
        value_release(v);
    }
    driver->result_free(driver, result);
    return n;
}

static int row_count(DBDriver* driver, const char* sql) {
    void* result = NULL;
    if (!driver->query(driver, sql, NULL, 0, &result)) return -1;
    void* row = NULL;
    int count = 0;
    while (driver->result_next(driver, result, &row)) count++;
    driver->result_free(driver, result);
    return count;
}

/* The driver's impl starts with its Context, which the catalog API takes.
   Selecting from a missing table yields no rows rather than an error on
   this engine, so absence is checked in the catalog itself. */
static Context* driver_ctx(DBDriver* driver) {
    return (Context*)driver->impl;
}

static int has_table(DBDriver* driver, const char* name) {
    return catalog_find_table(driver_ctx(driver), name) != NULL;
}

static int has_column(DBDriver* driver, const char* table, const char* column) {
    Table* t = catalog_find_table(driver_ctx(driver), table);
    if (t == NULL) return 0;
    for (int i = 0; i < t->column_count; i++) {
        if (strcmp(t->columns[i].name, column) == 0) return 1;
    }
    return 0;
}

static int has_view(DBDriver* driver, const char* name) {
    return catalog_view_query(driver_ctx(driver), name) != NULL;
}

static int table_in_file(const char* path, const char* name) {
    DBDriver driver = open_driver(path);
    if (driver.impl == NULL) return -2;
    int found = has_table(&driver, name);
    driver.close(&driver);
    return found;
}

/* Opens `path` in a fresh driver (the catalog is process-global, so only
   after the first one is closed) and counts the rows of `sql`. */
static int count_in_file(const char* path, const char* sql) {
    DBDriver driver = open_driver(path);
    if (driver.impl == NULL) return -2;
    int n = row_count(&driver, sql);
    driver.close(&driver);
    return n;
}

/* ---- implicit transaction (parity with test_sqlite.c) ---- */

TEST(custom_implicit_transaction_held_until_close) {
    char path[160], copy[200];
    db_path(path, sizeof(path), "held");
    snprintf(copy, sizeof(copy), "%s.copy", path);
    remove_db(copy);

    DBDriver driver = open_driver(path);
    ASSERT_PTR_NOT_NULL(driver.impl);
    ASSERT_INT_EQ(1, exec(&driver, "CREATE TABLE t (id INT)"));
    ASSERT_INT_EQ(1, exec(&driver, "INSERT INTO t VALUES (1)"));
    ASSERT_INT_EQ(CUSTOM_TXN_IMPLICIT, custom_driver_transaction_state(&driver));
    ASSERT_INT_EQ(1, snapshot_file(path, copy));
    driver.close(&driver);

    ASSERT_INT_EQ(0, table_in_file(copy, "t")); /* not on disk yet */
    ASSERT_INT_EQ(1, count_in_file(path, "SELECT id FROM t"));
    remove_db(copy);
    remove_db(path);
}

TEST(custom_implicit_transaction_not_opened_by_reads) {
    char path[160];
    db_path(path, sizeof(path), "reads");
    DBDriver driver = open_driver(path);
    ASSERT_INT_EQ(1, exec(&driver, "CREATE TABLE t (id INT)"));
    ASSERT_INT_EQ(1, driver.flush_implicit(&driver));
    ASSERT_INT_EQ(CUSTOM_TXN_NONE, custom_driver_transaction_state(&driver));
    ASSERT_INT_EQ(0, row_count(&driver, "SELECT id FROM t"));
    ASSERT_INT_EQ(CUSTOM_TXN_NONE, custom_driver_transaction_state(&driver));
    driver.close(&driver);
    remove_db(path);
}

TEST(custom_implicit_transaction_flush_commits) {
    char path[160], copy[200];
    db_path(path, sizeof(path), "flush");
    snprintf(copy, sizeof(copy), "%s.copy", path);
    remove_db(copy);

    DBDriver driver = open_driver(path);
    ASSERT_PTR_NOT_NULL(driver.flush_implicit);
    ASSERT_INT_EQ(1, driver.flush_implicit(&driver)); /* nothing open: no-op */
    ASSERT_INT_EQ(1, exec(&driver, "CREATE TABLE t (id INT)"));
    ASSERT_INT_EQ(1, exec(&driver, "INSERT INTO t VALUES (1)"));
    ASSERT_INT_EQ(1, driver.flush_implicit(&driver));
    ASSERT_INT_EQ(CUSTOM_TXN_NONE, custom_driver_transaction_state(&driver));
    ASSERT_INT_EQ(1, snapshot_file(path, copy));
    driver.close(&driver);

    ASSERT_INT_EQ(1, count_in_file(copy, "SELECT id FROM t"));
    remove_db(copy);
    remove_db(path);
}

/* BEGIN starts after earlier writes are committed, so ROLLBACK undoes only
   what followed it. */
TEST(custom_explicit_transaction_starts_after_implicit_writes) {
    char path[160], copy[200];
    db_path(path, sizeof(path), "explicit");
    snprintf(copy, sizeof(copy), "%s.copy", path);
    remove_db(copy);

    DBDriver driver = open_driver(path);
    ASSERT_INT_EQ(1, exec(&driver, "CREATE TABLE t (id INT)"));
    ASSERT_INT_EQ(1, exec(&driver, "INSERT INTO t VALUES (1)"));
    ASSERT_INT_EQ(1, driver.begin(&driver));
    ASSERT_INT_EQ(CUSTOM_TXN_EXPLICIT, custom_driver_transaction_state(&driver));
    ASSERT_INT_EQ(1, snapshot_file(path, copy));
    ASSERT_INT_EQ(1, exec(&driver, "INSERT INTO t VALUES (2)"));
    ASSERT_INT_EQ(2, row_count(&driver, "SELECT id FROM t"));
    ASSERT_INT_EQ(1, driver.rollback(&driver));
    ASSERT_INT_EQ(CUSTOM_TXN_NONE, custom_driver_transaction_state(&driver));
    ASSERT_INT_EQ(1, row_count(&driver, "SELECT id FROM t"));
    driver.close(&driver);

    ASSERT_INT_EQ(1, count_in_file(copy, "SELECT id FROM t"));
    ASSERT_INT_EQ(1, count_in_file(path, "SELECT id FROM t"));
    remove_db(copy);
    remove_db(path);
}

TEST(custom_rollback_without_begin_keeps_implicit_writes) {
    char path[160];
    db_path(path, sizeof(path), "nobegin");
    DBDriver driver = open_driver(path);
    ASSERT_INT_EQ(1, exec(&driver, "CREATE TABLE t (id INT)"));
    ASSERT_INT_EQ(1, exec(&driver, "INSERT INTO t VALUES (1)"));
    ASSERT_INT_EQ(0, driver.rollback(&driver));
    ASSERT_INT_EQ(1, strstr(driver.error_message, "no transaction is active") != NULL);
    ASSERT_INT_EQ(0, driver.commit(&driver));
    ASSERT_INT_EQ(1, strstr(driver.error_message, "no transaction is active") != NULL);
    driver.close(&driver);
    ASSERT_INT_EQ(1, count_in_file(path, "SELECT id FROM t"));
    remove_db(path);
}

TEST(custom_begin_inside_transaction_fails) {
    char path[160];
    db_path(path, sizeof(path), "nested");
    DBDriver driver = open_driver(path);
    ASSERT_INT_EQ(1, driver.begin(&driver));
    ASSERT_INT_EQ(0, driver.begin(&driver));
    ASSERT_INT_EQ(1, strstr(driver.error_message, "within a transaction") != NULL);
    ASSERT_INT_EQ(CUSTOM_TXN_EXPLICIT, custom_driver_transaction_state(&driver));
    ASSERT_INT_EQ(1, driver.commit(&driver));
    driver.close(&driver);
    remove_db(path);
}

TEST(custom_commit_persists_and_ends_transaction) {
    char path[160], copy[200];
    db_path(path, sizeof(path), "commit");
    snprintf(copy, sizeof(copy), "%s.copy", path);
    remove_db(copy);

    DBDriver driver = open_driver(path);
    ASSERT_INT_EQ(1, driver.begin(&driver));
    ASSERT_INT_EQ(1, exec(&driver, "CREATE TABLE t (id INT)"));
    ASSERT_INT_EQ(1, exec(&driver, "INSERT INTO t VALUES (1)"));
    ASSERT_INT_EQ(1, driver.commit(&driver));
    ASSERT_INT_EQ(CUSTOM_TXN_NONE, custom_driver_transaction_state(&driver));
    ASSERT_INT_EQ(1, snapshot_file(path, copy));
    driver.close(&driver);

    ASSERT_INT_EQ(1, count_in_file(copy, "SELECT id FROM t"));
    remove_db(copy);
    remove_db(path);
}

/* An explicit transaction left open at close is rolled back; what came
   before BEGIN was already committed. */
TEST(custom_close_rolls_back_open_explicit_transaction) {
    char path[160];
    db_path(path, sizeof(path), "close");
    DBDriver driver = open_driver(path);
    ASSERT_INT_EQ(1, exec(&driver, "CREATE TABLE t (id INT)"));
    ASSERT_INT_EQ(1, exec(&driver, "INSERT INTO t VALUES (1)"));
    ASSERT_INT_EQ(1, driver.begin(&driver));
    ASSERT_INT_EQ(1, exec(&driver, "INSERT INTO t VALUES (2)"));
    ASSERT_INT_EQ(1, exec(&driver, "CREATE TABLE u (id INT)"));
    driver.close(&driver);

    ASSERT_INT_EQ(1, count_in_file(path, "SELECT id FROM t"));
    ASSERT_INT_EQ(0, table_in_file(path, "u"));
    remove_db(path);
}

/* ---- rollback of each kind of change ---- */

TEST(custom_rollback_undoes_insert_update_delete) {
    char path[160];
    db_path(path, sizeof(path), "dml");
    DBDriver driver = open_driver(path);
    ASSERT_INT_EQ(1, exec(&driver, "CREATE TABLE t (id INT, v INT)"));
    ASSERT_INT_EQ(1, exec(&driver, "CREATE INDEX t_id ON t (id)"));
    for (int i = 1; i <= 50; i++) {
        char sql[96];
        snprintf(sql, sizeof(sql), "INSERT INTO t VALUES (%d, %d)", i, i * 10);
        ASSERT_INT_EQ(1, exec(&driver, sql));
    }

    ASSERT_INT_EQ(1, driver.begin(&driver));
    for (int i = 51; i <= 400; i++) { /* enough to split the index root */
        char sql[96];
        snprintf(sql, sizeof(sql), "INSERT INTO t VALUES (%d, %d)", i, i * 10);
        ASSERT_INT_EQ(1, exec(&driver, sql));
    }
    ASSERT_INT_EQ(1, exec(&driver, "UPDATE t SET v = 0 WHERE id = 7") >= 1);
    ASSERT_INT_EQ(1, exec(&driver, "DELETE FROM t WHERE id = 8") >= 1);
    ASSERT_INT_EQ(399, row_count(&driver, "SELECT id FROM t"));
    ASSERT_INT_EQ(1, driver.rollback(&driver));

    ASSERT_INT_EQ(50, row_count(&driver, "SELECT id FROM t"));
    ASSERT_INT_EQ(70, query_int(&driver, "SELECT v FROM t WHERE id = 7"));
    ASSERT_INT_EQ(80, query_int(&driver, "SELECT v FROM t WHERE id = 8"));
    ASSERT_INT_EQ(0, row_count(&driver, "SELECT id FROM t WHERE id = 300"));

    /* The table and its index keep working after the rollback. */
    ASSERT_INT_EQ(1, exec(&driver, "INSERT INTO t VALUES (300, 3000)"));
    ASSERT_INT_EQ(3000, query_int(&driver, "SELECT v FROM t WHERE id = 300"));
    driver.close(&driver);

    ASSERT_INT_EQ(51, count_in_file(path, "SELECT id FROM t"));
    remove_db(path);
}

TEST(custom_rollback_undoes_create_and_drop_table) {
    char path[160];
    db_path(path, sizeof(path), "ddl_table");
    DBDriver driver = open_driver(path);
    ASSERT_INT_EQ(1, exec(&driver, "CREATE TABLE keep (id INT)"));
    ASSERT_INT_EQ(1, exec(&driver, "INSERT INTO keep VALUES (1)"));

    ASSERT_INT_EQ(1, driver.begin(&driver));
    ASSERT_INT_EQ(1, exec(&driver, "CREATE TABLE fresh (id INT)"));
    ASSERT_INT_EQ(1, exec(&driver, "INSERT INTO fresh VALUES (1)"));
    ASSERT_INT_EQ(1, exec(&driver, "DROP TABLE keep"));
    ASSERT_INT_EQ(0, has_table(&driver, "keep"));
    ASSERT_INT_EQ(1, driver.rollback(&driver));

    ASSERT_INT_EQ(0, has_table(&driver, "fresh"));
    ASSERT_INT_EQ(1, row_count(&driver, "SELECT id FROM keep"));
    /* The name is free again. */
    ASSERT_INT_EQ(1, exec(&driver, "CREATE TABLE fresh (id INT)"));
    ASSERT_INT_EQ(0, row_count(&driver, "SELECT id FROM fresh"));
    driver.close(&driver);
    remove_db(path);
}

TEST(custom_rollback_undoes_create_index) {
    char path[160];
    db_path(path, sizeof(path), "ddl_index");
    DBDriver driver = open_driver(path);
    ASSERT_INT_EQ(1, exec(&driver, "CREATE TABLE t (id INT)"));
    ASSERT_INT_EQ(1, exec(&driver, "INSERT INTO t VALUES (1)"));

    ASSERT_INT_EQ(1, driver.begin(&driver));
    ASSERT_INT_EQ(1, exec(&driver, "CREATE INDEX t_id ON t (id)"));
    ASSERT_INT_EQ(1, driver.rollback(&driver));

    /* Creating it again only works if the rollback removed it. */
    ASSERT_INT_EQ(1, exec(&driver, "CREATE INDEX t_id ON t (id)"));
    ASSERT_INT_EQ(1, row_count(&driver, "SELECT id FROM t WHERE id = 1"));
    driver.close(&driver);
    remove_db(path);
}

TEST(custom_rollback_undoes_alter_table) {
    char path[160];
    db_path(path, sizeof(path), "ddl_alter");
    DBDriver driver = open_driver(path);
    ASSERT_INT_EQ(1, exec(&driver, "CREATE TABLE t (id INT, old INT)"));
    ASSERT_INT_EQ(1, exec(&driver, "INSERT INTO t VALUES (1, 5)"));

    ASSERT_INT_EQ(1, driver.begin(&driver));
    ASSERT_INT_EQ(1, exec(&driver, "ALTER TABLE t ADD COLUMN extra INT") >= 0);
    ASSERT_INT_EQ(1, exec(&driver, "ALTER TABLE t DROP COLUMN old") >= 0);
    ASSERT_INT_EQ(1, has_column(&driver, "t", "extra"));
    ASSERT_INT_EQ(0, has_column(&driver, "t", "old"));
    ASSERT_INT_EQ(1, driver.rollback(&driver));

    ASSERT_INT_EQ(0, has_column(&driver, "t", "extra"));
    ASSERT_INT_EQ(5, query_int(&driver, "SELECT old FROM t"));
    driver.close(&driver);
    remove_db(path);
}

TEST(custom_rollback_undoes_create_view) {
    char path[160];
    db_path(path, sizeof(path), "ddl_view");
    DBDriver driver = open_driver(path);
    ASSERT_INT_EQ(1, exec(&driver, "CREATE TABLE t (id INT)"));
    ASSERT_INT_EQ(1, exec(&driver, "INSERT INTO t VALUES (2)"));

    ASSERT_INT_EQ(1, driver.begin(&driver));
    ASSERT_INT_EQ(1, exec(&driver, "CREATE VIEW v AS SELECT id FROM t WHERE id > 1") >= 0);
    ASSERT_INT_EQ(1, row_count(&driver, "SELECT id FROM v"));
    ASSERT_INT_EQ(1, driver.rollback(&driver));

    ASSERT_INT_EQ(0, has_view(&driver, "v"));
    ASSERT_INT_EQ(1, has_table(&driver, "t"));
    driver.close(&driver);
    remove_db(path);
}

/* Decision 1 of the plan: sequences live in the catalog and roll back. */
TEST(custom_rollback_undoes_sequence_changes) {
    char path[160];
    db_path(path, sizeof(path), "seq");
    DBDriver driver = open_driver(path);
    ASSERT_INT_EQ(1, driver.begin(&driver));
    DBSequence seq;
    memset(&seq, 0, sizeof(seq));
    snprintf(seq.name, sizeof(seq.name), "s");
    seq.increment = 1;
    ASSERT_INT_EQ(1, driver.sequence_save(&driver, &seq));
    ASSERT_INT_EQ(1, driver.rollback(&driver));

    DBSequence loaded[4];
    int count = -1;
    ASSERT_INT_EQ(1, driver.sequence_load(&driver, loaded, 4, &count));
    ASSERT_INT_EQ(0, count);
    driver.close(&driver);
    remove_db(path);
}

/* A long script without BEGIN must not hold every page in memory: the
   implicit transaction is committed early once it grows past a limit. */
TEST(custom_large_implicit_transaction_is_committed_early) {
    char path[160];
    db_path(path, sizeof(path), "large");
    DBDriver driver = open_driver(path);
    ASSERT_INT_EQ(1, exec(&driver, "CREATE TABLE t (id INT, pad STRING)"));
    char pad[1001];
    memset(pad, 'x', 1000);
    pad[1000] = '\0';
    char* sql = malloc(1200);
    int flushed = 0;
    for (int i = 0; i < 4 * CUSTOM_IMPLICIT_MAX_DIRTY_PAGES && !flushed; i++) {
        snprintf(sql, 1200, "INSERT INTO t VALUES (%d, '%s')", i, pad);
        ASSERT_INT_EQ(1, exec(&driver, sql));
        if (custom_driver_transaction_state(&driver) == CUSTOM_TXN_NONE) flushed = 1;
    }
    free(sql);
    ASSERT_INT_EQ(1, flushed);
    driver.close(&driver);
    remove_db(path);
}

/* ---- programs on the CLI (the custom engine uses auspex.db in the cwd) ---- */

static int run_program(const char* dir, const char* source, char* out, size_t out_size) {
    char src[600], outp[600], cmd[2048];
    snprintf(src, sizeof(src), "%s/prog.apx", dir);
    snprintf(outp, sizeof(outp), "%s/out.txt", dir);
    FILE* f = fopen(src, "w");
    if (f == NULL) return -1;
    fputs(source, f);
    fclose(f);
    /* The suite runs from the repository root, like the others. */
    snprintf(cmd, sizeof(cmd),
             "root=$(pwd) && cd %s && \"$root/bin/auspex\" prog.apx > out.txt 2>&1", dir);
    int rc = system(cmd);
    out[0] = '\0';
    FILE* o = fopen(outp, "r");
    if (o != NULL) {
        size_t n = fread(out, 1, out_size - 1, o);
        out[n] = '\0';
        fclose(o);
    }
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

static void make_dir(char* buf, size_t size, const char* tag) {
    snprintf(buf, size, "/tmp/auspex_test_custom_txn_dir_%s_%d", tag, (int)getpid());
    char cmd[700];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", buf, buf);
    test_shell(cmd);
}

static void remove_dir(const char* dir) {
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    test_shell(cmd);
}

TEST(custom_cli_begin_rollback_commit) {
    char dir[300], out[2048];
    make_dir(dir, sizeof(dir), "cli");
    ASSERT_INT_EQ(1, dir[0] != '\0');
    int rc = run_program(dir,
        "proc main() -> int {\n"
        "    create table t (id int);\n"
        "    insert into t values (1);\n"
        "    begin;\n"
        "    insert into t values (2);\n"
        "    rollback;\n"
        "    begin;\n"
        "    insert into t values (3);\n"
        "    commit;\n"
        "    int cnt;\n"
        "    select count(*) into cnt from t;\n"
        "    print int_to_string(cnt);\n"
        "    int s;\n"
        "    select sum(id) into s from t;\n"
        "    print int_to_string(s);\n"
        "    return 0;\n"
        "}\n", out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "2\n4\n") != NULL);
    remove_dir(dir);
}

TEST(custom_cli_rollback_undoes_row_trigger_writes) {
    char dir[300], out[2048];
    make_dir(dir, sizeof(dir), "trigger");
    ASSERT_INT_EQ(1, dir[0] != '\0');
    int rc = run_program(dir,
        "trigger t_audit after insert on t for each row {\n"
        "    execute_immediate(format(\"insert into audit values (%s)\",\n"
        "        [int_to_string(:new.id)]));\n"
        "}\n"
        "proc main() -> int {\n"
        "    create table t (id int);\n"
        "    create table audit (id int);\n"
        "    begin;\n"
        "    insert into t values (1);\n"
        "    rollback;\n"
        "    int a;\n"
        "    select count(*) into a from audit;\n"
        "    print int_to_string(a);\n"
        "    insert into t values (2);\n"
        "    select count(*) into a from audit;\n"
        "    print int_to_string(a);\n"
        "    return 0;\n"
        "}\n", out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "0\n1\n") != NULL);
    remove_dir(dir);
}

TEST(custom_cli_commit_without_begin_is_an_error) {
    char dir[300], out[2048];
    make_dir(dir, sizeof(dir), "nobegin");
    ASSERT_INT_EQ(1, dir[0] != '\0');
    int rc = run_program(dir,
        "proc main() -> int {\n"
        "    commit;\n"
        "    return 0;\n"
        "}\n", out, sizeof(out));
    ASSERT_INT_EQ(1, rc != 0);
    ASSERT_INT_EQ(1, strstr(out, "no transaction is active") != NULL);
    remove_dir(dir);
}

int main(void) {
    RUN_TEST(custom_implicit_transaction_held_until_close);
    RUN_TEST(custom_implicit_transaction_not_opened_by_reads);
    RUN_TEST(custom_implicit_transaction_flush_commits);
    RUN_TEST(custom_explicit_transaction_starts_after_implicit_writes);
    RUN_TEST(custom_rollback_without_begin_keeps_implicit_writes);
    RUN_TEST(custom_begin_inside_transaction_fails);
    RUN_TEST(custom_commit_persists_and_ends_transaction);
    RUN_TEST(custom_close_rolls_back_open_explicit_transaction);
    RUN_TEST(custom_rollback_undoes_insert_update_delete);
    RUN_TEST(custom_rollback_undoes_create_and_drop_table);
    RUN_TEST(custom_rollback_undoes_create_index);
    RUN_TEST(custom_rollback_undoes_alter_table);
    RUN_TEST(custom_rollback_undoes_create_view);
    RUN_TEST(custom_rollback_undoes_sequence_changes);
    RUN_TEST(custom_large_implicit_transaction_is_committed_early);
    RUN_TEST(custom_cli_begin_rollback_commit);
    RUN_TEST(custom_cli_rollback_undoes_row_trigger_writes);
    RUN_TEST(custom_cli_commit_without_begin_is_an_error);
    TEST_SUMMARY();
}
