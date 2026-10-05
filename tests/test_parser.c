#include <string.h>

#include "test_harness.h"
#include "parser.h"

TEST(parser_returns_empty_program_for_empty_source) {
    Program* program = parse("", NULL, 0);
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(0, program->proc_count);
    free_program(program);
}

TEST(parser_parses_integer_literal) {
    Expr* expr = parse_expression("42");
    ASSERT_PTR_NOT_NULL(expr);
    ASSERT_INT_EQ(EXPR_LITERAL, expr->kind);
    ASSERT_INT_EQ(VAL_INT, expr->as.literal.value.type);
    ASSERT_INT_EQ(42, expr->as.literal.value.as.as_int);
    free_expr(expr);
}

TEST(parser_parses_float_literal) {
    Expr* expr = parse_expression("3.14");
    ASSERT_PTR_NOT_NULL(expr);
    ASSERT_INT_EQ(EXPR_LITERAL, expr->kind);
    ASSERT_INT_EQ(VAL_FLOAT, expr->as.literal.value.type);
    ASSERT_FLOAT_EQ(3.14, expr->as.literal.value.as.as_float);
    free_expr(expr);
}

TEST(parser_parses_string_literal) {
    Expr* expr = parse_expression("\"hello\"");
    ASSERT_PTR_NOT_NULL(expr);
    ASSERT_INT_EQ(EXPR_LITERAL, expr->kind);
    ASSERT_INT_EQ(VAL_STRING, expr->as.literal.value.type);
    ASSERT_STRING_EQ("hello", expr->as.literal.value.as.as_string);
    free_expr(expr);
}

TEST(parser_parses_identifier) {
    Expr* expr = parse_expression("foo");
    ASSERT_PTR_NOT_NULL(expr);
    ASSERT_INT_EQ(EXPR_VARIABLE, expr->kind);
    ASSERT_INT_EQ(0, strcmp("foo", expr->as.variable.name));
    free_expr(expr);
}

TEST(parser_parses_addition) {
    Expr* expr = parse_expression("1 + 2");
    ASSERT_PTR_NOT_NULL(expr);
    ASSERT_INT_EQ(EXPR_BINARY, expr->kind);
    ASSERT_INT_EQ(TOKEN_PLUS, expr->as.binary.op);
    free_expr(expr);
}

TEST(parser_parses_unary_minus) {
    Expr* expr = parse_expression("-5");
    ASSERT_PTR_NOT_NULL(expr);
    ASSERT_INT_EQ(EXPR_UNARY, expr->kind);
    ASSERT_INT_EQ(TOKEN_MINUS, expr->as.unary.op);
    ASSERT_INT_EQ(EXPR_LITERAL, expr->as.unary.operand->kind);
    ASSERT_INT_EQ(5, expr->as.unary.operand->as.literal.value.as.as_int);
    free_expr(expr);
}

TEST(parser_parses_unary_not) {
    Expr* expr = parse_expression("!0");
    ASSERT_PTR_NOT_NULL(expr);
    ASSERT_INT_EQ(EXPR_UNARY, expr->kind);
    ASSERT_INT_EQ(TOKEN_BANG, expr->as.unary.op);
    ASSERT_INT_EQ(EXPR_LITERAL, expr->as.unary.operand->kind);
    ASSERT_INT_EQ(0, expr->as.unary.operand->as.literal.value.as.as_int);
    free_expr(expr);
}

TEST(parser_respects_precedence) {
    Expr* expr = parse_expression("1 + 2 * 3");
    ASSERT_PTR_NOT_NULL(expr);
    ASSERT_INT_EQ(EXPR_BINARY, expr->kind);
    ASSERT_INT_EQ(TOKEN_PLUS, expr->as.binary.op);
    ASSERT_INT_EQ(EXPR_BINARY, expr->as.binary.right->kind);
    ASSERT_INT_EQ(TOKEN_STAR, expr->as.binary.right->as.binary.op);
    free_expr(expr);
}

TEST(parser_parses_comparison) {
    Expr* expr = parse_expression("x == 1");
    ASSERT_PTR_NOT_NULL(expr);
    ASSERT_INT_EQ(EXPR_BINARY, expr->kind);
    ASSERT_INT_EQ(TOKEN_EQ, expr->as.binary.op);
    free_expr(expr);
}

