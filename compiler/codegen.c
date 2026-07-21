// codegen.c — Slag NASM x86-64 Win64 code generator
//
// Pipeline:
//   Program (AST) -> emit NASM assembly -> assemble with NASM -> link with MinGW-w64
//
// Calling convention: Win64 fastcall
//   - First 4 integer/pointer args: rcx, rdx, r8, r9
//   - First 4 float args: xmm0, xmm1, xmm2, xmm3
//   - Remaining args on stack (right to left)
//   - 32-byte shadow space must be allocated by caller before every call
//   - Stack must be 16-byte aligned at the point of a call
//   - Caller cleans up the stack
//
// Stack frame layout per function:
//   [rbp+16] : first stack argument (if any)
//   [rbp+8]  : return address
//   [rbp]    : saved rbp
//   [rbp-8]  : first local variable
//   [rbp-16] : second local variable
//   ...
//
// Register usage:
//   rax  — integer return value / scratch
//   rcx,rdx,r8,r9 — call arguments
//   xmm0 — float return value / scratch
//   r10,r11 — scratch (caller-saved, used freely)
//   rbp  — frame pointer
//   rsp  — stack pointer

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include "ast.h"
#include "codegen.h"
#include "window_runtime.h"
#include "net_runtime.h"
#include "server_runtime.h"
#include "mem_runtime.h"
#include "file_runtime.h"
#include "audio_runtime.h"
#include "crypto_runtime.h"
#include "matrix_runtime.h"
#include "simd_runtime.h"
#include "mesh_runtime.h"
#include "texture_runtime.h"

// ---------------------------------------------------------------------
// Codegen state
// ---------------------------------------------------------------------

// A local variable entry in the current function's symbol table.
typedef struct Local {
    char *name;
    SlagType type;
    int is_array;
    SlagType elem_type;   // valid if is_array
    int size;             // element count, valid if is_array
    int offset;           // negative offset from rbp, e.g. -8, -16, ...
} Local;

// A global variable entry.
typedef struct Global {
    char *name;
    SlagType type;
    int label_id;         // global is at _glob<label_id>
    int is_array;         // 1 if this is an array
    SlagType elem_type;   // element type if is_array
    int size;             // element count if is_array
    Expr *init;           // initializer expression (scalars)
    ExprList arr_init;    // initializer list (arrays)
} Global;

#define MAX_GLOBALS 2048

#define MAX_LOCALS 2048

typedef struct Codegen {
    FILE *out;            // output assembly file
    int label_counter;    // unique label suffix generator

    // Current function state
    Local locals[MAX_LOCALS];
    int local_count;
    int frame_size;       // total bytes allocated for locals (multiple of 16)

    // Global variables (file-scope)
    Global globals[MAX_GLOBALS];
    int global_count;

    // Float literals need to be emitted in .data section; we collect them.
    double float_consts[1024];
    int float_const_count;

    // String literals emitted in .data section.
    char *str_consts[1024];
    int str_const_count;

    // thread {} blocks found in the function currently being emitted.
    // Bodies are deferred and emitted as standalone procs right after
    // the enclosing function, mirroring the on-handler pattern.
    const StmtList *thread_bodies[64];
    int             thread_ids[64];
    int             thread_body_count;

    // Slot index into _slag_thread_handles for the *next* thread {}
    // encountered. Incremented by STMT_THREAD, reset to 0 by STMT_SYNC.
    int thread_slot;
} Codegen;

#define MAX_THREADS 64

// ---------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------

static void emit(Codegen *cg, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(cg->out, fmt, ap);
    va_end(ap);
    fprintf(cg->out, "\n");
}

static void emit_raw(Codegen *cg, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(cg->out, fmt, ap);
    va_end(ap);
}

static int new_label(Codegen *cg) {
    return cg->label_counter++;
}

// Resolves the calling thread's window struct into rbx (push rbx first,
// caller must pop rbx when done). Per-window input.* state lives in that
// struct's WSTATE_INPUT_* fields, so this must run before touching any of them.
static void emit_input_resolve_window(Codegen *cg) {
    int ok = new_label(cg);
    emit(cg, "    push rbx");
    emit(cg, "    sub  rsp, 40");
    emit(cg, "    mov  rcx, [_window_tls_index]");
    emit(cg, "    call TlsGetValue");
    emit(cg, "    mov  rbx, rax");
    emit(cg, "    test rbx, rbx");
    emit(cg, "    jnz  .L%d", ok);
    emit(cg, "    mov  rbx, [_window_primary_state]");
    emit(cg, ".L%d:", ok);
    emit(cg, "    add  rsp, 40");
}

// Register a float constant in the .data pool; return its index.
static int add_float_const(Codegen *cg, double val) {
    for (int i = 0; i < cg->float_const_count; i++) {
        // Use bit-pattern comparison so -0.0 and 0.0 are NOT treated as
        // the same constant: C's == considers -0.0 == 0.0 true, but they
        // have different IEEE-754 bit patterns (sign bit), which matters
        // for the xorpd-based float negation idiom.
        uint64_t a, b;
        memcpy(&a, &cg->float_consts[i], sizeof(a));
        memcpy(&b, &val, sizeof(b));
        if (a == b) return i;
    }
    cg->float_consts[cg->float_const_count] = val;
    return cg->float_const_count++;
}

// Register a string literal in the .data pool; return its index.
static int add_str_const(Codegen *cg, const char *s) {
    for (int i = 0; i < cg->str_const_count; i++) {
        if (strcmp(cg->str_consts[i], s) == 0) return i;
    }
    char *copy = malloc(strlen(s) + 1);
    strcpy(copy, s);
    cg->str_consts[cg->str_const_count] = copy;
    return cg->str_const_count++;
}

// Look up a local variable by name. Returns NULL if not found.
static Local *find_local(Codegen *cg, const char *name) {
    // Strip leading $ if present (dollar idents inside $(( )))
    if (name[0] == '$') name++;
    for (int i = cg->local_count - 1; i >= 0; i--) {
        if (strcmp(cg->locals[i].name, name) == 0) {
            return &cg->locals[i];
        }
    }
    return NULL;
}

// Look up a global variable by name. Returns NULL if not found.
static Global *find_global(Codegen *cg, const char *name) {
    // Strip leading $ if present
    if (name[0] == '$') name++;
    for (int i = 0; i < cg->global_count; i++) {
        if (strcmp(cg->globals[i].name, name) == 0) {
            return &cg->globals[i];
        }
    }
    return NULL;
}

// Allocate a new local. Returns its stack offset (negative from rbp).
//
// For non-array TYPE_STR locals, two 8-byte slots are reserved: the
// pointer at `offset` and the length (int64) at `offset - 8`. This lets
// string variables carry their length alongside the pointer, which
// print/println and future string operations rely on.
static Local *alloc_local(Codegen *cg, const char *name, SlagType type,
                           int is_array, SlagType elem_type, int size) {
    if (cg->local_count >= MAX_LOCALS) {
        fprintf(stderr, "codegen error: too many locals\n");
        exit(1);
    }
    // Each local takes 8 bytes (int/float/bool/ptr all stored as qword).
    // Arrays take 8 * size bytes. Non-array strings take 16 bytes
    // (ptr + len).
    int bytes;
    if (is_array) {
        bytes = 8 * size;
    } else if (type == TYPE_STR) {
        bytes = 16;
    } else {
        bytes = 8;
    }
    cg->frame_size += bytes;

    Local *loc = &cg->locals[cg->local_count++];
    loc->name      = malloc(strlen(name) + 1);
    strcpy(loc->name, name);
    loc->type      = type;
    loc->is_array  = is_array;
    loc->elem_type = elem_type;
    loc->size      = size;
    loc->offset    = -(cg->frame_size); // grows downward; offset = ptr slot
    return loc;
}

// Allocate a new global variable. Returns the global entry.
static Global *alloc_global(Codegen *cg, const char *name, SlagType type, Expr *init) {
    if (cg->global_count >= MAX_GLOBALS) {
        fprintf(stderr, "codegen error: too many globals\n");
        exit(1);
    }
    Global *g = &cg->globals[cg->global_count];
    g->name = malloc(strlen(name) + 1);
    strcpy(g->name, name);
    g->type = type;
    g->label_id = cg->global_count;
    g->is_array = 0;
    g->elem_type = TYPE_UNKNOWN;
    g->size = 0;
    g->init = init;
    exprlist_init(&g->arr_init);
    cg->global_count++;
    return g;
}

// Allocate a new global array. Returns the global entry.
static Global *alloc_global_array(Codegen *cg, const char *name, SlagType elem_type,
                                   int size, const ExprList *init_list) {
    if (cg->global_count >= MAX_GLOBALS) {
        fprintf(stderr, "codegen error: too many globals\n");
        exit(1);
    }
    Global *g = &cg->globals[cg->global_count];
    g->name = malloc(strlen(name) + 1);
    strcpy(g->name, name);
    g->type = elem_type;
    g->label_id = cg->global_count;
    g->is_array = 1;
    g->elem_type = elem_type;
    g->size = size;
    g->init = NULL;
    if (init_list) {
        g->arr_init = *init_list;  // copy the list
    } else {
        exprlist_init(&g->arr_init);
    }
    cg->global_count++;
    return g;
}

// For a TYPE_STR local, returns the stack offset of its length slot
// (8 bytes above the pointer slot).
static int str_len_offset(const Local *loc) {
    return loc->offset + 8;
}

// Non-static wrappers for use by window_runtime.c
void cg_emit(Codegen *cg, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(cg->out, fmt, ap);
    va_end(ap);
    fprintf(cg->out, "\n");
}

void cg_emit_raw(Codegen *cg, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(cg->out, fmt, ap);
    va_end(ap);
}

int cg_new_label(Codegen *cg) { return cg->label_counter++; }
int cg_add_str_const(Codegen *cg, const char *s) { return add_str_const(cg, s); }
int cg_add_float_const(Codegen *cg, double val) { return add_float_const(cg, val); }


static void emit_call_expr(Codegen *cg, const Expr *e);
// Load a string literal's ptr into rax and len into rdx.
static void emit_load_str_lit(Codegen *cg, const Expr *e) {
    int idx = add_str_const(cg, e->as.str.value);
    size_t slen = strlen(e->as.str.value);
    emit(cg, "    lea  rax, [_str%d]", idx);
    emit(cg, "    mov  rdx, %zu", slen);
}

// Load a string-typed local's ptr into rax and len into rdx.
static void emit_load_str_local(Codegen *cg, const Local *loc) {
    emit(cg, "    mov  rax, [rbp%+d]", loc->offset);
    emit(cg, "    mov  rdx, [rbp%+d]", str_len_offset(loc));
}

// Store ptr (in rax) and len (in rdx) into a string-typed local's slots.
static void emit_store_str_local(Codegen *cg, const Local *loc) {
    emit(cg, "    mov  [rbp%+d], rax", loc->offset);
    emit(cg, "    mov  [rbp%+d], rdx", str_len_offset(loc));
}

// Emit code that evaluates a str-typed expression, leaving ptr in rax
// and len in rdx. Handles: string literals, str-typed local variables,
// and calls (readfile/readline/match — assumed to return ptr in rax,
// len in rdx per the codegen calling convention).
static void emit_str_expr(Codegen *cg, const Expr *e) {
    switch (e->kind) {
        case EXPR_STR_LIT:
            emit_load_str_lit(cg, e);
            break;

        case EXPR_IDENT: {
            Local *loc = find_local(cg, e->as.str.value);
            if (loc) {
                emit_load_str_local(cg, loc);
            } else {
                Global *g = find_global(cg, e->as.str.value);
                if (g && g->type == TYPE_STR) {
                    emit(cg, "    mov  rax, [_glob%d]", g->label_id);
                    emit(cg, "    mov  rdx, [_glob%d_len]", g->label_id);
                } else {
                    fprintf(stderr, "codegen error: undefined string variable '%s'\n",
                            e->as.str.value);
                    emit(cg, "    xor  rax, rax");
                    emit(cg, "    xor  rdx, rdx");
                }
            }
            break;
        }

        case EXPR_CALL:
        case EXPR_MEMBER_CALL:
            // readfile/readline/match etc. return ptr in rax, len in rdx.
            emit_call_expr(cg, e);
            break;

        default:
            fprintf(stderr, "codegen error: unsupported string expression kind %d\n", e->kind);
            emit(cg, "    xor  rax, rax");
            emit(cg, "    xor  rdx, rdx");
            break;
    }
}

// ---------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------

static void emit_expr(Codegen *cg, const Expr *e, SlagType hint);
static void emit_stmt(Codegen *cg, const Stmt *s);
static void emit_stmtlist(Codegen *cg, const StmtList *list);

// ---------------------------------------------------------------------
// Expression emission
//
// Integer results land in rax.
// Float results land in xmm0.
// String results: rax = pointer, rdx = length.
// Bool results land in rax (0 or 1).
//
// `hint` is the expected result type from context; used to decide
// whether to treat an expression as int or float when ambiguous.
// ---------------------------------------------------------------------

// Emit a string constant's bytes as NASM db directives.
// Handles \n \t \\ and other escapes.
static void __attribute__((unused)) emit_str_bytes(Codegen *cg, const char *s) {
    // We emit as a mix of quoted segments and explicit byte values.
    int in_quote = 0;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\n' || c == '\t' || c == '\\' || c == '"' || c < 32) {
            if (in_quote) { emit_raw(cg, "\""); in_quote = 0; }
            emit_raw(cg, ", %d", (int)c);
        } else {
            if (!in_quote) { emit_raw(cg, ", \""); in_quote = 1; }
            emit_raw(cg, "%c", c);
        }
    }
    if (in_quote) emit_raw(cg, "\"");
}

// Emit code to push rax onto the stack (for saving across calls).
#define PUSH_RAX(cg)  emit(cg, "    push rax")
#define POP_RAX(cg)   emit(cg, "    pop  rax")
#define PUSH_XMM0(cg) emit(cg, "    sub  rsp, 8"); emit(cg, "    movsd [rsp], xmm0")
#define POP_XMM0(cg)  emit(cg, "    movsd xmm0, [rsp]"); emit(cg, "    add  rsp, 8")

// Emit an integer expression; result in rax.
static void emit_int_expr(Codegen *cg, const Expr *e);
static void emit_call_expr(Codegen *cg, const Expr *e);
// Emit a float expression; result in xmm0.
static void emit_float_expr(Codegen *cg, const Expr *e);

// Determine the "natural" type of an expression given context.
static SlagType expr_type(Codegen *cg, const Expr *e, SlagType hint) {
    switch (e->kind) {
        case EXPR_INT_LIT:   return TYPE_INT;
        case EXPR_FLOAT_LIT: return TYPE_FLOAT;
        case EXPR_BOOL_LIT:  return TYPE_BOOL;
        case EXPR_STR_LIT:   return TYPE_STR;
        case EXPR_REGEX_LIT: return TYPE_STR;
        case EXPR_IDENT: {
            Local *loc = find_local(cg, e->as.str.value);
            if (loc) return loc->type;
            Global *g = find_global(cg, e->as.str.value);
            if (g) return g->type;
            return hint;
        }
        case EXPR_BINARY: {
            // If either operand is float, result is float.
            SlagType lt = expr_type(cg, e->as.binary.left, hint);
            SlagType rt = expr_type(cg, e->as.binary.right, hint);
            if (lt == TYPE_FLOAT || rt == TYPE_FLOAT) return TYPE_FLOAT;
            return TYPE_INT;
        }
        case EXPR_UNARY:
            return expr_type(cg, e->as.unary.operand, hint);
        case EXPR_LOGICAL:
            return TYPE_BOOL;
        case EXPR_MEMBER: {
            if (strcmp(e->as.member.member, "len") == 0) return TYPE_INT;
            // cpu.* topology fields and window.open (the bare-member
            // read of the open/closed flag) are all int-typed globals.
            if (e->as.member.base->kind == EXPR_IDENT) {
                const char *base_name = e->as.member.base->as.str.value;
                if (strcmp(base_name, "cpu") == 0) return TYPE_INT;
                if (strcmp(base_name, "window") == 0) return TYPE_INT;
            }
            return hint;
        }
        case EXPR_MEMBER_CALL: {
            // All member-call builtins that return a value return int,
            // except window.native() and file.list_name(), which return
            // a str (ptr+len):
            // mem.alloc/peek8/peek64, net.recv/ack/connected, input.*,
            // window.is_open, time.now_ms, etc. (void ones are unused here).
            const char *mcmember = e->as.member_call.member;
            const char *mcbase = (e->as.member_call.base &&
                                  e->as.member_call.base->kind == EXPR_IDENT)
                                     ? e->as.member_call.base->as.str.value : "";
            if (strcmp(mcmember, "native") == 0 && strcmp(mcbase, "window") == 0) {
                return TYPE_STR;
            }
            if (strcmp(mcmember, "list_name") == 0 && strcmp(mcbase, "file") == 0) {
                return TYPE_STR;
            }
            return TYPE_INT;
        }
        case EXPR_INDEX: {
            if (e->as.index.base->kind == EXPR_IDENT) {
                Local *loc = find_local(cg, e->as.index.base->as.str.value);
                if (loc && loc->is_array) return loc->elem_type;
                Global *glb = find_global(cg, e->as.index.base->as.str.value);
                if (glb && glb->is_array) return glb->elem_type;
            }
            return hint;
        }
        case EXPR_CALL: {
            // Built-in float-returning calls.
            if (strcmp(e->as.call.name, "sqrt") == 0) return TYPE_FLOAT;
            if (strcmp(e->as.call.name, "sin") == 0)  return TYPE_FLOAT;
            if (strcmp(e->as.call.name, "cos") == 0)  return TYPE_FLOAT;
            // Built-in str-returning calls.
            if (strcmp(e->as.call.name, "readfile") == 0) return TYPE_STR;
            if (strcmp(e->as.call.name, "readline") == 0) return TYPE_STR;
            // Other calls: fall through to the hint-based default below.
            return hint != TYPE_UNKNOWN ? hint : TYPE_INT;
        }
        default:
            return hint != TYPE_UNKNOWN ? hint : TYPE_INT;
    }
}

// Emit a path/pointer argument for a file.* builtin, leaving the byte
// pointer in rax. A str expression yields ptr(rax)+len(rdx) as usual, but
// an int expression (e.g. a mem.alloc buffer the caller filled with
// mem.poke8 and null-terminated) is taken as the null-terminated path
// pointer directly. This lets programs build filenames/paths at runtime
// instead of being limited to string literals/str variables.
static void emit_path_expr(Codegen *cg, const Expr *e) {
    if (expr_type(cg, e, TYPE_STR) == TYPE_STR) {
        emit_str_expr(cg, e);   // rax = ptr, rdx = len
    } else {
        emit_int_expr(cg, e);   // rax = ptr (runtime-built byte buffer)
    }
}

// Maps a string literal used in a key comparison (e.g. `key == "a"` or
// `key == "left"`) to its canonical key code, matching the value the
// window runtime delivers to on key_down/on key_up after translating the
// raw virtual key. Printable keys keep their ASCII value (a single-char
// literal is just its byte, case-sensitive); non-character keys (arrows,
// F-keys, modifiers, etc.) are encoded as VK+256 so they never collide
// with a real character. Returns 1 and sets *out on a match, else 0.
static int keylit_code(const char *s, int *out) {
    if (s == NULL) return 0;
    if (s[0] != '\0' && s[1] == '\0') {      // single character -> its byte
        *out = (unsigned char)s[0];
        return 1;
    }
    static const struct { const char *name; int code; } tbl[] = {
        {"esc",27},{"escape",27},{"enter",13},{"return",13},
        {"backspace",8},{"bksp",8},{"tab",9},{"space",32},
        {"left",256+37},{"up",256+38},{"right",256+39},{"down",256+40},
        {"home",256+36},{"end",256+35},{"pageup",256+33},{"pagedown",256+34},
        {"insert",256+45},{"delete",256+46},{"del",256+46},
        {"shift",256+16},{"ctrl",256+17},{"control",256+17},{"alt",256+18},
        {"f1",256+112},{"f2",256+113},{"f3",256+114},{"f4",256+115},
        {"f5",256+116},{"f6",256+117},{"f7",256+118},{"f8",256+119},
        {"f9",256+120},{"f10",256+121},{"f11",256+122},{"f12",256+123},
    };
    for (size_t i = 0; i < sizeof(tbl)/sizeof(tbl[0]); i++) {
        if (strcmp(s, tbl[i].name) == 0) { *out = tbl[i].code; return 1; }
    }
    return 0;
}

