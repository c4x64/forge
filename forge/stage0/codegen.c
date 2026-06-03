#include "forge.h"
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>

/* ─── CatArch-derived opcode IR ───────────────────────────────────── */
/* Internal representation: array of simple RISC-like ops.
   Each translates to 1-6 x86_64 bytes. */

typedef enum { IR_NOP,
    IR_MOVI, IR_MOV, IR_MOVHI, IR_LEA,
    IR_LOAD, IR_STORE, IR_PUSH, IR_POP,
    IR_ADD, IR_SUB, IR_IMUL, IR_AND, IR_OR, IR_XOR,
    IR_SHL, IR_SHR, IR_SAR,
    IR_CMP, IR_CMPI,
    IR_JMP, IR_JE, IR_JNE, IR_JL, IR_JLE, IR_JG, IR_JGE, IR_JZ, IR_JNZ,
    IR_CALL, IR_RET, IR_SYSCALL, IR_HALT,
    IR_LABEL, IR_DATA_BYTE, IR_DATA_STRING,
} IROp;

typedef struct {
    IROp op;
    int a, b;          /* register operands */
    int64_t imm;       /* immediate / target IR index for branches */
    char* name;        /* label/string name */
} IR;

#define MAX_IR 131072
static IR ir[MAX_IR];
static int ir_n = 0;
static int ir_emit(IROp op, int a, int b, int64_t imm) {
    if (ir_n >= MAX_IR) return -1;
    ir[ir_n].op = op; ir[ir_n].a = a; ir[ir_n].b = b;
    ir[ir_n].imm = imm; ir[ir_n].name = NULL;
    return ir_n++;
}

static int ir_label(const char* name) {
    int idx = ir_emit(IR_LABEL, 0, 0, 0);
    ir[idx].name = strdup(name);
    return idx;
}

static int ir_data_string(const char* s) {
    int idx = ir_emit(IR_DATA_STRING, 0, 0, 0);
    ir[idx].name = strdup(s ? s : "");
    return idx;
}

/* ─── Virtual register helper (cycle through 8-15) ──────────────── */
/* x86-64 has only 16 registers (0-15); vregs must stay in 8-15. */
static int vreg() {
    static int n = 8;
    int r = n;
    n = (n + 1) % 8 + 8;
    return r;
}

/* ─── x86_64 encoder ─────────────────────────────────────────────── */
static uint8_t* code = NULL;
static int code_len = 0, code_cap = 0;

static void e1(uint8_t v) {
    if (code_len >= code_cap) {
        code_cap = code_cap ? code_cap * 2 : 262144;
        uint8_t* new_code = realloc(code, code_cap);
        if (!new_code) { fprintf(stderr, "codegen error: out of memory\n"); return; }
        code = new_code;
    }
    code[code_len++] = v;
}

static void e4(uint32_t v) {
    e1(v); e1(v>>8); e1(v>>16); e1(v>>24);
}

static void e8(uint64_t v) {
    e4((uint32_t)v); e4((uint32_t)(v>>32));
}

static void rm(int mod, int reg, int rm_) {
    e1((mod<<6) | ((reg&7)<<3) | (rm_&7));
}

/* Macro to handle REX prefix */
static void rex(int w, int r, int x, int b) {
    uint8_t v = 0x40 | (w?8:0) | (r?4:0) | (x?2:0) | (b?1:0);
    if (v != 0x40) e1(v);
}

/* ─── Known label resolution ─────────────────────────────────────── */
static int* ir_to_offset = NULL; /* IR index → byte offset */
static int codegen_arch = TARGET_X86_64;

/* ─── Local variable table ────────────────────────────────────────── */
static char local_names[MAX_LOCALS][64];
static int  local_offsets[MAX_LOCALS];
static int  nlocals;
static int  current_frame_offset;

static int find_local(const char* name) {
    for (int i = 0; i < nlocals; i++)
        if (strcmp(local_names[i], name) == 0) return i;
    return -1;
}

static int local_size(Node* type) {
    if (type && type->kind == N_ARRAY_TYPE)
        return (int)(type->as.array_type.count * 8);
    return 8;
}

static void add_local(const char* name, Node* type) {
    if (nlocals >= MAX_LOCALS) return;
    int len = strlen(name);
    if (len > 63) len = 63;
    memcpy(local_names[nlocals], name, len);
    local_names[nlocals][len] = 0;
    local_offsets[nlocals] = -(current_frame_offset + local_size(type));
    current_frame_offset += local_size(type);
    nlocals++;
}

static void collect_locals(Node* node) {
    if (!node) return;
    switch (node->kind) {
    case N_PROGRAM:
        for (int i = 0; i < node->as.program.count; i++)
            collect_locals(node->as.program.stmts[i]);
        break;
    case N_FUNC:
        collect_locals(node->as.func.body);
        break;
    case N_BLOCK:
        for (int i = 0; i < node->as.block.count; i++)
            collect_locals(node->as.block.stmts[i]);
        break;
    case N_LET: case N_VAR: case N_CONST:
        add_local(node->as.var.name, node->as.var.type);
        break;
    default: break;
    }
}

static void resolve_labels_arm64();
static void resolve_labels_arm32();

static void resolve_labels() {
    if (codegen_arch == TARGET_ARM64) {
        resolve_labels_arm64();
        return;
    }
    if (codegen_arch == TARGET_ARM32) {
        resolve_labels_arm32();
        return;
    }
    /* x86_64: simulate codegen to compute offsets */
    ir_to_offset = calloc(ir_n, sizeof(int));
    int off = 0;
    for (int i = 0; i < ir_n; i++) {
        ir_to_offset[i] = off;
        switch (ir[i].op) {
            case IR_NOP:       off += 1; break;
            case IR_MOVI:      off += 7; break;
            case IR_MOV:       off += 3; break;
            case IR_LEA:       off += 7; break;
            case IR_ADD: case IR_SUB: case IR_AND: case IR_OR: case IR_XOR:
                               off += 3; break;
            case IR_IMUL:      off += 4; break;
            case IR_SHL: case IR_SHR: case IR_SAR:
                               off += 3; break;
            case IR_CMP:       off += 3; break;
            case IR_CMPI:      off += 7; break;
            case IR_JMP:       off += 5; break;
            case IR_JE: case IR_JNE: case IR_JL: case IR_JLE:
            case IR_JG: case IR_JGE: case IR_JZ: case IR_JNZ:
                               off += 6; break;
            case IR_CALL:      off += 5; break;
            case IR_RET:       off += 1; break;
            case IR_SYSCALL:   off += 2; break;
            case IR_HALT:      off += 1; break;
            case IR_PUSH:      off += 1; break;
            case IR_POP:       off += 1; break;
            case IR_LABEL:     break;
            case IR_DATA_BYTE: off += 1; break;
            case IR_DATA_STRING: off += (ir[i].name ? strlen(ir[i].name) : 0) + 1; break;
            case IR_LOAD: case IR_STORE: off += 7; break;
            case IR_MOVHI: break;
        }
    }
}

static void resolve_labels_arm64() {
    ir_to_offset = calloc(ir_n, sizeof(int));
    int off = 0;
    for (int i = 0; i < ir_n; i++) {
        ir_to_offset[i] = off;
        switch (ir[i].op) {
            case IR_LABEL: break;
            case IR_DATA_BYTE: off += 1; break;
            case IR_DATA_STRING: off += (ir[i].name ? strlen(ir[i].name) : 0) + 1; break;
            case IR_MOVI:
                /* worst case: MOVZ + 3×MOVK = 4 instructions × 4 bytes */
                off += 16;
                break;
            case IR_JMP:
            case IR_CALL:
                off += 4; break;  /* B/BL: 4 bytes */
            case IR_JE: case IR_JNE: case IR_JL: case IR_JLE:
            case IR_JG: case IR_JGE: case IR_JZ: case IR_JNZ:
                off += 4; break;  /* B.cond: 4 bytes */
            case IR_LOAD: case IR_STORE: off += 4; break;
            case IR_MOVHI: off += 4; break;
            case IR_SHL: case IR_SHR: case IR_SAR: off += 4; break;
            case IR_PUSH: case IR_POP: off += 4; break;
            default:
                off += 4; break;  /* most ARM64 insns are 4 bytes */
        }
    }
}

static void resolve_labels_arm32() {
    ir_to_offset = calloc(ir_n, sizeof(int));
    int off = 0;
    for (int i = 0; i < ir_n; i++) {
        ir_to_offset[i] = off;
        switch (ir[i].op) {
            case IR_LABEL: break;
            case IR_DATA_BYTE: off += 1; break;
            case IR_DATA_STRING: off += (ir[i].name ? strlen(ir[i].name) : 0) + 1; break;
            case IR_MOVI:
                /* MOVW + optional MOVT = 4 or 8 bytes */
                off += ((uint64_t)ir[i].imm >> 16) ? 8 : 4;
                break;
            default:
                off += 4; break;  /* all ARM32 insns are 4 bytes */
        }
    }
}

