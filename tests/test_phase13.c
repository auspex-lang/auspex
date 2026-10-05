#include "test_harness.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ==========================================================================
 * Phase 13 open issues #34-#38: acceptance tests (TDD).
 *
 * These tests pin what "done" looks like for each Phase 13 item. The #34,
 * #35, #36 and #38 churn tests failed on the main that introduced them and
 * were driven green by the respective issues' implementations; the #37 test
 * and the #38 expressibility test are regression guards that must stay
 * green.
 * ========================================================================== */

static int run_auspex(const char* source, char* out, size_t out_size) {
    FILE* f = fopen("/tmp/test_phase13_src.apx", "w");
    if (f == NULL) return -1;
    fprintf(f, "%s", source);
    fclose(f);

    int rc = system("./bin/auspex /tmp/test_phase13_src.apx > /tmp/test_phase13_out.txt 2>&1");

    FILE* outf = fopen("/tmp/test_phase13_out.txt", "r");
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

static int read_file_to(const char* path, char* buf, size_t size) {
    FILE* f = fopen(path, "rb");
    if (f == NULL) return -1;
    size_t n = fread(buf, 1, size - 1, f);
    buf[n] = '\0';
    fclose(f);
    return (int)n;
}

static void clean_default_db(void) {
    remove("auspex.db");
    remove("auspex.db.packages");
    remove("auspex.db.programs");
}

/* ===== Issue #34: Makefile install target and man page =====
 *
 * The Makefile has no install/uninstall targets today
 * ("make: *** No rule to make target 'install'.  Stop.", exit 2). Acceptance:
 *   - `make install PREFIX=<dir>` installs bin/auspex and a man page at
 *     share/man/man1/auspex.1 (conventional PREFIX layout),
 *   - the installed binary runs standalone (`--version` prints "Auspex"),
 *   - `make uninstall PREFIX=<dir>` removes both again. */

#define PHASE13_PREFIX "/tmp/auspex_phase13_install"
#define PHASE13_BIN PHASE13_PREFIX "/bin/auspex"
#define PHASE13_MAN PHASE13_PREFIX "/share/man/man1/auspex.1"

TEST(phase13_issue34_make_install_target) {
    char out[256];
    system("rm -rf " PHASE13_PREFIX);

    int rc = system("make install PREFIX=" PHASE13_PREFIX
                    " > /tmp/test_phase13_install.log 2>&1");
    ASSERT_INT_EQ(0, WEXITSTATUS(rc));

    ASSERT_INT_EQ(0, access(PHASE13_BIN, X_OK));

    rc = system(PHASE13_BIN " --version > /tmp/test_phase13_ver.log 2>&1");
    ASSERT_INT_EQ(0, WEXITSTATUS(rc));
    ASSERT_INT_EQ(1, read_file_to("/tmp/test_phase13_ver.log", out, sizeof(out)) > 0);
    ASSERT_INT_EQ(1, output_contains(out, "Auspex"));

    /* Man page at the conventional PREFIX/share/man/man1 location. */
    ASSERT_INT_EQ(0, access(PHASE13_MAN, R_OK));
    ASSERT_INT_EQ(1, read_file_to(PHASE13_MAN, out, sizeof(out)) > 0);

    system("rm -rf " PHASE13_PREFIX);
}

TEST(phase13_issue34_make_uninstall_target) {
    system("rm -rf " PHASE13_PREFIX);

    int rc = system("make install PREFIX=" PHASE13_PREFIX
                    " > /tmp/test_phase13_install.log 2>&1");
    ASSERT_INT_EQ(0, WEXITSTATUS(rc));

    rc = system("make uninstall PREFIX=" PHASE13_PREFIX
                " > /tmp/test_phase13_uninstall.log 2>&1");
    ASSERT_INT_EQ(0, WEXITSTATUS(rc));
    ASSERT(access(PHASE13_BIN, F_OK) != 0);
    ASSERT(access(PHASE13_MAN, F_OK) != 0);

    system("rm -rf " PHASE13_PREFIX);
}

/* ===== Issue #35: README refresh =====
 *
 * README.md still presents the Phase 1-7 feature set: its roadmap section
 * claims "Phases 1-7 are complete" (with an UTF-8 en dash) and major landed
 * areas are undocumented (row-level/statement-level triggers, sequences,
 * views, the dbms_sql cursor API, utl_file, external_call/FFI). Acceptance:
 * the README documents each Phase 8-12 feature area and the stale roadmap
 * canary is gone. Strings below were verified absent (or present, for the
 * anchors) on current main; the en dash is written as octal escapes. */

TEST(phase13_issue35_readme_documents_phase12_features) {
    char* readme = malloc(65536);
    ASSERT_PTR_NOT_NULL(readme);
    int n = read_file_to("README.md", readme, 65536);
    ASSERT_INT_EQ(1, n > 0);

    /* Row-level and statement-level triggers. */
    ASSERT(strstr(readme, "for each row") != NULL);
    ASSERT(strstr(readme, "before insert") != NULL);

    /* Sequences (create_sequence/nextval/currval). */
    ASSERT(strstr(readme, "nextval") != NULL);

    /* Views. */
    ASSERT(strstr(readme, "create view") != NULL);

    /* dbms_sql cursor API. */
    ASSERT(strstr(readme, "dbms_sql.open_cursor") != NULL);

    /* utl_file. */
    ASSERT(strstr(readme, "utl_file.fopen") != NULL);

    /* external_call FFI marshalling. */
    ASSERT(strstr(readme, "external_call") != NULL);

    /* Stale roadmap canary: must be rewritten (en dash = \342\200\223). */
    ASSERT(strstr(readme, "1\342\200\2237 are complete") == NULL);

    /* Anchors: areas the rewrite must not drop. */
    ASSERT(strstr(readme, "Packages") != NULL);
    ASSERT(strstr(readme, "Conditional compilation") != NULL);
    ASSERT(strstr(readme, "-DNAME") != NULL);
    ASSERT(strstr(readme, "SQLite backend") != NULL);
    ASSERT(strstr(readme, "Custom SQL engine") != NULL);

    free(readme);
}

/* ===== Issue #36: dynamic growth of fixed ceilings =====
 *
 * Today: MAX_LOCALS is 256 (src/typecheck.c, src/codegen.c) and STACK_MAX
 * is 256 (include/vm.h); both reject overflow with a compile/runtime error
 * instead of growing. Acceptance: programs just beyond today's limits run
 * correctly. Each test fails today with the respective limit error (no
 * crash): "Too many local variables" (compile) and a runtime stack error. */

TEST(phase13_issue36_locals_beyond_fixed_limit) {
    clean_default_db();
    /* 300 locals in one scope (> MAX_LOCALS = 256), each initialized and
       summed. Expected sum: 0 + 1 + ... + 299 = 44850. */
    size_t cap = 65536;
    char* src = malloc(cap);
    ASSERT_PTR_NOT_NULL(src);
    size_t off = (size_t)snprintf(src, cap, "proc main() -> int {\n    int total = 0;\n");
    for (int i = 0; i < 300; i++) {
        off += (size_t)snprintf(src + off, cap - off,
                                "    int v%d = %d;\n    total = total + v%d;\n", i, i, i);
    }
    snprintf(src + off, cap - off, "    print total;\n    return 0;\n}\n");

    char out[256];
    int rc = run_auspex(src, out, sizeof(out));
    free(src);
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "44850"));
}

