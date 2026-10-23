/*
 * io.c - I/O registers, DMA, timers, IRQs, EEPROM and the Halt-driven
 * frame scheduler.  See io.h for the timing model.
 *
 * Game-specific facts this relies on (DBZ Legacy of Goku II, verified by
 * disassembly):
 *  - IRQ vector [0x03007FFC] = 0x03000824, an ARM dispatcher in IWRAM.  We
 *    replace it with irq_dispatch() below: same priority (DMA2 first, then
 *    bits 0..12), same IE/IF/flag-word handling, handlers called in System
 *    mode with lr = 0x03000951 / r4 = 0x03000934 as the original sets them.
 *  - Handler table at 0x03000E3C, "fired" flag word (u16) at 0x03000E74.
 *  - Audio: DMA1/DMA2 in FIFO mode feeding FIFO A (left) / B (right), Timer 0
 *    reload 0xFBE6 (1050 cycles per sample); DMA2 raises an IRQ per
 *    16-sample burst.
 */
#include <string.h>
#include <setjmp.h>
#include "io.h"
#include "widescreen.h"

#define IRQ_TABLE   0x03000E3Cu
#define IRQ_FLAGS   0x03000E74u
#define IRQ_RET_LR  0x03000951u      /* Thumb 'bx r4' trampoline */
#define IRQ_RET_R4  0x03000934u      /* dispatcher continuation */

uint32_t gba_frame_cycle;
uint32_t gba_frame_count;
gba_line_regs_t gba_line_regs[160];
int gba_line_regs_valid;

static uint64_t now;                 /* absolute guest cycles */

typedef struct {
    uint32_t src, dst, count;
    int active_fifo;                 /* FIFO-mode channel feeding audio */
} dma_t;
static dma_t dma[4];

typedef struct {
    uint16_t reload, ctl;
    uint64_t start;                  /* cycle at which counter == reload */
} timer_t_;
static timer_t_ tmr[4];
static const uint8_t tm_shift[4] = { 0, 6, 8, 10 };

static uint64_t next_audio;          /* next 16-sample FIFO block */

/* ------------------------------------------------------------ helpers */

static inline uint16_t R16(uint32_t off) { return io16(off); }
static inline void W16(uint32_t off, uint16_t v) { io16_set(off, v); }

static inline uint32_t cur_line(void) { return gba_frame_cycle / GBA_CYCLES_PER_LINE; }

static void raise_irq(uint16_t bits) {
    W16(REG_IF, R16(REG_IF) | bits);
}

/* ------------------------------------------------------------ timers */

static uint32_t timer_period(int i) {
    return 0x10000u - tmr[i].reload;
}

static uint16_t timer_read(int i) {
    timer_t_ *t = &tmr[i];
    if (!(t->ctl & 0x80))
        return t->reload;
    if (t->ctl & 0x04)           /* cascade: not used by this game */
        return t->reload;
    uint64_t ticks = (now - t->start) >> tm_shift[t->ctl & 3];
    return (uint16_t)(t->reload + ticks % timer_period(i));
}

static void timer_write_ctl(int i, uint16_t v) {
    timer_t_ *t = &tmr[i];
    int was = t->ctl & 0x80;
    t->ctl = v & 0xC7;
    if (!was && (v & 0x80)) {
        t->start = now;
        if (i == 0)
            next_audio = now + ((16ull * timer_period(0)) << tm_shift[t->ctl & 3]);
    }
}

static uint64_t timer_next_overflow(int i) {
    timer_t_ *t = &tmr[i];
    uint64_t per = (uint64_t)timer_period(i) << tm_shift[t->ctl & 3];
    uint64_t el = now - t->start;
    return t->start + (el / per + 1) * per;
}

/* ------------------------------------------------------------ DMA */

static inline int slow_region(uint32_t a) {
    uint32_t r = (a >> 24) & 0xF;
    return r == 0x0 || r == 0x1 || r == 0x4 || r == 0xD || r >= 0xE;
}

