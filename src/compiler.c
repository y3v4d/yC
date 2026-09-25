#include "compiler.h"
#include "debug.h"
#include "lexer.h"

#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    token_t current;
    token_t previous;

    bool had_error;
} parser_t;

// 32-bit layout for type
// [0..5] - enum type
// [6] - pointer flag
// [7] - absolute flag - frame_index is absolute instead of relative to current
// frame pointer
// [8-31] - data (TYPE_STRUCT - index, other - unspecified)

typedef enum {
    TYPE_UNKNOWN = 0,
    TYPE_VOID,
    TYPE_BOOL,
    TYPE_I32,
    TYPE_I64,
    TYPE_F32,
    TYPE_F64,
    TYPE_CHAR,
    TYPE_STRUCT,
    TYPE_FUN,

    TYPE_POINTER_FLAG = 0b01000000,
    TYPE_ABSOLUTE_FLAG = 0b10000000,

    TYPE_BASE_MASK = 0b00111111,
    TYPE_DATA_MASK = 0xFFFFFF00,

    TYPE_MAX_VALUE = INT_MAX // 32-bit integer max value
} type_e;

typedef enum {
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_ADDRESS_VAR,
    OP_LOAD,
    OP_STORE,
    OP_LOAD_VAR,
    OP_STORE_VAR,
    OP_COPY_TEMP,
    OP_CONST,
    OP_CONVERT,
    OP_CALL,
    OP_IF,
    OP_ELSE,
    OP_END,
    OP_PRINT,
    OP_LOOP,
    OP_BLOCK,
    OP_JMP,
    OP_JMPIF,
    OP_NEGATE
} optype_e;

typedef struct {
    optype_e type;
    type_e value_type;

    int offset;
    bool should_load;

    union {
        struct {
            type_e from;
            type_e to;
        } convert;

        int temp_offset;
        int const_value;
        int var_index;
        int label_index;
    } as;
} op_t;

typedef struct {
    token_t name;
    type_e type;

    bool is_on_stack;
    bool is_param;

    int frame_index;
    int depth;
} local_t;

typedef struct {
    token_t name;
    type_e type;
} fielddef_t;

typedef struct {
    token_t name;

    fielddef_t *fields;
    int field_count;
    int field_capacity;
} structdef_t;

typedef struct {
    token_t name;

    fielddef_t *params;
    int param_count;

    type_e return_type;
} fundef_t;

typedef enum {
    VTYPE_UNKNOWN = 0,
    VTYPE_CONST = 0b0001,
    VTYPE_LOCAL = 0b0010,
    VTYPE_STACK = 0b0100,
    VTYPE_LVALUE = 0b1000,

    VTYPE_STORAGE = VTYPE_CONST | VTYPE_LOCAL | VTYPE_STACK
} stackvtype_e;

typedef struct {
    stackvtype_e type;
    type_e ctype;

    int offset;

    union {
        int const_value;
        local_t *local;
    } as;
} stackv_t;

typedef struct {
    type_e type;
    bool is_local;
    int offset;
    int size;

    union {
        int const_value;
        const char *string;
        local_t *local;
    } as;
} const_t;

typedef struct {
    op_t *ops;
    int op_count;
    int op_capcity;

    char *buffer;
    char *current;

    int temp_max;

    stackv_t stack[256];
    int stack_count;

    local_t locals[256];
    int local_count;

    structdef_t structdefs[256];
    int structdef_count;

    fundef_t fundefs[256];
    int fundef_count;

    const_t consts[256];
    int const_count;

    int data_offset;

    int scope_depth;
    int loop_counter;
    bool no_op;
} compiler_t;

typedef enum {
    PREC_NONE,
    PREC_ASSIGNMENT, // =
    PREC_TERM,       // + -
    PREC_FACTOR,     // * /
    PREC_UNARY,      // - & (unary)
    PREC_ACCESS,     // . (field access)
    PREC_CALL,       // ()
} prec_e;

typedef void (*parsefn_t)();

typedef struct {
    parsefn_t prefix;
    parsefn_t infix;
    prec_e precedence;
} parserule_t;

parser_t parser;
compiler_t compiler;

static parserule_t *get_rule(tokentype_e type);
static void declaration();

static void compiler_init() {
    compiler.ops = NULL;
    compiler.op_count = 0;
    compiler.op_capcity = 0;

    compiler.buffer = malloc(1024 * 1024);
    compiler.current = compiler.buffer;
    compiler.local_count = 0;
    compiler.stack_count = 0;
    compiler.scope_depth = 0;
    compiler.no_op = false;

    compiler.temp_max = 0;
    compiler.const_count = 0;
    compiler.data_offset = 0;
    compiler.loop_counter = 0;

    compiler.structdef_count = 0;
    compiler.fundef_count = 0;
}

static void compiler_free() {
    if (compiler.ops != NULL)
        free(compiler.ops);

    if (compiler.buffer != NULL)
        free(compiler.buffer);
}

static void error(const char *message) {
    fprintf(stderr, "[line %d] Error: %s\n", parser.current.line, message);
    parser.had_error = true;
}

static void advance() {
    parser.previous = parser.current;

    while (true) {
        parser.current = lexer_token();
        if (parser.current.type != TOKEN_ERROR)
            break;

        parser.had_error = true;
    }

#ifdef DEBUG_OUTPUT
    print_token(parser.previous);
#endif
}

static bool check(tokentype_e type) { return parser.current.type == type; }

static bool match(tokentype_e type) {
    if (parser.current.type != type)
        return false;

    advance();
    return true;
}

static void consume(tokentype_e type, const char *message) {
    if (parser.current.type == type) {
        advance();
        return;
    }

    error(message);
}

static type_e create_type(type_e base_type, bool is_pointer, int data) {
    type_e type = base_type & TYPE_BASE_MASK;

    if (is_pointer) {
        type |= TYPE_POINTER_FLAG;
    }

    type |= (data << 8) & TYPE_DATA_MASK;

    return type;
}

static type_e token_to_type(tokentype_e type, bool is_pointer) {
    type_e base_type;

    switch (type) {
    case TOKEN_I32:
        base_type = TYPE_I32;
        break;
    case TOKEN_I64:
        base_type = TYPE_I64;
        break;
    case TOKEN_F32:
        base_type = TYPE_F32;
        break;
    case TOKEN_F64:
        base_type = TYPE_F64;
        break;
    case TOKEN_BOOL:
        base_type = TYPE_BOOL;
        break;
    case TOKEN_VOID:
        base_type = TYPE_VOID;
        break;
    case TOKEN_CHAR:
        base_type = TYPE_CHAR;
        break;
    default:
        return TYPE_UNKNOWN;
    }

    return create_type(base_type, is_pointer, 0);
}

static bool is_type(type_e type, type_e check) {
    return (type & TYPE_BASE_MASK) == check;
}

static bool is_pointer(type_e type) { return (type & TYPE_POINTER_FLAG); }
static bool is_absolute(type_e type) { return (type & TYPE_ABSOLUTE_FLAG); }

static bool is_struct(type_e type) {
    return is_type(type, TYPE_STRUCT) && !is_pointer(type);
}

static bool isv_local(stackvtype_e type) { return (type & VTYPE_LOCAL); }
static bool isv_const(stackvtype_e type) { return (type & VTYPE_CONST); }
static bool isv_stack(stackvtype_e type) { return (type & VTYPE_STACK); }
static bool isv_lvalue(stackvtype_e type) { return (type & VTYPE_LVALUE); }

static int get_type_data(type_e type) { return (type & TYPE_DATA_MASK) >> 8; }

