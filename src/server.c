/* ==================================================================
 * server.c - 待ち受けと iiv の手順(iivproto.h)
 *
 *  接続ごとにスレッドを 2 本使う。
 *    読み手  つなぐときの手順と、相手からのメッセージ(返事、入力、クリップボード、ファイル)
 *    書き手  映像のスレッド(video.c)が配ったフレーム、カーソル、クリップボード、
 *            ファイルの中身を送る。何も無ければ hWake で眠る。
 *
 *  映像は押し出し式。映像のスレッドは server_video_wanted() で「誰かが見ているか、
 *  キーフレームが要るか、返事の無いフレームが最大いくつか」を聞き、符号化したら
 *  server_video_deliver() で全員の待ち行列へ入れる(フレームは参照の数で共有)。
 *  来たばかりの相手はキーフレームから受け取り始める。返事を 3 秒返さない相手は
 *  送る速さの判断から外す(ほかの相手を止めないため)。待ち行列があふれたら
 *  その相手だけ捨てて、次のキーフレームからやり直す。
 *
 *  認証はパスワード(auth.c)。パスワードが無いときは 127.0.0.1 でしか待ち受けない
 *  (main.c が止める)。同じ IP から 60 秒に 5 回失敗したら、その IP を 60 秒締め出す。
 * ================================================================== */

#include "iiv.h"
#include <wtsapi32.h>

volatile LONG g_clientCount;

static SOCKET         g_listen[2] = { INVALID_SOCKET, INVALID_SOCKET };
static int            g_nlisten;
static HANDLE         g_acceptThread;
static volatile LONG  g_stopping;
static volatile LONG  g_nextId;
static CRITICAL_SECTION g_failCs;
static BOOL           g_inited;
static LARGE_INTEGER  g_qpf;

typedef struct { char ip[64]; int fails; DWORD first, blockedUntil; } FailRec;
static FailRec g_fails[32];

/* 送るのを待っているファイルのメッセージ(書き手が送る) */
typedef struct FxMsg { struct FxMsg *next; int len; BYTE data[1]; } FxMsg;

/* ------------------------------------------------------------------ */
/*  送受信                                                              */
/* ------------------------------------------------------------------ */

static BOOL rd(Client *c, void *buf, int n)
{
    char *p = (char *)buf;
    while (n > 0) {
        int r = recv(c->s, p, n, 0);
        if (r <= 0 || c->quit) return FALSE;
        p += r;
        n -= r;
    }
    return TRUE;
}

static BOOL send_raw(Client *c, const void *data, int len)
{
    const char *p = (const char *)data;
    while (len > 0) {
        int r;
        if (c->quit) return FALSE;
        r = send(c->s, p, len, 0);
        if (r <= 0) {
            InterlockedExchange(&c->quit, 1);
            return FALSE;
        }
        p += r;
        len -= r;
        InterlockedAdd64(&c->bytesSent, r);
    }
    return TRUE;
}

/* [u32 長さ][u8 種類][a][b] を 1 つのメッセージとして送る */
static BOOL send_msg(Client *c, int type, const void *a, int na, const void *b, int nb)
{
    BYTE h[5];
    BOOL ok;
    unsigned len = 1u + (unsigned)na + (unsigned)nb;
    memcpy(h, &len, 4);
    h[4] = (BYTE)type;
    EnterCriticalSection(&c->sendLock);
    ok = send_raw(c, h, 5) && (!na || send_raw(c, a, na)) && (!nb || send_raw(c, b, nb));
    LeaveCriticalSection(&c->sendLock);
    return ok;
}

static void kill_client(Client *c)
{
    InterlockedExchange(&c->quit, 1);
    shutdown(c->s, SD_BOTH);
    SetEvent(c->hWake);
}

static LONG64 qpc_now(void)
{
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return q.QuadPart;
}

/* ------------------------------------------------------------------ */
/*  締め出し                                                            */
/* ------------------------------------------------------------------ */

static FailRec *fail_rec(const char *ip, BOOL create)
{
    int i, oldest = 0;
    for (i = 0; i < 32; i++) if (!strcmp(g_fails[i].ip, ip)) return &g_fails[i];
    if (!create) return NULL;
    for (i = 1; i < 32; i++) if (g_fails[i].first < g_fails[oldest].first) oldest = i;
    ZeroMemory(&g_fails[oldest], sizeof(FailRec));
    lstrcpynA(g_fails[oldest].ip, ip, sizeof(g_fails[oldest].ip));
    return &g_fails[oldest];
}

static BOOL is_blocked(const char *ip)
{
    FailRec *f;
    BOOL b = FALSE;
    EnterCriticalSection(&g_failCs);
    f = fail_rec(ip, FALSE);
    if (f && f->blockedUntil && (LONG)(f->blockedUntil - GetTickCount()) > 0) b = TRUE;
    LeaveCriticalSection(&g_failCs);
    return b;
}

