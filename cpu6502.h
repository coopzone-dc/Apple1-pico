/*
 * cpu6502.h - compact NMOS 6502 core: documented opcodes, decimal mode,
 * cycle counted (page-cross and branch penalties included).
 *
 * The includer must define these BEFORE including this header:
 *     static uint32_t       cpu_clk;            (free-running cycle counter)
 *     static inline uint8_t bus_read(uint16_t addr);
 *     static inline void    bus_write(uint16_t addr, uint8_t val);
 *
 * Undocumented opcodes are treated as 2-cycle, 1-byte NOPs.
 * IRQ/NMI are not wired (the Apple I doesn't use them); BRK works.
 */
#ifndef CPU6502_H
#define CPU6502_H

#include <stdint.h>

#ifndef __not_in_flash_func
#define __not_in_flash_func(x) x
#endif

enum { F_C = 0x01, F_Z = 0x02, F_I = 0x04, F_D = 0x08,
       F_B = 0x10, F_U = 0x20, F_V = 0x40, F_N = 0x80 };

typedef struct { uint16_t pc; uint8_t a, x, y, sp, p; } cpu_regs_t;

static cpu_regs_t cpu;

/* Base cycle counts. Kept non-const so it lives in RAM, not XIP flash. */
static uint8_t cycle_tab[256] = {
    7,6,2,2,2,3,5,2,3,2,2,2,2,4,6,2,   2,5,2,2,2,4,6,2,2,4,2,2,2,4,7,2,
    6,6,2,2,3,3,5,2,4,2,2,2,4,4,6,2,   2,5,2,2,2,4,6,2,2,4,2,2,2,4,7,2,
    6,6,2,2,2,3,5,2,3,2,2,2,3,4,6,2,   2,5,2,2,2,4,6,2,2,4,2,2,2,4,7,2,
    6,6,2,2,2,3,5,2,4,2,2,2,5,4,6,2,   2,5,2,2,2,4,6,2,2,4,2,2,2,4,7,2,
    2,6,2,2,3,3,3,2,2,2,2,2,4,4,4,2,   2,6,2,2,4,4,4,2,2,5,2,2,2,5,2,2,
    2,6,2,2,3,3,3,2,2,2,2,2,4,4,4,2,   2,5,2,2,4,4,4,2,2,4,2,2,4,4,4,2,
    2,6,2,2,3,3,5,2,2,2,2,2,4,4,6,2,   2,5,2,2,2,4,6,2,2,4,2,2,2,4,7,2,
    2,6,2,2,3,3,5,2,2,2,2,2,4,4,6,2,   2,5,2,2,2,4,6,2,2,4,2,2,2,4,7,2,
};

static inline void do_adc(uint8_t *pa, uint8_t *pp, uint8_t v)
{
    unsigned a = *pa, p = *pp, c = p & F_C;
    unsigned bin = a + v + c;
    int dec = (p & F_D) != 0;
    p &= ~(unsigned)(F_N | F_V | F_Z | F_C);
    if (dec) {
        unsigned al = (a & 0x0F) + (v & 0x0F) + c;
        if (al >= 0x0A) al = ((al + 0x06) & 0x0F) + 0x10;
        unsigned r = (a & 0xF0) + (v & 0xF0) + al;
        if (r & 0x80) p |= F_N;
        if (~(a ^ v) & (a ^ r) & 0x80) p |= F_V;
        if (r >= 0xA0) r += 0x60;
        if (r >= 0x100) p |= F_C;
        if (!(bin & 0xFF)) p |= F_Z;
        *pa = (uint8_t)r;
    } else {
        if (bin & 0x80) p |= F_N;
        if (~(a ^ v) & (a ^ bin) & 0x80) p |= F_V;
        if (!(bin & 0xFF)) p |= F_Z;
        if (bin > 0xFF) p |= F_C;
        *pa = (uint8_t)bin;
    }
    *pp = (uint8_t)p;
}

