/* ==================================================================
 * video.c - 画面の取り込みから符号化までのスレッド、カーソル
 *
 *  1 本のスレッドで、取り込み → GPU の上で 1 枚の絵にまとめる → NV12 に変える
 *  (D3D11 の VideoProcessor。4096 を超える幅などはここで縮める) → H.264(venc.c)
 *  → 各接続へ配る(server.c)、を回す。絵は GPU から降ろさない(CPU のエンコーダの
 *  ときだけ NV12 を読み出す)。
 *
 *  取り込みは 3 通り。
 *   DXGI   Desktop Duplication。同じ GPU につながった画面を 1 枚の BGRA の
 *          テクスチャ(合成)へ GPU で写す。画面に出た時刻(LastPresentTime)を
 *          フレームに付けて送る(同じ PC での遅れの計測に使う)。
 *   GDI    DXGI が使えないとき。BitBlt して前の絵と比べ、変わっていれば送る。
 *   検証用 -testsrc。合成した絵を動かす(利用者の画面を写さない)。
 *
 *  押し出し式: 変化があれば、相手が求めなくても符号化して送る。ただし返事(表示した
 *  という知らせ)の無いフレームが IIV_MAX_INFLIGHT を超えた相手がいれば待つ。待つ間の
 *  変化は合成の絵に積もり、次のフレームにまとまる。新しい相手が来たらキーフレームを送る。
 *  相手がいなければ何もしない(3 秒たったら取り込みも閉じる)。
 * ================================================================== */

#include "iiv.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_5.h>
#include <mmsystem.h>

#pragma comment(lib, "winmm.lib")

Screen g_scr;

enum { MODE_NONE, MODE_DXGI, MODE_GDI, MODE_TEST };

typedef struct Dup {
    IDXGIOutputDuplication *dup;
    RECT                    desk;       /* 仮想画面の座標 */
    int                     ox, oy;     /* 合成の中の位置 */
    int                     w, h;
} Dup;

#define MAX_DUP 8
#define NNV     4                       /* NV12 のテクスチャの数(順に使う) */

static int            g_mode;
static Dup            g_dups[MAX_DUP];
static int            g_ndup;
static int            g_curOwner = -1;
static HANDLE         g_thread;
static volatile LONG  g_quit, g_reset;
static BYTE          *g_shape;
static UINT           g_shapeCap;

/* GPU */
static ID3D11Device         *g_dev;
static ID3D11DeviceContext  *g_ctx;
static ID3D11Texture2D      *g_comp;            /* 合成した BGRA */
static ID3D11Texture2D      *g_nv12[NNV];
static ID3D11VideoDevice    *g_vdev;
static ID3D11VideoContext   *g_vctx;
static ID3D11VideoProcessorEnumerator *g_venum;
static ID3D11VideoProcessor *g_vp;
static ID3D11VideoProcessorInputView  *g_inView;
static ID3D11VideoProcessorOutputView *g_outView[NNV];
static int            g_nvNext;
static VEnc          *g_enc;
static int            g_encKbps;
static WCHAR          g_encName[96];

/* 送る状態 */
static BOOL           g_dirty, g_forceKey;
static LONG64         g_presentQpc;
static UINT32         g_frameNo;
static LONG64         g_pts;
static int            g_keyMiss;
static DWORD          g_lastEncode;
static DWORD          g_lastMove;       /* 最後に変化を送った時刻 */
static BOOL           g_refined;        /* 止まってから、くっきりした絵を送った */
/* 統計 */
static int            g_statFrames;
static LONG64         g_statBytes, g_statEncTicks;
static DWORD          g_statSince;

/* GDI */
static HDC     g_memDC;
static HBITMAP g_dib, g_oldBmp;
static BYTE   *g_dibBits, *g_gdiPrev;
static HCURSOR g_lastCursor;
static DWORD   g_lastGdi;

/* 検証用 */
static BYTE   *g_test;
static HDC     g_testDC;
static HBITMAP g_testBmp, g_testOld;
static int     g_testFrame;

void vframe_release(VFrame *f)
{
    if (f && InterlockedDecrement(&f->ref) == 0) free(f);
}

static LONG64 qpc_now(void)
{
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return q.QuadPart;
}

/* ------------------------------------------------------------------ */
/*  範囲                                                                */
/* ------------------------------------------------------------------ */

typedef struct { int want; RECT r; BOOL found; } MonEnum;

static BOOL CALLBACK mon_proc(HMONITOR hm, HDC dc, LPRECT rc, LPARAM lp)
{
    MonEnum *me = (MonEnum *)lp;
    MONITORINFOEXW mi;
    (void)dc; (void)rc;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hm, (MONITORINFO *)&mi)) return TRUE;
    if (me->want) {
        const WCHAR *p = wcsstr(mi.szDevice, L"DISPLAY");
        if (p && _wtoi(p + 7) == me->want) { me->r = mi.rcMonitor; me->found = TRUE; }
    }
    return TRUE;
}

/* 選ばれた画面の範囲(仮想画面の座標) */
static void selected_rect(RECT *r)
{
    MonEnum me;
    ZeroMemory(&me, sizeof(me));
    me.want = g_cfg.display;
    if (me.want) EnumDisplayMonitors(NULL, NULL, mon_proc, (LPARAM)&me);
    if (me.found) { *r = me.r; return; }
    r->left   = GetSystemMetrics(SM_XVIRTUALSCREEN);
    r->top    = GetSystemMetrics(SM_YVIRTUALSCREEN);
    r->right  = r->left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    r->bottom = r->top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
}

/* 符号化する大きさ: 偶数、maxW x maxH に収める(縦横比はそのまま) */
static void video_size(int w, int h, int maxW, int maxH, int *vw, int *vh)
{
    double s = 1.0;
    if (w > maxW) s = (double)maxW / w;
    if (h * s > maxH) s = (double)maxH / h;
    *vw = ((int)(w * s + 0.5)) & ~1;
    *vh = ((int)(h * s + 0.5)) & ~1;
    if (*vw < 16) *vw = 16;
    if (*vh < 16) *vh = 16;
}

/* 範囲と大きさを決める。変わったら版を上げる(接続は次のフレームの前に知らせる) */
static void set_desk(int x, int y, int w, int h, int vw, int vh)
{
    AcquireSRWLockExclusive(&g_scr.lock);
    if (x != g_scr.deskX || y != g_scr.deskY || w != g_scr.deskW || h != g_scr.deskH || vw != g_scr.vidW || vh != g_scr.vidH) {
        g_scr.deskX = x; g_scr.deskY = y; g_scr.deskW = w; g_scr.deskH = h;
        g_scr.vidW = vw; g_scr.vidH = vh;
        g_scr.cfgVer++;
        log_printf(L"取り込む範囲: (%d,%d) %dx%d、符号化 %dx%d", x, y, w, h, vw, vh);
    }
    ReleaseSRWLockExclusive(&g_scr.lock);
}

