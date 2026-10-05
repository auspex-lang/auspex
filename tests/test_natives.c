#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "natives.h"
#include "vm.h"
#include "compiler.h"
#include "test_harness.h"

TEST(natives_finds_registered_functions) {
    ASSERT_INT_EQ(1, native_find("length") >= 0);
    ASSERT_INT_EQ(1, native_find("append") >= 0);
    ASSERT_INT_EQ(1, native_find("println") >= 0);
    ASSERT_INT_EQ(1, native_find("clock") >= 0);
    ASSERT_INT_EQ(0, native_find("not_a_native") >= 0);
}

TEST(natives_length_returns_array_length) {
    VM* vm = vm_init();
    ArrayObj* array = array_new();
    array_append(array, value_int(1));
    array_append(array, value_int(2));
    array_append(array, value_int(3));

    Value argv[1];
    argv[0] = value_array(array);
    Value result;
    ASSERT_INT_EQ(1, native_call(vm, native_find("length"), 1, argv, &result));
    ASSERT_INT_EQ(VAL_INT, result.type);
    ASSERT_INT_EQ(3, result.as.as_int);

    vm_free(vm);
}

TEST(natives_append_adds_element) {
    VM* vm = vm_init();
    ArrayObj* array = array_new();
    array_append(array, value_int(1));

    Value argv[2];
    argv[0] = value_array(array);
    argv[1] = value_int(2);
    Value result;
    ASSERT_INT_EQ(1, native_call(vm, native_find("append"), 2, argv, &result));
    ASSERT_INT_EQ(VAL_ARRAY, result.type);
    ASSERT_INT_EQ(2, array_length(result.as.as_array));

    vm_free(vm);
}

TEST(natives_clock_returns_non_negative_int) {
    VM* vm = vm_init();
    Value result;
    ASSERT_INT_EQ(1, native_call(vm, native_find("clock"), 0, NULL, &result));
    ASSERT_INT_EQ(VAL_INT, result.type);
    ASSERT_INT_EQ(1, result.as.as_int >= 0);
    vm_free(vm);
}

TEST(natives_wrong_arity_fails) {
    VM* vm = vm_init();
    Value result;
    Value argv[2];
    argv[0] = value_int(1);
    argv[1] = value_int(2);
    ASSERT_INT_EQ(0, native_call(vm, native_find("length"), 2, argv, &result));
    ASSERT_PTR_NOT_NULL(vm_get_error(vm));
    vm_free(vm);
}

TEST(natives_abs_int_returns_absolute_value) {
    VM* vm = vm_init();
    Value argv[1];
    argv[0] = value_int(-5);
    Value result;
    ASSERT_INT_EQ(1, native_call(vm, native_find("abs_int"), 1, argv, &result));
    ASSERT_INT_EQ(VAL_INT, result.type);
    ASSERT_INT_EQ(5, result.as.as_int);

    argv[0] = value_int(7);
    ASSERT_INT_EQ(1, native_call(vm, native_find("abs_int"), 1, argv, &result));
    ASSERT_INT_EQ(7, result.as.as_int);

    vm_free(vm);
}

TEST(natives_abs_int_rejects_float) {
    VM* vm = vm_init();
    Value argv[1];
    argv[0] = value_float(-3.5);
    Value result;
    ASSERT_INT_EQ(0, native_call(vm, native_find("abs_int"), 1, argv, &result));
    ASSERT_PTR_NOT_NULL(vm_get_error(vm));
    vm_free(vm);
}

TEST(natives_abs_float_returns_absolute_value) {
    VM* vm = vm_init();
    Value argv[1];
    argv[0] = value_float(-3.5);
    Value result;
    ASSERT_INT_EQ(1, native_call(vm, native_find("abs_float"), 1, argv, &result));
    ASSERT_INT_EQ(VAL_FLOAT, result.type);
    ASSERT_INT_EQ(1, result.as.as_float == 3.5);

    argv[0] = value_float(2.0);
    ASSERT_INT_EQ(1, native_call(vm, native_find("abs_float"), 1, argv, &result));
    ASSERT_INT_EQ(1, result.as.as_float == 2.0);

    vm_free(vm);
}

TEST(natives_abs_float_rejects_int) {
    VM* vm = vm_init();
    Value argv[1];
    argv[0] = value_int(-3);
    Value result;
    ASSERT_INT_EQ(0, native_call(vm, native_find("abs_float"), 1, argv, &result));
    ASSERT_PTR_NOT_NULL(vm_get_error(vm));
    vm_free(vm);
}

TEST(natives_abs_int_rejects_int_min) {
    VM* vm = vm_init();
    Value argv[1];
    argv[0] = value_int(INT_MIN);
    Value result;
    ASSERT_INT_EQ(0, native_call(vm, native_find("abs_int"), 1, argv, &result));
    ASSERT_PTR_NOT_NULL(vm_get_error(vm));
    vm_free(vm);
}

TEST(natives_min_int_returns_smaller) {
    VM* vm = vm_init();
    Value argv[2];
    argv[0] = value_int(3);
    argv[1] = value_int(7);
    Value result;
    ASSERT_INT_EQ(1, native_call(vm, native_find("min_int"), 2, argv, &result));
    ASSERT_INT_EQ(VAL_INT, result.type);
    ASSERT_INT_EQ(3, result.as.as_int);

    argv[0] = value_int(10);
    argv[1] = value_int(2);
    ASSERT_INT_EQ(1, native_call(vm, native_find("min_int"), 2, argv, &result));
    ASSERT_INT_EQ(2, result.as.as_int);

    vm_free(vm);
}

