#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "vm.h"
#include "ast.h"
#include "diagnostics.h"
#include "natives.h"
#include "sql_engine.h"
#include "stored_programs.h"
#include "trigger.h"
#include "os.h"

#define TRY_MAX 64
#define MAX_OUT_PARAMS 16

typedef struct {
    uint8_t* catch_ip;
    int      frame_count;
    Value*   frame_base;
    int      local_count;
    Value*   stack_top;
} TryFrame;

#define UTL_FILE_MAX_HANDLES 64
#define SEQUENCE_MAX 16
#define SEQUENCE_NAME_MAX 64
#define DBMS_SQL_MAX_CURSORS 16
#define DBMS_SQL_MAX_BINDS 16

typedef struct {
    int out_count;
    int out_positions[MAX_OUT_PARAMS];
    int out_slots[MAX_OUT_PARAMS];
    int entry_offset;   /* chunk offset the call entered: which proc runs */
} OutParamFrame;

typedef struct {
    int  used;
    int  has_value;
    int  current;
    int  increment;
    char name[SEQUENCE_NAME_MAX];
} SequenceSlot;

typedef struct {
    int    active;
    char*  sql;
    Value  binds[DBMS_SQL_MAX_BINDS];
    int    bind_count;
    void*  result_handle;
    void*  row_handle;
    int    row_count;
    int    current_row;
    int    is_select;
    int    col_count;
} DBMS_SQL_Cursor;

struct VM {
    Chunk*        chunk;
    uint8_t*      ip;
    /* Value stack: heap array, grows by doubling (hard cap STACK_MAX).
       stack_top, frame_base, frames[] and try_frames[] hold pointers into
       this array; vm_ensure_stack_capacity re-bases them after realloc. */
    Value*        stack;
    int           stack_capacity;
    Value*        stack_top;
    /* Per-frame bookkeeping arrays, all grown together by
       vm_ensure_frame_capacity (indexed by frame_count). */
    Value**       frames;
    uint8_t**     return_ips;
    OutParamFrame* out_frames;
    int           frames_capacity;
    int           frame_count;
    Value*        frame_base;
    int           local_count;
    /* Frame depth at which OP_SET_LOCAL writes are mirrored into repl_locals.
       1 for whole-program runs (main executes one frame below the bootstrap),
       0 for REPL incremental fragments (no bootstrap; the fragment body is
       the outermost frame). Child VMs inherit the default and never have
       their mirrors read, so the value is irrelevant for them. */
    int           capture_base;
    /* REPL local mirror (top-level main frame), heap array grown on demand. */
    Value*        repl_locals;
    int           repl_local_capacity;
    int           repl_local_count;
    void*         result_handle;
    void*         row_handle;
    struct Context* context;
    DBDriver*     driver;
    Value         sql_params[16];
    int           sql_param_count;
    int           sql_line;
    int           sql_rowcount;
    TryFrame      try_frames[TRY_MAX];
    int           try_count;
    char          error_message[256];
    int           sql_code;
    char          sql_errm[256];
    Value         globals[256];
    int           global_count;
    /* 1 in short-lived child VMs (trigger firing, autonomous transactions):
       children never run the cycle collector; only the top-level VM does. */
    int           is_child;
    /* A child VM's creator and the chunk offset it started at (-1 in a
       top-level VM), so a trigger's DML can see every trigger running above
       it, across the child VMs dynamic SQL and row triggers run in. */
    struct VM*    parent;
    int           entry_offset;
    int           dbms_output_enabled;
    int           dbms_output_limit;
    ArrayObj*     dbms_output_buffer;
    FILE*         utl_file_handles[UTL_FILE_MAX_HANDLES];
    SequenceSlot  sequences[SEQUENCE_MAX];
    int           sequences_loaded;
    DBMS_SQL_Cursor dbms_sql_cursors[DBMS_SQL_MAX_CURSORS];
};

/* Initial capacities for the dynamically grown VM arrays. Each doubles on
   demand up to the STACK_MAX hard cap. */
#define VM_INITIAL_STACK_CAPACITY 256
#define VM_INITIAL_FRAME_CAPACITY 256
#define VM_INITIAL_REPL_CAPACITY  256

VM* vm_init(void) {
    VM* vm = malloc(sizeof(VM));
    if (vm == NULL) return NULL;
    vm->stack_capacity = VM_INITIAL_STACK_CAPACITY;
    vm->stack = malloc(sizeof(Value) * (size_t)vm->stack_capacity);
    vm->frames_capacity = VM_INITIAL_FRAME_CAPACITY;
    vm->frames = malloc(sizeof(Value*) * (size_t)vm->frames_capacity);
    vm->return_ips = malloc(sizeof(uint8_t*) * (size_t)vm->frames_capacity);
    vm->out_frames = malloc(sizeof(OutParamFrame) * (size_t)vm->frames_capacity);
    vm->repl_local_capacity = VM_INITIAL_REPL_CAPACITY;
    vm->repl_locals = malloc(sizeof(Value) * (size_t)vm->repl_local_capacity);
    if (vm->stack == NULL || vm->frames == NULL || vm->return_ips == NULL ||
        vm->out_frames == NULL || vm->repl_locals == NULL) {
        free(vm->stack);
        free(vm->frames);
        free(vm->return_ips);
        free(vm->out_frames);
        free(vm->repl_locals);
        free(vm);
        return NULL;
    }
    vm->chunk = NULL;
    vm->ip = NULL;
    vm->stack_top = vm->stack;
    vm->frame_count = 0;
    vm->frame_base = vm->stack;
    vm->local_count = 0;
    vm->capture_base = 1;
    vm->repl_local_count = 0;
    vm->result_handle = NULL;
    vm->row_handle = NULL;
    vm->context = NULL;
    vm->driver = NULL;
    vm->sql_param_count = 0;
    vm->sql_line = 0;
    vm->sql_rowcount = 0;
    vm->try_count = 0;
    vm->error_message[0] = '\0';
    vm->sql_code = 0;
    vm->sql_errm[0] = '\0';
    vm->global_count = 0;
    for (int i = 0; i < 256; i++) {
        vm->globals[i].type = VAL_INT;
        vm->globals[i].as.as_int = 0;
    }
    vm->dbms_output_enabled = 0;
    vm->dbms_output_limit = 0;
    vm->dbms_output_buffer = NULL;
    vm->is_child = 0;
    vm->parent = NULL;
    vm->entry_offset = -1;
    for (int i = 0; i < UTL_FILE_MAX_HANDLES; i++) {
        vm->utl_file_handles[i] = NULL;
    }
    for (int i = 0; i < SEQUENCE_MAX; i++) {
        vm->sequences[i].used = 0;
        vm->sequences[i].has_value = 0;
        vm->sequences[i].current = 0;
        vm->sequences[i].increment = 1;
        vm->sequences[i].name[0] = '\0';
    }
    vm->sequences_loaded = 0;
    for (int i = 0; i < DBMS_SQL_MAX_CURSORS; i++) {
        vm->dbms_sql_cursors[i].active = 0;
        vm->dbms_sql_cursors[i].sql = NULL;
        vm->dbms_sql_cursors[i].bind_count = 0;
        vm->dbms_sql_cursors[i].result_handle = NULL;
        vm->dbms_sql_cursors[i].row_handle = NULL;
        vm->dbms_sql_cursors[i].row_count = 0;
        vm->dbms_sql_cursors[i].current_row = -1;
        vm->dbms_sql_cursors[i].is_select = 0;
        vm->dbms_sql_cursors[i].col_count = 0;
    }
    return vm;
}

static int current_line(VM* vm) {
    if (vm->chunk == NULL || vm->chunk->lines == NULL || vm->ip == NULL) return 0;
    int offset = (int)(vm->ip - vm->chunk->code) - 1;
    if (offset < 0 || offset >= vm->chunk->lines_count) return 0;
    return vm->chunk->lines[offset];
}

static int current_column(VM* vm) {
    if (vm->chunk == NULL || vm->chunk->columns == NULL || vm->ip == NULL) return 0;
    int offset = (int)(vm->ip - vm->chunk->code) - 1;
    if (offset < 0 || offset >= vm->chunk->columns_count) return 0;
    return vm->chunk->columns[offset];
}

/* Location to report a runtime error at. Code that came from the built-in or
 * stored declarations prepended to the user's source has a negative line (see
 * format_error); an error raised inside a built-in wrapper such as
 * dbms_output.put_line is the caller's, so report the nearest calling frame
 * that is in the user's own source instead. When there is none (the whole
 * stack is prepended code) the negative line stays and the message says so. */
static void error_location(VM* vm, int* line, int* column) {
    *line = current_line(vm);
    *column = current_column(vm);
    if (*line >= 0 || vm->chunk == NULL) return;
    for (int f = vm->frame_count - 1; f >= 0; f--) {
        const uint8_t* ip = vm->return_ips[f];
        if (ip == NULL) continue;
        int offset = (int)(ip - vm->chunk->code) - 1;
        if (offset < 0 || offset >= vm->chunk->lines_count || vm->chunk->lines == NULL) continue;
        if (vm->chunk->lines[offset] > 0) {
            *line = vm->chunk->lines[offset];
            *column = (vm->chunk->columns != NULL && offset < vm->chunk->columns_count)
                          ? vm->chunk->columns[offset] : 0;
            return;
        }
    }
}

/* Converts a row cell from the custom engine into a runtime value. Cells with
   no runtime scalar - NULL today - become `fallback`, which differs by call
   site: most paths substitute int 0, dbms_sql.column_value reports NULL. */
static Value value_from_cell(Cell cell, Value fallback) {
    Value out;
    return sql_cell_to_value(&cell, &out) ? out : fallback;
}

static void set_runtime_error_ex(VM* vm, const char* message, int code) {
    int line, column;
    error_location(vm, &line, &column);
    format_error(vm->error_message, sizeof(vm->error_message),
                 vm->chunk != NULL ? vm->chunk->source_path : NULL,
                 line, column, message);
    vm->sql_code = code;
    strncpy(vm->sql_errm, vm->error_message, sizeof(vm->sql_errm) - 1);
    vm->sql_errm[sizeof(vm->sql_errm) - 1] = '\0';
}

static void set_runtime_error(VM* vm, const char* message) {
    set_runtime_error_ex(vm, message, 1);
}

static void set_runtime_error_from_driver(VM* vm, const char* prefix) {
    if (vm->driver != NULL && vm->driver->error_message[0] != '\0') {
        char msg[512];
        snprintf(msg, sizeof(msg), "%s: %s", prefix, vm->driver->error_message);
        set_runtime_error(vm, msg);
    } else {
        set_runtime_error(vm, prefix);
    }
}

/* Note: this helper is used by natives and other callers that want to set an
 * error from C code. It uses the current bytecode location when available. */

static void set_runtime_error_sql(VM* vm, const char* message) {
    char msg[512];
    snprintf(msg, sizeof(msg), "SQL error: %s", message);
    set_runtime_error_ex(vm, msg, 1);
}

static void set_runtime_error_from_driver_sql(VM* vm, const char* prefix) {
    if (vm->driver != NULL && vm->driver->error_message[0] != '\0') {
        char msg[512];
        snprintf(msg, sizeof(msg), "%s: %s", prefix, vm->driver->error_message);
        set_runtime_error_sql(vm, msg);
    } else {
        set_runtime_error_sql(vm, prefix);
    }
}

const char* vm_get_error(VM* vm) {
    if (vm == NULL) return NULL;
    return vm->error_message[0] != '\0' ? vm->error_message : NULL;
}

void vm_set_error(VM* vm, const char* message) {
    if (vm == NULL) return;
    set_runtime_error(vm, message);
}

void vm_set_error_with_code(VM* vm, const char* message, int code) {
    if (vm == NULL) return;
    set_runtime_error_ex(vm, message, code);
}

DBDriver* vm_get_driver(VM* vm) {
    if (vm == NULL) return NULL;
    return vm->driver;
}

void vm_set_sql_rowcount(VM* vm, int rowcount) {
    if (vm == NULL) return;
    vm->sql_rowcount = rowcount;
}

int vm_get_sql_rowcount(VM* vm) {
    if (vm == NULL) return 0;
    return vm->sql_rowcount;
}

int vm_get_sql_found(VM* vm) {
    if (vm == NULL) return 0;
    return vm->sql_rowcount > 0;
}

int vm_get_sql_notfound(VM* vm) {
    if (vm == NULL) return 0;
    return vm->sql_rowcount == 0;
}

int vm_get_sql_code(VM* vm) {
    if (vm == NULL) return 0;
    return vm->sql_code;
}

const char* vm_get_sql_errm(VM* vm) {
    if (vm == NULL) return "";
    return vm->sql_errm;
}

void vm_dbms_output_enable(VM* vm, int limit) {
    if (vm == NULL) return;
    vm->dbms_output_enabled = 1;
    vm->dbms_output_limit = limit;
    if (vm->dbms_output_buffer != NULL) {
        array_free(vm->dbms_output_buffer);
    }
    vm->dbms_output_buffer = array_new();
}

void vm_dbms_output_put_line(VM* vm, Value line) {
    if (vm == NULL || !vm->dbms_output_enabled) return;
    if (vm->dbms_output_buffer == NULL) {
        vm->dbms_output_buffer = array_new();
    }
    if (vm->dbms_output_limit > 0 && array_length(vm->dbms_output_buffer) >= vm->dbms_output_limit) {
        set_runtime_error(vm, "dbms_output buffer full");
        return;
    }
    array_append(vm->dbms_output_buffer, line);
}

void vm_dbms_output_disable(VM* vm) {
    if (vm == NULL) return;
    vm->dbms_output_enabled = 0;
    if (vm->dbms_output_buffer != NULL) {
        array_free(vm->dbms_output_buffer);
        vm->dbms_output_buffer = NULL;
    }
}

Value vm_dbms_output_get_lines(VM* vm) {
    if (vm == NULL || vm->dbms_output_buffer == NULL) {
        return value_array(array_new());
    }
    ArrayObj* result = array_new();
    int n = array_length(vm->dbms_output_buffer);
    for (int i = 0; i < n; i++) {
        array_append(result, array_get(vm->dbms_output_buffer, i));
    }
    array_free(vm->dbms_output_buffer);
    vm->dbms_output_buffer = array_new();
    return value_array(result);
}

