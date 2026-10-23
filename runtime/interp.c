/*
 * interp.c - ARM7TDMI (ARMv4T) interpreter.
 *
 * Fallback for guest code that has no recompiled version.  Registers live in
 * cpu.r[] and the flags in cpu.n/z/c/v, exactly where recompiled code keeps
 * them between calls, so both sides can call each other freely.  The Thumb
 * state is a local of the run loop; it is mirrored into cpu.cpsr_ctl (CPSR_T)
 * whenever it changes so MRS / SWI / exception returns / native callees see
 * the state of the code that is running.
 *
 * cpu.r[15] is scratch while interpreting: before each instruction it is set
 * to the value a read of PC returns (+8 ARM, +4 Thumb, +12 for ARM
 * register-specified shifts), so operand reads need no PC special case.
 * Every write to PC is reported as a control transfer instead.
 */
#include "interp.h"
#include "recomp.h"
#include "dispatch.h"
#include "io.h"

/* result of executing one instruction */
#define X_SEQ  0    /* continue with the next sequential instruction */
#define X_JUMP 1    /* control transfer to *tgt (bit 0 = Thumb) */

/* plat_log once when a single interpreter run goes this long without
 * reaching its exit (diagnostic for guest spin loops) */
#define INTERP_WATCHDOG 4000000u

/* block transfer flags, at their ARM LDM/STM bit positions */
#define BLK_P (1u << 24)
#define BLK_U (1u << 23)
#define BLK_S (1u << 22)
#define BLK_W (1u << 21)
#define BLK_L (1u << 20)

/* condition code -> bitmap indexed by n<<3 | z<<2 | c<<1 | v (NV never runs) */
static const uint16_t cond_tab[16] = {
    0xF0F0, 0x0F0F, 0xCCCC, 0x3333, 0xFF00, 0x00FF, 0xAAAA, 0x5555,
    0x0C0C, 0xF3F3, 0xAA55, 0x55AA, 0x0A05, 0xF5FA, 0xFFFF, 0x0000,
};

static inline int cond_pass(uint32_t cond) {
    uint32_t f = (cpu.n << 3) | (cpu.z << 2) | (cpu.c << 1) | cpu.v;
    return (cond_tab[cond] >> f) & 1;
}

static inline void set_tbit(uint32_t thumb) {
    cpu.cpsr_ctl = (cpu.cpsr_ctl & ~CPSR_T) | (thumb ? CPSR_T : 0);
}

/* Thumb targets keep bit 0 as the state flag; ARM state has no PC bits 1..0 */
static inline uint32_t norm_target(uint32_t t) {
    return (t & 1) ? t : (t & ~3u);
}

static inline void set_nz(uint32_t r) {
    cpu.n = r >> 31;
    cpu.z = r == 0;
}

/* a + b + cin with all four flags; subtraction is a + ~b + carry */
static inline uint32_t add_nzcv(uint32_t a, uint32_t b, uint32_t cin) {
    uint32_t r = a + b + cin;
    cpu.c = cin ? (r <= a) : (r < a);
    cpu.v = ADD_V(a, b, r);
    set_nz(r);
    return r;
}

static inline uint32_t ror_imm(uint32_t op) {
    uint32_t rot = (op >> 7) & 0x1E, v = op & 0xFF;
    return rot ? (v >> rot) | (v << (32 - rot)) : v;
}

/* movs pc / ldm {..,pc}^: CPSR = SPSR, the restored T bit picks the state */
static uint32_t exc_return(uint32_t target) {
    gba_exc_return();
    if (cpu.cpsr_ctl & CPSR_T)
        return target | 1;
    return target & ~3u;
}

/* ------------------------------------------------------------ lookups */

/* Branch targets are looked up on every taken branch; the dispatch hash is
 * big enough to miss the N64 data cache, so keep a small direct-mapped cache
 * in front of it.  The function table never changes after boot. */
#define LK_BITS 8
static uint32_t lk_key[1u << LK_BITS];      /* ~target, 0 = empty */
static gba_func_t lk_fn[1u << LK_BITS];

static gba_func_t lookup(uint32_t t) {
    uint32_t i = (t * 0x9E3779B1u) >> (32 - LK_BITS);
    if (lk_key[i] == ~t)
        return lk_fn[i];
    gba_func_t fn = gba_lookup(t & ~1u, (int)(t & 1));
    lk_key[i] = ~t;
    lk_fn[i] = fn;
    return fn;
}

