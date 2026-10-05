#include "test_harness.h"
#include <stdlib.h>
#include <string.h>

static int run_auspex_with_setup(const char* source, const char* setup, const char* args, char* out, size_t out_size) {
    const char* db_arg = args != NULL ? args : "";

    if (setup != NULL) {
        FILE* f = fopen("/tmp/test_phase7_setup.apx", "w");
        if (f == NULL) return -1;
        fprintf(f, "%s", setup);
        fclose(f);

        char setup_cmd[1024];
        snprintf(setup_cmd, sizeof(setup_cmd),
                 "./bin/auspex /tmp/test_phase7_setup.apx %s > /tmp/test_phase7_setup_out.txt 2>&1",
                 db_arg);
        int setup_rc = system(setup_cmd);
        if (WEXITSTATUS(setup_rc) != 0) {
            FILE* outf = fopen("/tmp/test_phase7_setup_out.txt", "r");
            if (outf != NULL) {
                out[0] = '\0';
                size_t n = fread(out, 1, out_size - 1, outf);
                out[n] = '\0';
                fclose(outf);
            }
            return WEXITSTATUS(setup_rc);
        }
    }

    FILE* f = fopen("/tmp/test_phase7_src.apx", "w");
    if (f == NULL) return -1;
    fprintf(f, "%s", source);
    fclose(f);

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "./bin/auspex /tmp/test_phase7_src.apx %s > /tmp/test_phase7_out.txt 2>&1", db_arg);
    int rc = system(cmd);

    FILE* outf = fopen("/tmp/test_phase7_out.txt", "r");
    if (outf != NULL) {
        out[0] = '\0';
        size_t n = fread(out, 1, out_size - 1, outf);
        out[n] = '\0';
        fclose(outf);
    }
    return WEXITSTATUS(rc);
}

static int run_auspex(const char* source, const char* args, char* out, size_t out_size) {
    return run_auspex_with_setup(source, NULL, args, out, out_size);
}

TEST(phase7_var_percent_type) {
    char out[512];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    int x = 42;\n"
        "    x%type y = 7;\n"
        "    y = y + 1;\n"
        "    print int_to_string(y);\n"
        "    return 0;\n"
        "}\n",
        NULL, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "8") != NULL);
}

TEST(phase7_column_percent_type) {
    char out[512];
    remove("auspex.db");
    int rc = run_auspex_with_setup(
        "proc main() -> int {\n"
        "    products.name%type p_name;\n"
        "    p_name = \"widget\";\n"
        "    print p_name;\n"
        "    return 0;\n"
        "}\n",
        "proc main() -> int {\n"
        "    create table products (id int, name string);\n"
        "    return 0;\n"
        "}\n",
        NULL, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "widget") != NULL);
}

TEST(phase7_table_percent_rowtype) {
    char out[512];
    remove("auspex.db");
    int rc = run_auspex_with_setup(
        "proc main() -> int {\n"
        "    products%rowtype r;\n"
        "    r.id = 1;\n"
        "    r.name = \"gadget\";\n"
        "    print int_to_string(r.id);\n"
        "    print r.name;\n"
        "    return 0;\n"
        "}\n",
        "proc main() -> int {\n"
        "    create table products (id int, name string);\n"
        "    return 0;\n"
        "}\n",
        NULL, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "1") != NULL);
    ASSERT_INT_EQ(1, strstr(out, "gadget") != NULL);
}

TEST(phase7_date_type_and_to_char) {
    char out[512];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    date d = to_date(\"2024-03-15\", \"YYYY-MM-DD\");\n"
        "    string s = to_char(d, \"YYYY-MM-DD\");\n"
        "    print s;\n"
        "    return 0;\n"
        "}\n",
        NULL, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "2024-03-15") != NULL);
}

TEST(phase7_timestamp_type_and_current) {
    char out[512];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    timestamp t = current_timestamp();\n"
        "    string s = to_char(t, \"YYYY-MM-DD\");\n"
        "    print s;\n"
        "    return 0;\n"
        "}\n",
        NULL, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "2026-") != NULL || strstr(out, "2025-") != NULL);
}

TEST(phase7_user_defined_subtype) {
    char out[512];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    subtype score is int;\n"
        "    score s = 95;\n"
        "    print int_to_string(s);\n"
        "    return 0;\n"
        "}\n",
        NULL, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "95") != NULL);
}

int main(void) {
    RUN_TEST(phase7_var_percent_type);
    RUN_TEST(phase7_column_percent_type);
    RUN_TEST(phase7_table_percent_rowtype);
    RUN_TEST(phase7_date_type_and_to_char);
    RUN_TEST(phase7_timestamp_type_and_current);
    RUN_TEST(phase7_user_defined_subtype);
    TEST_SUMMARY();
}
