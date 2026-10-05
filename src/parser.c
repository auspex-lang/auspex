#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diagnostics.h"
#include "parser.h"
#include "lexer.h"

#define MAX_IMPORTS 64
#define MAX_PARSER_STRUCTS 64

typedef struct {
    Lexer lexer;
    Token current;
    Token previous;
    int had_error;
    const char* path;
    char error_message[256];
    char* struct_names[MAX_PARSER_STRUCTS];
    int struct_count;
} Parser;

typedef enum {
    PREC_NONE,
    PREC_ASSIGNMENT,
    PREC_OR,
    PREC_AND,
    PREC_EQUALITY,
    PREC_COMPARISON,
    PREC_TERM,
    PREC_FACTOR,
    PREC_UNARY,
    PREC_CALL,
    PREC_PRIMARY
} Precedence;

typedef Expr* (*PrefixFn)(Parser* parser);
typedef Expr* (*InfixFn)(Parser* parser, Expr* left);

typedef struct {
    PrefixFn prefix;
    InfixFn infix;
    Precedence precedence;
} ParseRule;

static ParseRule* get_rule(TokenType type);
static Expr* parse_precedence(Parser* parser, Precedence precedence);

static void error_at_current(Parser* parser, const char* message) {
    char msg[256];
    snprintf(msg, sizeof(msg), "%s (got token %d)", message, parser->current.type);
    format_error(parser->error_message, sizeof(parser->error_message),
                 parser->path, parser->current.line, parser->current.column, msg);
    parser->had_error = 1;
}

static void error_at_previous(Parser* parser, const char* message) {
    char msg[256];
    snprintf(msg, sizeof(msg), "%s (got token %d)", message, parser->previous.type);
    format_error(parser->error_message, sizeof(parser->error_message),
                 parser->path, parser->previous.line, parser->previous.column, msg);
    parser->had_error = 1;
}

static void advance(Parser* parser) {
    parser->previous = parser->current;
    parser->current = lexer_next_token(&parser->lexer);
}

static int check(Parser* parser, TokenType type) {
    return parser->current.type == type;
}

static int match(Parser* parser, TokenType type) {
    if (!check(parser, type)) return 0;
    advance(parser);
    return 1;
}

static int is_collection_method_name(const char* name) {
    return strcmp(name, "delete") == 0 ||
           strcmp(name, "first") == 0 ||
           strcmp(name, "last") == 0 ||
           strcmp(name, "next") == 0 ||
           strcmp(name, "prior") == 0 ||
           strcmp(name, "extend") == 0 ||
           strcmp(name, "trim") == 0;
}

static char* copy_token_lexeme(const Token* token) {
    char* lexeme = malloc((size_t)token->length + 1);
    memcpy(lexeme, token->start, (size_t)token->length);
    lexeme[token->length] = '\0';
    return lexeme;
}

static int parser_is_struct_name(Parser* parser, const char* name) {
    for (int i = 0; i < parser->struct_count; i++) {
        if (strcmp(parser->struct_names[i], name) == 0) return 1;
    }
    return 0;
}

static int parser_add_struct_name(Parser* parser, const char* name) {
    if (parser->struct_count >= MAX_PARSER_STRUCTS) return 0;
    char* copy = malloc(strlen(name) + 1);
    if (copy == NULL) return 0;
    strcpy(copy, name);
    parser->struct_names[parser->struct_count++] = copy;
    return 1;
}

static void parser_free_struct_names(Parser* parser) {
    for (int i = 0; i < parser->struct_count; i++) {
        free(parser->struct_names[i]);
    }
    parser->struct_count = 0;
}

static Expr* expression(Parser* parser);

static int is_ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static int is_word_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static int ascii_case_eq(char a, char b) {
    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
    return a == b;
}

static int current_token_is_keyword(Parser* parser, const char* word) {
    if (!check(parser, TOKEN_IDENT)) return 0;
    int len = (int)strlen(word);
    if (parser->current.length != len) return 0;
    for (int i = 0; i < len; i++) {
        if (!ascii_case_eq(parser->current.start[i], word[i])) return 0;
    }
    return 1;
}

static int match_authid(Parser* parser) {
    if (!check(parser, TOKEN_IDENT)) return 0;
    if (parser->current.length != 6) return 0;
    for (int i = 0; i < 6; i++) {
        if (!ascii_case_eq(parser->current.start[i], "authid"[i])) return 0;
    }
    advance(parser);
    return 1;
}

static const char* find_word(const char* s, const char* word) {
    size_t len = strlen(word);
    size_t s_len = strlen(s);
    if (len == 0 || len > s_len) return NULL;
    for (const char* p = s; p <= s + s_len - len; p++) {
        if ((p == s || !is_word_char(p[-1])) && !is_word_char(p[len])) {
            int match = 1;
            for (size_t i = 0; i < len; i++) {
                if (!ascii_case_eq(p[i], word[i])) {
                    match = 0;
                    break;
                }
            }
            if (match) return p;
        }
    }
    return NULL;
}

static Stmt* sql_statement(Parser* parser, int kind) {
    Token start_token = parser->previous;
    const char* sql_start = start_token.start;
    while (!check(parser, TOKEN_SEMICOLON) && !check(parser, TOKEN_EOF)) {
        advance(parser);
    }
    if (check(parser, TOKEN_EOF)) {
        error_at_current(parser, "expected ';' after SQL statement");
        return NULL;
    }
    int sql_len = (int)(parser->current.start - sql_start);
    char* sql = malloc((size_t)sql_len + 1);
    if (sql == NULL) {
        error_at_current(parser, "out of memory");
        return NULL;
    }
    memcpy(sql, sql_start, (size_t)sql_len);
    sql[sql_len] = '\0';

    Expr** params = NULL;
    int param_count = 0;
    char* out = sql;
    const char* p = sql;
    while (*p != '\0') {
        if (*p == '?') {
            const char* ident = p + 1;
            const char* q = ident;
            while (is_ident_char(*q)) q++;
            if (q > ident) {
                int name_len = (int)(q - ident);
                char* name = malloc((size_t)name_len + 1);
                if (name == NULL) {
                    free(sql);
                    for (int i = 0; i < param_count; i++) free_expr(params[i]);
                    free(params);
                    error_at_current(parser, "out of memory");
                    return NULL;
                }
                memcpy(name, ident, (size_t)name_len);
                name[name_len] = '\0';
                Expr** new_params = realloc(params, sizeof(Expr*) * (size_t)(param_count + 1));
                if (new_params == NULL) {
                    free(name);
                    free(sql);
                    for (int i = 0; i < param_count; i++) free_expr(params[i]);
                    free(params);
                    error_at_current(parser, "out of memory");
                    return NULL;
                }
                params = new_params;
                params[param_count++] = create_sql_param_expr(name);
                free(name);

                /* Copy '?' and skip identifier in output SQL. */
                *out++ = '?';
                p = q;
                continue;
            }
        }
        *out++ = *p++;
    }
    *out = '\0';

    char** into_vars = NULL;
    int into_count = 0;
    char* final_sql = sql;
    if (kind == STMT_SQL_QUERY) {
        const char* into_pos = find_word(sql, "into");
        if (into_pos != NULL) {
            const char* from_pos = find_word(into_pos + 4, "from");
            if (from_pos == NULL) {
                free(sql);
                for (int i = 0; i < param_count; i++) free_expr(params[i]);
                free(params);
                error_at_current(parser, "expected FROM after INTO in SELECT statement");
                return NULL;
            }
            const char* list_start = into_pos + 4;
            while (*list_start != '\0' && isspace((unsigned char)*list_start)) list_start++;
            const char* list_end = from_pos;
            while (list_end > list_start && isspace((unsigned char)list_end[-1])) list_end--;
            if (list_end == list_start) {
                free(sql);
                for (int i = 0; i < param_count; i++) free_expr(params[i]);
                free(params);
                error_at_current(parser, "expected variable name after INTO");
                return NULL;
            }

            const char* p = list_start;
            while (p < list_end) {
                while (p < list_end && isspace((unsigned char)*p)) p++;
                const char* part_start = p;
                while (p < list_end && *p != ',') p++;
                const char* part_end = p;
                while (part_end > part_start && isspace((unsigned char)part_end[-1])) part_end--;
                if (part_end == part_start) {
                    for (int i = 0; i < into_count; i++) free(into_vars[i]);
                    free(into_vars);
                    free(sql);
                    for (int i = 0; i < param_count; i++) free_expr(params[i]);
                    free(params);
                    error_at_current(parser, "expected variable name in INTO list");
                    return NULL;
                }
                int var_len = (int)(part_end - part_start);
                char* var_name = malloc((size_t)var_len + 1);
                if (var_name == NULL) {
                    for (int i = 0; i < into_count; i++) free(into_vars[i]);
                    free(into_vars);
                    free(sql);
                    for (int i = 0; i < param_count; i++) free_expr(params[i]);
                    free(params);
                    error_at_current(parser, "out of memory");
                    return NULL;
                }
                memcpy(var_name, part_start, (size_t)var_len);
                var_name[var_len] = '\0';
                char** new_vars = realloc(into_vars, sizeof(char*) * (size_t)(into_count + 1));
                if (new_vars == NULL) {
                    free(var_name);
                    for (int i = 0; i < into_count; i++) free(into_vars[i]);
                    free(into_vars);
                    free(sql);
                    for (int i = 0; i < param_count; i++) free_expr(params[i]);
                    free(params);
                    error_at_current(parser, "out of memory");
                    return NULL;
                }
                into_vars = new_vars;
                into_vars[into_count++] = var_name;
                if (p < list_end) p++; /* skip comma */
            }

            int bulk_collect = 0;
            const char* bulk_pos = find_word(sql, "bulk");
            if (bulk_pos != NULL && bulk_pos < into_pos) {
                const char* collect_pos = bulk_pos + 4;
                while (*collect_pos != '\0' && isspace((unsigned char)*collect_pos)) collect_pos++;
                if (strncmp(collect_pos, "collect", 7) == 0 || strncmp(collect_pos, "COLLECT", 7) == 0) {
                    bulk_collect = 1;
                }
            }

            int prefix_len = (int)(into_pos - sql);
            if (bulk_collect) {
                /* Trim "bulk collect" and trailing whitespace from prefix. */
                const char* prefix_end = bulk_pos;
                while (prefix_end > sql && isspace((unsigned char)prefix_end[-1])) prefix_end--;
                prefix_len = (int)(prefix_end - sql);
            }
            int suffix_len = (int)strlen(from_pos);
            int new_len = prefix_len + 5 + suffix_len; /* " FROM " + suffix, but keep one space before FROM */
            final_sql = malloc((size_t)new_len + 1);
            if (final_sql == NULL) {
                for (int i = 0; i < into_count; i++) free(into_vars[i]);
                free(into_vars);
                free(sql);
                for (int i = 0; i < param_count; i++) free_expr(params[i]);
                free(params);
                error_at_current(parser, "out of memory");
                return NULL;
            }
            /* Trim trailing whitespace from prefix. */
            while (prefix_len > 0 && isspace((unsigned char)sql[prefix_len - 1])) prefix_len--;
            memcpy(final_sql, sql, (size_t)prefix_len);
            final_sql[prefix_len] = ' ';
            memcpy(final_sql + prefix_len + 1, from_pos, (size_t)suffix_len);
            final_sql[prefix_len + 1 + suffix_len] = '\0';

            Stmt* stmt = create_sql_stmt(kind, final_sql, params, param_count, into_vars, into_count);
            if (stmt != NULL) {
                stmt->as.sql_stmt.bulk_collect = bulk_collect;
                stmt->loc.line = start_token.line;
                stmt->loc.column = start_token.column;
            }
            if (final_sql != sql) free(final_sql);
            free(sql);
            if (stmt == NULL) {
                for (int i = 0; i < param_count; i++) free_expr(params[i]);
                free(params);
                for (int i = 0; i < into_count; i++) free(into_vars[i]);
                free(into_vars);
                return NULL;
            }
            advance(parser); /* consume ; */
            return stmt;
        }
    }

    Stmt* stmt = create_sql_stmt(kind, final_sql, params, param_count, into_vars, into_count);
    if (stmt != NULL) {
        stmt->loc.line = start_token.line;
        stmt->loc.column = start_token.column;
    }
    if (final_sql != sql) free(final_sql);
    free(sql);
    if (stmt == NULL) {
        for (int i = 0; i < param_count; i++) free_expr(params[i]);
        free(params);
        for (int i = 0; i < into_count; i++) free(into_vars[i]);
        free(into_vars);
        error_at_current(parser, "out of memory");
        return NULL;
    }
    advance(parser); /* consume ; */
    return stmt;
}

static Expr* grouping(Parser* parser) {
    Expr* expr = expression(parser);
    advance(parser); /* consume ) */
    return expr;
}

static Expr* unary(Parser* parser) {
    Token op = parser->previous;
    Expr* operand = parse_precedence(parser, PREC_UNARY);
    Expr* expr = create_unary_expr(op.type, operand);
    expr->loc.line = op.line;
    expr->loc.column = op.column;
    return expr;
}

/* Convert a raw string literal body (without quotes) into an unescaped
   C string. The returned pointer must be freed by the caller. */
