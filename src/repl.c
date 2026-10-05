#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <termios.h>
#endif

#include "repl.h"
#include "compiler.h"
#include "os.h"
#include "packages.h"
#include "sql_engine.h"
#ifdef USE_SQLITE
#include "sqlite_driver.h"
#endif
#include "vm.h"

#define LINE_SIZE 1024
#define REPL_HISTORY_SIZE 100

typedef struct {
    char* data;
    size_t len;
    size_t cap;
} StringBuffer;

typedef struct {
    StringBuffer procedures;
    StringBuffer main_body;
    char* history[REPL_HISTORY_SIZE];
    int history_count;
    int history_index;
    Chunk chunk;
    VM* vm;
    Context ctx;
    DBDriver driver;
    int driver_open;
    /* Incremental compilation engine (Phase 13 #37): one persistent chunk and
       one persistent compiler across inputs; each input compiles only its own
       fragment. An input that fails to compile is dropped (#58). The stuck
       states reproduce the old engine's poison semantics where they remain:
       after a runtime error, or when the persisted definitions loaded at
       startup do not compile. */
    ReplCompiler* compiler;
    int pending_init_offset;      /* queued persisted-package init run, -1 none */
    int compile_stuck;            /* persisted definitions failed to compile */
    char compile_stuck_error[256];
    int runtime_stuck;            /* session poisoned by a runtime error */
    char runtime_stuck_error[256];
    int runtime_stuck_inputs;     /* code inputs seen while runtime-stuck */
} ReplSession;

static void string_buffer_init(StringBuffer* buf) {
    buf->data = malloc(256);
    buf->len = 0;
    buf->cap = 256;
    if (buf->data != NULL) buf->data[0] = '\0';
}

