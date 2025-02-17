﻿
#include "sleigh.hh"
#include "thumb_gen.hh"
#include "pass.hh"
#include <assert.h>
#include "vm.h"

#define a(vn)               ((vn)->get_addr())
#define ama                 d->ma_addr
#define asp                 d->sp_addr
#define ar0                 d->r0_addr
#define apc                 d->pc_addr
#define alr                 d->lr_addr
#define azr                 d->zr_addr
#define ang                 as("NG")
#define aov                 as("OV")
#define acy                 as("CY")
#define istemp(vn)          ((vn)->get_addr().getSpace()->getType() == IPTR_INTERNAL)
#define isreg(vn)           d->is_greg(vn->get_addr())
#define as(st)              d->trans->getRegister(st).getAddr()

#define pcode_cpsr_out_dead(p)      !(p->live_out[NG] | p->live_out[ZR] | p->live_out[CY]| p->live_out[OV])

/* 表明当前pcode后，是否有跟随对cpsr的操作 */
static int g_cpsr_follow = 0;

/* 表明当前pcode后，cpsr寄存器是否出口死亡 */
static int g_cpsr_dead = 0;

/* 表明是否可以对当前指令设置 setflags 标记 */
static int g_setflags = 0;

#define UPDATE_CPSR_SET(_p)  do { \
        g_cpsr_follow = follow_by_set_cpsr(_p); \
        g_cpsr_dead = pcode_cpsr_out_dead(_p); \
        g_setflags = g_cpsr_follow || g_cpsr_dead; \
    } while (0)


#define ANDNEQ(r1, r2)      ((r1 & ~r2) == 0)
#define ANDEQ(r1, r2)      ((r1 & r2) == r2)
#define align4(a)           ((a & 3) == 0)
#define align2(a)           ((a & 1) == 0)

/* A8.8.119 */
#define NOP1            0xbf00
#define NOP2            0xf3af8000

#define imm_map(imm, l, bw, r)          (((imm >> l) & ((1 <<  bw) - 1)) << r)
#define bit_get(imm, l, bw)             ((imm >> l) & ((1 << bw) - 1))

#define t1_param_check(rd,rn,rm,shtype,shval)   ((rd < 8)  && (rm < 8)  && (rn < 8)  && (shtype == SRType_LSL) && (!shval))
#define t1_param_check1(rd,rn,rm,shtype,shval)  (t1_param_check(rd,rn,rm,shtype,shval) && (rd == rn))

#define T_PARAM_MAP(s,rd,rn,rm,shtype,sh)      ((s << 20) | (rn << 16) | (rd << 8) | (rm) | SR4_IMM_MAP5(shtype,sh))

#define read_thumb2(b)          ((((uint32_t)(b)[0]) << 16) | (((uint32_t)(b)[1]) << 24) | ((uint32_t)(b)[2]) | (((uint32_t)(b)[3]) << 8))
#define write_thumb2(i, buf) do { \
            (buf)[2] = i & 255; \
            i >>= 8; \
            (buf)[3] = i & 255; \
            i >>= 8; \
            (buf)[0] = i & 255; \
            i >>= 8; \
            (buf)[1] = i & 255; \
            i >>= 8; \
    } while (0)

enum SRType {
    SRType_LSL,
    SRType_LSR,
    SRType_ASR,
    SRType_ROR
};

#define SR4_IMM_MAP5(sr,imm)                 ((sr << 4) | imm_map(imm, 2, 3, 12) | imm_map(imm, 0, 2, 6))

uint32_t bitcount(uint32_t x)
{
    x = (x & 0x55555555) + ((x >> 1) & 0x55555555);
    x = (x & 0x33333333) + ((x >> 2) & 0x33333333);
    x = (x & 0x0F0F0F0F) + ((x >> 4) & 0x0F0F0F0F);
    x = (x & 0x00FF00FF) + ((x >> 8) & 0x00FF00FF);
    x = (x & 0x0000FFFF) + ((x >>16) & 0x0000FFFF);

    return x;
}

int nlz(uint32_t x) 
{
    int n;
    if (x == 0) return(32); n = 1;
    if ((x >> 16) == 0) { n = n + 16; x = x << 16; }
    if ((x >> 24) == 0) { n = n + 8; x = x << 8; }
    if ((x >> 28) == 0) { n = n + 4; x = x << 4; }
    if ((x >> 30) == 0) { n = n + 2; x = x << 2; }
    n = n - (x >> 31);
    return n;
}

int ntz(unsigned x) 
{
    int n;
    if (x == 0) return(32);
    n = 1;
    if ((x & 0x0000FFFF) == 0) { n = n + 16; x = x >> 16; }
    if ((x & 0x000000FF) == 0) { n = n + 8; x = x >> 8; }
    if ((x & 0x0000000F) == 0) { n = n + 4; x = x >> 4; }
    if ((x & 0x00000003) == 0) { n = n + 2; x = x >> 2; }
    return n - (x & 1);
}

int continuous_zero_bits(uint32_t x, int *lsb, int *width)
{
    /* 检查x是否只有一段0的格式 */

    /* 去掉低部的连续1，兼容以0开始 */
    uint32_t t = ~((x + 1) & x);
    if ((t + 1) & t)
        return 0;

    if (x == (uint32_t)-1) return 0;

    int i = 0;

    while (x & (1 << i)) i++;

    *lsb = i;
    *width = ntz((x >> i));

    return 1;
}


/* 测试x里是否有连续的1 */
#define bitcont(x)          (!(((~x + 1) & x + x) & x))

static  thumb_gen *g_cg = NULL;

thumb_gen::thumb_gen(funcdata *f) : codegen(f)
{
    fd = f;
    d = fd->d;
    g_cg = this;

    data = d->loader->filedata;
    ind = fd->get_addr().getOffset();

    int size = fd->get_size();

    maxend = size + ind;
    end = size + ind;

    memset(data + ind, 0, size);
}

thumb_gen::~thumb_gen()
{
}

void thumb_gen::resort_blocks()
{
    blist = fd->bblocks.blist;
}

#define UNPREDICITABLE()        vm_error("not support");

int thumb_gen::reg2i(const Address &a)
{
    int ret = d->reg2i(a);

    if (ret == -1)
        throw LowlevelError("un-support reg2i address");

    return ret;
}

bool thumb_gen::simd_need_fix(pit it)
{
    return false;
}


static void ot(uint16_t i)
{
    g_cg->data[g_cg->ind++] = i & 255;
    i >>= 8;
    g_cg->data[g_cg->ind++] = i & 255;
}

static void ott(uint32_t i)
{
    dobc *d = g_cg->d;

    d->clear_it_info(Address(d->getDefaultCodeSpace(), g_cg->ind));

    write_thumb2(i, g_cg->data + g_cg->ind);
    g_cg->ind += 4;

}

static void ob(uint8_t *buf, int siz)
{
    memcpy(g_cg->data + g_cg->ind, buf, siz);
    g_cg->ind += siz;
}

static void o(uint32_t i)
{
    if (i == 0)
        vm_error("meet 0's value instruction");

    if (g_cg->ind >= g_cg->end)
        vm_error("exceed function max size");

    if ((i >> 16))
        ott(i);
    else
        ot((uint16_t)i);
}

uint32_t encbranch2(int pos, int target, int arm)
{
    if (arm && !align4(target))
        vm_error("blx address must be align 4 bytes");

    if (!arm && !align2(target))
        vm_error("bl address must be align 2bytes");

    int addr = 0;

    addr = target - (pos + 4);

    if (arm && !align4(addr))
        return (uint32_t)-1;

    addr /= 2;

    if ((addr >= 0xffffff) && (addr < -0xffffff))
        vm_error("FIXME: jmp bigger than 16MB");

    int s = bit_get(addr, 31, 1), j1, j2, i1 = bit_get(addr, 22, 1), i2 = bit_get(addr, 21, 1);

    j1 = (!i1) ^ s;
    j2 = (!i2) ^ s;

    return  (s << 26) | (j1 << 13) | (j2 << 11) | imm_map(addr, 11, 10, 16) | imm_map(addr, 0, 11, 0);
}


uint32_t thumb_gen::stuff_const(uint32_t op, uint32_t c)
{
    int try_neg = 0;
    uint32_t nc = 0, negop = 0;

    switch (op & 0x1f00000) {
    case 0x1000000:     // add
    case 0x1a00000:     // sub
        try_neg = 1;
        negop = op ^ 0xa00000;
        nc = -c;
        break;
    case 0x0400000:     // mov or eq
    case 0x0600000:     // mvn
        if (ANDEQ(op, 0xf0000)) { // mov
            try_neg = 1;
            negop = op ^ 0x200000;
            nc = ~c;
        }
        else if (c == ~0) { // orr
            return (op & 0xfff0ffff) | 0x1E00000;
        }
        break;
    case 0x800000:      // xor
        if (c == ~0)
            return (op & 0xe0100f00) | ((op >> 16) & 0xf) | 0x4f0000;
        break;
    case 0:             // and
        if (c == ~0)
            return (op & 0xe0100f00) | ((op >> 16) & 0xf) | 0x6f0000;
    case 0x0200000:     // bic
        try_neg = 1;
        negop = op ^ 0x200000;
        nc = ~c;
        break;
    }

    do {
        /* A6.3.2 */
        uint32_t m;
        int i, c1, c2, h;
        if (c < 256)
            return op | c;
        c1 = c & 0xff;
        if ((c2 = (c1 | (c1 << 16))) == c)
            return op | c | 0x0100;
        if ((c2 << 8) == c)
            return op | c | 0x0200;
        if ((c1 | c2) == c)
            return op | c | 0x0300;
        for (i = 8; i < 32; i ++) {
            m = 0xff << (32 - i);
            h = 1 << (39 - i);
            if ((c & m) == c && (c & h) == h)
                return op | imm_map(i, 4, 1, 26) | imm_map(i, 1, 3, 12) | imm_map(i, 0, 1, 7) | (((c) >> (32 - i)) & 0x7f);
        }
        op = negop;
        c = nc;
    } while (try_neg--);

    return 0;
}

/* A7.4.6 */
struct {
    int         op;
    int         cmode;
    uint64_t    mask;
    uint64_t    val;
    int         dt;
} simd_imm_tab[] = {
    { 0, 0b0000, 0,                    0x000000ff000000ff, 32 },
    { 0, 0b0010, 0,                    0x0000ff000000ff00, 32 },
    { 0, 0b0100, 0,                    0x00ff000000ff0000, 32 },
    { 0, 0b0110, 0,                    0xff000000ff000000, 32 },
    { 0, 0b1000, 0,                    0x00ff00ff00ff00ff, 16 },
    { 0, 0b1010, 0,                    0xff00ff00ff00ff00, 16 },

    { 0, 0b1100, 0x000000ff000000ff,   0x0000ff000000ff00, 32 },
    { 0, 0b1101, 0x0000ffff0000ffff,   0x00ff000000ff0000, 32 },

    { 0, 0b1110, 0,                    0xffffffffffffffff, 8 },
    { 0, 0b1111, 0,                    0xffffffffffffffff, 8 },

    { 1, 0b1110, 0,                    0xffffffffffffffff, 64 },
};

