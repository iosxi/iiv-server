/* ==================================================================
 * zdeflate.c - deflate 圧縮(zlite.h を参照)
 *
 *  LZ77 はハッシュ(4 バイト)の連鎖で探す。レベル 1 は 1 回だけ引き、
 *  見つからない状態が続くと探す間隔を空ける(画素の雑音のように
 *  縮まないデータで時間を使わないため)。レベル 4 以上は遅延一致。
 *
 *  ブロックごとに、動的ハフマン・固定ハフマン・格納の 3 通りの
 *  大きさを計算して一番小さいものを書く。
 *
 *  ハフマン符号の長さは Moffat-Katajainen の方法で求め、上限
 *  (15 ビット / 符号長の符号は 7 ビット)を超えたら miniz と同じ方法で
 *  詰め直す。どの木も使う記号を 2 個以上にして「完全な木」にする
 *  (zlib は符号長の符号が不完全だとエラーにする)。
 * ================================================================== */

#include "zlite.h"
#include <stdlib.h>
#include <string.h>
#include <intrin.h>

#define HBITS       15
#define HSIZE       (1 << HBITS)
#define WMASK       (ZD_WINDOW - 1)
#define MIN_MATCH   4
#define MAX_MATCH   258
#define BLOCK_SYMS  32768

struct ZDWork {
    unsigned int   head[HSIZE];         /* 位置 + 1(0 = なし) */
    unsigned int   prev[ZD_WINDOW];
    unsigned short sym[BLOCK_SYMS];     /* 文字(0..255)か一致の長さ(3..258) */
    unsigned short dst[BLOCK_SYMS];     /* 一致の距離(文字なら 0) */
    unsigned int   llFreq[288];
    unsigned int   dFreq[32];
};

/* ------------------------------------------------------------------ */
/*  表                                                                  */
/* ------------------------------------------------------------------ */

static const unsigned short k_lenBase[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const unsigned char k_lenExtra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const unsigned short k_distBase[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
    1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const unsigned char k_distExtra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
static const unsigned char k_clOrder[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };

static unsigned char  g_lenCode[MAX_MATCH + 1];     /* 長さ → 0..28 */
static unsigned char  g_distLo[256];                /* 距離-1 (<256) → 符号 */
static unsigned char  g_distHi[256];                /* (距離-1)>>7 → 符号 */
static unsigned short g_fixCode[288];               /* 固定ハフマン(ビット反転済み) */
static unsigned char  g_fixLen[288];
static unsigned short g_fixDCode[32];
static unsigned char  g_fixDLen[32];
static volatile long  g_tablesReady;

static unsigned rev_bits(unsigned v, int n)
{
    unsigned r = 0;
    while (n--) { r = (r << 1) | (v & 1); v >>= 1; }
    return r;
}

static void make_tables(void)
{
    int i, c;
    unsigned code;
    int bl[16] = { 0 }, next[16];

    if (g_tablesReady) return;
    for (c = 0; c < 29; c++) {
        int n = 1 << k_lenExtra[c];
        for (i = 0; i < n && k_lenBase[c] + i <= MAX_MATCH; i++) g_lenCode[k_lenBase[c] + i] = (unsigned char)c;
    }
    g_lenCode[258] = 28;
    for (c = 0; c < 30; c++) {
        int n = 1 << k_distExtra[c];
        for (i = 0; i < n; i++) {
            int d = k_distBase[c] + i - 1;
            if (d < 256) g_distLo[d] = (unsigned char)c;
            if ((d >> 7) < 256 && d >= 256) g_distHi[d >> 7] = (unsigned char)c;
        }
    }
    for (i = 0; i < 288; i++) g_fixLen[i] = (unsigned char)(i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8);
    for (i = 0; i < 288; i++) bl[g_fixLen[i]]++;
    code = 0; bl[0] = 0;
    for (i = 1; i < 16; i++) { code = (code + bl[i - 1]) << 1; next[i] = (int)code; }
    for (i = 0; i < 288; i++) g_fixCode[i] = (unsigned short)rev_bits((unsigned)next[g_fixLen[i]]++, g_fixLen[i]);
    for (i = 0; i < 32; i++) { g_fixDLen[i] = 5; g_fixDCode[i] = (unsigned short)rev_bits((unsigned)i, 5); }
    _InterlockedExchange(&g_tablesReady, 1);
}

static __forceinline int dist_code(unsigned d)
{
    d--;
    return d < 256 ? g_distLo[d] : g_distHi[d >> 7];
}

ZDWork *zd_work_new(void)
{
    make_tables();
    return (ZDWork *)malloc(sizeof(ZDWork));
}

void zd_work_free(ZDWork *w) { free(w); }

size_t zd_bound(size_t len) { return len + (len >> 10) + 64; }

/* ------------------------------------------------------------------ */
/*  ビットの書き出し                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    zbyte             *p;
    unsigned long long bb;
    int                bc;
} BW;

static __forceinline void put(BW *w, unsigned v, int n)
{
    w->bb |= (unsigned long long)v << w->bc;
    w->bc += n;
    if (w->bc >= 32) {
        unsigned lo = (unsigned)w->bb;
        memcpy(w->p, &lo, 4);
        w->p += 4;
        w->bb >>= 32;
        w->bc -= 32;
    }
}

static void align(BW *w)
{
    while (w->bc > 0) {
        *w->p++ = (zbyte)w->bb;
        w->bb >>= 8;
        w->bc -= 8;
    }
    w->bb = 0;
    w->bc = 0;
}

static void write_stored(BW *w, const zbyte *src, size_t len)
{
    do {
        unsigned n = len > 65535 ? 65535 : (unsigned)len;
        put(w, 0, 3);                   /* BFINAL=0, BTYPE=00 */
        align(w);
        w->p[0] = (zbyte)n;  w->p[1] = (zbyte)(n >> 8);
        w->p[2] = (zbyte)~n; w->p[3] = (zbyte)(~n >> 8);
        w->p += 4;
        memcpy(w->p, src, n);
        w->p += n;
        src += n;
        len -= n;
    } while (len);
}

