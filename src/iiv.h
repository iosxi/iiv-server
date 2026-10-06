/* ==================================================================
 * iiv.h - iiv-server 全体で使う宣言
 *
 *  構成
 *    main.c     起動、タスクトレイ、多重起動の判定
 *    config.c   iiv-server.ini の読み書き、ログ
 *    auth.c     パスワードの鍵(PBKDF2-SHA256)と確かめ(HMAC-SHA256)
 *    video.c    画面の取り込み(DXGI の複製 / GDI / 検証用の絵)から符号化までのスレッド、カーソル
 *    venc.c     H.264 の符号化(Media Foundation。GPU のエンコーダが無ければ CPU)
 *    server.c   待ち受け、iiv の手順、接続ごとの送受信
 *    input.c    キー・マウスの再現
 *    clip.c     クリップボードの受け渡し
 *    filexfer.c ファイルのコピー＆貼り付け(iiv-client と同じファイル)
 *    ui.c       設定画面
 *    svc.c      サービス(ログイン前から使う)
 *    theme.c    ライト/ダークの配色(kotemado と同じもの)
 *    fwrules.c  ファイアウォールの、この exe の規則(iiv-client と同じファイル)
 *    zdeflate.c zinflate.c  共通部品
 *  通信の取り決めは iivproto.h(iiv-client と同じファイル)。
 * ================================================================== */
#ifndef IIV_H
#define IIV_H

#ifndef UNICODE
#error "UNICODE を定義してビルドする(build.bat は /DUNICODE を付けている)"
#endif

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "zlite.h"
#include "iivproto.h"

#define APP_NAME     L"iiv-server"
#define APP_VERSION  L"1.1.0"
#define APP_VERSION_A "1.1.0"

#define WM_APP_TRAY     (WM_APP + 1)
#define WM_APP_COMMAND  (WM_APP + 2)    /* 別のプロセスから(-exit など) */
#define WM_APP_SETCLIP  (WM_APP + 3)    /* lParam = 相手から来た文字(malloc した WCHAR*) */
#define WM_APP_CLIENTS  (WM_APP + 4)    /* 接続の数が変わった */
#define WM_APP_NOTIFY   (WM_APP + 5)    /* lParam = 知らせる文字(malloc した WCHAR*) */
#define WM_APP_RESTART  (WM_APP + 6)    /* 待ち受けをやり直す(設定が変わった) */
#define WM_APP_RELOAD   (WM_APP + 7)    /* 分身: 設定を読み直す */
#define WM_APP_DISCONNECT (WM_APP + 8)  /* 分身: 全員を切断 */
#define WM_APP_FXOFFER  (WM_APP + 9)    /* 検証用: wParam の相手へ、今クリップボードにあるファイルを渡す */
#define TRAY_CLASS      L"iiv.Server.Tray"
#define AGENT_TITLE     L"iiv-server agent"   /* 分身の窓の題(サービスのトレイが探す) */
#define CD_CLIP_TEXT    1                   /* WM_COPYDATA: サービスのトレイ → 分身。利用者がコピーした文字(UTF-16) */
#define CD_CLIP_FILES   2                   /* 同じく、コピーしたファイルの一覧(0 区切り、最後に 0 が 2 つ) */

#define CMD_EXIT 1

/* ------------------------------------------------------------------ */
/*  設定(config.c)                                                     */
/* ------------------------------------------------------------------ */

typedef struct PwKey {          /* パスワードから導いた鍵(平文は持たない) */
    BOOL  set;
    BYTE  salt[16];
    DWORD iter;
    BYTE  key[32];
} PwKey;

typedef struct Config {
    int   port;                 /* 待ち受けるポート(既定 IIV_DEFAULT_PORT) */
    WCHAR listen[64];           /* 空 = すべて、"127.0.0.1" など */
    PwKey pw;                   /* 操作できるパスワード */
    PwKey viewPw;               /* 見るだけのパスワード */
    int   viewOnly;             /* 1 = 誰にも操作させない */
    int   display;              /* 0 = すべての画面、n = \\.\DISPLAYn だけ */
    int   notify;               /* 1 = 接続・切断を通知で知らせる */
    int   showCursor;           /* 1 = Windows がカーソルを隠していても(マウスが無い PC など)相手に見せる */
    int   noSleep;              /* 1 = 接続されている間はスリープさせず、画面も消さない */
    int   kbps;                 /* 映像のビットレート(kbps)。0 = 絵の大きさから決める */
    int   maxFps;               /* 0 = 画面の書き換えの速さまで */
    int   qMove;                /* 動いている間の画質(0〜100。CODECAPI_AVEncCommonQuality) */
    int   qStill;               /* 止まったら送り直す画質(0〜100) */
    int   stillMs;              /* 変化が止まってから送り直すまで(ms) */
    int   speed;                /* エンコーダの速さ優先(0〜100。0 = いちばん速い、-1 = 任せる。保存しない) */
    int   fxOffer;              /* 検証用(ini の fxoffer=1。保存しない) */
    int   theme;                /* 0 = システム、1 = ライト、2 = ダーク */
    int   log;
    int   sasBefore;            /* サービス登録の前の SoftwareSASGeneration(-1 = 無かった、-2 = 控えていない) */
    int   selftest;             /* 検証用: 分身が起動 20 秒後に入力デスクトップを撮ってログに書く */
} Config;

