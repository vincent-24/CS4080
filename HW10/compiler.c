#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "compiler.h"
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
} Local;

typedef struct {
  const char* start;
  int length;
  int local;
  bool occupied;
} LocalMapEntry;

#define LOCAL_MAP_SIZE (UINT8_COUNT * 2)

typedef struct {
  Local locals[UINT8_COUNT];
  int localCount;
  int scopeDepth;
  LocalMapEntry localMap[LOCAL_MAP_SIZE];
} Compiler;

Parser parser;
Compiler* current = NULL;
Chunk* compilingChunk;

static Chunk* currentChunk() {
  return compilingChunk;
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

static void initCompiler(Compiler* compiler) {
  compiler->localCount = 0;
  compiler->scopeDepth = 0;
  for (int i = 0; i < LOCAL_MAP_SIZE; i++) {
    compiler->localMap[i].occupied = false;
    compiler->localMap[i].local = -1;
  }
  current = compiler;
}

static void endCompiler() {
  emitReturn();
#ifdef DEBUG_PRINT_CODE
  if (!parser.hadError) {
    disassembleChunk(currentChunk(), "code");
  }
#endif
}

static void beginScope() {
  current->scopeDepth++;
}

static void localMapSet(Token* name, int local);

static void endScope() {
  current->scopeDepth--;

  while (current->localCount > 0 && current->locals[current->localCount - 1].depth > current->scopeDepth) {
    Local* local = &current->locals[current->localCount - 1];
    localMapSet(&local->name, local->previous);
    emitByte(OP_POP);
    current->localCount--;
  }
}

static void expression();
static void block();
static void statement();
static void declaration();
static void varDeclaration();
static void valDeclaration();
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
    if (existing->length == name->length &&
        memcmp(existing->chars, name->start, name->length) == 0) {
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
  current->locals[current->localCount - 1].depth = current->scopeDepth;
}

static void defineVariable(uint8_t global) {
  if (current->scopeDepth > 0) {
    markInitialized();
    return;
  }

  emitBytes(OP_DEFINE_GLOBAL, global);
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

    if (entry->length == name->length &&
        memcmp(entry->start, name->start, name->length) == 0) {
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
  if (current->localCount == UINT8_COUNT) {
    error("Too many local variables in function.");
    return;
  }

  int localIndex = current->localCount++;
  Local* local = &current->locals[localIndex];
  local->name = name;
  local->depth = -1;
  local->previous = previousLocal;
  local->isMutable = isMutable;
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
  (void)compiler;
  int localIndex = localMapGet(name);
  if (localIndex != -1) {
    Local* local = &current->locals[localIndex];
    if (local->depth == -1) {
      error("Can't read local variable in its own initializer.");
    }
    return localIndex;
  }

  return -1;
}

static void binary(bool canAssign) {
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

static void literal(bool canAssign) {
  switch (parser.previous.type) {
    case TOKEN_FALSE: emitByte(OP_FALSE); break;
    case TOKEN_NIL: emitByte(OP_NIL); break;
    case TOKEN_TRUE: emitByte(OP_TRUE); break;
    default: return; 
  }
}

static void grouping(bool canAssign) {
  expression();
  consume(TOKEN_RIGHT_PAREN, "Expect ')' after expression.");
}

static void number(bool canAssign) {
  double value = strtod(parser.previous.start, NULL);
  emitConstant(NUMBER_VAL(value));
}

static void string(bool canAssign) {
  emitConstant(OBJ_VAL(copyString(parser.previous.start + 1, parser.previous.length - 2)));
}

static void namedVariable(Token name, bool canAssign) {
  uint8_t getOp, setOp;
  int arg = resolveLocal(current, &name);
  bool isLocal = arg != -1;
  if (arg != -1) {
    getOp = OP_GET_LOCAL;
    setOp = OP_SET_LOCAL;
  } else {
    arg = identifierGlobalSlot(&name);
    getOp = OP_GET_GLOBAL;
    setOp = OP_SET_GLOBAL;
  }

  if (canAssign && match(TOKEN_EQUAL)) {
    expression();
    if (isLocal && !current->locals[arg].isMutable) {
      error("Can't assign to immutable variable.");
    } else if (!isLocal && !vm.globals[arg].isMutable) {
      error("Can't assign to immutable variable.");
    } else {
      emitBytes(setOp, (uint8_t)arg);
    }
  } else {
    emitBytes(getOp, (uint8_t)arg);
  }
}

static void variable(bool canAssign) {
  namedVariable(parser.previous, canAssign);
}

static void unary(bool canAssign) {
  TokenType operatorType = parser.previous.type;

  parsePrecedence(PREC_UNARY);

  switch (operatorType) {
    case TOKEN_BANG: emitByte(OP_NOT); break;
    case TOKEN_MINUS: emitByte(OP_NEGATE); break;
    default: return;
  }
}

static void ternary(bool canAssign) {
  parsePrecedence(PREC_TERNARY);
  consume(TOKEN_COLON, "Expect ':' in ternary expression.");
  parsePrecedence(PREC_TERNARY);
}

ParseRule rules[] = {
  [TOKEN_LEFT_PAREN]    = {grouping, NULL,     PREC_NONE},
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
  [TOKEN_AND]           = {NULL,     NULL,     PREC_NONE},
  [TOKEN_CLASS]         = {NULL,     NULL,     PREC_NONE},
  [TOKEN_ELSE]          = {NULL,     NULL,     PREC_NONE},
  [TOKEN_FALSE]         = {literal,  NULL,     PREC_NONE},
  [TOKEN_FOR]           = {NULL,     NULL,     PREC_NONE},
  [TOKEN_FUN]           = {NULL,     NULL,     PREC_NONE},
  [TOKEN_IF]            = {NULL,     NULL,     PREC_NONE},
  [TOKEN_NIL]           = {literal,  NULL,     PREC_NONE},
  [TOKEN_OR]            = {NULL,     NULL,     PREC_NONE},
  [TOKEN_PRINT]         = {NULL,     NULL,     PREC_NONE},
  [TOKEN_RETURN]        = {NULL,     NULL,     PREC_NONE},
  [TOKEN_SUPER]         = {NULL,     NULL,     PREC_NONE},
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
  if (match(TOKEN_VAL)) {
    valDeclaration();
  } else if (match(TOKEN_VAR)) {
    varDeclaration();
  } else {
    statement();
  }

  if (parser.panicMode) synchronize();
}

static void statement() {
  if (match(TOKEN_PRINT)) {
    printStatement();
  } else if (match(TOKEN_LEFT_BRACE)) {
    beginScope();
    block();
    endScope();
  } else {
    expressionStatement();
  }
}

bool compile(const char* source, Chunk* chunk) {
  initScanner(source);
  Compiler compiler;
  initCompiler(&compiler);
  compilingChunk = chunk;

  parser.hadError = false;
  parser.panicMode = false;

  advance();

  while (!match(TOKEN_EOF)) {
    declaration();
  }

  endCompiler();
  return !parser.hadError;
}
