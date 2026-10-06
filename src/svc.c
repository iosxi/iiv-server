/* ==================================================================
 * svc.c - サービスとして動かす(ログイン前・ロック中・UAC の確認画面でも)
 *
 *  サービスはセッション 0 で動くので、画面を取り込めない。そこで
 *  TightVNC / UltraVNC などと同じく 3 つに分ける。どれも同じ exe。
 *
 *   -service  サービス本体(SYSTEM、セッション 0)。画面のあるセッション
 *             (コンソール)へ「分身」を SYSTEM のまま送り込んで見張る。
 *             ログイン・ログオフ・ユーザーの切り替えで、送り込み直す。
 *             ログインしたユーザーには、そのユーザーの権限でトレイを出す。
 *             分身から頼まれたら SendSAS で Ctrl+Alt+Del を起こす。
 *   -agent    分身(SYSTEM、コンソールのセッション)。中身はふだんの
 *             iiv-server(取り込み・符号化・通信)で、トレイは出さない。
 *             取り込みと入力のスレッドは入力デスクトップ(ログイン画面・
 *             ロック画面・UAC の確認画面は Winlogon、ふだんは Default)へ
 *             付いて行く。状態は共有メモリに書く。
 *   -tray     ログインしたユーザーの権限で動くトレイ。共有メモリから状態を
 *             読んで、アイコン・通知を出す。設定は管理者として開く。
 *
 *  名前付きの物(すべて Global\。アクセス権は SDDL で決める)
 *   iiv-server-status      状態(分身が書き、ユーザーは読むだけ)
 *   iiv-server-agent-stop  分身を止める(サービスが作る)
 *   iiv-server-sas         Ctrl+Alt+Del を頼む(サービスが作る。SYSTEM と管理者だけ)
 *   iiv-server-reload      設定を読み直す(分身が作る。管理者だけ)
 *   iiv-server-disconnect  全員を切断(分身が作る。ログインしているユーザーも)
 *
 *  登録(-install-service、管理者)で行うこと
 *   - サービスの登録(自動開始、落ちたら 5 秒後に起動し直す)
 *   - Ctrl+Alt+Del を送れるようにするポリシー SoftwareSASGeneration を
 *     「サービスから」に設定する。元の値は ini の sas_before に控え、
 *     解除のときに戻す(アプリ自身がレジストリに書く唯一の場所)
 *   - Windows ファイアウォールに、この exe のポートへの受信を許可する規則を足す
 *     (ログイン前は確認の画面を出せないため)
 * ================================================================== */

#include "iiv.h"
#include "resource.h"
#include <shellapi.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <sddl.h>
#include <aclapi.h>
#include <netfw.h>

#define SVC_NAME        L"iiv-server"
#define SVC_DISPLAY     L"iiv-server (リモート デスクトップ)"
#define OBJ_STATUS      L"Global\\iiv-server-status"
#define OBJ_AGENT_STOP  L"Global\\iiv-server-agent-stop"
#define OBJ_SAS         L"Global\\iiv-server-sas"
#define OBJ_RELOAD      L"Global\\iiv-server-reload"
#define OBJ_DISCONNECT  L"Global\\iiv-server-disconnect"
#define STATUS_VERSION  1

#define SAS_KEY  L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System"
#define SAS_NAME L"SoftwareSASGeneration"

BOOL g_uiService;

static void exe_path(WCHAR *out)
{
    GetModuleFileNameW(NULL, out, MAX_PATH);
}

/* SDDL から作ったセキュリティ記述子(プロセスが終わるまで持つ) */
static SECURITY_ATTRIBUTES *make_sa(SECURITY_ATTRIBUTES *sa, const WCHAR *sddl)
{
    PSECURITY_DESCRIPTOR sd = NULL;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, NULL)) return NULL;
    sa->nLength = sizeof(*sa);
    sa->lpSecurityDescriptor = sd;
    sa->bInheritHandle = FALSE;
    return sa;
}

/* ------------------------------------------------------------------ */
/*  状態の共有メモリ                                                    */
/* ------------------------------------------------------------------ */

static HANDLE     g_statMap;
static SvcStatus *g_stat;

BOOL svc_read_status(SvcStatus *st)
{
    HANDLE m = OpenFileMappingW(FILE_MAP_READ, FALSE, OBJ_STATUS);
    const SvcStatus *p;
    if (!m) return FALSE;
    p = (const SvcStatus *)MapViewOfFile(m, FILE_MAP_READ, 0, 0, sizeof(SvcStatus));
    if (p) {
        int i;
        /* 書いている途中を読まないよう、seq が同じ間に写せたものを使う */
        for (i = 0; i < 5; i++) {
            LONG s = p->seq;
            memcpy(st, p, sizeof(*st));
            if (s == p->seq && !(s & 1)) break;
            Sleep(1);
        }
        UnmapViewOfFile(p);
    }
    CloseHandle(m);
    return p != NULL && st->version == STATUS_VERSION;
}

static void signal_event(const WCHAR *name)
{
    HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
    if (e) { SetEvent(e); CloseHandle(e); }
}

void svc_signal_disconnect(void) { signal_event(OBJ_DISCONNECT); }
void svc_signal_reload(void)     { signal_event(OBJ_RELOAD); }

/* ------------------------------------------------------------------ */
/*  分身(-agent)                                                       */
/* ------------------------------------------------------------------ */

static HANDLE g_evStop, g_evReload, g_evDisconnect;

static DWORD WINAPI agent_events(void *arg)
{
    HANDLE h[3];
    int n = 0;
    (void)arg;
    if (g_evStop) h[n++] = g_evStop;
    if (g_evReload) h[n++] = g_evReload;
    if (g_evDisconnect) h[n++] = g_evDisconnect;
    for (;;) {
        DWORD r = WaitForMultipleObjects((DWORD)n, h, FALSE, INFINITE);
        HANDLE which;
        if (r >= WAIT_OBJECT_0 + (DWORD)n) { Sleep(1000); continue; }
        which = h[r - WAIT_OBJECT_0];
        if (which == g_evStop) { PostMessageW(g_mainWnd, WM_APP_COMMAND, CMD_EXIT, 0); return 0; }
        if (which == g_evReload) PostMessageW(g_mainWnd, WM_APP_RELOAD, 0, 0);
        if (which == g_evDisconnect) PostMessageW(g_mainWnd, WM_APP_DISCONNECT, 0, 0);
    }
}

