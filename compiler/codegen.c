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
#include "ast.h"
#include "codegen.h"

// ---------------------------------------------------------------------
// Codegen state
// ---------------------------------------------------------------------

// A local variable entry in the current function's symbol table.
typedef struct {
    char *name;
    SlagType type;
    int is_array;
    SlagType elem_type;   // valid if is_array
    int size;             // element count, valid if is_array
    int offset;           // negative offset from rbp, e.g. -8, -16, ...
} Local;

#define MAX_LOCALS 256

typedef struct {
    FILE *out;            // output assembly file
    int label_counter;    // unique label suffix generator

    // Current function state
    Local locals[MAX_LOCALS];
    int local_count;
    int frame_size;       // total bytes allocated for locals (multiple of 16)

    // Float literals need to be emitted in .data section; we collect them.
    double float_consts[1024];
    int float_const_count;

    // String literals emitted in .data section.
    char *str_consts[1024];
    int str_const_count;
} Codegen;

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

// Register a float constant in the .data pool; return its index.
static int add_float_const(Codegen *cg, double val) {
    for (int i = 0; i < cg->float_const_count; i++) {
        if (cg->float_consts[i] == val) return i;
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
    for (int i = 0; i < cg->local_count; i++) {
        if (strcmp(cg->locals[i].name, name) == 0) {
            return &cg->locals[i];
        }
    }
    return NULL;
}

// Allocate a new local. Returns its stack offset (negative from rbp).
static Local *alloc_local(Codegen *cg, const char *name, SlagType type,
                           int is_array, SlagType elem_type, int size) {
    if (cg->local_count >= MAX_LOCALS) {
        fprintf(stderr, "codegen error: too many locals\n");
        exit(1);
    }
    // Each local takes 8 bytes (int/float/bool/ptr all stored as qword).
    // Arrays take 8 * size bytes.
    int bytes = is_array ? (8 * size) : 8;
    cg->frame_size += bytes;

    Local *loc = &cg->locals[cg->local_count++];
    loc->name      = malloc(strlen(name) + 1);
    strcpy(loc->name, name);
    loc->type      = type;
    loc->is_array  = is_array;
    loc->elem_type = elem_type;
    loc->size      = size;
    loc->offset    = -(cg->frame_size); // grows downward
    return loc;
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
static void emit_str_bytes(Codegen *cg, const char *s) {
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
        case EXPR_IDENT:
        case EXPR_DOLLAR_IDENT: {
            Local *loc = find_local(cg, e->as.str.value);
            if (loc) return loc->type;
            return hint;
        }
        case EXPR_ARITH:
            return expr_type(cg, e->as.arith.inner, hint);
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
            return hint;
        }
        case EXPR_INDEX: {
            if (e->as.index.base->kind == EXPR_IDENT ||
                e->as.index.base->kind == EXPR_DOLLAR_IDENT) {
                Local *loc = find_local(cg, e->as.index.base->as.str.value);
                if (loc && loc->is_array) return loc->elem_type;
            }
            return hint;
        }
        default:
            return hint != TYPE_UNKNOWN ? hint : TYPE_INT;
    }
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

        case EXPR_IDENT:
        case EXPR_DOLLAR_IDENT: {
            Local *loc = find_local(cg, e->as.str.value);
            if (!loc) {
                fprintf(stderr, "codegen error: undefined variable '%s'\n", e->as.str.value);
                emit(cg, "    xor  rax, rax");
            } else {
                emit(cg, "    mov  rax, [rbp%+d]", loc->offset);
            }
            break;
        }

        case EXPR_ARITH:
            emit_int_expr(cg, e->as.arith.inner);
            break;

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
            (void)rt;

            if (lt == TYPE_FLOAT) {
                // Comparison between floats — result is int (0/1)
                emit_float_expr(cg, e->as.binary.left);
                PUSH_XMM0(cg);
                emit_float_expr(cg, e->as.binary.right);
                emit(cg, "    movsd xmm1, [rsp]");
                emit(cg, "    add  rsp, 8");
                // xmm1 = left, xmm0 = right  (we compare xmm1 op xmm0)
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
            if (e->as.index.base->kind == EXPR_IDENT ||
                e->as.index.base->kind == EXPR_DOLLAR_IDENT) {
                loc = find_local(cg, e->as.index.base->as.str.value);
            }
            if (!loc) {
                fprintf(stderr, "codegen error: array base not found\n");
                emit(cg, "    xor  rax, rax");
                break;
            }
            // Compute index into rcx
            emit_int_expr(cg, e->as.index.index);
            emit(cg, "    mov  rcx, rax");
            // Base address
            emit(cg, "    lea  rax, [rbp%+d]", loc->offset);
            emit(cg, "    mov  rax, [rax + rcx*8]");
            break;
        }

        case EXPR_MEMBER: {
            // e.g. tokens.len, cpu.logical_cores
            // For now handle .len on arrays and cpu.* fields.
            Local *loc = NULL;
            if (e->as.member.base->kind == EXPR_IDENT) {
                loc = find_local(cg, e->as.member.base->as.str.value);
            }
            if (loc && strcmp(e->as.member.member, "len") == 0) {
                emit(cg, "    mov  rax, %d", loc->size);
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
            // Built-in and user function calls that return int.
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

        case EXPR_IDENT:
        case EXPR_DOLLAR_IDENT: {
            Local *loc = find_local(cg, e->as.str.value);
            if (!loc) {
                fprintf(stderr, "codegen error: undefined variable '%s'\n", e->as.str.value);
                emit(cg, "    xorpd xmm0, xmm0");
            } else if (loc->type == TYPE_INT) {
                emit(cg, "    mov  rax, [rbp%+d]", loc->offset);
                emit(cg, "    cvtsi2sd xmm0, rax");
            } else {
                emit(cg, "    movsd xmm0, [rbp%+d]", loc->offset);
            }
            break;
        }

        case EXPR_ARITH:
            emit_float_expr(cg, e->as.arith.inner);
            break;

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
            if (e->as.index.base->kind == EXPR_IDENT ||
                e->as.index.base->kind == EXPR_DOLLAR_IDENT) {
                loc = find_local(cg, e->as.index.base->as.str.value);
            }
            if (!loc) {
                emit(cg, "    xorpd xmm0, xmm0");
                break;
            }
            emit_int_expr(cg, e->as.index.index);
            emit(cg, "    mov  rcx, rax");
            emit(cg, "    lea  rax, [rbp%+d]", loc->offset);
            emit(cg, "    movsd xmm0, [rax + rcx*8]");
            break;
        }

        case EXPR_CALL: {
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
    // written: we use a stack slot we already allocated or a temp.
    emit(cg, "    sub  rsp, 40          ; shadow + written slot");
    emit(cg, "    lea  r9,  [rsp+32]    ; &written");
    emit(cg, "    mov  qword [rsp+32], 0");
    emit(cg, "    mov  qword [rsp+0],  0 ; lpOverlapped = NULL (5th arg)");
    emit(cg, "    call WriteConsoleA");
    emit(cg, "    add  rsp, 40");
}

// Emit a print or println call.
// `is_println` appends a newline.
// The argument is the first (and only) entry in args.
static void emit_print(Codegen *cg, const ExprList *args, int is_println) {
    if (args->count < 1) return;
    const Expr *arg = args->items[0];
    SlagType t = expr_type(cg, arg, TYPE_STR);

    if (t == TYPE_STR) {
        // String literal: rdx = ptr, r8 = len
        if (arg->kind == EXPR_STR_LIT) {
            int idx = add_str_const(cg, arg->as.str.value);
            size_t slen = strlen(arg->as.str.value);
            emit(cg, "    ; print string literal");
            emit(cg, "    mov  rcx, [_stdout]");
            emit(cg, "    lea  rdx, [_str%d]", idx);
            emit(cg, "    mov  r8,  %zu", slen);
            emit_write_console(cg);
        } else {
            // Variable: rax = ptr, rdx = len (from emit_expr convention)
            emit_int_expr(cg, arg);  // rax = ptr
            emit(cg, "    mov  rdx, rax");
            // length: try .len member or fall back to a fixed sentinel
            // For now emit as: r8 = 0 (caller must use typed str with len)
            // TODO: proper str length tracking
            emit(cg, "    mov  r8,  0   ; TODO: str length");
            emit(cg, "    mov  rcx, [_stdout]");
            emit_write_console(cg);
        }
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
    // Push args right-to-left for any beyond the first 4.
    // For the first 4, load into rcx/rdx/r8/r9.
    // We evaluate all args and push them, then pop into registers.
    // Simple approach: evaluate each arg, push onto stack, then set up regs.

    int n = args->count;

    // Evaluate and push all args onto the stack (left to right).
    for (int i = 0; i < n; i++) {
        SlagType t = expr_type(cg, args->items[i], TYPE_INT);
        if (t == TYPE_FLOAT) {
            emit_float_expr(cg, args->items[i]);
            PUSH_XMM0(cg);
        } else {
            emit_int_expr(cg, args->items[i]);
            PUSH_RAX(cg);
        }
    }

    // Pop args into registers by position (Win64: arg N uses the Nth
    // slot - rcx/xmm0, rdx/xmm1, r8/xmm2, r9/xmm3 - selected by the
    // arg's type, both register files indexed by position).
    // Stack has arg(n-1) on top (pushed last), arg0 deepest.
    const char *int_regs[] = { "rcx", "rdx", "r8", "r9" };
    const char *xmm_regs[] = { "xmm0", "xmm1", "xmm2", "xmm3" };
    int reg_args = n < 4 ? n : 4;

    for (int i = reg_args - 1; i >= 0; i--) {
        SlagType t = expr_type(cg, args->items[i], TYPE_INT);
        if (t == TYPE_FLOAT) {
            emit(cg, "    movsd %s, [rsp]", xmm_regs[i]);
            emit(cg, "    add  rsp, 8");
        } else {
            emit(cg, "    pop  %s", int_regs[i]);
        }
    }

    emit_call_prologue(cg);
    emit(cg, "    call _%s", name);
    emit_call_epilogue(cg, 0);

    // Clean up any stack args beyond the first 4.
    if (n > 4) {
        emit(cg, "    add  rsp, %d", (n - 4) * 8);
    }
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
            emit(cg, "    call _slag_readline   ; rax = str ptr, rdx = len");
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
        } else if (strcmp(name, "pixel") == 0) {
            // pixel(x, y, r, g, b)
            emit(cg, "    ; pixel()");
            emit_user_call(cg, "slag_pixel", args);
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
        const ExprList *args = &e->as.member_call.args;

        // window.open(w, h, title)
        if (strcmp(member, "open") == 0) {
            emit(cg, "    ; window.open");
            emit_user_call(cg, "slag_window_open", args);
        }
        // window.close()
        else if (strcmp(member, "close") == 0) {
            emit(cg, "    ; window.close");
            emit_call_prologue(cg);
            emit(cg, "    call _slag_window_close");
            emit_call_epilogue(cg, 0);
        }
        // window.flush()
        else if (strcmp(member, "flush") == 0) {
            emit(cg, "    ; window.flush");
            emit_call_prologue(cg);
            emit(cg, "    call _slag_window_flush");
            emit_call_epilogue(cg, 0);
        }
        // zbuffer.clear()
        else if (strcmp(member, "clear") == 0) {
            emit(cg, "    ; zbuffer.clear");
            emit_call_prologue(cg);
            emit(cg, "    call _slag_zbuffer_clear");
            emit_call_epilogue(cg, 0);
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
        // var TYPE name = expr;
        // ------------------------------------------------------------------
        case STMT_VAR_DECL: {
            Local *loc = alloc_local(cg,
                s->as.var_decl.name,
                s->as.var_decl.type,
                0, TYPE_UNKNOWN, 0);

            SlagType t = s->as.var_decl.type;
            emit(cg, "    ; var %s %s", slag_type_name(t), s->as.var_decl.name);

            if (t == TYPE_FLOAT) {
                emit_float_expr(cg, s->as.var_decl.init);
                emit(cg, "    movsd [rbp%+d], xmm0", loc->offset);
            } else {
                emit_int_expr(cg, s->as.var_decl.init);
                emit(cg, "    mov  [rbp%+d], rax", loc->offset);
            }
            break;
        }

        // ------------------------------------------------------------------
        // var TYPE[n] name = { ... } or var TYPE[] name = call(...)
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

            if (target->kind == EXPR_IDENT ||
                target->kind == EXPR_DOLLAR_IDENT) {
                Local *loc = find_local(cg, target->as.str.value);
                if (!loc) {
                    fprintf(stderr, "codegen error: assign to undeclared '%s'\n",
                            target->as.str.value);
                    break;
                }
                emit(cg, "    ; assign %s", loc->name);
                if (loc->type == TYPE_FLOAT) {
                    emit_float_expr(cg, value);
                    emit(cg, "    movsd [rbp%+d], xmm0", loc->offset);
                } else {
                    emit_int_expr(cg, value);
                    emit(cg, "    mov  [rbp%+d], rax", loc->offset);
                }

            } else if (target->kind == EXPR_INDEX) {
                // array[index] = value
                Local *loc = NULL;
                if (target->as.index.base->kind == EXPR_IDENT ||
                    target->as.index.base->kind == EXPR_DOLLAR_IDENT) {
                    loc = find_local(cg, target->as.index.base->as.str.value);
                }
                if (!loc) {
                    fprintf(stderr, "codegen error: array assign to undeclared\n");
                    break;
                }
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
            // Each thread block gets its own labeled helper function that
            // we emit after the current function. For now we emit an inline
            // stub that calls CreateThread.
            int tid = new_label(cg);
            emit(cg, "    ; thread block %d (stub)", tid);
            emit(cg, "    ; TODO: emit thread proc and CreateThread call");
            break;
        }

        // ------------------------------------------------------------------
        // sync { ... } — wait for all threads (WaitForMultipleObjects)
        // ------------------------------------------------------------------
        case STMT_SYNC: {
            emit(cg, "    ; sync block");
            emit_stmtlist(cg, &s->as.sync_stmt.body);
            emit(cg, "    ; TODO: WaitForMultipleObjects");
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
            case STMT_VAR_DECL:
                size += 8;
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
            default:
                break;
        }
    }
    return size;
}

static void emit_function(Codegen *cg, const Function *f) {
    // Reset per-function state.
    cg->local_count = 0;
    cg->frame_size  = 0;

    // Pre-allocate parameter locals (they live in the frame too).
    // Win64: first 4 params arrive in rcx/rdx/r8/r9; we spill them.
    for (int i = 0; i < f->params.count; i++) {
        alloc_local(cg, f->params.items[i].name,
                    f->params.items[i].type, 0, TYPE_UNKNOWN, 0);
    }

    // Calculate body frame size and round up to 16-byte alignment.
    int body_size = calculate_frame_size(&f->body);
    int total = (cg->frame_size + body_size + 15) & ~15;
    // Ensure at least 32 bytes for shadow space during any nested calls.
    if (total < 32) total = 32;

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

    // Emit body.
    emit_stmtlist(cg, &f->body);

    // Epilogue (single exit point; all returns jump here).
    emit(cg, ".Lepilogue:");
    emit(cg, "    mov  rsp, rbp");
    emit(cg, "    pop  rbp");
    emit(cg, "    ret");
    emit(cg, "");
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
    emit(cg, "    sub  rsp, 64");
    emit(cg, "    mov  r8,  rcx           ; output buffer");
    emit(cg, "    ; integer part");
    emit(cg, "    cvttsd2si rax, xmm0");
    emit(cg, "    mov  rcx, rax");
    emit(cg, "    mov  rdx, r8");
    emit(cg, "    call _slag_itoa         ; rax = int part length");
    emit(cg, "    mov  r9,  rax           ; r9 = chars written so far");
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

    // String constants.
    for (int i = 0; i < cg->str_const_count; i++) {
        emit_raw(cg, "_str%d:  db ", i);
        // First byte placeholder — emit_str_bytes starts with ", x"
        // so we seed with a zero-length open.
        const char *s = cg->str_consts[i];
        if (*s == '\0') {
            emit_raw(cg, "0");
        } else {
            // Start in quote mode.
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
        }
        fprintf(cg->out, "\n");
    }
    if (cg->str_const_count) emit(cg, "");

    // Newline byte for println.
    emit(cg, "_newline: db 10");
    emit(cg, "");

    // stdout handle storage (populated at startup).
    emit(cg, "_stdout:  dq 0");
    emit(cg, "");

    // CPU topology globals (populated at startup).
    emit(cg, "_cpu_physical_cores:   dq 0");
    emit(cg, "_cpu_logical_cores:    dq 0");
    emit(cg, "_cpu_threads_per_core: dq 0");
    emit(cg, "_cpu_safe_thread_limit:dq 0");
    emit(cg, "");
}

// ---------------------------------------------------------------------
// .bss section
// ---------------------------------------------------------------------

static void emit_bss_section(Codegen *cg) {
    (void)cg;
    emit(cg, "section .bss");
    emit(cg, "_written_bytes: resq 1   ; scratch for WriteConsoleA");
    emit(cg, "");
}

// ---------------------------------------------------------------------
// Import table (.idata / extern declarations)
// ---------------------------------------------------------------------

static void emit_imports(Codegen *cg) {
    emit(cg, "; --- Win32 imports ---");
    emit(cg, "extern GetStdHandle");
    emit(cg, "extern WriteConsoleA");
    emit(cg, "extern ExitProcess");
    emit(cg, "extern CreateThread");
    emit(cg, "extern WaitForMultipleObjects");
    emit(cg, "extern GetLogicalProcessorInformation");
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
    emit(cg, "    ; TODO: GetLogicalProcessorInformation for CPU topology");
    emit(cg, "    mov  qword [_cpu_physical_cores],    1");
    emit(cg, "    mov  qword [_cpu_logical_cores],     1");
    emit(cg, "    mov  qword [_cpu_threads_per_core],  1");
    emit(cg, "    mov  qword [_cpu_safe_thread_limit], 1");
    emit(cg, "");
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

static void emit_entry(Codegen *cg) {
    emit(cg, "; --- entry point ---");
    emit(cg, "global _start");
    emit(cg, "_start:");
    emit(cg, "    sub  rsp, 8          ; align stack to 16 bytes");
    emit(cg, "    call _slag_startup");
    emit(cg, "    call _main");
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

    // .text section.
    emit(&cg, "section .text");
    emit(&cg, "");

    // Entry point.
    emit_entry(&cg);

    // Startup helper.
    emit_startup(&cg);

    // Runtime helpers.
    emit_runtime_helpers(&cg);

    // User functions.
    for (int i = 0; i < prog->functions.count; i++) {
        emit_function(&cg, &prog->functions.items[i]);
    }

    // Data sections (emitted after text so float/string pools are fully
    // populated by the time we write them).
    emit_data_section(&cg);
    emit_bss_section(&cg);

    // Free string constant pool.
    for (int i = 0; i < cg.str_const_count; i++) {
        free(cg.str_consts[i]);
    }
}
