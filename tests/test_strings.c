#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_harness.h"
#include "compiler.h"
#include "natives.h"
#include "vm.h"

static int run_native_string(const char* name, int argc, Value* argv, Value* out) {
    VM* vm = vm_init();
    int ok = native_call(vm, native_find(name), argc, argv, out);
    if (!ok) {
        fprintf(stderr, "native error: %s\n", vm_get_error(vm));
    }
    vm_free(vm);
    return ok;
}

TEST(strings_value_add_concatenates) {
    Value a = value_string(strdup("hello"));
    Value b = value_string(strdup(" world"));
    Value result = value_add(a, b);
    ASSERT_INT_EQ(VAL_STRING, result.type);
    ASSERT_STRING_EQ("hello world", result.as.as_string);
}

TEST(strings_value_lt_compares_lexicographically) {
    Value a = value_string(strdup("apple"));
    Value b = value_string(strdup("banana"));
    ASSERT_INT_EQ(1, value_lt(a, b).as.as_int);
    ASSERT_INT_EQ(0, value_lt(b, a).as.as_int);
}

TEST(strings_value_gt_compares_lexicographically) {
    Value a = value_string(strdup("apple"));
    Value b = value_string(strdup("banana"));
    ASSERT_INT_EQ(0, value_gt(a, b).as.as_int);
    ASSERT_INT_EQ(1, value_gt(b, a).as.as_int);
}

TEST(natives_length_accepts_strings) {
    Value argv[1];
    argv[0] = value_string(strdup("hello"));
    Value out;
    ASSERT_INT_EQ(1, run_native_string("length", 1, argv, &out));
    ASSERT_INT_EQ(VAL_INT, out.type);
    ASSERT_INT_EQ(5, out.as.as_int);
}

TEST(natives_concat_returns_concatenated_string) {
    Value argv[2];
    argv[0] = value_string(strdup("Hello, "));
    argv[1] = value_string(strdup("world!"));
    Value out;
    ASSERT_INT_EQ(1, run_native_string("concat", 2, argv, &out));
    ASSERT_INT_EQ(VAL_STRING, out.type);
    ASSERT_STRING_EQ("Hello, world!", out.as.as_string);
}

TEST(natives_concat_rejects_non_strings) {
    Value argv[2];
    argv[0] = value_string(strdup("x"));
    argv[1] = value_int(1);
    Value out;
    ASSERT_INT_EQ(0, run_native_string("concat", 2, argv, &out));
}

TEST(natives_substring_extracts_substring) {
    Value argv[3];
    argv[0] = value_string(strdup("hello"));
    argv[1] = value_int(1);
    argv[2] = value_int(3);
    Value out;
    ASSERT_INT_EQ(1, run_native_string("substring", 3, argv, &out));
    ASSERT_STRING_EQ("ell", out.as.as_string);
}

TEST(natives_substring_clamps_out_of_range) {
    Value argv[3];
    argv[0] = value_string(strdup("hi"));
    argv[1] = value_int(1);
    argv[2] = value_int(100);
    Value out;
    ASSERT_INT_EQ(1, run_native_string("substring", 3, argv, &out));
    ASSERT_STRING_EQ("i", out.as.as_string);
}

TEST(natives_contains_finds_substring) {
    Value argv[2];
    argv[0] = value_string(strdup("hello world"));
    argv[1] = value_string(strdup("world"));
    Value out;
    ASSERT_INT_EQ(1, run_native_string("contains", 2, argv, &out));
    ASSERT_INT_EQ(1, out.as.as_int);
}

TEST(natives_contains_returns_false_when_missing) {
    Value argv[2];
    argv[0] = value_string(strdup("hello"));
    argv[1] = value_string(strdup("x"));
    Value out;
    ASSERT_INT_EQ(1, run_native_string("contains", 2, argv, &out));
    ASSERT_INT_EQ(0, out.as.as_int);
}

