#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stored_programs.h"
#include "os.h"

#ifdef USE_SQLITE
#include "sqlite_driver.h"
#endif

typedef struct {
    char* type;
    char* name;
    char* authid;
    char* source;
} ProgramUnit;

#ifdef USE_SQLITE
static const char* PROGRAM_UNITS_TABLE =
    "CREATE TABLE IF NOT EXISTS _auspex_program_units ("
    "    name TEXT NOT NULL,"
    "    unit_type TEXT NOT NULL,"
    "    source_text TEXT NOT NULL,"
    "    authid TEXT,"
    "    PRIMARY KEY (name, unit_type)"
    ")";
#endif

static char* sidecar_path(Context* ctx) {
    if (ctx == NULL || ctx->db_path == NULL) return NULL;
    size_t len = strlen(ctx->db_path) + strlen(".programs") + 1;
    char* path = malloc(len);
    if (path == NULL) return NULL;
    snprintf(path, len, "%s.programs", ctx->db_path);
    return path;
}

static void free_unit(ProgramUnit* unit) {
    free(unit->type);
    free(unit->name);
    free(unit->authid);
    free(unit->source);
}

static void free_units(ProgramUnit* units, int count) {
    for (int i = 0; i < count; i++) {
        free_unit(&units[i]);
    }
    free(units);
}

static int add_unit(ProgramUnit** units, int* count, int* cap, const ProgramUnit* unit) {
    if (*count >= *cap) {
        int newcap = *cap == 0 ? 8 : *cap * 2;
        ProgramUnit* n = realloc(*units, sizeof(ProgramUnit) * (size_t)newcap);
        if (n == NULL) return 0;
        *units = n;
        *cap = newcap;
    }
    (*units)[*count] = *unit;
    (*count)++;
    return 1;
}

static int is_id_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int is_id_cont(char c) {
    return is_id_start(c) || (c >= '0' && c <= '9');
}

static int keyword_eq(const char* p, const char* kw) {
    size_t len = strlen(kw);
    return strncasecmp(p, kw, len) == 0 && !is_id_cont(p[len]);
}

static int token_eq(const char* p, const char* token) {
    const char* start = p;
    while (is_id_cont(*p)) p++;
    size_t len = (size_t)(p - start);
    size_t tlen = strlen(token);
    if (len != tlen) return 0;
    return strncasecmp(start, token, len) == 0;
}

/* Advance past whitespace, line comments, block comments, and string literals.
   The string/escape state is maintained across calls. */
static const char* skip_ws_comments_strings(const char* p, int* in_string, int* escape) {
    while (1) {
        if (*in_string) {
            if (*p == '\0') return p;
            if (*escape) {
                *escape = 0;
                p++;
                continue;
            }
            if (*p == '\\') {
                *escape = 1;
                p++;
                continue;
            }
            if (*p == '"') {
                *in_string = 0;
                p++;
                continue;
            }
            p++;
            continue;
        }

        unsigned char ch = (unsigned char)*p;
        if (isspace(ch)) {
            p++;
            continue;
        }
        if (*p == '/' && *(p + 1) == '/') {
            while (*p != '\0' && *p != '\n') p++;
            continue;
        }
        if (*p == '/' && *(p + 1) == '*') {
            p += 2;
            while (*p != '\0' && !(*p == '*' && *(p + 1) == '/')) p++;
            if (*p != '\0') p += 2;
            continue;
        }
        if (*p == '"') {
            *in_string = 1;
            p++;
            continue;
        }
        break;
    }
    return p;
}

/* Advance past a package/package body block (delimited by `end name;` or
   `end package;`), maintaining string/comment state. Returns 1 on success. */
