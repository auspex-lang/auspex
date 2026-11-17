#include "test_harness.h"
#include <string.h>
#include "vm.h"
#include "natives.h"

TEST(vm_init_and_free) {
    VM* vm = vm_init();
    ASSERT_PTR_NOT_NULL(vm);
    vm_free(vm);
}

TEST(vm_executes_constant_and_return) {
    Chunk chunk;
    init_chunk(&chunk);
    int idx = add_constant(&chunk, value_int(42));
    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)idx);
    write_chunk(&chunk, OP_RETURN);

    VM* vm = vm_init();
    InterpretResult result = vm_interpret(vm, &chunk);
    ASSERT_INT_EQ(INTERPRET_OK, result);
    Value value = vm_pop(vm);
    ASSERT_INT_EQ(42, value.as.as_int);
    vm_free(vm);
    free_chunk(&chunk);
}

TEST(vm_executes_local_variables) {
    Chunk chunk;
    init_chunk(&chunk);

    int cst = add_constant(&chunk, value_int(99));
    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)cst);
    write_chunk(&chunk, OP_SET_LOCAL);
    write_chunk(&chunk, 0);
    write_chunk(&chunk, OP_GET_LOCAL);
    write_chunk(&chunk, 0);
    write_chunk(&chunk, OP_RETURN);

    VM* vm = vm_init();
    InterpretResult result = vm_interpret(vm, &chunk);
    ASSERT_INT_EQ(INTERPRET_OK, result);
    ASSERT_INT_EQ(99, vm_pop(vm).as.as_int);
    vm_free(vm);
    free_chunk(&chunk);
}

TEST(vm_executes_arithmetic) {
    Chunk chunk;
    init_chunk(&chunk);

    int a = add_constant(&chunk, value_int(10));
    int b = add_constant(&chunk, value_int(3));

    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)a);
    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)b);
    write_chunk(&chunk, OP_ADD);
    write_chunk(&chunk, OP_RETURN);

    VM* vm = vm_init();
    InterpretResult result = vm_interpret(vm, &chunk);
    ASSERT_INT_EQ(INTERPRET_OK, result);
    ASSERT_INT_EQ(13, vm_pop(vm).as.as_int);
    vm_free(vm);
    free_chunk(&chunk);
}

TEST(vm_executes_subtraction) {
    Chunk chunk;
    init_chunk(&chunk);

    int a = add_constant(&chunk, value_int(10));
    int b = add_constant(&chunk, value_int(3));

    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)a);
    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)b);
    write_chunk(&chunk, OP_SUB);
    write_chunk(&chunk, OP_RETURN);

    VM* vm = vm_init();
    InterpretResult result = vm_interpret(vm, &chunk);
    ASSERT_INT_EQ(INTERPRET_OK, result);
    ASSERT_INT_EQ(7, vm_pop(vm).as.as_int);
    vm_free(vm);
    free_chunk(&chunk);
}

TEST(vm_executes_multiplication) {
    Chunk chunk;
    init_chunk(&chunk);

    int a = add_constant(&chunk, value_int(6));
    int b = add_constant(&chunk, value_int(7));

    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)a);
    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)b);
    write_chunk(&chunk, OP_MUL);
    write_chunk(&chunk, OP_RETURN);

    VM* vm = vm_init();
    InterpretResult result = vm_interpret(vm, &chunk);
    ASSERT_INT_EQ(INTERPRET_OK, result);
    ASSERT_INT_EQ(42, vm_pop(vm).as.as_int);
    vm_free(vm);
    free_chunk(&chunk);
}

TEST(vm_executes_division) {
    Chunk chunk;
    init_chunk(&chunk);

    int a = add_constant(&chunk, value_int(15));
    int b = add_constant(&chunk, value_int(3));

    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)a);
    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)b);
    write_chunk(&chunk, OP_DIV);
    write_chunk(&chunk, OP_RETURN);

    VM* vm = vm_init();
    InterpretResult result = vm_interpret(vm, &chunk);
    ASSERT_INT_EQ(INTERPRET_OK, result);
    ASSERT_INT_EQ(5, vm_pop(vm).as.as_int);
    vm_free(vm);
    free_chunk(&chunk);
}

TEST(vm_executes_comparisons) {
    Chunk chunk;
    init_chunk(&chunk);

    int a = add_constant(&chunk, value_int(5));
    int b = add_constant(&chunk, value_int(5));

    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)a);
    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)b);
    write_chunk(&chunk, OP_EQ);
    write_chunk(&chunk, OP_RETURN);

    VM* vm = vm_init();
    InterpretResult result = vm_interpret(vm, &chunk);
    ASSERT_INT_EQ(INTERPRET_OK, result);
    ASSERT_INT_EQ(1, vm_pop(vm).as.as_int);
    vm_free(vm);
    free_chunk(&chunk);
}

