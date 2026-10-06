/* ==================================================================
 * fwrules.c - Windows ファイアウォールの、この exe の規則を数える・消す
 *             (iiv-server と iiv-client で同じファイル。先頭の #include だけ違う)
 *
 *  初めて待ち受けたとき、Windows が「アクセスを許可する」を聞き、答えに合わせて
 *  この exe の受信の規則(TCP と UDP)を作る。それを数え、消せるようにする
 *  (消せば、次に待ち受けるときにまた聞かれる)。
 *
 *  - 読むのは一般の権限でよい。消すには管理者が要るので、自分を管理者で起動して
 *    -remove-firewall で消す(すでに管理者なら、その場で消す)。
 *  - 規則の Name は表示名(「iiv-server (リモート デスクトップ)」)で、別の場所に置いた同じ exe の
 *    規則とも同じになる。INetFwRules::Remove は名前で消すので、消す規則だけを一意な名前に
 *    付け替えてから消す(ほかの場所の exe の規則を巻き込まない。2026-10-04 実測)。
 * ================================================================== */

#include "iiv.h"
#include <shellapi.h>
#include <netfw.h>

#define FW_MAX 64

static void exe_self(WCHAR *exe)
{
    GetModuleFileNameW(NULL, exe, MAX_PATH);
}

/* この exe の規則を集める(keep の名前の規則は除く)。集めた数、-1 = 読めない */
static int collect(const WCHAR *keep, INetFwPolicy2 **polOut, INetFwRules **rulesOut, INetFwRule **out)
{
    INetFwPolicy2 *pol = NULL;
    INetFwRules   *rules = NULL;
    IUnknown      *unk = NULL;
    IEnumVARIANT  *en = NULL;
    WCHAR          exe[MAX_PATH];
    VARIANT        v;
    ULONG          got;
    int            n = 0;

    exe_self(exe);
    if (FAILED(CoCreateInstance(&CLSID_NetFwPolicy2, NULL, CLSCTX_INPROC_SERVER, &IID_INetFwPolicy2, (void **)&pol)) ||
        FAILED(INetFwPolicy2_get_Rules(pol, &rules)) || FAILED(INetFwRules_get__NewEnum(rules, &unk)) ||
        FAILED(IUnknown_QueryInterface(unk, &IID_IEnumVARIANT, (void **)&en))) {
        if (unk) IUnknown_Release(unk);
        if (rules) INetFwRules_Release(rules);
        if (pol) INetFwPolicy2_Release(pol);
        return -1;
    }
    IUnknown_Release(unk);
    VariantInit(&v);
    while (n < FW_MAX && IEnumVARIANT_Next(en, 1, &v, &got) == S_OK && got == 1) {
        INetFwRule *r = NULL;
        if (v.vt == VT_DISPATCH && v.pdispVal &&
            SUCCEEDED(IDispatch_QueryInterface(v.pdispVal, &IID_INetFwRule, (void **)&r))) {
            BSTR app = NULL, name = NULL;
            BOOL mine = FALSE;
            INetFwRule_get_ApplicationName(r, &app);
            if (app) {
                WCHAR full[MAX_PATH];
                if (!ExpandEnvironmentStringsW(app, full, MAX_PATH)) lstrcpynW(full, app, MAX_PATH);
                mine = !lstrcmpiW(full, exe);
            }
            if (mine && keep) {
                INetFwRule_get_Name(r, &name);
                if (name && !lstrcmpW(name, keep)) mine = FALSE;
            }
            SysFreeString(app);
            SysFreeString(name);
            if (mine) out[n++] = r;
            else INetFwRule_Release(r);
        }
        VariantClear(&v);
    }
    IEnumVARIANT_Release(en);
    *polOut = pol;
    *rulesOut = rules;
    return n;
}

BOOL fw_query(const WCHAR *keep, FwInfo *fi)
{
    INetFwPolicy2 *pol;
    INetFwRules   *rules;
    INetFwRule    *r[FW_MAX];
    HRESULT co = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    int i, n = collect(keep, &pol, &rules, r);
    ZeroMemory(fi, sizeof(*fi));
    if (n >= 0) {
        for (i = 0; i < n; i++) {
            NET_FW_ACTION act = NET_FW_ACTION_ALLOW;
            long prof = 0;
            INetFwRule_get_Action(r[i], &act);
            INetFwRule_get_Profiles(r[i], &prof);
            if (act == NET_FW_ACTION_BLOCK) { fi->block++; fi->blockProfiles |= prof; }
            else { fi->allow++; fi->allowProfiles |= prof; }
            INetFwRule_Release(r[i]);
        }
        fi->count = n;
        INetFwRules_Release(rules);
        INetFwPolicy2_Release(pol);
    }
    if (SUCCEEDED(co)) CoUninitialize();
    return n >= 0;
}