static char* unescape_string(const char* raw, int len) {
    char* out = malloc((size_t)len + 1);
    if (out == NULL) return NULL;
    int j = 0;
    for (int i = 0; i < len; i++) {
        if (raw[i] == '\\' && i + 1 < len) {
            switch (raw[i + 1]) {
                case 'n': out[j++] = '\n'; i++; break;
                case 't': out[j++] = '\t'; i++; break;
                case 'r': out[j++] = '\r'; i++; break;
                case '\\': out[j++] = '\\'; i++; break;
                case '"': out[j++] = '"'; i++; break;
                default: out[j++] = raw[i]; break;
            }
        } else {
            out[j++] = raw[i];
        }
    }
    out[j] = '\0';
    return out;
}

static Expr* literal_bool(Parser* parser) {
    int v = parser->previous.type == TOKEN_TRUE ? 1 : 0;
    Expr* expr = create_literal_expr(value_bool(v));
    expr->loc.line = parser->previous.line;
    expr->loc.column = parser->previous.column;
    return expr;
}

static Expr* literal_null(Parser* parser) {
    Expr* expr = create_literal_expr(value_null());
    expr->loc.line = parser->previous.line;
    expr->loc.column = parser->previous.column;
    return expr;
}

static Expr* number(Parser* parser) {
    Expr* expr;
    if (parser->previous.type == TOKEN_FLOAT) {
        char buffer[64];
        int len = parser->previous.length;
        if (len >= 64) len = 63;
        memcpy(buffer, parser->previous.start, (size_t)len);
        buffer[len] = '\0';
        expr = create_literal_expr(value_float(strtod(buffer, NULL)));
    } else if (parser->previous.type == TOKEN_STRING) {
        int len = parser->previous.length - 2; /* without quotes */
        if (len < 0) len = 0;
        char* str = unescape_string(parser->previous.start + 1, len);
        if (str == NULL) return create_literal_expr(value_int(0));
        expr = create_literal_expr(value_string(str));
    } else {
        int value = 0;
        for (int i = 0; i < parser->previous.length; i++) {
            int digit = parser->previous.start[i] - '0';
            if (value > (INT_MAX - digit) / 10) {
                error_at_previous(parser, "integer literal too large");
                return NULL;
            }
            value = value * 10 + digit;
        }
        expr = create_literal_expr(value_int(value));
    }
    expr->loc.line = parser->previous.line;
    expr->loc.column = parser->previous.column;
    return expr;
}

static Expr* struct_literal(Parser* parser, char* struct_name) {
    advance(parser); /* { */
    char** field_names = NULL;
    Expr** values = NULL;
    int field_count = 0;
    if (!check(parser, TOKEN_RBRACE)) {
        do {
            if (!check(parser, TOKEN_IDENT)) {
                error_at_current(parser, "expected field name");
                for (int i = 0; i < field_count; i++) {
                    free(field_names[i]);
                    free_expr(values[i]);
                }
                free(field_names);
                free(values);
                free(struct_name);
                return NULL;
            }
            advance(parser); /* field name */
            char** new_names = malloc(sizeof(char*) * (size_t)(field_count + 1));
            Expr** new_values = malloc(sizeof(Expr*) * (size_t)(field_count + 1));
            if (new_names == NULL || new_values == NULL) {
                for (int i = 0; i < field_count; i++) {
                    free(field_names[i]);
                    free_expr(values[i]);
                }
                free(field_names);
                free(values);
                free(new_names);
                free(new_values);
                free(struct_name);
                return NULL;
            }
            if (field_count > 0) {
                memcpy(new_names, field_names, sizeof(char*) * (size_t)field_count);
                memcpy(new_values, values, sizeof(Expr*) * (size_t)field_count);
                free(field_names);
                free(values);
            }
            field_names = new_names;
            values = new_values;
            field_names[field_count] = copy_token_lexeme(&parser->previous);
            advance(parser); /* = */
            values[field_count] = expression(parser);
            field_count++;
        } while (match(parser, TOKEN_COMMA));
    }
    if (!match(parser, TOKEN_RBRACE)) {
        error_at_current(parser, "expected '}' after struct literal");
        for (int i = 0; i < field_count; i++) {
            free(field_names[i]);
            free_expr(values[i]);
        }
        free(field_names);
        free(values);
        free(struct_name);
        return NULL;
    }
    Expr* expr = create_struct_literal_expr(struct_name, field_names, values, field_count);
    free(struct_name);
    return expr;
}

static Expr* map_literal(Parser* parser) {
    /* '{' was already consumed by the prefix dispatcher */
    Expr** keys = NULL;
    Expr** values = NULL;
    int count = 0;
    if (!check(parser, TOKEN_RBRACE)) {
        do {
            Expr** new_keys = malloc(sizeof(Expr*) * (size_t)(count + 1));
            Expr** new_values = malloc(sizeof(Expr*) * (size_t)(count + 1));
            if (new_keys == NULL || new_values == NULL) {
                for (int i = 0; i < count; i++) {
                    free_expr(keys[i]);
                    free_expr(values[i]);
                }
                free(keys);
                free(values);
                free(new_keys);
                free(new_values);
                return NULL;
            }
            if (count > 0) {
                memcpy(new_keys, keys, sizeof(Expr*) * (size_t)count);
                memcpy(new_values, values, sizeof(Expr*) * (size_t)count);
                free(keys);
                free(values);
            }
            keys = new_keys;
            values = new_values;
            keys[count] = expression(parser);
            if (!match(parser, TOKEN_COLON)) {
                error_at_current(parser, "expected ':' after map key");
                /* values[count] has not been parsed yet. */
                for (int i = 0; i < count; i++) {
                    free_expr(keys[i]);
                    free_expr(values[i]);
                }
                free_expr(keys[count]);
                free(keys);
                free(values);
                return NULL;
            }
            values[count] = expression(parser);
            count++;
        } while (match(parser, TOKEN_COMMA));
    }
    if (!match(parser, TOKEN_RBRACE)) {
        error_at_current(parser, "expected '}' after map literal");
        for (int i = 0; i < count; i++) {
            free_expr(keys[i]);
            free_expr(values[i]);
        }
        free(keys);
        free(values);
        return NULL;
    }
    Expr* expr = create_map_literal_expr(keys, values, count);
    expr->loc.line = parser->previous.line;
    expr->loc.column = parser->previous.column;
    return expr;
}

static Expr* variable(Parser* parser) {
    char* name = copy_token_lexeme(&parser->previous);
    if (check(parser, TOKEN_LBRACE) && parser_is_struct_name(parser, name)) {
        return struct_literal(parser, name);
    }
    Expr* expr = create_variable_expr(name);
    free(name);
    expr->loc.line = parser->previous.line;
    expr->loc.column = parser->previous.column;
    return expr;
}

/* ':new' / ':old' row references inside row-level trigger bodies. Parses as
   a plain variable named "new"/"old"; the following `.field` is handled by the
   normal field infix rule. The variables are only in scope while compiling a
   FOR EACH ROW trigger body (typechecker/codegen reject them elsewhere). */
static Expr* trigger_row_ref(Parser* parser) {
    Token colon = parser->previous; /* ':' */
    if (!check(parser, TOKEN_IDENT) ||
        (!current_token_is_keyword(parser, "new") &&
         !current_token_is_keyword(parser, "old"))) {
        error_at_current(parser, "expected ':new' or ':old' after ':'");
        Expr* dummy = create_literal_expr(value_int(0));
        dummy->loc.line = colon.line;
        dummy->loc.column = colon.column;
        return dummy;
    }
    advance(parser); /* new / old */
    char* name = copy_token_lexeme(&parser->previous);
    Expr* expr = create_variable_expr(name);
    free(name);
    expr->loc.line = colon.line;
    expr->loc.column = colon.column;
    return expr;
}

static Expr* sqlcode_expr(Parser* parser) {
    Expr* expr = create_call_expr("sqlcode", NULL, 0);
    if (expr != NULL) {
        expr->loc.line = parser->previous.line;
        expr->loc.column = parser->previous.column;
    }
    return expr;
}

static Expr* sqlerrm_expr(Parser* parser) {
    Expr* expr = create_call_expr("sqlerrm", NULL, 0);
    if (expr != NULL) {
        expr->loc.line = parser->previous.line;
        expr->loc.column = parser->previous.column;
    }
    return expr;
}

static Expr* field(Parser* parser, Expr* left) {
    advance(parser); /* field name */
    char* field_name = copy_token_lexeme(&parser->previous);

    if (check(parser, TOKEN_LPAREN)) {
        if (is_collection_method_name(field_name)) {
            /* Collection method call: receiver.method(args) -> method(receiver, args) */
            advance(parser); /* ( */
            Expr** args = malloc(sizeof(Expr*));
            int arg_count = 0;
            if (args == NULL) {
                free(field_name);
                free_expr(left);
                return NULL;
            }
            args[arg_count++] = left;
            if (!check(parser, TOKEN_RPAREN)) {
                do {
                    Expr** new_args = realloc(args, sizeof(Expr*) * (size_t)(arg_count + 1));
                    if (new_args == NULL) {
                        for (int i = 0; i < arg_count; i++) free_expr(args[i]);
                        free(args);
                        free(field_name);
                        return NULL;
                    }
                    args = new_args;
                    args[arg_count++] = expression(parser);
                } while (match(parser, TOKEN_COMMA));
            }
            advance(parser); /* ) */
            const char* call_name = strcmp(field_name, "trim") == 0 ? "array_trim" : field_name;
            Expr* expr = create_call_expr(call_name, args, arg_count);
            free(field_name);
            expr->loc.line = parser->previous.line;
            expr->loc.column = parser->previous.column;
            return expr;
        }
        if (left->kind == EXPR_VARIABLE) {
            /* Qualified package call: pkg.member(...) */
            advance(parser); /* ( */
            Expr** args = NULL;
            int arg_count = 0;
            if (!check(parser, TOKEN_RPAREN)) {
                do {
                    Expr** new_args = realloc(args, sizeof(Expr*) * (size_t)(arg_count + 1));
                    if (new_args == NULL) {
                        for (int i = 0; i < arg_count; i++) free_expr(args[i]);
                        free(args);
                        free(field_name);
                        free_expr(left);
                        return NULL;
                    }
                    args = new_args;
                    args[arg_count++] = expression(parser);
                } while (match(parser, TOKEN_COMMA));
            }
            advance(parser); /* ) */
            Expr* expr = create_qualified_call_expr(left->as.variable.name, field_name, args, arg_count);
            free(field_name);
            free_expr(left);
            if (expr != NULL) {
                expr->loc.line = parser->previous.line;
                expr->loc.column = parser->previous.column;
            }
            return expr;
        }
    }

    Expr* expr;
    if (left->kind == EXPR_VARIABLE && strcmp(left->as.variable.name, "row") == 0) {
        expr = create_field_expr(left->as.variable.name, field_name);
        free_expr(left);
    } else {
        expr = create_row_field_expr(left, field_name);
    }
    expr->loc.line = parser->previous.line;
    expr->loc.column = parser->previous.column;
    free(field_name);
    return expr;
}

static Expr* cursor_attr(Parser* parser, Expr* left) {
    advance(parser); /* attribute name */
    char* attr_name = copy_token_lexeme(&parser->previous);
    Expr* expr = NULL;
    if (left->kind == EXPR_VARIABLE) {
        expr = create_cursor_attr_expr(left->as.variable.name, attr_name);
        free_expr(left);
    } else {
        error_at_previous(parser, "cursor attribute requires a cursor variable");
        free_expr(left);
    }
    free(attr_name);
    if (expr != NULL) {
        expr->loc.line = parser->previous.line;
        expr->loc.column = parser->previous.column;
    }
    return expr;
}

static Expr* call(Parser* parser, Expr* left) {
    if (left->kind != EXPR_VARIABLE) {
        error_at_current(parser, "can only call named procedures");
        return left;
    }

    Expr** args = NULL;
    int arg_count = 0;
    if (!check(parser, TOKEN_RPAREN)) {
        do {
            Expr** new_args = realloc(args, sizeof(Expr*) * (size_t)(arg_count + 1));
            if (new_args == NULL) {
                for (int i = 0; i < arg_count; i++) free_expr(args[i]);
                free(args);
                free_expr(left);
                return NULL;
            }
            args = new_args;
            args[arg_count++] = expression(parser);
        } while (match(parser, TOKEN_COMMA));
    }
    advance(parser); /* consume ) */

    Expr* expr = create_call_expr(left->as.variable.name, args, arg_count);
    expr->loc.line = parser->previous.line;
    expr->loc.column = parser->previous.column;
    free_expr(left);
    return expr;
}

static Expr* array_literal(Parser* parser) {
    Expr** elements = NULL;
    int count = 0;
    if (!check(parser, TOKEN_RBRACKET)) {
        do {
            Expr** new_elements = realloc(elements, sizeof(Expr*) * (size_t)(count + 1));
            if (new_elements == NULL) {
                for (int i = 0; i < count; i++) {
                    free_expr(elements[i]);
                }
                free(elements);
                return NULL;
            }
            elements = new_elements;
            elements[count++] = expression(parser);
        } while (match(parser, TOKEN_COMMA));
    }
    advance(parser); /* ] */
    Expr* expr = create_array_expr(elements, count);
    expr->loc.line = parser->previous.line;
    expr->loc.column = parser->previous.column;
    return expr;
}

static Expr* index_expr(Parser* parser, Expr* left) {
    Expr* idx = expression(parser);
    advance(parser); /* ] */
    Expr* expr = create_index_expr(left, idx);
    expr->loc.line = parser->previous.line;
    expr->loc.column = parser->previous.column;
    return expr;
}