TEST(natives_max_int_returns_larger) {
    VM* vm = vm_init();
    Value argv[2];
    argv[0] = value_int(3);
    argv[1] = value_int(7);
    Value result;
    ASSERT_INT_EQ(1, native_call(vm, native_find("max_int"), 2, argv, &result));
    ASSERT_INT_EQ(VAL_INT, result.type);
    ASSERT_INT_EQ(7, result.as.as_int);

    argv[0] = value_int(10);
    argv[1] = value_int(2);
    ASSERT_INT_EQ(1, native_call(vm, native_find("max_int"), 2, argv, &result));
    ASSERT_INT_EQ(10, result.as.as_int);

    vm_free(vm);
}

TEST(natives_min_max_int_reject_float) {
    VM* vm = vm_init();
    Value argv[2];
    argv[0] = value_int(1);
    argv[1] = value_float(2.0);
    Value result;
    ASSERT_INT_EQ(0, native_call(vm, native_find("min_int"), 2, argv, &result));
    ASSERT_PTR_NOT_NULL(vm_get_error(vm));

    ASSERT_INT_EQ(0, native_call(vm, native_find("max_int"), 2, argv, &result));
    ASSERT_PTR_NOT_NULL(vm_get_error(vm));
    vm_free(vm);
}

TEST(natives_min_float_returns_smaller) {
    VM* vm = vm_init();
    Value argv[2];
    argv[0] = value_float(3.5);
    argv[1] = value_float(7.2);
    Value result;
    ASSERT_INT_EQ(1, native_call(vm, native_find("min_float"), 2, argv, &result));
    ASSERT_INT_EQ(VAL_FLOAT, result.type);
    ASSERT_INT_EQ(1, result.as.as_float == 3.5);
    vm_free(vm);
}

TEST(natives_max_float_returns_larger) {
    VM* vm = vm_init();
    Value argv[2];
    argv[0] = value_float(3.5);
    argv[1] = value_float(7.2);
    Value result;
    ASSERT_INT_EQ(1, native_call(vm, native_find("max_float"), 2, argv, &result));
    ASSERT_INT_EQ(VAL_FLOAT, result.type);
    ASSERT_INT_EQ(1, result.as.as_float == 7.2);
    vm_free(vm);
}

TEST(natives_min_max_float_reject_int) {
    VM* vm = vm_init();
    Value argv[2];
    argv[0] = value_float(1.0);
    argv[1] = value_int(2);
    Value result;
    ASSERT_INT_EQ(0, native_call(vm, native_find("min_float"), 2, argv, &result));
    ASSERT_PTR_NOT_NULL(vm_get_error(vm));

    ASSERT_INT_EQ(0, native_call(vm, native_find("max_float"), 2, argv, &result));
    ASSERT_PTR_NOT_NULL(vm_get_error(vm));
    vm_free(vm);
}

TEST(natives_range_returns_int_array) {
    VM* vm = vm_init();
    Value argv[2];
    argv[0] = value_int(1);
    argv[1] = value_int(4);
    Value result;
    ASSERT_INT_EQ(1, native_call(vm, native_find("range"), 2, argv, &result));
    ASSERT_INT_EQ(VAL_ARRAY, result.type);
    ASSERT_INT_EQ(3, array_length(result.as.as_array));
    ASSERT_INT_EQ(1, array_get(result.as.as_array, 0).as.as_int);
    ASSERT_INT_EQ(2, array_get(result.as.as_array, 1).as.as_int);
    ASSERT_INT_EQ(3, array_get(result.as.as_array, 2).as.as_int);
    vm_free(vm);
}

TEST(natives_assert_passes_on_true) {
    VM* vm = vm_init();
    Value argv[2];
    argv[0] = value_bool(1);
    argv[1] = value_string(strdup("should not fail"));
    Value result;
    ASSERT_INT_EQ(1, native_call(vm, native_find("assert"), 2, argv, &result));
    vm_free(vm);
}

TEST(natives_assert_fails_on_false) {
    VM* vm = vm_init();
    Value argv[2];
    argv[0] = value_bool(0);
    argv[1] = value_string(strdup("bad value"));
    Value result;
    ASSERT_INT_EQ(0, native_call(vm, native_find("assert"), 2, argv, &result));
    ASSERT_PTR_NOT_NULL(strstr(vm_get_error(vm), "bad value"));
    vm_free(vm);
}

TEST(natives_parse_int_parses_valid) {
    VM* vm = vm_init();
    Value argv[1];
    argv[0] = value_string(strdup("-42"));
    Value result;
    ASSERT_INT_EQ(1, native_call(vm, native_find("parse_int"), 1, argv, &result));
    ASSERT_INT_EQ(VAL_INT, result.type);
    ASSERT_INT_EQ(-42, result.as.as_int);
    vm_free(vm);
}

TEST(natives_parse_int_rejects_invalid) {
    VM* vm = vm_init();
    Value argv[1];
    argv[0] = value_string(strdup("abc"));
    Value result;
    ASSERT_INT_EQ(0, native_call(vm, native_find("parse_int"), 1, argv, &result));
    ASSERT_PTR_NOT_NULL(vm_get_error(vm));
    vm_free(vm);
}

TEST(natives_split_lines_splits_by_newline) {
    VM* vm = vm_init();
    Value argv[1];
    argv[0] = value_string(strdup("a\nb\nc"));
    Value result;
    ASSERT_INT_EQ(1, native_call(vm, native_find("split_lines"), 1, argv, &result));
    ASSERT_INT_EQ(VAL_ARRAY, result.type);
    ASSERT_INT_EQ(3, array_length(result.as.as_array));
    ASSERT_STRING_EQ("a", array_get(result.as.as_array, 0).as.as_string);
    ASSERT_STRING_EQ("b", array_get(result.as.as_array, 1).as.as_string);
    ASSERT_STRING_EQ("c", array_get(result.as.as_array, 2).as.as_string);
    vm_free(vm);
}

