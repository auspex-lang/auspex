#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "packages.h"
#include "lexer.h"
#include "os.h"

#ifdef USE_SQLITE
#include "sqlite_driver.h"
#endif

static char* strip_main_procedure(const char* source);

#define BUILTIN_DBMS_OUTPUT \
    "package dbms_output is\n" \
    "    proc enable(buffer_size int) -> int;\n" \
    "    proc put_line(line string) -> int;\n" \
    "    proc disable() -> int;\n" \
    "    func get_lines() -> array<string>;\n" \
    "end dbms_output;\n" \
    "\n" \
    "package body dbms_output is\n" \
    "    proc enable(buffer_size int) -> int {\n" \
    "        dbms_output_enable(buffer_size);\n" \
    "        return 0;\n" \
    "    }\n" \
    "\n" \
    "    proc put_line(line string) -> int {\n" \
    "        dbms_output_put_line(line);\n" \
    "        return 0;\n" \
    "    }\n" \
    "\n" \
    "    proc disable() -> int {\n" \
    "        dbms_output_disable();\n" \
    "        return 0;\n" \
    "    }\n" \
    "\n" \
    "    func get_lines() -> array<string> {\n" \
    "        return dbms_output_get_lines();\n" \
    "    }\n" \
    "end dbms_output;"

#define BUILTIN_UTL_FILE \
    "package utl_file is\n" \
    "    func fopen(path string, mode string) -> int;\n" \
    "    func get_line(handle int) -> string;\n" \
    "    proc put_line(handle int, text string) -> int;\n" \
    "    func fseek(handle int, offset int) -> int;\n" \
    "    proc fflush(handle int) -> int;\n" \
    "    proc fclose(handle int) -> int;\n" \
    "    func mkdir(path string) -> int;\n" \
    "    func remove(path string) -> int;\n" \
    "end utl_file;\n" \
    "\n" \
    "package body utl_file is\n" \
    "    func fopen(path string, mode string) -> int {\n" \
    "        return utl_file_fopen(path, mode);\n" \
    "    }\n" \
    "\n" \
    "    func get_line(handle int) -> string {\n" \
    "        return utl_file_get_line(handle);\n" \
    "    }\n" \
    "\n" \
    "    proc put_line(handle int, text string) -> int {\n" \
    "        utl_file_put_line(handle, text);\n" \
    "        return 0;\n" \
    "    }\n" \
    "\n" \
    "    func fseek(handle int, offset int) -> int {\n" \
    "        return utl_file_fseek(handle, offset);\n" \
    "    }\n" \
    "\n" \
    "    proc fflush(handle int) -> int {\n" \
    "        return utl_file_fflush(handle);\n" \
    "    }\n" \
    "\n" \
    "    proc fclose(handle int) -> int {\n" \
    "        utl_file_fclose(handle);\n" \
    "        return 0;\n" \
    "    }\n" \
    "\n" \
    "    func mkdir(path string) -> int {\n" \
    "        return utl_file_mkdir(path);\n" \
    "    }\n" \
    "\n" \
    "    func remove(path string) -> int {\n" \
    "        return utl_file_remove(path);\n" \
    "    }\n" \
    "end utl_file;"

