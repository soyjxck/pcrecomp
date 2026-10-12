/*
 * d3d9_vs_selftest.c - the vertex shader assembler, listing and GLSL
 * translator against shaders written for this test, which between them use
 * every instruction, register and way of naming a constant the module
 * handles. The tokens and listings expected of them are what d3dx9_43.dll
 * gives for the same text, so a pass is agreement with D3DX rather than with
 * ourselves. The GLSL is checked a statement at a time against what each
 * instruction means.
 *
 * Pure like the module: no guest, no window. Builds with d3d9_vs.c alone.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "d3d9_vs.h"

/* build_selftests.sh links every selftest with the whole layer, whose host looks lifted code up
 * in a table the lifted program defines. There is no guest here and none of the layer's headers,
 * so this is an empty table of the same shape, there to satisfy that link. */
const struct { uint32_t address; void (*func)(void); } recomp_dispatch_table[1] = { { 0, 0 } };
const uint32_t recomp_dispatch_count = 0;

static int fails;
static void ok(const char *who, const char *what, int c) { if (!c) { printf("FAIL %s: %s\n", who, what); fails++; } }

/* every vs_1_1 instruction */
static const char src_all11[] =
    "; every vs_1_1 instruction, register and way of naming a constant\n"
    "vs_1_1\n"
    "dcl_position v0\n"
    "dcl_normal v1\n"
    "dcl_color0 v2           // index 0 may be spelled out\n"
    "dcl_color1 v3\n"
    "dcl_texcoord v4\n"
    "dcl_texcoord3 v5\n"
    "dcl_blendweight v6\n"
    "dcl_blendindices v7\n"
    "dcl_psize v8\n"
    "dcl_tangent v9\n"
    "dcl_binormal v10\n"
    "dcl_fog v11\n"
    "def c40, 1, 0.5, -2.25, 1e-3\n"
    "def c41, 0x10, 010, .75, 1.5f\n"
    "def c42, - 3, +4, 1e10, 1.\n"
    "mov r0, v0\n"
    "mov r1, c40\n"
    "mov r2, -v1.zyxw\n"
    "mov r3, v2.x\n"
    "mov r4, v3.xy\n"
    "mov r5, v4.bgr          /* rgba are the same four */\n"
    "mov r6, c[41]\n"
    "mov r7, c2[40]\n"
    "add r0, r0, c0\n"
    "add r1.x, r1.y, -c40.z\n"
    "sub r2.yw, r2, v1\n"
    "sub r3, r3, -c1.wzyx\n"
    "mul r4.xyz, r4, c40.y\n"
    "mad r5, r5, c3.x, c3.y\n"
    "min r6.xz, r6, r5\n"
    "max r7, r7, -r6.w\n"
    "dp3 r0.w, r1, r2\n"
    "dp4 r1.x, v0, c7\n"
    "slt r2, r2, c40.x\n"
    "sge r3.zw, r3, r4.x\n"
    "rcp r4.w, r0.w\n"
    "rcp r5, r0\n"
    "rsq r6.xy, -r1.x\n"
    "rsq r6.zw, c40\n"
    "exp r7.x, r0.y\n"
    "expp r7, r1.x\n"
    "log r0.z, r1.x\n"
    "logp r0.yz, c40.z\n"
    "lit r1, r2\n"
    "dst r2, r3, r4\n"
    "frc r3.xy, r4\n"
    "frc r5.y, c40\n"
    "m4x4 r4, v0, c8\n"
    "m4x3 r5, v0, c12\n"
    "m4x3 r5.xyz, r4, c12\n"
    "m3x4 r6, v1, c16\n"
    "m3x3 r7, v1, c20\n"
    "m3x2 r0, v4, c24\n"
    "m3x3 r1, v9, r4\n"
    "nop\n"
    "mov a0.x, r3.z\n"
    "mov r1, c[a0.x + 44]\n"
    "mov r2, c44[a0.x]\n"
    "mov r3, -c[30 + a0.x + 14].wzyx\n"
    "m4x3 r6, v0, c50[a0.x]\n"
    "mad r7, v6.x, c[a0.x + 60], c60[a0.x].w\n"
    "mul r7, r7, v7\n"
    "add r7, r7, v10\n"
    "add r7, r7, v11.x\n"
    "mov oPos, r4\n"
    "mov oD0, v2\n"
    "mov oD1.xyz, r5\n"
    "mov oD1.w, c40.x\n"
    "mov oT0, v4\n"
    "mov oT1.xy, v5\n"
    "mov oT2, r6\n"
    "mov oT3, r7\n"
    "mov oT4, r0\n"
    "mov oT5, r1\n"
    "mov oT6, r2\n"
    "mov oT7, r3\n"
    "mov oFog, r0.w\n"
    "mad oPts, v8.x, c40.y, c40.x\n";