static Expr* binary(Parser* parser, Expr* left) {
    Token op = parser->previous;
    ParseRule* rule = get_rule(op.type);
    Expr* right = parse_precedence(parser, (Precedence)(rule->precedence + 1));
    Expr* expr = create_binary_expr(op.type, left, right);
    expr->loc.line = op.line;
    expr->loc.column = op.column;
    return expr;
}

static Expr* parse_precedence(Parser* parser, Precedence precedence) {
    advance(parser);
    PrefixFn prefix_rule = get_rule(parser->previous.type)->prefix;
    if (prefix_rule == NULL) {
        error_at_previous(parser, "expected expression");
        return NULL;
    }

    Expr* left = prefix_rule(parser);

    /* Infix rules dereference left, so stop once an operand failed. */
    while (left != NULL && !parser->had_error &&
           precedence <= get_rule(parser->current.type)->precedence) {
        advance(parser);
        InfixFn infix_rule = get_rule(parser->previous.type)->infix;
        left = infix_rule(parser, left);
    }

    return left;
}

static Expr* expression(Parser* parser) {
    return parse_precedence(parser, PREC_ASSIGNMENT);
}

static Type* parse_type(Parser* parser) {
    if (match(parser, TOKEN_INT_TYPE)) return &type_int;
    if (match(parser, TOKEN_FLOAT_TYPE)) return &type_float;
    if (match(parser, TOKEN_STRING_TYPE)) return &type_string;
    if (match(parser, TOKEN_BOOL_TYPE)) return &type_bool;
    if (match(parser, TOKEN_ANY_TYPE)) return &type_unknown;
    if (match(parser, TOKEN_DATE_TYPE)) return &type_date;
    if (match(parser, TOKEN_TIMESTAMP_TYPE)) return &type_timestamp;
    if (match(parser, TOKEN_CURSOR)) return &type_cursor;
    if (check(parser, TOKEN_IDENT) && parser->current.length == 3 &&
        strncmp(parser->current.start, "row", 3) == 0) {
        advance(parser);
        return &type_row;
    }
    if (check(parser, TOKEN_IDENT)) {
        char* name = copy_token_lexeme(&parser->current);
        advance(parser);
        if (match(parser, TOKEN_DOT)) {
            if (!check(parser, TOKEN_IDENT)) {
                error_at_current(parser, "expected column name after '.'");
                free(name);
                return &type_int;
            }
            char* column = copy_token_lexeme(&parser->current);
            advance(parser);
            if (!match(parser, TOKEN_PERCENT)) {
                error_at_current(parser, "expected '%' after 'table.column'");
                free(name);
                free(column);
                return &type_int;
            }
            if (!match(parser, TOKEN_TYPE)) {
                error_at_current(parser, "expected 'type' after 'table.column%'");
                free(name);
                free(column);
                return &type_int;
            }
            Type* t = type_percent_type_column(name, column);
            free(name);
            free(column);
            return t != NULL ? t : &type_int;
        }
        if (match(parser, TOKEN_PERCENT)) {
            if (match(parser, TOKEN_TYPE)) {
                Type* t = type_percent_type_var(name);
                free(name);
                return t != NULL ? t : &type_int;
            }
            if (match(parser, TOKEN_ROWTYPE)) {
                Type* t = type_percent_rowtype(name);
                free(name);
                return t != NULL ? t : &type_int;
            }
            error_at_current(parser, "expected 'type' or 'rowtype' after '%'");
            free(name);
            return &type_int;
        }
        Type* t = type_new(TYPE_STRUCT, NULL);
        if (t == NULL) {
            error_at_current(parser, "out of memory");
            free(name);
            return &type_int;
        }
        t->struct_name = name;
        return t;
    }
    if (match(parser, TOKEN_ARRAY_TYPE)) {
        if (match(parser, TOKEN_LT)) {
            Type* element = parse_type(parser);
            if (!match(parser, TOKEN_GT)) {
                error_at_current(parser, "expected '>' after array element type");
            }
            return type_new(TYPE_ARRAY, element);
        }
        return type_new(TYPE_ARRAY, NULL);  /* generic array */
    }
    if (match(parser, TOKEN_MAP_TYPE)) {
        Type* key_type = &type_string;
        Type* value_type = &type_unknown;
        if (match(parser, TOKEN_LT)) {
            key_type = parse_type(parser);
            if (key_type == NULL || (key_type->kind != TYPE_STRING && key_type->kind != TYPE_INT)) {
                error_at_current(parser, "map key type must be int or string");
            }
            if (!match(parser, TOKEN_COMMA)) {
                error_at_current(parser, "expected ',' between map key and value types");
            }
            value_type = parse_type(parser);
            if (!match(parser, TOKEN_GT)) {
                error_at_current(parser, "expected '>' after map value type");
            }
        }
        /* The map takes over key_type and value_type; copying them here would
           leave the parsed originals unowned. */
        Type* map = type_new_map(key_type, value_type);
        if (map == NULL) {
            error_at_current(parser, "out of memory");
            type_free(key_type);
            type_free(value_type);
            return &type_int;
        }
        return map;
    }
    error_at_current(parser, "expected type");
    return &type_int;
}

static Stmt* var_decl(Parser* parser);
static Stmt* statement(Parser* parser);

static Block* block(Parser* parser) {
    Block* result = create_block();
    advance(parser); /* { */
    while (!check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF) &&
           !parser->had_error) {
        Stmt** new_stmts = realloc(result->stmts,
            sizeof(Stmt*) * (size_t)(result->stmt_count + 1));
        result->stmts = new_stmts;
        result->stmts[result->stmt_count++] = statement(parser);
    }
    if (parser->had_error) {
        free_block(result);
        return NULL;
    }
    if (!check(parser, TOKEN_RBRACE)) {
        error_at_current(parser, "expected '}'");
        free_block(result);
        return NULL;
    }
    advance(parser); /* } */
    return result;
}

static Stmt* var_decl(Parser* parser) {
    Type* type = parse_type(parser);
    advance(parser); /* identifier */
    int id_line = parser->previous.line;
    int id_column = parser->previous.column;
    char* name = copy_token_lexeme(&parser->previous);
    Expr* init = NULL;
    if (match(parser, TOKEN_ASSIGN)) {
        init = expression(parser);
    }
    advance(parser); /* semicolon */
    Stmt* stmt = create_var_decl_stmt(type, name, init);
    stmt->loc.line = id_line;
    stmt->loc.column = id_column;
    free(name);
    return stmt;
}

static Stmt* assignment(Parser* parser) {
    advance(parser); /* identifier */
    int id_line = parser->previous.line;
    int id_column = parser->previous.column;
    char* name = copy_token_lexeme(&parser->previous);

    if (check(parser, TOKEN_DOT)) {
        advance(parser); /* . */
        int method_name_token = parser->current.type;
        if (method_name_token != TOKEN_IDENT && method_name_token != TOKEN_DELETE) {
            error_at_current(parser, "expected field or member name after '.'");
            free(name);
            return NULL;
        }
        advance(parser); /* field/member name */
        char* field_name = copy_token_lexeme(&parser->previous);
        Stmt* stmt = NULL;
        if (check(parser, TOKEN_LPAREN)) {
            if (is_collection_method_name(field_name)) {
                /* Collection method statement: receiver.method(args); */
                advance(parser); /* ( */
                Expr** args = malloc(sizeof(Expr*));
                int arg_count = 0;
                if (args == NULL) {
                    free(field_name);
                    free(name);
                    return NULL;
                }
                args[arg_count++] = create_variable_expr(name);
                if (!check(parser, TOKEN_RPAREN)) {
                    do {
                        Expr** new_args = realloc(args, sizeof(Expr*) * (size_t)(arg_count + 1));
                        if (new_args == NULL) {
                            for (int i = 0; i < arg_count; i++) free_expr(args[i]);
                            free(args);
                            free(field_name);
                            free(name);
                            return NULL;
                        }
                        args = new_args;
                        args[arg_count++] = expression(parser);
                    } while (match(parser, TOKEN_COMMA));
                }
                advance(parser); /* ) */
                const char* call_name = strcmp(field_name, "trim") == 0 ? "array_trim" : field_name;
                Expr* call_expr = create_call_expr(call_name, args, arg_count);
                free(field_name);
                call_expr->loc.line = parser->previous.line;
                call_expr->loc.column = parser->previous.column;
                stmt = create_expr_stmt(call_expr);
                free(name);
            } else {
                advance(parser); /* ( */
                Expr* left = create_variable_expr(name);
                Expr* call_expr = call(parser, left);
                if (call_expr != NULL && call_expr->kind == EXPR_CALL) {
                    free(call_expr->as.call.package_name);
                    free(call_expr->as.call.name);
                    call_expr->as.call.package_name = name;
                    call_expr->as.call.name = field_name;
                    stmt = create_expr_stmt(call_expr);
                } else {
                    free_expr(call_expr);
                    free(field_name);
                }
            }
        } else {
            Expr* left = create_variable_expr(name);
            free(name);
            name = NULL;
            if (match(parser, TOKEN_ASSIGN)) {
                Expr* value = expression(parser);
                stmt = create_field_assign_stmt(left, field_name, value);
                free(field_name);
            } else {
                Expr* field_expr = create_row_field_expr(left, field_name);
                free(field_name);
                stmt = create_expr_stmt(field_expr);
            }
        }
        if (stmt == NULL) {
            free(name);
            return NULL;
        }
        if (!check(parser, TOKEN_SEMICOLON)) {
            error_at_current(parser, "expected ';' after statement");
            free_stmt(stmt);
            return NULL;
        }
        advance(parser); /* ; */
        stmt->loc.line = id_line;
        stmt->loc.column = id_column;
        return stmt;
    }

    if (check(parser, TOKEN_LPAREN)) {
        advance(parser); /* ( */
        Expr* call_expr = call(parser, create_variable_expr(name));
        advance(parser); /* ; */
        Stmt* stmt = create_expr_stmt(call_expr);
        stmt->loc.line = call_expr->loc.line;
        stmt->loc.column = call_expr->loc.column;
        free(name);
        return stmt;
    }

    if (match(parser, TOKEN_LBRACKET)) {
        Expr* array_expr = create_variable_expr(name);
        Expr* index_expr = expression(parser);
        advance(parser); /* ] */
        while (match(parser, TOKEN_LBRACKET)) {
            array_expr = create_index_expr(array_expr, index_expr);
            index_expr = expression(parser);
            advance(parser); /* ] */
        }
        advance(parser); /* = */
        Expr* value = expression(parser);
        advance(parser); /* ; */
        Stmt* stmt = create_index_assign_stmt(array_expr, index_expr, value);
        stmt->loc.line = id_line;
        stmt->loc.column = id_column;
        free(name);
        return stmt;
    }

    advance(parser); /* = */
    Expr* value = expression(parser);
    advance(parser); /* ; */
    Stmt* stmt = create_assign_stmt(name, value);
    stmt->loc.line = id_line;
    stmt->loc.column = id_column;
    free(name);
    return stmt;
}

static Stmt* if_statement(Parser* parser) {
    Token kw = parser->previous;  /* 'if' was just consumed */
    Expr* cond = expression(parser);
    Block* then_block = block(parser);
    if (then_block == NULL) {
        free_expr(cond);
        return NULL;
    }
    Block* else_block = NULL;
    if (match(parser, TOKEN_ELSE)) {
        if (check(parser, TOKEN_IF)) {
            advance(parser); /* consume 'if' */
            Stmt* nested = if_statement(parser);
            if (nested == NULL) {
                free_expr(cond);
                free_block(then_block);
                return NULL;
            }
            else_block = create_block();
            if (else_block == NULL) {
                free_stmt(nested);
                free_expr(cond);
                free_block(then_block);
                return NULL;
            }
            Stmt** stmts = malloc(sizeof(Stmt*));
            if (stmts == NULL) {
                free_block(else_block);
                free_stmt(nested);
                free_expr(cond);
                free_block(then_block);
                return NULL;
            }
            stmts[0] = nested;
            else_block->stmts = stmts;
            else_block->stmt_count = 1;
        } else {
            else_block = block(parser);
            if (else_block == NULL) {
                free_expr(cond);
                free_block(then_block);
                return NULL;
            }
        }
    }
    Stmt* stmt = create_if_stmt(cond, then_block, else_block);
    stmt->loc.line = kw.line;
    stmt->loc.column = kw.column;
    return stmt;
}

static Stmt* while_statement(Parser* parser) {
    Token kw = parser->previous;  /* 'while' was just consumed */
    Expr* cond = expression(parser);
    Block* body = block(parser);
    if (body == NULL) {
        free_expr(cond);
        return NULL;
    }
    Stmt* stmt = create_while_stmt(cond, body);
    stmt->loc.line = kw.line;
    stmt->loc.column = kw.column;
    return stmt;
}

static Stmt* do_while_statement(Parser* parser) {
    Token kw = parser->previous;  /* 'do' was just consumed */
    Block* body = block(parser);
    if (body == NULL) return NULL;
    if (!check(parser, TOKEN_WHILE)) {
        error_at_current(parser, "expected 'while' after 'do' body");
        free_block(body);
        return NULL;
    }
    advance(parser); /* consume 'while' */
    Expr* cond = expression(parser);
    if (cond == NULL) {
        free_block(body);
        return NULL;
    }
    if (!check(parser, TOKEN_SEMICOLON)) {
        error_at_current(parser, "expected ';' after do-while condition");
        free_expr(cond);
        free_block(body);
        return NULL;
    }
    advance(parser); /* consume ';' */
    Stmt* stmt = create_do_while_stmt(cond, body);
    stmt->loc.line = kw.line;
    stmt->loc.column = kw.column;
    return stmt;
}

