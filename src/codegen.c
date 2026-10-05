#include <libgen.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "compiler.h"
#include "diagnostics.h"
#include "natives.h"
#include "os.h"
#include "parser.h"
#include "trigger.h"
#include "typecheck.h"

#define MAX_PROCS 256
#define MAX_CALL_PATCHES 1024
#define MAX_MODULES 64
#define MAX_LOADING 64
#define MAX_LOOP_NESTING 64
#define MAX_BREAK_PATCHES 256
#define MAX_GLOBALS 256
#define MAX_EXCEPTIONS 64
#define MAX_TRIGGERS 64
/* One slot per compiled file (main plus every distinct imported module)
   that declares package-level state needing an initializer. */
#define MAX_MODULE_INITS (MAX_MODULES + 1)
/* Initial capacity of the per-procedure locals table; doubles on demand. */
#define LOCALS_INITIAL_CAPACITY 256
#define CURSOR_QUERIES_INITIAL_CAPACITY 16

typedef struct {
    const char* name;
    int length;
    int depth;
    Type* type;
    int is_sql_row; /* `for x in select` iterator: fields come from the current row */
} Local;

typedef struct {
    int continue_target;
    int local_count_at_start;
    int continue_local_count;
    int break_patches[MAX_BREAK_PATCHES];
    int break_count;
    int continue_patches[MAX_BREAK_PATCHES];
    int continue_count;
} LoopContext;

typedef struct {
    const char* name;
    int offset;
    Type* return_type;
    Type** param_types;
    ParamMode* param_modes;
    int param_count;
    int autonomous_transaction;
    /* Set for a package member declared in its package's spec (interface);
       clear for a body-only helper. Meaningless outside package members. */
    int is_public;
} ProcEntry;

typedef struct {
    int offset;
    const char* name;
} CallPatch;

typedef struct {
    const char* name;
    int length;
    const char* query;
    Expr** params;   /* ?var placeholders of query, owned by the declaration's AST */
    int param_count;
} CursorQuery;

typedef struct {
    const char* name;
    int code;
} ExceptionEntry;

typedef struct {
    char* proc_name;
    int   timing;
    int   event;
    char* table;
    int   for_each_row; /* 1 = row-level: fires per row via the driver hook */
    int   dropped;  /* set by `drop trigger`: suppresses compile-time firing */
} TriggerEntry;

typedef struct {
    Chunk* chunk;
    /* Per-procedure locals table: heap array, grows by doubling (locals
       beyond the old MAX_LOCALS = 256 fixed ceiling are now supported). */
    Local* locals;
    int local_count;
    int local_capacity;
    int scope_depth;
    ProcEntry procs[MAX_PROCS];
    int proc_count;
    CallPatch patches[MAX_CALL_PATCHES];
    int patch_count;
    int had_error;
    char error_message[256];
    int entry_index;
    int entry_patch;
    char* loaded_paths[MAX_MODULES];
    int loaded_count;
    const char* loading_stack[MAX_LOADING];
    int loading_count;
    char* current_path;
    const char* source_path;
    struct Context* ctx;
    LoopContext loops[MAX_LOOP_NESTING];
    int loop_count;
    int current_line;
    int current_column;
    CursorQuery* cursor_queries;
    int cursor_query_count;
    int cursor_query_capacity;
    const char* global_names[MAX_GLOBALS];
    int global_count;
    const char* current_package;
    int entry_jump_patch;
    int current_proc_autonomous;
    ExceptionEntry exceptions[MAX_EXCEPTIONS];
    int exception_count;
    TriggerEntry triggers[MAX_TRIGGERS];
    int trigger_count;
    const StructDecl* current_struct;
    /* Hidden 0-arg procs, one per compiled file that declares package-level
       state, in load order (deepest imports first, the main file last).
       Called in this order from the bootstrap sequence before main() runs. */
    char* module_init_procs[MAX_MODULE_INITS];
    int module_init_count;
    /* First index to use when naming the next __module_init_N proc. Always 0
       for whole-program compiles; the REPL's incremental compiler keeps a
       session-wide total here so each fragment's init proc has a unique name
       across the persistent procedure table. */
    int module_init_base;
    const CompileOptions* options;
    /* REPL only (record_call_sites set): the operand offset of every call
       emitted this session, so redefining a proc can re-point the callers
       already bound to its old body. Unused by whole-program compiles. */
    int record_call_sites;
    int* call_sites;
    int call_site_count;
    int call_site_capacity;
} Compiler;

static int add_proc_entry(Compiler* compiler, const char* name, int offset,
                          Type* return_type, Type** param_types,
                          ParamMode* param_modes, int param_count) {
    if (compiler->proc_count >= MAX_PROCS) return -3;
    for (int i = 0; i < compiler->proc_count; i++) {
        if (strcmp(compiler->procs[i].name, name) == 0) return -2;
    }
    char* name_copy = malloc(strlen(name) + 1);
    if (name_copy == NULL) return -1;
    strcpy(name_copy, name);
    Type* rt_copy = type_copy(return_type);
    if (rt_copy == NULL && return_type != NULL) { free(name_copy); return -1; }
    Type** pt_copy = NULL;
    ParamMode* pm_copy = NULL;
    if (param_count > 0) {
        pt_copy = malloc(sizeof(Type*) * (size_t)param_count);
        if (pt_copy == NULL) { free(name_copy); type_free(rt_copy); return -1; }
        pm_copy = malloc(sizeof(ParamMode) * (size_t)param_count);
        if (pm_copy == NULL) {
            free(pt_copy);
            free(name_copy);
            type_free(rt_copy);
            return -1;
        }
        for (int i = 0; i < param_count; i++) {
            pt_copy[i] = type_copy(param_types[i]);
            if (pt_copy[i] == NULL && param_types[i] != NULL) {
                for (int j = 0; j < i; j++) type_free(pt_copy[j]);
                free(pt_copy);
                free(pm_copy);
                free(name_copy);
                type_free(rt_copy);
                return -1;
            }
            pm_copy[i] = param_modes[i];
        }
    }
    compiler->procs[compiler->proc_count].name = name_copy;
    compiler->procs[compiler->proc_count].offset = offset;
    compiler->procs[compiler->proc_count].return_type = rt_copy;
    compiler->procs[compiler->proc_count].param_types = pt_copy;
    compiler->procs[compiler->proc_count].param_modes = pm_copy;
    compiler->procs[compiler->proc_count].param_count = param_count;
    compiler->procs[compiler->proc_count].autonomous_transaction = 0;
    compiler->procs[compiler->proc_count].is_public = 0;
    return compiler->proc_count++;
}

static void free_proc_entries(Compiler* compiler) {
    for (int i = 0; i < compiler->proc_count; i++) {
        free((void*)compiler->procs[i].name);
        type_free(compiler->procs[i].return_type);
        for (int p = 0; p < compiler->procs[i].param_count; p++) {
            type_free(compiler->procs[i].param_types[p]);
        }
        free(compiler->procs[i].param_types);
        free(compiler->procs[i].param_modes);
    }
}

static int find_exception_code(Compiler* compiler, const char* name) {
    for (int i = 0; i < compiler->exception_count; i++) {
        if (strcmp(compiler->exceptions[i].name, name) == 0) {
            return compiler->exceptions[i].code;
        }
    }
    return 1;  /* default user-defined exception code */
}

static int add_exception(Compiler* compiler, const char* name, int code) {
    if (compiler->exception_count >= MAX_EXCEPTIONS) return 0;
    char* copy = malloc(strlen(name) + 1);
    if (copy == NULL) return 0;
    strcpy(copy, name);
    compiler->exceptions[compiler->exception_count].name = copy;
    compiler->exceptions[compiler->exception_count].code = code;
    compiler->exception_count++;
    return 1;
}

static void free_exception_entries(Compiler* compiler) {
    for (int i = 0; i < compiler->exception_count; i++) {
        free((void*)compiler->exceptions[i].name);
    }
}

static void free_trigger_entries(Compiler* compiler) {
    for (int i = 0; i < compiler->trigger_count; i++) {
        free(compiler->triggers[i].proc_name);
        free(compiler->triggers[i].table);
    }
}

static void free_module_init_entries(Compiler* compiler) {
    for (int i = 0; i < compiler->module_init_count; i++) {
        free(compiler->module_init_procs[i]);
    }
}

static int add_call_patch(Compiler* compiler, const char* name, int offset) {
    if (compiler->patch_count >= MAX_CALL_PATCHES) return 0;
    char* copy = malloc(strlen(name) + 1);
    if (copy == NULL) return 0;
    strcpy(copy, name);
    compiler->patches[compiler->patch_count].offset = offset;
    compiler->patches[compiler->patch_count].name = copy;
    compiler->patch_count++;
    return 1;
}

static void error(Compiler* compiler, const char* message) {
    if (compiler->had_error) return;
    compiler->had_error = 1;
    format_error(compiler->error_message, sizeof(compiler->error_message),
                 compiler->source_path, compiler->current_line,
                 compiler->current_column, message);
}