static void desk_name(HDESK d, WCHAR *out, int cap);

/* 検証用(ini の selftest=1): 起動 20 秒後に入力デスクトップを撮り、名前・明るさ・
   ログインしているユーザーをログに書く。再起動してログインする前に分身が画面を
   見られることを、後から確かめるため */
static DWORD WINAPI agent_selftest(void *arg)
{
    HDC   sdc, mdc;
    HBITMAP bmp;
    BITMAPINFO bi;
    BYTE *bits = NULL;
    WCHAR name[64], *user = NULL;
    DWORD len = 0;
    int   w = GetSystemMetrics(SM_CXVIRTUALSCREEN), h = GetSystemMetrics(SM_CYVIRTUALSCREEN), i, n;
    unsigned long long sum = 0;
    (void)arg;
    Sleep(20000);
    agent_follow_input_desktop();
    desk_name(GetThreadDesktop(GetCurrentThreadId()), name, ARRAYSIZE(name));
    sdc = GetDC(NULL);
    mdc = CreateCompatibleDC(sdc);
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bmp = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    if (bmp) {
        HGDIOBJ old = SelectObject(mdc, bmp);
        BitBlt(mdc, 0, 0, w, h, sdc, GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), SRCCOPY | CAPTUREBLT);
        GdiFlush();
        n = w * h;
        for (i = 0; i < n; i += 101) sum += bits[i * 4] + bits[i * 4 + 1] + bits[i * 4 + 2];
        SelectObject(mdc, old);
        DeleteObject(bmp);
    }
    DeleteDC(mdc);
    ReleaseDC(NULL, sdc);
    WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION, WTSUserName, &user, &len);
    log_printf(L"[selftest] 入力デスクトップ %s、%dx%d の平均の明るさ %.1f、ログインしているユーザー「%s」",
               name, w, h, (double)sum / ((w * h) / 101 + 1) / 3, user && user[0] ? user : L"(なし)");
    if (user) WTSFreeMemory(user);
    return 0;
}

/* 分身は SYSTEM で動く。相手から来たファイルをクリップボードに置くと、ログインしている人の
   エクスプローラーが COM でこのプロセスへ中身を読みに来る。既定では同じ利用者(SYSTEM)からしか
   受け付けず、整合性レベルが低い(普通の権限の)呼び出しも断るので、対話ログオンの利用者と
   中位の整合性レベルからの呼び出しを受け付ける。COM を使う前に 1 回だけ呼ぶ。
   (この設定が無いと、利用者の側からは中身を取り出せず DV_E_FORMATETC になった。2026-10-05 実測)
   CoInitializeSecurity は絶対形式の記述子しか受けない(自己相対形式だと 0x80070551)ので変換する。
   記述子はプロセスが終わるまで持っておく */
void agent_com_security(void)
{
    PSECURITY_DESCRIPTOR rel = NULL, abs;
    DWORD   sdSz = 0, daclSz = 0, saclSz = 0, ownSz = 0, grpSz = 0;
    BYTE   *blk;
    HRESULT hr;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"O:SYG:SYD:(A;;0x3;;;SY)(A;;0x3;;;IU)(A;;0x3;;;BA)S:(ML;;NX;;;ME)", SDDL_REVISION_1, &rel, NULL)) {
        log_printf(L"COM の受け付けの設定を作れない (%lu)", GetLastError());
        return;
    }
    MakeAbsoluteSD(rel, NULL, &sdSz, NULL, &daclSz, NULL, &saclSz, NULL, &ownSz, NULL, &grpSz);
    blk = (BYTE *)calloc(1, (size_t)sdSz + daclSz + saclSz + ownSz + grpSz + 64);
    if (!blk) { LocalFree(rel); return; }
    abs = (PSECURITY_DESCRIPTOR)blk;
    if (!MakeAbsoluteSD(rel, abs, &sdSz, (PACL)(blk + sdSz), &daclSz, (PACL)(blk + sdSz + daclSz), &saclSz,
                        (PSID)(blk + sdSz + daclSz + saclSz), &ownSz, (PSID)(blk + sdSz + daclSz + saclSz + ownSz), &grpSz)) {
        log_printf(L"COM の受け付けの設定を変換できない (%lu)", GetLastError());
        LocalFree(rel);
        return;
    }
    LocalFree(rel);
    hr = CoInitializeSecurity(abs, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IDENTIFY, NULL, EOAC_NONE, NULL);
    log_printf(L"COM の受け付け: 対話ログオンの利用者から (0x%08lX)", (unsigned long)hr);
}

