// parser.c ??? Slag recursive descent parser
//
// Depends on: lexer.h, ast.h
//
// Public API (see parser.h):
//   Program parse_program(const char *src, size_t len);
//
// ---------------------------------------------------------------------
// Internal structure
// ---------------------------------------------------------------------
//
// The Parser struct wraps a Lexer and maintains a one-token lookahead
// (p->current). Helpers:
//
//   advance(p)          ??? consume current, load next token
//   check(p, type)      ??? true if current token is `type`
//   match(p, type)      ??? consume and return true if current == type
//   expect(p, type, msg)??? consume or emit error
//   error_at(p, msg)    ??? print error at current token position
//   copy_text(tok)      ??? heap-duplicate tok->text (caller owns)
//
// ---------------------------------------------------------------------
// Expression parsing
// ---------------------------------------------------------------------
//
// Expression grammar uses standard operator precedence.
// Arithmetic operators (+, -, *, /, %) work directly on expressions
// without requiring any special syntax wrapper.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lexer.h"
#include "ast.h"
#include "parser.h"

// ---------------------------------------------------------------------
// Parser state
// ---------------------------------------------------------------------

typedef struct {
    Lexer lexer;
    Token current;
    int had_error;
} Parser;

// ---------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------

static char *copy_text(const Token *tok) {
    size_t len = strlen(tok->text);
    char *s = malloc(len + 1);
    memcpy(s, tok->text, len + 1);
    return s;
}

static void error_at(Parser *p, const char *msg) {
    fprintf(stderr, "parse error at line %d col %d: %s (got '%s')\n",
            p->current.line, p->current.col, msg, p->current.text);
    p->had_error = 1;
}

static void advance(Parser *p) {
    token_free(&p->current);
    p->current = lexer_next(&p->lexer);
}

static int check(Parser *p, TokenType type) {
    return p->current.type == type;
}

static int match(Parser *p, TokenType type) {
    if (p->current.type == type) {
        advance(p);
        return 1;
    }
    return 0;
}

static void expect(Parser *p, TokenType type, const char *msg) {
    if (p->current.type == type) {
        advance(p);
    } else {
        error_at(p, msg);
    }
}

// Returns 1 if the current token is a type keyword.
static int is_type_keyword(Parser *p) {
    switch (p->current.type) {
        case TOK_KW_INT:
        case TOK_KW_FLOAT:
        case TOK_KW_STR:
        case TOK_KW_BOOL:
        case TOK_KW_VOID:
            return 1;
        default:
            return 0;
    }
}

static Expr *parse_expr(Parser *p);
static Stmt *parse_stmt(Parser *p);
static Stmt *parse_lock(Parser *p);
static void parse_block(Parser *p, StmtList *out);

// ---------------------------------------------------------------------
// Argument list
// ---------------------------------------------------------------------

// Parse a comma-separated argument list until a closing token is reached.
// Does not consume the closing token.
static void parse_arg_list(Parser *p, ExprList *out) {
    exprlist_init(out);
    if (check(p, TOK_RPAREN)) {
        return;
    }
    for (;;) {
        Expr *arg = parse_expr(p);
        exprlist_push(out, arg);
        if (!match(p, TOK_COMMA)) {
            break;
        }
    }
}

// ---------------------------------------------------------------------
// Postfix
// ---------------------------------------------------------------------

// Parse postfix operations: function calls, indexing, member access, and
// member calls (e.g. window.open(...), errors[i], errors.len).
static Expr *parse_postfix(Parser *p, Expr *base) {
    for (;;) {
        if (check(p, TOK_LBRACKET)) {
            int line = p->current.line, col = p->current.col;
            advance(p); // [
            Expr *index = parse_expr(p);
            expect(p, TOK_RBRACKET, "expected ']' after array index");

            Expr *e = expr_new(EXPR_INDEX, line, col);
            e->as.index.base = base;
            e->as.index.index = index;
            base = e;
            continue;
        }

        if (check(p, TOK_DOT)) {
            int line = p->current.line, col = p->current.col;
            advance(p); // .

            if (!check(p, TOK_IDENT) &&
                !(check(p, TOK_KW_WINDOW) || check(p, TOK_KW_PIXEL) ||
                  check(p, TOK_KW_FLUSH))) {
                error_at(p, "expected identifier after '.'");
                return base;
            }

            char *member = copy_text(&p->current);
            advance(p);

            if (check(p, TOK_LPAREN)) {
                advance(p); // (
                ExprList args;
                parse_arg_list(p, &args);
                expect(p, TOK_RPAREN, "expected ')' after arguments");

                Expr *e = expr_new(EXPR_MEMBER_CALL, line, col);
                e->as.member_call.base = base;
                e->as.member_call.member = member;
                e->as.member_call.args = args;
                base = e;
            } else {
                Expr *e = expr_new(EXPR_MEMBER, line, col);
                e->as.member.base = base;
                e->as.member.member = member;
                base = e;
            }
            continue;
        }

        break;
    }
    return base;
}

// ---------------------------------------------------------------------
// Primary
// ---------------------------------------------------------------------

