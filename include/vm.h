#ifndef MYDB_VM_H
#define MYDB_VM_H

#include "compiler.h"
#include "sql_engine.h"

struct Context;

/* Hard upper bound for the dynamically grown value stack and call-frame
   arrays, in slots/frames. Growth doubles from a small initial capacity
   until this cap; exceeding it fails cleanly with a stack-overflow error
   instead of runaway memory use (catches unbounded recursion). */
#define STACK_MAX (1 << 20)

typedef struct VM VM;

typedef enum {
    INTERPRET_OK,
    INTERPRET_COMPILE_ERROR,
    INTERPRET_RUNTIME_ERROR
} InterpretResult;

VM*             vm_init(void);
void            vm_free(VM* vm);
void            vm_set_context(VM* vm, struct Context* ctx);
void            vm_set_driver(VM* vm, struct DBDriver* driver);
struct DBDriver* vm_get_driver(VM* vm);
void            vm_set_sql_rowcount(VM* vm, int rowcount);
InterpretResult vm_interpret(VM* vm, Chunk* chunk);
/* Incremental (REPL) entry: run one appended fragment starting at offset.
   Unlike vm_interpret this does not reset the repl-local mirror (captures
   accumulate across fragments) and sets capture_base=0 (the fragment body
   runs at frame_count 0). Frame/stack state is left alone, exactly like
   vm_interpret. */
InterpretResult vm_interpret_from(VM* vm, Chunk* chunk, int offset);
/* Reset the repl-local mirror without releasing values (used by the REPL to
   emulate vm_interpret's per-run mirror reset in its stuck state). */
void            vm_repl_locals_clear(VM* vm);
const char*     vm_get_error(VM* vm);
void            vm_set_error(VM* vm, const char* message);
void            vm_set_error_with_code(VM* vm, const char* message, int code);
int             vm_get_sql_rowcount(VM* vm);
int             vm_get_sql_found(VM* vm);
int             vm_get_sql_notfound(VM* vm);
int             vm_get_sql_code(VM* vm);
const char*     vm_get_sql_errm(VM* vm);
Value           vm_pop(VM* vm);
int             vm_stack_depth(VM* vm);

/* dbms_output buffer */
void            vm_dbms_output_enable(VM* vm, int limit);
void            vm_dbms_output_put_line(VM* vm, Value line);
void            vm_dbms_output_disable(VM* vm);
Value           vm_dbms_output_get_lines(VM* vm);

/* utl_file handles */
int             vm_utl_file_fopen(VM* vm, const char* path, const char* mode);
int             vm_utl_file_get_line(VM* vm, int handle, Value* out);
int             vm_utl_file_put_line(VM* vm, int handle, const char* text);
int             vm_utl_file_fseek(VM* vm, int handle, int offset);
int             vm_utl_file_fflush(VM* vm, int handle);
int             vm_utl_file_fclose(VM* vm, int handle);
int             vm_utl_file_mkdir(const char* path);
int             vm_utl_file_remove(const char* path);

/* dbms_sql helpers */
int             vm_dbms_sql_execute(VM* vm, const char* sql);
Value           vm_dbms_sql_query(VM* vm, const char* sql);

/* dbms_sql cursor API (Phase 12 Task 4) */
int             vm_dbms_sql_open_cursor(VM* vm);
int             vm_dbms_sql_parse(VM* vm, int handle, const char* sql);
int             vm_dbms_sql_bind_variable(VM* vm, int handle, const char* name, Value value);
int             vm_dbms_sql_cursor_execute(VM* vm, int handle);
Value           vm_dbms_sql_fetch_rows(VM* vm, int handle, int count);
Value           vm_dbms_sql_column_value(VM* vm, int handle, int column);
int             vm_dbms_sql_close_cursor(VM* vm, int handle);

/* Dynamic SQL execution with runtime trigger firing and DROP TRIGGER
   interception (execute_immediate, dbms_sql.execute). */
int             vm_dynamic_exec(VM* vm, const char* sql);

/* sequences */
int             vm_sequence_create(VM* vm, const char* name, int64_t start, int64_t increment);
int             vm_sequence_nextval(VM* vm, const char* name, int64_t* out);
int             vm_sequence_currval(VM* vm, const char* name, int64_t* out);
int             vm_sequence_drop(VM* vm, const char* name);

Value           vm_stack_get(VM* vm, int index);
int             vm_local_count(VM* vm);
Value           vm_local_get(VM* vm, int index);

#endif
