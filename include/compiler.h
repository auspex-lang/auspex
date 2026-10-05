#ifndef MYDB_COMPILER_H
#define MYDB_COMPILER_H

#include <stddef.h>
#include <stdint.h>

typedef struct ArrayObj ArrayObj;
typedef struct MapObj MapObj;
typedef struct RowObj RowObj;
typedef struct CursorObj CursorObj;
typedef struct DBDriver DBDriver;

#define AUSPEX_CC_MAX_FLAGS 64
#define AUSPEX_CC_FLAG_NAME_MAX 64

typedef struct {
    const char* const* conditional_flags;
    int conditional_flag_count;
    /* Number of lines that precede the user's own source in the buffer handed
       to the compiler (built-in and stored declarations the driver prepends).
       The compiler numbers lines so the user's first line is 1: everything in
       that preamble gets a non-positive line, and is reported as coming from
       built-in or stored declarations rather than at a shifted line of the
       user's file. 0 (the default) means no preamble. Applies to the main
       source only, never to imported modules. */
    int line_offset;
} CompileOptions;

typedef enum {
    OBJ_STRING,
    OBJ_ARRAY,
    OBJ_MAP,
    OBJ_ROW,
    OBJ_CURSOR
} ObjType;

typedef struct Obj {
    ObjType type;
    int ref_count;
    /* Cycle-collector bookkeeping (src/value.c): gc_index is the object's slot
       in the container registry (-1 when not registered: strings and cursors,
       which hold no container references), gc_mark is the trace mark bit. */
    int gc_index;
    unsigned char gc_mark;
} Obj;

struct CursorObj {
    Obj obj;
    DBDriver* driver;
    void* result_handle;
    void* row_handle;
    int is_open;
    int row_count;
    int found;
};

typedef enum {
    OP_CONST,
    OP_GET_LOCAL,
    OP_SET_LOCAL,
    OP_GET_GLOBAL,
    OP_SET_GLOBAL,
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_EQ,
    OP_LT,
    OP_GT,
    OP_NEGATE,
    OP_NOT,
    OP_POP,
    OP_DUP,
    OP_JZ,
    OP_JMP,
    OP_SQL,
    OP_SQL_NEXT,
    OP_GET_FIELD,
    OP_CALL,
    OP_CALL_OUT,
    OP_CALL_AUTONOMOUS,
    OP_CALL_OUT_AUTONOMOUS,
    OP_RETURN,
    OP_PRINT,
    OP_ARRAY_BUILD,
    OP_INDEX_GET,
    OP_INDEX_SET,
    OP_NATIVE_CALL,
    OP_SQL_EXEC,
    OP_SQL_BIND_INT,
    OP_SQL_BIND_FLOAT,
    OP_SQL_BIND_STRING,
    OP_SQL_BEGIN,
    OP_SQL_COMMIT,
    OP_SQL_ROLLBACK,
    OP_SQL_SAVEPOINT,
    OP_SQL_ROLLBACK_TO,
    OP_SQL_RELEASE_SAVEPOINT,
    OP_ROW_GET,
    OP_SQL_GET_COLUMN,
    OP_SQL_TO_ARRAY,
    OP_STRUCT_BUILD,
    OP_MAP_BUILD,
    OP_TRY,
    OP_END_TRY,
    OP_RUNTIME_ERROR,
    OP_RAISE,
    OP_CURSOR_OPEN,
    OP_CURSOR_FETCH,
    OP_CURSOR_CLOSE,
    OP_CURSOR_ATTR,
    OP_DROP_TRIGGER,
    /* Wide local-slot variants used when a frame has more than 256 locals
       (slot index no longer fits in a byte). */
    OP_GET_LOCAL16,
    OP_SET_LOCAL16
} OpCode;

typedef enum {
    VAL_INT,
    VAL_FLOAT,
    VAL_STRING,
    VAL_BOOL,
    VAL_DATE,
    VAL_TIMESTAMP,
    VAL_ARRAY,
    VAL_MAP,
    VAL_ROW,
    VAL_CURSOR,
    /* SQL NULL. Kept last so the numeric tags of the earlier types (which the
       custom engine persists to disk) stay stable. */
    VAL_NULL
} ValueType;

