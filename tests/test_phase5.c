#include "test_harness.h"
#include <stdlib.h>
#include <string.h>

static int run_auspex(const char* source, const char* args, char* out, size_t out_size) {
    FILE* f = fopen("/tmp/test_phase5_src.apx", "w");
    if (f == NULL) return -1;
    fprintf(f, "%s", source);
    fclose(f);

    char cmd[1024];
    if (args != NULL) {
        snprintf(cmd, sizeof(cmd), "./bin/auspex /tmp/test_phase5_src.apx %s > /tmp/test_phase5_out.txt 2>&1", args);
    } else {
        snprintf(cmd, sizeof(cmd), "./bin/auspex /tmp/test_phase5_src.apx > /tmp/test_phase5_out.txt 2>&1");
    }
    int rc = system(cmd);

    FILE* outf = fopen("/tmp/test_phase5_out.txt", "r");
    if (outf != NULL) {
        out[0] = '\0';
        size_t n = fread(out, 1, out_size - 1, outf);
        out[n] = '\0';
        fclose(outf);
    }
    return WEXITSTATUS(rc);
}

TEST(phase5_user_defined_exception_raise) {
    char out[512];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    my_error exception;\n"
        "    try {\n"
        "        raise my_error;\n"
        "    } catch (err) {\n"
        "        print err;\n"
        "        print int_to_string(sqlcode);\n"
        "    }\n"
        "    return 42;\n"
        "}\n",
        NULL, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "my_error") != NULL);
    ASSERT_INT_EQ(1, strstr(out, "1") != NULL);
}

TEST(phase5_raise_application_error) {
    char out[512];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    try {\n"
        "        raise_application_error(-20001, \"custom business error\");\n"
        "    } catch (err) {\n"
        "        print err;\n"
        "        print int_to_string(sqlcode);\n"
        "        print sqlerrm;\n"
        "    }\n"
        "    return 0;\n"
        "}\n",
        NULL, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "custom business error") != NULL);
    ASSERT_INT_EQ(1, strstr(out, "-20001") != NULL);
}

TEST(phase5_predefined_no_data_found) {
    char out[512];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    try {\n"
        "        raise no_data_found;\n"
        "    } catch (err) {\n"
        "        print int_to_string(sqlcode);\n"
        "    }\n"
        "    return 0;\n"
        "}\n",
        NULL, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "100") != NULL);
}

TEST(phase5_predefined_too_many_rows) {
    char out[512];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    try {\n"
        "        raise too_many_rows;\n"
        "    } catch (err) {\n"
        "        print int_to_string(sqlcode);\n"
        "    }\n"
        "    return 0;\n"
        "}\n",
        NULL, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "-1422") != NULL || strstr(out, "1422") != NULL);
}

TEST(phase5_select_into_no_data_raised) {
    remove("auspex.db");
    remove("auspex.db.packages");
    char out[512];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    create table empty_table (id int);\n"
        "    int x = 0;\n"
        "    try {\n"
        "        select id into x from empty_table;\n"
        "    } catch (err) {\n"
        "        print int_to_string(sqlcode);\n"
        "    }\n"
        "    return 0;\n"
        "}\n",
        NULL, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "100") != NULL);
}

TEST(phase5_undefined_exception_rejected) {
    char out[512];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    try {\n"
        "        raise unknown_error;\n"
        "    } catch (err) {\n"
        "        print err;\n"
        "    }\n"
        "    return 0;\n"
        "}\n",
        NULL, out, sizeof(out));
    ASSERT_INT_EQ(1, rc);
    ASSERT_INT_EQ(1, strstr(out, "Undefined exception") != NULL);
}

TEST(phase5_try_without_error_continues) {
    char out[512];
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

int main(void) {
    RUN_TEST(phase5_user_defined_exception_raise);
    RUN_TEST(phase5_raise_application_error);
    RUN_TEST(phase5_predefined_no_data_found);
    RUN_TEST(phase5_predefined_too_many_rows);
    RUN_TEST(phase5_select_into_no_data_raised);
    RUN_TEST(phase5_undefined_exception_rejected);
    RUN_TEST(phase5_try_without_error_continues);
    TEST_SUMMARY();
}
