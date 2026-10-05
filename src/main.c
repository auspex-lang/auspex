#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mydb.h"
#include "repl.h"
#include "vm.h"
#include "compiler.h"
#include "os.h"
#include "packages.h"
#include "stored_programs.h"
#include "sql_engine.h"
#ifdef USE_SQLITE
#include "sqlite_driver.h"
#endif

static void print_usage(const char* program) {
    fprintf(stderr, "Usage: %s [options] [file]\n", program);
    fprintf(stderr, "\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  --help     Show this help message\n");
    fprintf(stderr, "  --version  Show version information\n");
    fprintf(stderr, "  -DNAME     Define a conditional-compilation flag\n");
#ifdef USE_SQLITE
    fprintf(stderr, "  --db PATH  Open a SQLite database\n");
#else
    fprintf(stderr, "  --db PATH  (disabled in this build)\n");
#endif
}

static void print_version(void) {
    printf("Auspex 0.2.0\n");
}

static void replace_with_filtered(char** loaded, const char* source) {
    if (*loaded == NULL) return;
    char* filtered = packages_filter_redefined(*loaded, source);
    free(*loaded);
    *loaded = filtered;
}

static int run_file(const char* path, DBDriver* driver, const CompileOptions* options) {
    char* source = os_read_file(path);
    if (source == NULL) {
        fprintf(stderr, "Could not read file: %s\n", path);
        return 1;
    }

    Context* ctx = NULL;
    Context custom_ctx;
    if (driver != NULL && !driver->is_sqlite) {
        custom_ctx.db_path = default_db_path();
        custom_ctx.pager = NULL;
        ctx = &custom_ctx;
    }

    char* builtin_source = packages_load_builtins();
    char* package_source = driver != NULL ? packages_load_source(driver, ctx) : NULL;
    /* A package declared in the file replaces a stored package of the same
       name, and either replaces a built-in one. Regions excluded by $if do
       not declare anything. */
    char cc_error[256];
    char* active_source = cc_preprocess(source, path, options, cc_error, sizeof(cc_error));
    const char* declaring_source = active_source != NULL ? active_source : source;
    replace_with_filtered(&package_source, declaring_source);
    replace_with_filtered(&builtin_source, declaring_source);
    replace_with_filtered(&builtin_source, package_source);
    free(active_source);
    char* program_source = driver != NULL ? stored_programs_load_source(driver, ctx) : NULL;
    if (program_source != NULL && source != NULL) {
        char* filtered = stored_programs_filter_redefined(program_source, source);
        free(program_source);
        program_source = filtered;
    }

    size_t builtin_len = (builtin_source != NULL && *builtin_source != '\0') ? strlen(builtin_source) : 0;
    size_t pkg_len = (package_source != NULL && *package_source != '\0') ? strlen(package_source) : 0;
    size_t prg_len = (program_source != NULL && *program_source != '\0') ? strlen(program_source) : 0;
    size_t src_len = strlen(source);

    char* combined = NULL;
    int preamble_lines = 0;
    if (builtin_len > 0 || pkg_len > 0 || prg_len > 0) {
        combined = malloc(builtin_len + pkg_len + prg_len + src_len + 5);
        if (combined != NULL) {
            size_t off = 0;
            if (builtin_len > 0) {
                memcpy(combined + off, builtin_source, builtin_len);
                off += builtin_len;
                combined[off++] = '\n';
            }
            if (pkg_len > 0) {
                memcpy(combined + off, package_source, pkg_len);
                off += pkg_len;
                combined[off++] = '\n';
            }
            if (prg_len > 0) {
                memcpy(combined + off, program_source, prg_len);
                off += prg_len;
                combined[off++] = '\n';
            }
            /* A blank line closes the preamble, so no declaration in it can
               land on the line just before the user's first one. */
            combined[off++] = '\n';
            for (size_t i = 0; i < off; i++) {
                if (combined[i] == '\n') preamble_lines++;
            }
            memcpy(combined + off, source, src_len + 1);
        }
    }

    free(builtin_source);
    free(package_source);
    free(program_source);
    const char* compile_source = combined != NULL ? combined : source;

    /* Everything ahead of the user's source is built-in or stored
       declarations; tell the compiler so it numbers the user's own lines
       from 1 instead of reporting them shifted by the preamble. */
    CompileOptions compile_options = {0};
    if (options != NULL) compile_options = *options;
    compile_options.line_offset = preamble_lines;

    Chunk chunk;
    init_chunk(&chunk);
    char error[256];
    if (!compile_with_options(compile_source, &chunk, path, error, sizeof(error),
                              ctx, &compile_options)) {
        fprintf(stderr, "Compile error: %s\n", error[0] != '\0' ? error : "unknown error");
        free(source);
        free(combined);
        free_chunk(&chunk);
        return 1;
    }

    if (driver != NULL && strstr(source, "package ") != NULL) {
        packages_save_source(driver, ctx, source, 0);
    }
    if (driver != NULL) {
        stored_programs_save_source(driver, ctx, source);
    }

    free(source);
    free(combined);

    VM* vm = vm_init();
    if (vm == NULL) {
        fprintf(stderr, "Out of memory\n");
        free_chunk(&chunk);
        return 1;
    }

    if (driver != NULL) {
        vm_set_driver(vm, driver);
    }

    InterpretResult result = vm_interpret(vm, &chunk);
    int rc = 0;
    if (result == INTERPRET_OK) {
        Value v = vm_pop(vm);
        value_print(v);
        printf("\n");
        value_release(v);
    } else {
        const char* err = vm_get_error(vm);
        fprintf(stderr, "Runtime error: %s\n", err != NULL ? err : "unknown error");
        rc = 1;
    }

    vm_free(vm);
    free_chunk(&chunk);
    return rc;
}

