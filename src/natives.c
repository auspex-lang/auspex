#include "natives.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "compiler.h"
#include "os.h"
#include "vm.h"

typedef struct {
    const char* name;
    int arity;
    NativeFn fn;
} NativeDef;

static int native_length(VM* vm, int argc, Value* argv, Value* out) {
    (void)vm;
    (void)argc;
    if (argv[0].type == VAL_ARRAY) {
        *out = value_int(array_length(argv[0].as.as_array));
        return 1;
    }
    if (argv[0].type == VAL_STRING) {
        const char* s = argv[0].as.as_string ? argv[0].as.as_string : "";
        *out = value_int((int)strlen(s));
        return 1;
    }
    if (argv[0].type == VAL_MAP) {
        *out = value_int(map_count(argv[0].as.as_map));
        return 1;
    }
    vm_set_error(vm, "length expects a string, array, or map");
    return 0;
}

static int native_append(VM* vm, int argc, Value* argv, Value* out) {
    (void)argc;
    if (argv[0].type != VAL_ARRAY) {
        vm_set_error(vm, "append expects an array");
        return 0;
    }
    if (!array_append(argv[0].as.as_array, argv[1])) {
        vm_set_error(vm, "Out of memory");
        return 0;
    }
    value_retain(argv[0]);
    *out = argv[0];
    return 1;
}

static int native_delete(VM* vm, int argc, Value* argv, Value* out) {
    (void)argc;
    if (argv[0].type != VAL_MAP) {
        vm_set_error(vm, "delete expects a map");
        return 0;
    }
    if (argv[1].type != VAL_INT && argv[1].type != VAL_STRING) {
        vm_set_error(vm, "delete expects an int or string key");
        return 0;
    }
    map_delete(argv[0].as.as_map, argv[1]);
    value_retain(argv[0]);
    *out = argv[0];
    return 1;
}

static int native_first(VM* vm, int argc, Value* argv, Value* out) {
    (void)argc;
    if (argv[0].type != VAL_MAP) {
        vm_set_error(vm, "first expects a map");
        return 0;
    }
    MapObj* map = argv[0].as.as_map;
    Value key;
    if (map_first_key(map, &key)) {
        *out = key;
        return 1;
    }
    *out = value_int(0);
    return 1;
}

static int native_last(VM* vm, int argc, Value* argv, Value* out) {
    (void)argc;
    if (argv[0].type != VAL_MAP) {
        vm_set_error(vm, "last expects a map");
        return 0;
    }
    MapObj* map = argv[0].as.as_map;
    Value key;
    if (map_last_key(map, &key)) {
        *out = key;
        return 1;
    }
    *out = value_int(0);
    return 1;
}

static int native_next(VM* vm, int argc, Value* argv, Value* out) {
    (void)argc;
    if (argv[0].type != VAL_MAP) {
        vm_set_error(vm, "next expects a map");
        return 0;
    }
    if (argv[1].type != VAL_INT && argv[1].type != VAL_STRING) {
        vm_set_error(vm, "next expects an int or string key");
        return 0;
    }
    MapObj* map = argv[0].as.as_map;
    Value key;
    if (map_next_key(map, argv[1], &key)) {
        *out = key;
        return 1;
    }
    *out = value_int(0);
    return 1;
}

static int native_prior(VM* vm, int argc, Value* argv, Value* out) {
    (void)argc;
    if (argv[0].type != VAL_MAP) {
        vm_set_error(vm, "prior expects a map");
        return 0;
    }
    if (argv[1].type != VAL_INT && argv[1].type != VAL_STRING) {
        vm_set_error(vm, "prior expects an int or string key");
        return 0;
    }
    MapObj* map = argv[0].as.as_map;
    Value key;
    if (map_prior_key(map, argv[1], &key)) {
        *out = key;
        return 1;
    }
    *out = value_int(0);
    return 1;
}

static int native_extend(VM* vm, int argc, Value* argv, Value* out) {
    (void)argc;
    if (argv[0].type != VAL_ARRAY) {
        vm_set_error(vm, "extend expects an array");
        return 0;
    }
    if (argv[1].type != VAL_INT) {
        vm_set_error(vm, "extend expects an int count");
        return 0;
    }
    if (!array_extend(argv[0].as.as_array, argv[1].as.as_int)) {
        vm_set_error(vm, "Out of memory");
        return 0;
    }
    value_retain(argv[0]);
    *out = argv[0];
    return 1;
}

static int native_array_trim(VM* vm, int argc, Value* argv, Value* out) {
    (void)argc;
    if (argv[0].type != VAL_ARRAY) {
        vm_set_error(vm, "array_trim expects an array");
        return 0;
    }
    if (argv[1].type != VAL_INT) {
        vm_set_error(vm, "array_trim expects an int count");
        return 0;
    }
    array_trim(argv[0].as.as_array, argv[1].as.as_int);
    value_retain(argv[0]);
    *out = argv[0];
    return 1;
}

static int native_println(VM* vm, int argc, Value* argv, Value* out) {
    (void)vm;
    (void)argc;
    value_print(argv[0]);
    printf("\n");
    *out = value_int(0);
    return 1;
}

static int native_print(VM* vm, int argc, Value* argv, Value* out) {
    (void)vm;
    (void)argc;
    value_print(argv[0]);
    *out = value_int(0);
    return 1;
}

static int native_read_line(VM* vm, int argc, Value* argv, Value* out) {
    (void)vm;
    (void)argc;
    (void)argv;
    char buf[1024];
    if (fgets(buf, sizeof(buf), stdin) == NULL) {
        *out = value_string(strdup(""));
        return 1;
    }
    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') {
        buf[len - 1] = '\0';
        len--;
    }
    char* line = malloc(len + 1);
    if (line == NULL) {
        vm_set_error(vm, "Out of memory");
        return 0;
    }
    memcpy(line, buf, len + 1);
    *out = value_string(line);
    return 1;
}

static int native_clock(VM* vm, int argc, Value* argv, Value* out) {
    (void)vm;
    (void)argc;
    (void)argv;
    *out = value_int((int)(clock() * 1000 / CLOCKS_PER_SEC));
    return 1;
}

static int native_concat(VM* vm, int argc, Value* argv, Value* out) {
    (void)vm;
    (void)argc;
    if (argv[0].type != VAL_STRING || argv[1].type != VAL_STRING) {
        vm_set_error(vm, "concat expects two strings");
        return 0;
    }
    const char* a = argv[0].as.as_string ? argv[0].as.as_string : "";
    const char* b = argv[1].as.as_string ? argv[1].as.as_string : "";
    size_t len = strlen(a) + strlen(b) + 1;
    char* buf = malloc(len);
    if (buf == NULL) {
        vm_set_error(vm, "Out of memory");
        return 0;
    }
    snprintf(buf, len, "%s%s", a, b);
    *out = value_string(buf);
    return 1;
}

static int native_substring(VM* vm, int argc, Value* argv, Value* out) {
    (void)vm;
    (void)argc;
    if (argv[0].type != VAL_STRING || argv[1].type != VAL_INT || argv[2].type != VAL_INT) {
        vm_set_error(vm, "substring expects (string, int, int)");
        return 0;
    }
    const char* s = argv[0].as.as_string ? argv[0].as.as_string : "";
    int len_s = (int)strlen(s);
    int start = argv[1].as.as_int;
    int len = argv[2].as.as_int;
    if (start < 0) start = 0;
    if (start > len_s) start = len_s;
    if (len < 0) len = 0;
    if (start + len > len_s) len = len_s - start;
    char* buf = malloc((size_t)len + 1);
    if (buf == NULL) {
        vm_set_error(vm, "Out of memory");
        return 0;
    }
    memcpy(buf, s + start, len);
    buf[len] = '\0';
    *out = value_string(buf);
    return 1;
}

