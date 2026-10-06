/* ==================================================================
 * ui.c - 設定画面(モードレス)
 *
 *  上半分は状態(待ち受けているアドレス、取り込みの方法、接続中の相手)。
 *  1 秒ごとに書き直す。下半分が設定。OK で ini に書き、待ち受けや
 *  取り込みに関わる項目が変わっていればやり直す。
 *
 *  パスワード欄は空のまま OK なら変えない(今の値は画面に出さない)。
 *
 *  サービスとして動いているときは、トレイから管理者として開かれる
 *  (-svcsettings、g_uiService)。状態は分身の共有メモリから読み、OK で ini に
 *  書いて分身に読み直させる。「サービスとして登録」「サービスをやめる」もここ。
 *
 *  「ネットワークの許可」: 初めて待ち受けたときに Windows が作った、この exe の
 *  ファイアウォールの規則を数え、「許可を消す」で消す(管理者で。fwrules.c)。
 *  サービスとして登録しているときは、サービス用の規則(FW_RULE)は数えず、消さない。
 * ================================================================== */

#include "iiv.h"
#include "resource.h"
#include <dwmapi.h>

#define TIMER_STATUS 1

static HWND  g_dlg;
static HFONT g_heading;
static int   g_footerTop;
static int   g_ndisplays;

static void fw_refresh(HWND dlg, const WCHAR *done);

BOOL ui_settings_open(void) { return g_dlg != NULL; }

BOOL ui_dialog_message(MSG *msg)
{
    return g_dlg && IsDialogMessageW(g_dlg, msg);
}

static void set_status(HWND dlg)
{
    WCHAR s[1024], addrs[512], list[2048];
    int   n;
    if (g_uiService) {
        SvcStatus st;
        if (!svc_read_status(&st)) {
            lstrcpyW(s, L"サービスとして登録されていますが、まだ動いていません(始まるまで数秒かかります)。");
            n = 0;
        } else {
            app_listen_addresses(addrs, ARRAYSIZE(addrs));
            if (!st.listening)
                _snwprintf(s, ARRAYSIZE(s), L"サービスとして動いています。%s", st.error[0] ? st.error : L"待ち受けていません。");
            else if (st.listen[0])
                _snwprintf(s, ARRAYSIZE(s), L"サービスとして動いています。%s:%ld で待ち受けています。\n取り込み: %s",
                           st.listen, st.port, lstrcmpW(st.method, L"-") ? st.method : L"(接続が来たら始めます)");
            else
                _snwprintf(s, ARRAYSIZE(s), L"サービスとして動いています。ポート %ld で待ち受けています。このPC のアドレス: %s\n取り込み: %s",
                           st.port, addrs[0] ? addrs : L"(不明)", lstrcmpW(st.method, L"-") ? st.method : L"(接続が来たら始めます)");
            lstrcpynW(list, st.clientList, ARRAYSIZE(list));
            n = st.clients;
        }
        s[ARRAYSIZE(s) - 1] = 0;
        SetDlgItemTextW(dlg, IDC_STATUS, s);
        goto clients;
    }
    if (app_listening()) {
        const WCHAR *method = lstrcmpW(g_scr.method, L"-") ? g_scr.method : L"(接続が来たら始めます)";
        app_listen_addresses(addrs, ARRAYSIZE(addrs));
        if (g_cfg.listen[0])
            _snwprintf(s, ARRAYSIZE(s), L"%s:%d で待ち受けています。\n取り込み: %s", g_cfg.listen, g_cfg.port, method);
        else
            _snwprintf(s, ARRAYSIZE(s), L"ポート %d で待ち受けています。このPC のアドレス: %s\n取り込み: %s",
                       g_cfg.port, addrs[0] ? addrs : L"(不明)", method);
    } else {
        _snwprintf(s, ARRAYSIZE(s), L"%s", app_listen_error()[0] ? app_listen_error() : L"待ち受けていません。");
    }
    s[ARRAYSIZE(s) - 1] = 0;
    SetDlgItemTextW(dlg, IDC_STATUS, s);

    n = server_list(list, ARRAYSIZE(list));
clients:
    SendDlgItemMessageW(dlg, IDC_CLIENTS, WM_SETREDRAW, FALSE, 0);
    SendDlgItemMessageW(dlg, IDC_CLIENTS, LB_RESETCONTENT, 0, 0);
    if (n) {
        WCHAR *p, *e;
        for (p = list; p && *p; p = e) {
            e = wcschr(p, L'\n');
            if (e) *e++ = 0;
            SendDlgItemMessageW(dlg, IDC_CLIENTS, LB_ADDSTRING, 0, (LPARAM)p);
        }
    } else {
        SendDlgItemMessageW(dlg, IDC_CLIENTS, LB_ADDSTRING, 0, (LPARAM)L"(なし)");
    }
    SendDlgItemMessageW(dlg, IDC_CLIENTS, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(GetDlgItem(dlg, IDC_CLIENTS), NULL, TRUE);
    EnableWindow(GetDlgItem(dlg, IDC_DISCONNECT), n > 0);
}

void ui_refresh_status(void)
{
    if (g_dlg) set_status(g_dlg);
}

static BOOL CALLBACK count_mon(HMONITOR hm, HDC dc, LPRECT rc, LPARAM lp)
{
    MONITORINFOEXW mi;
    HWND combo = (HWND)lp;
    (void)dc; (void)rc;
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hm, (MONITORINFO *)&mi)) {
        const WCHAR *p = wcsstr(mi.szDevice, L"DISPLAY");
        int num = p ? _wtoi(p + 7) : 0;
        WCHAR s[96];
        int idx;
        wsprintfW(s, L"ディスプレイ %d(%d×%d)%s", num, mi.rcMonitor.right - mi.rcMonitor.left,
                  mi.rcMonitor.bottom - mi.rcMonitor.top, (mi.dwFlags & MONITORINFOF_PRIMARY) ? L" メイン" : L"");
        idx = (int)SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)s);
        SendMessageW(combo, CB_SETITEMDATA, (WPARAM)idx, (LPARAM)num);
        g_ndisplays++;
    }
    return TRUE;
}

