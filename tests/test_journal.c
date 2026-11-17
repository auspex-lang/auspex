/* #113 phase 3: the rollback journal. Unit tests for recovery from
   incomplete or foreign journals, and crash tests that kill a committing
   program at every step (bin/auspex_hooks, built with -DAUSPEX_TEST_HOOKS)
   and check that reopening the database gives exactly the state before
   the commit. */
#include "test_harness.h"
#include "sql_engine.h"
#include "compiler.h"
#include "os.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- journal files written by hand ---- */

static void temp_path(char* buf, size_t size, const char* tag) {
    snprintf(buf, size, "/tmp/auspex_test_journal_%s_%d.db", tag, (int)getpid());
    char journal[300];
    snprintf(journal, sizeof(journal), "%s-journal", buf);
    unlink(buf);
    unlink(journal);
}

static uint32_t fnv(uint32_t seed, const void* data, size_t len) {
    const uint8_t* b = (const uint8_t*)data;
    uint32_t h = 2166136261u ^ seed;
    for (size_t i = 0; i < len; i++) {
        h ^= b[i];
        h *= 16777619u;
    }
    return h;
}

/* A 32-byte journal header in the documented layout, checksum included. */
static void journal_header(uint8_t* h, uint32_t page_size, uint32_t records,
                           uint32_t original_pages, uint64_t nonce) {
    uint32_t version = 1;
    memset(h, 0, 32);
    memcpy(h, "APXJ", 4);
    memcpy(h + 4, &version, 4);
    memcpy(h + 8, &page_size, 4);
    memcpy(h + 12, &records, 4);
    memcpy(h + 16, &original_pages, 4);
    memcpy(h + 20, &nonce, 8);
    uint32_t sum = fnv(0, h, 28);
    memcpy(h + 28, &sum, 4);
}

static int write_bytes(const char* path, const void* data, size_t len) {
    FILE* f = fopen(path, "wb");
    if (f == NULL) return 0;
    int ok = fwrite(data, 1, len, f) == len;
    fclose(f);
    return ok;
}

/* A database with one data page filled with `fill`; returns its number. */
static int make_db(const char* path, uint8_t fill) {
    Pager* pager = pager_open(path);
    if (pager == NULL) return -1;
    int page = pager_allocate_page(pager);
    uint8_t data[PAGE_SIZE];
    memset(data, fill, PAGE_SIZE);
    pager_write_page(pager, page, data);
    pager_close(pager);
    return page;
}

static int page_is(const char* path, int page, uint8_t fill) {
    Pager* pager = pager_open(path);
    if (pager == NULL) return 0;
    uint8_t data[PAGE_SIZE], want[PAGE_SIZE];
    pager_read_page(pager, page, data);
    memset(want, fill, PAGE_SIZE);
    pager_close(pager);
    return memcmp(data, want, PAGE_SIZE) == 0;
}

TEST(journal_commit_leaves_no_journal) {
    char path[200], journal[300];
    temp_path(path, sizeof(path), "clean");
    snprintf(journal, sizeof(journal), "%s-journal", path);
    int page = make_db(path, 0x11);
    Pager* pager = pager_open(path);
    ASSERT_PTR_NOT_NULL(pager);
    ASSERT_INT_EQ(1, pager_begin(pager));
    uint8_t data[PAGE_SIZE];
    memset(data, 0x22, PAGE_SIZE);
    pager_write_page(pager, page, data);
    ASSERT_INT_EQ(0, os_file_exists(journal)); /* only while committing */
    ASSERT_INT_EQ(1, pager_commit(pager));
    ASSERT_INT_EQ(0, os_file_exists(journal));
    pager_close(pager);
    ASSERT_INT_EQ(1, page_is(path, page, 0x22));
    unlink(path);
}

TEST(journal_garbage_is_discarded) {
    char path[200], journal[300];
    temp_path(path, sizeof(path), "garbage");
    snprintf(journal, sizeof(journal), "%s-journal", path);
    int page = make_db(path, 0x33);
    uint8_t junk[5000];
    for (size_t i = 0; i < sizeof(junk); i++) junk[i] = (uint8_t)(i * 31 + 7);
    ASSERT_INT_EQ(1, write_bytes(journal, junk, sizeof(junk)));

    ASSERT_INT_EQ(1, page_is(path, page, 0x33));
    ASSERT_INT_EQ(0, os_file_exists(journal));
    unlink(path);
}

/* The header made it to disk but a record did not: the crash came before
   the journal's fsync finished, so the database was never written. */
