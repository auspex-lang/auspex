#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "sql_engine.h"
#include "os.h"

#define PAGER_MAGIC   "MYDB"
#define PAGER_VERSION 1
#define BITMAP_BYTES  (PAGE_SIZE - 16)
#define BITMAP_BITS   (BITMAP_BYTES * 8)

/* One page a transaction has written: its number and current content. */
typedef struct {
    int      page_no; /* -1: empty slot */
    uint8_t* data;
} DirtyPage;

struct Pager {
    int     fd;
    char*   journal_path; /* <db>-journal */
    int     page_count;
    uint8_t bitmap[BITMAP_BYTES];

    /* Transaction state (#113). The dirty pages live in an open-addressing
       hash table keyed by page number; the header (page count and bitmap)
       is snapshotted at begin so rollback can restore it. */
    int        in_txn;
    int        autobegin;
    int        synced; /* nothing written since the last fsync */
    DirtyPage* dirty;
    int        dirty_capacity; /* power of two, or 0 */
    int        dirty_count;
    int        saved_page_count;
    uint8_t    saved_bitmap[BITMAP_BYTES];
};

static void bitmap_set(uint8_t* bitmap, int bit) {
    bitmap[bit / 8] |= (uint8_t)(1 << (bit % 8));
}

static void bitmap_clear(uint8_t* bitmap, int bit) {
    bitmap[bit / 8] &= (uint8_t)~(1 << (bit % 8));
}

static int bitmap_test(const uint8_t* bitmap, int bit) {
    return (bitmap[bit / 8] >> (bit % 8)) & 1;
}

static int read_header(Pager* pager) {
    uint8_t header[PAGE_SIZE];
    if (os_read(pager->fd, header, PAGE_SIZE, 0) != PAGE_SIZE) {
        return 0;
    }

    if (memcmp(header, PAGER_MAGIC, 4) != 0) {
        return 0;
    }

    uint32_t version = 0;
    memcpy(&version, header + 4, sizeof(version));
    if (version != PAGER_VERSION) {
        return 0;
    }

    uint32_t page_size = 0;
    memcpy(&page_size, header + 8, sizeof(page_size));
    if (page_size != PAGE_SIZE) {
        return 0;
    }

    uint32_t page_count = 0;
    memcpy(&page_count, header + 12, sizeof(page_count));
    pager->page_count = (int)page_count;

    memcpy(pager->bitmap, header + 16, BITMAP_BYTES);
    return 1;
}

static void write_header(Pager* pager) {
    uint8_t header[PAGE_SIZE];
    memset(header, 0, PAGE_SIZE);
    memcpy(header, PAGER_MAGIC, 4);

    uint32_t version = PAGER_VERSION;
    memcpy(header + 4, &version, sizeof(version));

    uint32_t page_size = PAGE_SIZE;
    memcpy(header + 8, &page_size, sizeof(page_size));

    uint32_t page_count = (uint32_t)pager->page_count;
    memcpy(header + 12, &page_count, sizeof(page_count));

    memcpy(header + 16, pager->bitmap, BITMAP_BYTES);
    os_write(pager->fd, header, PAGE_SIZE, 0);
    pager->synced = 0;
}

/* ---- dirty page cache ---- */

static unsigned dirty_hash(int page_no, int capacity) {
    return ((unsigned)page_no * 2654435761u) & (unsigned)(capacity - 1);
}

static DirtyPage* dirty_find(Pager* pager, int page_no) {
    if (pager->dirty_capacity == 0) return NULL;
    unsigned i = dirty_hash(page_no, pager->dirty_capacity);
    for (;;) {
        DirtyPage* slot = &pager->dirty[i];
        if (slot->page_no == page_no) return slot;
        if (slot->page_no < 0) return NULL;
        i = (i + 1) & (unsigned)(pager->dirty_capacity - 1);
    }
}