static void fill(HWND dlg)
{
    HWND lc = GetDlgItem(dlg, IDC_LISTEN), dc = GetDlgItem(dlg, IDC_DISPLAY);
    int  i, n;

    SendDlgItemMessageW(dlg, IDC_PASSWORD, EM_LIMITTEXT, 64, 0);
    SendDlgItemMessageW(dlg, IDC_VIEWPW, EM_LIMITTEXT, 64, 0);
    SetDlgItemTextW(dlg, IDC_PASSWORD, L"");
    SetDlgItemTextW(dlg, IDC_VIEWPW, L"");
    SetDlgItemTextW(dlg, IDC_PWNOTE, g_cfg.pw.set ? L"設定してあります" : L"未設定");
    CheckDlgButton(dlg, IDC_USEVIEWPW, g_cfg.viewPw.set ? BST_CHECKED : BST_UNCHECKED);
    EnableWindow(GetDlgItem(dlg, IDC_VIEWPW), g_cfg.viewPw.set);
    SetDlgItemInt(dlg, IDC_PORT, (UINT)g_cfg.port, FALSE);

    SendMessageW(lc, CB_RESETCONTENT, 0, 0);
    SendMessageW(lc, CB_ADDSTRING, 0, (LPARAM)L"ほかの PC からも受け付ける");
    SendMessageW(lc, CB_ADDSTRING, 0, (LPARAM)L"この PC の中からだけ(127.0.0.1)");
    if (g_cfg.listen[0] && lstrcmpW(g_cfg.listen, L"127.0.0.1")) {
        WCHAR s[96];
        wsprintfW(s, L"%s だけ(ini で指定)", g_cfg.listen);
        SendMessageW(lc, CB_ADDSTRING, 0, (LPARAM)s);
        SendMessageW(lc, CB_SETCURSEL, 2, 0);
    } else {
        SendMessageW(lc, CB_SETCURSEL, g_cfg.listen[0] ? 1 : 0, 0);
    }

    SendMessageW(dc, CB_RESETCONTENT, 0, 0);
    SendMessageW(dc, CB_ADDSTRING, 0, (LPARAM)L"すべての画面");
    SendMessageW(dc, CB_SETITEMDATA, 0, 0);
    g_ndisplays = 0;
    EnumDisplayMonitors(NULL, NULL, count_mon, (LPARAM)dc);
    n = (int)SendMessageW(dc, CB_GETCOUNT, 0, 0);
    SendMessageW(dc, CB_SETCURSEL, 0, 0);
    for (i = 0; i < n; i++)
        if ((int)SendMessageW(dc, CB_GETITEMDATA, (WPARAM)i, 0) == g_cfg.display) SendMessageW(dc, CB_SETCURSEL, (WPARAM)i, 0);

    if (g_uiService) {
        SetDlgItemTextW(dlg, IDC_SVCTEXT, L"サービスとして動いています。ログイン前・ロック中・UAC の確認画面でも接続できます。");
        SetDlgItemTextW(dlg, IDC_SERVICE, L"サービスをやめる(&V)...");
    } else {
        SetDlgItemTextW(dlg, IDC_SVCTEXT, L"サービスとして登録すると、ログイン前・ロック中・UAC の確認画面でも接続できます。");
        SetDlgItemTextW(dlg, IDC_SERVICE, L"サービスにする(&V)...");
    }
    CheckDlgButton(dlg, IDC_VIEWONLY, g_cfg.viewOnly ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_NOTIFY, g_cfg.notify ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_SHOWCURSOR, g_cfg.showCursor ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_NOSLEEP, g_cfg.noSleep ? BST_CHECKED : BST_UNCHECKED);
    {
        WCHAR s[MAX_PATH + 16];
        wsprintfW(s, L"設定: %s", g_iniPath);
        SetDlgItemTextW(dlg, IDC_INIPATH, s);
    }
    fw_refresh(dlg, NULL);
}

