#include <stdio.h>
#include <string.h>

#include "memory.h"
#include "object.h"
#include "value.h"
#include "vm.h"

#define ALLOCATE_OBJ(type, objectType) \
    (type*)allocateObject(sizeof(type), objectType)

static Obj* allocateObject(size_t size, ObjType type) {
  Obj* object = (Obj*)reallocate(NULL, 0, size);
  object->type = type;

  object->next = vm.objects;
  vm.objects = object;
  return object;
}

static ObjString* allocateOwnedString(int length) {
  ObjString* string = (ObjString*)allocateObject(
      sizeof(ObjString) + sizeof(char) * (length + 1),
      OBJ_STRING);
  string->length = length;
  string->ownsChars = true;
  string->chars = string->ownedChars;
  string->ownedChars[length] = '\0';
  return string;
}

static ObjString* allocateConstantString(const char* chars, int length) {
  ObjString* string = (ObjString*)allocateObject(sizeof(ObjString),
      OBJ_STRING);
  string->length = length;
  string->ownsChars = false;
  string->chars = (char*)chars;
  return string;
}

ObjString* takeString(char* chars, int length) {
  ObjString* string = allocateOwnedString(length);
  memcpy(string->chars, chars, length);
  FREE_ARRAY(char, chars, length + 1);
  return string;
}

ObjString* copyString(const char* chars, int length) {
  ObjString* string = allocateOwnedString(length);
  memcpy(string->chars, chars, length);
  return string;
}

ObjString* constantString(const char* chars, int length) {
  return allocateConstantString(chars, length);
}

ObjString* concatenateStrings(ObjString* a, ObjString* b) {
  int length = a->length + b->length;
  ObjString* string = allocateOwnedString(length);
  memcpy(string->chars, a->chars, a->length);
  memcpy(string->chars + a->length, b->chars, b->length);
  return string;
}

void printObject(Value value) {
  switch (OBJ_TYPE(value)) {
    case OBJ_STRING: {
      ObjString* string = AS_STRING(value);
      printf("%.*s", string->length, string->chars);
      break;
    }
  }
}