static Stmt* break_statement(Parser* parser) {
    Token kw = parser->previous;  /* 'break' was just consumed */
    advance(parser); /* ; */
    Stmt* stmt = create_break_stmt();
    stmt->loc.line = kw.line;
    stmt->loc.column = kw.column;
    return stmt;
}

static Stmt* continue_statement(Parser* parser) {
    Token kw = parser->previous;  /* 'continue' was just consumed */
    advance(parser); /* ; */
    Stmt* stmt = create_continue_stmt();
    stmt->loc.line = kw.line;
    stmt->loc.column = kw.column;
    return stmt;
}

static Stmt* try_statement(Parser* parser) {
    Token kw = parser->previous;  /* 'try' was just consumed */
    Block* try_block = block(parser);
    if (try_block == NULL) return NULL;
    if (!match(parser, TOKEN_CATCH)) {
        error_at_current(parser, "expected 'catch' after 'try' body");
        free_block(try_block);
        return NULL;
    }
    if (!match(parser, TOKEN_LPAREN)) {
        error_at_current(parser, "expected '(' after 'catch'");
        free_block(try_block);
        return NULL;
    }
    if (!check(parser, TOKEN_IDENT)) {
        error_at_current(parser, "expected catch variable name");
        free_block(try_block);
        return NULL;
    }
    advance(parser); /* catch variable */
    char* catch_var = copy_token_lexeme(&parser->previous);
    if (catch_var == NULL) {
        free_block(try_block);
        return NULL;
    }
    if (!match(parser, TOKEN_RPAREN)) {
        error_at_current(parser, "expected ')' after catch variable");
        free(catch_var);
        free_block(try_block);
        return NULL;
    }
    Block* catch_block = block(parser);
    if (catch_block == NULL) {
        free(catch_var);
        free_block(try_block);
        return NULL;
    }
    Stmt* stmt = create_try_catch_stmt(try_block, catch_var, catch_block);
    if (stmt != NULL) {
        stmt->loc.line = kw.line;
        stmt->loc.column = kw.column;
    }
    free(catch_var);
    return stmt;
}

static Stmt* exception_decl_statement(Parser* parser) {
    /* current is IDENT (exception name); caller verified next is 'exception' */
    advance(parser); /* exception name */
    int line = parser->previous.line;
    int column = parser->previous.column;
    char* name = copy_token_lexeme(&parser->previous);
    if (name == NULL) return NULL;
    advance(parser); /* 'exception' keyword */
    if (!check(parser, TOKEN_SEMICOLON)) {
        error_at_current(parser, "expected ';' after exception declaration");
        free(name);
        return NULL;
    }
    advance(parser); /* semicolon */
    Stmt* stmt = create_exception_decl_stmt(name);
    if (stmt != NULL) {
        stmt->loc.line = line;
        stmt->loc.column = column;
    }
    free(name);
    return stmt;
}

static Stmt* raise_statement(Parser* parser) {
    Token kw = parser->previous;  /* 'raise' was just consumed */
    if (!check(parser, TOKEN_IDENT)) {
        error_at_current(parser, "expected exception name after 'raise'");
        return NULL;
    }
    advance(parser); /* exception name */
    char* name = copy_token_lexeme(&parser->previous);
    if (name == NULL) return NULL;
    if (!check(parser, TOKEN_SEMICOLON)) {
        error_at_current(parser, "expected ';' after raise");
        free(name);
        return NULL;
    }
    advance(parser); /* semicolon */
    Stmt* stmt = create_raise_stmt(name);
    if (stmt != NULL) {
        stmt->loc.line = kw.line;
        stmt->loc.column = kw.column;
    }
    free(name);
    return stmt;
}

static Stmt* subtype_decl_statement(Parser* parser) {
    Token kw = parser->previous;  /* 'subtype' was just consumed */
    if (!check(parser, TOKEN_IDENT)) {
        error_at_current(parser, "expected subtype name after 'subtype'");
        return NULL;
    }
    advance(parser); /* name */
    char* name = copy_token_lexeme(&parser->previous);
    if (!match(parser, TOKEN_IS)) {
        error_at_current(parser, "expected 'is' after subtype name");
        free(name);
        return NULL;
    }
    Type* base_type = parse_type(parser);
    if (parser->had_error) {
        free(name);
        type_free(base_type);
        return NULL;
    }
    if (!match(parser, TOKEN_SEMICOLON)) {
        error_at_current(parser, "expected ';' after subtype declaration");
        free(name);
        type_free(base_type);
        return NULL;
    }
    Stmt* stmt = create_subtype_decl_stmt(name, base_type);
    if (stmt != NULL) {
        stmt->loc.line = kw.line;
        stmt->loc.column = kw.column;
    }
    free(name);
    return stmt;
}

static Stmt* case_statement(Parser* parser) {
    Token kw = parser->previous;  /* 'case' was just consumed */
    Expr* selector = expression(parser);
    if (selector == NULL) return NULL;
    if (!match(parser, TOKEN_LBRACE)) {
        error_at_current(parser, "expected '{' after 'case' selector");
        free_expr(selector);
        return NULL;
    }

    int capacity = 4;
    int count = 0;
    Expr** values = malloc(sizeof(Expr*) * (size_t)capacity);
    Block** blocks = malloc(sizeof(Block*) * (size_t)capacity);
    if (values == NULL || blocks == NULL) {
        free_expr(selector);
        free(values);
        free(blocks);
        error_at_current(parser, "out of memory");
        return NULL;
    }

    Block* else_block = NULL;
    while (!check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
        if (match(parser, TOKEN_ELSE)) {
            if (!match(parser, TOKEN_COLON)) {
                error_at_current(parser, "expected ':' after 'else'");
                goto cleanup;
            }
            else_block = block(parser);
            if (else_block == NULL) goto cleanup;
            break;
        }
        if (!match(parser, TOKEN_WHEN)) {
            error_at_current(parser, "expected 'when' or 'else' in 'case' body");
            goto cleanup;
        }
        Expr* value = expression(parser);
        if (value == NULL) goto cleanup;
        if (!match(parser, TOKEN_COLON)) {
            error_at_current(parser, "expected ':' after 'when' value");
            free_expr(value);
            goto cleanup;
        }
        Block* branch_block = block(parser);
        if (branch_block == NULL) {
            free_expr(value);
            goto cleanup;
        }
        if (count >= capacity) {
            int new_capacity = capacity * 2;
            Expr** new_values = realloc(values, sizeof(Expr*) * (size_t)new_capacity);
            Block** new_blocks = realloc(blocks, sizeof(Block*) * (size_t)new_capacity);
            if (new_values == NULL || new_blocks == NULL) {
                free_expr(value);
                free_block(branch_block);
                error_at_current(parser, "out of memory");
                goto cleanup;
            }
            values = new_values;
            blocks = new_blocks;
            capacity = new_capacity;
        }
        values[count] = value;
        blocks[count] = branch_block;
        count++;
    }

    if (!match(parser, TOKEN_RBRACE)) {
        error_at_current(parser, "expected '}' after 'case' body");
        goto cleanup;
    }

    Stmt* stmt = create_case_stmt(selector, values, blocks, count, else_block);
    if (stmt != NULL) {
        stmt->loc.line = kw.line;
        stmt->loc.column = kw.column;
    }
    return stmt;

cleanup:
    free_expr(selector);
    for (int i = 0; i < count; i++) {
        free_expr(values[i]);
        free_block(blocks[i]);
    }
    free(values);
    free(blocks);
    free_block(else_block);
    return NULL;
}

static Stmt* sql_for_statement(Parser* parser, Token kw, char* var_name) {
    Token sql_token = parser->current;
    advance(parser); /* SQL query */

    int sql_len = sql_token.length;
    while (sql_len > 0) {
        char c = sql_token.start[sql_len - 1];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
        sql_len--;
    }
    char* sql = malloc((size_t)sql_len + 1);
    if (sql == NULL) {
        free(var_name);
        error_at_current(parser, "out of memory");
        return NULL;
    }
    memcpy(sql, sql_token.start, (size_t)sql_len);
    sql[sql_len] = '\0';

    Expr** params = NULL;
    int param_count = 0;
    char* out = sql;
    const char* p = sql;
    while (*p != '\0') {
        if (*p == '?') {
            const char* ident = p + 1;
            const char* q = ident;
            while (is_ident_char(*q)) q++;
            if (q > ident) {
                int name_len = (int)(q - ident);
                char* name = malloc((size_t)name_len + 1);
                if (name == NULL) {
                    free(sql);
                    for (int i = 0; i < param_count; i++) free_expr(params[i]);
                    free(params);
                    free(var_name);
                    error_at_current(parser, "out of memory");
                    return NULL;
                }
                memcpy(name, ident, (size_t)name_len);
                name[name_len] = '\0';
                Expr** new_params = realloc(params, sizeof(Expr*) * (size_t)(param_count + 1));
                if (new_params == NULL) {
                    free(name);
                    free(sql);
                    for (int i = 0; i < param_count; i++) free_expr(params[i]);
                    free(params);
                    free(var_name);
                    error_at_current(parser, "out of memory");
                    return NULL;
                }
                params = new_params;
                params[param_count++] = create_sql_param_expr(name);
                free(name);

                *out++ = '?';
                p = q;
                continue;
            }
        }
        *out++ = *p++;
    }
    *out = '\0';

    Block* body = block(parser);
    if (body == NULL) {
        free(var_name);
        free(sql);
        for (int i = 0; i < param_count; i++) free_expr(params[i]);
        free(params);
        return NULL;
    }
    Stmt* stmt = create_for_stmt(var_name, sql, params, param_count, body);
    stmt->loc.line = kw.line;
    stmt->loc.column = kw.column;
    free(var_name);
    free(sql);
    return stmt;
}

static Stmt* for_step(Parser* parser);

static Stmt* cfor_statement(Parser* parser, Token kw) {
    /* '(' was already consumed */
    Stmt* init = NULL;
    if (!check(parser, TOKEN_SEMICOLON)) {
        init = statement(parser);
        if (init == NULL) return NULL;
    } else {
        advance(parser); /* consume ';' for empty init */
    }

    Expr* cond = NULL;
    if (!check(parser, TOKEN_SEMICOLON)) {
        cond = expression(parser);
        if (cond == NULL) {
            if (init != NULL) free_stmt(init);
            return NULL;
        }
    }
    if (!check(parser, TOKEN_SEMICOLON)) {
        error_at_current(parser, "expected ';' after for condition");
        if (init != NULL) free_stmt(init);
        if (cond != NULL) free_expr(cond);
        return NULL;
    }
    advance(parser); /* consume ';' */

    Stmt* step = NULL;
    if (!check(parser, TOKEN_RPAREN)) {
        step = for_step(parser);
        if (step == NULL) {
            if (init != NULL) free_stmt(init);
            if (cond != NULL) free_expr(cond);
            return NULL;
        }
    }
    if (!check(parser, TOKEN_RPAREN)) {
        error_at_current(parser, "expected ')' after for clauses");
        if (init != NULL) free_stmt(init);
        if (cond != NULL) free_expr(cond);
        if (step != NULL) free_stmt(step);
        return NULL;
    }
    advance(parser); /* consume ')' */

    Block* body = block(parser);
    if (body == NULL) {
        if (init != NULL) free_stmt(init);
        if (cond != NULL) free_expr(cond);
        if (step != NULL) free_stmt(step);
        return NULL;
    }
    Stmt* stmt = create_cfor_stmt(init, cond, step, body);
    stmt->loc.line = kw.line;
    stmt->loc.column = kw.column;
    return stmt;
}

static Stmt* for_step(Parser* parser) {
    if (check(parser, TOKEN_IDENT)) {
        Token ident = parser->current;
        advance(parser);
        if (check(parser, TOKEN_ASSIGN)) {
            advance(parser); /* = */
            Expr* value = expression(parser);
            if (value == NULL) return NULL;
            /* create_assign_stmt copies the name, so the lexeme copy is ours. */
            char* name = copy_token_lexeme(&ident);
            Stmt* stmt = create_assign_stmt(name, value);
            free(name);
            stmt->loc.line = ident.line;
            stmt->loc.column = ident.column;
            return stmt;
        }
        if (check(parser, TOKEN_LPAREN)) {
            advance(parser); /* ( : call() parses from the first argument */
            char* name = copy_token_lexeme(&ident);
            Expr* callee = create_variable_expr(name);
            free(name);
            Expr* call_expr = call(parser, callee);
            Stmt* stmt = create_expr_stmt(call_expr);
            stmt->loc.line = ident.line;
            stmt->loc.column = ident.column;
            return stmt;
        }
        error_at_current(parser, "expected assignment or call in for step");
        return NULL;
    }
    Expr* expr = expression(parser);
    if (expr == NULL) return NULL;
    Stmt* stmt = create_expr_stmt(expr);
    return stmt;
}

static Stmt* for_statement(Parser* parser) {
    Token kw = parser->previous;  /* 'for' was just consumed */
    if (match(parser, TOKEN_LPAREN)) {
        return cfor_statement(parser, kw);
    }
    advance(parser); /* iterator name */
    char* var_name = copy_token_lexeme(&parser->previous);
    if (!match(parser, TOKEN_IN)) {
        free(var_name);
        error_at_current(parser, "expected 'in' after iterator variable");
        return NULL;
    }
    if (check(parser, TOKEN_SQL_QUERY)) {
        return sql_for_statement(parser, kw, var_name);
    }
    Expr* iterable = expression(parser);
    if (iterable == NULL) {
        free(var_name);
        return NULL;
    }
    Block* body = block(parser);
    if (body == NULL) {
        free_expr(iterable);
        free(var_name);
        return NULL;
    }
    Stmt* stmt = create_foreach_stmt(var_name, iterable, body);
    stmt->loc.line = kw.line;
    stmt->loc.column = kw.column;
    free(var_name);
    return stmt;
}