static int dirty_grow(Pager* pager) {
    int capacity = pager->dirty_capacity == 0 ? 64 : pager->dirty_capacity * 2;
    DirtyPage* table = malloc((size_t)capacity * sizeof(DirtyPage));
    if (table == NULL) return 0;
    for (int i = 0; i < capacity; i++) {
        table[i].page_no = -1;
        table[i].data = NULL;
    }
    for (int i = 0; i < pager->dirty_capacity; i++) {
        DirtyPage* old = &pager->dirty[i];
        if (old->page_no < 0) continue;
        unsigned j = dirty_hash(old->page_no, capacity);
        while (table[j].page_no >= 0) j = (j + 1) & (unsigned)(capacity - 1);
        table[j] = *old;
    }
    free(pager->dirty);
    pager->dirty = table;
    pager->dirty_capacity = capacity;
    return 1;
}

/* Stores `data` as the transaction's copy of `page_no`. Returns 0 when out
   of memory. */
static int dirty_put(Pager* pager, int page_no, const uint8_t* data) {
    DirtyPage* slot = dirty_find(pager, page_no);
    if (slot != NULL) {
        memcpy(slot->data, data, PAGE_SIZE);
        return 1;
    }
    /* Keep the load factor at or below one half. */
    if ((pager->dirty_count + 1) * 2 > pager->dirty_capacity && !dirty_grow(pager)) {
        return 0;
    }
    uint8_t* copy = malloc(PAGE_SIZE);
    if (copy == NULL) return 0;
    memcpy(copy, data, PAGE_SIZE);
    unsigned i = dirty_hash(page_no, pager->dirty_capacity);
    while (pager->dirty[i].page_no >= 0) {
        i = (i + 1) & (unsigned)(pager->dirty_capacity - 1);
    }
    pager->dirty[i].page_no = page_no;
    pager->dirty[i].data = copy;
    pager->dirty_count++;
    return 1;
}

static void dirty_clear(Pager* pager) {
    for (int i = 0; i < pager->dirty_capacity; i++) {
        free(pager->dirty[i].data);
    }
    free(pager->dirty);
    pager->dirty = NULL;
    pager->dirty_capacity = 0;
    pager->dirty_count = 0;
}

static int compare_ints(const void* a, const void* b) {
    int x = *(const int*)a;
    int y = *(const int*)b;
    return (x > y) - (x < y);
}

/* Called before every change. Opens the transaction autobegin asks for. */
static void begin_if_auto(Pager* pager) {
    if (!pager->in_txn && pager->autobegin) pager_begin(pager);
}

/* ---- test crash hook ---- */

/* Built with -DAUSPEX_TEST_HOOKS only (bin/auspex_hooks, for the crash
   tests): AUSPEX_TEST_CRASH_AT=<point>:<n> ends the process the n-th time
   it reaches <point>, leaving the files as a crash would. */
#ifdef AUSPEX_TEST_HOOKS
static int crash_due(const char* point) {
    static int hits = 0;
    const char* spec = getenv("AUSPEX_TEST_CRASH_AT");
    if (spec == NULL) return 0;
    const char* colon = strchr(spec, ':');
    if (colon == NULL) return 0;
    size_t len = (size_t)(colon - spec);
    if (strlen(point) != len || strncmp(spec, point, len) != 0) return 0;
    return ++hits == atoi(colon + 1);
}
#define CRASH_EXIT() _Exit(99)
#else
#define crash_due(point) 0
#define CRASH_EXIT() ((void)0)
#endif

/* ---- rollback journal (#113 phase 3) ----

   <db>-journal exists only while a commit runs. It holds the original
   image of every page the commit overwrites, so a commit that dies half
   way can be undone the next time the database opens.

   header (32 bytes): "APXJ" | u32 version | u32 page_size | u32 record_count
                      | u32 original_page_count | u64 nonce | u32 checksum
   record:            u32 page_no | page image | u32 checksum

   Commit: write the records, then the header, fsync the journal and its
   directory; write the pages and the database header, fsync the database;
   delete the journal and fsync the directory. Deleting the journal is the
   commit point. A journal that does not verify was never complete, so the
   database was not touched yet and the journal is simply deleted. */

#define JOURNAL_MAGIC   "APXJ"
#define JOURNAL_VERSION 1
#define JOURNAL_HEADER  32
#define JOURNAL_RECORD  (4 + PAGE_SIZE + 4)

