/* beta-map-draw: the fast drawer for beta-map (MIT licence). It draws exactly what beta_map.py draws, the same
 * pixels, but in C, in little memory, and only what changed since the last run.
 *
 *   Build:  cc -std=c99 -O2 -o beta-map-draw beta-map-draw.c -lz -lm
 *   (beta_map.py finds it next to itself and uses it; it isn't meant to be run by hand.) -std=c99 for older
 *   compilers (GCC 4 defaults to C89), and because in ISO C mode GCC doesn't fuse a multiply and an add into one
 *   rounding: fused, some see-through pixels come out 1 off from beta_map.py's. The pragmas below say the same.
 *
 *   beta-map-draw WORLD CACHE STORE [--png-level 0..9] [--views 1..4]
 *
 * STORE gets the tiles (STORE/<layer>/<zoom>/<x>_<y>.png, the same layers as beta_map.py) and keeps them between
 * runs; CACHE keeps each region's drawing cut into tile pieces, and a list of the region files it drew (size, time
 * and a hash). A run reads only region files whose size or time changed (and redraws them only if their contents
 * did), then rebuilds only the tiles those regions touch, and the zoomed-out tiles over them. At the end it writes
 * CACHE/summary.json: each layer's extent and the torch tiles, for map.json.
 *
 * Safe to stop or fail at any point: new and removed tiles are staged in STORE/.next and only moved into place once
 * everything is drawn, so the website keeps showing the last good map until then. The record in CACHE and a mark
 * in STORE (.drawn) carry the same run id; when they don't match (the tiles were deleted, or beta_map.py drew in
 * Python, which removes them) everything is drawn again.
 *
 * Memory: a region file, the top-down data of its 512 x 512 columns, the blocks each column shows (from a little
 * under the ground to its top), and a 512 x 1280 strip of the picture at a time: the isometric picture of a region is
 * 2048 px wide, and every pixel column of it comes only from the blocks on one diagonal (x - z), so it's drawn in
 * strips that line up with the tiles. Typically 15 to 30 MB in all.
 *
 * Big-endian safe (the region files are big-endian anyway): it runs on PowerPC, like a Wii. */
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L     /* (with -std=c99: stat's st_mtim, dirent) */
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <zlib.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir(p, 0755)
#endif

#define DRAW_VERSION "c4"   /* bump when the drawing or the cache changes: everything is redrawn (c4: fog backgrounds) */
#define MAX_REGION 100000   /* region coordinates beyond this (the world border is at 58,594) are ignored */
#define T 512
#define IW 2048
#define IH 1280
#define SW 512              /* strip width */
#define NEAR 12
#define MINZ (-4)
#define INF 255

static int png_level = 1;
static int NV = 4;          /* how many of the four directions to draw (--views): 1 is a quarter of the work */
/* Where there's no world: the game's own fog colour (Beta 1.7.3: the Overworld's daytime fog 0.753, 0.847, 1.0; the
   Nether's 0.2, 0.03, 0.03), so the map's edges fade into it like the edge of view in game. The page uses the same. */
static const unsigned char BGS[2][3] = {{192, 216, 255}, {51, 8, 8}};

/* ------------------------------------------------------------------ small helpers */
static void *xmalloc(size_t n) { void *p = malloc(n ? n : 1); if (!p) { fprintf(stderr, "out of memory (%lu bytes)\n", (unsigned long)n); exit(2); } return p; }
/* deflate with a chosen strategy; wbits 15: zlib format (PNG), -15: raw (our own pieces, no checksum to work out).
 * *outn: room in out, then the size. Z_OK if done. */
static int pack(unsigned char *out, uLongf *outn, const unsigned char *in, size_t n, int level, int strategy, int wbits) {
    z_stream z; memset(&z, 0, sizeof z);
    if (deflateInit2(&z, level, Z_DEFLATED, wbits, 8, strategy) != Z_OK) return Z_MEM_ERROR;
    z.next_in = (unsigned char *)in; z.avail_in = (uInt)n; z.next_out = out; z.avail_out = (uInt)*outn;
    int s = deflate(&z, Z_FINISH);
    *outn = z.total_out; deflateEnd(&z);
    return s == Z_STREAM_END ? Z_OK : Z_BUF_ERROR;
}
static int unpack_raw(unsigned char *out, size_t n, const unsigned char *in, size_t inn) {
    z_stream z; memset(&z, 0, sizeof z);
    if (inflateInit2(&z, -15) != Z_OK) return -1;
    z.next_in = (unsigned char *)in; z.avail_in = (uInt)inn; z.next_out = out; z.avail_out = (uInt)n;
    int s = inflate(&z, Z_FINISH); size_t got = z.total_out; inflateEnd(&z);
    return s == Z_STREAM_END && got == n ? 0 : -1;
}
static int png_strategy = Z_DEFAULT_STRATEGY;
static void *xrealloc(void *q, size_t n) { void *p = realloc(q, n ? n : 1); if (!p) { fprintf(stderr, "out of memory (%lu bytes)\n", (unsigned long)n); exit(2); } return p; }
static uint32_t be32(const unsigned char *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void put32(unsigned char *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static int floordiv(int a, int b) { int q = a / b; if ((a % b) && ((a < 0) != (b < 0))) q--; return q; }   /* floor */
static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }
static unsigned char sat(int v) { return v > 255 ? 255 : (unsigned char)v; }

static void mkdirs(const char *path) {
    char buf[4096]; size_t n = strlen(path);
    if (n >= sizeof buf) return;
    memcpy(buf, path, n + 1);
    for (size_t i = 1; i <= n; i++)
        if (buf[i] == '/' || buf[i] == '\\' || buf[i] == 0) { char c = buf[i]; buf[i] = 0; MKDIR(buf); buf[i] = c; }
}

static unsigned char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    struct stat st;                                   /* sized to the file: no doubling while reading */
    size_t cap = stat(path, &st) == 0 && st.st_size > 0 ? (size_t)st.st_size + 1 : 1 << 20, n = 0;
    unsigned char *b = xmalloc(cap);
    for (;;) {
        if (n == cap) b = xrealloc(b, cap *= 2);
        size_t r = fread(b + n, 1, cap - n, f);
        if (!r) break;
        n += r;
    }
    fclose(f); *len = n; return b;
}

static int move_over(const char *from, const char *to) {
#ifdef _WIN32
    remove(to);                                     /* (rename doesn't replace there) */
#endif
    return rename(from, to);
}
static int write_file(const char *path, const void *a, size_t an, const void *b, size_t bn) {
    char tmp[4200]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    int ok = fwrite(a, 1, an, f) == an && (!bn || fwrite(b, 1, bn, f) == bn);
    ok = fclose(f) == 0 && ok;
    if (!ok) { remove(tmp); return -1; }
    return move_over(tmp, path);
}

/* ------------------------------------------------------------------ PNG: 8-bit RGB or RGBA, filter 0 */
static uint32_t crc_of(const unsigned char *t, const unsigned char *d, size_t n) {
    uLong c = crc32(0L, t, 4); return (uint32_t)crc32(c, d, (uInt)n);
}
static unsigned char *png_buf; static size_t png_cap;
static int png_write(const char *path, int w, int h, const unsigned char *px, int alpha) {
    int n = alpha ? 4 : 3; size_t row = (size_t)w * n + 1, raw_n = row * h;
    unsigned char *raw = xmalloc(raw_n);
    for (int y = 0; y < h; y++) { raw[y * row] = 0; memcpy(raw + y * row + 1, px + (size_t)y * w * n, (size_t)w * n); }
    uLongf zn = compressBound(raw_n);
    if (png_cap < zn + 64) { png_cap = zn + 64; png_buf = xrealloc(png_buf, png_cap); }
    unsigned char *o = png_buf;
    memcpy(o, "\x89PNG\r\n\x1a\n", 8);
    unsigned char ihdr[13]; put32(ihdr, w); put32(ihdr + 4, h); ihdr[8] = 8; ihdr[9] = alpha ? 6 : 2; ihdr[10] = ihdr[11] = ihdr[12] = 0;
    put32(o + 8, 13); memcpy(o + 12, "IHDR", 4); memcpy(o + 16, ihdr, 13); put32(o + 29, crc_of((const unsigned char *)"IHDR", ihdr, 13));
    if (pack(o + 41, &zn, raw, raw_n, png_level, png_strategy, 15) != Z_OK) { free(raw); return -1; }
    free(raw);
    put32(o + 33, (uint32_t)zn); memcpy(o + 37, "IDAT", 4); put32(o + 41 + zn, crc_of((const unsigned char *)"IDAT", o + 41, zn));
    size_t e = 45 + zn;
    put32(o + e, 0); memcpy(o + e + 4, "IEND", 4); put32(o + e + 8, crc_of((const unsigned char *)"IEND", o, 0));
    return write_file(path, o, e + 12, NULL, 0);
}
/* Our own PNGs back (RGB, filter 0 on every row): pixels into px (w*h*3). 0 if read. */
static int png_read_rgb(const char *path, unsigned char *px, int w, int h) {
    size_t len; unsigned char *d = read_file(path, &len);
    if (!d) return -1;
    size_t pos = 8, in = 0; unsigned char *idat = NULL; int ok = 0;
    while (pos + 12 <= len) {
        uint32_t n = be32(d + pos);
        if (n > len - pos - 12) break;
        if (!memcmp(d + pos + 4, "IDAT", 4)) { idat = xrealloc(idat, in + n); memcpy(idat + in, d + pos + 8, n); in += n; }
        pos += 12 + n;
    }
    size_t row = (size_t)w * 3 + 1; uLongf rn = row * h;
    unsigned char *raw = xmalloc(rn);
    if (idat && uncompress(raw, &rn, idat, in) == Z_OK && rn == row * h) {
        ok = 1;
        for (int y = 0; y < h; y++) { if (raw[y * row]) ok = 0; memcpy(px + (size_t)y * w * 3, raw + y * row + 1, (size_t)w * 3); }
    }
    free(raw); free(idat); free(d);
    return ok ? 0 : -1;
}