/* vector */
uint32_t stuff_constv(uint32_t op, uint64_t c)
{
    int bw = 64, i, j, a = 0, dt;
    uint64_t c0 = c, mask;

    for (i = 0; i < (count_of_array(simd_imm_tab) - 1); i++) {
        if (!ANDEQ(simd_imm_tab[i].mask, c)) continue;
        if ((~simd_imm_tab[i].val & c) != c) continue;
        dt = simd_imm_tab[i].dt;
        mask = (1ll << dt) - 1;

        for (bw = 64, c0 = c; bw; bw -= dt, c0 >>= dt) {
            if ((c0 & mask) != (c & mask)) break;
        }
        if (bw) continue;

        a = (c >> nlz((uint32_t)simd_imm_tab[i].val)) & 0xff;
        break;
    }

    if (i == (count_of_array(simd_imm_tab) - 1)) {
        for (i = 0; i < 8; i++) {
            j = (c >> (i * 8)) & 0xff;
            if (j && (j < 255)) return 0;
            a |= (j & 1) << i;
        }
        i = count_of_array(simd_imm_tab) - 1;
    }

    return op | imm_map(a, 7, 1, 28) | imm_map(a, 4, 3, 16) | (a & 0xf) | (simd_imm_tab[i].op << 5) | (simd_imm_tab[i].cmode << 8);
}

/* 这里的m是规定c的bitwidth ，假如为0，则默认的12，因为大部分的数都是12位填法 */
static uint32_t stuff_constw(uint32_t op, uint32_t c, int m)
{
    if (m == 0) m = 12;
    if (c >= (1 << m)) return 0;

    op |= imm_map(c, 11, 1, 26) | imm_map(c, 8, 3, 12) | imm_map(c, 0, 8, 0);
    if (m == 16) op |= imm_map(c, 12, 4, 16);
    return op;
}

void thumb_gen::stuff_const_harder(uint32_t op, uint32_t v)
{
    uint32_t x;
    x = stuff_const(op, v);
    if (x)
        o(x);
    else
    {
        /*
        当我们
        add r0, r1, c
        时，假如c过大，那么如下处理
        op: add r0, r1, c & 0xff
        o2: add r0, r0, c & 0xff00
        o2: add r0, r0, c & 0xff0000
        o2: add r0, r0, c & 0xff000000
        */
        uint32_t a[24], o2, no; 
        int i;
        a[0] = 0xff << 24;
        /* o2的源操作和目的操作数需要修正成同一个 */
        o2 = (op & 0xfff0ffff) | (op & 0x0f00);
        no = o2 ^ 0xa00000;
        for (i = 1; i < 24; i++)
            a[i] = a[i - 1] >> 1;

        o(stuff_const(op, v & 0xff));
        if (!(v & 0x8000) || !(v & 0x800000) || !(v & 0x80000000)) vm_error("stuff_const cant fit instruction");

        o(stuff_const(o2, v & 0xff00));
        if (!(v & 0x8000)) 
            o(stuff_const(no, v & 0x8000));
        o(stuff_const(o2, v & 0xff0000));
        if (!(v & 0x800000))
            o(stuff_const(no, v & 0x800000));
        o(stuff_const(o2, v & 0xff000000));
        if (!(v & 0x80000000))
            o(stuff_const(no, v & 0x80000000));
    }
}

int _push(int32_t reglist)
{
#define T1_REGLIST_MASK             0x40ff         
#define T2_REGLIST_MASK             0x5fff

    if (ANDNEQ(reglist, T1_REGLIST_MASK)) 
        o(0xb400 | (reglist & 0xff) | ((reglist & 0x4000) ? 0x100:0));
    else if (ANDNEQ(reglist, T2_REGLIST_MASK)) 
        o(0xe92d0000 | reglist);
    return 0;
}

/* A8.8.368 */
void _vpush(int s, uint32_t reglist)
{
    uint32_t n = ntz(reglist), bc = bitcount(reglist), b = reglist >> n, x = 0;

    if (((b + 1) & b))
        vm_error("reglist[%x] must continuous 1 bit", reglist);

    if (s) { // T2
        x = 0xed2d0a00 | imm_map(n, 4, 1, 22) | imm_map(n, 0, 4, 12) | bc;
    }
    else { // T1
        if ((bc & 1) || (n & 1))
            vm_error("reglist[%x] bitcount must event 1 bit", reglist);

        n /= 2;
        x = 0xed2d0b00 | imm_map(n, 4, 1, 22) | imm_map(n, 0, 4, 12) | bc;
    }
    o(x);
}

/* A8.8.131 */
void _pop(int32_t reglist)
{
    /* A8.8.131 */
    if (ANDNEQ(reglist, 0x80ff)) {
        if (bitcount(reglist) < 1) UNPREDICITABLE();
        o(0xbc00 | (reglist & 0xff) | ((reglist & 0x8000) ? 0x100 : 0));
    }
    else if (ANDNEQ(reglist, 0xdfff)) {
        if (bitcount(reglist) < 2 || ANDEQ(reglist, 0xc000)) UNPREDICITABLE();
        o(0xe8bd0000 | (reglist & 0xdfff));
    }
}

/* A8.8.47 */
void _xor_reg(int rd, int rn, int rm, SRType shtype, int sh)
{
    if (t1_param_check1(rd,rn,rm,shtype,sh) && g_setflags) // t1
		o(0x4040 | (rm << 3) | rd);	
	else if (rd != 13 && rd != 15 && rn != 13 && rn != 15) // t2
        o(0xea800000 | T_PARAM_MAP(g_cpsr_follow, rd, rn, rm, shtype, sh));
}

/* A8.8.238 */
void _teq_reg(int rn, int rm, int shift)
{
    if (shift) UNPREDICITABLE();

    o(0xea900f00 | (rn << 16) | rm);
}

/* A8.8.123 */
void _orr_reg(int rd, int rn, int rm, SRType sht, int shval)
{
    if (g_setflags && (rd == rn) && rm < 8 && rn < 8 && (sht == SRType_LSL) && !shval)
        o(0x4300 | (rm << 3) | rd);
    else if (rd != 13 && rd != 15 && rn != 13 && rm != 13 && rm != 15) 
        o(0xea400000 | (rn << 16) | (rd << 8) | rm | (g_setflags << 20) | SR4_IMM_MAP5(sht,shval));
	else
		vm_error ("internal error: th_orr_reg invalid parameters\n");
}

/* A8.8.122 */
void _orr_imm(int rd, int rn, SRType sht, int shval)
{
    o(0x40400000 | (rn << 16) | (rd << 8) | thumb_gen::stuff_const(0, shval));
}

void _adr(int rd, int imm)
{
    int x = abs(imm), add = imm >= 0;

    if (x >= 4096) return;

    if (align4(imm) && (imm < 1024) && (rd < 8))
        o(0xa000 | (rd << 8) || (imm >> 2));
    else if (!add)
        o(0xf2af0000 | (rd << 8) | stuff_constw(0, x, 12));
    else
        o(0xf20f0000 | (rd << 8) | stuff_constw(0, x, 12));
}

/* A8.8.2 adc */
void _adc_reg(int rd, int rn, int rm, enum SRType shtype, int sh)
{
    if (t1_param_check1(rd, rn, rm, shtype, sh) && g_setflags)
        o(0x4140 | (rm << 3) | rd);
    else
        o(0xeb400000 | T_PARAM_MAP(g_setflags,rd,rn,rm,shtype,sh));
}

/* A8.8.6 */
void _add_reg(int rd, int rn, int rm, enum SRType shtype, int sh)
{
    if (t1_param_check(rd,rn,rm,shtype,sh) && g_setflags) // t1
        o(0x1800 | (rm << 6) | (rn << 3) | rd);
    else if (!sh && (rd == rn) && !g_cpsr_follow) // t2
        o(0x4400 | (rm << 3) | (rd & 7) | (rd >> 3 << 7));
    else // t3
        o(0xeb000000 | T_PARAM_MAP(g_setflags, rd, rn, rm, shtype, sh));
}

/* A8.8.4 */
void _add_imm(int rd, int rn, int imm)
{
    int a = abs(imm), add = imm >= 0;

    if (add && imm < 8 && rd < 8 && rn < 8)
        o(0x1c00 | (imm << 6) | (rn << 3) | rd); // t1
    else if (add && imm < 256 && rd < 8 && (rd == rn))
        o(0x3000 | (rd << 8) | imm); // t2
    else if (add && imm < 4096)
        o(0xf2000000 | (rn << 16) | (rd << 8) | imm_map(imm, 11, 1, 26) | imm_map(imm, 8, 3, 12) | imm_map(imm, 0, 8, 0));
    else {
        uint32_t x = thumb_gen::stuff_const(0, imm);
        if (x) {
            o(0xf1000000 | x | (rn << 16) | (rd << 8));
        }
    }
}

void _and_reg(int rd, int rn, int rm, enum SRType sh_type, int shift, int setflags)
{
    if ((rd == rn) && (rd < 8) && (rm < 8) && setflags && (sh_type == SRType_LSL) && !shift) // T1
        o(0x4000 | (rm << 3) | rd);
    else
        o(0xea000000 | (setflags << 20) | (rn << 16) | (rd << 8) | rm | SR4_IMM_MAP5(sh_type, shift));
}

int thumb_gen::_add_sp_imm(int rd, int rn, uint32_t imm)
{
    /* A8.8.9 */
    if (rn == SP) {
        if (align4(imm) && (imm < 1024) && (rd < 8))
            o(0xa800 | (rd << 8) | (imm >> 2));
        else if ((rd == SP) && align4(imm) && (imm < 512))
            o(0xb000 | (imm >> 2));
        else if (imm < 4096)
            o(stuff_constw(0xf20d0000 | (rd << 8), imm, 12));
        else
            stuff_const_harder(0xf10d0000, imm);
    }

    return 0;
}

/* A8.8.19 */
void _bfc(int rd, int lsb, int width)
{
    int msb = lsb + width - 1;

    o(0xf36f0000 | (rd << 8) | imm_map(lsb, 3, 2, 12) | imm_map(lsb, 0, 2, 6) | msb);
}

void _bic(int rd, int rn, int rm, SRType shtype,  int shift, int setflags)
{
    if (setflags && !shift && (rd == rn) && (rd < 8) && (rn < 8))
        o(0x4380 | (rm << 3) | (rd << 3));
    else
        o(0xea200000 | (setflags << 20) | (rd << 8) | (rn << 16) | rm | (shtype << 4) | imm_map(shift, 2, 3, 12) | imm_map(shift, 0, 2, 6));
}

/* A8.8.25 bl or blx */
void _bl_x_imm(int arm, int to)
{
    uint32_t x;
    if (arm) {
        if ((x = encbranch2(g_cg->ind, to, arm)) == (uint32_t)-1) {
            o(NOP1);
            x = encbranch2(g_cg->ind, to, arm);
        }
        o(0xf000c000 | x);
    }
    else
        o(0xf000d000 | encbranch2(g_cg->ind, to, arm));
}

/* A8.8.221 */
void thumb_gen::_sub_imm(int rd, int rn, uint32_t imm, int setflags)
{
    if ((rd == rn) && (rn == SP)) {
        _sub_sp_imm(imm, setflags);
        return;
    }

    if (setflags && (imm < 8) && (rd < 8) && (rn < 8)) // t1
        o(0x1e00 | (imm << 6) | (rn << 3) | rd);
    else if (setflags && imm < 256 && (rd == rn) && (rn < 8)) // t2
        o(0x3800 | imm | (rn << 8));
    else if (imm <= 0x0fff && rd != 13 && rd != 15) { // t4
        o(stuff_constw(0xf2a00000 | (rn << 16) | (rd << 8), imm, 0) | (setflags << 20));
    }
    else if (!setflags)
        stuff_const_harder(0xf1a00000 | (rn << 16) | (rd << 8), imm);
}

