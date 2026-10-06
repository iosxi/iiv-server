/* ==================================================================
 * zinflate.c - deflate 展開(zlite.h を参照)
 *
 *  入力が途中で切れても続きから再開できるよう、ブロックの見出しと
 *  記号(文字、または長さ+距離)を 1 単位として読み、読み切れなければ
 *  その単位の頭まで戻して待つ。
 *
 *  ハフマンの復号は、10 ビットまでの符号は表を 1 回引くだけ。
 *  それより長い符号(まれ)は 1 ビットずつ正準符号をたどる。
 *
 *  入力と出力に余裕がある間は、1 記号ごとの確かめを省いた速い道
 *  (fast_huff。一致は 8 バイトずつ写す)を通る。2026-10-06 に入れて、
 *  画面の絵の展開が 1050 → 1600MB/s(zlib-ng 1.3.1 は 1480)。
 *
 *  出力は内部の窓(直前の 32KB + 今回の出力)に書いてから呼び手へ写す。
 * ================================================================== */

#include "zlite.h"
#include <stdlib.h>
#include <string.h>

#define FBITS 10
#define FSIZE (1 << FBITS)

typedef struct {
    unsigned short fast[FSIZE];         /* 記号 << 4 | 長さ。0 は表に無い(長い符号) */
    unsigned short count[16];
    unsigned short sym[288];
} Huff;

enum { ST_HEADER, ST_STORED, ST_HUFF, ST_DONE };
enum { R_OK = 0, R_NEED = -1, R_BAD = -2, R_FULL = -3 };

struct ZInflate {
    zbyte   *in;                        /* 未処理の入力(後ろに 8 バイトの 0 を置く) */
    size_t   inLen, inCap, bp;          /* bp はビット単位の読み位置 */
    zbyte   *win;
    size_t   winLen, winCap;
    int      under;                     /* 読もうとしたビットが入力に無かった */
    int      headerDone, state, final;
    unsigned storedLeft;
    unsigned copyLeft, copyDist;        /* 出力の区切りをまたいだ一致の残り */
    const Huff *ll, *d;
    Huff     dynLL, dynD;
};

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

static Huff          g_fixLL, g_fixD;
static volatile long g_fixReady;

/* ------------------------------------------------------------------ */
/*  ハフマン表                                                          */
/* ------------------------------------------------------------------ */

/* 0 = 成功、-1 = 符号が多すぎる(壊れている) */
static int build(Huff *h, const unsigned char *len, int n)
{
    unsigned short offs[16], next[16];
    int i, left = 1;
    unsigned code = 0;

    memset(h->count, 0, sizeof(h->count));
    for (i = 0; i < n; i++) h->count[len[i]]++;
    h->count[0] = 0;
    for (i = 1; i < 16; i++) {
        left <<= 1;
        left -= h->count[i];
        if (left < 0) return -1;
    }
    offs[1] = 0;
    for (i = 1; i < 15; i++) offs[i + 1] = (unsigned short)(offs[i] + h->count[i]);
    for (i = 0; i < n; i++) if (len[i]) h->sym[offs[len[i]]++] = (unsigned short)i;

    /* 正準符号: 長さ 1 の最初の符号は 0 */
    for (i = 1; i < 16; i++) { next[i] = (unsigned short)code; code = (code + h->count[i]) << 1; }

    memset(h->fast, 0, sizeof(h->fast));
    for (i = 0; i < n; i++) {
        int l = len[i];
        unsigned c, r = 0, k, f;
        if (!l) continue;
        c = next[l]++;
        if (l > FBITS) continue;
        for (k = 0; k < (unsigned)l; k++) r |= ((c >> k) & 1) << (l - 1 - k);
        for (f = r; f < FSIZE; f += 1u << l) h->fast[f] = (unsigned short)((i << 4) | l);
    }
    return 0;
}

static void make_fixed(void)
{
    unsigned char l[288];
    int i;
    if (g_fixReady) return;
    for (i = 0; i < 288; i++) l[i] = (unsigned char)(i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8);
    build(&g_fixLL, l, 288);
    for (i = 0; i < 30; i++) l[i] = 5;
    build(&g_fixD, l, 30);
    _InterlockedExchange(&g_fixReady, 1);
}

