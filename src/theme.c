/* ==================================================================
 * theme.c - システムのライト/ダークに合わせた配色
 *
 *  kotemado / mayous の theme.c と同じもの。Win32 の古いコントロールには
 *  ダークモードの公式 API が無いので、Windows 自身(エクスプローラなど)と
 *  同じく uxtheme.dll の序数エクスポートを使う。取れなければライトのまま動く。
 *
 *      序数 104  RefreshImmersiveColorPolicyState()
 *      序数 132  ShouldAppsUseDarkMode() -> BYTE
 *      序数 133  AllowDarkModeForWindow(HWND, BYTE)
 *      序数 135  SetPreferredAppMode(1 = AllowDark)
 *      序数 136  FlushMenuThemes()
 *
 *  見た目は Windows 11 の標準ダイアログ(タスク ダイアログ)に寄せる。
 *  本文は白(ダークでは #202020)、下のボタン帯は一段濃い色にする。
 *
 *  テーマが効かないもの(ダークのチェックボックスの文字、一覧の見出し)は
 *  こちらで描く。
 * ================================================================== */

#include "iiv.h"
#include <dwmapi.h>
#include <uxtheme.h>
#include <vssym32.h>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#define DWMWA_USE_IMMERSIVE_DARK_MODE_OLD 19

typedef BYTE (WINAPI *fnShouldAppsUseDarkMode)(void);
typedef BYTE (WINAPI *fnAllowDarkModeForWindow)(HWND, BYTE);
typedef int  (WINAPI *fnSetPreferredAppMode)(int);
typedef void (WINAPI *fnVoid)(void);

static fnShouldAppsUseDarkMode  p_ShouldAppsUseDarkMode;
static fnAllowDarkModeForWindow p_AllowDarkModeForWindow;
static fnSetPreferredAppMode    p_SetPreferredAppMode;
static fnVoid                   p_RefreshImmersiveColorPolicyState;
static fnVoid                   p_FlushMenuThemes;

static BOOL   g_ready, g_dark;
static HBRUSH g_brBack, g_brFooter, g_brCtrl;

void theme_init(void)
{
    HMODULE ux;

    if (g_ready) return;
    g_ready = TRUE;

    ux = LoadLibraryExW(L"uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (ux) {
        p_RefreshImmersiveColorPolicyState = (fnVoid)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(104));
        p_ShouldAppsUseDarkMode  = (fnShouldAppsUseDarkMode)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(132));
        p_AllowDarkModeForWindow = (fnAllowDarkModeForWindow)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(133));
        p_SetPreferredAppMode    = (fnSetPreferredAppMode)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(135));
        p_FlushMenuThemes        = (fnVoid)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    }
    if (p_SetPreferredAppMode) p_SetPreferredAppMode(1);
    if (p_RefreshImmersiveColorPolicyState) p_RefreshImmersiveColorPolicyState();
    theme_refresh();
}

/* システムの設定を読み直す。変わっていたら TRUE */
BOOL theme_refresh(void)
{
    BOOL dark;

    if (p_RefreshImmersiveColorPolicyState) p_RefreshImmersiveColorPolicyState();
    if (g_cfg.theme == 1)      dark = FALSE;
    else if (g_cfg.theme == 2) dark = TRUE;
    else dark = p_ShouldAppsUseDarkMode ? p_ShouldAppsUseDarkMode() != 0 : FALSE;
    if (g_brBack && dark == g_dark) return FALSE;

    g_dark = dark;
    if (g_brBack)   DeleteObject(g_brBack);
    if (g_brFooter) DeleteObject(g_brFooter);
    if (g_brCtrl)   DeleteObject(g_brCtrl);
    g_brBack   = CreateSolidBrush(theme_back());
    g_brFooter = CreateSolidBrush(theme_footer());
    g_brCtrl   = CreateSolidBrush(theme_ctrl_back());
    if (p_FlushMenuThemes) p_FlushMenuThemes();
    return TRUE;
}

BOOL     theme_is_dark(void)    { return g_dark; }
COLORREF theme_back(void)       { return g_dark ? RGB(32, 32, 32)    : GetSysColor(COLOR_WINDOW); }
COLORREF theme_footer(void)     { return g_dark ? RGB(44, 44, 44)    : GetSysColor(COLOR_BTNFACE); }
COLORREF theme_ctrl_back(void)  { return g_dark ? RGB(45, 45, 45)    : GetSysColor(COLOR_WINDOW); }
COLORREF theme_text(void)       { return g_dark ? RGB(255, 255, 255) : GetSysColor(COLOR_WINDOWTEXT); }
COLORREF theme_dim_text(void)   { return g_dark ? RGB(170, 170, 170) : RGB(96, 96, 96); }
COLORREF theme_line(void)       { return g_dark ? RGB(60, 60, 60)    : RGB(223, 223, 223); }
HBRUSH   theme_back_brush(void)   { return g_brBack; }
HBRUSH   theme_footer_brush(void) { return g_brFooter; }
HBRUSH   theme_ctrl_brush(void)   { return g_brCtrl; }

