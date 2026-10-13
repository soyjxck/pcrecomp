/*
 * d3d9_gl.c — the Direct3D 9 state of d3d9.c, drawn with OpenGL ES 3.
 *
 * Fixed-function vertex processing and the one texture stage games of that
 * time used are GLSL programs written from the state a draw is made with and
 * kept by that state; an assembly vertex shader is its translation
 * (d3d9_vs.c) between the same head and tail.
 *
 * Direct3D's picture has its first row at the top and OpenGL's at the bottom.
 * Everything here is drawn upside down as OpenGL sees it, so that row 0 of a
 * framebuffer is the top row: a texture drawn into then reads back, and
 * samples, exactly like one the guest filled from memory. Only the copy to
 * the window turns it over. That, the half pixel between the two APIs' pixel
 * centres and the depth range (0..1 against -1..1) are the last line of
 * every vertex program.
 *
 * The context is an SDL window's, or with no window (a test run) an EGL
 * context without a surface. A game may draw from more than one thread, one
 * at a time, and a context belongs to one thread at a time: the thread that
 * has it lets it go when it stops to wait, and when another thread has asked
 * for it (on_leave), and whoever draws next takes it (gl_enter).
 */
#ifdef HLE_WITH_D3D9
#define _GNU_SOURCE
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <GLES3/gl3.h>
#include <SDL.h>
#if defined(__linux__)
#include <EGL/egl.h>
#include <EGL/eglext.h>
#define D3D9_HAVE_EGL_HEADLESS 1
#endif
#include "d3d9_priv.h"

#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif

const char *hle_d3d9_title = "Direct3D";
const char *volatile hle_d3d9_doing = "nothing";       /* what the drawing half is in the middle of, for a watchdog */
#define DOING(what) (hle_d3d9_doing = (what))
int hle_d3d9_vsync;
static int g_swap_interval = -1;               /* what the window was last told of hle_d3d9_vsync */

static SDL_Window *g_win;
static SDL_GLContext g_ctx;
#ifdef D3D9_HAVE_EGL_HEADLESS
static EGLDisplay g_egl_dpy;
static EGLContext g_egl_ctx;
#endif
static pthread_t g_owner, g_window_thread;
static int g_owned;                            /* the context is some thread's: g_owner's */
static volatile int g_wanted;                  /* another thread is waiting for it */
static int g_have_aniso, g_max_tex = 4096, g_trace;
static unsigned g_census_frame;                /* HLE_D3D9_CENSUS: every so many frames, one whose draws are counted */
static int g_fills_said = 400;                 /* HLE_D3D9_FILLS: texture fills written to the log so far */
static GLuint g_vao, g_stream_vb, g_stream_ib;
static GLuint g_cur_fbo = ~0u, g_cur_prog, g_cur_tex = ~0u;
static GLuint g_cur_vb, g_cur_ib;              /* the buffers bound for vertices and for indices */
/* What the vertex arrays were last pointed at: a buffer, where its first
 * vertex is, how far apart the vertices are and what they are made of. A
 * draw that reads the same says nothing about arrays at all. */
static GLuint g_arr_buf;
static size_t g_arr_base;
static int g_arr_stride;
static uint64_t g_arr_sig;
static unsigned g_enabled;                     /* vertex arrays switched on, a bit each */
static unsigned long g_draws;

/* ---- names to delete, on the thread that owns the context ---------------------- */
static struct { GLuint tex, rb, fbo, buf; } g_retired[1024];
static int g_nretired;
void d3d9_gl_retire(unsigned tex, unsigned rb, unsigned fbo, unsigned buf) {
    if (!(tex | rb | fbo | buf) || g_nretired >= 1024) return;
    g_retired[g_nretired].tex = tex, g_retired[g_nretired].rb = rb, g_retired[g_nretired].fbo = fbo, g_retired[g_nretired].buf = buf;
    g_nretired++;
}

/* ---- framebuffers: a colour surface with a depth surface or none ---------------- */
typedef struct { const d3d9_surface *color, *depth; GLuint fbo; int dead; } fb_entry;
static fb_entry g_fb[64];
static int g_nfb;
void d3d9_gl_forget_surface(const d3d9_surface *s) {
    for (int i = 0; i < g_nfb; i++) if (g_fb[i].color == s || g_fb[i].depth == s) g_fb[i].dead = 1;
}

/* With HLE_D3D9_TRACE each hand-over is said, before and after: a driver
 * that will not move a context between threads stops or fails right here. */
static void make_current(int on) {
    int failed = 0, say = g_trace;
    unsigned me = (unsigned)((uintptr_t)pthread_self() >> 4) & 0xFFFFu;
    if (say) fprintf(stderr, "[d3d9] thread %04X %s the context (under sub_%08X)...\n", me, on ? "takes" : "lets go of", g_cur_func);
    if (g_win) failed = SDL_GL_MakeCurrent(on ? g_win : NULL, on ? g_ctx : NULL) != 0;
#ifdef D3D9_HAVE_EGL_HEADLESS
    else failed = !eglMakeCurrent(g_egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, on ? g_egl_ctx : EGL_NO_CONTEXT);
#endif
    if (say || failed) fprintf(stderr, "[d3d9] thread %04X: %s%s\n", me, failed ? "FAILED " : "done", failed && g_win ? SDL_GetError() : "");
}
static void on_leave(int waiting) {
    if (!g_owned || !pthread_equal(pthread_self(), g_owner) || (!waiting && !g_wanted)) return;
    glFlush();
    make_current(0);
    g_owned = 0;
}
static int gl_enter(void) {
    if (!d3d9.open && !g_vao) return 0;
    if (!g_owned || !pthread_equal(pthread_self(), g_owner)) {
        /* Whoever has it is waiting for the machine this thread holds, and
         * gives the context up as it gets its turn. */
        g_wanted = 1;
        for (int i = 0; g_owned && i < 10000; i++) hle_yield();
        g_wanted = 0;
        if (g_owned) {
            static int said;
            if (!said++) fprintf(stderr, "[d3d9] the thread that draws did not let go (under sub_%08X): drawing skipped\n", g_cur_func);
            return 0;
        }
        make_current(1);
        g_owner = pthread_self(), g_owned = 1;
    }
    for (int i = 0; i < g_nfb; i++)
        if (g_fb[i].dead) {
            glDeleteFramebuffers(1, &g_fb[i].fbo);
            if (g_cur_fbo == g_fb[i].fbo) g_cur_fbo = ~0u;
            g_fb[i--] = g_fb[--g_nfb];
        }
    for (int i = 0; i < g_nretired; i++) {
        if (g_retired[i].tex) { glDeleteTextures(1, &g_retired[i].tex); if (g_cur_tex == g_retired[i].tex) g_cur_tex = ~0u; }
        if (g_retired[i].rb) glDeleteRenderbuffers(1, &g_retired[i].rb);
        if (g_retired[i].fbo) glDeleteFramebuffers(1, &g_retired[i].fbo);
        if (g_retired[i].buf) {
            glDeleteBuffers(1, &g_retired[i].buf);          /* and its name may be given out again */
            if (g_cur_vb == g_retired[i].buf) g_cur_vb = 0;
            if (g_cur_ib == g_retired[i].buf) g_cur_ib = 0;
            if (g_arr_buf == g_retired[i].buf) g_arr_buf = 0;
        }
    }
    g_nretired = 0;
    return 1;
}

static void bind_texture(GLuint t) { if (g_cur_tex != t) glBindTexture(GL_TEXTURE_2D, t), g_cur_tex = t; }
static void bind_fbo(GLuint f) { if (g_cur_fbo != f) glBindFramebuffer(GL_FRAMEBUFFER, f), g_cur_fbo = f; }

/* The GL texture behind a surface that is drawn into, made on first use. A
 * format without alpha is kept with its alpha at 1 (cleared so here, and
 * never written: see apply_state). */
static void state_forget(void);
static GLuint target_texture(d3d9_surface *s) {
    GLuint *name = s->kind == D3D9_SURF_LEVEL ? &s->tex->gl : &s->gl_tex;
    if (!*name) {
        state_forget();                        /* the clear below has a mask and a scissor of its own */
        glGenTextures(1, name);
        bind_texture(*name);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, s->w, s->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        if (s->tex) s->tex->gl_min = s->tex->gl_mag = s->tex->gl_u = s->tex->gl_v = -1;
        GLuint f;
        glGenFramebuffers(1, &f);
        bind_fbo(f);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *name, 0);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(1, 1, 1, 1);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        bind_fbo(0);
        glDeleteFramebuffers(1, &f);
    }
    return *name;
}
static GLuint fbo_for(d3d9_surface *color, d3d9_surface *depth) {
    for (int i = 0; i < g_nfb; i++) if (g_fb[i].color == color && g_fb[i].depth == depth) return g_fb[i].fbo;
    GLuint tex = target_texture(color), f;
    glGenFramebuffers(1, &f);
    bind_fbo(f);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    if (depth) {
        int stencil = depth->fmt != D3DFMT_D16 && depth->fmt != D3DFMT_D16_LOCKABLE;
        if (!depth->gl_rb) {
            glGenRenderbuffers(1, &depth->gl_rb);
            glBindRenderbuffer(GL_RENDERBUFFER, depth->gl_rb);
            glRenderbufferStorage(GL_RENDERBUFFER, stencil ? GL_DEPTH24_STENCIL8 : GL_DEPTH_COMPONENT16, depth->w, depth->h);
        }
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, stencil ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth->gl_rb);
    }
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE)
        fprintf(stderr, "[d3d9] framebuffer %dx%d (depth %s) is not complete: 0x%X\n", color->w, color->h, depth ? "yes" : "no", st);
    if (g_nfb < 64) g_fb[g_nfb].color = color, g_fb[g_nfb].depth = depth, g_fb[g_nfb].fbo = f, g_fb[g_nfb].dead = 0, g_nfb++;
    return f;
}
static int drawn_into(const d3d9_surface *s) { return s->kind == D3D9_SURF_LEVEL ? s->tex->gl != 0 : s->gl_tex != 0; }

/* ---- the pointer ------------------------------------------------------------------- *
 * The window's mouse is the guest's mouse, at the point of the back buffer
 * the present has put under it. A touch screen is a mouse too (SDL reports a
 * finger as one), with one difference: a finger does not hover. It lands and
 * presses at once, and a menu that takes "what the pointer is over" a frame
 * before it takes the click would miss the press. So a touch moves the pointer
 * at once, presses two frames later, and lets go no sooner than two frames
 * after that. */
#define WM_MOUSEMOVE_   0x0200u
#define WM_LBUTTONDOWN_ 0x0201u
#define WM_LBUTTONUP_   0x0202u
#define WM_RBUTTONDOWN_ 0x0204u
#define WM_RBUTTONUP_   0x0205u
#define WM_MBUTTONDOWN_ 0x0207u
#define WM_MBUTTONUP_   0x0208u
static struct { uint32_t message; int x, y; unsigned frame; } g_later[8];
static int g_nlater;
static unsigned g_touch_pressed;        /* the frame the last touch's press is due */

