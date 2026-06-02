#include "forge.h"
#include <stdarg.h>

void print_error(Compiler* c, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(c->error_msg, sizeof(c->error_msg), fmt, args);
    va_end(args);
    Token t = c->tokens[c->pos];
    fprintf(stderr, "error[%s:%d:%d]: %s\n", c->filename, t.line, t.col, c->error_msg);
    c->nerrors++;
}

static Token peek(Compiler* c) {
    return c->tokens[c->pos];
}

static Token advance(Compiler* c) {
    if (c->pos < c->ntokens) c->pos++;
    return c->tokens[c->pos - 1];
}

static int match(Compiler* c, TokenKind k) {
    if (c->tokens[c->pos].kind == k) {
        advance(c);
        return 1;
    }
    return 0;
}

static int expect(Compiler* c, TokenKind k, const char* msg) {
    if (c->tokens[c->pos].kind == k) {
        advance(c);
        return 1;
    }
    if (!c->nerrors) print_error(c, "expected %s but got token %d", msg, c->tokens[c->pos].kind);
    return 0;
}

static Node* alloc_node(NodeKind kind, int line, int col) {
    Node* n = calloc(1, sizeof(Node));
    n->kind = kind;
    n->line = line;
    n->col = col;
    return n;
}

static Node* parse_type(Compiler* c);
static Node* parse_block(Compiler* c);
static Node* parse_stmt(Compiler* c);
static Node* parse_expr(Compiler* c);
static Node* parse_assignment(Compiler* c);

/* Forge-Sub grammar:
 * program   = (import | func_decl | struct_decl | typestate_decl)*
 * func_decl = ["pub"] "fn" ident "(" params ")" ["->" type] [safety_ann] ":" block
 * struct_decl = "struct" ident [layout(packed)] ":" indent fields dedent
 * param     = ident ":" type
 * safety_ann = "safety" "(" ("pure"|"bounded"|"hardware"|"unbounded") ")"
 */

static Node* parse_type(Compiler* c) {
    Token t = peek(c);
    int line = t.line, col = t.col;
    Node* n = alloc_node(N_IDENT, line, col);

    /* Pointer type: *T */
    if (t.kind == O_STAR) {
        advance(c);
        Node* elem = parse_type(c);
        Node* ptr = alloc_node(N_UNARY, line, col);
        ptr->as.unary.op = elem;
        ptr->as.unary.op_kind = O_STAR;
        return ptr;
    }
    /* Slice type: []T */
    if (t.kind == D_LBRACK && c->pos + 1 < c->ntokens && c->tokens[c->pos + 1].kind == D_RBRACK) {
        advance(c); advance(c);
        Node* elem = parse_type(c);
        Node* sl = alloc_node(N_UNARY, line, col);
        sl->as.unary.op = elem;
        sl->as.unary.op_kind = D_LBRACK;
        return sl;
    }
    /* Identifiers: u8, u32, struct names, etc. */
    if (t.kind == T_IDENT) {
        advance(c);
        n->as.s_val = t.str_val ? strdup(t.str_val) : strdup("");
        return n;
    }
    /* Type keywords */
    switch (t.kind) {
        case T_U8: case T_U16: case T_U32: case T_U64:
        case T_I8: case T_I16: case T_I32: case T_I64:
        case T_BOOL: case T_VOID: case T_USIZE:
            advance(c);
            n->as.i_val = t.kind;
            return n;
        default:
            print_error(c, "expected type");
            return n;
    }
}

static Node* parse_params(Compiler* c) {
    Node* n = alloc_node(N_BLOCK, peek(c).line, peek(c).col);
    n->as.block.stmts = calloc(64, sizeof(Node*));
    n->as.block.count = 0;

    expect(c, D_LPAREN, "'('");
    if (peek(c).kind != D_RPAREN) {
        while (1) {
            /* param: ident ":" type */
            Token id = advance(c);
            if (id.kind != T_IDENT) { print_error(c, "expected parameter name"); break; }
            expect(c, D_COLON, "':'");
            Node* type = parse_type(c);
            
            Node* p = alloc_node(N_LET, id.line, id.col);
            p->as.var.name = strdup(id.str_val);
            p->as.var.type = type;
            p->as.var.is_mut = 0;
            n->as.block.stmts[n->as.block.count++] = p;

            if (peek(c).kind == D_COMMA) { advance(c); continue; }
            break;
        }
    }
    expect(c, D_RPAREN, "')'");
    return n;
}

