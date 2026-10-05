#include "fuzz_common.h"

#include "compiler.h"
#include "parser.h"

/* The first input byte picks which command-line (-D) flags are defined, the
 * rest is source. Output is also parsed, since the parser only ever sees
 * preprocessed source. */
static const char* const fuzz_flag_names[] = {"DEBUG", "A", "B", "TRACE"};

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0) return 0;
    uint8_t mask = data[0];
    char* source = fuzz_cstring(data + 1, size - 1);
    if (source == NULL) return 0;
    size_t len = strlen(source);

    const char* flags[4];
    int flag_count = 0;
    for (int i = 0; i < 4; i++) {
        if (mask & (1u << i)) flags[flag_count++] = fuzz_flag_names[i];
    }
    CompileOptions options = {flags, flag_count, 0};

    char error[256];
    error[0] = '\0';
    char* out = cc_preprocess(source, "fuzz.apx", &options, error, sizeof(error));
    if (out != NULL) {
        /* Excluded regions are blanked in place: same length, newlines kept. */
        FUZZ_CHECK(strlen(out) == len, "preprocessor changed source length");
        for (size_t i = 0; i < len; i++) {
            FUZZ_CHECK((out[i] == '\n') == (source[i] == '\n'),
                       "preprocessor moved a newline");
        }
        char parse_error[256];
        Program* program = parse_with_path(out, "fuzz.apx", parse_error, sizeof(parse_error));
        if (program != NULL) free_program(program);
        free(out);
    } else {
        FUZZ_CHECK(error[0] != '\0', "preprocessor failed without an error message");
    }

    free(source);
    return 0;
}
