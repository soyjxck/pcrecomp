/*
 * winmm.c - win32hle WINMM: multimedia timers, joystick, waveOut audio, MCI.
 * On libc for timing; audio is "accept and retire" (a silent device that
 * completes every buffer immediately, so a game's double-buffered mixer thread
 * keeps cycling without a real sound card); no joystick; MCI stubbed. Real SDL
 * audio can replace the waveOut path later without touching callers.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include "win32hle.h"

#define MMSYSERR_NOERROR  0u
#define MMSYSERR_ERROR    1u
#define JOYERR_UNPLUGGED  167u
#define WHDR_DONE         0x00000001u
#define WHDR_INQUEUE      0x00000010u

/* --- multimedia timer --- */
static void m_timeGetTime(void) {
    hle_poll(HLE_POLL_TIME);
    RET(hle_ticks_ms(), 0);
}
static void m_timeBeginPeriod(void) { RET(MMSYSERR_NOERROR, 1); }
static void m_timeGetDevCaps(void) {           /* (&TIMECAPS, size) */
    if (A32(0) && A32(1) >= 8) MEM32(A32(0)) = 1, MEM32(A32(0) + 4) = 1000000;
    RET(MMSYSERR_NOERROR, 2);
}

/* --- multimedia timers ---
 * Each timer is a host thread that calls the guest's callback through
 * hle_call_guest, so the callback runs once it gets the machine, as on
 * Windows it runs on winmm's thread. timeKillEvent returns only once a
 * callback already under way has finished (giving the machine up while it
 * waits): a game frees what its callback uses right after the kill
 * (Tiberian Sun's movie audio, its bring-up log 8). */
#define MAX_MMTIMERS 32
static struct { volatile int used, killed, inflight; uint32_t id, delay, proc, user, flags; pthread_t thread; } g_mmt[MAX_MMTIMERS];
static uint32_t g_mmt_next = 1;
static void *mmtimer(void *arg) {
    int i = (int)(intptr_t)arg;
    hle_thread_aside();
    uint32_t due = hle_ticks_ms() + g_mmt[i].delay;
    for (;;) {
        int32_t wait = (int32_t)(due - hle_ticks_ms());
        if (wait > 0) usleep((useconds_t)wait * 1000u);
        if (g_mmt[i].killed) break;
        __sync_fetch_and_add(&g_mmt[i].inflight, 1);
        if (!g_mmt[i].killed) {
            if (g_mmt[i].flags & 0x30u) {                /* TIME_CALLBACK_EVENT_SET / _PULSE: proc is an event */
                uint32_t h = g_mmt[i].proc;
                mach_enter();
                hle_set_event(h);
                mach_leave();
            } else {
                uint32_t a[5] = { g_mmt[i].id, 0, g_mmt[i].user, 0, 0 };
                hle_call_guest(g_mmt[i].proc, 5, a);
            }
        }
        __sync_fetch_and_sub(&g_mmt[i].inflight, 1);
        if (!(g_mmt[i].flags & 1u)) break;               /* TIME_ONESHOT */
        due += g_mmt[i].delay ? g_mmt[i].delay : 1;
        if ((int32_t)(hle_ticks_ms() - due) > 1000) due = hle_ticks_ms();   /* fell far behind: no catch-up storm */
    }
    g_mmt[i].used = 0;
    return NULL;
}
static void m_timeSetEvent(void) {             /* (delay, resolution, proc, user, flags) */
    for (int i = 0; i < MAX_MMTIMERS; i++)
        if (!g_mmt[i].used) {
            g_mmt[i].used = 1, g_mmt[i].killed = 0, g_mmt[i].inflight = 0;
            g_mmt[i].id = g_mmt_next++, g_mmt[i].delay = A32(0), g_mmt[i].proc = A32(2), g_mmt[i].user = A32(3), g_mmt[i].flags = A32(4);
            if (pthread_create(&g_mmt[i].thread, NULL, mmtimer, (void *)(intptr_t)i) != 0) { g_mmt[i].used = 0; RET(0, 5); }
            pthread_detach(g_mmt[i].thread);
            RET(g_mmt[i].id, 5);
        }
    RET(0, 5);
}
static void m_timeKillEvent(void) {
    for (int i = 0; i < MAX_MMTIMERS; i++)
        if (g_mmt[i].used && g_mmt[i].id == A32(0) && !g_mmt[i].killed) {
            g_mmt[i].killed = 1;
            if (!pthread_equal(g_mmt[i].thread, pthread_self()))
                while (g_mmt[i].inflight) { hle_block_begin(); usleep(200); hle_block_end(); }
            RET(MMSYSERR_NOERROR, 1);
        }
    RET(97u /* MMSYSERR_INVALPARAM */, 1);
}
static void m_timeEndPeriod(void)   { RET(MMSYSERR_NOERROR, 1); }