extern Config g_cfg;
extern HINSTANCE g_inst;
extern WCHAR  g_iniPath[MAX_PATH];
extern WCHAR  g_exeDir[MAX_PATH];
extern HWND   g_mainWnd;
extern BOOL   g_dryRun;         /* -dryrun: 入力を再現せずログに書く */
extern int    g_testSrc;        /* -testsrc: 画面の代わりに検証用の絵(1 = 動く、2 = 止まった絵、3 = 全面が動く) */
extern int    g_testFrames;     /* -testsrc で動かすフレーム数(0 = ずっと) */
extern int    g_testFps;        /* -testsrc の 1 秒あたりのフレーム数(0 = 求められるだけ) */
extern int    g_testResize;     /* -testresize N: N フレーム目で 1280x720 に変える */
extern int    g_testCursor;     /* -testcursor: 1 = hidden、2 = none */
extern BOOL   g_forceGdi;       /* -gdi: DXGI を使わず GDI で取り込む */
extern BOOL   g_forceSoftEnc;   /* -softenc: GPU のエンコーダを使わない */
extern WCHAR  g_testDump[MAX_PATH]; /* -testdump: 検証用の絵を BMP に書く */

void config_init(void);
void config_load(void);
BOOL config_save(void);
void log_open(void);
void log_printf(const WCHAR *fmt, ...);
void log_printfA(const char *fmt, ...);
char  *utf16_to_utf8(const WCHAR *s, int *outLen);
WCHAR *utf8_to_utf16(const char *s, int len);

/* ------------------------------------------------------------------ */
/*  パスワード(auth.c)                                                 */
/* ------------------------------------------------------------------ */

BOOL auth_random(void *p, ULONG n);
BOOL auth_derive(const char *utf8pw, int len, const BYTE *salt, DWORD iter, BYTE key[32]);
BOOL auth_hmac(const BYTE key[32], const void *data, ULONG n, BYTE out[32]);
BOOL auth_set_password(PwKey *k, const WCHAR *pw);          /* 新しいソルトで鍵を作る。空なら消す */
void auth_to_text(const PwKey *k, char *out, int cap);      /* ini に書く形 */
void auth_from_text(PwKey *k, const char *s);
BOOL auth_check(const PwKey *k, const BYTE nonce[32], const BYTE proof[32]);

/* ------------------------------------------------------------------ */
/*  映像(video.c / venc.c)                                             */
/* ------------------------------------------------------------------ */

typedef struct VFrame {         /* 符号化した 1 フレーム(接続どうしで共有する) */
    volatile LONG ref;
    UINT32  no;
    BOOL    key;
    LONG64  presentQpc, encQpc;
    int     cfgVer;             /* この絵の大きさの版(g_scr.cfgVer) */
    int     len;
    BYTE    data[1];
} VFrame;

void vframe_release(VFrame *f);

typedef struct Client Client;

typedef struct Screen {
    SRWLOCK lock;
    int     deskX, deskY, deskW, deskH;     /* 取り込む範囲(仮想画面の座標) */
    int     vidW, vidH;                     /* 符号化する絵の大きさ(偶数。4096 を超えれば縮める) */
    int     cfgVer;                         /* 大きさが変わるたびに増える */
    /* カーソル */
    int     curVer;
    int     curW, curH, curHotX, curHotY;
    BYTE   *curPix;             /* BGRA */
    BYTE   *curMask;            /* 1 = 見える(1 行 = (curW+7)/8 バイト) */
    int     curX, curY;         /* 取り込む範囲での位置 */
    BOOL    curVisible;
    int     curPosVer;
    /* 接続 */
    Client *clients;
    HANDLE  hWork;              /* 映像のスレッドを起こす(誰かが来た・返事が来た) */
    WCHAR   method[64];         /* "DXGI + NVIDIA H.264 Encoder MFT" など */
} Screen;

extern Screen g_scr;

void video_init(void);
void video_shutdown(void);
void video_kick(void);                      /* 映像のスレッドを起こす */
void video_reset(void);                     /* 設定が変わったので開き直す */

