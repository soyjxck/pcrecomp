/*
 * d3d9_priv.h — what the two halves of the Direct3D 9 layer share.
 *
 * d3d9.c is Direct3D as the guest sees it: the COM objects, the device's
 * state, pixels and vertices kept in guest memory. It makes no GL call.
 * d3d9_gl.c draws that state with OpenGL ES 3. Everything GL owns is made
 * late, when a draw first needs it, so the guest may create and fill
 * resources from any thread while one thread draws.
 */
#ifndef D3D9_PRIV_H
#define D3D9_PRIV_H

#include <stddef.h>
#include <stdint.h>
#include "win32hle.h"
#include "d3d9_vs.h"

#define D3D_OK                 0u
#define D3DERR_INVALIDCALL     0x8876086Cu
#define D3DERR_NOTAVAILABLE    0x8876086Au
#define D3DERR_OUTOFVIDEOMEMORY 0x8876017Cu
#define E_FAIL_                0x80004005u
#define E_NOINTERFACE_         0x80004002u

enum {                          /* D3DFORMAT */
    D3DFMT_UNKNOWN = 0, D3DFMT_R8G8B8 = 20, D3DFMT_A8R8G8B8 = 21, D3DFMT_X8R8G8B8 = 22, D3DFMT_R5G6B5 = 23,
    D3DFMT_X1R5G5B5 = 24, D3DFMT_A1R5G5B5 = 25, D3DFMT_A4R4G4B4 = 26, D3DFMT_A8 = 28, D3DFMT_X4R4G4B4 = 30,
    D3DFMT_A8B8G8R8 = 32, D3DFMT_X8B8G8R8 = 33, D3DFMT_L8 = 50, D3DFMT_A8L8 = 51, D3DFMT_D16_LOCKABLE = 70,
    D3DFMT_D32 = 71, D3DFMT_D24S8 = 75, D3DFMT_D24X8 = 77, D3DFMT_D16 = 80, D3DFMT_VERTEXDATA = 100,
    D3DFMT_INDEX16 = 101, D3DFMT_INDEX32 = 102
};
enum { D3DPOOL_DEFAULT = 0, D3DPOOL_MANAGED = 1, D3DPOOL_SYSTEMMEM = 2, D3DPOOL_SCRATCH = 3 };
enum { D3DUSAGE_RENDERTARGET = 1, D3DUSAGE_DEPTHSTENCIL = 2, D3DUSAGE_WRITEONLY = 8, D3DUSAGE_DYNAMIC = 0x200,
       D3DUSAGE_AUTOGENMIPMAP = 0x400 };
enum { D3DRTYPE_SURFACE = 1, D3DRTYPE_TEXTURE = 3, D3DRTYPE_VERTEXBUFFER = 6, D3DRTYPE_INDEXBUFFER = 7 };
enum { D3DLOCK_READONLY = 0x10 };
enum { D3DCLEAR_TARGET = 1, D3DCLEAR_ZBUFFER = 2, D3DCLEAR_STENCIL = 4 };
enum { D3DPT_POINTLIST = 1, D3DPT_LINELIST, D3DPT_LINESTRIP, D3DPT_TRIANGLELIST, D3DPT_TRIANGLESTRIP,
       D3DPT_TRIANGLEFAN };