BOOL agent_init(void)
{
    SECURITY_ATTRIBUTES sa;
    HANDLE th;
    /* 状態: SYSTEM と管理者は何でも、ログインしたユーザーは読むだけ */
    g_statMap = CreateFileMappingW(INVALID_HANDLE_VALUE, make_sa(&sa, L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;AU)"),
                                   PAGE_READWRITE, 0, sizeof(SvcStatus), OBJ_STATUS);
    if (g_statMap) g_stat = (SvcStatus *)MapViewOfFile(g_statMap, FILE_MAP_WRITE, 0, 0, sizeof(SvcStatus));
    if (!g_stat) log_printf(L"[agent] 状態の共有メモリを作れない (%lu)", GetLastError());
    else { ZeroMemory(g_stat, sizeof(*g_stat)); g_stat->version = STATUS_VERSION; }
    g_evReload = CreateEventW(make_sa(&sa, L"D:(A;;GA;;;SY)(A;;GA;;;BA)"), FALSE, FALSE, OBJ_RELOAD);
    /* 全員を切断: ログインしているユーザー(IU)も知らせられる */
    g_evDisconnect = CreateEventW(make_sa(&sa, L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x100002;;;IU)"), FALSE, FALSE, OBJ_DISCONNECT);
    g_evStop = OpenEventW(SYNCHRONIZE, FALSE, OBJ_AGENT_STOP);
    th = CreateThread(NULL, 0, agent_events, NULL, 0, NULL);
    if (th) CloseHandle(th);
    if (g_cfg.selftest) {
        th = CreateThread(NULL, 0, agent_selftest, NULL, 0, NULL);
        if (th) CloseHandle(th);
    }
    return g_stat != NULL;
}

void agent_status_update(void)
{
    if (!g_stat) return;
    InterlockedIncrement(&g_stat->seq);         /* 奇数 = 書いている途中 */
    g_stat->listening = app_listening();
    g_stat->port = g_cfg.port;
    g_stat->clients = server_list(g_stat->clientList, ARRAYSIZE(g_stat->clientList));
    lstrcpynW(g_stat->method, g_scr.method, ARRAYSIZE(g_stat->method));
    lstrcpynW(g_stat->error, app_listen_error(), ARRAYSIZE(g_stat->error));
    lstrcpynW(g_stat->listen, g_cfg.listen, ARRAYSIZE(g_stat->listen));
    InterlockedIncrement(&g_stat->seq);
}

void agent_notify(const WCHAR *s)
{
    if (!g_stat) return;
    InterlockedIncrement(&g_stat->seq);
    lstrcpynW(g_stat->notify, s, ARRAYSIZE(g_stat->notify));
    g_stat->notifySeq++;
    InterlockedIncrement(&g_stat->seq);
}

void agent_request_sas(void)
{
    log_printf(L"[agent] Ctrl+Alt+Del をサービスに頼む");
    signal_event(OBJ_SAS);
}

static void desk_name(HDESK d, WCHAR *out, int cap)
{
    DWORD need = 0;
    out[0] = 0;
    if (d) GetUserObjectInformationW(d, UOI_NAME, out, (DWORD)(cap * sizeof(WCHAR)), &need);
}

/* 入力デスクトップが、呼んだスレッドのデスクトップと違うか */
BOOL agent_input_desktop_changed(void)
{
    WCHAR a[64], b[64];
    HDESK in = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!in) return FALSE;
    desk_name(in, a, ARRAYSIZE(a));
    CloseDesktop(in);
    desk_name(GetThreadDesktop(GetCurrentThreadId()), b, ARRAYSIZE(b));
    return lstrcmpiW(a, b) != 0;
}

/* 呼んだスレッドを入力デスクトップへ移す(窓やフックを持たないスレッドだけ) */
BOOL agent_follow_input_desktop(void)
{
    HDESK in;
    WCHAR a[64], b[64];
    in = OpenInputDesktop(0, FALSE, GENERIC_ALL);
    if (!in) {
        static DWORD lastErr;
        DWORD e = GetLastError();
        if (e != lastErr) log_printf(L"[agent] 入力デスクトップを開けない (%lu)", e);
        lastErr = e;
        return FALSE;
    }
    desk_name(in, a, ARRAYSIZE(a));
    desk_name(GetThreadDesktop(GetCurrentThreadId()), b, ARRAYSIZE(b));
    if (!lstrcmpiW(a, b)) { CloseDesktop(in); return FALSE; }
    if (!SetThreadDesktop(in)) {
        log_printf(L"[agent] デスクトップ %s へ移れない (%lu)", a, GetLastError());
        CloseDesktop(in);
        return FALSE;
    }
    /* 前のデスクトップのハンドルは閉じない(スレッドが使っていた物。小さいので持ったままでよい) */
    log_printf(L"[agent] スレッド %lu を %s から %s へ", GetCurrentThreadId(), b, a);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  サービス本体(-service)                                             */
/* ------------------------------------------------------------------ */

static SERVICE_STATUS_HANDLE g_ssh;
static SERVICE_STATUS        g_ss;
static HANDLE                g_svcStop, g_svcKick, g_svcSas, g_agentStop;
static volatile LONG         g_logoffSession = -1;

static void set_state(DWORD st)
{
    g_ss.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_ss.dwCurrentState = st;
    g_ss.dwControlsAccepted = st == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_SESSIONCHANGE : 0;
    g_ss.dwWin32ExitCode = NO_ERROR;
    g_ss.dwWaitHint = st == SERVICE_RUNNING || st == SERVICE_STOPPED ? 0 : 10000;
    SetServiceStatus(g_ssh, &g_ss);
}

static DWORD WINAPI svc_handler(DWORD ctrl, DWORD type, LPVOID data, LPVOID ctx)
{
    (void)ctx;
    switch (ctrl) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        set_state(SERVICE_STOP_PENDING);
        SetEvent(g_svcStop);
        return NO_ERROR;
    case SERVICE_CONTROL_SESSIONCHANGE:
        if (type == WTS_SESSION_LOGOFF && data) InterlockedExchange(&g_logoffSession, (LONG)((WTSSESSION_NOTIFICATION *)data)->dwSessionId);
        log_printf(L"[service] セッションの変化 %lu (セッション %lu)", type, data ? ((WTSSESSION_NOTIFICATION *)data)->dwSessionId : 0);
        SetEvent(g_svcKick);
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;
    }
    return ERROR_CALL_NOT_IMPLEMENTED;
}

static HANDLE launch_agent(DWORD session)
{
    HANDLE tok = NULL, dup = NULL;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi = { 0 };
    WCHAR exe[MAX_PATH], cmd[MAX_PATH * 2 + 64];
    BOOL ok = FALSE;

    exe_path(exe);
    swprintf(cmd, ARRAYSIZE(cmd), L"\"%s\" -agent -ini \"%s\"", exe, g_iniPath);
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &tok) &&
        DuplicateTokenEx(tok, MAXIMUM_ALLOWED, NULL, SecurityIdentification, TokenPrimary, &dup) &&
        SetTokenInformation(dup, TokenSessionId, &session, sizeof(session))) {
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        si.lpDesktop = L"winsta0\\default";
        ok = CreateProcessAsUserW(dup, exe, cmd, NULL, NULL, FALSE, NORMAL_PRIORITY_CLASS | CREATE_UNICODE_ENVIRONMENT,
                                  NULL, g_exeDir, &si, &pi);
    }
    if (!ok) log_printf(L"[service] 分身を起動できない (セッション %lu、%lu)", session, GetLastError());
    if (tok) CloseHandle(tok);
    if (dup) CloseHandle(dup);
    if (!ok) return NULL;
    CloseHandle(pi.hThread);
    log_printf(L"[service] 分身を起動した(セッション %lu、PID %lu)", session, pi.dwProcessId);
    return pi.hProcess;
}

