#include "test_harness.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* `args` go on the auspex command line, e.g. "--db /tmp/x.db"; "" for none. */
static int run_repl_with_args(const char* args, const char* input,
                              char* output, size_t output_size) {
    FILE* in = fopen("/tmp/repl_in.txt", "w");
    if (in == NULL) {
        output[0] = '\0';
        return -1;
    }
    fprintf(in, "%s", input);
    fclose(in);

    char command[512];
    snprintf(command, sizeof(command),
             "./bin/auspex %s < /tmp/repl_in.txt > /tmp/repl_out.txt 2>&1", args);
    int rc = system(command);
    FILE* f = fopen("/tmp/repl_out.txt", "r");
    if (f == NULL) {
        output[0] = '\0';
        return rc;
    }
    size_t n = fread(output, 1, output_size - 1, f);
    output[n] = '\0';
    fclose(f);
    return rc;
}

static int run_repl(const char* input, char* output, size_t output_size) {
    return run_repl_with_args("", input, output, output_size);
}

static int output_contains(const char* output, const char* needle) {
    return strstr(output, needle) != NULL;
}

TEST(repl_defines_and_calls_procedure) {
    char out[4096];
    run_repl("proc double(n int) -> int { return n * 2; }\ndouble(21)\n.exit\n",
             out, sizeof(out));
    ASSERT_INT_EQ(1, output_contains(out, "42"));
}

TEST(repl_persists_variables) {
    char out[4096];
    run_repl("int x = 5;\nx + 1\n.exit\n", out, sizeof(out));
    ASSERT_INT_EQ(1, output_contains(out, "6"));
}

TEST(repl_inspects_variables) {
    char out[4096];
    run_repl("int x = 5;\n.vars\n.exit\n", out, sizeof(out));
    ASSERT_INT_EQ(1, output_contains(out, "x = 5"));
}

TEST(repl_loads_file) {
    FILE* f = fopen("/tmp/repl_load_test.apx", "w");
    ASSERT_PTR_NOT_NULL(f);
    fprintf(f, "proc triple(n int) -> int { return n * 3; }\n");
    fclose(f);

    char out[4096];
    run_repl(".load /tmp/repl_load_test.apx\ntriple(7)\n.exit\n",
             out, sizeof(out));
    ASSERT_INT_EQ(1, output_contains(out, "21"));
}

TEST(repl_lists_tables_and_schema) {
    char out[4096];
    run_repl(".sql CREATE TABLE users (id INT, name STRING)\n"
             ".tables\n"
             ".schema\n"
             ".exit\n",
             out, sizeof(out));
    ASSERT_INT_EQ(1, output_contains(out, "users"));
    ASSERT_INT_EQ(1, output_contains(out, "id int"));
    ASSERT_INT_EQ(1, output_contains(out, "name string"));
}

TEST(repl_executes_sql_select) {
    char out[4096];
    run_repl(".sql CREATE TABLE items (id INT, label STRING)\n"
             ".sql INSERT INTO items VALUES (1, 'a')\n"
             ".sql INSERT INTO items VALUES (2, 'b')\n"
             ".sql SELECT id, label FROM items\n"
             ".exit\n",
             out, sizeof(out));
    ASSERT_INT_EQ(1, output_contains(out, "id = 1"));
    ASSERT_INT_EQ(1, output_contains(out, "label = a"));
    ASSERT_INT_EQ(1, output_contains(out, "id = 2"));
    ASSERT_INT_EQ(1, output_contains(out, "label = b"));
}

TEST(repl_shows_defined_procedures) {
    char out[4096];
    run_repl("proc inc(n int) -> int { return n + 1; }\n.defs\n.exit\n",
             out, sizeof(out));
    ASSERT_INT_EQ(1, output_contains(out, "proc inc"));
}

TEST(repl_reports_single_error_for_invalid_input) {
    char out[4096];
    run_repl("var x = 5;\n.exit\n", out, sizeof(out));
    ASSERT_INT_EQ(1, output_contains(out, "Compile error"));
    int error_count = 0;
    const char* p = out;
    while ((p = strstr(p, ": error:")) != NULL) {
        error_count++;
        p++;
    }
    ASSERT_INT_EQ(1, error_count);
}

TEST(repl_connects_to_database) {
    system("rm -f /tmp/repl_test.db");
    char out[4096];
    run_repl(".connect /tmp/repl_test.db\ncreate table t (id int);\n.tables\n.exit\n", out, sizeof(out));
    ASSERT_INT_EQ(1, output_contains(out, "t"));
    remove("/tmp/repl_test.db");
}