/* ------------------------------------------------------------------ */
/*  カーソル                                                            */
/* ------------------------------------------------------------------ */

/* 形を決める。pix は BGRA、mask は 1 = 見える */
static void set_cursor_shape(int w, int h, int hx, int hy, const BYTE *pix, const BYTE *mask)
{
    int mb = (w + 7) / 8;
    BYTE *p = (BYTE *)malloc((size_t)w * h * 4 + 4), *m = (BYTE *)malloc((size_t)mb * h + 1);
    if (!p || !m) { free(p); free(m); return; }
    memcpy(p, pix, (size_t)w * h * 4);
    memcpy(m, mask, (size_t)mb * h);
    AcquireSRWLockExclusive(&g_scr.lock);
    free(g_scr.curPix);
    free(g_scr.curMask);
    g_scr.curPix  = p;
    g_scr.curMask = m;
    g_scr.curW = w; g_scr.curH = h;
    g_scr.curHotX = hx; g_scr.curHotY = hy;
    g_scr.curVer++;
    ReleaseSRWLockExclusive(&g_scr.lock);
    server_cursor_changed();
}

static void set_cursor_pos(int x, int y, BOOL visible)
{
    BOOL changed = FALSE, moved = FALSE;
    AcquireSRWLockExclusive(&g_scr.lock);
    if (x != g_scr.curX || y != g_scr.curY || visible != g_scr.curVisible) {
        moved = TRUE;
        g_scr.curX = x;
        g_scr.curY = y;
        if (visible != g_scr.curVisible) { g_scr.curVer++; changed = TRUE; }   /* 見え方が変わった = 形を送り直す */
        g_scr.curVisible = visible;
        g_scr.curPosVer++;
    }
    ReleaseSRWLockExclusive(&g_scr.lock);
    if (moved) server_cursor_changed();
    if (changed) log_printf(L"カーソル: Windows が%s", visible ? L"表示している" : L"隠している");
}

static void cursor_from_hcursor(HCURSOR hc);

/* 形がまだ一つも無ければ、標準の矢印にする */
static void cursor_ensure_shape(void)
{
    BOOL none;
    AcquireSRWLockShared(&g_scr.lock);
    none = g_scr.curPix == NULL;
    ReleaseSRWLockShared(&g_scr.lock);
    if (none) {
        cursor_from_hcursor(LoadCursorW(NULL, IDC_ARROW));
        log_printf(L"カーソル: 形が無いので標準の矢印を使う");
    }
}

/* Windows 10 はマウスがつながっていないとカーソルを隠したままにする(見えるのは
   マウスを挿したときだけ)。隠れている間も Windows が持っている形(矢印・I ビームなど)を
   追い、相手には見せる(showcursor=1)。隠れているので DXGI は形を渡してこない */
static void cursor_follow_hidden(void)
{
    CURSORINFO ci;
    if (!g_cfg.showCursor) return;
    ci.cbSize = sizeof(ci);
    if (GetCursorInfo(&ci) && ci.hCursor && ci.hCursor != g_lastCursor) {
        g_lastCursor = ci.hCursor;
        cursor_from_hcursor(ci.hCursor);
    }
    cursor_ensure_shape();
}

/* マウスの有無を記録に書く(マウスが無い PC でカーソルが見えないときの手がかり) */
static void log_mouse_devices(void)
{
    static BOOL done;
    RAWINPUTDEVICELIST *l;
    UINT n = 0, i, mice = 0;
    CURSORINFO ci;
    if (done) return;
    done = TRUE;
    GetRawInputDeviceList(NULL, &n, sizeof(RAWINPUTDEVICELIST));
    l = (RAWINPUTDEVICELIST *)calloc(n ? n : 1, sizeof(*l));
    if (l && (int)GetRawInputDeviceList(l, &n, sizeof(*l)) >= 0) {
        for (i = 0; i < n; i++) {
            WCHAR name[256];
            UINT  len = ARRAYSIZE(name);
            if (l[i].dwType != RIM_TYPEMOUSE) continue;
            mice++;
            name[0] = 0;
            GetRawInputDeviceInfoW(l[i].hDevice, RIDI_DEVICENAME, name, &len);
            log_printf(L"マウスの装置: %s", name);
        }
    }
    free(l);
    ci.cbSize = sizeof(ci);
    GetCursorInfo(&ci);
    log_printf(L"マウス: SM_MOUSEPRESENT %d、装置 %u 個。カーソルの flags %lu、形 %p", GetSystemMetrics(SM_MOUSEPRESENT), mice,
               (unsigned long)ci.flags, (void *)ci.hCursor);
}

/* DXGI の形を BGRA と見える印に直す */
static void cursor_from_dxgi(const DXGI_OUTDUPL_POINTER_SHAPE_INFO *si, const BYTE *buf)
{
    int w = (int)si->Width, h = (int)si->Height, x, y, mb;
    BYTE *pix, *mask;

    if (si->Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) h /= 2;
    if (w <= 0 || h <= 0 || w > 256 || h > 256) return;
    mb = (w + 7) / 8;
    pix  = (BYTE *)calloc((size_t)w * h, 4);
    mask = (BYTE *)calloc((size_t)mb * h, 1);
    if (!pix || !mask) { free(pix); free(mask); return; }

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            BYTE *d = pix + ((size_t)y * w + x) * 4;
            BOOL  vis = FALSE;
            if (si->Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
                int a = (buf[y * si->Pitch + x / 8] >> (7 - (x & 7))) & 1;
                int o = (buf[(y + h) * si->Pitch + x / 8] >> (7 - (x & 7))) & 1;
                if (!a) {               /* 置き換え: 黒か白 */
                    BYTE v = o ? 255 : 0;
                    d[0] = d[1] = d[2] = v;
                    vis = TRUE;
                } else if (o) {         /* 反転: 黒で近似する */
                    vis = TRUE;
                }
            } else {
                const BYTE *s = buf + y * si->Pitch + x * 4;
                if (si->Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) {
                    vis = s[3] >= 128;
                    d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
                } else {                /* MASKED_COLOR: A=0 は置き換え、A=FF は XOR */
                    d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
                    vis = s[3] == 0 || (s[0] | s[1] | s[2]);
                }
            }
            d[3] = 255;
            if (vis) mask[y * mb + x / 8] |= (BYTE)(0x80 >> (x & 7));
        }
    }
    set_cursor_shape(w, h, (int)si->HotSpot.x, (int)si->HotSpot.y, pix, mask);
    free(pix);
    free(mask);
}

