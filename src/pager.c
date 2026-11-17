#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

Pager* pager_open(const char* filename) {
    int fd = os_open(filename);
    if (fd < 0) return NULL;

    Pager* pager = calloc(1, sizeof(Pager));
    if (pager == NULL) {
        os_close(fd);
        return NULL;
    }
    pager->fd = fd;

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
    int ok = 1;
    /* In page order, so a file that grows is written front to back. */
    int* pages = pager->dirty_count > 0
                     ? malloc((size_t)pager->dirty_count * sizeof(int)) : NULL;
    if (pager->dirty_count > 0 && pages == NULL) ok = 0;
    int n = 0;
    if (ok) {
        for (int i = 0; i < pager->dirty_capacity; i++) {
            if (pager->dirty[i].page_no >= 0) pages[n++] = pager->dirty[i].page_no;
        }
        qsort(pages, (size_t)n, sizeof(int), compare_ints);
        for (int i = 0; i < n && ok; i++) {
            DirtyPage* slot = dirty_find(pager, pages[i]);
            if (os_write(pager->fd, slot->data, PAGE_SIZE,
                         (off_t)pages[i] * PAGE_SIZE) != PAGE_SIZE) {
                ok = 0;
            }
        }
    }
    free(pages);
    if (ok) {
        write_header(pager);
        if (os_fsync(pager->fd) != 0) ok = 0;
        else pager->synced = 1;
    }
    dirty_clear(pager);
    pager->in_txn = 0;
    if (!ok) {
        /* Without a journal (phase 3) the pages already written stay; at
           least the in-memory header goes back to what the file had. */
        pager->page_count = pager->saved_page_count;
        memcpy(pager->bitmap, pager->saved_bitmap, BITMAP_BYTES);
    }
    return ok;
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