TEST(repl_lists_columns_for_table) {
    char out[4096];
    run_repl(".sql CREATE TABLE users (id INT, name STRING)\n"
             ".columns users\n"
             ".exit\n",
             out, sizeof(out));
    ASSERT_INT_EQ(1, output_contains(out, "users columns:"));
    ASSERT_INT_EQ(1, output_contains(out, "id int"));
    ASSERT_INT_EQ(1, output_contains(out, "name string"));
}

TEST(repl_counts_rows_in_table) {
    char out[4096];
    run_repl(".sql CREATE TABLE items (id INT)\n"
             ".sql INSERT INTO items VALUES (1)\n"
             ".sql INSERT INTO items VALUES (2)\n"
             ".count items\n"
             ".exit\n",
             out, sizeof(out));
    ASSERT_INT_EQ(1, output_contains(out, "items: 2 rows"));
}

TEST(repl_accepts_multiline_block_statement) {
    char out[4096];
    run_repl("int x = 0;\n"
             "if 0 {\n"
             "x = 5;\n"
             "}\n"
             "x\n"
             ".exit\n",
             out, sizeof(out));
    /* The inner assignment must not run because the if condition is false.
       We look for the final "0" after the prompt for "x". */
    ASSERT_INT_EQ(1, output_contains(out, "0"));
    ASSERT_INT_EQ(0, output_contains(out, "5"));
}

TEST(repl_history_lists_previous_input) {
    char out[4096];
    run_repl("int x = 1;\n"
             ".history\n"
             ".exit\n",
             out, sizeof(out));
    ASSERT_INT_EQ(1, output_contains(out, "int x = 1;"));
}

TEST(repl_no_indexes_or_foreign_keys_in_custom_engine) {
    char out[4096];
    run_repl(".sql CREATE TABLE users (id INT, name STRING)\n"
             ".indexes users\n"
             ".foreignkeys users\n"
             ".exit\n",
             out, sizeof(out));
    ASSERT_INT_EQ(1, output_contains(out, "(no indexes)"));
    ASSERT_INT_EQ(1, output_contains(out, "(no foreign keys)"));
}

TEST(repl_runs_each_statement_once) {
    /* Incremental engine (#37): a statement must not re-execute when later
       inputs arrive. Under the old recompile-everything engine the print
       repeated once per subsequent input. */
    char out[4096];
    run_repl("print \"marker\";\n"
             "int x = 1;\n"
             "x + 1\n"
             ".exit\n",
             out, sizeof(out));
    int count = 0;
    const char* p = out;
    while ((p = strstr(p, "marker")) != NULL) {
        count++;
        p++;
    }
    ASSERT_INT_EQ(1, count);
}

TEST(repl_vars_accumulate_without_rerunning) {
    /* Values persist across inputs: each statement runs exactly once on the
       accumulated state (x: 1 -> 2 -> 3). */
    char out[4096];
    run_repl("int x = 1;\n"
             "x = x + 1;\n"
             "x = x + 1;\n"
             ".vars\n"
             ".exit\n",
             out, sizeof(out));
    ASSERT_INT_EQ(1, output_contains(out, "x = 3"));
    ASSERT_INT_EQ(0, output_contains(out, "x = 1"));
}

static int count_occurrences(const char* output, const char* needle) {
    int count = 0;
    for (const char* p = output; (p = strstr(p, needle)) != NULL; p++) count++;
    return count;
}

/* Issue #58: an input that fails to compile is dropped, so it is reported
   once and later inputs run against the session as it was. */
TEST(repl_statement_failure_does_not_poison_session) {
    char out[4096];
    run_repl("int x = 1;\n"
             "var bad = ;\n"
             "x + 1\n"
             ".exit\n",
             out, sizeof(out));
    ASSERT_INT_EQ(1, count_occurrences(out, "Compile error"));
    ASSERT_INT_EQ(1, output_contains(out, "> 2\n"));
}

TEST(repl_failed_declaration_frees_its_name) {
    char out[4096];
    run_repl("int y = \"text\";\n"
             "int y = 5;\n"
             "y + 1\n"
             ".exit\n",
             out, sizeof(out));
    ASSERT_INT_EQ(1, count_occurrences(out, "Compile error"));
    ASSERT_INT_EQ(1, output_contains(out, "> 6\n"));
}

TEST(repl_failed_definition_does_not_poison_session) {
    char out[4096];
    run_repl("proc broken( -> int { return 1; }\n"
             "proc fine() -> int { return 7; }\n"
             "fine()\n"
             ".defs\n"
             ".exit\n",
             out, sizeof(out));
    ASSERT_INT_EQ(1, count_occurrences(out, "Compile error"));
    ASSERT_INT_EQ(1, output_contains(out, "> 7\n"));
    ASSERT_INT_EQ(0, output_contains(out, "broken"));
}