TEST(phase13_issue36_recursion_beyond_fixed_stack) {
    clean_default_db();
    /* Bounded recursion to depth 1000 (> STACK_MAX = 256 frames).
       Expected accumulator: 1000 + 999 + ... + 1 = 500500. */
    char out[256];
    int rc = run_auspex(
        "func countdown(n int, acc int) -> int {\n"
        "    if n == 0 {\n"
        "        return acc;\n"
        "    }\n"
        "    return countdown(n - 1, acc + n);\n"
        "}\n"
        "proc main() -> int {\n"
        "    print countdown(1000, 0);\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "500500"));
}

/* ===== Issue #37: incremental REPL compilation — regression guard =====
 *
 * #37 replaces the REPL's full recompile-per-input with incremental
 * compilation. The OBSERVABLE behavior must not change; this guard drives
 * ~20 sequential interactive inputs through the REPL and pins the results:
 * procs accumulate and can be called, top-level variables persist, packages
 * entered interactively are callable, expression inputs evaluate, and the
 * meta commands (.vars/.defs/.history) reflect the session.
 *
 * This test PASSES on current main and must stay green across the refactor.
 * (Redefining a proc and inputs that fail to compile are covered in
 * tests/test_repl.c since #58.) */

TEST(phase13_issue37_repl_accumulation_guard) {
    clean_default_db();
    FILE* f = fopen("/tmp/test_phase13_repl_in.txt", "w");
    ASSERT_PTR_NOT_NULL(f);
    fprintf(f, "%s\n",
        "proc add(a int, b int) -> int { return a + b; }");
    fprintf(f, "%s\n", "print concat(\"add:\", int_to_string(add(2, 3)));");
    fprintf(f, "%s\n", "int base = 10;");
    fprintf(f, "%s\n", "print concat(\"sum:\", int_to_string(add(base, 5)));");
    fprintf(f, "%s\n", "proc mul(a int, b int) -> int { return a * b; }");
    fprintf(f, "%s\n", "print concat(\"prod:\", int_to_string(mul(base, 2)));");
    fprintf(f, "%s\n", "base * 2");
    fprintf(f, "%s\n", "int extra = 32;");
    fprintf(f, "%s\n", "print concat(\"both:\", int_to_string(add(base, extra)));");
    fprintf(f, "%s\n", "6 * 7");
    fprintf(f, "%s\n", "package p13 is");
    fprintf(f, "%s\n", "    proc setb(v int) -> int;");
    fprintf(f, "%s\n", "    func getb() -> int;");
    fprintf(f, "%s\n", "end p13;");
    fprintf(f, "%s\n", "package body p13 is");
    fprintf(f, "%s\n", "    int backing = 0;");
    fprintf(f, "%s\n", "    proc setb(v int) -> int { backing = v; return 0; }");
    fprintf(f, "%s\n", "    func getb() -> int { return backing; }");
    fprintf(f, "%s\n", "end p13;");
    fprintf(f, "%s\n", "p13.setb(41);");
    fprintf(f, "%s\n", "print concat(\"pkg:\", int_to_string(p13.getb()));");
    fprintf(f, "%s\n", ".vars");
    fprintf(f, "%s\n", ".defs");
    fprintf(f, "%s\n", ".history");
    fprintf(f, "%s\n", "print concat(\"mul:\", int_to_string(mul(3, 7)));");
    fprintf(f, "%s\n", "base + 1");
    fprintf(f, "%s\n", ".help");
    fprintf(f, "%s\n", ".exit");
    fclose(f);

    int rc = system("./bin/auspex < /tmp/test_phase13_repl_in.txt"
                    " > /tmp/test_phase13_repl_out.txt 2>&1");
    ASSERT_INT_EQ(0, WEXITSTATUS(rc));

    char out[16384];
    ASSERT_INT_EQ(1, read_file_to("/tmp/test_phase13_repl_out.txt", out, sizeof(out)) > 0);
    ASSERT_INT_EQ(1, output_contains(out, "Auspex REPL"));

    /* Each input's marker output must appear, in input order (the REPL
       re-runs the accumulated main body on every input, so markers repeat;
       first occurrences must still be ordered). */
    const char* p_add = strstr(out, "add:5");
    const char* p_sum = strstr(out, "sum:15");
    const char* p_prod = strstr(out, "prod:20");
    const char* p_both = strstr(out, "both:42");
    const char* p_pkg = strstr(out, "pkg:41");
    const char* p_mul = strstr(out, "mul:21");
    ASSERT(p_add != NULL && p_sum != NULL && p_prod != NULL);
    ASSERT(p_both != NULL && p_pkg != NULL && p_mul != NULL);
    ASSERT(p_add < p_sum && p_sum < p_prod);
    ASSERT(p_prod < p_both && p_both < p_pkg && p_pkg < p_mul);

    /* Variables and definitions accumulate across inputs. */
    ASSERT_INT_EQ(1, output_contains(out, "= 10"));      /* .vars: base's value */
    ASSERT_INT_EQ(1, output_contains(out, "proc add"));
    ASSERT_INT_EQ(1, output_contains(out, "proc mul"));
    ASSERT_INT_EQ(1, output_contains(out, "package body p13"));
    ASSERT_INT_EQ(1, output_contains(out, "6 * 7"));     /* .history echoes inputs */

    clean_default_db();
}

/* ===== Issue #38: cycle-safe collection =====
 *
 * Auspex reference-counts arrays/maps/rows, so unreachable reference cycles
 * leak today. Cycles ARE expressible via the `any` type: a
 * map<string, any> can hold itself and an array<map<string, any>> that
 * holds the map.
 *
 * phase13_issue38_cycles_expressible_and_complete is a regression guard
 * (passes today): cyclic structures must remain expressible and runnable
 * once collection is cycle-safe.
 *
 * phase13_issue38_cycle_churn_completes is the acceptance test (FAILS
 * today): building and dropping 2000 cyclic structures in a loop must
 * complete. Today it dies with a runtime error ("Invalid local variable
 * slot") because loop-body temporaries exhaust the fixed STACK_MAX value
 * stack -- so this test needs #36's dynamic stack growth AND #38's cycle
 * collection to pass.
 *
 * TODO(#38): the true acceptance criterion is BOUNDED memory under cycle
 * churn. That cannot be pinned portably here (no /proc on the macOS CI
 * runners, and .github/workflows/sanitizers.yml runs ASan with
 * detect_leaks=0). Follow-up: re-run the churn program under
 * ASAN_OPTIONS=detect_leaks=1 (or enable leak detection in the sanitizer
 * workflow) and require zero leaks once cycle collection exists. */

TEST(phase13_issue38_cycles_expressible_and_complete) {
    clean_default_db();
    char out[256];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    map<string, any> m;\n"
        "    m[\"x\"] = 7;\n"
        "    m[\"self\"] = m;\n"
        "    array<map<string, any>> a;\n"
        "    append(a, m);\n"
        "    m[\"arr\"] = a;\n"
        "    print \"cycle-built\";\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "cycle-built"));
    clean_default_db();
}

TEST(phase13_issue38_cycle_churn_completes) {
    clean_default_db();
    char out[256];
    int rc = run_auspex(
        "proc main() -> int {\n"
        "    int i = 0;\n"
        "    while i < 2000 {\n"
        "        map<string, any> m;\n"
        "        m[\"x\"] = i;\n"
        "        m[\"self\"] = m;\n"
        "        array<map<string, any>> a;\n"
        "        append(a, m);\n"
        "        m[\"arr\"] = a;\n"
        "        i = i + 1;\n"
        "    }\n"
        "    print \"cycles-done\";\n"
        "    print i;\n"
        "    return 0;\n"
        "}\n",
        out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, output_contains(out, "cycles-done"));
    ASSERT_INT_EQ(1, output_contains(out, "2000"));
    clean_default_db();
}

int main(void) {
    printf("test_phase13:\n");
    RUN_TEST(phase13_issue34_make_install_target);
    RUN_TEST(phase13_issue34_make_uninstall_target);
    RUN_TEST(phase13_issue35_readme_documents_phase12_features);
    RUN_TEST(phase13_issue36_locals_beyond_fixed_limit);
    RUN_TEST(phase13_issue36_recursion_beyond_fixed_stack);
    RUN_TEST(phase13_issue37_repl_accumulation_guard);
    RUN_TEST(phase13_issue38_cycles_expressible_and_complete);
    RUN_TEST(phase13_issue38_cycle_churn_completes);
    TEST_SUMMARY();
}
