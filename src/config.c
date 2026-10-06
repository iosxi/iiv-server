/* ==================================================================
 * config.c - iiv-server.ini の読み書きとログ
 *
 *  レジストリは使わない。設定は exe と同じ場所の iiv-server.ini
 *  (UTF-8 のテキスト)だけ。-ini で別のファイルを指定できる。
 *
 *  パスワードはそのものを置かず、PBKDF2-SHA256 で導いた鍵とソルトを置く(auth.c)。
 *
 *  ログは log=1 のときだけ、ini と同じ場所の iiv-server.log に書く。
 * ================================================================== */

#include "iiv.h"
#include <stdio.h>
#include <stdarg.h>

Config g_cfg;
WCHAR  g_iniPath[MAX_PATH];
WCHAR  g_exeDir[MAX_PATH];

static CRITICAL_SECTION g_logCs;
static HANDLE           g_logFile = INVALID_HANDLE_VALUE;

void config_init(void)
{
    WCHAR *p;
    GetModuleFileNameW(NULL, g_exeDir, MAX_PATH);
    p = wcsrchr(g_exeDir, L'\\');
    if (p) *p = 0;
    if (!g_iniPath[0]) wsprintfW(g_iniPath, L"%s\\iiv-server.ini", g_exeDir);
    InitializeCriticalSection(&g_logCs);

    ZeroMemory(&g_cfg, sizeof(g_cfg));
    g_cfg.port   = IIV_DEFAULT_PORT;
    g_cfg.notify = 1;
    g_cfg.showCursor = 1;
    g_cfg.noSleep = 1;
    g_cfg.sasBefore = -2;
    g_cfg.qMove = 70;
    g_cfg.qStill = 95;
    g_cfg.stillMs = 150;
    g_cfg.speed = 0;               /* 4K で 符号化 11.4 → 9.2ms(2026-10-06 実測)。帯域はむしろ少し減った */
}

/* ------------------------------------------------------------------ */
/*  文字コード                                                          */
/* ------------------------------------------------------------------ */

char *utf16_to_utf8(const WCHAR *s, int *outLen)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    char *r = (char *)malloc(n > 0 ? (size_t)n : 1);
    if (!r) return NULL;
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, s, -1, r, n, NULL, NULL);
    else r[0] = 0;
    if (outLen) *outLen = n > 0 ? n - 1 : 0;
    return r;
}

WCHAR *utf8_to_utf16(const char *s, int len)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, len, NULL, 0);
    WCHAR *r = (WCHAR *)malloc(((size_t)n + 1) * sizeof(WCHAR));
    if (!r) return NULL;
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s, len, r, n);
    r[n > 0 ? n : 0] = 0;
    return r;
}

/* ------------------------------------------------------------------ */
/*  読み込み                                                            */
/* ------------------------------------------------------------------ */

static char *read_all(const WCHAR *path)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    DWORD  n, got = 0;
    char  *p;
    if (f == INVALID_HANDLE_VALUE) return NULL;
    n = GetFileSize(f, NULL);
    if (n == INVALID_FILE_SIZE || n > (1 << 20)) { CloseHandle(f); return NULL; }
    p = (char *)malloc((size_t)n + 1);
    if (p && !ReadFile(f, p, n, &got, NULL)) got = 0;
    CloseHandle(f);
    if (!p) return NULL;
    p[got] = 0;
    return p;
}