/* ─── Translate IR to x86_64 machine code ────────────────────────── */
static void ir_to_x86_64() {
    resolve_labels();

    code = NULL; code_len = 0; code_cap = 0;

    /* Write ELF entry point label */
    /* Entry is at the 'main' function */
    for (int i = 0; i < ir_n; i++) {
        IR* p = &ir[i];
        int off = ir_to_offset[i];
        int a = p->a, b = p->b, a7 = a & 7, b7 = b & 7;
        int rex_a = (a >= 8), rex_b = (b >= 8);

        switch (p->op) {
        case IR_NOP: e1(0x90); break;

        case IR_MOVI:
            /* mov r64, imm32  (REX.W + C7 /0 id) */
            /* Or: mov rax, imm64 for large values */
            if (a == 0 && (p->imm > 0x7FFFFFFF || p->imm < -2147483648LL)) {
                rex(1,0,0,0); e1(0xB8 | a7); e8(p->imm);
            } else {
                rex(1,0,0,rex_a); e1(0xC7); rm(3,0,a7); e4((uint32_t)p->imm);
            }
            break;

        case IR_MOV:
            /* mov r/m64, r64: REX.W + 89 /r */
            rex(1,rex_b,0,rex_a); e1(0x89); rm(3,b7,a7);
            break;

        case IR_LEA:
            if (p->name) {
                /* Forward reference: resolve by finding matching label */
                int target_idx = -1;
                for (int j = 0; j < ir_n && target_idx < 0; j++) {
                    if ((ir[j].op == IR_LABEL || ir[j].op == IR_DATA_STRING) &&
                        ir[j].name && strcmp(ir[j].name, p->name) == 0)
                        target_idx = j;
                }
                if (target_idx >= 0) {
                    int target_off = ir_to_offset[target_idx];
                    int disp = target_off - (off + 7);
                    rex(1,0,0, (a >= 8)); e1(0x8D); rm(0, a & 7, 5); e4(disp);
                } else {
                    /* Label not found, emit nop placeholder */
                    e1(0x90); e1(0x90); e1(0x90); e1(0x90);
                }
            } else if (p->imm < 0) {
                /* Absolute address: just use mov for bootstrap */
                rex(1,0,0,rex_a); e1(0xB8 | a7); e8(p->imm);
            } else {
                /* RIP-relative to data */
                int target = (int)p->imm;
                int disp = target - (off + 7);
                rex(1,0,0,rex_a); e1(0x8D); rm(0,a7,5); e4(disp);
            }
            break;

        case IR_ADD:  rex(1,rex_b,0,rex_a); e1(0x01); rm(3,b7,a7); break;
        case IR_SUB:  rex(1,rex_b,0,rex_a); e1(0x29); rm(3,b7,a7); break;
        case IR_AND:  rex(1,rex_b,0,rex_a); e1(0x21); rm(3,b7,a7); break;
        case IR_OR:   rex(1,rex_b,0,rex_a); e1(0x09); rm(3,b7,a7); break;
        case IR_XOR:  rex(1,rex_b,0,rex_a); e1(0x31); rm(3,b7,a7); break;
        case IR_IMUL: rex(1,rex_b,0,rex_a); e1(0x0F); e1(0xAF); rm(3,a7,b7); break;

        case IR_SHL:  rex(1,0,0,rex_a); e1(0xD3); rm(3,4,a7); break;
        case IR_SHR:  rex(1,0,0,rex_a); e1(0xD3); rm(3,5,a7); break;

        case IR_CMP:
            rex(1,rex_b,0,rex_a); e1(0x39); rm(3,b7,a7);
            break;

        case IR_CMPI:
            /* cmp r/m64, imm32 */
            rex(1,0,0,rex_a); e1(0x81); rm(3,7,a7); e4((uint32_t)p->imm);
            break;

        case IR_JMP:
        case IR_JE: case IR_JNE: case IR_JL: case IR_JLE:
        case IR_JG: case IR_JGE: case IR_JZ: case IR_JNZ: {
            int target_off = -1;
            if (p->name) {
                for (int j = 0; j < ir_n && target_off < 0; j++) {
                    if (ir[j].op == IR_LABEL && ir[j].name &&
                        strcmp(ir[j].name, p->name) == 0)
                        target_off = ir_to_offset[j];
                }
            } else if (p->imm >= 0 && p->imm < ir_n) {
                target_off = ir_to_offset[(int)p->imm];
            }
            if (target_off < 0) target_off = off;
            int rel;
            if (p->op == IR_JMP) {
                rel = target_off - (off + 5);
                e1(0xE9); e4(rel);
            } else {
                rel = target_off - (off + 6);
                switch (p->op) {
                    case IR_JE: case IR_JZ:
                        e1(0x0F); e1(0x84); break;
                    case IR_JNE: case IR_JNZ:
                        e1(0x0F); e1(0x85); break;
                    case IR_JL:  e1(0x0F); e1(0x8C); break;
                    case IR_JLE: e1(0x0F); e1(0x8E); break;
                    case IR_JG:  e1(0x0F); e1(0x8F); break;
                    case IR_JGE: e1(0x0F); e1(0x8D); break;
                    default: break;
                }
                e4(rel);
            }
            break;
        }

        case IR_CALL: {
            int target_off = -1;
            if (p->name) {
                for (int j = 0; j < ir_n && target_off < 0; j++) {
                    if (ir[j].op == IR_LABEL && ir[j].name &&
                        strcmp(ir[j].name, p->name) == 0)
                        target_off = ir_to_offset[j];
                }
            } else {
                target_off = ir_to_offset[(int)p->imm];
            }
            if (target_off >= 0) {
                int rel = target_off - (off + 5);
                e1(0xE8); e4(rel);
            }
            break;
        }
        case IR_RET:     e1(0xC3); break;
        case IR_SYSCALL: e1(0x0F); e1(0x05); break;
        case IR_HALT:    e1(0xF4); break;

        case IR_PUSH: e1(0x50 | a7); break;
        case IR_POP:  e1(0x58 | a7); break;

        case IR_DATA_STRING:
            /* Emit raw bytes */
            if (p->name) {
                int slen = strlen(p->name);
                memcpy(code + code_len, p->name, slen);
                code_len += slen;
                e1(0); /* null terminator */
            }
            break;

        case IR_DATA_BYTE:
            e1((uint8_t)p->imm);
            break;

        case IR_STORE:
            /* mov [base + disp32], src */
            rex(1, rex_b, 0, rex_a);
            e1(0x89);
            rm(2, b7, a7);
            e4((uint32_t)p->imm);
            break;

        case IR_LOAD:
            /* mov dst, [base + disp32] */
            rex(1, rex_a, 0, rex_b);
            e1(0x8B);
            rm(2, a7, b7);
            e4((uint32_t)p->imm);
            break;

        case IR_MOVHI: /* handled in x86_64 emit as MOVI with full 64-bit */
        case IR_SAR: break; /* placeholder */
        case IR_LABEL: break;
        }
    }
}