static void stop_agent(HANDLE *proc)
{
    if (!*proc) return;
    SetEvent(g_agentStop);
    if (WaitForSingleObject(*proc, 8000) != WAIT_OBJECT_0) {
        log_printf(L"[service] 分身が止まらないので終わらせる");
        TerminateProcess(*proc, 1);
        WaitForSingleObject(*proc, 3000);
    }
    CloseHandle(*proc);
    *proc = NULL;
    ResetEvent(g_agentStop);
}

/* ログインしているユーザーの権限でトレイを出す */
static BOOL launch_tray(DWORD session)
{
    HANDLE ut = NULL;
    LPVOID env = NULL;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi = { 0 };
    WCHAR exe[MAX_PATH], cmd[MAX_PATH * 2 + 64];
    BOOL ok;

    if (!WTSQueryUserToken(session, &ut)) return FALSE;     /* 誰もログインしていない */
    exe_path(exe);
    swprintf(cmd, ARRAYSIZE(cmd), L"\"%s\" -tray -ini \"%s\"", exe, g_iniPath);
    CreateEnvironmentBlock(&env, ut, FALSE);
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.lpDesktop = L"winsta0\\default";
    ok = CreateProcessAsUserW(ut, exe, cmd, NULL, NULL, FALSE, NORMAL_PRIORITY_CLASS | CREATE_UNICODE_ENVIRONMENT,
                              env, g_exeDir, &si, &pi);
    if (env) DestroyEnvironmentBlock(env);
    CloseHandle(ut);
    if (!ok) { log_printf(L"[service] トレイを起動できない (%lu)", GetLastError()); return FALSE; }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    log_printf(L"[service] トレイを起動した(セッション %lu)", session);
    return TRUE;
}

typedef VOID (WINAPI *PSendSAS)(BOOL);

static void send_sas(void)
{
    HMODULE m = LoadLibraryExW(L"sas.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    PSendSAS f = m ? (PSendSAS)(void *)GetProcAddress(m, "SendSAS") : NULL;
    if (f) { f(FALSE); log_printf(L"[service] SendSAS を呼んだ"); }
    else log_printf(L"[service] SendSAS が無い");
}

static VOID WINAPI service_main(DWORD argc, LPWSTR *argv)
{
    SECURITY_ATTRIBUTES sa;
    HANDLE agent = NULL;
    DWORD  agentSession = (DWORD)-1, traySession = (DWORD)-1, lastLaunch = 0, backoff = 1000;
    (void)argc; (void)argv;

    g_ssh = RegisterServiceCtrlHandlerExW(SVC_NAME, svc_handler, NULL);
    if (!g_ssh) return;
    g_svcStop = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_svcKick = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_agentStop = CreateEventW(make_sa(&sa, L"D:(A;;GA;;;SY)(A;;GA;;;BA)"), TRUE, FALSE, OBJ_AGENT_STOP);
    g_svcSas = CreateEventW(make_sa(&sa, L"D:(A;;GA;;;SY)(A;;GA;;;BA)"), FALSE, FALSE, OBJ_SAS);
    set_state(SERVICE_RUNNING);
    log_printf(L"[service] 開始 %s", APP_VERSION);

    for (;;) {
        DWORD  target = WTSGetActiveConsoleSessionId(), r;
        HANDLE h[4];
        int    n = 0;
        LONG   lo = InterlockedExchange(&g_logoffSession, -1);

        if (lo >= 0 && (DWORD)lo == traySession) traySession = (DWORD)-1;
        if (agent && agentSession != target) {
            log_printf(L"[service] コンソールがセッション %lu から %lu へ", agentSession, target);
            stop_agent(&agent);
        }
        if (!agent && target != (DWORD)-1 && GetTickCount() - lastLaunch >= backoff) {
            lastLaunch = GetTickCount();
            agent = launch_agent(target);
            agentSession = target;
        }
        if (target != (DWORD)-1 && traySession != target && launch_tray(target)) traySession = target;

        h[n++] = g_svcStop;
        h[n++] = g_svcKick;
        h[n++] = g_svcSas;
        if (agent) h[n++] = agent;
        r = WaitForMultipleObjects((DWORD)n, h, FALSE, 2000);
        if (r == WAIT_OBJECT_0) break;
        if (r == WAIT_OBJECT_0 + 2) send_sas();
        if (agent && r == WAIT_OBJECT_0 + 3) {
            DWORD code = 0;
            GetExitCodeProcess(agent, &code);
            log_printf(L"[service] 分身が終わった (終了コード %lu)", code);
            CloseHandle(agent);
            agent = NULL;
            /* すぐ終わるのを繰り返すなら、間を空ける(最大 30 秒) */
            backoff = GetTickCount() - lastLaunch < 10000 ? min(backoff * 2, 30000) : 1000;
        }
    }
    stop_agent(&agent);
    log_printf(L"[service] 終了");
    set_state(SERVICE_STOPPED);
}

int svc_service_main(void)
{
    SERVICE_TABLE_ENTRYW t[] = { { (LPWSTR)SVC_NAME, service_main }, { NULL, NULL } };
    if (!StartServiceCtrlDispatcherW(t)) return (int)GetLastError();
    return 0;
}

/* ------------------------------------------------------------------ */
/*  登録の状態                                                          */
/* ------------------------------------------------------------------ */

/* この ini で登録されているか(別の ini で動かす検証用の起動を巻き込まない) */
BOOL svc_installed(void)
{
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT), s;
    BOOL found = FALSE;
    if (!scm) return FALSE;
    s = OpenServiceW(scm, SVC_NAME, SERVICE_QUERY_CONFIG);
    if (s) {
        DWORD need = 0;
        QUERY_SERVICE_CONFIGW *qc;
        QueryServiceConfigW(s, NULL, 0, &need);
        qc = (QUERY_SERVICE_CONFIGW *)malloc(need ? need : 1);
        if (qc && QueryServiceConfigW(s, qc, need, &need) && qc->lpBinaryPathName) {
            WCHAR bin[MAX_PATH * 3], ini[MAX_PATH];
            lstrcpynW(bin, qc->lpBinaryPathName, ARRAYSIZE(bin));
            lstrcpynW(ini, g_iniPath, ARRAYSIZE(ini));
            CharLowerW(bin);
            CharLowerW(ini);
            found = wcsstr(bin, ini) != NULL;
        }
        free(qc);
        CloseServiceHandle(s);
    }
    CloseServiceHandle(scm);
    return found;
}