static inline void do_sbc(uint8_t *pa, uint8_t *pp, uint8_t v)
{
    unsigned a = *pa, p = *pp, c = p & F_C;
    unsigned bin = a + (uint8_t)~v + c;
    int dec = (p & F_D) != 0;
    p &= ~(unsigned)(F_N | F_V | F_Z | F_C);
    if (bin & 0x80) p |= F_N;
    if ((a ^ v) & (a ^ bin) & 0x80) p |= F_V;
    if (!(bin & 0xFF)) p |= F_Z;
    if (bin > 0xFF) p |= F_C;
    if (dec) {
        int al = (int)(a & 0x0F) - (int)(v & 0x0F) + (int)c - 1;
        if (al < 0) al = ((al - 0x06) & 0x0F) - 0x10;
        int r = (int)(a & 0xF0) - (int)(v & 0xF0) + al;
        if (r < 0) r -= 0x60;
        *pa = (uint8_t)r;
    } else {
        *pa = (uint8_t)bin;
    }
    *pp = (uint8_t)p;
}

/* ---- helpers used inside cpu_run (they refer to its locals) ------------ */
#define RD(a)     bus_read((uint16_t)(a))
#define WR(a, v)  bus_write((uint16_t)(a), (uint8_t)(v))
#define RD16(a)   ((uint16_t)(RD(a) | (RD((uint16_t)((a) + 1)) << 8)))
#define PUSH(v)   do { WR(0x0100 | sp, (v)); sp--; } while (0)
#define POP()     (sp++, RD(0x0100 | sp))
#define NZ(val)   do { uint8_t _n = (uint8_t)(val); \
                       p = (uint8_t)((p & ~(F_N | F_Z)) | (_n & 0x80) | (_n ? 0 : F_Z)); } while (0)

/* addressing modes: result in 'ea'. The ...R variants add the page-cross cycle */
#define AM_IMM    ea = pc++
#define AM_ZP     ea = RD(pc++)
#define AM_ZPX    ea = (uint8_t)(RD(pc++) + x)
#define AM_ZPY    ea = (uint8_t)(RD(pc++) + y)
#define AM_ABS    do { ea = RD16(pc); pc += 2; } while (0)
#define AM_ABX    do { ea = RD16(pc); pc += 2; ea += x; } while (0)
#define AM_ABY    do { ea = RD16(pc); pc += 2; ea += y; } while (0)
#define AM_ABXR   do { base = RD16(pc); pc += 2; ea = base + x; cyc += !!((base ^ ea) & 0xFF00); } while (0)
#define AM_ABYR   do { base = RD16(pc); pc += 2; ea = base + y; cyc += !!((base ^ ea) & 0xFF00); } while (0)
#define AM_IZX    do { uint8_t _z = (uint8_t)(RD(pc++) + x); \
                       ea = RD(_z) | (RD((uint8_t)(_z + 1)) << 8); } while (0)
#define AM_IZY    do { uint8_t _z = RD(pc++); \
                       base = RD(_z) | (RD((uint8_t)(_z + 1)) << 8); ea = base + y; } while (0)
#define AM_IZYR   do { AM_IZY; cyc += !!((base ^ ea) & 0xFF00); } while (0)

/* operations */
#define I_LDA     do { a = RD(ea); NZ(a); } while (0)
#define I_LDX     do { x = RD(ea); NZ(x); } while (0)
#define I_LDY     do { y = RD(ea); NZ(y); } while (0)
#define I_ORA     do { a |= RD(ea); NZ(a); } while (0)
#define I_AND     do { a &= RD(ea); NZ(a); } while (0)
#define I_EOR     do { a ^= RD(ea); NZ(a); } while (0)
#define I_ADC     do { v = RD(ea); do_adc(&a, &p, v); } while (0)
#define I_SBC     do { v = RD(ea); do_sbc(&a, &p, v); } while (0)
#define I_CMP(r)  do { v = RD(ea); t = (uint8_t)((r) - v); \
                       p = (uint8_t)((p & ~(F_N | F_Z | F_C)) | ((r) >= v ? F_C : 0) | (t & 0x80) | (t ? 0 : F_Z)); } while (0)
#define I_BIT     do { v = RD(ea); p = (uint8_t)((p & ~(F_N | F_V | F_Z)) | (v & (F_N | F_V)) | ((a & v) ? 0 : F_Z)); } while (0)
#define I_STA     WR(ea, a)
#define I_STX     WR(ea, x)
#define I_STY     WR(ea, y)

