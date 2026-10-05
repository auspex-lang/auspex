#include "test_harness.h"
#include <stdlib.h>
#include <string.h>

static int run_auspex(const char* source, const char* args, char* out, size_t out_size) {
    FILE* f = fopen("/tmp/test_phase6_src.apx", "w");
    if (f == NULL) return -1;
    fprintf(f, "%s", source);
    fclose(f);

    char cmd[1024];
    if (args != NULL) {
        snprintf(cmd, sizeof(cmd), "./bin/auspex /tmp/test_phase6_src.apx %s > /tmp/test_phase6_out.txt 2>&1", args);
    } else {
        snprintf(cmd, sizeof(cmd), "./bin/auspex /tmp/test_phase6_src.apx > /tmp/test_phase6_out.txt 2>&1");
    }
    int rc = system(cmd);

    FILE* outf = fopen("/tmp/test_phase6_out.txt", "r");
    if (outf != NULL) {
        out[0] = '\0';
        size_t n = fread(out, 1, out_size - 1, outf);
        out[n] = '\0';
        fclose(outf);
    }
    return WEXITSTATUS(rc);
}

TEST(phase6_map_string_key_methods) {
    char out[512];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    map<string,int> m;\n"
        "    m[\"a\"] = 1;\n"
        "    m[\"b\"] = 2;\n"
        "    m[\"c\"] = 3;\n"
        "    print m.first();\n"
        "    print m.last();\n"
        "    print m.next(\"a\");\n"
        "    print m.prior(\"c\");\n"
        "    m.delete(\"b\");\n"
        "    print int_to_string(length(m));\n"
        "    return 0;\n"
        "}\n",
        NULL, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "a") != NULL);
    ASSERT_INT_EQ(1, strstr(out, "c") != NULL);
    ASSERT_INT_EQ(1, strstr(out, "b") != NULL);
    ASSERT_INT_EQ(1, strstr(out, "2") != NULL);
}

TEST(phase6_map_int_key_methods) {
    char out[512];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    map<int,string> m;\n"
        "    m[1] = \"one\";\n"
        "    m[2] = \"two\";\n"
        "    m[3] = \"three\";\n"
        "    print int_to_string(m.first());\n"
        "    print int_to_string(m.last());\n"
        "    print int_to_string(m.next(1));\n"
        "    print int_to_string(m.prior(3));\n"
        "    m.delete(2);\n"
        "    print int_to_string(length(m));\n"
        "    return 0;\n"
        "}\n",
        NULL, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "1") != NULL);
    ASSERT_INT_EQ(1, strstr(out, "3") != NULL);
    ASSERT_INT_EQ(1, strstr(out, "2") != NULL);
}

TEST(phase6_array_extend_and_trim) {
    char out[512];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    array<int> a;\n"
        "    a.extend(3);\n"
        "    a[0] = 10;\n"
        "    a[1] = 20;\n"
        "    a[2] = 30;\n"
        "    print int_to_string(length(a));\n"
        "    a.trim(1);\n"
        "    print int_to_string(length(a));\n"
        "    print int_to_string(a[0]);\n"
        "    print int_to_string(a[1]);\n"
        "    return 0;\n"
        "}\n",
        NULL, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "3") != NULL);
    ASSERT_INT_EQ(1, strstr(out, "2") != NULL);
    ASSERT_INT_EQ(1, strstr(out, "10") != NULL);
    ASSERT_INT_EQ(1, strstr(out, "20") != NULL);
}

TEST(phase6_bulk_collect_into_array_row) {
    char out[512];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    create table nums (id int);\n"
        "    insert into nums values (1), (2), (3);\n"
        "    array<row> rows;\n"
        "    select id bulk collect into rows from nums;\n"
        "    print int_to_string(length(rows));\n"
        "    print int_to_string(rows[0].id);\n"
        "    print int_to_string(rows[1].id);\n"
        "    print int_to_string(rows[2].id);\n"
        "    return 0;\n"
        "}\n",
        "--db :memory:", out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "3") != NULL);
    ASSERT_INT_EQ(1, strstr(out, "1") != NULL);
    ASSERT_INT_EQ(1, strstr(out, "2") != NULL);
}