#define BUILTIN_DBMS_SQL \
    "package dbms_sql is\n" \
    "    proc execute(sql string) -> int;\n" \
    "    func query(sql string) -> array<row>;\n" \
    "    func open_cursor() -> int;\n" \
    "    proc parse(c int, sql string) -> int;\n" \
    "    proc bind_variable(c int, name string, value any) -> int;\n" \
    "    func execute_cursor(c int) -> int;\n" \
    "    func fetch_rows(c int, rows int) -> array<row>;\n" \
    "    func column_value(c int, column int) -> any;\n" \
    "    proc close_cursor(c int) -> int;\n" \
    "end dbms_sql;\n" \
    "\n" \
    "package body dbms_sql is\n" \
    "    proc execute(sql string) -> int {\n" \
    "        return dbms_sql_execute(sql);\n" \
    "    }\n" \
    "\n" \
    "    func query(sql string) -> array<row> {\n" \
    "        return dbms_sql_query(sql);\n" \
    "    }\n" \
    "\n" \
    "    func open_cursor() -> int {\n" \
    "        return dbms_sql_open_cursor();\n" \
    "    }\n" \
    "\n" \
    "    proc parse(c int, sql string) -> int {\n" \
    "        dbms_sql_parse(c, sql);\n" \
    "        return 0;\n" \
    "    }\n" \
    "\n" \
    "    proc bind_variable(c int, name string, value any) -> int {\n" \
    "        dbms_sql_bind_variable(c, name, value);\n" \
    "        return 0;\n" \
    "    }\n" \
    "\n" \
    "    func execute_cursor(c int) -> int {\n" \
    "        return dbms_sql_cursor_execute(c);\n" \
    "    }\n" \
    "\n" \
    "    func fetch_rows(c int, rows int) -> array<row> {\n" \
    "        return dbms_sql_fetch_rows(c, rows);\n" \
    "    }\n" \
    "\n" \
    "    func column_value(c int, column int) -> any {\n" \
    "        return dbms_sql_column_value(c, column);\n" \
    "    }\n" \
    "\n" \
    "    proc close_cursor(c int) -> int {\n" \
    "        dbms_sql_close_cursor(c);\n" \
    "        return 0;\n" \
    "    }\n" \
    "end dbms_sql;"

static const char* builtin_packages[] = {
    BUILTIN_DBMS_OUTPUT,
    BUILTIN_UTL_FILE,
    BUILTIN_DBMS_SQL,
    NULL
};

char* packages_load_builtins(void) {
    size_t total = 0;
    for (int i = 0; builtin_packages[i] != NULL; i++) {
        total += strlen(builtin_packages[i]) + 1;
    }
    if (total == 0) return NULL;
    char* result = malloc(total + 1);
    if (result == NULL) return NULL;
    result[0] = '\0';
    for (int i = 0; builtin_packages[i] != NULL; i++) {
        if (i > 0) strcat(result, "\n");
        strcat(result, builtin_packages[i]);
    }
    return result;
}

#define PACKAGE_NAME_MAX 64

typedef struct {
    const char* start; /* `create` or `package` keyword */
    const char* end;   /* one past the ';' of `end NAME;` */
    char name[PACKAGE_NAME_MAX];
} PackageBlock;

static int token_is_word(const Token* token, const char* word) {
    size_t len = strlen(word);
    return token->type == TOKEN_IDENT && (size_t)token->length == len &&
           memcmp(token->start, word, len) == 0;
}

/* Tokenize source (comments and string literals are skipped by the lexer)
   and collect every top-level `[create [or replace]] package [body] NAME is
   ... end NAME;` block. Returns the block count, or -1 if out of memory. */