static int calculate_struct_size(structdef_t *structdef);

static int get_type_size(type_e type) {
    if (is_pointer(type)) {
        return 4; // Assuming pointer size is 4 bytes
    }

    switch (type & TYPE_BASE_MASK) {
    case TYPE_I32:
        return 4;
    case TYPE_I64:
        return 8;
    case TYPE_F32:
        return 4;
    case TYPE_F64:
        return 8;
    case TYPE_CHAR:
        return 1;
    case TYPE_BOOL:
        return 4; // Assuming bool is represented as i32
    case TYPE_STRUCT: {
        int structdef_index = get_type_data(type);
        structdef_t *structdef = &compiler.structdefs[structdef_index];
        return calculate_struct_size(structdef);
    }
    default:
        error("Unknown type.");
        return -1;
    }
}

static int calculate_struct_size(structdef_t *structdef) {
    int size = 0;
    for (int i = 0; i < structdef->field_count; i++) {
        type_e field_type = structdef->fields[i].type;
        size += get_type_size(field_type);
    }

    return size;
}

static type_e match_type() {
    token_t current = parser.current;
    bool matched_primitive = false;

    switch (current.type) {
    case TOKEN_VOID:
    case TOKEN_I32:
    case TOKEN_I64:
    case TOKEN_F32:
    case TOKEN_F64:
    case TOKEN_BOOL:
    case TOKEN_CHAR:
        matched_primitive = true;
        break;
    default:
        matched_primitive = false;
        break;
    }

    structdef_t *structdef = NULL;
    if (!matched_primitive) {
        for (int i = 0; i < compiler.structdef_count; i++) {
            if (compiler.structdefs[i].name.length == current.length &&
                strncmp(compiler.structdefs[i].name.start, current.start,
                        current.length) == 0) {
                structdef = &compiler.structdefs[i];
                break;
            }
        }

        if (structdef == NULL) {
            return TYPE_UNKNOWN;
        }
    }

    advance();

    bool is_pointer_type = match(TOKEN_STAR);
    return matched_primitive ? token_to_type(current.type, is_pointer_type)
                             : create_type(TYPE_STRUCT, is_pointer_type,
                                           structdef - compiler.structdefs);
}

static void write_op(op_t op) {
    if (compiler.no_op) {
        return;
    }

    if (compiler.op_count >= compiler.op_capcity) {
        compiler.op_capcity =
            compiler.op_capcity < 8 ? 8 : compiler.op_capcity * 2;
        compiler.ops =
            realloc(compiler.ops, sizeof(op_t) * compiler.op_capcity);
    }

    compiler.ops[compiler.op_count++] = op;
}

static void print_op(op_t op) {
    if (compiler.no_op) {
        return;
    }

    switch (op.type) {
    case OP_ADD:
        printf("OP_ADD\n");
        break;
    case OP_SUB:
        printf("OP_SUB\n");
        break;
    case OP_MUL:
        printf("OP_MUL\n");
        break;
    case OP_DIV:
        printf("OP_DIV\n");
        break;
    case OP_ADDRESS_VAR:
        printf("OP_ADDRESS_VAR %d\n", op.as.var_index);
        break;
    case OP_CONST:
        printf("OP_CONST %d\n", op.as.const_value);
        break;
    case OP_CALL:
        printf("OP_CALL %d\n", op.as.var_index);
        break;
    case OP_IF:
        printf("OP_IF\n");
        break;
    case OP_ELSE:
        printf("OP_ELSE\n");
        break;
    case OP_END:
        printf("OP_END\n");
        break;
    case OP_PRINT:
        printf("OP_PRINT\n");
        break;
    case OP_LOAD:
        printf("OP_LOAD\n");
        break;
    case OP_STORE:
        printf("OP_STORE\n");
        break;
    case OP_CONVERT:
        printf("OP_CONVERT from %d to %d\n", op.as.convert.from,
               op.as.convert.to);
        break;
    case OP_COPY_TEMP:
        printf("OP_COPY_TEMP offset %d\n", op.as.temp_offset);
        break;
    case OP_LOAD_VAR:
        printf("OP_LOAD_VAR\n");
        break;
    case OP_STORE_VAR:
        printf("OP_STORE_VAR\n");
        break;
    case OP_LOOP:
        printf("OP_LOOP %d\n", op.as.label_index);
        break;
    case OP_BLOCK:
        printf("OP_BLOCK %d\n", op.as.label_index);
        break;
    case OP_JMP:
        printf("OP_JMP %d\n", op.as.label_index);
        break;
    case OP_JMPIF:
        printf("OP_JMPIF %d\n", op.as.label_index);
        break;
    case OP_NEGATE:
        printf("OP_NEGATE\n");
        break;
    default:
        printf("UNKNOWN_OP\n");
        break;
    }
}

static void write_string(const char *data) {
    char c;

    while ((c = *data++) != '\0') {
        *compiler.current++ = c;
    }
}

static void write_type(type_e type) {
    if (is_pointer(type)) {
        write_string("i32"); // Representing pointers as i32 in WebAssembly
        return;
    }

    switch (type & TYPE_BASE_MASK) {
    case TYPE_I32:
        write_string("i32");
        break;
    case TYPE_I64:
        write_string("i64");
        break;
    case TYPE_F32:
        write_string("f32");
        break;
    case TYPE_F64:
        write_string("f64");
        break;
    case TYPE_BOOL:
        write_string("i32"); // Representing bool as i32 in WebAssembly
        break;
    case TYPE_STRUCT:
        write_string(
            "i32"); // Representing structs as i32 (pointer) in WebAssembly
        break;
    case TYPE_CHAR:
        write_string("i32"); // Representing char as i32 in WebAssembly
        break;
    default:
        error("Expected a primitive type.");
        break;
    }
}

static void write_token(token_t token) {
    for (int i = 0; i < token.length; i++) {
        *compiler.current++ = token.start[i];
    }
}

// materialize vstack value on the wasm stack
static void materialize_vstack(stackv_t *stackv) {
    if (isv_lvalue(stackv->type)) {
        if (isv_local(stackv->type)) {
            write_op(
                (op_t){.type = OP_ADDRESS_VAR,
                       .value_type = stackv->ctype,
                       .offset = stackv->offset,
                       .as.var_index = stackv->as.local - compiler.locals});

            write_op(
                (op_t){.type = OP_LOAD_VAR,
                       .value_type = stackv->ctype,
                       .as.var_index = stackv->as.local - compiler.locals});

            stackv->type &= ~VTYPE_STORAGE;
            stackv->type |= VTYPE_STACK;
        } else if (isv_stack(stackv->type)) {
            if (stackv->offset > 0) {
                write_op((op_t){.type = OP_CONST,
                                .value_type = TYPE_I32,
                                .as.const_value = stackv->offset});

                write_op((op_t){.type = OP_ADD, .value_type = TYPE_I32});
            }

            // for struct types the address it the value, we just leave the
            // address on the stack
            if (!is_struct(stackv->ctype)) {
                write_op((op_t){.type = OP_LOAD, .value_type = stackv->ctype});
            }
        }

        stackv->type &= ~VTYPE_LVALUE;
    } else {
        if (isv_const(stackv->type)) {
            write_op((op_t){.type = OP_CONST,
                            .value_type = stackv->ctype,
                            .as.const_value = stackv->as.const_value});

            stackv->type &= ~VTYPE_STORAGE;
            stackv->type |= VTYPE_STACK;
        } else if (isv_local(stackv->type)) {
            write_op(
                (op_t){.type = OP_ADDRESS_VAR,
                       .value_type = stackv->ctype,
                       .offset = stackv->offset,
                       .as.var_index = stackv->as.local - compiler.locals});
        } else if (isv_stack(stackv->type)) {
            if (stackv->offset > 0) {
                write_op((op_t){.type = OP_CONST,
                                .value_type = TYPE_I32,
                                .as.const_value = stackv->offset});

                write_op((op_t){.type = OP_ADD, .value_type = TYPE_I32});
            }
        }
    }
}

