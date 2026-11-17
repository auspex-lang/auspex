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

struct Pager {
    int     fd;
    int     page_count;
    uint8_t bitmap[BITMAP_BYTES];
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
    }

    return pager;
}

void pager_close(Pager* pager) {
    if (pager == NULL) return;
    write_header(pager);
    os_fsync(pager->fd);
    os_close(pager->fd);
    free(pager);
}

int pager_page_count(Pager* pager) {
    if (pager == NULL) return 0;
    return pager->page_count;
}

int pager_allocate_page(Pager* pager) {
    if (pager == NULL) return -1;
    for (int i = 1; i < BITMAP_BITS; i++) {
        if (!bitmap_test(pager->bitmap, i)) {
            bitmap_set(pager->bitmap, i);
            if (i >= pager->page_count) {
                pager->page_count = i + 1;
            }
            /* Persist the allocation now: if the header waited for
               pager_close, a crash would leave this page marked free on
               disk and it would be handed out twice after reopening. */
            write_header(pager);
            return i;
        }
    }
    return -1;
}

void pager_free_page(Pager* pager, int page_num) {
    if (pager == NULL || page_num <= 0 || page_num >= BITMAP_BITS) return;
    if (!bitmap_test(pager->bitmap, page_num)) return;
    bitmap_clear(pager->bitmap, page_num);
    write_header(pager);
}

void pager_read_page(Pager* pager, int page_num, uint8_t* out) {
    if (pager == NULL || out == NULL) return;
    if (page_num < 0 || page_num >= pager->page_count) {
        memset(out, 0, PAGE_SIZE);
        return;
    }
    if (os_read(pager->fd, out, PAGE_SIZE, (off_t)page_num * PAGE_SIZE) != PAGE_SIZE) {
        memset(out, 0, PAGE_SIZE);
    }
}

void pager_write_page(Pager* pager, int page_num, const uint8_t* data) {
    if (pager == NULL || data == NULL) return;
    if (page_num < 0 || page_num >= BITMAP_BITS) return;

    int extends = page_num >= pager->page_count;
    if (extends) {
        pager->page_count = page_num + 1;
        bitmap_set(pager->bitmap, page_num);
    }

    os_write(pager->fd, data, PAGE_SIZE, (off_t)page_num * PAGE_SIZE);
    /* After the page itself, so the header never claims a page that is
       not in the file yet. */
    if (extends) write_header(pager);
}
