#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "common.h"
#include "compiler.h"
#include "debug.h"
#include "memory.h"
#include "object.h"
#include "vm.h"

VM vm;

static void resetStack() {
  vm.stackTop = vm.stack;
  vm.frameCount = 0;
  vm.openUpvalues = NULL;
}

void runtimeError(const char* format, ...) {
  va_list args;
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
  fputs("\n", stderr);

  for (int i = vm.frameCount - 1; i >= 0; i--) {
    CallFrame* frame = &vm.frames[i];
    ObjFunction* function = frame->closure->function;
    size_t instruction = frame->ip - function->chunk.code - 1;
    int line = getLine(&function->chunk, (int)instruction);
    fprintf(stderr, "[line %d] in ", line);
    if (function->name == NULL) {
      fprintf(stderr, "script\n");
    } else {
      fprintf(stderr, "%s()\n", function->name->chars);
    }
  }

  resetStack();
}

static bool clockNative(int argCount, Value* args, Value* result) {
  (void)argCount; (void)args;
  *result = NUMBER_VAL((double)clock() / CLOCKS_PER_SEC);
  return true;
}

static bool sqrtNative(int argCount, Value* args, Value* result) {
  (void)argCount;
  if (!IS_NUMBER(args[0])) {
    runtimeError("sqrt() expects a number.");
    return false;
  }
  double n = AS_NUMBER(args[0]);
  if (n < 0) {
    runtimeError("sqrt() of negative number.");
    return false;
  }
  *result = NUMBER_VAL(sqrt(n));
  return true;
}

static bool absNative(int argCount, Value* args, Value* result) {
  (void)argCount;
  if (!IS_NUMBER(args[0])) {
    runtimeError("abs() expects a number.");
    return false;
  }
  *result = NUMBER_VAL(fabs(AS_NUMBER(args[0])));
  return true;
}

static bool floorNative(int argCount, Value* args, Value* result) {
  (void)argCount;
  if (!IS_NUMBER(args[0])) {
    runtimeError("floor() expects a number.");
    return false;
  }
  *result = NUMBER_VAL(floor(AS_NUMBER(args[0])));
  return true;
}

static bool powNative(int argCount, Value* args, Value* result) {
  (void)argCount;
  if (!IS_NUMBER(args[0]) || !IS_NUMBER(args[1])) {
    runtimeError("pow() expects two numbers.");
    return false;
  }
  *result = NUMBER_VAL(pow(AS_NUMBER(args[0]), AS_NUMBER(args[1])));
  return true;
}

static bool strLenNative(int argCount, Value* args, Value* result) {
  (void)argCount;
  if (!IS_STRING(args[0])) {
    runtimeError("strLen() expects a string.");
    return false;
  }
  *result = NUMBER_VAL((double)AS_STRING(args[0])->length);
  return true;
}

static bool numberNative(int argCount, Value* args, Value* result) {
  (void)argCount;
  if (IS_NUMBER(args[0])) { *result = args[0]; return true; }
  if (IS_STRING(args[0])) {
    char* end;
    double n = strtod(AS_CSTRING(args[0]), &end);
    if (end == AS_CSTRING(args[0])) {
      runtimeError("number() could not parse string.");
      return false;
    }
    *result = NUMBER_VAL(n);
    return true;
  }
  runtimeError("number() expects a string or number.");
  return false;
}

static bool readLineNative(int argCount, Value* args, Value* result) {
  (void)argCount; (void)args;
  char buffer[1024];
  if (fgets(buffer, sizeof(buffer), stdin) == NULL) {
    *result = NIL_VAL;
    return true;
  }
  size_t len = strlen(buffer);
  if (len > 0 && buffer[len - 1] == '\n') buffer[--len] = '\0';
  *result = OBJ_VAL(copyString(buffer, (int)len));
  return true;
}

static void defineNative(const char* name, NativeFn function, int arity) {
  push(OBJ_VAL(copyString(name, (int)strlen(name))));
  push(OBJ_VAL(newNative(function, arity, name)));

  ObjString* nameStr = AS_STRING(vm.stack[0]);
  int slot = -1;
  for (int i = 0; i < vm.globalCount; i++) {
    if (vm.globals[i].name == nameStr) { slot = i; break; }
  }
  if (slot == -1) {
    if (vm.globalCount >= GLOBAL_MAX) {
      pop(); pop();
      return;
    }
    slot = vm.globalCount++;
    vm.globals[slot].name = nameStr;
  }
  vm.globals[slot].value = vm.stack[1];
  vm.globals[slot].defined = true;
  vm.globals[slot].isMutable = false;

  pop();
  pop();
}

