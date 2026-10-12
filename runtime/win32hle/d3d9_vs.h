/*
 * d3d9_vs.h - Direct3D 9 vertex shaders without d3dx9 and without a GPU that
 * runs their bytecode: an assembler for the text a game hands
 * D3DXAssembleShader, the listing D3DXDisassembleShader gives back, and a
 * translation of the token stream to GLSL ES 3.00 for a GL back end.
 *
 * The module is pure: text and tokens in, malloc'd results out. It knows
 * nothing of the guest, so it builds wherever C99 does.
 */
#ifndef D3D9_VS_H
#define D3D9_VS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Assemble vertex shader source (the language D3DXAssembleShader takes). On success returns 0 and
 * *tokens = malloc'd DWORDs, *count = how many, the END token (0x0000FFFF) included.
 * On failure returns -1 and *error = malloc'd message ("line 12: ..."); *tokens is NULL. */
int    d3d9_vs_assemble(const char *src, size_t len, uint32_t **tokens, size_t *count, char **error);

/* DWORDs in a vertex shader, END token included; 0 if tokens[0] is not a vertex shader version token. */
size_t d3d9_vs_tokens(const uint32_t *tokens);

/* The listing D3DXDisassembleShader writes for it (malloc'd, NUL-terminated), or NULL. */
char  *d3d9_vs_disassemble(const uint32_t *tokens);

#define D3D9_VS_OUT_POS  0x0001u
#define D3D9_VS_OUT_D0   0x0002u
#define D3D9_VS_OUT_D1   0x0004u
#define D3D9_VS_OUT_FOG  0x0008u
#define D3D9_VS_OUT_PTS  0x0010u
#define D3D9_VS_OUT_T(n) (0x0100u << (n))      /* oT0..oT7 */
typedef struct {
    struct { uint8_t declared, usage, usage_index; } in[16];   /* v0..v15, from the dcl instructions (D3DDECLUSAGE values) */
    uint32_t outputs;                 /* D3D9_VS_OUT_* written anywhere in the shader */
    int      consts;                  /* one more than the highest c register read; 256 if any read is relative (c[a0.x + n]) */
    int      ndefs;                   /* def cN, x, y, z, w instructions */
    struct { int reg; float v[4]; } def[32];
} d3d9_vs_info;

/* The shader body as GLSL ES 3.00 statements (malloc'd), to be pasted inside a function body.
 * Fills *info. NULL if the shader uses something not supported.
 *
 * The text reads vec4 v0..v15 (the vertex inputs, widened to four components) and
 * uniform vec4 c[256], and writes vec4 oPos, oD0, oD1, oFog, oPts, oT0..oT7 (of oFog and oPts
 * only .x means anything); whoever pastes it declares those. It declares the temporaries it
 * uses itself (vec4 r0.., ivec4 a0). Constants a def gives are read from c like any other, so
 * they are to be loaded there from info->def. */
char  *d3d9_vs_glsl(const uint32_t *tokens, d3d9_vs_info *info);

#ifdef __cplusplus
}
#endif

#endif