// Parse a primary expression: literals, identifiers,
// parenthesized expressions, and function calls.
static Expr *parse_primary(Parser *p) {
    int line = p->current.line, col = p->current.col;

    if (check(p, TOK_INT_LIT)) {
        Expr *e = expr_new(EXPR_INT_LIT, line, col);
        e->as.int_val = p->current.int_val;
        advance(p);
        return parse_postfix(p, e);
    }

    if (check(p, TOK_FLOAT_LIT)) {
        Expr *e = expr_new(EXPR_FLOAT_LIT, line, col);
        e->as.float_val = p->current.float_val;
        advance(p);
        return parse_postfix(p, e);
    }

    if (check(p, TOK_STR_LIT)) {
        Expr *e = expr_new(EXPR_STR_LIT, line, col);
        e->as.str.value = copy_text(&p->current);
        advance(p);
        return parse_postfix(p, e);
    }

    if (check(p, TOK_REGEX_LIT)) {
        Expr *e = expr_new(EXPR_REGEX_LIT, line, col);
        e->as.str.value = copy_text(&p->current);
        advance(p);
        return e; // regex literals are not indexable/callable
    }

    if (check(p, TOK_KW_TRUE) || check(p, TOK_KW_FALSE)) {
        Expr *e = expr_new(EXPR_BOOL_LIT, line, col);
        e->as.bool_val = check(p, TOK_KW_TRUE) ? 1 : 0;
        advance(p);
        return parse_postfix(p, e);
    }

    // Parenthesized expression: ( expr )
    if (check(p, TOK_LPAREN)) {
        advance(p); // (
        Expr *inner = parse_expr(p);
        expect(p, TOK_RPAREN, "expected ')' after expression");
        return parse_postfix(p, inner);
    }

    // Bare identifier: variable reference or function call.
    // Also handle window/pixel/flush used as base expressions.
    if (check(p, TOK_IDENT) || check(p, TOK_KW_WINDOW) ||
        check(p, TOK_KW_PIXEL) || check(p, TOK_KW_FLUSH)) {

        char *name = copy_text(&p->current);
        advance(p);

        if (check(p, TOK_LPAREN)) {
            advance(p); // (
            ExprList args;
            parse_arg_list(p, &args);
            expect(p, TOK_RPAREN, "expected ')' after arguments");

            Expr *e = expr_new(EXPR_CALL, line, col);
            e->as.call.name = name;
            e->as.call.args = args;
            return parse_postfix(p, e);
        }

        Expr *e = expr_new(EXPR_IDENT, line, col);
        e->as.str.value = name;
        return parse_postfix(p, e);
    }

    error_at(p, "expected expression");
    // Error recovery: return a dummy node and try to advance.
    Expr *e = expr_new(EXPR_INT_LIT, line, col);
    e->as.int_val = 0;
    if (!check(p, TOK_EOF)) advance(p);
    return e;
}

// ---------------------------------------------------------------------
// Precedence chain
// ---------------------------------------------------------------------

// Unary: !expr, -expr
static Expr *parse_unary(Parser *p) {
    if (check(p, TOK_NOT) || check(p, TOK_MINUS)) {
        int line = p->current.line, col = p->current.col;
        TokenType op = p->current.type;
        advance(p);
        Expr *operand = parse_unary(p);

        Expr *e = expr_new(EXPR_UNARY, line, col);
        e->as.unary.op = op;
        e->as.unary.operand = operand;
        return e;
    }
    return parse_primary(p);
}

// Multiplicative: * / %
static Expr *parse_multiplicative(Parser *p) {
    Expr *left = parse_unary(p);
    while (check(p, TOK_STAR) || check(p, TOK_SLASH) || check(p, TOK_PERCENT)) {
        int line = p->current.line, col = p->current.col;
        TokenType op = p->current.type;
        advance(p);
        Expr *right = parse_unary(p);

        Expr *e = expr_new(EXPR_BINARY, line, col);
        e->as.binary.op = op;
        e->as.binary.left = left;
        e->as.binary.right = right;
        left = e;
    }
    return left;
}

// Additive: + -
static Expr *parse_additive(Parser *p) {
    Expr *left = parse_multiplicative(p);
    while (check(p, TOK_PLUS) || check(p, TOK_MINUS)) {
        int line = p->current.line, col = p->current.col;
        TokenType op = p->current.type;
        advance(p);
        Expr *right = parse_multiplicative(p);

        Expr *e = expr_new(EXPR_BINARY, line, col);
        e->as.binary.op = op;
        e->as.binary.left = left;
        e->as.binary.right = right;
        left = e;
    }
    return left;
}

// Relational: < > <= >=
static Expr *parse_relational(Parser *p) {
    Expr *left = parse_additive(p);
    while (check(p, TOK_LT) || check(p, TOK_GT) ||
           check(p, TOK_LE) || check(p, TOK_GE)) {
        int line = p->current.line, col = p->current.col;
        TokenType op = p->current.type;
        advance(p);
        Expr *right = parse_additive(p);

        Expr *e = expr_new(EXPR_BINARY, line, col);
        e->as.binary.op = op;
        e->as.binary.left = left;
        e->as.binary.right = right;
        left = e;
    }
    return left;
}

// Equality: == !=
static Expr *parse_equality(Parser *p) {
    Expr *left = parse_relational(p);
    while (check(p, TOK_EQ) || check(p, TOK_NEQ)) {
        int line = p->current.line, col = p->current.col;
        TokenType op = p->current.type;
        advance(p);
        Expr *right = parse_relational(p);

        Expr *e = expr_new(EXPR_BINARY, line, col);
        e->as.binary.op = op;
        e->as.binary.left = left;
        e->as.binary.right = right;
        left = e;
    }
    return left;
}