static void dma_transfer(int ch) {
    dma_t *d = &dma[ch];
    uint32_t cntoff = REG_DMA0SAD + 12 * ch + 10;
    uint16_t cnt = R16(cntoff);
    uint32_t n = d->count;
    if (n == 0)
        n = ch == 3 ? 0x10000 : 0x4000;
    int word = cnt & 0x400;
    int dctl = (cnt >> 5) & 3, sctl = (cnt >> 7) & 3;
    uint32_t sz = word ? 4 : 2;
    int32_t sinc = sctl == 0 ? (int32_t)sz : sctl == 1 ? -(int32_t)sz : 0;
    int32_t dinc = (dctl == 0 || dctl == 3) ? (int32_t)sz : dctl == 1 ? -(int32_t)sz : 0;
    uint32_t s = d->src, t = d->dst;

    uint32_t span = (n - 1) * sz;
    if (word) {
        s &= ~3u;
        t &= ~3u;
    } else {
        s &= ~1u;
        t &= ~1u;
    }
    if (dinc > 0)
        gba_mem_touch(t, span + sz);
    uint8_t *hs = gba_host_ptr(s), *ht = gba_host_wptr(t);
    int fast = hs && ht && dinc > 0 && sinc >= 0 &&
               (uint8_t *)gba_host_ptr(s + (sinc ? span : 0)) == hs + (sinc ? span : 0) &&
               (uint8_t *)gba_host_wptr(t + span) == ht + span;
    if (fast && !word && (((s | t) & 3) || (n & 1)))
        fast = 0;
    if (fast) {
        /* contiguous host memory; 16-bit units on word boundaries copy as words */
        uint32_t nw = word ? n : n / 2;
        uint32_t *ws = (uint32_t *)hs, *wt = (uint32_t *)ht;
        if (sinc && nw <= 16) {
            /* tile-sized copies (most of the game's DMAs): forward word
             * copy like the hardware, without a library call */
            while (nw >= 4) {
                uint32_t a = ws[0], b = ws[1], c = ws[2], e = ws[3];
                wt[0] = a;
                wt[1] = b;
                wt[2] = c;
                wt[3] = e;
                ws += 4;
                wt += 4;
                nw -= 4;
            }
            while (nw--)
                *wt++ = *ws++;
        } else if (sinc) {
            memmove(wt, ws, nw * 4);
        } else {
            uint32_t v = *ws;
            if (!word) {
                uint32_t h = rd16(s);
                v = h | (h << 16);
            }
            for (uint32_t k = 0; k < nw; k++)
                wt[k] = v;
        }
        s += sinc * n;
        t += dinc * n;
    } else if (word) {
        /* the slow paths handle I/O, EEPROM and unmapped regions directly
         * (cheaper than faulting on every unit in TLB builds) */
        int slow = slow_region(s) || slow_region(t);
        for (uint32_t k = 0; k < n; k++) {
            if (slow)
                gba_wr32_slow(t, gba_rd32_slow(s));
            else
                wr32(t, rd32(s));
            s += sinc;
            t += dinc;
        }
    } else {
        int slow = slow_region(s) || slow_region(t);
        for (uint32_t k = 0; k < n; k++) {
            if (slow)
                gba_wr16_slow(t, gba_rd16_slow(s));
            else
                wr16(t, rd16(s));
            s += sinc;
            t += dinc;
        }
    }
    d->src = s;
    if (dctl != 3)
        d->dst = t;
    if (!(cnt & 0x200) || ((cnt >> 12) & 3) == 0)
        W16(cntoff, cnt & 0x7FFF);
    if (cnt & 0x4000)
        raise_irq(IRQ_DMA0 << ch);
}

static void dma_write_cnt(int ch, uint16_t v) {
    uint32_t base = REG_DMA0SAD + 12 * ch;
    uint16_t old = R16(base + 10);
    W16(base + 10, v);
    if (!(old & 0x8000) && (v & 0x8000)) {
        dma_t *d = &dma[ch];
        d->src = io32(base) & (ch == 0 ? 0x07FFFFFF : 0x0FFFFFFF);
        d->dst = io32(base + 4) & (ch == 3 ? 0x0FFFFFFF : 0x07FFFFFF);
        d->count = R16(base + 8) & (ch == 3 ? 0xFFFF : 0x3FFF);
        int timing = (v >> 12) & 3;
        d->active_fifo = (timing == 3 && (ch == 1 || ch == 2));
        if (timing == 0)
            dma_transfer(ch);
    } else if (!(v & 0x8000)) {
        dma[ch].active_fifo = 0;
    }
}

