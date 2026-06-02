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

/* ─── Virtual register helper ────────────────────────────────────── */
static int vreg() {
    static int n = 8;
    return n++;
}

/* ─── x86_64 encoder ─────────────────────────────────────────────── */
static uint8_t* code = NULL;
static int code_len = 0, code_cap = 0;

static void e1(uint8_t v) {
    if (code_len >= code_cap) {
        code_cap = code_cap ? code_cap * 2 : 262144;
        code = realloc(code, code_cap);
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

static void resolve_labels_arm64();

static void resolve_labels() {
    if (codegen_arch == TARGET_ARM64) {
        resolve_labels_arm64();
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
            case IR_MOVHI: case IR_LOAD: case IR_STORE: break;
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
            default:
                off += 4; break;  /* most ARM64 insns are 4 bytes */
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

        case IR_MOVHI: /* handled in x86_64 emit as MOVI with full 64-bit */
        case IR_LOAD: case IR_STORE: case IR_SAR: break; /* placeholder */
        case IR_LABEL: break;
        }
    }
}

/* ─── Translate IR to ARM64 machine code ──────────────────────────── */
static int a64_map(int r);
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

        case IR_MOVHI:
        case IR_LOAD:
        case IR_STORE:
        case IR_SHL:
        case IR_SHR:
        case IR_SAR:
        case IR_LABEL:
            break;
        }
    }
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
    case N_IDENT:
        ir_emit(IR_MOVI, r, 0, 0);
        return r;
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
            if (strcmp(fname, "__syscall") == 0 && n->as.call.acount >= 4) {
                int r0 = expr_to_ir(n->as.call.args[0]); /* syscall no */
                int r1 = expr_to_ir(n->as.call.args[1]); /* rdi */
                int r2_ = expr_to_ir(n->as.call.args[2]); /* rsi */
                int r3 = expr_to_ir(n->as.call.args[3]); /* rdx */
                ir_emit(IR_MOV, 7, r1, 0);   /* rdi */
                ir_emit(IR_MOV, 6, r2_, 0);  /* rsi */
                ir_emit(IR_MOV, 2, r3, 0);   /* rdx */
                ir_emit(IR_MOV, 0, r0, 0);   /* rax */
                ir_emit(IR_SYSCALL, 0, 0, 0);
                ir_emit(IR_MOV, r, 0, 0);
            } else {
                /* Regular function call: args in rdi, rsi, rdx, rcx, r8, r9 */
                int phys_regs[] = {7, 6, 2, 1, 8, 9};
                int nargs = n->as.call.acount;
                if (nargs > 6) nargs = 6;
                for (int i = 0; i < nargs; i++) {
                    int vr = expr_to_ir(n->as.call.args[i]);
                    ir_emit(IR_MOV, phys_regs[i], vr, 0);
                }
                /* Emit call with function name for resolution */
                int ci = ir_emit(IR_CALL, 0, 0, 0);
                ir[ci].name = strdup(fname != NULL ? fname : "");
                /* Return value from rax */
                ir_emit(IR_MOV, r, 0, 0);
            }
        }
        return r;
    case N_STRING: {
        int data_idx = ir_data_string(n->as.s_val ? n->as.s_val : "");
        ir_emit(IR_LEA, r, 0, data_idx);
        return r;
    }
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

    case N_LET: case N_VAR:
        if (stmt->as.var.init) {
            int r = expr_to_ir(stmt->as.var.init);
            ir_emit(IR_MOVI, r, 0, 0); /* consumed for effect */
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
void codegen(Compiler* c, Node* node) {
    ir_n = 0;
    code = NULL; code_len = 0; code_cap = 0;
    codegen_arch = c->target_arch;

    if (!node) return;

    /* First pass: collect all labels (function names, etc.) */
    /* Generate prologue for whole program */
    /* push rbp; mov rbp, rsp; sub rsp, frame_size */
    ir_emit(IR_PUSH, 5, 0, 0);       /* push rbp */
    ir_emit(IR_MOV, 5, 4, 0);         /* mov rbp, rsp */
    ir_emit(IR_SUB, 4, 0, 0);         /* will patch to sub rsp, N */

    switch (node->kind) {
    case N_PROGRAM:
        for (int i = 0; i < node->as.program.count; i++) {
            Node* s = node->as.program.stmts[i];
            if (s->kind == N_FUNC) {
                ir_label(s->as.func.name);
                /* Add function prologue */
                ir_emit(IR_PUSH, 5, 0, 0);
                ir_emit(IR_MOV, 5, 4, 0);
                ir_emit(IR_SUB, 4, 0, 0);
                stmt_to_ir(s->as.func.body);
                /* Function epilogue */
                ir_emit(IR_MOV, 4, 5, 0);
                ir_emit(IR_POP, 5, 0, 0);
                ir_emit(IR_RET, 0, 0, 0);
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
        ir_label(node->as.func.name);
        stmt_to_ir(node);
        break;
    default:
        stmt_to_ir(node);
        break;
    }

    /* Epilogue: mov rsp, rbp; pop rbp; ret */
    ir_emit(IR_MOV, 4, 5, 0);
    ir_emit(IR_POP, 5, 0, 0);
    ir_emit(IR_RET, 0, 0, 0);

    /* Translate to target machine code */
    if (codegen_arch == TARGET_ARM64) {
        ir_to_arm64();
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
    (void)c;
    /* Write as standard ELF64 x86_64 executable — no .comment, no section
       headers, no symbol tables. The output is indistinguishable from any
       other minimal ELF binary: no compiler branding, no custom metadata. */
    int total = code_len;
    int entry_point = 0;
    
    /* Find entry point: use 'main' label, or entry_override, or 0 */
    if (entry_override != 0) {
        entry_point = (int)(entry_override - 0x400000 - 128);
    } else {
        for (int i = 0; i < ir_n; i++) {
            if (ir[i].op == IR_LABEL && ir[i].name && strcmp(ir[i].name, "main") == 0) {
                entry_point = ir_to_offset[i];
                break;
            }
        }
    }

    int code_off = 128;

    /* ELF64 header — standard values throughout */
    uint8_t hdr[128];
    memset(hdr, 0, 128);
    hdr[0] = 0x7F; hdr[1] = 'E'; hdr[2] = 'L'; hdr[3] = 'F';
    hdr[4] = 2; hdr[5] = 1; hdr[6] = 1;
    if (codegen_arch == TARGET_ARM64) {
        hdr[16] = 2; hdr[17] = 0xB7;  /* ET_EXEC, AArch64 */
    } else {
        hdr[16] = 2; hdr[17] = 0x3E;  /* ET_EXEC, x86_64 */
    }
    *(uint32_t*)(hdr + 20) = 1;
    *(uint64_t*)(hdr + 24) = 0x400000 + code_off + entry_point;
    *(uint64_t*)(hdr + 32) = 64;
    hdr[52] = 64; hdr[53] = 0;
    hdr[54] = 56; hdr[55] = 0;
    hdr[56] = 1; hdr[57] = 0;

    /* Program header (PT_LOAD) — standard values */
    memset(hdr + 64, 0, 56);
    *(uint32_t*)(hdr + 64) = 1;
    *(uint32_t*)(hdr + 68) = 5;
    *(uint64_t*)(hdr + 72) = 0;
    *(uint64_t*)(hdr + 80) = 0x400000;
    *(uint64_t*)(hdr + 88) = 0x400000;
    *(uint64_t*)(hdr + 96) = code_off + total;
    *(uint64_t*)(hdr + 104) = code_off + total;
    *(uint64_t*)(hdr + 112) = 0x1000;

    FILE* f = fopen(outpath, "wb");
    if (!f) { fprintf(stderr, "error: cannot write %s\n", outpath); return; }
    fwrite(hdr, 1, 128, f);
    /* Zero pad — standard, no identifiable pattern */
    for (int i = 128; i < code_off; i++) fputc(0, f);
    fwrite(code, 1, code_len, f);
    fclose(f);
    chmod(outpath, 0755);
    fprintf(stderr, "forge: wrote %s (%d bytes, entry=0x%x)\n",
           outpath, code_off + code_len, 0x400000 + code_off + entry_point);
}