/* exe(とそのフォルダ・ini)を、管理者でない人が書き換えられるか。
   書き換えられると、その exe を差し替えた人が SYSTEM の権限を得られてしまう。
   who に、書き換えられる人の名前を並べる。 */
BOOL svc_path_risky(WCHAR *who, int cap)
{
    WCHAR exe[MAX_PATH], dir[MAX_PATH], *p;
    const WCHAR *paths[3];
    const WCHAR *sids[] = { L"S-1-1-0", L"S-1-5-11", L"S-1-5-32-545", L"S-1-5-4" };     /* Everyone, Authenticated Users, Users, INTERACTIVE */
    const WCHAR *names[] = { L"Everyone", L"Authenticated Users", L"Users", L"INTERACTIVE" };
    BOOL risky = FALSE;
    int  i, k;
    HANDLE tok;

    who[0] = 0;
    exe_path(exe);
    lstrcpynW(dir, exe, MAX_PATH);
    p = wcsrchr(dir, L'\\');
    if (p) *p = 0;
    paths[0] = exe; paths[1] = dir; paths[2] = g_iniPath;
    for (i = 0; i < 3; i++) {
        PACL dacl = NULL;
        PSECURITY_DESCRIPTOR sd = NULL;
        if (GetNamedSecurityInfoW(paths[i], SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, &dacl, NULL, &sd)) continue;
        for (k = 0; k < 5; k++) {
            TRUSTEEW tr;
            ACCESS_MASK m = 0;
            PSID sid = NULL;
            WCHAR label[128];
            if (k < 4) {
                if (!ConvertStringSidToSidW(sids[k], &sid)) continue;
                lstrcpyW(label, names[k]);
            } else {
                /* この画面を開いている人(管理者でも、ふだんは制限付きで動いている) */
                TOKEN_USER *tu;
                DWORD len = 0;
                if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) continue;
                GetTokenInformation(tok, TokenUser, NULL, 0, &len);
                tu = (TOKEN_USER *)malloc(len);
                if (tu && GetTokenInformation(tok, TokenUser, tu, len, &len)) {
                    DWORD sl = GetLengthSid(tu->User.Sid);
                    sid = LocalAlloc(LMEM_FIXED, sl);
                    if (sid) CopySid(sl, sid, tu->User.Sid);
                }
                free(tu);
                CloseHandle(tok);
                if (!sid) continue;
                {
                    WCHAR nm[64], dom[64];
                    DWORD nl = 64, dl = 64;
                    SID_NAME_USE use;
                    if (!LookupAccountSidW(NULL, sid, nm, &nl, dom, &dl, &use)) lstrcpyW(nm, L"このユーザー");
                    lstrcpynW(label, nm, ARRAYSIZE(label));
                }
            }
            BuildTrusteeWithSidW(&tr, sid);
            if (GetEffectiveRightsFromAclW(dacl, &tr, &m) == ERROR_SUCCESS &&
                (m & (FILE_WRITE_DATA | FILE_APPEND_DATA | WRITE_DAC | WRITE_OWNER | DELETE))) {
                if (!wcsstr(who, label)) {
                    if (who[0] && lstrlenW(who) + 3 < cap) lstrcatW(who, L"、");
                    if (lstrlenW(who) + lstrlenW(label) + 1 < cap) lstrcatW(who, label);
                }
                risky = TRUE;
            }
            LocalFree(sid);
        }
        LocalFree(sd);
    }
    return risky;
}

