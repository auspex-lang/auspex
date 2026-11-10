#include <inttypes.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "compiler.h"
#include "sql_engine.h"

typedef struct {
    Obj obj;
    char chars[];
} StringObj;

struct ArrayObj {
    Obj obj;
    Value* items;
    int count;
    int capacity;
};

struct RowObj {
    Obj obj;
    int column_count;
    char** column_names;
    Value* column_values;
};

CursorObj* cursor_obj_new(DBDriver* driver) {
    CursorObj* cursor = malloc(sizeof(CursorObj));
    if (cursor == NULL) return NULL;
    cursor->obj.type = OBJ_CURSOR;
    cursor->obj.ref_count = 1;
    cursor->obj.gc_index = -1;
    cursor->obj.gc_mark = 0;
    cursor->driver = driver;
    cursor->result_handle = NULL;
    cursor->row_handle = NULL;
    cursor->is_open = 0;
    cursor->row_count = 0;
    cursor->found = 0;
    return cursor;
}

/* ---------------------------------------------------------------------------
 * Container registry and cycle collector.
 *
 * Every live ArrayObj/MapObj/RowObj sits in g_gc_registry (slot stored in
 * Obj.gc_index, removal is O(1) swap-with-last). The VM traces its roots and
 * calls gc_sweep_unreachable(); unreachable containers keep their refcounts
 * only through each other, so the sweep first breaks those internal edges
 * (decrement child refcounts, clear the slots, release string children) and
 * then frees every container whose adjusted refcount reached zero through
 * its normal destructor. Edge breaking and freeing are separate passes, so
 * no destructor ever runs while one of its children is being processed and
 * no refcount is adjusted twice. Strings and cursors are not registered.
 * ------------------------------------------------------------------------- */

static Obj** g_gc_registry = NULL;
static int   g_gc_count = 0;
static int   g_gc_capacity = 0;
static long  g_gc_allocations = 0;

static Obj** g_gc_mark_stack = NULL;
static int   g_gc_mark_count = 0;
static int   g_gc_mark_capacity = 0;

static Obj** g_gc_worklist = NULL;
static int   g_gc_work_count = 0;
static int   g_gc_work_capacity = 0;

/* Grows on demand; on OOM the object is left registered (swept by a later
 * collection or teardown) rather than overflowing the list. */
static void gc_worklist_push(Obj* obj) {
    if (g_gc_work_count >= g_gc_work_capacity) {
        int new_capacity = g_gc_work_capacity == 0 ? 256 : g_gc_work_capacity * 2;
        Obj** grown = realloc(g_gc_worklist, sizeof(Obj*) * (size_t)new_capacity);
        if (grown == NULL) return;
        g_gc_worklist = grown;
        g_gc_work_capacity = new_capacity;
    }
    g_gc_worklist[g_gc_work_count++] = obj;
}

static int gc_registry_add(Obj* obj) {
    if (g_gc_count == g_gc_capacity) {
        int new_capacity = g_gc_capacity == 0 ? 256 : g_gc_capacity * 2;
        Obj** grown = realloc(g_gc_registry, sizeof(Obj*) * (size_t)new_capacity);
        if (grown == NULL) return 0;
        g_gc_registry = grown;
        g_gc_capacity = new_capacity;
    }
    obj->gc_index = g_gc_count;
    obj->gc_mark = 0;
    g_gc_registry[g_gc_count++] = obj;
    g_gc_allocations++;
    return 1;
}

static void gc_registry_remove(Obj* obj) {
    int idx = obj->gc_index;
    if (idx < 0 || idx >= g_gc_count || g_gc_registry[idx] != obj) return;
    g_gc_count--;
    if (idx != g_gc_count) {
        Obj* last = g_gc_registry[g_gc_count];
        g_gc_registry[idx] = last;
        last->gc_index = idx;
    }
    obj->gc_index = -1;
}

static void gc_mark_obj(Obj* obj) {
    if (obj == NULL || obj->gc_mark) return;
    obj->gc_mark = 1;
    /* Capacity is guaranteed by gc_begin_collection (<= live containers). */
    g_gc_mark_stack[g_gc_mark_count++] = obj;
}