enum { D3DTS_VIEW = 2, D3DTS_PROJECTION = 3, D3DTS_WORLD = 256 };
enum {                          /* D3DFVF */
    D3DFVF_XYZ = 0x002, D3DFVF_XYZRHW = 0x004, D3DFVF_XYZB1 = 0x006, D3DFVF_XYZB5 = 0x00E, D3DFVF_XYZW = 0x4002,
    D3DFVF_POSITION_MASK = 0x400E, D3DFVF_NORMAL = 0x010, D3DFVF_PSIZE = 0x020, D3DFVF_DIFFUSE = 0x040,
    D3DFVF_SPECULAR = 0x080, D3DFVF_TEXCOUNT_MASK = 0xF00, D3DFVF_TEXCOUNT_SHIFT = 8
};
enum {                          /* D3DRENDERSTATETYPE, those the layer acts on */
    D3DRS_ZENABLE = 7, D3DRS_FILLMODE = 8, D3DRS_SHADEMODE = 9, D3DRS_ZWRITEENABLE = 14,
    D3DRS_ALPHATESTENABLE = 15, D3DRS_LASTPIXEL = 16, D3DRS_SRCBLEND = 19, D3DRS_DESTBLEND = 20,
    D3DRS_CULLMODE = 22, D3DRS_ZFUNC = 23, D3DRS_ALPHAREF = 24, D3DRS_ALPHAFUNC = 25, D3DRS_DITHERENABLE = 26,
    D3DRS_ALPHABLENDENABLE = 27, D3DRS_FOGENABLE = 28, D3DRS_SPECULARENABLE = 29, D3DRS_FOGCOLOR = 34,
    D3DRS_FOGTABLEMODE = 35, D3DRS_FOGSTART = 36, D3DRS_FOGEND = 37, D3DRS_FOGDENSITY = 38,
    D3DRS_RANGEFOGENABLE = 48, D3DRS_STENCILENABLE = 52, D3DRS_STENCILFAIL = 53, D3DRS_STENCILZFAIL = 54,
    D3DRS_STENCILPASS = 55, D3DRS_STENCILFUNC = 56, D3DRS_STENCILREF = 57, D3DRS_STENCILMASK = 58,
    D3DRS_STENCILWRITEMASK = 59, D3DRS_TEXTUREFACTOR = 60, D3DRS_CLIPPING = 136, D3DRS_LIGHTING = 137,
    D3DRS_AMBIENT = 139, D3DRS_FOGVERTEXMODE = 140, D3DRS_COLORVERTEX = 141, D3DRS_LOCALVIEWER = 142,
    D3DRS_NORMALIZENORMALS = 143, D3DRS_DIFFUSEMATERIALSOURCE = 145, D3DRS_SPECULARMATERIALSOURCE = 146,
    D3DRS_AMBIENTMATERIALSOURCE = 147, D3DRS_EMISSIVEMATERIALSOURCE = 148, D3DRS_POINTSIZE = 154,
    D3DRS_MULTISAMPLEANTIALIAS = 161, D3DRS_COLORWRITEENABLE = 168, D3DRS_BLENDOP = 171,
    D3DRS_SCISSORTESTENABLE = 174, D3DRS_SLOPESCALEDEPTHBIAS = 175, D3DRS_BLENDFACTOR = 193,
    D3DRS_DEPTHBIAS = 195, D3DRS_SEPARATEALPHABLENDENABLE = 206, D3DRS_SRCBLENDALPHA = 207,
    D3DRS_DESTBLENDALPHA = 208, D3DRS_BLENDOPALPHA = 209, D3DRS_COUNT = 256
};
enum { D3DTSS_COLOROP = 1, D3DTSS_COLORARG1, D3DTSS_COLORARG2, D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1,
       D3DTSS_ALPHAARG2, D3DTSS_TEXCOORDINDEX = 11, D3DTSS_TEXTURETRANSFORMFLAGS = 24, D3DTSS_COLORARG0 = 26,
       D3DTSS_ALPHAARG0 = 27, D3DTSS_RESULTARG = 28, D3DTSS_COUNT = 36 };
enum { D3DTOP_DISABLE = 1, D3DTOP_SELECTARG1, D3DTOP_SELECTARG2, D3DTOP_MODULATE, D3DTOP_MODULATE2X,
       D3DTOP_MODULATE4X, D3DTOP_ADD, D3DTOP_ADDSIGNED, D3DTOP_ADDSIGNED2X, D3DTOP_SUBTRACT, D3DTOP_ADDSMOOTH,
       D3DTOP_BLENDDIFFUSEALPHA, D3DTOP_BLENDTEXTUREALPHA, D3DTOP_BLENDFACTORALPHA };
enum { D3DTA_DIFFUSE = 0, D3DTA_CURRENT, D3DTA_TEXTURE, D3DTA_TFACTOR, D3DTA_SPECULAR, D3DTA_SELECTMASK = 0xF,
       D3DTA_COMPLEMENT = 0x10, D3DTA_ALPHAREPLICATE = 0x20 };