static void string_buffer_free(StringBuffer* buf) {
    free(buf->data);
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

static int string_buffer_append(StringBuffer* buf, const char* text) {
    if (buf->data == NULL || text == NULL) return 0;
    size_t text_len = strlen(text);
    size_t needed = buf->len + text_len + 1;
    if (needed > buf->cap) {
        size_t new_cap = buf->cap * 2;
        while (new_cap < needed) new_cap *= 2;
        char* new_data = realloc(buf->data, new_cap);
        if (new_data == NULL) return 0;
        buf->data = new_data;
        buf->cap = new_cap;
    }
    memcpy(buf->data + buf->len, text, text_len + 1);
    buf->len += text_len;
    return 1;
}

static void string_buffer_truncate(StringBuffer* buf, size_t len) {
    if (buf->data == NULL || len > buf->len) return;
    buf->len = len;
    buf->data[len] = '\0';
}

/* ----- Top-level proc definitions in REPL source -----
   The procedures buffer is plain source text. When an input redefines a
   proc, the old definition's text has to leave it: .defs prints the buffer,
   and it is persisted and recompiled as one fragment at the next startup,
   where two definitions of one name are a duplicate. */

typedef struct {
    size_t start;
    size_t end;   /* past the closing '}' and one trailing newline */
    char name[128];
} ProcSpan;

#define MAX_PROC_SPANS 64

static int is_ident_char(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

static int word_at(const char* t, size_t i, size_t n, const char* word) {
    size_t len = strlen(word);
    if (i + len > n || strncmp(t + i, word, len) != 0) return 0;
    if (i > 0 && is_ident_char(t[i - 1])) return 0;
    return i + len == n || !is_ident_char(t[i + len]);
}

/* Index past a string literal or comment starting at i, or i if none does. */
static size_t skip_literal(const char* t, size_t i, size_t n) {
    if (t[i] == '"' || t[i] == '\'') {
        char quote = t[i++];
        while (i < n && t[i] != quote) {
            if (t[i] == '\\' && i + 1 < n) i++;
            i++;
        }
        return i < n ? i + 1 : n;
    }
    if (t[i] == '/' && i + 1 < n && t[i + 1] == '/') {
        while (i < n && t[i] != '\n') i++;
        return i;
    }
    if (t[i] == '/' && i + 1 < n && t[i + 1] == '*') {
        i += 2;
        while (i + 1 < n && !(t[i] == '*' && t[i + 1] == '/')) i++;
        return i + 1 < n ? i + 2 : n;
    }
    return i;
}

static size_t skip_space(const char* t, size_t i, size_t n) {
    while (i < n && isspace((unsigned char)t[i])) i++;
    return i;
}

/* Index past a package spec or body starting at i ("package [body] NAME
   ... end NAME;"). Its members are package procs, not top-level ones. */
static size_t skip_package(const char* t, size_t i, size_t n) {
    i = skip_space(t, i + strlen("package"), n);
    if (word_at(t, i, n, "body")) i = skip_space(t, i + strlen("body"), n);
    size_t name_start = i;
    while (i < n && is_ident_char(t[i])) i++;
    size_t name_len = i - name_start;
    if (name_len == 0) return i;
    while (i < n) {
        size_t j = skip_literal(t, i, n);
        if (j != i) {
            i = j;
            continue;
        }
        if (word_at(t, i, n, "end")) {
            size_t k = skip_space(t, i + 3, n);
            if (k + name_len <= n && strncmp(t + k, t + name_start, name_len) == 0 &&
                (k + name_len == n || !is_ident_char(t[k + name_len]))) {
                k = skip_space(t, k + name_len, n);
                if (k < n && t[k] == ';') return k + 1;
            }
        }
        i++;
    }
    return n;
}

/* Fills `out` with the top-level `proc NAME(...) ... { ... }` definitions in
   t[0, n) and returns how many there are. */
static int find_top_level_procs(const char* t, size_t n, ProcSpan* out, int max) {
    int count = 0;
    int depth = 0;
    size_t i = 0;
    while (i < n) {
        size_t j = skip_literal(t, i, n);
        if (j != i) {
            i = j;
            continue;
        }
        if (t[i] == '{') {
            depth++;
        } else if (t[i] == '}') {
            if (depth > 0) depth--;
        } else if (depth == 0 && word_at(t, i, n, "package")) {
            i = skip_package(t, i, n);
            continue;
        } else if (depth == 0 && word_at(t, i, n, "proc")) {
            size_t start = i;
            size_t k = skip_space(t, i + strlen("proc"), n);
            size_t name_start = k;
            while (k < n && is_ident_char(t[k])) k++;
            size_t name_len = k - name_start;
            k = skip_space(t, k, n);
            if (name_len == 0 || name_len >= sizeof(out[0].name) || k >= n || t[k] != '(') {
                i = k > i ? k : i + 1;
                continue;
            }
            /* The body is the first brace block after the header. */
            int body_depth = 0;
            int in_body = 0;
            while (k < n) {
                size_t m = skip_literal(t, k, n);
                if (m != k) {
                    k = m;
                    continue;
                }
                if (t[k] == '{') {
                    body_depth++;
                    in_body = 1;
                } else if (t[k] == '}' && in_body && --body_depth == 0) {
                    k++;
                    break;
                }
                k++;
            }
            if (!in_body || body_depth != 0) return count;
            if (k < n && t[k] == '\n') k++;
            if (count < max) {
                out[count].start = start;
                out[count].end = k;
                memcpy(out[count].name, t + name_start, name_len);
                out[count].name[name_len] = '\0';
                count++;
            }
            i = k;
            continue;
        }
        i++;
    }
    return count;
}

/* After `new_text` compiled, which the procedures buffer holds from
   `new_start` on, drop the text of every earlier top-level definition of a
   proc it defines. */
static void remove_replaced_definitions(StringBuffer* procedures, size_t new_start) {
    ProcSpan fresh[MAX_PROC_SPANS];
    int fresh_count = find_top_level_procs(procedures->data + new_start,
                                           procedures->len - new_start,
                                           fresh, MAX_PROC_SPANS);
    for (int f = 0; f < fresh_count; f++) {
        ProcSpan old[MAX_PROC_SPANS];
        int old_count = find_top_level_procs(procedures->data, new_start,
                                             old, MAX_PROC_SPANS);
        /* Back to front, so earlier spans stay valid as later ones go. */
        for (int o = old_count - 1; o >= 0; o--) {
            if (strcmp(old[o].name, fresh[f].name) != 0) continue;
            size_t len = old[o].end - old[o].start;
            memmove(procedures->data + old[o].start, procedures->data + old[o].end,
                    procedures->len - old[o].end + 1);
            procedures->len -= len;
            new_start -= len;
        }
    }
}

static int brace_depth(const char* s) {
    int depth = 0;
    int in_string = 0;
    int escape = 0;
    for (const char* p = s; *p != '\0'; p++) {
        if (escape) {
            escape = 0;
            continue;
        }
        if (*p == '\\' && in_string) {
            escape = 1;
            continue;
        }
        if (*p == '"') {
            in_string = !in_string;
            continue;
        }
        if (in_string) continue;
        if (*p == '/' && *(p + 1) == '/') break;
        if (*p == '{') depth++;
        else if (*p == '}') depth--;
    }
    return depth;
}

static int package_is_complete(const char* s) {
    while (*s != '\0' && isspace((unsigned char)*s)) s++;
    const char* first = s;
    while (*first != '\0' && (*first == ' ' || *first == '\t')) first++;
    if (strncmp(first, "package ", 8) != 0 && strncmp(first, "create ", 7) != 0) return 1;

    int starts = 0;
    int ends = 0;
    const char* p = s;
    while (*p != '\0') {
        while (*p != '\0' && (*p == ' ' || *p == '\t' || *p == '\r')) p++;
        if (*p == '\0') break;
        if (strncmp(p, "package ", 8) == 0) {
            starts++;
        } else if (strncmp(p, "end ", 4) == 0) {
            const char* q = p;
            while (*q != '\0' && *q != '\n') q++;
            while (q > p && isspace((unsigned char)q[-1])) q--;
            if (q > p && q[-1] == ';') ends++;
        }
        while (*p != '\0' && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    return starts > 0 && starts == ends;
}

static int input_is_complete(const char* line) {
    return brace_depth(line) <= 0 && package_is_complete(line);
}

static void history_add(ReplSession* session, const char* line) {
    if (line == NULL || *line == '\0') return;
    /* Skip duplicates at the top of history. */
    if (session->history_count > 0 &&
        strcmp(session->history[session->history_count - 1], line) == 0) {
        return;
    }
    char* copy = strdup(line);
    if (copy == NULL) return;
    if (session->history_count == REPL_HISTORY_SIZE) {
        free(session->history[0]);
        memmove(session->history, session->history + 1,
                sizeof(char*) * (REPL_HISTORY_SIZE - 1));
        session->history_count--;
    }
    session->history[session->history_count++] = copy;
}

static void history_free(ReplSession* session) {
    for (int i = 0; i < session->history_count; i++) {
        free(session->history[i]);
    }
    session->history_count = 0;
    session->history_index = 0;
}

static int read_line_simple(const char* prompt, char* buf, size_t size) {
    printf("%s", prompt);
    fflush(stdout);
    if (fgets(buf, (int)size, stdin) == NULL) return 0;
    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';
    return 1;
}

/* Line editing with history needs raw terminal input. */
#ifndef _WIN32
static int read_line_tty(ReplSession* session, const char* prompt,
                         char* buf, size_t size) {
    printf("%s", prompt);
    fflush(stdout);

    struct termios old_tio, new_tio;
    if (tcgetattr(STDIN_FILENO, &old_tio) != 0) {
        return read_line_simple(prompt, buf, size);
    }
    new_tio = old_tio;
    new_tio.c_lflag &= ~(ICANON | ECHO);
    new_tio.c_cc[VMIN] = 1;
    new_tio.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &new_tio) != 0) {
        return read_line_simple(prompt, buf, size);
    }

    size_t len = 0;
    buf[0] = '\0';
    session->history_index = session->history_count;

    for (;;) {
        int c = getchar();
        if (c == EOF || c == '\004') { /* Ctrl-D */
            buf[0] = '\0';
            len = 0;
            break;
        }
        if (c == '\003') { /* Ctrl-C */
            printf("\n");
            buf[0] = '\0';
            len = 0;
            break;
        }
        if (c == '\r' || c == '\n') {
            printf("\n");
            break;
        }
        if (c == 127 || c == '\b') { /* Backspace */
            if (len > 0) {
                len--;
                buf[len] = '\0';
                printf("\b \b");
                fflush(stdout);
            }
            continue;
        }
        if (c == 27) { /* Escape sequence */
            int c1 = getchar();
            int c2 = getchar();
            if (c1 == '[') {
                if (c2 == 'A' && session->history_count > 0) { /* Up */
                    if (session->history_index > 0) {
                        session->history_index--;
                        const char* entry = session->history[session->history_index];
                        while (len > 0) {
                            printf("\b \b");
                            len--;
                        }
                        size_t elen = strlen(entry);
                        if (elen >= size) elen = size - 1;
                        memcpy(buf, entry, elen);
                        buf[elen] = '\0';
                        len = elen;
                        printf("%s", buf);
                        fflush(stdout);
                    }
                    continue;
                }
                if (c2 == 'B' && session->history_count > 0) { /* Down */
                    if (session->history_index < session->history_count) {
                        session->history_index++;
                    }
                    while (len > 0) {
                        printf("\b \b");
                        len--;
                    }
                    const char* entry = "";
                    if (session->history_index < session->history_count) {
                        entry = session->history[session->history_index];
                    }
                    size_t elen = strlen(entry);
                    if (elen >= size) elen = size - 1;
                    memcpy(buf, entry, elen);
                    buf[elen] = '\0';
                    len = elen;
                    printf("%s", buf);
                    fflush(stdout);
                    continue;
                }
            }
            continue;
        }
        if (isprint((unsigned char)c) && len + 1 < size) {
            buf[len++] = (char)c;
            buf[len] = '\0';
            putchar(c);
            fflush(stdout);
        }
    }

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_tio);
    return 1;
}
#else
static COORD console_position_after(COORD start, size_t offset, SHORT width) {
    COORD result = start;
    if (width <= 0) return result;
    size_t absolute = (size_t)start.X + offset;
    result.X = (SHORT)(absolute % (size_t)width);
    result.Y = (SHORT)(start.Y + absolute / (size_t)width);
    return result;
}

static void redraw_windows_line(HANDLE output, COORD start, SHORT width,
                                const WCHAR* line, size_t len,
                                size_t cursor, size_t* displayed_len) {
    DWORD written = 0;
    SetConsoleCursorPosition(output, start);
    if (len > 0) {
        WriteConsoleW(output, line, (DWORD)len, &written, NULL);
    }
    for (size_t i = len; i < *displayed_len; i++) {
        WriteConsoleW(output, L" ", 1, &written, NULL);
    }
    *displayed_len = len;
    SetConsoleCursorPosition(output,
                             console_position_after(start, cursor, width));
}

static size_t history_to_wide(const char* entry, WCHAR* line, size_t capacity) {
    if (entry == NULL || capacity == 0) return 0;
    int converted = MultiByteToWideChar(CP_UTF8, 0, entry, -1,
                                        line, (int)capacity);
    if (converted <= 0) {
        line[0] = L'\0';
        return 0;
    }
    return (size_t)converted - 1;
}

static void wide_line_to_utf8(const WCHAR* line, size_t len,
                              char* buf, size_t size) {
    if (size == 0) return;
    while (len > 0) {
        int needed = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                         line, (int)len, NULL, 0,
                                         NULL, NULL);
        if (needed > 0 && (size_t)needed < size) {
            int converted = WideCharToMultiByte(
                CP_UTF8, WC_ERR_INVALID_CHARS, line, (int)len,
                buf, needed, NULL, NULL);
            if (converted > 0) {
                buf[converted] = '\0';
                return;
            }
        }
        len--;
        if (len > 0 && line[len - 1] >= 0xD800 &&
            line[len - 1] <= 0xDBFF) {
            len--;
        }
    }
    buf[0] = '\0';
}