static void gc_mark_drain(void) {
    while (g_gc_mark_count > 0) {
        Obj* obj = g_gc_mark_stack[--g_gc_mark_count];
        switch (obj->type) {
            case OBJ_ARRAY: {
                ArrayObj* array = (ArrayObj*)obj;
                for (int i = 0; i < array->count; i++) {
                    Value v = array->items[i];
                    if (v.type == VAL_ARRAY) gc_mark_obj(&v.as.as_array->obj);
                    else if (v.type == VAL_MAP) gc_mark_obj(&v.as.as_map->obj);
                    else if (v.type == VAL_ROW && v.as.as_row_handle != NULL)
                        gc_mark_obj(&((RowObj*)v.as.as_row_handle)->obj);
                }
                break;
            }
            case OBJ_MAP: {
                MapObj* map = (MapObj*)obj;
                for (int i = 0; i < map->count; i++) {
                    Value k = map->keys[i];
                    Value v = map->values[i];
                    if (k.type == VAL_ARRAY) gc_mark_obj(&k.as.as_array->obj);
                    else if (k.type == VAL_MAP) gc_mark_obj(&k.as.as_map->obj);
                    else if (k.type == VAL_ROW && k.as.as_row_handle != NULL)
                        gc_mark_obj(&((RowObj*)k.as.as_row_handle)->obj);
                    if (v.type == VAL_ARRAY) gc_mark_obj(&v.as.as_array->obj);
                    else if (v.type == VAL_MAP) gc_mark_obj(&v.as.as_map->obj);
                    else if (v.type == VAL_ROW && v.as.as_row_handle != NULL)
                        gc_mark_obj(&((RowObj*)v.as.as_row_handle)->obj);
                }
                break;
            }
            case OBJ_ROW: {
                RowObj* row = (RowObj*)obj;
                for (int i = 0; i < row->column_count; i++) {
                    Value v = row->column_values[i];
                    if (v.type == VAL_ARRAY) gc_mark_obj(&v.as.as_array->obj);
                    else if (v.type == VAL_MAP) gc_mark_obj(&v.as.as_map->obj);
                    else if (v.type == VAL_ROW && v.as.as_row_handle != NULL)
                        gc_mark_obj(&((RowObj*)v.as.as_row_handle)->obj);
                }
                break;
            }
            default:
                break;
        }
    }
}

void gc_trace_value(Value v) {
    if (v.type == VAL_ARRAY && v.as.as_array != NULL) gc_mark_obj(&v.as.as_array->obj);
    else if (v.type == VAL_MAP && v.as.as_map != NULL) gc_mark_obj(&v.as.as_map->obj);
    else if (v.type == VAL_ROW && v.as.as_row_handle != NULL)
        gc_mark_obj(&((RowObj*)v.as.as_row_handle)->obj);
    gc_mark_drain();
}

int gc_begin_collection(void) {
    if (g_gc_mark_capacity < g_gc_count) {
        int new_capacity = g_gc_mark_capacity == 0 ? 256 : g_gc_mark_capacity;
        while (new_capacity < g_gc_count) new_capacity *= 2;
        Obj** grown = realloc(g_gc_mark_stack, sizeof(Obj*) * (size_t)new_capacity);
        if (grown == NULL) return 0;
        g_gc_mark_stack = grown;
        g_gc_mark_capacity = new_capacity;
    }
    if (g_gc_work_capacity < g_gc_count) {
        int new_capacity = g_gc_work_capacity == 0 ? 256 : g_gc_work_capacity;
        while (new_capacity < g_gc_count) new_capacity *= 2;
        Obj** grown = realloc(g_gc_worklist, sizeof(Obj*) * (size_t)new_capacity);
        if (grown == NULL) return 0;
        g_gc_worklist = grown;
        g_gc_work_capacity = new_capacity;
    }
    g_gc_mark_count = 0;
    g_gc_work_count = 0;
    return 1;
}

/* Break one child edge of an unreachable container being swept: internal
 * container edges (child unmarked) are removed by decrementing the child
 * refcount and clearing the slot; edges to marked containers stay in place
 * so the owner's destructor releases them exactly once; string/scalar edges
 * are released right away (scalars are no-ops). */
static void gc_break_edge(Value* slot) {
    Value v = *slot;
    if (v.type == VAL_ARRAY || v.type == VAL_MAP ||
        (v.type == VAL_ROW && v.as.as_row_handle != NULL)) {
        Obj* child = v.type == VAL_ARRAY ? &v.as.as_array->obj
                     : v.type == VAL_MAP ? &v.as.as_map->obj
                     : &((RowObj*)v.as.as_row_handle)->obj;
        if (!child->gc_mark) {
            if (child->ref_count > 0) {
                child->ref_count--;
                if (child->ref_count == 0) gc_worklist_push(child);
            }
            *slot = value_null();
        }
        /* Marked children are unreachable from this owner but kept alive by
         * real roots: leave slot and count untouched; the destructor drops
         * the edge when it frees the owner. */
        return;
    }
    value_release(v);
    *slot = value_null();
}