/* ─── Translate IR to ARM64 machine code ──────────────────────────── */
static int a64_map(int r);
static int arm32_map(int r);
static void ir_to_arm64() {
    resolve_labels();

    code = NULL; code_len = 0; code_cap = 0;

    for (int i = 0; i < ir_n; i++) {
        IR* p = &ir[i];
        int off = ir_to_offset[i];
        int rd = a64_map(p->a), rn = a64_map(p->b);
        int rd5 = rd & 0x1F, rn5 = rn & 0x1F;

        switch (p->op) {

        case IR_NOP:
            e4(0xD503201F);
            break;

        case IR_MOVI: {
            uint64_t val = (uint64_t)p->imm;
            if (val == 0) {
                e4(0xD2800000 | rd5);  /* MOVZ Xd, #0 */
            } else {
                int first = 1;
                for (int hw = 0; hw < 4; hw++) {
                    uint16_t chunk = (val >> (hw * 16)) & 0xFFFF;
                    if (chunk != 0 || first) {
                        if (first) {
                            e4(0xD2800000 | (hw << 21) | (chunk << 5) | rd5);
                            first = 0;
                        } else {
                            e4(0xF2800000 | (hw << 21) | (chunk << 5) | rd5);
                        }
                    }
                }
            }
            break;
        }

        case IR_MOV:
            /* MOV Xd, Xm = ORR Xd, XZR, Xm */
            e4(0xAA0003E0 | (rn5 << 16) | rd5);
            break;

        case IR_LEA:
            if (p->name) {
                int target_idx = -1;
                for (int j = 0; j < ir_n && target_idx < 0; j++) {
                    if ((ir[j].op == IR_LABEL || ir[j].op == IR_DATA_STRING) &&
                        ir[j].name && strcmp(ir[j].name, p->name) == 0)
                        target_idx = j;
                }
                if (target_idx >= 0) {
                    int target_off = ir_to_offset[target_idx];
                    int disp = target_off - off;
                    e4(0x10000000 | ((disp & 3) << 29) | (((disp >> 2) & 0x7FFFF) << 5) | rd5);
                } else {
                    e4(0xD503201F); /* nop placeholder */
                }
            } else {
                int target = (int)p->imm;
                int disp = target - off;
                e4(0x10000000 | ((disp & 3) << 29) | (((disp >> 2) & 0x7FFFF) << 5) | rd5);
            }
            break;

        case IR_ADD:
            e4(0x8B000000 | (rn5 << 16) | (rd5 << 5) | rd5);
            break;

        case IR_SUB:
            e4(0xCB000000 | (rn5 << 16) | (rd5 << 5) | rd5);
            break;

        case IR_AND:
            e4(0x8A000000 | (rn5 << 16) | (rd5 << 5) | rd5);
            break;

        case IR_OR:
            e4(0xAA000000 | (rn5 << 16) | (rd5 << 5) | rd5);
            break;

        case IR_XOR:
            e4(0xCA000000 | (rn5 << 16) | (rd5 << 5) | rd5);
            break;

        case IR_IMUL:
            e4(0x9B007C00 | (rn5 << 16) | (rd5 << 5) | rd5);
            break;

        case IR_CMP:
            e4(0xEB00001F | (rn5 << 16) | (rd5 << 5));
            break;

        case IR_CMPI:
            if (p->imm >= 0 && p->imm <= 4095) {
                e4(0xF100001F | ((int)p->imm << 10) | (rd5 << 5));
            } else {
                uint64_t val = (uint64_t)p->imm;
                int tmp = 16;
                if (val == 0) {
                    e4(0xD2800000 | (tmp << 5) | tmp);
                } else {
                    int first = 1;
                    for (int hw = 0; hw < 4; hw++) {
                        uint16_t chunk = (val >> (hw * 16)) & 0xFFFF;
                        if (chunk != 0 || first) {
                            if (first) {
                                e4(0xD2800000 | (hw << 21) | (chunk << 5) | tmp);
                                first = 0;
                            } else {
                                e4(0xF2800000 | (hw << 21) | (chunk << 5) | tmp);
                            }
                        }
                    }
                }
                e4(0xEB00001F | (tmp << 16) | (rd5 << 5));
            }
            break;

        case IR_JMP: {
            int target_off = -1;
            if (p->name) {
                for (int j = 0; j < ir_n && target_off < 0; j++)
                    if (ir[j].op == IR_LABEL && ir[j].name && strcmp(ir[j].name, p->name) == 0)
                        target_off = ir_to_offset[j];
            } else if (p->imm >= 0 && p->imm < ir_n) {
                target_off = ir_to_offset[(int)p->imm];
            }
            if (target_off < 0) target_off = off;
            int rel = (target_off - off) / 4;
            e4(0x14000000 | (rel & 0x3FFFFFF));
            break;
        }

        case IR_CALL: {
            int target_off = -1;
            if (p->name) {
                for (int j = 0; j < ir_n && target_off < 0; j++)
                    if (ir[j].op == IR_LABEL && ir[j].name && strcmp(ir[j].name, p->name) == 0)
                        target_off = ir_to_offset[j];
            } else {
                target_off = ir_to_offset[(int)p->imm];
            }
            if (target_off >= 0) {
                int rel = (target_off - off) / 4;
                e4(0x94000000 | (rel & 0x3FFFFFF));
            }
            break;
        }

        case IR_JE: case IR_JZ: {
            int target_off = -1;
            if (p->name) {
                for (int j = 0; j < ir_n && target_off < 0; j++)
                    if (ir[j].op == IR_LABEL && ir[j].name && strcmp(ir[j].name, p->name) == 0)
                        target_off = ir_to_offset[j];
            } else if (p->imm >= 0 && p->imm < ir_n) {
                target_off = ir_to_offset[(int)p->imm];
            }
            if (target_off < 0) target_off = off;
            int rel = (target_off - off) / 4;
            e4(0x54000000 | ((rel & 0x7FFFF) << 5) | 0); /* B.EQ */
            break;
        }

        case IR_JNE: case IR_JNZ: {
            int target_off = -1;
            if (p->name) {
                for (int j = 0; j < ir_n && target_off < 0; j++)
                    if (ir[j].op == IR_LABEL && ir[j].name && strcmp(ir[j].name, p->name) == 0)
                        target_off = ir_to_offset[j];
            } else if (p->imm >= 0 && p->imm < ir_n) {
                target_off = ir_to_offset[(int)p->imm];
            }
            if (target_off < 0) target_off = off;
            int rel = (target_off - off) / 4;
            e4(0x54000000 | ((rel & 0x7FFFF) << 5) | 1); /* B.NE */
            break;
        }

        case IR_JL: {
            int target_off = -1;
            if (p->name) {
                for (int j = 0; j < ir_n && target_off < 0; j++)
                    if (ir[j].op == IR_LABEL && ir[j].name && strcmp(ir[j].name, p->name) == 0)
                        target_off = ir_to_offset[j];
            } else if (p->imm >= 0 && p->imm < ir_n) {
                target_off = ir_to_offset[(int)p->imm];
            }
            if (target_off < 0) target_off = off;
            int rel = (target_off - off) / 4;
            e4(0x54000000 | ((rel & 0x7FFFF) << 5) | 0xB); /* B.LT */
            break;
        }

        case IR_JLE: {
            int target_off = -1;
            if (p->name) {
                for (int j = 0; j < ir_n && target_off < 0; j++)
                    if (ir[j].op == IR_LABEL && ir[j].name && strcmp(ir[j].name, p->name) == 0)
                        target_off = ir_to_offset[j];
            } else if (p->imm >= 0 && p->imm < ir_n) {
                target_off = ir_to_offset[(int)p->imm];
            }
            if (target_off < 0) target_off = off;
            int rel = (target_off - off) / 4;
            e4(0x54000000 | ((rel & 0x7FFFF) << 5) | 0xD); /* B.LE */
            break;
        }

        case IR_JG: {
            int target_off = -1;
            if (p->name) {
                for (int j = 0; j < ir_n && target_off < 0; j++)
                    if (ir[j].op == IR_LABEL && ir[j].name && strcmp(ir[j].name, p->name) == 0)
                        target_off = ir_to_offset[j];
            } else if (p->imm >= 0 && p->imm < ir_n) {
                target_off = ir_to_offset[(int)p->imm];
            }
            if (target_off < 0) target_off = off;
            int rel = (target_off - off) / 4;
            e4(0x54000000 | ((rel & 0x7FFFF) << 5) | 0xC); /* B.GT */
            break;
        }

        case IR_JGE: {
            int target_off = -1;
            if (p->name) {
                for (int j = 0; j < ir_n && target_off < 0; j++)
                    if (ir[j].op == IR_LABEL && ir[j].name && strcmp(ir[j].name, p->name) == 0)
                        target_off = ir_to_offset[j];
            } else if (p->imm >= 0 && p->imm < ir_n) {
                target_off = ir_to_offset[(int)p->imm];
            }
            if (target_off < 0) target_off = off;
            int rel = (target_off - off) / 4;
            e4(0x54000000 | ((rel & 0x7FFFF) << 5) | 0xA); /* B.GE */
            break;
        }

        case IR_RET:
            e4(0xD65F03C0);  /* RET */
            break;

        case IR_SYSCALL:
            e4(0xD4000001);  /* SVC #0 */
            break;

        case IR_HALT:
            e4(0xD503201F);
            break;

        case IR_PUSH:
            /* STR Xt, [SP, #-16]!  (pre-index, 64-bit) */
            /* Encoding: size=11 V=0 opcode=11100 opc=10 simm9=-16 Rn=SP Rt */
            e4(0xDC800000 | ((0x1F0 & 0x1FF) << 12) | (31 << 5) | rd5);
            break;

        case IR_POP:
            /* LDR Xt, [SP], #16  (post-index, 64-bit) */
            /* Encoding: size=11 V=0 opcode=11101 opc=01 simm9=16 Rn=SP Rt */
            e4(0xDD400000 | ((16 & 0x1FF) << 12) | (31 << 5) | rd5);
            break;

        case IR_DATA_STRING:
            if (p->name) {
                int slen = strlen(p->name);
                memcpy(code + code_len, p->name, slen);
                code_len += slen;
                e1(0);
            }
            break;

        case IR_DATA_BYTE:
            e1((uint8_t)p->imm);
            break;

        case IR_STORE:
            /* STR Xt, [Xn, #imm12*8] — unsigned offset */
            {
                int rt5 = a64_map(p->b) & 0x1F;
                int rn5 = a64_map(p->a) & 0x1F;
                int imm12 = ((int)p->imm >> 3) & 0xFFF;
                e4(0xF9000000 | (imm12 << 10) | (rn5 << 5) | rt5);
            }
            break;

        case IR_LOAD:
            /* LDR Xt, [Xn, #imm12*8] — unsigned offset */
            {
                int rt5 = a64_map(p->a) & 0x1F;
                int rn5 = a64_map(p->b) & 0x1F;
                int imm12 = ((int)p->imm >> 3) & 0xFFF;
                e4(0xF9400000 | (imm12 << 10) | (rn5 << 5) | rt5);
            }
            break;

        case IR_MOVHI: {
            /* MOVK Xd, #imm16, LSL #hw (set upper 16-bit half) */
            /* p->a = dest, p->imm = 16-bit value, p->b = hw (0-3) */
            uint64_t val = (uint64_t)p->imm;
            int hw = p->b & 3;
            uint16_t chunk = (val >> (hw * 16)) & 0xFFFF;
            e4(0xF2800000 | (hw << 21) | (chunk << 5) | rd5);
            break;
        }

        case IR_SHL: {
            /* LSL Xd, Xn, #shift = UBFM Xd, Xn, #((64-s)&63), #(63-s) */
            int s = (int)p->imm & 0x3F;
            int immr = (64 - s) & 0x3F;
            int imms = (63 - s) & 0x3F;
            e4(0xD3400000 | (immr << 16) | (imms << 10) | (rn5 << 5) | rd5);
            break;
        }

        case IR_SHR: {
            /* LSR Xd, Xn, #shift = UBFM Xd, Xn, #s, #63 */
            int s = (int)p->imm & 0x3F;
            e4(0xD3400000 | (s << 16) | (63 << 10) | (rn5 << 5) | rd5);
            break;
        }

        case IR_SAR: {
            /* ASR Xd, Xn, #shift = SBFM Xd, Xn, #s, #63 */
            int s = (int)p->imm & 0x3F;
            e4(0x93400000 | (s << 16) | (63 << 10) | (rn5 << 5) | rd5);
            break;
        }

        case IR_LABEL:
            break;
        }
    }
}

/* ─── Translate IR to ARM32 machine code ──────────────────────────── */
static void arm32_emit_mov_imm(int rd, uint32_t val) {
    if ((val & ~0xFF) == 0) {
        e4(0xE3A00000 | (rd << 12) | val);
    } else {
        e4(0xE3000000 | (rd << 12) | ((val & 0xF000) << 4) | (val & 0xFFF));
        if ((val >> 16) != 0) {
            uint32_t hi = (val >> 16) & 0xFFFF;
            e4(0xE3400000 | (rd << 12) | ((hi & 0xF000) << 4) | (hi & 0xFFF));
        }
    }
}

