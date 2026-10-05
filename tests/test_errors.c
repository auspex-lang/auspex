#include <stdio.h>
#include <string.h>

#include "test_harness.h"
#include "compiler.h"
#include "diagnostics.h"
#include "vm.h"
#include "parser.h"
#include "typecheck.h"
#include "ast.h"

TEST(parser_error_includes_location) {
    char error[256];
    Program* program = parse_with_path(
        "proc main() -> int { return 1; ", "parser.apx", error, sizeof(error));
    ASSERT_PTR_NULL(program);
    ASSERT_PTR_NOT_NULL(strstr(error, "parser.apx:1:"));
    ASSERT_PTR_NOT_NULL(strstr(error, ": error:"));
}

TEST(type_error_includes_location) {
    Chunk chunk;
    init_chunk(&chunk);
    char error[256];
    int ok = compile_with_path(
        "proc main() -> int {\n"
        "    int x = \"hello\";\n"
        "    return 0;\n"
        "}",
        &chunk, "typecheck.apx", error, sizeof(error));
    ASSERT_INT_EQ(0, ok);
    ASSERT_PTR_NOT_NULL(strstr(error, "typecheck.apx:2:"));
    ASSERT_PTR_NOT_NULL(strstr(error, ": error:"));
    ASSERT_PTR_NOT_NULL(strstr(error, "string"));
    free_chunk(&chunk);
}

TEST(compiler_error_includes_location) {
    Chunk chunk;
    init_chunk(&chunk);
    char error[256];
    int ok = compile_with_path(
        "proc main() -> int {\n"
        "    return undefined_var;\n"
        "}",
        &chunk, "compiler.apx", error, sizeof(error));
    ASSERT_INT_EQ(0, ok);
    ASSERT_PTR_NOT_NULL(strstr(error, "compiler.apx:2:"));
    ASSERT_PTR_NOT_NULL(strstr(error, ": error:"));
    ASSERT_PTR_NOT_NULL(strstr(error, "Undefined variable"));
    free_chunk(&chunk);
}

TEST(runtime_error_includes_location) {
    Chunk chunk;
    init_chunk(&chunk);
    char error[256];
    int ok = compile_with_path(
        "proc main() -> int {\n"
        "    int x = 0;\n"
        "    assert(x == 1, \"failed\");\n"
        "    return 0;\n"
        "}",
        &chunk, "runtime.apx", error, sizeof(error));
    ASSERT_INT_EQ(1, ok);

    VM* vm = vm_init();
    ASSERT_INT_EQ(INTERPRET_RUNTIME_ERROR, vm_interpret(vm, &chunk));
    const char* msg = vm_get_error(vm);
    ASSERT_PTR_NOT_NULL(msg);
    ASSERT_PTR_NOT_NULL(strstr(msg, "runtime.apx:3:"));
    ASSERT_PTR_NOT_NULL(strstr(msg, ": error:"));
    ASSERT_PTR_NOT_NULL(strstr(msg, "failed"));
    vm_free(vm);
    free_chunk(&chunk);
}

TEST(native_error_includes_location) {
    Chunk chunk;
    init_chunk(&chunk);
    char error[256];
    int ok = compile_with_path(
        "proc main() -> int {\n"
        "    int x = parse_int(\"not a number\");\n"
        "    return x;\n"
        "}",
        &chunk, "native.apx", error, sizeof(error));
    ASSERT_INT_EQ(1, ok);

    VM* vm = vm_init();
    ASSERT_INT_EQ(INTERPRET_RUNTIME_ERROR, vm_interpret(vm, &chunk));
    const char* msg = vm_get_error(vm);
    ASSERT_PTR_NOT_NULL(msg);
    ASSERT_PTR_NOT_NULL(strstr(msg, "native.apx:2:"));
    ASSERT_PTR_NOT_NULL(strstr(msg, ": error:"));
    ASSERT_PTR_NOT_NULL(strstr(msg, "parse_int: invalid integer"));
    vm_free(vm);
    free_chunk(&chunk);
}

