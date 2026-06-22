#ifndef AST_H
#define AST_H

#include "lexer.h"

// ---------------------------------------------------------------------
// Type system
// ---------------------------------------------------------------------

typedef enum {
    TYPE_INT,
    TYPE_FLOAT,
    TYPE_STR,
    TYPE_BOOL,
    TYPE_VOID,
    TYPE_UNKNOWN
} SlagType;

// Returns the SlagType corresponding to a type keyword token, or
// TYPE_UNKNOWN if tok is not a type keyword.
SlagType slag_type_from_token(TokenType tok);

const char *slag_type_name(SlagType type);

// ---------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------

typedef enum {
    EXPR_INT_LIT,       // 123
    EXPR_FLOAT_LIT,     // 3.14
    EXPR_STR_LIT,       // "..."
    EXPR_BOOL_LIT,      // true / false
    EXPR_REGEX_LIT,     // /.../
    EXPR_IDENT,         // foo
    EXPR_DOLLAR_IDENT,  // $foo (only valid inside ARITH blocks)
    EXPR_ARITH,         // $(( ... )) - wraps a sub-expression tree
    EXPR_BINARY,        // a + b, a < b, etc.
    EXPR_UNARY,         // !a, -a
    EXPR_LOGICAL,       // a && b, a || b
    EXPR_CALL,          // foo(a, b, c)
    EXPR_INDEX,         // foo[expr]
    EXPR_MEMBER,        // foo.bar
    EXPR_MEMBER_CALL    // foo.bar(args)  e.g. window.open(...)
} ExprKind;

typedef struct Expr Expr;

typedef struct {
    Expr **items;
    int count;
    int capacity;
} ExprList;

struct Expr {
    ExprKind kind;
    int line;
    int col;

    union {
        long int_val;          // EXPR_INT_LIT
        double float_val;      // EXPR_FLOAT_LIT
        int bool_val;          // EXPR_BOOL_LIT (0/1)

        struct {
            char *value;        // EXPR_STR_LIT, EXPR_REGEX_LIT,
                                 // EXPR_IDENT, EXPR_DOLLAR_IDENT
        } str;

        struct {
            Expr *inner;        // EXPR_ARITH: the expression inside $(( ))
        } arith;

        struct {
            TokenType op;        // TOK_PLUS, TOK_MINUS, TOK_LT, etc.
            Expr *left;
            Expr *right;
        } binary;

        struct {
            TokenType op;        // TOK_NOT, TOK_MINUS
            Expr *operand;
        } unary;

        struct {
            TokenType op;        // TOK_AND, TOK_OR
            Expr *left;
            Expr *right;
        } logical;

        struct {
            char *name;          // function name
            ExprList args;
        } call;

        struct {
            Expr *base;          // array/expr being indexed
            Expr *index;
        } index;

        struct {
            Expr *base;          // e.g. "errors" or "window"
            char *member;        // e.g. "len", "open"
        } member;

        struct {
            Expr *base;          // e.g. "window"
            char *member;        // e.g. "open"
            ExprList args;
        } member_call;
    } as;
};

// ---------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------

typedef enum {
    STMT_VAR_DECL,      // var TYPE name = expr;
    STMT_ARRAY_DECL,    // var TYPE[size] name = {...};  or  var TYPE[] name = expr;
    STMT_ASSIGN,        // name = expr;   or  name[index] = expr;
    STMT_IF,            // if (cond) { ... } else { ... }
    STMT_WHILE,         // while (cond) { ... }
    STMT_RETURN,        // return; | return TYPE expr;
    STMT_EXPR,          // expression statement, e.g. println(x);
    STMT_BLOCK,         // { ... } - used for if/while bodies
    STMT_THREAD,        // thread { ... }
    STMT_SYNC,          // sync { ... }
    STMT_LOCK,          // lock { ... }
    STMT_ON_HANDLER,     // on key_down(int k) { ... }
    STMT_GLOBAL_DECL,   // global TYPE name = expr;
    STMT_LOCAL_DECL,    // local TYPE name = expr;
} StmtKind;

typedef struct Stmt Stmt;

typedef struct {
    Stmt **items;
    int count;
    int capacity;
} StmtList;

// A single typed parameter, used for function params and `on` handlers.
typedef struct {
    SlagType type;
    char *name;
} Param;

typedef struct {
    Param *items;
    int count;
    int capacity;
} ParamList;

struct Stmt {
    StmtKind kind;
    int line;
    int col;

    union {
        struct {
            SlagType type;
            char *name;
            Expr *init;          // initializer expression (required)
        } var_decl;

        struct {
            SlagType elem_type;
            char *name;
            Expr *size_expr;      // NULL if size is inferred (TYPE[])
            ExprList init_list;   // for {1,2,3} literal init; empty if init is a call
            Expr *init_call;      // e.g. match(...) - NULL if init_list used
            int is_global;        // 1 if declared with 'global', 0 otherwise
        } array_decl;

        struct {
            Expr *target;         // EXPR_IDENT or EXPR_INDEX
            Expr *value;
        } assign;

        struct {
            Expr *cond;
            StmtList then_body;
            StmtList else_body;   // empty if no else; else_body.count==0
            int has_else;
        } if_stmt;

        struct {
            Expr *cond;
            StmtList body;
        } while_stmt;

        struct {
            int is_void;          // 1 for bare "return;"
            SlagType type;        // valid if !is_void
            Expr *value;          // NULL if is_void
        } return_stmt;

        struct {
            Expr *expr;
        } expr_stmt;

        struct {
            StmtList body;
        } block;

        struct {
            StmtList body;
        } thread_stmt;

        struct {
            StmtList body;
        } sync_stmt;

        struct {
            StmtList body;
        } lock_stmt;

        struct {
            char *event_name;     // e.g. "key_down", "mouse_move"
            ParamList params;
            StmtList body;
        } on_handler;
    } as;
};

// ---------------------------------------------------------------------
// Top-level: functions and program
// ---------------------------------------------------------------------

typedef struct {
    char *name;
    ParamList params;
    StmtList body;
    int line;
    int col;
} Function;

typedef struct {
    Function *items;
    int count;
    int capacity;
} FunctionList;

typedef struct {
    FunctionList functions;
    StmtList globals;
} Program;

// ---------------------------------------------------------------------
// List helpers (generic growable arrays)
// ---------------------------------------------------------------------

void exprlist_init(ExprList *list);
void exprlist_push(ExprList *list, Expr *expr);

void stmtlist_init(StmtList *list);
void stmtlist_push(StmtList *list, Stmt *stmt);

void paramlist_init(ParamList *list);
void paramlist_push(ParamList *list, Param param);

void functionlist_init(FunctionList *list);
void functionlist_push(FunctionList *list, Function func);

// ---------------------------------------------------------------------
// Constructors (allocate on heap; caller does not need to free manually
// during normal compiler operation - freed via ast_free_* if needed)
// ---------------------------------------------------------------------

Expr *expr_new(ExprKind kind, int line, int col);
Stmt *stmt_new(StmtKind kind, int line, int col);

// ---------------------------------------------------------------------
// Debug printing
// ---------------------------------------------------------------------

void ast_print_program(const Program *prog);

#endif // AST_H
