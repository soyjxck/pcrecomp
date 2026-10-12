/*
 * d3dx9.c — the part of D3DX 9 (d3dx9_NN.dll) that games of the fixed-function
 * years lean on: the vector, matrix and quaternion maths, textures from DDS
 * and BMP files, copies between surfaces, and the vertex shader assembler.
 *
 * Meshes and X files are not here: the loaders answer as D3DX does for a file
 * it cannot find, and say so once.
 *
 * Built only with HLE_WITH_D3D9, beside d3d9.c.
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win32hle.h"

#ifndef HLE_WITH_D3D9
const win32hle_shim win32hle_d3dx9[] = { { NULL, NULL } };
#else
#include "d3d9_priv.h"

#define D3DXERR_INVALIDDATA 0x88760B59u
#define D3DXFERR_FILENOTFOUND 0x88760389u
#define D3DX_DEFAULT 0xFFFFFFFFu

static float AF(int i) { uint32_t u = A32(i); float f; memcpy(&f, &u, 4); return f; }
static const float *FP(int i) { return (const float *)G2H(A32(i)); }
static void once(const char *what) {
    static const char *said[32];
    static int n;
    for (int i = 0; i < n; i++) if (said[i] == what) return;
    if (n < 32) said[n++] = what;
    fprintf(stderr, "[d3dx] not done: %s\n", what);
}

/* ---- maths: vectors on the left, matrices in rows, as Direct3D has them ---------- */
static void m_identity(float *m) { memset(m, 0, 64); m[0] = m[5] = m[10] = m[15] = 1.0f; }
static void m_mul(float *r, const float *a, const float *b) {
    float t[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            t[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
    memcpy(r, t, 64);
}
static int m_inverse(float *r, const float *m, float *det_out) {
    float inv[16], det;
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (det_out) *det_out = det;
    if (det == 0.0f) return 0;
    for (int i = 0; i < 16; i++) r[i] = inv[i] / det;
    return 1;
}
static void v_coord(float *o, const float *v, const float *m) {      /* (v, 1) through m, then divided by w */
    float x = v[0], y = v[1], z = v[2], w = x * m[3] + y * m[7] + z * m[11] + m[15];
    o[0] = (x * m[0] + y * m[4] + z * m[8] + m[12]) / w;
    o[1] = (x * m[1] + y * m[5] + z * m[9] + m[13]) / w;
    o[2] = (x * m[2] + y * m[6] + z * m[10] + m[14]) / w;
}
static void v_normalize(float *o, const float *v) {
    float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (len == 0.0f) o[0] = o[1] = o[2] = 0.0f;
    else o[0] = v[0] / len, o[1] = v[1] / len, o[2] = v[2] / len;
}
static void v_cross(float *o, const float *a, const float *b) {
    float t[3] = { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
    memcpy(o, t, 12);
}
static float v_dot(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
/* world, view and projection as one, any of them left out */
static void wvp_of(float *m, uint32_t proj, uint32_t view, uint32_t world) {
    m_identity(m);
    if (world) m_mul(m, m, G2H(world));
    if (view) m_mul(m, m, G2H(view));
    if (proj) m_mul(m, m, G2H(proj));
}
#define OUT(i) ((float *)G2H(A32(i)))

static void x_MatrixMultiply(void) { m_mul(OUT(0), FP(1), FP(2)); RET(A32(0), 3); }
static void x_MatrixTranspose(void) {
    float t[16];
    const float *m = FP(1);
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) t[i * 4 + j] = m[j * 4 + i];
    memcpy(OUT(0), t, 64);
    RET(A32(0), 2);
}
static void x_MatrixInverse(void) {            /* (out, &determinant, m) */
    float det, r[16];
    int ok = m_inverse(r, FP(2), &det);
    if (A32(1)) memcpy(G2H(A32(1)), &det, 4);
    if (!ok) RET(0, 3);
    memcpy(OUT(0), r, 64);
    RET(A32(0), 3);
}
static void x_MatrixLookAtLH(void) {           /* (out, eye, at, up) */
    const float *eye = FP(1), *at = FP(2), *up = FP(3);
    float z[3] = { at[0] - eye[0], at[1] - eye[1], at[2] - eye[2] }, x[3], y[3], m[16];
    v_normalize(z, z);
    v_cross(x, up, z);
    v_normalize(x, x);
    v_cross(y, z, x);
    m[0] = x[0], m[1] = y[0], m[2] = z[0], m[3] = 0;
    m[4] = x[1], m[5] = y[1], m[6] = z[1], m[7] = 0;
    m[8] = x[2], m[9] = y[2], m[10] = z[2], m[11] = 0;
    m[12] = -v_dot(x, eye), m[13] = -v_dot(y, eye), m[14] = -v_dot(z, eye), m[15] = 1;
    memcpy(OUT(0), m, 64);
    RET(A32(0), 4);
}
static void x_MatrixPerspectiveFovLH(void) {   /* (out, angle up and down, width over height, near, far) */
    float m[16] = { 0 }, ys = 1.0f / tanf(AF(1) / 2.0f), zn = AF(3), zf = AF(4);
    m[0] = ys / AF(2), m[5] = ys, m[10] = zf / (zf - zn), m[11] = 1.0f, m[14] = (zf * zn) / (zn - zf);
    memcpy(OUT(0), m, 64);
    RET(A32(0), 5);
}
static void x_MatrixRotationAxis(void) {       /* (out, axis, angle) */
    float v[3], m[16], s = sinf(AF(2)), c = cosf(AF(2)), d = 1.0f - c;
    v_normalize(v, FP(1));
    m_identity(m);
    m[0] = d * v[0] * v[0] + c, m[4] = d * v[0] * v[1] - s * v[2], m[8] = d * v[0] * v[2] + s * v[1];
    m[1] = d * v[1] * v[0] + s * v[2], m[5] = d * v[1] * v[1] + c, m[9] = d * v[1] * v[2] - s * v[0];
    m[2] = d * v[2] * v[0] - s * v[1], m[6] = d * v[2] * v[1] + s * v[0], m[10] = d * v[2] * v[2] + c;
    memcpy(OUT(0), m, 64);
    RET(A32(0), 3);
}
static void x_MatrixRotationQuaternion(void) { /* (out, q) */
    const float *q = FP(1);
    float m[16], x = q[0], y = q[1], z = q[2], w = q[3];
    m_identity(m);
    m[0] = 1.0f - 2.0f * (y * y + z * z), m[1] = 2.0f * (x * y + z * w), m[2] = 2.0f * (x * z - y * w);
    m[4] = 2.0f * (x * y - z * w), m[5] = 1.0f - 2.0f * (x * x + z * z), m[6] = 2.0f * (y * z + x * w);
    m[8] = 2.0f * (x * z + y * w), m[9] = 2.0f * (y * z - x * w), m[10] = 1.0f - 2.0f * (x * x + y * y);
    memcpy(OUT(0), m, 64);
    RET(A32(0), 2);
}
static void rotation(int a, int b) {           /* about the axis that is neither a nor b */
    float m[16], s = sinf(AF(1)), c = cosf(AF(1));
    m_identity(m);
    m[a * 4 + a] = c, m[b * 4 + b] = c, m[a * 4 + b] = s, m[b * 4 + a] = -s;
    memcpy(OUT(0), m, 64);
}
static void x_MatrixRotationX(void) { rotation(1, 2); RET(A32(0), 2); }
static void x_MatrixRotationY(void) { rotation(2, 0); RET(A32(0), 2); }
static void x_MatrixRotationZ(void) { rotation(0, 1); RET(A32(0), 2); }
static void x_MatrixRotationYawPitchRoll(void) {   /* (out, yaw, pitch, roll) */
    float m[16], sy = sinf(AF(1)), cy = cosf(AF(1)), sp = sinf(AF(2)), cp = cosf(AF(2)), sr = sinf(AF(3)), cr = cosf(AF(3));
    m_identity(m);
    m[0] = sr * sp * sy + cr * cy, m[1] = sr * cp, m[2] = sr * sp * cy - cr * sy;
    m[4] = cr * sp * sy - sr * cy, m[5] = cr * cp, m[6] = cr * sp * cy + sr * sy;
    m[8] = cp * sy, m[9] = -sp, m[10] = cp * cy;
    memcpy(OUT(0), m, 64);
    RET(A32(0), 4);
}
static void x_MatrixScaling(void) { float m[16]; m_identity(m); m[0] = AF(1), m[5] = AF(2), m[10] = AF(3); memcpy(OUT(0), m, 64); RET(A32(0), 4); }
static void x_MatrixTranslation(void) { float m[16]; m_identity(m); m[12] = AF(1), m[13] = AF(2), m[14] = AF(3); memcpy(OUT(0), m, 64); RET(A32(0), 4); }
static void x_QuaternionRotationMatrix(void) { /* (out, m) */
    const float *m = FP(1);
    float q[4], s, trace = m[0] + m[5] + m[10] + 1.0f;
    if (trace > 1.0f) {
        s = 2.0f * sqrtf(trace);
        q[0] = (m[6] - m[9]) / s, q[1] = (m[8] - m[2]) / s, q[2] = (m[1] - m[4]) / s, q[3] = 0.25f * s;
    } else {
        int big = 0;
        if (m[5] > m[0]) big = 1;
        if (m[10] > m[big * 5]) big = 2;
        if (big == 0) {
            s = 2.0f * sqrtf(1.0f + m[0] - m[5] - m[10]);
            q[0] = 0.25f * s, q[1] = (m[1] + m[4]) / s, q[2] = (m[2] + m[8]) / s, q[3] = (m[6] - m[9]) / s;
        } else if (big == 1) {
            s = 2.0f * sqrtf(1.0f + m[5] - m[0] - m[10]);
            q[0] = (m[1] + m[4]) / s, q[1] = 0.25f * s, q[2] = (m[6] + m[9]) / s, q[3] = (m[8] - m[2]) / s;
        } else {
            s = 2.0f * sqrtf(1.0f + m[10] - m[0] - m[5]);
            q[0] = (m[2] + m[8]) / s, q[1] = (m[6] + m[9]) / s, q[2] = 0.25f * s, q[3] = (m[1] - m[4]) / s;
        }
    }
    memcpy(OUT(0), q, 16);
    RET(A32(0), 2);
}
static void x_QuaternionSlerp(void) {          /* (out, from, to, how far) */
    const float *a = FP(1), *b = FP(2);
    float t = AF(3), keep = 1.0f - t, dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3], q[4];
    if (dot < 0.0f) t = -t, dot = -dot;
    if (1.0f - dot > 0.001f) {
        float theta = acosf(dot);
        keep = sinf(theta * keep) / sinf(theta), t = sinf(theta * t) / sinf(theta);
    }
    for (int i = 0; i < 4; i++) q[i] = keep * a[i] + t * b[i];
    memcpy(OUT(0), q, 16);
    RET(A32(0), 4);
}
static void x_QuaternionToAxisAngle(void) {    /* (q, axis, &angle) */
    const float *q = FP(0);
    float angle = 2.0f * acosf(q[3]);
    if (A32(1)) memcpy(G2H(A32(1)), q, 12);
    if (A32(2)) memcpy(G2H(A32(2)), &angle, 4);
    RETV(3);
}
static void x_Vec3Hermite(void) {              /* (out, v1, t1, v2, t2, s) */
    const float *v1 = FP(1), *t1 = FP(2), *v2 = FP(3), *t2 = FP(4);
    float s = AF(5), h1 = 2 * s * s * s - 3 * s * s + 1, h2 = s * s * s - 2 * s * s + s, h3 = -2 * s * s * s + 3 * s * s, h4 = s * s * s - s * s, o[3];
    for (int i = 0; i < 3; i++) o[i] = h1 * v1[i] + h2 * t1[i] + h3 * v2[i] + h4 * t2[i];
    memcpy(OUT(0), o, 12);
    RET(A32(0), 6);
}
static void x_Vec3Normalize(void) { float o[3]; v_normalize(o, FP(1)); memcpy(OUT(0), o, 12); RET(A32(0), 2); }
static void x_Vec3Transform(void) {            /* (out of four, v, m) */
    const float *v = FP(1), *m = FP(2);
    float o[4];
    for (int j = 0; j < 4; j++) o[j] = v[0] * m[j] + v[1] * m[4 + j] + v[2] * m[8 + j] + m[12 + j];
    memcpy(OUT(0), o, 16);
    RET(A32(0), 3);
}
static void x_Vec3TransformCoord(void) { float o[3]; v_coord(o, FP(1), FP(2)); memcpy(OUT(0), o, 12); RET(A32(0), 3); }
/* D3DVIEWPORT9: x, y, width, height as integers, then the two depths. */
static void x_Vec3Project(void) {              /* (out, v, viewport, projection, view, world) */
    float m[16], o[3];
    wvp_of(m, A32(3), A32(4), A32(5));
    v_coord(o, FP(1), m);
    if (A32(2)) {
        uint32_t vp = A32(2);
        float minz, maxz;
        memcpy(&minz, G2H(vp + 16), 4), memcpy(&maxz, G2H(vp + 20), 4);
        o[0] = (float)MEM32(vp) + (1.0f + o[0]) * (float)MEM32(vp + 8) / 2.0f;
        o[1] = (float)MEM32(vp + 4) + (1.0f - o[1]) * (float)MEM32(vp + 12) / 2.0f;
        o[2] = minz + o[2] * (maxz - minz);
    }
    memcpy(OUT(0), o, 12);
    RET(A32(0), 6);
}
static void x_Vec3Unproject(void) {
    float m[16], o[3];
    wvp_of(m, A32(3), A32(4), A32(5));
    m_inverse(m, m, NULL);
    memcpy(o, FP(1), 12);
    if (A32(2)) {
        uint32_t vp = A32(2);
        float minz, maxz;
        memcpy(&minz, G2H(vp + 16), 4), memcpy(&maxz, G2H(vp + 20), 4);
        o[0] = 2.0f * (o[0] - (float)MEM32(vp)) / (float)MEM32(vp + 8) - 1.0f;
        o[1] = 1.0f - 2.0f * (o[1] - (float)MEM32(vp + 4)) / (float)MEM32(vp + 12);
        o[2] = (o[2] - minz) / (maxz - minz);
    }
    v_coord(o, o, m);
    memcpy(OUT(0), o, 12);
    RET(A32(0), 6);
}
static uint32_t fvf_size(uint32_t fvf) {
    static const int comps[] = { 2, 3, 4, 1 };
    uint32_t n = 0, pos = fvf & D3DFVF_POSITION_MASK;
    n = pos == D3DFVF_XYZRHW || pos == D3DFVF_XYZW ? 16 : pos >= D3DFVF_XYZB1 && pos <= D3DFVF_XYZB5 ? 12 + 4 * ((pos - D3DFVF_XYZB1) / 2 + 1) : pos ? 12 : 0;
    if (fvf & D3DFVF_NORMAL) n += 12;
    if (fvf & D3DFVF_PSIZE) n += 4;
    if (fvf & D3DFVF_DIFFUSE) n += 4;
    if (fvf & D3DFVF_SPECULAR) n += 4;
    for (uint32_t i = 0; i < ((fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT); i++) n += 4u * (uint32_t)comps[(fvf >> (16 + 2 * i)) & 3];
    return n;
}
static void x_GetFVFVertexSize(void) { RET(fvf_size(A32(0)), 1); }
static void x_ComputeBoundingBox(void) {       /* (first position, how many, stride, &lowest, &highest) */
    if (!A32(0) || !A32(3) || !A32(4)) RET(D3DERR_INVALIDCALL, 5);
    float lo[3], hi[3];
    memcpy(lo, G2H(A32(0)), 12), memcpy(hi, lo, 12);
    for (uint32_t i = 1; i < A32(1); i++) {
        float v[3];
        memcpy(v, G2H(A32(0) + i * A32(2)), 12);
        for (int k = 0; k < 3; k++) { if (v[k] < lo[k]) lo[k] = v[k]; if (v[k] > hi[k]) hi[k] = v[k]; }
    }
    memcpy(G2H(A32(3)), lo, 12), memcpy(G2H(A32(4)), hi, 12);
    RET(D3D_OK, 5);
}

/* ---- ID3DXBuffer: a block of bytes with a count ------------------------------------- */
static uint32_t g_vt_buffer;
static void xb_Release(void) {
    uint32_t self = A32(0), n = --MEM32(self + 4);
    if (!n) { hle_guest_free(MEM32(self + 8)); hle_guest_free(self); }
    RET(n, 1);
}
static void xb_GetBufferPointer(void) { RET(MEM32(A32(0) + 8), 1); }
static void xb_GetBufferSize(void) { RET(MEM32(A32(0) + 12), 1); }
static uint32_t buffer_of(const void *data, size_t n) {
    static const win32hle_shim m[] = {
        { "ID3DXBuffer::QueryInterface", hle_com_QueryInterface }, { "ID3DXBuffer::AddRef", hle_com_AddRef },
        { "ID3DXBuffer::Release", xb_Release }, { "ID3DXBuffer::GetBufferPointer", xb_GetBufferPointer },
        { "ID3DXBuffer::GetBufferSize", xb_GetBufferSize }, { NULL, NULL },
    };
    if (!g_vt_buffer) g_vt_buffer = hle_com_vtable(m);
    uint32_t o = hle_com_new(g_vt_buffer, 16), bytes = hle_guest_alloc((uint32_t)(n ? n : 1));
    if (!o || !bytes) return 0;
    memcpy(G2H(bytes), data, n);
    MEM32(o + 8) = bytes, MEM32(o + 12) = (uint32_t)n;
    return o;
}

/* ---- vertex shaders ------------------------------------------------------------------------ */
static void x_AssembleShader(void) {           /* (source, length, defines, include, flags, &shader, &errors) */
    uint32_t *tokens = NULL;
    size_t count = 0;
    char *error = NULL;
    if (A32(5)) MEM32(A32(5)) = 0;
    if (A32(6)) MEM32(A32(6)) = 0;
    if (!A32(0) || !A32(5)) RET(D3DERR_INVALIDCALL, 7);
    if (d3d9_vs_assemble(G2H(A32(0)), A32(1), &tokens, &count, &error) != 0) {
        fprintf(stderr, "[d3dx] AssembleShader: %s\n", error ? error : "failed");
        if (A32(6) && error) MEM32(A32(6)) = buffer_of(error, strlen(error) + 1);
        free(error);
        RET(D3DXERR_INVALIDDATA, 7);
    }
    MEM32(A32(5)) = buffer_of(tokens, count * 4);
    free(tokens);
    RET(D3D_OK, 7);
}
static void x_DisassembleShader(void) {        /* (tokens, colour, comments, &listing) */
    if (!A32(0) || !A32(3)) RET(D3DERR_INVALIDCALL, 4);
    char *text = d3d9_vs_disassemble(G2H(A32(0)));
    if (!text) { MEM32(A32(3)) = 0; RET(D3DXERR_INVALIDDATA, 4); }
    MEM32(A32(3)) = buffer_of(text, strlen(text) + 1);        /* with its ending zero, as D3DX's is */
    free(text);
    RET(D3D_OK, 4);
}

/* ---- pictures ------------------------------------------------------------------------------- */
typedef struct { int w, h, fmt, levels; uint8_t *bgra; } picture;   /* fmt: the Direct3D format the file is in */

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
/* One 4x4 block of DXT colour: two 5:6:5 colours and sixteen 2-bit choices. */
static void dxt_color(const uint8_t *b, uint8_t out[16][4], int dxt1) {
    unsigned c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8;
    uint8_t c[4][4];
    c[0][2] = (uint8_t)((c0 >> 11) * 255 / 31), c[0][1] = (uint8_t)(((c0 >> 5) & 63) * 255 / 63), c[0][0] = (uint8_t)((c0 & 31) * 255 / 31), c[0][3] = 255;
    c[1][2] = (uint8_t)((c1 >> 11) * 255 / 31), c[1][1] = (uint8_t)(((c1 >> 5) & 63) * 255 / 63), c[1][0] = (uint8_t)((c1 & 31) * 255 / 31), c[1][3] = 255;
    for (int k = 0; k < 3; k++) {
        if (c0 > c1 || !dxt1) c[2][k] = (uint8_t)((2 * c[0][k] + c[1][k]) / 3), c[3][k] = (uint8_t)((c[0][k] + 2 * c[1][k]) / 3);
        else c[2][k] = (uint8_t)((c[0][k] + c[1][k]) / 2), c[3][k] = 0;
    }
    c[2][3] = 255, c[3][3] = (c0 > c1 || !dxt1) ? 255 : 0;
    uint32_t bits = le32(b + 4);
    for (int i = 0; i < 16; i++, bits >>= 2) memcpy(out[i], c[bits & 3], 4);
}
static int dds_read(const uint8_t *d, size_t n, picture *p) {
    if (n < 128 || memcmp(d, "DDS ", 4)) return 0;
    uint32_t flags = le32(d + 80), fourcc = le32(d + 84), bits = le32(d + 88), rm = le32(d + 92), am = le32(d + 104);
    int w = (int)le32(d + 16), h = (int)le32(d + 12), fmt = 0;
    const uint8_t *px = d + 128;
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return 0;
    p->w = w, p->h = h, p->levels = le32(d + 28) ? (int)le32(d + 28) : 1;
    if (!(p->bgra = malloc((size_t)w * (size_t)h * 4))) return 0;
    if (flags & 4) {                           /* a four-character code: DXT1, DXT3, DXT5 */
        int kind = fourcc == 0x31545844 ? 1 : fourcc == 0x33545844 ? 3 : fourcc == 0x35545844 ? 5 : 0;
        size_t block = kind == 1 ? 8 : 16, need = (size_t)((w + 3) / 4) * (size_t)((h + 3) / 4) * block;
        if (!kind || n < 128 + need) { free(p->bgra); return 0; }
        for (int by = 0; by < h; by += 4)
            for (int bx = 0; bx < w; bx += 4, px += block) {
                uint8_t out[16][4];
                dxt_color(px + (kind == 1 ? 0 : 8), out, kind == 1);
                if (kind == 3) for (int i = 0; i < 16; i++) out[i][3] = (uint8_t)(((px[i / 2] >> (4 * (i & 1))) & 15) * 17);
                if (kind == 5) {
                    unsigned a0 = px[0], a1 = px[1];
                    uint64_t idx = 0;
                    for (int i = 0; i < 6; i++) idx |= (uint64_t)px[2 + i] << (8 * i);
                    for (int i = 0; i < 16; i++, idx >>= 3) {
                        unsigned c = (unsigned)(idx & 7);
                        out[i][3] = (uint8_t)(c == 0 ? a0 : c == 1 ? a1 : a0 > a1 ? ((8 - c) * a0 + (c - 1) * a1) / 7
                                              : c == 6 ? 0 : c == 7 ? 255 : ((6 - c) * a0 + (c - 1) * a1) / 5);
                    }
                }
                for (int i = 0; i < 16; i++)
                    if (bx + (i & 3) < w && by + (i >> 2) < h) memcpy(p->bgra + ((size_t)(by + (i >> 2)) * (size_t)w + (size_t)(bx + (i & 3))) * 4, out[i], 4);
            }
        p->fmt = D3DFMT_A8R8G8B8;
        return 1;
    }
    if (bits == 32) fmt = rm == 0xFF ? ((flags & 1) && am ? D3DFMT_A8B8G8R8 : D3DFMT_X8B8G8R8) : ((flags & 1) && am ? D3DFMT_A8R8G8B8 : D3DFMT_X8R8G8B8);
    else if (bits == 24) fmt = D3DFMT_R8G8B8;
    else if (bits == 16) fmt = rm == 0xF800 ? D3DFMT_R5G6B5 : rm == 0x7C00 ? (am ? D3DFMT_A1R5G5B5 : D3DFMT_X1R5G5B5)
                             : rm == 0x0F00 ? (am ? D3DFMT_A4R4G4B4 : D3DFMT_X4R4G4B4) : rm == 0x00FF ? D3DFMT_A8L8 : 0;
    else if (bits == 8) fmt = (flags & 2) && !(flags & 0x20000) ? D3DFMT_A8 : D3DFMT_L8;
    int bpp = d3d9_fmt_bytes(fmt);
    if (!bpp || n < 128 + (size_t)w * (size_t)h * (size_t)bpp) { free(p->bgra); return 0; }
    d3d9_to_bgra(fmt, px, p->bgra, w * h);
    p->fmt = fmt;
    return 1;
}
/* A Windows bitmap: 24 or 32 bits, or 8 with a colour table; not compressed. */
static int bmp_read(const uint8_t *d, size_t n, picture *p) {
    if (n < 54 || d[0] != 'B' || d[1] != 'M') return 0;
    uint32_t off = le32(d + 10), head = le32(d + 14);
    int w = (int)le32(d + 18), h = (int)le32(d + 22), bits = d[28] | d[29] << 8, flip = h > 0;
    if (h < 0) h = -h;
    size_t row = ((size_t)w * (size_t)bits / 8 + 3) & ~(size_t)3;
    if (le32(d + 30) != 0 || w <= 0 || h <= 0 || (bits != 8 && bits != 24 && bits != 32) || n < off + row * (size_t)h) return 0;
    if (!(p->bgra = malloc((size_t)w * (size_t)h * 4))) return 0;
    const uint8_t *pal = d + 14 + head;
    for (int y = 0; y < h; y++) {
        const uint8_t *s = d + off + row * (size_t)(flip ? h - 1 - y : y);
        uint8_t *o = p->bgra + (size_t)y * (size_t)w * 4;
        for (int x = 0; x < w; x++, o += 4) {
            if (bits == 8) memcpy(o, pal + 4 * s[x], 3);
            else memcpy(o, s + (size_t)x * (size_t)(bits / 8), 3);
            o[3] = 255;
        }
    }
    p->w = w, p->h = h, p->fmt = D3DFMT_X8R8G8B8, p->levels = 1;
    return 1;
}
/* The key colour, alpha included, becomes transparent black. */
static void color_key(uint8_t *bgra, size_t n, uint32_t key) {
    uint8_t k[4] = { (uint8_t)key, (uint8_t)(key >> 8), (uint8_t)(key >> 16), (uint8_t)(key >> 24) };
    if (!key) return;
    for (size_t i = 0; i < n; i++, bgra += 4) if (!memcmp(bgra, k, 4)) memset(bgra, 0, 4);
}
static uint8_t *scaled(const uint8_t *px, int w, int h, int tw, int th) {      /* by the nearest pixel */
    uint8_t *o = malloc((size_t)tw * (size_t)th * 4);
    for (int y = 0; o && y < th; y++)
        for (int x = 0; x < tw; x++)
            memcpy(o + ((size_t)y * (size_t)tw + (size_t)x) * 4, px + ((size_t)(y * h / th) * (size_t)w + (size_t)(x * w / tw)) * 4, 4);
    return o;
}
static uint8_t *halved(const uint8_t *px, int w, int h, int tw, int th) {      /* each pixel the mean of the four it covers */
    uint8_t *o = malloc((size_t)tw * (size_t)th * 4);
    for (int y = 0; o && y < th; y++)
        for (int x = 0; x < tw; x++) {
            int x0 = x * 2 < w ? x * 2 : w - 1, x1 = x * 2 + 1 < w ? x * 2 + 1 : w - 1, y0 = y * 2 < h ? y * 2 : h - 1, y1 = y * 2 + 1 < h ? y * 2 + 1 : h - 1;
            const uint8_t *a = px + ((size_t)y0 * (size_t)w + (size_t)x0) * 4, *b = px + ((size_t)y0 * (size_t)w + (size_t)x1) * 4;
            const uint8_t *c = px + ((size_t)y1 * (size_t)w + (size_t)x0) * 4, *e = px + ((size_t)y1 * (size_t)w + (size_t)x1) * 4;
            for (int k = 0; k < 4; k++) o[((size_t)y * (size_t)tw + (size_t)x) * 4 + (size_t)k] = (uint8_t)((a[k] + b[k] + c[k] + e[k] + 2) / 4);
        }
    return o;
}

/* A texture from a picture file's bytes: the size, levels and format asked
 * for, each defaulting to the file's own. */
static uint32_t texture_from(const uint8_t *data, size_t n, uint32_t w, uint32_t h, uint32_t levels, uint32_t usage, uint32_t fmt,
                             uint32_t pool, uint32_t key, uint32_t info, uint32_t out) {
    picture p = { 0 };
    if (!out) return D3DERR_INVALIDCALL;
    MEM32(out) = 0;
    if (!dds_read(data, n, &p) && !bmp_read(data, n, &p)) {
        fprintf(stderr, "[d3dx] a picture that is neither DDS nor BMP (%02X %02X %02X %02X, %u bytes)\n",
                n > 0 ? data[0] : 0, n > 1 ? data[1] : 0, n > 2 ? data[2] : 0, n > 3 ? data[3] : 0, (unsigned)n);
        return D3DXERR_INVALIDDATA;
    }
    if (info) {                                /* D3DXIMAGE_INFO: width, height, depth, levels, format, kind of resource, kind of file */
        MEM32(info) = (uint32_t)p.w, MEM32(info + 4) = (uint32_t)p.h, MEM32(info + 8) = 1, MEM32(info + 12) = (uint32_t)p.levels;
        MEM32(info + 16) = (uint32_t)p.fmt, MEM32(info + 20) = D3DRTYPE_TEXTURE, MEM32(info + 24) = data[0] == 'B' ? 0 : 4;
    }
    int tw = w == 0 || w >= 0xFFFFFFFDu ? p.w : (int)w, th = h == 0 || h >= 0xFFFFFFFDu ? p.h : (int)h;
    int tl = levels == 0xFFFFFFFDu ? p.levels : levels == D3DX_DEFAULT ? 0 : (int)levels;
    if (!fmt || fmt == D3DX_DEFAULT) fmt = (uint32_t)p.fmt;
    color_key(p.bgra, (size_t)p.w * (size_t)p.h, key);
    uint32_t tex = d3d9_texture_new(tw, th, tl, (int)usage, (int)fmt, (int)pool);
    d3d9_texture *t = d3d9_texture_of(tex);
    if (!t) { free(p.bgra); return D3DERR_INVALIDCALL; }
    uint8_t *px = p.bgra;
    int cw = p.w, ch = p.h;
    if (cw != tw || ch != th) { uint8_t *s = scaled(px, cw, ch, tw, th); free(px); px = s, cw = tw, ch = th; }
    for (int i = 0; px && i < t->levels; i++) {
        d3d9_surface *s = t->level[i];
        if (i) { uint8_t *half = halved(px, cw, ch, s->w, s->h); free(px); px = half, cw = s->w, ch = s->h; }
        if (px) d3d9_surface_write(s, 0, 0, s->w, s->h, px);
    }
    free(px);
    MEM32(out) = tex;
    return D3D_OK;
}
/* (device, data, size, width, height, levels, usage, format, pool, filter, mip filter, key, &info, palette, &texture) */
static void x_CreateTextureFromFileInMemoryEx(void) {
    if (!A32(1)) RET(D3DERR_INVALIDCALL, 15);
    RET(texture_from(G2H(A32(1)), A32(2), A32(3), A32(4), A32(5), A32(6), A32(7), A32(8), A32(11), A32(12), A32(14)), 15);
}
/* (device, file, width, height, levels, usage, format, pool, filter, mip filter, key, &info, palette, &texture) */
static void x_CreateTextureFromFileExA(void) {
    char path[1024];
    if (A32(13)) MEM32(A32(13)) = 0;
    if (!A32(1) || !hle_host_path(G2H(A32(1)), path, sizeof path)) RET(D3DXFERR_FILENOTFOUND, 14);
    FILE *f = fopen(path, "rb");
    if (!f) RET(D3DXFERR_FILENOTFOUND, 14);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = n > 0 ? malloc((size_t)n) : NULL;
    uint32_t hr = D3DXERR_INVALIDDATA;
    if (data && fread(data, 1, (size_t)n, f) == (size_t)n)
        hr = texture_from(data, (size_t)n, A32(2), A32(3), A32(4), A32(5), A32(6), A32(7), A32(10), A32(11), A32(13));
    free(data);
    fclose(f);
    RET(hr, 14);
}
/* (to, its palette, its rectangle, from, its palette, its rectangle, filter, key) */
static void x_LoadSurfaceFromSurface(void) {
    d3d9_surface *to = d3d9_surface_of(A32(0)), *from = d3d9_surface_of(A32(3));
    if (!to || !from) RET(D3DERR_INVALIDCALL, 8);
    int tr[4] = { 0, 0, to->w, to->h }, fr[4] = { 0, 0, from->w, from->h };
    if (A32(2)) for (int i = 0; i < 4; i++) tr[i] = (int)MEM32(A32(2) + 4u * (uint32_t)i);
    if (A32(5)) for (int i = 0; i < 4; i++) fr[i] = (int)MEM32(A32(5) + 4u * (uint32_t)i);
    int fw = fr[2] - fr[0], fh = fr[3] - fr[1], tw = tr[2] - tr[0], th = tr[3] - tr[1];
    if (fw <= 0 || fh <= 0 || tw <= 0 || th <= 0 || fr[0] < 0 || fr[1] < 0 || fr[2] > from->w || fr[3] > from->h ||
        tr[0] < 0 || tr[1] < 0 || tr[2] > to->w || tr[3] > to->h)
        RET(D3DERR_INVALIDCALL, 8);
    uint8_t *px = d3d9_surface_read(from, fr[0], fr[1], fw, fh);
    if (px && (fw != tw || fh != th)) { uint8_t *s = scaled(px, fw, fh, tw, th); free(px); px = s; }
    if (!px) RET(0x8007000Eu, 8);
    color_key(px, (size_t)tw * (size_t)th, A32(7));
    d3d9_surface_write(to, tr[0], tr[1], tw, th, px);
    free(px);
    RET(D3D_OK, 8);
}

/* ---- meshes: not here ----------------------------------------------------------------------- */
static void x_LoadMeshFromXA(void) {           /* (file, options, device, &adjacency, &materials, &effects, &how many, &mesh) */
    static int said;
    if (!said++) fprintf(stderr, "[d3dx] not done: meshes from X files (first asked for: %s)\n", A32(0) ? ASTR(0) : "?");
    if (A32(7)) MEM32(A32(7)) = 0;
    RET(D3DXFERR_FILENOTFOUND, 8);
}
static void x_LoadSkinMeshFromXof(void) { once("skinned meshes from X files"); if (A32(8)) MEM32(A32(8)) = 0; RET(E_FAIL_, 9); }
static void x_CreateMeshFVF(void) { once("meshes"); if (A32(5)) MEM32(A32(5)) = 0; RET(E_FAIL_, 6); }
static void x_ComputeNormals(void) { once("meshes"); RET(E_FAIL_, 2); }

const win32hle_shim win32hle_d3dx9[] = {
    { "D3DXMatrixMultiply", x_MatrixMultiply }, { "D3DXMatrixTranspose", x_MatrixTranspose },
    { "D3DXMatrixInverse", x_MatrixInverse }, { "D3DXMatrixLookAtLH", x_MatrixLookAtLH },
    { "D3DXMatrixPerspectiveFovLH", x_MatrixPerspectiveFovLH }, { "D3DXMatrixRotationAxis", x_MatrixRotationAxis },
    { "D3DXMatrixRotationQuaternion", x_MatrixRotationQuaternion }, { "D3DXMatrixRotationX", x_MatrixRotationX },
    { "D3DXMatrixRotationY", x_MatrixRotationY }, { "D3DXMatrixRotationZ", x_MatrixRotationZ },
    { "D3DXMatrixRotationYawPitchRoll", x_MatrixRotationYawPitchRoll }, { "D3DXMatrixScaling", x_MatrixScaling },
    { "D3DXMatrixTranslation", x_MatrixTranslation }, { "D3DXQuaternionRotationMatrix", x_QuaternionRotationMatrix },
    { "D3DXQuaternionSlerp", x_QuaternionSlerp }, { "D3DXQuaternionToAxisAngle", x_QuaternionToAxisAngle },
    { "D3DXVec3Hermite", x_Vec3Hermite }, { "D3DXVec3Normalize", x_Vec3Normalize }, { "D3DXVec3Project", x_Vec3Project },
    { "D3DXVec3Transform", x_Vec3Transform }, { "D3DXVec3TransformCoord", x_Vec3TransformCoord },
    { "D3DXVec3Unproject", x_Vec3Unproject }, { "D3DXGetFVFVertexSize", x_GetFVFVertexSize },
    { "D3DXComputeBoundingBox", x_ComputeBoundingBox }, { "D3DXAssembleShader", x_AssembleShader },
    { "D3DXDisassembleShader", x_DisassembleShader },
    { "D3DXCreateTextureFromFileInMemoryEx", x_CreateTextureFromFileInMemoryEx },
    { "D3DXCreateTextureFromFileExA", x_CreateTextureFromFileExA },
    { "D3DXLoadSurfaceFromSurface", x_LoadSurfaceFromSurface }, { "D3DXLoadMeshFromXA", x_LoadMeshFromXA },
    { "D3DXLoadSkinMeshFromXof", x_LoadSkinMeshFromXof }, { "D3DXCreateMeshFVF", x_CreateMeshFVF },
    { "D3DXComputeNormals", x_ComputeNormals },
    { NULL, NULL }
};
#endif /* HLE_WITH_D3D9 */