/* GDI のカーソル(HCURSOR)から作る */
static void cursor_from_hcursor(HCURSOR hc)
{
    ICONINFO   ii;
    BITMAP     bm;
    BITMAPINFO bi;
    HDC        dc;
    int        w, h, x, y, mb;
    BYTE      *col = NULL, *msk = NULL, *pix = NULL, *mask = NULL;
    BOOL       hasAlpha = FALSE;

    if (!GetIconInfo(hc, &ii)) return;
    GetObjectW(ii.hbmMask, sizeof(bm), &bm);
    w = bm.bmWidth;
    h = ii.hbmColor ? bm.bmHeight : bm.bmHeight / 2;
    if (w <= 0 || h <= 0 || w > 256 || h > 256) goto done;
    dc = GetDC(NULL);
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -(ii.hbmColor ? h : h * 2);
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    msk = (BYTE *)calloc((size_t)w * h * 2, 4);
    GetDIBits(dc, ii.hbmMask, 0, (UINT)(ii.hbmColor ? h : h * 2), msk, &bi, DIB_RGB_COLORS);
    if (ii.hbmColor) {
        bi.bmiHeader.biHeight = -h;
        col = (BYTE *)calloc((size_t)w * h, 4);
        GetDIBits(dc, ii.hbmColor, 0, (UINT)h, col, &bi, DIB_RGB_COLORS);
        for (y = 0; y < w * h; y++) if (col[y * 4 + 3]) { hasAlpha = TRUE; break; }
    }
    ReleaseDC(NULL, dc);

    mb = (w + 7) / 8;
    pix  = (BYTE *)calloc((size_t)w * h, 4);
    mask = (BYTE *)calloc((size_t)mb * h, 1);
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            BYTE *d = pix + ((size_t)y * w + x) * 4;
            BOOL  vis;
            int   a = msk[((size_t)y * w + x) * 4] != 0;
            if (col) {
                const BYTE *s = col + ((size_t)y * w + x) * 4;
                d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
                vis = hasAlpha ? s[3] >= 128 : (!a || (s[0] | s[1] | s[2]));
            } else {
                int o = msk[((size_t)(y + h) * w + x) * 4] != 0;
                d[0] = d[1] = d[2] = (BYTE)(!a && o ? 255 : 0);
                vis = !a || o;
            }
            d[3] = 255;
            if (vis) mask[y * mb + x / 8] |= (BYTE)(0x80 >> (x & 7));
        }
    set_cursor_shape(w, h, (int)ii.xHotspot, (int)ii.yHotspot, pix, mask);
done:
    free(col); free(msk); free(pix); free(mask);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
}


/* ------------------------------------------------------------------ */
/*  検証用の絵を描く                                                    */
/* ------------------------------------------------------------------ */

#define TW 1920
#define TH 1080
/* スクロールする文字の欄 */
#define SX 40
#define SY 120
#define SW 900
#define SH 600

static void test_text_line(int y, int n)
{
    RECT r;
    WCHAR s[160];
    SetRect(&r, SX, y, SX + SW, y + 20);
    FillRect(g_testDC, &r, (HBRUSH)GetStockObject(WHITE_BRUSH));
    wsprintfW(s, L"%05d  iiv の検証用の行です。The quick brown fox jumps over the lazy dog. 漢字かなカナ 0123456789", n);
    SetBkMode(g_testDC, TRANSPARENT);
    SetTextColor(g_testDC, RGB((n * 37) & 127, 20, 60));
    TextOutW(g_testDC, SX + 6, y + 2, s, lstrlenW(s));
}

static void test_photo(int frame)
{
    int x, y;
    unsigned seed = 12345u + (unsigned)frame * 7919u;
    for (y = 0; y < 300; y++) {
        BYTE *row = g_test + ((size_t)(140 + y) * TW + 1000) * 4;
        for (x = 0; x < 400; x++) {
            int r, g, b, n;
            seed = seed * 1103515245u + 12345u;
            n = (int)((seed >> 16) & 31) - 16;
            r = (x * 255 / 400 + frame * 3) & 255;
            g = (y * 255 / 300) & 255;
            b = (128 + (int)(64 * ((x + y + frame * 5) % 97) / 97));
            r = max(0, min(255, r + n)); g = max(0, min(255, g + n)); b = max(0, min(255, b + n));
            row[x * 4 + 0] = (BYTE)b; row[x * 4 + 1] = (BYTE)g; row[x * 4 + 2] = (BYTE)r; row[x * 4 + 3] = 255;
        }
    }
}

/* 全面: 動くグラデーションに縞と雑音(写真や動画のように色が多い) */
static void test_video(int frame)
{
    int x, y;
    unsigned seed = 777u + (unsigned)frame * 2654435761u;
    for (y = 0; y < TH; y++) {
        BYTE *row = g_test + (size_t)y * TW * 4;
        for (x = 0; x < TW; x++) {
            int r, g, b, n;
            seed = seed * 1103515245u + 12345u;
            n = (int)((seed >> 16) & 15) - 8;
            r = ((x + frame * 4) * 255 / TW) & 255;
            g = ((y + frame * 2) * 255 / TH) & 255;
            b = (((x / 40 + y / 40 + frame / 3) & 1) ? 200 : 60);
            r = max(0, min(255, r + n)); g = max(0, min(255, g + n)); b = max(0, min(255, b + n));
            row[x * 4 + 0] = (BYTE)b; row[x * 4 + 1] = (BYTE)g; row[x * 4 + 2] = (BYTE)r; row[x * 4 + 3] = 255;
        }
    }
}

static void test_fix_alpha(const RECT *r)
{
    int x, y;
    for (y = r->top; y < r->bottom; y++)
        for (x = r->left; x < r->right; x++) g_test[((size_t)y * TW + x) * 4 + 3] = 255;
}


/* ------------------------------------------------------------------ */
/*  GPU の準備: デバイス、合成の絵、NV12 への変換、エンコーダ             */
/* ------------------------------------------------------------------ */

#define SAFE_RELEASE(p, T) do { if (p) { T##_Release(p); (p) = NULL; } } while (0)

static void pipeline_close(void)
{
    int i;
    if (g_enc) { venc_close(g_enc); g_enc = NULL; }
    for (i = 0; i < NNV; i++) {
        SAFE_RELEASE(g_outView[i], ID3D11VideoProcessorOutputView);
        SAFE_RELEASE(g_nv12[i], ID3D11Texture2D);
    }
    SAFE_RELEASE(g_inView, ID3D11VideoProcessorInputView);
    SAFE_RELEASE(g_vp, ID3D11VideoProcessor);
    SAFE_RELEASE(g_venum, ID3D11VideoProcessorEnumerator);
    SAFE_RELEASE(g_vctx, ID3D11VideoContext);
    SAFE_RELEASE(g_vdev, ID3D11VideoDevice);
    SAFE_RELEASE(g_comp, ID3D11Texture2D);
    SAFE_RELEASE(g_ctx, ID3D11DeviceContext);
    SAFE_RELEASE(g_dev, ID3D11Device);
}