TEST(natives_index_of_returns_first_index) {
    Value argv[2];
    argv[0] = value_string(strdup("banana"));
    argv[1] = value_string(strdup("na"));
    Value out;
    ASSERT_INT_EQ(1, run_native_string("index_of", 2, argv, &out));
    ASSERT_INT_EQ(2, out.as.as_int);
}

TEST(natives_index_of_returns_minus_one_when_missing) {
    Value argv[2];
    argv[0] = value_string(strdup("abc"));
    argv[1] = value_string(strdup("z"));
    Value out;
    ASSERT_INT_EQ(1, run_native_string("index_of", 2, argv, &out));
    ASSERT_INT_EQ(-1, out.as.as_int);
}

TEST(natives_to_upper_converts_ascii) {
    Value argv[1];
    argv[0] = value_string(strdup("Hello"));
    Value out;
    ASSERT_INT_EQ(1, run_native_string("to_upper", 1, argv, &out));
    ASSERT_STRING_EQ("HELLO", out.as.as_string);
}

TEST(natives_to_lower_converts_ascii) {
    Value argv[1];
    argv[0] = value_string(strdup("Hello"));
    Value out;
    ASSERT_INT_EQ(1, run_native_string("to_lower", 1, argv, &out));
    ASSERT_STRING_EQ("hello", out.as.as_string);
}

TEST(natives_trim_strips_whitespace) {
    Value argv[1];
    argv[0] = value_string(strdup("  hello world  "));
    Value out;
    ASSERT_INT_EQ(1, run_native_string("trim", 1, argv, &out));
    ASSERT_STRING_EQ("hello world", out.as.as_string);
}

TEST(natives_int_to_string_formats_decimal) {
    Value argv[1];
    argv[0] = value_int(-42);
    Value out;
    ASSERT_INT_EQ(1, run_native_string("int_to_string", 1, argv, &out));
    ASSERT_STRING_EQ("-42", out.as.as_string);
}

TEST(natives_float_to_string_formats_g) {
    Value argv[1];
    argv[0] = value_float(3.5);
    Value out;
    ASSERT_INT_EQ(1, run_native_string("float_to_string", 1, argv, &out));
    ASSERT_STRING_EQ("3.5", out.as.as_string);
}

TEST(natives_contains_rejects_non_strings) {
    Value argv[2];
    argv[0] = value_int(1);
    argv[1] = value_string(strdup("x"));
    Value out;
    ASSERT_INT_EQ(0, run_native_string("contains", 2, argv, &out));
}

TEST(natives_index_of_rejects_non_strings) {
    Value argv[2];
    argv[0] = value_string(strdup("x"));
    argv[1] = value_float(1.5);
    Value out;
    ASSERT_INT_EQ(0, run_native_string("index_of", 2, argv, &out));
}

TEST(natives_to_upper_rejects_non_string) {
    Value argv[1];
    argv[0] = value_int(42);
    Value out;
    ASSERT_INT_EQ(0, run_native_string("to_upper", 1, argv, &out));
}

TEST(natives_to_lower_rejects_non_string) {
    Value argv[1];
    argv[0] = value_float(1.5);
    Value out;
    ASSERT_INT_EQ(0, run_native_string("to_lower", 1, argv, &out));
}

TEST(natives_trim_rejects_non_string) {
    Value argv[1];
    argv[0] = value_int(0);
    Value out;
    ASSERT_INT_EQ(0, run_native_string("trim", 1, argv, &out));
}

TEST(natives_int_to_string_rejects_non_int) {
    Value argv[1];
    argv[0] = value_string(strdup("42"));
    Value out;
    ASSERT_INT_EQ(0, run_native_string("int_to_string", 1, argv, &out));
}

TEST(natives_float_to_string_rejects_non_float) {
    Value argv[1];
    argv[0] = value_int(42);
    Value out;
    ASSERT_INT_EQ(0, run_native_string("float_to_string", 1, argv, &out));
}