void initVM(void) {
  resetStack();
  vm.objects = NULL;
  vm.bytesAllocated = 0;
  vm.nextGC = 1024 * 1024;
  vm.grayCount = 0;
  vm.grayCapacity = 0;
  vm.grayStack = NULL;

  vm.globalCount = 0;
  for (int i = 0; i < GLOBAL_MAX; i++) {
    vm.globals[i].name = NULL;
    vm.globals[i].value = NIL_VAL;
    vm.globals[i].defined = false;
    vm.globals[i].isMutable = true;
  }
  initTable(&vm.strings);

  vm.initString = NULL;
  vm.initString = copyString("init", 4);

  defineNative("clock", clockNative, 0);
  defineNative("sqrt", sqrtNative, 1);
  defineNative("abs", absNative, 1);
  defineNative("floor", floorNative, 1);
  defineNative("pow", powNative, 2);
  defineNative("strLen", strLenNative, 1);
  defineNative("number", numberNative, 1);
  defineNative("readLine", readLineNative, 0);
}

void freeVM(void) {
  freeTable(&vm.strings);
  vm.initString = NULL;
  freeObjects();
}

void push(Value value) {
  *vm.stackTop = value;
  vm.stackTop++;
}

Value pop(void) {
  vm.stackTop--;
  return *vm.stackTop;
}

static Value peek(int distance) {
  return vm.stackTop[-1 - distance];
}

static bool call(ObjClosure* closure, int argCount) {
  if (argCount != closure->function->arity) {
    runtimeError("Expected %d arguments but got %d.", closure->function->arity, argCount);
    return false;
  }

  if (vm.frameCount == FRAMES_MAX) {
    runtimeError("Stack overflow.");
    return false;
  }

  CallFrame* frame = &vm.frames[vm.frameCount++];
  frame->closure = closure;
  frame->ip = closure->function->chunk.code;
  frame->slots = vm.stackTop - argCount - 1;
  return true;
}

static bool callValue(Value callee, int argCount) {
  if (IS_OBJ(callee)) {
    switch (OBJ_TYPE(callee)) {
      case OBJ_BOUND_METHOD: {
        ObjBoundMethod* bound = AS_BOUND_METHOD(callee);
        vm.stackTop[-argCount - 1] = bound->receiver;
        return call(bound->method, argCount);
      }
      case OBJ_CLASS: {
        ObjClass* klass = AS_CLASS(callee);
        vm.stackTop[-argCount - 1] = OBJ_VAL(newInstance(klass));
        Value initializer;
        if (tableGet(&klass->methods, vm.initString, &initializer)) {
          return call(AS_CLOSURE(initializer), argCount);
        } else if (argCount != 0) {
          runtimeError("Expected 0 arguments but got %d.", argCount);
          return false;
        }
        return true;
      }
      case OBJ_CLOSURE:
        return call(AS_CLOSURE(callee), argCount);
      case OBJ_NATIVE: {
        ObjNative* native = AS_NATIVE(callee);
        if (argCount != native->arity) {
          runtimeError("Expected %d arguments but got %d.", native->arity, argCount);
          return false;
        }
        Value result;
        if (!native->function(argCount, vm.stackTop - argCount, &result)) {
          return false;
        }
        vm.stackTop -= argCount + 1;
        push(result);
        return true;
      }
      default:
        break;
    }
  }
  runtimeError("Can only call functions and classes.");
  return false;
}

static bool invokeFromClass(ObjClass* klass, ObjString* name, int argCount) {
  Value method;
  if (!tableGet(&klass->methods, name, &method)) {
    runtimeError("Undefined property '%s'.", name->chars);
    return false;
  }
  return call(AS_CLOSURE(method), argCount);
}

static bool invoke(ObjString* name, int argCount) {
  Value receiver = peek(argCount);
  if (!IS_INSTANCE(receiver)) {
    runtimeError("Only instances have methods.");
    return false;
  }
  ObjInstance* instance = AS_INSTANCE(receiver);

  Value value;
  if (tableGet(&instance->fields, name, &value)) {
    vm.stackTop[-argCount - 1] = value;
    return callValue(value, argCount);
  }

  return invokeFromClass(instance->klass, name, argCount);
}

