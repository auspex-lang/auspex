#ifndef MYDB_SQL_ENGINE_H
#define MYDB_SQL_ENGINE_H

#include <sys/types.h>

#include "mydb.h"
#include "compiler.h"

#define MAX_NAME_LEN 255

typedef struct {
    int type; /* VAL_INT, VAL_FLOAT, VAL_STRING */
    union {
        int    as_int;
        double as_float;
        char*  as_string;
    } as;
} Cell;

typedef struct {
    char* name;
    Cell  value;
} Field;

struct Row {
    Field* fields;
    int    field_count;
};

struct Result {
    int   row_count;
    int   current;
    Row*  rows;
};

/* Column constraint flags (persisted in the V3 catalog format). */
#define COL_FLAG_NOT_NULL    1
#define COL_FLAG_PRIMARY_KEY 2 /* implies NOT NULL + UNIQUE */
#define COL_FLAG_UNIQUE      4
#define COL_FLAG_HAS_DEFAULT 8 /* default_value holds the DEFAULT literal */

typedef struct Column {
    char* name;
    int   type;
    int   flags;
    Cell  default_value; /* only meaningful when COL_FLAG_HAS_DEFAULT is set */
} Column;

/* Secondary index metadata. The B-tree itself lives in pager pages;
   root_page is its entry point and is persisted in the catalog. */
typedef struct TableIndex {
    char* name;
    char* column_name;
    int   root_page;
} TableIndex;

typedef struct Table {
    char*       name;
    Column*     columns;
    int         column_count;
    int         first_row_page;
    int         last_row_page;
    TableIndex* indexes;
    int         index_count;
} Table;

struct Context {
    const char*    db_path;
    struct Pager*  pager;
};

/* Catalog lifecycle */
int    catalog_open(Context* ctx);
void   catalog_close(Context* ctx);
void   catalog_clear(Context* ctx);

/* Catalog DDL/DML */
Table* catalog_create_table(Context* ctx, const char* name, const char** columns, int* types, int column_count);
Table* catalog_find_table(Context* ctx, const char* name);
void   catalog_insert(Context* ctx, Table* table, Cell* cells);

/* Views (catalog format V4): a view stores a SELECT text and acts as a
   read-only row source. Returns the stored SELECT, or NULL when no view
   with that name exists. */
const char* catalog_view_query(Context* ctx, const char* name);

/* Sequences (catalog format V5). A sequence persists its name, increment,
   last-issued value, and whether a value was ever issued (has_value). */
#define DB_SEQUENCE_NAME_MAX 64

typedef struct {
    char name[DB_SEQUENCE_NAME_MAX];
    int  has_value;
    int  current;
    int  increment;
} DBSequence;

/* Copies up to `max` catalog sequences into `out`; returns the count. */
int catalog_sequence_list(Context* ctx, DBSequence* out, int max);
/* Creates or updates a sequence and rewrites the catalog page. Returns 0
   (and sets the DDL error) when the catalog page would overflow; the
   in-memory state is left unchanged in that case. */
int catalog_sequence_save(Context* ctx, const DBSequence* seq);
int catalog_sequence_drop(Context* ctx, const char* name);

/* Catalog introspection */
int         catalog_table_count(Context* ctx);
const char* catalog_table_name(Context* ctx, int index);
int         catalog_table_column_count(Context* ctx, int index);
const char* catalog_table_column_name(Context* ctx, int index, int col);
int         catalog_table_column_type(Context* ctx, int index, int col);

/* Converts a cell holding a scalar - int, float, string or bool - into a
   runtime Value. Returns 0 for anything else, NULL included, because callers
   disagree about what a non-scalar should become: the driver reports NULL as
   a value, while the VM's inline column reads substitute int 0. */
int         sql_cell_to_value(const Cell* cell, Value* out);

/* SQL execution */
Result* sql_exec(const char* query, Context* ctx);
int     sql_exec_ddl(const char* query, Context* ctx);

/* Same, with bind values for the statement's `?` placeholders, in order of
   appearance: the first `?` takes params[0], and so on. The number of
   placeholders (outside string literals) must equal param_count. Values may
   appear where a literal can: INSERT VALUES, UPDATE SET, WHERE comparisons,
   IN lists, LIKE patterns and LIMIT. A placeholder count that differs from
   param_count fails up front (NULL / 0); otherwise these behave like sql_exec
   and sql_exec_ddl. Values are copied; the caller keeps ownership of params. */
Result* sql_exec_params(const char* query, Context* ctx, const Value* params, int param_count);
int     sql_exec_ddl_params(const char* query, Context* ctx, const Value* params, int param_count);
Row*    result_next(Result* res);
void    result_free(Result* res);
Cell    row_get_field(Row* row, const char* name);

/* Resolve the type of a column in a SELECT query. */
/* out_type receives one of VAL_INT, VAL_FLOAT, VAL_STRING on success. */
int sql_query_column_type(Context* ctx, const char* query, const char* column_name, int* out_type);

/* -------------------------------------------------------------------------- */
/* Database driver abstraction                                                */
/* -------------------------------------------------------------------------- */

/* Row-level trigger hook (Phase 12 FOR EACH ROW triggers). The VM installs
   this hook on the driver when the active chunk declares row-level triggers;
   DML execution then calls it once per affected row, before and after the
   row is written. old_row/new_row are VAL_ROW Values, or NULL when that
   context does not apply to the event (:old for INSERT, :new for DELETE).
   Returns 1 on success; on 0, error/error_size carry the trigger failure
   message and the DML aborts. */
