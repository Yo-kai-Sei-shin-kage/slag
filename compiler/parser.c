// parser.c — Slag recursive descent parser
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
//   advance(p)          — consume current, load next token
//   check(p, type)      — true if current token is `type`
//   match(p, type)      — consume and return true if current == type
//   expect(p, type, msg)— consume or emit error
//   error_at(p, msg)    — print error at current token position
//   copy_text(tok)      — heap-duplicate tok->text (caller owns)
//
// ---------------------------------------------------------------------
// Expression parsing
// ---------------------------------------------------------------------
//
// Two parallel expression grammars exist:
//
//  - Normal expressions: identifiers appear as `foo`, used everywhere
//    outside $(( )).
//  - Arithmetic expressions: identifiers appear as `$foo` (TOK_DOLLAR_IDENT),
//    used only inside $(( )).
//
// Both share the same operator precedence structure. A flag threaded
// through the recursive descent (`in_arith`) selects which identifier
// form is expected at the primary level. Function calls, indexing, and
// member access are permitted in both grammars (e.g. errors.len inside
// $(( errors.len )) is plausible), but the base identifier/variable
// follows the grammar's form.

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

static Expr *parse_expr(Parser *p, int in_arith);
static Stmt *parse_stmt(Parser *p);
static Stmt *parse_lock(Parser *p);
static void parse_block(Parser *p, StmtList *out);

// ---------------------------------------------------------------------
// Argument list
// ---------------------------------------------------------------------