static int read_line_windows(ReplSession* session, const char* prompt,
                             char* buf, size_t size) {
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD old_mode;
    CONSOLE_SCREEN_BUFFER_INFO screen;
    if (input == INVALID_HANDLE_VALUE || output == INVALID_HANDLE_VALUE ||
        !GetConsoleMode(input, &old_mode) ||
        !GetConsoleScreenBufferInfo(output, &screen)) {
        return read_line_simple(prompt, buf, size);
    }

    DWORD raw_mode = old_mode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT |
                                  ENABLE_PROCESSED_INPUT);
    if (!SetConsoleMode(input, raw_mode)) {
        return read_line_simple(prompt, buf, size);
    }

    printf("%s", prompt);
    fflush(stdout);
    if (!GetConsoleScreenBufferInfo(output, &screen)) {
        SetConsoleMode(input, old_mode);
        return 0;
    }

    WCHAR* line = calloc(size, sizeof(WCHAR));
    if (line == NULL) {
        SetConsoleMode(input, old_mode);
        return 0;
    }
    size_t len = 0;
    size_t cursor = 0;
    size_t displayed_len = 0;
    COORD start = screen.dwCursorPosition;
    SHORT width = screen.dwSize.X;
    session->history_index = session->history_count;
    int result = 1;

    for (;;) {
        INPUT_RECORD record;
        DWORD count = 0;
        if (!ReadConsoleInputW(input, &record, 1, &count) || count == 0) {
            result = 0;
            break;
        }
        if (record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown) {
            continue;
        }

        KEY_EVENT_RECORD key = record.Event.KeyEvent;
        WORD code = key.wVirtualKeyCode;
        WCHAR ch = key.uChar.UnicodeChar;
        if (code == VK_RETURN) {
            DWORD written = 0;
            WriteConsoleW(output, L"\r\n", 2, &written, NULL);
            break;
        }
        if (ch == 4 || ch == 26) { /* Ctrl-D / Ctrl-Z */
            result = 0;
            break;
        }
        if (ch == 3) { /* Ctrl-C when processed input is disabled upstream */
            DWORD written = 0;
            WriteConsoleW(output, L"\r\n", 2, &written, NULL);
            len = 0;
            cursor = 0;
            line[0] = L'\0';
            break;
        }
        if (code == VK_LEFT) {
            if (cursor > 0) cursor--;
        } else if (code == VK_RIGHT) {
            if (cursor < len) cursor++;
        } else if (code == VK_HOME) {
            cursor = 0;
        } else if (code == VK_END) {
            cursor = len;
        } else if (code == VK_BACK) {
            if (cursor > 0) {
                memmove(line + cursor - 1, line + cursor,
                        (len - cursor + 1) * sizeof(WCHAR));
                cursor--;
                len--;
            }
        } else if (code == VK_DELETE) {
            if (cursor < len) {
                memmove(line + cursor, line + cursor + 1,
                        (len - cursor) * sizeof(WCHAR));
                len--;
            }
        } else if (code == VK_UP && session->history_count > 0) {
            if (session->history_index > 0) session->history_index--;
            len = history_to_wide(session->history[session->history_index],
                                  line, size);
            cursor = len;
        } else if (code == VK_DOWN && session->history_count > 0) {
            if (session->history_index < session->history_count) {
                session->history_index++;
            }
            if (session->history_index < session->history_count) {
                len = history_to_wide(
                    session->history[session->history_index], line, size);
            } else {
                len = 0;
                line[0] = L'\0';
            }
            cursor = len;
        } else if (ch >= L' ' && len + 1 < size) {
            memmove(line + cursor + 1, line + cursor,
                    (len - cursor + 1) * sizeof(WCHAR));
            line[cursor++] = ch;
            len++;
        } else {
            continue;
        }

        redraw_windows_line(output, start, width, line, len, cursor,
                            &displayed_len);
    }

    if (result) {
        wide_line_to_utf8(line, len, buf, size);
    } else {
        buf[0] = '\0';
    }
    free(line);
    SetConsoleMode(input, old_mode);
    return result;
}
#endif

static int repl_read_line(ReplSession* session, const char* prompt,
                          char* buf, size_t size) {
#ifndef _WIN32
    if (isatty(STDIN_FILENO)) {
        return read_line_tty(session, prompt, buf, size);
    }
#else
    return read_line_windows(session, prompt, buf, size);
#endif
    return read_line_simple(prompt, buf, size);
}

static void repl_session_init(ReplSession* session, const char* db_path) {
    string_buffer_init(&session->procedures);
    string_buffer_init(&session->main_body);
    session->history_count = 0;
    session->history_index = 0;
    init_chunk(&session->chunk);
    session->vm = vm_init();
    session->driver_open = 0;
    session->ctx.db_path = default_db_path();
    session->ctx.pager = NULL;
    session->compiler = NULL;
    session->pending_init_offset = -1;
    session->compile_stuck = 0;
    session->compile_stuck_error[0] = '\0';
    session->runtime_stuck = 0;
    session->runtime_stuck_error[0] = '\0';
    session->runtime_stuck_inputs = 0;

    if (db_path != NULL) {
#ifdef USE_SQLITE
        sqlite_driver_init(&session->driver);
        if (session->driver.open(&session->driver, db_path)) {
            session->driver_open = 1;
            if (session->vm != NULL) {
                vm_set_driver(session->vm, &session->driver);
            }
        } else {
            fprintf(stderr, "Could not open database: %s\n", db_path);
        }
#else
        fprintf(stderr, "SQLite support is disabled in this build\n");
#endif
    } else {
        catalog_open(&session->ctx);
        if (session->vm != NULL) {
            vm_set_context(session->vm, &session->ctx);
        }
    }

    DBDriver* driver = session->driver_open ? &session->driver : NULL;
    Context* ctx = session->driver_open ? NULL : &session->ctx;
    char* persisted = packages_load_source(driver, ctx);
    if (persisted != NULL) {
        /* Saved text ends in a newline already; adding one per load would
           grow the stored source by a blank line every session. */
        string_buffer_append(&session->procedures, persisted);
        size_t n = strlen(persisted);
        if (n == 0 || persisted[n - 1] != '\n') {
            string_buffer_append(&session->procedures, "\n");
        }
        free(persisted);
    }

    session->compiler = repl_compiler_create();
    if (session->compiler == NULL) {
        fprintf(stderr, "Out of memory\n");
        return;
    }
    /* Compile the persisted package/procedure definitions once so later
       inputs can call them. Their initializer run is queued and executed
       silently right before the first user fragment (the old engine ran the
       inits as part of the first input's validation run). A broken persisted
       source poisons the session exactly like a broken input did before. */
    if (session->procedures.len > 0) {
        char error[256];
        if (repl_compiler_compile(session->compiler, session->procedures.data,
                                  1, &session->chunk, error, sizeof(error), ctx)) {
            session->pending_init_offset = repl_compiler_exec_offset(session->compiler);
        } else {
            session->compile_stuck = 1;
            snprintf(session->compile_stuck_error,
                     sizeof(session->compile_stuck_error), "%s",
                     error[0] != '\0' ? error : "unknown error");
        }
    }
}

