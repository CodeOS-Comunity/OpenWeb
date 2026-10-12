/*
 * Jengine — a small tree-walking JavaScript engine.
 *
 * Pipeline: lexer -> recursive-descent parser -> AST -> tree-walking
 * interpreter.  Values are 64-bit NaN-boxed (see jengine.h).
 *
 * Heap values (strings, objects, arrays, functions, environments) are
 * malloc'd and linked into the context so jengine_destroy can release them.
 * Parse-time data (AST nodes, identifiers, decoded strings) lives in an
 * arena that is also released with the context.
 */

#include "jengine/jengine.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <math.h>

/* ==============================================================
 * Small lexical helpers
 * ============================================================== */

static int jg_hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Append `cp` to `buf` (bounded by `cap`) as UTF-8; updates *out. */
static void jg_utf8_emit(char* buf, size_t cap, size_t* out, unsigned cp) {
    unsigned char bytes[4];
    int n;
    if (cp < 0x80) { bytes[0] = (unsigned char)cp; n = 1; }
    else if (cp < 0x800) {
        bytes[0] = (unsigned char)(0xC0 | (cp >> 6));
        bytes[1] = (unsigned char)(0x80 | (cp & 0x3F)); n = 2;
    } else if (cp < 0x10000) {
        bytes[0] = (unsigned char)(0xE0 | (cp >> 12));
        bytes[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        bytes[2] = (unsigned char)(0x80 | (cp & 0x3F)); n = 3;
    } else {
        bytes[0] = (unsigned char)(0xF0 | (cp >> 18));
        bytes[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
        bytes[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        bytes[3] = (unsigned char)(0x80 | (cp & 0x3F)); n = 4;
    }
    for (int i = 0; i < n; i++)
        if (*out < cap - 1) buf[(*out)++] = (char)bytes[i];
}

/* ==============================================================
 * Arena — parse-time allocations
 * ============================================================== */

typedef struct AChunk {
    struct AChunk* next;
    size_t used;
    size_t cap;
    char data[];
} AChunk;

typedef struct {
    AChunk* head;
} Arena;

static void* arena_alloc(Arena* a, size_t n) {
    if (n == 0) n = 1;
    n = (n + 7u) & ~(size_t)7u;
    AChunk* c = a->head;
    if (!c || c->used + n > c->cap) {
        size_t cap = n > 65536 ? n : 65536;
        c = (AChunk*)malloc(sizeof(AChunk) + cap);
        if (!c) return NULL;
        c->next = a->head;
        c->used = 0;
        c->cap = cap;
        a->head = c;
    }
    void* p = c->data + c->used;
    c->used += n;
    return p;
}

static char* arena_strndup(Arena* a, const char* s, size_t n) {
    char* p = (char*)arena_alloc(a, n + 1);
    if (p) { if (n) memcpy(p, s, n); p[n] = 0; }
    return p;
}
static char* arena_strdup(Arena* a, const char* s) {
    return arena_strndup(a, s, strlen(s));
}

static void arena_free(Arena* a) {
    AChunk* c = a->head;
    while (c) { AChunk* nx = c->next; free(c); c = nx; }
    a->head = NULL;
}

/* ==============================================================
 * Heap object layouts
 * ============================================================== */

typedef struct jg_heap {
    struct jg_heap* next;
    int kind;
} jg_heap;

typedef struct {
    jg_heap h;
    size_t len;
    char* data;
} jg_string;

typedef struct {
    jg_heap h;
    int count;
    int cap;
    char** keys;
    jengine_value* vals;
} jg_object;

typedef struct {
    jg_heap h;
    size_t len;
    size_t cap;
    jengine_value* items;
} jg_array;

typedef struct jg_env {
    jg_heap h;
    struct jg_env* parent;
    int count;
    int cap;
    char** names;
    jengine_value* vals;
} jg_env;

typedef struct Node Node;

typedef jengine_value (*jg_native_fn)(struct jengine_context* ctx,
                                      jengine_value this_val,
                                      jengine_value* args, int argc);

typedef struct jg_func {
    jg_heap h;
    int kind;                /* 0 user, 1 internal native, 2 public native, 3 bound */
    char* name;
    Node* body;
    char** params;
    Node** param_defaults;   /* parallel to params; NULL entries = no default */
    int param_count;
    int rest_index;          /* index of a rest parameter, or -1 */
    jg_env* closure;
    bool is_arrow;
    jg_native_fn nfn;
    jengine_native_fn pfn;
    jg_object* props;      /* static / attached properties (Object.keys, ...) */
    /* bound function (kind 3) */
    jengine_value bound_target;
    jengine_value bound_this;
    jengine_value* bound_args;
    int bound_argc;
} jg_func;

typedef struct Interp {
    struct jengine_context* ctx;
    jg_env* cur;
    jengine_value this_val;
    int flow;
    jengine_value flow_val;
    int call_depth;
    jengine_value last_value;
} Interp;

struct jengine_context {
    int magic;
    jg_heap* heap_all;
    size_t heap_used;
    size_t heap_size;
    size_t gc_threshold;
    size_t allocations;
    Arena arena;
    jg_env* global_env;
    jengine_value global_this;
    jengine_value exception;
    bool has_exception;
    Interp* I;
    /* cached builtin tables */
    jengine_value arr_methods;
    jengine_value str_methods;
    jengine_value num_methods;
    jengine_value math_obj;
    jengine_value json_obj;
    jengine_value console_obj;
    jengine_value func_methods;
    jengine_value obj_methods;
    char* json_scratch;
};

enum {
    FLOW_NORMAL = 0,
    FLOW_BREAK,
    FLOW_CONTINUE,
    FLOW_RETURN,
    FLOW_THROW
};

#define UNDEF jengine_make_undefined()
#define NULLV jengine_make_null()
#define BOOLV(b) jengine_make_boolean(b)

/* ==============================================================
 * Allocation helpers
 * ============================================================== */

static char* xstrdup(const char* s) {
    size_t n = strlen(s);
    char* p = (char*)malloc(n + 1);
    if (p) memcpy(p, s, n + 1);
    return p;
}

static jengine_value mkbox(int tag, uintptr_t payload) {
    return JV_NAN_BASE | ((uint64_t)tag << JV_TAG_SHIFT)
                       | ((uint64_t)payload & JV_PAYLOAD_MASK);
}
static void* boxptr(jengine_value v) {
    return (void*)(uintptr_t)(v & JV_PAYLOAD_MASK);
}

jengine_value jengine_make_number(double n) {
    union { double d; uint64_t u; } u;
    u.d = n;
    /* Canonicalise NaN to a signalling pattern that stays outside the boxed
     * space, so it is still recognised as a number. */
    if (u.d != u.d) u.u = 0x7FF4000000000000ULL;
    return u.u;
}

double jengine_to_number(jengine_value v) {
    if (jengine_is_boxed(v)) {
        int t = jengine_tag(v);
        if (t == JENGINE_T_BOOLEAN) return (v & 1) ? 1.0 : 0.0;
        return 0.0;
    }
    union { double d; uint64_t u; } u;
    u.u = v;
    return u.d;
}

static jg_string* as_string(jengine_value v) { return (jg_string*)boxptr(v); }
static jg_object* as_object(jengine_value v) { return (jg_object*)boxptr(v); }
static jg_array*  as_array(jengine_value v)  { return (jg_array*)boxptr(v); }
static jg_func*   as_func(jengine_value v)   { return (jg_func*)boxptr(v); }

static const char* string_data(jengine_value v) {
    jg_string* s = as_string(v);
    return (s && s->data) ? s->data : "";
}

static void* heap_alloc(struct jengine_context* ctx, size_t size, int kind) {
    jg_heap* h = (jg_heap*)calloc(1, size);
    if (!h) return NULL;
    h->kind = kind;
    h->next = ctx->heap_all;
    ctx->heap_all = h;
    ctx->heap_used += size;
    ctx->allocations++;
    return h;
}

jengine_value jengine_make_string_len(struct jengine_context* ctx,
                                      const char* str, size_t len) {
    jg_string* s = (jg_string*)heap_alloc(ctx, sizeof(jg_string), 0);
    if (!s) return UNDEF;
    s->data = (char*)malloc(len + 1);
    if (!s->data) { s->len = 0; return mkbox(JENGINE_T_STRING, (uintptr_t)s); }
    if (len) memcpy(s->data, str, len);
    s->data[len] = 0;
    s->len = len;
    return mkbox(JENGINE_T_STRING, (uintptr_t)s);
}
jengine_value jengine_make_string(struct jengine_context* ctx, const char* str) {
    return jengine_make_string_len(ctx, str, str ? strlen(str) : 0);
}

jengine_value jengine_make_object(struct jengine_context* ctx) {
    jg_object* o = (jg_object*)heap_alloc(ctx, sizeof(jg_object), 1);
    if (!o) return UNDEF;
    return mkbox(JENGINE_T_OBJECT, (uintptr_t)o);
}

jengine_value jengine_make_array(struct jengine_context* ctx, size_t initial_capacity) {
    jg_array* a = (jg_array*)heap_alloc(ctx, sizeof(jg_array), 2);
    if (!a) return UNDEF;
    if (initial_capacity == 0) initial_capacity = 4;
    a->items = (jengine_value*)calloc(initial_capacity, sizeof(jengine_value));
    if (!a->items) { a->cap = 0; a->len = 0; return mkbox(JENGINE_T_ARRAY, (uintptr_t)a); }
    a->cap = initial_capacity;
    a->len = 0;
    return mkbox(JENGINE_T_ARRAY, (uintptr_t)a);
}

static jengine_value make_func_value(jg_func* f) {
    return mkbox(JENGINE_T_FUNCTION, (uintptr_t)f);
}

static jg_func* func_new(struct jengine_context* ctx) {
    return (jg_func*)heap_alloc(ctx, sizeof(jg_func), 4);
}

static jg_env* env_new(Interp* I, jg_env* parent) {
    jg_env* e = (jg_env*)heap_alloc(I->ctx, sizeof(jg_env), 3);
    if (!e) return NULL;
    e->parent = parent;
    return e;
}

static void env_def(jg_env* e, const char* name, jengine_value val) {
    if (!e) return;
    for (int i = 0; i < e->count; i++) {
        if (strcmp(e->names[i], name) == 0) { e->vals[i] = val; return; }
    }
    if (e->count >= e->cap) {
        int nc = e->cap ? e->cap * 2 : 4;
        char** nk = (char**)realloc(e->names, (size_t)nc * sizeof(char*));
        if (!nk) return;
        e->names = nk;
        jengine_value* nv = (jengine_value*)realloc(e->vals, (size_t)nc * sizeof(jengine_value));
        if (!nv) return;
        e->vals = nv;
        e->cap = nc;
    }
    e->names[e->count] = xstrdup(name);
    e->vals[e->count] = val;
    e->count++;
}

static jengine_value* env_find(jg_env* e, const char* name) {
    for (; e; e = e->parent) {
        for (int i = 0; i < e->count; i++) {
            if (strcmp(e->names[i], name) == 0) return &e->vals[i];
        }
    }
    return NULL;
}

/* ==============================================================
 * Objects / arrays
 * ============================================================== */

static void obj_set_raw(jengine_value objv, const char* key, jengine_value val) {
    jg_object* o = as_object(objv);
    if (!o) return;
    for (int i = 0; i < o->count; i++) {
        if (strcmp(o->keys[i], key) == 0) { o->vals[i] = val; return; }
    }
    if (o->count >= o->cap) {
        int nc = o->cap ? o->cap * 2 : 4;
        char** nk = (char**)realloc(o->keys, (size_t)nc * sizeof(char*));
        if (!nk) return;
        o->keys = nk;
        jengine_value* nv = (jengine_value*)realloc(o->vals, (size_t)nc * sizeof(jengine_value));
        if (!nv) return;
        o->vals = nv;
        o->cap = nc;
    }
    o->keys[o->count] = xstrdup(key);
    o->vals[o->count] = val;
    o->count++;
}

static bool obj_get_raw(jengine_value objv, const char* key, jengine_value* out) {
    jg_object* o = as_object(objv);
    if (!o) return false;
    for (int i = 0; i < o->count; i++) {
        if (strcmp(o->keys[i], key) == 0) { *out = o->vals[i]; return true; }
    }
    return false;
}

static void arr_reserve(jg_array* a, size_t need) {
    if (need <= a->cap) return;
    size_t nc = a->cap ? a->cap : 4;
    while (nc < need) nc *= 2;
    jengine_value* ni = (jengine_value*)realloc(a->items, nc * sizeof(jengine_value));
    if (!ni) return;
    a->items = ni;
    a->cap = nc;
}

static void arr_set_len(jg_array* a, size_t idx, jengine_value v) {
    arr_reserve(a, idx + 1);
    while (a->len < idx) a->items[a->len++] = UNDEF;
    if (a->len == idx) a->len = idx + 1;
    a->items[idx] = v;
}

void jengine_array_push(struct jengine_context* ctx, jengine_value arrv, jengine_value value) {
    jg_array* a = as_array(arrv);
    (void)ctx;
    if (!a) return;
    arr_reserve(a, a->len + 1);
    a->items[a->len++] = value;
}

size_t jengine_array_length(struct jengine_context* ctx, jengine_value arrv) {
    jg_array* a = as_array(arrv);
    (void)ctx;
    return a ? a->len : 0;
}

static jengine_value array_get(jg_array* a, size_t i) {
    return (a && i < a->len) ? a->items[i] : UNDEF;
}

static bool parse_index_key(const char* key, size_t keylen, size_t* out) {
    if (keylen == 0) return false;
    size_t v = 0;
    for (size_t i = 0; i < keylen; i++) {
        if (key[i] < '0' || key[i] > '9') return false;
        v = v * 10 + (size_t)(key[i] - '0');
        if (v > ((size_t)1 << 40)) return false;
    }
    *out = v;
    return true;
}

/* ==============================================================
 * Dynamic string buffer
 * ============================================================== */

typedef struct {
    char* data;
    size_t len;
    size_t cap;
} SB;

static void sb_init(SB* s) { s->data = NULL; s->len = 0; s->cap = 0; }
static void sb_reserve(SB* s, size_t need) {
    if (need <= s->cap) return;
    size_t nc = s->cap ? s->cap : 32;
    while (nc < need) nc *= 2;
    char* p = (char*)realloc(s->data, nc);
    if (!p) return;
    s->data = p;
    s->cap = nc;
}
static void sb_putc(SB* s, char c) {
    sb_reserve(s, s->len + 2);
    if (!s->data) return;
    s->data[s->len++] = c;
    s->data[s->len] = 0;
}
static void sb_putn(SB* s, const char* str, size_t n) {
    if (!str || n == 0) return;
    sb_reserve(s, s->len + n + 1);
    if (!s->data) return;
    memcpy(s->data + s->len, str, n);
    s->len += n;
    s->data[s->len] = 0;
}
static void sb_puts(SB* s, const char* str) { if (str) sb_putn(s, str, strlen(str)); }
static char* sb_take(SB* s) {
    if (!s->data) return xstrdup("");
    char* p = s->data;
    s->data = NULL; s->len = 0; s->cap = 0;
    return p;
}

/* forward declarations */
static jengine_value ev(Interp* I, Node* n);
static void exec_stmt(Interp* I, Node* n);
static void exec_block(Interp* I, Node* n);
static jengine_value call_func(Interp* I, jg_func* f, jengine_value thisv,
                               jengine_value* args, int argc);
static bool truthy(jengine_value v);
static jengine_value get_property(Interp* I, jengine_value obj, const char* key);
static jengine_value get_property_key(Interp* I, jengine_value obj, jengine_value key);
static void set_property(Interp* I, jengine_value obj, const char* key, jengine_value val);
static void set_property_key(Interp* I, jengine_value obj, jengine_value key, jengine_value val);
static void set_exception(Interp* I, jengine_value val);
static jengine_value make_native(Interp* I, const char* name, jg_native_fn fn);
static double to_number(Interp* I, jengine_value v);
static jengine_value m_error_toString(struct jengine_context* ctx, jengine_value t,
                                      jengine_value* a, int argc);

/* Build a JavaScript Error-like object with name/message/toString. */
static jengine_value jg_make_error(Interp* I, const char* name, const char* msg) {
    struct jengine_context* ctx = I->ctx;
    jengine_value e = jengine_make_object(ctx);
    if (jengine_is_undefined(e)) return e;
    obj_set_raw(e, "name", jengine_make_string(ctx, name ? name : "Error"));
    obj_set_raw(e, "message", jengine_make_string(ctx, msg ? msg : ""));
    obj_set_raw(e, "toString", make_native(I, "toString", m_error_toString));
    return e;
}

/* ==============================================================
 * Conversions
 * ============================================================== */

static char* num_to_cstr(double d) {
    char* buf = (char*)malloc(48);
    if (!buf) return NULL;
    if (d != d) { strcpy(buf, "NaN"); return buf; }
    if (d == 0.0) { strcpy(buf, "0"); return buf; }
    if (isinf(d)) { strcpy(buf, d < 0 ? "-Infinity" : "Infinity"); return buf; }
    double ip;
    if (modf(d, &ip) == 0.0 && d >= -1e15 && d <= 1e15) {
        snprintf(buf, 48, "%.0f", d);
        return buf;
    }
    for (int prec = 1; prec <= 17; prec++) {
        snprintf(buf, 48, "%.*g", prec, d);
        if (strtod(buf, NULL) == d) break;
    }
    return buf;
}

static double to_number(Interp* I, jengine_value v) {
    (void)I;
    switch (jengine_value_type(v)) {
        case JENGINE_T_NUMBER: return jengine_to_number(v);
        case JENGINE_T_BOOLEAN: return (v & 1) ? 1.0 : 0.0;
        case JENGINE_T_NULL: return 0.0;
        case JENGINE_T_UNDEFINED: return 0.0;  /* NaN in JS; keep simple */
        case JENGINE_T_STRING: {
            const char* s = string_data(v);
            if (!*s) return 0.0;
            char* end = NULL;
            double d = strtod(s, &end);
            while (end && *end && isspace((unsigned char)*end)) end++;
            return (end && *end) ? 0.0 : d;
        }
        default: return 0.0;
    }
}

static void append_js_string(SB* sb, jengine_value v) {
    switch (jengine_value_type(v)) {
        case JENGINE_T_STRING:  sb_puts(sb, string_data(v)); break;
        case JENGINE_T_NUMBER: {
            char* s = num_to_cstr(jengine_to_number(v));
            if (s) { sb_puts(sb, s); free(s); }
            break;
        }
        case JENGINE_T_BOOLEAN: sb_puts(sb, (v & 1) ? "true" : "false"); break;
        case JENGINE_T_NULL:      sb_puts(sb, "null"); break;
        case JENGINE_T_UNDEFINED: sb_puts(sb, "undefined"); break;
        case JENGINE_T_ARRAY: {
            jg_array* a = as_array(v);
            if (a) {
                for (size_t i = 0; i < a->len; i++) {
                    if (i) sb_putc(sb, ',');
                    jengine_value x = a->items[i];
                    int t = jengine_value_type(x);
                    if (t != JENGINE_T_NULL && t != JENGINE_T_UNDEFINED)
                        append_js_string(sb, x);
                }
            }
            break;
        }
        case JENGINE_T_FUNCTION: {
            jg_func* f = as_func(v);
            sb_puts(sb, "function ");
            if (f && f->name) sb_puts(sb, f->name);
            sb_puts(sb, "() { [native code] }");
            break;
        }
        default: sb_puts(sb, "[object Object]"); break;
    }
}

static char* value_to_string(Interp* I, jengine_value v) {
    (void)I;
    SB sb; sb_init(&sb);
    append_js_string(&sb, v);
    return sb_take(&sb);
}

/* JS-like textual form used for concatenation */
/* debug print used by jengine_print_value / console.log */
static void append_repr(SB* sb, jengine_value v, int depth) {
    if (depth > 8) { sb_puts(sb, "..."); return; }
    switch (jengine_value_type(v)) {
        case JENGINE_T_STRING: sb_putc(sb, '"'); sb_puts(sb, string_data(v)); sb_putc(sb, '"'); break;
        case JENGINE_T_NUMBER: {
            double d = jengine_to_number(v);
            char* s;
            if (modf(d, &(double){0}) == 0.0 && isfinite(d)) {
                char tmp[48]; snprintf(tmp, sizeof tmp, "%.1f", d); s = xstrdup(tmp);
            } else {
                s = num_to_cstr(d);
            }
            if (s) { sb_puts(sb, s); free(s); }
            break;
        }
        case JENGINE_T_BOOLEAN: sb_puts(sb, (v & 1) ? "true" : "false"); break;
        case JENGINE_T_NULL:      sb_puts(sb, "null"); break;
        case JENGINE_T_UNDEFINED: sb_puts(sb, "undefined"); break;
        case JENGINE_T_ARRAY: {
            jg_array* a = as_array(v);
            sb_putc(sb, '[');
            if (a) for (size_t i = 0; i < a->len; i++) {
                if (i) sb_puts(sb, ", ");
                append_repr(sb, a->items[i], depth + 1);
            }
            sb_putc(sb, ']');
            break;
        }
        case JENGINE_T_OBJECT: {
            jg_object* o = as_object(v);
            sb_putc(sb, '{');
            if (o) for (int i = 0; i < o->count; i++) {
                if (i) sb_puts(sb, ", ");
                sb_puts(sb, o->keys[i]);
                sb_puts(sb, ": ");
                append_repr(sb, o->vals[i], depth + 1);
            }
            sb_putc(sb, '}');
            break;
        }
        case JENGINE_T_FUNCTION: {
            jg_func* f = as_func(v);
            sb_puts(sb, "[Function: ");
            sb_puts(sb, (f && f->name) ? f->name : "anonymous");
            sb_putc(sb, ']');
            break;
        }
    }
}

static char* debug_str(jengine_value v) {
    SB sb; sb_init(&sb);
    if (jengine_is_string(v)) {
        sb_puts(&sb, string_data(v));       /* top-level strings print raw */
    } else if (jengine_is_number(v)) {
        double d = jengine_to_number(v);
        char* s = num_to_cstr(d);
        if (s) { sb_puts(&sb, s); free(s); }
    } else {
        append_repr(&sb, v, 0);
    }
    return sb_take(&sb);
}

/* boolean coercion (JS semantics) */
static bool truthy(jengine_value v) {
    switch (jengine_value_type(v)) {
        case JENGINE_T_UNDEFINED:
        case JENGINE_T_NULL:   return false;
        case JENGINE_T_BOOLEAN: return (v & 1) != 0;
        case JENGINE_T_NUMBER: {
            double d = jengine_to_number(v);
            return d != 0.0 && d == d;
        }
        case JENGINE_T_STRING: return string_data(v)[0] != 0;
        default: return true;
    }
}

static void set_exception(Interp* I, jengine_value val) {
    I->ctx->exception = val;
    I->ctx->has_exception = true;
    I->flow = FLOW_THROW;
    I->flow_val = val;
}
/* ==============================================================
 * Lexer
 * ============================================================== */

typedef struct {
    const char* src;
    size_t len;
    size_t pos;
    size_t line_start;
    int line;
    int col;
    bool nl_before;
    bool err;
    char errmsg[256];
    char sbuf[4096];
    jengine_token_t tok;
} Lexer;

static void lex_error(Lexer* lx, const char* fmt, ...) {
    if (lx->err) return;
    lx->err = true;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(lx->errmsg, sizeof lx->errmsg, fmt, ap);
    va_end(ap);
}

static void lex_init(Lexer* lx, const char* src) {
    memset(lx, 0, sizeof *lx);
    lx->src = src;
    lx->len = strlen(src);
    lx->line = 1;
    lx->line_start = 0;
    lx->col = 1;
}

typedef struct { const char* word; int type; } Keyword;
static const Keyword g_keywords[] = {
    {"if", TK_IF}, {"else", TK_ELSE}, {"while", TK_WHILE}, {"for", TK_FOR},
    {"do", TK_DO}, {"break", TK_BREAK}, {"continue", TK_CONTINUE},
    {"return", TK_RETURN}, {"var", TK_VAR}, {"let", TK_LET}, {"const", TK_CONST},
    {"function", TK_FUNCTION}, {"true", TK_TRUE}, {"false", TK_FALSE},
    {"null", TK_NULL}, {"this", TK_THIS}, {"switch", TK_SWITCH},
    {"case", TK_CASE}, {"default", TK_DEFAULT}, {"try", TK_TRY},
    {"catch", TK_CATCH}, {"finally", TK_FINALLY}, {"throw", TK_THROW},
    {"void", TK_VOID}, {"debugger", TK_DEBUGGER}, {"typeof", TK_TYPEOF},
    {"instanceof", TK_INSTANCEOF}, {"in", TK_IN}, {"new", TK_NEW},
    {"delete", TK_DELETE},
};
#define NUM_KEYWORDS ((int)(sizeof g_keywords / sizeof g_keywords[0]))

static bool is_ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$';
}
static bool is_ident_char(char c) {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}
static bool is_digit(char c) { return c >= '0' && c <= '9'; }

static void lex_next(Lexer* lx) {
    lx->nl_before = false;

    /* skip whitespace and comments */
    for (;;) {
        if (lx->pos >= lx->len) {
            lx->tok.type = TK_EOF;
            lx->tok.start = lx->src + lx->len;
            lx->tok.length = 0;
            lx->tok.line = lx->line;
            lx->tok.col = lx->col;
            return;
        }
        char c = lx->src[lx->pos];
        if (c == '\n') {
            lx->nl_before = true;
            lx->pos++;
            lx->line++;
            lx->line_start = lx->pos;
            lx->col = 1;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
            lx->pos++;
            lx->col++;
            continue;
        }
        if (c == '/' && lx->pos + 1 < lx->len) {
            char c2 = lx->src[lx->pos + 1];
            if (c2 == '/') {
                lx->pos += 2;
                while (lx->pos < lx->len && lx->src[lx->pos] != '\n') lx->pos++;
                continue;
            }
            if (c2 == '*') {
                lx->pos += 2;
                while (lx->pos + 1 < lx->len &&
                       !(lx->src[lx->pos] == '*' && lx->src[lx->pos + 1] == '/')) {
                    if (lx->src[lx->pos] == '\n') {
                        lx->nl_before = true;
                        lx->line++;
                        lx->line_start = lx->pos + 1;
                    }
                    lx->pos++;
                }
                if (lx->pos + 1 < lx->len) lx->pos += 2;
                else lx->pos = lx->len;
                continue;
            }
        }
        break;
    }

    size_t start = lx->pos;
    lx->tok.line = lx->line;
    lx->tok.col = (int)(lx->pos - lx->line_start) + 1;
    lx->tok.start = lx->src + start;
    char c = lx->src[lx->pos];

    /* identifiers / keywords */
    if (is_ident_start(c)) {
        size_t p = lx->pos + 1;
        while (p < lx->len && is_ident_char(lx->src[p])) p++;
        size_t n = p - start;
        lx->tok.start = lx->src + start;
        lx->tok.length = n;
        lx->tok.type = TK_IDENT;
        for (int i = 0; i < NUM_KEYWORDS; i++) {
            if (strlen(g_keywords[i].word) == n &&
                strncmp(g_keywords[i].word, lx->src + start, n) == 0) {
                lx->tok.type = g_keywords[i].type;
                break;
            }
        }
        lx->pos = p;
        lx->col += (int)n;
        return;
    }

    /* numbers */
    if (is_digit(c) || (c == '.' && lx->pos + 1 < lx->len && is_digit(lx->src[lx->pos + 1]))) {
        double val = 0.0;
        if (c == '0' && lx->pos + 1 < lx->len &&
            (lx->src[lx->pos + 1] == 'x' || lx->src[lx->pos + 1] == 'X')) {
            size_t p = lx->pos + 2;
            uint64_t v = 0;
            while (p < lx->len && isxdigit((unsigned char)lx->src[p])) {
                char h = lx->src[p];
                int dv = (h <= '9') ? h - '0' : (tolower((unsigned char)h) - 'a' + 10);
                v = v * 16 + (uint64_t)dv;
                p++;
            }
            val = (double)v;
            lx->pos = p;
        } else if (c == '0' && lx->pos + 1 < lx->len &&
                   (lx->src[lx->pos + 1] == 'b' || lx->src[lx->pos + 1] == 'B')) {
            size_t p = lx->pos + 2;
            uint64_t v = 0;
            while (p < lx->len && (lx->src[p] == '0' || lx->src[p] == '1')) {
                v = v * 2 + (uint64_t)(lx->src[p] - '0');
                p++;
            }
            val = (double)v;
            lx->pos = p;
        } else if (c == '0' && lx->pos + 1 < lx->len &&
                   (lx->src[lx->pos + 1] == 'o' || lx->src[lx->pos + 1] == 'O')) {
            size_t p = lx->pos + 2;
            uint64_t v = 0;
            while (p < lx->len && lx->src[p] >= '0' && lx->src[p] <= '7') {
                v = v * 8 + (uint64_t)(lx->src[p] - '0');
                p++;
            }
            val = (double)v;
            lx->pos = p;
        } else {
            char* end = NULL;
            val = strtod(lx->src + lx->pos, &end);
            if (end == lx->src + lx->pos) {
                lex_error(lx, "invalid number");
                lx->pos++;
            } else {
                lx->pos = (size_t)(end - lx->src);
            }
        }
        lx->tok.type = TK_NUM;
        lx->tok.length = lx->pos - start;
        lx->tok.value.number = val;
        return;
    }

    /* strings */
    if (c == '"' || c == '\'') {
        char quote = c;
        size_t p = lx->pos + 1;
        size_t out = 0;
        bool closed = false;
        while (p < lx->len) {
            char ch = lx->src[p];
            if (ch == quote) { p++; closed = true; break; }
            if (ch == '\\' && p + 1 < lx->len) {
                char e = lx->src[p + 1];
                p += 2;
                if (e == 'x' && p + 1 < lx->len) {
                    int h = jg_hexval(lx->src[p]);
                    int l = jg_hexval(lx->src[p + 1]);
                    if (h >= 0 && l >= 0) {
                        jg_utf8_emit(lx->sbuf, sizeof lx->sbuf, &out, (unsigned)(h * 16 + l));
                        p += 2;
                        continue;
                    }
                }
                if (e == 'u') {
                    unsigned cp = 0;
                    bool ok = false;
                    if (p < lx->len && lx->src[p] == '{') {
                        unsigned v = 0; bool any = false;
                        size_t q = p + 1;
                        while (q < lx->len && lx->src[q] != '}') {
                            int h = jg_hexval(lx->src[q]);
                            if (h < 0) break;
                            v = v * 16u + (unsigned)h; any = true; q++;
                        }
                        if (any && q < lx->len && lx->src[q] == '}') { cp = v; p = q + 1; ok = true; }
                    } else if (p + 3 < lx->len) {
                        int h0 = jg_hexval(lx->src[p]), h1 = jg_hexval(lx->src[p + 1]);
                        int h2 = jg_hexval(lx->src[p + 2]), h3 = jg_hexval(lx->src[p + 3]);
                        if (h0 >= 0 && h1 >= 0 && h2 >= 0 && h3 >= 0) {
                            cp = (unsigned)(h0 * 4096 + h1 * 256 + h2 * 16 + h3);
                            p += 4; ok = true;
                        }
                    }
                    if (ok) { jg_utf8_emit(lx->sbuf, sizeof lx->sbuf, &out, cp); continue; }
                }
                char decoded = 0;
                switch (e) {
                    case 'n': decoded = '\n'; break;
                    case 't': decoded = '\t'; break;
                    case 'r': decoded = '\r'; break;
                    case 'b': decoded = '\b'; break;
                    case 'f': decoded = '\f'; break;
                    case 'v': decoded = '\v'; break;
                    case '0': decoded = '\0'; break;
                    case '\\': decoded = '\\'; break;
                    case '\'': decoded = '\''; break;
                    case '"': decoded = '"'; break;
                    case '/': decoded = '/'; break;
                    case '\n': continue;  /* line continuation */
                    default: decoded = e; break;
                }
                if (out < sizeof lx->sbuf - 1) lx->sbuf[out++] = decoded;
                continue;
            }
            if (out < sizeof lx->sbuf - 1) lx->sbuf[out++] = ch;
            p++;
        }
        if (!closed) lex_error(lx, "unterminated string");
        lx->sbuf[out] = 0;
        lx->tok.type = TK_STR;
        lx->tok.length = p - lx->pos;
        lx->tok.value.str = lx->sbuf;
        lx->pos = p;
        return;
    }

    /* operators / punctuation */
    #define TWO(a, b, t) if (c == (a) && lx->pos + 1 < lx->len && lx->src[lx->pos+1] == (b)) { \
        lx->tok.type = (t); lx->tok.length = 2; lx->pos += 2; lx->col += 2; return; }
    #define THREE(s, t) if (lx->pos + 2 < lx->len && !strncmp(lx->src + lx->pos, s, 3)) { \
        lx->tok.type = (t); lx->tok.length = 3; lx->pos += 3; lx->col += 3; return; }
    #define FOUR(s, t) if (lx->pos + 3 < lx->len && !strncmp(lx->src + lx->pos, s, 4)) { \
        lx->tok.type = (t); lx->tok.length = 4; lx->pos += 4; lx->col += 4; return; }

    FOUR(">>>=", TK_URSHIFT_ASSIGN);
    THREE("===", TK_STRICT_EQ);
    THREE("!==", TK_STRICT_NE);
    THREE(">>>", TK_URSHIFT);
    THREE("**=", TK_POW_ASSIGN);
    THREE("<<=", TK_LSHIFT_ASSIGN);
    THREE(">>=", TK_RSHIFT_ASSIGN);
    THREE("...", TK_ELLIPSIS);
    TWO('=', '=', TK_EQ);
    TWO('!', '=', TK_NE);
    TWO('<', '=', TK_LE);
    TWO('>', '=', TK_GE);
    TWO('&', '&', TK_AND);
    TWO('|', '|', TK_OR);
    TWO('?', '?', TK_NULLISH);
    TWO('+', '+', TK_INC);
    TWO('-', '-', TK_DEC);
    TWO('+', '=', TK_PLUS_ASSIGN);
    TWO('-', '=', TK_MINUS_ASSIGN);
    TWO('*', '=', TK_STAR_ASSIGN);
    TWO('/', '=', TK_SLASH_ASSIGN);
    TWO('%', '=', TK_PERCENT_ASSIGN);
    TWO('<', '<', TK_LSHIFT);
    TWO('>', '>', TK_RSHIFT);
    TWO('*', '*', TK_POW);
    TWO('=', '>', TK_ARROW);
    TWO('&', '=', TK_AND_ASSIGN);
    TWO('|', '=', TK_OR_ASSIGN);
    TWO('^', '=', TK_XOR_ASSIGN);

    lx->tok.length = 1;
    lx->pos++;
    lx->col++;
    switch (c) {
        case '+': lx->tok.type = TK_PLUS; break;
        case '-': lx->tok.type = TK_MINUS; break;
        case '*': lx->tok.type = TK_STAR; break;
        case '/': lx->tok.type = TK_SLASH; break;
        case '%': lx->tok.type = TK_PERCENT; break;
        case '<': lx->tok.type = TK_LT; break;
        case '>': lx->tok.type = TK_GT; break;
        case '=': lx->tok.type = TK_ASSIGN; break;
        case '!': lx->tok.type = TK_NOT; break;
        case '&': lx->tok.type = TK_BITAND; break;
        case '|': lx->tok.type = TK_BITOR; break;
        case '^': lx->tok.type = TK_BITXOR; break;
        case '~': lx->tok.type = TK_BITNOT; break;
        case '?': lx->tok.type = TK_QUESTION; break;
        case ':': lx->tok.type = TK_COLON; break;
        case ',': lx->tok.type = TK_COMMA; break;
        case '.': lx->tok.type = TK_DOT; break;
        case '(': lx->tok.type = TK_LPAREN; break;
        case ')': lx->tok.type = TK_RPAREN; break;
        case '{': lx->tok.type = TK_LBRACE; break;
        case '}': lx->tok.type = TK_RBRACE; break;
        case '[': lx->tok.type = TK_LBRACKET; break;
        case ']': lx->tok.type = TK_RBRACKET; break;
        case ';': lx->tok.type = TK_SEMICOLON; break;
        default:
            lex_error(lx, "unexpected character '%c'", c);
            lx->tok.type = TK_EOF;
            break;
    }
    #undef TWO
    #undef THREE
    #undef FOUR
}
/* ==============================================================
 * AST
 * ============================================================== */