// ---------------------------------------------------------------------
// Integer expression emission — result in rax
// ---------------------------------------------------------------------
static void emit_int_expr(Codegen *cg, const Expr *e) {
    switch (e->kind) {
        case EXPR_INT_LIT:
            emit(cg, "    mov  rax, %ld", e->as.int_val);
            break;

        case EXPR_BOOL_LIT:
            emit(cg, "    mov  rax, %d", e->as.bool_val);
            break;

        case EXPR_IDENT: {
            Local *loc = find_local(cg, e->as.str.value);
            if (loc) {
                if (loc->type == TYPE_FLOAT) {
                    emit(cg, "    movsd xmm0, [rbp%+d]", loc->offset);
                    emit(cg, "    cvttsd2si rax, xmm0");
                } else {
                    emit(cg, "    mov  rax, [rbp%+d]", loc->offset);
                }
            } else {
                Global *g = find_global(cg, e->as.str.value);
                if (g) {
                    if (g->type == TYPE_FLOAT) {
                        emit(cg, "    movsd xmm0, [_glob%d]", g->label_id);
                        emit(cg, "    cvttsd2si rax, xmm0");
                    } else {
                        emit(cg, "    mov  rax, [_glob%d]", g->label_id);
                    }
                } else {
                    fprintf(stderr, "codegen error: undefined variable '%s'\n", e->as.str.value);
                    emit(cg, "    xor  rax, rax");
                }
            }
            break;
        }

        case EXPR_UNARY:
            emit_int_expr(cg, e->as.unary.operand);
            if (e->as.unary.op == TOK_MINUS) {
                emit(cg, "    neg  rax");
            } else if (e->as.unary.op == TOK_NOT) {
                emit(cg, "    test rax, rax");
                emit(cg, "    setz al");
                emit(cg, "    movzx rax, al");
            }
            break;

        case EXPR_BINARY: {
            SlagType lt = expr_type(cg, e->as.binary.left, TYPE_INT);
            SlagType rt = expr_type(cg, e->as.binary.right, TYPE_INT);
            // Key/char comparison: `intexpr == "a"` / `"left" == intexpr`
            // (and !=). The string literal lowers to its key code and the
            // comparison is done as plain integers. Lets key handlers test
            // characters and named keys instead of raw virtual-key numbers.
            if (e->as.binary.op == TOK_EQ || e->as.binary.op == TOK_NEQ) {
                const Expr *L = e->as.binary.left;
                const Expr *R = e->as.binary.right;
                const Expr *lit = NULL; const Expr *other = NULL;
                if (L->kind == EXPR_STR_LIT && rt != TYPE_STR) { lit = L; other = R; }
                else if (R->kind == EXPR_STR_LIT && lt != TYPE_STR) { lit = R; other = L; }
                if (lit) {
                    int code = 0;
                    if (keylit_code(lit->as.str.value, &code)) {
                        emit_int_expr(cg, other);
                        emit(cg, "    cmp  rax, %d", code);
                        if (e->as.binary.op == TOK_EQ) {
                            emit(cg, "    sete al");
                        } else {
                            emit(cg, "    setne al");
                        }
                        emit(cg, "    movzx rax, al");
                        break;
                    }
                }
            }
            if (lt == TYPE_STR || rt == TYPE_STR) {
                if (e->as.binary.op != TOK_EQ && e->as.binary.op != TOK_NEQ) {
                    fprintf(stderr, "codegen error: operator not supported on str operands (only == and != are)\n");
                    emit(cg, "    xor  rax, rax");
                    break;
                }
                emit(cg, "    ; str comparison");
                emit_str_expr(cg, e->as.binary.left);   // rax=ptr, rdx=len
                emit(cg, "    mov  r12, rax        ; left ptr");
                emit(cg, "    mov  r13, rdx        ; left len");
                emit_str_expr(cg, e->as.binary.right);  // rax=ptr, rdx=len
                emit(cg, "    mov  r8,  rax        ; right ptr");
                emit(cg, "    mov  r9,  rdx        ; right len");
                emit(cg, "    mov  rcx, r12        ; left ptr");
                emit(cg, "    mov  rdx, r13        ; left len");
                emit(cg, "    sub  rsp, 32");
                emit(cg, "    call _slag_str_eq");
                emit(cg, "    add  rsp, 32");
                if (e->as.binary.op == TOK_NEQ) {
                    emit(cg, "    xor  rax, 1");
                }
                break;
            }


            if (lt == TYPE_FLOAT || rt == TYPE_FLOAT) {
                int op = e->as.binary.op;
                // If arithmetic op with float operand, evaluate as float then convert to int
                if (op == TOK_PLUS || op == TOK_MINUS || op == TOK_STAR || op == TOK_SLASH) {
                    emit_float_expr(cg, e);
                    emit(cg, "    cvttsd2si rax, xmm0");
                    break;
                }
                // Comparison between floats — result is int (0/1)
                emit_float_expr(cg, e->as.binary.left);
                PUSH_XMM0(cg);
                emit_float_expr(cg, e->as.binary.right);
                emit(cg, "    movsd xmm1, [rsp]");
                emit(cg, "    add  rsp, 8");
                emit(cg, "    ucomisd xmm1, xmm0");
                switch (e->as.binary.op) {
                    case TOK_LT:  emit(cg, "    setb  al"); break;
                    case TOK_GT:  emit(cg, "    seta  al"); break;
                    case TOK_LE:  emit(cg, "    setbe al"); break;
                    case TOK_GE:  emit(cg, "    setae al"); break;
                    case TOK_EQ:  emit(cg, "    sete  al"); break;
                    case TOK_NEQ: emit(cg, "    setne al"); break;
                    default:      emit(cg, "    xor   al, al"); break;
                }
                emit(cg, "    movzx rax, al");
                break;
            }

            // Integer binary op
            emit_int_expr(cg, e->as.binary.left);
            PUSH_RAX(cg);
            emit_int_expr(cg, e->as.binary.right);
            // rax = right, [rsp] = left
            emit(cg, "    mov  rcx, rax");   // rcx = right
            emit(cg, "    pop  rax");         // rax = left

            switch (e->as.binary.op) {
                case TOK_PLUS:
                    emit(cg, "    add  rax, rcx");
                    break;
                case TOK_MINUS:
                    emit(cg, "    sub  rax, rcx");
                    break;
                case TOK_STAR:
                    emit(cg, "    imul rax, rcx");
                    break;
                case TOK_SLASH:
                    emit(cg, "    cqo");
                    emit(cg, "    idiv rcx");
                    break;
                case TOK_PERCENT:
                    emit(cg, "    cqo");
                    emit(cg, "    idiv rcx");
                    emit(cg, "    mov  rax, rdx");  // remainder in rdx
                    break;
                case TOK_LT:
                    emit(cg, "    cmp  rax, rcx");
                    emit(cg, "    setl  al");
                    emit(cg, "    movzx rax, al");
                    break;
                case TOK_GT:
                    emit(cg, "    cmp  rax, rcx");
                    emit(cg, "    setg  al");
                    emit(cg, "    movzx rax, al");
                    break;
                case TOK_LE:
                    emit(cg, "    cmp  rax, rcx");
                    emit(cg, "    setle al");
                    emit(cg, "    movzx rax, al");
                    break;
                case TOK_GE:
                    emit(cg, "    cmp  rax, rcx");
                    emit(cg, "    setge al");
                    emit(cg, "    movzx rax, al");
                    break;
                case TOK_EQ:
                    emit(cg, "    cmp  rax, rcx");
                    emit(cg, "    sete  al");
                    emit(cg, "    movzx rax, al");
                    break;
                case TOK_NEQ:
                    emit(cg, "    cmp  rax, rcx");
                    emit(cg, "    setne al");
                    emit(cg, "    movzx rax, al");
                    break;
                default:
                    emit(cg, "    ; unhandled binary op");
                    break;
            }
            break;
        }

        case EXPR_LOGICAL: {
            int end_label = new_label(cg);
            if (e->as.logical.op == TOK_AND) {
                int false_label = new_label(cg);
                emit_int_expr(cg, e->as.logical.left);
                emit(cg, "    test rax, rax");
                emit(cg, "    jz   .L%d", false_label);
                emit_int_expr(cg, e->as.logical.right);
                emit(cg, "    test rax, rax");
                emit(cg, "    jz   .L%d", false_label);
                emit(cg, "    mov  rax, 1");
                emit(cg, "    jmp  .L%d", end_label);
                emit(cg, ".L%d:", false_label);
                emit(cg, "    xor  rax, rax");
            } else { // TOK_OR
                int true_label = new_label(cg);
                emit_int_expr(cg, e->as.logical.left);
                emit(cg, "    test rax, rax");
                emit(cg, "    jnz  .L%d", true_label);
                emit_int_expr(cg, e->as.logical.right);
                emit(cg, "    test rax, rax");
                emit(cg, "    jnz  .L%d", true_label);
                emit(cg, "    xor  rax, rax");
                emit(cg, "    jmp  .L%d", end_label);
                emit(cg, ".L%d:", true_label);
                emit(cg, "    mov  rax, 1");
            }
            emit(cg, ".L%d:", end_label);
            break;
        }

        case EXPR_INDEX: {
            // Array element access: base[index]
            // Load base address, compute offset, load qword.
            Local *loc = NULL;
            Global *glb = NULL;
            if (e->as.index.base->kind == EXPR_IDENT) {
                loc = find_local(cg, e->as.index.base->as.str.value);
                if (!loc) {
                    glb = find_global(cg, e->as.index.base->as.str.value);
                }
            }
            if (!loc && !glb) {
                fprintf(stderr, "codegen error: array base not found\n");
                emit(cg, "    xor  rax, rax");
                break;
            }
            // Compute index into rcx
            emit_int_expr(cg, e->as.index.index);
            emit(cg, "    mov  rcx, rax");
            if (loc) {
                // Local array: base address on stack
                emit(cg, "    lea  rax, [rbp%+d]", loc->offset);
                emit(cg, "    mov  rax, [rax + rcx*8]");
            } else {
                // Global array: base address is label
                emit(cg, "    lea  rax, [_globarr%d]", glb->label_id);
                emit(cg, "    mov  rax, [rax + rcx*8]");
            }
            break;
        }

        case EXPR_MEMBER: {
            // e.g. tokens.len, cpu.logical_cores
            // For now handle .len on arrays and cpu.* fields.
            Local *loc = NULL;
            Global *glb = NULL;
            if (e->as.member.base->kind == EXPR_IDENT) {
                loc = find_local(cg, e->as.member.base->as.str.value);
                if (!loc) {
                    glb = find_global(cg, e->as.member.base->as.str.value);
                }
            }
            if (strcmp(e->as.member.member, "len") == 0) {
                if (loc) {
                    emit(cg, "    mov  rax, %d", loc->size);
                } else if (glb && glb->is_array) {
                    emit(cg, "    mov  rax, %d", glb->size);
                } else {
                    emit(cg, "    xor  rax, rax");
                }
            } else {
                // cpu.* fields — these are global read-only values
                // populated at startup (stub: return 0 for now)
                emit(cg, "    mov  rax, [_%s_%s]",
                     e->as.member.base->as.str.value,
                     e->as.member.member);
            }
            break;
        }

        case EXPR_CALL: {
            // Float-returning intrinsics used in an int context: compute the
            // float result then truncate to int.
            if ((strcmp(e->as.call.name, "sqrt") == 0 ||
                 strcmp(e->as.call.name, "sin") == 0 ||
                 strcmp(e->as.call.name, "cos") == 0) &&
                e->as.call.args.count >= 1) {
                emit_float_expr(cg, e);
                emit(cg, "    cvttsd2si rax, xmm0   ; (int)float-intrinsic");
                break;
            }
            // Built-in and user function calls that return int.
            emit_call_expr(cg, e);
            break;
        }

        case EXPR_MEMBER_CALL: {
            // e.g. window.is_open() -> int in rax.
            emit_call_expr(cg, e);
            break;
        }

        default:
            emit(cg, "    xor  rax, rax  ; unhandled expr kind %d", e->kind);
            break;
    }
}

// ---------------------------------------------------------------------
// Float expression emission — result in xmm0
// ---------------------------------------------------------------------
static void emit_float_expr(Codegen *cg, const Expr *e) {
    switch (e->kind) {
        case EXPR_FLOAT_LIT: {
            int idx = add_float_const(cg, e->as.float_val);
            emit(cg, "    movsd xmm0, [_flt%d]", idx);
            break;
        }

        case EXPR_INT_LIT:
            // Promote int literal to float.
            emit(cg, "    mov  rax, %ld", e->as.int_val);
            emit(cg, "    cvtsi2sd xmm0, rax");
            break;

        case EXPR_IDENT: {
            Local *loc = find_local(cg, e->as.str.value);
            if (loc) {
                if (loc->type == TYPE_INT) {
                    emit(cg, "    mov  rax, [rbp%+d]", loc->offset);
                    emit(cg, "    cvtsi2sd xmm0, rax");
                } else {
                    emit(cg, "    movsd xmm0, [rbp%+d]", loc->offset);
                }
            } else {
                Global *g = find_global(cg, e->as.str.value);
                if (g) {
                    if (g->type == TYPE_INT) {
                        emit(cg, "    mov  rax, [_glob%d]", g->label_id);
                        emit(cg, "    cvtsi2sd xmm0, rax");
                    } else {
                        emit(cg, "    movsd xmm0, [_glob%d]", g->label_id);
                    }
                } else {
                    fprintf(stderr, "codegen error: undefined variable '%s'\n", e->as.str.value);
                    emit(cg, "    xorpd xmm0, xmm0");
                }
            }
            break;
        }

        case EXPR_UNARY:
            emit_float_expr(cg, e->as.unary.operand);
            if (e->as.unary.op == TOK_MINUS) {
                // Negate: XOR sign bit using a mask.
                int idx = add_float_const(cg, -0.0);
                emit(cg, "    movsd xmm1, [_flt%d]", idx);
                emit(cg, "    xorpd xmm0, xmm1");
            }
            break;

        case EXPR_BINARY: {
            emit_float_expr(cg, e->as.binary.left);
            PUSH_XMM0(cg);
            emit_float_expr(cg, e->as.binary.right);
            // xmm0 = right; load left into xmm1
            emit(cg, "    movsd xmm1, [rsp]");
            emit(cg, "    add  rsp, 8");
            // Perform: xmm1 op xmm0 → xmm0
            switch (e->as.binary.op) {
                case TOK_PLUS:
                    emit(cg, "    addsd xmm0, xmm1"); // xmm0 = right+left
                    // addsd is commutative so order doesn't matter
                    break;
                case TOK_MINUS:
                    emit(cg, "    subsd xmm1, xmm0");
                    emit(cg, "    movsd xmm0, xmm1");
                    break;
                case TOK_STAR:
                    emit(cg, "    mulsd xmm0, xmm1");
                    break;
                case TOK_SLASH:
                    emit(cg, "    divsd xmm1, xmm0");
                    emit(cg, "    movsd xmm0, xmm1");
                    break;
                default:
                    emit(cg, "    ; unhandled float binary op");
                    break;
            }
            break;
        }

        case EXPR_INDEX: {
            Local *loc = NULL;
            Global *glb = NULL;
            if (e->as.index.base->kind == EXPR_IDENT) {
                loc = find_local(cg, e->as.index.base->as.str.value);
                if (!loc) {
                    glb = find_global(cg, e->as.index.base->as.str.value);
                }
            }
            if (!loc && !glb) {
                emit(cg, "    xorpd xmm0, xmm0");
                break;
            }
            emit_int_expr(cg, e->as.index.index);
            emit(cg, "    mov  rcx, rax");
            if (loc) {
                emit(cg, "    lea  rax, [rbp%+d]", loc->offset);
            } else {
                emit(cg, "    lea  rax, [_globarr%d]", glb->label_id);
            }
            emit(cg, "    movsd xmm0, [rax + rcx*8]");
            break;
        }

        case EXPR_CALL: {
            // Built-in sqrt(x): evaluate the argument as a float into xmm0,
            // then take the hardware square root.
            if (strcmp(e->as.call.name, "sqrt") == 0 &&
                e->as.call.args.count >= 1) {
                emit_float_expr(cg, e->as.call.args.items[0]);
                emit(cg, "    sqrtsd xmm0, xmm0   ; sqrt()");
                break;
            }
            // Built-in sin(x) / cos(x): evaluate arg into xmm0, then use the
            // x87 FPU (fsin/fcos). The value is bounced through an 8-byte
            // stack slot to move it between the SSE and x87 register files.
            if ((strcmp(e->as.call.name, "sin") == 0 ||
                 strcmp(e->as.call.name, "cos") == 0) &&
                e->as.call.args.count >= 1) {
                emit_float_expr(cg, e->as.call.args.items[0]);
                emit(cg, "    sub  rsp, 8");
                emit(cg, "    movsd [rsp], xmm0     ; bounce arg to stack");
                emit(cg, "    fld  qword [rsp]      ; load onto x87 stack");
                if (strcmp(e->as.call.name, "sin") == 0) {
                    emit(cg, "    fsin                  ; sin()");
                } else {
                    emit(cg, "    fcos                  ; cos()");
                }
                emit(cg, "    fstp qword [rsp]      ; store result back");
                emit(cg, "    movsd xmm0, [rsp]");
                emit(cg, "    add  rsp, 8");
                break;
            }
            emit_call_expr(cg, e);
            // Result already in xmm0 for float-returning functions.
            break;
        }

        default:
            break;
    }
}

// ---------------------------------------------------------------------
// General expression dispatcher
// ---------------------------------------------------------------------
static void emit_expr(Codegen *cg, const Expr *e, SlagType hint) {
    SlagType t = expr_type(cg, e, hint);
    if (t == TYPE_FLOAT) {
        emit_float_expr(cg, e);
    } else {
        emit_int_expr(cg, e);
    }
}

// ---------------------------------------------------------------------
// Built-in call emission
//
// Handles: print, println, readline, readfile, match, pixel,
//          window.open, window.close, window.flush, zbuffer.clear,
//          project, draw_triangle, bind_texture, and user functions.
//
// Win64 calling convention:
//   - Align rsp to 16 bytes before call
//   - Allocate 32-byte shadow space
//   - First 4 integer args: rcx, rdx, r8, r9
//   - First 4 float args: xmm0..xmm3
// ---------------------------------------------------------------------

// Emit a Win64-compliant call prologue: save alignment, allocate shadow space.
// Returns the number of extra bytes pushed for alignment (0 or 8).
// Caller must emit_call_epilogue with the same value afterward.
static int emit_call_prologue(Codegen *cg) {
    // Shadow space: 32 bytes. We also need rsp 16-byte aligned at the
    // call site. Since our frame is set up with a fixed size (multiple
    // of 16) we may need an extra 8-byte pad depending on whether the
    // current rsp is already aligned.
    // For simplicity we always push an 8-byte sentinel and check.
    // In practice the function prologue aligns the frame; we trust that.
    emit(cg, "    sub  rsp, 32          ; shadow space");
    return 0;
}

static void emit_call_epilogue(Codegen *cg, int extra) {
    emit(cg, "    add  rsp, 32          ; remove shadow space");
    (void)extra;
}

// Emit code to call WriteConsoleA(handle, buf, len, &written, NULL)
// which writes `len` bytes at `buf` to stdout.
// On entry: rcx = handle (already loaded), rdx = buf ptr, r8 = length.
static void emit_write_console(Codegen *cg) {
    // WriteConsoleA has 5 args: rcx, rdx, r8, r9, [rsp+32].
    // We need: 32 bytes shadow + 8 bytes for arg5 + 8 bytes alignment pad = 48.
    // 48 is a multiple of 16 so rsp stays aligned at the call site.
    emit(cg, "    sub  rsp, 48          ; shadow(32) + arg5(8) + pad(8)");
    emit(cg, "    lea  r9,  [rsp+32]    ; &written");
    emit(cg, "    mov  qword [rsp+32], 0");
    emit(cg, "    mov  qword [rsp+40], 0 ; lpOverlapped = NULL (5th arg on stack)");
    emit(cg, "    call WriteConsoleA");
    emit(cg, "    add  rsp, 48");
}

// Emit a print or println call.
// `is_println` appends a newline.
// The argument is the first (and only) entry in args.
static void emit_print(Codegen *cg, const ExprList *args, int is_println) {
    if (args->count < 1) return;
    const Expr *arg = args->items[0];
    SlagType t = expr_type(cg, arg, TYPE_STR);

if (t == TYPE_STR) {
        // Evaluate the string expression: rax = ptr, rdx = len.
        emit(cg, "    ; print string");
        emit_str_expr(cg, arg);
        emit(cg, "    mov  r8,  rdx        ; length");
        emit(cg, "    mov  rdx, rax        ; buffer ptr");
        emit(cg, "    mov  rcx, [_stdout]");
        emit_write_console(cg);
    } else if (t == TYPE_INT) {
        // Convert int to decimal string and print.
        // We use a small stack buffer and a simple itoa loop.
        emit(cg, "    ; print int");
        emit_int_expr(cg, arg);
        emit(cg, "    mov  rcx, rax          ; value to convert");
        emit(cg, "    lea  rdx, [rsp-24]     ; temp buffer (24 bytes)");
        emit(cg, "    sub  rsp, 24");
        emit(cg, "    call _slag_itoa        ; rcx=val, rdx=buf -> rax=len");
        emit(cg, "    mov  r8,  rax          ; length");
        emit(cg, "    lea  rdx, [rsp]        ; buffer");
        emit(cg, "    mov  rcx, [_stdout]");
        emit_write_console(cg);
        emit(cg, "    add  rsp, 24");
    } else if (t == TYPE_FLOAT) {
        // Convert float to string and print.
        emit(cg, "    ; print float");
        emit_float_expr(cg, arg);
        emit(cg, "    lea  rcx, [rsp-32]     ; temp buffer");
        emit(cg, "    sub  rsp, 32");
        emit(cg, "    call _slag_ftoa        ; xmm0=val, rcx=buf -> rax=len");
        emit(cg, "    mov  r8,  rax");
        emit(cg, "    lea  rdx, [rsp]");
        emit(cg, "    mov  rcx, [_stdout]");
        emit_write_console(cg);
        emit(cg, "    add  rsp, 32");
    }

    if (is_println) {
        // Print a newline character.
        emit(cg, "    ; println newline");
        emit(cg, "    mov  rcx, [_stdout]");
        emit(cg, "    lea  rdx, [_newline]");
        emit(cg, "    mov  r8,  1");
        emit_write_console(cg);
    }
}

// Emit a user-defined function call. Result in rax (int) or xmm0 (float).
static void emit_user_call(Codegen *cg, const char *name, const ExprList *args) {
    // Win64 calling convention:
    //   args 0-3 -> rcx, rdx, r8, r9
    //   args 4+  -> [rsp+32], [rsp+40], ... (after shadow space)
    //
    // Strategy:
    //   1. Evaluate args 0-3 into scratch regs (save to stack temporarily)
    //   2. Allocate shadow space (32 bytes)
    //   3. Evaluate and store args 4+ onto stack at [rsp+32], [rsp+40]...
    //   4. Load args 0-3 into rcx/rdx/r8/r9
    //   5. Call
    //   6. Clean up shadow + stack args

    int n = args->count;
    const char *int_regs[] = { "rcx", "rdx", "r8", "r9" };

    // Evaluate first 4 args and push to temp stack storage.
    // Each PUSH_RAX/PUSH_XMM0 is 8 bytes; an odd number of register args
    // therefore leaves rsp misaligned by 8. Since `alloc` below is always a
    // multiple of 16, rsp would be misaligned at the `call` whenever reg_args
    // is odd (1 or 3), which corrupts the 16-byte-alignment the Win64 ABI
    // requires — Win32 APIs called (transitively) by the callee then fault
    // deep in ntdll (SwitchBack/SbSelectProcedure). Emit an 8-byte pad first
    // so the total displacement (pad + reg_args*8 + alloc) stays a multiple
    // of 16. The pad sits above the pushed args, so shadow/stack-arg offsets
    // (measured from the post-`sub` rsp) are unaffected.
    int reg_args = n < 4 ? n : 4;
    int align_pad = (reg_args % 2 == 1) ? 8 : 0;
    if (align_pad) {
        emit(cg, "    sub  rsp, 8          ; align pad (odd reg-arg count)");
    }
    for (int i = 0; i < reg_args; i++) {
        SlagType t = expr_type(cg, args->items[i], TYPE_INT);
        if (t == TYPE_FLOAT) {
            emit_float_expr(cg, args->items[i]);
            PUSH_XMM0(cg);
        } else {
            emit_int_expr(cg, args->items[i]);
            PUSH_RAX(cg);
        }
    }

    // Allocate shadow space + slots for stack args.
    int stack_args = n > 4 ? n - 4 : 0;
    int alloc = 32 + stack_args * 8;
    // Align to 16 bytes.
    if (alloc % 16 != 0) alloc += 8;
    emit(cg, "    sub  rsp, %d", alloc);

    // Evaluate and store stack args (args 4+) into [rsp+32], [rsp+40]...
    for (int i = 4; i < n; i++) {
        SlagType t = expr_type(cg, args->items[i], TYPE_INT);
        int slot = 32 + (i - 4) * 8;
        if (t == TYPE_FLOAT) {
            emit_float_expr(cg, args->items[i]);
            emit(cg, "    movsd [rsp+%d], xmm0", slot);
        } else {
            emit_int_expr(cg, args->items[i]);
            emit(cg, "    mov  [rsp+%d], rax", slot);
        }
    }

    // Pop first 4 args into registers (they were pushed left-to-right,
    // so arg3 is on top — we need a temp area above rsp to swap).
    // Use r10/r11 as scratch for args 2/3, rcx/rdx for 0/1.
    // Simpler: use the shadow space slots to hold them then load.
    // We pushed arg0..arg(reg_args-1) before sub rsp, so they are at
    // [rsp+alloc], [rsp+alloc+8], ... from arg0 upward.
    for (int i = 0; i < reg_args; i++) {
        int off = alloc + (reg_args - 1 - i) * 8;
        SlagType t = expr_type(cg, args->items[i], TYPE_INT);
        if (t == TYPE_FLOAT) {
            emit(cg, "    movsd xmm%d, [rsp+%d]", i, off);
        } else {
            emit(cg, "    mov  %s, [rsp+%d]", int_regs[i], off);
        }
    }

    emit(cg, "    call _%s", name);

    // Clean up shadow + stack args + temp reg storage + alignment pad.
    emit(cg, "    add  rsp, %d", alloc + reg_args * 8 + align_pad);
}