static void dma_run_timed(int timing) {
    for (int ch = 0; ch < 4; ch++) {
        uint16_t cnt = R16(REG_DMA0SAD + 12 * ch + 10);
        if ((cnt & 0x8000) && ((cnt >> 12) & 3) == timing)
            dma_transfer(ch);
    }
}

/* ------------------------------------------------------------ audio */

static void audio_block(void) {
    int8_t l[16], r[16];
    int8_t a[16], b[16];
    memset(a, 0, sizeof a);
    memset(b, 0, sizeof b);
    uint16_t sch = R16(REG_SOUNDCNT_H);
    uint16_t irq = 0;
    for (int ch = 1; ch <= 2; ch++) {
        dma_t *d = &dma[ch];
        uint32_t base = REG_DMA0SAD + 12 * ch;
        uint16_t cnt = R16(base + 10);
        if (!d->active_fifo || !(cnt & 0x8000))
            continue;
        uint32_t dad = io32(base + 4) & 0x0FFFFFFF;
        int8_t *out = (dad == 0x040000A4u) ? b : a;
        for (int w = 0; w < 4; w++) {
            uint32_t v = rd32(d->src + 4 * w);
            out[4 * w + 0] = (int8_t)(v);
            out[4 * w + 1] = (int8_t)(v >> 8);
            out[4 * w + 2] = (int8_t)(v >> 16);
            out[4 * w + 3] = (int8_t)(v >> 24);
        }
        d->src += 16;
        if (cnt & 0x4000)
            irq |= IRQ_DMA0 << ch;
    }
    /* SOUNDCNT_H: bit 8/9 = A to right/left, bit 12/13 = B to right/left */
    for (int k = 0; k < 16; k++) {
        int lv = 0, rv = 0;
        if (sch & 0x0200) lv += a[k];
        if (sch & 0x0100) rv += a[k];
        if (sch & 0x2000) lv += b[k];
        if (sch & 0x1000) rv += b[k];
        lv = lv > 127 ? 127 : lv < -128 ? -128 : lv;
        rv = rv > 127 ? 127 : rv < -128 ? -128 : rv;
        l[k] = (int8_t)lv;
        r[k] = (int8_t)rv;
    }
    plat_audio_block(l, r);
    if (irq)
        raise_irq(irq);
}

/* ------------------------------------------------------------ IRQs */

static int in_irq;

/* Registers as the dispatcher leaves them at 'bx r0' (0x03000930): handlers
 * rely on some of them (the sound handler restarts the FIFO DMAs relative to
 * r3 = 0x04000200).  It runs in System mode on the interrupted stack after
 * pushing one word there.  The dispatcher passes its continuation in r4 and
 * the sound handler returns with 'bx r4'; that continuation unwinds a frame
 * we never build, so r4 gets the same return sentinel as lr. */
static void run_handler(uint32_t handler, uint32_t ack_word, int dma2) {
    gba_cpu_t saved = cpu;
    cpu.r[0] = handler;
    cpu.r[1] = IRQ_TABLE;
    cpu.r[2] = ack_word;
    cpu.r[3] = 0x04000200u;
    cpu.r[4] = IRQ_RET_LR;
    cpu.r[5] = 0x92u;                     /* CPSR of the IRQ mode it came from */
    cpu.r[12] = dma2 ? 0x9Fu : 0x1Fu;
    cpu.r[13] -= 4;                       /* the IRQ-mode lr pushed by 'str lr, [sp, #-4]!' */
    wr32(cpu.r[13], 0x00000138u);
    cpu.r[14] = IRQ_RET_LR;
    gba_call(handler);
    cpu = saved;
}