/* ------------------------------------------------------------------ blocks (Beta 1.7.3 ids), as in beta_map.py */
static unsigned char COL[256][3], HAS_COL[256];
static const unsigned char OTHER[3] = {150, 150, 150};
static const unsigned char WOOL[16][3] = {{222, 222, 222}, {234, 128, 56}, {190, 74, 201}, {110, 140, 210}, {196, 180, 30},
    {65, 174, 56}, {208, 132, 153}, {64, 64, 64}, {155, 161, 161}, {46, 110, 137}, {126, 61, 181}, {46, 56, 141}, {79, 50, 31},
    {53, 70, 27}, {150, 52, 48}, {25, 22, 22}};
static unsigned char ST[256], WATER[256], CLEAR[256], TREE[256], SMALL[256];
/* What the drawing keeps per block is a code, not the id: ids over 96 (none in Beta) all draw alike, so they're 97;
 * wool is 128 + its colour; water is 9, or 100 + its depth (0..10) where it's a surface. The tables below cover both. */
#define C_OTHER 97
#define C_SURF 100
#define C_WOOL 128
static unsigned char CODE_RGB[256][3];

static void tables(void) {
    static const int col[][4] = {{1,125,125,125},{2,106,160,64},{3,134,96,67},{4,112,112,112},{5,180,144,90},{7,60,60,60},
        {8,48,86,200},{9,48,86,200},{10,214,96,20},{11,214,96,20},{12,219,211,160},{13,136,126,126},{14,150,140,110},
        {15,140,128,120},{16,100,100,100},{17,102,81,50},{18,56,108,36},{19,195,195,80},{20,205,232,240},{21,100,110,150},
        {22,30,67,140},{23,110,110,110},{24,216,203,155},{25,100,67,50},{26,160,35,35},{27,150,120,80},{28,150,120,80},
        {29,110,110,90},{30,220,220,220},{33,110,110,90},{34,110,110,90},{35,222,222,222},{41,250,215,60},{42,220,220,220},
        {43,165,165,165},{44,165,165,165},{45,150,80,65},{46,200,60,40},{47,120,90,50},{48,80,110,80},{49,24,20,36},
        {51,240,140,30},{52,30,50,70},{53,180,144,90},{54,150,105,40},{56,129,140,143},{57,100,220,215},{58,140,95,55},
        {59,150,170,50},{60,110,75,45},{61,95,95,95},{62,95,95,95},{64,150,115,70},{65,160,125,75},{67,112,112,112},
        {71,190,190,190},{73,132,107,107},{74,132,107,107},{78,240,250,250},{79,165,195,245},{80,240,250,250},
        {81,20,120,30},{82,160,165,180},{83,140,190,90},{84,100,67,50},{85,150,115,70},{86,200,120,20},{87,120,52,50},
        {88,85,65,50},{89,245,225,140},{90,130,50,210},{91,220,140,30},{92,230,220,220},{93,150,150,150},{94,150,150,150},
        {95,150,105,40},{96,130,100,60}};
    for (size_t i = 0; i < sizeof col / sizeof col[0]; i++) {
        HAS_COL[col[i][0]] = 1; COL[col[i][0]][0] = col[i][1]; COL[col[i][0]][1] = col[i][2]; COL[col[i][0]][2] = col[i][3];
    }
    static const int st[] = {0, 6, 27, 28, 31, 32, 37, 38, 39, 40, 50, 51, 55, 63, 65, 66, 68, 69, 70, 72, 75, 76, 77};
    for (size_t i = 0; i < sizeof st / sizeof st[0]; i++) ST[st[i]] = CLEAR[st[i]] = 1;
    WATER[8] = WATER[9] = 1; CLEAR[85] = CLEAR[20] = CLEAR[8] = CLEAR[9] = 1;
    TREE[17] = TREE[18] = 1;
    static const int small[] = {66, 27, 28, 50, 75, 76, 85, 63, 68};
    for (size_t i = 0; i < sizeof small / sizeof small[0]; i++) SMALL[small[i]] = 1;
    for (int c = C_SURF; c <= C_SURF + 10; c++) WATER[c] = CLEAR[c] = 1;   /* (the codes; ids over 96 never get here) */
    for (int c = 0; c < 256; c++) {
        const unsigned char *rgb = c < 97 && HAS_COL[c] ? COL[c] : OTHER;
        if (c >= C_WOOL && c < C_WOOL + 16) rgb = WOOL[c - C_WOOL];
        memcpy(CODE_RGB[c], rgb, 3);
    }
}
static const unsigned char *col_rgb(int id) { return id < 256 && HAS_COL[id] ? COL[id] : OTHER; }

/* ------------------------------------------------------------------ NBT: just Level.Blocks and Level.Data */
static int skip(int t, const unsigned char **pp, const unsigned char *e, int depth) {
    const unsigned char *p = *pp;
    if (depth > 64) return -1;
    switch (t) {
    case 1: p += 1; break;
    case 2: p += 2; break;
    case 3: case 5: p += 4; break;
    case 4: case 6: p += 8; break;
    case 7: case 11: case 12: {
        if (e - p < 4) return -1;
        int32_t n = (int32_t)be32(p); p += 4;
        if (n < 0) return -1;
        size_t sz = t == 7 ? 1 : t == 11 ? 4 : 8;
        if ((size_t)(e - p) / sz < (size_t)n) return -1;
        p += (size_t)n * sz; break;
    }
    case 8: {
        if (e - p < 2) return -1;
        size_t n = (size_t)p[0] << 8 | p[1]; p += 2;
        if ((size_t)(e - p) < n) return -1;
        p += n; break;
    }
    case 9: {
        if (e - p < 5) return -1;
        int et = p[0]; int32_t n = (int32_t)be32(p + 1); p += 5;
        if (n < 0 || (et == 0 && n > 0)) return -1;
        for (int32_t i = 0; i < n; i++) if (skip(et, &p, e, depth + 1)) return -1;
        break;
    }
    case 10:
        for (;;) {
            if (p >= e) return -1;
            int ct = *p++;
            if (!ct) break;
            if (e - p < 2) return -1;
            size_t n = (size_t)p[0] << 8 | p[1]; p += 2;
            if ((size_t)(e - p) < n) return -1;
            p += n;
            if (skip(ct, &p, e, depth + 1)) return -1;
        }
        break;
    default: return -1;
    }
    if (p > e) return -1;
    *pp = p; return 0;
}
/* Blocks (32768) and Data (16384) of a chunk's NBT; 0 if both found. */
static int level_arrays(const unsigned char *p, const unsigned char *e, const unsigned char **blocks, const unsigned char **data) {
    *blocks = *data = NULL;
    if (e - p < 3 || p[0] != 10) return -1;
    size_t n = (size_t)p[1] << 8 | p[2]; p += 3 + n;
    if (p > e) return -1;
    for (int level = 0; ; ) {                       /* the root compound, then Level inside it */
        if (p >= e) return -1;
        int ct = *p++;
        if (!ct) return level ? (*blocks && *data ? 0 : -1) : -1;
        if (e - p < 2) return -1;
        n = (size_t)p[0] << 8 | p[1]; p += 2;
        if ((size_t)(e - p) < n) return -1;
        const unsigned char *name = p; p += n;
        if (!level && ct == 10 && n == 5 && !memcmp(name, "Level", 5)) { level = 1; continue; }
        if (level && ct == 7 && e - p >= 4) {
            int32_t len = (int32_t)be32(p);
            if (n == 6 && !memcmp(name, "Blocks", 6) && len == 32768 && e - p >= 4 + 32768) *blocks = p + 4;
            if (n == 4 && !memcmp(name, "Data", 4) && len == 16384 && e - p >= 4 + 16384) *data = p + 4;
        }
        if (skip(ct, &p, e, 1)) return -1;
        if (level && *blocks && *data) return 0;
    }
}

/* ------------------------------------------------------------------ one region file */
/* Chunks are unpacked once each, a row of 32 at a time, keeping three rows: a column's drawing starts 2 under the
 * lowest ground within NEAR (12) blocks, so a row's starts are known once the next row is read, and the blocks kept
 * for a column reach down to its neighbours' starts, known one row later again. */
#define KEEP_ROWS 3
typedef struct {
    unsigned char *file; size_t len; unsigned char *out; size_t cap;
    z_stream zraw, zany; int raw_ok, any_ok;
    unsigned char *rows;                               /* KEEP_ROWS x 32 chunks x (Blocks 32768 + Data 16384) */
    unsigned char ok[KEEP_ROWS][32];
} Reader;
#define CHUNK_BYTES (32768 + 16384)