TEST(vm_executes_less_than) {
    Chunk chunk;
    init_chunk(&chunk);

    int a = add_constant(&chunk, value_int(3));
    int b = add_constant(&chunk, value_int(5));

    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)a);
    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)b);
    write_chunk(&chunk, OP_LT);
    write_chunk(&chunk, OP_RETURN);

    VM* vm = vm_init();
    InterpretResult result = vm_interpret(vm, &chunk);
    ASSERT_INT_EQ(INTERPRET_OK, result);
    ASSERT_INT_EQ(1, vm_pop(vm).as.as_int);
    vm_free(vm);
    free_chunk(&chunk);
}

TEST(vm_executes_greater_than) {
    Chunk chunk;
    init_chunk(&chunk);

    int a = add_constant(&chunk, value_int(7));
    int b = add_constant(&chunk, value_int(2));

    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)a);
    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)b);
    write_chunk(&chunk, OP_GT);
    write_chunk(&chunk, OP_RETURN);

    VM* vm = vm_init();
    InterpretResult result = vm_interpret(vm, &chunk);
    ASSERT_INT_EQ(INTERPRET_OK, result);
    ASSERT_INT_EQ(1, vm_pop(vm).as.as_int);
    vm_free(vm);
    free_chunk(&chunk);
}

TEST(vm_executes_false_comparison) {
    Chunk chunk;
    init_chunk(&chunk);

    int a = add_constant(&chunk, value_int(5));
    int b = add_constant(&chunk, value_int(3));

    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)a);
    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)b);
    write_chunk(&chunk, OP_LT);
    write_chunk(&chunk, OP_RETURN);

    VM* vm = vm_init();
    InterpretResult result = vm_interpret(vm, &chunk);
    ASSERT_INT_EQ(INTERPRET_OK, result);
    ASSERT_INT_EQ(0, vm_pop(vm).as.as_int);
    vm_free(vm);
    free_chunk(&chunk);
}

TEST(vm_executes_conditional_jump) {
    Chunk chunk;
    init_chunk(&chunk);

    int truthy = add_constant(&chunk, value_int(1));
    int forty_two = add_constant(&chunk, value_int(42));
    int thirteen = add_constant(&chunk, value_int(13));

    /* if true: push 42, else: push 13 */
    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)truthy);
    write_chunk(&chunk, OP_JZ);
    write_chunk_u16(&chunk, 0); /* offset patched below */
    int jump_else = chunk.count - 2;

    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)forty_two);
    write_chunk(&chunk, OP_JMP);
    write_chunk_u16(&chunk, 0); /* offset patched below */
    int jump_end = chunk.count - 2;

    int else_offset = chunk.count;
    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)thirteen);
    int end_offset = chunk.count;

    /* Jump offset is measured from the instruction after the 16-bit operand. */
    chunk.code[jump_else] = (uint8_t)((else_offset - (jump_else + 2)) >> 8);
    chunk.code[jump_else + 1] = (uint8_t)((else_offset - (jump_else + 2)) & 0xFF);
    chunk.code[jump_end] = (uint8_t)((end_offset - (jump_end + 2)) >> 8);
    chunk.code[jump_end + 1] = (uint8_t)((end_offset - (jump_end + 2)) & 0xFF);

    write_chunk(&chunk, OP_RETURN);

    VM* vm = vm_init();
    InterpretResult result = vm_interpret(vm, &chunk);
    ASSERT_INT_EQ(INTERPRET_OK, result);
    ASSERT_INT_EQ(42, vm_pop(vm).as.as_int);
    vm_free(vm);
    free_chunk(&chunk);
}

TEST(vm_executes_jump_when_false) {
    Chunk chunk;
    init_chunk(&chunk);

    int falsy = add_constant(&chunk, value_int(0));
    int forty_two = add_constant(&chunk, value_int(42));
    int thirteen = add_constant(&chunk, value_int(13));

    /* if false: push 13, else: push 42 */
    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)falsy);
    write_chunk(&chunk, OP_JZ);
    write_chunk_u16(&chunk, 0); /* offset patched below */
    int jump_else = chunk.count - 2;

    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)forty_two);
    write_chunk(&chunk, OP_JMP);
    write_chunk_u16(&chunk, 0); /* offset patched below */
    int jump_end = chunk.count - 2;

    int else_offset = chunk.count;
    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)thirteen);
    int end_offset = chunk.count;

    /* Jump offset is measured from the instruction after the 16-bit operand. */
    chunk.code[jump_else] = (uint8_t)((else_offset - (jump_else + 2)) >> 8);
    chunk.code[jump_else + 1] = (uint8_t)((else_offset - (jump_else + 2)) & 0xFF);
    chunk.code[jump_end] = (uint8_t)((end_offset - (jump_end + 2)) >> 8);
    chunk.code[jump_end + 1] = (uint8_t)((end_offset - (jump_end + 2)) & 0xFF);

    write_chunk(&chunk, OP_RETURN);

    VM* vm = vm_init();
    InterpretResult result = vm_interpret(vm, &chunk);
    ASSERT_INT_EQ(INTERPRET_OK, result);
    ASSERT_INT_EQ(13, vm_pop(vm).as.as_int);
    vm_free(vm);
    free_chunk(&chunk);
}