// Dispatch a call expression (EXPR_CALL or EXPR_MEMBER_CALL).
static void emit_call_expr(Codegen *cg, const Expr *e) {
    if (e->kind == EXPR_CALL) {
        const char *name = e->as.call.name;
        const ExprList *args = &e->as.call.args;

        if (strcmp(name, "print") == 0) {
            emit_print(cg, args, 0);
        } else if (strcmp(name, "println") == 0) {
            emit_print(cg, args, 1);
        } else if (strcmp(name, "readline") == 0) {
            emit_call_prologue(cg);
            emit(cg, "    call _slag_readline   ; rax = str ptr, rdx = len");
            emit_call_epilogue(cg, 0);
        } else if (strcmp(name, "readfile") == 0) {
            if (args->count >= 1) {
                int idx = add_str_const(cg, args->items[0]->as.str.value);
                emit(cg, "    lea  rcx, [_str%d]", idx);
                emit_call_prologue(cg);
                emit(cg, "    call _slag_readfile   ; rcx=path -> rax=ptr, rdx=len");
                emit_call_epilogue(cg, 0);
            }
        } else if (strcmp(name, "match") == 0) {
            // match(str, regex) -> str array
            // For now emit a stub call.
            emit(cg, "    ; match() - regex engine not yet implemented");
            emit(cg, "    xor  rax, rax");
        } else if (strcmp(name, "sleep") == 0) {
            // sleep(ms) - busy-wait delay using QueryPerformanceCounter.
            // Accurate to microseconds; spins one core while counting.
            if (args->count >= 1) {
                int loop = new_label(cg);
                emit(cg, "    ; sleep(ms) busy-wait");
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  [_sleep_target], rax   ; ms");
                emit(cg, "    lea  rcx, [_qpc_freq]");
                emit(cg, "    sub  rsp, 32");
                emit(cg, "    call QueryPerformanceFrequency");
                emit(cg, "    add  rsp, 32");
                emit(cg, "    mov  rax, [_sleep_target]");
                emit(cg, "    mov  rcx, [_qpc_freq]");
                emit(cg, "    mul  rcx                    ; rdx:rax = ms * freq");
                emit(cg, "    mov  rcx, 1000");
                emit(cg, "    div  rcx                    ; rax = delay ticks");
                emit(cg, "    mov  [_sleep_target], rax");
                emit(cg, "    lea  rcx, [_qpc_now]");
                emit(cg, "    sub  rsp, 32");
                emit(cg, "    call QueryPerformanceCounter");
                emit(cg, "    add  rsp, 32");
                emit(cg, "    mov  rax, [_qpc_now]");
                emit(cg, "    add  rax, [_sleep_target]   ; target = start + delay");
                emit(cg, "    mov  [_sleep_target], rax");
                emit(cg, ".L%d:", loop);
                emit(cg, "    lea  rcx, [_qpc_now]");
                emit(cg, "    sub  rsp, 32");
                emit(cg, "    call QueryPerformanceCounter");
                emit(cg, "    add  rsp, 32");
                emit(cg, "    mov  rax, [_qpc_now]");
                emit(cg, "    cmp  rax, [_sleep_target]");
                emit(cg, "    jb   .L%d", loop);
            }
        } else if (strcmp(name, "sqrt") == 0 ||
                   strcmp(name, "sin") == 0 ||
                   strcmp(name, "cos") == 0) {
            // Float-returning intrinsics; normally handled in emit_float_expr.
            // If one reaches here (e.g. result discarded in a statement), just
            // evaluate it into xmm0 via the float path.
            emit_float_expr(cg, e);
        } else if (strcmp(name, "pixel") == 0) {
            // pixel(x, y, r, g, b)
            emit(cg, "    ; pixel()");
            emit_user_call(cg, "slag_pixel", args);
        } else if (strcmp(name, "fill_triangle") == 0) {
            // fill_triangle(x0,y0,x1,y1,x2,y2,r,g,b)
            emit(cg, "    ; fill_triangle()");
            emit_user_call(cg, "slag_fill_triangle", args);
        } else if (strcmp(name, "fill_triangle_gradient") == 0) {
            // fill_triangle_gradient(x0,y0,r0,g0,b0,x1,y1,r1,g1,b1,x2,y2,r2,g2,b2)
            emit(cg, "    ; fill_triangle_gradient()");
            emit_user_call(cg, "slag_fill_triangle_gradient", args);
        } else if (strcmp(name, "fill_triangle_z") == 0) {
            // fill_triangle_z(x0,y0,z0,x1,y1,z1,x2,y2,z2,r,g,b)
            emit(cg, "    ; fill_triangle_z()");
            emit_user_call(cg, "slag_fill_triangle_z", args);
        } else if (strcmp(name, "fill_triangle_affine") == 0) {
            // fill_triangle_affine(x0,y0,u0,v0,x1,y1,u1,v1,x2,y2,u2,v2,tex_ptr,tex_w,tex_h)
            emit(cg, "    ; fill_triangle_affine()");
            emit_user_call(cg, "slag_fill_triangle_affine", args);
        } else if (strcmp(name, "fill_triangle_persp") == 0) {
            // fill_triangle_persp(x0,y0,z0,u0,v0,x1,y1,z1,u1,v1,x2,y2,z2,u2,v2,tex_ptr,tex_w,tex_h)
            emit(cg, "    ; fill_triangle_persp()");
            emit_user_call(cg, "slag_fill_triangle_persp", args);
        } else if (strcmp(name, "fill_triangle_pcolor") == 0) {
            // fill_triangle_pcolor(verts, tex_ptr, tex_w, tex_h)
            // verts: ptr to 24 int64s (3 vertices x 8 values: x,y,z,u,v,r,g,b)
            emit(cg, "    ; fill_triangle_pcolor()");
            emit_user_call(cg, "slag_fill_triangle_pcolor", args);
        } else if (strcmp(name, "zbuffer") == 0) {
            emit(cg, "    ; zbuffer stub");
        } else {
            // User-defined function call.
            emit_user_call(cg, name, args);
        }
        return;
    }

    if (e->kind == EXPR_MEMBER_CALL) {
        const char *member = e->as.member_call.member;
        const Expr *base_expr = e->as.member_call.base;
        const char *base = (base_expr && base_expr->kind == EXPR_IDENT)
                                ? base_expr->as.str.value : "";
        const ExprList *args = &e->as.member_call.args;

        // window.open(w, h, title)
        if (strcmp(member, "open") == 0 && strcmp(base, "window") == 0) {
           emit(cg, "    ; window.open");
        if (args->count >= 3) {
           // w → rcx
           emit_int_expr(cg, args->items[0]);
           emit(cg, "    mov  r12, rax");
           // h → rdx
           emit_int_expr(cg, args->items[1]);
           emit(cg, "    mov  r13, rax");
           // optional fullscreen flag -> r14 (default 0)
           if (args->count >= 4) {
               emit_int_expr(cg, args->items[3]);
               emit(cg, "    mov  r14, rax");
           } else {
               emit(cg, "    xor  r14, r14");
           }
           // title string -> r8 (ptr), r9 (len)
           emit_str_expr(cg, args->items[2]);
           // rax=ptr, rdx=len at this point
           emit(cg, "    mov  r8,  rax");
           emit(cg, "    mov  r9,  rdx");
           emit(cg, "    mov  rcx, r12");
           emit(cg, "    mov  rdx, r13");
           emit(cg, "    sub  rsp, 48");
           emit(cg, "    mov  qword [rsp+32], r14   ; arg5 = fullscreen flag");
           emit(cg, "    call _slag_window_open");
           emit(cg, "    add  rsp, 48");
       }
   }
        // window.close()
        else if (strcmp(member, "close") == 0 && strcmp(base, "window") == 0) {
            emit(cg, "    ; window.close");
            emit_call_prologue(cg);
            emit(cg, "    call _slag_window_close");
            emit_call_epilogue(cg, 0);
        }
        // window.capture_mouse() - trap cursor in window
        else if (strcmp(member, "capture_mouse") == 0) {
            emit(cg, "    ; window.capture_mouse");
            emit_call_prologue(cg);
            emit(cg, "    call _slag_window_capture_mouse");
            emit_call_epilogue(cg, 0);
        }
        // window.release_mouse() - release cursor
        else if (strcmp(member, "release_mouse") == 0) {
            emit(cg, "    ; window.release_mouse");
            emit_call_prologue(cg);
            emit(cg, "    call _slag_window_release_mouse");
            emit_call_epilogue(cg, 0);
        }
        // window.center_cursor() - move cursor to window center
        else if (strcmp(member, "center_cursor") == 0) {
            emit(cg, "    ; window.center_cursor");
            emit_call_prologue(cg);
            emit(cg, "    call _slag_window_center_cursor");
            emit_call_epilogue(cg, 0);
        }
        // window.native() -> str "WxH" native screen resolution
        else if (strcmp(member, "native") == 0) {
            emit(cg, "    ; window.native");
            emit_call_prologue(cg);
            emit(cg, "    call _slag_window_native");
            emit_call_epilogue(cg, 0);
        }
        // window.flush()
        else if (strcmp(member, "flush") == 0) {
            emit(cg, "    ; window.flush");
            emit_call_prologue(cg);
            emit(cg, "    call _slag_window_flush");
            emit_call_epilogue(cg, 0);
        }
        // window.clear(r, g, b) - clear DIB buffer to solid color
        else if (strcmp(member, "clear") == 0 && strcmp(base, "window") == 0) {
            emit(cg, "    ; window.clear");
            if (args->count >= 3) {
            emit_int_expr(cg, args->items[0]);  // r
            emit(cg, "    mov  r12, rax");
            emit_int_expr(cg, args->items[1]);  // g
            emit(cg, "    mov  r13, rax");
            emit_int_expr(cg, args->items[2]);  // b
            emit(cg, "    mov  r8,  rax");
            emit(cg, "    mov  rcx, r12");
            emit(cg, "    mov  rdx, r13");
            emit_call_prologue(cg);
            emit(cg, "    call _slag_window_clear");
            emit_call_epilogue(cg, 0);
            }
        }
        // window.clip(x0, y0, x1, y1) - set raster clip rect for pcolor
        else if (strcmp(member, "clip") == 0 && strcmp(base, "window") == 0) {
            emit(cg, "    ; window.clip");
            if (args->count >= 4) {
            emit_int_expr(cg, args->items[0]);  // x0
            emit(cg, "    mov  r12, rax");
            emit_int_expr(cg, args->items[1]);  // y0
            emit(cg, "    mov  r13, rax");
            emit_int_expr(cg, args->items[2]);  // x1
            emit(cg, "    mov  r14, rax");
            emit_int_expr(cg, args->items[3]);  // y1
            emit(cg, "    mov  r9,  rax");
            emit(cg, "    mov  rcx, r12");
            emit(cg, "    mov  rdx, r13");
            emit(cg, "    mov  r8,  r14");
            emit_call_prologue(cg);
            emit(cg, "    call _slag_window_clip");
            emit_call_epilogue(cg, 0);
            }
        }
        // window.text(x, y, str|int, r, g, b) - draw text at x,y with color
        else if (strcmp(member, "text") == 0 && args->count >= 6) {
            emit(cg, "    ; window.text");
            emit(cg, "    push r12");
            emit(cg, "    push r13");
            emit(cg, "    push r14");
            emit(cg, "    push r15");
            // eval x, y
            emit_int_expr(cg, args->items[0]);
            emit(cg, "    mov  r12, rax");
            emit_int_expr(cg, args->items[1]);
            emit(cg, "    mov  r13, rax");
            // detect type of 3rd arg (text content)
            SlagType text_type = expr_type(cg, args->items[2], TYPE_STR);
            if (text_type == TYPE_INT) {
                // int: convert via _slag_itoa
                emit(cg, "    sub  rsp, 24");
                emit_int_expr(cg, args->items[2]);
                emit(cg, "    mov  rcx, rax");
                emit(cg, "    lea  rdx, [rsp]");
                emit(cg, "    call _slag_itoa");
                emit(cg, "    mov  r15, rax");      // len
                emit(cg, "    lea  r14, [rsp]");    // ptr
            } else {
                // str
                emit(cg, "    sub  rsp, 24");       // balance stack
                emit_str_expr(cg, args->items[2]);
                emit(cg, "    mov  r14, rax");
                emit(cg, "    mov  r15, rdx");
            }
            // eval r, g, b
            emit_int_expr(cg, args->items[3]);
            emit(cg, "    push rax");
            emit_int_expr(cg, args->items[4]);
            emit(cg, "    push rax");
            emit_int_expr(cg, args->items[5]);
            emit(cg, "    mov  r10, rax");
            emit(cg, "    pop  r11");
            emit(cg, "    pop  rax");
            // setup call
            emit(cg, "    sub  rsp, 56");
            emit(cg, "    mov  [rsp+32], rax");
            emit(cg, "    mov  [rsp+40], r11");
            emit(cg, "    mov  [rsp+48], r10");
            emit(cg, "    mov  rcx, r12");
            emit(cg, "    mov  rdx, r13");
            emit(cg, "    mov  r8,  r14");
            emit(cg, "    mov  r9,  r15");
            emit(cg, "    call _slag_window_text");
            emit(cg, "    add  rsp, 80");          // 56 + 24
            emit(cg, "    pop  r15");
            emit(cg, "    pop  r14");
            emit(cg, "    pop  r13");
            emit(cg, "    pop  r12");
        }
        // window.textbuf(x, y, ptr, len, r, g, b) - draw a runtime byte
        // buffer (len bytes at ptr) as text; same GDI path as window.text,
        // but the text comes from a pointer+length instead of a str/int.
        else if (strcmp(member, "textbuf") == 0 && args->count >= 7) {
            emit(cg, "    ; window.textbuf");
            emit(cg, "    push r12");
            emit(cg, "    push r13");
            emit(cg, "    push r14");
            emit(cg, "    push r15");
            emit_int_expr(cg, args->items[0]);
            emit(cg, "    mov  r12, rax");         // x
            emit_int_expr(cg, args->items[1]);
            emit(cg, "    mov  r13, rax");         // y
            emit(cg, "    sub  rsp, 24");          // balance (matches window.text)
            emit_int_expr(cg, args->items[2]);
            emit(cg, "    mov  r14, rax");         // ptr
            emit_int_expr(cg, args->items[3]);
            emit(cg, "    mov  r15, rax");         // len
            emit_int_expr(cg, args->items[4]);
            emit(cg, "    push rax");              // r
            emit_int_expr(cg, args->items[5]);
            emit(cg, "    push rax");              // g
            emit_int_expr(cg, args->items[6]);
            emit(cg, "    mov  r10, rax");         // b
            emit(cg, "    pop  r11");              // g
            emit(cg, "    pop  rax");              // r
            emit(cg, "    sub  rsp, 56");
            emit(cg, "    mov  [rsp+32], rax");    // r
            emit(cg, "    mov  [rsp+40], r11");    // g
            emit(cg, "    mov  [rsp+48], r10");    // b
            emit(cg, "    mov  rcx, r12");
            emit(cg, "    mov  rdx, r13");
            emit(cg, "    mov  r8,  r14");
            emit(cg, "    mov  r9,  r15");
            emit(cg, "    call _slag_window_text");
            emit(cg, "    add  rsp, 80");          // 56 + 24
            emit(cg, "    pop  r15");
            emit(cg, "    pop  r14");
            emit(cg, "    pop  r13");
            emit(cg, "    pop  r12");
        }
        // window.is_open() -> int (0 or 1) - TLS-based
        else if (strcmp(member, "is_open") == 0) {
            emit(cg, "    ; window.is_open (TLS)");
            emit(cg, "    push rbx");
            emit(cg, "    sub  rsp, 40");
            emit(cg, "    mov  rcx, [_window_tls_index]");
            emit(cg, "    call TlsGetValue");
            emit(cg, "    mov  rbx, rax");
            emit(cg, "    mov  rax, [rbx + WSTATE_OPEN]");
            emit(cg, "    add  rsp, 40");
            emit(cg, "    pop  rbx");
        }
        // window.width() -> int current client width (TLS, tracks resize)
        else if (strcmp(member, "width") == 0 && strcmp(base, "window") == 0) {
            int nl = new_label(cg);
            emit(cg, "    ; window.width (TLS, null-guarded -> 0 if no window)");
            emit(cg, "    push rbx");
            emit(cg, "    sub  rsp, 40");
            emit(cg, "    mov  rcx, [_window_tls_index]");
            emit(cg, "    call TlsGetValue");
            emit(cg, "    mov  rbx, rax");
            emit(cg, "    test rbx, rbx");
            emit(cg, "    jz   .L%d", nl);
            emit(cg, "    mov  rax, [rbx + WSTATE_WIDTH]");
            emit(cg, ".L%d:", nl);
            emit(cg, "    add  rsp, 40");
            emit(cg, "    pop  rbx");
        }
        // window.height() -> int current client height (TLS, tracks resize)
        else if (strcmp(member, "height") == 0 && strcmp(base, "window") == 0) {
            int nl = new_label(cg);
            emit(cg, "    ; window.height (TLS, null-guarded -> 0 if no window)");
            emit(cg, "    push rbx");
            emit(cg, "    sub  rsp, 40");
            emit(cg, "    mov  rcx, [_window_tls_index]");
            emit(cg, "    call TlsGetValue");
            emit(cg, "    mov  rbx, rax");
            emit(cg, "    test rbx, rbx");
            emit(cg, "    jz   .L%d", nl);
            emit(cg, "    mov  rax, [rbx + WSTATE_HEIGHT]");
            emit(cg, ".L%d:", nl);
            emit(cg, "    add  rsp, 40");
            emit(cg, "    pop  rbx");
        }
        // input.drag_x() -> int accumulated drag offset x (per-window, resolved via TLS)
        else if (strcmp(member, "drag_x") == 0) {
            emit(cg, "    ; input.drag_x");
            emit_input_resolve_window(cg);
            emit(cg, "    mov  rax, [rbx + WSTATE_INPUT_DRAG_X]");
            emit(cg, "    pop  rbx");
        }
        // input.drag_y() -> int accumulated drag offset y
        else if (strcmp(member, "drag_y") == 0) {
            emit(cg, "    ; input.drag_y");
            emit_input_resolve_window(cg);
            emit(cg, "    mov  rax, [rbx + WSTATE_INPUT_DRAG_Y]");
            emit(cg, "    pop  rbx");
        }
        // input.is_dragging() -> int 0/1
        else if (strcmp(member, "is_dragging") == 0) {
            emit(cg, "    ; input.is_dragging");
            emit_input_resolve_window(cg);
            emit(cg, "    mov  rax, [rbx + WSTATE_INPUT_DRAGGING]");
            emit(cg, "    pop  rbx");
        }
        // input.wheel() -> int accumulated wheel delta, resets to 0
        else if (strcmp(member, "wheel") == 0) {
            emit(cg, "    ; input.wheel");
            emit_input_resolve_window(cg);
            emit(cg, "    mov  rax, [rbx + WSTATE_INPUT_WHEEL]");
            emit(cg, "    mov  qword [rbx + WSTATE_INPUT_WHEEL], 0");
            emit(cg, "    pop  rbx");
        }
        // input.set_dragging(v)
        else if (strcmp(member, "set_dragging") == 0) {
            emit(cg, "    ; input.set_dragging");
            if (args->count >= 1) {
                emit_input_resolve_window(cg);
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  [rbx + WSTATE_INPUT_DRAGGING], rax");
                emit(cg, "    pop  rbx");
            }
        }
        // input.add_drag(dx, dy) -- accumulate drag offset
        else if (strcmp(member, "add_drag") == 0) {
            emit(cg, "    ; input.add_drag");
            if (args->count >= 2) {
                emit_input_resolve_window(cg);
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    add  [rbx + WSTATE_INPUT_DRAG_X], rax");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    add  [rbx + WSTATE_INPUT_DRAG_Y], rax");
                emit(cg, "    pop  rbx");
            }
        }
        // input.add_wheel(delta)
        else if (strcmp(member, "add_wheel") == 0) {
            emit(cg, "    ; input.add_wheel");
            if (args->count >= 1) {
                emit_input_resolve_window(cg);
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    add  [rbx + WSTATE_INPUT_WHEEL], rax");
                emit(cg, "    pop  rbx");
            }
        }
        // input.last_x() / input.last_y() -- last recorded mouse position
        else if (strcmp(member, "last_x") == 0) {
            emit(cg, "    ; input.last_x");
            emit_input_resolve_window(cg);
            emit(cg, "    mov  rax, [rbx + WSTATE_INPUT_LAST_X]");
            emit(cg, "    pop  rbx");
        }
        else if (strcmp(member, "last_y") == 0) {
            emit(cg, "    ; input.last_y");
            emit_input_resolve_window(cg);
            emit(cg, "    mov  rax, [rbx + WSTATE_INPUT_LAST_Y]");
            emit(cg, "    pop  rbx");
        }
        // input.set_last(x, y)
        else if (strcmp(member, "set_last") == 0) {
            emit(cg, "    ; input.set_last");
            if (args->count >= 2) {
                emit_input_resolve_window(cg);
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  [rbx + WSTATE_INPUT_LAST_X], rax");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  [rbx + WSTATE_INPUT_LAST_Y], rax");
                emit(cg, "    pop  rbx");
            }
        }
        // time.now_ms() -> int milliseconds since system start (GetTickCount)
        else if (strcmp(member, "now_ms") == 0) {
            emit(cg, "    ; time.now_ms");
            emit(cg, "    sub  rsp, 32");
            emit(cg, "    call GetTickCount");
            emit(cg, "    add  rsp, 32");
            emit(cg, "    mov  eax, eax         ; zero-extend 32-bit result into rax");
        }
        // time.now_us() -> int microseconds (QueryPerformanceCounter-based)
        else if (strcmp(member, "now_us") == 0) {
            emit(cg, "    ; time.now_us");
            emit(cg, "    lea  rcx, [_qpc_now]");
            emit(cg, "    sub  rsp, 32");
            emit(cg, "    call QueryPerformanceCounter");
            emit(cg, "    add  rsp, 32");
            emit(cg, "    lea  rcx, [_qpc_freq]");
            emit(cg, "    sub  rsp, 32");
            emit(cg, "    call QueryPerformanceFrequency");
            emit(cg, "    add  rsp, 32");
            emit(cg, "    mov  rax, [_qpc_now]");
            emit(cg, "    mov  rcx, 1000000");
            emit(cg, "    xor  rdx, rdx");
            emit(cg, "    mul  rcx              ; rdx:rax = counter * 1e6");
            emit(cg, "    mov  rcx, [_qpc_freq]");
            emit(cg, "    div  rcx              ; rax = (counter*1e6)/freq = microseconds");
        }
        else if (strcmp(member, "set_bbox") == 0) {
            emit(cg, "    ; input.set_bbox");
            if (args->count >= 4) {
                emit_input_resolve_window(cg);
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  [rbx + WSTATE_INPUT_BBOX_MINX], rax");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  [rbx + WSTATE_INPUT_BBOX_MINY], rax");
                emit_int_expr(cg, args->items[2]);
                emit(cg, "    mov  [rbx + WSTATE_INPUT_BBOX_MAXX], rax");
                emit_int_expr(cg, args->items[3]);
                emit(cg, "    mov  [rbx + WSTATE_INPUT_BBOX_MAXY], rax");
                emit(cg, "    pop  rbx");
            }
        }
        // input.in_bbox(mx, my) -> int 1/0
        else if (strcmp(member, "in_bbox") == 0) {
            emit(cg, "    ; input.in_bbox");
            if (args->count >= 2) {
                int fail_label = new_label(cg);
                int done_label = new_label(cg);
                emit_input_resolve_window(cg);
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  r10, rax            ; r10 = mx");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  r11, rax            ; r11 = my");
                emit(cg, "    cmp  r10, [rbx + WSTATE_INPUT_BBOX_MINX]");
                emit(cg, "    jl   .L%d", fail_label);
                emit(cg, "    cmp  r10, [rbx + WSTATE_INPUT_BBOX_MAXX]");
                emit(cg, "    jg   .L%d", fail_label);
                emit(cg, "    cmp  r11, [rbx + WSTATE_INPUT_BBOX_MINY]");
                emit(cg, "    jl   .L%d", fail_label);
                emit(cg, "    cmp  r11, [rbx + WSTATE_INPUT_BBOX_MAXY]");
                emit(cg, "    jg   .L%d", fail_label);
                emit(cg, "    mov  rax, 1");
                emit(cg, "    pop  rbx");
                emit(cg, "    jmp  .L%d", done_label);
                emit(cg, ".L%d:", fail_label);
                emit(cg, "    mov  rax, 0");
                emit(cg, "    pop  rbx");
                emit(cg, ".L%d:", done_label);
            } else {
                emit(cg, "    mov  rax, 0");
            }
        }
        // zbuffer.clear()
        else if (strcmp(member, "clear") == 0 && strcmp(base, "zbuffer") == 0) {
            emit(cg, "    ; zbuffer.clear");
            emit_call_prologue(cg);
            emit(cg, "    call _slag_zbuffer_clear");
            emit_call_epilogue(cg, 0);
        }
        // net.start()
        else if (strcmp(member, "start") == 0) {
            emit(cg, "    ; net.start");
            emit_call_prologue(cg);
            emit(cg, "    call _slag_net_start");
            emit_call_epilogue(cg, 0);
        }
        else if (strcmp(member, "listen") == 0) {
            emit(cg, "    ; net.listen");
            if (args->count >= 1) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  rcx, rax");
                emit_call_prologue(cg);
                emit(cg, "    call _slag_net_listen");
                emit_call_epilogue(cg, 0);
            }
        }
        else if (strcmp(member, "connect") == 0) {
            emit(cg, "    ; net.connect");
            if (args->count >= 2) {
                emit_str_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  rdx, rax");
                emit(cg, "    mov  rcx, r12");
                emit_call_prologue(cg);
                emit(cg, "    call _slag_net_connect");
                emit_call_epilogue(cg, 0);
            }
        }
        else if (strcmp(member, "send") == 0) {
            emit(cg, "    ; net.send (single byte)");
            if (args->count >= 1) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  rcx, rax");
                emit_call_prologue(cg);
                emit(cg, "    call _slag_net_send_byte");
                emit_call_epilogue(cg, 0);
            }
        }
        else if (strcmp(member, "recv") == 0) {
            emit(cg, "    ; net.recv (single byte)");
            emit_call_prologue(cg);
            emit(cg, "    call _slag_net_recv_byte");
            emit_call_epilogue(cg, 0);
        }
        // net.send_buf(ptr, len) — send len bytes from buffer
        else if (strcmp(member, "send_buf") == 0) {
            emit(cg, "    ; net.send_buf");
            if (args->count >= 2) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  rdx, rax");
                emit(cg, "    mov  rcx, r12");
                emit_call_prologue(cg);
                emit(cg, "    call _slag_net_send_buf");
                emit_call_epilogue(cg, 0);
            }
        }
        // net.recv_buf(ptr, maxlen) -> int bytes received
        else if (strcmp(member, "recv_buf") == 0) {
            emit(cg, "    ; net.recv_buf");
            if (args->count >= 2) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  rdx, rax");
                emit(cg, "    mov  rcx, r12");
                emit_call_prologue(cg);
                emit(cg, "    call _slag_net_recv_buf");
                emit_call_epilogue(cg, 0);
            }
        }
        // net.recv_buf_exact(ptr, n) -> int: n=complete, -2=pending, -1=err
        else if (strcmp(member, "recv_buf_exact") == 0) {
            emit(cg, "    ; net.recv_buf_exact");
            if (args->count >= 2) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  rdx, rax");
                emit(cg, "    mov  rcx, r12");
                emit_call_prologue(cg);
                emit(cg, "    call _slag_net_recv_buf_exact");
                emit_call_epilogue(cg, 0);
            }
        }
        else if (strcmp(member, "ack") == 0) {
            emit(cg, "    ; net.ack");
            emit(cg, "    mov  rax, [_net_last_ok]");
        }
        else if (strcmp(member, "end") == 0) {
            emit(cg, "    ; net.end");
            emit_call_prologue(cg);
            emit(cg, "    call _slag_net_end");
            emit_call_epilogue(cg, 0);
        }
        // net.bind(port) — socket+bind+listen (no block)
        else if (strcmp(member, "bind") == 0) {
            emit(cg, "    ; net.bind");
            if (args->count >= 1) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  rcx, rax");
                emit_call_prologue(cg);
                emit(cg, "    call _slag_net_bind");
                emit_call_epilogue(cg, 0);
            }
        }
        // net.accept() — block for a peer on the bound socket
        else if (strcmp(member, "accept") == 0) {
            emit(cg, "    ; net.accept");
            emit_call_prologue(cg);
            emit(cg, "    call _slag_net_accept");
            emit_call_epilogue(cg, 0);
        }
        // net.connected() -> int 1/0
        else if (strcmp(member, "connected") == 0) {
            emit_call_prologue(cg);
            emit(cg, "    call _slag_net_connected");
            emit_call_epilogue(cg, 0);
        }
        // net.server_start(port, name) -- WSAStartup + socket+bind+listen
        // for the TCP game port (flipped non-blocking for polling accept),
        // plus opens the UDP discovery listener so this server shows up
        // for net.discover_poll() on other machines.
        else if (strcmp(member, "server_start") == 0) {
            emit(cg, "    ; net.server_start");
            if (args->count >= 2) {
                emit(cg, "    push r12");
                emit(cg, "    push r13");
                emit(cg, "    push r14");
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax");
                emit_str_expr(cg, args->items[1]);
                emit(cg, "    mov  r13, rax");
                emit(cg, "    mov  r14, rdx");
                emit(cg, "    mov  r8, r14");
                emit(cg, "    mov  rdx, r13");
                emit(cg, "    mov  rcx, r12");
                emit(cg, "    sub  rsp, 40           ; 3 pushes -> need subamount = 8 (mod 16)");
                emit(cg, "    call _slag_server_start");
                emit(cg, "    add  rsp, 40");
                emit(cg, "    pop  r14");
                emit(cg, "    pop  r13");
                emit(cg, "    pop  r12");
            }
        }
        // net.server_accept() -> int client slot idx, or -1 if none pending
        else if (strcmp(member, "server_accept") == 0) {
            emit(cg, "    ; net.server_accept");
            emit_call_prologue(cg);
            emit(cg, "    call _slag_server_accept");
            emit_call_epilogue(cg, 0);
        }
        // net.server_send(idx, byte)
        else if (strcmp(member, "server_send") == 0) {
            emit(cg, "    ; net.server_send");
            if (args->count >= 2) {
                emit(cg, "    push r12");
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  rdx, rax");
                emit(cg, "    mov  rcx, r12");
                emit(cg, "    sub  rsp, 40           ; 1 push -> need subamount = 8 (mod 16)");
                emit(cg, "    call _slag_server_send");
                emit(cg, "    add  rsp, 40");
                emit(cg, "    pop  r12");
            }
        }
        // net.server_recv(idx) -> int byte, or -1 on fail/disconnect
        else if (strcmp(member, "server_recv") == 0) {
            emit(cg, "    ; net.server_recv");
            if (args->count >= 1) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  rcx, rax");
                emit_call_prologue(cg);
                emit(cg, "    call _slag_server_recv");
                emit_call_epilogue(cg, 0);
            }
        }
        // net.server_send_buf(idx, ptr, len)
        else if (strcmp(member, "server_send_buf") == 0) {
            emit(cg, "    ; net.server_send_buf");
            if (args->count >= 3) {
                emit(cg, "    push r12");
                emit(cg, "    push r13");
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  r13, rax");
                emit_int_expr(cg, args->items[2]);
                emit(cg, "    mov  r8, rax");
                emit(cg, "    mov  rdx, r13");
                emit(cg, "    mov  rcx, r12");
                emit_call_prologue(cg);
                emit(cg, "    call _slag_server_send_buf");
                emit_call_epilogue(cg, 0);
                emit(cg, "    pop  r13");
                emit(cg, "    pop  r12");
            }
        }
        // net.server_recv_buf(idx, ptr, maxlen) -> int bytes received
        else if (strcmp(member, "server_recv_buf") == 0) {
            emit(cg, "    ; net.server_recv_buf");
            if (args->count >= 3) {
                emit(cg, "    push r12");
                emit(cg, "    push r13");
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  r13, rax");
                emit_int_expr(cg, args->items[2]);
                emit(cg, "    mov  r8, rax");
                emit(cg, "    mov  rdx, r13");
                emit(cg, "    mov  rcx, r12");
                emit_call_prologue(cg);
                emit(cg, "    call _slag_server_recv_buf");
                emit_call_epilogue(cg, 0);
                emit(cg, "    pop  r13");
                emit(cg, "    pop  r12");
            }
        }
        // net.server_recv_buf_exact(idx, ptr, n) -> int: n=complete, -2=pending, -1=err
        else if (strcmp(member, "server_recv_buf_exact") == 0) {
            emit(cg, "    ; net.server_recv_buf_exact");
            if (args->count >= 3) {
                emit(cg, "    push r12");
                emit(cg, "    push r13");
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  r13, rax");
                emit_int_expr(cg, args->items[2]);
                emit(cg, "    mov  r8, rax");
                emit(cg, "    mov  rdx, r13");
                emit(cg, "    mov  rcx, r12");
                emit_call_prologue(cg);
                emit(cg, "    call _slag_server_recv_buf_exact");
                emit_call_epilogue(cg, 0);
                emit(cg, "    pop  r13");
                emit(cg, "    pop  r12");
            }
        }
        // net.server_connected(idx) -> int 1/0 -- active check (non-blocking
        // MSG_PEEK), not just a passive flag; detects a dead connection
        // even if no real data has been sent/received yet.
        else if (strcmp(member, "server_connected") == 0) {
            emit(cg, "    ; net.server_connected");
            if (args->count >= 1) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  rcx, rax");
                emit_call_prologue(cg);
                emit(cg, "    call _slag_server_connected");
                emit_call_epilogue(cg, 0);
            }
        }
        // net.server_stop() -- close all client sockets + listen socket + WSACleanup
        else if (strcmp(member, "server_stop") == 0) {
            emit(cg, "    ; net.server_stop");
            emit_call_prologue(cg);
            emit(cg, "    call _slag_server_stop");
            emit_call_epilogue(cg, 0);
        }
        // net.discover_send() -- fires one UDP broadcast "who's out
        // there?" query. Re-call occasionally to catch servers that
        // start up later (no sleep() builtin exists, so pacing is the
        // caller's job).
        else if (strcmp(member, "discover_send") == 0) {
            emit(cg, "    ; net.discover_send");
            emit_call_prologue(cg);
            emit(cg, "    call _slag_discover_send");
            emit_call_epilogue(cg, 0);
        }
        // net.discover_poll() -> int slot idx updated/inserted, or -1
        else if (strcmp(member, "discover_poll") == 0) {
            emit(cg, "    ; net.discover_poll");
            emit_call_prologue(cg);
            emit(cg, "    call _slag_discover_poll");
            emit_call_epilogue(cg, 0);
        }
        // net.discover_count() -> int number of discovered servers
        else if (strcmp(member, "discover_count") == 0) {
            emit(cg, "    ; net.discover_count");
            emit_call_prologue(cg);
            emit(cg, "    call _slag_discover_count");
            emit_call_epilogue(cg, 0);
        }
        // net.discover_port(idx) -> int
        else if (strcmp(member, "discover_port") == 0) {
            emit(cg, "    ; net.discover_port");
            if (args->count >= 1) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  rcx, rax");
                emit_call_prologue(cg);
                emit(cg, "    call _slag_discover_port");
                emit_call_epilogue(cg, 0);
            }
        }
        // net.discover_max(idx) -> int max clients
        else if (strcmp(member, "discover_max") == 0) {
            emit(cg, "    ; net.discover_max");
            if (args->count >= 1) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  rcx, rax");
                emit_call_prologue(cg);
                emit(cg, "    call _slag_discover_max");
                emit_call_epilogue(cg, 0);
            }
        }
        // net.discover_clients(idx) -> int current connected count
        else if (strcmp(member, "discover_clients") == 0) {
            emit(cg, "    ; net.discover_clients");
            if (args->count >= 1) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  rcx, rax");
                emit_call_prologue(cg);
                emit(cg, "    call _slag_discover_clients");
                emit_call_epilogue(cg, 0);
            }
        }
        // net.discover_name(idx) -> str (direct view, no copy)
        else if (strcmp(member, "discover_name") == 0) {
            emit(cg, "    ; net.discover_name");
            if (args->count >= 1) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  rcx, rax");
                emit_call_prologue(cg);
                emit(cg, "    call _slag_discover_name");
                emit_call_epilogue(cg, 0);
            }
        }
        // net.discover_ip(idx) -> str ("a.b.c.d" formatted on demand)
        else if (strcmp(member, "discover_ip") == 0) {
            emit(cg, "    ; net.discover_ip");
            if (args->count >= 1) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  rcx, rax");
                emit_call_prologue(cg);
                emit(cg, "    call _slag_discover_ip");
                emit_call_epilogue(cg, 0);
            }
        }
        // mem.alloc(nbytes) -> int pointer (0 on fail)
        else if (strcmp(member, "alloc") == 0) {
            emit(cg, "    ; mem.alloc");
            if (args->count >= 1) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  rcx, rax");
                emit_call_prologue(cg);
                emit(cg, "    call _slag_mem_alloc");
                emit_call_epilogue(cg, 0);
            }
        }
        // mem.free(ptr)
        else if (strcmp(member, "free") == 0 && strcmp(base, "mem") == 0) {
            emit(cg, "    ; mem.free");
            if (args->count >= 1) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  rcx, rax");
                emit_call_prologue(cg);
                emit(cg, "    call _slag_mem_free");
                emit_call_epilogue(cg, 0);
            }
        }
        // mem.poke8(ptr, byteoff, val)
        else if (strcmp(member, "poke8") == 0) {
            emit(cg, "    ; mem.poke8 (inlined)");
            if (args->count >= 3) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax        ; ptr");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  r13, rax        ; byteoff");
                emit(cg, "    push r12");
                emit(cg, "    push r13");
                emit_int_expr(cg, args->items[2]);
                emit(cg, "    pop  r13");
                emit(cg, "    pop  r12");
                emit(cg, "    mov  byte [r12 + r13], al");
            }
        }
        // mem.peek8(ptr, byteoff) -> int
        else if (strcmp(member, "peek8") == 0) {
            emit(cg, "    ; mem.peek8 (inlined)");
            if (args->count >= 2) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax        ; ptr");
                emit(cg, "    push r12");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    pop  r12");
                emit(cg, "    movzx rax, byte [r12 + rax]");
            }
        }
        // mem.poke64(ptr, byteoff, val)
        else if (strcmp(member, "poke64") == 0) {
            emit(cg, "    ; mem.poke64 (inlined)");
            if (args->count >= 3) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax        ; ptr");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  r13, rax        ; byteoff");
                emit(cg, "    push r12");
                emit(cg, "    push r13");
                emit_int_expr(cg, args->items[2]);
                emit(cg, "    pop  r13");
                emit(cg, "    pop  r12");
                emit(cg, "    mov  [r12 + r13], rax");
            }
        }
        // mem.peek64(ptr, byteoff) -> int
        else if (strcmp(member, "peek64") == 0) {
            emit(cg, "    ; mem.peek64 (inlined)");
            if (args->count >= 2) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax        ; ptr");
                emit(cg, "    push r12");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    pop  r12");
                emit(cg, "    mov  rax, [r12 + rax]");
            }
        }
        // mem.pokef32(ptr, byteoff, floatval) - store a 32-bit float.
        // Lets SIMD buffers be filled with readable literals instead of
        // hand-packed IEEE-754 bit patterns. Additive: independent of the
        // integer poke/peek paths above.
        else if (strcmp(member, "pokef32") == 0) {
            emit(cg, "    ; mem.pokef32 (inlined)");
            if (args->count >= 3) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax        ; ptr");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  r13, rax        ; byteoff");
                emit(cg, "    push r12");
                emit(cg, "    push r13");
                emit_float_expr(cg, args->items[2]);   // value -> xmm0 (double)
                emit(cg, "    pop  r13");
                emit(cg, "    pop  r12");
                emit(cg, "    cvtsd2ss xmm0, xmm0  ; narrow to 32-bit float");
                emit(cg, "    movss [r12 + r13], xmm0");
            }
        }
        // file.open(path, mode) -> int handle (-1 on fail)
        // mode: 1=read, 2=write(truncate), 3=append
        else if (strcmp(member, "open") == 0 && strcmp(base, "file") == 0) {
            emit(cg, "    ; file.open");
            if (args->count >= 2) {
                emit_path_expr(cg, args->items[0]);  // rax=ptr (str or buffer)
                emit(cg, "    mov  r12, rax        ; path ptr");
                emit_int_expr(cg, args->items[1]);   // mode
                emit(cg, "    mov  rdx, rax        ; mode");
                emit(cg, "    mov  rcx, r12        ; path ptr");
                emit(cg, "    sub  rsp, 32");
                emit(cg, "    call _slag_file_open");
                emit(cg, "    add  rsp, 32");
            }
        }
        // file.close(handle)
        else if (strcmp(member, "close") == 0 && strcmp(base, "file") == 0) {
            emit(cg, "    ; file.close");
            if (args->count >= 1) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  rcx, rax        ; handle");
                emit(cg, "    sub  rsp, 32");
                emit(cg, "    call _slag_file_close");
                emit(cg, "    add  rsp, 32");
            }
        }
        // file.read(handle, buf, nbytes) -> int bytes read (-1 on fail)
        else if (strcmp(member, "read") == 0 && strcmp(base, "file") == 0) {
            emit(cg, "    ; file.read");
            if (args->count >= 3) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax        ; handle");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  r13, rax        ; buf ptr");
                emit_int_expr(cg, args->items[2]);
                emit(cg, "    mov  r8,  rax        ; nbytes");
                emit(cg, "    mov  rcx, r12");
                emit(cg, "    mov  rdx, r13");
                emit(cg, "    sub  rsp, 32");
                emit(cg, "    call _slag_file_read");
                emit(cg, "    add  rsp, 32");
            }
        }
        // file.write(handle, buf, nbytes) -> int bytes written (-1 on fail)
        else if (strcmp(member, "write") == 0 && strcmp(base, "file") == 0) {
            emit(cg, "    ; file.write");
            if (args->count >= 3) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax        ; handle");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  r13, rax        ; buf ptr");
                emit_int_expr(cg, args->items[2]);
                emit(cg, "    mov  r8,  rax        ; nbytes");
                emit(cg, "    mov  rcx, r12");
                emit(cg, "    mov  rdx, r13");
                emit(cg, "    sub  rsp, 32");
                emit(cg, "    call _slag_file_write");
                emit(cg, "    add  rsp, 32");
            }
        }
        // file.seek(handle, offset, whence) -> int new position (-1 on fail)
        else if (strcmp(member, "seek") == 0 && strcmp(base, "file") == 0) {
            emit(cg, "    ; file.seek");
            if (args->count >= 3) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax        ; handle");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  r13, rax        ; offset");
                emit_int_expr(cg, args->items[2]);
                emit(cg, "    mov  r8,  rax        ; whence");
                emit(cg, "    mov  rcx, r12");
                emit(cg, "    mov  rdx, r13");
                emit(cg, "    sub  rsp, 32");
                emit(cg, "    call _slag_file_seek");
                emit(cg, "    add  rsp, 32");
            }
        }
        // file.size(handle) -> int size in bytes (-1 on fail)
        else if (strcmp(member, "size") == 0 && strcmp(base, "file") == 0) {
            emit(cg, "    ; file.size");
            if (args->count >= 1) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  rcx, rax        ; handle");
                emit(cg, "    sub  rsp, 32");
                emit(cg, "    call _slag_file_size");
                emit(cg, "    add  rsp, 32");
            }
        }
        // file.exists(path) -> int bool (1/0)
        else if (strcmp(member, "exists") == 0 && strcmp(base, "file") == 0) {
            emit(cg, "    ; file.exists");
            if (args->count >= 1) {
                emit_path_expr(cg, args->items[0]);
                emit(cg, "    mov  rcx, rax        ; path ptr");
                emit(cg, "    sub  rsp, 32");
                emit(cg, "    call _slag_file_exists");
                emit(cg, "    add  rsp, 32");
            }
        }
        // file.delete(path) -> int bool (1/0)
        else if (strcmp(member, "delete") == 0 && strcmp(base, "file") == 0) {
            emit(cg, "    ; file.delete");
            if (args->count >= 1) {
                emit_path_expr(cg, args->items[0]);
                emit(cg, "    mov  rcx, rax        ; path ptr");
                emit(cg, "    sub  rsp, 32");
                emit(cg, "    call _slag_file_delete");
                emit(cg, "    add  rsp, 32");
            }
        }
        // file.mkdir(path) -> int bool (1/0)
        else if (strcmp(member, "mkdir") == 0 && strcmp(base, "file") == 0) {
            emit(cg, "    ; file.mkdir");
            if (args->count >= 1) {
                emit_path_expr(cg, args->items[0]);
                emit(cg, "    mov  rcx, rax        ; path ptr");
                emit(cg, "    sub  rsp, 32");
                emit(cg, "    call _slag_file_mkdir");
                emit(cg, "    add  rsp, 32");
            }
        }
        // file.rmdir(path) -> int bool (1/0); removes an empty directory
        else if (strcmp(member, "rmdir") == 0 && strcmp(base, "file") == 0) {
            emit(cg, "    ; file.rmdir");
            if (args->count >= 1) {
                emit_path_expr(cg, args->items[0]);
                emit(cg, "    mov  rcx, rax        ; path ptr");
                emit(cg, "    sub  rsp, 32");
                emit(cg, "    call _slag_file_rmdir");
                emit(cg, "    add  rsp, 32");
            }
        }
        // file.make(dir, filename) -> int bool (1/0)
        // Joins dir + "/" + filename, creates missing parent folders, then
        // creates the file (an existing file is left untouched, not truncated).
        else if (strcmp(member, "make") == 0 && strcmp(base, "file") == 0) {
            emit(cg, "    ; file.make");
            if (args->count >= 2) {
                emit_path_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax        ; dir ptr");
                emit_path_expr(cg, args->items[1]);
                emit(cg, "    mov  rdx, rax        ; filename ptr");
                emit(cg, "    mov  rcx, r12        ; dir ptr");
                emit(cg, "    sub  rsp, 32");
                emit(cg, "    call _slag_file_make");
                emit(cg, "    add  rsp, 32");
            }
        }
        // file.list_open(pattern) -> int handle (-1 on fail)
        else if (strcmp(member, "list_open") == 0 && strcmp(base, "file") == 0) {
            emit(cg, "    ; file.list_open");
            if (args->count >= 1) {
                emit_path_expr(cg, args->items[0]);
                emit(cg, "    mov  rcx, rax        ; pattern ptr");
                emit(cg, "    sub  rsp, 32");
                emit(cg, "    call _slag_file_list_open");
                emit(cg, "    add  rsp, 32");
            }
        }
        // file.list_next(handle) -> int bool (1 = advanced, 0 = no more)
        else if (strcmp(member, "list_next") == 0 && strcmp(base, "file") == 0) {
            emit(cg, "    ; file.list_next");
            if (args->count >= 1) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  rcx, rax        ; handle");
                emit(cg, "    sub  rsp, 32");
                emit(cg, "    call _slag_file_list_next");
                emit(cg, "    add  rsp, 32");
            }
        }
        // file.list_name(handle) -> str (current entry's filename)
        else if (strcmp(member, "list_name") == 0 && strcmp(base, "file") == 0) {
            emit(cg, "    ; file.list_name");
            if (args->count >= 1) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  rcx, rax        ; handle");
                emit(cg, "    sub  rsp, 32");
                emit(cg, "    call _slag_file_list_name");
                emit(cg, "    add  rsp, 32");
            }
        }
        // file.list_close(handle)
        else if (strcmp(member, "list_close") == 0 && strcmp(base, "file") == 0) {
            emit(cg, "    ; file.list_close");
            if (args->count >= 1) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  rcx, rax        ; handle");
                emit(cg, "    sub  rsp, 32");
                emit(cg, "    call _slag_file_list_close");
                emit(cg, "    add  rsp, 32");
            }
        }
        // audio.init(rate, channels, bits) -> int bool (1/0)
        else if (strcmp(member, "init") == 0 && strcmp(base, "audio") == 0) {
            emit(cg, "    ; audio.init");
            if (args->count >= 3) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax        ; rate");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  r13, rax        ; channels");
                emit_int_expr(cg, args->items[2]);
                emit(cg, "    mov  r8,  rax        ; bits");
                emit(cg, "    mov  rcx, r12");
                emit(cg, "    mov  rdx, r13");
                emit(cg, "    sub  rsp, 32");
                emit(cg, "    call _slag_audio_init");
                emit(cg, "    add  rsp, 32");
            }
        }
        // audio.close()
        else if (strcmp(member, "close") == 0 && strcmp(base, "audio") == 0) {
            emit(cg, "    ; audio.close");
            emit(cg, "    sub  rsp, 32");
            emit(cg, "    call _slag_audio_close");
            emit(cg, "    add  rsp, 32");
        }
        // audio.load(path) -> int handle (-1 on fail)
        else if (strcmp(member, "load") == 0 && strcmp(base, "audio") == 0) {
            emit(cg, "    ; audio.load");
            if (args->count >= 1) {
                emit_path_expr(cg, args->items[0]);
                emit(cg, "    mov  rcx, rax        ; path ptr");
                emit(cg, "    sub  rsp, 32");
                emit(cg, "    call _slag_audio_load");
                emit(cg, "    add  rsp, 32");
            }
        }
        // audio.free(handle)
        else if (strcmp(member, "free") == 0 && strcmp(base, "audio") == 0) {
            emit(cg, "    ; audio.free");
            if (args->count >= 1) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  rcx, rax        ; handle");
                emit(cg, "    sub  rsp, 32");
                emit(cg, "    call _slag_audio_free");
                emit(cg, "    add  rsp, 32");
            }
        }
        // audio.play(handle)
        else if (strcmp(member, "play") == 0 && strcmp(base, "audio") == 0) {
            emit(cg, "    ; audio.play");
            if (args->count >= 1) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  rcx, rax        ; handle");
                emit(cg, "    sub  rsp, 32");
                emit(cg, "    call _slag_audio_play");
                emit(cg, "    add  rsp, 32");
            }
        }
        // audio.loop(handle)
        else if (strcmp(member, "loop") == 0 && strcmp(base, "audio") == 0) {
            emit(cg, "    ; audio.loop");
            if (args->count >= 1) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  rcx, rax        ; handle");
                emit(cg, "    sub  rsp, 32");
                emit(cg, "    call _slag_audio_loop");
                emit(cg, "    add  rsp, 32");
            }
        }
        // audio.stop(handle)
        else if (strcmp(member, "stop") == 0 && strcmp(base, "audio") == 0) {
            emit(cg, "    ; audio.stop");
            if (args->count >= 1) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  rcx, rax        ; handle");
                emit(cg, "    sub  rsp, 32");
                emit(cg, "    call _slag_audio_stop");
                emit(cg, "    add  rsp, 32");
            }
        }
        // audio.pause(handle)
        else if (strcmp(member, "pause") == 0 && strcmp(base, "audio") == 0) {
            emit(cg, "    ; audio.pause");
            if (args->count >= 1) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  rcx, rax        ; handle");
                emit(cg, "    sub  rsp, 32");
                emit(cg, "    call _slag_audio_pause");
                emit(cg, "    add  rsp, 32");
            }
        }
        // audio.resume(handle)
        else if (strcmp(member, "resume") == 0 && strcmp(base, "audio") == 0) {
            emit(cg, "    ; audio.resume");
            if (args->count >= 1) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  rcx, rax        ; handle");
                emit(cg, "    sub  rsp, 32");
                emit(cg, "    call _slag_audio_resume");
                emit(cg, "    add  rsp, 32");
            }
        }
        // audio.is_paused(handle) -> int bool (1/0)
        else if (strcmp(member, "is_paused") == 0 && strcmp(base, "audio") == 0) {
            emit(cg, "    ; audio.is_paused");
            if (args->count >= 1) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  rcx, rax        ; handle");
                emit(cg, "    sub  rsp, 32");
                emit(cg, "    call _slag_audio_is_paused");
                emit(cg, "    add  rsp, 32");
            }
        }
        // audio.volume(handle, vol)
        else if (strcmp(member, "volume") == 0 && strcmp(base, "audio") == 0) {
            emit(cg, "    ; audio.volume");
            if (args->count >= 2) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax        ; handle");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  rdx, rax        ; vol");
                emit(cg, "    mov  rcx, r12");
                emit(cg, "    sub  rsp, 32");
                emit(cg, "    call _slag_audio_volume");
                emit(cg, "    add  rsp, 32");
            }
        }
        // audio.pan(handle, pan)  0=left, 128=center, 255=right
        else if (strcmp(member, "pan") == 0 && strcmp(base, "audio") == 0) {
            emit(cg, "    ; audio.pan");
            if (args->count >= 2) {
                // Preserve r12 across the pan-arg eval: emit_int_expr may
                // recurse into an inlined builtin (mem.peek8/poke*, etc.)
                // that uses r12 as scratch without saving it. push keeps the
                // stack 8 (mod 16) so sub rsp,32 realigns for the call.
                emit(cg, "    push r12");
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax        ; handle");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  rdx, rax        ; pan");
                emit(cg, "    mov  rcx, r12");
                emit(cg, "    sub  rsp, 32");
                emit(cg, "    call _slag_audio_pan");
                emit(cg, "    add  rsp, 32");
                emit(cg, "    pop  r12");
            }
        }
        // audio.master_volume(vol)
        else if (strcmp(member, "master_volume") == 0 && strcmp(base, "audio") == 0) {
            emit(cg, "    ; audio.master_volume");
            if (args->count >= 1) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  rcx, rax        ; vol");
                emit(cg, "    sub  rsp, 32");
                emit(cg, "    call _slag_audio_master_volume");
                emit(cg, "    add  rsp, 32");
            }
        }
        // audio.is_playing(handle) -> int bool (1/0)
        else if (strcmp(member, "is_playing") == 0 && strcmp(base, "audio") == 0) {
            emit(cg, "    ; audio.is_playing");
            if (args->count >= 1) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  rcx, rax        ; handle");
                emit(cg, "    sub  rsp, 32");
                emit(cg, "    call _slag_audio_is_playing");
                emit(cg, "    add  rsp, 32");
            }
        }
        // audio.position(handle) -> int byte offset
        else if (strcmp(member, "position") == 0 && strcmp(base, "audio") == 0) {
            emit(cg, "    ; audio.position");
            if (args->count >= 1) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  rcx, rax        ; handle");
                emit(cg, "    sub  rsp, 32");
                emit(cg, "    call _slag_audio_position");
                emit(cg, "    add  rsp, 32");
            }
        }
        // crypto.dh_keygen() -> (void)
        else if (strcmp(member, "dh_keygen") == 0 && strcmp(base, "crypto") == 0) {
            emit(cg, "    ; crypto.dh_keygen");
            emit(cg, "    sub  rsp, 32");
            emit(cg, "    call _slag_crypto_dh_keygen");
            emit(cg, "    add  rsp, 32");
        }
        // crypto.dh_pubkey(out_ptr) -> int len
        else if (strcmp(member, "dh_pubkey") == 0 && strcmp(base, "crypto") == 0) {
            emit(cg, "    ; crypto.dh_pubkey");
            if (args->count >= 1) {
                emit_path_expr(cg, args->items[0]);
                emit(cg, "    mov  rcx, rax        ; out_ptr");
                emit(cg, "    sub  rsp, 32");
                emit(cg, "    call _slag_crypto_dh_pubkey");
                emit(cg, "    add  rsp, 32");
            }
        }
        // crypto.dh_derive(peer_pub_ptr, peer_pub_len) -> (void)
        else if (strcmp(member, "dh_derive") == 0 && strcmp(base, "crypto") == 0) {
            emit(cg, "    ; crypto.dh_derive");
            if (args->count >= 2) {
                emit_path_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax        ; peer_pub_ptr");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  rdx, rax        ; peer_pub_len");
                emit(cg, "    mov  rcx, r12");
                emit(cg, "    sub  rsp, 32");
                emit(cg, "    call _slag_crypto_dh_derive");
                emit(cg, "    add  rsp, 32");
            }
        }
        // crypto.aes_encrypt(in_ptr, in_len, out_ptr) -> int len
        else if (strcmp(member, "aes_encrypt") == 0 && strcmp(base, "crypto") == 0) {
            emit(cg, "    ; crypto.aes_encrypt");
            if (args->count >= 3) {
                emit_path_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax        ; in_ptr");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  r13, rax        ; in_len");
                emit_path_expr(cg, args->items[2]);
                emit(cg, "    mov  r8,  rax        ; out_ptr");
                emit(cg, "    mov  rcx, r12");
                emit(cg, "    mov  rdx, r13");
                emit(cg, "    sub  rsp, 32");
                emit(cg, "    call _slag_crypto_aes_encrypt");
                emit(cg, "    add  rsp, 32");
            }
        }
        // crypto.aes_decrypt(in_ptr, in_len, out_ptr) -> int len (-1 fail)
        else if (strcmp(member, "aes_decrypt") == 0 && strcmp(base, "crypto") == 0) {
            emit(cg, "    ; crypto.aes_decrypt");
            if (args->count >= 3) {
                emit_path_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax        ; in_ptr");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  r13, rax        ; in_len");
                emit_path_expr(cg, args->items[2]);
                emit(cg, "    mov  r8,  rax        ; out_ptr");
                emit(cg, "    mov  rcx, r12");
                emit(cg, "    mov  rdx, r13");
                emit(cg, "    sub  rsp, 32");
                emit(cg, "    call _slag_crypto_aes_decrypt");
                emit(cg, "    add  rsp, 32");
            }
        }
        // cpu.physical_cores() -> int
        else if (strcmp(member, "physical_cores") == 0) {
            emit(cg, "    ; cpu.physical_cores");
            emit(cg, "    mov  rax, [_cpu_physical_cores]");
        }
        // cpu.logical_cores() -> int
        else if (strcmp(member, "logical_cores") == 0) {
            emit(cg, "    ; cpu.logical_cores");
            emit(cg, "    mov  rax, [_cpu_logical_cores]");
        }
        // cpu.threads_per_core() -> int
        else if (strcmp(member, "threads_per_core") == 0) {
            emit(cg, "    ; cpu.threads_per_core");
            emit(cg, "    mov  rax, [_cpu_threads_per_core]");
        }
        // cpu.safe_thread_limit() -> int
        else if (strcmp(member, "safe_thread_limit") == 0) {
            emit(cg, "    ; cpu.safe_thread_limit");
            emit(cg, "    mov  rax, [_cpu_safe_thread_limit]");
        }
        // cpu.hyperthreaded() -> int (0 or 1)
        else if (strcmp(member, "hyperthreaded") == 0) {
            emit(cg, "    ; cpu.hyperthreaded");
            emit(cg, "    mov  rax, [_cpu_hyperthreaded]");
        }
        // cpu.simd_detect() -> int (re-runs CPUID feature detection; returns 1)
        // Runs automatically at startup; this builtin lets Slag re-run it
        // on demand. Idempotent — writes the same flag bytes each time.
        else if (strcmp(member, "simd_detect") == 0) {
            emit(cg, "    ; cpu.simd_detect");
            emit(cg, "    sub  rsp, 32");
            emit(cg, "    call _slag_detect_simd");
            emit(cg, "    add  rsp, 32");
            emit(cg, "    mov  rax, 1");
        }
        // cpu.has_<feature>() -> int (0 or 1); single byte load of a flag.
        else if (strcmp(member, "has_sse") == 0) {
            emit(cg, "    ; cpu.has_sse");
            emit(cg, "    movzx rax, byte [_simd_f_sse]");
        }
        else if (strcmp(member, "has_sse2") == 0) {
            emit(cg, "    ; cpu.has_sse2");
            emit(cg, "    movzx rax, byte [_simd_f_sse2]");
        }
        else if (strcmp(member, "has_sse3") == 0) {
            emit(cg, "    ; cpu.has_sse3");
            emit(cg, "    movzx rax, byte [_simd_f_sse3]");
        }
        else if (strcmp(member, "has_ssse3") == 0) {
            emit(cg, "    ; cpu.has_ssse3");
            emit(cg, "    movzx rax, byte [_simd_f_ssse3]");
        }
        else if (strcmp(member, "has_sse41") == 0) {
            emit(cg, "    ; cpu.has_sse41");
            emit(cg, "    movzx rax, byte [_simd_f_sse41]");
        }
        else if (strcmp(member, "has_sse42") == 0) {
            emit(cg, "    ; cpu.has_sse42");
            emit(cg, "    movzx rax, byte [_simd_f_sse42]");
        }
        else if (strcmp(member, "has_fma") == 0) {
            emit(cg, "    ; cpu.has_fma");
            emit(cg, "    movzx rax, byte [_simd_f_fma]");
        }
        else if (strcmp(member, "has_avx") == 0) {
            emit(cg, "    ; cpu.has_avx");
            emit(cg, "    movzx rax, byte [_simd_f_avx]");
        }
        else if (strcmp(member, "has_avx2") == 0) {
            emit(cg, "    ; cpu.has_avx2");
            emit(cg, "    movzx rax, byte [_simd_f_avx2]");
        }
        else if (strcmp(member, "has_avx512f") == 0) {
            emit(cg, "    ; cpu.has_avx512f");
            emit(cg, "    movzx rax, byte [_simd_f_avx512f]");
        }
        // bit.shl(value, count) -> int (left shift)
        else if (strcmp(member, "shl") == 0) {
            emit(cg, "    ; bit.shl (inlined)");
            if (args->count >= 2) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  rcx, rax");
                emit(cg, "    mov  rax, r12");
                emit(cg, "    shl  rax, cl");
            }
        }
        // bit.shr(value, count) -> int (right shift, unsigned)
        else if (strcmp(member, "shr") == 0) {
            emit(cg, "    ; bit.shr (inlined)");
            if (args->count >= 2) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  rcx, rax");
                emit(cg, "    mov  rax, r12");
                emit(cg, "    shr  rax, cl");
            }
        }
        // mat.identity() -> reset current matrix to identity
        else if (strcmp(member, "identity") == 0) {
            emit(cg, "    ; mat.identity");
            emit_call_prologue(cg);
            emit(cg, "    call _slag_mat_identity");
            emit_call_epilogue(cg, 0);
        }
        // mat.push() -> push current matrix onto stack
        else if (strcmp(member, "push") == 0) {
            emit(cg, "    ; mat.push");
            emit_call_prologue(cg);
            emit(cg, "    call _slag_mat_push");
            emit_call_epilogue(cg, 0);
        }
        // mat.pop() -> pop matrix from stack into current
        else if (strcmp(member, "pop") == 0) {
            emit(cg, "    ; mat.pop");
            emit_call_prologue(cg);
            emit(cg, "    call _slag_mat_pop");
            emit_call_epilogue(cg, 0);
        }
        // mat.translate(x, y, z) -> multiply translation into current
        else if (strcmp(member, "translate") == 0) {
            emit(cg, "    ; mat.translate");
            if (args->count >= 3) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  r13, rax");
                emit_int_expr(cg, args->items[2]);
                emit(cg, "    mov  r8, rax");
                emit(cg, "    mov  rcx, r12");
                emit(cg, "    mov  rdx, r13");
                emit_call_prologue(cg);
                emit(cg, "    call _slag_mat_translate");
                emit_call_epilogue(cg, 0);
            }
        }
        // mat.scale(sx, sy, sz) -> multiply scale into current
        else if (strcmp(member, "scale") == 0) {
            emit(cg, "    ; mat.scale");
            if (args->count >= 3) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  r13, rax");
                emit_int_expr(cg, args->items[2]);
                emit(cg, "    mov  r8, rax");
                emit(cg, "    mov  rcx, r12");
                emit(cg, "    mov  rdx, r13");
                emit_call_prologue(cg);
                emit(cg, "    call _slag_mat_scale");
                emit_call_epilogue(cg, 0);
            }
        }
        // mat.rotate_x(angle) -> multiply X rotation into current
        else if (strcmp(member, "rotate_x") == 0) {
            emit(cg, "    ; mat.rotate_x");
            if (args->count >= 1) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  rcx, rax");
                emit_call_prologue(cg);
                emit(cg, "    call _slag_mat_rotate_x");
                emit_call_epilogue(cg, 0);
            }
        }
        // mat.rotate_y(angle) -> multiply Y rotation into current
        else if (strcmp(member, "rotate_y") == 0) {
            emit(cg, "    ; mat.rotate_y");
            if (args->count >= 1) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  rcx, rax");
                emit_call_prologue(cg);
                emit(cg, "    call _slag_mat_rotate_y");
                emit_call_epilogue(cg, 0);
            }
        }
        // mat.rotate_z(angle) -> multiply Z rotation into current
        else if (strcmp(member, "rotate_z") == 0) {
            emit(cg, "    ; mat.rotate_z");
            if (args->count >= 1) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  rcx, rax");
                emit_call_prologue(cg);
                emit(cg, "    call _slag_mat_rotate_z");
                emit_call_epilogue(cg, 0);
            }
        }
        // mat.transform_x(x, y, z) -> return transformed X coordinate
        else if (strcmp(member, "transform_x") == 0) {
            emit(cg, "    ; mat.transform_x");
            if (args->count >= 3) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  r13, rax");
                emit_int_expr(cg, args->items[2]);
                emit(cg, "    mov  r8, rax");
                emit(cg, "    mov  rcx, r12");
                emit(cg, "    mov  rdx, r13");
                emit_call_prologue(cg);
                emit(cg, "    call _slag_mat_transform_x");
                emit_call_epilogue(cg, 0);
            }
        }
        // mat.transform_y(x, y, z) -> return transformed Y coordinate
        else if (strcmp(member, "transform_y") == 0) {
            emit(cg, "    ; mat.transform_y");
            if (args->count >= 3) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  r13, rax");
                emit_int_expr(cg, args->items[2]);
                emit(cg, "    mov  r8, rax");
                emit(cg, "    mov  rcx, r12");
                emit(cg, "    mov  rdx, r13");
                emit_call_prologue(cg);
                emit(cg, "    call _slag_mat_transform_y");
                emit_call_epilogue(cg, 0);
            }
        }
        // mat.transform_z(x, y, z) -> return transformed Z coordinate
        else if (strcmp(member, "transform_z") == 0) {
            emit(cg, "    ; mat.transform_z");
            if (args->count >= 3) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  r13, rax");
                emit_int_expr(cg, args->items[2]);
                emit(cg, "    mov  r8, rax");
                emit(cg, "    mov  rcx, r12");
                emit(cg, "    mov  rdx, r13");
                emit_call_prologue(cg);
                emit(cg, "    call _slag_mat_transform_z");
                emit_call_epilogue(cg, 0);
            }
        }
        // simd.addf4(dest, a, b) -> dest ptr; adds 4 packed floats
        else if (strcmp(member, "addf4") == 0) {
            emit(cg, "    ; simd.addf4");
            if (args->count >= 3) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax        ; dest");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  r13, rax        ; a");
                emit_int_expr(cg, args->items[2]);
                emit(cg, "    mov  r14, rax        ; b");
                emit(cg, "    mov  rcx, r12");
                emit(cg, "    mov  rdx, r13");
                emit(cg, "    mov  r8,  r14");
                emit_call_prologue(cg);
                emit(cg, "    call _slag_simd_addf4");
                emit_call_epilogue(cg, 0);
            }
        }
        // simd.subf4(dest, a, b) -> dest ptr; subtracts 4 packed floats
        else if (strcmp(member, "subf4") == 0) {
            emit(cg, "    ; simd.subf4");
            if (args->count >= 3) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax        ; dest");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  r13, rax        ; a");
                emit_int_expr(cg, args->items[2]);
                emit(cg, "    mov  r14, rax        ; b");
                emit(cg, "    mov  rcx, r12");
                emit(cg, "    mov  rdx, r13");
                emit(cg, "    mov  r8,  r14");
                emit_call_prologue(cg);
                emit(cg, "    call _slag_simd_subf4");
                emit_call_epilogue(cg, 0);
            }
        }
        // simd.mulf4(dest, a, b) -> dest ptr; multiplies 4 packed floats
        else if (strcmp(member, "mulf4") == 0) {
            emit(cg, "    ; simd.mulf4");
            if (args->count >= 3) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax        ; dest");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  r13, rax        ; a");
                emit_int_expr(cg, args->items[2]);
                emit(cg, "    mov  r14, rax        ; b");
                emit(cg, "    mov  rcx, r12");
                emit(cg, "    mov  rdx, r13");
                emit(cg, "    mov  r8,  r14");
                emit_call_prologue(cg);
                emit(cg, "    call _slag_simd_mulf4");
                emit_call_epilogue(cg, 0);
            }
        }
        // simd.divf4(dest, a, b) -> dest ptr; divides 4 packed floats
        else if (strcmp(member, "divf4") == 0) {
            emit(cg, "    ; simd.divf4");
            if (args->count >= 3) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax        ; dest");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  r13, rax        ; a");
                emit_int_expr(cg, args->items[2]);
                emit(cg, "    mov  r14, rax        ; b");
                emit(cg, "    mov  rcx, r12");
                emit(cg, "    mov  rdx, r13");
                emit(cg, "    mov  r8,  r14");
                emit_call_prologue(cg);
                emit(cg, "    call _slag_simd_divf4");
                emit_call_epilogue(cg, 0);
            }
        }
        // simd.dot4(dest, a, b) -> dest ptr; dot product of 4-float vectors
        else if (strcmp(member, "dot4") == 0) {
            emit(cg, "    ; simd.dot4");
            if (args->count >= 3) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax        ; dest");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  r13, rax        ; a");
                emit_int_expr(cg, args->items[2]);
                emit(cg, "    mov  r14, rax        ; b");
                emit(cg, "    mov  rcx, r12");
                emit(cg, "    mov  rdx, r13");
                emit(cg, "    mov  r8,  r14");
                emit_call_prologue(cg);
                emit(cg, "    call _slag_simd_dot4");
                emit_call_epilogue(cg, 0);
            }
        }
        // simd.cross3(dest, a, b) -> dest ptr; cross product of 3D vectors
        else if (strcmp(member, "cross3") == 0) {
            emit(cg, "    ; simd.cross3");
            if (args->count >= 3) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax        ; dest");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  r13, rax        ; a");
                emit_int_expr(cg, args->items[2]);
                emit(cg, "    mov  r14, rax        ; b");
                emit(cg, "    mov  rcx, r12");
                emit(cg, "    mov  rdx, r13");
                emit(cg, "    mov  r8,  r14");
                emit_call_prologue(cg);
                emit(cg, "    call _slag_simd_cross3");
                emit_call_epilogue(cg, 0);
            }
        }
        // simd.normalize4(dest, v) -> dest ptr; normalizes 4-float vector
        else if (strcmp(member, "normalize4") == 0) {
            emit(cg, "    ; simd.normalize4");
            if (args->count >= 2) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax        ; dest");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  r13, rax        ; v");
                emit(cg, "    mov  rcx, r12");
                emit(cg, "    mov  rdx, r13");
                emit_call_prologue(cg);
                emit(cg, "    call _slag_simd_normalize4");
                emit_call_epilogue(cg, 0);
            }
        }
        // simd.lint4(dest, a, b, t) -> dest ptr; linear interpolation a + t*(b-a)
        else if (strcmp(member, "lint4") == 0) {
            emit(cg, "    ; simd.lint4");
            if (args->count >= 4) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax        ; dest");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  r13, rax        ; a");
                emit_int_expr(cg, args->items[2]);
                emit(cg, "    mov  r14, rax        ; b");
                emit_int_expr(cg, args->items[3]);
                emit(cg, "    mov  r15, rax        ; t");
                emit(cg, "    mov  rcx, r12");
                emit(cg, "    mov  rdx, r13");
                emit(cg, "    mov  r8,  r14");
                emit(cg, "    mov  r9,  r15");
                emit_call_prologue(cg);
                emit(cg, "    call _slag_simd_lint4");
                emit_call_epilogue(cg, 0);
            }
        }
        // simd.mat4_mul(dest, a, b) -> dest ptr; 4x4 matrix multiply
        else if (strcmp(member, "mat4_mul") == 0) {
            emit(cg, "    ; simd.mat4_mul");
            if (args->count >= 3) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax        ; dest");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  r13, rax        ; a");
                emit_int_expr(cg, args->items[2]);
                emit(cg, "    mov  r14, rax        ; b");
                emit(cg, "    mov  rcx, r12");
                emit(cg, "    mov  rdx, r13");
                emit(cg, "    mov  r8,  r14");
                emit_call_prologue(cg);
                emit(cg, "    call _slag_simd_mat4_mul");
                emit_call_epilogue(cg, 0);
            }
        }
        // simd.mat4_vec4(dest, m, v) -> dest ptr; matrix-vector multiply
        else if (strcmp(member, "mat4_vec4") == 0) {
            emit(cg, "    ; simd.mat4_vec4");
            if (args->count >= 3) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax        ; dest");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  r13, rax        ; m");
                emit_int_expr(cg, args->items[2]);
                emit(cg, "    mov  r14, rax        ; v");
                emit(cg, "    mov  rcx, r12");
                emit(cg, "    mov  rdx, r13");
                emit(cg, "    mov  r8,  r14");
                emit_call_prologue(cg);
                emit(cg, "    call _slag_simd_mat4_vec4");
                emit_call_epilogue(cg, 0);
            }
        }
        // simd.rgb565_unpack(r, g, b, pixels) -> unpacks 8 RGB565 pixels to R/G/B
        else if (strcmp(member, "rgb565_unpack") == 0) {
            emit(cg, "    ; simd.rgb565_unpack");
            if (args->count >= 4) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax        ; r_dest");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  r13, rax        ; g_dest");
                emit_int_expr(cg, args->items[2]);
                emit(cg, "    mov  r14, rax        ; b_dest");
                emit_int_expr(cg, args->items[3]);
                emit(cg, "    mov  r15, rax        ; pixels");
                emit(cg, "    mov  rcx, r12");
                emit(cg, "    mov  rdx, r13");
                emit(cg, "    mov  r8,  r14");
                emit(cg, "    mov  r9,  r15");
                emit_call_prologue(cg);
                emit(cg, "    call _slag_simd_rgb565_unpack");
                emit_call_epilogue(cg, 0);
            }
        }
        // simd.rgb565_pack(dest, r, g, b) -> packs R/G/B to 8 RGB565 pixels
        else if (strcmp(member, "rgb565_pack") == 0) {
            emit(cg, "    ; simd.rgb565_pack");
            if (args->count >= 4) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax        ; dest");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  r13, rax        ; r");
                emit_int_expr(cg, args->items[2]);
                emit(cg, "    mov  r14, rax        ; g");
                emit_int_expr(cg, args->items[3]);
                emit(cg, "    mov  r15, rax        ; b");
                emit(cg, "    mov  rcx, r12");
                emit(cg, "    mov  rdx, r13");
                emit(cg, "    mov  r8,  r14");
                emit(cg, "    mov  r9,  r15");
                emit_call_prologue(cg);
                emit(cg, "    call _slag_simd_rgb565_pack");
                emit_call_epilogue(cg, 0);
            }
        }
        // simd.rgb565_blend(dest, a, b, alpha) -> alpha blend 8 RGB565 pixels
        else if (strcmp(member, "rgb565_blend") == 0) {
            emit(cg, "    ; simd.rgb565_blend");
            if (args->count >= 4) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax        ; dest");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  r13, rax        ; a");
                emit_int_expr(cg, args->items[2]);
                emit(cg, "    mov  r14, rax        ; b");
                emit_int_expr(cg, args->items[3]);
                emit(cg, "    mov  r15, rax        ; alpha");
                emit(cg, "    mov  rcx, r12");
                emit(cg, "    mov  rdx, r13");
                emit(cg, "    mov  r8,  r14");
                emit(cg, "    mov  r9,  r15");
                emit_call_prologue(cg);
                emit(cg, "    call _slag_simd_rgb565_blend");
                emit_call_epilogue(cg, 0);
            }
        }
        // mesh.bmp_width(ptr) -> int
        else if (strcmp(member, "bmp_width") == 0) {
            emit(cg, "    ; mesh.bmp_width");
            if (args->count >= 1) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  rcx, rax");
                emit(cg, "    call _slag_mesh_bmp_width");
            }
        }
        // mesh.bmp_height(ptr) -> int
        else if (strcmp(member, "bmp_height") == 0) {
            emit(cg, "    ; mesh.bmp_height");
            if (args->count >= 1) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  rcx, rax");
                emit(cg, "    call _slag_mesh_bmp_height");
            }
        }
        // mesh.bmp_pixel(ptr, x, y) -> int (0x00RRGGBB)
        else if (strcmp(member, "bmp_pixel") == 0) {
            emit(cg, "    ; mesh.bmp_pixel");
            if (args->count >= 3) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  r13, rax");
                emit_int_expr(cg, args->items[2]);
                emit(cg, "    mov  rcx, r12");
                emit(cg, "    mov  rdx, r13");
                emit(cg, "    mov  r8, rax");
                emit(cg, "    call _slag_mesh_bmp_pixel");
            }
        }
        // mesh.bmp_gray(ptr, x, y) -> int (0-255)
        else if (strcmp(member, "bmp_gray") == 0) {
            emit(cg, "    ; mesh.bmp_gray");
            if (args->count >= 3) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  r13, rax");
                emit_int_expr(cg, args->items[2]);
                emit(cg, "    mov  rcx, r12");
                emit(cg, "    mov  rdx, r13");
                emit(cg, "    mov  r8, rax");
                emit(cg, "    call _slag_mesh_bmp_gray");
            }
        }
        // mesh.create(verts, faces) -> handle
        else if (strcmp(member, "create") == 0 && args->count >= 2) {
            emit(cg, "    ; mesh.create");
            emit_int_expr(cg, args->items[0]);
            emit(cg, "    mov  r12, rax");
            emit_int_expr(cg, args->items[1]);
            emit(cg, "    mov  rcx, r12");
            emit(cg, "    mov  rdx, rax");
            emit_call_prologue(cg);
            emit(cg, "    call _slag_mesh_create");
            emit_call_epilogue(cg, 0);
        }
        // mesh.destroy(handle) - frees mesh and internal arrays
        else if (strcmp(member, "destroy") == 0) {
            emit(cg, "    ; mesh.destroy");
            if (args->count >= 1) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  rcx, rax");
                emit_call_prologue(cg);
                emit(cg, "    call _slag_mesh_free");
                emit_call_epilogue(cg, 0);
            }
        }
        // mesh.vertex_count(handle) -> int
        else if (strcmp(member, "vertex_count") == 0) {
            emit(cg, "    ; mesh.vertex_count");
            if (args->count >= 1) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  rcx, rax");
                emit(cg, "    call _slag_mesh_vertex_count");
            }
        }
        // mesh.face_count(handle) -> int
        else if (strcmp(member, "face_count") == 0) {
            emit(cg, "    ; mesh.face_count");
            if (args->count >= 1) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  rcx, rax");
                emit(cg, "    call _slag_mesh_face_count");
            }
        }
        // mesh.set_vertex(handle, i, x, y, z)
        else if (strcmp(member, "set_vertex") == 0) {
            emit(cg, "    ; mesh.set_vertex");
            if (args->count >= 5) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax        ; handle");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  r13, rax        ; i");
                emit_int_expr(cg, args->items[2]);
                emit(cg, "    mov  r14, rax        ; x");
                emit_int_expr(cg, args->items[3]);
                emit(cg, "    mov  r15, rax        ; y");
                emit_int_expr(cg, args->items[4]);
                emit(cg, "    mov  [rsp+32], rax   ; z (5th arg)");
                emit(cg, "    mov  rcx, r12");
                emit(cg, "    mov  rdx, r13");
                emit(cg, "    mov  r8, r14");
                emit(cg, "    mov  r9, r15");
                emit(cg, "    call _slag_mesh_set_vertex");
            }
        }
        // mesh.get_vertex_x(handle, i) -> int
        else if (strcmp(member, "get_vertex_x") == 0) {
            emit(cg, "    ; mesh.get_vertex_x");
            if (args->count >= 2) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  rcx, r12");
                emit(cg, "    mov  rdx, rax");
                emit(cg, "    call _slag_mesh_get_vertex_x");
            }
        }
        // mesh.get_vertex_y(handle, i) -> int
        else if (strcmp(member, "get_vertex_y") == 0) {
            emit(cg, "    ; mesh.get_vertex_y");
            if (args->count >= 2) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  rcx, r12");
                emit(cg, "    mov  rdx, rax");
                emit(cg, "    call _slag_mesh_get_vertex_y");
            }
        }
        // mesh.get_vertex_z(handle, i) -> int
        else if (strcmp(member, "get_vertex_z") == 0) {
            emit(cg, "    ; mesh.get_vertex_z");
            if (args->count >= 2) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  rcx, r12");
                emit(cg, "    mov  rdx, rax");
                emit(cg, "    call _slag_mesh_get_vertex_z");
            }
        }
        // mesh.set_face(handle, i, v0, v1, v2)
        else if (strcmp(member, "set_face") == 0) {
            emit(cg, "    ; mesh.set_face");
            if (args->count >= 5) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  r13, rax");
                emit_int_expr(cg, args->items[2]);
                emit(cg, "    mov  r14, rax");
                emit_int_expr(cg, args->items[3]);
                emit(cg, "    mov  r15, rax");
                emit_int_expr(cg, args->items[4]);
                emit(cg, "    mov  [rsp+32], rax");
                emit(cg, "    mov  rcx, r12");
                emit(cg, "    mov  rdx, r13");
                emit(cg, "    mov  r8, r14");
                emit(cg, "    mov  r9, r15");
                emit(cg, "    call _slag_mesh_set_face");
            }
        }
        // mesh.get_face(handle, i, c) -> int (vertex index)
        else if (strcmp(member, "get_face") == 0) {
            emit(cg, "    ; mesh.get_face");
            if (args->count >= 3) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  r13, rax");
                emit_int_expr(cg, args->items[2]);
                emit(cg, "    mov  rcx, r12");
                emit(cg, "    mov  rdx, r13");
                emit(cg, "    mov  r8, rax");
                emit(cg, "    call _slag_mesh_get_face");
            }
        }
        // mesh.from_heightmap(bmp_ptr, scale_xz, scale_y) -> handle
        else if (strcmp(member, "from_heightmap") == 0) {
            emit(cg, "    ; mesh.from_heightmap");
            if (args->count >= 3) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  r13, rax");
                emit_int_expr(cg, args->items[2]);
                emit(cg, "    mov  rcx, r12");
                emit(cg, "    mov  rdx, r13");
                emit(cg, "    mov  r8, rax");
                emit_call_prologue(cg);
                emit(cg, "    call _slag_mesh_from_heightmap");
                emit_call_epilogue(cg, 0);
            }
        }
        // tex.checker(x, y, size) -> int (0 or 255)
        else if (strcmp(member, "checker") == 0) {
            emit(cg, "    ; tex.checker");
            if (args->count >= 3) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  r13, rax");
                emit_int_expr(cg, args->items[2]);
                emit(cg, "    mov  rcx, r12");
                emit(cg, "    mov  rdx, r13");
                emit(cg, "    mov  r8, rax");
                emit_call_prologue(cg);
                emit(cg, "    call _slag_tex_checker");
                emit_call_epilogue(cg, 0);
            }
        }
        // tex.gradient_h(x, width) -> int (0-255)
        else if (strcmp(member, "gradient_h") == 0) {
            emit(cg, "    ; tex.gradient_h");
            if (args->count >= 2) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  rcx, r12");
                emit(cg, "    mov  rdx, rax");
                emit_call_prologue(cg);
                emit(cg, "    call _slag_tex_gradient_h");
                emit_call_epilogue(cg, 0);
            }
        }
        // tex.gradient_v(y, height) -> int (0-255)
        else if (strcmp(member, "gradient_v") == 0) {
            emit(cg, "    ; tex.gradient_v");
            if (args->count >= 2) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  rcx, r12");
                emit(cg, "    mov  rdx, rax");
                emit_call_prologue(cg);
                emit(cg, "    call _slag_tex_gradient_v");
                emit_call_epilogue(cg, 0);
            }
        }
        // tex.brick(x, y, bw, bh, mortar) -> int (0 or 255)
        else if (strcmp(member, "brick") == 0) {
            emit(cg, "    ; tex.brick");
            if (args->count >= 5) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  r13, rax");
                emit_int_expr(cg, args->items[2]);
                emit(cg, "    mov  r14, rax");
                emit_int_expr(cg, args->items[3]);
                emit(cg, "    mov  r15, rax");
                emit_int_expr(cg, args->items[4]);
                emit(cg, "    mov  [rsp+32], rax");
                emit(cg, "    mov  rcx, r12");
                emit(cg, "    mov  rdx, r13");
                emit(cg, "    mov  r8, r14");
                emit(cg, "    mov  r9, r15");
                emit_call_prologue(cg);
                emit(cg, "    call _slag_tex_brick");
                emit_call_epilogue(cg, 0);
            }
        }
        // tex.perlin2d(x, y, freq, seed) -> int (0-255), coherent value noise
        else if (strcmp(member, "perlin2d") == 0) {
            emit(cg, "    ; tex.perlin2d");
            if (args->count >= 4) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  r13, rax");
                emit_int_expr(cg, args->items[2]);
                emit(cg, "    mov  r14, rax");
                emit_int_expr(cg, args->items[3]);
                emit(cg, "    mov  rcx, r12");
                emit(cg, "    mov  rdx, r13");
                emit(cg, "    mov  r8, r14");
                emit(cg, "    mov  r9, rax");
                emit_call_prologue(cg);
                emit(cg, "    call _slag_tex_perlin2d");
                emit_call_epilogue(cg, 0);
            }
        }
        // tex.noise2d(x, y, seed) -> int (0-255)
        else if (strcmp(member, "noise2d") == 0) {
            emit(cg, "    ; tex.noise2d");
            if (args->count >= 3) {
                emit_int_expr(cg, args->items[0]);
                emit(cg, "    mov  r12, rax");
                emit_int_expr(cg, args->items[1]);
                emit(cg, "    mov  r13, rax");
                emit_int_expr(cg, args->items[2]);
                emit(cg, "    mov  rcx, r12");
                emit(cg, "    mov  rdx, r13");
                emit(cg, "    mov  r8, rax");
                emit_call_prologue(cg);
                emit(cg, "    call _slag_tex_noise2d");
                emit_call_epilogue(cg, 0);
            }
        }
        else {
            emit(cg, "    ; unhandled member call .%s", member);
        }
        return;
    }
}

