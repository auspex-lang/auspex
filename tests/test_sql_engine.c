#include "test_harness.h"
#include "sql_engine.h"
#include "compiler.h"
#include "vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char* make_temp_path(void) {
    char* path = malloc(256);
    if (path == NULL) return NULL;
    snprintf(path, 256, "/tmp/mydb_test_sql_%d_%d.db", (int)getpid(), (int)(size_t)path);
    if (strlen(path) > 240) path[240] = '\0';
    unlink(path);
    return path;
}

static void cleanup(char* path) {
    if (path == NULL) return;
    unlink(path);
    free(path);
}

TEST(sql_create_table_persists_schema) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE users (id INT, name STRING)", &ctx));
    catalog_close(&ctx);

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    Result* res = sql_exec("SELECT id, name FROM users", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(0, res->row_count);
    result_free(res);
    catalog_close(&ctx);

    cleanup(path);
}

TEST(sql_insert_and_select_persists_rows) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE users (id INT, name STRING)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO users VALUES (1, 'alice')", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO users VALUES (2, 'bob')", &ctx));
    catalog_close(&ctx);

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    Result* res = sql_exec("SELECT * FROM users", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(2, res->row_count);

    Row* row = result_next(res);
    ASSERT_PTR_NOT_NULL(row);
    ASSERT_INT_EQ(2, row->field_count);
    ASSERT_INT_EQ(1, row_get_field(row, "id").as.as_int);
    ASSERT_STRING_EQ("alice", row_get_field(row, "name").as.as_string);

    row = result_next(res);
    ASSERT_PTR_NOT_NULL(row);
    ASSERT_INT_EQ(2, row->field_count);
    ASSERT_INT_EQ(2, row_get_field(row, "id").as.as_int);
    ASSERT_STRING_EQ("bob", row_get_field(row, "name").as.as_string);

    result_free(res);
    catalog_close(&ctx);
    cleanup(path);
}

TEST(sql_select_specific_columns) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE people (id INT, name STRING, age INT)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO people VALUES (1, 'alice', 30)", &ctx));

    Result* res = sql_exec("SELECT name, age FROM people", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(1, res->row_count);

    Row* row = result_next(res);
    ASSERT_PTR_NOT_NULL(row);
    ASSERT_INT_EQ(2, row->field_count);
    ASSERT_STRING_EQ("alice", row_get_field(row, "name").as.as_string);
    ASSERT_INT_EQ(30, row_get_field(row, "age").as.as_int);

    result_free(res);
    catalog_close(&ctx);
    cleanup(path);
}

TEST(sql_where_equals_int) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE users (id INT, name STRING)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO users VALUES (1, 'alice')", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO users VALUES (2, 'bob')", &ctx));

    Result* res = sql_exec("SELECT name FROM users WHERE id = 2", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(1, res->row_count);

    Row* row = result_next(res);
    ASSERT_STRING_EQ("bob", row_get_field(row, "name").as.as_string);

    result_free(res);
    catalog_close(&ctx);
    cleanup(path);
}

TEST(sql_where_greater_than_int) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE people (id INT, age INT)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO people VALUES (1, 20)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO people VALUES (2, 35)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO people VALUES (3, 40)", &ctx));

    Result* res = sql_exec("SELECT id FROM people WHERE age > 25", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(2, res->row_count);

    Row* row = result_next(res);
    ASSERT_INT_EQ(2, row_get_field(row, "id").as.as_int);
    row = result_next(res);
    ASSERT_INT_EQ(3, row_get_field(row, "id").as.as_int);

    result_free(res);
    catalog_close(&ctx);
    cleanup(path);
}

TEST(sql_select_unknown_table_returns_empty_result) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    Result* res = sql_exec("SELECT id FROM missing", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(0, res->row_count);
    result_free(res);
    catalog_close(&ctx);
    cleanup(path);
}

TEST(sql_compiler_loop_uses_persisted_table) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE users (id INT)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO users VALUES (7)", &ctx));
    catalog_close(&ctx);

    Chunk chunk;
    init_chunk(&chunk);
    ASSERT_INT_EQ(1, compile("proc main() -> int { for row in SELECT id FROM users { return row.id; } return 0; }", &chunk, NULL, 0));

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    Result* check = sql_exec("SELECT id FROM users", &ctx);
    ASSERT_PTR_NOT_NULL(check);
    ASSERT_INT_EQ(1, check->row_count);
    result_free(check);

    VM* vm = vm_init();
    vm_set_context(vm, &ctx);
    ASSERT_INT_EQ(INTERPRET_OK, vm_interpret(vm, &chunk));
    ASSERT_INT_EQ(7, vm_pop(vm).as.as_int);
    vm_free(vm);
    free_chunk(&chunk);
    catalog_close(&ctx);
    cleanup(path);
}

TEST(custom_driver_runs_create_and_insert) {
    char* path = make_temp_path();
    DBDriver driver;
    custom_driver_init(&driver);
    int rc = driver.open(&driver, path);
    ASSERT_INT_EQ(1, rc);
    rc = driver.exec(&driver, "CREATE TABLE users (id INT, name STRING)", NULL, 0);
    ASSERT_INT_EQ(1, rc);
    rc = driver.exec(&driver, "INSERT INTO users VALUES (1, 'alice')", NULL, 0);
    ASSERT_INT_EQ(1, rc);

    void* result = NULL;
    rc = driver.query(&driver, "SELECT id, name FROM users", NULL, 0, &result);
    ASSERT_INT_EQ(1, rc);

    void* row = NULL;
    rc = driver.result_next(&driver, result, &row);
    ASSERT_INT_EQ(1, rc);

    Value id;
    rc = driver.row_get_field(&driver, row, "id", &id);
    ASSERT_INT_EQ(1, rc);
    ASSERT_INT_EQ(VAL_INT, id.type);
    ASSERT_INT_EQ(1, id.as.as_int);

    driver.result_free(&driver, result);
    driver.close(&driver);
    cleanup(path);
}

TEST(sql_update_modifies_rows) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE users (id INT, age INT)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO users VALUES (1, 20)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO users VALUES (2, 30)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("UPDATE users SET age = 21 WHERE id = 1", &ctx));

    Result* res = sql_exec("SELECT age FROM users WHERE id = 1", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(1, res->row_count);
    Row* row = result_next(res);
    ASSERT_INT_EQ(21, row_get_field(row, "age").as.as_int);
    result_free(res);

    catalog_close(&ctx);
    cleanup(path);
}

TEST(sql_update_with_string) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE users (id INT, name STRING)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO users VALUES (1, 'alice')", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("UPDATE users SET name = 'alicia' WHERE id = 1", &ctx));

    Result* res = sql_exec("SELECT name FROM users WHERE id = 1", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(1, res->row_count);
    Row* row = result_next(res);
    ASSERT_STRING_EQ("alicia", row_get_field(row, "name").as.as_string);
    result_free(res);

    catalog_close(&ctx);
    cleanup(path);
}

TEST(sql_delete_removes_rows) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE users (id INT, name STRING)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO users VALUES (1, 'alice')", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO users VALUES (2, 'bob')", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("DELETE FROM users WHERE id = 1", &ctx));

    Result* res = sql_exec("SELECT id FROM users", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(1, res->row_count);
    Row* row = result_next(res);
    ASSERT_INT_EQ(2, row_get_field(row, "id").as.as_int);
    result_free(res);

    catalog_close(&ctx);
    cleanup(path);
}

TEST(sql_update_and_delete_persist) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE users (id INT, name STRING, age INT)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO users VALUES (1, 'alice', 30)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO users VALUES (2, 'bob', 25)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("UPDATE users SET age = 31 WHERE id = 1", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("DELETE FROM users WHERE id = 2", &ctx));
    catalog_close(&ctx);

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    Result* res = sql_exec("SELECT id, name, age FROM users", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(1, res->row_count);
    Row* row = result_next(res);
    ASSERT_INT_EQ(1, row_get_field(row, "id").as.as_int);
    ASSERT_STRING_EQ("alice", row_get_field(row, "name").as.as_string);
    ASSERT_INT_EQ(31, row_get_field(row, "age").as.as_int);
    result_free(res);
    catalog_close(&ctx);

    cleanup(path);
}

TEST(sql_order_by_asc) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE users (id INT, name STRING)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO users VALUES (2, 'bob')", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO users VALUES (1, 'alice')", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO users VALUES (3, 'carol')", &ctx));

    Result* res = sql_exec("SELECT id, name FROM users ORDER BY id ASC", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(3, res->row_count);
    ASSERT_INT_EQ(1, row_get_field(&res->rows[0], "id").as.as_int);
    ASSERT_INT_EQ(2, row_get_field(&res->rows[1], "id").as.as_int);
    ASSERT_INT_EQ(3, row_get_field(&res->rows[2], "id").as.as_int);
    result_free(res);

    catalog_close(&ctx);
    cleanup(path);
}

TEST(sql_order_by_desc) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE users (id INT, name STRING)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO users VALUES (1, 'alice')", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO users VALUES (3, 'carol')", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO users VALUES (2, 'bob')", &ctx));

    Result* res = sql_exec("SELECT id FROM users ORDER BY id DESC", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(3, res->row_count);
    ASSERT_INT_EQ(3, row_get_field(&res->rows[0], "id").as.as_int);
    ASSERT_INT_EQ(2, row_get_field(&res->rows[1], "id").as.as_int);
    ASSERT_INT_EQ(1, row_get_field(&res->rows[2], "id").as.as_int);
    result_free(res);

    catalog_close(&ctx);
    cleanup(path);
}

TEST(sql_limit_clause) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE users (id INT, name STRING)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO users VALUES (1, 'alice')", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO users VALUES (2, 'bob')", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO users VALUES (3, 'carol')", &ctx));

    Result* res = sql_exec("SELECT id FROM users ORDER BY id DESC LIMIT 2", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(2, res->row_count);
    ASSERT_INT_EQ(3, row_get_field(&res->rows[0], "id").as.as_int);
    ASSERT_INT_EQ(2, row_get_field(&res->rows[1], "id").as.as_int);
    result_free(res);

    catalog_close(&ctx);
    cleanup(path);
}

TEST(sql_aggregate_count) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE users (id INT, name STRING)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO users VALUES (1, 'alice')", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO users VALUES (2, 'bob')", &ctx));

    Result* res = sql_exec("SELECT COUNT(*) FROM users", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(1, res->row_count);
    ASSERT_INT_EQ(2, row_get_field(&res->rows[0], "count").as.as_int);
    result_free(res);

    catalog_close(&ctx);
    cleanup(path);
}

TEST(sql_aggregate_sum_avg_min_max) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE scores (id INT, score INT)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO scores VALUES (1, 10)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO scores VALUES (2, 20)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO scores VALUES (3, 30)", &ctx));

    Result* res = sql_exec("SELECT SUM(score), AVG(score), MIN(score), MAX(score) FROM scores", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(1, res->row_count);
    ASSERT_INT_EQ(VAL_INT, row_get_field(&res->rows[0], "sum").type);
    ASSERT_INT_EQ(60, row_get_field(&res->rows[0], "sum").as.as_int);
    ASSERT_FLOAT_EQ(20.0, row_get_field(&res->rows[0], "avg").as.as_float);
    ASSERT_INT_EQ(10, row_get_field(&res->rows[0], "min").as.as_int);
    ASSERT_INT_EQ(30, row_get_field(&res->rows[0], "max").as.as_int);
    result_free(res);

    catalog_close(&ctx);
    cleanup(path);
}