static void compiler_format_error(Compiler* compiler, char* error, size_t error_size,
                                  const char* fmt, ...) {
    if (error == NULL || error_size == 0) return;
    char msg[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    format_error(error, error_size, compiler->source_path,
                 compiler->current_line, compiler->current_column, msg);
}

static void emit_byte(Compiler* compiler, uint8_t byte) {
    write_chunk_line(compiler->chunk, byte, compiler->current_line, compiler->current_column);
}

static void emit_u16(Compiler* compiler, uint16_t value) {
    write_chunk_u16_line(compiler->chunk, value, compiler->current_line, compiler->current_column);
}

/* Local-slot access: the narrow opcodes carry a 1-byte slot index; frames
   with more than 256 locals use the 16-bit variants. */
static void emit_get_local(Compiler* compiler, int slot) {
    if (slot < 256) {
        emit_byte(compiler, OP_GET_LOCAL);
        emit_byte(compiler, (uint8_t)slot);
    } else {
        emit_byte(compiler, OP_GET_LOCAL16);
        emit_u16(compiler, (uint16_t)slot);
    }
}

static void emit_set_local(Compiler* compiler, int slot) {
    if (slot < 256) {
        emit_byte(compiler, OP_SET_LOCAL);
        emit_byte(compiler, (uint8_t)slot);
    } else {
        emit_byte(compiler, OP_SET_LOCAL16);
        emit_u16(compiler, (uint16_t)slot);
    }
}

static int emit_jump(Compiler* compiler, uint8_t op) {
    emit_byte(compiler, op);
    emit_u16(compiler, 0);
    return compiler->chunk->count - 2;
}

static void patch_jump(Compiler* compiler, int offset) {
    int jump = compiler->chunk->count - (offset + 2);
    compiler->chunk->code[offset] = (uint8_t)((jump >> 8) & 0xFF);
    compiler->chunk->code[offset + 1] = (uint8_t)(jump & 0xFF);
}

static void patch_jump_to(Compiler* compiler, int offset, int target) {
    int jump = target - (offset + 2);
    compiler->chunk->code[offset] = (uint8_t)((jump >> 8) & 0xFF);
    compiler->chunk->code[offset + 1] = (uint8_t)(jump & 0xFF);
}

static int push_loop(Compiler* compiler, int continue_target,
                     int local_count_at_start, int continue_local_count) {
    if (compiler->loop_count >= MAX_LOOP_NESTING) {
        error(compiler, "too many nested loops");
        return 0;
    }
    LoopContext* loop = &compiler->loops[compiler->loop_count++];
    loop->continue_target = continue_target;
    loop->local_count_at_start = local_count_at_start;
    loop->continue_local_count = continue_local_count;
    loop->break_count = 0;
    loop->continue_count = 0;
    return 1;
}

static void pop_loop(Compiler* compiler) {
    LoopContext* loop = &compiler->loops[compiler->loop_count - 1];
    int target = compiler->chunk->count;
    for (int i = 0; i < loop->break_count; i++) {
        patch_jump_to(compiler, loop->break_patches[i], target);
    }
    for (int i = 0; i < loop->continue_count; i++) {
        patch_jump_to(compiler, loop->continue_patches[i], loop->continue_target);
    }
    compiler->loop_count--;
}

static void emit_break(Compiler* compiler) {
    if (compiler->loop_count == 0) {
        error(compiler, "break outside loop");
        return;
    }
    LoopContext* loop = &compiler->loops[compiler->loop_count - 1];
    if (loop->break_count >= MAX_BREAK_PATCHES) {
        error(compiler, "too many break statements in loop");
        return;
    }
    int pops = compiler->local_count - loop->local_count_at_start;
    for (int i = 0; i < pops; i++) {
        emit_byte(compiler, OP_POP);
    }
    loop->break_patches[loop->break_count++] = emit_jump(compiler, OP_JMP);
}

static void emit_continue(Compiler* compiler) {
    if (compiler->loop_count == 0) {
        error(compiler, "continue outside loop");
        return;
    }
    LoopContext* loop = &compiler->loops[compiler->loop_count - 1];
    if (loop->continue_count >= MAX_BREAK_PATCHES) {
        error(compiler, "too many continue statements in loop");
        return;
    }
    int pops = compiler->local_count - loop->continue_local_count;
    for (int i = 0; i < pops; i++) {
        emit_byte(compiler, OP_POP);
    }
    loop->continue_patches[loop->continue_count++] = emit_jump(compiler, OP_JMP);
}

static void emit_constant(Compiler* compiler, Value value) {
    int idx = add_constant(compiler->chunk, value);
    emit_byte(compiler, OP_CONST);
    emit_u16(compiler, (uint16_t)idx);
}

/* Adds a copy of `text` to the constant pool and returns its index, or reports
   an error and returns -1. add_constant retains what it stores, so the
   reference value_string hands back is dropped here; the pool's is the only
   one left, and free_chunk releases it. */
static int add_string_constant(Compiler* compiler, const char* text) {
    Value value = value_string(strdup(text));
    if (value.as.as_string == NULL) {
        error(compiler, "out of memory");
        return -1;
    }
    int idx = add_constant(compiler->chunk, value);
    value_release(value);
    if (idx < 0) {
        error(compiler, "too many constants");
        return -1;
    }
    return idx;
}

static int resolve_local(Compiler* compiler, const char* name, int length) {
    for (int i = compiler->local_count - 1; i >= 0; i--) {
        Local* local = &compiler->locals[i];
        if (local->length == length && memcmp(local->name, name, (size_t)length) == 0) {
            return i;
        }
    }
    return -1;
}

static Type* resolve_local_type(Compiler* compiler, const char* name, int length) {
    for (int i = compiler->local_count - 1; i >= 0; i--) {
        Local* local = &compiler->locals[i];
        if (local->length == length && memcmp(local->name, name, (size_t)length) == 0) {
            return local->type;
        }
    }
    return NULL;
}

static int ensure_local_capacity(Compiler* compiler, int needed) {
    if (needed <= compiler->local_capacity) return 1;
    int new_capacity = compiler->local_capacity == 0 ? LOCALS_INITIAL_CAPACITY
                                                       : compiler->local_capacity;
    while (new_capacity < needed) new_capacity *= 2;
    Local* new_locals = realloc(compiler->locals, sizeof(Local) * (size_t)new_capacity);
    if (new_locals == NULL) return 0;
    compiler->locals = new_locals;
    compiler->local_capacity = new_capacity;
    return 1;
}

static int add_local(Compiler* compiler, const char* name, int length, Type* type) {
    if (!ensure_local_capacity(compiler, compiler->local_count + 1)) return -1;
    Local* local = &compiler->locals[compiler->local_count];
    local->name = name;
    local->length = length;
    local->depth = compiler->scope_depth;
    local->type = type;
    local->is_sql_row = 0;
    return compiler->local_count++;
}

static int find_proc(Compiler* compiler, const char* name) {
    for (int i = 0; i < compiler->proc_count; i++) {
        if (strcmp(compiler->procs[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static void mark_proc_public(Compiler* compiler, const char* name) {
    int idx = find_proc(compiler, name);
    if (idx >= 0) compiler->procs[idx].is_public = 1;
}

static int find_global(Compiler* compiler, const char* name) {
    for (int i = 0; i < compiler->global_count; i++) {
        if (compiler->global_names[i] != NULL && strcmp(compiler->global_names[i], name) == 0) {
            return i;
        }
    }
    return -1;
}

static int add_global(Compiler* compiler, const char* name) {
    int slot = find_global(compiler, name);
    if (slot >= 0) return slot;
    if (compiler->global_count >= MAX_GLOBALS) {
        error(compiler, "too many global variables");
        return -1;
    }
    char* copy = malloc(strlen(name) + 1);
    if (copy == NULL) {
        error(compiler, "out of memory");
        return -1;
    }
    strcpy(copy, name);
    compiler->global_names[compiler->global_count] = copy;
    return compiler->global_count++;
}

static char* mangle_package_name(const char* package_name, const char* member_name) {
    size_t len = strlen(package_name) + 1 + strlen(member_name) + 1;
    char* mangled = malloc(len);
    if (mangled == NULL) return NULL;
    snprintf(mangled, len, "%s.%s", package_name, member_name);
    return mangled;
}

static char* mangle_struct_method(const char* struct_name, const char* method_name) {
    size_t len = strlen(struct_name) + strlen(method_name) + 11;
    char* mangled = malloc(len);
    if (mangled == NULL) return NULL;
    snprintf(mangled, len, "__struct_%s_%s", struct_name, method_name);
    return mangled;
}

static int struct_has_field(const StructDecl* decl, const char* name) {
    for (int i = 0; i < decl->field_count; i++) {
        if (strcmp(decl->field_names[i], name) == 0) return 1;
    }
    return 0;
}

static void register_cursor_query(Compiler* compiler, const char* name, int length, const char* query,
                                  Expr** params, int param_count) {
    for (int i = 0; i < compiler->cursor_query_count; i++) {
        if (compiler->cursor_queries[i].length == length &&
            memcmp(compiler->cursor_queries[i].name, name, (size_t)length) == 0) {
            compiler->cursor_queries[i].query = query;
            compiler->cursor_queries[i].params = params;
            compiler->cursor_queries[i].param_count = param_count;
            return;
        }
    }
    if (compiler->cursor_query_count >= compiler->cursor_query_capacity) {
        int new_capacity = compiler->cursor_query_capacity == 0 ? CURSOR_QUERIES_INITIAL_CAPACITY
                                                                : compiler->cursor_query_capacity * 2;
        CursorQuery* new_queries = realloc(compiler->cursor_queries, sizeof(CursorQuery) * (size_t)new_capacity);
        if (new_queries == NULL) return;
        compiler->cursor_queries = new_queries;
        compiler->cursor_query_capacity = new_capacity;
    }
    compiler->cursor_queries[compiler->cursor_query_count].name = name;
    compiler->cursor_queries[compiler->cursor_query_count].length = length;
    compiler->cursor_queries[compiler->cursor_query_count].query = query;
    compiler->cursor_queries[compiler->cursor_query_count].params = params;
    compiler->cursor_queries[compiler->cursor_query_count].param_count = param_count;
    compiler->cursor_query_count++;
}

static const CursorQuery* find_cursor_query(Compiler* compiler, const char* name, int length) {
    for (int i = 0; i < compiler->cursor_query_count; i++) {
        if (compiler->cursor_queries[i].length == length &&
            memcmp(compiler->cursor_queries[i].name, name, (size_t)length) == 0) {
            return &compiler->cursor_queries[i];
        }
    }
    return NULL;
}

static void patch_call(Chunk* chunk, int offset, int target) {
    chunk->code[offset] = (uint8_t)((target >> 8) & 0xFF);
    chunk->code[offset + 1] = (uint8_t)(target & 0xFF);
}

static void record_call_site(Compiler* compiler) {
    if (!compiler->record_call_sites) return;
    if (compiler->call_site_count == compiler->call_site_capacity) {
        int new_capacity = compiler->call_site_capacity == 0 ? 64 : compiler->call_site_capacity * 2;
        int* new_sites = realloc(compiler->call_sites, sizeof(int) * (size_t)new_capacity);
        if (new_sites == NULL) {
            error(compiler, "out of memory");
            return;
        }
        compiler->call_sites = new_sites;
        compiler->call_site_capacity = new_capacity;
    }
    compiler->call_sites[compiler->call_site_count++] = compiler->chunk->count;
}

static void emit_call(Compiler* compiler, const char* name, int arg_count) {
    int idx = find_proc(compiler, name);
    int autonomous = (idx >= 0 && compiler->procs[idx].autonomous_transaction);
    emit_byte(compiler, autonomous ? OP_CALL_AUTONOMOUS : OP_CALL);
    record_call_site(compiler);
    if (idx >= 0 && compiler->procs[idx].offset >= 0) {
        emit_u16(compiler, (uint16_t)compiler->procs[idx].offset);
    } else {
        if (!add_call_patch(compiler, name, compiler->chunk->count)) {
            error(compiler, "too many call patches");
            return;
        }
        emit_u16(compiler, 0);
    }
    emit_byte(compiler, (uint8_t)arg_count);
}

static void compile_expr(Compiler* compiler, Expr* expr);
static void compile_stmt(Compiler* compiler, Stmt* stmt);
static void emit_trigger_calls(Compiler* compiler, int timing, int event, const char* table);

static int compiler_load_module(Compiler* compiler, const char* path, char* error, size_t error_size);
static int compiler_compile_source(Compiler* compiler, const char* source, int is_main, const char* path, char* error, size_t error_size);

static void compile_block(Compiler* compiler, Block* block) {
    int previous_count = compiler->local_count;
    for (int i = 0; i < block->stmt_count; i++) {
        compile_stmt(compiler, block->stmts[i]);
        if (compiler->had_error) return;
    }
    int pops = compiler->local_count - previous_count;
    for (int i = 0; i < pops; i++) {
        emit_byte(compiler, OP_POP);
    }
    compiler->local_count = previous_count;
}

static void compile_expr(Compiler* compiler, Expr* expr) {
    /* Line 0 is a synthesized node with no source position; a negative line
       is prepended built-in/stored source and is kept (see format_error). */
    if (expr != NULL && expr->loc.line != 0) {
        compiler->current_line = expr->loc.line;
        compiler->current_column = expr->loc.column;
    }
    switch (expr->kind) {
        case EXPR_LITERAL: {
            Value v = expr->as.literal.value;
            emit_constant(compiler, v);
            break;
        }
        case EXPR_ARRAY: {
            ArrayExpr* a = &expr->as.array;
            for (int i = 0; i < a->count; i++) {
                compile_expr(compiler, a->elements[i]);
                if (compiler->had_error) return;
            }
            emit_byte(compiler, OP_ARRAY_BUILD);
            emit_u16(compiler, (uint16_t)a->count);
            break;
        }
        case EXPR_MAP_LITERAL: {
            MapLiteralExpr* m = &expr->as.map_literal;
            for (int i = 0; i < m->count; i++) {
                compile_expr(compiler, m->values[i]);
                if (compiler->had_error) return;
                compile_expr(compiler, m->keys[i]);
                if (compiler->had_error) return;
            }
            emit_byte(compiler, OP_MAP_BUILD);
            emit_u16(compiler, (uint16_t)m->count);
            break;
        }
        case EXPR_INDEX: {
            IndexExpr* idx = &expr->as.index;
            compile_expr(compiler, idx->array);
            if (compiler->had_error) return;
            compile_expr(compiler, idx->index);
            if (compiler->had_error) return;
            emit_byte(compiler, OP_INDEX_GET);
            break;
        }
        case EXPR_VARIABLE: {
            const char* name = expr->as.variable.name;
            int length = (int)strlen(name);
            int slot = resolve_local(compiler, name, length);
            if (slot < 0 && compiler->current_package != NULL) {
                char* global_name = mangle_package_name(compiler->current_package, name);
                if (global_name != NULL) {
                    slot = find_global(compiler, global_name);
                    free(global_name);
                }
                if (slot >= 0) {
                    emit_byte(compiler, OP_GET_GLOBAL);
                    emit_byte(compiler, (uint8_t)slot);
                    break;
                }
            }
            if (slot < 0 && compiler->current_struct != NULL &&
                struct_has_field(compiler->current_struct, name)) {
                /* Unqualified field access inside a struct method: self.<field> */
                int self_slot = resolve_local(compiler, "self", 4);
                if (self_slot < 0) {
                    error(compiler, "Undefined variable 'self'");
                    return;
                }
                int field_idx = add_string_constant(compiler, name);
                if (field_idx < 0) return;
                emit_get_local(compiler, self_slot);
                emit_byte(compiler, OP_ROW_GET);
                emit_u16(compiler, (uint16_t)field_idx);
                break;
            }
            if (slot < 0) {
                char msg[256];
                snprintf(msg, sizeof(msg), "Undefined variable '%s'", name);
                error(compiler, msg);
                return;
            }
            emit_get_local(compiler, slot);
            break;
        }
        case EXPR_BINARY: {
            BinaryExpr* b = &expr->as.binary;
            compile_expr(compiler, b->left);
            if (compiler->had_error) return;
            compile_expr(compiler, b->right);
            if (compiler->had_error) return;
            switch (b->op) {
                case TOKEN_PLUS:  emit_byte(compiler, OP_ADD); break;
                case TOKEN_MINUS: emit_byte(compiler, OP_SUB); break;
                case TOKEN_STAR:  emit_byte(compiler, OP_MUL); break;
                case TOKEN_SLASH: emit_byte(compiler, OP_DIV); break;
                case TOKEN_EQ:    emit_byte(compiler, OP_EQ);  break;
                case TOKEN_LT:    emit_byte(compiler, OP_LT);  break;
                case TOKEN_GT:    emit_byte(compiler, OP_GT);  break;
                default: error(compiler, "unsupported binary operator"); break;
            }
            break;
        }
        case EXPR_CALL: {
            CallExpr* c = &expr->as.call;
            if (c->package_name != NULL) {
                /* Method call on a struct-typed variable: the receiver becomes
                 * the implicit first argument ('self'). */
                Type* recv_type = resolve_local_type(compiler, c->package_name,
                                                     (int)strlen(c->package_name));
                if (recv_type != NULL && recv_type->kind == TYPE_STRUCT &&
                    recv_type->struct_name != NULL) {
                    char* mangled = mangle_struct_method(recv_type->struct_name, c->name);
                    if (mangled == NULL) {
                        error(compiler, "out of memory");
                        return;
                    }
                    if (find_proc(compiler, mangled) >= 0) {
                        int recv_slot = resolve_local(compiler, c->package_name,
                                                      (int)strlen(c->package_name));
                        emit_get_local(compiler, recv_slot);
                        for (int i = 0; i < c->arg_count; i++) {
                            compile_expr(compiler, c->args[i]);
                            if (compiler->had_error) { free(mangled); return; }
                        }
                        emit_call(compiler, mangled, c->arg_count + 1);
                        free(mangled);
                        break;
                    }
                    free(mangled);
                }
            }
            char* effective_name = NULL;
            int is_native = 0;
            int native_idx = -1;

            if (c->package_name != NULL) {
                effective_name = mangle_package_name(c->package_name, c->name);
            } else {
                native_idx = native_find(c->name);
                is_native = native_idx >= 0;
                if (is_native) {
                    effective_name = strdup(c->name);
                } else if (compiler->current_package != NULL) {
                    effective_name = mangle_package_name(compiler->current_package, c->name);
                    if (find_proc(compiler, effective_name) < 0) {
                        free(effective_name);
                        effective_name = strdup(c->name);
                    }
                } else {
                    effective_name = strdup(c->name);
                }
            }
            if (effective_name == NULL) {
                error(compiler, "out of memory");
                return;
            }

            if (is_native) {
                int expected_arity = native_arity(native_idx);
                if (expected_arity == NATIVE_VARIADIC && c->arg_count > MAX_NATIVE_ARGS) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "%s takes at most %d arguments", c->name,
                             MAX_NATIVE_ARGS);
                    error(compiler, msg);
                    free(effective_name);
                    return;
                }
                if (expected_arity != NATIVE_VARIADIC && c->arg_count != expected_arity) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "%s expects %d argument(s)", c->name, expected_arity);
                    error(compiler, msg);
                    free(effective_name);
                    return;
                }
                for (int i = 0; i < c->arg_count; i++) {
                    compile_expr(compiler, c->args[i]);
                    if (compiler->had_error) { free(effective_name); return; }
                }
                emit_byte(compiler, OP_NATIVE_CALL);
                emit_u16(compiler, (uint16_t)native_idx);
                emit_byte(compiler, (uint8_t)c->arg_count);
                free(effective_name);
            } else {
                int proc_idx = find_proc(compiler, effective_name);
                ParamMode* modes = NULL;
                if (proc_idx >= 0) {
                    modes = compiler->procs[proc_idx].param_modes;
                }

                int out_count = 0;
                int out_slots[64];
                int out_positions[64];
                for (int i = 0; i < c->arg_count; i++) {
                    ParamMode mode = (modes != NULL && i < compiler->procs[proc_idx].param_count)
                                         ? modes[i]
                                         : PARAM_IN;
                    if (mode == PARAM_OUT) {
                        emit_constant(compiler, value_int(0));
                    } else {
                        compile_expr(compiler, c->args[i]);
                        if (compiler->had_error) { free(effective_name); return; }
                    }
                    if (mode == PARAM_OUT || mode == PARAM_INOUT) {
                        if (c->args[i]->kind != EXPR_VARIABLE) {
                            error(compiler, "OUT/IN OUT argument must be a variable");
                            free(effective_name);
                            return;
                        }
                        if (out_count >= 64) {
                            error(compiler, "too many OUT/IN OUT arguments");
                            free(effective_name);
                            return;
                        }
                        const char* var_name = c->args[i]->as.variable.name;
                        int slot = resolve_local(compiler, var_name, (int)strlen(var_name));
                        if (slot < 0) {
                            char msg[256];
                            snprintf(msg, sizeof(msg), "Undefined variable '%s'", var_name);
                            error(compiler, msg);
                            free(effective_name);
                            return;
                        }
                        out_positions[out_count] = i;
                        out_slots[out_count++] = slot;
                    }
                }

                if (out_count > 0) {
                    int autonomous = (proc_idx >= 0 && compiler->procs[proc_idx].autonomous_transaction);
                    emit_byte(compiler, autonomous ? OP_CALL_OUT_AUTONOMOUS : OP_CALL_OUT);
                    record_call_site(compiler);
                    if (proc_idx >= 0 && compiler->procs[proc_idx].offset >= 0) {
                        emit_u16(compiler, (uint16_t)compiler->procs[proc_idx].offset);
                    } else {
                        if (!add_call_patch(compiler, effective_name, compiler->chunk->count)) {
                            error(compiler, "too many call patches");
                            free(effective_name);
                            return;
                        }
                        emit_u16(compiler, 0);
                    }
                    emit_byte(compiler, (uint8_t)c->arg_count);
                    emit_byte(compiler, (uint8_t)out_count);
                    for (int i = 0; i < out_count; i++) {
                        emit_u16(compiler, (uint16_t)out_positions[i]);
                        emit_u16(compiler, (uint16_t)out_slots[i]);
                    }
                } else {
                    emit_call(compiler, effective_name, c->arg_count);
                }
                free(effective_name);
            }
            break;
        }
        case EXPR_FIELD: {
            FieldExpr* f = &expr->as.field;
            int field_idx = add_string_constant(compiler, f->field);
            if (field_idx < 0) return;
            emit_byte(compiler, OP_GET_FIELD);
            emit_u16(compiler, (uint16_t)field_idx);
            break;
        }
        case EXPR_UNARY: {
            UnaryExpr* u = &expr->as.unary;
            compile_expr(compiler, u->operand);
            if (compiler->had_error) return;
            switch (u->op) {
                case TOKEN_MINUS: emit_byte(compiler, OP_NEGATE); break;
                case TOKEN_BANG:  emit_byte(compiler, OP_NOT); break;
                default: error(compiler, "unsupported unary operator"); break;
            }
            break;
        }
        case EXPR_SQL_PARAM: {
            const char* name = expr->as.sql_param.name;
            int slot = resolve_local(compiler, name, (int)strlen(name));
            if (slot < 0) {
                char msg[256];
                snprintf(msg, sizeof(msg), "Undefined variable '%s'", name);
                error(compiler, msg);
                return;
            }
            emit_get_local(compiler, slot);
            break;
        }
        case EXPR_CURSOR_ATTR: {
            const char* cursor_name = expr->as.cursor_attr.cursor_name;
            int slot = resolve_local(compiler, cursor_name, (int)strlen(cursor_name));
            if (slot < 0) {
                char msg[256];
                snprintf(msg, sizeof(msg), "Undefined variable '%s'", cursor_name);
                error(compiler, msg);
                return;
            }
            int attr_idx = add_string_constant(compiler, expr->as.cursor_attr.attr_name);
            if (attr_idx < 0) return;
            emit_get_local(compiler, slot);
            emit_byte(compiler, OP_CURSOR_ATTR);
            emit_u16(compiler, (uint16_t)attr_idx);
            break;
        }
        case EXPR_ROW_FIELD: {
            RowFieldExpr* rf = &expr->as.row_field;
            /* x.field on a SQL loop iterator reads the current row, as row.field does. */
            int sql_row = 0;
            if (rf->row->kind == EXPR_VARIABLE) {
                const char* var = rf->row->as.variable.name;
                int slot = resolve_local(compiler, var, (int)strlen(var));
                sql_row = slot >= 0 && compiler->locals[slot].is_sql_row;
            }
            if (!sql_row) {
                compile_expr(compiler, rf->row);
                if (compiler->had_error) return;
            }
            int field_idx = add_string_constant(compiler, rf->field);
            if (field_idx < 0) return;
            emit_byte(compiler, sql_row ? OP_GET_FIELD : OP_ROW_GET);
            emit_u16(compiler, (uint16_t)field_idx);
            break;
        }
        case EXPR_STRUCT_LITERAL: {
            StructLiteralExpr* sl = &expr->as.struct_literal;
            for (int i = 0; i < sl->field_count; i++) {
                compile_expr(compiler, sl->values[i]);
                if (compiler->had_error) return;
            }
            size_t schema_len = 0;
            for (int i = 0; i < sl->field_count; i++) {
                if (i > 0) schema_len++;
                schema_len += strlen(sl->field_names[i]);
            }
            char* schema = malloc(schema_len + 1);
            if (schema == NULL) {
                error(compiler, "out of memory");
                return;
            }
            schema[0] = '\0';
            for (int i = 0; i < sl->field_count; i++) {
                if (i > 0) strcat(schema, ",");
                strcat(schema, sl->field_names[i]);
            }
            int schema_idx = add_string_constant(compiler, schema);
            free(schema);
            if (schema_idx < 0) return;
            emit_byte(compiler, OP_STRUCT_BUILD);
            emit_u16(compiler, (uint16_t)schema_idx);
            break;
        }
        default:
            error(compiler, "unsupported expression");
            break;
    }
}

static void compile_stmt(Compiler* compiler, Stmt* stmt) {
    if (stmt != NULL && stmt->loc.line != 0) {
        compiler->current_line = stmt->loc.line;
        compiler->current_column = stmt->loc.column;
    }
    switch (stmt->kind) {
        case STMT_VAR_DECL: {
            VarDeclStmt* d = &stmt->as.var_decl;
            if (d->initializer != NULL) {
                compile_expr(compiler, d->initializer);
            } else {
                if (d->type != NULL && type_is_array(d->type)) {
                    emit_byte(compiler, OP_ARRAY_BUILD);
                    emit_u16(compiler, 0);
                } else if (d->type != NULL && type_is_map(d->type)) {
                    emit_byte(compiler, OP_MAP_BUILD);
                    emit_u16(compiler, 0);
                } else if (d->type != NULL &&
                           (d->type->kind == TYPE_STRUCT ||
                            d->type->kind == TYPE_ROW ||
                            d->type->kind == TYPE_PERCENT_ROWTYPE)) {
                    int schema_idx = add_string_constant(compiler, "");
                    if (schema_idx < 0) return;
                    emit_byte(compiler, OP_CONST);
                    emit_u16(compiler, (uint16_t)schema_idx);
                    emit_byte(compiler, OP_STRUCT_BUILD);
                    emit_u16(compiler, (uint16_t)schema_idx);
                } else {
                    emit_constant(compiler, value_int(0));
                }
            }
            if (compiler->had_error) return;
            if (compiler->current_package != NULL && compiler->scope_depth == 0) {
                char* global_name = mangle_package_name(compiler->current_package, d->name);
                if (global_name == NULL) {
                    error(compiler, "out of memory");
                    return;
                }
                int slot = add_global(compiler, global_name);
                free(global_name);
                if (slot < 0) return;
                emit_byte(compiler, OP_SET_GLOBAL);
                emit_byte(compiler, (uint8_t)slot);
            } else {
                int slot = add_local(compiler, d->name, (int)strlen(d->name), d->type);
                if (slot < 0) {
                    error(compiler, "too many locals");
                    return;
                }
                emit_set_local(compiler, slot);
            }
            break;
        }
        case STMT_ASSIGN: {
            AssignStmt* a = &stmt->as.assign;
            int slot = resolve_local(compiler, a->name, (int)strlen(a->name));
            if (slot < 0 && compiler->current_struct != NULL &&
                struct_has_field(compiler->current_struct, a->name)) {
                /* Unqualified field write inside a struct method: self.<field> = value */
                int self_slot = resolve_local(compiler, "self", 4);
                if (self_slot < 0) {
                    error(compiler, "Undefined variable 'self'");
                    return;
                }
                int field_idx = add_string_constant(compiler, a->name);
                if (field_idx < 0) return;
                emit_get_local(compiler, self_slot);
                emit_byte(compiler, OP_CONST);
                emit_u16(compiler, (uint16_t)field_idx);
                compile_expr(compiler, a->value);
                if (compiler->had_error) return;
                emit_byte(compiler, OP_INDEX_SET);
                break;
            }
            compile_expr(compiler, a->value);
            if (compiler->had_error) return;
            if (slot < 0 && compiler->current_package != NULL) {
                char* global_name = mangle_package_name(compiler->current_package, a->name);
                if (global_name != NULL) {
                    slot = find_global(compiler, global_name);
                    free(global_name);
                }
                if (slot >= 0) {
                    emit_byte(compiler, OP_SET_GLOBAL);
                    emit_byte(compiler, (uint8_t)slot);
                    emit_byte(compiler, OP_POP);
                    break;
                }
            }
            if (slot < 0) {
                char msg[256];
                snprintf(msg, sizeof(msg), "Undefined variable '%s'", a->name);
                error(compiler, msg);
                return;
            }
            /* The set opcodes store the value and leave it on the stack,
               which a declaration keeps as the new local's slot; an
               assignment has no use for it (#89). */
            emit_set_local(compiler, slot);
            emit_byte(compiler, OP_POP);
            break;
        }
        case STMT_FIELD_ASSIGN: {
            FieldAssignStmt* fa = &stmt->as.field_assign;
            compile_expr(compiler, fa->object);
            if (compiler->had_error) return;
            int field_idx = add_string_constant(compiler, fa->field);
            if (field_idx < 0) return;
            emit_byte(compiler, OP_CONST);
            emit_u16(compiler, (uint16_t)field_idx);
            compile_expr(compiler, fa->value);
            if (compiler->had_error) return;
            emit_byte(compiler, OP_INDEX_SET);
            break;
        }
        case STMT_RETURN:
            compile_expr(compiler, stmt->as.return_stmt.value);
            if (compiler->had_error) return;
            emit_byte(compiler, OP_RETURN);
            break;
        case STMT_IF: {
            IfStmt* i = &stmt->as.if_stmt;
            compile_expr(compiler, i->condition);
            if (compiler->had_error) return;
            int else_jump = emit_jump(compiler, OP_JZ);
            compile_block(compiler, i->then_block);
            if (compiler->had_error) return;
            int end_jump = emit_jump(compiler, OP_JMP);
            patch_jump(compiler, else_jump);
            if (i->else_block != NULL) {
                compile_block(compiler, i->else_block);
                if (compiler->had_error) return;
            }
            patch_jump(compiler, end_jump);
            break;
        }
        case STMT_WHILE: {
            WhileStmt* w = &stmt->as.while_stmt;
            int start_count = compiler->local_count;
            if (w->is_do_while) {
                int body_start = compiler->chunk->count;
                if (!push_loop(compiler, body_start, start_count, start_count)) return;
                compile_block(compiler, w->body);
                if (compiler->had_error) return;
                /* continue goes to the condition, as in a while loop; the
                   top of the body would skip the check. */
                compiler->loops[compiler->loop_count - 1].continue_target = compiler->chunk->count;
                compile_expr(compiler, w->condition);
                if (compiler->had_error) return;
                int exit_jump = emit_jump(compiler, OP_JZ);
                int back = emit_jump(compiler, OP_JMP);
                patch_jump_to(compiler, back, body_start);
                patch_jump(compiler, exit_jump);
                pop_loop(compiler);
            } else {
                int loop_start = compiler->chunk->count;
                if (!push_loop(compiler, loop_start, start_count, start_count)) return;
                compile_expr(compiler, w->condition);
                if (compiler->had_error) return;
                int exit_jump = emit_jump(compiler, OP_JZ);
                compile_block(compiler, w->body);
                if (compiler->had_error) return;
                int back = emit_jump(compiler, OP_JMP);
                patch_jump_to(compiler, back, loop_start);
                patch_jump(compiler, exit_jump);
                pop_loop(compiler);
            }
            break;
        }
        case STMT_FOREACH: {
            ForeachStmt* f = &stmt->as.foreach_stmt;
            int start_count = compiler->local_count;
            compile_expr(compiler, f->iterable);
            if (compiler->had_error) return;
            int array_slot = add_local(compiler, "__fe_array", 12, &type_unknown);
            if (array_slot < 0) { error(compiler, "too many locals"); return; }
            emit_set_local(compiler, array_slot);
            emit_constant(compiler, value_int(0));
            int idx_slot = add_local(compiler, "__fe_idx", 9, &type_int);
            emit_set_local(compiler, idx_slot);
            emit_constant(compiler, value_int(0));
            int var_slot = add_local(compiler, f->var_name, (int)strlen(f->var_name), &type_unknown);
            emit_set_local(compiler, var_slot);
            int loop_start = compiler->chunk->count;
            /* break and continue pop only the body's locals: the three
               loop locals are popped once, at the exit both reach. */
            if (!push_loop(compiler, loop_start, start_count + 3, start_count + 3)) return;
            emit_get_local(compiler, idx_slot);
            emit_get_local(compiler, array_slot);
            int len_native = native_find("length");
            emit_byte(compiler, OP_NATIVE_CALL);
            emit_u16(compiler, (uint16_t)len_native);
            emit_byte(compiler, 1);
            emit_byte(compiler, OP_LT);
            int exit_jump = emit_jump(compiler, OP_JZ);
            emit_get_local(compiler, array_slot);
            emit_get_local(compiler, idx_slot);
            emit_byte(compiler, OP_INDEX_GET);
            emit_set_local(compiler, var_slot);
            emit_byte(compiler, OP_POP);
            compile_block(compiler, f->body);
            if (compiler->had_error) return;
            /* continue has to advance the index, or it repeats the same
               element forever. */
            compiler->loops[compiler->loop_count - 1].continue_target = compiler->chunk->count;
            emit_get_local(compiler, idx_slot);
            emit_constant(compiler, value_int(1));
            emit_byte(compiler, OP_ADD);
            emit_set_local(compiler, idx_slot);
            emit_byte(compiler, OP_POP);
            int back = emit_jump(compiler, OP_JMP);
            patch_jump_to(compiler, back, loop_start);
            patch_jump(compiler, exit_jump);
            pop_loop(compiler);
            for (int i = 0; i < 3; i++) emit_byte(compiler, OP_POP);
            compiler->local_count = start_count;
            break;
        }
        case STMT_FORALL: {
            ForallStmt* f = &stmt->as.forall_stmt;
            int start_count = compiler->local_count;
            int array_slot = resolve_local(compiler, f->array_name, (int)strlen(f->array_name));
            if (array_slot < 0) {
                char msg[256];
                snprintf(msg, sizeof(msg), "Undefined variable '%s'", f->array_name);
                error(compiler, msg);
                return;
            }
            emit_get_local(compiler, array_slot);
            int tmp_array_slot = add_local(compiler, "__fa_array", 11, &type_unknown);
            if (tmp_array_slot < 0) { error(compiler, "too many locals"); return; }
            emit_set_local(compiler, tmp_array_slot);
            emit_constant(compiler, value_int(0));
            int idx_slot = add_local(compiler, "__fa_idx", 9, &type_int);
            emit_set_local(compiler, idx_slot);
            emit_constant(compiler, value_int(0));
            int var_slot = add_local(compiler, f->var_name, (int)strlen(f->var_name), &type_unknown);
            emit_set_local(compiler, var_slot);
            int loop_start = compiler->chunk->count;
            if (!push_loop(compiler, loop_start, start_count, start_count + 3)) return;
            emit_get_local(compiler, idx_slot);
            emit_get_local(compiler, tmp_array_slot);
            int len_native = native_find("length");
            emit_byte(compiler, OP_NATIVE_CALL);
            emit_u16(compiler, (uint16_t)len_native);
            emit_byte(compiler, 1);
            emit_byte(compiler, OP_LT);
            int exit_jump = emit_jump(compiler, OP_JZ);
            emit_get_local(compiler, tmp_array_slot);
            emit_get_local(compiler, idx_slot);
            emit_byte(compiler, OP_INDEX_GET);
            emit_set_local(compiler, var_slot);
            emit_byte(compiler, OP_POP);
            compile_stmt(compiler, f->sql_stmt);
            if (compiler->had_error) return;
            emit_get_local(compiler, idx_slot);
            emit_constant(compiler, value_int(1));
            emit_byte(compiler, OP_ADD);
            emit_set_local(compiler, idx_slot);
            emit_byte(compiler, OP_POP);
            int back = emit_jump(compiler, OP_JMP);
            patch_jump_to(compiler, back, loop_start);
            patch_jump(compiler, exit_jump);
            pop_loop(compiler);
            for (int i = 0; i < 3; i++) emit_byte(compiler, OP_POP);
            compiler->local_count = start_count;
            break;
        }
        case STMT_BREAK:
            emit_break(compiler);
            break;
        case STMT_CONTINUE:
            emit_continue(compiler);
            break;
        case STMT_FOR_C: {
            CForStmt* cf = &stmt->as.cfor_stmt;
            int start_count = compiler->local_count;

            if (cf->init != NULL) {
                compile_stmt(compiler, cf->init);
                if (compiler->had_error) return;
            }
            /* A variable the init declares lives for the whole loop: break
               and continue pop only what the body declared, and the exit
               below pops the init's locals once, for both ways out. */
            int loop_locals = compiler->local_count;

            int loop_start = compiler->chunk->count;

            if (cf->condition != NULL) {
                compile_expr(compiler, cf->condition);
                if (compiler->had_error) return;
            } else {
                emit_constant(compiler, value_bool(1));
            }
            int exit_jump = emit_jump(compiler, OP_JZ);

            /* Push loop before body so break/continue are captured; continue
               target will be set to the step clause once the body is emitted. */
            if (!push_loop(compiler, loop_start, loop_locals, loop_locals)) return;

            compile_block(compiler, cf->body);
            if (compiler->had_error) return;

            compiler->loops[compiler->loop_count - 1].continue_target = compiler->chunk->count;

            if (cf->step != NULL) {
                compile_stmt(compiler, cf->step);
                if (compiler->had_error) return;
            }

            int back = emit_jump(compiler, OP_JMP);
            patch_jump_to(compiler, back, loop_start);
            patch_jump(compiler, exit_jump);
            pop_loop(compiler);

            for (int i = start_count; i < loop_locals; i++) emit_byte(compiler, OP_POP);
            compiler->local_count = start_count;
            break;
        }
        case STMT_FOR: {
            ForStmt* f = &stmt->as.for_stmt;
            int start_count = compiler->local_count;

            for (int i = 0; i < f->param_count; i++) {
                compile_expr(compiler, f->params[i]);
                if (compiler->had_error) return;
                const char* name = f->params[i]->as.sql_param.name;
                Type* t = resolve_local_type(compiler, name, (int)strlen(name));
                if (t != NULL && t->kind == TYPE_STRING) {
                    emit_byte(compiler, OP_SQL_BIND_STRING);
                } else if (t != NULL && t->kind == TYPE_FLOAT) {
                    emit_byte(compiler, OP_SQL_BIND_FLOAT);
                } else {
                    emit_byte(compiler, OP_SQL_BIND_INT);
                }
            }

            int query_idx = add_string_constant(compiler, f->sql_query);
            if (query_idx < 0) return;
            emit_byte(compiler, OP_SQL);
            emit_u16(compiler, (uint16_t)query_idx);
            emit_u16(compiler, (uint16_t)stmt->loc.line);

            /* Bind the iterator variable to a local slot before the loop
               begins. The value is unused because fields are read with
               OP_GET_FIELD, but the slot has to exist so nested locals keep
               correct offsets - and it has to be pushed here rather than
               inside the loop, because the exit path pops it unconditionally
               and a query returning no rows never enters the body. */
            int slot = add_local(compiler, f->var_name, (int)strlen(f->var_name), &type_unknown);
            if (slot < 0) {
                error(compiler, "too many locals");
                return;
            }
            compiler->locals[slot].is_sql_row = 1;
            emit_byte(compiler, OP_CONST);
            emit_u16(compiler, (uint16_t)add_constant(compiler->chunk, value_int(0)));
            emit_set_local(compiler, slot);

            int loop_start = compiler->chunk->count;
            if (!push_loop(compiler, loop_start, start_count, start_count + 1)) return;
            int exit_jump = emit_jump(compiler, OP_SQL_NEXT);

            compile_block(compiler, f->body);
            if (compiler->had_error) return;

            int back = emit_jump(compiler, OP_JMP);
            patch_jump_to(compiler, back, loop_start);
            patch_jump(compiler, exit_jump);

            /* Pop the iterator local at loop exit so it does not leak. */
            emit_byte(compiler, OP_POP);
            compiler->local_count--;
            pop_loop(compiler);
            break;
        }
        case STMT_PRINT: {
            compile_expr(compiler, stmt->as.print_stmt.value);
            if (compiler->had_error) return;
            emit_byte(compiler, OP_PRINT);
            break;
        }
        case STMT_INDEX_ASSIGN: {
            IndexAssignStmt* ia = &stmt->as.index_assign;
            compile_expr(compiler, ia->array);
            if (compiler->had_error) return;
            compile_expr(compiler, ia->index);
            if (compiler->had_error) return;
            compile_expr(compiler, ia->value);
            if (compiler->had_error) return;
            emit_byte(compiler, OP_INDEX_SET);
            break;
        }
        case STMT_EXPR: {
            compile_expr(compiler, stmt->as.expr_stmt.value);
            if (compiler->had_error) return;
            emit_byte(compiler, OP_POP);
            break;
        }
        case STMT_SQL_DDL:
        case STMT_SQL_DML: {
            SqlStmt* s = &stmt->as.sql_stmt;
            for (int i = 0; i < s->param_count; i++) {
                compile_expr(compiler, s->params[i]);
                if (compiler->had_error) return;
                const char* name = s->params[i]->as.sql_param.name;
                Type* t = resolve_local_type(compiler, name, (int)strlen(name));
                if (t != NULL && t->kind == TYPE_STRING) {
                    emit_byte(compiler, OP_SQL_BIND_STRING);
                } else if (t != NULL && t->kind == TYPE_FLOAT) {
                    emit_byte(compiler, OP_SQL_BIND_FLOAT);
                } else {
                    emit_byte(compiler, OP_SQL_BIND_INT);
                }
            }
            int sql_idx = add_string_constant(compiler, s->sql);
            if (sql_idx < 0) return;
            int trig_event = -1;
            char trig_table[64];
            int has_trig = sql_trigger_info(s->sql, &trig_event, trig_table, sizeof(trig_table));
            if (has_trig) {
                emit_trigger_calls(compiler, TRIGGER_BEFORE, trig_event, trig_table);
            }
            emit_byte(compiler, OP_SQL_EXEC);
            emit_u16(compiler, (uint16_t)sql_idx);
            emit_u16(compiler, (uint16_t)stmt->loc.line);
            if (has_trig) {
                emit_trigger_calls(compiler, TRIGGER_AFTER, trig_event, trig_table);
            }
            break;
        }
        case STMT_DROP_TRIGGER: {
            const char* name = stmt->as.drop_trigger.name;
            /* Suppress compile-time firing of the dropped trigger for any
               static SQL compiled after this statement. The entry stays in
               the runtime registry (chunk->triggers): the OP_DROP_TRIGGER
               emitted below disables it at execution time, so dynamic SQL
               running before the drop still fires it. */
            for (int i = 0; i < compiler->trigger_count; i++) {
                TriggerEntry* entry = &compiler->triggers[i];
                if (trigger_name_equals(entry->proc_name + 10, name)) { /* skip "__trigger_" */
                    entry->dropped = 1;
                    break;
                }
            }
            /* Runtime drop: disables the registry entry and removes the
               persisted definition (see OP_DROP_TRIGGER in vm.c). */
            int name_idx = add_string_constant(compiler, name);
            if (name_idx < 0) return;
            emit_byte(compiler, OP_DROP_TRIGGER);
            emit_u16(compiler, (uint16_t)name_idx);
            break;
        }
        case STMT_SQL_TRANSACTION: {
            int kind = stmt->as.sql_transaction.kind;
            if (kind == 0) {
                emit_byte(compiler, OP_SQL_BEGIN);
            } else if (kind == 1) {
                emit_byte(compiler, OP_SQL_COMMIT);
            } else if (kind == 2) {
                emit_byte(compiler, OP_SQL_ROLLBACK);
            } else {
                int idx = add_string_constant(compiler, stmt->as.sql_transaction.name);
                if (idx < 0) return;
                if (kind == 3) {
                    emit_byte(compiler, OP_SQL_SAVEPOINT);
                } else if (kind == 4) {
                    emit_byte(compiler, OP_SQL_ROLLBACK_TO);
                } else {
                    emit_byte(compiler, OP_SQL_RELEASE_SAVEPOINT);
                }
                emit_u16(compiler, (uint16_t)idx);
            }
            break;
        }
        case STMT_TRY_CATCH: {
            TryCatchStmt* tc = &stmt->as.try_catch;
            int catch_slot = add_local(compiler, tc->catch_var, (int)strlen(tc->catch_var), &type_string);
            if (catch_slot < 0) {
                error(compiler, "too many locals");
                return;
            }
            int local_count_at_try = compiler->local_count;

            emit_byte(compiler, OP_TRY);
            int catch_offset = compiler->chunk->count;
            emit_u16(compiler, 0);
            emit_u16(compiler, (uint16_t)local_count_at_try);
            int after_try_operands = compiler->chunk->count;

            compile_block(compiler, tc->try_block);
            if (compiler->had_error) return;

            emit_byte(compiler, OP_END_TRY);
            int end_offset = compiler->chunk->count;
            emit_u16(compiler, 0);

            int catch_start = compiler->chunk->count;
            int catch_jump = catch_start - after_try_operands;
            compiler->chunk->code[catch_offset] = (uint8_t)((catch_jump >> 8) & 0xFF);
            compiler->chunk->code[catch_offset + 1] = (uint8_t)(catch_jump & 0xFF);

            /* vm_catch already pushed the error message at frame_base[catch_slot]
             * and set stack_top to catch_slot + 1, so the message is both the
             * initial value of the catch variable and the bottom of the stack.
             * Emitting SET_LOCAL + POP here would release the same object twice
             * because the source and destination are the same slot. */
            (void)catch_slot;

            compile_block(compiler, tc->catch_block);
            if (compiler->had_error) return;

            emit_byte(compiler, OP_POP);
            compiler->local_count--;

            int after_catch = compiler->chunk->count;
            patch_jump_to(compiler, end_offset, after_catch);
            break;
        }
        case STMT_EXCEPTION_DECL: {
            const char* name = stmt->as.exception_decl.name;
            int code = find_exception_code(compiler, name);
            if (!add_exception(compiler, name, code)) {
                error(compiler, "too many exceptions");
            }
            break;
        }
        case STMT_RAISE: {
            const char* name = stmt->as.raise_stmt.name;
            int code = find_exception_code(compiler, name);
            int msg_idx = add_string_constant(compiler, name);
            if (msg_idx < 0) break;
            emit_byte(compiler, OP_RAISE);
            emit_u16(compiler, (uint16_t)msg_idx);
            emit_u16(compiler, (uint16_t)(int16_t)code);
            break;
        }
        case STMT_CURSOR_DECL: {
            CursorDeclStmt* d = &stmt->as.cursor_decl;
            int slot = add_local(compiler, d->name, (int)strlen(d->name), &type_cursor);
            if (slot < 0) {
                error(compiler, "too many locals");
                return;
            }
            /* Initialize cursor slot to a closed cursor object. */
            emit_constant(compiler, value_cursor(NULL));
            emit_set_local(compiler, slot);
            if (d->sql_query != NULL) {
                register_cursor_query(compiler, d->name, (int)strlen(d->name), d->sql_query,
                                      d->params, d->param_count);
            }
            break;
        }
        case STMT_CURSOR_OPEN: {
            CursorOpenStmt* o = &stmt->as.cursor_open;
            int slot = resolve_local(compiler, o->name, (int)strlen(o->name));
            if (slot < 0) {
                char msg[256];
                snprintf(msg, sizeof(msg), "Undefined variable '%s'", o->name);
                error(compiler, msg);
                return;
            }
            const char* query = o->sql_query;
            Expr** params = o->params;
            int param_count = o->param_count;
            if (query == NULL) {
                /* A plain `open c;` runs the declared query, and its ?var
                   placeholders are read here, at open time. */
                const CursorQuery* declared = find_cursor_query(compiler, o->name, (int)strlen(o->name));
                if (declared == NULL) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "Cursor '%s' has no associated query", o->name);
                    error(compiler, msg);
                    return;
                }
                query = declared->query;
                params = declared->params;
                param_count = declared->param_count;
            }
            for (int i = 0; i < param_count; i++) {
                compile_expr(compiler, params[i]);
                if (compiler->had_error) return;
                const char* name = params[i]->as.sql_param.name;
                Type* t = resolve_local_type(compiler, name, (int)strlen(name));
                if (t != NULL && t->kind == TYPE_STRING) {
                    emit_byte(compiler, OP_SQL_BIND_STRING);
                } else if (t != NULL && t->kind == TYPE_FLOAT) {
                    emit_byte(compiler, OP_SQL_BIND_FLOAT);
                } else {
                    emit_byte(compiler, OP_SQL_BIND_INT);
                }
            }
            int query_idx = add_string_constant(compiler, query);
            if (query_idx < 0) return;
            emit_get_local(compiler, slot);
            emit_byte(compiler, OP_CURSOR_OPEN);
            emit_u16(compiler, (uint16_t)query_idx);
            emit_u16(compiler, (uint16_t)stmt->loc.line);
            emit_set_local(compiler, slot);
            emit_byte(compiler, OP_POP);
            break;
        }
        case STMT_CURSOR_FETCH: {
            CursorFetchStmt* f = &stmt->as.cursor_fetch;
            int slot = resolve_local(compiler, f->name, (int)strlen(f->name));
            if (slot < 0) {
                char msg[256];
                snprintf(msg, sizeof(msg), "Undefined variable '%s'", f->name);
                error(compiler, msg);
                return;
            }
            int into_slots[64];
            if (f->into_count > 64) {
                error(compiler, "too many fetch targets");
                return;
            }
            for (int i = 0; i < f->into_count; i++) {
                int var_slot = resolve_local(compiler, f->into_vars[i], (int)strlen(f->into_vars[i]));
                if (var_slot < 0) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "Undefined variable '%s'", f->into_vars[i]);
                    error(compiler, msg);
                    return;
                }
                into_slots[i] = var_slot;
            }
            emit_get_local(compiler, slot);
            emit_byte(compiler, OP_CURSOR_FETCH);
            emit_byte(compiler, (uint8_t)f->into_count);
            for (int i = 0; i < f->into_count; i++) {
                emit_u16(compiler, (uint16_t)into_slots[i]);
            }
            break;
        }
        case STMT_CURSOR_CLOSE: {
            CursorCloseStmt* c = &stmt->as.cursor_close;
            int slot = resolve_local(compiler, c->name, (int)strlen(c->name));
            if (slot < 0) {
                char msg[256];
                snprintf(msg, sizeof(msg), "Undefined variable '%s'", c->name);
                error(compiler, msg);
                return;
            }
            emit_get_local(compiler, slot);
            emit_byte(compiler, OP_CURSOR_CLOSE);
            break;
        }
        case STMT_CASE: {
            CaseStmt* cs = &stmt->as.case_stmt;
            compile_expr(compiler, cs->selector);
            if (compiler->had_error) return;

            int* end_jumps = malloc(sizeof(int) * (size_t)(cs->branch_count + 1));
            if (end_jumps == NULL) {
                error(compiler, "out of memory");
                return;
            }
            int end_count = 0;

            for (int i = 0; i < cs->branch_count; i++) {
                emit_byte(compiler, OP_DUP);
                compile_expr(compiler, cs->values[i]);
                if (compiler->had_error) {
                    free(end_jumps);
                    return;
                }
                emit_byte(compiler, OP_EQ);
                int skip_branch = emit_jump(compiler, OP_JZ);

                compile_block(compiler, cs->blocks[i]);
                if (compiler->had_error) {
                    free(end_jumps);
                    return;
                }
                end_jumps[end_count++] = emit_jump(compiler, OP_JMP);

                patch_jump_to(compiler, skip_branch, compiler->chunk->count);
            }

            if (cs->else_block != NULL) {
                compile_block(compiler, cs->else_block);
                if (compiler->had_error) {
                    free(end_jumps);
                    return;
                }
            }

            int after_case = compiler->chunk->count;
            for (int i = 0; i < end_count; i++) {
                patch_jump_to(compiler, end_jumps[i], after_case);
            }
            emit_byte(compiler, OP_POP); /* pop selector */
            free(end_jumps);
            break;
        }
        case STMT_SQL_QUERY: {
            SqlStmt* s = &stmt->as.sql_stmt;
            if (s->into_count == 0) {
                error(compiler, "SELECT statement requires INTO");
                return;
            }
            for (int i = 0; i < s->into_count; i++) {
                int slot = resolve_local(compiler, s->into_vars[i], (int)strlen(s->into_vars[i]));
                if (slot < 0) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "Undefined variable '%s'", s->into_vars[i]);
                    error(compiler, msg);
                    return;
                }
            }
            int into_array = s->bulk_collect;
            if (s->into_count == 1 && !into_array) {
                Type* t = resolve_local_type(compiler, s->into_vars[0], (int)strlen(s->into_vars[0]));
                if (t != NULL && t->kind == TYPE_ARRAY && t->element_type != NULL && t->element_type->kind == TYPE_ROW) {
                    into_array = 1;
                }
            }
            for (int i = 0; i < s->param_count; i++) {
                compile_expr(compiler, s->params[i]);
                if (compiler->had_error) return;
                const char* name = s->params[i]->as.sql_param.name;
                Type* t = resolve_local_type(compiler, name, (int)strlen(name));
                if (t != NULL && t->kind == TYPE_STRING) {
                    emit_byte(compiler, OP_SQL_BIND_STRING);
                } else if (t != NULL && t->kind == TYPE_FLOAT) {
                    emit_byte(compiler, OP_SQL_BIND_FLOAT);
                } else {
                    emit_byte(compiler, OP_SQL_BIND_INT);
                }
            }
            int sql_idx = add_string_constant(compiler, s->sql);
            if (sql_idx < 0) return;
            emit_byte(compiler, OP_SQL);
            emit_u16(compiler, (uint16_t)sql_idx);
            emit_u16(compiler, (uint16_t)stmt->loc.line);

            if (into_array) {
                int slot = resolve_local(compiler, s->into_vars[0], (int)strlen(s->into_vars[0]));
                emit_byte(compiler, OP_SQL_TO_ARRAY);
                emit_set_local(compiler, slot);
                emit_byte(compiler, OP_POP);
            } else {
                int exit_jump = emit_jump(compiler, OP_SQL_NEXT);

                for (int i = 0; i < s->into_count; i++) {
                    int slot = resolve_local(compiler, s->into_vars[i], (int)strlen(s->into_vars[i]));
                    emit_byte(compiler, OP_SQL_GET_COLUMN);
                    emit_u16(compiler, (uint16_t)i);
                    emit_set_local(compiler, slot);
                    emit_byte(compiler, OP_POP);
                }

                int skip_error = emit_jump(compiler, OP_JMP);
                patch_jump(compiler, exit_jump);

                const char* msg = "SELECT INTO found no matching row";
                int msg_idx = add_string_constant(compiler, msg);
                if (msg_idx < 0) return;
                emit_byte(compiler, OP_RAISE);
                emit_u16(compiler, (uint16_t)msg_idx);
                emit_u16(compiler, (uint16_t)(int16_t)100);  /* no_data_found */

                patch_jump(compiler, skip_error);
            }
            break;
        }
        case STMT_SUBTYPE_DECL:
            /* No runtime code; subtype information is resolved during type checking. */
            break;
        case STMT_PRAGMA:
            if (stmt->as.pragma.name != NULL &&
                strcmp(stmt->as.pragma.name, "autonomous_transaction") == 0) {
                compiler->current_proc_autonomous = 1;
            }
            break;
        default:
            error(compiler, "unsupported statement");
            break;
    }
}