static void pointer_at(uint32_t message, int x, int y) {       /* a point of the back buffer */
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= d3d9.w) x = d3d9.w - 1;
    if (y >= d3d9.h) y = d3d9.h - 1;
    hle_client_to_screen(d3d9.window, &x, &y);
    hle_input_mouse(message, x, y);
}
static void pointer_later(uint32_t message, int x, int y, unsigned frame) {
    if (g_nlater == 8) pointer_at(g_later[0].message, g_later[0].x, g_later[0].y), memmove(g_later, g_later + 1, 7 * sizeof g_later[0]), g_nlater--;
    g_later[g_nlater].message = message, g_later[g_nlater].x = x, g_later[g_nlater].y = y, g_later[g_nlater].frame = frame;
    g_nlater++;
}
static void pointer_due(void) {
    while (g_nlater && (int)(d3d9.frame - g_later[0].frame) >= 0) {
        pointer_at(g_later[0].message, g_later[0].x, g_later[0].y);
        memmove(g_later, g_later + 1, (size_t)(--g_nlater) * sizeof g_later[0]);
    }
}
void hle_d3d9_touch(int down, int x, int y) {
    if (!d3d9.open) return;
    if (down) {
        pointer_at(WM_MOUSEMOVE_, x, y);
        g_touch_pressed = d3d9.frame + 2;
        pointer_later(WM_LBUTTONDOWN_, x, y, g_touch_pressed);
    } else {
        unsigned at = d3d9.frame + 1;
        if ((int)(g_touch_pressed + 2 - at) > 0) at = g_touch_pressed + 2;
        pointer_later(WM_LBUTTONUP_, x, y, at);
    }
}
/* A point of the window, in the back buffer's pixels: the present's own sums, backwards. */
static void from_window(int wx, int wy, int *x, int *y) {
    int lw = 0, lh = 0, ww = 0, wh = 0;
    SDL_GetWindowSize(g_win, &lw, &lh);
    SDL_GL_GetDrawableSize(g_win, &ww, &wh);
    if (lw > 0 && lh > 0) wx = wx * ww / lw, wy = wy * wh / lh;
    int w = ww, h = d3d9.w ? ww * d3d9.h / d3d9.w : wh;
    if (h > wh) h = wh, w = d3d9.h ? wh * d3d9.w / d3d9.h : ww;
    *x = w > 0 ? (wx - (ww - w) / 2) * d3d9.w / w : 0;
    *y = h > 0 ? (wy - (wh - h) / 2) * d3d9.h / h : 0;
}

/* ---- the context ------------------------------------------------------------------- */
static void pump(void) {
    SDL_Event e;
    int x, y;
    if (!g_win || !pthread_equal(pthread_self(), g_window_thread)) return;     /* events belong to the window's thread */
    while (SDL_PollEvent(&e)) switch (e.type) {
    case SDL_QUIT:
        hle_post_message(d3d9.window, 0x0010 /* WM_CLOSE */, 0, 0);
        break;
    case SDL_MOUSEMOTION:
        from_window(e.motion.x, e.motion.y, &x, &y);
        pointer_at(WM_MOUSEMOVE_, x, y);
        break;
    case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: {
        int down = e.type == SDL_MOUSEBUTTONDOWN;
        from_window(e.button.x, e.button.y, &x, &y);
        if (e.button.which == SDL_TOUCH_MOUSEID) hle_d3d9_touch(down, x, y);
        else pointer_at(e.button.button == SDL_BUTTON_RIGHT ? (down ? WM_RBUTTONDOWN_ : WM_RBUTTONUP_)
                      : e.button.button == SDL_BUTTON_MIDDLE ? (down ? WM_MBUTTONDOWN_ : WM_MBUTTONUP_)
                      : (down ? WM_LBUTTONDOWN_ : WM_LBUTTONUP_), x, y);
        break;
    }
    }
}
#ifdef D3D9_HAVE_EGL_HEADLESS
static int open_headless(void) {
    EGLDisplay dpy = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    EGLint major, minor, n = 0;
    EGLConfig cfg;
    static const EGLint want[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE };
    static const EGLint version[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE };
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &major, &minor)) return -1;
    eglBindAPI(EGL_OPENGL_ES_API);
    if (!eglChooseConfig(dpy, want, &cfg, 1, &n) || n < 1) return -1;
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, version);
    if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) return -1;
    g_egl_dpy = dpy, g_egl_ctx = ctx;
    return 0;
}
#endif
static int open_window(int w, int h) {
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "[d3d9] SDL: %s\n", SDL_GetError()); return -1; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    g_win = SDL_CreateWindow(hle_d3d9_title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h,
                             SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!g_win) { fprintf(stderr, "[d3d9] no window: %s\n", SDL_GetError()); return -1; }
    g_ctx = SDL_GL_CreateContext(g_win);
    if (!g_ctx) { fprintf(stderr, "[d3d9] no OpenGL ES 3 context: %s\n", SDL_GetError()); return -1; }
    SDL_GL_SetSwapInterval(hle_d3d9_vsync), g_swap_interval = hle_d3d9_vsync;
    g_window_thread = pthread_self();
    hle_set_pump_hook(pump);
    return 0;
}
int d3d9_gl_open(int w, int h, int headless) {
    int rc = -1;
#ifdef D3D9_HAVE_EGL_HEADLESS
    if (headless) rc = open_headless();
#else
    (void)headless;
#endif
    if (rc != 0 && !headless) rc = open_window(w, h);
    if (rc != 0) { fprintf(stderr, "[d3d9] no OpenGL ES 3 context to draw with\n"); return -1; }
    g_owner = pthread_self(), g_owned = 1;
    hle_leave_hook = on_leave;
    const char *ext = (const char *)glGetString(GL_EXTENSIONS);
    g_have_aniso = ext && strstr(ext, "texture_filter_anisotropic");
    g_trace = getenv("HLE_D3D9_TRACE") != NULL;
    if (getenv("HLE_D3D9_CENSUS")) g_census_frame = (unsigned)atoi(getenv("HLE_D3D9_CENSUS"));
    if (getenv("HLE_D3D9_FILLS")) g_fills_said = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &g_max_tex);
    fprintf(stderr, "[d3d9] %s, %s (%s)\n", (const char *)glGetString(GL_VERSION), (const char *)glGetString(GL_RENDERER),
            headless ? "no window" : "window");
    glGenVertexArrays(1, &g_vao);
    glBindVertexArray(g_vao);
    glGenBuffers(1, &g_stream_vb);
    glGenBuffers(1, &g_stream_ib);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glDisable(GL_DITHER);
    return 0;
}
/* A new back buffer: what GL made for the old one goes. */
void d3d9_gl_reset(int w, int h) {
    (void)w, (void)h;
    if (!d3d9.back) return;
    d3d9_gl_forget_surface(d3d9.back), d3d9_gl_forget_surface(d3d9.back_ds);
    d3d9_gl_retire(d3d9.back->gl_tex, d3d9.back_ds->gl_rb, 0, 0);
    d3d9.back->gl_tex = 0, d3d9.back_ds->gl_rb = 0;
}
int d3d9_gl_max_texture(void) { return g_max_tex; }

/* ---- textures the guest fills ---------------------------------------------------- */
static uint8_t *g_scratch;
static size_t g_scratch_cap;
static uint8_t *scratch(size_t n) {
    if (n > g_scratch_cap) { free(g_scratch); g_scratch = malloc(n); g_scratch_cap = g_scratch ? n : 0; }
    return g_scratch;
}
static void swap_rb(uint8_t *px, size_t n) { for (size_t i = 0; i < n; i++, px += 4) { uint8_t t = px[0]; px[0] = px[2], px[2] = t; } }

/* A row of a texture as GL takes it: R, G, B, A bytes. The formats a game's
 * art is nearly all in go there in one pass (sixteen-bit pixels with four bits
 * a channel are what a sprite sheet is, and a game that keeps a few dozen of
 * them and refills them as its people turn about sends several a second);
 * anything else by way of d3d9_to_bgra. */
#if defined(__aarch64__)
#include <arm_neon.h>
#endif
static void row_to_rgba(int fmt, const uint8_t *src, uint8_t *o, int n) {
    int i = 0;
    switch (fmt) {
    case D3DFMT_A4R4G4B4: case D3DFMT_X4R4G4B4:
#if defined(__aarch64__)
        for (; i + 8 <= n; i += 8) {
            uint16x8_t v = vreinterpretq_u16_u8(vld1q_u8(src + 2 * i)), low = vdupq_n_u16(15);
            uint8x8_t k = vdup_n_u8(17);
            uint8x8x4_t px;
            px.val[0] = vmul_u8(vmovn_u16(vandq_u16(vshrq_n_u16(v, 8), low)), k);
            px.val[1] = vmul_u8(vmovn_u16(vandq_u16(vshrq_n_u16(v, 4), low)), k);
            px.val[2] = vmul_u8(vmovn_u16(vandq_u16(v, low)), k);
            px.val[3] = fmt == D3DFMT_X4R4G4B4 ? vdup_n_u8(255) : vmul_u8(vmovn_u16(vshrq_n_u16(v, 12)), k);
            vst4_u8(o + 4 * i, px);
        }
#endif
        for (; i < n; i++) {                   /* a nibble times 17 is the nibble twice: four of them at once, a byte apart */
            uint16_t v;
            memcpy(&v, src + 2 * i, 2);
            uint32_t x = (uint32_t)(v >> 8 & 15) | (uint32_t)(v >> 4 & 15) << 8 | (uint32_t)(v & 15) << 16 | (uint32_t)(v >> 12) << 24;
            x *= 17;
            if (fmt == D3DFMT_X4R4G4B4) x |= 0xFF000000u;
            o[4 * i] = (uint8_t)x, o[4 * i + 1] = (uint8_t)(x >> 8), o[4 * i + 2] = (uint8_t)(x >> 16), o[4 * i + 3] = (uint8_t)(x >> 24);
        }
        return;
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8:
#if defined(__aarch64__)
        for (; i + 16 <= n; i += 16) {
            uint8x16x4_t px = vld4q_u8(src + 4 * i);
            uint8x16_t b = px.val[0];
            px.val[0] = px.val[2], px.val[2] = b;
            if (fmt == D3DFMT_X8R8G8B8) px.val[3] = vdupq_n_u8(255);
            vst4q_u8(o + 4 * i, px);
        }
#endif
        for (; i < n; i++) {
            o[4 * i] = src[4 * i + 2], o[4 * i + 1] = src[4 * i + 1], o[4 * i + 2] = src[4 * i];
            o[4 * i + 3] = fmt == D3DFMT_X8R8G8B8 ? 255 : src[4 * i + 3];
        }
        return;
    }
    d3d9_to_bgra(fmt, src, o, n);
    swap_rb(o, (size_t)n);
}