TEST(sql_aggregate_with_where) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE scores (id INT, score INT)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO scores VALUES (1, 10)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO scores VALUES (2, 20)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO scores VALUES (3, 30)", &ctx));

    Result* res = sql_exec("SELECT COUNT(*), SUM(score) FROM scores WHERE score > 10", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(1, res->row_count);
    ASSERT_INT_EQ(2, row_get_field(&res->rows[0], "count").as.as_int);
    ASSERT_INT_EQ(50, row_get_field(&res->rows[0], "sum").as.as_int);
    result_free(res);

    catalog_close(&ctx);
    cleanup(path);
}

TEST(sql_join_basic) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE users (id INT, name STRING)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE orders (id INT, user_id INT, total INT)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO users VALUES (1, 'alice')", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO users VALUES (2, 'bob')", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO orders VALUES (101, 1, 50)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO orders VALUES (102, 1, 75)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO orders VALUES (103, 2, 30)", &ctx));

    Result* res = sql_exec("SELECT name, total FROM users JOIN orders ON users.id = orders.user_id ORDER BY total DESC", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(3, res->row_count);
    ASSERT_STRING_EQ("alice", row_get_field(&res->rows[0], "name").as.as_string);
    ASSERT_INT_EQ(75, row_get_field(&res->rows[0], "total").as.as_int);
    ASSERT_STRING_EQ("alice", row_get_field(&res->rows[1], "name").as.as_string);
    ASSERT_INT_EQ(50, row_get_field(&res->rows[1], "total").as.as_int);
    ASSERT_STRING_EQ("bob", row_get_field(&res->rows[2], "name").as.as_string);
    ASSERT_INT_EQ(30, row_get_field(&res->rows[2], "total").as.as_int);
    result_free(res);

    catalog_close(&ctx);
    cleanup(path);
}

TEST(sql_join_select_star) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE a (a_id INT, a_name STRING)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE b (b_id INT, a_id INT, b_val INT)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO a VALUES (1, 'one')", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO a VALUES (2, 'two')", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO b VALUES (10, 1, 100)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO b VALUES (20, 2, 200)", &ctx));

    Result* res = sql_exec("SELECT * FROM a JOIN b ON a.a_id = b.a_id", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(2, res->row_count);
    ASSERT_INT_EQ(5, res->rows[0].field_count);
    ASSERT_INT_EQ(1, row_get_field(&res->rows[0], "a_id").as.as_int);
    ASSERT_STRING_EQ("one", row_get_field(&res->rows[0], "a_name").as.as_string);
    ASSERT_INT_EQ(100, row_get_field(&res->rows[0], "b_val").as.as_int);
    result_free(res);

    catalog_close(&ctx);
    cleanup(path);
}

TEST(sql_join_with_where_limit) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE users (id INT, name STRING)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE items (id INT, user_id INT, price INT)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO users VALUES (1, 'alice')", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO users VALUES (2, 'bob')", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO items VALUES (1, 1, 10)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO items VALUES (2, 1, 20)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO items VALUES (3, 2, 5)", &ctx));

    Result* res = sql_exec("SELECT id, price FROM users JOIN items ON users.id = items.user_id WHERE name = 'alice' LIMIT 1", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(1, res->row_count);
    ASSERT_INT_EQ(1, row_get_field(&res->rows[0], "id").as.as_int);
    ASSERT_INT_EQ(10, row_get_field(&res->rows[0], "price").as.as_int);
    result_free(res);

    catalog_close(&ctx);
    cleanup(path);
}

TEST(sql_join_no_matches) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE users (id INT, name STRING)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE orders (id INT, user_id INT, total INT)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO users VALUES (1, 'alice')", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO orders VALUES (101, 2, 50)", &ctx));

    Result* res = sql_exec("SELECT * FROM users JOIN orders ON users.id = orders.user_id", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(0, res->row_count);
    result_free(res);

    catalog_close(&ctx);
    cleanup(path);
}

TEST(sql_qualified_column_select) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE people (id INT, name STRING, age INT)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO people VALUES (1, 'alice', 30)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO people VALUES (2, 'bob', 25)", &ctx));

    Result* res = sql_exec("SELECT people.name, people.age FROM people", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(2, res->row_count);
    ASSERT_STRING_EQ("alice", row_get_field(&res->rows[0], "name").as.as_string);
    ASSERT_INT_EQ(30, row_get_field(&res->rows[0], "age").as.as_int);
    ASSERT_STRING_EQ("bob", row_get_field(&res->rows[1], "name").as.as_string);
    ASSERT_INT_EQ(25, row_get_field(&res->rows[1], "age").as.as_int);
    result_free(res);

    catalog_close(&ctx);
    cleanup(path);
}

TEST(sql_qualified_column_where) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE users (id INT, name STRING)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO users VALUES (1, 'alice')", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO users VALUES (2, 'bob')", &ctx));

    Result* res = sql_exec("SELECT users.name FROM users WHERE users.id = 2", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(1, res->row_count);
    ASSERT_STRING_EQ("bob", row_get_field(&res->rows[0], "name").as.as_string);
    result_free(res);

    catalog_close(&ctx);
    cleanup(path);
}

TEST(sql_qualified_column_order_by) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE users (id INT, name STRING)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO users VALUES (2, 'bob')", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO users VALUES (1, 'alice')", &ctx));

    Result* res = sql_exec("SELECT users.name FROM users ORDER BY users.id ASC", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(2, res->row_count);
    ASSERT_STRING_EQ("alice", row_get_field(&res->rows[0], "name").as.as_string);
    ASSERT_STRING_EQ("bob", row_get_field(&res->rows[1], "name").as.as_string);
    result_free(res);

    catalog_close(&ctx);
    cleanup(path);
}

TEST(sql_qualified_column_in_join) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE users (id INT, name STRING)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE orders (id INT, user_id INT, total INT)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO users VALUES (1, 'alice')", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO users VALUES (2, 'bob')", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO orders VALUES (101, 1, 50)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO orders VALUES (102, 1, 75)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO orders VALUES (103, 2, 30)", &ctx));

    Result* res = sql_exec(
        "SELECT users.name, orders.total FROM users JOIN orders ON users.id = orders.user_id "
        "WHERE users.name = 'alice' ORDER BY orders.total DESC", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(2, res->row_count);
    ASSERT_STRING_EQ("alice", row_get_field(&res->rows[0], "name").as.as_string);
    ASSERT_INT_EQ(75, row_get_field(&res->rows[0], "total").as.as_int);
    ASSERT_STRING_EQ("alice", row_get_field(&res->rows[1], "name").as.as_string);
    ASSERT_INT_EQ(50, row_get_field(&res->rows[1], "total").as.as_int);
    result_free(res);

    catalog_close(&ctx);
    cleanup(path);
}

TEST(sql_left_join_includes_unmatched_left) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE users (id INT, name STRING)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE orders (id INT, user_id INT, total INT)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO users VALUES (1, 'alice')", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO users VALUES (2, 'bob')", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO orders VALUES (101, 2, 30)", &ctx));

    Result* res = sql_exec(
        "SELECT users.name, orders.total FROM users LEFT JOIN orders ON users.id = orders.user_id "
        "ORDER BY users.id ASC", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(2, res->row_count);
    ASSERT_STRING_EQ("alice", row_get_field(&res->rows[0], "name").as.as_string);
    ASSERT_INT_EQ(0, row_get_field(&res->rows[0], "total").as.as_int);
    ASSERT_STRING_EQ("bob", row_get_field(&res->rows[1], "name").as.as_string);
    ASSERT_INT_EQ(30, row_get_field(&res->rows[1], "total").as.as_int);
    result_free(res);

    catalog_close(&ctx);
    cleanup(path);
}

TEST(sql_left_join_no_matches) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE users (id INT, name STRING)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE orders (id INT, user_id INT, total INT)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO users VALUES (1, 'alice')", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO orders VALUES (101, 2, 50)", &ctx));

    Result* res = sql_exec(
        "SELECT users.name, orders.total FROM users LEFT JOIN orders ON users.id = orders.user_id", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(1, res->row_count);
    ASSERT_STRING_EQ("alice", row_get_field(&res->rows[0], "name").as.as_string);
    ASSERT_INT_EQ(0, row_get_field(&res->rows[0], "total").as.as_int);
    result_free(res);

    catalog_close(&ctx);
    cleanup(path);
}

TEST(sql_left_join_with_where) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE users (id INT, name STRING)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE orders (id INT, user_id INT, total INT)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO users VALUES (1, 'alice')", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO users VALUES (2, 'bob')", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO orders VALUES (101, 2, 30)", &ctx));

    Result* res = sql_exec(
        "SELECT users.name, orders.total FROM users LEFT JOIN orders ON users.id = orders.user_id "
        "WHERE users.id = 1", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(1, res->row_count);
    ASSERT_STRING_EQ("alice", row_get_field(&res->rows[0], "name").as.as_string);
    ASSERT_INT_EQ(0, row_get_field(&res->rows[0], "total").as.as_int);
    result_free(res);

    catalog_close(&ctx);
    cleanup(path);
}

TEST(sql_group_by_count_sum) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE scores (category INT, score INT)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO scores VALUES (1, 10)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO scores VALUES (1, 20)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO scores VALUES (2, 30)", &ctx));

    Result* res = sql_exec(
        "SELECT category, COUNT(*), SUM(score) FROM scores GROUP BY category ORDER BY category ASC", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(2, res->row_count);
    ASSERT_INT_EQ(1, row_get_field(&res->rows[0], "category").as.as_int);
    ASSERT_INT_EQ(2, row_get_field(&res->rows[0], "count").as.as_int);
    ASSERT_INT_EQ(30, row_get_field(&res->rows[0], "sum").as.as_int);
    ASSERT_INT_EQ(2, row_get_field(&res->rows[1], "category").as.as_int);
    ASSERT_INT_EQ(1, row_get_field(&res->rows[1], "count").as.as_int);
    ASSERT_INT_EQ(30, row_get_field(&res->rows[1], "sum").as.as_int);
    result_free(res);

    catalog_close(&ctx);
    cleanup(path);
}

TEST(sql_group_by_avg_min_max) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE scores (category INT, score INT)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO scores VALUES (1, 10)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO scores VALUES (1, 30)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO scores VALUES (2, 20)", &ctx));

    Result* res = sql_exec(
        "SELECT category, AVG(score), MIN(score), MAX(score) FROM scores GROUP BY category ORDER BY category ASC", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(2, res->row_count);
    ASSERT_INT_EQ(1, row_get_field(&res->rows[0], "category").as.as_int);
    ASSERT_FLOAT_EQ(20.0, row_get_field(&res->rows[0], "avg").as.as_float);
    ASSERT_INT_EQ(10, row_get_field(&res->rows[0], "min").as.as_int);
    ASSERT_INT_EQ(30, row_get_field(&res->rows[0], "max").as.as_int);
    ASSERT_INT_EQ(2, row_get_field(&res->rows[1], "category").as.as_int);
    ASSERT_FLOAT_EQ(20.0, row_get_field(&res->rows[1], "avg").as.as_float);
    ASSERT_INT_EQ(20, row_get_field(&res->rows[1], "min").as.as_int);
    ASSERT_INT_EQ(20, row_get_field(&res->rows[1], "max").as.as_int);
    result_free(res);

    catalog_close(&ctx);
    cleanup(path);
}