#define I_ASL     do { v = RD(ea); p = (uint8_t)((p & ~F_C) | (v >> 7)); v = (uint8_t)(v << 1); WR(ea, v); NZ(v); } while (0)
#define I_LSR     do { v = RD(ea); p = (uint8_t)((p & ~F_C) | (v & 1)); v >>= 1; WR(ea, v); NZ(v); } while (0)
#define I_ROL     do { v = RD(ea); t = (unsigned)((v << 1) | (p & F_C)); p = (uint8_t)((p & ~F_C) | (v >> 7)); v = (uint8_t)t; WR(ea, v); NZ(v); } while (0)
#define I_ROR     do { v = RD(ea); t = (unsigned)((v >> 1) | ((p & F_C) << 7)); p = (uint8_t)((p & ~F_C) | (v & 1)); v = (uint8_t)t; WR(ea, v); NZ(v); } while (0)
#define I_INC     do { v = (uint8_t)(RD(ea) + 1); WR(ea, v); NZ(v); } while (0)
#define I_DEC     do { v = (uint8_t)(RD(ea) - 1); WR(ea, v); NZ(v); } while (0)

#define I_ASL_A   do { p = (uint8_t)((p & ~F_C) | (a >> 7)); a = (uint8_t)(a << 1); NZ(a); } while (0)
#define I_LSR_A   do { p = (uint8_t)((p & ~F_C) | (a & 1)); a >>= 1; NZ(a); } while (0)
#define I_ROL_A   do { t = (unsigned)((a << 1) | (p & F_C)); p = (uint8_t)((p & ~F_C) | (a >> 7)); a = (uint8_t)t; NZ(a); } while (0)
#define I_ROR_A   do { t = (unsigned)((a >> 1) | ((p & F_C) << 7)); p = (uint8_t)((p & ~F_C) | (a & 1)); a = (uint8_t)t; NZ(a); } while (0)

#define BRANCH(cond) do { int8_t _o = (int8_t)RD(pc++); \
                          if (cond) { uint16_t _np = (uint16_t)(pc + _o); \
                                      cyc += 1 + !!((pc ^ _np) & 0xFF00); pc = _np; } } while (0)

static void cpu_reset(void)
{
    cpu.sp = 0xFD;
    cpu.p  = F_I | F_U;
    cpu.pc = (uint16_t)(bus_read(0xFFFC) | (bus_read(0xFFFD) << 8));
}

