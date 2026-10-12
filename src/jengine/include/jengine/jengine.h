/*
 * Jengine — small JavaScript engine.
 *
 * Public API: tagged values, context lifecycle, evaluation.
 *
 * Values are 64-bit NaN-boxed (Lua/QuickJS style): an ordinary IEEE-754
 * double is stored verbatim, and every non-number is stored in the quiet
 * NaN space so doubles keep full 64-bit precision.
 *
 *   boxed  : (v & 0x7FF8000000000000) == 0x7FF8000000000000
 *   tag    : (v >> 48) & 0x7
 *   payload:  v & 0x0000FFFFFFFFFFFF   (48 bits — enough for a pointer)
 */

#ifndef JENGINE_JENGINE_H
#define JENGINE_JENGINE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==============================================================
 * Token types (shared between the lexer and parser)
 * ============================================================== */

typedef enum {
    TK_EOF      = 0,
    TK_IDENT,
    TK_NUM,
    TK_STR,
    /* Keywords */
    TK_IF, TK_ELSE, TK_WHILE, TK_FOR, TK_DO,
    TK_BREAK, TK_CONTINUE, TK_RETURN,
    TK_VAR, TK_LET, TK_CONST,
    TK_FUNCTION, TK_TRUE, TK_FALSE, TK_NULL,
    TK_THIS, TK_SWITCH, TK_CASE, TK_DEFAULT,
    TK_TRY, TK_CATCH, TK_FINALLY, TK_THROW,
    TK_VOID, TK_DEBUGGER,
    TK_TYPEOF, TK_INSTANCEOF, TK_IN, TK_NEW, TK_DELETE,
    /* Operators */
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT, TK_POW,
    TK_EQ, TK_NE, TK_STRICT_EQ, TK_STRICT_NE, TK_LE, TK_GE, TK_LT, TK_GT,
    TK_AND, TK_OR, TK_NOT, TK_NULLISH,
    TK_INC, TK_DEC,
    TK_LSHIFT, TK_RSHIFT, TK_URSHIFT,
    TK_BITAND, TK_BITOR, TK_BITXOR, TK_BITNOT,
    TK_ASSIGN, TK_PLUS_ASSIGN, TK_MINUS_ASSIGN,
    TK_STAR_ASSIGN, TK_SLASH_ASSIGN, TK_PERCENT_ASSIGN,
    TK_LSHIFT_ASSIGN, TK_RSHIFT_ASSIGN, TK_URSHIFT_ASSIGN,
    TK_AND_ASSIGN, TK_OR_ASSIGN, TK_XOR_ASSIGN, TK_POW_ASSIGN,
    TK_QUESTION, TK_COLON, TK_COMMA,
    TK_DOT, TK_ELLIPSIS, TK_ARROW,
    /* Punctuation */
    TK_LPAREN, TK_RPAREN,
    TK_LBRACE, TK_RBRACE,
    TK_LBRACKET, TK_RBRACKET,
    TK_SEMICOLON,
} jengine_token_type_t;

typedef struct jengine_token {
    jengine_token_type_t type;
    const char* start;
    size_t length;
    int col;
    int line;
    union {
        double number;
        const char* str;   /* NUL-terminated decoded string (lexer owned) */
    } value;
} jengine_token_t;

/* ==============================================================
 * Tagged values
 * ============================================================== */

typedef uint64_t jengine_value;

#define JV_NAN_BASE    0x7FF8000000000000ULL
#define JV_TAG_MASK    0x0007000000000000ULL
#define JV_TAG_SHIFT   48
#define JV_PAYLOAD_MASK 0x0000FFFFFFFFFFFFULL

#define JENGINE_T_UNDEFINED 0
#define JENGINE_T_NULL      1
#define JENGINE_T_BOOLEAN   2
#define JENGINE_T_NUMBER    3
#define JENGINE_T_STRING    4
#define JENGINE_T_OBJECT    5
#define JENGINE_T_ARRAY     6
#define JENGINE_T_FUNCTION  7

static inline bool jengine_is_boxed(jengine_value v) {
    return (v & JV_NAN_BASE) == JV_NAN_BASE;
}
static inline int jengine_tag(jengine_value v) {
    return (int)((v >> JV_TAG_SHIFT) & 0x7);
}
static inline int jengine_value_type(jengine_value v) {
    return jengine_is_boxed(v) ? jengine_tag(v) : JENGINE_T_NUMBER;
}
#define JENGINE_TYPE(v) jengine_value_type(v)
#define JENGINE_PAYLOAD(v) ((v) & JV_PAYLOAD_MASK)

static inline void* jengine_value_ptr(jengine_value v) {
    return (void*)(uintptr_t)(v & JV_PAYLOAD_MASK);
}

