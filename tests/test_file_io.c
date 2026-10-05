#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "test_harness.h"
#include "compiler.h"
#include "natives.h"
#include "vm.h"

#define TEST_PATH "/tmp/auspex_test_file_io.txt"

TEST(file_io_write_file_returns_success_and_creates_file) {
    unlink(TEST_PATH);

    VM* vm = vm_init();
    Value argv[2];
    argv[0] = value_string(strdup(TEST_PATH));
    argv[1] = value_string(strdup("hello file"));
    Value result;

    int idx = native_find("write_file");
    ASSERT_INT_EQ(1, idx >= 0);
    ASSERT_INT_EQ(1, native_call(vm, idx, 2, argv, &result));
    ASSERT_INT_EQ(VAL_INT, result.type);
    ASSERT_INT_EQ(1, result.as.as_int);

    vm_free(vm);
    unlink(TEST_PATH);
}

TEST(file_io_read_file_returns_written_contents) {
    unlink(TEST_PATH);

    VM* vm = vm_init();
    Value write_argv[2];
    write_argv[0] = value_string(strdup(TEST_PATH));
    write_argv[1] = value_string(strdup("roundtrip contents"));
    Value write_result;
    ASSERT_INT_EQ(1, native_call(vm, native_find("write_file"), 2, write_argv, &write_result));

    Value read_argv[1];
    read_argv[0] = value_string(strdup(TEST_PATH));
    Value read_result;
    int read_idx = native_find("read_file");
    ASSERT_INT_EQ(1, read_idx >= 0);
    ASSERT_INT_EQ(1, native_call(vm, read_idx, 1, read_argv, &read_result));
    ASSERT_INT_EQ(VAL_STRING, read_result.type);
    ASSERT_STRING_EQ("roundtrip contents", read_result.as.as_string);

    vm_free(vm);
    unlink(TEST_PATH);
}

TEST(file_io_read_missing_file_fails) {
    unlink(TEST_PATH);

    VM* vm = vm_init();
    Value argv[1];
    argv[0] = value_string(strdup(TEST_PATH));
    Value result;
    ASSERT_INT_EQ(0, native_call(vm, native_find("read_file"), 1, argv, &result));
    ASSERT_PTR_NOT_NULL(vm_get_error(vm));

    vm_free(vm);
}

TEST(file_io_file_exists_reports_presence) {
    unlink(TEST_PATH);

    VM* vm = vm_init();
    Value argv[1];
    argv[0] = value_string(strdup(TEST_PATH));
    Value result;
    int idx = native_find("file_exists");
    ASSERT_INT_EQ(1, idx >= 0);

    ASSERT_INT_EQ(1, native_call(vm, idx, 1, argv, &result));
    ASSERT_INT_EQ(VAL_BOOL, result.type);
    ASSERT_INT_EQ(0, result.as.as_int);

    Value write_argv[2];
    write_argv[0] = value_string(strdup(TEST_PATH));
    write_argv[1] = value_string(strdup("x"));
    Value write_result;
    ASSERT_INT_EQ(1, native_call(vm, native_find("write_file"), 2, write_argv, &write_result));

    ASSERT_INT_EQ(1, native_call(vm, idx, 1, argv, &result));
    ASSERT_INT_EQ(VAL_BOOL, result.type);
    ASSERT_INT_EQ(1, result.as.as_int);

    vm_free(vm);
    unlink(TEST_PATH);
}

TEST(file_io_write_file_rejects_non_string_path) {
    VM* vm = vm_init();
    Value argv[2];
    argv[0] = value_int(1);
    argv[1] = value_string(strdup("contents"));
    Value result;
    ASSERT_INT_EQ(0, native_call(vm, native_find("write_file"), 2, argv, &result));
    ASSERT_PTR_NOT_NULL(vm_get_error(vm));
    vm_free(vm);
}

TEST(file_io_write_file_rejects_non_string_contents) {
    VM* vm = vm_init();
    Value argv[2];
    argv[0] = value_string(strdup(TEST_PATH));
    argv[1] = value_int(1);
    Value result;
    ASSERT_INT_EQ(0, native_call(vm, native_find("write_file"), 2, argv, &result));
    ASSERT_PTR_NOT_NULL(vm_get_error(vm));
    vm_free(vm);
}

TEST(file_io_read_file_rejects_non_string_path) {
    VM* vm = vm_init();
    Value argv[1];
    argv[0] = value_int(1);
    Value result;
    ASSERT_INT_EQ(0, native_call(vm, native_find("read_file"), 1, argv, &result));
    ASSERT_PTR_NOT_NULL(vm_get_error(vm));
    vm_free(vm);
}

TEST(file_io_file_exists_rejects_non_string_path) {
    VM* vm = vm_init();
    Value argv[1];
    argv[0] = value_int(1);
    Value result;
    ASSERT_INT_EQ(0, native_call(vm, native_find("file_exists"), 1, argv, &result));
    ASSERT_PTR_NOT_NULL(vm_get_error(vm));
    vm_free(vm);
}

int main(void) {
    RUN_TEST(file_io_write_file_returns_success_and_creates_file);
    RUN_TEST(file_io_read_file_returns_written_contents);
    RUN_TEST(file_io_read_missing_file_fails);
    RUN_TEST(file_io_file_exists_reports_presence);
    RUN_TEST(file_io_write_file_rejects_non_string_path);
    RUN_TEST(file_io_write_file_rejects_non_string_contents);
    RUN_TEST(file_io_read_file_rejects_non_string_path);
    RUN_TEST(file_io_file_exists_rejects_non_string_path);
    TEST_SUMMARY();
}