/* venc.c: H.264 の符号化。D3D11 の NV12 テクスチャを受け取る */
typedef struct VEnc VEnc;
VEnc   *venc_open(void *d3dDevice, int w, int h, int kbps, BOOL allowHw, WCHAR *name, int nameCap);
BOOL    venc_is_hw(VEnc *e);
VFrame *venc_encode(VEnc *e, void *nv12Texture, BOOL forceKey, LONG64 pts100ns);
void    venc_set_kbps(VEnc *e, int kbps);        /* 0 = 画質で決める(venc_set_quality) */
void    venc_set_quality(VEnc *e, int q);
void    venc_close(VEnc *e);

/* ------------------------------------------------------------------ */
/*  接続(server.c)                                                     */
/* ------------------------------------------------------------------ */

#define VQ_MAX 8                /* 接続ごとの映像の待ち行列 */

struct Client {
    Client *next;
    int     id;
    SOCKET  s;
    WCHAR   addr[80];
    HANDLE  thrRead, thrWrite, hWake;
    volatile LONG quit;
    CRITICAL_SECTION sendLock;  /* 1 つのメッセージを途中で混ぜない */
    CRITICAL_SECTION cs;        /* 下の「待ち行列」などを守る */
    BOOL    viewOnly;
    BOOL    active;             /* 初期化まで済んだ */
    BOOL    fileXfer;           /* 相手もファイルを受け渡せる */

    /* 映像(cs で守る) */
    VFrame *vq[VQ_MAX];         /* 送るのを待っているフレーム */
    int     vqHead, vqCount;
    BOOL    wantKey;            /* キーフレームから受け取り始める(来たばかり・絵が壊れた) */
    BOOL    started;            /* キーフレームを受け取った(以後のフレームを受け取れる) */
    BOOL    resync;             /* 待ち行列があふれた: 送り手が追いついたらキーフレームを求める */
    UINT32  lastQueued;         /* 待ち行列に入れた最後の番号 */
    volatile LONG lastAcked;    /* 表示したと返ってきた最後の番号 */
    int     cfgSent;            /* 送った IIV_S_VIDEO_CONFIG の版 */
    int     kbpsWanted;         /* 相手が求めたビットレート(0 = 任せる) */
    volatile DWORD lastAckTick; /* 最後に返事が来た時刻(返事の無い相手を待ち続けないため) */
    LONG64  sentPresent[64];    /* 番号 % 64 → 画面に出た時刻(返事が来たら遅れを測る) */

    /* カーソル(g_scr.lock で守る) */
    int     cursorVer;          /* 送った形の版 */
    POINT   cursorSent;         /* 送った位置 */
    POINT   pointerFromClient;  /* この相手が最後に動かした位置 */
    BOOL    cursorVisSent;

    /* ファイルのコピー＆貼り付け(cs で守る) */
    BOOL    fxHelloPending;
    struct FxMsg *fxHead, *fxTail;

    /* クリップボード(cs で守る) */
    char   *clipOut;            /* 相手へ送る UTF-8(NULL = 無し) */
    int     clipOutLen;

    /* 統計(返事の数・遅れは読み手、送った量は書き手が足す) */
    volatile LONG64 bytesSent;
    volatile LONG   frames;
    LONG64  statLatTicks, statDecodeUs, statBytes0;
    int     statAcks, statFrames0;
    DWORD   statSince;
    LONG64  pingQpc;            /* 送った IIV_S_PING の時刻 */
    double  rttMs;
};

extern volatile LONG g_clientCount;

BOOL server_start(void);
void server_stop(void);
void server_disconnect_all(void);
void server_disconnect(int id);
int  server_list(WCHAR *buf, int cap);      /* 「アドレス」を改行で並べる。戻り値は数 */
void server_clipboard_changed(const char *utf8, int len);
void server_files_changed(HDROP hd);
void server_files_changed_paths(const WCHAR *paths);
void server_fx_offer_current(int id);
void clip_text_from_tray(const WCHAR *text);    /* 分身: サービスのトレイが読んだ利用者の文字 */
void power_update(void);                        /* main.c */

/* 映像のスレッドから: 送る状態の確認と配る */
BOOL server_video_wanted(BOOL *needKey, int *maxInflight, int *kbps);
void server_video_deliver(VFrame *f);
void server_cursor_changed(void);               /* カーソルの形・位置が変わった */

/* filexfer.c: ファイルのコピー＆貼り付け(iiv-client と同じファイル) */
#define FX_MAX          (16 << 20)          /* 1 つのメッセージの中身の上限 */
enum { FX_HELLO = 1, FX_FILES, FX_READ, FX_DATA };
BOOL fx_make_offer(const int *conns, int nconn, HDROP hd, BYTE **out, int *outLen);
BOOL fx_make_offer_paths(const int *conns, int nconn, const WCHAR *paths, BYTE **out, int *outLen);
WCHAR *fx_hdrop_paths(HDROP hd);
HANDLE fx_host_user_token(void);
void fx_request(int conn, const BYTE *p, int n);
void fx_deliver(int conn, const BYTE *p, int n);
void fx_conn_closed(int conn);
void fx_offer_received(int conn, const BYTE *p, int n);
BOOL fx_clipboard_is_ours(void);
void fx_stop(void);
BOOL fx_host_send(int conn, int sub, const BYTE *p, int n);