/* 管理者で。消した数、-1 = 失敗 */
int fw_remove(const WCHAR *keep)
{
    INetFwPolicy2 *pol;
    INetFwRules   *rules;
    INetFwRule    *r[FW_MAX];
    HRESULT co = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    int i, n = collect(keep, &pol, &rules, r), done = 0;
    if (n >= 0) {
        for (i = 0; i < n; i++) {
            GUID  g;
            WCHAR tmp[80];
            BSTR  b;
            HRESULT hr;
            CoCreateGuid(&g);
            swprintf(tmp, ARRAYSIZE(tmp), L"iiv-remove-%08lX%04X%04X", g.Data1, g.Data2, g.Data3);
            b = SysAllocString(tmp);
            hr = INetFwRule_put_Name(r[i], b);           /* この規則だけの名前にしてから消す */
            if (SUCCEEDED(hr)) hr = INetFwRules_Remove(rules, b);
            if (SUCCEEDED(hr)) done++;
            else log_printf(L"ファイアウォールの規則を消せない (0x%08lX)", (unsigned long)hr);
            SysFreeString(b);
            INetFwRule_Release(r[i]);
        }
        INetFwRules_Release(rules);
        INetFwPolicy2_Release(pol);
        log_printf(L"ファイアウォールの、この exe の規則を消した: %d / %d 件", done, n);
    } else {
        log_printf(L"ファイアウォールの規則を読めない");
    }
    if (SUCCEEDED(co)) CoUninitialize();
    return n < 0 ? -1 : done;
}

static BOOL is_elevated(void)
{
    HANDLE tok;
    TOKEN_ELEVATION e = { 0 };
    DWORD len = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return FALSE;
    GetTokenInformation(tok, TokenElevation, &e, sizeof(e), &len);
    CloseHandle(tok);
    return e.TokenIsElevated != 0;
}

/* 管理者でなければ、自分を管理者で起動して消す。消した数、-1 = 失敗か断られた */
int fw_remove_elevated(HWND owner, const WCHAR *keep, const WCHAR *args)
{
    SHELLEXECUTEINFOW se;
    WCHAR exe[MAX_PATH];
    DWORD code = (DWORD)-1;
    if (is_elevated()) return fw_remove(keep);
    exe_self(exe);
    ZeroMemory(&se, sizeof(se));
    se.cbSize = sizeof(se);
    se.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    se.hwnd = owner;
    se.lpVerb = L"runas";
    se.lpFile = exe;
    se.lpParameters = args;
    se.nShow = SW_HIDE;
    if (!ShellExecuteExW(&se)) return -1;           /* 管理者の確認を断った */
    if (se.hProcess) {
        MSG msg;
        /* 待つ間も画面を描けるよう、メッセージを回す */
        while (MsgWaitForMultipleObjects(1, &se.hProcess, FALSE, INFINITE, QS_ALLINPUT) == WAIT_OBJECT_0 + 1)
            while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        GetExitCodeProcess(se.hProcess, &code);
        CloseHandle(se.hProcess);
    }
    return (int)code;
}

/* 「Windows ファイアウォールに、この exe の規則が 2 件(受信を許可: パブリック)」のような文 */
static void profiles(long p, WCHAR *s, int cap)
{
    s[0] = 0;
    if (p & NET_FW_PROFILE2_PRIVATE) wcsncat(s, L"プライベート・", cap - wcslen(s) - 1);
    if (p & NET_FW_PROFILE2_PUBLIC) wcsncat(s, L"パブリック・", cap - wcslen(s) - 1);
    if (p & NET_FW_PROFILE2_DOMAIN) wcsncat(s, L"ドメイン・", cap - wcslen(s) - 1);
    if (s[0]) s[wcslen(s) - 1] = 0;                 /* 最後の「・」 */
}

void fw_describe(const FwInfo *fi, WCHAR *s, int cap)
{
    WCHAR a[64], b[64];
    if (!fi->count) {
        lstrcpynW(s, L"Windows ファイアウォールに、この exe の許可設定はありません。", cap);
        return;
    }
    profiles(fi->allowProfiles, a, ARRAYSIZE(a));
    profiles(fi->blockProfiles, b, ARRAYSIZE(b));
    if (fi->allow && fi->block)
        swprintf(s, cap, L"Windows ファイアウォールに、この exe の規則が %d 件あります(受信を許可: %s、ブロック: %s)。", fi->count, a, b);
    else if (fi->allow)
        swprintf(s, cap, L"Windows ファイアウォールに、この exe の規則が %d 件あります(受信を許可: %s)。", fi->count, a);
    else
        swprintf(s, cap, L"Windows ファイアウォールに、この exe の規則が %d 件あります(受信をブロック: %s)。", fi->count, b);
}
