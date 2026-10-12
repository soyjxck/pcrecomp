/*
 * d3d9_vs.c - Direct3D 9 vertex shaders: assembler, listing, GLSL.
 *
 * A Direct3D 9 game keeps its vertex shaders as assembly text and has d3dx9
 * turn them into the token stream CreateVertexShader takes. Off Windows there
 * is neither d3dx9 nor a driver that runs those tokens, so both halves are
 * here: d3d9_vs_assemble is D3DXAssembleShader for vertex shaders (the same
 * tokens, bit for bit), d3d9_vs_disassemble is D3DXDisassembleShader (a game
 * may feed that listing back to the assembler, so it is the same text, byte
 * for byte), and d3d9_vs_glsl rewrites a token stream as GLSL ES 3.00.
 *
 * Covered: vs_1_1 whole, and of vs_2_0 the straight-line part (abs, sgn, nrm,
 * crs, lrp, pow, sincos, mova, c[a0.y + n]). Flow control (if, rep, loop,
 * call, the b and i registers), vs_2_x and vs_3_0 are refused: the assembler
 * reports an error, the other two return NULL.
 *
 * The assembler checks the grammar and the rules that decide the encoding. It
 * does not repeat D3DX's validation pass (a read of a register nothing wrote,
 * two constants in one instruction, oPos never written): a shader D3DX would
 * turn down for those assembles here. Nor has it D3DX's preprocessor: a
 * #define or #include is an error.
 *
 * The token stream, as far as these shaders use it:
 *   version      0xFFFE0000 | major << 8 | minor
 *   instruction  opcode in bits 0-15 and, from vs_2_0 on, the count of the
 *                tokens that follow in bits 24-27
 *   destination  bit 31 | register type | write mask << 16 | number
 *   source       bit 31 | register type | negate << 24 | swizzle << 16 |
 *                relative << 13 | number
 *   end          0x0000FFFF
 * A register type is five bits: the low three sit at bit 28, the high two at
 * bit 11. vs_1_1 has one address register component, so c[a0.x + n] is the
 * relative bit alone; vs_2_0 follows such a source with a token naming a0 and
 * the component.
 */
#include <locale.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "d3d9_vs.h"

/* ---- the token format ---- */
#define VS_VERSION      0xFFFEu        /* high word of a vertex shader's version token */
#define TOKEN_END       0x0000FFFFu
#define OPCODE_END      0xFFFFu
#define OPCODE_COMMENT  0xFFFEu        /* a comment block: its length in DWORDs in bits 16-30 */
#define PARAM           0x80000000u    /* set in every destination and source token */
#define SRC_NEGATE      0x01000000u
#define SRC_RELATIVE    0x00002000u
#define SWZ_XYZW        0xE4u          /* a swizzle is two bits a component, x first; this one changes nothing */
#define FLT_MAX_TEXT    "3.402823466e+38"

enum { REG_TEMP = 0, REG_INPUT = 1, REG_CONST = 2, REG_ADDR = 3, REG_RASTOUT = 4, REG_ATTROUT = 5, REG_TEXCRDOUT = 6 };

enum {
    OP_NOP = 0, OP_MOV, OP_ADD, OP_SUB, OP_MAD, OP_MUL, OP_RCP, OP_RSQ, OP_DP3, OP_DP4, OP_MIN, OP_MAX,
    OP_SLT, OP_SGE, OP_EXP, OP_LOG, OP_LIT, OP_DST, OP_LRP, OP_FRC, OP_M4X4, OP_M4X3, OP_M3X4, OP_M3X3, OP_M3X2,
    OP_DCL = 31, OP_POW = 32, OP_CRS, OP_SGN, OP_ABS, OP_NRM, OP_SINCOS,
    OP_MOVA = 46, OP_EXPP = 78, OP_LOGP = 79, OP_DEF = 81
};

/* An instruction with one destination and nsrc sources (nop has neither). dcl and def have
 * shapes of their own and are not in the table. */
typedef struct {
    const char *name;
    uint8_t     code;
    uint8_t     nsrc;
    uint8_t     since;          /* the first shader model that has it */
    uint8_t     slots[2];       /* what the listing's tally counts it as, in vs_1_1 and in vs_2_0 */
    uint8_t     scalar;         /* bit n: source n is one component, picked by a replicate swizzle */
} vs_op;

static const vs_op g_ops[] = {
    { "nop",    OP_NOP,    0, 1, {  0, 0 }, 0 },
    { "mov",    OP_MOV,    1, 1, {  1, 1 }, 0 },
    { "add",    OP_ADD,    2, 1, {  1, 1 }, 0 },
    { "sub",    OP_SUB,    2, 1, {  1, 1 }, 0 },
    { "mad",    OP_MAD,    3, 1, {  1, 1 }, 0 },
    { "mul",    OP_MUL,    2, 1, {  1, 1 }, 0 },
    { "rcp",    OP_RCP,    1, 1, {  1, 1 }, 1 },
    { "rsq",    OP_RSQ,    1, 1, {  1, 1 }, 1 },
    { "dp3",    OP_DP3,    2, 1, {  1, 1 }, 0 },
    { "dp4",    OP_DP4,    2, 1, {  1, 1 }, 0 },
    { "min",    OP_MIN,    2, 1, {  1, 1 }, 0 },
    { "max",    OP_MAX,    2, 1, {  1, 1 }, 0 },
    { "slt",    OP_SLT,    2, 1, {  1, 1 }, 0 },
    { "sge",    OP_SGE,    2, 1, {  1, 1 }, 0 },
    { "exp",    OP_EXP,    1, 1, { 10, 1 }, 1 },
    { "log",    OP_LOG,    1, 1, { 10, 1 }, 1 },
    { "lit",    OP_LIT,    1, 1, {  1, 1 }, 0 },
    { "dst",    OP_DST,    2, 1, {  1, 1 }, 0 },
    { "lrp",    OP_LRP,    3, 2, {  1, 1 }, 0 },
    { "frc",    OP_FRC,    1, 1, {  3, 1 }, 0 },
    { "m4x4",   OP_M4X4,   2, 1, {  4, 4 }, 0 },
    { "m4x3",   OP_M4X3,   2, 1, {  3, 3 }, 0 },
    { "m3x4",   OP_M3X4,   2, 1, {  4, 4 }, 0 },
    { "m3x3",   OP_M3X3,   2, 1, {  3, 3 }, 0 },
    { "m3x2",   OP_M3X2,   2, 1, {  2, 2 }, 0 },
    { "pow",    OP_POW,    2, 2, {  3, 3 }, 3 },
    { "crs",    OP_CRS,    2, 2, {  2, 2 }, 0 },
    { "sgn",    OP_SGN,    3, 2, {  3, 3 }, 0 },
    { "abs",    OP_ABS,    1, 2, {  1, 1 }, 0 },
    { "nrm",    OP_NRM,    1, 2, {  3, 3 }, 0 },
    { "sincos", OP_SINCOS, 3, 2, {  8, 8 }, 1 },
    { "mova",   OP_MOVA,   1, 2, {  1, 1 }, 0 },
    { "expp",   OP_EXPP,   1, 1, {  1, 1 }, 1 },
    { "logp",   OP_LOGP,   1, 1, {  1, 1 }, 1 },
};
#define NOPS (sizeof g_ops / sizeof g_ops[0])