// ---------------------------------------------------------------------
// Statement emission
// ---------------------------------------------------------------------

static void emit_stmtlist(Codegen *cg, const StmtList *list) {
    for (int i = 0; i < list->count; i++) {
        emit_stmt(cg, list->items[i]);
    }
}

static void emit_stmt(Codegen *cg, const Stmt *s) {
    if (!s) return;

    switch (s->kind) {

        // ------------------------------------------------------------------
        // global TYPE name = expr;
        // ------------------------------------------------------------------
        case STMT_GLOBAL_DECL: {
            Global *g = alloc_global(cg,
                s->as.var_decl.name,
                s->as.var_decl.type,
                s->as.var_decl.init);
            (void)g; // Global init handled in emit_data_section via AST pass
            break;
        }

        // ------------------------------------------------------------------
        // local TYPE name = expr; (block-scoped, same as var for now)
        // ------------------------------------------------------------------
        case STMT_LOCAL_DECL: {
            Local *loc = alloc_local(cg,
                s->as.var_decl.name,
                s->as.var_decl.type,
                0, TYPE_UNKNOWN, 0);

            SlagType t = s->as.var_decl.type;
            emit(cg, "    ; local %s %s", slag_type_name(t), s->as.var_decl.name);

            if (t == TYPE_FLOAT) {
                emit_float_expr(cg, s->as.var_decl.init);
                emit(cg, "    movsd [rbp%+d], xmm0", loc->offset);
            } else if (t == TYPE_STR) {
                emit_str_expr(cg, s->as.var_decl.init);
                emit_store_str_local(cg, loc);
            } else {
                emit_int_expr(cg, s->as.var_decl.init);
                emit(cg, "    mov  [rbp%+d], rax", loc->offset);
            }
            break;
        }

        // ------------------------------------------------------------------
        // var TYPE[n] name = { ... } or var TYPE[] name = call(...)
        // Also handles global TYPE[n] name = { ... }
        // ------------------------------------------------------------------
        case STMT_ARRAY_DECL: {
            SlagType elem = s->as.array_decl.elem_type;
            const char *name = s->as.array_decl.name;

            // Determine size.
            int size = 0;
            if (s->as.array_decl.init_call == NULL) {
                size = s->as.array_decl.init_list.count;
            } else if (s->as.array_decl.size_expr &&
                       s->as.array_decl.size_expr->kind == EXPR_INT_LIT) {
                size = (int)s->as.array_decl.size_expr->as.int_val;
            } else {
                size = 8; // fallback; runtime size not yet supported
            }

            if (s->as.array_decl.is_global) {
                // Global array: allocate in .data section
                const ExprList *init_list = NULL;
                if (s->as.array_decl.init_call == NULL) {
                    init_list = &s->as.array_decl.init_list;
                }
                alloc_global_array(cg, name, elem, size, init_list);
                break;
            }

            // Local array: allocate on stack
            Local *loc = alloc_local(cg, name, elem, 1, elem, size);
            emit(cg, "    ; array %s[%d] %s", slag_type_name(elem), size, name);

            if (s->as.array_decl.init_call == NULL) {
                // Brace initializer: store each element.
                for (int i = 0; i < s->as.array_decl.init_list.count; i++) {
                    Expr *elem_expr = s->as.array_decl.init_list.items[i];
                    int elem_offset = loc->offset + i * 8;
                    if (elem == TYPE_FLOAT) {
                        emit_float_expr(cg, elem_expr);
                        emit(cg, "    movsd [rbp%+d], xmm0", elem_offset);
                    } else {
                        emit_int_expr(cg, elem_expr);
                        emit(cg, "    mov  [rbp%+d], rax", elem_offset);
                    }
                }
            } else {
                // Call initializer (e.g. match()): result in rax = ptr.
                emit_call_expr(cg, s->as.array_decl.init_call);
                // Store the returned pointer as the first element.
                // (Full dynamic array support requires runtime integration.)
                emit(cg, "    mov  [rbp%+d], rax", loc->offset);
            }
            break;
        }

        // ------------------------------------------------------------------
        // assignment: target = value;
        // ------------------------------------------------------------------
        case STMT_ASSIGN: {
            const Expr *target = s->as.assign.target;
            const Expr *value  = s->as.assign.value;

            if (target->kind == EXPR_IDENT) {
                Local *loc = find_local(cg, target->as.str.value);
                if (loc) {
                    emit(cg, "    ; assign %s", loc->name);
                    if (loc->type == TYPE_FLOAT) {
                        emit_float_expr(cg, value);
                        emit(cg, "    movsd [rbp%+d], xmm0", loc->offset);
                    } else if (loc->type == TYPE_STR) {
                        emit_str_expr(cg, value);
                        emit_store_str_local(cg, loc);
                    } else {
                        emit_int_expr(cg, value);
                        emit(cg, "    mov  [rbp%+d], rax", loc->offset);
                    }
                } else {
                    Global *g = find_global(cg, target->as.str.value);
                    if (g) {
                        emit(cg, "    ; assign global %s", g->name);
                        if (g->type == TYPE_FLOAT) {
                            emit_float_expr(cg, value);
                            emit(cg, "    movsd [_glob%d], xmm0", g->label_id);
                        } else if (g->type == TYPE_STR) {
                            emit_str_expr(cg, value);
                            emit(cg, "    mov  [_glob%d], rax", g->label_id);
                            emit(cg, "    mov  [_glob%d_len], rdx", g->label_id);
                        } else {
                            emit_int_expr(cg, value);
                            emit(cg, "    mov  [_glob%d], rax", g->label_id);
                        }
                    } else {
                        fprintf(stderr, "codegen error: assign to undeclared '%s'\n",
                                target->as.str.value);
                    }
                }

            } else if (target->kind == EXPR_INDEX) {

                // array[index] = value
                Local *loc = NULL;
                Global *glb = NULL;
                if (target->as.index.base->kind == EXPR_IDENT) {
                    loc = find_local(cg, target->as.index.base->as.str.value);
                    if (!loc) {
                        glb = find_global(cg, target->as.index.base->as.str.value);
                    }
                }
                if (!loc && !glb) {
                    fprintf(stderr, "codegen error: array assign to undeclared\n");
                    break;
                }
                if (loc) {
                    emit(cg, "    ; assign %s[idx]", loc->name);
                    // Compute index into r10.
                    emit_int_expr(cg, target->as.index.index);
                    emit(cg, "    mov  r10, rax");
                    // Compute value.
                    if (loc->elem_type == TYPE_FLOAT) {
                        emit_float_expr(cg, value);
                        emit(cg, "    lea  rax, [rbp%+d]", loc->offset);
                        emit(cg, "    movsd [rax + r10*8], xmm0");
                    } else {
                        emit_int_expr(cg, value);
                        emit(cg, "    mov  r11, rax");
                        emit(cg, "    lea  rax, [rbp%+d]", loc->offset);
                        emit(cg, "    mov  [rax + r10*8], r11");
                    }
                } else {
                    emit(cg, "    ; assign %s[idx] (global)", glb->name);
                    // Compute index into r10.
                    emit_int_expr(cg, target->as.index.index);
                    emit(cg, "    mov  r10, rax");
                    // Compute value.
                    if (glb->elem_type == TYPE_FLOAT) {
                        emit_float_expr(cg, value);
                        emit(cg, "    lea  rax, [_globarr%d]", glb->label_id);
                        emit(cg, "    movsd [rax + r10*8], xmm0");
                    } else {
                        emit_int_expr(cg, value);
                        emit(cg, "    mov  r11, rax");
                        emit(cg, "    lea  rax, [_globarr%d]", glb->label_id);
                        emit(cg, "    mov  [rax + r10*8], r11");
                    }
                }
            }
            break;
        }

        // ------------------------------------------------------------------
        // if (cond) { ... } else { ... }
        // ------------------------------------------------------------------
        case STMT_IF: {
            int else_label = new_label(cg);
            int end_label  = new_label(cg);

            emit(cg, "    ; if");
            emit_int_expr(cg, s->as.if_stmt.cond);
            emit(cg, "    test rax, rax");
            emit(cg, "    jz   .L%d", else_label);

            emit_stmtlist(cg, &s->as.if_stmt.then_body);

            if (s->as.if_stmt.has_else) {
                emit(cg, "    jmp  .L%d", end_label);
            }
            emit(cg, ".L%d:", else_label);

            if (s->as.if_stmt.has_else) {
                emit_stmtlist(cg, &s->as.if_stmt.else_body);
                emit(cg, ".L%d:", end_label);
            }
            break;
        }

        // ------------------------------------------------------------------
        // while (cond) { ... }
        // ------------------------------------------------------------------
        case STMT_WHILE: {
            int loop_label = new_label(cg);
            int end_label  = new_label(cg);

            emit(cg, ".L%d:   ; while", loop_label);
            emit_int_expr(cg, s->as.while_stmt.cond);
            emit(cg, "    test rax, rax");
            emit(cg, "    jz   .L%d", end_label);

            emit_stmtlist(cg, &s->as.while_stmt.body);

            emit(cg, "    jmp  .L%d", loop_label);
            emit(cg, ".L%d:", end_label);
            break;
        }

        // ------------------------------------------------------------------
        // return; | return TYPE expr;
        // ------------------------------------------------------------------
        case STMT_RETURN: {
            if (!s->as.return_stmt.is_void && s->as.return_stmt.value) {
                SlagType t = s->as.return_stmt.type;
                emit(cg, "    ; return %s", slag_type_name(t));
                if (t == TYPE_FLOAT) {
                    emit_float_expr(cg, s->as.return_stmt.value);
                    // xmm0 holds return value
                } else {
                    emit_int_expr(cg, s->as.return_stmt.value);
                    // rax holds return value
                }
            }
            // Function epilogue jump — each function has a single exit point.
            emit(cg, "    jmp  .Lepilogue");
            break;
        }

        // ------------------------------------------------------------------
        // expression statement: call(args);
        // ------------------------------------------------------------------
        case STMT_EXPR: {
            const Expr *e = s->as.expr_stmt.expr;
            if (e->kind == EXPR_CALL || e->kind == EXPR_MEMBER_CALL) {
                emit_call_expr(cg, e);
            } else {
                emit_expr(cg, e, TYPE_INT);
            }
            break;
        }

        // ------------------------------------------------------------------
        // thread { ... } — spawn a Win32 thread for the body
        // ------------------------------------------------------------------
        case STMT_THREAD: {
            if (cg->thread_body_count >= MAX_THREADS) {
                fprintf(stderr, "codegen error: too many thread{} blocks "
                        "in one function (max %d)\n", MAX_THREADS);
                break;
            }
            int tid = new_label(cg);
            // Defer the body — it's emitted as a standalone proc
            // (_slag_thread_proc_N) right after the enclosing function,
            // same pattern as on-handlers.
            cg->thread_bodies[cg->thread_body_count] = &s->as.thread_stmt.body;
            cg->thread_ids[cg->thread_body_count]    = tid;
            cg->thread_body_count++;

            int slot = cg->thread_slot++;
            if (slot >= MAX_THREADS) {
                fprintf(stderr, "codegen error: thread slot %d exceeds "
                        "_slag_thread_handles capacity (%d); add a sync{} "
                        "block to drain it\n", slot, MAX_THREADS);
                slot = MAX_THREADS - 1;
            }

            emit(cg, "    ; thread block %d -> slot %d", tid, slot);
            emit(cg, "    sub  rsp, 48                   ; 32 shadow + 2 stack args");
            emit(cg, "    xor  rcx, rcx                  ; lpThreadAttributes = NULL");
            emit(cg, "    xor  rdx, rdx                  ; dwStackSize = 0 (default)");
            emit(cg, "    lea  r8,  [_slag_thread_proc_%d] ; lpStartAddress", tid);
            emit(cg, "    xor  r9,  r9                   ; lpParameter = NULL");
            emit(cg, "    mov  qword [rsp+32], 0          ; dwCreationFlags = 0");
            emit(cg, "    mov  qword [rsp+40], 0          ; lpThreadId = NULL");
            emit(cg, "    call CreateThread");
            emit(cg, "    add  rsp, 48");
            emit(cg, "    mov  [_slag_thread_handles + %d], rax", slot * 8);
            break;
        }

        // ------------------------------------------------------------------
        // sync { ... } — wait for all threads (WaitForMultipleObjects)
        // ------------------------------------------------------------------
        case STMT_SYNC: {
            emit(cg, "    ; sync block");
            emit_stmtlist(cg, &s->as.sync_stmt.body);

            int n = cg->thread_slot;
            if (n > 0) {
                if (n > MAX_THREADS) n = MAX_THREADS;
                emit(cg, "    ; WaitForMultipleObjects(%d, _slag_thread_handles, TRUE, INFINITE)", n);
                emit(cg, "    sub  rsp, 32");
                emit(cg, "    mov  rcx, %d                   ; nCount", n);
                emit(cg, "    lea  rdx, [_slag_thread_handles]");
                emit(cg, "    mov  r8,  1                    ; bWaitAll = TRUE");
                emit(cg, "    mov  r9,  0xFFFFFFFF            ; dwMilliseconds = INFINITE");
                emit(cg, "    call WaitForMultipleObjects");
                emit(cg, "    add  rsp, 32");

                // Close handles now that they've signaled, then reset
                // the slot counter so the array can be reused by any
                // thread{} blocks that follow.
                for (int i = 0; i < n; i++) {
                    emit(cg, "    mov  rcx, [_slag_thread_handles + %d]", i * 8);
                    emit(cg, "    sub  rsp, 32");
                    emit(cg, "    call CloseHandle");
                    emit(cg, "    add  rsp, 32");
                }
                cg->thread_slot = 0;
            }
            break;
        }

        // ------------------------------------------------------------------
        // lock { ... } — global critical section (mutual exclusion)
        // ------------------------------------------------------------------
        case STMT_LOCK: {
            emit(cg, "    ; lock { — enter global critical section");
            emit(cg, "    lea  rcx, [_slag_lock_cs]");
            emit(cg, "    sub  rsp, 32");
            emit(cg, "    call EnterCriticalSection");
            emit(cg, "    add  rsp, 32");
            emit_stmtlist(cg, &s->as.lock_stmt.body);
            emit(cg, "    lea  rcx, [_slag_lock_cs]");
            emit(cg, "    sub  rsp, 32");
            emit(cg, "    call LeaveCriticalSection");
            emit(cg, "    add  rsp, 32");
            emit(cg, "    ; } lock — left critical section");
            break;
        }

        // ------------------------------------------------------------------
        // on key_down / mouse_move etc. — register event handler
        // ------------------------------------------------------------------
        case STMT_ON_HANDLER: {
            emit(cg, "    ; on %s — registered at window message pump",
                 s->as.on_handler.event_name);
            // The handler body is emitted as a separate labeled proc
            // during window codegen. Stub for now.
            break;
        }

        case STMT_BLOCK:
            emit_stmtlist(cg, &s->as.block.body);
            break;

        default:
            emit(cg, "    ; unhandled stmt kind %d", s->kind);
            break;
    }
}