TEST(repl_redefined_proc_replaces_the_old_one) {
    /* Callers compiled earlier, like twice(), reach the new body too. */
    char out[4096];
    run_repl("proc add(a int, b int) -> int { return a + b; }\n"
             "proc twice(a int) -> int { return add(a, a); }\n"
             "twice(5)\n"
             "proc add(a int, b int) -> int { return a * b; }\n"
             "twice(5)\n"
             "add(2, 3)\n"
             ".defs\n"
             ".exit\n",
             out, sizeof(out));
    ASSERT_INT_EQ(0, output_contains(out, "error"));
    const char* first = strstr(out, "> 10\n");
    ASSERT_PTR_NOT_NULL(first);
    const char* second = strstr(first, "> 25\n");
    ASSERT_PTR_NOT_NULL(second);
    ASSERT_INT_EQ(1, output_contains(second, "> 6\n"));
    /* .defs lists add once, with the new body. */
    ASSERT_INT_EQ(1, count_occurrences(out, "proc add"));
    ASSERT_INT_EQ(1, output_contains(out, "return a * b;"));
}

TEST(repl_redefinition_with_new_signature_is_refused) {
    char out[4096];
    run_repl("proc add(a int, b int) -> int { return a + b; }\n"
             "proc add(a int) -> int { return a; }\n"
             "add(2, 3)\n"
             ".exit\n",
             out, sizeof(out));
    ASSERT_INT_EQ(1, output_contains(out,
        "Cannot redefine procedure 'add' with a different signature"));
    ASSERT_INT_EQ(1, output_contains(out, "> 5\n"));
}

TEST(repl_vars_names_follow_their_slots) {
    /* print base; used to be recorded as a second variable named base,
       shifting every later name by one. */
    char out[4096];
    run_repl("int base = 10;\n"
             "print base;\n"
             "int extra = 32;\n"
             "for i in range(0, 2) { print i; }\n"
             "string s = \"hi\";\n"
             ".vars\n"
             ".exit\n",
             out, sizeof(out));
    ASSERT_INT_EQ(1, output_contains(out, "base = 10\nextra = 32\ns = hi\n"));
}

TEST(repl_load_failure_does_not_poison_session) {
    FILE* f = fopen("/tmp/repl_load_bad.apx", "w");
    ASSERT_PTR_NOT_NULL(f);
    fprintf(f, "proc bad(n int) -> int { return missing_name; }\n");
    fclose(f);
    f = fopen("/tmp/repl_load_main.apx", "w");
    ASSERT_PTR_NOT_NULL(f);
    fprintf(f, "proc helper(n int) -> int { return n + 100; }\n"
               "proc main() -> int { print \"file main\"; return 0; }\n");
    fclose(f);

    char out[4096];
    run_repl(".load /tmp/repl_load_bad.apx\n"
             ".load /tmp/repl_load_main.apx\n"
             "helper(1)\n"
             "int after = 3;\n"
             "after + 1\n"
             ".exit\n",
             out, sizeof(out));
    ASSERT_INT_EQ(1, count_occurrences(out, "Compile error"));
    ASSERT_INT_EQ(1, output_contains(out, "file main"));
    ASSERT_INT_EQ(1, output_contains(out, "> 101\n"));
    ASSERT_INT_EQ(1, output_contains(out, "> 4\n"));
}

/* Issue #85: each package input used to APPEND the whole procedures buffer
   to the stored source, so a proc defined before two package inputs was
   stored twice and the next session failed with "Duplicate procedure". */
static const char* PERSIST_SESSION_1 =
    "proc add(a int, b int) -> int { return a + b; }\n"
    "package p1 is\n"
    "    func one() -> int;\n"
    "end p1;\n"
    "package body p1 is\n"
    "    func one() -> int { return 1; }\n"
    "end p1;\n"
    ".exit\n";
static const char* PERSIST_SESSION_2 =
    "add(2, 3)\n"
    "proc add(a int, b int) -> int { return a * b; }\n"
    "package p2 is\n"
    "    func two() -> int;\n"
    "end p2;\n"
    "package body p2 is\n"
    "    func two() -> int { return add(1, 2); }\n"
    "end p2;\n"
    ".exit\n";
static const char* PERSIST_SESSION_3 =
    "add(2, 3)\n"
    "p1.one()\n"
    "p2.two()\n"
    ".exit\n";