/* forget cached lookups (only needed if the function table changes) */
void gba_interp_flush_lookups(void) {
    for (unsigned i = 0; i < (1u << LK_BITS); i++) {
        lk_key[i] = 0;
        lk_fn[i] = NULL;
    }
}

/* ------------------------------------------------------------ block transfer */

/* register count of a 16-bit list (the builtin is a libgcc call on MIPS) */
static inline uint32_t popcount16(uint32_t x) {
    x = x - ((x >> 1) & 0x5555);
    x = (x & 0x3333) + ((x >> 2) & 0x3333);
    x = (x + (x >> 4)) & 0x0F0F;
    return (x + (x >> 8)) & 0x1F;
}

static uint32_t blk_start(uint32_t base, uint32_t list, uint32_t f, uint32_t *fin) {
    uint32_t bytes = list ? popcount16(list) * 4 : 0x40;
    if (f & BLK_U) {
        *fin = base + bytes;
        return base + ((f & BLK_P) ? 4 : 0);
    }
    *fin = base - bytes;
    return *fin + ((f & BLK_P) ? 0 : 4);
}

/* LDM/STM core shared by ARM and Thumb.  ARM7TDMI rules: lowest register at
 * the lowest address; STM writes the base back after the first transfer, so
 * a base that is the lowest listed register is stored unchanged, otherwise
 * the new base; LDM of the base keeps the loaded value; an empty list
 * transfers PC alone and moves the base by 0x40 (ARMv4).  The low address
 * bits are ignored (no rotation).  'pcval' is what a stored r15 reads as.
 * Returns 1 with the loaded word in *pcv when r15 was loaded. */
static int block_xfer(uint32_t rn, uint32_t list, uint32_t f, uint32_t pcval, uint32_t *pcv) {
    uint32_t *const r = cpu.r;
    uint32_t fin, a = blk_start(r[rn], list, f, &fin) & ~3u;
    uint32_t m = list ? list : 0x8000u;
    if (f & BLK_L) {
        if (f & BLK_W)
            r[rn] = fin;
        for (uint32_t i = 0; m; i++, m >>= 1) {
            if (!(m & 1))
                continue;
            uint32_t v = rd32(a);
            a += 4;
            if (i == 15) {
                *pcv = v;
                return 1;
            }
            r[i] = v;
        }
        return 0;
    }
    r[15] = pcval;
    int first = 1;
    for (uint32_t i = 0; m; i++, m >>= 1) {
        if (!(m & 1))
            continue;
        wr32(a, r[i]);
        a += 4;
        if (first) {
            first = 0;
            if (f & BLK_W)
                r[rn] = fin;
        }
    }
    return 0;
}

/* ------------------------------------------------------------ ARM */