static void ir_to_arm32() {
    resolve_labels();
    code = NULL; code_len = 0; code_cap = 0;
    int skip_next = 0;

    for (int i = 0; i < ir_n; i++) {
        if (skip_next) { skip_next = 0; continue; }
        IR* p = &ir[i];
        int off = ir_to_offset[i];
        int ra = arm32_map(p->a), rb = arm32_map(p->b);
        int ra4 = ra & 0xF, rb4 = rb & 0xF;

        switch (p->op) {

        case IR_NOP:
            e4(0xE1A00000);  /* MOV R0, R0 */
            break;

        case IR_MOVI:
            arm32_emit_mov_imm(ra, (uint32_t)p->imm);
            break;

        case IR_MOV:
            /* MOV Rd, Rm */
            e4(0xE1A00000 | (ra4 << 12) | rb4);
            break;

        case IR_MOVHI:
            /* MOVK Rd, #imm16, LSL #hw (set upper 16-bit half) */
            {
                uint32_t val = (uint32_t)p->imm;
                int hw = p->b & 3;
                uint16_t chunk = (val >> (hw * 16)) & 0xFFFF;
                e4(0xE3400000 | (ra4 << 12) | ((chunk & 0xF000) << 4) | (chunk & 0xFFF));
            }
            break;

        case IR_LEA:
            /* ADR Rd, label — compute PC-relative address */
            if (p->name) {
                int target_idx = -1;
                for (int j = 0; j < ir_n && target_idx < 0; j++) {
                    if ((ir[j].op == IR_LABEL || ir[j].op == IR_DATA_STRING) &&
                        ir[j].name && strcmp(ir[j].name, p->name) == 0)
                        target_idx = j;
                }
                if (target_idx >= 0) {
                    int target_off = ir_to_offset[target_idx];
                    int disp = target_off - (off + 8);
                    if (disp >= 0 && disp <= 4095) {
                        e4(0xE28F0000 | (ra4 << 12) | disp);
                    } else if (disp < 0 && -disp <= 4095) {
                        e4(0xE24F0000 | (ra4 << 12) | (-disp));
                    } else {
                        /* fallback: load into temp, add to PC */
                        arm32_emit_mov_imm(12, (uint32_t)(disp < 0 ? -disp : disp));
                        if (disp >= 0)
                            e4(0xE08F000C | (ra4 << 12) | 12);  /* ADD Rd, PC, R12 */
                        else
                            e4(0xE04F000C | (ra4 << 12) | 12);  /* SUB Rd, PC, R12 */
                    }
                } else {
                    e4(0xE1A00000); /* nop */
                }
            } else {
                int target = (int)p->imm;
                int disp = target - (off + 8);
                if (disp >= 0 && disp <= 4095) {
                    e4(0xE28F0000 | (ra4 << 12) | disp);
                } else if (disp < 0 && -disp <= 4095) {
                    e4(0xE24F0000 | (ra4 << 12) | (-disp));
                } else {
                    arm32_emit_mov_imm(12, disp < 0 ? -disp : disp);
                    if (disp >= 0)
                        e4(0xE08F000C | (ra4 << 12) | 12);
                    else
                        e4(0xE04F000C | (ra4 << 12) | 12);
                }
            }
            break;

        case IR_ADD:
            /* ADD Rd, Rn, Rm  — rd += rm */
            e4(0xE0800000 | (ra4 << 16) | (ra4 << 12) | rb4);
            break;

        case IR_SUB:
            /* SUB Rd, Rn, Rm  — rd -= rm */
            e4(0xE0400000 | (ra4 << 16) | (ra4 << 12) | rb4);
            break;

        case IR_AND:
            /* AND Rd, Rn, Rm  — rd &= rm */
            e4(0xE0000000 | (ra4 << 16) | (ra4 << 12) | rb4);
            break;

        case IR_OR:
            /* ORR Rd, Rn, Rm  — rd |= rm */
            e4(0xE1800000 | (ra4 << 16) | (ra4 << 12) | rb4);
            break;

        case IR_XOR:
            /* EOR Rd, Rn, Rm  — rd ^= rm */
            e4(0xE0200000 | (ra4 << 16) | (ra4 << 12) | rb4);
            break;

        case IR_IMUL:
            /* MUL Rd, Rm, Rs  — rd = rd * rs */
            e4(0xE0000090 | (ra4 << 16) | (rb4 << 8) | ra4);
            break;

        case IR_SHL:
            /* LSL: MOV Rd, Rd, LSL #s */
            e4(0xE1A00000 | (ra4 << 12) | (((int)p->imm & 0x1F) << 7) | ra4);
            break;

        case IR_SHR:
            /* LSR: MOV Rd, Rd, LSR #s */
            e4(0xE1A00020 | (ra4 << 12) | (((int)p->imm & 0x1F) << 7) | ra4);
            break;

        case IR_SAR:
            /* ASR: MOV Rd, Rd, ASR #s */
            e4(0xE1A00040 | (ra4 << 12) | (((int)p->imm & 0x1F) << 7) | ra4);
            break;

        case IR_CMP:
            /* CMP Rn, Rm */
            e4(0xE1500000 | (ra4 << 16) | rb4);
            break;

        case IR_CMPI:
            /* CMP Rn, #imm — load into R12(IP) first */
            arm32_emit_mov_imm(12, (uint32_t)p->imm);
            e4(0xE1500000 | (ra4 << 16) | 12);
            break;

        case IR_JMP: {
            int target_off = -1;
            if (p->name) {
                for (int j = 0; j < ir_n && target_off < 0; j++)
                    if (ir[j].op == IR_LABEL && ir[j].name && strcmp(ir[j].name, p->name) == 0)
                        target_off = ir_to_offset[j];
            } else if (p->imm >= 0 && p->imm < ir_n) {
                target_off = ir_to_offset[(int)p->imm];
            }
            if (target_off < 0) target_off = off;
            int rel = (target_off - off - 8) / 4;
            e4(0xEA000000 | (rel & 0xFFFFFF));
            break;
        }

        case IR_CALL: {
            int target_off = -1;
            if (p->name) {
                for (int j = 0; j < ir_n && target_off < 0; j++)
                    if (ir[j].op == IR_LABEL && ir[j].name && strcmp(ir[j].name, p->name) == 0)
                        target_off = ir_to_offset[j];
            } else {
                target_off = ir_to_offset[(int)p->imm];
            }
            if (target_off >= 0) {
                int rel = (target_off - off - 8) / 4;
                e4(0xEB000000 | (rel & 0xFFFFFF));
            }
            break;
        }

        case IR_JE: case IR_JZ: {
            int target_off = -1;
            if (p->name) {
                for (int j = 0; j < ir_n && target_off < 0; j++)
                    if (ir[j].op == IR_LABEL && ir[j].name && strcmp(ir[j].name, p->name) == 0)
                        target_off = ir_to_offset[j];
            } else if (p->imm >= 0 && p->imm < ir_n) {
                target_off = ir_to_offset[(int)p->imm];
            }
            if (target_off < 0) target_off = off;
            int rel = (target_off - off - 8) / 4;
            e4(0x0A000000 | (rel & 0xFFFFFF));  /* B.EQ */
            break;
        }

        case IR_JNE: case IR_JNZ: {
            int target_off = -1;
            if (p->name) {
                for (int j = 0; j < ir_n && target_off < 0; j++)
                    if (ir[j].op == IR_LABEL && ir[j].name && strcmp(ir[j].name, p->name) == 0)
                        target_off = ir_to_offset[j];
            } else if (p->imm >= 0 && p->imm < ir_n) {
                target_off = ir_to_offset[(int)p->imm];
            }
            if (target_off < 0) target_off = off;
            int rel = (target_off - off - 8) / 4;
            e4(0x1A000000 | (rel & 0xFFFFFF));  /* B.NE */
            break;
        }

        case IR_JL: {
            int target_off = -1;
            if (p->name) {
                for (int j = 0; j < ir_n && target_off < 0; j++)
                    if (ir[j].op == IR_LABEL && ir[j].name && strcmp(ir[j].name, p->name) == 0)
                        target_off = ir_to_offset[j];
            } else if (p->imm >= 0 && p->imm < ir_n) {
                target_off = ir_to_offset[(int)p->imm];
            }
            if (target_off < 0) target_off = off;
            int rel = (target_off - off - 8) / 4;
            e4(0xBA000000 | (rel & 0xFFFFFF));  /* B.LT */
            break;
        }

        case IR_JLE: {
            int target_off = -1;
            if (p->name) {
                for (int j = 0; j < ir_n && target_off < 0; j++)
                    if (ir[j].op == IR_LABEL && ir[j].name && strcmp(ir[j].name, p->name) == 0)
                        target_off = ir_to_offset[j];
            } else if (p->imm >= 0 && p->imm < ir_n) {
                target_off = ir_to_offset[(int)p->imm];
            }
            if (target_off < 0) target_off = off;
            int rel = (target_off - off - 8) / 4;
            e4(0xDA000000 | (rel & 0xFFFFFF));  /* B.LE */
            break;
        }

        case IR_JG: {
            int target_off = -1;
            if (p->name) {
                for (int j = 0; j < ir_n && target_off < 0; j++)
                    if (ir[j].op == IR_LABEL && ir[j].name && strcmp(ir[j].name, p->name) == 0)
                        target_off = ir_to_offset[j];
            } else if (p->imm >= 0 && p->imm < ir_n) {
                target_off = ir_to_offset[(int)p->imm];
            }
            if (target_off < 0) target_off = off;
            int rel = (target_off - off - 8) / 4;
            e4(0xCA000000 | (rel & 0xFFFFFF));  /* B.GT */
            break;
        }

        case IR_JGE: {
            int target_off = -1;
            if (p->name) {
                for (int j = 0; j < ir_n && target_off < 0; j++)
                    if (ir[j].op == IR_LABEL && ir[j].name && strcmp(ir[j].name, p->name) == 0)
                        target_off = ir_to_offset[j];
            } else if (p->imm >= 0 && p->imm < ir_n) {
                target_off = ir_to_offset[(int)p->imm];
            }
            if (target_off < 0) target_off = off;
            int rel = (target_off - off - 8) / 4;
            e4(0xAA000000 | (rel & 0xFFFFFF));  /* B.GE */
            break;
        }

        case IR_RET:
            /* BX LR — return via link register */
            e4(0xE12FFF1E);
            break;

        case IR_SYSCALL:
            /* SVC #0 — ARM Linux syscall, expects sysno in R7 */
            e4(0xEF000000);
            break;

        case IR_HALT:
            e4(0xE1A00000);  /* infinite loop: B . */
            e4(0xEAFFFFFE);
            break;

        case IR_PUSH:
            if (ra4 == 11) {
                /* PUSH {R11, LR} */
                e4(0xE92D4C00);
            } else {
                e4(0xE92D0000 | (1 << ra4));
            }
            break;

        case IR_POP:
            if (ra4 == 11 && i + 1 < ir_n && ir[i+1].op == IR_RET) {
                /* POP {R11, PC} — return sequence */
                e4(0xE8BD8800);
                skip_next = 1;
            } else {
                e4(0xE8BD0000 | (1 << ra4));
            }
            break;

        case IR_DATA_STRING:
            if (p->name) {
                int slen = strlen(p->name);
                memcpy(code + code_len, p->name, slen);
                code_len += slen;
                e1(0);
            }
            break;

        case IR_DATA_BYTE:
            e1((uint8_t)p->imm);
            break;

        case IR_STORE:
            /* STR Rt, [Rn, #imm12] */
            if (p->imm >= 0 && p->imm <= 4095) {
                e4(0xE5800000 | (ra4 << 16) | (rb4 << 12) | ((int)p->imm & 0xFFF));
            } else if (p->imm < 0 && -p->imm <= 4095) {
                e4(0xE5000000 | (ra4 << 16) | (rb4 << 12) | ((-(int)p->imm) & 0xFFF));
            } else {
                /* large or negative offset: add to R12, then STR with reg offset */
                arm32_emit_mov_imm(12, (uint32_t)(int)p->imm);
                e4(0xE780000B | (ra4 << 16) | (rb4 << 12));  /* STR Rb, [Ra, R12] */
            }
            break;

        case IR_LOAD:
            /* LDR Rt, [Rn, #imm12] */
            if (p->imm >= 0 && p->imm <= 4095) {
                e4(0xE5900000 | (rb4 << 16) | (ra4 << 12) | ((int)p->imm & 0xFFF));
            } else if (p->imm < 0 && -p->imm <= 4095) {
                e4(0xE5100000 | (rb4 << 16) | (ra4 << 12) | ((-(int)p->imm) & 0xFFF));
            } else {
                arm32_emit_mov_imm(12, (uint32_t)(int)p->imm);
                e4(0xE790000B | (rb4 << 16) | (ra4 << 12));  /* LDR Ra, [Rb, R12] */
            }
            break;

        case IR_LABEL:
            break;
        }
    }
}