static int skip_package_block(const char** p, int* in_string, int* escape) {
    /* *p points at the `package` keyword. */
    *p += 7; /* strlen("package") */
    *p = skip_ws_comments_strings(*p, in_string, escape);

    if (keyword_eq(*p, "body")) {
        *p += 4;
        *p = skip_ws_comments_strings(*p, in_string, escape);
    }

    char name[256];
    name[0] = '\0';
    if (is_id_start(**p)) {
        const char* ns = *p;
        while (is_id_cont(**p)) (*p)++;
        size_t nlen = (size_t)(*p - ns);
        if (nlen >= sizeof(name)) nlen = sizeof(name) - 1;
        memcpy(name, ns, nlen);
        name[nlen] = '\0';
    }

    int depth = 1;
    while (**p != '\0' && depth > 0) {
        *p = skip_ws_comments_strings(*p, in_string, escape);
        if (**p == '\0') break;

        if (keyword_eq(*p, "package")) {
            *p += 7;
            depth++;
            continue;
        }

        if (keyword_eq(*p, "end")) {
            const char* q = *p + 3;
            q = skip_ws_comments_strings(q, in_string, escape);
            if (keyword_eq(q, "package") || token_eq(q, name)) {
                if (keyword_eq(q, "package")) {
                    q += 7;
                } else {
                    while (is_id_cont(*q)) q++;
                }
                q = skip_ws_comments_strings(q, in_string, escape);
                if (*q == ';') q++;
                *p = q;
                depth--;
                continue;
            }
        }

        if (is_id_start(**p)) {
            while (is_id_cont(**p)) (*p)++;
        } else {
            (*p)++;
        }
    }

    return depth == 0;
}

/* Extract a single top-level proc/func/trigger unit beginning at `start`.
   Returns 1 and sets *out_end to the character after the matching `}` on success. */
static int parse_unit(const char* start, const char** out_end, ProgramUnit* unit) {
    memset(unit, 0, sizeof(*unit));

    int is_proc = keyword_eq(start, "proc");
    int is_func = keyword_eq(start, "func");
    int is_trigger = keyword_eq(start, "trigger");
    if (!is_proc && !is_func && !is_trigger) return 0;

    unit->type = strdup(is_proc ? "PROCEDURE" : is_func ? "FUNCTION" : "TRIGGER");
    if (unit->type == NULL) return 0;

    const char* p = start + (is_trigger ? 7 : 4); /* strlen("trigger") vs "proc"/"func" */
    int in_string = 0;
    int escape = 0;

    p = skip_ws_comments_strings(p, &in_string, &escape);
    if (!is_id_start(*p)) {
        free_unit(unit);
        return 0;
    }

    const char* name_start = p;
    while (is_id_cont(*p)) p++;
    size_t name_len = (size_t)(p - name_start);
    unit->name = malloc(name_len + 1);
    if (unit->name == NULL) {
        free_unit(unit);
        return 0;
    }
    memcpy(unit->name, name_start, name_len);
    unit->name[name_len] = '\0';

    /* Scan the signature for an optional authid clause and the opening `{`. */
    const char* authid_clause_start = NULL;
    const char* authid_clause_end = NULL;

    while (*p != '\0') {
        p = skip_ws_comments_strings(p, &in_string, &escape);
        if (*p == '\0') break;
        if (*p == '{') break;

        if (keyword_eq(p, "authid")) {
            authid_clause_start = p;
            p += 6; /* strlen("authid") */
            p = skip_ws_comments_strings(p, &in_string, &escape);
            if (is_id_start(*p)) {
                const char* astart = p;
                while (is_id_cont(*p)) p++;
                size_t alen = (size_t)(p - astart);
                unit->authid = malloc(alen + 1);
                if (unit->authid != NULL) {
                    memcpy(unit->authid, astart, alen);
                    unit->authid[alen] = '\0';
                }
                authid_clause_end = p;
            }
            continue;
        }

        if (is_id_start(*p)) {
            while (is_id_cont(*p)) p++;
        } else {
            p++;
        }
    }

    if (*p != '{') {
        free_unit(unit);
        return 0;
    }

    /* Walk to the matching closing brace, accounting for nesting and strings. */
    int depth = 0;
    while (*p != '\0') {
        p = skip_ws_comments_strings(p, &in_string, &escape);
        if (*p == '\0') break;
        if (*p == '{') {
            depth++;
            p++;
            continue;
        }
        if (*p == '}') {
            depth--;
            p++;
            if (depth == 0) break;
            continue;
        }
        p++;
    }

    if (depth != 0) {
        free_unit(unit);
        return 0;
    }

    *out_end = p;

    /* Strip the authid clause from the stored source so it remains valid Auspex. */
    if (authid_clause_start != NULL && authid_clause_end != NULL) {
        size_t prefix_len = (size_t)(authid_clause_start - start);
        size_t suffix_len = (size_t)(p - authid_clause_end);
        unit->source = malloc(prefix_len + suffix_len + 1);
        if (unit->source == NULL) {
            free_unit(unit);
            return 0;
        }
        if (prefix_len > 0) memcpy(unit->source, start, prefix_len);
        if (suffix_len > 0) memcpy(unit->source + prefix_len, authid_clause_end, suffix_len);
        unit->source[prefix_len + suffix_len] = '\0';
    } else {
        size_t source_len = (size_t)(p - start);
        unit->source = malloc(source_len + 1);
        if (unit->source == NULL) {
            free_unit(unit);
            return 0;
        }
        memcpy(unit->source, start, source_len);
        unit->source[source_len] = '\0';
    }
    return 1;
}