static GLuint texture_ready(d3d9_texture *t);
static GLuint sampled_texture(d3d9_texture *t) {         /* timed when there is something to fill */
    if (!(t->usage & D3DUSAGE_RENDERTARGET) && t->gl && !t->dirty) return t->gl;
    uint64_t t0 = hle_now_ns();
    GLuint gl = texture_ready(t);
    hle_perf.texture_ns += hle_now_ns() - t0, hle_perf.textures++;
    return gl;
}
static GLuint texture_ready(d3d9_texture *t) {
    if (t->usage & D3DUSAGE_RENDERTARGET) return target_texture(t->level[0]);
    int fresh = !t->gl;
    if (fresh) {
        glGenTextures(1, &t->gl);
        t->gl_min = t->gl_mag = t->gl_u = t->gl_v = -1, t->gl_aniso = 1;
    }
    if (!fresh && !t->dirty) return t->gl;
    if (g_fills_said < 400) {                  /* HLE_D3D9_FILLS: what the first four hundred fills are of */
        int levels = 0;
        for (int i = 0; i < t->levels; i++) levels += fresh || t->level[i]->dirty;
        g_fills_said++;
        fprintf(stderr, "[d3d9] frame %u: a texture filled %s: %dx%d, format %d, %d of %d levels (under sub_%08X)\n", d3d9.frame,
                fresh ? "for the first time" : "again", t->level[0]->w, t->level[0]->h, t->level[0]->fmt, levels, t->levels, g_cur_func);
    }
    bind_texture(t->gl);
    if (fresh) glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, t->levels - 1);
    for (int i = 0; i < t->levels; i++) {
        d3d9_surface *s = t->level[i];
        if (!fresh && !s->dirty) continue;
        uint8_t *px = NULL;
        if (s->bits && (px = scratch((size_t)s->w * (size_t)s->h * 4)) != NULL) {
            for (int r = 0; r < s->h; r++)
                row_to_rgba(s->fmt, (const uint8_t *)G2H(s->bits) + (size_t)r * (size_t)s->pitch, px + (size_t)r * (size_t)s->w * 4, s->w);
        }
        glTexImage2D(GL_TEXTURE_2D, i, GL_RGBA8, s->w, s->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        s->dirty = 0;
    }
    t->dirty = 0;
    return t->gl;
}
static void apply_sampler(d3d9_texture *t) {
    const uint32_t *ss = d3d9.ss[0];
    int mips = t->levels > 1 && ss[D3DSAMP_MIPFILTER] != D3DTEXF_NONE;
    int lin = ss[D3DSAMP_MINFILTER] >= D3DTEXF_LINEAR;
    int min = !mips ? (lin ? GL_LINEAR : GL_NEAREST)
            : ss[D3DSAMP_MIPFILTER] >= D3DTEXF_LINEAR ? (lin ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR)
            : (lin ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST);
    int mag = ss[D3DSAMP_MAGFILTER] >= D3DTEXF_LINEAR ? GL_LINEAR : GL_NEAREST;
    static const int wrap[] = { GL_REPEAT, GL_REPEAT, GL_MIRRORED_REPEAT, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE };
    int u = wrap[ss[D3DSAMP_ADDRESSU] < 6 ? ss[D3DSAMP_ADDRESSU] : 0], v = wrap[ss[D3DSAMP_ADDRESSV] < 6 ? ss[D3DSAMP_ADDRESSV] : 0];
    int aniso = ss[D3DSAMP_MINFILTER] == D3DTEXF_ANISOTROPIC && ss[D3DSAMP_MAXANISOTROPY] > 1 ? (int)ss[D3DSAMP_MAXANISOTROPY] : 1;
    if (t->gl_min != min) glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min), t->gl_min = min;
    if (t->gl_mag != mag) glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag), t->gl_mag = mag;
    if (t->gl_u != u) glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, u), t->gl_u = u;
    if (t->gl_v != v) glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, v), t->gl_v = v;
    if (g_have_aniso && t->gl_aniso != aniso) glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, (float)aniso), t->gl_aniso = aniso;
}

/* ---- pixels of a target, in and out -------------------------------------------------- */
int d3d9_gl_read(d3d9_surface *s, int x, int y, int w, int h, uint8_t *bgra) {
    if (!gl_enter()) return -1;
    if (!drawn_into(s)) {                      /* nothing has touched it: black, without making it */
        for (size_t i = 0; i < (size_t)w * (size_t)h; i++) bgra[4 * i] = bgra[4 * i + 1] = bgra[4 * i + 2] = 0, bgra[4 * i + 3] = 255;
        return 0;
    }
    bind_fbo(fbo_for(s, NULL));
    glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, bgra);
    swap_rb(bgra, (size_t)w * (size_t)h);
    if (!d3d9_fmt_alpha(s->fmt)) for (size_t i = 0; i < (size_t)w * (size_t)h; i++) bgra[4 * i + 3] = 255;
    return 0;
}
void d3d9_gl_upload(d3d9_surface *s, int x, int y, int w, int h, const uint8_t *bgra) {
    if (!gl_enter()) return;
    uint8_t *px = scratch((size_t)w * (size_t)h * 4);
    if (!px) return;
    memcpy(px, bgra, (size_t)w * (size_t)h * 4);
    swap_rb(px, (size_t)w * (size_t)h);
    if (!d3d9_fmt_alpha(s->fmt)) for (size_t i = 0; i < (size_t)w * (size_t)h; i++) px[4 * i + 3] = 255;
    bind_texture(target_texture(s));
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
}
void d3d9_gl_blit(d3d9_surface *from, const int *fr, d3d9_surface *to, const int *tr, int linear) {
    if (!gl_enter()) return;
    GLuint a = fbo_for(from, NULL), b = fbo_for(to, NULL);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, a);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, b);
    g_cur_fbo = ~0u;
    glDisable(GL_SCISSOR_TEST);
    glColorMask(1, 1, 1, 1);
    glBlitFramebuffer(fr[0], fr[1], fr[2], fr[3], tr[0], tr[1], tr[2], tr[3], GL_COLOR_BUFFER_BIT, linear ? GL_LINEAR : GL_NEAREST);
    state_forget();
}

/* ---- render state ------------------------------------------------------------------- */
static GLenum blend_factor(uint32_t b, int dest) {
    switch (b) {
    case 1: return GL_ZERO;
    case 2: return GL_ONE;
    case 3: return GL_SRC_COLOR;
    case 4: return GL_ONE_MINUS_SRC_COLOR;
    case 5: return GL_SRC_ALPHA;
    case 6: return GL_ONE_MINUS_SRC_ALPHA;
    case 7: return GL_DST_ALPHA;
    case 8: return GL_ONE_MINUS_DST_ALPHA;
    case 9: return GL_DST_COLOR;
    case 10: return GL_ONE_MINUS_DST_COLOR;
    case 11: return GL_SRC_ALPHA_SATURATE;
    case 12: return dest ? GL_ONE_MINUS_SRC_ALPHA : GL_SRC_ALPHA;     /* both source alpha */
    case 13: return dest ? GL_SRC_ALPHA : GL_ONE_MINUS_SRC_ALPHA;
    case 14: return GL_CONSTANT_COLOR;
    case 15: return GL_ONE_MINUS_CONSTANT_COLOR;
    }
    return dest ? GL_ZERO : GL_ONE;
}
static void color_of(uint32_t c, float *f) {   /* a D3DCOLOR as r, g, b, a */
    f[0] = (float)((c >> 16) & 255) / 255.0f, f[1] = (float)((c >> 8) & 255) / 255.0f;
    f[2] = (float)(c & 255) / 255.0f, f[3] = (float)(c >> 24) / 255.0f;
}
static float rs_float(int which) { float f; memcpy(&f, &d3d9.rs[which], 4); return f; }

/* The framebuffer, viewport and per-pixel state of the device, given to GL:
 * only what is not as GL was last told. Most of a frame's draws differ from
 * the one before in a texture and a matrix, and each of these calls is a
 * trip into the driver. Whatever else sets one of them (a clear, a present,
 * a copy between surfaces, a host's picture) calls state_forget, and the
 * next draw says everything again. */
static struct {
    int known;
    GLint vx, vy;
    GLsizei vw, vh;
    float zn, zf;
    int scissor, sx, sy, sw, sh;
    int depth, zwrite;
    GLenum zfunc;
    int blend, separate;
    GLenum sf, df, saf, daf, eq, eqa;
    uint32_t bcolor;
    int cull;
    GLenum face;
    int mask;
} g_gl;
static void state_forget(void) { g_gl.known = 0; }

static void apply_state(void) {
    const uint32_t *rs = d3d9.rs;
    bind_fbo(fbo_for(d3d9.rt, d3d9.ds));
    int all = !g_gl.known;
    GLint vx = (GLint)d3d9.vp.x, vy = (GLint)d3d9.vp.y;
    GLsizei vw = (GLsizei)d3d9.vp.w, vh = (GLsizei)d3d9.vp.h;
    if (all || vx != g_gl.vx || vy != g_gl.vy || vw != g_gl.vw || vh != g_gl.vh)
        glViewport(vx, vy, vw, vh), g_gl.vx = vx, g_gl.vy = vy, g_gl.vw = vw, g_gl.vh = vh;
    if (all || d3d9.vp.minz != g_gl.zn || d3d9.vp.maxz != g_gl.zf)
        glDepthRangef(d3d9.vp.minz, d3d9.vp.maxz), g_gl.zn = d3d9.vp.minz, g_gl.zf = d3d9.vp.maxz;
    int scissor = rs[D3DRS_SCISSORTESTENABLE] != 0;
    if (scissor) {
        int sx = d3d9.scissor[0], sy = d3d9.scissor[1], sw = d3d9.scissor[2] - d3d9.scissor[0], sh = d3d9.scissor[3] - d3d9.scissor[1];
        if (all || !g_gl.scissor) glEnable(GL_SCISSOR_TEST);
        if (all || !g_gl.scissor || sx != g_gl.sx || sy != g_gl.sy || sw != g_gl.sw || sh != g_gl.sh)
            glScissor(sx, sy, sw, sh), g_gl.sx = sx, g_gl.sy = sy, g_gl.sw = sw, g_gl.sh = sh;
    } else if (all || g_gl.scissor) glDisable(GL_SCISSOR_TEST);
    g_gl.scissor = scissor;
    int depth = rs[D3DRS_ZENABLE] && d3d9.ds;
    if (depth) {
        GLenum zfunc = GL_NEVER + (rs[D3DRS_ZFUNC] >= 1 && rs[D3DRS_ZFUNC] <= 8 ? rs[D3DRS_ZFUNC] - 1 : 7);
        int zwrite = rs[D3DRS_ZWRITEENABLE] != 0;
        if (all || !g_gl.depth) glEnable(GL_DEPTH_TEST);
        if (all || !g_gl.depth || zfunc != g_gl.zfunc) glDepthFunc(zfunc), g_gl.zfunc = zfunc;
        if (all || !g_gl.depth || zwrite != g_gl.zwrite) glDepthMask((GLboolean)zwrite), g_gl.zwrite = zwrite;
    } else if (all || g_gl.depth) glDisable(GL_DEPTH_TEST);
    g_gl.depth = depth;
    int blend = rs[D3DRS_ALPHABLENDENABLE] != 0;
    if (blend) {
        static const GLenum eq[] = { GL_FUNC_ADD, GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT, GL_MIN, GL_MAX };
        int separate = rs[D3DRS_SEPARATEALPHABLENDENABLE] != 0;
        GLenum sf = blend_factor(rs[D3DRS_SRCBLEND], 0), df = blend_factor(rs[D3DRS_DESTBLEND], 1);
        GLenum saf = separate ? blend_factor(rs[D3DRS_SRCBLENDALPHA], 0) : sf, daf = separate ? blend_factor(rs[D3DRS_DESTBLENDALPHA], 1) : df;
        GLenum e = eq[rs[D3DRS_BLENDOP] < 6 ? rs[D3DRS_BLENDOP] : 0], ea = separate ? eq[rs[D3DRS_BLENDOPALPHA] < 6 ? rs[D3DRS_BLENDOPALPHA] : 0] : e;
        int again = all || !g_gl.blend;
        if (again) glEnable(GL_BLEND);
        if (again || sf != g_gl.sf || df != g_gl.df || saf != g_gl.saf || daf != g_gl.daf)
            glBlendFuncSeparate(sf, df, saf, daf), g_gl.sf = sf, g_gl.df = df, g_gl.saf = saf, g_gl.daf = daf;
        if (again || e != g_gl.eq || ea != g_gl.eqa) glBlendEquationSeparate(e, ea), g_gl.eq = e, g_gl.eqa = ea;
        if (again || rs[D3DRS_BLENDFACTOR] != g_gl.bcolor) {
            float f[4];
            color_of(rs[D3DRS_BLENDFACTOR], f);
            glBlendColor(f[0], f[1], f[2], f[3]);
            g_gl.bcolor = rs[D3DRS_BLENDFACTOR];
        }
    } else if (all || g_gl.blend) glDisable(GL_BLEND);
    g_gl.blend = blend;
    /* Clockwise on Direct3D's screen is counter-clockwise on this one, drawn
     * upside down: GL's front faces are the ones Direct3D calls clockwise. */
    int cull = rs[D3DRS_CULLMODE] == D3DCULL_CW || rs[D3DRS_CULLMODE] == D3DCULL_CCW;
    if (cull) {
        GLenum face = rs[D3DRS_CULLMODE] == D3DCULL_CCW ? GL_BACK : GL_FRONT;
        if (all || !g_gl.cull) glEnable(GL_CULL_FACE);
        if (all || !g_gl.cull || face != g_gl.face) glCullFace(face), g_gl.face = face;
    } else if (all || g_gl.cull) glDisable(GL_CULL_FACE);
    g_gl.cull = cull;
    uint32_t cw = rs[D3DRS_COLORWRITEENABLE];
    int mask = (int)(cw & 7) | ((cw & 8) && d3d9_fmt_alpha(d3d9.rt->fmt) ? 8 : 0);
    if (all || mask != g_gl.mask) glColorMask((mask & 1) != 0, (mask & 2) != 0, (mask & 4) != 0, (mask & 8) != 0), g_gl.mask = mask;
    if (rs[D3DRS_STENCILENABLE]) { static int said; if (!said++) fprintf(stderr, "[d3d9] not done: the stencil buffer\n"); }
    g_gl.known = 1;
}