// store vstack value
static void store_vstack(stackv_t *stackv) {
    if (isv_stack(stackv->type)) {
        write_op((op_t){.type = OP_STORE, .value_type = stackv->ctype});
    } else {
        write_op((op_t){.type = OP_STORE_VAR,
                        .value_type = stackv->ctype,
                        .as.var_index = stackv->as.local - compiler.locals});
    }
}

static void expression(prec_e precedence) {
    advance();
    parsefn_t prefix_rule = get_rule(parser.previous.type)->prefix;
    if (prefix_rule == NULL) {
        error("Expected an expression.");
        return;
    }

    prefix_rule();

    while (precedence <= get_rule(parser.current.type)->precedence) {
        advance();

        parsefn_t infix_rule = get_rule(parser.previous.type)->infix;
        if (infix_rule == NULL) {
            error("Expected an infix operator.");
            return;
        }

        infix_rule();
    }
}

static void number() {
    stackv_t *stackv = &compiler.stack[compiler.stack_count++];
    stackv->type = VTYPE_CONST;
    stackv->ctype = TYPE_I32;
    stackv->as.const_value = atoi(parser.previous.start);
}

static void boolean() {
    stackv_t *stackv = &compiler.stack[compiler.stack_count++];
    stackv->type = VTYPE_CONST;
    stackv->ctype = TYPE_BOOL;

    switch (parser.previous.type) {
    case TOKEN_TRUE:
        stackv->as.const_value = 1;
        break;
    case TOKEN_FALSE:
        stackv->as.const_value = 0;
        break;
    default:
        error("Expected a boolean literal.");
        break;
    }
}

static bool is_numeric_type(type_e type) {
    return type == TYPE_I32 || type == TYPE_I64 || type == TYPE_F32 ||
           type == TYPE_F64 || is_pointer(type);
}

static void binary() {
    tokentype_e op_type = parser.previous.type;
    prec_e precedence = get_rule(op_type)->precedence;

    expression((prec_e)(precedence + 1));

    stackv_t *a = &compiler.stack[compiler.stack_count - 2];
    stackv_t *b = &compiler.stack[compiler.stack_count - 1];
    compiler.stack_count -= 2;

    materialize_vstack(a);
    materialize_vstack(b);

    type_e target_type = is_pointer(b->ctype) ? b->ctype : a->ctype;

    stackv_t *result = &compiler.stack[compiler.stack_count++];
    result->type = VTYPE_STACK;
    result->ctype = target_type;

    switch (op_type) {
    case TOKEN_PLUS:
        write_op((op_t){.type = OP_ADD, .value_type = target_type});
        break;
    case TOKEN_MINUS:
        write_op((op_t){.type = OP_SUB, .value_type = target_type});
        break;
    case TOKEN_STAR:
        write_op((op_t){.type = OP_MUL, .value_type = target_type});
        break;
    case TOKEN_SLASH:
        write_op((op_t){.type = OP_DIV, .value_type = target_type});
        break;
    default:
        error("Expected a binary operator.");
        break;
    }
}

static void unary() {
    tokentype_e op_type = parser.previous.type;

    expression(PREC_UNARY);

    stackv_t *last_stackv = &compiler.stack[compiler.stack_count - 1];
    compiler.stack_count--; // Pop the last stack value

    materialize_vstack(last_stackv);

    stackv_t *result = &compiler.stack[compiler.stack_count++];
    result->type = VTYPE_STACK;
    result->ctype = last_stackv->ctype;
    result->offset = 0;

    switch (op_type) {
    case TOKEN_MINUS:
        if (!is_numeric_type(last_stackv->ctype)) {
            error("Unary '-' operator requires a numeric type.");
            return;
        }

        write_op((op_t){.type = OP_CONST,
                        .value_type = last_stackv->ctype,
                        .as.const_value = -1});
        write_op((op_t){.type = OP_MUL, .value_type = last_stackv->ctype});
        break;
    case TOKEN_BANG:
        result->ctype = TYPE_BOOL;
        write_op((op_t){.type = OP_NEGATE, .value_type = TYPE_BOOL});
        break;
    default:
        error("Expected a unary operator.");
        break;
    }
}

static void group() {
    expression(PREC_ASSIGNMENT);
    if (!match(TOKEN_RIGHT_PAREN)) {
        error("Expected ')' after expression.");
    }
}

static void dot() {
    stackv_t *last_stackv = &compiler.stack[compiler.stack_count - 1];
    if (!isv_lvalue(last_stackv->type)) {
        error("Cannot access fields of a non-lvalue.");
        return;
    }

    if (!is_type(last_stackv->ctype, TYPE_STRUCT)) {
        error("Expected a struct type before '.'.");
        return;
    }

    if (is_pointer(last_stackv->ctype)) {
        error("Cannot access fields of a pointer to a struct.");
        return;
    }

    consume(TOKEN_IDENTIFIER, "Expected field name after '.'.");
    token_t field_name = parser.previous;

    structdef_t *structdef =
        &compiler.structdefs[get_type_data(last_stackv->ctype)];
    fielddef_t *field = NULL;

    for (int i = 0; i < structdef->field_count; i++) {
        if (structdef->fields[i].name.length == field_name.length &&
            strncmp(structdef->fields[i].name.start, field_name.start,
                    field_name.length) == 0) {
            field = &structdef->fields[i];
            break;
        }
    }

    if (field == NULL) {
        error("Undefined field in struct.");
        return;
    }

    int field_index = field - structdef->fields;
    int byte_offset = 0;
    for (int j = 0; j < field_index; j++) {
        fielddef_t *prev_field = &structdef->fields[j];
        if (is_pointer(prev_field->type)) {
            byte_offset += 4;
        } else if (prev_field->type == TYPE_I32 ||
                   prev_field->type == TYPE_F32 ||
                   prev_field->type == TYPE_BOOL) {
            byte_offset += 4;
        } else if (prev_field->type == TYPE_I64 ||
                   prev_field->type == TYPE_F64) {
            byte_offset += 8;
        } else {
            error("Unsupported field type in struct.");
        }
    }

    last_stackv->ctype = field->type;
    last_stackv->offset += byte_offset;
}

static void call() {
    stackv_t *last_stackv = &compiler.stack[compiler.stack_count - 1];
    if (!isv_local(last_stackv->type)) {
        error("Cannot call a non-local variable.");
        return;
    }

    if (!is_type(last_stackv->ctype, TYPE_FUN)) {
        error("Cannot call a non-function variable.");
        return;
    }

    if (!isv_lvalue(last_stackv->type)) {
        error("Cannot call a non-lvalue function.");
        return;
    }

    if (is_pointer(last_stackv->ctype)) {
        error("Cannot call a pointer to a function.");
        return;
    }

    int temp_offset = 0;
    while (!check(TOKEN_RIGHT_PAREN) && parser.current.type != TOKEN_EOF) {
        expression(PREC_ASSIGNMENT);
        if (!check(TOKEN_RIGHT_PAREN)) {
            consume(TOKEN_COMMA, "Expected ',' between arguments.");
        }

        stackv_t *last_stackv = &compiler.stack[compiler.stack_count - 1];
        materialize_vstack(last_stackv);

        if (is_type(last_stackv->ctype, TYPE_STRUCT) &&
            !is_pointer(last_stackv->ctype)) {
            int structdef_index = get_type_data(last_stackv->ctype);
            structdef_t *structdef = &compiler.structdefs[structdef_index];
            int size = calculate_struct_size(structdef);

            write_op((op_t){.type = OP_COPY_TEMP,
                            .value_type = last_stackv->ctype,
                            .as.temp_offset = temp_offset});

            temp_offset += size;
        }
    }

    if (temp_offset > compiler.temp_max) {
        compiler.temp_max = temp_offset;
    }

    write_op((op_t){.type = OP_CALL,
                    .value_type = last_stackv->ctype,
                    .as.var_index = last_stackv->as.local - compiler.locals});

    consume(TOKEN_RIGHT_PAREN, "Expected ')' after function call.");
}