TEST(sql_group_by_with_where) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE scores (category INT, score INT)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO scores VALUES (1, 10)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO scores VALUES (1, 20)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO scores VALUES (2, 30)", &ctx));

    Result* res = sql_exec(
        "SELECT category, COUNT(*), SUM(score) FROM scores WHERE score > 10 GROUP BY category", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(2, res->row_count);
    result_free(res);

    catalog_close(&ctx);
    cleanup(path);
}

TEST(sql_insert_into_select_star) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE source (id INT, name STRING)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE target (id INT, name STRING)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO source VALUES (1, 'alice')", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO source VALUES (2, 'bob')", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO target SELECT * FROM source", &ctx));

    Result* res = sql_exec("SELECT id, name FROM target ORDER BY id ASC", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(2, res->row_count);
    ASSERT_INT_EQ(1, row_get_field(&res->rows[0], "id").as.as_int);
    ASSERT_STRING_EQ("alice", row_get_field(&res->rows[0], "name").as.as_string);
    ASSERT_INT_EQ(2, row_get_field(&res->rows[1], "id").as.as_int);
    ASSERT_STRING_EQ("bob", row_get_field(&res->rows[1], "name").as.as_string);
    result_free(res);

    catalog_close(&ctx);
    cleanup(path);
}

TEST(sql_insert_into_select_specific_columns) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE source (id INT, name STRING, age INT)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE target (id INT, name STRING)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO source VALUES (1, 'alice', 30)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO source VALUES (2, 'bob', 25)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO target SELECT id, name FROM source", &ctx));

    Result* res = sql_exec("SELECT id, name FROM target ORDER BY id ASC", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(2, res->row_count);
    ASSERT_INT_EQ(1, row_get_field(&res->rows[0], "id").as.as_int);
    ASSERT_STRING_EQ("alice", row_get_field(&res->rows[0], "name").as.as_string);
    ASSERT_INT_EQ(2, row_get_field(&res->rows[1], "id").as.as_int);
    ASSERT_STRING_EQ("bob", row_get_field(&res->rows[1], "name").as.as_string);
    result_free(res);

    catalog_close(&ctx);
    cleanup(path);
}

TEST(sql_insert_into_select_with_where) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE source (id INT, score INT)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE target (id INT, score INT)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO source VALUES (1, 10)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO source VALUES (2, 20)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO source VALUES (3, 30)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO target SELECT id, score FROM source WHERE score > 10", &ctx));

    Result* res = sql_exec("SELECT id, score FROM target ORDER BY id ASC", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(2, res->row_count);
    ASSERT_INT_EQ(2, row_get_field(&res->rows[0], "id").as.as_int);
    ASSERT_INT_EQ(20, row_get_field(&res->rows[0], "score").as.as_int);
    ASSERT_INT_EQ(3, row_get_field(&res->rows[1], "id").as.as_int);
    ASSERT_INT_EQ(30, row_get_field(&res->rows[1], "score").as.as_int);
    result_free(res);

    catalog_close(&ctx);
    cleanup(path);
}
/* Row-page chain (issue #50): a page's next_page link shares header bytes
 * with the old record-count field, so a full page that has a successor used
 * to read back as empty. These tables are sized to span many pages: each row
 * is ~112 bytes, ~36 rows per 4 KB page. */
#define BIG_ROWS 300

static int insert_big_rows(Context* ctx, int first_id, int last_id) {
    char pad[101];
    memset(pad, 'x', 100);
    pad[100] = '\0';
    for (int id = first_id; id <= last_id; id++) {
        char sql[256];
        snprintf(sql, sizeof(sql), "INSERT INTO big VALUES (%d, '%s')", id, pad);
        if (!sql_exec_ddl(sql, ctx)) return 0;
    }
    return 1;
}

static int select_count(Context* ctx, const char* sql) {
    Result* res = sql_exec(sql, ctx);
    if (res == NULL) return -1;
    int n = res->row_count;
    result_free(res);
    return n;
}

static int select_single_int(Context* ctx, const char* sql, const char* field) {
    Result* res = sql_exec(sql, ctx);
    if (res == NULL || res->row_count != 1) {
        if (res != NULL) result_free(res);
        return -1;
    }
    int v = row_get_field(&res->rows[0], field).as.as_int;
    result_free(res);
    return v;
}

static double select_single_float(Context* ctx, const char* sql, const char* field) {
    Result* res = sql_exec(sql, ctx);
    if (res == NULL || res->row_count != 1) {
        if (res != NULL) result_free(res);
        return -1.0;
    }
    double v = row_get_field(&res->rows[0], field).as.as_float;
    result_free(res);
    return v;
}

TEST(sql_insert_multiple_value_rows) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE t (id INT, name STRING)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO t VALUES (1, 'a'), (2, 'b, c'),(3,'d')", &ctx));
    ASSERT_INT_EQ(3, select_count(&ctx, "SELECT * FROM t"));
    Result* res = sql_exec("SELECT name FROM t WHERE id = 2", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(1, res->row_count);
    ASSERT_STRING_EQ("b, c", row_get_field(&res->rows[0], "name").as.as_string);
    result_free(res);

    /* Malformed lists are rejected and insert nothing. */
    ASSERT_INT_EQ(0, sql_exec_ddl("INSERT INTO t VALUES (4, 'e'), (5)", &ctx));
    ASSERT_INT_EQ(0, sql_exec_ddl("INSERT INTO t VALUES (4, 'e'), (5, 'f', 6)", &ctx));
    ASSERT_INT_EQ(0, sql_exec_ddl("INSERT INTO t VALUES (4, 'e'),", &ctx));
    ASSERT_INT_EQ(0, sql_exec_ddl("INSERT INTO t VALUES (4, 'e') (5, 'f')", &ctx));
    ASSERT_INT_EQ(3, select_count(&ctx, "SELECT * FROM t"));

    catalog_close(&ctx);
    cleanup(path);
}

TEST(sql_insert_multiple_rows_checks_all_before_inserting) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE t (id INT NOT NULL)", &ctx));
    /* NOT NULL fails in the last row: none of the rows go in. */
    ASSERT_INT_EQ(0, sql_exec_ddl("INSERT INTO t VALUES (1), (2), (NULL)", &ctx));
    ASSERT_INT_EQ(0, select_count(&ctx, "SELECT * FROM t"));
    /* So does a value the column type rejects. */
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE d (day DATE)", &ctx));
    ASSERT_INT_EQ(0, sql_exec_ddl("INSERT INTO d VALUES ('2026-01-02'), ('soon')", &ctx));
    ASSERT_INT_EQ(0, select_count(&ctx, "SELECT * FROM d"));

    catalog_close(&ctx);
    cleanup(path);
}

/* SUM is an int over ints (as on SQLite), a float once any input is a
   float, and a float when the int sum would overflow. */
TEST(sql_sum_result_type_follows_inputs) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE n (i INT, f FLOAT)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO n VALUES (2000000000, 1.5)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO n VALUES (2000000000, 2)", &ctx));

    Result* res = sql_exec("SELECT SUM(f) FROM n", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(VAL_FLOAT, row_get_field(&res->rows[0], "sum").type);
    result_free(res);
    ASSERT_FLOAT_EQ(3.5, select_single_float(&ctx, "SELECT SUM(f) FROM n", "sum"));

    res = sql_exec("SELECT SUM(i) FROM n", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(VAL_FLOAT, row_get_field(&res->rows[0], "sum").type);
    result_free(res);
    ASSERT_FLOAT_EQ(4000000000.0, select_single_float(&ctx, "SELECT SUM(i) FROM n", "sum"));

    res = sql_exec("SELECT SUM(i) FROM n WHERE f > 1.6", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(VAL_INT, row_get_field(&res->rows[0], "sum").type);
    ASSERT_INT_EQ(2000000000, row_get_field(&res->rows[0], "sum").as.as_int);
    result_free(res);

    catalog_close(&ctx);
    cleanup(path);
}

/* A doubled quote inside a string literal stands for one quote, as in
   standard SQL and on SQLite. */
TEST(sql_string_literal_doubled_quote_escape) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE t (id INT, name STRING)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO t VALUES (1, 'it''s')", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO t VALUES (2, '''quoted''')", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO t VALUES (3, '')", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO t VALUES (4, \"say \"\"hi\"\"\")", &ctx));

    Result* res = sql_exec("SELECT id FROM t WHERE name = 'it''s'", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(1, res->row_count);
    ASSERT_INT_EQ(1, row_get_field(&res->rows[0], "id").as.as_int);
    result_free(res);

    res = sql_exec("SELECT name FROM t WHERE id = 2", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_STRING_EQ("'quoted'", row_get_field(&res->rows[0], "name").as.as_string);
    result_free(res);

    res = sql_exec("SELECT name FROM t WHERE id = 3", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_STRING_EQ("", row_get_field(&res->rows[0], "name").as.as_string);
    result_free(res);

    res = sql_exec("SELECT name FROM t WHERE id = 4", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_STRING_EQ("say \"hi\"", row_get_field(&res->rows[0], "name").as.as_string);
    result_free(res);

    /* Rows 1 and 2 contain a single quote. */
    ASSERT_INT_EQ(2, select_count(&ctx, "SELECT * FROM t WHERE name LIKE '%''%'"));

    catalog_close(&ctx);
    cleanup(path);
}

TEST(sql_scan_returns_rows_from_full_middle_pages) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE big (id INT, pad STRING)", &ctx));
    ASSERT_INT_EQ(1, insert_big_rows(&ctx, 1, BIG_ROWS));

    ASSERT_INT_EQ(BIG_ROWS, select_count(&ctx, "SELECT * FROM big"));
    ASSERT_INT_EQ(BIG_ROWS, select_single_int(&ctx, "SELECT COUNT(*) FROM big", "count"));
    ASSERT_INT_EQ(BIG_ROWS * (BIG_ROWS + 1) / 2,
                  select_single_int(&ctx, "SELECT SUM(id) FROM big", "sum"));
    /* A row that lives on a full middle page. */
    ASSERT_INT_EQ(1, select_count(&ctx, "SELECT id FROM big WHERE id = 100"));

    catalog_close(&ctx);
    cleanup(path);
}

TEST(sql_index_build_sees_rows_on_full_middle_pages) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE big (id INT, pad STRING)", &ctx));
    ASSERT_INT_EQ(1, insert_big_rows(&ctx, 1, BIG_ROWS));
    /* Built after the rows exist, so it has to walk the whole chain. */
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE INDEX big_id ON big (id)", &ctx));

    for (int id = 1; id <= BIG_ROWS; id += 37) {
        char sql[128];
        snprintf(sql, sizeof(sql), "SELECT id FROM big WHERE id = %d", id);
        ASSERT_INT_EQ(1, select_count(&ctx, sql));
    }
    ASSERT_INT_EQ(1, select_count(&ctx, "SELECT id FROM big WHERE id = 300"));
    ASSERT_INT_EQ(BIG_ROWS, select_count(&ctx, "SELECT * FROM big"));

    catalog_close(&ctx);
    cleanup(path);
}

TEST(sql_reads_and_appends_to_legacy_last_page_with_record_count) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE big (id INT, pad STRING)", &ctx));
    ASSERT_INT_EQ(1, insert_big_rows(&ctx, 1, BIG_ROWS));
    catalog_close(&ctx);

    /* Databases written before the fix kept a record count at header offset
       2 on the last page, which is the high half of its next_page field: the
       last page therefore "linked" to page count<<16, far past the end of the
       file. Recreate that state and require scans and appends to cope. */
    ASSERT_INT_EQ(1, catalog_open(&ctx));
    Table* t = catalog_find_table(&ctx, "big");
    ASSERT_PTR_NOT_NULL(t);
    int page_num = t->first_row_page;
    int pages = 0;
    for (;;) {
        uint8_t page[PAGE_SIZE];
        pager_read_page(ctx.pager, page_num, page);
        int32_t next;
        memcpy(&next, page, sizeof(next));
        pages++;
        if (next == 0) {
            int16_t legacy_count = 5;
            memcpy(page + 2, &legacy_count, sizeof(legacy_count));
            pager_write_page(ctx.pager, page_num, page);
            break;
        }
        page_num = (int)next;
    }
    ASSERT(pages > 2);
    catalog_close(&ctx);

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(BIG_ROWS, select_count(&ctx, "SELECT * FROM big"));
    ASSERT_INT_EQ(1, insert_big_rows(&ctx, BIG_ROWS + 1, BIG_ROWS + 1));
    ASSERT_INT_EQ(BIG_ROWS + 1, select_count(&ctx, "SELECT * FROM big"));
    ASSERT_INT_EQ(1, sql_exec_ddl("DROP TABLE big", &ctx));
    catalog_close(&ctx);
    cleanup(path);
}