static Node* parse_safety(Compiler* c) {
    Node* n = alloc_node(N_SAFETY_ANN, peek(c).line, peek(c).col);
    n->as.safety.level = 0;
    if (match(c, K_SAFETY)) {
        expect(c, D_LPAREN, "'('");
        Token t = advance(c);
        switch (t.kind) {
            case K_PURE: n->as.safety.level = 0; break;
            case K_BOUNDED: n->as.safety.level = 1; break;
            case K_HARDWARE: n->as.safety.level = 2; break;
            case K_UNBOUNDED: n->as.safety.level = 3; break;
            default: print_error(c, "expected safety level (pure/bounded/hardware/unbounded)");
        }
        expect(c, D_RPAREN, "')'");
    }
    return n;
}

/* ─── Expression parser (precedence climbing) ─────────────────────── */
typedef enum { PREC_MIN, PREC_ASSIGN, PREC_OR, PREC_AND, PREC_EQ,
    PREC_CMP, PREC_BITOR, PREC_BITXOR, PREC_BITAND,
    PREC_SHIFT, PREC_TERM, PREC_FACTOR, PREC_UNARY, PREC_CALL } Precedence;

static Precedence op_prec(int kind) {
    switch (kind) {
        case O_ASSIGN: case O_PLUS_EQ: case O_MINUS_EQ: case O_STAR_EQ: return PREC_ASSIGN;
        case O_OR_OR: return PREC_OR;
        case O_AND_AND: return PREC_AND;
        case O_EQ: case O_NE: return PREC_EQ;
        case O_LT: case O_GT: case O_LE: case O_GE: return PREC_CMP;
        case O_OR: return PREC_BITOR;
        case O_XOR: return PREC_BITXOR;
        case O_AND: return PREC_BITAND;
        case O_SHL: case O_SHR: return PREC_SHIFT;
        case O_PLUS: case O_MINUS: return PREC_TERM;
        case O_STAR: case O_SLASH: case O_PERCENT: return PREC_FACTOR;
        default: return PREC_MIN;
    }
}