/* ------------------------------------------------------------------ */
/*  ハフマン符号の長さ                                                  */
/* ------------------------------------------------------------------ */

typedef struct { unsigned key; unsigned short sym; } SymFreq;

static int cmp_symfreq(const void *a, const void *b)
{
    const SymFreq *x = (const SymFreq *)a, *y = (const SymFreq *)b;
    if (x->key != y->key) return x->key < y->key ? -1 : 1;
    return (int)x->sym - (int)y->sym;
}

/* Moffat-Katajainen。A は頻度の昇順。終わると key が符号の長さになる */
static void min_redundancy(SymFreq *A, int n)
{
    int root, leaf, next, avbl, used, dpth;

    if (n == 0) return;
    if (n == 1) { A[0].key = 1; return; }
    A[0].key += A[1].key;
    root = 0; leaf = 2;
    for (next = 1; next < n - 1; next++) {
        if (leaf >= n || A[root].key < A[leaf].key) { A[next].key = A[root].key; A[root++].key = (unsigned)next; }
        else A[next].key = A[leaf++].key;
        if (leaf >= n || (root < next && A[root].key < A[leaf].key)) { A[next].key += A[root].key; A[root++].key = (unsigned)next; }
        else A[next].key += A[leaf++].key;
    }
    A[n - 2].key = 0;
    for (next = n - 3; next >= 0; next--) A[next].key = A[A[next].key].key + 1;
    avbl = 1; used = dpth = 0; root = n - 2; next = n - 1;
    while (avbl > 0) {
        while (root >= 0 && (int)A[root].key == dpth) { used++; root--; }
        while (avbl > used) { A[next--].key = (unsigned)dpth; avbl--; }
        avbl = 2 * used; dpth++; used = 0;
    }
}

/* freq[0..n) から長さ len[] と(ビット反転した)符号 code[] を作る。
   使う記号が 2 個未満なら頻度 1 の記号を足して 2 個にする。 */