static void identifier() {
    local_t *found = NULL;
    for (int i = compiler.local_count - 1; i >= 0; --i) {
        if (compiler.locals[i].name.length == parser.previous.length &&
            strncmp(compiler.locals[i].name.start, parser.previous.start,
                    parser.previous.length) == 0) {
            found = &compiler.locals[i];
            break;
        }
    }

    if (found == NULL) {
        error("Undefined variable.");
        return;
    }

    stackv_t *stackv = &compiler.stack[compiler.stack_count++];
    stackv->type = VTYPE_LOCAL | VTYPE_LVALUE;
    stackv->ctype = found->type;
    stackv->offset = 0;
    stackv->as.local = found;
}

static void begin_scope() { compiler.scope_depth++; }

static void end_scope() { compiler.scope_depth--; }

static void block() {
    while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
        declaration();
    }

    consume(TOKEN_RIGHT_BRACE, "Expected '}' after block.");
}

static void address() {
    expression(PREC_UNARY);

    stackv_t *last_stackv = &compiler.stack[compiler.stack_count - 1];
    if (!isv_lvalue(last_stackv->type)) {
        error("Cannot take the address of a non-lvalue.");
        return;
    }

    local_t *local = last_stackv->as.local;
    bool is_struct =
        is_type(local->type, TYPE_STRUCT) && !is_pointer(local->type);

    if (!is_struct) {
        local->is_on_stack = true;
    }

    last_stackv->type &= ~VTYPE_LVALUE;
    last_stackv->ctype |= TYPE_POINTER_FLAG;
}

static void dereference() {
    expression(PREC_UNARY);

    stackv_t *last_stackv = &compiler.stack[compiler.stack_count - 1];
    if (!is_pointer(last_stackv->ctype)) {
        error("Cannot dereference a non-pointer type.");
        return;
    }

    if (isv_lvalue(last_stackv->type)) {
        materialize_vstack(last_stackv);
    }

    last_stackv->ctype &= ~TYPE_POINTER_FLAG;
    last_stackv->type |= VTYPE_LVALUE;
}

static void assignment() {
    stackv_t *last_stackv = &compiler.stack[compiler.stack_count - 1];
    if (!isv_lvalue(last_stackv->type)) {
        error("Invalid assignment target.");
        return;
    }

    last_stackv->type &= ~VTYPE_LVALUE;
    materialize_vstack(last_stackv);

    expression(PREC_ASSIGNMENT);

    stackv_t *value_stackv = &compiler.stack[compiler.stack_count - 1];
    compiler.stack_count -= 2; // Pop both the lvalue and the value

    materialize_vstack(value_stackv);
    store_vstack(last_stackv);
}

static void character() {
    stackv_t *stackv = &compiler.stack[compiler.stack_count++];
    stackv->type = VTYPE_CONST;
    stackv->ctype = TYPE_CHAR;

    char c = parser.previous.start[1];
    stackv->as.const_value = (int)c;
}

static void string() {
    stackv_t *stackv = &compiler.stack[compiler.stack_count++];
    stackv->type = VTYPE_CONST;
    stackv->ctype = TYPE_CHAR | TYPE_POINTER_FLAG;

    const_t *string_const = &compiler.consts[compiler.const_count++];

    string_const->is_local = false;
    string_const->type = TYPE_CHAR | TYPE_POINTER_FLAG;
    string_const->offset = compiler.data_offset;
    string_const->size = parser.previous.length - 2;
    string_const->as.string = parser.previous.start + 1;

    stackv->as.const_value = 16384 + compiler.data_offset;
    compiler.data_offset += string_const->size + 1;
}

static void bracket() {
    stackv_t *last_stackv = &compiler.stack[--compiler.stack_count];
    type_e element_type = last_stackv->ctype;
    if (!is_pointer(last_stackv->ctype)) {
        error("Cannot index a non-pointer type.");
        return;
    }

    materialize_vstack(last_stackv);

    expression(PREC_ASSIGNMENT);
    consume(TOKEN_RBRACKET, "Expected ']' after index expression.");

    stackv_t *index_stackv = &compiler.stack[compiler.stack_count - 1];
    if (!is_numeric_type(index_stackv->ctype)) {
        error("Index must be a numeric type.");
        return;
    }

    materialize_vstack(index_stackv);
    write_op((op_t){.type = OP_CONST,
                    .value_type = TYPE_I32,
                    .as.const_value =
                        get_type_size(element_type & ~TYPE_POINTER_FLAG)});
    write_op((op_t){.type = OP_MUL, .value_type = TYPE_I32});
    write_op((op_t){.type = OP_ADD, .value_type = TYPE_I32});

    index_stackv->ctype = element_type & ~TYPE_POINTER_FLAG;
    index_stackv->type |= VTYPE_LVALUE;
}

static void sizeof_prefix() {
    consume(TOKEN_LEFT_PAREN, "Expected '(' after 'sizeof'.");

    type_e type = match_type();
    if (type == TYPE_UNKNOWN) {
        bool last_no_op = compiler.no_op;

        compiler.no_op = true;
        expression(PREC_ASSIGNMENT);
        compiler.no_op = last_no_op;

        stackv_t *last_stackv = &compiler.stack[--compiler.stack_count];
        type = last_stackv->ctype;
    }

    int size = get_type_size(type);

    stackv_t *stackv = &compiler.stack[compiler.stack_count++];
    stackv->type = VTYPE_CONST;
    stackv->ctype = TYPE_I32;
    stackv->as.const_value = size;

    consume(TOKEN_RIGHT_PAREN, "Expected ')' after 'sizeof'.");
}