/* サービスとして登録しているなら、サービス用の規則は残す(ログイン前の接続に要る) */
static const WCHAR *fw_keep(void)
{
    return (g_uiService || svc_installed()) ? FW_RULE : NULL;
}

/* 「ネットワークの許可」の欄を書き直す。done があれば、それを出す */
static void fw_refresh(HWND dlg, const WCHAR *done)
{
    FwInfo fi;
    WCHAR  s[300];
    if (!fw_query(fw_keep(), &fi)) lstrcpyW(s, L"Windows ファイアウォールの設定を読めませんでした。");
    else fw_describe(&fi, s, ARRAYSIZE(s));
    SetDlgItemTextW(dlg, IDC_FWTEXT, done ? done : s);
    EnableWindow(GetDlgItem(dlg, IDC_FWREMOVE), fi.count > 0);
}

static void remove_firewall(HWND dlg)
{
    WCHAR args[MAX_PATH + 40], s[200];
    int   n;
    swprintf(args, ARRAYSIZE(args), L"-remove-firewall -ini \"%s\"", g_iniPath);
    n = fw_remove_elevated(dlg, fw_keep(), args);
    if (n < 0) lstrcpyW(s, L"消せませんでした(管理者の確認を断ったか、失敗しました)。");
    else swprintf(s, ARRAYSIZE(s), L"%d 件消しました。次に待ち受けを始めるとき、Windows がもう一度たずねます。", n);
    fw_refresh(dlg, s);
}

static int message(HWND owner, const WCHAR *main, const WCHAR *content)
{
    TASKDIALOGCONFIG tc;
    int pressed = IDCANCEL;
    ZeroMemory(&tc, sizeof(tc));
    tc.cbSize = sizeof(tc);
    tc.hwndParent = owner;
    tc.hInstance = g_inst;
    tc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW | TDF_SIZE_TO_CONTENT;
    tc.pszWindowTitle = APP_NAME;
    tc.pszMainIcon = TD_WARNING_ICON;
    tc.pszMainInstruction = main;
    tc.pszContent = content;
    tc.dwCommonButtons = TDCBF_OK_BUTTON;
    if (FAILED(TaskDialogIndirect(&tc, &pressed, NULL, NULL))) pressed = IDCANCEL;
    return pressed;
}