TEST(journal_with_torn_record_is_discarded) {
    char path[200], journal[300];
    temp_path(path, sizeof(path), "torn");
    snprintf(journal, sizeof(journal), "%s-journal", path);
    int page = make_db(path, 0x44);

    size_t size = 32 + 4 + PAGE_SIZE + 4;
    uint8_t* j = calloc(1, size);
    ASSERT_PTR_NOT_NULL(j);
    journal_header(j, PAGE_SIZE, 1, (uint32_t)page + 1, 12345);
    uint32_t no = (uint32_t)page;
    memcpy(j + 32, &no, 4);
    memset(j + 36, 0x99, PAGE_SIZE); /* image with a wrong checksum (zero) */
    ASSERT_INT_EQ(1, write_bytes(journal, j, size));
    free(j);

    ASSERT_INT_EQ(1, page_is(path, page, 0x44)); /* not overwritten with 0x99 */
    ASSERT_INT_EQ(0, os_file_exists(journal));
    unlink(path);
}

/* A record count the file is too short for is incomplete, too. */
TEST(journal_shorter_than_its_records_is_discarded) {
    char path[200], journal[300];
    temp_path(path, sizeof(path), "short");
    snprintf(journal, sizeof(journal), "%s-journal", path);
    int page = make_db(path, 0x55);
    uint8_t h[32];
    journal_header(h, PAGE_SIZE, 3, (uint32_t)page + 1, 1);
    ASSERT_INT_EQ(1, write_bytes(journal, h, sizeof(h)));
    ASSERT_INT_EQ(1, page_is(path, page, 0x55));
    ASSERT_INT_EQ(0, os_file_exists(journal));
    unlink(path);
}

/* A valid journal for another page size may be a newer build's: refuse to
   open rather than discard it. */
TEST(journal_for_another_page_size_blocks_open) {
    char path[200], journal[300];
    temp_path(path, sizeof(path), "pagesize");
    snprintf(journal, sizeof(journal), "%s-journal", path);
    ASSERT_INT_EQ(1, make_db(path, 0x66) > 0);
    uint8_t h[32];
    journal_header(h, PAGE_SIZE * 2, 0, 3, 1);
    ASSERT_INT_EQ(1, write_bytes(journal, h, sizeof(h)));

    ASSERT_PTR_NULL(pager_open(path));
    ASSERT_INT_EQ(1, os_file_exists(journal));
    unlink(journal);
    unlink(path);
}

/* A hot journal written by hand: the page goes back, the pages added after
   it are cut off. */
TEST(journal_hot_is_rolled_back) {
    char path[200], journal[300];
    temp_path(path, sizeof(path), "hot");
    snprintf(journal, sizeof(journal), "%s-journal", path);
    int page = make_db(path, 0x77);

    /* What a commit that died half way leaves: the page overwritten and the
       file grown by one page. */
    FILE* f = fopen(path, "r+b");
    ASSERT_PTR_NOT_NULL(f);
    uint8_t data[PAGE_SIZE];
    memset(data, 0x88, PAGE_SIZE);
    fseek(f, (long)page * PAGE_SIZE, SEEK_SET);
    fwrite(data, 1, PAGE_SIZE, f);
    fseek(f, (long)(page + 1) * PAGE_SIZE, SEEK_SET);
    fwrite(data, 1, PAGE_SIZE, f);
    fclose(f);

    size_t size = 32 + 4 + PAGE_SIZE + 4;
    uint8_t* j = calloc(1, size);
    ASSERT_PTR_NOT_NULL(j);
    uint64_t nonce = 0x0123456789abcdefull;
    journal_header(j, PAGE_SIZE, 1, (uint32_t)page + 1, nonce);
    uint32_t no = (uint32_t)page;
    memcpy(j + 32, &no, 4);
    memset(j + 36, 0x77, PAGE_SIZE);
    uint32_t sum = fnv(fnv((uint32_t)nonce ^ (uint32_t)(nonce >> 32), &no, 4), j + 36, PAGE_SIZE);
    memcpy(j + 36 + PAGE_SIZE, &sum, 4);
    ASSERT_INT_EQ(1, write_bytes(journal, j, size));
    free(j);

    ASSERT_INT_EQ(1, page_is(path, page, 0x77));
    ASSERT_INT_EQ(0, os_file_exists(journal));
    FILE* g = fopen(path, "rb");
    ASSERT_PTR_NOT_NULL(g);
    fseek(g, 0, SEEK_END);
    ASSERT_INT_EQ((page + 1) * PAGE_SIZE, (int)ftell(g));
    fclose(g);
    unlink(path);
}

/* ---- crash tests through the CLI ---- */

static const char* SETUP_PROGRAM =
    "proc main() -> int {\n"
    "    create table t (id int, v int);\n"
    "    int i = 0;\n"
    "    while i < 300 {\n"
    "        insert into t values (?i, ?i);\n"
    "        i = i + 1;\n"
    "    }\n"
    "    create index t_id on t (id);\n"
    "    return 0;\n"
    "}\n";