static void repl_session_free(ReplSession* session) {
    string_buffer_free(&session->procedures);
    string_buffer_free(&session->main_body);
    history_free(session);
    if (session->compiler != NULL) {
        repl_compiler_free(session->compiler);
        session->compiler = NULL;
    }
    if (session->vm != NULL) {
        vm_free(session->vm);
        session->vm = NULL;
    }
    free_chunk(&session->chunk);
    if (session->driver_open) {
        session->driver.close(&session->driver);
        session->driver_open = 0;
    } else {
        catalog_close(&session->ctx);
    }
}

static void trim_trailing_ws(char* s) {
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[len - 1] = '\0';
        len--;
    }
}

static int is_procedure_definition(const char* line) {
    while (*line != '\0' && isspace((unsigned char)*line)) line++;
    return strncmp(line, "proc ", 5) == 0;
}

static int is_package_definition(const char* line) {
    while (*line != '\0' && isspace((unsigned char)*line)) line++;
    if (strncmp(line, "package ", 8) == 0) return 1;
    if (strncmp(line, "create ", 7) == 0) {
        line += 7;
        while (*line != '\0' && isspace((unsigned char)*line)) line++;
        if (strncmp(line, "or ", 3) == 0) {
            line += 3;
            while (*line != '\0' && isspace((unsigned char)*line)) line++;
            if (strncmp(line, "replace ", 8) == 0) {
                line += 8;
                while (*line != '\0' && isspace((unsigned char)*line)) line++;
            } else {
                return 0;
            }
        }
        return strncmp(line, "package ", 8) == 0;
    }
    return 0;
}

static int is_statement(const char* line) {
    size_t len = strlen(line);
    if (len == 0) return 0;
    const char* end = line + len - 1;
    while (end > line && isspace((unsigned char)*end)) end--;
    char last = *end;
    return last == ';' || last == '}';
}

/* Number of lines the procedures buffer contributes before the wrapper main
   in the old whole-program layout: procedures, a blank line, then main. */
static int procedures_line_count(const ReplSession* session) {
    int lines = 0;
    for (const char* p = session->procedures.data; p != NULL && *p != '\0'; p++) {
        if (*p == '\n') lines++;
    }
    return lines;
}

static int string_buffer_append_newlines(StringBuffer* buf, int count) {
    for (int i = 0; i < count; i++) {
        if (!string_buffer_append(buf, "\n")) return 0;
    }
    return 1;
}

/* Build a fragment source whose diagnostics land exactly where the old
   whole-program compose placed them: definitions start at line
   (procedures_lines_before + 1), the wrapper main at line
   (procedures_lines_after + 2). The REPL prepends the procedures prefix as
   blank lines. */
static char* build_fragment_source(const char* body,
                                   int line_prefix) {
    StringBuffer buf;
    string_buffer_init(&buf);
    if (buf.data == NULL) return NULL;
    if (!string_buffer_append_newlines(&buf, line_prefix) ||
        !string_buffer_append(&buf, body)) {
        string_buffer_free(&buf);
        return NULL;
    }
    char* result = buf.data;
    return result;
}

/* Build the statement/expression wrapper: the exact text the old
   compose_source produced (minus the procedures prefix), so positions in
   error messages match the old engine. `complete` is included for statements
   (already appended to main_body) and becomes the return expression for
   expression inputs. */
static char* build_wrapper_source(const ReplSession* session, const char* complete,
                                  int current_is_expr, int line_prefix) {
    StringBuffer buf;
    string_buffer_init(&buf);
    if (buf.data == NULL) return NULL;
    if (!string_buffer_append_newlines(&buf, line_prefix)) goto oom;
    if (!string_buffer_append(&buf, "proc main() -> int { ")) goto oom;
    if (session->main_body.len > 0) {
        if (!string_buffer_append(&buf, session->main_body.data)) goto oom;
        if (!string_buffer_append(&buf, " ")) goto oom;
    }
    if (current_is_expr) {
        if (!string_buffer_append(&buf, "return ")) goto oom;
        if (!string_buffer_append(&buf, complete)) goto oom;
        if (!string_buffer_append(&buf, ";")) goto oom;
    } else {
        if (!string_buffer_append(&buf, "return 0;")) goto oom;
    }
    if (!string_buffer_append(&buf, " }\n")) goto oom;
    return buf.data;
oom:
    string_buffer_free(&buf);
    return NULL;
}

/* Fallback for poisoned sessions (a runtime error, or persisted definitions
   that failed to compile at startup): recompose the ENTIRE accumulated source
   (procedures + wrapper main) and compile+run it in a throwaway chunk, which
   is exactly what the pre-#37 engine did on every input. The recomposed
   compile or run then re-fails, and any later input that introduces an
   earlier error (e.g. a parse error) changes the reported error exactly like
   before. A runtime-poisoned session converts to a compile-poisoned one when
   a later input fails to compile. */
static void run_stuck_input(ReplSession* session, const char* complete,
                            int is_expr) {
    StringBuffer composed;
    string_buffer_init(&composed);
    int oom = composed.data == NULL;
    if (!oom && !string_buffer_append(&composed, session->procedures.data)) oom = 1;
    if (!oom && session->procedures.len > 0 &&
        !string_buffer_append(&composed, "\n")) oom = 1;
    if (!oom && !string_buffer_append(&composed, "proc main() -> int { ")) oom = 1;
    if (!oom && session->main_body.len > 0) {
        if (!string_buffer_append(&composed, session->main_body.data)) oom = 1;
        if (!oom && !string_buffer_append(&composed, " ")) oom = 1;
    }
    if (!oom) {
        if (is_expr) {
            if (!string_buffer_append(&composed, "return ")) oom = 1;
            if (!oom && !string_buffer_append(&composed, complete)) oom = 1;
            if (!oom && !string_buffer_append(&composed, ";")) oom = 1;
        } else {
            if (!string_buffer_append(&composed, "return 0;")) oom = 1;
        }
    }
    if (!oom && !string_buffer_append(&composed, " }\n")) oom = 1;
    if (oom) {
        fprintf(stderr, "Out of memory\n");
        string_buffer_free(&composed);
        return;
    }

    Chunk chunk;
    init_chunk(&chunk);
    char error[256];
    Context* ctx = session->driver_open ? NULL : &session->ctx;
    if (!compile_with_context_and_path(composed.data, &chunk, NULL,
                                       error, sizeof(error), ctx)) {
        fprintf(stderr, "Compile error: %s\n",
                error[0] != '\0' ? error : "unknown error");
        session->compile_stuck = 1;
        snprintf(session->compile_stuck_error,
                 sizeof(session->compile_stuck_error), "%s",
                 error[0] != '\0' ? error : "unknown error");
        string_buffer_free(&composed);
        free_chunk(&chunk);
        return;
    }
    string_buffer_free(&composed);

    InterpretResult result = vm_interpret(session->vm, &chunk);
    /* The old engine's poisoned runs executed main one frame below the
       bootstrap at a residual frame count >= 1, so its per-run mirror reset
       was never followed by a re-capture: .vars stayed empty after the
       second consecutive failure. Mirror that here. */
    vm_repl_locals_clear(session->vm);
    if (result == INTERPRET_OK) {
        Value v = vm_pop(session->vm);
        value_print(v);
        printf("\n");
        value_release(v);
    } else {
        const char* err = vm_get_error(session->vm);
        fprintf(stderr, "Runtime error: %s\n",
                err != NULL ? err : "unknown error");
        session->runtime_stuck = 1;
        snprintf(session->runtime_stuck_error,
                 sizeof(session->runtime_stuck_error), "%s",
                 err != NULL ? err : "unknown error");
    }
    free_chunk(&chunk);
}