// Parse a comma-separated argument list until a closing token is reached.
// Does not consume the closing token.
static void parse_arg_list(Parser *p, ExprList *out, int in_arith) {
    exprlist_init(out);
    if (check(p, TOK_RPAREN)) {
        return;
    }
    for (;;) {
        Expr *arg = parse_expr(p, in_arith);
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
// member calls (e.g. window.open(...), errors[$i], errors.len).
static Expr *parse_postfix(Parser *p, Expr *base, int in_arith) {
    for (;;) {
        if (check(p, TOK_LBRACKET)) {
            int line = p->current.line, col = p->current.col;
            advance(p); // [
            Expr *index = parse_expr(p, in_arith);
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
                parse_arg_list(p, &args, in_arith);
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

// Parse a primary expression: literals, identifiers, $idents, $((...)),
// parenthesized expressions, and function calls.
static Expr *parse_primary(Parser *p, int in_arith) {
    int line = p->current.line, col = p->current.col;

    if (check(p, TOK_INT_LIT)) {
        Expr *e = expr_new(EXPR_INT_LIT, line, col);
        e->as.int_val = p->current.int_val;
        advance(p);
        return parse_postfix(p, e, in_arith);
    }

    if (check(p, TOK_FLOAT_LIT)) {
        Expr *e = expr_new(EXPR_FLOAT_LIT, line, col);
        e->as.float_val = p->current.float_val;
        advance(p);
        return parse_postfix(p, e, in_arith);
    }

    if (check(p, TOK_STR_LIT)) {
        Expr *e = expr_new(EXPR_STR_LIT, line, col);
        e->as.str.value = copy_text(&p->current);
        advance(p);
        return parse_postfix(p, e, in_arith);
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
        return parse_postfix(p, e, in_arith);
    }

    // $identifier
    if (check(p, TOK_DOLLAR_IDENT)) {
        Expr *e = expr_new(EXPR_DOLLAR_IDENT, line, col);
        e->as.str.value = copy_text(&p->current);
        advance(p);
        return parse_postfix(p, e, in_arith);
    }

    // $(( ... )) — nested arithmetic block
    if (check(p, TOK_DOLLAR_LPAREN_LPAREN)) {
        advance(p); // $((
        Expr *inner = parse_expr(p, /*in_arith=*/1);
        expect(p, TOK_RPAREN, "expected ')' to close arithmetic block");
        expect(p, TOK_RPAREN, "expected second ')' to close arithmetic block");

        Expr *e = expr_new(EXPR_ARITH, line, col);
        e->as.arith.inner = inner;
        return parse_postfix(p, e, in_arith);
    }

    // Parenthesized expression: ( expr )
    if (check(p, TOK_LPAREN)) {
        advance(p); // (
        Expr *inner = parse_expr(p, in_arith);
        expect(p, TOK_RPAREN, "expected ')' after expression");
        return parse_postfix(p, inner, in_arith);
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
            parse_arg_list(p, &args, in_arith);
            expect(p, TOK_RPAREN, "expected ')' after arguments");

            Expr *e = expr_new(EXPR_CALL, line, col);
            e->as.call.name = name;
            e->as.call.args = args;
            return parse_postfix(p, e, in_arith);
        }

        Expr *e = expr_new(EXPR_IDENT, line, col);
        e->as.str.value = name;
        return parse_postfix(p, e, in_arith);
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
static Expr *parse_unary(Parser *p, int in_arith) {
    if (check(p, TOK_NOT) || check(p, TOK_MINUS)) {
        int line = p->current.line, col = p->current.col;
        TokenType op = p->current.type;
        advance(p);
        Expr *operand = parse_unary(p, in_arith);

        Expr *e = expr_new(EXPR_UNARY, line, col);
        e->as.unary.op = op;
        e->as.unary.operand = operand;
        return e;
    }
    return parse_primary(p, in_arith);
}

// Multiplicative: * / %
static Expr *parse_multiplicative(Parser *p, int in_arith) {
    Expr *left = parse_unary(p, in_arith);
    while (check(p, TOK_STAR) || check(p, TOK_SLASH) || check(p, TOK_PERCENT)) {
        int line = p->current.line, col = p->current.col;
        TokenType op = p->current.type;
        advance(p);
        Expr *right = parse_unary(p, in_arith);

        Expr *e = expr_new(EXPR_BINARY, line, col);
        e->as.binary.op = op;
        e->as.binary.left = left;
        e->as.binary.right = right;
        left = e;
    }
    return left;
}

// Additive: + -
static Expr *parse_additive(Parser *p, int in_arith) {
    Expr *left = parse_multiplicative(p, in_arith);
    while (check(p, TOK_PLUS) || check(p, TOK_MINUS)) {
        int line = p->current.line, col = p->current.col;
        TokenType op = p->current.type;
        advance(p);
        Expr *right = parse_multiplicative(p, in_arith);

        Expr *e = expr_new(EXPR_BINARY, line, col);
        e->as.binary.op = op;
        e->as.binary.left = left;
        e->as.binary.right = right;
        left = e;
    }
    return left;
}

// Relational: < > <= >=
static Expr *parse_relational(Parser *p, int in_arith) {
    Expr *left = parse_additive(p, in_arith);
    while (check(p, TOK_LT) || check(p, TOK_GT) ||
           check(p, TOK_LE) || check(p, TOK_GE)) {
        int line = p->current.line, col = p->current.col;
        TokenType op = p->current.type;
        advance(p);
        Expr *right = parse_additive(p, in_arith);

        Expr *e = expr_new(EXPR_BINARY, line, col);
        e->as.binary.op = op;
        e->as.binary.left = left;
        e->as.binary.right = right;
        left = e;
    }
    return left;
}

// Equality: == !=
static Expr *parse_equality(Parser *p, int in_arith) {
    Expr *left = parse_relational(p, in_arith);
    while (check(p, TOK_EQ) || check(p, TOK_NEQ)) {
        int line = p->current.line, col = p->current.col;
        TokenType op = p->current.type;
        advance(p);
        Expr *right = parse_relational(p, in_arith);

        Expr *e = expr_new(EXPR_BINARY, line, col);
        e->as.binary.op = op;
        e->as.binary.left = left;
        e->as.binary.right = right;
        left = e;
    }
    return left;
}

// Logical AND: &&
static Expr *parse_logical_and(Parser *p, int in_arith) {
    Expr *left = parse_equality(p, in_arith);
    while (check(p, TOK_AND)) {
        int line = p->current.line, col = p->current.col;
        advance(p);
        Expr *right = parse_equality(p, in_arith);

        Expr *e = expr_new(EXPR_LOGICAL, line, col);
        e->as.logical.op = TOK_AND;
        e->as.logical.left = left;
        e->as.logical.right = right;
        left = e;
    }
    return left;
}

// Logical OR: ||
static Expr *parse_logical_or(Parser *p, int in_arith) {
    Expr *left = parse_logical_and(p, in_arith);
    while (check(p, TOK_OR)) {
        int line = p->current.line, col = p->current.col;
        advance(p);
        Expr *right = parse_logical_and(p, in_arith);

        Expr *e = expr_new(EXPR_LOGICAL, line, col);
        e->as.logical.op = TOK_OR;
        e->as.logical.left = left;
        e->as.logical.right = right;
        left = e;
    }
    return left;
}

// Top-level expression entry point.
static Expr *parse_expr(Parser *p, int in_arith) {
    return parse_logical_or(p, in_arith);
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
// var declaration
//
// var TYPE name = expr;
// var TYPE[size] name = { expr, ... };
// var TYPE[size] name = call(...);
// var TYPE[]     name = call(...);    // size inferred from initializer
// ---------------------------------------------------------------------
static Stmt *parse_var_decl(Parser *p) {
    int line = p->current.line, col = p->current.col;
    advance(p); // consume 'var'

    if (!is_type_keyword(p)) {
        error_at(p, "expected type keyword after 'var'");
        return NULL;
    }

    SlagType base_type = slag_type_from_token(p->current.type);
    advance(p); // consume type keyword

    // Array declaration: TYPE[ or TYPE[]
    if (check(p, TOK_LBRACKET)) {
        advance(p); // [

        Expr *size_expr = NULL;
        if (!check(p, TOK_RBRACKET)) {
            // Fixed size: int[8] or int[$n]
            size_expr = parse_expr(p, 0);
        }
        // else: inferred size: TYPE[]

        expect(p, TOK_RBRACKET, "expected ']' after array size");

        if (!check(p, TOK_IDENT)) {
            error_at(p, "expected array name");
            return NULL;
        }
        char *name = copy_text(&p->current);
        advance(p);

        expect(p, TOK_ASSIGN, "expected '=' in array declaration");

        Stmt *s = stmt_new(STMT_ARRAY_DECL, line, col);
        s->as.array_decl.elem_type = base_type;
        s->as.array_decl.name = name;
        s->as.array_decl.size_expr = size_expr;
        exprlist_init(&s->as.array_decl.init_list);
        s->as.array_decl.init_call = NULL;

        if (check(p, TOK_LBRACE)) {
            // Brace initializer: { expr, expr, ... }
            advance(p); // {
            if (!check(p, TOK_RBRACE)) {
                for (;;) {
                    Expr *elem = parse_expr(p, 0);
                    exprlist_push(&s->as.array_decl.init_list, elem);
                    if (!match(p, TOK_COMMA)) break;
                    if (check(p, TOK_RBRACE)) break; // trailing comma OK
                }
            }
            expect(p, TOK_RBRACE, "expected '}' after array initializer");
        } else {
            // Call initializer: match(...) or similar
            s->as.array_decl.init_call = parse_expr(p, 0);
        }

        expect(p, TOK_SEMICOLON, "expected ';' after array declaration");
        return s;
    }

    // Scalar declaration: TYPE name = expr;
    if (!check(p, TOK_IDENT)) {
        error_at(p, "expected variable name");
        return NULL;
    }
    char *name = copy_text(&p->current);
    advance(p);

    expect(p, TOK_ASSIGN, "expected '=' in variable declaration");

    Expr *init = parse_expr(p, 0);
    expect(p, TOK_SEMICOLON, "expected ';' after variable declaration");

    Stmt *s = stmt_new(STMT_VAR_DECL, line, col);
    s->as.var_decl.type = base_type;
    s->as.var_decl.name = name;
    s->as.var_decl.init = init;
    return s;
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

    if (!check(p, TOK_IDENT)) {
        error_at(p, "expected variable name");
        return NULL;
    }
    char *name = copy_text(&p->current);
    advance(p);

    expect(p, TOK_ASSIGN, "expected '=' in global declaration");

    Expr *init = parse_expr(p, 0);
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

    if (!check(p, TOK_IDENT)) {
        error_at(p, "expected variable name");
        return NULL;
    }
    char *name = copy_text(&p->current);
    advance(p);

    expect(p, TOK_ASSIGN, "expected '=' in local declaration");

    Expr *init = parse_expr(p, 0);
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

    Expr *lhs = parse_expr(p, 0);

    if (check(p, TOK_ASSIGN)) {
        advance(p); // =
        Expr *rhs = parse_expr(p, 0);
        expect(p, TOK_SEMICOLON, "expected ';' after assignment");

        // Validate LHS: must be an ident, index, or member.
        if (lhs->kind != EXPR_IDENT && lhs->kind != EXPR_INDEX &&
            lhs->kind != EXPR_MEMBER && lhs->kind != EXPR_DOLLAR_IDENT) {
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
    Expr *cond = parse_expr(p, 0);
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
// while statement
// ---------------------------------------------------------------------
static Stmt *parse_while(Parser *p) {
    int line = p->current.line, col = p->current.col;
    advance(p); // consume 'while'

    expect(p, TOK_LPAREN, "expected '(' after 'while'");
    Expr *cond = parse_expr(p, 0);
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
// return;              — void return
// return TYPE expr;    — typed return
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
        // No type keyword — treat as void return with a warning.
        fprintf(stderr, "warning: line %d: 'return' missing type keyword, treating as void\n", line);
        s->as.return_stmt.is_void = 1;
        s->as.return_stmt.type = TYPE_VOID;
        s->as.return_stmt.value = NULL;
        expect(p, TOK_SEMICOLON, "expected ';' after return");
        return s;
    }

    s->as.return_stmt.is_void = 0;
    s->as.return_stmt.value = parse_expr(p, 0);
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
        case TOK_KW_VAR:    return parse_var_decl(p);
        case TOK_KW_GLOBAL: return parse_global_decl(p);
        case TOK_KW_LOCAL:  return parse_local_decl(p);
        case TOK_KW_IF:     return parse_if(p);
        case TOK_KW_WHILE:  return parse_while(p);
        case TOK_KW_RETURN: return parse_return(p);
        case TOK_KW_THREAD: return parse_thread(p);
        case TOK_KW_SYNC:   return parse_sync(p);
        case TOK_KW_LOCK:   return parse_lock(p);
        case TOK_KW_ON:     return parse_on_handler(p);

        case TOK_LBRACE: {
            // Anonymous block — rare but legal.
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
            // Empty statement — skip silently.
            advance(p);
            return NULL;

        // Identifiers and window/pixel/flush can start assignments or
        // expression statements.
        case TOK_IDENT:
        case TOK_KW_WINDOW:
        case TOK_KW_PIXEL:
        case TOK_KW_FLUSH:
        case TOK_DOLLAR_IDENT:
        case TOK_DOLLAR_LPAREN_LPAREN:
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
        } else {
            error_at(&p, "expected 'function' or 'global' at top level");
            advance(&p); // recover
        }
    }

    token_free(&p.current);
    return prog;
}