typedef struct {
    int type;
    union {
        int       as_int;
        double    as_float;
        char*     as_string;
        void*     as_row_handle;
        ArrayObj* as_array;
        MapObj*   as_map;
        CursorObj* as_cursor;
    } as;
} Value;

struct MapObj {
    Obj obj;
    Value* keys;
    Value* values;
    int count;
    int capacity;
};

/* Runtime trigger registry entry. Filled by the code generator for every
   active trigger declaration so dynamic SQL (execute_immediate, dbms_sql)
   can fire triggers at runtime. timing/event use the TRIGGER_* constants
   from ast.h; offset is the bytecode address of the hidden
   __trigger_<name> proc, or -1 once the trigger has been dropped. */
typedef struct {
    char* name;
    char* table;
    int   timing;
    int   event;
    int   offset;
    int   for_each_row;  /* 1 = row-level: fired per row via the driver hook
                            with :new/:old row arguments; never fired by the
                            statement-level call paths */
} ChunkTrigger;

typedef struct {
    uint8_t* code;
    int      count;
    int      capacity;

    int* lines;
    int  lines_count;
    int  lines_capacity;

    int* columns;
    int  columns_count;
    int  columns_capacity;

    Value* constants;
    int    constants_count;
    int    constants_capacity;

    ChunkTrigger* triggers;
    int           trigger_count;
    int           trigger_capacity;

    const char* source_path;
} Chunk;

void init_chunk(Chunk* chunk);
void free_chunk(Chunk* chunk);
void write_chunk(Chunk* chunk, uint8_t byte);
void write_chunk_line(Chunk* chunk, uint8_t byte, int line, int column);
int  add_constant(Chunk* chunk, Value value);

/* Runtime trigger registry. chunk_add_trigger copies name/table.
   chunk_remove_trigger disables the named trigger (offset = -1). */
void chunk_add_trigger(Chunk* chunk, const char* name, int timing, int event,
                       const char* table, int offset, int for_each_row);
void chunk_remove_trigger(Chunk* chunk, const char* name);

void   write_chunk_u16(Chunk* chunk, uint16_t value);
void   write_chunk_u16_line(Chunk* chunk, uint16_t value, int line, int column);
uint16_t read_u16(const uint8_t* bytes);

Value value_int(int v);
Value value_null(void);
Value value_float(double v);
Value value_string(char* s);
Value value_bool(int v);
Value value_date(char* s);
Value value_timestamp(char* s);
Value value_array(ArrayObj* array);
Value value_map(MapObj* map);
Value value_row(RowObj* row);
Value value_cursor(CursorObj* cursor);
CursorObj* cursor_obj_new(DBDriver* driver);
void value_print(Value value);

void value_retain(Value v);
void value_release(Value v);
int  value_ref_count(Value v);

/* ---------------------------------------------------------------------------
 * Cycle collector (implemented in src/value.c, driven by src/vm.c).
 *
 * Every ArrayObj/MapObj/RowObj registers itself on allocation and unregisters
 * when its refcount reaches zero. Reference counting alone cannot reclaim
 * reference cycles, so the VM periodically traces all roots (value stack,
 * REPL locals, globals, chunk constants, SQL params, dbms_output buffer,
 * dbms_sql binds), marks reachable containers, and sweeps the unreachable
 * ones: internal edges are broken (child refcounts decremented, slots
 * cleared) and string children released exactly once, then zero-refcount
 * containers are freed through their normal destructors. Cursors and strings
 * are never registered (they hold no container references).
 *
 * vm_gc_collect() may only run in the top-level VM between bytecode
 * instructions; child VMs (trigger firing, autonomous transactions) never
 * collect. vm_free() finishes with a roots-free sweep so teardown stays
 * leak-free even with cyclic garbage still registered.
 * ------------------------------------------------------------------------- */
