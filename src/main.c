/* ==================================================================
 * main.c - 起動、タスクトレイ、多重起動の判定
 *
 *  iiv-server.exe                 起動してトレイに常駐し、待ち受ける
 *  iiv-server.exe -settings       設定画面を開く(動いていればそちらで)
 *  iiv-server.exe -exit           動いている iiv-server を終わらせる(終わるまで待つ)
 *  iiv-server.exe -ini <path>     別の設定ファイルで動かす(多重起動の判定も別)
 *  iiv-server.exe -log            ログを書く(ini の log=1 と同じ)
 *
 *  サービスとして(svc.c):
 *  -install-service    管理者で: サービスとして登録して始める(設定画面のボタンから呼ぶ)
 *  -uninstall-service  管理者で: サービスを止めて登録を消す(ポリシーとファイアウォールも戻す)
 *  -service            サービス本体(SCM から起動される)
 *  -agent              分身(サービスがコンソールのセッションへ SYSTEM で起動する)
 *  -tray               ログインしたユーザーのトレイ(サービスが起動する)
 *  -svcsettings        管理者で: サービスの設定画面
 *  -remove-firewall    管理者で: ファイアウォールの、この exe の規則を消す(設定画面のボタンから呼ぶ。
 *                      終了コードは消した数、-1 = 失敗。サービスとして登録していれば、その規則は残す)
 *  サービスとして登録されているときに、同じ ini でふつうに起動すると -tray になる。
 *
 *  検証用:
 *  -testsrc [static|video]  画面の代わりに合成した絵を出す(利用者の画面を写さない)。
 *                      video は全面が毎フレーム変わる(動画のような)絵。
 *                      入力は再現せずログに書く(-dryrun と同じ)。
 *  -testframes N       -testsrc の絵を N フレーム動かしたら止める
 *  -testfps N          -testsrc の 1 秒あたりのフレーム数(既定 60、0 = 送れるだけ)
 *  -testresize N       -testsrc の N フレーム目で画面を 1280x720 に変える
 *  -testcursor hidden|none  -testsrc のカーソルを隠れた状態にする(none は形も無し)
 *  -testdump <file.bmp>  -testsrc の絵を開いたときに BMP に書く(相手が描いた絵と比べるため)
 *  -dryrun             入力を再現せずログに書く
 *  -gdi                DXGI を使わず GDI で取り込む
 *  -softenc            GPU のエンコーダを使わず、CPU(Windows 標準の H.264 エンコーダ)で符号化する
 *
 *  パスワードが無いときは、127.0.0.1 でしか待ち受けない(listen=127.0.0.1 の
 *  ときだけ動く)。それ以外は設定画面を開いて頼む。
 * ================================================================== */

#include "iiv.h"
#include "resource.h"
#include <shellapi.h>
#include <iphlpapi.h>
#include <stdarg.h>

#define TRAY_ID    1

enum { ID_SETTINGS = 100, ID_DISCONNECT_ALL, ID_EXIT };

HWND  g_mainWnd;
int   g_runMode = RUN_NORMAL;
BOOL  g_dryRun;
int   g_testSrc;
int   g_testFrames;
int   g_testFps = 60;
int   g_testResize;
int   g_testCursor;
BOOL  g_forceGdi;
BOOL  g_forceSoftEnc;
WCHAR g_testDump[MAX_PATH];
HINSTANCE g_inst;

static NOTIFYICONDATAW g_nid;
static UINT            g_wmTaskbarCreated;
static HICON           g_icoIdle, g_icoActive;
static BOOL            g_listening;
static WCHAR           g_listenError[160];

/* ------------------------------------------------------------------ */
/*  通知                                                                */
/* ------------------------------------------------------------------ */

void app_notify(const WCHAR *fmt, ...)
{
    WCHAR  *s = (WCHAR *)malloc(256 * sizeof(WCHAR));
    va_list ap;
    if (!s) return;
    va_start(ap, fmt);
    _vsnwprintf(s, 255, fmt, ap);
    va_end(ap);
    s[255] = 0;
    if (!g_mainWnd || !PostMessageW(g_mainWnd, WM_APP_NOTIFY, 0, (LPARAM)s)) free(s);
}