static const uint32_t tok_all11[] = {
    0xFFFE0101, 0x0000001F, 0x80000000, 0x900F0000, 0x0000001F, 0x80000003, 0x900F0001, 0x0000001F,
    0x8000000A, 0x900F0002, 0x0000001F, 0x8001000A, 0x900F0003, 0x0000001F, 0x80000005, 0x900F0004,
    0x0000001F, 0x80030005, 0x900F0005, 0x0000001F, 0x80000001, 0x900F0006, 0x0000001F, 0x80000002,
    0x900F0007, 0x0000001F, 0x80000004, 0x900F0008, 0x0000001F, 0x80000006, 0x900F0009, 0x0000001F,
    0x80000007, 0x900F000A, 0x0000001F, 0x8000000B, 0x900F000B, 0x00000051, 0xA00F0028, 0x3F800000,
    0x3F000000, 0xC0100000, 0x3A83126F, 0x00000051, 0xA00F0029, 0x41800000, 0x41000000, 0x3F400000,
    0x3FC00000, 0x00000051, 0xA00F002A, 0xC0400000, 0x40800000, 0x501502F9, 0x3F800000, 0x00000001,
    0x800F0000, 0x90E40000, 0x00000001, 0x800F0001, 0xA0E40028, 0x00000001, 0x800F0002, 0x91C60001,
    0x00000001, 0x800F0003, 0x90000002, 0x00000001, 0x800F0004, 0x90540003, 0x00000001, 0x800F0005,
    0x90060004, 0x00000001, 0x800F0006, 0xA0E40029, 0x00000001, 0x800F0007, 0xA0E4002A, 0x00000002,
    0x800F0000, 0x80E40000, 0xA0E40000, 0x00000002, 0x80010001, 0x80550001, 0xA1AA0028, 0x00000002,
    0x800A0002, 0x80E40002, 0x91E40001, 0x00000002, 0x800F0003, 0x80E40003, 0xA01B0001, 0x00000005,
    0x80070004, 0x80E40004, 0xA0550028, 0x00000004, 0x800F0005, 0x80E40005, 0xA0000003, 0xA0550003,
    0x0000000A, 0x80050006, 0x80E40006, 0x80E40005, 0x0000000B, 0x800F0007, 0x80E40007, 0x81FF0006,
    0x00000008, 0x80080000, 0x80E40001, 0x80E40002, 0x00000009, 0x80010001, 0x90E40000, 0xA0E40007,
    0x0000000C, 0x800F0002, 0x80E40002, 0xA0000028, 0x0000000D, 0x800C0003, 0x80E40003, 0x80000004,
    0x00000006, 0x80080004, 0x80FF0000, 0x00000006, 0x800F0005, 0x80FF0000, 0x00000007, 0x80030006,
    0x81000001, 0x00000007, 0x800C0006, 0xA0FF0028, 0x0000000E, 0x80010007, 0x80550000, 0x0000004E,
    0x800F0007, 0x80000001, 0x0000000F, 0x80040000, 0x80000001, 0x0000004F, 0x80060000, 0xA0AA0028,
    0x00000010, 0x800F0001, 0x80E40002, 0x00000011, 0x800F0002, 0x80E40003, 0x80E40004, 0x00000013,
    0x80030003, 0x80E40004, 0x00000013, 0x80020005, 0xA0E40028, 0x00000014, 0x800F0004, 0x90E40000,
    0xA0E40008, 0x00000015, 0x80070005, 0x90E40000, 0xA0E4000C, 0x00000015, 0x80070005, 0x80E40004,
    0xA0E4000C, 0x00000016, 0x800F0006, 0x90E40001, 0xA0E40010, 0x00000017, 0x80070007, 0x90E40001,
    0xA0E40014, 0x00000018, 0x80030000, 0x90E40004, 0xA0E40018, 0x00000017, 0x80070001, 0x90E40009,
    0x80E40004, 0x00000000, 0x00000001, 0xB0010000, 0x80AA0003, 0x00000001, 0x800F0001, 0xA0E4202C,
    0x00000001, 0x800F0002, 0xA0E4202C, 0x00000001, 0x800F0003, 0xA11B202C, 0x00000015, 0x80070006,
    0x90E40000, 0xA0E42032, 0x00000004, 0x800F0007, 0x90000006, 0xA0E4203C, 0xA0FF203C, 0x00000005,
    0x800F0007, 0x80E40007, 0x90E40007, 0x00000002, 0x800F0007, 0x80E40007, 0x90E4000A, 0x00000002,
    0x800F0007, 0x80E40007, 0x9000000B, 0x00000001, 0xC00F0000, 0x80E40004, 0x00000001, 0xD00F0000,
    0x90E40002, 0x00000001, 0xD0070001, 0x80E40005, 0x00000001, 0xD0080001, 0xA0000028, 0x00000001,
    0xE00F0000, 0x90E40004, 0x00000001, 0xE0030001, 0x90E40005, 0x00000001, 0xE00F0002, 0x80E40006,
    0x00000001, 0xE00F0003, 0x80E40007, 0x00000001, 0xE00F0004, 0x80E40000, 0x00000001, 0xE00F0005,
    0x80E40001, 0x00000001, 0xE00F0006, 0x80E40002, 0x00000001, 0xE00F0007, 0x80E40003, 0x00000001,
    0xC00F0001, 0x80FF0000, 0x00000004, 0xC00F0002, 0x90000008, 0xA0550028, 0xA0000028, 0x0000FFFF,
};
static const char lst_all11[] =
    "    vs_1_1\n"
    "    dcl_position v0\n"
    "    dcl_normal v1\n"
    "    dcl_color v2\n"
    "    dcl_color1 v3\n"
    "    dcl_texcoord v4\n"
    "    dcl_texcoord3 v5\n"
    "    dcl_blendweight v6\n"
    "    dcl_blendindices v7\n"
    "    dcl_psize v8\n"
    "    dcl_tangent v9\n"
    "    dcl_binormal v10\n"
    "    dcl_fog v11\n"
    "    def c40, 1, 0.5, -2.25, 0.00100000005\n"
    "    def c41, 16, 8, 0.75, 1.5\n"
    "    def c42, -3, 4, 1e+010, 1\n"
    "    mov r0, v0\n"
    "    mov r1, c40\n"
    "    mov r2, -v1.zyxw\n"
    "    mov r3, v2.x\n"
    "    mov r4, v3.xyyy\n"
    "    mov r5, v4.zyxx\n"
    "    mov r6, c41\n"
    "    mov r7, c42\n"
    "    add r0, r0, c0\n"
    "    add r1.x, r1.y, -c40.z\n"
    "    add r2.yw, r2, -v1\n"
    "    add r3, r3, c1.wzyx\n"
    "    mul r4.xyz, r4, c40.y\n"
    "    mad r5, r5, c3.x, c3.y\n"
    "    min r6.xz, r6, r5\n"
    "    max r7, r7, -r6.w\n"
    "    dp3 r0.w, r1, r2\n"
    "    dp4 r1.x, v0, c7\n"
    "    slt r2, r2, c40.x\n"
    "    sge r3.zw, r3, r4.x\n"
    "    rcp r4.w, r0.w\n"
    "    rcp r5, r0.w\n"
    "    rsq r6.xy, -r1.x\n"
    "    rsq r6.zw, c40.w\n"
    "    exp r7.x, r0.y\n"
    "    expp r7, r1.x\n"
    "    log r0.z, r1.x\n"
    "    logp r0.yz, c40.z\n"
    "    lit r1, r2\n"
    "    dst r2, r3, r4\n"
    "    frc r3.xy, r4\n"
    "    frc r5.y, c40\n"
    "    m4x4 r4, v0, c8\n"
    "    m4x3 r5.xyz, v0, c12\n"
    "    m4x3 r5.xyz, r4, c12\n"
    "    m3x4 r6, v1, c16\n"
    "    m3x3 r7.xyz, v1, c20\n"
    "    m3x2 r0.xy, v4, c24\n"
    "    m3x3 r1.xyz, v9, r4\n"
    "    nop\n"
    "    mov a0.x, r3.z\n"
    "    mov r1, c44[a0.x]\n"
    "    mov r2, c44[a0.x]\n"
    "    mov r3, -c44[a0.x].wzyx\n"
    "    m4x3 r6.xyz, v0, c50[a0.x]\n"
    "    mad r7, v6.x, c60[a0.x], c60[a0.x].w\n"
    "    mul r7, r7, v7\n"
    "    add r7, r7, v10\n"
    "    add r7, r7, v11.x\n"
    "    mov oPos, r4\n"
    "    mov oD0, v2\n"
    "    mov oD1.xyz, r5\n"
    "    mov oD1.w, c40.x\n"
    "    mov oT0, v4\n"
    "    mov oT1.xy, v5\n"
    "    mov oT2, r6\n"
    "    mov oT3, r7\n"
    "    mov oT4, r0\n"
    "    mov oT5, r1\n"
    "    mov oT6, r2\n"
    "    mov oT7, r3\n"
    "    mov oFog, r0.w\n"
    "    mad oPts, v8.x, c40.y, c40.x\n"
    "\n"
    "// approximately 101 instruction slots used\n";