TEST(natives_split_lines_preserves_empty_lines) {
    VM* vm = vm_init();
    Value argv[1];
    argv[0] = value_string(strdup("a\n\nb"));
    Value result;
    ASSERT_INT_EQ(1, native_call(vm, native_find("split_lines"), 1, argv, &result));
    ASSERT_INT_EQ(VAL_ARRAY, result.type);
    ASSERT_INT_EQ(3, array_length(result.as.as_array));
    ASSERT_STRING_EQ("a", array_get(result.as.as_array, 0).as.as_string);
    ASSERT_STRING_EQ("", array_get(result.as.as_array, 1).as.as_string);
    ASSERT_STRING_EQ("b", array_get(result.as.as_array, 2).as.as_string);
    vm_free(vm);
}

TEST(natives_join_paths_joins_two_strings) {
    VM* vm = vm_init();
    Value argv[2];
    argv[0] = value_string(strdup("/home"));
    argv[1] = value_string(strdup("user"));
    Value result;
    ASSERT_INT_EQ(1, native_call(vm, native_find("join_paths"), 2, argv, &result));
    ASSERT_INT_EQ(VAL_STRING, result.type);
    ASSERT_STRING_EQ("/home/user", result.as.as_string);
    vm_free(vm);
}

TEST(natives_join_paths_avoids_double_slash) {
    VM* vm = vm_init();
    Value argv[2];
    argv[0] = value_string(strdup("/home/"));
    argv[1] = value_string(strdup("user"));
    Value result;
    ASSERT_INT_EQ(1, native_call(vm, native_find("join_paths"), 2, argv, &result));
    ASSERT_INT_EQ(VAL_STRING, result.type);
    ASSERT_STRING_EQ("/home/user", result.as.as_string);
    vm_free(vm);
}

TEST(natives_join_paths_respects_absolute_second) {
    VM* vm = vm_init();
    Value argv[2];
    argv[0] = value_string(strdup("/home"));
    argv[1] = value_string(strdup("/etc"));
    Value result;
    ASSERT_INT_EQ(1, native_call(vm, native_find("join_paths"), 2, argv, &result));
    ASSERT_INT_EQ(VAL_STRING, result.type);
    ASSERT_STRING_EQ("/etc", result.as.as_string);
    vm_free(vm);
}

TEST(natives_format_replaces_placeholders) {
    VM* vm = vm_init();
    ArrayObj* parts = array_new();
    array_append(parts, value_string(strdup("world")));
    array_append(parts, value_string(strdup("42")));
    Value argv[2];
    argv[0] = value_string(strdup("Hello %s, count %s"));
    argv[1] = value_array(parts);
    Value result;
    ASSERT_INT_EQ(1, native_call(vm, native_find("format"), 2, argv, &result));
    ASSERT_INT_EQ(VAL_STRING, result.type);
    ASSERT_STRING_EQ("Hello world, count 42", result.as.as_string);
    value_release(argv[0]);
    value_release(argv[1]);
    value_release(result);
    vm_free(vm);
}

TEST(natives_format_rejects_mismatch) {
    VM* vm = vm_init();
    ArrayObj* parts = array_new();
    Value argv[2];
    argv[0] = value_string(strdup("Hello %s"));
    argv[1] = value_array(parts);
    Value result;
    ASSERT_INT_EQ(0, native_call(vm, native_find("format"), 2, argv, &result));
    ASSERT_PTR_NOT_NULL(vm_get_error(vm));
    value_release(argv[0]);
    value_release(argv[1]);
    vm_free(vm);
}

TEST(natives_sort_ints_ascending) {
    VM* vm = vm_init();
    ArrayObj* arr = array_new();
    array_append(arr, value_int(3));
    array_append(arr, value_int(1));
    array_append(arr, value_int(2));
    Value argv[1];
    argv[0] = value_array(arr);
    Value result;
    ASSERT_INT_EQ(1, native_call(vm, native_find("sort"), 1, argv, &result));
    ASSERT_INT_EQ(VAL_ARRAY, result.type);
    ASSERT_INT_EQ(1, array_get(result.as.as_array, 0).as.as_int);
    ASSERT_INT_EQ(2, array_get(result.as.as_array, 1).as.as_int);
    ASSERT_INT_EQ(3, array_get(result.as.as_array, 2).as.as_int);
    value_release(argv[0]);
    value_release(result);
    vm_free(vm);
}

TEST(natives_sort_strings_ascending) {
    VM* vm = vm_init();
    ArrayObj* arr = array_new();
    array_append(arr, value_string(strdup("banana")));
    array_append(arr, value_string(strdup("apple")));
    Value argv[1];
    argv[0] = value_array(arr);
    Value result;
    ASSERT_INT_EQ(1, native_call(vm, native_find("sort"), 1, argv, &result));
    ASSERT_INT_EQ(VAL_ARRAY, result.type);
    ASSERT_STRING_EQ("apple", array_get(result.as.as_array, 0).as.as_string);
    ASSERT_STRING_EQ("banana", array_get(result.as.as_array, 1).as.as_string);
    value_release(argv[0]);
    value_release(result);
    vm_free(vm);
}

TEST(natives_reverse_array) {
    VM* vm = vm_init();
    ArrayObj* arr = array_new();
    array_append(arr, value_int(1));
    array_append(arr, value_int(2));
    array_append(arr, value_int(3));
    Value argv[1];
    argv[0] = value_array(arr);
    Value result;
    ASSERT_INT_EQ(1, native_call(vm, native_find("reverse"), 1, argv, &result));
    ASSERT_INT_EQ(VAL_ARRAY, result.type);
    ASSERT_INT_EQ(3, array_get(result.as.as_array, 0).as.as_int);
    ASSERT_INT_EQ(2, array_get(result.as.as_array, 1).as.as_int);
    ASSERT_INT_EQ(1, array_get(result.as.as_array, 2).as.as_int);
    value_release(argv[0]);
    value_release(result);
    vm_free(vm);
}