// Logical AND: &&
static Expr *parse_logical_and(Parser *p) {
    Expr *left = parse_equality(p);
    while (check(p, TOK_AND)) {
        int line = p->current.line, col = p->current.col;
        advance(p);
        Expr *right = parse_equality(p);

        Expr *e = expr_new(EXPR_LOGICAL, line, col);
        e->as.logical.op = TOK_AND;
        e->as.logical.left = left;
        e->as.logical.right = right;
        left = e;
    }
    return left;
}

// Logical XOR: ^^  (precedence between || and &&, like C's ^ )
static Expr *parse_logical_xor(Parser *p) {
    Expr *left = parse_logical_and(p);
    while (check(p, TOK_XOR)) {
        int line = p->current.line, col = p->current.col;
        advance(p);
        Expr *right = parse_logical_and(p);

        Expr *e = expr_new(EXPR_LOGICAL, line, col);
        e->as.logical.op = TOK_XOR;
        e->as.logical.left = left;
        e->as.logical.right = right;
        left = e;
    }
    return left;
}

// Logical OR: ||
static Expr *parse_logical_or(Parser *p) {
    Expr *left = parse_logical_xor(p);
    while (check(p, TOK_OR)) {
        int line = p->current.line, col = p->current.col;
        advance(p);
        Expr *right = parse_logical_xor(p);

        Expr *e = expr_new(EXPR_LOGICAL, line, col);
        e->as.logical.op = TOK_OR;
        e->as.logical.left = left;
        e->as.logical.right = right;
        left = e;
    }
    return left;
}

// Top-level expression entry point.
static Expr *parse_expr(Parser *p) {
    return parse_logical_or(p);
}

// ---------------------------------------------------------------------
// Statement parsing
// ---------------------------------------------------------------------

// Parse the body of a block: zero or more statements until '}'.
// Does not consume the closing '}'.
static void parse_block(Parser *p, StmtList *out) {
    stmtlist_init(out);
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        Stmt *s = parse_stmt(p);
        if (s) stmtlist_push(out, s);
    }
}

// ---------------------------------------------------------------------
// global declaration
//
// global TYPE name = expr;
// ---------------------------------------------------------------------
static Stmt *parse_global_decl(Parser *p) {
    int line = p->current.line, col = p->current.col;
    advance(p); // consume 'global'

    if (!is_type_keyword(p)) {
        error_at(p, "expected type keyword after 'global'");
        return NULL;
    }

    SlagType base_type = slag_type_from_token(p->current.type);
    advance(p); // consume type keyword

    // Global array declaration: global TYPE[n] name = { ... }
    if (check(p, TOK_LBRACKET)) {
        advance(p); // [

        Expr *size_expr = NULL;
        if (!check(p, TOK_RBRACKET)) {
            size_expr = parse_expr(p);
        }
        expect(p, TOK_RBRACKET, "expected ']' after array size");

        if (!check(p, TOK_IDENT)) {
            error_at(p, "expected array name");
            return NULL;
        }
        char *name = copy_text(&p->current);
        advance(p);

        expect(p, TOK_ASSIGN, "expected '=' in global array declaration");

        Stmt *s = stmt_new(STMT_ARRAY_DECL, line, col);
        s->as.array_decl.elem_type = base_type;
        s->as.array_decl.name = name;
        s->as.array_decl.size_expr = size_expr;
        exprlist_init(&s->as.array_decl.init_list);
        s->as.array_decl.init_call = NULL;
        s->as.array_decl.is_global = 1;

        if (check(p, TOK_LBRACE)) {
            advance(p); // {
            if (!check(p, TOK_RBRACE)) {
                for (;;) {
                    Expr *elem = parse_expr(p);
                    exprlist_push(&s->as.array_decl.init_list, elem);
                    if (!match(p, TOK_COMMA)) break;
                    if (check(p, TOK_RBRACE)) break;
                }
            }
            expect(p, TOK_RBRACE, "expected '}' after array initializer");
        } else {
            s->as.array_decl.init_call = parse_expr(p);
        }

        expect(p, TOK_SEMICOLON, "expected ';' after global array declaration");
        return s;
    }

    // Global scalar declaration
    if (!check(p, TOK_IDENT)) {
        error_at(p, "expected variable name");
        return NULL;
    }
    char *name = copy_text(&p->current);
    advance(p);

    expect(p, TOK_ASSIGN, "expected '=' in global declaration");

    Expr *init = parse_expr(p);
    expect(p, TOK_SEMICOLON, "expected ';' after global declaration");

    Stmt *s = stmt_new(STMT_GLOBAL_DECL, line, col);
    s->as.var_decl.type = base_type;
    s->as.var_decl.name = name;
    s->as.var_decl.init = init;
    return s;
}