typedef enum {
    ND_NUM, ND_STR, ND_BOOL, ND_NULL, ND_IDENT, ND_THIS,
    ND_ARRAY, ND_OBJECT, ND_FUNC, ND_MEMBER, ND_INDEX, ND_CALL,
    ND_UNARY, ND_UPDATE, ND_BINARY, ND_ASSIGN, ND_TERNARY, ND_SEQ,
    ND_VAR, ND_IF, ND_WHILE, ND_DOWHILE, ND_FOR, ND_FORIN, ND_RETURN,
    ND_BREAK, ND_CONTINUE, ND_BLOCK, ND_PROGRAM, ND_FUNC_DECL,
    ND_SWITCH, ND_CASE, ND_TRY, ND_THROW, ND_EMPTY, ND_SPREAD
} NodeType;

struct Node {
    int type;
    int line;
    union {
        double num;
        struct { const char* data; size_t len; } str;
        bool boolean;
        const char* ident;
        struct { Node** items; int count; int cap; } list;
        struct { const char** keys; Node** vals; int count; int cap; } obj;
        struct { Node* callee; Node** args; int argc; int cap; bool is_new; } call;
        struct { Node* obj; const char* prop; } member;
        struct { Node* obj; Node* index; } index;
        struct { int op; Node* operand; bool prefix; } unary;
        struct { int op; Node* left; Node* right; } binary;
        struct { int op; Node* target; Node* value; } assign;
        struct { Node* cond; Node* then; Node* els; } tern;
        struct { int decl_kind; Node** names; Node** inits; int count; int cap; } var;
        struct { Node* cond; Node* cons; Node* alt; } iff;
        struct { Node* init; Node* cond; Node* update; Node* body; } for_;
        struct { Node* left; Node* right; Node* body; } forin;
        struct { Node* value; } ret;
        struct { const char* name; const char** params; Node** param_defaults; int param_count; int rest_index; Node* body; } func;
        struct { Node* disc; Node** cases; int count; int cap; } sw;
        struct { Node* test; Node** stmts; int count; int cap; } cs;
        struct { Node* block; const char* param; Node* handler; Node* finalizer; } try_;
        struct { Node* arg; } thr;
        struct { Node* arg; } spread;
    } as;
};

/* ==============================================================
 * Parser
 * ============================================================== */

typedef struct {
    Lexer lx;
    struct jengine_context* ctx;
    bool err;
    char errmsg[256];
} Parser;

static void perr(Parser* p, const char* fmt, ...) {
    if (p->err) return;
    p->err = true;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->errmsg, sizeof p->errmsg, fmt, ap);
    va_end(ap);
}

static void next(Parser* p) { lex_next(&p->lx); }
static bool at(Parser* p, int t) { return (int)p->lx.tok.type == t; }

static bool eat(Parser* p, int t) {
    if (at(p, t)) { next(p); return true; }
    return false;
}

static void expect(Parser* p, int t, const char* what) {
    if (at(p, t)) { next(p); return; }
    perr(p, "expected %s at line %d", what, p->lx.tok.line);
}

static int peek_type(Parser* p) {
    Lexer tmp;
    memset(&tmp, 0, sizeof tmp);
    tmp.src = p->lx.src;
    tmp.len = p->lx.len;
    tmp.pos = p->lx.pos;
    tmp.line = p->lx.line;
    tmp.line_start = p->lx.line_start;
    tmp.col = p->lx.col;
    tmp.tok = p->lx.tok;
    lex_next(&tmp);
    return tmp.tok.type;
}

static Node* node_new(Parser* p, int type) {
    Node* n = (Node*)arena_alloc(&p->ctx->arena, sizeof(Node));
    if (!n) { perr(p, "out of memory"); return NULL; }
    memset(n, 0, sizeof(Node));
    n->type = type;
    n->line = p->lx.tok.line;
    return n;
}

static void nlist_push(struct jengine_context* ctx, Node*** items, int* count,
                       int* cap, Node* item) {
    if (*count >= *cap) {
        int nc = *cap ? *cap * 2 : 4;
        Node** ni = (Node**)arena_alloc(&ctx->arena, sizeof(Node*) * (size_t)nc);
        if (!ni) return;
        if (*items && *count) memcpy(ni, *items, sizeof(Node*) * (size_t)(*count));
        *items = ni;
        *cap = nc;
    }
    (*items)[(*count)++] = item;
}

static void slist_push(struct jengine_context* ctx, const char*** items, int* count,
                       int* cap, const char* s) {
    if (*count >= *cap) {
        int nc = *cap ? *cap * 2 : 4;
        const char** ni = (const char**)arena_alloc(&ctx->arena, sizeof(char*) * (size_t)nc);
        if (!ni) return;
        if (*items && *count) memcpy(ni, *items, sizeof(char*) * (size_t)(*count));
        *items = ni;
        *cap = nc;
    }
    (*items)[(*count)++] = s;
}

/* append a key/value pair sharing one count */
static void obj_push(struct jengine_context* ctx, Node* n, const char* key, Node* val) {
    int c = n->as.obj.count;
    if (c >= n->as.obj.cap) {
        int nc = n->as.obj.cap ? n->as.obj.cap * 2 : 4;
        const char** nk = (const char**)arena_alloc(&ctx->arena, sizeof(char*) * (size_t)nc);
        Node** nv = (Node**)arena_alloc(&ctx->arena, sizeof(Node*) * (size_t)nc);
        if (!nk || !nv) return;
        if (n->as.obj.count) {
            memcpy(nk, n->as.obj.keys, sizeof(char*) * (size_t)n->as.obj.count);
            memcpy(nv, n->as.obj.vals, sizeof(Node*) * (size_t)n->as.obj.count);
        }
        n->as.obj.keys = nk;
        n->as.obj.vals = nv;
        n->as.obj.cap = nc;
    }
    n->as.obj.keys[n->as.obj.count] = key;
    n->as.obj.vals[n->as.obj.count] = val;
    n->as.obj.count++;
}

static void var_push(Parser* p, Node* v, Node* id, Node* init) {
    int c = v->as.var.count;
    if (c >= v->as.var.cap) {
        int nc = v->as.var.cap ? v->as.var.cap * 2 : 4;
        Node** nn = (Node**)arena_alloc(&p->ctx->arena, sizeof(Node*) * (size_t)nc);
        Node** ni = (Node**)arena_alloc(&p->ctx->arena, sizeof(Node*) * (size_t)nc);
        if (!nn || !ni) return;
        if (c) {
            memcpy(nn, v->as.var.names, sizeof(Node*) * (size_t)c);
            memcpy(ni, v->as.var.inits, sizeof(Node*) * (size_t)c);
        }
        v->as.var.names = nn;
        v->as.var.inits = ni;
        v->as.var.cap = nc;
    }
    v->as.var.names[c] = id;
    v->as.var.inits[c] = init;
    v->as.var.count = c + 1;
}

static bool tok_is_name(Parser* p) {
    int t = p->lx.tok.type;
    return t == TK_IDENT || (t >= TK_IF && t <= TK_DELETE);
}

static const char* tok_name_copy(Parser* p) {
    return arena_strndup(&p->ctx->arena, p->lx.tok.start, p->lx.tok.length);
}

/* forward declarations */
static Node* parse_statement(Parser* p);
static Node* parse_expression(Parser* p);
static Node* parse_assignment(Parser* p);
static Node* parse_block(Parser* p);
static Node* parse_object_expr(Parser* p);
static Node* parse_function_expr(Parser* p);
static Node* parse_arrow_body(Parser* p, Node* fn);
static void parse_params_and_body(Parser* p, Node* fn);

/* ---------- helpers ---------- */

static Node* make_binary(Parser* p, int op, Node* l, Node* r) {
    Node* n = node_new(p, ND_BINARY);
    n->as.binary.op = op;
    n->as.binary.left = l;
    n->as.binary.right = r;
    return n;
}

static Node* make_ident(Parser* p, const char* name) {
    Node* n = node_new(p, ND_IDENT);
    n->as.ident = name;
    return n;
}

/* ---------- primary ---------- */

