/*
 * vorbisfile.c — the part of libvorbisfile (vorbisfile.dll) a game streams
 * its music through.
 *
 * A game loads vorbisfile.dll by name and asks it for functions with
 * GetProcAddress. The DLL's own code is not run here: the names are answered
 * from this table instead (module.c looks a mapped DLL's exports up among the
 * shims). The functions are cdecl, as the DLL's are: the caller takes its
 * arguments off the stack.
 *
 * What the game calls an OggVorbis_File is its own block of memory, used here
 * only as a name for the stream. A stream is read whole through the game's
 * read function when it is opened (a music track is a few megabytes), and
 * decoded from memory after that.
 *
 * The decoder is the host's libvorbisfile with HLE_WITH_LIBVORBIS, and
 * otherwise stb_vorbis (third_party/stb_vorbis.c), which needs no library but
 * cannot read a stream that uses floor 0, as old encoders' do: such a stream
 * is refused, and the log says so.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win32hle.h"

#ifdef HLE_WITH_LIBVORBIS
#include <vorbis/vorbisfile.h>
#else
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-value"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#endif
#include "third_party/stb_vorbis.c"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#endif

#define GUEST_OV_EREAD      (-128)
#define GUEST_OV_EINVAL     (-131)
#define GUEST_OV_ENOTVORBIS (-132)

typedef struct {
    uint32_t vf;                /* the game's OggVorbis_File: 0 when the seat is free */
    uint8_t *file;
    size_t size, at;            /* the stream's bytes, and where the decoder is reading */
    int channels, rate;
    uint32_t datasource, close_fn;
    uint32_t info, comment;     /* what ov_info and ov_comment hand out, in guest memory */
#ifdef HLE_WITH_LIBVORBIS
    OggVorbis_File ov;
#else
    stb_vorbis *v;
#endif
} stream;
#define MAX_STREAMS 16
static stream g_stream[MAX_STREAMS];

/* ---- the decoder, either one --------------------------------------------------- */
#ifdef HLE_WITH_LIBVORBIS
static size_t mem_read(void *to, size_t size, size_t count, void *src) {
    stream *s = src;
    size_t n = size * count;
    if (n > s->size - s->at) n = s->size - s->at;
    memcpy(to, s->file + s->at, n), s->at += n;
    return size ? n / size : 0;
}
static int mem_seek(void *src, ogg_int64_t off, int whence) {
    stream *s = src;
    ogg_int64_t to = whence == SEEK_SET ? off : whence == SEEK_CUR ? (ogg_int64_t)s->at + off : (ogg_int64_t)s->size + off;
    if (to < 0 || to > (ogg_int64_t)s->size) return -1;
    s->at = (size_t)to;
    return 0;
}
static long mem_tell(void *src) { return (long)((stream *)src)->at; }
static int dec_open(stream *s) {
    static const ov_callbacks cb = { mem_read, mem_seek, NULL, mem_tell };
    int rc = ov_open_callbacks(s, &s->ov, NULL, 0, cb);
    if (rc != 0) { fprintf(stderr, "[vorbisfile] a stream of %u bytes would not open (%d)\n", (unsigned)s->size, rc); return 0; }
    vorbis_info *in = ov_info(&s->ov, -1);
    s->channels = in->channels, s->rate = (int)in->rate;
    return 1;
}
static void dec_close(stream *s) { ov_clear(&s->ov); }
static uint32_t dec_total(stream *s) { ogg_int64_t n = ov_pcm_total(&s->ov, -1); return n < 0 ? 0 : (uint32_t)n; }
static void dec_seek(stream *s, uint32_t sample) { ov_pcm_seek(&s->ov, sample); }
static long dec_read(stream *s, void *to, int bytes) {         /* signed 16-bit, this machine's byte order */
    int link = 0;
    long n = ov_read(&s->ov, to, bytes, 0, 2, 1, &link);
    return n < 0 ? 0 : n;
}
static int dec_comments(stream *s, const char **vendor) { vorbis_comment *c = ov_comment(&s->ov, -1); *vendor = c && c->vendor ? c->vendor : ""; return c ? c->comments : 0; }
static const char *dec_comment(stream *s, int i) { return ov_comment(&s->ov, -1)->user_comments[i]; }
#else
static int dec_open(stream *s) {
    int error = 0;
    s->v = stb_vorbis_open_memory(s->file, (int)s->size, &error, NULL);
    if (!s->v) {
        fprintf(stderr, "[vorbisfile] a stream of %u bytes would not open: %s\n", (unsigned)s->size,
                error == VORBIS_feature_not_supported ? "it uses floor 0, which needs the host built with libvorbis (HLE_WITH_LIBVORBIS)" : "not Vorbis, or damaged");
        return 0;
    }
    stb_vorbis_info in = stb_vorbis_get_info(s->v);
    s->channels = in.channels, s->rate = (int)in.sample_rate;
    return 1;
}
static void dec_close(stream *s) { stb_vorbis_close(s->v); }
static uint32_t dec_total(stream *s) { return stb_vorbis_stream_length_in_samples(s->v); }
static void dec_seek(stream *s, uint32_t sample) { stb_vorbis_seek(s->v, sample); }
static long dec_read(stream *s, void *to, int bytes) {
    int shorts = bytes / 2;
    shorts -= shorts % s->channels;
    return shorts > 0 ? (long)stb_vorbis_get_samples_short_interleaved(s->v, s->channels, to, shorts) * s->channels * 2 : 0;
}
static int dec_comments(stream *s, const char **vendor) { stb_vorbis_comment c = stb_vorbis_get_comment(s->v); *vendor = c.vendor ? c.vendor : ""; return c.comment_list_length; }
static const char *dec_comment(stream *s, int i) { return stb_vorbis_get_comment(s->v).comment_list[i]; }
#endif

