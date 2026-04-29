#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "compiler.h"
#include "memory.h"
#include "object.h"
#include "scanner.h"

#ifdef DEBUG_PRINT_CODE
#include "debug.h"
#endif

typedef struct {
  Token current;
  Token previous;
  bool hadError;
  bool panicMode;
} Parser;

typedef enum {
  PREC_NONE,
  PREC_ASSIGNMENT,  // =
  PREC_TERNARY,     // ? :
  PREC_OR,          // or
  PREC_AND,         // and
  PREC_EQUALITY,    // == !=
  PREC_COMPARISON,  // < > <= >=
  PREC_TERM,        // + -
  PREC_FACTOR,      // * /
  PREC_UNARY,       // ! -
  PREC_CALL,        // . ()
  PREC_PRIMARY
} Precedence;

typedef void (*ParseFn)(bool canAssign);

typedef struct {
  ParseFn prefix;
  ParseFn infix;
  Precedence precedence;
} ParseRule;

typedef struct {
  Token name;
  int depth;
  int previous;
  bool isMutable;
  bool isCaptured;
} Local;

typedef struct {
  uint8_t index;
  bool isLocal;
} Upvalue;

typedef struct {
  const char* start;
  int length;
  int local;
  bool occupied;
} LocalMapEntry;

#define LOCAL_MAX UINT16_COUNT
#define LOCAL_MAP_SIZE (UINT16_COUNT * 2)

typedef struct Loop {
  int start;
  int scopeDepth;
  struct Loop* enclosing;
} Loop;

typedef enum {
  TYPE_FUNCTION,
  TYPE_SCRIPT
} FunctionType;

typedef struct Compiler {
  struct Compiler* enclosing;
  ObjFunction* function;
  FunctionType type;

  Local* locals;
  int localCount;
  int scopeDepth;
  LocalMapEntry* localMap;
  Upvalue upvalues[UINT8_COUNT];
  Loop* currentLoop;
} Compiler;

Parser parser;
Compiler* current = NULL;

static Chunk* currentChunk() {
  return &current->function->chunk;
}

static void errorAt(Token* token, const char* message) {
  if (parser.panicMode) return;
  parser.panicMode = true;
  fprintf(stderr, "[line %d] Error", token->line);

  if (token->type == TOKEN_EOF) {
    fprintf(stderr, " at end");
  } else if (token->type == TOKEN_ERROR) {
    // Nothing.
  } else {
    fprintf(stderr, " at '%.*s'", token->length, token->start);
  }

  fprintf(stderr, ": %s\n", message);
  parser.hadError = true;
}

static void error(const char* message) {
  errorAt(&parser.previous, message);
}

static void errorAtCurrent(const char* message) {
  errorAt(&parser.current, message);
}

static void advance() {
  parser.previous = parser.current;

  for (;;) {
    parser.current = scanToken();
    if (parser.current.type != TOKEN_ERROR) break;

    errorAtCurrent(parser.current.start);
  }
}

static void consume(TokenType type, const char* message) {
  if (parser.current.type == type) {
    advance();
    return;
  }

  errorAtCurrent(message);
}

static bool check(TokenType type) {
  return parser.current.type == type;
}

static bool match(TokenType type) {
  if (!check(type)) return false;
  advance();
  return true;
}

static void emitByte(uint8_t byte) {
  writeChunk(currentChunk(), byte, parser.previous.line);
}

static void emitBytes(uint8_t byte1, uint8_t byte2) {
  emitByte(byte1);
  emitByte(byte2);
}

static void emitReturn() {
  emitByte(OP_NIL);
  emitByte(OP_RETURN);
}

static uint8_t makeConstant(Value value) {
  int constant = addConstant(currentChunk(), value);
  if (constant > UINT8_MAX) {
    error("Too many constants in one chunk.");
    return 0;
  }

  return (uint8_t)constant;
}

static void emitConstant(Value value) {
  emitBytes(OP_CONSTANT, makeConstant(value));
}

static void emitLocalOp(uint8_t shortOp, uint8_t longOp, int slot) {
  if (slot <= UINT8_MAX) {
    emitBytes(shortOp, (uint8_t)slot);
  } else {
    emitByte(longOp);
    emitByte((uint8_t)((slot >> 8) & 0xff));
    emitByte((uint8_t)(slot & 0xff));
  }
}