TEST(parser_parses_field_expression) {
    Expr* expr = parse_expression("row.id");
    ASSERT_PTR_NOT_NULL(expr);
    ASSERT_INT_EQ(EXPR_FIELD, expr->kind);
    ASSERT_INT_EQ(0, strcmp("row", expr->as.field.row));
    ASSERT_INT_EQ(0, strcmp("id", expr->as.field.field));
    free_expr(expr);
}

TEST(parser_parses_procedure_and_var_decl) {
    Program* program = parse("proc main() -> int { int x = 42; }", NULL, 0);
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(1, program->proc_count);
    ProcDecl* proc = &program->procs[0];
    ASSERT_INT_EQ(0, strcmp("main", proc->name));
    ASSERT_INT_EQ(TYPE_INT, proc->return_type->kind);
    ASSERT_INT_EQ(1, proc->body->stmt_count);
    Stmt* stmt = proc->body->stmts[0];
    ASSERT_INT_EQ(STMT_VAR_DECL, stmt->kind);
    ASSERT_INT_EQ(TYPE_INT, stmt->as.var_decl.type->kind);
    ASSERT_INT_EQ(0, strcmp("x", stmt->as.var_decl.name));
    ASSERT_INT_EQ(EXPR_LITERAL, stmt->as.var_decl.initializer->kind);
    ASSERT_INT_EQ(42, stmt->as.var_decl.initializer->as.literal.value.as.as_int);
    free_program(program);
}

TEST(parser_parses_assignment) {
    Program* program = parse("proc main() -> int { x = 1; }", NULL, 0);
    ASSERT_PTR_NOT_NULL(program);
    Stmt* stmt = program->procs[0].body->stmts[0];
    ASSERT_INT_EQ(STMT_ASSIGN, stmt->kind);
    ASSERT_INT_EQ(0, strcmp("x", stmt->as.assign.name));
    free_program(program);
}

TEST(parser_parses_if_statement) {
    Program* program = parse("proc main() -> int { if x == 1 { return 0; } }", NULL, 0);
    ASSERT_PTR_NOT_NULL(program);
    Stmt* stmt = program->procs[0].body->stmts[0];
    ASSERT_INT_EQ(STMT_IF, stmt->kind);
    free_program(program);
}

TEST(parser_parses_while_statement) {
    Program* program = parse("proc main() -> int { while x < 10 { x = x + 1; } }", NULL, 0);
    ASSERT_PTR_NOT_NULL(program);
    Stmt* stmt = program->procs[0].body->stmts[0];
    ASSERT_INT_EQ(STMT_WHILE, stmt->kind);
    ASSERT_PTR_NOT_NULL(stmt->as.while_stmt.condition);
    ASSERT_PTR_NOT_NULL(stmt->as.while_stmt.body);
    free_program(program);
}

TEST(parser_parses_break_statement) {
    Program* program = parse("proc main() -> int { while 1 { break; } }", NULL, 0);
    ASSERT_PTR_NOT_NULL(program);
    Stmt* while_stmt = program->procs[0].body->stmts[0];
    ASSERT_INT_EQ(STMT_WHILE, while_stmt->kind);
    Stmt* body_stmt = while_stmt->as.while_stmt.body->stmts[0];
    ASSERT_INT_EQ(STMT_BREAK, body_stmt->kind);
    free_program(program);
}

TEST(parser_parses_continue_statement) {
    Program* program = parse("proc main() -> int { while 1 { continue; } }", NULL, 0);
    ASSERT_PTR_NOT_NULL(program);
    Stmt* while_stmt = program->procs[0].body->stmts[0];
    ASSERT_INT_EQ(STMT_WHILE, while_stmt->kind);
    Stmt* body_stmt = while_stmt->as.while_stmt.body->stmts[0];
    ASSERT_INT_EQ(STMT_CONTINUE, body_stmt->kind);
    free_program(program);
}

TEST(parser_parses_do_while_statement) {
    Program* program = parse("proc main() -> int { do { x = x + 1; } while x < 10; }", NULL, 0);
    ASSERT_PTR_NOT_NULL(program);
    Stmt* stmt = program->procs[0].body->stmts[0];
    ASSERT_INT_EQ(STMT_WHILE, stmt->kind);
    ASSERT_INT_EQ(1, stmt->as.while_stmt.is_do_while);
    ASSERT_PTR_NOT_NULL(stmt->as.while_stmt.condition);
    ASSERT_PTR_NOT_NULL(stmt->as.while_stmt.body);
    free_program(program);
}

