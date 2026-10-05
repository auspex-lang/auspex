#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sql_engine.h"
#include "os.h"
#include "ast.h"   /* TRIGGER_BEFORE/AFTER/INSERT/UPDATE/DELETE constants */

/* -------------------------------------------------------------------------- */
/* In-memory catalog cache                                                    */
/* -------------------------------------------------------------------------- */

#define MAX_CATALOG_TABLES 64
#define MAX_TABLE_INDEXES 8
#define MAX_CATALOG_VIEWS 16
/* Stored view SELECT text is capped so several views fit the catalog page. */
#define MAX_VIEW_QUERY_LEN 768
#define MAX_CATALOG_SEQUENCES 16

static Table* g_catalog[MAX_CATALOG_TABLES];
static int    g_catalog_count = 0;
static int    g_catalog_page  = -1;

typedef struct {
    char* name;
    char* select_query;
} ViewDef;

static ViewDef g_views[MAX_CATALOG_VIEWS];
static int     g_view_count = 0;

typedef struct {
    char* name;
    int   has_value;
    int   current;
    int   increment;
} SequenceDef;

static SequenceDef g_sequences[MAX_CATALOG_SEQUENCES];
static int         g_sequence_count = 0;

/* VAL_DATE and VAL_TIMESTAMP are text-backed exactly as VAL_STRING is: the
   cell owns a heap char*. Everything that frees, copies or zero-fills a cell
   asks this rather than testing for VAL_STRING, so a text-backed column added
   later cannot leak its buffer in one of the thirty-odd places that would
   otherwise need updating. Places that genuinely mean "is a string" - ordering
   and index eligibility - still test the type directly. */
static int cell_owns_text(int type) {
    return type == VAL_STRING || type == VAL_DATE || type == VAL_TIMESTAMP;
}

/* The canonical text a date or timestamp is stored as, which is what to_date,
   current_date and current_timestamp produce: YYYY-MM-DD, optionally followed
   by ' HH:MM:SS'. Both are fixed width and order chronologically under a plain
   byte comparison, which is what lets them share the string key space in an
   index and compare with strcmp. */
static int text_is_datetime(const char* s, int want_time) {
    if (s == NULL) return 0;
    size_t len = strlen(s);
    if (len != (want_time ? 19u : 10u)) return 0;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (i == 4 || i == 7) {
            if (c != '-') return 0;
        } else if (i == 10) {
            if (c != ' ') return 0;
        } else if (i == 13 || i == 16) {
            if (c != ':') return 0;
        } else if (c < '0' || c > '9') {
            return 0;
        }
    }
    return 1;
}

/* A date or timestamp is written as a string literal - the column type is what
   gives it its type, the way an int literal becomes a float in a float column.
   Retags the cell in place. Returns 0 when the text is not in the canonical
   format, so a bad literal is rejected at write time rather than stored and
   read back as something that is not a date. */
static int cell_coerce_to_column(Cell* cell, int column_type) {
    if (cell == NULL) return 0;
    if (column_type != VAL_DATE && column_type != VAL_TIMESTAMP) return 1;
    if (cell->type == VAL_NULL || cell->type == column_type) return 1;
    if (cell->type != VAL_STRING) return 0;
    if (!text_is_datetime(cell->as.as_string, column_type == VAL_TIMESTAMP)) return 0;
    cell->type = column_type;
    return 1;
}

static void free_column(Column* col) {
    free(col->name);
    if ((col->flags & COL_FLAG_HAS_DEFAULT) && cell_owns_text(col->default_value.type)) {
        free(col->default_value.as.as_string);
    }
}

static void free_table(Table* t) {
    if (t == NULL) return;
    for (int i = 0; i < t->column_count; i++) {
        free_column(&t->columns[i]);
    }
    free(t->columns);
    for (int i = 0; i < t->index_count; i++) {
        free(t->indexes[i].name);
        free(t->indexes[i].column_name);
    }
    free(t->indexes);
    free(t->name);
    free(t);
}

void catalog_clear(Context* ctx) {
    (void)ctx;
    for (int i = 0; i < g_catalog_count; i++) {
        free_table(g_catalog[i]);
        g_catalog[i] = NULL;
    }
    g_catalog_count = 0;
    g_catalog_page = -1;
    for (int i = 0; i < g_view_count; i++) {
        free(g_views[i].name);
        free(g_views[i].select_query);
        g_views[i].name = NULL;
        g_views[i].select_query = NULL;
    }
    g_view_count = 0;
    for (int i = 0; i < g_sequence_count; i++) {
        free(g_sequences[i].name);
        g_sequences[i].name = NULL;
    }
    g_sequence_count = 0;
}

/* -------------------------------------------------------------------------- */
/* Catalog serialization                                                      */
/* -------------------------------------------------------------------------- */

/* Catalog page formats. V1 (legacy): u32 table_count followed by per-table
   records with no index data. V2: magic marker, then table_count, then
   per-table records each carrying their index list. V3: like V2, plus
   per-column constraint flags and an optional DEFAULT literal. V4: like V3,
   plus a view section after the table records (u8 view_count, then per view
   a u8 name + u16 SELECT text). V5: like V4, plus a sequence section after
   the view section (u8 sequence_count, then per sequence a u8 name, u8
   has_value, i32 current, i32 increment). V1–V4 files still load — they
   simply have no indexes and/or no constraints and/or no views and/or no
   sequences; the page is rewritten as V6 on the next save. V6: same layout as
   V5, but a column type may now be one of the extended scalars (bool today)
   as well as int, float and string. The layout is unchanged - the marker is
   what tells an older reader that a type tag it does not know may appear,
   rather than letting it silently mis-handle one. */
#define CATALOG_MAGIC_V2 0x4D594932u /* "MYI2" */
#define CATALOG_MAGIC_V3 0x4D594333u /* "MYC3" */
#define CATALOG_MAGIC_V4 0x4D595634u /* "MYV4" */
#define CATALOG_MAGIC_V5 0x4D595635u /* "MYV5" */
#define CATALOG_MAGIC_V6 0x4D595636u /* "MYV6" */

/* Reads a serialized cell (type tag + payload) from the catalog page. */
static int catalog_read_cell(uint8_t* page, int* offset, Cell* cell) {
    cell->type = page[(*offset)++];
    switch (cell->type) {
        case VAL_NULL:
            cell->as.as_int = 0;
            return 1;
        case VAL_INT: {
            int32_t v = 0;
            if (*offset + 4 > PAGE_SIZE) return 0;
            memcpy(&v, page + *offset, sizeof(v));
            *offset += 4;
            cell->as.as_int = (int)v;
            return 1;
        }
        case VAL_FLOAT: {
            double v = 0;
            if (*offset + 8 > PAGE_SIZE) return 0;
            memcpy(&v, page + *offset, sizeof(v));
            *offset += 8;
            cell->as.as_float = v;
            return 1;
        }
        case VAL_BOOL: {
            int32_t v = 0;
            if (*offset + 4 > PAGE_SIZE) return 0;
            memcpy(&v, page + *offset, sizeof(v));
            *offset += 4;
            cell->as.as_int = v != 0 ? 1 : 0;
            return 1;
        }
        case VAL_STRING:
        case VAL_DATE:
        case VAL_TIMESTAMP: {
            int32_t len = 0;
            if (*offset + 4 > PAGE_SIZE) return 0;
            memcpy(&len, page + *offset, sizeof(len));
            *offset += 4;
            if (len < 0 || *offset + len > PAGE_SIZE) return 0;
            cell->as.as_string = malloc((size_t)len + 1);
            if (cell->as.as_string == NULL) return 0;
            memcpy(cell->as.as_string, page + *offset, (size_t)len);
            cell->as.as_string[len] = '\0';
            *offset += len;
            return 1;
        }
        default:
            return 0;
    }
}

/* Writes a cell (type tag + payload) to the catalog page. */
static int catalog_cell_size(const Cell* cell) {
    switch (cell->type) {
        case VAL_INT:    return 1 + 4;
        case VAL_BOOL:   return 1 + 4;
        case VAL_FLOAT:  return 1 + 8;
        case VAL_STRING:
        case VAL_DATE:
        case VAL_TIMESTAMP:
            return 1 + 4 + (int)strlen(cell->as.as_string != NULL ? cell->as.as_string : "");
        default:         return 1; /* VAL_NULL: tag only */
    }
}

static void catalog_write_cell(uint8_t* page, int* offset, const Cell* cell) {
    page[(*offset)++] = (uint8_t)cell->type;
    switch (cell->type) {
        case VAL_INT: {
            int32_t v = (int32_t)cell->as.as_int;
            memcpy(page + *offset, &v, sizeof(v));
            *offset += 4;
            break;
        }
        case VAL_FLOAT: {
            double v = cell->as.as_float;
            memcpy(page + *offset, &v, sizeof(v));
            *offset += 8;
            break;
        }
        case VAL_BOOL: {
            int32_t v = cell->as.as_int != 0 ? 1 : 0;
            memcpy(page + *offset, &v, sizeof(v));
            *offset += 4;
            break;
        }
        case VAL_STRING:
        case VAL_DATE:
        case VAL_TIMESTAMP: {
            const char* s = cell->as.as_string != NULL ? cell->as.as_string : "";
            int32_t len = (int32_t)strlen(s);
            memcpy(page + *offset, &len, sizeof(len));
            *offset += 4;
            memcpy(page + *offset, s, (size_t)len);
            *offset += len;
            break;
        }
        default:
            break;
    }
}

static int catalog_read_indexes(uint8_t* page, int* offset, Table* table) {
    uint8_t index_count = page[(*offset)++];
    if (index_count > MAX_TABLE_INDEXES) index_count = MAX_TABLE_INDEXES;
    if (index_count == 0) return 1;

    table->indexes = calloc(index_count, sizeof(TableIndex));
    if (table->indexes == NULL) return 0;

    for (int i = 0; i < (int)index_count; i++) {
        uint8_t name_len = page[(*offset)++];
        if (*offset + name_len + 1 > PAGE_SIZE) return 0;
        table->indexes[i].name = malloc((size_t)name_len + 1);
        if (table->indexes[i].name == NULL) return 0;
        memcpy(table->indexes[i].name, page + *offset, name_len);
        table->indexes[i].name[name_len] = '\0';
        *offset += name_len;

        uint8_t col_len = page[(*offset)++];
        if (*offset + col_len + 4 > PAGE_SIZE) return 0;
        table->indexes[i].column_name = malloc((size_t)col_len + 1);
        if (table->indexes[i].column_name == NULL) return 0;
        memcpy(table->indexes[i].column_name, page + *offset, col_len);
        table->indexes[i].column_name[col_len] = '\0';
        *offset += col_len;

        memcpy(&table->indexes[i].root_page, page + *offset, sizeof(int32_t));
        *offset += 4;
        table->index_count++;
    }
    return 1;
}

static int catalog_read_page(Context* ctx) {
    Pager* pager = ctx->pager;
    /* Page 1 is reserved for the catalog. */
    g_catalog_page = 1;

    uint8_t page[PAGE_SIZE];
    pager_read_page(pager, g_catalog_page, page);

    int offset = 0;
    uint32_t table_count = 0;
    memcpy(&table_count, page + offset, sizeof(table_count));
    offset += 4;

    int catalog_version = 1;
    if (table_count == CATALOG_MAGIC_V2) {
        catalog_version = 2;
        memcpy(&table_count, page + offset, sizeof(table_count));
        offset += 4;
    } else if (table_count == CATALOG_MAGIC_V3) {
        catalog_version = 3;
        memcpy(&table_count, page + offset, sizeof(table_count));
        offset += 4;
    } else if (table_count == CATALOG_MAGIC_V4) {
        catalog_version = 4;
        memcpy(&table_count, page + offset, sizeof(table_count));
        offset += 4;
    } else if (table_count == CATALOG_MAGIC_V5) {
        catalog_version = 5;
        memcpy(&table_count, page + offset, sizeof(table_count));
        offset += 4;
    } else if (table_count == CATALOG_MAGIC_V6) {
        catalog_version = 6;
        memcpy(&table_count, page + offset, sizeof(table_count));
        offset += 4;
    }

    for (uint32_t t = 0; t < table_count && g_catalog_count < MAX_CATALOG_TABLES; t++) {
        Table* table = calloc(1, sizeof(Table));
        if (table == NULL) return 0;

        uint8_t name_len = page[offset++];
        table->name = malloc((size_t)name_len + 1);
        if (table->name == NULL) {
            free(table);
            return 0;
        }
        memcpy(table->name, page + offset, name_len);
        table->name[name_len] = '\0';
        offset += name_len;

        uint8_t column_count = page[offset++];
        table->column_count = (int)column_count;
        table->columns = calloc((size_t)column_count, sizeof(Column));
        if (table->columns == NULL) {
            free_table(table);
            return 0;
        }

        for (int c = 0; c < (int)column_count; c++) {
            uint8_t col_name_len = page[offset++];
            table->columns[c].name = malloc((size_t)col_name_len + 1);
            if (table->columns[c].name == NULL) {
                free_table(table);
                return 0;
            }
            memcpy(table->columns[c].name, page + offset, col_name_len);
            table->columns[c].name[col_name_len] = '\0';
            offset += col_name_len;
            table->columns[c].type = page[offset++];
            table->columns[c].flags = 0;
            table->columns[c].default_value.type = VAL_NULL;
            table->columns[c].default_value.as.as_int = 0;
            if (catalog_version >= 3) {
                table->columns[c].flags = page[offset++];
                if (table->columns[c].flags & COL_FLAG_HAS_DEFAULT) {
                    if (!catalog_read_cell(page, &offset, &table->columns[c].default_value)) {
                        free_table(table);
                        return 0;
                    }
                }
            }
        }

        memcpy(&table->first_row_page, page + offset, sizeof(table->first_row_page));
        offset += 4;
        table->last_row_page = table->first_row_page;

        if (catalog_version >= 2) {
            if (!catalog_read_indexes(page, &offset, table)) {
                free_table(table);
                return 0;
            }
        }

        g_catalog[g_catalog_count++] = table;
    }

    /* V4 appends the view section after the table records. */
    if (catalog_version >= 4 && offset < PAGE_SIZE) {
        uint8_t view_count = page[offset++];
        if (view_count > MAX_CATALOG_VIEWS) view_count = MAX_CATALOG_VIEWS;
        for (int i = 0; i < (int)view_count; i++) {
            uint8_t name_len = page[offset++];
            if (offset + name_len + 2 > PAGE_SIZE) return 0;
            char* name = malloc((size_t)name_len + 1);
            if (name == NULL) return 0;
            memcpy(name, page + offset, name_len);
            name[name_len] = '\0';
            offset += name_len;

            uint16_t query_len = 0;
            memcpy(&query_len, page + offset, sizeof(query_len));
            offset += 2;
            if (offset + (int)query_len > PAGE_SIZE) {
                free(name);
                return 0;
            }
            char* select_query = malloc((size_t)query_len + 1);
            if (select_query == NULL) {
                free(name);
                return 0;
            }
            memcpy(select_query, page + offset, query_len);
            select_query[query_len] = '\0';
            offset += (int)query_len;

            g_views[g_view_count].name = name;
            g_views[g_view_count].select_query = select_query;
            g_view_count++;
        }
    }

    /* V5 appends the sequence section after the view section: u8
       sequence_count, then per sequence a u8 name, u8 has_value, i32
       current, i32 increment. */
    if (catalog_version >= 5 && offset < PAGE_SIZE) {
        uint8_t sequence_count = page[offset++];
        if (sequence_count > MAX_CATALOG_SEQUENCES) sequence_count = MAX_CATALOG_SEQUENCES;
        for (int i = 0; i < (int)sequence_count; i++) {
            uint8_t name_len = page[offset++];
            if (offset + name_len + 9 > PAGE_SIZE) return 0;
            char* name = malloc((size_t)name_len + 1);
            if (name == NULL) return 0;
            memcpy(name, page + offset, name_len);
            name[name_len] = '\0';
            offset += name_len;

            int has_value = page[offset++];
            int32_t current = 0;
            memcpy(&current, page + offset, sizeof(current));
            offset += 4;
            int32_t increment = 0;
            memcpy(&increment, page + offset, sizeof(increment));
            offset += 4;

            g_sequences[g_sequence_count].name = name;
            g_sequences[g_sequence_count].has_value = has_value;
            g_sequences[g_sequence_count].current = (int)current;
            g_sequences[g_sequence_count].increment = (int)increment;
            g_sequence_count++;
        }
    }

    return 1;
}

static int catalog_write_page(Context* ctx) {
    Pager* pager = ctx->pager;
    /* Page 1 is reserved for the catalog. */
    g_catalog_page = 1;

    uint8_t page[PAGE_SIZE];
    memset(page, 0, PAGE_SIZE);

    int offset = 0;
    uint32_t magic = CATALOG_MAGIC_V6;
    memcpy(page + offset, &magic, sizeof(magic));
    offset += 4;
    uint32_t table_count = (uint32_t)g_catalog_count;
    memcpy(page + offset, &table_count, sizeof(table_count));
    offset += 4;

    for (int t = 0; t < g_catalog_count; t++) {
        Table* table = g_catalog[t];

        size_t name_len = strlen(table->name);
        if (name_len > MAX_NAME_LEN) name_len = MAX_NAME_LEN;
        page[offset++] = (uint8_t)name_len;
        memcpy(page + offset, table->name, name_len);
        offset += (int)name_len;

        page[offset++] = (uint8_t)table->column_count;
        for (int c = 0; c < table->column_count; c++) {
            size_t col_name_len = strlen(table->columns[c].name);
            if (col_name_len > MAX_NAME_LEN) col_name_len = MAX_NAME_LEN;
            int default_size = (table->columns[c].flags & COL_FLAG_HAS_DEFAULT)
                ? catalog_cell_size(&table->columns[c].default_value)
                : 0;
            if (offset + 3 + (int)col_name_len + default_size > PAGE_SIZE) break;
            page[offset++] = (uint8_t)col_name_len;
            memcpy(page + offset, table->columns[c].name, col_name_len);
            offset += (int)col_name_len;
            page[offset++] = (uint8_t)table->columns[c].type;
            page[offset++] = (uint8_t)table->columns[c].flags;
            if (table->columns[c].flags & COL_FLAG_HAS_DEFAULT) {
                catalog_write_cell(page, &offset, &table->columns[c].default_value);
            }
        }

        memcpy(page + offset, &table->first_row_page, sizeof(table->first_row_page));
        offset += 4;

        page[offset++] = (uint8_t)table->index_count;
        for (int i = 0; i < table->index_count; i++) {
            /* Index metadata is an optimization hint: if the catalog page is
               nearly full, drop the remaining entries rather than overflow.
               A lost index only costs a full scan after reopen. */
            size_t idx_name_len = strlen(table->indexes[i].name);
            size_t idx_col_len = strlen(table->indexes[i].column_name);
            if (idx_name_len > MAX_NAME_LEN) idx_name_len = MAX_NAME_LEN;
            if (idx_col_len > MAX_NAME_LEN) idx_col_len = MAX_NAME_LEN;
            if (offset + 2 + (int)idx_name_len + (int)idx_col_len + 4 > PAGE_SIZE) {
                page[offset - 1] = (uint8_t)i;
                break;
            }
            page[offset++] = (uint8_t)idx_name_len;
            memcpy(page + offset, table->indexes[i].name, idx_name_len);
            offset += (int)idx_name_len;
            page[offset++] = (uint8_t)idx_col_len;
            memcpy(page + offset, table->indexes[i].column_name, idx_col_len);
            offset += (int)idx_col_len;
            memcpy(page + offset, &table->indexes[i].root_page, sizeof(int32_t));
            offset += 4;
        }
    }

    /* V4 appends the view section after the table records: u8 view_count,
       then per view a u8 name length + name and a u16 length + SELECT text.
       Unlike index metadata, view definitions are user data — if they do not
       fit the page the write fails (without touching the page) so the caller
       can roll back instead of silently losing definitions. */
    if (offset + 1 > PAGE_SIZE) return 0;
    int view_count_pos = offset++;
    for (int i = 0; i < g_view_count; i++) {
        size_t view_name_len = strlen(g_views[i].name);
        size_t view_query_len = strlen(g_views[i].select_query);
        if (offset + 3 + (int)view_name_len + (int)view_query_len > PAGE_SIZE) {
            return 0;
        }
        page[offset++] = (uint8_t)view_name_len;
        memcpy(page + offset, g_views[i].name, view_name_len);
        offset += (int)view_name_len;
        uint16_t view_query_len16 = (uint16_t)view_query_len;
        memcpy(page + offset, &view_query_len16, sizeof(view_query_len16));
        offset += 2;
        memcpy(page + offset, g_views[i].select_query, view_query_len);
        offset += (int)view_query_len;
    }
    page[view_count_pos] = (uint8_t)g_view_count;

    /* V5 appends the sequence section after the view section: u8
       sequence_count, then per sequence a u8 name length + name, u8
       has_value, i32 current, i32 increment. Like views, sequences are user
       data — if they do not fit the page the write fails (without touching
       the page) so the caller can roll back. */
    if (offset + 1 > PAGE_SIZE) return 0;
    int seq_count_pos = offset++;
    for (int i = 0; i < g_sequence_count; i++) {
        size_t seq_name_len = strlen(g_sequences[i].name);
        if (offset + 10 + (int)seq_name_len > PAGE_SIZE) {
            return 0;
        }
        page[offset++] = (uint8_t)seq_name_len;
        memcpy(page + offset, g_sequences[i].name, seq_name_len);
        offset += (int)seq_name_len;
        page[offset++] = (uint8_t)g_sequences[i].has_value;
        int32_t v32 = (int32_t)g_sequences[i].current;
        memcpy(page + offset, &v32, sizeof(v32));
        offset += 4;
        v32 = (int32_t)g_sequences[i].increment;
        memcpy(page + offset, &v32, sizeof(v32));
        offset += 4;
    }
    page[seq_count_pos] = (uint8_t)g_sequence_count;

    pager_write_page(pager, g_catalog_page, page);
    return 1;
}

/* -------------------------------------------------------------------------- */
/* Catalog API                                                                */
/* -------------------------------------------------------------------------- */

int catalog_open(Context* ctx) {
    if (ctx == NULL || ctx->db_path == NULL) return 0;
    ctx->pager = pager_open(ctx->db_path);
    if (ctx->pager == NULL) return 0;
    catalog_clear(ctx);
    return catalog_read_page(ctx);
}

void catalog_close(Context* ctx) {
    if (ctx == NULL) return;
    if (ctx->pager != NULL) {
        catalog_write_page(ctx);
        pager_close(ctx->pager);
        ctx->pager = NULL;
    }
    catalog_clear(ctx);
}

Table* catalog_create_table(Context* ctx, const char* name, const char** columns, int* types, int column_count) {
    if (ctx == NULL || ctx->pager == NULL) return NULL;
    if (g_catalog_count >= MAX_CATALOG_TABLES) return NULL;
    if (name == NULL || columns == NULL || types == NULL || column_count <= 0) return NULL;
    if (catalog_find_table(ctx, name) != NULL) return NULL;

    Table* t = calloc(1, sizeof(Table));
    if (t == NULL) return NULL;

    t->name = strdup(name);
    t->columns = calloc((size_t)column_count, sizeof(Column));
    if (t->columns == NULL) {
        free_table(t);
        return NULL;
    }
    t->column_count = column_count;

    for (int i = 0; i < column_count; i++) {
        t->columns[i].name = strdup(columns[i]);
        t->columns[i].type = types[i];
    }

    t->first_row_page = 0;
    t->last_row_page = 0;

    g_catalog[g_catalog_count++] = t;
    catalog_write_page(ctx);
    return t;
}

Table* catalog_find_table(Context* ctx, const char* name) {
    (void)ctx;
    if (name == NULL) return NULL;
    for (int i = 0; i < g_catalog_count; i++) {
        if (g_catalog[i] != NULL && strcmp(g_catalog[i]->name, name) == 0) {
            return g_catalog[i];
        }
    }
    return NULL;
}

const char* catalog_view_query(Context* ctx, const char* name) {
    (void)ctx;
    if (name == NULL) return NULL;
    for (int i = 0; i < g_view_count; i++) {
        if (g_views[i].name != NULL && strcmp(g_views[i].name, name) == 0) {
            return g_views[i].select_query;
        }
    }
    return NULL;
}

int catalog_table_count(Context* ctx) {
    (void)ctx;
    return g_catalog_count;
}

const char* catalog_table_name(Context* ctx, int index) {
    (void)ctx;
    if (index < 0 || index >= g_catalog_count) return NULL;
    return g_catalog[index]->name;
}

