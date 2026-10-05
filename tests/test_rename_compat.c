#include "test_harness.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef USE_SQLITE
#include <sqlite3.h>
#endif

/* ==========================================================================
 * MyPL -> Auspex rename: backward compatibility (v0.2.0 - v0.3.x).
 *
 * Data written by MyPL v0.1.0 must keep working: a mypl.db default database
 * and its sidecars, the old sidecar markers, the _mypl_* SQLite meta tables,
 * and MYPL_INDEX_DEBUG. Every legacy path warns once on stderr. All legacy
 * cases are removed together with the compat code in v0.4.0.
 * ========================================================================== */

#define WORK "/tmp/auspex_rename_compat"

static char g_bin[1024];

static void init_bin(void) {
    if (getcwd(g_bin, sizeof(g_bin) - 16) == NULL) g_bin[0] = '\0';
    strcat(g_bin, "/bin/auspex");
}

static void reset_work(void) {
    system("rm -rf " WORK " && mkdir -p " WORK);
}

static void write_file(const char* path, const char* text) {
    FILE* f = fopen(path, "w");
    if (f == NULL) return;
    fputs(text, f);
    fclose(f);
}

static int read_file_to(const char* path, char* buf, size_t size) {
    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        buf[0] = '\0';
        return -1;
    }
    size_t n = fread(buf, 1, size - 1, f);
    buf[n] = '\0';
    fclose(f);
    return (int)n;
}

/* Runs `source` with the auspex binary inside WORK. `env` is prefixed to the
   command line, `args` goes before the file. stdout and stderr are captured
   separately. */
static int run_in_work(const char* env, const char* args, const char* source,
                       char* out, size_t out_size, char* err, size_t err_size) {
    write_file(WORK "/prog.apx", source);
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "cd " WORK " && %s %s %s prog.apx > out.txt 2> err.txt",
             env != NULL ? env : "", g_bin, args != NULL ? args : "");
    int rc = system(cmd);
    read_file_to(WORK "/out.txt", out, out_size);
    read_file_to(WORK "/err.txt", err, err_size);
    return WEXITSTATUS(rc);
}

static int file_exists(const char* path) {
    return access(path, F_OK) == 0;
}

/* Creates a custom-engine database with one table holding `value`, then
   renames it (and its sidecars) to `name`, so tests can fake a database
   written under either the old or the new default name. */
static void make_custom_db(const char* name, int value) {
    char src[256], cmd[1400];
    snprintf(src, sizeof(src),
             "proc main() -> int {\n"
             "    create table t (v int);\n"
             "    insert into t values (%d);\n"
             "    return 0;\n"
             "}\n", value);
    system("mkdir -p " WORK "/gen && rm -f " WORK "/gen/*");
    write_file(WORK "/gen/gen.apx", src);
    snprintf(cmd, sizeof(cmd), "cd " WORK "/gen && %s gen.apx > /dev/null 2>&1", g_bin);
    system(cmd);
    snprintf(cmd, sizeof(cmd), "mv " WORK "/gen/auspex.db " WORK "/%s", name);
    system(cmd);
}

static const char* READ_T =
    "proc main() -> int {\n"
    "    int v = 0;\n"
    "    SELECT v INTO v FROM t;\n"
    "    print v;\n"
    "    return 0;\n"
    "}\n";

/* ===== New names ===== */

TEST(rename_version_says_auspex) {
    char out[256];
    reset_work();
    char cmd[1200];
    snprintf(cmd, sizeof(cmd), "%s --version > " WORK "/out.txt 2>&1", g_bin);
    int rc = system(cmd);
    ASSERT_INT_EQ(0, WEXITSTATUS(rc));
    read_file_to(WORK "/out.txt", out, sizeof(out));
    ASSERT(strstr(out, "Auspex") != NULL);
}

TEST(rename_default_db_is_auspex_db) {
    char out[256], err[256];
    reset_work();
    int rc = run_in_work(NULL, NULL,
                         "proc main() -> int { create table t (v int); return 0; }\n",
                         out, sizeof(out), err, sizeof(err));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, file_exists(WORK "/auspex.db"));
    ASSERT_INT_EQ(0, file_exists(WORK "/mypl.db"));
}