/* the vs_2_0 additions */
static const char src_all20[] =
    "// what vs_2_0 adds to the straight-line language\n"
    "vs_2_0\n"
    "dcl_position v0\n"
    "dcl_normal v1\n"
    "dcl_texcoord v2\n"
    "def c30, 0.25, 4, -1, 0\n"
    "mov r0, v0\n"
    "mov r1, v1\n"
    "mov r2, v2\n"
    "mov r3, c30\n"
    "mov r4, c30.x\n"
    "mov r5, c30.y\n"
    "mov r6, c30.z\n"
    "mov r7, c30.w\n"
    "mov r8, c30\n"
    "abs r3, r0\n"
    "sgn r4, r1, r5, r6\n"
    "nrm r5.xyz, r1\n"
    "nrm r6, r0\n"
    "crs r7.xyz, r0, r1\n"
    "crs r8.xz, r1, r2\n"
    "lrp r3, r4, r0, r1\n"
    "pow r4, r0.x, r1.y\n"
    "pow r5.w, c30.y, r0.w\n"
    "sincos r6.xy, r0.x, c31, c32\n"
    "sincos r7.y, r2.w, c31, c32\n"
    "mova a0, r0\n"
    "mova a0.xw, r1\n"
    "mov r1, c[a0.x + 5]\n"
    "mov r2, c5[a0.y]\n"
    "add r3, r3, -c[a0.w + 40].zyxw\n"
    "m4x3 r4.xyz, v0, c[a0.z + 8]\n"
    "m4x4 r5, v0, c12\n"
    "m3x2 r6.xy, v1, c16\n"
    "frc r7, r0\n"
    "exp r8, r0.z\n"
    "log r8.x, r0.x\n"
    "rsq r8.y, r1.w\n"
    "rcp r8.z, c30.y\n"
    "expp r8.w, r0.x\n"
    "sub r0, r0, r1\n"
    "mov oPos, r5\n"
    "mov oD0, r3\n"
    "mov oT0, r4\n"
    "mov oT1.xy, r6\n"
    "mov oFog, r8.x\n";
static const uint32_t tok_all20[] = {
    0xFFFE0200, 0x0200001F, 0x80000000, 0x900F0000, 0x0200001F, 0x80000003, 0x900F0001, 0x0200001F,
    0x80000005, 0x900F0002, 0x05000051, 0xA00F001E, 0x3E800000, 0x40800000, 0xBF800000, 0x00000000,
    0x02000001, 0x800F0000, 0x90E40000, 0x02000001, 0x800F0001, 0x90E40001, 0x02000001, 0x800F0002,
    0x90E40002, 0x02000001, 0x800F0003, 0xA0E4001E, 0x02000001, 0x800F0004, 0xA000001E, 0x02000001,
    0x800F0005, 0xA055001E, 0x02000001, 0x800F0006, 0xA0AA001E, 0x02000001, 0x800F0007, 0xA0FF001E,
    0x02000001, 0x800F0008, 0xA0E4001E, 0x02000023, 0x800F0003, 0x80E40000, 0x04000022, 0x800F0004,
    0x80E40001, 0x80E40005, 0x80E40006, 0x02000024, 0x80070005, 0x80E40001, 0x02000024, 0x800F0006,
    0x80E40000, 0x03000021, 0x80070007, 0x80E40000, 0x80E40001, 0x03000021, 0x80050008, 0x80E40001,
    0x80E40002, 0x04000012, 0x800F0003, 0x80E40004, 0x80E40000, 0x80E40001, 0x03000020, 0x800F0004,
    0x80000000, 0x80550001, 0x03000020, 0x80080005, 0xA055001E, 0x80FF0000, 0x04000025, 0x80030006,
    0x80000000, 0xA0E4001F, 0xA0E40020, 0x04000025, 0x80020007, 0x80FF0002, 0xA0E4001F, 0xA0E40020,
    0x0200002E, 0xB00F0000, 0x80E40000, 0x0200002E, 0xB0090000, 0x80E40001, 0x03000001, 0x800F0001,
    0xA0E42005, 0xB0000000, 0x03000001, 0x800F0002, 0xA0E42005, 0xB0550000, 0x04000002, 0x800F0003,
    0x80E40003, 0xA1C62028, 0xB0FF0000, 0x04000015, 0x80070004, 0x90E40000, 0xA0E42008, 0xB0AA0000,
    0x03000014, 0x800F0005, 0x90E40000, 0xA0E4000C, 0x03000018, 0x80030006, 0x90E40001, 0xA0E40010,
    0x02000013, 0x800F0007, 0x80E40000, 0x0200000E, 0x800F0008, 0x80AA0000, 0x0200000F, 0x80010008,
    0x80000000, 0x02000007, 0x80020008, 0x80FF0001, 0x02000006, 0x80040008, 0xA055001E, 0x0200004E,
    0x80080008, 0x80000000, 0x03000002, 0x800F0000, 0x80E40000, 0x81E40001, 0x02000001, 0xC00F0000,
    0x80E40005, 0x02000001, 0xD00F0000, 0x80E40003, 0x02000001, 0xE00F0000, 0x80E40004, 0x02000001,
    0xE0030001, 0x80E40006, 0x02000001, 0xC00F0001, 0x80000008, 0x0000FFFF,
};
static const char lst_all20[] =
    "    vs_2_0\n"
    "    dcl_position v0\n"
    "    dcl_normal v1\n"
    "    dcl_texcoord v2\n"
    "    def c30, 0.25, 4, -1, 0\n"
    "    mov r0, v0\n"
    "    mov r1, v1\n"
    "    mov r2, v2\n"
    "    mov r3, c30\n"
    "    mov r4, c30.x\n"
    "    mov r5, c30.y\n"
    "    mov r6, c30.z\n"
    "    mov r7, c30.w\n"
    "    mov r8, c30\n"
    "    abs r3, r0\n"
    "    sgn r4, r1, r5, r6\n"
    "    nrm r5.xyz, r1\n"
    "    nrm r6, r0\n"
    "    crs r7.xyz, r0, r1\n"
    "    crs r8.xz, r1, r2\n"
    "    lrp r3, r4, r0, r1\n"
    "    pow r4, r0.x, r1.y\n"
    "    pow r5.w, c30.y, r0.w\n"
    "    sincos r6.xy, r0.x, c31, c32\n"
    "    sincos r7.y, r2.w, c31, c32\n"
    "    mova a0, r0\n"
    "    mova a0.xw, r1\n"
    "    mov r1, c5[a0.x]\n"
    "    mov r2, c5[a0.y]\n"
    "    add r3, r3, -c40[a0.w].zyxw\n"
    "    m4x3 r4.xyz, v0, c8[a0.z]\n"
    "    m4x4 r5, v0, c12\n"
    "    m3x2 r6.xy, v1, c16\n"
    "    frc r7, r0\n"
    "    exp r8, r0.z\n"
    "    log r8.x, r0.x\n"
    "    rsq r8.y, r1.w\n"
    "    rcp r8.z, c30.y\n"
    "    expp r8.w, r0.x\n"
    "    add r0, r0, -r1\n"
    "    mov oPos, r5\n"
    "    mov oD0, r3\n"
    "    mov oT0, r4\n"
    "    mov oT1.xy, r6\n"
    "    mov oFog, r8.x\n"
    "\n"
    "// approximately 72 instruction slots used\n";