static Node* parse_primary(Parser* p) {
    if (p->err) return NULL;
    int t = p->lx.tok.type;

    if (t == TK_NUM) {
        double v = p->lx.tok.value.number;
        next(p);
        Node* n = node_new(p, ND_NUM);
        n->as.num = v;
        return n;
    }
    if (t == TK_STR) {
        const char* s = arena_strdup(&p->ctx->arena, p->lx.tok.value.str);
        next(p);
        Node* n = node_new(p, ND_STR);
        n->as.str.data = s;
        n->as.str.len = s ? strlen(s) : 0;
        return n;
    }
    if (t == TK_TRUE || t == TK_FALSE) {
        bool b = (t == TK_TRUE);
        next(p);
        Node* n = node_new(p, ND_BOOL);
        n->as.boolean = b;
        return n;
    }
    if (t == TK_NULL) { next(p); return node_new(p, ND_NULL); }
    if (t == TK_IDENT) {
        const char* name = tok_name_copy(p);
        next(p);
        return make_ident(p, name);
    }
    if (t == TK_THIS) { next(p); return node_new(p, ND_THIS); }

    if (t == TK_LPAREN) {
        next(p);
        Node* e = parse_expression(p);
        expect(p, TK_RPAREN, "')'");
        return e;
    }
    if (t == TK_LBRACKET) {
        next(p);
        Node* n = node_new(p, ND_ARRAY);
        while (!p->err && !at(p, TK_RBRACKET)) {
            Node* item;
            if (at(p, TK_ELLIPSIS)) {
                next(p);
                Node* arg = parse_assignment(p);
                item = node_new(p, ND_SPREAD);
                item->as.spread.arg = arg;
            } else {
                item = parse_assignment(p);
            }
            nlist_push(p->ctx, &n->as.list.items, &n->as.list.count, &n->as.list.cap, item);
            if (!eat(p, TK_COMMA)) break;
        }
        expect(p, TK_RBRACKET, "']'");
        return n;
    }
    if (t == TK_LBRACE) return parse_object_expr(p);
    if (t == TK_FUNCTION) return parse_function_expr(p);
    if (t == TK_NEW) {
        next(p);
        Node* callee = parse_primary(p);
        while (!p->err && (at(p, TK_DOT) || at(p, TK_LBRACKET))) {
            if (eat(p, TK_DOT)) {
                if (!tok_is_name(p)) { perr(p, "expected property name"); break; }
                const char* prop = tok_name_copy(p);
                next(p);
                Node* m = node_new(p, ND_MEMBER);
                m->as.member.obj = callee;
                m->as.member.prop = prop;
                callee = m;
            } else {
                next(p);
                Node* idx = parse_expression(p);
                expect(p, TK_RBRACKET, "']'");
                Node* ix = node_new(p, ND_INDEX);
                ix->as.index.obj = callee;
                ix->as.index.index = idx;
                callee = ix;
            }
        }
        Node* call = node_new(p, ND_CALL);
        call->as.call.callee = callee;
        call->as.call.is_new = true;
        if (at(p, TK_LPAREN)) {
            next(p);
            while (!p->err && !at(p, TK_RPAREN)) {
                Node* a = parse_assignment(p);
                nlist_push(p->ctx, &call->as.call.args, &call->as.call.argc,
                           &call->as.call.cap, a);
                if (!eat(p, TK_COMMA)) break;
            }
            expect(p, TK_RPAREN, "')'");
        }
        return call;
    }

    perr(p, "unexpected token at line %d", p->lx.tok.line);
    return NULL;
}

static Node* parse_call_member(Parser* p) {
    Node* expr = parse_primary(p);
    while (!p->err) {
        if (at(p, TK_DOT)) {
            next(p);
            if (!tok_is_name(p)) { perr(p, "expected property name"); return expr; }
            const char* prop = tok_name_copy(p);
            next(p);
            Node* m = node_new(p, ND_MEMBER);
            m->as.member.obj = expr;
            m->as.member.prop = prop;
            expr = m;
        } else if (at(p, TK_LBRACKET)) {
            next(p);
            Node* idx = parse_expression(p);
            expect(p, TK_RBRACKET, "']'");
            Node* ix = node_new(p, ND_INDEX);
            ix->as.index.obj = expr;
            ix->as.index.index = idx;
            expr = ix;
        } else if (at(p, TK_LPAREN)) {
            next(p);
            Node* call = node_new(p, ND_CALL);
            call->as.call.callee = expr;
            while (!p->err && !at(p, TK_RPAREN)) {
                Node* a;
                if (at(p, TK_ELLIPSIS)) {
                    next(p);
                    Node* arg = parse_assignment(p);
                    a = node_new(p, ND_SPREAD);
                    a->as.spread.arg = arg;
                } else {
                    a = parse_assignment(p);
                }
                nlist_push(p->ctx, &call->as.call.args, &call->as.call.argc,
                           &call->as.call.cap, a);
                if (!eat(p, TK_COMMA)) break;
            }
            expect(p, TK_RPAREN, "')'");
            expr = call;
        } else {
            break;
        }
    }
    return expr;
}

static Node* parse_postfix(Parser* p) {
    Node* expr = parse_call_member(p);
    while (!p->err && (at(p, TK_INC) || at(p, TK_DEC))) {
        int op = p->lx.tok.type;
        next(p);
        Node* n = node_new(p, ND_UPDATE);
        n->as.unary.op = op;
        n->as.unary.operand = expr;
        n->as.unary.prefix = false;
        expr = n;
    }
    return expr;
}

static Node* parse_unary(Parser* p) {
    if (p->err) return NULL;
    int t = p->lx.tok.type;
    if (t == TK_NOT || t == TK_MINUS || t == TK_PLUS || t == TK_BITNOT ||
        t == TK_TYPEOF || t == TK_VOID || t == TK_DELETE) {
        next(p);
        Node* operand = parse_unary(p);
        Node* n = node_new(p, ND_UNARY);
        n->as.unary.op = t;
        n->as.unary.operand = operand;
        n->as.unary.prefix = true;
        return n;
    }
    if (t == TK_INC || t == TK_DEC) {
        next(p);
        Node* operand = parse_unary(p);
        Node* n = node_new(p, ND_UPDATE);
        n->as.unary.op = t;
        n->as.unary.operand = operand;
        n->as.unary.prefix = true;
        return n;
    }
    return parse_postfix(p);
}

static Node* parse_exponent(Parser* p) {
    Node* left = parse_unary(p);
    if (!p->err && at(p, TK_POW)) {
        next(p);
        Node* right = parse_exponent(p);
        return make_binary(p, TK_POW, left, right);
    }
    return left;
}
static Node* parse_multiplicative(Parser* p) {
    Node* left = parse_exponent(p);
    while (!p->err && (at(p, TK_STAR) || at(p, TK_SLASH) || at(p, TK_PERCENT))) {
        int op = p->lx.tok.type; next(p);
        left = make_binary(p, op, left, parse_exponent(p));
    }
    return left;
}
static Node* parse_additive(Parser* p) {
    Node* left = parse_multiplicative(p);
    while (!p->err && (at(p, TK_PLUS) || at(p, TK_MINUS))) {
        int op = p->lx.tok.type; next(p);
        left = make_binary(p, op, left, parse_multiplicative(p));
    }
    return left;
}
static Node* parse_shift(Parser* p) {
    Node* left = parse_additive(p);
    while (!p->err && (at(p, TK_LSHIFT) || at(p, TK_RSHIFT) || at(p, TK_URSHIFT))) {
        int op = p->lx.tok.type; next(p);
        left = make_binary(p, op, left, parse_additive(p));
    }
    return left;
}
static Node* parse_relational(Parser* p) {
    Node* left = parse_shift(p);
    while (!p->err && (at(p, TK_LT) || at(p, TK_GT) || at(p, TK_LE) ||
                       at(p, TK_GE) || at(p, TK_INSTANCEOF) || at(p, TK_IN))) {
        int op = p->lx.tok.type; next(p);
        left = make_binary(p, op, left, parse_shift(p));
    }
    return left;
}
static Node* parse_equality(Parser* p) {
    Node* left = parse_relational(p);
    while (!p->err && (at(p, TK_EQ) || at(p, TK_NE) ||
                       at(p, TK_STRICT_EQ) || at(p, TK_STRICT_NE))) {
        int op = p->lx.tok.type; next(p);
        left = make_binary(p, op, left, parse_relational(p));
    }
    return left;
}
static Node* parse_bitand(Parser* p) {
    Node* left = parse_equality(p);
    while (!p->err && at(p, TK_BITAND)) { next(p); left = make_binary(p, TK_BITAND, left, parse_equality(p)); }
    return left;
}
static Node* parse_bitxor(Parser* p) {
    Node* left = parse_bitand(p);
    while (!p->err && at(p, TK_BITXOR)) { next(p); left = make_binary(p, TK_BITXOR, left, parse_bitand(p)); }
    return left;
}
static Node* parse_bitor(Parser* p) {
    Node* left = parse_bitxor(p);
    while (!p->err && at(p, TK_BITOR)) { next(p); left = make_binary(p, TK_BITOR, left, parse_bitxor(p)); }
    return left;
}
static Node* parse_logical_and(Parser* p) {
    Node* left = parse_bitor(p);
    while (!p->err && at(p, TK_AND)) { next(p); left = make_binary(p, TK_AND, left, parse_bitor(p)); }
    return left;
}
static Node* parse_logical_or(Parser* p) {
    Node* left = parse_logical_and(p);
    while (!p->err && at(p, TK_OR)) { next(p); left = make_binary(p, TK_OR, left, parse_logical_and(p)); }
    return left;
}
static Node* parse_nullish(Parser* p) {
    Node* left = parse_logical_or(p);
    while (!p->err && at(p, TK_NULLISH)) {
        next(p);
        Node* right = parse_logical_or(p);
        left = make_binary(p, TK_NULLISH, left, right);
    }
    return left;
}
static Node* parse_conditional(Parser* p) {
    Node* cond = parse_nullish(p);
    if (!p->err && at(p, TK_QUESTION)) {
        next(p);
        Node* then = parse_assignment(p);
        expect(p, TK_COLON, "':'");
        Node* els = parse_assignment(p);
        Node* n = node_new(p, ND_TERNARY);
        n->as.tern.cond = cond;
        n->as.tern.then = then;
        n->as.tern.els = els;
        return n;
    }
    return cond;
}

static bool is_assign_op(int t) {
    switch (t) {
        case TK_ASSIGN:
        case TK_PLUS_ASSIGN: case TK_MINUS_ASSIGN:
        case TK_STAR_ASSIGN: case TK_SLASH_ASSIGN: case TK_PERCENT_ASSIGN:
        case TK_LSHIFT_ASSIGN: case TK_RSHIFT_ASSIGN: case TK_URSHIFT_ASSIGN:
        case TK_AND_ASSIGN: case TK_OR_ASSIGN: case TK_XOR_ASSIGN:
        case TK_POW_ASSIGN:
            return true;
        default: return false;
    }
}

static Node* parse_assignment(Parser* p) {
    if (p->err) return NULL;

    if (at(p, TK_IDENT) && peek_type(p) == TK_ARROW) {
        const char* name = tok_name_copy(p);
        next(p);
        next(p);
        const char** params = (const char**)arena_alloc(&p->ctx->arena, sizeof(char*));
        params[0] = name;
        Node* fn = node_new(p, ND_FUNC);
        fn->as.func.name = NULL;
        fn->as.func.params = params;
        fn->as.func.param_defaults = NULL;
        fn->as.func.param_count = 1;
        fn->as.func.rest_index = -1;
        return parse_arrow_body(p, fn);
    }

    if (at(p, TK_LPAREN)) {
        Lexer save = p->lx;
        next(p);
        const char** params = NULL;
        int count = 0, cap = 0;
        bool ok = true;
        if (!at(p, TK_RPAREN)) {
            for (;;) {
                if (!at(p, TK_IDENT)) { ok = false; break; }
                const char* nm = tok_name_copy(p);
                slist_push(p->ctx, &params, &count, &cap, nm);
                next(p);
                if (eat(p, TK_COMMA)) continue;
                break;
            }
        }
        if (ok && at(p, TK_RPAREN)) {
            next(p);
            if (at(p, TK_ARROW)) {
                next(p);
                Node* fn = node_new(p, ND_FUNC);
                fn->as.func.name = NULL;
                fn->as.func.params = params;
                fn->as.func.param_defaults = NULL;
                fn->as.func.param_count = count;
                fn->as.func.rest_index = -1;
                return parse_arrow_body(p, fn);
            }
        }
        p->lx = save;
    }

    Node* left = parse_conditional(p);
    if (!p->err && is_assign_op(p->lx.tok.type)) {
        int op = p->lx.tok.type;
        next(p);
        Node* value = parse_assignment(p);
        Node* n = node_new(p, ND_ASSIGN);
        n->as.assign.op = op;
        n->as.assign.target = left;
        n->as.assign.value = value;
        return n;
    }
    return left;
}

static Node* parse_expression(Parser* p) {
    Node* first = parse_assignment(p);
    if (p->err || !at(p, TK_COMMA)) return first;
    Node* n = node_new(p, ND_SEQ);
    nlist_push(p->ctx, &n->as.list.items, &n->as.list.count, &n->as.list.cap, first);
    while (!p->err && eat(p, TK_COMMA)) {
        Node* e = parse_assignment(p);
        nlist_push(p->ctx, &n->as.list.items, &n->as.list.count, &n->as.list.cap, e);
    }
    return n;
}

/* ---------- literals ---------- */

static Node* parse_object_expr(Parser* p) {
    next(p);
    Node* n = node_new(p, ND_OBJECT);
    while (!p->err && !at(p, TK_RBRACE)) {
        const char* key = NULL;
        if (at(p, TK_STR)) {
            key = arena_strdup(&p->ctx->arena, p->lx.tok.value.str);
            next(p);
        } else if (at(p, TK_NUM)) {
            char* s = num_to_cstr(p->lx.tok.value.number);
            key = arena_strdup(&p->ctx->arena, s ? s : "0");
            free(s);
            next(p);
        } else if (tok_is_name(p)) {
            key = tok_name_copy(p);
            next(p);
        } else {
            perr(p, "invalid object key at line %d", p->lx.tok.line);
            break;
        }

        Node* val;
        if (eat(p, TK_COLON)) {
            val = parse_assignment(p);
        } else if (at(p, TK_LPAREN)) {
            val = node_new(p, ND_FUNC);
            val->as.func.name = key;
            parse_params_and_body(p, val);
        } else {
            val = make_ident(p, key);
        }
        obj_push(p->ctx, n, key, val);
        if (!eat(p, TK_COMMA)) break;
    }
    expect(p, TK_RBRACE, "'}'");
    return n;
}

static void param_push(Parser* p, const char*** params, Node*** defaults,
                       int* count, int* cap, const char* name, Node* def) {
    if (*count >= *cap) {
        int nc = *cap ? *cap * 2 : 4;
        const char** np = (const char**)arena_alloc(&p->ctx->arena, sizeof(char*) * (size_t)nc);
        Node** nd = (Node**)arena_alloc(&p->ctx->arena, sizeof(Node*) * (size_t)nc);
        if (!np || !nd) return;
        if (*count) {
            memcpy(np, *params, sizeof(char*) * (size_t)(*count));
            memcpy(nd, *defaults, sizeof(Node*) * (size_t)(*count));
        }
        *params = np;
        *defaults = nd;
        *cap = nc;
    }
    (*params)[*count] = name;
    (*defaults)[*count] = def;
    (*count)++;
}

static void parse_params(Parser* p, const char*** params, Node*** defaults,
                         int* count, int* rest_index) {
    int cap = 0;
    expect(p, TK_LPAREN, "'('");
    while (!p->err && !at(p, TK_RPAREN)) {
        const char* name = NULL;
        Node* def = NULL;
        if (at(p, TK_ELLIPSIS)) {
            next(p);
            if (!at(p, TK_IDENT)) { perr(p, "expected rest parameter name"); break; }
            *rest_index = *count;
            name = tok_name_copy(p);
            next(p);
        } else if (at(p, TK_IDENT)) {
            name = tok_name_copy(p);
            next(p);
            if (eat(p, TK_ASSIGN)) def = parse_assignment(p);
        } else {
            perr(p, "expected parameter name at line %d", p->lx.tok.line);
            break;
        }
        param_push(p, params, defaults, count, &cap, name, def);
        if (!eat(p, TK_COMMA)) break;
    }
    expect(p, TK_RPAREN, "')'");
}

static void parse_params_and_body(Parser* p, Node* fn) {
    const char** params = NULL;
    Node** defaults = NULL;
    int count = 0;
    fn->as.func.rest_index = -1;
    parse_params(p, &params, &defaults, &count, &fn->as.func.rest_index);
    fn->as.func.params = params;
    fn->as.func.param_defaults = defaults;
    fn->as.func.param_count = count;
    fn->as.func.body = parse_block(p);
}

static Node* parse_function_expr(Parser* p) {
    next(p);
    Node* n = node_new(p, ND_FUNC);
    if (at(p, TK_IDENT)) { n->as.func.name = tok_name_copy(p); next(p); }
    parse_params_and_body(p, n);
    return n;
}

static Node* parse_arrow_body(Parser* p, Node* fn) {
    if (at(p, TK_LBRACE)) {
        fn->as.func.body = parse_block(p);
    } else {
        Node* expr = parse_assignment(p);
        Node* ret = node_new(p, ND_RETURN);
        ret->as.ret.value = expr;
        Node* blk = node_new(p, ND_BLOCK);
        nlist_push(p->ctx, &blk->as.list.items, &blk->as.list.count, &blk->as.list.cap, ret);
        fn->as.func.body = blk;
    }
    return fn;
}

/* ---------- statements ---------- */

static void eat_semicolon(Parser* p) {
    if (at(p, TK_SEMICOLON)) { next(p); return; }
    if (at(p, TK_RBRACE) || at(p, TK_EOF) || p->lx.nl_before) return;
    perr(p, "expected ';' at line %d", p->lx.tok.line);
}

static Node* parse_var_decl(Parser* p) {
    int kind = p->lx.tok.type;
    next(p);
    Node* n = node_new(p, ND_VAR);
    n->as.var.decl_kind = kind;
    for (;;) {
        if (!at(p, TK_IDENT)) { perr(p, "expected variable name"); break; }
        const char* name = tok_name_copy(p);
        next(p);
        Node* init = NULL;
        if (eat(p, TK_ASSIGN)) init = parse_assignment(p);
        var_push(p, n, make_ident(p, name), init);
        if (!eat(p, TK_COMMA)) break;
    }
    eat_semicolon(p);
    return n;
}

static Node* parse_block(Parser* p) {
    next(p);
    Node* n = node_new(p, ND_BLOCK);
    while (!p->err && !at(p, TK_RBRACE) && !at(p, TK_EOF)) {
        Node* s = parse_statement(p);
        if (p->err) break;
        if (s) nlist_push(p->ctx, &n->as.list.items, &n->as.list.count, &n->as.list.cap, s);
    }
    expect(p, TK_RBRACE, "'}'");
    return n;
}

static Node* parse_if(Parser* p) {
    next(p);
    expect(p, TK_LPAREN, "'('");
    Node* cond = parse_expression(p);
    expect(p, TK_RPAREN, "')'");
    Node* cons = parse_statement(p);
    Node* alt = NULL;
    if (eat(p, TK_ELSE)) alt = parse_statement(p);
    Node* n = node_new(p, ND_IF);
    n->as.iff.cond = cond;
    n->as.iff.cons = cons;
    n->as.iff.alt = alt;
    return n;
}

static Node* parse_while(Parser* p) {
    next(p);
    expect(p, TK_LPAREN, "'('");
    Node* cond = parse_expression(p);
    expect(p, TK_RPAREN, "')'");
    Node* body = parse_statement(p);
    Node* n = node_new(p, ND_WHILE);
    n->as.for_.cond = cond;
    n->as.for_.body = body;
    return n;
}

static Node* parse_do(Parser* p) {
    next(p);
    Node* body = parse_statement(p);
    expect(p, TK_WHILE, "'while'");
    expect(p, TK_LPAREN, "'('");
    Node* cond = parse_expression(p);
    expect(p, TK_RPAREN, "')'");
    eat_semicolon(p);
    Node* n = node_new(p, ND_DOWHILE);
    n->as.for_.cond = cond;
    n->as.for_.body = body;
    return n;
}

static Node* parse_for(Parser* p) {
    next(p);
    expect(p, TK_LPAREN, "'('");

    if (at(p, TK_VAR) || at(p, TK_LET) || at(p, TK_CONST)) {
        int kind = p->lx.tok.type;
        next(p);
        if (!at(p, TK_IDENT)) { perr(p, "expected variable name"); return NULL; }
        const char* name = tok_name_copy(p);
        next(p);
        if (at(p, TK_IN)) {
            next(p);
            Node* right = parse_expression(p);
            expect(p, TK_RPAREN, "')'");
            Node* fn = node_new(p, ND_FORIN);
            fn->as.forin.left = make_ident(p, name);
            fn->as.forin.right = right;
            fn->as.forin.body = parse_statement(p);
            return fn;
        }
        Node* var = node_new(p, ND_VAR);
        var->as.var.decl_kind = kind;
        Node* init = NULL;
        if (eat(p, TK_ASSIGN)) init = parse_assignment(p);
        var_push(p, var, make_ident(p, name), init);
        while (!p->err && eat(p, TK_COMMA)) {
            if (!at(p, TK_IDENT)) { perr(p, "expected variable name"); break; }
            const char* nm = tok_name_copy(p);
            next(p);
            Node* in2 = NULL;
            if (eat(p, TK_ASSIGN)) in2 = parse_assignment(p);
            var_push(p, var, make_ident(p, nm), in2);
        }
        Node* n = node_new(p, ND_FOR);
        n->as.for_.init = var;
        expect(p, TK_SEMICOLON, "';'");
        if (!at(p, TK_SEMICOLON)) n->as.for_.cond = parse_expression(p);
        expect(p, TK_SEMICOLON, "';'");
        if (!at(p, TK_RPAREN)) n->as.for_.update = parse_expression(p);
        expect(p, TK_RPAREN, "')'");
        n->as.for_.body = parse_statement(p);
        return n;
    }

    Node* n = node_new(p, ND_FOR);
    if (!at(p, TK_SEMICOLON)) {
        Node* e = parse_expression(p);
        if (!p->err && at(p, TK_IN)) {
            next(p);
            Node* right = parse_expression(p);
            expect(p, TK_RPAREN, "')'");
            Node* fn = node_new(p, ND_FORIN);
            fn->as.forin.left = e;
            fn->as.forin.right = right;
            fn->as.forin.body = parse_statement(p);
            return fn;
        }
        n->as.for_.init = e;
    }
    expect(p, TK_SEMICOLON, "';'");
    if (!at(p, TK_SEMICOLON)) n->as.for_.cond = parse_expression(p);
    expect(p, TK_SEMICOLON, "';'");
    if (!at(p, TK_RPAREN)) n->as.for_.update = parse_expression(p);
    expect(p, TK_RPAREN, "')'");
    n->as.for_.body = parse_statement(p);
    return n;
}

static Node* parse_return(Parser* p) {
    next(p);
    Node* n = node_new(p, ND_RETURN);
    if (!at(p, TK_SEMICOLON) && !at(p, TK_RBRACE) && !at(p, TK_EOF) && !p->lx.nl_before)
        n->as.ret.value = parse_expression(p);
    eat_semicolon(p);
    return n;
}

static Node* parse_switch(Parser* p) {
    next(p);
    expect(p, TK_LPAREN, "'('");
    Node* disc = parse_expression(p);
    expect(p, TK_RPAREN, "')'");
    expect(p, TK_LBRACE, "'{'");
    Node* n = node_new(p, ND_SWITCH);
    n->as.sw.disc = disc;
    while (!p->err && !at(p, TK_RBRACE) && !at(p, TK_EOF)) {
        Node* c = node_new(p, ND_CASE);
        if (eat(p, TK_CASE)) {
            c->as.cs.test = parse_expression(p);
            expect(p, TK_COLON, "':'");
        } else if (eat(p, TK_DEFAULT)) {
            c->as.cs.test = NULL;
            expect(p, TK_COLON, "':'");
        } else {
            perr(p, "expected 'case' or 'default'");
            break;
        }
        while (!p->err && !at(p, TK_CASE) && !at(p, TK_DEFAULT) &&
               !at(p, TK_RBRACE) && !at(p, TK_EOF)) {
            Node* s = parse_statement(p);
            if (p->err) break;
            if (s) nlist_push(p->ctx, &c->as.cs.stmts, &c->as.cs.count, &c->as.cs.cap, s);
        }
        nlist_push(p->ctx, &n->as.sw.cases, &n->as.sw.count, &n->as.sw.cap, c);
    }
    expect(p, TK_RBRACE, "'}'");
    return n;
}