static Node* parse_primary(Compiler* c) {
    Token t = peek(c);
    int line = t.line, col = t.col;

    if (t.kind == T_INT) {
        advance(c);
        Node* n = alloc_node(N_INT, line, col);
        n->as.i_val = t.int_val;
        return n;
    }
    if (t.kind == T_FLOAT) {
        advance(c);
        Node* n = alloc_node(N_FLOAT, line, col);
        n->as.f_val = t.float_val;
        return n;
    }
    if (t.kind == T_STRING) {
        advance(c);
        Node* n = alloc_node(N_STRING, line, col);
        n->as.s_val = t.str_val ? t.str_val : strdup("");
        return n;
    }
    if (t.kind == K_TRUE || t.kind == K_FALSE) {
        advance(c);
        Node* n = alloc_node(N_BOOL, line, col);
        n->as.b_val = (t.kind == K_TRUE);
        return n;
    }
    if (t.kind == T_IDENT) {
        advance(c);
        Node* n = alloc_node(N_IDENT, line, col);
        n->as.s_val = t.str_val ? strdup(t.str_val) : strdup("");
        return n;
    }
    if (t.kind == D_LPAREN) {
        advance(c);
        Node* n = parse_expr(c);
        expect(c, D_RPAREN, "')'");
        return n;
    }
    if (t.kind == D_LBRACK) {
        /* array literal or slice */
        advance(c);
        Node* n = alloc_node(N_ARRAY_LIT, line, col);
        Node** elems = calloc(256, sizeof(Node*));
        int count = 0;
        if (peek(c).kind != D_RBRACK) {
            while (1) {
                elems[count++] = parse_expr(c);
                if (peek(c).kind == D_COMMA) advance(c);
                else break;
            }
        }
        expect(c, D_RBRACK, "']'");
        n->as.program.stmts = elems;
        n->as.program.count = count;
        return n;
    }
    if (t.kind == K_CAST) {
        advance(c);
        expect(c, D_LPAREN, "'('");
        Node* n = alloc_node(N_CAST, line, col);
        n->as.cast.type = parse_type(c);
        expect(c, D_COMMA, "','");
        n->as.cast.expr = parse_expr(c);
        expect(c, D_RPAREN, "')'");
        return n;
    }
    if (t.kind == K_ADDR_OF || t.kind == O_AND) {
        advance(c);
        Node* n = alloc_node(N_ADDR_OF, line, col);
        n->as.unary.op = parse_primary(c);
        return n;
    }
    if (t.kind == K_SIZE_OF) {
        advance(c);
        expect(c, D_LPAREN, "'('");
        Node* n = alloc_node(N_SIZE_OF, line, col);
        n->as.unary.op = parse_type(c);
        expect(c, D_RPAREN, "')'");
        return n;
    }
    if (t.kind == O_MINUS) {
        advance(c);
        Node* n = alloc_node(N_UNARY, line, col);
        n->as.unary.op_kind = O_MINUS;
        n->as.unary.op = parse_primary(c);
        return n;
    }
    if (t.kind == O_BANG) {
        advance(c);
        Node* n = alloc_node(N_UNARY, line, col);
        n->as.unary.op_kind = O_BANG;
        n->as.unary.op = parse_primary(c);
        return n;
    }
    if (t.kind == O_STAR) {
        advance(c);
        Node* n = alloc_node(N_DEREF, line, col);
        n->as.unary.op = parse_primary(c);
        return n;
    }
    if (t.kind == D_LBRACE) {
        /* Struct literal */
        advance(c);
        Node* n = alloc_node(N_FIELD, line, col);
        /* Read field: name ":" expr */
        if (peek(c).kind == T_IDENT && c->pos + 2 < c->ntokens && c->tokens[c->pos + 1].kind == D_COLON) {
            Token fn = advance(c);
            advance(c); /* skip colon */
            n->as.field.field = strdup(fn.str_val);
            n->as.field.obj = parse_expr(c);
            while (match(c, D_COMMA)) {
                /* additional fields — we just chain */
                Node* chain = alloc_node(N_FIELD, line, col);
                chain->as.field.field = strdup(peek(c).str_val);
                advance(c); advance(c); /* skip name colon */
                chain->as.field.obj = n;
                n = chain;
                parse_expr(c);
            }
            expect(c, D_RBRACE, "'}'");
        } else {
            expect(c, D_RBRACE, "'}'");
        }
        return n;
    }

    print_error(c, "unexpected token in expression");
    return alloc_node(N_INT, line, col);
}

static Node* parse_expr(Compiler* c) {
    return parse_assignment(c);
}