TEST(parser_parses_cfor_statement) {
    Program* program = parse("proc main() -> int { for (int i = 0; i < 10; i = i + 1) { return 0; } }", NULL, 0);
    ASSERT_PTR_NOT_NULL(program);
    Stmt* stmt = program->procs[0].body->stmts[0];
    ASSERT_INT_EQ(STMT_FOR_C, stmt->kind);
    ASSERT_PTR_NOT_NULL(stmt->as.cfor_stmt.init);
    ASSERT_PTR_NOT_NULL(stmt->as.cfor_stmt.condition);
    ASSERT_PTR_NOT_NULL(stmt->as.cfor_stmt.step);
    ASSERT_PTR_NOT_NULL(stmt->as.cfor_stmt.body);
    free_program(program);
}

TEST(parser_parses_else_branch) {
    Program* program = parse("proc main() -> int { if 0 { return 1; } else { return 2; } }", NULL, 0);
    ASSERT_PTR_NOT_NULL(program);
    Stmt* stmt = program->procs[0].body->stmts[0];
    ASSERT_INT_EQ(STMT_IF, stmt->kind);
    ASSERT_PTR_NOT_NULL(stmt->as.if_stmt.else_block);
    ASSERT_INT_EQ(1, stmt->as.if_stmt.else_block->stmt_count);
    free_program(program);
}

TEST(parser_parses_else_if_chain) {
    Program* program = parse("proc main() -> int { if 0 { return 1; } else if 1 { return 2; } else { return 3; } }", NULL, 0);
    ASSERT_PTR_NOT_NULL(program);
    Stmt* stmt = program->procs[0].body->stmts[0];
    ASSERT_INT_EQ(STMT_IF, stmt->kind);
    ASSERT_PTR_NOT_NULL(stmt->as.if_stmt.else_block);
    ASSERT_INT_EQ(1, stmt->as.if_stmt.else_block->stmt_count);
    Stmt* nested = stmt->as.if_stmt.else_block->stmts[0];
    ASSERT_INT_EQ(STMT_IF, nested->kind);
    ASSERT_PTR_NOT_NULL(nested->as.if_stmt.else_block);
    free_program(program);
}

TEST(parser_parses_return_statement) {
    Program* program = parse("proc main() -> int { return 42; }", NULL, 0);
    ASSERT_PTR_NOT_NULL(program);
    Stmt* stmt = program->procs[0].body->stmts[0];
    ASSERT_INT_EQ(STMT_RETURN, stmt->kind);
    ASSERT_INT_EQ(EXPR_LITERAL, stmt->as.return_stmt.value->kind);
    free_program(program);
}

TEST(parser_parses_for_sql_loop) {
    Program* program = parse("proc main() -> int { for row in SELECT * FROM users { return 0; } }", NULL, 0);
    ASSERT_PTR_NOT_NULL(program);
    Stmt* stmt = program->procs[0].body->stmts[0];
    ASSERT_INT_EQ(STMT_FOR, stmt->kind);
    ASSERT_INT_EQ(0, strcmp("row", stmt->as.for_stmt.var_name));
    ASSERT_INT_EQ(0, strcmp("SELECT * FROM users", stmt->as.for_stmt.sql_query));
    free_program(program);
}

TEST(parser_parses_for_sql_loop_with_param) {
    Program* program = parse("proc main() -> int { int id = 1; for row in SELECT * FROM users WHERE id = ?id { return 0; } }", NULL, 0);
    ASSERT_PTR_NOT_NULL(program);
    Stmt* stmt = program->procs[0].body->stmts[1];
    ASSERT_INT_EQ(STMT_FOR, stmt->kind);
    ASSERT_STRING_EQ("SELECT * FROM users WHERE id = ?", stmt->as.for_stmt.sql_query);
    ASSERT_INT_EQ(1, stmt->as.for_stmt.param_count);
    ASSERT_INT_EQ(EXPR_SQL_PARAM, stmt->as.for_stmt.params[0]->kind);
    ASSERT_STRING_EQ("id", stmt->as.for_stmt.params[0]->as.sql_param.name);
    free_program(program);
}

