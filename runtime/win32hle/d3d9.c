/*
 * d3d9.c — Direct3D 9 as the guest sees it.
 *
 * The COM objects a Direct3D 9 game holds (IDirect3D9, the device, textures,
 * surfaces, vertex and index buffers, vertex shaders and declarations) and
 * the device's state. Pixels and vertices live in guest memory, where the
 * game locks and writes them; nothing here calls the graphics library.
 * d3d9_gl.c draws from this state.
 *
 * What is here is what games written against the fixed-function pipeline and
 * assembly vertex shaders ask for: one swap chain, no pixel shaders, no
 * volume or cube textures, no state blocks, no queries. A method that is not
 * here stops the run with its name the first time it is called.
 *
 * Built only with HLE_WITH_D3D9 (it needs d3d9_gl.c and OpenGL ES 3).
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win32hle.h"

#ifndef HLE_WITH_D3D9
const win32hle_shim win32hle_d3d9[] = { { NULL, NULL } };
#else
#include "d3d9_priv.h"

d3d9_state d3d9;
int hle_d3d9_headless;
int hle_d3d9_log;                              /* say what is drawn into, through which viewport, and what is read back */
void (*hle_d3d9_frame_hook)(unsigned frame);
static int g_desk_w = 1280, g_desk_h = 720;
void hle_d3d9_desktop(int w, int h) { g_desk_w = w, g_desk_h = h; }

static uint32_t g_d3d, g_dev;
static uint32_t g_vt_d3d, g_vt_dev, g_vt_tex, g_vt_surf, g_vt_vb, g_vt_ib, g_vt_vs, g_vt_decl;
static int g_next_id = 1;

/* An object is its vtable, its count, then the host's record of it. */
#define HOSTP(obj)   (*(void **)G2H((obj) + 8))
#define COUNT(obj)   MEM32((obj) + 4)
static uint32_t object_new(uint32_t vt, void *host) {
    uint32_t o = hle_com_new(vt, 16);
    if (o) HOSTP(o) = host;
    return o;
}
static float AF(int i) { uint32_t u = A32(i); float f; memcpy(&f, &u, 4); return f; }
static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

static void once(const char *what) {
    static const char *said[64];
    static int n;
    for (int i = 0; i < n; i++) if (said[i] == what) return;
    if (n < 64) said[n++] = what;
    fprintf(stderr, "[d3d9] not done: %s\n", what);
}

/* ---- pixels ---------------------------------------------------------------- */
int d3d9_fmt_bytes(int fmt) {
    switch (fmt) {
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_A8B8G8R8: case D3DFMT_X8B8G8R8: return 4;
    case D3DFMT_R8G8B8: return 3;
    case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4: case D3DFMT_X4R4G4B4:
    case D3DFMT_A8L8: return 2;
    case D3DFMT_A8: case D3DFMT_L8: return 1;
    }
    return 0;
}
int d3d9_fmt_alpha(int fmt) {
    return fmt == D3DFMT_A8R8G8B8 || fmt == D3DFMT_A8B8G8R8 || fmt == D3DFMT_A1R5G5B5 || fmt == D3DFMT_A4R4G4B4 ||
           fmt == D3DFMT_A8 || fmt == D3DFMT_A8L8;
}
static int is_depth(int fmt) {
    return fmt == D3DFMT_D16 || fmt == D3DFMT_D16_LOCKABLE || fmt == D3DFMT_D24S8 || fmt == D3DFMT_D24X8 ||
           fmt == D3DFMT_D32;
}
#define X5(v) (uint8_t)(((v) << 3) | ((v) >> 2))
#define X6(v) (uint8_t)(((v) << 2) | ((v) >> 4))
void d3d9_to_bgra(int fmt, const void *src, uint8_t *o, int n) {
    const uint8_t *b = src;
    const uint16_t *w = src;
    switch (fmt) {
    case D3DFMT_A8R8G8B8: memcpy(o, src, 4u * (size_t)n); break;
    case D3DFMT_X8R8G8B8: for (int i = 0; i < n; i++, o += 4, b += 4) o[0] = b[0], o[1] = b[1], o[2] = b[2], o[3] = 255; break;
    case D3DFMT_A8B8G8R8: for (int i = 0; i < n; i++, o += 4, b += 4) o[0] = b[2], o[1] = b[1], o[2] = b[0], o[3] = b[3]; break;
    case D3DFMT_X8B8G8R8: for (int i = 0; i < n; i++, o += 4, b += 4) o[0] = b[2], o[1] = b[1], o[2] = b[0], o[3] = 255; break;
    case D3DFMT_R8G8B8:   for (int i = 0; i < n; i++, o += 4, b += 3) o[0] = b[0], o[1] = b[1], o[2] = b[2], o[3] = 255; break;
    case D3DFMT_R5G6B5:
        for (int i = 0; i < n; i++, o += 4) { unsigned v = w[i]; o[0] = X5(v & 31), o[1] = X6((v >> 5) & 63), o[2] = X5(v >> 11), o[3] = 255; }
        break;
    case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5:
        for (int i = 0; i < n; i++, o += 4) {
            unsigned v = w[i];
            o[0] = X5(v & 31), o[1] = X5((v >> 5) & 31), o[2] = X5((v >> 10) & 31);
            o[3] = (fmt == D3DFMT_X1R5G5B5 || (v & 0x8000)) ? 255 : 0;
        }
        break;
    case D3DFMT_A4R4G4B4: case D3DFMT_X4R4G4B4:
        for (int i = 0; i < n; i++, o += 4) {
            unsigned v = w[i];
            o[0] = (uint8_t)((v & 15) * 17), o[1] = (uint8_t)(((v >> 4) & 15) * 17), o[2] = (uint8_t)(((v >> 8) & 15) * 17);
            o[3] = fmt == D3DFMT_X4R4G4B4 ? 255 : (uint8_t)((v >> 12) * 17);
        }
        break;
    case D3DFMT_A8:   for (int i = 0; i < n; i++, o += 4) o[0] = o[1] = o[2] = 0, o[3] = b[i]; break;
    case D3DFMT_L8:   for (int i = 0; i < n; i++, o += 4) o[0] = o[1] = o[2] = b[i], o[3] = 255; break;
    case D3DFMT_A8L8: for (int i = 0; i < n; i++, o += 4) o[0] = o[1] = o[2] = b[2 * i], o[3] = b[2 * i + 1]; break;
    default: memset(o, 0, 4u * (size_t)n); break;
    }
}
#define N5(v) (unsigned)(((v) * 31 + 127) / 255)
#define N6(v) (unsigned)(((v) * 63 + 127) / 255)
#define N4(v) (unsigned)(((v) * 15 + 127) / 255)
void d3d9_from_bgra(int fmt, const uint8_t *s, void *dst, int n) {
    uint8_t *b = dst;
    uint16_t *w = dst;
    switch (fmt) {
    case D3DFMT_A8R8G8B8: memcpy(dst, s, 4u * (size_t)n); break;
    case D3DFMT_X8R8G8B8: for (int i = 0; i < n; i++, s += 4, b += 4) b[0] = s[0], b[1] = s[1], b[2] = s[2], b[3] = 255; break;
    case D3DFMT_A8B8G8R8: for (int i = 0; i < n; i++, s += 4, b += 4) b[0] = s[2], b[1] = s[1], b[2] = s[0], b[3] = s[3]; break;
    case D3DFMT_X8B8G8R8: for (int i = 0; i < n; i++, s += 4, b += 4) b[0] = s[2], b[1] = s[1], b[2] = s[0], b[3] = 255; break;
    case D3DFMT_R8G8B8:   for (int i = 0; i < n; i++, s += 4, b += 3) b[0] = s[0], b[1] = s[1], b[2] = s[2]; break;
    case D3DFMT_R5G6B5:   for (int i = 0; i < n; i++, s += 4) w[i] = (uint16_t)(N5(s[0]) | (N6(s[1]) << 5) | (N5(s[2]) << 11)); break;
    case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5:
        for (int i = 0; i < n; i++, s += 4)
            w[i] = (uint16_t)(N5(s[0]) | (N5(s[1]) << 5) | (N5(s[2]) << 10) | ((fmt == D3DFMT_X1R5G5B5 || s[3] >= 128) ? 0x8000 : 0));
        break;
    case D3DFMT_A4R4G4B4: case D3DFMT_X4R4G4B4:
        for (int i = 0; i < n; i++, s += 4)
            w[i] = (uint16_t)(N4(s[0]) | (N4(s[1]) << 4) | (N4(s[2]) << 8) | ((fmt == D3DFMT_X4R4G4B4 ? 15u : N4(s[3])) << 12));
        break;
    case D3DFMT_A8:   for (int i = 0; i < n; i++, s += 4) b[i] = s[3]; break;
    case D3DFMT_L8:   for (int i = 0; i < n; i++, s += 4) b[i] = s[2]; break;
    case D3DFMT_A8L8: for (int i = 0; i < n; i++, s += 4) b[2 * i] = s[2], b[2 * i + 1] = s[3]; break;
    default: break;
    }
}

/* ---- surfaces and textures ------------------------------------------------------ */
static int on_gl(const d3d9_surface *s) {      /* its pixels are GL's, not the guest's */
    return s->kind == D3D9_SURF_BACK || s->kind == D3D9_SURF_TARGET ||
           (s->kind == D3D9_SURF_LEVEL && (s->usage & D3DUSAGE_RENDERTARGET));
}
static d3d9_surface *surface_new(int kind, int w, int h, int fmt, int usage, int pool) {
    d3d9_surface *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->kind = kind, s->w = w, s->h = h, s->fmt = fmt, s->usage = usage, s->pool = pool;
    s->pitch = (w * d3d9_fmt_bytes(fmt) + 3) & ~3;         /* rows start on a 4-byte boundary, as Direct3D's do */
    s->self = object_new(g_vt_surf, s);
    if (!s->self) { free(s); return NULL; }
    return s;
}
uint32_t d3d9_surface_bits(d3d9_surface *s) {
    if (!s->bits && s->pitch) s->bits = hle_guest_alloc((uint32_t)s->pitch * (uint32_t)s->h);
    return s->bits;
}
static void surface_free(d3d9_surface *s) {
    if (d3d9.rt == s) d3d9.rt = d3d9.back;
    if (d3d9.ds == s) d3d9.ds = NULL;
    d3d9_gl_forget_surface(s);
    d3d9_gl_retire(s->gl_tex, s->gl_rb, s->gl_fbo, 0);
    if (s->bits) hle_guest_free(s->bits);
    hle_guest_free(s->self);
    free(s);
}
d3d9_surface *d3d9_surface_of(uint32_t obj) { return obj && MEM32(obj) == g_vt_surf ? HOSTP(obj) : NULL; }
d3d9_texture *d3d9_texture_of(uint32_t obj) { return obj && MEM32(obj) == g_vt_tex ? HOSTP(obj) : NULL; }

