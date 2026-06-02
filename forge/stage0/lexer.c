#include "forge.h"
#include <ctype.h>

static int is_ident_start(char c) {
    return isalpha((unsigned char)c) || c == '_';
}

static int is_ident_cont(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static Token make_token(TokenKind k, int line, int col) {
    Token t = { .kind = k, .line = line, .col = col };
    return t;
}
static Token make_int(int64_t v, int line, int col) {
    Token t = { .kind = T_INT, .int_val = v, .line = line, .col = col };
    return t;
}
static Token make_str(char* s, int line, int col) {
    Token t = { .kind = T_STRING, .str_val = s, .line = line, .col = col };
    return t;
}

static int word_to_keyword(const char* s, int len) {
    #define KW(s_, k_) if (strncmp(s, s_, len) == 0 && strlen(s_) == len) return k_
    KW("fn", K_FN); KW("let", K_LET); KW("var", K_VAR);
    KW("struct", K_STRUCT); KW("if", K_IF); KW("else", K_ELSE);
    KW("while", K_WHILE); KW("for", K_FOR); KW("return", K_RETURN);
    KW("asm", K_ASM); KW("volatile", K_VOLATILE);
    KW("safety", K_SAFETY); KW("pure", K_PURE); KW("hardware", K_HARDWARE);
    KW("bounded", K_BOUNDED); KW("unbounded", K_UNBOUNDED);
    KW("typestate", K_TYPESTATE); KW("true", K_TRUE); KW("false", K_FALSE);
    KW("in", K_IN); KW("break", K_BREAK); KW("continue", K_CONTINUE);
    KW("import", K_IMPORT); KW("cast", K_CAST);
    KW("layout", K_LAYOUT); KW("packed", K_PACKED);
    KW("sizeof", K_SIZE_OF); KW("addr_of", K_ADDR_OF);
    KW("undefined", K_UNDEFINED); KW("const", K_CONST);
    KW("pub", K_PUB);
    /* Types */
    KW("u8", T_U8); KW("u16", T_U16); KW("u32", T_U32); KW("u64", T_U64);
    KW("i8", T_I8); KW("i16", T_I16); KW("i32", T_I32); KW("i64", T_I64);
    KW("bool", T_BOOL); KW("void", T_VOID);
    KW("usize", T_USIZE);
    #undef KW
    return 0;
}

void lex(Compiler* c) {
    int line = 1, col = 1;
    int nt = 0;

    /* Indentation stack */
    int indent_stack[256];
    int indent_top = 0;
    indent_stack[0] = 0;

    int at_line_start = 1;

    while (c->pos < (int)c->src_len) {
        char ch = c->src[c->pos];
        int save_line = line, save_col = col;

        if (at_line_start) {
            at_line_start = 0;
            int level = 0;
            while (c->pos < (int)c->src_len && (c->src[c->pos] == ' ' || c->src[c->pos] == '\t')) {
                if (c->src[c->pos] == '\t') level += 8;
                else level++;
                c->pos++; col++;
            }
            /* Check if this is a blank/comment-only line */
            save_line = line; save_col = col;
            /* Update ch to the first content character */
            ch = (c->pos < (int)c->src_len) ? c->src[c->pos] : 0;
            /* Handle indent/dedent */
            if (level > indent_stack[indent_top]) {
                if (nt < MAX_TOKENS) {
                    Token t = make_token( T_INDENT, line, 1);
                    c->tokens[nt++] = t;
                }
                indent_stack[++indent_top] = level;
            } else {
                while (indent_top > 0 && level < indent_stack[indent_top]) {
                    if (nt < MAX_TOKENS) {
                        Token t = make_token( T_DEDENT, line, 1);
                        c->tokens[nt++] = t;
                    }
                    indent_top--;
                }
                if (level < 0) { /* blank line */
                    while (c->pos < (int)c->src_len && c->src[c->pos] != '\n') c->pos++;
                    ch = c->src[c->pos];
                }
            }
        }

        /* Skip whitespace (not at line start) */
        if (ch == ' ' || ch == '\t') {
            c->pos++; col++;
            continue;
        }

        /* Newline */
        if (ch == '\n') {
            c->pos++; line++; col = 1;
            if (nt < MAX_TOKENS) c->tokens[nt++] = make_token( T_NEWLINE, save_line, save_col);
            at_line_start = 1;
            continue;
        }

        /* Comments */
        if (ch == '#') {
            while (c->pos < (int)c->src_len && c->src[c->pos] != '\n') {
                c->pos++; col++;
            }
            continue;
        }

        /* String literals */
        if (ch == '"') {
            c->pos++; col++;
            int start = c->pos;
            while (c->pos < (int)c->src_len && c->src[c->pos] != '"') {
                if (c->src[c->pos] == '\\') { c->pos++; col++; }
                c->pos++; col++;
            }
            int end = c->pos;
            if (c->pos < (int)c->src_len) { c->pos++; col++; } /* skip closing */
            /* Properly handle escape sequences */
            char* buf = calloc(end - start + 1, 1);
            int bi = 0;
            for (int i = start; i < end; i++) {
                if (c->src[i] == '\\' && i + 1 < end) {
                    i++;
                    switch (c->src[i]) {
                        case 'n': buf[bi++] = '\n'; break;
                        case 't': buf[bi++] = '\t'; break;
                        case 'r': buf[bi++] = '\r'; break;
                        case '0': buf[bi++] = '\0'; break;
                        case '\\': buf[bi++] = '\\'; break;
                        case '"': buf[bi++] = '"'; break;
                        case 'x': {
                            if (i + 2 < end) {
                                int h1 = hex_val(c->src[i+1]);
                                int h2 = hex_val(c->src[i+2]);
                                if (h1 >= 0 && h2 >= 0) {
                                    buf[bi++] = (h1 << 4) | h2;
                                    i += 2;
                                }
                            }
                            break;
                        }
                        default: buf[bi++] = c->src[i]; break;
                    }
                } else {
                    buf[bi++] = c->src[i];
                }
            }
            buf[bi] = 0;
            if (nt < MAX_TOKENS) c->tokens[nt++] = make_str( buf, save_line, save_col);
            continue;
        }

        /* Numbers */
        if (isdigit((unsigned char)ch)) {
            if (ch == '0' && c->pos + 1 < (int)c->src_len) {
                char nx = c->src[c->pos + 1];
                if (nx == 'x' || nx == 'X') {
                    c->pos += 2; col += 2;
                    int64_t v = 0;
                    while (c->pos < (int)c->src_len) {
                        int h = hex_val(c->src[c->pos]);
                        if (h < 0) break;
                        v = (v << 4) | h;
                        c->pos++; col++;
                    }
                    if (nt < MAX_TOKENS) c->tokens[nt++] = make_int( v, save_line, save_col);
                    continue;
                }
                if (nx == 'b' || nx == 'B') {
                    c->pos += 2; col += 2;
                    int64_t v = 0;
                    while (c->pos < (int)c->src_len && (c->src[c->pos] == '0' || c->src[c->pos] == '1')) {
                        v = (v << 1) | (c->src[c->pos] - '0');
                        c->pos++; col++;
                    }
                    if (nt < MAX_TOKENS) c->tokens[nt++] = make_int( v, save_line, save_col);
                    continue;
                }
            }
            int64_t v = 0;
            int is_float = 0;
            while (c->pos < (int)c->src_len && (isdigit((unsigned char)c->src[c->pos]) || c->src[c->pos] == '.')) {
                if (c->src[c->pos] == '.') { is_float = 1; c->pos++; col++; break; }
                v = v * 10 + (c->src[c->pos] - '0');
                c->pos++; col++;
            }
            if (is_float) {
                double fv = (double)v;
                double div = 10.0;
                while (c->pos < (int)c->src_len && isdigit((unsigned char)c->src[c->pos])) {
                    fv += (c->src[c->pos] - '0') / div;
                    div *= 10.0;
                    c->pos++; col++;
                }
                Token t = { .kind = T_FLOAT, .float_val = fv, .line = save_line, .col = save_col };
                if (nt < MAX_TOKENS) c->tokens[nt++] = t;
            } else {
                if (nt < MAX_TOKENS) c->tokens[nt++] = make_int( v, save_line, save_col);
            }
            continue;
        }

        /* Identifiers and keywords */
        if (is_ident_start(ch)) {
            int start = c->pos;
            while (c->pos < (int)c->src_len && is_ident_cont(c->src[c->pos])) {
                c->pos++; col++;
            }
            int len = c->pos - start;
            char buf[128];
            if (len < 128) {
                memcpy(buf, c->src + start, len);
                buf[len] = 0;
            }
            int kw = word_to_keyword(buf, len);
            if (kw) {
                if (nt < MAX_TOKENS) c->tokens[nt++] = make_token( kw, save_line, save_col);
            } else {
                Token t = { .kind = T_IDENT, .str_val = strdup(buf), .line = save_line, .col = save_col };
                if (nt < MAX_TOKENS) c->tokens[nt++] = t;
            }
            continue;
        }

        /* Multi-char operators */
        if (ch == '=' && c->pos + 1 < (int)c->src_len && c->src[c->pos+1] == '=') {
            c->pos += 2; col += 2;
            if (nt < MAX_TOKENS) c->tokens[nt++] = make_token( O_EQ, save_line, save_col);
            continue;
        }
        if (ch == '!' && c->pos + 1 < (int)c->src_len && c->src[c->pos+1] == '=') {
            c->pos += 2; col += 2;
            if (nt < MAX_TOKENS) c->tokens[nt++] = make_token( O_NE, save_line, save_col);
            continue;
        }
        if (ch == '<' && c->pos + 1 < (int)c->src_len && c->src[c->pos+1] == '=') {
            c->pos += 2; col += 2;
            if (nt < MAX_TOKENS) c->tokens[nt++] = make_token( O_LE, save_line, save_col);
            continue;
        }
        if (ch == '>' && c->pos + 1 < (int)c->src_len && c->src[c->pos+1] == '=') {
            c->pos += 2; col += 2;
            if (nt < MAX_TOKENS) c->tokens[nt++] = make_token( O_GE, save_line, save_col);
            continue;
        }
        if (ch == '<' && c->pos + 1 < (int)c->src_len && c->src[c->pos+1] == '<') {
            c->pos += 2; col += 2;
            if (nt < MAX_TOKENS) c->tokens[nt++] = make_token( O_SHL, save_line, save_col);
            continue;
        }
        if (ch == '>' && c->pos + 1 < (int)c->src_len && c->src[c->pos+1] == '>') {
            c->pos += 2; col += 2;
            if (nt < MAX_TOKENS) c->tokens[nt++] = make_token( O_SHR, save_line, save_col);
            continue;
        }
        if (ch == '&' && c->pos + 1 < (int)c->src_len && c->src[c->pos+1] == '&') {
            c->pos += 2; col += 2;
            if (nt < MAX_TOKENS) c->tokens[nt++] = make_token( O_AND_AND, save_line, save_col);
            continue;
        }
        if (ch == '|' && c->pos + 1 < (int)c->src_len && c->src[c->pos+1] == '|') {
            c->pos += 2; col += 2;
            if (nt < MAX_TOKENS) c->tokens[nt++] = make_token( O_OR_OR, save_line, save_col);
            continue;
        }
        if (ch == '+' && c->pos + 1 < (int)c->src_len && c->src[c->pos+1] == '=') {
            c->pos += 2; col += 2;
            if (nt < MAX_TOKENS) c->tokens[nt++] = make_token( O_PLUS_EQ, save_line, save_col);
            continue;
        }
        if (ch == '-' && c->pos + 1 < (int)c->src_len && c->src[c->pos+1] == '>') {
            c->pos += 2; col += 2;
            if (nt < MAX_TOKENS) c->tokens[nt++] = make_token( D_ARROW, save_line, save_col);
            continue;
        }
        if (ch == '-' && c->pos + 1 < (int)c->src_len && c->src[c->pos+1] == '=') {
            c->pos += 2; col += 2;
            if (nt < MAX_TOKENS) c->tokens[nt++] = make_token( O_MINUS_EQ, save_line, save_col);
            continue;
        }
        if (ch == '*' && c->pos + 1 < (int)c->src_len && c->src[c->pos+1] == '=') {
            c->pos += 2; col += 2;
            if (nt < MAX_TOKENS) c->tokens[nt++] = make_token( O_STAR_EQ, save_line, save_col);
            continue;
        }

        /* Single char tokens */
        TokenKind single = 0;
        switch (ch) {
            case '(': single = D_LPAREN; break;
            case ')': single = D_RPAREN; break;
            case '[': single = D_LBRACK; break;
            case ']': single = D_RBRACK; break;
            case '{': single = D_LBRACE; break;
            case '}': single = D_RBRACE; break;
            case ',': single = D_COMMA; break;
            case ':': single = D_COLON; break;
            case ';': single = D_SEMI; break;
            case '.': single = D_DOT; break;
            case '@': single = D_AT; break;
            case '+': single = O_PLUS; break;
            case '-': single = O_MINUS; break;
            case '*': single = O_STAR; break;
            case '/': single = O_SLASH; break;
            case '%': single = O_PERCENT; break;
            case '=': single = O_ASSIGN; break;
            case '<': single = O_LT; break;
            case '>': single = O_GT; break;
            case '&': single = O_AND; break;
            case '|': single = O_OR; break;
            case '^': single = O_XOR; break;
            case '~': single = O_NOT; break;
            case '!': single = O_BANG; break;
        }
        if (single) {
            if (nt < MAX_TOKENS) c->tokens[nt++] = make_token( single, save_line, save_col);
            c->pos++; col++;
            continue;
        }

        /* Error */
        fprintf(stderr, "lex error: unexpected byte 0x%02x at %d:%d\n", (unsigned char)ch, line, col);
        c->pos++; col++;
    }

    /* Close all open indents */
    while (indent_top > 0) {
        if (nt < MAX_TOKENS) c->tokens[nt++] = make_token( T_DEDENT, line, 1);
        indent_top--;
    }

    if (nt < MAX_TOKENS) c->tokens[nt++] = make_token( T_EOF, line, 1);
    c->ntokens = nt;
}