/* ------------------------------------------------------------------ */
/*  入力(input.c)                                                      */
/* ------------------------------------------------------------------ */

void input_mouse(Client *c, const IivMouse *m);
void input_key(Client *c, const IivKey *k);
void input_sas(Client *c);
void input_release_all(Client *c);

/* ------------------------------------------------------------------ */
/*  クリップボード(clip.c)                                             */
/* ------------------------------------------------------------------ */

void clip_init(HWND hwnd);
void clip_on_update(HWND hwnd);
void clip_set_from_remote(HWND hwnd, WCHAR *text);  /* text は free する */
void clip_get_current(char **utf8, int *len);       /* 今の内容(malloc)。無ければ NULL */

/* ------------------------------------------------------------------ */
/*  画面まわり(ui.c / theme.c)                                         */
/* ------------------------------------------------------------------ */

void ui_show_settings(HWND owner);
BOOL ui_dialog_message(MSG *msg);
BOOL ui_settings_open(void);
void ui_refresh_status(void);
void ui_theme_changed(void);

void     theme_init(void);
BOOL     theme_refresh(void);
BOOL     theme_is_dark(void);
COLORREF theme_back(void);
COLORREF theme_footer(void);
COLORREF theme_ctrl_back(void);
COLORREF theme_text(void);
COLORREF theme_dim_text(void);
COLORREF theme_line(void);
HBRUSH   theme_back_brush(void);
HBRUSH   theme_footer_brush(void);
HBRUSH   theme_ctrl_brush(void);
void     theme_allow_dark(HWND hwnd);
void     theme_apply_dialog(HWND dlg);
LRESULT  theme_ctlcolor(UINT msg, HDC dc, HWND ctl, BOOL dimText);
BOOL     theme_custom_draw_button(NMCUSTOMDRAW *cd, LRESULT *result);

/* ------------------------------------------------------------------ */
/*  サービス(svc.c)                                                    */
/* ------------------------------------------------------------------ */

enum { RUN_NORMAL, RUN_AGENT };
extern int  g_runMode;
extern BOOL g_uiService;        /* 設定画面がサービスの設定(管理者)として開いている */

typedef struct SvcStatus {      /* 分身 → トレイ・設定画面(共有メモリ) */
    LONG  version, seq;
    LONG  listening, port, clients, notifySeq;
    WCHAR method[32];
    WCHAR listen[64];
    WCHAR error[160];
    WCHAR notify[256];
    WCHAR clientList[2048];
} SvcStatus;

int  svc_service_main(void);                /* -service */
int  svc_tray_main(int cmd);                /* -tray */
int  svc_install_cmd(void);                 /* -install-service(管理者) */
BOOL svc_uninstall(HWND owner);             /* 管理者で */
BOOL svc_installed(void);                   /* この ini で登録されているか */
BOOL svc_path_risky(WCHAR *who, int cap);
BOOL svc_run_elevated(const WCHAR *args, HWND owner, BOOL wait, DWORD *exitCode);
BOOL svc_read_status(SvcStatus *st);
void svc_signal_disconnect(void);
void svc_signal_reload(void);
void svc_firewall(BOOL add);
#define FW_RULE L"iiv-server (サービス)"   /* サービスのときに足す規則の名前 */

/* fwrules.c: Windows ファイアウォールの、この exe の規則(iiv-client と同じファイル) */
typedef struct { int count, allow, block; long allowProfiles, blockProfiles; } FwInfo;
BOOL fw_query(const WCHAR *keep, FwInfo *fi);
int  fw_remove(const WCHAR *keep);
int  fw_remove_elevated(HWND owner, const WCHAR *keep, const WCHAR *args);
void fw_describe(const FwInfo *fi, WCHAR *s, int cap);
BOOL agent_init(void);
void agent_com_security(void);
void agent_status_update(void);
void agent_notify(const WCHAR *s);
void agent_request_sas(void);
BOOL agent_follow_input_desktop(void);      /* 呼んだスレッドを入力デスクトップへ。移ったら TRUE */
BOOL agent_input_desktop_changed(void);

/* main.c */
void app_notify(const WCHAR *fmt, ...);
void app_update_tray(void);
void app_listen_addresses(WCHAR *buf, int cap);
const WCHAR *app_listen_error(void);
BOOL app_listening(void);

#endif