static int find_package_blocks(const char* source, PackageBlock** out_blocks) {
    *out_blocks = NULL;
    Token* tokens = NULL;
    int token_count = 0;
    int token_cap = 0;
    Lexer lexer;
    lexer_init(&lexer, source);
    for (;;) {
        Token token = lexer_next_token(&lexer);
        if (token.type == TOKEN_EOF) break;
        if (token.type == TOKEN_ERROR) continue;
        if (token_count == token_cap) {
            int cap = token_cap == 0 ? 256 : token_cap * 2;
            Token* grown = realloc(tokens, sizeof(Token) * (size_t)cap);
            if (grown == NULL) {
                free(tokens);
                return -1;
            }
            tokens = grown;
            token_cap = cap;
        }
        tokens[token_count++] = token;
    }

    PackageBlock* blocks = NULL;
    int block_count = 0;
    int block_cap = 0;
    int depth = 0;
    for (int i = 0; i < token_count; i++) {
        if (tokens[i].type == TOKEN_LBRACE) {
            depth++;
            continue;
        }
        if (tokens[i].type == TOKEN_RBRACE) {
            if (depth > 0) depth--;
            continue;
        }
        if (depth > 0) continue;

        int k = i;
        if (tokens[k].type == TOKEN_CREATE) {
            k++;
            if (k + 1 < token_count && token_is_word(&tokens[k], "or") &&
                token_is_word(&tokens[k + 1], "replace")) {
                k += 2;
            }
        }
        if (k >= token_count || tokens[k].type != TOKEN_PACKAGE) continue;
        k++;
        if (k < token_count && tokens[k].type == TOKEN_BODY) k++;
        if (k >= token_count || tokens[k].type != TOKEN_IDENT ||
            tokens[k].length >= PACKAGE_NAME_MAX) {
            continue;
        }
        const Token* name = &tokens[k];

        int close = -1;
        for (int j = k + 1; j + 2 < token_count; j++) {
            if (tokens[j].type == TOKEN_END &&
                tokens[j + 1].type == TOKEN_IDENT &&
                tokens[j + 1].length == name->length &&
                memcmp(tokens[j + 1].start, name->start, (size_t)name->length) == 0 &&
                tokens[j + 2].type == TOKEN_SEMICOLON) {
                close = j + 2;
                break;
            }
        }
        if (close < 0) continue;

        if (block_count == block_cap) {
            int cap = block_cap == 0 ? 8 : block_cap * 2;
            PackageBlock* grown = realloc(blocks, sizeof(PackageBlock) * (size_t)cap);
            if (grown == NULL) {
                free(blocks);
                free(tokens);
                return -1;
            }
            blocks = grown;
            block_cap = cap;
        }
        PackageBlock* block = &blocks[block_count++];
        block->start = tokens[i].start;
        block->end = tokens[close].start + 1;
        memcpy(block->name, name->start, (size_t)name->length);
        block->name[name->length] = '\0';
        i = close;
    }

    free(tokens);
    *out_blocks = blocks;
    return block_count;
}

static int package_block_named(const PackageBlock* blocks, int count, const char* name) {
    for (int i = 0; i < count; i++) {
        if (strcmp(blocks[i].name, name) == 0) return 1;
    }
    return 0;
}

char* packages_filter_redefined(const char* loaded, const char* source) {
    if (loaded == NULL || *loaded == '\0') return NULL;

    PackageBlock* declared = NULL;
    int declared_count = source != NULL ? find_package_blocks(source, &declared) : 0;
    PackageBlock* blocks = NULL;
    int block_count = declared_count > 0 ? find_package_blocks(loaded, &blocks) : 0;
    if (declared_count < 0 || block_count < 0) {
        free(declared);
        free(blocks);
        return strdup(loaded);
    }

    char* result = malloc(strlen(loaded) + 1);
    if (result == NULL) {
        free(declared);
        free(blocks);
        return strdup(loaded);
    }
    size_t out = 0;
    const char* p = loaded;
    for (int i = 0; i < block_count; i++) {
        if (!package_block_named(declared, declared_count, blocks[i].name)) continue;
        memcpy(result + out, p, (size_t)(blocks[i].start - p));
        out += (size_t)(blocks[i].start - p);
        p = blocks[i].end;
        if (*p == '\n') p++;
    }
    strcpy(result + out, p);
    free(declared);
    free(blocks);

    for (const char* q = result; *q != '\0'; q++) {
        if (*q != ' ' && *q != '\t' && *q != '\n' && *q != '\r') return result;
    }
    free(result);
    return NULL;
}

#ifdef USE_SQLITE
static const char* PACKAGES_TABLE =
    "CREATE TABLE IF NOT EXISTS _auspex_packages ("
    "    name TEXT PRIMARY KEY,"
    "    spec_source TEXT,"
    "    body_source TEXT"
    ")";