static Node* parse_assignment(Compiler* c) {
    Node* left = parse_primary(c);
    
    Token t = peek(c);
    while (t.kind == D_LPAREN || t.kind == D_LBRACK || t.kind == D_DOT) {
        if (t.kind == D_LPAREN) {
            /* Function call */
            advance(c);
            Node* call = alloc_node(N_CALL, left->line, left->col);
            call->as.call.callee = left;
            call->as.call.args = calloc(256, sizeof(Node*));
            call->as.call.acount = 0;
            if (peek(c).kind != D_RPAREN) {
                while (1) {
                    call->as.call.args[call->as.call.acount++] = parse_expr(c);
                    if (peek(c).kind == D_COMMA) advance(c);
                    else break;
                }
            }
            expect(c, D_RPAREN, "')'");
            left = call;
        } else if (t.kind == D_LBRACK) {
            /* Index */
            advance(c);
            Node* idx = alloc_node(N_INDEX, left->line, left->col);
            idx->as.index_.obj = left;
            idx->as.index_.idx = parse_expr(c);
            expect(c, D_RBRACK, "']'");
            left = idx;
        } else if (t.kind == D_DOT) {
            /* Field access */
            advance(c);
            Node* f = alloc_node(N_FIELD, left->line, left->col);
            f->as.field.obj = left;
            Token name = advance(c);
            f->as.field.field = name.kind == T_IDENT ? strdup(name.str_val) : strdup("");
            left = f;
        }
        t = peek(c);
    }

    /* Postfix operators and then binary */
    /* Actually, use precedence climbing */
    /* For simplicity in bootstrap, handle binary ops directly */
    while (1) {
        Token op = peek(c);
        Precedence prec = op_prec(op.kind);
        if (prec == PREC_MIN) break;

        /* Handle assignment ops specially */
        if (prec == PREC_ASSIGN) {
            advance(c);
            Node* n = alloc_node(N_ASSIGN, left->line, left->col);
            n->as.assign.target = left;
            n->as.assign.val = parse_assignment(c);
            n->as.assign.op = op.kind;
            left = n;
            break;
        }

        advance(c);
        Node* right = parse_primary(c);

        /* Handle chained ops with higher precedence */
        while (1) {
            Token nxt = peek(c);
            Precedence nxt_prec = op_prec(nxt.kind);
            if (nxt_prec > prec && nxt_prec != PREC_ASSIGN) {
                advance(c);
                Node* n = alloc_node(N_BINARY, right->line, right->col);
                n->as.binary.l = right;
                n->as.binary.r = parse_primary(c);
                n->as.binary.op = nxt.kind;
                right = n;
            } else break;
        }

        Node* n = alloc_node(N_BINARY, left->line, left->col);
        n->as.binary.l = left;
        n->as.binary.r = right;
        n->as.binary.op = op.kind;
        left = n;
    }

    return left;
}

/* ─── Statement parser ────────────────────────────────────────────── */
static Node* parse_block(Compiler* c) {
    Node* block = alloc_node(N_BLOCK, peek(c).line, peek(c).col);
    block->as.block.stmts = calloc(4096, sizeof(Node*));
    block->as.block.count = 0;

    expect(c, D_COLON, "':'");
    expect(c, T_NEWLINE, "newline after ':'");
    expect(c, T_INDENT, "indented block");

    while (c->pos < c->ntokens) {
        Token t = peek(c);
        if (t.kind == T_DEDENT) break;
        if (t.kind == T_EOF) break;
        Node* stmt = parse_stmt(c);
        if (stmt) block->as.block.stmts[block->as.block.count++] = stmt;
    }

    expect(c, T_DEDENT, "dedent (end of block)");
    return block;
}

static Node* parse_struct_decl(Compiler* c) {
    Token name = advance(c);
    Node* n = alloc_node(N_STRUCT_DECL, name.line, name.col);
    n->as.struct_decl.name = strdup(name.str_val);
    n->as.struct_decl.packed = 0;

    /* Optional layout(packed) */
    if (peek(c).kind == K_LAYOUT) {
        advance(c);
        expect(c, D_LPAREN, "'('");
        if (match(c, K_PACKED)) n->as.struct_decl.packed = 1;
        expect(c, D_RPAREN, "')'");
    }

    expect(c, D_COLON, "':'");
    expect(c, T_NEWLINE, "newline");
    expect(c, T_INDENT, "indent");

    n->as.struct_decl.fields = calloc(256, sizeof(Node*));
    n->as.struct_decl.fcount = 0;

    while (c->pos < c->ntokens) {
        if (peek(c).kind == T_DEDENT) break;
        Token fn = advance(c);
        if (fn.kind != T_IDENT) { print_error(c, "expected field name"); break; }
        expect(c, D_COLON, "':'");
        Node* ft = parse_type(c);
        Node* fnode = alloc_node(N_LET, fn.line, fn.col);
        fnode->as.var.name = strdup(fn.str_val);
        fnode->as.var.type = ft;
        n->as.struct_decl.fields[n->as.struct_decl.fcount++] = fnode;
        match(c, T_NEWLINE);
    }

    expect(c, T_DEDENT, "dedent");
    return n;
}