static int valid_conditional_flag(const char* name) {
    size_t len = name != NULL ? strlen(name) : 0;
    if (len == 0 || len >= AUSPEX_CC_FLAG_NAME_MAX) return 0;
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_')) {
            return 0;
        }
    }
    return 1;
}

int main(int argc, char** argv) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        print_usage(argv[0]);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "--version") == 0) {
        print_version();
        return 0;
    }

    const char* file = NULL;
    const char* db_path = NULL;
    const char* conditional_flags[AUSPEX_CC_MAX_FLAGS];
    int conditional_flag_count = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--db") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing argument for --db\n");
                print_usage(argv[0]);
                return 1;
            }
            db_path = argv[++i];
        } else if (strncmp(argv[i], "-D", 2) == 0) {
            const char* name = argv[i] + 2;
            if (!valid_conditional_flag(name)) {
                fprintf(stderr, "Invalid conditional-compilation flag: %s\n", argv[i]);
                print_usage(argv[0]);
                return 1;
            }
            int duplicate = 0;
            for (int j = 0; j < conditional_flag_count; j++) {
                if (strcmp(conditional_flags[j], name) == 0) {
                    duplicate = 1;
                    break;
                }
            }
            if (!duplicate) {
                if (conditional_flag_count >= AUSPEX_CC_MAX_FLAGS) {
                    fprintf(stderr, "Too many conditional-compilation flags (maximum %d)\n",
                            AUSPEX_CC_MAX_FLAGS);
                    return 1;
                }
                conditional_flags[conditional_flag_count++] = name;
            }
        } else if (file == NULL) {
            file = argv[i];
        } else {
            fprintf(stderr, "Unexpected argument: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (file == NULL && conditional_flag_count > 0) {
        fprintf(stderr, "Conditional-compilation flags require an input file\n");
        print_usage(argv[0]);
        return 1;
    }

    CompileOptions compile_options = {
        conditional_flags,
        conditional_flag_count,
        0 /* line_offset: set per source by run_file */
    };

    DBDriver driver;
    int driver_open = 0;
    if (db_path != NULL) {
#ifdef USE_SQLITE
        sqlite_driver_init(&driver);
        if (!driver.open(&driver, db_path)) {
            fprintf(stderr, "Could not open database: %s\n",
                    driver.error_message[0] != '\0' ? driver.error_message : db_path);
            return 1;
        }
        driver_open = 1;
#else
        fprintf(stderr, "SQLite support is disabled in this build\n");
        return 1;
#endif
    } else if (file != NULL) {  
        custom_driver_init(&driver);
        if (!driver.open(&driver, default_db_path())) {
            fprintf(stderr, "Could not open database: %s\n", default_db_path());
            return 1;
        }
        driver_open = 1;
    }

    int rc = 0;
    if (file != NULL) {
        rc = run_file(file, driver_open ? &driver : NULL, &compile_options);
    } else {
        repl_run(db_path);
    }

    if (driver_open) {
        driver.close(&driver);
    }
    return rc;
}