static int expr_to_ir(Node* n);  /* forward decl for emit_syscall */

/* ─── Syscall number tables ─────────────────────────────────────── */
static const long sys_exit[]   = {60, 93, 1};
static const long sys_read[]   = {0, 63, 3};
static const long sys_write[]  = {1, 64, 4};
static const long sys_open[]   = {2, 56, 5};
static const long sys_close[]  = {3, 57, 6};
static const long sys_mmap[]   = {9, 222, 192};
static const long sys_munmap[] = {11, 215, 91};
static const long sys_mprotect[] = {10, 226, 125};
static const long sys_brk[]    = {12, 214, 45};

/* Syscall number register per arch: [x86_64, arm64, arm32] */
static const int sysno_reg[3] = {0, 8, 7};
/* Argument registers per arch (up to 6 args): rax/rdi/rsi/rdx/r10/r8/r9 */
static const int sysarg_reg[3][6] = {
    {7, 6, 2, 10, 8, 9},   /* x86_64: rdi, rsi, rdx, r10, r8, r9 */
    {0, 1, 2, 3, 4, 5},    /* arm64:  x0, x1, x2, x3, x4, x5 */
    {0, 1, 2, 3, 4, 5},    /* arm32:  r0, r1, r2, r3, r4, r5 */
};

/* Emit a full syscall: sets up registers and emits SYSCALL */
static int emit_syscall(int sysno, Node** args, int nargs) {
    int sysno_regno = sysno_reg[codegen_arch];
    int r_sysno;
    if (sysno >= 0) {
        r_sysno = vreg();
        ir_emit(IR_MOVI, r_sysno, 0, sysno);
    } else {
        /* syscall number is an expression (from __syscall built-in) */
        r_sysno = expr_to_ir(args[0]);
        args++; nargs--;
    }
    int nr = nargs > 6 ? 6 : nargs;
    for (int i = 0; i < nr; i++) {
        int vr = expr_to_ir(args[i]);
        int pr = sysarg_reg[codegen_arch][i];
        ir_emit(IR_MOV, pr, vr, 0);
    }
    ir_emit(IR_MOV, sysno_regno, r_sysno, 0);
    ir_emit(IR_SYSCALL, 0, 0, 0);
    int r = vreg();
    ir_emit(IR_MOV, r, 0, 0);
    return r;
}

/* ─── Expression → IR ────────────────────────────────────────────── */
static int expr_to_ir(Node* n) {
    if (!n) return 0;
    int r = vreg();

    switch (n->kind) {
    case N_INT:
        ir_emit(IR_MOVI, r, 0, n->as.i_val);
        return r;
    case N_BOOL:
        ir_emit(IR_MOVI, r, 0, n->as.b_val ? 1 : 0);
        return r;
    case N_IDENT: {
        int idx = find_local(n->as.s_val);
        if (idx >= 0) {
            int base = vreg();
            ir_emit(IR_MOV, base, 5, 0);
            ir_emit(IR_LOAD, r, base, local_offsets[idx]);
        } else {
            ir_emit(IR_MOVI, r, 0, 0);
        }
        return r;
    }
    case N_BINARY: {
        int l = expr_to_ir(n->as.binary.l);
        int r2 = expr_to_ir(n->as.binary.r);
        switch (n->as.binary.op) {
            case O_PLUS:  ir_emit(IR_ADD, l, r2, 0); break;
            case O_MINUS: ir_emit(IR_SUB, l, r2, 0); break;
            case O_STAR:  ir_emit(IR_IMUL, l, r2, 0); break;
            case O_AND:   ir_emit(IR_AND, l, r2, 0); break;
            case O_OR:    ir_emit(IR_OR, l, r2, 0); break;
            case O_XOR:   ir_emit(IR_XOR, l, r2, 0); break;
            case O_EQ: {
                ir_emit(IR_CMP, l, r2, 0);
                int eq_lab = ir_label("__eq_skip");
                ir_emit(IR_JNE, 0, 0, eq_lab);
                ir_emit(IR_MOVI, r, 0, 1);
                int eq_ret = ir_label("__eq_ret");
                ir_emit(IR_JMP, 0, 0, eq_ret);
                ir[eq_lab].op = IR_LABEL;
                ir_emit(IR_MOVI, r, 0, 0);
                ir[eq_ret].op = IR_LABEL;
                break;
            }
            case O_LT: {
                ir_emit(IR_CMP, l, r2, 0);
                int lab = ir_label("__lt_set");
                ir_emit(IR_JL, 0, 0, lab);
                ir_emit(IR_MOVI, r, 0, 0);
                int lab2 = ir_label("__lt_done");
                ir_emit(IR_JMP, 0, 0, lab2);
                ir[lab].op = IR_LABEL;
                ir_emit(IR_MOVI, r, 0, 1);
                ir[lab2].op = IR_LABEL;
                break;
            }
            default: ir_emit(IR_MOV, r, l, 0); break;
        }
        return r;
    }
    case N_CALL:
        if (n->as.call.callee->kind == N_IDENT) {
            const char* fname = n->as.call.callee->as.s_val;

            /* ─── Built-in syscall wrappers ─────────────────────────── */
            struct { const char* name; const long* sysno; int min_args; } bwrap[] = {
                {"exit",     sys_exit,   1},
                {"read",     sys_read,   3},
                {"write",    sys_write,  3},
                {"open",     sys_open,   2},
                {"close",    sys_close,  1},
                {"mmap",     sys_mmap,   6},
                {"munmap",   sys_munmap, 2},
                {"mprotect", sys_mprotect, 3},
                {"brk",      sys_brk,    1},
                {NULL, NULL, 0}
            };
            int bwrap_match = 0;
            if (fname) {
                for (int bi = 0; bwrap[bi].name; bi++) {
                    if (strcmp(fname, bwrap[bi].name) == 0) {
                        r = emit_syscall((int)bwrap[bi].sysno[codegen_arch],
                                         n->as.call.args, n->as.call.acount);
                        bwrap_match = 1;
                        break;
                    }
                }
            }
            if (bwrap_match) {
                /* done — r already set by emit_syscall */
            } else if (fname && strcmp(fname, "__syscall") == 0) {
                r = emit_syscall(-1, n->as.call.args, n->as.call.acount);
            } else if (fname && (strcmp(fname, "dlopen") == 0 ||
                                 strcmp(fname, "dlsym") == 0 ||
                                 strcmp(fname, "dlclose") == 0)) {
                /* Dynamic linker functions — emit regular call */
                int phys_regs[] = {7, 6, 2, 1, 8, 9};
                int nargs = n->as.call.acount;
                if (nargs > 6) nargs = 6;
                for (int i = 0; i < nargs; i++) {
                    int vr = expr_to_ir(n->as.call.args[i]);
                    ir_emit(IR_MOV, phys_regs[i], vr, 0);
                }
                int ci = ir_emit(IR_CALL, 0, 0, 0);
                ir[ci].name = strdup(fname);
                ir_emit(IR_MOV, r, 0, 0);
            } else if (fname && strcmp(fname, "assert") == 0) {
                int nargs = n->as.call.acount;
                if (nargs >= 1) {
                    int cond = expr_to_ir(n->as.call.args[0]);
                    ir_emit(IR_CMPI, cond, 0, 0);
                    int pass = ir_label("__assert_pass");
                    ir_emit(IR_JNZ, 0, 0, pass);
                    if (nargs >= 2 && n->as.call.args[1]->kind == N_STRING) {
                        const char* msg = n->as.call.args[1]->as.s_val;
                        if (msg && *msg) {
                            int msglen = (int)strlen(msg);
                            int msg_idx = ir_data_string(msg);
                            int sv = vreg();
                            ir_emit(IR_MOVI, sv, 0, sys_write[codegen_arch]);
                            ir_emit(IR_MOV, sysno_reg[codegen_arch], sv, 0);
                            ir_emit(IR_MOVI, sv, 0, 2);
                            ir_emit(IR_MOV, sysarg_reg[codegen_arch][0], sv, 0);
                            ir_emit(IR_LEA, sysarg_reg[codegen_arch][1], 0, msg_idx);
                            ir_emit(IR_MOVI, sv, 0, msglen);
                            ir_emit(IR_MOV, sysarg_reg[codegen_arch][2], sv, 0);
                            ir_emit(IR_SYSCALL, 0, 0, 0);
                        }
                    }
                    {
                        int sv = vreg();
                        ir_emit(IR_MOVI, sv, 0, sys_exit[codegen_arch]);
                        ir_emit(IR_MOV, sysno_reg[codegen_arch], sv, 0);
                        ir_emit(IR_MOVI, sv, 0, 1);
                        ir_emit(IR_MOV, sysarg_reg[codegen_arch][0], sv, 0);
                        ir_emit(IR_SYSCALL, 0, 0, 0);
                        ir_emit(IR_HALT, 0, 0, 0);
                    }
                    ir[pass].op = IR_LABEL;
                }
                ir_emit(IR_MOVI, r, 0, 0);
            } else {
                /* Regular function call */
                int phys_regs[] = {7, 6, 2, 1, 8, 9};
                int nargs = n->as.call.acount;
                if (nargs > 6) nargs = 6;
                for (int i = 0; i < nargs; i++) {
                    int vr = expr_to_ir(n->as.call.args[i]);
                    ir_emit(IR_MOV, phys_regs[i], vr, 0);
                }
                int ci = ir_emit(IR_CALL, 0, 0, 0);
                ir[ci].name = strdup(fname ? fname : "");
                ir_emit(IR_MOV, r, 0, 0);
            }
        }
        return r;
    case N_STRING: {
        int data_idx = ir_data_string(n->as.s_val ? n->as.s_val : "");
        ir_emit(IR_LEA, r, 0, data_idx);
        return r;
    }
    case N_INDEX: {
        if (n->as.index_.obj &&
            n->as.index_.obj->kind == N_IDENT) {
            int local_idx = find_local(n->as.index_.obj->as.s_val);
            if (local_idx >= 0) {
                int idx_reg = expr_to_ir(n->as.index_.idx);
                // idx_reg *= 8 using 3×ADD
                ir_emit(IR_ADD, idx_reg, idx_reg, 0);  // *2
                ir_emit(IR_ADD, idx_reg, idx_reg, 0);  // *4
                ir_emit(IR_ADD, idx_reg, idx_reg, 0);  // *8
                int base = vreg();
                ir_emit(IR_MOV, base, 5, 0);  // base = rbp
                int off = vreg();
                ir_emit(IR_MOVI, off, 0, local_offsets[local_idx]);
                ir_emit(IR_ADD, base, off, 0);
                ir_emit(IR_ADD, base, idx_reg, 0);
                ir_emit(IR_LOAD, r, base, 0);
            }
        }
        return r;
    }
    case N_ASSIGN:
        if (n->as.assign.target &&
            n->as.assign.target->kind == N_IDENT) {
            int idx = find_local(n->as.assign.target->as.s_val);
            if (idx >= 0) {
                int val = expr_to_ir(n->as.assign.val);
                int base = vreg();
                ir_emit(IR_MOV, base, 5, 0);
                ir_emit(IR_STORE, base, val, local_offsets[idx]);
                ir_emit(IR_MOV, r, val, 0);
            }
        }
        if (n->as.assign.target &&
            n->as.assign.target->kind == N_INDEX) {
            Node* idx_node = n->as.assign.target;
            if (idx_node->as.index_.obj &&
                idx_node->as.index_.obj->kind == N_IDENT) {
                int local_idx = find_local(idx_node->as.index_.obj->as.s_val);
                if (local_idx >= 0) {
                    int idx_reg = expr_to_ir(idx_node->as.index_.idx);
                    ir_emit(IR_ADD, idx_reg, idx_reg, 0);  // *2
                    ir_emit(IR_ADD, idx_reg, idx_reg, 0);  // *4
                    ir_emit(IR_ADD, idx_reg, idx_reg, 0);  // *8
                    int val = expr_to_ir(n->as.assign.val);
                    ir_emit(IR_MOV, r, val, 0);
                    int base = vreg();
                    ir_emit(IR_MOV, base, 5, 0);
                    int off = vreg();
                    ir_emit(IR_MOVI, off, 0, local_offsets[local_idx]);
                    ir_emit(IR_ADD, base, off, 0);
                    ir_emit(IR_ADD, base, idx_reg, 0);
                    ir_emit(IR_STORE, base, val, 0);
                }
            }
        }
        return r;
    case N_ADDR_OF:
        if (n->as.unary.op &&
            n->as.unary.op->kind == N_IDENT) {
            int idx = find_local(n->as.unary.op->as.s_val);
            if (idx >= 0) {
                ir_emit(IR_MOV, r, 5, 0);
                int off = vreg();
                ir_emit(IR_MOVI, off, 0, local_offsets[idx]);
                ir_emit(IR_ADD, r, off, 0);
            }
        }
        return r;
    case N_DEREF: {
        int ptr = expr_to_ir(n->as.unary.op);
        ir_emit(IR_LOAD, r, ptr, 0);
        return r;
    }
    case N_CAST:
        /* cast is a compile-time no-op; just evaluate the inner expression */
        if (n->as.cast.expr) {
            r = expr_to_ir(n->as.cast.expr);
        } else {
            ir_emit(IR_MOVI, r, 0, 0);
        }
        return r;
    default:
        ir_emit(IR_MOVI, r, 0, 0);
        return r;
    }
}

