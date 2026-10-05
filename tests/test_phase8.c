#include "test_harness.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef USE_SQLITE
#include <sqlite3.h>
#include "sqlite_driver.h"
#include "stored_programs.h"
#endif

static int run_auspex(const char* source, const char* args, char* out, size_t out_size) {
    FILE* f = fopen("/tmp/test_phase8_src.apx", "w");
    if (f == NULL) return -1;
    fprintf(f, "%s", source);
    fclose(f);

    char cmd[1024];
    if (args != NULL) {
        snprintf(cmd, sizeof(cmd), "./bin/auspex /tmp/test_phase8_src.apx %s > /tmp/test_phase8_out.txt 2>&1", args);
    } else {
        snprintf(cmd, sizeof(cmd), "./bin/auspex /tmp/test_phase8_src.apx > /tmp/test_phase8_out.txt 2>&1");
    }
    int rc = system(cmd);

    FILE* outf = fopen("/tmp/test_phase8_out.txt", "r");
    if (outf != NULL) {
        out[0] = '\0';
        size_t n = fread(out, 1, out_size - 1, outf);
        out[n] = '\0';
        fclose(outf);
    }
    return WEXITSTATUS(rc);
}

TEST(phase8_stored_proc_persists_across_runs_sqlite) {
    const char* db = "/tmp/test_phase8_proc.db";
    unlink(db);
    char out[256];
    char args[256];
    snprintf(args, sizeof(args), "--db %s", db);

    int rc = run_auspex(
        "proc add(a int, b int) -> int { return a + b; }\n"
        "proc main() -> int { return add(2, 3); }\n",
        args, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "5") != NULL);

    rc = run_auspex(
        "proc main() -> int { return add(10, 20); }\n",
        args, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "30") != NULL);
}

TEST(phase8_stored_func_persists_across_runs_sqlite) {
    const char* db = "/tmp/test_phase8_func.db";
    unlink(db);
    char out[256];
    char args[256];
    snprintf(args, sizeof(args), "--db %s", db);

    int rc = run_auspex(
        "func double(x int) -> int { return x * 2; }\n"
        "proc main() -> int { return double(7); }\n",
        args, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "14") != NULL);

    rc = run_auspex(
        "proc main() -> int { return double(5); }\n",
        args, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "10") != NULL);
}

TEST(phase8_proc_main_is_not_persisted) {
    const char* db = "/tmp/test_phase8_main.db";
    unlink(db);
    char out[256];
    char args[256];
    snprintf(args, sizeof(args), "--db %s", db);

    int rc = run_auspex(
        "proc helper() -> int { return 99; }\n"
        "proc main() -> int { return helper(); }\n",
        args, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "99") != NULL);

    rc = run_auspex(
        "proc main() -> int { return helper(); }\n",
        args, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "99") != NULL);
}

#ifdef USE_SQLITE
TEST(phase8_authid_definer_is_stored_and_stripped) {
    const char* db = "/tmp/test_phase8_authid.db";
    unlink(db);

    DBDriver driver;
    sqlite_driver_init(&driver);
    if (!driver.open(&driver, db)) {
        FAIL("could not open sqlite database");
    }

    const char* source = "func secret() -> int authid definer { return 42; }\n";
    ASSERT_INT_EQ(1, stored_programs_save_source(&driver, NULL, source));

    sqlite3* dbh = ((SQLiteImpl*)driver.impl)->db;
    sqlite3_stmt* stmt = NULL;
    if (sqlite3_prepare_v2(dbh,
                           "SELECT authid FROM _auspex_program_units "
                           "WHERE name = 'secret' AND unit_type = 'FUNCTION'",
                           -1, &stmt, NULL) != SQLITE_OK) {
        driver.close(&driver);
        FAIL("could not prepare authid query");
    }

    char authid[64] = "";
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* text = (const char*)sqlite3_column_text(stmt, 0);
        if (text != NULL) {
            snprintf(authid, sizeof(authid), "%s", text);
        }
    }
    sqlite3_finalize(stmt);
    ASSERT_STRING_EQ("definer", authid);

    char* loaded = stored_programs_load_source(&driver, NULL);
    ASSERT_PTR_NOT_NULL(loaded);
    ASSERT(strstr(loaded, "authid") == NULL);
    ASSERT(strstr(loaded, "func secret") != NULL);
    free(loaded);

    driver.close(&driver);
}

TEST(phase8_savepoint_rollback_to_sqlite) {
    const char* db = "/tmp/test_phase8_savepoint.db";
    unlink(db);
    char out[512];
    char args[256];
    snprintf(args, sizeof(args), "--db %s", db);

    int rc = run_auspex(
        "proc main() -> int {\n"
        "    create table t (id int);\n"
        "    begin;\n"
        "    insert into t values (1);\n"
        "    savepoint sp1;\n"
        "    insert into t values (2);\n"
        "    rollback to savepoint sp1;\n"
        "    commit;\n"
        "    int cnt;\n"
        "    select count(*) into cnt from t;\n"
        "    print int_to_string(cnt);\n"
        "    return 0;\n"
        "}\n",
        args, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "1") != NULL);
}