// ---------------------------------------------------------------------
// local declaration
//
// local TYPE name = expr;
// ---------------------------------------------------------------------
static Stmt *parse_local_decl(Parser *p) {
    int line = p->current.line, col = p->current.col;
    advance(p); // consume 'local'

    if (!is_type_keyword(p)) {
        error_at(p, "expected type keyword after 'local'");
        return NULL;
    }

    SlagType base_type = slag_type_from_token(p->current.type);
    advance(p); // consume type keyword

    // Local array declaration: local TYPE[] name = { ... }
    if (check(p, TOK_LBRACKET)) {
        advance(p); // [

        Expr *size_expr = NULL;
        if (!check(p, TOK_RBRACKET)) {
            size_expr = parse_expr(p);
        }
        expect(p, TOK_RBRACKET, "expected ']' after array size");

        if (!check(p, TOK_IDENT)) {
            error_at(p, "expected array name");
            return NULL;
        }
        char *name = copy_text(&p->current);
        advance(p);

        expect(p, TOK_ASSIGN, "expected '=' in local array declaration");

        Stmt *s = stmt_new(STMT_ARRAY_DECL, line, col);
        s->as.array_decl.elem_type = base_type;
        s->as.array_decl.name = name;
        s->as.array_decl.size_expr = size_expr;
        exprlist_init(&s->as.array_decl.init_list);
        s->as.array_decl.init_call = NULL;
        s->as.array_decl.is_global = 0;

        if (check(p, TOK_LBRACE)) {
            advance(p); // {
            if (!check(p, TOK_RBRACE)) {
                for (;;) {
                    Expr *elem = parse_expr(p);
                    exprlist_push(&s->as.array_decl.init_list, elem);
                    if (!match(p, TOK_COMMA)) break;
                    if (check(p, TOK_RBRACE)) break;
                }
            }
            expect(p, TOK_RBRACE, "expected '}' after array initializer");
        } else {
            s->as.array_decl.init_call = parse_expr(p);
        }

        expect(p, TOK_SEMICOLON, "expected ';' after local array declaration");
        return s;
    }

    if (!check(p, TOK_IDENT)) {
        error_at(p, "expected variable name");
        return NULL;
    }
    char *name = copy_text(&p->current);
    advance(p);

    expect(p, TOK_ASSIGN, "expected '=' in local declaration");

    Expr *init = parse_expr(p);
    expect(p, TOK_SEMICOLON, "expected ';' after local declaration");

    Stmt *s = stmt_new(STMT_LOCAL_DECL, line, col);
    s->as.var_decl.type = base_type;
    s->as.var_decl.name = name;
    s->as.var_decl.init = init;
    return s;
}

// ---------------------------------------------------------------------
// Assignment or expression statement
//
// After seeing an identifier (or window/pixel), we may have:
//   name = expr;           STMT_ASSIGN  (plain variable)
//   name[idx] = expr;      STMT_ASSIGN  (array element)
//   name(args);            STMT_EXPR    (function call)
//   name.member(args);     STMT_EXPR    (member call)
//
// We parse an expression starting from the identifier and then decide:
// if the result is followed by '=' it's an assignment, otherwise it's
// an expression statement.
// ---------------------------------------------------------------------
static Stmt *parse_assign_or_expr(Parser *p) {
    int line = p->current.line, col = p->current.col;

    Expr *lhs = parse_expr(p);

    // Post-increment / -decrement statement: `name++;` / `name--;`
    // Desugars to `name = name +/- 1;`. Only a plain identifier target is
    // supported (matches C's common counter idiom).
    if (check(p, TOK_INC) || check(p, TOK_DEC)) {
        int is_inc = check(p, TOK_INC);
        advance(p); // ++ or --
        expect(p, TOK_SEMICOLON, "expected ';' after '++' / '--'");
        if (lhs->kind != EXPR_IDENT) {
            fprintf(stderr, "parse error at line %d: '++'/'--' requires a simple variable\n", line);
            p->had_error = 1;
        }
        Expr *cur = expr_new(EXPR_IDENT, line, col);
        cur->as.str.value = lhs->kind == EXPR_IDENT ? ast_strdup(lhs->as.str.value) : NULL;
        Expr *one = expr_new(EXPR_INT_LIT, line, col);
        one->as.int_val = 1;
        Expr *bin = expr_new(EXPR_BINARY, line, col);
        bin->as.binary.op = is_inc ? TOK_PLUS : TOK_MINUS;
        bin->as.binary.left = cur;
        bin->as.binary.right = one;
        Stmt *s = stmt_new(STMT_ASSIGN, line, col);
        s->as.assign.target = lhs;
        s->as.assign.value = bin;
        return s;
    }

    if (check(p, TOK_ASSIGN)) {
        advance(p); // =
        Expr *rhs = parse_expr(p);
        expect(p, TOK_SEMICOLON, "expected ';' after assignment");

        // Validate LHS: must be an ident, index, or member.
        if (lhs->kind != EXPR_IDENT && lhs->kind != EXPR_INDEX &&
            lhs->kind != EXPR_MEMBER) {
            fprintf(stderr, "parse error at line %d: invalid assignment target\n", line);
            p->had_error = 1;
        }

        Stmt *s = stmt_new(STMT_ASSIGN, line, col);
        s->as.assign.target = lhs;
        s->as.assign.value = rhs;
        return s;
    }

    // Expression statement (e.g. a function call used for its side effects).
    expect(p, TOK_SEMICOLON, "expected ';' after expression statement");

    Stmt *s = stmt_new(STMT_EXPR, line, col);
    s->as.expr_stmt.expr = lhs;
    return s;
}