static int arm_dp(uint32_t op, uint32_t pc, uint32_t *tgt) {
    uint32_t *const r = cpu.r;
    uint32_t b, sc = cpu.c;     /* operand 2 and shifter carry-out */

    if (op & (1u << 25)) {
        b = ror_imm(op);
        if (op & 0xF00)
            sc = b >> 31;
    } else if (!(op & 0x10)) {
        uint32_t m = r[op & 15], s = (op >> 7) & 31;
        switch ((op >> 5) & 3) {
        case 0:
            b = m << s;
            if (s)
                sc = (m >> (32 - s)) & 1;
            break;
        case 1:             /* LSR #0 encodes LSR #32 */
            if (s) {
                b = m >> s;
                sc = (m >> (s - 1)) & 1;
            } else {
                b = 0;
                sc = m >> 31;
            }
            break;
        case 2:             /* ASR #0 encodes ASR #32 */
            if (s) {
                b = (uint32_t)((int32_t)m >> s);
                sc = (m >> (s - 1)) & 1;
            } else {
                b = (uint32_t)((int32_t)m >> 31);
                sc = m >> 31;
            }
            break;
        default:            /* ROR #0 encodes RRX */
            if (s) {
                b = (m >> s) | (m << (32 - s));
                sc = (m >> (s - 1)) & 1;
            } else {
                b = (cpu.c << 31) | (m >> 1);
                sc = m & 1;
            }
            break;
        }
    } else {
        /* register-specified shift: the extra cycle makes PC read as +12 */
        r[15] = pc + 12;
        uint32_t m = r[op & 15], s = r[(op >> 8) & 15];
        switch ((op >> 5) & 3) {
        case 0: b = sh_lsl_c(m, s, &sc); break;
        case 1: b = sh_lsr_c(m, s, &sc); break;
        case 2: b = sh_asr_c(m, s, &sc); break;
        default: b = sh_ror_c(m, s, &sc); break;
        }
    }

    uint32_t rd = (op >> 12) & 15, a = r[(op >> 16) & 15], res;
    if (op & (1u << 20)) {
        switch ((op >> 21) & 15) {
        case 0x0: res = a & b; goto logical;
        case 0x1: res = a ^ b; goto logical;
        case 0x2: res = add_nzcv(a, ~b, 1); break;
        case 0x3: res = add_nzcv(b, ~a, 1); break;
        case 0x4: res = add_nzcv(a, b, 0); break;
        case 0x5: res = add_nzcv(a, b, cpu.c); break;
        case 0x6: res = add_nzcv(a, ~b, cpu.c); break;
        case 0x7: res = add_nzcv(b, ~a, cpu.c); break;
        case 0x8: set_nz(a & b); cpu.c = sc; goto test;
        case 0x9: set_nz(a ^ b); cpu.c = sc; goto test;
        case 0xA: add_nzcv(a, ~b, 1); goto test;
        case 0xB: add_nzcv(a, b, 0); goto test;
        case 0xC: res = a | b; goto logical;
        case 0xD: res = b; goto logical;
        case 0xE: res = a & ~b; goto logical;
        default:  res = ~b; goto logical;
        }
        goto store;
    logical:
        set_nz(res);
        cpu.c = sc;
    store:
        r[rd] = res;
        if (likely(rd != 15))
            return X_SEQ;
        /* flags were set from the result first, as the recompiler does;
         * the SPSR copy replaces them in modes that have one */
        *tgt = exc_return(res);
        return X_JUMP;
    test:
        if (unlikely(rd == 15)) {
            /* legacy TSTP/CMPP form: CPSR = SPSR, no branch */
            *tgt = exc_return(pc + 4);
            return X_JUMP;
        }
        return X_SEQ;
    }

    switch ((op >> 21) & 15) {
    case 0x0: res = a & b; break;
    case 0x1: res = a ^ b; break;
    case 0x2: res = a - b; break;
    case 0x3: res = b - a; break;
    case 0x4: res = a + b; break;
    case 0x5: res = a + b + cpu.c; break;
    case 0x6: res = a + ~b + cpu.c; break;
    case 0x7: res = b + ~a + cpu.c; break;
    case 0xC: res = a | b; break;
    case 0xD: res = b; break;
    case 0xE: res = a & ~b; break;
    case 0xF: res = ~b; break;
    default:  return X_SEQ;      /* PSR transfer space, decoded elsewhere */
    }
    r[rd] = res;
    if (likely(rd != 15))
        return X_SEQ;
    *tgt = res & ~3u;
    return X_JUMP;
}

static void arm_msr(uint32_t v, uint32_t op) {
    gba_msr(v, (op >> 16) & 15, (int)((op >> 22) & 1));
    /* MSR cannot switch instruction sets: keep CPSR_T matching ARM state */
    set_tbit(0);
}

/* BX, MRS, MSR (register form): opcodes 1000..1011 with S clear */
static int arm_misc(uint32_t op, uint32_t pc, uint32_t *tgt) {
    if ((op & 0x0FFFFFF0u) == 0x012FFF10u) {
        *tgt = norm_target(cpu.r[op & 15]);
        return X_JUMP;
    }
    if (op & 0xF0) {            /* ARMv5 BLX/CLZ/BKPT/DSP space */
        gba_undefined(pc, op);
        return X_SEQ;
    }
    if (op & (1u << 21))
        arm_msr(cpu.r[op & 15], op);
    else
        cpu.r[(op >> 12) & 15] = gba_mrs((int)((op >> 22) & 1));
    return X_SEQ;
}

static int arm_mul(uint32_t op) {
    uint32_t *const r = cpu.r;
    uint32_t res = r[op & 15] * r[(op >> 8) & 15];
    if (op & (1u << 21))
        res += r[(op >> 12) & 15];
    r[(op >> 16) & 15] = res;
    if (op & (1u << 20))
        set_nz(res);        /* C is unpredictable on ARMv4: left alone */
    return X_SEQ;
}