/* chunk i's Blocks and Data, or -1 (missing or broken: it stays blank, as in beta_map.py). zlib data (nearly all)
 * is unpacked raw, past its 2-byte header: zlib's checksum costs a tenth of the whole run otherwise. */
static int chunk(Reader *r, int i, const unsigned char **blocks, const unsigned char **data) {
    uint32_t loc = be32(r->file + 4 * i);
    if (!loc) return -1;
    size_t off = (size_t)(loc >> 8) * 4096;
    if (off + 5 > r->len) return -1;
    size_t ln = be32(r->file + off);
    if (ln < 1) return -1;
    size_t cn = ln - 1;
    if (cn > r->len - off - 5) cn = r->len - off - 5;
    const unsigned char *in = r->file + off + 5;
    z_stream *z;
    if (r->file[off + 4] == 2) {                     /* zlib, as beta_map.py: a bad header is a broken chunk */
        if (cn <= 2 || (in[0] & 15) != 8 || (in[1] & 32) || (in[0] * 256 + in[1]) % 31) return -1;
        if (!r->raw_ok) { memset(&r->zraw, 0, sizeof r->zraw); if (inflateInit2(&r->zraw, -15) != Z_OK) return -1; r->raw_ok = 1; }
        else inflateReset(&r->zraw);
        z = &r->zraw; in += 2; cn -= 2;
        /* (the checksum at the end isn't checked: a chunk whose data unpacks but whose checksum is wrong is drawn,
         * where beta_map.py leaves it blank; that takes damage inside the last 4 bytes, and saves a tenth of the run) */
    } else {                                          /* anything else is gzip, as beta_map.py */
        if (!r->any_ok) { memset(&r->zany, 0, sizeof r->zany); if (inflateInit2(&r->zany, 15 + 16) != Z_OK) return -1; r->any_ok = 1; }
        else inflateReset(&r->zany);
        z = &r->zany;
    }
    z->next_in = (unsigned char *)in; z->avail_in = (uInt)cn;
    size_t got = 0;
    for (;;) {
        if (r->cap - got < 65536) { r->cap = r->cap ? r->cap * 2 : 131072; r->out = xrealloc(r->out, r->cap); }
        z->next_out = r->out + got; z->avail_out = (uInt)(r->cap - got);
        int s = inflate(z, Z_NO_FLUSH);
        got = r->cap - z->avail_out;
        if (s == Z_STREAM_END) break;
        if (s != Z_OK || (z->avail_in == 0 && z->avail_out != 0)) return -1;
        if (got > (64u << 20)) return -1;
    }
    return level_arrays(r->out, r->out + got, blocks, data);
}

/* per column, index z * 512 + x (the region's own, unturned coordinates) */
typedef struct {
    unsigned char *exists, *h, *up, *start, *lo, *hi;   /* top block, highest drawn, where drawing starts, lowest and
                                                       highest kept (one over the highest drawn: the Nether's top can be
                                                       under solid rock, when the roof has no air under it) */
    uint32_t *off; unsigned char *codes; size_t ncodes, capcodes;
    unsigned char *gr, *rowmin, *water;              /* ground (INF: no column), its min along x within NEAR, water top */
} Columns;

static int top_of(const unsigned char *col, int nether, int *hy_out) {
    int hy = 127;
    while (hy >= 0 && !col[hy]) hy--;
    *hy_out = hy;
    int y;
    if (!nether) { y = hy; while (y > 0 && ST[col[y]]) y--; }
    else { y = imin(hy, 126); while (y > 0 && !ST[col[y]]) y--; while (y > 0 && ST[col[y]]) y--; }
    return y;
}

/* the top-down picture of a column's top block (beta_map.py's block_colour) */
static void block_colour(const unsigned char *col, const unsigned char *data, int base, int y, unsigned char *out) {
    int b = col[y];
    if (WATER[b]) {
        int d = y;
        while (d > 0 && WATER[col[d]]) d--;
        const unsigned char *under = col_rgb(col[d]), *w = COL[9];
        double f = (double)imin(y - d, 8) / 8 * 0.3 + 0.62;
        for (int i = 0; i < 3; i++) out[i] = (unsigned char)(int)(w[i] * f + under[i] * (1 - f));
        return;
    }
    if (b == 35) { int k = base + y; memcpy(out, WOOL[(data[k >> 1] >> (4 * (k & 1))) & 15], 3); return; }
    memcpy(out, col_rgb(b), 3);
}

/* chunk row q: unpack its chunks into their slot; tops, ground, the top-down picture and heights */
static void row_read(Reader *r, int q, int nether, Columns *c, unsigned char *flat, unsigned char *hp) {
    int slot = q % KEEP_ROWS;
    for (int cx = 0; cx < 32; cx++) {
        const unsigned char *blocks, *data;
        unsigned char *keep = r->rows + ((size_t)slot * 32 + cx) * CHUNK_BYTES;
        r->ok[slot][cx] = 0;
        if (chunk(r, q * 32 + cx, &blocks, &data)) continue;
        memcpy(keep, blocks, 32768); memcpy(keep + 32768, data, 16384);
        r->ok[slot][cx] = 1;
        blocks = keep; data = keep + 32768;
        int ox = cx * 16, oz = q * 16;
        for (int lx = 0; lx < 16; lx++) for (int lz = 0; lz < 16; lz++) {
            int base = lx * 2048 + lz * 128, hy, idx = (oz + lz) * T + ox + lx;
            const unsigned char *col = blocks + base;
            int y = top_of(col, nether, &hy);
            if (y < 0) continue;
            c->exists[idx] = 1; c->h[idx] = y; c->up[idx] = nether ? y : hy;
            int g = y;
            while (g > 0 && (CLEAR[col[g]] || TREE[col[g]])) g--;
            c->gr[idx] = g;
            hp[3 * idx] = y; hp[3 * idx + 1] = 255; c->water[idx] = WATER[col[y]];
            block_colour(col, data, base, y, flat + 3 * idx);
        }
    }
    for (int z = q * 16; z < q * 16 + 16; z++) for (int x = 0; x < T; x++) {   /* the lowest ground along x */
        int m = INF;
        for (int j = imax(0, x - NEAR); j <= imin(T - 1, x + NEAR); j++) if (c->gr[z * T + j] < m) m = c->gr[z * T + j];
        c->rowmin[z * T + x] = m;
    }
}
/* chunk row q (once row q + 1 is read): where each column's drawing starts */
static void row_start(int q, Columns *c) {
    for (int z = q * 16; z < q * 16 + 16; z++) for (int x = 0; x < T; x++) {
        int m = INF;
        for (int j = imax(0, z - NEAR); j <= imin(T - 1, z + NEAR); j++) if (c->rowmin[j * T + x] < m) m = c->rowmin[j * T + x];
        int fl = m == INF ? 0 : imax(0, m - 2), idx = z * T + x;
        c->start[idx] = c->exists[idx] ? imin(c->h[idx], fl) : 0;
    }
}
/* chunk row q (once row q + 1's starts are known): the blocks each column shows, as codes, from the lowest start of
 * it and its four neighbours (any of them can look at its side) to one over its top */
static void row_codes(Reader *r, int q, Columns *c) {
    int slot = q % KEEP_ROWS;
    for (int cx = 0; cx < 32; cx++) {
        if (!r->ok[slot][cx]) continue;
        const unsigned char *blocks = r->rows + ((size_t)slot * 32 + cx) * CHUNK_BYTES, *data = blocks + 32768;
        int ox = cx * 16, oz = q * 16;
        for (int lx = 0; lx < 16; lx++) for (int lz = 0; lz < 16; lz++) {
            int x = ox + lx, z = oz + lz, idx = z * T + x, base = lx * 2048 + lz * 128;
            if (!c->exists[idx]) continue;
            int lo = c->start[idx];
            if (x > 0 && c->exists[idx - 1]) lo = imin(lo, c->start[idx - 1]);
            if (x < T - 1 && c->exists[idx + 1]) lo = imin(lo, c->start[idx + 1]);
            if (z > 0 && c->exists[idx - T]) lo = imin(lo, c->start[idx - T]);
            if (z < T - 1 && c->exists[idx + T]) lo = imin(lo, c->start[idx + T]);
            const unsigned char *col = blocks + base;
            int hi = imin(127, c->up[idx] + 1), n = hi - lo + 1;
            c->lo[idx] = lo; c->hi[idx] = hi;
            if (c->ncodes + n > c->capcodes) { c->capcodes = (c->ncodes + n) * 2; c->codes = xrealloc(c->codes, c->capcodes); }
            c->off[idx] = (uint32_t)c->ncodes;
            unsigned char *o = c->codes + c->ncodes; c->ncodes += n;
            for (int y = lo; y <= hi; y++) {
                int b = col[y], code = b;
                if (WATER[b]) {
                    if (!(y < 127 && WATER[col[y + 1]])) {
                        int d = y;
                        while (d > 0 && WATER[col[d]]) d--;
                        code = C_SURF + imin(y - d, 10);
                    } else code = 9;
                } else if (b == 35) { int k = base + y; code = C_WOOL + ((data[k >> 1] >> (4 * (k & 1))) & 15); }
                else if (b > 96) code = C_OTHER;
                o[y - lo] = code;
            }
        }
    }
}