// ---------------------------------------------------------------------
// if statement
//
// if (cond) { ... }
// if (cond) { ... } else { ... }
// if (cond) { ... } else if (cond) { ... }    <- desugars to else { if ... }
// ---------------------------------------------------------------------
static Stmt *parse_if(Parser *p) {
    int line = p->current.line, col = p->current.col;
    advance(p); // consume 'if'

    expect(p, TOK_LPAREN, "expected '(' after 'if'");
    Expr *cond = parse_expr(p);
    expect(p, TOK_RPAREN, "expected ')' after if condition");

    expect(p, TOK_LBRACE, "expected '{' to open if body");
    StmtList then_body;
    parse_block(p, &then_body);
    expect(p, TOK_RBRACE, "expected '}' to close if body");

    Stmt *s = stmt_new(STMT_IF, line, col);
    s->as.if_stmt.cond = cond;
    s->as.if_stmt.then_body = then_body;
    stmtlist_init(&s->as.if_stmt.else_body);
    s->as.if_stmt.has_else = 0;

    if (match(p, TOK_KW_ELSE)) {
        s->as.if_stmt.has_else = 1;

        if (check(p, TOK_KW_IF)) {
            // else if: wrap the inner if as a single stmt in the else body.
            Stmt *inner = parse_if(p);
            stmtlist_push(&s->as.if_stmt.else_body, inner);
        } else {
            expect(p, TOK_LBRACE, "expected '{' to open else body");
            parse_block(p, &s->as.if_stmt.else_body);
            expect(p, TOK_RBRACE, "expected '}' to close else body");
        }
    }

    return s;
}

// ---------------------------------------------------------------------
// for statement  (three surface forms, all desugared to existing nodes)
//
//   C-style:     for (init ; cond ; post) { body }
//   Range-decl:  for (local TYPE i = a ; cond) { body }   (no post clause)
//   Count:       for i in a .. b { body }
//
// Each lowers to a STMT_BLOCK wrapping an optional init/decl followed by a
// STMT_WHILE. STMT_BLOCK codegen is a bare emit_stmtlist (no scope push,
// locals are function-flat), so the emitted asm is byte-for-byte what a
// hand-written `while`-with-counter produces ??? no new codegen, no runtime
// cost.
// ---------------------------------------------------------------------

// Parse the init clause of a paren-form for, WITHOUT consuming the trailing
// ';'. Accepts a local decl (local TYPE name = expr) or a plain assignment
// (name = expr), mirroring parse_local_decl / parse_assign_or_expr.
static Stmt *parse_for_init(Parser *p) {
    int line = p->current.line, col = p->current.col;

    // A for-init that begins with `local` or directly with a type keyword is
    // a declaration; the `local` keyword is optional here (for-scoped).
    if (check(p, TOK_KW_LOCAL) || is_type_keyword(p)) {
        if (check(p, TOK_KW_LOCAL)) advance(p); // optional 'local'
        if (!is_type_keyword(p)) {
            error_at(p, "expected type keyword in for-init declaration");
            return NULL;
        }
        SlagType t = slag_type_from_token(p->current.type);
        advance(p);
        if (!check(p, TOK_IDENT)) {
            error_at(p, "expected variable name in for-init");
            return NULL;
        }
        char *name = copy_text(&p->current);
        advance(p);
        expect(p, TOK_ASSIGN, "expected '=' in for-init declaration");
        Stmt *s = stmt_new(STMT_LOCAL_DECL, line, col);
        s->as.var_decl.type = t;
        s->as.var_decl.name = name;
        s->as.var_decl.init = parse_expr(p);
        return s;
    }

    Expr *lhs = parse_expr(p);
    expect(p, TOK_ASSIGN, "expected '=' in for-init assignment");
    Expr *rhs = parse_expr(p);
    if (lhs->kind != EXPR_IDENT && lhs->kind != EXPR_INDEX &&
        lhs->kind != EXPR_MEMBER) {
        error_at(p, "invalid for-init assignment target");
    }
    Stmt *s = stmt_new(STMT_ASSIGN, line, col);
    s->as.assign.target = lhs;
    s->as.assign.value = rhs;
    return s;
}

// Desugar `IDENT++` / `IDENT--` into `IDENT = IDENT +/- 1`. `name` is the
// (already parsed) target token; consumes the TOK_INC/TOK_DEC.
static Stmt *make_incdec(Parser *p, char *name, int is_inc, int line, int col) {
    advance(p); // consume ++ or --
    Expr *tgt = expr_new(EXPR_IDENT, line, col);
    tgt->as.str.value = name;              // takes ownership of the dup'd name
    Expr *cur = expr_new(EXPR_IDENT, line, col);
    cur->as.str.value = ast_strdup(name);
    Expr *one = expr_new(EXPR_INT_LIT, line, col);
    one->as.int_val = 1;
    Expr *bin = expr_new(EXPR_BINARY, line, col);
    bin->as.binary.op = is_inc ? TOK_PLUS : TOK_MINUS;
    bin->as.binary.left = cur;
    bin->as.binary.right = one;
    Stmt *s = stmt_new(STMT_ASSIGN, line, col);
    s->as.assign.target = tgt;
    s->as.assign.value = bin;
    return s;
}