TEST(vm_stack_overflow_returns_runtime_error) {
    Chunk chunk;
    init_chunk(&chunk);

    int idx = add_constant(&chunk, value_int(1));
    for (int i = 0; i < STACK_MAX + 1; i++) {
        write_chunk(&chunk, OP_CONST);
        write_chunk_u16(&chunk, (uint16_t)idx);
    }
    write_chunk(&chunk, OP_RETURN);

    VM* vm = vm_init();
    InterpretResult result = vm_interpret(vm, &chunk);
    ASSERT_INT_EQ(INTERPRET_RUNTIME_ERROR, result);
    ASSERT_PTR_NOT_NULL(vm_get_error(vm));
    vm_free(vm);
    free_chunk(&chunk);
}

TEST(vm_stack_underflow_returns_runtime_error) {
    Chunk chunk;
    init_chunk(&chunk);

    write_chunk(&chunk, OP_ADD);
    write_chunk(&chunk, OP_RETURN);

    VM* vm = vm_init();
    InterpretResult result = vm_interpret(vm, &chunk);
    ASSERT_INT_EQ(INTERPRET_RUNTIME_ERROR, result);
    vm_free(vm);
    free_chunk(&chunk);
}

TEST(vm_missing_return_returns_runtime_error) {
    Chunk chunk;
    init_chunk(&chunk);

    VM* vm = vm_init();
    InterpretResult result = vm_interpret(vm, &chunk);
    ASSERT_INT_EQ(INTERPRET_RUNTIME_ERROR, result);
    vm_free(vm);
    free_chunk(&chunk);
}

TEST(vm_invalid_constant_index_returns_runtime_error) {
    Chunk chunk;
    init_chunk(&chunk);

    int idx = add_constant(&chunk, value_int(1));
    ASSERT_INT_EQ(0, idx);

    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, 1); /* invalid: constants_count == 1 */
    write_chunk(&chunk, OP_RETURN);

    VM* vm = vm_init();
    InterpretResult result = vm_interpret(vm, &chunk);
    ASSERT_INT_EQ(INTERPRET_RUNTIME_ERROR, result);
    vm_free(vm);
    free_chunk(&chunk);
}

TEST(vm_invalid_get_local_slot_returns_runtime_error) {
    Chunk chunk;
    init_chunk(&chunk);

    write_chunk(&chunk, OP_GET_LOCAL);
    write_chunk(&chunk, 0);
    write_chunk(&chunk, OP_RETURN);

    VM* vm = vm_init();
    InterpretResult result = vm_interpret(vm, &chunk);
    ASSERT_INT_EQ(INTERPRET_RUNTIME_ERROR, result);
    vm_free(vm);
    free_chunk(&chunk);
}

TEST(vm_invalid_set_local_slot_returns_runtime_error) {
    Chunk chunk;
    init_chunk(&chunk);

    int idx = add_constant(&chunk, value_int(1));
    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)idx);
    write_chunk(&chunk, OP_SET_LOCAL);
    write_chunk(&chunk, 1); /* invalid: only one value on stack */
    write_chunk(&chunk, OP_RETURN);

    VM* vm = vm_init();
    InterpretResult result = vm_interpret(vm, &chunk);
    ASSERT_INT_EQ(INTERPRET_RUNTIME_ERROR, result);
    vm_free(vm);
    free_chunk(&chunk);
}

TEST(vm_invalid_jump_target_returns_runtime_error) {
    Chunk chunk;
    init_chunk(&chunk);

    write_chunk(&chunk, OP_JMP);
    write_chunk_u16(&chunk, 0xFFFF);

    VM* vm = vm_init();
    InterpretResult result = vm_interpret(vm, &chunk);
    ASSERT_INT_EQ(INTERPRET_RUNTIME_ERROR, result);
    vm_free(vm);
    free_chunk(&chunk);
}

TEST(vm_invalid_jz_target_returns_runtime_error) {
    Chunk chunk;
    init_chunk(&chunk);

    int idx = add_constant(&chunk, value_int(0));
    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)idx);
    write_chunk(&chunk, OP_JZ);
    write_chunk_u16(&chunk, 0xFFFF);
    write_chunk(&chunk, OP_RETURN);

    VM* vm = vm_init();
    InterpretResult result = vm_interpret(vm, &chunk);
    ASSERT_INT_EQ(INTERPRET_RUNTIME_ERROR, result);
    vm_free(vm);
    free_chunk(&chunk);
}