TEST(sql_insert_after_reopen_keeps_multi_page_chain) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE big (id INT, pad STRING)", &ctx));
    ASSERT_INT_EQ(1, insert_big_rows(&ctx, 1, BIG_ROWS));
    catalog_close(&ctx);

    /* New process equivalent: last_row_page is not persisted, so appends must
       find the real tail instead of treating the first page as the tail. */
    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, insert_big_rows(&ctx, BIG_ROWS + 1, 2 * BIG_ROWS));
    ASSERT_INT_EQ(2 * BIG_ROWS, select_count(&ctx, "SELECT * FROM big"));
    catalog_close(&ctx);

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(2 * BIG_ROWS, select_count(&ctx, "SELECT * FROM big"));
    ASSERT_INT_EQ(1, select_count(&ctx, "SELECT id FROM big WHERE id = 450"));
    catalog_close(&ctx);
    cleanup(path);
}

/* ---- Bind parameters (?) ---------------------------------------------- */

static int bound_row_count(Context* ctx, const char* query, const Value* params, int count) {
    Result* res = sql_exec_params(query, ctx, params, count);
    if (res == NULL) return -1;
    int rows = res->row_count;
    result_free(res);
    return rows;
}

TEST(sql_bound_insert_values_of_each_type) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};
    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE t (id INT, price FLOAT, name STRING)", &ctx));

    /* -5 has no literal spelling in the engine's SQL lexer, so a value that
       round-trips proves it is bound as a value and not pasted into the text. */
    Value first[3] = {value_int(-5), value_float(2.5), value_string(strdup("bob"))};
    ASSERT_INT_EQ(1, sql_exec_ddl_params("INSERT INTO t VALUES (?, ?, ?)", &ctx, first, 3));
    value_release(first[2]);

    Value second[3] = {value_int(2), value_null(), value_bool(1)};
    ASSERT_INT_EQ(1, sql_exec_ddl_params("INSERT INTO t VALUES (?, ?, ?)", &ctx, second, 3));

    Value find_first[1] = {value_int(-5)};
    Result* res = sql_exec_params("SELECT price, name FROM t WHERE id = ?", &ctx, find_first, 1);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(1, res->row_count);
    Row* row = result_next(res);
    ASSERT_FLOAT_EQ(2.5, row_get_field(row, "price").as.as_float);
    ASSERT_STRING_EQ("bob", row_get_field(row, "name").as.as_string);
    result_free(res);

    res = sql_exec("SELECT price FROM t WHERE id = 2", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(1, res->row_count);
    ASSERT_INT_EQ(VAL_NULL, row_get_field(result_next(res), "price").type);
    result_free(res);

    catalog_close(&ctx);
    cleanup(path);
}

TEST(sql_bound_where_update_delete) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};
    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE t (id INT, name STRING)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO t VALUES (1, 'a')", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO t VALUES (2, 'b')", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO t VALUES (3, 'c')", &ctx));

    Value two[1] = {value_int(2)};
    ASSERT_INT_EQ(1, bound_row_count(&ctx, "SELECT id FROM t WHERE id = ?", two, 1));
    Value one[1] = {value_int(1)};
    ASSERT_INT_EQ(2, bound_row_count(&ctx, "SELECT id FROM t WHERE id > ?", one, 1));

    Value upd[2] = {value_string(strdup("B")), value_int(2)};
    ASSERT_INT_EQ(1, sql_exec_ddl_params("UPDATE t SET name = ? WHERE id = ?", &ctx, upd, 2));
    value_release(upd[0]);
    Result* res = sql_exec("SELECT name FROM t WHERE id = 2", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_STRING_EQ("B", row_get_field(result_next(res), "name").as.as_string);
    result_free(res);

    Value del[1] = {value_int(3)};
    ASSERT_INT_EQ(1, sql_exec_ddl_params("DELETE FROM t WHERE id = ?", &ctx, del, 1));
    ASSERT_INT_EQ(2, bound_row_count(&ctx, "SELECT id FROM t", NULL, 0));

    catalog_close(&ctx);
    cleanup(path);
}

TEST(sql_bound_in_list_like_and_limit) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};
    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE t (id INT, name STRING)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO t VALUES (1, 'apple')", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO t VALUES (2, 'apricot')", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO t VALUES (3, 'banana')", &ctx));

    Value in[2] = {value_int(1), value_int(3)};
    ASSERT_INT_EQ(2, bound_row_count(&ctx, "SELECT id FROM t WHERE id IN (?, ?)", in, 2));

    Value like[1] = {value_string(strdup("ap%"))};
    ASSERT_INT_EQ(2, bound_row_count(&ctx, "SELECT id FROM t WHERE name LIKE ?", like, 1));
    value_release(like[0]);

    /* A LIKE pattern has to be text; a number is rejected, not coerced. */
    Value not_text[1] = {value_int(7)};
    ASSERT_INT_EQ(0, bound_row_count(&ctx, "SELECT id FROM t WHERE name LIKE ?", not_text, 1));

    Value limit[1] = {value_int(2)};
    ASSERT_INT_EQ(2, bound_row_count(&ctx, "SELECT id FROM t LIMIT ?", limit, 1));

    catalog_close(&ctx);
    cleanup(path);
}

TEST(sql_bound_string_is_data_not_sql) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};
    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE t (id INT, name STRING)", &ctx));

    /* Both quote characters, a placeholder look-alike and a statement
       terminator; none of it may be interpreted. */
    const char* nasty = "x' OR '1'='1\" ?2; DROP TABLE t";
    Value params[2] = {value_int(1), value_string(strdup(nasty))};
    ASSERT_INT_EQ(1, sql_exec_ddl_params("INSERT INTO t VALUES (?, ?)", &ctx, params, 2));
    value_release(params[1]);
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO t VALUES (2, 'other')", &ctx));

    Value find[1] = {value_string(strdup(nasty))};
    Result* res = sql_exec_params("SELECT id, name FROM t WHERE name = ?", &ctx, find, 1);
    value_release(find[0]);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(1, res->row_count);
    Row* row = result_next(res);
    ASSERT_INT_EQ(1, row_get_field(row, "id").as.as_int);
    ASSERT_STRING_EQ(nasty, row_get_field(row, "name").as.as_string);
    result_free(res);

    ASSERT_INT_EQ(2, bound_row_count(&ctx, "SELECT id FROM t", NULL, 0));
    catalog_close(&ctx);
    cleanup(path);
}

TEST(sql_bound_question_mark_in_literal_is_not_a_placeholder) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};
    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE t (id INT, note STRING)", &ctx));

    Value params[1] = {value_int(4)};
    ASSERT_INT_EQ(1, sql_exec_ddl_params("INSERT INTO t VALUES (?, 'why?')", &ctx, params, 1));
    Result* res = sql_exec("SELECT note FROM t WHERE id = 4", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_STRING_EQ("why?", row_get_field(result_next(res), "note").as.as_string);
    result_free(res);

    catalog_close(&ctx);
    cleanup(path);
}

TEST(sql_bound_placeholder_count_must_match) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};
    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE t (id INT)", &ctx));

    Value one[1] = {value_int(1)};
    Value two[2] = {value_int(1), value_int(2)};
    ASSERT_INT_EQ(0, sql_exec_ddl_params("INSERT INTO t VALUES (?)", &ctx, two, 2));
    ASSERT_INT_EQ(0, sql_exec_ddl_params("INSERT INTO t VALUES (?, ?)", &ctx, one, 1));
    ASSERT_PTR_NULL(sql_exec_params("SELECT id FROM t WHERE id = ? AND id = ?", &ctx, one, 1));
    ASSERT_INT_EQ(0, bound_row_count(&ctx, "SELECT id FROM t", NULL, 0));

    catalog_close(&ctx);
    cleanup(path);
}

TEST(sql_bound_insert_select_keeps_placeholder_positions) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};
    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE src (id INT, name STRING)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE dst (id INT, name STRING)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO src VALUES (1, 'a')", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO src VALUES (2, 'b')", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO src VALUES (3, 'c')", &ctx));

    /* The SELECT is executed on its own, so its placeholders must still refer
       to the second and third values of the whole statement. */
    Value params[3] = {value_int(0), value_int(1), value_string(strdup("c"))};
    ASSERT_INT_EQ(1, sql_exec_ddl_params(
        "INSERT INTO dst SELECT id, name FROM src WHERE id > ? AND name <> ?", &ctx, params + 1, 2));
    value_release(params[2]);
    ASSERT_INT_EQ(1, bound_row_count(&ctx, "SELECT id FROM dst", NULL, 0));

    catalog_close(&ctx);
    cleanup(path);
}

TEST(sql_bound_view_definition_rejects_placeholders) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};
    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE t (id INT)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO t VALUES (1)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO t VALUES (2)", &ctx));

    Value one[1] = {value_int(1)};
    ASSERT_INT_EQ(0, sql_exec_ddl_params("CREATE VIEW v AS SELECT id FROM t WHERE id = ?", &ctx, one, 1));

    /* A view is unaffected by the values bound to the query that reads it. */
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE VIEW v2 AS SELECT id FROM t WHERE id > 1", &ctx));
    Value nine[1] = {value_int(9)};
    ASSERT_INT_EQ(1, bound_row_count(&ctx, "SELECT id FROM v2 WHERE id < ?", nine, 1));

    catalog_close(&ctx);
    cleanup(path);
}

TEST(custom_driver_binds_params_for_exec_and_query) {
    char* path = make_temp_path();
    DBDriver driver;
    custom_driver_init(&driver);
    ASSERT_INT_EQ(1, driver.open(&driver, path));
    ASSERT_INT_EQ(1, driver.exec(&driver, "CREATE TABLE users (id INT, name STRING)", NULL, 0));

    Value ins[2] = {value_int(7), value_string(strdup("o'brien"))};
    ASSERT_INT_EQ(1, driver.exec(&driver, "INSERT INTO users VALUES (?, ?)", ins, 2));
    value_release(ins[1]);

    Value find[1] = {value_int(7)};
    void* result = NULL;
    ASSERT_INT_EQ(1, driver.query(&driver, "SELECT name FROM users WHERE id = ?", find, 1, &result));
    void* row = NULL;
    ASSERT_INT_EQ(1, driver.result_next(&driver, result, &row));
    Value name;
    ASSERT_INT_EQ(1, driver.row_get_field(&driver, row, "name", &name));
    ASSERT_STRING_EQ("o'brien", name.as.as_string);
    value_release(name);
    driver.result_free(&driver, result);

    driver.close(&driver);
    cleanup(path);
}