static Stmt* forall_statement(Parser* parser) {
    Token kw = parser->previous;  /* 'forall' was just consumed */
    if (!check(parser, TOKEN_IDENT)) {
        error_at_current(parser, "expected iterator variable after 'forall'");
        return NULL;
    }
    advance(parser); /* iterator name */
    char* var_name = copy_token_lexeme(&parser->previous);
    if (!match(parser, TOKEN_IN)) {
        free(var_name);
        error_at_current(parser, "expected 'in' after iterator variable");
        return NULL;
    }
    if (!check(parser, TOKEN_IDENT)) {
        free(var_name);
        error_at_current(parser, "expected array variable after 'in'");
        return NULL;
    }
    advance(parser); /* array name */
    char* array_name = copy_token_lexeme(&parser->previous);

    Stmt* sql_stmt = NULL;
    if (match(parser, TOKEN_INSERT)) {
        sql_stmt = sql_statement(parser, STMT_SQL_DML);
    } else if (match(parser, TOKEN_UPDATE)) {
        sql_stmt = sql_statement(parser, STMT_SQL_DML);
    } else if (match(parser, TOKEN_DELETE)) {
        sql_stmt = sql_statement(parser, STMT_SQL_DML);
    } else {
        free(var_name);
        free(array_name);
        error_at_current(parser, "expected INSERT, UPDATE, or DELETE after 'forall ... in ...'");
        return NULL;
    }
    if (sql_stmt == NULL) {
        free(var_name);
        free(array_name);
        return NULL;
    }
    Stmt* stmt = create_forall_stmt(var_name, array_name, sql_stmt);
    free(var_name);
    free(array_name);
    if (stmt != NULL) {
        stmt->loc.line = kw.line;
        stmt->loc.column = kw.column;
    }
    return stmt;
}

static Stmt* return_statement(Parser* parser) {
    /* 'return' was already consumed by the statement dispatcher */
    int kw_line = parser->previous.line;
    int kw_column = parser->previous.column;
    Expr* value = expression(parser);
    advance(parser); /* ; */
    Stmt* stmt = create_return_stmt(value);
    stmt->loc.line = kw_line;
    stmt->loc.column = kw_column;
    return stmt;
}

static Stmt* print_statement(Parser* parser) {
    /* 'print' was already consumed by the statement dispatcher */
    int kw_line = parser->previous.line;
    int kw_column = parser->previous.column;
    Expr* value = expression(parser);
    advance(parser); /* ; */
    Stmt* stmt = create_print_stmt(value);
    stmt->loc.line = kw_line;
    stmt->loc.column = kw_column;
    return stmt;
}

static char* copy_sql_token_text(const Token* token) {
    int len = token->length;
    while (len > 0) {
        char c = token->start[len - 1];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
        len--;
    }
    char* sql = malloc((size_t)len + 1);
    if (sql == NULL) return NULL;
    memcpy(sql, token->start, (size_t)len);
    sql[len] = '\0';
    return sql;
}

/* Rewrites each `?name` placeholder in `sql` (outside string literals) to a
   bare `?` in place and appends an SQL-parameter expression for `name` to
   *params, so the query text carries only positional markers and the values
   travel as binds, in order of appearance. Returns 0 when out of memory,
   with *params already released. */
static int extract_sql_params(char* sql, Expr*** params, int* param_count) {
    char* out = sql;
    const char* p = sql;
    char quote = '\0';
    while (*p != '\0') {
        if (quote != '\0') {
            if (*p == quote) quote = '\0';
        } else if (*p == '\'' || *p == '"') {
            quote = *p;
        } else if (*p == '?') {
            const char* ident = p + 1;
            const char* q = ident;
            while (is_ident_char(*q)) q++;
            if (q > ident) {
                char* pname = malloc((size_t)(q - ident) + 1);
                Expr* param = NULL;
                if (pname != NULL) {
                    memcpy(pname, ident, (size_t)(q - ident));
                    pname[q - ident] = '\0';
                    param = create_sql_param_expr(pname);
                    free(pname);
                }
                Expr** grown = param != NULL
                    ? realloc(*params, sizeof(Expr*) * (size_t)(*param_count + 1))
                    : NULL;
                if (grown == NULL) {
                    free_expr(param);
                    for (int i = 0; i < *param_count; i++) free_expr((*params)[i]);
                    free(*params);
                    *params = NULL;
                    *param_count = 0;
                    return 0;
                }
                *params = grown;
                grown[(*param_count)++] = param;
                *out++ = '?';
                p = q;
                continue;
            }
        }
        *out++ = *p++;
    }
    *out = '\0';
    return 1;
}

static Stmt* cursor_decl_statement(Parser* parser) {
    Token kw = parser->previous;  /* 'cursor' was just consumed */
    if (!check(parser, TOKEN_IDENT)) {
        error_at_current(parser, "expected cursor name");
        return NULL;
    }
    advance(parser); /* cursor name */
    char* name = copy_token_lexeme(&parser->previous);
    char* sql_query = NULL;
    Expr** params = NULL;
    int param_count = 0;
    if (match(parser, TOKEN_IS)) {
        if (!check(parser, TOKEN_SQL_QUERY)) {
            error_at_current(parser, "expected SELECT query after 'is'");
            free(name);
            return NULL;
        }
        Token sql_token = parser->current;
        advance(parser); /* SQL query */
        sql_query = copy_sql_token_text(&sql_token);
        if (sql_query == NULL || !extract_sql_params(sql_query, &params, &param_count)) {
            free(name);
            free(sql_query);
            error_at_current(parser, "out of memory");
            return NULL;
        }
    }
    if (!check(parser, TOKEN_SEMICOLON)) {
        error_at_current(parser, "expected ';' after cursor declaration");
        free(name);
        free(sql_query);
        for (int i = 0; i < param_count; i++) free_expr(params[i]);
        free(params);
        return NULL;
    }
    advance(parser); /* ; */
    Stmt* stmt = create_cursor_decl_stmt(name, sql_query, params, param_count);
    free(name);
    if (stmt != NULL) {
        stmt->loc.line = kw.line;
        stmt->loc.column = kw.column;
    }
    return stmt;
}

static Stmt* cursor_open_statement(Parser* parser) {
    Token kw = parser->previous;  /* 'open' was just consumed */
    if (!check(parser, TOKEN_IDENT)) {
        error_at_current(parser, "expected cursor name");
        return NULL;
    }
    advance(parser); /* cursor name */
    char* name = copy_token_lexeme(&parser->previous);
    char* sql_query = NULL;
    Expr** params = NULL;
    int param_count = 0;
    if (match(parser, TOKEN_FOR)) {
        if (!check(parser, TOKEN_SQL_QUERY)) {
            error_at_current(parser, "expected SELECT query after 'for'");
            free(name);
            return NULL;
        }
        Token sql_token = parser->current;
        advance(parser); /* SQL query */
        sql_query = copy_sql_token_text(&sql_token);
        if (sql_query == NULL) {
            free(name);
            error_at_current(parser, "out of memory");
            return NULL;
        }
        if (!extract_sql_params(sql_query, &params, &param_count)) {
            free(name);
            free(sql_query);
            error_at_current(parser, "out of memory");
            return NULL;
        }
    }
    if (!check(parser, TOKEN_SEMICOLON)) {
        error_at_current(parser, "expected ';' after open statement");
        free(name);
        free(sql_query);
        for (int i = 0; i < param_count; i++) free_expr(params[i]);
        free(params);
        return NULL;
    }
    advance(parser); /* ; */
    Stmt* stmt = create_cursor_open_stmt(name, sql_query, params, param_count);
    free(name);
    if (stmt != NULL) {
        stmt->loc.line = kw.line;
        stmt->loc.column = kw.column;
    }
    return stmt;
}

static Stmt* cursor_fetch_statement(Parser* parser) {
    Token kw = parser->previous;  /* 'fetch' was just consumed */
    if (!check(parser, TOKEN_IDENT)) {
        error_at_current(parser, "expected cursor name");
        return NULL;
    }
    advance(parser); /* cursor name */
    char* name = copy_token_lexeme(&parser->previous);
    if (!match(parser, TOKEN_INTO)) {
        error_at_current(parser, "expected 'into' after cursor name in fetch");
        free(name);
        return NULL;
    }
    char** into_vars = NULL;
    int into_count = 0;
    do {
        if (!check(parser, TOKEN_IDENT)) {
            error_at_current(parser, "expected variable name after 'into'");
            for (int i = 0; i < into_count; i++) free(into_vars[i]);
            free(into_vars);
            free(name);
            return NULL;
        }
        advance(parser); /* variable name */
        char** new_vars = realloc(into_vars, sizeof(char*) * (size_t)(into_count + 1));
        if (new_vars == NULL) {
            error_at_current(parser, "out of memory");
            for (int i = 0; i < into_count; i++) free(into_vars[i]);
            free(into_vars);
            free(name);
            return NULL;
        }
        into_vars = new_vars;
        into_vars[into_count++] = copy_token_lexeme(&parser->previous);
    } while (match(parser, TOKEN_COMMA));
    if (!check(parser, TOKEN_SEMICOLON)) {
        error_at_current(parser, "expected ';' after fetch statement");
        for (int i = 0; i < into_count; i++) free(into_vars[i]);
        free(into_vars);
        free(name);
        return NULL;
    }
    advance(parser); /* ; */
    Stmt* stmt = create_cursor_fetch_stmt(name, into_vars, into_count);
    free(name);
    if (stmt != NULL) {
        stmt->loc.line = kw.line;
        stmt->loc.column = kw.column;
    }
    return stmt;
}

static Stmt* cursor_close_statement(Parser* parser) {
    Token kw = parser->previous;  /* 'close' was just consumed */
    if (!check(parser, TOKEN_IDENT)) {
        error_at_current(parser, "expected cursor name");
        return NULL;
    }
    advance(parser); /* cursor name */
    char* name = copy_token_lexeme(&parser->previous);
    if (!check(parser, TOKEN_SEMICOLON)) {
        error_at_current(parser, "expected ';' after close statement");
        free(name);
        return NULL;
    }
    advance(parser); /* ; */
    Stmt* stmt = create_cursor_close_stmt(name);
    free(name);
    if (stmt != NULL) {
        stmt->loc.line = kw.line;
        stmt->loc.column = kw.column;
    }
    return stmt;
}

static int program_add_import(Program* program, Stmt* import_stmt) {
    if (program->import_count >= MAX_IMPORTS) {
        free_stmt(import_stmt);
        return 0;
    }
    if (program->import_count >= program->import_capacity) {
        int new_capacity = program->import_capacity == 0 ? 4 : program->import_capacity * 2;
        if (new_capacity > MAX_IMPORTS) new_capacity = MAX_IMPORTS;
        Stmt** new_imports = realloc(program->imports, sizeof(Stmt*) * (size_t)new_capacity);
        if (new_imports == NULL) {
            free_stmt(import_stmt);
            return 0;
        }
        program->imports = new_imports;
        program->import_capacity = new_capacity;
    }
    program->imports[program->import_count++] = import_stmt;
    return 1;
}

static void parse_import(Parser* parser, Program* program) {
    /* 'import' was already consumed by the top-level dispatcher */
    if (!check(parser, TOKEN_STRING)) {
        error_at_current(parser, "expected module path string");
        return;
    }
    int path_line = parser->current.line;
    int path_column = parser->current.column;
    char* raw_path = copy_token_lexeme(&parser->current);
    Stmt* stmt = create_import_stmt(raw_path);
    free(raw_path);
    if (stmt == NULL) {
        error_at_current(parser, "out of memory");
        return;
    }
    stmt->loc.line = path_line;
    stmt->loc.column = path_column;
    char* path = stmt->as.import_stmt.path;
    int len = (int)strlen(path);
    if (len >= 2 && path[0] == '"' && path[len - 1] == '"') {
        memmove(path, path + 1, (size_t)(len - 2));
        path[len - 2] = '\0';
    }
    advance(parser); /* string */
    if (!match(parser, TOKEN_SEMICOLON)) {
        error_at_current(parser, "expected ';' after import path");
        free_stmt(stmt);
        return;
    }
    if (!program_add_import(program, stmt)) {
        error_at_current(parser, "too many imports");
    }
}

static Token peek_next(Parser* parser) {
    Lexer copy = parser->lexer;
    return lexer_next_token(&copy);
}

static int is_column_percent_type_start(Parser* parser) {
    if (!check(parser, TOKEN_IDENT)) return 0;
    Lexer copy = parser->lexer;
    Token t1 = lexer_next_token(&copy);
    if (t1.type != TOKEN_DOT) return 0;
    Token t2 = lexer_next_token(&copy);
    if (t2.type != TOKEN_IDENT) return 0;
    Token t3 = lexer_next_token(&copy);
    return t3.type == TOKEN_PERCENT;
}