/* ---- what the game calls ---------------------------------------------------------- */
static stream *find(uint32_t vf) {
    for (int i = 0; vf && i < MAX_STREAMS; i++) if (g_stream[i].vf == vf) return &g_stream[i];
    return NULL;
}
static void drop(stream *s, int opened) {
    if (opened) dec_close(s);
    free(s->file);
    if (s->info) hle_guest_free(s->info);
    if (s->comment) hle_guest_free(s->comment);
    memset(s, 0, sizeof *s);
}

/* (datasource, &file, bytes already read, how many, then the four callbacks: read, seek, close, tell) */
static void ov_open_callbacks_(void) {
    uint32_t datasource = A32(0), vf = A32(1), initial = A32(2), ibytes = A32(3), read_fn = A32(4), close_fn = A32(6);
    stream *s = find(vf);
    if (s) drop(s, 1);
    for (int i = 0; !s && i < MAX_STREAMS; i++) if (!g_stream[i].vf) s = &g_stream[i];
    if (!vf || !read_fn || !s) RET(GUEST_OV_EINVAL, 0);

    size_t size = 0, cap = 1u << 20;
    uint8_t *file = malloc(cap);
    uint32_t chunk = hle_guest_alloc(65536);
    if (!file || !chunk) { free(file); if (chunk) hle_guest_free(chunk); RET(GUEST_OV_EREAD, 0); }
    if (initial && ibytes && ibytes <= cap) memcpy(file, G2H(initial), ibytes), size = ibytes;
    for (;;) {
        uint32_t args[4] = { chunk, 1, 65536, datasource };      /* size_t read(void *to, size_t size, size_t count, void *datasource) */
        uint32_t got = hle_call_guest(read_fn, 4, args);
        if (!got || got > 65536) break;
        if (size + got > cap) {
            uint8_t *more = realloc(file, cap *= 2);
            if (!more) { size = 0; break; }
            file = more;
        }
        memcpy(file + size, G2H(chunk), got), size += got;
    }
    hle_guest_free(chunk);

    s->file = file, s->size = size, s->at = 0;
    if (!size || !dec_open(s)) { drop(s, 0); RET(GUEST_OV_ENOTVORBIS, 0); }
    s->vf = vf, s->datasource = datasource, s->close_fn = close_fn;
    /* vorbis_info: version, channels, rate, three bitrates, a window, private data */
    s->info = hle_guest_alloc(32);
    if (s->info) MEM32(s->info + 4) = (uint32_t)s->channels, MEM32(s->info + 8) = (uint32_t)s->rate;
    /* vorbis_comment: the strings, their lengths, how many, the vendor */
    const char *vendor = "";
    int count = dec_comments(s, &vendor);
    uint32_t n = count > 0 ? (uint32_t)count : 0;
    size_t text = strlen(vendor) + 1;
    for (uint32_t i = 0; i < n; i++) text += strlen(dec_comment(s, (int)i)) + 1;
    s->comment = hle_guest_alloc((uint32_t)(16 + 8 * (n + 1) + text));
    if (s->comment) {
        uint32_t list = s->comment + 16, lens = list + 4 * (n + 1), at = lens + 4 * (n + 1);
        MEM32(s->comment) = list, MEM32(s->comment + 4) = lens, MEM32(s->comment + 8) = n, MEM32(s->comment + 12) = at;
        strcpy(G2H(at), vendor), at += (uint32_t)strlen(vendor) + 1;
        for (uint32_t i = 0; i < n; i++) {
            const char *c = dec_comment(s, (int)i);
            uint32_t len = (uint32_t)strlen(c);
            MEM32(list + 4 * i) = at, MEM32(lens + 4 * i) = len;
            memcpy(G2H(at), c, len + 1), at += len + 1;
        }
    }
    RET(0, 0);
}
static void ov_clear_(void) {                  /* (&file): the stream's own close function is called, as libvorbisfile calls it */
    stream *s = find(A32(0));
    if (s) {
        uint32_t close_fn = s->close_fn, datasource = s->datasource;
        drop(s, 1);
        if (close_fn) hle_call_guest(close_fn, 1, &datasource);
    }
    RET(0, 0);
}
static void ov_pcm_total_(void) {              /* (&file, link): samples of one channel, as 64 bits */
    stream *s = find(A32(0));
    g_edx = s ? 0 : 0xFFFFFFFFu;
    RET(s ? dec_total(s) : (uint32_t)GUEST_OV_EINVAL, 0);
}
static void ov_pcm_seek_(void) {               /* (&file, the sample as 64 bits) */
    stream *s = find(A32(0));
    if (!s) RET(GUEST_OV_EINVAL, 0);
    dec_seek(s, A32(1));
    RET(0, 0);
}
static void ov_info_(void) { stream *s = find(A32(0)); RET(s ? s->info : 0, 0); }
static void ov_comment_(void) { stream *s = find(A32(0)); RET(s ? s->comment : 0, 0); }
/* (&file, to, bytes, big endian, bytes a sample, signed, &which stream): bytes
 * written, 0 at the end. Signed 16-bit in this machine's order is all that is
 * asked for. */
static void ov_read_(void) {
    stream *s = find(A32(0));
    if (!s || !A32(1)) RET(GUEST_OV_EINVAL, 0);
    if (A32(4) != 2 || A32(3)) { static int said; if (!said++) fprintf(stderr, "[vorbisfile] not done: samples other than 16-bit little-endian\n"); }
    if (A32(6)) MEM32(A32(6)) = 0;
    RET((uint32_t)dec_read(s, G2H(A32(1)), (int)A32(2)), 0);
}

const win32hle_shim win32hle_vorbisfile[] = {
    { "ov_open_callbacks", ov_open_callbacks_ }, { "ov_clear", ov_clear_ }, { "ov_pcm_total", ov_pcm_total_ },
    { "ov_pcm_seek", ov_pcm_seek_ }, { "ov_info", ov_info_ }, { "ov_comment", ov_comment_ }, { "ov_read", ov_read_ },
    { NULL, NULL }
};