TEST(natives_contains_finds_array_element) {
    VM* vm = vm_init();
    ArrayObj* arr = array_new();
    array_append(arr, value_int(1));
    array_append(arr, value_int(2));
    array_append(arr, value_int(3));
    Value argv[2];
    argv[0] = value_array(arr);
    argv[1] = value_int(2);
    Value result;
    ASSERT_INT_EQ(1, native_call(vm, native_find("contains"), 2, argv, &result));
    ASSERT_INT_EQ(VAL_BOOL, result.type);
    ASSERT_INT_EQ(1, result.as.as_int);
    value_release(argv[0]);
    vm_free(vm);
}

TEST(natives_contains_returns_false_for_missing_array_element) {
    VM* vm = vm_init();
    ArrayObj* arr = array_new();
    array_append(arr, value_int(1));
    array_append(arr, value_int(2));
    Value argv[2];
    argv[0] = value_array(arr);
    argv[1] = value_int(3);
    Value result;
    ASSERT_INT_EQ(1, native_call(vm, native_find("contains"), 2, argv, &result));
    ASSERT_INT_EQ(VAL_BOOL, result.type);
    ASSERT_INT_EQ(0, result.as.as_int);
    value_release(argv[0]);
    vm_free(vm);
}

TEST(natives_index_of_returns_array_index) {
    VM* vm = vm_init();
    ArrayObj* arr = array_new();
    array_append(arr, value_int(10));
    array_append(arr, value_int(20));
    array_append(arr, value_int(30));
    Value argv[2];
    argv[0] = value_array(arr);
    argv[1] = value_int(20);
    Value result;
    ASSERT_INT_EQ(1, native_call(vm, native_find("index_of"), 2, argv, &result));
    ASSERT_INT_EQ(VAL_INT, result.type);
    ASSERT_INT_EQ(1, result.as.as_int);
    value_release(argv[0]);
    vm_free(vm);
}

TEST(natives_index_of_returns_minus_one_for_missing_array_element) {
    VM* vm = vm_init();
    ArrayObj* arr = array_new();
    array_append(arr, value_int(10));
    array_append(arr, value_int(20));
    Value argv[2];
    argv[0] = value_array(arr);
    argv[1] = value_int(30);
    Value result;
    ASSERT_INT_EQ(1, native_call(vm, native_find("index_of"), 2, argv, &result));
    ASSERT_INT_EQ(VAL_INT, result.type);
    ASSERT_INT_EQ(-1, result.as.as_int);
    value_release(argv[0]);
    vm_free(vm);
}

TEST(natives_slice_returns_subarray) {
    VM* vm = vm_init();
    ArrayObj* arr = array_new();
    array_append(arr, value_int(1));
    array_append(arr, value_int(2));
    array_append(arr, value_int(3));
    array_append(arr, value_int(4));
    Value argv[3];
    argv[0] = value_array(arr);
    argv[1] = value_int(1);
    argv[2] = value_int(3);
    Value result;
    ASSERT_INT_EQ(1, native_call(vm, native_find("slice"), 3, argv, &result));
    ASSERT_INT_EQ(VAL_ARRAY, result.type);
    ASSERT_INT_EQ(2, array_length(result.as.as_array));
    ASSERT_INT_EQ(2, array_get(result.as.as_array, 0).as.as_int);
    ASSERT_INT_EQ(3, array_get(result.as.as_array, 1).as.as_int);
    value_release(argv[0]);
    value_release(result);
    vm_free(vm);
}

TEST(natives_remove_at_returns_array_without_element) {
    VM* vm = vm_init();
    ArrayObj* arr = array_new();
    array_append(arr, value_int(1));
    array_append(arr, value_int(2));
    array_append(arr, value_int(3));
    Value argv[2];
    argv[0] = value_array(arr);
    argv[1] = value_int(1);
    Value result;
    ASSERT_INT_EQ(1, native_call(vm, native_find("remove_at"), 2, argv, &result));
    ASSERT_INT_EQ(VAL_ARRAY, result.type);
    ASSERT_INT_EQ(2, array_length(result.as.as_array));
    ASSERT_INT_EQ(1, array_get(result.as.as_array, 0).as.as_int);
    ASSERT_INT_EQ(3, array_get(result.as.as_array, 1).as.as_int);
    value_release(argv[0]);
    value_release(result);
    vm_free(vm);
}

TEST(natives_remove_at_rejects_out_of_bounds) {
    VM* vm = vm_init();
    ArrayObj* arr = array_new();
    array_append(arr, value_int(1));
    Value argv[2];
    argv[0] = value_array(arr);
    argv[1] = value_int(5);
    Value result;
    ASSERT_INT_EQ(0, native_call(vm, native_find("remove_at"), 2, argv, &result));
    ASSERT_PTR_NOT_NULL(vm_get_error(vm));
    value_release(argv[0]);
    vm_free(vm);
}

TEST(natives_clamp_int_within_range) {
    VM* vm = vm_init();
    Value argv[3];
    argv[0] = value_int(5);
    argv[1] = value_int(0);
    argv[2] = value_int(10);
    Value result;
    ASSERT_INT_EQ(1, native_call(vm, native_find("clamp"), 3, argv, &result));
    ASSERT_INT_EQ(VAL_INT, result.type);
    ASSERT_INT_EQ(5, result.as.as_int);
    vm_free(vm);
}

TEST(natives_clamp_int_below_range) {
    VM* vm = vm_init();
    Value argv[3];
    argv[0] = value_int(-5);
    argv[1] = value_int(0);
    argv[2] = value_int(10);
    Value result;
    ASSERT_INT_EQ(1, native_call(vm, native_find("clamp"), 3, argv, &result));
    ASSERT_INT_EQ(VAL_INT, result.type);
    ASSERT_INT_EQ(0, result.as.as_int);
    vm_free(vm);
}

TEST(natives_clamp_float_coerces_ints) {
    VM* vm = vm_init();
    Value argv[3];
    argv[0] = value_float(2.5);
    argv[1] = value_int(0);
    argv[2] = value_int(1);
    Value result;
    ASSERT_INT_EQ(1, native_call(vm, native_find("clamp"), 3, argv, &result));
    ASSERT_INT_EQ(VAL_FLOAT, result.type);
    ASSERT_FLOAT_EQ(1.0, result.as.as_float);
    vm_free(vm);
}

