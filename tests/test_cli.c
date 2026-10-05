#include "test_harness.h"
#include <stdlib.h>
#include <string.h>

TEST(cli_runs_file_and_prints_int_result) {
    int rc = system("./bin/auspex tests/fixtures/add.apx > /tmp/auspex_out.txt 2>&1");
    ASSERT_INT_EQ(0, WEXITSTATUS(rc));
    FILE* f = fopen("/tmp/auspex_out.txt", "r");
    char buf[64];
    fgets(buf, sizeof(buf), f);
    fclose(f);
    ASSERT_INT_EQ(3, atoi(buf));
}

TEST(cli_runs_file_and_prints_float_result) {
    int rc = system("./bin/auspex tests/fixtures/float.apx > /tmp/auspex_out.txt 2>&1");
    ASSERT_INT_EQ(0, WEXITSTATUS(rc));
    FILE* f = fopen("/tmp/auspex_out.txt", "r");
    char buf[64];
    fgets(buf, sizeof(buf), f);
    fclose(f);
    ASSERT_FLOAT_EQ(3.14, atof(buf));
}

TEST(cli_returns_nonzero_on_compile_error) {
    int rc = system("./bin/auspex tests/fixtures/error.apx > /dev/null 2>&1");
    ASSERT_INT_EQ(1, WEXITSTATUS(rc));
}

TEST(cli_resolves_import_relative_to_importing_file) {
    system("rm -rf /tmp/auspex_import_test");
    system("mkdir -p /tmp/auspex_import_test/lib");

    FILE* f = fopen("/tmp/auspex_import_test/lib/helper.apx", "w");
    ASSERT_PTR_NOT_NULL(f);
    fprintf(f, "proc double(n int) -> int { return n * 2; }\n");
    fclose(f);

    f = fopen("/tmp/auspex_import_test/main.apx", "w");
    ASSERT_PTR_NOT_NULL(f);
    fprintf(f, "import \"lib/helper.apx\";\nproc main() -> int { return double(21); }\n");
    fclose(f);

    int rc = system("./bin/auspex /tmp/auspex_import_test/main.apx > /tmp/auspex_out.txt 2>&1");
    ASSERT_INT_EQ(0, WEXITSTATUS(rc));

    FILE* out = fopen("/tmp/auspex_out.txt", "r");
    ASSERT_PTR_NOT_NULL(out);
    char buf[64];
    fgets(buf, sizeof(buf), out);
    fclose(out);
    ASSERT_INT_EQ(42, atoi(buf));

    system("rm -rf /tmp/auspex_import_test");
}

TEST(cli_resolves_nested_import_relative_to_importing_file) {
    system("rm -rf /tmp/auspex_nested_import_test");
    system("mkdir -p /tmp/auspex_nested_import_test/lib");

    FILE* f = fopen("/tmp/auspex_nested_import_test/lib/utils.apx", "w");
    ASSERT_PTR_NOT_NULL(f);
    fprintf(f, "proc triple(n int) -> int { return n * 3; }\n");
    fclose(f);

    f = fopen("/tmp/auspex_nested_import_test/lib/helper.apx", "w");
    ASSERT_PTR_NOT_NULL(f);
    fprintf(f, "import \"utils.apx\";\nproc call_triple(n int) -> int { return triple(n); }\n");
    fclose(f);

    f = fopen("/tmp/auspex_nested_import_test/main.apx", "w");
    ASSERT_PTR_NOT_NULL(f);
    fprintf(f, "import \"lib/helper.apx\";\nproc main() -> int { return call_triple(7); }\n");
    fclose(f);

    int rc = system("./bin/auspex /tmp/auspex_nested_import_test/main.apx > /tmp/auspex_out.txt 2>&1");
    ASSERT_INT_EQ(0, WEXITSTATUS(rc));

    FILE* out = fopen("/tmp/auspex_out.txt", "r");
    ASSERT_PTR_NOT_NULL(out);
    char buf[64];
    fgets(buf, sizeof(buf), out);
    fclose(out);
    ASSERT_INT_EQ(21, atoi(buf));

    system("rm -rf /tmp/auspex_nested_import_test");
}

