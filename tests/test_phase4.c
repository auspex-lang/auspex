#include "test_harness.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int run_auspex(const char* source, const char* args, char* out, size_t out_size) {
    FILE* f = fopen("/tmp/test_phase4_src.apx", "w");
    if (f == NULL) return -1;
    fprintf(f, "%s", source);
    fclose(f);

    char cmd[1024];
    if (args != NULL) {
        snprintf(cmd, sizeof(cmd), "./bin/auspex /tmp/test_phase4_src.apx %s > /tmp/test_phase4_out.txt 2>&1", args);
    } else {
        snprintf(cmd, sizeof(cmd), "./bin/auspex /tmp/test_phase4_src.apx > /tmp/test_phase4_out.txt 2>&1");
    }
    int rc = system(cmd);

    FILE* outf = fopen("/tmp/test_phase4_out.txt", "r");
    if (outf != NULL) {
        out[0] = '\0';
        size_t n = fread(out, 1, out_size - 1, outf);
        out[n] = '\0';
        fclose(outf);
    }
    return WEXITSTATUS(rc);
}

TEST(phase4_package_spec_body_qualified_call) {
    remove("auspex.db");
    remove("auspex.db.packages");
    remove("/tmp/phase4_test.db");
    char out[512];
    int rc = run_auspex(
        "package counter is\n"
        "    counter int;\n"
        "    proc inc() -> int;\n"
        "    func get() -> int;\n"
        "end counter;\n"
        "package body counter is\n"
        "    int counter = 0;\n"
        "    proc inc() -> int {\n"
        "        counter = counter + 1;\n"
        "        return 0;\n"
        "    }\n"
        "    func get() -> int {\n"
        "        return counter;\n"
        "    }\n"
        "end counter;\n"
        "proc main() -> int {\n"
        "    counter.inc();\n"
        "    counter.inc();\n"
        "    return counter.get();\n"
        "}\n",
        NULL, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(2, atoi(out));
}

TEST(phase4_package_state_persists_across_calls) {
    remove("auspex.db");
    remove("auspex.db.packages");
    remove("/tmp/phase4_test.db");
    char out[512];
    int rc = run_auspex(
        "package counter is\n"
        "    counter int;\n"
        "    proc inc() -> int;\n"
        "    func get() -> int;\n"
        "end counter;\n"
        "package body counter is\n"
        "    int counter = 10;\n"
        "    proc inc() -> int {\n"
        "        counter = counter + 1;\n"
        "        return 0;\n"
        "    }\n"
        "    func get() -> int {\n"
        "        return counter;\n"
        "    }\n"
        "end counter;\n"
        "proc main() -> int {\n"
        "    counter.inc();\n"
        "    counter.inc();\n"
        "    counter.inc();\n"
        "    return counter.get();\n"
        "}\n",
        NULL, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(13, atoi(out));
}

TEST(phase4_private_member_rejected_from_outside) {
    remove("auspex.db");
    remove("auspex.db.packages");
    remove("/tmp/phase4_test.db");
    char out[512];
    int rc = run_auspex(
        "package counter is\n"
        "    proc inc() -> int;\n"
        "end counter;\n"
        "package body counter is\n"
        "    int counter = 0;\n"
        "    proc bump() -> int {\n"
        "        counter = counter + 1;\n"
        "        return 0;\n"
        "    }\n"
        "    proc inc() -> int {\n"
        "        return bump();\n"
        "    }\n"
        "end counter;\n"
        "proc main() -> int {\n"
        "    return counter.bump();\n"
        "}\n",
        NULL, out, sizeof(out));
    ASSERT_INT_EQ(1, rc);
    ASSERT_INT_EQ(1, strstr(out, "private") != NULL || strstr(out, "Undefined") != NULL ||
                       strstr(out, "not") != NULL);
}

TEST(phase4_catalog_persistence_sidecar) {
    /* First run defines a package and leaves it in the sidecar. */
    char out[512];
    int rc = run_auspex(
        "package counter is\n"
        "    counter int;\n"
        "    proc inc() -> int;\n"
        "    func get() -> int;\n"
        "end counter;\n"
        "package body counter is\n"
        "    int counter = 0;\n"
        "    proc inc() -> int { counter = counter + 1; return 0; }\n"
        "    func get() -> int { return counter; }\n"
        "end counter;\n"
        "proc main() -> int { return 0; }\n",
        NULL, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);

    /* Second run uses the persisted package without redefining it. */
    rc = run_auspex(
        "proc main() -> int {\n"
        "    counter.inc();\n"
        "    counter.inc();\n"
        "    return counter.get();\n"
        "}\n",
        NULL, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(2, atoi(out));
    remove("auspex.db.packages");
}

#ifdef USE_SQLITE
TEST(phase4_catalog_persistence_sqlite) {
    remove("/tmp/phase4_test.db");
    char out[512];
    int rc = run_auspex(
        "package counter is\n"
        "    counter int;\n"
        "    proc inc() -> int;\n"
        "    func get() -> int;\n"
        "end counter;\n"
        "package body counter is\n"
        "    int counter = 0;\n"
        "    proc inc() -> int { counter = counter + 1; return 0; }\n"
        "    func get() -> int { return counter; }\n"
        "end counter;\n"
        "proc main() -> int { return 0; }\n",
        "--db /tmp/phase4_test.db", out, sizeof(out));
    ASSERT_INT_EQ(0, rc);

    rc = run_auspex(
        "proc main() -> int {\n"
        "    counter.inc();\n"
        "    return counter.get();\n"
        "}\n",
        "--db /tmp/phase4_test.db", out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, atoi(out));
    remove("/tmp/phase4_test.db");
}
#endif

int main(void) {
    RUN_TEST(phase4_package_spec_body_qualified_call);
    RUN_TEST(phase4_package_state_persists_across_calls);
    RUN_TEST(phase4_private_member_rejected_from_outside);
    RUN_TEST(phase4_catalog_persistence_sidecar);
#ifdef USE_SQLITE
    RUN_TEST(phase4_catalog_persistence_sqlite);
#endif
    TEST_SUMMARY();
}