int catalog_table_column_count(Context* ctx, int index) {
    (void)ctx;
    if (index < 0 || index >= g_catalog_count) return 0;
    return g_catalog[index]->column_count;
}

const char* catalog_table_column_name(Context* ctx, int index, int col) {
    (void)ctx;
    if (index < 0 || index >= g_catalog_count) return NULL;
    if (col < 0 || col >= g_catalog[index]->column_count) return NULL;
    return g_catalog[index]->columns[col].name;
}

int catalog_table_column_type(Context* ctx, int index, int col) {
    (void)ctx;
    if (index < 0 || index >= g_catalog_count) return 0;
    if (col < 0 || col >= g_catalog[index]->column_count) return 0;
    return g_catalog[index]->columns[col].type;
}

/* -------------------------------------------------------------------------- */
/* Row serialization                                                          */
/* -------------------------------------------------------------------------- */

/* Row page header (8 bytes):
 *   bytes 0-3  int32 next_page  (0 = last page of the table's chain)
 *   bytes 4-7  int32 free_ptr   (offset one past the last record)
 * Records are packed from ROW_PAGE_HEADER_SIZE up to free_ptr, so the free
 * pointer alone says how much of a page is data.
 *
 * Older builds also stored an int16 record count at offset 2, which is the
 * high half of next_page. Linking a full page to its successor zeroed that
 * count, so every full page that had a successor scanned as empty (issue
 * #50); and on the last page the count made next_page read as count<<16,
 * a link far past the end of the file. Nothing writes the count any more
 * and nothing may read it. Databases written by those builds stay readable:
 * scans stop at free_ptr, and row_page_next() discards a link that points
 * outside the file, which is what a legacy last page carries. */
#define ROW_PAGE_HEADER_SIZE 8
#define ROW_PAGE_DATA_SIZE   (PAGE_SIZE - ROW_PAGE_HEADER_SIZE)

/* Offset one past the last record on the page, clamped to the data area. */
static int row_page_data_end(const uint8_t* page) {
    int32_t free_ptr;
    memcpy(&free_ptr, page + 4, sizeof(free_ptr));
    if (free_ptr < ROW_PAGE_HEADER_SIZE) return ROW_PAGE_HEADER_SIZE;
    if (free_ptr > PAGE_SIZE) return PAGE_SIZE;
    return (int)free_ptr;
}

/* The page that follows `page` in its table's chain, or 0 when it is the
   last one. A link outside the file is not a link (see the layout note). */
static int row_page_next(Pager* pager, const uint8_t* page) {
    int32_t next;
    memcpy(&next, page, sizeof(next));
    if (next <= 0 || next >= pager_page_count(pager)) return 0;
    return (int)next;
}

static int row_record_size(Table* table, Cell* cells) {
    int size = 2; /* record length prefix */
    for (int i = 0; i < table->column_count; i++) {
        size += 1; /* type tag */
        switch (cells[i].type) {
            case VAL_INT:    size += 4; break;
            case VAL_FLOAT:  size += 8; break;
            case VAL_STRING:
            case VAL_DATE:
            case VAL_TIMESTAMP:
                size += 4 + (int)strlen(cells[i].as.as_string != NULL
                                        ? cells[i].as.as_string : "");
                break;
            case VAL_BOOL:   size += 4; break;
            case VAL_NULL:   break; /* tag only, no payload */
            default:         size += 4; break;
        }
    }
    return size;
}

static void serialize_row(Table* table, Cell* cells, uint8_t* out) {
    int offset = 2; /* length prefix */
    for (int i = 0; i < table->column_count; i++) {
        out[offset++] = (uint8_t)cells[i].type;
        switch (cells[i].type) {
            case VAL_INT: {
                int32_t v = (int32_t)cells[i].as.as_int;
                memcpy(out + offset, &v, sizeof(v));
                offset += sizeof(v);
                break;
            }
            case VAL_FLOAT: {
                double v = cells[i].as.as_float;
                memcpy(out + offset, &v, sizeof(v));
                offset += sizeof(v);
                break;
            }
            case VAL_STRING:
            case VAL_DATE:
            case VAL_TIMESTAMP: {
                const char* s = cells[i].as.as_string ? cells[i].as.as_string : "";
                int32_t len = (int32_t)strlen(s);
                memcpy(out + offset, &len, sizeof(len));
                offset += sizeof(len);
                memcpy(out + offset, s, (size_t)len);
                offset += len;
                break;
            }
            case VAL_BOOL: {
                /* Stored in the same four bytes an int uses: a bool column is
                   0 or 1 and its own tag keeps it distinguishable. */
                int32_t v = cells[i].as.as_int != 0 ? 1 : 0;
                memcpy(out + offset, &v, sizeof(v));
                offset += sizeof(v);
                break;
            }
            case VAL_NULL:
                /* NULL cells serialize as just the type tag. */
                break;
            default: {
                int32_t v = 0;
                memcpy(out + offset, &v, sizeof(v));
                offset += sizeof(v);
                break;
            }
        }
    }
    int16_t total = (int16_t)offset;
    memcpy(out, &total, sizeof(total));
}

static int deserialize_cell(const uint8_t* data, int* offset, Cell* cell) {
    cell->type = data[(*offset)++];
    switch (cell->type) {
        case VAL_NULL:
            cell->as.as_int = 0;
            return 1;
        case VAL_INT: {
            int32_t v = 0;
            memcpy(&v, data + *offset, sizeof(v));
            *offset += sizeof(v);
            cell->as.as_int = (int)v;
            return 1;
        }
        case VAL_FLOAT: {
            double v = 0;
            memcpy(&v, data + *offset, sizeof(v));
            *offset += sizeof(v);
            cell->as.as_float = v;
            return 1;
        }
        case VAL_BOOL: {
            int32_t v = 0;
            memcpy(&v, data + *offset, sizeof(v));
            *offset += sizeof(v);
            cell->as.as_int = v != 0 ? 1 : 0;
            return 1;
        }
        case VAL_STRING:
        case VAL_DATE:
        case VAL_TIMESTAMP: {
            int32_t len = 0;
            memcpy(&len, data + *offset, sizeof(len));
            *offset += sizeof(len);
            cell->as.as_string = malloc((size_t)len + 1);
            if (cell->as.as_string == NULL) return 0;
            memcpy(cell->as.as_string, data + *offset, (size_t)len);
            cell->as.as_string[len] = '\0';
            *offset += len;
            return 1;
        }
        default: {
            *offset += 4;
            cell->type = VAL_INT;
            cell->as.as_int = 0;
            return 1;
        }
    }
}

static Row* deserialize_row(Table* table, const uint8_t* record) {
    Row* row = malloc(sizeof(Row));
    if (row == NULL) return NULL;
    row->field_count = table->column_count;
    row->fields = calloc((size_t)row->field_count, sizeof(Field));
    if (row->fields == NULL) {
        free(row);
        return NULL;
    }

    int offset = 2; /* skip length prefix */
    for (int i = 0; i < row->field_count; i++) {
        row->fields[i].name = strdup(table->columns[i].name);
        if (!deserialize_cell(record, &offset, &row->fields[i].value)) {
            for (int j = 0; j <= i; j++) {
                free(row->fields[j].name);
                if (cell_owns_text(row->fields[j].value.type)) {
                    free(row->fields[j].value.as.as_string);
                }
            }
            free(row->fields);
            free(row);
            return NULL;
        }
    }
    return row;
}

/* -------------------------------------------------------------------------- */
/* Row page storage                                                           */
/* -------------------------------------------------------------------------- */

static void row_page_init(uint8_t* page) {
    memset(page, 0, PAGE_SIZE);
    int32_t free_ptr = ROW_PAGE_HEADER_SIZE;
    memcpy(page + 4, &free_ptr, sizeof(free_ptr));
}

static int row_page_append(Context* ctx, Table* table, Cell* cells,
                           int* out_page, int* out_offset) {
    Pager* pager = ctx->pager;
    int record_size = row_record_size(table, cells);
    if (record_size > ROW_PAGE_DATA_SIZE) return 0;

    uint8_t* record = calloc(1, (size_t)record_size);
    if (record == NULL) return 0;
    serialize_row(table, cells, record);

    uint8_t page[PAGE_SIZE];
    int page_num = table->last_row_page;

    if (page_num == 0) {
        /* First row for this table. */
        page_num = pager_allocate_page(pager);
        if (page_num < 0) {
            free(record);
            return 0;
        }
        row_page_init(page);
        table->first_row_page = page_num;
        table->last_row_page = page_num;
    } else {
        /* last_row_page is not persisted (a reopened catalog only knows the
           first page), so find the real tail: appending to a page that already
           has a successor would overwrite its link and orphan the rest. */
        for (;;) {
            pager_read_page(pager, page_num, page);
            int next = row_page_next(pager, page);
            if (next == 0) break;
            page_num = next;
        }
        table->last_row_page = page_num;
    }

    int32_t free_ptr = row_page_data_end(page);
    /* The tail has no successor: drop anything a legacy build left in the
       link bytes (its old record count). */
    memset(page, 0, sizeof(int32_t));

    if (free_ptr + record_size > PAGE_SIZE) {
        /* Page full: allocate next page. */
        int next_page_num = pager_allocate_page(pager);
        if (next_page_num < 0) {
            free(record);
            return 0;
        }
        memcpy(page, &next_page_num, sizeof(next_page_num));
        pager_write_page(pager, page_num, page);

        page_num = next_page_num;
        row_page_init(page);
        free_ptr = ROW_PAGE_HEADER_SIZE;
        table->last_row_page = page_num;
    }

    memcpy(page + free_ptr, record, (size_t)record_size);
    free(record);
    if (out_page != NULL) *out_page = page_num;
    if (out_offset != NULL) *out_offset = (int)free_ptr;
    free_ptr += record_size;

    memcpy(page + 4, &free_ptr, sizeof(free_ptr));
    pager_write_page(pager, page_num, page);

    catalog_write_page(ctx);
    return 1;
}

static int table_column_index(Table* table, const char* name) {
    for (int i = 0; i < table->column_count; i++) {
        if (strcmp(table->columns[i].name, name) == 0) return i;
    }
    return -1;
}

/* Adds (cells[column], locator) to every index defined on the table. */
static void indexes_add_row(Context* ctx, Table* table, Cell* cells,
                            int row_page, int row_offset) {
    for (int i = 0; i < table->index_count; i++) {
        TableIndex* idx = &table->indexes[i];
        int col = table_column_index(table, idx->column_name);
        if (col < 0 || col >= table->column_count) continue;
        BTree* tree = btree_open(ctx->pager, idx->root_page);
        if (tree == NULL) continue;
        if (btree_insert(tree, &cells[col], row_page, row_offset)) {
            idx->root_page = btree_root_page(tree);
        }
        btree_destroy(tree);
    }
}

void catalog_insert(Context* ctx, Table* table, Cell* cells) {
    if (ctx == NULL || ctx->pager == NULL || table == NULL || cells == NULL) return;
    int row_page = 0;
    int row_offset = 0;
    if (!row_page_append(ctx, table, cells, &row_page, &row_offset)) return;
    indexes_add_row(ctx, table, cells, row_page, row_offset);
}

/* -------------------------------------------------------------------------- */
/* Row scanning                                                               */
/* -------------------------------------------------------------------------- */

static int read_all_rows(Context* ctx, Table* table, Row** out_rows, int* out_count) {
    Pager* pager = ctx->pager;
    int capacity = 8;
    int count = 0;
    Row* rows = calloc((size_t)capacity, sizeof(Row));
    if (rows == NULL) return 0;

    int page_num = table->first_row_page;
    while (page_num != 0) {
        uint8_t page[PAGE_SIZE];
        pager_read_page(pager, page_num, page);

        int data_end = row_page_data_end(page);
        int offset = ROW_PAGE_HEADER_SIZE;
        while (offset < data_end) {
            int16_t record_size;
            memcpy(&record_size, page + offset, sizeof(record_size));

            /* A bad length would otherwise loop forever or read past the
               page; report it like any other undecodable record. */
            Row* row = (record_size > 0 && offset + (int)record_size <= data_end)
                ? deserialize_row(table, page + offset)
                : NULL;
            if (row == NULL) {
                for (int i = 0; i < count; i++) {
                    for (int j = 0; j < rows[i].field_count; j++) {
                        free(rows[i].fields[j].name);
                        if (cell_owns_text(rows[i].fields[j].value.type)) {
                            free(rows[i].fields[j].value.as.as_string);
                        }
                    }
                    free(rows[i].fields);
                }
                free(rows);
                return 0;
            }

            if (count >= capacity) {
                capacity *= 2;
                Row* new_rows = realloc(rows, (size_t)capacity * sizeof(Row));
                if (new_rows == NULL) {
                    /* cleanup */
                    for (int j = 0; j < row->field_count; j++) {
                        free(row->fields[j].name);
                        if (cell_owns_text(row->fields[j].value.type)) {
                            free(row->fields[j].value.as.as_string);
                        }
                    }
                    free(row->fields);
                    free(row);
                    for (int i = 0; i < count; i++) {
                        for (int j = 0; j < rows[i].field_count; j++) {
                            free(rows[i].fields[j].name);
                            if (cell_owns_text(rows[i].fields[j].value.type)) {
                                free(rows[i].fields[j].value.as.as_string);
                            }
                        }
                        free(rows[i].fields);
                    }
                    free(rows);
                    return 0;
                }
                rows = new_rows;
            }

            rows[count++] = *row;
            free(row);
            offset += (int)record_size;
        }

        page_num = row_page_next(pager, page);
    }

    *out_rows = rows;
    *out_count = count;
    return 1;
}

/* -------------------------------------------------------------------------- */
/* SQL parser (lexer + SELECT/CREATE/INSERT)                                  */
/* -------------------------------------------------------------------------- */

typedef enum {
    TOK_EOF,
    TOK_IDENT,
    TOK_NUMBER,
    TOK_STRING,
    TOK_STAR,
    TOK_COMMA,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_DOT,
    TOK_SELECT,
    TOK_FROM,
    TOK_WHERE,
    TOK_ORDER,
    TOK_GROUP,
    TOK_BY,
    TOK_ASC,
    TOK_DESC,
    TOK_LIMIT,
    TOK_JOIN,
    TOK_LEFT,
    TOK_OUTER,
    TOK_ON,
    TOK_COUNT,
    TOK_SUM,
    TOK_AVG,
    TOK_MIN,
    TOK_MAX,
    TOK_CREATE,
    TOK_TABLE,
    TOK_INSERT,
    TOK_INTO,
    TOK_VALUES,
    TOK_UPDATE,
    TOK_SET,
    TOK_DELETE,
    TOK_INT,
    TOK_FLOAT,
    TOK_BOOL_KW,
    TOK_DATE_KW,
    TOK_TIMESTAMP_KW,
    TOK_TRUE,
    TOK_FALSE,
    TOK_STRING_KW,
    TOK_EQ,
    TOK_LT,
    TOK_GT,
    TOK_LE,
    TOK_GE,
    TOK_NE,
    TOK_NULL,
    TOK_IS,
    TOK_NOT,
    TOK_AND,
    TOK_OR,
    TOK_IN,
    TOK_LIKE,
    TOK_DROP,
    TOK_ALTER,
    TOK_ADD,
    TOK_IF,
    TOK_EXISTS,
    TOK_COLUMN,
    TOK_INDEX,
    TOK_PRIMARY,
    TOK_KEY,
    TOK_UNIQUE,
    TOK_DEFAULT,
    TOK_VIEW,
    TOK_AS,
    TOK_PARAM  /* ?N bind placeholder; the token text is the digits */
} SqlTokenType;

typedef struct {
    SqlTokenType type;
    const char*  text;
    int          length;
} SqlToken;

typedef struct {
    const char* start;
    const char* current;
    int         has_pushback;
    SqlToken    pushback;
} SqlLexer;

static void sql_lexer_init(SqlLexer* lex, const char* query) {
    lex->start = query;
    lex->current = query;
    lex->has_pushback = 0;
}

static void sql_skip_whitespace(SqlLexer* lex) {
    while (*lex->current != '\0' && isspace((unsigned char)*lex->current)) {
        lex->current++;
    }
}

static int sql_is_alpha_or_(char c) {
    return isalpha((unsigned char)c) || c == '_';
}

static SqlToken sql_make_token(SqlLexer* lex, SqlTokenType type) {
    SqlToken tok;
    tok.type = type;
    tok.text = lex->start;
    tok.length = (int)(lex->current - lex->start);
    return tok;
}

static SqlTokenType sql_check_keyword(const char* start, int length) {
    if (length == 6 && strncasecmp(start, "SELECT", 6) == 0) return TOK_SELECT;
    if (length == 4 && strncasecmp(start, "FROM", 4) == 0) return TOK_FROM;
    if (length == 5 && strncasecmp(start, "WHERE", 5) == 0) return TOK_WHERE;
    if (length == 5 && strncasecmp(start, "ORDER", 5) == 0) return TOK_ORDER;
    if (length == 5 && strncasecmp(start, "GROUP", 5) == 0) return TOK_GROUP;
    if (length == 2 && strncasecmp(start, "BY", 2) == 0) return TOK_BY;
    if (length == 3 && strncasecmp(start, "ASC", 3) == 0) return TOK_ASC;
    if (length == 4 && strncasecmp(start, "DESC", 4) == 0) return TOK_DESC;
    if (length == 5 && strncasecmp(start, "LIMIT", 5) == 0) return TOK_LIMIT;
    if (length == 4 && strncasecmp(start, "JOIN", 4) == 0) return TOK_JOIN;
    if (length == 4 && strncasecmp(start, "LEFT", 4) == 0) return TOK_LEFT;
    if (length == 5 && strncasecmp(start, "OUTER", 5) == 0) return TOK_OUTER;
    if (length == 2 && strncasecmp(start, "ON", 2) == 0) return TOK_ON;
    if (length == 5 && strncasecmp(start, "COUNT", 5) == 0) return TOK_COUNT;
    if (length == 3 && strncasecmp(start, "SUM", 3) == 0) return TOK_SUM;
    if (length == 3 && strncasecmp(start, "AVG", 3) == 0) return TOK_AVG;
    if (length == 3 && strncasecmp(start, "MIN", 3) == 0) return TOK_MIN;
    if (length == 3 && strncasecmp(start, "MAX", 3) == 0) return TOK_MAX;
    if (length == 6 && strncasecmp(start, "CREATE", 6) == 0) return TOK_CREATE;
    if (length == 5 && strncasecmp(start, "TABLE", 5) == 0) return TOK_TABLE;
    if (length == 6 && strncasecmp(start, "INSERT", 6) == 0) return TOK_INSERT;
    if (length == 4 && strncasecmp(start, "INTO", 4) == 0) return TOK_INTO;
    if (length == 6 && strncasecmp(start, "VALUES", 6) == 0) return TOK_VALUES;
    if (length == 6 && strncasecmp(start, "UPDATE", 6) == 0) return TOK_UPDATE;
    if (length == 3 && strncasecmp(start, "SET", 3) == 0) return TOK_SET;
    if (length == 6 && strncasecmp(start, "DELETE", 6) == 0) return TOK_DELETE;
    if (length == 3 && strncasecmp(start, "INT", 3) == 0) return TOK_INT;
    if (length == 5 && strncasecmp(start, "FLOAT", 5) == 0) return TOK_FLOAT;
    if (length == 6 && strncasecmp(start, "STRING", 6) == 0) return TOK_STRING_KW;
    if (length == 4 && strncasecmp(start, "BOOL", 4) == 0) return TOK_BOOL_KW;
    if (length == 7 && strncasecmp(start, "BOOLEAN", 7) == 0) return TOK_BOOL_KW;
    if (length == 4 && strncasecmp(start, "DATE", 4) == 0) return TOK_DATE_KW;
    if (length == 9 && strncasecmp(start, "TIMESTAMP", 9) == 0) return TOK_TIMESTAMP_KW;
    if (length == 4 && strncasecmp(start, "TRUE", 4) == 0) return TOK_TRUE;
    if (length == 5 && strncasecmp(start, "FALSE", 5) == 0) return TOK_FALSE;
    if (length == 4 && strncasecmp(start, "NULL", 4) == 0) return TOK_NULL;
    if (length == 2 && strncasecmp(start, "IS", 2) == 0) return TOK_IS;
    if (length == 3 && strncasecmp(start, "NOT", 3) == 0) return TOK_NOT;
    if (length == 3 && strncasecmp(start, "AND", 3) == 0) return TOK_AND;
    if (length == 2 && strncasecmp(start, "OR", 2) == 0) return TOK_OR;
    if (length == 2 && strncasecmp(start, "IN", 2) == 0) return TOK_IN;
    if (length == 4 && strncasecmp(start, "LIKE", 4) == 0) return TOK_LIKE;
    if (length == 4 && strncasecmp(start, "DROP", 4) == 0) return TOK_DROP;
    if (length == 5 && strncasecmp(start, "ALTER", 5) == 0) return TOK_ALTER;
    if (length == 3 && strncasecmp(start, "ADD", 3) == 0) return TOK_ADD;
    if (length == 2 && strncasecmp(start, "IF", 2) == 0) return TOK_IF;
    if (length == 6 && strncasecmp(start, "EXISTS", 6) == 0) return TOK_EXISTS;
    if (length == 6 && strncasecmp(start, "COLUMN", 6) == 0) return TOK_COLUMN;
    if (length == 5 && strncasecmp(start, "INDEX", 5) == 0) return TOK_INDEX;
    if (length == 7 && strncasecmp(start, "PRIMARY", 7) == 0) return TOK_PRIMARY;
    if (length == 3 && strncasecmp(start, "KEY", 3) == 0) return TOK_KEY;
    if (length == 6 && strncasecmp(start, "UNIQUE", 6) == 0) return TOK_UNIQUE;
    if (length == 7 && strncasecmp(start, "DEFAULT", 7) == 0) return TOK_DEFAULT;
    if (length == 4 && strncasecmp(start, "VIEW", 4) == 0) return TOK_VIEW;
    if (length == 2 && strncasecmp(start, "AS", 2) == 0) return TOK_AS;
    return TOK_IDENT;
}

static void sql_lexer_pushback(SqlLexer* lex, SqlToken tok) {
    lex->pushback = tok;
    lex->has_pushback = 1;
}

static SqlToken sql_next_token(SqlLexer* lex) {
    if (lex->has_pushback) {
        lex->has_pushback = 0;
        return lex->pushback;
    }
    sql_skip_whitespace(lex);
    lex->start = lex->current;

    if (*lex->current == '\0') {
        return sql_make_token(lex, TOK_EOF);
    }

    char c = *lex->current;

    if (sql_is_alpha_or_(c)) {
        while (sql_is_alpha_or_(*lex->current) || isdigit((unsigned char)*lex->current)) {
            lex->current++;
        }
        SqlTokenType type = sql_check_keyword(lex->start, (int)(lex->current - lex->start));
        return sql_make_token(lex, type);
    }

    if (isdigit((unsigned char)c)) {
        while (isdigit((unsigned char)*lex->current) || *lex->current == '.') {
            lex->current++;
        }
        return sql_make_token(lex, TOK_NUMBER);
    }

    if (c == '\'' || c == '"') {
        char quote = c;
        lex->current++;
        lex->start = lex->current;
        while (*lex->current != '\0' && *lex->current != quote) {
            lex->current++;
        }
        SqlToken tok = sql_make_token(lex, TOK_STRING);
        if (*lex->current == quote) {
            lex->current++;
        }
        return tok;
    }

    if (c == '?' && isdigit((unsigned char)lex->current[1])) {
        lex->current++;
        lex->start = lex->current;
        while (isdigit((unsigned char)*lex->current)) {
            lex->current++;
        }
        return sql_make_token(lex, TOK_PARAM);
    }

    lex->current++;
    switch (c) {
        case '*': return sql_make_token(lex, TOK_STAR);
        case ',': return sql_make_token(lex, TOK_COMMA);
        case '(': return sql_make_token(lex, TOK_LPAREN);
        case ')': return sql_make_token(lex, TOK_RPAREN);
        case '.': return sql_make_token(lex, TOK_DOT);
        case '=': return sql_make_token(lex, TOK_EQ);
        case '<':
            if (*lex->current == '=') { lex->current++; return sql_make_token(lex, TOK_LE); }
            if (*lex->current == '>') { lex->current++; return sql_make_token(lex, TOK_NE); }
            return sql_make_token(lex, TOK_LT);
        case '>':
            if (*lex->current == '=') { lex->current++; return sql_make_token(lex, TOK_GE); }
            return sql_make_token(lex, TOK_GT);
        default:
            return sql_make_token(lex, TOK_EOF);
    }
}