uint32_t d3d9_texture_new(int w, int h, int levels, int usage, int fmt, int pool) {
    if (w <= 0 || h <= 0 || !d3d9_fmt_bytes(fmt)) return 0;
    int full = 1;
    for (int m = w > h ? w : h; m > 1; m >>= 1) full++;
    if (levels <= 0 || levels > full) levels = full;
    if (levels > D3D9_MAX_LEVELS) levels = D3D9_MAX_LEVELS;
    if (usage & D3DUSAGE_RENDERTARGET) levels = 1;
    d3d9_texture *t = calloc(1, sizeof *t);
    if (!t) return 0;
    t->w = w, t->h = h, t->levels = levels, t->fmt = fmt, t->usage = usage, t->pool = pool;
    t->self = object_new(g_vt_tex, t);
    for (int i = 0; t->self && i < levels; i++) {
        int lw = w >> i ? w >> i : 1, lh = h >> i ? h >> i : 1;
        d3d9_surface *s = surface_new(D3D9_SURF_LEVEL, lw, lh, fmt, usage, pool);
        if (!s) break;
        s->tex = t, s->level = i;
        t->level[i] = s;
    }
    if (!t->self || !t->level[levels - 1]) {
        for (int i = 0; i < levels; i++) if (t->level[i]) surface_free(t->level[i]);
        if (t->self) hle_guest_free(t->self);
        free(t);
        return 0;
    }
    return t->self;
}
static void texture_free(d3d9_texture *t) {
    for (int i = 0; i < 8; i++) if (d3d9.tex[i] == t) d3d9.tex[i] = NULL;
    for (int i = 0; i < t->levels; i++) surface_free(t->level[i]);
    d3d9_gl_retire(t->gl, 0, 0, 0);
    hle_guest_free(t->self);
    free(t);
}

/* Where a picture read back is not black, and what its first pixel is. */
static void log_read(const d3d9_surface *s, int x, int y, int w, int h, const uint8_t *px) {
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++) {
            const uint8_t *p = px + ((size_t)r * (size_t)w + (size_t)c) * 4;
            if (p[0] | p[1] | p[2]) { if (c < x0) x0 = c; if (c > x1) x1 = c; if (r < y0) y0 = r; if (r > y1) y1 = r; }
        }
    fprintf(stderr, "[d3d9] read back %dx%d at %d,%d of a %dx%d target: ", w, h, x, y, s->w, s->h);
    if (x1 < 0) fprintf(stderr, "all black\n");
    else fprintf(stderr, "not black from %d,%d to %d,%d; the first pixel is %02X%02X%02X%02X\n", x0, y0, x1, y1, px[3], px[2], px[1], px[0]);
}
uint8_t *d3d9_surface_read(d3d9_surface *s, int x, int y, int w, int h) {
    uint8_t *px = calloc((size_t)w * (size_t)h, 4);
    if (!px) return NULL;
    if (on_gl(s)) {
        d3d9_gl_read(s, x, y, w, h, px);
        if (hle_d3d9_log) log_read(s, x, y, w, h, px);
        return px;
    }
    if (!s->bits) return px;
    int bpp = d3d9_fmt_bytes(s->fmt);
    for (int r = 0; r < h; r++)
        d3d9_to_bgra(s->fmt, (uint8_t *)G2H(s->bits) + (size_t)(y + r) * (size_t)s->pitch + (size_t)x * (size_t)bpp,
                     px + (size_t)r * (size_t)w * 4, w);
    return px;
}
void d3d9_surface_write(d3d9_surface *s, int x, int y, int w, int h, const uint8_t *bgra) {
    if (on_gl(s)) { d3d9_gl_upload(s, x, y, w, h, bgra); return; }
    if (!d3d9_surface_bits(s)) return;
    int bpp = d3d9_fmt_bytes(s->fmt);
    for (int r = 0; r < h; r++)
        d3d9_from_bgra(s->fmt, bgra + (size_t)r * (size_t)w * 4,
                       (uint8_t *)G2H(s->bits) + (size_t)(y + r) * (size_t)s->pitch + (size_t)x * (size_t)bpp, w);
    s->dirty = 1;
    if (s->tex) s->tex->dirty = 1;
}

/* A level of a texture is counted with its texture; a surface of its own has
 * its own count, and the device's two are never given up. */
static uint32_t surface_addref(d3d9_surface *s) { return ++COUNT(s->tex ? s->tex->self : s->self); }
static uint32_t surface_release(d3d9_surface *s) {
    if (s->tex) {
        uint32_t n = COUNT(s->tex->self) ? --COUNT(s->tex->self) : 0;
        if (!n) texture_free(s->tex);
        return n;
    }
    uint32_t n = COUNT(s->self) ? --COUNT(s->self) : 0;
    if (!n) {
        if (s == d3d9.back || s == d3d9.back_ds) { COUNT(s->self) = 1; return 0; }
        surface_free(s);
    }
    return n;
}

static void s_QueryInterface(void) { d3d9_surface *s = HOSTP(A32(0)); if (A32(2)) MEM32(A32(2)) = A32(0); surface_addref(s); RET(D3D_OK, 3); }
static void s_AddRef(void)  { RET(surface_addref(HOSTP(A32(0))), 1); }
static void s_Release(void) { RET(surface_release(HOSTP(A32(0))), 1); }
static void any_GetDevice(void) { if (A32(1)) MEM32(A32(1)) = g_dev; COUNT(g_dev)++; RET(D3D_OK, 2); }
static void any_SetPriority(void) { RET(0, 2); }
static void any_GetPriority(void) { RET(0, 1); }
static void any_PreLoad(void) { RETV(1); }
static void s_GetType(void) { RET(D3DRTYPE_SURFACE, 1); }
/* What a surface is part of: its texture for a level, the device for a
 * surface of its own. Asked for as an interface, and refused if the container
 * is not one of those: a game asks a surface for "its texture" to find out
 * whether it has one. */
static void s_GetContainer(void) {             /* (this, iid, &out) */
    static const uint8_t unknown[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46 };
    static const uint8_t device[16] = { 0x96, 0x3B, 0x22, 0xD0, 0x7A, 0xBF, 0xFD, 0x43, 0x92, 0xBD, 0xA4, 0x3B, 0x0D, 0x82, 0xB9, 0xEB };
    static const uint8_t texture[3][16] = {
        { 0x27, 0x12, 0xC3, 0x85, 0xE5, 0x3D, 0x00, 0x4F, 0x9B, 0x3A, 0xF1, 0x1A, 0xC3, 0x8C, 0x18, 0xB5 },   /* IDirect3DTexture9 */
        { 0x7E, 0xA8, 0x0C, 0x58, 0x3C, 0x1D, 0x54, 0x4D, 0x99, 0x1D, 0xB7, 0xD3, 0xE3, 0xC2, 0x98, 0xCE },   /* IDirect3DBaseTexture9 */
        { 0x5D, 0xC0, 0xEE, 0x05, 0x7D, 0x8F, 0x62, 0x43, 0xB9, 0x99, 0xD1, 0xBA, 0xF3, 0x57, 0xC7, 0x04 },   /* IDirect3DResource9 */
    };
    d3d9_surface *s = HOSTP(A32(0));
    if (!A32(1) || !A32(2)) RET(D3DERR_INVALIDCALL, 3);
    const uint8_t *iid = G2H(A32(1));
    int any = !memcmp(iid, unknown, 16);
    MEM32(A32(2)) = 0;
    if (s->tex) {
        if (!any && memcmp(iid, texture[0], 16) && memcmp(iid, texture[1], 16) && memcmp(iid, texture[2], 16)) RET(E_NOINTERFACE_, 3);
        MEM32(A32(2)) = s->tex->self, COUNT(s->tex->self)++;
    } else {
        if (!any && memcmp(iid, device, 16)) RET(E_NOINTERFACE_, 3);
        MEM32(A32(2)) = g_dev, COUNT(g_dev)++;
    }
    RET(D3D_OK, 3);
}
static void desc_write(uint32_t p, int fmt, int type, int usage, int pool, int w, int h) {
    MEM32(p) = (uint32_t)fmt, MEM32(p + 4) = (uint32_t)type, MEM32(p + 8) = (uint32_t)usage, MEM32(p + 12) = (uint32_t)pool;
    MEM32(p + 16) = 0, MEM32(p + 20) = 0, MEM32(p + 24) = (uint32_t)w, MEM32(p + 28) = (uint32_t)h;
}
static void s_GetDesc(void) {
    d3d9_surface *s = HOSTP(A32(0));
    if (!A32(1)) RET(D3DERR_INVALIDCALL, 2);
    desc_write(A32(1), s->fmt, D3DRTYPE_SURFACE, s->usage, s->pool, s->w, s->h);
    RET(D3D_OK, 2);
}
static uint32_t surface_lock(d3d9_surface *s, uint32_t out, uint32_t rect, uint32_t flags) {
    if (!out || s->kind == D3D9_SURF_BACK || s->kind == D3D9_SURF_DEPTH ||
        (s->kind == D3D9_SURF_LEVEL && (s->usage & D3DUSAGE_RENDERTARGET)))
        return D3DERR_INVALIDCALL;                  /* only a target made lockable can be locked */
    if (!d3d9_surface_bits(s)) return D3DERR_OUTOFVIDEOMEMORY;
    if (s->kind == D3D9_SURF_TARGET) {              /* what has been drawn into it comes back first */
        uint8_t *px = malloc((size_t)s->w * (size_t)s->h * 4);
        if (px && d3d9_gl_read(s, 0, 0, s->w, s->h, px) == 0)
            for (int r = 0; r < s->h; r++)
                d3d9_from_bgra(s->fmt, px + (size_t)r * (size_t)s->w * 4, (uint8_t *)G2H(s->bits) + (size_t)r * (size_t)s->pitch, s->w);
        free(px);
    }
    uint32_t at = s->bits;
    if (rect) at += MEM32(rect + 4) * (uint32_t)s->pitch + MEM32(rect) * (uint32_t)d3d9_fmt_bytes(s->fmt);
    MEM32(out) = (uint32_t)s->pitch, MEM32(out + 4) = at;
    s->locked = 1, s->lock_write = !(flags & D3DLOCK_READONLY);
    return D3D_OK;
}
static uint32_t surface_unlock(d3d9_surface *s) {
    if (!s->locked) return D3DERR_INVALIDCALL;
    s->locked = 0;
    if (!s->lock_write) return D3D_OK;
    if (s->kind == D3D9_SURF_TARGET) {
        uint8_t *px = d3d9_surface_read(&(d3d9_surface){ .kind = D3D9_SURF_PLAIN, .w = s->w, .h = s->h, .fmt = s->fmt,
                                                         .bits = s->bits, .pitch = s->pitch }, 0, 0, s->w, s->h);
        if (px) d3d9_gl_upload(s, 0, 0, s->w, s->h, px);
        free(px);
        return D3D_OK;
    }
    s->dirty = 1;
    if (s->tex) s->tex->dirty = 1;
    return D3D_OK;
}
static void s_LockRect(void)   { RET(surface_lock(HOSTP(A32(0)), A32(1), A32(2), A32(3)), 4); }   /* (this, &locked, rect, flags) */
static void s_UnlockRect(void) { RET(surface_unlock(HOSTP(A32(0))), 1); }