/* 管理者として自分を起動する */
BOOL svc_run_elevated(const WCHAR *args, HWND owner, BOOL wait, DWORD *exitCode)
{
    SHELLEXECUTEINFOW se;
    WCHAR exe[MAX_PATH], params[MAX_PATH * 2 + 64];
    exe_path(exe);
    swprintf(params, ARRAYSIZE(params), L"%s -ini \"%s\"", args, g_iniPath);
    ZeroMemory(&se, sizeof(se));
    se.cbSize = sizeof(se);
    se.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    se.hwnd = owner;
    se.lpVerb = L"runas";
    se.lpFile = exe;
    se.lpParameters = params;
    se.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&se)) return FALSE;            /* 管理者の確認を断った */
    if (se.hProcess) {
        if (wait) {
            MSG msg;
            /* 待つ間も画面を描けるよう、メッセージを回す */
            while (MsgWaitForMultipleObjects(1, &se.hProcess, FALSE, INFINITE, QS_ALLINPUT) == WAIT_OBJECT_0 + 1)
                while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
            if (exitCode) GetExitCodeProcess(se.hProcess, exitCode);
        }
        CloseHandle(se.hProcess);
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  ファイアウォールと Ctrl+Alt+Del のポリシー(管理者で)               */
/* ------------------------------------------------------------------ */

void svc_firewall(BOOL add)
{
    INetFwPolicy2 *pol = NULL;
    INetFwRules   *rules = NULL;
    INetFwRule    *rule = NULL;
    BSTR name = SysAllocString(FW_RULE);
    HRESULT hr, co = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    hr = CoCreateInstance(&CLSID_NetFwPolicy2, NULL, CLSCTX_INPROC_SERVER, &IID_INetFwPolicy2, (void **)&pol);
    if (SUCCEEDED(hr)) hr = INetFwPolicy2_get_Rules(pol, &rules);
    if (SUCCEEDED(hr)) {
        INetFwRules_Remove(rules, name);            /* 前の規則(ポートが変わったときも) */
        if (add && SUCCEEDED(CoCreateInstance(&CLSID_NetFwRule, NULL, CLSCTX_INPROC_SERVER, &IID_INetFwRule, (void **)&rule))) {
            WCHAR exe[MAX_PATH], port[16];
            BSTR app, ports, desc;
            exe_path(exe);
            swprintf(port, ARRAYSIZE(port), L"%d", g_cfg.port);
            app = SysAllocString(exe);
            ports = SysAllocString(port);
            desc = SysAllocString(L"iiv-server(サービス)への接続を受け付ける");
            INetFwRule_put_Name(rule, name);
            INetFwRule_put_Description(rule, desc);
            INetFwRule_put_ApplicationName(rule, app);
            INetFwRule_put_Protocol(rule, NET_FW_IP_PROTOCOL_TCP);
            INetFwRule_put_LocalPorts(rule, ports);
            INetFwRule_put_Direction(rule, NET_FW_RULE_DIR_IN);
            INetFwRule_put_Action(rule, NET_FW_ACTION_ALLOW);
            INetFwRule_put_Profiles(rule, NET_FW_PROFILE2_ALL);
            INetFwRule_put_Enabled(rule, VARIANT_TRUE);
            hr = INetFwRules_Add(rules, rule);
            log_printf(L"ファイアウォールの規則を足した: TCP %s (0x%08lX)", port, (unsigned long)hr);
            SysFreeString(app); SysFreeString(ports); SysFreeString(desc);
            INetFwRule_Release(rule);
        } else if (!add) {
            log_printf(L"ファイアウォールの規則を消した");
        }
    }
    if (rules) INetFwRules_Release(rules);
    if (pol) INetFwPolicy2_Release(pol);
    SysFreeString(name);
    if (SUCCEEDED(co)) CoUninitialize();
}

/* SoftwareSASGeneration: 1 = サービス、2 = 簡単操作のアプリ、3 = 両方 */
static void sas_policy(BOOL set)
{
    HKEY  k;
    DWORD v = 0, type = 0, len = sizeof(v);
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, SAS_KEY, 0, NULL, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, NULL, &k, NULL)) {
        log_printf(L"SAS のポリシーを開けない");
        return;
    }
    if (set) {
        BOOL had = RegQueryValueExW(k, SAS_NAME, NULL, &type, (BYTE *)&v, &len) == ERROR_SUCCESS && type == REG_DWORD;
        if (g_cfg.sasBefore == -2) g_cfg.sasBefore = had ? (int)v : -1;
        v = (had ? v : 0) | 1;
        RegSetValueExW(k, SAS_NAME, 0, REG_DWORD, (const BYTE *)&v, sizeof(v));
        log_printf(L"SoftwareSASGeneration を %lu にした(元 %d)", v, g_cfg.sasBefore);
    } else if (g_cfg.sasBefore != -2) {
        if (g_cfg.sasBefore < 0) RegDeleteValueW(k, SAS_NAME);
        else { v = (DWORD)g_cfg.sasBefore; RegSetValueExW(k, SAS_NAME, 0, REG_DWORD, (const BYTE *)&v, sizeof(v)); }
        log_printf(L"SoftwareSASGeneration を元に戻した(%d)", g_cfg.sasBefore);
        g_cfg.sasBefore = -2;
    }
    RegCloseKey(k);
}

/* ------------------------------------------------------------------ */
/*  登録・解除(管理者で)                                              */
/* ------------------------------------------------------------------ */

static void admin_message(const WCHAR *main, const WCHAR *content)
{
    TASKDIALOGCONFIG tc;
    ZeroMemory(&tc, sizeof(tc));
    tc.cbSize = sizeof(tc);
    tc.hInstance = g_inst;
    tc.dwFlags = TDF_SIZE_TO_CONTENT;
    tc.pszWindowTitle = APP_NAME;
    tc.pszMainIcon = TD_ERROR_ICON;
    tc.pszMainInstruction = main;
    tc.pszContent = content;
    tc.dwCommonButtons = TDCBF_OK_BUTTON;
    TaskDialogIndirect(&tc, NULL, NULL, NULL);
}

/* -install-service: 0 = 登録して始めた */
int svc_install_cmd(void)
{
    SC_HANDLE scm, s;
    WCHAR exe[MAX_PATH], bin[MAX_PATH * 2 + 64], err[256];
    SERVICE_DESCRIPTIONW desc;
    SC_ACTION acts[3] = { { SC_ACTION_RESTART, 5000 }, { SC_ACTION_RESTART, 5000 }, { SC_ACTION_RESTART, 30000 } };
    SERVICE_FAILURE_ACTIONSW fa;

    if (!g_cfg.pw.set && !g_cfg.viewPw.set && lstrcmpW(g_cfg.listen, L"127.0.0.1")) {
        admin_message(L"パスワードを設定してから登録してください。", NULL);
        return 3;
    }
    scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!scm) {
        swprintf(err, ARRAYSIZE(err), L"サービスの管理を開けません (%lu)。", GetLastError());
        admin_message(L"サービスとして登録できませんでした。", err);
        return 2;
    }
    exe_path(exe);
    swprintf(bin, ARRAYSIZE(bin), L"\"%s\" -service -ini \"%s\"", exe, g_iniPath);
    s = CreateServiceW(scm, SVC_NAME, SVC_DISPLAY, SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
                       SERVICE_ERROR_NORMAL, bin, NULL, NULL, NULL, NULL, NULL);
    if (!s && GetLastError() == ERROR_SERVICE_EXISTS) {
        s = OpenServiceW(scm, SVC_NAME, SERVICE_ALL_ACCESS);
        if (s) ChangeServiceConfigW(s, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, bin,
                                    NULL, NULL, NULL, NULL, NULL, SVC_DISPLAY);
    }
    if (!s) {
        swprintf(err, ARRAYSIZE(err), L"CreateService が失敗しました (%lu)。", GetLastError());
        admin_message(L"サービスとして登録できませんでした。", err);
        CloseServiceHandle(scm);
        return 2;
    }
    desc.lpDescription = (LPWSTR)L"ログイン前・ロック中・UAC の確認画面でも、この PC の画面をほかの PC から見て操作できるようにする(iiv-server)";
    ChangeServiceConfig2W(s, SERVICE_CONFIG_DESCRIPTION, &desc);
    ZeroMemory(&fa, sizeof(fa));
    fa.dwResetPeriod = 86400;
    fa.cActions = 3;
    fa.lpsaActions = acts;
    ChangeServiceConfig2W(s, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);

    sas_policy(TRUE);
    if (lstrcmpW(g_cfg.listen, L"127.0.0.1")) svc_firewall(TRUE);
    config_save();
    log_printf(L"サービスとして登録した: %s", bin);

    if (!StartServiceW(s, 0, NULL) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
        swprintf(err, ARRAYSIZE(err), L"登録はできましたが、始められませんでした (%lu)。", GetLastError());
        admin_message(L"サービスを始められませんでした。", err);
        CloseServiceHandle(s);
        CloseServiceHandle(scm);
        return 4;
    }
    CloseServiceHandle(s);
    CloseServiceHandle(scm);
    return 0;
}

