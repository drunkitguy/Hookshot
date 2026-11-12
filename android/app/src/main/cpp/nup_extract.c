/* (android) The start screen's one-time unpack: the romfs of a .nup's newest title into <out>/romfs, as nx2/nup.py's
   extract does on a PC (the layout and the "is this file stored compressed" rules are that file's; read it first).
   Files already there at their full size are kept, so a stopped unpack carries on where it was.
   tests: cc -DNUP_TEST -Ithird_party/zstd/lib nup_extract.c <zstd's decoder> -o nup_test; nup_test file.nup outdir */
#define _FILE_OFFSET_BITS 64
#define ZSTD_STATIC_LINKING_ONLY
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "zstd.h"

static volatile int64_t g_done, g_total;
static volatile int g_cancel;

typedef struct {
    int fd;
    uint8_t *meta;
    uint64_t meta_size, nd, nf, nk, dirs_at, files_at, kids_at, strings_at, strings_size;
    uint32_t nt;
    char err[512];
} Nup;

typedef struct { uint32_t name, name_len; uint64_t size, off, stored; uint8_t codec; } File;

static uint32_t u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t u64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

static int fail(Nup *n, const char *fmt, const char *a, long long b) {
    if (!n->err[0]) snprintf(n->err, sizeof n->err, fmt, a, b);
    return -1;
}

static int read_at(Nup *n, void *buf, uint64_t size, uint64_t off) {
    uint8_t *p = buf;
    while (size) {
        const ssize_t r = pread(n->fd, p, size > (1u << 30) ? (1u << 30) : (size_t)size, (off_t)off);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return fail(n, "the .nup file ends early or cannot be read (%s at %lld MB): copy it again", r ? strerror(errno) : "short read", (long long)(off >> 20));
        p += r, off += (uint64_t)r, size -= (uint64_t)r;
    }
    return 0;
}

static int nup_open(Nup *n, int fd) {
    uint8_t h[0xb0];
    memset(n, 0, sizeof *n);
    n->fd = fd;
    if (read_at(n, h, sizeof h, 0)) return -1;
    if (memcmp(h, "NUP\x01", 4)) return fail(n, "this is not a .nup file%s", "", 0);
    if (memcmp(h + 0x80, "TOC\x01", 4)) return fail(n, "the .nup file has no table of contents%s", "", 0);
    struct stat st;
    const uint64_t stated = u64(h + 0x40);
    if (!fstat(fd, &st) && stated && (uint64_t)st.st_size < stated)
        return fail(n, "the .nup file is incomplete%s (%lld MB short): copy it again", "", (long long)((stated - (uint64_t)st.st_size) >> 20));
    n->nt = u32(h + 0x84);
    n->nd = u64(h + 0x88), n->nf = u64(h + 0x90);
    const uint64_t soff = u64(h + 0x98);
    n->strings_size = u64(h + 0xa0);
    n->dirs_at = 0xb0 + 0x40ull * n->nt;
    n->files_at = n->dirs_at + 24 * n->nd;
    n->kids_at = n->files_at + 80 * n->nf;
    n->strings_at = 0x80 + soff;
    n->meta_size = n->strings_at + n->strings_size;
    if (n->nt > 64 || n->kids_at > n->strings_at || n->meta_size > (1ull << 31)) return fail(n, "the .nup file's table of contents is damaged%s", "", 0);
    n->nk = (n->strings_at - n->kids_at) / 4;
    n->meta = malloc(n->meta_size);
    if (!n->meta) return fail(n, "not enough memory for the .nup's table of contents%s", "", 0);
    return read_at(n, n->meta, n->meta_size, 0);
}

static int file_at(Nup *n, uint64_t i, File *f) {
    if (i >= n->nf) return fail(n, "the .nup file's table of contents is damaged%s", "", 0);
    const uint8_t *p = n->meta + n->files_at + 80 * i;
    f->name = u32(p), f->name_len = u32(p + 4), f->size = u64(p + 8), f->off = u64(p + 16), f->stored = u64(p + 24), f->codec = p[64];
    return 0;
}