static int arm_mull(uint32_t op) {
    uint32_t *const r = cpu.r;
    uint32_t lo = (op >> 12) & 15, hi = (op >> 16) & 15;
    uint32_t m = r[op & 15], s = r[(op >> 8) & 15];
    uint64_t p;
    if (op & (1u << 22))
        p = (uint64_t)((int64_t)(int32_t)m * (int32_t)s);
    else
        p = (uint64_t)m * s;
    if (op & (1u << 21))
        p += ((uint64_t)r[hi] << 32) | r[lo];
    r[lo] = (uint32_t)p;
    r[hi] = (uint32_t)(p >> 32);
    if (op & (1u << 20)) {
        cpu.n = (uint32_t)(p >> 63);
        cpu.z = p == 0;
    }
    return X_SEQ;
}

static int arm_swp(uint32_t op) {
    uint32_t *const r = cpu.r;
    uint32_t addr = r[(op >> 16) & 15], src = r[op & 15], v;
    if (op & (1u << 22)) {
        v = rd8(addr);
        wr8(addr, src);
    } else {
        v = rd32(addr);         /* unaligned SWP rotates like LDR */
        wr32(addr, src);
    }
    r[(op >> 12) & 15] = v;
    return X_SEQ;
}

/* LDRH / STRH / LDRSB / LDRSH */
static int arm_xfer_half(uint32_t op, uint32_t pc, uint32_t *tgt) {
    uint32_t *const r = cpu.r;
    uint32_t rn = (op >> 16) & 15, rd = (op >> 12) & 15, v;
    uint32_t off = (op & (1u << 22)) ? (((op >> 4) & 0xF0) | (op & 0xF)) : r[op & 15];
    uint32_t base = r[rn];
    uint32_t addr = (op & (1u << 23)) ? base + off : base - off;
    uint32_t ea = (op & (1u << 24)) ? addr : base;
    int wb = !(op & (1u << 24)) || (op & (1u << 21));

    if (op & (1u << 20)) {
        switch ((op >> 5) & 3) {
        case 1: v = rd16(ea); break;
        case 2: v = (uint32_t)(int32_t)(int8_t)rd8(ea); break;
        default: v = (uint32_t)rd16s(ea); break;
        }
        if (wb)
            r[rn] = addr;       /* a load into the base wins */
        r[rd] = v;
        if (unlikely(rd == 15)) {
            *tgt = v & ~3u;
            return X_JUMP;
        }
        return X_SEQ;
    }
    if (((op >> 5) & 3) != 1) {  /* LDRD/STRD are ARMv5E */
        gba_undefined(pc, op);
        return X_SEQ;
    }
    wr16(ea, rd == 15 ? pc + 12 : r[rd]);
    if (wb)
        r[rn] = addr;
    return X_SEQ;
}

/* LDR / STR / LDRB / STRB (T variants are the same without an MMU) */
static int arm_xfer(uint32_t op, uint32_t pc, uint32_t *tgt) {
    uint32_t *const r = cpu.r;
    uint32_t rn = (op >> 16) & 15, rd = (op >> 12) & 15, off;

    if (op & (1u << 25)) {
        uint32_t m = r[op & 15], s = (op >> 7) & 31;
        switch ((op >> 5) & 3) {
        case 0: off = m << s; break;
        case 1: off = s ? m >> s : 0; break;
        case 2: off = (uint32_t)((int32_t)m >> (s ? s : 31)); break;
        default: off = s ? (m >> s) | (m << (32 - s)) : (cpu.c << 31) | (m >> 1); break;
        }
    } else {
        off = op & 0xFFF;
    }
    uint32_t base = r[rn];
    uint32_t addr = (op & (1u << 23)) ? base + off : base - off;
    uint32_t ea = (op & (1u << 24)) ? addr : base;
    int wb = !(op & (1u << 24)) || (op & (1u << 21));

    if (op & (1u << 20)) {
        uint32_t v = (op & (1u << 22)) ? rd8(ea) : rd32(ea);   /* rd32 rotates */
        if (wb)
            r[rn] = addr;
        r[rd] = v;
        if (unlikely(rd == 15)) {
            *tgt = v & ~3u;     /* ARMv4: no interworking on loads to PC */
            return X_JUMP;
        }
        return X_SEQ;
    }
    uint32_t v = rd == 15 ? pc + 12 : r[rd];
    if (op & (1u << 22))
        wr8(ea, v);
    else
        wr32(ea, v);
    if (wb)
        r[rn] = addr;
    return X_SEQ;
}