/* Scan source for top-level proc/func/trigger declarations. */
static int extract_units(const char* source, ProgramUnit** out_units, int* out_count) {
    *out_units = NULL;
    *out_count = 0;
    int cap = 0;

    const char* p = source;
    int depth = 0;
    int in_string = 0;
    int escape = 0;

    while (*p != '\0') {
        p = skip_ws_comments_strings(p, &in_string, &escape);
        if (*p == '\0') break;

        if (*p == '{') {
            depth++;
            p++;
            continue;
        }
        if (*p == '}') {
            if (depth > 0) depth--;
            p++;
            continue;
        }

        if (depth == 0 && keyword_eq(p, "package")) {
            skip_package_block(&p, &in_string, &escape);
            continue;
        }

        if (depth == 0 && (keyword_eq(p, "proc") || keyword_eq(p, "func") ||
                           keyword_eq(p, "trigger"))) {
            const char* end;
            ProgramUnit unit;
            if (parse_unit(p, &end, &unit)) {
                int skip = (strcmp(unit.type, "PROCEDURE") == 0 && strcmp(unit.name, "main") == 0);
                if (!skip) {
                    if (!add_unit(out_units, out_count, &cap, &unit)) {
                        free_unit(&unit);
                        free_units(*out_units, *out_count);
                        *out_units = NULL;
                        *out_count = 0;
                        return 0;
                    }
                } else {
                    free_unit(&unit);
                }
                p = end;
                continue;
            }
        }

        if (is_id_start(*p)) {
            while (is_id_cont(*p)) p++;
        } else {
            p++;
        }
    }

    return 1;
}

#ifdef USE_SQLITE
static char* sqlite_load_source(DBDriver* driver) {
    sqlite3* db = ((SQLiteImpl*)driver->impl)->db;
    sqlite3_stmt* check = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT name FROM sqlite_master WHERE type='table' AND name='_auspex_program_units'",
                           -1, &check, NULL) != SQLITE_OK) {
        return NULL;
    }
    int exists = sqlite3_step(check) == SQLITE_ROW;
    sqlite3_finalize(check);
    if (!exists) return NULL;

    sqlite3_stmt* stmt = NULL;
    const char* sql = "SELECT source_text FROM _auspex_program_units ORDER BY rowid";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return NULL;
    }

    size_t total = 0;
    int count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* text = (const char*)sqlite3_column_text(stmt, 0);
        if (text != NULL) {
            total += strlen(text);
            count++;
        }
    }

    if (count == 0) {
        sqlite3_finalize(stmt);
        return NULL;
    }

    char* result = malloc(total + (size_t)count);
    if (result == NULL) {
        sqlite3_finalize(stmt);
        return NULL;
    }
    result[0] = '\0';

    sqlite3_reset(stmt);
    int first = 1;
    char* write = result;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* text = (const char*)sqlite3_column_text(stmt, 0);
        if (text != NULL) {
            if (!first) {
                *write = '\n';
                write++;
            }
            size_t len = strlen(text);
            memcpy(write, text, len);
            write += len;
            first = 0;
        }
    }
    *write = '\0';
    sqlite3_finalize(stmt);
    return result;
}