parserule_t rules[] = {[TOKEN_LEFT_PAREN] = {group, call, PREC_CALL},
                       [TOKEN_RIGHT_PAREN] = {NULL, NULL, PREC_NONE},
                       [TOKEN_NUMBER] = {number, NULL, PREC_NONE},
                       [TOKEN_PLUS] = {NULL, binary, PREC_TERM},
                       [TOKEN_MINUS] = {unary, binary, PREC_TERM},
                       [TOKEN_STAR] = {dereference, binary, PREC_FACTOR},
                       [TOKEN_SLASH] = {NULL, binary, PREC_FACTOR},
                       [TOKEN_IDENTIFIER] = {identifier, NULL, PREC_NONE},
                       [TOKEN_AMPERSAND] = {address, NULL, PREC_NONE},
                       [TOKEN_I32] = {NULL, NULL, PREC_NONE},
                       [TOKEN_I64] = {NULL, NULL, PREC_NONE},
                       [TOKEN_F32] = {NULL, NULL, PREC_NONE},
                       [TOKEN_F64] = {NULL, NULL, PREC_NONE},
                       [TOKEN_IF] = {NULL, NULL, PREC_NONE},
                       [TOKEN_ELSE] = {NULL, NULL, PREC_NONE},
                       [TOKEN_LEFT_BRACE] = {NULL, NULL, PREC_NONE},
                       [TOKEN_RIGHT_BRACE] = {NULL, NULL, PREC_NONE},
                       [TOKEN_COMMA] = {NULL, NULL, PREC_NONE},
                       [TOKEN_SEMICOLON] = {NULL, NULL, PREC_NONE},
                       [TOKEN_PRINT] = {NULL, NULL, PREC_NONE},
                       [TOKEN_EQUAL] = {NULL, assignment, PREC_ASSIGNMENT},
                       [TOKEN_ERROR] = {NULL, NULL, PREC_NONE},
                       [TOKEN_BOOL] = {NULL, NULL, PREC_NONE},
                       [TOKEN_TRUE] = {boolean, NULL, PREC_NONE},
                       [TOKEN_FALSE] = {boolean, NULL, PREC_NONE},
                       [TOKEN_EOF] = {NULL, NULL, PREC_NONE},
                       [TOKEN_STRUCT] = {NULL, NULL, PREC_NONE},
                       [TOKEN_DOT] = {NULL, dot, PREC_ACCESS},
                       [TOKEN_CHAR_LITERAL] = {character, NULL, PREC_NONE},
                       [TOKEN_STRING_LITERAL] = {string, NULL, PREC_NONE},
                       [TOKEN_VOID] = {NULL, NULL, PREC_NONE},
                       [TOKEN_LBRACKET] = {NULL, bracket, PREC_ACCESS},
                       [TOKEN_RBRACKET] = {NULL, NULL, PREC_NONE},
                       [TOKEN_BANG] = {unary, NULL, PREC_UNARY},
                       [TOKEN_SIZEOF] = {sizeof_prefix, NULL, PREC_NONE}};

static parserule_t *get_rule(tokentype_e type) { return &rules[type]; }

static void print_statement() {
    expression(PREC_ASSIGNMENT);
    consume(TOKEN_SEMICOLON, "Expected ';' after declaration.");

    stackv_t *last_stackv = &compiler.stack[compiler.stack_count - 1];
    compiler.stack_count--; // Pop the last stack value

    materialize_vstack(last_stackv);
    write_op((op_t){.type = OP_PRINT, .value_type = last_stackv->ctype});
}

static void if_statement() {
    consume(TOKEN_LEFT_PAREN, "Expected '(' after 'if'.");
    expression(PREC_ASSIGNMENT);
    consume(TOKEN_RIGHT_PAREN, "Expected ')' after condition.");

    materialize_vstack(&compiler.stack[compiler.stack_count - 1]);

    write_op((op_t){.type = OP_IF});

    consume(TOKEN_LEFT_BRACE, "Expected '{' after 'if' condition.");

    begin_scope();
    block();
    end_scope();

    write_op((op_t){.type = OP_END});

    if (match(TOKEN_ELSE)) {
        consume(TOKEN_LEFT_BRACE, "Expected '{' after 'else'.");
        write_op((op_t){.type = OP_ELSE});

        begin_scope();
        block();
        end_scope();

        write_op((op_t){.type = OP_END});
    }

    write_op((op_t){.type = OP_END});
}

static void while_statement() {
    consume(TOKEN_LEFT_PAREN, "Expected '(' after 'while'.");
    expression(PREC_ASSIGNMENT);
    consume(TOKEN_RIGHT_PAREN, "Expected ')' after condition.");

    int loop_label = compiler.loop_counter++;
    int block_label = compiler.loop_counter++;

    write_op((op_t){.type = OP_LOOP, .as.label_index = loop_label});
    write_op((op_t){.type = OP_BLOCK, .as.label_index = block_label});

    materialize_vstack(&compiler.stack[compiler.stack_count - 1]);

    write_op((op_t){.type = OP_NEGATE, .value_type = TYPE_BOOL});
    write_op((op_t){.type = OP_JMPIF, .as.label_index = block_label});

    consume(TOKEN_LEFT_BRACE, "Expected '{' after 'while' condition.");

    begin_scope();
    block();
    end_scope();

    write_op((op_t){.type = OP_JMP, .as.label_index = loop_label});
    write_op((op_t){.type = OP_END});
    write_op((op_t){.type = OP_END});
}

static parser_t parser_snapshot() { return parser; }

static void parser_restore(parser_t *snapshot) { parser = *snapshot; }

static void for_statement() {
    begin_scope();
    consume(TOKEN_LEFT_PAREN, "Expected '(' after 'for'.");

    if (!check(TOKEN_SEMICOLON)) {
        declaration();
    } else {
        advance(); // Consume the semicolon
    }

    int loop_label = compiler.loop_counter++;
    int block_label = compiler.loop_counter++;

    write_op((op_t){.type = OP_LOOP, .as.label_index = loop_label});
    write_op((op_t){.type = OP_BLOCK, .as.label_index = block_label});

    if (!check(TOKEN_SEMICOLON)) {
        expression(PREC_ASSIGNMENT);
    }

    materialize_vstack(&compiler.stack[compiler.stack_count - 1]);
    write_op((op_t){.type = OP_NEGATE, .value_type = TYPE_BOOL});
    write_op((op_t){.type = OP_JMPIF, .as.label_index = block_label});

    bool has_inc = false;
    consume(TOKEN_SEMICOLON, "Expected ';' after loop condition.");

    lexer_t snapshot = lexer_snapshot();
    parser_t psnapshot = parser_snapshot();
    if (!check(TOKEN_RIGHT_PAREN)) {
        has_inc = true;
        // expression(PREC_ASSIGNMENT);
        while (!check(TOKEN_RIGHT_PAREN) && !check(TOKEN_EOF)) {
            advance();
        }
    }
    consume(TOKEN_RIGHT_PAREN, "Expected ')' after 'for' clauses.");

    consume(TOKEN_LEFT_BRACE, "Expected '{' after 'for' clauses.");
    begin_scope();
    block();
    end_scope();

    if (has_inc) {
        lexer_t now = lexer_snapshot();
        parser_t psnapshot_now = parser_snapshot();

        lexer_restore(&snapshot);
        parser_restore(&psnapshot);
        expression(PREC_ASSIGNMENT);
        lexer_restore(&now);
        parser_restore(&psnapshot_now);
    }

    write_op((op_t){.type = OP_JMP, .as.label_index = loop_label});
    write_op((op_t){.type = OP_END});
    write_op((op_t){.type = OP_END});

    end_scope();
}

static void named_var(type_e type) {
    token_t name = parser.previous;
    bool is_global = compiler.scope_depth == 0;

    local_t *local = &compiler.locals[compiler.local_count];
    local->name = name;
    local->type = type;
    local->is_on_stack = is_type(type, TYPE_STRUCT) && !is_pointer(type);
    local->is_param = false;
    local->depth = compiler.scope_depth;

    compiler.local_count++;

    if (is_global) {
        local->type |= TYPE_ABSOLUTE_FLAG;
        local->is_on_stack = true;
        local->frame_index = 16384 + compiler.data_offset;

        const_t *global_const = &compiler.consts[compiler.const_count++];
        global_const->type = type;
        global_const->is_local = false;
        global_const->offset = compiler.data_offset;
        global_const->size = get_type_size(type);

        compiler.data_offset += global_const->size;

        if (match(TOKEN_EQUAL)) {
            expression(PREC_ASSIGNMENT);

            stackv_t *last_stackv = &compiler.stack[compiler.stack_count - 1];
            compiler.stack_count--; // Pop the last stack value

            if (isv_lvalue(last_stackv->type)) {
                error("Cannot assign an lvalue to a global variable.");
                return;
            }

            if (isv_local(last_stackv->type)) {
                global_const->is_local = true;
                global_const->as.local = last_stackv->as.local;
            } else if (isv_const(last_stackv->type)) {
                global_const->as.const_value = last_stackv->as.const_value;
            } else {
                error(
                    "Cannot assign a non-constant value to a global variable.");
                return;
            }
        }

        return;
    }

    if (match(TOKEN_EQUAL)) {
        write_op((op_t){.type = OP_ADDRESS_VAR,
                        .value_type = type,
                        .offset = 0,
                        .as.var_index = local - compiler.locals});

        expression(PREC_ASSIGNMENT);

        stackv_t *last_stackv = &compiler.stack[compiler.stack_count - 1];
        compiler.stack_count--; // Pop the last stack value

        materialize_vstack(last_stackv);
        write_op((op_t){.type = OP_STORE_VAR,
                        .value_type = type,
                        .as.var_index = local - compiler.locals});
    }
}