TEST(error_format_without_path) {
    char error[256];
    Program* program = parse("proc main() -> int { return 1; ", error, sizeof(error));
    ASSERT_PTR_NULL(program);
    ASSERT_PTR_NOT_NULL(strstr(error, "1:"));
    ASSERT_PTR_NOT_NULL(strstr(error, ": error:"));
}

/* Issue #52: the driver prepends built-in and stored declarations to the
 * user's source, then tells the compiler how many lines that is
 * (CompileOptions.line_offset) so the user's own lines are numbered from 1. */

static int compile_after_preamble(const char* preamble, int preamble_lines, const char* user,
                                  const char* path, Chunk* chunk, char* error, size_t error_size) {
    char combined[2048];
    snprintf(combined, sizeof(combined), "%s%s", preamble, user);
    CompileOptions options = {NULL, 0, preamble_lines};
    return compile_with_options(combined, chunk, path, error, error_size, NULL, &options);
}

TEST(compile_error_line_is_relative_to_user_source_after_preamble) {
    Chunk chunk;
    init_chunk(&chunk);
    char error[256];
    int ok = compile_after_preamble(
        "// prepended 1\n// prepended 2\nproc helper() -> int { return 1; }\n\n", 4,
        "proc main() -> int {\n"
        "    int x = \"hello\";\n"
        "    return 0;\n"
        "}",
        "user.apx", &chunk, error, sizeof(error));
    ASSERT_INT_EQ(0, ok);
    ASSERT_PTR_NOT_NULL(strstr(error, "user.apx:2:"));
    free_chunk(&chunk);
}

TEST(compile_error_in_preamble_is_attributed_to_prepended_declarations) {
    Chunk chunk;
    init_chunk(&chunk);
    char error[256];
    int ok = compile_after_preamble(
        "proc bad() -> int { int y = \"s\"; return 0; }\n\n", 2,
        "proc main() -> int { return 0; }",
        "user.apx", &chunk, error, sizeof(error));
    ASSERT_INT_EQ(0, ok);
    ASSERT_PTR_NOT_NULL(strstr(error, "user.apx: error: in built-in or stored declarations"));
    free_chunk(&chunk);
}

TEST(runtime_error_line_is_relative_to_user_source_after_preamble) {
    Chunk chunk;
    init_chunk(&chunk);
    char error[256];
    int ok = compile_after_preamble(
        "proc helper() -> int { return 1; }\n\n", 2,
        "proc main() -> int {\n"
        "    int x = 0;\n"
        "    assert(x == 1, \"failed\");\n"
        "    return 0;\n"
        "}",
        "user.apx", &chunk, error, sizeof(error));
    ASSERT_INT_EQ(1, ok);

    VM* vm = vm_init();
    ASSERT_INT_EQ(INTERPRET_RUNTIME_ERROR, vm_interpret(vm, &chunk));
    const char* msg = vm_get_error(vm);
    ASSERT_PTR_NOT_NULL(msg);
    ASSERT_PTR_NOT_NULL(strstr(msg, "user.apx:3:"));
    vm_free(vm);
    free_chunk(&chunk);
}

TEST(runtime_error_inside_prepended_code_is_reported_at_the_call_site) {
    Chunk chunk;
    init_chunk(&chunk);
    char error[256];
    /* A prepended wrapper (like dbms_output.put_line) whose native fails. */
    int ok = compile_after_preamble(
        "proc wrapper(s string) -> int {\n"
        "    return parse_int(s);\n"
        "}\n\n", 4,
        "proc main() -> int {\n"
        "    int x = wrapper(\"nope\");\n"
        "    return x;\n"
        "}",
        "user.apx", &chunk, error, sizeof(error));
    ASSERT_INT_EQ(1, ok);

    VM* vm = vm_init();
    ASSERT_INT_EQ(INTERPRET_RUNTIME_ERROR, vm_interpret(vm, &chunk));
    const char* msg = vm_get_error(vm);
    ASSERT_PTR_NOT_NULL(msg);
    ASSERT_PTR_NOT_NULL(strstr(msg, "user.apx:2:"));
    ASSERT_PTR_NOT_NULL(strstr(msg, "parse_int: invalid integer"));
    vm_free(vm);
    free_chunk(&chunk);
}