// Parse the post clause WITHOUT consuming the trailing ')'. Accepts either an
// assignment (name = expr) or a post-increment/decrement (name++ / name--).
static Stmt *parse_for_post(Parser *p) {
    int line = p->current.line, col = p->current.col;

    // Fast path: bare `IDENT++` / `IDENT--`. Duplicate the identifier text
    // BEFORE advancing ? advance() frees the current token's storage.
    if (check(p, TOK_IDENT)) {
        char *name = copy_text(&p->current);
        advance(p); // IDENT
        if (check(p, TOK_INC)) return make_incdec(p, name, 1, line, col);
        if (check(p, TOK_DEC)) return make_incdec(p, name, 0, line, col);
        // Not ++/-- : it was a plain assignment target; finish it below.
        Expr *lhs = expr_new(EXPR_IDENT, line, col);
        lhs->as.str.value = name;              // takes ownership of the dup
        lhs = parse_postfix(p, lhs); // allow name[idx] / name.member
        expect(p, TOK_ASSIGN, "expected '=', '++' or '--' in for-post clause");
        Expr *rhs = parse_expr(p);
        if (lhs->kind != EXPR_IDENT && lhs->kind != EXPR_INDEX &&
            lhs->kind != EXPR_MEMBER) {
            error_at(p, "invalid for-post assignment target");
        }
        Stmt *s = stmt_new(STMT_ASSIGN, line, col);
        s->as.assign.target = lhs;
        s->as.assign.value = rhs;
        return s;
    }

    Expr *lhs = parse_expr(p);
    expect(p, TOK_ASSIGN, "expected '=' in for-post assignment");
    Expr *rhs = parse_expr(p);
    if (lhs->kind != EXPR_IDENT && lhs->kind != EXPR_INDEX &&
        lhs->kind != EXPR_MEMBER) {
        error_at(p, "invalid for-post assignment target");
    }
    Stmt *s = stmt_new(STMT_ASSIGN, line, col);
    s->as.assign.target = lhs;
    s->as.assign.value = rhs;
    return s;
}

// Wrap [init] + while(cond){body} in a STMT_BLOCK and return it.
static Stmt *for_wrap(Stmt *init, Expr *cond, StmtList body, int line, int col) {
    // An omitted condition is an always-true loop: synthesize the constant 1
    // so STMT_WHILE codegen tests a nonzero value each iteration (ISO C rule).
    if (!cond) {
        cond = expr_new(EXPR_INT_LIT, line, col);
        cond->as.int_val = 1;
    }
    Stmt *wh = stmt_new(STMT_WHILE, line, col);
    wh->as.while_stmt.cond = cond;
    wh->as.while_stmt.body = body;

    Stmt *blk = stmt_new(STMT_BLOCK, line, col);
    stmtlist_init(&blk->as.block.body);
    if (init) stmtlist_push(&blk->as.block.body, init);
    stmtlist_push(&blk->as.block.body, wh);
    return blk;
}

static Stmt *parse_for(Parser *p) {
    int line = p->current.line, col = p->current.col;
    advance(p); // consume 'for'

    // ---- Count form:  for IDENT in A .. B { body } ----
    if (check(p, TOK_IDENT)) {
        // Duplicate the loop-variable name BEFORE advancing ? advance() frees
        // the current token's storage. `var` is heap-owned for the rest of
        // this function and consumed by the decl below.
        char *var = copy_text(&p->current);
        advance(p);                      // IDENT
        expect(p, TOK_KW_IN, "expected 'in' after loop variable in count-for");
        Expr *lo = parse_expr(p);
        expect(p, TOK_DOTDOT, "expected '..' in count-for range");
        Expr *hi = parse_expr(p);

        expect(p, TOK_LBRACE, "expected '{' to open for body");
        StmtList body;
        parse_block(p, &body);
        expect(p, TOK_RBRACE, "expected '}' to close for body");

        // Desugar: { local int V = lo; while (V < hi) { body...; V = V + 1; } }
        Stmt *decl = stmt_new(STMT_LOCAL_DECL, line, col);
        decl->as.var_decl.type = TYPE_INT;
        decl->as.var_decl.name = var;              // takes ownership
        decl->as.var_decl.init = lo;

        Expr *lhsv = expr_new(EXPR_IDENT, line, col);
        lhsv->as.str.value = ast_strdup(var);
        Expr *cond = expr_new(EXPR_BINARY, line, col);
        cond->as.binary.op = TOK_LT;
        cond->as.binary.left = lhsv;
        cond->as.binary.right = hi;

        Expr *incl = expr_new(EXPR_IDENT, line, col);
        incl->as.str.value = ast_strdup(var);
        Expr *one = expr_new(EXPR_INT_LIT, line, col);
        one->as.int_val = 1;
        Expr *sum = expr_new(EXPR_BINARY, line, col);
        sum->as.binary.op = TOK_PLUS;
        sum->as.binary.left = incl;
        sum->as.binary.right = one;
        Expr *tgt = expr_new(EXPR_IDENT, line, col);
        tgt->as.str.value = ast_strdup(var);
        Stmt *inc = stmt_new(STMT_ASSIGN, line, col);
        inc->as.assign.target = tgt;
        inc->as.assign.value = sum;
        stmtlist_push(&body, inc);

        return for_wrap(decl, cond, body, line, col);
    }

    // ---- Paren forms: C-style and range-decl ----
    expect(p, TOK_LPAREN, "expected '(' or loop variable after 'for'");

    // Each clause is independently optional (ISO C: `for (clause1opt ; e2opt ; e3opt)`).
    // clause-1 (init) empty  -> no init emitted.
    // e2 (condition) empty   -> always-true (unconditional loop).
    // e3 (post) empty        -> no step emitted.
    // A one-semicolon form `for (init ; cond)` (range-decl) is still accepted:
    // post simply stays NULL.
    Stmt *init = NULL;
    if (!check(p, TOK_SEMICOLON)) {
        init = parse_for_init(p);
    }
    expect(p, TOK_SEMICOLON, "expected ';' after for-init clause");

    Expr *cond = NULL;                    // NULL condition == always true
    if (!check(p, TOK_SEMICOLON) && !check(p, TOK_RPAREN)) {
        cond = parse_expr(p);
    }

    Stmt *post = NULL;
    if (check(p, TOK_SEMICOLON)) {
        advance(p);                      // second ';' -> post clause present/optional
        if (!check(p, TOK_RPAREN)) {
            post = parse_for_post(p);
        }
    }
    // else: one-semicolon (range-decl) form; no post clause.
    expect(p, TOK_RPAREN, "expected ')' after for clauses");

    expect(p, TOK_LBRACE, "expected '{' to open for body");
    StmtList body;
    parse_block(p, &body);
    expect(p, TOK_RBRACE, "expected '}' to close for body");

    if (post) stmtlist_push(&body, post);

    return for_wrap(init, cond, body, line, col);
}