TEST(natives_clamp_rejects_string) {
    VM* vm = vm_init();
    Value argv[3];
    argv[0] = value_string(strdup("x"));
    argv[1] = value_int(0);
    argv[2] = value_int(1);
    Value result;
    ASSERT_INT_EQ(0, native_call(vm, native_find("clamp"), 3, argv, &result));
    ASSERT_PTR_NOT_NULL(vm_get_error(vm));
    value_release(argv[0]);
    vm_free(vm);
}

TEST(natives_pow_returns_float) {
    VM* vm = vm_init();
    Value argv[2];
    argv[0] = value_int(2);
    argv[1] = value_int(3);
    Value result;
    ASSERT_INT_EQ(1, native_call(vm, native_find("pow"), 2, argv, &result));
    ASSERT_INT_EQ(VAL_FLOAT, result.type);
    ASSERT_INT_EQ(1, result.as.as_float == 8.0);
    vm_free(vm);
}

TEST(natives_sqrt_returns_float) {
    VM* vm = vm_init();
    Value argv[1];
    argv[0] = value_int(16);
    Value result;
    ASSERT_INT_EQ(1, native_call(vm, native_find("sqrt"), 1, argv, &result));
    ASSERT_INT_EQ(VAL_FLOAT, result.type);
    ASSERT_INT_EQ(1, result.as.as_float == 4.0);
    vm_free(vm);
}

TEST(natives_sqrt_rejects_negative) {
    VM* vm = vm_init();
    Value argv[1];
    argv[0] = value_int(-1);
    Value result;
    ASSERT_INT_EQ(0, native_call(vm, native_find("sqrt"), 1, argv, &result));
    ASSERT_PTR_NOT_NULL(vm_get_error(vm));
    vm_free(vm);
}

TEST(natives_round_returns_int) {
    VM* vm = vm_init();
    Value argv[1];
    argv[0] = value_float(3.7);
    Value result;
    ASSERT_INT_EQ(1, native_call(vm, native_find("round"), 1, argv, &result));
    ASSERT_INT_EQ(VAL_INT, result.type);
    ASSERT_INT_EQ(4, result.as.as_int);
    vm_free(vm);
}

TEST(natives_floor_returns_int) {
    VM* vm = vm_init();
    Value argv[1];
    argv[0] = value_float(3.9);
    Value result;
    ASSERT_INT_EQ(1, native_call(vm, native_find("floor"), 1, argv, &result));
    ASSERT_INT_EQ(VAL_INT, result.type);
    ASSERT_INT_EQ(3, result.as.as_int);
    vm_free(vm);
}

TEST(natives_ceil_returns_int) {
    VM* vm = vm_init();
    Value argv[1];
    argv[0] = value_float(3.1);
    Value result;
    ASSERT_INT_EQ(1, native_call(vm, native_find("ceil"), 1, argv, &result));
    ASSERT_INT_EQ(VAL_INT, result.type);
    ASSERT_INT_EQ(4, result.as.as_int);
    vm_free(vm);
}

TEST(natives_mod_returns_int) {
    VM* vm = vm_init();
    Value argv[2];
    argv[0] = value_int(10);
    argv[1] = value_int(3);
    Value result;
    ASSERT_INT_EQ(1, native_call(vm, native_find("mod"), 2, argv, &result));
    ASSERT_INT_EQ(VAL_INT, result.type);
    ASSERT_INT_EQ(1, result.as.as_int);
    vm_free(vm);
}

TEST(natives_mod_rejects_division_by_zero) {
    VM* vm = vm_init();
    Value argv[2];
    argv[0] = value_int(10);
    argv[1] = value_int(0);
    Value result;
    ASSERT_INT_EQ(0, native_call(vm, native_find("mod"), 2, argv, &result));
    ASSERT_PTR_NOT_NULL(vm_get_error(vm));
    vm_free(vm);
}

TEST(natives_find_returns_index_of_existing_element) {
    VM* vm = vm_init();
    ArrayObj* arr = array_new();
    array_append(arr, value_int(10));
    array_append(arr, value_int(20));
    array_append(arr, value_int(30));
    Value argv[2];
    argv[0] = value_array(arr);
    argv[1] = value_int(20);
    Value result;
    ASSERT_INT_EQ(1, native_call(vm, native_find("find"), 2, argv, &result));
    ASSERT_INT_EQ(VAL_INT, result.type);
    ASSERT_INT_EQ(1, result.as.as_int);
    value_release(argv[0]);
    vm_free(vm);
}

TEST(natives_find_returns_minus_one_for_missing_element) {
    VM* vm = vm_init();
    ArrayObj* arr = array_new();
    array_append(arr, value_int(10));
    array_append(arr, value_int(20));
    Value argv[2];
    argv[0] = value_array(arr);
    argv[1] = value_int(30);
    Value result;
    ASSERT_INT_EQ(1, native_call(vm, native_find("find"), 2, argv, &result));
    ASSERT_INT_EQ(VAL_INT, result.type);
    ASSERT_INT_EQ(-1, result.as.as_int);
    value_release(argv[0]);
    vm_free(vm);
}

TEST(natives_insert_adds_element_at_index) {
    VM* vm = vm_init();
    ArrayObj* arr = array_new();
    array_append(arr, value_int(10));
    array_append(arr, value_int(30));
    Value argv[3];
    argv[0] = value_array(arr);
    argv[1] = value_int(1);
    argv[2] = value_int(20);
    Value result;
    ASSERT_INT_EQ(1, native_call(vm, native_find("insert"), 3, argv, &result));
    ASSERT_INT_EQ(VAL_ARRAY, result.type);
    ASSERT_INT_EQ(3, array_length(result.as.as_array));
    ASSERT_INT_EQ(10, array_get(result.as.as_array, 0).as.as_int);
    ASSERT_INT_EQ(20, array_get(result.as.as_array, 1).as.as_int);
    ASSERT_INT_EQ(30, array_get(result.as.as_array, 2).as.as_int);
    value_release(argv[0]);
    value_release(result);
    vm_free(vm);
}