static uint32_t checksum(uint32_t seed, const void* data, size_t len) {
    /* FNV-1a, seeded so a record cannot verify under another journal's
       nonce. */
    const uint8_t* bytes = (const uint8_t*)data;
    uint32_t h = 2166136261u ^ seed;
    for (size_t i = 0; i < len; i++) {
        h ^= bytes[i];
        h *= 16777619u;
    }
    return h;
}

static uint32_t record_checksum(uint64_t nonce, uint32_t page_no, const uint8_t* image) {
    uint32_t h = checksum((uint32_t)nonce ^ (uint32_t)(nonce >> 32), &page_no, sizeof(page_no));
    return checksum(h, image, PAGE_SIZE);
}

static off_t file_size(int fd) {
    return lseek(fd, 0, SEEK_END);
}

static int pages_in_file(int fd) {
    off_t size = file_size(fd);
    if (size <= 0) return 0;
    return (int)((size + PAGE_SIZE - 1) / PAGE_SIZE);
}

static void remove_journal(const char* journal_path) {
    os_remove(journal_path);
    os_fsync_dir(journal_path);
}

/* Undoes an interrupted commit from `journal_path`, if there is one.
   Returns 1 when the database can be opened (no journal, a journal that
   was never complete, or a successful rollback) and 0 when it cannot: a
   journal from another version or page size, or an I/O error while
   restoring. The journal is left in place then. */
static int journal_recover(int db_fd, const char* journal_path) {
    if (!os_file_exists(journal_path)) return 1;
    int jfd = os_open(journal_path);
    if (jfd < 0) return 0;

    uint8_t header[JOURNAL_HEADER];
    off_t jsize = file_size(jfd);
    int complete = jsize >= JOURNAL_HEADER &&
                   os_read(jfd, header, JOURNAL_HEADER, 0) == JOURNAL_HEADER &&
                   memcmp(header, JOURNAL_MAGIC, 4) == 0;
    uint32_t version = 0, page_size = 0, records = 0, original_pages = 0, stored = 0;
    uint64_t nonce = 0;
    if (complete) {
        memcpy(&version, header + 4, 4);
        memcpy(&page_size, header + 8, 4);
        memcpy(&records, header + 12, 4);
        memcpy(&original_pages, header + 16, 4);
        memcpy(&nonce, header + 20, 8);
        memcpy(&stored, header + 28, 4);
        complete = checksum(0, header, 28) == stored;
    }
    if (complete && (version != JOURNAL_VERSION || page_size != PAGE_SIZE)) {
        /* Not ours to discard: a newer build may know how to replay it. */
        os_close(jfd);
        return 0;
    }
    if (complete) {
        complete = jsize >= (off_t)JOURNAL_HEADER + (off_t)records * JOURNAL_RECORD;
    }

    uint8_t* record = complete ? malloc(JOURNAL_RECORD) : NULL;
    if (complete && record == NULL) {
        os_close(jfd);
        return 0;
    }
    /* Verify every record before touching the database. */
    for (uint32_t i = 0; complete && i < records; i++) {
        off_t at = (off_t)JOURNAL_HEADER + (off_t)i * JOURNAL_RECORD;
        uint32_t page_no = 0, sum = 0;
        if (os_read(jfd, record, JOURNAL_RECORD, at) != JOURNAL_RECORD) {
            complete = 0;
            break;
        }
        memcpy(&page_no, record, 4);
        memcpy(&sum, record + 4 + PAGE_SIZE, 4);
        if (record_checksum(nonce, page_no, record + 4) != sum) complete = 0;
    }

    int ok = 1;
    if (complete) {
        for (uint32_t i = 0; ok && i < records; i++) {
            off_t at = (off_t)JOURNAL_HEADER + (off_t)i * JOURNAL_RECORD;
            uint32_t page_no = 0;
            if (os_read(jfd, record, JOURNAL_RECORD, at) != JOURNAL_RECORD) {
                ok = 0;
                break;
            }
            memcpy(&page_no, record, 4);
            if (os_write(db_fd, record + 4, PAGE_SIZE, (off_t)page_no * PAGE_SIZE) != PAGE_SIZE) {
                ok = 0;
            }
        }
        if (ok && os_ftruncate(db_fd, (off_t)original_pages * PAGE_SIZE) != 0) ok = 0;
        if (ok && os_fsync(db_fd) != 0) ok = 0;
    }
    free(record);
    os_close(jfd);
    if (!ok) return 0;
    remove_journal(journal_path);
    return 1;
}