static void check_persisted_sessions(const char* args) {
    char out[4096];
    run_repl_with_args(args, PERSIST_SESSION_1, out, sizeof(out));
    ASSERT_INT_EQ(0, output_contains(out, "error"));
    run_repl_with_args(args, PERSIST_SESSION_2, out, sizeof(out));
    ASSERT_INT_EQ(0, output_contains(out, "error"));
    ASSERT_INT_EQ(1, output_contains(out, "> 5\n"));
    run_repl_with_args(args, PERSIST_SESSION_3, out, sizeof(out));
    ASSERT_INT_EQ(0, output_contains(out, "error"));
    ASSERT_INT_EQ(1, output_contains(out, "> 6\n")); /* the redefined add */
    ASSERT_INT_EQ(1, output_contains(out, "> 1\n"));
    ASSERT_INT_EQ(1, output_contains(out, "> 2\n"));
}

TEST(repl_persisted_packages_load_in_later_sessions) {
    remove("auspex.db");
    remove("auspex.db.packages");
    check_persisted_sessions("");
    if (current_test_failed) {
        remove("auspex.db");
        remove("auspex.db.packages");
        return;
    }
    /* The stored source holds each definition once, under one marker. */
    FILE* f = fopen("auspex.db.packages", "r");
    ASSERT_PTR_NOT_NULL(f);
    char stored[4096];
    size_t n = fread(stored, 1, sizeof(stored) - 1, f);
    stored[n] = '\0';
    fclose(f);
    ASSERT_INT_EQ(1, count_occurrences(stored, "proc add"));
    ASSERT_INT_EQ(1, count_occurrences(stored, "return a * b;"));
    ASSERT_INT_EQ(1, count_occurrences(stored, "package body p1"));
    ASSERT_INT_EQ(1, count_occurrences(stored, "__AUSPEX_PACKAGE_SOURCE__"));
    remove("auspex.db");
    remove("auspex.db.packages");
}

#ifdef USE_SQLITE
TEST(repl_persisted_packages_load_in_later_sessions_sqlite) {
    remove("/tmp/repl_persist.db");
    check_persisted_sessions("--db /tmp/repl_persist.db");
    remove("/tmp/repl_persist.db");
}
#endif

#ifdef USE_SQLITE
TEST(repl_lists_indexes_and_foreign_keys_in_sqlite) {
    system("rm -f /tmp/repl_fk.db");
    char out[4096];
    run_repl(".connect /tmp/repl_fk.db\n"
             ".sql create table users (id int primary key, name string)\n"
             ".sql create table orders (id int, user_id int, foreign key(user_id) references users(id))\n"
             ".sql create index idx_user_id on orders(user_id)\n"
             ".indexes orders\n"
             ".foreignkeys orders\n"
             ".exit\n",
             out, sizeof(out));
    ASSERT_INT_EQ(1, output_contains(out, "idx_user_id"));
    ASSERT_INT_EQ(1, output_contains(out, "user_id -> users(id)"));
    remove("/tmp/repl_fk.db");
}
#endif

int main(void) {
    /* Stored package source from an earlier run would be loaded by every
       session, so it goes too. */
    system("rm -f auspex.db auspex.db.packages");
    RUN_TEST(repl_defines_and_calls_procedure);
    RUN_TEST(repl_persists_variables);
    RUN_TEST(repl_inspects_variables);
    RUN_TEST(repl_loads_file);
    RUN_TEST(repl_lists_tables_and_schema);
    RUN_TEST(repl_executes_sql_select);
    RUN_TEST(repl_shows_defined_procedures);
    RUN_TEST(repl_reports_single_error_for_invalid_input);
    RUN_TEST(repl_connects_to_database);
    RUN_TEST(repl_lists_columns_for_table);
    RUN_TEST(repl_counts_rows_in_table);
    RUN_TEST(repl_accepts_multiline_block_statement);
    RUN_TEST(repl_history_lists_previous_input);
    RUN_TEST(repl_no_indexes_or_foreign_keys_in_custom_engine);
    RUN_TEST(repl_runs_each_statement_once);
    RUN_TEST(repl_vars_accumulate_without_rerunning);
    RUN_TEST(repl_statement_failure_does_not_poison_session);
    RUN_TEST(repl_failed_declaration_frees_its_name);
    RUN_TEST(repl_failed_definition_does_not_poison_session);
    RUN_TEST(repl_redefined_proc_replaces_the_old_one);
    RUN_TEST(repl_redefinition_with_new_signature_is_refused);
    RUN_TEST(repl_vars_names_follow_their_slots);
    RUN_TEST(repl_load_failure_does_not_poison_session);
    RUN_TEST(repl_persisted_packages_load_in_later_sessions);
#ifdef USE_SQLITE
    RUN_TEST(repl_lists_indexes_and_foreign_keys_in_sqlite);
    RUN_TEST(repl_persisted_packages_load_in_later_sessions_sqlite);
#endif
    TEST_SUMMARY();
}