TEST(custom_driver_reports_placeholder_count_mismatch) {
    char* path = make_temp_path();
    DBDriver driver;
    custom_driver_init(&driver);
    ASSERT_INT_EQ(1, driver.open(&driver, path));
    ASSERT_INT_EQ(1, driver.exec(&driver, "CREATE TABLE users (id INT)", NULL, 0));

    Value two[2] = {value_int(1), value_int(2)};
    ASSERT_INT_EQ(-1, driver.exec(&driver, "INSERT INTO users VALUES (?)", two, 2));
    ASSERT_INT_EQ(1, strstr(driver.error_message, "1 bind placeholder(s) but 2 value(s)") != NULL);

    void* result = NULL;
    Value one[1] = {value_int(1)};
    ASSERT_INT_EQ(0, driver.query(&driver, "SELECT id FROM users WHERE id = ? AND id = ?", one, 1, &result));
    ASSERT_INT_EQ(1, strstr(driver.error_message, "2 bind placeholder(s) but 1 value(s)") != NULL);

    driver.close(&driver);
    cleanup(path);
}

TEST(custom_engine_cursors_bind_placeholders) {
    char* path = make_temp_path();
    DBDriver driver;
    custom_driver_init(&driver);
    ASSERT_INT_EQ(1, driver.open(&driver, path));

    Chunk chunk;
    init_chunk(&chunk);
    char error[256];
    /* A declared cursor and an OPEN ... FOR cursor, both with ?var values.
       Re-opening the declared cursor after changing `lim` must pick up the
       new value: it is read at OPEN, not at declaration. */
    int ok = compile_with_context_and_path(
        "proc main() -> int {\n"
        "    create table t (id int, name string);\n"
        "    insert into t values (1, 'a');\n"
        "    insert into t values (5, 'b');\n"
        "    insert into t values (9, 'c');\n"
        "    int lim = 4;\n"
        "    string want = \"c\";\n"
        "    cursor c is select id from t where id > ?lim;\n"
        "    cursor d;\n"
        "    open d for select id from t where name = ?want;\n"
        "    open c;\n"
        "    int first = 0;\n"
        "    fetch c into first;\n"
        "    int named = 0;\n"
        "    fetch d into named;\n"
        "    close c;\n"
        "    lim = 8;\n"
        "    open c;\n"
        "    int second = 0;\n"
        "    fetch c into second;\n"
        "    return first * 100 + named * 10 + second;\n"
        "}\n",
        &chunk, NULL, error, sizeof(error), NULL);
    ASSERT_INT_EQ(1, ok);

    VM* vm = vm_init();
    vm_set_driver(vm, &driver);
    ASSERT_INT_EQ(INTERPRET_OK, vm_interpret(vm, &chunk));
    ASSERT_INT_EQ(599, vm_pop(vm).as.as_int); /* 5, 9 and 9 */

    vm_free(vm);
    free_chunk(&chunk);
    driver.close(&driver);
    cleanup(path);
}

/* Indexed string predicates vs. full scans                                   */
/* -------------------------------------------------------------------------- */

/* Exactly BTREE_STRING_KEY_BYTES characters: anything appended to it is invisible
   to the index key. */
#define SWEEP_PREFIX "abcdefghijklmnopqrstuvwxyz0123456789"

/* Three families of names, so the sweep covers every case the key encoding
   distinguishes: short names that fit inside the key, long names that are
   identical for the whole significant prefix, and long names that differ inside
   it. */
static void sweep_name(char* buf, size_t size, int i) {
    if (i % 3 == 0) {
        snprintf(buf, size, "name%02d", i);
    } else if (i % 3 == 1) {
        snprintf(buf, size, "%s-%03d", SWEEP_PREFIX, i);
    } else {
        snprintf(buf, size, "zz%02d" SWEEP_PREFIX "-tail", i);
    }
}

static int sweep_count(Context* ctx, const char* op, const char* literal) {
    char query[512];
    snprintf(query, sizeof(query),
             "SELECT id FROM sweep WHERE name %s '%.400s'", op, literal);
    Result* res = sql_exec(query, ctx);
    if (res == NULL) return -1;
    int count = res->row_count;
    result_free(res);
    return count;
}

/* An index may not change an answer. Every comparison is run over the same rows
   before and after CREATE INDEX, across literals that sit inside, on and past
   the significant prefix - the boundary where a mis-widened bound would start
   dropping rows. */
TEST(sql_indexed_string_predicates_match_full_scans) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE sweep (id INT, name STRING)", &ctx));
    for (int i = 0; i < 60; i++) {
        char name[128];
        char query[256];
        sweep_name(name, sizeof(name), i);
        snprintf(query, sizeof(query),
                 "INSERT INTO sweep VALUES (%d, '%s')", i, name);
        ASSERT_INT_EQ(1, sql_exec_ddl(query, &ctx));
    }

    const char* ops[] = { "=", "<", "<=", ">", ">=" };
    const int op_count = 5;

    char literals[16][128];
    int literal_count = 0;
    for (int i = 0; i < 60; i += 7) {
        sweep_name(literals[literal_count++], sizeof(literals[0]), i);
    }
    snprintf(literals[literal_count++], sizeof(literals[0]), "%s", "");
    snprintf(literals[literal_count++], sizeof(literals[0]), "%s", "a");
    snprintf(literals[literal_count++], sizeof(literals[0]), "%s", SWEEP_PREFIX);
    snprintf(literals[literal_count++], sizeof(literals[0]), "%s-999", SWEEP_PREFIX);

    int expected[16 * 5];
    int total = 0;
    int k = 0;
    for (int l = 0; l < literal_count; l++) {
        for (int o = 0; o < op_count; o++) {
            expected[k] = sweep_count(&ctx, ops[o], literals[l]);
            ASSERT(expected[k] >= 0);
            total += expected[k];
            k++;
        }
    }
    /* If nothing matched, the comparison below would be vacuous. */
    ASSERT(total > 0);

    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE INDEX sweep_name ON sweep (name)", &ctx));

    k = 0;
    for (int l = 0; l < literal_count; l++) {
        for (int o = 0; o < op_count; o++) {
            ASSERT_INT_EQ(expected[k], sweep_count(&ctx, ops[o], literals[l]));
            k++;
        }
    }

    catalog_close(&ctx);
    cleanup(path);
}

/* -------------------------------------------------------------------------- */
/* BOOL columns                                                               */
/* -------------------------------------------------------------------------- */