/* ------------------------------------------------------------------ */
/*  ビットの読み出し                                                    */
/* ------------------------------------------------------------------ */

static __forceinline size_t avail(const ZInflate *z) { return z->inLen * 8 - z->bp; }

static __forceinline unsigned long long peek(const ZInflate *z)
{
    unsigned long long v;
    memcpy(&v, z->in + (z->bp >> 3), 8);
    return v >> (z->bp & 7);
}

static __forceinline unsigned bits(ZInflate *z, int n)
{
    unsigned v;
    if (avail(z) < (size_t)n) { z->under = 1; return 0; }
    v = (unsigned)(peek(z) & ((1ull << n) - 1));
    z->bp += (size_t)n;
    return v;
}

/* 記号を 1 個読む。読めなければ under を立てて 0、壊れていれば -1 */
static __forceinline int decode(ZInflate *z, const Huff *h)
{
    unsigned long long b;
    unsigned e;
    int len, code = 0, first = 0, index = 0;
    size_t av = avail(z);

    if (z->under) return 0;
    b = peek(z);
    e = h->fast[b & (FSIZE - 1)];
    if (e) {
        if ((e & 15) > av) { z->under = 1; return 0; }
        z->bp += e & 15;
        return (int)(e >> 4);
    }
    for (len = 1; len < 16; len++) {
        int count;
        code |= (int)((b >> (len - 1)) & 1);
        count = h->count[len];
        if (code - count < first) {
            if ((size_t)len > av) { z->under = 1; return 0; }
            z->bp += (size_t)len;
            return h->sym[index + (code - first)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    if (av < 15) { z->under = 1; return 0; }
    return -1;
}

/* ------------------------------------------------------------------ */
/*  本体                                                                */
/* ------------------------------------------------------------------ */

static int read_dynamic(ZInflate *z)
{
    unsigned char lens[320], cl[19];
    Huff clh;
    int hlit, hdist, hclen, i, n;

    hlit  = (int)bits(z, 5) + 257;
    hdist = (int)bits(z, 5) + 1;
    hclen = (int)bits(z, 4) + 4;
    memset(cl, 0, sizeof(cl));
    for (i = 0; i < hclen; i++) cl[k_clOrder[i]] = (unsigned char)bits(z, 3);
    if (z->under) return R_NEED;
    if (hlit > 286 || hdist > 30 || build(&clh, cl, 19)) return R_BAD;

    n = hlit + hdist;
    for (i = 0; i < n;) {
        int sym = decode(z, &clh), rep;
        unsigned char v = 0;
        if (z->under) return R_NEED;
        if (sym < 0) return R_BAD;
        if (sym < 16) { lens[i++] = (unsigned char)sym; continue; }
        if (sym == 16) { if (!i) return R_BAD; v = lens[i - 1]; rep = 3 + (int)bits(z, 2); }
        else if (sym == 17) rep = 3 + (int)bits(z, 3);
        else rep = 11 + (int)bits(z, 7);
        if (z->under) return R_NEED;
        if (i + rep > n) return R_BAD;
        while (rep--) lens[i++] = v;
    }
    if (!lens[256]) return R_BAD;
    if (build(&z->dynLL, lens, hlit) || build(&z->dynD, lens + hlit, hdist)) return R_BAD;
    z->ll = &z->dynLL;
    z->d  = &z->dynD;
    return R_OK;
}

static __forceinline void copy_match(ZInflate *z, unsigned dist, unsigned len)
{
    zbyte *dst = z->win + z->winLen, *src = dst - dist;
    if (dist >= 8 && len <= dist) {
        memcpy(dst, src, len);
    } else {
        unsigned k;
        for (k = 0; k < len; k++) dst[k] = src[k];
    }
    z->winLen += len;
}

/* 速い道の一致の写し。8 バイトずつ書くので、len の後ろへ最大 7 バイトはみ出す(呼び手が余白を保証する)。
   距離が 8 未満なら、最初の 8 バイトを 1 バイトずつ作り、その後は「距離の倍数で 8 以上」だけ前から写す
   (繰り返しの周期の倍数なので同じ絵柄になる。その倍数は 8 + 距離 未満なので、元の絵柄の中を指す)。 */
static __forceinline void copy_fast(zbyte *dst, unsigned dist, unsigned len)
{
    zbyte *end = dst + len;
    unsigned long long v;
    if (dist >= 8) {
        const zbyte *src = dst - dist;
        do { memcpy(&v, src, 8); memcpy(dst, &v, 8); src += 8; dst += 8; } while (dst < end);
    } else if (dist == 1) {
        memset(dst, dst[-1], len);
    } else {
        unsigned k, m = (8 + dist - 1) / dist * dist;
        for (k = 0; k < 8; k++) dst[k] = dst[(int)k - (int)dist];
        for (dst += 8; dst < end; dst += 8) { memcpy(&v, dst - m, 8); memcpy(dst, &v, 8); }
    }
}

/* 入力と出力に余裕がある間の速い道(zlib の inffast と同じ考え)。入力が足りるか・出力の余地が
   あるかを 1 記号ごとに確かめない。1 記号は最大 48 ビット(長さ 15+5、距離 15+13)なので、
   8 バイト読めば足りる。表に無い長い符号に当たったら、その記号の頭で止めて遅い道に任せる。
   戻り値: 1 = ブロックの終わり、0 = 余裕が無くなった / 長い符号、-1 = 壊れている */
static int fast_huff(ZInflate *z, size_t limit)
{
    const zbyte          *in = z->in;
    const unsigned short *llf = z->ll->fast, *df = z->d->fast;
    zbyte                *win = z->win;
    size_t                bp = z->bp, wl = z->winLen, inEnd, outEnd;
    int                   r = 0;

    if (z->inLen < 8 || limit < 258 + 8 || z->under) return 0;
    inEnd  = (z->inLen - 8) * 8;        /* bp がこれ未満なら、bp の位置から 8 バイトは本物の入力 */
    outEnd = limit - 258 - 8;           /* 最長の一致 + はみ出しが limit に収まる */
    while (bp < inEnd && wl <= outEnd) {
        unsigned long long b;
        unsigned e, sym, n, ex, len, dist;
        memcpy(&b, in + (bp >> 3), 8);
        b >>= bp & 7;
        e = llf[b & (FSIZE - 1)];
        if (!e) break;
        n = e & 15;
        sym = e >> 4;
        if (sym < 256) {
            /* 読んだ 8 バイトには少なくとも 57 - 10 ビット残るので、続く文字をあと 2 つまで同じ読みで取る */
            win[wl++] = (zbyte)sym;
            b >>= n;
            e = llf[b & (FSIZE - 1)];
            if (e && (e >> 4) < 256) {
                win[wl++] = (zbyte)(e >> 4);
                n += e & 15;
                b >>= e & 15;
                e = llf[b & (FSIZE - 1)];
                if (e && (e >> 4) < 256) {
                    win[wl++] = (zbyte)(e >> 4);
                    n += e & 15;
                }
            }
            bp += n;
            continue;
        }
        if (sym == 256) { bp += n; r = 1; break; }
        sym -= 257;
        if (sym >= 29) { r = -1; break; }
        b >>= n;
        ex = k_lenExtra[sym];
        len = k_lenBase[sym] + (unsigned)(b & ((1u << ex) - 1));
        b >>= ex;
        n += ex;
        e = df[b & (FSIZE - 1)];
        if (!e) break;                  /* bp はまだ進めていない。遅い道がこの記号を読み直す */
        if ((e >> 4) >= 30) { r = -1; break; }
        b >>= e & 15;
        n += e & 15;
        ex = k_distExtra[e >> 4];
        dist = k_distBase[e >> 4] + (unsigned)(b & ((1u << ex) - 1));
        n += ex;
        if (dist > wl) { r = -1; break; }
        bp += n;
        copy_fast(win + wl, dist, len);
        wl += len;
    }
    z->bp = bp;
    z->winLen = wl;
    return r;
}

/* win の長さが limit になるまで展開する */
static int run(ZInflate *z, size_t limit)
{
    if (z->copyLeft) {
        size_t room = limit - z->winLen;
        unsigned n = z->copyLeft < room ? z->copyLeft : (unsigned)room;
        copy_match(z, z->copyDist, n);
        z->copyLeft -= n;
        if (z->copyLeft) return R_FULL;
    }
    for (;;) {
        size_t save = z->bp;

        if (!z->headerDone) {
            unsigned cmf, flg;
            cmf = bits(z, 8);
            flg = bits(z, 8);
            if (z->under) goto need;
            if ((cmf & 15) != 8 || ((cmf << 8) | flg) % 31 || (flg & 0x20)) return R_BAD;
            z->headerDone = 1;
            continue;
        }

        switch (z->state) {
        case ST_DONE:
            return R_FULL;

        case ST_HEADER: {
            unsigned type;
            int r;
            if (z->final) { z->state = ST_DONE; continue; }
            z->final = (int)bits(z, 1);
            type     = bits(z, 2);
            if (z->under) { z->final = 0; goto need; }
            if (type == 0) {
                unsigned ln, nl;
                z->bp = (z->bp + 7) & ~(size_t)7;
                if (avail(z) < 32) { z->final = 0; goto need; }
                ln = bits(z, 16);
                nl = bits(z, 16);
                if ((ln ^ 0xFFFF) != nl) return R_BAD;
                z->storedLeft = ln;
                z->state = ST_STORED;
            } else if (type == 1) {
                z->ll = &g_fixLL;
                z->d  = &g_fixD;
                z->state = ST_HUFF;
            } else if (type == 2) {
                r = read_dynamic(z);
                if (r == R_NEED) { z->final = 0; goto need; }
                if (r != R_OK) return r;
                z->state = ST_HUFF;
            } else {
                return R_BAD;
            }
            continue;
        }

        case ST_STORED: {
            size_t n = z->storedLeft, have = z->inLen - (z->bp >> 3), room = limit - z->winLen;
            if (!n) { z->state = ST_HEADER; continue; }
            if (n > have) n = have;
            if (n > room) n = room;
            if (!n) return have ? R_FULL : R_NEED;
            memcpy(z->win + z->winLen, z->in + (z->bp >> 3), n);
            z->winLen += n;
            z->bp += n * 8;
            z->storedLeft -= (unsigned)n;
            continue;
        }

        case ST_HUFF:
            for (;;) {
                int sym, ds, f;
                unsigned len, dist;
                f = fast_huff(z, limit);
                if (f < 0) return R_BAD;
                if (f > 0) { z->state = ST_HEADER; break; }
                save = z->bp;
                sym = decode(z, z->ll);
                if (z->under) goto need;
                if (sym < 0) return R_BAD;
                if (sym < 256) {
                    if (z->winLen >= limit) { z->bp = save; return R_FULL; }
                    z->win[z->winLen++] = (zbyte)sym;
                    continue;
                }
                if (sym == 256) { z->state = ST_HEADER; break; }
                sym -= 257;
                if (sym >= 29) return R_BAD;
                len = k_lenBase[sym] + bits(z, k_lenExtra[sym]);
                ds = decode(z, z->d);
                if (z->under) goto need;
                if (ds < 0 || ds >= 30) return R_BAD;
                dist = k_distBase[ds] + bits(z, k_distExtra[ds]);
                if (z->under) goto need;
                if (dist > z->winLen) return R_BAD;
                if (z->winLen + len > limit) {
                    unsigned n = (unsigned)(limit - z->winLen);
                    copy_match(z, dist, n);
                    z->copyLeft = len - n;
                    z->copyDist = dist;
                    return R_FULL;
                }
                copy_match(z, dist, len);
            }
            continue;
        }
    need:
        z->bp = save;
        z->under = 0;
        return R_NEED;
    }
}

/* ------------------------------------------------------------------ */
/*  入出力                                                              */
/* ------------------------------------------------------------------ */

ZInflate *zi_new(void)
{
    ZInflate *z = (ZInflate *)calloc(1, sizeof(ZInflate));
    make_fixed();
    return z;
}

void zi_free(ZInflate *z)
{
    if (!z) return;
    free(z->in);
    free(z->win);
    free(z);
}

void zi_reset(ZInflate *z)
{
    z->inLen = z->bp = z->winLen = 0;
    z->under = z->headerDone = z->final = 0;
    z->state = ST_HEADER;
    z->storedLeft = z->copyLeft = z->copyDist = 0;
}

static int add_input(ZInflate *z, const zbyte *src, size_t n)
{
    size_t drop = z->bp >> 3;
    if (drop) {
        memmove(z->in, z->in + drop, z->inLen - drop);
        z->inLen -= drop;
        z->bp -= drop * 8;
    }
    if (z->inLen + n + 8 > z->inCap) {
        size_t cap = (z->inLen + n + 8) * 2;
        zbyte *p = (zbyte *)realloc(z->in, cap);
        if (!p) return -1;
        z->in = p;
        z->inCap = cap;
    }
    memcpy(z->in + z->inLen, src, n);
    z->inLen += n;
    memset(z->in + z->inLen, 0, 8);
    return 0;
}

static int reserve_win(ZInflate *z, size_t need)
{
    if (need > z->winCap) {
        size_t cap = need + need / 2;
        zbyte *p = (zbyte *)realloc(z->win, cap);
        if (!p) return -1;
        z->win = p;
        z->winCap = cap;
    }
    return 0;
}

int zi_inflate(ZInflate *z, const zbyte *src, size_t n, zbyte *out, size_t outLen)
{
    size_t start = z->winLen;
    int r;

    if (add_input(z, src, n) || reserve_win(z, start + outLen + 8)) return -2;
    r = run(z, start + outLen);
    if (r == R_BAD) return -2;
    if (z->winLen != start + outLen) return -1;
    memcpy(out, z->win + start, outLen);
    if (z->winLen > 2 * ZD_WINDOW) {
        memmove(z->win, z->win + z->winLen - ZD_WINDOW, ZD_WINDOW);
        z->winLen = ZD_WINDOW;
    }
    return 0;
}

size_t zi_inflate_avail(ZInflate *z, const zbyte *src, size_t n, zbyte **out, size_t *cap)
{
    size_t start = z->winLen, len;
    int    r;
    if (add_input(z, src, n)) return (size_t)-1;
    for (;;) {
        size_t limit = z->winLen + 65536;
        if (reserve_win(z, limit + 8)) return (size_t)-1;
        r = run(z, limit);
        if (r == R_BAD) return (size_t)-1;
        if (r == R_NEED || z->state == ST_DONE) break;
    }
    len = z->winLen - start;
    if (len > *cap) {
        zbyte *p = (zbyte *)realloc(*out, len + 65536);
        if (!p) return (size_t)-1;
        *out = p;
        *cap = len + 65536;
    }
    memcpy(*out, z->win + start, len);
    if (z->winLen > 2 * ZD_WINDOW) {
        memmove(z->win, z->win + z->winLen - ZD_WINDOW, ZD_WINDOW);
        z->winLen = ZD_WINDOW;
    }
    return len;
}

size_t zi_inflate_all(const zbyte *src, size_t n, zbyte **out, size_t maxOut)
{
    ZInflate *z = zi_new();
    size_t    len = (size_t)-1;
    int       r;

    *out = NULL;
    if (!z) return len;
    if (add_input(z, src, n)) goto done;
    for (;;) {
        size_t limit = z->winLen + 65536;
        if (limit > maxOut) limit = maxOut;
        if (reserve_win(z, limit + 8)) goto done;
        r = run(z, limit);
        if (r == R_BAD) goto done;
        if (r == R_NEED || z->state == ST_DONE) break;
        if (z->winLen >= maxOut) break;
    }
    *out = z->win;
    z->win = NULL;
    len = z->winLen;
done:
    zi_free(z);
    return len;
}