/* Execute the current fragment (and any queued persisted-package init before
   it, silently). Prints the popped result like the old run_source unless
   silent. A runtime error poisons the session (runtime_stuck), mirroring the
   old engine, where the failed statement stayed in the accumulated main body
   and made every later run fail. Returns 0 on success. */
static int session_execute(ReplSession* session, int silent) {
    if (session->vm == NULL) {
        fprintf(stderr, "Out of memory\n");
        return 1;
    }
    if (session->pending_init_offset >= 0) {
        InterpretResult init_result =
            vm_interpret_from(session->vm, &session->chunk,
                              session->pending_init_offset);
        session->pending_init_offset = -1;
        if (init_result == INTERPRET_OK) {
            Value v = vm_pop(session->vm);
            value_release(v);
        } else {
            const char* err = vm_get_error(session->vm);
            fprintf(stderr, "Runtime error: %s\n",
                    err != NULL ? err : "unknown error");
            session->runtime_stuck = 1;
            snprintf(session->runtime_stuck_error,
                     sizeof(session->runtime_stuck_error), "%s",
                     err != NULL ? err : "unknown error");
            return 1;
        }
    }

    int offset = repl_compiler_exec_offset(session->compiler);
    InterpretResult result = vm_interpret_from(session->vm, &session->chunk, offset);
    if (result == INTERPRET_OK) {
        if (!silent) {
            Value v = vm_pop(session->vm);
            value_print(v);
            printf("\n");
            value_release(v);
        }
        return 0;
    }
    const char* err = vm_get_error(session->vm);
    fprintf(stderr, "Runtime error: %s\n", err != NULL ? err : "unknown error");
    session->runtime_stuck = 1;
    snprintf(session->runtime_stuck_error,
             sizeof(session->runtime_stuck_error), "%s",
             err != NULL ? err : "unknown error");
    return 1;
}

/* Compile one fragment against the persistent compiler/chunk. On failure the
   compiler rolls back internally; here we only report. */
static int session_compile(ReplSession* session, const char* source, int is_def,
                           char* error, size_t error_size) {
    Context* ctx = session->driver_open ? NULL : &session->ctx;
    return repl_compiler_compile(session->compiler, source, is_def,
                                 &session->chunk, error, error_size, ctx);
}

/* Handle a proc/package definition input: append it to the procedures
   buffer, compile it as a fragment and run its package initializers. A
   definition that fails to compile is dropped again, so the session carries
   on as it was. One that redefines a proc from an earlier input replaces it,
   text included. Returns 0 only when out of memory. */
static int run_input_definition(ReplSession* session, const char* complete,
                                int is_package) {
    size_t mark = session->procedures.len;
    int prefix = procedures_line_count(session); /* before the append */
    if (!string_buffer_append(&session->procedures, complete) ||
        !string_buffer_append(&session->procedures, "\n")) {
        fprintf(stderr, "Out of memory\n");
        return 0;
    }
    DBDriver* driver = session->driver_open ? &session->driver : NULL;
    Context* ctx = session->driver_open ? NULL : &session->ctx;

    if (session->compile_stuck || session->runtime_stuck) {
        if (is_package) packages_save_source(driver, ctx, session->procedures.data, 0);
        run_stuck_input(session, "0", 0);
        return 1;
    }

    char* source = build_fragment_source(complete, prefix);
    if (source == NULL) {
        fprintf(stderr, "Out of memory\n");
        return 0;
    }
    char error[256];
    int ok = session_compile(session, source, 1, error, sizeof(error));
    free(source);
    if (!ok) {
        fprintf(stderr, "Compile error: %s\n", error[0] != '\0' ? error : "unknown error");
        string_buffer_truncate(&session->procedures, mark);
        return 1;
    }
    remove_replaced_definitions(&session->procedures, mark);
    /* The buffer holds everything persisted plus this session's definitions,
       so it replaces the stored source; appending stored every definition
       again on each package input (#85). */
    if (is_package) packages_save_source(driver, ctx, session->procedures.data, 0);
    session_execute(session, 0);
    return 1;
}

/* Handle a statement or expression input. A statement is appended to the
   main body the wrapper recompiles; if it fails to compile it is taken out
   again, so later inputs are unaffected. Expression inputs are never
   appended. Returns 0 only when out of memory. */
static int run_input_statement(ReplSession* session, const char* complete,
                               int is_expr) {
    size_t mark = session->main_body.len;
    if (!is_expr) {
        if (!string_buffer_append(&session->main_body, complete) ||
            !string_buffer_append(&session->main_body, " ")) {
            fprintf(stderr, "Out of memory\n");
            return 0;
        }
    }
    if (session->compile_stuck || session->runtime_stuck) {
        run_stuck_input(session, complete, is_expr);
        return 1;
    }

    /* Old compose: procedures, a blank line (only when any exist), then the
       wrapper main on the following line. */
    int plines = procedures_line_count(session);
    int prefix = plines > 0 ? plines + 1 : 0;
    char* source = build_wrapper_source(session, complete, is_expr, prefix);
    if (source == NULL) {
        fprintf(stderr, "Out of memory\n");
        return 0;
    }
    char error[256];
    int ok = session_compile(session, source, 0, error, sizeof(error));
    free(source);
    if (!ok) {
        fprintf(stderr, "Compile error: %s\n", error[0] != '\0' ? error : "unknown error");
        string_buffer_truncate(&session->main_body, mark);
        return 1;
    }
    session_execute(session, 0);
    return 1;
}

/* Compile+run a standalone source (used by .load) in a throwaway chunk so
   the persistent REPL chunk is never clobbered. Observable behavior matches
   the old run_source. */
static int run_source_standalone(ReplSession* session, const char* source) {
    Chunk chunk;
    init_chunk(&chunk);
    char error[256];
    Context* ctx = session->driver_open ? NULL : &session->ctx;
    if (!compile_with_context_and_path(source, &chunk, NULL,
                                       error, sizeof(error), ctx)) {
        fprintf(stderr, "Compile error: %s\n",
                error[0] != '\0' ? error : "unknown error");
        free_chunk(&chunk);
        return 1;
    }

    InterpretResult result = vm_interpret(session->vm, &chunk);
    int rc = 0;
    if (result == INTERPRET_OK) {
        Value v = vm_pop(session->vm);
        value_print(v);
        printf("\n");
        value_release(v);
    } else {
        const char* err = vm_get_error(session->vm);
        fprintf(stderr, "Runtime error: %s\n", err != NULL ? err : "unknown error");
        rc = 1;
    }
    free_chunk(&chunk);
    return rc;
}