static void function(type_e return_type) {
    if (compiler.scope_depth > 0) {
        error("Functions cannot be declared inside other functions or blocks.");
        return;
    }

    token_t name = parser.previous;
    consume(TOKEN_LEFT_PAREN, "Expected '(' after function name.");

    local_t *local = &compiler.locals[compiler.local_count];
    local->name = name;
    local->type = create_type(TYPE_FUN, false, compiler.fundef_count);
    local->is_on_stack = false;
    local->depth = compiler.scope_depth;
    compiler.local_count++;

    if (!compiler.no_op) {
        write_string("(func $");
        write_token(name);
        write_string(" ");
    }

    compiler.temp_max = 0;
    int frame_ptr = compiler.local_count;

    fundef_t *fundef = &compiler.fundefs[compiler.fundef_count++];
    fundef->name = name;
    fundef->return_type = return_type;
    fundef->param_count = 0;

    // Parse parameters
    while (!check(TOKEN_RIGHT_PAREN) && !check(TOKEN_EOF)) {
        type_e param_type;
        if ((param_type = match_type())) {
            consume(TOKEN_IDENTIFIER, "Expected parameter name.");
            token_t param_name = parser.previous;

            if (!compiler.no_op) {
                write_string("(param $");
                write_token(param_name);
                write_string(" ");
                write_type(param_type);
                write_string(")");
            }

            local_t *local = &compiler.locals[compiler.local_count];
            local->name = param_name;
            local->type = param_type;
            local->is_on_stack = false;
            local->is_param = true;

            compiler.local_count++;
        } else {
            error("Expected a type for parameter.");
        }

        if (!check(TOKEN_RIGHT_PAREN)) {
            consume(TOKEN_COMMA, "Expected ',' between parameters.");
        }
    }

    fundef->param_count = compiler.local_count - frame_ptr;
    fundef->params =
        (fielddef_t *)malloc(sizeof(fielddef_t) * fundef->param_count);

    for (int i = 0; i < fundef->param_count; i++) {
        local_t *param_local = &compiler.locals[frame_ptr + i];

        fundef->params[i].name = param_local->name;
        fundef->params[i].type = param_local->type;
    }

    if (!compiler.no_op) {
        if (return_type != TYPE_VOID) {
            write_string(" (result ");
            write_type(return_type);
            write_string(")");
        }

        write_string("\n");
    }

    consume(TOKEN_RIGHT_PAREN, "Expected ')' after function parameters.");
    consume(TOKEN_LEFT_BRACE, "Expected '{' before function body.");

    begin_scope();
    block();
    end_scope();

    if (compiler.no_op) {
        return;
    }

    int stack_length = 0;
    for (int i = compiler.local_count - 1; i >= frame_ptr; --i) {
        local_t *local = &compiler.locals[i];
        if (local->is_on_stack) {
            local->frame_index = stack_length;
            stack_length += get_type_size(local->type);
        } else if (!local->is_param) {
            write_string("(local $");
            write_token(local->name);
            write_string(" ");
            write_type(local->type);
            write_string(")\n");
        }
    }

    int temp_base = stack_length;
    stack_length += compiler.temp_max;

    write_string("(local $_scratch i32)\n");
    write_string("(local $_scratch_64 i64)\n");

    if (stack_length > 0) {
        write_string("global.get $__sp\n");
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "i32.const %d\n", stack_length);
        write_string(buffer);
        write_string("i32.sub\n");
        write_string("global.set $__sp\n");
    }

    for (int i = compiler.local_count - 1; i >= frame_ptr; --i) {
        local_t *local = &compiler.locals[i];
        if (local->is_on_stack && local->is_param) {
            write_string("global.get $__sp\n");
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "i32.const %d\n",
                     local->frame_index);
            write_string(buffer);
            write_string("i32.add\n");
            write_string("local.get $");
            write_token(local->name);
            write_string("\n");
            write_type(local->type);
            write_string(".store\n");
        }
    }

    for (int i = 0; i < compiler.op_count; i++) {
#ifdef DEBUG_OUTPUT
        print_op(compiler.ops[i]);
#endif
        switch (compiler.ops[i].type) {
        case OP_COPY_TEMP: { // pops last value on wasm-stack, copied to temp
                             // and pushes address of temp on wasm-stack
            op_t *op = &compiler.ops[i];
            if (is_type(op->value_type, TYPE_STRUCT) &&
                !is_pointer(op->value_type)) {
                int structdef_index = get_type_data(op->value_type);
                structdef_t *structdef = &compiler.structdefs[structdef_index];
                int size = calculate_struct_size(structdef);

                int target_offset = temp_base + op->as.temp_offset;
                write_string(";; Copy struct to temp\n");
                write_string(
                    "local.set $_scratch\n"); // store struct address in scratch
                write_string("global.get $__sp\n");
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "i32.const %d\n",
                         target_offset);
                write_string(buffer);
                write_string("i32.add\n");
                write_string("local.get $_scratch\n");
                write_string("i32.const ");
                snprintf(buffer, sizeof(buffer), "%d\n", size);
                write_string(buffer);
                write_string("memory.copy\n");
                write_string("global.get $__sp\n");
                snprintf(buffer, sizeof(buffer), "i32.const %d\n",
                         target_offset);
                write_string(buffer);
                write_string("i32.add\n");
            } else {
                error("OP_COPY_TEMP is only supported for struct types.");
            }

            break;
        }
        case OP_ADD: {
            op_t *op = &compiler.ops[i];

            if (is_pointer(op->value_type)) {
                write_string("i32.add\n");
            } else if (is_type(op->value_type, TYPE_I64)) {
                write_string("i64.add\n");
            } else if (is_type(op->value_type, TYPE_I32)) {
                write_string("i32.add\n");
            } else {
                error("Unsupported type for addition.");
            }

            break;
        }
        case OP_SUB: {
            op_t *op = &compiler.ops[i];

            if (is_pointer(op->value_type)) {
                write_string("i32.sub\n");
            } else if (is_type(op->value_type, TYPE_I64)) {
                write_string("i64.sub\n");
            } else if (is_type(op->value_type, TYPE_I32)) {
                write_string("i32.sub\n");
            } else {
                error("Unsupported type for subtraction.");
            }

            break;
        }
        case OP_MUL:
            if (compiler.ops[i].value_type == TYPE_I64) {
                write_string("i64.mul\n");
            } else if (compiler.ops[i].value_type == TYPE_I32) {
                write_string("i32.mul\n");
            } else {
                error("Unsupported type for multiplication.");
            }

            break;
        case OP_DIV:
            if (compiler.ops[i].value_type == TYPE_I64) {
                write_string("i64.div_s\n");
            } else if (compiler.ops[i].value_type == TYPE_I32) {
                write_string("i32.div_s\n");
            } else {
                error("Unsupported type for division.");
            }

            break;
        case OP_NEGATE:
            write_string("i32.eqz\n");
            break;
        case OP_ADDRESS_VAR: {
            op_t *op = &compiler.ops[i];
            local_t *local = &compiler.locals[op->as.var_index];
            int offset = op->offset;

            if (is_absolute(local->type)) {
                write_string(";; Get address of absolute variable\n");
                write_string("i32.const ");
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d\n", local->frame_index);
                write_string(buffer);
            } else if (local->is_on_stack) {
                write_string(";; Get address of variable on stack\n");
                write_string("global.get $__sp\n");

                offset += local->frame_index;
            } else if (is_struct(local->type)) {
                write_string(";; Get address of struct variable\n");
                write_string("local.get $");
                write_token(local->name);
                write_string("\n");
            } else {
                break;
            }

            if (offset > 0) {
                write_string("i32.const ");
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d\n", offset);
                write_string(buffer);
                write_string("i32.add\n");
            }

            break;
        }
        case OP_STORE_VAR: {
            op_t *op = &compiler.ops[i];
            local_t *local = &compiler.locals[op->as.var_index];

            bool is_local_struct =
                is_type(local->type, TYPE_STRUCT) && !is_pointer(local->type);
            bool is_op_struct = is_type(op->value_type, TYPE_STRUCT) &&
                                !is_pointer(op->value_type);

            if (is_op_struct) {
                write_string(";; Copy struct to variable\n");
                write_string("i32.const ");
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d\n",
                         calculate_struct_size(
                             &compiler.structdefs[get_type_data(local->type)]));
                write_string(buffer);
                write_string("memory.copy\n");
            } else if (local->is_on_stack || is_local_struct) {
                write_type(op->value_type);

                if (is_type(op->value_type, TYPE_CHAR)) {
                    write_string(".store8\n");
                } else {
                    write_string(".store\n");
                }
            } else {
                write_string("local.set $");
                write_token(local->name);
                write_string("\n");
            }
            break;
        }
        case OP_LOAD_VAR: {
            op_t *op = &compiler.ops[i];
            local_t *local = &compiler.locals[op->as.var_index];

            bool is_local_struct =
                is_type(local->type, TYPE_STRUCT) && !is_pointer(local->type);
            bool is_op_struct = is_type(op->value_type, TYPE_STRUCT) &&
                                !is_pointer(op->value_type);

            if (is_op_struct) {
                break;
            }

            if (is_local_struct) {
                write_string(";; Load struct variable\n");
                write_type(op->value_type);
                write_string(".load\n");
            } else if (local->is_on_stack) {
                write_string(";; Load variable from stack\n");

                write_type(op->value_type);
                if (is_type(op->value_type, TYPE_CHAR)) {
                    write_string(".load8_u\n");
                } else {
                    write_string(".load\n");
                }
            } else {
                write_string("local.get $");
                write_token(local->name);
                write_string("\n");
            }

            break;
        }
        case OP_LOAD:
            if (is_struct(compiler.ops[i].value_type)) {
                // structs are represented as addresses, since address is
                // expected to be on the stack at this point, we don't need to
                // do anything here
            } else {
                write_type(compiler.ops[i].value_type);

                if (is_type(compiler.ops[i].value_type, TYPE_CHAR)) {
                    write_string(".load8_u\n");
                } else {
                    write_string(".load\n");
                }
            }
            break;
        case OP_STORE: {
            if (is_struct(compiler.ops[i].value_type)) {
                write_string(";; Copy struct to address\n");
                write_string("i32.const ");
                char buffer[32];
                snprintf(
                    buffer, sizeof(buffer), "%d\n",
                    calculate_struct_size(&compiler.structdefs[get_type_data(
                        compiler.ops[i].value_type)]));
                write_string(buffer);
                write_string("memory.copy\n");
            } else {
                write_type(compiler.ops[i].value_type);

                if (is_type(compiler.ops[i].value_type, TYPE_CHAR)) {
                    write_string(".store8\n");
                } else {
                    write_string(".store\n");
                }
            }

            break;
        }
        case OP_CONST:
            write_string("i32.const ");
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%d",
                     compiler.ops[i].as.const_value);
            write_string(buffer);
            write_string("\n");
            break;
        case OP_CONVERT: {
            op_t *op = &compiler.ops[i];

            if (op->as.convert.from == TYPE_I32 &&
                op->as.convert.to == TYPE_I64) {
                write_string("i64.extend_i32_s\n");
            } else if (op->as.convert.from == TYPE_I64 &&
                       op->as.convert.to == TYPE_I32) {
                write_string("i32.wrap_i64\n");
            } else {
                error("Unsupported type conversion.");
            }

            break;
        }
        case OP_CALL:
            write_string("call $");
            write_token(compiler.locals[compiler.ops[i].as.var_index].name);
            write_string("\n");
            break;
        case OP_IF:
            write_string("(if\n");
            write_string("(then\n");
            break;
        case OP_ELSE:
            write_string("(else\n");
            break;
        case OP_END:
            write_string(")\n");
            break;
        case OP_LOOP: {
            write_string("(loop $label");
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%d",
                     compiler.ops[i].as.label_index);
            write_string(buffer);
            write_string("\n");
            break;
        }
        case OP_BLOCK: {
            write_string("(block $label");
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%d",
                     compiler.ops[i].as.label_index);
            write_string(buffer);
            write_string("\n");
            break;
        }
        case OP_JMP: {
            write_string("br $label");
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%d",
                     compiler.ops[i].as.label_index);
            write_string(buffer);
            write_string("\n");
            break;
        }
        case OP_JMPIF: {
            write_string("br_if $label");
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%d",
                     compiler.ops[i].as.label_index);
            write_string(buffer);
            write_string("\n");
            break;
        }
        case OP_PRINT: {
            op_t *op = &compiler.ops[i];
            if (is_pointer(op->value_type)) {
                write_string("call $print_i32\n");
            } else if (is_type(op->value_type, TYPE_I32)) {
                write_string("call $print_i32\n");
            } else if (is_type(op->value_type, TYPE_I64)) {
                write_string("call $print_i64\n");
            } else {
                error("Unsupported type for print.");
            }

            break;
        }
        default:
            error("Unknown operation.");
            break;
        }
    }

    if (stack_length > 0) {
        write_string("global.get $__sp\n");
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "i32.const %d\n", stack_length);
        write_string(buffer);
        write_string("i32.add\n");
        write_string("global.set $__sp\n");
    }

    compiler.op_count = 0;
    compiler.local_count = frame_ptr;
    compiler.temp_max = 0;

    write_string(")\n");
}