static Node* parse_try(Parser* p) {
    next(p);
    Node* n = node_new(p, ND_TRY);
    n->as.try_.block = parse_block(p);
    if (eat(p, TK_CATCH)) {
        expect(p, TK_LPAREN, "'('");
        if (at(p, TK_IDENT)) { n->as.try_.param = tok_name_copy(p); next(p); }
        else perr(p, "expected catch parameter");
        expect(p, TK_RPAREN, "')'");
        n->as.try_.handler = parse_block(p);
    }
    if (eat(p, TK_FINALLY)) n->as.try_.finalizer = parse_block(p);
    return n;
}

static Node* parse_statement(Parser* p) {
    if (p->err) return NULL;
    int t = p->lx.tok.type;
    switch (t) {
        case TK_LBRACE: return parse_block(p);
        case TK_VAR: case TK_LET: case TK_CONST: return parse_var_decl(p);
        case TK_IF: return parse_if(p);
        case TK_WHILE: return parse_while(p);
        case TK_DO: return parse_do(p);
        case TK_FOR: return parse_for(p);
        case TK_RETURN: return parse_return(p);
        case TK_BREAK: next(p); eat_semicolon(p); return node_new(p, ND_BREAK);
        case TK_CONTINUE: next(p); eat_semicolon(p); return node_new(p, ND_CONTINUE);
        case TK_SWITCH: return parse_switch(p);
        case TK_TRY: return parse_try(p);
        case TK_THROW: {
            next(p);
            Node* n = node_new(p, ND_THROW);
            n->as.thr.arg = parse_expression(p);
            eat_semicolon(p);
            return n;
        }
        case TK_FUNCTION: {
            Node* n = parse_function_expr(p);
            if (n) n->type = ND_FUNC_DECL;
            return n;
        }
        case TK_SEMICOLON:
            next(p);
            return node_new(p, ND_EMPTY);
        case TK_EOF:
            return NULL;
        default: {
            Node* e = parse_expression(p);
            eat_semicolon(p);
            return e;
        }
    }
}

static Node* parse_program(Parser* p) {
    Node* prog = node_new(p, ND_PROGRAM);
    while (!p->err && !at(p, TK_EOF)) {
        Node* s = parse_statement(p);
        if (p->err) break;
        if (s) nlist_push(p->ctx, &prog->as.list.items, &prog->as.list.count,
                         &prog->as.list.cap, s);
    }
    return prog;
}
/* ==============================================================
 * Interpreter — property access, operators, evaluation
 * ============================================================== */

static jengine_value make_native(Interp* I, const char* name, jg_native_fn fn) {
    jg_func* f = func_new(I->ctx);
    if (!f) return UNDEF;
    f->kind = 1;
    f->name = name ? xstrdup(name) : NULL;
    f->nfn = fn;
    return make_func_value(f);
}

static void func_props_set(struct jengine_context* ctx, jengine_value fnval,
                           const char* key, jengine_value val) {
    jg_func* f = as_func(fnval);
    if (!f) return;
    if (!f->props) f->props = as_object(jengine_make_object(ctx));
    if (!f->props) return;
    jengine_value pv = mkbox(JENGINE_T_OBJECT, (uintptr_t)f->props);
    obj_set_raw(pv, key, val);
}

static jengine_value make_user_func(Interp* I, Node* fnnode) {
    jg_func* f = func_new(I->ctx);
    if (!f) return UNDEF;
    f->kind = 0;
    f->name = fnnode->as.func.name ? xstrdup(fnnode->as.func.name) : NULL;
    f->body = fnnode->as.func.body;
    f->params = (char**)fnnode->as.func.params;
    f->param_defaults = (Node**)fnnode->as.func.param_defaults;
    f->param_count = fnnode->as.func.param_count;
    f->rest_index = fnnode->as.func.rest_index;
    f->closure = I->cur;
    return make_func_value(f);
}

static jengine_value get_property(Interp* I, jengine_value obj, const char* key) {
    int oty = jengine_value_type(obj);
    if (oty == JENGINE_T_NULL || oty == JENGINE_T_UNDEFINED) {
        SB sb; sb_init(&sb);
        sb_puts(&sb, "cannot read properties of ");
        sb_puts(&sb, oty == JENGINE_T_NULL ? "null" : "undefined");
        sb_puts(&sb, " (reading '");
        sb_puts(&sb, key ? key : "");
        sb_puts(&sb, "')");
        set_exception(I, jg_make_error(I, "TypeError", sb.data ? sb.data : ""));
        free(sb.data);
        return UNDEF;
    }
    switch (oty) {
        case JENGINE_T_OBJECT: {
            jengine_value out;
            if (obj_get_raw(obj, key, &out)) return out;
            if (jengine_is_object(I->ctx->obj_methods)) {
                if (obj_get_raw(I->ctx->obj_methods, key, &out)) return out;
            }
            return UNDEF;
        }
        case JENGINE_T_ARRAY: {
            jg_array* a = as_array(obj);
            if (strcmp(key, "length") == 0)
                return jengine_make_number((double)(a ? a->len : 0));
            size_t idx;
            if (parse_index_key(key, strlen(key), &idx))
                return (a && idx < a->len) ? a->items[idx] : UNDEF;
            if (jengine_is_object(I->ctx->arr_methods)) {
                jengine_value out;
                if (obj_get_raw(I->ctx->arr_methods, key, &out)) return out;
            }
            return UNDEF;
        }
        case JENGINE_T_STRING: {
            jg_string* s = as_string(obj);
            size_t len = s ? s->len : 0;
            const char* data = s ? s->data : "";
            if (strcmp(key, "length") == 0) return jengine_make_number((double)len);
            size_t idx;
            if (parse_index_key(key, strlen(key), &idx)) {
                if (idx < len) return jengine_make_string_len(I->ctx, data + idx, 1);
                return UNDEF;
            }
            if (jengine_is_object(I->ctx->str_methods)) {
                jengine_value out;
                if (obj_get_raw(I->ctx->str_methods, key, &out)) return out;
            }
            return UNDEF;
        }
        case JENGINE_T_FUNCTION: {
            jg_func* f = as_func(obj);
            if (!f) return UNDEF;
            if (strcmp(key, "name") == 0)
                return jengine_make_string(I->ctx, f->name ? f->name : "");
            if (strcmp(key, "length") == 0)
                return jengine_make_number((double)f->param_count);
            if (f->props) {
                jengine_value pv = mkbox(JENGINE_T_OBJECT, (uintptr_t)f->props);
                jengine_value out;
                if (obj_get_raw(pv, key, &out)) return out;
            }
            if (jengine_is_object(I->ctx->func_methods)) {
                jengine_value out;
                if (obj_get_raw(I->ctx->func_methods, key, &out)) return out;
            }
            return UNDEF;
        }
        case JENGINE_T_NUMBER: {
            if (jengine_is_object(I->ctx->num_methods)) {
                jengine_value out;
                if (obj_get_raw(I->ctx->num_methods, key, &out)) return out;
            }
            return UNDEF;
        }
        default:
            return UNDEF;
    }
}

static jengine_value get_property_key(Interp* I, jengine_value obj, jengine_value key) {
    if (jengine_is_string(key)) return get_property(I, obj, string_data(key));
    char* s = value_to_string(I, key);
    jengine_value r = get_property(I, obj, s ? s : "");
    free(s);
    return r;
}

static void set_property(Interp* I, jengine_value obj, const char* key, jengine_value val) {
    switch (jengine_value_type(obj)) {
        case JENGINE_T_OBJECT:
            obj_set_raw(obj, key, val);
            break;
        case JENGINE_T_ARRAY: {
            jg_array* a = as_array(obj);
            if (!a) break;
            if (strcmp(key, "length") == 0) {
                size_t nl = (size_t)to_number(I, val);
                if (nl <= a->len) { a->len = nl; }
                else { arr_reserve(a, nl); while (a->len < nl) a->items[a->len++] = UNDEF; }
                break;
            }
            size_t idx;
            if (parse_index_key(key, strlen(key), &idx)) arr_set_len(a, idx, val);
            break;
        }
        default:
            break;
    }
}

static void set_property_key(Interp* I, jengine_value obj, jengine_value key, jengine_value val) {
    if (jengine_is_string(key)) { set_property(I, obj, string_data(key), val); return; }
    char* s = value_to_string(I, key);
    set_property(I, obj, s ? s : "", val);
    free(s);
}

/* ---------- equality / comparison ---------- */

static bool strict_eq(jengine_value a, jengine_value b) {
    int ta = jengine_value_type(a), tb = jengine_value_type(b);
    if (ta != tb) return false;
    switch (ta) {
        case JENGINE_T_NUMBER: return jengine_to_number(a) == jengine_to_number(b);
        case JENGINE_T_STRING: return strcmp(string_data(a), string_data(b)) == 0;
        case JENGINE_T_BOOLEAN: return (a & 1) == (b & 1);
        case JENGINE_T_NULL:
        case JENGINE_T_UNDEFINED: return true;
        default: return boxptr(a) == boxptr(b);
    }
}

static bool loose_eq(Interp* I, jengine_value a, jengine_value b) {
    int ta = jengine_value_type(a), tb = jengine_value_type(b);
    if (ta == tb) return strict_eq(a, b);
    if ((ta == JENGINE_T_NULL && tb == JENGINE_T_UNDEFINED) ||
        (ta == JENGINE_T_UNDEFINED && tb == JENGINE_T_NULL)) return true;
    if (ta == JENGINE_T_BOOLEAN) return loose_eq(I, jengine_make_number(to_number(I, a)), b);
    if (tb == JENGINE_T_BOOLEAN) return loose_eq(I, a, jengine_make_number(to_number(I, b)));
    if ((ta == JENGINE_T_NUMBER && tb == JENGINE_T_STRING) ||
        (ta == JENGINE_T_STRING && tb == JENGINE_T_NUMBER))
        return to_number(I, a) == to_number(I, b);
    return false;
}

static bool value_has_key(Interp* I, jengine_value obj, jengine_value key) {
    (void)I;
    if (jengine_is_object(obj)) {
        jengine_value out;
        if (jengine_is_string(key)) return obj_get_raw(obj, string_data(key), &out);
        return false;
    }
    if (jengine_is_array(obj)) {
        jg_array* a = as_array(obj);
        if (jengine_is_string(key)) {
            if (strcmp(string_data(key), "length") == 0) return true;
            size_t idx;
            if (parse_index_key(string_data(key), strlen(string_data(key)), &idx))
                return a && idx < a->len;
        }
        return false;
    }
    if (jengine_is_string(obj)) {
        if (!jengine_is_string(key)) return false;
        size_t idx;
        jg_string* s = as_string(obj);
        if (parse_index_key(string_data(key), strlen(string_data(key)), &idx))
            return s && idx < s->len;
    }
    return false;
}

static jengine_value apply_binary(Interp* I, int op, jengine_value l, jengine_value r) {
    struct jengine_context* ctx = I->ctx;

    if (op == TK_PLUS) {
        if (jengine_is_string(l) || jengine_is_string(r) ||
            jengine_is_object(l) || jengine_is_array(l) ||
            jengine_is_object(r) || jengine_is_array(r)) {
            char* ls = value_to_string(I, l);
            char* rs = value_to_string(I, r);
            SB sb; sb_init(&sb);
            sb_puts(&sb, ls ? ls : "");
            sb_puts(&sb, rs ? rs : "");
            jengine_value v = jengine_make_string(ctx, sb.data ? sb.data : "");
            free(ls); free(rs); free(sb.data);
            return v;
        }
        return jengine_make_number(to_number(I, l) + to_number(I, r));
    }

    switch (op) {
        case TK_MINUS: return jengine_make_number(to_number(I, l) - to_number(I, r));
        case TK_STAR:  return jengine_make_number(to_number(I, l) * to_number(I, r));
        case TK_SLASH: return jengine_make_number(to_number(I, l) / to_number(I, r));
        case TK_PERCENT:
            return jengine_make_number(fmod(to_number(I, l), to_number(I, r)));
        case TK_POW: return jengine_make_number(pow(to_number(I, l), to_number(I, r)));
        case TK_LT: {
            if (jengine_is_string(l) && jengine_is_string(r))
                return BOOLV(strcmp(string_data(l), string_data(r)) < 0);
            return BOOLV(to_number(I, l) < to_number(I, r));
        }
        case TK_GT: {
            if (jengine_is_string(l) && jengine_is_string(r))
                return BOOLV(strcmp(string_data(l), string_data(r)) > 0);
            return BOOLV(to_number(I, l) > to_number(I, r));
        }
        case TK_LE: {
            if (jengine_is_string(l) && jengine_is_string(r))
                return BOOLV(strcmp(string_data(l), string_data(r)) <= 0);
            return BOOLV(to_number(I, l) <= to_number(I, r));
        }
        case TK_GE: {
            if (jengine_is_string(l) && jengine_is_string(r))
                return BOOLV(strcmp(string_data(l), string_data(r)) >= 0);
            return BOOLV(to_number(I, l) >= to_number(I, r));
        }
        case TK_EQ: return BOOLV(loose_eq(I, l, r));
        case TK_NE: return BOOLV(!loose_eq(I, l, r));
        case TK_STRICT_EQ: return BOOLV(strict_eq(l, r));
        case TK_STRICT_NE: return BOOLV(!strict_eq(l, r));
        case TK_BITAND: return jengine_make_number((double)((int32_t)to_number(I, l) & (int32_t)to_number(I, r)));
        case TK_BITOR:  return jengine_make_number((double)((int32_t)to_number(I, l) | (int32_t)to_number(I, r)));
        case TK_BITXOR: return jengine_make_number((double)((int32_t)to_number(I, l) ^ (int32_t)to_number(I, r)));
        case TK_LSHIFT: return jengine_make_number((double)((int32_t)to_number(I, l) << ((int)to_number(I, r) & 31)));
        case TK_RSHIFT: return jengine_make_number((double)((int32_t)to_number(I, l) >> (to_number(I, r) < 0 ? 0 : ((int)to_number(I, r) & 31))));
        case TK_URSHIFT: return jengine_make_number((double)((uint32_t)to_number(I, l) >> (to_number(I, r) < 0 ? 0 : ((int)to_number(I, r) & 31))));
        case TK_IN: return BOOLV(value_has_key(I, r, l));
        case TK_INSTANCEOF: return BOOLV(false);
        default: return UNDEF;
    }
}

static const char* typeof_str(jengine_value v) {
    switch (jengine_value_type(v)) {
        case JENGINE_T_UNDEFINED: return "undefined";
        case JENGINE_T_NULL: return "object";
        case JENGINE_T_BOOLEAN: return "boolean";
        case JENGINE_T_NUMBER: return "number";
        case JENGINE_T_STRING: return "string";
        case JENGINE_T_FUNCTION: return "function";
        default: return "object";
    }
}

/* ---------- function calls ---------- */

static jengine_value call_func(Interp* I, jg_func* f, jengine_value thisv,
                               jengine_value* args, int argc) {
    if (!f) return UNDEF;
    if (f->kind == 1) return f->nfn(I->ctx, thisv, args, argc);
    if (f->kind == 2) return f->pfn(I->ctx, args, argc);
    if (f->kind == 3) {
        int total = f->bound_argc + argc;
        jengine_value* all = total > 0
            ? (jengine_value*)malloc(sizeof(jengine_value) * (size_t)total) : NULL;
        for (int i = 0; i < f->bound_argc; i++) all[i] = f->bound_args[i];
        for (int i = 0; i < argc; i++) all[f->bound_argc + i] = args[i];
        jengine_value r = call_func(I, as_func(f->bound_target), f->bound_this, all, total);
        free(all);
        return r;
    }

    if (I->call_depth > 2000) {
        set_exception(I, jengine_make_string(I->ctx, "RangeError: maximum call stack size exceeded"));
        return UNDEF;
    }
    jg_env* env = env_new(I, f->closure);
    if (!env) { set_exception(I, jengine_make_string(I->ctx, "out of memory")); return UNDEF; }

    /* arguments object */
    {
        jengine_value argarr = jengine_make_array(I->ctx, (size_t)(argc > 0 ? argc : 1));
        for (int i = 0; i < argc; i++) jengine_array_push(I->ctx, argarr, args[i]);
        env_def(env, "arguments", argarr);
    }

    int fixed = f->param_count;
    int rest = f->rest_index;
    if (rest >= 0 && rest < fixed) fixed = rest;
    for (int i = 0; i < fixed; i++) {
        jengine_value v;
        if (i < argc && !jengine_is_undefined(args[i])) {
            v = args[i];
        } else if (f->param_defaults && f->param_defaults[i]) {
            v = ev(I, f->param_defaults[i]);
        } else {
            v = (i < argc) ? args[i] : UNDEF;
        }
        env_def(env, f->params[i], v);
    }
    if (rest >= 0 && rest < f->param_count) {
        jengine_value restarr = jengine_make_array(I->ctx, 4);
        for (int i = rest; i < argc; i++) jengine_array_push(I->ctx, restarr, args[i]);
        env_def(env, f->params[rest], restarr);
    }

    jg_env* saved_env = I->cur;
    jengine_value saved_this = I->this_val;
    I->cur = env;
    I->this_val = thisv;
    I->call_depth++;

    exec_block(I, f->body);

    jengine_value rv = UNDEF;
    if (I->flow == FLOW_RETURN) {
        rv = I->flow_val;
        I->flow = FLOW_NORMAL;
    }
    I->cur = saved_env;
    I->this_val = saved_this;
    I->call_depth--;
    return rv;
}

static jengine_value eval_call_expr(Interp* I, Node* n) {
    jengine_value thisv = UNDEF;
    jengine_value callee = UNDEF;

    Node* ce = n->as.call.callee;
    if (!n->as.call.is_new && ce && ce->type == ND_MEMBER) {
        jengine_value obj = ev(I, ce->as.member.obj);
        thisv = obj;
        callee = get_property(I, obj, ce->as.member.prop);
    } else if (!n->as.call.is_new && ce && ce->type == ND_INDEX) {
        jengine_value obj = ev(I, ce->as.index.obj);
        jengine_value key = ev(I, ce->as.index.index);
        thisv = obj;
        callee = get_property_key(I, obj, key);
    } else {
        callee = ev(I, ce);
    }
    if (I->flow != FLOW_NORMAL) return UNDEF;

    if (!jengine_is_function(callee)) {
        set_exception(I, jengine_make_string(I->ctx, "TypeError: value is not a function"));
        return UNDEF;
    }

    int argc = 0, cap = 0;
    jengine_value* args = NULL;
    for (int i = 0; i < n->as.call.argc; i++) {
        Node* a = n->as.call.args[i];
        if (a && a->type == ND_SPREAD) {
            jengine_value seq = ev(I, a->as.spread.arg);
            if (jengine_is_array(seq)) {
                jg_array* ar = as_array(seq);
                for (size_t k = 0; ar && k < ar->len; k++) {
                    if (argc >= cap) { cap = cap ? cap * 2 : 4; args = (jengine_value*)realloc(args, sizeof(jengine_value) * (size_t)cap); }
                    args[argc++] = ar->items[k];
                }
            } else if (jengine_is_string(seq)) {
                jg_string* s = as_string(seq);
                for (size_t k = 0; s && k < s->len; k++) {
                    if (argc >= cap) { cap = cap ? cap * 2 : 4; args = (jengine_value*)realloc(args, sizeof(jengine_value) * (size_t)cap); }
                    args[argc++] = jengine_make_string_len(I->ctx, s->data + k, 1);
                }
            }
        } else {
            if (argc >= cap) { cap = cap ? cap * 2 : 4; args = (jengine_value*)realloc(args, sizeof(jengine_value) * (size_t)cap); }
            args[argc++] = ev(I, a);
            if (I->flow != FLOW_NORMAL) { free(args); return UNDEF; }
        }
    }

    jengine_value result;
    if (n->as.call.is_new) {
        jengine_value newobj = jengine_make_object(I->ctx);
        result = call_func(I, as_func(callee), newobj, args, argc);
        if (jengine_is_object(result) || jengine_is_array(result) || jengine_is_function(result))
            { /* use returned object */ }
        else result = newobj;
    } else {
        result = call_func(I, as_func(callee), thisv, args, argc);
    }
    free(args);
    return result;
}

/* ---------- assignment targets ---------- */

static void write_target(Interp* I, Node* t, jengine_value v) {
    if (!t) return;
    if (t->type == ND_IDENT) {
        jengine_value* slot = env_find(I->cur, t->as.ident);
        if (slot) *slot = v;
        else env_def(I->cur, t->as.ident, v);
    } else if (t->type == ND_MEMBER) {
        jengine_value obj = ev(I, t->as.member.obj);
        set_property(I, obj, t->as.member.prop, v);
    } else if (t->type == ND_INDEX) {
        jengine_value obj = ev(I, t->as.index.obj);
        jengine_value key = ev(I, t->as.index.index);
        set_property_key(I, obj, key, v);
    }
}

static int compound_base_op(int op) {
    switch (op) {
        case TK_PLUS_ASSIGN: return TK_PLUS;
        case TK_MINUS_ASSIGN: return TK_MINUS;
        case TK_STAR_ASSIGN: return TK_STAR;
        case TK_SLASH_ASSIGN: return TK_SLASH;
        case TK_PERCENT_ASSIGN: return TK_PERCENT;
        case TK_LSHIFT_ASSIGN: return TK_LSHIFT;
        case TK_RSHIFT_ASSIGN: return TK_RSHIFT;
        case TK_URSHIFT_ASSIGN: return TK_URSHIFT;
        case TK_AND_ASSIGN: return TK_BITAND;
        case TK_OR_ASSIGN: return TK_BITOR;
        case TK_XOR_ASSIGN: return TK_BITXOR;
        case TK_POW_ASSIGN: return TK_POW;
        default: return TK_ASSIGN;
    }
}

/* ---------- expression evaluation ---------- */