TEST(natives_insert_rejects_out_of_bounds) {
    VM* vm = vm_init();
    ArrayObj* arr = array_new();
    array_append(arr, value_int(10));
    Value argv[3];
    argv[0] = value_array(arr);
    argv[1] = value_int(5);
    argv[2] = value_int(20);
    Value result;
    ASSERT_INT_EQ(0, native_call(vm, native_find("insert"), 3, argv, &result));
    ASSERT_PTR_NOT_NULL(vm_get_error(vm));
    value_release(argv[0]);
    vm_free(vm);
}

TEST(natives_trim_start_removes_leading_whitespace) {
    VM* vm = vm_init();
    Value argv[1];
    argv[0] = value_string(strdup("  hello  "));
    Value result;
    ASSERT_INT_EQ(1, native_call(vm, native_find("trim_start"), 1, argv, &result));
    ASSERT_INT_EQ(VAL_STRING, result.type);
    ASSERT_STRING_EQ("hello  ", result.as.as_string);
    value_release(argv[0]);
    value_release(result);
    vm_free(vm);
}

TEST(natives_trim_end_removes_trailing_whitespace) {
    VM* vm = vm_init();
    Value argv[1];
    argv[0] = value_string(strdup("  hello  "));
    Value result;
    ASSERT_INT_EQ(1, native_call(vm, native_find("trim_end"), 1, argv, &result));
    ASSERT_INT_EQ(VAL_STRING, result.type);
    ASSERT_STRING_EQ("  hello", result.as.as_string);
    value_release(argv[0]);
    value_release(result);
    vm_free(vm);
}

TEST(natives_trim_start_rejects_non_string) {
    VM* vm = vm_init();
    Value argv[1];
    argv[0] = value_int(123);
    Value result;
    ASSERT_INT_EQ(0, native_call(vm, native_find("trim_start"), 1, argv, &result));
    ASSERT_PTR_NOT_NULL(vm_get_error(vm));
    vm_free(vm);
}

TEST(natives_env_get_returns_variable_value) {
    setenv("AUSPEX_TEST_VAR", "hello", 1);
    VM* vm = vm_init();
    Value argv[1];
    argv[0] = value_string(strdup("AUSPEX_TEST_VAR"));
    Value result;
    ASSERT_INT_EQ(1, native_call(vm, native_find("env_get"), 1, argv, &result));
    ASSERT_INT_EQ(VAL_STRING, result.type);
    ASSERT_STRING_EQ("hello", result.as.as_string);
    value_release(argv[0]);
    value_release(result);
    vm_free(vm);
    unsetenv("AUSPEX_TEST_VAR");
}

TEST(natives_env_get_rejects_non_string) {
    VM* vm = vm_init();
    Value argv[1];
    argv[0] = value_int(123);
    Value result;
    ASSERT_INT_EQ(0, native_call(vm, native_find("env_get"), 1, argv, &result));
    ASSERT_PTR_NOT_NULL(vm_get_error(vm));
    vm_free(vm);
}

TEST(natives_sleep_returns_success) {
    VM* vm = vm_init();
    Value argv[1];
    argv[0] = value_int(10);
    Value result;
    ASSERT_INT_EQ(1, native_call(vm, native_find("sleep"), 1, argv, &result));
    ASSERT_INT_EQ(VAL_INT, result.type);
    vm_free(vm);
}

TEST(natives_random_int_returns_value_in_range) {
    VM* vm = vm_init();
    Value argv[2];
    argv[0] = value_int(1);
    argv[1] = value_int(10);
    Value result;
    ASSERT_INT_EQ(1, native_call(vm, native_find("random_int"), 2, argv, &result));
    ASSERT_INT_EQ(VAL_INT, result.type);
    ASSERT_INT_EQ(1, result.as.as_int >= 1);
    ASSERT_INT_EQ(1, result.as.as_int <= 10);
    vm_free(vm);
}

TEST(natives_random_int_rejects_inverted_range) {
    VM* vm = vm_init();
    Value argv[2];
    argv[0] = value_int(10);
    argv[1] = value_int(1);
    Value result;
    ASSERT_INT_EQ(0, native_call(vm, native_find("random_int"), 2, argv, &result));
    ASSERT_PTR_NOT_NULL(vm_get_error(vm));
    vm_free(vm);
}

TEST(natives_array_fill_creates_repeated_array) {
    VM* vm = vm_init();
    Value argv[2];
    argv[0] = value_int(3);
    argv[1] = value_int(7);
    Value result;
    ASSERT_INT_EQ(1, native_call(vm, native_find("array_fill"), 2, argv, &result));
    ASSERT_INT_EQ(VAL_ARRAY, result.type);
    ASSERT_INT_EQ(3, array_length(result.as.as_array));
    for (int i = 0; i < 3; i++) {
        ASSERT_INT_EQ(7, array_get(result.as.as_array, i).as.as_int);
    }
    value_release(result);
    vm_free(vm);
}

TEST(natives_array_fill_rejects_negative_count) {
    VM* vm = vm_init();
    Value argv[2];
    argv[0] = value_int(-1);
    argv[1] = value_int(7);
    Value result;
    ASSERT_INT_EQ(0, native_call(vm, native_find("array_fill"), 2, argv, &result));
    ASSERT_PTR_NOT_NULL(vm_get_error(vm));
    vm_free(vm);
}