/* 管理者の設定画面から: 止めて登録を消し、ポリシーとファイアウォールを戻す */
BOOL svc_uninstall(HWND owner)
{
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS), s;
    SERVICE_STATUS st;
    WCHAR err[128];
    int i;
    (void)owner;
    if (!scm) return FALSE;
    s = OpenServiceW(scm, SVC_NAME, SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
    if (!s) { CloseServiceHandle(scm); return FALSE; }
    ControlService(s, SERVICE_CONTROL_STOP, &st);
    for (i = 0; i < 150; i++) {
        if (!QueryServiceStatus(s, &st) || st.dwCurrentState == SERVICE_STOPPED) break;
        Sleep(100);
    }
    if (!DeleteService(s)) {
        swprintf(err, ARRAYSIZE(err), L"DeleteService が失敗しました (%lu)。", GetLastError());
        admin_message(L"サービスの登録を消せませんでした。", err);
        CloseServiceHandle(s);
        CloseServiceHandle(scm);
        return FALSE;
    }
    CloseServiceHandle(s);
    CloseServiceHandle(scm);
    sas_policy(FALSE);
    svc_firewall(FALSE);
    config_save();
    log_printf(L"サービスの登録を消した");
    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  トレイ(-tray、ログインしたユーザーの権限)                        */
/* ------------------------------------------------------------------ */

#define TRAYS_CLASS L"iiv.Server.SvcTray"
#define TIMER_POLL  1
#define TIMER_SVC   2
static BOOL g_tV4;              /* 新しい形式(NOTIFYICON_VERSION_4)にできた */

enum { IDT_SETTINGS = 200, IDT_DISCONNECT, IDT_HIDE };

static NOTIFYICONDATAW g_tn;
static HICON           g_tIdle, g_tActive;
static BOOL            g_tAdded, g_tHidden;     /* g_tHidden: 「通知領域から消す」(クリップボードの受け渡しは続ける) */
static LONG            g_tNotifySeq = -1;
static UINT            g_tTaskbarCreated;

/* 利用者がコピーしたものを分身へ渡す(分身は SYSTEM なので、自分ではクリップボードを読めない) */
static void tray_clip_forward(HWND hwnd)
{
    HWND   agent = FindWindowW(TRAY_CLASS, AGENT_TITLE), owner = GetClipboardOwner();
    DWORD  apid = 0, opid = 0;
    COPYDATASTRUCT cd;
    WCHAR *buf = NULL;
    size_t chars = 0;
    int    i;
    if (!agent) return;
    GetWindowThreadProcessId(agent, &apid);
    if (owner) GetWindowThreadProcessId(owner, &opid);
    if (opid && opid == apid) return;       /* 分身が置いた(相手から来た)もの。送り返さない */
    for (i = 0; i < 10 && !OpenClipboard(hwnd); i++) Sleep(20);
    if (i == 10) return;
    ZeroMemory(&cd, sizeof(cd));
    if (IsClipboardFormatAvailable(CF_HDROP)) {
        HDROP hd = (HDROP)GetClipboardData(CF_HDROP);
        WCHAR *p = hd ? fx_hdrop_paths(hd) : NULL;
        if (p) {
            const WCHAR *q = p;
            while (*q) q += lstrlenW(q) + 1;
            chars = (size_t)(q - p) + 1;
            buf = (WCHAR *)malloc(chars * sizeof(WCHAR));
            if (buf) memcpy(buf, p, chars * sizeof(WCHAR));
            HeapFree(GetProcessHeap(), 0, p);
            cd.dwData = CD_CLIP_FILES;
        }
    } else if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
        const WCHAR *w = h ? (const WCHAR *)GlobalLock(h) : NULL;
        if (w) {
            chars = wcslen(w) + 1;
            if (chars <= (8u << 20)) {
                buf = (WCHAR *)malloc(chars * sizeof(WCHAR));
                if (buf) memcpy(buf, w, chars * sizeof(WCHAR));
            }
            GlobalUnlock(h);
            cd.dwData = CD_CLIP_TEXT;
        }
    }
    CloseClipboard();
    if (!buf) return;
    cd.cbData = (DWORD)(chars * sizeof(WCHAR));
    cd.lpData = buf;
    SendMessageTimeoutW(agent, WM_COPYDATA, (WPARAM)hwnd, (LPARAM)&cd, SMTO_ABORTIFHUNG, 3000, NULL);
    free(buf);
}

static void tray_refresh(HWND hwnd)
{
    SvcStatus st;
    BOOL ok;
    if (g_tHidden) return;
    ok = svc_read_status(&st);
    g_tn.cbSize = sizeof(g_tn);
    g_tn.hWnd = hwnd;
    g_tn.uID = 1;
    g_tn.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_tn.uCallbackMessage = WM_APP_TRAY;
    g_tn.hIcon = ok && st.clients ? g_tActive : g_tIdle;
    if (!ok) swprintf(g_tn.szTip, ARRAYSIZE(g_tn.szTip), L"iiv-server(サービス)- 起動を待っています");
    else if (!st.listening) swprintf(g_tn.szTip, ARRAYSIZE(g_tn.szTip), L"iiv-server(サービス)- 待ち受けていません");
    else if (st.clients) swprintf(g_tn.szTip, ARRAYSIZE(g_tn.szTip), L"iiv-server(サービス)- ポート %ld、%ld 人が接続中", st.port, st.clients);
    else swprintf(g_tn.szTip, ARRAYSIZE(g_tn.szTip), L"iiv-server(サービス)- ポート %ld で待ち受け中", st.port);
    if (!g_tAdded) {
        g_tAdded = Shell_NotifyIconW(NIM_ADD, &g_tn);
        if (g_tAdded) { g_tn.uVersion = NOTIFYICON_VERSION_4; g_tV4 = Shell_NotifyIconW(NIM_SETVERSION, &g_tn); }
    } else {
        Shell_NotifyIconW(NIM_MODIFY, &g_tn);
    }
    if (ok && g_tAdded) {
        if (g_tNotifySeq >= 0 && st.notifySeq != g_tNotifySeq && st.notify[0] && g_cfg.notify) {
            g_tn.uFlags = NIF_INFO;
            lstrcpynW(g_tn.szInfo, st.notify, ARRAYSIZE(g_tn.szInfo));
            lstrcpyW(g_tn.szInfoTitle, L"iiv-server");
            g_tn.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
            Shell_NotifyIconW(NIM_MODIFY, &g_tn);
            g_tn.szInfo[0] = 0;
        }
        g_tNotifySeq = st.notifySeq;
    }
}

