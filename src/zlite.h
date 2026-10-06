/* ==================================================================
 * zlite.h - RFB 用の zlib 互換 deflate / inflate
 *
 *  iiv-server と iiv-client で同じファイルを使う(中身をそろえる)。
 *
 *  RFB の zlib ストリーム(ZRLE、Tight の 4 本、拡張クリップボード)は
 *  接続中ずっと続き、矩形ごとに Z_SYNC_FLUSH で区切られる。
 *  終わり(BFINAL と Adler-32)は送られない。
 *
 *  圧縮側は「直前の最大 32KB を辞書として渡し、その続きを圧縮して
 *  同期フラッシュで終える」関数だけを持つ。状態を持たないので、
 *  ストリーム上で後に続く矩形も、前の矩形の圧縮を待たずに別スレッドで
 *  圧縮できる(元のデータさえ決まっていればよい。pigz と同じ方法)。
 *  受け手から見れば普通の 1 本の zlib ストリームと区別がつかない。
 *
 *  展開側は途中で入力が切れても続きから再開できる。
 * ================================================================== */
#ifndef ZLITE_H
#define ZLITE_H

#include <stddef.h>

typedef unsigned char  zbyte;

#define ZD_WINDOW 32768

/* ------------------------------------------------------------------ */
/*  圧縮                                                                */
/* ------------------------------------------------------------------ */

typedef struct ZDWork ZDWork;           /* スレッドごとの作業領域(約 600KB) */

ZDWork *zd_work_new(void);
void    zd_work_free(ZDWork *w);

/* 出力に必要な大きさの上限 */
size_t zd_bound(size_t len);

/* buf[0..dictLen) を辞書、buf[dictLen..dictLen+len) を圧縮する。
 *   header: 1 ならストリームの先頭(zlib の 2 バイトの見出し)を付ける
 *   level : 0 = 圧縮しない(格納ブロック)、1 = 最速 … 9 = 最大
 * 出力は同期フラッシュ(00 00 FF FF)で終わる。戻り値は出力の大きさ。 */
size_t zd_compress(ZDWork *w, const zbyte *buf, size_t dictLen, size_t len,
                   zbyte *out, int level, int header);

/* ------------------------------------------------------------------ */
/*  展開                                                                */
/* ------------------------------------------------------------------ */

typedef struct ZInflate ZInflate;

ZInflate *zi_new(void);
void      zi_free(ZInflate *z);
void      zi_reset(ZInflate *z);        /* 新しいストリームとして読み直す */

/* src[0..n) を入力に足し、ちょうど outLen バイトを out へ展開する。
 * 戻り値: 0 = 成功、-1 = 入力が足りない、-2 = データが壊れている。
 * -1 のとき、展開できた分は失われる(RFB では矩形の途中で足りなくなる
 * のは壊れているのと同じなので、呼び手は接続を切る)。
 * 余った入力は次の呼び出しへ持ち越す。 */
int zi_inflate(ZInflate *z, const zbyte *src, size_t n, zbyte *out, size_t outLen);

/* src[0..n) を入力に足し、展開できるだけ展開する(ZRLE のように、
 * 展開後の大きさが前もって分からないとき)。*out は realloc で広げる。
 * 戻り値は今回展開した大きさ、壊れていれば (size_t)-1。 */
size_t zi_inflate_avail(ZInflate *z, const zbyte *src, size_t n, zbyte **out, size_t *cap);

/* 出力の大きさを決めずに、入力を全部展開する(拡張クリップボード用)。
 * *out は malloc したもの。戻り値は大きさ、壊れていれば (size_t)-1。
 * maxOut を超えたら打ち切る。 */
size_t zi_inflate_all(const zbyte *src, size_t n, zbyte **out, size_t maxOut);

#endif