void d3d9_gl_clear(uint32_t flags, uint32_t color, float z, uint32_t stencil) {
    if (!gl_enter()) return;
    uint64_t t0 = hle_now_ns();
    DOING("clearing");
    GLbitfield bits = 0;
    float f[4];
    bind_fbo(fbo_for(d3d9.rt, d3d9.ds));
    glEnable(GL_SCISSOR_TEST);                 /* Direct3D clears the viewport, GL the whole buffer */
    glScissor((GLint)d3d9.vp.x, (GLint)d3d9.vp.y, (GLsizei)d3d9.vp.w, (GLsizei)d3d9.vp.h);
    if (flags & D3DCLEAR_TARGET) {
        color_of(color, f);
        glColorMask(1, 1, 1, 1);
        glClearColor(f[0], f[1], f[2], d3d9_fmt_alpha(d3d9.rt->fmt) ? f[3] : 1.0f);
        bits |= GL_COLOR_BUFFER_BIT;
    }
    if ((flags & D3DCLEAR_ZBUFFER) && d3d9.ds) { glDepthMask(1); glClearDepthf(z); bits |= GL_DEPTH_BUFFER_BIT; }
    if ((flags & D3DCLEAR_STENCIL) && d3d9.ds) { glStencilMask(~0u); glClearStencil((GLint)stencil); bits |= GL_STENCIL_BUFFER_BIT; }
    if (bits) glClear(bits);
    state_forget();
    hle_perf.clear_ns += hle_now_ns() - t0;
    DOING("nothing");
}

/* ---- programs --------------------------------------------------------------------------- */
enum { MODE_RHW, MODE_FIXED, MODE_SHADER };
enum { FOG_OFF, FOG_LINEAR, FOG_EXP, FOG_EXP2, FOG_OWN };
enum { FOG_BY_W, FOG_BY_Z, FOG_BY_EYE };
typedef struct {
    uint8_t mode, diffuse, specular, tex, normal;      /* how vertices come, and what they carry */
    uint8_t lighting, light[4];                        /* fixed function: lights on, and the kind of each of four */
    uint8_t dsrc, asrc;                                /* where a lit vertex's diffuse and ambient come from */
    uint8_t fog, fog_by;
    uint8_t cop, ca1, ca2, aop, aa1, aa2, sample;      /* texture stage 0, settled */
    uint8_t atest, spec_add;
    int32_t vs, decl;
    uint32_t colors;                                   /* shader inputs that are D3DCOLOR, a bit each */
} prog_key;
typedef struct {
    prog_key key;
    GLuint id;
    GLint view, fog, fogcolor, aref, tfactor, wvp, wv, c, mat, amb, lpos, ldir, ldif, lamb, latt, lspot;
    int nconst;
    /* what it was last sent, so that the same is not sent again */
    unsigned sent, consts;          /* SENT_* bits; the constants' serial + 1 */
    float u_view[4], u_fog[8], u_aref, u_wvp[16], u_wv[16];
    uint32_t u_tfactor;
} prog;
enum { SENT_VIEW = 1, SENT_FOG = 2, SENT_AREF = 4, SENT_TFACTOR = 8, SENT_WVP = 16, SENT_WV = 32 };
static prog g_prog[512];
static int g_nprog;

typedef struct { char *s; size_t n, cap; } sbuf;
static void sb(sbuf *b, const char *fmt, ...) {
    va_list ap;
    for (;;) {
        va_start(ap, fmt);
        int k = vsnprintf(b->s + b->n, b->cap - b->n, fmt, ap);
        va_end(ap);
        if (k >= 0 && (size_t)k < b->cap - b->n) { b->n += (size_t)k; return; }
        b->cap = b->cap * 2 + (size_t)(k > 0 ? k : 0) + 256;
        b->s = realloc(b->s, b->cap);
    }
}