void app_listen_addresses(WCHAR *buf, int cap)
{
    ULONG size = 16384;
    IP_ADAPTER_ADDRESSES *aa, *a;
    int used = 0;
    static BOOL wsa;
    buf[0] = 0;
    if (!wsa) {                         /* 設定画面だけのプロセスでも WSAAddressToString を使えるように */
        WSADATA wd;
        wsa = WSAStartup(MAKEWORD(2, 2), &wd) == 0;
    }
    aa = (IP_ADAPTER_ADDRESSES *)malloc(size);
    if (!aa) return;
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, NULL, aa, &size) == ERROR_BUFFER_OVERFLOW) {
        free(aa);
        aa = (IP_ADAPTER_ADDRESSES *)malloc(size);
        if (!aa || GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, NULL, aa, &size)) { free(aa); return; }
    }
    for (a = aa; a; a = a->Next) {
        IP_ADAPTER_UNICAST_ADDRESS *u;
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (u = a->FirstUnicastAddress; u; u = u->Next) {
            WCHAR ip[64];
            DWORD n = ARRAYSIZE(ip);
            int   k;
            if (WSAAddressToStringW(u->Address.lpSockaddr, (DWORD)u->Address.iSockaddrLength, NULL, ip, &n)) continue;
            if (!wcsncmp(ip, L"169.254.", 8)) continue;
            k = _snwprintf(buf + used, (size_t)(cap - used), L"%s%s", used ? L", " : L"", ip);
            if (k < 0) break;
            used += k;
        }
    }
    free(aa);
}

/* ------------------------------------------------------------------ */
/*  トレイ                                                              */
/* ------------------------------------------------------------------ */