static void record_fail(const char *ip, BOOL failed)
{
    FailRec *f;
    DWORD now = GetTickCount();
    EnterCriticalSection(&g_failCs);
    f = fail_rec(ip, failed);
    if (f && !failed) ZeroMemory(f, sizeof(*f));
    else if (f) {
        if (now - f->first > 60000) { f->first = now; f->fails = 0; }
        if (!f->fails) f->first = now;
        if (++f->fails >= 5) {
            f->blockedUntil = now + 60000;
            log_printfA("%s: 認証の失敗が続いたので 60 秒締め出す", ip);
        }
    }
    LeaveCriticalSection(&g_failCs);
}

/* ------------------------------------------------------------------ */
/*  つなぐときの手順                                                    */
/* ------------------------------------------------------------------ */

static BOOL send_welcome(Client *c, unsigned result)
{
    IivWelcome w;
    WCHAR  host[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD  hn = ARRAYSIZE(host);
    char  *name;
    int    nlen = 0;
    BOOL   ok;
    if (!GetComputerNameW(host, &hn)) lstrcpyW(host, L"?");
    name = utf16_to_utf8(host, &nlen);
    ZeroMemory(&w, sizeof(w));
    w.result = result;
    w.flags = (c->viewOnly ? IIV_WF_VIEWONLY : 0) | IIV_WF_FILES;
    w.nameLen = (unsigned short)nlen;
    ok = send_raw(c, &w, sizeof(w)) && send_raw(c, name ? name : "", nlen);
    free(name);
    return ok;
}

static BOOL handshake(Client *c, const char *ip)
{
    IivHello     hi;
    IivChallenge ch;
    BOOL         hasPw = g_cfg.pw.set || g_cfg.viewPw.set;

    if (!rd(c, &hi, sizeof(hi))) return FALSE;
    if (hi.magic != IIV_MAGIC) return FALSE;            /* iiv ではない(VNC のビューアなど)。黙って切る */
    ZeroMemory(&ch, sizeof(ch));
    ch.magic = IIV_MAGIC;
    ch.version = IIV_VERSION;
    ch.auth = hasPw ? IIV_AUTH_PASSWORD : IIV_AUTH_NONE;
    if (g_cfg.pw.set) { memcpy(ch.salt, g_cfg.pw.salt, 16); ch.iterations = g_cfg.pw.iter; }
    if (g_cfg.viewPw.set) { memcpy(ch.saltView, g_cfg.viewPw.salt, 16); ch.iterationsView = g_cfg.viewPw.iter; }
    if (!auth_random(ch.nonce, 32) || !send_raw(c, &ch, sizeof(ch))) return FALSE;

    if (hasPw) {
        BYTE proof[64];
        BOOL ok = FALSE;
        if (!rd(c, proof, 64)) return FALSE;
        if (is_blocked(ip)) {
            send_welcome(c, IIV_BLOCKED);
            return FALSE;
        }
        if (auth_check(&g_cfg.pw, ch.nonce, proof)) ok = TRUE;
        else if (auth_check(&g_cfg.viewPw, ch.nonce, proof + 32)) { ok = TRUE; c->viewOnly = TRUE; }
        record_fail(ip, !ok);
        if (!ok) {
            send_welcome(c, IIV_BAD_PASSWORD);
            log_printf(L"%s: パスワードが違う", c->addr);
            return FALSE;
        }
    }
    if (hi.version != IIV_VERSION) {
        send_welcome(c, IIV_BAD_VERSION);
        log_printf(L"%s: 版が違う(相手 %u、こちら %u)", c->addr, hi.version, IIV_VERSION);
        return FALSE;
    }
    if (!(hi.codecs & (1u << IIV_CODEC_H264))) {
        send_welcome(c, IIV_BAD_VERSION);
        log_printf(L"%s: H.264 を受け取れない相手", c->addr);
        return FALSE;
    }
    if (g_cfg.viewOnly) c->viewOnly = TRUE;
    c->fileXfer = (hi.flags & IIV_HF_FILES) != 0;
    if (!send_welcome(c, IIV_OK)) return FALSE;

    /* 一覧に入れ、キーフレームから受け取り始める */
    AcquireSRWLockExclusive(&g_scr.lock);
    c->wantKey = TRUE;
    c->cfgSent = -1;
    c->cursorVer = -1;
    c->cursorSent.x = c->cursorSent.y = -1;
    c->lastAckTick = GetTickCount();
    c->next = g_scr.clients;
    g_scr.clients = c;
    c->active = TRUE;
    ReleaseSRWLockExclusive(&g_scr.lock);
    video_kick();
    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  映像のスレッドとのやりとり                                          */
/* ------------------------------------------------------------------ */

BOOL server_video_wanted(BOOL *needKey, int *maxInflight, int *kbps)
{
    Client *c;
    BOOL    any = FALSE;
    DWORD   now = GetTickCount();
    *needKey = FALSE;
    *maxInflight = 0;
    *kbps = 0;
    AcquireSRWLockShared(&g_scr.lock);
    for (c = g_scr.clients; c; c = c->next) {
        int in;
        if (!c->active || c->quit) continue;
        any = TRUE;
        EnterCriticalSection(&c->cs);
        if (c->wantKey) *needKey = TRUE;
        in = c->started ? (int)(c->lastQueued - (UINT32)c->lastAcked) : 0;
        if (c->kbpsWanted > 0 && (!*kbps || c->kbpsWanted < *kbps)) *kbps = c->kbpsWanted;
        LeaveCriticalSection(&c->cs);
        /* 返事を 3 秒返さない相手と、詰まってやり直す相手は待たない */
        if (c->resync || (in > 0 && now - c->lastAckTick > 3000)) continue;
        if (in > *maxInflight) *maxInflight = in;
    }
    ReleaseSRWLockShared(&g_scr.lock);
    return any;
}

void server_video_deliver(VFrame *f)
{
    Client *c;
    AcquireSRWLockShared(&g_scr.lock);
    for (c = g_scr.clients; c; c = c->next) {
        BOOL wake = FALSE;
        if (!c->active || c->quit) continue;
        EnterCriticalSection(&c->cs);
        if (!c->started && f->key && !c->resync) {
            c->started = TRUE;
            c->wantKey = FALSE;
            InterlockedExchange(&c->lastAcked, (LONG)(f->no - 1));
        }
        if (c->started) {
            if (c->vqCount >= VQ_MAX) {
                /* 受け取れていない: 捨てる。送り手が追いついたら(相手が受け取り始めたら)キーフレームを求める。
                   すぐ求めると、受け取らない相手のためにキーフレームを作り続け、ほかの相手まで遅くなる */
                while (c->vqCount) {
                    vframe_release(c->vq[c->vqHead]);
                    c->vqHead = (c->vqHead + 1) % VQ_MAX;
                    c->vqCount--;
                }
                c->started = FALSE;
                c->wantKey = FALSE;
                c->resync = TRUE;
                log_printf(L"%s: 映像が詰まったので捨てた。追いついたらキーフレームからやり直す", c->addr);
            } else {
                InterlockedIncrement(&f->ref);
                c->vq[(c->vqHead + c->vqCount) % VQ_MAX] = f;
                c->vqCount++;
                c->lastQueued = f->no;
                c->sentPresent[f->no % 64] = f->presentQpc;
                wake = TRUE;
            }
        }
        LeaveCriticalSection(&c->cs);
        if (wake) SetEvent(c->hWake);
    }
    ReleaseSRWLockShared(&g_scr.lock);
}

void server_cursor_changed(void)
{
    Client *c;
    AcquireSRWLockShared(&g_scr.lock);
    for (c = g_scr.clients; c; c = c->next) if (c->active) SetEvent(c->hWake);
    ReleaseSRWLockShared(&g_scr.lock);
}

/* ------------------------------------------------------------------ */
/*  相手からのメッセージ                                                */
/* ------------------------------------------------------------------ */

static BOOL fx_ok(Client *c) { return c->fileXfer && !c->viewOnly; }

static void stat_ack(Client *c, const IivAck *a)
{
    LONG64 pres, now = qpc_now();
    DWORD  t = GetTickCount();
    EnterCriticalSection(&c->cs);
    pres = c->sentPresent[a->frame % 64];
    if (pres && now > pres) c->statLatTicks += now - pres;
    c->statDecodeUs += a->decodeUs;
    c->statAcks++;
    if (!c->statSince) c->statSince = t;
    if (t - c->statSince >= 5000 && c->statAcks) {
        double sec = (t - c->statSince) / 1000.0;
        LONG64 bytes = c->bytesSent - c->statBytes0;
        log_printf(L"%s: %.1f フレーム/秒、%.1f Mbps、画面に出てから相手が表示して返事が届くまで 平均 %.1fms"
                   L"(相手の受信から表示まで %.1fms、往復 %.1fms)", c->addr, c->statAcks / sec, bytes * 8 / sec / 1e6,
                   c->statLatTicks * 1000.0 / (double)g_qpf.QuadPart / c->statAcks, c->statDecodeUs / 1000.0 / c->statAcks, c->rttMs);
        c->statLatTicks = c->statDecodeUs = 0;
        c->statAcks = 0;
        c->statBytes0 = c->bytesSent;
        c->statSince = t;
    }
    LeaveCriticalSection(&c->cs);
}

static BOOL message_loop(Client *c)
{
    BYTE *buf = NULL;
    unsigned cap = 0;
    BOOL ok = FALSE;
    for (;;) {
        unsigned len;
        BYTE type, *p;
        unsigned n;
        if (!rd(c, &len, 4) || len < 1 || len > IIV_MAX_MSG) break;
        if (len > cap) {
            BYTE *nb = (BYTE *)realloc(buf, len);
            if (!nb) break;
            buf = nb;
            cap = len;
        }
        if (!rd(c, buf, (int)len)) break;
        type = buf[0];
        p = buf + 1;
        n = len - 1;
        switch (type) {
        case IIV_C_ACK:
            if (n >= sizeof(IivAck)) {
                IivAck a;
                memcpy(&a, p, sizeof(a));
                if ((LONG)(a.frame - (UINT32)c->lastAcked) > 0) InterlockedExchange(&c->lastAcked, (LONG)a.frame);
                c->lastAckTick = GetTickCount();
                stat_ack(c, &a);
                video_kick();
            }
            break;
        case IIV_C_KEY:
            if (n >= sizeof(IivKey) && !c->viewOnly) {
                IivKey k;
                memcpy(&k, p, sizeof(k));
                input_key(c, &k);
            }
            break;
        case IIV_C_MOUSE:
            if (n >= sizeof(IivMouse) && !c->viewOnly) {
                IivMouse m;
                memcpy(&m, p, sizeof(m));
                AcquireSRWLockExclusive(&g_scr.lock);
                c->pointerFromClient.x = m.x;
                c->pointerFromClient.y = m.y;
                ReleaseSRWLockExclusive(&g_scr.lock);
                input_mouse(c, &m);
            }
            break;
        case IIV_C_CLIPBOARD:
            if (!c->viewOnly && n <= (16u << 20)) {
                WCHAR *w = utf8_to_utf16((const char *)p, (int)n);
                if (w && !PostMessageW(g_mainWnd, WM_APP_SETCLIP, 0, (LPARAM)w)) free(w);
            }
            break;
        case IIV_C_FX:
            if (n >= 1) {
                switch (p[0]) {
                case FX_FILES: if (fx_ok(c)) fx_offer_received(c->id, p + 1, (int)n - 1); break;
                case FX_READ:  if (fx_ok(c)) fx_request(c->id, p + 1, (int)n - 1); break;
                case FX_DATA:  fx_deliver(c->id, p + 1, (int)n - 1); break;
                }
            }
            break;
        case IIV_C_KEYFRAME:
            EnterCriticalSection(&c->cs);
            c->wantKey = TRUE;
            LeaveCriticalSection(&c->cs);
            log_printf(L"%s: キーフレームを求められた", c->addr);
            video_kick();
            break;
        case IIV_C_SAS:
            if (!c->viewOnly) input_sas(c);
            break;
        case IIV_C_PONG:
            if (n >= sizeof(IivPing)) {
                IivPing pg;
                memcpy(&pg, p, sizeof(pg));
                if (pg.qpc == c->pingQpc) c->rttMs = (qpc_now() - pg.qpc) * 1000.0 / (double)g_qpf.QuadPart;
            }
            break;
        case IIV_C_SETTINGS:
            if (n >= sizeof(IivSettings)) {
                IivSettings st;
                memcpy(&st, p, sizeof(st));
                EnterCriticalSection(&c->cs);
                c->kbpsWanted = st.kbps > 1000000 ? 1000000 : (int)st.kbps;
                LeaveCriticalSection(&c->cs);
                log_printf(L"%s: ビットレート %u kbps を求められた", c->addr, st.kbps);
                video_kick();
            }
            break;
        default:
            break;                          /* 知らないものは読み飛ばす(新しい版の相手のため) */
        }
    }
    ok = TRUE;
    free(buf);
    return ok;
}

/* ------------------------------------------------------------------ */
/*  書き手                                                              */
/* ------------------------------------------------------------------ */

BOOL fx_host_send(int conn, int sub, const BYTE *p, int n)
{
    Client *c;
    BOOL    found = FALSE;
    FxMsg  *m = (FxMsg *)malloc(sizeof(FxMsg) + 6 + (size_t)n);
    unsigned len = 2u + (unsigned)n;
    if (!m) return FALSE;
    m->next = NULL;
    m->len = 6 + n;
    memcpy(m->data, &len, 4);
    m->data[4] = IIV_S_FX;
    m->data[5] = (BYTE)sub;
    memcpy(m->data + 6, p, (size_t)n);
    AcquireSRWLockShared(&g_scr.lock);
    for (c = g_scr.clients; c; c = c->next) {
        if (c->id != conn || !c->active || c->quit) continue;
        EnterCriticalSection(&c->cs);
        if (c->fileXfer) {
            if (c->fxTail) c->fxTail->next = m; else c->fxHead = m;
            c->fxTail = m;
            found = TRUE;
        }
        LeaveCriticalSection(&c->cs);
        if (found) SetEvent(c->hWake);
        break;
    }
    ReleaseSRWLockShared(&g_scr.lock);
    if (!found) free(m);
    return found;
}

static void send_fx_and_clip(Client *c)
{
    FxMsg *q, *nx;
    char  *text;
    int    tlen;
    EnterCriticalSection(&c->cs);
    q = c->fxHead;
    c->fxHead = c->fxTail = NULL;
    text = c->clipOut;
    tlen = c->clipOutLen;
    c->clipOut = NULL;
    LeaveCriticalSection(&c->cs);
    for (; q; q = nx) {
        nx = q->next;
        if (!c->quit) {
            EnterCriticalSection(&c->sendLock);
            send_raw(c, q->data, q->len);
            LeaveCriticalSection(&c->sendLock);
        }
        free(q);
    }
    if (text) {
        send_msg(c, IIV_S_CLIPBOARD, text, tlen, NULL, 0);
        free(text);
    }
}

static void send_config(Client *c)
{
    IivVideoConfig vc;
    ZeroMemory(&vc, sizeof(vc));
    AcquireSRWLockShared(&g_scr.lock);
    vc.codec = IIV_CODEC_H264;
    vc.videoW = (unsigned short)g_scr.vidW;
    vc.videoH = (unsigned short)g_scr.vidH;
    vc.deskX = g_scr.deskX;
    vc.deskY = g_scr.deskY;
    vc.deskW = (unsigned short)g_scr.deskW;
    vc.deskH = (unsigned short)g_scr.deskH;
    c->cfgSent = g_scr.cfgVer;
    ReleaseSRWLockShared(&g_scr.lock);
    vc.qpcFreq = g_qpf.QuadPart;
    send_msg(c, IIV_S_VIDEO_CONFIG, &vc, sizeof(vc), NULL, 0);
    log_printf(L"%s: 映像 %ux%u(範囲 %ux%u)を知らせた", c->addr, vc.videoW, vc.videoH, vc.deskW, vc.deskH);
}

static void send_cursor(Client *c)
{
    IivCursorShape sh;
    IivCursorPos   pos;
    BYTE *pix = NULL;
    BOOL  shape = FALSE, move = FALSE;
    int   n = 0;

    AcquireSRWLockShared(&g_scr.lock);
    if (c->cursorVer != g_scr.curVer && g_scr.curPix) {
        int mb = (g_scr.curW + 7) / 8;
        BOOL vis = g_scr.curVisible || g_cfg.showCursor;        /* 隠れていても見せる(マウスが無い PC 向け) */
        c->cursorVer = g_scr.curVer;
        sh.w = (unsigned short)g_scr.curW; sh.h = (unsigned short)g_scr.curH;
        sh.hotX = (unsigned short)g_scr.curHotX; sh.hotY = (unsigned short)g_scr.curHotY;
        sh.visible = (unsigned char)vis;
        n = g_scr.curW * g_scr.curH * 4 + mb * g_scr.curH;
        pix = (BYTE *)malloc((size_t)n);
        if (pix) {
            memcpy(pix, g_scr.curPix, (size_t)g_scr.curW * g_scr.curH * 4);
            memcpy(pix + g_scr.curW * g_scr.curH * 4, g_scr.curMask, (size_t)mb * g_scr.curH);
            shape = TRUE;
        }
    }
    if (g_scr.curX != c->cursorSent.x || g_scr.curY != c->cursorSent.y || g_scr.curVisible != c->cursorVisSent) {
        pos.x = g_scr.curX;
        pos.y = g_scr.curY;
        pos.visible = (unsigned char)(g_scr.curVisible || g_cfg.showCursor);
        c->cursorSent.x = pos.x;
        c->cursorSent.y = pos.y;
        c->cursorVisSent = g_scr.curVisible;
        /* この相手が自分で動かした位置なら知らせなくてよい */
        if (pos.x != c->pointerFromClient.x || pos.y != c->pointerFromClient.y) move = TRUE;
    }
    ReleaseSRWLockShared(&g_scr.lock);
    if (shape) send_msg(c, IIV_S_CURSOR_SHAPE, &sh, sizeof(sh), pix, n);
    free(pix);
    if (move) send_msg(c, IIV_S_CURSOR_POS, &pos, sizeof(pos), NULL, 0);
}

static DWORD WINAPI writer_thread(void *arg)
{
    Client *c = (Client *)arg;
    DWORD   lastPing = 0;

    if (g_cfg.fxOffer && fx_ok(c)) PostMessageW(g_mainWnd, WM_APP_FXOFFER, (WPARAM)c->id, 0);
    while (!c->quit) {
        VFrame *f;
        int     cfgVer;

        WaitForSingleObject(c->hWake, 500);
        if (c->quit) break;
        send_fx_and_clip(c);
        AcquireSRWLockShared(&g_scr.lock);
        cfgVer = g_scr.cfgVer;
        ReleaseSRWLockShared(&g_scr.lock);
        if (cfgVer > 0 && c->cfgSent < 0) send_config(c);       /* 窓の大きさを早めに決められるように */
        send_cursor(c);
        for (;;) {
            IivVideoHead vh;
            EnterCriticalSection(&c->cs);
            f = NULL;
            if (c->vqCount) {
                f = c->vq[c->vqHead];
                c->vqHead = (c->vqHead + 1) % VQ_MAX;
                c->vqCount--;
            }
            if (!f && c->resync) {
                /* 詰まった分を送り終えた(相手が受け取っている): キーフレームからやり直す */
                c->resync = FALSE;
                c->wantKey = TRUE;
                c->lastAckTick = GetTickCount();
                video_kick();
            }
            LeaveCriticalSection(&c->cs);
            if (!f) break;
            if (f->cfgVer != c->cfgSent) send_config(c);
            vh.frame = f->no;
            vh.flags = f->key ? IIV_VF_KEY : 0;
            vh.presentQpc = f->presentQpc;
            vh.sendQpc = qpc_now();
            send_msg(c, IIV_S_VIDEO, &vh, sizeof(vh), f->data, f->len);
            InterlockedIncrement(&c->frames);
            vframe_release(f);
            if (c->quit) break;
        }
        if (GetTickCount() - lastPing >= 2000) {
            IivPing pg;
            lastPing = GetTickCount();
            pg.qpc = c->pingQpc = qpc_now();
            send_msg(c, IIV_S_PING, &pg, sizeof(pg), NULL, 0);
        }
    }
    InterlockedExchange(&c->quit, 1);
    shutdown(c->s, SD_BOTH);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  接続 1 本                                                           */
/* ------------------------------------------------------------------ */

static DWORD WINAPI client_thread(void *arg)
{
    Client *c = (Client *)arg;
    char    ip[64];
    WideCharToMultiByte(CP_UTF8, 0, c->addr, -1, ip, sizeof(ip), NULL, NULL);
    {
        char *colon = strrchr(ip, ':');
        if (colon && strchr(ip, ']')) { char *br = strchr(ip, ']'); *br = 0; memmove(ip, ip + 1, strlen(ip)); }
        else if (colon && strchr(ip, '.') && colon == strchr(ip, ':')) *colon = 0;
    }

    if (handshake(c, ip)) {
        InterlockedIncrement(&g_clientCount);
        PostMessageW(g_mainWnd, WM_APP_CLIENTS, 0, 0);
        log_printf(L"%s: 接続した%s", c->addr, c->viewOnly ? L"(見るだけ)" : L"");
        if (g_cfg.notify) app_notify(L"%s から接続されました%s", c->addr, c->viewOnly ? L"(見るだけ)" : L"");
        c->thrWrite = CreateThread(NULL, 0, writer_thread, c, 0, NULL);
        message_loop(c);
        kill_client(c);
        if (c->thrWrite) { WaitForSingleObject(c->thrWrite, INFINITE); CloseHandle(c->thrWrite); }
        input_release_all(c);
        InterlockedDecrement(&g_clientCount);
        log_printf(L"%s: 切れた(送ったバイト %I64d、フレーム %ld)", c->addr, c->bytesSent, c->frames);
        if (g_cfg.notify && !g_stopping) app_notify(L"%s との接続が切れました", c->addr);
    }

    /* 一覧から外す */
    AcquireSRWLockExclusive(&g_scr.lock);
    {
        Client **pp;
        for (pp = &g_scr.clients; *pp; pp = &(*pp)->next)
            if (*pp == c) { *pp = c->next; break; }
    }
    ReleaseSRWLockExclusive(&g_scr.lock);
    PostMessageW(g_mainWnd, WM_APP_CLIENTS, 0, 0);
    video_kick();

    closesocket(c->s);
    CloseHandle(c->hWake);
    while (c->vqCount) {
        vframe_release(c->vq[c->vqHead]);
        c->vqHead = (c->vqHead + 1) % VQ_MAX;
        c->vqCount--;
    }
    DeleteCriticalSection(&c->sendLock);
    DeleteCriticalSection(&c->cs);
    free(c->clipOut);
    fx_conn_closed(c->id);
    {
        FxMsg *q, *n;
        for (q = c->fxHead; q; q = n) { n = q->next; free(q); }
    }
    if (c->thrRead) CloseHandle(c->thrRead);
    free(c);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  待ち受け                                                            */
/* ------------------------------------------------------------------ */

static DWORD WINAPI accept_thread(void *arg)
{
    (void)arg;
    while (!g_stopping) {
        fd_set fs;
        struct timeval tv = { 0, 500000 };
        int i;
        FD_ZERO(&fs);
        for (i = 0; i < g_nlisten; i++) FD_SET(g_listen[i], &fs);
        if (select(0, &fs, NULL, NULL, &tv) <= 0) continue;
        for (i = 0; i < g_nlisten; i++) {
            struct sockaddr_storage sa;
            int    slen = sizeof(sa), one = 1, big = 4 << 20;
            SOCKET s;
            Client *c;
            DWORD  alen;
            if (!FD_ISSET(g_listen[i], &fs)) continue;
            s = accept(g_listen[i], (struct sockaddr *)&sa, &slen);
            if (s == INVALID_SOCKET) continue;
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
            setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char *)&big, sizeof(big));
            c = (Client *)calloc(1, sizeof(Client));
            if (!c) { closesocket(s); continue; }
            c->s = s;
            c->id = InterlockedIncrement(&g_nextId);
            alen = ARRAYSIZE(c->addr);
            if (WSAAddressToStringW((struct sockaddr *)&sa, (DWORD)slen, NULL, c->addr, &alen)) lstrcpyW(c->addr, L"?");
            /* ::ffff:1.2.3.4 は 1.2.3.4 と書く */
            if (!wcsncmp(c->addr, L"[::ffff:", 8)) {
                WCHAR *e = wcschr(c->addr, L']');
                if (e) { WCHAR t[80]; *e = 0; wsprintfW(t, L"%s%s", c->addr + 8, e + 1); lstrcpyW(c->addr, t); }
            }
            c->hWake = CreateEventW(NULL, FALSE, FALSE, NULL);
            InitializeCriticalSection(&c->sendLock);
            InitializeCriticalSection(&c->cs);
            c->thrRead = CreateThread(NULL, 0, client_thread, c, 0, NULL);
            if (!c->thrRead) {
                closesocket(s);
                CloseHandle(c->hWake);
                free(c);
            }
        }
    }
    return 0;
}

static SOCKET open_listen(int family, const struct sockaddr *sa, int salen)
{
    SOCKET s = socket(family, SOCK_STREAM, IPPROTO_TCP);
    int    zero = 0, one = 1;
    if (s == INVALID_SOCKET) return s;
    if (family == AF_INET6) setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&zero, sizeof(zero));
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&one, sizeof(one));
    if (bind(s, sa, salen) || listen(s, 8)) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

BOOL server_start(void)
{
    if (!g_inited) {
        WSADATA wd;
        WSAStartup(MAKEWORD(2, 2), &wd);
        InitializeCriticalSection(&g_failCs);
        QueryPerformanceFrequency(&g_qpf);
        g_inited = TRUE;
    }
    g_stopping = 0;
    g_nlisten = 0;
    if (!g_cfg.listen[0]) {
        struct sockaddr_in6 a6;
        struct sockaddr_in  a4;
        ZeroMemory(&a6, sizeof(a6));
        a6.sin6_family = AF_INET6;
        a6.sin6_port = htons((u_short)g_cfg.port);
        g_listen[0] = open_listen(AF_INET6, (struct sockaddr *)&a6, sizeof(a6));
        if (g_listen[0] != INVALID_SOCKET) g_nlisten = 1;
        else {
            ZeroMemory(&a4, sizeof(a4));
            a4.sin_family = AF_INET;
            a4.sin_port = htons((u_short)g_cfg.port);
            g_listen[0] = open_listen(AF_INET, (struct sockaddr *)&a4, sizeof(a4));
            if (g_listen[0] != INVALID_SOCKET) g_nlisten = 1;
        }
    } else {
        ADDRINFOW hints, *res = NULL, *ai;
        WCHAR port[16];
        ZeroMemory(&hints, sizeof(hints));
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE | AI_NUMERICHOST;
        wsprintfW(port, L"%d", g_cfg.port);
        if (!GetAddrInfoW(g_cfg.listen, port, &hints, &res)) {
            for (ai = res; ai && g_nlisten < 2; ai = ai->ai_next) {
                SOCKET s = open_listen(ai->ai_family, ai->ai_addr, (int)ai->ai_addrlen);
                if (s != INVALID_SOCKET) g_listen[g_nlisten++] = s;
            }
            FreeAddrInfoW(res);
        }
    }
    if (!g_nlisten) {
        log_printf(L"ポート %d で待ち受けられない (%d)", g_cfg.port, WSAGetLastError());
        return FALSE;
    }
    g_acceptThread = CreateThread(NULL, 0, accept_thread, NULL, 0, NULL);
    log_printf(L"ポート %d で待ち受ける(%s)", g_cfg.port, g_cfg.listen[0] ? g_cfg.listen : L"すべて");
    return TRUE;
}

void server_disconnect_all(void)
{
    Client *c;
    AcquireSRWLockShared(&g_scr.lock);
    for (c = g_scr.clients; c; c = c->next) kill_client(c);
    ReleaseSRWLockShared(&g_scr.lock);
}

void server_disconnect(int id)
{
    Client *c;
    AcquireSRWLockShared(&g_scr.lock);
    for (c = g_scr.clients; c; c = c->next) if (c->id == id) kill_client(c);
    ReleaseSRWLockShared(&g_scr.lock);
}

void server_stop(void)
{
    int i;
    DWORD t0;
    InterlockedExchange(&g_stopping, 1);
    for (i = 0; i < g_nlisten; i++) closesocket(g_listen[i]);
    if (g_acceptThread) {
        WaitForSingleObject(g_acceptThread, 2000);
        CloseHandle(g_acceptThread);
        g_acceptThread = NULL;
    }
    g_nlisten = 0;
    server_disconnect_all();
    t0 = GetTickCount();
    for (;;) {
        BOOL any;
        AcquireSRWLockShared(&g_scr.lock);
        any = g_scr.clients != NULL;
        ReleaseSRWLockShared(&g_scr.lock);
        if (!any || GetTickCount() - t0 > 3000) break;
        Sleep(20);
    }
}

int server_list(WCHAR *buf, int cap)
{
    Client *c;
    int n = 0, used = 0;
    buf[0] = 0;
    AcquireSRWLockShared(&g_scr.lock);
    for (c = g_scr.clients; c; c = c->next) {
        int k;
        if (!c->active) continue;
        k = _snwprintf(buf + used, (size_t)(cap - used), L"%s%s%s", n ? L"\n" : L"", c->addr, c->viewOnly ? L"(見るだけ)" : L"");
        if (k < 0) break;
        used += k;
        n++;
    }
    ReleaseSRWLockShared(&g_scr.lock);
    return n;
}

/* ------------------------------------------------------------------ */
/*  ファイルとクリップボード                                            */
/* ------------------------------------------------------------------ */

static int fx_targets(int *ids, int cap, int only)
{
    Client *c;
    int     n = 0;
    AcquireSRWLockShared(&g_scr.lock);
    for (c = g_scr.clients; c && n < cap; c = c->next) {
        if (!c->active || c->quit || (only && c->id != only)) continue;
        if (fx_ok(c)) ids[n++] = c->id;
    }
    ReleaseSRWLockShared(&g_scr.lock);
    return n;
}

static void fx_offer_to(HDROP hd, int only)
{
    int   ids[8], n = fx_targets(ids, 8, only), i, len = 0;
    BYTE *out = NULL;
    if (!n || !fx_make_offer(ids, n, hd, &out, &len)) return;
    for (i = 0; i < n; i++) fx_host_send(ids[i], FX_FILES, out, len);
    HeapFree(GetProcessHeap(), 0, out);
}

void server_files_changed(HDROP hd)
{
    fx_offer_to(hd, 0);
}

void server_files_changed_paths(const WCHAR *paths)
{
    int   ids[8], n = fx_targets(ids, 8, 0), i, len = 0;
    BYTE *out = NULL;
    if (!n || !fx_make_offer_paths(ids, n, paths, &out, &len)) return;
    for (i = 0; i < n; i++) fx_host_send(ids[i], FX_FILES, out, len);
    HeapFree(GetProcessHeap(), 0, out);
}

/* サービスの分身は SYSTEM。ファイルは、コンソールにログインしている利用者として読む */
HANDLE fx_host_user_token(void)
{
    HANDLE t = NULL;
    if (g_runMode != RUN_AGENT) return NULL;
    if (!WTSQueryUserToken(WTSGetActiveConsoleSessionId(), &t)) return NULL;
    return t;
}

void server_fx_offer_current(int id)
{
    int i;
    if (!IsClipboardFormatAvailable(CF_HDROP)) { log_printf(L"[fxoffer] クリップボードにファイルが無い"); return; }
    for (i = 0; i < 10 && !OpenClipboard(g_mainWnd); i++) Sleep(20);
    if (i == 10) return;
    {
        HDROP hd = (HDROP)GetClipboardData(CF_HDROP);
        if (hd) fx_offer_to(hd, id);
    }
    CloseClipboard();
}

void server_clipboard_changed(const char *utf8, int len)
{
    Client *c;
    AcquireSRWLockShared(&g_scr.lock);
    for (c = g_scr.clients; c; c = c->next) {
        if (!c->active) continue;
        EnterCriticalSection(&c->cs);
        free(c->clipOut);
        c->clipOut = (char *)malloc((size_t)len + 1);
        if (c->clipOut) { memcpy(c->clipOut, utf8, (size_t)len); c->clipOut[len] = 0; c->clipOutLen = len; }
        LeaveCriticalSection(&c->cs);
        SetEvent(c->hWake);
    }
    ReleaseSRWLockShared(&g_scr.lock);
}