static const vs_op *op_by_code(uint32_t code) {
    for (size_t i = 0; i < NOPS; i++) if (g_ops[i].code == code) return &g_ops[i];
    return NULL;
}
static const vs_op *op_by_name(const char *name) {
    for (size_t i = 0; i < NOPS; i++) if (!strcmp(g_ops[i].name, name)) return &g_ops[i];
    return NULL;
}

/* The matrix instructions are that many dot products against consecutive registers:
 * rows of them, each over cols components. */
static int matrix_shape(uint32_t code, int *cols) {
    switch (code) {
    case OP_M4X4: *cols = 4; return 4;
    case OP_M4X3: *cols = 4; return 3;
    case OP_M3X4: *cols = 3; return 4;
    case OP_M3X3: *cols = 3; return 3;
    case OP_M3X2: *cols = 3; return 2;
    }
    *cols = 0;
    return 0;
}

/* The D3DDECLUSAGE names, in the order of their values. */
static const char *const g_usage[] = {
    "position", "blendweight", "blendindices", "normal", "psize", "texcoord", "tangent",
    "binormal", "tessfactor", "positiont", "color", "fog", "depth", "sample",
};
#define NUSAGES (sizeof g_usage / sizeof g_usage[0])

/* One operand, as the assembler parses it and the other two decode it. */
typedef struct {
    uint32_t type, num;
    uint32_t mask;              /* destination: the components written, bit 0 = x */
    uint32_t swz;               /* source: the swizzle byte */
    int      neg;
    int      rel;               /* a constant read as c[a0.<relcomp> + num] */
    int      relcomp;
} vs_arg;

typedef struct {
    uint32_t     code;
    const vs_op *op;            /* NULL for dcl and def */
    vs_arg       dst, src[3];
    uint32_t     usage, usage_index;     /* dcl */
    uint32_t     value[4];               /* def: the four floats, as their bits */
} vs_ins;

static uint32_t reg_bits(uint32_t type, uint32_t num) {
    return PARAM | (type & 7) << 28 | (type & 0x18) << 8 | num;
}

/* 1 or 2 for the versions handled (vs_1_0 is vs_1_1 without the address register), else 0. */
static int version_of(const uint32_t *tokens, int *minor) {
    if (!tokens || tokens[0] >> 16 != VS_VERSION) return 0;
    uint32_t v = tokens[0] & 0xFFFF;
    if (v != 0x0100 && v != 0x0101 && v != 0x0200) return 0;
    if (minor) *minor = (int)(v & 0xFF);
    return (int)(v >> 8);
}

/* Letters and digits by hand: the shader language is ASCII whatever the host's locale is. */
static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int hex_digit(char c) { return is_digit(c) ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

/* ---- text that grows ---- */
typedef struct { char *p; size_t len, cap; int failed; } sbuf;

static void sb_put(sbuf *b, const char *s, size_t n) {
    if (b->failed) return;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 256;
        while (cap < b->len + n + 1) cap *= 2;
        char *q = realloc(b->p, cap);
        if (!q) { b->failed = 1; return; }
        b->p = q, b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}
static void sb_printf(sbuf *b, const char *fmt, ...) {
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof tmp) { b->failed = 1; return; }
    sb_put(b, tmp, (size_t)n);
}
/* The finished text, the caller's to free; NULL if memory ran out on the way. */
static char *sb_take(sbuf *b) {
    sb_put(b, "", 0);                                       /* nothing written is still a string */
    if (b->failed) { free(b->p); b->p = NULL; }
    return b->p;
}

/* ---- walking a token stream ---- */

/* Past the instruction or comment at p, whatever it is. From vs_2_0 on an instruction says how
 * long it is. A vs_1_x one does not, but its parameters all have bit 31 set and the next
 * instruction token never has; only def breaks that rule, its four floats being anything. */
static const uint32_t *skip(const uint32_t *p, int major) {
    uint32_t tok = *p++, code = tok & 0xFFFF;
    if (code == OPCODE_COMMENT) return p + (tok >> 16 & 0x7FFF);
    if (major >= 2) return p + (tok >> 24 & 0xF);
    if (code == OP_DEF) return p + 5;
    while (*p & PARAM) p++;
    return p;
}

size_t d3d9_vs_tokens(const uint32_t *tokens) {
    if (!tokens || tokens[0] >> 16 != VS_VERSION) return 0;
    int major = (int)(tokens[0] >> 8 & 0xFF);
    const uint32_t *p = tokens + 1;
    while ((*p & 0xFFFF) != OPCODE_END) p = skip(p, major);
    return (size_t)(p - tokens) + 1;
}

static int decode_reg(uint32_t t, vs_arg *a) {
    a->type = (t >> 28 & 7) | (t >> 8 & 0x18);
    a->num = t & 0x7FF;
    return (t & PARAM) && !(t & 0xC000) && a->type <= REG_TEXCRDOUT;
}
static int decode_dst(uint32_t t, vs_arg *a) {
    a->mask = t >> 16 & 0xF;
    return decode_reg(t, a) && !(t & 0x0FF02000u);          /* no saturate, no shift, not relative */
}

/* The instruction at p into *ins; returns where the next one starts, or NULL for an
 * instruction, a register or a modifier this module does not handle. */
static const uint32_t *decode(const uint32_t *p, int major, vs_ins *ins) {
    const uint32_t *start = p;
    uint32_t tok = *p++;
    memset(ins, 0, sizeof *ins);
    if (tok & 0xF0FF0000u) return NULL;                     /* predicated, co-issued, or a comparison */
    ins->code = tok & 0xFFFF;
    ins->op = op_by_code(ins->code);
    if (ins->code == OP_DCL) {
        if (!(*p & PARAM) || (*p & 0x7FF0FFE0u)) return NULL;
        ins->usage = *p & 0x1F, ins->usage_index = *p >> 16 & 0xF;
        p++;
        if (!decode_dst(*p++, &ins->dst)) return NULL;
    } else if (ins->code == OP_DEF) {
        if (!decode_dst(*p++, &ins->dst)) return NULL;
        memcpy(ins->value, p, sizeof ins->value);
        p += 4;
    } else if (!ins->op) {
        return NULL;
    } else if (ins->code != OP_NOP) {
        if (!decode_dst(*p++, &ins->dst)) return NULL;
        for (int i = 0; i < ins->op->nsrc; i++) {
            vs_arg *a = &ins->src[i];
            uint32_t t = *p++;
            if (!decode_reg(t, a) || (t & 0x0E000000u)) return NULL;       /* negate is the one modifier */
            a->swz = t >> 16 & 0xFF;
            a->neg = (t & SRC_NEGATE) != 0;
            a->rel = (t & SRC_RELATIVE) != 0;
            if (a->rel && a->type != REG_CONST) return NULL;
            if (a->rel && major >= 2) {                     /* which component of a0: a source token of its own */
                uint32_t r = *p++, c = r >> 16 & 3;
                if ((r & 0xFF00FFFFu) != reg_bits(REG_ADDR, 0) || (r >> 16 & 0xFF) != c * 0x55) return NULL;
                a->relcomp = (int)c;
            }
        }
    }
    if (major >= 2 && (tok >> 24 & 0xF) != (uint32_t)(p - start - 1)) return NULL;
    return p;
}