enum { D3DSAMP_ADDRESSU = 1, D3DSAMP_ADDRESSV, D3DSAMP_ADDRESSW, D3DSAMP_BORDERCOLOR, D3DSAMP_MAGFILTER,
       D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER, D3DSAMP_MIPMAPLODBIAS, D3DSAMP_MAXMIPLEVEL, D3DSAMP_MAXANISOTROPY,
       D3DSAMP_COUNT = 16 };
enum { D3DTADDRESS_WRAP = 1, D3DTADDRESS_MIRROR, D3DTADDRESS_CLAMP, D3DTADDRESS_BORDER };
enum { D3DTEXF_NONE = 0, D3DTEXF_POINT, D3DTEXF_LINEAR, D3DTEXF_ANISOTROPIC };
enum { D3DCULL_NONE = 1, D3DCULL_CW, D3DCULL_CCW };
enum { D3DFOG_NONE = 0, D3DFOG_EXP, D3DFOG_EXP2, D3DFOG_LINEAR };
enum { D3DMCS_MATERIAL = 0, D3DMCS_COLOR1, D3DMCS_COLOR2 };
enum { D3DLIGHT_POINT = 1, D3DLIGHT_SPOT, D3DLIGHT_DIRECTIONAL };
enum { D3DDECLTYPE_FLOAT1 = 0, D3DDECLTYPE_FLOAT2, D3DDECLTYPE_FLOAT3, D3DDECLTYPE_FLOAT4, D3DDECLTYPE_D3DCOLOR,
       D3DDECLTYPE_UBYTE4, D3DDECLTYPE_SHORT2, D3DDECLTYPE_SHORT4, D3DDECLTYPE_UBYTE4N, D3DDECLTYPE_SHORT2N,
       D3DDECLTYPE_SHORT4N, D3DDECLTYPE_USHORT2N, D3DDECLTYPE_USHORT4N, D3DDECLTYPE_UNUSED = 17 };

typedef struct d3d9_texture d3d9_texture;

enum { D3D9_SURF_LEVEL, D3D9_SURF_BACK, D3D9_SURF_TARGET, D3D9_SURF_DEPTH, D3D9_SURF_PLAIN };
typedef struct d3d9_surface {
    uint32_t self;              /* the guest's object */
    int kind;
    int w, h, fmt, usage, pool;
    d3d9_texture *tex;          /* the texture a level belongs to */
    int level;
    uint32_t bits;              /* the pixels in guest memory, top row first; 0 until someone wants them */
    int pitch;
    int dirty;                  /* the guest's pixels are newer than GL's */
    int locked, lock_write;
    unsigned gl_tex, gl_rb, gl_fbo;   /* a target's own texture, a depth buffer, a framebuffer to read or blit it */
} d3d9_surface;

#define D3D9_MAX_LEVELS 14
struct d3d9_texture {
    uint32_t self;
    int w, h, levels, fmt, usage, pool;
    d3d9_surface *level[D3D9_MAX_LEVELS];
    int dirty;                  /* some level is */
    unsigned gl;                /* 0 until first drawn with */
    int gl_min, gl_mag, gl_u, gl_v, gl_aniso;   /* the sampling GL has for it */
};

typedef struct d3d9_buffer {
    uint32_t self;
    int is_index;
    int size, usage, pool, fvf, fmt;
    uint32_t data;              /* the bytes in guest memory */
    int dirty;
    unsigned gl;
} d3d9_buffer;

typedef struct d3d9_vshader {
    uint32_t self;
    int id;                     /* a number no other shader of the run has had */
    uint32_t *tokens;
    size_t ntokens;
    char *glsl;                 /* its body; NULL if it could not be translated */
    d3d9_vs_info info;
} d3d9_vshader;

typedef struct d3d9_vdecl {
    uint32_t self;
    int id;
    int n;
    struct { uint16_t stream, offset; uint8_t type, method, usage, index; } el[32];
} d3d9_vdecl;