int vm_utl_file_fopen(VM* vm, const char* path, const char* mode) {
    if (vm == NULL || path == NULL || mode == NULL) return -1;
    int slot = -1;
    for (int i = 0; i < UTL_FILE_MAX_HANDLES; i++) {
        if (vm->utl_file_handles[i] == NULL) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return -1;
    FILE* f = fopen(path, mode);
    if (f == NULL) return -1;
    vm->utl_file_handles[slot] = f;
    return slot;
}

/* Reads the next line, of any length, without its line ending. Past the
   last line it raises no_data_found (SQLCODE 100), as PL/SQL's UTL_FILE
   does, so a reader loop can end on it; a bad handle or a read error is an
   ordinary error. Returns 0 with the error set, 1 with *out set. */
int vm_utl_file_get_line(VM* vm, int handle, Value* out) {
    if (vm == NULL) return 0;
    if (handle < 0 || handle >= UTL_FILE_MAX_HANDLES || vm->utl_file_handles[handle] == NULL) {
        set_runtime_error(vm, "utl_file.get_line: invalid file handle");
        return 0;
    }
    FILE* f = vm->utl_file_handles[handle];
    char* line = NULL;
    size_t capacity = 0;
    long len = os_getline(&line, &capacity, f);
    if (len < 0) {
        int failed = ferror(f);
        free(line);
        if (failed) {
            clearerr(f);
            set_runtime_error(vm, "utl_file.get_line: read failed");
        } else {
            set_runtime_error_ex(vm, "utl_file.get_line: end of file (no_data_found)",
                                 100 /* no_data_found */);
        }
        return 0;
    }
    if (len > 0 && line[len - 1] == '\n') line[--len] = '\0';
    if (len > 0 && line[len - 1] == '\r') line[--len] = '\0';
    *out = value_string(line);
    if (out->as.as_string == NULL) {
        set_runtime_error(vm, "Out of memory");
        return 0;
    }
    return 1;
}

int vm_utl_file_put_line(VM* vm, int handle, const char* text) {
    if (vm == NULL || handle < 0 || handle >= UTL_FILE_MAX_HANDLES || vm->utl_file_handles[handle] == NULL || text == NULL) {
        return 0;
    }
    FILE* f = vm->utl_file_handles[handle];
    if (fputs(text, f) == EOF || fputc('\n', f) == EOF) {
        return 0;
    }
    return 1;
}

int vm_utl_file_fseek(VM* vm, int handle, int offset) {
    if (vm == NULL || handle < 0 || handle >= UTL_FILE_MAX_HANDLES ||
        vm->utl_file_handles[handle] == NULL) {
        return -1;
    }
    return fseek(vm->utl_file_handles[handle], offset, SEEK_SET);
}

int vm_utl_file_fflush(VM* vm, int handle) {
    if (vm == NULL || handle < 0 || handle >= UTL_FILE_MAX_HANDLES ||
        vm->utl_file_handles[handle] == NULL) {
        return 0;
    }
    return fflush(vm->utl_file_handles[handle]) == 0;
}

int vm_utl_file_fclose(VM* vm, int handle) {
    if (vm == NULL || handle < 0 || handle >= UTL_FILE_MAX_HANDLES || vm->utl_file_handles[handle] == NULL) {
        return 0;
    }
    int ok = fclose(vm->utl_file_handles[handle]) == 0;
    vm->utl_file_handles[handle] = NULL;
    return ok;
}

int vm_utl_file_mkdir(const char* path) {
    if (path == NULL) return -1;
    return os_mkdir(path) ? 0 : -1;
}

int vm_utl_file_remove(const char* path) {
    if (path == NULL) return -1;
    return os_remove(path);
}

int vm_dbms_sql_execute(VM* vm, const char* sql) {
    return vm_dynamic_exec(vm, sql);
}

Value vm_dbms_sql_query(VM* vm, const char* sql) {
    if (vm == NULL || sql == NULL) return value_array(array_new());
    ArrayObj* result = array_new();
    DBDriver* driver = vm_get_driver(vm);

    if (driver != NULL) {
        void* handle = NULL;
        if (!driver->query(driver, sql, NULL, 0, &handle)) {
            return value_array(result);
        }
        int col_count = driver->result_column_count(driver, handle);
        void* row_handle = NULL;
        while (driver->result_next(driver, handle, &row_handle)) {
            RowObj* row = row_obj_new(col_count);
            if (row == NULL) break;
            for (int c = 0; c < col_count; c++) {
                Value cell;
                if (!driver->row_get_column(driver, row_handle, c, &cell)) {
                    cell = value_int(0);
                }
                const char* col_name = driver->result_column_name(driver, handle, c);
                row_obj_set_column(row, c, col_name, cell);
                value_release(cell);
            }
            array_append(result, value_row(row));
            value_release(value_row(row));
        }
        driver->result_free(driver, handle);
    } else {
        Context* ctx = vm->context;
        if (ctx == NULL || ctx->pager == NULL) {
            return value_array(result);
        }
        Result* res = sql_exec(sql, ctx);
        if (res == NULL) {
            return value_array(result);
        }
        Row* row = NULL;
        while ((row = result_next(res)) != NULL) {
            int col_count = row->field_count;
            RowObj* row_obj = row_obj_new(col_count);
            if (row_obj == NULL) break;
            for (int c = 0; c < col_count; c++) {
                Value cell = value_from_cell(row->fields[c].value, value_int(0));
                row_obj_set_column(row_obj, c, row->fields[c].name, cell);
                value_release(cell);
            }
            array_append(result, value_row(row_obj));
            value_release(value_row(row_obj));
        }
        result_free(res);
    }
    return value_array(result);
}

/* --------------------------------------------------------------------------
 * dbms_sql cursor API (Phase 12 Task 4)
 * ------------------------------------------------------------------------ */

static void dbms_sql_cursor_reset(DBMS_SQL_Cursor* cursor) {
    if (cursor == NULL) return;
    if (cursor->sql != NULL) {
        free(cursor->sql);
        cursor->sql = NULL;
    }
    for (int i = 0; i < cursor->bind_count; i++) {
        value_release(cursor->binds[i]);
    }
    cursor->bind_count = 0;
    cursor->result_handle = NULL;
    cursor->row_handle = NULL;
    cursor->row_count = 0;
    cursor->current_row = -1;
    cursor->is_select = 0;
    cursor->col_count = 0;
}

static int dbms_sql_cursor_close_internal(VM* vm, DBMS_SQL_Cursor* cursor) {
    if (cursor == NULL) return 0;
    if (cursor->result_handle != NULL) {
        if (vm->driver != NULL) {
            vm->driver->result_free(vm->driver, cursor->result_handle);
        } else if (vm->context != NULL) {
            result_free((Result*)cursor->result_handle);
        }
    }
    dbms_sql_cursor_reset(cursor);
    cursor->active = 0;
    return 1;
}

static int dbms_sql_is_select(const char* sql) {
    if (sql == NULL) return 0;
    const char* p = sql;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return strncasecmp(p, "select", 6) == 0 &&
           (p[6] == '\0' || p[6] == ' ' || p[6] == '\t' || p[6] == '\n' || p[6] == '\r');
}

static int dbms_sql_format_value(Value v, char* out, size_t out_size) {
    if (out == NULL || out_size == 0) return 0;
    switch (v.type) {
        case VAL_NULL:
            snprintf(out, out_size, "NULL");
            return 1;
        case VAL_INT:
            snprintf(out, out_size, "%d", v.as.as_int);
            return 1;
        case VAL_FLOAT:
            snprintf(out, out_size, "%g", v.as.as_float);
            return 1;
        case VAL_BOOL:
            snprintf(out, out_size, "%d", v.as.as_int ? 1 : 0);
            return 1;
        case VAL_STRING: {
            const char* s = v.as.as_string != NULL ? v.as.as_string : "";
            size_t needed = 3; /* quotes + NUL */
            for (const char* q = s; *q != '\0'; q++) {
                needed += (*q == '\'') ? 2 : 1;
            }
            if (needed > out_size) return 0;
            char* dst = out;
            *dst++ = '\'';
            for (const char* q = s; *q != '\0'; q++) {
                if (*q == '\'') {
                    *dst++ = '\'';
                    *dst++ = '\'';
                } else {
                    *dst++ = *q;
                }
            }
            *dst++ = '\'';
            *dst = '\0';
            return 1;
        }
        default:
            snprintf(out, out_size, "NULL");
            return 1;
    }
}

static char* dbms_sql_substitute_binds(VM* vm, const char* sql, Value* binds, int bind_count) {
    (void)vm;
    if (sql == NULL) return NULL;
    size_t sql_len = strlen(sql);
    size_t capacity = sql_len + 1;
    char* out = malloc(capacity);
    if (out == NULL) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < sql_len; ) {
        if (sql[i] == '?') {
            size_t k = i + 1;
            int pos = 0;
            while (k < sql_len && sql[k] >= '0' && sql[k] <= '9') {
                pos = pos * 10 + (sql[k] - '0');
                k++;
            }
            if (k > i + 1 && pos > 0 && pos <= bind_count) {
                char buf[1024];
                if (!dbms_sql_format_value(binds[pos - 1], buf, sizeof(buf))) {
                    buf[0] = '\0';
                }
                size_t blen = strlen(buf);
                size_t needed = j + blen + (sql_len - k) + 1;
                if (needed > capacity) {
                    capacity = needed * 2;
                    char* grown = realloc(out, capacity);
                    if (grown == NULL) {
                        free(out);
                        return NULL;
                    }
                    out = grown;
                }
                memcpy(out + j, buf, blen);
                j += blen;
                i = k;
                continue;
            }
        }
        if (j + 2 > capacity) {
            capacity = capacity * 2 + 16;
            char* grown = realloc(out, capacity);
            if (grown == NULL) {
                free(out);
                return NULL;
            }
            out = grown;
        }
        out[j++] = sql[i++];
    }
    out[j] = '\0';
    return out;
}

int vm_dbms_sql_open_cursor(VM* vm) {
    if (vm == NULL) return -1;
    for (int i = 0; i < DBMS_SQL_MAX_CURSORS; i++) {
        if (!vm->dbms_sql_cursors[i].active) {
            dbms_sql_cursor_reset(&vm->dbms_sql_cursors[i]);
            vm->dbms_sql_cursors[i].active = 1;
            return i;
        }
    }
    return -1;
}

int vm_dbms_sql_parse(VM* vm, int handle, const char* sql) {
    if (vm == NULL || sql == NULL) return 0;
    if (handle < 0 || handle >= DBMS_SQL_MAX_CURSORS || !vm->dbms_sql_cursors[handle].active) {
        vm_set_error(vm, "dbms_sql.parse: invalid cursor handle");
        return 0;
    }
    DBMS_SQL_Cursor* cursor = &vm->dbms_sql_cursors[handle];
    dbms_sql_cursor_reset(cursor);
    cursor->sql = strdup(sql);
    if (cursor->sql == NULL) {
        vm_set_error(vm, "Out of memory");
        cursor->active = 0;
        return 0;
    }
    cursor->active = 1;
    return 1;
}

int vm_dbms_sql_bind_variable(VM* vm, int handle, const char* name, Value value) {
    if (vm == NULL || name == NULL) return 0;
    if (handle < 0 || handle >= DBMS_SQL_MAX_CURSORS || !vm->dbms_sql_cursors[handle].active) {
        vm_set_error(vm, "dbms_sql.bind_variable: invalid cursor handle");
        return 0;
    }
    DBMS_SQL_Cursor* cursor = &vm->dbms_sql_cursors[handle];
    if (cursor->sql == NULL) {
        vm_set_error(vm, "dbms_sql.bind_variable: cursor has no parsed SQL");
        return 0;
    }
    int pos = 0;
    if (name[0] == '\0' || (name[0] == '0' && name[1] == '\0')) {
        vm_set_error(vm, "dbms_sql.bind_variable: invalid bind name");
        return 0;
    }
    for (const char* p = name; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            vm_set_error(vm, "dbms_sql.bind_variable: bind name must be a positional number");
            return 0;
        }
        pos = pos * 10 + (*p - '0');
    }
    if (pos < 1 || pos > DBMS_SQL_MAX_BINDS) {
        vm_set_error(vm, "dbms_sql.bind_variable: bind position out of range");
        return 0;
    }
    if (pos > cursor->bind_count) {
        for (int i = cursor->bind_count; i < pos; i++) {
            cursor->binds[i] = value_null();
        }
        cursor->bind_count = pos;
    }
    value_release(cursor->binds[pos - 1]);
    value_retain(value);
    cursor->binds[pos - 1] = value;
    return 1;
}

int vm_dbms_sql_cursor_execute(VM* vm, int handle) {
    if (vm == NULL) return -1;
    if (handle < 0 || handle >= DBMS_SQL_MAX_CURSORS || !vm->dbms_sql_cursors[handle].active) {
        vm_set_error(vm, "dbms_sql.execute: invalid cursor handle");
        return -1;
    }
    DBMS_SQL_Cursor* cursor = &vm->dbms_sql_cursors[handle];
    if (cursor->sql == NULL) {
        vm_set_error(vm, "dbms_sql.execute: cursor has no parsed SQL");
        return -1;
    }
    char* sql = dbms_sql_substitute_binds(vm, cursor->sql, cursor->binds, cursor->bind_count);
    if (sql == NULL) {
        vm_set_error(vm, "Out of memory");
        return -1;
    }
    cursor->is_select = dbms_sql_is_select(sql);
    if (cursor->result_handle != NULL) {
        if (vm->driver != NULL) {
            vm->driver->result_free(vm->driver, cursor->result_handle);
        } else if (vm->context != NULL) {
            result_free((Result*)cursor->result_handle);
        }
        cursor->result_handle = NULL;
    }
    cursor->row_handle = NULL;
    cursor->row_count = 0;
    cursor->current_row = -1;
    cursor->col_count = 0;

    if (cursor->is_select) {
        DBDriver* driver = vm->driver;
        if (driver != NULL) {
            void* handle = NULL;
            if (!driver->query(driver, sql, NULL, 0, &handle)) {
                free(sql);
                set_runtime_error_from_driver_sql(vm, "dbms_sql.execute query failed");
                return -1;
            }
            cursor->result_handle = handle;
            cursor->col_count = driver->result_column_count(driver, handle);
        } else {
            Context* ctx = vm->context;
            if (ctx == NULL || ctx->pager == NULL) {
                free(sql);
                vm_set_error(vm, "dbms_sql.execute: no database context");
                return -1;
            }
            Result* res = sql_exec(sql, ctx);
            if (res == NULL) {
                free(sql);
                vm_set_error(vm, "dbms_sql.execute: SQL query failed");
                return -1;
            }
            cursor->result_handle = res;
            cursor->col_count = res->rows != NULL && res->row_count > 0 ? res->rows[0].field_count : 0;
        }
        free(sql);
        vm_set_sql_rowcount(vm, 0);
        return 0;
    }

    int row_count = vm_dynamic_exec(vm, sql);
    free(sql);
    if (row_count < 0) return -1;
    vm_set_sql_rowcount(vm, row_count);
    return row_count;
}

Value vm_dbms_sql_fetch_rows(VM* vm, int handle, int count) {
    if (vm == NULL || count < 0) return value_array(array_new());
    if (handle < 0 || handle >= DBMS_SQL_MAX_CURSORS || !vm->dbms_sql_cursors[handle].active) {
        vm_set_error(vm, "dbms_sql.fetch_rows: invalid cursor handle");
        return value_array(array_new());
    }
    DBMS_SQL_Cursor* cursor = &vm->dbms_sql_cursors[handle];
    if (!cursor->is_select || cursor->result_handle == NULL) {
        vm_set_error(vm, "dbms_sql.fetch_rows: cursor is not a SELECT");
        return value_array(array_new());
    }
    ArrayObj* result = array_new();
    DBDriver* driver = vm->driver;
    int fetched = 0;
    if (driver != NULL) {
        void* row_handle = NULL;
        while (fetched < count && driver->result_next(driver, cursor->result_handle, &row_handle)) {
            cursor->row_handle = row_handle;
            RowObj* row = row_obj_new(cursor->col_count);
            if (row == NULL) break;
            for (int c = 0; c < cursor->col_count; c++) {
                Value cell;
                if (!driver->row_get_column(driver, row_handle, c, &cell)) {
                    cell = value_int(0);
                }
                const char* col_name = driver->result_column_name(driver, cursor->result_handle, c);
                row_obj_set_column(row, c, col_name, cell);
                value_release(cell);
            }
            array_append(result, value_row(row));
            value_release(value_row(row));
            fetched++;
            cursor->current_row++;
        }
    } else {
        Result* res = (Result*)cursor->result_handle;
        Row* row = NULL;
        while (fetched < count && (row = result_next(res)) != NULL) {
            cursor->row_handle = row;
            int col_count = row->field_count;
            RowObj* row_obj = row_obj_new(col_count);
            if (row_obj == NULL) break;
            for (int c = 0; c < col_count; c++) {
                Value cell = value_from_cell(row->fields[c].value, value_int(0));
                row_obj_set_column(row_obj, c, row->fields[c].name, cell);
                value_release(cell);
            }
            array_append(result, value_row(row_obj));
            value_release(value_row(row_obj));
            fetched++;
            cursor->current_row++;
        }
    }
    if (fetched == 0) {
        cursor->row_handle = NULL;
    }
    return value_array(result);
}

Value vm_dbms_sql_column_value(VM* vm, int handle, int column) {
    if (vm == NULL) return value_null();
    if (handle < 0 || handle >= DBMS_SQL_MAX_CURSORS || !vm->dbms_sql_cursors[handle].active) {
        vm_set_error(vm, "dbms_sql.column_value: invalid cursor handle");
        return value_null();
    }
    DBMS_SQL_Cursor* cursor = &vm->dbms_sql_cursors[handle];
    if (!cursor->is_select || cursor->result_handle == NULL) {
        vm_set_error(vm, "dbms_sql.column_value: cursor is not a SELECT");
        return value_null();
    }
    if (cursor->row_handle == NULL || column < 0 || column >= cursor->col_count) {
        vm_set_error(vm, "dbms_sql.column_value: no current row or invalid column");
        return value_null();
    }
    DBDriver* driver = vm->driver;
    if (driver != NULL) {
        Value cell;
        if (!driver->row_get_column(driver, cursor->row_handle, column, &cell)) {
            return value_null();
        }
        return cell;
    } else {
        Row* row = (Row*)cursor->row_handle;
        if (column >= row->field_count) {
            return value_null();
        }
        return value_from_cell(row->fields[column].value, value_null());
    }
}