static void build_code(unsigned *freq, int n, int maxLen, unsigned char *len, unsigned short *code)
{
    SymFreq A[288];
    int     cnt[33] = { 0 }, next[17];
    int     i, j, k, used = 0;
    unsigned total, c;

    for (i = 0; i < n; i++) if (freq[i]) used++;
    for (i = 0; used < 2 && i < n; i++) if (!freq[i]) { freq[i] = 1; used++; }

    k = 0;
    for (i = 0; i < n; i++) if (freq[i]) { A[k].key = freq[i]; A[k].sym = (unsigned short)i; k++; }
    qsort(A, (size_t)k, sizeof(A[0]), cmp_symfreq);
    min_redundancy(A, k);

    for (i = 0; i < k; i++) cnt[A[i].key > 32 ? 32 : A[i].key]++;
    for (i = maxLen + 1; i <= 32; i++) { cnt[maxLen] += cnt[i]; cnt[i] = 0; }
    total = 0;
    for (i = maxLen; i > 0; i--) total += (unsigned)cnt[i] << (maxLen - i);
    while (total != (1u << maxLen)) {
        cnt[maxLen]--;
        for (i = maxLen - 1; i > 0; i--) if (cnt[i]) { cnt[i]--; cnt[i + 1] += 2; break; }
        total--;
    }

    memset(len, 0, (size_t)n);
    for (i = 1, j = k; i <= maxLen; i++)
        for (c = (unsigned)cnt[i]; c > 0; c--) len[A[--j].sym] = (unsigned char)i;

    memset(cnt, 0, sizeof(cnt));
    for (i = 0; i < n; i++) cnt[len[i]]++;
    cnt[0] = 0;
    c = 0;
    for (i = 1; i <= maxLen; i++) { c = (c + (unsigned)cnt[i - 1]) << 1; next[i] = (int)c; }
    for (i = 0; i < n; i++) code[i] = len[i] ? (unsigned short)rev_bits((unsigned)next[len[i]]++, len[i]) : 0;
}

/* ------------------------------------------------------------------ */
/*  ブロック                                                            */
/* ------------------------------------------------------------------ */

static void write_syms(BW *w, const ZDWork *z, int nsym,
                       const unsigned char *llLen, const unsigned short *llCode,
                       const unsigned char *dLen, const unsigned short *dCode)
{
    int i;
    for (i = 0; i < nsym; i++) {
        unsigned s = z->sym[i], d = z->dst[i];
        if (!d) {
            put(w, llCode[s], llLen[s]);
        } else {
            int lc = g_lenCode[s], dc = dist_code(d);
            put(w, llCode[257 + lc], llLen[257 + lc]);
            if (k_lenExtra[lc]) put(w, s - k_lenBase[lc], k_lenExtra[lc]);
            put(w, dCode[dc], dLen[dc]);
            if (k_distExtra[dc]) put(w, d - k_distBase[dc], k_distExtra[dc]);
        }
    }
    put(w, llCode[256], llLen[256]);
}