/* a small skinning shader */
static const char src_skin[] =
    "vs_1_1\n"
    "dcl_position v0\n"
    "dcl_blendindices v1\n"
    "dcl_color v2\n"
    "def c5, 3, 0, 1, 0.5\n"
    "mul r0, v1.zyxw, c5.x\n"
    "mov a0.x, r0.x\n"
    "m4x3 r1, v0, c[a0.x + 16]\n"
    "mov r1.w, c5.z\n"
    "m4x4 oPos, r1, c0\n"
    "add r2.xy, v0.xzzz, -c4.yx\n"
    "mov oT0.xy, r2\n"
    "mul oD0, v2, c5.w\n"
    "rsq r3, r1.w\n"
    "mov oFog, r3.x\n";
static const uint32_t tok_skin[] = {
    0xFFFE0101, 0x0000001F, 0x80000000, 0x900F0000, 0x0000001F, 0x80000002, 0x900F0001, 0x0000001F,
    0x8000000A, 0x900F0002, 0x00000051, 0xA00F0005, 0x40400000, 0x00000000, 0x3F800000, 0x3F000000,
    0x00000005, 0x800F0000, 0x90C60001, 0xA0000005, 0x00000001, 0xB0010000, 0x80000000, 0x00000015,
    0x80070001, 0x90E40000, 0xA0E42010, 0x00000001, 0x80080001, 0xA0AA0005, 0x00000014, 0xC00F0000,
    0x80E40001, 0xA0E40000, 0x00000002, 0x80030002, 0x90A80000, 0xA1010004, 0x00000001, 0xE0030000,
    0x80E40002, 0x00000005, 0xD00F0000, 0x90E40002, 0xA0FF0005, 0x00000007, 0x800F0003, 0x80FF0001,
    0x00000001, 0xC00F0001, 0x80000003, 0x0000FFFF,
};
static const char lst_skin[] =
    "    vs_1_1\n"
    "    dcl_position v0\n"
    "    dcl_blendindices v1\n"
    "    dcl_color v2\n"
    "    def c5, 3, 0, 1, 0.5\n"
    "    mul r0, v1.zyxw, c5.x\n"
    "    mov a0.x, r0.x\n"
    "    m4x3 r1.xyz, v0, c16[a0.x]\n"
    "    mov r1.w, c5.z\n"
    "    m4x4 oPos, r1, c0\n"
    "    add r2.xy, v0.xzzz, -c4.yxxx\n"
    "    mov oT0.xy, r2\n"
    "    mul oD0, v2, c5.w\n"
    "    rsq r3, r1.w\n"
    "    mov oFog, r3.x\n"
    "\n"
    "// approximately 15 instruction slots used\n";

/* loose layout */
static const char src_loose[] =
    "    vs.1.1\r\n"
    "    dcl_position v0\r\n"
    "\tdcl_texcoord\tv1\r\n"
    "    m4x4 oPos , v0 , c0\r\n"
    "    mov oT0.xy,v1   mov oD0 , c5 . wzyx ; two on a line\r\n"
    "    mad oFog , v0.z, c4.x, c4.y\r\n"
    "\r\n"
    "// approximately 7 instruction slots used\r\n";
static const uint32_t tok_loose[] = {
    0xFFFE0101, 0x0000001F, 0x80000000, 0x900F0000, 0x0000001F, 0x80000005, 0x900F0001, 0x00000014,
    0xC00F0000, 0x90E40000, 0xA0E40000, 0x00000001, 0xE0030000, 0x90E40001, 0x00000001, 0xD00F0000,
    0xA01B0005, 0x00000004, 0xC00F0001, 0x90AA0000, 0xA0000004, 0xA0550004, 0x0000FFFF,
};
static const char lst_loose[] =
    "    vs_1_1\n"
    "    dcl_position v0\n"
    "    dcl_texcoord v1\n"
    "    m4x4 oPos, v0, c0\n"
    "    mov oT0.xy, v1\n"
    "    mov oD0, c5.wzyx\n"
    "    mad oFog, v0.z, c4.x, c4.y\n"
    "\n"
    "// approximately 7 instruction slots used\n";

typedef struct {
    const char     *name, *src;
    const uint32_t *tokens;
    size_t          count;
    const char     *listing;
} shader_case;
static const shader_case g_cases[] = {
    { "all11", src_all11, tok_all11, sizeof tok_all11 / 4, lst_all11 },
    { "all20", src_all20, tok_all20, sizeof tok_all20 / 4, lst_all20 },
    { "skin", src_skin, tok_skin, sizeof tok_skin / 4, lst_skin },
    { "loose", src_loose, tok_loose, sizeof tok_loose / 4, lst_loose },
};
#define NCASES (sizeof g_cases / sizeof g_cases[0])

static int same_tokens(const uint32_t *a, size_t na, const uint32_t *b, size_t nb) {
    return a && na == nb && !memcmp(a, b, na * 4);
}

/* Whether text has this line, whole. */
static int has_line(const char *text, const char *line) {
    size_t n = strlen(line);
    for (const char *p = text; text && (p = strstr(p, line)) != NULL; p += n)
        if ((p == text || p[-1] == '\n') && p[n] == '\n') return 1;
    return 0;
}

/* Source to tokens with nothing kept: 1 if it assembled. */
static int assembles(const char *src, uint32_t **tok, size_t *n) {
    char *err = NULL;
    int rc = d3d9_vs_assemble(src, strlen(src), tok, n, &err);
    if (err) printf("  %s\n", err);
    free(err);
    return rc == 0 && *tok;
}

/* The three entry points on one shader: source to tokens, tokens to listing, and the listing
 * through the assembler again, which is the round trip a game can make with D3DX. */