int vm_dbms_sql_close_cursor(VM* vm, int handle) {
    if (vm == NULL) return 0;
    if (handle < 0 || handle >= DBMS_SQL_MAX_CURSORS || !vm->dbms_sql_cursors[handle].active) {
        vm_set_error(vm, "dbms_sql.close_cursor: invalid cursor handle");
        return 0;
    }
    return dbms_sql_cursor_close_internal(vm, &vm->dbms_sql_cursors[handle]);
}

static SequenceSlot* sequence_find(VM* vm, const char* name) {
    for (int i = 0; i < SEQUENCE_MAX; i++) {
        if (vm->sequences[i].used && strcmp(vm->sequences[i].name, name) == 0) {
            return &vm->sequences[i];
        }
    }
    return NULL;
}

/* Sequences persist through the active storage backend: the custom engine
   keeps them in the V5 catalog page, the SQLite driver in the
   _auspex_sequences table. The VM slots are a cache, populated lazily on the
   first sequence op after a driver/context is attached, and every mutation
   is written through immediately so a later process resumes where the
   previous one stopped. */
static void sequence_ensure_loaded(VM* vm) {
    if (vm->sequences_loaded) return;
    vm->sequences_loaded = 1;
    DBSequence stored[SEQUENCE_MAX];
    int count = 0;
    if (vm->driver != NULL && vm->driver->sequence_load != NULL) {
        if (!vm->driver->sequence_load(vm->driver, stored, SEQUENCE_MAX, &count)) {
            return;
        }
    } else if (vm->driver == NULL && vm->context != NULL) {
        count = catalog_sequence_list(vm->context, stored, SEQUENCE_MAX);
    } else {
        return;
    }
    for (int i = 0; i < count && i < SEQUENCE_MAX; i++) {
        SequenceSlot* slot = &vm->sequences[i];
        slot->used = 1;
        slot->has_value = stored[i].has_value;
        slot->current = stored[i].current;
        slot->increment = stored[i].increment;
        snprintf(slot->name, sizeof(slot->name), "%s", stored[i].name);
    }
}

static int sequence_persist_save(VM* vm, const SequenceSlot* slot) {
    DBSequence seq;
    memset(&seq, 0, sizeof(seq));
    snprintf(seq.name, sizeof(seq.name), "%s", slot->name);
    seq.has_value = slot->has_value;
    seq.current = slot->current;
    seq.increment = slot->increment;
    if (vm->driver != NULL && vm->driver->sequence_save != NULL) {
        return vm->driver->sequence_save(vm->driver, &seq);
    }
    if (vm->driver == NULL && vm->context != NULL) {
        return catalog_sequence_save(vm->context, &seq);
    }
    return 1; /* no storage backend: session-only */
}

static int sequence_persist_drop(VM* vm, const char* name) {
    if (vm->driver != NULL && vm->driver->sequence_drop != NULL) {
        return vm->driver->sequence_drop(vm->driver, name);
    }
    if (vm->driver == NULL && vm->context != NULL) {
        return catalog_sequence_drop(vm->context, name);
    }
    return 1;
}

int vm_sequence_create(VM* vm, const char* name, int start, int increment) {
    if (vm == NULL || name == NULL || name[0] == '\0') return 0;
    sequence_ensure_loaded(vm);
    if (sequence_find(vm, name) != NULL) return 0;
    for (int i = 0; i < SEQUENCE_MAX; i++) {
        if (!vm->sequences[i].used) {
            SequenceSlot* slot = &vm->sequences[i];
            slot->used = 1;
            slot->has_value = 0;
            slot->current = start;
            slot->increment = increment;
            snprintf(slot->name, sizeof(slot->name), "%s", name);
            if (!sequence_persist_save(vm, slot)) {
                slot->used = 0;
                slot->name[0] = '\0';
                return 0;
            }
            return 1;
        }
    }
    return 0;
}

int vm_sequence_nextval(VM* vm, const char* name, int* out) {
    if (vm == NULL || name == NULL || out == NULL) return 0;
    sequence_ensure_loaded(vm);
    SequenceSlot* slot = sequence_find(vm, name);
    if (slot == NULL) return 0;
    int old_has_value = slot->has_value;
    int old_current = slot->current;
    if (slot->has_value) {
        slot->current += slot->increment;
    } else {
        slot->has_value = 1;
    }
    if (!sequence_persist_save(vm, slot)) {
        slot->has_value = old_has_value;
        slot->current = old_current;
        return 0;
    }
    *out = slot->current;
    return 1;
}

int vm_sequence_currval(VM* vm, const char* name, int* out) {
    if (vm == NULL || name == NULL || out == NULL) return 0;
    sequence_ensure_loaded(vm);
    SequenceSlot* slot = sequence_find(vm, name);
    if (slot == NULL || !slot->has_value) return 0;
    *out = slot->current;
    return 1;
}

int vm_sequence_drop(VM* vm, const char* name) {
    if (vm == NULL || name == NULL) return 0;
    sequence_ensure_loaded(vm);
    SequenceSlot* slot = sequence_find(vm, name);
    if (slot == NULL) return 0;
    if (!sequence_persist_drop(vm, name)) return 0;
    slot->used = 0;
    slot->has_value = 0;
    slot->name[0] = '\0';
    return 1;
}

static int push(VM* vm, Value value);

static int vm_catch(VM* vm) {
    if (vm->try_count == 0) return 0;
    TryFrame* tf = &vm->try_frames[--vm->try_count];
    /* Preserve locals that existed before the try block (slots below catch_var).
     * local_count includes the catch variable slot, so preserve_top points at
     * the slot where the error message will live. */
    Value* preserve_top = tf->frame_base + (tf->local_count - 1);
    for (Value* p = vm->stack_top - 1; p >= preserve_top; p--) {
        value_release(*p);
    }
    vm->stack_top = preserve_top;
    vm->frame_count = tf->frame_count;
    vm->frame_base = tf->frame_base;
    const char* msg = vm->error_message[0] != '\0' ? vm->error_message : "Runtime error";
    char* copy = strdup(msg);
    if (copy == NULL) {
        set_runtime_error(vm, "Out of memory");
        return 0;
    }
    Value msg_value = value_string(copy);
    if (!push(vm, msg_value)) {
        value_release(msg_value);
        return 0;
    }
    vm->ip = tf->catch_ip;
    vm->error_message[0] = '\0';
    return 1;
}

#define THROW(vm) do { \
    if (!vm_catch(vm)) return INTERPRET_RUNTIME_ERROR; \
    goto dispatch; \
} while (0)

void vm_free(VM* vm) {
    if (vm == NULL) return;
    for (Value* p = vm->stack; p < vm->stack_top; p++) {
        value_release(*p);
    }
    for (int i = 0; i < vm->repl_local_count; i++) {
        value_release(vm->repl_locals[i]);
    }
    for (int i = 0; i < vm->global_count; i++) {
        value_release(vm->globals[i]);
    }
    if (vm->driver != NULL && vm->result_handle != NULL) {
        vm->driver->result_free(vm->driver, vm->result_handle);
    } else if (vm->driver == NULL) {
        result_free((Result*)vm->result_handle);
    }
    if (vm->dbms_output_buffer != NULL) {
        array_free(vm->dbms_output_buffer);
    }
    for (int i = 0; i < UTL_FILE_MAX_HANDLES; i++) {
        if (vm->utl_file_handles[i] != NULL) {
            fclose(vm->utl_file_handles[i]);
            vm->utl_file_handles[i] = NULL;
        }
    }
    for (int i = 0; i < DBMS_SQL_MAX_CURSORS; i++) {
        if (vm->dbms_sql_cursors[i].active) {
            dbms_sql_cursor_close_internal(vm, &vm->dbms_sql_cursors[i]);
        }
    }
    /* All roots are released: sweep every remaining registered container.
     * This frees cyclic garbage exactly (the sweep breaks internal edges
     * before freeing) so leak-checking teardown runs stay clean. */
    gc_sweep_unreachable();
    free(vm->stack);
    free(vm->frames);
    free(vm->return_ips);
    free(vm->out_frames);
    free(vm->repl_locals);
    free(vm);
}

static int vm_ensure_stack_capacity(VM* vm, int additional) {
    int used = (int)(vm->stack_top - vm->stack);
    if (used + additional <= vm->stack_capacity) return 1;
    int new_capacity = vm->stack_capacity;
    while (new_capacity < used + additional) {
        if (new_capacity >= STACK_MAX) return 0;
        new_capacity *= 2;
        if (new_capacity > STACK_MAX) new_capacity = STACK_MAX;
    }
    Value* old_stack = vm->stack;
    Value* new_stack = realloc(vm->stack, sizeof(Value) * (size_t)new_capacity);
    if (new_stack == NULL) return 0;
    ptrdiff_t delta = new_stack - old_stack;
    vm->stack = new_stack;
    vm->stack_capacity = new_capacity;
    if (delta != 0) {
        /* Every pointer into the old array follows the block. */
        vm->stack_top += delta;
        vm->frame_base += delta;
        for (int i = 0; i < vm->frame_count; i++) {
            vm->frames[i] += delta;
        }
        for (int i = 0; i < vm->try_count; i++) {
            vm->try_frames[i].frame_base += delta;
            vm->try_frames[i].stack_top += delta;
        }
    }
    return 1;
}

/* Grow the frames/return_ips/out_frames arrays together (they are indexed
   by the same frame_count). Hard-capped at STACK_MAX frames so runaway
   recursion fails cleanly instead of exhausting memory. */
static int vm_ensure_frame_capacity(VM* vm, int needed) {
    if (needed <= vm->frames_capacity) return 1;
    if (vm->frames_capacity >= STACK_MAX) return 0;
    int new_capacity = vm->frames_capacity * 2;
    if (new_capacity > STACK_MAX) new_capacity = STACK_MAX;
    if (new_capacity < needed) return 0;
    Value** new_frames = realloc(vm->frames, sizeof(Value*) * (size_t)new_capacity);
    if (new_frames == NULL) return 0;
    vm->frames = new_frames;
    uint8_t** new_return_ips = realloc(vm->return_ips, sizeof(uint8_t*) * (size_t)new_capacity);
    if (new_return_ips == NULL) return 0;
    vm->return_ips = new_return_ips;
    OutParamFrame* new_out_frames = realloc(vm->out_frames, sizeof(OutParamFrame) * (size_t)new_capacity);
    if (new_out_frames == NULL) return 0;
    vm->out_frames = new_out_frames;
    vm->frames_capacity = new_capacity;
    return 1;
}

static int vm_ensure_repl_capacity(VM* vm, int needed) {
    if (needed <= vm->repl_local_capacity) return 1;
    int new_capacity = vm->repl_local_capacity;
    while (new_capacity < needed) {
        if (new_capacity >= STACK_MAX) return 0;
        new_capacity *= 2;
        if (new_capacity > STACK_MAX) new_capacity = STACK_MAX;
    }
    Value* new_locals = realloc(vm->repl_locals, sizeof(Value) * (size_t)new_capacity);
    if (new_locals == NULL) return 0;
    vm->repl_locals = new_locals;
    vm->repl_local_capacity = new_capacity;
    return 1;
}

static int push(VM* vm, Value value) {
    if (!vm_ensure_stack_capacity(vm, 1)) {
        return 0;
    }
    *vm->stack_top = value;
    vm->stack_top++;
    return 1;
}

static int pop(VM* vm, Value* out) {
    if (vm->stack_top <= vm->stack) {
        return 0;
    }
    vm->stack_top--;
    *out = *vm->stack_top;
    return 1;
}

static int binary_op(VM* vm, Value (*op)(Value, Value)) {
    Value b;
    Value a;
    if (!pop(vm, &b)) return 0;
    if (!pop(vm, &a)) return 0;
    Value r = op(a, b);
    int ok = push(vm, r);
    value_release(a);
    value_release(b);
    return ok;
}

Value vm_pop(VM* vm) {
    Value value;
    pop(vm, &value);
    return value;
}

int vm_stack_depth(VM* vm) {
    if (vm == NULL) return 0;
    return (int)(vm->stack_top - vm->stack);
}

Value vm_stack_get(VM* vm, int index) {
    if (vm == NULL || index < 0 || index >= vm_stack_depth(vm)) {
        return value_int(0);
    }
    return vm->stack[index];
}

int vm_local_count(VM* vm) {
    if (vm == NULL) return 0;
    return vm->repl_local_count;
}

Value vm_local_get(VM* vm, int index) {
    if (vm == NULL || index < 0 || index >= vm->repl_local_count) {
        return value_int(0);
    }
    return vm->repl_locals[index];
}

void vm_set_context(VM* vm, struct Context* ctx) {
    if (vm == NULL) return;
    vm->context = ctx;
    /* Attaching a storage backend re-syncs sequences from it: drop the
       cached slots so the next sequence op lazily reloads them. */
    for (int i = 0; i < SEQUENCE_MAX; i++) {
        vm->sequences[i].used = 0;
        vm->sequences[i].name[0] = '\0';
    }
    vm->sequences_loaded = 0;
}

void vm_set_driver(VM* vm, DBDriver* driver) {
    if (vm == NULL) return;
    vm->driver = driver;
    for (int i = 0; i < SEQUENCE_MAX; i++) {
        vm->sequences[i].used = 0;
        vm->sequences[i].name[0] = '\0';
    }
    vm->sequences_loaded = 0;
}

static int vm_call_autonomous(VM* parent, uint16_t target, uint8_t arg_count,
                              int out_count, int* out_positions, int* out_slots,
                              Value* out_result);

static void vm_free_child(VM* vm);
static InterpretResult vm_run(VM* vm, uint8_t* end);

/* --------------------------------------------------------------------------
 * Cycle collector driver (Phase 13 #38)
 *
 * The interpreter loop checks an allocation counter at every instruction
 * boundary (only in the top-level VM; child VMs never collect) and runs a
 * mark/sweep when it crosses an adaptive threshold. The threshold floor is
 * GC_MIN_INTERVAL container allocations; after each collection it doubles
 * the live-container count so huge live sets are not re-traced constantly.
 * Collection runs only between instructions, where the VM holds no raw
 * pointers into container interiors.
 * ------------------------------------------------------------------------ */

#define GC_MIN_INTERVAL 1024

static long g_gc_next_collection = GC_MIN_INTERVAL;

static void vm_gc_collect(VM* vm) {
    if (!gc_begin_collection()) {
        /* Cannot pre-grow the trace stack: skip this cycle entirely. */
        gc_reset_container_allocations();
        return;
    }
    for (Value* p = vm->stack; p < vm->stack_top; p++) gc_trace_value(*p);
    for (int i = 0; i < vm->repl_local_count; i++) gc_trace_value(vm->repl_locals[i]);
    for (int i = 0; i < vm->global_count; i++) gc_trace_value(vm->globals[i]);
    if (vm->chunk != NULL) {
        for (int i = 0; i < vm->chunk->constants_count; i++)
            gc_trace_value(vm->chunk->constants[i]);
    }
    for (int i = 0; i < vm->sql_param_count; i++) gc_trace_value(vm->sql_params[i]);
    if (vm->dbms_output_buffer != NULL) gc_trace_value(value_array(vm->dbms_output_buffer));
    for (int i = 0; i < DBMS_SQL_MAX_CURSORS; i++) {
        DBMS_SQL_Cursor* cursor = &vm->dbms_sql_cursors[i];
        if (cursor->active) {
            for (int j = 0; j < cursor->bind_count; j++) gc_trace_value(cursor->binds[j]);
        }
    }
    gc_sweep_unreachable();
    gc_reset_container_allocations();
    long live = gc_container_count();
    long next = live * 2;
    if (next < GC_MIN_INTERVAL) next = GC_MIN_INTERVAL;
    g_gc_next_collection = next;
}

/* --------------------------------------------------------------------------
 * Runtime trigger firing (dynamic SQL)
 *
 * Static SQL statements fire triggers via calls emitted at compile time
 * (codegen.c emit_trigger_calls). Dynamic SQL built at runtime —
 * execute_immediate(...) and dbms_sql.execute(...) — is sniffed here instead:
 * matching triggers from the chunk's runtime registry run in a child VM that
 * shares the chunk, driver, and globals (like vm_call_autonomous, but on the
 * same connection/transaction).
 * ------------------------------------------------------------------------ */

