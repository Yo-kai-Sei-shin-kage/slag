#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ast.h"

// ---------------------------------------------------------------------
// Type system
// ---------------------------------------------------------------------

SlagType slag_type_from_token(TokenType tok) {
    switch (tok) {
        case TOK_KW_INT:   return TYPE_INT;
        case TOK_KW_FLOAT: return TYPE_FLOAT;
        case TOK_KW_STR:   return TYPE_STR;
        case TOK_KW_BOOL:  return TYPE_BOOL;
        case TOK_KW_VOID:  return TYPE_VOID;
        default:           return TYPE_UNKNOWN;
    }
}

const char *slag_type_name(SlagType type) {
    switch (type) {
        case TYPE_INT:   return "int";
        case TYPE_FLOAT: return "float";
        case TYPE_STR:   return "str";
        case TYPE_BOOL:  return "bool";
        case TYPE_VOID:  return "void";
        default:         return "unknown";
    }
}

// ---------------------------------------------------------------------
// Generic growable list helpers
// ---------------------------------------------------------------------

void exprlist_init(ExprList *list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

void exprlist_push(ExprList *list, Expr *expr) {
    if (list->count >= list->capacity) {
        list->capacity = list->capacity == 0 ? 4 : list->capacity * 2;
        list->items = realloc(list->items, sizeof(Expr *) * list->capacity);
    }
    list->items[list->count++] = expr;
}

void stmtlist_init(StmtList *list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

void stmtlist_push(StmtList *list, Stmt *stmt) {
    if (list->count >= list->capacity) {
        list->capacity = list->capacity == 0 ? 4 : list->capacity * 2;
        list->items = realloc(list->items, sizeof(Stmt *) * list->capacity);
    }
    list->items[list->count++] = stmt;
}

void paramlist_init(ParamList *list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

void paramlist_push(ParamList *list, Param param) {
    if (list->count >= list->capacity) {
        list->capacity = list->capacity == 0 ? 4 : list->capacity * 2;
        list->items = realloc(list->items, sizeof(Param) * list->capacity);
    }
    list->items[list->count++] = param;
}

void functionlist_init(FunctionList *list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

void functionlist_push(FunctionList *list, Function func) {
    if (list->count >= list->capacity) {
        list->capacity = list->capacity == 0 ? 4 : list->capacity * 2;
        list->items = realloc(list->items, sizeof(Function) * list->capacity);
    }
    list->items[list->count++] = func;
}

// ---------------------------------------------------------------------
// Constructors
// ---------------------------------------------------------------------

Expr *expr_new(ExprKind kind, int line, int col) {
    Expr *e = malloc(sizeof(Expr));
    memset(e, 0, sizeof(Expr));
    e->kind = kind;
    e->line = line;
    e->col = col;
    return e;
}

Stmt *stmt_new(StmtKind kind, int line, int col) {
    Stmt *s = malloc(sizeof(Stmt));
    memset(s, 0, sizeof(Stmt));
    s->kind = kind;
    s->line = line;
    s->col = col;
    return s;
}

// ---------------------------------------------------------------------
// Debug printing
// ---------------------------------------------------------------------

static void print_indent(int depth) {
    for (int i = 0; i < depth; i++) {
        printf("  ");
    }
}

static const char *binop_name(TokenType op) {
    switch (op) {
        case TOK_PLUS: return "+";
        case TOK_MINUS: return "-";
        case TOK_STAR: return "*";
        case TOK_SLASH: return "/";
        case TOK_PERCENT: return "%";
        case TOK_EQ: return "==";
        case TOK_NEQ: return "!=";
        case TOK_LT: return "<";
        case TOK_GT: return ">";
        case TOK_LE: return "<=";
        case TOK_GE: return ">=";
        case TOK_AND: return "&&";
        case TOK_OR: return "||";
        case TOK_NOT: return "!";
        default: return "?";
    }
}
static void print_expr(const Expr *e, int depth);

static void print_exprlist(const ExprList *list, int depth) {
    for (int i = 0; i < list->count; i++) {
        print_expr(list->items[i], depth);
    }
}

static void print_expr(const Expr *e, int depth) {
    if (!e) {
        print_indent(depth);
        printf("(null expr)\n");
        return;
    }

    print_indent(depth);

    switch (e->kind) {
        case EXPR_INT_LIT:
            printf("IntLit %ld\n", e->as.int_val);
            break;
        case EXPR_FLOAT_LIT:
            printf("FloatLit %f\n", e->as.float_val);
            break;
        case EXPR_STR_LIT:
            printf("StrLit \"%s\"\n", e->as.str.value);
            break;
        case EXPR_BOOL_LIT:
            printf("BoolLit %s\n", e->as.bool_val ? "true" : "false");
            break;
        case EXPR_REGEX_LIT:
            printf("RegexLit /%s/\n", e->as.str.value);
            break;
        case EXPR_IDENT:
            printf("Ident %s\n", e->as.str.value);
            break;
        case EXPR_BINARY:
            printf("Binary %s\n", binop_name(e->as.binary.op));
            print_expr(e->as.binary.left, depth + 1);
            print_expr(e->as.binary.right, depth + 1);
            break;
        case EXPR_UNARY:
            printf("Unary %s\n", binop_name(e->as.unary.op));
            print_expr(e->as.unary.operand, depth + 1);
            break;
        case EXPR_LOGICAL:
            printf("Logical %s\n", binop_name(e->as.logical.op));
            print_expr(e->as.logical.left, depth + 1);
            print_expr(e->as.logical.right, depth + 1);
            break;
        case EXPR_CALL:
            printf("Call %s(\n", e->as.call.name);
            print_exprlist(&e->as.call.args, depth + 1);
            print_indent(depth);
            printf(")\n");
            break;
        case EXPR_INDEX:
            printf("Index\n");
            print_expr(e->as.index.base, depth + 1);
            print_indent(depth);
            printf("[\n");
            print_expr(e->as.index.index, depth + 1);
            print_indent(depth);
            printf("]\n");
            break;
        case EXPR_MEMBER:
            printf("Member .%s\n", e->as.member.member);
            print_expr(e->as.member.base, depth + 1);
            break;
        case EXPR_MEMBER_CALL:
            printf("MemberCall .%s(\n", e->as.member_call.member);
            print_expr(e->as.member_call.base, depth + 1);
            print_exprlist(&e->as.member_call.args, depth + 1);
            print_indent(depth);
            printf(")\n");
            break;
        default:
            printf("(unknown expr kind %d)\n", e->kind);
            break;
    }
}

static void print_stmtlist(const StmtList *list, int depth);

static void print_stmt(const Stmt *s, int depth) {
    if (!s) {
        print_indent(depth);
        printf("(null stmt)\n");
        return;
    }

    print_indent(depth);

    switch (s->kind) {
        case STMT_ARRAY_DECL:
            printf("ArrayDecl %s[] %s =\n", slag_type_name(s->as.array_decl.elem_type), s->as.array_decl.name);
            if (s->as.array_decl.size_expr) {
                print_indent(depth + 1);
                printf("size:\n");
                print_expr(s->as.array_decl.size_expr, depth + 2);
            }
            if (s->as.array_decl.init_call) {
                print_expr(s->as.array_decl.init_call, depth + 1);
            } else {
                print_indent(depth + 1);
                printf("{\n");
                print_exprlist(&s->as.array_decl.init_list, depth + 2);
                print_indent(depth + 1);
                printf("}\n");
            }
            break;

        case STMT_ASSIGN:
            printf("Assign\n");
            print_expr(s->as.assign.target, depth + 1);
            print_indent(depth);
            printf("=\n");
            print_expr(s->as.assign.value, depth + 1);
            break;

        case STMT_IF:
            printf("If\n");
            print_expr(s->as.if_stmt.cond, depth + 1);
            print_indent(depth);
            printf("then:\n");
            print_stmtlist(&s->as.if_stmt.then_body, depth + 1);
            if (s->as.if_stmt.has_else) {
                print_indent(depth);
                printf("else:\n");
                print_stmtlist(&s->as.if_stmt.else_body, depth + 1);
            }
            break;

        case STMT_WHILE:
            printf("While\n");
            print_expr(s->as.while_stmt.cond, depth + 1);
            print_indent(depth);
            printf("body:\n");
            print_stmtlist(&s->as.while_stmt.body, depth + 1);
            break;

        case STMT_RETURN:
            if (s->as.return_stmt.is_void) {
                printf("Return (void)\n");
            } else {
                printf("Return %s\n", slag_type_name(s->as.return_stmt.type));
                print_expr(s->as.return_stmt.value, depth + 1);
            }
            break;

        case STMT_EXPR:
            printf("ExprStmt\n");
            print_expr(s->as.expr_stmt.expr, depth + 1);
            break;

        case STMT_BLOCK:
            printf("Block\n");
            print_stmtlist(&s->as.block.body, depth + 1);
            break;

        case STMT_THREAD:
            printf("Thread\n");
            print_stmtlist(&s->as.thread_stmt.body, depth + 1);
            break;

        case STMT_SYNC:
            printf("Sync\n");
            print_stmtlist(&s->as.sync_stmt.body, depth + 1);
            break;

        case STMT_ON_HANDLER:
            printf("On %s(", s->as.on_handler.event_name);
            for (int i = 0; i < s->as.on_handler.params.count; i++) {
                Param p = s->as.on_handler.params.items[i];
                printf("%s %s%s", slag_type_name(p.type), p.name,
                       (i + 1 < s->as.on_handler.params.count) ? ", " : "");
            }
            printf(")\n");
            print_stmtlist(&s->as.on_handler.body, depth + 1);
            break;

        case STMT_GLOBAL_DECL:
            printf("GlobalDecl %s %s =\n", slag_type_name(s->as.var_decl.type), s->as.var_decl.name);
            print_expr(s->as.var_decl.init, depth + 1);
            break;

        case STMT_LOCAL_DECL:
            printf("LocalDecl %s %s =\n", slag_type_name(s->as.var_decl.type), s->as.var_decl.name);
            print_expr(s->as.var_decl.init, depth + 1);
            break;

        default:
            printf("(unknown stmt kind %d)\n", s->kind);
            break;
    }
}

static void print_stmtlist(const StmtList *list, int depth) {
    for (int i = 0; i < list->count; i++) {
        print_stmt(list->items[i], depth);
    }
}

void ast_print_program(const Program *prog) {
    for (int i = 0; i < prog->functions.count; i++) {
        Function *f = &prog->functions.items[i];
        printf("Function %s(", f->name);
        for (int j = 0; j < f->params.count; j++) {
            Param p = f->params.items[j];
            printf("%s %s%s", slag_type_name(p.type), p.name,
                   (j + 1 < f->params.count) ? ", " : "");
        }
        printf(")\n");
        print_stmtlist(&f->body, 1);
        printf("\n");
    }
}
