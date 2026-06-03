#include "forge.h"
#include <sys/stat.h>

void init_compiler(Compiler* c) {
    memset(c, 0, sizeof(Compiler));
    c->code = NULL; c->code_cap = 0; c->code_len = 0;
    c->pos = 0; c->ntokens = 0; c->nerrors = 0;
    c->nlabels = 0; c->nfuncs = 0; c->nstrings = 0; c->nfixups = 0;
    c->current_func = -1;
}

void free_node(Node* n) {
    if (!n) return;
    switch (n->kind) {
    case N_PROGRAM:
        for (int i = 0; i < n->as.program.count; i++) free_node(n->as.program.stmts[i]);
        free(n->as.program.stmts); break;
    case N_FUNC:
        free(n->as.func.name);
        for (int i = 0; i < n->as.func.pcount; i++) free_node(n->as.func.params[i]);
        free(n->as.func.params); free_node(n->as.func.ret_type);
        free_node(n->as.func.body); free_node(n->as.func.safety); break;
    case N_LET: case N_VAR: case N_CONST:
        free(n->as.var.name); free_node(n->as.var.type); free_node(n->as.var.init); break;
    case N_BLOCK:
        for (int i = 0; i < n->as.block.count; i++) free_node(n->as.block.stmts[i]);
        free(n->as.block.stmts); break;
    case N_RETURN: free_node(n->as.ret.val); break;
    case N_IF: case N_WHILE: case N_FOR:
        free_node(n->as.flow.cond); free_node(n->as.flow.body);
        free_node(n->as.flow.else_body); free(n->as.flow.loop_var);
        free_node(n->as.flow.init); free_node(n->as.flow.iter); break;
    case N_ASSIGN: free_node(n->as.assign.target); free_node(n->as.assign.val); break;
    case N_CALL:
        for (int i = 0; i < n->as.call.acount; i++) free_node(n->as.call.args[i]);
        free(n->as.call.args); free_node(n->as.call.callee); break;
    case N_INDEX: free_node(n->as.index_.obj); free_node(n->as.index_.idx); break;
    case N_BINARY: free_node(n->as.binary.l); free_node(n->as.binary.r); break;
    case N_UNARY: free_node(n->as.unary.op); break;
    case N_DATA:
        free(n->as.data.name); free(n->as.data.val); break;
    case N_ASM_BLOCK:
        for (int i = 0; i < n->as.asm_block.count; i++) free(n->as.asm_block.lines[i]);
        free(n->as.asm_block.lines); break;
    case N_IDENT: case N_STRING: free(n->as.s_val); break;
    case N_INT: case N_FLOAT: case N_BOOL: break;
    case N_SIZE_OF: case N_SAFETY_ANN: break;
    case N_DEREF: case N_ADDR_OF: free_node(n->as.unary.op); break;
    case N_ARRAY_TYPE: free_node(n->as.array_type.elem_type); break;
    case N_CAST: free_node(n->as.cast.type); free_node(n->as.cast.expr); break;
    case N_STRUCT_DECL:
        free(n->as.struct_decl.name);
        for (int i = 0; i < n->as.struct_decl.fcount; i++) free_node(n->as.struct_decl.fields[i]);
        free(n->as.struct_decl.fields); break;
    case N_TYPESTATE_DECL:
        free(n->as.typestate.name);
        for (int i = 0; i < n->as.typestate.scount; i++) free(n->as.typestate.states[i]);
        free(n->as.typestate.states); break;
    case N_IMPORT: free(n->as.import.path); break;
    case N_FIELD: free_node(n->as.field.obj); free(n->as.field.field); break;
    case N_ARRAY_LIT:
        free(n->as.program.stmts); break;
    default: break;
    }
    free(n);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: forge <input.forge> [-o <output>] [-m <arch>] [-k] [-e <entry>] [-q] [-shared]\n");
        fprintf(stderr, "  -o <file>   output path (default: a.out)\n");
        fprintf(stderr, "  -m <arch>   target architecture: x86_64, arm64, or arm32\n");
        fprintf(stderr, "  -k          kernel mode: raw flat binary (no ELF wrapper)\n");
        fprintf(stderr, "  -e <addr>   set entry point address (hex)\n");
        fprintf(stderr, "  -q          quiet mode (no diagnostic output)\n");
        fprintf(stderr, "  -shared     produce ELF shared object (.so) instead of executable\n");
        return 1;
    }

    const char* input = argv[1];
    const char* output = "a.out";
    int kernel_mode = 0;
    uint64_t entry_override = 0;
    int quiet = 0;
    int shared = 0;
    int target_arch = TARGET_X86_64;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) { output = argv[i+1]; i++; }
        else if (strcmp(argv[i], "-k") == 0) { kernel_mode = 1; }
        else if (strcmp(argv[i], "-e") == 0 && i + 1 < argc) { entry_override = strtoull(argv[i+1], NULL, 16); i++; }
        else if (strcmp(argv[i], "-q") == 0) { quiet = 1; }
        else if (strcmp(argv[i], "-shared") == 0) { shared = 1; }
        else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "x86_64") == 0) target_arch = TARGET_X86_64;
            else if (strcmp(argv[i], "arm64") == 0) target_arch = TARGET_ARM64;
            else if (strcmp(argv[i], "arm32") == 0) target_arch = TARGET_ARM32;
            else { fprintf(stderr, "error: unknown arch '%s' (use x86_64, arm64, or arm32)\n", argv[i]); return 1; }
        }
    }

    FILE* f = fopen(input, "rb");
    if (!f) { fprintf(stderr, "error: cannot open %s\n", input); return 1; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > MAX_SRC_LEN) { fclose(f); return 1; }

    Compiler comp;
    init_compiler(&comp);
    comp.target_arch = target_arch;
    comp.shared = shared;
    comp.src = malloc(len + 1);
    if (!comp.src) { fclose(f); fprintf(stderr, "error: out of memory\n"); return 1; }
    size_t rlen = fread(comp.src, 1, len, f);
    if (rlen != (size_t)len) { free(comp.src); fclose(f); fprintf(stderr, "error: failed to read %s\n", input); return 1; }
    comp.src[len] = 0;
    comp.src_len = len;
    fclose(f);
    comp.filename[sizeof(comp.filename) - 1] = 0;
    snprintf(comp.filename, sizeof(comp.filename), "%s", input);

    if (!quiet) fprintf(stderr, "forge: lexing...\n");
    lex(&comp);
    if (!quiet) fprintf(stderr, "forge: %d tokens\n", comp.ntokens);

    if (!quiet) fprintf(stderr, "forge: parsing...\n");
    comp.pos = 0;
    Node* ast = parse_program(&comp);
    if (comp.nerrors) {
        if (!quiet) fprintf(stderr, "forge: %d error(s), aborting\n", comp.nerrors);
        free_node(ast); free(comp.src); free(comp.code); return 1;
    }

    if (!quiet) fprintf(stderr, "forge: codegen...\n");
    codegen(&comp, ast);
    resolve_fixups(&comp);
    if (!quiet) fprintf(stderr, "forge: %d bytes generated\n", comp.code_len);

    if (!quiet) fprintf(stderr, "forge: writing %s...\n", output);
    if (kernel_mode) {
        emit_raw_binary(&comp, output, entry_override);
    } else {
        emit_catarch_binary(&comp, output, entry_override);
    }

    free_node(ast); free(comp.src); free(comp.code);
    return 0;
}