static BOOL apply(HWND dlg)
{
    WCHAR pw[16], vpw[16];
    BOOL  ok, restart = FALSE, recapture = FALSE;
    UINT  port = GetDlgItemInt(dlg, IDC_PORT, &ok, FALSE);
    int   sel = (int)SendDlgItemMessageW(dlg, IDC_LISTEN, CB_GETCURSEL, 0, 0);
    int   disp = (int)SendDlgItemMessageW(dlg, IDC_DISPLAY, CB_GETITEMDATA,
                                          (WPARAM)SendDlgItemMessageW(dlg, IDC_DISPLAY, CB_GETCURSEL, 0, 0), 0);
    WCHAR listen[64];
    BOOL  useV = IsDlgButtonChecked(dlg, IDC_USEVIEWPW) == BST_CHECKED;

    if (!ok || port < 1 || port > 65535) {
        message(dlg, L"ポートは 1～65535 で指定してください。", NULL);
        return FALSE;
    }
    GetDlgItemTextW(dlg, IDC_PASSWORD, pw, ARRAYSIZE(pw));
    GetDlgItemTextW(dlg, IDC_VIEWPW, vpw, ARRAYSIZE(vpw));

    if (sel == 0) listen[0] = 0;
    else if (sel == 1) lstrcpyW(listen, L"127.0.0.1");
    else lstrcpyW(listen, g_cfg.listen);

    if (!pw[0] && !g_cfg.pw.set && sel != 1) {
        message(dlg, L"パスワードを設定してください。",
                L"パスワードが無いと、ほかの PC からは誰でも接続できてしまうため、待ち受けません。"
                L"この PC の中からだけ受け付けるなら、パスワードは無くても動きます。");
        SetFocus(GetDlgItem(dlg, IDC_PASSWORD));
        return FALSE;
    }

    if ((int)port != g_cfg.port || lstrcmpW(listen, g_cfg.listen)) restart = TRUE;
    if (disp != g_cfg.display) recapture = TRUE;
    if (!g_cfg.pw.set && pw[0]) restart = TRUE;
    g_cfg.port = (int)port;
    lstrcpyW(g_cfg.listen, listen);
    if (pw[0]) auth_set_password(&g_cfg.pw, pw);
    if (!useV) ZeroMemory(&g_cfg.viewPw, sizeof(g_cfg.viewPw));
    else if (vpw[0]) auth_set_password(&g_cfg.viewPw, vpw);
    g_cfg.display = disp;
    g_cfg.viewOnly = IsDlgButtonChecked(dlg, IDC_VIEWONLY) == BST_CHECKED;
    g_cfg.notify = IsDlgButtonChecked(dlg, IDC_NOTIFY) == BST_CHECKED;
    g_cfg.showCursor = IsDlgButtonChecked(dlg, IDC_SHOWCURSOR) == BST_CHECKED;
    g_cfg.noSleep = IsDlgButtonChecked(dlg, IDC_NOSLEEP) == BST_CHECKED;
    SecureZeroMemory(pw, sizeof(pw));
    SecureZeroMemory(vpw, sizeof(vpw));

    if (!config_save()) message(dlg, L"設定を保存できませんでした。", g_iniPath);
    if (g_uiService) {
        /* サービス: ファイアウォールの規則のポートを合わせ、分身に読み直させる */
        if (restart) svc_firewall(lstrcmpW(g_cfg.listen, L"127.0.0.1") != 0);
        svc_signal_reload();
        return TRUE;
    }
    power_update();
    if (recapture) video_reset();
    if (restart || !app_listening()) PostMessageW(g_mainWnd, WM_APP_RESTART, 0, 0);
    return TRUE;
}

static int confirm(HWND owner, const WCHAR *main, const WCHAR *content, const WCHAR *yes, PCWSTR icon)
{
    TASKDIALOGCONFIG  tc;
    TASKDIALOG_BUTTON b[1];
    int pressed = IDCANCEL;
    b[0].nButtonID = IDYES;
    b[0].pszButtonText = yes;
    ZeroMemory(&tc, sizeof(tc));
    tc.cbSize = sizeof(tc);
    tc.hwndParent = owner;
    tc.hInstance = g_inst;
    tc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW | TDF_SIZE_TO_CONTENT;
    tc.pszWindowTitle = APP_NAME;
    tc.pszMainIcon = icon;
    tc.pszMainInstruction = main;
    tc.pszContent = content;
    tc.pButtons = b;
    tc.cButtons = 1;
    tc.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    tc.nDefaultButton = IDCANCEL;
    if (FAILED(TaskDialogIndirect(&tc, &pressed, NULL, NULL))) pressed = IDCANCEL;
    return pressed;
}