TEST(sql_bool_column_roundtrips_through_the_catalog) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl(
        "CREATE TABLE flags (id INT, active BOOL, label STRING)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO flags VALUES (1, TRUE, 'alice')", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO flags VALUES (2, FALSE, 'bob')", &ctx));
    catalog_close(&ctx);

    /* New process: the column type and both values survive the page. */
    ASSERT_INT_EQ(1, catalog_open(&ctx));
    Result* res = sql_exec("SELECT id, active FROM flags", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(2, res->row_count);

    Row* row = result_next(res);
    ASSERT_PTR_NOT_NULL(row);
    ASSERT_INT_EQ(VAL_BOOL, row->fields[1].value.type);
    ASSERT_INT_EQ(1, row->fields[1].value.as.as_int);

    row = result_next(res);
    ASSERT_PTR_NOT_NULL(row);
    ASSERT_INT_EQ(VAL_BOOL, row->fields[1].value.type);
    ASSERT_INT_EQ(0, row->fields[1].value.as.as_int);
    result_free(res);

    catalog_close(&ctx);
    cleanup(path);
}

TEST(sql_bool_column_filters_and_orders) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE b (id INT, on_ BOOL)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO b VALUES (1, TRUE)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO b VALUES (2, FALSE)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO b VALUES (3, TRUE)", &ctx));

    Result* res = sql_exec("SELECT id FROM b WHERE on_ = TRUE", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(2, res->row_count);
    result_free(res);

    res = sql_exec("SELECT id FROM b WHERE on_ = FALSE", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(1, res->row_count);
    result_free(res);

    /* false sorts before true. */
    res = sql_exec("SELECT id, on_ FROM b ORDER BY on_", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(3, res->row_count);
    Row* row = result_next(res);
    ASSERT_INT_EQ(0, row->fields[1].value.as.as_int);
    row = result_next(res);
    ASSERT_INT_EQ(1, row->fields[1].value.as.as_int);
    result_free(res);

    catalog_close(&ctx);
    cleanup(path);
}

/* A bool DEFAULT is stored as a catalog cell, which is the path that failed
   first: an unknown tag there takes the whole catalog page down, not just the
   column. */
TEST(sql_bool_default_and_not_null_survive_reopen) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl(
        "CREATE TABLE d (id INT, active BOOL NOT NULL, seen BOOL DEFAULT FALSE)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO d VALUES (1, TRUE, TRUE)", &ctx));
    catalog_close(&ctx);

    /* The reopen is the assertion: a catalog it cannot parse fails here. */
    ASSERT_INT_EQ(1, catalog_open(&ctx));
    Result* res = sql_exec("SELECT id, active, seen FROM d", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(1, res->row_count);
    Row* row = result_next(res);
    ASSERT_INT_EQ(VAL_BOOL, row->fields[2].value.type);
    result_free(res);

    /* NOT NULL still applies to a bool column. */
    ASSERT_INT_EQ(0, sql_exec_ddl("INSERT INTO d VALUES (2, NULL, TRUE)", &ctx));

    catalog_close(&ctx);
    cleanup(path);
}

TEST(sql_bool_column_updates_and_indexes) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE u (id INT, active BOOL)", &ctx));
    for (int i = 0; i < 20; i++) {
        char query[128];
        snprintf(query, sizeof(query), "INSERT INTO u VALUES (%d, %s)",
                 i, i % 2 == 0 ? "TRUE" : "FALSE");
        ASSERT_INT_EQ(1, sql_exec_ddl(query, &ctx));
    }
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE INDEX u_active ON u (active)", &ctx));

    /* The index must agree with the full scan it replaces. */
    Result* res = sql_exec("SELECT id FROM u WHERE active = TRUE", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(10, res->row_count);
    result_free(res);

    ASSERT_INT_EQ(1, sql_exec_ddl("UPDATE u SET active = FALSE WHERE id = 0", &ctx));
    res = sql_exec("SELECT id FROM u WHERE active = TRUE", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(9, res->row_count);
    result_free(res);

    ASSERT_INT_EQ(1, sql_exec_ddl("DELETE FROM u WHERE active = FALSE", &ctx));
    res = sql_exec("SELECT id FROM u", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(9, res->row_count);
    result_free(res);

    catalog_close(&ctx);
    cleanup(path);
}

/* -------------------------------------------------------------------------- */
/* DATE and TIMESTAMP columns                                                 */
/* -------------------------------------------------------------------------- */

TEST(sql_date_and_timestamp_columns_roundtrip) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl(
        "CREATE TABLE ev (id INT, day DATE, at TIMESTAMP)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl(
        "INSERT INTO ev VALUES (1, '2024-03-01', '2024-03-01 09:30:00')", &ctx));
    catalog_close(&ctx);

    /* New process: the column types and the text both survive the page. */
    ASSERT_INT_EQ(1, catalog_open(&ctx));
    Result* res = sql_exec("SELECT id, day, at FROM ev", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(1, res->row_count);
    Row* row = result_next(res);
    ASSERT_PTR_NOT_NULL(row);
    /* A string literal written into a date column is stored as a date. */
    ASSERT_INT_EQ(VAL_DATE, row->fields[1].value.type);
    ASSERT_STRING_EQ("2024-03-01", row->fields[1].value.as.as_string);
    ASSERT_INT_EQ(VAL_TIMESTAMP, row->fields[2].value.type);
    ASSERT_STRING_EQ("2024-03-01 09:30:00", row->fields[2].value.as.as_string);
    result_free(res);

    catalog_close(&ctx);
    cleanup(path);
}

/* Canonical text is fixed width and orders chronologically byte by byte, which
   is what lets dates share the string key space and compare with strcmp. */
TEST(sql_date_columns_compare_chronologically) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE d (id INT, day DATE)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO d VALUES (1, '2024-03-01')", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO d VALUES (2, '2023-12-25')", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO d VALUES (3, '2024-07-04')", &ctx));

    Result* res = sql_exec("SELECT id FROM d WHERE day = '2023-12-25'", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(1, res->row_count);
    result_free(res);

    res = sql_exec("SELECT id FROM d WHERE day > '2024-01-01'", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(2, res->row_count);
    result_free(res);

    res = sql_exec("SELECT id, day FROM d ORDER BY day", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(3, res->row_count);
    Row* row = result_next(res);
    ASSERT_STRING_EQ("2023-12-25", row->fields[1].value.as.as_string);
    row = result_next(res);
    ASSERT_STRING_EQ("2024-03-01", row->fields[1].value.as.as_string);
    row = result_next(res);
    ASSERT_STRING_EQ("2024-07-04", row->fields[1].value.as.as_string);
    result_free(res);

    catalog_close(&ctx);
    cleanup(path);
}

/* A date column that accepted arbitrary text would read back as something that
   is not a date, so the write is refused instead. */
TEST(sql_date_columns_reject_text_that_is_not_a_date) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl(
        "CREATE TABLE r (id INT, day DATE, at TIMESTAMP)", &ctx));

    ASSERT_INT_EQ(0, sql_exec_ddl(
        "INSERT INTO r VALUES (1, 'not-a-date', '2024-01-01 00:00:00')", &ctx));
    ASSERT_INT_EQ(0, sql_exec_ddl(
        "INSERT INTO r VALUES (2, '2024-1-1', '2024-01-01 00:00:00')", &ctx));
    /* A date is not a timestamp: the widths differ. */
    ASSERT_INT_EQ(0, sql_exec_ddl(
        "INSERT INTO r VALUES (3, '2024-01-01', '2024-01-01')", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl(
        "INSERT INTO r VALUES (4, '2024-01-01', '2024-01-01 00:00:00')", &ctx));

    Result* res = sql_exec("SELECT id FROM r", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(1, res->row_count);
    result_free(res);

    /* UPDATE is held to the same rule. */
    ASSERT_INT_EQ(0, sql_exec_ddl("UPDATE r SET day = 'nope' WHERE id = 4", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("UPDATE r SET day = '2025-02-02' WHERE id = 4", &ctx));

    res = sql_exec("SELECT day FROM r WHERE id = 4", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    Row* row = result_next(res);
    ASSERT_STRING_EQ("2025-02-02", row->fields[0].value.as.as_string);
    ASSERT_INT_EQ(VAL_DATE, row->fields[0].value.type);
    result_free(res);

    catalog_close(&ctx);
    cleanup(path);
}

TEST(sql_date_default_and_index_and_delete) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl(
        "CREATE TABLE dd (id INT, day DATE DEFAULT '2000-01-01')", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO dd VALUES (1, NULL)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO dd VALUES (2, '2024-06-01')", &ctx));
    catalog_close(&ctx);

    /* The DEFAULT is a catalog cell, so this reopen exercises its encoding. */
    ASSERT_INT_EQ(1, catalog_open(&ctx));
    Result* res = sql_exec("SELECT day FROM dd WHERE id = 1", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    Row* row = result_next(res);
    ASSERT_INT_EQ(VAL_DATE, row->fields[0].value.type);
    ASSERT_STRING_EQ("2000-01-01", row->fields[0].value.as.as_string);
    result_free(res);

    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE INDEX dd_day ON dd (day)", &ctx));
    res = sql_exec("SELECT id FROM dd WHERE day = '2024-06-01'", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(1, res->row_count);
    result_free(res);

    ASSERT_INT_EQ(1, sql_exec_ddl("DELETE FROM dd WHERE day = '2000-01-01'", &ctx));
    res = sql_exec("SELECT id FROM dd", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(1, res->row_count);
    result_free(res);

    catalog_close(&ctx);
    cleanup(path);
}

/* ---- The catalog is one page; DDL that would overflow it fails (#121) ---- */

static int count_rows(Context* ctx, const char* query) {
    Result* res = sql_exec(query, ctx);
    int n = res != NULL ? res->row_count : -1;
    result_free(res);
    return n;
}

/* Name of the i-th filler table: long, so a few dozen fill the page. */
static void big_table_name(int i, char* out, size_t size) {
    snprintf(out, size, "big_%0180d", i);
}

/* Creates big tables until the catalog refuses one; returns how many fit. */
static int fill_catalog_with_big_tables(Context* ctx) {
    for (int i = 0; i < 64; i++) {
        char name[200];
        char sql[300];
        big_table_name(i, name, sizeof(name));
        snprintf(sql, sizeof(sql), "CREATE TABLE %s (id INT, label STRING)", name);
        if (!sql_exec_ddl(sql, ctx)) return i;
    }
    return 64;
}

TEST(sql_catalog_full_create_table_fails_cleanly) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};
    ASSERT_INT_EQ(1, catalog_open(&ctx));
    int created = fill_catalog_with_big_tables(&ctx);
    /* The page filled up well before the 64-table limit. */
    ASSERT_INT_EQ(1, created > 0 && created < 64);
    char name[200];
    big_table_name(created, name, sizeof(name));
    ASSERT_PTR_NULL(catalog_find_table(&ctx, name));

    big_table_name(0, name, sizeof(name));
    char sql[300];
    snprintf(sql, sizeof(sql), "INSERT INTO %s VALUES (1, 'one')", name);
    ASSERT_INT_EQ(1, sql_exec_ddl(sql, &ctx));
    catalog_close(&ctx);

    /* Every table that was accepted is on disk, the refused one is not. */
    ASSERT_INT_EQ(1, catalog_open(&ctx));
    for (int i = 0; i < created; i++) {
        big_table_name(i, name, sizeof(name));
        ASSERT_PTR_NOT_NULL(catalog_find_table(&ctx, name));
    }
    big_table_name(created, name, sizeof(name));
    ASSERT_PTR_NULL(catalog_find_table(&ctx, name));
    big_table_name(0, name, sizeof(name));
    snprintf(sql, sizeof(sql), "SELECT id FROM %s", name);
    ASSERT_INT_EQ(1, count_rows(&ctx, sql));
    catalog_close(&ctx);
    cleanup(path);
}

TEST(sql_catalog_full_reports_the_reason) {
    char* path = make_temp_path();
    DBDriver driver;
    custom_driver_init(&driver);
    ASSERT_INT_EQ(1, driver.open(&driver, path));
    int refused = 0;
    for (int i = 0; i < 64 && !refused; i++) {
        char name[200];
        char sql[300];
        big_table_name(i, name, sizeof(name));
        snprintf(sql, sizeof(sql), "CREATE TABLE %s (id INT)", name);
        refused = driver.exec(&driver, sql, NULL, 0) < 0;
    }
    ASSERT_INT_EQ(1, refused);
    ASSERT_PTR_NOT_NULL(strstr(driver.error_message, "catalog page full"));
    driver.close(&driver);
    cleanup(path);
}

TEST(sql_catalog_full_rolls_back_defaults_indexes_and_columns) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};
    ASSERT_INT_EQ(1, catalog_open(&ctx));
    int created = fill_catalog_with_big_tables(&ctx);
    ASSERT_INT_EQ(1, created > 1 && created < 64);
    /* Free one big table's worth of room, top the page up with small tables
       (14 bytes each), then drop the last of those: between 14 and 27 bytes
       are left. */
    char sql[400];
    char big_last[200];
    big_table_name(created - 1, big_last, sizeof(big_last));
    snprintf(sql, sizeof(sql), "DROP TABLE %s", big_last);
    ASSERT_INT_EQ(1, sql_exec_ddl(sql, &ctx));
    created--;
    int small = 0;
    for (; small < 64 - created; small++) {
        snprintf(sql, sizeof(sql), "CREATE TABLE s%02d (a INT)", small);
        if (!sql_exec_ddl(sql, &ctx)) break;
    }
    ASSERT_INT_EQ(1, small > 0 && small < 64 - created);
    snprintf(sql, sizeof(sql), "DROP TABLE s%02d", small - 1);
    ASSERT_INT_EQ(1, sql_exec_ddl(sql, &ctx));

    char long_name[101];
    memset(long_name, 'n', 100);
    long_name[100] = '\0';

    /* The bare table (12 bytes) fits; its 100-character DEFAULT does not. */
    snprintf(sql, sizeof(sql), "CREATE TABLE x (a STRING DEFAULT '%s')", long_name);
    ASSERT_INT_EQ(0, sql_exec_ddl(sql, &ctx));
    ASSERT_PTR_NULL(catalog_find_table(&ctx, "x"));

    char big0[200];
    big_table_name(0, big0, sizeof(big0));
    snprintf(sql, sizeof(sql), "INSERT INTO %s VALUES (1, 'one')", big0);
    ASSERT_INT_EQ(1, sql_exec_ddl(sql, &ctx));

    snprintf(sql, sizeof(sql), "CREATE INDEX %s ON %s (id)", long_name, big0);
    ASSERT_INT_EQ(0, sql_exec_ddl(sql, &ctx));
    ASSERT_INT_EQ(0, catalog_find_table(&ctx, big0)->index_count);

    snprintf(sql, sizeof(sql), "ALTER TABLE %s ADD COLUMN %s INT", big0, long_name);
    ASSERT_INT_EQ(0, sql_exec_ddl(sql, &ctx));
    ASSERT_INT_EQ(2, catalog_find_table(&ctx, big0)->column_count);

    /* Nothing leaked into the page: a table that needs the freed space still
       fits. */
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE y (a INT)", &ctx));
    catalog_close(&ctx);

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_PTR_NULL(catalog_find_table(&ctx, "x"));
    ASSERT_PTR_NOT_NULL(catalog_find_table(&ctx, "y"));
    ASSERT_INT_EQ(0, catalog_find_table(&ctx, big0)->index_count);
    ASSERT_INT_EQ(2, catalog_find_table(&ctx, big0)->column_count);
    snprintf(sql, sizeof(sql), "SELECT id FROM %s", big0);
    ASSERT_INT_EQ(1, count_rows(&ctx, sql));
    catalog_close(&ctx);
    cleanup(path);
}

TEST(sql_catalog_rejects_a_table_record_past_the_page) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};
    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE t (a INT)", &ctx));
    catalog_close(&ctx);

    /* Keep the magic and claim one table with an empty name and 255
       columns. Every byte after that is 0x10: each column says it has a
       16-byte name, type 16 and flags 16 (no DEFAULT), 19 bytes in all, so
       the columns run off the end of the page. */
    uint8_t page[PAGE_SIZE];
    FILE* f = fopen(path, "r+b");
    ASSERT_PTR_NOT_NULL(f);
    ASSERT_INT_EQ(0, fseek(f, PAGE_SIZE, SEEK_SET));
    ASSERT_INT_EQ(PAGE_SIZE, (int)fread(page, 1, PAGE_SIZE, f));
    uint32_t table_count = 1;
    memcpy(page + 4, &table_count, sizeof(table_count));
    page[8] = 0;   /* name length */
    page[9] = 255; /* column count */
    memset(page + 10, 0x10, PAGE_SIZE - 10);
    ASSERT_INT_EQ(0, fseek(f, PAGE_SIZE, SEEK_SET));
    ASSERT_INT_EQ(PAGE_SIZE, (int)fwrite(page, 1, PAGE_SIZE, f));
    fclose(f);

    ASSERT_INT_EQ(0, catalog_open(&ctx));
    catalog_close(&ctx);
    cleanup(path);
}

/* ---- UPDATE / DELETE in place (#107) ------------------------------------ */

static int rows_where(Context* ctx, const char* fmt, int arg) {
    char sql[160];
    snprintf(sql, sizeof(sql), fmt, arg);
    Result* res = sql_exec(sql, ctx);
    int n = res != NULL ? res->row_count : -1;
    result_free(res);
    return n;
}

/* 600 rows over several pages, indexed on every column. */
static void make_indexed_table(Context* ctx) {
    sql_exec_ddl("CREATE TABLE t (id INT, v INT, s STRING)", ctx);
    for (int i = 0; i < 600; i++) {
        char sql[128];
        snprintf(sql, sizeof(sql), "INSERT INTO t VALUES (%d, %d, 'name-%05d')", i, i % 10, i);
        sql_exec_ddl(sql, ctx);
    }
    sql_exec_ddl("CREATE INDEX t_id ON t (id)", ctx);
    sql_exec_ddl("CREATE INDEX t_v ON t (v)", ctx);
    sql_exec_ddl("CREATE INDEX t_s ON t (s)", ctx);
}

TEST(sql_update_in_place_keeps_indexes_in_step) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};
    ASSERT_INT_EQ(1, catalog_open(&ctx));
    make_indexed_table(&ctx);

    /* Same-size value on an indexed column. */
    ASSERT_INT_EQ(1, sql_exec_ddl("UPDATE t SET v = 77 WHERE id = 523", &ctx));
    ASSERT_INT_EQ(1, rows_where(&ctx, "SELECT id FROM t WHERE v = %d", 77));
    ASSERT_INT_EQ(59, rows_where(&ctx, "SELECT id FROM t WHERE v = %d", 3));
    ASSERT_INT_EQ(1, rows_where(&ctx, "SELECT id FROM t WHERE id = %d AND v = 77", 523));

    /* A shorter string stays in the old record; a longer one takes the
       general path. Both stay reachable through the index. */
    ASSERT_INT_EQ(1, sql_exec_ddl("UPDATE t SET s = 'x' WHERE id = 200", &ctx));
    ASSERT_INT_EQ(1, count_rows(&ctx, "SELECT id FROM t WHERE s = 'x'"));
    ASSERT_INT_EQ(0, count_rows(&ctx, "SELECT id FROM t WHERE s = 'name-00200'"));
    ASSERT_INT_EQ(1, sql_exec_ddl(
        "UPDATE t SET s = 'a-much-longer-name-than-before' WHERE id = 201", &ctx));
    ASSERT_INT_EQ(1, count_rows(&ctx, "SELECT id FROM t WHERE s = 'a-much-longer-name-than-before'"));
    ASSERT_INT_EQ(1, count_rows(&ctx, "SELECT id FROM t WHERE s = 'x'"));

    /* Many rows at once, matched by a full scan (no index on the term). */
    ASSERT_INT_EQ(1, sql_exec_ddl("UPDATE t SET v = 50 WHERE s LIKE 'name-001%'", &ctx));
    ASSERT_INT_EQ(100, rows_where(&ctx, "SELECT id FROM t WHERE v = %d", 50));

    /* No match writes nothing and succeeds. */
    ASSERT_INT_EQ(1, sql_exec_ddl("UPDATE t SET v = 1 WHERE id = 99999", &ctx));
    catalog_close(&ctx);

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(600, count_rows(&ctx, "SELECT id FROM t"));
    ASSERT_INT_EQ(1, rows_where(&ctx, "SELECT id FROM t WHERE v = %d", 77));
    ASSERT_INT_EQ(1, count_rows(&ctx, "SELECT id FROM t WHERE s = 'x'"));
    ASSERT_INT_EQ(100, rows_where(&ctx, "SELECT id FROM t WHERE v = %d", 50));
    for (int i = 0; i < 600; i++) {
        ASSERT_INT_EQ(1, rows_where(&ctx, "SELECT id FROM t WHERE id = %d", i));
    }
    catalog_close(&ctx);
    cleanup(path);
}

TEST(sql_update_in_place_enforces_not_null) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};
    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE n (id INT, v INT NOT NULL)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO n VALUES (1, 10)", &ctx));
    ASSERT_INT_EQ(0, sql_exec_ddl("UPDATE n SET v = NULL WHERE id = 1", &ctx));
    ASSERT_INT_EQ(1, count_rows(&ctx, "SELECT id FROM n WHERE v = 10"));
    /* No row matches, so nothing violates the constraint. */
    ASSERT_INT_EQ(1, sql_exec_ddl("UPDATE n SET v = NULL WHERE id = 2", &ctx));
    catalog_close(&ctx);
    cleanup(path);
}