static void read_region(Reader *r, int nether, Columns *c, unsigned char *flat, unsigned char *hp) {
    memset(c->exists, 0, T * T); memset(c->gr, INF, T * T); memset(c->water, 0, T * T);
    const unsigned char *BG = BGS[nether ? 1 : 0];
    for (int i = 0; i < T * T; i++) memcpy(flat + 3 * i, BG, 3);
    memset(hp, 0, 3 * T * T);
    c->ncodes = 0;
    for (int step = 0; step < 32 + 2; step++) {
        if (step < 32) row_read(r, step, nether, c, flat, hp);
        if (step >= 1 && step - 1 < 32) row_start(step - 1, c);
        if (step >= 2) row_codes(r, step - 2, c);
    }
    /* top-down shading: higher than the block north of it is lit, lower is in shadow (water stays flat) */
    for (int z = 1; z < T; z++) for (int x = 0; x < T; x++) {
        int p = z * T + x;
        if (c->water[p]) continue;
        int dh = hp[3 * p] - hp[3 * (p - T)];
        if (!dh || !memcmp(flat + 3 * p, BG, 3)) continue;
        double f = dh > 0 ? 1.14 : 0.84;
        if (abs(dh) > 2) f = dh > 0 ? 1.24 : 0.72;
        for (int k = 0; k < 3; k++) { int v = (int)(flat[3 * p + k] * f); flat[3 * p + k] = v > 255 ? 255 : v; }
    }
}

/* ------------------------------------------------------------------ the isometric picture, one strip at a time */
typedef struct { unsigned char *rgb, *mask, *wet, *under, *under_a, *tor; int s0, s1; } Strip;
static Strip S;

static void put(int row, int x, const unsigned char *c) {
    if (x < S.s0 || x >= S.s1) return;
    int q = row * SW + x - S.s0;
    memcpy(S.rgb + 3 * q, c, 3); S.mask[q] = 255; S.wet[q] = 0; S.tor[q] = 0;
}
static void blend(int row, int x, const unsigned char *c, double a, int water) {
    if (x < S.s0 || x >= S.s1) return;
    int q = row * SW + x - S.s0;
    unsigned char p0[3]; int a0;
    if (water) {
        if (!S.wet[q]) { memcpy(S.under + 3 * q, S.rgb + 3 * q, 3); S.under_a[q] = S.mask[q]; S.wet[q] = 1; }
        memcpy(p0, S.under + 3 * q, 3); a0 = S.under_a[q];
    } else { memcpy(p0, S.rgb + 3 * q, 3); a0 = S.mask[q]; S.wet[q] = 0; }
    for (int i = 0; i < 3; i++) S.rgb[3 * q + i] = (unsigned char)(int)(c[i] * a + p0[i] * (1 - a));
    double m = nearbyint(255 * a + a0 * (1 - a));   /* Python's round(): halves to even */
    S.mask[q] = m > 255 ? 255 : (unsigned char)(int)m; S.tor[q] = 0;
}
static void shade(const unsigned char *c, double f, unsigned char *o) { for (int i = 0; i < 3; i++) o[i] = sat((int)(c[i] * f)); }
static void torch(int row, int x, int v) { if (x >= S.s0 && x < S.s1) S.tor[row * SW + x - S.s0] = v; }

static void sprite(int b, int row, int X) {
    if (b == 66 || b == 27 || b == 28) {               /* rails: on the top of the block under them */
        static const unsigned char gold[3] = {170, 140, 60}, plain[3] = {118, 98, 78};
        const unsigned char *c = b == 27 ? gold : plain;
        put(row + 2, X + 1, c); put(row + 2, X + 2, c);
        for (int x = 0; x < 4; x++) put(row + 3, X + x, c);
    } else if (b == 50 || b == 75 || b == 76) {        /* torches: a flame and a stick, in their own layer */
        torch(row + 1, X + 1, b == 50 ? 1 : 2); torch(row + 2, X + 1, 3);
    } else if (b == 37 || b == 38) {                   /* flowers: one pixel, just above the block under them */
        static const unsigned char dand[3] = {0xff, 0xe2, 0x28}, rose[3] = {0xd6, 0x22, 0x22};
        put(row + 1, X + 1, b == 37 ? dand : rose);
    } else if (b == 85) {                              /* fence post */
        const unsigned char *c = COL[85]; unsigned char l[3], r[3];
        shade(c, 0.70, l); shade(c, 0.86, r);
        put(row, X + 1, c); put(row, X + 2, c);
        for (int k = 1; k <= 3; k++) { put(row + k, X + 1, l); put(row + k, X + 2, r); }
    } else if (b == 63 || b == 68) {                   /* signs */
        static const unsigned char c[3] = {180, 144, 90}, post[3] = {110, 85, 55}; unsigned char a[3], d[3];
        shade(c, 0.8, a); shade(c, 0.86, d);
        put(row + 1, X + 1, c); put(row + 1, X + 2, c); put(row + 2, X + 1, a); put(row + 2, X + 2, d);
        if (b == 63) put(row + 3, X + 1, post);
    }
}

static int orig_of(int lx, int lz, int k) {            /* view k's (lx, lz) -> the region's own column index */
    for (int i = 0; i < k; i++) { int a = lx; lx = lz; lz = 511 - a; }
    return lz * T + lx;
}
static int code_at(const Columns *c, int idx, int y) {
    if (y < c->lo[idx] || y > c->hi[idx]) return 0;
    return c->codes[c->off[idx] + y - c->lo[idx]];
}

static void draw_strip(const Columns *c, int k) {
    static const unsigned char WATER_RGB[3] = {34, 82, 214}, SIDE1[3] = {34, 68, 170}, SIDE2[3] = {28, 56, 140};
    static const unsigned char GLASS_RGB[3] = {205, 232, 240};
    int w = S.s1 - S.s0;
    for (int row = 0; row < IH; row++) {
        size_t q = (size_t)row * SW;
        memset(S.rgb + 3 * q, 0, 3 * w); memset(S.mask + q, 0, w); memset(S.wet + q, 0, w);
        memset(S.under + 3 * q, 0, 3 * w); memset(S.under_a + q, 0, w); memset(S.tor + q, 0, w);
    }
    for (int v = 0; v < 1023; v++) {                   /* back to front: x + z grows towards the viewer */
        for (int lx = imax(0, v - 511); lx <= imin(511, v); lx++) {
            int lz = v - lx, X = 2 * (lx - lz) + 1022;
            if (X + 3 < S.s0 || X >= S.s1) continue;
            int idx = orig_of(lx, lz, k);
            if (!c->exists[idx]) continue;
            int up = c->up[idx];
            int i1 = lx < 511 ? orig_of(lx + 1, lz, k) : -1, i2 = lz < 511 ? orig_of(lx, lz + 1, k) : -1;
            int e1 = i1 >= 0 && c->exists[i1], e2 = i2 >= 0 && c->exists[i2];
            int h1 = e1 ? c->h[i1] : -1, h2 = e2 ? c->h[i2] : -1;
            for (int y = c->start[idx]; y <= up; y++) {
                int b = code_at(c, idx, y);
                if (!b) continue;
                int row = (lx + lz) - 2 * y + 254;
                if (SMALL[b] || b == 37 || b == 38) { sprite(b, row, X); continue; }
                if (ST[b]) continue;
                int b1 = y <= h1 ? code_at(c, i1, y) : 0, b2 = y <= h2 ? code_at(c, i2, y) : 0;
                int open1 = y > h1 || CLEAR[b1], open2 = y > h2 || CLEAR[b2];
                if (WATER[b]) {
                    if (b >= C_SURF && b <= C_SURF + 10) {   /* the surface: the deeper, the less of the floor shows */
                        double a = 0.52 + (b - C_SURF) * 0.038;
                        blend(row, X + 1, WATER_RGB, a, 1); blend(row, X + 2, WATER_RGB, a, 1);
                        for (int x = 0; x < 4; x++) blend(row + 1, X + x, WATER_RGB, a, 1);
                    }
                    if (e1 && open1 && !(y <= h1 && WATER[b1]))
                        { blend(row + 2, X + 2, SIDE1, 0.7, 1); blend(row + 2, X + 3, SIDE1, 0.7, 1); blend(row + 3, X + 2, SIDE1, 0.7, 1); blend(row + 3, X + 3, SIDE1, 0.7, 1); }
                    if (e2 && open2 && !(y <= h2 && WATER[b2]))
                        { blend(row + 2, X, SIDE2, 0.7, 1); blend(row + 2, X + 1, SIDE2, 0.7, 1); blend(row + 3, X, SIDE2, 0.7, 1); blend(row + 3, X + 1, SIDE2, 0.7, 1); }
                    continue;
                }
                if (!(y == 127 || CLEAR[code_at(c, idx, y + 1)] || open1 || open2)) continue;
                if (b == 20) {                         /* glass: tinted, see-through */
                    unsigned char g1[3], g8[3], g9[3];
                    for (int i = 0; i < 3; i++) { g1[i] = GLASS_RGB[i]; g8[i] = (int)(GLASS_RGB[i] * .8); g9[i] = (int)(GLASS_RGB[i] * .9); }
                    blend(row, X + 1, g1, 0.38, 0); blend(row, X + 2, g1, 0.38, 0);
                    for (int x = 0; x < 4; x++) blend(row + 1, X + x, g1, 0.38, 0);
                    blend(row + 2, X, g8, 0.38, 0); blend(row + 2, X + 1, g8, 0.38, 0); blend(row + 3, X, g8, 0.38, 0); blend(row + 3, X + 1, g8, 0.38, 0);
                    blend(row + 2, X + 2, g9, 0.38, 0); blend(row + 2, X + 3, g9, 0.38, 0); blend(row + 3, X + 2, g9, 0.38, 0); blend(row + 3, X + 3, g9, 0.38, 0);
                    continue;
                }
                const unsigned char *t = CODE_RGB[b]; unsigned char l[3], r[3];
                shade(t, 0.70, l); shade(t, 0.86, r);
                put(row, X + 1, t); put(row, X + 2, t);
                for (int x = 0; x < 4; x++) put(row + 1, X + x, t);
                for (int rr = 2; rr <= 3; rr++) { put(row + rr, X, l); put(row + rr, X + 1, l); put(row + rr, X + 2, r); put(row + rr, X + 3, r); }
            }
        }
    }
}

