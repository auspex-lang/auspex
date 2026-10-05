#include "test_harness.h"
#include <stdlib.h>
#include <string.h>

static int run_auspex(const char* source, const char* args, char* out, size_t out_size) {
    FILE* f = fopen("/tmp/test_phase1_src.apx", "w");
    if (f == NULL) return -1;
    fprintf(f, "%s", source);
    fclose(f);

    char cmd[1024];
    if (args != NULL) {
        snprintf(cmd, sizeof(cmd), "./bin/auspex /tmp/test_phase1_src.apx %s > /tmp/test_phase1_out.txt 2>&1", args);
    } else {
        snprintf(cmd, sizeof(cmd), "./bin/auspex /tmp/test_phase1_src.apx > /tmp/test_phase1_out.txt 2>&1");
    }
    int rc = system(cmd);

    FILE* outf = fopen("/tmp/test_phase1_out.txt", "r");
    if (outf != NULL) {
        out[0] = '\0';
        size_t n = fread(out, 1, out_size - 1, outf);
        out[n] = '\0';
        fclose(outf);
    }
    return WEXITSTATUS(rc);
}

TEST(phase1_exception_catch_variable_holds_message) {
    char out[256];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    try {\n"
        "        assert(false, \"boom\");\n"
        "    } catch (err) {\n"
        "        print err;\n"
        "    }\n"
        "    return 42;\n"
        "}\n",
        NULL, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "boom") != NULL);
    ASSERT_INT_EQ(1, strstr(out, "42") != NULL);
}

TEST(phase1_exception_try_block_without_error_runs_normally) {
    char out[256];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    try {\n"
        "        int x = 7;\n"
        "    } catch (err) {\n"
        "        print err;\n"
        "    }\n"
        "    return 99;\n"
        "}\n",
        NULL, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "99") != NULL);
}

TEST(phase1_exception_preserves_locals_outside_try) {
    char out[256];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    int before = 5;\n"
        "    try {\n"
        "        assert(false, \"oops\");\n"
        "    } catch (err) {\n"
        "        print int_to_string(before);\n"
        "    }\n"
        "    return before;\n"
        "}\n",
        NULL, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "5") != NULL);
}

TEST(phase1_sql_rowcount_after_insert) {
    char out[256];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    create table t (id int);\n"
        "    insert into t values (1), (2), (3);\n"
        "    print int_to_string(sql_rowcount());\n"
        "    return 0;\n"
        "}\n",
        "--db :memory:", out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "3") != NULL);
}

TEST(phase1_sql_found_and_notfound) {
    char out[256];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    create table t (id int);\n"
        "    insert into t values (1);\n"
        "    int found_id = 0;\n"
        "    select id into found_id from t where id = 1;\n"
        "    print sql_found();\n"
        "    print sql_notfound();\n"
        "    return 0;\n"
        "}\n",
        "--db :memory:", out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "true") != NULL);
    ASSERT_INT_EQ(1, strstr(out, "false") != NULL);
}

TEST(phase1_dynamic_sql_execute_immediate) {
    char out[256];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    execute_immediate(\"create table dyn (id int)\");\n"
        "    int n = execute_immediate(\"insert into dyn values (10), (20)\");\n"
        "    print int_to_string(n);\n"
        "    print int_to_string(sql_rowcount());\n"
        "    return 0;\n"
        "}\n",
        "--db :memory:", out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "2") != NULL);
}

TEST(phase1_dynamic_sql_and_exception_together) {
    char out[256];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    try {\n"
        "        execute_immediate(\"insert into missing values (1)\");\n"
        "    } catch (err) {\n"
        "        print err;\n"
        "    }\n"
        "    return 0;\n"
        "}\n",
        "--db :memory:", out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "error") != NULL || strstr(out, "Error") != NULL);
}

int main(void) {
    RUN_TEST(phase1_exception_catch_variable_holds_message);
    RUN_TEST(phase1_exception_try_block_without_error_runs_normally);
    RUN_TEST(phase1_exception_preserves_locals_outside_try);
    RUN_TEST(phase1_sql_rowcount_after_insert);
    RUN_TEST(phase1_sql_found_and_notfound);
    RUN_TEST(phase1_dynamic_sql_execute_immediate);
    RUN_TEST(phase1_dynamic_sql_and_exception_together);
    TEST_SUMMARY();
}