int gc_sweep_unreachable(void) {
    int freed = 0;
    /* Pass 1: break internal edges of every unmarked container and release
     * their string children. Reachable (marked) containers only get their
     * mark bit cleared. The registry does not change in this pass. */
    for (int i = 0; i < g_gc_count; i++) {
        Obj* obj = g_gc_registry[i];
        if (obj->gc_mark) {
            obj->gc_mark = 0;
            continue;
        }
        switch (obj->type) {
            case OBJ_ARRAY: {
                ArrayObj* array = (ArrayObj*)obj;
                for (int j = 0; j < array->count; j++) gc_break_edge(&array->items[j]);
                break;
            }
            case OBJ_MAP: {
                MapObj* map = (MapObj*)obj;
                for (int j = 0; j < map->count; j++) {
                    gc_break_edge(&map->keys[j]);
                    gc_break_edge(&map->values[j]);
                }
                break;
            }
            case OBJ_ROW: {
                RowObj* row = (RowObj*)obj;
                for (int j = 0; j < row->column_count; j++)
                    gc_break_edge(&row->column_values[j]);
                break;
            }
            default:
                break;
        }
        /* No end-of-object push here: an unreachable container reaches
         * refcount zero exactly when its last internal owner edge is broken
         * in gc_break_edge (registry objects always start with refcount >= 1
         * and only this pass decrements them), and that is the single place
         * that queues for freeing. */
    }
    /* Pass 2: free zero-refcount containers. Their internal slots are
     * already cleared, so the destructors only drop the (marked) children
     * they still reference. Freeing is worklist-driven, never recursive. */
    while (g_gc_work_count > 0) {
        Obj* obj = g_gc_worklist[--g_gc_work_count];
        if (obj->gc_index < 0) continue;
        gc_registry_remove(obj);
        switch (obj->type) {
            case OBJ_ARRAY:  array_free((ArrayObj*)obj);   break;
            case OBJ_MAP:    map_free((MapObj*)obj);       break;
            case OBJ_ROW:    row_obj_free((RowObj*)obj);   break;
            default: break;
        }
        freed++;
    }
    return freed;
}

long gc_container_allocations(void) { return g_gc_allocations; }
void gc_reset_container_allocations(void) { g_gc_allocations = 0; }
int  gc_container_count(void) { return g_gc_count; }

static StringObj* string_obj_from_chars(const char* chars) {
    if (chars == NULL) return NULL;
    return (StringObj*)(chars - offsetof(StringObj, chars));
}

Value value_int(int64_t v) {
    Value value;
    value.type = VAL_INT;
    value.as.as_int = v;
    return value;
}

/* SQL NULL semantics (three-valued logic):
 * - Any arithmetic or comparison involving NULL yields NULL (unknown).
 * - NULL in a boolean condition is not true (value_is_truthy returns 0).
 * - NULL carries no payload, so retain/release are no-ops for it. */
Value value_null(void) {
    Value value;
    value.type = VAL_NULL;
    value.as.as_int = 0;
    return value;
}

Value value_float(double v) {
    Value value;
    value.type = VAL_FLOAT;
    value.as.as_float = v;
    return value;
}

Value value_string(char* s) {
    Value value;
    value.type = VAL_STRING;
    if (s == NULL) {
        value.as.as_string = NULL;
        return value;
    }
    size_t len = strlen(s);
    StringObj* obj = malloc(sizeof(StringObj) + len + 1);
    if (obj == NULL) {
        value.as.as_string = NULL;
        free(s);
        return value;
    }
    obj->obj.type = OBJ_STRING;
    obj->obj.ref_count = 1;
    obj->obj.gc_index = -1;
    obj->obj.gc_mark = 0;
    memcpy(obj->chars, s, len + 1);
    free(s);
    value.as.as_string = obj->chars;
    return value;
}

Value value_bool(int v) {
    Value value;
    value.type = VAL_BOOL;
    value.as.as_int = v ? 1 : 0;
    return value;
}

Value value_date(char* s) {
    Value value;
    value.type = VAL_DATE;
    if (s == NULL) {
        value.as.as_string = NULL;
        return value;
    }
    size_t len = strlen(s);
    StringObj* obj = malloc(sizeof(StringObj) + len + 1);
    if (obj == NULL) {
        value.as.as_string = NULL;
        free(s);
        return value;
    }
    obj->obj.type = OBJ_STRING;
    obj->obj.ref_count = 1;
    obj->obj.gc_index = -1;
    obj->obj.gc_mark = 0;
    memcpy(obj->chars, s, len + 1);
    free(s);
    value.as.as_string = obj->chars;
    return value;
}