TEST(sql_delete_in_place_moves_index_entries_of_shifted_rows) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};
    ASSERT_INT_EQ(1, catalog_open(&ctx));
    make_indexed_table(&ctx);

    /* One row from the middle of a page: the rows after it on that page
       move, and must still be found through every index. */
    ASSERT_INT_EQ(1, sql_exec_ddl("DELETE FROM t WHERE id = 100", &ctx));
    ASSERT_INT_EQ(599, count_rows(&ctx, "SELECT id FROM t"));
    ASSERT_INT_EQ(0, rows_where(&ctx, "SELECT id FROM t WHERE id = %d", 100));
    for (int i = 101; i < 140; i++) {
        ASSERT_INT_EQ(1, rows_where(&ctx, "SELECT id FROM t WHERE id = %d", i));
    }
    ASSERT_INT_EQ(59, rows_where(&ctx, "SELECT id FROM t WHERE v = %d", 0));
    ASSERT_INT_EQ(1, count_rows(&ctx, "SELECT id FROM t WHERE s = 'name-00101'"));

    /* Rows spread over every page, matched through the v index. */
    ASSERT_INT_EQ(1, sql_exec_ddl("DELETE FROM t WHERE v = 3", &ctx));
    ASSERT_INT_EQ(539, count_rows(&ctx, "SELECT id FROM t"));
    catalog_close(&ctx);

    ASSERT_INT_EQ(1, catalog_open(&ctx));
    for (int i = 0; i < 600; i++) {
        int expected = (i == 100 || i % 10 == 3) ? 0 : 1;
        ASSERT_INT_EQ(expected, rows_where(&ctx, "SELECT id FROM t WHERE id = %d", i));
    }
    for (int v = 0; v < 10; v++) {
        int expected = v == 3 ? 0 : (v == 0 ? 59 : 60);
        ASSERT_INT_EQ(expected, rows_where(&ctx, "SELECT id FROM t WHERE v = %d", v));
    }
    /* New rows still append after the compacted pages. */
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO t VALUES (1000, 3, 'new')", &ctx));
    ASSERT_INT_EQ(1, rows_where(&ctx, "SELECT id FROM t WHERE v = %d", 3));
    /* Everything, then the table is empty but usable. */
    ASSERT_INT_EQ(1, sql_exec_ddl("DELETE FROM t", &ctx));
    ASSERT_INT_EQ(0, count_rows(&ctx, "SELECT id FROM t"));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO t VALUES (1, 1, 'again')", &ctx));
    ASSERT_INT_EQ(1, rows_where(&ctx, "SELECT id FROM t WHERE id = %d", 1));
    catalog_close(&ctx);
    cleanup(path);
}

/* ---- Filtered full scans (#115) ----------------------------------------- */

TEST(sql_filtered_scan_returns_owned_copies_of_matching_rows) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};
    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE p (id INT, name STRING, tag STRING)", &ctx));
    for (int i = 0; i < 1000; i++) {
        char sql[128];
        snprintf(sql, sizeof(sql), "INSERT INTO p VALUES (%d, 'person-%d', '%s')",
                 i, i, i % 100 == 7 ? "keep" : "drop");
        ASSERT_INT_EQ(1, sql_exec_ddl(sql, &ctx));
    }
    /* No index: the scan filters on a string column, then on an int. */
    Result* res = sql_exec("SELECT id, name, tag FROM p WHERE tag = 'keep' ORDER BY id", &ctx);
    ASSERT_PTR_NOT_NULL(res);
    ASSERT_INT_EQ(10, res->row_count);
    for (int i = 0; i < 10; i++) {
        Row* row = result_next(res);
        ASSERT_PTR_NOT_NULL(row);
        char expected[32];
        snprintf(expected, sizeof(expected), "person-%d", i * 100 + 7);
        ASSERT_INT_EQ(i * 100 + 7, row_get_field(row, "id").as.as_int);
        ASSERT_STRING_EQ(expected, row_get_field(row, "name").as.as_string);
        ASSERT_STRING_EQ("keep", row_get_field(row, "tag").as.as_string);
    }
    result_free(res);
    ASSERT_INT_EQ(1, count_rows(&ctx, "SELECT id FROM p WHERE id = 999 AND tag = 'drop'"));
    ASSERT_INT_EQ(0, count_rows(&ctx, "SELECT id FROM p WHERE id = 1000"));
    ASSERT_INT_EQ(1000, count_rows(&ctx, "SELECT id FROM p"));
    catalog_close(&ctx);
    cleanup(path);
}

/* ---- PRIMARY KEY / UNIQUE checks through an index (#106) --------------- */

static int unique_rows(Context* ctx, const char* query) {
    Result* res = sql_exec(query, ctx);
    int n = res != NULL ? res->row_count : -1;
    result_free(res);
    return n;
}