static jengine_value ev(Interp* I, Node* n) {
    if (!n) return UNDEF;
    switch (n->type) {
        case ND_NUM: return jengine_make_number(n->as.num);
        case ND_STR: return jengine_make_string_len(I->ctx, n->as.str.data, n->as.str.len);
        case ND_BOOL: return BOOLV(n->as.boolean);
        case ND_NULL: return NULLV;
        case ND_THIS: return I->this_val;
        case ND_IDENT: {
            jengine_value* slot = env_find(I->cur, n->as.ident);
            if (slot) return *slot;
            {
                SB sb; sb_init(&sb);
                sb_puts(&sb, n->as.ident ? n->as.ident : "");
                sb_puts(&sb, " is not defined");
                set_exception(I, jg_make_error(I, "ReferenceError", sb.data ? sb.data : ""));
                free(sb.data);
            }
            return UNDEF;
        }
        case ND_ARRAY: {
            jengine_value arr = jengine_make_array(I->ctx, (size_t)(n->as.list.count > 0 ? n->as.list.count : 4));
            for (int i = 0; i < n->as.list.count; i++) {
                Node* item = n->as.list.items[i];
                if (item && item->type == ND_SPREAD) {
                    jengine_value seq = ev(I, item->as.spread.arg);
                    if (jengine_is_array(seq)) {
                        jg_array* ar = as_array(seq);
                        for (size_t k = 0; ar && k < ar->len; k++) jengine_array_push(I->ctx, arr, ar->items[k]);
                    } else if (jengine_is_string(seq)) {
                        jg_string* s = as_string(seq);
                        for (size_t k = 0; s && k < s->len; k++)
                            jengine_array_push(I->ctx, arr, jengine_make_string_len(I->ctx, s->data + k, 1));
                    }
                } else {
                    jengine_array_push(I->ctx, arr, ev(I, item));
                }
                if (I->flow != FLOW_NORMAL) return UNDEF;
            }
            return arr;
        }
        case ND_OBJECT: {
            jengine_value obj = jengine_make_object(I->ctx);
            for (int i = 0; i < n->as.obj.count; i++) {
                jengine_value v = ev(I, n->as.obj.vals[i]);
                obj_set_raw(obj, n->as.obj.keys[i], v);
                if (I->flow != FLOW_NORMAL) return UNDEF;
            }
            return obj;
        }
        case ND_FUNC: return make_user_func(I, n);
        case ND_MEMBER: {
            jengine_value obj = ev(I, n->as.member.obj);
            if (I->flow != FLOW_NORMAL) return UNDEF;
            return get_property(I, obj, n->as.member.prop);
        }
        case ND_INDEX: {
            jengine_value obj = ev(I, n->as.index.obj);
            if (I->flow != FLOW_NORMAL) return UNDEF;
            jengine_value key = ev(I, n->as.index.index);
            if (I->flow != FLOW_NORMAL) return UNDEF;
            return get_property_key(I, obj, key);
        }
        case ND_CALL: return eval_call_expr(I, n);
        case ND_UNARY: {
            int op = n->as.unary.op;
            if (op == TK_TYPEOF) {
                Node* operand = n->as.unary.operand;
                if (operand && operand->type == ND_IDENT &&
                    !env_find(I->cur, operand->as.ident)) {
                    return jengine_make_string(I->ctx, "undefined");
                }
                jengine_value v = ev(I, n->as.unary.operand);
                if (I->flow != FLOW_NORMAL) return UNDEF;
                return jengine_make_string(I->ctx, typeof_str(v));
            }
            if (op == TK_DELETE) {
                Node* t = n->as.unary.operand;
                if (t && t->type == ND_MEMBER) {
                    jengine_value obj = ev(I, t->as.member.obj);
                    jg_object* o = as_object(obj);
                    if (o) {
                        for (int i = 0; i < o->count; i++) {
                            if (strcmp(o->keys[i], t->as.member.prop) == 0) {
                                free(o->keys[i]);
                                for (int j = i; j < o->count - 1; j++) { o->keys[j] = o->keys[j+1]; o->vals[j] = o->vals[j+1]; }
                                o->count--;
                                break;
                            }
                        }
                    }
                    return BOOLV(true);
                }
                return BOOLV(true);
            }
            jengine_value v = ev(I, n->as.unary.operand);
            if (I->flow != FLOW_NORMAL) return UNDEF;
            switch (op) {
                case TK_NOT: return BOOLV(!truthy(v));
                case TK_MINUS: return jengine_make_number(-to_number(I, v));
                case TK_PLUS: return jengine_make_number(to_number(I, v));
                case TK_BITNOT: return jengine_make_number((double)(~(int32_t)to_number(I, v)));
                case TK_VOID: return UNDEF;
                default: return UNDEF;
            }
        }
        case ND_UPDATE: {
            jengine_value old = ev(I, n->as.unary.operand);
            if (I->flow != FLOW_NORMAL) return UNDEF;
            double nv = to_number(I, old) + (n->as.unary.op == TK_INC ? 1.0 : -1.0);
            write_target(I, n->as.unary.operand, jengine_make_number(nv));
            return n->as.unary.prefix ? jengine_make_number(nv) : jengine_make_number(to_number(I, old));
        }
        case ND_BINARY: {
            int op = n->as.binary.op;
            if (op == TK_AND) {
                jengine_value l = ev(I, n->as.binary.left);
                if (I->flow != FLOW_NORMAL) return UNDEF;
                return truthy(l) ? ev(I, n->as.binary.right) : l;
            }
            if (op == TK_OR) {
                jengine_value l = ev(I, n->as.binary.left);
                if (I->flow != FLOW_NORMAL) return UNDEF;
                return truthy(l) ? l : ev(I, n->as.binary.right);
            }
            if (op == TK_NULLISH) {
                jengine_value l = ev(I, n->as.binary.left);
                if (I->flow != FLOW_NORMAL) return UNDEF;
                int lt = jengine_value_type(l);
                if (lt == JENGINE_T_NULL || lt == JENGINE_T_UNDEFINED)
                    return ev(I, n->as.binary.right);
                return l;
            }
            jengine_value l = ev(I, n->as.binary.left);
            if (I->flow != FLOW_NORMAL) return UNDEF;
            jengine_value r = ev(I, n->as.binary.right);
            if (I->flow != FLOW_NORMAL) return UNDEF;
            return apply_binary(I, op, l, r);
        }
        case ND_ASSIGN: {
            int op = n->as.assign.op;
            if (op == TK_ASSIGN) {
                jengine_value v = ev(I, n->as.assign.value);
                if (I->flow != FLOW_NORMAL) return UNDEF;
                write_target(I, n->as.assign.target, v);
                return v;
            }
            jengine_value cur = ev(I, n->as.assign.target);
            if (I->flow != FLOW_NORMAL) return UNDEF;
            jengine_value v = ev(I, n->as.assign.value);
            if (I->flow != FLOW_NORMAL) return UNDEF;
            jengine_value nv = apply_binary(I, compound_base_op(op), cur, v);
            write_target(I, n->as.assign.target, nv);
            return nv;
        }
        case ND_TERNARY: {
            jengine_value c = ev(I, n->as.tern.cond);
            if (I->flow != FLOW_NORMAL) return UNDEF;
            return truthy(c) ? ev(I, n->as.tern.then) : ev(I, n->as.tern.els);
        }
        case ND_SEQ: {
            jengine_value last = UNDEF;
            for (int i = 0; i < n->as.list.count; i++) {
                last = ev(I, n->as.list.items[i]);
                if (I->flow != FLOW_NORMAL) return UNDEF;
            }
            return last;
        }
        case ND_SPREAD: return ev(I, n->as.spread.arg);
        default:
            return UNDEF;
    }
}

/* ---------- statements ---------- */

static void exec_block(Interp* I, Node* n) {
    if (!n) return;
    if (n->type != ND_BLOCK && n->type != ND_PROGRAM) return;

    /* hoist function and var declarations */
    for (int i = 0; i < n->as.list.count; i++) {
        Node* s = n->as.list.items[i];
        if (!s) continue;
        if (s->type == ND_FUNC_DECL && s->as.func.name) {
            env_def(I->cur, s->as.func.name, make_user_func(I, s));
        } else if (s->type == ND_VAR) {
            for (int j = 0; j < s->as.var.count; j++) {
                Node* id = s->as.var.names[j];
                if (id && !env_find(I->cur, id->as.ident))
                    env_def(I->cur, id->as.ident, UNDEF);
            }
        }
    }

    for (int i = 0; i < n->as.list.count; i++) {
        exec_stmt(I, n->as.list.items[i]);
        if (I->flow != FLOW_NORMAL) break;
    }
}

static void exec_forin(Interp* I, Node* n) {
    jengine_value right = ev(I, n->as.forin.right);
    if (I->flow != FLOW_NORMAL) return;

    /* collect keys */
    char** keys = NULL;
    int kcount = 0, kcap = 0;
    if (jengine_is_object(right)) {
        jg_object* o = as_object(right);
        if (o) for (int i = 0; i < o->count; i++) {
            if (kcount >= kcap) { kcap = kcap ? kcap * 2 : 8; keys = (char**)realloc(keys, sizeof(char*) * (size_t)kcap); }
            keys[kcount++] = xstrdup(o->keys[i]);
        }
    } else if (jengine_is_array(right)) {
        jg_array* a = as_array(right);
        for (size_t i = 0; a && i < a->len; i++) {
            if (kcount >= kcap) { kcap = kcap ? kcap * 2 : 8; keys = (char**)realloc(keys, sizeof(char*) * (size_t)kcap); }
            char buf[32];
            snprintf(buf, sizeof buf, "%zu", i);
            keys[kcount++] = xstrdup(buf);
        }
    } else if (jengine_is_string(right)) {
        jg_string* s = as_string(right);
        for (size_t i = 0; s && i < s->len; i++) {
            if (kcount >= kcap) { kcap = kcap ? kcap * 2 : 8; keys = (char**)realloc(keys, sizeof(char*) * (size_t)kcap); }
            char buf[32];
            snprintf(buf, sizeof buf, "%zu", i);
            keys[kcount++] = xstrdup(buf);
        }
    }

    for (int i = 0; i < kcount; i++) {
        write_target(I, n->as.forin.left, jengine_make_string(I->ctx, keys[i]));
        if (I->flow != FLOW_NORMAL) break;
        exec_stmt(I, n->as.forin.body);
        if (I->flow == FLOW_BREAK) { I->flow = FLOW_NORMAL; break; }
        if (I->flow == FLOW_CONTINUE) I->flow = FLOW_NORMAL;
        if (I->flow != FLOW_NORMAL) break;
    }
    for (int i = 0; i < kcount; i++) free(keys[i]);
    free(keys);
}

static void exec_stmt(Interp* I, Node* n) {
    if (!n || I->flow != FLOW_NORMAL) return;
    switch (n->type) {
        case ND_PROGRAM:
        case ND_BLOCK:
            exec_block(I, n);
            break;
        case ND_EMPTY:
            break;
        case ND_VAR: {
            for (int i = 0; i < n->as.var.count; i++) {
                Node* id = n->as.var.names[i];
                Node* init = n->as.var.inits[i];
                jengine_value v = init ? ev(I, init) : UNDEF;
                if (I->flow != FLOW_NORMAL) return;
                env_def(I->cur, id->as.ident, v);
            }
            break;
        }
        case ND_FUNC_DECL: {
            jengine_value f = make_user_func(I, n);
            if (n->as.func.name) env_def(I->cur, n->as.func.name, f);
            break;
        }
        case ND_IF: {
            jengine_value c = ev(I, n->as.iff.cond);
            if (I->flow != FLOW_NORMAL) return;
            if (truthy(c)) exec_stmt(I, n->as.iff.cons);
            else if (n->as.iff.alt) exec_stmt(I, n->as.iff.alt);
            break;
        }
        case ND_WHILE: {
            while (I->flow == FLOW_NORMAL) {
                jengine_value c = ev(I, n->as.for_.cond);
                if (I->flow != FLOW_NORMAL) return;
                if (!truthy(c)) break;
                exec_stmt(I, n->as.for_.body);
                if (I->flow == FLOW_BREAK) { I->flow = FLOW_NORMAL; break; }
                if (I->flow == FLOW_CONTINUE) I->flow = FLOW_NORMAL;
                if (I->flow != FLOW_NORMAL) break;
            }
            break;
        }
        case ND_DOWHILE: {
            do {
                exec_stmt(I, n->as.for_.body);
                if (I->flow == FLOW_BREAK) { I->flow = FLOW_NORMAL; break; }
                if (I->flow == FLOW_CONTINUE) I->flow = FLOW_NORMAL;
                if (I->flow != FLOW_NORMAL) break;
                jengine_value c = ev(I, n->as.for_.cond);
                if (I->flow != FLOW_NORMAL) return;
                if (!truthy(c)) break;
            } while (1);
            break;
        }
        case ND_FOR: {
            if (n->as.for_.init) exec_stmt(I, n->as.for_.init);
            while (I->flow == FLOW_NORMAL) {
                if (n->as.for_.cond) {
                    jengine_value c = ev(I, n->as.for_.cond);
                    if (I->flow != FLOW_NORMAL) return;
                    if (!truthy(c)) break;
                }
                exec_stmt(I, n->as.for_.body);
                if (I->flow == FLOW_BREAK) { I->flow = FLOW_NORMAL; break; }
                if (I->flow == FLOW_CONTINUE) I->flow = FLOW_NORMAL;
                if (I->flow != FLOW_NORMAL) break;
                if (n->as.for_.update) {
                    ev(I, n->as.for_.update);
                    if (I->flow != FLOW_NORMAL) return;
                }
            }
            break;
        }
        case ND_FORIN:
            exec_forin(I, n);
            break;
        case ND_RETURN: {
            jengine_value v = n->as.ret.value ? ev(I, n->as.ret.value) : UNDEF;
            if (I->flow == FLOW_THROW) return;
            I->flow = FLOW_RETURN;
            I->flow_val = v;
            break;
        }
        case ND_BREAK: I->flow = FLOW_BREAK; break;
        case ND_CONTINUE: I->flow = FLOW_CONTINUE; break;
        case ND_THROW: {
            jengine_value v = ev(I, n->as.thr.arg);
            if (I->flow == FLOW_THROW) return;
            set_exception(I, v);
            break;
        }
        case ND_TRY: {
            exec_block(I, n->as.try_.block);
            if (I->flow == FLOW_THROW && n->as.try_.handler) {
                jengine_value exc = I->flow_val;
                I->flow = FLOW_NORMAL;
                I->ctx->has_exception = false;
                jg_env* henv = env_new(I, I->cur);
                if (n->as.try_.param) env_def(henv, n->as.try_.param, exc);
                jg_env* save = I->cur;
                I->cur = henv;
                exec_block(I, n->as.try_.handler);
                I->cur = save;
            }
            if (n->as.try_.finalizer) {
                int flow = I->flow;
                jengine_value fv = I->flow_val;
                I->flow = FLOW_NORMAL;
                exec_block(I, n->as.try_.finalizer);
                if (I->flow == FLOW_NORMAL) { I->flow = flow; I->flow_val = fv; }
            }
            break;
        }
        case ND_SWITCH: {
            jengine_value disc = ev(I, n->as.sw.disc);
            if (I->flow != FLOW_NORMAL) return;
            int def = -1, start = -1;
            for (int i = 0; i < n->as.sw.count; i++) {
                Node* c = n->as.sw.cases[i];
                if (c->as.cs.test == NULL) { def = i; continue; }
                if (start < 0) {
                    jengine_value t = ev(I, c->as.cs.test);
                    if (I->flow != FLOW_NORMAL) return;
                    if (strict_eq(disc, t)) start = i;
                }
            }
            if (start < 0) start = def;
            if (start >= 0) {
                for (int i = start; i < n->as.sw.count; i++) {
                    Node* c = n->as.sw.cases[i];
                    for (int j = 0; j < c->as.cs.count; j++) {
                        exec_stmt(I, c->as.cs.stmts[j]);
                        if (I->flow != FLOW_NORMAL) break;
                    }
                    if (I->flow != FLOW_NORMAL) break;
                }
            }
            if (I->flow == FLOW_BREAK) I->flow = FLOW_NORMAL;
            break;
        }
        default:
            I->last_value = ev(I, n);
            break;
    }
}
/* ==============================================================
 * Standard library
 * ============================================================== */

static double narg(struct jengine_context* ctx, jengine_value* a, int argc, int i) {
    if (!a || i < 0 || i >= argc) return 0.0;
    if (jengine_is_number(a[i])) return jengine_to_number(a[i]);
    return ctx->I ? to_number(ctx->I, a[i]) : 0.0;
}

static char* self_cstr(struct jengine_context* ctx, jengine_value t) {
    if (jengine_is_string(t)) return xstrdup(string_data(t));
    if (ctx->I) return value_to_string(ctx->I, t);
    return xstrdup("");
}

static void def_method(Interp* I, jengine_value obj, const char* name, jg_native_fn fn) {
    obj_set_raw(obj, name, make_native(I, name, fn));
}

/* ---------- array methods ---------- */