/* --------------------------------------------------------------------------
 * Trigger self-modification guard (#59)
 *
 * A trigger must not modify the table it fires on, directly or through a
 * proc, dynamic SQL, or another table's trigger: the custom engine would
 * rewrite the row chain it is iterating, and any driver would recurse. Each
 * call frame records the proc it entered, and a child VM its creator and
 * start offset, so the running triggers are exactly the frames, in this VM
 * and every VM above it, that entered a trigger proc.
 * ------------------------------------------------------------------------ */

static const ChunkTrigger* vm_trigger_at(const Chunk* chunk, int offset) {
    if (chunk == NULL || offset < 0) return NULL;
    for (int i = 0; i < chunk->trigger_count; i++) {
        if (chunk->triggers[i].offset == offset) return &chunk->triggers[i];
    }
    return NULL;
}

/* Returns 0 with a runtime error set when a trigger on `table` is running. */
static int vm_check_table_not_firing(VM* vm, const char* table) {
    for (VM* v = vm; v != NULL; v = v->parent) {
        if (v->chunk == NULL || v->chunk->trigger_count == 0) continue;
        for (int i = -1; i < v->frame_count; i++) {
            int entry = i < 0 ? v->entry_offset : v->out_frames[i].entry_offset;
            const ChunkTrigger* running = vm_trigger_at(v->chunk, entry);
            if (running == NULL || !trigger_name_equals(running->table, table)) continue;
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "Cannot modify table '%s' while its trigger '%s' is running",
                     table, running->name);
            set_runtime_error(vm, msg);
            return 0;
        }
    }
    return 1;
}

/* Entering a trigger proc means its table is being modified, so it must not
   already be firing. This is where static SQL inside a trigger body is
   caught: its BEFORE triggers are called ahead of the statement itself. */
static int vm_check_trigger_entry(VM* vm, int target) {
    const ChunkTrigger* trigger = vm_trigger_at(vm->chunk, target);
    return trigger == NULL || vm_check_table_not_firing(vm, trigger->table);
}

/* Returns 0 with a runtime error set when `sql` modifies a table whose
   trigger is running. */
static int vm_check_sql_not_firing(VM* vm, const char* sql) {
    int event = -1;
    char table[64];
    if (!sql_trigger_info(sql, &event, table, sizeof(table))) return 1;
    if (event != TRIGGER_INSERT && event != TRIGGER_UPDATE &&
        event != TRIGGER_DELETE && event != TRIGGER_DROP) {
        return 1;
    }
    return vm_check_table_not_firing(vm, table);
}

static int vm_fire_trigger(VM* vm, int offset) {
    if (!vm_check_trigger_entry(vm, offset)) return 0;
    VM* child = vm_init();
    if (child == NULL) {
        set_runtime_error(vm, "Out of memory");
        return 0;
    }
    child->is_child = 1;
    child->parent = vm;
    child->entry_offset = offset;
    child->chunk = vm->chunk;
    child->ip = child->chunk->code + offset;
    child->frame_base = child->stack;
    child->frame_count = 0;
    child->driver = vm->driver;
    child->context = vm->context;
    child->global_count = vm->global_count;
    for (int i = 0; i < vm->global_count; i++) {
        child->globals[i] = vm->globals[i];
        value_retain(child->globals[i]);
    }
    InterpretResult result = vm_run(child, child->chunk->code + child->chunk->count);
    if (result != INTERPRET_OK) {
        snprintf(vm->error_message, sizeof(vm->error_message), "%s", child->error_message);
        vm_free_child(child);
        return 0;
    }
    Value value;
    if (pop(child, &value)) {
        value_release(value);
    }
    vm_free_child(child);
    return 1;
}

static int vm_fire_triggers(VM* vm, int timing, int event, const char* table) {
    Chunk* chunk = vm->chunk;
    if (chunk == NULL) return 1;
    for (int i = 0; i < chunk->trigger_count; i++) {
        ChunkTrigger* trigger = &chunk->triggers[i];
        if (trigger->offset < 0) continue;
        /* Row-level triggers fire per row through the driver hook, not here. */
        if (trigger->for_each_row) continue;
        if (trigger->timing != timing || trigger->event != event) continue;
        if (!trigger_name_equals(trigger->table, table)) continue;
        if (!vm_fire_trigger(vm, trigger->offset)) return 0;
    }
    return 1;
}

/* Disable a trigger in the runtime registry and remove its persisted
   definition so it stays dropped after a restart. */
static void vm_drop_trigger(VM* vm, const char* name) {
    if (vm->chunk != NULL) {
        chunk_remove_trigger(vm->chunk, name);
    }
    if (vm->driver != NULL) {
        Context ctx;
        ctx.db_path = vm->driver->connection_string[0] != '\0'
                          ? vm->driver->connection_string
                          : NULL;
        ctx.pager = NULL;
        stored_programs_drop_unit(vm->driver, &ctx, name, "TRIGGER");
    } else if (vm->context != NULL) {
        stored_programs_drop_unit(NULL, vm->context, name, "TRIGGER");
    }
}

/* --------------------------------------------------------------------------
 * Row-level trigger firing (FOR EACH ROW)
 *
 * Row-level trigger bodies compile to hidden procs taking two implicit row
 * parameters: :new (local slot 0) and :old (slot 1). They never fire through
 * the statement-level call paths (emit_trigger_calls / vm_fire_triggers);
 * instead the DML drivers report each affected row through the
 * DBDriver.row_trigger_fn hook, which the VM installs around driver->exec.
 *
 * Custom engine: the engine's DML loops call the hook per row with the old
 * and new row images (BEFORE before the write, AFTER after it).
 *
 * SQLite: OP_SQL_EXEC / vm_dynamic_exec route DML with matching row triggers
 * through vm_sqlite_exec_row_triggers, which snapshots the affected rows with
 * SELECT rowid, * before the statement and re-selects after it. Both BEFORE
 * and AFTER row triggers observe the row images around the completed
 * statement; the firing order per row is preserved, but on SQLite a BEFORE
 * row trigger fires after the row has actually been written (the engine
 * cannot expose pre-write row images through the DBDriver interface).
 * ------------------------------------------------------------------------ */

static int vm_chunk_has_row_triggers(VM* vm, int event, const char* table) {
    Chunk* chunk = vm->chunk;
    if (chunk == NULL) return 0;
    for (int i = 0; i < chunk->trigger_count; i++) {
        ChunkTrigger* trigger = &chunk->triggers[i];
        if (trigger->offset < 0 || !trigger->for_each_row) continue;
        if (trigger->event != event) continue;
        if (!trigger_name_equals(trigger->table, table)) continue;
        return 1;
    }
    return 0;
}

/* Fire one row-level trigger in a child VM (like vm_fire_trigger, but with
   the :new and :old row images pushed as the two implicit arguments). */
static int vm_fire_row_trigger(VM* vm, int offset, Value new_row, Value old_row) {
    if (!vm_check_trigger_entry(vm, offset)) return 0;
    VM* child = vm_init();
    if (child == NULL) {
        set_runtime_error(vm, "Out of memory");
        return 0;
    }
    child->is_child = 1;
    child->parent = vm;
    child->entry_offset = offset;
    child->chunk = vm->chunk;
    child->driver = vm->driver;
    child->context = vm->context;
    child->global_count = vm->global_count;
    for (int i = 0; i < vm->global_count; i++) {
        child->globals[i] = vm->globals[i];
        value_retain(child->globals[i]);
    }
    value_retain(new_row);
    value_retain(old_row);
    if (!push(child, new_row) || !push(child, old_row)) {
        set_runtime_error(vm, "Stack overflow");
        vm_free_child(child);
        return 0;
    }
    child->ip = child->chunk->code + offset;
    child->frame_base = child->stack;
    child->frame_count = 0;
    InterpretResult result = vm_run(child, child->chunk->code + child->chunk->count);
    if (result != INTERPRET_OK) {
        snprintf(vm->error_message, sizeof(vm->error_message), "%s", child->error_message);
        vm_free_child(child);
        return 0;
    }
    Value value;
    if (pop(child, &value)) {
        value_release(value);
    }
    vm_free_child(child);
    return 1;
}

static int vm_fire_row_triggers(VM* vm, int timing, int event, const char* table,
                                Value new_row, Value old_row) {
    Chunk* chunk = vm->chunk;
    if (chunk == NULL) return 1;
    for (int i = 0; i < chunk->trigger_count; i++) {
        ChunkTrigger* trigger = &chunk->triggers[i];
        if (trigger->offset < 0 || !trigger->for_each_row) continue;
        if (trigger->timing != timing || trigger->event != event) continue;
        if (!trigger_name_equals(trigger->table, table)) continue;
        if (!vm_fire_row_trigger(vm, trigger->offset, new_row, old_row)) return 0;
    }
    return 1;
}

/* Driver row-trigger hook: the DML drivers call this once per affected row.
   A missing row image is passed to the trigger as VAL_NULL, so using :new in
   a DELETE trigger (or :old in an INSERT trigger) is a runtime error. */
static int vm_row_trigger_hook(void* user, int timing, int event, const char* table,
                               const Value* old_row, const Value* new_row,
                               char* error, size_t error_size) {
    VM* vm = (VM*)user;
    Value new_v = new_row != NULL ? *new_row : value_null();
    Value old_v = old_row != NULL ? *old_row : value_null();
    if (vm_fire_row_triggers(vm, timing, event, table, new_v, old_v)) return 1;
    if (error != NULL && error_size > 0) {
        snprintf(error, error_size, "%s",
                 vm->error_message[0] != '\0' ? vm->error_message : "row trigger failed");
    }
    return 0;
}

static int is_trigger_ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/* Points at the WHERE keyword of a DML statement (scanning past string
   literals), or NULL when the statement has no WHERE clause. */
static const char* sql_find_where(const char* sql) {
    char quote = '\0';
    for (const char* p = sql; *p != '\0'; p++) {
        if (quote != '\0') {
            if (*p == quote) quote = '\0';
            continue;
        }
        if (*p == '\'' || *p == '"') {
            quote = *p;
            continue;
        }
        if ((*p == 'w' || *p == 'W') &&
            (p == sql || !is_trigger_ident_char(p[-1])) &&
            strncasecmp(p, "where", 5) == 0 &&
            !is_trigger_ident_char(p[5])) {
            return p;
        }
    }
    return NULL;
}

/* A row snapshot for the SQLite row-trigger path: the rowid plus the row's
   columns materialized as a RowObj. */
typedef struct {
    int     rowid;
    RowObj* row;
} TriggerRowImage;

static void free_trigger_row_images(TriggerRowImage* images, int count) {
    for (int i = 0; i < count; i++) {
        row_obj_free(images[i].row);
    }
    free(images);
}

/* Runs `SELECT rowid, * FROM ...` through the active driver and materializes
   every result row (column 0 = rowid, remaining columns become the RowObj). */
static int vm_select_row_images(VM* vm, const char* query,
                                TriggerRowImage** out_images, int* out_count) {
    DBDriver* driver = vm->driver;
    *out_images = NULL;
    *out_count = 0;
    void* handle = NULL;
    if (!driver->query(driver, query, NULL, 0, &handle)) return 0;
    TriggerRowImage* images = NULL;
    int count = 0;
    int capacity = 0;
    int col_count = driver->result_column_count(driver, handle);
    void* row_handle = NULL;
    while (driver->result_next(driver, handle, &row_handle)) {
        if (count >= capacity) {
            capacity = capacity == 0 ? 8 : capacity * 2;
            TriggerRowImage* grown = realloc(images, sizeof(TriggerRowImage) * (size_t)capacity);
            if (grown == NULL) {
                free_trigger_row_images(images, count);
                driver->result_free(driver, handle);
                return 0;
            }
            images = grown;
        }
        Value rowid_value;
        int rowid = 0;
        if (driver->row_get_column(driver, row_handle, 0, &rowid_value) &&
            rowid_value.type == VAL_INT) {
            rowid = rowid_value.as.as_int;
        }
        RowObj* row = row_obj_new(col_count - 1);
        if (row == NULL) {
            free_trigger_row_images(images, count);
            driver->result_free(driver, handle);
            return 0;
        }
        for (int c = 1; c < col_count; c++) {
            Value cell;
            if (!driver->row_get_column(driver, row_handle, c, &cell)) {
                cell = value_null();
            }
            const char* col_name = driver->result_column_name(driver, handle, c);
            row_obj_set_column(row, c - 1, col_name, cell);
            value_release(cell);
        }
        images[count].rowid = rowid;
        images[count].row = row;
        count++;
    }
    driver->result_free(driver, handle);
    *out_images = images;
    *out_count = count;
    return 1;
}

/* Runs a query expected to produce a single int (e.g. MAX(rowid)). */
static int vm_query_scalar_int(VM* vm, const char* query, int* out) {
    DBDriver* driver = vm->driver;
    void* handle = NULL;
    if (!driver->query(driver, query, NULL, 0, &handle)) return 0;
    void* row_handle = NULL;
    int ok = 0;
    if (driver->result_next(driver, handle, &row_handle)) {
        Value v;
        if (driver->row_get_column(driver, row_handle, 0, &v)) {
            if (v.type == VAL_INT) {
                *out = v.as.as_int;
                ok = 1;
            } else if (v.type == VAL_FLOAT) {
                *out = (int)v.as.as_float;
                ok = 1;
            }
        }
    }
    driver->result_free(driver, handle);
    return ok;
}

/* Fire BEFORE then AFTER row triggers for one (old, new) row image pair and
   release both images. */
static int vm_fire_row_trigger_pair(VM* vm, int event, const char* table,
                                    RowObj* old_obj, RowObj* new_obj) {
    Value old_row = old_obj != NULL ? value_row(old_obj) : value_null();
    Value new_row = new_obj != NULL ? value_row(new_obj) : value_null();
    int ok = vm_fire_row_triggers(vm, TRIGGER_BEFORE, event, table, new_row, old_row) &&
             vm_fire_row_triggers(vm, TRIGGER_AFTER, event, table, new_row, old_row);
    value_release(new_row);
    value_release(old_row);
    if (!ok && vm->driver != NULL) {
        /* The statement's caller reports the driver's error, as it does for
           the custom engine's row-trigger hook: carry the trigger's there. */
        snprintf(vm->driver->error_message, sizeof(vm->driver->error_message), "%s",
                 vm->error_message[0] != '\0' ? vm->error_message : "row trigger failed");
    }
    return ok;
}

/* SQLite row-trigger execution: snapshot affected rows via SELECT rowid, *
   before running the DML, run it, then fire row triggers per affected row
   (re-selecting :new by rowid for UPDATE). Returns the DML's change count. */
static int vm_sqlite_exec_row_triggers(VM* vm, const char* sql, int event,
                                       const char* table, Value* params, int param_count) {
    DBDriver* driver = vm->driver;
    const char* where = sql_find_where(sql);
    if (where != NULL && param_count > 0 && strchr(where, '?') != NULL) {
        set_runtime_error(vm,
            "row-level triggers on SQLite do not support ? parameters in WHERE");
        return -1;
    }

    if (event == TRIGGER_INSERT) {
        char scalar_query[320];
        snprintf(scalar_query, sizeof(scalar_query),
                 "SELECT COALESCE(MAX(rowid), 0) FROM %s", table);
        int before_max = 0;
        if (!vm_query_scalar_int(vm, scalar_query, &before_max)) {
            set_runtime_error_from_driver_sql(vm, "row trigger pre-select failed");
            return -1;
        }
        int row_count = driver->exec(driver, sql, params, param_count);
        if (row_count < 0) return -1;
        size_t qlen = strlen(table) + 96;
        char* query = malloc(qlen);
        if (query == NULL) {
            set_runtime_error(vm, "Out of memory");
            return -1;
        }
        snprintf(query, qlen, "SELECT rowid, * FROM %s WHERE rowid > %d", table, before_max);
        TriggerRowImage* images = NULL;
        int count = 0;
        if (!vm_select_row_images(vm, query, &images, &count)) {
            free(query);
            set_runtime_error_from_driver_sql(vm, "row trigger post-select failed");
            return -1;
        }
        free(query);
        for (int i = 0; i < count; i++) {
            if (!vm_fire_row_trigger_pair(vm, event, table, NULL, images[i].row)) {
                for (int j = i + 1; j < count; j++) row_obj_free(images[j].row);
                free(images);
                return -1;
            }
        }
        free(images);
        return row_count;
    }

    /* UPDATE / DELETE: snapshot the matching rows first. */
    size_t qlen = strlen(table) + (where != NULL ? strlen(where) : 0) + 96;
    char* query = malloc(qlen);
    if (query == NULL) {
        set_runtime_error(vm, "Out of memory");
        return -1;
    }
    snprintf(query, qlen, "SELECT rowid, * FROM %s%s%s", table,
             where != NULL ? " " : "", where != NULL ? where : "");
    TriggerRowImage* olds = NULL;
    int old_count = 0;
    if (!vm_select_row_images(vm, query, &olds, &old_count)) {
        free(query);
        set_runtime_error_from_driver_sql(vm, "row trigger pre-select failed");
        return -1;
    }
    free(query);

    int row_count = driver->exec(driver, sql, params, param_count);
    if (row_count < 0) {
        free_trigger_row_images(olds, old_count);
        return -1;
    }

    for (int i = 0; i < old_count; i++) {
        RowObj* new_obj = NULL;
        if (event == TRIGGER_UPDATE) {
            char new_query[320];
            snprintf(new_query, sizeof(new_query),
                     "SELECT rowid, * FROM %s WHERE rowid = %d", table, olds[i].rowid);
            TriggerRowImage* news = NULL;
            int new_count = 0;
            if (!vm_select_row_images(vm, new_query, &news, &new_count)) {
                row_obj_free(olds[i].row);
                for (int j = i + 1; j < old_count; j++) row_obj_free(olds[j].row);
                free(olds);
                set_runtime_error_from_driver_sql(vm, "row trigger post-select failed");
                return -1;
            }
            if (new_count > 0) {
                new_obj = news[0].row;
            }
            free(news);
        }
        if (!vm_fire_row_trigger_pair(vm, event, table, olds[i].row, new_obj)) {
            for (int j = i + 1; j < old_count; j++) row_obj_free(olds[j].row);
            free(olds);
            return -1;
        }
    }
    free(olds);
    return row_count;
}