/* r3, v0, c17, a0, oPos, oD1, oT2: a register as the assembly language spells it. */
static int reg_name(char *out, size_t cap, uint32_t type, uint32_t num) {
    static const char *const rast[] = { "oPos", "oFog", "oPts" };
    static const char *const prefix[] = { "r", "v", "c", "a", NULL, "oD", "oT" };
    if (type == REG_RASTOUT) {
        if (num > 2) return 0;
        snprintf(out, cap, "%s", rast[num]);
    } else if (type <= REG_TEXCRDOUT) {
        snprintf(out, cap, "%s%u", prefix[type], (unsigned)num);
    } else return 0;
    return 1;
}

static void mask_text(char *out, uint32_t mask) {           /* out: five bytes */
    for (int i = 0; i < 4; i++) if (mask >> i & 1) *out++ = "xyzw"[i];
    *out = 0;
}

/* ---- the listing ---- */

/*
 * A float the way the listing prints it: "%.9g" as the C runtime d3dx9 came with did it, which
 * is not what every runtime prints. That one wrote three exponent digits (1e+010), rounded an
 * exact tie away from zero (1234567.125 is 1234567.13), and had names of its own for the
 * infinities and the NaNs. So the digits are worked out here, exactly: a float is an integer
 * times a power of two, and a negative power of two is a power of five over a power of ten.
 */
static void float_text(char *out, size_t cap, uint32_t bits) {
    uint32_t frac = bits & 0x7FFFFF, biased = bits >> 23 & 0xFF;
    const char *sign = bits >> 31 ? "-" : "";
    if (biased == 0xFF) {
        snprintf(out, cap, "%s%s", sign, !frac ? "1.#INF" : bits == 0xFFC00000u ? "1.#IND" : "1.#QNAN");
        return;
    }
    if (!biased && !frac) { snprintf(out, cap, "%s0", sign); return; }

    uint32_t limb[14] = { biased ? frac | 0x800000 : frac };       /* base 10^9, least significant first */
    int nlimb = 1, exp2 = (int)(biased ? biased : 1) - 150;
    for (int k = exp2 < 0 ? -exp2 : exp2; k > 0; k--) {
        uint64_t carry = 0;
        for (int i = 0; i < nlimb; i++) {
            uint64_t v = (uint64_t)limb[i] * (exp2 < 0 ? 5u : 2u) + carry;
            limb[i] = (uint32_t)(v % 1000000000u);
            carry = v / 1000000000u;
        }
        if (carry) limb[nlimb++] = (uint32_t)carry;
    }
    char dig[14 * 9 + 1];
    int nd = snprintf(dig, sizeof dig, "%u", (unsigned)limb[nlimb - 1]);
    for (int i = nlimb - 2; i >= 0; i--) nd += snprintf(dig + nd, sizeof dig - (size_t)nd, "%09u", (unsigned)limb[i]);
    int x = nd - 1 + (exp2 < 0 ? exp2 : 0);                 /* the decimal exponent of the first digit */

    if (nd > 9) {                                           /* nine significant digits */
        int up = dig[9] >= '5';
        nd = 9;
        for (int i = 8; up && i >= 0; i--) {
            if (dig[i] == '9') dig[i] = '0'; else { dig[i]++; up = 0; }
        }
        if (up) { dig[0] = '1'; x++; }                      /* all nines: it is the next power of ten */
    }
    while (nd > 1 && dig[nd - 1] == '0') nd--;
    dig[nd] = 0;

    /* (the precisions say nothing new, nine digits are all there are: they let a compiler see it) */
    if (x < -4 || x >= 9) {                                 /* d.ddde+xxx */
        snprintf(out, cap, "%s%c%s%.8se%c%03d", sign, dig[0], nd > 1 ? "." : "", dig + 1, x < 0 ? '-' : '+', (x < 0 ? -x : x) % 1000);
    } else if (x < 0) {                                     /* 0.000ddd */
        snprintf(out, cap, "%s0.%.*s%.9s", sign, -x - 1, "000", dig);
    } else {                                                /* ddd000 or ddd.ddd */
        char whole[10];
        for (int i = 0; i <= x; i++) whole[i] = i < nd ? dig[i] : '0';
        whole[x + 1] = 0;
        snprintf(out, cap, "%s%.9s%s%.8s", sign, whole, nd > x + 1 ? "." : "", nd > x + 1 ? dig + x + 1 : "");
    }
}

static int list_dst(sbuf *b, const vs_arg *a) {
    char name[16], mask[5];
    if (!reg_name(name, sizeof name, a->type, a->num)) return 0;
    mask_text(mask, a->mask);
    sb_printf(b, "%s%s%s", name, a->mask == 0xF ? "" : ".", a->mask == 0xF ? "" : mask);
    return 1;
}

/* A source: -c24[a0.x].xyzz. A swizzle that changes nothing is left out, one that repeats a
 * component is that one letter. */
static int list_src(sbuf *b, const vs_arg *a) {
    char name[16];
    if (!reg_name(name, sizeof name, a->type, a->num)) return 0;
    sb_printf(b, "%s%s", a->neg ? "-" : "", name);
    if (a->rel) sb_printf(b, "[a0.%c]", "xyzw"[a->relcomp]);
    if (a->swz != SWZ_XYZW) {
        int letters = a->swz == (a->swz & 3) * 0x55 ? 1 : 4;
        sb_put(b, ".", 1);
        for (int i = 0; i < letters; i++) sb_put(b, &"xyzw"[a->swz >> 2 * i & 3], 1);
    }
    return 1;
}

