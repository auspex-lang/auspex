#include "test_harness.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "packages.h"

static int run_auspex(const char* source, char* out, size_t out_size) {
    FILE* f = fopen("/tmp/test_packages_src.apx", "w");
    if (f == NULL) return -1;
    fprintf(f, "%s", source);
    fclose(f);

    int rc = system("./bin/auspex /tmp/test_packages_src.apx > /tmp/test_packages_out.txt 2>&1");

    FILE* outf = fopen("/tmp/test_packages_out.txt", "r");
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

static void clean_db(void) {
    remove("auspex.db");
    remove("auspex.db.packages");
    remove("auspex.db.programs");
}

/* --- packages_filter_redefined --- */

TEST(packages_filter_removes_redeclared_spec_and_body) {
    char* builtins = packages_load_builtins();
    ASSERT_PTR_NOT_NULL(builtins);
    char* filtered = packages_filter_redefined(builtins,
        "package dbms_output is\n"
        "    proc put_line(s string) -> int;\n"
        "end dbms_output;\n"
        "proc main() -> int { return 0; }\n");
    ASSERT_PTR_NOT_NULL(filtered);
    ASSERT_PTR_NULL(strstr(filtered, "package dbms_output"));
    ASSERT_PTR_NULL(strstr(filtered, "package body dbms_output"));
    ASSERT_PTR_NOT_NULL(strstr(filtered, "package utl_file is"));
    ASSERT_PTR_NOT_NULL(strstr(filtered, "package body utl_file is"));
    ASSERT_PTR_NOT_NULL(strstr(filtered, "package body dbms_sql is"));
    free(filtered);
    free(builtins);
}

TEST(packages_filter_body_only_declaration_counts) {
    char* builtins = packages_load_builtins();
    ASSERT_PTR_NOT_NULL(builtins);
    char* filtered = packages_filter_redefined(builtins,
        "create or replace package body utl_file is\n"
        "end utl_file;\n");
    ASSERT_PTR_NOT_NULL(filtered);
    ASSERT_PTR_NULL(strstr(filtered, "utl_file is"));
    ASSERT_PTR_NOT_NULL(strstr(filtered, "package dbms_output is"));
    free(filtered);
    free(builtins);
}

TEST(packages_filter_ignores_comments_and_strings) {
    char* builtins = packages_load_builtins();
    ASSERT_PTR_NOT_NULL(builtins);
    char* filtered = packages_filter_redefined(builtins,
        "// package dbms_output is\n"
        "/* package body dbms_sql is */\n"
        "proc main() -> int { print \"package utl_file is\"; return 0; }\n");
    ASSERT_PTR_NOT_NULL(filtered);
    ASSERT_STRING_EQ(builtins, filtered);
    free(filtered);
    free(builtins);
}

TEST(packages_filter_keeps_text_outside_removed_blocks) {
    const char* loaded =
        "// __AUSPEX_PACKAGE_SOURCE__\n"
        "package a is\n    proc f() -> int;\nend a;\n"
        "package body a is\n    proc f() -> int { return 1; }\nend a;\n"
        "package b is\n    proc g() -> int;\nend b;\n";
    char* filtered = packages_filter_redefined(loaded, "package body a is\nend a;\n");
    ASSERT_PTR_NOT_NULL(filtered);
    ASSERT_PTR_NOT_NULL(strstr(filtered, "// __AUSPEX_PACKAGE_SOURCE__"));
    ASSERT_PTR_NULL(strstr(filtered, "package a is"));
    ASSERT_PTR_NULL(strstr(filtered, "package body a is"));
    ASSERT_PTR_NOT_NULL(strstr(filtered, "package b is\n    proc g() -> int;\nend b;"));
    free(filtered);
}

TEST(packages_filter_everything_removed_returns_null) {
    char* filtered = packages_filter_redefined(
        "package a is\nend a;\n",
        "package a is\nend a;\n");
    ASSERT_PTR_NULL(filtered);
    ASSERT_PTR_NULL(packages_filter_redefined(NULL, "package a is\nend a;\n"));
}

/* --- CLI: user packages replace built-in and persisted packages --- */

TEST(packages_user_package_overrides_builtin_dbms_sql) {
    clean_db();
    char out[512];
    int rc = run_auspex(
        "package dbms_sql is\n"
        "    proc execute(sql string) -> int;\n"
        "end dbms_sql;\n"
        "package body dbms_sql is\n"
        "    proc execute(sql string) -> int {\n"
        "        print concat(\"[audit] \", sql);\n"
        "        return 0;\n"
        "    }\n"
        "end dbms_sql;\n"
        "proc main() -> int {\n"
        "    dbms_sql.execute(\"drop table nope\");\n"
        "    dbms_output.enable(100);\n"
        "    dbms_output.put_line(\"builtin ok\");\n"
        "    array<string> lines = dbms_output.get_lines();\n"
        "    print lines[0];\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "[audit] drop table nope"));
    ASSERT_INT_EQ(1, output_contains(out, "builtin ok"));
    clean_db();
}

TEST(packages_rerunning_package_declaration_with_same_db) {
    clean_db();
    const char* source =
        "package greet is\n"
        "    proc hello() -> int;\n"
        "end greet;\n"
        "package body greet is\n"
        "    proc hello() -> int { print \"hello\"; return 0; }\n"
        "end greet;\n"
        "proc main() -> int { greet.hello(); return 0; }\n";
    char out[512];
    ASSERT_INT_EQ(0, run_auspex(source, out, sizeof(out)));
    /* The persisted copy from the first run must not clash with the file. */
    ASSERT_INT_EQ(0, run_auspex(source, out, sizeof(out)));
    ASSERT_INT_EQ(1, output_contains(out, "hello"));
    clean_db();
}

TEST(packages_persisted_override_replaces_builtin_in_later_runs) {
    clean_db();
    char out[512];
    int rc = run_auspex(
        "package dbms_output is\n"
        "    proc put_line(s string) -> int;\n"
        "end dbms_output;\n"
        "package body dbms_output is\n"
        "    proc put_line(s string) -> int { print concat(\"[stored] \", s); return 0; }\n"
        "end dbms_output;\n"
        "proc main() -> int { return 0; }\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);

    /* New program without the declaration: the stored package is used and
       does not collide with the built-in one. */
    rc = run_auspex(
        "proc main() -> int { dbms_output.put_line(\"later\"); return 0; }\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "[stored] later"));
    clean_db();
}

TEST(packages_override_excluded_by_conditional_compilation_keeps_builtin) {
    clean_db();
    char out[512];
    int rc = run_auspex(
        "$if CUSTOM_OUTPUT $then\n"
        "package dbms_output is\n"
        "    proc put_line(s string) -> int;\n"
        "end dbms_output;\n"
        "package body dbms_output is\n"
        "    proc put_line(s string) -> int { print \"custom\"; return 0; }\n"
        "end dbms_output;\n"
        "$end\n"
        "proc main() -> int {\n"
        "    dbms_output.enable(100);\n"
        "    dbms_output.put_line(\"from builtin\");\n"
        "    array<string> lines = dbms_output.get_lines();\n"
        "    print lines[0];\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "from builtin"));
    ASSERT_INT_EQ(0, output_contains(out, "custom"));
    clean_db();
}

int main(void) {
    printf("test_packages:\n");
    RUN_TEST(packages_filter_removes_redeclared_spec_and_body);
    RUN_TEST(packages_filter_body_only_declaration_counts);
    RUN_TEST(packages_filter_ignores_comments_and_strings);
    RUN_TEST(packages_filter_keeps_text_outside_removed_blocks);
    RUN_TEST(packages_filter_everything_removed_returns_null);
    RUN_TEST(packages_user_package_overrides_builtin_dbms_sql);
    RUN_TEST(packages_rerunning_package_declaration_with_same_db);
    RUN_TEST(packages_persisted_override_replaces_builtin_in_later_runs);
    RUN_TEST(packages_override_excluded_by_conditional_compilation_keeps_builtin);
    TEST_SUMMARY();
}