/* One commit that rewrites existing pages (UPDATE, DELETE, index changes)
   and adds new ones (300 inserts split index pages). */
static const char* COMMIT_PROGRAM =
    "proc main() -> int {\n"
    "    begin;\n"
    "    update t set v = 0 where id < 150;\n"
    "    delete from t where id = 5;\n"
    "    int i = 1000;\n"
    "    while i < 1300 {\n"
    "        insert into t values (?i, ?i);\n"
    "        i = i + 1;\n"
    "    }\n"
    "    commit;\n"
    "    print \"committed\";\n"
    "    return 0;\n"
    "}\n";

static char g_dir[256];

static void write_text(const char* name, const char* text) {
    char path[400];
    snprintf(path, sizeof(path), "%s/%s", g_dir, name);
    FILE* f = fopen(path, "w");
    if (f != NULL) {
        fputs(text, f);
        fclose(f);
    }
}

/* Runs `file` in g_dir with bin/auspex_hooks; `crash` is the
   AUSPEX_TEST_CRASH_AT value or NULL. Returns the exit status. */
static int run_in_dir(const char* file, const char* crash) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "root=$(pwd) && cd %s && %s%s \"$root/bin/auspex_hooks\" %s > out.txt 2>&1",
             g_dir, crash != NULL ? "AUSPEX_TEST_CRASH_AT=" : "",
             crash != NULL ? crash : "", file);
    int rc = system(cmd);
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

static void reset_db(void) {
    char cmd[1200];
    snprintf(cmd, sizeof(cmd), "rm -f %s/auspex.db %s/auspex.db-journal %s/auspex.db.packages %s/auspex.db.programs",
             g_dir, g_dir, g_dir, g_dir);
    test_shell(cmd);
}

static void db_file(char* buf, size_t size, const char* suffix) {
    snprintf(buf, size, "%s/auspex.db%s", g_dir, suffix);
}

/* The table's rows as "id:v," in id order, read by a full scan. Also checks
   every row through the index (an equality lookup on id) and returns NULL
   when the two disagree. Caller frees. */
static char* table_state(DBDriver* driver) {
    void* result = NULL;
    if (!driver->query(driver, "SELECT id, v FROM t", NULL, 0, &result)) return NULL;
    int cap = 64, n = 0;
    int* ids = malloc(sizeof(int) * (size_t)cap);
    int* vs = malloc(sizeof(int) * (size_t)cap);
    void* row = NULL;
    while (driver->result_next(driver, result, &row)) {
        Value id, v;
        driver->row_get_column(driver, row, 0, &id);
        driver->row_get_column(driver, row, 1, &v);
        if (n == cap) {
            cap *= 2;
            ids = realloc(ids, sizeof(int) * (size_t)cap);
            vs = realloc(vs, sizeof(int) * (size_t)cap);
        }
        ids[n] = (int)id.as.as_int;
        vs[n] = (int)v.as.as_int;
        n++;
    }
    driver->result_free(driver, result);

    /* Sort by id (insertion sort; a few hundred rows). */
    for (int i = 1; i < n; i++) {
        int id = ids[i], v = vs[i], j = i - 1;
        while (j >= 0 && ids[j] > id) {
            ids[j + 1] = ids[j];
            vs[j + 1] = vs[j];
            j--;
        }
        ids[j + 1] = id;
        vs[j + 1] = v;
    }

    int consistent = 1;
    for (int i = 0; i < n && consistent; i++) {
        Value p = value_int(ids[i]);
        void* r = NULL;
        if (!driver->query(driver, "SELECT v FROM t WHERE id = ?", &p, 1, &r)) {
            consistent = 0;
            break;
        }
        int hits = 0, v = -1;
        while (driver->result_next(driver, r, &row)) {
            Value got;
            driver->row_get_column(driver, row, 0, &got);
            v = (int)got.as.as_int;
            hits++;
        }
        driver->result_free(driver, r);
        if (hits != 1 || v != vs[i]) consistent = 0;
    }

    char* out = NULL;
    if (consistent) {
        out = malloc((size_t)n * 24 + 1);
        size_t len = 0;
        out[0] = '\0';
        for (int i = 0; i < n; i++) {
            len += (size_t)sprintf(out + len, "%d:%d,", ids[i], vs[i]);
        }
    }
    free(ids);
    free(vs);
    return out;
}

/* Opens the database (recovering it), returns its state, and checks it
   still takes writes without pages being handed out twice. */