static void tray_menu2(HWND hwnd)
{
    HMENU m = CreatePopupMenu();
    SvcStatus st;
    POINT pt;
    BOOL ok = svc_read_status(&st);
    AppendMenuW(m, MF_STRING, IDT_SETTINGS, L"設定(&S)...(管理者)");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    if (ok && st.clients) {
        WCHAR *p = st.clientList, *e;
        for (; p && *p; p = e) {
            e = wcschr(p, L'\n');
            if (e) *e++ = 0;
            AppendMenuW(m, MF_STRING | MF_GRAYED, 0, p);
        }
        AppendMenuW(m, MF_STRING, IDT_DISCONNECT, L"全員を切断(&D)");
    } else {
        AppendMenuW(m, MF_STRING | MF_GRAYED, 0, ok ? L"接続はありません" : L"サービスの起動を待っています");
    }
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDT_HIDE, L"通知領域から消す(&H)(サービスは動き続けます)");
    SetMenuDefaultItem(m, IDT_SETTINGS, FALSE);
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static LRESULT CALLBACK tray_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TIMER:
        if (wp == TIMER_POLL) tray_refresh(hwnd);
        else if (wp == TIMER_SVC && !svc_installed()) {
            /* サービスをやめた: ふだんのトレイ常駐で動き直す */
            STARTUPINFOW si;
            PROCESS_INFORMATION pi = { 0 };
            WCHAR exe[MAX_PATH], cmd[MAX_PATH * 2 + 32];
            exe_path(exe);
            swprintf(cmd, ARRAYSIZE(cmd), L"\"%s\" -ini \"%s\"", exe, g_iniPath);
            ZeroMemory(&si, sizeof(si));
            si.cb = sizeof(si);
            KillTimer(hwnd, TIMER_SVC);
            if (g_tAdded) Shell_NotifyIconW(NIM_DELETE, &g_tn);
            g_tAdded = FALSE;
            DestroyWindow(hwnd);
            if (CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, g_exeDir, &si, &pi)) {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
        }
        return 0;
    case WM_CLIPBOARDUPDATE:
        tray_clip_forward(hwnd);
        return 0;
    case WM_APP_TRAY:
        switch (LOWORD(lp)) {
        /* 新しい形式では、左クリック 1 回で WM_LBUTTONUP と NIN_SELECT の両方が来る
           (右クリックは WM_RBUTTONUP と WM_CONTEXTMENU)。両方で開くと設定画面が 2 枚になる */
        case NIN_SELECT: case NIN_KEYSELECT:
            svc_run_elevated(L"-svcsettings", NULL, FALSE, NULL);
            break;
        case WM_LBUTTONUP:
            if (!g_tV4) svc_run_elevated(L"-svcsettings", NULL, FALSE, NULL);
            break;
        case WM_CONTEXTMENU:
            tray_menu2(hwnd);
            break;
        case WM_RBUTTONUP:
            if (!g_tV4) tray_menu2(hwnd);
            break;
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDT_SETTINGS:   svc_run_elevated(L"-svcsettings", NULL, FALSE, NULL); break;
        case IDT_DISCONNECT: svc_signal_disconnect(); break;
        case IDT_HIDE:       /* アイコンだけ消す。クリップボードの受け渡しのため、プロセスは残す */
            if (g_tAdded) Shell_NotifyIconW(NIM_DELETE, &g_tn);
            g_tAdded = FALSE;
            g_tHidden = TRUE;
            break;
        }
        return 0;
    case WM_APP_COMMAND:
        if (wp == CMD_EXIT) DestroyWindow(hwnd);
        else svc_run_elevated(L"-svcsettings", NULL, FALSE, NULL);
        return 0;
    case WM_DESTROY:
        if (g_tAdded) Shell_NotifyIconW(NIM_DELETE, &g_tn);
        PostQuitMessage(0);
        return 0;
    }
    if (msg == g_tTaskbarCreated && msg) {
        g_tAdded = FALSE;
        tray_refresh(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* -tray(と、サービスが登録されているときのふだんの起動) */
int svc_tray_main(int cmd)
{
    WNDCLASSW wc;
    MSG msg;
    HWND hwnd;
    HANDLE mutex = CreateMutexW(NULL, FALSE, L"Local\\iiv-server-svctray");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(TRAYS_CLASS, NULL);
        if (other) {
            DWORD pid = 0;
            GetWindowThreadProcessId(other, &pid);
            AllowSetForegroundWindow(pid);
            PostMessageW(other, WM_APP_COMMAND, (WPARAM)cmd, 0);
        }
        if (mutex) CloseHandle(mutex);
        return 0;
    }
    if (cmd == CMD_EXIT) { if (mutex) CloseHandle(mutex); return 0; }
    config_load();
    log_open();
    g_tIdle   = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    g_tActive = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(IDI_ACTIVE), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    g_tTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = tray_proc;
    wc.hInstance = g_inst;
    wc.lpszClassName = TRAYS_CLASS;
    RegisterClassW(&wc);
    hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, TRAYS_CLASS, L"iiv-server", WS_POPUP, 0, 0, 0, 0, NULL, NULL, g_inst, NULL);
    if (!hwnd) return 1;
    tray_refresh(hwnd);
    SetTimer(hwnd, TIMER_POLL, 500, NULL);
    SetTimer(hwnd, TIMER_SVC, 3000, NULL);
    AddClipboardFormatListener(hwnd);       /* 利用者がコピーしたものを分身へ渡す */
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (mutex) CloseHandle(mutex);
    return 0;
}