/* Native version of the IWRAM dispatcher at 0x03000824. */
void gba_irq_service(void) {
    if (in_irq)
        return;
    in_irq = 1;
    for (int guard = 0; guard < 64; guard++) {
        uint16_t ime = R16(REG_IME);
        uint16_t ie = R16(REG_IE);
        uint16_t act = ie & R16(REG_IF);
        if (!(ime & 1) || !act)
            break;
        uint16_t bit;
        uint32_t idx;
        if (act & 0x0400) {
            bit = 0x0400;
            idx = 0x28;
        } else {
            uint16_t m = act & 0x1BFF;
            if (!m) {
                /* only the GamePak IRQ: the original hangs; just ack it */
                W16(REG_IF, R16(REG_IF) & ~act);
                break;
            }
            int b = __builtin_ctz(m);
            bit = (uint16_t)(1u << b);
            idx = (uint32_t)b * 4;
            W16(REG_IE, ie & ~bit);
        }
        W16(REG_IF, R16(REG_IF) & ~bit);
        *(uint16_t *)((uint8_t *)gba_mem.iwram + ((IRQ_FLAGS & 0x7FFE) ^ GBA_HX)) |= bit;
        uint32_t handler = gba_mem.iwram[((IRQ_TABLE + idx) & 0x7FFC) >> 2];
        /* the word the dispatcher wrote to IE|IF<<16: IE (minus the source
         * unless it is DMA2) with the acknowledged bit in the IF half */
        uint32_t ack = (bit == 0x0400 ? ie : (uint16_t)(ie & ~bit)) | ((uint32_t)bit << 16);
        run_handler(handler, ack, bit == 0x0400);
        W16(REG_IE, ie);
        W16(REG_IME, ime);
    }
    in_irq = 0;
}

/* ------------------------------------------------------------ scheduler */

/* render sequence number each snapshot was taken in: lines not captured
 * since the last render (HBlank IRQs enabled or disabled mid-frame) get the
 * live registers instead of a snapshot from an older frame */
static uint32_t line_stamp[160], render_seq;

static void snapshot_line(int line) {
    if (line < 0 || line >= 160)
        return;
    line_stamp[line] = render_seq;
    gba_line_regs_t *s = &gba_line_regs[line];
    s->bg2pa = R16(0x20); s->bg2pb = R16(0x22); s->bg2pc = R16(0x24); s->bg2pd = R16(0x26);
    s->bg2x = io32(0x28);
    s->bg2y = io32(0x2C);
    for (int i = 0; i < 4; i++) {
        s->hofs[i] = R16(0x10 + 4 * i);
        s->vofs[i] = R16(0x12 + 4 * i);
    }
}

static int hblank_wanted(void) {
    return (R16(REG_IE) & IRQ_HBLANK) && (R16(REG_DISPSTAT) & 0x10);
}

static void set_time(uint64_t t) {
    now = t;
    gba_frame_count = (uint32_t)(t / GBA_CYCLES_PER_FRAME);
    gba_frame_cycle = (uint32_t)(t % GBA_CYCLES_PER_FRAME);
}

/* Advance to the next event and process it. */
static void run_next_event(void) {
    uint64_t frame_base = now - gba_frame_cycle;
    uint64_t vblank = frame_base + 160ull * GBA_CYCLES_PER_LINE;
    if (vblank <= now)
        vblank += GBA_CYCLES_PER_FRAME;
    uint64_t t = vblank;
    int ev = 0;   /* 0 vblank, 1 hblank, 2 audio, 3+i timer i */

    if (hblank_wanted()) {
        uint64_t lb = now - (gba_frame_cycle % GBA_CYCLES_PER_LINE);
        uint64_t hb = lb + 1006;
        if (hb <= now)
            hb += GBA_CYCLES_PER_LINE;
        if (hb < t) { t = hb; ev = 1; }
    }
    if ((tmr[0].ctl & 0x80) && (dma[1].active_fifo || dma[2].active_fifo)) {
        if (next_audio <= now)
            next_audio = now + 1;
        if (next_audio < t) { t = next_audio; ev = 2; }
    }
    for (int i = 0; i < 4; i++) {
        if ((tmr[i].ctl & 0xC4) == 0xC0) {
            uint64_t o = timer_next_overflow(i);
            if (o < t) { t = o; ev = 3 + i; }
        }
    }
    set_time(t);
    switch (ev) {
    case 0:
        if (gba_line_regs_valid)
            for (int l = 0; l < 160; l++)
                if (line_stamp[l] != render_seq)
                    snapshot_line(l);
        ws_frame_begin();            /* 320x240 field view state for this frame */
        plat_frame();
        render_seq++;
        gba_line_regs_valid = 0;
        dma_run_timed(1);
        if (R16(REG_DISPSTAT) & 0x08)
            raise_irq(IRQ_VBLANK);
        break;
    case 1: {
        int line = (int)cur_line();
        dma_run_timed(2);
        raise_irq(IRQ_HBLANK);
        gba_irq_service();
        /* registers written now are used for the next line */
        int nl = line + 1 >= (int)GBA_LINES ? 0 : line + 1;
        snapshot_line(nl);
        gba_line_regs_valid = 1;
        break;
    }
    case 2: {
        uint64_t per = (uint64_t)timer_period(0) << tm_shift[tmr[0].ctl & 3];
        next_audio = t + 16 * per;
        audio_block();
        break;
    }
    default: {
        int i = ev - 3;
        raise_irq(IRQ_TIMER0 << i);
        break;
    }
    }
}