/* ─── Statement → IR ─────────────────────────────────────────────── */
static void stmt_to_ir(Node* stmt) {
    if (!stmt) return;

    switch (stmt->kind) {
    case N_RETURN:
        if (stmt->as.ret.val) {
            int r = expr_to_ir(stmt->as.ret.val);
            ir_emit(IR_MOV, 0, r, 0);
        }
        ir_emit(IR_RET, 0, 0, 0);
        break;

    case N_IF: {
        int r = expr_to_ir(stmt->as.flow.cond);
        ir_emit(IR_CMPI, r, 0, 0);
        int else_lab = ir_label("__if_else");
        ir_emit(IR_JE, 0, 0, else_lab);
        stmt_to_ir(stmt->as.flow.body);
        int end_lab = ir_label("__if_end");
        ir_emit(IR_JMP, 0, 0, end_lab);
        ir[else_lab].op = IR_LABEL;
        if (stmt->as.flow.else_body)
            stmt_to_ir(stmt->as.flow.else_body);
        ir[end_lab].op = IR_LABEL;
        break;
    }

    case N_WHILE: {
        int start = ir_label("__while_start");
        int r = expr_to_ir(stmt->as.flow.cond);
        ir_emit(IR_CMPI, r, 0, 0);
        int end_lab = ir_label("__while_end");
        ir_emit(IR_JE, 0, 0, end_lab);
        stmt_to_ir(stmt->as.flow.body);
        ir_emit(IR_JMP, 0, 0, start);
        ir[end_lab].op = IR_LABEL;
        break;
    }

    case N_BLOCK:
        for (int i = 0; i < stmt->as.block.count; i++)
            stmt_to_ir(stmt->as.block.stmts[i]);
        break;

    case N_LET: case N_VAR: case N_CONST:
        if (stmt->as.var.init) {
            int idx = find_local(stmt->as.var.name);
            if (idx >= 0) {
                int r = expr_to_ir(stmt->as.var.init);
                int base = vreg();
                ir_emit(IR_MOV, base, 5, 0);
                ir_emit(IR_STORE, base, r, local_offsets[idx]);
            }
        }
        break;

    case N_ASSIGN:
        if (stmt->as.assign.target &&
            stmt->as.assign.target->kind == N_IDENT) {
            int idx = find_local(stmt->as.assign.target->as.s_val);
            if (idx >= 0) {
                int r = expr_to_ir(stmt->as.assign.val);
                int base = vreg();
                ir_emit(IR_MOV, base, 5, 0);
                ir_emit(IR_STORE, base, r, local_offsets[idx]);
            }
        }
        if (stmt->as.assign.target &&
            stmt->as.assign.target->kind == N_INDEX) {
            Node* idx_node = stmt->as.assign.target;
            if (idx_node->as.index_.obj &&
                idx_node->as.index_.obj->kind == N_IDENT) {
                int local_idx = find_local(idx_node->as.index_.obj->as.s_val);
                if (local_idx >= 0) {
                    int idx_reg = expr_to_ir(idx_node->as.index_.idx);
                    ir_emit(IR_ADD, idx_reg, idx_reg, 0);  // *2
                    ir_emit(IR_ADD, idx_reg, idx_reg, 0);  // *4
                    ir_emit(IR_ADD, idx_reg, idx_reg, 0);  // *8
                    int r = expr_to_ir(stmt->as.assign.val);
                    int base = vreg();
                    ir_emit(IR_MOV, base, 5, 0);
                    int off = vreg();
                    ir_emit(IR_MOVI, off, 0, local_offsets[local_idx]);
                    ir_emit(IR_ADD, base, off, 0);
                    ir_emit(IR_ADD, base, idx_reg, 0);
                    ir_emit(IR_STORE, base, r, 0);
                }
            }
        }
        break;

    case N_DATA:
        ir_label(stmt->as.data.name);
        if (stmt->as.data.val) {
            int di = ir_emit(IR_DATA_STRING, 0, 0, 0);
            ir[di].name = strdup(stmt->as.data.val);
        }
        break;

    case N_ASM_BLOCK: {
        for (int j = 0; j < stmt->as.asm_block.count; j++) {
            const char* line = stmt->as.asm_block.lines[j];
            if (!line || !*line) continue;
            /* Strip trailing comma from line copy */
            char linebuf[256];
            int lbi = 0;
            for (int k = 0; line[k] && lbi < 250; k++) {
                if (line[k] != ',') linebuf[lbi++] = line[k];
            }
            linebuf[lbi] = 0;
            
            char op[32], a1[64], a2[64];
            a1[0] = a2[0] = 0;
            int n = sscanf(linebuf, "%31s %63s %63s", op, a1, a2);
            if (n < 1) continue;

            if (strcmp(op, "mov") == 0 && n >= 2) {
                int rd = reg_encode(a1);
                int rs = reg_encode(a2);
                if (rs >= 0) {
                    ir_emit(IR_MOV, rd, rs, 0);
                } else {
                    char* end; long imm = strtol(a2, &end, 0);
                    if (*end == 0) ir_emit(IR_MOVI, rd, 0, imm);
                }
            } else if (strcmp(op, "add") == 0 && n >= 2) {
                int rd = reg_encode(a1), rs = reg_encode(a2);
                if (rs >= 0) { ir_emit(IR_ADD, rd, rs, 0); }
                else { char* e; long v = strtol(a2, &e, 0);
                       if (*e == 0) { int t = vreg(); ir_emit(IR_MOVI, t, 0, v);
                                      ir_emit(IR_ADD, rd, t, 0); } }
            } else if (strcmp(op, "sub") == 0 && n >= 2) {
                int rd = reg_encode(a1), rs = reg_encode(a2);
                if (rs >= 0) { ir_emit(IR_SUB, rd, rs, 0); }
                else { char* e; long v = strtol(a2, &e, 0);
                       if (*e == 0) { int t = vreg(); ir_emit(IR_MOVI, t, 0, v);
                                      ir_emit(IR_SUB, rd, t, 0); } }
            } else if (strcmp(op, "cmp") == 0 && n >= 2) {
                int rd = reg_encode(a1), rs = reg_encode(a2);
                if (rs >= 0) { ir_emit(IR_CMP, rd, rs, 0); }
                else { char* e; long v = strtol(a2, &e, 0);
                       if (*e == 0) { int t = vreg(); ir_emit(IR_MOVI, t, 0, v);
                                      ir_emit(IR_CMP, rd, t, 0); } }
            } else if (strcmp(op, "xor") == 0 && n >= 2) {
                ir_emit(IR_XOR, reg_encode(a1), reg_encode(a2), 0);
            } else if (strcmp(op, "syscall") == 0) {
                ir_emit(IR_SYSCALL, 0, 0, 0);
            } else if (strcmp(op, "ret") == 0) {
                ir_emit(IR_RET, 0, 0, 0);
            } else if (strcmp(op, "push") == 0 && n >= 2) {
                ir_emit(IR_PUSH, reg_encode(a1), 0, 0);
            } else if (strcmp(op, "pop") == 0 && n >= 2) {
                ir_emit(IR_POP, reg_encode(a1), 0, 0);
            } else if (strcmp(op, "int") == 0 && n >= 2) {
                /* Software interrupt: int imm8 */
                char* end; long v = strtol(a1, &end, 0);
                (void)v;
            } else if (op[0] == ':') {
                ir_label(op + 1);
            } else if (strlen(op) > 0 && op[strlen(op)-1] == ':') {
                int olen = strlen(op);
                char buf[64] = {0};
                strncpy(buf, op, olen - 1);
                ir_label(buf);
            } else if (strcmp(op, "jmp") == 0 && n >= 2) {
                int li = ir_label("__asm_jmp");
                ir[li].op = IR_JMP; /* reuse as forward ref */
                ir[li].name = strdup(a1);
            } else if (strcmp(op, "je") == 0 && n >= 2) {
                int li = ir_label("__asm_je");
                ir[li].op = IR_JE;
                ir[li].name = strdup(a1);
            } else if (strcmp(op, "jne") == 0 && n >= 2) {
                int li = ir_label("__asm_jne");
                ir[li].op = IR_JNE;
                ir[li].name = strdup(a1);
            } else if (strcmp(op, "jl") == 0 && n >= 2) {
                int li = ir_label("__asm_jl");
                ir[li].op = IR_JL; ir[li].name = strdup(a1);
            } else if (strcmp(op, "jle") == 0 && n >= 2) {
                int li = ir_label("__asm_jle");
                ir[li].op = IR_JLE; ir[li].name = strdup(a1);
            } else if (strcmp(op, "jg") == 0 && n >= 2) {
                int li = ir_label("__asm_jg");
                ir[li].op = IR_JG; ir[li].name = strdup(a1);
            } else if (strcmp(op, "jge") == 0 && n >= 2) {
                int li = ir_label("__asm_jge");
                ir[li].op = IR_JGE; ir[li].name = strdup(a1);
            } else if (strcmp(op, "jz") == 0 && n >= 2) {
                int li = ir_label("__asm_jz");
                ir[li].op = IR_JZ; ir[li].name = strdup(a1);
            } else if (strcmp(op, "jnz") == 0 && n >= 2) {
                int li = ir_label("__asm_jnz");
                ir[li].op = IR_JNZ; ir[li].name = strdup(a1);
            } else if (strcmp(op, "dec") == 0 && n >= 2) {
                int rd = reg_encode(a1);
                if (rd >= 0) { int t = vreg(); ir_emit(IR_MOVI, t, 0, 1);
                               ir_emit(IR_SUB, rd, t, 0); }
            } else if (strcmp(op, "inc") == 0 && n >= 2) {
                int rd = reg_encode(a1);
                if (rd >= 0) { int t = vreg(); ir_emit(IR_MOVI, t, 0, 1);
                               ir_emit(IR_ADD, rd, t, 0); }
            } else if (strcmp(op, "call") == 0 && n >= 2) {
                int ci = ir_emit(IR_CALL, 0, 0, 0);
                ir[ci].name = strdup(a1);
            } else if (strcmp(op, "lea") == 0 && n >= 2) {
                /* lea rd, [label] — parse the bracket */
                char label[64] = {0};
                if (a2[0] == '[') {
                    sscanf(a2, "[%63[^]]]", label);
                }
                /* Always use name-based forward reference. ir_to_offset is
                   not yet available during codegen (resolve_labels runs later). */
                int li = ir_emit(IR_LEA, reg_encode(a1), 0, 0);
                ir[li].name = strdup(label);
            }
        }
        break;
    }
    default:
        /* Expression used as statement */
        if (stmt->kind == N_CALL || stmt->kind == N_BINARY ||
            stmt->kind == N_INT || stmt->kind == N_IDENT ||
            stmt->kind == N_STRING || stmt->kind == N_BOOL) {
            expr_to_ir(stmt);
        }
        break;
    }
}