static inline bool jengine_is_number(jengine_value v)    { return jengine_value_type(v) == JENGINE_T_NUMBER; }
static inline bool jengine_is_boolean(jengine_value v)   { return jengine_value_type(v) == JENGINE_T_BOOLEAN; }
static inline bool jengine_is_null(jengine_value v)      { return jengine_value_type(v) == JENGINE_T_NULL; }
static inline bool jengine_is_undefined(jengine_value v) { return jengine_value_type(v) == JENGINE_T_UNDEFINED; }
static inline bool jengine_is_string(jengine_value v)    { return jengine_value_type(v) == JENGINE_T_STRING; }
static inline bool jengine_is_object(jengine_value v)    { return jengine_value_type(v) == JENGINE_T_OBJECT; }
static inline bool jengine_is_array(jengine_value v)     { return jengine_value_type(v) == JENGINE_T_ARRAY; }
static inline bool jengine_is_function(jengine_value v)  { return jengine_value_type(v) == JENGINE_T_FUNCTION; }

static inline jengine_value jengine_make_boolean(bool b) {
    return JV_NAN_BASE | ((uint64_t)JENGINE_T_BOOLEAN << JV_TAG_SHIFT) | (b ? 1u : 0u);
}
static inline jengine_value jengine_make_null(void)      { return JV_NAN_BASE | ((uint64_t)JENGINE_T_NULL << JV_TAG_SHIFT); }
static inline jengine_value jengine_make_undefined(void) { return JV_NAN_BASE; }

static inline bool jengine_to_boolean(jengine_value v) {
    if (!jengine_is_boxed(v)) {
        double d;
        memcpy(&d, &v, sizeof d);
        return d != 0.0 && !(d != d);
    }
    int t = jengine_tag(v);
    if (t == JENGINE_T_BOOLEAN) return (v & 1) != 0;
    if (t == JENGINE_T_UNDEFINED || t == JENGINE_T_NULL) return false;
    return true;
}

/* Non-inline constructors */
jengine_value jengine_make_number(double n);
double jengine_to_number(jengine_value v);

/* ==============================================================
 * Context (opaque — internals live in jengine.c)
 * ============================================================== */

typedef struct jengine_context jengine_context;

#define JENGINE_CTX_MAGIC 0x4A454E47

jengine_context* jengine_create(void);
void jengine_destroy(jengine_context* ctx);

/* ==============================================================
 * Evaluation
 * ============================================================== */

int jengine_eval(jengine_context* ctx, const char* source, const char* filename,
                 jengine_value* result);
int jengine_eval_file(jengine_context* ctx, const char* filepath, jengine_value* result);

jengine_value jengine_call(jengine_context* ctx, jengine_value fn,
                           jengine_value this_val, jengine_value* args, int argc);

/* ==============================================================
 * Heap object constructors / accessors
 * ============================================================== */

jengine_value jengine_make_string(jengine_context* ctx, const char* str);
jengine_value jengine_make_string_len(jengine_context* ctx, const char* str, size_t len);
jengine_value jengine_make_object(jengine_context* ctx);
jengine_value jengine_make_array(jengine_context* ctx, size_t initial_capacity);

jengine_value jengine_get_property(jengine_context* ctx, jengine_value obj, const char* prop);
void jengine_set_property(jengine_context* ctx, jengine_value obj, const char* prop, jengine_value value);
jengine_value jengine_get_index(jengine_context* ctx, jengine_value arr, size_t index);
void jengine_set_index(jengine_context* ctx, jengine_value arr, size_t index, jengine_value value);

void jengine_array_push(jengine_context* ctx, jengine_value arr, jengine_value value);
size_t jengine_array_length(jengine_context* ctx, jengine_value arr);

/* ==============================================================
 * Native functions
 * ============================================================== */

typedef jengine_value (*jengine_native_fn)(jengine_context* ctx, jengine_value* args, int argc);
void jengine_register_native(jengine_context* ctx, const char* name, jengine_native_fn fn);

/* ==============================================================
 * Standard library / exceptions / GC / debug
 * ============================================================== */

void jengine_register_math(jengine_context* ctx);

bool jengine_has_exception(jengine_context* ctx);
jengine_value jengine_get_exception(jengine_context* ctx);
void jengine_clear_exception(jengine_context* ctx);
void jengine_throw(jengine_context* ctx, jengine_value error);

void jengine_gc(jengine_context* ctx);
size_t jengine_memory_usage(jengine_context* ctx);

void jengine_print_value(jengine_context* ctx, jengine_value v);
const char* jengine_value_to_json(jengine_context* ctx, jengine_value v);

#ifdef __cplusplus
}
#endif

#endif /* JENGINE_JENGINE_H */