// ---------------------------------------------------------------------
// while statement
// ---------------------------------------------------------------------
static Stmt *parse_while(Parser *p) {
    int line = p->current.line, col = p->current.col;
    advance(p); // consume 'while'

    expect(p, TOK_LPAREN, "expected '(' after 'while'");
    Expr *cond = parse_expr(p);
    expect(p, TOK_RPAREN, "expected ')' after while condition");

    expect(p, TOK_LBRACE, "expected '{' to open while body");
    StmtList body;
    parse_block(p, &body);
    expect(p, TOK_RBRACE, "expected '}' to close while body");

    Stmt *s = stmt_new(STMT_WHILE, line, col);
    s->as.while_stmt.cond = cond;
    s->as.while_stmt.body = body;
    return s;
}

// ---------------------------------------------------------------------
// return statement
//
// return;              ??? void return
// return TYPE expr;    ??? typed return
// ---------------------------------------------------------------------
static Stmt *parse_return(Parser *p) {
    int line = p->current.line, col = p->current.col;
    advance(p); // consume 'return'

    Stmt *s = stmt_new(STMT_RETURN, line, col);

    if (check(p, TOK_SEMICOLON)) {
        // Bare return.
        advance(p);
        s->as.return_stmt.is_void = 1;
        s->as.return_stmt.type = TYPE_VOID;
        s->as.return_stmt.value = NULL;
        return s;
    }

    // Typed return: return TYPE expr;
    if (is_type_keyword(p)) {
        s->as.return_stmt.type = slag_type_from_token(p->current.type);
        advance(p); // consume type keyword
    } else {
        // No type keyword ??? treat as void return with a warning.
        fprintf(stderr, "warning: line %d: 'return' missing type keyword, treating as void\n", line);
        s->as.return_stmt.is_void = 1;
        s->as.return_stmt.type = TYPE_VOID;
        s->as.return_stmt.value = NULL;
        expect(p, TOK_SEMICOLON, "expected ';' after return");
        return s;
    }

    s->as.return_stmt.is_void = 0;
    s->as.return_stmt.value = parse_expr(p);
    expect(p, TOK_SEMICOLON, "expected ';' after return expression");
    return s;
}

// ---------------------------------------------------------------------
// thread block
// ---------------------------------------------------------------------
static Stmt *parse_thread(Parser *p) {
    int line = p->current.line, col = p->current.col;
    advance(p); // consume 'thread'

    expect(p, TOK_LBRACE, "expected '{' to open thread block");
    StmtList body;
    parse_block(p, &body);
    expect(p, TOK_RBRACE, "expected '}' to close thread block");

    Stmt *s = stmt_new(STMT_THREAD, line, col);
    s->as.thread_stmt.body = body;
    return s;
}

// ---------------------------------------------------------------------
// sync block
// ---------------------------------------------------------------------
static Stmt *parse_sync(Parser *p) {
    int line = p->current.line, col = p->current.col;
    advance(p); // consume 'sync'

    expect(p, TOK_LBRACE, "expected '{' to open sync block");
    StmtList body;
    parse_block(p, &body);
    expect(p, TOK_RBRACE, "expected '}' to close sync block");

    Stmt *s = stmt_new(STMT_SYNC, line, col);
    s->as.sync_stmt.body = body;
    return s;
}

static Stmt *parse_lock(Parser *p) {
    int line = p->current.line, col = p->current.col;
    advance(p); // consume 'lock'

    expect(p, TOK_LBRACE, "expected '{' to open lock block");
    StmtList body;
    parse_block(p, &body);
    expect(p, TOK_RBRACE, "expected '}' to close lock block");

    Stmt *s = stmt_new(STMT_LOCK, line, col);
    s->as.lock_stmt.body = body;
    return s;
}

// ---------------------------------------------------------------------
// on handler
//
// on key_down(int k) { ... }
// on mouse_move(int x, int y) { ... }
// ---------------------------------------------------------------------
static Stmt *parse_on_handler(Parser *p) {
    int line = p->current.line, col = p->current.col;
    advance(p); // consume 'on'

    if (!check(p, TOK_IDENT)) {
        error_at(p, "expected event name after 'on'");
        return NULL;
    }
    char *event_name = copy_text(&p->current);
    advance(p);

    expect(p, TOK_LPAREN, "expected '(' after event name");

    ParamList params;
    paramlist_init(&params);

    if (!check(p, TOK_RPAREN)) {
        for (;;) {
            if (!is_type_keyword(p)) {
                error_at(p, "expected type keyword in event handler parameter");
                break;
            }
            SlagType ptype = slag_type_from_token(p->current.type);
            advance(p);

            if (!check(p, TOK_IDENT)) {
                error_at(p, "expected parameter name");
                break;
            }
            Param param;
            param.type = ptype;
            param.name = copy_text(&p->current);
            advance(p);
            paramlist_push(&params, param);

            if (!match(p, TOK_COMMA)) break;
        }
    }

    expect(p, TOK_RPAREN, "expected ')' after event handler parameters");
    expect(p, TOK_LBRACE, "expected '{' to open event handler body");

    StmtList body;
    parse_block(p, &body);
    expect(p, TOK_RBRACE, "expected '}' to close event handler body");

    Stmt *s = stmt_new(STMT_ON_HANDLER, line, col);
    s->as.on_handler.event_name = event_name;
    s->as.on_handler.params = params;
    s->as.on_handler.body = body;
    return s;
}