/* ─── Public API ──────────────────────────────────────────────────── */
static void emit_prologue(int frame_size) {
    ir_emit(IR_PUSH, 5, 0, 0);
    ir_emit(IR_MOV, 5, 4, 0);
    if (frame_size > 0) {
        int tmp = vreg();
        ir_emit(IR_MOVI, tmp, 0, frame_size);
        ir_emit(IR_SUB, 4, tmp, 0);
    }
}

static void emit_epilogue(void) {
    ir_emit(IR_MOV, 4, 5, 0);
    ir_emit(IR_POP, 5, 0, 0);
    ir_emit(IR_RET, 0, 0, 0);
}

void codegen(Compiler* c, Node* node) {
    ir_n = 0;
    code = NULL; code_len = 0; code_cap = 0;
    codegen_arch = c->target_arch;

    if (!node) return;

    switch (node->kind) {
    case N_PROGRAM:
        /* Emit _start entry point: call main then exit */
        ir_label("_start");
        {
            int ci = ir_emit(IR_CALL, 0, 0, 0);
            ir[ci].name = strdup("main");
        }
        if (codegen_arch == TARGET_ARM64) {
            ir_emit(IR_MOVI, 8, 0, 93);   /* x8 = 93 (ARM64 sys_exit) */
            ir_emit(IR_SYSCALL, 0, 0, 0);
        } else if (codegen_arch == TARGET_ARM32) {
            ir_emit(IR_MOV, 0, 0, 0);      /* R0 = exit code */
            ir_emit(IR_MOVI, 7, 0, 1);     /* R7 = 1 (sys_exit) */
            ir_emit(IR_SYSCALL, 0, 0, 0);
        } else {
            ir_emit(IR_MOV, 7, 0, 0);      /* rdi = rax (exit code) */
            ir_emit(IR_MOVI, 0, 0, 60);    /* rax = 60 (sys_exit) */
            ir_emit(IR_SYSCALL, 0, 0, 0);
        }

        for (int i = 0; i < node->as.program.count; i++) {
            Node* s = node->as.program.stmts[i];
            if (s->kind == N_FUNC) {
                nlocals = 0;
                current_frame_offset = 0;
                collect_locals(s);
                int frame_size = current_frame_offset;
                ir_label(s->as.func.name);
                emit_prologue(frame_size);
                stmt_to_ir(s->as.func.body);
                emit_epilogue();
            } else if (s->kind == N_DATA) {
                ir_label(s->as.data.name);
                if (s->as.data.val) {
                    int di = ir_emit(IR_DATA_STRING, 0, 0, 0);
                    ir[di].name = strdup(s->as.data.val);
                }
            }
        }
        break;
    case N_FUNC:
        ir_label("_start");
        {
            int ci = ir_emit(IR_CALL, 0, 0, 0);
            ir[ci].name = strdup("main");
        }
        if (codegen_arch == TARGET_ARM64) {
            ir_emit(IR_MOVI, 8, 0, 93);
            ir_emit(IR_SYSCALL, 0, 0, 0);
        } else if (codegen_arch == TARGET_ARM32) {
            ir_emit(IR_MOV, 0, 0, 0);
            ir_emit(IR_MOVI, 7, 0, 1);
            ir_emit(IR_SYSCALL, 0, 0, 0);
        } else {
            ir_emit(IR_MOV, 7, 0, 0);
            ir_emit(IR_MOVI, 0, 0, 60);
            ir_emit(IR_SYSCALL, 0, 0, 0);
        }
        nlocals = 0;
        current_frame_offset = 0;
        collect_locals(node);
        ir_label(node->as.func.name);
        emit_prologue(current_frame_offset);
        stmt_to_ir(node);
        emit_epilogue();
        break;
    default:
        emit_prologue(0);
        stmt_to_ir(node);
        emit_epilogue();
        break;
    }

    /* Translate to target machine code */
    if (codegen_arch == TARGET_ARM64) {
        ir_to_arm64();
    } else if (codegen_arch == TARGET_ARM32) {
        ir_to_arm32();
    } else {
        ir_to_x86_64();
    }
    c->code = code;
    c->code_len = code_len;
}