static bool bindMethod(ObjClass* klass, ObjString* name) {
  Value method;
  if (!tableGet(&klass->methods, name, &method)) {
    runtimeError("Undefined property '%s'.", name->chars);
    return false;
  }

  ObjBoundMethod* bound = newBoundMethod(peek(0), AS_CLOSURE(method));
  pop();
  push(OBJ_VAL(bound));
  return true;
}

static ObjUpvalue* captureUpvalue(Value* local) {
  ObjUpvalue* prevUpvalue = NULL;
  ObjUpvalue* upvalue = vm.openUpvalues;
  while (upvalue != NULL && upvalue->location > local) {
    prevUpvalue = upvalue;
    upvalue = upvalue->next;
  }

  if (upvalue != NULL && upvalue->location == local) {
    return upvalue;
  }

  ObjUpvalue* createdUpvalue = newUpvalue(local);
  createdUpvalue->next = upvalue;

  if (prevUpvalue == NULL) {
    vm.openUpvalues = createdUpvalue;
  } else {
    prevUpvalue->next = createdUpvalue;
  }

  return createdUpvalue;
}

static void closeUpvalues(Value* last) {
  while (vm.openUpvalues != NULL && vm.openUpvalues->location >= last) {
    ObjUpvalue* upvalue = vm.openUpvalues;
    upvalue->closed = *upvalue->location;
    upvalue->location = &upvalue->closed;
    vm.openUpvalues = upvalue->next;
  }
}

static void defineMethod(ObjString* name) {
  Value method = peek(0);
  ObjClass* klass = AS_CLASS(peek(1));
  tableSet(&klass->methods, name, method);
  pop();
}

static bool isFalsey(Value value) {
  return IS_NIL(value) || (IS_BOOL(value) && !AS_BOOL(value));
}

static void concatenate() {
  ObjString* b = AS_STRING(peek(0));
  ObjString* a = AS_STRING(peek(1));

  int length = a->length + b->length;
  char* chars = ALLOCATE(char, length + 1);
  memcpy(chars, a->chars, a->length);
  memcpy(chars + a->length, b->chars, b->length);
  chars[length] = '\0';

  ObjString* result = takeString(chars, length);
  pop();
  pop();
  push(OBJ_VAL(result));
}