/* ------------------------------------------------------------------ tile pieces (CACHE/p-<world>-<view>-<rx>_<rz>.bin) */
/* A region's picture in view k, cut at the tile edges: per piece the tile, its place in it, and zlib(alpha, rgb
 * (premultiplied by alpha), torch codes). Header: "BMP2", count; then 28-byte entries; then the data. */
typedef struct { int tx, ty, x0, y0, w, h; uint32_t off, len; } Piece;
typedef struct { Piece *p; int n, cap; unsigned char *data; size_t dn, dcap; } Pieces;

static void turn_region(int *rx, int *rz, int k) { for (int i = 0; i < k; i++) { int a = *rx; *rx = -*rz - 1; *rz = a; } }

static void cut_strip(Pieces *ps, int X0, int Y0, unsigned char *tmp) {
    int w = S.s1 - S.s0, tx = floordiv(X0 + S.s0, T), x0 = X0 + S.s0 - tx * T;
    for (int r0 = 0; r0 < IH; ) {
        int ty = floordiv(Y0 + r0, T), r1 = imin(IH, (ty + 1) * T - Y0), h = r1 - r0;
        int any = 0;
        for (int row = r0; row < r1 && !any; row++)
            for (int x = 0; x < w; x++) if (S.mask[row * SW + x] || S.tor[row * SW + x]) { any = 1; break; }
        if (any) {
            size_t n = (size_t)w * h;
            for (int row = r0; row < r1; row++) {
                size_t o = (size_t)(row - r0) * w, q = (size_t)row * SW;
                memcpy(tmp + o, S.mask + q, w); memcpy(tmp + n + 3 * o, S.rgb + 3 * q, 3 * w); memcpy(tmp + 4 * n + o, S.tor + q, w);
            }
            uLongf zn = compressBound(5 * n);
            if (ps->dcap < ps->dn + zn) { ps->dcap = (ps->dn + zn) * 2; ps->data = xrealloc(ps->data, ps->dcap); }
            zn += 64;                                  /* (raw deflate: a little over compressBound to be safe) */
            if (ps->dcap < ps->dn + zn) { ps->dcap = (ps->dn + zn) * 2; ps->data = xrealloc(ps->data, ps->dcap); }
            if (pack(ps->data + ps->dn, &zn, tmp, 5 * n, 1, Z_DEFAULT_STRATEGY, -15) != Z_OK) { fprintf(stderr, "compress failed\n"); exit(2); }
            if (ps->n == ps->cap) { ps->cap = ps->cap ? ps->cap * 2 : 32; ps->p = xrealloc(ps->p, ps->cap * sizeof(Piece)); }
            Piece *p = &ps->p[ps->n++];
            p->tx = tx; p->ty = ty; p->x0 = x0; p->y0 = Y0 + r0 - ty * T; p->w = w; p->h = h; p->off = (uint32_t)ps->dn; p->len = (uint32_t)zn;
            ps->dn += zn;
        }
        r0 = r1;
    }
}
static int save_pieces(const char *path, const Pieces *ps) {
    size_t hn = 8 + 28 * (size_t)ps->n;
    unsigned char *h = xmalloc(hn);
    memcpy(h, "BMP2", 4); put32(h + 4, ps->n);
    for (int i = 0; i < ps->n; i++) {
        unsigned char *e = h + 8 + 28 * i; const Piece *p = &ps->p[i];
        put32(e, (uint32_t)p->tx); put32(e + 4, (uint32_t)p->ty); put32(e + 8, p->x0); put32(e + 12, p->y0);
        put32(e + 16, (uint32_t)(p->w << 16 | p->h)); put32(e + 20, p->off); put32(e + 24, p->len);
    }
    int r = write_file(path, h, hn, ps->data, ps->dn);
    free(h); return r;
}
/* the piece of tile (tx, ty) in a pieces file: planes into buf (5 * w * h); 0 if there is one */
static int load_piece(const char *path, int tx, int ty, Piece *out, unsigned char *buf) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    unsigned char h[8], e[28]; int found = -1;
    if (fread(h, 1, 8, f) == 8 && !memcmp(h, "BMP2", 4)) {
        uint32_t n = be32(h + 4);
        for (uint32_t i = 0; i < n && n < 100000; i++) {
            if (fread(e, 1, 28, f) != 28) break;
            if ((int32_t)be32(e) == tx && (int32_t)be32(e + 4) == ty) {
                uint32_t x0 = be32(e + 8), y0 = be32(e + 12);
                out->tx = tx; out->ty = ty;
                out->w = be32(e + 16) >> 16; out->h = be32(e + 16) & 0xffff; out->off = be32(e + 20); out->len = be32(e + 24);
                /* (our own file, but never trusted: it has to fit the tile, and its data its size) */
                if (x0 > T || y0 > T || out->w < 1 || out->h < 1 || out->w > T || out->h > T) break;
                out->x0 = (int)x0; out->y0 = (int)y0;
                if (out->x0 + out->w > T || out->y0 + out->h > T) break;
                if (out->len > compressBound(5 * (uLong)out->w * out->h) + 64) break;
                unsigned char *z = xmalloc(out->len);
                if (!fseek(f, 8 + 28 * (long)n + (long)out->off, SEEK_SET) && fread(z, 1, out->len, f) == out->len) {
                    uLongf rn = 5 * (uLongf)out->w * out->h;
                    if (!unpack_raw(buf, rn, z, out->len)) found = 0;
                }
                free(z); break;
            }
        }
    }
    fclose(f); return found;
}

/* ------------------------------------------------------------------ the world's regions and what was drawn last time */
/* what a run does with a region */
enum { KEEP,                /* drawn before, the same file: nothing to do */
       DRAW,                /* new, or changed: draw it */
       SAME_FILE,           /* (while reading the record: same size and time as last time) */
       CHECK };             /* (same: size or time differ; drawn again only if its contents do) */
typedef struct { int w, rx, rz; long long size, mtime; uint64_t hash; int dirty, gone; char path[1400]; } Region;

/* "r.<x>.<z>.mcr", the numbers written plainly (no "+", no leading zeros, no "-0") and inside MAX_REGION: the
 * same names beta_map.py takes. 0 if it is one. */
static int region_name(const char *s, int *rx, int *rz) {
    if (strncmp(s, "r.", 2)) return -1;
    s += 2;
    for (int i = 0; i < 2; i++) {
        int neg = *s == '-'; s += neg;
        if (*s < '0' || *s > '9' || (s[0] == '0' && s[1] >= '0' && s[1] <= '9') || (neg && s[0] == '0')) return -1;
        long v = 0;
        while (*s >= '0' && *s <= '9') { v = v * 10 + (*s++ - '0'); if (v > MAX_REGION) return -1; }
        if (*s++ != '.') return -1;
        *(i ? rz : rx) = (int)(neg ? -v : v);
    }
    return strcmp(s, "mcr") ? -1 : 0;
}
/* where a region's picture goes in view k: its top-left pixel (X0, Y0); the region turned for that view */
static void region_origin(int rx, int rz, int k, int *trx, int *trz, int *X0, int *Y0) {
    turn_region(&rx, &rz, k);
    *trx = rx; *trz = rz; *X0 = (rx - rz) * 1024 - 1022; *Y0 = (rx + rz) * 512 - 254;
}
static Region *R; static int NR, CAPR;
static const char *WNAME[2] = {"overworld", "nether"};
static const char *ISO[2] = {"iso", "nether-iso"}, *FLAT[2] = {"flat", "nether"}, *HEIGHT[2] = {"height", "nether-height"};

static void find_regions(const char *world) {
    for (int w = 0; w < 2; w++) {
        char dir[1100]; snprintf(dir, sizeof dir, w ? "%s/DIM-1/region" : "%s/region", world);
        DIR *d = opendir(dir);
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d))) {
            int rx, rz;
            if (region_name(e->d_name, &rx, &rz)) continue;
            char p[1400]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
            struct stat st;
            if (stat(p, &st) || st.st_size <= 8192) continue;   /* beta_map.py: too small to have a chunk */
            if (NR == CAPR) { CAPR = CAPR ? CAPR * 2 : 64; R = xrealloc(R, CAPR * sizeof(Region)); }
            Region *g = &R[NR++]; memset(g, 0, sizeof *g);
            g->w = w; g->rx = rx; g->rz = rz; g->size = st.st_size;
#if defined(__linux__)
            g->mtime = (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
#else
            g->mtime = (long long)st.st_mtime * 1000000000LL;
#endif
            snprintf(g->path, sizeof g->path, "%s", p);
        }
        closedir(d);
    }
}
static uint64_t fnv(const unsigned char *d, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) { h ^= d[i]; h *= 1099511628211ULL; }
    return h;
}