/* --- joystick: none present, so the game takes its keyboard/mouse path --- */
static void m_joyGetNumDevs(void)   { RET(0, 0); }
static void m_joyGetDevCapsA(void)  { RET(JOYERR_UNPLUGGED, 3); }
static void m_joyGetPos(void)       { RET(JOYERR_UNPLUGGED, 2); }
static void m_joyGetPosEx(void)     { RET(JOYERR_UNPLUGGED, 2); }

/* --- waveOut: a silent device that retires every buffer at once ---
 * waveOutOpen succeeds with a fake handle; waveOutWrite marks the WAVEHDR done
 * and not-in-queue, so a mixer that waits for buffers to drain keeps going. */
static void m_waveOutOpen(void) {              /* (&hwo, devID, &fmt, cb, inst, flags) */
    uint32_t ph = A32(0);
    if (ph) MEM32(ph) = 0x5A5E0001u;           /* a nonzero HWAVEOUT */
    RET(MMSYSERR_NOERROR, 6);
}
static void m_waveOutWrite(void) {             /* (hwo, lpWaveHdr, cbwh) */
    uint32_t wh = A32(1);
    if (wh) { MEM32(wh + 16) = (MEM32(wh + 16) | WHDR_DONE) & ~WHDR_INQUEUE; }
    RET(MMSYSERR_NOERROR, 3);
}
static void m_waveOutPrepareHeader(void)   { uint32_t wh=A32(1); if(wh) MEM32(wh+16)|=0x00000002u/*WHDR_PREPARED*/; RET(MMSYSERR_NOERROR, 3); }
static void m_waveOutUnprepareHeader(void) { uint32_t wh=A32(1); if(wh) MEM32(wh+16)&=~0x00000002u; RET(MMSYSERR_NOERROR, 3); }
static void m_waveOutReset(void)       { RET(MMSYSERR_NOERROR, 1); }
static void m_waveOutClose(void)       { RET(MMSYSERR_NOERROR, 1); }
static void m_waveOutGetNumDevs(void)  { RET(1, 0); }          /* one (silent) device */
static void m_waveOutGetDevCapsA(void) { RET(MMSYSERR_NOERROR, 3); }

/* --- MCI (CD audio / command strings): not available --- */
static void m_mciSendStringA(void)     { RET(MMSYSERR_ERROR, 4); }
static void m_mciGetErrorStringA(void) { RET(0, 3); }          /* FALSE: no string */

/* --- mmio: a RIFF file read chunk by chunk (a wave file's format, then its
 * samples). Files on disk only; a file in memory is refused. --- */
#define MAX_MMIO 16
#define MMIO_BASE 0x00770000u
#define MMIOERR_CHUNKNOTFOUND 265u
static FILE *g_mmio[MAX_MMIO];
static FILE *mmio_of(uint32_t h) { return h >= MMIO_BASE && h < MMIO_BASE + MAX_MMIO ? g_mmio[h - MMIO_BASE] : NULL; }
static void m_mmioOpenW(void) {                /* (wide name, &MMIOINFO, flags) */
    char guest[600], host[1024];
    uint32_t name = A32(0), n = 0;
    int seat = -1;
    for (int i = 0; i < MAX_MMIO; i++) if (!g_mmio[i]) { seat = i; break; }
    if (!name || seat < 0) RET(0, 3);
    for (; n < sizeof guest - 1 && MEM16(name + 2 * n); n++) guest[n] = (char)MEM16(name + 2 * n);
    guest[n] = 0;
    if (!hle_host_path(guest, host, sizeof host) || !(g_mmio[seat] = fopen(host, "rb"))) RET(0, 3);
    RET(MMIO_BASE + (uint32_t)seat, 3);
}
static void m_mmioClose(void) {                /* (file, flags) */
    FILE *f = mmio_of(A32(0));
    if (f) fclose(f), g_mmio[A32(0) - MMIO_BASE] = NULL;
    RET(0, 2);
}
static void m_mmioRead(void) {                 /* (file, to, bytes): how many were read, -1 if none can be */
    FILE *f = mmio_of(A32(0));
    if (!f || !A32(1)) RET(0xFFFFFFFFu, 3);
    RET((uint32_t)fread(G2H(A32(1)), 1, A32(2), f), 3);
}
static void m_mmioSeek(void) {                 /* (file, offset, from where): the new place */
    FILE *f = mmio_of(A32(0));
    if (!f || fseek(f, (long)(int32_t)A32(1), A32(2) == 1 ? SEEK_CUR : A32(2) == 2 ? SEEK_END : SEEK_SET) != 0) RET(0xFFFFFFFFu, 3);
    RET((uint32_t)ftell(f), 3);
}
/* MMCKINFO: the chunk's name, its size, its type (RIFF and LIST chunks have
 * one), where its data starts, flags. Descending finds the next chunk, or
 * the next of the name or type asked for, inside the parent if one is given,
 * and leaves the file at its data. */