static int cmd_load(ReplSession* session, const char* path) {
    if (path == NULL || *path == '\0') {
        fprintf(stderr, "Usage: .load <file>\n");
        return 1;
    }
    char* source = os_read_file(path);
    if (source == NULL) {
        fprintf(stderr, "Could not read file: %s\n", path);
        return 1;
    }

    /* Append any procedure definitions found in the file so they remain
       available for later REPL input. The crude '}'-delimited scan is
       intentionally unchanged from the old engine (including its breakage on
       nested braces). The file's own main is not one of them: it only runs,
       below. */
    int prefix_before = procedures_line_count(session);
    size_t mark = session->procedures.len;
    StringBuffer new_defs;
    string_buffer_init(&new_defs);
    const char* p = source;
    while (*p != '\0') {
        while (*p != '\0' && isspace((unsigned char)*p)) p++;
        if (*p == '\0') break;
        if (strncmp(p, "proc ", 5) == 0) {
            const char* start = p;
            const char* name = p + 5;
            while (isspace((unsigned char)*name)) name++;
            int is_main = strncmp(name, "main", 4) == 0 && !is_ident_char(name[4]);
            while (*p != '\0' && *p != '}') p++;
            if (*p == '}') p++;
            if (is_main) continue;
            size_t len = (size_t)(p - start);
            char* def = malloc(len + 1);
            if (def != NULL) {
                memcpy(def, start, len);
                def[len] = '\0';
                string_buffer_append(&session->procedures, def);
                string_buffer_append(&session->procedures, "\n");
                if (new_defs.data != NULL) {
                    string_buffer_append(&new_defs, def);
                    string_buffer_append(&new_defs, "\n");
                }
                free(def);
            }
        } else {
            p++;
        }
    }

    int has_main = strstr(source, "proc main") != NULL;

    if (session->compile_stuck || session->runtime_stuck) {
        /* Old engine behavior: no incremental registration, just run the
           file (or validate the accumulated definitions) and leave the
           stuck state untouched. */
        string_buffer_free(&new_defs);
        if (has_main) {
            int rc = run_source_standalone(session, source);
            free(source);
            return rc;
        }
        StringBuffer validation;
        string_buffer_init(&validation);
        if (validation.data != NULL) {
            string_buffer_append(&validation, session->procedures.data);
            string_buffer_append(&validation, "proc main() -> int { return 0; }\n");
            run_source_standalone(session, validation.data);
            string_buffer_free(&validation);
        }
        free(source);
        return 0;
    }

    /* Make the newly loaded definitions callable from later inputs (the old
       engine got them for free by recompiling the procedures buffer). If they
       fail to compile they are dropped again, like a failed definition
       input. */
    int defs_failed = 0;
    if (new_defs.data != NULL && new_defs.len > 0) {
        char* frag = build_fragment_source(new_defs.data, prefix_before);
        if (frag != NULL) {
            char error[256];
            if (session_compile(session, frag, 1, error, sizeof(error))) {
                remove_replaced_definitions(&session->procedures, mark);
            } else {
                fprintf(stderr, "Compile error: %s\n",
                        error[0] != '\0' ? error : "unknown error");
                string_buffer_truncate(&session->procedures, mark);
                defs_failed = 1;
            }
            free(frag);
        }
    }
    string_buffer_free(&new_defs);

    /* Run the loaded source. If the file has no main procedure, validate the
       extracted procedure definitions (the old engine composed procedures
       plus a synthetic empty main and ran that, printing its 0). */
    if (has_main) {
        int rc = run_source_standalone(session, source);
        free(source);
        return rc;
    }
    if (session->compile_stuck || session->runtime_stuck) {
        run_stuck_input(session, "0", 0);
        free(source);
        return 0;
    }
    if (!defs_failed) session_execute(session, 0);
    free(source);
    return 0;
}

static void print_value(Value v) {
    value_print(v);
}

static void print_driver_row(ReplSession* session, void* result, void* row, int col_count) {
    for (int i = 0; i < col_count; i++) {
        if (i > 0) printf(" | ");
        const char* name = session->driver.result_column_name(&session->driver, result, i);
        if (name == NULL) name = "?";
        Value field;
        if (session->driver.row_get_field(&session->driver, row, name, &field)) {
            printf("%s = ", name);
            print_value(field);
            value_release(field);
        } else {
            printf("%s = ?", name);
        }
    }
}

static void cmd_sql(ReplSession* session, const char* query) {
    if (query == NULL || *query == '\0') {
        fprintf(stderr, "Usage: .sql <query>\n");
        return;
    }

    while (*query != '\0' && isspace((unsigned char)*query)) query++;

    if (session->driver_open) {
        if (strncasecmp(query, "SELECT ", 7) == 0) {
            void* result = NULL;
            if (!session->driver.query(&session->driver, query, NULL, 0, &result)) {
                printf("(empty result)\n");
                return;
            }
            int col_count = session->driver.result_column_count(&session->driver, result);
            void* row = NULL;
            int first = 1;
            while (session->driver.result_next(&session->driver, result, &row)) {
                if (!first) printf("\n");
                first = 0;
                print_driver_row(session, result, row, col_count);
            }
            if (first) {
                printf("(empty result)\n");
            } else {
                printf("\n");
            }
            session->driver.result_free(&session->driver, result);
            return;
        }
        if (!session->driver.exec(&session->driver, query, NULL, 0)) {
            fprintf(stderr, "SQL error: could not execute '%s'\n", query);
        }
        return;
    }

    if (strncasecmp(query, "SELECT ", 7) == 0) {
        Result* res = sql_exec(query, &session->ctx);
        if (res == NULL) {
            printf("(empty result)\n");
            return;
        }
        Row* row;
        int first = 1;
        while ((row = result_next(res)) != NULL) {
            if (!first) printf("\n");
            first = 0;
            for (int i = 0; i < row->field_count; i++) {
                if (i > 0) printf(" | ");
                printf("%s = ", row->fields[i].name);
                Cell c = row->fields[i].value;
                switch (c.type) {
                    case VAL_INT: printf("%d", c.as.as_int); break;
                    case VAL_FLOAT: printf("%g", c.as.as_float); break;
                    case VAL_STRING: printf("%s", c.as.as_string ? c.as.as_string : ""); break;
                    default: printf("?"); break;
                }
            }
        }
        if (first) {
            printf("(empty result)\n");
        } else {
            printf("\n");
        }
        result_free(res);
        return;
    }

    if (!sql_exec_ddl(query, &session->ctx)) {
        fprintf(stderr, "SQL error: could not execute '%s'\n", query);
    }
}

static void cmd_tables(ReplSession* session) {
#ifdef USE_SQLITE
    if (session->driver_open) {
        void* result = NULL;
        if (!session->driver.query(&session->driver,
                                   "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name",
                                   NULL, 0, &result)) {
            printf("(no tables)\n");
            return;
        }
        void* row = NULL;
        int first = 1;
        while (session->driver.result_next(&session->driver, result, &row)) {
            Value name;
            if (session->driver.row_get_field(&session->driver, row, "name", &name)) {
                print_value(name);
                printf("\n");
                value_release(name);
                first = 0;
            }
        }
        session->driver.result_free(&session->driver, result);
        if (first) printf("(no tables)\n");
        return;
    }
#endif

    int count = catalog_table_count(&session->ctx);
    if (count == 0) {
        printf("(no tables)\n");
        return;
    }
    for (int i = 0; i < count; i++) {
        const char* name = catalog_table_name(&session->ctx, i);
        printf("%s\n", name != NULL ? name : "?");
    }
}

static const char* type_name(int type) {
    switch (type) {
        case VAL_INT: return "int";
        case VAL_FLOAT: return "float";
        case VAL_STRING: return "string";
        default: return "?";
    }
}

#ifdef USE_SQLITE
static void print_driver_table_schema(ReplSession* session, const char* table_name) {
    char query[512];
    snprintf(query, sizeof(query), "PRAGMA table_info(%s)", table_name);
    void* result = NULL;
    if (!session->driver.query(&session->driver, query, NULL, 0, &result)) {
        return;
    }
    printf("%s(", table_name);
    void* row = NULL;
    int first_col = 1;
    while (session->driver.result_next(&session->driver, result, &row)) {
        Value name;
        Value type;
        int has_name = session->driver.row_get_field(&session->driver, row, "name", &name);
        int has_type = session->driver.row_get_field(&session->driver, row, "type", &type);
        if (has_name && name.type == VAL_STRING) {
            if (!first_col) printf(", ");
            first_col = 0;
            printf("%s", name.as.as_string ? name.as.as_string : "?");
            if (has_type && type.type == VAL_STRING && type.as.as_string != NULL) {
                printf(" %s", type.as.as_string);
            }
        }
        value_release(name);
        value_release(type);
    }
    printf(")\n");
    session->driver.result_free(&session->driver, result);
}
#endif