/* ふだんのトレイ常駐から: サービスとして登録する */
static void install_service(HWND dlg)
{
    WCHAR who[256], text[1024];
    DWORD code = 1;

    if (!apply(dlg)) return;
    if (!g_cfg.pw.set && !g_cfg.viewPw.set && lstrcmpW(g_cfg.listen, L"127.0.0.1")) {
        message(dlg, L"パスワードを設定してから登録してください。", NULL);
        return;
    }
    if (svc_path_risky(who, ARRAYSIZE(who))) {
        _snwprintf(text, ARRAYSIZE(text),
                   L"iiv-server.exe のある場所(%s)は、%s が書き換えられます。\n\n"
                   L"サービスは SYSTEM(この PC のすべての権限)で動くので、この exe を差し替えられると、"
                   L"差し替えた人やプログラムが PC のすべての権限を得られます。"
                   L"自分しか使わない PC なら、そのままでも実害は小さいです。",
                   g_exeDir, who);
        text[ARRAYSIZE(text) - 1] = 0;
        if (confirm(dlg, L"この場所のままサービスにしますか？", text, L"このまま登録する", TD_WARNING_ICON) != IDYES) return;
    }
    _snwprintf(text, ARRAYSIZE(text),
               L"Windows の起動時から動き、ログイン前・ロック中・UAC の確認画面でも接続できるようになります。\n\n"
               L"・管理者の確認が出ます。\n"
               L"・Ctrl+Alt+Del を送れるよう、Windows のポリシー(SoftwareSASGeneration)を設定します。サービスをやめると元に戻します。\n"
               L"・Windows ファイアウォールに、ポート %d への受信を許可する規則を足します。\n"
               L"・このトレイ常駐は終わり、サービスがトレイを出し直します。",
               g_cfg.port);
    text[ARRAYSIZE(text) - 1] = 0;
    if (confirm(dlg, L"サービスとして登録しますか？", text, L"登録する", TD_SHIELD_ICON) != IDYES) return;

    server_stop();                      /* ポートを空ける(サービスの分身が使う) */
    if (!svc_run_elevated(L"-install-service", dlg, TRUE, &code) || code != 0) {
        PostMessageW(g_mainWnd, WM_APP_RESTART, 0, 0);
        return;
    }
    log_printf(L"サービスとして登録した。トレイ常駐を終わる");
    DestroyWindow(dlg);
    PostMessageW(g_mainWnd, WM_APP_COMMAND, CMD_EXIT, 0);
}

/* サービスの設定画面(管理者)から: サービスをやめる */
static void uninstall_service(HWND dlg)
{
    if (confirm(dlg, L"サービスをやめますか？",
                L"ログイン前・ロック中・UAC の確認画面では接続できなくなります。\n"
                L"Ctrl+Alt+Del のポリシーとファイアウォールの規則は元に戻します。\n"
                L"ログインしているなら、ふだんのトレイ常駐で動き直します。",
                L"サービスをやめる", TD_WARNING_ICON) != IDYES) return;
    if (svc_uninstall(dlg)) DestroyWindow(dlg);
}

static void apply_theme(HWND dlg)
{
    theme_apply_dialog(dlg);
}