Value value_timestamp(char* s) {
    Value value;
    value.type = VAL_TIMESTAMP;
    if (s == NULL) {
        value.as.as_string = NULL;
        return value;
    }
    size_t len = strlen(s);
    StringObj* obj = malloc(sizeof(StringObj) + len + 1);
    if (obj == NULL) {
        value.as.as_string = NULL;
        free(s);
        return value;
    }
    obj->obj.type = OBJ_STRING;
    obj->obj.ref_count = 1;
    obj->obj.gc_index = -1;
    obj->obj.gc_mark = 0;
    memcpy(obj->chars, s, len + 1);
    free(s);
    value.as.as_string = obj->chars;
    return value;
}

Value value_array(ArrayObj* array) {
    Value value;
    value.type = VAL_ARRAY;
    value.as.as_array = array;
    return value;
}

Value value_map(MapObj* map) {
    Value value;
    value.type = VAL_MAP;
    value.as.as_map = map;
    return value;
}

Value value_row(RowObj* row) {
    Value value;
    value.type = VAL_ROW;
    value.as.as_row_handle = row;
    return value;
}

Value value_cursor(CursorObj* cursor) {
    Value value;
    value.type = VAL_CURSOR;
    value.as.as_cursor = cursor;
    return value;
}

void value_retain_heap(Value v) {
    if ((v.type == VAL_STRING || v.type == VAL_DATE || v.type == VAL_TIMESTAMP) && v.as.as_string != NULL) {
        string_obj_from_chars(v.as.as_string)->obj.ref_count++;
    } else if (v.type == VAL_ARRAY && v.as.as_array != NULL) {
        v.as.as_array->obj.ref_count++;
    } else if (v.type == VAL_MAP && v.as.as_map != NULL) {
        v.as.as_map->obj.ref_count++;
    } else if (v.type == VAL_ROW && v.as.as_row_handle != NULL) {
        ((RowObj*)v.as.as_row_handle)->obj.ref_count++;
    } else if (v.type == VAL_CURSOR && v.as.as_cursor != NULL) {
        v.as.as_cursor->obj.ref_count++;
    }
}

static void cursor_obj_free(CursorObj* cursor) {
    if (cursor == NULL) return;
    if (cursor->result_handle != NULL && cursor->driver != NULL) {
        cursor->driver->result_free(cursor->driver, cursor->result_handle);
    }
    free(cursor);
}

void value_release_heap(Value v) {
    if ((v.type == VAL_STRING || v.type == VAL_DATE || v.type == VAL_TIMESTAMP) && v.as.as_string != NULL) {
        StringObj* obj = string_obj_from_chars(v.as.as_string);
        if (--obj->obj.ref_count <= 0) {
            free(obj);
        }
    } else if (v.type == VAL_ARRAY && v.as.as_array != NULL) {
        ArrayObj* array = v.as.as_array;
        if (--array->obj.ref_count <= 0) {
            array_free(array);
        }
    } else if (v.type == VAL_MAP && v.as.as_map != NULL) {
        MapObj* map = v.as.as_map;
        if (--map->obj.ref_count <= 0) {
            map_free(map);
        }
    } else if (v.type == VAL_ROW && v.as.as_row_handle != NULL) {
        RowObj* row = (RowObj*)v.as.as_row_handle;
        if (--row->obj.ref_count <= 0) {
            row_obj_free(row);
        }
    } else if (v.type == VAL_CURSOR && v.as.as_cursor != NULL) {
        CursorObj* cursor = v.as.as_cursor;
        if (--cursor->obj.ref_count <= 0) {
            cursor_obj_free(cursor);
        }
    }
}

int value_ref_count(Value v) {
    if ((v.type == VAL_STRING || v.type == VAL_DATE || v.type == VAL_TIMESTAMP) && v.as.as_string != NULL) {
        return string_obj_from_chars(v.as.as_string)->obj.ref_count;
    }
    if (v.type == VAL_ARRAY && v.as.as_array != NULL) {
        return v.as.as_array->obj.ref_count;
    }
    if (v.type == VAL_MAP && v.as.as_map != NULL) {
        return v.as.as_map->obj.ref_count;
    }
    if (v.type == VAL_ROW && v.as.as_row_handle != NULL) {
        return ((RowObj*)v.as.as_row_handle)->obj.ref_count;
    }
    if (v.type == VAL_CURSOR && v.as.as_cursor != NULL) {
        return v.as.as_cursor->obj.ref_count;
    }
    return 0;
}

/* Helper: returns true when either operand is a float. */
static int either_float(Value a, Value b) {
    return a.type == VAL_FLOAT || b.type == VAL_FLOAT;
}

/* Helper: returns the numeric value as double. */
static double as_number(Value v) {
    if (v.type == VAL_FLOAT) return v.as.as_float;
    if (v.type == VAL_INT) return (double)v.as.as_int;
    return 0.0;
}

