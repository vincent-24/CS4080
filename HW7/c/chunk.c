#include <stdlib.h>
#include "chunk.h"
#include "memory.h"

void initChunk(Chunk* chunk) {
  chunk->count = 0;
  chunk->capacity = 0;
  chunk->code = NULL;
  initValueArray(&chunk->constants);
}

void freeChunk(Chunk* chunk) {
  FREE_ARRAY(uint8_t, chunk->code, chunk->capacity);
  freeValueArray(&chunk->constants);
  initChunk(chunk);
}

void writeChunk(Chunk* chunk, uint8_t byte, int line) {
  if (chunk->capacity < chunk->count + 1) {
    int oldCapacity = chunk->capacity;
    chunk->capacity = GROW_CAPACITY(oldCapacity);
    chunk->code = GROW_ARRAY(uint8_t, chunk->code, oldCapacity, chunk->capacity);
  }

  chunk->code[chunk->count] = byte;
  chunk->count++;

  if (chunk->lines.count > 0 && chunk->lines.values[chunk->lines.count - 1].line == line) {
    chunk->lines.values[chunk->lines.count - 1].runLength++;
  } else {
    if (chunk->lines.capacity < chunk->lines.count + 1) {
      int oldCapacity = chunk->lines.capacity;
      chunk->lines.capacity = GROW_CAPACITY(oldCapacity);
      chunk->lines.values = GROW_ARRAY(LineRun, chunk->lines.values, oldCapacity, chunk->lines.capacity);
    }
    chunk->lines.values[chunk->lines.count].line = line;
    chunk->lines.values[chunk->lines.count].runLength = 1;
    chunk->lines.count++;
  }
}

int getLine(Chunk* chunk, int instruction) {
  int offset = 0;
  for (int i = 0; i < chunk->lines.count; i++) {
    if (instruction < offset + chunk->lines.values[i].runLength) {
      return chunk->lines.values[i].line;
    }
    offset += chunk->lines.values[i].runLength;
  }
  return -1; 
}

int addConstant(Chunk* chunk, Value value) {
  writeValueArray(&chunk->constants, value);
  return chunk->constants.count - 1;
}

void writeConstant(Chunk* chunk, Value value, int line) {
  int constantIndex = addConstant(chunk, value);
  writeChunk(chunk, OP_CONSTANT, line);
  writeChunk(chunk, (uint8_t)constantIndex, line);
}