// ---------------------------------------------------------------------
// Function emission
// ---------------------------------------------------------------------

// First pass over a function body to calculate total frame size needed.
// We walk all var/array declarations and sum up their stack space.
// This lets us emit the correct `sub rsp, N` in the prologue before
// emitting any statement code.
static int calculate_frame_size(const StmtList *list) {
    int size = 0;
    for (int i = 0; i < list->count; i++) {
        const Stmt *s = list->items[i];
        if (!s) continue;
           switch (s->kind) {
            case STMT_LOCAL_DECL:
                size += (s->as.var_decl.type == TYPE_STR) ? 16 : 8;
                break;
            case STMT_GLOBAL_DECL:
                // Globals are in .data section, not stack
                break;
            case STMT_ARRAY_DECL: {
                int n = 0;
                if (s->as.array_decl.init_call == NULL) {
                    n = s->as.array_decl.init_list.count;
                } else if (s->as.array_decl.size_expr &&
                           s->as.array_decl.size_expr->kind == EXPR_INT_LIT) {
                    n = (int)s->as.array_decl.size_expr->as.int_val;
                } else {
                    n = 8;
                }
                size += 8 * n;
                break;
            }
            case STMT_IF:
                size += calculate_frame_size(&s->as.if_stmt.then_body);
                size += calculate_frame_size(&s->as.if_stmt.else_body);
                break;
            case STMT_WHILE:
                size += calculate_frame_size(&s->as.while_stmt.body);
                break;
            case STMT_BLOCK:
                size += calculate_frame_size(&s->as.block.body);
                break;
            case STMT_THREAD:
                size += calculate_frame_size(&s->as.thread_stmt.body);
                break;
            case STMT_SYNC:
                size += calculate_frame_size(&s->as.sync_stmt.body);
                break;
            case STMT_LOCK:
                size += calculate_frame_size(&s->as.lock_stmt.body);
                break;
            default:
                break;
        }
    }
    return size;
}