static Stmt* statement(Parser* parser) {
    if (check(parser, TOKEN_INT_TYPE) ||
        check(parser, TOKEN_FLOAT_TYPE) ||
        check(parser, TOKEN_STRING_TYPE) ||
        check(parser, TOKEN_BOOL_TYPE) ||
        check(parser, TOKEN_ANY_TYPE) ||
        check(parser, TOKEN_DATE_TYPE) ||
        check(parser, TOKEN_TIMESTAMP_TYPE) ||
        check(parser, TOKEN_ARRAY_TYPE) ||
        check(parser, TOKEN_MAP_TYPE)) {
        return var_decl(parser);
    }
    if (current_token_is_keyword(parser, "savepoint")) {
        advance(parser); /* consume savepoint */
        advance(parser); /* consume name */
        char* name = copy_token_lexeme(&parser->previous);
        advance(parser); /* consume ; */
        Stmt* stmt = create_sql_transaction_stmt(3, name);
        free(name);
        return stmt;
    }
    if (current_token_is_keyword(parser, "release")) {
        advance(parser); /* consume release */
        if (current_token_is_keyword(parser, "savepoint")) {
            advance(parser); /* consume savepoint */
        }
        advance(parser); /* consume name */
        char* name = copy_token_lexeme(&parser->previous);
        advance(parser); /* consume ; */
        Stmt* stmt = create_sql_transaction_stmt(5, name);
        free(name);
        return stmt;
    }
    if (current_token_is_keyword(parser, "pragma")) {
        Token kw = parser->current;
        advance(parser); /* consume pragma */
        if (!current_token_is_keyword(parser, "autonomous_transaction")) {
            error_at_current(parser, "expected 'autonomous_transaction' after 'pragma'");
            return NULL;
        }
        advance(parser); /* consume autonomous_transaction */
        if (!check(parser, TOKEN_SEMICOLON)) {
            error_at_current(parser, "expected ';' after pragma");
            return NULL;
        }
        advance(parser); /* consume ; */
        Stmt* stmt = create_pragma_stmt("autonomous_transaction");
        if (stmt != NULL) {
            stmt->loc.line = kw.line;
            stmt->loc.column = kw.column;
        }
        return stmt;
    }
    if (current_token_is_keyword(parser, "alter")) {
        advance(parser); /* consume alter */
        return sql_statement(parser, STMT_SQL_DDL);
    }
    if (check(parser, TOKEN_IDENT)) {
        /* Ambiguity between struct-typed or percent-typed variable
           declarations and other identifier-started statements. */
        Token next = peek_next(parser);
        if (next.type == TOKEN_IDENT || next.type == TOKEN_PERCENT ||
            is_column_percent_type_start(parser)) {
            return var_decl(parser);
        }
        if (next.type == TOKEN_EXCEPTION) {
            return exception_decl_statement(parser);
        }
        return assignment(parser);
    }
    if (match(parser, TOKEN_IF)) return if_statement(parser);
    if (match(parser, TOKEN_WHILE)) return while_statement(parser);
    if (match(parser, TOKEN_DO)) return do_while_statement(parser);
    if (match(parser, TOKEN_FOR)) return for_statement(parser);
    if (match(parser, TOKEN_FORALL)) return forall_statement(parser);
    if (match(parser, TOKEN_BREAK)) return break_statement(parser);
    if (match(parser, TOKEN_CONTINUE)) return continue_statement(parser);
    if (match(parser, TOKEN_TRY)) return try_statement(parser);
    if (match(parser, TOKEN_RAISE)) return raise_statement(parser);
    if (match(parser, TOKEN_SUBTYPE)) return subtype_decl_statement(parser);
    if (match(parser, TOKEN_CASE)) return case_statement(parser);
    if (match(parser, TOKEN_RETURN)) return return_statement(parser);
    if (match(parser, TOKEN_PRINT)) return print_statement(parser);
    if (check(parser, TOKEN_DROP) && peek_next(parser).type == TOKEN_TRIGGER) {
        /* Language-level DROP TRIGGER (not SQL DDL): removes a Auspex trigger. */
        Token kw = parser->current;
        advance(parser); /* consume drop */
        advance(parser); /* consume trigger */
        if (!check(parser, TOKEN_IDENT)) {
            error_at_current(parser, "expected trigger name after 'drop trigger'");
            return NULL;
        }
        advance(parser); /* consume name */
        char* name = copy_token_lexeme(&parser->previous);
        if (!match(parser, TOKEN_SEMICOLON)) {
            error_at_current(parser, "expected ';' after trigger name");
            free(name);
            return NULL;
        }
        Stmt* stmt = create_drop_trigger_stmt(name);
        free(name);
        if (stmt != NULL) {
            stmt->loc.line = kw.line;
            stmt->loc.column = kw.column;
        }
        return stmt;
    }
    if (match(parser, TOKEN_CREATE) || match(parser, TOKEN_DROP)) {
        return sql_statement(parser, STMT_SQL_DDL);
    }
    if (match(parser, TOKEN_INSERT) || match(parser, TOKEN_UPDATE) || match(parser, TOKEN_DELETE)) {
        return sql_statement(parser, STMT_SQL_DML);
    }
    if (match(parser, TOKEN_BEGIN)) {
        advance(parser); /* consume ; */
        return create_sql_transaction_stmt(0, NULL);
    }
    if (match(parser, TOKEN_COMMIT)) {
        advance(parser); /* consume ; */
        return create_sql_transaction_stmt(1, NULL);
    }
    if (match(parser, TOKEN_ROLLBACK)) {
        if (current_token_is_keyword(parser, "to")) {
            advance(parser); /* consume to */
            if (current_token_is_keyword(parser, "savepoint")) {
                advance(parser); /* consume savepoint */
            }
            advance(parser); /* consume name */
            char* name = copy_token_lexeme(&parser->previous);
            advance(parser); /* consume ; */
            Stmt* stmt = create_sql_transaction_stmt(4, name);
            free(name);
            return stmt;
        }
        advance(parser); /* consume ; */
        return create_sql_transaction_stmt(2, NULL);
    }
    if (match(parser, TOKEN_SQL_QUERY)) {
        return sql_statement(parser, STMT_SQL_QUERY);
    }
    if (match(parser, TOKEN_CURSOR)) return cursor_decl_statement(parser);
    if (match(parser, TOKEN_OPEN)) return cursor_open_statement(parser);
    if (match(parser, TOKEN_FETCH)) return cursor_fetch_statement(parser);
    if (match(parser, TOKEN_CLOSE)) return cursor_close_statement(parser);
    error_at_current(parser, "expected statement");
    return NULL;
}

static ProcDecl* parse_proc_header(Parser* parser, int is_function, int allow_no_return) {
    /* current is IDENT (procedure/function name); 'proc'/'func' was consumed by caller */
    advance(parser); /* name */
    char* name = copy_token_lexeme(&parser->previous);
    if (!match(parser, TOKEN_LPAREN)) {
        error_at_current(parser, "expected '(' after procedure name");
        free(name);
        return NULL;
    }

    Param* params = NULL;
    int param_count = 0;
    if (!check(parser, TOKEN_RPAREN)) {
        do {
            if (!check(parser, TOKEN_IDENT)) {
                error_at_current(parser, "expected parameter name");
                for (int i = 0; i < param_count; i++) {
                    free(params[i].name);
                    type_free(params[i].type);
                }
                free(params);
                free(name);
                return NULL;
            }
            advance(parser); /* param name */
            char* param_name = copy_token_lexeme(&parser->previous);

            ParamMode mode = PARAM_IN;
            if (match(parser, TOKEN_OUT)) {
                mode = PARAM_OUT;
            } else if (match(parser, TOKEN_IN) && check(parser, TOKEN_OUT)) {
                advance(parser);
                mode = PARAM_INOUT;
            }

            Type* param_type = parse_type(parser);

            Param* new_params = realloc(params,
                sizeof(Param) * (size_t)(param_count + 1));
            if (new_params == NULL) {
                free(param_name);
                type_free(param_type);
                for (int i = 0; i < param_count; i++) {
                    free(params[i].name);
                    type_free(params[i].type);
                }
                free(params);
                free(name);
                return NULL;
            }
            params = new_params;
            params[param_count].name = param_name;
            params[param_count].type = param_type;
            params[param_count].mode = mode;
            param_count++;
        } while (match(parser, TOKEN_COMMA));
    }

    if (!match(parser, TOKEN_RPAREN)) {
        error_at_current(parser, "expected ')' after parameters");
        for (int i = 0; i < param_count; i++) {
            free(params[i].name);
            type_free(params[i].type);
        }
        free(params);
        free(name);
        return NULL;
    }
    Type* return_type = NULL;
    if (match(parser, TOKEN_ARROW)) {
        return_type = parse_type(parser);
    } else if (allow_no_return && !is_function) {
        /* Procedure members of structs may omit the return type. */
        return_type = &type_int;
    } else {
        error_at_current(parser, "expected '->' after parameter list");
        for (int i = 0; i < param_count; i++) {
            free(params[i].name);
            type_free(params[i].type);
        }
        free(params);
        free(name);
        return NULL;
    }
    ProcDecl* proc = create_proc_decl(name, return_type);
    free(name);
    if (proc == NULL) {
        for (int i = 0; i < param_count; i++) {
            free(params[i].name);
            type_free(params[i].type);
        }
        free(params);
        return NULL;
    }
    proc->is_function = is_function;
    proc->params = params;
    proc->param_count = param_count;

    if (match_authid(parser)) {
        if (!check(parser, TOKEN_IDENT)) {
            error_at_current(parser, "expected authid value after 'authid'");
            free_proc_decl(proc);
            free(proc);
            return NULL;
        }
        proc->authid = copy_token_lexeme(&parser->current);
        advance(parser);
    }

    return proc;
}

static void parse_proc_with_kind(Parser* parser, Program* program, int is_function) {
    ProcDecl* proc = parse_proc_header(parser, is_function, 0);
    if (proc == NULL) return;
    proc->body = block(parser);
    if (proc->body == NULL) {
        free(proc->name);
        free(proc->authid);
        for (int i = 0; i < proc->param_count; i++) {
            free(proc->params[i].name);
            type_free(proc->params[i].type);
        }
        free(proc->params);
        type_free(proc->return_type);
        free(proc);
        return;
    }

    ProcDecl* new_procs = realloc(program->procs,
        sizeof(ProcDecl) * (size_t)(program->proc_count + 1));
    program->procs = new_procs;
    program->procs[program->proc_count++] = *proc;
    free(proc);
}

static void parse_proc(Parser* parser, Program* program) {
    parse_proc_with_kind(parser, program, 0);
}

static void parse_func(Parser* parser, Program* program) {
    parse_proc_with_kind(parser, program, 1);
}

static int package_add_var(Stmt*** vars, int* count, int* capacity, Stmt* var) {
    if (*count >= *capacity) {
        int new_capacity = *capacity == 0 ? 4 : *capacity * 2;
        Stmt** new_vars = realloc(*vars, sizeof(Stmt*) * (size_t)new_capacity);
        if (new_vars == NULL) return 0;
        *vars = new_vars;
        *capacity = new_capacity;
    }
    (*vars)[(*count)++] = var;
    return 1;
}

static int package_add_proc(ProcDecl** procs, int* count, int* capacity, ProcDecl* proc) {
    if (*count >= *capacity) {
        int new_capacity = *capacity == 0 ? 4 : *capacity * 2;
        ProcDecl* new_procs = realloc(*procs, sizeof(ProcDecl) * (size_t)new_capacity);
        if (new_procs == NULL) return 0;
        *procs = new_procs;
        *capacity = new_capacity;
    }
    (*procs)[(*count)++] = *proc;
    return 1;
}

static int token_is_type_after(Parser* parser) {
    Token next = peek_next(parser);
    return next.type == TOKEN_INT_TYPE ||
           next.type == TOKEN_FLOAT_TYPE ||
           next.type == TOKEN_STRING_TYPE ||
           next.type == TOKEN_BOOL_TYPE ||
           next.type == TOKEN_ANY_TYPE ||
           next.type == TOKEN_DATE_TYPE ||
           next.type == TOKEN_TIMESTAMP_TYPE ||
           next.type == TOKEN_ARRAY_TYPE ||
           next.type == TOKEN_MAP_TYPE ||
           next.type == TOKEN_CURSOR ||
           next.type == TOKEN_IDENT;
}

static int token_starts_type(Parser* parser) {
    if (check(parser, TOKEN_INT_TYPE) ||
        check(parser, TOKEN_FLOAT_TYPE) ||
        check(parser, TOKEN_STRING_TYPE) ||
        check(parser, TOKEN_BOOL_TYPE) ||
        check(parser, TOKEN_ANY_TYPE) ||
        check(parser, TOKEN_DATE_TYPE) ||
        check(parser, TOKEN_TIMESTAMP_TYPE) ||
        check(parser, TOKEN_ARRAY_TYPE) ||
        check(parser, TOKEN_MAP_TYPE) ||
        check(parser, TOKEN_CURSOR)) {
        return 1;
    }
    if (check(parser, TOKEN_IDENT)) {
        Token next = peek_next(parser);
        if (next.type == TOKEN_IDENT || next.type == TOKEN_PERCENT ||
            is_column_percent_type_start(parser)) {
            return 1;
        }
    }
    return 0;
}