static jengine_value m_arr_push(struct jengine_context* ctx, jengine_value t,
                                jengine_value* a, int argc) {
    jg_array* arr = as_array(t);
    if (!arr) return UNDEF;
    for (int i = 0; i < argc; i++) jengine_array_push(ctx, t, a[i]);
    return jengine_make_number((double)arr->len);
}
static jengine_value m_arr_pop(struct jengine_context* ctx, jengine_value t,
                               jengine_value* a, int argc) {
    (void)ctx; (void)a; (void)argc;
    jg_array* arr = as_array(t);
    if (!arr || arr->len == 0) return UNDEF;
    jengine_value v = arr->items[arr->len - 1];
    arr->len--;
    return v;
}
static jengine_value m_arr_shift(struct jengine_context* ctx, jengine_value t,
                                 jengine_value* a, int argc) {
    (void)ctx; (void)a; (void)argc;
    jg_array* arr = as_array(t);
    if (!arr || arr->len == 0) return UNDEF;
    jengine_value v = arr->items[0];
    for (size_t i = 0; i + 1 < arr->len; i++) arr->items[i] = arr->items[i + 1];
    arr->len--;
    return v;
}
static jengine_value m_arr_unshift(struct jengine_context* ctx, jengine_value t,
                                   jengine_value* a, int argc) {
    (void)ctx;
    jg_array* arr = as_array(t);
    if (!arr) return UNDEF;
    if (argc > 0) {
        arr_reserve(arr, arr->len + (size_t)argc);
        for (size_t i = arr->len; i > 0; i--) arr->items[i - 1 + (size_t)argc] = arr->items[i - 1];
        for (int i = 0; i < argc; i++) arr->items[i] = a[i];
        arr->len += (size_t)argc;
    }
    return jengine_make_number((double)arr->len);
}
static jengine_value m_arr_slice(struct jengine_context* ctx, jengine_value t,
                                 jengine_value* a, int argc) {
    jg_array* arr = as_array(t);
    if (!arr) return UNDEF;
    long len = (long)arr->len;
    long start = (long)narg(ctx, a, argc, 0);
    long end = (argc > 1 && !jengine_is_undefined(a[1])) ? (long)narg(ctx, a, argc, 1) : len;
    if (start < 0) start += len;
    if (end < 0) end += len;
    if (start < 0) start = 0;
    if (end > len) end = len;
    if (end < start) end = start;
    jengine_value out = jengine_make_array(ctx, (size_t)(end - start) + 1);
    for (long i = start; i < end; i++) jengine_array_push(ctx, out, arr->items[i]);
    return out;
}
static jengine_value m_arr_splice(struct jengine_context* ctx, jengine_value t,
                                  jengine_value* a, int argc) {
    jg_array* arr = as_array(t);
    if (!arr) return UNDEF;
    long len = (long)arr->len;
    long start = (long)narg(ctx, a, argc, 0);
    if (start < 0) start += len;
    if (start < 0) start = 0;
    if (start > len) start = len;
    long del = (argc > 1) ? (long)narg(ctx, a, argc, 1) : (len - start);
    if (del < 0) del = 0;
    if (start + del > len) del = len - start;

    jengine_value removed = jengine_make_array(ctx, (size_t)del + 1);
    for (long i = 0; i < del; i++) jengine_array_push(ctx, removed, arr->items[start + i]);

    int ins = argc > 2 ? argc - 2 : 0;
    if (ins > (int)del) arr_reserve(arr, arr->len + (size_t)(ins - del));
    if (ins != (int)del) {
        if (ins > (int)del)
            for (size_t i = arr->len; i > (size_t)(start + del); i--)
                arr->items[i - 1 + (size_t)(ins - del)] = arr->items[i - 1];
        else
            for (size_t i = (size_t)(start + del); i < arr->len; i++)
                arr->items[i + (ins - del)] = arr->items[i];
        arr->len = (size_t)((long)arr->len + (ins - del));
    }
    for (int i = 0; i < ins; i++) arr->items[start + i] = a[2 + i];
    return removed;
}
static jengine_value m_arr_concat(struct jengine_context* ctx, jengine_value t,
                                  jengine_value* a, int argc) {
    jengine_value out = jengine_make_array(ctx, 8);
    jg_array* arr = as_array(t);
    if (arr) for (size_t i = 0; i < arr->len; i++) jengine_array_push(ctx, out, arr->items[i]);
    for (int i = 0; i < argc; i++) {
        if (jengine_is_array(a[i])) {
            jg_array* o = as_array(a[i]);
            for (size_t k = 0; o && k < o->len; k++) jengine_array_push(ctx, out, o->items[k]);
        } else {
            jengine_array_push(ctx, out, a[i]);
        }
    }
    return out;
}
static jengine_value m_arr_join(struct jengine_context* ctx, jengine_value t,
                                jengine_value* a, int argc) {
    jg_array* arr = as_array(t);
    if (!arr) return jengine_make_string(ctx, "");
    char* sep = (argc > 0 && !jengine_is_undefined(a[0])) ? self_cstr(ctx, a[0]) : xstrdup(",");
    SB sb; sb_init(&sb);
    for (size_t i = 0; i < arr->len; i++) {
        if (i) sb_puts(&sb, sep);
        jengine_value v = arr->items[i];
        int ty = jengine_value_type(v);
        if (ty == JENGINE_T_NULL || ty == JENGINE_T_UNDEFINED) {
            /* null / undefined join as empty string */
        } else if (ty == JENGINE_T_STRING) {
            sb_puts(&sb, string_data(v));
        } else {
            char* s = ctx->I ? value_to_string(ctx->I, v) : xstrdup("");
            sb_puts(&sb, s);
            free(s);
        }
    }
    jengine_value r = jengine_make_string(ctx, sb.data ? sb.data : "");
    free(sep);
    free(sb.data);
    return r;
}
static jengine_value m_arr_indexOf(struct jengine_context* ctx, jengine_value t,
                                   jengine_value* a, int argc) {
    jg_array* arr = as_array(t);
    if (!arr || argc < 1) return jengine_make_number(-1);
    long from = argc > 1 ? (long)narg(ctx, a, argc, 1) : 0;
    if (from < 0) from += (long)arr->len;
    if (from < 0) from = 0;
    for (long i = from; i < (long)arr->len; i++)
        if (strict_eq(arr->items[i], a[0])) return jengine_make_number((double)i);
    return jengine_make_number(-1);
}
static jengine_value m_arr_includes(struct jengine_context* ctx, jengine_value t,
                                    jengine_value* a, int argc) {
    (void)ctx;
    jg_array* arr = as_array(t);
    if (!arr || argc < 1) return BOOLV(false);
    for (size_t i = 0; i < arr->len; i++) {
        if (strict_eq(arr->items[i], a[0])) return BOOLV(true);
        if (jengine_is_number(arr->items[i]) && jengine_is_number(a[0]) &&
            jengine_to_number(arr->items[i]) != jengine_to_number(arr->items[i]) &&
            jengine_to_number(a[0]) != jengine_to_number(a[0])) return BOOLV(true);
    }
    return BOOLV(false);
}
static jengine_value m_arr_index_of_fn(struct jengine_context* ctx, jengine_value t,
                                       jengine_value* a, int argc) {
    return m_arr_indexOf(ctx, t, a, argc);
}
static jengine_value m_arr_reverse(struct jengine_context* ctx, jengine_value t,
                                   jengine_value* a, int argc) {
    (void)ctx; (void)a; (void)argc;
    jg_array* arr = as_array(t);
    if (!arr) return t;
    for (size_t i = 0; i < arr->len / 2; i++) {
        jengine_value tmp = arr->items[i];
        arr->items[i] = arr->items[arr->len - 1 - i];
        arr->items[arr->len - 1 - i] = tmp;
    }
    return t;
}
static jengine_value m_arr_forEach(struct jengine_context* ctx, jengine_value t,
                                   jengine_value* a, int argc) {
    jg_array* arr = as_array(t);
    if (!arr || argc < 1 || !jengine_is_function(a[0])) return UNDEF;
    for (size_t i = 0; i < arr->len; i++) {
        jengine_value args[3] = { arr->items[i], jengine_make_number((double)i), t };
        jengine_call(ctx, a[0], UNDEF, args, 3);
        if (ctx->has_exception) return UNDEF;
    }
    return UNDEF;
}
static jengine_value m_arr_map(struct jengine_context* ctx, jengine_value t,
                               jengine_value* a, int argc) {
    jg_array* arr = as_array(t);
    if (!arr || argc < 1 || !jengine_is_function(a[0])) return UNDEF;
    jengine_value out = jengine_make_array(ctx, arr->len + 1);
    for (size_t i = 0; i < arr->len; i++) {
        jengine_value args[3] = { arr->items[i], jengine_make_number((double)i), t };
        jengine_value r = jengine_call(ctx, a[0], UNDEF, args, 3);
        if (ctx->has_exception) return UNDEF;
        jengine_array_push(ctx, out, r);
    }
    return out;
}
static jengine_value m_arr_filter(struct jengine_context* ctx, jengine_value t,
                                  jengine_value* a, int argc) {
    jg_array* arr = as_array(t);
    if (!arr || argc < 1 || !jengine_is_function(a[0])) return UNDEF;
    jengine_value out = jengine_make_array(ctx, 8);
    for (size_t i = 0; i < arr->len; i++) {
        jengine_value args[3] = { arr->items[i], jengine_make_number((double)i), t };
        jengine_value r = jengine_call(ctx, a[0], UNDEF, args, 3);
        if (ctx->has_exception) return UNDEF;
        if (truthy(r)) jengine_array_push(ctx, out, arr->items[i]);
    }
    return out;
}
static jengine_value m_arr_some(struct jengine_context* ctx, jengine_value t,
                                jengine_value* a, int argc) {
    jg_array* arr = as_array(t);
    if (!arr || argc < 1 || !jengine_is_function(a[0])) return BOOLV(false);
    for (size_t i = 0; i < arr->len; i++) {
        jengine_value args[3] = { arr->items[i], jengine_make_number((double)i), t };
        jengine_value r = jengine_call(ctx, a[0], UNDEF, args, 3);
        if (ctx->has_exception) return UNDEF;
        if (truthy(r)) return BOOLV(true);
    }
    return BOOLV(false);
}
static jengine_value m_arr_every(struct jengine_context* ctx, jengine_value t,
                                 jengine_value* a, int argc) {
    jg_array* arr = as_array(t);
    if (!arr || argc < 1 || !jengine_is_function(a[0])) return BOOLV(false);
    for (size_t i = 0; i < arr->len; i++) {
        jengine_value args[3] = { arr->items[i], jengine_make_number((double)i), t };
        jengine_value r = jengine_call(ctx, a[0], UNDEF, args, 3);
        if (ctx->has_exception) return UNDEF;
        if (!truthy(r)) return BOOLV(false);
    }
    return BOOLV(true);
}
static jengine_value m_arr_find(struct jengine_context* ctx, jengine_value t,
                                jengine_value* a, int argc) {
    jg_array* arr = as_array(t);
    if (!arr || argc < 1 || !jengine_is_function(a[0])) return UNDEF;
    for (size_t i = 0; i < arr->len; i++) {
        jengine_value args[3] = { arr->items[i], jengine_make_number((double)i), t };
        jengine_value r = jengine_call(ctx, a[0], UNDEF, args, 3);
        if (ctx->has_exception) return UNDEF;
        if (truthy(r)) return arr->items[i];
    }
    return UNDEF;
}
static jengine_value m_arr_findIndex(struct jengine_context* ctx, jengine_value t,
                                     jengine_value* a, int argc) {
    jg_array* arr = as_array(t);
    if (!arr || argc < 1 || !jengine_is_function(a[0])) return jengine_make_number(-1);
    for (size_t i = 0; i < arr->len; i++) {
        jengine_value args[3] = { arr->items[i], jengine_make_number((double)i), t };
        jengine_value r = jengine_call(ctx, a[0], UNDEF, args, 3);
        if (ctx->has_exception) return UNDEF;
        if (truthy(r)) return jengine_make_number((double)i);
    }
    return jengine_make_number(-1);
}
static jengine_value m_arr_reduce(struct jengine_context* ctx, jengine_value t,
                                  jengine_value* a, int argc) {
    jg_array* arr = as_array(t);
    if (!arr || argc < 1 || !jengine_is_function(a[0])) return UNDEF;
    size_t i = 0;
    jengine_value acc;
    if (argc >= 2) acc = a[1];
    else {
        if (arr->len == 0) { ctx->has_exception = true; ctx->exception = jengine_make_string(ctx, "TypeError: reduce of empty array"); return UNDEF; }
        acc = arr->items[0];
        i = 1;
    }
    for (; i < arr->len; i++) {
        jengine_value args[4] = { acc, arr->items[i], jengine_make_number((double)i), t };
        acc = jengine_call(ctx, a[0], UNDEF, args, 4);
        if (ctx->has_exception) return UNDEF;
    }
    return acc;
}
static jengine_value m_arr_sort(struct jengine_context* ctx, jengine_value t,
                                jengine_value* a, int argc) {
    jg_array* arr = as_array(t);
    if (!arr) return t;
    bool has_cmp = (argc > 0 && jengine_is_function(a[0]));
    for (size_t i = 1; i < arr->len; i++) {
        jengine_value key = arr->items[i];
        size_t j = i;
        while (j > 0) {
            double cmp;
            if (has_cmp) {
                jengine_value args[2] = { arr->items[j - 1], key };
                jengine_value r = jengine_call(ctx, a[0], UNDEF, args, 2);
                if (ctx->has_exception) return t;
                cmp = to_number(ctx->I, r);
            } else {
                char* x = value_to_string(ctx->I, arr->items[j - 1]);
                char* y = value_to_string(ctx->I, key);
                cmp = (double)strcmp(x ? x : "", y ? y : "");
                free(x); free(y);
            }
            if (cmp <= 0) break;
            arr->items[j] = arr->items[j - 1];
            j--;
        }
        arr->items[j] = key;
    }
    return t;
}
static jengine_value m_arr_toString(struct jengine_context* ctx, jengine_value t,
                                    jengine_value* a, int argc) {
    return m_arr_join(ctx, t, a, argc);
}

/* ---------- string methods ---------- */

static jengine_value m_str_charAt(struct jengine_context* ctx, jengine_value t,
                                  jengine_value* a, int argc) {
    char* s = self_cstr(ctx, t);
    long len = (long)strlen(s);
    long i = (long)narg(ctx, a, argc, 0);
    if (i < 0 || i >= len) { free(s); return jengine_make_string(ctx, ""); }
    jengine_value r = jengine_make_string_len(ctx, s + i, 1);
    free(s);
    return r;
}
static jengine_value m_str_charCodeAt(struct jengine_context* ctx, jengine_value t,
                                      jengine_value* a, int argc) {
    char* s = self_cstr(ctx, t);
    long len = (long)strlen(s);
    long i = (long)narg(ctx, a, argc, 0);
    if (i < 0 || i >= len) { free(s); return jengine_make_number(NAN); }
    double c = (double)(unsigned char)s[i];
    free(s);
    return jengine_make_number(c);
}
static jengine_value m_str_indexOf(struct jengine_context* ctx, jengine_value t,
                                   jengine_value* a, int argc) {
    char* s = self_cstr(ctx, t);
    char* sub = (argc > 0) ? self_cstr(ctx, a[0]) : xstrdup("undefined");
    long from = (argc > 1) ? (long)narg(ctx, a, argc, 1) : 0;
    if (from < 0) from = 0;
    const char* p = (from <= (long)strlen(s)) ? strstr(s + from, sub) : NULL;
    double r = p ? (double)(p - s) : -1.0;
    free(s); free(sub);
    return jengine_make_number(r);
}
static jengine_value m_str_lastIndexOf(struct jengine_context* ctx, jengine_value t,
                                       jengine_value* a, int argc) {
    char* s = self_cstr(ctx, t);
    char* sub = (argc > 0) ? self_cstr(ctx, a[0]) : xstrdup("undefined");
    long slen = (long)strlen(s), sublen = (long)strlen(sub);
    long last = -1;
    for (long i = 0; i + sublen <= slen; i++)
        if (strncmp(s + i, sub, (size_t)sublen) == 0) last = i;
    free(s); free(sub);
    return jengine_make_number((double)last);
}
static jengine_value m_str_includes(struct jengine_context* ctx, jengine_value t,
                                    jengine_value* a, int argc) {
    char* s = self_cstr(ctx, t);
    char* sub = (argc > 0) ? self_cstr(ctx, a[0]) : xstrdup("");
    bool r = strstr(s, sub) != NULL;
    free(s); free(sub);
    return BOOLV(r);
}
static jengine_value m_str_startsWith(struct jengine_context* ctx, jengine_value t,
                                      jengine_value* a, int argc) {
    char* s = self_cstr(ctx, t);
    char* sub = (argc > 0) ? self_cstr(ctx, a[0]) : xstrdup("");
    long from = (argc > 1) ? (long)narg(ctx, a, argc, 1) : 0;
    bool r = (from >= 0 && strncmp(s + from, sub, strlen(sub)) == 0);
    free(s); free(sub);
    return BOOLV(r);
}
static jengine_value m_str_endsWith(struct jengine_context* ctx, jengine_value t,
                                    jengine_value* a, int argc) {
    char* s = self_cstr(ctx, t);
    char* sub = (argc > 0) ? self_cstr(ctx, a[0]) : xstrdup("");
    long slen = (long)strlen(s), sublen = (long)strlen(sub);
    bool r = (sublen <= slen) && (strcmp(s + slen - sublen, sub) == 0);
    free(s); free(sub);
    return BOOLV(r);
}
static jengine_value m_str_slice(struct jengine_context* ctx, jengine_value t,
                                 jengine_value* a, int argc) {
    char* s = self_cstr(ctx, t);
    long len = (long)strlen(s);
    long start = (long)narg(ctx, a, argc, 0);
    long end = (argc > 1 && !jengine_is_undefined(a[1])) ? (long)narg(ctx, a, argc, 1) : len;
    if (start < 0) start += len;
    if (end < 0) end += len;
    if (start < 0) start = 0;
    if (end > len) end = len;
    if (end < start) end = start;
    jengine_value r = jengine_make_string_len(ctx, s + start, (size_t)(end - start));
    free(s);
    return r;
}
static jengine_value m_str_substring(struct jengine_context* ctx, jengine_value t,
                                     jengine_value* a, int argc) {
    char* s = self_cstr(ctx, t);
    long len = (long)strlen(s);
    long start = (long)narg(ctx, a, argc, 0);
    long end = (argc > 1 && !jengine_is_undefined(a[1])) ? (long)narg(ctx, a, argc, 1) : len;
    if (start < 0) start = 0;
    if (end < 0) end = 0;
    if (start > len) start = len;
    if (end > len) end = len;
    if (start > end) { long tmp = start; start = end; end = tmp; }
    jengine_value r = jengine_make_string_len(ctx, s + start, (size_t)(end - start));
    free(s);
    return r;
}
static jengine_value m_str_substr(struct jengine_context* ctx, jengine_value t,
                                  jengine_value* a, int argc) {
    char* s = self_cstr(ctx, t);
    long len = (long)strlen(s);
    long start = (long)narg(ctx, a, argc, 0);
    long count = (argc > 1) ? (long)narg(ctx, a, argc, 1) : (len - start);
    if (start < 0) start += len;
    if (start < 0) start = 0;
    if (start > len) start = len;
    if (count < 0) count = 0;
    if (start + count > len) count = len - start;
    jengine_value r = jengine_make_string_len(ctx, s + start, (size_t)count);
    free(s);
    return r;
}
static jengine_value m_str_toUpperCase(struct jengine_context* ctx, jengine_value t,
                                       jengine_value* a, int argc) {
    (void)a; (void)argc;
    char* s = self_cstr(ctx, t);
    for (char* p = s; *p; p++) *p = (char)toupper((unsigned char)*p);
    jengine_value r = jengine_make_string(ctx, s);
    free(s);
    return r;
}
static jengine_value m_str_toLowerCase(struct jengine_context* ctx, jengine_value t,
                                       jengine_value* a, int argc) {
    (void)a; (void)argc;
    char* s = self_cstr(ctx, t);
    for (char* p = s; *p; p++) *p = (char)tolower((unsigned char)*p);
    jengine_value r = jengine_make_string(ctx, s);
    free(s);
    return r;
}
static jengine_value m_str_trim(struct jengine_context* ctx, jengine_value t,
                                jengine_value* a, int argc) {
    (void)a; (void)argc;
    char* s = self_cstr(ctx, t);
    char* p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    char* e = p + strlen(p);
    while (e > p && isspace((unsigned char)e[-1])) *--e = 0;
    jengine_value r = jengine_make_string(ctx, p);
    free(s);
    return r;
}
static jengine_value m_str_split(struct jengine_context* ctx, jengine_value t,
                                 jengine_value* a, int argc) {
    char* s = self_cstr(ctx, t);
    jengine_value out = jengine_make_array(ctx, 8);
    if (argc == 0 || jengine_is_undefined(a[0])) {
        jengine_array_push(ctx, out, jengine_make_string(ctx, s));
        free(s);
        return out;
    }
    char* sep = self_cstr(ctx, a[0]);
    if (strlen(sep) == 0) {
        for (size_t i = 0; s[i]; i++) jengine_array_push(ctx, out, jengine_make_string_len(ctx, s + i, 1));
    } else {
        char* cur = s;
        char* p;
        while ((p = strstr(cur, sep)) != NULL) {
            jengine_array_push(ctx, out, jengine_make_string_len(ctx, cur, (size_t)(p - cur)));
            cur = p + strlen(sep);
        }
        jengine_array_push(ctx, out, jengine_make_string(ctx, cur));
    }
    free(s); free(sep);
    return out;
}
static jengine_value m_str_replace(struct jengine_context* ctx, jengine_value t,
                                   jengine_value* a, int argc) {
    char* s = self_cstr(ctx, t);
    char* find = (argc > 0) ? self_cstr(ctx, a[0]) : xstrdup("");
    char* repl = (argc > 1) ? self_cstr(ctx, a[1]) : xstrdup("undefined");
    char* p = strstr(s, find);
    if (!p || strlen(find) == 0) { free(find); free(repl); return jengine_make_string(ctx, s); }
    SB sb; sb_init(&sb);
    sb_putn(&sb, s, (size_t)(p - s));
    sb_puts(&sb, repl);
    sb_puts(&sb, p + strlen(find));
    jengine_value r = jengine_make_string(ctx, sb.data ? sb.data : "");
    free(sb.data); free(s); free(find); free(repl);
    return r;
}
static jengine_value m_str_replaceAll(struct jengine_context* ctx, jengine_value t,
                                      jengine_value* a, int argc) {
    char* s = self_cstr(ctx, t);
    char* find = (argc > 0) ? self_cstr(ctx, a[0]) : xstrdup("");
    char* repl = (argc > 1) ? self_cstr(ctx, a[1]) : xstrdup("undefined");
    if (strlen(find) == 0) { free(find); free(repl); return jengine_make_string(ctx, s); }
    SB sb; sb_init(&sb);
    char* cur = s;
    char* p;
    while ((p = strstr(cur, find)) != NULL) {
        sb_putn(&sb, cur, (size_t)(p - cur));
        sb_puts(&sb, repl);
        cur = p + strlen(find);
    }
    sb_puts(&sb, cur);
    jengine_value r = jengine_make_string(ctx, sb.data ? sb.data : "");
    free(sb.data); free(s); free(find); free(repl);
    return r;
}
static jengine_value m_str_repeat(struct jengine_context* ctx, jengine_value t,
                                  jengine_value* a, int argc) {
    char* s = self_cstr(ctx, t);
    int n = (int)narg(ctx, a, argc, 0);
    if (n < 0) n = 0;
    SB sb; sb_init(&sb);
    for (int i = 0; i < n; i++) sb_puts(&sb, s);
    jengine_value r = jengine_make_string(ctx, sb.data ? sb.data : "");
    free(sb.data); free(s);
    return r;
}
static jengine_value m_str_concat(struct jengine_context* ctx, jengine_value t,
                                  jengine_value* a, int argc) {
    char* s = self_cstr(ctx, t);
    SB sb; sb_init(&sb);
    sb_puts(&sb, s);
    for (int i = 0; i < argc; i++) {
        char* x = self_cstr(ctx, a[i]);
        sb_puts(&sb, x);
        free(x);
    }
    jengine_value r = jengine_make_string(ctx, sb.data ? sb.data : "");
    free(sb.data); free(s);
    return r;
}
static jengine_value m_str_trimStart(struct jengine_context* ctx, jengine_value t,
                                     jengine_value* a, int argc) {
    (void)a; (void)argc;
    char* s = self_cstr(ctx, t);
    char* p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    jengine_value r = jengine_make_string(ctx, p);
    free(s);
    return r;
}
static jengine_value m_str_trimEnd(struct jengine_context* ctx, jengine_value t,
                                   jengine_value* a, int argc) {
    (void)a; (void)argc;
    char* s = self_cstr(ctx, t);
    char* e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    jengine_value r = jengine_make_string(ctx, s);
    free(s);
    return r;
}
static jengine_value m_str_toString(struct jengine_context* ctx, jengine_value t,
                                    jengine_value* a, int argc) {
    (void)a; (void)argc;
    char* s = self_cstr(ctx, t);
    jengine_value r = jengine_make_string(ctx, s);
    free(s);
    return r;
}

/* ---------- number methods ---------- */

static jengine_value m_num_toString(struct jengine_context* ctx, jengine_value t,
                                    jengine_value* a, int argc) {
    double d = jengine_is_number(t) ? jengine_to_number(t) : narg(ctx, a, argc, 0);
    int radix = (int)narg(ctx, a, argc, 0);
    if (argc > 0 && radix >= 2 && radix <= 36 && d == (double)(long long)d) {
        char buf[80];
        long long v = (long long)d;
        char* p = buf + sizeof buf;
        *--p = 0;
        int neg = v < 0;
        unsigned long long uv = neg ? (unsigned long long)(-v) : (unsigned long long)v;
        if (uv == 0) *--p = '0';
        while (uv) {
            int dig = (int)(uv % (unsigned)radix);
            *--p = (char)(dig < 10 ? '0' + dig : 'a' + dig - 10);
            uv /= (unsigned)radix;
        }
        if (neg) *--p = '-';
        return jengine_make_string(ctx, p);
    }
    char* s = num_to_cstr(d);
    jengine_value r = jengine_make_string(ctx, s ? s : "0");
    free(s);
    return r;
}
static jengine_value m_num_toFixed(struct jengine_context* ctx, jengine_value t,
                                   jengine_value* a, int argc) {
    double d = jengine_is_number(t) ? jengine_to_number(t) : narg(ctx, a, argc, 0);
    int digits = (int)narg(ctx, a, argc, 0);
    if (digits < 0) digits = 0;
    if (digits > 20) digits = 20;
    char buf[512];
    snprintf(buf, sizeof buf, "%.*f", digits, d);
    return jengine_make_string(ctx, buf);
}
static jengine_value m_num_valueOf(struct jengine_context* ctx, jengine_value t,
                                   jengine_value* a, int argc) {
    (void)ctx; (void)a; (void)argc;
    return jengine_is_number(t) ? t : jengine_make_number(0);
}

/* ---------- Math ---------- */
#define MATH1(name, fn) \
    static jengine_value m_math_##name(struct jengine_context* ctx, jengine_value t, \
                                       jengine_value* a, int argc) { \
        (void)t; return jengine_make_number(fn(narg(ctx, a, argc, 0))); }

MATH1(abs, fabs)
MATH1(floor, floor)
MATH1(ceil, ceil)
MATH1(round, round)
MATH1(trunc, trunc)
MATH1(sqrt, sqrt)
MATH1(cbrt, cbrt)
MATH1(log, log)
MATH1(log2, log2)
MATH1(log10, log10)
MATH1(exp, exp)
MATH1(sin, sin)
MATH1(cos, cos)
MATH1(tan, tan)
MATH1(asin, asin)
MATH1(acos, acos)
MATH1(atan, atan)
#undef MATH1

static jengine_value m_math_sign(struct jengine_context* ctx, jengine_value t,
                                 jengine_value* a, int argc) {
    (void)t;
    double d = narg(ctx, a, argc, 0);
    return jengine_make_number(d > 0 ? 1 : (d < 0 ? -1 : d));
}
static jengine_value m_math_pow(struct jengine_context* ctx, jengine_value t,
                                jengine_value* a, int argc) {
    (void)t;
    return jengine_make_number(pow(narg(ctx, a, argc, 0), narg(ctx, a, argc, 1)));
}
static jengine_value m_math_atan2(struct jengine_context* ctx, jengine_value t,
                                  jengine_value* a, int argc) {
    (void)t;
    return jengine_make_number(atan2(narg(ctx, a, argc, 0), narg(ctx, a, argc, 1)));
}
static jengine_value m_math_min(struct jengine_context* ctx, jengine_value t,
                                jengine_value* a, int argc) {
    (void)t;
    if (argc == 0) return jengine_make_number(INFINITY);
    double m = narg(ctx, a, argc, 0);
    for (int i = 1; i < argc; i++) { double v = narg(ctx, a, argc, i); if (v < m) m = v; }
    return jengine_make_number(m);
}
static jengine_value m_math_max(struct jengine_context* ctx, jengine_value t,
                                jengine_value* a, int argc) {
    (void)t;
    if (argc == 0) return jengine_make_number(-INFINITY);
    double m = narg(ctx, a, argc, 0);
    for (int i = 1; i < argc; i++) { double v = narg(ctx, a, argc, i); if (v > m) m = v; }
    return jengine_make_number(m);
}
static jengine_value m_math_random(struct jengine_context* ctx, jengine_value t,
                                   jengine_value* a, int argc) {
    (void)ctx; (void)t; (void)a; (void)argc;
    return jengine_make_number((double)rand() / ((double)RAND_MAX + 1.0));
}