static BOOL create_device(IDXGIAdapter *ad)
{
    D3D_FEATURE_LEVEL fl;
    ID3D10Multithread *mt = NULL;
    HRESULT hr = D3D11CreateDevice(ad, ad ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, NULL,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, NULL, 0,
                                   D3D11_SDK_VERSION, &g_dev, &fl, &g_ctx);
    if (FAILED(hr)) { log_printf(L"D3D11 のデバイスを作れない (0x%08lX)", hr); return FALSE; }
    /* エンコーダ(MFT)が別のスレッドから同じデバイスを使う */
    if (SUCCEEDED(ID3D11Device_QueryInterface(g_dev, &IID_ID3D10Multithread, (void **)&mt))) {
        ID3D10Multithread_SetMultithreadProtected(mt, TRUE);
        ID3D10Multithread_Release(mt);
    }
    return TRUE;
}

static int auto_kbps(int w, int h)
{
    double mp = (double)w * h / 1e6;            /* 1080p = 2.07 */
    int k = (int)(mp * 10000);                   /* 1080p で 約 20Mbps、4K で 約 80Mbps */
    if (k < 4000) k = 4000;
    if (k > 80000) k = 80000;
    return k;
}

/* 0 = 画質で決める(既定)。相手か設定が上限を求めたときだけ kbps */
static int target_kbps(int wanted)
{
    if (wanted > 0) return wanted;
    if (g_cfg.kbps > 0) return g_cfg.kbps;
    return 0;
}

/* 合成の絵(desk の大きさ)から、符号化する大きさの NV12 までを作る */
static BOOL pipeline_open(int dx, int dy, int dw, int dh)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd;
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd;
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd;
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE cs;
    int vw, vh, i;
    HRESULT hr;

    ZeroMemory(&td, sizeof(td));
    td.Width = (UINT)dw; td.Height = (UINT)dh; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(hr = ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &g_comp))) { log_printf(L"合成の絵を作れない (0x%08lX)", hr); return FALSE; }

    /* H.264 のレベル 5.1 の上限(4096x2304)に収める。GPU のエンコーダが無ければ 1920x1080 まで */
    video_size(dw, dh, 4096, 2304, &vw, &vh);
    set_desk(dx, dy, dw, dh, vw, vh);
    g_encKbps = target_kbps(0);
    g_enc = venc_open(g_dev, vw, vh, g_encKbps, !g_forceSoftEnc, g_encName, ARRAYSIZE(g_encName));
    if (g_enc && !venc_is_hw(g_enc) && (vw > 1920 || vh > 1080)) {
        venc_close(g_enc);
        video_size(dw, dh, 1920, 1080, &vw, &vh);
        set_desk(dx, dy, dw, dh, vw, vh);
        g_encKbps = target_kbps(0);
        g_enc = venc_open(g_dev, vw, vh, g_encKbps, FALSE, g_encName, ARRAYSIZE(g_encName));
    }
    if (!g_enc) return FALSE;

    if (FAILED(ID3D11Device_QueryInterface(g_dev, &IID_ID3D11VideoDevice, (void **)&g_vdev)) ||
        FAILED(ID3D11DeviceContext_QueryInterface(g_ctx, &IID_ID3D11VideoContext, (void **)&g_vctx))) {
        log_printf(L"D3D11 の動画の機能が無い");
        return FALSE;
    }
    ZeroMemory(&cd, sizeof(cd));
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputFrameRate.Numerator = 60; cd.InputFrameRate.Denominator = 1;
    cd.InputWidth = (UINT)dw; cd.InputHeight = (UINT)dh;
    cd.OutputFrameRate = cd.InputFrameRate;
    cd.OutputWidth = (UINT)vw; cd.OutputHeight = (UINT)vh;
    cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
    if (FAILED(hr = ID3D11VideoDevice_CreateVideoProcessorEnumerator(g_vdev, &cd, &g_venum)) ||
        FAILED(hr = ID3D11VideoDevice_CreateVideoProcessor(g_vdev, g_venum, 0, &g_vp))) {
        log_printf(L"NV12 への変換を作れない (0x%08lX)", hr);
        return FALSE;
    }
    ZeroMemory(&ivd, sizeof(ivd));
    ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    if (FAILED(hr = ID3D11VideoDevice_CreateVideoProcessorInputView(g_vdev, (ID3D11Resource *)g_comp, g_venum, &ivd, &g_inView))) {
        log_printf(L"変換の入力を作れない (0x%08lX)", hr);
        return FALSE;
    }
    td.Width = (UINT)vw; td.Height = (UINT)vh; td.Format = DXGI_FORMAT_NV12;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_VIDEO_ENCODER;
    ZeroMemory(&ovd, sizeof(ovd));
    ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    for (i = 0; i < NNV; i++) {
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &g_nv12[i]))) {
            td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;    /* D3D11.1 より前のドライバ */
            if (FAILED(hr = ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &g_nv12[i]))) { log_printf(L"NV12 の絵を作れない (0x%08lX)", hr); return FALSE; }
        }
        if (FAILED(hr = ID3D11VideoDevice_CreateVideoProcessorOutputView(g_vdev, (ID3D11Resource *)g_nv12[i], g_venum, &ovd, &g_outView[i]))) {
            log_printf(L"変換の出力を作れない (0x%08lX)", hr);
            return FALSE;
        }
    }
    /* 入力は RGB(0〜255)、出力は BT.709 の YCbCr(16〜235)。受け手も同じ式で戻す */
    ZeroMemory(&cs, sizeof(cs));
    cs.RGB_Range = 0;
    ID3D11VideoContext_VideoProcessorSetStreamColorSpace(g_vctx, g_vp, 0, &cs);
    cs.YCbCr_Matrix = 1;
    cs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    ID3D11VideoContext_VideoProcessorSetOutputColorSpace(g_vctx, g_vp, &cs);
    ID3D11VideoContext_VideoProcessorSetStreamFrameFormat(g_vctx, g_vp, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    ID3D11VideoContext_VideoProcessorSetStreamAutoProcessingMode(g_vctx, g_vp, 0, FALSE);
    g_nvNext = 0;
    g_forceKey = TRUE;
    g_dirty = TRUE;
    g_keyMiss = 0;
    return TRUE;
}

/* 合成の絵の一部を CPU の画素で置き換える(GDI・検証用・最初の絵) */
static void upload(const BYTE *bits, int pitch, const RECT *r)
{
    D3D11_BOX box;
    box.left = (UINT)r->left; box.top = (UINT)r->top; box.front = 0;
    box.right = (UINT)r->right; box.bottom = (UINT)r->bottom; box.back = 1;
    ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)g_comp, 0, &box,
                                          bits + (size_t)r->top * pitch + (size_t)r->left * 4, (UINT)pitch, 0);
}

/* ------------------------------------------------------------------ */
/*  DXGI                                                                */
/* ------------------------------------------------------------------ */

static void dxgi_close(void)
{
    int i;
    for (i = 0; i < g_ndup; i++)
        if (g_dups[i].dup) IDXGIOutputDuplication_Release(g_dups[i].dup);
    ZeroMemory(g_dups, sizeof(g_dups));
    g_ndup = 0;
}