TEST(natives_split_splits_by_delimiter) {
    VM* vm = vm_init();
    Value argv[2];
    argv[0] = value_string(strdup("a,b,c"));
    argv[1] = value_string(strdup(","));
    Value out;
    ASSERT_INT_EQ(1, native_call(vm, native_find("split"), 2, argv, &out));
    ASSERT_INT_EQ(VAL_ARRAY, out.type);
    ASSERT_INT_EQ(3, array_length(out.as.as_array));
    ASSERT_STRING_EQ("a", array_get(out.as.as_array, 0).as.as_string);
    ASSERT_STRING_EQ("b", array_get(out.as.as_array, 1).as.as_string);
    ASSERT_STRING_EQ("c", array_get(out.as.as_array, 2).as.as_string);
    vm_free(vm);
}

TEST(natives_split_returns_single_part_when_delimiter_missing) {
    VM* vm = vm_init();
    Value argv[2];
    argv[0] = value_string(strdup("abc"));
    argv[1] = value_string(strdup(","));
    Value out;
    ASSERT_INT_EQ(1, native_call(vm, native_find("split"), 2, argv, &out));
    ASSERT_INT_EQ(VAL_ARRAY, out.type);
    ASSERT_INT_EQ(1, array_length(out.as.as_array));
    ASSERT_STRING_EQ("abc", array_get(out.as.as_array, 0).as.as_string);
    vm_free(vm);
}

TEST(natives_split_rejects_empty_delimiter) {
    VM* vm = vm_init();
    Value argv[2];
    argv[0] = value_string(strdup("abc"));
    argv[1] = value_string(strdup(""));
    Value out;
    ASSERT_INT_EQ(0, native_call(vm, native_find("split"), 2, argv, &out));
    ASSERT_PTR_NOT_NULL(vm_get_error(vm));
    vm_free(vm);
}

TEST(natives_split_rejects_non_string_arguments) {
    VM* vm = vm_init();
    Value argv[2];
    argv[0] = value_int(1);
    argv[1] = value_string(strdup(","));
    Value out;
    ASSERT_INT_EQ(0, native_call(vm, native_find("split"), 2, argv, &out));
    ASSERT_PTR_NOT_NULL(vm_get_error(vm));
    vm_free(vm);
}

TEST(natives_join_combines_parts) {
    VM* vm = vm_init();
    ArrayObj* arr = array_new();
    array_append(arr, value_string(strdup("a")));
    array_append(arr, value_string(strdup("b")));
    array_append(arr, value_string(strdup("c")));
    Value argv[2];
    argv[0] = value_array(arr);
    argv[1] = value_string(strdup("-"));
    Value out;
    ASSERT_INT_EQ(1, native_call(vm, native_find("join"), 2, argv, &out));
    ASSERT_INT_EQ(VAL_STRING, out.type);
    ASSERT_STRING_EQ("a-b-c", out.as.as_string);
    vm_free(vm);
}

TEST(natives_join_rejects_non_array) {
    VM* vm = vm_init();
    Value argv[2];
    argv[0] = value_int(1);
    argv[1] = value_string(strdup("-"));
    Value out;
    ASSERT_INT_EQ(0, native_call(vm, native_find("join"), 2, argv, &out));
    ASSERT_PTR_NOT_NULL(vm_get_error(vm));
    vm_free(vm);
}

TEST(natives_join_rejects_non_string_element) {
    VM* vm = vm_init();
    ArrayObj* arr = array_new();
    array_append(arr, value_int(1));
    Value argv[2];
    argv[0] = value_array(arr);
    argv[1] = value_string(strdup("-"));
    Value out;
    ASSERT_INT_EQ(0, native_call(vm, native_find("join"), 2, argv, &out));
    ASSERT_PTR_NOT_NULL(vm_get_error(vm));
    vm_free(vm);
}

