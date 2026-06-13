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

static Expr *parse_expr(Parser *p, int in_arith);

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

    // $(( ... )) - nested arithmetic block
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
    Expr *e = expr_new(EXPR_INT_LIT, line, col);
    e->as.int_val = 0;
    advance(p);
    return e;
}
// Parse a unary expression: !expr, -expr, or fall through to primary.
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
    while (check(p, TOK_LT) || check(p, TOK_GT) || check(p, TOK_LE) || check(p, TOK_GE)) {
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
