#include "test_harness.h"
#include "sql_engine.h"
#include "os.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#define PAGE_SIZE 4096

static char* make_temp_path(void) {
    char* path = malloc(256);
    if (path == NULL) return NULL;
    snprintf(path, 256, "/tmp/mydb_test_pager_%d.db", (int)getpid());
    unlink(path);
    return path;
}

static void cleanup(const char* path) {
    unlink(path);
    free((void*)path);
}

TEST(pager_creates_file_and_allocates_page) {
    char* path = make_temp_path();
    Pager* pager = pager_open(path);
    ASSERT_PTR_NOT_NULL(pager);

    int page = pager_allocate_page(pager);
    ASSERT_INT_EQ(2, page);

    uint8_t data[PAGE_SIZE];
    memset(data, 0xAB, PAGE_SIZE);
    pager_write_page(pager, page, data);

    uint8_t read[PAGE_SIZE];
    pager_read_page(pager, page, read);
    ASSERT_INT_EQ(0, memcmp(data, read, PAGE_SIZE));

    pager_close(pager);
    cleanup(path);
}

TEST(pager_preserves_data_after_reopen) {
    char* path = make_temp_path();
    Pager* pager = pager_open(path);
    ASSERT_PTR_NOT_NULL(pager);

    int page = pager_allocate_page(pager);
    uint8_t data[PAGE_SIZE];
    memset(data, 0xCD, PAGE_SIZE);
    pager_write_page(pager, page, data);
    pager_close(pager);

    pager = pager_open(path);
    ASSERT_PTR_NOT_NULL(pager);

    uint8_t read[PAGE_SIZE];
    pager_read_page(pager, page, read);
    ASSERT_INT_EQ(0, memcmp(data, read, PAGE_SIZE));

    pager_close(pager);
    cleanup(path);
}

TEST(pager_allocates_multiple_pages_and_tracks_free_list) {
    char* path = make_temp_path();
    Pager* pager = pager_open(path);
    ASSERT_PTR_NOT_NULL(pager);

    int a = pager_allocate_page(pager);
    int b = pager_allocate_page(pager);
    int c = pager_allocate_page(pager);
    ASSERT_INT_EQ(2, a);
    ASSERT_INT_EQ(3, b);
    ASSERT_INT_EQ(4, c);

    pager_free_page(pager, b);

    int d = pager_allocate_page(pager);
    ASSERT_INT_EQ(3, d);

    int e = pager_allocate_page(pager);
    ASSERT_INT_EQ(5, e);

    pager_close(pager);
    cleanup(path);
}

TEST(pager_reports_header_and_catalog_pages_for_new_file) {
    char* path = make_temp_path();
    Pager* pager = pager_open(path);
    ASSERT_PTR_NOT_NULL(pager);
    ASSERT_INT_EQ(2, pager_page_count(pager));
    pager_close(pager);
    cleanup(path);
}

/* What a crash leaves behind: the file as it is on disk while the pager is
   still open, before pager_close writes anything. Copies `path` to `copy`. */
static int snapshot_file(const char* path, const char* copy) {
    char* data = NULL;
    FILE* in = fopen(path, "rb");
    if (in == NULL) return 0;
    fseek(in, 0, SEEK_END);
    long size = ftell(in);
    fseek(in, 0, SEEK_SET);
    data = malloc(size > 0 ? (size_t)size : 1);
    int ok = data != NULL && fread(data, 1, (size_t)size, in) == (size_t)size;
    fclose(in);
    if (ok) {
        FILE* out = fopen(copy, "wb");
        ok = out != NULL && fwrite(data, 1, (size_t)size, out) == (size_t)size;
        if (out != NULL) fclose(out);
    }
    free(data);
    return ok;
}

/* #113 phase 1: the header (page count and allocation bitmap) used to be
   written only by pager_close, so after a crash pages already in use were
   free again and got handed out twice. */
TEST(pager_allocation_survives_crash_before_close) {
    char* path = make_temp_path();
    char copy[300];
    snprintf(copy, sizeof(copy), "%s.crash", path);
    unlink(copy);

    Pager* pager = pager_open(path);
    ASSERT_PTR_NOT_NULL(pager);
    int a = pager_allocate_page(pager);
    int b = pager_allocate_page(pager);
    uint8_t data[PAGE_SIZE];
    memset(data, 0x5A, PAGE_SIZE);
    pager_write_page(pager, a, data);
    pager_write_page(pager, b, data);
    ASSERT_INT_EQ(1, snapshot_file(path, copy));

    Pager* after = pager_open(copy);
    ASSERT_PTR_NOT_NULL(after);
    ASSERT_INT_EQ(pager_page_count(pager), pager_page_count(after));
    /* The next free page is past both, not one of them again. */
    int next = pager_allocate_page(after);
    ASSERT_INT_EQ(1, next != a && next != b);
    pager_close(after);

    pager_close(pager);
    unlink(copy);
    cleanup(path);
}

TEST(pager_free_survives_crash_before_close) {
    char* path = make_temp_path();
    char copy[300];
    snprintf(copy, sizeof(copy), "%s.crash", path);
    unlink(copy);

    /* Both pages allocated and persisted by a clean close... */
    Pager* pager = pager_open(path);
    ASSERT_PTR_NOT_NULL(pager);
    int a = pager_allocate_page(pager);
    int b = pager_allocate_page(pager);
    (void)b;
    pager_close(pager);
    /* ...then one is freed and the process crashes. */
    pager = pager_open(path);
    ASSERT_PTR_NOT_NULL(pager);
    pager_free_page(pager, a);
    ASSERT_INT_EQ(1, snapshot_file(path, copy));

    Pager* after = pager_open(copy);
    ASSERT_PTR_NOT_NULL(after);
    ASSERT_INT_EQ(a, pager_allocate_page(after)); /* freed page is reusable */
    pager_close(after);

    pager_close(pager);
    unlink(copy);
    cleanup(path);
}

TEST(os_fsync_flushes_files_and_rejects_bad_fds) {
    char* path = make_temp_path();
    int fd = os_open(path);
    ASSERT_INT_EQ(1, fd >= 0);
    ASSERT_INT_EQ(4, os_write(fd, "data", 4, 0));
    ASSERT_INT_EQ(0, os_fsync(fd));
    os_close(fd);
    ASSERT_INT_EQ(-1, os_fsync(-1));
    cleanup(path);
}

int main(void) {
    RUN_TEST(pager_creates_file_and_allocates_page);
    RUN_TEST(pager_preserves_data_after_reopen);
    RUN_TEST(pager_allocates_multiple_pages_and_tracks_free_list);
    RUN_TEST(pager_reports_header_and_catalog_pages_for_new_file);
    RUN_TEST(pager_allocation_survives_crash_before_close);
    RUN_TEST(pager_free_survives_crash_before_close);
    RUN_TEST(os_fsync_flushes_files_and_rejects_bad_fds);
    TEST_SUMMARY();
}