static int is_module_loaded(Compiler* compiler, const char* path) {
    for (int i = 0; i < compiler->loaded_count; i++) {
        if (strcmp(compiler->loaded_paths[i], path) == 0) return 1;
    }
    return 0;
}

static int is_module_loading(Compiler* compiler, const char* path) {
    for (int i = 0; i < compiler->loading_count; i++) {
        if (strcmp(compiler->loading_stack[i], path) == 0) return 1;
    }
    return 0;
}

static char* path_directory(const char* path) {
    char* copy = strdup(path);
    if (copy == NULL) return NULL;
    char* dir = dirname(copy);
    char* result = strdup(dir);
    free(copy);
    return result;
}

static char* resolve_import_path(const char* base_dir, const char* import_path) {
    if (os_path_is_absolute(import_path)) {
        return os_realpath(import_path);
    }

    char combined[4096];
    if (base_dir != NULL) {
        snprintf(combined, sizeof(combined), "%s/%s", base_dir, import_path);
    } else {
        if (getcwd(combined, sizeof(combined)) == NULL) return NULL;
        size_t len = strlen(combined);
        snprintf(combined + len, sizeof(combined) - len, "/%s", import_path);
    }
    return os_realpath(combined);
}

static int compiler_load_module(Compiler* compiler, const char* import_path, char* error, size_t error_size) {
    char* resolved = resolve_import_path(compiler->current_path, import_path);
    if (resolved == NULL) {
        compiler_format_error(compiler, error, error_size,
                              "Module not found: %s", import_path);
        return 0;
    }

    if (is_module_loading(compiler, resolved)) {
        free(resolved);
        compiler_format_error(compiler, error, error_size,
                              "Circular import of '%s'", import_path);
        return 0;
    }
    if (is_module_loaded(compiler, resolved)) {
        free(resolved);
        return 1;
    }
    if (compiler->loaded_count >= MAX_MODULES) {
        free(resolved);
        compiler_format_error(compiler, error, error_size,
                              "Too many imported modules");
        return 0;
    }
    if (!os_file_exists(resolved)) {
        free(resolved);
        compiler_format_error(compiler, error, error_size,
                              "Module not found: %s", import_path);
        return 0;
    }

    char* source = os_read_file(resolved);
    if (source == NULL) {
        free(resolved);
        compiler_format_error(compiler, error, error_size,
                              "Could not read module: %s", import_path);
        return 0;
    }

    if (compiler->loading_count >= MAX_LOADING) {
        free(resolved);
        free(source);
        compiler_format_error(compiler, error, error_size,
                              "Too many nested imports");
        return 0;
    }
    compiler->loading_stack[compiler->loading_count++] = resolved;

    int ok = compiler_compile_source(compiler, source, 0, resolved, error, error_size);

    compiler->loading_count--;
    if (ok) {
        compiler->loaded_paths[compiler->loaded_count++] = resolved;
    } else {
        free(resolved);
    }
    free(source);
    return ok;
}