static int arm_block(uint32_t op, uint32_t pc, uint32_t *tgt) {
    uint32_t rn = (op >> 16) & 15, list = op & 0xFFFF, v;

    if (likely(!(op & BLK_S))) {
        if (block_xfer(rn, list, op, pc + 12, &v)) {
            *tgt = v & ~3u;
            return X_JUMP;
        }
        return X_SEQ;
    }
    /* S bit with PC loaded: exception return after the transfer */
    if (!list || ((op & BLK_L) && (list & 0x8000))) {
        if (block_xfer(rn, list, op, pc + 12, &v)) {
            *tgt = exc_return(v);
            return X_JUMP;
        }
        return X_SEQ;
    }
    /* S bit otherwise: transfer the user-bank registers */
    uint32_t fin, a = blk_start(cpu.r[rn], list, op, &fin) & ~3u;
    if (op & BLK_L) {
        gba_ldm_user(a, list);
        if ((op & BLK_W) && !((list >> rn) & 1))
            cpu.r[rn] = fin;
    } else {
        cpu.r[15] = pc + 12;
        gba_stm_user(a, list);
        if (op & BLK_W)
            cpu.r[rn] = fin;
    }
    return X_SEQ;
}

static int arm_exec(uint32_t op, uint32_t pc, uint32_t *tgt) {
    switch ((op >> 25) & 7) {
    case 0:
        if ((op & 0x90) == 0x90) {
            if (op & 0x60)
                return arm_xfer_half(op, pc, tgt);
            if ((op & 0x0FC00000u) == 0)
                return arm_mul(op);
            if ((op & 0x0F800000u) == 0x00800000u)
                return arm_mull(op);
            if ((op & 0x0FB00F00u) == 0x01000000u)
                return arm_swp(op);
            break;
        }
        if ((op & 0x01900000u) == 0x01000000u)
            return arm_misc(op, pc, tgt);
        return arm_dp(op, pc, tgt);
    case 1:
        if ((op & 0x01900000u) == 0x01000000u) {
            if (!(op & (1u << 21)))
                break;
            arm_msr(ror_imm(op), op);
            return X_SEQ;
        }
        return arm_dp(op, pc, tgt);
    case 3:
        if (op & 0x10)
            break;              /* architecturally undefined space */
        /* fall through */
    case 2:
        return arm_xfer(op, pc, tgt);
    case 4:
        return arm_block(op, pc, tgt);
    case 5:
        if (op & (1u << 24))
            cpu.r[14] = pc + 4;
        *tgt = pc + 8 + (uint32_t)((int32_t)(op << 8) >> 6);
        return X_JUMP;
    case 7:
        if (op & (1u << 24)) {
            /* the BIOS takes the comment field's top byte as the number */
            gba_swi((op >> 16) & 0xFF, pc + 4, 0);
            return X_SEQ;
        }
        break;
    default:
        break;                  /* coprocessor space: no coprocessors */
    }
    gba_undefined(pc, op);
    return X_SEQ;
}

/* ------------------------------------------------------------ Thumb */

/* format 4: ALU operations, all set flags */
static int thumb_alu(uint32_t op) {
    uint32_t *const r = cpu.r;
    uint32_t rd = op & 7, a = r[rd], b = r[(op >> 3) & 7], v;
    switch ((op >> 6) & 15) {
    case 0x0: v = a & b; break;
    case 0x1: v = a ^ b; break;
    case 0x2: v = sh_lsl_c(a, b, &cpu.c); break;
    case 0x3: v = sh_lsr_c(a, b, &cpu.c); break;
    case 0x4: v = sh_asr_c(a, b, &cpu.c); break;
    case 0x5: r[rd] = add_nzcv(a, b, cpu.c); return X_SEQ;
    case 0x6: r[rd] = add_nzcv(a, ~b, cpu.c); return X_SEQ;
    case 0x7: v = sh_ror_c(a, b, &cpu.c); break;
    case 0x8: set_nz(a & b); return X_SEQ;
    case 0x9: r[rd] = add_nzcv(0, ~b, 1); return X_SEQ;     /* NEG */
    case 0xA: add_nzcv(a, ~b, 1); return X_SEQ;
    case 0xB: add_nzcv(a, b, 0); return X_SEQ;
    case 0xC: v = a | b; break;
    case 0xD: v = a * b; break;     /* C unpredictable on ARMv4: left alone */
    case 0xE: v = a & ~b; break;
    default:  v = ~b; break;
    }
    r[rd] = v;
    set_nz(v);
    return X_SEQ;
}