TEST(sql_unique_checks_reject_duplicates_through_the_index) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};
    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl(
        "CREATE TABLE u (id INT PRIMARY KEY, email STRING UNIQUE, score FLOAT UNIQUE)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO u VALUES (1, 'a@x', 1.5)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO u VALUES (2, 'b@x', 2.5)", &ctx));
    ASSERT_INT_EQ(0, sql_exec_ddl("INSERT INTO u VALUES (1, 'c@x', 3.5)", &ctx));
    ASSERT_INT_EQ(0, sql_exec_ddl("INSERT INTO u VALUES (3, 'b@x', 3.5)", &ctx));
    ASSERT_INT_EQ(0, sql_exec_ddl("INSERT INTO u VALUES (3, 'c@x', 2.5)", &ctx));

    /* NULL never conflicts, however often it appears. */
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO u VALUES (3, NULL, NULL)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO u VALUES (4, NULL, NULL)", &ctx));

    /* Index keys keep only 36 bytes of a string: two longer strings with the
       same 36-byte prefix are different values, an exact repeat is not. */
    const char* long_a = "a-very-long-address-that-shares-its-prefix-1@example.com";
    const char* long_b = "a-very-long-address-that-shares-its-prefix-2@example.com";
    char sql[256];
    snprintf(sql, sizeof(sql), "INSERT INTO u VALUES (5, '%s', 5.5)", long_a);
    ASSERT_INT_EQ(1, sql_exec_ddl(sql, &ctx));
    snprintf(sql, sizeof(sql), "INSERT INTO u VALUES (6, '%s', 6.5)", long_b);
    ASSERT_INT_EQ(1, sql_exec_ddl(sql, &ctx));
    snprintf(sql, sizeof(sql), "INSERT INTO u VALUES (7, '%s', 7.5)", long_a);
    ASSERT_INT_EQ(0, sql_exec_ddl(sql, &ctx));

    /* 0.0 and -0.0 are equal values with different index keys. */
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO u VALUES (8, 'zero@x', 0.0)", &ctx));
    Value neg_zero[1] = {value_float(-0.0)};
    ASSERT_INT_EQ(0, sql_exec_ddl_params("INSERT INTO u VALUES (9, 'nz@x', ?)", &ctx, neg_zero, 1));
    ASSERT_INT_EQ(7, unique_rows(&ctx, "SELECT id FROM u"));
    catalog_close(&ctx);

    /* The index is in the catalog: after a reopen it still enforces the
       constraints and serves lookups. */
    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(0, sql_exec_ddl("INSERT INTO u VALUES (2, 'new@x', 9.5)", &ctx));
    ASSERT_INT_EQ(0, sql_exec_ddl("INSERT INTO u VALUES (10, 'a@x', 9.5)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO u VALUES (10, 'new@x', 9.5)", &ctx));
    ASSERT_INT_EQ(1, unique_rows(&ctx, "SELECT id FROM u WHERE id = 10"));
    catalog_close(&ctx);
    cleanup(path);
}

TEST(sql_unique_check_matches_an_int_against_a_stored_float) {
    /* INSERT ... SELECT copies cells as they are, so an int column can hold
       a float; 2 and 2.0 are still the same value. */
    char* path = make_temp_path();
    Context ctx = {path, NULL};
    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE src (id FLOAT)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO src VALUES (2.0)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE t (id INT PRIMARY KEY)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO t SELECT id FROM src", &ctx));
    ASSERT_INT_EQ(0, sql_exec_ddl("INSERT INTO t VALUES (2)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO t VALUES (3)", &ctx));
    catalog_close(&ctx);
    cleanup(path);
}

TEST(sql_unique_check_survives_a_dropped_or_missing_index) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};
    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE t (id INT PRIMARY KEY, v INT)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO t VALUES (1, 10)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO t VALUES (2, 20)", &ctx));
    /* Dropping the implicit index loses nothing: the next check builds it
       again from the stored rows. */
    ASSERT_INT_EQ(1, sql_exec_ddl("DROP INDEX __unique_id ON t", &ctx));
    ASSERT_INT_EQ(0, sql_exec_ddl("INSERT INTO t VALUES (2, 30)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO t VALUES (3, 30)", &ctx));
    ASSERT_INT_EQ(0, sql_exec_ddl("INSERT INTO t VALUES (3, 40)", &ctx));

    /* A table whose index slots are all taken falls back to scanning. */
    ASSERT_INT_EQ(1, sql_exec_ddl(
        "CREATE TABLE wide (a INT, b INT, c INT, d INT, e INT, f INT, g INT, h INT, k INT UNIQUE)",
        &ctx));
    static const char* const cols[] = {"a", "b", "c", "d", "e", "f", "g", "h"};
    for (int i = 0; i < 8; i++) {
        char sql[96];
        snprintf(sql, sizeof(sql), "CREATE INDEX wide_%s ON wide (%s)", cols[i], cols[i]);
        ASSERT_INT_EQ(1, sql_exec_ddl(sql, &ctx));
    }
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO wide VALUES (1, 1, 1, 1, 1, 1, 1, 1, 7)", &ctx));
    ASSERT_INT_EQ(0, sql_exec_ddl("INSERT INTO wide VALUES (2, 2, 2, 2, 2, 2, 2, 2, 7)", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("INSERT INTO wide VALUES (2, 2, 2, 2, 2, 2, 2, 2, 8)", &ctx));
    catalog_close(&ctx);
    cleanup(path);
}

TEST(sql_update_duplicate_check_sorts_the_row_set) {
    char* path = make_temp_path();
    Context ctx = {path, NULL};
    ASSERT_INT_EQ(1, catalog_open(&ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("CREATE TABLE t (id INT PRIMARY KEY, code STRING UNIQUE, n INT)", &ctx));
    for (int i = 0; i < 300; i++) {
        char sql[128];
        if (i % 3 == 0) {
            snprintf(sql, sizeof(sql), "INSERT INTO t VALUES (%d, NULL, %d)", i, i % 7);
        } else {
            snprintf(sql, sizeof(sql), "INSERT INTO t VALUES (%d, 'c%d', %d)", i, i, i % 7);
        }
        ASSERT_INT_EQ(1, sql_exec_ddl(sql, &ctx));
    }
    /* Distinct values (and any number of NULLs) pass. */
    ASSERT_INT_EQ(1, sql_exec_ddl("UPDATE t SET n = 5 WHERE n = 6", &ctx));
    ASSERT_INT_EQ(1, sql_exec_ddl("UPDATE t SET code = NULL WHERE n = 5", &ctx));
    /* One value given to two rows is rejected, wherever in the set they are,
       and nothing is written. */
    ASSERT_INT_EQ(0, sql_exec_ddl("UPDATE t SET code = 'same' WHERE n = 4", &ctx));
    ASSERT_INT_EQ(0, unique_rows(&ctx, "SELECT id FROM t WHERE code = 'same'"));
    ASSERT_INT_EQ(0, sql_exec_ddl("UPDATE t SET code = 'c1' WHERE id = 298", &ctx));
    ASSERT_INT_EQ(1, unique_rows(&ctx, "SELECT id FROM t WHERE code = 'c298'"));
    ASSERT_INT_EQ(1, sql_exec_ddl("UPDATE t SET code = 'only' WHERE id = 1", &ctx));
    ASSERT_INT_EQ(1, unique_rows(&ctx, "SELECT id FROM t WHERE code = 'only'"));
    catalog_close(&ctx);
    cleanup(path);
}

int main(void) {
    RUN_TEST(sql_scan_returns_rows_from_full_middle_pages);
    RUN_TEST(sql_index_build_sees_rows_on_full_middle_pages);
    RUN_TEST(sql_reads_and_appends_to_legacy_last_page_with_record_count);
    RUN_TEST(sql_insert_after_reopen_keeps_multi_page_chain);
    RUN_TEST(sql_create_table_persists_schema);
    RUN_TEST(sql_insert_and_select_persists_rows);
    RUN_TEST(sql_select_specific_columns);
    RUN_TEST(sql_where_equals_int);
    RUN_TEST(sql_where_greater_than_int);
    RUN_TEST(sql_select_unknown_table_returns_empty_result);
    RUN_TEST(sql_compiler_loop_uses_persisted_table);
    RUN_TEST(custom_driver_runs_create_and_insert);
    RUN_TEST(sql_update_modifies_rows);
    RUN_TEST(sql_update_with_string);
    RUN_TEST(sql_delete_removes_rows);
    RUN_TEST(sql_update_and_delete_persist);
    RUN_TEST(sql_order_by_asc);
    RUN_TEST(sql_order_by_desc);
    RUN_TEST(sql_limit_clause);
    RUN_TEST(sql_aggregate_count);
    RUN_TEST(sql_aggregate_sum_avg_min_max);
    RUN_TEST(sql_aggregate_with_where);
    RUN_TEST(sql_string_literal_doubled_quote_escape);
    RUN_TEST(sql_sum_result_type_follows_inputs);
    RUN_TEST(sql_insert_multiple_value_rows);
    RUN_TEST(sql_insert_multiple_rows_checks_all_before_inserting);
    RUN_TEST(sql_join_basic);
    RUN_TEST(sql_join_select_star);
    RUN_TEST(sql_join_with_where_limit);
    RUN_TEST(sql_join_no_matches);
    RUN_TEST(sql_qualified_column_select);
    RUN_TEST(sql_qualified_column_where);
    RUN_TEST(sql_qualified_column_order_by);
    RUN_TEST(sql_qualified_column_in_join);
    RUN_TEST(sql_left_join_includes_unmatched_left);
    RUN_TEST(sql_left_join_no_matches);
    RUN_TEST(sql_left_join_with_where);
    RUN_TEST(sql_group_by_count_sum);
    RUN_TEST(sql_group_by_avg_min_max);
    RUN_TEST(sql_group_by_with_where);
    RUN_TEST(sql_insert_into_select_star);
    RUN_TEST(sql_insert_into_select_specific_columns);
    RUN_TEST(sql_insert_into_select_with_where);
    RUN_TEST(sql_bound_insert_values_of_each_type);
    RUN_TEST(sql_bound_where_update_delete);
    RUN_TEST(sql_bound_in_list_like_and_limit);
    RUN_TEST(sql_bound_string_is_data_not_sql);
    RUN_TEST(sql_bound_question_mark_in_literal_is_not_a_placeholder);
    RUN_TEST(sql_bound_placeholder_count_must_match);
    RUN_TEST(sql_bound_insert_select_keeps_placeholder_positions);
    RUN_TEST(sql_bound_view_definition_rejects_placeholders);
    RUN_TEST(custom_driver_binds_params_for_exec_and_query);
    RUN_TEST(custom_driver_reports_placeholder_count_mismatch);
    RUN_TEST(custom_engine_cursors_bind_placeholders);
    RUN_TEST(sql_indexed_string_predicates_match_full_scans);
    RUN_TEST(sql_bool_column_roundtrips_through_the_catalog);
    RUN_TEST(sql_bool_column_filters_and_orders);
    RUN_TEST(sql_bool_default_and_not_null_survive_reopen);
    RUN_TEST(sql_bool_column_updates_and_indexes);
    RUN_TEST(sql_date_and_timestamp_columns_roundtrip);
    RUN_TEST(sql_date_columns_compare_chronologically);
    RUN_TEST(sql_date_columns_reject_text_that_is_not_a_date);
    RUN_TEST(sql_date_default_and_index_and_delete);
    RUN_TEST(sql_catalog_full_create_table_fails_cleanly);
    RUN_TEST(sql_catalog_full_reports_the_reason);
    RUN_TEST(sql_catalog_full_rolls_back_defaults_indexes_and_columns);
    RUN_TEST(sql_catalog_rejects_a_table_record_past_the_page);
    RUN_TEST(sql_update_in_place_keeps_indexes_in_step);
    RUN_TEST(sql_update_in_place_enforces_not_null);
    RUN_TEST(sql_delete_in_place_moves_index_entries_of_shifted_rows);
    RUN_TEST(sql_filtered_scan_returns_owned_copies_of_matching_rows);
    RUN_TEST(sql_unique_checks_reject_duplicates_through_the_index);
    RUN_TEST(sql_unique_check_matches_an_int_against_a_stored_float);
    RUN_TEST(sql_unique_check_survives_a_dropped_or_missing_index);
    RUN_TEST(sql_update_duplicate_check_sorts_the_row_set);
    TEST_SUMMARY();
}