TEST(parser_parses_foreach_loop) {
    Program* program = parse("proc main() -> int { for i in range(0, 3) { return 0; } }", NULL, 0);
    ASSERT_PTR_NOT_NULL(program);
    Stmt* stmt = program->procs[0].body->stmts[0];
    ASSERT_INT_EQ(STMT_FOREACH, stmt->kind);
    ASSERT_STRING_EQ("i", stmt->as.foreach_stmt.var_name);
    ASSERT_INT_EQ(EXPR_CALL, stmt->as.foreach_stmt.iterable->kind);
    free_program(program);
}

TEST(parser_parses_foreach_over_array_variable) {
    Program* program = parse("proc main() -> int { array<int> a = [1, 2]; for i in a { return 0; } }", NULL, 0);
    ASSERT_PTR_NOT_NULL(program);
    Stmt* stmt = program->procs[0].body->stmts[1];
    ASSERT_INT_EQ(STMT_FOREACH, stmt->kind);
    ASSERT_INT_EQ(EXPR_VARIABLE, stmt->as.foreach_stmt.iterable->kind);
    free_program(program);
}

TEST(parser_parses_call_expression) {
    Expr* expr = parse_expression("foo()");
    ASSERT_PTR_NOT_NULL(expr);
    ASSERT_INT_EQ(EXPR_CALL, expr->kind);
    ASSERT_INT_EQ(0, strcmp("foo", expr->as.call.name));
    ASSERT_INT_EQ(0, expr->as.call.arg_count);
    free_expr(expr);
}

TEST(parser_parses_procedure_parameters) {
    Program* program = parse("proc add(a int, b int) -> int { return a + b; }", NULL, 0);
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(1, program->proc_count);
    ProcDecl* proc = &program->procs[0];
    ASSERT_INT_EQ(2, proc->param_count);
    ASSERT_INT_EQ(0, strcmp("a", proc->params[0].name));
    ASSERT_INT_EQ(TYPE_INT, proc->params[0].type->kind);
    ASSERT_INT_EQ(0, strcmp("b", proc->params[1].name));
    ASSERT_INT_EQ(TYPE_INT, proc->params[1].type->kind);
    free_program(program);
}

TEST(parser_reports_error_for_missing_brace) {
    char error[256];
    Program* program = parse("proc main() -> int { return 1; ", error, sizeof(error));
    ASSERT_PTR_NULL(program);
    ASSERT_PTR_NOT_NULL(strstr(error, "1:"));
    ASSERT_PTR_NOT_NULL(strstr(error, ": error:"));
}

TEST(parser_reports_error_location_on_correct_line) {
    char error[256];
    Program* program = parse(
        "proc main() -> int {\n"
        "    return ;\n"
        "}",
        error, sizeof(error));
    ASSERT_PTR_NULL(program);
    ASSERT_PTR_NOT_NULL(strstr(error, "2:"));
    ASSERT_PTR_NOT_NULL(strstr(error, ": error:"));
}

TEST(parser_parses_bool_literal_true) {
    Expr* expr = parse_expression("true");
    ASSERT_PTR_NOT_NULL(expr);
    ASSERT_INT_EQ(EXPR_LITERAL, expr->kind);
    ASSERT_INT_EQ(VAL_BOOL, expr->as.literal.value.type);
    ASSERT_INT_EQ(1, expr->as.literal.value.as.as_int);
    free_expr(expr);
}

TEST(parser_parses_bool_literal_false) {
    Expr* expr = parse_expression("false");
    ASSERT_PTR_NOT_NULL(expr);
    ASSERT_INT_EQ(EXPR_LITERAL, expr->kind);
    ASSERT_INT_EQ(VAL_BOOL, expr->as.literal.value.type);
    ASSERT_INT_EQ(0, expr->as.literal.value.as.as_int);
    free_expr(expr);
}

TEST(parser_parses_array_literal) {
    Program* program = parse("proc main() -> array { array a = [1, 2, 3]; return a; }", NULL, 0);
    ASSERT_PTR_NOT_NULL(program);
    Stmt* stmt = program->procs[0].body->stmts[0];
    ASSERT_INT_EQ(STMT_VAR_DECL, stmt->kind);
    ASSERT_INT_EQ(TYPE_ARRAY, stmt->as.var_decl.type->kind);
    ASSERT_INT_EQ(EXPR_ARRAY, stmt->as.var_decl.initializer->kind);
    ASSERT_INT_EQ(3, stmt->as.var_decl.initializer->as.array.count);
    free_program(program);
}