static int register_package_procs(Compiler* compiler, Program* program, char* error_buf, size_t error_size) {
    for (int s = 0; s < program->spec_count; s++) {
        PackageSpecDecl* spec = &program->specs[s];
        for (int i = 0; i < spec->proc_count; i++) {
            ProcDecl* proc = &spec->procs[i];
            char* mangled = mangle_package_name(spec->name, proc->name);
            if (mangled == NULL) { error(compiler, "out of memory"); return 0; }
            Type** pt = NULL;
            ParamMode* pm = NULL;
            if (proc->param_count > 0) {
                pt = malloc(sizeof(Type*) * (size_t)proc->param_count);
                pm = malloc(sizeof(ParamMode) * (size_t)proc->param_count);
                if (pt == NULL || pm == NULL) {
                    free(pt); free(pm); free(mangled);
                    error(compiler, "out of memory"); return 0;
                }
                for (int p = 0; p < proc->param_count; p++) {
                    pt[p] = proc->params[p].type;
                    pm[p] = proc->params[p].mode;
                }
            }
            int idx = add_proc_entry(compiler, mangled, -1, proc->return_type, pt, pm, proc->param_count);
            if (idx >= 0) mark_proc_public(compiler, mangled);
            free(pt); free(pm); free(mangled);
            if (idx < 0) {
                compiler_format_error(compiler, error_buf, error_size,
                                      "Duplicate package member '%s.%s'", spec->name, proc->name);
                return 0;
            }
        }
        for (int i = 0; i < spec->func_count; i++) {
            ProcDecl* proc = &spec->funcs[i];
            char* mangled = mangle_package_name(spec->name, proc->name);
            if (mangled == NULL) { error(compiler, "out of memory"); return 0; }
            Type** pt = NULL;
            ParamMode* pm = NULL;
            if (proc->param_count > 0) {
                pt = malloc(sizeof(Type*) * (size_t)proc->param_count);
                pm = malloc(sizeof(ParamMode) * (size_t)proc->param_count);
                if (pt == NULL || pm == NULL) {
                    free(pt); free(pm); free(mangled);
                    error(compiler, "out of memory"); return 0;
                }
                for (int p = 0; p < proc->param_count; p++) {
                    pt[p] = proc->params[p].type;
                    pm[p] = proc->params[p].mode;
                }
            }
            int idx = add_proc_entry(compiler, mangled, -1, proc->return_type, pt, pm, proc->param_count);
            if (idx >= 0) mark_proc_public(compiler, mangled);
            free(pt); free(pm); free(mangled);
            if (idx < 0) {
                compiler_format_error(compiler, error_buf, error_size,
                                      "Duplicate package member '%s.%s'", spec->name, proc->name);
                return 0;
            }
        }
    }
    for (int b = 0; b < program->body_count; b++) {
        PackageBodyDecl* body = &program->bodies[b];
        for (int i = 0; i < body->proc_count; i++) {
            ProcDecl* proc = &body->procs[i];
            char* mangled = mangle_package_name(body->name, proc->name);
            if (mangled == NULL) { error(compiler, "out of memory"); return 0; }
            Type** pt = NULL;
            ParamMode* pm = NULL;
            if (proc->param_count > 0) {
                pt = malloc(sizeof(Type*) * (size_t)proc->param_count);
                pm = malloc(sizeof(ParamMode) * (size_t)proc->param_count);
                if (pt == NULL || pm == NULL) {
                    free(pt); free(pm); free(mangled);
                    error(compiler, "out of memory"); return 0;
                }
                for (int p = 0; p < proc->param_count; p++) {
                    pt[p] = proc->params[p].type;
                    pm[p] = proc->params[p].mode;
                }
            }
            int idx = add_proc_entry(compiler, mangled, -1, proc->return_type, pt, pm, proc->param_count);
            free(pt); free(pm); free(mangled);
            if (idx < 0 && idx != -2) {
                compiler_format_error(compiler, error_buf, error_size,
                                      "Duplicate package member '%s.%s'", body->name, proc->name);
                return 0;
            }
        }
        for (int i = 0; i < body->func_count; i++) {
            ProcDecl* proc = &body->funcs[i];
            char* mangled = mangle_package_name(body->name, proc->name);
            if (mangled == NULL) { error(compiler, "out of memory"); return 0; }
            Type** pt = NULL;
            ParamMode* pm = NULL;
            if (proc->param_count > 0) {
                pt = malloc(sizeof(Type*) * (size_t)proc->param_count);
                pm = malloc(sizeof(ParamMode) * (size_t)proc->param_count);
                if (pt == NULL || pm == NULL) {
                    free(pt); free(pm); free(mangled);
                    error(compiler, "out of memory"); return 0;
                }
                for (int p = 0; p < proc->param_count; p++) {
                    pt[p] = proc->params[p].type;
                    pm[p] = proc->params[p].mode;
                }
            }
            int idx = add_proc_entry(compiler, mangled, -1, proc->return_type, pt, pm, proc->param_count);
            free(pt); free(pm); free(mangled);
            if (idx < 0 && idx != -2) {
                compiler_format_error(compiler, error_buf, error_size,
                                      "Duplicate package member '%s.%s'", body->name, proc->name);
                return 0;
            }
        }
    }
    return 1;
}