Value value_add(Value a, Value b) {
    if (a.type == VAL_NULL || b.type == VAL_NULL) return value_null();
    if (a.type == VAL_STRING && b.type == VAL_STRING) {
        const char* as = a.as.as_string ? a.as.as_string : "";
        const char* bs = b.as.as_string ? b.as.as_string : "";
        size_t len = strlen(as) + strlen(bs) + 1;
        char* buf = malloc(len);
        if (buf == NULL) return value_int(0);
        snprintf(buf, len, "%s%s", as, bs);
        return value_string(buf);
    }
    if (either_float(a, b)) {
        return value_float(as_number(a) + as_number(b));
    }
    if (a.type == VAL_INT && b.type == VAL_INT) {
        return value_int(a.as.as_int + b.as.as_int);
    }
    return value_int(0);
}

Value value_sub(Value a, Value b) {
    if (a.type == VAL_NULL || b.type == VAL_NULL) return value_null();
    if (either_float(a, b)) {
        return value_float(as_number(a) - as_number(b));
    }
    if (a.type == VAL_INT && b.type == VAL_INT) {
        return value_int(a.as.as_int - b.as.as_int);
    }
    return value_int(0);
}

Value value_mul(Value a, Value b) {
    if (a.type == VAL_NULL || b.type == VAL_NULL) return value_null();
    if (either_float(a, b)) {
        return value_float(as_number(a) * as_number(b));
    }
    if (a.type == VAL_INT && b.type == VAL_INT) {
        return value_int(a.as.as_int * b.as.as_int);
    }
    return value_int(0);
}

Value value_div(Value a, Value b) {
    if (a.type == VAL_NULL || b.type == VAL_NULL) return value_null();
    if (either_float(a, b)) {
        double divisor = as_number(b);
        if (divisor == 0.0) return value_int(0);
        return value_float(as_number(a) / divisor);
    }
    if (a.type == VAL_INT && b.type == VAL_INT) {
        if (b.as.as_int == 0) return value_int(0);
        return value_int(a.as.as_int / b.as.as_int);
    }
    return value_int(0);
}

Value value_eq(Value a, Value b) {
    if (a.type == VAL_NULL || b.type == VAL_NULL) return value_null();
    if (a.type == VAL_BOOL && b.type == VAL_BOOL) {
        return value_int(a.as.as_int == b.as.as_int ? 1 : 0);
    }
    if (a.type == VAL_BOOL || b.type == VAL_BOOL) {
        return value_int(0);
    }
    if (either_float(a, b)) {
        return value_int(as_number(a) == as_number(b) ? 1 : 0);
    }
    if (a.type == VAL_INT && b.type == VAL_INT) {
        return value_int(a.as.as_int == b.as.as_int ? 1 : 0);
    }
    if (a.type == VAL_STRING && b.type == VAL_STRING) {
        const char* as = a.as.as_string ? a.as.as_string : "";
        const char* bs = b.as.as_string ? b.as.as_string : "";
        return value_int(strcmp(as, bs) == 0 ? 1 : 0);
    }
    return value_int(0);
}

Value value_lt(Value a, Value b) {
    if (a.type == VAL_NULL || b.type == VAL_NULL) return value_null();
    if (a.type == VAL_STRING && b.type == VAL_STRING) {
        const char* as = a.as.as_string ? a.as.as_string : "";
        const char* bs = b.as.as_string ? b.as.as_string : "";
        return value_int(strcmp(as, bs) < 0 ? 1 : 0);
    }
    if (a.type == VAL_BOOL && b.type == VAL_BOOL) {
        return value_int(a.as.as_int < b.as.as_int ? 1 : 0);
    }
    if (a.type == VAL_BOOL || b.type == VAL_BOOL) {
        return value_int(0);
    }
    if (either_float(a, b)) {
        return value_int(as_number(a) < as_number(b) ? 1 : 0);
    }
    if (a.type == VAL_INT && b.type == VAL_INT) {
        return value_int(a.as.as_int < b.as.as_int ? 1 : 0);
    }
    return value_int(0);
}

Value value_gt(Value a, Value b) {
    if (a.type == VAL_NULL || b.type == VAL_NULL) return value_null();
    if (a.type == VAL_STRING && b.type == VAL_STRING) {
        const char* as = a.as.as_string ? a.as.as_string : "";
        const char* bs = b.as.as_string ? b.as.as_string : "";
        return value_int(strcmp(as, bs) > 0 ? 1 : 0);
    }
    if (a.type == VAL_BOOL && b.type == VAL_BOOL) {
        return value_int(a.as.as_int > b.as.as_int ? 1 : 0);
    }
    if (a.type == VAL_BOOL || b.type == VAL_BOOL) {
        return value_int(0);
    }
    if (either_float(a, b)) {
        return value_int(as_number(a) > as_number(b) ? 1 : 0);
    }
    if (a.type == VAL_INT && b.type == VAL_INT) {
        return value_int(a.as.as_int > b.as.as_int ? 1 : 0);
    }
    return value_int(0);
}