static Stmt* parse_package_var(Parser* parser) {
    if (token_starts_type(parser)) {
        return var_decl(parser);
    }
    if (check(parser, TOKEN_IDENT) && token_is_type_after(parser)) {
        /* name-then-type form: "var_name int;" */
        Token name_token = parser->current;
        advance(parser); /* name */
        Type* type = parse_type(parser);
        if (!match(parser, TOKEN_SEMICOLON)) {
            error_at_current(parser, "expected ';' after variable declaration");
            type_free(type);
            return NULL;
        }
        char* name = copy_token_lexeme(&name_token);
        Stmt* stmt = create_var_decl_stmt(type, name, NULL);
        stmt->loc.line = name_token.line;
        stmt->loc.column = name_token.column;
        free(name);
        return stmt;
    }
    return NULL;
}

static void parse_package_spec(Parser* parser, Program* program) {
    /* 'package' was already consumed; current is IDENT (package name) */
    advance(parser); /* name */
    char* name = copy_token_lexeme(&parser->previous);
    if (!match(parser, TOKEN_IS)) {
        error_at_current(parser, "expected 'is' after package name");
        free(name);
        return;
    }

    PackageSpecDecl spec = {0};
    spec.name = name;

    if (match_authid(parser)) {
        if (!check(parser, TOKEN_IDENT)) {
            error_at_current(parser, "expected authid value after 'authid'");
            free_package_spec(&spec);
            return;
        }
        spec.authid = copy_token_lexeme(&parser->current);
        advance(parser);
    }

    while (!check(parser, TOKEN_END) && !check(parser, TOKEN_EOF) && !parser->had_error) {
        if (check(parser, TOKEN_PROC)) {
            advance(parser); /* proc */
            ProcDecl* proc = parse_proc_header(parser, 0, 0);
            if (proc == NULL) {
                free_package_spec(&spec);
                return;
            }
            if (!match(parser, TOKEN_SEMICOLON)) {
                error_at_current(parser, "expected ';' after procedure declaration");
                free(proc->name);
                free(proc->authid);
                for (int i = 0; i < proc->param_count; i++) {
                    free(proc->params[i].name);
                    type_free(proc->params[i].type);
                }
                free(proc->params);
                type_free(proc->return_type);
                free(proc);
                free_package_spec(&spec);
                return;
            }
            if (!package_add_proc(&spec.procs, &spec.proc_count, &spec.proc_capacity, proc)) {
                error_at_current(parser, "out of memory");
                free(proc->name);
                free(proc->authid);
                for (int i = 0; i < proc->param_count; i++) {
                    free(proc->params[i].name);
                    type_free(proc->params[i].type);
                }
                free(proc->params);
                type_free(proc->return_type);
                free(proc);
                free_package_spec(&spec);
                return;
            }
            free(proc);
        } else if (check(parser, TOKEN_FUNC)) {
            advance(parser); /* func */
            ProcDecl* proc = parse_proc_header(parser, 1, 0);
            if (proc == NULL) {
                free_package_spec(&spec);
                return;
            }
            if (!match(parser, TOKEN_SEMICOLON)) {
                error_at_current(parser, "expected ';' after function declaration");
                free(proc->name);
                free(proc->authid);
                for (int i = 0; i < proc->param_count; i++) {
                    free(proc->params[i].name);
                    type_free(proc->params[i].type);
                }
                free(proc->params);
                type_free(proc->return_type);
                free(proc);
                free_package_spec(&spec);
                return;
            }
            if (!package_add_proc(&spec.funcs, &spec.func_count, &spec.func_capacity, proc)) {
                error_at_current(parser, "out of memory");
                free(proc->name);
                free(proc->authid);
                for (int i = 0; i < proc->param_count; i++) {
                    free(proc->params[i].name);
                    type_free(proc->params[i].type);
                }
                free(proc->params);
                type_free(proc->return_type);
                free(proc);
                free_package_spec(&spec);
                return;
            }
            free(proc);
        } else {
            Stmt* var = parse_package_var(parser);
            if (var == NULL) {
                error_at_current(parser, "expected variable, procedure, or function declaration in package spec");
                free_package_spec(&spec);
                return;
            }
            if (!package_add_var(&spec.vars, &spec.var_count, &spec.var_capacity, var)) {
                error_at_current(parser, "out of memory");
                free_stmt(var);
                free_package_spec(&spec);
                return;
            }
        }
    }

    if (!match(parser, TOKEN_END)) {
        error_at_current(parser, "expected 'end' to close package spec");
        free_package_spec(&spec);
        return;
    }
    if (!check(parser, TOKEN_IDENT) ||
        (int)strlen(name) != parser->current.length ||
        memcmp(parser->current.start, name, parser->current.length) != 0) {
        error_at_current(parser, "expected matching package name after 'end'");
        free_package_spec(&spec);
        return;
    }
    advance(parser); /* matching name */
    if (!match(parser, TOKEN_SEMICOLON)) {
        error_at_current(parser, "expected ';' after 'end package'");
        free_package_spec(&spec);
        return;
    }

    if (program->spec_count >= program->spec_capacity) {
        int new_capacity = program->spec_capacity == 0 ? 4 : program->spec_capacity * 2;
        PackageSpecDecl* new_specs = realloc(program->specs,
            sizeof(PackageSpecDecl) * (size_t)new_capacity);
        if (new_specs == NULL) {
            error_at_current(parser, "out of memory");
            free_package_spec(&spec);
            return;
        }
        program->specs = new_specs;
        program->spec_capacity = new_capacity;
    }
    program->specs[program->spec_count++] = spec;
}

static void parse_package_body(Parser* parser, Program* program) {
    /* 'package' and 'body' were already consumed; current is IDENT (package name) */
    advance(parser); /* name */
    char* name = copy_token_lexeme(&parser->previous);
    if (!match(parser, TOKEN_IS)) {
        error_at_current(parser, "expected 'is' after package body name");
        free(name);
        return;
    }

    PackageBodyDecl body = {0};
    body.name = name;

    if (match_authid(parser)) {
        if (!check(parser, TOKEN_IDENT)) {
            error_at_current(parser, "expected authid value after 'authid'");
            free_package_body(&body);
            return;
        }
        body.authid = copy_token_lexeme(&parser->current);
        advance(parser);
    }

    while (!check(parser, TOKEN_END) && !check(parser, TOKEN_EOF) && !parser->had_error) {
        if (check(parser, TOKEN_PROC)) {
            advance(parser); /* proc */
            ProcDecl* proc = parse_proc_header(parser, 0, 0);
            if (proc == NULL) {
                free_package_body(&body);
                return;
            }
            proc->body = block(parser);
            if (proc->body == NULL) {
                free(proc->name);
                free(proc->authid);
                for (int i = 0; i < proc->param_count; i++) {
                    free(proc->params[i].name);
                    type_free(proc->params[i].type);
                }
                free(proc->params);
                type_free(proc->return_type);
                free(proc);
                free_package_body(&body);
                return;
            }
            if (!package_add_proc(&body.procs, &body.proc_count, &body.proc_capacity, proc)) {
                error_at_current(parser, "out of memory");
                free(proc->name);
                free(proc->authid);
                for (int i = 0; i < proc->param_count; i++) {
                    free(proc->params[i].name);
                    type_free(proc->params[i].type);
                }
                free(proc->params);
                type_free(proc->return_type);
                free(proc);
                free_package_body(&body);
                return;
            }
            free(proc);
        } else if (check(parser, TOKEN_FUNC)) {
            advance(parser); /* func */
            ProcDecl* proc = parse_proc_header(parser, 1, 0);
            if (proc == NULL) {
                free_package_body(&body);
                return;
            }
            proc->body = block(parser);
            if (proc->body == NULL) {
                free(proc->name);
                free(proc->authid);
                for (int i = 0; i < proc->param_count; i++) {
                    free(proc->params[i].name);
                    type_free(proc->params[i].type);
                }
                free(proc->params);
                type_free(proc->return_type);
                free(proc);
                free_package_body(&body);
                return;
            }
            if (!package_add_proc(&body.funcs, &body.func_count, &body.func_capacity, proc)) {
                error_at_current(parser, "out of memory");
                free(proc->name);
                free(proc->authid);
                for (int i = 0; i < proc->param_count; i++) {
                    free(proc->params[i].name);
                    type_free(proc->params[i].type);
                }
                free(proc->params);
                type_free(proc->return_type);
                free(proc);
                free_package_body(&body);
                return;
            }
            free(proc);
        } else {
            Stmt* var = parse_package_var(parser);
            if (var == NULL) {
                error_at_current(parser, "expected variable, procedure, or function declaration in package body");
                free_package_body(&body);
                return;
            }
            if (!package_add_var(&body.vars, &body.var_count, &body.var_capacity, var)) {
                error_at_current(parser, "out of memory");
                free_stmt(var);
                free_package_body(&body);
                return;
            }
        }
    }

    if (!match(parser, TOKEN_END)) {
        error_at_current(parser, "expected 'end' to close package body");
        free_package_body(&body);
        return;
    }
    if (!check(parser, TOKEN_IDENT) ||
        (int)strlen(name) != parser->current.length ||
        memcmp(parser->current.start, name, parser->current.length) != 0) {
        error_at_current(parser, "expected matching package name after 'end'");
        free_package_body(&body);
        return;
    }
    advance(parser); /* matching name */
    if (!match(parser, TOKEN_SEMICOLON)) {
        error_at_current(parser, "expected ';' after 'end package body'");
        free_package_body(&body);
        return;
    }

    if (program->body_count >= program->body_capacity) {
        int new_capacity = program->body_capacity == 0 ? 4 : program->body_capacity * 2;
        PackageBodyDecl* new_bodies = realloc(program->bodies,
            sizeof(PackageBodyDecl) * (size_t)new_capacity);
        if (new_bodies == NULL) {
            error_at_current(parser, "out of memory");
            free_package_body(&body);
            return;
        }
        program->bodies = new_bodies;
        program->body_capacity = new_capacity;
    }
    program->bodies[program->body_count++] = body;
}

static void parse_package(Parser* parser, Program* program) {
    /* 'package' was already consumed */
    if (match(parser, TOKEN_BODY)) {
        parse_package_body(parser, program);
    } else {
        parse_package_spec(parser, program);
    }
}

static int program_has_main(Program* program) {
    for (int i = 0; i < program->proc_count; i++) {
        if (strcmp(program->procs[i].name, "main") == 0) return 1;
    }
    return 0;
}

static void parse_anon_block(Parser* parser, Program* program) {
    /* 'block' was already consumed by the top-level dispatcher */
    if (program_has_main(program)) {
        error_at_current(parser, "anonymous block cannot be used when 'main' is already defined");
        return;
    }
    Block* body = block(parser);
    if (body == NULL) return;

    /* Append 'return 0;' to the block body. */
    Stmt** new_stmts = realloc(body->stmts,
        sizeof(Stmt*) * (size_t)(body->stmt_count + 1));
    if (new_stmts == NULL) {
        error_at_current(parser, "out of memory");
        free_block(body);
        return;
    }
    body->stmts = new_stmts;
    body->stmts[body->stmt_count++] = create_return_stmt(
        create_literal_expr(value_int(0)));

    ProcDecl* proc = create_proc_decl("main", &type_int);
    if (proc == NULL) {
        error_at_current(parser, "out of memory");
        free_block(body);
        return;
    }
    proc->body = body;

    ProcDecl* new_procs = realloc(program->procs,
        sizeof(ProcDecl) * (size_t)(program->proc_count + 1));
    if (new_procs == NULL) {
        error_at_current(parser, "out of memory");
        free(proc->name);
        free(proc);
        free_block(body);
        return;
    }
    program->procs = new_procs;
    program->procs[program->proc_count++] = *proc;
    free(proc);
}