TEST(natives_replace_replaces_all_occurrences) {
    Value argv[3];
    argv[0] = value_string(strdup("hello world world"));
    argv[1] = value_string(strdup("world"));
    argv[2] = value_string(strdup("auspex"));
    Value out;
    ASSERT_INT_EQ(1, run_native_string("replace", 3, argv, &out));
    ASSERT_STRING_EQ("hello auspex auspex", out.as.as_string);
}

TEST(natives_replace_rejects_non_strings) {
    Value argv[3];
    argv[0] = value_string(strdup("x"));
    argv[1] = value_int(1);
    argv[2] = value_string(strdup("y"));
    Value out;
    ASSERT_INT_EQ(0, run_native_string("replace", 3, argv, &out));
}

TEST(natives_repeat_repeats_string) {
    Value argv[2];
    argv[0] = value_string(strdup("ab"));
    argv[1] = value_int(3);
    Value out;
    ASSERT_INT_EQ(1, run_native_string("repeat", 2, argv, &out));
    ASSERT_STRING_EQ("ababab", out.as.as_string);
}

TEST(natives_repeat_returns_empty_for_zero_count) {
    Value argv[2];
    argv[0] = value_string(strdup("x"));
    argv[1] = value_int(0);
    Value out;
    ASSERT_INT_EQ(1, run_native_string("repeat", 2, argv, &out));
    ASSERT_STRING_EQ("", out.as.as_string);
}

TEST(natives_repeat_rejects_non_string) {
    Value argv[2];
    argv[0] = value_int(1);
    argv[1] = value_int(3);
    Value out;
    ASSERT_INT_EQ(0, run_native_string("repeat", 2, argv, &out));
}

TEST(natives_repeat_rejects_non_int_count) {
    Value argv[2];
    argv[0] = value_string(strdup("x"));
    argv[1] = value_float(3.0);
    Value out;
    ASSERT_INT_EQ(0, run_native_string("repeat", 2, argv, &out));
}

TEST(natives_starts_with_returns_true_for_prefix) {
    Value argv[2];
    argv[0] = value_string(strdup("hello world"));
    argv[1] = value_string(strdup("hello"));
    Value out;
    ASSERT_INT_EQ(1, run_native_string("starts_with", 2, argv, &out));
    ASSERT_INT_EQ(VAL_BOOL, out.type);
    ASSERT_INT_EQ(1, out.as.as_int);
}

TEST(natives_starts_with_returns_false_for_non_prefix) {
    Value argv[2];
    argv[0] = value_string(strdup("hello world"));
    argv[1] = value_string(strdup("world"));
    Value out;
    ASSERT_INT_EQ(1, run_native_string("starts_with", 2, argv, &out));
    ASSERT_INT_EQ(VAL_BOOL, out.type);
    ASSERT_INT_EQ(0, out.as.as_int);
}

TEST(natives_ends_with_returns_true_for_suffix) {
    Value argv[2];
    argv[0] = value_string(strdup("hello world"));
    argv[1] = value_string(strdup("world"));
    Value out;
    ASSERT_INT_EQ(1, run_native_string("ends_with", 2, argv, &out));
    ASSERT_INT_EQ(VAL_BOOL, out.type);
    ASSERT_INT_EQ(1, out.as.as_int);
}

TEST(natives_ends_with_returns_false_for_non_suffix) {
    Value argv[2];
    argv[0] = value_string(strdup("hello world"));
    argv[1] = value_string(strdup("hello"));
    Value out;
    ASSERT_INT_EQ(1, run_native_string("ends_with", 2, argv, &out));
    ASSERT_INT_EQ(VAL_BOOL, out.type);
    ASSERT_INT_EQ(0, out.as.as_int);
}

TEST(natives_char_at_returns_character) {
    Value argv[2];
    argv[0] = value_string(strdup("hello"));
    argv[1] = value_int(1);
    Value out;
    ASSERT_INT_EQ(1, run_native_string("char_at", 2, argv, &out));
    ASSERT_INT_EQ(VAL_STRING, out.type);
    ASSERT_STRING_EQ("e", out.as.as_string);
}