int value_is_truthy(Value value) {
    switch (value.type) {
        case VAL_NULL:
            /* NULL is unknown, which is not true. */
            return 0;
        case VAL_INT:
            return value.as.as_int != 0;
        case VAL_FLOAT:
            return value.as.as_float != 0.0;
        case VAL_STRING:
        case VAL_DATE:
        case VAL_TIMESTAMP:
            return value.as.as_string != NULL;
        case VAL_BOOL:
            return value.as.as_int != 0;
        case VAL_ARRAY:
            return value.as.as_array != NULL;
        case VAL_MAP:
            return value.as.as_map != NULL;
        case VAL_CURSOR:
            return value.as.as_cursor != NULL;
        default:
            /* VAL_ROW and any future types: treat non-NULL as truthy. */
            return value.as.as_row_handle != NULL;
    }
}

void value_print(Value value) {
    switch (value.type) {
        case VAL_NULL:
            printf("null");
            break;
        case VAL_INT:
            printf("%" PRId64, value.as.as_int);
            break;
        case VAL_FLOAT:
            printf("%g", value.as.as_float);
            break;
        case VAL_STRING:
            printf("%s", value.as.as_string ? value.as.as_string : "");
            break;
        case VAL_DATE:
            printf("%s", value.as.as_string ? value.as.as_string : "");
            break;
        case VAL_TIMESTAMP:
            printf("%s", value.as.as_string ? value.as.as_string : "");
            break;
        case VAL_BOOL:
            printf("%s", value.as.as_int ? "true" : "false");
            break;
        case VAL_ARRAY: {
            ArrayObj* array = value.as.as_array;
            printf("[");
            if (array != NULL) {
                for (int i = 0; i < array->count; i++) {
                    value_print(array->items[i]);
                    if (i < array->count - 1) printf(", ");
                }
            }
            printf("]");
            break;
        }
        case VAL_MAP: {
            MapObj* map = value.as.as_map;
            printf("{");
            if (map != NULL) {
                for (int i = 0; i < map->count; i++) {
                    value_print(map->keys[i]);
                    printf(": ");
                    value_print(map->values[i]);
                    if (i < map->count - 1) printf(", ");
                }
            }
            printf("}");
            break;
        }
        case VAL_ROW:
            printf("(row)");
            break;
        case VAL_CURSOR:
            printf("(cursor)");
            break;
        default:
            printf("?");
            break;
    }
}

ArrayObj* array_new(void) {
    ArrayObj* array = malloc(sizeof(ArrayObj));
    if (array == NULL) return NULL;
    array->obj.type = OBJ_ARRAY;
    array->obj.ref_count = 1;
    array->obj.gc_index = -1;
    array->obj.gc_mark = 0;
    array->items = NULL;
    array->count = 0;
    array->capacity = 0;
    if (!gc_registry_add(&array->obj)) {
        free(array);
        return NULL;
    }
    return array;
}

void array_free(ArrayObj* array) {
    if (array == NULL) return;
    gc_registry_remove(&array->obj);
    for (int i = 0; i < array->count; i++) {
        value_release(array->items[i]);
    }
    free(array->items);
    free(array);
}

RowObj* row_obj_new(int column_count) {
    RowObj* row = malloc(sizeof(RowObj));
    if (row == NULL) return NULL;
    row->obj.type = OBJ_ROW;
    row->obj.ref_count = 1;
    row->obj.gc_index = -1;
    row->obj.gc_mark = 0;
    row->column_count = column_count;
    row->column_names = calloc((size_t)column_count, sizeof(char*));
    if (row->column_names == NULL && column_count > 0) {
        free(row);
        return NULL;
    }
    row->column_values = calloc((size_t)column_count, sizeof(Value));
    if (row->column_values == NULL && column_count > 0) {
        free(row->column_names);
        free(row);
        return NULL;
    }
    if (!gc_registry_add(&row->obj)) {
        free(row->column_names);
        free(row->column_values);
        free(row);
        return NULL;
    }
    return row;
}

void row_obj_free(RowObj* row) {
    if (row == NULL) return;
    gc_registry_remove(&row->obj);
    for (int i = 0; i < row->column_count; i++) {
        free(row->column_names[i]);
        value_release(row->column_values[i]);
    }
    free(row->column_names);
    free(row->column_values);
    free(row);
}