static int emitJump(uint8_t instruction) {
  emitByte(instruction);
  emitByte(0xff);
  emitByte(0xff);
  return currentChunk()->count - 2;
}

static void patchJump(int offset) {
  int jump = currentChunk()->count - offset - 2;
  if (jump > UINT16_MAX) {
    error("Too much code to jump over.");
  }
  currentChunk()->code[offset] = (jump >> 8) & 0xff;
  currentChunk()->code[offset + 1] = jump & 0xff;
}

static void emitLoop(int loopStart) {
  emitByte(OP_LOOP);
  int offset = currentChunk()->count - loopStart + 2;
  if (offset > UINT16_MAX) error("Loop body too large.");
  emitByte((offset >> 8) & 0xff);
  emitByte(offset & 0xff);
}

static void initCompiler(Compiler* compiler, FunctionType type) {
  compiler->enclosing = current;
  compiler->function = NULL;
  compiler->type = type;
  compiler->localCount = 0;
  compiler->scopeDepth = 0;
  compiler->currentLoop = NULL;
  compiler->locals = (Local*)malloc(sizeof(Local) * LOCAL_MAX);
  compiler->localMap = (LocalMapEntry*)malloc(sizeof(LocalMapEntry) * LOCAL_MAP_SIZE);
  for (int i = 0; i < LOCAL_MAP_SIZE; i++) {
    compiler->localMap[i].occupied = false;
    compiler->localMap[i].local = -1;
  }
  compiler->function = newFunction();
  current = compiler;

  if (type != TYPE_SCRIPT) 
    current->function->name = copyString(parser.previous.start, parser.previous.length);

  Local* local = &current->locals[current->localCount++];
  local->depth = 0;
  local->name.start = "";
  local->name.length = 0;
  local->previous = -1;
  local->isMutable = false;
  local->isCaptured = false;
}

static ObjFunction* endCompiler() {
  emitReturn();
  ObjFunction* function = current->function;

#ifdef DEBUG_PRINT_CODE
  if (!parser.hadError) {
    disassembleChunk(currentChunk(), function->name != NULL ? function->name->chars : "<script>");
  }
#endif

  free(current->locals);
  free(current->localMap);
  current = current->enclosing;
  return function;
}

static void beginScope() {
  current->scopeDepth++;
}

static void localMapSet(Token* name, int local);

static void endScope() {
  current->scopeDepth--;

  while (current->localCount > 0 && current->locals[current->localCount - 1].depth > current->scopeDepth) {
    Local* local = &current->locals[current->localCount - 1];
    if (local->isCaptured) {
      emitByte(OP_CLOSE_UPVALUE);
    } else {
      emitByte(OP_POP);
    }
    localMapSet(&local->name, local->previous);
    current->localCount--;
  }
}

static void expression(void);
static void block(void);
static void statement(void);
static void declaration(void);
static void varDeclaration(void);
static void valDeclaration(void);
static void and_(bool canAssign);
static void or_(bool canAssign);
static ParseRule* getRule(TokenType type);
static void parsePrecedence(Precedence precedence);
static void addLocal(Token name, int previousLocal, bool isMutable);
static void declareVariable(bool isMutable);
static int resolveLocal(Compiler* compiler, Token* name);
static int localMapGet(Token* name);
static void localMapSet(Token* name, int local);
static int findGlobalSlot(Token* name);

static uint8_t identifierGlobalSlot(Token* name) {
  int existing = findGlobalSlot(name);
  if (existing != -1) return (uint8_t)existing;

  if (vm.globalCount >= GLOBAL_MAX) {
    error("Too many global variables.");
    return 0;
  }

  int slot = vm.globalCount++;
  vm.globals[slot].name = copyString(name->start, name->length);
  vm.globals[slot].defined = false;
  vm.globals[slot].isMutable = true;
  vm.globals[slot].value = NIL_VAL;
  return (uint8_t)slot;
}

static int findGlobalSlot(Token* name) {
  for (int i = 0; i < vm.globalCount; i++) {
    ObjString* existing = vm.globals[i].name;
    if (existing->length == name->length && memcmp(existing->chars, name->start, name->length) == 0) {
      return i;
    }
  }
  return -1;
}

