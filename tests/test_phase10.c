#include "test_harness.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int run_auspex(const char* source, char* out, size_t out_size) {
    FILE* f = fopen("/tmp/test_phase10_src.apx", "w");
    if (f == NULL) return -1;
    fprintf(f, "%s", source);
    fclose(f);

    int rc = system("./bin/auspex /tmp/test_phase10_src.apx > /tmp/test_phase10_out.txt 2>&1");

    FILE* outf = fopen("/tmp/test_phase10_out.txt", "r");
    if (outf != NULL) {
        out[0] = '\0';
        size_t n = fread(out, 1, out_size - 1, outf);
        out[n] = '\0';
        fclose(outf);
    }
    return WEXITSTATUS(rc);
}

static int output_contains(const char* out, const char* substr) {
    return strstr(out, substr) != NULL;
}

TEST(phase10_trigger_before_after_insert) {
    remove("auspex.db");
    char out[512];
    int rc = run_auspex(
        "trigger t_log after insert on t {\n"
        "    dbms_output.put_line(\"after\");\n"
        "}\n"
        "trigger t_pre before insert on t {\n"
        "    dbms_output.put_line(\"before\");\n"
        "}\n"
        "proc main() -> int {\n"
        "    dbms_output.enable(10);\n"
        "    create table t (id int);\n"
        "    insert into t values (1);\n"
        "    array<string> lines = dbms_output.get_lines();\n"
        "    for line in lines {\n"
        "        print line;\n"
        "    }\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "before"));
    ASSERT_INT_EQ(1, output_contains(out, "after"));
    /* before must fire before after */
    const char* b = strstr(out, "before");
    const char* a = strstr(out, "after");
    ASSERT(b != NULL && a != NULL && b < a);
}

TEST(phase10_trigger_does_not_fire_on_other_table) {
    remove("auspex.db");
    char out[512];
    int rc = run_auspex(
        "trigger t_wrong after insert on other_table {\n"
        "    dbms_output.put_line(\"wrong\");\n"
        "}\n"
        "proc main() -> int {\n"
        "    dbms_output.enable(10);\n"
        "    create table t (id int);\n"
        "    insert into t values (1);\n"
        "    array<string> lines = dbms_output.get_lines();\n"
        "    print int_to_string(length(lines));\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(0, output_contains(out, "wrong"));
    ASSERT_INT_EQ(1, output_contains(out, "0"));
}

TEST(phase10_trigger_ddl_create_table) {
    remove("auspex.db");
    char out[512];
    int rc = run_auspex(
        "trigger t_ddl after create on t2 {\n"
        "    dbms_output.put_line(\"created\");\n"
        "}\n"
        "proc main() -> int {\n"
        "    dbms_output.enable(10);\n"
        "    create table t2 (id int);\n"
        "    array<string> lines = dbms_output.get_lines();\n"
        "    for line in lines {\n"
        "        print line;\n"
        "    }\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "created"));
}

TEST(phase10_trigger_update_and_delete) {
    remove("auspex.db");
    char out[512];
    int rc = run_auspex(
        "trigger t_upd after update on t {\n"
        "    dbms_output.put_line(\"updated\");\n"
        "}\n"
        "trigger t_del after delete on t {\n"
        "    dbms_output.put_line(\"deleted\");\n"
        "}\n"
        "proc main() -> int {\n"
        "    dbms_output.enable(10);\n"
        "    create table t (id int);\n"
        "    insert into t values (1);\n"
        "    update t set id = 2 where id = 1;\n"
        "    delete from t where id = 2;\n"
        "    array<string> lines = dbms_output.get_lines();\n"
        "    for line in lines {\n"
        "        print line;\n"
        "    }\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "updated"));
    ASSERT_INT_EQ(1, output_contains(out, "deleted"));
}

TEST(phase10_table_function_scalar_collection) {
    char out[256];
    int rc = run_auspex(
        "func numbers() -> array<int> {\n"
        "    return range(1, 5);\n"
        "}\n"
        "proc main() -> int {\n"
        "    int total = 0;\n"
        "    for n in numbers() {\n"
        "        total = total + n;\n"
        "    }\n"
        "    print int_to_string(total);\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "10"));
}

TEST(phase10_table_function_row_collection) {
    remove("auspex.db");
    char out[256];
    int rc = run_auspex(
        "func all_accounts() -> array<row> {\n"
        "    array<row> rows;\n"
        "    select * into rows from accounts;\n"
        "    return rows;\n"
        "}\n"
        "proc main() -> int {\n"
        "    create table accounts (id int, name string);\n"
        "    insert into accounts values (1, 'alice');\n"
        "    insert into accounts values (2, 'bob');\n"
        "    print int_to_string(length(all_accounts()));\n"
        "    for r in all_accounts() {\n"
        "        print r.name;\n"
        "    }\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "2"));
    ASSERT_INT_EQ(1, output_contains(out, "alice"));
    ASSERT_INT_EQ(1, output_contains(out, "bob"));
}

TEST(phase10_struct_method_mutates_field) {
    char out[256];
    int rc = run_auspex(
        "struct Counter {\n"
        "    value int,\n"
        "    proc inc() {\n"
        "        value = value + 1;\n"
        "    },\n"
        "    func get() -> int {\n"
        "        return value;\n"
        "    }\n"
        "}\n"
        "proc main() -> int {\n"
        "    Counter c = Counter { value = 0 };\n"
        "    c.inc();\n"
        "    c.inc();\n"
        "    print int_to_string(c.get());\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "2"));
}

TEST(phase10_struct_func_method_returns_value) {
    char out[256];
    int rc = run_auspex(
        "struct Point {\n"
        "    x int,\n"
        "    y int,\n"
        "    func sum() -> int {\n"
        "        return x + y;\n"
        "    }\n"
        "}\n"
        "proc main() -> int {\n"
        "    Point p = Point { x = 3, y = 4 };\n"
        "    print int_to_string(p.sum());\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "7"));
}

TEST(phase10_struct_method_with_params) {
    char out[256];
    int rc = run_auspex(
        "struct Acc {\n"
        "    balance int,\n"
        "    proc deposit(n int) {\n"
        "        balance = balance + n;\n"
        "    },\n"
        "    func total() -> int {\n"
        "        return balance;\n"
        "    }\n"
        "}\n"
        "proc main() -> int {\n"
        "    Acc a = Acc { balance = 10 };\n"
        "    a.deposit(5);\n"
        "    a.deposit(25);\n"
        "    print int_to_string(a.total());\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "40"));
}

TEST(phase10_cc_if_defined_takes_then_branch) {
    char out[256];
    int rc = run_auspex(
        "$define DEBUG\n"
        "proc main() -> int {\n"
        "$if DEBUG $then\n"
        "    print \"debug on\";\n"
        "$else\n"
        "    print \"debug off\";\n"
        "$end\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "debug on"));
    ASSERT_INT_EQ(0, output_contains(out, "debug off"));
}

TEST(phase10_cc_if_undefined_takes_else_branch) {
    char out[256];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "$if DEBUG $then\n"
        "    print \"debug on\";\n"
        "$else\n"
        "    print \"debug off\";\n"
        "$end\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "debug off"));
    ASSERT_INT_EQ(0, output_contains(out, "debug on"));
}

TEST(phase10_cc_excluded_code_is_not_parsed) {
    char out[256];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "$if DISABLED $then\n"
        "    this is not valid auspex syntax at all +++ ;;;\n"
        "$end\n"
        "    print \"ok\";\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "ok"));
}

TEST(phase10_cc_elsif_chain) {
    char out[256];
    int rc = run_auspex(
        "$define B\n"
        "proc main() -> int {\n"
        "$if A $then\n"
        "    print \"a\";\n"
        "$elsif B $then\n"
        "    print \"b\";\n"
        "$else\n"
        "    print \"c\";\n"
        "$end\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "b"));
    ASSERT_INT_EQ(0, output_contains(out, "a"));
    ASSERT_INT_EQ(0, output_contains(out, "c"));
}

TEST(phase10_cc_unterminated_if_is_error) {
    char out[256];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "$if DEBUG $then\n"
        "    print \"x\";\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(1, rc);
}

static int build_test_shared_lib(void) {
    FILE* f = fopen("/tmp/test_phase10_ext.c", "w");
    if (f == NULL) return 0;
    fprintf(f, "int auspex_double(int x) { return x * 2; }\n");
    fclose(f);
    int rc = system("cc -shared -fPIC -o /tmp/test_phase10_ext.so /tmp/test_phase10_ext.c");
    return rc == 0;
}

TEST(phase10_external_call_shared_library) {
    ASSERT_INT_EQ(1, build_test_shared_lib());
    char out[256];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    int r = external_call(\"/tmp/test_phase10_ext.so\", \"auspex_double\", 21);\n"
        "    print int_to_string(r);\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "42"));
}

TEST(phase10_external_call_missing_library_fails) {
    char out[256];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    external_call(\"/tmp/nonexistent_lib_xyz.so\", \"foo\", 1);\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(1, rc);
}

TEST(phase10_external_call_missing_symbol_fails) {
    ASSERT_INT_EQ(1, build_test_shared_lib());
    char out[256];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    external_call(\"/tmp/test_phase10_ext.so\", \"no_such_symbol\", 1);\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(1, rc);
}

int main(void) {
    RUN_TEST(phase10_trigger_before_after_insert);
    RUN_TEST(phase10_trigger_does_not_fire_on_other_table);
    RUN_TEST(phase10_trigger_ddl_create_table);
    RUN_TEST(phase10_trigger_update_and_delete);
    RUN_TEST(phase10_table_function_scalar_collection);
    RUN_TEST(phase10_table_function_row_collection);
    RUN_TEST(phase10_struct_method_mutates_field);
    RUN_TEST(phase10_struct_func_method_returns_value);
    RUN_TEST(phase10_struct_method_with_params);
    RUN_TEST(phase10_cc_if_defined_takes_then_branch);
    RUN_TEST(phase10_cc_if_undefined_takes_else_branch);
    RUN_TEST(phase10_cc_excluded_code_is_not_parsed);
    RUN_TEST(phase10_cc_elsif_chain);
    RUN_TEST(phase10_cc_unterminated_if_is_error);
    RUN_TEST(phase10_external_call_shared_library);
    RUN_TEST(phase10_external_call_missing_library_fails);
    RUN_TEST(phase10_external_call_missing_symbol_fails);
    TEST_SUMMARY();
}
