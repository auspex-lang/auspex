#include <stdlib.h>
#include <unistd.h>

#include "test_harness.h"
#include "parser.h"
#include "sql_engine.h"
#include "typecheck.h"

TEST(typecheck_rejects_string_to_int_assignment) {
    char error[256];
    Program* program = parse("proc main() -> int { int x = \"hello\"; return 0; }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(0, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_accepts_valid_program) {
    char error[256];
    Program* program = parse("proc main() -> int { int x = 1 + 2; return x; }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(1, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_rejects_typed_array_mismatch) {
    char error[256];
    Program* program = parse("proc main() -> int { array<int> a = [1, \"two\"]; return 0; }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(0, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_accepts_int_float_coercion) {
    char error[256];
    Program* program = parse("proc f(x float) -> float { return x + 1; } proc main() -> int { return 0; }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(1, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_rejects_bool_mismatch) {
    char error[256];
    Program* program = parse("proc main() -> int { bool b = 1; return 0; }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(0, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_accepts_unary_operators) {
    char error[256];
    Program* program = parse("proc main() -> int { int a = -5; bool b = !true; return a; }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(1, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_rejects_bad_unary) {
    char error[256];
    Program* program = parse("proc main() -> int { int a = -\"hello\"; return 0; }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(0, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_accepts_comparisons) {
    char error[256];
    Program* program = parse("proc main() -> int { bool a = 1 < 2; bool b = \"x\" == \"y\"; return 0; }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(1, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_rejects_mixed_comparison) {
    char error[256];
    Program* program = parse("proc main() -> int { bool a = 1 < \"x\"; return 0; }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(0, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_accepts_indexing_and_index_assign) {
    char error[256];
    Program* program = parse("proc main() -> int { array<int> a = [1, 2]; int x = a[0]; a[1] = 3; return x; }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(1, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_rejects_indexing_non_int_index) {
    char error[256];
    Program* program = parse("proc main() -> int { array<int> a = [1, 2]; int x = a[\"x\"]; return 0; }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(0, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_rejects_if_string_condition) {
    char error[256];
    Program* program = parse("proc main() -> int { if \"bad\" { } return 0; }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(0, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_rejects_narrowing_float_to_int) {
    char error[256];
    Program* program = parse("proc main() -> int { int x = 1.5; return 0; }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(0, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_rejects_mixed_int_float_array) {
    char error[256];
    Program* program = parse("proc main() -> int { array<int> a = [1, 2.5]; return 0; }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(0, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_rejects_out_of_scope_block_variable) {
    char error[256];
    Program* program = parse("proc main() -> int { if true { int x = 1; } return x; }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(0, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_accepts_external_proc_signature_arguments) {
    char error[256];
    Type* params[] = { &type_int, &type_int };
    ProcSignature sig = { .name = "add", .return_type = &type_int, .param_types = params, .param_count = 2 };
    ProcSignature procs[] = { sig };
    Program* program = parse("proc main() -> int { int x = add(1, 2); return x; }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(1, typecheck_program(program, procs, 1, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_accepts_for_row_in_select_loop) {
    char error[256];
    Program* program = parse("proc main() -> int { for row in SELECT id FROM users { return 0; } return 0; }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(1, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_accepts_mixed_int_float_comparison) {
    char error[256];
    Program* program = parse("proc main() -> int { bool b = 1 < 2.5; return 0; }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(1, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_accepts_int_to_float_assignment) {
    char error[256];
    Program* program = parse("proc main() -> int { float x = 1; return 0; }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(1, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_accepts_int_to_float_return) {
    char error[256];
    Program* program = parse("proc f() -> float { return 1; } proc main() -> int { return 0; }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(1, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_accepts_row_field_return) {
    char error[256];
    Program* program = parse("proc main() -> int { for row in SELECT id FROM users { return row.id; } return 0; }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(1, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_rejects_undefined_variable_assignment) {
    char error[256];
    Program* program = parse("proc main() -> int { x = 1; return 0; }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(0, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_accepts_intra_program_call) {
    char error[256];
    Program* program = parse(
        "proc add(a int, b int) -> int { return a + b; } "
        "proc main() -> int { int x = add(1, 2); return x; }",
        error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(1, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_rejects_undefined_procedure_call) {
    char error[256];
    Program* program = parse("proc main() -> int { foo(); return 0; }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(0, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_accepts_uninitialized_variable) {
    char error[256];
    Program* program = parse("proc main() -> int { int x; return 0; }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(1, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_rejects_intra_program_wrong_arg_count) {
    char error[256];
    Program* program = parse(
        "proc add(a int, b int) -> int { return a + b; } "
        "proc main() -> int { int x = add(1); return x; }",
        error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(0, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_rejects_intra_program_wrong_arg_type) {
    char error[256];
    Program* program = parse(
        "proc add(a int, b int) -> int { return a + b; } "
        "proc main() -> int { int x = add(1, \"two\"); return x; }",
        error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(0, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_accepts_length_on_array) {
    char error[256];
    Program* program = parse("proc main() -> int { array<int> a = [1, 2]; return length(a); }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(1, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_rejects_length_on_int) {
    char error[256];
    Program* program = parse("proc main() -> int { return length(42); }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(0, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_accepts_append_matching_type) {
    char error[256];
    Program* program = parse("proc main() -> int { array<int> a = [1]; a = append(a, 2); return length(a); }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(1, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_rejects_append_mismatch) {
    char error[256];
    Program* program = parse("proc main() -> int { array<int> a = [1]; a = append(a, \"x\"); return 0; }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(0, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_accepts_println) {
    char error[256];
    Program* program = parse("proc main() -> int { println(42); return 0; }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(1, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_accepts_clock) {
    char error[256];
    Program* program = parse("proc main() -> int { int t = clock(); return 0; }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(1, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_rejects_clock_with_args) {
    char error[256];
    Program* program = parse("proc main() -> int { clock(1); return 0; }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(0, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_accepts_empty_array_return_with_hint) {
    char error[256];
    Program* program = parse("proc make() -> array<int> { return []; } proc main() -> int { return 0; }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(1, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_resolves_sql_row_field_type) {
    char db_path[] = "/tmp/auspex_test_typecheck_sql_XXXXXX.db";
    int fd = mkstemp(db_path);
    if (fd >= 0) close(fd);
    unlink(db_path);

    Context ctx;
    ctx.db_path = db_path;
    ctx.pager = NULL;
    ASSERT_INT_EQ(1, catalog_open(&ctx));
    const char* cols[] = {"id"};
    int types[] = {VAL_INT};
    Table* t = catalog_create_table(&ctx, "users", cols, types, 1);
    ASSERT_PTR_NOT_NULL(t);

    char error[256];
    Program* program = parse(
        "proc main() -> int { for row in SELECT id FROM users { return row.id; } return 0; }",
        error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(1, typecheck_program(program, NULL, 0, &ctx, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);

    catalog_close(&ctx);
    unlink(db_path);
}

TEST(typecheck_rejects_sql_row_field_type_mismatch) {
    char db_path[] = "/tmp/auspex_test_typecheck_sql2_XXXXXX.db";
    int fd = mkstemp(db_path);
    if (fd >= 0) close(fd);
    unlink(db_path);

    Context ctx;
    ctx.db_path = db_path;
    ctx.pager = NULL;
    ASSERT_INT_EQ(1, catalog_open(&ctx));
    const char* cols[] = {"name"};
    int types[] = {VAL_STRING};
    Table* t = catalog_create_table(&ctx, "users", cols, types, 1);
    ASSERT_PTR_NOT_NULL(t);

    char error[256];
    Program* program = parse(
        "proc main() -> int { for row in SELECT name FROM users { int x = row.name; return x; } return 0; }",
        error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(0, typecheck_program(program, NULL, 0, &ctx, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);

    catalog_close(&ctx);
    unlink(db_path);
}

TEST(typecheck_rejects_row_field_not_selected) {
    char db_path[] = "/tmp/auspex_test_typecheck_sql3_XXXXXX.db";
    int fd = mkstemp(db_path);
    if (fd >= 0) close(fd);
    unlink(db_path);

    Context ctx;
    ctx.db_path = db_path;
    ctx.pager = NULL;
    ASSERT_INT_EQ(1, catalog_open(&ctx));
    const char* cols[] = {"id", "name"};
    int types[] = {VAL_INT, VAL_STRING};
    Table* t = catalog_create_table(&ctx, "users", cols, types, 2);
    ASSERT_PTR_NOT_NULL(t);

    char error[256];
    Program* program = parse(
        "proc main() -> int { for row in SELECT name FROM users { return row.id; } return 0; }",
        error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(0, typecheck_program(program, NULL, 0, &ctx, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);

    catalog_close(&ctx);
    unlink(db_path);
}

TEST(typecheck_accepts_row_field_with_select_star) {
    char db_path[] = "/tmp/auspex_test_typecheck_sql4_XXXXXX.db";
    int fd = mkstemp(db_path);
    if (fd >= 0) close(fd);
    unlink(db_path);

    Context ctx;
    ctx.db_path = db_path;
    ctx.pager = NULL;
    ASSERT_INT_EQ(1, catalog_open(&ctx));
    const char* cols[] = {"id", "name"};
    int types[] = {VAL_INT, VAL_STRING};
    Table* t = catalog_create_table(&ctx, "users", cols, types, 2);
    ASSERT_PTR_NOT_NULL(t);

    char error[256];
    Program* program = parse(
        "proc main() -> int { for row in SELECT * FROM users { return row.id; } return 0; }",
        error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(1, typecheck_program(program, NULL, 0, &ctx, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);

    catalog_close(&ctx);
    unlink(db_path);
}

TEST(typecheck_rejects_field_on_undefined_variable) {
    char error[256];
    Program* program = parse("proc main() -> int { return row.id; }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(0, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

/* The loop variable of `for x in select` is a row whatever it is named;
 * only `row` used to be accepted (examples/inventory.apx uses `p`). */
TEST(typecheck_accepts_named_row_variable_field) {
    char error[256];
    Program* program = parse(
        "proc main() -> int { for p in SELECT id FROM users { return p.id; } return 0; }",
        error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(1, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_resolves_named_row_variable_field_type) {
    char db_path[] = "/tmp/auspex_test_typecheck_sql5_XXXXXX.db";
    int fd = mkstemp(db_path);
    if (fd >= 0) close(fd);
    unlink(db_path);

    Context ctx;
    ctx.db_path = db_path;
    ctx.pager = NULL;
    ASSERT_INT_EQ(1, catalog_open(&ctx));
    const char* cols[] = {"id", "name"};
    int types[] = {VAL_INT, VAL_STRING};
    Table* t = catalog_create_table(&ctx, "users", cols, types, 2);
    ASSERT_PTR_NOT_NULL(t);

    char error[256];
    Program* program = parse(
        "proc main() -> int { for p in SELECT id, name FROM users { return p.id; } return 0; }",
        error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(1, typecheck_program(program, NULL, 0, &ctx, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);

    program = parse(
        "proc main() -> int { for p in SELECT name FROM users { int x = p.name; return x; } return 0; }",
        error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(0, typecheck_program(program, NULL, 0, &ctx, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);

    program = parse(
        "proc main() -> int { for p in SELECT name FROM users { return p.id; } return 0; }",
        error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(0, typecheck_program(program, NULL, 0, &ctx, NULL, error, sizeof(error), NULL, NULL, 0));
    ASSERT_PTR_NOT_NULL(strstr(error, "Unknown column 'id' for row variable 'p'"));
    free_program(program);

    catalog_close(&ctx);
    unlink(db_path);
}

TEST(typecheck_rejects_field_on_non_row_variable) {
    char error[256];
    Program* program = parse("proc main() -> int { int x = 1; return x.y; }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(0, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_accepts_string_concat) {
    char error[256];
    Program* program = parse("proc main() -> string { string s = \"a\" + \"b\"; return s; }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(1, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_rejects_string_plus_int) {
    char error[256];
    Program* program = parse("proc main() -> string { string s = \"a\" + 1; return s; }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(0, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_accepts_string_comparison) {
    char error[256];
    Program* program = parse("proc main() -> bool { bool b = \"a\" < \"b\"; return b; }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(1, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_accepts_length_on_string) {
    char error[256];
    Program* program = parse("proc main() -> int { int n = length(\"hello\"); return n; }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(1, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_rejects_concat_with_int) {
    char error[256];
    Program* program = parse("proc main() -> string { string s = concat(\"a\", 1); return s; }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(0, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_accepts_abs_int) {
    char error[256];
    Program* program = parse("proc main() -> int { return abs_int(-5); }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(1, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_rejects_abs_int_with_float) {
    char error[256];
    Program* program = parse("proc main() -> int { return abs_int(-5.5); }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(0, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_accepts_min_max_float) {
    char error[256];
    Program* program = parse("proc main() -> float { return min_float(1.0, 2.5); }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(1, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_rejects_min_float_with_int) {
    char error[256];
    Program* program = parse("proc main() -> float { return min_float(1, 2.5); }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(0, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_accepts_read_file) {
    char error[256];
    Program* program = parse("proc main() -> string { string s = read_file(\"x.txt\"); return s; }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(1, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_accepts_write_file) {
    char error[256];
    Program* program = parse("proc main() -> int { int ok = write_file(\"x.txt\", \"hi\"); return ok; }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(1, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_accepts_file_exists) {
    char error[256];
    Program* program = parse("proc main() -> bool { bool b = file_exists(\"x.txt\"); return b; }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(1, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_accepts_split) {
    char error[256];
    Program* program = parse("proc main() -> int { array<string> parts = split(\"a,b\", \",\"); return length(parts); }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(1, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_accepts_join) {
    char error[256];
    Program* program = parse("proc main() -> string { string s = join([\"a\", \"b\"], \"-\"); return s; }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(1, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_accepts_replace) {
    char error[256];
    Program* program = parse("proc main() -> string { string s = replace(\"a\", \"b\", \"c\"); return s; }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(1, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_accepts_repeat) {
    char error[256];
    Program* program = parse("proc main() -> string { string s = repeat(\"a\", 3); return s; }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(1, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_rejects_read_file_with_int_path) {
    char error[256];
    Program* program = parse("proc main() -> string { string s = read_file(1); return s; }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(0, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_rejects_write_file_with_int_path) {
    char error[256];
    Program* program = parse("proc main() -> int { int ok = write_file(1, \"hi\"); return ok; }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(0, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_rejects_split_with_int_delimiter) {
    char error[256];
    Program* program = parse("proc main() -> int { array<string> parts = split(\"a,b\", 1); return length(parts); }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(0, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_rejects_join_with_string_parts) {
    char error[256];
    Program* program = parse("proc main() -> string { string s = join(\"ab\", \"-\"); return s; }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(0, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_rejects_replace_with_int_old) {
    char error[256];
    Program* program = parse("proc main() -> string { string s = replace(\"a\", 1, \"c\"); return s; }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(0, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_accepts_sql_param) {
    char error[256];
    Program* program = parse(
        "proc main() -> int { int x = 1; insert into t values (?x); return 0; }",
        error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    free_program(program);
}

TEST(typecheck_rejects_repeat_with_float_count) {
    char error[256];
    Program* program = parse("proc main() -> string { string s = repeat(\"a\", 3.0); return s; }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(0, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_accepts_for_loop_with_sql_param) {
    char error[256];
    Program* program = parse(
        "proc main() -> int { int id = 1; for row in SELECT id FROM users WHERE id = ?id { return 0; } return 0; }",
        error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(1, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_rejects_for_loop_with_undefined_sql_param) {
    char error[256];
    Program* program = parse(
        "proc main() -> int { for row in SELECT id FROM users WHERE id = ?missing { return 0; } return 0; }",
        error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(0, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_accepts_format_with_string_array) {
    char error[256];
    Program* program = parse(
        "proc main() -> int { string s = format(\"%s %s\", [\"hello\", \"world\"]); return 0; }",
        error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(1, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_rejects_format_with_int_array) {
    char error[256];
    Program* program = parse(
        "proc main() -> int { string s = format(\"%s\", [1]); return 0; }",
        error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(0, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_accepts_sort_int_array) {
    char error[256];
    Program* program = parse(
        "proc main() -> int { array<int> a = [3, 1, 2]; array<int> b = sort(a); return 0; }",
        error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(1, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_rejects_sort_string_array_assigned_to_int_array) {
    char error[256];
    Program* program = parse(
        "proc main() -> int { array<int> a = sort([\"x\"]); return 0; }",
        error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(0, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_accepts_reverse_any_array) {
    char error[256];
    Program* program = parse(
        "proc main() -> int { array<bool> a = [true, false]; array<bool> b = reverse(a); return 0; }",
        error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(1, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_accepts_clamp_int) {
    char error[256];
    Program* program = parse(
        "proc main() -> int { int x = clamp(5, 0, 10); return x; }",
        error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(1, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_accepts_clamp_float_coercion) {
    char error[256];
    Program* program = parse(
        "proc main() -> int { float x = clamp(1.5, 0, 1); return 0; }",
        error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(1, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_rejects_clamp_string) {
    char error[256];
    Program* program = parse(
        "proc main() -> int { int x = clamp(\"a\", 0, 1); return 0; }",
        error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(0, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

/* Issue #51: a row loop over a table the program itself creates cannot be
 * checked against the catalog at compile time (the table does not exist yet),
 * so its columns resolve at runtime instead. Tables that already exist in the
 * catalog, and tables nothing creates, keep being checked. */

/* Typechecks `source` against an open, empty catalog. Returns typecheck's
 * result and leaves the message in `error`. */
static int typecheck_against_empty_catalog(const char* source, char* error, size_t error_size) {
    char db_path[] = "/tmp/auspex_test_typecheck_same_prog_XXXXXX.db";
    int fd = mkstemp(db_path);
    if (fd >= 0) close(fd);
    unlink(db_path);

    Context ctx;
    ctx.db_path = db_path;
    ctx.pager = NULL;
    if (!catalog_open(&ctx)) return -1;

    Program* program = parse(source, error, error_size);
    int ok = -1;
    if (program != NULL) {
        ok = typecheck_program(program, NULL, 0, &ctx, NULL, error, error_size, NULL, NULL, 0);
        free_program(program);
    }
    catalog_close(&ctx);
    unlink(db_path);
    return ok;
}

TEST(typecheck_accepts_row_loop_over_table_created_earlier_in_same_proc) {
    char error[256];
    ASSERT_INT_EQ(1, typecheck_against_empty_catalog(
        "proc main() -> int {\n"
        "    create table t (id int, name string);\n"
        "    insert into t values (1, 'a');\n"
        "    for row in select id, name from t { return row.id; }\n"
        "    return 0;\n"
        "}\n",
        error, sizeof(error)));
}

TEST(typecheck_accepts_row_loop_over_table_created_by_a_later_proc) {
    char error[256];
    ASSERT_INT_EQ(1, typecheck_against_empty_catalog(
        "proc main() -> int {\n"
        "    setup();\n"
        "    for row in select id from t { return row.id; }\n"
        "    return 0;\n"
        "}\n"
        "proc setup() -> int {\n"
        "    create table t (id int);\n"
        "    return 0;\n"
        "}\n",
        error, sizeof(error)));
}

TEST(typecheck_accepts_row_loop_over_table_created_in_nested_block) {
    char error[256];
    ASSERT_INT_EQ(1, typecheck_against_empty_catalog(
        "proc main() -> int {\n"
        "    int n = 1;\n"
        "    if n > 0 {\n"
        "        try {\n"
        "            create table if not exists t (id int);\n"
        "        } catch (e) {\n"
        "            n = 0;\n"
        "        }\n"
        "    }\n"
        "    for row in select id from t { return row.id; }\n"
        "    return 0;\n"
        "}\n",
        error, sizeof(error)));
}

TEST(typecheck_accepts_row_loop_over_view_created_in_same_program) {
    char error[256];
    ASSERT_INT_EQ(1, typecheck_against_empty_catalog(
        "proc main() -> int {\n"
        "    create table base (id int);\n"
        "    create view v as select id from base;\n"
        "    for row in select id from v { return row.id; }\n"
        "    return 0;\n"
        "}\n",
        error, sizeof(error)));
}

TEST(typecheck_accepts_row_loop_when_joined_table_is_created_in_program) {
    char error[256];
    ASSERT_INT_EQ(1, typecheck_against_empty_catalog(
        "proc main() -> int {\n"
        "    create table a (id int);\n"
        "    create table b (id int, label string);\n"
        "    for row in select a.id, b.label from a join b on a.id = b.id { return row.id; }\n"
        "    return 0;\n"
        "}\n",
        error, sizeof(error)));
}

TEST(typecheck_still_rejects_row_loop_over_table_nothing_creates) {
    char error[256];
    ASSERT_INT_EQ(0, typecheck_against_empty_catalog(
        "proc main() -> int {\n"
        "    create table t (id int);\n"
        "    for row in select id from ghost { return row.id; }\n"
        "    return 0;\n"
        "}\n",
        error, sizeof(error)));
    ASSERT_PTR_NOT_NULL(strstr(error, "Unknown column 'id' for row variable 'row'"));
}

TEST(typecheck_still_checks_catalog_table_when_program_creates_other_tables) {
    /* A table that exists in the catalog and is NOT created by the program
       keeps strict checking, even when the program creates other tables. */
    char db_path[] = "/tmp/auspex_test_typecheck_same_prog2_XXXXXX.db";
    int fd = mkstemp(db_path);
    if (fd >= 0) close(fd);
    unlink(db_path);

    Context ctx;
    ctx.db_path = db_path;
    ctx.pager = NULL;
    ASSERT_INT_EQ(1, catalog_open(&ctx));
    const char* cols[] = {"id"};
    int types[] = {VAL_INT};
    ASSERT_PTR_NOT_NULL(catalog_create_table(&ctx, "users", cols, types, 1));

    char error[256];
    Program* program = parse(
        "proc main() -> int {\n"
        "    create table other (id int);\n"
        "    for row in select id from users { return row.nope; }\n"
        "    return 0;\n"
        "}\n",
        error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(0, typecheck_program(program, NULL, 0, &ctx, NULL, error, sizeof(error), NULL, NULL, 0));
    ASSERT_PTR_NOT_NULL(strstr(error, "Unknown column 'nope' for row variable 'row'"));
    free_program(program);

    catalog_close(&ctx);
    unlink(db_path);
}

TEST(typecheck_accepts_declared_cursor_with_defined_sql_param) {
    char error[256];
    Program* program = parse(
        "proc main() -> int { int lim = 1; cursor c is select id from t where id > ?lim; open c; return 0; }",
        error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(1, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_rejects_declared_cursor_with_undefined_sql_param_at_open) {
    char error[256];
    Program* program = parse(
        "proc main() -> int { cursor c is select id from t where id > ?missing; open c; return 0; }",
        error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(0, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    ASSERT_INT_EQ(1, strstr(error, "missing") != NULL);
    free_program(program);
}

TEST(typecheck_accepts_declared_cursor_param_defined_after_the_declaration) {
    /* The value is read when the cursor is opened, so the variable only has
       to exist by then. */
    char error[256];
    Program* program = parse(
        "proc main() -> int { cursor c is select id from t where id > ?lim; int lim = 1; open c; return 0; }",
        error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(1, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

TEST(typecheck_rejects_open_for_with_undefined_sql_param) {
    char error[256];
    Program* program = parse(
        "proc main() -> int { cursor c; open c for select id from t where id = ?missing; return 0; }",
        error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(0, typecheck_program(program, NULL, 0, NULL, NULL, error, sizeof(error), NULL, NULL, 0));
    free_program(program);
}

int main(void) {
    RUN_TEST(typecheck_accepts_row_loop_over_table_created_earlier_in_same_proc);
    RUN_TEST(typecheck_accepts_row_loop_over_table_created_by_a_later_proc);
    RUN_TEST(typecheck_accepts_row_loop_over_table_created_in_nested_block);
    RUN_TEST(typecheck_accepts_row_loop_over_view_created_in_same_program);
    RUN_TEST(typecheck_accepts_row_loop_when_joined_table_is_created_in_program);
    RUN_TEST(typecheck_still_rejects_row_loop_over_table_nothing_creates);
    RUN_TEST(typecheck_still_checks_catalog_table_when_program_creates_other_tables);
    RUN_TEST(typecheck_rejects_string_to_int_assignment);
    RUN_TEST(typecheck_accepts_valid_program);
    RUN_TEST(typecheck_rejects_typed_array_mismatch);
    RUN_TEST(typecheck_accepts_int_float_coercion);
    RUN_TEST(typecheck_rejects_bool_mismatch);
    RUN_TEST(typecheck_accepts_unary_operators);
    RUN_TEST(typecheck_rejects_bad_unary);
    RUN_TEST(typecheck_accepts_comparisons);
    RUN_TEST(typecheck_rejects_mixed_comparison);
    RUN_TEST(typecheck_accepts_indexing_and_index_assign);
    RUN_TEST(typecheck_rejects_indexing_non_int_index);
    RUN_TEST(typecheck_rejects_if_string_condition);
    RUN_TEST(typecheck_rejects_narrowing_float_to_int);
    RUN_TEST(typecheck_rejects_mixed_int_float_array);
    RUN_TEST(typecheck_rejects_out_of_scope_block_variable);
    RUN_TEST(typecheck_accepts_external_proc_signature_arguments);
    RUN_TEST(typecheck_accepts_for_row_in_select_loop);
    RUN_TEST(typecheck_accepts_mixed_int_float_comparison);
    RUN_TEST(typecheck_accepts_int_to_float_assignment);
    RUN_TEST(typecheck_accepts_int_to_float_return);
    RUN_TEST(typecheck_accepts_row_field_return);
    RUN_TEST(typecheck_rejects_undefined_variable_assignment);
    RUN_TEST(typecheck_rejects_undefined_procedure_call);
    RUN_TEST(typecheck_accepts_intra_program_call);
    RUN_TEST(typecheck_accepts_uninitialized_variable);
    RUN_TEST(typecheck_rejects_intra_program_wrong_arg_count);
    RUN_TEST(typecheck_rejects_intra_program_wrong_arg_type);
    RUN_TEST(typecheck_accepts_length_on_array);
    RUN_TEST(typecheck_rejects_length_on_int);
    RUN_TEST(typecheck_accepts_append_matching_type);
    RUN_TEST(typecheck_rejects_append_mismatch);
    RUN_TEST(typecheck_accepts_println);
    RUN_TEST(typecheck_accepts_clock);
    RUN_TEST(typecheck_rejects_clock_with_args);
    RUN_TEST(typecheck_accepts_empty_array_return_with_hint);
    RUN_TEST(typecheck_resolves_sql_row_field_type);
    RUN_TEST(typecheck_rejects_sql_row_field_type_mismatch);
    RUN_TEST(typecheck_rejects_row_field_not_selected);
    RUN_TEST(typecheck_accepts_row_field_with_select_star);
    RUN_TEST(typecheck_rejects_field_on_undefined_variable);
    RUN_TEST(typecheck_accepts_named_row_variable_field);
    RUN_TEST(typecheck_resolves_named_row_variable_field_type);
    RUN_TEST(typecheck_rejects_field_on_non_row_variable);
    RUN_TEST(typecheck_accepts_string_concat);
    RUN_TEST(typecheck_rejects_string_plus_int);
    RUN_TEST(typecheck_accepts_string_comparison);
    RUN_TEST(typecheck_accepts_length_on_string);
    RUN_TEST(typecheck_rejects_concat_with_int);
    RUN_TEST(typecheck_accepts_abs_int);
    RUN_TEST(typecheck_rejects_abs_int_with_float);
    RUN_TEST(typecheck_accepts_min_max_float);
    RUN_TEST(typecheck_rejects_min_float_with_int);
    RUN_TEST(typecheck_accepts_read_file);
    RUN_TEST(typecheck_accepts_write_file);
    RUN_TEST(typecheck_accepts_file_exists);
    RUN_TEST(typecheck_accepts_split);
    RUN_TEST(typecheck_accepts_join);
    RUN_TEST(typecheck_accepts_replace);
    RUN_TEST(typecheck_accepts_repeat);
    RUN_TEST(typecheck_rejects_read_file_with_int_path);
    RUN_TEST(typecheck_rejects_write_file_with_int_path);
    RUN_TEST(typecheck_rejects_split_with_int_delimiter);
    RUN_TEST(typecheck_rejects_join_with_string_parts);
    RUN_TEST(typecheck_rejects_replace_with_int_old);
    RUN_TEST(typecheck_rejects_repeat_with_float_count);
    RUN_TEST(typecheck_accepts_sql_param);
    RUN_TEST(typecheck_accepts_for_loop_with_sql_param);
    RUN_TEST(typecheck_rejects_for_loop_with_undefined_sql_param);
    RUN_TEST(typecheck_accepts_declared_cursor_with_defined_sql_param);
    RUN_TEST(typecheck_rejects_declared_cursor_with_undefined_sql_param_at_open);
    RUN_TEST(typecheck_accepts_declared_cursor_param_defined_after_the_declaration);
    RUN_TEST(typecheck_rejects_open_for_with_undefined_sql_param);
    RUN_TEST(typecheck_accepts_format_with_string_array);
    RUN_TEST(typecheck_rejects_format_with_int_array);
    RUN_TEST(typecheck_accepts_sort_int_array);
    RUN_TEST(typecheck_rejects_sort_string_array_assigned_to_int_array);
    RUN_TEST(typecheck_accepts_reverse_any_array);
    RUN_TEST(typecheck_accepts_clamp_int);
    RUN_TEST(typecheck_accepts_clamp_float_coercion);
    RUN_TEST(typecheck_rejects_clamp_string);
    TEST_SUMMARY();
}
