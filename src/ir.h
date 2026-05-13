#ifndef IR_H
#define IR_H

#include <stdint.h>

// Virtual register (unlimited)
typedef int VReg; // %0, %1, %2, ...

// IR opcodes
typedef enum {
    IR_CONST,       // %dst = const imm
    IR_LOAD_STR,    // %dst = load_str "string"
    IR_COPY,        // %dst = %src
    IR_ADD,         // %dst = %a + %b
    IR_SUB,         // %dst = %a - %b
    IR_MUL,         // %dst = %a * %b
    IR_DIV,         // %dst = %a / %b
    IR_MOD,         // %dst = %a % %b
    IR_NEG,         // %dst = -%a
    IR_CMP_EQ,     // %dst = %a == %b
    IR_CMP_NE,     // %dst = %a != %b
    IR_CMP_LT,     // %dst = %a < %b
    IR_CMP_GT,     // %dst = %a > %b
    IR_CMP_LE,     // %dst = %a <= %b
    IR_CMP_GE,     // %dst = %a >= %b
    IR_AND,         // %dst = %a && %b (short-circuit)
    IR_OR,          // %dst = %a || %b (short-circuit)
    IR_NOT,         // %dst = !%a
    IR_CALL,        // %dst = call func(%args...)
    IR_ARG,         // set arg N = %src (before call)
    IR_RET,         // return %src
    IR_RET_VOID,    // return void
    IR_STORE,       // mem[%addr + offset] = %val
    IR_LOAD,        // %dst = mem[%addr + offset]
    IR_STORE_LOCAL, // local[slot] = %src
    IR_LOAD_LOCAL,  // %dst = local[slot]
    IR_BR,          // unconditional branch to label
    IR_BR_COND,    // branch if %cond != 0 to label_true, else label_false
    IR_LABEL,       // label:
    IR_PHI,         // %dst = phi(%a from L1, %b from L2) — SSA merge
} IROp;

// Single IR instruction
typedef struct {
    IROp op;
    VReg dst;       // destination virtual register (-1 if none)
    VReg a, b;      // source operands
    int64_t imm;    // immediate value (for IR_CONST, offsets)
    char *str;      // string (for IR_LOAD_STR, IR_CALL func name)
    int label;      // target label (for branches)
    int label2;     // false branch (for IR_BR_COND)
    // For IR_CALL
    VReg *args;
    int arg_count;
} IRInst;

// Basic block
typedef struct {
    int label;
    IRInst *insts;
    int count;
    int cap;
} IRBlock;

// Function IR
typedef struct {
    char *name;
    int param_count;
    int vreg_count;     // total virtual registers used
    int local_slots;    // stack slots needed (after regalloc)
    IRBlock *blocks;
    int block_count;
    int block_cap;
} IRFunc;

// Whole program IR
typedef struct {
    IRFunc *funcs;
    int func_count;
    int func_cap;
} IRProgram;

#endif