TEST(parser_parses_index_expression) {
    Expr* expr = parse_expression("a[0]");
    ASSERT_PTR_NOT_NULL(expr);
    ASSERT_INT_EQ(EXPR_INDEX, expr->kind);
    ASSERT_INT_EQ(EXPR_VARIABLE, expr->as.index.array->kind);
    ASSERT_INT_EQ(EXPR_LITERAL, expr->as.index.index->kind);
    free_expr(expr);
}

TEST(parser_parses_index_assignment) {
    Program* program = parse("proc main() -> int { a[0] = 1; }", NULL, 0);
    ASSERT_PTR_NOT_NULL(program);
    Stmt* stmt = program->procs[0].body->stmts[0];
    ASSERT_INT_EQ(STMT_INDEX_ASSIGN, stmt->kind);
    ASSERT_INT_EQ(EXPR_VARIABLE, stmt->as.index_assign.array->kind);
    free_program(program);
}

TEST(parser_parses_nested_index_assignment) {
    Program* program = parse("proc main() -> int { a[0][1] = 2; }", NULL, 0);
    ASSERT_PTR_NOT_NULL(program);
    Stmt* stmt = program->procs[0].body->stmts[0];
    ASSERT_INT_EQ(STMT_INDEX_ASSIGN, stmt->kind);
    ASSERT_INT_EQ(EXPR_INDEX, stmt->as.index_assign.array->kind);
    ASSERT_INT_EQ(EXPR_VARIABLE, stmt->as.index_assign.array->as.index.array->kind);
    ASSERT_INT_EQ(EXPR_LITERAL, stmt->as.index_assign.array->as.index.index->kind);
    ASSERT_INT_EQ(EXPR_LITERAL, stmt->as.index_assign.index->kind);
    free_program(program);
}

TEST(ast_sql_stmt_node_exists) {
    Stmt* stmt = create_sql_stmt(STMT_SQL_DDL, "CREATE TABLE t (id int)", NULL, 0, NULL, 0);
    ASSERT_PTR_NOT_NULL(stmt);
    ASSERT_INT_EQ(STMT_SQL_DDL, stmt->kind);
    free_stmt(stmt);
}

TEST(parser_parses_create_table) {
    Program* program = parse("proc main() -> int { create table t (id int); return 0; }", NULL, 0);
    ASSERT_PTR_NOT_NULL(program);
    Stmt* stmt = program->procs[0].body->stmts[0];
    ASSERT_INT_EQ(STMT_SQL_DDL, stmt->kind);
    ASSERT_STRING_EQ("create table t (id int)", stmt->as.sql_stmt.sql);
    free_program(program);
}

TEST(parser_parses_insert_with_param) {
    Program* program = parse("proc main() -> int { int x = 1; insert into t values (?x); return 0; }", NULL, 0);
    ASSERT_PTR_NOT_NULL(program);
    Stmt* stmt = program->procs[0].body->stmts[1];
    ASSERT_INT_EQ(STMT_SQL_DML, stmt->kind);
    ASSERT_STRING_EQ("insert into t values (?)", stmt->as.sql_stmt.sql);
    ASSERT_INT_EQ(1, stmt->as.sql_stmt.param_count);
    ASSERT_INT_EQ(EXPR_SQL_PARAM, stmt->as.sql_stmt.params[0]->kind);
    ASSERT_STRING_EQ("x", stmt->as.sql_stmt.params[0]->as.sql_param.name);
    free_program(program);
}

TEST(parser_parses_select_into_single_value) {
    Program* program = parse("proc main() -> int { string name; SELECT name INTO my_name FROM users WHERE id = 1; return 0; }", NULL, 0);
    ASSERT_PTR_NOT_NULL(program);
    Stmt* stmt = program->procs[0].body->stmts[1];
    ASSERT_INT_EQ(STMT_SQL_QUERY, stmt->kind);
    ASSERT_STRING_EQ("SELECT name FROM users WHERE id = 1", stmt->as.sql_stmt.sql);
    ASSERT_INT_EQ(1, stmt->as.sql_stmt.into_count);
    ASSERT_STRING_EQ("my_name", stmt->as.sql_stmt.into_vars[0]);
    free_program(program);
}