static void check_case(const shader_case *t) {
    uint32_t *tok = NULL;
    size_t n = 0;
    ok(t->name, "assembles", assembles(t->src, &tok, &n));
    ok(t->name, "tokens are D3DX's", same_tokens(tok, n, t->tokens, t->count));
    for (size_t i = 0; tok && i < n && i < t->count; i++)
        if (tok[i] != t->tokens[i]) {
            printf("  token %u is %08X, expected %08X\n", (unsigned)i, (unsigned)tok[i], (unsigned)t->tokens[i]);
            break;
        }
    free(tok);

    ok(t->name, "token count", d3d9_vs_tokens(t->tokens) == t->count);

    char *text = d3d9_vs_disassemble(t->tokens);
    ok(t->name, "listing is D3DX's", text && !strcmp(text, t->listing));
    tok = NULL;
    ok(t->name, "its listing assembles to the same tokens", text && assembles(text, &tok, &n) && same_tokens(tok, n, t->tokens, t->count));
    free(tok);
    free(text);
}

/* ---- GLSL ---- */

/* The small shader whole, to pin the shape of the text: the declarations first, then a
 * statement an instruction. */
static const char glsl_skin[] =
    "    vec4 r0 = vec4(0.0);\n"
    "    vec4 r1 = vec4(0.0);\n"
    "    vec4 r2 = vec4(0.0);\n"
    "    vec4 r3 = vec4(0.0);\n"
    "    ivec4 a0 = ivec4(0);\n"
    "    r0 = v1.zyxw * c[5].xxxx;\n"
    "    a0.x = ivec4(floor(r0.xxxx)).x;\n"
    "    r1.xyz = vec4(dot(v0, c[clamp(a0.x + 16, 0, 255)]), dot(v0, c[clamp(a0.x + 17, 0, 255)]), dot(v0, c[clamp(a0.x + 18, 0, 255)]), 0.0).xyz;\n"
    "    r1.w = (c[5].zzzz).w;\n"
    "    oPos = vec4(dot(r1, c[0]), dot(r1, c[1]), dot(r1, c[2]), dot(r1, c[3]));\n"
    "    r2.xy = (v0.xzzz + (-c[4].yxxx)).xy;\n"
    "    oT0.xy = (r2).xy;\n"
    "    oD0 = v2 * c[5].wwww;\n"
    "    r3 = vec4(r1.w == 0.0 ? 3.402823466e+38 : inversesqrt(abs(r1.w)));\n"
    "    oFog = r3.xxxx;\n";

/* What single instructions of the two big shaders become. D3DX has already turned sub into add
 * of the negated operand, .xy into .xyyy, and a scalar operand of vs_1_1 without a component
 * into .w. */
static const struct { const char *shader, *statement; } g_statements[] = {
    { "all11", "vec4 r7 = vec4(0.0);" },
    { "all11", "ivec4 a0 = ivec4(0);" },
    { "all11", "r2 = (-v1.zyxw);" },
    { "all11", "r3 = v2.xxxx;" },
    { "all11", "r4 = v3.xyyy;" },
    { "all11", "r5 = v4.zyxx;" },
    { "all11", "r7 = c[42];" },
    { "all11", "r1.x = (r1.yyyy + (-c[40].zzzz)).x;" },
    { "all11", "r2.yw = (r2 + (-v1)).yw;" },
    { "all11", "r3 = r3 + c[1].wzyx;" },
    { "all11", "r4.xyz = (r4 * c[40].yyyy).xyz;" },
    { "all11", "r5 = r5 * c[3].xxxx + c[3].yyyy;" },
    { "all11", "r6.xz = min(r6, r5).xz;" },
    { "all11", "r7 = max(r7, (-r6.wwww));" },
    { "all11", "r0.w = vec4(dot(r1.xyz, r2.xyz)).w;" },
    { "all11", "r1.x = vec4(dot(v0, c[7])).x;" },
    { "all11", "r2 = vec4(lessThan(r2, c[40].xxxx));" },
    { "all11", "r3.zw = vec4(greaterThanEqual(r3, r4.xxxx)).zw;" },
    { "all11", "r4.w = vec4(r0.w == 0.0 ? 3.402823466e+38 : 1.0 / r0.w).w;" },
    { "all11", "r5 = vec4(r0.w == 0.0 ? 3.402823466e+38 : 1.0 / r0.w);" },
    { "all11", "r6.xy = vec4((-r1.x) == 0.0 ? 3.402823466e+38 : inversesqrt(abs((-r1.x)))).xy;" },
    { "all11", "r6.zw = vec4(c[40].w == 0.0 ? 3.402823466e+38 : inversesqrt(abs(c[40].w))).zw;" },
    { "all11", "r7.x = vec4(exp2(r0.y)).x;" },
    { "all11", "r7 = vec4(exp2(floor(r1.x)), r1.x - floor(r1.x), exp2(r1.x), 1.0);" },
    { "all11", "r0.z = vec4(r1.x == 0.0 ? -3.402823466e+38 : log2(abs(r1.x))).z;" },
    { "all11", "r0.yz = vec4(c[40].z == 0.0 ? -3.402823466e+38 : log2(abs(c[40].z))).yz;" },
    { "all11", "r1 = vec4(1.0, max(r2.x, 0.0), ((r2.x > 0.0 && r2.y > 0.0) ? pow(r2.y, clamp(r2.w, -127.9961, 127.9961)) : 0.0), 1.0);" },
    { "all11", "r2 = vec4(1.0, r3.y * r4.y, r3.z, r4.w);" },
    { "all11", "r3.xy = fract(r4).xy;" },
    { "all11", "r4 = vec4(dot(v0, c[8]), dot(v0, c[9]), dot(v0, c[10]), dot(v0, c[11]));" },
    { "all11", "r5.xyz = vec4(dot(v0, c[12]), dot(v0, c[13]), dot(v0, c[14]), 0.0).xyz;" },
    { "all11", "r6 = vec4(dot(v1.xyz, c[16].xyz), dot(v1.xyz, c[17].xyz), dot(v1.xyz, c[18].xyz), dot(v1.xyz, c[19].xyz));" },
    { "all11", "r7.xyz = vec4(dot(v1.xyz, c[20].xyz), dot(v1.xyz, c[21].xyz), dot(v1.xyz, c[22].xyz), 0.0).xyz;" },
    { "all11", "r0.xy = vec4(dot(v4.xyz, c[24].xyz), dot(v4.xyz, c[25].xyz), 0.0, 0.0).xy;" },
    { "all11", "r1.xyz = vec4(dot(v9.xyz, r4.xyz), dot(v9.xyz, r5.xyz), dot(v9.xyz, r6.xyz), 0.0).xyz;" },
    { "all11", "a0.x = ivec4(floor(r3.zzzz)).x;" },
    { "all11", "r1 = c[clamp(a0.x + 44, 0, 255)];" },
    { "all11", "r3 = (-c[clamp(a0.x + 44, 0, 255)].wzyx);" },
    { "all11", "r6.xyz = vec4(dot(v0, c[clamp(a0.x + 50, 0, 255)]), dot(v0, c[clamp(a0.x + 51, 0, 255)]), dot(v0, c[clamp(a0.x + 52, 0, 255)]), 0.0).xyz;" },
    { "all11", "r7 = v6.xxxx * c[clamp(a0.x + 60, 0, 255)] + c[clamp(a0.x + 60, 0, 255)].wwww;" },
    { "all11", "oPos = r4;" },
    { "all11", "oD1.xyz = (r5).xyz;" },
    { "all11", "oD1.w = (c[40].xxxx).w;" },
    { "all11", "oT1.xy = (v5).xy;" },
    { "all11", "oT7 = r3;" },
    { "all11", "oFog = r0.wwww;" },
    { "all11", "oPts = v8.xxxx * c[40].yyyy + c[40].xxxx;" },
    { "all20", "r3 = abs(r0);" },
    { "all20", "r4 = sign(r1);" },
    { "all20", "r5.xyz = (r1 * (dot(r1.xyz, r1.xyz) == 0.0 ? 3.402823466e+38 : inversesqrt(dot(r1.xyz, r1.xyz)))).xyz;" },
    { "all20", "r8.xz = vec4(cross(r1.xyz, r2.xyz), 0.0).xz;" },
    { "all20", "r3 = mix(r1, r0, r4);" },
    { "all20", "r5.w = vec4(pow(abs(c[30].y), r0.w)).w;" },
    { "all20", "r7.y = vec4(cos(r2.w), sin(r2.w), 0.0, 0.0).y;" },
    { "all20", "a0 = ivec4(floor(abs(r0) + 0.5) * sign(r0));" },
    { "all20", "a0.xw = ivec4(floor(abs(r1) + 0.5) * sign(r1)).xw;" },
    { "all20", "r2 = c[clamp(a0.y + 5, 0, 255)];" },
    { "all20", "r3 = r3 + (-c[clamp(a0.w + 40, 0, 255)].zyxw);" },
    { "all20", "r4.xyz = vec4(dot(v0, c[clamp(a0.z + 8, 0, 255)]), dot(v0, c[clamp(a0.z + 9, 0, 255)]), dot(v0, c[clamp(a0.z + 10, 0, 255)]), 0.0).xyz;" },
    { "all20", "r7 = fract(r0);" },
    { "all20", "r8.w = vec4(exp2(r0.x)).w;" },
    { "all20", "r0 = r0 + (-r1);" },
};