TEST(phase6_forall_insert_with_scalar_array) {
    char out[512];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    create table targets (id int);\n"
        "    array<int> ids;\n"
        "    ids.extend(3);\n"
        "    ids[0] = 10;\n"
        "    ids[1] = 20;\n"
        "    ids[2] = 30;\n"
        "    forall i in ids insert into targets (id) values (?i);\n"
        "    int total = 0;\n"
        "    select sum(id) into total from targets;\n"
        "    print int_to_string(total);\n"
        "    return 0;\n"
        "}\n",
        "--db :memory:", out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "60") != NULL);
}

TEST(phase6_forall_update_with_scalar_array) {
    char out[512];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    create table targets (id int);\n"
        "    insert into targets values (1), (2), (3);\n"
        "    array<int> ids;\n"
        "    ids.extend(2);\n"
        "    ids[0] = 1;\n"
        "    ids[1] = 3;\n"
        "    forall i in ids update targets set id = id * 10 where id = ?i;\n"
        "    int total = 0;\n"
        "    select sum(id) into total from targets;\n"
        "    print int_to_string(total);\n"
        "    return 0;\n"
        "}\n",
        "--db :memory:", out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "42") != NULL);
}

TEST(phase6_forall_delete_with_scalar_array) {
    char out[512];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    create table targets (id int);\n"
        "    insert into targets values (1), (2), (3), (4);\n"
        "    array<int> ids;\n"
        "    ids.extend(2);\n"
        "    ids[0] = 1;\n"
        "    ids[1] = 3;\n"
        "    forall i in ids delete from targets where id = ?i;\n"
        "    int total = 0;\n"
        "    select sum(id) into total from targets;\n"
        "    print int_to_string(total);\n"
        "    return 0;\n"
        "}\n",
        "--db :memory:", out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "6") != NULL);
}

/* Static SQL with ?var placeholders on the custom engine (no --db). */
TEST(phase6_custom_engine_binds_static_sql) {
    remove("auspex.db");
    char out[512];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    create table people (id int, name string);\n"
        "    int x = 7;\n"
        "    string s = \"o'brien\";\n"
        "    insert into people values (?x, ?s);\n"
        "    insert into people values (8, 'al');\n"
        "    int n = 0;\n"
        "    select count(*) into n from people where id = ?x;\n"
        "    print concat(\"matching=\", int_to_string(n));\n"
        "    update people set name = ?s where id = 8;\n"
        "    string got = \"\";\n"
        "    select name into got from people where id = 8;\n"
        "    print concat(\"name=\", got);\n"
        "    delete from people where id = ?x;\n"
        "    select count(*) into n from people;\n"
        "    print concat(\"left=\", int_to_string(n));\n"
        "    return 0;\n"
        "}\n",
        NULL, out, sizeof(out));
    remove("auspex.db");
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "matching=1") != NULL);
    ASSERT_INT_EQ(1, strstr(out, "name=o'brien") != NULL);
    ASSERT_INT_EQ(1, strstr(out, "left=1") != NULL);
}

TEST(phase6_custom_engine_forall_binds_scalar_array) {
    remove("auspex.db");
    char out[512];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    create table targets (id int, tag string);\n"
        "    array<int> ids;\n"
        "    ids.extend(3);\n"
        "    ids[0] = 10;\n"
        "    ids[1] = 20;\n"
        "    ids[2] = 30;\n"
        "    forall i in ids insert into targets values (?i, 'x');\n"
        "    int n = 0;\n"
        "    select count(*) into n from targets;\n"
        "    print concat(\"rows=\", int_to_string(n));\n"
        "    forall i in ids delete from targets where id = ?i;\n"
        "    select count(*) into n from targets;\n"
        "    print concat(\"after=\", int_to_string(n));\n"
        "    return 0;\n"
        "}\n",
        NULL, out, sizeof(out));
    remove("auspex.db");
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "rows=3") != NULL);
    ASSERT_INT_EQ(1, strstr(out, "after=0") != NULL);
}

int main(void) {
    RUN_TEST(phase6_map_string_key_methods);
    RUN_TEST(phase6_map_int_key_methods);
    RUN_TEST(phase6_array_extend_and_trim);
    RUN_TEST(phase6_bulk_collect_into_array_row);
    RUN_TEST(phase6_forall_insert_with_scalar_array);
    RUN_TEST(phase6_forall_update_with_scalar_array);
    RUN_TEST(phase6_forall_delete_with_scalar_array);
    RUN_TEST(phase6_custom_engine_binds_static_sql);
    RUN_TEST(phase6_custom_engine_forall_binds_scalar_array);
    TEST_SUMMARY();
}