/* tiles to redo, per layer: a growing list of (x, y), made unique before use */
typedef struct { int *xy; int n, cap; } TileSet;
static void ts_add(TileSet *s, int x, int y) {
    if (s->n == s->cap) { s->cap = s->cap ? s->cap * 2 : 256; s->xy = xrealloc(s->xy, s->cap * 2 * sizeof(int)); }
    s->xy[2 * s->n] = x; s->xy[2 * s->n + 1] = y; s->n++;
}
static int cmp_xy(const void *a, const void *b) {
    const int *p = a, *q = b; return p[0] != q[0] ? (p[0] < q[0] ? -1 : 1) : p[1] != q[1] ? (p[1] < q[1] ? -1 : 1) : 0;
}
static void ts_unique(TileSet *s) {
    if (!s->n) return;
    qsort(s->xy, s->n, 2 * sizeof(int), cmp_xy);
    int m = 1;
    for (int i = 1; i < s->n; i++) if (cmp_xy(s->xy + 2 * i, s->xy + 2 * (m - 1))) { s->xy[2 * m] = s->xy[2 * i]; s->xy[2 * m + 1] = s->xy[2 * i + 1]; m++; }
    s->n = m;
}

static char STORE[1024], CACHE[1024], STAGE[1100];
static void pieces_path(char *out, size_t n, const Region *g, int k) { snprintf(out, n, "%s/p-%d-%d-%d_%d.bin", CACHE, g->w, k, g->rx, g->rz); }

/* Tiles: written and removed in STAGE (a removal is an empty file there), read from STAGE first; commit() moves
 * them into STORE at the end of a run that got that far. */
static void tile_path(char *out, size_t n, const char *base, const char *layer, int z, int x, int y) {
    snprintf(out, n, "%s/%s/%d/%d_%d.png", base, layer, z, x, y);
}
static void staged_dir(const char *layer, int z) { char d[1200]; snprintf(d, sizeof d, "%s/%s/%d", STAGE, layer, z); mkdirs(d); }
static void tile_write(const char *layer, int z, int x, int y, const unsigned char *px, int alpha) {
    char p[1200]; staged_dir(layer, z); tile_path(p, sizeof p, STAGE, layer, z, x, y);
    if (png_write(p, T, T, px, alpha)) { fprintf(stderr, "can't write %s\n", p); exit(1); }
}
static void tile_remove(const char *layer, int z, int x, int y) {
    char p[1200]; tile_path(p, sizeof p, STORE, layer, z, x, y);
    FILE *f = fopen(p, "rb");
    if (f) fclose(f);                                 /* only if it's there now (or staged: then it's replaced) */
    tile_path(p, sizeof p, STAGE, layer, z, x, y);
    if (!f) { remove(p); return; }
    staged_dir(layer, z);
    if (write_file(p, "", 0, NULL, 0)) { fprintf(stderr, "can't write %s\n", p); exit(1); }
}
static int tile_read_rgb(const char *layer, int z, int x, int y, unsigned char *px) {
    char p[1200]; struct stat st;
    tile_path(p, sizeof p, STAGE, layer, z, x, y);
    if (stat(p, &st) == 0) return st.st_size ? png_read_rgb(p, px, T, T) : -1;   /* staged, or staged as removed */
    tile_path(p, sizeof p, STORE, layer, z, x, y);
    return png_read_rgb(p, px, T, T);
}

/* the tiles a region's picture covers in view k */
static void iso_tiles(const Region *g, int k, TileSet *s) {
    int rx, rz, X0, Y0; region_origin(g->rx, g->rz, k, &rx, &rz, &X0, &Y0);
    for (int ty = floordiv(Y0, T); ty <= floordiv(Y0 + IH - 1, T); ty++)
        for (int tx = floordiv(X0, T); tx <= floordiv(X0 + IW - 1, T); tx++) ts_add(s, tx, ty);
}

/* ------------------------------------------------------------------ compose, zoom out */
typedef struct { int i, key0, key1; } Order;
static int cmp_order(const void *a, const void *b) {
    const Order *p = a, *q = b;
    return p->key0 != q->key0 ? (p->key0 < q->key0 ? -1 : 1) : p->key1 != q->key1 ? (p->key1 < q->key1 ? -1 : 1) : 0;
}
static void compose_tile(int w, int k, int tx, int ty, unsigned char *buf, unsigned char *tb, unsigned char *planes, unsigned char *rgba) {
    Order *ord = xmalloc((NR + 1) * sizeof(Order)); int n = 0;
    for (int i = 0; i < NR; i++) {
        const Region *g = &R[i];
        if (g->w != w || g->gone) continue;
        int rx, rz, X0, Y0; region_origin(g->rx, g->rz, k, &rx, &rz, &X0, &Y0);
        if (X0 + IW <= tx * T || X0 >= (tx + 1) * T || Y0 + IH <= ty * T || Y0 >= (ty + 1) * T) continue;
        ord[n].i = i; ord[n].key0 = rx + rz; ord[n].key1 = rx; n++;
    }
    qsort(ord, n, sizeof(Order), cmp_order);
    for (int i = 0; i < T * T; i++) memcpy(buf + 3 * i, BGS[w], 3);
    memset(tb, 0, T * T);
    int drawn = 0;
    for (int o = 0; o < n; o++) {
        char pp[1200]; Piece p;
        pieces_path(pp, sizeof pp, &R[ord[o].i], k);
        if (load_piece(pp, tx, ty, &p, planes)) continue;
        size_t pn = (size_t)p.w * p.h;
        const unsigned char *al = planes, *rgb = planes + pn, *tor = planes + 4 * pn;
        for (int r = 0; r < p.h; r++) for (int x = 0; x < p.w; x++) {
            size_t s = (size_t)r * p.w + x; int t = (p.y0 + r) * T + p.x0 + x;
            int a = al[s];
            if (a) {                                    /* covers torches of the regions behind */
                drawn = 1; tb[t] = 0;
                if (a == 255) memcpy(buf + 3 * t, rgb + 3 * s, 3);
                else {
                    double f = 1 - a / 255.0;
                    for (int c = 0; c < 3; c++) buf[3 * t + c] = sat((int)(rgb[3 * s + c] + buf[3 * t + c] * f));
                }
            }
            if (tor[s]) tb[t] = tor[s];
        }
    }
    free(ord);
    char layer[64];
    snprintf(layer, sizeof layer, "%s%d", ISO[w], k);
    if (drawn) tile_write(layer, 0, tx, ty, buf, 0); else tile_remove(layer, 0, tx, ty);
    snprintf(layer, sizeof layer, "%s%d-torches", ISO[w], k);
    int any = 0;
    static const unsigned char TORCH[4][4] = {{0, 0, 0, 0}, {255, 214, 80, 255}, {200, 40, 30, 255}, {100, 75, 45, 255}};
    for (int i = 0; i < T * T; i++) {
        int v = tb[i] < 4 ? tb[i] : 0;
        memcpy(rgba + 4 * i, TORCH[v], 4); if (v) any = 1;
    }
    if (any) tile_write(layer, 0, tx, ty, rgba, 1); else tile_remove(layer, 0, tx, ty);
}

static void zoom_out(const char *layer, const unsigned char *BG, TileSet *dirty, unsigned char *child, unsigned char *out) {
    TileSet cur = *dirty, next = {0};
    for (int z = -1; z >= MINZ; z--) {
        next.n = 0;
        for (int i = 0; i < cur.n; i++) ts_add(&next, floordiv(cur.xy[2 * i], 2), floordiv(cur.xy[2 * i + 1], 2));
        ts_unique(&next);
        for (int i = 0; i < next.n; i++) {
            int px = next.xy[2 * i], pz = next.xy[2 * i + 1], have = 0;
            for (int j = 0; j < T * T; j++) memcpy(out + 3 * j, BG, 3);
            for (int qx = 0; qx < 2; qx++) for (int qz = 0; qz < 2; qz++) {
                if (tile_read_rgb(layer, z + 1, px * 2 + qx, pz * 2 + qz, child)) continue;
                have = 1;
                for (int y = 0; y < T / 2; y++) {
                    const unsigned char *r1 = child + (size_t)(2 * y) * T * 3, *r2 = r1 + T * 3;
                    unsigned char *o = out + ((size_t)(qz * (T / 2) + y) * T + qx * (T / 2)) * 3;
                    for (int x = 0; x < T / 2; x++)
                        for (int c = 0; c < 3; c++) o[3 * x + c] = (r1[6 * x + c] + r1[6 * x + 3 + c] + r2[6 * x + c] + r2[6 * x + 3 + c]) >> 2;
                }
            }
            if (have) tile_write(layer, z, px, pz, out, 0); else tile_remove(layer, z, px, pz);
        }
        if (cur.xy != dirty->xy) free(cur.xy);
        cur = next; next = (TileSet){0};
    }
    if (cur.xy != dirty->xy) free(cur.xy);
}