static int cmd_connect(ReplSession* session, const char* path) {
    if (path == NULL || *path == '\0') {
        fprintf(stderr, "Usage: .connect <path>\n");
        return 1;
    }
    while (*path != '\0' && isspace((unsigned char)*path)) path++;

#ifdef USE_SQLITE
    if (session->driver_open) {
        session->driver.close(&session->driver);
        session->driver_open = 0;
    } else {
        catalog_close(&session->ctx);
        session->ctx.db_path = default_db_path();
        session->ctx.pager = NULL;
    }

    sqlite_driver_init(&session->driver);
    if (!session->driver.open(&session->driver, path)) {
        fprintf(stderr, "Could not open database: %s\n", path);
        return 1;
    }
    session->driver_open = 1;
    if (session->vm != NULL) {
        vm_set_driver(session->vm, &session->driver);
    }
    printf("Connected to %s\n", path);
    return 0;
#else
    (void)session;
    fprintf(stderr, "SQLite support is disabled in this build\n");
    return 1;
#endif
}

static void cmd_columns(ReplSession* session, const char* table_name) {
    if (table_name == NULL || *table_name == '\0') {
        fprintf(stderr, "Usage: .columns <table>\n");
        return;
    }
    while (*table_name != '\0' && isspace((unsigned char)*table_name)) table_name++;

#ifdef USE_SQLITE
    if (session->driver_open) {
        char query[512];
        snprintf(query, sizeof(query), "PRAGMA table_info(%s)", table_name);
        void* result = NULL;
        if (!session->driver.query(&session->driver, query, NULL, 0, &result)) {
            printf("(no such table)\n");
            return;
        }
        printf("%s columns:\n", table_name);
        void* row = NULL;
        int first = 1;
        while (session->driver.result_next(&session->driver, result, &row)) {
            Value name;
            Value type;
            int has_name = session->driver.row_get_field(&session->driver, row, "name", &name);
            int has_type = session->driver.row_get_field(&session->driver, row, "type", &type);
            if (has_name && name.type == VAL_STRING && name.as.as_string != NULL) {
                printf("  %s", name.as.as_string);
                if (has_type && type.type == VAL_STRING && type.as.as_string != NULL) {
                    printf(" %s", type.as.as_string);
                }
                printf("\n");
                first = 0;
            }
            value_release(name);
            value_release(type);
        }
        session->driver.result_free(&session->driver, result);
        if (first) printf("(no columns)\n");
        return;
    }
#endif

    Table* table = catalog_find_table(&session->ctx, table_name);
    if (table == NULL) {
        printf("(no such table)\n");
        return;
    }
    printf("%s columns:\n", table_name);
    for (int i = 0; i < table->column_count; i++) {
        printf("  %s %s\n",
               table->columns[i].name != NULL ? table->columns[i].name : "?",
               type_name(table->columns[i].type));
    }
}

static void cmd_count(ReplSession* session, const char* table_name) {
    if (table_name == NULL || *table_name == '\0') {
        fprintf(stderr, "Usage: .count <table>\n");
        return;
    }
    while (*table_name != '\0' && isspace((unsigned char)*table_name)) table_name++;

#ifdef USE_SQLITE
    if (session->driver_open) {
        char query[512];
        snprintf(query, sizeof(query), "SELECT count(*) FROM %s", table_name);
        void* result = NULL;
        if (!session->driver.query(&session->driver, query, NULL, 0, &result)) {
            printf("(could not count)\n");
            return;
        }
        void* row = NULL;
        int n = 0;
        if (session->driver.result_next(&session->driver, result, &row)) {
            Value v;
            if (session->driver.row_get_column(&session->driver, row, 0, &v)) {
                if (v.type == VAL_INT) n = v.as.as_int;
                value_release(v);
            }
        }
        printf("%s: %d rows\n", table_name, n);
        session->driver.result_free(&session->driver, result);
        return;
    }
#endif

    char query[512];
    snprintf(query, sizeof(query), "SELECT count(*) FROM %s", table_name);
    Result* res = sql_exec(query, &session->ctx);
    int n = 0;
    if (res != NULL) {
        Row* row = result_next(res);
        if (row != NULL && row->field_count > 0) {
            Cell c = row->fields[0].value;
            if (c.type == VAL_INT) n = c.as.as_int;
        }
        result_free(res);
    }
    printf("%s: %d rows\n", table_name, n);
}

static void cmd_indexes(ReplSession* session, const char* table_name) {
    if (table_name == NULL || *table_name == '\0') {
        fprintf(stderr, "Usage: .indexes <table>\n");
        return;
    }
    while (*table_name != '\0' && isspace((unsigned char)*table_name)) table_name++;

#ifdef USE_SQLITE
    if (session->driver_open) {
        char query[512];
        snprintf(query, sizeof(query), "PRAGMA index_list(%s)", table_name);
        void* result = NULL;
        if (!session->driver.query(&session->driver, query, NULL, 0, &result)) {
            printf("(no indexes)\n");
            return;
        }
        void* row = NULL;
        int first = 1;
        while (session->driver.result_next(&session->driver, result, &row)) {
            Value name;
            int has_name = session->driver.row_get_field(&session->driver, row, "name", &name);
            if (has_name && name.type == VAL_STRING && name.as.as_string != NULL) {
                printf("%s\n", name.as.as_string);
                first = 0;
            }
            value_release(name);
        }
        session->driver.result_free(&session->driver, result);
        if (first) printf("(no indexes)\n");
        return;
    }
#endif

    Table* table = catalog_find_table(&session->ctx, table_name);
    if (table == NULL) {
        printf("(no such table)\n");
        return;
    }
    printf("(no indexes)\n");
}

static void cmd_foreignkeys(ReplSession* session, const char* table_name) {
    if (table_name == NULL || *table_name == '\0') {
        fprintf(stderr, "Usage: .foreignkeys <table>\n");
        return;
    }
    while (*table_name != '\0' && isspace((unsigned char)*table_name)) table_name++;

#ifdef USE_SQLITE
    if (session->driver_open) {
        char query[512];
        snprintf(query, sizeof(query), "PRAGMA foreign_key_list(%s)", table_name);
        void* result = NULL;
        if (!session->driver.query(&session->driver, query, NULL, 0, &result)) {
            printf("(no foreign keys)\n");
            return;
        }
        void* row = NULL;
        int first = 1;
        while (session->driver.result_next(&session->driver, result, &row)) {
            Value from;
            Value to_table;
            Value to_col;
            int has_from = session->driver.row_get_field(&session->driver, row, "from", &from);
            int has_table = session->driver.row_get_field(&session->driver, row, "table", &to_table);
            int has_to = session->driver.row_get_field(&session->driver, row, "to", &to_col);
            if (has_from && from.type == VAL_STRING && from.as.as_string != NULL &&
                has_table && to_table.type == VAL_STRING && to_table.as.as_string != NULL &&
                has_to && to_col.type == VAL_STRING && to_col.as.as_string != NULL) {
                printf("%s -> %s(%s)\n",
                       from.as.as_string,
                       to_table.as.as_string,
                       to_col.as.as_string);
                first = 0;
            }
            value_release(from);
            value_release(to_table);
            value_release(to_col);
        }
        session->driver.result_free(&session->driver, result);
        if (first) printf("(no foreign keys)\n");
        return;
    }
#endif

    Table* table = catalog_find_table(&session->ctx, table_name);
    if (table == NULL) {
        printf("(no such table)\n");
        return;
    }
    printf("(no foreign keys)\n");
}