/* Execute a DML/DDL statement, firing row-level triggers per affected row
   when the chunk declares any matching the statement's table and event. */
static int vm_exec_dml(VM* vm, const char* sql, Value* params, int param_count) {
    DBDriver* driver = vm->driver;
    int event = -1;
    char table[64];
    int row_triggered =
        sql_trigger_info(sql, &event, table, sizeof(table)) &&
        (event == TRIGGER_INSERT || event == TRIGGER_UPDATE || event == TRIGGER_DELETE) &&
        vm_chunk_has_row_triggers(vm, event, table);
    if (row_triggered && driver->is_sqlite) {
        return vm_sqlite_exec_row_triggers(vm, sql, event, table, params, param_count);
    }
    driver->row_trigger_fn = row_triggered ? vm_row_trigger_hook : NULL;
    driver->row_trigger_user = row_triggered ? vm : NULL;
    return driver->exec(driver, sql, params, param_count);
}
/* Execute a dynamic SQL string: intercepts `drop trigger <name>`, fires
   matching BEFORE/AFTER triggers around the statement, and runs it through
   the active driver (or the driver-less custom-engine context). Returns the
   affected row count, or -1 on error. */
int vm_dynamic_exec(VM* vm, const char* sql) {
    if (vm == NULL || sql == NULL) return -1;

    char drop_name[256];
    if (sql_drop_trigger_name(sql, drop_name, sizeof(drop_name))) {
        vm_drop_trigger(vm, drop_name);
        vm->sql_rowcount = 0;
        return 0;
    }

    if (!vm_check_sql_not_firing(vm, sql)) return -1;

    int event = -1;
    char table[64];
    int has_triggers = sql_trigger_info(sql, &event, table, sizeof(table));
    if (has_triggers && !vm_fire_triggers(vm, TRIGGER_BEFORE, event, table)) {
        return -1;
    }

    int row_count;
    DBDriver* driver = vm->driver;
    if (driver != NULL) {
        row_count = vm_exec_dml(vm, sql, NULL, 0);
    } else {
        Context* ctx = vm->context;
        if (ctx == NULL || ctx->pager == NULL) return -1;
        Result* res = sql_exec(sql, ctx);
        if (res == NULL) return -1;
        row_count = res->row_count;
        result_free(res);
    }
    if (row_count < 0) return -1;
    vm_set_sql_rowcount(vm, row_count);

    if (has_triggers && !vm_fire_triggers(vm, TRIGGER_AFTER, event, table)) {
        return -1;
    }
    return row_count;
}