TEST(parser_parses_select_into_multi_value) {
    Program* program = parse("proc main() -> int { int a; int b; SELECT id, name INTO a, b FROM users WHERE id = 1; return 0; }", NULL, 0);
    ASSERT_PTR_NOT_NULL(program);
    Stmt* stmt = program->procs[0].body->stmts[2];
    ASSERT_INT_EQ(STMT_SQL_QUERY, stmt->kind);
    ASSERT_STRING_EQ("SELECT id, name FROM users WHERE id = 1", stmt->as.sql_stmt.sql);
    ASSERT_INT_EQ(2, stmt->as.sql_stmt.into_count);
    ASSERT_STRING_EQ("a", stmt->as.sql_stmt.into_vars[0]);
    ASSERT_STRING_EQ("b", stmt->as.sql_stmt.into_vars[1]);
    free_program(program);
}

TEST(parser_parses_select_into_array_row) {
    Program* program = parse("proc main() -> int { array<row> rows; SELECT * INTO rows FROM users; return 0; }", NULL, 0);
    ASSERT_PTR_NOT_NULL(program);
    Stmt* stmt = program->procs[0].body->stmts[1];
    ASSERT_INT_EQ(STMT_SQL_QUERY, stmt->kind);
    ASSERT_STRING_EQ("SELECT * FROM users", stmt->as.sql_stmt.sql);
    ASSERT_INT_EQ(1, stmt->as.sql_stmt.into_count);
    ASSERT_STRING_EQ("rows", stmt->as.sql_stmt.into_vars[0]);
    free_program(program);
}

TEST(parser_parses_import_statement) {
    Program* program = parse("import \"foo.apx\"; proc main() -> int { return 0; }", NULL, 0);
    ASSERT_PTR_NOT_NULL(program);
    ASSERT_INT_EQ(1, program->import_count);
    ASSERT_INT_EQ(STMT_IMPORT, program->imports[0]->kind);
    ASSERT_STRING_EQ("foo.apx", program->imports[0]->as.import_stmt.path);
    free_program(program);
}