static void cmd_schema(ReplSession* session, const char* table_name) {
#ifdef USE_SQLITE
    if (session->driver_open) {
        if (table_name != NULL) {
            print_driver_table_schema(session, table_name);
            return;
        }
        void* result = NULL;
        if (!session->driver.query(&session->driver,
                                   "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name",
                                   NULL, 0, &result)) {
            printf("(no tables)\n");
            return;
        }
        void* row = NULL;
        int first = 1;
        while (session->driver.result_next(&session->driver, result, &row)) {
            Value name;
            if (session->driver.row_get_field(&session->driver, row, "name", &name)) {
                if (name.type == VAL_STRING && name.as.as_string != NULL) {
                    print_driver_table_schema(session, name.as.as_string);
                    first = 0;
                }
                value_release(name);
            }
        }
        session->driver.result_free(&session->driver, result);
        if (first) printf("(no tables)\n");
        return;
    }
#endif

    int count = catalog_table_count(&session->ctx);
    if (count == 0) {
        printf("(no tables)\n");
        return;
    }

    for (int i = 0; i < count; i++) {
        const char* name = catalog_table_name(&session->ctx, i);
        if (table_name != NULL && strcmp(table_name, name) != 0) {
            continue;
        }
        printf("%s(", name);
        int cols = catalog_table_column_count(&session->ctx, i);
        for (int c = 0; c < cols; c++) {
            const char* col_name = catalog_table_column_name(&session->ctx, i, c);
            int col_type = catalog_table_column_type(&session->ctx, i, c);
            printf("%s %s", col_name != NULL ? col_name : "?", type_name(col_type));
            if (c < cols - 1) printf(", ");
        }
        printf(")\n");
        if (table_name != NULL) break;
    }
}

/* Names come from the compiler's table of top-level locals, whose index is
   the slot the VM mirrors, so they cannot drift from the values. */
static void cmd_vars(ReplSession* session) {
    int count = repl_compiler_local_count(session->compiler);
    int locals = session->vm != NULL ? vm_local_count(session->vm) : 0;
    if (count > locals) count = locals;
    if (count == 0) {
        printf("(no variables)\n");
        return;
    }
    for (int i = 0; i < count; i++) {
        Value v = vm_local_get(session->vm, i);
        const char* name = repl_compiler_local_name(session->compiler, i);
        printf("%s = ", name != NULL ? name : "?");
        value_print(v);
        printf("\n");
    }
}

static void cmd_defs(ReplSession* session) {
    if (session->procedures.len == 0) {
        printf("(no procedures)\n");
        return;
    }
    printf("%s", session->procedures.data);
}

static void cmd_history(ReplSession* session) {
    if (session->history_count == 0) {
        printf("(no history)\n");
        return;
    }
    for (int i = 0; i < session->history_count; i++) {
        printf("%d  %s\n", i + 1, session->history[i]);
    }
}

static void print_repl_help(void) {
    printf("Auspex REPL commands:\n");
    printf("  .exit             Quit the REPL\n");
    printf("  .quit             Same as .exit\n");
    printf("  .help             Show this help message\n");
    printf("  .load <file>      Load and run an Auspex source file\n");
    printf("  .tables           List all tables in the catalog\n");
    printf("  .schema [table]   Show schema for all tables or one table\n");
    printf("  .columns <table>  List columns for a table\n");
    printf("  .count <table>    Count rows in a table\n");
    printf("  .indexes <table>  List indexes for a table\n");
    printf("  .foreignkeys <table>  List foreign keys for a table\n");
    printf("  .sql <query>      Execute a SQL DDL or SELECT query\n");
    printf("  .connect <path>   Connect to a SQLite database\n");
    printf("  .vars             Show current REPL variables\n");
    printf("  .defs             Show defined procedures\n");
    printf("  .history          Show REPL input history\n");
}

void repl_run(const char* db_path) {
    ReplSession session;
    repl_session_init(&session, db_path);

    printf("Auspex REPL (type '.help' for commands, '.exit' to quit)\n");

    char line[LINE_SIZE];
    StringBuffer accumulated;
    string_buffer_init(&accumulated);
    if (accumulated.data == NULL) {
        fprintf(stderr, "Out of memory\n");
        repl_session_free(&session);
        return;
    }

    for (;;) {
        const char* prompt = accumulated.len > 0 ? "... " : "> ";
        if (!repl_read_line(&session, prompt, line, sizeof(line))) {
            printf("\n");
            break;
        }

        trim_trailing_ws(line);
        if (accumulated.len == 0 && line[0] == '\0') {
            continue;
        }

        /* Commands are processed immediately, not accumulated. */
        if (accumulated.len == 0 && line[0] == '.') {
            if (strcmp(line, ".exit") == 0 || strcmp(line, ".quit") == 0) {
                break;
            }
            if (strcmp(line, ".help") == 0) {
                print_repl_help();
                continue;
            }
            if (strcmp(line, ".tables") == 0) {
                cmd_tables(&session);
                continue;
            }
            if (strcmp(line, ".vars") == 0) {
                cmd_vars(&session);
                continue;
            }
            if (strcmp(line, ".defs") == 0) {
                cmd_defs(&session);
                continue;
            }
            if (strcmp(line, ".history") == 0) {
                cmd_history(&session);
                continue;
            }
            if (strncmp(line, ".load ", 6) == 0) {
                cmd_load(&session, line + 6);
                continue;
            }
            if (strncmp(line, ".schema", 7) == 0) {
                const char* arg = line + 7;
                while (*arg != '\0' && isspace((unsigned char)*arg)) arg++;
                if (*arg == '\0') arg = NULL;
                cmd_schema(&session, arg);
                continue;
            }
            if (strncmp(line, ".columns ", 9) == 0) {
                cmd_columns(&session, line + 9);
                continue;
            }
            if (strncmp(line, ".count ", 7) == 0) {
                cmd_count(&session, line + 7);
                continue;
            }
            if (strncmp(line, ".indexes ", 9) == 0) {
                cmd_indexes(&session, line + 9);
                continue;
            }
            if (strncmp(line, ".foreignkeys ", 13) == 0) {
                cmd_foreignkeys(&session, line + 13);
                continue;
            }
            if (strncmp(line, ".sql ", 5) == 0) {
                cmd_sql(&session, line + 5);
                continue;
            }
            if (strncmp(line, ".connect ", 9) == 0) {
                cmd_connect(&session, line + 9);
                continue;
            }
            fprintf(stderr, "Unknown command: %s\n", line);
            continue;
        }

        if (accumulated.len > 0) {
            if (!string_buffer_append(&accumulated, "\n") ||
                !string_buffer_append(&accumulated, line)) {
                fprintf(stderr, "Out of memory\n");
                break;
            }
        } else {
            string_buffer_free(&accumulated);
            string_buffer_init(&accumulated);
            if (accumulated.data == NULL ||
                !string_buffer_append(&accumulated, line)) {
                fprintf(stderr, "Out of memory\n");
                break;
            }
        }

        if (!input_is_complete(accumulated.data)) {
            continue;
        }

        char* complete = accumulated.data;
        history_add(&session, complete);

        int is_package = is_package_definition(complete);
        int ok;
        if (is_procedure_definition(complete) || is_package) {
            ok = run_input_definition(&session, complete, is_package);
        } else {
            ok = run_input_statement(&session, complete, !is_statement(complete));
        }
        if (!ok) break;

        string_buffer_free(&accumulated);
        string_buffer_init(&accumulated);
        if (accumulated.data == NULL) {
            fprintf(stderr, "Out of memory\n");
            break;
        }
    }

    string_buffer_free(&accumulated);
    repl_session_free(&session);
}