TEST(rename_programs_sidecar_uses_new_marker) {
    char out[256], err[256], data[4096];
    reset_work();
    int rc = run_in_work(NULL, NULL,
                         "proc helper() -> int { return 7; }\n"
                         "proc main() -> int { return 0; }\n",
                         out, sizeof(out), err, sizeof(err));
    ASSERT_INT_EQ(0, rc);
    ASSERT(read_file_to(WORK "/auspex.db.programs", data, sizeof(data)) > 0);
    ASSERT(strstr(data, "// __AUSPEX_PROGRAM_UNIT__ ") != NULL);
    ASSERT(strstr(data, "__MYPL_") == NULL);
}

/* ===== Legacy default database ===== */

TEST(rename_legacy_db_fallback) {
    char out[256], err[512];
    reset_work();
    make_custom_db("mypl.db", 41);
    int rc = run_in_work(NULL, NULL, READ_T, out, sizeof(out), err, sizeof(err));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(41, atoi(out));
    ASSERT(strstr(err, "legacy database 'mypl.db'") != NULL);
    ASSERT_INT_EQ(0, file_exists(WORK "/auspex.db"));
}

TEST(rename_new_db_preferred_over_legacy) {
    char out[256], err[512];
    reset_work();
    make_custom_db("mypl.db", 41);
    make_custom_db("auspex.db", 42);
    int rc = run_in_work(NULL, NULL, READ_T, out, sizeof(out), err, sizeof(err));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(42, atoi(out));
    ASSERT(strstr(err, "legacy") == NULL);
}

/* ===== Legacy sidecar markers ===== */

TEST(rename_legacy_program_unit_marker_loads) {
    char out[256], err[512];
    reset_work();
    make_custom_db("auspex.db", 1);
    write_file(WORK "/auspex.db.programs",
               "// __MYPL_PROGRAM_UNIT__ PROCEDURE helper\n"
               "proc helper() -> int { return 7; }\n");
    int rc = run_in_work(NULL, NULL,
                         "proc main() -> int { print helper(); return 0; }\n",
                         out, sizeof(out), err, sizeof(err));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(7, atoi(out));
}

TEST(rename_legacy_package_marker_loads) {
    char out[256], err[512];
    reset_work();
    make_custom_db("auspex.db", 1);
    write_file(WORK "/auspex.db.packages",
               "// __MYPL_PACKAGE_SOURCE__\n"
               "package legacy_pkg is\n"
               "    func answer() -> int;\n"
               "end legacy_pkg;\n"
               "package body legacy_pkg is\n"
               "    func answer() -> int { return 99; }\n"
               "end legacy_pkg;\n");
    int rc = run_in_work(NULL, NULL,
                         "proc main() -> int { print legacy_pkg.answer(); return 0; }\n",
                         out, sizeof(out), err, sizeof(err));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(99, atoi(out));
}

/* ===== Legacy debug variable ===== */

static const char* INDEX_PROG =
    "proc main() -> int {\n"
    "    create table ix_t (id int);\n"
    "    insert into ix_t values (1);\n"
    "    insert into ix_t values (2);\n"
    "    create index ix_id on ix_t (id);\n"
    "    int n = 0;\n"
    "    SELECT id INTO n FROM ix_t WHERE id = 2;\n"
    "    print n;\n"
    "    return 0;\n"
    "}\n";

TEST(rename_new_index_debug_env) {
    char out[256], err[1024];
    reset_work();
    int rc = run_in_work("AUSPEX_INDEX_DEBUG=1", NULL, INDEX_PROG,
                         out, sizeof(out), err, sizeof(err));
    ASSERT_INT_EQ(0, rc);
    ASSERT(strstr(err, "index lookup on ix_t(id)") != NULL);
}

TEST(rename_legacy_index_debug_env) {
    char out[256], err[1024];
    reset_work();
    int rc = run_in_work("MYPL_INDEX_DEBUG=1", NULL, INDEX_PROG,
                         out, sizeof(out), err, sizeof(err));
    ASSERT_INT_EQ(0, rc);
    ASSERT(strstr(err, "index lookup on ix_t(id)") != NULL);
}

/* ===== Legacy file extension ===== */

TEST(rename_mypl_extension_still_runs) {
    char cmd[1200], out[256];
    reset_work();
    write_file(WORK "/old.mypl", "proc main() -> int { print 5; return 0; }\n");
    snprintf(cmd, sizeof(cmd), "cd " WORK " && %s old.mypl > out.txt 2>&1", g_bin);
    int rc = system(cmd);
    ASSERT_INT_EQ(0, WEXITSTATUS(rc));
    read_file_to(WORK "/out.txt", out, sizeof(out));
    ASSERT_INT_EQ(5, atoi(out));
}

