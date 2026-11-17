#include <stdlib.h>
#include <string.h>

#include "compiler.h"
#include "trigger.h"

static int grow_capacity(int capacity) {
    return capacity < 8 ? 8 : capacity * 2;
}

void init_chunk(Chunk* chunk) {
    chunk->code = NULL;
    chunk->count = 0;
    chunk->capacity = 0;
    chunk->lines = NULL;
    chunk->lines_count = 0;
    chunk->lines_capacity = 0;
    chunk->columns = NULL;
    chunk->columns_count = 0;
    chunk->columns_capacity = 0;
    chunk->constants = NULL;
    chunk->constants_count = 0;
    chunk->constants_capacity = 0;
    chunk->triggers = NULL;
    chunk->trigger_count = 0;
    chunk->trigger_capacity = 0;
    chunk->source_path = NULL;
}

void free_chunk(Chunk* chunk) {
    if (chunk->constants != NULL) {
        for (int i = 0; i < chunk->constants_count; i++) {
            value_release(chunk->constants[i]);
        }
    }
    for (int i = 0; i < chunk->trigger_count; i++) {
        free(chunk->triggers[i].name);
        free(chunk->triggers[i].table);
    }
    free(chunk->triggers);
    free(chunk->code);
    free(chunk->lines);
    free(chunk->columns);
    free(chunk->constants);
    init_chunk(chunk);
}

void chunk_add_trigger(Chunk* chunk, const char* name, int timing, int event,
                       const char* table, int offset, int for_each_row) {
    if (chunk->trigger_count >= chunk->trigger_capacity) {
        chunk->trigger_capacity = grow_capacity(chunk->trigger_capacity);
        ChunkTrigger* new_triggers = realloc(chunk->triggers,
            sizeof(ChunkTrigger) * (size_t)chunk->trigger_capacity);
        if (new_triggers == NULL) return;
        chunk->triggers = new_triggers;
    }
    ChunkTrigger* t = &chunk->triggers[chunk->trigger_count];
    t->name = malloc(strlen(name) + 1);
    t->table = malloc(strlen(table) + 1);
    if (t->name == NULL || t->table == NULL) {
        free(t->name);
        free(t->table);
        return;
    }
    strcpy(t->name, name);
    strcpy(t->table, table);
    t->timing = timing;
    t->event = event;
    t->offset = offset;
    t->for_each_row = for_each_row;
    chunk->trigger_count++;
}

void chunk_remove_trigger(Chunk* chunk, const char* name) {
    for (int i = 0; i < chunk->trigger_count; i++) {
        if (trigger_name_equals(chunk->triggers[i].name, name)) {
            chunk->triggers[i].offset = -1;
        }
    }
}

void write_chunk_line(Chunk* chunk, uint8_t byte, int line, int column) {
    if (chunk->count >= chunk->capacity) {
        chunk->capacity = grow_capacity(chunk->capacity);
        uint8_t* new_code = realloc(chunk->code, (size_t)chunk->capacity);
        if (new_code == NULL) return;
        chunk->code = new_code;
    }
    chunk->code[chunk->count] = byte;
    chunk->count++;

    if (chunk->lines_count >= chunk->lines_capacity) {
        chunk->lines_capacity = grow_capacity(chunk->lines_capacity);
        int* new_lines = realloc(chunk->lines, sizeof(int) * (size_t)chunk->lines_capacity);
        if (new_lines == NULL) return;
        chunk->lines = new_lines;
    }
    chunk->lines[chunk->lines_count++] = line;

    if (chunk->columns_count >= chunk->columns_capacity) {
        chunk->columns_capacity = grow_capacity(chunk->columns_capacity);
        int* new_columns = realloc(chunk->columns, sizeof(int) * (size_t)chunk->columns_capacity);
        if (new_columns == NULL) return;
        chunk->columns = new_columns;
    }
    chunk->columns[chunk->columns_count++] = column;
}

void write_chunk(Chunk* chunk, uint8_t byte) {
    write_chunk_line(chunk, byte, 0, 0);
}

int chunk_seal(Chunk* chunk) {
    if (chunk->count >= chunk->capacity) {
        int capacity = grow_capacity(chunk->capacity);
        uint8_t* new_code = realloc(chunk->code, (size_t)capacity);
        if (new_code == NULL) return 0;
        chunk->code = new_code;
        chunk->capacity = capacity;
    }
    chunk->code[chunk->count] = OP_END_OF_CODE;
    return 1;
}

int add_constant(Chunk* chunk, Value value) {
    if (chunk->constants_count >= chunk->constants_capacity) {
        chunk->constants_capacity = grow_capacity(chunk->constants_capacity);
        Value* new_constants = realloc(chunk->constants,
                                       sizeof(Value) * (size_t)chunk->constants_capacity);
        if (new_constants == NULL) return -1;
        chunk->constants = new_constants;
    }
    value_retain(value);
    chunk->constants[chunk->constants_count] = value;
    chunk->constants_count++;
    return chunk->constants_count - 1;
}

void write_chunk_u16_line(Chunk* chunk, uint16_t value, int line, int column) {
    write_chunk_line(chunk, (uint8_t)((value >> 8) & 0xFF), line, column);
    write_chunk_line(chunk, (uint8_t)(value & 0xFF), line, column);
}

void write_chunk_u16(Chunk* chunk, uint16_t value) {
    write_chunk_u16_line(chunk, value, 0, 0);
}

uint16_t read_u16(const uint8_t* bytes) {
    return (uint16_t)((bytes[0] << 8) | bytes[1]);
}