void thumb_gen::_sub_sp_imm(int imm, int setflags)
{
    if (align4(imm) && (imm < 512)) /* ALIGN 4 */
        o(0xb080 | (imm >> 2));
    else if (imm < 4096) //subw, T3
        o(stuff_constw(0xf2ad0000, imm, 12) | (SP << 8));
    else // T2
        o(stuff_const(0xf1ad0000 | (SP << 8), imm));
}

void _sub_reg(int rd, int rn, int rm, SRType shtype, int sh)
{
    if (!sh && rd < 8 && rn < 8 && rm < 8)
        o(0x1a00 | (rm << 6) | (rn << 3) | rd);
    else
        o(0xeba00000 | (rn << 16) | (rd << 8) | rm | (shtype << 4) | ((sh & 3) << 6) | (sh >> 2 << 12));
}

/* A8.8.38 */
void _cmp_reg(int rn, int rm)
{
    if (rn < 8 && rm < 8)
        o(0x4280 | (rm << 3) | rn);
    else
        o(0x4500 | (rm << 3) | (rn & 7) | ((rn >> 3) << 7));
}

/* A8.8.46 */
void _eor_imm(int rd, int rn, int imm, int setflags)
{
    o(0xf0800000 | (setflags << 20) | (rn << 16) | (rd << 8) | thumb_gen::stuff_const(0, imm));
}

/* A8.8.47 */
void _eor_reg(int rd, int rn, int rm, SRType shtype, int shval, int setflags)
{
    if (setflags && (rd == rn) && (rd < 8) && (rn < 8) && (shtype == SRType_LSL) && !shval)
        o(0x4040 | (rm << 3) | rd);
    else
        o(0xea800000 | (setflags << 20) | (rn << 16) | (rd << 8) | rm | SR4_IMM_MAP5(shtype, shval));
}


void thumb_gen::_cmp_imm(int rn, uint32_t imm)
{
    uint32_t x, rm;

    if ((imm < 256) && (rn < 8))
        o(0x2800 | (rn << 8) | imm);
    else if ((x = stuff_const(0, imm)))
        o(0xf1b00f00 | x | (rn << 16));
    else {
        rm = regalloc(curp);
        _mov_imm(rm, imm, 0, 0);
        _cmp_reg(rn, rm);
    }
}

/* A8.8.72 */
void _ldrd_imm(int rt, int rt2, int rn, int imm)
{
    int add = imm >= 0;
    imm = abs(imm);

    if (imm < 1024 && align4(imm)) 
        o(0xe9500000 |  (rn << 16) | (rt << 12) | (rt2 << 8) | (imm >> 2) | (add << 23));
    /* wback ? */
}

/* 这个指令和其他的ldr指令不一样的地方在于，传入的是一个绝对pos，
需要根据当前的pc(g_cg->ind)转成一个PIC code*/
int _ldr_lit(int rt, int pos)
{
    int imm = pos - ((g_cg->ind + 4) & ~3), add = imm >= 0, x = abs(imm);
    int imm8 = x >> 2;

    if ((imm >= 0) && (imm < 1024) && align4(imm))
        o(0x4800 | (rt << 8) | imm8);
    else if (x < 4096)
        o(0xf85f0000 | (add << 23) | (rt << 12) | x);
    else
        return -1;

    return 0;
}

/* A8.8.62 */
void _ldr_imm(int rt, int rn, int imm, int wback)
{
    int x = abs(imm), add = imm >= 0, p = imm != 0;

    if (!wback && add && align4(imm) && rt < 8) {
        if (rn == SP && imm < 1024) {
            o(0x9800 | (rt << 8) | (imm >> 2));     // T1
            return;
        }
        else if (rn < 8 && imm < 128) {
            o(0x6800 | ((imm >> 2) << 6) | (rn << 3) | rt);     // T2
            return;
        }
    }

    if (!wback && add && imm < 4096)
        o(0xf8d00000 | (rn << 16) | (rt << 12) | imm);  // T3
    else if (x < 256)
        o(0xf8500800 | (rn << 16) | (rt << 12) | (add << 9) | x | (wback << 8) | (p << 10)); // T4
}

void _ldr_reg(int rt, int rn, int rm, int lsl)
{
    if (!lsl && rt < 8 && rn < 8 && rm < 8)
        o(0x5800 | (rm << 6) | (rn << 3) | rt);
    else
        o(0xf8500000 | (rn << 16) | (rt << 12) | (lsl << 4) | rm);
}

/* A8.8.67 */
void _ldrb_imm(int rt, int rn, int imm, int wback)
{
    int x = abs(imm), add = imm >= 0, p = 1;

    if (add && imm < 32 && rn < 8 && rt < 8)
        o(0x7800 | (imm << 6) | (rn << 3) | rt);
    else if (add && (imm < 4096))
        o(0xf8900000 | (rn << 16) | (rt << 12) | imm);
    else if (x < 256)
        o(0xf8100800 | (rn << 16) | (rt << 12) | x | (p << 10) | (add << 9) | (wback << 8));
}

void _ldrb_reg(int rt, int rn, int rm, int lsl)
{
    if (rt < 8 && rn < 8 && rm < 8 && !lsl)
        o(0x5c00 | (rm << 6) | (rn << 3) | rt);
    else
        o(0xf811 | (rt << 12) | (rn << 16) | rm | (lsl << 4));
}

/* A8.8.94 */
void _lsl_imm(int rd, int rm, int imm, int setflags)
{
    if (imm >= 32) return;

    if (setflags && rm < 8 && rd < 8)
        o((imm << 6) | (rm << 3) | (rd << 3));
    else
        o(0xea4f0000 | (rd << 8) | rm | imm_map(imm, 2, 3, 12) | imm_map(imm, 0, 2, 6) | (setflags << 20));
}

void _lsl_reg(int rd, int rn, int rm)
{
    if ((rd == rn) && (rm < 8) && (rd < 8) && g_setflags)
        o(0x4080 | (rm << 3));
    else
        o(0xfa00f000 | (g_setflags << 20) | (rn << 16) | (rd << 8) | rm);
}

/* A8.8.96 */
void _lsr_imm(int rd, int rm, int imm)
{
    if (imm >= 32) return;

    if (rm < 8 && rd < 8)
        o(0x0800 | (imm << 6) | (rm << 3) | rd);
    else
        o(0xea4f0010 | (rd << 8) | rm | imm_map(imm, 2, 3, 12) | imm_map(imm, 0, 2, 6));
}

/* A8.8.97 */
void _lsr_reg(int rd, int rn, int rm)
{
    if ((rd == rn) && (rd < 8) && (rm < 8) && g_setflags)
        o(0x40c0 | (rm << 3) | rd);
    else
        o(0xfa20f000 | (g_setflags << 20) | (rn << 16) | (rd << 12) | rm);
}

/* A8.8.100 */
void _mla(int rd, int rn, int rm, int ra)
{
    o(0xfb000000 | (rn << 16) | (ra << 12) | (rd << 8) | rm);
}

/* A8.8.114 */
void _mul(int rd, int rn, int rm)
{
    if (rd < 8 && rn < 8 && (rd == rm))
        o(0x4340 | (rn << 3) | rd);
    else
        o(0xfb00f000 | (rn << 16) | (rd << 8) | rm);
}

void _rsb_imm(int rd, int imm, int rn)
{
    uint32_t x;

    if (rd < 8 && rn < 8 && !imm)
        o(0x4240 | (rn << 3) | rd);
    else if (x = thumb_gen::stuff_const(0, imm))
        o(0xf1c00000 | x | (rn << 16) | (rd << 8));
}

/* A8.8.153 */
void _rsb_reg(int rd, int rn, int rm, SRType shtype, int shift, int setflags)
{
    o(0xebc00000 | (rn << 16) | (rd << 8) | rm | (setflags << 20)| SR4_IMM_MAP5(shtype, shift));
}

/* A8.8.162 */
void _sbc_reg(int rd, int rn, int rm, SRType shtype, int shval)
{
    if ((rd == rn) && (rn < 8) && (rm < 8) && g_setflags) // t1
        o(0x4180 | (rm << 3) | rd);
    else
        o(0xeb600000 | (rn << 16) | (rd << 8) | rm | (g_setflags << 20) | SR4_IMM_MAP5(shtype, shval));
}

void _str_reg(int rt, int rn, int rm, int lsl)
{
    if (!lsl && rt < 8 && rn < 8 && rm < 8)
        o(0x5000 | (rm << 6) | (rn << 3) | rt);
    else 
        o(0xf8400000 | (rn << 16) | (rt << 12) | (lsl << 4) | rm);
}

/* A8.8.203 */
void _str_imm(int rt, int rn, int imm, int wback)
{
    int add = imm >= 0, pos = abs(imm), p = pos >= 0;

    if (add && align4(imm) && imm < 128 && rt < 8 && rn < 8) // t1
        o(0x6000 | (imm >> 2 << 6) | rt | (rn << 3));
    else if (add && (rn == SP) && rt < 8 && align4(imm) && (imm < 1024)) // t2
        o(0x9000 | (rt << 8) | (imm >> 2));
    else if (add && imm < 4096) // t3
        o(0xf8c00000 | (rn << 16) | (rt << 12) | imm);
    else if (pos < 256)
        o(0xf8400800 | pos | (rt << 12) | (rn << 16) | (p << 10) | (add << 9) | (wback << 8));
}

void _strd(int rt, int rt2, int rn, int imm, int w)
{
    int add = imm >= 0, pos = abs(imm), p = imm != 0;

    if (align4(pos) && (pos < 1024))
        o(0xe8400000 | (pos >> 2) | (add << 23) | (rn << 16) | (rt << 12) | (rt2 << 8) | (1 << 24) | (w << 21));
}

/* A8.8.206 

@w  writeback
*/
void _strb_imm(int rt, int rn, int imm, int w)
{
    int x, u;

    if (imm >= 0 && imm < 32 && rt < 8 && rn < 8) // t1
        o(0x7000 | (rn << 3) | rt | (imm << 6));
    else if (imm >= 0 && imm < 4096) // t2
        o(0xf8800000 | (rn << 16) | (rt << 12) | imm);
    else if (imm > -255 && imm < 256) { // T3
        x = abs(imm);
        u = imm >= 0;
        /* op | P | */
        o(0xf8000800 | (1 << 10) | (rn << 16) | (rt << 12) | x | (w << 8) | (u << 9));
    }
}

/* A8.8.208 */
void _strb_reg(int rt, int rn, int rm, int shval)
{
    if (rt < 8 && rn < 8 && rm < 8 && !shval)
        o(0x5400 | (rm << 6) | (rn << 3) | rt);
    else
        o(0xf8000000 | (rn << 16) | (rt << 12) | rm | (shval << 4));
}

void _tst_reg(int rn, int rm, SRType shtype, int shval)
{
    if (rn < 8 && rm < 8 && (shtype == SRType_LSL) && !shval) {
        o(0x4200 | (rm << 3) | rn);
    }
    else {
        o(0xea100f00 | (rn << 16) | rm | SR4_IMM_MAP5(shtype, shval));
    }
}