/* DXGI の複製を始めた直後の 1 枚目は中身が無いことがある(iivnc で 2026-10-04 に実測)。
   画面が止まっていれば 2 枚目も来ないので、最初の絵とカーソルは GDI で撮って入れる。 */
static void gdi_seed(void)
{
    BITMAPINFO bi;
    HDC        sdc = GetDC(NULL), mdc = CreateCompatibleDC(sdc);
    HBITMAP    bmp, old;
    BYTE      *bits = NULL;
    CURSORINFO ci;
    int        i, n, w = g_scr.deskW, h = g_scr.deskH;

    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    bmp = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    if (bmp) {
        RECT full;
        old = (HBITMAP)SelectObject(mdc, bmp);
        BitBlt(mdc, 0, 0, w, h, sdc, g_scr.deskX, g_scr.deskY, SRCCOPY | CAPTUREBLT);
        GdiFlush();
        n = w * h;
        for (i = 0; i < n; i++) bits[i * 4 + 3] = 255;
        SetRect(&full, 0, 0, w, h);
        upload(bits, w * 4, &full);
        SelectObject(mdc, old);
        DeleteObject(bmp);
    }
    DeleteDC(mdc);
    ReleaseDC(NULL, sdc);
    ci.cbSize = sizeof(ci);
    if (GetCursorInfo(&ci)) {
        BOOL vis = (ci.flags & CURSOR_SHOWING) != 0;
        if (vis && ci.hCursor) cursor_from_hcursor(ci.hCursor);
        set_cursor_pos(ci.ptScreenPos.x - g_scr.deskX, ci.ptScreenPos.y - g_scr.deskY, vis);
        if (!vis) cursor_follow_hidden();
    }
}

static BOOL dxgi_open(void)
{
    IDXGIFactory1 *fac = NULL;
    IDXGIAdapter1 *ad;
    UINT   a, o;
    RECT   sel, bound = { 0 };
    BOOL   rotated = FALSE;
    HRESULT lastErr = S_OK;
    int    i;

    selected_rect(&sel);
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&fac))) return FALSE;
    /* 選ばれた範囲に画面がある最初の GPU を使い、その GPU の画面だけを取り込む */
    for (a = 0; !g_dev && IDXGIFactory1_EnumAdapters1(fac, a, &ad) == S_OK; a++) {
        IDXGIOutput *out;
        for (o = 0; g_ndup < MAX_DUP && IDXGIAdapter1_EnumOutputs(ad, o, &out) == S_OK; o++) {
            DXGI_OUTPUT_DESC        od;
            IDXGIOutput1           *o1 = NULL;
            IDXGIOutput5           *o5 = NULL;
            IDXGIOutputDuplication *dup = NULL;
            RECT                    isect;
            HRESULT                 hr = E_FAIL;

            IDXGIOutput_GetDesc(out, &od);
            if (!od.AttachedToDesktop || !IntersectRect(&isect, &od.DesktopCoordinates, &sel)) { IDXGIOutput_Release(out); continue; }
            if (od.Rotation != DXGI_MODE_ROTATION_IDENTITY && od.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED) {
                rotated = TRUE;
                IDXGIOutput_Release(out);
                continue;
            }
            if (!g_dev && !create_device((IDXGIAdapter *)ad)) { IDXGIOutput_Release(out); break; }
            if (SUCCEEDED(IDXGIOutput_QueryInterface(out, &IID_IDXGIOutput5, (void **)&o5))) {
                DXGI_FORMAT fmt = DXGI_FORMAT_B8G8R8A8_UNORM;
                hr = IDXGIOutput5_DuplicateOutput1(o5, (IUnknown *)g_dev, 0, 1, &fmt, &dup);
                IDXGIOutput5_Release(o5);
            }
            if (FAILED(hr) && SUCCEEDED(IDXGIOutput_QueryInterface(out, &IID_IDXGIOutput1, (void **)&o1))) {
                hr = IDXGIOutput1_DuplicateOutput(o1, (IUnknown *)g_dev, &dup);
                IDXGIOutput1_Release(o1);
            }
            IDXGIOutput_Release(out);
            if (FAILED(hr)) { lastErr = hr; continue; }
            {
                Dup *d = &g_dups[g_ndup++];
                ZeroMemory(d, sizeof(*d));
                d->dup = dup;
                d->desk = od.DesktopCoordinates;
                d->w = d->desk.right - d->desk.left;
                d->h = d->desk.bottom - d->desk.top;
            }
        }
        IDXGIAdapter1_Release(ad);
    }
    IDXGIFactory1_Release(fac);

    if (!g_ndup || rotated) {
        if (rotated) log_printf(L"DXGI: 回転した画面があるので GDI で取り込む");
        else log_printf(L"DXGI: 複製できない (0x%08lX)", (unsigned long)lastErr);
        dxgi_close();
        pipeline_close();
        return FALSE;
    }
    /* 範囲 = 選ばれた範囲のうち、複製できた画面の分 */
    bound = g_dups[0].desk;
    for (i = 1; i < g_ndup; i++) UnionRect(&bound, &bound, &g_dups[i].desk);
    IntersectRect(&bound, &bound, &sel);
    if (!pipeline_open(bound.left, bound.top, bound.right - bound.left, bound.bottom - bound.top)) {
        dxgi_close();
        pipeline_close();
        return FALSE;
    }
    for (i = 0; i < g_ndup; i++) {
        g_dups[i].ox = g_dups[i].desk.left - bound.left;
        g_dups[i].oy = g_dups[i].desk.top - bound.top;
    }
    log_mouse_devices();
    gdi_seed();
    log_printf(L"DXGI で取り込む(画面 %d 個)。最初の絵は GDI で撮った", g_ndup);
    return TRUE;
}