typedef struct { int offset, type, usage, index; } elem;
static int elems_from_fvf(uint32_t fvf, elem *e) {
    int n = 0, off = 12;
    uint32_t pos = fvf & D3DFVF_POSITION_MASK;
    if (pos == D3DFVF_XYZRHW) e[n++] = (elem){ 0, D3DDECLTYPE_FLOAT4, 9, 0 }, off = 16;        /* a transformed position */
    else if (pos == D3DFVF_XYZW) e[n++] = (elem){ 0, D3DDECLTYPE_FLOAT4, 0, 0 }, off = 16;
    else {
        e[n++] = (elem){ 0, D3DDECLTYPE_FLOAT3, 0, 0 };
        if (pos >= D3DFVF_XYZB1 && pos <= D3DFVF_XYZB5) off += 4 * (int)((pos - D3DFVF_XYZB1) / 2 + 1);   /* blend weights: not used */
    }
    if (fvf & D3DFVF_NORMAL) e[n++] = (elem){ off, D3DDECLTYPE_FLOAT3, 3, 0 }, off += 12;
    if (fvf & D3DFVF_PSIZE) off += 4;
    if (fvf & D3DFVF_DIFFUSE) e[n++] = (elem){ off, D3DDECLTYPE_D3DCOLOR, 10, 0 }, off += 4;
    if (fvf & D3DFVF_SPECULAR) e[n++] = (elem){ off, D3DDECLTYPE_D3DCOLOR, 10, 1 }, off += 4;
    int ntex = (int)((fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT);
    for (int i = 0; i < ntex && i < 8; i++) {
        static const int comps[] = { 2, 3, 4, 1 };
        int c = comps[(fvf >> (16 + 2 * i)) & 3];
        e[n++] = (elem){ off, D3DDECLTYPE_FLOAT1 + c - 1, 5, i }, off += 4 * c;
    }
    return n;
}
static const elem *find_elem(const elem *e, int n, int usage, int index) {
    for (int i = 0; i < n; i++) if (e[i].usage == usage && e[i].index == index) return &e[i];
    return NULL;
}

static const char *stage_arg(int arg, int alpha) {
    static char out[4][48];
    static int turn;
    static const char *const base[] = { "dif", "cur", "tex", "u_tfactor", "v_d1" };
    char *o = out[turn++ & 3];
    int sel = arg & D3DTA_SELECTMASK;
    const char *b = sel < 5 ? base[sel] : "cur";
    const char *part = alpha || (arg & D3DTA_ALPHAREPLICATE) ? (alpha ? ".a" : ".aaa") : ".rgb";
    if (arg & D3DTA_COMPLEMENT) snprintf(o, 48, "(1.0 - %s%s)", b, part);
    else snprintf(o, 48, "%s%s", b, part);
    return o;
}
static void stage_op(sbuf *b, const char *dst, int op, int a1, int a2, int alpha) {
    const char *x = stage_arg(a1, alpha), *y = stage_arg(a2, alpha);
    switch (op) {
    case D3DTOP_SELECTARG1: sb(b, "  %s = %s;\n", dst, x); break;
    case D3DTOP_SELECTARG2: sb(b, "  %s = %s;\n", dst, y); break;
    case D3DTOP_MODULATE:   sb(b, "  %s = %s * %s;\n", dst, x, y); break;
    case D3DTOP_MODULATE2X: sb(b, "  %s = %s * %s * 2.0;\n", dst, x, y); break;
    case D3DTOP_MODULATE4X: sb(b, "  %s = %s * %s * 4.0;\n", dst, x, y); break;
    case D3DTOP_ADD:        sb(b, "  %s = %s + %s;\n", dst, x, y); break;
    case D3DTOP_ADDSIGNED:  sb(b, "  %s = %s + %s - 0.5;\n", dst, x, y); break;
    case D3DTOP_ADDSIGNED2X: sb(b, "  %s = (%s + %s - 0.5) * 2.0;\n", dst, x, y); break;
    case D3DTOP_SUBTRACT:   sb(b, "  %s = %s - %s;\n", dst, x, y); break;
    case D3DTOP_ADDSMOOTH:  sb(b, "  %s = %s + %s - %s * %s;\n", dst, x, y, x, y); break;
    case D3DTOP_BLENDDIFFUSEALPHA: sb(b, "  %s = mix(%s, %s, dif.a);\n", dst, y, x); break;
    case D3DTOP_BLENDTEXTUREALPHA: sb(b, "  %s = mix(%s, %s, tex.a);\n", dst, y, x); break;
    case D3DTOP_BLENDFACTORALPHA:  sb(b, "  %s = mix(%s, %s, u_tfactor.a);\n", dst, y, x); break;
    default: break;                            /* disabled: what came in goes out */
    }
}
static char *fragment_source(const prog_key *k) {
    sbuf b = { 0 };
    static const char *const cmp[] = { "", "false", "a8 < u_aref", "a8 == u_aref", "a8 <= u_aref", "a8 > u_aref", "a8 != u_aref", "a8 >= u_aref", "true" };
    sb(&b, "#version 300 es\nprecision highp float;\nuniform sampler2D u_tex;\nuniform vec4 u_tfactor;\nuniform vec4 u_fogcolor;\n"
           "uniform float u_aref;\nin vec4 v_d0;\nin vec4 v_d1;\nin vec4 v_t0;\nin float v_fog;\nout vec4 o_color;\nvoid main() {\n"
           "  vec4 dif = v_d0;\n  vec4 cur = dif;\n");
    sb(&b, k->sample ? "  vec4 tex = texture(u_tex, v_t0.xy);\n" : "  vec4 tex = vec4(1.0);\n");
    sb(&b, "  vec3 rgb = cur.rgb;\n  float a = cur.a;\n");
    stage_op(&b, "rgb", k->cop, k->ca1, k->ca2, 0);
    stage_op(&b, "a", k->aop, k->aa1, k->aa2, 1);
    sb(&b, "  cur = clamp(vec4(rgb, a), 0.0, 1.0);\n");
    if (k->atest) sb(&b, "  float a8 = floor(cur.a * 255.0 + 0.5);\n  if (!(%s)) discard;\n", cmp[k->atest]);
    if (k->spec_add) sb(&b, "  cur.rgb = min(cur.rgb + v_d1.rgb, 1.0);\n");
    if (k->fog) sb(&b, "  cur.rgb = mix(u_fogcolor.rgb, cur.rgb, clamp(v_fog, 0.0, 1.0));\n");
    sb(&b, "  o_color = cur;\n}\n");
    return b.s;
}
/* The fog factor (1: none) from a depth d. */
static void fog_lines(sbuf *b, const prog_key *k, const char *own, const char *w, const char *z, const char *eye) {
    const char *d = k->fog_by == FOG_BY_W ? w : k->fog_by == FOG_BY_Z ? z : eye;
    switch (k->fog) {
    case FOG_LINEAR: sb(b, "  v_fog = (u_fog.y - (%s)) / (u_fog.y - u_fog.x);\n", d); break;
    case FOG_EXP:    sb(b, "  v_fog = exp(-u_fog.z * (%s));\n", d); break;
    case FOG_EXP2:   sb(b, "  v_fog = exp(-(u_fog.z * (%s)) * (u_fog.z * (%s)));\n", d, d); break;
    case FOG_OWN:    sb(b, "  v_fog = %s;\n", own); break;
    default:         sb(b, "  v_fog = 1.0;\n"); break;
    }
}
#define TAIL "  gl_Position = vec4(p.x + p.w * u_view.x, p.w * u_view.y - p.y, 2.0 * p.z - p.w, p.w);\n}\n"
static char *vertex_source(const prog_key *k, const d3d9_vshader *vs) {
    sbuf b = { 0 };
    sb(&b, "#version 300 es\nprecision highp float;\nuniform vec4 u_view;\nuniform vec4 u_fog;\n"
           "out vec4 v_d0;\nout vec4 v_d1;\nout vec4 v_t0;\nout float v_fog;\n");
    if (k->mode == MODE_SHADER) {
        sb(&b, "uniform vec4 c[%d];\n", vs->info.consts > 0 ? vs->info.consts : 1);
        for (int i = 0; i < 16; i++) if (vs->info.in[i].declared) sb(&b, "layout(location = %d) in vec4 a_v%d;\n", i, i);
        sb(&b, "void main() {\n");
        for (int i = 0; i < 16; i++)
            if (vs->info.in[i].declared) sb(&b, "  vec4 v%d = a_v%d%s;\n", i, i, (k->colors >> i) & 1 ? ".zyxw" : "");
            else sb(&b, "  vec4 v%d = vec4(0.0, 0.0, 0.0, 1.0);\n", i);
        sb(&b, "  vec4 oPos = vec4(0.0, 0.0, 0.0, 1.0), oD0 = vec4(1.0), oD1 = vec4(0.0), oFog = vec4(1.0), oPts = vec4(1.0);\n");
        for (int i = 0; i < 8; i++) sb(&b, "  vec4 oT%d = vec4(0.0, 0.0, 0.0, 1.0);\n", i);
        sb(&b, "%s\n", vs->glsl);
        sb(&b, "  vec4 p = oPos;\n  v_d0 = clamp(oD0, 0.0, 1.0);\n  v_d1 = clamp(oD1, 0.0, 1.0);\n  v_t0 = oT0;\n");
        fog_lines(&b, k, "oFog.x", "p.w", "p.z / p.w", "p.w");
        sb(&b, TAIL);
        return b.s;
    }
    sb(&b, "layout(location = 0) in vec4 a_pos;\nlayout(location = 1) in vec3 a_normal;\nlayout(location = 2) in vec4 a_d0;\n"
           "layout(location = 3) in vec4 a_d1;\nlayout(location = 4) in vec4 a_t0;\n");
    const char *d0 = k->diffuse ? "a_d0.zyxw" : "vec4(1.0)", *d1 = k->specular ? "a_d1.zyxw" : "vec4(0.0, 0.0, 0.0, 1.0)";
    if (k->mode == MODE_RHW) {
        /* Already in pixels: the viewport is undone here so that GL's own puts
         * them back, and w is what the vertex says it was. */
        sb(&b, "void main() {\n  float w = a_pos.w != 0.0 ? 1.0 / a_pos.w : 1.0;\n"
               "  vec2 q = (a_pos.xy + 0.5 - u_view.zw) * u_view.xy - 1.0;\n"
               "  v_d0 = %s;\n  v_d1 = %s;\n  v_t0 = %s;\n", d0, d1, k->tex ? "a_t0" : "vec4(0.0, 0.0, 0.0, 1.0)");
        fog_lines(&b, k, "v_d1.a", "w", "a_pos.z", "w");
        sb(&b, "  gl_Position = vec4(q * w, (a_pos.z * 2.0 - 1.0) * w, w);\n}\n");
        return b.s;
    }
    sb(&b, "uniform mat4 u_wvp;\nuniform mat4 u_wv;\nuniform vec4 u_mat[4];\nuniform vec4 u_amb;\nuniform vec4 u_lpos[4];\n"
           "uniform vec4 u_ldir[4];\nuniform vec4 u_ldif[4];\nuniform vec4 u_lamb[4];\nuniform vec4 u_latt[4];\nuniform vec4 u_lspot[4];\n"
           "void main() {\n  vec4 pos = vec4(a_pos.xyz, 1.0);\n  vec4 p = u_wvp * pos;\n  vec3 eye = (u_wv * pos).xyz;\n");
    if (!k->lighting) sb(&b, "  v_d0 = %s;\n  v_d1 = %s;\n", d0, d1);
    else {
        sb(&b, "  vec3 n = %s;\n", k->normal ? "normalize(mat3(u_wv) * a_normal)" : "vec3(0.0)");
        sb(&b, "  vec4 dsrc = %s;\n  vec4 asrc = %s;\n", k->dsrc == D3DMCS_COLOR1 ? d0 : k->dsrc == D3DMCS_COLOR2 ? d1 : "u_mat[0]",
           k->asrc == D3DMCS_COLOR1 ? d0 : k->asrc == D3DMCS_COLOR2 ? d1 : "u_mat[1]");
        sb(&b, "  vec3 col = u_mat[3].rgb + asrc.rgb * u_amb.rgb;\n");
        for (int i = 0; i < 4; i++) {
            if (k->light[i] == D3DLIGHT_DIRECTIONAL)
                sb(&b, "  col += asrc.rgb * u_lamb[%d].rgb + dsrc.rgb * u_ldif[%d].rgb * max(dot(n, -u_ldir[%d].xyz), 0.0);\n", i, i, i);
            else if (k->light[i]) {
                sb(&b, "  {\n    vec3 to = u_lpos[%d].xyz - eye;\n    float dist = length(to);\n    vec3 l = to / max(dist, 0.000001);\n"
                       "    float att = dist > u_latt[%d].x ? 0.0 : 1.0 / (u_latt[%d].y + u_latt[%d].z * dist + u_latt[%d].w * dist * dist);\n",
                   i, i, i, i, i);
                if (k->light[i] == D3DLIGHT_SPOT)
                    sb(&b, "    float rho = dot(-l, u_ldir[%d].xyz);\n"
                           "    att *= rho > u_lspot[%d].x ? 1.0 : rho <= u_lspot[%d].y ? 0.0 : "
                           "pow((rho - u_lspot[%d].y) / (u_lspot[%d].x - u_lspot[%d].y), u_lspot[%d].z);\n", i, i, i, i, i, i, i);
                sb(&b, "    col += att * (asrc.rgb * u_lamb[%d].rgb + dsrc.rgb * u_ldif[%d].rgb * max(dot(n, l), 0.0));\n  }\n", i, i);
            }
        }
        sb(&b, "  v_d0 = clamp(vec4(col, dsrc.a), 0.0, 1.0);\n  v_d1 = vec4(0.0, 0.0, 0.0, 1.0);\n");
    }
    sb(&b, "  v_t0 = %s;\n", k->tex ? "a_t0" : "vec4(0.0, 0.0, 0.0, 1.0)");
    fog_lines(&b, k, "v_d1.a", "p.w", "p.z / p.w", "abs(eye.z)");
    sb(&b, TAIL);
    return b.s;
}
static GLuint compile(GLenum kind, const char *src) {
    DOING(kind == GL_VERTEX_SHADER ? "compiling a vertex program" : "compiling a pixel program");
    GLuint s = glCreateShader(kind);
    GLint ok = 0;
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "[d3d9] a %s program did not compile:\n%s\n%s\n", kind == GL_VERTEX_SHADER ? "vertex" : "pixel", log, src);
        glDeleteShader(s);
        return 0;
    }
    return s;
}
static prog *program_new(const prog_key *k, const d3d9_vshader *vs);
static prog *program_for(const prog_key *k, const d3d9_vshader *vs) {      /* timed when one has to be made */
    static prog *last;                         /* a draw mostly wants the program of the draw before */
    if (last && !memcmp(&last->key, k, sizeof *k)) return last;
    for (int i = 0; i < g_nprog; i++) if (!memcmp(&g_prog[i].key, k, sizeof *k)) return last = &g_prog[i];
    uint64_t t0 = hle_now_ns();
    prog *p = program_new(k, vs);
    hle_perf.program_ns += hle_now_ns() - t0, hle_perf.programs++;
    return p;
}
static prog *program_new(const prog_key *k, const d3d9_vshader *vs) {
    if (g_nprog >= 512) return NULL;
    prog *p = &g_prog[g_nprog++];
    memset(p, 0, sizeof *p);
    memcpy(&p->key, k, sizeof *k);             /* with its padding: keys are compared as bytes */
    char *vsrc = vertex_source(k, vs), *fsrc = fragment_source(k);
    GLuint v = compile(GL_VERTEX_SHADER, vsrc), f = compile(GL_FRAGMENT_SHADER, fsrc);
    if (g_trace) fprintf(stderr, "[d3d9] program %d: mode %d, shader %d, declaration %d\n%s\n%s\n", g_nprog, k->mode, k->vs, k->decl, vsrc, fsrc);
    free(vsrc), free(fsrc);
    if (!v || !f) return p;
    GLint ok = 0;
    DOING("linking a program");
    p->id = glCreateProgram();
    glAttachShader(p->id, v), glAttachShader(p->id, f);
    glLinkProgram(p->id);
    glGetProgramiv(p->id, GL_LINK_STATUS, &ok);
    glDeleteShader(v), glDeleteShader(f);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(p->id, sizeof log, NULL, log);
        fprintf(stderr, "[d3d9] a program did not link: %s\n", log);
        glDeleteProgram(p->id), p->id = 0;
        return p;
    }
    glUseProgram(p->id), g_cur_prog = p->id;
    glUniform1i(glGetUniformLocation(p->id, "u_tex"), 0);
#define LOC(field, name) p->field = glGetUniformLocation(p->id, name)
    LOC(view, "u_view"), LOC(fog, "u_fog"), LOC(fogcolor, "u_fogcolor"), LOC(aref, "u_aref"), LOC(tfactor, "u_tfactor");
    LOC(wvp, "u_wvp"), LOC(wv, "u_wv"), LOC(c, "c"), LOC(mat, "u_mat"), LOC(amb, "u_amb"), LOC(lpos, "u_lpos"), LOC(ldir, "u_ldir");
    LOC(ldif, "u_ldif"), LOC(lamb, "u_lamb"), LOC(latt, "u_latt"), LOC(lspot, "u_lspot");
#undef LOC
    p->nconst = vs ? vs->info.consts : 0;
    return p;
}

/* a * b, both as Direct3D keeps them (rows, vectors on the left) */
static void mat_mul(float *r, const float *a, const float *b) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
}

/* Texture stage 0 as it will really be worked: a stage that names a texture
 * when none is set passes on what came in, as Direct3D does. */
static void settle_stage(prog_key *k, int has_tex) {
    const uint32_t *t = d3d9.tss[0];
    int cop = (int)t[D3DTSS_COLOROP], aop = (int)t[D3DTSS_ALPHAOP];
    int ca1 = (int)t[D3DTSS_COLORARG1], ca2 = (int)t[D3DTSS_COLORARG2], aa1 = (int)t[D3DTSS_ALPHAARG1], aa2 = (int)t[D3DTSS_ALPHAARG2];
    if (cop == D3DTOP_DISABLE) { k->cop = k->aop = D3DTOP_DISABLE; return; }
#define IS_TEX(a) (((a) & D3DTA_SELECTMASK) == D3DTA_TEXTURE)
    if (!has_tex) {
        if ((IS_TEX(ca1) && cop != D3DTOP_SELECTARG2) || (IS_TEX(ca2) && cop != D3DTOP_SELECTARG1)) cop = D3DTOP_SELECTARG1, ca1 = D3DTA_CURRENT;
        if ((IS_TEX(aa1) && aop != D3DTOP_SELECTARG2) || (IS_TEX(aa2) && aop != D3DTOP_SELECTARG1)) aop = D3DTOP_SELECTARG1, aa1 = D3DTA_CURRENT;
    }
    int uses = 0;
    if (cop == D3DTOP_SELECTARG1) ca2 = D3DTA_CURRENT; else if (cop == D3DTOP_SELECTARG2) ca1 = D3DTA_CURRENT;
    if (aop == D3DTOP_SELECTARG1) aa2 = D3DTA_CURRENT; else if (aop == D3DTOP_SELECTARG2) aa1 = D3DTA_CURRENT;
    if (aop == D3DTOP_DISABLE) aa1 = aa2 = D3DTA_CURRENT;
    uses = IS_TEX(ca1) || IS_TEX(ca2) || (aop != D3DTOP_DISABLE && (IS_TEX(aa1) || IS_TEX(aa2))) ||
           cop == D3DTOP_BLENDTEXTUREALPHA || aop == D3DTOP_BLENDTEXTUREALPHA;
#undef IS_TEX
    k->cop = (uint8_t)cop, k->ca1 = (uint8_t)ca1, k->ca2 = (uint8_t)ca2, k->aop = (uint8_t)aop, k->aa1 = (uint8_t)aa1, k->aa2 = (uint8_t)aa2;
    k->sample = (uint8_t)(uses && has_tex);
}