TEST(vm_division_by_zero_returns_zero) {
    Chunk chunk;
    init_chunk(&chunk);

    int a = add_constant(&chunk, value_int(1));
    int b = add_constant(&chunk, value_int(0));

    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)a);
    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)b);
    write_chunk(&chunk, OP_DIV);
    write_chunk(&chunk, OP_RETURN);

    VM* vm = vm_init();
    InterpretResult result = vm_interpret(vm, &chunk);
    ASSERT_INT_EQ(INTERPRET_OK, result);
    ASSERT_INT_EQ(0, vm_pop(vm).as.as_int);
    vm_free(vm);
    free_chunk(&chunk);
}

TEST(vm_add_float_and_int) {
    Chunk chunk;
    init_chunk(&chunk);

    int a = add_constant(&chunk, value_float(1.5));
    int b = add_constant(&chunk, value_int(2));

    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)a);
    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)b);
    write_chunk(&chunk, OP_ADD);
    write_chunk(&chunk, OP_RETURN);

    VM* vm = vm_init();
    InterpretResult result = vm_interpret(vm, &chunk);
    ASSERT_INT_EQ(INTERPRET_OK, result);
    ASSERT_FLOAT_EQ(3.5, vm_pop(vm).as.as_float);
    vm_free(vm);
    free_chunk(&chunk);
}

TEST(vm_sub_string_and_int_returns_zero) {
    Chunk chunk;
    init_chunk(&chunk);

    int a = add_constant(&chunk, value_string(strdup("x")));
    int b = add_constant(&chunk, value_int(2));

    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)a);
    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)b);
    write_chunk(&chunk, OP_SUB);
    write_chunk(&chunk, OP_RETURN);

    VM* vm = vm_init();
    InterpretResult result = vm_interpret(vm, &chunk);
    ASSERT_INT_EQ(INTERPRET_OK, result);
    ASSERT_INT_EQ(0, vm_pop(vm).as.as_int);
    vm_free(vm);
    free_chunk(&chunk);
}

TEST(vm_mul_float_and_string_returns_zero) {
    Chunk chunk;
    init_chunk(&chunk);

    int a = add_constant(&chunk, value_float(2.0));
    int b = add_constant(&chunk, value_string(strdup("x")));

    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)a);
    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)b);
    write_chunk(&chunk, OP_MUL);
    write_chunk(&chunk, OP_RETURN);

    VM* vm = vm_init();
    InterpretResult result = vm_interpret(vm, &chunk);
    ASSERT_INT_EQ(INTERPRET_OK, result);
    ASSERT_INT_EQ(0, vm_pop(vm).as.as_int);
    vm_free(vm);
    free_chunk(&chunk);
}

TEST(vm_div_string_and_int_returns_zero) {
    Chunk chunk;
    init_chunk(&chunk);

    int a = add_constant(&chunk, value_string(strdup("x")));
    int b = add_constant(&chunk, value_int(2));

    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)a);
    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)b);
    write_chunk(&chunk, OP_DIV);
    write_chunk(&chunk, OP_RETURN);

    VM* vm = vm_init();
    InterpretResult result = vm_interpret(vm, &chunk);
    ASSERT_INT_EQ(INTERPRET_OK, result);
    ASSERT_INT_EQ(0, vm_pop(vm).as.as_int);
    vm_free(vm);
    free_chunk(&chunk);
}

TEST(vm_eq_float_and_int_coerces) {
    Chunk chunk;
    init_chunk(&chunk);

    int a = add_constant(&chunk, value_float(1.0));
    int b = add_constant(&chunk, value_int(1));

    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)a);
    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)b);
    write_chunk(&chunk, OP_EQ);
    write_chunk(&chunk, OP_RETURN);

    VM* vm = vm_init();
    InterpretResult result = vm_interpret(vm, &chunk);
    ASSERT_INT_EQ(INTERPRET_OK, result);
    ASSERT_INT_EQ(1, vm_pop(vm).as.as_int);
    vm_free(vm);
    free_chunk(&chunk);
}

TEST(vm_lt_float_and_int_coerces) {
    Chunk chunk;
    init_chunk(&chunk);

    int a = add_constant(&chunk, value_float(1.0));
    int b = add_constant(&chunk, value_int(2));

    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)a);
    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)b);
    write_chunk(&chunk, OP_LT);
    write_chunk(&chunk, OP_RETURN);

    VM* vm = vm_init();
    InterpretResult result = vm_interpret(vm, &chunk);
    ASSERT_INT_EQ(INTERPRET_OK, result);
    ASSERT_INT_EQ(1, vm_pop(vm).as.as_int);
    vm_free(vm);
    free_chunk(&chunk);
}