static int sqlite_save_source(DBDriver* driver, Context* ctx, const char* source) {
    (void)ctx;
    sqlite3* db = ((SQLiteImpl*)driver->impl)->db;
    char* err = NULL;
    if (sqlite3_exec(db, PROGRAM_UNITS_TABLE, NULL, NULL, &err) != SQLITE_OK) {
        if (err != NULL) {
            snprintf(driver->error_message, sizeof(driver->error_message), "%s", err);
            sqlite3_free(err);
        }
        return 0;
    }

    ProgramUnit* units = NULL;
    int count = 0;
    if (!extract_units(source, &units, &count)) {
        return 0;
    }
    if (count == 0) {
        return 1;
    }

    sqlite3_stmt* stmt = NULL;
    const char* sql =
        "INSERT INTO _auspex_program_units (name, unit_type, source_text, authid) "
        "VALUES (?1, ?2, ?3, ?4) "
        "ON CONFLICT(name, unit_type) DO UPDATE SET "
        "source_text = excluded.source_text, authid = excluded.authid";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        snprintf(driver->error_message, sizeof(driver->error_message), "%s",
                 sqlite3_errmsg(db));
        free_units(units, count);
        return 0;
    }

    int ok = 1;
    for (int i = 0; i < count; i++) {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        if (sqlite3_bind_text(stmt, 1, units[i].name, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_bind_text(stmt, 2, units[i].type, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_bind_text(stmt, 3, units[i].source, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_bind_text(stmt, 4, units[i].authid ? units[i].authid : "", -1,
                              SQLITE_TRANSIENT) != SQLITE_OK) {
            snprintf(driver->error_message, sizeof(driver->error_message), "%s",
                     sqlite3_errmsg(db));
            ok = 0;
            break;
        }
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            snprintf(driver->error_message, sizeof(driver->error_message), "%s",
                     sqlite3_errmsg(db));
            ok = 0;
            break;
        }
    }

    sqlite3_finalize(stmt);
    free_units(units, count);
    return ok;
}
#endif

static int find_unit(ProgramUnit* units, int count, const char* name, const char* type) {
    for (int i = 0; i < count; i++) {
        if (strcmp(units[i].name, name) == 0 && strcmp(units[i].type, type) == 0) {
            return i;
        }
    }
    return -1;
}

static void str_toupper(char* dest, const char* src, size_t size) {
    size_t i;
    for (i = 0; i + 1 < size && src[i] != '\0'; i++) {
        dest[i] = (char)toupper((unsigned char)src[i]);
    }
    dest[i] = '\0';
}

#define UNIT_MARKER "// __AUSPEX_PROGRAM_UNIT__ "
/* Written by MyPL; still read until v0.4.0. */
#define LEGACY_UNIT_MARKER "// __MYPL_PROGRAM_UNIT__ "

/* The first unit marker (current or legacy) at or after p; its length goes
   to *len. NULL when there is none. */
static const char* find_unit_marker(const char* p, size_t* len) {
    const char* m = strstr(p, UNIT_MARKER);
    const char* legacy = strstr(p, LEGACY_UNIT_MARKER);
    if (legacy != NULL && (m == NULL || legacy < m)) {
        *len = strlen(LEGACY_UNIT_MARKER);
        return legacy;
    }
    *len = strlen(UNIT_MARKER);
    return m;
}

static int parse_sidecar(const char* data, ProgramUnit** out_units, int* out_count) {
    *out_units = NULL;
    *out_count = 0;
    int cap = 0;

    size_t mlen = 0;
    const char* p = data;

    while (*p != '\0') {
        const char* m = find_unit_marker(p, &mlen);
        if (m == NULL) break;

        const char* line_end = strchr(m, '\n');
        if (line_end == NULL) {
            line_end = m + strlen(m);
        } else {
            line_end++;
        }

        const char* t = m + mlen;
        while (*t == ' ' || *t == '\t') t++;

        char type[32];
        char name[256];
        char authid[256];
        type[0] = '\0';
        name[0] = '\0';
        authid[0] = '\0';

        const char* ts = t;
        while (*t != '\0' && *t != ' ' && *t != '\t' && *t != '\n' && *t != '\r') t++;
        size_t tlen = (size_t)(t - ts);
        if (tlen >= sizeof(type)) tlen = sizeof(type) - 1;
        memcpy(type, ts, tlen);
        type[tlen] = '\0';
        for (size_t k = 0; type[k] != '\0'; k++) {
            type[k] = (char)toupper((unsigned char)type[k]);
        }

        while (*t == ' ' || *t == '\t') t++;
        const char* ns = t;
        while (*t != '\0' && *t != ' ' && *t != '\t' && *t != '\n' && *t != '\r') t++;
        size_t nlen = (size_t)(t - ns);
        if (nlen >= sizeof(name)) nlen = sizeof(name) - 1;
        memcpy(name, ns, nlen);
        name[nlen] = '\0';

        while (*t == ' ' || *t == '\t') t++;
        if (strncasecmp(t, "AUTHID", 6) == 0 && !is_id_cont(t[6])) {
            t += 6;
            while (*t == ' ' || *t == '\t') t++;
            const char* as = t;
            while (*t != '\0' && *t != ' ' && *t != '\t' && *t != '\n' && *t != '\r') t++;
            size_t alen = (size_t)(t - as);
            if (alen >= sizeof(authid)) alen = sizeof(authid) - 1;
            memcpy(authid, as, alen);
            authid[alen] = '\0';
        }

        const char* body_start = line_end;
        size_t next_len = 0;
        const char* next = find_unit_marker(body_start, &next_len);
        const char* body_end = next ? next : (body_start + strlen(body_start));
        while (body_end > body_start &&
               (*(body_end - 1) == '\n' || *(body_end - 1) == '\r')) {
            body_end--;
        }

        size_t slen = (size_t)(body_end - body_start);
        ProgramUnit u = {0};
        u.type = strdup(type);
        u.name = strdup(name);
        u.authid = authid[0] != '\0' ? strdup(authid) : NULL;
        if (u.type == NULL || u.name == NULL ||
            (authid[0] != '\0' && u.authid == NULL)) {
            free_unit(&u);
            free_units(*out_units, *out_count);
            *out_units = NULL;
            *out_count = 0;
            return 0;
        }
        u.source = malloc(slen + 1);
        if (u.source == NULL) {
            free_unit(&u);
            free_units(*out_units, *out_count);
            *out_units = NULL;
            *out_count = 0;
            return 0;
        }
        if (slen > 0) memcpy(u.source, body_start, slen);
        u.source[slen] = '\0';

        if (!add_unit(out_units, out_count, &cap, &u)) {
            free_unit(&u);
            free_units(*out_units, *out_count);
            *out_units = NULL;
            *out_count = 0;
            return 0;
        }

        p = next ? next : body_end;
    }

    return 1;
}

static void write_unit(FILE* f, const ProgramUnit* unit) {
    if (unit->authid != NULL && unit->authid[0] != '\0') {
        char authid_upper[256];
        str_toupper(authid_upper, unit->authid, sizeof(authid_upper));
        fprintf(f, UNIT_MARKER "%s %s AUTHID %s\n%s\n",
                unit->type, unit->name, authid_upper, unit->source);
    } else {
        fprintf(f, UNIT_MARKER "%s %s\n%s\n",
                unit->type, unit->name, unit->source);
    }
}

static int custom_save_source(DBDriver* driver, Context* ctx, const char* source) {
    (void)driver;
    ProgramUnit* current = NULL;
    int current_count = 0;
    if (!extract_units(source, &current, &current_count)) {
        return 0;
    }

    char* path = sidecar_path(ctx);
    if (path == NULL) {
        free_units(current, current_count);
        return 1;
    }

    ProgramUnit* merged = NULL;
    int merged_count = 0;
    int cap = 0;

    char* existing = os_read_file(path);
    if (existing != NULL) {
        parse_sidecar(existing, &merged, &merged_count);
        free(existing);
        /* parse_sidecar grows the array with its own capacity tracking;
         * seed cap from the loaded count so add_unit grows correctly. */
        cap = merged_count;
    }

    FILE* f = fopen(path, "w");
    if (f == NULL) {
        free_units(merged, merged_count);
        free_units(current, current_count);
        free(path);
        return 0;
    }

    for (int i = 0; i < current_count; i++) {
        int idx = find_unit(merged, merged_count, current[i].name, current[i].type);
        if (idx >= 0) {
            char* new_source = strdup(current[i].source);
            if (new_source == NULL) {
                fclose(f);
                free_units(merged, merged_count);
                free_units(current, current_count);
                free(path);
                return 0;
            }
            char* new_authid = current[i].authid ? strdup(current[i].authid) : NULL;
            if (current[i].authid != NULL && new_authid == NULL) {
                free(new_source);
                fclose(f);
                free_units(merged, merged_count);
                free_units(current, current_count);
                free(path);
                return 0;
            }
            free(merged[idx].source);
            merged[idx].source = new_source;
            free(merged[idx].authid);
            merged[idx].authid = new_authid;
        } else {
            ProgramUnit u = {0};
            u.type = strdup(current[i].type);
            if (u.type == NULL) {
                free_unit(&u);
                fclose(f);
                free_units(merged, merged_count);
                free_units(current, current_count);
                free(path);
                return 0;
            }
            u.name = strdup(current[i].name);
            if (u.name == NULL) {
                free_unit(&u);
                fclose(f);
                free_units(merged, merged_count);
                free_units(current, current_count);
                free(path);
                return 0;
            }
            u.authid = current[i].authid ? strdup(current[i].authid) : NULL;
            if (current[i].authid != NULL && u.authid == NULL) {
                free_unit(&u);
                fclose(f);
                free_units(merged, merged_count);
                free_units(current, current_count);
                free(path);
                return 0;
            }
            u.source = strdup(current[i].source);
            if (u.source == NULL) {
                free_unit(&u);
                fclose(f);
                free_units(merged, merged_count);
                free_units(current, current_count);
                free(path);
                return 0;
            }
            if (!add_unit(&merged, &merged_count, &cap, &u)) {
                free_unit(&u);
                fclose(f);
                free_units(merged, merged_count);
                free_units(current, current_count);
                free(path);
                return 0;
            }
        }
    }

    for (int i = 0; i < merged_count; i++) {
        write_unit(f, &merged[i]);
    }

    fclose(f);
    free(path);
    free_units(merged, merged_count);
    free_units(current, current_count);
    return 1;
}

static char* custom_load_source(Context* ctx) {
    char* path = sidecar_path(ctx);
    if (path == NULL) return NULL;
    char* data = os_read_file(path);
    free(path);
    if (data == NULL || *data == '\0') {
        free(data);
        return NULL;
    }
    return data;
}

char* stored_programs_filter_redefined(const char* loaded, const char* source) {
    if (loaded == NULL || *loaded == '\0') return NULL;
    if (source == NULL) return strdup(loaded);

    ProgramUnit* loaded_units = NULL;
    int loaded_count = 0;
    if (!extract_units(loaded, &loaded_units, &loaded_count)) {
        return strdup(loaded);
    }

    ProgramUnit* current = NULL;
    int current_count = 0;
    if (!extract_units(source, &current, &current_count)) {
        free_units(loaded_units, loaded_count);
        return strdup(loaded);
    }

    size_t total = 0;
    int kept = 0;
    for (int i = 0; i < loaded_count; i++) {
        if (find_unit(current, current_count, loaded_units[i].name,
                      loaded_units[i].type) >= 0) {
            continue;
        }
        total += strlen(loaded_units[i].source) + 1;
        kept++;
    }

    if (kept == 0) {
        free_units(loaded_units, loaded_count);
        free_units(current, current_count);
        return NULL;
    }

    char* result = malloc(total + 1);
    if (result == NULL) {
        free_units(loaded_units, loaded_count);
        free_units(current, current_count);
        return strdup(loaded);
    }

    char* write = result;
    for (int i = 0; i < loaded_count; i++) {
        if (find_unit(current, current_count, loaded_units[i].name,
                      loaded_units[i].type) >= 0) {
            continue;
        }
        size_t len = strlen(loaded_units[i].source);
        if (len > 0) {
            memcpy(write, loaded_units[i].source, len);
            write += len;
        }
        *write = '\n';
        write++;
    }
    *write = '\0';

    free_units(loaded_units, loaded_count);
    free_units(current, current_count);
    return result;
}

char* stored_programs_load_source(DBDriver* driver, Context* ctx) {
    if (driver == NULL) {
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

int stored_programs_save_source(DBDriver* driver, Context* ctx, const char* source) {
    if (source == NULL) return 1;
    if (driver == NULL) {
        return custom_save_source(driver, ctx, source);
    }
    if (driver->is_sqlite) {
#ifdef USE_SQLITE
        return sqlite_save_source(driver, ctx, source);
#else
        (void)driver; (void)ctx; (void)source;
        return 1;
#endif
    }
    return custom_save_source(driver, ctx, source);
}

#ifdef USE_SQLITE
static int sqlite_drop_unit(DBDriver* driver, const char* name, const char* type) {
    sqlite3* db = ((SQLiteImpl*)driver->impl)->db;
    sqlite3_stmt* check = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT name FROM sqlite_master WHERE type='table' AND name='_auspex_program_units'",
                           -1, &check, NULL) != SQLITE_OK) {
        return 0;
    }
    int exists = sqlite3_step(check) == SQLITE_ROW;
    sqlite3_finalize(check);
    if (!exists) return 1;

    sqlite3_stmt* stmt = NULL;
    const char* sql =
        "DELETE FROM _auspex_program_units WHERE name = ?1 COLLATE NOCASE AND unit_type = ?2";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        snprintf(driver->error_message, sizeof(driver->error_message), "%s",
                 sqlite3_errmsg(db));
        return 0;
    }
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, type, -1, SQLITE_TRANSIENT);
    int ok = sqlite3_step(stmt) == SQLITE_DONE;
    if (!ok) {
        snprintf(driver->error_message, sizeof(driver->error_message), "%s",
                 sqlite3_errmsg(db));
    }
    sqlite3_finalize(stmt);
    return ok;
}
#endif