static void flush_block(BW *w, ZDWork *z, int nsym, const zbyte *raw, size_t rawLen)
{
    unsigned char  llLen[288], dLen[32], clLen[19];
    unsigned short llCode[288], dCode[32], clCode[19];
    unsigned       clFreq[19] = { 0 };
    unsigned char  lens[288 + 32], rle[288 + 32], rleX[288 + 32];
    int            nrle = 0, hlit, hdist, hclen, i, j, n;
    unsigned long long dynBits, fixBits, storedBits, extra = 0;

    z->llFreq[256]++;
    for (i = 0; i < 29; i++) extra += (unsigned long long)z->llFreq[257 + i] * k_lenExtra[i];
    for (i = 0; i < 30; i++) extra += (unsigned long long)z->dFreq[i] * k_distExtra[i];

    fixBits = 3 + extra;
    for (i = 0; i < 286; i++) fixBits += (unsigned long long)z->llFreq[i] * g_fixLen[i];
    for (i = 0; i < 30; i++) fixBits += (unsigned long long)z->dFreq[i] * 5;

    build_code(z->llFreq, 286, 15, llLen, llCode);
    build_code(z->dFreq, 30, 15, dLen, dCode);
    for (hlit = 286; hlit > 257 && !llLen[hlit - 1]; hlit--) ;
    for (hdist = 30; hdist > 1 && !dLen[hdist - 1]; hdist--) ;

    /* 符号の長さの並びを 16/17/18 で詰める */
    memcpy(lens, llLen, (size_t)hlit);
    memcpy(lens + hlit, dLen, (size_t)hdist);
    n = hlit + hdist;
    for (i = 0; i < n; i = j) {
        int run;
        for (j = i + 1; j < n && lens[j] == lens[i]; j++) ;
        run = j - i;
        if (lens[i] == 0) {
            while (run >= 11) { int r = run > 138 ? 138 : run; rle[nrle] = 18; rleX[nrle++] = (unsigned char)(r - 11); run -= r; }
            if (run >= 3) { rle[nrle] = 17; rleX[nrle++] = (unsigned char)(run - 3); run = 0; }
            while (run-- > 0) { rle[nrle] = 0; rleX[nrle++] = 0; }
        } else {
            rle[nrle] = lens[i]; rleX[nrle++] = 0; run--;
            while (run >= 3) { int r = run > 6 ? 6 : run; rle[nrle] = 16; rleX[nrle++] = (unsigned char)(r - 3); run -= r; }
            while (run-- > 0) { rle[nrle] = lens[i]; rleX[nrle++] = 0; }
        }
    }
    for (i = 0; i < nrle; i++) clFreq[rle[i]]++;
    build_code(clFreq, 19, 7, clLen, clCode);
    for (hclen = 19; hclen > 4 && !clLen[k_clOrder[hclen - 1]]; hclen--) ;

    dynBits = 3 + 5 + 5 + 4 + 3ull * hclen + extra;
    for (i = 0; i < nrle; i++) dynBits += clLen[rle[i]] + (rle[i] == 16 ? 2 : rle[i] == 17 ? 3 : rle[i] == 18 ? 7 : 0);
    for (i = 0; i < 286; i++) dynBits += (unsigned long long)z->llFreq[i] * llLen[i];
    for (i = 0; i < 30; i++) dynBits += (unsigned long long)z->dFreq[i] * dLen[i];

    storedBits = (unsigned long long)rawLen * 8 + 40ull * (rawLen / 65535 + 1) + 7;

    if (storedBits <= dynBits && storedBits <= fixBits) {
        write_stored(w, raw, rawLen);
    } else if (fixBits <= dynBits) {
        put(w, 1 << 1, 3);              /* BFINAL=0, BTYPE=01 */
        write_syms(w, z, nsym, g_fixLen, g_fixCode, g_fixDLen, g_fixDCode);
    } else {
        put(w, 2 << 1, 3);              /* BFINAL=0, BTYPE=10 */
        put(w, (unsigned)(hlit - 257), 5);
        put(w, (unsigned)(hdist - 1), 5);
        put(w, (unsigned)(hclen - 4), 4);
        for (i = 0; i < hclen; i++) put(w, clLen[k_clOrder[i]], 3);
        for (i = 0; i < nrle; i++) {
            put(w, clCode[rle[i]], clLen[rle[i]]);
            if (rle[i] == 16) put(w, rleX[i], 2);
            else if (rle[i] == 17) put(w, rleX[i], 3);
            else if (rle[i] == 18) put(w, rleX[i], 7);
        }
        write_syms(w, z, nsym, llLen, llCode, dLen, dCode);
    }
    memset(z->llFreq, 0, sizeof(z->llFreq));
    memset(z->dFreq, 0, sizeof(z->dFreq));
}

/* ------------------------------------------------------------------ */
/*  LZ77                                                                */
/* ------------------------------------------------------------------ */

static __forceinline unsigned rd32(const zbyte *p) { unsigned v; memcpy(&v, p, 4); return v; }
static __forceinline unsigned hash4(unsigned v) { return (v * 2654435761u) >> (32 - HBITS); }

static __forceinline unsigned match_len(const zbyte *a, const zbyte *b, unsigned max)
{
    unsigned n = 0;
    while (n + 8 <= max) {
        unsigned long long x, y;
        memcpy(&x, a + n, 8);
        memcpy(&y, b + n, 8);
        if (x != y) {
            unsigned long idx;
            _BitScanForward64(&idx, x ^ y);
            return n + (idx >> 3);
        }
        n += 8;
    }
    while (n < max && a[n] == b[n]) n++;
    return n;
}

typedef struct { int chain, nice, lazy; } Level;

static const Level k_levels[10] = {
    { 0, 0, 0 }, { 1, 32, 0 }, { 2, 64, 0 }, { 4, 64, 0 }, { 4, 32, 1 },
    { 8, 64, 1 }, { 16, 128, 1 }, { 32, 128, 1 }, { 64, 258, 1 }, { 256, 258, 1 } };

/* 一致を探して長さを返す(MIN_MATCH 未満なら 0)。p はハッシュ表へ登録する */
static __forceinline unsigned find_match(ZDWork *z, const zbyte *buf, unsigned p, unsigned end,
                                         int chain, unsigned nice, unsigned *dist)
{
    unsigned cur = rd32(buf + p), h = hash4(cur), cand = z->head[h], best = 0;
    unsigned maxl = end - p < MAX_MATCH ? end - p : MAX_MATCH;

    z->head[h] = p + 1;
    z->prev[p & WMASK] = cand;
    while (cand && chain-- > 0) {
        unsigned c = cand - 1, l;
        if (c >= p || p - c > ZD_WINDOW) break;
        if (rd32(buf + c) == cur) {
            l = match_len(buf + c, buf + p, maxl);
            if (l > best) { best = l; *dist = p - c; if (l >= nice || l == maxl) break; }
        }
        cand = z->prev[c & WMASK];
    }
    return best >= MIN_MATCH ? best : 0;
}