static InterpretResult vm_run(VM* vm, uint8_t* end) {
    for (;;) {
dispatch:
        if (!vm->is_child && gc_container_allocations() >= g_gc_next_collection) {
            vm_gc_collect(vm);
        }
        if (vm->ip >= end) {
            set_runtime_error(vm, "Runtime error");
            THROW(vm);
        }
        uint8_t op = *vm->ip++;
        switch (op) {
            case OP_CONST: {
                if (vm->ip + 2 > end) return INTERPRET_RUNTIME_ERROR;
                uint16_t idx = read_u16(vm->ip);
                vm->ip += 2;
                if (idx >= (uint16_t)vm->chunk->constants_count) {
                    set_runtime_error(vm, "Invalid constant index");
                    THROW(vm);
                }
                Value v = vm->chunk->constants[idx];
                value_retain(v);
                if (!push(vm, v)) {
                    value_release(v);
                    set_runtime_error(vm, "Stack overflow");
                    THROW(vm);
                }
                break;
            }
            case OP_GET_LOCAL: {
                if (vm->ip + 1 > end) return INTERPRET_RUNTIME_ERROR;
                uint8_t slot = *vm->ip++;
                int depth = (int)(vm->stack_top - vm->frame_base);
                if (slot >= depth) return INTERPRET_RUNTIME_ERROR;
                if (vm->frame_count == vm->capture_base && slot + 1 > vm->local_count) {
                    vm->local_count = slot + 1;
                }
                Value v = vm->frame_base[slot];
                value_retain(v);
                if (!push(vm, v)) {
                    value_release(v);
                    set_runtime_error(vm, "Invalid local variable slot");
                    THROW(vm);
                }
                break;
            }
            case OP_GET_LOCAL16: {
                if (vm->ip + 2 > end) return INTERPRET_RUNTIME_ERROR;
                uint16_t slot = read_u16(vm->ip);
                vm->ip += 2;
                int depth = (int)(vm->stack_top - vm->frame_base);
                if (slot >= depth) return INTERPRET_RUNTIME_ERROR;
                if (vm->frame_count == vm->capture_base && slot + 1 > vm->local_count) {
                    vm->local_count = slot + 1;
                }
                Value v = vm->frame_base[slot];
                value_retain(v);
                if (!push(vm, v)) {
                    value_release(v);
                    set_runtime_error(vm, "Invalid local variable slot");
                    THROW(vm);
                }
                break;
            }
            case OP_SET_LOCAL: {
                if (vm->ip + 1 > end) return INTERPRET_RUNTIME_ERROR;
                uint8_t slot = *vm->ip++;
                int depth = (int)(vm->stack_top - vm->frame_base);
                if (slot >= depth) return INTERPRET_RUNTIME_ERROR;
                if (vm->frame_count == vm->capture_base && slot + 1 > vm->local_count) {
                    vm->local_count = slot + 1;
                }
                Value v = *(vm->stack_top - 1);
                value_retain(v);
                value_release(vm->frame_base[slot]);
                vm->frame_base[slot] = v;
                if (vm->frame_count == vm->capture_base) {
                    if (slot < vm->repl_local_count) {
                        value_release(vm->repl_locals[slot]);
                    }
                    vm->repl_locals[slot] = v;
                    value_retain(v);
                    if (slot + 1 > vm->repl_local_count) {
                        vm->repl_local_count = slot + 1;
                    }
                }
                break;
            }
            case OP_SET_LOCAL16: {
                if (vm->ip + 2 > end) return INTERPRET_RUNTIME_ERROR;
                uint16_t slot = read_u16(vm->ip);
                vm->ip += 2;
                int depth = (int)(vm->stack_top - vm->frame_base);
                if (slot >= depth) return INTERPRET_RUNTIME_ERROR;
                if (vm->frame_count == vm->capture_base && slot + 1 > vm->local_count) {
                    vm->local_count = slot + 1;
                }
                Value v = *(vm->stack_top - 1);
                value_retain(v);
                value_release(vm->frame_base[slot]);
                vm->frame_base[slot] = v;
                if (vm->frame_count == vm->capture_base) {
                    if (!vm_ensure_repl_capacity(vm, (int)slot + 1)) {
                        set_runtime_error(vm, "Too many local variables");
                        THROW(vm);
                    }
                    if (slot < vm->repl_local_count) {
                        value_release(vm->repl_locals[slot]);
                    }
                    vm->repl_locals[slot] = v;
                    value_retain(v);
                    if (slot + 1 > vm->repl_local_count) {
                        vm->repl_local_count = slot + 1;
                    }
                }
                break;
            }
            case OP_GET_GLOBAL: {
                if (vm->ip + 1 > end) return INTERPRET_RUNTIME_ERROR;
                uint8_t slot = *vm->ip++;
                if (slot >= (size_t)vm->global_count) return INTERPRET_RUNTIME_ERROR;
                Value v = vm->globals[slot];
                value_retain(v);
                if (!push(vm, v)) {
                    value_release(v);
                    set_runtime_error(vm, "Stack overflow");
                    THROW(vm);
                }
                break;
            }
            case OP_SET_GLOBAL: {
                if (vm->ip + 1 > end) return INTERPRET_RUNTIME_ERROR;
                uint8_t slot = *vm->ip++;
                if (slot >= (size_t)vm->global_count) {
                    vm->global_count = slot + 1;
                }
                Value v = *(vm->stack_top - 1);
                value_retain(v);
                value_release(vm->globals[slot]);
                vm->globals[slot] = v;
                break;
            }
            case OP_ADD: {
                if (!binary_op(vm, value_add)) return INTERPRET_RUNTIME_ERROR;
                break;
            }
            case OP_SUB: {
                if (!binary_op(vm, value_sub)) return INTERPRET_RUNTIME_ERROR;
                break;
            }
            case OP_MUL: {
                if (!binary_op(vm, value_mul)) return INTERPRET_RUNTIME_ERROR;
                break;
            }
            case OP_DIV: {
                if (!binary_op(vm, value_div)) return INTERPRET_RUNTIME_ERROR;
                break;
            }
            case OP_EQ: {
                if (!binary_op(vm, value_eq)) return INTERPRET_RUNTIME_ERROR;
                break;
            }
            case OP_LT: {
                if (!binary_op(vm, value_lt)) return INTERPRET_RUNTIME_ERROR;
                break;
            }
            case OP_GT: {
                if (!binary_op(vm, value_gt)) return INTERPRET_RUNTIME_ERROR;
                break;
            }
            case OP_NEGATE: {
                Value value;
                if (!pop(vm, &value)) return INTERPRET_RUNTIME_ERROR;
                int ok = 0;
                if (value.type == VAL_FLOAT) {
                    ok = push(vm, value_float(-value.as.as_float));
                } else if (value.type == VAL_INT) {
                    ok = push(vm, value_int(-value.as.as_int));
                } else if (value.type == VAL_NULL) {
                    ok = push(vm, value_null());
                } else {
                    value_release(value);
                    set_runtime_error(vm, "Cannot negate non-numeric value");
                    THROW(vm);
                }
                value_release(value);
                if (!ok) return INTERPRET_RUNTIME_ERROR;
                break;
            }
            case OP_NOT: {
                Value value;
                if (!pop(vm, &value)) return INTERPRET_RUNTIME_ERROR;
                int ok = push(vm, value_bool(!value_is_truthy(value)));
                value_release(value);
                if (!ok) return INTERPRET_RUNTIME_ERROR;
                break;
            }
            case OP_DUP: {
                if (vm->stack_top <= vm->stack) {
                    set_runtime_error(vm, "Stack underflow");
                    return INTERPRET_RUNTIME_ERROR;
                }
                Value value = *(vm->stack_top - 1);
                value_retain(value);
                if (!push(vm, value)) {
                    value_release(value);
                    return INTERPRET_RUNTIME_ERROR;
                }
                break;
            }
            case OP_POP: {
                Value value;
                if (!pop(vm, &value)) return INTERPRET_RUNTIME_ERROR;
                value_release(value);
                break;
            }
            case OP_SQL: {
                if (vm->ip + 4 > end) return INTERPRET_RUNTIME_ERROR;
                uint16_t idx = read_u16(vm->ip);
                vm->ip += 2;
                uint16_t line = read_u16(vm->ip);
                vm->ip += 2;
                vm->sql_line = (int)line;
                if (idx >= (uint16_t)vm->chunk->constants_count) return INTERPRET_RUNTIME_ERROR;
                Value query_value = vm->chunk->constants[idx];
                if (query_value.type != VAL_STRING || query_value.as.as_string == NULL) {
                    set_runtime_error_sql(vm, "Invalid SQL query");
                    THROW(vm);
                }
                if (vm->driver != NULL) {
                    if (vm->result_handle != NULL) {
                        vm->driver->result_free(vm->driver, vm->result_handle);
                    }
                    if (!vm->driver->query(vm->driver, query_value.as.as_string,
                                           vm->sql_params, vm->sql_param_count, &vm->result_handle)) {
                        for (int i = 0; i < vm->sql_param_count; i++) {
                            value_release(vm->sql_params[i]);
                        }
                        vm->sql_param_count = 0;
                        set_runtime_error_from_driver_sql(vm, "SQL query failed");
                        THROW(vm);
                    }
                    for (int i = 0; i < vm->sql_param_count; i++) {
                        value_release(vm->sql_params[i]);
                    }
                    vm->sql_param_count = 0;
                } else {
                    Context* ctx = vm->context;
                    if (ctx == NULL || ctx->pager == NULL) {
                        set_runtime_error_sql(vm, "No database context");
                        THROW(vm);
                    }
                    result_free((Result*)vm->result_handle);
                    vm->result_handle = sql_exec(query_value.as.as_string, ctx);
                    for (int i = 0; i < vm->sql_param_count; i++) {
                        value_release(vm->sql_params[i]);
                    }
                    vm->sql_param_count = 0;
                    if (vm->result_handle == NULL) {
                        set_runtime_error_sql(vm, "SQL query failed");
                        THROW(vm);
                    }
                }
                vm->row_handle = NULL;
                break;
            }
            case OP_SQL_NEXT: {
                if (vm->ip + 2 > end) return INTERPRET_RUNTIME_ERROR;
                int16_t offset = (int16_t)read_u16(vm->ip);
                vm->ip += 2;
                int has_row = 0;
                if (vm->driver != NULL) {
                    void* next = NULL;
                    has_row = vm->driver->result_next(vm->driver, vm->result_handle, &next);
                    vm->row_handle = has_row ? next : NULL;
                } else {
                    Row* next = result_next((Result*)vm->result_handle);
                    has_row = next != NULL;
                    vm->row_handle = next;
                }
                if (!has_row) {
                    uint8_t* target = vm->ip + offset;
                    if (target < vm->chunk->code || target > end) {
                        set_runtime_error(vm, "Invalid SQL query");
                        THROW(vm);
                    }
                    vm->ip = target;
                }
                break;
            }
            case OP_GET_FIELD: {
                if (vm->ip + 2 > end) return INTERPRET_RUNTIME_ERROR;
                uint16_t idx = read_u16(vm->ip);
                vm->ip += 2;
                if (idx >= (uint16_t)vm->chunk->constants_count) return INTERPRET_RUNTIME_ERROR;
                Value name_value = vm->chunk->constants[idx];
                if (name_value.type != VAL_STRING || name_value.as.as_string == NULL) {
                    set_runtime_error(vm, "Invalid field access");
                    THROW(vm);
                }
                Value field_value;
                if (vm->driver != NULL) {
                    if (!vm->driver->row_get_field(vm->driver, vm->row_handle,
                                                   name_value.as.as_string, &field_value)) {
                        set_runtime_error_from_driver(vm, "Field access failed");
                        THROW(vm);
                    }
                } else {
                    Cell cell = row_get_field((Row*)vm->row_handle, name_value.as.as_string);
                    field_value.type = cell.type;
                    if (cell.type == VAL_STRING) {
                        field_value = value_string(strdup(cell.as.as_string));
                    } else if (cell.type == VAL_FLOAT) {
                        field_value.as.as_float = cell.as.as_float;
                    } else {
                        field_value.as.as_int = cell.as.as_int;
                    }
                }
                if (!push(vm, field_value)) return INTERPRET_RUNTIME_ERROR;
                break;
            }
            case OP_JZ: {
                if (vm->ip + 2 > end) return INTERPRET_RUNTIME_ERROR;
                int16_t offset = (int16_t)read_u16(vm->ip);
                vm->ip += 2;
                Value cond;
                if (!pop(vm, &cond)) return INTERPRET_RUNTIME_ERROR;
                int truthy = value_is_truthy(cond);
                value_release(cond);
                if (!truthy) {
                    uint8_t* target = vm->ip + offset;
                    if (target < vm->chunk->code || target > end) {
                        set_runtime_error(vm, "Invalid jump target");
                        THROW(vm);
                    }
                    vm->ip = target;
                }
                break;
            }
            case OP_JMP: {
                if (vm->ip + 2 > end) return INTERPRET_RUNTIME_ERROR;
                int16_t offset = (int16_t)read_u16(vm->ip);
                vm->ip += 2;
                uint8_t* target = vm->ip + offset;
                if (target < vm->chunk->code || target > end) {
                    set_runtime_error(vm, "Invalid jump target");
                    THROW(vm);
                }
                vm->ip = target;
                break;
            }
            case OP_CALL: {
                if (vm->ip + 3 > end) return INTERPRET_RUNTIME_ERROR;
                uint16_t target = read_u16(vm->ip);
                vm->ip += 2;
                uint8_t arg_count = *vm->ip++;
                if (target > (uint16_t)vm->chunk->count) return INTERPRET_RUNTIME_ERROR;
                if (arg_count > (size_t)(vm->stack_top - vm->frame_base)) {
                    set_runtime_error(vm, "Invalid argument count");
                    THROW(vm);
                }
                if (!vm_ensure_frame_capacity(vm, vm->frame_count + 1)) {
                    set_runtime_error(vm, "Stack overflow");
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (!vm_check_trigger_entry(vm, target)) THROW(vm);
                vm->out_frames[vm->frame_count].out_count = 0;
                vm->out_frames[vm->frame_count].entry_offset = target;
                vm->return_ips[vm->frame_count] = vm->ip;
                vm->frames[vm->frame_count] = vm->frame_base;
                vm->frame_count++;
                vm->frame_base = vm->stack_top - arg_count;
                vm->ip = vm->chunk->code + target;
                break;
            }
            case OP_CALL_AUTONOMOUS: {
                if (vm->ip + 3 > end) return INTERPRET_RUNTIME_ERROR;
                uint16_t target = read_u16(vm->ip);
                vm->ip += 2;
                uint8_t arg_count = *vm->ip++;
                if (arg_count > (size_t)(vm->stack_top - vm->frame_base)) {
                    set_runtime_error(vm, "Invalid argument count");
                    THROW(vm);
                }
                Value result;
                int auto_result = vm_call_autonomous(vm, target, arg_count, 0, NULL, NULL, &result);
                if (auto_result == 1) {
                    for (int i = 0; i < arg_count; i++) {
                        Value dummy;
                        pop(vm, &dummy);
                        value_release(dummy);
                    }
                    if (!push(vm, result)) {
                        value_release(result);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                } else if (auto_result == -1) {
                    if (!vm_ensure_frame_capacity(vm, vm->frame_count + 1)) {
                    set_runtime_error(vm, "Stack overflow");
                    return INTERPRET_RUNTIME_ERROR;
                }
                    if (!vm_check_trigger_entry(vm, target)) THROW(vm);
                    vm->out_frames[vm->frame_count].out_count = 0;
                    vm->out_frames[vm->frame_count].entry_offset = target;
                    vm->return_ips[vm->frame_count] = vm->ip;
                    vm->frames[vm->frame_count] = vm->frame_base;
                    vm->frame_count++;
                    vm->frame_base = vm->stack_top - arg_count;
                    vm->ip = vm->chunk->code + target;
                } else {
                    THROW(vm);
                }
                break;
            }
            case OP_CALL_OUT: {
                if (vm->ip + 4 > end) return INTERPRET_RUNTIME_ERROR;
                uint16_t target = read_u16(vm->ip);
                vm->ip += 2;
                uint8_t arg_count = *vm->ip++;
                uint8_t out_count = *vm->ip++;
                if (target > (uint16_t)vm->chunk->count) return INTERPRET_RUNTIME_ERROR;
                if (arg_count > (size_t)(vm->stack_top - vm->frame_base)) {
                    set_runtime_error(vm, "Invalid argument count");
                    THROW(vm);
                }
                if (out_count > MAX_OUT_PARAMS) {
                    set_runtime_error(vm, "Too many OUT/IN OUT parameters");
                    THROW(vm);
                }
                if (vm->ip + out_count * 4 > end) return INTERPRET_RUNTIME_ERROR;
                if (!vm_ensure_frame_capacity(vm, vm->frame_count + 1)) {
                    set_runtime_error(vm, "Stack overflow");
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (!vm_check_trigger_entry(vm, target)) THROW(vm);
                OutParamFrame* out_frame = &vm->out_frames[vm->frame_count];
                out_frame->out_count = (int)out_count;
                out_frame->entry_offset = target;
                for (int i = 0; i < out_count; i++) {
                    out_frame->out_positions[i] = (int)read_u16(vm->ip);
                    vm->ip += 2;
                    out_frame->out_slots[i] = (int)read_u16(vm->ip);
                    vm->ip += 2;
                }
                vm->return_ips[vm->frame_count] = vm->ip;
                vm->frames[vm->frame_count] = vm->frame_base;
                vm->frame_count++;
                vm->frame_base = vm->stack_top - arg_count;
                vm->ip = vm->chunk->code + target;
                break;
            }
            case OP_CALL_OUT_AUTONOMOUS: {
                if (vm->ip + 4 > end) return INTERPRET_RUNTIME_ERROR;
                uint16_t target = read_u16(vm->ip);
                vm->ip += 2;
                uint8_t arg_count = *vm->ip++;
                uint8_t out_count = *vm->ip++;
                if (target > (uint16_t)vm->chunk->count) return INTERPRET_RUNTIME_ERROR;
                if (arg_count > (size_t)(vm->stack_top - vm->frame_base)) {
                    set_runtime_error(vm, "Invalid argument count");
                    THROW(vm);
                }
                if (out_count > MAX_OUT_PARAMS) {
                    set_runtime_error(vm, "Too many OUT/IN OUT parameters");
                    THROW(vm);
                }
                if (vm->ip + out_count * 4 > end) return INTERPRET_RUNTIME_ERROR;
                int out_positions[MAX_OUT_PARAMS];
                int out_slots[MAX_OUT_PARAMS];
                for (int i = 0; i < out_count; i++) {
                    out_positions[i] = (int)read_u16(vm->ip);
                    vm->ip += 2;
                    out_slots[i] = (int)read_u16(vm->ip);
                    vm->ip += 2;
                }
                Value result;
                int auto_result = vm_call_autonomous(vm, target, arg_count,
                                                     (int)out_count, out_positions, out_slots,
                                                     &result);
                if (auto_result == 1) {
                    for (int i = 0; i < arg_count; i++) {
                        Value dummy;
                        pop(vm, &dummy);
                        value_release(dummy);
                    }
                    if (!push(vm, result)) {
                        value_release(result);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                } else if (auto_result == -1) {
                    if (!vm_ensure_frame_capacity(vm, vm->frame_count + 1)) {
                    set_runtime_error(vm, "Stack overflow");
                    return INTERPRET_RUNTIME_ERROR;
                }
                    if (!vm_check_trigger_entry(vm, target)) THROW(vm);
                    OutParamFrame* out_frame = &vm->out_frames[vm->frame_count];
                    out_frame->out_count = (int)out_count;
                    out_frame->entry_offset = target;
                    for (int i = 0; i < out_count; i++) {
                        out_frame->out_positions[i] = out_positions[i];
                        out_frame->out_slots[i] = out_slots[i];
                    }
                    vm->return_ips[vm->frame_count] = vm->ip;
                    vm->frames[vm->frame_count] = vm->frame_base;
                    vm->frame_count++;
                    vm->frame_base = vm->stack_top - arg_count;
                    vm->ip = vm->chunk->code + target;
                } else {
                    THROW(vm);
                }
                break;
            }
            case OP_RETURN: {
                if (vm->frame_count == 0) {
                    return INTERPRET_OK;
                }
                Value* callee_frame_base = vm->frame_base;
                Value result = *(vm->stack_top - 1);
                value_retain(result);
                int callee_stack_size = (int)(vm->stack_top - callee_frame_base);
                OutParamFrame* out_frame = &vm->out_frames[vm->frame_count - 1];
                Value out_values[MAX_OUT_PARAMS];
                for (int i = 0; i < out_frame->out_count; i++) {
                    int pos = out_frame->out_positions[i];
                    if (pos < 0 || pos >= callee_stack_size) {
                        set_runtime_error(vm, "Invalid OUT parameter position");
                        THROW(vm);
                    }
                    out_values[i] = callee_frame_base[pos];
                    value_retain(out_values[i]);
                }
                for (Value* p = callee_frame_base; p < vm->stack_top; p++) {
                    value_release(*p);
                }
                vm->frame_count--;
                vm->frame_base = vm->frames[vm->frame_count];
                vm->ip = vm->return_ips[vm->frame_count];
                vm->stack_top = callee_frame_base;
                for (int i = 0; i < out_frame->out_count; i++) {
                    int slot = out_frame->out_slots[i];
                    if (slot < 0 || slot >= (vm->stack_top - vm->frame_base)) {
                        set_runtime_error(vm, "Invalid OUT parameter slot");
                        value_release(out_values[i]);
                        THROW(vm);
                    }
                    value_release(vm->frame_base[slot]);
                    vm->frame_base[slot] = out_values[i];
                }
                *vm->stack_top++ = result;
                break;
            }
            case OP_NATIVE_CALL: {
                if (vm->ip + 3 > end) return INTERPRET_RUNTIME_ERROR;
                uint16_t idx = read_u16(vm->ip);
                vm->ip += 2;
                uint8_t argc = *vm->ip++;
                if (argc > MAX_NATIVE_ARGS) {
                    set_runtime_error(vm, "Too many arguments");
                    THROW(vm);
                }
                if (argc > (size_t)(vm->stack_top - vm->frame_base)) {
                    set_runtime_error(vm, "Invalid argument count");
                    THROW(vm);
                }
                Value argv[MAX_NATIVE_ARGS];
                for (int i = (int)argc - 1; i >= 0; i--) {
                    if (!pop(vm, &argv[i])) {
                        set_runtime_error(vm, "Stack underflow");
                        THROW(vm);
                    }
                }
                Value result;
                if (!native_call(vm, idx, argc, argv, &result)) {
                    for (int i = 0; i < (int)argc; i++) value_release(argv[i]);
                    THROW(vm);
                }
                if (!push(vm, result)) {
                    value_release(result);
                    for (int i = 0; i < (int)argc; i++) value_release(argv[i]);
                    return INTERPRET_RUNTIME_ERROR;
                }
                for (int i = 0; i < (int)argc; i++) value_release(argv[i]);
                break;
            }
            case OP_PRINT: {
                Value value;
                if (!pop(vm, &value)) return INTERPRET_RUNTIME_ERROR;
                value_print(value);
                printf("\n");
                value_release(value);
                break;
            }
            case OP_ARRAY_BUILD: {
                if (vm->ip + 2 > end) return INTERPRET_RUNTIME_ERROR;
                uint16_t count = read_u16(vm->ip);
                vm->ip += 2;
                if (count > (size_t)(vm->stack_top - vm->frame_base)) {
                    set_runtime_error(vm, "Not enough values for array");
                    THROW(vm);
                }
                ArrayObj* array = array_new();
                if (array == NULL) {
                    set_runtime_error(vm, "Out of memory");
                    THROW(vm);
                }
                Value* temp = NULL;
                if (count > 0) {
                    temp = malloc(sizeof(Value) * count);
                    if (temp == NULL) {
                        array_free(array);
                        set_runtime_error(vm, "Out of memory");
                        THROW(vm);
                    }
                    for (int i = (int)count - 1; i >= 0; i--) {
                        if (!pop(vm, &temp[i])) {
                            free(temp);
                            array_free(array);
                            return INTERPRET_RUNTIME_ERROR;
                        }
                    }
                    for (int i = 0; i < count; i++) {
                        if (!array_append(array, temp[i])) {
                            free(temp);
                            array_free(array);
                            set_runtime_error(vm, "Out of memory");
                            THROW(vm);
                        }
                    }
                }
                if (!push(vm, value_array(array))) {
                    array_free(array);
                    if (temp != NULL) free(temp);
                    return INTERPRET_RUNTIME_ERROR;
                }
                for (int i = 0; i < (int)count; i++) {
                    value_release(temp[i]);
                }
                free(temp);
                break;
            }
            case OP_MAP_BUILD: {
                if (vm->ip + 2 > end) return INTERPRET_RUNTIME_ERROR;
                uint16_t count = read_u16(vm->ip);
                vm->ip += 2;
                if (count * 2 > (size_t)(vm->stack_top - vm->frame_base)) {
                    set_runtime_error(vm, "Not enough values for map");
                    THROW(vm);
                }
                MapObj* map = map_new();
                if (map == NULL) {
                    set_runtime_error(vm, "Out of memory");
                    THROW(vm);
                }
                for (int i = (int)count - 1; i >= 0; i--) {
                    Value key;
                    Value val;
                    if (!pop(vm, &key)) {
                        map_free(map);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    if (!pop(vm, &val)) {
                        value_release(key);
                        map_free(map);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    if (key.type != VAL_INT && key.type != VAL_STRING) {
                        value_release(key);
                        value_release(val);
                        map_free(map);
                        set_runtime_error(vm, "Map key must be int or string");
                        THROW(vm);
                    }
                    if (!map_set(map, key, val)) {
                        value_release(key);
                        value_release(val);
                        map_free(map);
                        set_runtime_error(vm, "Out of memory");
                        THROW(vm);
                    }
                    value_release(key);
                    value_release(val);
                }
                Value result = value_map(map);
                if (!push(vm, result)) {
                    value_release(result);
                    return INTERPRET_RUNTIME_ERROR;
                }
                break;
            }
            case OP_INDEX_GET: {
                Value idx_val;
                Value arr_val;
                if (!pop(vm, &idx_val)) return INTERPRET_RUNTIME_ERROR;
                if (!pop(vm, &arr_val)) return INTERPRET_RUNTIME_ERROR;
                if (arr_val.type == VAL_ARRAY && idx_val.type == VAL_INT) {
                    int idx = idx_val.as.as_int;
                    if (idx < 0 || idx >= array_length(arr_val.as.as_array)) {
                        value_release(idx_val);
                        value_release(arr_val);
                        set_runtime_error(vm, "Array index out of bounds");
                        THROW(vm);
                    }
                    Value result = array_get(arr_val.as.as_array, idx);
                    value_retain(result);
                    if (!push(vm, result)) {
                        value_release(result);
                        value_release(idx_val);
                        value_release(arr_val);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    value_release(idx_val);
                    value_release(arr_val);
                    break;
                }
                if (arr_val.type == VAL_ROW && idx_val.type == VAL_STRING) {
                    Value result = row_obj_get_field((RowObj*)arr_val.as.as_row_handle,
                                                     idx_val.as.as_string);
                    if (!push(vm, result)) {
                        value_release(result);
                        value_release(idx_val);
                        value_release(arr_val);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    value_release(idx_val);
                    value_release(arr_val);
                    break;
                }
                if (arr_val.type == VAL_MAP) {
                    if (idx_val.type != VAL_INT && idx_val.type != VAL_STRING) {
                        value_release(idx_val);
                        value_release(arr_val);
                        set_runtime_error(vm, "Map key must be int or string");
                        THROW(vm);
                    }
                    Value result;
                    if (!map_get(arr_val.as.as_map, idx_val, &result)) {
                        value_release(idx_val);
                        value_release(arr_val);
                        set_runtime_error(vm, "Key not found in map");
                        THROW(vm);
                    }
                    if (!push(vm, result)) {
                        value_release(result);
                        value_release(idx_val);
                        value_release(arr_val);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    value_release(result);
                    value_release(idx_val);
                    value_release(arr_val);
                    break;
                }
                value_release(idx_val);
                value_release(arr_val);
                set_runtime_error(vm, "Invalid index");
                THROW(vm);
                break;
            }
            case OP_INDEX_SET: {
                Value val;
                Value idx_val;
                Value arr_val;
                if (!pop(vm, &val)) return INTERPRET_RUNTIME_ERROR;
                if (!pop(vm, &idx_val)) return INTERPRET_RUNTIME_ERROR;
                if (!pop(vm, &arr_val)) return INTERPRET_RUNTIME_ERROR;
                if (arr_val.type == VAL_ARRAY && idx_val.type == VAL_INT) {
                    int idx = idx_val.as.as_int;
                    if (idx < 0 || idx >= array_length(arr_val.as.as_array)) {
                        value_release(val);
                        value_release(idx_val);
                        value_release(arr_val);
                        set_runtime_error(vm, "Array index out of bounds");
                        THROW(vm);
                    }
                    array_set(arr_val.as.as_array, idx, val);
                    value_release(val);
                    value_release(idx_val);
                    value_release(arr_val);
                    break;
                }
                if (arr_val.type == VAL_ROW && idx_val.type == VAL_STRING) {
                    RowObj* row = (RowObj*)arr_val.as.as_row_handle;
                    if (!row_obj_set_field(row, idx_val.as.as_string, val)) {
                        value_release(val);
                        value_release(idx_val);
                        value_release(arr_val);
                        set_runtime_error(vm, "Map key not found");
                        THROW(vm);
                    }
                    value_release(val);
                    value_release(idx_val);
                    value_release(arr_val);
                    break;
                }
                if (arr_val.type == VAL_MAP) {
                    if (idx_val.type != VAL_INT && idx_val.type != VAL_STRING) {
                        value_release(val);
                        value_release(idx_val);
                        value_release(arr_val);
                        set_runtime_error(vm, "Map key must be int or string");
                        THROW(vm);
                    }
                    if (!map_set(arr_val.as.as_map, idx_val, val)) {
                        value_release(val);
                        value_release(idx_val);
                        value_release(arr_val);
                        set_runtime_error(vm, "Out of memory");
                        THROW(vm);
                    }
                    value_release(val);
                    value_release(idx_val);
                    value_release(arr_val);
                    break;
                }
                value_release(val);
                value_release(idx_val);
                value_release(arr_val);
                set_runtime_error(vm, "Invalid index assignment");
                THROW(vm);
                break;
            }
            case OP_SQL_EXEC: {
                if (vm->ip + 4 > end) return INTERPRET_RUNTIME_ERROR;
                uint16_t idx = read_u16(vm->ip);
                vm->ip += 2;
                uint16_t line = read_u16(vm->ip);
                vm->ip += 2;
                vm->sql_line = (int)line;
                if (idx >= (uint16_t)vm->chunk->constants_count) return INTERPRET_RUNTIME_ERROR;
                Value sql_value = vm->chunk->constants[idx];
                if (sql_value.type != VAL_STRING || sql_value.as.as_string == NULL) {
                    set_runtime_error_sql(vm, "Invalid SQL statement");
                    THROW(vm);
                }
                if (vm->driver == NULL) {
                    set_runtime_error_sql(vm, "No database driver");
                    THROW(vm);
                }
                if (!vm_check_sql_not_firing(vm, sql_value.as.as_string)) {
                    for (int i = 0; i < vm->sql_param_count; i++) {
                        value_release(vm->sql_params[i]);
                    }
                    vm->sql_param_count = 0;
                    THROW(vm);
                }
                int row_count = vm_exec_dml(vm, sql_value.as.as_string,
                                            vm->sql_params, vm->sql_param_count);
                for (int i = 0; i < vm->sql_param_count; i++) {
                    value_release(vm->sql_params[i]);
                }
                vm->sql_param_count = 0;
                if (row_count < 0) {
                    set_runtime_error_from_driver_sql(vm, "SQL execution failed");
                    THROW(vm);
                }
                vm->sql_rowcount = row_count;
                break;
            }
            case OP_DROP_TRIGGER: {
                if (vm->ip + 2 > end) return INTERPRET_RUNTIME_ERROR;
                uint16_t idx = read_u16(vm->ip);
                vm->ip += 2;
                if (idx >= (uint16_t)vm->chunk->constants_count) return INTERPRET_RUNTIME_ERROR;
                Value name_value = vm->chunk->constants[idx];
                if (name_value.type != VAL_STRING || name_value.as.as_string == NULL) {
                    set_runtime_error(vm, "Invalid trigger name");
                    THROW(vm);
                }
                vm_drop_trigger(vm, name_value.as.as_string);
                break;
            }
            case OP_SQL_BIND_INT:
            case OP_SQL_BIND_FLOAT:
            case OP_SQL_BIND_STRING: {
                Value v;
                if (!pop(vm, &v)) return INTERPRET_RUNTIME_ERROR;
                if (vm->driver == NULL) {
                    value_release(v);
                    set_runtime_error(vm, "No database driver");
                    THROW(vm);
                }
                if (vm->sql_param_count >= 16) {
                    value_release(v);
                    set_runtime_error(vm, "Too many SQL parameters");
                    THROW(vm);
                }
                vm->sql_params[vm->sql_param_count++] = v;
                break;
            }
            case OP_SQL_BEGIN: {
                if (vm->driver == NULL) {
                    set_runtime_error(vm, "No database driver");
                    THROW(vm);
                }
                if (!vm->driver->begin(vm->driver)) {
                    set_runtime_error_from_driver(vm, "BEGIN failed");
                    THROW(vm);
                }
                break;
            }
            case OP_SQL_COMMIT: {
                if (vm->driver == NULL) {
                    set_runtime_error(vm, "No database driver");
                    THROW(vm);
                }
                if (!vm->driver->commit(vm->driver)) {
                    set_runtime_error_from_driver(vm, "COMMIT failed");
                    THROW(vm);
                }
                break;
            }
            case OP_SQL_ROLLBACK: {
                if (vm->driver == NULL) {
                    set_runtime_error(vm, "No database driver");
                    THROW(vm);
                }
                if (!vm->driver->rollback(vm->driver)) {
                    set_runtime_error_from_driver(vm, "ROLLBACK failed");
                    THROW(vm);
                }
                break;
            }
            case OP_SQL_SAVEPOINT:
            case OP_SQL_ROLLBACK_TO:
            case OP_SQL_RELEASE_SAVEPOINT: {
                if (vm->ip + 2 > end) return INTERPRET_RUNTIME_ERROR;
                uint16_t idx = read_u16(vm->ip);
                vm->ip += 2;
                if (idx >= (uint16_t)vm->chunk->constants_count) {
                    set_runtime_error(vm, "Invalid constant index");
                    THROW(vm);
                }
                Value name_value = vm->chunk->constants[idx];
                if (name_value.type != VAL_STRING || name_value.as.as_string == NULL) {
                    set_runtime_error(vm, "Invalid savepoint name");
                    THROW(vm);
                }
                if (vm->driver == NULL) {
                    set_runtime_error(vm, "No database driver");
                    THROW(vm);
                }
                int ok = 0;
                if (op == OP_SQL_SAVEPOINT) {
                    ok = vm->driver->savepoint(vm->driver, name_value.as.as_string);
                } else if (op == OP_SQL_ROLLBACK_TO) {
                    ok = vm->driver->rollback_to_savepoint(vm->driver, name_value.as.as_string);
                } else {
                    ok = vm->driver->release_savepoint(vm->driver, name_value.as.as_string);
                }
                if (!ok) {
                    const char* op_name = (op == OP_SQL_SAVEPOINT) ? "SAVEPOINT" :
                                          (op == OP_SQL_ROLLBACK_TO) ? "ROLLBACK TO SAVEPOINT" :
                                          "RELEASE SAVEPOINT";
                    set_runtime_error_from_driver(vm, op_name);
                    THROW(vm);
                }
                break;
            }
            case OP_CURSOR_OPEN: {
                if (vm->ip + 4 > end) return INTERPRET_RUNTIME_ERROR;
                uint16_t idx = read_u16(vm->ip);
                vm->ip += 2;
                uint16_t line = read_u16(vm->ip);
                vm->ip += 2;
                vm->sql_line = (int)line;
                Value old_cursor;
                if (!pop(vm, &old_cursor)) return INTERPRET_RUNTIME_ERROR;
                if (old_cursor.type != VAL_CURSOR) {
                    value_release(old_cursor);
                    set_runtime_error_sql(vm, "OPEN requires a cursor variable");
                    THROW(vm);
                }
                value_release(old_cursor);
                if (idx >= (uint16_t)vm->chunk->constants_count) return INTERPRET_RUNTIME_ERROR;
                Value query_value = vm->chunk->constants[idx];
                if (query_value.type != VAL_STRING || query_value.as.as_string == NULL) {
                    set_runtime_error_sql(vm, "Invalid SQL query");
                    THROW(vm);
                }
                CursorObj* cursor = cursor_obj_new(vm->driver);
                if (cursor == NULL) {
                    set_runtime_error_sql(vm, "out of memory");
                    THROW(vm);
                }
                if (vm->driver != NULL) {
                    if (!vm->driver->query(vm->driver, query_value.as.as_string,
                                           vm->sql_params, vm->sql_param_count, &cursor->result_handle)) {
                        for (int i = 0; i < vm->sql_param_count; i++) {
                            value_release(vm->sql_params[i]);
                        }
                        vm->sql_param_count = 0;
                        value_release(value_cursor(cursor));
                        set_runtime_error_from_driver_sql(vm, "SQL query failed");
                        THROW(vm);
                    }
                    for (int i = 0; i < vm->sql_param_count; i++) {
                        value_release(vm->sql_params[i]);
                    }
                    vm->sql_param_count = 0;
                } else {
                    Context* ctx = vm->context;
                    if (ctx == NULL || ctx->pager == NULL) {
                        value_release(value_cursor(cursor));
                        set_runtime_error_sql(vm, "No database context");
                        THROW(vm);
                    }
                    cursor->result_handle = sql_exec(query_value.as.as_string, ctx);
                    for (int i = 0; i < vm->sql_param_count; i++) {
                        value_release(vm->sql_params[i]);
                    }
                    vm->sql_param_count = 0;
                    if (cursor->result_handle == NULL) {
                        value_release(value_cursor(cursor));
                        set_runtime_error_sql(vm, "SQL query failed");
                        THROW(vm);
                    }
                }
                cursor->is_open = 1;
                cursor->row_count = 0;
                cursor->found = 0;
                cursor->row_handle = NULL;
                if (!push(vm, value_cursor(cursor))) {
                    value_release(value_cursor(cursor));
                    return INTERPRET_RUNTIME_ERROR;
                }
                break;
            }
            case OP_CURSOR_FETCH: {
                if (vm->ip + 1 > end) return INTERPRET_RUNTIME_ERROR;
                uint8_t into_count = *vm->ip++;
                if (vm->ip + into_count * 2 > end) return INTERPRET_RUNTIME_ERROR;
                int into_slots[64];
                for (int i = 0; i < into_count; i++) {
                    into_slots[i] = (int)read_u16(vm->ip);
                    vm->ip += 2;
                }
                Value cursor_value;
                if (!pop(vm, &cursor_value)) return INTERPRET_RUNTIME_ERROR;
                if (cursor_value.type != VAL_CURSOR || cursor_value.as.as_cursor == NULL) {
                    value_release(cursor_value);
                    set_runtime_error(vm, "FETCH requires a cursor variable");
                    THROW(vm);
                }
                CursorObj* cursor = cursor_value.as.as_cursor;
                if (cursor == NULL || !cursor->is_open) {
                    value_release(cursor_value);
                    set_runtime_error(vm, "FETCH on closed cursor");
                    THROW(vm);
                }
                int has_row = 0;
                if (vm->driver != NULL) {
                    void* next = NULL;
                    has_row = vm->driver->result_next(vm->driver, cursor->result_handle, &next);
                    cursor->row_handle = has_row ? next : NULL;
                } else {
                    Row* next = result_next((Result*)cursor->result_handle);
                    has_row = next != NULL;
                    cursor->row_handle = next;
                }
                cursor->found = has_row;
                if (has_row) {
                    cursor->row_count++;
                    for (int i = 0; i < into_count; i++) {
                        Value col_value;
                        if (vm->driver != NULL) {
                            if (!vm->driver->row_get_column(vm->driver, cursor->row_handle, i, &col_value)) {
                                value_release(cursor_value);
                                set_runtime_error_from_driver_sql(vm, "Column access failed");
                                THROW(vm);
                            }
                        } else {
                            Row* row = (Row*)cursor->row_handle;
                            if (row == NULL || i >= row->field_count) {
                                value_release(cursor_value);
                                set_runtime_error_sql(vm, "Invalid column index");
                                THROW(vm);
                            }
                            col_value = value_from_cell(row->fields[i].value, value_int(0));
                        }
                        int slot = into_slots[i];
                        int depth = (int)(vm->stack_top - vm->frame_base);
                        if (slot >= depth) {
                            value_release(col_value);
                            value_release(cursor_value);
                            set_runtime_error(vm, "Invalid local variable slot");
                            THROW(vm);
                        }
                        /* col_value is a new value; its reference moves into the slot. */
                        value_release(vm->frame_base[slot]);
                        vm->frame_base[slot] = col_value;
                    }
                }
                value_release(cursor_value);
                break;
            }
            case OP_CURSOR_CLOSE: {
                Value cursor_value;
                if (!pop(vm, &cursor_value)) return INTERPRET_RUNTIME_ERROR;
                if (cursor_value.type != VAL_CURSOR || cursor_value.as.as_cursor == NULL) {
                    value_release(cursor_value);
                    set_runtime_error(vm, "CLOSE requires a cursor variable");
                    THROW(vm);
                }
                CursorObj* cursor = cursor_value.as.as_cursor;
                if (cursor == NULL) {
                    value_release(cursor_value);
                    break;
                }
                cursor->is_open = 0;
                cursor->found = 0;
                if (cursor->result_handle != NULL) {
                    if (cursor->driver != NULL) {
                        cursor->driver->result_free(cursor->driver, cursor->result_handle);
                    } else {
                        result_free((Result*)cursor->result_handle);
                    }
                    cursor->result_handle = NULL;
                }
                cursor->row_handle = NULL;
                value_release(cursor_value);
                break;
            }
            case OP_CURSOR_ATTR: {
                if (vm->ip + 2 > end) return INTERPRET_RUNTIME_ERROR;
                uint16_t idx = read_u16(vm->ip);
                vm->ip += 2;
                Value cursor_value;
                if (!pop(vm, &cursor_value)) return INTERPRET_RUNTIME_ERROR;
                if (cursor_value.type != VAL_CURSOR) {
                    value_release(cursor_value);
                    set_runtime_error(vm, "Cursor attribute requires a cursor variable");
                    THROW(vm);
                }
                CursorObj* cursor = cursor_value.as.as_cursor;
                if (idx >= (uint16_t)vm->chunk->constants_count) {
                    value_release(cursor_value);
                    return INTERPRET_RUNTIME_ERROR;
                }
                Value attr_value = vm->chunk->constants[idx];
                if (attr_value.type != VAL_STRING || attr_value.as.as_string == NULL) {
                    value_release(cursor_value);
                    set_runtime_error(vm, "Invalid cursor attribute");
                    THROW(vm);
                }
                const char* attr = attr_value.as.as_string;
                Value result = value_int(0);
                if (strcmp(attr, "found") == 0) {
                    result = value_bool(cursor != NULL ? cursor->found : 0);
                } else if (strcmp(attr, "notfound") == 0) {
                    result = value_bool(cursor != NULL ? !cursor->found : 1);
                } else if (strcmp(attr, "rowcount") == 0) {
                    result = value_int(cursor != NULL ? cursor->row_count : 0);
                } else if (strcmp(attr, "isopen") == 0) {
                    result = value_bool(cursor != NULL ? cursor->is_open : 0);
                } else {
                    value_release(cursor_value);
                    set_runtime_error(vm, "Unknown cursor attribute");
                    THROW(vm);
                }
                if (!push(vm, result)) {
                    value_release(cursor_value);
                    return INTERPRET_RUNTIME_ERROR;
                }
                value_release(cursor_value);
                break;
            }
            case OP_ROW_GET: {
                if (vm->ip + 2 > end) return INTERPRET_RUNTIME_ERROR;
                uint16_t idx = read_u16(vm->ip);
                vm->ip += 2;
                if (idx >= (uint16_t)vm->chunk->constants_count) return INTERPRET_RUNTIME_ERROR;
                Value name_value = vm->chunk->constants[idx];
                if (name_value.type != VAL_STRING || name_value.as.as_string == NULL) {
                    set_runtime_error(vm, "Invalid field access");
                    THROW(vm);
                }
                Value row_value;
                if (!pop(vm, &row_value)) return INTERPRET_RUNTIME_ERROR;
                if (row_value.type != VAL_ROW || row_value.as.as_row_handle == NULL) {
                    value_release(row_value);
                    set_runtime_error(vm, "Cannot access field on non-row value");
                    THROW(vm);
                }
                Value field_value = row_obj_get_field((RowObj*)row_value.as.as_row_handle,
                                                      name_value.as.as_string);
                if (!push(vm, field_value)) {
                    value_release(field_value);
                    value_release(row_value);
                    return INTERPRET_RUNTIME_ERROR;
                }
                value_release(row_value);
                break;
            }
            case OP_SQL_GET_COLUMN: {
                if (vm->ip + 2 > end) return INTERPRET_RUNTIME_ERROR;
                uint16_t idx = read_u16(vm->ip);
                vm->ip += 2;
                Value value;
                if (vm->driver != NULL) {
                    if (!vm->driver->row_get_column(vm->driver, vm->row_handle, (int)idx, &value)) {
                        set_runtime_error_from_driver_sql(vm, "Column access failed");
                        THROW(vm);
                    }
                } else {
                    Row* row = (Row*)vm->row_handle;
                    if (row == NULL || idx >= (uint16_t)row->field_count) {
                        set_runtime_error_sql(vm, "Invalid column index");
                        THROW(vm);
                    }
                    value = value_from_cell(row->fields[idx].value, value_int(0));
                }
                if (!push(vm, value)) return INTERPRET_RUNTIME_ERROR;
                break;
            }
            case OP_SQL_TO_ARRAY: {
                ArrayObj* array = array_new();
                if (array == NULL) {
                    set_runtime_error_sql(vm, "out of memory");
                    THROW(vm);
                }
                if (vm->driver != NULL) {
                    int column_count = vm->driver->result_column_count(vm->driver, vm->result_handle);
                    while (1) {
                        void* row_handle = NULL;
                        int has_row = vm->driver->result_next(vm->driver, vm->result_handle, &row_handle);
                        if (!has_row) break;
                        RowObj* row_obj = row_obj_new(column_count);
                        if (row_obj == NULL) {
                            set_runtime_error_sql(vm, "out of memory");
                            value_release(value_array(array));
                            return INTERPRET_RUNTIME_ERROR;
                        }
                        for (int i = 0; i < column_count; i++) {
                            const char* name = vm->driver->result_column_name(vm->driver, vm->result_handle, i);
                            Value col_value;
                            if (!vm->driver->row_get_column(vm->driver, row_handle, i, &col_value)) {
                                set_runtime_error_from_driver_sql(vm, "Column access failed");
                                row_obj_free(row_obj);
                                value_release(value_array(array));
                                return INTERPRET_RUNTIME_ERROR;
                            }
                            row_obj_set_column(row_obj, i, name, col_value);
                            value_release(col_value);
                        }
                        Value row_value = value_row(row_obj);
                        if (!array_append(array, row_value)) {
                            value_release(row_value);
                            value_release(value_array(array));
                            set_runtime_error_sql(vm, "out of memory");
                            THROW(vm);
                        }
                        value_release(row_value);
                    }
                    vm->driver->result_free(vm->driver, vm->result_handle);
                } else {
                    Context* ctx = vm->context;
                    if (ctx == NULL || ctx->pager == NULL) {
                        set_runtime_error_sql(vm, "No database context");
                        value_release(value_array(array));
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    Result* res = (Result*)vm->result_handle;
                    while (1) {
                        Row* row = result_next(res);
                        if (row == NULL) break;
                        RowObj* row_obj = row_obj_new(row->field_count);
                        if (row_obj == NULL) {
                            set_runtime_error_sql(vm, "out of memory");
                            value_release(value_array(array));
                            return INTERPRET_RUNTIME_ERROR;
                        }
                        for (int i = 0; i < row->field_count; i++) {
                            Value col_value =
                                value_from_cell(row->fields[i].value, value_int(0));
                            row_obj_set_column(row_obj, i, row->fields[i].name, col_value);
                            value_release(col_value);
                        }
                        Value row_value = value_row(row_obj);
                        if (!array_append(array, row_value)) {
                            value_release(row_value);
                            value_release(value_array(array));
                            set_runtime_error_sql(vm, "out of memory");
                            THROW(vm);
                        }
                        value_release(row_value);
                    }
                    result_free(res);
                }
                vm->result_handle = NULL;
                vm->row_handle = NULL;
                if (!push(vm, value_array(array))) {
                    value_release(value_array(array));
                    return INTERPRET_RUNTIME_ERROR;
                }
                break;
            }
            case OP_STRUCT_BUILD: {
                if (vm->ip + 2 > end) return INTERPRET_RUNTIME_ERROR;
                uint16_t idx = read_u16(vm->ip);
                vm->ip += 2;
                if (idx >= (uint16_t)vm->chunk->constants_count) return INTERPRET_RUNTIME_ERROR;
                Value schema_value = vm->chunk->constants[idx];
                if (schema_value.type != VAL_STRING || schema_value.as.as_string == NULL) {
                    set_runtime_error(vm, "Invalid struct schema");
                    THROW(vm);
                }
                const char* schema = schema_value.as.as_string;
                int field_count = 0;
                for (const char* p = schema; *p != '\0'; p++) {
                    if (*p == ',') field_count++;
                }
                if (field_count > 0) field_count++;
                else if (schema[0] != '\0') field_count = 1;

                RowObj* row = row_obj_new(field_count);
                if (row == NULL) {
                    set_runtime_error(vm, "out of memory");
                    THROW(vm);
                }

                Value* fields = malloc(sizeof(Value) * (size_t)(field_count > 0 ? field_count : 1));
                if (fields == NULL && field_count > 0) {
                    row_obj_free(row);
                    set_runtime_error(vm, "out of memory");
                    THROW(vm);
                }
                for (int i = field_count - 1; i >= 0; i--) {
                    if (!pop(vm, &fields[i])) {
                        for (int j = i + 1; j < field_count; j++) value_release(fields[j]);
                        free(fields);
                        row_obj_free(row);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                }

                const char* p = schema;
                int i = 0;
                while (*p != '\0') {
                    const char* start = p;
                    while (*p != '\0' && *p != ',') p++;
                    size_t len = (size_t)(p - start);
                    char* name = malloc(len + 1);
                    if (name == NULL) {
                        for (int j = 0; j < field_count; j++) value_release(fields[j]);
                        free(fields);
                        row_obj_free(row);
                        set_runtime_error(vm, "out of memory");
                        THROW(vm);
                    }
                    memcpy(name, start, len);
                    name[len] = '\0';
                    row_obj_set_column(row, i, name, fields[i]);
                    free(name);
                    i++;
                    if (*p == ',') p++;
                }

                free(fields);
                Value result = value_row(row);
                if (!push(vm, result)) {
                    value_release(result);
                    return INTERPRET_RUNTIME_ERROR;
                }
                break;
            }
            case OP_RUNTIME_ERROR: {
                if (vm->ip + 2 > end) return INTERPRET_RUNTIME_ERROR;
                uint16_t idx = read_u16(vm->ip);
                vm->ip += 2;
                if (idx >= (uint16_t)vm->chunk->constants_count) return INTERPRET_RUNTIME_ERROR;
                Value msg_value = vm->chunk->constants[idx];
                if (msg_value.type != VAL_STRING || msg_value.as.as_string == NULL) {
                    set_runtime_error(vm, "Runtime error");
                } else {
                    set_runtime_error(vm, msg_value.as.as_string);
                }
                THROW(vm);
                break;
            }
            case OP_RAISE: {
                if (vm->ip + 4 > end) return INTERPRET_RUNTIME_ERROR;
                uint16_t msg_idx = read_u16(vm->ip);
                vm->ip += 2;
                int16_t code = (int16_t)read_u16(vm->ip);
                vm->ip += 2;
                if (msg_idx >= (uint16_t)vm->chunk->constants_count) return INTERPRET_RUNTIME_ERROR;
                Value msg_value = vm->chunk->constants[msg_idx];
                if (msg_value.type != VAL_STRING || msg_value.as.as_string == NULL) {
                    set_runtime_error_ex(vm, "Runtime error", code);
                } else {
                    set_runtime_error_ex(vm, msg_value.as.as_string, code);
                }
                THROW(vm);
                break;
            }
            case OP_TRY: {
                if (vm->ip + 4 > end) return INTERPRET_RUNTIME_ERROR;
                uint16_t offset = read_u16(vm->ip);
                vm->ip += 2;
                uint16_t local_count = read_u16(vm->ip);
                vm->ip += 2;
                if (vm->try_count >= TRY_MAX) {
                    set_runtime_error(vm, "Too many nested try blocks");
                    THROW(vm);
                }
                /* Push a placeholder for the catch variable so its slot exists
                 * on the stack throughout the try block. vm_catch will replace it
                 * with the real error message; OP_END_TRY will pop it on the
                 * non-throwing path. */
                if (!push(vm, value_int(0))) {
                    set_runtime_error(vm, "Stack overflow");
                    THROW(vm);
                }
                TryFrame* tf = &vm->try_frames[vm->try_count++];
                tf->catch_ip = vm->ip + (int16_t)offset;
                tf->frame_count = vm->frame_count;
                tf->frame_base = vm->frame_base;
                tf->local_count = (int)local_count;
                tf->stack_top = vm->stack_top;
                break;
            }
            case OP_END_TRY: {
                if (vm->ip + 2 > end) return INTERPRET_RUNTIME_ERROR;
                int16_t offset = (int16_t)read_u16(vm->ip);
                vm->ip += 2;
                if (vm->try_count <= 0) {
                    set_runtime_error(vm, "END_TRY without try");
                    THROW(vm);
                }
                vm->try_count--;
                /* Pop the catch-variable placeholder that OP_TRY pushed. */
                Value placeholder;
                if (!pop(vm, &placeholder)) {
                    set_runtime_error(vm, "Stack underflow");
                    THROW(vm);
                }
                value_release(placeholder);
                uint8_t* target = vm->ip + offset;
                if (target < vm->chunk->code || target > end) {
                    set_runtime_error(vm, "Invalid jump target");
                    THROW(vm);
                }
                vm->ip = target;
                break;
            }
            default:
                set_runtime_error(vm, "Unknown opcode");
                THROW(vm);
        }
    }
}

static void vm_free_child(VM* vm) {
    if (vm == NULL) return;
    for (Value* p = vm->stack; p < vm->stack_top; p++) {
        value_release(*p);
    }
    for (int i = 0; i < vm->repl_local_count; i++) {
        value_release(vm->repl_locals[i]);
    }
    for (int i = 0; i < vm->global_count; i++) {
        value_release(vm->globals[i]);
    }
    if (vm->driver != NULL && vm->result_handle != NULL) {
        vm->driver->result_free(vm->driver, vm->result_handle);
    } else if (vm->driver == NULL) {
        result_free((Result*)vm->result_handle);
    }
    for (int i = 0; i < DBMS_SQL_MAX_CURSORS; i++) {
        if (vm->dbms_sql_cursors[i].active) {
            dbms_sql_cursor_close_internal(vm, &vm->dbms_sql_cursors[i]);
        }
    }
    free(vm->stack);
    free(vm->frames);
    free(vm->return_ips);
    free(vm->out_frames);
    free(vm->repl_locals);
    free(vm);
}

static int vm_call_autonomous(VM* parent, uint16_t target, uint8_t arg_count,
                              int out_count, int* out_positions, int* out_slots,
                              Value* out_result) {
    if (parent->driver == NULL || !parent->driver->is_sqlite) {
        return -1; /* fall back to normal call path */
    }
    VM* child = vm_init();
    if (child == NULL) {
        set_runtime_error(parent, "Out of memory");
        return 0;
    }
    child->is_child = 1;
    child->parent = parent;
    child->entry_offset = target;
    DBDriver auto_driver;
    parent->driver->init(&auto_driver);
    if (!auto_driver.open(&auto_driver, parent->driver->connection_string)) {
        snprintf(parent->error_message, sizeof(parent->error_message), "%s", auto_driver.error_message);
        vm_free_child(child);
        return 0;
    }
    if (!auto_driver.begin(&auto_driver)) {
        set_runtime_error_from_driver(parent, "Autonomous BEGIN failed");
        auto_driver.close(&auto_driver);
        vm_free_child(child);
        return 0;
    }
    child->global_count = parent->global_count;
    for (int i = 0; i < parent->global_count; i++) {
        child->globals[i] = parent->globals[i];
        value_retain(child->globals[i]);
    }
    for (int i = 0; i < arg_count; i++) {
        Value arg = parent->stack_top[-arg_count + i];
        value_retain(arg);
        if (!push(child, arg)) {
            value_release(arg);
            set_runtime_error(parent, "Stack overflow");
            auto_driver.close(&auto_driver);
            vm_free_child(child);
            return 0;
        }
    }
    child->chunk = parent->chunk;
    child->ip = child->chunk->code + target;
    child->frame_base = child->stack;
    child->frame_count = 0;
    child->driver = &auto_driver;
    child->context = parent->context;
    InterpretResult run_result = vm_run(child, child->chunk->code + child->chunk->count);
    if (run_result == INTERPRET_OK) {
        Value result;
        if (!pop(child, &result)) {
            set_runtime_error(parent, "Autonomous call returned no value");
            auto_driver.close(&auto_driver);
            vm_free_child(child);
            return 0;
        }
        if (!auto_driver.commit(&auto_driver)) {
            set_runtime_error_from_driver(parent, "Autonomous commit failed");
            auto_driver.close(&auto_driver);
            vm_free_child(child);
            return 0;
        }
        for (int i = 0; i < out_count; i++) {
            int pos = out_positions[i];
            int slot = out_slots[i];
            if (pos < 0 || pos >= arg_count) {
                value_release(result);
                auto_driver.close(&auto_driver);
                vm_free_child(child);
                set_runtime_error(parent, "Invalid OUT parameter position");
                return 0;
            }
            if (slot < 0 || slot >= (parent->stack_top - parent->frame_base - arg_count)) {
                value_release(result);
                auto_driver.close(&auto_driver);
                vm_free_child(child);
                set_runtime_error(parent, "Invalid OUT parameter slot");
                return 0;
            }
            value_release(parent->frame_base[slot]);
            parent->frame_base[slot] = child->frame_base[pos];
            value_retain(parent->frame_base[slot]);
        }
        *out_result = result;
        auto_driver.close(&auto_driver);
        vm_free_child(child);
        return 1;
    }
    snprintf(parent->error_message, sizeof(parent->error_message), "%s", child->error_message);
    auto_driver.rollback(&auto_driver);
    auto_driver.close(&auto_driver);
    vm_free_child(child);
    return 0;
}

InterpretResult vm_interpret(VM* vm, Chunk* chunk) {
    vm->chunk = chunk;
    vm->ip = chunk->code;
    vm->error_message[0] = '\0';
    vm->sql_rowcount = 0;
    vm->try_count = 0;
    vm->local_count = 0;
    vm->capture_base = 1;
    vm->repl_local_count = 0;
    return vm_run(vm, chunk->code + chunk->count);
}

InterpretResult vm_interpret_from(VM* vm, Chunk* chunk, int offset) {
    /* Incremental (REPL) fragment entry: like vm_interpret but starts at an
       offset inside a persistent chunk. Frame state is intentionally NOT
       reset (mirrors vm_interpret, which also leaves frames alone), and the
       repl-local mirror is NOT cleared so captures accumulate across
       fragments. capture_base=0 because the fragment body runs at
       frame_count 0 (there is no bootstrap frame). */
    vm->chunk = chunk;
    if (offset < 0 || offset >= chunk->count) {
        set_runtime_error(vm, "Invalid fragment offset");
        return INTERPRET_RUNTIME_ERROR;
    }
    vm->ip = chunk->code + offset;
    vm->error_message[0] = '\0';
    vm->sql_rowcount = 0;
    vm->try_count = 0;
    vm->local_count = 0;
    vm->capture_base = 0;
    return vm_run(vm, chunk->code + chunk->count);
}

void vm_repl_locals_clear(VM* vm) {
    /* Emulates the mirror reset vm_interpret performs per run without
       releasing the mirrored values (matching that reset exactly). Used by
       the REPL's stuck-state emulation. */
    if (vm == NULL) return;
    vm->repl_local_count = 0;
}