static char* open_and_check(int* journal_left, int* writable) {
    char path[400], journal[400];
    db_file(path, sizeof(path), "");
    db_file(journal, sizeof(journal), "-journal");
    DBDriver driver;
    custom_driver_init(&driver);
    if (!driver.open(&driver, path)) return NULL;
    *journal_left = os_file_exists(journal);
    char* state = table_state(&driver);

    *writable = 0;
    if (state != NULL) {
        int ok = 1;
        for (int i = 5000; i < 5200 && ok; i++) {
            char sql[80];
            snprintf(sql, sizeof(sql), "INSERT INTO t VALUES (%d, %d)", i, i);
            ok = driver.exec(&driver, sql, NULL, 0) == 1;
        }
        char* after = ok ? table_state(&driver) : NULL;
        *writable = after != NULL && strlen(after) > strlen(state);
        free(after);
    }
    driver.close(&driver);
    return state;
}

static char* g_before = NULL;
static char* g_after = NULL;

TEST(journal_crash_reference_states) {
    reset_db();
    ASSERT_INT_EQ(0, run_in_dir("setup.apx", NULL));
    int left = 0, writable = 0;
    g_before = open_and_check(&left, &writable);
    ASSERT_PTR_NOT_NULL(g_before);

    reset_db();
    ASSERT_INT_EQ(0, run_in_dir("setup.apx", NULL));
    ASSERT_INT_EQ(0, run_in_dir("commit.apx", NULL));
    g_after = open_and_check(&left, &writable);
    ASSERT_PTR_NOT_NULL(g_after);
    ASSERT_INT_EQ(1, strcmp(g_before, g_after) != 0);
}

/* Kills the commit at the n-th `point` for n = 1, 2, ... until the program
   gets through. Every crash must reopen to exactly the state before the
   commit, with no journal left and the database still writable. */
static void crash_at_every(const char* point) {
    if (g_before == NULL || g_after == NULL) {
        ASSERT_INT_EQ(1, 0); /* reference states missing */
        return;
    }
    int crashes = 0;
    for (int n = 1; n <= 400; n++) {
        char spec[64];
        snprintf(spec, sizeof(spec), "%s:%d", point, n);
        reset_db();
        if (run_in_dir("setup.apx", NULL) != 0) {
            ASSERT_INT_EQ(1, 0);
            return;
        }
        int rc = run_in_dir("commit.apx", spec);
        int left = 1, writable = 0;
        char* state = open_and_check(&left, &writable);
        if (state == NULL) {
            fprintf(stderr, "    %s: database unreadable after crash\n", spec);
            ASSERT_PTR_NOT_NULL(state);
            return;
        }
        int same_before = strcmp(g_before, state) == 0;
        int same_after = strcmp(g_after, state) == 0;
        free(state);
        if (rc == 0) {
            ASSERT_INT_EQ(1, same_after);
            ASSERT_INT_EQ(0, left);
            break;
        }
        if (rc != 99 || !same_before || left || !writable) {
            fprintf(stderr, "    %s: exit %d, matches before %d, journal left %d, writable %d\n",
                    spec, rc, same_before, left, writable);
        }
        ASSERT_INT_EQ(99, rc);
        crashes++;
        ASSERT_INT_EQ(1, same_before);
        ASSERT_INT_EQ(0, left);
        ASSERT_INT_EQ(1, writable);
        if (current_test_failed) return;
    }
    ASSERT_INT_EQ(1, crashes > 0);
}

TEST(journal_crash_while_writing_journal) { crash_at_every("journal"); }
TEST(journal_crash_after_journal_sync) { crash_at_every("journal_synced"); }
TEST(journal_crash_while_writing_database) { crash_at_every("db"); }
TEST(journal_crash_with_torn_database_page) { crash_at_every("db_torn"); }
TEST(journal_crash_before_journal_delete) { crash_at_every("db_synced"); }

int main(void) {
    RUN_TEST(journal_commit_leaves_no_journal);
    RUN_TEST(journal_garbage_is_discarded);
    RUN_TEST(journal_with_torn_record_is_discarded);
    RUN_TEST(journal_shorter_than_its_records_is_discarded);
    RUN_TEST(journal_for_another_page_size_blocks_open);
    RUN_TEST(journal_hot_is_rolled_back);

    snprintf(g_dir, sizeof(g_dir), "/tmp/auspex_test_journal_dir_%d", (int)getpid());
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", g_dir, g_dir);
    test_shell(cmd);
    write_text("setup.apx", SETUP_PROGRAM);
    write_text("commit.apx", COMMIT_PROGRAM);
    RUN_TEST(journal_crash_reference_states);
    RUN_TEST(journal_crash_while_writing_journal);
    RUN_TEST(journal_crash_after_journal_sync);
    RUN_TEST(journal_crash_while_writing_database);
    RUN_TEST(journal_crash_with_torn_database_page);
    RUN_TEST(journal_crash_before_journal_delete);
    free(g_before);
    free(g_after);
    snprintf(cmd, sizeof(cmd), "rm -rf %s", g_dir);
    test_shell(cmd);
    TEST_SUMMARY();
}
