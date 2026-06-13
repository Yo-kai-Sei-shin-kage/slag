#ifndef LEXER_H
#define LEXER_H

typedef enum {
    // End of file
    TOK_EOF,

    // Literals
    TOK_INT_LIT,      // 123
    TOK_FLOAT_LIT,    // 3.14
    TOK_STR_LIT,      // "..."
    TOK_REGEX_LIT,    // /.../

    // Identifiers and keywords
    TOK_IDENT,
    TOK_KW_INT,
    TOK_KW_FLOAT,
    TOK_KW_STR,
    TOK_KW_BOOL,
    TOK_KW_IF,
    TOK_KW_ELSE,
    TOK_KW_WHILE,
    TOK_KW_TRUE,
    TOK_KW_FALSE,
    TOK_KW_FUNCTION,
    TOK_KW_RETURN,
    TOK_KW_VAR,
    TOK_KW_THREAD,
    TOK_KW_SYNC,
    TOK_KW_LOCK,
    TOK_KW_WINDOW,
    TOK_KW_PIXEL,
    TOK_KW_FLUSH,
    TOK_KW_ON,
    TOK_KW_VOID,

    // $(( opener for arithmetic blocks
    TOK_DOLLAR_LPAREN_LPAREN,  // $((

    // $identifier (variable reference inside $(( )))
    TOK_DOLLAR_IDENT,

    // Punctuation / operators
    TOK_LPAREN,       // (
    TOK_RPAREN,       // )
    TOK_LBRACE,       // {
    TOK_RBRACE,       // }
    TOK_LBRACKET,     // [
    TOK_RBRACKET,     // ]
    TOK_SEMICOLON,    // ;
    TOK_COMMA,        // ,
    TOK_DOT,          // .

    TOK_PLUS,         // +
    TOK_MINUS,        // -
    TOK_STAR,         // *
    TOK_SLASH,        // /
    TOK_PERCENT,      // %

    TOK_ASSIGN,       // =
    TOK_EQ,           // ==
    TOK_NEQ,          // !=
    TOK_LT,           // 
    TOK_GT,           // >
    TOK_LE,           // <=
    TOK_GE,           // >=

    TOK_AND,          // &&
    TOK_OR,           // ||
    TOK_NOT,          // !

    TOK_UNKNOWN
} TokenType;

typedef struct {
    TokenType type;
    char *text;     // raw lexeme (heap-allocated, owned by token)
    int line;
    int col;

    // Literal values
    long int_val;
    double float_val;
} Token;

typedef struct {
    const char *src;   // full source buffer (not owned)
    size_t pos;        // current byte offset
    size_t len;        // length of src
    int line;
    int col;
} Lexer;

// Initialize a lexer over a source buffer (caller retains ownership of src).
void lexer_init(Lexer *lex, const char *src, size_t len);

// Produce the next token. Caller owns the returned Token.text and must
// free it via token_free.
Token lexer_next(Lexer *lex);

// Free a token's heap-allocated lexeme.
void token_free(Token *tok);

// Debug: return a human-readable name for a token type.
const char *token_type_name(TokenType type);

#endif // LEXER_H