static int value_equal_values(Value a, Value b) {
    if (a.type != b.type) return 0;
    switch (a.type) {
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

static int native_contains(VM* vm, int argc, Value* argv, Value* out) {
    (void)vm;
    (void)argc;
    if (argv[0].type == VAL_STRING && argv[1].type == VAL_STRING) {
        const char* hay = argv[0].as.as_string ? argv[0].as.as_string : "";
        const char* needle = argv[1].as.as_string ? argv[1].as.as_string : "";
        *out = value_bool(strstr(hay, needle) != NULL);
        return 1;
    }
    if (argv[0].type == VAL_ARRAY) {
        ArrayObj* arr = argv[0].as.as_array;
        int len = array_length(arr);
        for (int i = 0; i < len; i++) {
            if (value_equal_values(array_get(arr, i), argv[1])) {
                *out = value_bool(1);
                return 1;
            }
        }
        *out = value_bool(0);
        return 1;
    }
    vm_set_error(vm, "contains expects (string, string) or (array, any)");
    return 0;
}

static int native_index_of(VM* vm, int argc, Value* argv, Value* out) {
    (void)vm;
    (void)argc;
    if (argv[0].type == VAL_STRING && argv[1].type == VAL_STRING) {
        const char* hay = argv[0].as.as_string ? argv[0].as.as_string : "";
        const char* needle = argv[1].as.as_string ? argv[1].as.as_string : "";
        const char* found = strstr(hay, needle);
        *out = value_int(found != NULL ? (int)(found - hay) : -1);
        return 1;
    }
    if (argv[0].type == VAL_ARRAY) {
        ArrayObj* arr = argv[0].as.as_array;
        int len = array_length(arr);
        for (int i = 0; i < len; i++) {
            if (value_equal_values(array_get(arr, i), argv[1])) {
                *out = value_int(i);
                return 1;
            }
        }
        *out = value_int(-1);
        return 1;
    }
    vm_set_error(vm, "index_of expects (string, string) or (array, any)");
    return 0;
}

static int native_array_find(VM* vm, int argc, Value* argv, Value* out) {
    (void)vm;
    (void)argc;
    if (argv[0].type != VAL_ARRAY) {
        vm_set_error(vm, "find expects an array");
        return 0;
    }
    ArrayObj* arr = argv[0].as.as_array;
    int len = array_length(arr);
    for (int i = 0; i < len; i++) {
        if (value_equal_values(array_get(arr, i), argv[1])) {
            *out = value_int(i);
            return 1;
        }
    }
    *out = value_int(-1);
    return 1;
}

static int native_to_upper(VM* vm, int argc, Value* argv, Value* out) {
    (void)vm;
    (void)argc;
    if (argv[0].type != VAL_STRING) {
        vm_set_error(vm, "to_upper expects a string");
        return 0;
    }
    const char* s = argv[0].as.as_string ? argv[0].as.as_string : "";
    char* buf = strdup(s);
    if (buf == NULL) {
        vm_set_error(vm, "Out of memory");
        return 0;
    }
    for (char* p = buf; *p; p++) {
        if (*p >= 'a' && *p <= 'z') *p -= 32;
    }
    *out = value_string(buf);
    return 1;
}

static int native_to_lower(VM* vm, int argc, Value* argv, Value* out) {
    (void)vm;
    (void)argc;
    if (argv[0].type != VAL_STRING) {
        vm_set_error(vm, "to_lower expects a string");
        return 0;
    }
    const char* s = argv[0].as.as_string ? argv[0].as.as_string : "";
    char* buf = strdup(s);
    if (buf == NULL) {
        vm_set_error(vm, "Out of memory");
        return 0;
    }
    for (char* p = buf; *p; p++) {
        if (*p >= 'A' && *p <= 'Z') *p += 32;
    }
    *out = value_string(buf);
    return 1;
}

static int native_trim_start(VM* vm, int argc, Value* argv, Value* out) {
    (void)vm;
    (void)argc;
    if (argv[0].type != VAL_STRING) {
        vm_set_error(vm, "trim_start expects a string");
        return 0;
    }
    const char* s = argv[0].as.as_string ? argv[0].as.as_string : "";
    while (isspace((unsigned char)*s)) s++;
    char* buf = strdup(s);
    if (buf == NULL) {
        vm_set_error(vm, "Out of memory");
        return 0;
    }
    *out = value_string(buf);
    return 1;
}

static int native_trim_end(VM* vm, int argc, Value* argv, Value* out) {
    (void)vm;
    (void)argc;
    if (argv[0].type != VAL_STRING) {
        vm_set_error(vm, "trim_end expects a string");
        return 0;
    }
    const char* s = argv[0].as.as_string ? argv[0].as.as_string : "";
    const char* end = s + strlen(s);
    while (end > s && isspace((unsigned char)*(end - 1))) end--;
    size_t len = (size_t)(end - s);
    char* buf = malloc(len + 1);
    if (buf == NULL) {
        vm_set_error(vm, "Out of memory");
        return 0;
    }
    memcpy(buf, s, len);
    buf[len] = '\0';
    *out = value_string(buf);
    return 1;
}

static int native_trim(VM* vm, int argc, Value* argv, Value* out) {
    (void)vm;
    (void)argc;
    if (argv[0].type != VAL_STRING) {
        vm_set_error(vm, "trim expects a string");
        return 0;
    }
    const char* s = argv[0].as.as_string ? argv[0].as.as_string : "";
    while (isspace((unsigned char)*s)) s++;
    const char* end = s + strlen(s);
    while (end > s && isspace((unsigned char)*(end - 1))) end--;
    size_t len = (size_t)(end - s);
    char* buf = malloc(len + 1);
    if (buf == NULL) {
        vm_set_error(vm, "Out of memory");
        return 0;
    }
    memcpy(buf, s, len);
    buf[len] = '\0';
    *out = value_string(buf);
    return 1;
}

static int native_starts_with(VM* vm, int argc, Value* argv, Value* out) {
    (void)vm;
    (void)argc;
    if (argv[0].type != VAL_STRING || argv[1].type != VAL_STRING) {
        vm_set_error(vm, "starts_with expects two strings");
        return 0;
    }
    const char* s = argv[0].as.as_string ? argv[0].as.as_string : "";
    const char* prefix = argv[1].as.as_string ? argv[1].as.as_string : "";
    size_t len_s = strlen(s);
    size_t len_p = strlen(prefix);
    *out = value_bool(len_p <= len_s && strncmp(s, prefix, len_p) == 0);
    return 1;
}

static int native_ends_with(VM* vm, int argc, Value* argv, Value* out) {
    (void)vm;
    (void)argc;
    if (argv[0].type != VAL_STRING || argv[1].type != VAL_STRING) {
        vm_set_error(vm, "ends_with expects two strings");
        return 0;
    }
    const char* s = argv[0].as.as_string ? argv[0].as.as_string : "";
    const char* suffix = argv[1].as.as_string ? argv[1].as.as_string : "";
    size_t len_s = strlen(s);
    size_t len_x = strlen(suffix);
    *out = value_bool(len_x <= len_s && strcmp(s + len_s - len_x, suffix) == 0);
    return 1;
}

static int native_char_at(VM* vm, int argc, Value* argv, Value* out) {
    (void)vm;
    (void)argc;
    if (argv[0].type != VAL_STRING || argv[1].type != VAL_INT) {
        vm_set_error(vm, "char_at expects (string, int)");
        return 0;
    }
    const char* s = argv[0].as.as_string ? argv[0].as.as_string : "";
    int idx = argv[1].as.as_int;
    int len = (int)strlen(s);
    if (idx < 0 || idx >= len) {
        vm_set_error(vm, "char_at: index out of bounds");
        return 0;
    }
    char buf[2] = {s[idx], '\0'};
    *out = value_string(strdup(buf));
    return 1;
}

static int native_reverse_string(VM* vm, int argc, Value* argv, Value* out) {
    (void)vm;
    (void)argc;
    if (argv[0].type != VAL_STRING) {
        vm_set_error(vm, "reverse_string expects a string");
        return 0;
    }
    const char* s = argv[0].as.as_string ? argv[0].as.as_string : "";
    size_t len = strlen(s);
    char* buf = malloc(len + 1);
    if (buf == NULL) {
        vm_set_error(vm, "Out of memory");
        return 0;
    }
    for (size_t i = 0; i < len; i++) {
        buf[i] = s[len - 1 - i];
    }
    buf[len] = '\0';
    *out = value_string(buf);
    return 1;
}

static int native_int_to_string(VM* vm, int argc, Value* argv, Value* out) {
    (void)vm;
    (void)argc;
    if (argv[0].type != VAL_INT) {
        vm_set_error(vm, "int_to_string expects an int");
        return 0;
    }
    char* buf = malloc(32);
    if (buf == NULL) {
        vm_set_error(vm, "Out of memory");
        return 0;
    }
    snprintf(buf, 32, "%d", argv[0].as.as_int);
    *out = value_string(buf);
    return 1;
}

static int native_float_to_string(VM* vm, int argc, Value* argv, Value* out) {
    (void)vm;
    (void)argc;
    if (argv[0].type != VAL_FLOAT) {
        vm_set_error(vm, "float_to_string expects a float");
        return 0;
    }
    double n = argv[0].as.as_float;
    char* buf = malloc(64);
    if (buf == NULL) {
        vm_set_error(vm, "Out of memory");
        return 0;
    }
    snprintf(buf, 64, "%g", n);
    *out = value_string(buf);
    return 1;
}

static int native_abs_int(VM* vm, int argc, Value* argv, Value* out) {
    (void)vm;
    (void)argc;
    if (argv[0].type != VAL_INT) {
        vm_set_error(vm, "abs_int expects an int");
        return 0;
    }
    int n = argv[0].as.as_int;
    if (n == INT_MIN) {
        vm_set_error(vm, "abs_int: absolute value of INT_MIN is undefined");
        return 0;
    }
    *out = value_int(n < 0 ? -n : n);
    return 1;
}

static int native_abs_float(VM* vm, int argc, Value* argv, Value* out) {
    (void)vm;
    (void)argc;
    if (argv[0].type != VAL_FLOAT) {
        vm_set_error(vm, "abs_float expects a float");
        return 0;
    }
    double n = argv[0].as.as_float;
    *out = value_float(n < 0.0 ? -n : n);
    return 1;
}

static int native_min_int(VM* vm, int argc, Value* argv, Value* out) {
    (void)vm;
    (void)argc;
    if (argv[0].type != VAL_INT || argv[1].type != VAL_INT) {
        vm_set_error(vm, "min_int expects two ints");
        return 0;
    }
    int a = argv[0].as.as_int;
    int b = argv[1].as.as_int;
    *out = value_int(a < b ? a : b);
    return 1;
}

static int native_max_int(VM* vm, int argc, Value* argv, Value* out) {
    (void)vm;
    (void)argc;
    if (argv[0].type != VAL_INT || argv[1].type != VAL_INT) {
        vm_set_error(vm, "max_int expects two ints");
        return 0;
    }
    int a = argv[0].as.as_int;
    int b = argv[1].as.as_int;
    *out = value_int(a > b ? a : b);
    return 1;
}

static int native_min_float(VM* vm, int argc, Value* argv, Value* out) {
    (void)vm;
    (void)argc;
    if (argv[0].type != VAL_FLOAT || argv[1].type != VAL_FLOAT) {
        vm_set_error(vm, "min_float expects two floats");
        return 0;
    }
    double a = argv[0].as.as_float;
    double b = argv[1].as.as_float;
    *out = value_float(a < b ? a : b);
    return 1;
}

static int native_max_float(VM* vm, int argc, Value* argv, Value* out) {
    (void)vm;
    (void)argc;
    if (argv[0].type != VAL_FLOAT || argv[1].type != VAL_FLOAT) {
        vm_set_error(vm, "max_float expects two floats");
        return 0;
    }
    double a = argv[0].as.as_float;
    double b = argv[1].as.as_float;
    *out = value_float(a > b ? a : b);
    return 1;
}

static int native_pow(VM* vm, int argc, Value* argv, Value* out) {
    (void)vm;
    (void)argc;
    double base = argv[0].type == VAL_FLOAT ? argv[0].as.as_float : (double)argv[0].as.as_int;
    double exp = argv[1].type == VAL_FLOAT ? argv[1].as.as_float : (double)argv[1].as.as_int;
    *out = value_float(pow(base, exp));
    return 1;
}

static int native_sqrt(VM* vm, int argc, Value* argv, Value* out) {
    (void)vm;
    (void)argc;
    double n = argv[0].type == VAL_FLOAT ? argv[0].as.as_float : (double)argv[0].as.as_int;
    if (n < 0.0) {
        vm_set_error(vm, "sqrt expects a non-negative number");
        return 0;
    }
    *out = value_float(sqrt(n));
    return 1;
}

static int native_round(VM* vm, int argc, Value* argv, Value* out) {
    (void)vm;
    (void)argc;
    if (argv[0].type != VAL_FLOAT && argv[0].type != VAL_INT) {
        vm_set_error(vm, "round expects a number");
        return 0;
    }
    double n = argv[0].type == VAL_FLOAT ? argv[0].as.as_float : (double)argv[0].as.as_int;
    *out = value_int((int)round(n));
    return 1;
}

static int native_floor(VM* vm, int argc, Value* argv, Value* out) {
    (void)vm;
    (void)argc;
    if (argv[0].type != VAL_FLOAT && argv[0].type != VAL_INT) {
        vm_set_error(vm, "floor expects a number");
        return 0;
    }
    double n = argv[0].type == VAL_FLOAT ? argv[0].as.as_float : (double)argv[0].as.as_int;
    *out = value_int((int)floor(n));
    return 1;
}

static int native_ceil(VM* vm, int argc, Value* argv, Value* out) {
    (void)vm;
    (void)argc;
    if (argv[0].type != VAL_FLOAT && argv[0].type != VAL_INT) {
        vm_set_error(vm, "ceil expects a number");
        return 0;
    }
    double n = argv[0].type == VAL_FLOAT ? argv[0].as.as_float : (double)argv[0].as.as_int;
    *out = value_int((int)ceil(n));
    return 1;
}

static int native_mod(VM* vm, int argc, Value* argv, Value* out) {
    (void)vm;
    (void)argc;
    if (argv[0].type != VAL_INT || argv[1].type != VAL_INT) {
        vm_set_error(vm, "mod expects two ints");
        return 0;
    }
    int b = argv[1].as.as_int;
    if (b == 0) {
        vm_set_error(vm, "mod: division by zero");
        return 0;
    }
    *out = value_int(argv[0].as.as_int % b);
    return 1;
}

static int native_read_file(VM* vm, int argc, Value* argv, Value* out) {
    (void)argc;
    if (argv[0].type != VAL_STRING) {
        vm_set_error(vm, "read_file expects a string path");
        return 0;
    }
    const char* path = argv[0].as.as_string ? argv[0].as.as_string : "";
    char* contents = os_read_file(path);
    if (contents == NULL) {
        vm_set_error(vm, "read_file: could not read file");
        return 0;
    }
    *out = value_string(contents);
    return 1;
}

static int native_write_file(VM* vm, int argc, Value* argv, Value* out) {
    (void)argc;
    if (argv[0].type != VAL_STRING || argv[1].type != VAL_STRING) {
        vm_set_error(vm, "write_file expects (string, string)");
        return 0;
    }
    const char* path = argv[0].as.as_string ? argv[0].as.as_string : "";
    const char* contents = argv[1].as.as_string ? argv[1].as.as_string : "";
    if (!os_write_file(path, contents, strlen(contents))) {
        vm_set_error(vm, "write_file: could not write file");
        return 0;
    }
    *out = value_int(1);
    return 1;
}

static int native_file_exists(VM* vm, int argc, Value* argv, Value* out) {
    (void)vm;
    (void)argc;
    if (argv[0].type != VAL_STRING) {
        vm_set_error(vm, "file_exists expects a string path");
        return 0;
    }
    const char* path = argv[0].as.as_string ? argv[0].as.as_string : "";
    *out = value_bool(os_file_exists(path));
    return 1;
}

static int native_is_dir(VM* vm, int argc, Value* argv, Value* out) {
    (void)vm;
    (void)argc;
    if (argv[0].type != VAL_STRING) {
        vm_set_error(vm, "is_dir expects a string path");
        return 0;
    }
    const char* path = argv[0].as.as_string ? argv[0].as.as_string : "";
    *out = value_bool(os_is_dir(path));
    return 1;
}

static int native_mkdir(VM* vm, int argc, Value* argv, Value* out) {
    (void)vm;
    (void)argc;
    if (argv[0].type != VAL_STRING) {
        vm_set_error(vm, "mkdir expects a string path");
        return 0;
    }
    const char* path = argv[0].as.as_string ? argv[0].as.as_string : "";
    *out = value_bool(os_mkdir(path));
    return 1;
}

/* Appends a value the caller has just created. array_append takes its own
   reference, so the caller's is dropped here, whether or not the append
   succeeded. */
static int array_append_new(ArrayObj* array, Value value) {
    int ok = array_append(array, value);
    value_release(value);
    return ok;
}

static int native_list_dir(VM* vm, int argc, Value* argv, Value* out) {
    (void)vm;
    (void)argc;
    if (argv[0].type != VAL_STRING) {
        vm_set_error(vm, "list_dir expects a string path");
        return 0;
    }
    const char* path = argv[0].as.as_string ? argv[0].as.as_string : "";
    char** names = NULL;
    int count = 0;
    if (!os_list_dir(path, &names, &count)) {
        vm_set_error(vm, "list_dir: could not read directory");
        return 0;
    }
    ArrayObj* arr = array_new();
    if (arr == NULL) {
        for (int i = 0; i < count; i++) free(names[i]);
        free(names);
        vm_set_error(vm, "Out of memory");
        return 0;
    }
    for (int i = 0; i < count; i++) {
        if (!array_append_new(arr, value_string(strdup(names[i])))) {
            for (int j = i; j < count; j++) free(names[j]);
            free(names);
            array_free(arr);
            vm_set_error(vm, "Out of memory");
            return 0;
        }
    }
    for (int i = 0; i < count; i++) free(names[i]);
    free(names);
    *out = value_array(arr);
    return 1;
}

static int native_is_digit(VM* vm, int argc, Value* argv, Value* out) {
    (void)vm;
    (void)argc;
    if (argv[0].type != VAL_STRING) {
        vm_set_error(vm, "is_digit expects a string");
        return 0;
    }
    const char* s = argv[0].as.as_string ? argv[0].as.as_string : "";
    int result = (s[0] != '\0' && s[1] == '\0' && isdigit((unsigned char)s[0]));
    *out = value_bool(result);
    return 1;
}

static int native_is_alpha(VM* vm, int argc, Value* argv, Value* out) {
    (void)vm;
    (void)argc;
    if (argv[0].type != VAL_STRING) {
        vm_set_error(vm, "is_alpha expects a string");
        return 0;
    }
    const char* s = argv[0].as.as_string ? argv[0].as.as_string : "";
    int result = (s[0] != '\0' && s[1] == '\0' && isalpha((unsigned char)s[0]));
    *out = value_bool(result);
    return 1;
}

static int native_split(VM* vm, int argc, Value* argv, Value* out) {
    (void)argc;
    if (argv[0].type != VAL_STRING || argv[1].type != VAL_STRING) {
        vm_set_error(vm, "split expects (string, string)");
        return 0;
    }
    const char* s = argv[0].as.as_string ? argv[0].as.as_string : "";
    const char* delim = argv[1].as.as_string ? argv[1].as.as_string : "";
    if (strlen(delim) == 0) {
        vm_set_error(vm, "split delimiter cannot be empty");
        return 0;
    }
    ArrayObj* arr = array_new();
    if (arr == NULL) {
        vm_set_error(vm, "Out of memory");
        return 0;
    }
    char* copy = strdup(s);
    if (copy == NULL) {
        array_free(arr);
        vm_set_error(vm, "Out of memory");
        return 0;
    }
    char* token = strtok(copy, delim);
    while (token != NULL) {
        if (!array_append_new(arr, value_string(strdup(token)))) {
            free(copy);
            array_free(arr);
            vm_set_error(vm, "Out of memory");
            return 0;
        }
        token = strtok(NULL, delim);
    }
    free(copy);
    *out = value_array(arr);
    return 1;
}

static int native_join(VM* vm, int argc, Value* argv, Value* out) {
    (void)argc;
    if (argv[0].type != VAL_ARRAY || argv[1].type != VAL_STRING) {
        vm_set_error(vm, "join expects (array<string>, string)");
        return 0;
    }
    ArrayObj* arr = argv[0].as.as_array;
    const char* delim = argv[1].as.as_string ? argv[1].as.as_string : "";
    size_t delim_len = strlen(delim);
    int count = array_length(arr);
    size_t total = 0;
    for (int i = 0; i < count; i++) {
        Value v = array_get(arr, i);
        if (v.type != VAL_STRING) {
            vm_set_error(vm, "join expects array<string>");
            return 0;
        }
        total += strlen(v.as.as_string ? v.as.as_string : "");
    }
    if (count > 1) {
        total += delim_len * (size_t)(count - 1);
    }
    char* buf = malloc(total + 1);
    if (buf == NULL) {
        vm_set_error(vm, "Out of memory");
        return 0;
    }
    buf[0] = '\0';
    for (int i = 0; i < count; i++) {
        if (i > 0) strcat(buf, delim);
        const char* part = array_get(arr, i).as.as_string;
        strcat(buf, part ? part : "");
    }
    *out = value_string(buf);
    return 1;
}

static int native_replace(VM* vm, int argc, Value* argv, Value* out) {
    (void)argc;
    if (argv[0].type != VAL_STRING || argv[1].type != VAL_STRING || argv[2].type != VAL_STRING) {
        vm_set_error(vm, "replace expects (string, string, string)");
        return 0;
    }
    const char* s = argv[0].as.as_string ? argv[0].as.as_string : "";
    const char* old = argv[1].as.as_string ? argv[1].as.as_string : "";
    const char* new_ = argv[2].as.as_string ? argv[2].as.as_string : "";
    size_t old_len = strlen(old);
    if (old_len == 0) {
        vm_set_error(vm, "replace: old string cannot be empty");
        return 0;
    }
    size_t new_len = strlen(new_);
    size_t count = 0;
    const char* tmp = s;
    const char* match;
    while ((match = strstr(tmp, old)) != NULL) {
        count++;
        tmp = match + old_len;
    }
    size_t result_len = strlen(s) + count * (new_len - old_len);
    char* buf = malloc(result_len + 1);
    if (buf == NULL) {
        vm_set_error(vm, "Out of memory");
        return 0;
    }
    char* dst = buf;
    const char* pos = s;
    while ((match = strstr(pos, old)) != NULL) {
        size_t prefix = (size_t)(match - pos);
        memcpy(dst, pos, prefix);
        dst += prefix;
        memcpy(dst, new_, new_len);
        dst += new_len;
        pos = match + old_len;
    }
    size_t tail = strlen(pos);
    memcpy(dst, pos, tail);
    dst += tail;
    *dst = '\0';
    *out = value_string(buf);
    return 1;
}

static int native_range(VM* vm, int argc, Value* argv, Value* out) {
    (void)vm;
    (void)argc;
    if (argv[0].type != VAL_INT || argv[1].type != VAL_INT) {
        vm_set_error(vm, "range expects two ints");
        return 0;
    }
    int start = argv[0].as.as_int;
    int end = argv[1].as.as_int;
    if (end < start) end = start;
    ArrayObj* arr = array_new();
    if (arr == NULL) {
        vm_set_error(vm, "Out of memory");
        return 0;
    }
    for (int i = start; i < end; i++) {
        if (!array_append(arr, value_int(i))) {
            array_free(arr);
            vm_set_error(vm, "Out of memory");
            return 0;
        }
    }
    *out = value_array(arr);
    return 1;
}

static int native_assert(VM* vm, int argc, Value* argv, Value* out) {
    (void)argc;
    int cond = 0;
    if (argv[0].type == VAL_BOOL || argv[0].type == VAL_INT) {
        cond = argv[0].as.as_int;
    }
    if (argv[1].type != VAL_STRING) {
        vm_set_error(vm, "assert expects (bool/int, string)");
        return 0;
    }
    if (!cond) {
        const char* msg = argv[1].as.as_string ? argv[1].as.as_string : "assertion failed";
        vm_set_error(vm, msg);
        return 0;
    }
    *out = value_int(0);
    return 1;
}

static int native_parse_int(VM* vm, int argc, Value* argv, Value* out) {
    (void)argc;
    if (argv[0].type != VAL_STRING) {
        vm_set_error(vm, "parse_int expects a string");
        return 0;
    }
    const char* s = argv[0].as.as_string ? argv[0].as.as_string : "";
    char* endptr = NULL;
    long n = strtol(s, &endptr, 10);
    if (endptr == s || *endptr != '\0') {
        vm_set_error(vm, "parse_int: invalid integer");
        return 0;
    }
    *out = value_int((int)n);
    return 1;
}

static int native_split_lines(VM* vm, int argc, Value* argv, Value* out) {
    (void)vm;
    (void)argc;
    if (argv[0].type != VAL_STRING) {
        vm_set_error(vm, "split_lines expects a string");
        return 0;
    }
    const char* s = argv[0].as.as_string ? argv[0].as.as_string : "";
    ArrayObj* arr = array_new();
    if (arr == NULL) {
        vm_set_error(vm, "Out of memory");
        return 0;
    }
    const char* line_start = s;
    for (const char* p = s; *p != '\0'; p++) {
        if (*p == '\n') {
            size_t len = (size_t)(p - line_start);
            char* part = malloc(len + 1);
            if (part == NULL) {
                array_free(arr);
                vm_set_error(vm, "Out of memory");
                return 0;
            }
            memcpy(part, line_start, len);
            part[len] = '\0';
            if (!array_append_new(arr, value_string(part))) {
                array_free(arr);
                vm_set_error(vm, "Out of memory");
                return 0;
            }
            line_start = p + 1;
        }
    }
    /* Append the final segment (even if it is empty). */
    size_t len = strlen(line_start);
    char* part = malloc(len + 1);
    if (part == NULL) {
        array_free(arr);
        vm_set_error(vm, "Out of memory");
        return 0;
    }
    memcpy(part, line_start, len);
    part[len] = '\0';
    if (!array_append_new(arr, value_string(part))) {
        array_free(arr);
        vm_set_error(vm, "Out of memory");
        return 0;
    }
    *out = value_array(arr);
    return 1;
}

static int native_join_paths(VM* vm, int argc, Value* argv, Value* out) {
    (void)vm;
    (void)argc;
    if (argv[0].type != VAL_STRING || argv[1].type != VAL_STRING) {
        vm_set_error(vm, "join_paths expects two strings");
        return 0;
    }
    const char* a = argv[0].as.as_string ? argv[0].as.as_string : "";
    const char* b = argv[1].as.as_string ? argv[1].as.as_string : "";
    if (os_path_is_absolute(b)) {
        *out = value_string(strdup(b));
        return out->as.as_string ? 1 : 0;
    }
    size_t a_len = strlen(a);
    size_t b_len = strlen(b);
    size_t total = a_len + b_len + 2;
    char* result = malloc(total);
    if (result == NULL) {
        vm_set_error(vm, "Out of memory");
        return 0;
    }
    if (a_len == 0) {
        strcpy(result, b);
    } else if (a[a_len - 1] == '/') {
        snprintf(result, total, "%s%s", a, b);
    } else {
        snprintf(result, total, "%s/%s", a, b);
    }
    *out = value_string(result);
    return 1;
}

static int native_repeat(VM* vm, int argc, Value* argv, Value* out) {
    (void)argc;
    if (argv[0].type != VAL_STRING || argv[1].type != VAL_INT) {
        vm_set_error(vm, "repeat expects (string, int)");
        return 0;
    }
    const char* s = argv[0].as.as_string ? argv[0].as.as_string : "";
    int count = argv[1].as.as_int;
    if (count < 0) count = 0;
    size_t len = strlen(s);
    char* buf = malloc(len * (size_t)count + 1);
    if (buf == NULL) {
        vm_set_error(vm, "Out of memory");
        return 0;
    }
    for (int i = 0; i < count; i++) {
        memcpy(buf + i * len, s, len);
    }
    buf[len * (size_t)count] = '\0';
    *out = value_string(buf);
    return 1;
}

static int native_format(VM* vm, int argc, Value* argv, Value* out) {
    (void)argc;
    if (argv[0].type != VAL_STRING || argv[1].type != VAL_ARRAY) {
        vm_set_error(vm, "format expects (string, array<string>)");
        return 0;
    }
    const char* fmt = argv[0].as.as_string ? argv[0].as.as_string : "";
    ArrayObj* arr = argv[1].as.as_array;
    int arg_count = array_length(arr);
    int placeholders = 0;
    for (const char* p = fmt; *p != '\0'; p++) {
        if (*p == '%' && *(p + 1) == 's') placeholders++;
    }
    if (placeholders != arg_count) {
        vm_set_error(vm, "format placeholder count does not match argument count");
        return 0;
    }
    size_t result_size = strlen(fmt) + 1;
    for (int i = 0; i < arg_count; i++) {
        Value v = array_get(arr, i);
        if (v.type != VAL_STRING) {
            vm_set_error(vm, "format arguments must be strings");
            return 0;
        }
        result_size += strlen(v.as.as_string ? v.as.as_string : "");
    }
    char* buf = malloc(result_size);
    if (buf == NULL) {
        vm_set_error(vm, "Out of memory");
        return 0;
    }
    char* dest = buf;
    const char* p = fmt;
    int idx = 0;
    while (*p != '\0') {
        if (*p == '%' && *(p + 1) == 's') {
            Value v = array_get(arr, idx++);
            const char* s = v.as.as_string ? v.as.as_string : "";
            size_t slen = strlen(s);
            memcpy(dest, s, slen);
            dest += slen;
            p += 2;
        } else {
            *dest++ = *p++;
        }
    }
    *dest = '\0';
    *out = value_string(buf);
    return 1;
}

static int compare_values(Value a, Value b) {
    if (a.type == VAL_INT && b.type == VAL_INT) {
        return a.as.as_int - b.as.as_int;
    }
    if (a.type == VAL_FLOAT || b.type == VAL_FLOAT) {
        double av = a.type == VAL_FLOAT ? a.as.as_float : (double)a.as.as_int;
        double bv = b.type == VAL_FLOAT ? b.as.as_float : (double)b.as.as_int;
        return av < bv ? -1 : (av > bv ? 1 : 0);
    }
    if (a.type == VAL_STRING && b.type == VAL_STRING) {
        const char* as = a.as.as_string ? a.as.as_string : "";
        const char* bs = b.as.as_string ? b.as.as_string : "";
        return strcmp(as, bs);
    }
    return 0;
}

static int native_sort(VM* vm, int argc, Value* argv, Value* out) {
    (void)argc;
    if (argv[0].type != VAL_ARRAY) {
        vm_set_error(vm, "sort expects an array");
        return 0;
    }
    ArrayObj* arr = argv[0].as.as_array;
    int len = array_length(arr);
    if (len > 0) {
        Value first = array_get(arr, 0);
        if (first.type != VAL_INT && first.type != VAL_FLOAT && first.type != VAL_STRING) {
            vm_set_error(vm, "sort expects array of int, float, or string");
            return 0;
        }
        for (int i = 1; i < len; i++) {
            if (array_get(arr, i).type != first.type) {
                vm_set_error(vm, "sort array elements must be the same type");
                return 0;
            }
        }
    }
    for (int i = 0; i < len - 1; i++) {
        for (int j = 0; j < len - 1 - i; j++) {
            Value a = array_get(arr, j);
            Value b = array_get(arr, j + 1);
            if (compare_values(a, b) > 0) {
                array_set(arr, j, b);
                array_set(arr, j + 1, a);
            }
        }
    }
    value_retain(argv[0]);
    *out = argv[0];
    return 1;
}

static int native_reverse(VM* vm, int argc, Value* argv, Value* out) {
    (void)argc;
    if (argv[0].type != VAL_ARRAY) {
        vm_set_error(vm, "reverse expects an array");
        return 0;
    }
    ArrayObj* arr = argv[0].as.as_array;
    int len = array_length(arr);
    for (int i = 0; i < len / 2; i++) {
        Value a = array_get(arr, i);
        Value b = array_get(arr, len - 1 - i);
        array_set(arr, i, b);
        array_set(arr, len - 1 - i, a);
    }
    value_retain(argv[0]);
    *out = argv[0];
    return 1;
}

static int native_slice(VM* vm, int argc, Value* argv, Value* out) {
    (void)vm;
    (void)argc;
    if (argv[0].type != VAL_ARRAY || argv[1].type != VAL_INT || argv[2].type != VAL_INT) {
        vm_set_error(vm, "slice expects (array, int, int)");
        return 0;
    }
    ArrayObj* arr = argv[0].as.as_array;
    int len = array_length(arr);
    int start = argv[1].as.as_int;
    int end = argv[2].as.as_int;
    if (start < 0) start = 0;
    if (end > len) end = len;
    if (end < start) end = start;
    ArrayObj* result = array_new();
    if (result == NULL) {
        vm_set_error(vm, "Out of memory");
        return 0;
    }
    for (int i = start; i < end; i++) {
        Value v = array_get(arr, i);
        if (!array_append(result, v)) {
            array_free(result);
            vm_set_error(vm, "Out of memory");
            return 0;
        }
    }
    *out = value_array(result);
    return 1;
}

static int native_remove_at(VM* vm, int argc, Value* argv, Value* out) {
    (void)vm;
    (void)argc;
    if (argv[0].type != VAL_ARRAY || argv[1].type != VAL_INT) {
        vm_set_error(vm, "remove_at expects (array, int)");
        return 0;
    }
    ArrayObj* arr = argv[0].as.as_array;
    int len = array_length(arr);
    int idx = argv[1].as.as_int;
    if (idx < 0 || idx >= len) {
        vm_set_error(vm, "remove_at: index out of bounds");
        return 0;
    }
    ArrayObj* result = array_new();
    if (result == NULL) {
        vm_set_error(vm, "Out of memory");
        return 0;
    }
    for (int i = 0; i < len; i++) {
        if (i == idx) continue;
        Value v = array_get(arr, i);
        if (!array_append(result, v)) {
            array_free(result);
            vm_set_error(vm, "Out of memory");
            return 0;
        }
    }
    *out = value_array(result);
    return 1;
}

static int native_insert(VM* vm, int argc, Value* argv, Value* out) {
    (void)vm;
    (void)argc;
    if (argv[0].type != VAL_ARRAY || argv[1].type != VAL_INT) {
        vm_set_error(vm, "insert expects (array, int, any)");
        return 0;
    }
    ArrayObj* arr = argv[0].as.as_array;
    int len = array_length(arr);
    int idx = argv[1].as.as_int;
    if (idx < 0 || idx > len) {
        vm_set_error(vm, "insert: index out of bounds");
        return 0;
    }
    ArrayObj* result = array_new();
    if (result == NULL) {
        vm_set_error(vm, "Out of memory");
        return 0;
    }
    for (int i = 0; i < len; i++) {
        if (i == idx) {
            if (!array_append(result, argv[2])) {
                array_free(result);
                vm_set_error(vm, "Out of memory");
                return 0;
            }
        }
        Value v = array_get(arr, i);
        if (!array_append(result, v)) {
            array_free(result);
            vm_set_error(vm, "Out of memory");
            return 0;
        }
    }
    if (idx == len) {
        if (!array_append(result, argv[2])) {
            array_free(result);
            vm_set_error(vm, "Out of memory");
            return 0;
        }
    }
    *out = value_array(result);
    return 1;
}

static int native_clamp(VM* vm, int argc, Value* argv, Value* out) {
    (void)argc;
    if (argc != 3) {
        vm_set_error(vm, "clamp expects 3 arguments");
        return 0;
    }
    int all_int = argv[0].type == VAL_INT && argv[1].type == VAL_INT && argv[2].type == VAL_INT;
    int all_numeric = (argv[0].type == VAL_INT || argv[0].type == VAL_FLOAT) &&
                      (argv[1].type == VAL_INT || argv[1].type == VAL_FLOAT) &&
                      (argv[2].type == VAL_INT || argv[2].type == VAL_FLOAT);
    if (!all_numeric) {
        vm_set_error(vm, "clamp expects numeric arguments");
        return 0;
    }
    if (all_int) {
        int v = argv[0].as.as_int;
        int lo = argv[1].as.as_int;
        int hi = argv[2].as.as_int;
        if (lo > hi) { int tmp = lo; lo = hi; hi = tmp; }
        if (v < lo) v = lo;
        if (v > hi) v = hi;
        *out = value_int(v);
        return 1;
    }
    double v = argv[0].type == VAL_FLOAT ? argv[0].as.as_float : (double)argv[0].as.as_int;
    double lo = argv[1].type == VAL_FLOAT ? argv[1].as.as_float : (double)argv[1].as.as_int;
    double hi = argv[2].type == VAL_FLOAT ? argv[2].as.as_float : (double)argv[2].as.as_int;
    if (lo > hi) { double tmp = lo; lo = hi; hi = tmp; }
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    *out = value_float(v);
    return 1;
}

/* coalesce(a, b) / nvl(a, b): return the first non-NULL argument.
   The chosen argument is retained because the VM releases argv after the
   native returns. */
static int native_coalesce(VM* vm, int argc, Value* argv, Value* out) {
    if (argc != 2) {
        vm_set_error(vm, "coalesce expects 2 arguments");
        return 0;
    }
    Value chosen = argv[0].type != VAL_NULL ? argv[0] : argv[1];
    value_retain(chosen);
    *out = chosen;
    return 1;
}

static int native_env_get(VM* vm, int argc, Value* argv, Value* out) {
    (void)vm;
    (void)argc;
    if (argv[0].type != VAL_STRING) {
        vm_set_error(vm, "env_get expects a string");
        return 0;
    }
    const char* name = argv[0].as.as_string ? argv[0].as.as_string : "";
    const char* value = getenv(name);
    char* buf = strdup(value != NULL ? value : "");
    if (buf == NULL) {
        vm_set_error(vm, "Out of memory");
        return 0;
    }
    *out = value_string(buf);
    return 1;
}

static int native_sleep(VM* vm, int argc, Value* argv, Value* out) {
    (void)vm;
    (void)argc;
    if (argv[0].type != VAL_INT) {
        vm_set_error(vm, "sleep expects an int");
        return 0;
    }
    int ms = argv[0].as.as_int;
    if (ms < 0) {
        vm_set_error(vm, "sleep expects a non-negative duration");
        return 0;
    }
    usleep((useconds_t)(ms * 1000));
    *out = value_int(0);
    return 1;
}

static int native_random_int(VM* vm, int argc, Value* argv, Value* out) {
    (void)vm;
    (void)argc;
    if (argv[0].type != VAL_INT || argv[1].type != VAL_INT) {
        vm_set_error(vm, "random_int expects two ints");
        return 0;
    }
    int lo = argv[0].as.as_int;
    int hi = argv[1].as.as_int;
    if (lo > hi) {
        vm_set_error(vm, "random_int: min must be <= max");
        return 0;
    }
    static int seeded = 0;
    if (!seeded) {
        srand((unsigned int)time(NULL));
        seeded = 1;
    }
    int range = hi - lo + 1;
    *out = value_int(lo + (rand() % range));
    return 1;
}

static int native_array_fill(VM* vm, int argc, Value* argv, Value* out) {
    (void)vm;
    (void)argc;
    if (argv[0].type != VAL_INT) {
        vm_set_error(vm, "array_fill expects (int, any)");
        return 0;
    }
    int count = argv[0].as.as_int;
    if (count < 0) {
        vm_set_error(vm, "array_fill: count must be non-negative");
        return 0;
    }
    ArrayObj* result = array_new();
    if (result == NULL) {
        vm_set_error(vm, "Out of memory");
        return 0;
    }
    for (int i = 0; i < count; i++) {
        if (!array_append(result, argv[1])) {
            array_free(result);
            vm_set_error(vm, "Out of memory");
            return 0;
        }
    }
    *out = value_array(result);
    return 1;
}

static int native_pad_start(VM* vm, int argc, Value* argv, Value* out) {
    (void)vm;
    (void)argc;
    if (argv[0].type != VAL_STRING || argv[1].type != VAL_INT || argv[2].type != VAL_STRING) {
        vm_set_error(vm, "pad_start expects (string, int, string)");
        return 0;
    }
    const char* s = argv[0].as.as_string ? argv[0].as.as_string : "";
    int target = argv[1].as.as_int;
    const char* pad = argv[2].as.as_string ? argv[2].as.as_string : "";
    if (target < 0) {
        vm_set_error(vm, "pad_start: target length must be non-negative");
        return 0;
    }
    size_t s_len = strlen(s);
    if ((int)s_len >= target) {
        char* buf = strdup(s);
        if (buf == NULL) {
            vm_set_error(vm, "Out of memory");
            return 0;
        }
        *out = value_string(buf);
        return 1;
    }
    size_t pad_len = strlen(pad);
    if (pad_len == 0) {
        char* buf = strdup(s);
        if (buf == NULL) {
            vm_set_error(vm, "Out of memory");
            return 0;
        }
        *out = value_string(buf);
        return 1;
    }
    size_t needed = (size_t)target - s_len;
    size_t buf_len = (size_t)target;
    char* buf = malloc(buf_len + 1);
    if (buf == NULL) {
        vm_set_error(vm, "Out of memory");
        return 0;
    }
    size_t p = 0;
    for (size_t i = 0; i < needed; i++) {
        buf[p++] = pad[i % pad_len];
    }
    memcpy(buf + p, s, s_len);
    buf[buf_len] = '\0';
    *out = value_string(buf);
    return 1;
}

static int native_sql_rowcount(VM* vm, int argc, Value* argv, Value* out) {
    (void)argc;
    (void)argv;
    *out = value_int(vm_get_sql_rowcount(vm));
    return 1;
}

static int native_sql_found(VM* vm, int argc, Value* argv, Value* out) {
    (void)argc;
    (void)argv;
    *out = value_bool(vm_get_sql_found(vm));
    return 1;
}

static int native_sql_notfound(VM* vm, int argc, Value* argv, Value* out) {
    (void)argc;
    (void)argv;
    *out = value_bool(vm_get_sql_notfound(vm));
    return 1;
}

static int native_execute_immediate(VM* vm, int argc, Value* argv, Value* out) {
    (void)argc;
    if (argv[0].type != VAL_STRING) {
        vm_set_error(vm, "execute_immediate expects a string");
        return 0;
    }
    const char* sql = argv[0].as.as_string ? argv[0].as.as_string : "";
    DBDriver* driver = vm_get_driver(vm);
    if (driver == NULL) {
        vm_set_error(vm, "execute_immediate: no database driver");
        return 0;
    }
    /* Routes through the VM so matching triggers fire and DROP TRIGGER is
       intercepted (see vm_dynamic_exec). */
    int row_count = vm_dynamic_exec(vm, sql);
    if (row_count < 0) {
        if (vm_get_error(vm) == NULL) {
            if (driver->error_message[0] != '\0') {
                vm_set_error(vm, driver->error_message);
            } else {
                vm_set_error(vm, "execute_immediate: SQL execution failed");
            }
        }
        return 0;
    }
    vm_set_sql_rowcount(vm, row_count);
    *out = value_int(row_count);
    return 1;
}

static int native_sqlcode(VM* vm, int argc, Value* argv, Value* out) {
    (void)argc;
    (void)argv;
    *out = value_int(vm_get_sql_code(vm));
    return 1;
}

static int native_sqlerrm(VM* vm, int argc, Value* argv, Value* out) {
    (void)argc;
    (void)argv;
    const char* msg = vm_get_sql_errm(vm);
    char* copy = strdup(msg != NULL ? msg : "");
    if (copy == NULL) {
        vm_set_error(vm, "Out of memory");
        return 0;
    }
    *out = value_string(copy);
    return 1;
}

static int native_raise_application_error(VM* vm, int argc, Value* argv, Value* out) {
    (void)argc;
    (void)out;
    if (argv[0].type != VAL_INT || argv[1].type != VAL_STRING) {
        vm_set_error(vm, "raise_application_error expects (int, string)");
        return 0;
    }
    int code = argv[0].as.as_int;
    const char* msg = argv[1].as.as_string ? argv[1].as.as_string : "";
    vm_set_error_with_code(vm, msg, code);
    return 0;
}

static int native_pad_end(VM* vm, int argc, Value* argv, Value* out) {
    (void)vm;
    (void)argc;
    if (argv[0].type != VAL_STRING || argv[1].type != VAL_INT || argv[2].type != VAL_STRING) {
        vm_set_error(vm, "pad_end expects (string, int, string)");
        return 0;
    }
    const char* s = argv[0].as.as_string ? argv[0].as.as_string : "";
    int target = argv[1].as.as_int;
    const char* pad = argv[2].as.as_string ? argv[2].as.as_string : "";
    if (target < 0) {
        vm_set_error(vm, "pad_end: target length must be non-negative");
        return 0;
    }
    size_t s_len = strlen(s);
    if ((int)s_len >= target) {
        char* buf = strdup(s);
        if (buf == NULL) {
            vm_set_error(vm, "Out of memory");
            return 0;
        }
        *out = value_string(buf);
        return 1;
    }
    size_t pad_len = strlen(pad);
    if (pad_len == 0) {
        char* buf = strdup(s);
        if (buf == NULL) {
            vm_set_error(vm, "Out of memory");
            return 0;
        }
        *out = value_string(buf);
        return 1;
    }
    size_t needed = (size_t)target - s_len;
    size_t buf_len = (size_t)target;
    char* buf = malloc(buf_len + 1);
    if (buf == NULL) {
        vm_set_error(vm, "Out of memory");
        return 0;
    }
    memcpy(buf, s, s_len);
    size_t p = s_len;
    for (size_t i = 0; i < needed; i++) {
        buf[p++] = pad[i % pad_len];
    }
    buf[buf_len] = '\0';
    *out = value_string(buf);
    return 1;
}

static int parse_datetime(const char* s, int* year, int* month, int* day,
                          int* hour, int* minute, int* second) {
    *hour = 0;
    *minute = 0;
    *second = 0;
    return sscanf(s, "%d-%d-%d %d:%d:%d", year, month, day, hour, minute, second) >= 3;
}

static int native_to_date(VM* vm, int argc, Value* argv, Value* out) {
    (void)argc;
    if (argv[0].type != VAL_STRING || argv[1].type != VAL_STRING) {
        vm_set_error(vm, "to_date expects (string, string)");
        return 0;
    }
    const char* s = argv[0].as.as_string ? argv[0].as.as_string : "";
    (void)argv[1];
    int year, month, day, hour, minute, second;
    if (!parse_datetime(s, &year, &month, &day, &hour, &minute, &second)) {
        vm_set_error(vm, "to_date: invalid date string");
        return 0;
    }
    char* buf = malloc(128);
    if (buf == NULL) {
        vm_set_error(vm, "Out of memory");
        return 0;
    }
    snprintf(buf, 128, "%04d-%02d-%02d", year, month, day);
    *out = value_date(buf);
    return 1;
}

static void append_token(char** out, size_t* cap, size_t* len, const char* token, size_t token_len) {
    if (*len + token_len + 1 > *cap) {
        size_t new_cap = *cap == 0 ? 64 : *cap * 2;
        while (new_cap < *len + token_len + 1) new_cap *= 2;
        char* new_out = realloc(*out, new_cap);
        if (new_out == NULL) return;
        *out = new_out;
        *cap = new_cap;
    }
    memcpy(*out + *len, token, token_len);
    *len += token_len;
    (*out)[*len] = '\0';
}

static int native_to_char(VM* vm, int argc, Value* argv, Value* out) {
    (void)argc;
    if ((argv[0].type != VAL_DATE && argv[0].type != VAL_TIMESTAMP) || argv[1].type != VAL_STRING) {
        vm_set_error(vm, "to_char expects (date/timestamp, string)");
        return 0;
    }
    const char* s = argv[0].as.as_string ? argv[0].as.as_string : "";
    const char* fmt = argv[1].as.as_string ? argv[1].as.as_string : "";
    int year, month, day, hour, minute, second;
    if (!parse_datetime(s, &year, &month, &day, &hour, &minute, &second)) {
        vm_set_error(vm, "to_char: invalid date/timestamp");
        return 0;
    }

    char* result = malloc(1);
    if (result == NULL) {
        vm_set_error(vm, "Out of memory");
        return 0;
    }
    result[0] = '\0';
    size_t cap = 1;
    size_t len = 0;

    const char* p = fmt;
    while (*p != '\0') {
        if (strncmp(p, "YYYY", 4) == 0) {
            char token[5];
            snprintf(token, sizeof(token), "%04d", year);
            append_token(&result, &cap, &len, token, strlen(token));
            p += 4;
        } else if (strncmp(p, "MM", 2) == 0) {
            char token[3];
            snprintf(token, sizeof(token), "%02d", month);
            append_token(&result, &cap, &len, token, strlen(token));
            p += 2;
        } else if (strncmp(p, "DD", 2) == 0) {
            char token[3];
            snprintf(token, sizeof(token), "%02d", day);
            append_token(&result, &cap, &len, token, strlen(token));
            p += 2;
        } else if (strncmp(p, "HH24", 4) == 0) {
            char token[3];
            snprintf(token, sizeof(token), "%02d", hour);
            append_token(&result, &cap, &len, token, strlen(token));
            p += 4;
        } else if (strncmp(p, "MI", 2) == 0) {
            char token[3];
            snprintf(token, sizeof(token), "%02d", minute);
            append_token(&result, &cap, &len, token, strlen(token));
            p += 2;
        } else if (strncmp(p, "SS", 2) == 0) {
            char token[3];
            snprintf(token, sizeof(token), "%02d", second);
            append_token(&result, &cap, &len, token, strlen(token));
            p += 2;
        } else {
            append_token(&result, &cap, &len, p, 1);
            p++;
        }
    }

    *out = value_string(result);
    return 1;
}

static int native_current_date(VM* vm, int argc, Value* argv, Value* out) {
    (void)vm;
    (void)argc;
    (void)argv;
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    if (tm_info == NULL) {
        vm_set_error(vm, "current_date: could not get local time");
        return 0;
    }
    char* buf = malloc(128);
    if (buf == NULL) {
        vm_set_error(vm, "Out of memory");
        return 0;
    }
    snprintf(buf, 128, "%04d-%02d-%02d", tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday);
    *out = value_date(buf);
    return 1;
}

static int native_current_timestamp(VM* vm, int argc, Value* argv, Value* out) {
    (void)vm;
    (void)argc;
    (void)argv;
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    if (tm_info == NULL) {
        vm_set_error(vm, "current_timestamp: could not get local time");
        return 0;
    }
    char* buf = malloc(128);
    if (buf == NULL) {
        vm_set_error(vm, "Out of memory");
        return 0;
    }
    snprintf(buf, 128, "%04d-%02d-%02d %02d:%02d:%02d",
             tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
             tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);
    *out = value_timestamp(buf);
    return 1;
}

static int native_dbms_output_enable(VM* vm, int argc, Value* argv, Value* out) {
    (void)out;
    if (argc != 1 || argv[0].type != VAL_INT) {
        vm_set_error(vm, "dbms_output_enable expects an int");
        return 0;
    }
    vm_dbms_output_enable(vm, argv[0].as.as_int);
    *out = value_int(0);
    return 1;
}

static int native_dbms_output_put_line(VM* vm, int argc, Value* argv, Value* out) {
    (void)out;
    if (argc != 1 || argv[0].type != VAL_STRING) {
        vm_set_error(vm, "dbms_output_put_line expects a string");
        return 0;
    }
    vm_dbms_output_put_line(vm, argv[0]);
    *out = value_int(0);
    return 1;
}

static int native_dbms_output_disable(VM* vm, int argc, Value* argv, Value* out) {
    (void)argc; (void)argv;
    vm_dbms_output_disable(vm);
    *out = value_int(0);
    return 1;
}

static int native_dbms_output_get_lines(VM* vm, int argc, Value* argv, Value* out) {
    (void)argc; (void)argv;
    *out = vm_dbms_output_get_lines(vm);
    return 1;
}

static int native_utl_file_fopen(VM* vm, int argc, Value* argv, Value* out) {
    if (argc != 2 || argv[0].type != VAL_STRING || argv[1].type != VAL_STRING) {
        vm_set_error(vm, "utl_file_fopen expects (string, string)");
        return 0;
    }
    const char* path = argv[0].as.as_string ? argv[0].as.as_string : "";
    const char* mode = argv[1].as.as_string ? argv[1].as.as_string : "";
    *out = value_int(vm_utl_file_fopen(vm, path, mode));
    return 1;
}

static int native_utl_file_get_line(VM* vm, int argc, Value* argv, Value* out) {
    if (argc != 1 || argv[0].type != VAL_INT) {
        vm_set_error(vm, "utl_file_get_line expects an int handle");
        return 0;
    }
    return vm_utl_file_get_line(vm, argv[0].as.as_int, out);
}

static int native_utl_file_put_line(VM* vm, int argc, Value* argv, Value* out) {
    if (argc != 2 || argv[0].type != VAL_INT || argv[1].type != VAL_STRING) {
        vm_set_error(vm, "utl_file_put_line expects (int, string)");
        return 0;
    }
    const char* text = argv[1].as.as_string ? argv[1].as.as_string : "";
    *out = value_int(vm_utl_file_put_line(vm, argv[0].as.as_int, text) ? 0 : -1);
    return 1;
}

static int native_utl_file_fseek(VM* vm, int argc, Value* argv, Value* out) {
    if (argc != 2 || argv[0].type != VAL_INT || argv[1].type != VAL_INT) {
        vm_set_error(vm, "utl_file_fseek expects (int, int)");
        return 0;
    }
    *out = value_int(vm_utl_file_fseek(vm, argv[0].as.as_int, argv[1].as.as_int));
    return 1;
}

static int native_utl_file_fflush(VM* vm, int argc, Value* argv, Value* out) {
    if (argc != 1 || argv[0].type != VAL_INT) {
        vm_set_error(vm, "utl_file_fflush expects an int handle");
        return 0;
    }
    *out = value_int(vm_utl_file_fflush(vm, argv[0].as.as_int) ? 0 : -1);
    return 1;
}

static int native_utl_file_fclose(VM* vm, int argc, Value* argv, Value* out) {
    if (argc != 1 || argv[0].type != VAL_INT) {
        vm_set_error(vm, "utl_file_fclose expects an int handle");
        return 0;
    }
    *out = value_int(vm_utl_file_fclose(vm, argv[0].as.as_int) ? 0 : -1);
    return 1;
}

static int native_utl_file_mkdir(VM* vm, int argc, Value* argv, Value* out) {
    if (argc != 1 || argv[0].type != VAL_STRING) {
        vm_set_error(vm, "utl_file_mkdir expects a string path");
        return 0;
    }
    const char* path = argv[0].as.as_string ? argv[0].as.as_string : "";
    *out = value_int(vm_utl_file_mkdir(path));
    return 1;
}

static int native_utl_file_remove(VM* vm, int argc, Value* argv, Value* out) {
    if (argc != 1 || argv[0].type != VAL_STRING) {
        vm_set_error(vm, "utl_file_remove expects a string path");
        return 0;
    }
    const char* path = argv[0].as.as_string ? argv[0].as.as_string : "";
    *out = value_int(vm_utl_file_remove(path));
    return 1;
}

static int native_dbms_sql_execute(VM* vm, int argc, Value* argv, Value* out) {
    if (argc != 1 || argv[0].type != VAL_STRING) {
        vm_set_error(vm, "dbms_sql_execute expects a string");
        return 0;
    }
    const char* sql = argv[0].as.as_string ? argv[0].as.as_string : "";
    int rc = vm_dbms_sql_execute(vm, sql);
    if (rc < 0) {
        DBDriver* driver = vm_get_driver(vm);
        if (driver != NULL && driver->error_message[0] != '\0') {
            vm_set_error(vm, driver->error_message);
        } else {
            vm_set_error(vm, "dbms_sql_execute failed");
        }
        return 0;
    }
    *out = value_int(rc);
    return 1;
}

static int native_dbms_sql_query(VM* vm, int argc, Value* argv, Value* out) {
    if (argc != 1 || argv[0].type != VAL_STRING) {
        vm_set_error(vm, "dbms_sql_query expects a string");
        return 0;
    }
    const char* sql = argv[0].as.as_string ? argv[0].as.as_string : "";
    *out = vm_dbms_sql_query(vm, sql);
    return 1;
}

static int native_dbms_sql_open_cursor(VM* vm, int argc, Value* argv, Value* out) {
    (void)argc;
    (void)argv;
    int handle = vm_dbms_sql_open_cursor(vm);
    if (handle < 0) {
        vm_set_error(vm, "dbms_sql.open_cursor: no available cursor handle");
        return 0;
    }
    *out = value_int(handle);
    return 1;
}

static int native_dbms_sql_parse(VM* vm, int argc, Value* argv, Value* out) {
    if (argc != 2 || argv[0].type != VAL_INT || argv[1].type != VAL_STRING) {
        vm_set_error(vm, "dbms_sql_parse expects (int, string)");
        return 0;
    }
    int ok = vm_dbms_sql_parse(vm, argv[0].as.as_int,
                               argv[1].as.as_string ? argv[1].as.as_string : "");
    *out = value_int(ok ? 0 : -1);
    return ok;
}

static int native_dbms_sql_bind_variable(VM* vm, int argc, Value* argv, Value* out) {
    if (argc != 3 || argv[0].type != VAL_INT || argv[1].type != VAL_STRING) {
        vm_set_error(vm, "dbms_sql_bind_variable expects (int, string, value)");
        return 0;
    }
    int ok = vm_dbms_sql_bind_variable(vm, argv[0].as.as_int,
                                       argv[1].as.as_string ? argv[1].as.as_string : "",
                                       argv[2]);
    *out = value_int(ok ? 0 : -1);
    return ok;
}

static int native_dbms_sql_cursor_execute(VM* vm, int argc, Value* argv, Value* out) {
    if (argc != 1 || argv[0].type != VAL_INT) {
        vm_set_error(vm, "dbms_sql_cursor_execute expects an int handle");
        return 0;
    }
    int rc = vm_dbms_sql_cursor_execute(vm, argv[0].as.as_int);
    if (rc < 0) return 0;
    *out = value_int(rc);
    return 1;
}

static int native_dbms_sql_fetch_rows(VM* vm, int argc, Value* argv, Value* out) {
    if (argc != 2 || argv[0].type != VAL_INT || argv[1].type != VAL_INT) {
        vm_set_error(vm, "dbms_sql_fetch_rows expects (int, int)");
        return 0;
    }
    *out = vm_dbms_sql_fetch_rows(vm, argv[0].as.as_int, argv[1].as.as_int);
    return 1;
}

static int native_dbms_sql_column_value(VM* vm, int argc, Value* argv, Value* out) {
    if (argc != 2 || argv[0].type != VAL_INT || argv[1].type != VAL_INT) {
        vm_set_error(vm, "dbms_sql_column_value expects (int, int)");
        return 0;
    }
    *out = vm_dbms_sql_column_value(vm, argv[0].as.as_int, argv[1].as.as_int);
    return 1;
}

static int native_dbms_sql_close_cursor(VM* vm, int argc, Value* argv, Value* out) {
    if (argc != 1 || argv[0].type != VAL_INT) {
        vm_set_error(vm, "dbms_sql_close_cursor expects an int handle");
        return 0;
    }
    int ok = vm_dbms_sql_close_cursor(vm, argv[0].as.as_int);
    *out = value_int(ok ? 0 : -1);
    return ok;
}

static int regex_compile(VM* vm, const char* pattern, regex_t* out_re) {
    int rc = regcomp(out_re, pattern, REG_EXTENDED);
    if (rc != 0) {
        char buf[128];
        regerror(rc, out_re, buf, sizeof(buf));
        char msg[256];
        snprintf(msg, sizeof(msg), "invalid regular expression: %s", buf);
        vm_set_error(vm, msg);
        regfree(out_re);
        return 0;
    }
    return 1;
}

static int native_regexp_like(VM* vm, int argc, Value* argv, Value* out) {
    if (argc != 2 || argv[0].type != VAL_STRING || argv[1].type != VAL_STRING) {
        vm_set_error(vm, "regexp_like expects (string, string)");
        return 0;
    }
    const char* subject = argv[0].as.as_string ? argv[0].as.as_string : "";
    const char* pattern = argv[1].as.as_string ? argv[1].as.as_string : "";
    regex_t re;
    if (!regex_compile(vm, pattern, &re)) return 0;
    *out = value_bool(regexec(&re, subject, 0, NULL, 0) == 0);
    regfree(&re);
    return 1;
}

static int native_regexp_substr(VM* vm, int argc, Value* argv, Value* out) {
    if (argc != 2 || argv[0].type != VAL_STRING || argv[1].type != VAL_STRING) {
        vm_set_error(vm, "regexp_substr expects (string, string)");
        return 0;
    }
    const char* subject = argv[0].as.as_string ? argv[0].as.as_string : "";
    const char* pattern = argv[1].as.as_string ? argv[1].as.as_string : "";
    regex_t re;
    if (!regex_compile(vm, pattern, &re)) return 0;
    regmatch_t match;
    if (regexec(&re, subject, 1, &match, 0) != 0) {
        regfree(&re);
        *out = value_string(strdup(""));
        return 1;
    }
    regfree(&re);
    size_t len = (size_t)(match.rm_eo - match.rm_so);
    char* result = malloc(len + 1);
    if (result == NULL) {
        vm_set_error(vm, "out of memory");
        return 0;
    }
    memcpy(result, subject + match.rm_so, len);
    result[len] = '\0';
    *out = value_string(result);
    return 1;
}

static int native_regexp_replace(VM* vm, int argc, Value* argv, Value* out) {
    if (argc != 3 || argv[0].type != VAL_STRING || argv[1].type != VAL_STRING ||
        argv[2].type != VAL_STRING) {
        vm_set_error(vm, "regexp_replace expects (string, string, string)");
        return 0;
    }
    const char* subject = argv[0].as.as_string ? argv[0].as.as_string : "";
    const char* pattern = argv[1].as.as_string ? argv[1].as.as_string : "";
    const char* replacement = argv[2].as.as_string ? argv[2].as.as_string : "";
    regex_t re;
    if (!regex_compile(vm, pattern, &re)) return 0;

    size_t cap = strlen(subject) + 1;
    char* result = malloc(cap);
    if (result == NULL) {
        regfree(&re);
        vm_set_error(vm, "out of memory");
        return 0;
    }
    size_t used = 0;
    const char* cursor = subject;
    regmatch_t match;
    while (regexec(&re, cursor, 1, &match, 0) == 0) {
        if (match.rm_eo == 0) break; /* empty match: avoid infinite loop */
        size_t prefix_len = (size_t)match.rm_so;
        size_t repl_len = strlen(replacement);
        size_t need = used + prefix_len + repl_len + 1;
        if (need > cap) {
            while (cap < need) cap *= 2;
            char* bigger = realloc(result, cap);
            if (bigger == NULL) {
                free(result);
                regfree(&re);
                vm_set_error(vm, "out of memory");
                return 0;
            }
            result = bigger;
        }
        memcpy(result + used, cursor, prefix_len);
        used += prefix_len;
        memcpy(result + used, replacement, repl_len);
        used += repl_len;
        cursor += match.rm_eo;
    }
    size_t tail_len = strlen(cursor);
    size_t need = used + tail_len + 1;
    if (need > cap) {
        char* bigger = realloc(result, need);
        if (bigger == NULL) {
            free(result);
            regfree(&re);
            vm_set_error(vm, "out of memory");
            return 0;
        }
        result = bigger;
    }
    memcpy(result + used, cursor, tail_len + 1);
    regfree(&re);
    *out = value_string(result);
    return 1;
}

static int native_create_sequence(VM* vm, int argc, Value* argv, Value* out) {
    if (argc != 3 || argv[0].type != VAL_STRING || argv[1].type != VAL_INT || argv[2].type != VAL_INT) {
        vm_set_error(vm, "create_sequence expects (string, int, int)");
        return 0;
    }
    const char* name = argv[0].as.as_string ? argv[0].as.as_string : "";
    if (!vm_sequence_create(vm, name, argv[1].as.as_int, argv[2].as.as_int)) {
        vm_set_error(vm, "create_sequence failed (duplicate name, empty name, or too many sequences)");
        return 0;
    }
    *out = value_int(0);
    return 1;
}

static int native_nextval(VM* vm, int argc, Value* argv, Value* out) {
    if (argc != 1 || argv[0].type != VAL_STRING) {
        vm_set_error(vm, "nextval expects a string sequence name");
        return 0;
    }
    const char* name = argv[0].as.as_string ? argv[0].as.as_string : "";
    int value = 0;
    if (!vm_sequence_nextval(vm, name, &value)) {
        vm_set_error(vm, "nextval: sequence does not exist");
        return 0;
    }
    *out = value_int(value);
    return 1;
}

static int native_currval(VM* vm, int argc, Value* argv, Value* out) {
    if (argc != 1 || argv[0].type != VAL_STRING) {
        vm_set_error(vm, "currval expects a string sequence name");
        return 0;
    }
    const char* name = argv[0].as.as_string ? argv[0].as.as_string : "";
    int value = 0;
    if (!vm_sequence_currval(vm, name, &value)) {
        vm_set_error(vm, "currval: sequence does not exist or nextval was never called");
        return 0;
    }
    *out = value_int(value);
    return 1;
}

static int native_drop_sequence(VM* vm, int argc, Value* argv, Value* out) {
    if (argc != 1 || argv[0].type != VAL_STRING) {
        vm_set_error(vm, "drop_sequence expects a string sequence name");
        return 0;
    }
    const char* name = argv[0].as.as_string ? argv[0].as.as_string : "";
    if (!vm_sequence_drop(vm, name)) {
        vm_set_error(vm, "drop_sequence: sequence does not exist");
        return 0;
    }
    *out = value_int(0);
    return 1;
}

/* external_call marshalling: Auspex int, float and string map to C int, double
 * and const char*. The argument's C type follows the runtime type of argv[2];
 * the return type is selected by which external_call* native was called. */
/* Same order as EXTERNAL_SIG_INT/FLOAT/STRING, so a signature's return type
   can be passed where an ExtKind is expected. */
typedef enum { EXT_INT, EXT_FLOAT, EXT_STRING } ExtKind;

/* Looks `sym` up in `lib`, reporting failures as `who`. */
static int ext_resolve(VM* vm, const char* who, const char* lib, const char* sym, void** addr) {
    void* handle = os_dl_open(lib != NULL ? lib : "");
    if (handle == NULL) {
        char msg[320];
        snprintf(msg, sizeof(msg), "%s: %s", who, os_dl_error());
        vm_set_error(vm, msg);
        return 0;
    }
    *addr = os_dl_sym(handle, sym != NULL ? sym : "");
    if (*addr == NULL) {
        char msg[320];
        snprintf(msg, sizeof(msg), "%s: %s", who, os_dl_error());
        os_dl_close(handle);
        vm_set_error(vm, msg);
        return 0;
    }
    return 1;
}

/* Wraps a C return value of kind `ret` as a Auspex value. */
static int ext_return_value(VM* vm, int ret, int i_ret, double f_ret, const char* s_ret, Value* out) {
    if (ret == EXT_INT) {
        *out = value_int(i_ret);
    } else if (ret == EXT_FLOAT) {
        *out = value_float(f_ret);
    } else if (s_ret == NULL) {
        *out = value_null();
    } else {
        /* The library owns the returned buffer: copy it, never free it. */
        char* copy = strdup(s_ret);
        if (copy == NULL) {
            vm_set_error(vm, "Out of memory");
            return 0;
        }
        *out = value_string(copy);
    }
    return 1;
}

static int external_call_invoke(VM* vm, const char* who, ExtKind ret, int argc, Value* argv, Value* out) {
    if (argc != 3 || argv[0].type != VAL_STRING || argv[1].type != VAL_STRING ||
        (argv[2].type != VAL_INT && argv[2].type != VAL_FLOAT && argv[2].type != VAL_STRING)) {
        char msg[128];
        snprintf(msg, sizeof(msg), "%s expects (string, string, int|float|string)", who);
        vm_set_error(vm, msg);
        return 0;
    }
    void* addr = NULL;
    if (!ext_resolve(vm, who, argv[0].as.as_string, argv[1].as.as_string, &addr)) return 0;

    ExtKind arg = argv[2].type == VAL_INT ? EXT_INT :
                  argv[2].type == VAL_FLOAT ? EXT_FLOAT : EXT_STRING;
    int i_arg = arg == EXT_INT ? argv[2].as.as_int : 0;
    double f_arg = arg == EXT_FLOAT ? argv[2].as.as_float : 0.0;
    const char* s_arg = arg == EXT_STRING && argv[2].as.as_string ? argv[2].as.as_string : "";

    /* Function pointers are copied out of the void* with memcpy, as the
     * object-to-function pointer cast is not valid ISO C. */
    int i_ret = 0;
    double f_ret = 0.0;
    const char* s_ret = NULL;
#define EXT_DISPATCH(RetT, ret_var)                                         \
    switch (arg) {                                                          \
        case EXT_INT: {                                                     \
            RetT (*fn)(int);                                                \
            memcpy(&fn, &addr, sizeof(fn));                                 \
            ret_var = fn(i_arg);                                            \
            break;                                                          \
        }                                                                   \
        case EXT_FLOAT: {                                                   \
            RetT (*fn)(double);                                             \
            memcpy(&fn, &addr, sizeof(fn));                                 \
            ret_var = fn(f_arg);                                            \
            break;                                                          \
        }                                                                   \
        case EXT_STRING: {                                                  \
            RetT (*fn)(const char*);                                        \
            memcpy(&fn, &addr, sizeof(fn));                                 \
            ret_var = fn(s_arg);                                            \
            break;                                                          \
        }                                                                   \
    }
    switch (ret) {
        case EXT_INT:    EXT_DISPATCH(int, i_ret); break;
        case EXT_FLOAT:  EXT_DISPATCH(double, f_ret); break;
        case EXT_STRING: EXT_DISPATCH(const char*, s_ret); break;
    }
#undef EXT_DISPATCH

    return ext_return_value(vm, ret, i_ret, f_ret, s_ret, out);
}

/* ----- external_call_sig: fixed multi-argument signatures -----
   Without libffi a C function can only be called through a function pointer
   of its exact prototype, so every return/argument combination a descriptor
   can name gets one. The X-macros below spell out all of them, 3 returns x
   (1 + 3 + 9 + 27 + 81) argument lists for 0..4 arguments, as the cases of
   one switch keyed by EXT_KEY. Type index 0/1/2 is int/double/const char*. */

int external_sig_parse(const char* sig, int* ret, int* arg_types, int* arg_count,
                       char* error, size_t error_size) {
    static const char* letters = "ids";
    const char* p = sig != NULL ? sig : "";
    const char* r = *p != '\0' ? strchr(letters, *p) : NULL;
    if (r == NULL || p[1] != '(') {
        snprintf(error, error_size,
                 "signature '%s' must look like \"i(ds)\": a return type i, d or s, "
                 "then the argument types in parentheses", p);
        return 0;
    }
    *ret = (int)(r - letters);
    int count = 0;
    for (p += 2; *p != ')'; p++) {
        const char* a = *p != '\0' ? strchr(letters, *p) : NULL;
        if (a == NULL) {
            snprintf(error, error_size,
                     "signature '%s': argument types are i, d or s, closed by ')'", sig);
            return 0;
        }
        if (count == EXTERNAL_SIG_MAX_ARGS) {
            snprintf(error, error_size, "signature '%s': at most %d arguments",
                     sig, EXTERNAL_SIG_MAX_ARGS);
            return 0;
        }
        arg_types[count++] = (int)(a - letters);
    }
    if (p[1] != '\0') {
        snprintf(error, error_size, "signature '%s': unexpected text after ')'", sig);
        return 0;
    }
    *arg_count = count;
    return 1;
}

typedef union {
    int i;
    double d;
    const char* s;
} ExtValue;

#define EXT_T0 int
#define EXT_T1 double
#define EXT_T2 const char*
#define EXT_F0 i
#define EXT_F1 d
#define EXT_F2 s

#define EXT_KEY(r, n, a, b, c, d) ((((r) * 5 + (n)) * 81) + (a) * 27 + (b) * 9 + (c) * 3 + (d))

/* One case: call through a pointer of this exact prototype. Function
   pointers are copied out of the void* with memcpy, as the object-to-
   function pointer cast is not valid ISO C. */
#define EXT_CASE(r, key, params, call_args)                         \
    case key: {                                                     \
        EXT_T##r (*fn) params;                                      \
        memcpy(&fn, &addr, sizeof(fn));                             \
        result.EXT_F##r = fn call_args;                             \
        break;                                                      \
    }

#define EXT_EACH_R(M) M(0) M(1) M(2)
#define EXT_EACH_1(M, ...) M(0, __VA_ARGS__) M(1, __VA_ARGS__) M(2, __VA_ARGS__)
#define EXT_EACH_2(M, ...) M(0, __VA_ARGS__) M(1, __VA_ARGS__) M(2, __VA_ARGS__)
#define EXT_EACH_3(M, ...) M(0, __VA_ARGS__) M(1, __VA_ARGS__) M(2, __VA_ARGS__)
#define EXT_EACH_4(M, ...) M(0, __VA_ARGS__) M(1, __VA_ARGS__) M(2, __VA_ARGS__)

#define EXT_C0(r) EXT_CASE(r, EXT_KEY(r, 0, 0, 0, 0, 0), (void), ())
#define EXT_C1(a, r)                                                \
    EXT_CASE(r, EXT_KEY(r, 1, a, 0, 0, 0), (EXT_T##a),              \
             (args[0].EXT_F##a))
#define EXT_C2(b, a, r)                                             \
    EXT_CASE(r, EXT_KEY(r, 2, a, b, 0, 0), (EXT_T##a, EXT_T##b),    \
             (args[0].EXT_F##a, args[1].EXT_F##b))
#define EXT_C3(c, b, a, r)                                          \
    EXT_CASE(r, EXT_KEY(r, 3, a, b, c, 0),                          \
             (EXT_T##a, EXT_T##b, EXT_T##c),                        \
             (args[0].EXT_F##a, args[1].EXT_F##b, args[2].EXT_F##c))
#define EXT_C4(d, c, b, a, r)                                       \
    EXT_CASE(r, EXT_KEY(r, 4, a, b, c, d),                          \
             (EXT_T##a, EXT_T##b, EXT_T##c, EXT_T##d),              \
             (args[0].EXT_F##a, args[1].EXT_F##b,                   \
              args[2].EXT_F##c, args[3].EXT_F##d))

#define EXT_A0(r) EXT_C0(r)
#define EXT_A1(r) EXT_EACH_1(EXT_C1, r)
#define EXT_A2(r) EXT_EACH_1(EXT_A2_B, r)
#define EXT_A2_B(a, r) EXT_EACH_2(EXT_C2, a, r)
#define EXT_A3(r) EXT_EACH_1(EXT_A3_B, r)
#define EXT_A3_B(a, r) EXT_EACH_2(EXT_A3_C, a, r)
#define EXT_A3_C(b, a, r) EXT_EACH_3(EXT_C3, b, a, r)
#define EXT_A4(r) EXT_EACH_1(EXT_A4_B, r)
#define EXT_A4_B(a, r) EXT_EACH_2(EXT_A4_C, a, r)
#define EXT_A4_C(b, a, r) EXT_EACH_3(EXT_A4_D, b, a, r)
#define EXT_A4_D(c, b, a, r) EXT_EACH_4(EXT_C4, c, b, a, r)

static int ext_dispatch(void* addr, int ret, const int* types, int count,
                        const ExtValue* args, ExtValue* out) {
    int t[EXTERNAL_SIG_MAX_ARGS] = {0, 0, 0, 0};
    for (int i = 0; i < count; i++) t[i] = types[i];
    ExtValue result;
    result.i = 0;
    switch (EXT_KEY(ret, count, t[0], t[1], t[2], t[3])) {
        EXT_EACH_R(EXT_A0)
        EXT_EACH_R(EXT_A1)
        EXT_EACH_R(EXT_A2)
        EXT_EACH_R(EXT_A3)
        EXT_EACH_R(EXT_A4)
        default:
            return 0;
    }
    *out = result;
    return 1;
}

static int native_external_call_sig(VM* vm, int argc, Value* argv, Value* out) {
    const char* who = "external_call_sig";
    if (argc < 3 || argv[0].type != VAL_STRING || argv[1].type != VAL_STRING ||
        argv[2].type != VAL_STRING) {
        vm_set_error(vm, "external_call_sig expects (string library, string symbol, "
                         "string signature, arguments...)");
        return 0;
    }
    char msg[256];
    int ret = 0;
    int types[EXTERNAL_SIG_MAX_ARGS];
    int count = 0;
    if (!external_sig_parse(argv[2].as.as_string, &ret, types, &count, msg, sizeof(msg))) {
        char full[320];
        snprintf(full, sizeof(full), "%s: %s", who, msg);
        vm_set_error(vm, full);
        return 0;
    }
    if (argc - 3 != count) {
        snprintf(msg, sizeof(msg), "%s: signature '%s' takes %d argument(s), got %d",
                 who, argv[2].as.as_string, count, argc - 3);
        vm_set_error(vm, msg);
        return 0;
    }
    ExtValue args[EXTERNAL_SIG_MAX_ARGS];
    for (int i = 0; i < count; i++) {
        Value v = argv[3 + i];
        int ok = 1;
        if (types[i] == EXTERNAL_SIG_INT) {
            ok = v.type == VAL_INT;
            args[i].i = ok ? v.as.as_int : 0;
        } else if (types[i] == EXTERNAL_SIG_FLOAT) {
            ok = v.type == VAL_FLOAT || v.type == VAL_INT;
            args[i].d = v.type == VAL_FLOAT ? v.as.as_float : (double)v.as.as_int;
        } else {
            ok = v.type == VAL_STRING;
            args[i].s = ok && v.as.as_string != NULL ? v.as.as_string : "";
        }
        if (!ok) {
            static const char* names[] = {"an int", "a float", "a string"};
            snprintf(msg, sizeof(msg), "%s: argument %d must be %s, as the signature says",
                     who, i + 1, names[types[i]]);
            vm_set_error(vm, msg);
            return 0;
        }
    }

    void* addr = NULL;
    if (!ext_resolve(vm, who, argv[0].as.as_string, argv[1].as.as_string, &addr)) return 0;
    ExtValue result;
    if (!ext_dispatch(addr, ret, types, count, args, &result)) {
        vm_set_error(vm, "external_call_sig: unsupported signature");
        return 0;
    }
    return ext_return_value(vm, ret, result.i, result.d, result.s, out);
}

static int native_external_call(VM* vm, int argc, Value* argv, Value* out) {
    return external_call_invoke(vm, "external_call", EXT_INT, argc, argv, out);
}

static int native_external_call_float(VM* vm, int argc, Value* argv, Value* out) {
    return external_call_invoke(vm, "external_call_float", EXT_FLOAT, argc, argv, out);
}

static int native_external_call_string(VM* vm, int argc, Value* argv, Value* out) {
    return external_call_invoke(vm, "external_call_string", EXT_STRING, argc, argv, out);
}

static NativeDef natives[] = {
    {"length",  1, native_length},
    {"append",  2, native_append},
    {"delete",  2, native_delete},
    {"first",   1, native_first},
    {"last",    1, native_last},
    {"next",    2, native_next},
    {"prior",   2, native_prior},
    {"extend",  2, native_extend},
    {"array_trim", 2, native_array_trim},
    {"println", 1, native_println},
    {"print",   1, native_print},
    {"read_line", 0, native_read_line},
    {"clock",   0, native_clock},
    {"concat",  2, native_concat},
    {"substring", 3, native_substring},
    {"contains", 2, native_contains},
    {"index_of", 2, native_index_of},
    {"find", 2, native_array_find},
    {"to_upper", 1, native_to_upper},
    {"to_lower", 1, native_to_lower},
    {"trim",    1, native_trim},
    {"trim_start", 1, native_trim_start},
    {"trim_end", 1, native_trim_end},
    {"starts_with", 2, native_starts_with},
    {"ends_with", 2, native_ends_with},
    {"char_at", 2, native_char_at},
    {"reverse_string", 1, native_reverse_string},
    {"int_to_string", 1, native_int_to_string},
    {"float_to_string", 1, native_float_to_string},
    {"abs_int",   1, native_abs_int},
    {"abs_float", 1, native_abs_float},
    {"min_int",   2, native_min_int},
    {"max_int",   2, native_max_int},
    {"min_float", 2, native_min_float},
    {"max_float", 2, native_max_float},
    {"pow",       2, native_pow},
    {"sqrt",      1, native_sqrt},
    {"round",     1, native_round},
    {"floor",     1, native_floor},
    {"ceil",      1, native_ceil},
    {"mod",       2, native_mod},
    {"read_file", 1, native_read_file},
    {"write_file", 2, native_write_file},
    {"file_exists", 1, native_file_exists},
    {"is_dir", 1, native_is_dir},
    {"mkdir", 1, native_mkdir},
    {"list_dir", 1, native_list_dir},
    {"is_digit", 1, native_is_digit},
    {"is_alpha", 1, native_is_alpha},
    {"split", 2, native_split},
    {"join", 2, native_join},
    {"replace", 3, native_replace},
    {"repeat", 2, native_repeat},
    {"range", 2, native_range},
    {"format", 2, native_format},
    {"sort", 1, native_sort},
    {"reverse", 1, native_reverse},
    {"slice", 3, native_slice},
    {"remove_at", 2, native_remove_at},
    {"insert", 3, native_insert},
    {"array_fill", 2, native_array_fill},
    {"clamp", 3, native_clamp},
    {"coalesce", 2, native_coalesce},
    {"nvl", 2, native_coalesce},
    {"env_get", 1, native_env_get},
    {"sleep", 1, native_sleep},
    {"random_int", 2, native_random_int},
    {"pad_start", 3, native_pad_start},
    {"pad_end", 3, native_pad_end},
    {"to_date", 2, native_to_date},
    {"to_char", 2, native_to_char},
    {"current_date", 0, native_current_date},
    {"current_timestamp", 0, native_current_timestamp},
    {"sql_rowcount", 0, native_sql_rowcount},
    {"sql_found", 0, native_sql_found},
    {"sql_notfound", 0, native_sql_notfound},
    {"execute_immediate", 1, native_execute_immediate},
    {"sqlcode", 0, native_sqlcode},
    {"sqlerrm", 0, native_sqlerrm},
    {"raise_application_error", 2, native_raise_application_error},
    {"dbms_output_enable", 1, native_dbms_output_enable},
    {"dbms_output_put_line", 1, native_dbms_output_put_line},
    {"dbms_output_disable", 0, native_dbms_output_disable},
    {"dbms_output_get_lines", 0, native_dbms_output_get_lines},
    {"utl_file_fopen", 2, native_utl_file_fopen},
    {"utl_file_get_line", 1, native_utl_file_get_line},
    {"utl_file_put_line", 2, native_utl_file_put_line},
    {"utl_file_fseek", 2, native_utl_file_fseek},
    {"utl_file_fflush", 1, native_utl_file_fflush},
    {"utl_file_fclose", 1, native_utl_file_fclose},
    {"utl_file_mkdir", 1, native_utl_file_mkdir},
    {"utl_file_remove", 1, native_utl_file_remove},
    {"dbms_sql_execute", 1, native_dbms_sql_execute},
    {"dbms_sql_query", 1, native_dbms_sql_query},
    {"dbms_sql_open_cursor", 0, native_dbms_sql_open_cursor},
    {"dbms_sql_parse", 2, native_dbms_sql_parse},
    {"dbms_sql_bind_variable", 3, native_dbms_sql_bind_variable},
    {"dbms_sql_cursor_execute", 1, native_dbms_sql_cursor_execute},
    {"dbms_sql_fetch_rows", 2, native_dbms_sql_fetch_rows},
    {"dbms_sql_column_value", 2, native_dbms_sql_column_value},
    {"dbms_sql_close_cursor", 1, native_dbms_sql_close_cursor},
    {"regexp_like", 2, native_regexp_like},
    {"regexp_substr", 2, native_regexp_substr},
    {"regexp_replace", 3, native_regexp_replace},
    {"create_sequence", 3, native_create_sequence},
    {"nextval", 1, native_nextval},
    {"currval", 1, native_currval},
    {"drop_sequence", 1, native_drop_sequence},
    {"external_call", 3, native_external_call},
    {"external_call_float", 3, native_external_call_float},
    {"external_call_string", 3, native_external_call_string},
    {"external_call_sig", NATIVE_VARIADIC, native_external_call_sig},
    {"assert", 2, native_assert},
    {"parse_int", 1, native_parse_int},
    {"split_lines", 1, native_split_lines},
    {"join_paths", 2, native_join_paths},
    {NULL,      0, NULL}
};

int native_count(void) {
    int count = 0;
    while (natives[count].name != NULL) count++;
    return count;
}

int native_find(const char* name) {
    for (int i = 0; natives[i].name != NULL; i++) {
        if (strcmp(natives[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

int native_arity(int idx) {
    if (idx < 0 || idx >= native_count()) return -1;
    return natives[idx].arity;
}

int native_call(VM* vm, int idx, int argc, Value* argv, Value* out) {
    if (idx < 0 || idx >= native_count()) {
        vm_set_error(vm, "Unknown native function");
        return 0;
    }
    NativeDef* def = &natives[idx];
    if (def->arity == NATIVE_VARIADIC ? argc > MAX_NATIVE_ARGS : argc != def->arity) {
        char msg[256];
        snprintf(msg, sizeof(msg), "%s expects %d argument(s)", def->name, def->arity);
        vm_set_error(vm, msg);
        return 0;
    }
    return def->fn(vm, argc, argv, out);
}