static void t_QueryInterface(void) { if (A32(2)) MEM32(A32(2)) = A32(0); COUNT(A32(0))++; RET(D3D_OK, 3); }
static void t_AddRef(void)  { RET(++COUNT(A32(0)), 1); }
static void t_Release(void) {
    uint32_t n = COUNT(A32(0)) ? --COUNT(A32(0)) : 0;
    if (!n) texture_free(HOSTP(A32(0)));
    RET(n, 1);
}
static void t_GetType(void) { RET(D3DRTYPE_TEXTURE, 1); }
static void t_SetLOD(void) { RET(0, 2); }
static void t_GetLOD(void) { RET(0, 1); }
static void t_GetLevelCount(void) { RET(((d3d9_texture *)HOSTP(A32(0)))->levels, 1); }
static void t_SetAutoGenFilterType(void) { RET(D3D_OK, 2); }
static void t_GetAutoGenFilterType(void) { RET(D3DTEXF_LINEAR, 1); }
static void t_GenerateMipSubLevels(void) { RETV(1); }
static void t_GetLevelDesc(void) {             /* (this, level, &desc) */
    d3d9_texture *t = HOSTP(A32(0));
    if (A32(1) >= (uint32_t)t->levels || !A32(2)) RET(D3DERR_INVALIDCALL, 3);
    d3d9_surface *s = t->level[A32(1)];
    desc_write(A32(2), s->fmt, D3DRTYPE_SURFACE, s->usage, s->pool, s->w, s->h);
    RET(D3D_OK, 3);
}
static void t_GetSurfaceLevel(void) {          /* (this, level, &surface) */
    d3d9_texture *t = HOSTP(A32(0));
    if (A32(1) >= (uint32_t)t->levels || !A32(2)) RET(D3DERR_INVALIDCALL, 3);
    MEM32(A32(2)) = t->level[A32(1)]->self;
    COUNT(t->self)++;
    RET(D3D_OK, 3);
}
static void t_LockRect(void) {                 /* (this, level, &locked, rect, flags) */
    d3d9_texture *t = HOSTP(A32(0));
    if (A32(1) >= (uint32_t)t->levels) RET(D3DERR_INVALIDCALL, 5);
    RET(surface_lock(t->level[A32(1)], A32(2), A32(3), A32(4)), 5);
}
static void t_UnlockRect(void) {               /* (this, level) */
    d3d9_texture *t = HOSTP(A32(0));
    if (A32(1) >= (uint32_t)t->levels) RET(D3DERR_INVALIDCALL, 2);
    RET(surface_unlock(t->level[A32(1)]), 2);
}
static void t_AddDirtyRect(void) {             /* (this, rect) */
    d3d9_texture *t = HOSTP(A32(0));
    t->level[0]->dirty = 1, t->dirty = 1;
    RET(D3D_OK, 2);
}

/* ---- vertex and index buffers ------------------------------------------------------ */
static uint32_t buffer_new(int is_index, int size, int usage, int fmt_or_fvf, int pool) {
    d3d9_buffer *b = calloc(1, sizeof *b);
    if (!b || size <= 0) { free(b); return 0; }
    b->is_index = is_index, b->size = size, b->usage = usage, b->pool = pool;
    if (is_index) b->fmt = fmt_or_fvf; else b->fvf = fmt_or_fvf, b->fmt = D3DFMT_VERTEXDATA;
    b->data = hle_guest_alloc((uint32_t)size);
    b->self = b->data ? object_new(is_index ? g_vt_ib : g_vt_vb, b) : 0;
    if (!b->self) { if (b->data) hle_guest_free(b->data); free(b); return 0; }
    b->dirty = 1;
    return b->self;
}
static void b_Release(void) {
    uint32_t n = COUNT(A32(0)) ? --COUNT(A32(0)) : 0;
    if (!n) {
        d3d9_buffer *b = HOSTP(A32(0));
        if (d3d9.vb == b) d3d9.vb = NULL;
        if (d3d9.ib == b) d3d9.ib = NULL;
        d3d9_gl_retire(0, 0, 0, b->gl);
        hle_guest_free(b->data);
        hle_guest_free(b->self);
        free(b);
    }
    RET(n, 1);
}
static void b_GetType(void) { RET(((d3d9_buffer *)HOSTP(A32(0)))->is_index ? D3DRTYPE_INDEXBUFFER : D3DRTYPE_VERTEXBUFFER, 1); }
static void b_Lock(void) {                     /* (this, offset, size, &data, flags) */
    d3d9_buffer *b = HOSTP(A32(0));
    if (!A32(3) || A32(1) > (uint32_t)b->size) RET(D3DERR_INVALIDCALL, 5);
    MEM32(A32(3)) = b->data + A32(1);
    if (!(A32(4) & D3DLOCK_READONLY)) b->dirty = 1;     /* written between here and the next draw */
    RET(D3D_OK, 5);
}
static void b_Unlock(void) { RET(D3D_OK, 1); }
static void b_GetDesc(void) {
    d3d9_buffer *b = HOSTP(A32(0));
    uint32_t p = A32(1);
    if (!p) RET(D3DERR_INVALIDCALL, 2);
    MEM32(p) = (uint32_t)b->fmt, MEM32(p + 4) = b->is_index ? D3DRTYPE_INDEXBUFFER : D3DRTYPE_VERTEXBUFFER;
    MEM32(p + 8) = (uint32_t)b->usage, MEM32(p + 12) = (uint32_t)b->pool, MEM32(p + 16) = (uint32_t)b->size;
    if (!b->is_index) MEM32(p + 20) = (uint32_t)b->fvf;
    RET(D3D_OK, 2);
}

/* ---- vertex shaders and declarations ---------------------------------------------- */
static void vs_Release(void) {
    uint32_t n = COUNT(A32(0)) ? --COUNT(A32(0)) : 0;
    if (!n) {
        d3d9_vshader *v = HOSTP(A32(0));
        if (d3d9.vs == v) d3d9.vs = NULL;
        free(v->tokens), free(v->glsl);
        hle_guest_free(v->self);
        free(v);
    }
    RET(n, 1);
}
static void vs_GetFunction(void) {             /* (this, data, &size) */
    d3d9_vshader *v = HOSTP(A32(0));
    uint32_t bytes = (uint32_t)(v->ntokens * 4);
    if (A32(1)) { if (A32(2) && MEM32(A32(2)) < bytes) RET(D3DERR_INVALIDCALL, 3); memcpy(G2H(A32(1)), v->tokens, bytes); }
    if (A32(2)) MEM32(A32(2)) = bytes;
    RET(D3D_OK, 3);
}
static void decl_Release(void) {
    uint32_t n = COUNT(A32(0)) ? --COUNT(A32(0)) : 0;
    if (!n) {
        d3d9_vdecl *d = HOSTP(A32(0));
        if (d3d9.decl == d) d3d9.decl = NULL;
        hle_guest_free(d->self);
        free(d);
    }
    RET(n, 1);
}
static void decl_GetDeclaration(void) {        /* (this, elements, &count) */
    d3d9_vdecl *d = HOSTP(A32(0));
    if (A32(1)) {
        uint8_t *o = G2H(A32(1));
        memcpy(o, d->el, 8u * (size_t)d->n);
        memcpy(o + 8 * d->n, "\xFF\x00\x00\x00\x11\x00\x00\x00", 8);     /* D3DDECL_END */
    }
    if (A32(2)) MEM32(A32(2)) = (uint32_t)d->n + 1;
    RET(D3D_OK, 3);
}