static void typed_declaration(type_e decl_type) {
    consume(TOKEN_IDENTIFIER, "Expected variable name.");

    if (check(TOKEN_LEFT_PAREN)) {
        function(decl_type);
    } else {
        named_var(decl_type);
        consume(TOKEN_SEMICOLON, "Expected ';' after declaration.");
    }
}

static void struct_declaration() {
    if (compiler.scope_depth > 0) {
        error("Structs cannot be declared inside functions or blocks.");
        return;
    }

    consume(TOKEN_IDENTIFIER, "Expected struct name.");
    token_t struct_name = parser.previous;

    structdef_t *structdef = &compiler.structdefs[compiler.structdef_count++];
    structdef->name = struct_name;
    structdef->fields = NULL;
    structdef->field_count = 0;
    structdef->field_capacity = 0;

    consume(TOKEN_LEFT_BRACE, "Expected '{' after struct name.");

    while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
        type_e field_type;
        if ((field_type = match_type())) {
            consume(TOKEN_IDENTIFIER, "Expected field name.");

            if (structdef->field_count >= structdef->field_capacity) {
                structdef->field_capacity = structdef->field_capacity < 8
                                                ? 8
                                                : structdef->field_capacity * 2;
                structdef->fields =
                    realloc(structdef->fields,
                            sizeof(fielddef_t) * structdef->field_capacity);
            }

            token_t field_name = parser.previous;

            fielddef_t *field = &structdef->fields[structdef->field_count++];
            field->name = field_name;
            field->type = field_type;
        } else {
            error("Expected a type for struct field.");
        }

        consume(TOKEN_SEMICOLON, "Expected ';' after struct field.");
    }

    consume(TOKEN_RIGHT_BRACE, "Expected '}' after struct fields.");
}