// ---------------------------------------------------------------------
// Event handler procs: on key_down(int key) { ... } etc.
//
// Emitted as standalone procs named _slag_on_<event_name>, called
// directly from the WndProc in window_runtime.c. Parameter -> register
// mapping follows Win64 int-arg convention (rcx, rdx, r8, r9), matching
// what window_runtime.c passes:
//   key_down(int key)              -> rcx = key
//   key_up(int key)                -> rcx = key
//   mouse_move(int x, int y)        -> rcx = x, rdx = y
//   mouse_down(int button, int x, int y) -> rcx=button, rdx=x, r8=y
//   mouse_up(int button, int x, int y)   -> rcx=button, rdx=x, r8=y
// ---------------------------------------------------------------------
static void emit_on_handler(Codegen *cg, const Stmt *s) {
    const char *event_name = s->as.on_handler.event_name;
    const ParamList *params = &s->as.on_handler.params;
    const StmtList *body = &s->as.on_handler.body;

    // Reset per-handler local state, sized like a function.
    cg->local_count = 0;
    cg->frame_size  = 0;

    for (int i = 0; i < params->count; i++) {
        alloc_local(cg, params->items[i].name, params->items[i].type,
                     0, TYPE_UNKNOWN, 0);
    }
    int param_size = cg->frame_size;

    int body_size = calculate_frame_size(body);

    int total = (param_size + body_size + 32 + 15) & ~15;
    if (total < 64) total = 64;

    cg->local_count = 0;
    cg->frame_size  = 0;
    for (int i = 0; i < params->count; i++) {
        alloc_local(cg, params->items[i].name, params->items[i].type,
                     0, TYPE_UNKNOWN, 0);
    }

    emit(cg, "; --- on %s handler ---", event_name);
    emit(cg, "_slag_on_%s:", event_name);
    emit(cg, "    push rbp");
    emit(cg, "    mov  rbp, rsp");
    emit(cg, "    sub  rsp, %d", total);

    const char *param_regs[] = { "rcx", "rdx", "r8", "r9" };
    for (int i = 0; i < params->count && i < 4; i++) {
        Local *loc = find_local(cg, params->items[i].name);
        if (loc) {
            if (params->items[i].type == TYPE_FLOAT) {
                emit(cg, "    movsd [rbp%+d], xmm%d", loc->offset, i);
            } else {
                emit(cg, "    mov  [rbp%+d], %s", loc->offset, param_regs[i]);
            }
        }
    }

    emit_stmtlist(cg, body);

    emit(cg, "    mov  rsp, rbp");
    emit(cg, "    pop  rbp");
    emit(cg, "    ret");
    emit(cg, "");
}

// Scan a function body for top-level `on` handlers and emit each as a
// standalone proc. Handlers nested inside if/while are not supported
// in v0.1 (only top-level statements in the function body are scanned).
static void emit_on_handlers(Codegen *cg, const Function *f) {
    for (int i = 0; i < f->body.count; i++) {
        Stmt *s = f->body.items[i];
        if (s->kind == STMT_ON_HANDLER) {
            emit_on_handler(cg, s);
        }
    }
}