static uint64_t make_nonce(const Pager* pager) {
    static uint64_t counter = 0;
    uint64_t n = (uint64_t)time(NULL) ^ ((uint64_t)getpid() << 32) ^
                 (uint64_t)(uintptr_t)pager ^ (++counter * 0x9E3779B97F4A7C15ull);
    return n;
}

/* Writes the journal for the open transaction: the on-disk image of page 0
   and of every dirty page the file already has. Returns the journal's fd,
   or -1 on failure (nothing then exists on disk). `pages` holds the n
   dirty page numbers in ascending order. */
static int journal_write(Pager* pager, const int* pages, int n) {
    int original_pages = pages_in_file(pager->fd);
    int jfd = os_open(pager->journal_path);
    if (jfd < 0) return -1;
    if (os_ftruncate(jfd, 0) != 0) {
        os_close(jfd);
        remove_journal(pager->journal_path);
        return -1;
    }

    uint64_t nonce = make_nonce(pager);
    uint8_t* record = malloc(JOURNAL_RECORD);
    int ok = record != NULL;
    uint32_t count = 0;
    /* Page 0 first (the header always changes), then the dirty pages. */
    for (int i = -1; ok && i < n; i++) {
        int page_no = i < 0 ? 0 : pages[i];
        if (i >= 0 && page_no == 0) continue;
        if (page_no >= original_pages) continue; /* new: truncation undoes it */
        uint32_t no = (uint32_t)page_no;
        memcpy(record, &no, 4);
        if (os_read(pager->fd, record + 4, PAGE_SIZE, (off_t)page_no * PAGE_SIZE) != PAGE_SIZE) {
            memset(record + 4, 0, PAGE_SIZE);
        }
        uint32_t sum = record_checksum(nonce, no, record + 4);
        memcpy(record + 4 + PAGE_SIZE, &sum, 4);
        off_t at = (off_t)JOURNAL_HEADER + (off_t)count * JOURNAL_RECORD;
        if (os_write(jfd, record, JOURNAL_RECORD, at) != JOURNAL_RECORD) ok = 0;
        count++;
        if (crash_due("journal")) CRASH_EXIT();
    }
    free(record);

    if (ok) {
        uint8_t header[JOURNAL_HEADER];
        memset(header, 0, sizeof(header));
        uint32_t version = JOURNAL_VERSION, page_size = PAGE_SIZE;
        uint32_t original = (uint32_t)original_pages;
        memcpy(header, JOURNAL_MAGIC, 4);
        memcpy(header + 4, &version, 4);
        memcpy(header + 8, &page_size, 4);
        memcpy(header + 12, &count, 4);
        memcpy(header + 16, &original, 4);
        memcpy(header + 20, &nonce, 8);
        uint32_t sum = checksum(0, header, 28);
        memcpy(header + 28, &sum, 4);
        /* The header goes last: until it is on disk the journal does not
           verify and is discarded on open. */
        if (os_write(jfd, header, JOURNAL_HEADER, 0) != JOURNAL_HEADER) ok = 0;
    }
    if (ok && os_fsync(jfd) != 0) ok = 0;
    if (ok && os_fsync_dir(pager->journal_path) != 0) ok = 0;
    if (!ok) {
        os_close(jfd);
        remove_journal(pager->journal_path);
        return -1;
    }
    if (crash_due("journal_synced")) CRASH_EXIT();
    return jfd;
}

