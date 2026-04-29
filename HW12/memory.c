#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "chunk.h"
#include "memory.h"
#include "object.h"
#include "vm.h"

static uint8_t* heap = NULL;

typedef struct BlockHeader {
  size_t size;
  int is_free;
  struct BlockHeader* next;
} BlockHeader;

static BlockHeader* free_list = NULL;

void initHeap(void) {
  if (heap == NULL) {
    heap = (uint8_t*)malloc(1024 * 1024);
    free_list = (BlockHeader*)heap;
    free_list->size = 1024 * 1024 - sizeof(BlockHeader);
    free_list->is_free = 1;
    free_list->next = NULL;
  }
}

void* reallocate(void* pointer, size_t oldSize, size_t newSize) {
  if (heap == NULL) {
    initHeap();
  }

  if (newSize == 0) {
    freeBlock(pointer);
    return NULL;
  }

  if (pointer == NULL) {
    return allocateBlock(newSize);
  }

  void* newBlock = allocateBlock(newSize);
  if (newBlock == NULL) {
    return NULL;
  }

  size_t copySize = oldSize < newSize ? oldSize : newSize;
  memcpy(newBlock, pointer, copySize);
  freeBlock(pointer);
  return newBlock;
}

static void freeObject(Obj* object) {
  switch (object->type) {
    case OBJ_CLOSURE: {
      ObjClosure* closure = (ObjClosure*)object;
      FREE_ARRAY(ObjUpvalue*, closure->upvalues, closure->upvalueCount);
      reallocate(object, sizeof(ObjClosure), 0);
      break;
    }
    case OBJ_FUNCTION: {
      ObjFunction* function = (ObjFunction*)object;
      freeChunk(&function->chunk);
      reallocate(object, sizeof(ObjFunction), 0);
      break;
    }
    case OBJ_NATIVE: {
      reallocate(object, sizeof(ObjNative), 0);
      break;
    }
    case OBJ_STRING: {
      ObjString* string = (ObjString*)object;
      FREE_ARRAY(char, string->chars, string->length + 1);
      reallocate(object, sizeof(ObjString), 0);
      break;
    }
    case OBJ_UPVALUE: {
      reallocate(object, sizeof(ObjUpvalue), 0);
      break;
    }
  }
}

void freeObjects(void) {
  Obj* object = vm.objects;
  while (object != NULL) {
    Obj* next = object->next;
    freeObject(object);
    object = next;
  }
}

void* allocateBlock(size_t size) {
  BlockHeader* current = free_list;

  while (current != NULL) {
    if (current->is_free && current->size >= size) {
      if (current->size > size + sizeof(BlockHeader)) {
        BlockHeader* newBlock = (BlockHeader*)((uint8_t*)current + sizeof(BlockHeader) + size);
        newBlock->size = current->size - size - sizeof(BlockHeader);
        newBlock->is_free = 1;
        newBlock->next = current->next;
        current->next = newBlock;
      }

      current->size = size;
      current->is_free = 0;
      return (uint8_t*)current + sizeof(BlockHeader);
    }

    current = current->next;
  }

  return NULL;
}

void freeBlock(void* pointer) {
  if (pointer == NULL) {
    return;
  }

  BlockHeader* block = (BlockHeader*)((uint8_t*)pointer - sizeof(BlockHeader));
  block->is_free = 1;

  BlockHeader* current = free_list;
  while (current != NULL) {
    if (current->is_free && current->next != NULL && current->next->is_free) {
      current->size += current->next->size + sizeof(BlockHeader);
      current->next = current->next->next;
    }
    current = current->next;
  }
}