void row_obj_set_column(RowObj* row, int index, const char* name, Value value) {
    if (row == NULL || index < 0 || index >= row->column_count) return;
    free(row->column_names[index]);
    row->column_names[index] = name != NULL ? strdup(name) : NULL;
    value_release(row->column_values[index]);
    row->column_values[index] = value;
    value_retain(value);
}

Value row_obj_get_field(RowObj* row, const char* name) {
    if (row == NULL || name == NULL) return value_int(0);
    for (int i = 0; i < row->column_count; i++) {
        if (row->column_names[i] != NULL && strcmp(row->column_names[i], name) == 0) {
            Value v = row->column_values[i];
            value_retain(v);
            return v;
        }
    }
    return value_int(0);
}

int row_obj_set_field(RowObj* row, const char* name, Value value) {
    if (row == NULL || name == NULL) return 0;
    for (int i = 0; i < row->column_count; i++) {
        if (row->column_names[i] != NULL && strcmp(row->column_names[i], name) == 0) {
            row_obj_set_column(row, i, name, value);
            return 1;
        }
    }
    /* Field not present: grow the row and add it. */
    int new_index = row->column_count;
    char** new_names = realloc(row->column_names,
                               sizeof(char*) * (size_t)(new_index + 1));
    Value* new_values = realloc(row->column_values,
                                sizeof(Value) * (size_t)(new_index + 1));
    if (new_names == NULL || new_values == NULL) {
        if (new_names != NULL) free(new_names);
        if (new_values != NULL) free(new_values);
        return 0;
    }
    row->column_names = new_names;
    row->column_values = new_values;
    row->column_count = new_index + 1;
    row->column_names[new_index] = strdup(name);
    row->column_values[new_index] = value;
    value_retain(value);
    return 1;
}

int array_append(ArrayObj* array, Value value) {
    if (array->count >= array->capacity) {
        int new_capacity = array->capacity == 0 ? 4 : array->capacity * 2;
        Value* new_items = realloc(array->items, sizeof(Value) * (size_t)new_capacity);
        if (new_items == NULL) return 0;
        array->items = new_items;
        array->capacity = new_capacity;
    }
    value_retain(value);
    array->items[array->count++] = value;
    return 1;
}

Value array_get(ArrayObj* array, int index) {
    if (array == NULL || index < 0 || index >= array->count) {
        return value_int(0);
    }
    return array->items[index];
}

void array_set(ArrayObj* array, int index, Value value) {
    if (array == NULL || index < 0 || index >= array->count) return;
    value_retain(value);
    value_release(array->items[index]);
    array->items[index] = value;
}

int array_length(ArrayObj* array) {
    if (array == NULL) return 0;
    return array->count;
}

int array_extend(ArrayObj* array, int count) {
    if (array == NULL || count < 0) return 0;
    for (int i = 0; i < count; i++) {
        if (!array_append(array, value_int(0))) return 0;
    }
    return 1;
}

int array_trim(ArrayObj* array, int count) {
    if (array == NULL || count < 0) return 0;
    if (count > array->count) count = array->count;
    for (int i = array->count - count; i < array->count; i++) {
        value_release(array->items[i]);
    }
    array->count -= count;
    return 1;
}

static int values_equal_values(Value a, Value b) {
    if (a.type != b.type) return 0;
    switch (a.type) {
        case VAL_NULL:   return 1; /* two NULLs are identical for internal use */
        case VAL_INT:    return a.as.as_int == b.as.as_int;
        case VAL_FLOAT:  return a.as.as_float == b.as.as_float;
        case VAL_BOOL:   return a.as.as_int == b.as.as_int;
        case VAL_STRING:
        case VAL_DATE:
        case VAL_TIMESTAMP: {
            const char* as = a.as.as_string ? a.as.as_string : "";
            const char* bs = b.as.as_string ? b.as.as_string : "";
            return strcmp(as, bs) == 0;
        }
        case VAL_ARRAY:  return a.as.as_array == b.as.as_array;
        case VAL_MAP:    return a.as.as_map == b.as.as_map;
        case VAL_ROW:    return a.as.as_row_handle == b.as.as_row_handle;
        case VAL_CURSOR: return a.as.as_cursor == b.as.as_cursor;
    }
    return 0;
}

MapObj* map_new(void) {
    MapObj* map = malloc(sizeof(MapObj));
    if (map == NULL) return NULL;
    map->obj.type = OBJ_MAP;
    map->obj.ref_count = 1;
    map->obj.gc_index = -1;
    map->obj.gc_mark = 0;
    map->keys = NULL;
    map->values = NULL;
    map->count = 0;
    map->capacity = 0;
    if (!gc_registry_add(&map->obj)) {
        free(map);
        return NULL;
    }
    return map;
}