char *d3d9_vs_disassemble(const uint32_t *tokens) {
    int minor = 0, major = version_of(tokens, &minor);
    if (!major) return NULL;
    sbuf b = { 0 };
    int slots = 0, good = 1;
    sb_printf(&b, "    vs_%d_%d\n", major, minor);
    for (const uint32_t *p = tokens + 1; good && (*p & 0xFFFF) != OPCODE_END; ) {
        vs_ins ins;
        if ((*p & 0xFFFF) == OPCODE_COMMENT) { p = skip(p, major); continue; }
        p = decode(p, major, &ins);
        if (!p) { good = 0; break; }
        if (ins.code == OP_DCL) {
            if (ins.usage >= NUSAGES) { good = 0; break; }
            sb_printf(&b, "    dcl_%s", g_usage[ins.usage]);
            if (ins.usage_index) sb_printf(&b, "%u", (unsigned)ins.usage_index);
            sb_put(&b, " ", 1);
            good = list_dst(&b, &ins.dst);
        } else if (ins.code == OP_DEF) {
            sb_put(&b, "    def ", 8);
            good = list_dst(&b, &ins.dst);
            for (int i = 0; i < 4; i++) {
                char f[32];
                float_text(f, sizeof f, ins.value[i]);
                sb_printf(&b, ", %s", f);
            }
        } else {
            sb_printf(&b, "    %s", ins.op->name);
            if (ins.code != OP_NOP) {
                sb_put(&b, " ", 1);
                good = list_dst(&b, &ins.dst);
                for (int i = 0; good && i < ins.op->nsrc; i++) {
                    sb_put(&b, ", ", 2);
                    good = list_src(&b, &ins.src[i]);
                }
            }
            slots += ins.op->slots[major - 1];
        }
        sb_put(&b, "\n", 1);
    }
    if (!good) { free(b.p); return NULL; }
    sb_printf(&b, "\n// approximately %d instruction slot%s used\n", slots, slots == 1 ? "" : "s");
    return sb_take(&b);
}

/* ---- GLSL ---- */

/* A source operand as GLSL: n components of the swizzled value from component `first` on
 * (4 from 0 is the whole vector, 3 from 0 what dp3 reads, 1 a single one). row is added to the
 * register number, for the matrix instructions. A relative constant is clamped into the array
 * the shader has: Direct3D leaves a read outside it undefined, GLSL ES does too, and a clamp
 * is the one answer that is the same on every driver. */
static void glsl_src(char *out, size_t cap, const vs_arg *a, int first, int n, int row) {
    char reg[48], sel[6] = "";
    unsigned num = (unsigned)a->num + (unsigned)row;
    if (a->type == REG_TEMP) snprintf(reg, sizeof reg, "r%u", num);
    else if (a->type == REG_INPUT) snprintf(reg, sizeof reg, "v%u", num);
    else if (a->rel) snprintf(reg, sizeof reg, "c[clamp(a0.%c + %u, 0, 255)]", "xyzw"[a->relcomp], num);
    else snprintf(reg, sizeof reg, "c[%u]", num);
    if (n < 4 || a->swz != SWZ_XYZW) {
        sel[0] = '.';
        for (int i = 0; i < n; i++) sel[1 + i] = "xyzw"[a->swz >> 2 * (first + i) & 3];
        sel[1 + n] = 0;
    }
    snprintf(out, cap, a->neg ? "(-%s%s)" : "%s%s", reg, sel);
}

/* Whether e is a single call, f(...): that takes a .xy straight after it, anything else wants
 * brackets first. */
static int is_call(const char *e) {
    size_t i = 0, n = strlen(e);
    int depth = 0;
    while (is_alpha(e[i]) || is_digit(e[i])) i++;
    if (!i || e[i] != '(') return 0;
    for (; i < n; i++) {
        if (e[i] == '(') depth++;
        else if (e[i] == ')' && --depth == 0) return i == n - 1;
    }
    return 0;
}

/* One instruction as one statement appended to b. Notes in *info, *temps (a bit per r register)
 * and *addr (a0) what the shader declares, writes and reads. 0 if it cannot be translated. */