static Node* parse_func_decl(Compiler* c) {
    int is_pub = match(c, K_PUB);
    Token name = advance(c); /* fn name already consumed or we need to */
    Node* n = alloc_node(N_FUNC, name.line, name.col);
    n->as.func.name = strdup(name.str_val);
    n->as.func.params = NULL;
    n->as.func.pcount = 0;
    n->as.func.ret_type = NULL;
    n->as.func.is_pub = is_pub;

    /* Parameters */
    if (peek(c).kind == D_LPAREN) {
        Node* params = parse_params(c);
        n->as.func.params = params->as.block.stmts;
        n->as.func.pcount = params->as.block.count;
        free(params);
    }

    /* Return type */
    if (peek(c).kind == D_ARROW) {
        advance(c);
        n->as.func.ret_type = parse_type(c);
    }

    /* Safety annotation */
    if (peek(c).kind == K_SAFETY) {
        n->as.func.safety = parse_safety(c);
    }

    /* Body */
    n->as.func.body = parse_block(c);

    /* Register function */
    if (c->nfuncs < MAX_FUNCS) {
        c->func_names[c->nfuncs][0] = 0;
        strncat(c->func_names[c->nfuncs], n->as.func.name, 63);
        c->func_nodes[c->nfuncs] = n;
        c->nfuncs++;
    }

    return n;
}

static Node* parse_typestate_decl(Compiler* c) {
    Token name = advance(c);
    Node* n = alloc_node(N_TYPESTATE_DECL, name.line, name.col);
    n->as.typestate.name = strdup(name.str_val);
    expect(c, D_COLON, "':'");
    
    n->as.typestate.states = calloc(64, sizeof(char*));
    n->as.typestate.scount = 0;
    
    while (peek(c).kind == T_IDENT) {
        Token s = advance(c);
        if (n->as.typestate.scount < 64)
            n->as.typestate.states[n->as.typestate.scount++] = strdup(s.str_val);
        if (peek(c).kind == D_COMMA) advance(c);
    }
    match(c, T_NEWLINE);
    return n;
}

/* Helper: reconstruct a line of text from tokens, matching the original source */
static int token_kind_to_char(TokenKind k) {
    switch (k) {
        case O_PLUS: return '+'; case O_MINUS: return '-';
        case O_STAR: return '*'; case O_SLASH: return '/';
        case O_PERCENT: return '%'; case O_EQ: return '=';
        case O_LT: return '<'; case O_GT: return '>';
        case O_BANG: return '!'; case O_AND: return '&';
        case O_OR: return '|'; case O_XOR: return '^';
        case O_NOT: return '~'; case D_COMMA: return ',';
        case D_COLON: return ':'; case D_SEMI: return ';';
        case D_DOT: return '.'; case D_LPAREN: return '(';
        case D_RPAREN: return ')'; case D_LBRACK: return '[';
        case D_RBRACK: return ']'; case D_LBRACE: return '{';
        case D_RBRACE: return '}'; case D_AT: return '@';
        case O_ASSIGN: return '='; default: return 0;
    }
}

static void append_token_str(char* buf, int* bi, int max, Token* t) {
    if (*bi >= max) return;
    if (t->kind == T_IDENT || (t->kind >= K_FN && t->kind <= K_PUB)) {
        *bi += snprintf(buf + *bi, max - *bi, "%s", t->str_val ? t->str_val : "?");
    } else if (t->kind == T_INT) {
        *bi += snprintf(buf + *bi, max - *bi, "%ld", (long)t->int_val);
    } else if (t->kind == T_STRING) {
        *bi += snprintf(buf + *bi, max - *bi, "\"%s\"", t->str_val ? t->str_val : "");
    } else {
        int c = token_kind_to_char(t->kind);
        if (c) { buf[(*bi)++] = c; buf[(*bi)] = 0; }
    }
}