/*
_mov_imm一般的情况，就是参照arm_arch_ref进行对照即可，但是有几张情况需要说明一下

1. 尽可能的把数赛进去一条指令
2. 最糟糕的情况是用 movw, movt 2条指令来生成一个 立即数拷贝，比起最好的情况，它多占用了6个字节
3. 填充数的逻辑是 T1 -> T3 -> T2 -> ldr搜索 -> 2指令填充，看代码即可
4. ldr搜索是指，搜索原先的代码填入的一些DATA，假如没有被擦，那么尝试用ldr访问
*/
void thumb_gen::_mov_imm(int rd, uint32_t v, int setflags, int out_cpsr_dead)
{
    const_item *item;

    /* A8.8.102 */
    if ((setflags || out_cpsr_dead) && (v < 256) && (rd < 8)) 
        o(0x2000 | (rd << 8) | v); // T1
    else if (!setflags && (v < 65536)) { // T3
        o(stuff_constw(0xf2400000 | (rd << 8), v, 16));
    }
    else {
        uint32_t x = stuff_const(0xf04f0000 | (rd << 8), v); // T2
        if (x) {
            o(x);
            return;
        }

        /* 假如constmap里面有这个值 */
        item = constmap[v];
        if (item && !_ldr_lit(rd, item->ind))
            return;

        /* 
        碰见超大数
        mov rd, v.lo
        movt rd, v.hi
        */
        o(stuff_constw(0xf2400000 | (rd << 8), v & 0xffff, 16));
        v >>= 16;
        o(stuff_constw(0xf2c00000 | (rd << 8), v, 16));
    }
}

/* A8.8.103 */
void _mov_reg(int rd, int rm, int s)
{
    if (!s)
        o(0x4600 | (rm << 3) | (rd & 7) | (rd >> 3 << 7));
    else if (s && (rd < 8) && (rm < 8))
        o(0x0000 | (rm << 3) | rd);
    else
        o(0xea4f0000 | (s << 20) | (rd << 8) | rm);
}

/* A8.8.116 */
void _mvn_reg(int rd, int rm, SRType shtype, int shift, int setflags)
{
    if (setflags && (rd < 8) && (rm < 8) && (shtype == SRType_LSL) && (shift == 0))
        o(0x43c0 | (rm << 3) | rd);
    else
        o(0xea6f0000 | (setflags << 20) | (rd << 8) | rm | (shtype << 4) | imm_map(shift, 3, 2, 12) | imm_map(shift, 2, 0, 6));
}


/* A8.8.101 */
void _mls(int rd, int rn, int rm, int ra)
{
    o(0xfb000010 | (rn << 16) | (ra << 12) | (rd << 8) | rm);
}

/* A8.8.274 */
void _uxtb(int rd, int rm, int rotate)
{
    if (rd < 8 && rm < 8) // t1
        o(0xb260 | (rm << 3) | rd);
    else // t2
        o(0xfa5ff080 | (rd << 8) | (rotate << 4) | rm);
}

/* A8.8.339 */
void vmov_imm(int siz, int vd, uint64_t imm)
{
    /* T1 , vd是以4字节为单位的，当传入后，需要转换到 Dd, Qd为单位 */
    uint32_t x = 0xef800010, d = vd / 2;
    if (siz == 16) x |= (1 << 6);
    o(stuff_constv(x, imm) | imm_map(d, 4, 1, 22) | imm_map(d, 0, 4, 12));
}

uint32_t encbranch(int pos, int addr, int fail)
{
    /* A8.8.18 */
    addr -= pos + 4;
    addr /= 2;
    if ((addr >= 0xEFFF) || addr < -0xEFFF) {
        vm_error("FIXME: function bigger than 1MB");
        return 0;
    }
    return 0xf0008000 | imm_map(addr, 31, 1, 26) | imm_map(addr, 18, 1, 11) | imm_map(addr, 17, 1, 13) | imm_map(addr, 11, 6, 16) | imm_map(addr, 0, 11, 0);
}

char *vstl_slgh[] = {
    "ma:4 = copy rn:4" ,
    ""
};

pit thumb_gen::g_vstl(flowblock *b, pit pit)
{
    return retrieve_orig_inst(b, pit, 1);
}

pit thumb_gen::g_vpop(flowblock *b, pit pit)
{
    return retrieve_orig_inst(b, pit, 1);
}

void thumb_gen::topologsort()
{
}

int thumb_gen::run()
{
    int i;
    uint32_t x;

    preprocess();
    //fd->bblocks.dump_live_set(fd->find_op(SeqNum(Address(), 2170)));
    //fd->dump_cfg(fd->name, "exe_pre", 1);

    /* 对block进行排序 

    */
    sort_blocks(blist);

    /* 针对每一个Block生成代码，但是不处理末尾的jmp */
    for (i = 0; i < blist.size(); i++) {
        flowblock *b = blist[i];
        //printf("index = %d, index = %d, b = %p, this = %p, dead = %d\n", i, ind, b, this, b->flags.f_dead);
        run_block(b, i);

        if (b->is_rel_branch()) {
            /* FIXME:bblock的start_addr起始和first op的addr可能不是一回事，一般出现在复制的情况下 */
            while (blist[i + 1]->first_op()->get_addr() == b->first_op()->get_addr()) i++;
        }
    }

    /* 修复末尾的跳转 */
    printf("fix jmp list\n");
    for (i = 0; i < flist.size(); i++) {
        fix_item *item = flist[i];

        /* 读取出原先的inst，然后取出其中的cond，生成新的inst，然后写入buf */
        if (item->cond == COND_AL) {
            x = 0xf0009000;
            x |= encbranch2(item->from, item->to_blk->cg.data - data, 0);
        }
        else {
            x = read_thumb2(data + item->from);
            x =  (x & 0x03c00000)| encbranch(item->from, item->to_blk->cg.data - data, 0);
        }
        write_thumb2(x, data + item->from);
        //dump_one_inst(item->from, NULL);
    }
    printf("fix jmp list success\n");

    return 0;
}

void thumb_gen::save(void)
{
}