TEST(multiline_select_error_is_reported_at_the_statement_start) {
    Chunk chunk;
    init_chunk(&chunk);
    char error[256];
    int ok = compile_with_path(
        "proc main() -> int {\n"
        "    int n = 0;\n"
        "    select id into n\n"
        "        from t\n"
        "        where id = 1;\n"
        "    return n;\n"
        "}",
        &chunk, "select.apx", error, sizeof(error));
    ASSERT_INT_EQ(1, ok);

    /* No database is attached, so the query fails where it is issued. */
    VM* vm = vm_init();
    ASSERT_INT_EQ(INTERPRET_RUNTIME_ERROR, vm_interpret(vm, &chunk));
    const char* msg = vm_get_error(vm);
    ASSERT_PTR_NOT_NULL(msg);
    ASSERT_PTR_NOT_NULL(strstr(msg, "select.apx:3:"));
    vm_free(vm);
    free_chunk(&chunk);
}

TEST(imported_module_is_numbered_from_its_own_first_line) {
    const char* module_path = "/tmp/auspex_test_errors_import_mod.apx";
    FILE* f = fopen(module_path, "w");
    ASSERT_PTR_NOT_NULL(f);
    fprintf(f, "// line 1\nproc broken() -> int { int y = \"s\"; return 0; }\n");
    fclose(f);

    char user[512];
    snprintf(user, sizeof(user),
             "import \"%s\";\nproc main() -> int { return 0; }\n", module_path);
    Chunk chunk;
    init_chunk(&chunk);
    char error[256];
    /* The main source has a 3-line preamble; the module must not inherit it. */
    int ok = compile_after_preamble("// a\n// b\n\n", 3, user, "user.apx",
                                    &chunk, error, sizeof(error));
    remove(module_path);
    ASSERT_INT_EQ(0, ok);
    ASSERT_PTR_NOT_NULL(strstr(error, "auspex_test_errors_import_mod.apx:2:"));
    free_chunk(&chunk);
}

TEST(conditional_compilation_error_line_is_relative_to_user_source) {
    Chunk chunk;
    init_chunk(&chunk);
    char error[256];
    int ok = compile_after_preamble(
        "// a\n// b\n\n", 3,
        "$end\nproc main() -> int { return 0; }",
        "user.apx", &chunk, error, sizeof(error));
    ASSERT_INT_EQ(0, ok);
    ASSERT_PTR_NOT_NULL(strstr(error, "user.apx:1:"));
    free_chunk(&chunk);
}

TEST(error_format_for_negative_line_names_prepended_declarations) {
    char buf[256];
    format_error(buf, sizeof(buf), "user.apx", -7, 3, "boom");
    ASSERT_PTR_NOT_NULL(strstr(buf, "user.apx: error: in built-in or stored declarations: boom"));
    ASSERT_PTR_NULL(strstr(buf, "-7"));
    format_error(buf, sizeof(buf), NULL, -1, 0, "boom");
    ASSERT_PTR_NOT_NULL(strstr(buf, "in built-in or stored declarations: boom"));
}

int main(void) {
    RUN_TEST(compile_error_line_is_relative_to_user_source_after_preamble);
    RUN_TEST(compile_error_in_preamble_is_attributed_to_prepended_declarations);
    RUN_TEST(runtime_error_line_is_relative_to_user_source_after_preamble);
    RUN_TEST(runtime_error_inside_prepended_code_is_reported_at_the_call_site);
    RUN_TEST(multiline_select_error_is_reported_at_the_statement_start);
    RUN_TEST(imported_module_is_numbered_from_its_own_first_line);
    RUN_TEST(conditional_compilation_error_line_is_relative_to_user_source);
    RUN_TEST(error_format_for_negative_line_names_prepended_declarations);
    RUN_TEST(parser_error_includes_location);
    RUN_TEST(type_error_includes_location);
    RUN_TEST(compiler_error_includes_location);
    RUN_TEST(runtime_error_includes_location);
    RUN_TEST(native_error_includes_location);
    RUN_TEST(error_format_without_path);
    TEST_SUMMARY();
}