int reg_encode(const char* name) {
    if (!name) return -1;
    if (strcasecmp(name, "rax") == 0 || strcasecmp(name, "eax") == 0) return 0;
    if (strcasecmp(name, "rcx") == 0 || strcasecmp(name, "ecx") == 0) return 1;
    if (strcasecmp(name, "rdx") == 0 || strcasecmp(name, "edx") == 0) return 2;
    if (strcasecmp(name, "rbx") == 0 || strcasecmp(name, "ebx") == 0) return 3;
    if (strcasecmp(name, "rsp") == 0 || strcasecmp(name, "esp") == 0) return 4;
    if (strcasecmp(name, "rbp") == 0 || strcasecmp(name, "ebp") == 0) return 5;
    if (strcasecmp(name, "rsi") == 0 || strcasecmp(name, "esi") == 0) return 6;
    if (strcasecmp(name, "rdi") == 0 || strcasecmp(name, "edi") == 0) return 7;
    if (strcasecmp(name, "r8") == 0) return 8;  if (strcasecmp(name, "r9") == 0) return 9;
    if (strcasecmp(name, "r10") == 0) return 10; if (strcasecmp(name, "r11") == 0) return 11;
    if (strcasecmp(name, "r12") == 0) return 12; if (strcasecmp(name, "r13") == 0) return 13;
    if (strcasecmp(name, "r14") == 0) return 14; if (strcasecmp(name, "r15") == 0) return 15;
    return -1;
}

/* Map x86 virtual register numbers to ARM64 registers */
static int a64_map(int r) {
    if (r == 4) return 31;   /* rsp → sp */
    if (r == 5) return 29;   /* rbp → x29 (fp) */
    return r;                 /* x0-x15 map directly */
}

/* Map x86 virtual register numbers to ARM32 (ARM mode) registers.
   Fixed mapping:
     RAX(0)→R0, RCX(1)→R1, RDX(2)→R2, RBX(3)→R3,
     RSP(4)→R13(SP), RBP(5)→R11(FP),
     RSI(6)→R6, RDI(7)→R7,
     vregs 8-13 → R8,R9,R10,R4,R5,R12 (IP)
     vreg 14,15 → alias to R4,R5 */
static int arm32_map(int r) {
    switch (r) {
        case 0:  return 0;   /* RAX → R0 */
        case 1:  return 1;   /* RCX → R1 */
        case 2:  return 2;   /* RDX → R2 */
        case 3:  return 3;   /* RBX → R3 */
        case 4:  return 13;  /* RSP → R13(SP) */
        case 5:  return 11;  /* RBP → R11(FP) */
        case 6:  return 6;   /* RSI → R6 */
        case 7:  return 7;   /* RDI → R7 */
        case 8:  return 8;   /* vreg → R8 */
        case 9:  return 9;   /* vreg → R9 */
        case 10: return 10;  /* vreg → R10 */
        case 11: return 4;   /* vreg → R4 */
        case 12: return 5;   /* vreg → R5 */
        case 13: return 12;  /* vreg → R12(IP) */
        case 14: return 4;   /* vreg → R4 (alias) */
        case 15: return 5;   /* vreg → R5 (alias) */
        default: return r;
    }
}

void resolve_fixups(Compiler* c) {
    (void)c;
}

void emit_raw_binary(Compiler* c, const char* outpath, uint64_t entry_override) {
    (void)c;
    (void)entry_override;
    /* Raw flat binary — no ELF headers, code starts at byte 0 */
    /* Entry point is the first byte (offset 0) — suitable for kernels,
       bootloaders, and firmware images where the loader knows the address. */
    FILE* f = fopen(outpath, "wb");
    if (!f) { fprintf(stderr, "error: cannot write %s\n", outpath); return; }
    fwrite(code, 1, code_len, f);
    fclose(f);
    chmod(outpath, 0755);
    fprintf(stderr, "forge: wrote %s (%d bytes, raw binary)\n", outpath, code_len);
}

void emit_catarch_binary(Compiler* c, const char* outpath, uint64_t entry_override) {
    int total = code_len;
    int entry_point = 0;
    int is_shared = c ? c->shared : 0;
    
    /* Find entry point: prefer _start, fallback main, or entry_override */
    if (entry_override != 0) {
        entry_point = (int)(entry_override - 0x400000 - 128);
    } else if (!is_shared) {
        for (int i = 0; i < ir_n; i++) {
            if (ir[i].op == IR_LABEL && ir[i].name) {
                if (strcmp(ir[i].name, "_start") == 0) {
                    entry_point = ir_to_offset[i];
                    break;
                }
            }
        }
    }

    int code_off = 128;
    uint64_t base_addr = is_shared ? 0 : 0x400000;

    if (codegen_arch == TARGET_ARM32) {
        /* ELF32 header (52 bytes) */
        uint8_t hdr[128];
        memset(hdr, 0, 128);
        hdr[0] = 0x7F; hdr[1] = 'E'; hdr[2] = 'L'; hdr[3] = 'F';
        hdr[4] = 1; hdr[5] = 1; hdr[6] = 1;
        *(uint16_t*)(hdr + 16) = is_shared ? 3 : 2;
        *(uint16_t*)(hdr + 18) = 0x28;  /* EM_ARM */
        *(uint32_t*)(hdr + 20) = 1;
        *(uint32_t*)(hdr + 24) = (uint32_t)(base_addr + code_off + (int64_t)entry_point);
        *(uint32_t*)(hdr + 28) = 52;    /* e_phoff */
        *(uint32_t*)(hdr + 32) = 0;     /* e_shoff */
        *(uint32_t*)(hdr + 36) = 0;     /* e_flags */
        *(uint16_t*)(hdr + 40) = 52;    /* e_ehsize */
        *(uint16_t*)(hdr + 42) = 32;    /* e_phentsize */
        *(uint16_t*)(hdr + 44) = 1;     /* e_phnum */
        *(uint16_t*)(hdr + 46) = 40;    /* e_shentsize */
        *(uint16_t*)(hdr + 48) = 0;     /* e_shnum */
        *(uint16_t*)(hdr + 50) = 0;     /* e_shstrndx */

        /* Program header (PT_LOAD, 32 bytes) */
        *(uint32_t*)(hdr + 52) = 1;     /* p_type */
        *(uint32_t*)(hdr + 56) = 0;     /* p_offset */
        *(uint32_t*)(hdr + 60) = (uint32_t)base_addr;  /* p_vaddr */
        *(uint32_t*)(hdr + 64) = (uint32_t)base_addr;  /* p_paddr */
        *(uint32_t*)(hdr + 68) = code_off + total;     /* p_filesz */
        *(uint32_t*)(hdr + 72) = code_off + total;     /* p_memsz */
        *(uint32_t*)(hdr + 76) = 5;      /* p_flags = RX */
        *(uint32_t*)(hdr + 80) = 0x1000; /* p_align */

        FILE* f = fopen(outpath, "wb");
        if (!f) { fprintf(stderr, "error: cannot write %s\n", outpath); return; }
        fwrite(hdr, 1, 128, f);
        for (int i = 128; i < code_off; i++) fputc(0, f);
        fwrite(code, 1, code_len, f);
        fclose(f);
        chmod(outpath, 0755);
        fprintf(stderr, "forge: wrote %s (%d bytes, %s)\n",
               outpath, code_off + code_len,
               is_shared ? "shared object" : "executable");
        return;
    }

    /* ELF64 header */
    uint8_t hdr[128];
    memset(hdr, 0, 128);
    hdr[0] = 0x7F; hdr[1] = 'E'; hdr[2] = 'L'; hdr[3] = 'F';
    hdr[4] = 2; hdr[5] = 1; hdr[6] = 1;
    *(uint16_t*)(hdr + 16) = is_shared ? 3 : 2;  /* ET_DYN or ET_EXEC */
    if (codegen_arch == TARGET_ARM64) {
        *(uint16_t*)(hdr + 18) = 0xB7;  /* EM_AARCH64 */
    } else {
        *(uint16_t*)(hdr + 18) = 0x3E;  /* EM_X86_64 */
    }
    *(uint32_t*)(hdr + 20) = 1;
    *(uint64_t*)(hdr + 24) = base_addr + code_off + entry_point;
    *(uint64_t*)(hdr + 32) = 64;
    hdr[52] = 64; hdr[53] = 0;
    hdr[54] = 56; hdr[55] = 0;
    hdr[56] = 1; hdr[57] = 0;

    /* Program header (PT_LOAD) */
    memset(hdr + 64, 0, 56);
    *(uint32_t*)(hdr + 64) = 1;
    *(uint32_t*)(hdr + 68) = 5;
    *(uint64_t*)(hdr + 72) = 0;
    *(uint64_t*)(hdr + 80) = base_addr;
    *(uint64_t*)(hdr + 88) = base_addr;
    *(uint64_t*)(hdr + 96) = code_off + total;
    *(uint64_t*)(hdr + 104) = code_off + total;
    *(uint64_t*)(hdr + 112) = 0x1000;

    FILE* f = fopen(outpath, "wb");
    if (!f) { fprintf(stderr, "error: cannot write %s\n", outpath); return; }
    fwrite(hdr, 1, 128, f);
    for (int i = 128; i < code_off; i++) fputc(0, f);
    fwrite(code, 1, code_len, f);
    fclose(f);
    chmod(outpath, 0755);
    fprintf(stderr, "forge: wrote %s (%d bytes, %s)\n",
           outpath, code_off + code_len,
           is_shared ? "shared object" : "executable");
}