TEST(vm_gt_string_and_int_returns_zero) {
    Chunk chunk;
    init_chunk(&chunk);

    int a = add_constant(&chunk, value_string(strdup("x")));
    int b = add_constant(&chunk, value_int(2));

    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)a);
    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)b);
    write_chunk(&chunk, OP_GT);
    write_chunk(&chunk, OP_RETURN);

    VM* vm = vm_init();
    InterpretResult result = vm_interpret(vm, &chunk);
    ASSERT_INT_EQ(INTERPRET_OK, result);
    ASSERT_INT_EQ(0, vm_pop(vm).as.as_int);
    vm_free(vm);
    free_chunk(&chunk);
}

TEST(vm_executes_procedure_call) {
    Chunk chunk;
    init_chunk(&chunk);

    int target = chunk.count + 5; /* after call + arg_count + return */
    write_chunk(&chunk, OP_CALL);
    write_chunk_u16(&chunk, (uint16_t)target);
    write_chunk(&chunk, 0); /* arg count */
    write_chunk(&chunk, OP_RETURN);

    /* procedure body */
    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)add_constant(&chunk, value_int(7)));
    write_chunk(&chunk, OP_RETURN);

    VM* vm = vm_init();
    ASSERT_INT_EQ(INTERPRET_OK, vm_interpret(vm, &chunk));
    ASSERT_INT_EQ(7, vm_pop(vm).as.as_int);
    vm_free(vm);
    free_chunk(&chunk);
}

TEST(vm_executes_procedure_call_with_arguments) {
    Chunk chunk;
    init_chunk(&chunk);

    int arg1 = add_constant(&chunk, value_int(3));
    int arg2 = add_constant(&chunk, value_int(4));

    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)arg1);
    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)arg2);
    write_chunk(&chunk, OP_CALL);
    int target_offset = chunk.count;
    write_chunk_u16(&chunk, 0); /* patched below */
    write_chunk(&chunk, 2); /* arg count */
    write_chunk(&chunk, OP_RETURN);

    /* patch target */
    int target = chunk.count;
    chunk.code[target_offset] = (uint8_t)((target >> 8) & 0xFF);
    chunk.code[target_offset + 1] = (uint8_t)(target & 0xFF);

    /* procedure body: add a + b */
    write_chunk(&chunk, OP_GET_LOCAL);
    write_chunk(&chunk, 0);
    write_chunk(&chunk, OP_GET_LOCAL);
    write_chunk(&chunk, 1);
    write_chunk(&chunk, OP_ADD);
    write_chunk(&chunk, OP_RETURN);

    VM* vm = vm_init();
    ASSERT_INT_EQ(INTERPRET_OK, vm_interpret(vm, &chunk));
    ASSERT_INT_EQ(7, vm_pop(vm).as.as_int);
    vm_free(vm);
    free_chunk(&chunk);
}

TEST(vm_executes_unary_minus) {
    Chunk chunk;
    init_chunk(&chunk);

    int idx = add_constant(&chunk, value_int(7));
    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)idx);
    write_chunk(&chunk, OP_NEGATE);
    write_chunk(&chunk, OP_RETURN);

    VM* vm = vm_init();
    ASSERT_INT_EQ(INTERPRET_OK, vm_interpret(vm, &chunk));
    ASSERT_INT_EQ(-7, vm_pop(vm).as.as_int);
    vm_free(vm);
    free_chunk(&chunk);
}

TEST(vm_executes_unary_not) {
    Chunk chunk;
    init_chunk(&chunk);

    int idx = add_constant(&chunk, value_int(0));
    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)idx);
    write_chunk(&chunk, OP_NOT);
    write_chunk(&chunk, OP_RETURN);

    VM* vm = vm_init();
    ASSERT_INT_EQ(INTERPRET_OK, vm_interpret(vm, &chunk));
    ASSERT_INT_EQ(1, vm_pop(vm).as.as_int);
    vm_free(vm);
    free_chunk(&chunk);
}

TEST(vm_reports_source_line_on_runtime_error) {
    Chunk chunk;
    init_chunk(&chunk);

    int idx = add_constant(&chunk, value_string(strdup("x")));
    int line = 7;
    int col = 3;
    write_chunk_line(&chunk, OP_CONST, line, col);
    write_chunk_u16_line(&chunk, (uint16_t)idx, line, col);
    write_chunk_line(&chunk, OP_NEGATE, line, col);
    write_chunk_line(&chunk, OP_RETURN, line, col);

    VM* vm = vm_init();
    ASSERT_INT_EQ(INTERPRET_RUNTIME_ERROR, vm_interpret(vm, &chunk));
    const char* msg = vm_get_error(vm);
    ASSERT_PTR_NOT_NULL(msg);
    ASSERT_INT_EQ(1, strstr(msg, "7:3: error:") != NULL);
    vm_free(vm);
    free_chunk(&chunk);
}