TEST(natives_pad_start_pads_on_left) {
    VM* vm = vm_init();
    Value argv[3];
    argv[0] = value_string(strdup("42"));
    argv[1] = value_int(5);
    argv[2] = value_string(strdup("0"));
    Value result;
    ASSERT_INT_EQ(1, native_call(vm, native_find("pad_start"), 3, argv, &result));
    ASSERT_INT_EQ(VAL_STRING, result.type);
    ASSERT_STRING_EQ("00042", result.as.as_string);
    value_release(argv[0]);
    value_release(argv[2]);
    value_release(result);
    vm_free(vm);
}

TEST(natives_pad_end_pads_on_right) {
    VM* vm = vm_init();
    Value argv[3];
    argv[0] = value_string(strdup("42"));
    argv[1] = value_int(5);
    argv[2] = value_string(strdup("0"));
    Value result;
    ASSERT_INT_EQ(1, native_call(vm, native_find("pad_end"), 3, argv, &result));
    ASSERT_INT_EQ(VAL_STRING, result.type);
    ASSERT_STRING_EQ("42000", result.as.as_string);
    value_release(argv[0]);
    value_release(argv[2]);
    value_release(result);
    vm_free(vm);
}

TEST(natives_pad_start_rejects_non_string) {
    VM* vm = vm_init();
    Value argv[3];
    argv[0] = value_int(42);
    argv[1] = value_int(5);
    argv[2] = value_string(strdup("0"));
    Value result;
    ASSERT_INT_EQ(0, native_call(vm, native_find("pad_start"), 3, argv, &result));
    ASSERT_PTR_NOT_NULL(vm_get_error(vm));
    value_release(argv[2]);
    vm_free(vm);
}

TEST(natives_read_line_reads_from_stdin) {
    /* Point file descriptor 0 at a temporary file for the duration: stdin
       itself cannot be assigned everywhere (it is a macro on Windows). */
    FILE* fp = tmpfile();
    ASSERT_PTR_NOT_NULL(fp);
    fputs("hello\n", fp);
    fflush(fp);
    rewind(fp);
    int saved_stdin = dup(fileno(stdin));
    ASSERT_INT_EQ(1, saved_stdin >= 0);
    dup2(fileno(fp), fileno(stdin));
    clearerr(stdin);

    VM* vm = vm_init();
    Value result;
    ASSERT_INT_EQ(1, native_call(vm, native_find("read_line"), 0, NULL, &result));
    ASSERT_INT_EQ(VAL_STRING, result.type);
    ASSERT_STRING_EQ("hello", result.as.as_string);
    value_release(result);
    vm_free(vm);

    dup2(saved_stdin, fileno(stdin));
    close(saved_stdin);
    clearerr(stdin);
    fclose(fp);
}

TEST(natives_is_dir_detects_directory) {
    system("rm -rf /tmp/auspex_isdir_test");
    system("mkdir -p /tmp/auspex_isdir_test");

    VM* vm = vm_init();
    Value argv[1];
    Value result;

    argv[0] = value_string(strdup("/tmp/auspex_isdir_test"));
    ASSERT_INT_EQ(1, native_call(vm, native_find("is_dir"), 1, argv, &result));
    ASSERT_INT_EQ(VAL_BOOL, result.type);
    ASSERT_INT_EQ(1, result.as.as_int);
    value_release(argv[0]);
    value_release(result);

    argv[0] = value_string(strdup("/tmp/auspex_isdir_test_does_not_exist"));
    ASSERT_INT_EQ(1, native_call(vm, native_find("is_dir"), 1, argv, &result));
    ASSERT_INT_EQ(VAL_BOOL, result.type);
    ASSERT_INT_EQ(0, result.as.as_int);
    value_release(argv[0]);
    value_release(result);

    vm_free(vm);
    system("rm -rf /tmp/auspex_isdir_test");
}

TEST(natives_mkdir_creates_directory) {
    system("rm -rf /tmp/auspex_mkdir_test");

    VM* vm = vm_init();
    Value argv[1];
    argv[0] = value_string(strdup("/tmp/auspex_mkdir_test"));
    Value result;
    ASSERT_INT_EQ(1, native_call(vm, native_find("mkdir"), 1, argv, &result));
    ASSERT_INT_EQ(VAL_BOOL, result.type);
    ASSERT_INT_EQ(1, result.as.as_int);
    value_release(argv[0]);
    value_release(result);

    argv[0] = value_string(strdup("/tmp/auspex_mkdir_test"));
    ASSERT_INT_EQ(1, native_call(vm, native_find("is_dir"), 1, argv, &result));
    ASSERT_INT_EQ(VAL_BOOL, result.type);
    ASSERT_INT_EQ(1, result.as.as_int);
    value_release(argv[0]);
    value_release(result);

    vm_free(vm);
    system("rm -rf /tmp/auspex_mkdir_test");
}

TEST(natives_list_dir_lists_entries) {
    system("rm -rf /tmp/auspex_listdir_test");
    system("mkdir -p /tmp/auspex_listdir_test");
    system("touch /tmp/auspex_listdir_test/hello.txt");

    VM* vm = vm_init();
    Value argv[1];
    argv[0] = value_string(strdup("/tmp/auspex_listdir_test"));
    Value result;
    ASSERT_INT_EQ(1, native_call(vm, native_find("list_dir"), 1, argv, &result));
    ASSERT_INT_EQ(VAL_ARRAY, result.type);
    ASSERT_INT_EQ(1, array_length(result.as.as_array));
    ASSERT_STRING_EQ("hello.txt", array_get(result.as.as_array, 0).as.as_string);
    value_release(argv[0]);
    value_release(result);

    vm_free(vm);
    system("rm -rf /tmp/auspex_listdir_test");
}