static uint8_t parseVariable(const char* errorMessage, bool isMutable) {
  consume(TOKEN_IDENTIFIER, errorMessage);
  declareVariable(isMutable);
  if (current->scopeDepth > 0) return 0;

  int existing = findGlobalSlot(&parser.previous);
  uint8_t global = identifierGlobalSlot(&parser.previous);
  if (existing == -1) {
    vm.globals[global].isMutable = isMutable;
  } else if (!isMutable) {
    vm.globals[global].isMutable = false;
  }

  return global;
}

static void markInitialized() {
  if (current->scopeDepth == 0) return;
  current->locals[current->localCount - 1].depth = current->scopeDepth;
}

static void defineVariable(uint8_t global) {
  if (current->scopeDepth > 0) {
    markInitialized();
    return;
  }

  emitBytes(OP_DEFINE_GLOBAL, global);
}

static uint8_t argumentList() {
  uint8_t argCount = 0;
  if (!check(TOKEN_RIGHT_PAREN)) {
    do {
      expression();
      if (argCount == 255) {
        error("Can't have more than 255 arguments.");
      }
      argCount++;
    } while (match(TOKEN_COMMA));
  }
  consume(TOKEN_RIGHT_PAREN, "Expect ')' after arguments.");
  return argCount;
}

static uint32_t hashToken(Token* token) {
  uint32_t hash = 2166136261u;
  for (int i = 0; i < token->length; i++) {
    hash ^= (uint8_t)token->start[i];
    hash *= 16777619;
  }
  return hash;
}

static int localMapFindSlot(Token* name) {
  uint32_t index = hashToken(name) % LOCAL_MAP_SIZE;
  for (;;) {
    LocalMapEntry* entry = &current->localMap[index];
    if (!entry->occupied) return (int)index;

    if (entry->length == name->length && memcmp(entry->start, name->start, name->length) == 0) {
      return (int)index;
    }

    index = (index + 1) % LOCAL_MAP_SIZE;
  }
}

static int localMapGet(Token* name) {
  int slot = localMapFindSlot(name);
  LocalMapEntry* entry = &current->localMap[slot];
  if (!entry->occupied) return -1;
  return entry->local;
}

static void localMapSet(Token* name, int local) {
  int slot = localMapFindSlot(name);
  LocalMapEntry* entry = &current->localMap[slot];
  if (!entry->occupied) {
    entry->occupied = true;
    entry->start = name->start;
    entry->length = name->length;
  }
  entry->local = local;
}

static void addLocal(Token name, int previousLocal, bool isMutable) {
  if (current->localCount == LOCAL_MAX) {
    error("Too many local variables in function.");
    return;
  }

  int localIndex = current->localCount++;
  Local* local = &current->locals[localIndex];
  local->name = name;
  local->depth = -1;
  local->previous = previousLocal;
  local->isMutable = isMutable;
  local->isCaptured = false;
  localMapSet(&name, localIndex);
}

static void declareVariable(bool isMutable) {
  if (current->scopeDepth == 0) return;

  Token* name = &parser.previous;
  int existing = localMapGet(name);
  if (existing != -1) {
    Local* local = &current->locals[existing];
    if (local->depth == -1 || local->depth == current->scopeDepth) {
      error("Already a variable with this name in this scope.");
    }
  }

  addLocal(*name, existing, isMutable);
}

static int resolveLocal(Compiler* compiler, Token* name) {
  uint32_t index = hashToken(name) % LOCAL_MAP_SIZE;
  for (;;) {
    LocalMapEntry* entry = &compiler->localMap[index];
    if (!entry->occupied) return -1;
    if (entry->length == name->length &&
        memcmp(entry->start, name->start, name->length) == 0) {
      Local* local = &compiler->locals[entry->local];
      if (compiler == current && local->depth == -1) {
        error("Can't read local variable in its own initializer.");
      }
      return entry->local;
    }
    index = (index + 1) % LOCAL_MAP_SIZE;
  }
}

static int addUpvalue(Compiler* compiler, uint8_t index, bool isLocal) {
  int upvalueCount = compiler->function->upvalueCount;

  for (int i = 0; i < upvalueCount; i++) {
    Upvalue* upvalue = &compiler->upvalues[i];
    if (upvalue->index == index && upvalue->isLocal == isLocal) {
      return i;
    }
  }

  if (upvalueCount == UINT8_COUNT) {
    error("Too many closure variables in function.");
    return 0;
  }

  compiler->upvalues[upvalueCount].isLocal = isLocal;
  compiler->upvalues[upvalueCount].index = index;
  return compiler->function->upvalueCount++;
}