static int sqlite_save_source(DBDriver* driver, const char* source, int append) {
    sqlite3* db = ((SQLiteImpl*)driver->impl)->db;
    char* err = NULL;
    if (sqlite3_exec(db, PACKAGES_TABLE, NULL, NULL, &err) != SQLITE_OK) {
        if (err != NULL) {
            snprintf(driver->error_message, sizeof(driver->error_message), "%s", err);
            sqlite3_free(err);
        }
        return 0;
    }

    char* stripped = strip_main_procedure(source);
    if (stripped == NULL) {
        snprintf(driver->error_message, sizeof(driver->error_message), "out of memory");
        return 0;
    }

    sqlite3_stmt* stmt = NULL;
    const char* sql = append
        ? "INSERT INTO _auspex_packages (name, spec_source, body_source) "
          "VALUES ('__packages__', '', ?1) "
          "ON CONFLICT(name) DO UPDATE SET body_source = body_source || ?1"
        : "INSERT INTO _auspex_packages (name, spec_source, body_source) "
          "VALUES ('__packages__', '', ?1) "
          "ON CONFLICT(name) DO UPDATE SET body_source = ?1";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        snprintf(driver->error_message, sizeof(driver->error_message), "%s",
                 sqlite3_errmsg(db));
        free(stripped);
        return 0;
    }
    if (sqlite3_bind_text(stmt, 1, stripped, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
        snprintf(driver->error_message, sizeof(driver->error_message), "%s",
                 sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        free(stripped);
        return 0;
    }
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    free(stripped);
    if (rc != SQLITE_DONE) {
        snprintf(driver->error_message, sizeof(driver->error_message), "%s",
                 sqlite3_errmsg(db));
        return 0;
    }
    return 1;
}

static char* sqlite_load_source(DBDriver* driver) {
    sqlite3* db = ((SQLiteImpl*)driver->impl)->db;
    sqlite3_stmt* check = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT name FROM sqlite_master WHERE type='table' AND name='_auspex_packages'",
                           -1, &check, NULL) != SQLITE_OK) {
        return NULL;
    }
    int exists = sqlite3_step(check) == SQLITE_ROW;
    sqlite3_finalize(check);
    if (!exists) return NULL;

    sqlite3_stmt* stmt = NULL;
    const char* sql = "SELECT COALESCE(body_source, '') FROM _auspex_packages";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return NULL;
    }

    size_t total = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* text = (const char*)sqlite3_column_text(stmt, 0);
        if (text != NULL) total += strlen(text);
    }
    sqlite3_reset(stmt);

    if (total == 0) {
        sqlite3_finalize(stmt);
        return NULL;
    }

    char* result = malloc(total + 1);
    if (result == NULL) {
        sqlite3_finalize(stmt);
        return NULL;
    }
    result[0] = '\0';
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* text = (const char*)sqlite3_column_text(stmt, 0);
        if (text != NULL) strcat(result, text);
    }
    sqlite3_finalize(stmt);
    return result;
}
#endif

static char* sidecar_path(Context* ctx) {
    if (ctx == NULL || ctx->db_path == NULL) return NULL;
    size_t len = strlen(ctx->db_path) + strlen(".packages") + 1;
    char* path = malloc(len);
    if (path == NULL) return NULL;
    snprintf(path, len, "%s.packages", ctx->db_path);
    return path;
}

/* Remove top-level proc main blocks so persisted package sources can be
   prepended to later programs without creating a duplicate main. */