/* 戻り値: 1 = 何か変わった、0 = 変わらない、-1 = 開き直しが要る */
static int dxgi_grab(DWORD timeout)
{
    int i, result = 0;
    for (i = 0; i < g_ndup; i++) {
        Dup *d = &g_dups[i];
        DXGI_OUTDUPL_FRAME_INFO fi;
        IDXGIResource *res = NULL;
        HRESULT hr = IDXGIOutputDuplication_AcquireNextFrame(d->dup, i == 0 ? timeout : 0, &fi, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
        if (FAILED(hr)) {
            log_printf(L"DXGI: AcquireNextFrame 0x%08lX", (unsigned long)hr);
            return -1;
        }
        if (fi.LastMouseUpdateTime.QuadPart) {
            if (fi.PointerPosition.Visible) {
                g_curOwner = i;
                set_cursor_pos(d->ox + fi.PointerPosition.Position.x, d->oy + fi.PointerPosition.Position.y, TRUE);
            } else if (g_curOwner == i || g_curOwner < 0) {
                set_cursor_pos(g_scr.curX, g_scr.curY, FALSE);
            }
        }
        if (fi.PointerShapeBufferSize) {
            if (fi.PointerShapeBufferSize > g_shapeCap) {
                free(g_shape);
                g_shape = (BYTE *)malloc(fi.PointerShapeBufferSize);
                g_shapeCap = g_shape ? fi.PointerShapeBufferSize : 0;
            }
            if (g_shape) {
                DXGI_OUTDUPL_POINTER_SHAPE_INFO si;
                UINT need = 0;
                if (SUCCEEDED(IDXGIOutputDuplication_GetFramePointerShape(d->dup, g_shapeCap, g_shape, &need, &si))) {
                    cursor_from_dxgi(&si, g_shape);
                    g_lastCursor = NULL;
                }
            }
        }
        if (!g_scr.curVisible) cursor_follow_hidden();

        if (fi.LastPresentTime.QuadPart && res && fi.TotalMetadataBufferSize) {
            ID3D11Texture2D *tex = NULL;
            if (SUCCEEDED(IDXGIResource_QueryInterface(res, &IID_ID3D11Texture2D, (void **)&tex))) {
                D3D11_BOX box;
                int x0 = max(0, -d->ox), y0 = max(0, -d->oy);
                int x1 = min(d->w, g_scr.deskW - d->ox), y1 = min(d->h, g_scr.deskH - d->oy);
                if (x1 > x0 && y1 > y0) {
                    box.left = (UINT)x0; box.top = (UINT)y0; box.front = 0;
                    box.right = (UINT)x1; box.bottom = (UINT)y1; box.back = 1;
                    ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)g_comp, 0, (UINT)(d->ox + x0), (UINT)(d->oy + y0), 0,
                                                              (ID3D11Resource *)tex, 0, &box);
                    g_dirty = TRUE;
                    if (fi.LastPresentTime.QuadPart > g_presentQpc) g_presentQpc = fi.LastPresentTime.QuadPart;
                    result = 1;
                }
                ID3D11Texture2D_Release(tex);
            }
        }
        if (res) IDXGIResource_Release(res);
        IDXGIOutputDuplication_ReleaseFrame(d->dup);
    }
    return result;
}

/* ------------------------------------------------------------------ */
/*  GDI                                                                 */
/* ------------------------------------------------------------------ */

static void gdi_close(void)
{
    if (!g_memDC) return;
    SelectObject(g_memDC, g_oldBmp);
    DeleteObject(g_dib);
    DeleteDC(g_memDC);
    g_memDC = NULL;
    g_dibBits = NULL;
    free(g_gdiPrev);
    g_gdiPrev = NULL;
}

static BOOL gdi_open(void)
{
    BITMAPINFO bi;
    HDC  dc;
    RECT r;
    selected_rect(&r);
    if (!create_device(NULL) || !pipeline_open(r.left, r.top, r.right - r.left, r.bottom - r.top)) { pipeline_close(); return FALSE; }
    dc = GetDC(NULL);
    g_memDC = CreateCompatibleDC(dc);
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = g_scr.deskW;
    bi.bmiHeader.biHeight = -g_scr.deskH;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    g_dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&g_dibBits, NULL, 0);
    ReleaseDC(NULL, dc);
    g_gdiPrev = (BYTE *)calloc((size_t)g_scr.deskW * g_scr.deskH, 4);
    if (!g_dib || !g_gdiPrev) { gdi_close(); pipeline_close(); return FALSE; }
    g_oldBmp = (HBITMAP)SelectObject(g_memDC, g_dib);
    g_lastGdi = 0;
    log_mouse_devices();
    log_printf(L"GDI で取り込む");
    return TRUE;
}

static int gdi_grab(DWORD timeout)
{
    HDC   dc;
    RECT  full;
    CURSORINFO ci;
    int   i, n = g_scr.deskW * g_scr.deskH, result = 0;
    DWORD el = GetTickCount() - g_lastGdi;
    BOOL  ok;

    if (el < 33) {                       /* 30 回/秒まで */
        Sleep(min(timeout, 33 - el));
        if (GetTickCount() - g_lastGdi < 33) return 0;
    }
    g_lastGdi = GetTickCount();
    dc = GetDC(NULL);
    ok = BitBlt(g_memDC, 0, 0, g_scr.deskW, g_scr.deskH, dc, g_scr.deskX, g_scr.deskY, SRCCOPY | CAPTUREBLT);
    ReleaseDC(NULL, dc);
    if (!ok) return -1;
    GdiFlush();
    for (i = 0; i < n; i++) g_dibBits[i * 4 + 3] = 255;
    if (memcmp(g_dibBits, g_gdiPrev, (size_t)n * 4)) {
        memcpy(g_gdiPrev, g_dibBits, (size_t)n * 4);
        SetRect(&full, 0, 0, g_scr.deskW, g_scr.deskH);
        upload(g_dibBits, g_scr.deskW * 4, &full);
        g_dirty = TRUE;
        g_presentQpc = qpc_now();
        result = 1;
    }
    ci.cbSize = sizeof(ci);
    if (GetCursorInfo(&ci)) {
        BOOL vis = (ci.flags & CURSOR_SHOWING) != 0;
        if (vis && ci.hCursor != g_lastCursor) {
            g_lastCursor = ci.hCursor;
            cursor_from_hcursor(ci.hCursor);
        }
        set_cursor_pos(ci.ptScreenPos.x - g_scr.deskX, ci.ptScreenPos.y - g_scr.deskY, vis);
        if (!vis) cursor_follow_hidden();
    }
    return result;
}

/* ------------------------------------------------------------------ */
/*  検証用の絵(合成)                                                    */
/* ------------------------------------------------------------------ */

static BOOL g_testSmall;        /* -testresize の後(1280x720) */