TEST(phase8_savepoint_release_sqlite) {
    const char* db = "/tmp/test_phase8_release.db";
    unlink(db);
    char out[512];
    char args[256];
    snprintf(args, sizeof(args), "--db %s", db);

    int rc = run_auspex(
        "proc main() -> int {\n"
        "    create table t (id int);\n"
        "    begin;\n"
        "    insert into t values (1);\n"
        "    savepoint sp1;\n"
        "    insert into t values (2);\n"
        "    release savepoint sp1;\n"
        "    commit;\n"
        "    int cnt;\n"
        "    select count(*) into cnt from t;\n"
        "    print int_to_string(cnt);\n"
        "    return 0;\n"
        "}\n",
        args, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "2") != NULL);
}

TEST(phase8_rollback_to_name_sqlite) {
    const char* db = "/tmp/test_phase8_rollback_to.db";
    unlink(db);
    char out[512];
    char args[256];
    snprintf(args, sizeof(args), "--db %s", db);

    int rc = run_auspex(
        "proc main() -> int {\n"
        "    create table t (id int);\n"
        "    begin;\n"
        "    insert into t values (1);\n"
        "    savepoint sp1;\n"
        "    insert into t values (2);\n"
        "    rollback to sp1;\n"
        "    commit;\n"
        "    int cnt;\n"
        "    select count(*) into cnt from t;\n"
        "    print int_to_string(cnt);\n"
        "    return 0;\n"
        "}\n",
        args, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "1") != NULL);
}
#endif

TEST(phase8_autonomous_proc_commits_independently) {
    const char* db = "/tmp/test_phase8_auto.db";
    unlink(db);
    char out[512];
    char args[256];
    snprintf(args, sizeof(args), "--db %s", db);

    /* SQLite allows only one writer per database file, so the caller must not
     * hold a write lock when the autonomous connection writes. We therefore
     * call the autonomous procedure before the caller performs its own write;
     * the semantics remain the same: the autonomous work commits independently
     * and the caller's rollback undoes only the caller's write. */
    int rc = run_auspex(
        "proc auto_insert() -> int {\n"
        "    pragma autonomous_transaction;\n"
        "    insert into t values (2);\n"
        "    return 0;\n"
        "}\n"
        "proc main() -> int {\n"
        "    create table t (id int);\n"
        "    begin;\n"
        "    auto_insert();\n"
        "    insert into t values (1);\n"
        "    rollback;\n"
        "    int cnt;\n"
        "    select count(*) into cnt from t;\n"
        "    print int_to_string(cnt);\n"
        "    return 0;\n"
        "}\n",
        args, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "1") != NULL);
}

TEST(phase8_autonomous_proc_error_does_not_affect_caller) {
    const char* db = "/tmp/test_phase8_auto_err.db";
    unlink(db);
    char out[512];
    char args[256];
    snprintf(args, sizeof(args), "--db %s", db);

    int rc = run_auspex(
        "proc auto_insert_and_raise() -> int {\n"
        "    pragma autonomous_transaction;\n"
        "    my_error exception;\n"
        "    insert into t values (2);\n"
        "    raise my_error;\n"
        "    return 0;\n"
        "}\n"
        "proc main() -> int {\n"
        "    create table t (id int);\n"
        "    begin;\n"
        "    insert into t values (1);\n"
        "    try {\n"
        "        auto_insert_and_raise();\n"
        "    } catch (msg) { }\n"
        "    rollback;\n"
        "    int cnt;\n"
        "    select count(*) into cnt from t;\n"
        "    print int_to_string(cnt);\n"
        "    return 0;\n"
        "}\n",
        args, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "0") != NULL);
}

TEST(phase8_stored_func_persists_across_runs_custom) {
    unlink("auspex.db");
    unlink("auspex.db.programs");
    char out[256];

    int rc = run_auspex(
        "func double(x int) -> int { return x * 2; }\n"
        "proc main() -> int { return double(7); }\n",
        NULL, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "14") != NULL);

    rc = run_auspex(
        "proc main() -> int { return double(5); }\n",
        NULL, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "10") != NULL);
}

TEST(phase8_update_upserts_existing_unit_custom) {
    unlink("auspex.db");
    unlink("auspex.db.programs");
    char out[256];

    int rc = run_auspex(
        "func answer() -> int { return 1; }\n"
        "proc main() -> int { return answer(); }\n",
        NULL, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "1") != NULL);

    rc = run_auspex(
        "func answer() -> int { return 2; }\n"
        "proc main() -> int { return answer(); }\n",
        NULL, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "2") != NULL);
}