TEST(natives_is_digit_checks_single_character) {
    VM* vm = vm_init();
    Value argv[1];
    Value result;

    argv[0] = value_string(strdup("5"));
    ASSERT_INT_EQ(1, native_call(vm, native_find("is_digit"), 1, argv, &result));
    ASSERT_INT_EQ(VAL_BOOL, result.type);
    ASSERT_INT_EQ(1, result.as.as_int);
    value_release(argv[0]);
    value_release(result);

    argv[0] = value_string(strdup("a"));
    ASSERT_INT_EQ(1, native_call(vm, native_find("is_digit"), 1, argv, &result));
    ASSERT_INT_EQ(VAL_BOOL, result.type);
    ASSERT_INT_EQ(0, result.as.as_int);
    value_release(argv[0]);
    value_release(result);

    vm_free(vm);
}

TEST(natives_is_alpha_checks_single_character) {
    VM* vm = vm_init();
    Value argv[1];
    Value result;

    argv[0] = value_string(strdup("a"));
    ASSERT_INT_EQ(1, native_call(vm, native_find("is_alpha"), 1, argv, &result));
    ASSERT_INT_EQ(VAL_BOOL, result.type);
    ASSERT_INT_EQ(1, result.as.as_int);
    value_release(argv[0]);
    value_release(result);

    argv[0] = value_string(strdup("5"));
    ASSERT_INT_EQ(1, native_call(vm, native_find("is_alpha"), 1, argv, &result));
    ASSERT_INT_EQ(VAL_BOOL, result.type);
    ASSERT_INT_EQ(0, result.as.as_int);
    value_release(argv[0]);
    value_release(result);

    vm_free(vm);
}

int main(void) {
    RUN_TEST(natives_finds_registered_functions);
    RUN_TEST(natives_length_returns_array_length);
    RUN_TEST(natives_append_adds_element);
    RUN_TEST(natives_clock_returns_non_negative_int);
    RUN_TEST(natives_wrong_arity_fails);
    RUN_TEST(natives_abs_int_returns_absolute_value);
    RUN_TEST(natives_abs_int_rejects_float);
    RUN_TEST(natives_abs_float_returns_absolute_value);
    RUN_TEST(natives_abs_float_rejects_int);
    RUN_TEST(natives_abs_int_rejects_int_min);
    RUN_TEST(natives_min_int_returns_smaller);
    RUN_TEST(natives_max_int_returns_larger);
    RUN_TEST(natives_min_max_int_reject_float);
    RUN_TEST(natives_min_float_returns_smaller);
    RUN_TEST(natives_max_float_returns_larger);
    RUN_TEST(natives_min_max_float_reject_int);
    RUN_TEST(natives_range_returns_int_array);
    RUN_TEST(natives_assert_passes_on_true);
    RUN_TEST(natives_assert_fails_on_false);
    RUN_TEST(natives_parse_int_parses_valid);
    RUN_TEST(natives_parse_int_rejects_invalid);
    RUN_TEST(natives_split_lines_splits_by_newline);
    RUN_TEST(natives_split_lines_preserves_empty_lines);
    RUN_TEST(natives_join_paths_joins_two_strings);
    RUN_TEST(natives_join_paths_avoids_double_slash);
    RUN_TEST(natives_join_paths_respects_absolute_second);
    RUN_TEST(natives_format_replaces_placeholders);
    RUN_TEST(natives_format_rejects_mismatch);
    RUN_TEST(natives_sort_ints_ascending);
    RUN_TEST(natives_sort_strings_ascending);
    RUN_TEST(natives_reverse_array);
    RUN_TEST(natives_contains_finds_array_element);
    RUN_TEST(natives_contains_returns_false_for_missing_array_element);
    RUN_TEST(natives_index_of_returns_array_index);
    RUN_TEST(natives_index_of_returns_minus_one_for_missing_array_element);
    RUN_TEST(natives_slice_returns_subarray);
    RUN_TEST(natives_remove_at_returns_array_without_element);
    RUN_TEST(natives_remove_at_rejects_out_of_bounds);
    RUN_TEST(natives_clamp_int_within_range);
    RUN_TEST(natives_clamp_int_below_range);
    RUN_TEST(natives_clamp_float_coerces_ints);
    RUN_TEST(natives_clamp_rejects_string);
    RUN_TEST(natives_pow_returns_float);
    RUN_TEST(natives_sqrt_returns_float);
    RUN_TEST(natives_sqrt_rejects_negative);
    RUN_TEST(natives_round_returns_int);
    RUN_TEST(natives_floor_returns_int);
    RUN_TEST(natives_ceil_returns_int);
    RUN_TEST(natives_mod_returns_int);
    RUN_TEST(natives_mod_rejects_division_by_zero);
    RUN_TEST(natives_find_returns_index_of_existing_element);
    RUN_TEST(natives_find_returns_minus_one_for_missing_element);
    RUN_TEST(natives_insert_adds_element_at_index);
    RUN_TEST(natives_insert_rejects_out_of_bounds);
    RUN_TEST(natives_trim_start_removes_leading_whitespace);
    RUN_TEST(natives_trim_end_removes_trailing_whitespace);
    RUN_TEST(natives_trim_start_rejects_non_string);
    RUN_TEST(natives_env_get_returns_variable_value);
    RUN_TEST(natives_env_get_rejects_non_string);
    RUN_TEST(natives_sleep_returns_success);
    RUN_TEST(natives_random_int_returns_value_in_range);
    RUN_TEST(natives_random_int_rejects_inverted_range);
    RUN_TEST(natives_array_fill_creates_repeated_array);
    RUN_TEST(natives_array_fill_rejects_negative_count);
    RUN_TEST(natives_pad_start_pads_on_left);
    RUN_TEST(natives_pad_end_pads_on_right);
    RUN_TEST(natives_pad_start_rejects_non_string);
    RUN_TEST(natives_read_line_reads_from_stdin);
    RUN_TEST(natives_is_dir_detects_directory);
    RUN_TEST(natives_mkdir_creates_directory);
    RUN_TEST(natives_list_dir_lists_entries);
    RUN_TEST(natives_is_digit_checks_single_character);
    RUN_TEST(natives_is_alpha_checks_single_character);
    TEST_SUMMARY();
}