static __forceinline void insert(ZDWork *z, const zbyte *buf, unsigned p)
{
    unsigned h = hash4(rd32(buf + p));
    z->prev[p & WMASK] = z->head[h];
    z->head[h] = p + 1;
}

size_t zd_compress(ZDWork *z, const zbyte *buf, size_t dictLen, size_t len,
                   zbyte *out, int level, int header)
{
    BW       w;
    unsigned p, end, blockStart, last4, misses = 0;
    int      nsym = 0;
    Level    lv;

    w.p = out; w.bb = 0; w.bc = 0;
    if (header) { *w.p++ = 0x78; *w.p++ = 0x01; }
    if (level < 0) level = 0;
    if (level > 9) level = 9;

    if (level == 0 || len < 16) {
        if (len) write_stored(&w, buf + dictLen, len);
        goto sync;
    }
    lv = k_levels[level];

    memset(z->head, 0, sizeof(z->head));
    memset(z->llFreq, 0, sizeof(z->llFreq));
    memset(z->dFreq, 0, sizeof(z->dFreq));

    end   = (unsigned)(dictLen + len);
    last4 = end - 3;                    /* p < last4 なら 4 バイト読める */
    p     = dictLen > ZD_WINDOW ? (unsigned)(dictLen - ZD_WINDOW) : 0;
    for (; p < dictLen && p < last4; p++) insert(z, buf, p);
    p = (unsigned)dictLen;
    blockStart = p;

    while (p < end) {
        unsigned l = 0, d = 0;

        if (nsym >= BLOCK_SYMS - 2) {
            flush_block(&w, z, nsym, buf + blockStart, p - blockStart);
            nsym = 0;
            blockStart = p;
        }
        if (p < last4) l = find_match(z, buf, p, end, lv.chain, (unsigned)lv.nice, &d);

        if (l && lv.lazy && l < (unsigned)lv.nice && p + 1 < last4) {
            unsigned d2 = 0, l2 = find_match(z, buf, p + 1, end, lv.chain, (unsigned)lv.nice, &d2);
            if (l2 > l) {
                /* p は文字で出し、p+1 の一致を使う */
                z->sym[nsym] = buf[p]; z->dst[nsym++] = 0; z->llFreq[buf[p]]++;
                p++;
                l = l2; d = d2;
            } else {
                /* p+1 は登録済み。一致の残りを登録する */
                unsigned q;
                z->sym[nsym] = (unsigned short)l; z->dst[nsym++] = (unsigned short)d;
                z->llFreq[257 + g_lenCode[l]]++; z->dFreq[dist_code(d)]++;
                for (q = p + 2; q < p + l && q < last4; q++) insert(z, buf, q);
                p += l;
                continue;
            }
        }

        if (l) {
            unsigned q, stop = p + l;
            z->sym[nsym] = (unsigned short)l; z->dst[nsym++] = (unsigned short)d;
            z->llFreq[257 + g_lenCode[l]]++; z->dFreq[dist_code(d)]++;
            misses = 0;
            if (level == 1 && l > 32) {         /* 長い一致は終わりだけ登録する */
                for (q = stop - 3; q < stop && q < last4; q++) insert(z, buf, q);
            } else {
                for (q = p + 1; q < stop && q < last4; q++) insert(z, buf, q);
            }
            p = stop;
        } else {
            unsigned step = 1, k;
            if (level <= 2) { step = 1 + (++misses >> 6); if (step > 32) step = 32; }
            for (k = 0; k < step && p < end && nsym < BLOCK_SYMS - 2; k++) {
                z->sym[nsym] = buf[p]; z->dst[nsym++] = 0; z->llFreq[buf[p]]++;
                p++;
            }
        }
    }
    if (nsym) flush_block(&w, z, nsym, buf + blockStart, p - blockStart);

sync:
    /* 同期フラッシュ: 空の格納ブロック */
    put(&w, 0, 3);
    align(&w);
    w.p[0] = 0; w.p[1] = 0; w.p[2] = 0xFF; w.p[3] = 0xFF;
    w.p += 4;
    return (size_t)(w.p - out);
}
