/* ==================================================================
 * clip.c - クリップボードの受け渡し(メインのスレッドで動く)
 *
 *  こちらのクリップボードが変わったら(WM_CLIPBOARDUPDATE)、文字を
 *  UTF-8 で覚えて接続へ知らせる。拡張クリップボードに対応した相手には
 *  「変わった」だけを知らせ、求められたら中身を送る(server.c)。
 *  対応していない相手には Latin-1 で中身を送る。
 *
 *  相手から来た文字は、ここでクリップボードに置く。自分で置いたときの
 *  WM_CLIPBOARDUPDATE は無視する(相手へ送り返さない)。
 *
 *  エクスプローラーでファイルをコピーした(CF_HDROP)ときは、ファイルの一覧を
 *  渡す(filexfer.c。中身は相手が貼り付けたときに送る)。
 *
 *  サービスの分身(SYSTEM)からは、利用者がコピーしたものが見えない(クリップボードの形式が 0 個に
 *  見える。利用者になりすましても同じ。2026-10-05 実測)。そこで分身では自分では読まず、利用者の
 *  権限で動くサービスのトレイが読んで WM_COPYDATA で渡す(clip_text_from_tray、svc.c)。
 *  分身が置く(相手から来た文字・ファイル)のはできる。
 * ================================================================== */

#include "iiv.h"

static CRITICAL_SECTION g_cs;
static char            *g_text;         /* 今の内容(UTF-8、CRLF) */
static int              g_len;
static BOOL             g_ignoreNext;

static void set_current(char *utf8, int len)
{
    EnterCriticalSection(&g_cs);
    free(g_text);
    g_text = utf8;
    g_len = len;
    LeaveCriticalSection(&g_cs);
}

static BOOL open_clipboard(HWND hwnd)
{
    int i;
    for (i = 0; i < 10; i++) {
        if (OpenClipboard(hwnd)) return TRUE;
        Sleep(20);
    }
    return FALSE;
}

static char *read_clipboard(HWND hwnd, int *len)
{
    HANDLE h;
    char  *r = NULL;
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !open_clipboard(hwnd)) return NULL;
    h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const WCHAR *w = (const WCHAR *)GlobalLock(h);
        if (w) {
            r = utf16_to_utf8(w, len);
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return r;
}

void clip_init(HWND hwnd)
{
    int len = 0;
    char *t;
    InitializeCriticalSection(&g_cs);
    AddClipboardFormatListener(hwnd);
    t = read_clipboard(hwnd, &len);
    if (t) set_current(t, len);
}

void clip_on_update(HWND hwnd)
{
    char *t;
    int   len = 0;
    if (g_ignoreNext || GetClipboardOwner() == hwnd) {
        g_ignoreNext = FALSE;
        return;
    }
    if (g_runMode == RUN_AGENT) return;     /* 分身は読めない。サービスのトレイから届く */
    if (fx_clipboard_is_ours()) return;     /* 相手から来たファイルを置いた */
    if (IsClipboardFormatAvailable(CF_HDROP)) {
        if (open_clipboard(hwnd)) {
            HDROP hd = (HDROP)GetClipboardData(CF_HDROP);
            if (hd) server_files_changed(hd);
            CloseClipboard();
        }
        return;
    }
    t = read_clipboard(hwnd, &len);
    if (!t) return;
    if (len > (16 << 20)) { free(t); return; }
    EnterCriticalSection(&g_cs);
    if (g_text && g_len == len && !memcmp(g_text, t, (size_t)len)) {
        LeaveCriticalSection(&g_cs);
        free(t);
        return;
    }
    LeaveCriticalSection(&g_cs);
    server_clipboard_changed(t, len);
    set_current(t, len);
}

void clip_text_from_tray(const WCHAR *text)
{
    int   len = 0;
    char *t = utf16_to_utf8(text, &len);
    if (!t) return;
    if (len > (16 << 20)) { free(t); return; }
    EnterCriticalSection(&g_cs);
    if (g_text && g_len == len && !memcmp(g_text, t, (size_t)len)) {
        LeaveCriticalSection(&g_cs);
        free(t);
        return;
    }
    LeaveCriticalSection(&g_cs);
    server_clipboard_changed(t, len);
    set_current(t, len);
}

void clip_set_from_remote(HWND hwnd, WCHAR *text)
{
    size_t n = wcslen(text);
    HGLOBAL g;
    char *u;
    int ulen;

    u = utf16_to_utf8(text, &ulen);
    if (u) {
        EnterCriticalSection(&g_cs);
        if (g_text && g_len == ulen && !memcmp(g_text, u, (size_t)ulen)) {
            LeaveCriticalSection(&g_cs);
            free(u);
            free(text);
            return;
        }
        LeaveCriticalSection(&g_cs);
    }
    g = GlobalAlloc(GMEM_MOVEABLE, (n + 1) * sizeof(WCHAR));
    if (g) {
        WCHAR *d = (WCHAR *)GlobalLock(g);
        memcpy(d, text, (n + 1) * sizeof(WCHAR));
        GlobalUnlock(g);
        if (open_clipboard(hwnd)) {
            EmptyClipboard();
            if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
            else g_ignoreNext = TRUE;
            CloseClipboard();
        } else {
            GlobalFree(g);
        }
    }
    if (u) set_current(u, ulen);
    free(text);
}

void clip_get_current(char **utf8, int *len)
{
    EnterCriticalSection(&g_cs);
    *utf8 = NULL;
    *len = 0;
    if (g_text) {
        *utf8 = (char *)malloc((size_t)g_len + 1);
        if (*utf8) { memcpy(*utf8, g_text, (size_t)g_len + 1); *len = g_len; }
    }
    LeaveCriticalSection(&g_cs);
}