// ---------------------------------------------------------------------
// thread {} proc emission
//
// thread{} blocks are collected (not emitted in place) while the
// enclosing function's body is generated — see STMT_THREAD in
// emit_stmt(). Once the function body is fully emitted, this walks
// the deferred list and emits each body as a standalone proc:
//
//   _slag_thread_proc_<id>:
//       ... body ...
//       xor rax, rax
//       ret
//
// Win32 thread procs take a single lpParameter arg (rcx) and return a
// DWORD; Slag threads currently take no parameters and the return
// value is unused (CreateThread is called with lpParameter = NULL).
// The proc gets its own fresh local symbol table sized for its body,
// completely separate from the enclosing function's frame — captured
// variables are not currently supported (file-scope shared data should
// be used instead, per spec 11.4).
// ---------------------------------------------------------------------
static void emit_thread_proc(Codegen *cg, const StmtList *body, int tid) {
    cg->local_count = 0;
    cg->frame_size  = 0;

    int body_size = calculate_frame_size(body);
    int total = (body_size + 32 + 15) & ~15;
    if (total < 64) total = 64;

    emit(cg, "; --- thread proc %d ---", tid);
    emit(cg, "_slag_thread_proc_%d:", tid);
    emit(cg, "    push rbp");
    emit(cg, "    mov  rbp, rsp");
    emit(cg, "    sub  rsp, %d", total);

    emit_stmtlist(cg, body);

    emit(cg, "    xor  rax, rax        ; DWORD return value (unused)");
    emit(cg, "    mov  rsp, rbp");
    emit(cg, "    pop  rbp");
    emit(cg, "    ret");
    emit(cg, "");
}

// Emit all thread{} procs deferred while generating the function that
// was just finished, then clear the deferral list for the next one.
static void emit_thread_procs(Codegen *cg) {
    for (int i = 0; i < cg->thread_body_count; i++) {
        emit_thread_proc(cg, cg->thread_bodies[i], cg->thread_ids[i]);
    }
    cg->thread_body_count = 0;
}

static void emit_function(Codegen *cg, const Function *f) {
    // Reset per-function state.
    cg->local_count = 0;
    cg->frame_size  = 0;


    // First pass: calculate param space.
    for (int i = 0; i < f->params.count; i++) {
        alloc_local(cg, f->params.items[i].name,
                    f->params.items[i].type, 0, TYPE_UNKNOWN, 0);
    }
    int param_size = cg->frame_size;

    // Calculate body frame size.
    int body_size = calculate_frame_size(&f->body);

    // Total frame: params + body + 32 bytes shadow space for calls,
    // rounded up to 16-byte alignment. Minimum 64.
    int total = (param_size + body_size + 32 + 15) & ~15;
    if (total < 64) total = 64;

    // Reset symbol table so alloc_local during emit_stmtlist assigns
    // offsets starting from rbp-8 downward within the reserved frame.
    cg->local_count = 0;
    cg->frame_size  = 0;

    // Second pass: re-alloc params so their offsets are registered before
    // the body is emitted (find_local must resolve param names in body).
    for (int i = 0; i < f->params.count; i++) {
        alloc_local(cg, f->params.items[i].name,
                    f->params.items[i].type, 0, TYPE_UNKNOWN, 0);
    }

    // Emit label.
    if (strcmp(f->name, "main") == 0) {
        emit(cg, "main:");
        emit(cg, "_%s:", f->name);
    } else {
        emit(cg, "_%s:", f->name);
    }

    // Prologue.
    emit(cg, "    push rbp");
    emit(cg, "    mov  rbp, rsp");
    emit(cg, "    sub  rsp, %d", total);

    // Spill incoming parameters from registers into their stack slots.
    const char *param_regs[] = { "rcx", "rdx", "r8", "r9" };
    for (int i = 0; i < f->params.count && i < 4; i++) {
        Local *loc = find_local(cg, f->params.items[i].name);
        if (loc) {
            if (f->params.items[i].type == TYPE_FLOAT) {
                emit(cg, "    movsd [rbp%+d], xmm%d", loc->offset, i);
            } else {
                emit(cg, "    mov  [rbp%+d], %s", loc->offset, param_regs[i]);
            }
        }
    }
    // Params beyond the first 4 arrive on the stack at [rbp+16],
    // [rbp+24], ... (after saved rbp and return address; the 32-byte
    // shadow space the caller reserved sits below these in the
    // caller's frame and does not shift these offsets). Copy them
    // into their local slots too.
    for (int i = 4; i < f->params.count; i++) {
        Local *loc = find_local(cg, f->params.items[i].name);
        if (loc) {
            int src_off = 48 + (i - 4) * 8;
            if (f->params.items[i].type == TYPE_FLOAT) {
                emit(cg, "    movsd xmm0, [rbp+%d]", src_off);
                emit(cg, "    movsd [rbp%+d], xmm0", loc->offset);
            } else {
                emit(cg, "    mov  rax, [rbp+%d]", src_off);
                emit(cg, "    mov  [rbp%+d], rax", loc->offset);
            }
        }
    }

    // Emit body.
    emit_stmtlist(cg, &f->body);

    // Epilogue (single exit point; all returns jump here).
    emit(cg, ".Lepilogue:");
    emit(cg, "    mov  rsp, rbp");
    emit(cg, "    pop  rbp");
    emit(cg, "    ret");
    emit(cg, "");

    // Any thread{} blocks encountered while emitting this function's
    // body are deferred until here, so their procs don't get spliced
    // into the middle of the enclosing function's instruction stream.
    emit_thread_procs(cg);
    cg->thread_slot = 0;
}

// ---------------------------------------------------------------------
// Runtime helpers emitted inline in the .text section
// ---------------------------------------------------------------------

static void emit_runtime_helpers(Codegen *cg) {
    // _slag_itoa: convert signed 64-bit int in rcx to decimal ASCII.
    // rcx = value, rdx = output buffer (caller-allocated, min 21 bytes).
    // Returns length in rax.
    emit(cg, "; --- _slag_itoa ---");
    emit(cg, "_slag_itoa:");
    emit(cg, "    push rbp");
    emit(cg, "    mov  rbp, rsp");
    emit(cg, "    push rsi");
    emit(cg, "    push rdi");
    emit(cg, "    sub  rsp, 32");
    emit(cg, "    mov  rax, rcx       ; value");
    emit(cg, "    mov  r8,  rdx       ; output buffer");
    emit(cg, "    test rax, rax");
    emit(cg, "    jns  .itoa_pos");
    emit(cg, "    neg  rax");
    emit(cg, "    mov  byte [r8], '-'");
    emit(cg, "    inc  r8");
    emit(cg, ".itoa_pos:");
    emit(cg, "    mov  rcx, 10");
    emit(cg, "    lea  r9,  [rsp+20]  ; end of temp buf");
    emit(cg, "    mov  r10, r9");
    emit(cg, ".itoa_loop:");
    emit(cg, "    xor  rdx, rdx");
    emit(cg, "    div  rcx            ; rax=quot, rdx=rem");
    emit(cg, "    add  dl, '0'");
    emit(cg, "    dec  r10");
    emit(cg, "    mov  [r10], dl");
    emit(cg, "    test rax, rax");
    emit(cg, "    jnz  .itoa_loop");
    emit(cg, "    ; copy digits to output");
    emit(cg, "    mov  rsi, r10");
    emit(cg, "    mov  rdi, r8");
    emit(cg, "    mov  rcx, r9");
    emit(cg, "    sub  rcx, r10       ; digit count");
    emit(cg, "    mov  rax, rcx       ; return length");
    emit(cg, "    rep  movsb");
    emit(cg, "    add  rsp, 32");
    emit(cg, "    pop  rdi");
    emit(cg, "    pop  rsi");
    emit(cg, "    pop  rbp");
    emit(cg, "    ret");
    emit(cg, "");

    // _slag_ftoa: convert double in xmm0 to ASCII in rcx buffer.
    // Prints up to 6 decimal places. Returns length in rax.
    // Simple implementation: split into integer and fractional parts.
    emit(cg, "; --- _slag_ftoa ---");
    emit(cg, "_slag_ftoa:");
    emit(cg, "    push rbp");
    emit(cg, "    mov  rbp, rsp");
    emit(cg, "    sub  rsp, 8             ; align rsp to 16 for calls");
    emit(cg, "    mov  r8,  rcx           ; output buffer");
    emit(cg, "    xor  r9,  r9            ; r9 = chars written so far");
    emit(cg, "    ; --- sign handling: if value is negative, emit '-' and");
    emit(cg, "    ; work on the absolute value so |x|<1 cases keep their sign ---");
    emit(cg, "    movq rax, xmm0          ; raw double bits");
    emit(cg, "    test rax, rax           ; sign bit = bit 63 -> SF");
    emit(cg, "    jns  .ftoa_nonneg");
    emit(cg, "    mov  byte [r8], '-'");
    emit(cg, "    inc  r8");
    emit(cg, "    inc  r9");
    emit(cg, "    btr  rax, 63            ; clear sign bit in the raw bits");
    emit(cg, "    movq xmm0, rax          ; xmm0 = |value|");
    emit(cg, ".ftoa_nonneg:");
    emit(cg, "    ; integer part");
    emit(cg, "    cvttsd2si rax, xmm0");
    emit(cg, "    mov  rcx, rax");
    emit(cg, "    mov  rdx, r8");
    emit(cg, "    push r9");
    emit(cg, "    call _slag_itoa         ; rax = int part length");
    emit(cg, "    pop  r9");
    emit(cg, "    add  r9,  rax           ; r9 = total chars so far");
    emit(cg, "    add  r8,  rax");
    emit(cg, "    ; decimal point");
    emit(cg, "    mov  byte [r8], '.'");
    emit(cg, "    inc  r8");
    emit(cg, "    inc  r9");
    emit(cg, "    ; fractional part: multiply by 1000000, take integer");
    emit(cg, "    cvttsd2si rcx, xmm0");
    emit(cg, "    cvtsi2sd  xmm1, rcx");
    emit(cg, "    subsd     xmm0, xmm1   ; xmm0 = fractional part");
    emit(cg, "    mov  rax,  1000000");
    emit(cg, "    cvtsi2sd  xmm1, rax");
    emit(cg, "    mulsd     xmm0, xmm1");
    emit(cg, "    cvttsd2si rax, xmm0");
    emit(cg, "    test rax, rax");
    emit(cg, "    jns  .ftoa_frac_pos");
    emit(cg, "    neg  rax");

    emit(cg, ".ftoa_frac_pos:");
    emit(cg, "    mov  rcx, rax");
    emit(cg, "    mov  rdx, r8");
    emit(cg, "    push r9");
    emit(cg, "    call _slag_itoa");
    emit(cg, "    pop  r9");
    emit(cg, "    add  r9,  rax");
    emit(cg, "    mov  rax, r9            ; total length");
    emit(cg, "    mov  rsp, rbp");
    emit(cg, "    pop  rbp");
    emit(cg, "    ret");
    emit(cg, "");

    // _slag_readline: read a line from stdin into a static buffer,
    // strip trailing \r\n. Returns rax = ptr, rdx = length.
    emit(cg, "; --- _slag_readline ---");
    emit(cg, "_slag_readline:");
    emit(cg, "    push rbp");
    emit(cg, "    mov  rbp, rsp");
    emit(cg, "    sub  rsp, 64");
    emit(cg, "    mov  rcx, [_stdin]");
    emit(cg, "    lea  rdx, [_readline_buf]");
    emit(cg, "    mov  r8,  1024          ; max chars to read");
    emit(cg, "    sub  rsp, 48            ; shadow space + out-params");
    emit(cg, "    lea  r9,  [rsp+32]      ; &charsRead");
    emit(cg, "    mov  qword [rsp+32], 0");
    emit(cg, "    mov  qword [rsp+40], 0  ; lpInputControl = NULL");
    emit(cg, "    call ReadConsoleA");
    emit(cg, "    mov  rax, [rsp+32]      ; save charsRead BEFORE restoring stack");
    emit(cg, "    add  rsp, 48");
    emit(cg, "    ; strip trailing \\r\\n");
    emit(cg, "    lea  rcx, [_readline_buf]");
    emit(cg, ".readline_strip:");
    emit(cg, "    test rax, rax");
    emit(cg, "    jz   .readline_done");
    emit(cg, "    mov  dl, [rcx + rax - 1]");
    emit(cg, "    cmp  dl, 10             ; '\\n'");
    emit(cg, "    je   .readline_dec");
    emit(cg, "    cmp  dl, 13             ; '\\r'");
    emit(cg, "    je   .readline_dec");
    emit(cg, "    jmp  .readline_done");
    emit(cg, ".readline_dec:");
    emit(cg, "    dec  rax");
    emit(cg, "    jmp  .readline_strip");
    emit(cg, ".readline_done:");
    emit(cg, "    mov  rdx, rax           ; length");
    emit(cg, "    lea  rax, [_readline_buf]");
    emit(cg, "    mov  rsp, rbp");
    emit(cg, "    pop  rbp");
    emit(cg, "    ret");
    emit(cg, "");

    // _slag_readfile: rcx = path ptr (null-terminated).
    // Returns rax = ptr to heap buffer, rdx = length in bytes.
    // On error, returns rax = 0, rdx = 0.
    emit(cg, "; --- _slag_readfile ---");
    emit(cg, "_slag_readfile:");
    emit(cg, "    push rbp");
    emit(cg, "    mov  rbp, rsp");
    emit(cg, "    push r12");
    emit(cg, "    push r13");
    emit(cg, "    push r14");
    emit(cg, "    sub  rsp, 8");
    emit(cg, "    mov  r12, rcx           ; save path ptr (non-volatile)");
    emit(cg, "");
    emit(cg, "    ; CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,");
    emit(cg, "    ;             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL)");
    emit(cg, "    mov  rcx, r12");
    emit(cg, "    mov  rdx, 0x80000000    ; GENERIC_READ");
    emit(cg, "    mov  r8,  1             ; FILE_SHARE_READ");
    emit(cg, "    xor  r9,  r9            ; lpSecurityAttributes = NULL");
    emit(cg, "    sub  rsp, 64            ; shadow(32) + 3 stack args(24) + pad(8)");
    emit(cg, "    mov  qword [rsp+32], 3  ; OPEN_EXISTING");
    emit(cg, "    mov  qword [rsp+40], 0x80 ; FILE_ATTRIBUTE_NORMAL");
    emit(cg, "    mov  qword [rsp+48], 0  ; hTemplateFile = NULL");
    emit(cg, "    call CreateFileA");
    emit(cg, "    add  rsp, 64");
    emit(cg, "    cmp  rax, -1            ; INVALID_HANDLE_VALUE");
    emit(cg, "    je   .readfile_fail");
    emit(cg, "    mov  r13, rax           ; save file handle");
    emit(cg, "");
    emit(cg, "    ; GetFileSize(handle, NULL)");
    emit(cg, "    mov  rcx, r13");
    emit(cg, "    xor  rdx, rdx");
    emit(cg, "    sub  rsp, 32");
    emit(cg, "    call GetFileSize");
    emit(cg, "    add  rsp, 32");
    emit(cg, "    mov  eax, eax           ; zero-extend 32-bit result to 64-bit");
    emit(cg, "    cmp  eax, 0xFFFFFFFF   ; INVALID_FILE_SIZE");
    emit(cg, "    je   .readfile_fail_close");
    emit(cg, "    mov  r14, rax           ; save file size");
    emit(cg, "");
    emit(cg, "    ; GetProcessHeap()");
    emit(cg, "    sub  rsp, 32");
    emit(cg, "    call GetProcessHeap");
    emit(cg, "    add  rsp, 32");
    emit(cg, "    mov  r12, rax           ; save heap handle");
    emit(cg, "");
    emit(cg, "    ; HeapAlloc(heap, 0, size)");
    emit(cg, "    mov  rcx, r12");
    emit(cg, "    xor  rdx, rdx");
    emit(cg, "    mov  r8,  r14");
    emit(cg, "    sub  rsp, 32");
    emit(cg, "    call HeapAlloc");
    emit(cg, "    add  rsp, 32");
    emit(cg, "    test rax, rax");
    emit(cg, "    jz   .readfile_fail_close");
    emit(cg, "    mov  r12, rax           ; save buffer ptr");
    emit(cg, "");
    emit(cg, "    ; ReadFile(handle, buf, size, &bytesRead, NULL)");
    emit(cg, "    mov  rcx, r13");
    emit(cg, "    mov  rdx, r12");
    emit(cg, "    mov  r8,  r14");
    emit(cg, "    sub  rsp, 48");
    emit(cg, "    lea  r9,  [rsp+32]");
    emit(cg, "    mov  qword [rsp+32], 0");
    emit(cg, "    mov  qword [rsp+40], 0  ; lpOverlapped = NULL");
    emit(cg, "    call ReadFile");
    emit(cg, "    add  rsp, 48");
    emit(cg, "");
    emit(cg, "    ; CloseHandle(file handle)");
    emit(cg, "    mov  rcx, r13");
    emit(cg, "    sub  rsp, 32");
    emit(cg, "    call CloseHandle");
    emit(cg, "    add  rsp, 32");
    emit(cg, "");
    emit(cg, "    mov  rax, r12           ; ptr");
    emit(cg, "    mov  rdx, r14           ; length");
    emit(cg, "    jmp  .readfile_done");
    emit(cg, "");
    emit(cg, ".readfile_fail_close:");
    emit(cg, "    mov  rcx, r13");
    emit(cg, "    sub  rsp, 32");
    emit(cg, "    call CloseHandle");
    emit(cg, "    add  rsp, 32");
    emit(cg, ".readfile_fail:");
    emit(cg, "    xor  rax, rax");
    emit(cg, "    xor  rdx, rdx");
    emit(cg, ".readfile_done:");
    emit(cg, "    lea  rsp, [rbp-24]      ; restore rsp to just above pushed regs");
    emit(cg, "    pop  r14");
    emit(cg, "    pop  r13");
    emit(cg, "    pop  r12");
    emit(cg, "    pop  rbp");
    emit(cg, "    ret");
    emit(cg, "");
    // _slag_str_eq: rcx=ptr1, rdx=len1, r8=ptr2, r9=len2 -> rax = 1 if equal, else 0.
    // Used by the == and != operators when either operand is a str.
    emit(cg, "; --- _slag_str_eq ---");
    emit(cg, "_slag_str_eq:");
    emit(cg, "    cmp  rdx, r9");
    emit(cg, "    jne  .str_eq_false");
    emit(cg, "    test rdx, rdx");
    emit(cg, "    jz   .str_eq_true");
    emit(cg, "    mov  r10, rdx           ; byte counter");
    emit(cg, ".str_eq_loop:");
    emit(cg, "    mov  al,  [rcx]");
    emit(cg, "    cmp  al,  [r8]");
    emit(cg, "    jne  .str_eq_false");
    emit(cg, "    inc  rcx");
    emit(cg, "    inc  r8");
    emit(cg, "    dec  r10");
    emit(cg, "    jnz  .str_eq_loop");
    emit(cg, ".str_eq_true:");
    emit(cg, "    mov  rax, 1");
    emit(cg, "    ret");
    emit(cg, ".str_eq_false:");
    emit(cg, "    xor  rax, rax");
    emit(cg, "    ret");
    emit(cg, "");
}

// ---------------------------------------------------------------------
// .data section emission
// ---------------------------------------------------------------------

static void emit_data_section(Codegen *cg) {
    emit(cg, "section .data");
    emit(cg, "");

    // Float constants.
    for (int i = 0; i < cg->float_const_count; i++) {
        // Emit as raw 8-byte little-endian using a union trick via printf.
        union { double d; unsigned long long u; } v;
        v.d = cg->float_consts[i];
        emit(cg, "_flt%d:  dq 0x%016llx   ; %f", i, v.u, v.d);
    }
    if (cg->float_const_count) emit(cg, "");

    // String constants. Always emitted with a trailing 0 byte so they
    // can double as null-terminated C strings (needed by readfile's
    // CreateFileA path argument), in addition to being used with
    // explicit ptr+len elsewhere.
    for (int i = 0; i < cg->str_const_count; i++) {
        emit_raw(cg, "_str%d:  db ", i);
        const char *s = cg->str_consts[i];
        if (*s == '\0') {
            emit_raw(cg, "0");
        } else {
            int in_quote = 0;
            for (const char *p = s; *p; p++) {
                unsigned char c = (unsigned char)*p;
                if (c == '\n' || c == '\t' || c == '\\' || c == '"' || c < 32) {
                    if (in_quote) { emit_raw(cg, "\""); in_quote = 0; }
                    if (p != s) emit_raw(cg, ", ");
                    emit_raw(cg, "%d", (int)c);
                } else {
                    if (!in_quote) {
                        if (p != s) emit_raw(cg, ", ");
                        emit_raw(cg, "\"");
                        in_quote = 1;
                    }
                    emit_raw(cg, "%c", c);
                }
            }
            if (in_quote) emit_raw(cg, "\"");
            emit_raw(cg, ", 0");  // null terminator
        }
        fprintf(cg->out, "\n");
    }
    if (cg->str_const_count) emit(cg, "");

    // Newline byte for println.
    emit(cg, "_newline: db 10");
    emit(cg, "");

    // stdout/stdin handle storage (populated at startup).
    emit(cg, "_stdout:  dq 0");
    emit(cg, "_stdin:   dq 0");
    emit(cg, "");

    // CPU topology globals (populated at startup).
    emit(cg, "_cpu_physical_cores:   dq 0");
    emit(cg, "_cpu_logical_cores:    dq 0");
    emit(cg, "_cpu_threads_per_core: dq 0");
    emit(cg, "_cpu_safe_thread_limit:dq 0");
    emit(cg, "_cpu_hyperthreaded:    dq 0   ; 1 if any core reports LTP_PC_SMT, else 0");
    emit(cg, "");

    // SIMD feature flags (populated by _slag_detect_simd at startup).
    // Each byte is 0/1. _simd_flags is a contiguous array indexed by the
    // SIMD_* constants below (kept in sync with the spec); the named
    // labels alias into it so cpu.has_*() can do a single byte load.
    emit(cg, "_simd_flags:");
    emit(cg, "_simd_f_sse:      db 0   ; [0] SSE");
    emit(cg, "_simd_f_sse2:     db 0   ; [1] SSE2");
    emit(cg, "_simd_f_sse3:     db 0   ; [2] SSE3");
    emit(cg, "_simd_f_ssse3:    db 0   ; [3] SSSE3");
    emit(cg, "_simd_f_sse41:    db 0   ; [4] SSE4.1");
    emit(cg, "_simd_f_sse42:    db 0   ; [5] SSE4.2");
    emit(cg, "_simd_f_fma:      db 0   ; [6] FMA");
    emit(cg, "_simd_f_avx:      db 0   ; [7] AVX (OS-enabled)");
    emit(cg, "_simd_f_avx2:     db 0   ; [8] AVX2 (OS-enabled)");
    emit(cg, "_simd_f_avx512f:  db 0   ; [9] AVX-512F (OS-enabled)");
    emit(cg, "_simd_flags_count equ 10");
    emit(cg, "");

    // SIMD constants (RGB565 masks).
    emit_simd_data(cg);
    emit(cg, "");

    // User-defined global variables.
    if (cg->global_count > 0) {
        emit(cg, "; User-defined globals");
        for (int i = 0; i < cg->global_count; i++) {
            Global *g = &cg->globals[i];
            Expr *init = g->init;
            // Skip arrays here - they are emitted separately
            if (g->is_array) continue;
            if (g->type == TYPE_INT || g->type == TYPE_BOOL) {
                long val = 0;
                if (init && init->kind == EXPR_INT_LIT) {
                    val = init->as.int_val;
                } else if (init && init->kind == EXPR_BOOL_LIT) {
                    val = init->as.bool_val ? 1 : 0;
                }
                emit(cg, "_glob%d:  dq %ld   ; global %s", g->label_id, val, g->name);
            } else if (g->type == TYPE_FLOAT) {
                double val = 0.0;
                if (init && init->kind == EXPR_FLOAT_LIT) {
                    val = init->as.float_val;
                } else if (init && init->kind == EXPR_INT_LIT) {
                    val = (double)init->as.int_val;
                }
                union { double d; unsigned long long u; } v;
                v.d = val;
                emit(cg, "_glob%d:  dq 0x%016llx   ; global %s = %f", g->label_id, v.u, g->name, val);
            } else if (g->type == TYPE_STR) {
                // For strings, emit pointer to string constant and length
                if (init && init->kind == EXPR_STR_LIT) {
                    int str_id = add_str_const(cg, init->as.str.value);
                    int len = (int)strlen(init->as.str.value);
                    emit(cg, "_glob%d:  dq _str%d   ; global %s (ptr)", g->label_id, str_id, g->name);
                    emit(cg, "_glob%d_len:  dq %d   ; global %s (len)", g->label_id, len, g->name);
                } else {
                    emit(cg, "_glob%d:  dq 0   ; global %s (uninitialized ptr)", g->label_id, g->name);
                    emit(cg, "_glob%d_len:  dq 0   ; global %s (len)", g->label_id, g->name);
                }
            }
        }
        // Emit global arrays
        for (int i = 0; i < cg->global_count; i++) {
            Global *g = &cg->globals[i];
            if (!g->is_array) continue;
            emit(cg, "; global array %s[%d]", g->name, g->size);
            for (int j = 0; j < g->size; j++) {
                if (j < g->arr_init.count) {
                    Expr *e = g->arr_init.items[j];
                    if (g->elem_type == TYPE_FLOAT) {
                        double val = 0.0;
                        if (e->kind == EXPR_FLOAT_LIT) {
                            val = e->as.float_val;
                        } else if (e->kind == EXPR_INT_LIT) {
                            val = (double)e->as.int_val;
                        } else if (e->kind == EXPR_UNARY && e->as.unary.op == TOK_MINUS) {
                            Expr *inner = e->as.unary.operand;
                            if (inner->kind == EXPR_FLOAT_LIT) {
                                val = -inner->as.float_val;
                            } else if (inner->kind == EXPR_INT_LIT) {
                                val = -(double)inner->as.int_val;
                            }
                        }
                        union { double d; unsigned long long u; } v;
                        v.d = val;
                        if (j == 0) {
                            emit(cg, "_globarr%d: dq 0x%016llx", g->label_id, v.u);
                        } else {
                            emit(cg, "            dq 0x%016llx", v.u);
                        }
                    } else {
                        long val = 0;
                        if (e->kind == EXPR_INT_LIT) {
                            val = e->as.int_val;
                        } else if (e->kind == EXPR_UNARY && e->as.unary.op == TOK_MINUS) {
                            Expr *inner = e->as.unary.operand;
                            if (inner->kind == EXPR_INT_LIT) {
                                val = -inner->as.int_val;
                            }
                        }
                        if (j == 0) {
                            emit(cg, "_globarr%d: dq %ld", g->label_id, val);
                        } else {
                            emit(cg, "            dq %ld", val);
                        }
                    }
                } else {
                    if (j == 0) {
                        emit(cg, "_globarr%d: dq 0", g->label_id);
                    } else {
                        emit(cg, "            dq 0");
                    }
                }
            }
        }
        emit(cg, "");
    }
}