static int resolveUpvalue(Compiler* compiler, Token* name) {
  if (compiler->enclosing == NULL) return -1;

  int local = resolveLocal(compiler->enclosing, name);
  if (local != -1) {
    if (local > UINT8_MAX) {
      error("Captured variable slot exceeds 255.");
      return -1;
    }
    compiler->enclosing->locals[local].isCaptured = true;
    return addUpvalue(compiler, (uint8_t)local, true);
  }

  int upvalue = resolveUpvalue(compiler->enclosing, name);
  if (upvalue != -1) {
    return addUpvalue(compiler, (uint8_t)upvalue, false);
  }

  return -1;
}

static void and_(bool canAssign) {
  (void)canAssign;
  int endJump = emitJump(OP_JUMP_IF_FALSE);
  emitByte(OP_POP);
  parsePrecedence(PREC_AND);
  patchJump(endJump);
}

static void or_(bool canAssign) {
  (void)canAssign;
  int elseJump = emitJump(OP_JUMP_IF_FALSE);
  int endJump = emitJump(OP_JUMP);
  patchJump(elseJump);
  emitByte(OP_POP);
  parsePrecedence(PREC_OR);
  patchJump(endJump);
}

static void binary(bool canAssign) {
  (void)canAssign;
  TokenType operatorType = parser.previous.type;
  ParseRule* rule = getRule(operatorType);
  parsePrecedence((Precedence)(rule->precedence + 1));

  switch (operatorType) {
    case TOKEN_BANG_EQUAL:    emitBytes(OP_EQUAL, OP_NOT); break;
    case TOKEN_EQUAL_EQUAL:   emitByte(OP_EQUAL); break;
    case TOKEN_GREATER:       emitByte(OP_GREATER); break;
    case TOKEN_GREATER_EQUAL: emitBytes(OP_LESS, OP_NOT); break;
    case TOKEN_LESS:          emitByte(OP_LESS); break;
    case TOKEN_LESS_EQUAL:    emitBytes(OP_GREATER, OP_NOT); break;
    case TOKEN_PLUS:          emitByte(OP_ADD); break;
    case TOKEN_MINUS:         emitByte(OP_SUBTRACT); break;
    case TOKEN_STAR:          emitByte(OP_MULTIPLY); break;
    case TOKEN_SLASH:         emitByte(OP_DIVIDE); break;
    default: return;
  }
}

static void call(bool canAssign) {
  (void)canAssign;
  uint8_t argCount = argumentList();
  emitBytes(OP_CALL, argCount);
}

static void literal(bool canAssign) {
  (void)canAssign;
  switch (parser.previous.type) {
    case TOKEN_FALSE: emitByte(OP_FALSE); break;
    case TOKEN_NIL: emitByte(OP_NIL); break;
    case TOKEN_TRUE: emitByte(OP_TRUE); break;
    default: return;
  }
}

static void grouping(bool canAssign) {
  (void)canAssign;
  expression();
  consume(TOKEN_RIGHT_PAREN, "Expect ')' after expression.");
}

static void number(bool canAssign) {
  (void)canAssign;
  double value = strtod(parser.previous.start, NULL);
  emitConstant(NUMBER_VAL(value));
}

static void string(bool canAssign) {
  (void)canAssign;
  emitConstant(OBJ_VAL(copyString(parser.previous.start + 1, parser.previous.length - 2)));
}

static void namedVariable(Token name, bool canAssign) {
  enum { VAR_LOCAL, VAR_UPVALUE, VAR_GLOBAL } kind;
  int arg = resolveLocal(current, &name);
  if (arg != -1) {
    kind = VAR_LOCAL;
  } else if ((arg = resolveUpvalue(current, &name)) != -1) {
    kind = VAR_UPVALUE;
  } else {
    arg = identifierGlobalSlot(&name);
    kind = VAR_GLOBAL;
  }

  if (canAssign && match(TOKEN_EQUAL)) {
    expression();
    if (kind == VAR_LOCAL && !current->locals[arg].isMutable) {
      error("Can't assign to immutable variable.");
    } else if (kind == VAR_GLOBAL && !vm.globals[arg].isMutable) {
      error("Can't assign to immutable variable.");
    } else if (kind == VAR_LOCAL) {
      emitLocalOp(OP_SET_LOCAL, OP_SET_LOCAL_LONG, arg);
    } else if (kind == VAR_UPVALUE) {
      emitBytes(OP_SET_UPVALUE, (uint8_t)arg);
    } else {
      emitBytes(OP_SET_GLOBAL, (uint8_t)arg);
    }
  } else if (kind == VAR_LOCAL) {
    emitLocalOp(OP_GET_LOCAL, OP_GET_LOCAL_LONG, arg);
  } else if (kind == VAR_UPVALUE) {
    emitBytes(OP_GET_UPVALUE, (uint8_t)arg);
  } else {
    emitBytes(OP_GET_GLOBAL, (uint8_t)arg);
  }
}