/* ------------------------------------------------------------------ summary.json: extents and torch tiles */
static int list_tiles(const char *layer, int **xy) {
    char d[1200]; snprintf(d, sizeof d, "%s/%s/0", STORE, layer);
    DIR *dd = opendir(d); int n = 0, cap = 0; *xy = NULL;
    if (!dd) return 0;
    struct dirent *e;
    while ((e = readdir(dd))) {
        int x, y; char tail[8];
        if (sscanf(e->d_name, "%d_%d.%7s", &x, &y, tail) != 3 || strcmp(tail, "png")) continue;
        if (n == cap) { cap = cap ? cap * 2 : 64; *xy = xrealloc(*xy, cap * 2 * sizeof(int)); }
        (*xy)[2 * n] = x; (*xy)[2 * n + 1] = y; n++;
    }
    closedir(dd);
    return n;
}
static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static void summary(void) {
    char path[1200]; snprintf(path, sizeof path, "%s/summary.json", CACHE);
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "can't write %s\n", path); exit(1); }
    fprintf(f, "{\"layers\": {");
    int first = 1;
    for (int w = 0; w < 2; w++) {
        int n = 0, x0 = 0, z0 = 0, x1 = 0, z1 = 0;
        for (int i = 0; i < NR; i++) if (R[i].w == w && !R[i].gone) {
            if (!n++) { x0 = x1 = R[i].rx; z0 = z1 = R[i].rz; }
            x0 = imin(x0, R[i].rx); x1 = imax(x1, R[i].rx); z0 = imin(z0, R[i].rz); z1 = imax(z1, R[i].rz);
        }
        if (!n) continue;
        fprintf(f, "%s\"%s\": {\"bounds\": [%d, %d, %d, %d]}", first ? "" : ", ", FLAT[w], x0 * T, z0 * T, (x1 + 1) * T, (z1 + 1) * T);
        first = 0;
        for (int k = 0; k < NV; k++) {
            char layer[64]; int *xy; snprintf(layer, sizeof layer, "%s%d", ISO[w], k);
            int m = list_tiles(layer, &xy);
            if (m) {
                int a = xy[0], b = xy[1], c = xy[0], d = xy[1];
                for (int i = 1; i < m; i++) { a = imin(a, xy[2 * i]); b = imin(b, xy[2 * i + 1]); c = imax(c, xy[2 * i]); d = imax(d, xy[2 * i + 1]); }
                fprintf(f, ", \"%s\": {\"bounds\": [%d, %d, %d, %d]}", layer, a * T, b * T, (c + 1) * T, (d + 1) * T);
            } else fprintf(f, ", \"%s\": {\"bounds\": null}", layer);
            free(xy);
            snprintf(layer, sizeof layer, "%s%d-torches", ISO[w], k);
            m = list_tiles(layer, &xy);
            char **names = xmalloc((m + 1) * sizeof(char *));
            for (int i = 0; i < m; i++) { names[i] = xmalloc(32); snprintf(names[i], 32, "%d,%d", xy[2 * i], xy[2 * i + 1]); }
            qsort(names, m, sizeof(char *), cmp_str);   /* (sorted as text, like beta_map.py) */
            fprintf(f, ", \"%s\": {\"list\": [", layer);
            for (int i = 0; i < m; i++) { fprintf(f, "%s\"%s\"", i ? ", " : "", names[i]); free(names[i]); }
            fprintf(f, "]}");
            free(names); free(xy);
        }
    }
    fprintf(f, "}}\n");
    fclose(f);
}

/* ------------------------------------------------------------------ the run */
/* every layer's every zoom folder: fn(layer, z) */
static void each_layer(void (*fn)(const char *, int)) {
    for (int w = 0; w < 2; w++) {
        const char *names[2] = {FLAT[w], HEIGHT[w]};
        for (int j = 0; j < 2; j++) for (int z = 0; z >= MINZ; z--) fn(names[j], z);
        for (int k = 0; k < 4; k++) {
            char layer[64];
            snprintf(layer, sizeof layer, "%s%d", ISO[w], k);
            for (int z = 0; z >= MINZ; z--) fn(layer, z);
            snprintf(layer, sizeof layer, "%s%d-torches", ISO[w], k);
            for (int z = 0; z >= MINZ; z--) fn(layer, z);
        }
    }
}
/* the tile file names (x_y.png) in a folder: fn(folder, name) for each */
static void each_png(const char *dir, void (*fn)(const char *, const char *, const char *, int), const char *layer, int z) {
    DIR *dd = opendir(dir); struct dirent *e;
    if (!dd) return;
    while ((e = readdir(dd))) {
        size_t n = strlen(e->d_name);
        if (n > 4 && !strcmp(e->d_name + n - 4, ".png")) fn(dir, e->d_name, layer, z);
    }
    closedir(dd);
}
static void drop_staged(const char *dir, const char *name, const char *layer, int z) {
    (void)layer; (void)z; char p[1500]; snprintf(p, sizeof p, "%s/%s", dir, name); remove(p);
}
static void clear_stage_dir(const char *layer, int z) {   /* what a run that stopped early left staged */
    char d[1200]; snprintf(d, sizeof d, "%s/%s/%d", STAGE, layer, z); each_png(d, drop_staged, layer, z);
}
static void unstage_tile(const char *dir, const char *name, const char *layer, int z) {
    char from[1500], to[1500], d[1200]; struct stat st;
    snprintf(from, sizeof from, "%s/%s", dir, name);
    snprintf(d, sizeof d, "%s/%s/%d", STORE, layer, z); snprintf(to, sizeof to, "%s/%s", d, name);
    if (stat(from, &st)) return;
    if (!st.st_size) { remove(to); remove(from); return; }   /* staged as removed */
    mkdirs(d);
    if (move_over(from, to)) { fprintf(stderr, "can't move %s into place\n", from); exit(1); }
}
static void commit_dir(const char *layer, int z) {
    char d[1200]; snprintf(d, sizeof d, "%s/%s/%d", STAGE, layer, z); each_png(d, unstage_tile, layer, z);
}
static void remove_live(const char *dir, const char *name, const char *layer, int z) {
    int x, y;
    (void)dir; if (sscanf(name, "%d_%d.png", &x, &y) == 2) tile_remove(layer, z, x, y);
}
static void stage_remove_dir(const char *layer, int z) {
    char d[1200]; snprintf(d, sizeof d, "%s/%s/%d", STORE, layer, z); each_png(d, remove_live, layer, z);
}
/* starting over (a new drawing version, other views, or a record that doesn't match the tiles): every tile there
 * now is staged as removed (the ones drawn again replace that), and the old pieces go */
static void start_over(void) {
    each_layer(stage_remove_dir);
    DIR *dd = opendir(CACHE); struct dirent *e;
    if (!dd) return;
    while ((e = readdir(dd))) {
        size_t n = strlen(e->d_name);
        if (!strncmp(e->d_name, "p-", 2) && n > 4 && !strcmp(e->d_name + n - 4, ".bin")) {
            char p[1500]; snprintf(p, sizeof p, "%s/%s", CACHE, e->d_name); remove(p);
        }
    }
    closedir(dd);
}

