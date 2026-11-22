#include "test_harness.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ==========================================================================
 * Product naming: the binary, the default database, the sidecar markers and
 * the debug variable all use the Auspex names.
 * ========================================================================== */

#define WORK "/tmp/auspex_naming_compat"

/* Commands cd into WORK, so the binary needs an absolute path. The shell
   computes it: on Windows getcwd() would return a backslashed path that
   MSYS2's bash mangles. Every command line starts with ROOT. */
#define ROOT "R=$(pwd) && "
#define BIN "\"$R/bin/auspex\""

static void reset_work(void) {
    test_shell("rm -rf " WORK " && mkdir -p " WORK);
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
             ROOT "cd " WORK " && %s " BIN " %s prog.apx > out.txt 2> err.txt",
             env != NULL ? env : "", args != NULL ? args : "");
    int rc = system(cmd);
    read_file_to(WORK "/out.txt", out, out_size);
    read_file_to(WORK "/err.txt", err, err_size);
    return WEXITSTATUS(rc);
}

static int file_exists(const char* path) {
    return access(path, F_OK) == 0;
}

TEST(naming_version_says_auspex) {
    char out[256];
    reset_work();
    int rc = system(ROOT BIN " --version > " WORK "/out.txt 2>&1");
    ASSERT_INT_EQ(0, WEXITSTATUS(rc));
    read_file_to(WORK "/out.txt", out, sizeof(out));
    ASSERT(strstr(out, "Auspex") != NULL);
}

TEST(naming_default_db_is_auspex_db) {
    char out[256], err[256];
    reset_work();
    int rc = run_in_work(NULL, NULL,
                         "proc main() -> int { create table t (v int); return 0; }\n",
                         out, sizeof(out), err, sizeof(err));
    ASSERT_INT_EQ(0, rc);
    ASSERT_INT_EQ(1, file_exists(WORK "/auspex.db"));
}

TEST(naming_programs_sidecar_uses_new_marker) {
    char out[256], err[256], data[4096];
    reset_work();
    int rc = run_in_work(NULL, NULL,
                         "proc helper() -> int { return 7; }\n"
                         "proc main() -> int { return 0; }\n",
                         out, sizeof(out), err, sizeof(err));
    ASSERT_INT_EQ(0, rc);
    ASSERT(read_file_to(WORK "/auspex.db.programs", data, sizeof(data)) > 0);
    ASSERT(strstr(data, "// __AUSPEX_PROGRAM_UNIT__ ") != NULL);
}

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

TEST(naming_new_index_debug_env) {
    char out[256], err[1024];
    reset_work();
    int rc = run_in_work("AUSPEX_INDEX_DEBUG=1", NULL, INDEX_PROG,
                         out, sizeof(out), err, sizeof(err));
    ASSERT_INT_EQ(0, rc);
    ASSERT(strstr(err, "index lookup on ix_t(id)") != NULL);
}

int main(void) {
    RUN_TEST(naming_version_says_auspex);
    RUN_TEST(naming_default_db_is_auspex_db);
    RUN_TEST(naming_programs_sidecar_uses_new_marker);
    RUN_TEST(naming_new_index_debug_env);
    test_shell("rm -rf " WORK);
    TEST_SUMMARY();
}