/* ===== Legacy SQLite meta tables ===== */

#ifdef USE_SQLITE
static int sqlite_table_exists(sqlite3* db, const char* name) {
    sqlite3_stmt* st = NULL;
    int found = 0;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
        found = sqlite3_step(st) == SQLITE_ROW;
    }
    sqlite3_finalize(st);
    return found;
}

TEST(rename_sqlite_legacy_tables_migrated) {
    char out[512], err[1024];
    reset_work();

    sqlite3* db = NULL;
    ASSERT_INT_EQ(SQLITE_OK, sqlite3_open(WORK "/legacy.sqlite", &db));
    const char* setup =
        "CREATE TABLE _mypl_packages (name TEXT PRIMARY KEY, spec_source TEXT, body_source TEXT);"
        "INSERT INTO _mypl_packages VALUES ('legacy_pkg', '', "
        "  'package legacy_pkg is\n    func answer() -> int;\nend legacy_pkg;\n"
        "package body legacy_pkg is\n    func answer() -> int { return 99; }\nend legacy_pkg;\n');"
        "CREATE TABLE _mypl_sequences (name TEXT PRIMARY KEY, has_value INTEGER NOT NULL,"
        "  current INTEGER NOT NULL, increment INTEGER NOT NULL);"
        "INSERT INTO _mypl_sequences VALUES ('legacy_seq', 1, 10, 5);"
        "CREATE TABLE _mypl_program_units (name TEXT NOT NULL, unit_type TEXT NOT NULL,"
        "  source_text TEXT NOT NULL, authid TEXT, PRIMARY KEY (name, unit_type));"
        "INSERT INTO _mypl_program_units VALUES ('helper', 'PROCEDURE',"
        "  'proc helper() -> int { return 7; }', NULL);";
    ASSERT_INT_EQ(SQLITE_OK, sqlite3_exec(db, setup, NULL, NULL, NULL));
    sqlite3_close(db);

    const char* prog =
        "proc main() -> int {\n"
        "    print legacy_pkg.answer();\n"
        "    print helper();\n"
        "    print nextval(\"legacy_seq\");\n"
        "    return 0;\n"
        "}\n";
    int rc = run_in_work(NULL, "--db legacy.sqlite", prog, out, sizeof(out), err, sizeof(err));
    ASSERT_INT_EQ(0, rc);
    ASSERT(strstr(out, "99\n7\n15\n") != NULL);
    ASSERT(strstr(err, "_mypl_") != NULL);

    ASSERT_INT_EQ(SQLITE_OK, sqlite3_open(WORK "/legacy.sqlite", &db));
    ASSERT_INT_EQ(1, sqlite_table_exists(db, "_auspex_packages"));
    ASSERT_INT_EQ(1, sqlite_table_exists(db, "_auspex_sequences"));
    ASSERT_INT_EQ(1, sqlite_table_exists(db, "_auspex_program_units"));
    ASSERT_INT_EQ(0, sqlite_table_exists(db, "_mypl_packages"));
    ASSERT_INT_EQ(0, sqlite_table_exists(db, "_mypl_sequences"));
    ASSERT_INT_EQ(0, sqlite_table_exists(db, "_mypl_program_units"));
    sqlite3_close(db);

    /* Second run: already migrated, so no warning and the state carried on. */
    rc = run_in_work(NULL, "--db legacy.sqlite", prog, out, sizeof(out), err, sizeof(err));
    ASSERT_INT_EQ(0, rc);
    ASSERT(strstr(out, "99\n7\n20\n") != NULL);
    ASSERT(strstr(err, "_mypl_") == NULL);
}
#endif

int main(void) {
    init_bin();
    RUN_TEST(rename_version_says_auspex);
    RUN_TEST(rename_default_db_is_auspex_db);
    RUN_TEST(rename_programs_sidecar_uses_new_marker);
    RUN_TEST(rename_legacy_db_fallback);
    RUN_TEST(rename_new_db_preferred_over_legacy);
    RUN_TEST(rename_legacy_program_unit_marker_loads);
    RUN_TEST(rename_legacy_package_marker_loads);
    RUN_TEST(rename_new_index_debug_env);
    RUN_TEST(rename_legacy_index_debug_env);
    RUN_TEST(rename_mypl_extension_still_runs);
#ifdef USE_SQLITE
    RUN_TEST(rename_sqlite_legacy_tables_migrated);
#endif
    system("rm -rf " WORK);
    TEST_SUMMARY();
}