static Node* parse_asm_block(Compiler* c) {
    advance(c); /* ':' */
    Node* n = alloc_node(N_ASM_BLOCK, peek(c).line, peek(c).col);
    n->as.asm_block.is_volatile = 0;
    n->as.asm_block.lines = calloc(4096, sizeof(char*));
    n->as.asm_block.count = 0;

    if (peek(c).kind == T_NEWLINE) {
        advance(c);
        if (peek(c).kind == T_INDENT) {
            advance(c);
            while (c->pos < c->ntokens) {
                if (peek(c).kind == T_DEDENT) break;
                if (peek(c).kind == T_EOF) break;
                int start = c->pos;
                while (c->pos < c->ntokens && peek(c).kind != T_NEWLINE && peek(c).kind != T_DEDENT)
                    c->pos++;
                if (c->pos > start) {
                    char line_buf[512];
                    int bi = 0;
                    for (int i = start; i < c->pos && bi < 490; i++) {
                        append_token_str(line_buf, &bi, 500, &c->tokens[i]);
                        if (bi < 499) {
                            int skip = 0;
                            char cc = token_kind_to_char(c->tokens[i].kind);
                            if (cc == '[' || cc == '(' || cc == ',') skip = 1;
                            if (i + 1 < c->pos) {
                                char nc = token_kind_to_char(c->tokens[i+1].kind);
                                if (nc == ']' || nc == ')' || nc == ':') skip = 1;
                            }
                            if (!skip) { line_buf[bi++] = ' '; line_buf[bi] = 0; }
                        }
                    }
                    line_buf[bi] = 0;
                    while (bi > 0 && line_buf[bi-1] == ' ') bi--;
                    line_buf[bi] = 0;
                    if (bi > 0) {
                        n->as.asm_block.lines[n->as.asm_block.count++] = strdup(line_buf);
                    }
                }
                match(c, T_NEWLINE);
            }
            expect(c, T_DEDENT, "dedent after asm block");
        }
    }
    return n;
}