static InterpretResult run() {
  CallFrame* frame = &vm.frames[vm.frameCount - 1];
  register uint8_t* ip = frame->ip;

#define LOAD_FRAME() \
    do { frame = &vm.frames[vm.frameCount - 1]; ip = frame->ip; } while (false)
#define STORE_FRAME() (frame->ip = ip)

#define READ_BYTE() (*ip++)
#define READ_SHORT() \
    (ip += 2, (uint16_t)((ip[-2] << 8) | ip[-1]))
#define READ_CONSTANT() \
    (frame->closure->function->chunk.constants.values[READ_BYTE()])
#define READ_STRING() AS_STRING(READ_CONSTANT())
#define READ_SLOT() (READ_BYTE())
#define RUNTIME_ERROR(...) \
    do { STORE_FRAME(); runtimeError(__VA_ARGS__); \
         return INTERPRET_RUNTIME_ERROR; } while (false)
#define BINARY_OP(valueType, op) \
    do { \
      if (!IS_NUMBER(peek(0)) || !IS_NUMBER(peek(1))) { \
        RUNTIME_ERROR("Operands must be numbers."); \
      } \
      double b = AS_NUMBER(pop()); \
      double a = AS_NUMBER(pop()); \
      push(valueType(a op b)); \
    } while (false)

  for (;;) {
#ifdef DEBUG_TRACE_EXECUTION
    printf("          ");
    for (Value* slot = vm.stack; slot < vm.stackTop; slot++) {
      printf("[ ");
      printValue(*slot);
      printf(" ]");
    }
    printf("\n");
    disassembleInstruction(&frame->closure->function->chunk,
        (int)(ip - frame->closure->function->chunk.code));
#endif

    uint8_t instruction;
    switch (instruction = READ_BYTE()) {
      case OP_CONSTANT: {
        Value constant = READ_CONSTANT();
        push(constant);
        break;
      }
      case OP_CONSTANT_LONG: {
        uint32_t index = (uint32_t)(READ_BYTE() << 16);
        index |= (uint32_t)(READ_BYTE() << 8);
        index |= READ_BYTE();
        push(frame->closure->function->chunk.constants.values[index]);
        break;
      }
      case OP_NIL: push(NIL_VAL); break;
      case OP_TRUE: push(BOOL_VAL(true)); break;
      case OP_FALSE: push(BOOL_VAL(false)); break;
      case OP_POP: pop(); break;
      case OP_GET_LOCAL: {
        uint8_t slot = READ_BYTE();
        push(frame->slots[slot]);
        break;
      }
      case OP_SET_LOCAL: {
        uint8_t slot = READ_BYTE();
        frame->slots[slot] = peek(0);
        break;
      }
      case OP_GET_LOCAL_LONG: {
        uint16_t slot = (uint16_t)(READ_BYTE() << 8);
        slot |= READ_BYTE();
        push(frame->slots[slot]);
        break;
      }
      case OP_SET_LOCAL_LONG: {
        uint16_t slot = (uint16_t)(READ_BYTE() << 8);
        slot |= READ_BYTE();
        frame->slots[slot] = peek(0);
        break;
      }
      case OP_GET_GLOBAL: {
        uint8_t slot = READ_SLOT();
        GlobalSlot* global = &vm.globals[slot];
        if (!global->defined) {
          RUNTIME_ERROR("Undefined variable '%s'.", global->name->chars);
        }
        push(global->value);
        break;
      }
      case OP_DEFINE_GLOBAL: {
        uint8_t slot = READ_SLOT();
        vm.globals[slot].value = peek(0);
        vm.globals[slot].defined = true;
        pop();
        break;
      }
      case OP_SET_GLOBAL: {
        uint8_t slot = READ_SLOT();
        GlobalSlot* global = &vm.globals[slot];
        if (!global->defined) {
          RUNTIME_ERROR("Undefined variable '%s'.", global->name->chars);
        }
        global->value = peek(0);
        break;
      }
      case OP_GET_PROPERTY: {
        if (!IS_INSTANCE(peek(0))) {
          RUNTIME_ERROR("Only instances have properties.");
        }
        ObjInstance* instance = AS_INSTANCE(peek(0));
        ObjString* name = READ_STRING();

        Value value;
        if (tableGet(&instance->fields, name, &value)) {
          pop();
          push(value);
          break;
        }

        ObjString* methodName = name;
        for (int i = 0; i < name->length; i++) {
          if (name->chars[i] == '$') {
            methodName = copyString(name->chars + i + 1, name->length - i - 1);
            break;
          }
        }

        STORE_FRAME();
        if (!bindMethod(instance->klass, methodName)) {
          return INTERPRET_RUNTIME_ERROR;
        }
        break;
      }
      case OP_SET_PROPERTY: {
        if (!IS_INSTANCE(peek(1))) {
          RUNTIME_ERROR("Only instances have fields.");
        }
        ObjInstance* instance = AS_INSTANCE(peek(1));
        tableSet(&instance->fields, READ_STRING(), peek(0));
        Value value = pop();
        pop();
        push(value);
        break;
      }
      case OP_EQUAL: {
        Value b = pop();
        Value a = pop();
        push(BOOL_VAL(valuesEqual(a, b)));
        break;
      }
      case OP_GREATER: BINARY_OP(BOOL_VAL, >); break;
      case OP_LESS: BINARY_OP(BOOL_VAL, <); break;
      case OP_ADD: {
        if (IS_STRING(peek(0)) && IS_STRING(peek(1))) {
          concatenate();
        } else if (IS_NUMBER(peek(0)) && IS_NUMBER(peek(1))) {
          double b = AS_NUMBER(pop());
          double a = AS_NUMBER(pop());
          push(NUMBER_VAL(a + b));
        } else {
          RUNTIME_ERROR("Operands must be two numbers or two strings.");
        }
        break;
      }
      case OP_SUBTRACT: BINARY_OP(NUMBER_VAL, -); break;
      case OP_MULTIPLY: BINARY_OP(NUMBER_VAL, *); break;
      case OP_DIVIDE: BINARY_OP(NUMBER_VAL, /); break;
      case OP_NOT:
        push(BOOL_VAL(isFalsey(pop())));
        break;
      case OP_NEGATE:
        if (!IS_NUMBER(peek(0))) {
          RUNTIME_ERROR("Operand must be a number.");
        }
        push(NUMBER_VAL(-AS_NUMBER(pop())));
        break;
      case OP_PRINT: {
        printValue(pop());
        printf("\n");
        break;
      }
      case OP_DUP:
        push(peek(0));
        break;
      case OP_JUMP: {
        uint16_t offset = READ_SHORT();
        ip += offset;
        break;
      }
      case OP_JUMP_IF_FALSE: {
        uint16_t offset = READ_SHORT();
        if (isFalsey(peek(0))) ip += offset;
        break;
      }
      case OP_LOOP: {
        uint16_t offset = READ_SHORT();
        ip -= offset;
        break;
      }
      case OP_CALL: {
        int argCount = READ_BYTE();
        STORE_FRAME();
        if (!callValue(peek(argCount), argCount)) {
          return INTERPRET_RUNTIME_ERROR;
        }
        LOAD_FRAME();
        break;
      }
      case OP_CLOSURE: {
        ObjFunction* function = AS_FUNCTION(READ_CONSTANT());
        ObjClosure* closure = newClosure(function);
        push(OBJ_VAL(closure));
        for (int i = 0; i < closure->upvalueCount; i++) {
          uint8_t isLocal = READ_BYTE();
          uint8_t index = READ_BYTE();
          if (isLocal) {
            closure->upvalues[i] = captureUpvalue(frame->slots + index);
          } else {
            closure->upvalues[i] = frame->closure->upvalues[index];
          }
        }
        break;
      }
      case OP_GET_UPVALUE: {
        uint8_t slot = READ_BYTE();
        push(*frame->closure->upvalues[slot]->location);
        break;
      }
      case OP_SET_UPVALUE: {
        uint8_t slot = READ_BYTE();
        *frame->closure->upvalues[slot]->location = peek(0);
        break;
      }
      case OP_CLOSE_UPVALUE:
        closeUpvalues(vm.stackTop - 1);
        pop();
        break;
      case OP_CLASS:
        push(OBJ_VAL(newClass(READ_STRING())));
        break;
      case OP_GET_SUPER: {
        ObjString* name = READ_STRING();
        ObjClass* superclass = AS_CLASS(pop());

        STORE_FRAME();
        if (!bindMethod(superclass, name)) {
          return INTERPRET_RUNTIME_ERROR;
        }
        break;
      }
      case OP_INVOKE: {
        ObjString* method = READ_STRING();
        int argCount = READ_BYTE();
        STORE_FRAME();
        if (!invoke(method, argCount)) {
          return INTERPRET_RUNTIME_ERROR;
        }
        LOAD_FRAME();
        break;
      }
      case OP_SUPER_INVOKE: {
        ObjString* method = READ_STRING();
        int argCount = READ_BYTE();
        ObjClass* superclass = AS_CLASS(pop());
        STORE_FRAME();
        if (!invokeFromClass(superclass, method, argCount)) {
          return INTERPRET_RUNTIME_ERROR;
        }
        LOAD_FRAME();
        break;
      }
      case OP_INHERIT: {
        Value superclass = peek(1);
        if (!IS_CLASS(superclass)) {
          RUNTIME_ERROR("Superclass must be a class.");
        }
        ObjClass* subclass = AS_CLASS(peek(0));
        tableAddAll(&AS_CLASS(superclass)->methods, &subclass->methods);
        pop();
        break;
      }
      case OP_METHOD:
        defineMethod(READ_STRING());
        break;
      case OP_RETURN: {
        Value result = pop();
        closeUpvalues(frame->slots);
        vm.frameCount--;
        if (vm.frameCount == 0) {
          pop();
          return INTERPRET_OK;
        }

        vm.stackTop = frame->slots;
        push(result);
        LOAD_FRAME();
        break;
      }
      default:
        RUNTIME_ERROR("Unknown opcode %d.", instruction);
    }
  }

#undef READ_BYTE
#undef READ_SHORT
#undef READ_CONSTANT
#undef READ_STRING
#undef READ_SLOT
#undef BINARY_OP
#undef LOAD_FRAME
#undef STORE_FRAME
#undef RUNTIME_ERROR
}

InterpretResult interpret(const char* source) {
  ObjFunction* function = compile(source);
  if (function == NULL) return INTERPRET_COMPILE_ERROR;

  push(OBJ_VAL(function));
  ObjClosure* closure = newClosure(function);
  pop();
  push(OBJ_VAL(closure));
  call(closure, 0);

  return run();
}