static int register_trigger_procs(Compiler* compiler, Program* program, char* error_buf, size_t error_size) {
    for (int i = 0; i < program->trigger_count; i++) {
        TriggerDecl* trig = &program->triggers[i];
        size_t len = strlen(trig->name) + 11;
        char* mangled = malloc(len);
        if (mangled == NULL) { error(compiler, "out of memory"); return 0; }
        snprintf(mangled, len, "__trigger_%s", trig->name);
        /* Row-level triggers take two implicit row parameters, :new and
           :old, passed by the VM's row-trigger hook at fire time. */
        Type* row_param_types[2] = {&type_row, &type_row};
        ParamMode row_param_modes[2] = {PARAM_IN, PARAM_IN};
        int idx = add_proc_entry(compiler, mangled, -1, &type_int,
                                 trig->for_each_row ? row_param_types : NULL,
                                 trig->for_each_row ? row_param_modes : NULL,
                                 trig->for_each_row ? 2 : 0);
        if (idx < 0) {
            compiler_format_error(compiler, error_buf, error_size,
                                  "Duplicate trigger '%s'", trig->name);
            free(mangled);
            return 0;
        }
        if (compiler->trigger_count >= MAX_TRIGGERS) {
            compiler_format_error(compiler, error_buf, error_size, "Too many triggers");
            free(mangled);
            return 0;
        }
        TriggerEntry* entry = &compiler->triggers[compiler->trigger_count++];
        entry->proc_name = mangled;
        entry->timing = trig->timing;
        entry->event = trig->event;
        entry->for_each_row = trig->for_each_row;
        entry->dropped = 0;
        entry->table = malloc(strlen(trig->table) + 1);
        if (entry->table == NULL) {
            error(compiler, "out of memory");
            return 0;
        }
        strcpy(entry->table, trig->table);
    }
    return 1;
}

static void compile_trigger_members(Compiler* compiler, Program* program) {
    for (int i = 0; i < program->trigger_count; i++) {
        TriggerDecl* trig = &program->triggers[i];
        size_t len = strlen(trig->name) + 11;
        char* mangled = malloc(len);
        if (mangled == NULL) { error(compiler, "out of memory"); return; }
        snprintf(mangled, len, "__trigger_%s", trig->name);
        int idx = find_proc(compiler, mangled);
        free(mangled);
        if (idx < 0) continue;
        compiler->current_proc_autonomous = 0;
        compiler->procs[idx].offset = compiler->chunk->count;
        if (trig->for_each_row) {
            /* Implicit row-context parameters: the VM hook pushes the :new
               and :old row images as the first two stack slots. */
            add_local(compiler, "new", 3, &type_row);
            add_local(compiler, "old", 3, &type_row);
        }
        compile_block(compiler, trig->body);
        compiler->procs[idx].autonomous_transaction = compiler->current_proc_autonomous;
        if (compiler->had_error) return;
        emit_constant(compiler, value_int(0));
        emit_byte(compiler, OP_RETURN);
        compiler->local_count = 0;
        compiler->scope_depth = 0;
    }
}

/* Register struct methods as hidden procs named __struct_<Struct>_<method>
 * with an implicit first parameter 'self' of the struct type. */
static int register_struct_methods(Compiler* compiler, Program* program, char* error_buf, size_t error_size) {
    for (int s = 0; s < program->struct_count; s++) {
        StructDecl* decl = &program->structs[s];
        for (int m = 0; m < decl->method_count; m++) {
            ProcDecl* method = &decl->methods[m];
            char* mangled = mangle_struct_method(decl->name, method->name);
            if (mangled == NULL) { error(compiler, "out of memory"); return 0; }
            int total = method->param_count + 1;
            Type** pt = malloc(sizeof(Type*) * (size_t)total);
            ParamMode* pm = malloc(sizeof(ParamMode) * (size_t)total);
            Type* self_type = type_new(TYPE_STRUCT, NULL);
            if (pt == NULL || pm == NULL || self_type == NULL) {
                free(pt); free(pm); type_free(self_type); free(mangled);
                error(compiler, "out of memory"); return 0;
            }
            self_type->struct_name = malloc(strlen(decl->name) + 1);
            if (self_type->struct_name == NULL) {
                free(pt); free(pm); type_free(self_type); free(mangled);
                error(compiler, "out of memory"); return 0;
            }
            strcpy(self_type->struct_name, decl->name);
            pt[0] = self_type;
            pm[0] = PARAM_IN;
            for (int p = 0; p < method->param_count; p++) {
                pt[p + 1] = method->params[p].type;
                pm[p + 1] = method->params[p].mode;
            }
            int idx = add_proc_entry(compiler, mangled, -1, method->return_type,
                                     pt, pm, total);
            free(pt); free(pm); type_free(self_type); free(mangled);
            if (idx < 0) {
                compiler_format_error(compiler, error_buf, error_size,
                                      "Duplicate method '%s' in struct '%s'",
                                      method->name, decl->name);
                return 0;
            }
        }
    }
    return 1;
}

static void compile_struct_methods(Compiler* compiler, Program* program) {
    for (int s = 0; s < program->struct_count; s++) {
        StructDecl* decl = &program->structs[s];
        if (decl->method_count == 0) continue;
        Type* self_type = type_new(TYPE_STRUCT, NULL);
        if (self_type == NULL) { error(compiler, "out of memory"); return; }
        self_type->struct_name = malloc(strlen(decl->name) + 1);
        if (self_type->struct_name == NULL) {
            type_free(self_type);
            error(compiler, "out of memory");
            return;
        }
        strcpy(self_type->struct_name, decl->name);
        for (int m = 0; m < decl->method_count; m++) {
            ProcDecl* method = &decl->methods[m];
            char* mangled = mangle_struct_method(decl->name, method->name);
            if (mangled == NULL) {
                type_free(self_type);
                error(compiler, "out of memory");
                return;
            }
            int idx = find_proc(compiler, mangled);
            free(mangled);
            if (idx < 0) continue;
            compiler->current_proc_autonomous = 0;
            compiler->procs[idx].offset = compiler->chunk->count;
            add_local(compiler, "self", 4, self_type);
            for (int p = 0; p < method->param_count; p++) {
                add_local(compiler, method->params[p].name,
                          (int)strlen(method->params[p].name), method->params[p].type);
            }
            compiler->current_struct = decl;
            compile_block(compiler, method->body);
            compiler->current_struct = NULL;
            compiler->procs[idx].autonomous_transaction = compiler->current_proc_autonomous;
            if (compiler->had_error) { type_free(self_type); return; }
            emit_byte(compiler, OP_RETURN);
            compiler->local_count = 0;
            compiler->scope_depth = 0;
        }
        type_free(self_type);
    }
}

static void emit_trigger_calls(Compiler* compiler, int timing, int event, const char* table) {
    for (int i = 0; i < compiler->trigger_count; i++) {
        TriggerEntry* entry = &compiler->triggers[i];
        /* Row-level triggers are not fired here: they run per affected row
           through the VM's driver row-trigger hook. */
        if (entry->for_each_row) continue;
        if (entry->timing == timing && entry->event == event && !entry->dropped &&
            trigger_name_equals(entry->table, table)) {
            emit_call(compiler, entry->proc_name, 0);
            emit_byte(compiler, OP_POP);
        }
    }
}

static void compile_package_initializers(Compiler* compiler, Program* program) {
    for (int b = 0; b < program->body_count; b++) {
        PackageBodyDecl* body = &program->bodies[b];
        const char* saved_package = compiler->current_package;
        compiler->current_package = body->name;
        for (int i = 0; i < body->var_count; i++) {
            compile_stmt(compiler, body->vars[i]);
        }
        compiler->current_package = saved_package;
    }
}

/* Wraps this file's package-body initializers (if it declares any) in a
 * hidden 0-arg proc and records it in compiler->module_init_procs, so the
 * bootstrap sequence built for the outermost (main) file can call it
 * regardless of whether this file IS the main file or one of its imports.
 *
 * This matters because compile_package_initializers() is also what
 * registers each package-level variable as a global (add_global(), inside
 * compile_stmt()'s VarDeclStmt case) — before this function existed, that
 * step only ran for the main file (see the call site below), so a package
 * declared in an imported module had no global slot at all: referencing
 * its state from within the package's own procs failed to compile
 * ("Undefined variable"), and even where it happened to resolve, the
 * initializer that would have set it never ran. Wrapping every file's
 * initializers in a callable proc, invoked once each in load order, fixes
 * both: the global gets registered when this proc is compiled, and it
 * gets run when the bootstrap sequence calls it. */
static int compile_module_init_proc(Compiler* compiler, Program* program, char* error_buf, size_t error_size) {
    if (program->body_count == 0) return 1;
    if (compiler->module_init_count >= MAX_MODULE_INITS) {
        compiler_format_error(compiler, error_buf, error_size,
                              "Too many imported modules with package state");
        return 0;
    }

    char name[32];
    snprintf(name, sizeof(name), "__module_init_%d",
             compiler->module_init_base + compiler->module_init_count);
    char* mangled = strdup(name);
    if (mangled == NULL) { error(compiler, "out of memory"); return 0; }

    int idx = add_proc_entry(compiler, mangled, -1, &type_int, NULL, NULL, 0);
    if (idx < 0) {
        compiler_format_error(compiler, error_buf, error_size, "Too many procedures");
        free(mangled);
        return 0;
    }

    compiler->procs[idx].offset = compiler->chunk->count;
    compile_package_initializers(compiler, program);
    if (compiler->had_error) {
        if (error_buf != NULL && error_size > 0) {
            strncpy(error_buf, compiler->error_message, error_size - 1);
            error_buf[error_size - 1] = '\0';
        }
        free(mangled);
        return 0;
    }
    emit_constant(compiler, value_int(0));
    emit_byte(compiler, OP_RETURN);
    compiler->local_count = 0;
    compiler->scope_depth = 0;

    compiler->module_init_procs[compiler->module_init_count++] = mangled;
    return 1;
}