void config_load(void)
{
    char *text = read_all(g_iniPath), *line, *next;
    if (!text) return;
    for (line = text; line && *line; line = next) {
        char *eq, *key, *val, *e;
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        if ((e = strchr(line, '\r')) != NULL) *e = 0;
        if ((unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF) line += 3;
        while (*line == ' ' || *line == '\t') line++;
        if (*line == ';' || *line == '#' || *line == '[' || !*line) continue;
        eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        key = line;
        val = eq + 1;
        for (e = eq - 1; e >= key && (*e == ' ' || *e == '\t'); e--) *e = 0;
        while (*val == ' ' || *val == '\t') val++;
        for (e = val + strlen(val) - 1; e >= val && (*e == ' ' || *e == '\t'); e--) *e = 0;

        if (!_stricmp(key, "port")) g_cfg.port = atoi(val);
        else if (!_stricmp(key, "listen")) MultiByteToWideChar(CP_UTF8, 0, val, -1, g_cfg.listen, ARRAYSIZE(g_cfg.listen));
        else if (!_stricmp(key, "password")) auth_from_text(&g_cfg.pw, val);
        else if (!_stricmp(key, "viewpassword")) auth_from_text(&g_cfg.viewPw, val);
        else if (!_stricmp(key, "kbps")) g_cfg.kbps = atoi(val);
        else if (!_stricmp(key, "qmove")) g_cfg.qMove = atoi(val);
        else if (!_stricmp(key, "qstill")) g_cfg.qStill = atoi(val);
        else if (!_stricmp(key, "stillms")) g_cfg.stillMs = atoi(val);
        else if (!_stricmp(key, "speed")) g_cfg.speed = atoi(val);
        else if (!_stricmp(key, "viewonly")) g_cfg.viewOnly = atoi(val) != 0;
        else if (!_stricmp(key, "display")) g_cfg.display = atoi(val);
        else if (!_stricmp(key, "notify")) g_cfg.notify = atoi(val) != 0;
        else if (!_stricmp(key, "showcursor")) g_cfg.showCursor = atoi(val) != 0;
        else if (!_stricmp(key, "nosleep")) g_cfg.noSleep = atoi(val) != 0;
        else if (!_stricmp(key, "fxoffer")) g_cfg.fxOffer = atoi(val) != 0;
        else if (!_stricmp(key, "maxfps")) g_cfg.maxFps = atoi(val);
        else if (!_stricmp(key, "theme")) g_cfg.theme = !_stricmp(val, "light") ? 1 : !_stricmp(val, "dark") ? 2 : 0;
        else if (!_stricmp(key, "log")) g_cfg.log = atoi(val) != 0;
        else if (!_stricmp(key, "sas_before")) g_cfg.sasBefore = atoi(val);
        else if (!_stricmp(key, "selftest")) g_cfg.selftest = atoi(val) != 0;
    }
    free(text);
    if (g_cfg.port <= 0 || g_cfg.port > 65535) g_cfg.port = IIV_DEFAULT_PORT;
    if (g_cfg.kbps < 0) g_cfg.kbps = 0;
    if (g_cfg.qMove < 1 || g_cfg.qMove > 100) g_cfg.qMove = 70;
    if (g_cfg.qStill < 1 || g_cfg.qStill > 100) g_cfg.qStill = 95;
    if (g_cfg.stillMs < 0) g_cfg.stillMs = 150;
    if (g_cfg.display < 0) g_cfg.display = 0;
    if (g_cfg.maxFps < 0) g_cfg.maxFps = 0;
}

/* ------------------------------------------------------------------ */
/*  保存(書いてから置き換える)                                          */
/* ------------------------------------------------------------------ */

BOOL config_save(void)
{
    char   buf[4096], pw[160], vpw[160], *listen;
    WCHAR  tmp[MAX_PATH + 8];
    HANDLE f;
    DWORD  wr;
    BOOL   ok;
    int    n;

    auth_to_text(&g_cfg.pw, pw, sizeof(pw));
    auth_to_text(&g_cfg.viewPw, vpw, sizeof(vpw));
    listen = utf16_to_utf8(g_cfg.listen, NULL);
    n = _snprintf(buf, sizeof(buf),
        "; iiv-server の設定(UTF-8)。画面の「設定」で変えられる。\n"
        "[server]\n"
        "; 待ち受けるポート\n"
        "port=%d\n"
        "; 待ち受けるアドレス。空 = すべて、127.0.0.1 = この PC からだけ\n"
        "listen=%s\n"
        "; パスワード(そのものではなく、PBKDF2-SHA256 で導いた鍵。画面から設定する)\n"
        "password=%s\n"
        "; 見るだけのパスワード(空なら無し)\n"
        "viewpassword=%s\n"
        "; 1 = 誰にも操作させない(見るだけ)\n"
        "viewonly=%d\n"
        "; 0 = すべての画面、n = ディスプレイ n だけ\n"
        "display=%d\n"
        "; 1 = 接続・切断を通知で知らせる\n"
        "notify=%d\n"
        "; 1 = Windows がカーソルを隠していても(マウスが無い PC など)、相手にカーソルを見せる\n"
        "showcursor=%d\n"
        "; 1 = 接続されている間はスリープさせず、画面も消さない\n"
        "nosleep=%d\n"
        "; 映像のビットレート(kbps)の上限。0 = 決めない(画質で決める。おすすめ)\n"
        "kbps=%d\n"
        "; 動いている間の画質(1〜100)と、止まったら送り直す画質。止まってから送り直すまで(ms)\n"
        "qmove=%d\n"
        "qstill=%d\n"
        "stillms=%d\n"
        "; 1 秒あたりの更新の上限。0 = 画面の書き換えの速さまで\n"
        "maxfps=%d\n"
        "\n[general]\n"
        "; system / light / dark\n"
        "theme=%s\n"
        "; 1 = iiv-server.log に動作の記録を書く\n"
        "log=%d\n"
        "\n[service]\n"
        "; サービスとして登録する前の SoftwareSASGeneration(解除のときに戻す。-1 = 無かった、-2 = 控えていない)\n"
        "sas_before=%d\n",
        g_cfg.port, listen ? listen : "", pw, vpw, g_cfg.viewOnly, g_cfg.display, g_cfg.notify, g_cfg.showCursor, g_cfg.noSleep, g_cfg.kbps, g_cfg.qMove, g_cfg.qStill, g_cfg.stillMs, g_cfg.maxFps,
        g_cfg.theme == 1 ? "light" : g_cfg.theme == 2 ? "dark" : "system", g_cfg.log, g_cfg.sasBefore);
    free(listen);
    if (n < 0) return FALSE;

    wsprintfW(tmp, L"%s.tmp", g_iniPath);
    f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return FALSE;
    ok = WriteFile(f, buf, (DWORD)n, &wr, NULL) && wr == (DWORD)n;
    ok = FlushFileBuffers(f) && ok;
    CloseHandle(f);
    if (ok) ok = MoveFileExW(tmp, g_iniPath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) {
        DeleteFileW(tmp);
        log_printf(L"設定を保存できませんでした: %s (%lu)", g_iniPath, GetLastError());
    }
    return ok;
}

/* ------------------------------------------------------------------ */
/*  ログ                                                                */
/* ------------------------------------------------------------------ */

void log_open(void)
{
    WCHAR path[MAX_PATH], *p;
    if (!g_cfg.log || g_logFile != INVALID_HANDLE_VALUE) return;
    lstrcpynW(path, g_iniPath, MAX_PATH - 8);
    p = wcsrchr(path, L'.');
    if (p) *p = 0;
    lstrcatW(path, L".log");
    g_logFile = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
}

static void log_write_utf8(const char *s, int n)
{
    SYSTEMTIME t;
    char head[40];
    DWORD wr;
    GetLocalTime(&t);
    sprintf(head, "%04d-%02d-%02d %02d:%02d:%02d.%03d ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    EnterCriticalSection(&g_logCs);
    WriteFile(g_logFile, head, (DWORD)strlen(head), &wr, NULL);
    WriteFile(g_logFile, s, (DWORD)n, &wr, NULL);
    WriteFile(g_logFile, "\r\n", 2, &wr, NULL);
    LeaveCriticalSection(&g_logCs);
}

void log_printf(const WCHAR *fmt, ...)
{
    WCHAR   w[1024];
    char   *u;
    int     n;
    va_list ap;
    if (g_logFile == INVALID_HANDLE_VALUE) return;
    va_start(ap, fmt);
    _vsnwprintf(w, ARRAYSIZE(w) - 1, fmt, ap);
    va_end(ap);
    w[ARRAYSIZE(w) - 1] = 0;
    u = utf16_to_utf8(w, &n);
    if (u) { log_write_utf8(u, n); free(u); }
}

void log_printfA(const char *fmt, ...)
{
    char    s[1024];
    va_list ap;
    if (g_logFile == INVALID_HANDLE_VALUE) return;
    va_start(ap, fmt);
    _vsnprintf(s, sizeof(s) - 1, fmt, ap);
    va_end(ap);
    s[sizeof(s) - 1] = 0;
    log_write_utf8(s, (int)strlen(s));
}