// ---------------------------------------------------------------------
// Statement dispatcher
// ---------------------------------------------------------------------
static Stmt *parse_stmt(Parser *p) {
    switch (p->current.type) {
        case TOK_KW_GLOBAL: return parse_global_decl(p);
        case TOK_KW_LOCAL:  return parse_local_decl(p);
        case TOK_KW_IF:     return parse_if(p);
        case TOK_KW_FOR:    return parse_for(p);
        case TOK_KW_WHILE:  return parse_while(p);
        case TOK_KW_RETURN: return parse_return(p);
        case TOK_KW_THREAD: return parse_thread(p);
        case TOK_KW_SYNC:   return parse_sync(p);
        case TOK_KW_LOCK:   return parse_lock(p);
        case TOK_KW_ON:     return parse_on_handler(p);

        case TOK_LBRACE: {
            // Anonymous block ??? rare but legal.
            int line = p->current.line, col = p->current.col;
            advance(p);
            StmtList body;
            parse_block(p, &body);
            expect(p, TOK_RBRACE, "expected '}' to close block");
            Stmt *s = stmt_new(STMT_BLOCK, line, col);
            s->as.block.body = body;
            return s;
        }

        case TOK_SEMICOLON:
            // Empty statement ??? skip silently.
            advance(p);
            return NULL;

        // Identifiers and window/pixel/flush can start assignments or
        // expression statements.
        case TOK_IDENT:
        case TOK_KW_WINDOW:
        case TOK_KW_PIXEL:
        case TOK_KW_FLUSH:
        case TOK_INT_LIT:
        case TOK_FLOAT_LIT:
        case TOK_STR_LIT:
        case TOK_KW_TRUE:
        case TOK_KW_FALSE:
            return parse_assign_or_expr(p);

        default:
            error_at(p, "unexpected token at statement level");
            advance(p); // skip and try to recover
            return NULL;
    }
}

// ---------------------------------------------------------------------
// Function declaration
//
// function name(TYPE param, ...) { ... }
// ---------------------------------------------------------------------
static int parse_function(Parser *p, Function *out) {
    out->line = p->current.line;
    out->col  = p->current.col;
    advance(p); // consume 'function'

    if (!check(p, TOK_IDENT)) {
        error_at(p, "expected function name");
        return 0;
    }
    out->name = copy_text(&p->current);
    advance(p);

    expect(p, TOK_LPAREN, "expected '(' after function name");

    paramlist_init(&out->params);

    if (!check(p, TOK_RPAREN)) {
        for (;;) {
            if (!is_type_keyword(p)) {
                error_at(p, "expected type keyword in parameter list");
                break;
            }
            SlagType ptype = slag_type_from_token(p->current.type);
            advance(p);

            if (!check(p, TOK_IDENT)) {
                error_at(p, "expected parameter name");
                break;
            }
            Param param;
            param.type = ptype;
            param.name = copy_text(&p->current);
            advance(p);
            paramlist_push(&out->params, param);

            if (!match(p, TOK_COMMA)) break;
        }
    }

    expect(p, TOK_RPAREN, "expected ')' after parameter list");
    expect(p, TOK_LBRACE, "expected '{' to open function body");

    stmtlist_init(&out->body);
    parse_block(p, &out->body);

    expect(p, TOK_RBRACE, "expected '}' to close function body");
    return 1;
}

// ---------------------------------------------------------------------
// Top-level program
// ---------------------------------------------------------------------

Program parse_program(const char *src, size_t len) {
    Parser p;
    lexer_init(&p.lexer, src, len);
    p.had_error = 0;

    // Prime the lookahead.
    // We need a dummy token to free on the first advance; use a static empty one.
    p.current.type = TOK_UNKNOWN;
    p.current.text = NULL;
    p.current.line = 1;
    p.current.col  = 1;
    p.current.int_val = 0;
    p.current.float_val = 0.0;
    advance(&p); // loads first real token

    Program prog;
    functionlist_init(&prog.functions);
    stmtlist_init(&prog.globals);
    stmtlist_init(&prog.handlers);

    while (!check(&p, TOK_EOF)) {
        if (check(&p, TOK_KW_FUNCTION)) {
            Function func;
            if (parse_function(&p, &func)) {
                functionlist_push(&prog.functions, func);
            }
        } else if (check(&p, TOK_KW_GLOBAL)) {
            Stmt *glob = parse_global_decl(&p);
            if (glob) {
                stmtlist_push(&prog.globals, glob);
            }
        } else if (check(&p, TOK_KW_ON)) {
            Stmt *h = parse_on_handler(&p);
            if (h) {
                stmtlist_push(&prog.handlers, h);
            }
        } else {
            error_at(&p, "expected 'function', 'global', or 'on' at top level");
            advance(&p); // recover
        }
    }

    token_free(&p.current);
    return prog;
}