static const shader_case *find_case(const char *name) {
    for (size_t i = 0; i < NCASES; i++) if (!strcmp(g_cases[i].name, name)) return &g_cases[i];
    return NULL;
}

static void check_glsl(void) {
    d3d9_vs_info info;
    char *text = d3d9_vs_glsl(find_case("skin")->tokens, &info);
    ok("skin", "GLSL text", text && !strcmp(text, glsl_skin));
    if (text && strcmp(text, glsl_skin)) printf("%s", text);
    free(text);

    for (size_t i = 0; i < sizeof g_statements / sizeof g_statements[0]; i++) {
        char line[256];
        snprintf(line, sizeof line, "    %s", g_statements[i].statement);
        text = d3d9_vs_glsl(find_case(g_statements[i].shader)->tokens, NULL);       /* info is optional */
        ok(g_statements[i].shader, g_statements[i].statement, has_line(text, line));
        free(text);
    }
}

/* What the translator reports about a shader beside its text. */
static void check_info(void) {
    static const uint8_t usage11[12][2] = {     /* all11's dcl lines: D3DDECLUSAGE, index */
        { 0, 0 }, { 3, 0 }, { 10, 0 }, { 10, 1 }, { 5, 0 }, { 5, 3 }, { 1, 0 }, { 2, 0 }, { 4, 0 }, { 6, 0 }, { 7, 0 }, { 11, 0 },
    };
    static const float def11[3][4] = { { 1, 0.5f, -2.25f, 1e-3f }, { 16, 8, 0.75f, 1.5f }, { -3, 4, 1e10f, 1 } };
    d3d9_vs_info info;
    char *text = d3d9_vs_glsl(find_case("all11")->tokens, &info);
    int good = text != NULL;
    for (int i = 0; i < 16; i++) {
        if (i < 12) good &= info.in[i].declared == 1 && info.in[i].usage == usage11[i][0] && info.in[i].usage_index == usage11[i][1];
        else good &= info.in[i].declared == 0;
    }
    ok("all11", "inputs", good);
    ok("all11", "outputs", info.outputs == (D3D9_VS_OUT_POS | D3D9_VS_OUT_D0 | D3D9_VS_OUT_D1 | D3D9_VS_OUT_FOG | D3D9_VS_OUT_PTS | 0xFF00u));
    ok("all11", "a relative read makes every constant live", info.consts == 256);
    good = info.ndefs == 3;
    for (int i = 0; good && i < 3; i++) good = info.def[i].reg == 40 + i && !memcmp(info.def[i].v, def11[i], sizeof def11[i]);
    ok("all11", "defs", good);
    free(text);

    text = d3d9_vs_glsl(find_case("loose")->tokens, &info);
    ok("loose", "outputs", text && info.outputs == (D3D9_VS_OUT_POS | D3D9_VS_OUT_D0 | D3D9_VS_OUT_FOG | D3D9_VS_OUT_T(0)));
    ok("loose", "consts: m4x4 at c0 reads c0-c3, then c4 and c5", info.consts == 6 && info.ndefs == 0);
    ok("loose", "inputs", info.in[0].declared && info.in[0].usage == 0 && info.in[1].declared && info.in[1].usage == 5 && !info.in[2].declared);
    free(text);

    text = d3d9_vs_glsl(find_case("all20")->tokens, &info);
    ok("all20", "outputs", text && info.outputs == (D3D9_VS_OUT_POS | D3D9_VS_OUT_D0 | D3D9_VS_OUT_FOG | D3D9_VS_OUT_T(0) | D3D9_VS_OUT_T(1)));
    ok("all20", "def", info.ndefs == 1 && info.def[0].reg == 30 && info.def[0].v[0] == 0.25f && info.def[0].v[2] == -1.0f);
    free(text);

    /* the caller's array is c[256]: a matrix whose last row is c255 fits, one row further does not */
    uint32_t *tok = NULL;
    size_t n = 0;
    ok("limits", "m4x4 at c252 assembles", assembles("vs_1_1 dcl_position v0 m4x4 oPos, v0, c252", &tok, &n));
    text = tok ? d3d9_vs_glsl(tok, &info) : NULL;
    ok("limits", "m4x4 at c252 reads up to c255", text && info.consts == 256);
    free(text);
    free(tok);
    tok = NULL;
    ok("limits", "m4x4 at c253 assembles", assembles("vs_1_1 dcl_position v0 m4x4 oPos, v0, c253", &tok, &n));
    ok("limits", "m4x4 at c253 has no translation", tok && d3d9_vs_glsl(tok, &info) == NULL);
    free(tok);

    /* info has room for 32 defs */
    for (int ndefs = 32; ndefs <= 33; ndefs++) {
        char src[2048];
        size_t len = (size_t)snprintf(src, sizeof src, "vs_1_1\n");
        for (int i = 0; i < ndefs; i++) len += (size_t)snprintf(src + len, sizeof src - len, "def c%d, %d, 0, 0, 1\n", 100 + i, i);
        snprintf(src + len, sizeof src - len, "mov oPos, c100\n");
        tok = NULL;
        ok("limits", "defs assemble", assembles(src, &tok, &n));
        text = tok ? d3d9_vs_glsl(tok, &info) : NULL;
        if (ndefs == 32) ok("limits", "32 defs", text && info.ndefs == 32 && info.def[31].reg == 131 && info.def[31].v[0] == 31.0f && info.consts == 101);
        else ok("limits", "33 defs are one too many", text == NULL);
        free(text);
        free(tok);
    }
}