void gc_trace_value(Value v);            /* mark one root value + drain */
int  gc_begin_collection(void);          /* pre-grow trace/work stacks; 0 = skip */
int  gc_sweep_unreachable(void);         /* sweep unmarked containers; returns freed count */
long gc_container_allocations(void);     /* container allocs since last reset */
void gc_reset_container_allocations(void);
int  gc_container_count(void);           /* live registered containers */

ArrayObj* array_new(void);
void      array_free(ArrayObj* array);
int       array_append(ArrayObj* array, Value value);
Value     array_get(ArrayObj* array, int index);
void      array_set(ArrayObj* array, int index, Value value);
int       array_length(ArrayObj* array);
int       array_extend(ArrayObj* array, int count);
int       array_trim(ArrayObj* array, int count);

MapObj*   map_new(void);
void      map_free(MapObj* map);
int       map_set(MapObj* map, Value key, Value value);
int       map_get(MapObj* map, Value key, Value* out);
int       map_delete(MapObj* map, Value key);
int       map_count(MapObj* map);
int       map_first_key(MapObj* map, Value* out);
int       map_last_key(MapObj* map, Value* out);
int       map_next_key(MapObj* map, Value key, Value* out);
int       map_prior_key(MapObj* map, Value key, Value* out);

RowObj* row_obj_new(int column_count);
void    row_obj_free(RowObj* row);
void    row_obj_set_column(RowObj* row, int index, const char* name, Value value);
Value   row_obj_get_field(RowObj* row, const char* name);
int     row_obj_set_field(RowObj* row, const char* name, Value value);

Value value_add(Value a, Value b);
Value value_sub(Value a, Value b);
Value value_mul(Value a, Value b);
Value value_div(Value a, Value b);
Value value_eq(Value a, Value b);
Value value_lt(Value a, Value b);
Value value_gt(Value a, Value b);
int   value_is_truthy(Value value);

struct Context;

int compile(const char* source, Chunk* chunk, char* error, size_t error_size);
int compile_with_context(const char* source, Chunk* chunk, char* error, size_t error_size, struct Context* ctx);
int compile_with_path(const char* source, Chunk* chunk, const char* path, char* error, size_t error_size);
int compile_with_context_and_path(const char* source, Chunk* chunk, const char* path, char* error, size_t error_size, struct Context* ctx);
int compile_with_options(const char* source, Chunk* chunk, const char* path,
                         char* error, size_t error_size, struct Context* ctx,
                         const CompileOptions* options);

/* Conditional-compilation pass run by compile_with_options before parsing.
 * Returns a malloc'd copy of source with directive lines and excluded
 * regions blanked (newlines kept, same length), or NULL with a message in
 * error_buf. */
char* cc_preprocess(const char* source, const char* source_path,
                    const CompileOptions* options,
                    char* error_buf, size_t error_size);

/* ---------------------------------------------------------------------------
 * Incremental (REPL) compilation — Phase 13 #37.
 *
 * A ReplCompiler persists procedure/global/trigger tables and the top-level
 * main-body local table across compile calls, so each REPL input compiles
 * only itself and appends to one growing Chunk (executed via
 * vm_interpret_from at the returned offset). Statement/expression inputs are
 * wrapped by the caller as "proc main() -> int { <main body so far> ... }";
 * definition inputs (proc/package) are compiled standalone. On failure all
 * internal tables roll back to the pre-fragment state and the caller gets an
 * error string identical to whole-program compilation (the REPL prepends the
 * appropriate line prefixes).
 * ------------------------------------------------------------------------- */
typedef struct ReplCompiler ReplCompiler;

ReplCompiler* repl_compiler_create(void);
void          repl_compiler_free(ReplCompiler* rc);
int           repl_compiler_compile(ReplCompiler* rc, const char* source,
                                    int is_def_fragment, Chunk* chunk,
                                    char* error, size_t error_size,
                                    struct Context* ctx);
int           repl_compiler_exec_offset(const ReplCompiler* rc);
/* The session's persistent top-level variables, by slot: the names .vars
   pairs with the VM's REPL locals. */
int           repl_compiler_local_count(const ReplCompiler* rc);
const char*   repl_compiler_local_name(const ReplCompiler* rc, int slot);

#endif