static ParseRule rules[] = {
    [TOKEN_PROC]       = {NULL,        NULL,   PREC_NONE},
    [TOKEN_FUNC]       = {NULL,        NULL,   PREC_NONE},
    [TOKEN_IN]         = {NULL,        NULL,   PREC_NONE},
    [TOKEN_OUT]        = {NULL,        NULL,   PREC_NONE},
    [TOKEN_BLOCK]      = {NULL,        NULL,   PREC_NONE},
    [TOKEN_FOR]        = {NULL,        NULL,   PREC_NONE},
    [TOKEN_IF]         = {NULL,        NULL,   PREC_NONE},
    [TOKEN_RETURN]     = {NULL,        NULL,   PREC_NONE},
    [TOKEN_PRINT]      = {NULL,        NULL,   PREC_NONE},
    [TOKEN_IMPORT]     = {NULL,        NULL,   PREC_NONE},
    [TOKEN_INT_TYPE]   = {NULL,        NULL,   PREC_NONE},
    [TOKEN_FLOAT_TYPE] = {NULL,        NULL,   PREC_NONE},
    [TOKEN_STRING_TYPE]= {NULL,        NULL,   PREC_NONE},
    [TOKEN_BOOL_TYPE]  = {NULL,        NULL,   PREC_NONE},
    [TOKEN_ANY_TYPE]   = {NULL,        NULL,   PREC_NONE},
    [TOKEN_ARRAY_TYPE] = {NULL,        NULL,   PREC_NONE},
    [TOKEN_CURSOR]     = {NULL,        NULL,   PREC_NONE},
    [TOKEN_OPEN]       = {NULL,        NULL,   PREC_NONE},
    [TOKEN_FETCH]      = {NULL,        NULL,   PREC_NONE},
    [TOKEN_CLOSE]      = {NULL,        NULL,   PREC_NONE},
    [TOKEN_IS]         = {NULL,        NULL,   PREC_NONE},
    [TOKEN_PACKAGE]    = {NULL,        NULL,   PREC_NONE},
    [TOKEN_BODY]       = {NULL,        NULL,   PREC_NONE},
    [TOKEN_END]        = {NULL,        NULL,   PREC_NONE},
    [TOKEN_TRUE]       = {literal_bool,NULL,   PREC_NONE},
    [TOKEN_FALSE]      = {literal_bool,NULL,   PREC_NONE},
    [TOKEN_NULL]       = {literal_null,NULL,   PREC_NONE},
    [TOKEN_SQLCODE]    = {sqlcode_expr,NULL,   PREC_NONE},
    [TOKEN_SQLERRM]    = {sqlerrm_expr,NULL,   PREC_NONE},
    [TOKEN_IDENT]      = {variable,    NULL,   PREC_NONE},
    [TOKEN_INT]        = {number,      NULL,   PREC_NONE},
    [TOKEN_FLOAT]      = {number,      NULL,   PREC_NONE},
    [TOKEN_STRING]     = {number,      NULL,   PREC_NONE},
    [TOKEN_SQL_QUERY]  = {NULL,        NULL,   PREC_NONE},

    [TOKEN_ARROW]      = {NULL,        NULL,   PREC_NONE},
    [TOKEN_EQ]         = {NULL,        binary, PREC_EQUALITY},
    [TOKEN_NE]         = {NULL,        binary, PREC_EQUALITY},
    [TOKEN_GE]         = {NULL,        binary, PREC_COMPARISON},
    [TOKEN_LE]         = {NULL,        binary, PREC_COMPARISON},
    [TOKEN_ASSIGN]     = {NULL,        NULL,   PREC_NONE},
    [TOKEN_LT]         = {NULL,        binary, PREC_COMPARISON},
    [TOKEN_GT]         = {NULL,        binary, PREC_COMPARISON},
    [TOKEN_BANG]       = {unary,       NULL,   PREC_NONE},
    [TOKEN_PERCENT]    = {NULL,        cursor_attr, PREC_CALL},

    [TOKEN_PLUS]       = {NULL,        binary, PREC_TERM},
    [TOKEN_MINUS]      = {unary,       binary, PREC_TERM},
    [TOKEN_STAR]       = {NULL,        binary, PREC_FACTOR},
    [TOKEN_SLASH]      = {NULL,        binary, PREC_FACTOR},
    [TOKEN_DOT]        = {NULL,        field,  PREC_CALL},

    [TOKEN_LPAREN]     = {grouping,    call,   PREC_CALL},
    [TOKEN_RPAREN]     = {NULL,        NULL,   PREC_NONE},
    [TOKEN_LBRACE]     = {map_literal, NULL,   PREC_NONE},
    [TOKEN_RBRACE]     = {NULL,        NULL,   PREC_NONE},
    [TOKEN_LBRACKET]   = {array_literal, index_expr, PREC_CALL},
    [TOKEN_RBRACKET]   = {NULL,        NULL,   PREC_NONE},
    [TOKEN_COMMA]      = {NULL,        NULL,   PREC_NONE},
    [TOKEN_SEMICOLON]  = {NULL,        NULL,   PREC_NONE},
    [TOKEN_COLON]      = {trigger_row_ref, NULL, PREC_NONE},

    [TOKEN_ERROR]      = {NULL,        NULL,   PREC_NONE},
    [TOKEN_EOF]        = {NULL,        NULL,   PREC_NONE},
};

static ParseRule* get_rule(TokenType type) {
    return &rules[type];
}

Expr* parse_expression(const char* source) {
    Parser parser;
    lexer_init(&parser.lexer, source);
    parser.had_error = 0;
    parser.path = NULL;
    parser.struct_count = 0;
    advance(&parser);
    Expr* expr = expression(&parser);
    parser_free_struct_names(&parser);
    if (parser.had_error) {
        free_expr(expr);
        return NULL;
    }
    return expr;
}

static void parse_struct(Parser* parser, Program* program) {
    /* 'struct' was already consumed by the top-level dispatcher */
    if (!check(parser, TOKEN_IDENT)) {
        error_at_current(parser, "expected struct name");
        return;
    }
    advance(parser); /* name */
    char* name = copy_token_lexeme(&parser->previous);

    if (!match(parser, TOKEN_LBRACE)) {
        error_at_current(parser, "expected '{' after struct name");
        free(name);
        return;
    }

    char** field_names = NULL;
    Type** field_types = NULL;
    int field_count = 0;
    ProcDecl* methods = NULL;
    int method_count = 0;
    int method_capacity = 0;
    while (!check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF) && !parser->had_error) {
        if (check(parser, TOKEN_PROC) || check(parser, TOKEN_FUNC)) {
            int is_function = check(parser, TOKEN_FUNC);
            advance(parser); /* proc/func */
            ProcDecl* method = parse_proc_header(parser, is_function, 1);
            if (method == NULL) break;
            method->body = block(parser);
            if (method->body == NULL) {
                free_proc_decl(method);
                free(method);
                break;
            }
            if (!package_add_proc(&methods, &method_count, &method_capacity, method)) {
                error_at_current(parser, "out of memory");
                free_proc_decl(method);
                free(method);
                break;
            }
            free(method);
        } else {
            if (!check(parser, TOKEN_IDENT)) {
                error_at_current(parser, "expected field or method name");
                break;
            }
            advance(parser); /* field name */
            char** new_names = realloc(field_names, sizeof(char*) * (size_t)(field_count + 1));
            Type** new_types = realloc(field_types, sizeof(Type*) * (size_t)(field_count + 1));
            if (new_names == NULL || new_types == NULL) {
                error_at_current(parser, "out of memory");
                break;
            }
            field_names = new_names;
            field_types = new_types;
            field_names[field_count] = copy_token_lexeme(&parser->previous);
            field_types[field_count] = parse_type(parser);
            field_count++;
        }
        if (!check(parser, TOKEN_RBRACE)) {
            if (!match(parser, TOKEN_COMMA)) {
                error_at_current(parser, "expected ',' or '}' in struct declaration");
                break;
            }
        }
    }

    if (!parser->had_error && !match(parser, TOKEN_RBRACE)) {
        error_at_current(parser, "expected '}' after struct members");
    }

    if (!parser->had_error && program->struct_count >= program->struct_capacity) {
        int new_capacity = program->struct_capacity == 0 ? 4 : program->struct_capacity * 2;
        StructDecl* new_structs = realloc(program->structs,
            sizeof(StructDecl) * (size_t)new_capacity);
        if (new_structs == NULL) {
            error_at_current(parser, "out of memory");
        } else {
            program->structs = new_structs;
            program->struct_capacity = new_capacity;
        }
    }

    if (parser->had_error) {
        for (int i = 0; i < field_count; i++) {
            free(field_names[i]);
            type_free(field_types[i]);
        }
        free(field_names);
        free(field_types);
        for (int i = 0; i < method_count; i++) {
            free_proc_decl(&methods[i]);
        }
        free(methods);
        free(name);
        return;
    }

    StructDecl* decl = &program->structs[program->struct_count++];
    decl->name = name;
    decl->field_names = field_names;
    decl->field_types = field_types;
    decl->field_count = field_count;
    decl->methods = methods;
    decl->method_count = method_count;
    decl->method_capacity = method_capacity;
    parser_add_struct_name(parser, name);
}

static void parse_trigger(Parser* parser, Program* program) {
    /* 'trigger' was already consumed; current is IDENT (trigger name) */
    if (!check(parser, TOKEN_IDENT)) {
        error_at_current(parser, "expected trigger name after 'trigger'");
        return;
    }
    advance(parser); /* name */
    char* name = copy_token_lexeme(&parser->previous);

    int timing = -1;
    if (current_token_is_keyword(parser, "before")) {
        timing = TRIGGER_BEFORE;
    } else if (current_token_is_keyword(parser, "after")) {
        timing = TRIGGER_AFTER;
    }
    if (timing < 0) {
        error_at_current(parser, "expected 'before' or 'after' in trigger declaration");
        free(name);
        return;
    }
    advance(parser); /* before/after */

    int event = -1;
    if (match(parser, TOKEN_INSERT)) {
        event = TRIGGER_INSERT;
    } else if (match(parser, TOKEN_UPDATE)) {
        event = TRIGGER_UPDATE;
    } else if (match(parser, TOKEN_DELETE)) {
        event = TRIGGER_DELETE;
    } else if (match(parser, TOKEN_CREATE)) {
        event = TRIGGER_CREATE;
    } else if (match(parser, TOKEN_DROP)) {
        event = TRIGGER_DROP;
    }
    if (event < 0) {
        error_at_current(parser, "expected 'insert', 'update', 'delete', 'create', or 'drop' in trigger declaration");
        free(name);
        return;
    }

    if (!current_token_is_keyword(parser, "on")) {
        error_at_current(parser, "expected 'on' in trigger declaration");
        free(name);
        return;
    }
    advance(parser); /* on */

    if (!check(parser, TOKEN_IDENT)) {
        error_at_current(parser, "expected table name after 'on'");
        free(name);
        return;
    }
    advance(parser); /* table name */
    char* table = copy_token_lexeme(&parser->previous);

    int for_each_row = 0;
    if (match(parser, TOKEN_FOR)) {
        if (!current_token_is_keyword(parser, "each")) {
            error_at_current(parser, "expected 'each' after 'for' in trigger declaration");
            free(name);
            free(table);
            return;
        }
        advance(parser); /* each */
        if (!current_token_is_keyword(parser, "row")) {
            error_at_current(parser, "expected 'row' after 'for each' in trigger declaration");
            free(name);
            free(table);
            return;
        }
        advance(parser); /* row */
        for_each_row = 1;
    }
    if (for_each_row && event != TRIGGER_INSERT && event != TRIGGER_UPDATE &&
        event != TRIGGER_DELETE) {
        error_at_current(parser, "'for each row' requires an insert, update, or delete trigger");
        free(name);
        free(table);
        return;
    }

    if (!check(parser, TOKEN_LBRACE)) {
        error_at_current(parser, "expected '{' to start trigger body");
        free(name);
        free(table);
        return;
    }
    Block* body = block(parser);
    if (body == NULL) {
        free(name);
        free(table);
        return;
    }

    if (program->trigger_count >= program->trigger_capacity) {
        int new_capacity = program->trigger_capacity == 0 ? 4 : program->trigger_capacity * 2;
        TriggerDecl* new_triggers = realloc(program->triggers,
            sizeof(TriggerDecl) * (size_t)new_capacity);
        if (new_triggers == NULL) {
            error_at_current(parser, "out of memory");
            free(name);
            free(table);
            free_block(body);
            return;
        }
        program->triggers = new_triggers;
        program->trigger_capacity = new_capacity;
    }
    TriggerDecl* decl = &program->triggers[program->trigger_count++];
    decl->name = name;
    decl->timing = timing;
    decl->event = event;
    decl->table = table;
    decl->for_each_row = for_each_row;
    decl->body = body;
}

Program* parse_with_path(const char* source, const char* path, char* error, size_t error_size) {
    return parse_with_path_from_line(source, path, 1, error, error_size);
}

Program* parse_with_path_from_line(const char* source, const char* path, int first_line,
                                   char* error, size_t error_size) {
    Parser parser;
    lexer_init_at_line(&parser.lexer, source, first_line);
    parser.had_error = 0;
    parser.path = path;
    parser.error_message[0] = '\0';
    parser.struct_count = 0;
    advance(&parser);

    Program* program = create_program();
    while (!check(&parser, TOKEN_EOF) && !parser.had_error) {
        if (match(&parser, TOKEN_IMPORT)) {
            parse_import(&parser, program);
        } else if (match(&parser, TOKEN_STRUCT)) {
            parse_struct(&parser, program);
        } else if (match(&parser, TOKEN_PROC)) {
            parse_proc(&parser, program);
        } else if (match(&parser, TOKEN_FUNC)) {
            parse_func(&parser, program);
        } else if (match(&parser, TOKEN_PACKAGE)) {
            parse_package(&parser, program);
        } else if (match(&parser, TOKEN_TRIGGER)) {
            parse_trigger(&parser, program);
        } else if (check(&parser, TOKEN_CREATE)) {
            /* Support 'create [or replace] package [body] ...' syntax. */
            advance(&parser); /* create */
            if (check(&parser, TOKEN_IDENT) && parser.current.length == 2 &&
                memcmp(parser.current.start, "or", 2) == 0) {
                advance(&parser); /* or */
                if (check(&parser, TOKEN_IDENT) && parser.current.length == 7 &&
                    memcmp(parser.current.start, "replace", 7) == 0) {
                    advance(&parser); /* replace */
                } else {
                    error_at_current(&parser, "expected 'replace' after 'or'");
                    break;
                }
            }
            if (match(&parser, TOKEN_PACKAGE)) {
                parse_package(&parser, program);
            } else {
                error_at_current(&parser, "expected 'package' after 'create'");
                break;
            }
        } else if (match(&parser, TOKEN_BLOCK)) {
            parse_anon_block(&parser, program);
        } else {
            error_at_current(&parser, "expected 'proc', 'func', 'struct', 'import', 'package', 'trigger', or 'block'");
            break;
        }
    }

    if (parser.had_error) {
        if (error != NULL && error_size > 0) {
            strncpy(error, parser.error_message, error_size - 1);
            error[error_size - 1] = '\0';
        }
        parser_free_struct_names(&parser);
        free_program(program);
        return NULL;
    }
    parser_free_struct_names(&parser);
    return program;
}

Program* parse(const char* source, char* error, size_t error_size) {
    return parse_with_path(source, NULL, error, error_size);
}