static void variable(bool canAssign) {
  namedVariable(parser.previous, canAssign);
}

static void unary(bool canAssign) {
  (void)canAssign;
  TokenType operatorType = parser.previous.type;

  parsePrecedence(PREC_UNARY);

  switch (operatorType) {
    case TOKEN_BANG: emitByte(OP_NOT); break;
    case TOKEN_MINUS: emitByte(OP_NEGATE); break;
    default: return;
  }
}

static void ternary(bool canAssign) {
  (void)canAssign;
  parsePrecedence(PREC_TERNARY);
  consume(TOKEN_COLON, "Expect ':' in ternary expression.");
  parsePrecedence(PREC_TERNARY);
}

ParseRule rules[] = {
  [TOKEN_LEFT_PAREN]    = {grouping, call,     PREC_CALL},
  [TOKEN_RIGHT_PAREN]   = {NULL,     NULL,     PREC_NONE},
  [TOKEN_LEFT_BRACE]    = {NULL,     NULL,     PREC_NONE},
  [TOKEN_RIGHT_BRACE]   = {NULL,     NULL,     PREC_NONE},
  [TOKEN_COMMA]         = {NULL,     NULL,     PREC_NONE},
  [TOKEN_DOT]           = {NULL,     NULL,     PREC_NONE},
  [TOKEN_MINUS]         = {unary,    binary,   PREC_TERM},
  [TOKEN_PLUS]          = {NULL,     binary,   PREC_TERM},
  [TOKEN_SEMICOLON]     = {NULL,     NULL,     PREC_NONE},
  [TOKEN_SLASH]         = {NULL,     binary,   PREC_FACTOR},
  [TOKEN_STAR]          = {NULL,     binary,   PREC_FACTOR},
  [TOKEN_QUESTION]      = {NULL,     ternary,  PREC_TERNARY},
  [TOKEN_BANG]          = {unary,    NULL,     PREC_NONE},
  [TOKEN_BANG_EQUAL]    = {NULL,     binary,   PREC_EQUALITY},
  [TOKEN_EQUAL]         = {NULL,     NULL,     PREC_NONE},
  [TOKEN_EQUAL_EQUAL]   = {NULL,     binary,   PREC_EQUALITY},
  [TOKEN_GREATER]       = {NULL,     binary,   PREC_COMPARISON},
  [TOKEN_GREATER_EQUAL] = {NULL,     binary,   PREC_COMPARISON},
  [TOKEN_LESS]          = {NULL,     binary,   PREC_COMPARISON},
  [TOKEN_LESS_EQUAL]    = {NULL,     binary,   PREC_COMPARISON},
  [TOKEN_IDENTIFIER]    = {variable, NULL,     PREC_NONE},
  [TOKEN_STRING]        = {string,   NULL,     PREC_NONE},
  [TOKEN_NUMBER]        = {number,   NULL,     PREC_NONE},
  [TOKEN_AND]           = {NULL,     and_,     PREC_AND},
  [TOKEN_CASE]          = {NULL,     NULL,     PREC_NONE},
  [TOKEN_CLASS]         = {NULL,     NULL,     PREC_NONE},
  [TOKEN_CONTINUE]      = {NULL,     NULL,     PREC_NONE},
  [TOKEN_DEFAULT]       = {NULL,     NULL,     PREC_NONE},
  [TOKEN_ELSE]          = {NULL,     NULL,     PREC_NONE},
  [TOKEN_FALSE]         = {literal,  NULL,     PREC_NONE},
  [TOKEN_FOR]           = {NULL,     NULL,     PREC_NONE},
  [TOKEN_FUN]           = {NULL,     NULL,     PREC_NONE},
  [TOKEN_IF]            = {NULL,     NULL,     PREC_NONE},
  [TOKEN_NIL]           = {literal,  NULL,     PREC_NONE},
  [TOKEN_OR]            = {NULL,     or_,      PREC_OR},
  [TOKEN_PRINT]         = {NULL,     NULL,     PREC_NONE},
  [TOKEN_RETURN]        = {NULL,     NULL,     PREC_NONE},
  [TOKEN_SUPER]         = {NULL,     NULL,     PREC_NONE},
  [TOKEN_SWITCH]        = {NULL,     NULL,     PREC_NONE},
  [TOKEN_THIS]          = {NULL,     NULL,     PREC_NONE},
  [TOKEN_TRUE]          = {literal,  NULL,     PREC_NONE},
  [TOKEN_VAL]           = {NULL,     NULL,     PREC_NONE},
  [TOKEN_VAR]           = {NULL,     NULL,     PREC_NONE},
  [TOKEN_WHILE]         = {NULL,     NULL,     PREC_NONE},
  [TOKEN_ERROR]         = {NULL,     NULL,     PREC_NONE},
  [TOKEN_EOF]           = {NULL,     NULL,     PREC_NONE},
};