int thumb_gen::run_block(flowblock *b, int b_ind)
{
    vector<fix_vldr_item> fix_vldr_list;
    vector<fix_vld1_item> fix_vld1_list;
    list<pcodeop *>::iterator it, it1;
    pcodeop *p;
    uint32_t x, rt, rd, rn, rm, setflags;
    pc_rel_table pc_rel_tab;
    int oind, imm, i, size;
    uint8_t fillbuf[MAX_INST_SIZ];

    b->cg.data = data + ind;

    for (it = b->ops.begin(); it != b->ops.end(); ++it) {
        curp = p = *it;

        /* branch 指令不处理，由外层循环进行填充 */
        if (p->opcode == CPUI_BRANCH) continue;
        /* phi节点不处理 */
        if (p->opcode == CPUI_MULTIEQUAL) continue;

        vector<pcodeop *> ps;

        for (it1 = it; (it1 != b->ops.end()) && ((*it1)->get_addr() == (*it)->get_addr()); it1++)
            ps.push_back(*it1);

#define p1      ((ps.size() >= 2) ? ps[1]:NULL)
#define p2      ((ps.size() >= 3) ? ps[2]:NULL)
#define p3      ((ps.size() >= 4) ? ps[3]:NULL)
#define p4      ((ps.size() >= 5) ? ps[4]:NULL)

        p->ind = oind = ind;
        setflags = 0;

        if (dobc::singleton()->is_simd(p->get_addr())) {
            if (p->output && p->output->is_pc_constant()) {
                intb loadaddr;
                int loadsize;

                get_load_addr_size(p, loadaddr, loadsize);
                if (d->is_vldr(p->get_addr()))
                    fix_vldr_list.push_back(fix_vldr_item(p, p3, ind));
                else if (d->is_vld(p->get_addr())) {
                    fix_vld1_list.push_back(fix_vld1_item(loadaddr, loadsize));
                }
                else
                    vm_error("meet not support pc-rel-offset simd instruction");
            }
            it = retrieve_orig_inst(b, it, 1);
            goto inst_label;
        }

        switch (p->opcode) {
        case CPUI_COPY:
            /* push */
            if (poa(p) == ama) {
                if ((pi0a(p) == asp) && p1) {
                    if (p1->opcode == CPUI_LOAD)
                        it = g_pop(b, it);
                    else
                        it = g_push(b, it);
                }
                else if (d->is_greg(pi0a(p)))
                    it = g_vstl(b, it);
            }
            else if (poa(p) == alr) {
                if (p1 && (p1->opcode == CPUI_COPY) && poa(p1) == d->get_addr("ISAModeSwitch")
                    && (p2->opcode == CPUI_COPY) && poa(p2) == d->get_addr("TB")
                    && (p3->opcode == CPUI_CALL) && (pi0(p3)->get_addr().getSpace() == d->ram_spc)) {

                    imm = pi0(p3)->get_addr().getOffset();
                    int tb = p2->output->get_val();
                    _bl_x_imm(!tb, imm);
                }
                else if (p1 && (p1->opcode == CPUI_CALL) && (pi0(p1)->get_addr().getSpace() == d->ram_spc)) {
                    imm = pi0(p1)->get_addr().getOffset();
                    _bl_x_imm(0, imm);
                }
                else if (pi0(p)->is_constant()) {
                    _mov_imm(LR, pi0(p)->get_val(), 0, 0);
                }
                else if (isreg(pi0(p))) {
                    _mov_reg(reg2i(poa(p)), reg2i(pi0a(p)), follow_by_set_cpsr(p));
                }
                it = advance_to_inst_end(it);
            }
            else if (pi0(p)->is_hard_constant()) {
                if (isreg(p->output))
                    _mov_imm(reg2i(poa(p)), pi0(p)->get_val(), 0, pcode_cpsr_out_dead(p));
                else if (istemp(p->output)){
                    if (isreg(p1->output)) {
                        switch (p1->opcode) {
                        case CPUI_COPY:
                            _mov_imm(reg2i(poa(p1)), pi0(p)->get_val(), 0, 0);
                            break;

                        case CPUI_LOAD:
                            /* FIXME:假如load指令调用了一个常数，那么它一定是相对pc寄存器的？ */
                            rn = reg2i(poa(p1));
                            imm = pi0(p)->get_val();
                            /*
                            pc - (ind) - 4
                            */
                            _mov_imm(rn, imm - (ind + 4 * (stuff_const(0, imm) ? 1:2)) - 4, 0, 0);

                            UPDATE_CPSR_SET(p1);
                            _add_reg(rn, rn, PC, SRType_LSL, 0);
                            _ldr_imm(rn, rn, 0, 0);
                            break;

                        case CPUI_INT_ADD:
                            rn = reg2i(pi0a(p1));
                            if (rn == SP)
                                _add_sp_imm(reg2i(poa(p1)), rn, pi0(p)->get_val());
                            else
                                _add_imm(reg2i(poa(p1)), rn, pi0(p)->get_val());
                            break;

                        case CPUI_INT_SUB:
                            if (p->output == pi0(p1))
                                _rsb_imm(reg2i(poa(p1)), pi0(p)->get_val(), reg2i(pi1a(p1)));
                            else {
                                _sub_imm(reg2i(poa(p1)), reg2i(pi0a(p1)), pi0(p)->get_val(), follow_by_set_cpsr(p));
                            }
                            it = advance_to_inst_end(it);
                            break;

                        case CPUI_INT_XOR:
                        case CPUI_INT_AND:
                        case CPUI_INT_OR:
                        case CPUI_INT_RIGHT:
                        case CPUI_INT_LEFT:
                            it = retrieve_orig_inst(b, it, 1);
                            break;
                        }

                    }
                    else if (istemp(p1->output)) {
                        it = retrieve_orig_inst(b, it, 1);
                    }
                    else if (d->is_tsreg(poa(p1))) {
                        if ((p1->opcode == CPUI_INT_SBORROW) || (p1->opcode == CPUI_INT_LESSEQUAL)) {
                            it = retrieve_orig_inst(b, it, 1);
                        }
                    }
                    it = advance_to_inst_end(it);
                }
                else if (d->is_vreg(poa(p))) {
                    int size = p->output->get_size();
                    if (p1 && d->is_vreg(poa(p1)) && pi0(p1)->is_constant() && pi0(p)->get_val() == pi0(p1)->get_val()) {
                        size += p1->output->get_size();
                    }
                    vmov_imm(size, d->vreg2i(poa(p)), pi0(p)->get_val());
                    it = advance_to_inst_end(it);
                }
                else if (d->is_tsreg(poa(p))) {
                    if (poa(p) == as("tmpZR") && poa(p1) == azr) {
                        int rd = regalloc(p);
                        imm = pi1(p)->get_val();
                        _mov_imm(rd, !imm, 1, 0);
                    }
                    it = advance_to_inst_end(it);
                }
            }
            else if (pi0(p)->is_hard_pc_constant()) {
                if (isreg(p->output)) {
                    /*  _adr一般都是占位符号，不用节省空间，直接用4字节充满 */
                    _adr(reg2i(poa(p)), 4095);
                    pc_rel_tab[pi0(p)->get_val()] = p;
                }
            }
            else if (istemp(p->output)) {
                switch (p1->opcode) {
                case CPUI_INT_ADD:
                    UPDATE_CPSR_SET(p1);
                    _add_reg(reg2i(poa(p1)), reg2i(pi0a(p1)), reg2i(pi0a(p)), SRType_LSL, 0);
                    break;

                case CPUI_INT_SUB:
                    _sub_reg(reg2i(poa(p1)), reg2i(pi0a(p1)), reg2i(pi0a(p)), SRType_LSL, 0);
                    break;
                }
                it = advance_to_inst_end(it);
            }
            else if (isreg(pi0(p))) {
                if (isreg(p->output)) {
                    rd = reg2i(poa(p));
                    /* A8.8.103*/
                    _mov_reg(rd, reg2i(pi0a(p)), 0);
                }
                else if (istemp(p->output)) {
                    switch (p1->opcode) {
                    case CPUI_INT_SUB:
                        _sub_reg(reg2i(poa(p1)), reg2i(pi0a(p1)), reg2i(pi0a(p)), SRType_LSL, 0);
                        break;
                    }

                    it = advance_to_inst_end(it);
                }
            }
            /* 是从内存的某个地址加载的一个字

            FIXME:要从代码段加载的数据才是有意义的
            */
            else if (isreg(p->output) && pi0(p)->in_ram() && p->output->is_constant()) {
                _mov_imm(reg2i(poa(p)),  p->output->get_val(), 0, 0);
            }
            break;

        case CPUI_LOAD:
            if (isreg(p->output)) {
                if (pi1(p)->is_constant()) {
                    rd = reg2i(poa(p));
                    _mov_imm(rd, pi1(p)->get_val(), 0, 0);
                    o(0xf8d00000 | (rd << 12) | (rd << 16) | 0);
                }
                else if ((pi1a(p) == ama) && p1->opcode == CPUI_INT_ADD) {
                    it = retrieve_orig_inst(b, it, 1);
                }
            }
            break;

        case CPUI_CALLOTHER:
            it = retrieve_orig_inst(b, it, 1);
            break;

        case CPUI_INT_NOTEQUAL:
        case CPUI_INT_EQUAL:
            if (istemp(p->output) ) {
                /* A8.3 

                FIXME:这一整块都需要重写，需要依赖根据 A8.3 的规则，自动计算，而不能简单的Pattern进行匹配
                */
                if ((pi0a(p) == azr) && pi1(p)->is_constant()) {
                    imm = pi1(p)->get_val();
                    /*

                mov.eq r1, xxxx 转换以后

                    t0 = INT_NOTEQUAL zr, 0
                    t1 = BOOL_NEGATE t0
                    cbranch true_label t1
         false_label:


          true_label:

                    这个是ne


                    warn: sleigh在转it指令时，mov.后面的条件，走的是false的分支，为真的条件实际上是相反的。
                    */
                    if (!imm && (p1->opcode == CPUI_BOOL_NEGATE) && (p2->opcode == CPUI_CBRANCH)) {
                        write_cbranch(b, ((p->opcode == CPUI_INT_NOTEQUAL) ? COND_NE : COND_EQ));
                    }
                    else if (imm && p1->opcode == CPUI_CBRANCH) {
                        write_cbranch(b, ((p->opcode == CPUI_INT_NOTEQUAL) ? COND_NE : COND_EQ));
                    }
                    else if (!imm && p1->opcode == CPUI_CBRANCH) {
                        write_cbranch(b, ((p->opcode == CPUI_INT_NOTEQUAL) ? COND_EQ : COND_NE));
                    }
                }
                else if ((pi0a(p) == acy) && pi1(p)->is_constant()) {
                    imm = pi1(p)->get_val();
                    if (p->opcode == CPUI_INT_NOTEQUAL) imm = !imm;
                    if (p1->opcode == CPUI_BOOL_NEGATE) imm = !imm;

                    write_cbranch(b, imm ? COND_CS : COND_CC);

                }
                else if ((pi0a(p) == ang) && pi1(p)->is_hard_constant()) {
                    imm = pi1(p)->get_val();
                    if (p->opcode == CPUI_INT_NOTEQUAL) imm = !imm;
                    if (p1->opcode == CPUI_BOOL_NEGATE) imm = !imm;
                    write_cbranch(b, imm ? COND_MI : COND_PL);
                }
                else if (pi0a(p) == ang && pi1a(p) == aov && p1->opcode == CPUI_BOOL_NEGATE && p2->opcode == CPUI_CBRANCH) {
                    write_cbranch(b, (p->opcode == CPUI_INT_EQUAL) ? COND_LT : COND_GE);
                }
                else if ((p1->opcode == CPUI_BOOL_OR) && (p2->opcode == CPUI_BOOL_NEGATE) && (p3->opcode == CPUI_CBRANCH)) {
                    if ((pi0a(p) == ang) && (pi1a(p) == aov) && (pi0a(p1) == azr)) {
                        write_cbranch(b, COND_GT);
                    }
                }
                else if ((p1->opcode == CPUI_CBRANCH) && isreg(pi0(p)) && pi1(p)->is_constant()) {
                    imm = pi1(p)->get_val();

                    _cmp_imm(reg2i(pi0a(p)), imm);
                    write_cbranch(b, ((p->opcode == CPUI_INT_NOTEQUAL) ? COND_NE : COND_EQ));
                }

                it = advance_to_inst_end(it);
            }
            else if (d->is_tsreg(poa(p))) {
                /* rsb */
                if (p1->opcode == CPUI_INT_SUB) {
                    it = retrieve_orig_inst(b, it, 1);
                }
            }
            break;

        case CPUI_INT_SEXT:
            it = retrieve_orig_inst(b, it, 1);
            break;

        case CPUI_INT_SUB:
            if (poa(p) == ama) it = g_push(b, it);
            else if (istemp(p->output)) {
                rn = d->reg2i(pi0a(p));

                switch (p1->opcode) {
                case CPUI_LOAD:
                    // ?这是什么鬼
                    if ((p1->output->size == 1) && istemp(p1->output)) {
                        _ldrb_imm(reg2i(poa(p2)), reg2i(pi0a(p)), - pi1(p)->get_val(), 0);
                    }
                    else if (!p2 || (p2->get_addr() != p1->get_addr())) {
                        _ldr_imm(reg2i(poa(p1)), reg2i(pi0a(p)), - pi1(p)->get_val(), 0);
                    }
                    break;

                case CPUI_STORE:
                    if (p1->output->size == 4) {
                        _str_imm(reg2i(pi2a(p1)), reg2i(pi0a(p)), -pi1(p)->get_val(), 0);
                    }
                    break;

                case CPUI_INT_EQUAL:
                    /* A8.8.37 */
                    if (pi1(p)->is_hard_constant()) 
                        _cmp_imm(rn, pi1(p)->get_val());
                    /* A8.8.38 */
                    else if (isreg(pi0(p)) && isreg(pi1(p))) {
                        rm = d->reg2i(pi1a(p));
                        _cmp_reg(rn, rm);
                    }
                    break;

                case CPUI_INT_SLESS:
                    it = retrieve_orig_inst(b, it, 1);
                    break;

                case CPUI_SUBPIECE:
                    if (istemp(p1->output) && p2->opcode == CPUI_STORE) {
                        _strb_imm(reg2i(pi0a(p1)), rn, -pi1(p)->get_val(), 0);
                    }
                    break;

                case CPUI_BOOL_NEGATE:
                    if (p1 && p2 && p3 && istemp(p1->output) && istemp(p2->output) 
                        && (p2->opcode == CPUI_INT_ZEXT)
                        && (p3->opcode == CPUI_INT_SUB)) {
                        UPDATE_CPSR_SET(p3);
                        _sbc_reg(reg2i(poa(p3)), reg2i(pi0a(p)), reg2i(pi1a(p)), SRType_LSL, 0);
                    }
                    break;
                }
                it = advance_to_inst_end(it);
            }
            else if (isreg(p->output)) {
                if (pi0(p)->is_hard_constant()) {
                    _rsb_imm(reg2i(poa(p)), pi0(p)->get_val(), reg2i(pi1a(p)));
                }
                else if (isreg(pi0(p)) || (pi0a(p) == ama)) {
                    rd = reg2i(poa(p));
                    rn = reg2i(pi0a(p));
                    if (isreg(pi1(p))) {
                        /* A8.8.223 */
                        rm = reg2i(pi1a(p));

                        _sub_reg(rd, rn, rm, SRType_LSL, 0);
                        it = advance_to_inst_end(it);
                    }
                    /* sub sp, sp, c 
                    A8.8.225.T1
                    */
                    else if (pi1(p)->is_constant()) {
                        if (((pi0a(p) == asp) || (pi0a(p) == ama)) && (poa(p) == asp)) {
                            if (p1 && p2 && (p1->opcode == CPUI_COPY) && (p2->opcode == CPUI_STORE) && d->is_vreg(pi2a(p2))) {
                                it = g_vpush(b, it);
                                goto inst_label;
                            }
                            _sub_sp_imm(pi1(p)->get_val(), 0);
                        }
                        else {
                            _sub_imm(rd, rn, pi1(p)->get_val(), follow_by_set_cpsr(p));
                            it = advance_to_inst_end(it);
                        }
                    }
                }
            }
            break;


        case CPUI_INT_LESSEQUAL:
        case CPUI_INT_ZEXT:
            it = retrieve_orig_inst(b, it, 1);
            break;


        case CPUI_INT_ADD:
            if (istemp(p->output)) {
                switch (p1->opcode) {
                case CPUI_COPY:
                    if ((pi0a(p) == asp) && pi1(p)->is_constant() && a(pi0(p1)) == poa(p) && isreg(p1->output))
                        _add_sp_imm(reg2i(poa(p1)), SP, pi1(p)->get_val());
                    else if (istemp(p1->output)) {
                        if (p2 && (p2->opcode == CPUI_STORE)) {
                            if (pi2(p2)->size == 1) {
                                if (!d->is_greg(pi0a(p1))) {
                                    rt = regalloc(p1);
                                    _mov_imm(rt, pi0(p1)->get_val(), 0, 0);
                                }
                                else
                                    rt = d->reg2i(pi0a(p1));
                                rn = d->reg2i(pi0a(p));
                                _strb_imm(rt, rn, pi1(p)->get_val(), 0);
                            }
                            else if (p3 && (p3->opcode == CPUI_INT_ADD) && pi0(p3) == p1->output && p4->opcode == CPUI_STORE) {
                                _strd(reg2i(pi2a(p2)), reg2i(pi2a(p4)), reg2i(pi0a(p)), pi1(p)->get_val(), 0);
                            }
                            else if (!p3 || (p3->get_addr() != p2->get_addr())) {
                                _str_imm(reg2i(pi2a(p2)), reg2i(pi0a(p)), pi1(p)->get_val(), 0);
                            }

                            it = advance_to_inst_end(it);
                        }
                        /* 
                        以下是兼容这么一种情况

                        libkwsgmain.so/sub_cb59:

                        strd r0,r12,[sp,#0x1b0]
                        ...
                        strd r0,r12,[sp,#0x1b0]

                        它们在转换pcode以后，变成这样

                        1. u1 = sp - 0x1b0
                        2. store [u1], r0
                        3. u1 = u1 - 4
                        4. store [u1], r12

                        ....
                        ....

                        n.      u1 = sp - 0x1b0
                        n+1.    store [u1], r0
                        n+2.    u1 = u1 - 4
                        n+3.    store [u1], r12

                        出于某些原因，指令2会被删除，变成这样
                        1. u1 = sp - 0x1b0
                        3. u1 = u1 - 4
                        4. store [u1], r12

                        我们转换回去时，不能再拷贝了，转成:

                        str r12, [sp,#0x1b4]
                        */
                        else if ((p2->opcode == CPUI_INT_ADD) && istemp(p2->output)) {
                            _str_imm(reg2i(pi2a(p3)), reg2i(pi0a(p)), pi1(p)->get_val() + pi1(p2)->get_val(), 0);
                        }
                    }
                    else if (isreg(p1->output) && isreg(pi0(p1)) 
                        && (p2->get_addr() == p1->get_addr())
                        && p2->opcode == CPUI_INT_ADD
                        && p3->opcode == CPUI_LOAD) {
                        it = retrieve_orig_inst(b, it, 1);
                    }
                    it = advance_to_inst_end(it);
                    break;

                case CPUI_INT_ZEXT:
                    if (istemp(p1->output) && (pi0a(p1) == acy) && (p2->opcode == CPUI_INT_ADD)) {
                        UPDATE_CPSR_SET(p2);
                        _adc_reg(reg2i(poa(p2)), reg2i(pi0a(p)), reg2i(pi1a(p)), SRType_LSL, 0);
                    }
                    it = advance_to_inst_end(it);
                    break;

                case CPUI_STORE:
                    if (a(pi1(p1)) == poa(p)) {
                        if ((ps.size() == 4) && (p2->opcode == CPUI_INT_ADD) 
                            && (p3->opcode == CPUI_STORE) && (pi0a(p2) == poa(p))) {
                            _strd(reg2i(pi2a(p1)), reg2i(pi2a(p3)), reg2i(pi0a(p)), pi1(p)->get_val(), 0);
                            advance(it, 2);
                        }
                        else
                            _str_imm(reg2i(pi2a(p1)), reg2i(pi0a(p)), pi1(p)->get_val(), 0);

                        advance(it, 1);
                    }
                    break;

                case CPUI_LOAD:
                    if (a(pi1(p1)) == poa(p)) {
                        if ((ps.size() == 4) && (pi0a(p2) == poa(p)) && (p3->opcode == CPUI_LOAD)) {
                            _ldrd_imm(reg2i(poa(p1)), reg2i(poa(p3)), reg2i(pi0a(p)), p->get_in(1)->get_val());
                        }
                        else if (istemp(p1->output) && isreg(p2->output) && p2->opcode == CPUI_INT_ZEXT) {
                            if (pi1(p)->is_hard_constant()) {
                                _ldrb_imm(reg2i(poa(p2)), reg2i(pi0a(p)), pi1(p)->get_val(), 0);
                            }
                            else
                                _ldrb_reg(reg2i(poa(p2)), reg2i(pi0a(p)), reg2i(pi1a(p)), 0);
                        }
                        else if (isreg(p1->output)){
                            rt = reg2i(poa(p1));
                            imm = pi1(p)->get_val();
                            /* A8.8.62 */
                            if ((pi0a(p) == asp) && align4(imm) && (imm < 1024) && (rt < 8)) // T2
                                o(0x9800 | (rt << 8) | (imm >> 2));
                            else if (imm < 4096) // T3
                                o(0xf8d00000 | (rt << 12) | (reg2i(pi0a(p)) << 16) | imm);
                        }

                        it = advance_to_inst_end(it);
                    }
                    break;

                case CPUI_SUBPIECE:
                    if (istemp(p1->output) && (p2->opcode == CPUI_STORE)) {
                        rt = reg2i(pi0a(p1));
                        rn = reg2i(pi0a(p));
                        if (pi1(p)->is_constant())
                            _strb_imm(rt, rn, pi1(p)->get_val(), 0);
                        else
                            _strb_reg(rt, rn, reg2i(pi1a(p)), 0);
                        advance(it, 2);
                    }
                    break;

                case CPUI_INT_ADD:
                    /*
                    u0 = pc + c;
                    rn = u0 + 
                    */
                    if (pi0(p)->flags.from_pc && isreg(p1->output)) {
                        rd = reg2i(poa(p1));
                        imm = p1->output->get_val();
                        UPDATE_CPSR_SET(p1);
                        _mov_imm(rd, imm - (ind + 4 * ((stuff_const(0, imm) || stuff_constw(0, imm, 16)) ? 1:2)) - 4, 0, 0);
                        _add_reg(rd, rd, PC, SRType_LSL, 0);
                    }
                    else if (istemp(p1->output) && isreg(p2->output) && (p2->opcode == CPUI_LOAD)) {
                        _ldr_imm(reg2i(poa(p2)), reg2i(pi0a(p)), pi1(p)->get_val() + pi1(p1)->get_val(), 0);
                    }
                    it = advance_to_inst_end(it);
                    break;
                }
            }
            else if ((pi0a(p) == asp) && pi1(p)->is_constant()) {
                if (p1 && p1->opcode == CPUI_LOAD)
                    it = g_pop(b, it);
                else if (isreg(p->output))
                    _add_sp_imm(reg2i(poa(p)), SP, pi1(p)->get_val());
            }
            else if (isreg(p->output)) {
                rd = reg2i(poa(p));
                rn = reg2i(pi0a(p));
                /* A8.8.4 */
                if (pi1(p)->is_hard_constant()) {
                    imm = (int)pi1(p)->get_val();
                    _add_imm(rd, rn, imm);
                }
                else if (isreg(pi1(p))) {
                    UPDATE_CPSR_SET(p);
                    _add_reg(reg2i(poa(p)), reg2i(pi0a(p)), reg2i(pi1a(p)), SRType_LSL, 0);
                }

                it = advance_to_inst_end(it);
            }
            break;

        case CPUI_INT_CARRY:
            /* 有一些特殊的指令，头部pcode会有int_carray，跳过去就好了，后面是正文，但是一定要是temp cpsr才行，否则会遗漏 cpsr的设置部分 */
            if (d->is_tsreg(poa(p)))
                continue;
            break;

        case CPUI_INT_SBORROW:
            if (d->is_tsreg(poa(p))) {
                if (p1->opcode == CPUI_INT_SUB && istemp(p1->output)) {
                    if (pi1(p)->is_hard_constant())
                        _cmp_imm(reg2i(pi0a(p)), pi1(p)->get_val());
                    else if (isreg(pi1(p)))
                        _cmp_reg(reg2i(pi0a(p)), reg2i(pi1a(p)));

                    it = advance_to_inst_end(it);
                }
            }
            break;

        case CPUI_INT_NEGATE:
            if (istemp(p->output)) {
                if (isreg(p1->output)) {
                    it = retrieve_orig_inst(b, it, 1);
                }
            }
            else if (isreg(p->output)) {
                _mvn_reg(reg2i(poa(p)), reg2i(pi0a(p)), SRType_LSL, 0, follow_by_set_cpsr(p));
                it = advance_to_inst_end(it);
            }
            break;


        case CPUI_INT_XOR:
            if (istemp(p->output)) {
                if (p1) {
                    if (d->is_sreg(poa(p1))) {
                        _teq_reg(reg2i(pi0a(p)), reg2i(pi1a(p)), 0);
                        advance(it, 1);
                    }
                    else if (p2 && d->is_tsreg(poa(p1)) && d->is_sreg(poa(p2))) {
                        _teq_reg(reg2i(pi0a(p)), reg2i(pi1a(p)), 0);
                        advance(it, 2);
                    }
                }
            }
            else 
                it = retrieve_orig_inst(b, it, 1);
            break;

        case CPUI_INT_AND:
            if (poa(p) == apc) {
                if (poa(p1) == alr) {
                    if (p2->opcode == CPUI_CALLIND) 
                        o(0x4780 | (reg2i(pi0a(p)) << 3));
                }
                it = advance_to_inst_end(it);
            }
            else if (istemp(p->output)) {
                if (p1->opcode == CPUI_STORE) {
                    if (isreg(pi0(p)) && pi1(p)->is_constant()) {
                        imm = pi1(p)->get_val();
                        rt = reg2i(pi2a(p1));
                        rn = reg2i(pi0a(p));
                        _str_imm(rt, rn, imm, 0);
                    }
                }
                else if (p1->opcode == CPUI_INT_RIGHT) {
                    if (isreg(p1->output)) {
                        UPDATE_CPSR_SET(p1);
                        _lsr_reg(reg2i(poa(p1)), reg2i(poa(p1)), reg2i(pi0a(p)));
                    }
                    else
                        it = retrieve_orig_inst(b, it, 1);
                }
                else if (follow_by_set_cpsr(p)) {
                    _tst_reg(reg2i(pi0a(p)), reg2i(pi1a(p)), SRType_LSL, 0);
                }

                it = advance_to_inst_end(it);
            }
            else if (isreg(p->output)) {
                _and_reg(reg2i(poa(p)), reg2i(pi0a(p)), reg2i(pi1a(p)), SRType_LSL, 0, follow_by_set_cpsr(p));
                it = advance_to_inst_end(it);
            }
            break;


        case CPUI_INT_OR:
            if (isreg(p->output)) {
                UPDATE_CPSR_SET(p);
                _orr_reg(reg2i(poa(p)), reg2i(pi0a(p)), reg2i(pi1a(p)), SRType_LSL, 0);
                it = advance_to_inst_end(it);
            }
            break;

        case CPUI_INT_LEFT:
            if (istemp(p->output)) {
                if (istemp(p1->output)) {
                    if (p1->opcode == CPUI_INT_ADD) {
                        if (p2->opcode == CPUI_LOAD) {
                            if (isreg(p2->output))
                                _ldr_reg(reg2i(poa(p2)), reg2i(pi0a(p1)), reg2i(pi0a(p)), pi1(p)->get_val());
                            else  if (isreg(p3->output))
                                it = retrieve_orig_inst(b, it, 1);
                        }
                        else if (p2->opcode == CPUI_STORE) {
                            _str_reg(reg2i(pi2a(p2)), reg2i(pi0a(p1)), reg2i(pi0a(p)), pi1(p)->get_val());
                        }
                        else if (p2->opcode == CPUI_SUBPIECE)
                            it = retrieve_orig_inst(b, it, 1);
                    }
                    else if (p1->opcode == CPUI_COPY) {
                        if (p2->opcode == CPUI_INT_ADD) {
                            UPDATE_CPSR_SET(p2);
                            _add_reg(reg2i(poa(p2)), reg2i(pi0a(p2)), reg2i(pi0a(p)), SRType_LSL, pi1(p)->get_val());
                        }
                        else if (p2->opcode == CPUI_INT_SUB) {
                            _sub_reg(reg2i(poa(p2)), reg2i(pi0a(p2)), reg2i(pi0a(p)), SRType_LSL, pi1(p)->get_val());
                        }
                        /* sbc */
                        else if ((p2->opcode == CPUI_INT_LESSEQUAL) || (p2->opcode == CPUI_INT_CARRY)) {
                            it = retrieve_orig_inst(b, it, 1);
                        }
                    }
                    else if (p1->opcode == CPUI_INT_NEGATE)
                        it = retrieve_orig_inst(b, it, 1);
                }
                else if (isreg(p1->output)) {
                    UPDATE_CPSR_SET(p1);
                    switch (p1->opcode) {
                    case CPUI_INT_AND:
                        _and_reg(reg2i(poa(p1)), reg2i(pi0a(p1)), reg2i(pi0a(p)), SRType_LSL, pi1(p)->get_val(), follow_by_set_cpsr(p1));
                        break;

                    case CPUI_INT_OR:
                        _orr_reg(reg2i(poa(p1)), reg2i(pi0a(p1)), reg2i(pi0a(p)), SRType_LSL, pi1(p)->get_val());
                        break;

                    case CPUI_INT_XOR:
                        _xor_reg(reg2i(poa(p1)), reg2i(pi0a(p1)), reg2i(pi0a(p)), SRType_LSL, pi1(p)->get_val());
                        break;
                    }
                }
            }
            else if (isreg(p->output)) {
                if (pi1(p)->is_hard_constant()) {
                    _lsl_imm(reg2i(poa(p)), reg2i(pi0a(p)), pi1(p)->get_val(), follow_by_set_cpsr(p));
                }
            }
            it = advance_to_inst_end(it);
            break;

        case CPUI_INT_RIGHT:
            if (isreg(p->output)) {
                _lsr_imm(reg2i(poa(p)), reg2i(pi0a(p)), pi1(p)->get_val());
            }
            else if (istemp(p->output)) {
                if (istemp(p1->output)) {
                    if (p1->opcode == CPUI_COPY) {
                        if (p2->opcode == CPUI_INT_ADD) {
                            UPDATE_CPSR_SET(p2);
                            _add_reg(reg2i(poa(p2)), reg2i(pi0a(p2)), reg2i(pi0a(p)), SRType_LSR, pi1(p)->get_val());
                        }
                    }
                    else if (p1->opcode == CPUI_INT_NEGATE)
                        it = retrieve_orig_inst(b, it, 1);
                }
                else if (isreg(p1->output)) {
                    if (p1->opcode == CPUI_INT_AND)
                        _and_reg(reg2i(poa(p1)), reg2i(pi0a(p1)), reg2i(pi0a(p)), SRType_LSR, pi1(p)->get_val(), follow_by_set_cpsr(p1));
                    else if (p1->opcode == CPUI_INT_SUB)
                        _rsb_reg(reg2i(poa(p1)), reg2i(pi1a(p1)), reg2i(pi0a(p)), SRType_LSR, pi1(p)->get_val(), follow_by_set_cpsr(p1));
                    else if (p1->opcode == CPUI_INT_OR) {
                        UPDATE_CPSR_SET(p1);
                        if (pi1(p)->is_hard_constant())
                            _orr_reg(reg2i(poa(p1)), reg2i(pi0a(p1)), reg2i(pi0a(p)), SRType_LSR, pi1(p)->get_val());
                    }
                }

                it = advance_to_inst_end(it);
            }
            break;

        case CPUI_INT_SRIGHT:
            /* NOTE：
            2021年5月15日，我从这个点开始，尝试把部分指令完全不处理，直接从原文中拷贝，但是因为我还没有考虑具体
            哪些指令可以拷贝，哪些不可以，现在只能把一些我觉的影响比较小的，先处理了
            */
            it = retrieve_orig_inst(b, it, 1);
            break;

        case CPUI_INT_MULT:
            if (istemp(p->output)) {
                if (p1->opcode == CPUI_INT_ADD) {
                    _mla(reg2i(poa(p1)), reg2i(pi0a(p)), reg2i(pi1a(p)), reg2i(pi1a(p1)));
                }
                else if (p1->opcode == CPUI_INT_SUB) {
                    _mls(reg2i(poa(p1)), reg2i(pi0a(p)), reg2i(pi1a(p)), reg2i(pi0a(p1)));
                }

                advance(it, 1);
            }
            else if (isreg(p->output)) {
                _mul(reg2i(poa(p)), reg2i(pi0a(p)), reg2i(pi1a(p)));
            }
            break;

        case CPUI_BOOL_NEGATE:
            if (p3->opcode == CPUI_CBRANCH) {
                if ((p->opcode == CPUI_BOOL_NEGATE)) {
                    if (pi0a(p) == azr) {
                        if (p1->opcode == CPUI_BOOL_AND
                            && p2->opcode == CPUI_BOOL_NEGATE
                            && pi0a(p1) == acy) {
                            write_cbranch(b, COND_LS);
                        }
                        else if (p1->opcode == CPUI_INT_EQUAL
                            && p2->opcode == CPUI_BOOL_AND
                            && pi0a(p1) == ang && pi1a(p1) == aov) {
                            write_cbranch(b, COND_GT);
                        }
                    }
                    else if (pi0a(p) == acy) {
                        if (p1->opcode == CPUI_BOOL_OR
                            && p2->opcode == CPUI_BOOL_NEGATE
                            && pi1a(p1) == azr) {
                            write_cbranch(b, COND_HI);
                        }
                    }
                }
            }
            else if (p4->opcode == CPUI_CBRANCH) {
                if (p1->opcode == CPUI_INT_EQUAL
                    && p2->opcode == CPUI_BOOL_AND
                    && p3->opcode == CPUI_BOOL_NEGATE
                    && pi0a(p) == azr
                    && pi0a(p1) == ang
                    && pi1a(p1) == aov) {
                    write_cbranch(b, COND_LE);
                }
            }
            it = advance_to_inst_end(it);
            break;

        case CPUI_SUBPIECE:
            if (p1->opcode == CPUI_SUBPIECE) {
                if (p2->opcode == CPUI_INT_SEXT) {
                    it = retrieve_orig_inst(b, it, 1);
                }
            }
            else if (p1->opcode == CPUI_INT_ZEXT) {
                _uxtb(reg2i(poa(p1)), reg2i(pi0a(p)), 0);
            }
            break;

        case CPUI_USE:
            /* FIXME:这里其实等于在做硬编码了，后期考虑干掉 */
            if (!p1 || (p1->opcode != CPUI_INT_SUB))
                continue;

            rn = regalloc(p);
            rm = regalloc(p);

            _ldr_imm(rn, SP, pi0(p1)->get_sp_offset() - pi0(p)->get_val(), 0);
            _mov_imm(rm, pi1(p1)->get_val(), 0, 0);
            _cmp_reg(rn, rm);
            it = advance_to_inst_end(it);
            break;

        default:
            break;
        }

inst_label:
        if (oind == ind) {
            /* len == 0 说明没有生成代码，这个时候自动尝试匹配原文代码 */
            if (fd->use_old_inst(ps)) {
                size = fd->get_inst_size(p->get_addr());
                d->loader1->loadFill(fillbuf, size, p->get_addr());
                ob(fillbuf, size);
            }
            else
                vm_error("thumbgen find un-support pcode seq[%d]", p->start.getTime());
        }

        int len = ind - oind;

        /* 打印新生成的代码 */
#if 1

        while (len > 0) {
            size = dump_one_inst(oind, p);
            len -= size;
            oind += size;
        }
#endif
    }

#undef p1
#undef p2
#undef p3
#undef p4

    if (b->out.size() == 0) return 0;

    /* 相对跳转，不需要任何额外处理，直接返回 */
    if (b->is_rel_header()) return 0;

    int add_jmp = 0;
    flowblock *b1;

    if (b->out.size() == 1) {
        b1 = b->get_out(0);
        if (((b_ind + 1) == blist.size()) || (blist[b_ind + 1] != b1)) 
            add_jmp = 1;
    }
    else if (b->out.size() == 2) {
        assert(b->last_op()->opcode == CPUI_CBRANCH);
        b1 = b->get_false_edge()->point;
        if (((b_ind + 1) == blist.size()) || (blist[b_ind + 1] != b1)) 
            add_jmp = 1;
    }
    else 
        throw LowlevelError("now not support switch code gen");

    /* 假如出现了需要修复的vldr指令，那么我们把所有加载的数据全部移动到，这个block末尾，

    那么这个块末尾，必须得加上一个jmp，否则会把数据解析成指令


    b1:

    0002:   xxx 
    0004:   xxxx
    cbranch bX
    branch bN
    data: xxxx
    */

    if (fix_vldr_list.size() || fix_vld1_list.size())
        add_jmp = 1;

    if (add_jmp) {
        //x = COND_AL << 22;
        x = 0xf0009000;
        x |=  b1->cg.data ?  encbranch2(ind, b1->cg.data - data, 0):encbranch2(0, 0, 0);
        if (!b1->cg.data)
            add_fix_list(ind, b1, COND_AL);
        o(x);
        // dump_one_inst(ind - 4, NULL);
    }


    for (i = 0; i < fix_vldr_list.size(); i++)
        fix_vldr(fix_vldr_list[i]);

    for (i = 0; i < fix_vld1_list.size(); i++)
        fix_vld1(fix_vld1_list[i], pc_rel_tab);

    return 0;
}