TEST(cli_initializes_package_declared_in_imported_module) {
    system("rm -rf /tmp/auspex_import_pkginit_test");
    system("mkdir -p /tmp/auspex_import_pkginit_test/lib");

    FILE* f = fopen("/tmp/auspex_import_pkginit_test/lib/counter.apx", "w");
    ASSERT_PTR_NOT_NULL(f);
    fprintf(f,
        "package counter is\n"
        "    counter int;\n"
        "    func get() -> int;\n"
        "end counter;\n"
        "\n"
        "package body counter is\n"
        "    int counter = 41;\n"
        "    func get() -> int {\n"
        "        return counter;\n"
        "    }\n"
        "end counter;\n");
    fclose(f);

    f = fopen("/tmp/auspex_import_pkginit_test/main.apx", "w");
    ASSERT_PTR_NOT_NULL(f);
    fprintf(f, "import \"lib/counter.apx\";\nproc main() -> int { return counter.get(); }\n");
    fclose(f);

    /* Package-level state declared in the imported module must run its
       initializer before main() executes, exactly as if it were declared
       in the main file itself. Without that, `counter` starts at the
       int zero-value instead of 41. */
    int rc = system("./bin/auspex /tmp/auspex_import_pkginit_test/main.apx > /tmp/auspex_out.txt 2>&1");
    ASSERT_INT_EQ(0, WEXITSTATUS(rc));

    FILE* out = fopen("/tmp/auspex_out.txt", "r");
    ASSERT_PTR_NOT_NULL(out);
    char buf[64];
    fgets(buf, sizeof(buf), out);
    fclose(out);
    ASSERT_INT_EQ(41, atoi(buf));

    system("rm -rf /tmp/auspex_import_pkginit_test");
}

TEST(cli_initializes_packages_from_multiple_imported_modules_in_order) {
    system("rm -rf /tmp/auspex_import_pkginit_multi_test");
    system("mkdir -p /tmp/auspex_import_pkginit_multi_test/lib");

    FILE* f = fopen("/tmp/auspex_import_pkginit_multi_test/lib/a.apx", "w");
    ASSERT_PTR_NOT_NULL(f);
    fprintf(f,
        "package pkg_a is\n"
        "    func get() -> int;\n"
        "end pkg_a;\n"
        "\n"
        "package body pkg_a is\n"
        "    int value = 10;\n"
        "    func get() -> int {\n"
        "        return value;\n"
        "    }\n"
        "end pkg_a;\n");
    fclose(f);

    f = fopen("/tmp/auspex_import_pkginit_multi_test/lib/b.apx", "w");
    ASSERT_PTR_NOT_NULL(f);
    fprintf(f,
        "package pkg_b is\n"
        "    func get() -> int;\n"
        "end pkg_b;\n"
        "\n"
        "package body pkg_b is\n"
        "    int value = 20;\n"
        "    func get() -> int {\n"
        "        return value;\n"
        "    }\n"
        "end pkg_b;\n");
    fclose(f);

    f = fopen("/tmp/auspex_import_pkginit_multi_test/main.apx", "w");
    ASSERT_PTR_NOT_NULL(f);
    fprintf(f,
        "import \"lib/a.apx\";\n"
        "import \"lib/b.apx\";\n"
        "proc main() -> int { return pkg_a.get() + pkg_b.get(); }\n");
    fclose(f);

    int rc = system("./bin/auspex /tmp/auspex_import_pkginit_multi_test/main.apx > /tmp/auspex_out.txt 2>&1");
    ASSERT_INT_EQ(0, WEXITSTATUS(rc));

    FILE* out = fopen("/tmp/auspex_out.txt", "r");
    ASSERT_PTR_NOT_NULL(out);
    char buf[64];
    fgets(buf, sizeof(buf), out);
    fclose(out);
    ASSERT_INT_EQ(30, atoi(buf));

    system("rm -rf /tmp/auspex_import_pkginit_multi_test");
}

TEST(cli_accepts_conditional_flags_in_any_order) {
    FILE* f = fopen("/tmp/cli_cc.apx", "w");
    ASSERT_PTR_NOT_NULL(f);
    fprintf(f,
            "$if DEBUG $then\n"
            "func debug_value() -> int { return 40; }\n"
            "$else\n"
            "func debug_value() -> int { return 0; }\n"
            "$end\n"
            "proc main() -> int {\n"
            "$if TRACE $then\n"
            "    return debug_value() + 2;\n"
            "$else\n"
            "    return 0;\n"
            "$end\n"
            "}\n");
    fclose(f);

    int rc = system("./bin/auspex -DDEBUG /tmp/cli_cc.apx -DTRACE -DDEBUG > /tmp/auspex_out.txt 2>&1");
    ASSERT_INT_EQ(0, WEXITSTATUS(rc));
    FILE* out = fopen("/tmp/auspex_out.txt", "r");
    ASSERT_PTR_NOT_NULL(out);
    char buf[64];
    ASSERT_PTR_NOT_NULL(fgets(buf, sizeof(buf), out));
    fclose(out);
    ASSERT_INT_EQ(42, atoi(buf));
    remove("/tmp/cli_cc.apx");
}