typedef struct {
    int open;
    int w, h;                   /* the back buffer */
    uint32_t window;
    uint32_t rs[D3DRS_COUNT];
    uint32_t ss[8][D3DSAMP_COUNT];
    uint32_t tss[8][D3DTSS_COUNT];
    float world[16], view[16], proj[16];
    struct { uint32_t x, y, w, h; float minz, maxz; } vp;
    int scissor[4];             /* left, top, right, bottom */
    float material[17];         /* D3DMATERIAL9: diffuse, ambient, specular, emissive, power */
    struct { uint32_t type; float v[25]; int enabled; } light[8];   /* v: D3DLIGHT9 after its type */
    uint32_t fvf;
    d3d9_vshader *vs;
    d3d9_vdecl *decl;
    d3d9_buffer *vb, *ib;
    uint32_t vb_offset, vb_stride;
    d3d9_texture *tex[8];
    float vsc[256][4];
    d3d9_surface *rt, *ds;      /* what is drawn into */
    d3d9_surface *back, *back_ds;
    unsigned frame;             /* Presents so far */
    unsigned vsc_serial;        /* goes up whenever a vertex shader constant is set */
} d3d9_state;
extern d3d9_state d3d9;

/* ---- pixels (d3d9.c) ---- */
int  d3d9_fmt_bytes(int fmt);                                            /* a pixel's bytes; 0 if the format is not kept */
int  d3d9_fmt_alpha(int fmt);
void d3d9_to_bgra(int fmt, const void *src, uint8_t *bgra, int n);       /* n pixels to B, G, R, A bytes */
void d3d9_from_bgra(int fmt, const uint8_t *bgra, void *dst, int n);
d3d9_surface *d3d9_surface_of(uint32_t obj);
d3d9_texture *d3d9_texture_of(uint32_t obj);
uint32_t d3d9_texture_new(int w, int h, int levels, int usage, int fmt, int pool);   /* the guest object, or 0 */
/* A rectangle of a surface as BGRA (malloc'd), wherever its pixels are; and put back. */
uint8_t *d3d9_surface_read(d3d9_surface *s, int x, int y, int w, int h);
void     d3d9_surface_write(d3d9_surface *s, int x, int y, int w, int h, const uint8_t *bgra);
uint32_t d3d9_surface_bits(d3d9_surface *s);                             /* its guest pixels, made if need be */

/* ---- the drawing half (d3d9_gl.c) ---- */
typedef struct {
    int prim, nprims;
    int indexed;
    const uint8_t *user_verts;  /* DrawPrimitiveUP's vertices; NULL: the stream source */
    int user_stride;
    const uint8_t *user_index;  /* DrawIndexedPrimitiveUP's indices; NULL: the index buffer */
    int index_bytes;            /* 2 or 4 */
    int first;                  /* first vertex (not indexed), or first index */
    int base;                   /* added to every index */
    int min_vertex, num_vertices;   /* the range of vertices an indexed draw uses */
} d3d9_draw;

int  d3d9_gl_open(int w, int h, int headless);        /* the window, the context, the back buffer: 0 if it worked */
void d3d9_gl_reset(int w, int h);                     /* a new back buffer size */
void d3d9_gl_clear(uint32_t flags, uint32_t color, float z, uint32_t stencil);
void d3d9_gl_draw(const d3d9_draw *d);
void d3d9_gl_present(void);
int  d3d9_gl_read(d3d9_surface *s, int x, int y, int w, int h, uint8_t *bgra);    /* a target's pixels */
void d3d9_gl_upload(d3d9_surface *s, int x, int y, int w, int h, const uint8_t *bgra);   /* into a target */
void d3d9_gl_blit(d3d9_surface *from, const int *fr, d3d9_surface *to, const int *tr, int linear);
void d3d9_gl_retire(unsigned tex, unsigned rb, unsigned fbo, unsigned buf);   /* GL names nothing uses any more */
void d3d9_gl_forget_surface(const d3d9_surface *s);   /* it is going: drop framebuffers made with it */
int  d3d9_gl_max_texture(void);

#endif /* D3D9_PRIV_H */