/* a node's name into out (one path part: no separators, not . or ..) */
static int name_of(Nup *n, uint32_t off, uint32_t len, char *out, size_t cap) {
    if ((uint64_t)off + len > n->strings_size || len + 1 > cap || !len) return fail(n, "the .nup file has a name that cannot be used%s", "", 0);
    memcpy(out, n->meta + n->strings_at + off, len);
    out[len] = 0;
    if (memchr(out, '/', len) || memchr(out, '\\', len) || strlen(out) != len || !strcmp(out, ".") || !strcmp(out, ".."))
        return fail(n, "the .nup file has a name that cannot be used: %s", out, 0);
    return 0;
}

/* the content size a zstd frame header states, -1 when it states none (nup.py frame_content_size) */
static int64_t frame_content_size(const uint8_t *head, size_t len) {
    static const int dict[4] = {0, 1, 2, 4}, fcs[4] = {0, 2, 4, 8};
    if (len < 18 || u32(head) != 0xFD2FB528u) return -1;
    const int fhd = head[4], single = (fhd >> 5) & 1;
    const int at = 5 + (single ? 0 : 1) + dict[fhd & 3];
    const int nb = (fhd >> 6) ? fcs[fhd >> 6] : single;
    if (!nb) return -1;
    uint64_t v = 0;
    memcpy(&v, head + at, (size_t)nb);
    return (int64_t)(nb == 2 ? v + 256 : v);
}

enum { IO = 1 << 20 };

/* Unpacks the frame at f->off (at most `limit` stored bytes) into out_fd (-1: only counts; sniff: only the first bytes kept).
   The unpacked size, or -1 on a write error. *ended: the frame was complete */
static int64_t unpack(Nup *n, const File *f, uint64_t limit, int out_fd, uint8_t *sniff, int *ended, uint8_t *in, uint8_t *out, ZSTD_DStream *z) {
    uint64_t off = f->off, left = limit;
    int64_t got = 0;
    *ended = 0;
    ZSTD_initDStream(z);
    while (left && !*ended) {
        const size_t want = left > IO ? IO : (size_t)left;
        const ssize_t r = pread(n->fd, in, want, (off_t)off);
        if (r <= 0) break;
        off += (uint64_t)r, left -= (uint64_t)r;
        ZSTD_inBuffer ib = {in, (size_t)r, 0};
        while (ib.pos < ib.size) {
            ZSTD_outBuffer ob = {out, IO, 0};
            const size_t rc = ZSTD_decompressStream(z, &ob, &ib);
            if (ZSTD_isError(rc)) return got;   /* (the end of the frame and the next file's bytes, or not a frame at all) */
            if (sniff && got < 4 && ob.pos) memcpy(sniff + got, out, (size_t)(4 - got) < ob.pos ? (size_t)(4 - got) : ob.pos);
            if (out_fd >= 0 && ob.pos) {
                size_t w = 0;
                while (w < ob.pos) {
                    const ssize_t k = write(out_fd, out + w, ob.pos - w);
                    if (k < 0 && errno == EINTR) continue;
                    if (k <= 0) return -1;
                    w += (size_t)k;
                }
                g_done += (int64_t)ob.pos;
            }
            got += (int64_t)ob.pos;
            if ((uint64_t)got > f->size + IO) return got;
            if (rc == 0) { *ended = 1; break; }
        }
    }
    return got;
}

/* nup.py is_packed: whether the package keeps this file as a zstd frame of it */
static int is_packed(Nup *n, const File *f, const char *name, uint8_t *in, uint8_t *out, ZSTD_DStream *z) {
    if (f->codec == 1) return 1;
    uint8_t head[18];
    if (f->size < 18 || pread(n->fd, head, 18, (off_t)f->off) != 18 || u32(head) != 0xFD2FB528u) return 0;
    const int64_t size = frame_content_size(head, 18);
    const size_t nl = strlen(name);
    int ended;
    if (nl > 3 && !strcmp(name + nl - 3, ".zs") && (size < 0 || (uint64_t)size == f->size)) {
        if ((uint64_t)size != f->size) return 0;
        uint8_t first[4] = {0};   /* (a .zs file is zstd in the game too: packed only if what it holds is zstd again) */
        const int64_t got = unpack(n, f, f->size > IO ? IO : f->size, -1, first, &ended, in, out, z);
        return got >= 4 && u32(first) == 0xFD2FB528u;
    }
    if (size >= 0) return (uint64_t)size == f->size;
    return (uint64_t)unpack(n, f, f->size, -1, NULL, &ended, in, out, z) == f->size;
}