/* ---- the listing's floats ---- */

/* def is the one place a float is printed, "%.9g" as the C runtime D3DX shipped with did it:
 * three exponent digits, ties away from zero, its own words for infinity and NaN. */
static const struct { uint32_t bits[4]; const char *line; } g_floats[] = {
    { { 0x3DCCCCCD, 0x501502F9, 0x3727C5AC, 0x40490FDB }, "    def c5, 0.100000001, 1e+010, 9.99999975e-006, 3.14159274" },
    { { 0x4CEB79A3, 0x3EAAAAAB, 0x2F24ED3F, 0x4B800000 }, "    def c5, 123456792, 0.333333343, 1.49999999e-010, 16777216" },
    { { 0x7E967699, 0xBAA3D70A, 0x47C35000, 0x4E6E6B28 }, "    def c5, 9.99999968e+037, -0.00124999997, 100000, 1e+009" },
    { { 0x4996B439, 0x3F804000, 0x19416D9A, 0x38D1B717 }, "    def c5, 1234567.13, 1.00195313, 1e-023, 9.99999975e-005" },
    { { 0x00000000, 0x80000000, 0x00000001, 0x80000001 }, "    def c5, 0, -0, 1.40129846e-045, -1.40129846e-045" },
    { { 0x7F800000, 0xFF800000, 0x7FC00000, 0xFFC00000 }, "    def c5, 1.#INF, -1.#INF, 1.#QNAN, -1.#IND" },
    { { 0x7F7FFFFF, 0x00800000, 0x3F000000, 0xC0000000 }, "    def c5, 3.40282347e+038, 1.17549435e-038, 0.5, -2" },
};

static void check_floats(void) {
    for (size_t i = 0; i < sizeof g_floats / sizeof g_floats[0]; i++) {
        const uint32_t *b = g_floats[i].bits;
        uint32_t tok[8] = { 0xFFFE0101u, 0x00000051u, 0xA00F0005u, b[0], b[1], b[2], b[3], 0x0000FFFFu };
        char *text = d3d9_vs_disassemble(tok);
        ok("floats", g_floats[i].line, has_line(text, g_floats[i].line));
        ok("floats", "a def is six tokens whatever its floats look like", d3d9_vs_tokens(tok) == 8);
        free(text);
    }
}

/* ---- token streams no assembler of ours makes ---- */

/* vs_2_0 with an if and a rep in it, as D3DX assembles it. */
static const uint32_t tok_flow[] = {
    0xFFFE0200, 0x0200001F, 0x80000000, 0x900F0000, 0x0200002F, 0xE00F0800, 0x00000001, 0x05000030,
    0xF00F0000, 0x00000004, 0x00000000, 0x00000001, 0x00000000, 0x02000001, 0x800F0000, 0x90E40000,
    0x01000028, 0xE0E40800, 0x03000002, 0x800F0000, 0x80E40000, 0xA0E40001, 0x0000002B, 0x01000026,
    0xF0E40000, 0x03000002, 0x800F0000, 0x80E40000, 0xA0E40003, 0x00000027, 0x02000001, 0xC00F0000,
    0x80E40000, 0x0000FFFF,
};

static void check_streams(void) {
    d3d9_vs_info info;
    char *text;

    /* D3DX writes a - b as a + -b; other tools emit the sub opcode */
    static const uint32_t sub[] = { 0xFFFE0101u, 0x00000003u, 0x800F0000u, 0x90E40000u, 0xA0E40001u, 0x0000FFFFu };
    text = d3d9_vs_disassemble(sub);
    ok("sub", "listing", text && !strcmp(text, "    vs_1_1\n    sub r0, v0, c1\n\n// approximately 1 instruction slot used\n"));
    free(text);
    text = d3d9_vs_glsl(sub, &info);
    ok("sub", "GLSL", text && !strcmp(text, "    vec4 r0 = vec4(0.0);\n    r0 = v0 - c[1];\n") && info.consts == 2 && info.outputs == 0);
    free(text);

    /* a compiler's comment block (its constant table) is stepped over */
    static const uint32_t commented[] = { 0xFFFE0101u, 0x0003FFFEu, 0x42415443u, 0, 0, 0x00000001u, 0xC00F0000u, 0x90E40000u, 0x0000FFFFu };
    ok("comment", "token count", d3d9_vs_tokens(commented) == 9);
    text = d3d9_vs_glsl(commented, &info);
    ok("comment", "GLSL", text && !strcmp(text, "    oPos = v0;\n") && info.outputs == D3D9_VS_OUT_POS && info.consts == 0);
    free(text);

    /* nothing but the version */
    static const uint32_t empty[] = { 0xFFFE0101u, 0x0000FFFFu };
    text = d3d9_vs_disassemble(empty);
    ok("empty", "listing", text && !strcmp(text, "    vs_1_1\n\n// approximately 0 instruction slots used\n"));
    free(text);
    text = d3d9_vs_glsl(empty, &info);
    ok("empty", "GLSL is an empty string, not NULL", text && !*text);
    free(text);

    /* flow control: its length is known, its meaning is not taken on */
    ok("flow", "token count", d3d9_vs_tokens(tok_flow) == sizeof tok_flow / 4);
    ok("flow", "no listing", d3d9_vs_disassemble(tok_flow) == NULL);
    ok("flow", "no GLSL", d3d9_vs_glsl(tok_flow, &info) == NULL);

    /* vs_3_0 is counted (an instruction says how long it is) and otherwise refused */
    static const uint32_t vs3[] = { 0xFFFE0300u, 0x0200001Fu, 0x80000000u, 0x900F0000u, 0x02000001u, 0xE00F0000u, 0x90E40000u, 0x0000FFFFu };
    ok("vs_3_0", "token count", d3d9_vs_tokens(vs3) == 8);
    ok("vs_3_0", "refused", d3d9_vs_disassemble(vs3) == NULL && d3d9_vs_glsl(vs3, &info) == NULL);

    /* a pixel shader is not ours at all */
    static const uint32_t ps[] = { 0xFFFF0101u, 0x00000001u, 0x800F0000u, 0x90E40000u, 0x0000FFFFu };
    ok("pixel shader", "refused", d3d9_vs_tokens(ps) == 0 && d3d9_vs_disassemble(ps) == NULL && d3d9_vs_glsl(ps, &info) == NULL);

    /* a register outside what the GLSL side declares: assembles, as D3DX has it, but has no translation */
    uint32_t *tok = NULL;
    size_t n = 0;
    ok("c256", "assembles", assembles("vs_1_1 mov oPos, c256", &tok, &n) && n == 5 && tok[3] == 0xA0E40100u);
    ok("c256", "no GLSL", tok && d3d9_vs_glsl(tok, &info) == NULL);
    free(tok);
}