TEST(cli_conditional_flags_apply_to_imports) {
    system("rm -rf /tmp/auspex_cc_import_test");
    system("mkdir -p /tmp/auspex_cc_import_test");

    FILE* f = fopen("/tmp/auspex_cc_import_test/helper.apx", "w");
    ASSERT_PTR_NOT_NULL(f);
    fprintf(f,
            "$if FEATURE $then\n"
            "proc feature_value() -> int { return 42; }\n"
            "$else\n"
            "proc feature_value() -> int { return 0; }\n"
            "$end\n");
    fclose(f);

    f = fopen("/tmp/auspex_cc_import_test/main.apx", "w");
    ASSERT_PTR_NOT_NULL(f);
    fprintf(f,
            "import \"helper.apx\";\n"
            "proc main() -> int { return feature_value(); }\n");
    fclose(f);

    int rc = system("./bin/auspex -DFEATURE /tmp/auspex_cc_import_test/main.apx > /tmp/auspex_out.txt 2>&1");
    ASSERT_INT_EQ(0, WEXITSTATUS(rc));
    FILE* out = fopen("/tmp/auspex_out.txt", "r");
    ASSERT_PTR_NOT_NULL(out);
    char buf[64];
    ASSERT_PTR_NOT_NULL(fgets(buf, sizeof(buf), out));
    fclose(out);
    ASSERT_INT_EQ(42, atoi(buf));
    system("rm -rf /tmp/auspex_cc_import_test");
}

TEST(cli_rejects_invalid_conditional_flag) {
    int rc = system("./bin/auspex -DDEBUG=1 tests/fixtures/add.apx > /tmp/auspex_out.txt 2>&1");
    ASSERT_INT_EQ(1, WEXITSTATUS(rc));
    FILE* out = fopen("/tmp/auspex_out.txt", "r");
    ASSERT_PTR_NOT_NULL(out);
    char buf[256] = {0};
    ASSERT_PTR_NOT_NULL(fgets(buf, sizeof(buf), out));
    fclose(out);
    ASSERT_PTR_NOT_NULL(strstr(buf, "Invalid conditional-compilation flag"));
}

TEST(cli_rejects_conditional_flag_without_file) {
    int rc = system("./bin/auspex -DDEBUG > /tmp/auspex_out.txt 2>&1");
    ASSERT_INT_EQ(1, WEXITSTATUS(rc));
}

/* Issue #52: the CLI prepends built-in (and stored) declarations to the
 * user's file before compiling it. The lines it reports must still be the
 * user's own, and a statement spanning several lines is located where it
 * starts. */

static int write_text_file(const char* path, const char* text) {
    FILE* f = fopen(path, "w");
    if (f == NULL) return 0;
    fputs(text, f);
    fclose(f);
    return 1;
}

static int run_cli_capture(const char* path, char* out, size_t out_size) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "./bin/auspex %s > /tmp/auspex_out.txt 2>&1", path);
    int rc = system(cmd);
    out[0] = '\0';
    FILE* f = fopen("/tmp/auspex_out.txt", "r");
    if (f != NULL) {
        size_t n = fread(out, 1, out_size - 1, f);
        out[n] = '\0';
        fclose(f);
    }
    return WEXITSTATUS(rc);
}

TEST(cli_compile_error_reports_the_line_in_the_users_file) {
    char out[512];
    ASSERT_INT_EQ(1, write_text_file("/tmp/auspex_cli_line_a.apx",
        "proc main() -> int {\n"
        "    int x = 1;\n"
        "    string s = x;\n"
        "    return 0;\n"
        "}\n"));
    ASSERT_INT_EQ(1, run_cli_capture("/tmp/auspex_cli_line_a.apx", out, sizeof(out)));
    ASSERT_PTR_NOT_NULL(strstr(out, "/tmp/auspex_cli_line_a.apx:3:"));
    remove("/tmp/auspex_cli_line_a.apx");
}

TEST(cli_runtime_sql_error_reports_the_line_in_the_users_file) {
    char out[512];
    remove("auspex.db");
    ASSERT_INT_EQ(1, write_text_file("/tmp/auspex_cli_line_b.apx",
        "proc main() -> int {\n"
        "    print \"start\";\n"
        "    insert into cli_line_missing values (1);\n"
        "    return 0;\n"
        "}\n"));
    ASSERT_INT_EQ(1, run_cli_capture("/tmp/auspex_cli_line_b.apx", out, sizeof(out)));
    ASSERT_PTR_NOT_NULL(strstr(out, "/tmp/auspex_cli_line_b.apx:3:"));
    remove("/tmp/auspex_cli_line_b.apx");
    remove("auspex.db");
}