static int extract_file(Nup *n, const File *f, const char *name, const char *path, uint8_t *in, uint8_t *out, ZSTD_DStream *z) {
    struct stat st;
    if (!stat(path, &st) && S_ISREG(st.st_mode) && (uint64_t)st.st_size == f->size) {   /* (kept from a stopped unpack) */
        g_done += (int64_t)f->size;
        return 0;
    }
    char part[4200];
    snprintf(part, sizeof part, "%s.part", path);
    const int fd = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return fail(n, "cannot write %s (%lld): is the storage full?", path, errno);
    int ok = 1;
    if (is_packed(n, f, name, in, out, z)) {
        int ended;
        const int64_t got = unpack(n, f, f->codec == 1 ? f->stored : f->size + (1 << 16), fd, NULL, &ended, in, out, z);
        if (got < 0) ok = 0, fail(n, "cannot write %s (%lld): is the storage full?", path, errno);
        else if ((uint64_t)got != f->size) ok = 0, fail(n, "a compressed file in the .nup did not unpack to its size: %s (%lld bytes)", name, got);
    } else {
        uint64_t off = f->off, left = f->size;
        while (left && ok) {
            const size_t want = left > IO ? IO : (size_t)left;
            if (read_at(n, in, want, off)) { ok = 0; break; }
            size_t w = 0;
            while (w < want) {
                const ssize_t k = write(fd, in + w, want - w);
                if (k < 0 && errno == EINTR) continue;
                if (k <= 0) { ok = 0, fail(n, "cannot write %s (%lld): is the storage full?", path, errno); break; }
                w += (size_t)k;
            }
            off += want, left -= want, g_done += (int64_t)want;
        }
    }
    if (close(fd) && ok) ok = 0, fail(n, "cannot write %s (%lld): is the storage full?", path, errno);
    if (ok && rename(part, path)) ok = 0, fail(n, "cannot finish %s (%lld)", path, errno);
    if (!ok) unlink(part);
    return ok ? 0 : -1;
}

/* dir node d under path (which exists): totals when count_only, else the files */
static int walk(Nup *n, uint64_t d, char *path, size_t len, int depth, int count_only, uint8_t *in, uint8_t *out, ZSTD_DStream *z) {
    if (d >= n->nd || depth > 64) return fail(n, "the .nup file's table of contents is damaged%s", "", 0);
    const uint8_t *p = n->meta + n->dirs_at + 24 * d;
    const uint64_t first = u32(p + 8), count = u32(p + 12);
    if (first + count > n->nk) return fail(n, "the .nup file's table of contents is damaged%s", "", 0);
    for (uint64_t i = 0; i < count; i++) {
        if (g_cancel) return fail(n, "stopped%s", "", 0);
        const uint64_t k = u32(n->meta + n->kids_at + 4 * (first + i));
        char name[1024];
        if (k < n->nd) {
            const uint8_t *c = n->meta + n->dirs_at + 24 * k;
            if (name_of(n, u32(c), u32(c + 4), name, sizeof name)) return -1;
            const size_t add = strlen(name);
            if (len + add + 2 > 4000) return fail(n, "a path in the .nup is too long: %s", name, 0);
            path[len] = '/', memcpy(path + len + 1, name, add + 1);
            if (!count_only && mkdir(path, 0755) && errno != EEXIST) return fail(n, "cannot make the folder %s (%lld)", path, errno);
            if (walk(n, k, path, len + 1 + add, depth + 1, count_only, in, out, z)) return -1;
            path[len] = 0;
        } else {
            File f;
            if (file_at(n, k - n->nd, &f) || name_of(n, f.name, f.name_len, name, sizeof name)) return -1;
            if (count_only) { g_total += (int64_t)f.size; continue; }
            const size_t add = strlen(name);
            if (len + add + 2 > 4000) return fail(n, "a path in the .nup is too long: %s", name, 0);
            path[len] = '/', memcpy(path + len + 1, name, add + 1);
            const int r = extract_file(n, &f, name, path, in, out, z);
            path[len] = 0;
            if (r) return -1;
        }
    }
    return 0;
}