Pager* pager_open(const char* filename) {
    int fd = os_open(filename);
    if (fd < 0) return NULL;

    Pager* pager = calloc(1, sizeof(Pager));
    size_t name_len = strlen(filename);
    char* journal_path = malloc(name_len + sizeof("-journal"));
    if (pager == NULL || journal_path == NULL) {
        free(pager);
        free(journal_path);
        os_close(fd);
        return NULL;
    }
    memcpy(journal_path, filename, name_len);
    memcpy(journal_path + name_len, "-journal", sizeof("-journal"));
    pager->fd = fd;
    pager->journal_path = journal_path;

    /* Before the header is read: recovery may restore the header page. */
    if (!journal_recover(fd, journal_path)) {
        free(journal_path);
        free(pager);
        os_close(fd);
        return NULL;
    }

    off_t size = lseek(fd, 0, SEEK_END);
    if (size < 0) {
        free(pager);
        os_close(fd);
        return NULL;
    }

    if (size == 0) {
        /* New database: initialize header page and reserve page 1 for catalog. */
        pager->page_count = 2;
        memset(pager->bitmap, 0, BITMAP_BYTES);
        bitmap_set(pager->bitmap, 0);
        bitmap_set(pager->bitmap, 1);
        write_header(pager);
    } else {
        if (!read_header(pager)) {
            free(pager->journal_path);
            free(pager);
            os_close(fd);
            return NULL;
        }
        pager->synced = 1;
    }

    return pager;
}

void pager_close(Pager* pager) {
    if (pager == NULL) return;
    if (pager->in_txn) {
        /* Commit writes the header and fsyncs; on failure there is nothing
           better to do than close. */
        pager_commit(pager);
    } else if (!pager->synced) {
        write_header(pager);
        os_fsync(pager->fd);
    }
    dirty_clear(pager);
    os_close(pager->fd);
    free(pager->journal_path);
    free(pager);
}

int pager_page_count(Pager* pager) {
    if (pager == NULL) return 0;
    return pager->page_count;
}

int pager_allocate_page(Pager* pager) {
    if (pager == NULL) return -1;
    begin_if_auto(pager);
    for (int i = 1; i < BITMAP_BITS; i++) {
        if (!bitmap_test(pager->bitmap, i)) {
            bitmap_set(pager->bitmap, i);
            if (i >= pager->page_count) {
                pager->page_count = i + 1;
            }
            /* Persist the allocation now: if the header waited for
               pager_close, a crash would leave this page marked free on
               disk and it would be handed out twice after reopening.
               Inside a transaction, commit writes it. */
            if (!pager->in_txn) write_header(pager);
            return i;
        }
    }
    return -1;
}

void pager_free_page(Pager* pager, int page_num) {
    if (pager == NULL || page_num <= 0 || page_num >= BITMAP_BITS) return;
    if (!bitmap_test(pager->bitmap, page_num)) return;
    begin_if_auto(pager);
    bitmap_clear(pager->bitmap, page_num);
    if (!pager->in_txn) write_header(pager);
}

void pager_read_page(Pager* pager, int page_num, uint8_t* out) {
    if (pager == NULL || out == NULL) return;
    if (page_num < 0 || page_num >= pager->page_count) {
        memset(out, 0, PAGE_SIZE);
        return;
    }
    if (pager->in_txn) {
        DirtyPage* slot = dirty_find(pager, page_num);
        if (slot != NULL) {
            memcpy(out, slot->data, PAGE_SIZE);
            return;
        }
    }
    if (os_read(pager->fd, out, PAGE_SIZE, (off_t)page_num * PAGE_SIZE) != PAGE_SIZE) {
        memset(out, 0, PAGE_SIZE);
    }
}

void pager_write_page(Pager* pager, int page_num, const uint8_t* data) {
    if (pager == NULL || data == NULL) return;
    if (page_num < 0 || page_num >= BITMAP_BITS) return;
    begin_if_auto(pager);

    if (pager->in_txn && page_num < pager->page_count &&
        dirty_find(pager, page_num) == NULL) {
        /* Rewriting a page with what it already holds changes nothing; not
           caching it lets a transaction that only did that commit without
           any I/O (catalog_close rewrites the catalog page on every close). */
        uint8_t current[PAGE_SIZE];
        if (os_read(pager->fd, current, PAGE_SIZE, (off_t)page_num * PAGE_SIZE) == PAGE_SIZE &&
            memcmp(current, data, PAGE_SIZE) == 0) {
            return;
        }
    }

    if (pager->in_txn) {
        /* Out of memory: write through rather than lose the page. The
           transaction can then no longer be rolled back cleanly, but the
           data the caller asked for is kept. */
        if (dirty_put(pager, page_num, data)) {
            if (page_num >= pager->page_count) {
                pager->page_count = page_num + 1;
                bitmap_set(pager->bitmap, page_num);
            }
            return;
        }
    }

    int extends = page_num >= pager->page_count;
    if (extends) {
        pager->page_count = page_num + 1;
        bitmap_set(pager->bitmap, page_num);
    }

    os_write(pager->fd, data, PAGE_SIZE, (off_t)page_num * PAGE_SIZE);
    pager->synced = 0;
    /* After the page itself, so the header never claims a page that is
       not in the file yet. */
    if (extends && !pager->in_txn) write_header(pager);
}

