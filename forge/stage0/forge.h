#ifndef FORGE_H
#define FORGE_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define MAX_TOKENS    65536
#define MAX_SRC_LEN   (4 * 1024 * 1024)
#define MAX_LABELS    4096
#define MAX_FUNCS     1024
#define MAX_LOCALS    256
#define MAX_STRINGS   1024
#define MAX_INCLUDES  64

/* ─── Token types ────────────────────────────────────────────────── */
typedef enum {
    T_EOF, T_NEWLINE, T_INDENT, T_DEDENT,
    T_IDENT, T_INT, T_HEX, T_BIN, T_FLOAT, T_STRING,
    K_FN, K_LET, K_VAR, K_STRUCT, K_IF, K_ELSE, K_WHILE, K_FOR,
    K_RETURN, K_ASM, K_VOLATILE, K_SAFETY, K_PURE, K_HARDWARE,
    K_BOUNDED, K_UNBOUNDED, K_TYPESTATE, K_TRUE, K_FALSE,
    K_IN, K_BREAK, K_CONTINUE, K_IMPORT, K_CAST, K_LAYOUT, K_PACKED,
    K_SIZE_OF, K_ADDR_OF, K_UNDEFINED, K_CONST, K_PUB,
    T_U8, T_U16, T_U32, T_U64, T_I8, T_I16, T_I32, T_I64,
    T_BOOL, T_VOID, T_PTR, T_SLICE, T_USIZE,
    O_PLUS, O_MINUS, O_STAR, O_SLASH, O_PERCENT,
    O_EQ, O_NE, O_LT, O_GT, O_LE, O_GE,
    O_AND, O_OR, O_XOR, O_SHL, O_SHR, O_NOT, O_BANG,
    O_ASSIGN, O_PLUS_EQ, O_MINUS_EQ, O_STAR_EQ,
    O_AND_AND, O_OR_OR,
    D_LPAREN, D_RPAREN, D_LBRACK, D_RBRACK,
    D_COMMA, D_COLON, D_SEMI, D_DOT, D_ARROW, D_AT,
    D_LBRACE, D_RBRACE,
    B_ERROR = 255
} TokenKind;

typedef struct {
    TokenKind kind;
    int64_t   int_val;
    double    float_val;
    char*     str_val;
    int       line;
    int       col;
    int       offset; /* byte offset in source */
} Token;

/* ─── AST node types ──────────────────────────────────────────────── */
typedef enum {
    N_PROGRAM, N_FUNC, N_STRUCT_DECL, N_TYPESTATE_DECL,
    N_LET, N_VAR, N_CONST, N_IMPORT,
    N_IF, N_WHILE, N_FOR, N_BREAK, N_CONTINUE,
    N_RETURN, N_ASM_BLOCK,
    N_BLOCK, N_ASSIGN, N_CALL, N_INDEX, N_FIELD,
    N_BINARY, N_UNARY, N_CAST, N_INT, N_FLOAT,
    N_STRING, N_BOOL, N_IDENT, N_ARRAY_LIT, N_ADDR_OF,
    N_SIZE_OF, N_DEREF, N_SAFETY_ANN, N_DATA,
} NodeKind;

typedef struct Node {
    NodeKind kind;
    int      line, col;
    union {
        struct { struct Node** stmts; int count; } program;
        struct {
            char* name; struct Node** params; int pcount;
            struct Node* ret_type; struct Node* body;
            struct Node* safety; int is_pub;
        } func;
        struct { char* name; struct Node** fields; int fcount; int packed; } struct_decl;
        struct { char* name; char** states; int scount; } typestate;
        struct { char* name; struct Node* type; struct Node* init; int is_mut; int is_const; } var;
        struct { char* path; } import;
        struct { struct Node* cond; struct Node* body; struct Node* else_body; struct Node* init; struct Node* iter; char* loop_var; } flow;
        struct { struct Node* val; } ret;
        struct { char** lines; int count; int is_volatile; } asm_block;
        struct { struct Node** stmts; int count; } block;
        struct { char* name; char* val; } data;
        struct { struct Node* target; struct Node* val; int op; } assign;
        struct { struct Node* callee; struct Node** args; int acount; } call;
        struct { struct Node* obj; struct Node* idx; } index_;
        struct { struct Node* obj; char* field; } field;
        struct { struct Node* l; struct Node* r; int op; } binary;
        struct { struct Node* op; int op_kind; } unary;
        struct { struct Node* expr; struct Node* type; } cast;
        struct { int level; } safety;
        int64_t i_val; double f_val; char* s_val; int b_val;
    } as;
} Node;

/* ─── Target architectures ────────────────────────────────────────── */
#define TARGET_X86_64 0
#define TARGET_ARM64  1

/* ─── Compiler context ────────────────────────────────────────────── */
typedef struct {
    char*     src;
    size_t    src_len;
    Token     tokens[MAX_TOKENS];
    int       ntokens;
    int       pos;
    int       target_arch; /* TARGET_X86_64 or TARGET_ARM64 */
    char      filename[256];
    char      labels[MAX_LABELS][64];
    int       label_addrs[MAX_LABELS];
    int       nlabels;
    char      func_names[MAX_FUNCS][64];
    Node*     func_nodes[MAX_FUNCS];
    int       nfuncs;
    int       current_func;
    char*     strings[MAX_STRINGS];
    int       string_addrs[MAX_STRINGS];
    int       nstrings;
    uint8_t*  code;
    int       code_cap;
    int       code_len;
    int       fixup_offsets[65536];
    int       fixup_labels[65536];
    int       nfixups;
    int       nerrors;
    char      error_msg[1024];
} Compiler;

/* ─── API ─────────────────────────────────────────────────────────── */
void lex(Compiler* c);
Node* parse_program(Compiler* c);
void print_error(Compiler* c, const char* fmt, ...);
void codegen(Compiler* c, Node* node);
void resolve_fixups(Compiler* c);
void emit_catarch_binary(Compiler* c, const char* outpath, uint64_t entry_override);
void emit_raw_binary(Compiler* c, const char* outpath, uint64_t entry_override);
int reg_encode(const char* name);
void init_compiler(Compiler* c);
void free_node(Node* n);

#endif /* FORGE_H */
