/* ==================================================================
 * input.c - 相手から来たキー・マウスの再現
 *
 *  両端とも Windows なので、キーはスキャン コード(拡張 = E0 付き)をそのまま
 *  SendInput する。物理的なキーの位置がそのまま伝わるので、配列の違いや IME の
 *  キー(半角/全角・変換・無変換・カタカナひらがな)で迷わない。スキャン コードで
 *  送りにくいキー(Pause、メディア・ブラウザのキー)は仮想キーで送る。
 *
 *  マウス: 絶対座標で SendInput する。座標の換算は画素の中心を指す
 *  ((2d+1)·65536)/(2w)(iivnc で実測。d·65535/(w-1) は 3840 幅で 1px ずれる点が出た)。
 *
 *  -dryrun のときは再現せずログに書く。
 *
 *  サービスの分身(SYSTEM)として動いているときは、送る前に入力デスクトップ
 *  (ログイン画面・ロック画面・UAC の確認画面は Winlogon)へスレッドを移す。
 *  Ctrl+Alt+Del は SendInput では起きないので、サービスに SendSAS を頼む。
 * ================================================================== */

#include "iiv.h"

#define INJECT_MAGIC 0x11A5C0DE
#define MAX_PRESSED  64

typedef struct InputState {
    int     buttons;
    int     lastX, lastY;
    BOOL    moved;
    WORD    scan[MAX_PRESSED], vk[MAX_PRESSED];
    int     nkeys;
    BOOL    ctrl, alt;
} InputState;

static CRITICAL_SECTION g_cs;
static volatile LONG    g_csInit;
static InputState       g_st[64];   /* 接続 ID の下位 6 ビットで引く */

static InputState *state_of(Client *c)
{
    if (InterlockedCompareExchange(&g_csInit, 1, 0) == 0) InitializeCriticalSection(&g_cs);
    return &g_st[c->id & 63];
}

static void send_inputs(INPUT *in, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (in[i].type == INPUT_MOUSE) in[i].mi.dwExtraInfo = INJECT_MAGIC;
        else in[i].ki.dwExtraInfo = INJECT_MAGIC;
    }
    if (g_runMode == RUN_AGENT) agent_follow_input_desktop();
    if (g_dryRun) {
        for (i = 0; i < n; i++) {
            if (in[i].type == INPUT_MOUSE)
                log_printf(L"[dryrun] mouse flags=%04lX dx=%ld dy=%ld data=%ld", in[i].mi.dwFlags, in[i].mi.dx, in[i].mi.dy, (long)in[i].mi.mouseData);
            else
                log_printf(L"[dryrun] key vk=%02X scan=%02X flags=%lX", in[i].ki.wVk, in[i].ki.wScan, in[i].ki.dwFlags);
        }
        return;
    }
    SendInput((UINT)n, in, sizeof(INPUT));
}

/* ------------------------------------------------------------------ */
/*  マウス                                                              */
/* ------------------------------------------------------------------ */

void input_mouse(Client *c, const IivMouse *m)
{
    static const struct { int bit; DWORD down, up, data; } k_btn[] = {
        { IIV_MB_LEFT, MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP, 0 },
        { IIV_MB_RIGHT, MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP, 0 },
        { IIV_MB_MIDDLE, MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP, 0 },
        { IIV_MB_X1, MOUSEEVENTF_XDOWN, MOUSEEVENTF_XUP, XBUTTON1 },
        { IIV_MB_X2, MOUSEEVENTF_XDOWN, MOUSEEVENTF_XUP, XBUTTON2 },
    };
    InputState *s = state_of(c);
    INPUT in[12];
    int   n = 0, i, changed;

    EnterCriticalSection(&g_cs);
    ZeroMemory(in, sizeof(in));
    if (m->x != s->lastX || m->y != s->lastY || !s->moved) {
        int vl = GetSystemMetrics(SM_XVIRTUALSCREEN), vt = GetSystemMetrics(SM_YVIRTUALSCREEN);
        int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        int px, py, x = m->x, y = m->y;
        AcquireSRWLockShared(&g_scr.lock);
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x >= g_scr.deskW) x = g_scr.deskW - 1;
        if (y >= g_scr.deskH) y = g_scr.deskH - 1;
        px = g_scr.deskX + x;
        py = g_scr.deskY + y;
        ReleaseSRWLockShared(&g_scr.lock);
        if (g_dryRun) { vl = 0; vt = 0; vw = 1920; vh = 1080; }
        in[n].type = INPUT_MOUSE;
        in[n].mi.dx = (LONG)(((LONGLONG)(2 * (px - vl) + 1) * 65536) / (2 * (LONGLONG)vw));
        in[n].mi.dy = (LONG)(((LONGLONG)(2 * (py - vt) + 1) * 65536) / (2 * (LONGLONG)vh));
        in[n].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        n++;
        s->lastX = m->x;
        s->lastY = m->y;
        s->moved = TRUE;
    }
    changed = m->buttons ^ s->buttons;
    for (i = 0; i < (int)ARRAYSIZE(k_btn); i++) {
        if (!(changed & k_btn[i].bit)) continue;
        in[n].type = INPUT_MOUSE;
        in[n].mi.dwFlags = (m->buttons & k_btn[i].bit) ? k_btn[i].down : k_btn[i].up;
        in[n].mi.mouseData = k_btn[i].data;
        n++;
    }
    s->buttons = m->buttons;
    if (m->wheel) { in[n].type = INPUT_MOUSE; in[n].mi.dwFlags = MOUSEEVENTF_WHEEL; in[n].mi.mouseData = (DWORD)(LONG)m->wheel; n++; }
    if (m->hwheel) { in[n].type = INPUT_MOUSE; in[n].mi.dwFlags = MOUSEEVENTF_HWHEEL; in[n].mi.mouseData = (DWORD)(LONG)m->hwheel; n++; }
    if (n) send_inputs(in, n);
    LeaveCriticalSection(&g_cs);
}