/* format 5: ADD/CMP/MOV with high registers, BX */
static int thumb_hireg(uint32_t op, uint32_t *tgt) {
    uint32_t *const r = cpu.r;
    uint32_t rd = (op & 7) | ((op >> 4) & 8), rs = (op >> 3) & 15, v;
    switch ((op >> 8) & 3) {
    case 0:
        v = r[rd] + r[rs];
        break;
    case 1:
        add_nzcv(r[rd], ~r[rs], 1);
        return X_SEQ;
    case 2:
        v = r[rs];
        break;
    default:
        *tgt = norm_target(r[rs]);
        return X_JUMP;
    }
    r[rd] = v;
    if (likely(rd != 15))
        return X_SEQ;
    *tgt = v | 1;               /* PC writes stay in Thumb state */
    return X_JUMP;
}

static int thumb_exec(uint32_t op, uint32_t pc, uint32_t *tgt) {
    uint32_t *const r = cpu.r;
    uint32_t rd = op & 7, rs = (op >> 3) & 7, imm5 = (op >> 6) & 31, a, b, v;

    switch (op >> 11) {
    case 0x00:                  /* LSL #imm (#0 keeps C) */
        a = r[rs];
        if (imm5) {
            cpu.c = (a >> (32 - imm5)) & 1;
            a <<= imm5;
        }
        r[rd] = a;
        set_nz(a);
        return X_SEQ;
    case 0x01:                  /* LSR #imm (#0 means #32) */
        a = r[rs];
        if (imm5) {
            cpu.c = (a >> (imm5 - 1)) & 1;
            a >>= imm5;
        } else {
            cpu.c = a >> 31;
            a = 0;
        }
        r[rd] = a;
        set_nz(a);
        return X_SEQ;
    case 0x02:                  /* ASR #imm (#0 means #32) */
        a = r[rs];
        if (imm5) {
            cpu.c = (a >> (imm5 - 1)) & 1;
            a = (uint32_t)((int32_t)a >> imm5);
        } else {
            cpu.c = a >> 31;
            a = (uint32_t)((int32_t)a >> 31);
        }
        r[rd] = a;
        set_nz(a);
        return X_SEQ;
    case 0x03:                  /* ADD/SUB Rd, Rs, Rn | #imm3 */
        a = r[rs];
        b = (op & 0x400) ? ((op >> 6) & 7) : r[(op >> 6) & 7];
        r[rd] = (op & 0x200) ? add_nzcv(a, ~b, 1) : add_nzcv(a, b, 0);
        return X_SEQ;
    case 0x04:                  /* MOV Rd, #imm8 */
        v = op & 0xFF;
        r[(op >> 8) & 7] = v;
        set_nz(v);
        return X_SEQ;
    case 0x05:                  /* CMP Rd, #imm8 */
        add_nzcv(r[(op >> 8) & 7], ~(op & 0xFF), 1);
        return X_SEQ;
    case 0x06:                  /* ADD Rd, #imm8 */
        rd = (op >> 8) & 7;
        r[rd] = add_nzcv(r[rd], op & 0xFF, 0);
        return X_SEQ;
    case 0x07:                  /* SUB Rd, #imm8 */
        rd = (op >> 8) & 7;
        r[rd] = add_nzcv(r[rd], ~(op & 0xFF), 1);
        return X_SEQ;
    case 0x08:
        if (op & 0x400)
            return thumb_hireg(op, tgt);
        return thumb_alu(op);
    case 0x09:                  /* LDR Rd, [PC, #imm8*4]: PC word aligned */
        r[(op >> 8) & 7] = rd32(((pc + 4) & ~3u) + ((op & 0xFF) << 2));
        return X_SEQ;
    case 0x0A: case 0x0B:       /* register offset transfers */
        a = r[rs] + r[(op >> 6) & 7];
        switch ((op >> 9) & 7) {
        case 0: wr32(a, r[rd]); break;
        case 1: wr16(a, r[rd]); break;
        case 2: wr8(a, r[rd]); break;
        case 3: r[rd] = (uint32_t)(int32_t)(int8_t)rd8(a); break;
        case 4: r[rd] = rd32(a); break;
        case 5: r[rd] = rd16(a); break;
        case 6: r[rd] = rd8(a); break;
        default: r[rd] = (uint32_t)rd16s(a); break;
        }
        return X_SEQ;
    case 0x0C: wr32(r[rs] + (imm5 << 2), r[rd]); return X_SEQ;
    case 0x0D: r[rd] = rd32(r[rs] + (imm5 << 2)); return X_SEQ;
    case 0x0E: wr8(r[rs] + imm5, r[rd]); return X_SEQ;
    case 0x0F: r[rd] = rd8(r[rs] + imm5); return X_SEQ;
    case 0x10: wr16(r[rs] + (imm5 << 1), r[rd]); return X_SEQ;
    case 0x11: r[rd] = rd16(r[rs] + (imm5 << 1)); return X_SEQ;
    case 0x12: wr32(r[13] + ((op & 0xFF) << 2), r[(op >> 8) & 7]); return X_SEQ;
    case 0x13: r[(op >> 8) & 7] = rd32(r[13] + ((op & 0xFF) << 2)); return X_SEQ;
    case 0x14:                  /* ADD Rd, PC, #imm8*4: PC word aligned */
        r[(op >> 8) & 7] = ((pc + 4) & ~3u) + ((op & 0xFF) << 2);
        return X_SEQ;
    case 0x15:
        r[(op >> 8) & 7] = r[13] + ((op & 0xFF) << 2);
        return X_SEQ;
    case 0x16: case 0x17:
        switch ((op >> 8) & 15) {
        case 0x0:               /* ADD/SUB SP, #imm7*4 */
            v = (op & 0x7F) << 2;
            r[13] = (op & 0x80) ? r[13] - v : r[13] + v;
            return X_SEQ;
        case 0x4: case 0x5:     /* PUSH {rlist, lr} = STMDB sp! */
            block_xfer(13, (op & 0xFF) | ((op & 0x100) << 6), BLK_P | BLK_W, pc + 6, &v);
            return X_SEQ;
        case 0xC: case 0xD:     /* POP {rlist, pc} = LDMIA sp!; ARMv4T stays Thumb */
            if (block_xfer(13, (op & 0xFF) | ((op & 0x100) << 7), BLK_U | BLK_W | BLK_L, 0, &v)) {
                *tgt = v | 1;
                return X_JUMP;
            }
            return X_SEQ;
        default:
            break;
        }
        break;
    case 0x18:                  /* STMIA Rb!, {rlist} */
        block_xfer((op >> 8) & 7, op & 0xFF, BLK_U | BLK_W, pc + 6, &v);
        return X_SEQ;
    case 0x19:                  /* LDMIA Rb!, {rlist} (empty list loads PC) */
        if (block_xfer((op >> 8) & 7, op & 0xFF, BLK_U | BLK_W | BLK_L, 0, &v)) {
            *tgt = v | 1;
            return X_JUMP;
        }
        return X_SEQ;
    case 0x1A: case 0x1B: {     /* Bcc, SWI */
        uint32_t cond = (op >> 8) & 15;
        if (cond == 15) {
            gba_swi(op & 0xFF, pc + 2, 1);
            return X_SEQ;
        }
        if (cond == 14)
            break;
        if (!cond_pass(cond))
            return X_SEQ;
        *tgt = (pc + 4 + ((uint32_t)(int32_t)(int8_t)(op & 0xFF) << 1)) | 1;
        return X_JUMP;
    }
    case 0x1C:                  /* B */
        *tgt = (pc + 4 + (uint32_t)((int32_t)(op << 21) >> 20)) | 1;
        return X_JUMP;
    case 0x1E:                  /* BL prefix: LR = PC + (offset << 12) */
        r[14] = pc + 4 + (uint32_t)((int32_t)(op << 21) >> 9);
        return X_SEQ;
    case 0x1F:                  /* BL suffix: PC = LR + offset, LR = return | 1 */
        v = r[14] + ((op & 0x7FF) << 1);
        r[14] = (pc + 2) | 1;
        *tgt = v | 1;
        return X_JUMP;
    default:
        break;                  /* 11101: ARMv5 BLX suffix */
    }
    gba_undefined(pc, op);
    return X_SEQ;
}