static void parsePrecedence(Precedence precedence) {
  advance();
  ParseFn prefixRule = getRule(parser.previous.type)->prefix;
  if (prefixRule == NULL) {
    error("Expect expression.");
    return;
  }

  bool canAssign = precedence <= PREC_ASSIGNMENT;
  prefixRule(canAssign);

  while (precedence <= getRule(parser.current.type)->precedence) {
    advance();
    ParseFn infixRule = getRule(parser.previous.type)->infix;
    infixRule(canAssign);
  }

  if (canAssign && match(TOKEN_EQUAL)) {
    error("Invalid assignment target.");
  }
}

static ParseRule* getRule(TokenType type) {
  return &rules[type];
}

static void expression() {
  parsePrecedence(PREC_ASSIGNMENT);
}

static void block() {
  while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
    declaration();
  }

  consume(TOKEN_RIGHT_BRACE, "Expect '}' after block.");
}

static void function(FunctionType type) {
  Compiler compiler;
  initCompiler(&compiler, type);
  beginScope();

  consume(TOKEN_LEFT_PAREN, "Expect '(' after function name.");
  if (!check(TOKEN_RIGHT_PAREN)) {
    do {
      current->function->arity++;
      if (current->function->arity > 255) {
        errorAtCurrent("Can't have more than 255 parameters.");
      }
      uint8_t constant = parseVariable("Expect parameter name.", true);
      defineVariable(constant);
    } while (match(TOKEN_COMMA));
  }
  consume(TOKEN_RIGHT_PAREN, "Expect ')' after parameters.");
  consume(TOKEN_LEFT_BRACE, "Expect '{' before function body.");
  block();

  ObjFunction* func = endCompiler();
  emitBytes(OP_CLOSURE, makeConstant(OBJ_VAL(func)));
  for (int i = 0; i < func->upvalueCount; i++) {
    emitByte(compiler.upvalues[i].isLocal ? 1 : 0);
    emitByte(compiler.upvalues[i].index);
  }
}

static void funDeclaration() {
  uint8_t global = parseVariable("Expect function name.", true);
  markInitialized();
  function(TYPE_FUNCTION);
  defineVariable(global);
}

static void varDeclaration() {
  uint8_t global = parseVariable("Expect variable name.", true);

  if (match(TOKEN_EQUAL)) {
    expression();
  } else {
    emitByte(OP_NIL);
  }
  consume(TOKEN_SEMICOLON, "Expect ';' after variable declaration.");

  defineVariable(global);
}

static void valDeclaration() {
  uint8_t global = parseVariable("Expect variable name.", false);

  consume(TOKEN_EQUAL, "Expect '=' after immutable variable declaration.");
  expression();
  consume(TOKEN_SEMICOLON, "Expect ';' after immutable variable declaration.");

  defineVariable(global);
}

static void expressionStatement() {
  expression();
  consume(TOKEN_SEMICOLON, "Expect ';' after expression.");
  emitByte(OP_POP);
}

static void printStatement() {
  expression();
  consume(TOKEN_SEMICOLON, "Expect ';' after value.");
  emitByte(OP_PRINT);
}