/* A8.8.333 */
void thumb_gen::fix_vldr(fix_vldr_item &vldr)
{
    uint1 fillbuf[16];
    int siz = vldr.end->output->get_size(), oind = ind, offset;

    offset = oind - ((vldr.ind + 4) & ~3);
    if (offset >= 1024)
        vm_error("vldr only support <1024 offset");

    /* 
    4字节对齐，因为vldr加载的偏移必须4字节对齐，假如偏移不够，则补齐， 因为thumb指令，
    只有2字节对齐，和4字节对齐2种，假如没有4字节对齐，那么就是2字节，对齐，补2个字节即可 
    */
    if (!align4(offset)) {
        ind += 2;
        offset += 2;
    }

    d->loader1->loadFill(fillbuf, siz, Address(d->getDefaultCodeSpace(), vldr.end->get_in(1)->get_val()));

    memcpy(data + ind, fillbuf, siz);
    ind += siz;

    uint32_t inst = read_thumb2(data + vldr.ind);

    inst &= ~0xff;

    /* 全部转成正向的pc相对偏移 */
    inst |= (0x00800000 | (offset >> 2));

    write_thumb2(inst, data + vldr.ind);
}

void thumb_gen::fix_vld1(fix_vld1_item &item, pc_rel_table &tab)
{
    uint1 fillbuf[32];
    int oind = ind, offset;

    d->loader1->loadFill(fillbuf, item.loadsiz, Address(d->getDefaultCodeSpace(), item.loadaddr));

    memcpy(data + ind, fillbuf, item.loadsiz);
    ind += item.loadsiz;

    pcodeop *adr = tab[item.loadaddr];
    if (adr == NULL)
        vm_error("pc_rel_offset = %llx, not found correspond adr instruction", item.loadaddr);

    offset = oind - ((adr->ind + 4) & ~3);
    if (offset >= 4096)
        vm_error("vld write offset[%d] exceed 4095", offset);

    /* A8.8.12 T2 */
    uint32_t inst = read_thumb2(data + adr->ind);

    inst &= ~0x040070ff;
    inst |= stuff_constw(0, offset, 12);

    write_thumb2(inst, data + adr->ind);
}