static BOOL test_open(void)
{
    BITMAPINFO bi;
    HDC dc;
    HFONT f;
    RECT r;
    int i, tw = g_testSmall ? 1280 : TW, th = g_testSmall ? 720 : TH;
    BYTE *cur, *mask;

    if (!create_device(NULL) || !pipeline_open(0, 0, tw, th)) { pipeline_close(); return FALSE; }
    if (!g_test) {
        timeBeginPeriod(1);             /* 検証用の絵の速さを Sleep で合わせるため(既定の 15.6ms 刻みでは 60fps にならない) */
        dc = GetDC(NULL);
        g_testDC = CreateCompatibleDC(dc);
        ZeroMemory(&bi, sizeof(bi));
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = TW;
        bi.bmiHeader.biHeight = -TH;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        g_testBmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&g_test, NULL, 0);
        ReleaseDC(NULL, dc);
        g_testOld = (HBITMAP)SelectObject(g_testDC, g_testBmp);
        f = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, SHIFTJIS_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Yu Gothic UI");
        SelectObject(g_testDC, f);

        /* 背景、窓、帯 */
        SetRect(&r, 0, 0, TW, TH);
        FillRect(g_testDC, &r, (HBRUSH)GetStockObject(LTGRAY_BRUSH));
        for (i = 0; i < 16; i++) {
            HBRUSH b = CreateSolidBrush(RGB(i * 16, 255 - i * 16, (i * 53) & 255));
            SetRect(&r, i * (TW / 16), TH - 60, (i + 1) * (TW / 16), TH);
            FillRect(g_testDC, &r, b);
            DeleteObject(b);
        }
        SetRect(&r, SX - 4, SY - 30, SX + SW + 4, SY + SH + 4);
        FillRect(g_testDC, &r, (HBRUSH)GetStockObject(GRAY_BRUSH));
        for (i = 0; i < SH / 20; i++) test_text_line(SY + i * 20, i);
        test_photo(0);
        SetRect(&r, 0, 0, TW, TH);
        test_fix_alpha(&r);
        g_testFrame = 0;

        /* 矢印のカーソル(12x19) */
        cur  = (BYTE *)calloc(12 * 19, 4);
        mask = (BYTE *)calloc(2 * 19, 1);
        for (i = 0; i < 19; i++) {
            int x, w = i < 12 ? i + 1 : (i < 16 ? 4 : 3);
            for (x = 0; x < w && x < 12; x++) {
                BYTE v = (x == 0 || x == w - 1 || i == 18) ? 0 : 255;
                cur[(i * 12 + x) * 4 + 0] = cur[(i * 12 + x) * 4 + 1] = cur[(i * 12 + x) * 4 + 2] = v;
                cur[(i * 12 + x) * 4 + 3] = 255;
                mask[i * 2 + x / 8] |= (BYTE)(0x80 >> (x & 7));
            }
        }
        if (g_testCursor != 2) set_cursor_shape(12, 19, 0, 0, cur, mask);
        set_cursor_pos(960, 540, !g_testCursor);
        if (g_testCursor && g_cfg.showCursor) cursor_ensure_shape();
        free(cur);
        free(mask);
    }
    SetRect(&r, 0, 0, tw, th);
    upload(g_test, TW * 4, &r);
    g_presentQpc = qpc_now();
    log_printf(L"検証用の絵(%dx%d)", tw, th);
    if (g_testDump[0]) {
        BITMAPFILEHEADER fh;
        BITMAPINFOHEADER ih;
        HANDLE fd = CreateFileW(g_testDump, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        DWORD  wr;
        int    y;
        ZeroMemory(&fh, sizeof(fh));
        ZeroMemory(&ih, sizeof(ih));
        ih.biSize = sizeof(ih); ih.biWidth = tw; ih.biHeight = -th; ih.biPlanes = 1; ih.biBitCount = 32; ih.biCompression = BI_RGB;
        fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(ih); fh.bfSize = fh.bfOffBits + (DWORD)tw * th * 4;
        if (fd != INVALID_HANDLE_VALUE) {
            WriteFile(fd, &fh, sizeof(fh), &wr, NULL);
            WriteFile(fd, &ih, sizeof(ih), &wr, NULL);
            for (y = 0; y < th; y++) WriteFile(fd, g_test + (size_t)y * TW * 4, (DWORD)tw * 4, &wr, NULL);
            CloseHandle(fd);
        }
    }
    return TRUE;
}

static void test_close(void)
{
    if (!g_testDC) return;
    SelectObject(g_testDC, g_testOld);
    DeleteObject(g_testBmp);
    DeleteDC(g_testDC);
    g_testDC = NULL;
    g_test = NULL;
}

/* blocked = 送れない(返事待ち)。そのときは新しい絵を作らない */
static int test_grab(DWORD timeout, BOOL blocked)
{
    RECT r;
    int  y;

    if (blocked || g_testSrc == 2 || (g_testFrames && g_testFrame >= g_testFrames)) {
        WaitForSingleObject(g_scr.hWork, blocked ? 2 : 50);
        return 0;
    }
    if (g_testFps > 0) {                /* 決めた速さより速くは作らない */
        static LARGE_INTEGER last, freq;
        LARGE_INTEGER now;
        LONGLONG wait;
        if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&now);
        wait = last.QuadPart ? (last.QuadPart + freq.QuadPart / g_testFps - now.QuadPart) * 1000 / freq.QuadPart : 0;
        if (wait > 0) {
            Sleep((DWORD)min(wait, (LONGLONG)timeout));
            QueryPerformanceCounter(&now);
            if (now.QuadPart < last.QuadPart + freq.QuadPart / g_testFps) return 0;
        }
        last = now;
    }
    (void)timeout;
    g_testFrame++;
    if (g_testResize && g_testFrame == g_testResize && !g_testSmall) {
        g_testSmall = TRUE;             /* 画面の大きさが変わった: 開き直す */
        return -1;
    }
    if (g_testSrc == 3) {
        test_video(g_testFrame);
    } else {
        /* 文字の欄を 20px 上へ送り、下に 1 行足す。四角を動かす。写真のような欄を変える */
        for (y = 0; y < SH - 20; y++)
            memmove(g_test + ((size_t)(SY + y) * TW + SX) * 4, g_test + ((size_t)(SY + y + 20) * TW + SX) * 4, (size_t)SW * 4);
        test_text_line(SY + SH - 20, SH / 20 + g_testFrame - 1);
        SetRect(&r, SX, SY + SH - 20, SX + SW, SY + SH);
        test_fix_alpha(&r);
        {
            int bx = 1000 + (g_testFrame * 7) % 800, by = 600;
            HBRUSH b = CreateSolidBrush(RGB((g_testFrame * 5) & 255, 80, 200));
            SetRect(&r, 1000, 600, 1900, 700);
            FillRect(g_testDC, &r, (HBRUSH)GetStockObject(LTGRAY_BRUSH));
            SetRect(&r, bx, by, bx + 60, by + 60);
            FillRect(g_testDC, &r, b);
            DeleteObject(b);
            SetRect(&r, 1000, 600, 1900, 700);
            test_fix_alpha(&r);
        }
        test_photo(g_testFrame);
    }
    SetRect(&r, 0, 0, g_scr.deskW, g_scr.deskH);
    upload(g_test, TW * 4, &r);
    g_dirty = TRUE;
    g_presentQpc = qpc_now();
    return 1;
}

/* ------------------------------------------------------------------ */
/*  符号化して配る                                                      */
/* ------------------------------------------------------------------ */

static void stat_add(int bytes, LONG64 encTicks)
{
    static LARGE_INTEGER freq;
    DWORD now = GetTickCount();
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    if (!g_statSince) g_statSince = now;
    g_statFrames++;
    g_statBytes += bytes;
    g_statEncTicks += encTicks;
    if (now - g_statSince >= 5000) {
        double sec = (now - g_statSince) / 1000.0;
        log_printf(L"映像: %.1f フレーム/秒、符号化 %.2fms、平均 %I64d バイト、%.1f Mbps", g_statFrames / sec,
                   g_statEncTicks * 1000.0 / (double)freq.QuadPart / g_statFrames, g_statBytes / g_statFrames, g_statBytes * 8 / sec / 1e6);
        g_statFrames = 0;
        g_statBytes = g_statEncTicks = 0;
        g_statSince = now;
    }
}