static Node* parse_stmt(Compiler* c) {
    Token t = peek(c);
    int line = t.line, col = t.col;

    /* fn declaration */
    if (t.kind == K_PUB || t.kind == K_FN) {
        if (t.kind == K_PUB) advance(c);
        if (peek(c).kind == K_FN) advance(c);
        else { print_error(c, "expected 'fn' after 'pub'"); return NULL; }
        return parse_func_decl(c);
    }
    if (t.kind == K_FN) {
        advance(c);
        return parse_func_decl(c);
    }

    /* struct declaration */
    if (t.kind == K_STRUCT) {
        advance(c);
        return parse_struct_decl(c);
    }

    /* typestate declaration */
    if (t.kind == K_TYPESTATE) {
        advance(c);
        return parse_typestate_decl(c);
    }

    /* let/var/const */
    if (t.kind == K_LET || t.kind == K_VAR || t.kind == K_CONST) {
        int is_mut = (t.kind == K_VAR);
        int is_const = (t.kind == K_CONST);
        advance(c);
        Token name = advance(c);
        if (name.kind != T_IDENT) { print_error(c, "expected variable name"); return NULL; }
        
        Node* n = alloc_node(is_const ? N_CONST : (is_mut ? N_VAR : N_LET), line, col);
        n->as.var.name = strdup(name.str_val);
        n->as.var.is_mut = is_mut;
        n->as.var.is_const = is_const;
        n->as.var.type = NULL;
        n->as.var.init = NULL;

        /* Optional : type */
        if (peek(c).kind == D_COLON) {
            advance(c);
            n->as.var.type = parse_type(c);
        }

        /* Optional = expr */
        if (peek(c).kind == O_ASSIGN) {
            advance(c);
            n->as.var.init = parse_expr(c);
        }

        match(c, T_NEWLINE);
        return n;
    }

    /* import */
    if (t.kind == K_IMPORT) {
        advance(c);
        Node* n = alloc_node(N_IMPORT, line, col);
        Token path = advance(c);
        n->as.import.path = strdup(path.str_val);
        match(c, T_NEWLINE);
        return n;
    }

    /* return */
    if (t.kind == K_RETURN) {
        advance(c);
        Node* n = alloc_node(N_RETURN, line, col);
        if (peek(c).kind != T_NEWLINE && peek(c).kind != T_DEDENT && peek(c).kind != T_EOF) {
            n->as.ret.val = parse_expr(c);
        }
        match(c, T_NEWLINE);
        return n;
    }

    /* break / continue */
    if (t.kind == K_BREAK) { advance(c); match(c, T_NEWLINE); return alloc_node(N_BREAK, line, col); }
    if (t.kind == K_CONTINUE) { advance(c); match(c, T_NEWLINE); return alloc_node(N_CONTINUE, line, col); }

    /* Data label (:name "string") */
    if (t.kind == D_COLON) {
        advance(c);
        Token label = advance(c);
        if (label.kind != T_IDENT) { print_error(c, "expected label name"); return NULL; }
        Node* n = alloc_node(N_DATA, line, col);
        n->as.data.name = strdup(label.str_val);
        n->as.data.val = NULL;
        if (peek(c).kind == T_STRING) {
            Token s = advance(c);
            n->as.data.val = strdup(s.str_val);
        } else if (peek(c).kind == T_NEWLINE) {
            advance(c);
            if (peek(c).kind == T_STRING) {
                Token s = advance(c);
                n->as.data.val = strdup(s.str_val);
            }
        }
        match(c, T_NEWLINE);
        return n;
    }

    /* asm block */
    if (t.kind == K_ASM) {
        advance(c);
        if (match(c, K_VOLATILE)) (void)0;
        return parse_asm_block(c);
    }

    /* if / while / for */
    if (t.kind == K_IF) {
        advance(c);
        Node* n = alloc_node(N_IF, line, col);
        n->as.flow.cond = parse_expr(c);
        n->as.flow.body = parse_block(c);
        n->as.flow.else_body = NULL;
        if (peek(c).kind == K_ELSE) {
            advance(c);
            if (peek(c).kind == K_IF) {
                n->as.flow.else_body = parse_stmt(c);
            } else {
                n->as.flow.else_body = parse_block(c);
            }
        }
        return n;
    }

    if (t.kind == K_WHILE) {
        advance(c);
        Node* n = alloc_node(N_WHILE, line, col);
        n->as.flow.cond = parse_expr(c);
        n->as.flow.body = parse_block(c);
        return n;
    }

    if (t.kind == K_FOR) {
        advance(c);
        Node* n = alloc_node(N_FOR, line, col);
        Token var = advance(c);
        n->as.flow.loop_var = strdup(var.str_val);
        expect(c, K_IN, "'in'");
        n->as.flow.iter = parse_expr(c);
        n->as.flow.body = parse_block(c);
        return n;
    }

    /* Expression statement */
    Node* expr = parse_expr(c);
    match(c, T_NEWLINE);
    Node* es = alloc_node(N_BLOCK, line, col);
    es->as.block.stmts = calloc(2, sizeof(Node*));
    es->as.block.stmts[0] = expr;
    es->as.block.count = 1;
    return expr;
}

Node* parse_program(Compiler* c) {
    Node* prog = alloc_node(N_PROGRAM, 1, 1);
    prog->as.program.stmts = calloc(4096, sizeof(Node*));
    prog->as.program.count = 0;

    while (c->pos < c->ntokens) {
        Token t = peek(c);
        if (t.kind == T_EOF) break;
        if (t.kind == T_NEWLINE) { advance(c); continue; }
        if (t.kind == T_DEDENT) { advance(c); continue; }
        Node* stmt = parse_stmt(c);
        if (stmt) prog->as.program.stmts[prog->as.program.count++] = stmt;
    }

    return prog;
}