/* ---- transactions ---- */

int pager_begin(Pager* pager) {
    if (pager == NULL || pager->in_txn) return 0;
    pager->saved_page_count = pager->page_count;
    memcpy(pager->saved_bitmap, pager->bitmap, BITMAP_BYTES);
    pager->in_txn = 1;
    return 1;
}

int pager_commit(Pager* pager) {
    if (pager == NULL || !pager->in_txn) return 0;
    if (pager->dirty_count == 0 && pager->page_count == pager->saved_page_count &&
        memcmp(pager->bitmap, pager->saved_bitmap, BITMAP_BYTES) == 0) {
        pager->in_txn = 0; /* nothing to write */
        return 1;
    }
    /* In page order, so a file that grows is written front to back. */
    int n = 0;
    int* pages = pager->dirty_count > 0
                     ? malloc((size_t)pager->dirty_count * sizeof(int)) : NULL;
    int ok = pager->dirty_count == 0 || pages != NULL;
    if (ok) {
        for (int i = 0; i < pager->dirty_capacity; i++) {
            if (pager->dirty[i].page_no >= 0) pages[n++] = pager->dirty[i].page_no;
        }
        qsort(pages, (size_t)n, sizeof(int), compare_ints);
    }

    int jfd = ok ? journal_write(pager, pages, n) : -1;
    if (jfd < 0) ok = 0;
    int touched = 0; /* the database file was written to */
    for (int i = 0; ok && i < n; i++) {
        DirtyPage* slot = dirty_find(pager, pages[i]);
        if (crash_due("db_torn")) {
            os_write(pager->fd, slot->data, PAGE_SIZE / 2, (off_t)pages[i] * PAGE_SIZE);
            CRASH_EXIT();
        }
        touched = 1;
        if (os_write(pager->fd, slot->data, PAGE_SIZE,
                     (off_t)pages[i] * PAGE_SIZE) != PAGE_SIZE) {
            ok = 0;
        }
        if (crash_due("db")) CRASH_EXIT();
    }
    free(pages);
    if (ok) {
        touched = 1;
        write_header(pager);
        if (os_fsync(pager->fd) != 0) ok = 0;
    }
    if (ok && crash_due("db_synced")) CRASH_EXIT();
    if (jfd >= 0) os_close(jfd);

    dirty_clear(pager);
    pager->in_txn = 0;
    if (ok) {
        remove_journal(pager->journal_path); /* the commit point */
        pager->synced = 1;
        return 1;
    }
    /* Failed: put the file back as it was and the header with it. */
    if (touched) journal_recover(pager->fd, pager->journal_path);
    else if (jfd >= 0) remove_journal(pager->journal_path);
    pager->page_count = pager->saved_page_count;
    memcpy(pager->bitmap, pager->saved_bitmap, BITMAP_BYTES);
    return 0;
}

void pager_rollback(Pager* pager) {
    if (pager == NULL || !pager->in_txn) return;
    dirty_clear(pager);
    pager->page_count = pager->saved_page_count;
    memcpy(pager->bitmap, pager->saved_bitmap, BITMAP_BYTES);
    pager->in_txn = 0;
}

int pager_in_transaction(Pager* pager) {
    return pager != NULL && pager->in_txn;
}

int pager_dirty_count(Pager* pager) {
    return pager != NULL ? pager->dirty_count : 0;
}

void pager_set_autobegin(Pager* pager, int on) {
    if (pager != NULL) pager->autobegin = on ? 1 : 0;
}
