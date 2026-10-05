#include "test_harness.h"
#include <stdlib.h>
#include <string.h>

static int run_auspex(const char* source, const char* args, char* out, size_t out_size) {
    remove("auspex.db");
    FILE* f = fopen("/tmp/test_phase3_src.apx", "w");
    if (f == NULL) return -1;
    fprintf(f, "%s", source);
    fclose(f);

    char cmd[1024];
    if (args != NULL) {
        snprintf(cmd, sizeof(cmd), "./bin/auspex /tmp/test_phase3_src.apx %s > /tmp/test_phase3_out.txt 2>&1", args);
    } else {
        snprintf(cmd, sizeof(cmd), "./bin/auspex /tmp/test_phase3_src.apx > /tmp/test_phase3_out.txt 2>&1");
    }
    int rc = system(cmd);

    FILE* outf = fopen("/tmp/test_phase3_out.txt", "r");
    if (outf != NULL) {
        out[0] = '\0';
        size_t n = fread(out, 1, out_size - 1, outf);
        out[n] = '\0';
        fclose(outf);
    }
    return WEXITSTATUS(rc);
}

TEST(phase3_cursor_declare_open_fetch_close) {
    char out[512];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    create table t1 (id int, name string);\n"
        "    insert into t1 values (1, 'Alice');\n"
        "    insert into t1 values (2, 'Bob');\n"
        "    cursor c is select id, name from t1;\n"
        "    open c;\n"
        "    int id = 0;\n"
        "    string name = \"\";\n"
        "    fetch c into id, name;\n"
        "    print int_to_string(id);\n"
        "    print name;\n"
        "    close c;\n"
        "    return 0;\n"
        "}\n",
        NULL, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "1") != NULL);
    ASSERT_INT_EQ(1, strstr(out, "Alice") != NULL);
}

TEST(phase3_cursor_dynamic_open_fetch_loop) {
    char out[512];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    create table t2 (id int, name string);\n"
        "    insert into t2 values (1, 'a');\n"
        "    insert into t2 values (2, 'b');\n"
        "    insert into t2 values (3, 'c');\n"
        "    cursor c;\n"
        "    open c for select id, name from t2 order by id;\n"
        "    int id = 0;\n"
        "    string name = \"\";\n"
        "    int total = 0;\n"
        "    while (c%isopen) {\n"
        "        fetch c into id, name;\n"
        "        if (c%notfound) { close c; break; }\n"
        "        total = total + id;\n"
        "    }\n"
        "    print int_to_string(total);\n"
        "    return 0;\n"
        "}\n",
        NULL, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "6") != NULL);
}

TEST(phase3_cursor_attributes) {
    char out[512];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    create table t3 (id int, name string);\n"
        "    insert into t3 values (1, 'Alice');\n"
        "    cursor c is select id, name from t3;\n"
        "    print c%isopen;\n"
        "    open c;\n"
        "    print c%isopen;\n"
        "    print c%found;\n"
        "    print int_to_string(c%rowcount);\n"
        "    int id = 0;\n"
        "    string name = \"\";\n"
        "    fetch c into id, name;\n"
        "    print c%found;\n"
        "    print c%notfound;\n"
        "    print int_to_string(c%rowcount);\n"
        "    fetch c into id, name;\n"
        "    print c%found;\n"
        "    print c%notfound;\n"
        "    print int_to_string(c%rowcount);\n"
        "    close c;\n"
        "    print c%isopen;\n"
        "    return 0;\n"
        "}\n",
        NULL, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "false") != NULL);
    ASSERT_INT_EQ(1, strstr(out, "true") != NULL);
}

TEST(phase3_cursor_passed_to_procedure) {
    char out[512];
    int rc = run_auspex(
        "proc process(c cursor) -> int {\n"
        "    int id = 0;\n"
        "    string name = \"\";\n"
        "    fetch c into id, name;\n"
        "    print int_to_string(id);\n"
        "    print name;\n"
        "    close c;\n"
        "    return 0;\n"
        "}\n"
        "proc main() -> int {\n"
        "    create table t4 (id int, name string);\n"
        "    insert into t4 values (7, 'Gwen');\n"
        "    cursor c is select id, name from t4;\n"
        "    open c;\n"
        "    process(c);\n"
        "    return 0;\n"
        "}\n",
        NULL, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "7") != NULL);
    ASSERT_INT_EQ(1, strstr(out, "Gwen") != NULL);
}

int main(void) {
    RUN_TEST(phase3_cursor_declare_open_fetch_close);
    RUN_TEST(phase3_cursor_dynamic_open_fetch_loop);
    RUN_TEST(phase3_cursor_attributes);
    RUN_TEST(phase3_cursor_passed_to_procedure);
    TEST_SUMMARY();
}