void app_update_tray(void)
{
    int n = (int)g_clientCount;
    if (g_runMode == RUN_AGENT) { agent_status_update(); return; }
    g_nid.uFlags = NIF_ICON | NIF_TIP;
    g_nid.hIcon = n ? g_icoActive : g_icoIdle;
    if (!g_listening) _snwprintf(g_nid.szTip, ARRAYSIZE(g_nid.szTip), L"iiv-server - 待ち受けていません");
    else if (n) _snwprintf(g_nid.szTip, ARRAYSIZE(g_nid.szTip), L"iiv-server - ポート %d、%d 人が接続中", g_cfg.port, n);
    else _snwprintf(g_nid.szTip, ARRAYSIZE(g_nid.szTip), L"iiv-server - ポート %d で待ち受け中", g_cfg.port);
    g_nid.szTip[ARRAYSIZE(g_nid.szTip) - 1] = 0;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static BOOL g_trayV4;           /* 新しい形式(NOTIFYICON_VERSION_4)にできた */

static void tray_add(void)
{
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_mainWnd;
    g_nid.uID = TRAY_ID;
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_nid.hIcon = g_icoIdle;
    lstrcpyW(g_nid.szTip, L"iiv-server");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_nid.uVersion = NOTIFYICON_VERSION_4;
    g_trayV4 = Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
    app_update_tray();
}

static void tray_menu(void)
{
    HMENU m = CreatePopupMenu();
    WCHAR list[2048], *p, *e;
    POINT pt;
    int   n = server_list(list, ARRAYSIZE(list));

    AppendMenuW(m, MF_STRING, ID_SETTINGS, L"設定(&S)...");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    if (n) {
        for (p = list; p && *p; p = e) {
            e = wcschr(p, L'\n');
            if (e) *e++ = 0;
            AppendMenuW(m, MF_STRING | MF_GRAYED, 0, p);
        }
        AppendMenuW(m, MF_STRING, ID_DISCONNECT_ALL, L"全員を切断(&D)");
    } else {
        AppendMenuW(m, MF_STRING | MF_GRAYED, 0, g_listening ? L"接続はありません" : L"待ち受けていません");
    }
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"終了(&X)");
    SetMenuDefaultItem(m, ID_SETTINGS, FALSE);
    GetCursorPos(&pt);
    SetForegroundWindow(g_mainWnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_mainWnd, NULL);
    PostMessageW(g_mainWnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

/* パスワードが無いのに外から受けようとしていないか */
static BOOL may_listen(void)
{
    if (g_cfg.pw.set || g_cfg.viewPw.set) return TRUE;
    return !lstrcmpW(g_cfg.listen, L"127.0.0.1") || !lstrcmpW(g_cfg.listen, L"::1");
}

static void start_listening(void)
{
    g_listenError[0] = 0;
    if (!may_listen()) {
        lstrcpyW(g_listenError, L"パスワードが設定されていないので待ち受けていません。");
        g_listening = FALSE;
    } else {
        g_listening = server_start();
        if (!g_listening) _snwprintf(g_listenError, ARRAYSIZE(g_listenError), L"ポート %d で待ち受けられません(ほかのソフトが使っていませんか)。", g_cfg.port);
    }
    app_update_tray();
    ui_refresh_status();
}

const WCHAR *app_listen_error(void) { return g_listenError; }
BOOL app_listening(void) { return g_listening; }

/* ------------------------------------------------------------------ */
/*  スリープさせない(nosleep=1)                                         */
/* ------------------------------------------------------------------ */

/* Windows の電源の要求(PowerSetRequest)で、スリープと画面の消灯を止める。
   理由の文字列を付けるので、powercfg /requests で誰が止めているか見える */
static void keep_awake(BOOL on, const WCHAR *reason)
{
    static HANDLE req;
    static BOOL   active;
    if (on == active) return;
    if (!req) {
        REASON_CONTEXT rc;
        ZeroMemory(&rc, sizeof(rc));
        rc.Version = POWER_REQUEST_CONTEXT_VERSION;
        rc.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
        rc.Reason.SimpleReasonString = (LPWSTR)reason;
        req = PowerCreateRequest(&rc);
        if (req == INVALID_HANDLE_VALUE) req = NULL;
        if (!req) { log_printf(L"スリープを止められない (%lu)", GetLastError()); return; }
    }
    if (on) {
        PowerSetRequest(req, PowerRequestSystemRequired);
        PowerSetRequest(req, PowerRequestDisplayRequired);
    } else {
        PowerClearRequest(req, PowerRequestSystemRequired);
        PowerClearRequest(req, PowerRequestDisplayRequired);
    }
    active = on;
    log_printf(on ? L"スリープと画面の消灯を止めた" : L"スリープと画面の消灯の止めを解いた");
}

void power_update(void)
{
    keep_awake(g_cfg.noSleep && g_clientCount > 0, L"iiv-server: ビューアが接続している");
}

static LRESULT CALLBACK main_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_APP_TRAY:
        switch (LOWORD(lp)) {
        /* 新しい形式では、左クリックで WM_LBUTTONUP と NIN_SELECT、右クリックで WM_RBUTTONUP と
           WM_CONTEXTMENU の両方が来る。片方だけで動く */
        case WM_LBUTTONUP:
            if (g_trayV4) break;
            /* fall through */
        case NIN_SELECT:
        case NIN_KEYSELECT:
            ui_show_settings(NULL);
            break;
        case WM_RBUTTONUP:
            if (g_trayV4) break;
            /* fall through */
        case WM_CONTEXTMENU:
            tray_menu();
            break;
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_SETTINGS:       ui_show_settings(NULL); break;
        case ID_DISCONNECT_ALL: server_disconnect_all(); break;
        case ID_EXIT:           DestroyWindow(hwnd); break;
        }
        return 0;

    case WM_APP_COMMAND:
        if (wp == CMD_EXIT) DestroyWindow(hwnd);
        else ui_show_settings(NULL);
        return 0;

    case WM_APP_CLIENTS:
        app_update_tray();
        ui_refresh_status();
        power_update();
        return 0;

    case WM_APP_NOTIFY: {
        WCHAR *s = (WCHAR *)lp;
        if (g_runMode == RUN_AGENT) { agent_notify(s); free(s); return 0; }
        g_nid.uFlags = NIF_INFO;
        lstrcpynW(g_nid.szInfo, s, ARRAYSIZE(g_nid.szInfo));
        lstrcpyW(g_nid.szInfoTitle, L"iiv-server");
        g_nid.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
        g_nid.szInfo[0] = 0;
        free(s);
        return 0;
    }

    case WM_APP_RESTART:
        server_stop();
        start_listening();
        return 0;

    case WM_APP_RELOAD: {               /* 分身: 管理者の設定画面で変わった */
        Config old = g_cfg;
        config_load();
        log_printf(L"[agent] 設定を読み直した");
        power_update();
        if (old.port != g_cfg.port || lstrcmpW(old.listen, g_cfg.listen) || (!g_listening && may_listen())) {
            server_stop();
            start_listening();
        }
        if (old.display != g_cfg.display || old.kbps != g_cfg.kbps) video_reset();
        agent_status_update();
        return 0;
    }

    case WM_APP_DISCONNECT:
        server_disconnect_all();
        return 0;

    case WM_TIMER:                      /* 分身: 待ち受けられなかったら、やり直す */
        if (!g_listening && may_listen()) start_listening();
        else agent_status_update();
        return 0;

    case WM_APP_SETCLIP:
        clip_set_from_remote(hwnd, (WCHAR *)lp);
        return 0;

    case WM_APP_FXOFFER:
        server_fx_offer_current((int)wp);
        return 0;

    case WM_COPYDATA: {                 /* 分身: サービスのトレイが読んだ、利用者のクリップボード */
        const COPYDATASTRUCT *cd = (const COPYDATASTRUCT *)lp;
        if (g_runMode != RUN_AGENT || !cd || !cd->lpData || cd->cbData < 2 * sizeof(WCHAR) || (cd->cbData & 1)) return FALSE;
        {
            size_t n = cd->cbData / sizeof(WCHAR);
            WCHAR *w = (WCHAR *)malloc((n + 2) * sizeof(WCHAR));
            if (!w) return FALSE;
            memcpy(w, cd->lpData, cd->cbData);
            w[n] = w[n + 1] = 0;            /* 終わりを必ず付ける */
            if (cd->dwData == CD_CLIP_TEXT) clip_text_from_tray(w);
            else if (cd->dwData == CD_CLIP_FILES) server_files_changed_paths(w);
            free(w);
        }
        return TRUE;
    }

    case WM_CLIPBOARDUPDATE:
        clip_on_update(hwnd);
        return 0;

    case WM_SETTINGCHANGE:
        if (lp && !lstrcmpW((const WCHAR *)lp, L"ImmersiveColorSet") && theme_refresh()) ui_theme_changed();
        return 0;

    case WM_DESTROY:
        if (g_runMode != RUN_AGENT) Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    if (msg == g_wmTaskbarCreated && msg && g_runMode != RUN_AGENT) {     /* エクスプローラが再起動した */
        tray_add();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
/*  起動                                                                */
/* ------------------------------------------------------------------ */

static void mutex_name(WCHAR *out)
{
    WCHAR low[MAX_PATH];
    DWORD h = 2166136261u;
    int   i;
    lstrcpynW(low, g_iniPath, MAX_PATH);
    CharLowerW(low);
    for (i = 0; low[i]; i++) { h ^= low[i]; h *= 16777619u; }
    wsprintfW(out, L"Local\\iiv-server-%08lX", h);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdline, int show)
{
    WCHAR   mname[64];
    HANDLE  mutex = NULL;
    WNDCLASSW wc;
    MSG     msg;
    LPWSTR *argv;
    int     argc, i, cmd = 0;
    BOOL    openSettings = FALSE, first, logArg = FALSE;
    int     svcMode = 0;            /* 1 = -service、2 = -agent、3 = -tray、4 = -svcsettings、5 = -install-service、6 = -uninstall-service、7 = -remove-firewall */
    INITCOMMONCONTROLSEX icc;

    (void)prev; (void)cmdline; (void)show;
    g_inst = inst;

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (i = 1; argv && i < argc; i++) {
        const WCHAR *a = argv[i];
        if (a[0] == L'/') a++;
        else if (a[0] == L'-') { a++; if (a[0] == L'-') a++; }
        if (!lstrcmpiW(a, L"ini") && i + 1 < argc) GetFullPathNameW(argv[++i], MAX_PATH, g_iniPath, NULL);
        else if (!lstrcmpiW(a, L"log")) logArg = TRUE;
        else if (!lstrcmpiW(a, L"exit")) cmd = CMD_EXIT;
        else if (!lstrcmpiW(a, L"settings")) openSettings = TRUE;
        else if (!lstrcmpiW(a, L"dryrun")) g_dryRun = TRUE;
        else if (!lstrcmpiW(a, L"gdi")) g_forceGdi = TRUE;
        else if (!lstrcmpiW(a, L"softenc")) g_forceSoftEnc = TRUE;
        else if (!lstrcmpiW(a, L"service")) svcMode = 1;
        else if (!lstrcmpiW(a, L"agent")) svcMode = 2;
        else if (!lstrcmpiW(a, L"tray")) svcMode = 3;
        else if (!lstrcmpiW(a, L"svcsettings")) svcMode = 4;
        else if (!lstrcmpiW(a, L"install-service")) svcMode = 5;
        else if (!lstrcmpiW(a, L"uninstall-service")) svcMode = 6;
        else if (!lstrcmpiW(a, L"remove-firewall")) svcMode = 7;
        else if (!lstrcmpiW(a, L"testsrc")) {
            g_testSrc = 1;
            g_dryRun = TRUE;
            if (i + 1 < argc && !lstrcmpiW(argv[i + 1], L"static")) { g_testSrc = 2; i++; }
            else if (i + 1 < argc && !lstrcmpiW(argv[i + 1], L"video")) { g_testSrc = 3; i++; }
        }
        else if (!lstrcmpiW(a, L"testframes") && i + 1 < argc) g_testFrames = _wtoi(argv[++i]);
        else if (!lstrcmpiW(a, L"testfps") && i + 1 < argc) g_testFps = _wtoi(argv[++i]);
        else if (!lstrcmpiW(a, L"testresize") && i + 1 < argc) g_testResize = _wtoi(argv[++i]);
        else if (!lstrcmpiW(a, L"testcursor") && i + 1 < argc) g_testCursor = !lstrcmpiW(argv[++i], L"none") ? 2 : 1;
        else if (!lstrcmpiW(a, L"testdump") && i + 1 < argc) GetFullPathNameW(argv[++i], MAX_PATH, g_testDump, NULL);
    }
    if (argv) LocalFree(argv);
    config_init();

    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    switch (svcMode) {
    case 1:                             /* サービス本体 */
        config_load();
        if (logArg) g_cfg.log = TRUE;
        log_open();
        return svc_service_main();
    case 3:                             /* トレイ */
        return svc_tray_main(cmd);
    case 4: {                           /* サービスの設定画面(管理者) */
        /* すでに開いていれば、それを手前に出して終わる(アイコンを続けて押したときなど) */
        HANDLE one = CreateMutexW(NULL, FALSE, L"Local\\iiv-server-svcsettings");
        if (one && GetLastError() == ERROR_ALREADY_EXISTS) {
            HWND other = FindWindowW(L"#32770", L"iiv-server");
            if (other) {
                if (IsIconic(other)) ShowWindow(other, SW_RESTORE);
                SetForegroundWindow(other);
            }
            return 0;
        }
        config_load();
        log_open();
        theme_init();
        g_uiService = TRUE;
        ui_show_settings(NULL);
        while (GetMessageW(&msg, NULL, 0, 0) > 0) {
            if (ui_dialog_message(&msg)) continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return 0;
    }
    case 5:                             /* 登録(管理者) */
        config_load();
        if (logArg) g_cfg.log = TRUE;
        log_open();
        return svc_install_cmd();
    case 6:                             /* 解除(管理者) */
        config_load();
        if (logArg) g_cfg.log = TRUE;
        log_open();
        return svc_uninstall(NULL) ? 0 : 2;
    case 7:                             /* ファイアウォールの規則を消す(管理者) */
        config_load();
        if (logArg) g_cfg.log = TRUE;
        log_open();
        return fw_remove(svc_installed() ? FW_RULE : NULL);
    case 2:
        g_runMode = RUN_AGENT;
        break;
    default:
        /* この ini でサービスとして登録されているなら、トレイだけを出す */
        if (!g_testSrc && svc_installed()) return svc_tray_main(cmd);
    }

    /* 同じ設定ファイルで動いているものがあれば、そちらに頼んで終わる(分身は除く) */
    if (g_runMode == RUN_AGENT) goto skip_mutex;
    mutex_name(mname);
    mutex = CreateMutexW(NULL, FALSE, mname);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(TRAY_CLASS, g_iniPath);
        if (other) {
            DWORD  pid = 0;
            HANDLE proc;
            GetWindowThreadProcessId(other, &pid);
            AllowSetForegroundWindow(pid);
            proc = cmd == CMD_EXIT ? OpenProcess(SYNCHRONIZE, FALSE, pid) : NULL;
            PostMessageW(other, WM_APP_COMMAND, (WPARAM)cmd, 0);
            /* -exit は相手が終わるまで待つ(続けて起動し直すスクリプトのため) */
            if (proc) {
                WaitForSingleObject(proc, 10000);
                CloseHandle(proc);
            }
        }
        if (mutex) CloseHandle(mutex);
        return 0;
    }
    if (cmd) {                      /* -exit の相手がいない */
        if (mutex) CloseHandle(mutex);
        return 0;
    }
skip_mutex:

    first = GetFileAttributesW(g_iniPath) == INVALID_FILE_ATTRIBUTES;
    config_load();
    if (logArg) g_cfg.log = TRUE;
    log_open();
    theme_init();
    if (first) config_save();
    log_printf(L"iiv-server %s 起動 (設定 %s)%s%s%s", APP_VERSION, g_iniPath, g_runMode == RUN_AGENT ? L" [agent]" : L"",
               g_testSrc ? L" 検証用の絵" : L"", g_dryRun ? L" 入力はログだけ" : L"");

    g_icoIdle   = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    g_icoActive = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_ACTIVE), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);

    g_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = main_proc;
    wc.hInstance = inst;
    wc.lpszClassName = TRAY_CLASS;
    RegisterClassW(&wc);
    /* 別のインスタンスが探せるよう、タイトルに設定ファイルのパスを入れておく */
    g_mainWnd = CreateWindowExW(WS_EX_TOOLWINDOW, TRAY_CLASS, g_runMode == RUN_AGENT ? AGENT_TITLE : g_iniPath,
                                WS_POPUP, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    if (!g_mainWnd) return 1;
    if (g_runMode == RUN_AGENT) {
        agent_com_security();
        agent_init();
        ChangeWindowMessageFilterEx(g_mainWnd, WM_COPYDATA, MSGFLT_ALLOW, NULL);   /* 利用者の権限のトレイから */
        SetTimer(g_mainWnd, 1, 5000, NULL);
    } else {
        ChangeWindowMessageFilterEx(g_mainWnd, g_wmTaskbarCreated, MSGFLT_ALLOW, NULL);
        ChangeWindowMessageFilterEx(g_mainWnd, WM_APP_COMMAND, MSGFLT_ALLOW, NULL);
        theme_allow_dark(g_mainWnd);
        tray_add();
    }

    video_init();
    clip_init(g_mainWnd);
    start_listening();
    if (g_runMode != RUN_AGENT) {
        if (!g_listening && !may_listen()) openSettings = TRUE;
        if (openSettings) ui_show_settings(NULL);
    }

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (ui_dialog_message(&msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    server_stop();
    video_shutdown();
    log_printf(L"iiv-server 終了");
    if (mutex) CloseHandle(mutex);
    return 0;
}