void gba_halt(void) {
    /* wake when an enabled IRQ is pending (IME does not matter) */
    for (int guard = 0; !(R16(REG_IE) & R16(REG_IF) & 0x3FFF); guard++) {
        if (guard > 4000) {
            plat_log("halt: nothing can wake the CPU (IE=%04X)", (unsigned)R16(REG_IE));
            return;
        }
        run_next_event();
    }
    gba_irq_service();
}

void gba_stop(void) {
    plat_sleep();
}

/* ------------------------------------------------------------ EEPROM */

uint8_t gba_eeprom_data[0x2000];
int gba_eeprom_dirty;

static struct {
    int state;          /* 0 idle/cmd, 1 address, 2 data (write), 3 stop */
    int bits;
    uint32_t cmd;
    uint32_t addr;
    uint8_t buf[8];
    int read_pos;       /* -1 when no read is pending */
    int nbits;
} ee = { .read_pos = -1 };

int gba_eeprom_active(void) { return 1; }

uint32_t gba_eeprom_read(void) {
    if (ee.read_pos < 0)
        return 1;      /* ready */
    int p = ee.read_pos++;
    uint32_t v = 0;
    if (p >= 4) {
        int b = p - 4;
        v = (gba_eeprom_data[(ee.addr * 8 + (b >> 3)) & 0x1FFF] >> (7 - (b & 7))) & 1;
    }
    if (ee.read_pos >= 68)
        ee.read_pos = -1;
    return v;
}

void gba_eeprom_write(uint32_t v) {
    v &= 1;
    switch (ee.state) {
    case 0:
        ee.cmd = (ee.cmd << 1) | v;
        if (++ee.bits == 2) {
            if (ee.cmd == 3 || ee.cmd == 2) {
                ee.state = 1;
                ee.bits = 0;
                ee.addr = 0;
            } else {
                ee.bits = 0;
                ee.cmd = 0;
            }
        }
        break;
    case 1:
        ee.addr = (ee.addr << 1) | v;
        if (++ee.bits == 14) {
            ee.addr &= 0x3FF;
            ee.bits = 0;
            if (ee.cmd == 3) {
                ee.state = 3;
            } else {
                ee.state = 2;
                memset(ee.buf, 0, 8);
            }
        }
        break;
    case 2:
        ee.buf[ee.bits >> 3] |= (uint8_t)(v << (7 - (ee.bits & 7)));
        if (++ee.bits == 64)
            ee.state = 3;
        break;
    case 3:
        /* stop bit */
        if (ee.cmd == 3) {
            ee.read_pos = 0;
        } else {
            memcpy(&gba_eeprom_data[ee.addr * 8], ee.buf, 8);
            gba_eeprom_dirty = 1;
            plat_save_block(ee.addr);
        }
        ee.state = 0;
        ee.bits = 0;
        ee.cmd = 0;
        break;
    }
}

/* ------------------------------------------------------------ registers */

void gba_io_reset(void) {
    memset(gba_mem.io, 0, sizeof gba_mem.io);
    memset(dma, 0, sizeof dma);
    memset(tmr, 0, sizeof tmr);
    W16(REG_KEYINPUT, 0x03FF);
    W16(0x088, 0x0200);   /* SOUNDBIAS */
    set_time(160ull * GBA_CYCLES_PER_LINE);   /* boot right after VBlank */
    next_audio = 0;
    in_irq = 0;
    ee.state = 0; ee.bits = 0; ee.cmd = 0; ee.read_pos = -1;
}