static void sql_token_text(SqlToken* tok, char* out, size_t out_size) {
    size_t len = (size_t)tok->length;
    if (len >= out_size) len = out_size - 1;
    memcpy(out, tok->text, len);
    out[len] = '\0';
}

/* Values bound to the ?N placeholders of the statement being executed (see
   sql_exec_params). The parsers read them while turning a TOK_PARAM into a
   literal; NULL/0 outside a bound statement, so a stray ?N fails to parse. */
static const Value* g_sql_binds;
static int          g_sql_bind_count;

/* Turns the TOK_PARAM `tok` (?1, ?2, ...) into the Cell holding the bound
   value. String cells are heap copies, freed like any parsed literal.
   Returns 0 when no such value is bound. */
static int sql_param_cell(const SqlToken* tok, Cell* out) {
    int index = 0;
    for (int i = 0; i < tok->length; i++) {
        index = index * 10 + (tok->text[i] - '0');
        if (index > g_sql_bind_count) return 0;
    }
    if (index < 1 || g_sql_binds == NULL) return 0;

    Value v = g_sql_binds[index - 1];
    switch (v.type) {
        case VAL_INT:
        case VAL_BOOL:
            out->type = VAL_INT;
            out->as.as_int = v.as.as_int;
            return 1;
        case VAL_FLOAT:
            out->type = VAL_FLOAT;
            out->as.as_float = v.as.as_float;
            return 1;
        case VAL_STRING: {
            const char* s = v.as.as_string != NULL ? v.as.as_string : "";
            out->type = VAL_STRING;
            out->as.as_string = strdup(s);
            return out->as.as_string != NULL;
        }
        default:
            out->type = VAL_NULL;
            out->as.as_int = 0;
            return 1;
    }
}

/* -------------------------------------------------------------------------- */
/* SELECT parsing & execution                                                 */
/* -------------------------------------------------------------------------- */

#define MAX_SELECT_COLUMNS 16
#define MAX_COLUMNS        16

typedef enum {
    AGG_NONE,
    AGG_COUNT,
    AGG_SUM,
    AGG_AVG,
    AGG_MIN,
    AGG_MAX
} AggregateFunc;

/* -------------------------------------------------------------------------- */
/* WHERE expression trees                                                     */
/* -------------------------------------------------------------------------- */

#define MAX_IN_LIST 16

typedef enum {
    WHERE_CMP,         /* column <cmp_op> literal */
    WHERE_IS_NULL,     /* column IS NULL */
    WHERE_IS_NOT_NULL, /* column IS NOT NULL */
    WHERE_IN,          /* column [NOT] IN (literal, ...) */
    WHERE_LIKE,        /* column [NOT] LIKE 'pattern' */
    WHERE_AND,
    WHERE_OR,
    WHERE_NOT
} WhereType;

typedef struct WhereNode {
    WhereType type;
    struct WhereNode* left;
    struct WhereNode* right; /* AND/OR only; NOT uses left */
    char table_prefix[MAX_NAME_LEN + 1];
    char column[MAX_NAME_LEN + 1];
    int  cmp_op;             /* 0 =, 1 <, 2 >, 3 <=, 4 >=, 5 <> */
    int  negate;             /* NOT IN / NOT LIKE */
    Cell literal;            /* WHERE_CMP operand / WHERE_LIKE pattern */
    Cell list[MAX_IN_LIST];  /* WHERE_IN operands */
    int  list_count;
} WhereNode;

typedef struct {
    char column_names[MAX_SELECT_COLUMNS][MAX_NAME_LEN + 1];
    char column_table_prefix[MAX_SELECT_COLUMNS][MAX_NAME_LEN + 1];
    int  column_count;
    AggregateFunc column_aggregates[MAX_SELECT_COLUMNS];
    char column_aggregate_args[MAX_SELECT_COLUMNS][MAX_NAME_LEN + 1];
    int  column_aggregate_star[MAX_SELECT_COLUMNS];
    char table_name[MAX_NAME_LEN + 1];
    WhereNode* where;
    int  has_join;
    int  join_type; /* 0 = inner, 1 = left */
    char join_table_name[MAX_NAME_LEN + 1];
    char join_left_column[MAX_NAME_LEN + 1];
    char join_right_column[MAX_NAME_LEN + 1];
    int  join_left_column_count;
    int  join_right_column_count;
    int  has_group_by;
    char group_by_table_prefix[MAX_NAME_LEN + 1];
    char group_by_column[MAX_NAME_LEN + 1];
    int  has_order_by;
    char order_by_table_prefix[MAX_NAME_LEN + 1];
    char order_by_column[MAX_NAME_LEN + 1];
    int  order_by_desc;
    int  has_limit;
    int  limit_count;
} SelectStmt;

static Cell* resolve_field(Row* row, const char* prefix, const char* name,
                           SelectStmt* stmt);
static Cell* row_find_field(Row* row, const char* name);
static int   cell_compare(Cell* a, Cell* b);

/* -------------------------------------------------------------------------- */
/* WHERE expression parsing & evaluation                                      */
/*                                                                            */
/* Grammar (precedence: NOT > AND > OR):                                      */
/*   or       := and ( OR and )*                                              */
/*   and      := not ( AND not )*                                             */
/*   not      := NOT not | primary                                            */
/*   primary  := '(' or ')' | comparison                                      */
/*   comparison := ident[.ident] ( cmp_op literal                             */
/*               | IS [NOT] NULL                                              */
/*               | [NOT] IN ( literal, ... )                                  */
/*               | [NOT] LIKE 'pattern' )                                     */
/* LIKE is case-sensitive: '%' matches any sequence, '_' one character.       */
/* -------------------------------------------------------------------------- */

static void where_free(WhereNode* node) {
    if (node == NULL) return;
    where_free(node->left);
    where_free(node->right);
    if (cell_owns_text(node->literal.type)) {
        free(node->literal.as.as_string);
    }
    for (int i = 0; i < node->list_count; i++) {
        if (cell_owns_text(node->list[i].type)) {
            free(node->list[i].as.as_string);
        }
    }
    free(node);
}

static WhereNode* where_node_new(WhereType type) {
    WhereNode* node = calloc(1, sizeof(WhereNode));
    if (node != NULL) node->type = type;
    return node;
}

/* Parses the literal under *tok into out and advances *tok past it. */
/* Defined with the DDL parsers below; declared here because the WHERE parser
   is the first thing that needs it. */
static int sql_parse_literal_cell(SqlToken* tok, Cell* out);

static int where_parse_literal(SqlLexer* lex, SqlToken* tok, Cell* out) {
    if (!sql_parse_literal_cell(tok, out)) return 0;
    *tok = sql_next_token(lex);
    return 1;
}

static WhereNode* where_parse_or(SqlLexer* lex, SqlToken* tok);

static WhereNode* where_parse_primary(SqlLexer* lex, SqlToken* tok) {
    if (tok->type == TOK_LPAREN) {
        *tok = sql_next_token(lex);
        WhereNode* inner = where_parse_or(lex, tok);
        if (inner == NULL) return NULL;
        if (tok->type != TOK_RPAREN) {
            where_free(inner);
            return NULL;
        }
        *tok = sql_next_token(lex);
        return inner;
    }
    if (tok->type != TOK_IDENT) return NULL;

    WhereNode* node = where_node_new(WHERE_CMP);
    if (node == NULL) return NULL;

    char name_buf[MAX_NAME_LEN + 1];
    sql_token_text(tok, name_buf, sizeof(name_buf));
    *tok = sql_next_token(lex);
    if (tok->type == TOK_DOT) {
        strcpy(node->table_prefix, name_buf);
        *tok = sql_next_token(lex);
        if (tok->type != TOK_IDENT) {
            where_free(node);
            return NULL;
        }
        sql_token_text(tok, name_buf, sizeof(name_buf));
        *tok = sql_next_token(lex);
    }
    strcpy(node->column, name_buf);

    if (tok->type == TOK_IS) {
        *tok = sql_next_token(lex);
        int is_not = 0;
        if (tok->type == TOK_NOT) {
            is_not = 1;
            *tok = sql_next_token(lex);
        }
        if (tok->type != TOK_NULL) {
            where_free(node);
            return NULL;
        }
        *tok = sql_next_token(lex);
        node->type = is_not ? WHERE_IS_NOT_NULL : WHERE_IS_NULL;
        return node;
    }

    int is_not = 0;
    if (tok->type == TOK_NOT) {
        /* Only NOT IN / NOT LIKE may follow a column name. */
        is_not = 1;
        *tok = sql_next_token(lex);
    }

    if (tok->type == TOK_IN) {
        node->type = WHERE_IN;
        node->negate = is_not;
        *tok = sql_next_token(lex);
        if (tok->type != TOK_LPAREN) {
            where_free(node);
            return NULL;
        }
        *tok = sql_next_token(lex);
        while (tok->type != TOK_RPAREN) {
            if (node->list_count >= MAX_IN_LIST || tok->type == TOK_EOF) {
                where_free(node);
                return NULL;
            }
            if (!where_parse_literal(lex, tok, &node->list[node->list_count])) {
                where_free(node);
                return NULL;
            }
            node->list_count++;
            if (tok->type == TOK_COMMA) {
                *tok = sql_next_token(lex);
            } else if (tok->type != TOK_RPAREN) {
                where_free(node);
                return NULL;
            }
        }
        if (node->list_count == 0) {
            where_free(node);
            return NULL;
        }
        *tok = sql_next_token(lex); /* consume ')' */
        return node;
    }

    if (tok->type == TOK_LIKE) {
        node->type = WHERE_LIKE;
        node->negate = is_not;
        *tok = sql_next_token(lex);
        if ((tok->type != TOK_STRING && tok->type != TOK_PARAM) ||
            !where_parse_literal(lex, tok, &node->literal) ||
            node->literal.type != VAL_STRING) {
            where_free(node);
            return NULL;
        }
        return node;
    }

    if (is_not) {
        where_free(node);
        return NULL;
    }

    if (tok->type == TOK_EQ) node->cmp_op = 0;
    else if (tok->type == TOK_LT) node->cmp_op = 1;
    else if (tok->type == TOK_GT) node->cmp_op = 2;
    else if (tok->type == TOK_LE) node->cmp_op = 3;
    else if (tok->type == TOK_GE) node->cmp_op = 4;
    else if (tok->type == TOK_NE) node->cmp_op = 5;
    else {
        where_free(node);
        return NULL;
    }

    *tok = sql_next_token(lex);
    if (!where_parse_literal(lex, tok, &node->literal)) {
        where_free(node);
        return NULL;
    }
    return node;
}

static WhereNode* where_parse_not(SqlLexer* lex, SqlToken* tok) {
    if (tok->type == TOK_NOT) {
        *tok = sql_next_token(lex);
        WhereNode* operand = where_parse_not(lex, tok);
        if (operand == NULL) return NULL;
        WhereNode* node = where_node_new(WHERE_NOT);
        if (node == NULL) {
            where_free(operand);
            return NULL;
        }
        node->left = operand;
        return node;
    }
    return where_parse_primary(lex, tok);
}

static WhereNode* where_parse_and(SqlLexer* lex, SqlToken* tok) {
    WhereNode* left = where_parse_not(lex, tok);
    if (left == NULL) return NULL;
    while (tok->type == TOK_AND) {
        *tok = sql_next_token(lex);
        WhereNode* right = where_parse_not(lex, tok);
        if (right == NULL) {
            where_free(left);
            return NULL;
        }
        WhereNode* node = where_node_new(WHERE_AND);
        if (node == NULL) {
            where_free(left);
            where_free(right);
            return NULL;
        }
        node->left = left;
        node->right = right;
        left = node;
    }
    return left;
}

static WhereNode* where_parse_or(SqlLexer* lex, SqlToken* tok) {
    WhereNode* left = where_parse_and(lex, tok);
    if (left == NULL) return NULL;
    while (tok->type == TOK_OR) {
        *tok = sql_next_token(lex);
        WhereNode* right = where_parse_and(lex, tok);
        if (right == NULL) {
            where_free(left);
            return NULL;
        }
        WhereNode* node = where_node_new(WHERE_OR);
        if (node == NULL) {
            where_free(left);
            where_free(right);
            return NULL;
        }
        node->left = left;
        node->right = right;
        left = node;
    }
    return left;
}

/* LIKE matcher: '%' matches any (possibly empty) sequence, '_' matches
   exactly one character. Matching is case-sensitive. */
static int like_match(const char* pattern, const char* text) {
    while (*pattern != '\0') {
        if (*pattern == '%') {
            pattern++;
            if (*pattern == '\0') return 1;
            for (;;) {
                if (like_match(pattern, text)) return 1;
                if (*text == '\0') return 0;
                text++;
            }
        }
        if (*text == '\0') return 0;
        if (*pattern != '_' && *pattern != *text) return 0;
        pattern++;
        text++;
    }
    return *text == '\0';
}

static Cell* where_resolve(Row* row, WhereNode* node, SelectStmt* stmt) {
    if (stmt != NULL) {
        return resolve_field(row, node->table_prefix, node->column, stmt);
    }
    /* UPDATE/DELETE rows come from a single table; resolve by name only. */
    return row_find_field(row, node->column);
}

/* Three-valued evaluation: 1 = true, 0 = false, -1 = unknown.
   Rows only pass a WHERE filter when the result is 1. */
static int where_eval(Row* row, WhereNode* node, SelectStmt* stmt) {
    switch (node->type) {
        case WHERE_AND: {
            int l = where_eval(row, node->left, stmt);
            if (l == 0) return 0;
            int r = where_eval(row, node->right, stmt);
            if (r == 0) return 0;
            if (l < 0 || r < 0) return -1;
            return 1;
        }
        case WHERE_OR: {
            int l = where_eval(row, node->left, stmt);
            if (l == 1) return 1;
            int r = where_eval(row, node->right, stmt);
            if (r == 1) return 1;
            if (l < 0 || r < 0) return -1;
            return 0;
        }
        case WHERE_NOT: {
            int v = where_eval(row, node->left, stmt);
            if (v < 0) return -1;
            return !v;
        }
        case WHERE_IS_NULL:
        case WHERE_IS_NOT_NULL: {
            Cell* value = where_resolve(row, node, stmt);
            if (value == NULL) return -1;
            int is_null = value->type == VAL_NULL;
            return node->type == WHERE_IS_NULL ? is_null : !is_null;
        }
        case WHERE_CMP: {
            Cell* value = where_resolve(row, node, stmt);
            if (value == NULL) return -1;
            /* Three-valued logic: any comparison against NULL is unknown. */
            if (value->type == VAL_NULL || node->literal.type == VAL_NULL) return -1;
            int cmp = cell_compare(value, &node->literal);
            switch (node->cmp_op) {
                case 0: return cmp == 0;
                case 1: return cmp < 0;
                case 2: return cmp > 0;
                case 3: return cmp <= 0;
                case 4: return cmp >= 0;
                case 5: return cmp != 0;
                default: return -1;
            }
        }
        case WHERE_IN: {
            Cell* value = where_resolve(row, node, stmt);
            if (value == NULL || value->type == VAL_NULL) return -1;
            int saw_null = 0;
            for (int i = 0; i < node->list_count; i++) {
                if (node->list[i].type == VAL_NULL) {
                    saw_null = 1;
                    continue;
                }
                if (cell_compare(value, &node->list[i]) == 0) {
                    return node->negate ? 0 : 1;
                }
            }
            if (saw_null) return -1;
            return node->negate ? 1 : 0;
        }
        case WHERE_LIKE: {
            Cell* value = where_resolve(row, node, stmt);
            if (value == NULL || value->type == VAL_NULL) return -1;
            if (value->type != VAL_STRING) return -1;
            int matched = like_match(node->literal.as.as_string, value->as.as_string);
            return node->negate ? !matched : matched;
        }
    }
    return -1;
}


static int sql_parse_select(const char* query, SelectStmt* stmt) {
    memset(stmt, 0, sizeof(*stmt));

    SqlLexer lex;
    sql_lexer_init(&lex, query);

    SqlToken tok = sql_next_token(&lex);
    if (tok.type != TOK_SELECT) return 0;

    tok = sql_next_token(&lex);
    if (tok.type == TOK_STAR) {
        stmt->column_count = 0;
        tok = sql_next_token(&lex);
    } else {
        while (1) {
            if (stmt->column_count >= MAX_SELECT_COLUMNS) return 0;

            AggregateFunc agg = AGG_NONE;
            if (tok.type == TOK_COUNT) agg = AGG_COUNT;
            else if (tok.type == TOK_SUM) agg = AGG_SUM;
            else if (tok.type == TOK_AVG) agg = AGG_AVG;
            else if (tok.type == TOK_MIN) agg = AGG_MIN;
            else if (tok.type == TOK_MAX) agg = AGG_MAX;

            if (agg != AGG_NONE) {
                stmt->column_aggregates[stmt->column_count] = agg;
                tok = sql_next_token(&lex);
                if (tok.type != TOK_LPAREN) return 0;
                tok = sql_next_token(&lex);
                if (tok.type == TOK_STAR) {
                    stmt->column_aggregate_star[stmt->column_count] = 1;
                    strcpy(stmt->column_aggregate_args[stmt->column_count], "*");
                    tok = sql_next_token(&lex);
                } else if (tok.type == TOK_IDENT) {
                    sql_token_text(&tok, stmt->column_aggregate_args[stmt->column_count],
                                   sizeof(stmt->column_aggregate_args[0]));
                    tok = sql_next_token(&lex);
                } else {
                    return 0;
                }
                if (tok.type != TOK_RPAREN) return 0;
                switch (agg) {
                    case AGG_COUNT: strcpy(stmt->column_names[stmt->column_count], "count"); break;
                    case AGG_SUM:   strcpy(stmt->column_names[stmt->column_count], "sum"); break;
                    case AGG_AVG:   strcpy(stmt->column_names[stmt->column_count], "avg"); break;
                    case AGG_MIN:   strcpy(stmt->column_names[stmt->column_count], "min"); break;
                    case AGG_MAX:   strcpy(stmt->column_names[stmt->column_count], "max"); break;
                    default: break;
                }
                stmt->column_count++;
                tok = sql_next_token(&lex);
            } else if (tok.type == TOK_IDENT) {
                char prefix_buf[MAX_NAME_LEN + 1] = "";
                char name_buf[MAX_NAME_LEN + 1];
                sql_token_text(&tok, name_buf, sizeof(name_buf));

                tok = sql_next_token(&lex);
                if (tok.type == TOK_DOT) {
                    SqlToken col_tok = sql_next_token(&lex);
                    if (col_tok.type != TOK_IDENT) return 0;
                    strcpy(prefix_buf, name_buf);
                    sql_token_text(&col_tok, name_buf, sizeof(name_buf));
                    tok = sql_next_token(&lex);
                }

                strcpy(stmt->column_names[stmt->column_count], name_buf);
                strcpy(stmt->column_table_prefix[stmt->column_count], prefix_buf);
                stmt->column_count++;
            } else {
                return 0;
            }

            if (tok.type == TOK_COMMA) {
                tok = sql_next_token(&lex);
            } else {
                break;
            }
        }
    }

    if (tok.type != TOK_FROM) return 0;

    tok = sql_next_token(&lex);
    if (tok.type != TOK_IDENT) return 0;
    sql_token_text(&tok, stmt->table_name, sizeof(stmt->table_name));

    tok = sql_next_token(&lex);
    if (tok.type == TOK_JOIN || tok.type == TOK_LEFT) {
        if (tok.type == TOK_LEFT) {
            stmt->join_type = 1;
            tok = sql_next_token(&lex);
            if (tok.type == TOK_OUTER) {
                tok = sql_next_token(&lex);
            }
            if (tok.type != TOK_JOIN) return 0;
            tok = sql_next_token(&lex);
        } else {
            stmt->join_type = 0;
            tok = sql_next_token(&lex);
        }
        if (tok.type != TOK_IDENT) return 0;
        sql_token_text(&tok, stmt->join_table_name, sizeof(stmt->join_table_name));
        stmt->has_join = 1;

        tok = sql_next_token(&lex);
        if (tok.type != TOK_ON) return 0;

        tok = sql_next_token(&lex);
        if (tok.type != TOK_IDENT) return 0;
        char left_table[MAX_NAME_LEN + 1];
        sql_token_text(&tok, left_table, sizeof(left_table));
        tok = sql_next_token(&lex);
        if (tok.type != TOK_DOT) return 0;
        tok = sql_next_token(&lex);
        if (tok.type != TOK_IDENT) return 0;
        sql_token_text(&tok, stmt->join_left_column, sizeof(stmt->join_left_column));

        tok = sql_next_token(&lex);
        if (tok.type != TOK_EQ) return 0;

        tok = sql_next_token(&lex);
        if (tok.type != TOK_IDENT) return 0;
        char right_table[MAX_NAME_LEN + 1];
        sql_token_text(&tok, right_table, sizeof(right_table));
        tok = sql_next_token(&lex);
        if (tok.type != TOK_DOT) return 0;
        tok = sql_next_token(&lex);
        if (tok.type != TOK_IDENT) return 0;
        sql_token_text(&tok, stmt->join_right_column, sizeof(stmt->join_right_column));

        if (strcmp(left_table, stmt->table_name) != 0 ||
            strcmp(right_table, stmt->join_table_name) != 0) {
            return 0;
        }

        tok = sql_next_token(&lex);
    }

    if (tok.type == TOK_WHERE) {
        tok = sql_next_token(&lex);
        stmt->where = where_parse_or(&lex, &tok);
        if (stmt->where == NULL) return 0;
    }

    if (tok.type == TOK_GROUP) {
        tok = sql_next_token(&lex);
        if (tok.type != TOK_BY) return 0;
        tok = sql_next_token(&lex);
        if (tok.type != TOK_IDENT) return 0;
        char group_prefix_buf[MAX_NAME_LEN + 1] = "";
        char group_name_buf[MAX_NAME_LEN + 1];
        sql_token_text(&tok, group_name_buf, sizeof(group_name_buf));

        tok = sql_next_token(&lex);
        if (tok.type == TOK_DOT) {
            SqlToken col_tok = sql_next_token(&lex);
            if (col_tok.type != TOK_IDENT) return 0;
            strcpy(group_prefix_buf, group_name_buf);
            sql_token_text(&col_tok, group_name_buf, sizeof(group_name_buf));
            tok = sql_next_token(&lex);
        }
        strcpy(stmt->group_by_table_prefix, group_prefix_buf);
        strcpy(stmt->group_by_column, group_name_buf);
        stmt->has_group_by = 1;
    }

    if (tok.type == TOK_ORDER) {
        tok = sql_next_token(&lex);
        if (tok.type != TOK_BY) return 0;
        tok = sql_next_token(&lex);
        if (tok.type != TOK_IDENT) return 0;
        char order_prefix_buf[MAX_NAME_LEN + 1] = "";
        char order_name_buf[MAX_NAME_LEN + 1];
        sql_token_text(&tok, order_name_buf, sizeof(order_name_buf));

        tok = sql_next_token(&lex);
        if (tok.type == TOK_DOT) {
            SqlToken col_tok = sql_next_token(&lex);
            if (col_tok.type != TOK_IDENT) return 0;
            strcpy(order_prefix_buf, order_name_buf);
            sql_token_text(&col_tok, order_name_buf, sizeof(order_name_buf));
            tok = sql_next_token(&lex);
        }
        strcpy(stmt->order_by_table_prefix, order_prefix_buf);
        strcpy(stmt->order_by_column, order_name_buf);
        stmt->has_order_by = 1;
        if (tok.type == TOK_ASC) {
            stmt->order_by_desc = 0;
            tok = sql_next_token(&lex);
        } else if (tok.type == TOK_DESC) {
            stmt->order_by_desc = 1;
            tok = sql_next_token(&lex);
        }
    }

    if (tok.type == TOK_LIMIT) {
        tok = sql_next_token(&lex);
        if (tok.type == TOK_PARAM) {
            Cell bound;
            if (!sql_param_cell(&tok, &bound)) return 0;
            if (bound.type != VAL_INT) {
                if (bound.type == VAL_STRING) free(bound.as.as_string);
                return 0;
            }
            stmt->limit_count = bound.as.as_int;
        } else if (tok.type == TOK_NUMBER) {
            char buf[64];
            sql_token_text(&tok, buf, sizeof(buf));
            stmt->limit_count = atoi(buf);
        } else {
            return 0;
        }
        if (stmt->limit_count < 0) stmt->limit_count = 0;
        stmt->has_limit = 1;
        tok = sql_next_token(&lex);
    }

    return tok.type == TOK_EOF;
}