int thumb_gen::get_load_addr_size(pcodeop *p, intb &loadaddr, int &loadsiz)
{
    pcodeop *p1;
    const Address &addr = p->get_addr();
    list<pcodeop *>::iterator it = p->basiciter;
    flowblock *b = p->parent;

    loadaddr = 0;
    loadsiz = 0;

    for (; it != b->ops.end(); it++) {
        p1 = *it;
        if (p1->get_addr() != addr)
            break;

        if (p1->opcode != CPUI_LOAD) continue;
        if (!p1->get_in(1)->is_pc_constant()) continue;

        if (!loadaddr) {
            loadaddr = pi1(p1)->get_val();
            loadsiz = p1->output->get_size();
        }
        else if ((loadaddr + loadsiz) == pi1(p1)->get_val()) {
            loadsiz += p1->output->get_size();
        }
        else
            vm_error("single instruction load address not continuous");
    }

    return 0;
}

int thumb_gen::follow_by_set_cpsr(pcodeop *p)
{
    flowblock *b = p->parent;
    list<pcodeop *>::iterator it = p->basiciter;

    if (it == b->ops.end()) return 0;

    pcodeop *p1 = *++it;

    if (!p1 || (p1->get_addr() != p->get_addr())) return 0;

    return (p1 && p1->output && (d->is_tsreg(poa(p1)) || d->is_sreg(poa(p1))));
}