/* ---------- JSON ---------- */

static void json_quote(SB* sb, const char* s) {
    sb_putc(sb, '"');
    for (const char* p = s; *p; p++) {
        char c = *p;
        switch (c) {
            case '"': sb_puts(sb, "\\\""); break;
            case '\\': sb_puts(sb, "\\\\"); break;
            case '\n': sb_puts(sb, "\\n"); break;
            case '\r': sb_puts(sb, "\\r"); break;
            case '\t': sb_puts(sb, "\\t"); break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof buf, "\\u%04x", (unsigned char)c);
                    sb_puts(sb, buf);
                } else {
                    sb_putc(sb, c);
                }
        }
    }
    sb_putc(sb, '"');
}

static void json_write(SB* sb, jengine_value v) {
    switch (jengine_value_type(v)) {
        case JENGINE_T_NUMBER: {
            double d = jengine_to_number(v);
            if (d != d || isinf(d)) { sb_puts(sb, "null"); break; }
            char* s = num_to_cstr(d);
            sb_puts(sb, s ? s : "0");
            free(s);
            break;
        }
        case JENGINE_T_STRING: json_quote(sb, string_data(v)); break;
        case JENGINE_T_BOOLEAN: sb_puts(sb, (v & 1) ? "true" : "false"); break;
        case JENGINE_T_NULL: case JENGINE_T_UNDEFINED: sb_puts(sb, "null"); break;
        case JENGINE_T_ARRAY: {
            jg_array* a = as_array(v);
            sb_putc(sb, '[');
            for (size_t i = 0; a && i < a->len; i++) {
                if (i) sb_putc(sb, ',');
                json_write(sb, a->items[i]);
            }
            sb_putc(sb, ']');
            break;
        }
        case JENGINE_T_OBJECT: {
            jg_object* o = as_object(v);
            sb_putc(sb, '{');
            for (int i = 0; o && i < o->count; i++) {
                if (i) sb_putc(sb, ',');
                json_quote(sb, o->keys[i]);
                sb_putc(sb, ':');
                json_write(sb, o->vals[i]);
            }
            sb_putc(sb, '}');
            break;
        }
        default: sb_puts(sb, "null"); break;
    }
}

static jengine_value m_json_stringify(struct jengine_context* ctx, jengine_value t,
                                      jengine_value* a, int argc) {
    (void)t;
    if (argc < 1) return UNDEF;
    SB sb; sb_init(&sb);
    json_write(&sb, a[0]);
    jengine_value r = jengine_make_string(ctx, sb.data ? sb.data : "");
    free(sb.data);
    return r;
}

static jengine_value m_json_parse(struct jengine_context* ctx, jengine_value t,
                                  jengine_value* a, int argc) {
    (void)t;
    if (argc < 1 || !jengine_is_string(a[0])) return UNDEF;
    Interp* I = ctx->I;
    Parser p;
    memset(&p, 0, sizeof p);
    p.ctx = ctx;
    lex_init(&p.lx, string_data(a[0]));
    next(&p);
    Node* n = parse_expression(&p);
    if (p.err) { set_exception(I, jengine_make_string(ctx, p.errmsg)); return UNDEF; }
    return ev(I, n);
}

/* ---------- console ---------- */

static jengine_value m_console_log(struct jengine_context* ctx, jengine_value t,
                                   jengine_value* a, int argc) {
    (void)t;
    for (int i = 0; i < argc; i++) {
        if (i) fputc(' ', stdout);
        if (jengine_is_string(a[i])) fputs(string_data(a[i]), stdout);
        else {
            char* s = debug_str(a[i]);
            fputs(s ? s : "", stdout);
            free(s);
        }
    }
    fputc('\n', stdout);
    (void)ctx;
    return UNDEF;
}

/* ---------- globals ---------- */

static jengine_value m_parseInt(struct jengine_context* ctx, jengine_value t,
                                jengine_value* a, int argc) {
    (void)t;
    char* s = (argc > 0) ? self_cstr(ctx, a[0]) : xstrdup("");
    int radix = (argc > 1) ? (int)narg(ctx, a, argc, 1) : 10;
    while (*s && isspace((unsigned char)*s)) s++;
    char* p = s;
    int neg = 0;
    if (*p == '+' || *p == '-') { neg = (*p == '-'); p++; }
    if (radix == 10 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { radix = 16; p += 2; }
    if (radix == 0) radix = (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) ? (p += 2, 16) : 10;
    long long val = 0;
    int any = 0;
    for (; *p; p++) {
        int d;
        if (*p >= '0' && *p <= '9') d = *p - '0';
        else if (*p >= 'a' && *p <= 'z') d = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'Z') d = *p - 'A' + 10;
        else break;
        if (d >= radix) break;
        val = val * radix + d;
        any = 1;
    }
    free(s);
    if (!any) return jengine_make_number(NAN);
    return jengine_make_number((double)(neg ? -val : val));
}
static jengine_value m_parseFloat(struct jengine_context* ctx, jengine_value t,
                                  jengine_value* a, int argc) {
    (void)t;
    char* s = (argc > 0) ? self_cstr(ctx, a[0]) : xstrdup("");
    char* end = NULL;
    double d = strtod(s, &end);
    if (end == s) d = NAN;
    free(s);
    return jengine_make_number(d);
}
static jengine_value m_isNaN(struct jengine_context* ctx, jengine_value t,
                             jengine_value* a, int argc) {
    (void)t;
    double d = narg(ctx, a, argc, 0);
    return BOOLV(d != d);
}
static jengine_value m_isFinite(struct jengine_context* ctx, jengine_value t,
                                jengine_value* a, int argc) {
    (void)t;
    double d = narg(ctx, a, argc, 0);
    return BOOLV(isfinite(d));
}
static jengine_value m_Number(struct jengine_context* ctx, jengine_value t,
                              jengine_value* a, int argc) {
    (void)t;
    return jengine_make_number(narg(ctx, a, argc, 0));
}
static jengine_value m_String(struct jengine_context* ctx, jengine_value t,
                              jengine_value* a, int argc) {
    (void)t;
    if (argc == 0) return jengine_make_string(ctx, "");
    jengine_value v = a[0];
    /* objects with their own toString() (e.g. errors) stringify via it */
    if (jengine_is_object(v)) {
        jengine_value fn;
        if (obj_get_raw(v, "toString", &fn) && jengine_is_function(fn)) {
            jengine_value r = jengine_call(ctx, fn, v, NULL, 0);
            if (jengine_is_string(r)) return r;
        }
    }
    char* s = self_cstr(ctx, v);
    jengine_value r = jengine_make_string(ctx, s);
    free(s);
    return r;
}
static jengine_value m_Boolean(struct jengine_context* ctx, jengine_value t,
                               jengine_value* a, int argc) {
    (void)ctx; (void)t;
    return BOOLV(argc > 0 && truthy(a[0]));
}
static jengine_value m_Object(struct jengine_context* ctx, jengine_value t,
                              jengine_value* a, int argc) {
    (void)t;
    if (argc > 0 && jengine_is_object(a[0])) return a[0];
    return jengine_make_object(ctx);
}
static jengine_value m_Array(struct jengine_context* ctx, jengine_value t,
                             jengine_value* a, int argc) {
    (void)t;
    if (argc == 1 && jengine_is_number(a[0])) {
        size_t n = (size_t)jengine_to_number(a[0]);
        jengine_value arr = jengine_make_array(ctx, n + 1);
        jg_array* ar = as_array(arr);
        for (size_t i = 0; ar && i < n; i++) ar->items[i] = UNDEF;
        if (ar) ar->len = n;
        return arr;
    }
    jengine_value arr = jengine_make_array(ctx, (size_t)argc + 1);
    for (int i = 0; i < argc; i++) jengine_array_push(ctx, arr, a[i]);
    return arr;
}
static jengine_value m_Object_keys(struct jengine_context* ctx, jengine_value t,
                                   jengine_value* a, int argc) {
    (void)t;
    jengine_value out = jengine_make_array(ctx, 8);
    if (argc < 1) return out;
    jengine_value o = a[0];
    if (jengine_is_object(o)) {
        jg_object* obj = as_object(o);
        for (int i = 0; obj && i < obj->count; i++)
            jengine_array_push(ctx, out, jengine_make_string(ctx, obj->keys[i]));
    } else if (jengine_is_array(o)) {
        jg_array* ar = as_array(o);
        for (size_t i = 0; ar && i < ar->len; i++) {
            char buf[32]; snprintf(buf, sizeof buf, "%zu", i);
            jengine_array_push(ctx, out, jengine_make_string(ctx, buf));
        }
    }
    return out;
}
static jengine_value m_Object_values(struct jengine_context* ctx, jengine_value t,
                                     jengine_value* a, int argc) {
    (void)t;
    jengine_value out = jengine_make_array(ctx, 8);
    if (argc < 1) return out;
    jengine_value o = a[0];
    if (jengine_is_object(o)) {
        jg_object* obj = as_object(o);
        for (int i = 0; obj && i < obj->count; i++) jengine_array_push(ctx, out, obj->vals[i]);
    } else if (jengine_is_array(o)) {
        jg_array* ar = as_array(o);
        for (size_t i = 0; ar && i < ar->len; i++) jengine_array_push(ctx, out, ar->items[i]);
    }
    return out;
}
static jengine_value m_Object_entries(struct jengine_context* ctx, jengine_value t,
                                      jengine_value* a, int argc) {
    (void)t;
    jengine_value out = jengine_make_array(ctx, 8);
    if (argc < 1) return out;
    jengine_value o = a[0];
    if (jengine_is_object(o)) {
        jg_object* obj = as_object(o);
        for (int i = 0; obj && i < obj->count; i++) {
            jengine_value pair = jengine_make_array(ctx, 3);
            jengine_array_push(ctx, pair, jengine_make_string(ctx, obj->keys[i]));
            jengine_array_push(ctx, pair, obj->vals[i]);
            jengine_array_push(ctx, out, pair);
        }
    }
    return out;
}
static jengine_value m_Array_isArray(struct jengine_context* ctx, jengine_value t,
                                     jengine_value* a, int argc) {
    (void)ctx; (void)t;
    return BOOLV(argc > 0 && jengine_is_array(a[0]));
}
static jengine_value m_Array_from(struct jengine_context* ctx, jengine_value t,
                                  jengine_value* a, int argc) {
    (void)t;
    jengine_value out = jengine_make_array(ctx, 8);
    if (argc < 1) return out;
    if (jengine_is_array(a[0])) {
        jg_array* ar = as_array(a[0]);
        for (size_t i = 0; ar && i < ar->len; i++) jengine_array_push(ctx, out, ar->items[i]);
    } else if (jengine_is_string(a[0])) {
        const char* s = string_data(a[0]);
        for (size_t i = 0; s[i]; i++) jengine_array_push(ctx, out, jengine_make_string_len(ctx, s + i, 1));
    }
    return out;
}
static jengine_value m_Number_isInteger(struct jengine_context* ctx, jengine_value t,
                                        jengine_value* a, int argc) {
    (void)ctx; (void)t;
    if (argc < 1 || !jengine_is_number(a[0])) return BOOLV(false);
    double d = jengine_to_number(a[0]);
    return BOOLV(isfinite(d) && d == floor(d));
}
static jengine_value m_Number_isNaN(struct jengine_context* ctx, jengine_value t,
                                    jengine_value* a, int argc) {
    (void)ctx; (void)t;
    if (argc < 1 || !jengine_is_number(a[0])) return BOOLV(false);
    double d = jengine_to_number(a[0]);
    return BOOLV(d != d);
}
static jengine_value m_String_fromCharCode2(struct jengine_context* ctx, jengine_value t,
                                            jengine_value* a, int argc) {
    (void)t;
    SB sb; sb_init(&sb);
    for (int i = 0; i < argc; i++) {
        double d = narg(ctx, a, argc, i);
        sb_putc(&sb, (char)((int)d & 0xff));
    }
    jengine_value r = jengine_make_string(ctx, sb.data ? sb.data : "");
    free(sb.data);
    return r;
}

/* ==============================================================
 * Builtin installation
 * ============================================================== */

/* ---------- function methods ---------- */

static jengine_value m_fn_toString(struct jengine_context* ctx, jengine_value t,
                                   jengine_value* a, int argc) {
    (void)a; (void)argc;
    jg_func* f = as_func(t);
    SB sb; sb_init(&sb);
    sb_puts(&sb, "function ");
    if (f && f->name) sb_puts(&sb, f->name);
    sb_puts(&sb, "() { [native code] }");
    jengine_value r = jengine_make_string(ctx, sb.data ? sb.data : "");
    free(sb.data);
    return r;
}
static jengine_value m_fn_call(struct jengine_context* ctx, jengine_value t,
                               jengine_value* a, int argc) {
    if (!jengine_is_function(t)) return UNDEF;
    jengine_value thisv = (argc > 0) ? a[0] : UNDEF;
    return jengine_call(ctx, t, thisv, argc > 0 ? a + 1 : NULL, argc > 0 ? argc - 1 : 0);
}
static jengine_value m_fn_apply(struct jengine_context* ctx, jengine_value t,
                                jengine_value* a, int argc) {
    if (!jengine_is_function(t)) return UNDEF;
    jengine_value thisv = (argc > 0) ? a[0] : UNDEF;
    if (argc > 1 && jengine_is_array(a[1])) {
        jg_array* ar = as_array(a[1]);
        int n = (int)(ar ? ar->len : 0);
        jengine_value* args = n > 0 ? (jengine_value*)malloc(sizeof(jengine_value) * (size_t)n) : NULL;
        for (int i = 0; i < n; i++) args[i] = ar->items[i];
        jengine_value r = jengine_call(ctx, t, thisv, args, n);
        free(args);
        return r;
    }
    return jengine_call(ctx, t, thisv, NULL, 0);
}
static jengine_value m_fn_bind(struct jengine_context* ctx, jengine_value t,
                               jengine_value* a, int argc) {
    if (!jengine_is_function(t)) return UNDEF;
    jg_func* b = func_new(ctx);
    if (!b) return UNDEF;
    b->kind = 3;
    b->name = xstrdup("bound");
    b->bound_target = t;
    b->bound_this = (argc > 0) ? a[0] : UNDEF;
    b->bound_argc = (argc > 1) ? argc - 1 : 0;
    if (b->bound_argc > 0) {
        b->bound_args = (jengine_value*)malloc(sizeof(jengine_value) * (size_t)b->bound_argc);
        for (int i = 0; i < b->bound_argc; i++) b->bound_args[i] = a[1 + i];
    }
    return make_func_value(b);
}

/* ---------- errors ---------- */

static jengine_value m_error_toString(struct jengine_context* ctx, jengine_value t,
                                      jengine_value* a, int argc) {
    (void)a; (void)argc;
    const char* name = "Error";
    const char* msg = "";
    jengine_value v;
    if (obj_get_raw(t, "name", &v) && jengine_is_string(v)) name = string_data(v);
    if (obj_get_raw(t, "message", &v) && jengine_is_string(v)) msg = string_data(v);
    SB sb; sb_init(&sb);
    sb_puts(&sb, name);
    if (msg && *msg) { sb_puts(&sb, ": "); sb_puts(&sb, msg); }
    jengine_value r = jengine_make_string(ctx, sb.data ? sb.data : name);
    free(sb.data);
    return r;
}
static jengine_value make_error_type(struct jengine_context* ctx, const char* name,
                                     jengine_value* a, int argc) {
    char* msg = NULL;
    if (argc > 0) msg = ctx->I ? value_to_string(ctx->I, a[0]) : NULL;
    jengine_value e;
    if (ctx->I) {
        e = jg_make_error(ctx->I, name, msg ? msg : "");
    } else {
        e = jengine_make_object(ctx);
        obj_set_raw(e, "name", jengine_make_string(ctx, name));
        obj_set_raw(e, "message", jengine_make_string(ctx, msg ? msg : ""));
    }
    free(msg);
    return e;
}
static jengine_value m_Error(struct jengine_context* ctx, jengine_value t,
                             jengine_value* a, int argc) {
    (void)t; return make_error_type(ctx, "Error", a, argc);
}
static jengine_value m_TypeError(struct jengine_context* ctx, jengine_value t,
                                 jengine_value* a, int argc) {
    (void)t; return make_error_type(ctx, "TypeError", a, argc);
}
static jengine_value m_RangeError(struct jengine_context* ctx, jengine_value t,
                                  jengine_value* a, int argc) {
    (void)t; return make_error_type(ctx, "RangeError", a, argc);
}
static jengine_value m_SyntaxError(struct jengine_context* ctx, jengine_value t,
                                   jengine_value* a, int argc) {
    (void)t; return make_error_type(ctx, "SyntaxError", a, argc);
}
static jengine_value m_ReferenceError(struct jengine_context* ctx, jengine_value t,
                                      jengine_value* a, int argc) {
    (void)t; return make_error_type(ctx, "ReferenceError", a, argc);
}

/* ---------- extra array methods ---------- */

static void flatten_into(struct jengine_context* ctx, jengine_value out, jengine_value src, int depth) {
    if (!jengine_is_array(src)) { jengine_array_push(ctx, out, src); return; }
    jg_array* a = as_array(src);
    for (size_t i = 0; a && i < a->len; i++) {
        if (depth > 0 && jengine_is_array(a->items[i])) flatten_into(ctx, out, a->items[i], depth - 1);
        else jengine_array_push(ctx, out, a->items[i]);
    }
}
static jengine_value m_arr_flat(struct jengine_context* ctx, jengine_value t,
                                jengine_value* a, int argc) {
    jengine_value out = jengine_make_array(ctx, 8);
    int depth = (argc > 0) ? (int)narg(ctx, a, argc, 0) : 1;
    flatten_into(ctx, out, t, depth);
    return out;
}
static jengine_value m_arr_flatMap(struct jengine_context* ctx, jengine_value t,
                                   jengine_value* a, int argc) {
    jengine_value mapped = m_arr_map(ctx, t, a, argc);
    jengine_value out = jengine_make_array(ctx, 8);
    if (jengine_is_array(mapped)) {
        jg_array* m = as_array(mapped);
        for (size_t i = 0; m && i < m->len; i++) {
            if (jengine_is_array(m->items[i])) flatten_into(ctx, out, m->items[i], 0);
            else jengine_array_push(ctx, out, m->items[i]);
        }
    }
    return out;
}
static jengine_value m_arr_fill(struct jengine_context* ctx, jengine_value t,
                                jengine_value* a, int argc) {
    jg_array* arr = as_array(t);
    if (!arr) return t;
    jengine_value val = (argc > 0) ? a[0] : UNDEF;
    long start = (argc > 1) ? (long)narg(ctx, a, argc, 1) : 0;
    long end = (argc > 2) ? (long)narg(ctx, a, argc, 2) : (long)arr->len;
    if (start < 0) start += (long)arr->len;
    if (end < 0) end += (long)arr->len;
    if (start < 0) start = 0;
    if (end > (long)arr->len) end = (long)arr->len;
    for (long i = start; i < end; i++) arr->items[i] = val;
    return t;
}
static jengine_value m_arr_lastIndexOf(struct jengine_context* ctx, jengine_value t,
                                       jengine_value* a, int argc) {
    (void)ctx;
    jg_array* arr = as_array(t);
    if (!arr || argc < 1) return jengine_make_number(-1);
    for (long i = (long)arr->len - 1; i >= 0; i--)
        if (strict_eq(arr->items[i], a[0])) return jengine_make_number((double)i);
    return jengine_make_number(-1);
}
static jengine_value m_arr_at(struct jengine_context* ctx, jengine_value t,
                              jengine_value* a, int argc) {
    (void)ctx;
    jg_array* arr = as_array(t);
    if (!arr) return UNDEF;
    long i = (long)narg(ctx, a, argc, 0);
    if (i < 0) i += (long)arr->len;
    if (i < 0 || i >= (long)arr->len) return UNDEF;
    return arr->items[i];
}
static jengine_value m_arr_toReversed(struct jengine_context* ctx, jengine_value t,
                                      jengine_value* a, int argc) {
    jengine_value out = jengine_make_array(ctx, 8);
    jg_array* arr = as_array(t);
    for (long i = (arr ? (long)arr->len : 0) - 1; i >= 0; i--) jengine_array_push(ctx, out, arr->items[i]);
    (void)a; (void)argc;
    return out;
}

/* ---------- extra string methods ---------- */

static jengine_value m_str_at(struct jengine_context* ctx, jengine_value t,
                              jengine_value* a, int argc) {
    char* s = self_cstr(ctx, t);
    long len = (long)strlen(s);
    long i = (long)narg(ctx, a, argc, 0);
    if (i < 0) i += len;
    if (i < 0 || i >= len) { free(s); return UNDEF; }
    jengine_value r = jengine_make_string_len(ctx, s + i, 1);
    free(s);
    return r;
}
static jengine_value m_str_codePointAt(struct jengine_context* ctx, jengine_value t,
                                       jengine_value* a, int argc) {
    char* s = self_cstr(ctx, t);
    long len = (long)strlen(s);
    long i = (long)narg(ctx, a, argc, 0);
    if (i < 0 || i >= len) { free(s); return UNDEF; }
    double c = (double)(unsigned char)s[i];
    free(s);
    return jengine_make_number(c);
}
static jengine_value pad_impl(struct jengine_context* ctx, jengine_value t,
                              jengine_value* a, int argc, int at_start) {
    char* s = self_cstr(ctx, t);
    long target = (argc > 0) ? (long)narg(ctx, a, argc, 0) : 0;
    char* pad = (argc > 1 && !jengine_is_undefined(a[1])) ? self_cstr(ctx, a[1]) : xstrdup(" ");
    long slen = (long)strlen(s);
    long plen = (long)strlen(pad);
    SB sb; sb_init(&sb);
    if (target <= slen || plen == 0) {
        sb_puts(&sb, s);
    } else {
        long need = target - slen;
        if (at_start) {
            for (long i = 0; i < need; i++) sb_putc(&sb, pad[i % plen]);
            sb_puts(&sb, s);
        } else {
            sb_puts(&sb, s);
            for (long i = 0; i < need; i++) sb_putc(&sb, pad[i % plen]);
        }
    }
    jengine_value r = jengine_make_string(ctx, sb.data ? sb.data : "");
    free(s); free(pad); free(sb.data);
    return r;
}
static jengine_value m_str_padStart(struct jengine_context* ctx, jengine_value t,
                                    jengine_value* a, int argc) {
    return pad_impl(ctx, t, a, argc, 1);
}
static jengine_value m_str_padEnd(struct jengine_context* ctx, jengine_value t,
                                  jengine_value* a, int argc) {
    return pad_impl(ctx, t, a, argc, 0);
}

/* ---------- extra object / number methods ---------- */