static void compile_package_members(Compiler* compiler, Program* program) {
    for (int b = 0; b < program->body_count; b++) {
        PackageBodyDecl* body = &program->bodies[b];
        const char* saved_package = compiler->current_package;
        compiler->current_package = body->name;

        for (int i = 0; i < body->proc_count; i++) {
            ProcDecl* proc = &body->procs[i];
            char* mangled = mangle_package_name(body->name, proc->name);
            if (mangled == NULL) { error(compiler, "out of memory"); compiler->current_package = saved_package; return; }
            int idx = find_proc(compiler, mangled);
            free(mangled);
            if (idx < 0) continue;
            compiler->current_proc_autonomous = 0;
            compiler->procs[idx].offset = compiler->chunk->count;
            for (int p = 0; p < proc->param_count; p++) {
                add_local(compiler, proc->params[p].name, (int)strlen(proc->params[p].name), proc->params[p].type);
            }
            compile_block(compiler, proc->body);
            compiler->procs[idx].autonomous_transaction = compiler->current_proc_autonomous;
            if (compiler->had_error) { compiler->current_package = saved_package; return; }
            emit_byte(compiler, OP_RETURN);
            compiler->local_count = 0;
            compiler->scope_depth = 0;
        }

        for (int i = 0; i < body->func_count; i++) {
            ProcDecl* proc = &body->funcs[i];
            char* mangled = mangle_package_name(body->name, proc->name);
            if (mangled == NULL) { error(compiler, "out of memory"); compiler->current_package = saved_package; return; }
            int idx = find_proc(compiler, mangled);
            free(mangled);
            if (idx < 0) continue;
            compiler->current_proc_autonomous = 0;
            compiler->procs[idx].offset = compiler->chunk->count;
            for (int p = 0; p < proc->param_count; p++) {
                add_local(compiler, proc->params[p].name, (int)strlen(proc->params[p].name), proc->params[p].type);
            }
            compile_block(compiler, proc->body);
            compiler->procs[idx].autonomous_transaction = compiler->current_proc_autonomous;
            if (compiler->had_error) { compiler->current_package = saved_package; return; }
            emit_byte(compiler, OP_RETURN);
            compiler->local_count = 0;
            compiler->scope_depth = 0;
        }

        compiler->current_package = saved_package;
    }
}

/* -------------------------------------------------------------------------- */
/* Conditional compilation preprocessor                                        */
/*                                                                            */
/* Line-oriented directives:                                                  */
/*   $define NAME / $undefine NAME                                            */
/*   $if NAME $then ... $elsif NAME $then ... $else ... $end                  */
/* Excluded regions are blanked in place (newlines kept) so diagnostics keep  */
/* their line numbers. Undefined flags evaluate to false.                     */
/* -------------------------------------------------------------------------- */

#define CC_MAX_FLAGS AUSPEX_CC_MAX_FLAGS
/* Room for the built-in PLATFORM_* flags on top of the command line's. */
#define CC_FLAG_CAPACITY (CC_MAX_FLAGS + 2)
#define CC_NAME_MAX AUSPEX_CC_FLAG_NAME_MAX
#define CC_MAX_DEPTH 16

typedef struct {
    int parent_active;
    int branch_taken;
    int branch_active;
    int saw_else;
} CCFrame;

static int cc_is_name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static int cc_flag_defined(char flags[][CC_NAME_MAX], int flag_count, const char* name) {
    for (int i = 0; i < flag_count; i++) {
        if (strcmp(flags[i], name) == 0) return 1;
    }
    return 0;
}

/* Defined in every compilation, so a program can choose, say, which C
   library external_call loads on the platform it runs on. */
static const char* const cc_platform_flags[] = {
#if defined(_WIN32)
    "PLATFORM_WINDOWS",
#elif defined(__APPLE__)
    "PLATFORM_MACOS", "PLATFORM_POSIX",
#elif defined(__linux__)
    "PLATFORM_LINUX", "PLATFORM_POSIX",
#else
    "PLATFORM_POSIX",
#endif
};

static int cc_seed_flags(char flags[][CC_NAME_MAX], int* flag_count,
                         const CompileOptions* options, const char* source_path,
                         char* error_buf, size_t error_size) {
    for (size_t i = 0; i < sizeof(cc_platform_flags) / sizeof(cc_platform_flags[0]); i++) {
        if (!cc_flag_defined(flags, *flag_count, cc_platform_flags[i])) {
            strcpy(flags[(*flag_count)++], cc_platform_flags[i]);
        }
    }
    if (options == NULL) return 1;
    if (options->conditional_flag_count < 0 ||
        options->conditional_flag_count > CC_MAX_FLAGS ||
        (options->conditional_flag_count > 0 && options->conditional_flags == NULL)) {
        format_error(error_buf, error_size, source_path, 0, 0,
                     "conditional compilation: too many command-line flags");
        return 0;
    }

    for (int i = 0; i < options->conditional_flag_count; i++) {
        const char* name = options->conditional_flags[i];
        size_t len = name != NULL ? strlen(name) : 0;
        if (len == 0 || len >= CC_NAME_MAX) {
            format_error(error_buf, error_size, source_path, 0, 0,
                         "conditional compilation: invalid command-line flag name");
            return 0;
        }
        for (size_t j = 0; j < len; j++) {
            if (!cc_is_name_char(name[j])) {
                format_error(error_buf, error_size, source_path, 0, 0,
                             "conditional compilation: invalid command-line flag name");
                return 0;
            }
        }
        if (!cc_flag_defined(flags, *flag_count, name)) {
            strcpy(flags[(*flag_count)++], name);
        }
    }
    return 1;
}

/* Returns a malloc'd processed copy of source (same length), or NULL and
 * writes an error message into error_buf. Declared in compiler.h so the
 * fuzz harness can drive the preprocessor directly. */
char* cc_preprocess(const char* source, const char* source_path,
                    const CompileOptions* options,
                    char* error_buf, size_t error_size) {
    size_t len = strlen(source);
    char* out = malloc(len + 1);
    if (out == NULL) return NULL;
    memcpy(out, source, len + 1);

    char flags[CC_FLAG_CAPACITY][CC_NAME_MAX];
    int flag_count = 0;
    if (!cc_seed_flags(flags, &flag_count, options, source_path, error_buf, error_size)) {
        free(out);
        return NULL;
    }
    CCFrame stack[CC_MAX_DEPTH];
    int depth = 0;
    int active = 1;

    const char* p = source;
    /* Number lines so the user's own source starts at 1 (see
       CompileOptions.line_offset). */
    int line_no = 1 - (options != NULL ? options->line_offset : 0);
    while (*p != '\0') {
        const char* line_start = p;
        const char* nl = strchr(p, '\n');
        const char* line_end = nl != NULL ? nl : p + strlen(p);

        /* Find first non-blank char of the line. */
        const char* q = line_start;
        while (q < line_end && (*q == ' ' || *q == '\t' || *q == '\r')) q++;

        int is_directive = (q < line_end && *q == '$');
        if (is_directive) {
            char kw[16];
            char arg[CC_NAME_MAX];
            kw[0] = '\0';
            arg[0] = '\0';
            q++; /* skip '$' */
            int ki = 0;
            while (q < line_end && cc_is_name_char(*q) && ki < 15) kw[ki++] = *q++;
            kw[ki] = '\0';
            while (q < line_end && (*q == ' ' || *q == '\t')) q++;
            int ai = 0;
            while (q < line_end && cc_is_name_char(*q) && ai < CC_NAME_MAX - 1) arg[ai++] = *q++;
            arg[ai] = '\0';

            if (strcmp(kw, "define") == 0) {
                if (active && arg[0] != '\0' && !cc_flag_defined(flags, flag_count, arg)) {
                    if (flag_count < CC_FLAG_CAPACITY) {
                        strcpy(flags[flag_count++], arg);
                    }
                }
            } else if (strcmp(kw, "undefine") == 0) {
                if (active) {
                    for (int i = 0; i < flag_count; i++) {
                        if (strcmp(flags[i], arg) == 0) {
                            memmove(flags[i], flags[i + 1], (size_t)(flag_count - i - 1) * CC_NAME_MAX);
                            flag_count--;
                            break;
                        }
                    }
                }
            } else if (strcmp(kw, "if") == 0) {
                if (depth >= CC_MAX_DEPTH) {
                    free(out);
                    format_error(error_buf, error_size, source_path, line_no, 1,
                                 "conditional compilation: too many nested $if blocks");
                    return NULL;
                }
                CCFrame* f = &stack[depth++];
                f->parent_active = active;
                f->branch_active = active && arg[0] != '\0' && cc_flag_defined(flags, flag_count, arg);
                f->branch_taken = f->branch_active;
                f->saw_else = 0;
                active = f->branch_active;
            } else if (strcmp(kw, "elsif") == 0) {
                if (depth == 0) {
                    free(out);
                    format_error(error_buf, error_size, source_path, line_no, 1,
                                 "conditional compilation: $elsif without $if");
                    return NULL;
                }
                CCFrame* f = &stack[depth - 1];
                if (f->branch_taken || !f->parent_active) {
                    f->branch_active = 0;
                } else {
                    f->branch_active = arg[0] != '\0' && cc_flag_defined(flags, flag_count, arg);
                    f->branch_taken = f->branch_taken || f->branch_active;
                }
                active = f->branch_active;
            } else if (strcmp(kw, "else") == 0) {
                if (depth == 0) {
                    free(out);
                    format_error(error_buf, error_size, source_path, line_no, 1,
                                 "conditional compilation: $else without $if");
                    return NULL;
                }
                CCFrame* f = &stack[depth - 1];
                if (f->saw_else) {
                    free(out);
                    format_error(error_buf, error_size, source_path, line_no, 1,
                                 "conditional compilation: duplicate $else");
                    return NULL;
                }
                f->saw_else = 1;
                f->branch_active = f->parent_active && !f->branch_taken;
                f->branch_taken = 1;
                active = f->branch_active;
            } else if (strcmp(kw, "end") == 0) {
                if (depth == 0) {
                    free(out);
                    format_error(error_buf, error_size, source_path, line_no, 1,
                                 "conditional compilation: $end without $if");
                    return NULL;
                }
                active = stack[--depth].parent_active;
            } else {
                free(out);
                format_error(error_buf, error_size, source_path, line_no, 1,
                             "conditional compilation: unknown directive");
                return NULL;
            }

            /* Blank the directive line itself. */
            for (const char* r = line_start; r < line_end; r++) {
                out[r - source] = ' ';
            }
        } else if (!active) {
            /* Blank excluded source, keeping newlines for line numbers. */
            for (const char* r = line_start; r < line_end; r++) {
                out[r - source] = ' ';
            }
        }

        p = nl != NULL ? nl + 1 : line_end;
        line_no++;
    }

    if (depth != 0) {
        free(out);
        format_error(error_buf, error_size, source_path, line_no, 1,
                     "conditional compilation: unterminated $if (missing $end)");
        return NULL;
    }
    return out;
}

static int do_compile_source(Compiler* compiler, const char* source, int is_main, char* error, size_t error_size) {
    /* Only the main source is preceded by prepended declarations; an imported
       module is a file of its own and is numbered from its first line. */
    int line_offset = (is_main && compiler->options != NULL) ? compiler->options->line_offset : 0;
    CompileOptions file_options;
    const CompileOptions* options = compiler->options;
    if (options != NULL && options->line_offset != line_offset) {
        file_options = *options;
        file_options.line_offset = line_offset;
        options = &file_options;
    }

    char cc_error[256] = {0};
    char* processed = cc_preprocess(source, compiler->source_path, options,
                                    cc_error, sizeof(cc_error));
    if (processed == NULL) {
        if (error != NULL && error_size > 0) {
            if (cc_error[0] != '\0') {
                strncpy(error, cc_error, error_size - 1);
                error[error_size - 1] = '\0';
            } else {
                strncpy(error, "conditional compilation: out of memory", error_size - 1);
                error[error_size - 1] = '\0';
            }
        }
        return 0;
    }
    char parse_error[256] = {0};
    Program* program = parse_with_path_from_line(processed, compiler->source_path, 1 - line_offset,
                                                 parse_error, sizeof(parse_error));
    free(processed);
    if (program == NULL) {
        if (error != NULL && error_size > 0) {
            strncpy(error, parse_error, error_size - 1);
            error[error_size - 1] = '\0';
        }
        return 0;
    }

    if (is_main) {
        compiler->entry_jump_patch = emit_jump(compiler, OP_JMP);
    }

    for (int i = 0; i < program->import_count; i++) {
        Stmt* import_stmt = program->imports[i];
        const char* path = import_stmt->as.import_stmt.path;
        compiler->current_line = import_stmt->loc.line;
        compiler->current_column = import_stmt->loc.column;
        if (!compiler_load_module(compiler, path, error, error_size)) {
            free_program(program);
            return 0;
        }
    }

    for (int i = 0; i < program->proc_count; i++) {
        ProcDecl* proc = &program->procs[i];
        Type** pts = NULL;
        ParamMode* pms = NULL;
        if (proc->param_count > 0) {
            pts = malloc(sizeof(Type*) * (size_t)proc->param_count);
            pms = malloc(sizeof(ParamMode) * (size_t)proc->param_count);
            for (int p = 0; p < proc->param_count; p++) {
                pts[p] = proc->params[p].type;
                pms[p] = proc->params[p].mode;
            }
        }
        int idx = add_proc_entry(compiler, proc->name, -1, proc->return_type, pts, pms, proc->param_count);
        free(pts);
        free(pms);
        if (idx < 0) {
            if (error != NULL && error_size > 0) {
                if (idx == -1) {
                    compiler_format_error(compiler, error, error_size,
                                          "Out of memory registering procedure '%s'", proc->name);
                } else if (idx == -2) {
                    compiler_format_error(compiler, error, error_size,
                                          "Duplicate procedure '%s'", proc->name);
                } else {
                    compiler_format_error(compiler, error, error_size,
                                          "Too many procedures");
                }
            }
            free_program(program);
            return 0;
        }
        if (is_main && strcmp(proc->name, "main") == 0) {
            compiler->entry_index = idx;
        }
    }

    if (!register_package_procs(compiler, program, error, error_size)) {
        free_program(program);
        return 0;
    }

    if (!register_trigger_procs(compiler, program, error, error_size)) {
        free_program(program);
        return 0;
    }

    if (!register_struct_methods(compiler, program, error, error_size)) {
        free_program(program);
        return 0;
    }

    ProcSignature* sigs = malloc(sizeof(ProcSignature) * (size_t)compiler->proc_count);
    if (sigs == NULL) {
        compiler_format_error(compiler, error, error_size, "out of memory");
        free_program(program);
        return 0;
    }
    for (int i = 0; i < compiler->proc_count; i++) {
        sigs[i].name = compiler->procs[i].name;
        sigs[i].return_type = compiler->procs[i].return_type;
        sigs[i].param_types = compiler->procs[i].param_types;
        sigs[i].param_modes = compiler->procs[i].param_modes;
        sigs[i].param_count = compiler->procs[i].param_count;
        sigs[i].is_public = compiler->procs[i].is_public;
    }

    if (!typecheck_program(program, sigs, compiler->proc_count, compiler->ctx,
                           compiler->source_path, error, error_size,
                           NULL, NULL, 0)) {
        free(sigs);
        free_program(program);
        return 0;
    }
    free(sigs);

    /* Every file that declares package-level state gets a callable
       initializer proc, compiled before compile_package_members() below —
       that's also where each package-level variable is registered as a
       global (see compile_module_init_proc()'s comment), and package
       procs/funcs referencing it need that global to already exist. Main
       is included here too, so it's invoked from the same bootstrap list
       further down rather than inlined as a special case. */
    if (!compile_module_init_proc(compiler, program, error, error_size)) {
        free_program(program);
        return 0;
    }

    compile_package_members(compiler, program);
    if (compiler->had_error) {
        if (error != NULL && error_size > 0) {
            strncpy(error, compiler->error_message, error_size - 1);
            error[error_size - 1] = '\0';
        }
        free_program(program);
        return 0;
    }

    compile_trigger_members(compiler, program);
    if (compiler->had_error) {
        if (error != NULL && error_size > 0) {
            strncpy(error, compiler->error_message, error_size - 1);
            error[error_size - 1] = '\0';
        }
        free_program(program);
        return 0;
    }

    compile_struct_methods(compiler, program);
    if (compiler->had_error) {
        if (error != NULL && error_size > 0) {
            strncpy(error, compiler->error_message, error_size - 1);
            error[error_size - 1] = '\0';
        }
        free_program(program);
        return 0;
    }

    for (int i = 0; i < program->proc_count; i++) {
        ProcDecl* proc = &program->procs[i];
        int idx = find_proc(compiler, proc->name);
        compiler->current_proc_autonomous = 0;
        compiler->procs[idx].offset = compiler->chunk->count;
        for (int p = 0; p < proc->param_count; p++) {
            add_local(compiler, proc->params[p].name, (int)strlen(proc->params[p].name), proc->params[p].type);
        }
        compile_block(compiler, proc->body);
        compiler->procs[idx].autonomous_transaction = compiler->current_proc_autonomous;
        if (compiler->had_error) {
            if (error != NULL && error_size > 0) {
                strncpy(error, compiler->error_message, error_size - 1);
                error[error_size - 1] = '\0';
            }
            free_program(program);
            return 0;
        }
        emit_byte(compiler, OP_RETURN);
        compiler->local_count = 0;
        compiler->scope_depth = 0;
    }

    if (is_main) {
        int init_start = compiler->chunk->count;
        for (int i = 0; i < compiler->module_init_count; i++) {
            emit_call(compiler, compiler->module_init_procs[i], 0);
            emit_byte(compiler, OP_POP);
        }
        int init_to_main_jump = emit_jump(compiler, OP_JMP);

        int main_entry = compiler->chunk->count;
        if (compiler->entry_index >= 0) {
            emit_call(compiler, compiler->procs[compiler->entry_index].name, 0);
            emit_byte(compiler, OP_RETURN);
        }
        if (compiler->entry_jump_patch >= 0) {
            patch_jump_to(compiler, compiler->entry_jump_patch, init_start);
        }
        patch_jump_to(compiler, init_to_main_jump, main_entry);
    }

    free_program(program);
    return 1;
}