static void set_array(unsigned loc, const elem *e, int stride, const uint8_t *base) {
    static const struct { int n; GLenum type; int norm; } t[] = {
        { 1, GL_FLOAT, 0 }, { 2, GL_FLOAT, 0 }, { 3, GL_FLOAT, 0 }, { 4, GL_FLOAT, 0 }, { 4, GL_UNSIGNED_BYTE, 1 },
        { 4, GL_UNSIGNED_BYTE, 0 }, { 2, GL_SHORT, 0 }, { 4, GL_SHORT, 0 }, { 4, GL_UNSIGNED_BYTE, 1 }, { 2, GL_SHORT, 1 },
        { 4, GL_SHORT, 1 }, { 2, GL_UNSIGNED_SHORT, 1 }, { 4, GL_UNSIGNED_SHORT, 1 },
    };
    if (e->type > D3DDECLTYPE_USHORT4N) return;
    glVertexAttribPointer(loc, t[e->type].n, t[e->type].type, (GLboolean)t[e->type].norm, stride,
                          (const void *)((uintptr_t)base + (uintptr_t)e->offset));
}
static void enable_arrays(unsigned want) {
    if (want == g_enabled) return;
    for (unsigned i = 0; i < 16; i++) {
        unsigned bit = 1u << i;
        if ((want & bit) && !(g_enabled & bit)) glEnableVertexAttribArray(i);
        else if (!(want & bit) && (g_enabled & bit)) { glDisableVertexAttribArray(i); glVertexAttrib4f(i, 0.0f, 0.0f, 0.0f, 1.0f); }
    }
    g_enabled = want;
}

/* Vertices and indices that are sent with their draw go one after another
 * into a buffer that is thrown away for a fresh one only when it is full: a
 * buffer a draw, allocated and filled, was most of the driver's work for
 * small draws. A range is written through a mapping that asks for no
 * waiting (nothing still being drawn is overwritten). */