static INT_PTR CALLBACK dlg_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG: {
        LOGFONTW lf;
        HFONT    base = (HFONT)SendMessageW(dlg, WM_GETFONT, 0, 0);
        RECT     r, pad = { 0, 0, 0, 7 };
        static const int heads[] = { IDC_H_STATUS, IDC_H_CLIENTS, IDC_H_CONN, IDC_H_SERVICE, IDC_H_FW };
        int i;
        HICON ic = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
        HICON ib = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
        SendMessageW(dlg, WM_SETICON, ICON_SMALL, (LPARAM)ic);
        SendMessageW(dlg, WM_SETICON, ICON_BIG, (LPARAM)ib);
        if (base && GetObjectW(base, sizeof(lf), &lf)) {
            lf.lfWeight = FW_SEMIBOLD;
            lf.lfHeight = MulDiv(lf.lfHeight, 118, 100);
            g_heading = CreateFontIndirectW(&lf);
        }
        for (i = 0; i < (int)ARRAYSIZE(heads); i++) if (g_heading) SendDlgItemMessageW(dlg, heads[i], WM_SETFONT, (WPARAM)g_heading, TRUE);
        GetWindowRect(GetDlgItem(dlg, IDOK), &r);
        MapWindowPoints(NULL, dlg, (POINT *)&r, 2);
        MapDialogRect(dlg, &pad);
        g_footerTop = r.top - pad.bottom;
        SetPropW(dlg, L"iiv.footer", (HANDLE)(INT_PTR)g_footerTop);
        fill(dlg);
        set_status(dlg);
        apply_theme(dlg);
        SetTimer(dlg, TIMER_STATUS, 1000, NULL);
        return TRUE;
    }

    case WM_TIMER:
        if (wp == TIMER_STATUS) set_status(dlg);
        return TRUE;

    case WM_ERASEBKGND: {
        HDC  dc = (HDC)wp;
        RECT c, f;
        GetClientRect(dlg, &c);
        f = c;
        c.bottom = g_footerTop;
        f.top = g_footerTop;
        FillRect(dc, &f, theme_footer_brush());
        {
            HBRUSH line = CreateSolidBrush(theme_line());
            RECT l = f;
            l.bottom = l.top + 1;
            FillRect(dc, &l, line);
            DeleteObject(line);
        }
        FillRect(dc, &c, theme_back_brush());
        SetWindowLongPtrW(dlg, DWLP_MSGRESULT, 1);
        return TRUE;
    }

    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        int id = GetDlgCtrlID((HWND)lp);
        LRESULT r = theme_ctlcolor(msg, (HDC)wp, (HWND)lp, id == IDC_INIPATH || id == IDC_HINT_PW || id == IDC_PWNOTE);
        if (r) return (INT_PTR)r;
        break;
    }

    case WM_NOTIFY: {
        NMHDR *n = (NMHDR *)lp;
        LRESULT res;
        if (n->code == NM_CUSTOMDRAW && theme_custom_draw_button((NMCUSTOMDRAW *)lp, &res)) {
            SetWindowLongPtrW(dlg, DWLP_MSGRESULT, res);
            return TRUE;
        }
        break;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_USEVIEWPW:
            EnableWindow(GetDlgItem(dlg, IDC_VIEWPW), IsDlgButtonChecked(dlg, IDC_USEVIEWPW) == BST_CHECKED);
            return TRUE;
        case IDC_DISCONNECT:
            if (g_uiService) svc_signal_disconnect();
            else server_disconnect_all();
            return TRUE;
        case IDC_SERVICE:
            if (g_uiService) uninstall_service(dlg);
            else install_service(dlg);
            return TRUE;
        case IDC_FWREMOVE:
            remove_firewall(dlg);
            return TRUE;
        case IDOK:
            if (apply(dlg)) DestroyWindow(dlg);
            return TRUE;
        case IDCANCEL:
            DestroyWindow(dlg);
            return TRUE;
        }
        break;

    case WM_DESTROY:
        KillTimer(dlg, TIMER_STATUS);
        if (g_heading) DeleteObject(g_heading);
        g_heading = NULL;
        RemovePropW(dlg, L"iiv.footer");
        g_dlg = NULL;
        if (g_uiService) PostQuitMessage(0);     /* 設定画面だけのプロセス */
        return TRUE;
    }
    return FALSE;
}

void ui_show_settings(HWND owner)
{
    if (g_dlg) {
        ShowWindow(g_dlg, SW_RESTORE);
        SetForegroundWindow(g_dlg);
        return;
    }
    g_dlg = CreateDialogParamW(g_inst, MAKEINTRESOURCEW(IDD_SETTINGS), owner, dlg_proc, 0);
    if (g_dlg) {
        ShowWindow(g_dlg, SW_SHOW);
        SetForegroundWindow(g_dlg);
    }
}

void ui_theme_changed(void)
{
    if (g_dlg) apply_theme(g_dlg);
}
