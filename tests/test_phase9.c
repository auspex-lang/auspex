#include "test_harness.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int run_auspex(const char* source, char* out, size_t out_size) {
    FILE* f = fopen("/tmp/test_phase9_src.apx", "w");
    if (f == NULL) return -1;
    fprintf(f, "%s", source);
    fclose(f);

    int rc = system("./bin/auspex /tmp/test_phase9_src.apx > /tmp/test_phase9_out.txt 2>&1");

    FILE* outf = fopen("/tmp/test_phase9_out.txt", "r");
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

TEST(phase9_dbms_output_buffer_and_get_lines) {
    char out[256];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    dbms_output.enable(10);\n"
        "    dbms_output.put_line(\"hello\");\n"
        "    dbms_output.put_line(\"world\");\n"
        "    array<string> lines = dbms_output.get_lines();\n"
        "    print int_to_string(length(lines));\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "2"));
}

TEST(phase9_dbms_output_disabled_put_is_noop) {
    char out[256];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    dbms_output.put_line(\"ignored\");\n"
        "    array<string> lines = dbms_output.get_lines();\n"
        "    print int_to_string(length(lines));\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "0"));
}

TEST(phase9_utl_file_write_and_read) {
    FILE* f = fopen("/tmp/test_phase9_file.txt", "w");
    if (f != NULL) fclose(f);

    char out[256];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    int h = utl_file.fopen(\"/tmp/test_phase9_file.txt\", \"w\");\n"
        "    utl_file.put_line(h, \"hello\");\n"
        "    utl_file.put_line(h, \"world\");\n"
        "    utl_file.fclose(h);\n"
        "\n"
        "    int r = utl_file.fopen(\"/tmp/test_phase9_file.txt\", \"r\");\n"
        "    string a = utl_file.get_line(r);\n"
        "    string b = utl_file.get_line(r);\n"
        "    utl_file.fclose(r);\n"
        "\n"
        "    print a;\n"
        "    print b;\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "hello"));
    ASSERT_INT_EQ(1, output_contains(out, "world"));
}

TEST(phase9_dbms_sql_execute_and_query) {
    remove("auspex.db");
    char out[256];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    dbms_sql.execute(\"CREATE TABLE phase9_t (id int, name string)\");\n"
        "    dbms_sql.execute(\"INSERT INTO phase9_t VALUES (1, 'alice')\");\n"
        "    dbms_sql.execute(\"INSERT INTO phase9_t VALUES (2, 'bob')\");\n"
        "    array<row> rows = dbms_sql.query(\"SELECT * FROM phase9_t ORDER BY id\");\n"
        "    print int_to_string(length(rows));\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "2"));
}

TEST(phase9_regexp_like) {
    char out[256];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    if regexp_like(\"hello123\", \"[0-9]+\") { print \"match\"; }\n"
        "    if regexp_like(\"hello\", \"^[a-z]+$\") { print \"lower\"; }\n"
        "    if regexp_like(\"hello\", \"[0-9]+\") { print \"bad\"; }\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "match"));
    ASSERT_INT_EQ(1, output_contains(out, "lower"));
    ASSERT_INT_EQ(0, output_contains(out, "bad"));
}

TEST(phase9_regexp_replace) {
    char out[256];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    print regexp_replace(\"a1b22c333\", \"[0-9]+\", \"#\");\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "a#b#c#"));
}

TEST(phase9_regexp_substr) {
    char out[256];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    print regexp_substr(\"abc123def\", \"[0-9]+\");\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "123"));
}

TEST(phase9_sequence_nextval_and_currval) {
    char out[256];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    create_sequence(\"order_seq\", 10, 5);\n"
        "    print int_to_string(nextval(\"order_seq\"));\n"
        "    print int_to_string(nextval(\"order_seq\"));\n"
        "    print int_to_string(currval(\"order_seq\"));\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "10"));
    ASSERT_INT_EQ(1, output_contains(out, "15"));
}

TEST(phase9_sequence_drop_and_undefined) {
    char out[256];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    create_sequence(\"tmp_seq\", 1, 1);\n"
        "    nextval(\"tmp_seq\");\n"
        "    drop_sequence(\"tmp_seq\");\n"
        "    try {\n"
        "        nextval(\"tmp_seq\");\n"
        "        print \"bad\";\n"
        "    } catch (err) {\n"
        "        print \"caught\";\n"
        "    }\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "caught"));
    ASSERT_INT_EQ(0, output_contains(out, "bad"));
}

int main(void) {
    RUN_TEST(phase9_dbms_output_buffer_and_get_lines);
    RUN_TEST(phase9_dbms_output_disabled_put_is_noop);
    RUN_TEST(phase9_utl_file_write_and_read);
    RUN_TEST(phase9_dbms_sql_execute_and_query);
    RUN_TEST(phase9_regexp_like);
    RUN_TEST(phase9_regexp_replace);
    RUN_TEST(phase9_regexp_substr);
    RUN_TEST(phase9_sequence_nextval_and_currval);
    RUN_TEST(phase9_sequence_drop_and_undefined);
    TEST_SUMMARY();
}