TEST(cli_multiline_select_error_reports_the_statement_start) {
    char out[512];
    remove("auspex.db");
    ASSERT_INT_EQ(1, write_text_file("/tmp/auspex_cli_line_c.apx",
        "proc main() -> int {\n"
        "    create table cli_line_t (id int);\n"
        "    int n = 0;\n"
        "    select id into n\n"
        "        from cli_line_t\n"
        "        where id = 99;\n"
        "    return n;\n"
        "}\n"));
    ASSERT_INT_EQ(1, run_cli_capture("/tmp/auspex_cli_line_c.apx", out, sizeof(out)));
    ASSERT_PTR_NOT_NULL(strstr(out, "/tmp/auspex_cli_line_c.apx:4:"));
    remove("/tmp/auspex_cli_line_c.apx");
    remove("auspex.db");
}

TEST(cli_error_in_stored_proc_is_reported_at_the_call_site) {
    char out[512];
    remove("auspex.db");
    remove("auspex.db.programs");
    /* The first run stores boom(); the second calls it, so its source is part
       of what the CLI prepends. */
    ASSERT_INT_EQ(1, write_text_file("/tmp/auspex_cli_line_d1.apx",
        "proc boom() -> int {\n"
        "    int x = parse_int(\"zzz\");\n"
        "    return x;\n"
        "}\n"
        "proc main() -> int { return 0; }\n"));
    ASSERT_INT_EQ(0, run_cli_capture("/tmp/auspex_cli_line_d1.apx", out, sizeof(out)));

    ASSERT_INT_EQ(1, write_text_file("/tmp/auspex_cli_line_d2.apx",
        "proc main() -> int {\n"
        "    print \"calling\";\n"
        "    return boom();\n"
        "}\n"));
    ASSERT_INT_EQ(1, run_cli_capture("/tmp/auspex_cli_line_d2.apx", out, sizeof(out)));
    ASSERT_PTR_NOT_NULL(strstr(out, "/tmp/auspex_cli_line_d2.apx:3:"));
    ASSERT_PTR_NOT_NULL(strstr(out, "parse_int: invalid integer"));
    remove("/tmp/auspex_cli_line_d1.apx");
    remove("/tmp/auspex_cli_line_d2.apx");
    remove("auspex.db");
    remove("auspex.db.programs");
}

#ifdef USE_SQLITE
TEST(cli_accepts_db_flag) {
    remove("/tmp/cli_test.db");
    FILE* f = fopen("/tmp/cli_db.apx", "w");
    ASSERT_PTR_NOT_NULL(f);
    fprintf(f,
            "proc main() -> int {\n"
            "$if DB_TEST $then\n"
            "    create table t (id int);\n"
            "$end\n"
            "    return 0;\n"
            "}\n");
    fclose(f);
    int rc = system("./bin/auspex /tmp/cli_db.apx --db /tmp/cli_test.db -DDB_TEST > /tmp/cli_db_out.txt 2>&1");
    ASSERT_INT_EQ(0, WEXITSTATUS(rc));
    remove("/tmp/cli_db.apx");
    remove("/tmp/cli_test.db");
}
#endif

int main(void) {
    RUN_TEST(cli_runs_file_and_prints_int_result);
    RUN_TEST(cli_runs_file_and_prints_float_result);
    RUN_TEST(cli_returns_nonzero_on_compile_error);
    RUN_TEST(cli_resolves_import_relative_to_importing_file);
    RUN_TEST(cli_resolves_nested_import_relative_to_importing_file);
    RUN_TEST(cli_initializes_package_declared_in_imported_module);
    RUN_TEST(cli_initializes_packages_from_multiple_imported_modules_in_order);
    RUN_TEST(cli_accepts_conditional_flags_in_any_order);
    RUN_TEST(cli_conditional_flags_apply_to_imports);
    RUN_TEST(cli_rejects_invalid_conditional_flag);
    RUN_TEST(cli_rejects_conditional_flag_without_file);
    RUN_TEST(cli_compile_error_reports_the_line_in_the_users_file);
    RUN_TEST(cli_runtime_sql_error_reports_the_line_in_the_users_file);
    RUN_TEST(cli_multiline_select_error_reports_the_statement_start);
    RUN_TEST(cli_error_in_stored_proc_is_reported_at_the_call_site);
#ifdef USE_SQLITE
    RUN_TEST(cli_accepts_db_flag);
#endif
    TEST_SUMMARY();
}
