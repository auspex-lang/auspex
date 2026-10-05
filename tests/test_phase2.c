#include "test_harness.h"
#include <stdlib.h>
#include <string.h>

static int run_auspex(const char* source, const char* args, char* out, size_t out_size) {
    FILE* f = fopen("/tmp/test_phase2_src.apx", "w");
    if (f == NULL) return -1;
    fprintf(f, "%s", source);
    fclose(f);

    char cmd[1024];
    if (args != NULL) {
        snprintf(cmd, sizeof(cmd), "./bin/auspex /tmp/test_phase2_src.apx %s > /tmp/test_phase2_out.txt 2>&1", args);
    } else {
        snprintf(cmd, sizeof(cmd), "./bin/auspex /tmp/test_phase2_src.apx > /tmp/test_phase2_out.txt 2>&1");
    }
    int rc = system(cmd);

    FILE* outf = fopen("/tmp/test_phase2_out.txt", "r");
    if (outf != NULL) {
        out[0] = '\0';
        size_t n = fread(out, 1, out_size - 1, outf);
        out[n] = '\0';
        fclose(outf);
    }
    return WEXITSTATUS(rc);
}

TEST(phase2_case_statement_selects_branch) {
    char out[256];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    int x = 2;\n"
        "    case x {\n"
        "        when 1: { print \"one\"; }\n"
        "        when 2: { print \"two\"; }\n"
        "        else: { print \"other\"; }\n"
        "    }\n"
        "    return 0;\n"
        "}\n",
        NULL, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "two") != NULL);
}

TEST(phase2_case_statement_uses_else) {
    char out[256];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    int x = 99;\n"
        "    case x {\n"
        "        when 1: { print \"one\"; }\n"
        "        when 2: { print \"two\"; }\n"
        "        else: { print \"other\"; }\n"
        "    }\n"
        "    return 0;\n"
        "}\n",
        NULL, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "other") != NULL);
}

TEST(phase2_case_statement_works_with_strings) {
    char out[256];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    string s = \"b\";\n"
        "    case s {\n"
        "        when \"a\": { print \"alpha\"; }\n"
        "        when \"b\": { print \"beta\"; }\n"
        "        else: { print \"unknown\"; }\n"
        "    }\n"
        "    return 0;\n"
        "}\n",
        NULL, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "beta") != NULL);
}

TEST(phase2_case_statement_rejects_mismatched_value_type) {
    char out[256];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    int x = 1;\n"
        "    case x {\n"
        "        when \"one\": { print \"one\"; }\n"
        "        else: { print \"other\"; }\n"
        "    }\n"
        "    return 0;\n"
        "}\n",
        NULL, out, sizeof(out));
    ASSERT_INT_EQ(1, rc);
}

TEST(phase2_anonymous_block_runs_top_level_statements) {
    char out[256];
    int rc = run_auspex(
        "block {\n"
        "    int x = 42;\n"
        "    print int_to_string(x);\n"
        "}\n",
        NULL, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "42") != NULL);
}

TEST(phase2_func_is_callable_from_expression) {
    char out[256];
    int rc = run_auspex(
        "func add(a int, b int) -> int { return a + b; }\n"
        "proc main() -> int { return add(1, 2); }\n",
        NULL, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "3") != NULL);
}

TEST(phase2_func_returns_string) {
    char out[256];
    int rc = run_auspex(
        "func greet() -> string { return \"hello\"; }\n"
        "proc main() -> int { print greet(); return 0; }\n",
        NULL, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "hello") != NULL);
}

TEST(phase2_func_returns_float) {
    char out[256];
    int rc = run_auspex(
        "func half(x float) -> float { return x / 2.0; }\n"
        "proc main() -> int { print float_to_string(half(4.0)); return 0; }\n",
        NULL, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "2") != NULL);
}

TEST(phase2_func_used_in_expression) {
    char out[256];
    int rc = run_auspex(
        "func square(n int) -> int { return n * n; }\n"
        "proc main() -> int { print int_to_string(square(3) + square(4)); return 0; }\n",
        NULL, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "25") != NULL);
}

TEST(phase2_out_param_returns_value) {
    char out[256];
    int rc = run_auspex(
        "proc get_answer(x out int) -> int { x = 42; return 0; }\n"
        "proc main() -> int {\n"
        "    int n = 0;\n"
        "    get_answer(n);\n"
        "    print int_to_string(n);\n"
        "    return 0;\n"
        "}\n",
        NULL, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "42") != NULL);
}

TEST(phase2_inout_param_doubles_value) {
    char out[256];
    int rc = run_auspex(
        "proc double_it(x in out int) -> int { x = x * 2; return 0; }\n"
        "proc main() -> int {\n"
        "    int n = 21;\n"
        "    double_it(n);\n"
        "    print int_to_string(n);\n"
        "    return 0;\n"
        "}\n",
        NULL, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "42") != NULL);
}

TEST(phase2_mixed_param_modes) {
    char out[256];
    int rc = run_auspex(
        "proc combine(a int, b in out int, c out int) -> int { b = b + a; c = b * 2; return 0; }\n"
        "proc main() -> int {\n"
        "    int x = 10;\n"
        "    int y = 0;\n"
        "    combine(5, x, y);\n"
        "    print int_to_string(x);\n"
        "    print int_to_string(y);\n"
        "    return 0;\n"
        "}\n",
        NULL, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "15") != NULL);
    ASSERT_INT_EQ(1, strstr(out, "30") != NULL);
}

TEST(phase2_out_param_rejects_literal_argument) {
    char out[256];
    int rc = run_auspex(
        "proc set_it(x out int) -> int { x = 1; return 0; }\n"
        "proc main() -> int {\n"
        "    set_it(42);\n"
        "    return 0;\n"
        "}\n",
        NULL, out, sizeof(out));
    ASSERT_INT_EQ(1, rc);
}

int main(void) {
    RUN_TEST(phase2_case_statement_selects_branch);
    RUN_TEST(phase2_case_statement_uses_else);
    RUN_TEST(phase2_case_statement_works_with_strings);
    RUN_TEST(phase2_case_statement_rejects_mismatched_value_type);
    RUN_TEST(phase2_anonymous_block_runs_top_level_statements);
    RUN_TEST(phase2_func_is_callable_from_expression);
    RUN_TEST(phase2_func_returns_string);
    RUN_TEST(phase2_func_returns_float);
    RUN_TEST(phase2_func_used_in_expression);
    RUN_TEST(phase2_out_param_returns_value);
    RUN_TEST(phase2_inout_param_doubles_value);
    RUN_TEST(phase2_mixed_param_modes);
    RUN_TEST(phase2_out_param_rejects_literal_argument);
    TEST_SUMMARY();
}