static void encode_and_deliver(BOOL key, BOOL still)
{
    D3D11_VIDEO_PROCESSOR_STREAM st;
    VFrame *f;
    int     i = g_nvNext;
    LONG64  t0, t1;
    HRESULT hr;

    g_nvNext = (g_nvNext + 1) % NNV;
    ZeroMemory(&st, sizeof(st));
    st.Enable = TRUE;
    st.pInputSurface = g_inView;
    hr = ID3D11VideoContext_VideoProcessorBlt(g_vctx, g_vp, g_outView[i], 0, 1, &st);
    if (FAILED(hr)) { log_printf(L"NV12 への変換に失敗 (0x%08lX)", hr); return; }
    /* キーフレームと、止まった後の送り直しは高い画質で。動いている間は軽く */
    venc_set_quality(g_enc, (key || still) ? g_cfg.qStill : g_cfg.qMove);
    t0 = qpc_now();
    f = venc_encode(g_enc, g_nv12[i], key, g_pts);
    t1 = qpc_now();
    g_pts += 166667;
    g_lastEncode = GetTickCount();
    g_dirty = FALSE;
    if (!f) return;
    if (key && !f->key) {
        /* キーフレームを求めたのに来なかった。続くようならエンコーダを開き直す(最初の絵は必ずキーフレーム) */
        if (++g_keyMiss >= 3) { log_printf(L"符号化: キーフレームにならないので開き直す"); InterlockedExchange(&g_reset, 1); }
    } else {
        if (f->key) g_keyMiss = 0;
        g_forceKey = FALSE;
    }
    f->no = ++g_frameNo;
    f->presentQpc = g_presentQpc ? g_presentQpc : t0;
    f->encQpc = t1;
    f->cfgVer = g_scr.cfgVer;
    g_presentQpc = 0;
    server_video_deliver(f);
    stat_add(f->len, t1 - t0);
    vframe_release(f);
}

/* ------------------------------------------------------------------ */
/*  スレッド                                                            */
/* ------------------------------------------------------------------ */

static void close_all(void)
{
    dxgi_close();
    gdi_close();
    pipeline_close();
    g_mode = MODE_NONE;
    lstrcpyW(g_scr.method, L"-");
}

static BOOL open_all(void)
{
    if (g_runMode == RUN_AGENT && !g_testSrc) agent_follow_input_desktop();
    if (g_testSrc) {
        if (!test_open()) return FALSE;
        g_mode = MODE_TEST;
        _snwprintf(g_scr.method, ARRAYSIZE(g_scr.method), L"検証用・%s", g_encName);
    } else if (!g_forceGdi && dxgi_open()) {
        g_mode = MODE_DXGI;
        _snwprintf(g_scr.method, ARRAYSIZE(g_scr.method), L"DXGI・%s", g_encName);
    } else if (gdi_open()) {
        g_mode = MODE_GDI;
        _snwprintf(g_scr.method, ARRAYSIZE(g_scr.method), L"GDI・%s", g_encName);
    } else {
        return FALSE;
    }
    g_scr.method[ARRAYSIZE(g_scr.method) - 1] = 0;
    return TRUE;
}

static DWORD WINAPI video_thread(void *arg)
{
    DWORD idleSince = GetTickCount();
    (void)arg;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    while (!g_quit) {
        BOOL needKey = FALSE, blocked, pending;
        int  inflight = 0, kbps = 0, r = 0;
        DWORD timeout;

        if (!server_video_wanted(&needKey, &inflight, &kbps)) {
            if (g_mode != MODE_NONE && GetTickCount() - idleSince >= 3000) {
                close_all();
                log_printf(L"接続が無いので取り込みを止めた");
            }
            WaitForSingleObject(g_scr.hWork, 1000);
            continue;
        }
        idleSince = GetTickCount();
        if (InterlockedExchange(&g_reset, 0) && g_mode != MODE_NONE) close_all();
        if (g_runMode == RUN_AGENT && g_mode != MODE_NONE && !g_testSrc) {
            static DWORD lastCheck;
            if (GetTickCount() - lastCheck >= 250) {
                lastCheck = GetTickCount();
                if (agent_input_desktop_changed()) {
                    log_printf(L"[agent] 入力デスクトップが替わった");
                    close_all();
                }
            }
        }
        if (g_mode == MODE_NONE && !open_all()) {
            close_all();
            Sleep(500);
            continue;
        }
        if (target_kbps(kbps) != g_encKbps) {
            g_encKbps = target_kbps(kbps);
            venc_set_kbps(g_enc, g_encKbps);
        }
        if (needKey) g_forceKey = TRUE;

        blocked = inflight >= IIV_MAX_INFLIGHT;
        pending = g_dirty || g_forceKey;
        timeout = pending ? (blocked ? 4 : 0) : 50;
        switch (g_mode) {
        case MODE_DXGI: r = dxgi_grab(timeout); break;
        case MODE_GDI:  r = gdi_grab(timeout); break;
        case MODE_TEST: r = test_grab(timeout, blocked && !g_forceKey); break;
        }
        if (r < 0) {
            close_all();
            Sleep(50);
            continue;
        }
        if ((g_dirty || g_forceKey) && !blocked) {
            if (g_cfg.maxFps > 0 && !g_forceKey && GetTickCount() - g_lastEncode < (DWORD)(1000 / g_cfg.maxFps)) continue;
            BOOL wasKey = g_forceKey;
            encode_and_deliver(g_forceKey, FALSE);
            g_lastMove = GetTickCount();
            g_refined = wasKey;                     /* キーフレームは高い画質で送ったので、送り直さなくてよい */
        } else if (!g_dirty && !blocked && !g_refined && !g_encKbps && g_lastMove &&
                   GetTickCount() - g_lastMove >= (DWORD)g_cfg.stillMs) {
            /* 変化が止まった: 同じ絵を高い画質でもう 1 枚(にじんだ文字がくっきりする) */
            g_refined = TRUE;
            g_presentQpc = qpc_now();
            encode_and_deliver(FALSE, TRUE);
        } else if (blocked && g_mode == MODE_DXGI && !timeout) {
            WaitForSingleObject(g_scr.hWork, 4);
        }
    }
    close_all();
    test_close();
    CoUninitialize();
    return 0;
}

void video_kick(void) { SetEvent(g_scr.hWork); }

void video_reset(void)
{
    InterlockedExchange(&g_reset, 1);
    SetEvent(g_scr.hWork);
}

void video_init(void)
{
    InitializeSRWLock(&g_scr.lock);
    g_scr.hWork = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_scr.curVisible = TRUE;
    lstrcpyW(g_scr.method, L"-");
    g_thread = CreateThread(NULL, 0, video_thread, NULL, 0, NULL);
}

void video_shutdown(void)
{
    InterlockedExchange(&g_quit, 1);
    SetEvent(g_scr.hWork);
    if (g_thread) {
        WaitForSingleObject(g_thread, 5000);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
}