/* the first line of a file, or "" */
static void first_line(const char *path, char *out, size_t n) {
    FILE *f = fopen(path, "r");
    out[0] = 0;
    if (!f) return;
    if (!fgets(out, (int)n, f)) out[0] = 0;
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: beta-map-draw WORLD CACHE STORE [--png-level 0..9] [--views 1..4]\n(run by beta_map.py)\n"); return 1; }
    for (int i = 4; i + 1 < argc; i++) {
        if (!strcmp(argv[i], "--png-level")) png_level = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--views")) NV = atoi(argv[++i]);
    }
    if (NV < 1 || NV > 4) NV = 4;
    if (png_level < 0 || png_level > 9) png_level = 1;
    tables();
    snprintf(CACHE, sizeof CACHE, "%s", argv[2]); snprintf(STORE, sizeof STORE, "%s", argv[3]);
    snprintf(STAGE, sizeof STAGE, "%s/.next", STORE);
    mkdirs(CACHE); mkdirs(STORE);
    each_layer(clear_stage_dir);
    find_regions(argv[1]);

    /* what was drawn last time: the record's first line names the version, the views and the run, and the tiles'
     * mark has to say the same, or the tiles aren't the ones the record describes */
    char statep[1200], markp[1200], head[256], mark[256], want[64];
    snprintf(statep, sizeof statep, "%s/state.txt", CACHE); snprintf(markp, sizeof markp, "%s/.drawn", STORE);
    first_line(statep, head, sizeof head); first_line(markp, mark, sizeof mark);
    snprintf(want, sizeof want, "beta-map-draw " DRAW_VERSION " views %d run ", NV);
    int same_version = head[0] && !strncmp(head, want, strlen(want)) && !strcmp(head, mark);
    FILE *sf = same_version ? fopen(statep, "r") : NULL;
    if (sf) {
        char line[512];
        if (!fgets(line, sizeof line, sf)) line[0] = 0;   /* (the first line, read above) */
        while (fgets(line, sizeof line, sf)) {
            int w, rx, rz; long long size, mtime; unsigned long long hash;
            if (sscanf(line, "%d %d %d %lld %lld %llx", &w, &rx, &rz, &size, &mtime, &hash) != 6) continue;
            if (w < 0 || w > 1 || rx < -MAX_REGION || rx > MAX_REGION || rz < -MAX_REGION || rz > MAX_REGION) continue;
            int found = 0;
            for (int i = 0; i < NR; i++) if (R[i].w == w && R[i].rx == rx && R[i].rz == rz && !R[i].gone) {
                found = 1; R[i].hash = hash;
                R[i].dirty = R[i].size == size && R[i].mtime == mtime ? SAME_FILE : CHECK;
            }
            if (!found) {                               /* gone since: its tiles change */
                if (NR == CAPR) { CAPR = CAPR ? CAPR * 2 : 64; R = xrealloc(R, CAPR * sizeof(Region)); }
                Region *g = &R[NR++]; memset(g, 0, sizeof *g); g->w = w; g->rx = rx; g->rz = rz; g->gone = 1;
            }
        }
        fclose(sf);
    }
    if (!same_version) { start_over(); for (int i = 0; i < NR; i++) R[i].dirty = DRAW; }
    for (int i = 0; i < NR; i++) {
        if (R[i].gone) { R[i].dirty = DRAW; continue; }
        if (R[i].dirty == KEEP) R[i].dirty = DRAW;     /* not in the record: new */
        else if (R[i].dirty == SAME_FILE) {            /* unchanged, if its pieces are all there */
            R[i].dirty = KEEP;
            for (int k = 0; k < NV; k++) {
                char p[1200]; pieces_path(p, sizeof p, &R[i], k);
                FILE *t = fopen(p, "rb");
                if (t) fclose(t); else R[i].dirty = DRAW;
            }
        }
    }

    int nw[2] = {0, 0}, todo = 0, done = 0;
    for (int i = 0; i < NR; i++) if (!R[i].gone) nw[R[i].w]++;
    for (int i = 0; i < NR; i++) if (R[i].dirty) todo++;
    printf("%d Overworld and %d Nether region(s); %d to check or draw\n", nw[0], nw[1], todo); fflush(stdout);

    for (int w = 0; w < 2; w++) {
        char d[1200];
        snprintf(d, sizeof d, "%s/%s/0", STORE, FLAT[w]); mkdirs(d);
        snprintf(d, sizeof d, "%s/%s/0", STORE, HEIGHT[w]); mkdirs(d);
        for (int k = 0; k < NV; k++) {
            snprintf(d, sizeof d, "%s/%s%d/0", STORE, ISO[w], k); mkdirs(d);
            snprintf(d, sizeof d, "%s/%s%d-torches/0", STORE, ISO[w], k); mkdirs(d);
        }
    }
    TileSet iso_dirty[2][4] = {{{0}}}, flat_dirty[2] = {{0}};
    Columns c = {0};
    c.exists = xmalloc(T * T); c.h = xmalloc(T * T); c.up = xmalloc(T * T); c.start = xmalloc(T * T); c.lo = xmalloc(T * T); c.hi = xmalloc(T * T);
    c.off = xmalloc(T * T * sizeof(uint32_t)); c.gr = xmalloc(T * T); c.rowmin = xmalloc(T * T); c.water = xmalloc(T * T);
    unsigned char *flat = xmalloc(3 * T * T), *hp = xmalloc(3 * T * T), *tmp = xmalloc(5 * (size_t)SW * T);
    S.rgb = xmalloc(3 * (size_t)SW * IH); S.mask = xmalloc((size_t)SW * IH); S.wet = xmalloc((size_t)SW * IH);
    S.under = xmalloc(3 * (size_t)SW * IH); S.under_a = xmalloc((size_t)SW * IH); S.tor = xmalloc((size_t)SW * IH);
    Reader rd = {0}; rd.rows = xmalloc((size_t)KEEP_ROWS * 32 * CHUNK_BYTES);
    static const int S0[5] = {0, 510, 1022, 1534, 2046}, S1[5] = {510, 1022, 1534, 2046, 2048};
    for (int i = 0; i < NR; i++) {
        Region *g = &R[i];
        if (!g->dirty) continue;
        char p[1200];
        if (!g->gone) {
            rd.file = read_file(g->path, &rd.len);
            if (!rd.file || rd.len < 8192) { free(rd.file); rd.file = NULL; g->gone = 1; }   /* gone while we looked */
        }
        if (g->gone) {
            for (int k = 0; k < 4; k++) { if (k < NV) iso_tiles(g, k, &iso_dirty[g->w][k]); pieces_path(p, sizeof p, g, k); remove(p); }
            ts_add(&flat_dirty[g->w], g->rx, g->rz);
            tile_remove(FLAT[g->w], 0, g->rx, g->rz); tile_remove(HEIGHT[g->w], 0, g->rx, g->rz);
            done++; continue;
        }
        uint64_t h = fnv(rd.file, rd.len);
        int drawn_before = 1;
        for (int k = 0; k < NV && drawn_before; k++) { pieces_path(p, sizeof p, g, k); FILE *t = fopen(p, "rb"); if (t) fclose(t); else drawn_before = 0; }
        if (g->dirty == CHECK && h == g->hash && drawn_before) { g->dirty = KEEP; free(rd.file); rd.file = NULL; done++; continue; }
        g->hash = h; g->dirty = DRAW;
        printf("  drawing %s r.%d.%d (%d/%d)\n", WNAME[g->w], g->rx, g->rz, ++done, todo); fflush(stdout);
        read_region(&rd, g->w == 1, &c, flat, hp);
        free(rd.file); rd.file = NULL;
        tile_write(FLAT[g->w], 0, g->rx, g->rz, flat, 0);
        tile_write(HEIGHT[g->w], 0, g->rx, g->rz, hp, 0);
        ts_add(&flat_dirty[g->w], g->rx, g->rz);
        for (int k = 0; k < NV; k++) {
            int rx, rz, X0, Y0; region_origin(g->rx, g->rz, k, &rx, &rz, &X0, &Y0);
            Pieces ps = {0};
            for (int s = 0; s < 5; s++) { S.s0 = S0[s]; S.s1 = S1[s]; draw_strip(&c, k); cut_strip(&ps, X0, Y0, tmp); }
            pieces_path(p, sizeof p, g, k);
            if (save_pieces(p, &ps)) { fprintf(stderr, "can't write %s\n", p); return 1; }
            free(ps.p); free(ps.data);
            iso_tiles(g, k, &iso_dirty[g->w][k]);
        }
    }
    if (rd.raw_ok) inflateEnd(&rd.zraw);
    if (rd.any_ok) inflateEnd(&rd.zany);
    free(rd.out); free(rd.rows); free(c.codes); free(c.gr); free(c.rowmin); free(c.water);
    free(S.rgb); free(S.mask); free(S.wet); free(S.under); free(S.under_a); free(S.tor);
    free(c.exists); free(c.h); free(c.up); free(c.start); free(c.lo); free(c.hi); free(c.off);

    /* the tiles those regions touch, then the zoomed-out ones over them */
    unsigned char *buf = xmalloc(3 * T * T), *tb = xmalloc(T * T), *rgba = xmalloc(4 * T * T);
    for (int w = 0; w < 2; w++) {
        if (flat_dirty[w].n) { ts_unique(&flat_dirty[w]); zoom_out(FLAT[w], BGS[w], &flat_dirty[w], flat, hp); }
        for (int k = 0; k < NV; k++) {
            TileSet *s = &iso_dirty[w][k];
            if (!s->n) continue;
            ts_unique(s);
            char layer[64]; snprintf(layer, sizeof layer, "%s%d", ISO[w], k);
            printf("  %s: %d tile(s) to redo\n", layer, s->n); fflush(stdout);
            for (int i = 0; i < s->n; i++) compose_tile(w, k, s->xy[2 * i], s->xy[2 * i + 1], buf, tb, tmp, rgba);
            zoom_out(layer, BGS[w], s, flat, hp);
        }
    }
    free(buf); free(tb); free(rgba); free(flat); free(hp); free(tmp); free(png_buf);
    for (int w = 0; w < 2; w++) { free(flat_dirty[w].xy); for (int k = 0; k < 4; k++) free(iso_dirty[w][k].xy); }

    /* everything's drawn: the staged tiles go into place, then the record and the tiles' mark, with a new run id
     * (stopped before this, the site still has the last good map; stopped during it, the record and the mark don't
     * match, so the next run starts over) */
    each_layer(commit_dir);
    char line[128], tmpstate[1300];
    snprintf(line, sizeof line, "beta-map-draw " DRAW_VERSION " views %d run %lx-%lx\n", NV, (unsigned long)time(NULL), (unsigned long)clock());
    snprintf(tmpstate, sizeof tmpstate, "%s.tmp", statep);
    sf = fopen(tmpstate, "w");
    if (!sf) { fprintf(stderr, "can't write %s\n", tmpstate); return 1; }
    fputs(line, sf);
    for (int i = 0; i < NR; i++) if (!R[i].gone)
        fprintf(sf, "%d %d %d %lld %lld %016llx\n", R[i].w, R[i].rx, R[i].rz, R[i].size, R[i].mtime, (unsigned long long)R[i].hash);
    if (fclose(sf) || move_over(tmpstate, statep)) { fprintf(stderr, "can't write %s\n", statep); return 1; }
    if (write_file(markp, line, strlen(line), NULL, 0)) { fprintf(stderr, "can't write %s\n", markp); return 1; }
    summary();
    printf("done\n");
    return 0;
}