static int custom_drop_unit(Context* ctx, const char* name, const char* type) {
    char* path = sidecar_path(ctx);
    if (path == NULL) return 1;

    char* existing = os_read_file(path);
    if (existing == NULL) {
        free(path);
        return 1;
    }

    ProgramUnit* units = NULL;
    int count = 0;
    if (!parse_sidecar(existing, &units, &count)) {
        free(existing);
        free(path);
        return 0;
    }
    free(existing);

    FILE* f = fopen(path, "w");
    if (f == NULL) {
        free_units(units, count);
        free(path);
        return 0;
    }
    for (int i = 0; i < count; i++) {
        if (strcmp(units[i].type, type) == 0 && strcasecmp(units[i].name, name) == 0) {
            continue; /* dropped */
        }
        write_unit(f, &units[i]);
    }
    fclose(f);
    free_units(units, count);
    free(path);
    return 1;
}

int stored_programs_drop_unit(DBDriver* driver, Context* ctx, const char* name, const char* type) {
    if (name == NULL || type == NULL) return 0;
    if (driver != NULL && driver->is_sqlite) {
#ifdef USE_SQLITE
        return sqlite_drop_unit(driver, name, type);
#else
        (void)driver;
        return 1;
#endif
    }
    return custom_drop_unit(ctx, name, type);
}