void thumb_gen::write_cbranch(flowblock *b, uint32_t cond)
{
    uint32_t x;
    flowblock *t = b->get_true_edge()->point;
    x = (cond << 22);
    x |=  t->cg.data ?  encbranch(ind, t->cg.data - data, 0):encbranch(0, 0, 0);
    if (!t->cg.data)
        add_fix_list(ind, t, 0);
    o(x);
}

int thumb_gen::dump_one_inst(int oind, pcodeop *p)
{
    int i, siz;
    AssemblyRaw assem;
    assem.disable_html();
    char buf[128];
    assem.set_buf(buf);
    siz = d->trans->printAssembly(assem, Address(d->trans->getDefaultCodeSpace(), oind));

    if (p)
        printf("[p%4d] ", p->start.getTime());
    else
        printf("[     ] ");

    for (i = 0; i < siz; i++)
        printf("%02x ", data[oind + i]);

    for (; i < 4; i++)
        printf("   ");

    puts(buf);

    return siz;
}

void thumb_gen::add_fix_list(int ind, flowblock *b, int cond)
{
    flist.push_back(new fix_item(ind, b, cond));
}

void thumb_gen::dump()
{
    funcdata *fd1 = new funcdata(fd->name.c_str(), fd->get_addr(), 0, d);

    fd1->flags.dump_inst = 1;
    fd1->flags.thumb = fd->get_thumb();
    fd1->follow_flow();
    fd1->heritage();
    fd1->dump_cfg(fd1->name, "after_codegen", 1);
}

void thumb_gen::preprocess()
{
    int ret;
    pass_cond_reduce pass_cond_reduce(fd);
    pass_regalloc_const_arm pass_regalloc(fd);

    fd->flags.enable_to_const = 0;

    ret = pass_cond_reduce.run();
    if (PASS_DO_STHING(ret)) {
        fd->heritage_clear();
        fd->heritage();
    }
#if 0
    ret = pass_regalloc.run();
    if (PASS_DO_STHING(ret)) {
        fd->heritage_clear();
        fd->heritage();
    }
#endif
    fd->bblocks.compute_local_live_sets_p();
    fd->bblocks.compute_global_live_sets_p();

    collect_const();
}

int thumb_gen::regalloc(pcodeop *p)
{
    int i;

    for (i = R0; i <= R12; i++) {
        if (!p->live_in.test(i)) {
            p->live_in.set(i);
            return i;
        }
    }

    vm_error("regalloc failure on pcode[%d]", p->start.getTime());
    return -1;
}

/* 
@save 写到so buf内
*/
pit thumb_gen::retrieve_orig_inst(flowblock *b, pit pit, int save)
{
    uint8_t fillbuf[MAX_INST_SIZ];
    pcodeop *p, *prevp = *pit;

    for (++pit; pit != b->ops.end(); prevp = p, pit++) {
        p = *pit;
        if (p->get_addr() != prevp->get_addr()) break;
    }

    int size = fd->inst_size(prevp->get_addr());
    assert(size > 0);
    d->loader1->loadFill(fillbuf, size, prevp->get_addr());

    if (save)
        ob(fillbuf, size);

    return --pit;
}

pit thumb_gen::advance_to_inst_end(pit pit)
{
    flowblock *b = (*pit)->parent;
    const Address &addr = (*pit)->get_addr();

    if (addr == d->zero_addr) return pit;

    for (++pit; pit != b->ops.end(); pit++) {
        if ((*pit)->get_addr() != addr)
            return --pit;
    }

    return --pit;
}

int thumb_gen::save_to_end(uint32_t imm)
{
    if ((end - 4) >= ind)
        vm_error("filesize need expand");

    end =- 4;
    mbytes_write_int_little_endian_4b(data + end, imm);

    return end;
}

pit thumb_gen::g_push(flowblock *b, pit pit)
{
    int reglist = 0;

    pcodeop *p = *pit, *p1;

    if ((p->opcode == CPUI_COPY) && pi0a(p) == asp) 
        p = *++pit;

    while (p->output->get_addr() != d->sp_addr) {
        p = *pit++;
        p1 = *pit++;
        if ((p->opcode == CPUI_INT_SUB) && (p1->opcode == CPUI_STORE) && (pi1a(p1) == ama)) {
            reglist |= 1 << reg2i(pi2a(p1));
        }
        else if ((p->opcode == CPUI_INT_SUB) && (p1->opcode == CPUI_INT_ADD) && (pi0a(p1) == ama) && (poa(p1) == asp)) {
            p = p1;
            continue;
        }
        else throw LowlevelError("not support");

        p = *pit;
    }

    if ((poa(p) == asp) && (pi0a(p) == ama)) {
        if ((p->opcode == CPUI_COPY)
            /* FIXME: rn = rm + 0 == > rn = copy rm*/
            || ((p->opcode == CPUI_INT_ADD) && pi1(p)->is_constant())
            || ((p->opcode == CPUI_INT_SUB) && pi1(p)->is_constant())) {
            /* NOTE:
            所有的push {reglist} 指令，在转成pcode以后，末尾对sp的操作都是等于copy的，一般有以下几种形式:

            1. sp = COPY mult_addr
            2. mult_addr = INT_SUB mult_addr, 4:4 
               sp = INT_ADD mult_addr, 4:4

            第2种情况，可以合并成 sp = copy multi_addr，大家认真看，上面的 -4 和 +4 可以合并掉

            所以我们在把pcode转回push时，一般搜索到store的后一条指令为止，也就是 sp = copy multi_addr为止，
            但是可能出现这样一种情况

            1. sp = copy multi_addr
            2  sp = add sp, 4

            在某些情况下指令1， 2 合并成了 sp = add multi_addr, 4

            针对这种情况，当我们发现store的最后一条指令不是等价的sp = copy multi_addr时，要退回pit指针
            */
            if ((p->opcode != CPUI_COPY) && pi1(p)->get_val())
                pit--;
            _push(reglist);
        }
    }

    return pit;
}

pit thumb_gen::g_pop(flowblock *b, pit pit)
{
    int reglist = 0, reg, preg;
    pcodeop *p = *pit, *p1;

    p = *pit++;
    p1 = *pit++;
    preg = reg2i(poa(p1));
    reglist |= 1 << preg;

    while (1) {
        p = *pit++;
        p1 = *pit++;
        if ((p->opcode == CPUI_INT_ADD) && (p1->opcode == CPUI_LOAD)) {
            reg = reg2i(poa(p1));
            if (preg > reg)
                break;

            preg = reg;
            reglist |= 1 << reg;
        }
        else
            break;
    }

    _pop(reglist);

    if ((p1->opcode == CPUI_COPY)) {
        if ((reglist & (1 << PC))) {
            if ((*pit++)->opcode != CPUI_INT_AND
                || (*pit)->opcode != CPUI_RETURN)
                vm_error("pop need pc operation ");
        }
    }

    return pit;
}

pit thumb_gen::g_vpush(flowblock *b, pit pit)
{
    pcodeop *p = *pit, *p1;
    uint32_t reglist = 0, size = 0, dsize = 0, single = 0, x;

    assert(p->opcode == CPUI_INT_SUB);
    size = p->get_in(1)->get_val();
    p = *++pit;

    while (poa(p) == ama) {
        p = *pit++;
        p1 = *pit++;
        if (((p->opcode == CPUI_INT_ADD) || (p->opcode == CPUI_COPY)) && (p1->opcode == CPUI_STORE) && (pi1a(p1) == ama)) {
            single = p1->get_in(2)->get_size() == 4;
            dsize += p1->get_in(2)->get_size();
            x = d->vreg2i(pi2a(p1));
            if (single) reglist |= 1 << x;
            else 
                reglist |= (1 << x) + (1 << (x + 1));
        }
        else throw LowlevelError("not support");

        p = *pit;
    }

    _sub_sp_imm(size - dsize, 0);
    _vpush(single, reglist);

    return pit;
}

int const_item_cmp(const_item *l, const_item *r)
{
    return r->count < l->count;
}

void thumb_gen::collect_const()
{
    int i, pos;
    uint32_t imm;
    intb val;
    list<pcodeop *>::iterator it;
    map<uint32_t, const_item *>::iterator cit;
    const_item *item;

    for (i = 0; i < fd->bblocks.get_size(); i++) {
        flowblock *b = fd->bblocks.get_block(i);

        for (it = b->ops.begin(); it != b->ops.end(); it++) {
            pcodeop *p = *it;
            if (!p->output || !p->output->is_constant() || (p->opcode != CPUI_COPY)) continue;
            if (pi0(p)->in_ram()) {
                pos = (int)pi0(p)->get_ram_val();
                val = d->loader->read_val(pos, p->output->get_size());

                /* 假如PIC区域读出的值和原始的值已经不等，证明这块区域被擦除了，跳过  */
                if (val != p->output->get_val()) continue;

                /* 暂时先只处理uint32_t 类型的*/
                if (p->output->get_size() != 4) continue;

                imm = (uint32_t)val;
                cit = constmap.find(imm);
                if ((cit == constmap.end()))
                    constmap[imm] = item = new const_item(imm, pos);
                else
                    item = cit->second;

                item->count++;
            }
        }
    }
}