TEST(natives_char_at_rejects_out_of_bounds) {
    Value argv[2];
    argv[0] = value_string(strdup("hello"));
    argv[1] = value_int(10);
    Value out;
    ASSERT_INT_EQ(0, run_native_string("char_at", 2, argv, &out));
}

TEST(natives_reverse_string_reverses) {
    Value argv[1];
    argv[0] = value_string(strdup("hello"));
    Value out;
    ASSERT_INT_EQ(1, run_native_string("reverse_string", 1, argv, &out));
    ASSERT_INT_EQ(VAL_STRING, out.type);
    ASSERT_STRING_EQ("olleh", out.as.as_string);
}

TEST(natives_reverse_string_rejects_non_string) {
    Value argv[1];
    argv[0] = value_int(1);
    Value out;
    ASSERT_INT_EQ(0, run_native_string("reverse_string", 1, argv, &out));
}

int main(void) {
    RUN_TEST(strings_value_add_concatenates);
    RUN_TEST(strings_value_lt_compares_lexicographically);
    RUN_TEST(strings_value_gt_compares_lexicographically);
    RUN_TEST(natives_length_accepts_strings);
    RUN_TEST(natives_concat_returns_concatenated_string);
    RUN_TEST(natives_concat_rejects_non_strings);
    RUN_TEST(natives_substring_extracts_substring);
    RUN_TEST(natives_substring_clamps_out_of_range);
    RUN_TEST(natives_contains_finds_substring);
    RUN_TEST(natives_contains_returns_false_when_missing);
    RUN_TEST(natives_index_of_returns_first_index);
    RUN_TEST(natives_index_of_returns_minus_one_when_missing);
    RUN_TEST(natives_to_upper_converts_ascii);
    RUN_TEST(natives_to_lower_converts_ascii);
    RUN_TEST(natives_trim_strips_whitespace);
    RUN_TEST(natives_int_to_string_formats_decimal);
    RUN_TEST(natives_float_to_string_formats_g);
    RUN_TEST(natives_contains_rejects_non_strings);
    RUN_TEST(natives_index_of_rejects_non_strings);
    RUN_TEST(natives_to_upper_rejects_non_string);
    RUN_TEST(natives_to_lower_rejects_non_string);
    RUN_TEST(natives_trim_rejects_non_string);
    RUN_TEST(natives_int_to_string_rejects_non_int);
    RUN_TEST(natives_float_to_string_rejects_non_float);
    RUN_TEST(natives_split_splits_by_delimiter);
    RUN_TEST(natives_split_returns_single_part_when_delimiter_missing);
    RUN_TEST(natives_split_rejects_empty_delimiter);
    RUN_TEST(natives_split_rejects_non_string_arguments);
    RUN_TEST(natives_join_combines_parts);
    RUN_TEST(natives_join_rejects_non_array);
    RUN_TEST(natives_join_rejects_non_string_element);
    RUN_TEST(natives_replace_replaces_all_occurrences);
    RUN_TEST(natives_replace_rejects_non_strings);
    RUN_TEST(natives_repeat_repeats_string);
    RUN_TEST(natives_repeat_returns_empty_for_zero_count);
    RUN_TEST(natives_repeat_rejects_non_string);
    RUN_TEST(natives_repeat_rejects_non_int_count);
    RUN_TEST(natives_starts_with_returns_true_for_prefix);
    RUN_TEST(natives_starts_with_returns_false_for_non_prefix);
    RUN_TEST(natives_ends_with_returns_true_for_suffix);
    RUN_TEST(natives_ends_with_returns_false_for_non_suffix);
    RUN_TEST(natives_char_at_returns_character);
    RUN_TEST(natives_char_at_rejects_out_of_bounds);
    RUN_TEST(natives_reverse_string_reverses);
    RUN_TEST(natives_reverse_string_rejects_non_string);
    TEST_SUMMARY();
}