/* ---- what the assembler turns down ---- */

static const struct { const char *src; int line; const char *about; } g_errors[] = {
    { "mov oPos, v0",                                   1, "shader version expected" },
    { "vs_3_0\nmov oPos, v0",                           1, "not supported" },
    { "ps_1_1\nmov r0, c0",                             1, "not supported" },
    { "vs_1_1\nfoo r0, v0",                             2, "unexpected token 'foo'" },
    { "vs_1_1\nMOV r0, v0",                             2, "unexpected token 'MOV'" },
    { "vs_1_1\nmov r12, v0",                            2, "invalid register 'r12'" },
    { "vs_1_1\nmov r0, V0",                             2, "invalid register 'V0'" },
    { "vs_1_1\nmov oT8, v0",                            2, "invalid register 'oT8'" },
    { "vs_1_1\nmov r0, c",                              2, "invalid register 'c'" },
    { "vs_1_1\nmov r0.yx, v0",                          2, "invalid mask 'yx'" },
    { "vs_1_1\nmov r0, v0.xyzwx",                       2, "invalid swizzle 'xyzwx'" },
    { "vs_1_1\nmov r0, v0.q",                           2, "invalid swizzle 'q'" },
    { "vs_1_1\nadd r0, v0",                             2, "unexpected end of file" },
    { "vs_1_1\nmov r0, v0, v1",                         2, "unexpected token ','" },
    { "vs_1_1\nmov r0 v0",                              2, "unexpected token 'v0'" },
    { "vs_1_1\nmov -r0, v0",                            2, "unexpected token '-'" },
    { "vs_1_1\nabs r0, v0",                             2, "not a valid instruction in this shader version" },
    { "vs_2_0\nmov r0, c0\nif b0\nendif",               3, "not supported" },
    { "vs_1_1\nmov oPos, v0\ndcl_position v0",          3, "must appear before" },
    { "vs_1_1\nmov oPos, v0\ndef c0, 1, 2, 3, 4",       3, "must appear before" },
    { "vs_1_1\ndcl_position r0",                        2, "input register" },
    { "vs_1_1\ndcl_wibble v0",                          2, "unexpected token 'dcl_wibble'" },
    { "vs_1_1\ndef c0, 1, 2, 3",                        2, "unexpected end of file" },
    { "vs_1_1\ndef c0, 1, 2, 3, x",                     2, "unexpected token 'x'" },
    { "vs_1_1\ndef r0, 1, 2, 3, 4",                     2, "constant register" },
    { "vs_1_1\nmov oFog.x, v0.x",                       2, "scalar registers cannot be masked" },
    { "vs_1_1\nm4x3 r0.xy, v0, c0",                     2, "write mask .xyz" },
    { "vs_2_0\nm3x2 r0, v0, c0",                        2, "write mask .xy" },
    { "vs_1_1\nfrc r0, v0",                             2, "frc" },
    { "vs_1_1\nrsq r0, r1.xy",                          2, "replicate swizzle" },
    { "vs_2_0\nrsq r0, r1",                             2, "replicate swizzle" },
    { "vs_1_1\nmov r0, c[a0.y + 1]",                    2, "only a0.x" },
    { "vs_1_1\nmov r0, c[a0.x + a0.x]",                 2, "only one address register" },
    { "vs_2_0\nmov r0, c[a0 + 1]",                      2, "single component" },
    { "vs_1_1\nmov r0, c[1",                            2, "unexpected end of file" },
    { "vs_1_1\nmov r0, v[1]",                           2, "invalid register 'v'" },
    { "vs_1_1\nmov r0, r1[2]",                          2, "invalid register 'r1[...]'" },
    { "vs_1_1\nmov a0, r0",                             2, "a0.x" },
    { "vs_1_1\nadd a0.x, r0, r1",                       2, "address register" },
    { "vs_2_0\nmov a0.x, r0",                           2, "address register" },
    { "vs_1_1\nmov c0, r0",                             2, "destination" },
    { "vs_1_1\nmov r0, oPos",                           2, "source 1" },
    { "vs_1_1\n\n\nmov r0, /* never closed",            4, "comment continues past end of file" },
    { "vs_1_1\n#define X c0",                           2, "preprocessor" },
    { "vs_1_1\nmov r0, c0 @",                           2, "unexpected character '@'" },
};

static void check_errors(void) {
    for (size_t i = 0; i < sizeof g_errors / sizeof g_errors[0]; i++) {
        uint32_t *tok = (uint32_t *)&fails;         /* anything but NULL: a failure has to clear it */
        size_t n = 99;
        char *err = NULL, where[32];
        int rc = d3d9_vs_assemble(g_errors[i].src, strlen(g_errors[i].src), &tok, &n, &err);
        snprintf(where, sizeof where, "line %d: ", g_errors[i].line);
        ok(g_errors[i].src, g_errors[i].about,
           rc == -1 && tok == NULL && err && !strncmp(err, where, strlen(where)) && strstr(err, g_errors[i].about));
        if (rc == 0) free(tok);
        else if (err && (strncmp(err, where, strlen(where)) || !strstr(err, g_errors[i].about))) printf("  said: %s\n", err);
        free(err);
    }

    /* the text is len bytes, not a C string: what lies beyond is not read */
    static const char two[] = "vs_1_1\nmov oPos, c0\nmov oD0, c1\n";
    uint32_t *tok = NULL;
    size_t n = 0;
    ok("len", "stops at len", d3d9_vs_assemble(two, 20, &tok, &n, NULL) == 0 && n == 5 && tok[1] == 0x00000001u && tok[4] == 0x0000FFFFu);
    free(tok);
    tok = NULL;
    ok("len", "and at a NUL before it", d3d9_vs_assemble("vs_1_1\nmov oPos, c0\0mov oD0", 28, &tok, &n, NULL) == 0 && n == 5);
    free(tok);
}

int main(void) {
    for (size_t i = 0; i < NCASES; i++) check_case(&g_cases[i]);
    check_glsl();
    check_info();
    check_floats();
    check_streams();
    check_errors();
    if (fails == 0) printf("d3d9_vs_selftest: all checks passed\n");
    return fails != 0;
}