TEST(vm_executes_pop) {
    Chunk chunk;
    init_chunk(&chunk);

    int a = add_constant(&chunk, value_int(1));
    int b = add_constant(&chunk, value_int(2));
    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)a);
    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)b);
    write_chunk(&chunk, OP_POP);
    write_chunk(&chunk, OP_RETURN);

    VM* vm = vm_init();
    ASSERT_INT_EQ(INTERPRET_OK, vm_interpret(vm, &chunk));
    ASSERT_INT_EQ(1, vm_pop(vm).as.as_int);
    vm_free(vm);
    free_chunk(&chunk);
}

TEST(vm_executes_bool_not_true) {
    Chunk chunk;
    init_chunk(&chunk);
    int idx = add_constant(&chunk, value_bool(1));
    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)idx);
    write_chunk(&chunk, OP_NOT);
    write_chunk(&chunk, OP_RETURN);

    VM* vm = vm_init();
    InterpretResult result = vm_interpret(vm, &chunk);
    ASSERT_INT_EQ(INTERPRET_OK, result);
    ASSERT_INT_EQ(0, vm_pop(vm).as.as_int);
    vm_free(vm);
    free_chunk(&chunk);
}

TEST(vm_executes_array_build_and_index) {
    Chunk chunk;
    init_chunk(&chunk);

    int a = add_constant(&chunk, value_int(10));
    int b = add_constant(&chunk, value_int(20));
    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)a);
    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)b);
    write_chunk(&chunk, OP_ARRAY_BUILD);
    write_chunk_u16(&chunk, 2);

    int idx = add_constant(&chunk, value_int(1));
    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)idx);
    write_chunk(&chunk, OP_INDEX_GET);
    write_chunk(&chunk, OP_RETURN);

    VM* vm = vm_init();
    InterpretResult result = vm_interpret(vm, &chunk);
    ASSERT_INT_EQ(INTERPRET_OK, result);
    ASSERT_INT_EQ(20, vm_pop(vm).as.as_int);
    vm_free(vm);
    free_chunk(&chunk);
}

TEST(vm_executes_array_index_set) {
    Chunk chunk;
    init_chunk(&chunk);

    int a = add_constant(&chunk, value_int(1));
    int b = add_constant(&chunk, value_int(2));
    int c = add_constant(&chunk, value_int(99));
    int idx0 = add_constant(&chunk, value_int(0));

    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)a);
    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)b);
    write_chunk(&chunk, OP_ARRAY_BUILD);
    write_chunk_u16(&chunk, 2);
    write_chunk(&chunk, OP_SET_LOCAL);
    write_chunk(&chunk, 0);

    write_chunk(&chunk, OP_GET_LOCAL);
    write_chunk(&chunk, 0);
    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)idx0);
    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)c);
    write_chunk(&chunk, OP_INDEX_SET);

    write_chunk(&chunk, OP_GET_LOCAL);
    write_chunk(&chunk, 0);
    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)idx0);
    write_chunk(&chunk, OP_INDEX_GET);
    write_chunk(&chunk, OP_RETURN);

    VM* vm = vm_init();
    InterpretResult result = vm_interpret(vm, &chunk);
    ASSERT_INT_EQ(INTERPRET_OK, result);
    ASSERT_INT_EQ(99, vm_pop(vm).as.as_int);
    vm_free(vm);
    free_chunk(&chunk);
}

TEST(vm_executes_array_length) {
    Chunk chunk;
    init_chunk(&chunk);

    int a = add_constant(&chunk, value_int(7));
    int b = add_constant(&chunk, value_int(8));
    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)a);
    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)b);
    write_chunk(&chunk, OP_ARRAY_BUILD);
    write_chunk_u16(&chunk, 2);
    write_chunk(&chunk, OP_NATIVE_CALL);
    write_chunk_u16(&chunk, (uint16_t)native_find("length"));
    write_chunk(&chunk, 1);
    write_chunk(&chunk, OP_RETURN);

    VM* vm = vm_init();
    InterpretResult result = vm_interpret(vm, &chunk);
    ASSERT_INT_EQ(INTERPRET_OK, result);
    ASSERT_INT_EQ(2, vm_pop(vm).as.as_int);
    vm_free(vm);
    free_chunk(&chunk);
}

TEST(vm_executes_array_append) {
    Chunk chunk;
    init_chunk(&chunk);

    int a = add_constant(&chunk, value_int(1));
    int b = add_constant(&chunk, value_int(2));
    int c = add_constant(&chunk, value_int(3));

    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)a);
    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)b);
    write_chunk(&chunk, OP_ARRAY_BUILD);
    write_chunk_u16(&chunk, 2);

    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)c);
    write_chunk(&chunk, OP_NATIVE_CALL);
    write_chunk_u16(&chunk, (uint16_t)native_find("append"));
    write_chunk(&chunk, 2);
    write_chunk(&chunk, OP_NATIVE_CALL);
    write_chunk_u16(&chunk, (uint16_t)native_find("length"));
    write_chunk(&chunk, 1);
    write_chunk(&chunk, OP_RETURN);

    VM* vm = vm_init();
    InterpretResult result = vm_interpret(vm, &chunk);
    ASSERT_INT_EQ(INTERPRET_OK, result);
    ASSERT_INT_EQ(3, vm_pop(vm).as.as_int);
    vm_free(vm);
    free_chunk(&chunk);
}