/* ------------------------------------------------------------------ */
/*  キー                                                                */
/* ------------------------------------------------------------------ */

static void key_event(INPUT *in, WORD vk, WORD scan, DWORD flags)
{
    ZeroMemory(in, sizeof(*in));
    in->type = INPUT_KEYBOARD;
    in->ki.wVk = vk;
    in->ki.wScan = scan;
    in->ki.dwFlags = flags;
}

/* スキャン コードで送ると確かでないキー */
static BOOL by_vk(WORD vk, WORD scan)
{
    if (vk == VK_PAUSE || scan == 0) return TRUE;
    return (vk >= VK_BROWSER_BACK && vk <= VK_LAUNCH_APP2);
}

void input_key(Client *c, const IivKey *k)
{
    InputState *s = state_of(c);
    INPUT in[2];
    WORD  scan = (WORD)(k->scan & 0xFF), vk = k->vk;
    BOOL  ext = (k->scan & 0x100) != 0;
    DWORD fl;
    int   i;

    if (g_dryRun) log_printf(L"[dryrun-key] %s scan=%X vk=%X", k->down ? L"down" : L"up", k->scan, k->vk);
    EnterCriticalSection(&g_cs);
    if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL || scan == 0x1D) s->ctrl = k->down;
    if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU || scan == 0x38) s->alt = k->down;
    /* Ctrl+Alt+Del: サービスの分身なら SendSAS を頼む */
    if (k->down && scan == 0x53 && s->ctrl && s->alt && g_runMode == RUN_AGENT) {
        agent_request_sas();
        LeaveCriticalSection(&g_cs);
        return;
    }
    if (by_vk(vk, scan)) {
        fl = (ext ? KEYEVENTF_EXTENDEDKEY : 0) | (k->down ? 0 : KEYEVENTF_KEYUP);
        key_event(&in[0], vk, scan, fl);
    } else {
        fl = KEYEVENTF_SCANCODE | (ext ? KEYEVENTF_EXTENDEDKEY : 0) | (k->down ? 0 : KEYEVENTF_KEYUP);
        key_event(&in[0], 0, scan, fl);
    }
    /* 押したままのキーを覚える(切れたときに離すため) */
    for (i = 0; i < s->nkeys; i++) if (s->scan[i] == k->scan && s->vk[i] == vk) break;
    if (k->down && i == s->nkeys && s->nkeys < MAX_PRESSED) { s->scan[s->nkeys] = k->scan; s->vk[s->nkeys] = vk; s->nkeys++; }
    else if (!k->down && i < s->nkeys) { s->nkeys--; s->scan[i] = s->scan[s->nkeys]; s->vk[i] = s->vk[s->nkeys]; }
    send_inputs(in, 1);
    LeaveCriticalSection(&g_cs);
}

void input_sas(Client *c)
{
    (void)c;
    if (g_runMode == RUN_AGENT) agent_request_sas();
    else log_printf(L"Ctrl+Alt+Del は、サービスとして動いているときだけ送れる");
}

void input_release_all(Client *c)
{
    InputState *s = state_of(c);
    INPUT in[MAX_PRESSED + 8];
    int   n = 0, i;
    EnterCriticalSection(&g_cs);
    for (i = 0; i < s->nkeys; i++) {
        WORD scan = (WORD)(s->scan[i] & 0xFF), vk = s->vk[i];
        DWORD ext = (s->scan[i] & 0x100) ? KEYEVENTF_EXTENDEDKEY : 0;
        if (by_vk(vk, scan)) key_event(&in[n++], vk, scan, ext | KEYEVENTF_KEYUP);
        else key_event(&in[n++], 0, scan, KEYEVENTF_SCANCODE | ext | KEYEVENTF_KEYUP);
    }
    if (s->buttons & IIV_MB_LEFT)   { ZeroMemory(&in[n], sizeof(INPUT)); in[n].type = INPUT_MOUSE; in[n++].mi.dwFlags = MOUSEEVENTF_LEFTUP; }
    if (s->buttons & IIV_MB_MIDDLE) { ZeroMemory(&in[n], sizeof(INPUT)); in[n].type = INPUT_MOUSE; in[n++].mi.dwFlags = MOUSEEVENTF_MIDDLEUP; }
    if (s->buttons & IIV_MB_RIGHT)  { ZeroMemory(&in[n], sizeof(INPUT)); in[n].type = INPUT_MOUSE; in[n++].mi.dwFlags = MOUSEEVENTF_RIGHTUP; }
    if (n) {
        send_inputs(in, n);
        log_printf(L"押されたままのキー・ボタン %d 個を離した", n);
    }
    ZeroMemory(s, sizeof(*s));
    LeaveCriticalSection(&g_cs);
}