TEST(phase8_package_boundary_excluded_custom) {
    unlink("auspex.db");
    unlink("auspex.db.programs");
    char out[256];

    int rc = run_auspex(
        "package body math_pkg is\n"
        "  func hidden(x int) -> int { return x * 2; }\n"
        "end math_pkg;\n"
        "proc main() -> int { return 42; }\n",
        NULL, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "42") != NULL);

    rc = run_auspex(
        "proc main() -> int { return hidden(3); }\n",
        NULL, out, sizeof(out));
    ASSERT_INT_EQ(1, rc);
}

TEST(phase8_complex_body_persists_custom) {
    unlink("auspex.db");
    unlink("auspex.db.programs");
    char out[256];

    int rc = run_auspex(
        "proc complex(n int) -> int {\n"
        "  // { this is a comment with braces }\n"
        "  string s = \"proc func inside string { }\";\n"
        "  if (n > 0) {\n"
        "    return n * 2;\n"
        "  }\n"
        "  return 0;\n"
        "}\n"
        "proc main() -> int { return complex(7); }\n",
        NULL, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "14") != NULL);

    rc = run_auspex(
        "proc main() -> int { return complex(3); }\n",
        NULL, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "6") != NULL);
}

TEST(phase8_autonomous_proc_custom_engine_runs) {
    unlink("auspex.db");
    unlink("auspex.db.programs");
    char out[256];

    int rc = run_auspex(
        "proc auto_add(x int) -> int {\n"
        "    pragma autonomous_transaction;\n"
        "    return x + 1;\n"
        "}\n"
        "proc main() -> int { return auto_add(5); }\n",
        NULL, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "6") != NULL);
}

TEST(phase8_proc_authid_definer_compiles_and_runs) {
    unlink("auspex.db");
    unlink("auspex.db.programs");
    char out[256];

    int rc = run_auspex(
        "proc add(a int, b int) -> int authid definer { return a + b; }\n"
        "proc main() -> int { return add(2, 3); }\n",
        NULL, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "5") != NULL);
}

TEST(phase8_func_authid_current_user_compiles_and_runs) {
    unlink("auspex.db");
    unlink("auspex.db.programs");
    char out[256];

    int rc = run_auspex(
        "func double(x int) -> int authid current_user { return x * 2; }\n"
        "proc main() -> int { return double(7); }\n",
        NULL, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "14") != NULL);
}

TEST(phase8_package_authid_compiles_and_runs) {
    unlink("auspex.db");
    unlink("auspex.db.programs");
    char out[256];

    int rc = run_auspex(
        "package math_pkg is\n"
        "  func answer() -> int authid current_user;\n"
        "end math_pkg;\n"
        "package body math_pkg is\n"
        "  authid definer\n"
        "  func answer() -> int { return 42; }\n"
        "end math_pkg;\n"
        "proc main() -> int { return math_pkg.answer(); }\n",
        NULL, out, sizeof(out));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, strstr(out, "42") != NULL);
}

TEST(phase8_proc_authid_missing_value_errors) {
    unlink("auspex.db");
    unlink("auspex.db.programs");
    char out[256];

    int rc = run_auspex(
        "proc bad() -> int authid { return 0; }\n"
        "proc main() -> int { return bad(); }\n",
        NULL, out, sizeof(out));
    ASSERT_INT_EQ(1, rc != 0);
}

int main(void) {
    RUN_TEST(phase8_stored_proc_persists_across_runs_sqlite);
    RUN_TEST(phase8_stored_func_persists_across_runs_sqlite);
    RUN_TEST(phase8_proc_main_is_not_persisted);
    RUN_TEST(phase8_autonomous_proc_commits_independently);
    RUN_TEST(phase8_autonomous_proc_error_does_not_affect_caller);
#ifdef USE_SQLITE
    RUN_TEST(phase8_authid_definer_is_stored_and_stripped);
    RUN_TEST(phase8_savepoint_rollback_to_sqlite);
    RUN_TEST(phase8_savepoint_release_sqlite);
    RUN_TEST(phase8_rollback_to_name_sqlite);
#endif
    RUN_TEST(phase8_stored_func_persists_across_runs_custom);
    RUN_TEST(phase8_update_upserts_existing_unit_custom);
    RUN_TEST(phase8_package_boundary_excluded_custom);
    RUN_TEST(phase8_complex_body_persists_custom);
    RUN_TEST(phase8_autonomous_proc_custom_engine_runs);
    RUN_TEST(phase8_proc_authid_definer_compiles_and_runs);
    RUN_TEST(phase8_func_authid_current_user_compiles_and_runs);
    RUN_TEST(phase8_package_authid_compiles_and_runs);
    RUN_TEST(phase8_proc_authid_missing_value_errors);
    TEST_SUMMARY();
}