TEST(vm_array_index_out_of_bounds_get_returns_error) {
    Chunk chunk;
    init_chunk(&chunk);

    int a = add_constant(&chunk, value_int(1));
    int idx = add_constant(&chunk, value_int(5));
    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)a);
    write_chunk(&chunk, OP_ARRAY_BUILD);
    write_chunk_u16(&chunk, 1);
    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)idx);
    write_chunk(&chunk, OP_INDEX_GET);
    write_chunk(&chunk, OP_RETURN);

    VM* vm = vm_init();
    ASSERT_INT_EQ(INTERPRET_RUNTIME_ERROR, vm_interpret(vm, &chunk));
    vm_free(vm);
    free_chunk(&chunk);
}

TEST(vm_array_index_out_of_bounds_set_returns_error) {
    Chunk chunk;
    init_chunk(&chunk);

    int a = add_constant(&chunk, value_int(1));
    int idx = add_constant(&chunk, value_int(5));
    int val = add_constant(&chunk, value_int(99));
    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)a);
    write_chunk(&chunk, OP_ARRAY_BUILD);
    write_chunk_u16(&chunk, 1);
    write_chunk(&chunk, OP_SET_LOCAL);
    write_chunk(&chunk, 0);
    write_chunk(&chunk, OP_GET_LOCAL);
    write_chunk(&chunk, 0);
    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)idx);
    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)val);
    write_chunk(&chunk, OP_INDEX_SET);
    write_chunk(&chunk, OP_RETURN);

    VM* vm = vm_init();
    ASSERT_INT_EQ(INTERPRET_RUNTIME_ERROR, vm_interpret(vm, &chunk));
    vm_free(vm);
    free_chunk(&chunk);
}

TEST(vm_index_get_on_non_array_returns_error) {
    Chunk chunk;
    init_chunk(&chunk);

    int a = add_constant(&chunk, value_int(5));
    int idx = add_constant(&chunk, value_int(0));
    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)a);
    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)idx);
    write_chunk(&chunk, OP_INDEX_GET);
    write_chunk(&chunk, OP_RETURN);

    VM* vm = vm_init();
    ASSERT_INT_EQ(INTERPRET_RUNTIME_ERROR, vm_interpret(vm, &chunk));
    vm_free(vm);
    free_chunk(&chunk);
}

TEST(vm_append_on_non_array_returns_error) {
    Chunk chunk;
    init_chunk(&chunk);

    int a = add_constant(&chunk, value_int(5));
    int b = add_constant(&chunk, value_int(1));
    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)a);
    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)b);
    write_chunk(&chunk, OP_NATIVE_CALL);
    write_chunk_u16(&chunk, (uint16_t)native_find("append"));
    write_chunk(&chunk, 2);
    write_chunk(&chunk, OP_RETURN);

    VM* vm = vm_init();
    ASSERT_INT_EQ(INTERPRET_RUNTIME_ERROR, vm_interpret(vm, &chunk));
    vm_free(vm);
    free_chunk(&chunk);
}

TEST(vm_not_false_returns_true_bool) {
    Chunk chunk;
    init_chunk(&chunk);

    int idx = add_constant(&chunk, value_bool(0));
    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)idx);
    write_chunk(&chunk, OP_NOT);
    write_chunk(&chunk, OP_RETURN);

    VM* vm = vm_init();
    ASSERT_INT_EQ(INTERPRET_OK, vm_interpret(vm, &chunk));
    Value result = vm_pop(vm);
    ASSERT_INT_EQ(VAL_BOOL, result.type);
    ASSERT_INT_EQ(1, result.as.as_int);
    vm_free(vm);
    free_chunk(&chunk);
}

/* #110: there is no per-instruction bounds check; a chunk_seal sentinel
   stops execution that runs or jumps past the last instruction. */
TEST(vm_running_off_the_end_is_a_runtime_error) {
    Chunk chunk;
    init_chunk(&chunk);
    int idx = add_constant(&chunk, value_int(1));
    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)idx);
    /* No OP_RETURN. Fill the buffer to capacity, so the sentinel needs a
       grown buffer rather than a spare byte. */
    while (chunk.count < chunk.capacity) write_chunk(&chunk, OP_DUP);

    VM* vm = vm_init();
    ASSERT_INT_EQ(INTERPRET_RUNTIME_ERROR, vm_interpret(vm, &chunk));
    ASSERT_PTR_NOT_NULL(strstr(vm_get_error(vm), "Runtime error"));
    vm_free(vm);
    free_chunk(&chunk);
}