void theme_allow_dark(HWND hwnd)
{
    if (p_AllowDarkModeForWindow) p_AllowDarkModeForWindow(hwnd, (BYTE)g_dark);
}

static void title_bar(HWND hwnd)
{
    BOOL on = g_dark;
    if (FAILED(DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &on, sizeof(on))))
        DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE_OLD, &on, sizeof(on));
}

/* ------------------------------------------------------------------ */
/*  一覧(ListView)の見出しの文字色                                      */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK lv_subclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    (void)id; (void)ref;
    if (msg == WM_NOTIFY && g_dark) {
        NMHDR *n = (NMHDR *)lp;
        if (n->code == NM_CUSTOMDRAW && n->hwndFrom == ListView_GetHeader(h)) {
            NMCUSTOMDRAW *cd = (NMCUSTOMDRAW *)lp;
            if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
                SetTextColor(cd->hdc, theme_text());
                return CDRF_DODEFAULT;
            }
        }
    }
    return DefSubclassProc(h, msg, wp, lp);
}

static void apply_listview(HWND lv)
{
    HWND hd = ListView_GetHeader(lv);
    SetWindowSubclass(lv, lv_subclass, 1, 0);
    theme_allow_dark(lv);
    SetWindowTheme(lv, g_dark ? L"DarkMode_ItemsView" : L"ItemsView", NULL);
    if (hd) {
        theme_allow_dark(hd);
        SetWindowTheme(hd, g_dark ? L"DarkMode_ItemsView" : NULL, NULL);
    }
    ListView_SetBkColor(lv, theme_ctrl_back());
    ListView_SetTextBkColor(lv, theme_ctrl_back());
    ListView_SetTextColor(lv, theme_text());
}

/* ------------------------------------------------------------------ */
/*  ダイアログ全体                                                      */
/* ------------------------------------------------------------------ */

static BOOL CALLBACK apply_child(HWND c, LPARAM lp)
{
    WCHAR cls[32];
    (void)lp;

    GetClassNameW(c, cls, ARRAYSIZE(cls));
    theme_allow_dark(c);
    if (!lstrcmpiW(cls, WC_LISTVIEWW)) {
        apply_listview(c);
    } else if (!lstrcmpiW(cls, L"Edit")) {
        SetWindowTheme(c, g_dark ? L"DarkMode_CFD" : NULL, NULL);
    } else if (!lstrcmpiW(cls, L"ComboBox")) {
        COMBOBOXINFO ci;
        SetWindowTheme(c, g_dark ? L"DarkMode_CFD" : NULL, NULL);
        ci.cbSize = sizeof(ci);
        if (GetComboBoxInfo(c, &ci) && ci.hwndList) {
            theme_allow_dark(ci.hwndList);
            SetWindowTheme(ci.hwndList, g_dark ? L"DarkMode_Explorer" : NULL, NULL);
        }
    } else if (!lstrcmpiW(cls, L"Button")) {
        SetWindowTheme(c, g_dark ? L"DarkMode_Explorer" : NULL, NULL);
    }
    SendMessageW(c, WM_THEMECHANGED, 0, 0);
    return TRUE;
}