static int compiler_compile_source(Compiler* compiler, const char* source, int is_main, const char* path, char* error, size_t error_size) {
    char* saved = compiler->current_path;
    const char* saved_source = compiler->source_path;
    char* dir = NULL;
    if (path != NULL) {
        dir = path_directory(path);
        compiler->current_path = dir;
        compiler->source_path = path;
    }
    int ok = do_compile_source(compiler, source, is_main, error, error_size);
    if (dir != NULL) free(dir);
    compiler->current_path = saved;
    compiler->source_path = saved_source;
    return ok;
}

static void free_global_names(Compiler* compiler) {
    for (int i = 0; i < compiler->global_count; i++) {
        free((void*)compiler->global_names[i]);
        compiler->global_names[i] = NULL;
    }
    compiler->global_count = 0;
}

int compile_with_options(const char* source, Chunk* chunk, const char* path,
                         char* error, size_t error_size, struct Context* ctx,
                         const CompileOptions* options) {
    Compiler compiler;
    compiler.chunk = chunk;
    compiler.locals = NULL;
    compiler.local_count = 0;
    compiler.local_capacity = 0;
    compiler.cursor_queries = NULL;
    compiler.cursor_query_count = 0;
    compiler.cursor_query_capacity = 0;
    compiler.scope_depth = 0;
    compiler.proc_count = 0;
    compiler.patch_count = 0;
    compiler.had_error = 0;
    compiler.error_message[0] = '\0';
    compiler.entry_index = -1;
    compiler.loaded_count = 0;
    compiler.loading_count = 0;
    compiler.current_path = NULL;
    compiler.source_path = path;
    compiler.ctx = ctx;
    compiler.loop_count = 0;
    compiler.current_line = 0;
    compiler.current_column = 0;
    compiler.global_count = 0;
    compiler.current_package = NULL;
    compiler.entry_jump_patch = -1;
    compiler.current_proc_autonomous = 0;
    compiler.cursor_query_count = 0;
    compiler.exception_count = 0;
    compiler.trigger_count = 0;
    compiler.current_struct = NULL;
    compiler.module_init_count = 0;
    compiler.module_init_base = 0;
    compiler.options = options;
    compiler.record_call_sites = 0;
    compiler.call_sites = NULL;
    compiler.call_site_count = 0;
    compiler.call_site_capacity = 0;
    add_exception(&compiler, "no_data_found", 100);
    add_exception(&compiler, "too_many_rows", -1422);
    for (int i = 0; i < MAX_GLOBALS; i++) compiler.global_names[i] = NULL;
    chunk->source_path = path;

    if (!compiler_compile_source(&compiler, source, 1, path, error, error_size)) {
        free_exception_entries(&compiler);
        free_trigger_entries(&compiler);
        free_module_init_entries(&compiler);
        free_proc_entries(&compiler);
        free_global_names(&compiler);
        free(compiler.locals);
        free(compiler.cursor_queries);
        for (int i = 0; i < compiler.patch_count; i++) free((void*)compiler.patches[i].name);
        for (int i = 0; i < compiler.loaded_count; i++) free(compiler.loaded_paths[i]);
        return 0;
    }

    if (compiler.entry_index < 0) {
        if (error != NULL && error_size > 0) {
            format_error(error, error_size, compiler.source_path, 0, 0,
                         "No main procedure");
        }
        free_exception_entries(&compiler);
        free_trigger_entries(&compiler);
        free_module_init_entries(&compiler);
        free_proc_entries(&compiler);
        free_global_names(&compiler);
        free(compiler.locals);
        free(compiler.cursor_queries);
        for (int i = 0; i < compiler.patch_count; i++) free((void*)compiler.patches[i].name);
        for (int i = 0; i < compiler.loaded_count; i++) free(compiler.loaded_paths[i]);
        return 0;
    }

    for (int i = 0; i < compiler.patch_count; i++) {
        int idx = find_proc(&compiler, compiler.patches[i].name);
        if (idx < 0) {
            if (error != NULL && error_size > 0) {
                char msg[256];
                snprintf(msg, sizeof(msg), "Undefined procedure '%s'", compiler.patches[i].name);
                format_error(error, error_size, compiler.source_path, 0, 0, msg);
            }
            free_exception_entries(&compiler);
            free_trigger_entries(&compiler);
            free_module_init_entries(&compiler);
            free_proc_entries(&compiler);
            free_global_names(&compiler);
            free(compiler.locals);
            free(compiler.cursor_queries);
            for (int j = 0; j < compiler.patch_count; j++) free((void*)compiler.patches[j].name);
            for (int j = 0; j < compiler.loaded_count; j++) free(compiler.loaded_paths[j]);
            return 0;
        }
        patch_call(chunk, compiler.patches[i].offset, compiler.procs[idx].offset);
        if (compiler.procs[idx].autonomous_transaction) {
            int op_offset = compiler.patches[i].offset - 1;
            if (op_offset >= 0 && op_offset < chunk->count) {
                if (chunk->code[op_offset] == OP_CALL) {
                    chunk->code[op_offset] = OP_CALL_AUTONOMOUS;
                } else if (chunk->code[op_offset] == OP_CALL_OUT) {
                    chunk->code[op_offset] = OP_CALL_OUT_AUTONOMOUS;
                }
            }
        }
    }

    /* Publish the runtime trigger registry: dynamic SQL (execute_immediate,
       dbms_sql.execute) fires triggers through these chunk entries. Triggers
       dropped by a `drop trigger` statement are included; the emitted
       OP_DROP_TRIGGER disables them at execution time. */
    for (int i = 0; i < compiler.trigger_count; i++) {
        TriggerEntry* te = &compiler.triggers[i];
        int idx = find_proc(&compiler, te->proc_name);
        if (idx < 0 || compiler.procs[idx].offset < 0) continue;
        chunk_add_trigger(chunk, te->proc_name + 10 /* skip "__trigger_" */,
                          te->timing, te->event, te->table,
                          compiler.procs[idx].offset, te->for_each_row);
    }

    free_exception_entries(&compiler);
    free_trigger_entries(&compiler);
    free_module_init_entries(&compiler);
    free_proc_entries(&compiler);
    free_global_names(&compiler);
    free(compiler.locals);
    free(compiler.cursor_queries);
    for (int i = 0; i < compiler.patch_count; i++) free((void*)compiler.patches[i].name);
    for (int i = 0; i < compiler.loaded_count; i++) free(compiler.loaded_paths[i]);
    return 1;
}

int compile_with_context_and_path(const char* source, Chunk* chunk, const char* path,
                                  char* error, size_t error_size, struct Context* ctx) {
    return compile_with_options(source, chunk, path, error, error_size, ctx, NULL);
}

int compile_with_context(const char* source, Chunk* chunk, char* error, size_t error_size, struct Context* ctx) {
    return compile_with_context_and_path(source, chunk, NULL, error, error_size, ctx);
}

int compile_with_path(const char* source, Chunk* chunk, const char* path, char* error, size_t error_size) {
    return compile_with_context_and_path(source, chunk, path, error, error_size, NULL);
}

int compile(const char* source, Chunk* chunk, char* error, size_t error_size) {
    return compile_with_context(source, chunk, error, error_size, NULL);
}


/* ---------------------------------------------------------------------------
 * Incremental (REPL) compilation — Phase 13 #37.
 *
 * A ReplCompiler persists the procedure table (name -> chunk offset +
 * signature), the global-name table, the trigger table, and the top-level
 * main-body local table across compile calls. Each REPL input is compiled as
 * ONE fragment and appended to a single growing Chunk:
 *
 *  - Statement/expression inputs arrive wrapped by the REPL as
 *    "proc main() -> int { <main body so far> ... }" (the exact text the REPL
 *    always built, minus the procedures prefix). Only the NEW top-level
 *    statements plus the synthetic return are code-generated; the typechecker
 *    sees previous top-level locals through a seed scope. The fragment ends
 *    with the return's OP_RETURN and runs at frame_count 0 via
 *    vm_interpret_from().
 *
 *  - Definition inputs (proc/package, i.e. the REPL's `procedures` buffer
 *    entries) compile through the standard program path minus the entry
 *    jump/bootstrap. Package-level state initializers become a
 *    __module_init_N proc that the fragment's run sequence calls once (the
 *    old engine re-ran all inits before every input).
 *
 * On fragment failure every table is rolled back to the watermarks taken at
 * fragment start (the old engine discarded the whole compile), and the REPL
 * reproduces the observable poison semantics (reprinting the first error for
 * later inputs) at the repl.c level.
 * ------------------------------------------------------------------------- */

struct ReplCompiler {
    Compiler compiler;
    int      exec_offset;        /* chunk offset the current fragment runs from */
    int      stmt_watermark;     /* wrapper main stmts already code-generated */
    int      module_init_total;  /* session-wide __module_init_N counter */
    int      published_triggers; /* chunk->triggers entries already published */
    /* Seed arrays mirroring the persistent top-level locals for the
       typechecker's wrapper-main scope. Entries are owned copies. */
    char**   seed_names;
    Type**   seed_types;
    int      seed_count;
    int      seed_capacity;
    /* Rollback watermarks, captured at fragment start. */
    int      mark_proc_count;
    int      mark_global_count;
    int      mark_trigger_count;
    int      mark_local_count;
    int      mark_chunk_count;
    int      mark_lines_count;
    int      mark_columns_count;
    int      mark_constants_count;
    int      mark_call_site_count;
    int      mark_stmt_watermark;
    /* Procs from earlier inputs that this fragment redefines, with what the
       table held before, so a rollback can put it back. */
    struct {
        int idx;
        int old_offset;
        int old_autonomous;
    } redefs[MAX_PROCS];
    int      redef_count;
};

static void repl_compiler_rollback(ReplCompiler* rc) {
    Compiler* c = &rc->compiler;
    for (int i = 0; i < rc->redef_count; i++) {
        c->procs[rc->redefs[i].idx].offset = rc->redefs[i].old_offset;
        c->procs[rc->redefs[i].idx].autonomous_transaction = rc->redefs[i].old_autonomous;
    }
    rc->redef_count = 0;
    c->call_site_count = rc->mark_call_site_count;
    rc->stmt_watermark = rc->mark_stmt_watermark;
    /* repl_reset_fragment registered this fragment's built-in exceptions;
       the success path frees them after publishing, so do it here too. */
    free_exception_entries(c);
    c->exception_count = 0;
    for (int i = rc->mark_proc_count; i < c->proc_count; i++) {
        free((void*)c->procs[i].name);
        type_free(c->procs[i].return_type);
        for (int p = 0; p < c->procs[i].param_count; p++) {
            type_free(c->procs[i].param_types[p]);
        }
        free(c->procs[i].param_types);
        free(c->procs[i].param_modes);
    }
    c->proc_count = rc->mark_proc_count;

    for (int i = rc->mark_global_count; i < c->global_count; i++) {
        free((void*)c->global_names[i]);
        c->global_names[i] = NULL;
    }
    c->global_count = rc->mark_global_count;

    for (int i = rc->mark_trigger_count; i < c->trigger_count; i++) {
        free(c->triggers[i].proc_name);
        free(c->triggers[i].table);
    }
    c->trigger_count = rc->mark_trigger_count;

    c->local_count = rc->mark_local_count;

    c->chunk->count = rc->mark_chunk_count;
    c->chunk->lines_count = rc->mark_lines_count;
    c->chunk->columns_count = rc->mark_columns_count;
    /* Truncated constants are leaked deliberately: the old engine freed them
       with the discarded chunk, but unwinding individual constant-pool entries
       is not worth the bookkeeping for the rare failed-input case. */
    c->chunk->constants_count = rc->mark_constants_count;

    for (int i = 0; i < c->patch_count; i++) free((void*)c->patches[i].name);
    c->patch_count = 0;
    free_exception_entries(c);
    c->exception_count = 0;
    free_module_init_entries(c);
    c->module_init_count = 0;
}

/* Reset the per-fragment compiler state; persistent tables stay. */
static void repl_reset_fragment(ReplCompiler* rc) {
    Compiler* c = &rc->compiler;
    c->patch_count = 0;
    c->had_error = 0;
    c->error_message[0] = '\0';
    c->entry_index = -1;
    c->entry_jump_patch = -1;
    c->loop_count = 0;
    c->scope_depth = 0;
    c->current_line = 0;
    c->current_column = 0;
    c->current_package = NULL;
    c->current_proc_autonomous = 0;
    c->current_struct = NULL;
    c->module_init_count = 0;
    c->module_init_base = rc->module_init_total;
    rc->redef_count = 0;
    c->cursor_query_count = 0;
    c->exception_count = 0;
    add_exception(c, "no_data_found", 100);
    add_exception(c, "too_many_rows", -1422);
    c->options = NULL;
}

static int repl_build_sigs(Compiler* c, ProcSignature** out) {
    *out = NULL;
    if (c->proc_count == 0) return 1;
    ProcSignature* sigs = malloc(sizeof(ProcSignature) * (size_t)c->proc_count);
    if (sigs == NULL) return 0;
    for (int i = 0; i < c->proc_count; i++) {
        sigs[i].name = c->procs[i].name;
        sigs[i].return_type = c->procs[i].return_type;
        sigs[i].param_types = c->procs[i].param_types;
        sigs[i].param_modes = c->procs[i].param_modes;
        sigs[i].param_count = c->procs[i].param_count;
        sigs[i].is_public = c->procs[i].is_public;
    }
    *out = sigs;
    return 1;
}

/* Apply this fragment's call patches (copied from the end of
   compile_with_options, minus the wholesale teardown). */
static int repl_patch_calls(Compiler* c, char* error, size_t error_size) {
    Chunk* chunk = c->chunk;
    for (int i = 0; i < c->patch_count; i++) {
        int idx = find_proc(c, c->patches[i].name);
        if (idx < 0) {
            if (error != NULL && error_size > 0) {
                char msg[256];
                snprintf(msg, sizeof(msg), "Undefined procedure '%s'", c->patches[i].name);
                format_error(error, error_size, c->source_path, 0, 0, msg);
            }
            return 0;
        }
        patch_call(chunk, c->patches[i].offset, c->procs[idx].offset);
        if (c->procs[idx].autonomous_transaction) {
            int op_offset = c->patches[i].offset - 1;
            if (op_offset >= 0 && op_offset < chunk->count) {
                if (chunk->code[op_offset] == OP_CALL) {
                    chunk->code[op_offset] = OP_CALL_AUTONOMOUS;
                } else if (chunk->code[op_offset] == OP_CALL_OUT) {
                    chunk->code[op_offset] = OP_CALL_OUT_AUTONOMOUS;
                }
            }
        }
    }
    return 1;
}

static int repl_compiler_error(Compiler* c, char* error, size_t error_size) {
    if (error != NULL && error_size > 0) {
        strncpy(error, c->error_message[0] != '\0' ? c->error_message : "unknown error",
                error_size - 1);
        error[error_size - 1] = '\0';
    }
    return 0;
}

/* Take ownership of the local-table entries added by this fragment: names and
   types point into the freed AST, so persist deep copies and extend the
   typecheck seed arrays to match. */
static int repl_take_ownership(ReplCompiler* rc, char* error, size_t error_size) {
    Compiler* c = &rc->compiler;
    for (int i = rc->mark_local_count; i < c->local_count; i++) {
        Local* l = &c->locals[i];
        char* name_copy = malloc((size_t)l->length + 1);
        if (name_copy == NULL) {
            if (error != NULL && error_size > 0) {
                strncpy(error, "out of memory", error_size - 1);
                error[error_size - 1] = '\0';
            }
            return 0;
        }
        memcpy(name_copy, l->name, (size_t)l->length);
        name_copy[l->length] = '\0';
        Type* type_copy_v = NULL;
        if (l->type != NULL) {
            type_copy_v = type_copy(l->type);
            if (type_copy_v == NULL) {
                free(name_copy);
                if (error != NULL && error_size > 0) {
                    strncpy(error, "out of memory", error_size - 1);
                    error[error_size - 1] = '\0';
                }
                return 0;
            }
        }
        if (rc->seed_count == rc->seed_capacity) {
            int new_cap = rc->seed_capacity == 0 ? 16 : rc->seed_capacity * 2;
            char** new_names = realloc(rc->seed_names, sizeof(char*) * (size_t)new_cap);
            if (new_names == NULL) {
                free(name_copy);
                type_free(type_copy_v);
                if (error != NULL && error_size > 0) {
                    strncpy(error, "out of memory", error_size - 1);
                    error[error_size - 1] = '\0';
                }
                return 0;
            }
            rc->seed_names = new_names;
            Type** new_types = realloc(rc->seed_types, sizeof(Type*) * (size_t)new_cap);
            if (new_types == NULL) {
                free(name_copy);
                type_free(type_copy_v);
                if (error != NULL && error_size > 0) {
                    strncpy(error, "out of memory", error_size - 1);
                    error[error_size - 1] = '\0';
                }
                return 0;
            }
            rc->seed_types = new_types;
            rc->seed_capacity = new_cap;
        }
        rc->seed_names[rc->seed_count] = name_copy;
        rc->seed_types[rc->seed_count] = type_copy_v;
        rc->seed_count++;
        l->name = name_copy;
        l->type = type_copy_v;
    }
    return 1;
}