TEST(vm_jumping_to_the_end_is_a_runtime_error) {
    Chunk chunk;
    init_chunk(&chunk);
    write_chunk(&chunk, OP_JMP);
    write_chunk_u16(&chunk, 0); /* lands exactly on the end of the code */

    VM* vm = vm_init();
    ASSERT_INT_EQ(INTERPRET_RUNTIME_ERROR, vm_interpret(vm, &chunk));
    ASSERT_PTR_NOT_NULL(strstr(vm_get_error(vm), "Runtime error"));
    vm_free(vm);
    free_chunk(&chunk);
}

TEST(vm_sentinel_is_overwritten_when_code_is_appended) {
    /* The REPL appends to a chunk it already ran. */
    Chunk chunk;
    init_chunk(&chunk);
    int one = add_constant(&chunk, value_int(1));
    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)one);
    write_chunk(&chunk, OP_RETURN);
    VM* vm = vm_init();
    ASSERT_INT_EQ(INTERPRET_OK, vm_interpret(vm, &chunk));
    vm_pop(vm);

    int start = chunk.count;
    int two = add_constant(&chunk, value_int(2));
    write_chunk(&chunk, OP_CONST);
    write_chunk_u16(&chunk, (uint16_t)two);
    write_chunk(&chunk, OP_RETURN);
    ASSERT_INT_EQ(INTERPRET_OK, vm_interpret_from(vm, &chunk, start));
    ASSERT_INT_EQ(2, vm_pop(vm).as.as_int);
    vm_free(vm);
    free_chunk(&chunk);
}

int main(void) {
    RUN_TEST(vm_init_and_free);
    RUN_TEST(vm_executes_constant_and_return);
    RUN_TEST(vm_executes_local_variables);
    RUN_TEST(vm_executes_arithmetic);
    RUN_TEST(vm_executes_subtraction);
    RUN_TEST(vm_executes_multiplication);
    RUN_TEST(vm_executes_division);
    RUN_TEST(vm_executes_comparisons);
    RUN_TEST(vm_executes_less_than);
    RUN_TEST(vm_executes_greater_than);
    RUN_TEST(vm_executes_false_comparison);
    RUN_TEST(vm_executes_conditional_jump);
    RUN_TEST(vm_executes_jump_when_false);
    RUN_TEST(vm_stack_overflow_returns_runtime_error);
    RUN_TEST(vm_stack_underflow_returns_runtime_error);
    RUN_TEST(vm_missing_return_returns_runtime_error);
    RUN_TEST(vm_invalid_constant_index_returns_runtime_error);
    RUN_TEST(vm_invalid_get_local_slot_returns_runtime_error);
    RUN_TEST(vm_invalid_set_local_slot_returns_runtime_error);
    RUN_TEST(vm_invalid_jump_target_returns_runtime_error);
    RUN_TEST(vm_invalid_jz_target_returns_runtime_error);
    RUN_TEST(vm_division_by_zero_returns_zero);
    RUN_TEST(vm_add_float_and_int);
    RUN_TEST(vm_sub_string_and_int_returns_zero);
    RUN_TEST(vm_mul_float_and_string_returns_zero);
    RUN_TEST(vm_div_string_and_int_returns_zero);
    RUN_TEST(vm_eq_float_and_int_coerces);
    RUN_TEST(vm_lt_float_and_int_coerces);
    RUN_TEST(vm_gt_string_and_int_returns_zero);
    RUN_TEST(vm_executes_procedure_call);
    RUN_TEST(vm_executes_procedure_call_with_arguments);
    RUN_TEST(vm_executes_unary_minus);
    RUN_TEST(vm_executes_unary_not);
    RUN_TEST(vm_reports_source_line_on_runtime_error);
    RUN_TEST(vm_executes_pop);
    RUN_TEST(vm_executes_bool_not_true);
    RUN_TEST(vm_executes_array_build_and_index);
    RUN_TEST(vm_executes_array_index_set);
    RUN_TEST(vm_executes_array_length);
    RUN_TEST(vm_executes_array_append);
    RUN_TEST(vm_array_index_out_of_bounds_get_returns_error);
    RUN_TEST(vm_array_index_out_of_bounds_set_returns_error);
    RUN_TEST(vm_index_get_on_non_array_returns_error);
    RUN_TEST(vm_append_on_non_array_returns_error);
    RUN_TEST(vm_not_false_returns_true_bool);
    RUN_TEST(vm_running_off_the_end_is_a_runtime_error);
    RUN_TEST(vm_jumping_to_the_end_is_a_runtime_error);
    RUN_TEST(vm_sentinel_is_overwritten_when_code_is_appended);
    TEST_SUMMARY();
}