static int glsl_instruction(sbuf *b, const vs_ins *ins, int major, d3d9_vs_info *info, uint32_t *temps, int *addr) {
    const vs_arg *d = &ins->dst;
    if (ins->code == OP_DCL) {
        if (d->type != REG_INPUT || d->num > 15) return 0;
        info->in[d->num].declared = 1;
        info->in[d->num].usage = (uint8_t)ins->usage;
        info->in[d->num].usage_index = (uint8_t)ins->usage_index;
        return 1;
    }
    if (ins->code == OP_DEF) {
        if (d->type != REG_CONST || d->num > 255 || info->ndefs == 32) return 0;
        info->def[info->ndefs].reg = (int)d->num;
        memcpy(info->def[info->ndefs].v, ins->value, sizeof ins->value);
        info->ndefs++;
        return 1;
    }
    if (ins->code == OP_NOP) return 1;

    switch (d->type) {
    case REG_TEMP:
        if (d->num > 31) return 0;
        *temps |= 1u << d->num;
        break;
    case REG_ADDR:
        if (d->num || (ins->code != OP_MOV && ins->code != OP_MOVA)) return 0;
        *addr = 1;
        break;
    case REG_RASTOUT:
        if (d->num > 2) return 0;
        info->outputs |= d->num == 0 ? D3D9_VS_OUT_POS : d->num == 1 ? D3D9_VS_OUT_FOG : D3D9_VS_OUT_PTS;
        break;
    case REG_ATTROUT:
        if (d->num > 1) return 0;
        info->outputs |= d->num ? D3D9_VS_OUT_D1 : D3D9_VS_OUT_D0;
        break;
    case REG_TEXCRDOUT:
        if (d->num > 7) return 0;
        info->outputs |= D3D9_VS_OUT_T(d->num);
        break;
    default:
        return 0;
    }

    int cols = 0, rows = matrix_shape(ins->code, &cols);
    for (int i = 0; i < ins->op->nsrc; i++) {
        const vs_arg *s = &ins->src[i];
        uint32_t last = s->num + (uint32_t)(i == 1 && rows ? rows - 1 : 0);
        if (s->type == REG_TEMP) {
            if (last > 31) return 0;
            for (uint32_t n = s->num; n <= last; n++) *temps |= 1u << n;
        } else if (s->type == REG_INPUT) {
            if (last > 15) return 0;
        } else if (s->type != REG_CONST) {
            return 0;
        } else if (s->rel) {
            info->consts = 256;                             /* any of them, as far as anyone can tell here */
            *addr = 1;
        } else {
            if (last > 255) return 0;
            if ((int)last + 1 > info->consts) info->consts = (int)last + 1;
        }
    }

    /* The right side: a vec4 whatever the instruction, so that one rule writes it. */
    char s0[64], s1[64], s2[64], x[64], y[64], z[64], w[64];
    sbuf e = { 0 };
#define SRC(buf, i, first, n) glsl_src(buf, sizeof buf, &ins->src[i], first, n, 0)
    switch (ins->code) {
    case OP_MOV: case OP_MOVA:
        SRC(s0, 0, 0, 4);
        if (d->type != REG_ADDR) sb_printf(&e, "%s", s0);
        else if (ins->code == OP_MOV) sb_printf(&e, "ivec4(floor(%s))", s0);       /* vs_1_1 rounds down */
        else sb_printf(&e, "ivec4(floor(abs(%s) + 0.5) * sign(%s))", s0, s0);      /* mova to the nearest */
        break;
    case OP_ADD: case OP_SUB: case OP_MUL:
        SRC(s0, 0, 0, 4); SRC(s1, 1, 0, 4);
        sb_printf(&e, "%s %c %s", s0, ins->code == OP_ADD ? '+' : ins->code == OP_SUB ? '-' : '*', s1);
        break;
    case OP_MAD:
        SRC(s0, 0, 0, 4); SRC(s1, 1, 0, 4); SRC(s2, 2, 0, 4);
        sb_printf(&e, "%s * %s + %s", s0, s1, s2);
        break;
    case OP_RCP:                                            /* Direct3D gives the largest float for 1/0 */
        SRC(s0, 0, 0, 1);
        sb_printf(&e, "vec4(%s == 0.0 ? " FLT_MAX_TEXT " : 1.0 / %s)", s0, s0);
        break;
    case OP_RSQ:
        SRC(s0, 0, 0, 1);
        sb_printf(&e, "vec4(%s == 0.0 ? " FLT_MAX_TEXT " : inversesqrt(abs(%s)))", s0, s0);
        break;
    case OP_DP3:
        SRC(s0, 0, 0, 3); SRC(s1, 1, 0, 3);
        sb_printf(&e, "vec4(dot(%s, %s))", s0, s1);
        break;
    case OP_DP4:
        SRC(s0, 0, 0, 4); SRC(s1, 1, 0, 4);
        sb_printf(&e, "vec4(dot(%s, %s))", s0, s1);
        break;
    case OP_MIN: case OP_MAX:
        SRC(s0, 0, 0, 4); SRC(s1, 1, 0, 4);
        sb_printf(&e, "%s(%s, %s)", ins->code == OP_MIN ? "min" : "max", s0, s1);
        break;
    case OP_SLT: case OP_SGE:
        SRC(s0, 0, 0, 4); SRC(s1, 1, 0, 4);
        sb_printf(&e, "vec4(%s(%s, %s))", ins->code == OP_SLT ? "lessThan" : "greaterThanEqual", s0, s1);
        break;
    case OP_EXP: case OP_EXPP:
        SRC(s0, 0, 0, 1);
        if (ins->code == OP_EXPP && major == 1)             /* the vs_1_1 one hands out the pieces as well */
            sb_printf(&e, "vec4(exp2(floor(%s)), %s - floor(%s), exp2(%s), 1.0)", s0, s0, s0, s0);
        else sb_printf(&e, "vec4(exp2(%s))", s0);
        break;
    case OP_LOG: case OP_LOGP:
        SRC(s0, 0, 0, 1);
        sb_printf(&e, "vec4(%s == 0.0 ? -" FLT_MAX_TEXT " : log2(abs(%s)))", s0, s0);
        break;
    case OP_LIT:                                            /* (1, diffuse, specular, 1) from (N.L, N.H, -, power) */
        SRC(x, 0, 0, 1); SRC(y, 0, 1, 1); SRC(w, 0, 3, 1);
        sb_printf(&e, "vec4(1.0, max(%s, 0.0), ((%s > 0.0 && %s > 0.0) ? pow(%s, clamp(%s, -127.9961, 127.9961)) : 0.0), 1.0)",
                  x, x, y, y, w);
        break;
    case OP_DST:                                            /* (1, d, d*d, 1/d) from (-, d*d, d*d, -) and (-, 1/d, -, 1/d) */
        SRC(y, 0, 1, 1); SRC(z, 0, 2, 1); SRC(s1, 1, 1, 1); SRC(w, 1, 3, 1);
        sb_printf(&e, "vec4(1.0, %s * %s, %s, %s)", y, s1, z, w);
        break;
    case OP_LRP:
        SRC(s0, 0, 0, 4); SRC(s1, 1, 0, 4); SRC(s2, 2, 0, 4);
        sb_printf(&e, "mix(%s, %s, %s)", s2, s1, s0);
        break;
    case OP_FRC:
        SRC(s0, 0, 0, 4);
        sb_printf(&e, "fract(%s)", s0);
        break;
    case OP_M4X4: case OP_M4X3: case OP_M3X4: case OP_M3X3: case OP_M3X2:
        SRC(s0, 0, 0, cols);
        sb_put(&e, "vec4(", 5);
        for (int r = 0; r < 4; r++) {
            if (r < rows) {
                glsl_src(s1, sizeof s1, &ins->src[1], 0, cols, r);
                sb_printf(&e, "%sdot(%s, %s)", r ? ", " : "", s0, s1);
            } else sb_put(&e, ", 0.0", 5);
        }
        sb_put(&e, ")", 1);
        break;
    case OP_POW:
        SRC(s0, 0, 0, 1); SRC(s1, 1, 0, 1);
        sb_printf(&e, "vec4(pow(abs(%s), %s))", s0, s1);
        break;
    case OP_CRS:
        SRC(s0, 0, 0, 3); SRC(s1, 1, 0, 3);
        sb_printf(&e, "vec4(cross(%s, %s), 0.0)", s0, s1);
        break;
    case OP_SGN:                                            /* its other two operands are scratch registers */
        SRC(s0, 0, 0, 4);
        sb_printf(&e, "sign(%s)", s0);
        break;
    case OP_ABS:
        SRC(s0, 0, 0, 4);
        sb_printf(&e, "abs(%s)", s0);
        break;
    case OP_NRM:
        SRC(s0, 0, 0, 4); SRC(s1, 0, 0, 3);
        sb_printf(&e, "%s * (dot(%s, %s) == 0.0 ? " FLT_MAX_TEXT " : inversesqrt(dot(%s, %s)))", s0, s1, s1, s1, s1);
        break;
    case OP_SINCOS:
        SRC(s0, 0, 0, 1);
        sb_printf(&e, "vec4(cos(%s), sin(%s), 0.0, 0.0)", s0, s0);
        break;
    default:
        return 0;
    }
#undef SRC

    char name[16], mask[5];
    reg_name(name, sizeof name, d->type, d->num);
    mask_text(mask, d->mask);
    if (e.failed) b->failed = 1;
    else if (d->mask == 0xF) sb_printf(b, "    %s = %s;\n", name, e.p);
    else if (d->mask && is_call(e.p)) sb_printf(b, "    %s.%s = %s.%s;\n", name, mask, e.p, mask);
    else if (d->mask) sb_printf(b, "    %s.%s = (%s).%s;\n", name, mask, e.p, mask);
    free(e.p);
    return 1;
}

char *d3d9_vs_glsl(const uint32_t *tokens, d3d9_vs_info *info) {
    d3d9_vs_info unused;
    if (!info) info = &unused;
    memset(info, 0, sizeof *info);
    int major = version_of(tokens, NULL);
    if (!major) return NULL;

    sbuf body = { 0 }, out = { 0 };
    uint32_t temps = 0;
    int addr = 0;
    for (const uint32_t *p = tokens + 1; (*p & 0xFFFF) != OPCODE_END; ) {
        vs_ins ins;
        if ((*p & 0xFFFF) == OPCODE_COMMENT) { p = skip(p, major); continue; }
        p = decode(p, major, &ins);
        if (!p || !glsl_instruction(&body, &ins, major, info, &temps, &addr)) { free(body.p); return NULL; }
    }
    /* A register read before anything wrote it is undefined in Direct3D; zero is as good as any. */
    for (int i = 0; i < 32; i++) if (temps >> i & 1) sb_printf(&out, "    vec4 r%d = vec4(0.0);\n", i);
    if (addr) sb_printf(&out, "    ivec4 a0 = ivec4(0);\n");
    if (body.failed) out.failed = 1;
    else if (body.p) sb_put(&out, body.p, body.len);
    free(body.p);
    return sb_take(&out);
}