static char* strip_main_procedure(const char* source) {
    size_t len = strlen(source);
    char* out = malloc(len + 1);
    if (out == NULL) return NULL;
    size_t j = 0;
    const char* p = source;
    while (*p != '\0') {
        const char* line_start = p;
        while (*p != '\0' && (*p == ' ' || *p == '\t')) p++;
        if (strncmp(p, "proc main", 9) == 0) {
            /* Skip to matching brace, accounting for nested braces and strings. */
            int depth = 0;
            int in_string = 0;
            int escape = 0;
            int found_open = 0;
            while (*p != '\0') {
                if (escape) {
                    escape = 0;
                    p++;
                    continue;
                }
                if (*p == '\\' && in_string) {
                    escape = 1;
                    p++;
                    continue;
                }
                if (*p == '"') {
                    in_string = !in_string;
                    p++;
                    continue;
                }
                if (in_string) {
                    p++;
                    continue;
                }
                if (*p == '{') {
                    found_open = 1;
                    depth++;
                } else if (*p == '}') {
                    depth--;
                }
                p++;
                if (found_open && depth == 0) break;
            }
            /* Skip trailing whitespace/newline after the block. */
            while (*p != '\0' && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
        } else {
            while (line_start < p) out[j++] = *line_start++;
            while (*p != '\0' && *p != '\n') out[j++] = *p++;
            if (*p == '\n') out[j++] = *p++;
        }
    }
    out[j] = '\0';
    return out;
}

#define PACKAGE_SOURCE_MARKER "// __AUSPEX_PACKAGE_SOURCE__"
/* Written by MyPL; still recognized until v0.4.0. */
#define LEGACY_PACKAGE_SOURCE_MARKER "// __MYPL_PACKAGE_SOURCE__"

static int custom_save_source(Context* ctx, const char* source, int append) {
    char* path = sidecar_path(ctx);
    if (path == NULL) return 1;

    char* stripped = strip_main_procedure(source);
    if (stripped == NULL) {
        free(path);
        return 0;
    }

    FILE* f = fopen(path, append ? "a" : "w");
    if (f == NULL) {
        free(path);
        free(stripped);
        return 0;
    }
    /* Source the REPL loaded from here and saves back already starts with
       the marker and ends with a newline: add neither a second time. */
    const char* body = stripped;
    while (*body == ' ' || *body == '\t' || *body == '\n' || *body == '\r') body++;
    int has_marker =
        strncmp(body, PACKAGE_SOURCE_MARKER, strlen(PACKAGE_SOURCE_MARKER)) == 0 ||
        strncmp(body, LEGACY_PACKAGE_SOURCE_MARKER, strlen(LEGACY_PACKAGE_SOURCE_MARKER)) == 0;
    size_t len = strlen(stripped);
    int ends_in_newline = len > 0 && stripped[len - 1] == '\n';
    fprintf(f, "%s%s%s", has_marker ? "" : PACKAGE_SOURCE_MARKER "\n", stripped,
            ends_in_newline ? "" : "\n");
    fclose(f);
    free(path);
    free(stripped);
    return 1;
}

static char* custom_load_source(Context* ctx) {
    char* path = sidecar_path(ctx);
    if (path == NULL) return NULL;
    char* data = os_read_file(path);
    free(path);
    return data;
}

char* packages_load_source(DBDriver* driver, Context* ctx) {
    if (driver == NULL) {
        /* No driver means the custom engine is being used directly via ctx. */
        return custom_load_source(ctx);
    }
    if (driver->is_sqlite) {
#ifdef USE_SQLITE
        return sqlite_load_source(driver);
#else
        (void)driver;
        return NULL;
#endif
    }
    return custom_load_source(ctx);
}

int packages_save_source(DBDriver* driver, Context* ctx, const char* source, int append) {
    if (source == NULL) return 1;
    if (driver == NULL) {
        /* No driver means the custom engine is being used directly via ctx. */
        return custom_save_source(ctx, source, append);
    }
    if (driver->is_sqlite) {
#ifdef USE_SQLITE
        return sqlite_save_source(driver, source, append);
#else
        (void)driver; (void)source; (void)append;
        return 1;
#endif
    }
    return custom_save_source(ctx, source, append);
}