void map_free(MapObj* map) {
    if (map == NULL) return;
    gc_registry_remove(&map->obj);
    for (int i = 0; i < map->count; i++) {
        value_release(map->keys[i]);
        value_release(map->values[i]);
    }
    free(map->keys);
    free(map->values);
    free(map);
}

static int map_find_key(MapObj* map, Value key) {
    for (int i = 0; i < map->count; i++) {
        if (values_equal_values(map->keys[i], key)) return i;
    }
    return -1;
}

int map_set(MapObj* map, Value key, Value value) {
    if (map == NULL) return 0;
    if (key.type != VAL_INT && key.type != VAL_STRING) return 0;
    int idx = map_find_key(map, key);
    if (idx >= 0) {
        value_release(map->values[idx]);
        map->values[idx] = value;
        value_retain(value);
        return 1;
    }
    if (map->count >= map->capacity) {
        int new_capacity = map->capacity == 0 ? 4 : map->capacity * 2;
        Value* new_keys = realloc(map->keys, sizeof(Value) * (size_t)new_capacity);
        Value* new_values = realloc(map->values, sizeof(Value) * (size_t)new_capacity);
        if (new_keys == NULL || new_values == NULL) {
            free(new_keys);
            free(new_values);
            return 0;
        }
        map->keys = new_keys;
        map->values = new_values;
        map->capacity = new_capacity;
    }
    value_retain(key);
    value_retain(value);
    map->keys[map->count] = key;
    map->values[map->count] = value;
    map->count++;
    return 1;
}

int map_get(MapObj* map, Value key, Value* out) {
    if (map == NULL || out == NULL) return 0;
    int idx = map_find_key(map, key);
    if (idx < 0) return 0;
    *out = map->values[idx];
    value_retain(*out);
    return 1;
}

int map_delete(MapObj* map, Value key) {
    if (map == NULL) return 0;
    int idx = map_find_key(map, key);
    if (idx < 0) return 0;
    value_release(map->keys[idx]);
    value_release(map->values[idx]);
    map->count--;
    for (int i = idx; i < map->count; i++) {
        map->keys[i] = map->keys[i + 1];
        map->values[i] = map->values[i + 1];
    }
    return 1;
}

int map_count(MapObj* map) {
    if (map == NULL) return 0;
    return map->count;
}

static int map_key_compare(Value a, Value b) {
    if (a.type == VAL_INT && b.type == VAL_INT) {
        /* Compared, not subtracted: the difference of two 64-bit keys
           need not fit the int result. */
        return (a.as.as_int > b.as.as_int) - (a.as.as_int < b.as.as_int);
    }
    if (a.type == VAL_STRING && b.type == VAL_STRING) {
        const char* as = a.as.as_string ? a.as.as_string : "";
        const char* bs = b.as.as_string ? b.as.as_string : "";
        return strcmp(as, bs);
    }
    return 0;
}

int map_first_key(MapObj* map, Value* out) {
    if (map == NULL || map->count == 0 || out == NULL) return 0;
    Value min = map->keys[0];
    for (int i = 1; i < map->count; i++) {
        if (map_key_compare(map->keys[i], min) < 0) min = map->keys[i];
    }
    *out = min;
    value_retain(*out);
    return 1;
}

int map_last_key(MapObj* map, Value* out) {
    if (map == NULL || map->count == 0 || out == NULL) return 0;
    Value max = map->keys[0];
    for (int i = 1; i < map->count; i++) {
        if (map_key_compare(map->keys[i], max) > 0) max = map->keys[i];
    }
    *out = max;
    value_retain(*out);
    return 1;
}

int map_next_key(MapObj* map, Value key, Value* out) {
    if (map == NULL || map->count == 0 || out == NULL) return 0;
    Value candidate;
    int found = 0;
    for (int i = 0; i < map->count; i++) {
        if (map_key_compare(map->keys[i], key) > 0) {
            if (!found || map_key_compare(map->keys[i], candidate) < 0) {
                candidate = map->keys[i];
                found = 1;
            }
        }
    }
    if (!found) return 0;
    *out = candidate;
    value_retain(*out);
    return 1;
}

int map_prior_key(MapObj* map, Value key, Value* out) {
    if (map == NULL || map->count == 0 || out == NULL) return 0;
    Value candidate;
    int found = 0;
    for (int i = 0; i < map->count; i++) {
        if (map_key_compare(map->keys[i], key) < 0) {
            if (!found || map_key_compare(map->keys[i], candidate) > 0) {
                candidate = map->keys[i];
                found = 1;
            }
        }
    }
    if (!found) return 0;
    *out = candidate;
    value_retain(*out);
    return 1;
}