// ---------------------------------------------------------------------
// .bss section
// ---------------------------------------------------------------------

static void emit_bss_section(Codegen *cg) {
    (void)cg;
    emit(cg, "section .bss");
    emit(cg, "_written_bytes: resq 1     ; scratch for WriteConsoleA");
    emit(cg, "_readline_buf:  resb 1024  ; line input buffer for readline()");
    emit(cg, "_qpc_now:  resq 1   ; QueryPerformanceCounter scratch");
    emit(cg, "_qpc_freq: resq 1   ; QueryPerformanceFrequency scratch");
    emit(cg, "_sleep_target: resq 1  ; sleep busy-wait target counter");
    emit(cg, "_slag_lock_cs:  resb 40   ; CRITICAL_SECTION (40 bytes x64) for lock{}");
    emit(cg, "_slag_thread_handles: resq %d  ; HANDLEs from CreateThread, drained by sync{}", MAX_THREADS);
    emit(cg, "");
}

// ---------------------------------------------------------------------
// Import table (.idata / extern declarations)
// ---------------------------------------------------------------------

static void emit_imports(Codegen *cg) {
    emit(cg, "; --- Win32 imports ---");
    emit(cg, "extern GetStdHandle");
    emit(cg, "extern WriteConsoleA");
    emit(cg, "extern ReadConsoleA");
    emit(cg, "extern ExitProcess");
    emit(cg, "extern CreateThread");
    emit(cg, "extern WaitForMultipleObjects");
    emit(cg, "extern GetLogicalProcessorInformation");
    emit(cg, "extern CreateFileA");
    emit(cg, "extern ReadFile");
    emit(cg, "extern GetFileSize");
    emit(cg, "extern CloseHandle");
    emit(cg, "extern GetProcessHeap");
    emit(cg, "extern HeapAlloc");
    emit(cg, "extern HeapFree");
    emit(cg, "extern InitializeCriticalSection");
    emit(cg, "extern TlsAlloc");
    emit(cg, "extern EnterCriticalSection");
    emit(cg, "extern LeaveCriticalSection");
    emit(cg, "extern QueryPerformanceCounter");
    emit(cg, "extern QueryPerformanceFrequency");
    emit(cg, "");
}

// ---------------------------------------------------------------------
// Startup stub: _slag_startup
//
// Called before main. Initialises stdout handle and CPU topology.
// ---------------------------------------------------------------------

static void emit_startup(Codegen *cg) {
    emit(cg, "; --- startup ---");
    emit(cg, "_slag_startup:");
    emit(cg, "    push rbp");
    emit(cg, "    mov  rbp, rsp");
    emit(cg, "    sub  rsp, 64");
    emit(cg, "");
    emit(cg, "    ; get stdout handle (STD_OUTPUT_HANDLE = -11)");
    emit(cg, "    mov  rcx, -11");
    emit(cg, "    sub  rsp, 32");
    emit(cg, "    call GetStdHandle");
    emit(cg, "    add  rsp, 32");
    emit(cg, "    mov  [_stdout], rax");
    emit(cg, "");
    emit(cg, "    ; get stdin handle (STD_INPUT_HANDLE = -10)");
    emit(cg, "    mov  rcx, -10");
    emit(cg, "    sub  rsp, 32");
    emit(cg, "    call GetStdHandle");
    emit(cg, "    add  rsp, 32");
    emit(cg, "    mov  [_stdin], rax");
    emit(cg, "");
    emit(cg, "");
    emit(cg, "    ; init global critical section for lock{}");
    emit(cg, "    lea  rcx, [_slag_lock_cs]");
    emit(cg, "    sub  rsp, 32");
    emit(cg, "    call InitializeCriticalSection");
    emit(cg, "    add  rsp, 32");
    emit(cg, "");
    emit(cg, "    ; init window TLS slot");
    emit(cg, "    sub  rsp, 32");
    emit(cg, "    call TlsAlloc");
    emit(cg, "    add  rsp, 32");
    emit(cg, "    mov  [_window_tls_index], rax");
    emit(cg, "    mov  qword [_window_tls_init], 1");
    emit(cg, "");
    emit(cg, "    call _slag_detect_cpu_topology");
    emit(cg, "    call _slag_detect_simd");
    emit(cg, "");
    emit(cg, "    mov  rsp, rbp");
    emit(cg, "    pop  rbp");
    emit(cg, "    ret");
    emit(cg, "");
}

// ---------------------------------------------------------------------
// _slag_detect_cpu_topology
//
// Calls GetLogicalProcessorInformation using the standard two-pass
// pattern: first call with a 0-size buffer to learn the required size
// (it fails with ERROR_INSUFFICIENT_BUFFER and writes the size into our
// out-param), then allocate that much from the process heap and call
// again to get the actual SYSTEM_LOGICAL_PROCESSOR_INFORMATION array.
//
// Each entry is 32 bytes on x64:
//   BYTE  Relationship   (offset 0; 0 = RelationProcessorCore)
//   BYTE  Reserved[21]
//   WORD  Reserved2
//   union { ProcessorMask (8 bytes) ... } (offset 24)
//
// For every entry with Relationship == 0 (RelationProcessorCore):
//   physical_cores += 1
//   logical_cores  += popcount(ProcessorMask)
//
// threads_per_core = logical_cores / physical_cores (integer divide;
// safe since physical_cores >= 1 whenever the call succeeds).
//
// safe_thread_limit = logical_cores - 1, floored at 1, leaving one
// logical core free for the OS/UI thread rather than oversubscribing.
//
// On any failure (heap alloc fails, API fails on the second call), all
// four globals fall back to 1 so callers always see a sane value.
// ---------------------------------------------------------------------
static void emit_cpu_topology_helper(Codegen *cg) {
    emit(cg, "; --- _slag_detect_cpu_topology ---");
    emit(cg, "_slag_detect_cpu_topology:");
    emit(cg, "    push rbp");
    emit(cg, "    mov  rbp, rsp");
    emit(cg, "    push rbx");
    emit(cg, "    push rsi");
    emit(cg, "    push rdi");
    emit(cg, "    push r12              ; required size");
    emit(cg, "    push r13              ; physical_cores accumulator");
    emit(cg, "    push r14              ; logical_cores accumulator");
    emit(cg, "    push r15              ; hyperthreaded accumulator (OR of LTP_PC_SMT)");
    emit(cg, "    sub  rsp, 40          ; 32 shadow + local dword, 16-aligned");
    emit(cg, "");
    emit(cg, "    mov  dword [rsp+32], 0");
    emit(cg, "    xor  rcx, rcx                    ; Buffer = NULL");
    emit(cg, "    lea  rdx, [rsp+32]                ; &ReturnedLength");
    emit(cg, "    call GetLogicalProcessorInformation");
    emit(cg, "    mov  r12d, [rsp+32]");
    emit(cg, "    test r12d, r12d");
    emit(cg, "    jz   .cpu_fail");
    emit(cg, "");
    emit(cg, "    call GetProcessHeap");
    emit(cg, "    mov  rbx, rax                     ; heap handle");
    emit(cg, "    mov  rcx, rbx");
    emit(cg, "    xor  rdx, rdx");
    emit(cg, "    mov  r8,  r12");
    emit(cg, "    call HeapAlloc");
    emit(cg, "    test rax, rax");
    emit(cg, "    jz   .cpu_fail");
    emit(cg, "    mov  rsi, rax                     ; buffer ptr");
    emit(cg, "");
    emit(cg, "    mov  rcx, rsi");
    emit(cg, "    lea  rdx, [rsp+32]");
    emit(cg, "    call GetLogicalProcessorInformation");
    emit(cg, "    test eax, eax");
    emit(cg, "    jz   .cpu_free_fail");
    emit(cg, "");
    emit(cg, "    xor  r13, r13                     ; physical_cores = 0");
    emit(cg, "    xor  r14, r14                     ; logical_cores = 0");
    emit(cg, "    xor  r15, r15                     ; hyperthreaded = 0");
    emit(cg, "    xor  rdi, rdi                     ; byte offset");
    emit(cg, ".cpu_loop:");
    emit(cg, "    cmp  edi, r12d");
    emit(cg, "    jge  .cpu_free_ok");
    emit(cg, "    mov  eax, [rsi + rdi + 8]         ; Relationship (DWORD at offset 8)");
    emit(cg, "    test eax, eax");
    emit(cg, "    jnz  .cpu_next                    ; not RelationProcessorCore, skip");
    emit(cg, "    inc  r13");
    emit(cg, "    mov  rax, [rsi + rdi]             ; ProcessorMask (offset 0)");
    emit(cg, "    popcnt rax, rax");
    emit(cg, "    add  r14, rax");
    emit(cg, "    movzx eax, byte [rsi + rdi + 12]  ; ProcessorCore.Flags");
    emit(cg, "    test eax, eax");
    emit(cg, "    setnz al                          ; normalize to clean 0/1 boolean");
    emit(cg, "    movzx eax, al");
    emit(cg, "    or   r15d, eax                    ; hyperthreaded |= IsTRUE(flags)");
    emit(cg, ".cpu_next:");
    emit(cg, "    add  rdi, 32                      ; sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION)");
    emit(cg, "    jmp  .cpu_loop");
    emit(cg, "");
    emit(cg, ".cpu_free_ok:");
    emit(cg, "    mov  rcx, rbx");
    emit(cg, "    xor  rdx, rdx");
    emit(cg, "    mov  r8,  rsi");
    emit(cg, "    call HeapFree");
    emit(cg, "    cmp  r13, 0");
    emit(cg, "    jle  .cpu_fail            ; no core entries found, fall back");
    emit(cg, "    mov  [_cpu_physical_cores], r13");
    emit(cg, "    mov  [_cpu_logical_cores],  r14");
    emit(cg, "    mov  rax, r14");
    emit(cg, "    xor  rdx, rdx");
    emit(cg, "    div  r13                          ; logical / physical");
    emit(cg, "    mov  [_cpu_threads_per_core], rax");
    emit(cg, "    mov  rax, r14");
    emit(cg, "    sub  rax, 1");
    emit(cg, "    cmp  rax, 1");
    emit(cg, "    jge  .cpu_limit_ok");
    emit(cg, "    mov  rax, 1");
    emit(cg, ".cpu_limit_ok:");
    emit(cg, "    mov  [_cpu_safe_thread_limit], rax");
    emit(cg, "    mov  [_cpu_hyperthreaded], r15");
    emit(cg, "    jmp  .cpu_done");
    emit(cg, "");
    emit(cg, ".cpu_free_fail:");
    emit(cg, "    mov  rcx, rbx");
    emit(cg, "    xor  rdx, rdx");
    emit(cg, "    mov  r8,  rsi");
    emit(cg, "    call HeapFree");
    emit(cg, "    jmp  .cpu_fail");
    emit(cg, "");
    emit(cg, ".cpu_fail:");
    emit(cg, "    mov  qword [_cpu_physical_cores],    1");
    emit(cg, "    mov  qword [_cpu_logical_cores],     1");
    emit(cg, "    mov  qword [_cpu_threads_per_core],  1");
    emit(cg, "    mov  qword [_cpu_safe_thread_limit], 1");
    emit(cg, "    mov  qword [_cpu_hyperthreaded],     0");
    emit(cg, "");
    emit(cg, ".cpu_done:");
    emit(cg, "    add  rsp, 40");
    emit(cg, "    pop  r15");
    emit(cg, "    pop  r14");
    emit(cg, "    pop  r13");
    emit(cg, "    pop  r12");
    emit(cg, "    pop  rdi");
    emit(cg, "    pop  rsi");
    emit(cg, "    pop  rbx");
    emit(cg, "    mov  rsp, rbp");
    emit(cg, "    pop  rbp");
    emit(cg, "    ret");
    emit(cg, "");
}

// ---------------------------------------------------------------------
// _slag_detect_simd
//
// Populates the _simd_f_* flag bytes via CPUID. Runs at startup and is
// re-runnable from Slag as cpu.simd_detect().
//
// Sources:
//   leaf 1  -> EDX bit 25 SSE, 26 SSE2; ECX bit 0 SSE3, 9 SSSE3,
//              19 SSE4.1, 20 SSE4.2, 12 FMA, 27 OSXSAVE, 28 AVX
//   leaf 7,0-> EBX bit 5 AVX2, 16 AVX-512F
//
// AVX/AVX2/AVX-512 are gated on the OS having enabled YMM/ZMM state:
// OSXSAVE (leaf1 ECX bit 27) must be set, then XGETBV(XCR0) must report
// XMM(1)+YMM(2); AVX-512 additionally needs OPMASK(5)+ZMM_hi256(6)+
// ZMM16-31(7). Without this an AVX instruction faults (#UD) on a CPU
// whose OS never turned the state on. cpuid/xgetbv clobber RBX (a Win64
// nonvolatile), so it is saved and restored.
// ---------------------------------------------------------------------
static void emit_simd_detect_helper(Codegen *cg) {
    emit(cg, "; --- _slag_detect_simd ---");
    emit(cg, "_slag_detect_simd:");
    emit(cg, "    push rbp");
    emit(cg, "    mov  rbp, rsp");
    emit(cg, "    push rbx                 ; nonvolatile; cpuid clobbers it");
    emit(cg, "    push rsi");
    emit(cg, "");
    emit(cg, "    ; ---- leaf 1: legacy SSE/AVX feature bits ----");
    emit(cg, "    mov  eax, 1");
    emit(cg, "    xor  ecx, ecx");
    emit(cg, "    cpuid");
    emit(cg, "    ; EDX: SSE(25), SSE2(26)");
    emit(cg, "    mov  esi, edx");
    emit(cg, "    shr  esi, 25");
    emit(cg, "    and  esi, 1");
    emit(cg, "    mov  [_simd_f_sse], sil");
    emit(cg, "    mov  esi, edx");
    emit(cg, "    shr  esi, 26");
    emit(cg, "    and  esi, 1");
    emit(cg, "    mov  [_simd_f_sse2], sil");
    emit(cg, "    ; ECX: SSE3(0), SSSE3(9), SSE4.1(19), SSE4.2(20), FMA(12)");
    emit(cg, "    mov  esi, ecx");
    emit(cg, "    and  esi, 1");
    emit(cg, "    mov  [_simd_f_sse3], sil");
    emit(cg, "    mov  esi, ecx");
    emit(cg, "    shr  esi, 9");
    emit(cg, "    and  esi, 1");
    emit(cg, "    mov  [_simd_f_ssse3], sil");
    emit(cg, "    mov  esi, ecx");
    emit(cg, "    shr  esi, 19");
    emit(cg, "    and  esi, 1");
    emit(cg, "    mov  [_simd_f_sse41], sil");
    emit(cg, "    mov  esi, ecx");
    emit(cg, "    shr  esi, 20");
    emit(cg, "    and  esi, 1");
    emit(cg, "    mov  [_simd_f_sse42], sil");
    emit(cg, "    mov  esi, ecx");
    emit(cg, "    shr  esi, 12");
    emit(cg, "    and  esi, 1");
    emit(cg, "    mov  [_simd_f_fma], sil");
    emit(cg, "");
    emit(cg, "    ; ---- OS-enabled AVX state check ----");
    emit(cg, "    ; require OSXSAVE (ECX bit 27) before trusting AVX/AVX2");
    emit(cg, "    mov  esi, ecx");
    emit(cg, "    shr  esi, 27");
    emit(cg, "    and  esi, 1");
    emit(cg, "    jz   .simd_no_avx        ; OS did not enable XSAVE -> no AVX");
    emit(cg, "    ; also need AVX supported by CPU (ECX bit 28)");
    emit(cg, "    mov  esi, ecx");
    emit(cg, "    shr  esi, 28");
    emit(cg, "    and  esi, 1");
    emit(cg, "    jz   .simd_no_avx");
    emit(cg, "    ; read XCR0");
    emit(cg, "    xor  ecx, ecx");
    emit(cg, "    xgetbv                   ; -> EDX:EAX = XCR0");
    emit(cg, "    ; XMM(1)+YMM(2) must both be set for AVX");
    emit(cg, "    mov  esi, eax");
    emit(cg, "    and  esi, 6              ; bits 1|2");
    emit(cg, "    cmp  esi, 6");
    emit(cg, "    jne  .simd_no_avx");
    emit(cg, "    mov  byte [_simd_f_avx], 1");
    emit(cg, "    ; keep XCR0 low dword in EDX for the AVX-512 test below");
    emit(cg, "    mov  edx, eax");
    emit(cg, "    jmp  .simd_leaf7");
    emit(cg, ".simd_no_avx:");
    emit(cg, "    mov  byte [_simd_f_avx],     0");
    emit(cg, "    mov  byte [_simd_f_avx2],    0");
    emit(cg, "    mov  byte [_simd_f_avx512f], 0");
    emit(cg, "    jmp  .simd_done");
    emit(cg, "");
    emit(cg, ".simd_leaf7:");
    emit(cg, "    ; ---- leaf 7, subleaf 0: AVX2 / AVX-512F ----");
    emit(cg, "    ; edx currently holds XCR0 low dword; preserve across cpuid");
    emit(cg, "    push rdx");
    emit(cg, "    mov  eax, 7");
    emit(cg, "    xor  ecx, ecx");
    emit(cg, "    cpuid                    ; EBX has the flags");
    emit(cg, "    pop  rdx                 ; restore XCR0 low");
    emit(cg, "    ; AVX2 = EBX bit 5 (already OS-AVX gated above)");
    emit(cg, "    mov  esi, ebx");
    emit(cg, "    shr  esi, 5");
    emit(cg, "    and  esi, 1");
    emit(cg, "    mov  [_simd_f_avx2], sil");
    emit(cg, "    ; AVX-512F = EBX bit 16, AND OS enabled OPMASK(5)+ZMMhi(6)+ZMM16_31(7)");
    emit(cg, "    mov  esi, ebx");
    emit(cg, "    shr  esi, 16");
    emit(cg, "    and  esi, 1");
    emit(cg, "    jz   .simd_no_avx512");
    emit(cg, "    mov  eax, edx            ; XCR0 low");
    emit(cg, "    and  eax, 0xE0           ; bits 5|6|7");
    emit(cg, "    cmp  eax, 0xE0");
    emit(cg, "    jne  .simd_no_avx512");
    emit(cg, "    mov  byte [_simd_f_avx512f], 1");
    emit(cg, "    jmp  .simd_done");
    emit(cg, ".simd_no_avx512:");
    emit(cg, "    mov  byte [_simd_f_avx512f], 0");
    emit(cg, "");
    emit(cg, ".simd_done:");
    emit(cg, "    pop  rsi");
    emit(cg, "    pop  rbx");
    emit(cg, "    mov  rsp, rbp");
    emit(cg, "    pop  rbp");
    emit(cg, "    ret");
    emit(cg, "");
}

// ---------------------------------------------------------------------
// Entry point: _start / WinMain stub
//
// Win64 PE console entry point. Calls _slag_startup then main,
// then ExitProcess(0).
// ---------------------------------------------------------------------

static void emit_entry(Codegen *cg, const char *entry_name) {
    emit(cg, "; --- entry point ---");
    emit(cg, "global _start");
    emit(cg, "_start:");
    emit(cg, "    sub  rsp, 8          ; align stack to 16 bytes");
    emit(cg, "    call _slag_startup");
    emit(cg, "    call _%s", entry_name);
    emit(cg, "    xor  rcx, rcx        ; exit code 0");
    emit(cg, "    sub  rsp, 32");
    emit(cg, "    call ExitProcess");
    emit(cg, "");
}

// ---------------------------------------------------------------------
// Top-level codegen entry point
// ---------------------------------------------------------------------

void codegen_program(const Program *prog, FILE *out) {
    Codegen cg;
    memset(&cg, 0, sizeof(cg));
    cg.out           = out;
    cg.label_counter = 0;

    // File header.
    emit(&cg, "; Generated by the Slag compiler");
    emit(&cg, "; NASM x86-64 Win64");
    emit(&cg, "bits 64");
    emit(&cg, "default rel");
    emit(&cg, "");

    // Imports.
    emit_imports(&cg);
    emit_window_imports(&cg);
    emit_net_imports(&cg);
    emit_server_imports(&cg);
    emit_mem_imports(&cg);
    emit_file_imports(&cg);
    emit_audio_imports(&cg);
    emit_crypto_imports(&cg);
    emit_simd_imports(&cg);

    // .text section.
    emit(&cg, "section .text");
    emit(&cg, "");

    // Entry point.
    // Pick the entry function: prefer one named "main" for backward
    // compatibility, otherwise fall back to the first function defined in
    // the file. This means a standalone function file runs without requiring
    // a main wrapper.
    const char *entry_name = "main";
    int have_main = 0;
    for (int i = 0; i < prog->functions.count; i++) {
        if (strcmp(prog->functions.items[i].name, "main") == 0) {
            have_main = 1;
            break;
        }
    }
    if (!have_main && prog->functions.count > 0) {
        entry_name = prog->functions.items[0].name;
    }
    emit_entry(&cg, entry_name);

    // Startup helper.
    emit_startup(&cg);

    // Process global declarations (populate global symbol table).
    // Also pre-add string constants so they're emitted before global references.
    for (int i = 0; i < prog->globals.count; i++) {
        const Stmt *s = prog->globals.items[i];
        if (s && s->kind == STMT_GLOBAL_DECL) {
            // Pre-add string constants for global strings
            if (s->as.var_decl.type == TYPE_STR &&
                s->as.var_decl.init &&
                s->as.var_decl.init->kind == EXPR_STR_LIT) {
                add_str_const(&cg, s->as.var_decl.init->as.str.value);
            }
            alloc_global(&cg, s->as.var_decl.name, s->as.var_decl.type, s->as.var_decl.init);
        } else if (s && s->kind == STMT_ARRAY_DECL && s->as.array_decl.is_global) {
            // Global array declaration
            int size = s->as.array_decl.init_list.count;
            if (size == 0 && s->as.array_decl.size_expr &&
                s->as.array_decl.size_expr->kind == EXPR_INT_LIT) {
                size = (int)s->as.array_decl.size_expr->as.int_val;
            }
            alloc_global_array(&cg, s->as.array_decl.name, s->as.array_decl.elem_type,
                               size, &s->as.array_decl.init_list);
        }
    }

    // Scan all functions for top-level `on` handlers so we know which
    // default stubs window_runtime.c should skip.
    EventHandlerFlags ev_flags;
    memset(&ev_flags, 0, sizeof(ev_flags));
    for (int i = 0; i < prog->functions.count; i++) {
        const Function *f = &prog->functions.items[i];
        for (int j = 0; j < f->body.count; j++) {
            Stmt *s = f->body.items[j];
            if (s->kind != STMT_ON_HANDLER) continue;
            const char *ev = s->as.on_handler.event_name;
            if (strcmp(ev, "key_down") == 0)    ev_flags.has_key_down = 1;
            else if (strcmp(ev, "key_up") == 0)    ev_flags.has_key_up = 1;
            else if (strcmp(ev, "mouse_move") == 0) ev_flags.has_mouse_move = 1;
            else if (strcmp(ev, "mouse_down") == 0) ev_flags.has_mouse_down = 1;
            else if (strcmp(ev, "mouse_up") == 0)   ev_flags.has_mouse_up = 1;
            else if (strcmp(ev, "mouse_wheel") == 0) ev_flags.has_mouse_wheel = 1;
        }
    }

    // Runtime helpers.
    emit_runtime_helpers(&cg);
    emit_cpu_topology_helper(&cg);
    emit_simd_detect_helper(&cg);
    emit_window_runtime(&cg, &ev_flags);
    emit_net_runtime(&cg);
    emit_server_runtime(&cg);
    emit_mem_runtime(&cg);
    emit_file_runtime(&cg);
    emit_audio_runtime(&cg);
    emit_crypto_runtime(&cg);
    emit_mat_runtime(&cg);
    emit_simd_runtime(&cg);
    emit_mesh_runtime(&cg);
    emit_tex_runtime(&cg);

    // User functions.
    for (int i = 0; i < prog->functions.count; i++) {
        emit_on_handlers(&cg, &prog->functions.items[i]);
        emit_function(&cg, &prog->functions.items[i]);
    }

    // Data sections (emitted after text so float/string pools are fully
    // populated by the time we write them).
    emit_data_section(&cg);
    emit_window_data(&cg);
    emit_mat_data(&cg);
    emit_mesh_data(&cg);
    emit_tex_data(&cg);
    emit_bss_section(&cg);
    emit_window_bss(&cg);
    emit_net_bss(&cg);
    emit_server_bss(&cg);
    emit_mem_bss(&cg);
    emit_file_bss(&cg);
    emit_audio_bss(&cg);
    emit_crypto_bss(&cg);
    emit_mat_bss(&cg);
    emit_simd_bss(&cg);
    emit_mesh_bss(&cg);
    emit_tex_bss(&cg);

    // Free string constant pool.
    for (int i = 0; i < cg.str_const_count; i++) {
        free(cg.str_consts[i]);
    }
}