/* 0, or -1 with the reason in err. want_version: the newest title's must be this (0: any) */
int nup_extract_romfs(int fd, const char *out_dir, uint32_t want_version, char *err, size_t errcap) {
    Nup n;
    int rc = -1;
    uint8_t *in = malloc(IO), *out = malloc(IO);
    ZSTD_DStream *z = ZSTD_createDStream();
    g_done = g_total = 0, g_cancel = 0;
    if (!in || !out || !z) { snprintf(err, errcap, "not enough memory"); goto done; }
    if (nup_open(&n, fd)) goto failed;
    uint32_t best = 0, romfs = 0;
    int found = 0;
    for (uint32_t i = 0; i < n.nt; i++) {   /* (the newest: the update over the base, nup.py pick_title) */
        const uint8_t *t = n.meta + 0xb0 + 0x40 * i;
        if (!found || u32(t + 20) > best) best = u32(t + 20), romfs = u32(t + 16), found = 1;
    }
    if (!found) { fail(&n, "the .nup file holds no game%s", "", 0); goto failed; }
    if (want_version && best != want_version) {
        fail(&n, "this .nup's newest title is not the version the app was built from%s (it is %lld; 131072, the game with its update, is needed)", "", best);
        goto failed;
    }
    char path[4200];
    snprintf(path, sizeof path, "%s/romfs", out_dir);
    if (mkdir(path, 0755) && errno != EEXIST) { fail(&n, "cannot make the folder %s (%lld)", path, errno); goto failed; }
    if (walk(&n, romfs, path, strlen(path), 0, 1, in, out, z)) goto failed;
    if (walk(&n, romfs, path, strlen(path), 0, 0, in, out, z)) goto failed;
    rc = 0;
failed:
    if (rc) snprintf(err, errcap, "%s", n.err[0] ? n.err : "the unpack failed");
    free(n.meta);
done:
    free(in), free(out);
    if (z) ZSTD_freeDStream(z);
    return rc;
}

#ifdef NUP_TEST
int main(int argc, char **argv) {
    char err[512] = "";
    const int fd = argc > 2 ? open(argv[1], O_RDONLY) : -1;
    if (fd < 0) return puts("usage: nup_test <file.nup> <out dir>"), 2;
    const int rc = nup_extract_romfs(fd, argv[2], argc > 3 ? (uint32_t)atoi(argv[3]) : 0, err, sizeof err);
    printf("rc %d done %lld of %lld %s\n", rc, (long long)g_done, (long long)g_total, err);
    return rc ? 1 : 0;
}
#endif

#ifdef __ANDROID__
#include <jni.h>
/* (io.github.hookshot.LauncherActivity) null when done, else why not; the fd is closed here */
JNIEXPORT jstring JNICALL Java_io_github_hookshot_LauncherActivity_nativeExtract(JNIEnv *env, jclass cls, jint fd, jstring out_dir) {
    (void)cls;
    char err[512] = "";
    const char *out = (*env)->GetStringUTFChars(env, out_dir, NULL);
    const int rc = nup_extract_romfs(fd, out, 131072, err, sizeof err);
    (*env)->ReleaseStringUTFChars(env, out_dir, out);
    close(fd);
    return rc ? (*env)->NewStringUTF(env, err) : NULL;
}
JNIEXPORT jlong JNICALL Java_io_github_hookshot_LauncherActivity_nativeDone(JNIEnv *env, jclass cls) { (void)env, (void)cls; return g_done; }
JNIEXPORT jlong JNICALL Java_io_github_hookshot_LauncherActivity_nativeTotal(JNIEnv *env, jclass cls) { (void)env, (void)cls; return g_total; }
JNIEXPORT void JNICALL Java_io_github_hookshot_LauncherActivity_nativeCancel(JNIEnv *env, jclass cls) { (void)env, (void)cls; g_cancel = 1; }
#endif