/* ------------------------------------------------------------ run loop */

/* execute the instruction at pc; X_JUMP leaves the new pc | T in *tgt */
static inline int exec_one(uint32_t pc, uint32_t thumb, uint32_t *tgt) {
    if (thumb) {
        uint32_t op = rd16(pc);
        cpu.r[15] = pc + 4;
        return thumb_exec(op, pc, tgt);
    }
    uint32_t op = rd32(pc);
    uint32_t cond = op >> 28;
    cpu.r[15] = pc + 8;
    if (cond != 0xE && !cond_pass(cond))
        return X_SEQ;
    return arm_exec(op, pc, tgt);
}

/* Interpret from 'start'.  Every control transfer is checked against the
 * exit link (cpu.r[14] at entry) and against recompiled code: calls to
 * recompiled code (BL, or any transfer that leaves LR pointing at the next
 * instruction) run natively and interpretation resumes where they return;
 * other jumps to recompiled code end a jump-mode run, while a call-mode run
 * executes them like gba_jump and follows cpu.next_pc. */
static void interp_run(uint32_t start, int jump_mode) {
    const uint32_t stop = cpu.r[14];
    uint32_t th = start & 1, pc = th ? start & ~1u : start & ~3u, tgt;
    uint32_t watchdog = INTERP_WATCHDOG;

    set_tbit(th);
    for (;;) {
        if (unlikely(watchdog != 0) && --watchdog == 0)
            plat_log("interp: %u instructions without returning, pc=%08X %s (exit link %08X)",
                     INTERP_WATCHDOG, (unsigned)pc, th ? "thumb" : "arm", (unsigned)stop);

        if (likely(exec_one(pc, th, &tgt) == X_SEQ)) {
            pc += th ? 2 : 4;
            continue;
        }

        uint32_t seq = pc + (th ? 2 : 4);
        int call = ((cpu.r[14] ^ seq) >> 1) == 0;
        for (;;) {
            if (((tgt ^ stop) >> 1) == 0) {
                /* a caller compares the exact link; a jump-mode run must
                 * keep the state really reached (a Thumb 'mov lr, pc' link
                 * has bit 0 clear) for gba_run_from to resume it */
                cpu.next_pc = jump_mode ? tgt : stop;
                set_tbit(cpu.next_pc & 1);
                return;
            }
            gba_func_t fn = lookup(tgt);
            if (!fn)
                break;
            if (call) {
                uint32_t link = cpu.r[14];
                gba_saved_regs_t keep;
                gba_save_regs(&keep);
                set_tbit(tgt & 1);
                gba_call(tgt);
                if (((cpu.next_pc ^ link) >> 1) == 0) {
                    /* the registers the callee preserves are only right
                     * in its recompiled callers' locals (dispatch.h) */
                    gba_restore_preserved(&keep, tgt);
                    /* resume in the caller's state, as recompiled callers
                     * do: a Thumb 'mov lr, pc' link has bit 0 clear */
                    tgt = seq | th;
                    break;
                }
                call = 0;       /* returned elsewhere: a jump to next_pc */
            } else if (jump_mode) {
                cpu.next_pc = tgt;
                set_tbit(tgt & 1);
                return;
            } else {
                uint32_t link = cpu.r[14];
                gba_saved_regs_t keep;
                gba_save_regs(&keep);
                set_tbit(tgt & 1);
                fn();
                if (((cpu.next_pc ^ link) >> 1) == 0)
                    gba_restore_preserved(&keep, tgt);
            }
            tgt = norm_target(cpu.next_pc);
        }
        th = tgt & 1;
        pc = tgt & ~1u;
        set_tbit(th);
    }
}

void gba_interp_call(uint32_t target) {
    interp_run(target, 0);
}

void gba_interp_jump(uint32_t target) {
    interp_run(target, 1);
}

uint32_t gba_interp_step(uint32_t pc_with_thumb_bit) {
    uint32_t th = pc_with_thumb_bit & 1, tgt;
    uint32_t pc = th ? pc_with_thumb_bit & ~1u : pc_with_thumb_bit & ~3u;
    set_tbit(th);
    if (exec_one(pc, th, &tgt) == X_SEQ)
        tgt = (pc + (th ? 2 : 4)) | th;
    set_tbit(tgt & 1);
    return tgt;
}
