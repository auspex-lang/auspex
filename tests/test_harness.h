#ifndef TEST_HARNESS_H
#define TEST_HARNESS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The suites drive bin/auspex, the shell and the file system through POSIX
   conventions: shell command lines passed to system(), exit statuses read
   with WEXITSTATUS, files under /tmp. On Windows (MinGW-w64 under MSYS2)
   the same tests run unchanged: system() goes to MSYS2's bash rather than
   cmd.exe, a spawned process's status already is its exit code, and CI
   creates the /tmp directory the paths resolve to. */
#ifdef _WIN32
#include <process.h>
#include <stdint.h>

#define WEXITSTATUS(status) (status)
#define WIFEXITED(status) 1

static inline int test_system(const char* command) {
    if (command == NULL) return 1;
    /* _spawn joins its arguments into one command line, which the child
       splits again by the Windows rules: quote the script, and escape the
       quotes and the backslashes in front of them. */
    size_t len = strlen(command);
    char* quoted = malloc(len * 2 + 3);
    if (quoted == NULL) return -1;
    size_t q = 0;
    size_t backslashes = 0;
    quoted[q++] = '"';
    for (size_t i = 0; i < len; i++) {
        char c = command[i];
        if (c == '\\') {
            backslashes++;
        } else {
            if (c == '"') {
                for (size_t b = 0; b < backslashes + 1; b++) quoted[q++] = '\\';
            }
            backslashes = 0;
        }
        quoted[q++] = c;
    }
    for (size_t b = 0; b < backslashes; b++) quoted[q++] = '\\';
    quoted[q++] = '"';
    quoted[q] = '\0';
    fflush(stdout);
    fflush(stderr);
    intptr_t status = _spawnlp(_P_WAIT, "bash", "bash", "-c", quoted, (char*)NULL);
    free(quoted);
    return (int)status;
}
#define system test_system

#define setenv(name, value, overwrite) _putenv_s((name), (value))
#define unsetenv(name) _putenv_s((name), "")
#else
#include <sys/wait.h>
#endif

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;
static const char* current_test_name = NULL;
static int current_test_failed = 0;

#define TEST(name) static void name(void)

#define RUN_TEST(name)                                          \
    do {                                                        \
        current_test_name = #name;                              \
        current_test_failed = 0;                                \
        tests_run++;                                            \
        name();                                                 \
        if (!current_test_failed) {                             \
            tests_passed++;                                     \
            printf("  PASS: %s\n", #name);                      \
        }                                                       \
    } while (0)

#define FAIL(message)                                           \
    do {                                                        \
        current_test_failed = 1;                                \
        tests_failed++;                                         \
        fprintf(stderr,                                         \
                "  FAIL: %s at %s:%d: %s\n",                   \
                current_test_name, __FILE__, __LINE__, message); \
        return;                                                 \
    } while (0)

#define ASSERT(condition)                                       \
    do {                                                        \
        if (!(condition)) {                                     \
            FAIL(#condition);                                   \
        }                                                       \
    } while (0)

#define ASSERT_INT_EQ(expected, actual)                         \
    do {                                                        \
        int _expected = (expected);                             \
        int _actual = (actual);                                 \
        if (_expected != _actual) {                             \
            current_test_failed = 1;                            \
            tests_failed++;                                     \
            fprintf(stderr,                                     \
                    "  FAIL: %s at %s:%d: expected %d, got %d\n", \
                    current_test_name, __FILE__, __LINE__,      \
                    _expected, _actual);                        \
            return;                                             \
        }                                                       \
    } while (0)

#define ASSERT_FLOAT_EQ(expected, actual)                       \
    do {                                                        \
        double _expected = (expected);                          \
        double _actual = (actual);                              \
        if (_expected != _actual) {                             \
            current_test_failed = 1;                            \
            tests_failed++;                                     \
            fprintf(stderr,                                     \
                    "  FAIL: %s at %s:%d: expected %f, got %f\n", \
                    current_test_name, __FILE__, __LINE__,      \
                    _expected, _actual);                        \
            return;                                             \
        }                                                       \
    } while (0)

#define ASSERT_STRING_EQ(expected, actual)                      \
    do {                                                        \
        const char* _expected = (expected);                     \
        const char* _actual = (actual);                         \
        if (strcmp(_expected, _actual) != 0) {                  \
            current_test_failed = 1;                            \
            tests_failed++;                                     \
            fprintf(stderr,                                     \
                    "  FAIL: %s at %s:%d: expected '%s', got '%s'\n", \
                    current_test_name, __FILE__, __LINE__,      \
                    _expected, _actual);                        \
            return;                                             \
        }                                                       \
    } while (0)

#define ASSERT_PTR_NULL(ptr)                                    \
    do {                                                        \
        if ((ptr) != NULL) {                                    \
            FAIL(#ptr " should be NULL");                       \
        }                                                       \
    } while (0)

#define ASSERT_PTR_NOT_NULL(ptr)                                \
    do {                                                        \
        if ((ptr) == NULL) {                                    \
            FAIL(#ptr " should not be NULL");                   \
        }                                                       \
    } while (0)

#define ASSERT_PTR_EQ(expected, actual)                         \
    do {                                                        \
        const void* _expected = (expected);                     \
        const void* _actual = (actual);                         \
        if (_expected != _actual) {                             \
            current_test_failed = 1;                            \
            tests_failed++;                                     \
            fprintf(stderr,                                     \
                    "  FAIL: %s at %s:%d: expected pointer %p, got %p\n", \
                    current_test_name, __FILE__, __LINE__,      \
                    _expected, _actual);                        \
            return;                                             \
        }                                                       \
    } while (0)

#define TEST_SUMMARY()                                          \
    do {                                                        \
        printf("\n%d run, %d passed, %d failed\n",              \
               tests_run, tests_passed, tests_failed);          \
        return tests_failed == 0 ? 0 : 1;                       \
    } while (0)

#endif