static void sql_free_select_stmt(SelectStmt* stmt) {
    where_free(stmt->where);
    stmt->where = NULL;
}

static Cell cell_from_int(int v) {
    Cell c;
    c.type = VAL_INT;
    c.as.as_int = v;
    return c;
}

static int cell_compare(Cell* a, Cell* b) {
    /* Deterministic ordering for NULLs (sorts/grouping only): NULL is
       smaller than any non-NULL value. WHERE evaluation handles NULL
       separately with three-valued logic before calling this. */
    if (a->type == VAL_NULL && b->type == VAL_NULL) return 0;
    if (a->type == VAL_NULL) return -1;
    if (b->type == VAL_NULL) return 1;
    if (a->type == VAL_INT && b->type == VAL_INT) {
        if (a->as.as_int < b->as.as_int) return -1;
        if (a->as.as_int > b->as.as_int) return 1;
        return 0;
    }
    if (a->type == VAL_FLOAT && b->type == VAL_FLOAT) {
        if (a->as.as_float < b->as.as_float) return -1;
        if (a->as.as_float > b->as.as_float) return 1;
        return 0;
    }
    if (cell_owns_text(a->type) && cell_owns_text(b->type)) {
        /* Dates and timestamps are canonical fixed-width text, so byte order
           is chronological order, and a plain string literal compares against
           one without being coerced first. */
        return strcmp(a->as.as_string != NULL ? a->as.as_string : "",
                      b->as.as_string != NULL ? b->as.as_string : "");
    }
    /* false sorts before true, and a bool compares with an int by its 0/1
       value, which is how the runtime already treats it. */
    if (a->type == VAL_BOOL && b->type == VAL_BOOL) {
        return a->as.as_int - b->as.as_int;
    }
    if ((a->type == VAL_BOOL && b->type == VAL_INT) ||
        (a->type == VAL_INT && b->type == VAL_BOOL)) {
        return a->as.as_int - b->as.as_int;
    }
    if (a->type == VAL_FLOAT || b->type == VAL_FLOAT) {
        double av = (a->type == VAL_FLOAT) ? a->as.as_float : (double)a->as.as_int;
        double bv = (b->type == VAL_FLOAT) ? b->as.as_float : (double)b->as.as_int;
        if (av < bv) return -1;
        if (av > bv) return 1;
        return 0;
    }
    return 0;
}

static int evaluate_where(Row* row, SelectStmt* stmt) {
    return where_eval(row, stmt->where, stmt) == 1;
}

static void free_rows(Row* rows, int count) {
    if (rows == NULL) return;
    for (int i = 0; i < count; i++) {
        for (int j = 0; j < rows[i].field_count; j++) {
            free(rows[i].fields[j].name);
            if (cell_owns_text(rows[i].fields[j].value.type)) {
                free(rows[i].fields[j].value.as.as_string);
            }
        }
        free(rows[i].fields);
    }
    free(rows);
}

static Cell cell_dup(Cell* src) {
    Cell dst;
    dst.type = src->type;
    if (cell_owns_text(src->type)) {
        dst.as.as_string = strdup(src->as.as_string != NULL ? src->as.as_string : "");
    } else if (src->type == VAL_FLOAT) {
        dst.as.as_float = src->as.as_float;
    } else {
        dst.as.as_int = src->as.as_int;
    }
    return dst;
}

static Row row_combine(Row* a, Row* b) {
    Row combined;
    combined.field_count = a->field_count + b->field_count;
    combined.fields = calloc((size_t)combined.field_count, sizeof(Field));
    if (combined.fields == NULL) {
        combined.field_count = 0;
        return combined;
    }

    int k = 0;
    for (int i = 0; i < a->field_count; i++) {
        combined.fields[k].name = strdup(a->fields[i].name);
        combined.fields[k].value = cell_dup(&a->fields[i].value);
        k++;
    }
    for (int i = 0; i < b->field_count; i++) {
        combined.fields[k].name = strdup(b->fields[i].name);
        combined.fields[k].value = cell_dup(&b->fields[i].value);
        k++;
    }

    return combined;
}

static Row row_zero(Table* table) {
    Row row;
    row.field_count = table->column_count;
    row.fields = calloc((size_t)row.field_count, sizeof(Field));
    if (row.fields == NULL) {
        row.field_count = 0;
        return row;
    }
    for (int i = 0; i < row.field_count; i++) {
        row.fields[i].name = strdup(table->columns[i].name);
        row.fields[i].value.type = table->columns[i].type;
        if (cell_owns_text(table->columns[i].type)) {
            row.fields[i].value.as.as_string = strdup("");
        } else if (table->columns[i].type == VAL_FLOAT) {
            row.fields[i].value.as.as_float = 0.0;
        } else {
            row.fields[i].value.as.as_int = 0;
        }
    }
    return row;
}

/* Frees a row's fields but not the Row itself, which may live on the stack
   (row_zero) or inside a rows array. A Row from deserialize_row also needs
   free(row). */
static void free_row(Row* row) {
    if (row == NULL) return;
    for (int i = 0; i < row->field_count; i++) {
        free(row->fields[i].name);
        if (cell_owns_text(row->fields[i].value.type)) {
            free(row->fields[i].value.as.as_string);
        }
    }
    free(row->fields);
}

static int evaluate_join(Row* left, Row* right, SelectStmt* stmt) {
    Cell* left_value = NULL;
    for (int i = 0; i < left->field_count; i++) {
        if (strcmp(left->fields[i].name, stmt->join_left_column) == 0) {
            left_value = &left->fields[i].value;
            break;
        }
    }
    if (left_value == NULL) return 0;

    Cell* right_value = NULL;
    for (int i = 0; i < right->field_count; i++) {
        if (strcmp(right->fields[i].name, stmt->join_right_column) == 0) {
            right_value = &right->fields[i].value;
            break;
        }
    }
    if (right_value == NULL) return 0;

    /* NULL join keys never match (three-valued logic). */
    if (left_value->type == VAL_NULL || right_value->type == VAL_NULL) return 0;

    return cell_compare(left_value, right_value) == 0;
}

static Result* result_create(int capacity) {
    Result* res = calloc(1, sizeof(Result));
    if (res == NULL) return NULL;
    if (capacity > 0) {
        res->rows = calloc((size_t)capacity, sizeof(Row));
        if (res->rows == NULL) {
            free(res);
            return NULL;
        }
    }
    return res;
}

static void result_append(Result* res, Row* src, SelectStmt* stmt) {
    Row* dst = &res->rows[res->row_count];
    res->row_count++;

    int count;
    if (stmt->column_count == 0) {
        count = src->field_count;
    } else {
        count = stmt->column_count;
    }

    dst->field_count = count;
    dst->fields = calloc((size_t)count, sizeof(Field));
    if (dst->fields == NULL) return;

    for (int i = 0; i < count; i++) {
        const char* name;
        Cell* value;
        if (stmt->column_count == 0) {
            name = src->fields[i].name;
            value = &src->fields[i].value;
        } else {
            name = stmt->column_names[i];
            value = resolve_field(src, stmt->column_table_prefix[i], name, stmt);
            if (value == NULL) {
                dst->fields[i].name = strdup(name);
                dst->fields[i].value = cell_from_int(0);
                continue;
            }
        }

        dst->fields[i].name = strdup(name);
        dst->fields[i].value.type = value->type;
        if (cell_owns_text(value->type)) {
            dst->fields[i].value.as.as_string = strdup(value->as.as_string);
        } else if (value->type == VAL_FLOAT) {
            dst->fields[i].value.as.as_float = value->as.as_float;
        } else {
            dst->fields[i].value.as.as_int = value->as.as_int;
        }
    }
}

static Cell* row_find_field(Row* row, const char* name) {
    for (int i = 0; i < row->field_count; i++) {
        if (strcmp(row->fields[i].name, name) == 0) {
            return &row->fields[i].value;
        }
    }
    return NULL;
}

static Cell* resolve_field(Row* row, const char* prefix, const char* name,
                           SelectStmt* stmt) {
    if (prefix != NULL && prefix[0] != '\0') {
        int start = 0;
        int end = row->field_count;
        if (stmt->has_join) {
            if (strcmp(prefix, stmt->table_name) == 0) {
                end = stmt->join_left_column_count;
            } else if (strcmp(prefix, stmt->join_table_name) == 0) {
                start = stmt->join_left_column_count;
                end = stmt->join_left_column_count + stmt->join_right_column_count;
            } else {
                return NULL;
            }
        } else {
            if (strcmp(prefix, stmt->table_name) != 0) {
                return NULL;
            }
        }
        for (int i = start; i < end && i < row->field_count; i++) {
            if (strcmp(row->fields[i].name, name) == 0) {
                return &row->fields[i].value;
            }
        }
        return NULL;
    }
    return row_find_field(row, name);
}

static Cell zero_cell(void) {
    Cell c;
    c.type = VAL_INT;
    c.as.as_int = 0;
    return c;
}

/* Portable in-place insertion sort for Row* arrays. Avoids qsort_r which has
   incompatible signatures between GNU libc and BSD/macOS. */
static void row_ptr_sort(Row** rows, int count,
                         int (*compare)(Row* a, Row* b, void* arg),
                         void* arg) {
    for (int i = 1; i < count; i++) {
        Row* key = rows[i];
        int j = i - 1;
        while (j >= 0 && compare(rows[j], key, arg) > 0) {
            rows[j + 1] = rows[j];
            j--;
        }
        rows[j + 1] = key;
    }
}

static int filtered_row_compare(Row* a, Row* b, void* arg) {
    SelectStmt* stmt = (SelectStmt*)arg;
    Cell* ca = resolve_field(a, stmt->order_by_table_prefix, stmt->order_by_column, stmt);
    Cell* cb = resolve_field(b, stmt->order_by_table_prefix, stmt->order_by_column, stmt);
    Cell z = zero_cell();
    int cmp = cell_compare(ca != NULL ? ca : &z, cb != NULL ? cb : &z);
    return stmt->order_by_desc ? -cmp : cmp;
}

static int group_row_compare(Row* a, Row* b, void* arg) {
    SelectStmt* stmt = (SelectStmt*)arg;
    Cell* ca = resolve_field(a, stmt->group_by_table_prefix, stmt->group_by_column, stmt);
    Cell* cb = resolve_field(b, stmt->group_by_table_prefix, stmt->group_by_column, stmt);
    Cell z = zero_cell();
    return cell_compare(ca != NULL ? ca : &z, cb != NULL ? cb : &z);
}

static void result_limit(Result* res, int limit) {
    if (res == NULL || limit < 0) return;
    if (limit < res->row_count) {
        for (int i = limit; i < res->row_count; i++) {
            Row* row = &res->rows[i];
            for (int j = 0; j < row->field_count; j++) {
                free(row->fields[j].name);
                if (cell_owns_text(row->fields[j].value.type)) {
                    free(row->fields[j].value.as.as_string);
                }
            }
            free(row->fields);
        }
        res->row_count = limit;
    }
}

static int stmt_has_aggregates(SelectStmt* stmt) {
    for (int i = 0; i < stmt->column_count; i++) {
        if (stmt->column_aggregates[i] != AGG_NONE) return 1;
    }
    return 0;
}

static Cell* source_row_find_field(Row* row, const char* name) {
    for (int i = 0; i < row->field_count; i++) {
        if (strcmp(row->fields[i].name, name) == 0) {
            return &row->fields[i].value;
        }
    }
    return NULL;
}

static Cell compute_aggregate(AggregateFunc agg, Row** rows, int row_count,
                              const char* arg) {
    Cell result;
    if (agg == AGG_COUNT) {
        /* COUNT(*) counts rows; COUNT(col) skips NULL values (SQLite-style). */
        result.type = VAL_INT;
        if (strcmp(arg, "*") == 0) {
            result.as.as_int = row_count;
            return result;
        }
        int non_null = 0;
        for (int i = 0; i < row_count; i++) {
            Cell* value = source_row_find_field(rows[i], arg);
            if (value != NULL && value->type != VAL_NULL) non_null++;
        }
        result.as.as_int = non_null;
        return result;
    }

    if (row_count == 0) {
        result.type = VAL_INT;
        result.as.as_int = 0;
        return result;
    }

    if (agg == AGG_SUM || agg == AGG_AVG) {
        double sum = 0;
        int numeric_count = 0;
        for (int i = 0; i < row_count; i++) {
            Cell* value = source_row_find_field(rows[i], arg);
            if (value == NULL || value->type == VAL_NULL) continue;
            if (value->type == VAL_INT) {
                sum += value->as.as_int;
                numeric_count++;
            } else if (value->type == VAL_FLOAT) {
                sum += value->as.as_float;
                numeric_count++;
            }
        }
        if (agg == AGG_AVG && numeric_count > 0) {
            result.type = VAL_FLOAT;
            result.as.as_float = sum / numeric_count;
        } else {
            result.type = VAL_FLOAT;
            result.as.as_float = sum;
        }
        return result;
    }

    if (agg == AGG_MIN || agg == AGG_MAX) {
        /* MIN/MAX skip NULLs; if every value is NULL the result is NULL. */
        int found = 0;
        result.type = VAL_NULL;
        result.as.as_int = 0;
        for (int i = 0; i < row_count; i++) {
            Cell* value = source_row_find_field(rows[i], arg);
            if (value == NULL || value->type == VAL_NULL) continue;
            if (!found) {
                result = *value;
                found = 1;
                continue;
            }
            int cmp = cell_compare(value, &result);
            if ((agg == AGG_MIN && cmp < 0) || (agg == AGG_MAX && cmp > 0)) {
                result = *value;
            }
        }
        return result;
    }

    result.type = VAL_INT;
    result.as.as_int = 0;
    return result;
}