typedef struct { GLenum target; size_t room, cap, at; } stream;
static stream g_vstream = { GL_ARRAY_BUFFER, 4u << 20, 0, 0 }, g_istream = { GL_ELEMENT_ARRAY_BUFFER, 1u << 20, 0, 0 };
static size_t stream_put(stream *st, const void *src, size_t n, size_t align) {
    size_t at = (st->at + align - 1) / align * align;
    if (!st->cap || at + n > st->cap) {
        st->cap = n > st->room ? n : st->room;
        glBufferData(st->target, (GLsizeiptr)st->cap, NULL, GL_STREAM_DRAW);
        at = 0;
    }
    void *to = glMapBufferRange(st->target, (GLintptr)at, (GLsizeiptr)n, GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
    if (to) memcpy(to, src, n), glUnmapBuffer(st->target);
    else glBufferSubData(st->target, (GLintptr)at, (GLsizeiptr)n, src);
    st->at = at + n;
    return at;
}

/* HLE_D3D9_CENSUS=N: what the draws of every Nth frame were, in a few lines of the
 * log: how many of each kind, how large, and how many came with the same
 * program, texture and state as the draw before them (the ones that could
 * be drawn together). */
static struct {
    unsigned n, rhw, fixed, shader, user, dynamic, kept, indexed, upto4, upto16, upto64, more, lists, strips, fans, other;
    unsigned same_state, same_all, textured, sends, sent;
    uint64_t last_state, last_all;
} g_census;
static uint64_t mix(uint64_t h, uint64_t v) { return (h ^ v) * 1099511628211ull; }
static void census_draw(const d3d9_draw *d, const prog *p, int mode, GLuint tex, int streamed) {
    static const int which[] = { D3DRS_ZENABLE, D3DRS_ZFUNC, D3DRS_ZWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND,
                                 D3DRS_BLENDOP, D3DRS_CULLMODE, D3DRS_COLORWRITEENABLE, D3DRS_SCISSORTESTENABLE, D3DRS_ALPHAREF,
                                 D3DRS_TEXTUREFACTOR, D3DRS_FOGSTART, D3DRS_FOGEND, D3DRS_FOGCOLOR };
    uint64_t h = mix(mix(1469598103934665603ull, p->id), tex);
    for (unsigned i = 0; i < sizeof which / sizeof which[0]; i++) h = mix(h, d3d9.rs[which[i]]);
    h = mix(mix(mix(mix(h, d3d9.vp.x), d3d9.vp.y), d3d9.vp.w), d3d9.vp.h);
    h = mix(mix(h, (uintptr_t)d3d9.rt), (uintptr_t)d3d9.ds);
    h = mix(h, (uint64_t)d->prim);
    uint64_t all = h;
    if (mode == MODE_FIXED) {
        const uint32_t *w = (const uint32_t *)d3d9.world, *v = (const uint32_t *)d3d9.view, *pr = (const uint32_t *)d3d9.proj;
        for (int i = 0; i < 16; i++) all = mix(mix(mix(all, w[i]), v[i]), pr[i]);
    } else if (mode == MODE_SHADER) all = mix(all, d3d9.vsc_serial);
    g_census.n++;
    if (mode == MODE_RHW) g_census.rhw++; else if (mode == MODE_FIXED) g_census.fixed++; else g_census.shader++;
    if (d->user_verts) g_census.user++; else if (streamed) g_census.dynamic++; else g_census.kept++;
    if (d->indexed) g_census.indexed++;
    if (tex) g_census.textured++;
    if (d->num_vertices <= 4) g_census.upto4++; else if (d->num_vertices <= 16) g_census.upto16++; else if (d->num_vertices <= 64) g_census.upto64++; else g_census.more++;
    if (d->prim == D3DPT_TRIANGLELIST) g_census.lists++; else if (d->prim == D3DPT_TRIANGLESTRIP) g_census.strips++;
    else if (d->prim == D3DPT_TRIANGLEFAN) g_census.fans++; else g_census.other++;
    if (g_census.n > 1 && h == g_census.last_state) g_census.same_state++;
    if (g_census.n > 1 && all == g_census.last_all) g_census.same_all++;
    g_census.last_state = h, g_census.last_all = all;
}
static void census_say(void) {
    fprintf(stderr, "[d3d9] frame %u: %u draws; already transformed %u, fixed function %u, vertex shader %u; with a texture %u\n",
            d3d9.frame, g_census.n, g_census.rhw, g_census.fixed, g_census.shader, g_census.textured);
    fprintf(stderr, "[d3d9]   vertices with the draw %u, from a buffer the game rewrites %u, from one GL keeps %u; indexed %u\n",
            g_census.user, g_census.dynamic, g_census.kept, g_census.indexed);
    fprintf(stderr, "[d3d9]   up to 4 vertices %u, up to 16 %u, up to 64 %u, more %u; lists %u, strips %u, fans %u, lines and points %u\n",
            g_census.upto4, g_census.upto16, g_census.upto64, g_census.more, g_census.lists, g_census.strips, g_census.fans, g_census.other);
    fprintf(stderr, "[d3d9]   as the draw before: program, texture and state %u; and matrices or constants too %u\n",
            g_census.same_state, g_census.same_all);
    fprintf(stderr, "[d3d9]   a shader's constants sent %u times, %u of them each time\n",
            g_census.sends, g_census.sends ? g_census.sent / g_census.sends : 0);
    memset(&g_census, 0, sizeof g_census);
}

/* What a vertex is made of and which of it a program reads, worked out once
 * for each pairing of a layout (a declaration, or an FVF code) with a vertex
 * shader (or none) and kept: for every draw it was forty searches of the
 * layout, in a frame of nine hundred draws. */
typedef struct {
    int32_t decl;                   /* the declaration's number, or minus the FVF code */
    const d3d9_vshader *vs;
    int32_t vs_id;
    int nel;
    elem el[40];
    const elem *pos, *post, *d0, *d1, *n, *t0;
    const elem *arr[16];            /* what each of the program's inputs reads; NULL: nothing */
    unsigned want, colors;          /* the inputs that read something; those that read a D3DCOLOR */
    uint64_t sig;                   /* of what the inputs read: the same layout by another name has the same */
} layout;
static layout g_layout[64];
static int g_nlayout;
static const layout *layout_for(const d3d9_vshader *vs) {
    static const layout *last;
    static unsigned turn;
    int32_t decl = d3d9.decl ? d3d9.decl->id : -(int32_t)(d3d9.fvf & 0x7FFFFFFF), vs_id = vs ? vs->id : 0;
    if (last && last->decl == decl && last->vs == vs && last->vs_id == vs_id) return last;
    for (int i = 0; i < g_nlayout; i++)
        if (g_layout[i].decl == decl && g_layout[i].vs == vs && g_layout[i].vs_id == vs_id) return last = &g_layout[i];
    layout *l = &g_layout[g_nlayout < 64 ? g_nlayout++ : (int)(turn++ & 63)];      /* full: the seats are used again in turn */
    memset(l, 0, sizeof *l);
    l->decl = decl, l->vs = vs, l->vs_id = vs_id;
    if (d3d9.decl) {
        for (int i = 0; i < d3d9.decl->n && l->nel < 40; i++)
            if (d3d9.decl->el[i].stream == 0)
                l->el[l->nel++] = (elem){ d3d9.decl->el[i].offset, d3d9.decl->el[i].type, d3d9.decl->el[i].usage, d3d9.decl->el[i].index };
    } else l->nel = elems_from_fvf(d3d9.fvf, l->el);
    l->pos = find_elem(l->el, l->nel, 0, 0), l->post = find_elem(l->el, l->nel, 9, 0);
    l->d0 = find_elem(l->el, l->nel, 10, 0), l->d1 = find_elem(l->el, l->nel, 10, 1);
    l->n = find_elem(l->el, l->nel, 3, 0), l->t0 = find_elem(l->el, l->nel, 5, 0);
    if (vs) {
        for (int i = 0; i < 16; i++)
            l->arr[i] = vs->info.in[i].declared ? find_elem(l->el, l->nel, vs->info.in[i].usage, vs->info.in[i].usage_index) : NULL;
    } else l->arr[0] = l->post ? l->post : l->pos, l->arr[1] = l->n, l->arr[2] = l->d0, l->arr[3] = l->d1, l->arr[4] = l->t0;
    l->sig = 1469598103934665603ull;
    for (unsigned i = 0; i < 16; i++) {
        if (!l->arr[i]) continue;
        l->want |= 1u << i;
        if (l->arr[i]->type == D3DDECLTYPE_D3DCOLOR) l->colors |= 1u << i;
        l->sig = mix(l->sig, (uint64_t)i << 24 | (uint64_t)l->arr[i]->offset << 8 | (uint64_t)l->arr[i]->type);
    }
    return last = l;
}

static void draw_one(const d3d9_draw *d);
void d3d9_gl_draw(const d3d9_draw *d) {
    hle_perf.draws++;
    if (!hle_perf_on) { draw_one(d); return; }
    uint64_t t0 = hle_now_ns();
    draw_one(d);
    hle_perf.draw_ns += hle_now_ns() - t0;
}
#define PHASE(i) do { if (tp) { uint64_t now_ = hle_now_ns(); hle_perf.phase_ns[i] += now_ - tp, tp = now_; } } while (0)
static void draw_one(const d3d9_draw *d) {
    static const GLenum mode[] = { 0, GL_POINTS, GL_LINES, GL_LINE_STRIP, GL_TRIANGLES, GL_TRIANGLE_STRIP, GL_TRIANGLE_FAN };
    uint64_t tp = hle_perf_phases ? hle_now_ns() : 0;
    const uint32_t *rs = d3d9.rs;
    if (!gl_enter() || d->prim < 1 || d->prim > 6) return;

    /* what a vertex is made of */
    d3d9_vshader *vs = d3d9.vs;
    if (vs && !vs->glsl) return;
    const layout *lay = layout_for(vs);
    int stride = d->user_verts ? d->user_stride : (int)d3d9.vb_stride;
    if (!lay->nel || stride <= 0 || (!d->user_verts && !d3d9.vb)) return;

    /* the program for this draw's state */
    prog_key k;
    memset(&k, 0, sizeof k);
    const elem *e_pos = lay->pos, *e_post = lay->post, *e_d0 = lay->d0, *e_d1 = lay->d1, *e_n = lay->n, *e_t0 = lay->t0;
    d3d9_texture *tex = d3d9.tex[0];
    settle_stage(&k, tex != NULL);
    int perspective = d3d9.proj[3] != 0.0f || d3d9.proj[7] != 0.0f || d3d9.proj[11] != 0.0f || d3d9.proj[15] != 1.0f;
    if (rs[D3DRS_FOGENABLE]) {
        uint32_t table = rs[D3DRS_FOGTABLEMODE], vertex = rs[D3DRS_FOGVERTEXMODE];
        uint32_t m = table ? table : vertex;
        k.fog = (uint8_t)(m == D3DFOG_LINEAR ? FOG_LINEAR : m == D3DFOG_EXP ? FOG_EXP : m == D3DFOG_EXP2 ? FOG_EXP2 : FOG_OWN);
        k.fog_by = (uint8_t)(table ? (perspective ? FOG_BY_W : FOG_BY_Z) : FOG_BY_EYE);
        if (vs && !table) k.fog = FOG_OWN;     /* a vertex shader's fog is the number it writes */
    }
    k.atest = (uint8_t)(rs[D3DRS_ALPHATESTENABLE] ? (rs[D3DRS_ALPHAFUNC] >= 1 && rs[D3DRS_ALPHAFUNC] <= 7 ? rs[D3DRS_ALPHAFUNC] : 0) : 0);
    k.spec_add = (uint8_t)(rs[D3DRS_SPECULARENABLE] != 0);
    if (vs) {
        k.mode = MODE_SHADER, k.vs = vs->id, k.decl = lay->decl;
        k.colors = lay->colors;
    } else {
        if (!e_pos && !e_post) return;
        k.mode = (uint8_t)(e_post ? MODE_RHW : MODE_FIXED);
        k.diffuse = e_d0 != NULL, k.specular = e_d1 != NULL, k.tex = e_t0 != NULL, k.normal = e_n != NULL;
        if (k.mode == MODE_FIXED && rs[D3DRS_LIGHTING]) {
            k.lighting = 1;
            for (int i = 0; i < 4; i++) k.light[i] = (uint8_t)(d3d9.light[i].enabled ? d3d9.light[i].type : 0);
            int cv = rs[D3DRS_COLORVERTEX] != 0;
            k.dsrc = (uint8_t)(cv ? rs[D3DRS_DIFFUSEMATERIALSOURCE] : D3DMCS_MATERIAL);
            k.asrc = (uint8_t)(cv ? rs[D3DRS_AMBIENTMATERIALSOURCE] : D3DMCS_MATERIAL);
            if ((k.dsrc == D3DMCS_COLOR1 && !e_d0) || (k.dsrc == D3DMCS_COLOR2 && !e_d1)) k.dsrc = D3DMCS_MATERIAL;
            if ((k.asrc == D3DMCS_COLOR1 && !e_d0) || (k.asrc == D3DMCS_COLOR2 && !e_d1)) k.asrc = D3DMCS_MATERIAL;
        }
    }
    DOING("finding the program");
    prog *p = program_for(&k, vs);
    if (!p || !p->id) return;

    DOING("filling a texture");
    GLuint gltex = k.sample ? sampled_texture(tex) : 0;      /* before the state: making one may disturb it */
    PHASE(0);
    DOING("setting the state");
    apply_state();
    if (g_cur_prog != p->id) glUseProgram(p->id), g_cur_prog = p->id;
    if (k.sample) {
        bind_texture(gltex);
        apply_sampler(tex);
    }

    PHASE(1);
    DOING("setting uniforms");
    /* uniforms: each only when it is not what this program was last sent */
    float vw = (float)(d3d9.vp.w ? d3d9.vp.w : 1), vh = (float)(d3d9.vp.h ? d3d9.vp.h : 1), f[8];
    if (k.mode == MODE_RHW) f[0] = 2.0f / vw, f[1] = 2.0f / vh, f[2] = (float)d3d9.vp.x, f[3] = (float)d3d9.vp.y;
    else f[0] = 1.0f / vw, f[1] = 1.0f / vh, f[2] = f[3] = 0.0f;
    if (!(p->sent & SENT_VIEW) || memcmp(p->u_view, f, 16)) glUniform4fv(p->view, 1, f), memcpy(p->u_view, f, 16), p->sent |= SENT_VIEW;
    if (k.fog) {
        f[0] = rs_float(D3DRS_FOGSTART), f[1] = rs_float(D3DRS_FOGEND), f[2] = rs_float(D3DRS_FOGDENSITY), f[3] = 0.0f;
        color_of(rs[D3DRS_FOGCOLOR], f + 4);
        if (!(p->sent & SENT_FOG) || memcmp(p->u_fog, f, 32))
            glUniform4fv(p->fog, 1, f), glUniform4fv(p->fogcolor, 1, f + 4), memcpy(p->u_fog, f, 32), p->sent |= SENT_FOG;
    }
    if (k.atest) {
        float ref = (float)(rs[D3DRS_ALPHAREF] & 255);
        if (!(p->sent & SENT_AREF) || p->u_aref != ref) glUniform1f(p->aref, ref), p->u_aref = ref, p->sent |= SENT_AREF;
    }
    if (p->tfactor >= 0 && (!(p->sent & SENT_TFACTOR) || p->u_tfactor != rs[D3DRS_TEXTUREFACTOR])) {
        color_of(rs[D3DRS_TEXTUREFACTOR], f);
        glUniform4fv(p->tfactor, 1, f);
        p->u_tfactor = rs[D3DRS_TEXTUREFACTOR], p->sent |= SENT_TFACTOR;
    }
    if (k.mode == MODE_SHADER) {
        if (p->c >= 0 && p->nconst > 0 && p->consts != d3d9.vsc_serial + 1) {
            if (vs->info.ndefs) {              /* constants the shader defines itself win over the device's */
                static float c[256][4];
                memcpy(c, d3d9.vsc, sizeof c);
                for (int i = 0; i < vs->info.ndefs; i++) if (vs->info.def[i].reg >= 0 && vs->info.def[i].reg < 256) memcpy(c[vs->info.def[i].reg], vs->info.def[i].v, 16);
                glUniform4fv(p->c, p->nconst > 256 ? 256 : p->nconst, &c[0][0]);
            } else glUniform4fv(p->c, p->nconst > 256 ? 256 : p->nconst, &d3d9.vsc[0][0]);
            p->consts = d3d9.vsc_serial + 1;
            if (g_census_frame && (d3d9.frame + 1) % g_census_frame == 0) g_census.sends++, g_census.sent += (unsigned)(p->nconst > 256 ? 256 : p->nconst);
        }
    } else if (k.mode == MODE_FIXED) {
        float wv[16], wvp[16];
        mat_mul(wv, d3d9.world, d3d9.view);
        mat_mul(wvp, wv, d3d9.proj);
        if (!(p->sent & SENT_WVP) || memcmp(p->u_wvp, wvp, 64))        /* rows here are columns there: the vector moves to the right */
            glUniformMatrix4fv(p->wvp, 1, GL_FALSE, wvp), memcpy(p->u_wvp, wvp, 64), p->sent |= SENT_WVP;
        if (p->wv >= 0 && (!(p->sent & SENT_WV) || memcmp(p->u_wv, wv, 64)))
            glUniformMatrix4fv(p->wv, 1, GL_FALSE, wv), memcpy(p->u_wv, wv, 64), p->sent |= SENT_WV;
        if (k.lighting) {
            float lp[4][4] = { { 0 } }, ld[4][4] = { { 0 } }, ldf[4][4] = { { 0 } }, la[4][4] = { { 0 } }, lat[4][4] = { { 0 } }, ls[4][4] = { { 0 } };
            const float *V = d3d9.view;
            for (int i = 0; i < 4; i++) {
                const float *v = d3d9.light[i].v;
                if (!k.light[i]) continue;
                memcpy(ldf[i], v, 16), memcpy(la[i], v + 8, 16);
                for (int j = 0; j < 3; j++) {
                    lp[i][j] = v[12] * V[j] + v[13] * V[4 + j] + v[14] * V[8 + j] + V[12 + j];
                    ld[i][j] = v[15] * V[j] + v[16] * V[4 + j] + v[17] * V[8 + j];
                }
                float len = sqrtf(ld[i][0] * ld[i][0] + ld[i][1] * ld[i][1] + ld[i][2] * ld[i][2]);
                if (len > 0.0f) ld[i][0] /= len, ld[i][1] /= len, ld[i][2] /= len;
                lat[i][0] = v[18], lat[i][1] = v[20], lat[i][2] = v[21], lat[i][3] = v[22];
                ls[i][0] = cosf(v[23] * 0.5f), ls[i][1] = cosf(v[24] * 0.5f), ls[i][2] = v[19];
            }
            glUniform4fv(p->mat, 4, d3d9.material);
            color_of(rs[D3DRS_AMBIENT], f);
            glUniform4fv(p->amb, 1, f);
            glUniform4fv(p->lpos, 4, &lp[0][0]), glUniform4fv(p->ldir, 4, &ld[0][0]), glUniform4fv(p->ldif, 4, &ldf[0][0]);
            glUniform4fv(p->lamb, 4, &la[0][0]), glUniform4fv(p->latt, 4, &lat[0][0]), glUniform4fv(p->lspot, 4, &ls[0][0]);
        }
    }

    /* Vertices. A buffer the guest keeps rewriting, and vertices handed over
     * with the draw, are sent as just the vertices this draw uses, into the
     * stream; any other buffer is GL's own copy. Either way the arrays stay
     * pointed at the buffer's start and the draw says which vertex is its
     * first, so that draw after draw from one buffer says nothing of arrays. */
    PHASE(2);
    DOING("sending vertices");
    int nidx = d->indexed ? (d->prim == D3DPT_TRIANGLELIST ? d->nprims * 3 : d->prim == D3DPT_LINELIST ? d->nprims * 2 :
                             d->prim == D3DPT_POINTLIST ? d->nprims : d->prim == D3DPT_LINESTRIP ? d->nprims + 1 : d->nprims + 2) : 0;
    d3d9_buffer *vb = d3d9.vb, *ib = d3d9.ib;
    int streamed = d->user_verts != NULL || (vb->usage & D3DUSAGE_DYNAMIC) || vb->pool == D3DPOOL_SYSTEMMEM;
    int first_vertex = d->indexed ? d->base + d->min_vertex : d->first;
    if (d->indexed && !d->user_index && !ib) return;
    if (g_census_frame && (d3d9.frame + 1) % g_census_frame == 0) census_draw(d, p, k.mode, gltex, streamed);
    GLuint abuf;                               /* the buffer the arrays read */
    size_t abase;                              /* where its vertex 0 is */
    int first;                                 /* the first vertex of this draw, or what an index of 0 means */
    if (streamed) {
        const uint8_t *src = d->user_verts ? d->user_verts + (size_t)(d->indexed ? d->min_vertex : 0) * (size_t)stride
                                           : (const uint8_t *)G2H(vb->data) + d3d9.vb_offset + (size_t)first_vertex * (size_t)stride;
        size_t bytes = (size_t)d->num_vertices * (size_t)stride;
        if (!d->user_verts && d3d9.vb_offset + (size_t)(first_vertex + d->num_vertices) * (size_t)stride > (size_t)vb->size) return;
        if (g_cur_vb != g_stream_vb) glBindBuffer(GL_ARRAY_BUFFER, g_stream_vb), g_cur_vb = g_stream_vb;
        abuf = g_stream_vb, abase = 0;
        first = (int)(stream_put(&g_vstream, src, bytes, (size_t)stride) / (size_t)stride);
    } else {
        if (!vb->gl) glGenBuffers(1, &vb->gl), vb->dirty = 1;
        if (g_cur_vb != vb->gl) glBindBuffer(GL_ARRAY_BUFFER, vb->gl), g_cur_vb = vb->gl;
        if (vb->dirty) glBufferData(GL_ARRAY_BUFFER, vb->size, G2H(vb->data), GL_STATIC_DRAW), vb->dirty = 0;
        abuf = vb->gl;
        if (d->indexed) abase = d3d9.vb_offset + (size_t)d->base * (size_t)stride, first = 0;     /* its indices count from there */
        else abase = d3d9.vb_offset, first = d->first;
    }
    if (g_arr_buf != abuf || g_arr_base != abase || g_arr_stride != stride || g_arr_sig != lay->sig) {
        for (unsigned i = 0; i < 16; i++) if (lay->arr[i]) set_array(i, lay->arr[i], stride, (const uint8_t *)abase);
        g_arr_buf = abuf, g_arr_base = abase, g_arr_stride = stride, g_arr_sig = lay->sig;
    }
    enable_arrays(lay->want);

    PHASE(3);
    DOING("the draw call");
    if (!d->indexed) {
        glDrawArrays(mode[d->prim], first, d->num_vertices);
    } else if (streamed || d->user_index || (ib->usage & D3DUSAGE_DYNAMIC) || ib->pool == D3DPOOL_SYSTEMMEM) {
        /* The indices are sent with the draw too: as 32-bit ones, each moved
         * to where this draw's vertices are in the stream. */
        int bytes = d->user_index ? d->index_bytes : ib->fmt == D3DFMT_INDEX32 ? 4 : 2;
        const uint8_t *src = d->user_index ? d->user_index : (const uint8_t *)G2H(ib->data) + (size_t)d->first * (size_t)bytes;
        uint32_t move = (uint32_t)first - (uint32_t)(streamed ? d->min_vertex : 0);
        if (!d->user_index && (size_t)(d->first + nidx) * (size_t)bytes > (size_t)ib->size) return;
        uint32_t *tmp = (uint32_t *)(void *)scratch((size_t)nidx * 4);
        if (!tmp) return;
        if (bytes == 2) for (int i = 0; i < nidx; i++) { uint16_t v; memcpy(&v, src + 2 * i, 2); tmp[i] = (uint32_t)v + move; }
        else for (int i = 0; i < nidx; i++) { uint32_t v; memcpy(&v, src + 4 * i, 4); tmp[i] = v + move; }
        if (g_cur_ib != g_stream_ib) glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_stream_ib), g_cur_ib = g_stream_ib;
        size_t at = stream_put(&g_istream, tmp, (size_t)nidx * 4, 4);
        glDrawElements(mode[d->prim], nidx, GL_UNSIGNED_INT, (const void *)(uintptr_t)at);
    } else {
        int bytes = ib->fmt == D3DFMT_INDEX32 ? 4 : 2;
        if (!ib->gl) glGenBuffers(1, &ib->gl), ib->dirty = 1;
        if (g_cur_ib != ib->gl) glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib->gl), g_cur_ib = ib->gl;
        if (ib->dirty) glBufferData(GL_ELEMENT_ARRAY_BUFFER, ib->size, G2H(ib->data), GL_STATIC_DRAW), ib->dirty = 0;
        glDrawElements(mode[d->prim], nidx, bytes == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT, (const void *)(uintptr_t)((size_t)d->first * (size_t)bytes));
    }
    PHASE(4);
    g_draws++;
    DOING("nothing");
}
#undef PHASE