static void returnStatement() {
  if (current->type == TYPE_SCRIPT) {
    error("Can't return from top-level code.");
  }

  if (match(TOKEN_SEMICOLON)) {
    emitReturn();
  } else {
    expression();
    consume(TOKEN_SEMICOLON, "Expect ';' after return value.");
    emitByte(OP_RETURN);
  }
}

static void synchronize() {
  parser.panicMode = false;

  while (parser.current.type != TOKEN_EOF) {
    if (parser.previous.type == TOKEN_SEMICOLON) return;
    switch (parser.current.type) {
      case TOKEN_CLASS:
      case TOKEN_FUN:
      case TOKEN_VAR:
      case TOKEN_FOR:
      case TOKEN_IF:
      case TOKEN_WHILE:
      case TOKEN_PRINT:
      case TOKEN_RETURN:
        return;

      default:
        ; // Do nothing.
    }

    advance();
  }
}

static void declaration() {
  if (match(TOKEN_FUN)) {
    funDeclaration();
  } else if (match(TOKEN_VAL)) {
    valDeclaration();
  } else if (match(TOKEN_VAR)) {
    varDeclaration();
  } else {
    statement();
  }

  if (parser.panicMode) synchronize();
}

static void ifStatement() {
  consume(TOKEN_LEFT_PAREN, "Expect '(' after 'if'.");
  expression();
  consume(TOKEN_RIGHT_PAREN, "Expect ')' after condition.");

  int thenJump = emitJump(OP_JUMP_IF_FALSE);
  emitByte(OP_POP);
  statement();

  int elseJump = emitJump(OP_JUMP);

  patchJump(thenJump);
  emitByte(OP_POP);

  if (match(TOKEN_ELSE)) statement();
  patchJump(elseJump);
}

static void whileStatement() {
  int loopStart = currentChunk()->count;

  consume(TOKEN_LEFT_PAREN, "Expect '(' after 'while'.");
  expression();
  consume(TOKEN_RIGHT_PAREN, "Expect ')' after condition.");

  int exitJump = emitJump(OP_JUMP_IF_FALSE);
  emitByte(OP_POP);

  Loop loop;
  loop.start = loopStart;
  loop.scopeDepth = current->scopeDepth;
  loop.enclosing = current->currentLoop;
  current->currentLoop = &loop;

  statement();

  current->currentLoop = loop.enclosing;

  emitLoop(loopStart);

  patchJump(exitJump);
  emitByte(OP_POP);
}

static void forStatement() {
  beginScope();
  consume(TOKEN_LEFT_PAREN, "Expect '(' after 'for'.");

  // Challenge 2: track the loop variable so each iteration gets a fresh
  // binding. Closures created in the body capture per-iteration values.
  int loopVarSlot = -1;
  bool loopVarMutable = false;

  if (match(TOKEN_SEMICOLON)) {
    // No initializer.
  } else if (match(TOKEN_VAR)) {
    varDeclaration();
    loopVarSlot = current->localCount - 1;
    loopVarMutable = true;
  } else if (match(TOKEN_VAL)) {
    valDeclaration();
    loopVarSlot = current->localCount - 1;
    loopVarMutable = false;
  } else {
    expressionStatement();
  }

  int loopStart = currentChunk()->count;
  int exitJump = -1;
  if (!match(TOKEN_SEMICOLON)) {
    expression();
    consume(TOKEN_SEMICOLON, "Expect ';' after loop condition.");

    exitJump = emitJump(OP_JUMP_IF_FALSE);
    emitByte(OP_POP);
  }

  if (!match(TOKEN_RIGHT_PAREN)) {
    int bodyJump = emitJump(OP_JUMP);
    int incrementStart = currentChunk()->count;
    expression();
    emitByte(OP_POP);
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after for clauses.");

    emitLoop(loopStart);
    loopStart = incrementStart;
    patchJump(bodyJump);
  }

  Loop loop;
  loop.start = loopStart;
  loop.scopeDepth = current->scopeDepth;
  loop.enclosing = current->currentLoop;
  current->currentLoop = &loop;

  if (loopVarSlot != -1) {
    Token loopVarName = current->locals[loopVarSlot].name;
    beginScope();
    emitLocalOp(OP_GET_LOCAL, OP_GET_LOCAL_LONG, loopVarSlot);
    int previous = localMapGet(&loopVarName);
    addLocal(loopVarName, previous, loopVarMutable);
    markInitialized();
  }

  statement();

  if (loopVarSlot != -1) {
    endScope();
  }

  current->currentLoop = loop.enclosing;

  emitLoop(loopStart);

  if (exitJump != -1) {
    patchJump(exitJump);
    emitByte(OP_POP);
  }

  endScope();
}