TEST(parser_parses_typed_array_variable) {
    char error[256];
    Program* program = parse("proc main() -> int { array<int> a = [1, 2]; return 0; }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    free_program(program);
}

TEST(parser_parses_nested_typed_array) {
    char error[256];
    Program* program = parse("proc main() -> int { array<array<int>> a = []; return 0; }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    free_program(program);
}

/* Regressions for crashes found by the fuzz harness (tests/fuzz). Each input
 * is invalid; the parser must report an error rather than crash. */

TEST(parser_rejects_integer_literal_overflow) {
    char error[256];
    Program* program = parse("proc main() -> int { int x = 2147483647; return 0; }", error, sizeof(error));
    ASSERT_PTR_NOT_NULL(program);
    free_program(program);
    program = parse("proc main() -> int { int x = 99999999999; return 0; }", error, sizeof(error));
    ASSERT_PTR_NULL(program);
    ASSERT_PTR_NOT_NULL(strstr(error, "integer literal too large"));
}

TEST(parser_map_literal_missing_colon_is_error) {
    char error[256];
    Program* program = parse(
        "proc main() -> int { map<string, int> m = {\"a\": 1, \"b\" 2}; return 0; }",
        error, sizeof(error));
    ASSERT_PTR_NULL(program);
    ASSERT_PTR_NOT_NULL(strstr(error, "expected ':' after map key"));
}

TEST(parser_failed_operand_before_field_access_is_error) {
    char error[256];
    Program* program = parse("proc main() -> int { int x = (;).y; return 0; }", error, sizeof(error));
    ASSERT_PTR_NULL(program);
    ASSERT_PTR_NOT_NULL(strstr(error, "expected expression"));
}

TEST(parser_failed_operand_before_cursor_attr_is_error) {
    char error[256];
    Program* program = parse("proc main() -> int { int x = (;)%rowcount; return 0; }", error, sizeof(error));
    ASSERT_PTR_NULL(program);
    ASSERT_PTR_NOT_NULL(strstr(error, "expected expression"));
}

TEST(parser_failed_operand_before_call_is_error) {
    char error[256];
    Program* program = parse("proc main() -> int { int x = (;)(1); return 0; }", error, sizeof(error));
    ASSERT_PTR_NULL(program);
    ASSERT_PTR_NOT_NULL(strstr(error, "expected expression"));
}

TEST(parser_open_for_query_carries_placeholders_as_params) {
    Program* program = parse(
        "proc main() -> int { cursor c; open c for select id from t where id > ?lo and id < ?hi; return 0; }",
        NULL, 0);
    ASSERT_PTR_NOT_NULL(program);
    Stmt* open = program->procs[0].body->stmts[1];
    ASSERT_INT_EQ(STMT_CURSOR_OPEN, open->kind);
    ASSERT_STRING_EQ("select id from t where id > ? and id < ?", open->as.cursor_open.sql_query);
    ASSERT_INT_EQ(2, open->as.cursor_open.param_count);
    ASSERT_INT_EQ(EXPR_SQL_PARAM, open->as.cursor_open.params[0]->kind);
    ASSERT_STRING_EQ("lo", open->as.cursor_open.params[0]->as.sql_param.name);
    ASSERT_STRING_EQ("hi", open->as.cursor_open.params[1]->as.sql_param.name);
    free_program(program);
}

TEST(parser_declared_cursor_query_carries_placeholders_as_params) {
    Program* program = parse(
        "proc main() -> int { cursor c is select id from t where name = ?who and id > ?n; open c; return 0; }",
        NULL, 0);
    ASSERT_PTR_NOT_NULL(program);
    Stmt* decl = program->procs[0].body->stmts[0];
    ASSERT_INT_EQ(STMT_CURSOR_DECL, decl->kind);
    ASSERT_STRING_EQ("select id from t where name = ? and id > ?", decl->as.cursor_decl.sql_query);
    ASSERT_INT_EQ(2, decl->as.cursor_decl.param_count);
    ASSERT_STRING_EQ("who", decl->as.cursor_decl.params[0]->as.sql_param.name);
    ASSERT_STRING_EQ("n", decl->as.cursor_decl.params[1]->as.sql_param.name);
    free_program(program);
}

TEST(parser_cursor_query_placeholder_inside_string_literal_is_text) {
    Program* program = parse(
        "proc main() -> int { cursor c is select id from t where note = 'why?not' and id = ?n; return 0; }",
        NULL, 0);
    ASSERT_PTR_NOT_NULL(program);
    Stmt* decl = program->procs[0].body->stmts[0];
    ASSERT_STRING_EQ("select id from t where note = 'why?not' and id = ?", decl->as.cursor_decl.sql_query);
    ASSERT_INT_EQ(1, decl->as.cursor_decl.param_count);
    ASSERT_STRING_EQ("n", decl->as.cursor_decl.params[0]->as.sql_param.name);
    free_program(program);
}

TEST(parser_cursor_without_placeholders_has_no_params) {
    Program* program = parse(
        "proc main() -> int { cursor c is select id from t; return 0; }", NULL, 0);
    ASSERT_PTR_NOT_NULL(program);
    Stmt* decl = program->procs[0].body->stmts[0];
    ASSERT_INT_EQ(0, decl->as.cursor_decl.param_count);
    ASSERT_PTR_NULL(decl->as.cursor_decl.params);
    free_program(program);
}

TEST(parser_parses_call_as_cfor_step) {
    Program* program = parse("proc main() -> int { for (int i = 0; i < 3; tick()) { return 0; } }", NULL, 0);
    ASSERT_PTR_NOT_NULL(program);
    Stmt* stmt = program->procs[0].body->stmts[0];
    ASSERT_INT_EQ(STMT_FOR_C, stmt->kind);
    Stmt* step = stmt->as.cfor_stmt.step;
    ASSERT_PTR_NOT_NULL(step);
    ASSERT_INT_EQ(STMT_EXPR, step->kind);
    Expr* call = step->as.expr_stmt.value;
    ASSERT_PTR_NOT_NULL(call);
    ASSERT_INT_EQ(EXPR_CALL, call->kind);
    ASSERT_STRING_EQ("tick", call->as.call.name);
    ASSERT_INT_EQ(0, call->as.call.arg_count);
    free_program(program);
}

TEST(parser_parses_call_with_arguments_as_cfor_step) {
    Program* program = parse("proc main() -> int { for (int i = 0; i < 3; bump(i, 2)) { return 0; } }", NULL, 0);
    ASSERT_PTR_NOT_NULL(program);
    Stmt* step = program->procs[0].body->stmts[0]->as.cfor_stmt.step;
    ASSERT_PTR_NOT_NULL(step);
    ASSERT_INT_EQ(STMT_EXPR, step->kind);
    Expr* call = step->as.expr_stmt.value;
    ASSERT_INT_EQ(EXPR_CALL, call->kind);
    ASSERT_STRING_EQ("bump", call->as.call.name);
    ASSERT_INT_EQ(2, call->as.call.arg_count);
    ASSERT_INT_EQ(EXPR_VARIABLE, call->as.call.args[0]->kind);
    ASSERT_INT_EQ(EXPR_LITERAL, call->as.call.args[1]->kind);
    free_program(program);
}

TEST(parser_rejects_cfor_call_step_missing_close_paren) {
    char error[256];
    Program* program = parse("proc main() -> int { for (int i = 0; i < 3; bump(i) { return 0; } }", error, sizeof(error));
    ASSERT_PTR_NULL(program);
    ASSERT_INT_EQ(1, error[0] != '\0');
}

int main(void) {
    RUN_TEST(parser_returns_empty_program_for_empty_source);
    RUN_TEST(parser_parses_integer_literal);
    RUN_TEST(parser_parses_float_literal);
    RUN_TEST(parser_parses_string_literal);
    RUN_TEST(parser_parses_identifier);
    RUN_TEST(parser_parses_addition);
    RUN_TEST(parser_parses_unary_minus);
    RUN_TEST(parser_parses_unary_not);
    RUN_TEST(parser_respects_precedence);
    RUN_TEST(parser_parses_comparison);
    RUN_TEST(parser_parses_field_expression);
    RUN_TEST(parser_parses_procedure_and_var_decl);
    RUN_TEST(parser_parses_assignment);
    RUN_TEST(parser_parses_if_statement);
    RUN_TEST(parser_parses_while_statement);
    RUN_TEST(parser_parses_break_statement);
    RUN_TEST(parser_parses_continue_statement);
    RUN_TEST(parser_parses_do_while_statement);
    RUN_TEST(parser_parses_cfor_statement);
    RUN_TEST(parser_open_for_query_carries_placeholders_as_params);
    RUN_TEST(parser_declared_cursor_query_carries_placeholders_as_params);
    RUN_TEST(parser_cursor_query_placeholder_inside_string_literal_is_text);
    RUN_TEST(parser_cursor_without_placeholders_has_no_params);
    RUN_TEST(parser_parses_call_as_cfor_step);
    RUN_TEST(parser_parses_call_with_arguments_as_cfor_step);
    RUN_TEST(parser_rejects_cfor_call_step_missing_close_paren);
    RUN_TEST(parser_parses_else_branch);
    RUN_TEST(parser_parses_else_if_chain);
    RUN_TEST(parser_parses_return_statement);
    RUN_TEST(parser_parses_for_sql_loop);
    RUN_TEST(parser_parses_for_sql_loop_with_param);
    RUN_TEST(parser_parses_foreach_loop);
    RUN_TEST(parser_parses_foreach_over_array_variable);
    RUN_TEST(parser_parses_call_expression);
    RUN_TEST(parser_parses_procedure_parameters);
    RUN_TEST(parser_reports_error_for_missing_brace);
    RUN_TEST(parser_reports_error_location_on_correct_line);
    RUN_TEST(parser_parses_bool_literal_true);
    RUN_TEST(parser_parses_bool_literal_false);
    RUN_TEST(parser_parses_array_literal);
    RUN_TEST(parser_parses_index_expression);
    RUN_TEST(parser_parses_index_assignment);
    RUN_TEST(parser_parses_nested_index_assignment);
    RUN_TEST(ast_sql_stmt_node_exists);
    RUN_TEST(parser_parses_create_table);
    RUN_TEST(parser_parses_insert_with_param);
    RUN_TEST(parser_parses_select_into_single_value);
    RUN_TEST(parser_parses_select_into_multi_value);
    RUN_TEST(parser_parses_select_into_array_row);
    RUN_TEST(parser_parses_import_statement);
    RUN_TEST(parser_parses_typed_array_variable);
    RUN_TEST(parser_parses_nested_typed_array);
    RUN_TEST(parser_rejects_integer_literal_overflow);
    RUN_TEST(parser_map_literal_missing_colon_is_error);
    RUN_TEST(parser_failed_operand_before_field_access_is_error);
    RUN_TEST(parser_failed_operand_before_cursor_attr_is_error);
    RUN_TEST(parser_failed_operand_before_call_is_error);
    TEST_SUMMARY();
}
