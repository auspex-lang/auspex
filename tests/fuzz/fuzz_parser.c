#include "fuzz_common.h"

#include "parser.h"

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    char* source = fuzz_cstring(data, size);
    if (source == NULL) return 0;

    char error[256];
    error[0] = '\0';
    Program* program = parse_with_path(source, "fuzz.apx", error, sizeof(error));
    if (program != NULL) {
        free_program(program);
    } else {
        FUZZ_CHECK(error[0] != '\0', "parse failed without an error message");
    }

    Expr* expr = parse_expression(source);
    free_expr(expr);

    free(source);
    return 0;
}