uint32_t gba_io_read16(uint32_t off) {
    off &= 0xFFFFFE;
    if (off >= 0x400) {
        if ((off & 0xFFFE) == 0x800)   /* mirror of 0x04000800 (internal memory control) */
            return 0x0D00;
        return 0;
    }
    switch (off) {
    case REG_DISPSTAT: {
        uint32_t line = cur_line();
        uint16_t v = R16(REG_DISPSTAT) & 0xFFF8;
        if (line >= 160 && line < 227) v |= 1;
        if ((gba_frame_cycle % GBA_CYCLES_PER_LINE) >= 1006) v |= 2;
        if (line == (uint32_t)(v >> 8)) v |= 4;
        return v;
    }
    case REG_VCOUNT:
        return cur_line();
    case REG_TM0CNT: case REG_TM0CNT + 4: case REG_TM0CNT + 8: case REG_TM0CNT + 12:
        return timer_read((off - REG_TM0CNT) >> 2);
    case REG_KEYINPUT:
        return plat_keyinput();
    default:
        return R16(off);
    }
}

uint32_t gba_io_read32(uint32_t off) {
    return gba_io_read16(off) | (gba_io_read16(off + 2) << 16);
}

uint32_t gba_io_read8(uint32_t off) {
    uint32_t v = gba_io_read16(off & ~1u);
    return (off & 1) ? (v >> 8) & 0xFF : v & 0xFF;
}

void gba_io_write16(uint32_t off, uint32_t v) {
    off &= 0xFFFFFE;
    v &= 0xFFFF;
    if (off >= 0x400)
        return;
    switch (off) {
    case REG_DISPSTAT:
        W16(off, (R16(off) & 7) | (v & 0xFFF8));
        return;
    case REG_VCOUNT:
    case REG_KEYINPUT:
        return;
    case REG_IF:
        W16(off, R16(off) & ~v);
        return;
    case REG_IE:
    case REG_IME:
        W16(off, v);
        if (!in_irq && (R16(REG_IME) & 1) && (R16(REG_IE) & R16(REG_IF)))
            gba_irq_service();
        return;
    case REG_SOUNDCNT_H:
        W16(off, v & 0x770F);   /* FIFO reset bits are write-only */
        return;
    case REG_FIFO_A: case REG_FIFO_A + 2: case REG_FIFO_B: case REG_FIFO_B + 2:
        return;
    case REG_TM0CNT: case REG_TM0CNT + 4: case REG_TM0CNT + 8: case REG_TM0CNT + 12:
        tmr[(off - REG_TM0CNT) >> 2].reload = (uint16_t)v;
        W16(off, v);
        return;
    case REG_TM0CNT + 2: case REG_TM0CNT + 6: case REG_TM0CNT + 10: case REG_TM0CNT + 14:
        W16(off, v);
        timer_write_ctl((off - REG_TM0CNT - 2) >> 2, (uint16_t)v);
        return;
    case 0x0BA: dma_write_cnt(0, (uint16_t)v); return;
    case 0x0C6: dma_write_cnt(1, (uint16_t)v); return;
    case 0x0D2: dma_write_cnt(2, (uint16_t)v); return;
    case 0x0DE: dma_write_cnt(3, (uint16_t)v); return;
    case 0x300:
        W16(off, v);
        if (v & 0xFF00) {
            if (v & 0x8000) gba_stop(); else gba_halt();
        }
        return;
    default:
        W16(off, v);
        return;
    }
}

void gba_io_write32(uint32_t off, uint32_t v) {
    gba_io_write16(off, v & 0xFFFF);
    gba_io_write16(off + 2, v >> 16);
}

void gba_io_write8(uint32_t off, uint32_t v) {
    off &= 0xFFFFFF;
    v &= 0xFF;
    if (off == REG_HALTCNT) {
        if (v & 0x80) gba_stop(); else gba_halt();
        return;
    }
    if (off == REG_IF || off == REG_IF + 1) {
        W16(REG_IF, R16(REG_IF) & ~(v << ((off & 1) * 8)));
        return;
    }
    uint32_t cur = R16(off & ~1u);
    uint32_t nv = (off & 1) ? (cur & 0x00FF) | (v << 8) : (cur & 0xFF00) | v;
    gba_io_write16(off & ~1u, nv);
}