static void switchStatement() {
  consume(TOKEN_LEFT_PAREN, "Expect '(' after 'switch'.");
  expression();
  consume(TOKEN_RIGHT_PAREN, "Expect ')' after switch value.");
  consume(TOKEN_LEFT_BRACE, "Expect '{' before switch cases.");

  int endJumps[UINT8_COUNT];
  int endJumpCount = 0;
  int previousCaseSkip = -1;
  bool hadDefault = false;

  while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
    if (match(TOKEN_CASE)) {
      if (hadDefault) {
        error("Can't have a case after the default branch.");
      }
      if (previousCaseSkip != -1) {
        patchJump(previousCaseSkip);
        emitByte(OP_POP);
      }

      emitByte(OP_DUP);
      expression();
      consume(TOKEN_COLON, "Expect ':' after case value.");
      emitByte(OP_EQUAL);

      previousCaseSkip = emitJump(OP_JUMP_IF_FALSE);
      emitByte(OP_POP);

      while (!check(TOKEN_CASE) && !check(TOKEN_DEFAULT) && !check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
        statement();
      }

      if (endJumpCount >= UINT8_COUNT) {
        error("Too many cases in switch.");
      } else {
        endJumps[endJumpCount++] = emitJump(OP_JUMP);
      }
    } else if (match(TOKEN_DEFAULT)) {
      if (hadDefault) {
        error("Can't have more than one default branch.");
      }
      hadDefault = true;

      if (previousCaseSkip != -1) {
        patchJump(previousCaseSkip);
        emitByte(OP_POP);
        previousCaseSkip = -1;
      }

      consume(TOKEN_COLON, "Expect ':' after 'default'.");

      while (!check(TOKEN_CASE) && !check(TOKEN_DEFAULT) && !check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
        statement();
      }
    } else {
      errorAtCurrent("Expect 'case' or 'default'.");
      advance();
    }
  }

  consume(TOKEN_RIGHT_BRACE, "Expect '}' after switch cases.");

  if (previousCaseSkip != -1) {
    patchJump(previousCaseSkip);
    emitByte(OP_POP);
  }

  for (int i = 0; i < endJumpCount; i++) {
    patchJump(endJumps[i]);
  }

  emitByte(OP_POP);
}

static void continueStatement() {
  if (current->currentLoop == NULL) {
    error("Can't use 'continue' outside of a loop.");
    consume(TOKEN_SEMICOLON, "Expect ';' after 'continue'.");
    return;
  }
  consume(TOKEN_SEMICOLON, "Expect ';' after 'continue'.");

  for (int i = current->localCount - 1; i >= 0 && current->locals[i].depth > current->currentLoop->scopeDepth; i--) {
    if (current->locals[i].isCaptured) {
      emitByte(OP_CLOSE_UPVALUE);
    } else {
      emitByte(OP_POP);
    }
  }

  emitLoop(current->currentLoop->start);
}

static void statement() {
  if (match(TOKEN_PRINT)) {
    printStatement();
  } else if (match(TOKEN_RETURN)) {
    returnStatement();
  } else if (match(TOKEN_IF)) {
    ifStatement();
  } else if (match(TOKEN_WHILE)) {
    whileStatement();
  } else if (match(TOKEN_FOR)) {
    forStatement();
  } else if (match(TOKEN_SWITCH)) {
    switchStatement();
  } else if (match(TOKEN_CONTINUE)) {
    continueStatement();
  } else if (match(TOKEN_LEFT_BRACE)) {
    beginScope();
    block();
    endScope();
  } else {
    expressionStatement();
  }
}

ObjFunction* compile(const char* source) {
  initScanner(source);
  Compiler compiler;
  initCompiler(&compiler, TYPE_SCRIPT);

  parser.hadError = false;
  parser.panicMode = false;

  advance();

  while (!match(TOKEN_EOF)) {
    declaration();
  }

  ObjFunction* function = endCompiler();
  return parser.hadError ? NULL : function;
}