static int repl_compile_stmt_fragment(ReplCompiler* rc, Program* program,
                                      char* error, size_t error_size) {
    Compiler* c = &rc->compiler;
    ProcDecl* main_proc = NULL;
    for (int i = 0; i < program->proc_count; i++) {
        if (strcmp(program->procs[i].name, "main") == 0) {
            main_proc = &program->procs[i];
            break;
        }
    }
    if (main_proc == NULL || main_proc->body == NULL) {
        if (error != NULL && error_size > 0) {
            strncpy(error, "internal: wrapper main missing", error_size - 1);
            error[error_size - 1] = '\0';
        }
        return 0;
    }

    ProcSignature* sigs = NULL;
    if (!repl_build_sigs(c, &sigs)) {
        if (error != NULL && error_size > 0) {
            strncpy(error, "out of memory", error_size - 1);
            error[error_size - 1] = '\0';
        }
        return 0;
    }
    int tc_ok = typecheck_program(program, sigs, c->proc_count, c->ctx,
                                  c->source_path, error, error_size,
                                  (const char* const*)rc->seed_names,
                                  (Type* const*)rc->seed_types,
                                  rc->seed_count);
    free(sigs);
    if (!tc_ok) return 0;

    /* Codegen only the statements added since the previous input, plus the
       synthetic trailing return. No compile_block epilogue: top-level locals
       stay live (values persist on the VM stack across fragments), matching
       the mid-body state of a whole-program main compile. */
    Block* body = main_proc->body;
    int total = body->stmt_count;
    int start = rc->stmt_watermark;
    if (start < 0) start = 0;
    if (start > total - 1) start = total - 1;
    for (int i = start; i < total; i++) {
        compile_stmt(c, body->stmts[i]);
        if (c->had_error) return repl_compiler_error(c, error, error_size);
    }
    rc->stmt_watermark = total - 1;
    return repl_patch_calls(c, error, error_size);
}

/* A REPL input may redefine a plain proc from an earlier input, keeping its
   signature: everything compiled so far was type-checked against it. */
static int repl_same_signature(const ProcEntry* entry, const ProcDecl* proc) {
    if (entry->param_count != proc->param_count) return 0;
    if (!type_equals(entry->return_type, proc->return_type)) return 0;
    for (int p = 0; p < proc->param_count; p++) {
        if (entry->param_modes[p] != proc->params[p].mode) return 0;
        if (!type_equals(entry->param_types[p], proc->params[p].type)) return 0;
    }
    return 1;
}

/* Returns 1 when `proc` redefines a proc from an earlier input and records
   it, 0 when it is new, -1 (error set) when the redefinition is refused. */
static int repl_register_redefinition(ReplCompiler* rc, const ProcDecl* proc,
                                      char* error, size_t error_size) {
    Compiler* c = &rc->compiler;
    int idx = find_proc(c, proc->name);
    if (idx < 0 || idx >= rc->mark_proc_count) return 0;
    char msg[256];
    msg[0] = '\0';
    for (int i = 0; i < rc->redef_count; i++) {
        if (rc->redefs[i].idx == idx) {
            snprintf(msg, sizeof(msg), "Duplicate procedure '%s'", proc->name);
        }
    }
    if (msg[0] == '\0' && !repl_same_signature(&c->procs[idx], proc)) {
        snprintf(msg, sizeof(msg),
                 "Cannot redefine procedure '%s' with a different signature",
                 proc->name);
    }
    if (msg[0] != '\0') {
        if (error != NULL && error_size > 0) {
            format_error(error, error_size, c->source_path, 0, 0, msg);
        }
        return -1;
    }
    rc->redefs[rc->redef_count].idx = idx;
    rc->redefs[rc->redef_count].old_offset = c->procs[idx].offset;
    rc->redefs[rc->redef_count].old_autonomous = c->procs[idx].autonomous_transaction;
    rc->redef_count++;
    return 1;
}

/* Point every recorded call of a redefined proc's old body at its new one,
   switching the opcode if the new body's autonomous_transaction differs. */
static void repl_repoint_redefined_calls(ReplCompiler* rc) {
    Compiler* c = &rc->compiler;
    Chunk* chunk = c->chunk;
    for (int r = 0; r < rc->redef_count; r++) {
        const ProcEntry* entry = &c->procs[rc->redefs[r].idx];
        int old_offset = rc->redefs[r].old_offset;
        for (int i = 0; i < c->call_site_count; i++) {
            int site = c->call_sites[i];
            if (read_u16(chunk->code + site) != (uint16_t)old_offset) continue;
            patch_call(chunk, site, entry->offset);
            uint8_t* op = &chunk->code[site - 1];
            if (*op == OP_CALL || *op == OP_CALL_AUTONOMOUS) {
                *op = entry->autonomous_transaction ? OP_CALL_AUTONOMOUS : OP_CALL;
            } else if (*op == OP_CALL_OUT || *op == OP_CALL_OUT_AUTONOMOUS) {
                *op = entry->autonomous_transaction ? OP_CALL_OUT_AUTONOMOUS : OP_CALL_OUT;
            }
        }
    }
    rc->redef_count = 0;
}

static int repl_compile_def_fragment(ReplCompiler* rc, Program* program,
                                     char* error, size_t error_size) {
    Compiler* c = &rc->compiler;

    for (int i = 0; i < program->import_count; i++) {
        Stmt* import_stmt = program->imports[i];
        const char* path = import_stmt->as.import_stmt.path;
        c->current_line = import_stmt->loc.line;
        c->current_column = import_stmt->loc.column;
        if (!compiler_load_module(c, path, error, error_size)) return 0;
    }

    for (int i = 0; i < program->proc_count; i++) {
        ProcDecl* proc = &program->procs[i];
        int redefined = repl_register_redefinition(rc, proc, error, error_size);
        if (redefined < 0) return 0;
        if (redefined) continue;
        Type** pts = NULL;
        ParamMode* pms = NULL;
        if (proc->param_count > 0) {
            pts = malloc(sizeof(Type*) * (size_t)proc->param_count);
            pms = malloc(sizeof(ParamMode) * (size_t)proc->param_count);
            for (int p = 0; p < proc->param_count; p++) {
                pts[p] = proc->params[p].type;
                pms[p] = proc->params[p].mode;
            }
        }
        int idx = add_proc_entry(c, proc->name, -1, proc->return_type,
                                 pts, pms, proc->param_count);
        free(pts);
        free(pms);
        if (idx < 0) {
            if (error != NULL && error_size > 0) {
                if (idx == -1) {
                    format_error(error, error_size, c->source_path, 0, 0,
                                 "Out of memory registering procedure");
                } else if (idx == -2) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "Duplicate procedure '%s'", proc->name);
                    format_error(error, error_size, c->source_path, 0, 0, msg);
                } else {
                    format_error(error, error_size, c->source_path, 0, 0,
                                 "Too many procedures");
                }
            }
            return 0;
        }
    }

    if (!register_package_procs(c, program, error, error_size)) return 0;
    if (!register_trigger_procs(c, program, error, error_size)) return 0;
    if (!register_struct_methods(c, program, error, error_size)) return 0;

    ProcSignature* sigs = NULL;
    if (!repl_build_sigs(c, &sigs)) {
        if (error != NULL && error_size > 0) {
            strncpy(error, "out of memory", error_size - 1);
            error[error_size - 1] = '\0';
        }
        return 0;
    }
    int tc_ok = typecheck_program(program, sigs, c->proc_count, c->ctx,
                                  c->source_path, error, error_size,
                                  NULL, NULL, 0);
    free(sigs);
    if (!tc_ok) return 0;

    /* Definition codegen (module init, package members, proc bodies) assumes
       a fresh locals table per body, exactly like a whole-program compile
       where main's table does not exist yet. Swap in a scratch table so the
       persistent main-body locals survive; the scratch is freed after. */
    Local* saved_locals = c->locals;
    int saved_local_count = c->local_count;
    int saved_local_capacity = c->local_capacity;
    c->locals = NULL;
    c->local_count = 0;
    c->local_capacity = 0;

    int init_ok = compile_module_init_proc(c, program, error, error_size);
    if (init_ok) {
        compile_package_members(c, program);
        if (!c->had_error) compile_trigger_members(c, program);
        if (!c->had_error) compile_struct_methods(c, program);

        for (int i = 0; i < program->proc_count && !c->had_error; i++) {
            ProcDecl* proc = &program->procs[i];
            int idx = find_proc(c, proc->name);
            if (idx < 0) continue;
            c->current_proc_autonomous = 0;
            c->procs[idx].offset = c->chunk->count;
            for (int p = 0; p < proc->param_count; p++) {
                add_local(c, proc->params[p].name,
                          (int)strlen(proc->params[p].name), proc->params[p].type);
            }
            compile_block(c, proc->body);
            c->procs[idx].autonomous_transaction = c->current_proc_autonomous;
            if (!c->had_error) emit_byte(c, OP_RETURN);
            c->local_count = 0;
            c->scope_depth = 0;
        }
    }

    free(c->locals);
    c->locals = saved_locals;
    c->local_count = saved_local_count;
    c->local_capacity = saved_local_capacity;
    c->scope_depth = 0;

    if (!init_ok) return 0;
    if (c->had_error) return repl_compiler_error(c, error, error_size);

    /* Run sequence: invoke this fragment's package-state initializers once,
       then leave 0 on the stack (the REPL prints it, matching the old
       validation run's main return value). */
    int exec_start = c->chunk->count;
    for (int i = 0; i < c->module_init_count; i++) {
        emit_call(c, c->module_init_procs[i], 0);
        emit_byte(c, OP_POP);
    }
    emit_constant(c, value_int(0));
    emit_byte(c, OP_RETURN);
    rc->module_init_total += c->module_init_count;
    rc->exec_offset = exec_start;
    return repl_patch_calls(c, error, error_size);
}

ReplCompiler* repl_compiler_create(void) {
    ReplCompiler* rc = calloc(1, sizeof(ReplCompiler));
    if (rc == NULL) return NULL;
    Compiler* c = &rc->compiler;
    c->chunk = NULL;
    c->locals = NULL;
    c->local_count = 0;
    c->local_capacity = 0;
    c->cursor_queries = NULL;
    c->cursor_query_count = 0;
    c->cursor_query_capacity = 0;
    c->scope_depth = 0;
    c->proc_count = 0;
    c->patch_count = 0;
    c->had_error = 0;
    c->error_message[0] = '\0';
    c->entry_index = -1;
    c->entry_jump_patch = -1;
    c->loaded_count = 0;
    c->loading_count = 0;
    c->current_path = NULL;
    c->source_path = NULL;
    c->ctx = NULL;
    c->loop_count = 0;
    c->current_line = 0;
    c->current_column = 0;
    c->global_count = 0;
    c->current_package = NULL;
    c->current_proc_autonomous = 0;
    c->exception_count = 0;
    c->trigger_count = 0;
    c->current_struct = NULL;
    c->module_init_count = 0;
    c->module_init_base = 0;
    c->options = NULL;
    for (int i = 0; i < MAX_GLOBALS; i++) c->global_names[i] = NULL;
    rc->exec_offset = -1;
    rc->stmt_watermark = 0;
    rc->module_init_total = 0;
    rc->published_triggers = 0;
    rc->seed_names = NULL;
    rc->seed_types = NULL;
    rc->seed_count = 0;
    rc->seed_capacity = 0;
    c->record_call_sites = 1;
    return rc;
}

void repl_compiler_free(ReplCompiler* rc) {
    if (rc == NULL) return;
    Compiler* c = &rc->compiler;
    free_proc_entries(c);
    free_global_names(c);
    free_exception_entries(c);
    free_trigger_entries(c);
    free_module_init_entries(c);
    for (int i = 0; i < c->patch_count; i++) free((void*)c->patches[i].name);
    for (int i = 0; i < c->loaded_count; i++) free(c->loaded_paths[i]);
    /* Locals [0, local_count) are ReplCompiler-owned copies; the seed arrays
       reference the SAME copies (one per local), so only free them here. */
    for (int i = 0; i < c->local_count; i++) {
        free((void*)c->locals[i].name);
        type_free(c->locals[i].type);
    }
    free(c->locals);
    free(c->cursor_queries);
    free(rc->seed_names);
    free(rc->seed_types);
    free(c->call_sites);
    free(rc);
}

int repl_compiler_compile(ReplCompiler* rc, const char* source, int is_def_fragment,
                          Chunk* chunk, char* error, size_t error_size,
                          struct Context* ctx) {
    if (rc == NULL || source == NULL || chunk == NULL) return 0;
    Compiler* c = &rc->compiler;

    /* Bytecode, jump targets, and constant indices are u16; guard the growing
       session chunk instead of silently wrapping offsets. */
    if (chunk->count > 60000 || chunk->constants_count > 60000) {
        if (error != NULL && error_size > 0) {
            strncpy(error, "REPL session too large (bytecode limit reached)",
                    error_size - 1);
            error[error_size - 1] = '\0';
        }
        return 0;
    }

    rc->mark_proc_count = c->proc_count;
    rc->mark_global_count = c->global_count;
    rc->mark_trigger_count = c->trigger_count;
    rc->mark_local_count = c->local_count;
    rc->mark_chunk_count = chunk->count;
    rc->mark_lines_count = chunk->lines_count;
    rc->mark_columns_count = chunk->columns_count;
    rc->mark_constants_count = chunk->constants_count;
    rc->mark_call_site_count = c->call_site_count;
    rc->mark_stmt_watermark = rc->stmt_watermark;

    repl_reset_fragment(rc);

    c->chunk = chunk;
    c->ctx = ctx;
    c->source_path = NULL;
    rc->exec_offset = chunk->count;

    char cc_error[256] = {0};
    char* processed = cc_preprocess(source, c->source_path, c->options,
                                    cc_error, sizeof(cc_error));
    if (processed == NULL) {
        if (error != NULL && error_size > 0) {
            strncpy(error, cc_error[0] != '\0' ? cc_error
                                               : "conditional compilation: out of memory",
                    error_size - 1);
            error[error_size - 1] = '\0';
        }
        repl_compiler_rollback(rc);
        return 0;
    }
    char parse_error[256] = {0};
    Program* program = parse_with_path(processed, c->source_path,
                                       parse_error, sizeof(parse_error));
    free(processed);
    if (program == NULL) {
        if (error != NULL && error_size > 0) {
            strncpy(error, parse_error, error_size - 1);
            error[error_size - 1] = '\0';
        }
        repl_compiler_rollback(rc);
        return 0;
    }

    int ok = is_def_fragment
                 ? repl_compile_def_fragment(rc, program, error, error_size)
                 : repl_compile_stmt_fragment(rc, program, error, error_size);
    if (!ok) {
        free_program(program);
        repl_compiler_rollback(rc);
        return 0;
    }

    /* Persist copies of the new locals' names/types BEFORE the AST is freed:
       the table entries point into it. */
    if (!repl_take_ownership(rc, error, error_size)) {
        free_program(program);
        repl_compiler_rollback(rc);
        return 0;
    }
    free_program(program);

    /* Nothing can fail past this point, so a redefinition may now re-point
       the callers bound to the old body, from any earlier input. */
    repl_repoint_redefined_calls(rc);

    /* Publish this fragment's trigger entries to the runtime registry. */
    for (int i = rc->published_triggers; i < c->trigger_count; i++) {
        TriggerEntry* te = &c->triggers[i];
        int idx = find_proc(c, te->proc_name);
        if (idx < 0 || c->procs[idx].offset < 0) continue;
        chunk_add_trigger(chunk, te->proc_name + 10 /* skip "__trigger_" */,
                          te->timing, te->event, te->table,
                          c->procs[idx].offset, te->for_each_row);
    }
    rc->published_triggers = c->trigger_count;

    /* Per-fragment cleanup: patches and module-init names are consumed. */
    for (int i = 0; i < c->patch_count; i++) free((void*)c->patches[i].name);
    c->patch_count = 0;
    free_module_init_entries(c);
    c->module_init_count = 0;
    free_exception_entries(c);
    c->exception_count = 0;

    return 1;
}

int repl_compiler_exec_offset(const ReplCompiler* rc) {
    return rc != NULL ? rc->exec_offset : -1;
}

int repl_compiler_local_count(const ReplCompiler* rc) {
    return rc != NULL ? rc->compiler.local_count : 0;
}

const char* repl_compiler_local_name(const ReplCompiler* rc, int slot) {
    if (rc == NULL || slot < 0 || slot >= rc->compiler.local_count) return NULL;
    return rc->compiler.locals[slot].name;
}