/* ---- the assembler ---- */

enum { T_END, T_IDENT, T_INT, T_FLOAT, T_PUNCT };

typedef struct {
    const char *s;              /* what is left of the source, NUL-terminated */
    int         line;           /* the line s is on, from 1 */
    int         kind;           /* the token in hand... */
    int         tline;          /* ...the line it starts on... */
    char        text[48];       /* ...and as it was written */
    uint64_t    ival;
    double      fval;
    int         major;
    uint32_t   *out;
    size_t      n, cap;
    int         failed;         /* nothing is parsed after the first error... */
    char       *err;            /* ...which is this (NULL if there was no memory for it) */
} assembler;

static void fail(assembler *a, int line, const char *fmt, ...) {
    char msg[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (!a->failed && (a->err = malloc(strlen(msg) + 32)) != NULL) snprintf(a->err, strlen(msg) + 32, "line %d: %s", line, msg);
    a->failed = 1;
    a->kind = T_END;
}

/* The next token into a->kind and friends. The language has no line structure: an instruction
 * ends where the next begins, and ';' and '//' start comments that run to the end of the line. */
static void next(assembler *a) {
    const char *s = a->s;
    if (a->failed) { a->kind = T_END; return; }
    for (;;) {
        if (*s == '\n') { a->line++; s++; }
        else if (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\f' || *s == '\v') s++;
        else if (*s == ';' || (s[0] == '/' && s[1] == '/')) { while (*s && *s != '\n') s++; }
        else if (s[0] == '/' && s[1] == '*') {
            int line = a->line;
            for (s += 2; *s && !(s[0] == '*' && s[1] == '/'); s++) if (*s == '\n') a->line++;
            if (!*s) { a->s = s; fail(a, line, "comment continues past end of file"); return; }
            s += 2;
        }
        else break;
    }
    const char *start = s;
    a->tline = a->line;
    if (!*s) {
        a->kind = T_END;
    } else if (is_alpha(*s)) {
        while (is_alpha(*s) || is_digit(*s)) s++;
        a->kind = T_IDENT;
    } else if (s[0] == '0' && s[1] == 'x' && hex_digit(s[2]) >= 0) {
        a->kind = T_INT;
        for (s += 2, a->ival = 0; hex_digit(*s) >= 0 && a->ival <= 0xFFFFFFFFu; s++)
            a->ival = a->ival * 16 + (uint64_t)hex_digit(*s);
    } else if (is_digit(*s) || (*s == '.' && is_digit(s[1]))) {
        int isfloat = 0;
        while (is_digit(*s)) s++;
        if (*s == '.') { isfloat = 1; s++; while (is_digit(*s)) s++; }
        if ((*s == 'e' || *s == 'E') && (is_digit(s[1]) || ((s[1] == '+' || s[1] == '-') && is_digit(s[2])))) {
            isfloat = 1;
            s += 2;
            while (is_digit(*s)) s++;
        }
        if (isfloat) {
            /* strtod reads the decimal point the host's locale writes, so give it that one */
            char num[64];
            size_t len = (size_t)(s - start);
            if (len >= sizeof num) { a->s = s; fail(a, a->tline, "number too long"); return; }
            memcpy(num, start, len);
            num[len] = 0;
            const struct lconv *lc = localeconv();
            char *dot = strchr(num, '.');
            if (dot && lc && lc->decimal_point && lc->decimal_point[0]) *dot = lc->decimal_point[0];
            a->kind = T_FLOAT;
            a->fval = strtod(num, NULL);
            if (*s == 'f' || *s == 'F') s++;
        } else {                                            /* as in C, a leading 0 makes it octal */
            unsigned base = start[0] == '0' ? 8 : 10;
            a->kind = T_INT;
            a->ival = 0;
            for (const char *q = start; q < s && a->ival <= 0xFFFFFFFFu; q++) {
                if ((unsigned)(*q - '0') >= base) { a->s = s; fail(a, a->tline, "invalid octal digit '%c'", *q); return; }
                a->ival = a->ival * base + (unsigned)(*q - '0');
            }
        }
    } else if (strchr(",.[]+-", *s)) {
        s++;
        a->kind = T_PUNCT;
    } else {
        a->s = s;
        if (*s == '#') fail(a, a->tline, "preprocessor directives are not supported");
        else fail(a, a->tline, "syntax error: unexpected character '%c'", *s);
        return;
    }
    if (a->kind == T_INT && a->ival > 0xFFFFFFFFu) { a->s = s; fail(a, a->tline, "integer too large"); return; }
    size_t len = (size_t)(s - start);
    if (len >= sizeof a->text) len = sizeof a->text - 1;
    memcpy(a->text, start, len);
    a->text[len] = 0;
    a->s = s;
}

static int is_punct(const assembler *a, char c) { return a->kind == T_PUNCT && a->text[0] == c; }

static void unexpected(assembler *a) {
    if (a->failed) return;
    if (a->kind == T_END) fail(a, a->tline, "syntax error: unexpected end of file");
    else fail(a, a->tline, "syntax error: unexpected token '%s'", a->text);
}

static void emit(assembler *a, uint32_t token) {
    if (a->n == a->cap) {
        size_t cap = a->cap ? a->cap * 2 : 128;
        uint32_t *q = realloc(a->out, cap * sizeof *q);
        if (!q) { fail(a, a->line, "out of memory"); return; }
        a->out = q, a->cap = cap;
    }
    a->out[a->n++] = token;
}

/* A register: r0-r11, v0-v15, c<n>, a0, oPos, oFog, oPts, oD0-oD1, oT0-oT7. A constant may
 * carry a bracketed sum of numbers and at most one a0 component, before or after or instead of
 * its own number: c24[a0.x], c[a0.x + 24] and c[20 + a0.x + 4] are one register. */
static int parse_reg(assembler *a, vs_arg *r) {
    memset(r, 0, sizeof *r);
    if (a->kind != T_IDENT) { unexpected(a); return 0; }
    char name[48];
    int line = a->tline;
    snprintf(name, sizeof name, "%s", a->text);

    size_t plen = strlen(name);                             /* the name is a prefix and trailing digits */
    while (plen && is_digit(name[plen - 1])) plen--;
    size_t digits = strlen(name) - plen;
    uint32_t num = digits <= 6 ? (uint32_t)strtoul(name + plen, NULL, 10) : 0xFFFFFFFFu;
#define IS(p) (plen == sizeof (p) - 1 && !memcmp(name, p, plen))
    int good = 1;
    if (!strcmp(name, "oPos")) r->type = REG_RASTOUT, num = 0;
    else if (!strcmp(name, "oFog")) r->type = REG_RASTOUT, num = 1;
    else if (!strcmp(name, "oPts")) r->type = REG_RASTOUT, num = 2;
    else if (!strcmp(name, "a0")) r->type = REG_ADDR;
    else if (IS("oD")) r->type = REG_ATTROUT, good = digits && num <= 1;
    else if (IS("oT")) r->type = REG_TEXCRDOUT, good = digits && num <= 7;
    else if (IS("r")) r->type = REG_TEMP, good = digits && num <= 11;
    else if (IS("v")) r->type = REG_INPUT, good = digits && num <= 15;
    else if (IS("c")) r->type = REG_CONST, good = num <= 2047;
    else good = 0;
#undef IS
    if (!good) { fail(a, line, "invalid register '%s'", name); return 0; }
    next(a);

    if (is_punct(a, '[')) {
        if (r->type != REG_CONST) { fail(a, line, "invalid register '%s[...]'", name); return 0; }
        next(a);
        for (;;) {
            if (a->kind == T_INT) {
                if (a->ival > 2047 || (num += (uint32_t)a->ival) > 2047) { fail(a, line, "invalid register '%s[...]'", name); return 0; }
                next(a);
            } else if (a->kind == T_IDENT && !strcmp(a->text, "a0")) {
                if (r->rel) { fail(a, line, "only one address register reference allowed in a relative address expression"); return 0; }
                r->rel = 1;
                r->relcomp = -1;
                next(a);
                if (is_punct(a, '.')) {
                    static const char letters[] = "xyzwrgba";
                    next(a);
                    const char *l = a->kind == T_IDENT && a->text[0] && !a->text[1] ? strchr(letters, a->text[0]) : NULL;
                    if (!l) { unexpected(a); return 0; }
                    r->relcomp = (int)(l - letters) & 3;
                    next(a);
                }
                if (a->major == 1 && r->relcomp != 0) { fail(a, line, "only a0.x is allowed as a relative address register in vs_1_1"); return 0; }
                if (r->relcomp < 0) { fail(a, line, "relative address must name a single component of a0 (.x, .y, .z or .w)"); return 0; }
            } else { unexpected(a); return 0; }
            if (is_punct(a, ']')) { next(a); break; }
            if (!is_punct(a, '+')) { unexpected(a); return 0; }
            next(a);
        }
    } else if (r->type == REG_CONST && !digits) {
        fail(a, line, "invalid register '%s'", name);
        return 0;
    }
    r->num = num;
    return !a->failed;
}

/* The .xyzw after a register, as component numbers (rgba are the same four). Returns how many
 * letters, 0 when there is no selector, -1 after reporting a bad one. */
static int parse_selector(assembler *a, int comp[4], const char *what) {
    static const char letters[] = "xyzwrgba";
    if (!is_punct(a, '.')) return 0;
    next(a);
    if (a->kind != T_IDENT) { unexpected(a); return -1; }
    size_t n = strlen(a->text);
    for (size_t i = 0; i < n; i++) {
        const char *l = strchr(letters, a->text[i]);
        if (n > 4 || !l) { fail(a, a->tline, "invalid %s '%s'", what, a->text); return -1; }
        comp[i] = (int)(l - letters) & 3;
    }
    return (int)n;
}

/* A destination: a register and the components to write, in xyzw order; all four if none are
 * named. *masked says whether any were. */
static int parse_dst(assembler *a, vs_arg *r, int *masked) {
    int comp[4], n;
    if (!parse_reg(a, r)) return 0;
    if ((n = parse_selector(a, comp, "mask")) < 0) return 0;
    *masked = n > 0;
    r->mask = n ? 0 : 0xF;
    for (int i = 0; i < n; i++) {
        if (i && comp[i] <= comp[i - 1]) { fail(a, a->tline, "invalid mask '%s'", a->text); return 0; }
        r->mask |= 1u << comp[i];
    }
    if (n) next(a);
    return 1;
}

/* A source: -register.swizzle, both extras optional. A swizzle of fewer than four letters
 * repeats its last: .xy is .xyyy. */
static int parse_src(assembler *a, vs_arg *r) {
    int comp[4], n, neg = is_punct(a, '-');
    if (neg) next(a);
    if (!parse_reg(a, r)) return 0;
    if ((n = parse_selector(a, comp, "swizzle")) < 0) return 0;
    r->neg = neg;
    r->swz = n ? 0 : SWZ_XYZW;
    for (int i = 0; n && i < 4; i++) r->swz |= (uint32_t)comp[i < n ? i : n - 1] << 2 * i;
    if (n) next(a);
    return 1;
}

/* vs_1_1 or vs.1.1, first thing in the source. D3DX reads vs_1_0 as vs_1_1, so that too. */
static void parse_version(assembler *a) {
    char v[48] = "";
    int line = a->tline;
    if (a->kind == T_IDENT) {
        const char *s = a->s;                               /* the text right after the identifier */
        snprintf(v, sizeof v, "%s", a->text);
        if (!strcmp(v, "vs") && s[0] == '.' && is_digit(s[1]) && s[2] == '.' && is_digit(s[3])) {
            snprintf(v, sizeof v, "vs_%c_%c", s[1], s[3]);  /* the DirectX 8 spelling: vs.1.1 */
            a->s += 4;
        }
        next(a);
    }
    if (a->failed) return;
    if (!strcmp(v, "vs_1_1") || !strcmp(v, "vs_1_0")) a->major = 1, emit(a, 0xFFFE0101u);
    else if (!strcmp(v, "vs_2_0")) a->major = 2, emit(a, 0xFFFE0200u);
    else if (!strncmp(v, "vs_", 3) || !strncmp(v, "ps_", 3)) fail(a, line, "%s is not supported (vs_1_1 and vs_2_0 are)", v);
    else fail(a, line, "shader version expected");
}

/* dcl_<usage><index> v<n>: which vertex element register n is fed from. */
static void parse_dcl(assembler *a, const char *name, int line) {
    size_t plen = strlen(name);
    while (plen > 4 && is_digit(name[plen - 1])) plen--;
    uint32_t usage = 0, index = (uint32_t)strtoul(name + plen, NULL, 10);
    while (usage < NUSAGES && (strlen(g_usage[usage]) != plen - 4 || memcmp(g_usage[usage], name + 4, plen - 4))) usage++;
    if (usage == NUSAGES || index > 15 || strlen(name) - plen > 2) { fail(a, line, "syntax error: unexpected token '%s'", name); return; }
    next(a);
    vs_arg r;
    int masked;
    if (!parse_dst(a, &r, &masked)) return;
    if (r.type != REG_INPUT) { fail(a, line, "invalid register type for dcl: it must be an input register (v#)"); return; }
    if (r.mask != 0xF) { fail(a, line, "dcl must specify the full write mask (or none) for v# registers"); return; }
    emit(a, OP_DCL);
    emit(a, PARAM | index << 16 | usage);
    emit(a, reg_bits(r.type, r.num) | r.mask << 16);
}

/* def c<n>, x, y, z, w: a constant the shader carries with it. */
static void parse_def(assembler *a, int line) {
    vs_arg r;
    next(a);
    if (!parse_reg(a, &r)) return;
    if (r.type != REG_CONST || r.rel) { fail(a, line, "def needs a constant register (c#)"); return; }
    emit(a, OP_DEF);
    emit(a, reg_bits(r.type, r.num) | 0xFu << 16);
    for (int i = 0; i < 4; i++) {
        if (!is_punct(a, ',')) { unexpected(a); return; }
        next(a);
        int neg = is_punct(a, '-');
        if (neg || is_punct(a, '+')) next(a);
        if (a->kind != T_INT && a->kind != T_FLOAT) { unexpected(a); return; }
        double v = a->kind == T_INT ? (double)a->ival : a->fval;
        if (v > 3.4028235677973366e38) { fail(a, a->tline, "'%s' is too large for a float", a->text); return; }
        float f = (float)v;                                 /* through a double, as D3DX's atof goes */
        uint32_t bits;
        if (neg) f = -f;
        memcpy(&bits, &f, 4);
        emit(a, bits);
        next(a);
    }
}

static void parse_instruction(assembler *a, int *body) {
    static const char *const flow[] = {
        "call", "callnz", "loop", "endloop", "rep", "endrep", "if", "else", "endif", "break", "ret", "label",
        "defb", "defi", "setp", "texldl",
    };
    if (a->kind != T_IDENT) { unexpected(a); return; }
    char name[48];
    int line = a->tline;
    size_t at = a->n;                                       /* where its instruction token goes */
    snprintf(name, sizeof name, "%s", a->text);

    if (!strncmp(name, "dcl_", 4) || !strcmp(name, "def")) {
        if (*body) { fail(a, line, "def and dcl instructions must appear before other instructions"); return; }
        if (name[2] == 'l') parse_dcl(a, name, line); else parse_def(a, line);
    } else {
        const vs_op *op = op_by_name(name);
        if (!op) {
            int known = !strncmp(name, "if_", 3) || !strncmp(name, "break", 5) || !strncmp(name, "setp_", 5);    /* if_gt, breakp, setp_lt... */
            for (size_t i = 0; i < sizeof flow / sizeof flow[0]; i++) known |= !strcmp(name, flow[i]);
            if (known) fail(a, line, "'%s' is not supported: only straight-line shaders are", name);
            else unexpected(a);
            return;
        }
        if (op->since > a->major) { fail(a, line, "'%s' is not a valid instruction in this shader version", name); return; }
        next(a);
        *body = 1;
        if (op->code == OP_NOP) { emit(a, OP_NOP); return; }

        vs_arg dst, src[3];
        int masked = 0, cols, rows = matrix_shape(op->code, &cols);
        if (!parse_dst(a, &dst, &masked)) return;
        for (int i = 0; i < op->nsrc; i++) {
            if (!is_punct(a, ',')) { unexpected(a); return; }
            next(a);
            if (!parse_src(a, &src[i])) return;
        }

        /* The rules that change what is encoded, and the ones D3DX enforces alongside them. */
        if (dst.type == REG_INPUT || dst.type == REG_CONST) { fail(a, line, "invalid register type for the destination"); return; }
        if (dst.type == REG_ADDR || op->code == OP_MOVA) {
            uint32_t writer = a->major == 1 ? OP_MOV : OP_MOVA;
            if (dst.type != REG_ADDR || op->code != writer) {
                fail(a, line, "only the %s instruction is allowed to write to the address register, and mova writes nothing else", a->major == 1 ? "mov" : "mova");
                return;
            }
            if (a->major == 1 && (!masked || dst.mask != 1)) { fail(a, line, "format for address register must be a0.x"); return; }
        }
        if (dst.type == REG_RASTOUT && dst.num && masked) { fail(a, line, "scalar registers cannot be masked"); return; }
        for (int i = 0; i < op->nsrc; i++) {
            if (src[i].type > REG_CONST) { fail(a, line, "invalid register type for source %d", i + 1); return; }
            if (!(op->scalar >> i & 1)) continue;
            /* One component: vs_1_1 takes .w when none is named (it cannot tell .xyzw from nothing),
             * vs_2_0 wants it said. */
            if (src[i].swz == SWZ_XYZW && a->major == 1) src[i].swz = 0xFF;
            if (src[i].swz != (src[i].swz & 3) * 0x55) {
                fail(a, line, "%s requires a replicate swizzle (.x, .y, .z or .w) on source %d to select a component", name, i + 1);
                return;
            }
        }
        if (rows) {                                         /* a matrix instruction writes a component a row, no fewer */
            uint32_t need = (1u << rows) - 1;
            if (dst.mask == 0xF && a->major == 1) dst.mask = need;         /* vs_1_1 narrows it for you */
            if (dst.mask != need) {
                char want[5];
                mask_text(want, need);
                fail(a, line, "%s must use destination write mask .%s", name, want);
                return;
            }
        }
        if (op->code == OP_FRC && a->major == 1 && dst.mask != 2 && dst.mask != 3) {
            fail(a, line, "the only valid write masks for frc in vs_1_1 are .xy and .y");
            return;
        }
        uint32_t code = op->code;
        if (code == OP_SUB) {                               /* D3DX has no sub of its own: a - b is a + -b */
            code = OP_ADD;
            src[1].neg = !src[1].neg;
        }

        emit(a, code);
        emit(a, reg_bits(dst.type, dst.num) | dst.mask << 16);
        for (int i = 0; i < op->nsrc; i++) {
            emit(a, reg_bits(src[i].type, src[i].num) | src[i].swz << 16 | (src[i].neg ? SRC_NEGATE : 0) | (src[i].rel ? SRC_RELATIVE : 0));
            if (src[i].rel && a->major >= 2) emit(a, reg_bits(REG_ADDR, 0) | (uint32_t)src[i].relcomp * 0x55u << 16);
        }
    }
    if (a->major >= 2 && !a->failed) a->out[at] |= (uint32_t)(a->n - at - 1) << 24;
}

int d3d9_vs_assemble(const char *src, size_t len, uint32_t **tokens, size_t *count, char **error) {
    assembler a;
    size_t n = 0;
    int body = 0;                                           /* an instruction other than dcl and def has gone by */
    memset(&a, 0, sizeof a);
    if (tokens) *tokens = NULL;
    if (count) *count = 0;
    if (error) *error = NULL;

    while (src && n < len && src[n]) n++;                   /* a NUL inside len ends the text too */
    char *text = malloc(n + 1);
    if (text) {
        if (n) memcpy(text, src, n);
        text[n] = 0;
        a.s = text, a.line = 1;
        next(&a);
        parse_version(&a);
        while (!a.failed && a.kind != T_END) parse_instruction(&a, &body);
        emit(&a, TOKEN_END);
    }
    free(text);
    if (!text || a.failed || !tokens) {
        free(a.out);
        if (error) *error = a.err; else free(a.err);
        return -1;
    }
    *tokens = a.out;
    if (count) *count = a.n;
    return 0;
}