/* Run for at least 'budget' cycles; returns the cycles actually used. */
static uint32_t __not_in_flash_func(cpu_run)(uint32_t budget)
{
    uint16_t pc = cpu.pc;
    uint8_t  a = cpu.a, x = cpu.x, y = cpu.y, sp = cpu.sp, p = cpu.p;
    const uint32_t start = cpu_clk;

    while ((uint32_t)(cpu_clk - start) < budget) {
        uint8_t  op  = RD(pc++);
        uint32_t cyc = cycle_tab[op];
        uint16_t ea = 0, base = 0;
        uint8_t  v;
        unsigned t;

        switch (op) {
        /* 00 */ case 0x00: pc++; PUSH(pc >> 8); PUSH(pc & 0xFF); PUSH(p | F_B | F_U);
                            p |= F_I; pc = RD16(0xFFFE); break;
        case 0x01: AM_IZX;  I_ORA; break;
        case 0x05: AM_ZP;   I_ORA; break;
        case 0x06: AM_ZP;   I_ASL; break;
        case 0x08: PUSH(p | F_B | F_U); break;
        case 0x09: AM_IMM;  I_ORA; break;
        case 0x0A: I_ASL_A; break;
        case 0x0D: AM_ABS;  I_ORA; break;
        case 0x0E: AM_ABS;  I_ASL; break;
        /* 10 */
        case 0x10: BRANCH(!(p & F_N)); break;
        case 0x11: AM_IZYR; I_ORA; break;
        case 0x15: AM_ZPX;  I_ORA; break;
        case 0x16: AM_ZPX;  I_ASL; break;
        case 0x18: p &= ~F_C; break;
        case 0x19: AM_ABYR; I_ORA; break;
        case 0x1D: AM_ABXR; I_ORA; break;
        case 0x1E: AM_ABX;  I_ASL; break;
        /* 20 */
        case 0x20: ea = RD16(pc); pc++; PUSH(pc >> 8); PUSH(pc & 0xFF); pc = ea; break;
        case 0x21: AM_IZX;  I_AND; break;
        case 0x24: AM_ZP;   I_BIT; break;
        case 0x25: AM_ZP;   I_AND; break;
        case 0x26: AM_ZP;   I_ROL; break;
        case 0x28: v = POP(); p = (uint8_t)((v & ~F_B) | F_U); break;
        case 0x29: AM_IMM;  I_AND; break;
        case 0x2A: I_ROL_A; break;
        case 0x2C: AM_ABS;  I_BIT; break;
        case 0x2D: AM_ABS;  I_AND; break;
        case 0x2E: AM_ABS;  I_ROL; break;
        /* 30 */
        case 0x30: BRANCH(p & F_N); break;
        case 0x31: AM_IZYR; I_AND; break;
        case 0x35: AM_ZPX;  I_AND; break;
        case 0x36: AM_ZPX;  I_ROL; break;
        case 0x38: p |= F_C; break;
        case 0x39: AM_ABYR; I_AND; break;
        case 0x3D: AM_ABXR; I_AND; break;
        case 0x3E: AM_ABX;  I_ROL; break;
        /* 40 */
        case 0x40: v = POP(); p = (uint8_t)((v & ~F_B) | F_U);
                   v = POP(); pc = v; v = POP(); pc |= (uint16_t)(v << 8); break;
        case 0x41: AM_IZX;  I_EOR; break;
        case 0x45: AM_ZP;   I_EOR; break;
        case 0x46: AM_ZP;   I_LSR; break;
        case 0x48: PUSH(a); break;
        case 0x49: AM_IMM;  I_EOR; break;
        case 0x4A: I_LSR_A; break;
        case 0x4C: pc = RD16(pc); break;
        case 0x4D: AM_ABS;  I_EOR; break;
        case 0x4E: AM_ABS;  I_LSR; break;
        /* 50 */
        case 0x50: BRANCH(!(p & F_V)); break;
        case 0x51: AM_IZYR; I_EOR; break;
        case 0x55: AM_ZPX;  I_EOR; break;
        case 0x56: AM_ZPX;  I_LSR; break;
        case 0x58: p &= ~F_I; break;
        case 0x59: AM_ABYR; I_EOR; break;
        case 0x5D: AM_ABXR; I_EOR; break;
        case 0x5E: AM_ABX;  I_LSR; break;
        /* 60 */
        case 0x60: v = POP(); t = POP(); pc = (uint16_t)((v | (t << 8)) + 1); break;
        case 0x61: AM_IZX;  I_ADC; break;
        case 0x65: AM_ZP;   I_ADC; break;
        case 0x66: AM_ZP;   I_ROR; break;
        case 0x68: a = POP(); NZ(a); break;
        case 0x69: AM_IMM;  I_ADC; break;
        case 0x6A: I_ROR_A; break;
        case 0x6C: ea = RD16(pc);   /* NMOS page-wrap bug reproduced */
                   v = RD(ea);
                   t = RD((ea & 0xFF00) | ((ea + 1) & 0x00FF));
                   pc = (uint16_t)(v | (t << 8)); break;
        case 0x6D: AM_ABS;  I_ADC; break;
        case 0x6E: AM_ABS;  I_ROR; break;
        /* 70 */
        case 0x70: BRANCH(p & F_V); break;
        case 0x71: AM_IZYR; I_ADC; break;
        case 0x75: AM_ZPX;  I_ADC; break;
        case 0x76: AM_ZPX;  I_ROR; break;
        case 0x78: p |= F_I; break;
        case 0x79: AM_ABYR; I_ADC; break;
        case 0x7D: AM_ABXR; I_ADC; break;
        case 0x7E: AM_ABX;  I_ROR; break;
        /* 80 */
        case 0x81: AM_IZX;  I_STA; break;
        case 0x84: AM_ZP;   I_STY; break;
        case 0x85: AM_ZP;   I_STA; break;
        case 0x86: AM_ZP;   I_STX; break;
        case 0x88: y--; NZ(y); break;
        case 0x8A: a = x; NZ(a); break;
        case 0x8C: AM_ABS;  I_STY; break;
        case 0x8D: AM_ABS;  I_STA; break;
        case 0x8E: AM_ABS;  I_STX; break;
        /* 90 */
        case 0x90: BRANCH(!(p & F_C)); break;
        case 0x91: AM_IZY;  I_STA; break;
        case 0x94: AM_ZPX;  I_STY; break;
        case 0x95: AM_ZPX;  I_STA; break;
        case 0x96: AM_ZPY;  I_STX; break;
        case 0x98: a = y; NZ(a); break;
        case 0x99: AM_ABY;  I_STA; break;
        case 0x9A: sp = x; break;
        case 0x9D: AM_ABX;  I_STA; break;
        /* A0 */
        case 0xA0: AM_IMM;  I_LDY; break;
        case 0xA1: AM_IZX;  I_LDA; break;
        case 0xA2: AM_IMM;  I_LDX; break;
        case 0xA4: AM_ZP;   I_LDY; break;
        case 0xA5: AM_ZP;   I_LDA; break;
        case 0xA6: AM_ZP;   I_LDX; break;
        case 0xA8: y = a; NZ(y); break;
        case 0xA9: AM_IMM;  I_LDA; break;
        case 0xAA: x = a; NZ(x); break;
        case 0xAC: AM_ABS;  I_LDY; break;
        case 0xAD: AM_ABS;  I_LDA; break;
        case 0xAE: AM_ABS;  I_LDX; break;
        /* B0 */
        case 0xB0: BRANCH(p & F_C); break;
        case 0xB1: AM_IZYR; I_LDA; break;
        case 0xB4: AM_ZPX;  I_LDY; break;
        case 0xB5: AM_ZPX;  I_LDA; break;
        case 0xB6: AM_ZPY;  I_LDX; break;
        case 0xB8: p &= ~F_V; break;
        case 0xB9: AM_ABYR; I_LDA; break;
        case 0xBA: x = sp; NZ(x); break;
        case 0xBC: AM_ABXR; I_LDY; break;
        case 0xBD: AM_ABXR; I_LDA; break;
        case 0xBE: AM_ABYR; I_LDX; break;
        /* C0 */
        case 0xC0: AM_IMM;  I_CMP(y); break;
        case 0xC1: AM_IZX;  I_CMP(a); break;
        case 0xC4: AM_ZP;   I_CMP(y); break;
        case 0xC5: AM_ZP;   I_CMP(a); break;
        case 0xC6: AM_ZP;   I_DEC; break;
        case 0xC8: y++; NZ(y); break;
        case 0xC9: AM_IMM;  I_CMP(a); break;
        case 0xCA: x--; NZ(x); break;
        case 0xCC: AM_ABS;  I_CMP(y); break;
        case 0xCD: AM_ABS;  I_CMP(a); break;
        case 0xCE: AM_ABS;  I_DEC; break;
        /* D0 */
        case 0xD0: BRANCH(!(p & F_Z)); break;
        case 0xD1: AM_IZYR; I_CMP(a); break;
        case 0xD5: AM_ZPX;  I_CMP(a); break;
        case 0xD6: AM_ZPX;  I_DEC; break;
        case 0xD8: p &= ~F_D; break;
        case 0xD9: AM_ABYR; I_CMP(a); break;
        case 0xDD: AM_ABXR; I_CMP(a); break;
        case 0xDE: AM_ABX;  I_DEC; break;
        /* E0 */
        case 0xE0: AM_IMM;  I_CMP(x); break;
        case 0xE1: AM_IZX;  I_SBC; break;
        case 0xE4: AM_ZP;   I_CMP(x); break;
        case 0xE5: AM_ZP;   I_SBC; break;
        case 0xE6: AM_ZP;   I_INC; break;
        case 0xE8: x++; NZ(x); break;
        case 0xE9: AM_IMM;  I_SBC; break;
        case 0xEA: break;
        case 0xEC: AM_ABS;  I_CMP(x); break;
        case 0xED: AM_ABS;  I_SBC; break;
        case 0xEE: AM_ABS;  I_INC; break;
        /* F0 */
        case 0xF0: BRANCH(p & F_Z); break;
        case 0xF1: AM_IZYR; I_SBC; break;
        case 0xF5: AM_ZPX;  I_SBC; break;
        case 0xF6: AM_ZPX;  I_INC; break;
        case 0xF8: p |= F_D; break;
        case 0xF9: AM_ABYR; I_SBC; break;
        case 0xFD: AM_ABXR; I_SBC; break;
        case 0xFE: AM_ABX;  I_INC; break;
        default:   break;   /* undocumented opcode: NOP */
        }
        cpu_clk += cyc;
    }

    cpu.pc = pc; cpu.a = a; cpu.x = x; cpu.y = y; cpu.sp = sp; cpu.p = p;
    return (uint32_t)(cpu_clk - start);
}

#endif /* CPU6502_H */