static void result_append_aggregate(Result* res, SelectStmt* stmt,
                                    Row** rows, int row_count) {
    Row* dst = &res->rows[res->row_count++];
    dst->field_count = stmt->column_count;
    dst->fields = calloc((size_t)stmt->column_count, sizeof(Field));
    if (dst->fields == NULL) return;

    for (int i = 0; i < stmt->column_count; i++) {
        dst->fields[i].name = strdup(stmt->column_names[i]);
        if (stmt->column_aggregates[i] != AGG_NONE) {
            Cell agg_value = compute_aggregate(stmt->column_aggregates[i], rows,
                                               row_count,
                                               stmt->column_aggregate_args[i]);
            dst->fields[i].value = cell_dup(&agg_value);
        } else if (row_count > 0) {
            Cell* value = resolve_field(rows[0], stmt->column_table_prefix[i],
                                        stmt->column_names[i], stmt);
            if (value != NULL) {
                dst->fields[i].value = cell_dup(value);
            } else {
                dst->fields[i].value = cell_from_int(0);
            }
        } else {
            dst->fields[i].value = cell_from_int(0);
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Index-assisted row lookup                                                  */
/*                                                                            */
/* When a top-level AND-term of the WHERE clause compares an indexed column   */
/* against a literal, the index supplies candidate rows instead of a full     */
/* scan. The complete WHERE clause is still evaluated on every candidate,     */
/* so the result is identical to a full scan. Supported cases:                */
/*   - string column  = string literal (equality only)                        */
/*   - numeric column <cmp> numeric literal (=, <, <=, >, >=)                 */
/* Numeric lookups scan the int and float key spaces separately; bounds are   */
/* widened conservatively for float literals, so candidates are a superset    */
/* of the matches. Anything else falls back to a full scan.                   */
/* -------------------------------------------------------------------------- */

typedef struct {
    Context* ctx;
    Table*   table;
    Row*     rows;
    int      count;
    int      capacity;
    int      failed;
} IndexScanCtx;

static void index_scan_collect(int row_page, int row_offset, void* user) {
    IndexScanCtx* scan = (IndexScanCtx*)user;
    if (scan->failed) return;
    if (row_offset < ROW_PAGE_HEADER_SIZE || row_offset >= PAGE_SIZE) {
        scan->failed = 1;
        return;
    }
    uint8_t page[PAGE_SIZE];
    pager_read_page(scan->ctx->pager, row_page, page);
    int16_t record_size;
    memcpy(&record_size, page + row_offset, sizeof(record_size));
    if (record_size <= 0 || row_offset + (int)record_size > PAGE_SIZE) {
        scan->failed = 1;
        return;
    }
    Row* row = deserialize_row(scan->table, page + row_offset);
    if (row == NULL) {
        scan->failed = 1;
        return;
    }
    if (scan->count >= scan->capacity) {
        int new_capacity = scan->capacity == 0 ? 8 : scan->capacity * 2;
        Row* new_rows = realloc(scan->rows, (size_t)new_capacity * sizeof(Row));
        if (new_rows == NULL) {
            free_row(row);
            free(row);
            scan->failed = 1;
            return;
        }
        scan->rows = new_rows;
        scan->capacity = new_capacity;
    }
    scan->rows[scan->count++] = *row;
    free(row);
}

#define MAX_INDEX_CONJUNCTS 16

static void collect_and_terms(WhereNode* node, WhereNode** out, int* count) {
    if (node == NULL || *count >= MAX_INDEX_CONJUNCTS) return;
    if (node->type == WHERE_AND) {
        collect_and_terms(node->left, out, count);
        collect_and_terms(node->right, out, count);
        return;
    }
    out[(*count)++] = node;
}

static int conjunct_matches_index(WhereNode* node, Table* table, TableIndex* idx) {
    if (node->type != WHERE_CMP) return 0;
    if (node->cmp_op == 5) return 0; /* <> cannot be served by an index scan */
    if (strcmp(node->column, idx->column_name) != 0) return 0;
    if (node->table_prefix[0] != '\0' && strcmp(node->table_prefix, table->name) != 0) {
        return 0;
    }
    if (node->literal.type == VAL_NULL) return 0; /* three-valued: matches nothing */
    int column = table_column_index(table, idx->column_name);
    if (column < 0) return 0;
    int col_type = table->columns[column].type;
    if (node->literal.type == VAL_STRING) {
        /* Equality and ranges are both servable: index_scan_string widens a
           truncated bound so the candidate set stays a superset. Dates and
           timestamps are written as string literals and share the string key
           space, and their canonical forms are fixed width well under the key
           cap, so they qualify on the same terms. */
        return col_type == VAL_STRING || col_type == VAL_DATE ||
               col_type == VAL_TIMESTAMP;
    }
    if (node->literal.type == VAL_BOOL) {
        /* Two distinct keys, so only equality is worth serving. */
        return col_type == VAL_BOOL && node->cmp_op == 0;
    }
    return col_type == VAL_INT || col_type == VAL_FLOAT;
}

/* Runs one pass over the int key space and one over the float key space.
   Each pass is bounded inside its own space so no cell is reported twice:
   the int space spans [INT32_MIN, INT32_MAX], the float space [-inf, +inf]. */
static int index_scan_numeric(BTree* tree, WhereNode* node, IndexScanCtx* scan) {
    Cell* lit = &node->literal;
    double dval = lit->type == VAL_FLOAT ? lit->as.as_float : (double)lit->as.as_int;
    int    ival = lit->type == VAL_INT ? lit->as.as_int : (int)dval;

    for (int space = 0; space < 2; space++) {
        Cell key;
        Cell lo_bound;
        Cell hi_bound;
        if (space == 0) {
            key.type = VAL_INT;
            key.as.as_int = ival;
            lo_bound.type = VAL_INT;
            lo_bound.as.as_int = INT32_MIN;
            hi_bound.type = VAL_INT;
            hi_bound.as.as_int = INT32_MAX;
        } else {
            key.type = VAL_FLOAT;
            key.as.as_float = dval;
            lo_bound.type = VAL_FLOAT;
            lo_bound.as.as_float = -INFINITY;
            hi_bound.type = VAL_FLOAT;
            hi_bound.as.as_float = INFINITY;
        }

        if (node->cmp_op == 0) {
            if (space == 0 && lit->type == VAL_FLOAT && dval != (double)ival) {
                continue; /* no int cell can equal a non-integral float */
            }
            if (btree_scan_eq(tree, &key, index_scan_collect, scan) < 0) return 0;
            continue;
        }

        Cell* lo = &lo_bound;
        Cell* hi = &hi_bound;
        int lo_inc = 1;
        int hi_inc = 1;
        switch (node->cmp_op) {
            case 1: hi = &key; hi_inc = 0; break; /* <  */
            case 2: lo = &key; lo_inc = 0; break; /* >  */
            case 3: hi = &key; hi_inc = 1; break; /* <= */
            case 4: lo = &key; lo_inc = 1; break; /* >= */
            default: return 0;
        }
        /* Widen float-literal bounds in the int space so no satisfying int
           cell can be missed (truncation of the bound would be exact only
           for integral literals). */
        if (space == 0 && lit->type == VAL_FLOAT) {
            if (lo == &key) {
                key.as.as_int = ival - 1;
                lo_inc = 1;
            }
            if (hi == &key) {
                key.as.as_int = ival + 1;
                hi_inc = 1;
            }
        }
        if (btree_scan_range(tree, lo, lo_inc, hi, hi_inc,
                             index_scan_collect, scan) < 0) return 0;
    }
    return 1;
}

/* Serves =, <, <=, > and >= on an indexed string column.

   Only the first BTREE_STRING_KEY_BYTES bytes of a string reach the key, and
   truncation is monotone: if value < literal then key(value) <= key(literal).
   A bound taken from a literal the key cannot hold in full is therefore only a
   prefix of the bound the caller asked for, and excluding it would drop rows
   that share that prefix - so such a bound is widened to inclusive. The extra
   candidates that admits are dropped by the WHERE pass this function's caller
   runs over the rows. A literal shorter than the key width has its key to
   itself and keeps the operator's own strictness.

   The open end of a range is fenced with the empty string rather than left
   unbounded, because the int, float and NULL key spaces sort below the string
   space and must stay out of the scan. Nothing sorts above strings, so the
   upper end can be left open. */
static int index_scan_string(BTree* tree, WhereNode* node, IndexScanCtx* scan) {
    Cell* lit = &node->literal;
    if (node->cmp_op == 0) {
        return btree_scan_eq(tree, lit, index_scan_collect, scan) >= 0;
    }

    const char* text = lit->as.as_string != NULL ? lit->as.as_string : "";
    /* The test is >=, not >: a literal of exactly the key width still shares
       its key with every longer value that starts with it, and those values
       are on the far side of a > or < bound. Only a literal strictly shorter
       than the key width is guaranteed to have the key to itself, because a
       shorter key is zero-padded and strings hold no NUL bytes. */
    int exact = strlen(text) < BTREE_STRING_KEY_BYTES;

    char empty[1] = "";
    Cell floor_bound;
    floor_bound.type = VAL_STRING;
    floor_bound.as.as_string = empty;

    Cell* lo = &floor_bound;
    Cell* hi = NULL;
    int lo_inclusive = 1;
    int hi_inclusive = 1;
    switch (node->cmp_op) {
        case 1: hi = lit; hi_inclusive = !exact; break; /* <  */
        case 2: lo = lit; lo_inclusive = !exact; break; /* >  */
        case 3: hi = lit; break;                        /* <= */
        case 4: lo = lit; break;                        /* >= */
        default: return 0;
    }
    return btree_scan_range(tree, lo, lo_inclusive, hi, hi_inclusive,
                            index_scan_collect, scan) >= 0;
}

/* Returns 1 when an index produced the candidate rows, 0 to fall back to a
   full scan. */
static int try_index_lookup(Context* ctx, Table* table, WhereNode* where,
                            Row** out_rows, int* out_count) {
    if (where == NULL || table->index_count == 0) return 0;

    WhereNode* terms[MAX_INDEX_CONJUNCTS];
    int term_count = 0;
    collect_and_terms(where, terms, &term_count);

    for (int t = 0; t < term_count; t++) {
        WhereNode* node = terms[t];
        for (int i = 0; i < table->index_count; i++) {
            TableIndex* idx = &table->indexes[i];
            if (!conjunct_matches_index(node, table, idx)) continue;
            if (idx->root_page <= 0) continue;

            BTree* tree = btree_open(ctx->pager, idx->root_page);
            if (tree == NULL) continue;

            IndexScanCtx scan;
            memset(&scan, 0, sizeof(scan));
            scan.ctx = ctx;
            scan.table = table;

            int ok;
            if (node->literal.type == VAL_STRING) {
                ok = index_scan_string(tree, node, &scan);
            } else if (node->literal.type == VAL_BOOL) {
                ok = btree_scan_eq(tree, &node->literal, index_scan_collect, &scan) >= 0;
            } else {
                ok = index_scan_numeric(tree, node, &scan);
            }
            btree_destroy(tree);

            if (!ok || scan.failed) {
                free_rows(scan.rows, scan.count);
                return 0; /* corrupt index: full scan is still correct */
            }
            if (getenv("AUSPEX_INDEX_DEBUG") != NULL || getenv("MYPL_INDEX_DEBUG") != NULL) {
                fprintf(stderr, "index lookup on %s(%s): %d candidates\n",
                        table->name, idx->column_name, scan.count);
            }
            *out_rows = scan.rows;
            *out_count = scan.count;
            return 1;
        }
    }
    return 0;
}

/* A FROM source is usable when it names an existing table or view. Either side
   of a JOIN may be a view, because a view is materialized into rows wherever a
   table would be read. */
static int source_exists(Context* ctx, const char* name) {
    return catalog_find_table(ctx, name) != NULL ||
           catalog_view_query(ctx, name) != NULL;
}

/* A view definition stays valid only while its own sources exist. */
static int view_source_exists(Context* ctx, SelectStmt* stmt) {
    if (!source_exists(ctx, stmt->table_name)) return 0;
    if (stmt->has_join && !source_exists(ctx, stmt->join_table_name)) return 0;
    return 1;
}

static Result* execute_select(Context* ctx, SelectStmt* stmt);

/* Materializes a view by executing its stored SELECT and taking the rows it
   produces. Views over views work because the recursive call resolves the
   inner one the same way, and a view whose own FROM is a join resolves through
   the join path below. The caller owns the returned rows.

   Returns 0 only for a genuine failure; callers check catalog_view_query first
   when a missing view should mean an empty result rather than an error. */
static int materialize_view(Context* ctx, const char* name,
                            Row** out_rows, int* out_count) {
    *out_rows = NULL;
    *out_count = 0;

    const char* view_query = catalog_view_query(ctx, name);
    if (view_query == NULL) return 0;

    /* The view body is stored text, not part of the calling statement:
       it must not pick up that statement's bind values (issue #54). */
    const Value* saved_binds = g_sql_binds;
    int saved_bind_count = g_sql_bind_count;
    g_sql_binds = NULL;
    g_sql_bind_count = 0;
    SelectStmt view_stmt;
    int view_parsed = sql_parse_select(view_query, &view_stmt);
    g_sql_binds = saved_binds;
    g_sql_bind_count = saved_bind_count;
    if (!view_parsed) {
        sql_free_select_stmt(&view_stmt);
        return 0;
    }
    if (!view_source_exists(ctx, &view_stmt)) {
        /* e.g. the base table was dropped after CREATE VIEW */
        sql_free_select_stmt(&view_stmt);
        return 0;
    }
    Result* inner = execute_select(ctx, &view_stmt);
    sql_free_select_stmt(&view_stmt);
    if (inner == NULL) return 0;

    /* Steal the inner row array rather than copying it. */
    *out_rows = inner->rows;
    *out_count = inner->row_count;
    inner->rows = NULL;
    inner->row_count = 0;
    free(inner);
    return 1;
}

/* One side of a join, already materialized into rows. The column count is kept
   alongside them because resolve_field splits a combined row at that boundary
   to decide which side a prefixed name refers to. */
typedef struct {
    Row* rows;
    int  count;
    int  column_count;
} JoinSource;

static void join_source_free(JoinSource* src) {
    free_rows(src->rows, src->count);
    src->rows = NULL;
    src->count = 0;
    src->column_count = 0;
}

/* Reads a named join side into rows. Returns 1 on success. */
static int materialize_source(Context* ctx, const char* name, JoinSource* out) {
    out->rows = NULL;
    out->count = 0;
    out->column_count = 0;

    Table* table = catalog_find_table(ctx, name);
    if (table != NULL) {
        if (!read_all_rows(ctx, table, &out->rows, &out->count)) return 0;
        out->column_count = table->column_count;
        return 1;
    }

    if (!materialize_view(ctx, name, &out->rows, &out->count)) return 0;
    /* A view has no catalog column list, so its width is whatever its rows
       carry. An empty view leaves it at zero - see join_source_zero_row. */
    out->column_count = out->count > 0 ? out->rows[0].field_count : 0;
    return 1;
}

/* Filler for a left row that found no match. Zero and empty values rather than
   SQL NULLs, which is what this path has always produced. */
static int join_source_zero_row(Context* ctx, const char* name,
                                JoinSource* src, Row* out) {
    out->fields = NULL;
    out->field_count = 0;

    Table* table = catalog_find_table(ctx, name);
    if (table != NULL) {
        *out = row_zero(table);
        return out->fields != NULL;
    }

    /* A view's shape is only visible in the rows it produced. An empty view
       therefore gets an empty filler: a LEFT JOIN against it still keeps every
       left row, and references to the view's own columns resolve to 0 the way
       result_append handles any name it cannot find. */
    if (src->count == 0) return 1;

    Row* shape = &src->rows[0];
    out->fields = calloc((size_t)shape->field_count, sizeof(Field));
    if (out->fields == NULL) return 0;
    out->field_count = shape->field_count;
    for (int i = 0; i < out->field_count; i++) {
        out->fields[i].name = strdup(shape->fields[i].name);
        out->fields[i].value.type = shape->fields[i].value.type;
        if (shape->fields[i].value.type == VAL_STRING) {
            out->fields[i].value.as.as_string = strdup("");
        } else if (shape->fields[i].value.type == VAL_FLOAT) {
            out->fields[i].value.as.as_float = 0.0;
        } else {
            out->fields[i].value.as.as_int = 0;
        }
    }
    return 1;
}

static Result* execute_select(Context* ctx, SelectStmt* stmt) {
    Row* source_rows = NULL;
    int source_count = 0;

    if (stmt->has_join) {
        JoinSource left_src;
        JoinSource right_src;
        if (!materialize_source(ctx, stmt->table_name, &left_src)) {
            return result_create(0);
        }
        if (!materialize_source(ctx, stmt->join_table_name, &right_src)) {
            join_source_free(&left_src);
            return result_create(0);
        }
        stmt->join_left_column_count = left_src.column_count;
        stmt->join_right_column_count = right_src.column_count;

        int max_combined = left_src.count * (right_src.count + 1);
        source_rows = calloc((size_t)max_combined, sizeof(Row));
        if (source_rows == NULL) {
            join_source_free(&left_src);
            join_source_free(&right_src);
            return NULL;
        }

        Row zero_right;
        if (!join_source_zero_row(ctx, stmt->join_table_name, &right_src, &zero_right)) {
            free(source_rows);
            join_source_free(&left_src);
            join_source_free(&right_src);
            return NULL;
        }

        source_count = 0;
        for (int i = 0; i < left_src.count; i++) {
            int matched = 0;
            for (int j = 0; j < right_src.count; j++) {
                if (evaluate_join(&left_src.rows[i], &right_src.rows[j], stmt)) {
                    source_rows[source_count] =
                        row_combine(&left_src.rows[i], &right_src.rows[j]);
                    if (source_rows[source_count].fields == NULL) {
                        free_row(&zero_right);
                        free_rows(source_rows, source_count);
                        join_source_free(&left_src);
                        join_source_free(&right_src);
                        return NULL;
                    }
                    source_count++;
                    matched = 1;
                }
            }
            if (!matched && stmt->join_type == 1) {
                source_rows[source_count] = row_combine(&left_src.rows[i], &zero_right);
                if (source_rows[source_count].fields == NULL) {
                    free_row(&zero_right);
                    free_rows(source_rows, source_count);
                    join_source_free(&left_src);
                    join_source_free(&right_src);
                    return NULL;
                }
                source_count++;
            }
        }

        free_row(&zero_right);
        join_source_free(&left_src);
        join_source_free(&right_src);
    } else {
        Table* table = catalog_find_table(ctx, stmt->table_name);
        if (table != NULL) {
            if (!try_index_lookup(ctx, table, stmt->where, &source_rows, &source_count) &&
                !read_all_rows(ctx, table, &source_rows, &source_count)) {
                return result_create(0);
            }
        } else {
            /* View resolution: the view's rows become a read-only source set,
               and the outer WHERE/ORDER BY/LIMIT then run over them, composing
               with the view's own clauses. */
            if (catalog_view_query(ctx, stmt->table_name) == NULL) {
                return result_create(0);
            }
            if (!materialize_view(ctx, stmt->table_name, &source_rows, &source_count)) {
                return NULL;
            }
        }
    }

    Row** filtered = malloc(sizeof(Row*) * (size_t)source_count);
    if (filtered == NULL) {
        free_rows(source_rows, source_count);
        return NULL;
    }
    int filtered_count = 0;
    for (int i = 0; i < source_count; i++) {
        if (stmt->where != NULL && !evaluate_where(&source_rows[i], stmt)) {
            continue;
        }
        filtered[filtered_count++] = &source_rows[i];
    }

    int has_agg = stmt_has_aggregates(stmt);

    if (stmt->has_order_by && !has_agg) {
        row_ptr_sort(filtered, filtered_count, filtered_row_compare, stmt);
    }

    Result* res = result_create(has_agg ? (stmt->has_group_by ? filtered_count : 1) : filtered_count);
    if (res == NULL) {
        free(filtered);
        free_rows(source_rows, source_count);
        return NULL;
    }

    if (has_agg) {
        if (stmt->has_group_by) {
            row_ptr_sort(filtered, filtered_count, group_row_compare, stmt);
            int i = 0;
            while (i < filtered_count) {
                int j = i + 1;
                Cell* key_i = resolve_field(filtered[i], stmt->group_by_table_prefix,
                                            stmt->group_by_column, stmt);
                Cell z = zero_cell();
                while (j < filtered_count) {
                    Cell* key_j = resolve_field(filtered[j], stmt->group_by_table_prefix,
                                                stmt->group_by_column, stmt);
                    if (cell_compare(key_i != NULL ? key_i : &z,
                                     key_j != NULL ? key_j : &z) != 0) {
                        break;
                    }
                    j++;
                }
                result_append_aggregate(res, stmt, &filtered[i], j - i);
                i = j;
            }
        } else {
            result_append_aggregate(res, stmt, filtered, filtered_count);
        }
    } else {
        for (int i = 0; i < filtered_count; i++) {
            result_append(res, filtered[i], stmt);
        }
    }

    free(filtered);
    free_rows(source_rows, source_count);

    if (stmt->has_limit && !has_agg) {
        result_limit(res, stmt->limit_count);
    }

    return res;
}

/* -------------------------------------------------------------------------- */
/* CREATE TABLE / INSERT parsing and execution                                */
/* -------------------------------------------------------------------------- */

typedef struct {
    char        table_name[MAX_NAME_LEN + 1];
    char*       column_names[MAX_COLUMNS];
    int         column_types[MAX_COLUMNS];
    int         column_flags[MAX_COLUMNS];
    Cell        column_defaults[MAX_COLUMNS];
    int         column_count;
    Cell        values[MAX_COLUMNS];
    int         value_count;
    int         has_select;
    char*       select_query;
    int         if_not_exists;
} DdlStmt;

/* Error detail for DDL/constraint failures, consumed by custom_exec so the
   runtime error message can name the violated constraint. */
static char g_sql_ddl_error[256];

/* Parses a literal (number, string, or NULL) into a Cell. */
static int sql_parse_literal_cell(SqlToken* tok, Cell* out) {
    if (tok->type == TOK_NUMBER) {
        char buf[64];
        sql_token_text(tok, buf, sizeof(buf));
        if (strchr(buf, '.') != NULL) {
            out->type = VAL_FLOAT;
            out->as.as_float = strtod(buf, NULL);
        } else {
            out->type = VAL_INT;
            out->as.as_int = atoi(buf);
        }
        return 1;
    }
    if (tok->type == TOK_NULL) {
        out->type = VAL_NULL;
        out->as.as_int = 0;
        return 1;
    }
    if (tok->type == TOK_TRUE || tok->type == TOK_FALSE) {
        out->type = VAL_BOOL;
        out->as.as_int = tok->type == TOK_TRUE ? 1 : 0;
        return 1;
    }
    if (tok->type == TOK_PARAM) {
        /* ?N placeholder bound by sql_exec_params/sql_exec_ddl_params (#54):
           one parser for every literal position, so binds work in WHERE, in
           INSERT VALUES and in UPDATE SET alike. */
        return sql_param_cell(tok, out);
    }
    if (tok->type == TOK_STRING) {
        out->type = VAL_STRING;
        out->as.as_string = malloc((size_t)tok->length + 1);
        if (out->as.as_string == NULL) return 0;
        memcpy(out->as.as_string, tok->text, (size_t)tok->length);
        out->as.as_string[tok->length] = '\0';
        return 1;
    }
    return 0;
}

/* Parses zero or more column constraint clauses after a column type:
   NOT NULL, PRIMARY KEY, UNIQUE, DEFAULT <literal>. The first token that
   does not start a clause is pushed back. */
static int sql_parse_column_constraints(SqlLexer* lex, int* out_flags, Cell* out_default) {
    int flags = 0;
    out_default->type = VAL_NULL;
    out_default->as.as_int = 0;
    while (1) {
        SqlToken tok = sql_next_token(lex);
        if (tok.type == TOK_NOT) {
            tok = sql_next_token(lex);
            if (tok.type != TOK_NULL) return 0;
            flags |= COL_FLAG_NOT_NULL;
        } else if (tok.type == TOK_PRIMARY) {
            tok = sql_next_token(lex);
            if (tok.type != TOK_KEY) return 0;
            flags |= COL_FLAG_PRIMARY_KEY;
        } else if (tok.type == TOK_UNIQUE) {
            flags |= COL_FLAG_UNIQUE;
        } else if (tok.type == TOK_DEFAULT) {
            tok = sql_next_token(lex);
            if (!sql_parse_literal_cell(&tok, out_default)) return 0;
            flags |= COL_FLAG_HAS_DEFAULT;
        } else {
            sql_lexer_pushback(lex, tok);
            break;
        }
    }
    *out_flags = flags;
    return 1;
}

static int sql_parse_type(SqlToken* tok, int* out_type) {
    if (tok->type == TOK_INT) {
        *out_type = VAL_INT;
        return 1;
    }
    if (tok->type == TOK_FLOAT) {
        *out_type = VAL_FLOAT;
        return 1;
    }
    if (tok->type == TOK_STRING_KW) {
        *out_type = VAL_STRING;
        return 1;
    }
    if (tok->type == TOK_BOOL_KW) {
        *out_type = VAL_BOOL;
        return 1;
    }
    if (tok->type == TOK_DATE_KW) {
        *out_type = VAL_DATE;
        return 1;
    }
    if (tok->type == TOK_TIMESTAMP_KW) {
        *out_type = VAL_TIMESTAMP;
        return 1;
    }
    return 0;
}

static int sql_parse_create_table(const char* query, DdlStmt* stmt) {
    memset(stmt, 0, sizeof(*stmt));

    SqlLexer lex;
    sql_lexer_init(&lex, query);

    SqlToken tok = sql_next_token(&lex);
    if (tok.type != TOK_CREATE) return 0;
    tok = sql_next_token(&lex);
    if (tok.type != TOK_TABLE) return 0;
    tok = sql_next_token(&lex);
    if (tok.type == TOK_IF) {
        tok = sql_next_token(&lex);
        if (tok.type != TOK_NOT) return 0;
        tok = sql_next_token(&lex);
        if (tok.type != TOK_EXISTS) return 0;
        stmt->if_not_exists = 1;
        tok = sql_next_token(&lex);
    }
    if (tok.type != TOK_IDENT) return 0;
    sql_token_text(&tok, stmt->table_name, sizeof(stmt->table_name));

    tok = sql_next_token(&lex);
    if (tok.type != TOK_LPAREN) return 0;

    int pk_count = 0;
    while (1) {
        if (stmt->column_count >= MAX_COLUMNS) return 0;
        tok = sql_next_token(&lex);
        if (tok.type != TOK_IDENT) return 0;
        char buf[MAX_NAME_LEN + 1];
        sql_token_text(&tok, buf, sizeof(buf));
        stmt->column_names[stmt->column_count] = strdup(buf);
        if (stmt->column_names[stmt->column_count] == NULL) return 0;
        tok = sql_next_token(&lex);
        if (!sql_parse_type(&tok, &stmt->column_types[stmt->column_count])) return 0;
        stmt->column_defaults[stmt->column_count].type = VAL_NULL;
        stmt->column_defaults[stmt->column_count].as.as_int = 0;
        if (!sql_parse_column_constraints(&lex, &stmt->column_flags[stmt->column_count],
                                          &stmt->column_defaults[stmt->column_count])) {
            return 0;
        }
        if (stmt->column_flags[stmt->column_count] & COL_FLAG_PRIMARY_KEY) {
            pk_count++;
            if (pk_count > 1) {
                snprintf(g_sql_ddl_error, sizeof(g_sql_ddl_error),
                         "only one PRIMARY KEY column allowed per table");
                return 0;
            }
        }
        if ((stmt->column_flags[stmt->column_count] & COL_FLAG_HAS_DEFAULT) &&
            !cell_coerce_to_column(&stmt->column_defaults[stmt->column_count],
                                   stmt->column_types[stmt->column_count])) {
            snprintf(g_sql_ddl_error, sizeof(g_sql_ddl_error),
                     "DEFAULT value is not valid text for column '%.200s'", buf);
            return 0;
        }
        if ((stmt->column_flags[stmt->column_count] & COL_FLAG_HAS_DEFAULT) &&
            stmt->column_defaults[stmt->column_count].type != VAL_NULL &&
            stmt->column_defaults[stmt->column_count].type != stmt->column_types[stmt->column_count]) {
            snprintf(g_sql_ddl_error, sizeof(g_sql_ddl_error),
                     "DEFAULT value type does not match type of column '%.200s'", buf);
            return 0;
        }
        stmt->column_count++;

        tok = sql_next_token(&lex);
        if (tok.type == TOK_RPAREN) break;
        if (tok.type != TOK_COMMA) return 0;
    }

    tok = sql_next_token(&lex);
    return tok.type == TOK_EOF;
}

static int sql_parse_insert(const char* query, DdlStmt* stmt) {
    memset(stmt, 0, sizeof(*stmt));

    SqlLexer lex;
    sql_lexer_init(&lex, query);

    SqlToken tok = sql_next_token(&lex);
    if (tok.type != TOK_INSERT) return 0;
    tok = sql_next_token(&lex);
    if (tok.type != TOK_INTO) return 0;
    tok = sql_next_token(&lex);
    if (tok.type != TOK_IDENT) return 0;
    sql_token_text(&tok, stmt->table_name, sizeof(stmt->table_name));

    tok = sql_next_token(&lex);
    if (tok.type == TOK_SELECT) {
        stmt->has_select = 1;
        stmt->select_query = strdup(tok.text);
        return stmt->select_query != NULL;
    }
    if (tok.type != TOK_VALUES) return 0;
    tok = sql_next_token(&lex);
    if (tok.type != TOK_LPAREN) return 0;

    tok = sql_next_token(&lex);
    while (tok.type != TOK_RPAREN && tok.type != TOK_EOF) {
        if (stmt->value_count >= MAX_COLUMNS) return 0;
        if (!sql_parse_literal_cell(&tok, &stmt->values[stmt->value_count])) {
            return 0;
        }
        stmt->value_count++;

        tok = sql_next_token(&lex);
        if (tok.type == TOK_COMMA) {
            tok = sql_next_token(&lex);
        }
    }

    if (tok.type != TOK_RPAREN) return 0;
    tok = sql_next_token(&lex);
    return tok.type == TOK_EOF;
}

static void sql_free_ddl_stmt(DdlStmt* stmt) {
    for (int i = 0; i < stmt->column_count; i++) {
        free(stmt->column_names[i]);
        stmt->column_names[i] = NULL;
        if ((stmt->column_flags[i] & COL_FLAG_HAS_DEFAULT) &&
            cell_owns_text(stmt->column_defaults[i].type)) {
            free(stmt->column_defaults[i].as.as_string);
            stmt->column_defaults[i].type = VAL_NULL;
        }
    }
    for (int i = 0; i < stmt->value_count; i++) {
        if (cell_owns_text(stmt->values[i].type)) {
            free(stmt->values[i].as.as_string);
        }
    }
    if (stmt->select_query != NULL) {
        free(stmt->select_query);
        stmt->select_query = NULL;
    }
}

typedef struct {
    char table_name[MAX_NAME_LEN + 1];
    char set_column[MAX_NAME_LEN + 1];
    Cell set_value;
    WhereNode* where;
} UpdateStmt;

typedef struct {
    char table_name[MAX_NAME_LEN + 1];
    WhereNode* where;
} DeleteStmt;

static int sql_parse_update(const char* query, UpdateStmt* stmt);
static int sql_parse_delete(const char* query, DeleteStmt* stmt);
static void sql_free_update_stmt(UpdateStmt* stmt);
static void sql_free_delete_stmt(DeleteStmt* stmt);
static int execute_update(Context* ctx, UpdateStmt* stmt);
static int execute_delete(Context* ctx, DeleteStmt* stmt);
static int sql_parse_drop_table(const char* query, char* table_name, size_t table_name_size,
                                int* if_exists);
static int sql_parse_create_view(const char* query, char* view_name, size_t view_name_size,
                                 const char** select_text, int* select_len);
static int sql_parse_drop_view(const char* query, char* view_name, size_t view_name_size,
                               int* if_exists);
static int execute_create_view(Context* ctx, const char* view_name,
                               const char* select_text, int select_len);
static int execute_drop_view(Context* ctx, const char* view_name, int if_exists);
static int sql_parse_alter_table(const char* query, char* table_name, size_t table_name_size,
                                 int* action, char* column_name, size_t column_name_size,
                                 int* column_type, int* column_flags, Cell* column_default);
static int execute_drop_table(Context* ctx, const char* table_name, int if_exists);
static int execute_alter_add_column(Context* ctx, const char* table_name,
                                    const char* column_name, int column_type,
                                    int column_flags, const Cell* column_default);
static int execute_alter_drop_column(Context* ctx, const char* table_name,
                                     const char* column_name);
static int sql_parse_create_index(const char* query, char* index_name, size_t index_name_size,
                                  char* table_name, size_t table_name_size,
                                  char* column_name, size_t column_name_size);
static int sql_parse_drop_index(const char* query, char* index_name, size_t index_name_size,
                                char* table_name, size_t table_name_size);
static int execute_create_index(Context* ctx, const char* index_name,
                                const char* table_name, const char* column_name);
static int execute_drop_index(Context* ctx, const char* index_name, const char* table_name);
static void table_drop_index_at(Context* ctx, Table* table, int i);
static void indexes_reset(Context* ctx, Table* table);

/* -------------------------------------------------------------------------- */
/* Column constraint enforcement                                              */
/* -------------------------------------------------------------------------- */

static const char* constraint_label(int flags) {
    if (flags & COL_FLAG_PRIMARY_KEY) return "PRIMARY KEY";
    if (flags & COL_FLAG_UNIQUE) return "UNIQUE";
    return "NOT NULL";
}

/* Applies DEFAULT values to NULL cells, then enforces NOT NULL / PRIMARY KEY.
   Returns 1 when the row is valid; on failure sets g_sql_ddl_error. */
static int constraints_apply_row(Table* table, Cell* cells) {
    for (int c = 0; c < table->column_count; c++) {
        if (cells[c].type != VAL_NULL) continue;
        if (table->columns[c].flags & COL_FLAG_HAS_DEFAULT) {
            cells[c] = cell_dup(&table->columns[c].default_value);
        }
    }
    for (int c = 0; c < table->column_count; c++) {
        int flags = table->columns[c].flags;
        if (cells[c].type == VAL_NULL && (flags & (COL_FLAG_NOT_NULL | COL_FLAG_PRIMARY_KEY))) {
            snprintf(g_sql_ddl_error, sizeof(g_sql_ddl_error),
                     "%s constraint violated: %s.%s",
                     constraint_label(flags), table->name, table->columns[c].name);
            return 0;
        }
    }
    return 1;
}

/* Checks UNIQUE / PRIMARY KEY for a new row against the rows already stored.
   NULL values never conflict (SQL-standard UNIQUE semantics). */
static int constraints_check_new_row(Context* ctx, Table* table, Cell* cells) {
    int needs_scan = 0;
    for (int c = 0; c < table->column_count; c++) {
        if ((table->columns[c].flags & (COL_FLAG_UNIQUE | COL_FLAG_PRIMARY_KEY)) &&
            cells[c].type != VAL_NULL) {
            needs_scan = 1;
            break;
        }
    }
    if (!needs_scan) return 1;

    Row* rows = NULL;
    int row_count = 0;
    if (!read_all_rows(ctx, table, &rows, &row_count)) return 0;
    for (int r = 0; r < row_count; r++) {
        for (int c = 0; c < table->column_count; c++) {
            int flags = table->columns[c].flags;
            if (!(flags & (COL_FLAG_UNIQUE | COL_FLAG_PRIMARY_KEY))) continue;
            if (cells[c].type == VAL_NULL) continue;
            Cell* existing = &rows[r].fields[c].value;
            if (existing->type == VAL_NULL) continue;
            if (cell_compare(existing, &cells[c]) == 0) {
                snprintf(g_sql_ddl_error, sizeof(g_sql_ddl_error),
                         "%s constraint violated: %s.%s",
                         constraint_label(flags), table->name, table->columns[c].name);
                free_rows(rows, row_count);
                return 0;
            }
        }
    }
    free_rows(rows, row_count);
    return 1;
}

/* Validates a full in-memory row set (used by UPDATE after mutation, before
   the row chain is rewritten). */
static int constraints_check_row_set(Table* table, Row* rows, int row_count) {
    for (int r = 0; r < row_count; r++) {
        for (int c = 0; c < table->column_count; c++) {
            int flags = table->columns[c].flags;
            if (rows[r].fields[c].value.type == VAL_NULL &&
                (flags & (COL_FLAG_NOT_NULL | COL_FLAG_PRIMARY_KEY))) {
                snprintf(g_sql_ddl_error, sizeof(g_sql_ddl_error),
                         "%s constraint violated: %s.%s",
                         constraint_label(flags), table->name, table->columns[c].name);
                return 0;
            }
        }
    }
    for (int c = 0; c < table->column_count; c++) {
        int flags = table->columns[c].flags;
        if (!(flags & (COL_FLAG_UNIQUE | COL_FLAG_PRIMARY_KEY))) continue;
        for (int i = 0; i < row_count; i++) {
            Cell* a = &rows[i].fields[c].value;
            if (a->type == VAL_NULL) continue;
            for (int j = i + 1; j < row_count; j++) {
                Cell* b = &rows[j].fields[c].value;
                if (b->type == VAL_NULL) continue;
                if (cell_compare(a, b) == 0) {
                    snprintf(g_sql_ddl_error, sizeof(g_sql_ddl_error),
                             "%s constraint violated: %s.%s",
                             constraint_label(flags), table->name, table->columns[c].name);
                    return 0;
                }
            }
        }
    }
    return 1;
}

/* -------------------------------------------------------------------------- */
/* Row-level trigger hook (FOR EACH ROW triggers)                              */
/*                                                                            */
/* The VM installs the hook on the active DBDriver; custom_exec copies it    */
/* into these globals before each statement. A NULL hook means no row-level  */
/* triggers are active and DML runs exactly as before. The hook receives the  */
/* old and new row images as VAL_ROW Values (NULL where the context does not  */
/* apply) and returns 0 to abort the statement.                              */
/* -------------------------------------------------------------------------- */

static RowTriggerFn g_row_trigger_fn = NULL;
static void*        g_row_trigger_user = NULL;

static Value trigger_cell_value(const Cell* cell) {
    switch (cell->type) {
        case VAL_INT:    return value_int(cell->as.as_int);
        case VAL_FLOAT:  return value_float(cell->as.as_float);
        case VAL_STRING: return value_string(strdup(cell->as.as_string != NULL
                                                    ? cell->as.as_string : ""));
        case VAL_NULL:   return value_null();
        default:         return value_int(0);
    }
}

/* Builds a VAL_ROW Value (refcount 1, caller releases) from positional cells. */
static Value trigger_row_value_from_cells(Table* table, Cell* cells) {
    RowObj* row = row_obj_new(table->column_count);
    if (row == NULL) return value_null();
    for (int c = 0; c < table->column_count; c++) {
        Value v = trigger_cell_value(&cells[c]);
        row_obj_set_column(row, c, table->columns[c].name, v);
        value_release(v);
    }
    return value_row(row);
}

/* Builds a VAL_ROW Value (refcount 1, caller releases) from a named Row. */
static Value trigger_row_value_from_row(Row* src) {
    RowObj* row = row_obj_new(src->field_count);
    if (row == NULL) return value_null();
    for (int c = 0; c < src->field_count; c++) {
        Value v = trigger_cell_value(&src->fields[c].value);
        row_obj_set_column(row, c, src->fields[c].name, v);
        value_release(v);
    }
    return value_row(row);
}

/* Fire the row-trigger hook for one row image pair. Returns 1 on success; on
   failure copies the hook's message into g_sql_ddl_error and returns 0. */
static int engine_fire_row_triggers(int timing, int event, const char* table,
                                    const Value* old_row, const Value* new_row) {
    if (g_row_trigger_fn == NULL) return 1;
    char error[256];
    error[0] = '\0';
    if (g_row_trigger_fn(g_row_trigger_user, timing, event, table, old_row, new_row,
                         error, sizeof(error))) {
        return 1;
    }
    snprintf(g_sql_ddl_error, sizeof(g_sql_ddl_error), "%s",
             error[0] != '\0' ? error : "row trigger failed");
    return 0;
}

static Row trigger_row_dup(Row* src) {
    Row copy;
    copy.field_count = src->field_count;
    copy.fields = calloc((size_t)copy.field_count, sizeof(Field));
    if (copy.fields == NULL) {
        copy.field_count = 0;
        return copy;
    }
    for (int i = 0; i < copy.field_count; i++) {
        copy.fields[i].name = strdup(src->fields[i].name);
        copy.fields[i].value = cell_dup(&src->fields[i].value);
    }
    return copy;
}

static void trigger_row_free(Row* row) {
    if (row->fields == NULL) return;
    for (int i = 0; i < row->field_count; i++) {
        free(row->fields[i].name);
        if (cell_owns_text(row->fields[i].value.type)) {
            free(row->fields[i].value.as.as_string);
        }
    }
    free(row->fields);
    row->fields = NULL;
}

/* Retags string literals written into date or timestamp columns, so a row
   reaches storage with the type its column declares. */
static int cells_coerce_to_columns(Table* table, Cell* cells) {
    for (int i = 0; i < table->column_count; i++) {
        if (cell_coerce_to_column(&cells[i], table->columns[i].type)) continue;
        snprintf(g_sql_ddl_error, sizeof(g_sql_ddl_error),
                 "column '%.100s' expects %s", table->columns[i].name,
                 table->columns[i].type == VAL_DATE
                     ? "a date written as 'YYYY-MM-DD'"
                     : "a timestamp written as 'YYYY-MM-DD HH:MM:SS'");
        return 0;
    }
    return 1;
}

static int execute_insert_select(Context* ctx, Table* table, const char* select_query) {
    Result* res = sql_exec(select_query, ctx);
    if (res == NULL) return 0;

    for (int r = 0; r < res->row_count; r++) {
        Row* src = &res->rows[r];
        Cell cells[MAX_COLUMNS];
        for (int c = 0; c < table->column_count && c < MAX_COLUMNS; c++) {
            Cell* value = row_find_field(src, table->columns[c].name);
            if (value != NULL) {
                cells[c] = cell_dup(value);
            } else {
                cells[c].type = table->columns[c].type;
                if (cell_owns_text(cells[c].type)) {
                    cells[c].as.as_string = strdup("");
                } else if (cells[c].type == VAL_FLOAT) {
                    cells[c].as.as_float = 0.0;
                } else {
                    cells[c].as.as_int = 0;
                }
            }
        }
        int ok = constraints_apply_row(table, cells);
        if (ok) {
            Value new_row = trigger_row_value_from_cells(table, cells);
            ok = engine_fire_row_triggers(TRIGGER_BEFORE, TRIGGER_INSERT,
                                          table->name, NULL, &new_row);
            value_release(new_row);
        }
        if (ok) {
            ok = constraints_check_new_row(ctx, table, cells);
        }
        if (ok) {
            catalog_insert(ctx, table, cells);
            Value new_row = trigger_row_value_from_cells(table, cells);
            ok = engine_fire_row_triggers(TRIGGER_AFTER, TRIGGER_INSERT,
                                          table->name, NULL, &new_row);
            value_release(new_row);
        }
        for (int c = 0; c < table->column_count && c < MAX_COLUMNS; c++) {
            if (cell_owns_text(cells[c].type)) {
                free(cells[c].as.as_string);
            }
        }
        if (!ok) {
            result_free(res);
            return 0;
        }
    }

    result_free(res);
    catalog_write_page(ctx);
    return 1;
}

int sql_exec_ddl(const char* query, Context* ctx) {
    g_sql_ddl_error[0] = '\0';
    DdlStmt stmt;
    if (sql_parse_create_table(query, &stmt)) {
        if (stmt.if_not_exists && catalog_find_table(ctx, stmt.table_name) != NULL) {
            sql_free_ddl_stmt(&stmt);
            return 1;
        }
        Table* t = catalog_create_table(ctx, stmt.table_name,
                                        (const char**)stmt.column_names,
                                        stmt.column_types,
                                        stmt.column_count);
        if (t != NULL) {
            for (int i = 0; i < stmt.column_count; i++) {
                t->columns[i].flags = stmt.column_flags[i];
                t->columns[i].default_value = cell_dup(&stmt.column_defaults[i]);
            }
            catalog_write_page(ctx);
        }
        sql_free_ddl_stmt(&stmt);
        return t != NULL ? 1 : 0;
    }

    {
        char table_name[MAX_NAME_LEN + 1];
        int if_exists = 0;
        if (sql_parse_drop_table(query, table_name, sizeof(table_name), &if_exists)) {
            return execute_drop_table(ctx, table_name, if_exists);
        }
    }

    {
        char table_name[MAX_NAME_LEN + 1];
        char column_name[MAX_NAME_LEN + 1];
        int action = 0;
        int column_type = 0;
        int column_flags = 0;
        Cell column_default;
        column_default.type = VAL_NULL;
        column_default.as.as_int = 0;
        if (sql_parse_alter_table(query, table_name, sizeof(table_name),
                                  &action, column_name, sizeof(column_name), &column_type,
                                  &column_flags, &column_default)) {
            int ok;
            if (action == 1) {
                ok = execute_alter_add_column(ctx, table_name, column_name, column_type,
                                              column_flags, &column_default);
            } else {
                ok = execute_alter_drop_column(ctx, table_name, column_name);
            }
            if (cell_owns_text(column_default.type)) {
                free(column_default.as.as_string);
            }
            return ok;
        }
    }

    {
        char index_name[MAX_NAME_LEN + 1];
        char table_name[MAX_NAME_LEN + 1];
        char column_name[MAX_NAME_LEN + 1];
        if (sql_parse_create_index(query, index_name, sizeof(index_name),
                                   table_name, sizeof(table_name),
                                   column_name, sizeof(column_name))) {
            return execute_create_index(ctx, index_name, table_name, column_name);
        }
        table_name[0] = '\0';
        if (sql_parse_drop_index(query, index_name, sizeof(index_name),
                                 table_name, sizeof(table_name))) {
            return execute_drop_index(ctx, index_name,
                                      table_name[0] != '\0' ? table_name : NULL);
        }
    }

    {
        char view_name[MAX_NAME_LEN + 1];
        const char* select_text = NULL;
        int select_len = 0;
        if (sql_parse_create_view(query, view_name, sizeof(view_name),
                                  &select_text, &select_len)) {
            /* A view body is stored and re-parsed on every use, long after
               the values bound to this statement are gone. */
            char quote = '\0';
            for (int i = 0; i < select_len; i++) {
                char c = select_text[i];
                if (quote != '\0') {
                    if (c == quote) quote = '\0';
                } else if (c == '\'' || c == '"') {
                    quote = c;
                } else if (c == '?') {
                    snprintf(g_sql_ddl_error, sizeof(g_sql_ddl_error),
                             "bind parameters are not allowed in a view");
                    return 0;
                }
            }
            return execute_create_view(ctx, view_name, select_text, select_len);
        }
        int if_exists = 0;
        if (sql_parse_drop_view(query, view_name, sizeof(view_name), &if_exists)) {
            return execute_drop_view(ctx, view_name, if_exists);
        }
    }

    if (sql_parse_insert(query, &stmt)) {
        Table* t = catalog_find_table(ctx, stmt.table_name);
        if (t == NULL) {
            if (catalog_view_query(ctx, stmt.table_name) != NULL) {
                snprintf(g_sql_ddl_error, sizeof(g_sql_ddl_error),
                         "cannot insert into view '%.200s' (views are read-only)",
                         stmt.table_name);
            }
            sql_free_ddl_stmt(&stmt);
            return 0;
        }
        if (stmt.has_select) {
            int ok = execute_insert_select(ctx, t, stmt.select_query);
            sql_free_ddl_stmt(&stmt);
            return ok;
        }
        if (t->column_count != stmt.value_count) {
            sql_free_ddl_stmt(&stmt);
            return 0;
        }
        if (!cells_coerce_to_columns(t, stmt.values) ||
            !constraints_apply_row(t, stmt.values)) {
            sql_free_ddl_stmt(&stmt);
            return 0;
        }
        if (g_row_trigger_fn != NULL) {
            Value new_row = trigger_row_value_from_cells(t, stmt.values);
            int fired = engine_fire_row_triggers(TRIGGER_BEFORE, TRIGGER_INSERT,
                                                 t->name, NULL, &new_row);
            value_release(new_row);
            if (!fired) {
                sql_free_ddl_stmt(&stmt);
                return 0;
            }
        }
        if (!constraints_check_new_row(ctx, t, stmt.values)) {
            sql_free_ddl_stmt(&stmt);
            return 0;
        }
        catalog_insert(ctx, t, stmt.values);
        if (g_row_trigger_fn != NULL) {
            Value new_row = trigger_row_value_from_cells(t, stmt.values);
            int fired = engine_fire_row_triggers(TRIGGER_AFTER, TRIGGER_INSERT,
                                                 t->name, NULL, &new_row);
            value_release(new_row);
            if (!fired) {
                sql_free_ddl_stmt(&stmt);
                return 0;
            }
        }
        sql_free_ddl_stmt(&stmt);
        return 1;
    }

    UpdateStmt update_stmt;
    if (sql_parse_update(query, &update_stmt)) {
        int ok = execute_update(ctx, &update_stmt);
        sql_free_update_stmt(&update_stmt);
        return ok;
    }

    DeleteStmt delete_stmt;
    if (sql_parse_delete(query, &delete_stmt)) {
        int ok = execute_delete(ctx, &delete_stmt);
        sql_free_delete_stmt(&delete_stmt);
        return ok;
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
/* UPDATE / DELETE parsing and execution                                      */
/* -------------------------------------------------------------------------- */

static int parse_where_clause(SqlLexer* lex, WhereNode** out) {
    *out = NULL;
    SqlToken tok = sql_next_token(lex);
    if (tok.type == TOK_EOF) {
        return 1;
    }
    if (tok.type != TOK_WHERE) return 0;

    tok = sql_next_token(lex);
    WhereNode* node = where_parse_or(lex, &tok);
    if (node == NULL) return 0;
    if (tok.type != TOK_EOF) {
        where_free(node);
        return 0;
    }
    *out = node;
    return 1;
}

static void sql_free_update_stmt(UpdateStmt* stmt) {
    if (cell_owns_text(stmt->set_value.type) && stmt->set_value.as.as_string != NULL) {
        free(stmt->set_value.as.as_string);
        stmt->set_value.as.as_string = NULL;
    }
    where_free(stmt->where);
    stmt->where = NULL;
}

static void sql_free_delete_stmt(DeleteStmt* stmt) {
    where_free(stmt->where);
    stmt->where = NULL;
}

static int sql_parse_update(const char* query, UpdateStmt* stmt) {
    memset(stmt, 0, sizeof(*stmt));

    SqlLexer lex;
    sql_lexer_init(&lex, query);

    SqlToken tok = sql_next_token(&lex);
    if (tok.type != TOK_UPDATE) return 0;
    tok = sql_next_token(&lex);
    if (tok.type != TOK_IDENT) return 0;
    sql_token_text(&tok, stmt->table_name, sizeof(stmt->table_name));

    tok = sql_next_token(&lex);
    if (tok.type != TOK_SET) return 0;

    tok = sql_next_token(&lex);
    if (tok.type != TOK_IDENT) return 0;
    sql_token_text(&tok, stmt->set_column, sizeof(stmt->set_column));

    tok = sql_next_token(&lex);
    if (tok.type != TOK_EQ) return 0;

    tok = sql_next_token(&lex);
    if (!sql_parse_literal_cell(&tok, &stmt->set_value)) return 0;

    return parse_where_clause(&lex, &stmt->where);
}

static int sql_parse_delete(const char* query, DeleteStmt* stmt) {
    memset(stmt, 0, sizeof(*stmt));

    SqlLexer lex;
    sql_lexer_init(&lex, query);

    SqlToken tok = sql_next_token(&lex);
    if (tok.type != TOK_DELETE) return 0;
    tok = sql_next_token(&lex);
    if (tok.type != TOK_FROM) return 0;
    tok = sql_next_token(&lex);
    if (tok.type != TOK_IDENT) return 0;
    sql_token_text(&tok, stmt->table_name, sizeof(stmt->table_name));

    return parse_where_clause(&lex, &stmt->where);
}

static int row_matches_where(Row* row, WhereNode* where) {
    return where_eval(row, where, NULL) == 1;
}

static void free_row_pages(Context* ctx, Table* table) {
    Pager* pager = ctx->pager;
    int page_num = table->first_row_page;
    while (page_num != 0) {
        uint8_t page[PAGE_SIZE];
        pager_read_page(pager, page_num, page);
        int next_page_num = row_page_next(pager, page);
        pager_free_page(pager, page_num);
        page_num = next_page_num;
    }
    table->first_row_page = 0;
    table->last_row_page = 0;
}

static int execute_update(Context* ctx, UpdateStmt* stmt) {
    Table* table = catalog_find_table(ctx, stmt->table_name);
    if (table == NULL) {
        if (catalog_view_query(ctx, stmt->table_name) != NULL) {
            snprintf(g_sql_ddl_error, sizeof(g_sql_ddl_error),
                     "cannot update view '%.200s' (views are read-only)", stmt->table_name);
        }
        return 0;
    }

    Row* rows = NULL;
    int row_count = 0;
    if (!read_all_rows(ctx, table, &rows, &row_count)) return 0;

    int set_col_index = -1;
    for (int i = 0; i < table->column_count; i++) {
        if (strcmp(table->columns[i].name, stmt->set_column) == 0) {
            set_col_index = i;
            break;
        }
    }
    if (set_col_index >= 0 &&
        !cell_coerce_to_column(&stmt->set_value,
                               table->columns[set_col_index].type)) {
        snprintf(g_sql_ddl_error, sizeof(g_sql_ddl_error),
                 "column '%.100s' expects %s", table->columns[set_col_index].name,
                 table->columns[set_col_index].type == VAL_DATE
                     ? "a date written as 'YYYY-MM-DD'"
                     : "a timestamp written as 'YYYY-MM-DD HH:MM:SS'");
        set_col_index = -1;
    }
    if (set_col_index < 0) {
        for (int i = 0; i < row_count; i++) {
            for (int j = 0; j < rows[i].field_count; j++) {
                free(rows[i].fields[j].name);
                if (cell_owns_text(rows[i].fields[j].value.type)) {
                    free(rows[i].fields[j].value.as.as_string);
                }
            }
            free(rows[i].fields);
        }
        free(rows);
        return 0;
    }

    /* When row-level triggers are active, snapshot the pre-update image of
       every matched row so BEFORE/AFTER row triggers receive :old. */
    int* matched = NULL;
    Row* old_rows = NULL;
    if (g_row_trigger_fn != NULL && row_count > 0) {
        matched = calloc((size_t)row_count, sizeof(int));
        old_rows = calloc((size_t)row_count, sizeof(Row));
        if (matched == NULL || old_rows == NULL) {
            free(matched);
            free(old_rows);
            snprintf(g_sql_ddl_error, sizeof(g_sql_ddl_error), "out of memory");
            free_rows(rows, row_count);
            return 0;
        }
    }

    for (int i = 0; i < row_count; i++) {
        if (stmt->where != NULL && !row_matches_where(&rows[i], stmt->where)) {
            continue;
        }
        if (matched != NULL) {
            matched[i] = 1;
            old_rows[i] = trigger_row_dup(&rows[i]);
        }
        Cell* cell = &rows[i].fields[set_col_index].value;
        if (cell_owns_text(cell->type) && cell->as.as_string != NULL) {
            free(cell->as.as_string);
        }
        cell->type = stmt->set_value.type;
        if (cell_owns_text(cell->type)) {
            cell->as.as_string = stmt->set_value.as.as_string != NULL
                ? strdup(stmt->set_value.as.as_string)
                : strdup("");
        } else if (cell->type == VAL_FLOAT) {
            cell->as.as_float = stmt->set_value.as.as_float;
        } else {
            cell->as.as_int = stmt->set_value.as.as_int;
        }
        if (matched != NULL && matched[i]) {
            Value old_row = trigger_row_value_from_row(&old_rows[i]);
            Value new_row = trigger_row_value_from_row(&rows[i]);
            int fired = engine_fire_row_triggers(TRIGGER_BEFORE, TRIGGER_UPDATE,
                                                 table->name, &old_row, &new_row);
            value_release(old_row);
            value_release(new_row);
            if (!fired) {
                for (int k = 0; k < row_count; k++) {
                    if (matched[k]) trigger_row_free(&old_rows[k]);
                }
                free(matched);
                free(old_rows);
                free_rows(rows, row_count);
                return 0;
            }
        }
    }

    /* Validate the mutated row set before the row chain is rewritten, so a
       rejected UPDATE leaves the stored data untouched. */
    if (!constraints_check_row_set(table, rows, row_count)) {
        if (matched != NULL) {
            for (int k = 0; k < row_count; k++) {
                if (matched[k]) trigger_row_free(&old_rows[k]);
            }
            free(matched);
            free(old_rows);
        }
        free_rows(rows, row_count);
        return 0;
    }

    free_row_pages(ctx, table);
    indexes_reset(ctx, table);
    for (int i = 0; i < row_count; i++) {
        Cell* cells = malloc((size_t)rows[i].field_count * sizeof(Cell));
        if (cells == NULL) {
            if (matched != NULL) {
                for (int k = 0; k < row_count; k++) {
                    if (matched[k]) trigger_row_free(&old_rows[k]);
                }
                free(matched);
                free(old_rows);
            }
            for (int k = i; k < row_count; k++) {
                for (int j = 0; j < rows[k].field_count; j++) {
                    free(rows[k].fields[j].name);
                    if (cell_owns_text(rows[k].fields[j].value.type)) {
                        free(rows[k].fields[j].value.as.as_string);
                    }
                }
                free(rows[k].fields);
            }
            free(rows);
            return 0;
        }
        for (int j = 0; j < rows[i].field_count; j++) {
            cells[j] = rows[i].fields[j].value;
        }
        catalog_insert(ctx, table, cells);
        free(cells);
        if (matched != NULL && matched[i]) {
            Value old_row = trigger_row_value_from_row(&old_rows[i]);
            Value new_row = trigger_row_value_from_row(&rows[i]);
            int fired = engine_fire_row_triggers(TRIGGER_AFTER, TRIGGER_UPDATE,
                                                 table->name, &old_row, &new_row);
            value_release(old_row);
            value_release(new_row);
            trigger_row_free(&old_rows[i]);
            matched[i] = 0;
            if (!fired) {
                for (int k = 0; k < row_count; k++) {
                    if (matched[k]) trigger_row_free(&old_rows[k]);
                }
                free(matched);
                free(old_rows);
                free_rows(rows, row_count);
                return 0;
            }
        }
    }

    free(matched);
    free(old_rows);

    for (int i = 0; i < row_count; i++) {
        for (int j = 0; j < rows[i].field_count; j++) {
            free(rows[i].fields[j].name);
            if (cell_owns_text(rows[i].fields[j].value.type)) {
                free(rows[i].fields[j].value.as.as_string);
            }
        }
        free(rows[i].fields);
    }
    free(rows);

    catalog_write_page(ctx);
    return 1;
}

static int execute_delete(Context* ctx, DeleteStmt* stmt) {
    Table* table = catalog_find_table(ctx, stmt->table_name);
    if (table == NULL) {
        if (catalog_view_query(ctx, stmt->table_name) != NULL) {
            snprintf(g_sql_ddl_error, sizeof(g_sql_ddl_error),
                     "cannot delete from view '%.200s' (views are read-only)", stmt->table_name);
        }
        return 0;
    }

    Row* rows = NULL;
    int row_count = 0;
    if (!read_all_rows(ctx, table, &rows, &row_count)) return 0;

    /* Row-level BEFORE triggers fire per matched row before the row chain
       is rewritten; AFTER triggers fire once the rewrite is complete. */
    int* matched = NULL;
    if (g_row_trigger_fn != NULL && row_count > 0) {
        matched = calloc((size_t)row_count, sizeof(int));
        if (matched == NULL) {
            snprintf(g_sql_ddl_error, sizeof(g_sql_ddl_error), "out of memory");
            free_rows(rows, row_count);
            return 0;
        }
        for (int i = 0; i < row_count; i++) {
            if (stmt->where != NULL && !row_matches_where(&rows[i], stmt->where)) {
                continue;
            }
            matched[i] = 1;
            Value old_row = trigger_row_value_from_row(&rows[i]);
            int fired = engine_fire_row_triggers(TRIGGER_BEFORE, TRIGGER_DELETE,
                                                 table->name, &old_row, NULL);
            value_release(old_row);
            if (!fired) {
                free(matched);
                free_rows(rows, row_count);
                return 0;
            }
        }
    }

    free_row_pages(ctx, table);
    indexes_reset(ctx, table);
    for (int i = 0; i < row_count; i++) {
        if (stmt->where != NULL && !row_matches_where(&rows[i], stmt->where)) {
            Cell* cells = malloc((size_t)rows[i].field_count * sizeof(Cell));
            if (cells == NULL) {
                free(matched);
                for (int k = i; k < row_count; k++) {
                    for (int j = 0; j < rows[k].field_count; j++) {
                        free(rows[k].fields[j].name);
                        if (cell_owns_text(rows[k].fields[j].value.type)) {
                            free(rows[k].fields[j].value.as.as_string);
                        }
                    }
                    free(rows[k].fields);
                }
                free(rows);
                return 0;
            }
            for (int j = 0; j < rows[i].field_count; j++) {
                cells[j] = rows[i].fields[j].value;
            }
            catalog_insert(ctx, table, cells);
            free(cells);
        }
    }

    if (matched != NULL) {
        for (int i = 0; i < row_count; i++) {
            if (!matched[i]) continue;
            Value old_row = trigger_row_value_from_row(&rows[i]);
            int fired = engine_fire_row_triggers(TRIGGER_AFTER, TRIGGER_DELETE,
                                                 table->name, &old_row, NULL);
            value_release(old_row);
            if (!fired) {
                free(matched);
                free_rows(rows, row_count);
                return 0;
            }
        }
        free(matched);
    }

    for (int i = 0; i < row_count; i++) {
        for (int j = 0; j < rows[i].field_count; j++) {
            free(rows[i].fields[j].name);
            if (cell_owns_text(rows[i].fields[j].value.type)) {
                free(rows[i].fields[j].value.as.as_string);
            }
        }
        free(rows[i].fields);
    }
    free(rows);

    catalog_write_page(ctx);
    return 1;
}

/* -------------------------------------------------------------------------- */
/* DROP TABLE / ALTER TABLE parsing and execution                             */
/* -------------------------------------------------------------------------- */

static int sql_parse_drop_table(const char* query, char* table_name, size_t table_name_size,
                                int* if_exists) {
    SqlLexer lex;
    sql_lexer_init(&lex, query);

    SqlToken tok = sql_next_token(&lex);
    if (tok.type != TOK_DROP) return 0;
    tok = sql_next_token(&lex);
    if (tok.type != TOK_TABLE) return 0;

    *if_exists = 0;
    tok = sql_next_token(&lex);
    if (tok.type == TOK_IF) {
        tok = sql_next_token(&lex);
        if (tok.type != TOK_EXISTS) return 0;
        *if_exists = 1;
        tok = sql_next_token(&lex);
    }
    if (tok.type != TOK_IDENT) return 0;
    sql_token_text(&tok, table_name, table_name_size);

    tok = sql_next_token(&lex);
    return tok.type == TOK_EOF;
}

/* action: 1 = ADD COLUMN, 2 = DROP COLUMN */
static int sql_parse_alter_table(const char* query, char* table_name, size_t table_name_size,
                                 int* action, char* column_name, size_t column_name_size,
                                 int* column_type, int* column_flags, Cell* column_default) {
    SqlLexer lex;
    sql_lexer_init(&lex, query);

    *column_flags = 0;
    column_default->type = VAL_NULL;
    column_default->as.as_int = 0;

    SqlToken tok = sql_next_token(&lex);
    if (tok.type != TOK_ALTER) return 0;
    tok = sql_next_token(&lex);
    if (tok.type != TOK_TABLE) return 0;
    tok = sql_next_token(&lex);
    if (tok.type != TOK_IDENT) return 0;
    sql_token_text(&tok, table_name, table_name_size);

    tok = sql_next_token(&lex);
    if (tok.type == TOK_ADD) {
        *action = 1;
    } else if (tok.type == TOK_DROP) {
        *action = 2;
    } else {
        return 0;
    }

    tok = sql_next_token(&lex);
    if (tok.type == TOK_COLUMN) {
        tok = sql_next_token(&lex);
    }
    if (tok.type != TOK_IDENT) return 0;
    sql_token_text(&tok, column_name, column_name_size);

    if (*action == 1) {
        tok = sql_next_token(&lex);
        if (!sql_parse_type(&tok, column_type)) return 0;
        if (!sql_parse_column_constraints(&lex, column_flags, column_default)) return 0;
        if ((*column_flags & COL_FLAG_HAS_DEFAULT) &&
            !cell_coerce_to_column(column_default, *column_type)) {
            snprintf(g_sql_ddl_error, sizeof(g_sql_ddl_error),
                     "DEFAULT value is not valid text for column '%s'", column_name);
            return 0;
        }
        if ((*column_flags & COL_FLAG_HAS_DEFAULT) &&
            column_default->type != VAL_NULL &&
            column_default->type != *column_type) {
            snprintf(g_sql_ddl_error, sizeof(g_sql_ddl_error),
                     "DEFAULT value type does not match type of column '%s'", column_name);
            return 0;
        }
    }

    tok = sql_next_token(&lex);
    return tok.type == TOK_EOF;
}

static int execute_drop_table(Context* ctx, const char* table_name, int if_exists) {
    for (int i = 0; i < g_catalog_count; i++) {
        if (g_catalog[i] != NULL && strcmp(g_catalog[i]->name, table_name) == 0) {
            Table* table = g_catalog[i];
            free_row_pages(ctx, table);
            while (table->index_count > 0) {
                table_drop_index_at(ctx, table, table->index_count - 1);
            }
            free_table(table);
            for (int j = i + 1; j < g_catalog_count; j++) {
                g_catalog[j - 1] = g_catalog[j];
            }
            g_catalog_count--;
            g_catalog[g_catalog_count] = NULL;
            catalog_write_page(ctx);
            return 1;
        }
    }
    if (catalog_view_query(ctx, table_name) != NULL) {
        snprintf(g_sql_ddl_error, sizeof(g_sql_ddl_error),
                 "'%.200s' is a view, not a table (use DROP VIEW)", table_name);
        return 0;
    }
    return if_exists;
}

/* -------------------------------------------------------------------------- */
/* CREATE VIEW / DROP VIEW                                                    */
/*                                                                            */
/* A view stores its SELECT text (trimmed, verbatim) in the V4 catalog. At    */
/* query time the stored SELECT is executed recursively and its rows become   */
/* a read-only source set (see execute_select). Views are validated at        */
/* CREATE time: the SELECT must parse and its FROM sources must exist.        */
/* -------------------------------------------------------------------------- */

/* Parses "CREATE VIEW <name> AS <select text>". On success *select_text
   points into query (not a copy) and *select_len is its trimmed length. */
static int sql_parse_create_view(const char* query, char* view_name, size_t view_name_size,
                                 const char** select_text, int* select_len) {
    SqlLexer lex;
    sql_lexer_init(&lex, query);

    SqlToken tok = sql_next_token(&lex);
    if (tok.type != TOK_CREATE) return 0;
    tok = sql_next_token(&lex);
    if (tok.type != TOK_VIEW) return 0;
    tok = sql_next_token(&lex);
    if (tok.type != TOK_IDENT) return 0;
    sql_token_text(&tok, view_name, view_name_size);
    tok = sql_next_token(&lex);
    if (tok.type != TOK_AS) return 0;

    /* The SELECT text is the remainder of the statement, trimmed. */
    const char* start = lex.current;
    while (*start != '\0' && isspace((unsigned char)*start)) start++;
    const char* end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) end--;
    if (end == start) return 0;
    *select_text = start;
    *select_len = (int)(end - start);
    return 1;
}

static int sql_parse_drop_view(const char* query, char* view_name, size_t view_name_size,
                               int* if_exists) {
    SqlLexer lex;
    sql_lexer_init(&lex, query);

    SqlToken tok = sql_next_token(&lex);
    if (tok.type != TOK_DROP) return 0;
    tok = sql_next_token(&lex);
    if (tok.type != TOK_VIEW) return 0;

    *if_exists = 0;
    tok = sql_next_token(&lex);
    if (tok.type == TOK_IF) {
        tok = sql_next_token(&lex);
        if (tok.type != TOK_EXISTS) return 0;
        *if_exists = 1;
        tok = sql_next_token(&lex);
    }
    if (tok.type != TOK_IDENT) return 0;
    sql_token_text(&tok, view_name, view_name_size);

    tok = sql_next_token(&lex);
    return tok.type == TOK_EOF;
}

static int execute_create_view(Context* ctx, const char* view_name,
                               const char* select_text, int select_len) {
    if (catalog_find_table(ctx, view_name) != NULL) {
        snprintf(g_sql_ddl_error, sizeof(g_sql_ddl_error),
                 "cannot create view '%.200s': a table with that name exists", view_name);
        return 0;
    }
    if (catalog_view_query(ctx, view_name) != NULL) {
        snprintf(g_sql_ddl_error, sizeof(g_sql_ddl_error),
                 "view '%.200s' already exists", view_name);
        return 0;
    }
    if (g_view_count >= MAX_CATALOG_VIEWS) {
        snprintf(g_sql_ddl_error, sizeof(g_sql_ddl_error),
                 "too many views (max %d)", MAX_CATALOG_VIEWS);
        return 0;
    }
    if (select_len > MAX_VIEW_QUERY_LEN) {
        snprintf(g_sql_ddl_error, sizeof(g_sql_ddl_error),
                 "view '%.200s' query too long (max %d bytes)", view_name, MAX_VIEW_QUERY_LEN);
        return 0;
    }

    char* query_copy = malloc((size_t)select_len + 1);
    if (query_copy == NULL) return 0;
    memcpy(query_copy, select_text, (size_t)select_len);
    query_copy[select_len] = '\0';

    /* Validate the definition: it must parse as a SELECT whose FROM sources
       name existing tables or views. */
    SelectStmt stmt;
    int valid = sql_parse_select(query_copy, &stmt) && view_source_exists(ctx, &stmt);
    sql_free_select_stmt(&stmt);
    if (!valid) {
        snprintf(g_sql_ddl_error, sizeof(g_sql_ddl_error),
                 "view '%.180s' has an invalid SELECT (unknown base table or view)",
                 view_name);
        free(query_copy);
        return 0;
    }

    g_views[g_view_count].name = strdup(view_name);
    if (g_views[g_view_count].name == NULL) {
        free(query_copy);
        return 0;
    }
    g_views[g_view_count].select_query = query_copy;
    g_view_count++;

    if (!catalog_write_page(ctx)) {
        /* The catalog page is full: roll back so memory and disk agree. */
        g_view_count--;
        free(g_views[g_view_count].name);
        free(g_views[g_view_count].select_query);
        g_views[g_view_count].name = NULL;
        g_views[g_view_count].select_query = NULL;
        snprintf(g_sql_ddl_error, sizeof(g_sql_ddl_error),
                 "catalog page full: cannot persist view '%.200s'", view_name);
        return 0;
    }
    return 1;
}

static int execute_drop_view(Context* ctx, const char* view_name, int if_exists) {
    for (int i = 0; i < g_view_count; i++) {
        if (g_views[i].name != NULL && strcmp(g_views[i].name, view_name) == 0) {
            free(g_views[i].name);
            free(g_views[i].select_query);
            for (int j = i + 1; j < g_view_count; j++) {
                g_views[j - 1] = g_views[j];
            }
            g_view_count--;
            g_views[g_view_count].name = NULL;
            g_views[g_view_count].select_query = NULL;
            catalog_write_page(ctx);
            return 1;
        }
    }
    if (catalog_find_table(ctx, view_name) != NULL) {
        snprintf(g_sql_ddl_error, sizeof(g_sql_ddl_error),
                 "'%.200s' is a table, not a view (use DROP TABLE)", view_name);
        return 0;
    }
    if (if_exists) return 1;
    snprintf(g_sql_ddl_error, sizeof(g_sql_ddl_error),
             "view '%.200s' does not exist", view_name);
    return 0;
}

static int execute_alter_add_column(Context* ctx, const char* table_name,
                                    const char* column_name, int column_type,
                                    int column_flags, const Cell* column_default) {
    Table* table = catalog_find_table(ctx, table_name);
    if (table == NULL) return 0;
    if (table->column_count >= MAX_COLUMNS) return 0;
    for (int i = 0; i < table->column_count; i++) {
        if (strcmp(table->columns[i].name, column_name) == 0) return 0;
    }

    /* Read existing rows under the old schema before touching the table. */
    Row* rows = NULL;
    int row_count = 0;
    if (!read_all_rows(ctx, table, &rows, &row_count)) return 0;

    /* Old rows are backfilled with the DEFAULT literal, or NULL when the new
       column has no DEFAULT. Reject combinations that would violate the new
       column's own constraints. */
    int has_default = (column_flags & COL_FLAG_HAS_DEFAULT) != 0;
    if (row_count > 0 && !has_default &&
        (column_flags & (COL_FLAG_NOT_NULL | COL_FLAG_PRIMARY_KEY))) {
        snprintf(g_sql_ddl_error, sizeof(g_sql_ddl_error),
                 "%s constraint violated: %s.%s",
                 constraint_label(column_flags), table_name, column_name);
        free_rows(rows, row_count);
        return 0;
    }
    if (row_count > 1 && has_default && column_default->type != VAL_NULL &&
        (column_flags & (COL_FLAG_UNIQUE | COL_FLAG_PRIMARY_KEY))) {
        /* Every old row would be backfilled with the same value. */
        snprintf(g_sql_ddl_error, sizeof(g_sql_ddl_error),
                 "%s constraint violated: %s.%s",
                 constraint_label(column_flags), table_name, column_name);
        free_rows(rows, row_count);
        return 0;
    }

    Column* new_columns = realloc(table->columns,
                                  sizeof(Column) * (size_t)(table->column_count + 1));
    if (new_columns == NULL) {
        free_rows(rows, row_count);
        return 0;
    }
    table->columns = new_columns;
    char* name_copy = strdup(column_name);
    if (name_copy == NULL) {
        free_rows(rows, row_count);
        return 0;
    }
    table->columns[table->column_count].name = name_copy;
    table->columns[table->column_count].type = column_type;
    table->columns[table->column_count].flags = column_flags;
    if (has_default) {
        table->columns[table->column_count].default_value = cell_dup((Cell*)column_default);
    } else {
        table->columns[table->column_count].default_value.type = VAL_NULL;
        table->columns[table->column_count].default_value.as.as_int = 0;
    }
    table->column_count++;

    /* Rebuild the row chain; pre-existing rows get the DEFAULT (or NULL). */
    free_row_pages(ctx, table);
    indexes_reset(ctx, table);
    for (int i = 0; i < row_count; i++) {
        Cell cells[MAX_COLUMNS];
        for (int j = 0; j < rows[i].field_count; j++) {
            cells[j] = rows[i].fields[j].value;
        }
        int last = table->column_count - 1;
        if (has_default) {
            cells[last] = cell_dup((Cell*)column_default);
        } else {
            cells[last].type = VAL_NULL;
            cells[last].as.as_int = 0;
        }
        catalog_insert(ctx, table, cells);
        if (cell_owns_text(cells[last].type)) {
            free(cells[last].as.as_string);
        }
    }
    free_rows(rows, row_count);

    catalog_write_page(ctx);
    return 1;
}

static int execute_alter_drop_column(Context* ctx, const char* table_name,
                                     const char* column_name) {
    Table* table = catalog_find_table(ctx, table_name);
    if (table == NULL) return 0;
    if (table->column_count <= 1) return 0; /* cannot drop the last column */

    int drop_index = -1;
    for (int i = 0; i < table->column_count; i++) {
        if (strcmp(table->columns[i].name, column_name) == 0) {
            drop_index = i;
            break;
        }
    }
    if (drop_index < 0) return 0;

    /* Read existing rows under the old schema before touching the table. */
    Row* rows = NULL;
    int row_count = 0;
    if (!read_all_rows(ctx, table, &rows, &row_count)) return 0;

    int new_count = table->column_count - 1;
    Column* new_columns = calloc((size_t)new_count, sizeof(Column));
    if (new_columns == NULL) {
        free_rows(rows, row_count);
        return 0;
    }
    int k = 0;
    for (int i = 0; i < table->column_count; i++) {
        if (i == drop_index) continue;
        new_columns[k].name = strdup(table->columns[i].name);
        if (new_columns[k].name == NULL) {
            for (int j = 0; j < k; j++) {
                free_column(&new_columns[j]);
            }
            free(new_columns);
            free_rows(rows, row_count);
            return 0;
        }
        new_columns[k].type = table->columns[i].type;
        new_columns[k].flags = table->columns[i].flags;
        new_columns[k].default_value = cell_dup(&table->columns[i].default_value);
        k++;
    }

    /* Indexes on the dropped column go away with it. */
    for (int i = 0; i < table->index_count; i++) {
        if (strcmp(table->indexes[i].column_name, column_name) == 0) {
            table_drop_index_at(ctx, table, i);
            i--;
        }
    }

    /* Rebuild the row chain without the dropped column. */
    free_row_pages(ctx, table);
    indexes_reset(ctx, table);
    for (int i = 0; i < table->column_count; i++) {
        free_column(&table->columns[i]);
    }
    free(table->columns);
    table->columns = new_columns;
    table->column_count = new_count;

    for (int i = 0; i < row_count; i++) {
        Cell cells[MAX_COLUMNS];
        int c = 0;
        for (int j = 0; j < rows[i].field_count; j++) {
            if (j == drop_index) continue;
            cells[c++] = rows[i].fields[j].value;
        }
        catalog_insert(ctx, table, cells);
    }
    free_rows(rows, row_count);

    catalog_write_page(ctx);
    return 1;
}

/* -------------------------------------------------------------------------- */
/* CREATE INDEX / DROP INDEX                                                  */
/* -------------------------------------------------------------------------- */

/* Frees the B-tree pages of one index and removes its metadata slot. */
static void table_drop_index_at(Context* ctx, Table* table, int i) {
    BTree* tree = btree_open(ctx->pager, table->indexes[i].root_page);
    if (tree != NULL) {
        btree_free_pages(tree);
        btree_destroy(tree);
    }
    free(table->indexes[i].name);
    free(table->indexes[i].column_name);
    for (int j = i + 1; j < table->index_count; j++) {
        table->indexes[j - 1] = table->indexes[j];
    }
    table->index_count--;
}

/* Empties every index on the table. Used when the row chain is about to be
   rebuilt (UPDATE/DELETE/ALTER): the rebuild re-inserts every surviving row
   through catalog_insert, which re-populates the indexes. */
static void indexes_reset(Context* ctx, Table* table) {
    for (int i = 0; i < table->index_count; i++) {
        TableIndex* idx = &table->indexes[i];
        BTree* old = btree_open(ctx->pager, idx->root_page);
        if (old != NULL) {
            btree_free_pages(old);
            btree_destroy(old);
        }
        BTree* fresh = btree_create(ctx->pager);
        idx->root_page = fresh != NULL ? btree_root_page(fresh) : 0;
        btree_destroy(fresh);
    }
}

static int sql_parse_create_index(const char* query, char* index_name, size_t index_name_size,
                                  char* table_name, size_t table_name_size,
                                  char* column_name, size_t column_name_size) {
    SqlLexer lex;
    sql_lexer_init(&lex, query);

    SqlToken tok = sql_next_token(&lex);
    if (tok.type != TOK_CREATE) return 0;
    tok = sql_next_token(&lex);
    if (tok.type != TOK_INDEX) return 0;
    tok = sql_next_token(&lex);
    if (tok.type != TOK_IDENT) return 0;
    sql_token_text(&tok, index_name, index_name_size);

    tok = sql_next_token(&lex);
    if (tok.type != TOK_ON) return 0;
    tok = sql_next_token(&lex);
    if (tok.type != TOK_IDENT) return 0;
    sql_token_text(&tok, table_name, table_name_size);

    tok = sql_next_token(&lex);
    if (tok.type != TOK_LPAREN) return 0;
    tok = sql_next_token(&lex);
    if (tok.type != TOK_IDENT) return 0;
    sql_token_text(&tok, column_name, column_name_size);

    tok = sql_next_token(&lex);
    if (tok.type != TOK_RPAREN) return 0;
    tok = sql_next_token(&lex);
    return tok.type == TOK_EOF;
}

static int sql_parse_drop_index(const char* query, char* index_name, size_t index_name_size,
                                char* table_name, size_t table_name_size) {
    SqlLexer lex;
    sql_lexer_init(&lex, query);

    SqlToken tok = sql_next_token(&lex);
    if (tok.type != TOK_DROP) return 0;
    tok = sql_next_token(&lex);
    if (tok.type != TOK_INDEX) return 0;
    tok = sql_next_token(&lex);
    if (tok.type != TOK_IDENT) return 0;
    sql_token_text(&tok, index_name, index_name_size);
    table_name[0] = '\0';

    tok = sql_next_token(&lex);
    if (tok.type == TOK_ON) {
        tok = sql_next_token(&lex);
        if (tok.type != TOK_IDENT) return 0;
        sql_token_text(&tok, table_name, table_name_size);
        tok = sql_next_token(&lex);
    }
    return tok.type == TOK_EOF;
}

/* Populates a fresh index tree by walking the table's row chain. */
static int index_build_from_rows(Context* ctx, Table* table, int column, BTree* tree) {
    Pager* pager = ctx->pager;
    int page_num = table->first_row_page;
    while (page_num != 0) {
        uint8_t page[PAGE_SIZE];
        pager_read_page(pager, page_num, page);

        int data_end = row_page_data_end(page);
        int offset = ROW_PAGE_HEADER_SIZE;
        while (offset < data_end) {
            int16_t record_size;
            memcpy(&record_size, page + offset, sizeof(record_size));
            if (record_size <= 0 || offset + (int)record_size > data_end) return 0;

            Row* row = deserialize_row(table, page + offset);
            if (row == NULL) return 0;
            int inserted = column >= row->field_count ||
                btree_insert(tree, &row->fields[column].value, page_num, offset);
            free_row(row);
            free(row);
            if (!inserted) return 0;
            offset += (int)record_size;
        }

        page_num = row_page_next(pager, page);
    }
    return 1;
}

static int execute_create_index(Context* ctx, const char* index_name,
                                const char* table_name, const char* column_name) {
    Table* table = catalog_find_table(ctx, table_name);
    if (table == NULL) return 0;
    int column = table_column_index(table, column_name);
    if (column < 0) return 0;
    for (int i = 0; i < table->index_count; i++) {
        if (strcmp(table->indexes[i].name, index_name) == 0) return 0;
    }
    if (table->index_count >= MAX_TABLE_INDEXES) return 0;

    BTree* tree = btree_create(ctx->pager);
    if (tree == NULL) return 0;
    if (!index_build_from_rows(ctx, table, column, tree)) {
        btree_free_pages(tree);
        btree_destroy(tree);
        return 0;
    }
    int root_page = btree_root_page(tree);
    btree_destroy(tree);

    TableIndex* new_indexes = realloc(table->indexes,
                                      sizeof(TableIndex) * (size_t)(table->index_count + 1));
    if (new_indexes == NULL) {
        BTree* cleanup = btree_open(ctx->pager, root_page);
        if (cleanup != NULL) {
            btree_free_pages(cleanup);
            btree_destroy(cleanup);
        }
        return 0;
    }
    table->indexes = new_indexes;
    TableIndex* idx = &table->indexes[table->index_count];
    idx->name = strdup(index_name);
    idx->column_name = strdup(column_name);
    idx->root_page = root_page;
    if (idx->name == NULL || idx->column_name == NULL) {
        free(idx->name);
        free(idx->column_name);
        BTree* cleanup = btree_open(ctx->pager, root_page);
        if (cleanup != NULL) {
            btree_free_pages(cleanup);
            btree_destroy(cleanup);
        }
        return 0;
    }
    table->index_count++;

    catalog_write_page(ctx);
    return 1;
}

static int execute_drop_index(Context* ctx, const char* index_name, const char* table_name) {
    for (int i = 0; i < g_catalog_count; i++) {
        Table* table = g_catalog[i];
        if (table_name != NULL && strcmp(table->name, table_name) != 0) continue;
        for (int j = 0; j < table->index_count; j++) {
            if (strcmp(table->indexes[j].name, index_name) == 0) {
                table_drop_index_at(ctx, table, j);
                catalog_write_page(ctx);
                return 1;
            }
        }
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Column type resolution                                                     */
/* -------------------------------------------------------------------------- */

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

static const char* skip_space(const char* p) { while (*p && is_space(*p)) p++; return p; }

static int starts_with_ci(const char* p, const char* word) {
    const char* w = word;
    while (*w) {
        if (tolower((unsigned char)*p) != tolower((unsigned char)*w)) return 0;
        p++; w++;
    }
    return 1;
}

/* Resolves the type of a column exposed by a view definition, recursing into
   nested views (views may be defined over other views). The depth bound is a
   safety net; the catalog cannot contain reference cycles because a view's
   sources must exist when it is created. */
static int view_column_type(Context* ctx, const char* view_query,
                            const char* column_name, int* out_type, int depth) {
    if (depth > MAX_CATALOG_VIEWS) return 0;

    SelectStmt stmt;
    if (!sql_parse_select(view_query, &stmt)) {
        sql_free_select_stmt(&stmt);
        return 0;
    }

    int star = stmt.column_count == 0;
    int col_index = -1;
    for (int i = 0; i < stmt.column_count; i++) {
        if (stmt.column_table_prefix[i][0] == '\0' &&
            strcasecmp(stmt.column_names[i], column_name) == 0) {
            col_index = i;
            break;
        }
    }
    if (!star && col_index < 0) {
        sql_free_select_stmt(&stmt);
        return 0;
    }

    int result = 0;
    Table* table = catalog_find_table(ctx, stmt.table_name);
    if (table != NULL) {
        if (col_index >= 0 && stmt.column_aggregates[col_index] != AGG_NONE) {
            /* Aggregate output: COUNT is an int, the rest are numeric. */
            *out_type = stmt.column_aggregates[col_index] == AGG_COUNT ? VAL_INT : VAL_FLOAT;
            result = 1;
        } else {
            for (int c = 0; c < table->column_count; c++) {
                if (strcasecmp(table->columns[c].name, column_name) == 0) {
                    *out_type = table->columns[c].type;
                    result = 1;
                    break;
                }
            }
        }
    } else {
        const char* inner = catalog_view_query(ctx, stmt.table_name);
        if (inner != NULL) {
            result = view_column_type(ctx, inner, column_name, out_type, depth + 1);
        }
    }

    sql_free_select_stmt(&stmt);
    return result;
}

int sql_query_column_type(Context* ctx, const char* query, const char* column_name, int* out_type) {
    if (ctx == NULL || query == NULL || column_name == NULL || out_type == NULL) return 0;

    const char* p = skip_space(query);
    if (!starts_with_ci(p, "SELECT")) return 0;
    p += 6;
    p = skip_space(p);
    if (*p == '\0') return 0;

    /* Locate FROM as a whole word. */
    const char* from_pos = NULL;
    const char* q = p;
    while (*q) {
        if (starts_with_ci(q, "FROM")) {
            int preceded_by_ws = (q == p) || is_space(*(q - 1));
            int followed_by_ws = is_space(*(q + 4)) || *(q + 4) == '\0';
            if (preceded_by_ws && followed_by_ws) {
                from_pos = q;
                break;
            }
        }
        q++;
    }
    if (from_pos == NULL) return 0;

    /* Parse comma-separated selected columns between SELECT and FROM. */
    int has_star = 0;
    int selected = 0;
    const char* s = p;
    while (s < from_pos) {
        s = skip_space(s);
        const char* tok_start = s;
        while (s < from_pos && !is_space(*s) && *s != ',') s++;
        const char* tok_end = s;
        while (tok_end > tok_start && is_space(*(tok_end - 1))) tok_end--;

        int tok_len = (int)(tok_end - tok_start);
        if (tok_len > 0) {
            if (tok_len == 1 && *tok_start == '*') {
                has_star = 1;
            } else if (tok_len == (int)strlen(column_name) &&
                       strncasecmp(tok_start, column_name, tok_len) == 0) {
                selected = 1;
            }
        }

        s = skip_space(s);
        if (s < from_pos && *s == ',') {
            s++;
        }
    }

    /* Extract table name after FROM. */
    const char* t = from_pos + 4;
    t = skip_space(t);
    char table_name[MAX_NAME_LEN + 1];
    int i = 0;
    while (*t && !is_space(*t) && i < MAX_NAME_LEN) {
        table_name[i++] = *t++;
    }
    table_name[i] = '\0';
    if (table_name[0] == '\0') return 0;

    Table* table = catalog_find_table(ctx, table_name);
    if (table == NULL) {
        /* The FROM clause may name a view: resolve the column through the
           view's stored SELECT (views may nest). */
        const char* view_query = catalog_view_query(ctx, table_name);
        if (view_query == NULL) return 0;
        if (!has_star && !selected) return 0;
        return view_column_type(ctx, view_query, column_name, out_type, 0);
    }
    if (!has_star && !selected) return 0;

    for (int c = 0; c < table->column_count; c++) {
        if (strcmp(table->columns[c].name, column_name) == 0) {
            *out_type = table->columns[c].type;
            return 1;
        }
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Public query API                                                           */
/* -------------------------------------------------------------------------- */

Result* sql_exec(const char* query, Context* ctx) {
    if (ctx == NULL || ctx->pager == NULL) return NULL;

    SelectStmt stmt;
    if (!sql_parse_select(query, &stmt)) {
        /* The statement was zeroed on entry; free any WHERE tree that was
           built before the parse failed. */
        sql_free_select_stmt(&stmt);
        return result_create(0);
    }

    Result* res = execute_select(ctx, &stmt);
    sql_free_select_stmt(&stmt);
    return res;
}

/* Rewrites the bare `?` placeholders of `query` (outside string literals) to
   `?1`, `?2`, ... in order, so each keeps its position when the statement is
   later split up (INSERT ... SELECT hands the SELECT to sql_exec on its own).
   Returns a malloc'd copy, or NULL with g_sql_ddl_error set when the number
   of placeholders differs from `param_count`. Existing `?N` are left alone. */
static char* sql_number_placeholders(const char* query, int param_count) {
    size_t len = strlen(query);
    int placeholders = 0;
    char quote = '\0';
    for (const char* p = query; *p != '\0'; p++) {
        if (quote != '\0') {
            if (*p == quote) quote = '\0';
        } else if (*p == '\'' || *p == '"') {
            quote = *p;
        } else if (*p == '?' && !isdigit((unsigned char)p[1])) {
            placeholders++;
        }
    }
    if (placeholders != param_count) {
        snprintf(g_sql_ddl_error, sizeof(g_sql_ddl_error),
                 "SQL has %d bind placeholder(s) but %d value(s) were supplied",
                 placeholders, param_count);
        return NULL;
    }

    char* out = malloc(len + (size_t)placeholders * 11 + 1);
    if (out == NULL) return NULL;
    char* dst = out;
    int next = 1;
    quote = '\0';
    for (const char* p = query; *p != '\0'; p++) {
        *dst++ = *p;
        if (quote != '\0') {
            if (*p == quote) quote = '\0';
        } else if (*p == '\'' || *p == '"') {
            quote = *p;
        } else if (*p == '?' && !isdigit((unsigned char)p[1])) {
            dst += sprintf(dst, "%d", next++);
        }
    }
    *dst = '\0';
    return out;
}

Result* sql_exec_params(const char* query, Context* ctx, const Value* params, int param_count) {
    g_sql_ddl_error[0] = '\0';
    char* numbered = NULL;
    if (param_count > 0) {
        numbered = sql_number_placeholders(query, param_count);
        if (numbered == NULL) return NULL;
    }

    /* Bind for this statement only; restored so a nested statement (a trigger
       body run from inside DML) never sees another statement's values. */
    const Value* saved_binds = g_sql_binds;
    int saved_count = g_sql_bind_count;
    g_sql_binds = param_count > 0 ? params : NULL;
    g_sql_bind_count = param_count > 0 ? param_count : 0;
    Result* res = sql_exec(numbered != NULL ? numbered : query, ctx);
    g_sql_binds = saved_binds;
    g_sql_bind_count = saved_count;
    free(numbered);
    return res;
}

int sql_exec_ddl_params(const char* query, Context* ctx, const Value* params, int param_count) {
    g_sql_ddl_error[0] = '\0';
    char* numbered = NULL;
    if (param_count > 0) {
        numbered = sql_number_placeholders(query, param_count);
        if (numbered == NULL) return 0;
    }

    const Value* saved_binds = g_sql_binds;
    int saved_count = g_sql_bind_count;
    g_sql_binds = param_count > 0 ? params : NULL;
    g_sql_bind_count = param_count > 0 ? param_count : 0;
    int rc = sql_exec_ddl(numbered != NULL ? numbered : query, ctx);
    g_sql_binds = saved_binds;
    g_sql_bind_count = saved_count;
    free(numbered);
    return rc;
}

Row* result_next(Result* res) {
    if (res == NULL || res->current >= res->row_count) {
        return NULL;
    }
    return &res->rows[res->current++];
}

void result_free(Result* res) {
    if (res == NULL) return;
    for (int i = 0; i < res->row_count; i++) {
        Row* row = &res->rows[i];
        for (int j = 0; j < row->field_count; j++) {
            free(row->fields[j].name);
            if (cell_owns_text(row->fields[j].value.type)) {
                free(row->fields[j].value.as.as_string);
            }
        }
        free(row->fields);
    }
    free(res->rows);
    free(res);
}

Cell row_get_field(Row* row, const char* name) {
    Cell empty;
    empty.type = VAL_INT;
    empty.as.as_int = 0;
    if (row == NULL || name == NULL) return empty;
    for (int i = 0; i < row->field_count; i++) {
        if (row->fields[i].name != NULL && strcmp(row->fields[i].name, name) == 0) {
            return row->fields[i].value;
        }
    }
    return empty;
}

/* -------------------------------------------------------------------------- */
/* Sequence persistence (catalog V5 sequence section)                         */
/* -------------------------------------------------------------------------- */

int catalog_sequence_list(Context* ctx, DBSequence* out, int max) {
    (void)ctx;
    if (out == NULL || max <= 0) return 0;
    int n = g_sequence_count < max ? g_sequence_count : max;
    for (int i = 0; i < n; i++) {
        snprintf(out[i].name, sizeof(out[i].name), "%s", g_sequences[i].name);
        out[i].has_value = g_sequences[i].has_value;
        out[i].current = g_sequences[i].current;
        out[i].increment = g_sequences[i].increment;
    }
    return n;
}

int catalog_sequence_save(Context* ctx, const DBSequence* seq) {
    if (ctx == NULL || ctx->pager == NULL || seq == NULL || seq->name[0] == '\0') return 0;
    /* Snapshot for rollback: the catalog write fails cleanly when the page
       would overflow, and the in-memory state must stay unchanged then. */
    SequenceDef backup[MAX_CATALOG_SEQUENCES];
    int backup_count = g_sequence_count;
    memcpy(backup, g_sequences, sizeof(backup));

    int found = 0;
    for (int i = 0; i < g_sequence_count; i++) {
        if (strcmp(g_sequences[i].name, seq->name) == 0) {
            g_sequences[i].has_value = seq->has_value;
            g_sequences[i].current = seq->current;
            g_sequences[i].increment = seq->increment;
            found = 1;
            break;
        }
    }
    if (!found) {
        if (g_sequence_count >= MAX_CATALOG_SEQUENCES) {
            snprintf(g_sql_ddl_error, sizeof(g_sql_ddl_error),
                     "too many sequences (max %d)", MAX_CATALOG_SEQUENCES);
            return 0;
        }
        g_sequences[g_sequence_count].name = strdup(seq->name);
        if (g_sequences[g_sequence_count].name == NULL) {
            snprintf(g_sql_ddl_error, sizeof(g_sql_ddl_error), "out of memory");
            return 0;
        }
        g_sequences[g_sequence_count].has_value = seq->has_value;
        g_sequences[g_sequence_count].current = seq->current;
        g_sequences[g_sequence_count].increment = seq->increment;
        g_sequence_count++;
    }
    if (!catalog_write_page(ctx)) {
        /* Roll back. On the append path the backup still owns the name
           pointer, so free the copy that was just added. */
        if (!found) {
            free(g_sequences[g_sequence_count - 1].name);
        }
        memcpy(g_sequences, backup, sizeof(backup));
        g_sequence_count = backup_count;
        snprintf(g_sql_ddl_error, sizeof(g_sql_ddl_error),
                 "catalog page full: could not persist sequence '%s'", seq->name);
        return 0;
    }
    return 1;
}

int catalog_sequence_drop(Context* ctx, const char* name) {
    if (ctx == NULL || ctx->pager == NULL || name == NULL) return 0;
    for (int i = 0; i < g_sequence_count; i++) {
        if (strcmp(g_sequences[i].name, name) == 0) {
            SequenceDef backup[MAX_CATALOG_SEQUENCES];
            int backup_count = g_sequence_count;
            memcpy(backup, g_sequences, sizeof(backup));

            char* removed = g_sequences[i].name;
            for (int j = i + 1; j < g_sequence_count; j++) {
                g_sequences[j - 1] = g_sequences[j];
            }
            g_sequence_count--;
            g_sequences[g_sequence_count].name = NULL;
            if (!catalog_write_page(ctx)) {
                memcpy(g_sequences, backup, sizeof(backup));
                g_sequence_count = backup_count;
                snprintf(g_sql_ddl_error, sizeof(g_sql_ddl_error),
                         "catalog page full: could not persist sequence drop '%s'", name);
                return 0;
            }
            free(removed);
            return 1;
        }
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Custom engine DBDriver implementation                                      */
/* -------------------------------------------------------------------------- */

typedef struct {
    Context ctx;
} CustomDriverImpl;

#define DEFAULT_DB_PATH "auspex.db"
#define LEGACY_DB_PATH "mypl.db"

const char* default_db_path(void) {
    static int warned = 0;
    if (!os_file_exists(DEFAULT_DB_PATH) && os_file_exists(LEGACY_DB_PATH)) {
        if (!warned) {
            fprintf(stderr, "auspex: using legacy database '" LEGACY_DB_PATH "'; rename it "
                            "(and its .packages/.programs files) to '" DEFAULT_DB_PATH
                            "' (support ends in v0.4.0)\n");
            warned = 1;
        }
        return LEGACY_DB_PATH;
    }
    return DEFAULT_DB_PATH;
}

static int custom_open(DBDriver* driver, const char* connection_string) {
    CustomDriverImpl* impl = malloc(sizeof(CustomDriverImpl));
    if (impl == NULL) {
        snprintf(driver->error_message, sizeof(driver->error_message), "out of memory");
        return 0;
    }
    impl->ctx.db_path = connection_string != NULL ? connection_string : default_db_path();
    impl->ctx.pager = NULL;
    if (!catalog_open(&impl->ctx)) {
        snprintf(driver->error_message, sizeof(driver->error_message),
                 "could not open catalog: %s", impl->ctx.db_path);
        free(impl);
        return 0;
    }
    driver->impl = impl;
    driver->error_message[0] = '\0';
    if (connection_string != NULL) {
        snprintf(driver->connection_string, sizeof(driver->connection_string), "%s", connection_string);
    } else {
        driver->connection_string[0] = '\0';
    }
    return 1;
}

static void custom_close(DBDriver* driver) {
    CustomDriverImpl* impl = (CustomDriverImpl*)driver->impl;
    if (impl == NULL) return;
    catalog_close(&impl->ctx);
    free(impl);
    driver->impl = NULL;
}

static int custom_exec(DBDriver* driver, const char* sql, Value* params, int param_count) {
    CustomDriverImpl* impl = (CustomDriverImpl*)driver->impl;
    /* Install the VM's row-level trigger hook (NULL when none is active).
       Save/restore so a nested statement executed from inside a trigger body
       (execute_immediate) does not clobber the outer statement's hook. */
    RowTriggerFn saved_fn = g_row_trigger_fn;
    void* saved_user = g_row_trigger_user;
    g_row_trigger_fn = driver->row_trigger_fn;
    g_row_trigger_user = driver->row_trigger_user;
    int row_count = sql_exec_ddl_params(sql, &impl->ctx, params, param_count);
    g_row_trigger_fn = saved_fn;
    g_row_trigger_user = saved_user;
    if (!row_count) {
        if (g_sql_ddl_error[0] != '\0') {
            snprintf(driver->error_message, sizeof(driver->error_message),
                     "%s", g_sql_ddl_error);
        } else {
            snprintf(driver->error_message, sizeof(driver->error_message),
                     "custom engine: could not execute '%s'", sql);
        }
        return -1;
    }
    driver->error_message[0] = '\0';
    return row_count;
}

static int custom_query(DBDriver* driver, const char* sql, Value* params, int param_count, void** result_handle) {
    CustomDriverImpl* impl = (CustomDriverImpl*)driver->impl;
    Result* res = sql_exec_params(sql, &impl->ctx, params, param_count);
    if (res == NULL) {
        if (g_sql_ddl_error[0] != '\0') {
            snprintf(driver->error_message, sizeof(driver->error_message),
                     "%s", g_sql_ddl_error);
        } else {
            snprintf(driver->error_message, sizeof(driver->error_message),
                     "custom engine: could not execute '%s'", sql);
        }
        return 0;
    }
    driver->error_message[0] = '\0';
    *result_handle = res;
    return 1;
}

static int custom_result_next(DBDriver* driver, void* result_handle, void** row_handle) {
    (void)driver;
    Row* row = result_next((Result*)result_handle);
    if (row == NULL) return 0;
    *row_handle = row;
    return 1;
}

int sql_cell_to_value(const Cell* cell, Value* out) {
    if (cell == NULL || out == NULL) return 0;
    switch (cell->type) {
        case VAL_INT:    *out = value_int(cell->as.as_int);     return 1;
        case VAL_FLOAT:  *out = value_float(cell->as.as_float); return 1;
        case VAL_BOOL:   *out = value_bool(cell->as.as_int);    return 1;
        case VAL_STRING:
            *out = value_string(strdup(cell->as.as_string != NULL
                                       ? cell->as.as_string : ""));
            return 1;
        case VAL_DATE:
            *out = value_date(strdup(cell->as.as_string != NULL
                                     ? cell->as.as_string : ""));
            return 1;
        case VAL_TIMESTAMP:
            *out = value_timestamp(strdup(cell->as.as_string != NULL
                                          ? cell->as.as_string : ""));
            return 1;
        default:         return 0;
    }
}

static int custom_row_get_field(DBDriver* driver, void* row_handle, const char* name, Value* out) {
    Cell cell = row_get_field((Row*)row_handle, name);
    if (!sql_cell_to_value(&cell, out)) {
        if (cell.type == VAL_NULL) {
            *out = value_null();
            driver->error_message[0] = '\0';
            return 1;
        }
        /* Only reachable for a cell type with no runtime scalar. A genuinely
           missing column is not detectable here: row_get_field returns an
           int 0 cell for one, which is indistinguishable from a real int 0.
           The message predates that and is left alone. */
        snprintf(driver->error_message, sizeof(driver->error_message),
                 "column '%s' not found", name);
        *out = value_int(0);
        return 0;
    }
    driver->error_message[0] = '\0';
    return 1;
}

static int custom_row_get_column(DBDriver* driver, void* row_handle, int index, Value* out) {
    (void)driver;
    Row* row = (Row*)row_handle;
    if (row == NULL || index < 0 || index >= row->field_count) {
        *out = value_int(0);
        return 0;
    }
    Cell cell = row->fields[index].value;
    if (sql_cell_to_value(&cell, out)) return 1;
    if (cell.type == VAL_NULL) {
        *out = value_null();
        return 1;
    }
    *out = value_int(0);
    return 0;
}

static int custom_result_column_count(DBDriver* driver, void* result_handle) {
    (void)driver;
    Result* res = (Result*)result_handle;
    if (res == NULL || res->row_count == 0) return 0;
    return res->rows[0].field_count;
}

static const char* custom_result_column_name(DBDriver* driver, void* result_handle, int index) {
    (void)driver;
    Result* res = (Result*)result_handle;
    if (res == NULL || res->row_count == 0) return NULL;
    Row* row = &res->rows[0];
    if (index < 0 || index >= row->field_count) return NULL;
    return row->fields[index].name;
}

static void custom_result_free(DBDriver* driver, void* result_handle) {
    (void)driver;
    result_free((Result*)result_handle);
}

static int custom_begin(DBDriver* driver) {
    (void)driver;
    return 0; /* not supported by custom engine */
}

static int custom_commit(DBDriver* driver) {
    (void)driver;
    return 0; /* not supported by custom engine */
}

static int custom_rollback(DBDriver* driver) {
    (void)driver;
    return 0; /* not supported by custom engine */
}

static int custom_savepoint(DBDriver* driver, const char* name) {
    (void)driver;
    (void)name;
    return 0; /* not supported by custom engine */
}

static int custom_rollback_to_savepoint(DBDriver* driver, const char* name) {
    (void)driver;
    (void)name;
    return 0; /* not supported by custom engine */
}

static int custom_release_savepoint(DBDriver* driver, const char* name) {
    (void)driver;
    (void)name;
    return 0; /* not supported by custom engine */
}

static int custom_sequence_load(DBDriver* driver, DBSequence* out, int max, int* out_count) {
    if (out_count == NULL) return 0;
    *out_count = catalog_sequence_list(NULL, out, max);
    driver->error_message[0] = '\0';
    return 1;
}

static int custom_sequence_save(DBDriver* driver, const DBSequence* seq) {
    CustomDriverImpl* impl = (CustomDriverImpl*)driver->impl;
    if (!catalog_sequence_save(&impl->ctx, seq)) {
        if (g_sql_ddl_error[0] != '\0') {
            snprintf(driver->error_message, sizeof(driver->error_message),
                     "%s", g_sql_ddl_error);
        } else {
            snprintf(driver->error_message, sizeof(driver->error_message),
                     "could not persist sequence");
        }
        return 0;
    }
    driver->error_message[0] = '\0';
    return 1;
}

static int custom_sequence_drop(DBDriver* driver, const char* name) {
    CustomDriverImpl* impl = (CustomDriverImpl*)driver->impl;
    if (!catalog_sequence_drop(&impl->ctx, name)) {
        if (g_sql_ddl_error[0] != '\0') {
            snprintf(driver->error_message, sizeof(driver->error_message),
                     "%s", g_sql_ddl_error);
        } else {
            snprintf(driver->error_message, sizeof(driver->error_message),
                     "could not drop sequence '%s'", name);
        }
        return 0;
    }
    driver->error_message[0] = '\0';
    return 1;
}

void custom_driver_init(DBDriver* driver) {
    driver->impl = NULL;
    driver->is_sqlite = 0;
    driver->init = custom_driver_init;
    driver->connection_string[0] = '\0';
    driver->open = custom_open;
    driver->close = custom_close;
    driver->exec = custom_exec;
    driver->query = custom_query;
    driver->result_next = custom_result_next;
    driver->row_get_field = custom_row_get_field;
    driver->row_get_column = custom_row_get_column;
    driver->result_column_count = custom_result_column_count;
    driver->result_column_name = custom_result_column_name;
    driver->result_free = custom_result_free;
    driver->begin = custom_begin;
    driver->commit = custom_commit;
    driver->rollback = custom_rollback;
    driver->savepoint = custom_savepoint;
    driver->rollback_to_savepoint = custom_rollback_to_savepoint;
    driver->release_savepoint = custom_release_savepoint;
    driver->sequence_load = custom_sequence_load;
    driver->sequence_save = custom_sequence_save;
    driver->sequence_drop = custom_sequence_drop;
    driver->row_trigger_fn = NULL;
    driver->row_trigger_user = NULL;
}