void theme_apply_dialog(HWND dlg)
{
    theme_allow_dark(dlg);
    title_bar(dlg);
    EnumChildWindows(dlg, apply_child, 0);
    RedrawWindow(dlg, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

/* WM_CTLCOLOR* の共通処理。処理しないときは 0 を返す。
   dimText は補足説明の文字(薄い色にする)。 */
LRESULT theme_ctlcolor(UINT msg, HDC dc, HWND ctl, BOOL dimText)
{
    WCHAR cls[16];

    switch (msg) {
    case WM_CTLCOLORDLG:
        return (LRESULT)g_brBack;

    case WM_CTLCOLORSTATIC:
        GetClassNameW(ctl, cls, ARRAYSIZE(cls));
        if (!lstrcmpiW(cls, L"Edit")) {         /* 読み取り専用・無効のエディット */
            if (!g_dark) return 0;
            SetTextColor(dc, theme_dim_text());
            SetBkColor(dc, theme_ctrl_back());
            return (LRESULT)g_brCtrl;
        }
        SetTextColor(dc, dimText ? theme_dim_text() : theme_text());
        SetBkColor(dc, theme_back());
        SetBkMode(dc, TRANSPARENT);
        return (LRESULT)g_brBack;

    case WM_CTLCOLORBTN:
        /* 押しボタンの角の背景。フッタ帯に置いたボタンは帯の色にする */
        {
            RECT r;
            HWND p      = GetParent(ctl);
            int  footer = (int)(INT_PTR)GetPropW(p, L"iiv.footer");
            GetWindowRect(ctl, &r);
            MapWindowPoints(NULL, p, (POINT *)&r, 2);
            if (footer > 0 && r.top >= footer) return (LRESULT)g_brFooter;
        }
        return (LRESULT)g_brBack;

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        if (!g_dark) return 0;
        SetTextColor(dc, theme_text());
        SetBkColor(dc, theme_ctrl_back());
        return (LRESULT)g_brCtrl;
    }
    return 0;
}

/* ダークではチェックボックスの文字色を変えられない(テーマが黒で描く)ので、
   NM_CUSTOMDRAW で箱はテーマに、文字はこちらで描く。 */
BOOL theme_custom_draw_button(NMCUSTOMDRAW *cd, LRESULT *result)
{
    HWND   b = cd->hdr.hwndFrom;
    LONG   st;
    WCHAR  text[256];
    HTHEME th;
    SIZE   box;
    RECT   rc, rb, rt;
    int    part, state, chk, gap;
    HFONT  of;

    if (!g_dark || cd->dwDrawStage != CDDS_PREPAINT) return FALSE;
    st = GetWindowLongW(b, GWL_STYLE) & BS_TYPEMASK;
    if (st == BS_CHECKBOX || st == BS_AUTOCHECKBOX)            part = BP_CHECKBOX;
    else if (st == BS_RADIOBUTTON || st == BS_AUTORADIOBUTTON) part = BP_RADIOBUTTON;
    else return FALSE;

    th = OpenThemeData(b, L"Button");
    if (!th) return FALSE;

    chk = (int)SendMessageW(b, BM_GETCHECK, 0, 0);
    if (cd->uItemState & CDIS_DISABLED)      state = 4;
    else if (cd->uItemState & CDIS_SELECTED) state = 3;
    else if (cd->uItemState & CDIS_HOT)      state = 2;
    else                                     state = 1;
    if (chk == BST_CHECKED) state += 4;          /* CBS_CHECKEDNORMAL など */

    GetClientRect(b, &rc);
    FillRect(cd->hdc, &rc, g_brBack);
    GetThemePartSize(th, cd->hdc, part, state, NULL, TS_DRAW, &box);
    rb.left   = rc.left;
    rb.top    = rc.top + (rc.bottom - rc.top - box.cy) / 2;
    rb.right  = rb.left + box.cx;
    rb.bottom = rb.top + box.cy;
    DrawThemeBackground(th, cd->hdc, part, state, &rb, NULL);
    CloseThemeData(th);

    GetWindowTextW(b, text, ARRAYSIZE(text));
    of  = (HFONT)SelectObject(cd->hdc, (HFONT)SendMessageW(b, WM_GETFONT, 0, 0));
    gap = box.cx / 3;
    rt  = rc;
    rt.left = rb.right + gap;
    SetBkMode(cd->hdc, TRANSPARENT);
    SetTextColor(cd->hdc, (cd->uItemState & CDIS_DISABLED) ? theme_dim_text() : theme_text());
    {
        UINT ui = (UINT)SendMessageW(b, WM_QUERYUISTATE, 0, 0);
        UINT f  = DT_SINGLELINE | DT_VCENTER | DT_LEFT;
        if (ui & UISF_HIDEACCEL) f |= DT_HIDEPREFIX;
        DrawTextW(cd->hdc, text, -1, &rt, f);
        if ((cd->uItemState & CDIS_FOCUS) && !(ui & UISF_HIDEFOCUS)) {
            RECT m = rt;
            int  th2;
            DrawTextW(cd->hdc, text, -1, &m, (f & ~DT_VCENTER) | DT_CALCRECT);
            th2 = m.bottom - m.top;
            m.top    = rt.top + (rt.bottom - rt.top - th2) / 2;
            m.bottom = m.top + th2;
            InflateRect(&m, 1, 0);
            DrawFocusRect(cd->hdc, &m);
        }
    }
    SelectObject(cd->hdc, of);
    *result = CDRF_SKIPDEFAULT;
    return TRUE;
}