static void return_statement() {
    expression(PREC_ASSIGNMENT);
    consume(TOKEN_SEMICOLON, "Expected ';' after return statement.");

    stackv_t *last_stackv = &compiler.stack[compiler.stack_count - 1];
    compiler.stack_count--; // Pop the last stack value

    materialize_vstack(last_stackv);
}

static void declaration() {
    type_e decl_type;

    if (match(TOKEN_STRUCT)) {
        struct_declaration();
    } else if ((decl_type = match_type())) {
        typed_declaration(decl_type);
    } else {
        if (match(TOKEN_PRINT)) {
            print_statement();
        } else if (match(TOKEN_IF)) {
            if_statement();
        } else if (match(TOKEN_WHILE)) {
            while_statement();
        } else if (match(TOKEN_FOR)) {
            for_statement();
        } else if (match(TOKEN_RETURN)) {
            return_statement();
        } else {
            expression(PREC_ASSIGNMENT);
            consume(TOKEN_SEMICOLON, "Expected ';' after declaration.");
        }
    }
}

static void module() {
    write_string("(module\n");
    write_string("(import \"core\" \"print\" (func $print_i32 (param i32)))\n");
    write_string("(import \"core\" \"print\" (func $print_i64 (param i64)))\n");
    write_string("(import \"core\" \"putc\" (func $putc (param i32)))\n");
    write_string("(import \"core\" \"puts\" (func $puts (param i32)))\n");
    write_string("(memory $0 1)\n");

    // create local for putc
    local_t *putc_local = &compiler.locals[compiler.local_count++];
    putc_local->name.start = "putc";
    putc_local->name.length = 4;
    putc_local->type = create_type(TYPE_FUN, false, compiler.fundef_count);
    putc_local->is_on_stack = false;
    putc_local->depth = 0;

    fundef_t *putc_fundef = &compiler.fundefs[compiler.fundef_count++];
    putc_fundef->name.start = "putc";
    putc_fundef->name.length = 4;
    putc_fundef->return_type = TYPE_VOID;
    putc_fundef->param_count = 1;
    putc_fundef->params = (fielddef_t *)malloc(sizeof(fielddef_t));
    putc_fundef->params[0].name.start = "c";
    putc_fundef->params[0].name.length = 1;
    putc_fundef->params[0].type = TYPE_I32;

    // create local for puts
    local_t *puts_local = &compiler.locals[compiler.local_count++];
    puts_local->name.start = "puts";
    puts_local->name.length = 4;
    puts_local->type = create_type(TYPE_FUN, false, compiler.fundef_count);
    puts_local->is_on_stack = false;
    puts_local->depth = 0;

    fundef_t *puts_fundef = &compiler.fundefs[compiler.fundef_count++];
    puts_fundef->name.start = "puts";
    puts_fundef->name.length = 4;
    puts_fundef->return_type = TYPE_VOID;
    puts_fundef->param_count = 1;
    puts_fundef->params = (fielddef_t *)malloc(sizeof(fielddef_t) * 1);
    puts_fundef->params[0].name.start = "ptr";
    puts_fundef->params[0].name.length = 3;
    puts_fundef->params[0].type = TYPE_I32;

    local_t *heap_base_local = &compiler.locals[compiler.local_count++];
    heap_base_local->name.start = "__heap_base";
    heap_base_local->name.length = 11;
    heap_base_local->type = TYPE_CHAR | TYPE_POINTER_FLAG | TYPE_ABSOLUTE_FLAG;
    heap_base_local->is_on_stack = true;
    heap_base_local->depth = 0;
    heap_base_local->frame_index = 0;

    int heap_base = 16384;
    lexer_t lsnap = lexer_snapshot();
    parser_t psnap = parser_snapshot();

    compiler.no_op = true;
    while (parser.current.type != TOKEN_EOF) {
        declaration();
    }

    heap_base = 16384 + compiler.data_offset;
    heap_base_local->frame_index = heap_base;

    compiler.fundef_count = 0;
    compiler.structdef_count = 0;
    compiler.local_count = 3;
    compiler.const_count = 0;
    compiler.data_offset = 0;
    compiler.loop_counter = 0;

    compiler.no_op = false;
    lexer_restore(&lsnap);
    parser_restore(&psnap);

    while (parser.current.type != TOKEN_EOF) {
        declaration();
    }

    for (int i = 0; i < compiler.const_count; i++) {
        const_t *const_value = &compiler.consts[i];
        write_string("(data (i32.const ");
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d", 16384 + const_value->offset);
        write_string(buffer);
        write_string(") \"");

        if (is_type(const_value->type, TYPE_CHAR)) {
            const char *str = const_value->as.string;
            for (int j = 0; j < const_value->size; j++) {
                char c = str[j];
                if (c == '"' || c == '\\') {
                    write_string((char[]){c, '\0'});
                } else if (c >= 32 && c <= 126) {
                    write_string((char[]){c, '\0'});
                } else {
                    snprintf(buffer, sizeof(buffer), "\\%02x",
                             (unsigned char)c);
                    write_string(buffer);
                }
            }

            write_string("\\00");
        } else if (is_pointer(const_value->type)) {
            if (const_value->is_local) {
                printf("Writing pointer to local: %d\n",
                       const_value->as.local->frame_index);
                for (int j = 0; j < 4; ++j) {
                    unsigned char byte =
                        (const_value->as.local->frame_index >> (j * 8)) & 0xFF;
                    snprintf(buffer, sizeof(buffer), "\\%02x", byte);
                    write_string(buffer);
                }
            } else {
                printf("Writing pointer constant: %d\n",
                       const_value->as.const_value);
                for (int j = 0; j < 4; ++j) {
                    unsigned char byte =
                        (const_value->as.const_value >> (j * 8)) & 0xFF;
                    snprintf(buffer, sizeof(buffer), "\\%02x", byte);
                    write_string(buffer);
                }
            }
        } else {
            error("Unsupported constant type for data segment.");
        }

        write_string("\")\n");
    }

    write_string("(global $__sp (mut i32) (i32.const 16384))\n");
    write_string("(global $__heap_base i32 (i32.const ");
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%d", heap_base);
    write_string(buffer);
    write_string("))\n");

    // write_string("(start $main)\n");
    write_string("(export \"memory\" (memory $0))\n");
    write_string("(export \"main\" (func $main))\n");
    write_string(")");
}

char *compile(const char *source) {
    compiler_free();

    lexer_init(source);
    compiler_init();

    parser.had_error = false;

    advance();
    module();

    *compiler.current = '\0';
    return parser.had_error ? NULL : compiler.buffer;
}