/* ---- the picture, shown and saved --------------------------------------------------- */
void d3d9_gl_present(void) {
    if (!gl_enter()) return;
    pointer_due();
    uint64_t t0 = hle_now_ns(), t1;
    state_forget();
    if (g_census.n) census_say();
    DOING("presenting");
    if (g_win) {
        int ww, wh;
        SDL_GL_GetDrawableSize(g_win, &ww, &wh);
        int w = ww, h = ww * d3d9.h / d3d9.w;                 /* as large as fits, its shape kept */
        if (h > wh) h = wh, w = wh * d3d9.w / d3d9.h;
        int x = (ww - w) / 2, y = (wh - h) / 2;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_for(d3d9.back, NULL));
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        g_cur_fbo = ~0u;
        glDisable(GL_SCISSOR_TEST);
        glColorMask(1, 1, 1, 1);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glBlitFramebuffer(0, 0, d3d9.w, d3d9.h, x, y + h, x + w, y, GL_COLOR_BUFFER_BIT, GL_LINEAR);   /* turned the right way up */
        if (hle_d3d9_vsync != g_swap_interval) SDL_GL_SetSwapInterval(hle_d3d9_vsync), g_swap_interval = hle_d3d9_vsync;
        t1 = hle_now_ns();
        SDL_GL_SwapWindow(g_win);
        hle_perf.swap_ns += hle_now_ns() - t1;
        pump();
    } else glFlush();
    hle_perf.present_ns += hle_now_ns() - t0;
    DOING("nothing");
}

/* A host's own picture (a frame of a movie it is playing itself): `rgba` is
 * w by h, top row first. It takes the place of the game's frame, as large as
 * fits with its shape kept, and is presented like one. Before a device
 * exists there is nowhere to show it.
 *
 * It is drawn the way the guest's own textures are, as a texture on a
 * rectangle: a copy between framebuffers from a texture that was only ever
 * filled from memory came out black under one Switch emulator. */
int hle_d3d9_show_picture(const uint8_t *rgba, int w, int h) {
    static GLuint tex, program;
    static GLint u_rect;
    if (!d3d9.open || !gl_enter() || w <= 0 || h <= 0) return -1;
    if (!program) {
        /* the four corners from the vertex's number: no vertex data at all */
        GLuint v = compile(GL_VERTEX_SHADER,
            "#version 300 es\nprecision highp float;\nuniform vec4 u_rect;\nout vec2 v_uv;\nvoid main() {\n"
            "  v_uv = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));\n"
            "  gl_Position = vec4((u_rect.xy + v_uv * u_rect.zw) * 2.0 - 1.0, 0.0, 1.0);\n}\n");
        GLuint f = compile(GL_FRAGMENT_SHADER,
            "#version 300 es\nprecision mediump float;\nuniform sampler2D u_tex;\nin vec2 v_uv;\nout vec4 o_color;\n"
            "void main() { o_color = vec4(texture(u_tex, v_uv).rgb, 1.0); }\n");
        program = glCreateProgram();
        glAttachShader(program, v), glAttachShader(program, f);
        glLinkProgram(program);
        glDeleteShader(v), glDeleteShader(f);
        glUseProgram(program);
        glUniform1i(glGetUniformLocation(program, "u_tex"), 0);
        u_rect = glGetUniformLocation(program, "u_rect");
        glGenTextures(1, &tex);
        bind_texture(tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    bind_texture(tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    int dw = d3d9.w, dh = d3d9.w * h / w;                     /* as large as fits the back buffer */
    if (dh > d3d9.h) dh = d3d9.h, dw = d3d9.h * w / h;
    bind_fbo(fbo_for(d3d9.back, NULL));
    glViewport(0, 0, d3d9.w, d3d9.h);
    glDisable(GL_SCISSOR_TEST), glDisable(GL_DEPTH_TEST), glDisable(GL_BLEND), glDisable(GL_CULL_FACE);
    glColorMask(1, 1, 1, 1);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    state_forget();
    glUseProgram(program), g_cur_prog = program;
    /* the back buffer's first row is the picture's top, and so is the texture's */
    glUniform4f(u_rect, (float)((d3d9.w - dw) / 2) / (float)d3d9.w, (float)((d3d9.h - dh) / 2) / (float)d3d9.h,
                (float)dw / (float)d3d9.w, (float)dh / (float)d3d9.h);
    enable_arrays(0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    d3d9_gl_present();
    d3d9.frame++;
    if (hle_d3d9_frame_hook) hle_d3d9_frame_hook(d3d9.frame);
    return 0;
}

static uint32_t crc32_of(uint32_t crc, const uint8_t *p, size_t n) {
    static uint32_t tab[256];
    if (!tab[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; tab[i] = c; }
    crc = ~crc;
    while (n--) crc = tab[(crc ^ *p++) & 255] ^ (crc >> 8);
    return ~crc;
}
static void chunk(FILE *f, const char *type, const uint8_t *data, size_t n) {
    uint8_t len[4] = { (uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n };
    uint32_t crc = crc32_of(crc32_of(0, (const uint8_t *)type, 4), data, n);
    uint8_t tail[4] = { (uint8_t)(crc >> 24), (uint8_t)(crc >> 16), (uint8_t)(crc >> 8), (uint8_t)crc };
    fwrite(len, 1, 4, f), fwrite(type, 1, 4, f);
    if (n) fwrite(data, 1, n, f);
    fwrite(tail, 1, 4, f);
}
/* The back buffer as a PNG (not compressed: this is for test runs). */
int hle_d3d9_save_png(const char *path) {
    if (!d3d9.open || !gl_enter()) return -1;
    int w = d3d9.w, h = d3d9.h;
    size_t row = 1 + (size_t)w * 3, raw = row * (size_t)h, blocks = (raw + 65534) / 65535;
    uint8_t *px = malloc((size_t)w * (size_t)h * 4), *z = malloc(2 + raw + blocks * 5 + 4);
    FILE *f = px && z ? fopen(path, "wb") : NULL;
    if (!f) { free(px), free(z); return -1; }
    bind_fbo(fbo_for(d3d9.back, NULL));
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    size_t n = 0, done = 0;
    uint32_t a = 1, b = 0;
    z[n++] = 0x78, z[n++] = 0x01;
    uint8_t *line = malloc(row);
    size_t in_block = 0;
    for (int y = 0; y < h; y++) {
        line[0] = 0;
        for (int x = 0; x < w; x++) memcpy(line + 1 + 3 * x, px + ((size_t)y * (size_t)w + (size_t)x) * 4, 3);
        for (size_t i = 0; i < row; i++) {
            if (!in_block) {
                size_t len = raw - done > 65535 ? 65535 : raw - done;
                z[n++] = done + len == raw, z[n++] = (uint8_t)len, z[n++] = (uint8_t)(len >> 8), z[n++] = (uint8_t)~len, z[n++] = (uint8_t)(~len >> 8);
                in_block = len;
            }
            z[n++] = line[i], in_block--, done++;
            a = (a + line[i]) % 65521, b = (b + a) % 65521;
        }
    }
    z[n++] = (uint8_t)(b >> 8), z[n++] = (uint8_t)b, z[n++] = (uint8_t)(a >> 8), z[n++] = (uint8_t)a;
    uint8_t head[13] = { (uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
                         (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h, 8, 2, 0, 0, 0 };
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    chunk(f, "IHDR", head, 13), chunk(f, "IDAT", z, n), chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(px), free(z), free(line);
    return 0;
}
unsigned long hle_d3d9_draws(void) { return g_draws; }
#endif /* HLE_WITH_D3D9 */