typedef int (*RowTriggerFn)(void* user, int timing, int event, const char* table,
                            const Value* old_row, const Value* new_row,
                            char* error, size_t error_size);

struct DBDriver {
    void* impl;
    char error_message[256];
    int is_sqlite;
    void (*init)(DBDriver* driver);
    char connection_string[256];
    int (*open)(DBDriver* driver, const char* connection_string);
    void (*close)(DBDriver* driver);
    /* Returns number of rows affected on success (>= 0), or -1 on error. */
    int (*exec)(DBDriver* driver, const char* sql, Value* params, int param_count);
    int (*query)(DBDriver* driver, const char* sql, Value* params, int param_count, void** result_handle);
    int (*result_next)(DBDriver* driver, void* result_handle, void** row_handle);
    int (*row_get_field)(DBDriver* driver, void* row_handle, const char* name, Value* out);
    int (*row_get_column)(DBDriver* driver, void* row_handle, int index, Value* out);
    int (*result_column_count)(DBDriver* driver, void* result_handle);
    const char* (*result_column_name)(DBDriver* driver, void* result_handle, int index);
    void (*result_free)(DBDriver* driver, void* result_handle);
    int (*begin)(DBDriver* driver);
    int (*commit)(DBDriver* driver);
    int (*rollback)(DBDriver* driver);
    int (*savepoint)(DBDriver* driver, const char* name);
    int (*rollback_to_savepoint)(DBDriver* driver, const char* name);
    int (*release_savepoint)(DBDriver* driver, const char* name);
    /* Sequence persistence. Loads copy every stored sequence into `out`
       (up to `max`) and set *out_count. save creates or updates by name;
       drop removes by name. All return 1 on success, 0 on error. */
    int (*sequence_load)(DBDriver* driver, DBSequence* out, int max, int* out_count);
    int (*sequence_save)(DBDriver* driver, const DBSequence* seq);
    int (*sequence_drop)(DBDriver* driver, const char* name);
    /* Row-level trigger hook: installed/cleared by the VM around DML
       execution. NULL means no row-level triggers are active. */
    RowTriggerFn row_trigger_fn;
    void* row_trigger_user;
};

void custom_driver_init(DBDriver* driver);

/* Default custom-engine database: auspex.db, or MyPL's mypl.db (with a
   one-time deprecation warning on stderr) when only that one exists. The
   mypl.db fallback goes away in v0.4.0. */
const char* default_db_path(void);

/* Storage layer */
#define PAGE_SIZE 4096

typedef struct Cursor Cursor;
typedef struct Pager Pager;
typedef struct BTree BTree;

Pager* pager_open(const char* filename);
void   pager_close(Pager* pager);
int    pager_page_count(Pager* pager);
int    pager_allocate_page(Pager* pager);
void   pager_free_page(Pager* pager, int page_num);
void   pager_read_page(Pager* pager, int page_num, uint8_t* out);
void   pager_write_page(Pager* pager, int page_num, const uint8_t* data);

BTree* btree_create(Pager* pager);
BTree* btree_open(Pager* pager, int root_page);
void   btree_destroy(BTree* tree);
/* Returns every page owned by the tree to the pager free list. */
void   btree_free_pages(BTree* tree);
int    btree_root_page(BTree* tree);

/* Only this many leading bytes of a string are significant in an index key.
   Two longer strings that share that prefix encode to the same key, so scans
   over them report extra candidates - never fewer - and callers must re-check
   the predicate against the rows a scan hands back. A bound built from a
   longer literal is likewise only a prefix of the intended bound, so it has to
   be treated as inclusive. */
#define BTREE_STRING_KEY_BYTES 36

/* Keys are Cells (VAL_INT / VAL_FLOAT / VAL_STRING / VAL_NULL). Ordering:
   NULL < int < float < string; ints and floats compare numerically within
   their own key space, strings bytewise up to BTREE_STRING_KEY_BYTES.
   The locator (row_page, row_offset) identifies one row record. */
int    btree_insert(BTree* tree, const Cell* key, int row_page, int row_offset);
/* Removes a single (key, locator) pair, keeping every node but the root at
   least half full by redistributing with a sibling or merging and freeing a
   page. A merge can shorten the tree, so btree_root_page may report a
   different root afterwards and callers that persist it must re-read it.
   Returns 1 when an entry was removed. */
int    btree_delete(BTree* tree, const Cell* key, int row_page, int row_offset);

typedef void (*BTreeScanFn)(int row_page, int row_offset, void* user);
/* Scans return the number of matching entries, or -1 on tree corruption.
   NULL lo/hi bounds are unbounded. */
int    btree_scan_eq(BTree* tree, const Cell* key, BTreeScanFn fn, void* user);
int    btree_scan_range(BTree* tree, const Cell* lo, int lo_inclusive,
                        const Cell* hi, int hi_inclusive, BTreeScanFn fn, void* user);

/* Shape of a tree, for tests and diagnostics. */
typedef struct {
    int height;      /* 1 when the root is a leaf */
    int node_count;  /* pages the tree occupies */
    int leaf_count;
    int entry_count; /* leaf entries, duplicates included */
} BTreeStats;

/* Walks the whole tree. Returns 1 and fills out, or 0 on a corrupt page. */
int    btree_stats(BTree* tree, BTreeStats* out);

int    os_open(const char* path);
int    os_close(int fd);
int    os_read(int fd, void* buf, size_t count, off_t offset);
int    os_write(int fd, const void* buf, size_t count, off_t offset);
int    os_ftruncate(int fd, off_t length);

#endif