static void m_mmioDescend(void) {              /* (file, &chunk, &parent, flags) */
    FILE *f = mmio_of(A32(0));
    uint32_t ck = A32(1), parent = A32(2), flags = A32(3);
    if (!f || !ck) RET(MMIOERR_CHUNKNOTFOUND, 4);
    uint32_t want = MEM32(ck), want_type = MEM32(ck + 8);
    long end = parent ? (long)(MEM32(parent + 12) + MEM32(parent + 4)) : 0;
    for (;;) {
        long at = ftell(f);
        uint32_t head[2], type = 0;
        if ((parent && at + 8 > end) || fread(head, 4, 2, f) != 2) RET(MMIOERR_CHUNKNOTFOUND, 4);
        int group = head[0] == 0x46464952u || head[0] == 0x5453494Cu;        /* RIFF, LIST */
        if (group && fread(&type, 4, 1, f) != 1) RET(MMIOERR_CHUNKNOTFOUND, 4);
        if (!(flags & 0x70u) || ((flags & 0x10u) && head[0] == want) || ((flags & 0x20u) && head[0] == 0x46464952u && type == want_type) ||
            ((flags & 0x40u) && head[0] == 0x5453494Cu && type == want_type)) {
            MEM32(ck) = head[0], MEM32(ck + 4) = head[1], MEM32(ck + 8) = type, MEM32(ck + 12) = (uint32_t)at + 8, MEM32(ck + 16) = 0;
            RET(0, 4);
        }
        fseek(f, at + 8 + (long)head[1] + (long)(head[1] & 1), SEEK_SET);   /* chunks start on even offsets */
    }
}

const win32hle_shim win32hle_winmm[] = {
    { "mmioOpenW", m_mmioOpenW }, { "mmioClose", m_mmioClose }, { "mmioRead", m_mmioRead }, { "mmioSeek", m_mmioSeek },
    { "mmioDescend", m_mmioDescend },
    { "timeGetTime",            m_timeGetTime },
    { "timeBeginPeriod",        m_timeBeginPeriod },
    { "timeEndPeriod",          m_timeEndPeriod },
    { "timeGetDevCaps",         m_timeGetDevCaps },
    { "timeSetEvent",           m_timeSetEvent },
    { "timeKillEvent",          m_timeKillEvent },
    { "joyGetNumDevs",          m_joyGetNumDevs },
    { "joyGetDevCapsA",         m_joyGetDevCapsA },
    { "joyGetPos",              m_joyGetPos },
    { "joyGetPosEx",            m_joyGetPosEx },
    { "waveOutOpen",            m_waveOutOpen },
    { "waveOutWrite",           m_waveOutWrite },
    { "waveOutPrepareHeader",   m_waveOutPrepareHeader },
    { "waveOutUnprepareHeader", m_waveOutUnprepareHeader },
    { "waveOutReset",           m_waveOutReset },
    { "waveOutClose",           m_waveOutClose },
    { "waveOutGetNumDevs",      m_waveOutGetNumDevs },
    { "waveOutGetDevCapsA",     m_waveOutGetDevCapsA },
    { "mciSendStringA",         m_mciSendStringA },
    { "mciGetErrorStringA",     m_mciGetErrorStringA },
    { 0, 0 }
};