/* ---- the device's state -------------------------------------------------------------- */
static void identity(float *m) { memset(m, 0, 64); m[0] = m[5] = m[10] = m[15] = 1.0f; }
static void state_defaults(int depth) {
    uint32_t *rs = d3d9.rs;
    memset(rs, 0, sizeof d3d9.rs);
    rs[D3DRS_ZENABLE] = depth ? 1 : 0, rs[D3DRS_FILLMODE] = 3, rs[D3DRS_SHADEMODE] = 2, rs[D3DRS_ZWRITEENABLE] = 1;
    rs[D3DRS_LASTPIXEL] = 1, rs[D3DRS_SRCBLEND] = 2, rs[D3DRS_DESTBLEND] = 1, rs[D3DRS_CULLMODE] = D3DCULL_CCW;
    rs[D3DRS_ZFUNC] = 4, rs[D3DRS_ALPHAFUNC] = 8, rs[D3DRS_FOGEND] = fbits(1.0f), rs[D3DRS_FOGDENSITY] = fbits(1.0f);
    rs[D3DRS_STENCILFAIL] = rs[D3DRS_STENCILZFAIL] = rs[D3DRS_STENCILPASS] = 1, rs[D3DRS_STENCILFUNC] = 8;
    rs[D3DRS_STENCILMASK] = rs[D3DRS_STENCILWRITEMASK] = 0xFFFFFFFFu, rs[D3DRS_TEXTUREFACTOR] = 0xFFFFFFFFu;
    rs[D3DRS_CLIPPING] = 1, rs[D3DRS_LIGHTING] = 1, rs[D3DRS_COLORVERTEX] = 1, rs[D3DRS_LOCALVIEWER] = 1;
    rs[D3DRS_DIFFUSEMATERIALSOURCE] = D3DMCS_COLOR1, rs[D3DRS_SPECULARMATERIALSOURCE] = D3DMCS_COLOR2;
    rs[D3DRS_POINTSIZE] = fbits(1.0f), rs[155] = fbits(1.0f), rs[158] = fbits(1.0f);       /* point size min, scale A */
    rs[D3DRS_MULTISAMPLEANTIALIAS] = 1, rs[162] = 0xFFFFFFFFu, rs[166] = fbits(64.0f);     /* sample mask, point size max */
    rs[D3DRS_COLORWRITEENABLE] = 0xF, rs[D3DRS_BLENDOP] = 1, rs[172] = 3, rs[173] = 1;      /* position and normal degree */
    rs[178] = rs[179] = fbits(1.0f), rs[182] = fbits(1.0f);                                 /* tessellation levels */
    rs[186] = rs[187] = rs[188] = 1, rs[189] = 8;                                           /* the other face's stencil */
    rs[190] = rs[191] = rs[192] = 0xF, rs[D3DRS_BLENDFACTOR] = 0xFFFFFFFFu;
    rs[D3DRS_SRCBLENDALPHA] = 2, rs[D3DRS_DESTBLENDALPHA] = 1, rs[D3DRS_BLENDOPALPHA] = 1;
    memset(d3d9.tss, 0, sizeof d3d9.tss);
    memset(d3d9.ss, 0, sizeof d3d9.ss);
    for (int i = 0; i < 8; i++) {
        uint32_t *t = d3d9.tss[i], *s = d3d9.ss[i];
        t[D3DTSS_COLOROP] = i ? D3DTOP_DISABLE : D3DTOP_MODULATE, t[D3DTSS_ALPHAOP] = i ? D3DTOP_DISABLE : D3DTOP_SELECTARG1;
        t[D3DTSS_COLORARG1] = t[D3DTSS_ALPHAARG1] = D3DTA_TEXTURE, t[D3DTSS_COLORARG2] = t[D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
        t[D3DTSS_TEXCOORDINDEX] = (uint32_t)i, t[D3DTSS_COLORARG0] = t[D3DTSS_ALPHAARG0] = t[D3DTSS_RESULTARG] = D3DTA_CURRENT;
        s[D3DSAMP_ADDRESSU] = s[D3DSAMP_ADDRESSV] = s[D3DSAMP_ADDRESSW] = D3DTADDRESS_WRAP;
        s[D3DSAMP_MAGFILTER] = s[D3DSAMP_MINFILTER] = D3DTEXF_POINT, s[D3DSAMP_MAXANISOTROPY] = 1;
    }
    identity(d3d9.world), identity(d3d9.view), identity(d3d9.proj);
    memset(d3d9.material, 0, sizeof d3d9.material);
    memset(d3d9.light, 0, sizeof d3d9.light);
    memset(d3d9.vsc, 0, sizeof d3d9.vsc);
    memset(d3d9.tex, 0, sizeof d3d9.tex);
    d3d9.fvf = 0, d3d9.vs = NULL, d3d9.decl = NULL, d3d9.vb = d3d9.ib = NULL;
}
static void target_set(d3d9_surface *s) {      /* the viewport follows what is drawn into */
    d3d9.rt = s;
    d3d9.vp.x = d3d9.vp.y = 0, d3d9.vp.w = (uint32_t)s->w, d3d9.vp.h = (uint32_t)s->h, d3d9.vp.minz = 0.0f, d3d9.vp.maxz = 1.0f;
    d3d9.scissor[0] = d3d9.scissor[1] = 0, d3d9.scissor[2] = s->w, d3d9.scissor[3] = s->h;
}

/* D3DPRESENT_PARAMETERS: width, height, format, count, multisample type and
 * quality, swap effect, window, windowed, auto depth, its format, flags,
 * refresh rate, interval. */
static uint32_t swap_setup(uint32_t pp) {
    int w = (int)MEM32(pp), h = (int)MEM32(pp + 4);
    if (w <= 0 || h <= 0) w = g_desk_w, h = g_desk_h;
    MEM32(pp) = (uint32_t)w, MEM32(pp + 4) = (uint32_t)h;
    if (!MEM32(pp + 8)) MEM32(pp + 8) = D3DFMT_X8R8G8B8;
    if (!MEM32(pp + 12)) MEM32(pp + 12) = 1;
    if (!d3d9.open) { if (d3d9_gl_open(w, h, hle_d3d9_headless) != 0) return D3DERR_NOTAVAILABLE; d3d9.open = 1; }
    else d3d9_gl_reset(w, h);
    d3d9.w = w, d3d9.h = h;
    if (!d3d9.back) d3d9.back = surface_new(D3D9_SURF_BACK, w, h, D3DFMT_X8R8G8B8, D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT);
    if (!d3d9.back_ds) d3d9.back_ds = surface_new(D3D9_SURF_DEPTH, w, h, D3DFMT_D16, D3DUSAGE_DEPTHSTENCIL, D3DPOOL_DEFAULT);
    if (!d3d9.back || !d3d9.back_ds) return D3DERR_OUTOFVIDEOMEMORY;
    d3d9.back->w = d3d9.back_ds->w = w, d3d9.back->h = d3d9.back_ds->h = h;
    if (MEM32(pp + 36) && MEM32(pp + 40)) d3d9.back_ds->fmt = (int)MEM32(pp + 40);
    state_defaults((int)MEM32(pp + 36));
    target_set(d3d9.back);
    d3d9.ds = MEM32(pp + 36) ? d3d9.back_ds : NULL;
    fprintf(stderr, "[d3d9] back buffer %dx%d, %s, depth %s\n", w, h, MEM32(pp + 32) ? "windowed" : "full screen",
            MEM32(pp + 36) ? "yes" : "no");
    return D3D_OK;
}

/* ---- IDirect3DDevice9 ------------------------------------------------------------------ */
static const uint8_t IID_IUnknown_[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46 };
static const uint8_t IID_Device9[16] = { 0x96, 0x3B, 0x22, 0xD0, 0x7A, 0xBF, 0xFD, 0x43, 0x92, 0xBD, 0xA4, 0x3B, 0x0D, 0x82, 0xB9, 0xEB };
static void dv_QueryInterface(void) {          /* (this, iid, &out) */
    const uint8_t *iid = G2H(A32(1));
    if (A32(2)) MEM32(A32(2)) = 0;
    if (!A32(2) || (memcmp(iid, IID_IUnknown_, 16) && memcmp(iid, IID_Device9, 16))) RET(E_NOINTERFACE_, 3);
    MEM32(A32(2)) = A32(0), COUNT(A32(0))++;
    RET(D3D_OK, 3);
}
static void dv_AddRef(void)  { RET(++COUNT(A32(0)), 1); }
static void dv_Release(void) { RET(COUNT(A32(0)) > 1 ? --COUNT(A32(0)) : 0, 1); }       /* the one device lives on */
static void dv_TestCooperativeLevel(void) { RET(D3D_OK, 1); }
static void dv_GetAvailableTextureMem(void) { RET(512u << 20, 1); }
static void dv_EvictManagedResources(void) { RET(D3D_OK, 1); }
static void dv_GetDirect3D(void) { if (A32(1)) MEM32(A32(1)) = g_d3d; COUNT(g_d3d)++; RET(D3D_OK, 2); }

/* D3DCAPS9 as a current card answers it, with no multisampling. */
static void caps_write(uint32_t p) {
    static const struct { int at; uint32_t v; } c[] = {
        { 0, 1 }, { 8, 0x00020000 }, { 12, 0x60020000 }, { 16, 0x000003A0 }, { 20, 0x80000001 }, { 24, 1 },
        { 28, 0x001BBEF0 }, { 32, 0x003FCEF2 }, { 36, 0x07732191 }, { 40, 0xFF }, { 44, 0x3FFF }, { 48, 0x3FFF },
        { 52, 0xFF }, { 56, 0x00084208 }, { 60, 0x0001ECC5 }, { 64, 0x07030700 }, { 68, 0x03030300 },
        { 72, 0x03030300 }, { 76, 0x3F }, { 80, 0x3F }, { 84, 0x1F }, { 96, 256 }, { 100, 8192 }, { 104, 8192 },
        { 108, 16 }, { 136, 0x1FF }, { 140, 0x00180008 }, { 144, 0x03FEFFFF }, { 148, 8 }, { 152, 8 },
        { 156, 0x0000017B }, { 160, 8 }, { 164, 6 }, { 168, 4 }, { 180, 0x00555555 }, { 184, 0x00FFFFFF },
        { 188, 16 }, { 192, 255 }, { 196, 0xFFFE0300 }, { 200, 256 }, { 204, 0xFFFF0000 }, { 212, 0x51 },
        { 232, 1 }, { 236, 0x30F }, { 240, 1 }, { 244, 0x03000300 }, { 248, 1 }, { 252, 24 }, { 256, 32 },
        { 260, 4 }, { 284, 0x03030700 }, { 288, 65535 }, { 292, 65535 }, { 296, 4096 }, { 300, 0 },
    };
    memset(G2H(p), 0, 304);
    for (size_t i = 0; i < sizeof c / sizeof c[0]; i++) MEM32(p + (uint32_t)c[i].at) = c[i].v;
    int max = d3d9.open ? d3d9_gl_max_texture() : 4096;
    MEM32(p + 88) = MEM32(p + 92) = (uint32_t)(max > 4096 ? 4096 : max);
    MEM32(p + 112) = fbits(1e10f);                                           /* the largest w */
    MEM32(p + 116) = MEM32(p + 120) = fbits(-32768.0f), MEM32(p + 124) = MEM32(p + 128) = fbits(32768.0f);
    MEM32(p + 176) = fbits(256.0f), MEM32(p + 208) = fbits(0.0f);
}
static void dv_GetDeviceCaps(void) { if (!A32(1)) RET(D3DERR_INVALIDCALL, 2); caps_write(A32(1)); RET(D3D_OK, 2); }
static void mode_write(uint32_t p, int w, int h) { MEM32(p) = (uint32_t)w, MEM32(p + 4) = (uint32_t)h, MEM32(p + 8) = 60, MEM32(p + 12) = D3DFMT_X8R8G8B8; }
static void dv_GetDisplayMode(void) { if (A32(2)) mode_write(A32(2), g_desk_w, g_desk_h); RET(D3D_OK, 3); }
static void dv_GetCreationParameters(void) {
    uint32_t p = A32(1);
    if (p) MEM32(p) = 0, MEM32(p + 4) = 1, MEM32(p + 8) = d3d9.window, MEM32(p + 12) = 0x40;   /* hardware vertex processing */
    RET(D3D_OK, 2);
}
static void dv_SetCursorProperties(void) { RET(D3D_OK, 4); }
static void dv_SetCursorPosition(void) { RETV(4); }
static void dv_ShowCursor(void) { RET(0, 2); }
static void dv_GetNumberOfSwapChains(void) { RET(1, 1); }
static void dv_Reset(void) { RET(swap_setup(A32(1)), 2); }
static void dv_Present(void) {                 /* (this, from, to, window, dirty) */
    d3d9_gl_present();
    d3d9.frame++;
    if (hle_d3d9_frame_hook) hle_d3d9_frame_hook(d3d9.frame);
    RET(D3D_OK, 5);
}
static void dv_GetBackBuffer(void) {           /* (this, swap chain, number, type, &surface) */
    if (!A32(4) || A32(2)) RET(D3DERR_INVALIDCALL, 5);
    MEM32(A32(4)) = d3d9.back->self, surface_addref(d3d9.back);
    RET(D3D_OK, 5);
}
static void dv_GetRasterStatus(void) { if (A32(2)) MEM32(A32(2)) = 0, MEM32(A32(2) + 4) = 0; RET(D3D_OK, 3); }
static void dv_SetDialogBoxMode(void) { RET(D3D_OK, 2); }
static void dv_SetGammaRamp(void) { RETV(4); }
static void dv_GetGammaRamp(void) {            /* (this, swap chain, &ramp): three tables of 256 words */
    if (A32(2)) for (int c = 0; c < 3; c++) for (int i = 0; i < 256; i++) MEM16(A32(2) + (uint32_t)(c * 512 + i * 2)) = (uint16_t)(i * 257);
    RETV(3);
}
static void dv_CreateTexture(void) {           /* (this, w, h, levels, usage, format, pool, &texture, shared) */
    if (!A32(7)) RET(D3DERR_INVALIDCALL, 9);
    uint32_t t = d3d9_texture_new((int)A32(1), (int)A32(2), (int)A32(3), (int)A32(4), (int)A32(5), (int)A32(6));
    MEM32(A32(7)) = t;
    if (!t) { fprintf(stderr, "[d3d9] CreateTexture %ux%u format %u refused\n", A32(1), A32(2), A32(5)); RET(D3DERR_INVALIDCALL, 9); }
    RET(D3D_OK, 9);
}
static void dv_CreateVertexBuffer(void) {      /* (this, bytes, usage, fvf, pool, &buffer, shared) */
    if (!A32(5)) RET(D3DERR_INVALIDCALL, 7);
    RET((MEM32(A32(5)) = buffer_new(0, (int)A32(1), (int)A32(2), (int)A32(3), (int)A32(4))) ? D3D_OK : D3DERR_OUTOFVIDEOMEMORY, 7);
}
static void dv_CreateIndexBuffer(void) {       /* (this, bytes, usage, format, pool, &buffer, shared) */
    if (!A32(5)) RET(D3DERR_INVALIDCALL, 7);
    RET((MEM32(A32(5)) = buffer_new(1, (int)A32(1), (int)A32(2), (int)A32(3), (int)A32(4))) ? D3D_OK : D3DERR_OUTOFVIDEOMEMORY, 7);
}
static uint32_t lone_surface(int kind, int w, int h, int fmt, int usage, int pool, uint32_t out) {
    if (!out) return D3DERR_INVALIDCALL;
    MEM32(out) = 0;
    if (w <= 0 || h <= 0 || (kind == D3D9_SURF_DEPTH ? !is_depth(fmt) : !d3d9_fmt_bytes(fmt))) return D3DERR_INVALIDCALL;
    d3d9_surface *s = surface_new(kind, w, h, fmt, usage, pool);
    if (!s) return D3DERR_OUTOFVIDEOMEMORY;
    MEM32(out) = s->self;
    return D3D_OK;
}
/* (this, w, h, format, multisample, quality, lockable or discard, &surface, shared) */
static void dv_CreateRenderTarget(void) { RET(lone_surface(D3D9_SURF_TARGET, (int)A32(1), (int)A32(2), (int)A32(3), D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT, A32(7)), 9); }
static void dv_CreateDepthStencilSurface(void) { RET(lone_surface(D3D9_SURF_DEPTH, (int)A32(1), (int)A32(2), (int)A32(3), D3DUSAGE_DEPTHSTENCIL, D3DPOOL_DEFAULT, A32(7)), 9); }
/* (this, w, h, format, pool, &surface, shared) */
static void dv_CreateOffscreenPlainSurface(void) { RET(lone_surface(D3D9_SURF_PLAIN, (int)A32(1), (int)A32(2), (int)A32(3), 0, (int)A32(4), A32(5)), 7); }

static void rect_of(uint32_t p, const d3d9_surface *s, int *r) {
    if (p) r[0] = (int)MEM32(p), r[1] = (int)MEM32(p + 4), r[2] = (int)MEM32(p + 8), r[3] = (int)MEM32(p + 12);
    else r[0] = r[1] = 0, r[2] = s->w, r[3] = s->h;
}
/* A copy between two surfaces through memory: any kind to any kind, scaled by
 * the nearest pixel if the sizes differ. */
static void copy_rect(d3d9_surface *from, const int *fr, d3d9_surface *to, const int *tr) {
    int fw = fr[2] - fr[0], fh = fr[3] - fr[1], tw = tr[2] - tr[0], th = tr[3] - tr[1];
    if (fw <= 0 || fh <= 0 || tw <= 0 || th <= 0) return;
    uint8_t *px = d3d9_surface_read(from, fr[0], fr[1], fw, fh);
    if (px && (fw != tw || fh != th)) {
        uint8_t *sc = malloc((size_t)tw * (size_t)th * 4);
        for (int y = 0; sc && y < th; y++)
            for (int x = 0; x < tw; x++)
                memcpy(sc + ((size_t)y * (size_t)tw + (size_t)x) * 4, px + ((size_t)(y * fh / th) * (size_t)fw + (size_t)(x * fw / tw)) * 4, 4);
        free(px), px = sc;
    }
    if (px) d3d9_surface_write(to, tr[0], tr[1], tw, th, px);
    free(px);
}
static void dv_StretchRect(void) {             /* (this, from, its rect, to, its rect, filter) */
    d3d9_surface *from = d3d9_surface_of(A32(1)), *to = d3d9_surface_of(A32(3));
    int fr[4], tr[4];
    if (!from || !to) RET(D3DERR_INVALIDCALL, 6);
    rect_of(A32(2), from, fr), rect_of(A32(4), to, tr);
    if (on_gl(from) && on_gl(to)) d3d9_gl_blit(from, fr, to, tr, A32(5) >= D3DTEXF_LINEAR);
    else copy_rect(from, fr, to, tr);
    RET(D3D_OK, 6);
}
static void dv_UpdateSurface(void) {           /* (this, from, its rect, to, its point) */
    d3d9_surface *from = d3d9_surface_of(A32(1)), *to = d3d9_surface_of(A32(3));
    int fr[4], tr[4];
    if (!from || !to) RET(D3DERR_INVALIDCALL, 5);
    rect_of(A32(2), from, fr);
    tr[0] = A32(4) ? (int)MEM32(A32(4)) : 0, tr[1] = A32(4) ? (int)MEM32(A32(4) + 4) : 0;
    tr[2] = tr[0] + fr[2] - fr[0], tr[3] = tr[1] + fr[3] - fr[1];
    copy_rect(from, fr, to, tr);
    RET(D3D_OK, 5);
}
static void dv_UpdateTexture(void) {           /* (this, from, to) */
    d3d9_texture *from = d3d9_texture_of(A32(1)), *to = d3d9_texture_of(A32(2));
    if (!from || !to) RET(D3DERR_INVALIDCALL, 3);
    for (int i = 0; i < from->levels && i < to->levels; i++) {
        int fr[4] = { 0, 0, from->level[i]->w, from->level[i]->h }, tr[4] = { 0, 0, to->level[i]->w, to->level[i]->h };
        copy_rect(from->level[i], fr, to->level[i], tr);
    }
    RET(D3D_OK, 3);
}
static void dv_GetRenderTargetData(void) {     /* (this, target, to) */
    d3d9_surface *from = d3d9_surface_of(A32(1)), *to = d3d9_surface_of(A32(2));
    if (!from || !to) RET(D3DERR_INVALIDCALL, 3);
    int fr[4] = { 0, 0, from->w, from->h }, tr[4] = { 0, 0, to->w, to->h };
    copy_rect(from, fr, to, tr);
    RET(D3D_OK, 3);
}
static void dv_GetFrontBufferData(void) {      /* (this, swap chain, to): what was last presented is still the back buffer's */
    d3d9_surface *to = d3d9_surface_of(A32(2));
    if (!to) RET(D3DERR_INVALIDCALL, 3);
    int fr[4] = { 0, 0, d3d9.back->w, d3d9.back->h }, tr[4] = { 0, 0, to->w, to->h };
    copy_rect(d3d9.back, fr, to, tr);
    RET(D3D_OK, 3);
}
static void dv_ColorFill(void) {               /* (this, surface, rect, colour) */
    d3d9_surface *s = d3d9_surface_of(A32(1));
    int r[4];
    if (!s) RET(D3DERR_INVALIDCALL, 4);
    rect_of(A32(2), s, r);
    int w = r[2] - r[0], h = r[3] - r[1];
    uint8_t *px = w > 0 && h > 0 ? malloc((size_t)w * (size_t)h * 4) : NULL;
    uint32_t c = A32(3);
    for (int i = 0; px && i < w * h; i++) memcpy(px + 4 * i, &c, 4);     /* a D3DCOLOR is B, G, R, A in memory */
    if (px) d3d9_surface_write(s, r[0], r[1], w, h, px);
    free(px);
    RET(D3D_OK, 4);
}
static void dv_SetRenderTarget(void) {         /* (this, number, surface) */
    d3d9_surface *s = d3d9_surface_of(A32(2));
    if (A32(1) || !s) RET(D3DERR_INVALIDCALL, 3);
    if (hle_d3d9_log) fprintf(stderr, "[d3d9] target: %s, %dx%d\n", s == d3d9.back ? "the back buffer" : "a texture", s->w, s->h);
    target_set(s);
    RET(D3D_OK, 3);
}
static void dv_GetRenderTarget(void) {
    if (A32(1) || !A32(2)) RET(D3DERR_INVALIDCALL, 3);
    MEM32(A32(2)) = d3d9.rt->self, surface_addref(d3d9.rt);
    RET(D3D_OK, 3);
}
static void dv_SetDepthStencilSurface(void) { d3d9.ds = d3d9_surface_of(A32(1)); RET(D3D_OK, 2); }
static void dv_GetDepthStencilSurface(void) {
    if (!A32(1)) RET(D3DERR_INVALIDCALL, 2);
    MEM32(A32(1)) = d3d9.ds ? d3d9.ds->self : 0;
    if (!d3d9.ds) RET(0x88760866u, 2);                                   /* D3DERR_NOTFOUND */
    surface_addref(d3d9.ds);
    RET(D3D_OK, 2);
}
static void dv_BeginScene(void) { RET(D3D_OK, 1); }
static void dv_EndScene(void) { RET(D3D_OK, 1); }
static void dv_Clear(void) {                   /* (this, count, rects, flags, colour, z, stencil) */
    if (A32(1)) once("Clear with rectangles (the whole viewport is cleared)");
    d3d9_gl_clear(A32(3), A32(4), AF(5), A32(6));
    RET(D3D_OK, 7);
}
static float *transform_of(uint32_t which) {
    return which == D3DTS_WORLD ? d3d9.world : which == D3DTS_VIEW ? d3d9.view : which == D3DTS_PROJECTION ? d3d9.proj : NULL;
}
static void dv_SetTransform(void) {            /* (this, which, &matrix) */
    float *m = transform_of(A32(1));
    if (m && A32(2)) memcpy(m, G2H(A32(2)), 64);
    else if (!m) once("a transform other than world, view and projection");
    RET(D3D_OK, 3);
}
static void dv_GetTransform(void) {
    float *m = transform_of(A32(1)), id[16];
    if (!m) identity(id), m = id;
    if (A32(2)) memcpy(G2H(A32(2)), m, 64);
    RET(D3D_OK, 3);
}
static void dv_MultiplyTransform(void) {       /* (this, which, &matrix): the given one goes first */
    float *m = transform_of(A32(1)), a[16], r[16];
    if (m && A32(2)) {
        memcpy(a, G2H(A32(2)), 64);
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                r[i * 4 + j] = a[i * 4] * m[j] + a[i * 4 + 1] * m[4 + j] + a[i * 4 + 2] * m[8 + j] + a[i * 4 + 3] * m[12 + j];
        memcpy(m, r, 64);
    }
    RET(D3D_OK, 3);
}
static void dv_SetViewport(void) {
    if (A32(1)) memcpy(&d3d9.vp, G2H(A32(1)), 24);
    if (hle_d3d9_log)
        fprintf(stderr, "[d3d9] viewport: %ux%u at %u,%u, depth %g to %g (under sub_%08X)\n", d3d9.vp.w, d3d9.vp.h, d3d9.vp.x, d3d9.vp.y,
                (double)d3d9.vp.minz, (double)d3d9.vp.maxz, g_cur_func);
    RET(D3D_OK, 2);
}
static void dv_GetViewport(void) { if (A32(1)) memcpy(G2H(A32(1)), &d3d9.vp, 24); RET(D3D_OK, 2); }
static void dv_SetMaterial(void) { if (A32(1)) memcpy(d3d9.material, G2H(A32(1)), 68); RET(D3D_OK, 2); }
static void dv_GetMaterial(void) { if (A32(1)) memcpy(G2H(A32(1)), d3d9.material, 68); RET(D3D_OK, 2); }
static void dv_SetLight(void) {                /* (this, number, &light) */
    if (A32(1) >= 8) { once("more than eight lights"); RET(D3D_OK, 3); }
    if (A32(2)) memcpy(&d3d9.light[A32(1)], G2H(A32(2)), 104);
    RET(D3D_OK, 3);
}
static void dv_GetLight(void) {
    if (A32(1) >= 8 || !A32(2)) RET(D3DERR_INVALIDCALL, 3);
    memcpy(G2H(A32(2)), &d3d9.light[A32(1)], 104);
    RET(D3D_OK, 3);
}
static void dv_LightEnable(void) { if (A32(1) < 8) d3d9.light[A32(1)].enabled = A32(2) != 0; RET(D3D_OK, 3); }
static void dv_GetLightEnable(void) { if (A32(1) < 8 && A32(2)) MEM32(A32(2)) = (uint32_t)d3d9.light[A32(1)].enabled; RET(D3D_OK, 3); }
static void dv_SetClipPlane(void) { once("clip planes"); RET(D3D_OK, 3); }
static void dv_SetRenderState(void) { if (A32(1) < D3DRS_COUNT) d3d9.rs[A32(1)] = A32(2); RET(D3D_OK, 3); }
static void dv_GetRenderState(void) {
    if (A32(1) >= D3DRS_COUNT || !A32(2)) RET(D3DERR_INVALIDCALL, 3);
    MEM32(A32(2)) = d3d9.rs[A32(1)];
    RET(D3D_OK, 3);
}
static void dv_GetTexture(void) {              /* (this, stage, &texture) */
    if (A32(1) >= 8 || !A32(2)) RET(D3DERR_INVALIDCALL, 3);
    d3d9_texture *t = d3d9.tex[A32(1)];
    MEM32(A32(2)) = t ? t->self : 0;
    if (t) COUNT(t->self)++;
    RET(D3D_OK, 3);
}
static void dv_SetTexture(void) { if (A32(1) < 8) d3d9.tex[A32(1)] = d3d9_texture_of(A32(2)); RET(D3D_OK, 3); }
static void dv_GetTextureStageState(void) {    /* (this, stage, which, &value) */
    if (A32(1) >= 8 || A32(2) >= D3DTSS_COUNT || !A32(3)) RET(D3DERR_INVALIDCALL, 4);
    MEM32(A32(3)) = d3d9.tss[A32(1)][A32(2)];
    RET(D3D_OK, 4);
}
static void dv_SetTextureStageState(void) { if (A32(1) < 8 && A32(2) < D3DTSS_COUNT) d3d9.tss[A32(1)][A32(2)] = A32(3); RET(D3D_OK, 4); }
static void dv_GetSamplerState(void) {
    if (A32(1) >= 8 || A32(2) >= D3DSAMP_COUNT || !A32(3)) RET(D3DERR_INVALIDCALL, 4);
    MEM32(A32(3)) = d3d9.ss[A32(1)][A32(2)];
    RET(D3D_OK, 4);
}
static void dv_SetSamplerState(void) { if (A32(1) < 8 && A32(2) < D3DSAMP_COUNT) d3d9.ss[A32(1)][A32(2)] = A32(3); RET(D3D_OK, 4); }
static void dv_ValidateDevice(void) { if (A32(1)) MEM32(A32(1)) = 1; RET(D3D_OK, 2); }
static void dv_SetScissorRect(void) { if (A32(1)) for (int i = 0; i < 4; i++) d3d9.scissor[i] = (int)MEM32(A32(1) + 4u * (uint32_t)i); RET(D3D_OK, 2); }
static void dv_GetScissorRect(void) { if (A32(1)) for (int i = 0; i < 4; i++) MEM32(A32(1) + 4u * (uint32_t)i) = (uint32_t)d3d9.scissor[i]; RET(D3D_OK, 2); }
static void dv_SetSoftwareVertexProcessing(void) { RET(D3D_OK, 2); }
static void dv_GetSoftwareVertexProcessing(void) { RET(0, 1); }
static void dv_SetNPatchMode(void) { RET(D3D_OK, 2); }

static int prim_vertices(int prim, int n) {
    switch (prim) {
    case D3DPT_POINTLIST: return n;
    case D3DPT_LINELIST: return n * 2;
    case D3DPT_LINESTRIP: return n + 1;
    case D3DPT_TRIANGLELIST: return n * 3;
    case D3DPT_TRIANGLESTRIP: case D3DPT_TRIANGLEFAN: return n + 2;
    }
    return 0;
}
static void dv_DrawPrimitive(void) {           /* (this, kind, first vertex, primitives) */
    d3d9_draw d = { .prim = (int)A32(1), .nprims = (int)A32(3), .first = (int)A32(2) };
    d.num_vertices = prim_vertices(d.prim, d.nprims);
    if (d.nprims > 0) d3d9_gl_draw(&d);
    RET(D3D_OK, 4);
}
static void dv_DrawIndexedPrimitive(void) {    /* (this, kind, base, lowest vertex, vertices, first index, primitives) */
    d3d9_draw d = { .prim = (int)A32(1), .nprims = (int)A32(6), .indexed = 1, .base = (int)A32(2),
                    .min_vertex = (int)A32(3), .num_vertices = (int)A32(4), .first = (int)A32(5) };
    if (d.nprims > 0) d3d9_gl_draw(&d);
    RET(D3D_OK, 7);
}
static void dv_DrawPrimitiveUP(void) {         /* (this, kind, primitives, vertices, stride) */
    d3d9_draw d = { .prim = (int)A32(1), .nprims = (int)A32(2), .user_verts = G2H(A32(3)), .user_stride = (int)A32(4) };
    d.num_vertices = prim_vertices(d.prim, d.nprims);
    if (d.nprims > 0 && A32(3)) d3d9_gl_draw(&d);
    d3d9.vb = NULL, d3d9.vb_stride = 0;                                  /* as Direct3D leaves the stream */
    RET(D3D_OK, 5);
}
static void dv_DrawIndexedPrimitiveUP(void) {  /* (this, kind, lowest vertex, vertices, primitives, indices, their format, vertices, stride) */
    d3d9_draw d = { .prim = (int)A32(1), .nprims = (int)A32(4), .indexed = 1, .min_vertex = (int)A32(2),
                    .num_vertices = (int)A32(3), .user_index = G2H(A32(5)), .index_bytes = A32(6) == D3DFMT_INDEX32 ? 4 : 2,
                    .user_verts = G2H(A32(7)), .user_stride = (int)A32(8) };
    if (d.nprims > 0 && A32(5) && A32(7)) d3d9_gl_draw(&d);
    d3d9.vb = NULL, d3d9.vb_stride = 0, d3d9.ib = NULL;
    RET(D3D_OK, 9);
}
static void dv_CreateVertexDeclaration(void) { /* (this, elements, &declaration) */
    if (!A32(1) || !A32(2)) RET(D3DERR_INVALIDCALL, 3);
    d3d9_vdecl *d = calloc(1, sizeof *d);
    const uint8_t *e = G2H(A32(1));
    for (; d && d->n < 32 && e[0] != 0xFF; e += 8, d->n++) memcpy(&d->el[d->n], e, 8);
    if (d) d->id = g_next_id++, d->self = object_new(g_vt_decl, d);
    if (!d || !d->self) { free(d); RET(D3DERR_OUTOFVIDEOMEMORY, 3); }
    MEM32(A32(2)) = d->self;
    RET(D3D_OK, 3);
}
static void dv_SetVertexDeclaration(void) {
    d3d9.decl = A32(1) && MEM32(A32(1)) == g_vt_decl ? HOSTP(A32(1)) : NULL;
    if (d3d9.decl) d3d9.fvf = 0;
    RET(D3D_OK, 2);
}
static void dv_GetVertexDeclaration(void) {
    if (!A32(1)) RET(D3DERR_INVALIDCALL, 2);
    MEM32(A32(1)) = d3d9.decl ? d3d9.decl->self : 0;
    if (d3d9.decl) COUNT(d3d9.decl->self)++;
    RET(D3D_OK, 2);
}
static void dv_SetFVF(void) { d3d9.fvf = A32(1); if (d3d9.fvf) d3d9.decl = NULL; RET(D3D_OK, 2); }
static void dv_GetFVF(void) { if (A32(1)) MEM32(A32(1)) = d3d9.fvf; RET(D3D_OK, 2); }
static void dv_CreateVertexShader(void) {      /* (this, tokens, &shader) */
    if (!A32(1) || !A32(2)) RET(D3DERR_INVALIDCALL, 3);
    const uint32_t *tok = G2H(A32(1));
    size_t n = d3d9_vs_tokens(tok);
    d3d9_vshader *v = n ? calloc(1, sizeof *v) : NULL;
    if (!v) RET(D3DERR_INVALIDCALL, 3);
    v->tokens = malloc(n * 4), v->ntokens = n, v->id = g_next_id++;
    memcpy(v->tokens, tok, n * 4);
    v->glsl = d3d9_vs_glsl(v->tokens, &v->info);
    if (!v->glsl) fprintf(stderr, "[d3d9] vertex shader %d could not be translated: what it draws will be missing\n", v->id);
    v->self = object_new(g_vt_vs, v);
    MEM32(A32(2)) = v->self;
    RET(D3D_OK, 3);
}
static void dv_SetVertexShader(void) { d3d9.vs = A32(1) && MEM32(A32(1)) == g_vt_vs ? HOSTP(A32(1)) : NULL; RET(D3D_OK, 2); }
static void dv_GetVertexShader(void) {
    if (!A32(1)) RET(D3DERR_INVALIDCALL, 2);
    MEM32(A32(1)) = d3d9.vs ? d3d9.vs->self : 0;
    if (d3d9.vs) COUNT(d3d9.vs->self)++;
    RET(D3D_OK, 2);
}
static void dv_SetVertexShaderConstantF(void) { /* (this, first register, floats, registers) */
    if (A32(1) + A32(3) > 256 || !A32(2)) RET(D3DERR_INVALIDCALL, 4);
    memcpy(d3d9.vsc[A32(1)], G2H(A32(2)), 16u * (size_t)A32(3));
    d3d9.vsc_serial++;
    RET(D3D_OK, 4);
}
static void dv_GetVertexShaderConstantF(void) {
    if (A32(1) + A32(3) > 256 || !A32(2)) RET(D3DERR_INVALIDCALL, 4);
    memcpy(G2H(A32(2)), d3d9.vsc[A32(1)], 16u * (size_t)A32(3));
    RET(D3D_OK, 4);
}
static void dv_SetStreamSource(void) {         /* (this, stream, buffer, offset, stride) */
    if (A32(1)) { once("a vertex stream other than the first"); RET(D3D_OK, 5); }
    d3d9.vb = A32(2) && MEM32(A32(2)) == g_vt_vb ? HOSTP(A32(2)) : NULL;
    d3d9.vb_offset = A32(3), d3d9.vb_stride = A32(4);
    RET(D3D_OK, 5);
}
static void dv_GetStreamSource(void) {         /* (this, stream, &buffer, &offset, &stride) */
    if (A32(2)) { MEM32(A32(2)) = !A32(1) && d3d9.vb ? d3d9.vb->self : 0; if (!A32(1) && d3d9.vb) COUNT(d3d9.vb->self)++; }
    if (A32(3)) MEM32(A32(3)) = A32(1) ? 0 : d3d9.vb_offset;
    if (A32(4)) MEM32(A32(4)) = A32(1) ? 0 : d3d9.vb_stride;
    RET(D3D_OK, 5);
}
static void dv_SetIndices(void) { d3d9.ib = A32(1) && MEM32(A32(1)) == g_vt_ib ? HOSTP(A32(1)) : NULL; RET(D3D_OK, 2); }
static void dv_GetIndices(void) {
    if (!A32(1)) RET(D3DERR_INVALIDCALL, 2);
    MEM32(A32(1)) = d3d9.ib ? d3d9.ib->self : 0;
    if (d3d9.ib) COUNT(d3d9.ib->self)++;
    RET(D3D_OK, 2);
}
static void dv_CreatePixelShader(void) { once("pixel shaders"); if (A32(2)) MEM32(A32(2)) = 0; RET(D3DERR_INVALIDCALL, 3); }
static void dv_SetPixelShader(void) { if (A32(1)) once("pixel shaders"); RET(D3D_OK, 2); }
static void dv_GetPixelShader(void) { if (A32(1)) MEM32(A32(1)) = 0; RET(D3D_OK, 2); }
static void dv_CreateQuery(void) { if (A32(2)) MEM32(A32(2)) = 0; RET(D3DERR_NOTAVAILABLE, 3); }

/* ---- IDirect3D9 ------------------------------------------------------------------------ */
static const struct { int w, h; } g_modes[] = { { 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1280, 720 }, { 1366, 768 },
                                                { 1600, 900 }, { 1920, 1080 } };
#define N_MODES (int)(sizeof g_modes / sizeof g_modes[0])
static void d3_Release(void) { RET(COUNT(A32(0)) > 1 ? --COUNT(A32(0)) : 0, 1); }
static void d3_RegisterSoftwareDevice(void) { RET(D3D_OK, 2); }
static void d3_GetAdapterCount(void) { RET(1, 1); }
static void d3_GetAdapterIdentifier(void) {    /* (this, adapter, flags, &identifier) */
    uint32_t p = A32(3);
    if (A32(1) || !p) RET(D3DERR_INVALIDCALL, 4);
    memset(G2H(p), 0, 1100);
    strcpy(G2H(p), "win32hle");                                           /* the driver's file */
    strcpy(G2H(p + 512), "OpenGL ES 3");                                  /* what the card is called */
    strcpy(G2H(p + 1024), "\\\\.\\DISPLAY1");
    RET(D3D_OK, 4);
}
static void d3_GetAdapterModeCount(void) { RET(A32(1) || A32(2) != D3DFMT_X8R8G8B8 ? 0 : N_MODES, 3); }
static void d3_EnumAdapterModes(void) {        /* (this, adapter, format, number, &mode) */
    if (A32(1) || A32(2) != D3DFMT_X8R8G8B8 || A32(3) >= (uint32_t)N_MODES || !A32(4)) RET(D3DERR_INVALIDCALL, 5);
    mode_write(A32(4), g_modes[A32(3)].w, g_modes[A32(3)].h);
    RET(D3D_OK, 5);
}
static void d3_GetAdapterDisplayMode(void) { if (A32(2)) mode_write(A32(2), g_desk_w, g_desk_h); RET(D3D_OK, 3); }
/* Only the hardware device is offered: a game that falls back to the
 * reference rasteriser would be choosing something this is not. */
static void d3_CheckDeviceType(void) { RET(A32(1) || A32(2) != 1 ? D3DERR_NOTAVAILABLE : D3D_OK, 6); }
static void d3_CheckDeviceFormat(void) {       /* (this, adapter, device, adapter format, usage, kind, format) */
    int fmt = (int)A32(6), ok = A32(2) == 1;
    if (A32(4) & D3DUSAGE_DEPTHSTENCIL) ok = ok && is_depth(fmt);
    else if (A32(4) & D3DUSAGE_RENDERTARGET) ok = ok && (fmt == D3DFMT_X8R8G8B8 || fmt == D3DFMT_A8R8G8B8);
    else ok = ok && A32(5) != 4 && A32(5) != 5 && d3d9_fmt_bytes(fmt);    /* no volume or cube textures */
    RET(ok ? D3D_OK : D3DERR_NOTAVAILABLE, 7);
}
static void d3_CheckDepthStencilMatch(void) { RET(A32(2) == 1 && is_depth((int)A32(5)) ? D3D_OK : D3DERR_NOTAVAILABLE, 6); }
static void d3_CheckDeviceMultiSampleType(void) {   /* (this, adapter, device, format, windowed, samples, &quality levels) */
    if (A32(2) != 1 || A32(5)) RET(D3DERR_NOTAVAILABLE, 7);
    if (A32(6)) MEM32(A32(6)) = 1;
    RET(D3D_OK, 7);
}
static void d3_CheckDeviceFormatConversion(void) { RET(D3D_OK, 5); }
static void d3_GetDeviceCaps(void) {           /* (this, adapter, device, &caps) */
    if (A32(2) != 1 || !A32(3)) RET(D3DERR_NOTAVAILABLE, 4);
    caps_write(A32(3));
    RET(D3D_OK, 4);
}
static void d3_GetAdapterMonitor(void) { RET(0x00010001u, 2); }
static void d3_CreateDevice(void) {            /* (this, adapter, device, window, behaviour, &parameters, &device) */
    if (!A32(5) || !A32(6)) RET(D3DERR_INVALIDCALL, 7);
    MEM32(A32(6)) = 0;
    if (A32(2) != 1) RET(D3DERR_NOTAVAILABLE, 7);
    d3d9.window = A32(3) ? A32(3) : MEM32(A32(5) + 28);
    uint32_t hr = swap_setup(A32(5));
    if (hr != D3D_OK) RET(hr, 7);
    COUNT(g_dev) = 1;
    MEM32(A32(6)) = g_dev;
    RET(D3D_OK, 7);
}

/* ---- the vtables ------------------------------------------------------------------------- */
typedef struct { const char *name; recomp_func_t fn; } method;
/* Every slot gets a shim: the methods above, and for the rest one that stops
 * the run with the method's name, since it cannot know how many arguments to
 * take off the stack. */
static uint32_t vtable(const char *iface, const method *m, int n) {
    uint32_t vt = hle_guest_alloc(4u * (uint32_t)n);
    for (int i = 0; i < n; i++) {
        size_t len = strlen(iface) + strlen(m[i].name) + 3;
        char *full = malloc(len);
        snprintf(full, len, "%s::%s", iface, m[i].name);
        if (m[i].fn && !hle_resolve(full)) {
            win32hle_shim *one = calloc(2, sizeof *one);                 /* the registry keeps the pointer */
            one[0].name = full, one[0].fn = m[i].fn;
            win32hle_register(one);
        }
        MEM32(vt + 4u * (uint32_t)i) = hle_resolve_or_stub(full);
    }
    return vt;
}
#define VT(iface, table) vtable(iface, table, (int)(sizeof table / sizeof table[0]))
#define RES_HEAD(release, type) \
    { "QueryInterface", t_QueryInterface }, { "AddRef", t_AddRef }, { "Release", release }, { "GetDevice", any_GetDevice }, \
    { "SetPrivateData", NULL }, { "GetPrivateData", NULL }, { "FreePrivateData", NULL }, { "SetPriority", any_SetPriority }, \
    { "GetPriority", any_GetPriority }, { "PreLoad", any_PreLoad }, { "GetType", type }

static void make_vtables(void) {
    static const method d3[] = {
        { "QueryInterface", t_QueryInterface }, { "AddRef", t_AddRef }, { "Release", d3_Release },
        { "RegisterSoftwareDevice", d3_RegisterSoftwareDevice }, { "GetAdapterCount", d3_GetAdapterCount },
        { "GetAdapterIdentifier", d3_GetAdapterIdentifier }, { "GetAdapterModeCount", d3_GetAdapterModeCount },
        { "EnumAdapterModes", d3_EnumAdapterModes }, { "GetAdapterDisplayMode", d3_GetAdapterDisplayMode },
        { "CheckDeviceType", d3_CheckDeviceType }, { "CheckDeviceFormat", d3_CheckDeviceFormat },
        { "CheckDeviceMultiSampleType", d3_CheckDeviceMultiSampleType }, { "CheckDepthStencilMatch", d3_CheckDepthStencilMatch },
        { "CheckDeviceFormatConversion", d3_CheckDeviceFormatConversion }, { "GetDeviceCaps", d3_GetDeviceCaps },
        { "GetAdapterMonitor", d3_GetAdapterMonitor }, { "CreateDevice", d3_CreateDevice },
    };
    static const method dv[] = {
        { "QueryInterface", dv_QueryInterface }, { "AddRef", dv_AddRef }, { "Release", dv_Release },
        { "TestCooperativeLevel", dv_TestCooperativeLevel }, { "GetAvailableTextureMem", dv_GetAvailableTextureMem },
        { "EvictManagedResources", dv_EvictManagedResources }, { "GetDirect3D", dv_GetDirect3D },
        { "GetDeviceCaps", dv_GetDeviceCaps }, { "GetDisplayMode", dv_GetDisplayMode },
        { "GetCreationParameters", dv_GetCreationParameters }, { "SetCursorProperties", dv_SetCursorProperties },
        { "SetCursorPosition", dv_SetCursorPosition }, { "ShowCursor", dv_ShowCursor },
        { "CreateAdditionalSwapChain", NULL }, { "GetSwapChain", NULL }, { "GetNumberOfSwapChains", dv_GetNumberOfSwapChains },
        { "Reset", dv_Reset }, { "Present", dv_Present }, { "GetBackBuffer", dv_GetBackBuffer },
        { "GetRasterStatus", dv_GetRasterStatus }, { "SetDialogBoxMode", dv_SetDialogBoxMode },
        { "SetGammaRamp", dv_SetGammaRamp }, { "GetGammaRamp", dv_GetGammaRamp }, { "CreateTexture", dv_CreateTexture },
        { "CreateVolumeTexture", NULL }, { "CreateCubeTexture", NULL }, { "CreateVertexBuffer", dv_CreateVertexBuffer },
        { "CreateIndexBuffer", dv_CreateIndexBuffer }, { "CreateRenderTarget", dv_CreateRenderTarget },
        { "CreateDepthStencilSurface", dv_CreateDepthStencilSurface }, { "UpdateSurface", dv_UpdateSurface },
        { "UpdateTexture", dv_UpdateTexture }, { "GetRenderTargetData", dv_GetRenderTargetData },
        { "GetFrontBufferData", dv_GetFrontBufferData }, { "StretchRect", dv_StretchRect }, { "ColorFill", dv_ColorFill },
        { "CreateOffscreenPlainSurface", dv_CreateOffscreenPlainSurface }, { "SetRenderTarget", dv_SetRenderTarget },
        { "GetRenderTarget", dv_GetRenderTarget }, { "SetDepthStencilSurface", dv_SetDepthStencilSurface },
        { "GetDepthStencilSurface", dv_GetDepthStencilSurface }, { "BeginScene", dv_BeginScene }, { "EndScene", dv_EndScene },
        { "Clear", dv_Clear }, { "SetTransform", dv_SetTransform }, { "GetTransform", dv_GetTransform },
        { "MultiplyTransform", dv_MultiplyTransform }, { "SetViewport", dv_SetViewport }, { "GetViewport", dv_GetViewport },
        { "SetMaterial", dv_SetMaterial }, { "GetMaterial", dv_GetMaterial }, { "SetLight", dv_SetLight },
        { "GetLight", dv_GetLight }, { "LightEnable", dv_LightEnable }, { "GetLightEnable", dv_GetLightEnable },
        { "SetClipPlane", dv_SetClipPlane }, { "GetClipPlane", NULL }, { "SetRenderState", dv_SetRenderState },
        { "GetRenderState", dv_GetRenderState }, { "CreateStateBlock", NULL }, { "BeginStateBlock", NULL },
        { "EndStateBlock", NULL }, { "SetClipStatus", NULL }, { "GetClipStatus", NULL }, { "GetTexture", dv_GetTexture },
        { "SetTexture", dv_SetTexture }, { "GetTextureStageState", dv_GetTextureStageState },
        { "SetTextureStageState", dv_SetTextureStageState }, { "GetSamplerState", dv_GetSamplerState },
        { "SetSamplerState", dv_SetSamplerState }, { "ValidateDevice", dv_ValidateDevice }, { "SetPaletteEntries", NULL },
        { "GetPaletteEntries", NULL }, { "SetCurrentTexturePalette", NULL }, { "GetCurrentTexturePalette", NULL },
        { "SetScissorRect", dv_SetScissorRect }, { "GetScissorRect", dv_GetScissorRect },
        { "SetSoftwareVertexProcessing", dv_SetSoftwareVertexProcessing },
        { "GetSoftwareVertexProcessing", dv_GetSoftwareVertexProcessing }, { "SetNPatchMode", dv_SetNPatchMode },
        { "GetNPatchMode", NULL }, { "DrawPrimitive", dv_DrawPrimitive }, { "DrawIndexedPrimitive", dv_DrawIndexedPrimitive },
        { "DrawPrimitiveUP", dv_DrawPrimitiveUP }, { "DrawIndexedPrimitiveUP", dv_DrawIndexedPrimitiveUP },
        { "ProcessVertices", NULL }, { "CreateVertexDeclaration", dv_CreateVertexDeclaration },
        { "SetVertexDeclaration", dv_SetVertexDeclaration }, { "GetVertexDeclaration", dv_GetVertexDeclaration },
        { "SetFVF", dv_SetFVF }, { "GetFVF", dv_GetFVF }, { "CreateVertexShader", dv_CreateVertexShader },
        { "SetVertexShader", dv_SetVertexShader }, { "GetVertexShader", dv_GetVertexShader },
        { "SetVertexShaderConstantF", dv_SetVertexShaderConstantF }, { "GetVertexShaderConstantF", dv_GetVertexShaderConstantF },
        { "SetVertexShaderConstantI", NULL }, { "GetVertexShaderConstantI", NULL }, { "SetVertexShaderConstantB", NULL },
        { "GetVertexShaderConstantB", NULL }, { "SetStreamSource", dv_SetStreamSource }, { "GetStreamSource", dv_GetStreamSource },
        { "SetStreamSourceFreq", NULL }, { "GetStreamSourceFreq", NULL }, { "SetIndices", dv_SetIndices },
        { "GetIndices", dv_GetIndices }, { "CreatePixelShader", dv_CreatePixelShader }, { "SetPixelShader", dv_SetPixelShader },
        { "GetPixelShader", dv_GetPixelShader }, { "SetPixelShaderConstantF", NULL }, { "GetPixelShaderConstantF", NULL },
        { "SetPixelShaderConstantI", NULL }, { "GetPixelShaderConstantI", NULL }, { "SetPixelShaderConstantB", NULL },
        { "GetPixelShaderConstantB", NULL }, { "DrawRectPatch", NULL }, { "DrawTriPatch", NULL }, { "DeletePatch", NULL },
        { "CreateQuery", dv_CreateQuery },
    };
    static const method tx[] = {
        RES_HEAD(t_Release, t_GetType), { "SetLOD", t_SetLOD }, { "GetLOD", t_GetLOD }, { "GetLevelCount", t_GetLevelCount },
        { "SetAutoGenFilterType", t_SetAutoGenFilterType }, { "GetAutoGenFilterType", t_GetAutoGenFilterType },
        { "GenerateMipSubLevels", t_GenerateMipSubLevels }, { "GetLevelDesc", t_GetLevelDesc },
        { "GetSurfaceLevel", t_GetSurfaceLevel }, { "LockRect", t_LockRect }, { "UnlockRect", t_UnlockRect },
        { "AddDirtyRect", t_AddDirtyRect },
    };
    static const method sf[] = {
        { "QueryInterface", s_QueryInterface }, { "AddRef", s_AddRef }, { "Release", s_Release }, { "GetDevice", any_GetDevice },
        { "SetPrivateData", NULL }, { "GetPrivateData", NULL }, { "FreePrivateData", NULL }, { "SetPriority", any_SetPriority },
        { "GetPriority", any_GetPriority }, { "PreLoad", any_PreLoad }, { "GetType", s_GetType },
        { "GetContainer", s_GetContainer }, { "GetDesc", s_GetDesc }, { "LockRect", s_LockRect },
        { "UnlockRect", s_UnlockRect }, { "GetDC", NULL }, { "ReleaseDC", NULL },
    };
    static const method bf[] = { RES_HEAD(b_Release, b_GetType), { "Lock", b_Lock }, { "Unlock", b_Unlock }, { "GetDesc", b_GetDesc } };
    static const method vs[] = { { "QueryInterface", t_QueryInterface }, { "AddRef", t_AddRef }, { "Release", vs_Release },
                                 { "GetDevice", any_GetDevice }, { "GetFunction", vs_GetFunction } };
    static const method dc[] = { { "QueryInterface", t_QueryInterface }, { "AddRef", t_AddRef }, { "Release", decl_Release },
                                 { "GetDevice", any_GetDevice }, { "GetDeclaration", decl_GetDeclaration } };
    g_vt_d3d = VT("IDirect3D9", d3), g_vt_dev = VT("IDirect3DDevice9", dv), g_vt_tex = VT("IDirect3DTexture9", tx);
    g_vt_surf = VT("IDirect3DSurface9", sf), g_vt_vb = VT("IDirect3DVertexBuffer9", bf), g_vt_ib = VT("IDirect3DIndexBuffer9", bf);
    g_vt_vs = VT("IDirect3DVertexShader9", vs), g_vt_decl = VT("IDirect3DVertexDeclaration9", dc);
    g_d3d = hle_com_new(g_vt_d3d, 16), g_dev = hle_com_new(g_vt_dev, 16);
}

static void k_Direct3DCreate9(void) {          /* (SDK version) */
    if (!g_d3d) make_vtables();
    else COUNT(g_d3d)++;
    RET(g_d3d, 1);
}
const win32hle_shim win32hle_d3d9[] = { { "Direct3DCreate9", k_Direct3DCreate9 }, { NULL, NULL } };
#endif /* HLE_WITH_D3D9 */