static jengine_value m_Object_assign(struct jengine_context* ctx, jengine_value t,
                                     jengine_value* a, int argc) {
    (void)t;
    if (argc < 1) return UNDEF;
    jengine_value target = a[0];
    if (!jengine_is_object(target) && !jengine_is_array(target)) {
        target = jengine_make_object(ctx);
    }
    for (int i = 1; i < argc; i++) {
        jengine_value src = a[i];
        if (jengine_is_object(src)) {
            jg_object* o = as_object(src);
            for (int j = 0; o && j < o->count; j++) set_property(ctx->I, target, o->keys[j], o->vals[j]);
        } else if (jengine_is_array(src)) {
            jg_array* ar = as_array(src);
            for (size_t j = 0; ar && j < ar->len; j++) {
                char buf[32]; snprintf(buf, sizeof buf, "%zu", j);
                set_property(ctx->I, target, buf, ar->items[j]);
            }
        }
    }
    return target;
}
static jengine_value m_Object_freeze(struct jengine_context* ctx, jengine_value t,
                                     jengine_value* a, int argc) {
    (void)ctx; (void)t;
    return (argc > 0) ? a[0] : UNDEF;
}
static jengine_value m_Number_isFinite(struct jengine_context* ctx, jengine_value t,
                                       jengine_value* a, int argc) {
    (void)ctx; (void)t;
    if (argc < 1 || !jengine_is_number(a[0])) return BOOLV(false);
    return BOOLV(isfinite(jengine_to_number(a[0])));
}
static jengine_value m_Number_parseInt(struct jengine_context* ctx, jengine_value t,
                                       jengine_value* a, int argc) {
    (void)t; return m_parseInt(ctx, t, a, argc);
}
static jengine_value m_Number_parseFloat(struct jengine_context* ctx, jengine_value t,
                                         jengine_value* a, int argc) {
    (void)t; return m_parseFloat(ctx, t, a, argc);
}
static jengine_value m_math_hypot(struct jengine_context* ctx, jengine_value t,
                                  jengine_value* a, int argc) {
    (void)t;
    double sum = 0;
    for (int i = 0; i < argc; i++) { double v = narg(ctx, a, argc, i); sum += v * v; }
    return jengine_make_number(sqrt(sum));
}
static jengine_value m_math_log1p(struct jengine_context* ctx, jengine_value t,
                                  jengine_value* a, int argc) {
    (void)t; return jengine_make_number(log1p(narg(ctx, a, argc, 0)));
}
static jengine_value m_Object_keys(struct jengine_context* ctx, jengine_value t,
                                   jengine_value* a, int argc);

/* ---------- object methods ---------- */

static jengine_value m_obj_toString(struct jengine_context* ctx, jengine_value t,
                                    jengine_value* a, int argc) {
    (void)a; (void)argc;
    if (jengine_is_object(t)) return jengine_make_string(ctx, "[object Object]");
    if (jengine_is_array(t))  return jengine_make_string(ctx, "[object Array]");
    if (jengine_is_function(t)) return jengine_make_string(ctx, "[object Function]");
    return jengine_make_string(ctx, "[object Object]");
}
static jengine_value m_obj_valueOf(struct jengine_context* ctx, jengine_value t,
                                   jengine_value* a, int argc) {
    (void)ctx; (void)a; (void)argc;
    return t;
}
static jengine_value m_obj_hasOwnProperty(struct jengine_context* ctx, jengine_value t,
                                          jengine_value* a, int argc) {
    if (argc < 1) return BOOLV(false);
    char* key = self_cstr(ctx, a[0]);
    jengine_value out;
    bool found = obj_get_raw(t, key, &out);
    free(key);
    return BOOLV(found);
}

static void add_const(Interp* I, jengine_value obj, const char* name, double v) {
    (void)I;
    obj_set_raw(obj, name, jengine_make_number(v));
}
static void add_fn(Interp* I, jengine_value obj, const char* name, jg_native_fn fn) {
    obj_set_raw(obj, name, make_native(I, name, fn));
}

static void setup_globals(Interp* I) {
    struct jengine_context* ctx = I->ctx;
    jg_env* g = ctx->global_env;

    env_def(g, "undefined", UNDEF);
    env_def(g, "NaN", jengine_make_number(NAN));
    env_def(g, "Infinity", jengine_make_number(INFINITY));
    env_def(g, "globalThis", ctx->global_this);

    /* Math */
    jengine_value math = jengine_make_object(ctx);
    add_const(I, math, "PI", 3.14159265358979323846);
    add_const(I, math, "E", 2.71828182845904523536);
    add_const(I, math, "LN2", 0.69314718055994530942);
    add_const(I, math, "LN10", 2.30258509299404568402);
    add_const(I, math, "LOG2E", 1.44269504088896340736);
    add_const(I, math, "LOG10E", 0.43429448190325182765);
    add_const(I, math, "SQRT2", 1.41421356237309504880);
    add_const(I, math, "SQRT1_2", 0.70710678118654752440);
    add_fn(I, math, "abs", m_math_abs);
    add_fn(I, math, "floor", m_math_floor);
    add_fn(I, math, "ceil", m_math_ceil);
    add_fn(I, math, "round", m_math_round);
    add_fn(I, math, "trunc", m_math_trunc);
    add_fn(I, math, "sqrt", m_math_sqrt);
    add_fn(I, math, "cbrt", m_math_cbrt);
    add_fn(I, math, "sign", m_math_sign);
    add_fn(I, math, "pow", m_math_pow);
    add_fn(I, math, "log", m_math_log);
    add_fn(I, math, "log2", m_math_log2);
    add_fn(I, math, "log10", m_math_log10);
    add_fn(I, math, "log1p", m_math_log1p);
    add_fn(I, math, "exp", m_math_exp);
    add_fn(I, math, "sin", m_math_sin);
    add_fn(I, math, "cos", m_math_cos);
    add_fn(I, math, "tan", m_math_tan);
    add_fn(I, math, "asin", m_math_asin);
    add_fn(I, math, "acos", m_math_acos);
    add_fn(I, math, "atan", m_math_atan);
    add_fn(I, math, "atan2", m_math_atan2);
    add_fn(I, math, "hypot", m_math_hypot);
    add_fn(I, math, "min", m_math_min);
    add_fn(I, math, "max", m_math_max);
    add_fn(I, math, "random", m_math_random);
    ctx->math_obj = math;
    env_def(g, "Math", math);

    /* JSON */
    jengine_value json = jengine_make_object(ctx);
    add_fn(I, json, "stringify", m_json_stringify);
    add_fn(I, json, "parse", m_json_parse);
    ctx->json_obj = json;
    env_def(g, "JSON", json);

    /* console */
    jengine_value cons = jengine_make_object(ctx);
    add_fn(I, cons, "log", m_console_log);
    add_fn(I, cons, "info", m_console_log);
    add_fn(I, cons, "warn", m_console_log);
    add_fn(I, cons, "error", m_console_log);
    ctx->console_obj = cons;
    env_def(g, "console", cons);

    /* global functions */
    env_def(g, "parseInt", make_native(I, "parseInt", m_parseInt));
    env_def(g, "parseFloat", make_native(I, "parseFloat", m_parseFloat));
    env_def(g, "isNaN", make_native(I, "isNaN", m_isNaN));
    env_def(g, "isFinite", make_native(I, "isFinite", m_isFinite));

    /* Object */
    jengine_value obj_ctor = make_native(I, "Object", m_Object);
    func_props_set(ctx, obj_ctor, "keys", make_native(I, "keys", m_Object_keys));
    func_props_set(ctx, obj_ctor, "values", make_native(I, "values", m_Object_values));
    func_props_set(ctx, obj_ctor, "entries", make_native(I, "entries", m_Object_entries));
    func_props_set(ctx, obj_ctor, "assign", make_native(I, "assign", m_Object_assign));
    func_props_set(ctx, obj_ctor, "freeze", make_native(I, "freeze", m_Object_freeze));
    env_def(g, "Object", obj_ctor);

    /* Array */
    jengine_value arr_ctor = make_native(I, "Array", m_Array);
    func_props_set(ctx, arr_ctor, "isArray", make_native(I, "isArray", m_Array_isArray));
    func_props_set(ctx, arr_ctor, "from", make_native(I, "from", m_Array_from));
    env_def(g, "Array", arr_ctor);

    /* String */
    jengine_value str_ctor = make_native(I, "String", m_String);
    func_props_set(ctx, str_ctor, "fromCharCode", make_native(I, "fromCharCode", m_String_fromCharCode2));
    env_def(g, "String", str_ctor);

    /* Number */
    jengine_value num_ctor = make_native(I, "Number", m_Number);
    func_props_set(ctx, num_ctor, "isInteger", make_native(I, "isInteger", m_Number_isInteger));
    func_props_set(ctx, num_ctor, "isNaN", make_native(I, "isNaN", m_Number_isNaN));
    func_props_set(ctx, num_ctor, "isFinite", make_native(I, "isFinite", m_Number_isFinite));
    func_props_set(ctx, num_ctor, "parseInt", make_native(I, "parseInt", m_Number_parseInt));
    func_props_set(ctx, num_ctor, "parseFloat", make_native(I, "parseFloat", m_Number_parseFloat));
    env_def(g, "Number", num_ctor);

    /* Error types */
    env_def(g, "Error", make_native(I, "Error", m_Error));
    env_def(g, "TypeError", make_native(I, "TypeError", m_TypeError));
    env_def(g, "RangeError", make_native(I, "RangeError", m_RangeError));
    env_def(g, "SyntaxError", make_native(I, "SyntaxError", m_SyntaxError));
    env_def(g, "ReferenceError", make_native(I, "ReferenceError", m_ReferenceError));

    /* Boolean */
    env_def(g, "Boolean", make_native(I, "Boolean", m_Boolean));

    /* method tables */
    jengine_value arr_m = jengine_make_object(ctx);
    def_method(I, arr_m, "push", m_arr_push);
    def_method(I, arr_m, "pop", m_arr_pop);
    def_method(I, arr_m, "shift", m_arr_shift);
    def_method(I, arr_m, "unshift", m_arr_unshift);
    def_method(I, arr_m, "slice", m_arr_slice);
    def_method(I, arr_m, "splice", m_arr_splice);
    def_method(I, arr_m, "concat", m_arr_concat);
    def_method(I, arr_m, "join", m_arr_join);
    def_method(I, arr_m, "indexOf", m_arr_index_of_fn);
    def_method(I, arr_m, "includes", m_arr_includes);
    def_method(I, arr_m, "reverse", m_arr_reverse);
    def_method(I, arr_m, "forEach", m_arr_forEach);
    def_method(I, arr_m, "map", m_arr_map);
    def_method(I, arr_m, "filter", m_arr_filter);
    def_method(I, arr_m, "some", m_arr_some);
    def_method(I, arr_m, "every", m_arr_every);
    def_method(I, arr_m, "find", m_arr_find);
    def_method(I, arr_m, "findIndex", m_arr_findIndex);
    def_method(I, arr_m, "reduce", m_arr_reduce);
    def_method(I, arr_m, "sort", m_arr_sort);
    def_method(I, arr_m, "flat", m_arr_flat);
    def_method(I, arr_m, "flatMap", m_arr_flatMap);
    def_method(I, arr_m, "fill", m_arr_fill);
    def_method(I, arr_m, "at", m_arr_at);
    def_method(I, arr_m, "lastIndexOf", m_arr_lastIndexOf);
    def_method(I, arr_m, "toReversed", m_arr_toReversed);
    def_method(I, arr_m, "toString", m_arr_toString);
    ctx->arr_methods = arr_m;

    jengine_value str_m = jengine_make_object(ctx);
    def_method(I, str_m, "charAt", m_str_charAt);
    def_method(I, str_m, "charCodeAt", m_str_charCodeAt);
    def_method(I, str_m, "indexOf", m_str_indexOf);
    def_method(I, str_m, "lastIndexOf", m_str_lastIndexOf);
    def_method(I, str_m, "includes", m_str_includes);
    def_method(I, str_m, "startsWith", m_str_startsWith);
    def_method(I, str_m, "endsWith", m_str_endsWith);
    def_method(I, str_m, "slice", m_str_slice);
    def_method(I, str_m, "substring", m_str_substring);
    def_method(I, str_m, "substr", m_str_substr);
    def_method(I, str_m, "toUpperCase", m_str_toUpperCase);
    def_method(I, str_m, "toLowerCase", m_str_toLowerCase);
    def_method(I, str_m, "trim", m_str_trim);
    def_method(I, str_m, "trimStart", m_str_trimStart);
    def_method(I, str_m, "trimEnd", m_str_trimEnd);
    def_method(I, str_m, "split", m_str_split);
    def_method(I, str_m, "replace", m_str_replace);
    def_method(I, str_m, "replaceAll", m_str_replaceAll);
    def_method(I, str_m, "repeat", m_str_repeat);
    def_method(I, str_m, "at", m_str_at);
    def_method(I, str_m, "codePointAt", m_str_codePointAt);
    def_method(I, str_m, "padStart", m_str_padStart);
    def_method(I, str_m, "padEnd", m_str_padEnd);
    def_method(I, str_m, "concat", m_str_concat);
    def_method(I, str_m, "toString", m_str_toString);
    def_method(I, str_m, "valueOf", m_str_toString);
    ctx->str_methods = str_m;

    jengine_value num_m = jengine_make_object(ctx);
    def_method(I, num_m, "toString", m_num_toString);
    def_method(I, num_m, "toFixed", m_num_toFixed);
    def_method(I, num_m, "valueOf", m_num_valueOf);
    ctx->num_methods = num_m;

    /* Function method table */
    jengine_value fm = jengine_make_object(ctx);
    def_method(I, fm, "call", m_fn_call);
    def_method(I, fm, "apply", m_fn_apply);
    def_method(I, fm, "bind", m_fn_bind);
    def_method(I, fm, "toString", m_fn_toString);
    ctx->func_methods = fm;

    /* Object method table */
    jengine_value om = jengine_make_object(ctx);
    def_method(I, om, "toString", m_obj_toString);
    def_method(I, om, "valueOf", m_obj_valueOf);
    def_method(I, om, "hasOwnProperty", m_obj_hasOwnProperty);
    ctx->obj_methods = om;
}

void jengine_register_math(jengine_context* ctx) {
    if (!ctx) return;
    /* Math is installed by jengine_create(); this is a no-op for API compatibility. */
}
/* ==============================================================
 * Public API
 * ============================================================== */

jengine_context* jengine_create(void) {
    struct jengine_context* ctx =
        (struct jengine_context*)calloc(1, sizeof(struct jengine_context));
    if (!ctx) return NULL;
    ctx->magic = JENGINE_CTX_MAGIC;
    ctx->heap_size = 64u * 1024u * 1024u;
    ctx->gc_threshold = 8u * 1024u * 1024u;

    ctx->global_env = (jg_env*)heap_alloc(ctx, sizeof(jg_env), 3);
    ctx->global_this = jengine_make_object(ctx);
    if (!ctx->global_env || jengine_is_undefined(ctx->global_this)) {
        jengine_destroy(ctx);
        return NULL;
    }

    Interp I;
    memset(&I, 0, sizeof I);
    I.ctx = ctx;
    I.cur = ctx->global_env;
    I.this_val = ctx->global_this;
    setup_globals(&I);
    return ctx;
}

void jengine_destroy(jengine_context* ctx) {
    if (!ctx) return;
    jg_heap* h = ctx->heap_all;
    while (h) {
        jg_heap* nx = h->next;
        switch (h->kind) {
            case 0: {  /* string */
                jg_string* s = (jg_string*)h;
                free(s->data);
                break;
            }
            case 1: {  /* object */
                jg_object* o = (jg_object*)h;
                for (int i = 0; i < o->count; i++) free(o->keys[i]);
                free(o->keys);
                free(o->vals);
                break;
            }
            case 2: {  /* array */
                jg_array* a = (jg_array*)h;
                free(a->items);
                break;
            }
            case 3: {  /* environment */
                jg_env* e = (jg_env*)h;
                for (int i = 0; i < e->count; i++) free(e->names[i]);
                free(e->names);
                free(e->vals);
                break;
            }
            case 4: {  /* function */
                jg_func* f = (jg_func*)h;
                free(f->name);
                free(f->bound_args);
                break;
            }
            default:
                break;
        }
        free(h);
        h = nx;
    }
    free(ctx->json_scratch);
    arena_free(&ctx->arena);
    free(ctx);
}

int jengine_eval(jengine_context* ctx, const char* source, const char* filename,
                 jengine_value* result) {
    if (!ctx || !source) return -1;
    (void)filename;

    /* REPL-style evaluation: if the whole input is a single expression,
     * evaluate that expression and return its value (so "{a:1}" is an
     * object literal rather than a block statement). */
    {
        Parser ep;
        memset(&ep, 0, sizeof ep);
        ep.ctx = ctx;
        lex_init(&ep.lx, source);
        next(&ep);
        Node* e = parse_expression(&ep);
        if (!ep.err && e && ep.lx.tok.type == TK_EOF) {
            Interp I;
            memset(&I, 0, sizeof I);
            I.ctx = ctx;
            I.cur = ctx->global_env;
            I.this_val = ctx->global_this;
            Interp* saved = ctx->I;
            ctx->I = &I;
            ctx->has_exception = false;
            if (e->type == ND_FUNC && e->as.func.name) {
                /* a bare named function acts like a declaration */
                env_def(ctx->global_env, e->as.func.name, make_user_func(&I, e));
                I.last_value = UNDEF;
            } else {
                I.last_value = ev(&I, e);
            }
            ctx->I = saved;
            if (ctx->has_exception) {
                if (result) *result = ctx->exception;
                return -1;
            }
            if (result) *result = I.last_value;
            return 0;
        }
    }

    Parser p;
    memset(&p, 0, sizeof p);
    p.ctx = ctx;
    lex_init(&p.lx, source);
    next(&p);
    Node* prog = parse_program(&p);

    if (p.err) {
        ctx->has_exception = true;
        ctx->exception = jengine_make_string(ctx, p.errmsg);
        if (result) *result = ctx->exception;
        return -1;
    }

    Interp I;
    memset(&I, 0, sizeof I);
    I.ctx = ctx;
    I.cur = ctx->global_env;
    I.this_val = ctx->global_this;

    Interp* saved = ctx->I;
    ctx->I = &I;
    ctx->has_exception = false;

    exec_block(&I, prog);

    ctx->I = saved;

    jengine_value rv = (I.flow == FLOW_RETURN) ? I.flow_val : I.last_value;

    if (ctx->has_exception) {
        if (result) *result = ctx->exception;
        return -1;
    }
    if (result) *result = rv;
    return 0;
}

int jengine_eval_file(jengine_context* ctx, const char* filepath, jengine_value* result) {
    if (!ctx || !filepath) return -1;
    FILE* f = fopen(filepath, "rb");
    if (!f) {
        ctx->has_exception = true;
        ctx->exception = jengine_make_string(ctx, "cannot open file");
        if (result) *result = ctx->exception;
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) size = 0;
    char* buf = (char*)malloc((size_t)size + 1);
    if (!buf) { fclose(f); return -1; }
    size_t got = fread(buf, 1, (size_t)size, f);
    buf[got] = 0;
    fclose(f);
    int rc = jengine_eval(ctx, buf, filepath, result);
    free(buf);
    return rc;
}

jengine_value jengine_call(jengine_context* ctx, jengine_value fn,
                           jengine_value this_val, jengine_value* args, int argc) {
    if (!ctx || !jengine_is_function(fn)) return UNDEF;
    jg_func* f = as_func(fn);
    if (!ctx->I) {
        if (f->kind == 1) return f->nfn(ctx, this_val, args, argc);
        if (f->kind == 2) return f->pfn(ctx, args, argc);
        return UNDEF;
    }
    return call_func(ctx->I, f, this_val, args, argc);
}

void jengine_register_native(jengine_context* ctx, const char* name, jengine_native_fn fn) {
    if (!ctx || !name || !fn) return;
    jg_func* f = func_new(ctx);
    if (!f) return;
    f->kind = 2;
    f->name = xstrdup(name);
    f->pfn = fn;
    env_def(ctx->global_env, name, make_func_value(f));
}

void jengine_gc(jengine_context* ctx) {
    (void)ctx;  /* all heap nodes are released by jengine_destroy */
}

size_t jengine_memory_usage(jengine_context* ctx) {
    return ctx ? ctx->heap_used : 0;
}

jengine_value jengine_get_exception(jengine_context* ctx) {
    return ctx ? ctx->exception : UNDEF;
}
bool jengine_has_exception(jengine_context* ctx) {
    return ctx ? ctx->has_exception : false;
}
void jengine_clear_exception(jengine_context* ctx) {
    if (!ctx) return;
    ctx->has_exception = false;
    ctx->exception = UNDEF;
}

void jengine_print_value(jengine_context* ctx, jengine_value v) {
    (void)ctx;
    if (jengine_is_number(v)) {
        double d = jengine_to_number(v);
        if (isfinite(d) && d == floor(d) && d >= -1e15 && d <= 1e15) {
            printf("%.1f", d);
        } else {
            char* s = num_to_cstr(d);
            fputs(s ? s : "0", stdout);
            free(s);
        }
    } else {
        char* s = debug_str(v);
        fputs(s ? s : "", stdout);
        free(s);
    }
}

const char* jengine_value_to_json(jengine_context* ctx, jengine_value v) {
    if (!ctx) return "";
    SB sb;
    sb_init(&sb);
    json_write(&sb, v);
    free(ctx->json_scratch);
    ctx->json_scratch = sb_take(&sb);
    return ctx->json_scratch;
}

/* ==============================================================
 * Property / index access (public)
 * ============================================================== */

jengine_value jengine_get_property(jengine_context* ctx, jengine_value obj, const char* prop) {
    if (!ctx || !prop) return UNDEF;
    if (ctx->I) return get_property(ctx->I, obj, prop);
    if (jengine_is_object(obj)) {
        jengine_value out;
        if (obj_get_raw(obj, prop, &out)) return out;
    }
    return UNDEF;
}

void jengine_set_property(jengine_context* ctx, jengine_value obj, const char* prop,
                          jengine_value value) {
    if (!ctx || !prop) return;
    if (ctx->I) { set_property(ctx->I, obj, prop, value); return; }
    if (jengine_is_object(obj)) obj_set_raw(obj, prop, value);
}

jengine_value jengine_get_index(jengine_context* ctx, jengine_value arrv, size_t index) {
    (void)ctx;
    jg_array* a = as_array(arrv);
    return a ? array_get(a, index) : UNDEF;
}

void jengine_set_index(jengine_context* ctx, jengine_value arrv, size_t index,
                       jengine_value value) {
    (void)ctx;
    jg_array* a = as_array(arrv);
    if (a) arr_set_len(a, index, value);
}

void jengine_throw(jengine_context* ctx, jengine_value error) {
    if (!ctx) return;
    ctx->has_exception = true;
    ctx->exception = error;
    if (ctx->I) {
        ctx->I->flow = FLOW_THROW;
        ctx->I->flow_val = error;
    }
}
