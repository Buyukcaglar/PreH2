/* host_win32.c - Prehistorik 2's Windows x64 host (Prehistorik2.exe, v1.1).
 *
 * The window shows the VGA picture scaled to the client area (4:3 aspect by default, optional integer
 * scaling and smoothing), passes the keyboard to the emulated PC as set-1 scancodes and plays the
 * sound through waveOut (audio_win32.c).  The title bar, the system menu and the About box follow
 * the system's light/dark app mode, and change with it at once (the theme code is the one of the
 * Civ64 project, platform/win32/host_win32.c).  The system menu holds the host's commands: Full screen
 * (Alt+Enter), the display options (with the widescreen modes of qol/wide.c), sound, save states, the
 * key bindings, Locate game files..., About... and Close.  Host keys: F5 saves a state, F7 loads it,
 * F12 takes a screenshot, Tab held runs the game fast, Pause stops it.
 * Settings and save states live in %APPDATA%\PreH2 (PreH2.ini, state1.p2s ...). */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <vssym32.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "../../hal/hal.h"
#include "../../hal/state.h"
#include "../../qol/qol.h"
#include "audio_win32.h"
#include "../crt.h"

#define APP_NAMEW L"Prehistorik 2"
#define APP_VERSIONW L"1.1"
#define ABOUT_URL L"https://github.com/Buyukcaglar/PreH2"

/* ---- state ---- */
static HWND g_hwnd;
static int g_quit;
static int g_running;               /* the emulation is up */
static char g_game_dir[MAX_PATH * 3];
static uint32_t g_frame[640 * 480];
static int g_fw = 320, g_fh = 200;
static int g_opt_crt = 0;   /* Display > CRT scanlines (crt.h) */
static int g_opt_aspect = 1, g_opt_integer = 0, g_opt_smooth = 0, g_opt_full = 0, g_opt_mute = 0, g_opt_pause_inactive = 1;
static int g_opt_wide = WIDE_OFF;    /* Display > Widescreen: the playfield's width (qol.h) */
static int g_opt_volume = 100, g_opt_slot = 1;
static int g_fa_w = 4, g_fa_h = 3;   /* the picture's shape (HostFrame) */
static int g_inactive;               /* the window is not the foreground window */
static int g_user_pause;             /* the Pause key */
static int g_turbo;                  /* Tab is held: fast forward */
static int g_pending;                /* asked for between frames: 1 save state, 2 load state, 3 screenshot */
static wchar_t g_note[96];           /* a short message over the picture ... */
static uint64_t g_note_until;        /* ... until this time (host_time_us) */
static WINDOWPLACEMENT g_wp = {sizeof(WINDOWPLACEMENT)};
static int g_menu_open;             /* a modal loop (menu, About) runs: the game waits */
static uint8_t g_keys_down[512];    /* scancode | ext<<8 held */
static LARGE_INTEGER g_qpf;
static HANDLE g_timer;
static wchar_t g_ini[MAX_PATH];

/* ---- settings ---- */
static void ini_path(void)
{
    wchar_t dir[MAX_PATH];
    if (SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, dir) != S_OK) {
        (void)GetModuleFileNameW(NULL, g_ini, MAX_PATH);
        wcscpy(wcsrchr(g_ini, L'\\') + 1, L"PreH2.ini");
        return;
    }
    (void)swprintf(g_ini, MAX_PATH, L"%ls\\PreH2", dir);
    (void)CreateDirectoryW(g_ini, NULL);
    (void)wcscat(g_ini, L"\\PreH2.ini");
}
static int ini_int(const wchar_t *key, int def)
{
    return (int)GetPrivateProfileIntW(L"PreH2", key, def, g_ini);
}
static void ini_set_int(const wchar_t *key, int v)
{
    wchar_t b[16];
    (void)swprintf(b, 16, L"%d", v);
    (void)WritePrivateProfileStringW(L"PreH2", key, b, g_ini);
}
static void save_options(void)
{
    ini_set_int(L"Aspect", g_opt_aspect);
    ini_set_int(L"Integer", g_opt_integer);
    ini_set_int(L"Smooth", g_opt_smooth);
    ini_set_int(L"Crt", g_opt_crt);
    ini_set_int(L"FullScreen", g_opt_full);
    ini_set_int(L"Mute", g_opt_mute);
    ini_set_int(L"PauseInactive", g_opt_pause_inactive);
    ini_set_int(L"Wide", g_opt_wide);
    ini_set_int(L"Volume", g_opt_volume);
    ini_set_int(L"Slot", g_opt_slot);
}

/* ---- the game folder ---- */
static int has_game(const wchar_t *dir)
{
    wchar_t p[MAX_PATH * 2];
    (void)swprintf(p, MAX_PATH * 2, L"%ls\\PRE2.SQZ", dir);
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}
/* dir, or its PREH2 subfolder: the folder with PRE2.SQZ into out; 1 when found */
static int resolve_game_dir(const wchar_t *dir, wchar_t *out)
{
    wchar_t sub[MAX_PATH * 2];
    if (dir == NULL || dir[0] == 0) return 0;
    if (has_game(dir)) { wcsncpy(out, dir, MAX_PATH * 2 - 1); return 1; }
    (void)swprintf(sub, MAX_PATH * 2, L"%ls\\PREH2", dir);
    if (has_game(sub)) { wcsncpy(out, sub, MAX_PATH * 2 - 1); return 1; }
    return 0;
}
static void set_game_dir(const wchar_t *w)
{
    (void)WideCharToMultiByte(CP_UTF8, 0, w, -1, g_game_dir, (int)sizeof g_game_dir, NULL, NULL);
}
const char *host_game_dir(void)
{
    return g_game_dir;
}

/* the folder picker; 1 when a folder with the game was chosen */
static int pick_game_dir(HWND owner, wchar_t *out)
{
    IFileOpenDialog *d = NULL;
    int ok = 0;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&d)))
        return 0;
    for (;;) {
        DWORD opts = 0;
        IShellItem *it = NULL;
        PWSTR path = NULL;
        (void)d->lpVtbl->GetOptions(d, &opts);
        (void)d->lpVtbl->SetOptions(d, opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        (void)d->lpVtbl->SetTitle(d, L"Locate the Prehistorik 2 game folder (the one with PRE2.SQZ)");
        if (FAILED(d->lpVtbl->Show(d, owner))) break;
        if (SUCCEEDED(d->lpVtbl->GetResult(d, &it)) && SUCCEEDED(it->lpVtbl->GetDisplayName(it, SIGDN_FILESYSPATH, &path))) {
            ok = resolve_game_dir(path, out);
            CoTaskMemFree(path);
        }
        if (it) it->lpVtbl->Release(it);
        if (ok) break;
        if (MessageBoxW(owner, L"PRE2.SQZ was not found in that folder or in its PREH2 subfolder.\n\n"
                               L"Please choose the folder of your Prehistorik 2 (DOS) installation.",
                        APP_NAMEW, MB_RETRYCANCEL | MB_ICONWARNING) != IDRETRY)
            break;
    }
    d->lpVtbl->Release(d);
    return ok;
}

/* a private build (build.cmd private) carries the game files as RCDATA 100 (tools/mkpak.py) */
static int load_embedded_pack(void)
{
    HRSRC r = FindResourceW(NULL, MAKEINTRESOURCEW(100), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    HGLOBAL g;
    const void *p;
    if (r == NULL || (g = LoadResource(NULL, r)) == NULL || (p = LockResource(g)) == NULL) return 0;
    if (!hal_set_pack((const uint8_t *)p, (size_t)SizeofResource(NULL, r))) return 0;
    (void)strcpy(g_game_dir, ".");
    return 1;
}

static int find_game_dir(const wchar_t *cmdline_dir)
{
    wchar_t found[MAX_PATH * 2], buf[MAX_PATH * 2], *slash;
    if (resolve_game_dir(cmdline_dir, found)) goto ok;
    (void)GetPrivateProfileStringW(L"PreH2", L"GameDir", L"", buf, MAX_PATH * 2, g_ini);
    if (resolve_game_dir(buf, found)) goto ok;
    (void)GetModuleFileNameW(NULL, buf, MAX_PATH * 2);
    if ((slash = wcsrchr(buf, L'\\')) != NULL) *slash = 0;
    if (resolve_game_dir(buf, found)) goto ok;
    (void)GetCurrentDirectoryW(MAX_PATH * 2, buf);
    if (resolve_game_dir(buf, found)) goto ok;
    MessageBoxW(NULL, L"Prehistorik 2 needs the files of the original DOS game.\n\n"
                      L"In the next window, choose the folder of your Prehistorik 2 installation "
                      L"(the folder with PRE2.SQZ, or the one above it).",
                APP_NAMEW, MB_OK | MB_ICONINFORMATION);
    if (!pick_game_dir(NULL, found)) return 0;
ok:
    (void)WritePrivateProfileStringW(L"PreH2", L"GameDir", found, g_ini);
    set_game_dir(found);
    return 1;
}

/* ---- theme (the Civ64 code) ---- */
#define THEME_DARK_BG RGB(32, 32, 32)
#define THEME_DARK_FIELD RGB(43, 43, 43)
#define THEME_DARK_TEXT RGB(255, 255, 255)
#define THEME_DARK_LINK RGB(96, 205, 255)
typedef int(WINAPI *ThemeModeFn)(int);
typedef BOOL(WINAPI *ThemeWindowFn)(HWND, BOOL);
typedef void(WINAPI *ThemeVoidFn)(void);
static ThemeModeFn g_ux_mode;
static ThemeWindowFn g_ux_window;
static ThemeVoidFn g_ux_refresh;
static ThemeVoidFn g_ux_flush;
static int g_ux_tried;
static DWORD g_os_build;
static int g_dark;

static DWORD os_build(void)
{
    typedef LONG(WINAPI * VersionFn)(OSVERSIONINFOW *);
    OSVERSIONINFOW v;
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    VersionFn fn = nt != NULL ? (VersionFn)(void *)GetProcAddress(nt, "RtlGetVersion") : NULL;
    memset(&v, 0, sizeof v);
    v.dwOSVersionInfoSize = sizeof v;
    if (fn == NULL || fn(&v) != 0 || v.dwMajorVersion < 10u) return 0u;
    return v.dwBuildNumber;
}
static void theme_load(void)
{
    HMODULE ux;
    if (g_ux_tried) return;
    g_ux_tried = 1;
    g_os_build = os_build();
    if (g_os_build < 18362u || (ux = LoadLibraryExW(L"uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32)) == NULL) return;
    g_ux_mode = (ThemeModeFn)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    g_ux_window = (ThemeWindowFn)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(133));
    g_ux_refresh = (ThemeVoidFn)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(104));
    g_ux_flush = (ThemeVoidFn)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    if (g_ux_mode == NULL || g_ux_window == NULL || g_ux_refresh == NULL || g_ux_flush == NULL) {
        g_ux_mode = NULL; g_ux_window = NULL; g_ux_refresh = NULL; g_ux_flush = NULL;
    }
}
static int theme_wanted(void)
{
    DWORD v = 1u, n = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &v, &n) != ERROR_SUCCESS)
        return 0;
    return v == 0u;
}
static int theme_setting(LPARAM l)
{
    return l != 0 && CompareStringOrdinal((LPCWCH)l, -1, L"ImmersiveColorSet", -1, TRUE) == CSTR_EQUAL;
}
static void theme_window(HWND h, int dark)
{
    BOOL on = dark ? TRUE : FALSE;
    theme_load();
    if (g_ux_window != NULL) (void)g_ux_window(h, on);
    if (g_os_build >= 17763u) (void)DwmSetWindowAttribute(h, g_os_build >= 18985u ? 20u : 19u, &on, sizeof on);
}
static void theme_apply(int redraw)
{
    theme_load();
    g_dark = theme_wanted();
    if (g_ux_mode != NULL) {
        (void)g_ux_mode(g_dark ? 2 : 3);
        g_ux_refresh();
        g_ux_flush();
    }
    if (g_hwnd != NULL) {
        theme_window(g_hwnd, g_dark);
        if (redraw)
            (void)SetWindowPos(g_hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
}

/* ---- the About box ---- */
#define IDC_ABOUT_ICON 201
#define IDC_ABOUT_LINK 202
#define IDC_ABOUT_LICENSE 203
static const wchar_t g_license[] =
    L"MIT License\r\n\r\n"
    L"Copyright (c) 2026 Onur BUYUKCAGLAR\r\n\r\n"
    L"Permission is hereby granted, free of charge, to any person obtaining a copy of this "
    L"software and associated documentation files (the \"Software\"), to deal in the Software "
    L"without restriction, including without limitation the rights to use, copy, modify, merge, "
    L"publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons "
    L"to whom the Software is furnished to do so, subject to the following conditions:\r\n\r\n"
    L"The above copyright notice and this permission notice shall be included in all copies or "
    L"substantial portions of the Software.\r\n\r\n"
    L"THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, "
    L"INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR "
    L"PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE "
    L"FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR "
    L"OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER "
    L"DEALINGS IN THE SOFTWARE.\r\n\r\n"
    L"The licence covers this port's own code (" APP_NAMEW L" 64bit). Prehistorik 2, its program "
    L"and its data belong to their owners; they are not part of this software, which needs a copy "
    L"of the original game to run.";

static int g_about_dark;
static HBRUSH g_about_bg, g_about_field;

typedef struct Tmpl {
    WORD buf[1024];
    size_t n;
} Tmpl;
static void t_word(Tmpl *t, WORD w) { if (t->n < 1024u) t->buf[t->n++] = w; }
static void t_str(Tmpl *t, const wchar_t *s) { do { t_word(t, (WORD)*s); } while (*s++ != L'\0'); }
static void t_align(Tmpl *t) { if ((t->n & 1u) != 0u) t_word(t, 0); }
static void t_item(Tmpl *t, DWORD style, short x, short y, short cx, short cy, WORD id, WORD cls, const wchar_t *text)
{
    t_align(t);
    t_word(t, LOWORD(style)); t_word(t, HIWORD(style));
    t_word(t, 0); t_word(t, 0);
    t_word(t, (WORD)x); t_word(t, (WORD)y); t_word(t, (WORD)cx); t_word(t, (WORD)cy);
    t_word(t, id);
    t_word(t, 0xFFFF); t_word(t, cls);
    t_str(t, text);
    t_word(t, 0);
}

static void about_theme(HWND dlg)
{
    HWND c = GetDlgItem(dlg, IDC_ABOUT_LICENSE);
    g_about_dark = theme_wanted();
    theme_window(dlg, g_about_dark);
    if (c != NULL) {
        if (g_ux_window != NULL) (void)g_ux_window(c, g_about_dark ? TRUE : FALSE);
        (void)SetWindowTheme(c, g_about_dark ? L"DarkMode_Explorer" : NULL, NULL);
        (void)SendMessageW(c, WM_THEMECHANGED, 0u, 0);
    }
    if ((c = GetDlgItem(dlg, IDOK)) != NULL) (void)InvalidateRect(c, NULL, TRUE);
}

/* the OK button, owner-drawn: no theme draws a dark push button */
static void box_button(const DRAWITEMSTRUCT *di, int dark, HBRUSH bg)
{
    RECT r = di->rcItem;
    wchar_t text[32];
    int pressed = (di->itemState & ODS_SELECTED) != 0u;
    int focus = (di->itemState & ODS_FOCUS) != 0u;
    HFONT font = (HFONT)SendMessageW(di->hwndItem, WM_GETFONT, 0u, 0);
    HGDIOBJ old = font != NULL ? SelectObject(di->hDC, font) : NULL;
    text[0] = L'\0';
    (void)GetWindowTextW(di->hwndItem, text, 32);
    if (dark) {
        HBRUSH face = CreateSolidBrush(pressed ? RGB(45, 45, 45) : RGB(60, 60, 60));
        HBRUSH edge = CreateSolidBrush(focus ? THEME_DARK_LINK : RGB(100, 100, 100));
        if (bg != NULL) (void)FillRect(di->hDC, &r, bg);
        if (face != NULL) { (void)FillRect(di->hDC, &r, face); (void)DeleteObject(face); }
        if (edge != NULL) { (void)FrameRect(di->hDC, &r, edge); (void)DeleteObject(edge); }
        (void)SetTextColor(di->hDC, THEME_DARK_TEXT);
    } else {
        HTHEME th = OpenThemeData(di->hwndItem, L"BUTTON");
        if (th != NULL) {
            (void)FillRect(di->hDC, &r, GetSysColorBrush(COLOR_BTNFACE));
            (void)DrawThemeBackground(th, di->hDC, BP_PUSHBUTTON, pressed ? PBS_PRESSED : PBS_DEFAULTED, &r, NULL);
            (void)CloseThemeData(th);
        } else {
            (void)DrawFrameControl(di->hDC, &r, DFC_BUTTON, DFCS_BUTTONPUSH | (pressed ? DFCS_PUSHED : 0u));
        }
        (void)SetTextColor(di->hDC, GetSysColor(COLOR_BTNTEXT));
        if (focus && (di->itemState & ODS_NOFOCUSRECT) == 0u) {
            RECT f = r;
            (void)InflateRect(&f, -3, -3);
            (void)DrawFocusRect(di->hDC, &f);
        }
    }
    (void)SetBkMode(di->hDC, TRANSPARENT);
    if (pressed) (void)OffsetRect(&r, 1, 1);
    (void)DrawTextW(di->hDC, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | ((di->itemState & ODS_NOACCEL) != 0u ? DT_HIDEPREFIX : 0u));
    if (old != NULL) (void)SelectObject(di->hDC, old);
}

static HICON g_about_icon;   /* the About box's large icon (64 px at 96 dpi) */

static INT_PTR CALLBACK about_proc(HWND dlg, UINT msg, WPARAM w, LPARAM l)
{
    switch (msg) {
    case WM_INITDIALOG: {
        int px = MulDiv(64, (int)GetDpiForWindow(dlg), 96);
        g_about_icon = (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1), IMAGE_ICON, px, px, 0);
        if (g_about_icon != NULL) (void)SendDlgItemMessageW(dlg, IDC_ABOUT_ICON, STM_SETICON, (WPARAM)g_about_icon, 0);
        (void)SetDlgItemTextW(dlg, IDC_ABOUT_LICENSE, g_license);
        g_about_bg = CreateSolidBrush(THEME_DARK_BG);
        g_about_field = CreateSolidBrush(THEME_DARK_FIELD);
        about_theme(dlg);
        (void)SetFocus(GetDlgItem(dlg, IDOK));
        return FALSE;
    }
    case WM_DESTROY:
        if (g_about_bg != NULL) { (void)DeleteObject(g_about_bg); g_about_bg = NULL; }
        if (g_about_field != NULL) { (void)DeleteObject(g_about_field); g_about_field = NULL; }
        if (g_about_icon != NULL) { (void)DestroyIcon(g_about_icon); g_about_icon = NULL; }
        break;
    case WM_SETTINGCHANGE:
        if (theme_setting(l)) {
            about_theme(dlg);
            (void)RedrawWindow(dlg, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
        }
        break;
    case WM_DRAWITEM:
        if (w == IDOK) { box_button((const DRAWITEMSTRUCT *)l, g_about_dark, g_about_bg); return TRUE; }
        break;
    case WM_CTLCOLORDLG:
        if (g_about_dark && g_about_bg != NULL) return (INT_PTR)g_about_bg;
        break;
    case WM_CTLCOLORSTATIC: {
        int dark = g_about_dark && g_about_bg != NULL && g_about_field != NULL;
        if ((HWND)l == GetDlgItem(dlg, IDC_ABOUT_LINK)) {
            (void)SetTextColor((HDC)w, dark ? THEME_DARK_LINK : RGB(0, 0, 204));
            (void)SetBkMode((HDC)w, TRANSPARENT);
            return (INT_PTR)(dark ? g_about_bg : GetSysColorBrush(COLOR_BTNFACE));
        }
        if (dark) {
            int field = (HWND)l == GetDlgItem(dlg, IDC_ABOUT_LICENSE);
            (void)SetTextColor((HDC)w, THEME_DARK_TEXT);
            (void)SetBkColor((HDC)w, field ? THEME_DARK_FIELD : THEME_DARK_BG);
            return (INT_PTR)(field ? g_about_field : g_about_bg);
        }
        break;
    }
    case WM_SETCURSOR:
        if ((HWND)w == GetDlgItem(dlg, IDC_ABOUT_LINK)) {
            (void)SetCursor(LoadCursorW(NULL, MAKEINTRESOURCEW(32649)));
            SetWindowLongPtrW(dlg, DWLP_MSGRESULT, TRUE);
            return TRUE;
        }
        break;
    case WM_COMMAND:
        if (LOWORD(w) == IDC_ABOUT_LINK && HIWORD(w) == STN_CLICKED) {
            (void)ShellExecuteW(dlg, L"open", ABOUT_URL, NULL, NULL, SW_SHOWNORMAL);
            return TRUE;
        }
        if (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL) { EndDialog(dlg, 1); return TRUE; }
        break;
    default:
        break;
    }
    return FALSE;
}

static void show_about(void)
{
    Tmpl t;
    DWORD style = DS_MODALFRAME | DS_SETFONT | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    memset(&t, 0, sizeof t);
    t_word(&t, LOWORD(style)); t_word(&t, HIWORD(style));
    t_word(&t, 0); t_word(&t, 0);
    t_word(&t, 7);
    t_word(&t, 0); t_word(&t, 0); t_word(&t, 260); t_word(&t, 206);
    t_word(&t, 0); t_word(&t, 0);
    t_str(&t, L"About " APP_NAMEW L" v" APP_VERSIONW);
    t_word(&t, 9);
    t_str(&t, L"Segoe UI");
    t_item(&t, WS_CHILD | WS_VISIBLE | SS_ICON | SS_REALSIZEIMAGE, 10, 8, 37, 34, IDC_ABOUT_ICON, 0x0082, L"");
    t_item(&t, WS_CHILD | WS_VISIBLE | SS_LEFT, 56, 12, 194, 9, 0xFFFF, 0x0082, L"64bit version by Onur BUYUKCAGLAR");
    t_item(&t, WS_CHILD | WS_VISIBLE | SS_LEFT, 56, 23, 194, 9, 0xFFFF, 0x0082, L"with Anthropic Claude AI assistance");
    t_item(&t, WS_CHILD | WS_VISIBLE | WS_TABSTOP | SS_LEFT | SS_NOTIFY, 56, 34, 194, 9, IDC_ABOUT_LINK, 0x0082, ABOUT_URL);
    t_item(&t, WS_CHILD | WS_VISIBLE | SS_LEFT, 10, 58, 240, 9, 0xFFFF, 0x0082, L"Licenses:");
    t_item(&t, WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
           10, 69, 240, 108, IDC_ABOUT_LICENSE, 0x0081, L"");
    t_item(&t, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 200, 184, 50, 15, IDOK, 0x0080, L"OK");
    g_menu_open++;
    (void)DialogBoxIndirectParamW(GetModuleHandleW(NULL), (LPCDLGTEMPLATEW)t.buf, g_hwnd, about_proc, 0);
    g_menu_open--;
}

/* ---- the picture ---- */
/* The canvas is what the window is laid out for: 320 units wide, or the widescreen width, and 240
 * high (200 without the 4:3 correction).  A picture narrower than the canvas (a menu while the
 * playfield is wide) keeps its own shape in the middle of it. */
static int wide_on(void)
{
    return g_opt_wide > WIDE_OFF && qol_wide_supported();
}
static void canvas_units(int *w, int *h)
{
    *w = wide_on() ? g_opt_wide : 320;
    *h = g_opt_aspect ? 240 : 200;
}
static void dest_rect(int cw, int ch, RECT *r)
{
    int uw, uh;
    double s, dw, dh, fw;
    canvas_units(&uw, &uh);
    if (g_opt_integer) {
        int k = 1;
        while ((k + 1) * uw <= cw && (k + 1) * uh <= ch) k++;
        s = k;
    } else {
        s = (double)cw / uw < (double)ch / uh ? (double)cw / uw : (double)ch / uh;
    }
    dh = uh * s;
    fw = g_opt_aspect ? (double)uh * g_fa_w / g_fa_h : (double)uh * g_fw / g_fh;
    if (fw > uw) fw = uw;
    dw = fw * s;
    r->left = (LONG)((cw - dw) / 2.0 + 0.5);
    r->top = (LONG)((ch - dh) / 2.0 + 0.5);
    r->right = r->left + (LONG)(dw + 0.5);
    r->bottom = r->top + (LONG)(dh + 0.5);
}

/* CRT scanlines: the scaled picture is made in a DIB section, filtered row by row (crt.h), then copied */
static HDC g_crt_dc;
static HBITMAP g_crt_bmp, g_crt_old;
static uint32_t *g_crt_bits;
static int g_crt_w, g_crt_h, g_crt_lines, g_crt_ph;
static uint8_t g_crt_lvl[8192];

/* the picture's height full screen on the window's monitor: the scanlines are spaced as there */
static int crt_period_rows(int dh)
{
    MONITORINFO mi;
    RECT f;
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return dh;
    dest_rect(mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top, &f);
    return f.bottom - f.top > 0 ? f.bottom - f.top : dh;
}

static int crt_surface(HDC dc, int w, int h)
{
    BITMAPINFO bi;
    void *bits = NULL;
    if (g_crt_bmp != NULL && g_crt_w == w && g_crt_h == h) return 1;
    if (g_crt_bmp != NULL) {
        (void)SelectObject(g_crt_dc, g_crt_old);
        (void)DeleteObject(g_crt_bmp);
        g_crt_bmp = NULL;
    }
    if (g_crt_dc == NULL && (g_crt_dc = CreateCompatibleDC(dc)) == NULL) return 0;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    g_crt_bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (g_crt_bmp == NULL) return 0;
    g_crt_old = (HBITMAP)SelectObject(g_crt_dc, g_crt_bmp);
    g_crt_bits = (uint32_t *)bits;
    g_crt_w = w;
    g_crt_h = h;
    g_crt_lines = 0;
    return 1;
}

static void paint_picture(HDC dc, RECT d);

static void paint(HDC dc)
{
    RECT cr, d;
    (void)GetClientRect(g_hwnd, &cr);
    dest_rect(cr.right, cr.bottom, &d);
    paint_picture(dc, d);
    if (g_note[0] != 0 && host_time_us() < g_note_until) {
        RECT t;
        HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HGDIOBJ old = SelectObject(dc, font);
        SIZE sz;
        int n = (int)wcslen(g_note);
        (void)GetTextExtentPoint32W(dc, g_note, n, &sz);
        t.left = d.left + 8; t.top = d.top + 8; t.right = t.left + sz.cx + 12; t.bottom = t.top + sz.cy + 6;
        (void)SetBkColor(dc, RGB(0, 0, 0));
        (void)SetTextColor(dc, RGB(255, 255, 255));
        (void)ExtTextOutW(dc, t.left + 6, t.top + 3, ETO_OPAQUE, &t, g_note, (UINT)n, NULL);
        (void)SelectObject(dc, old);
    }
}

static void paint_picture(HDC dc, RECT d)
{
    RECT cr;
    BITMAPINFO bi;
    HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RECT bar;
    (void)GetClientRect(g_hwnd, &cr);
    bar = cr; bar.bottom = d.top; (void)FillRect(dc, &bar, black);
    bar = cr; bar.top = d.bottom; (void)FillRect(dc, &bar, black);
    bar = cr; bar.top = d.top; bar.bottom = d.bottom; bar.right = d.left; (void)FillRect(dc, &bar, black);
    bar = cr; bar.top = d.top; bar.bottom = d.bottom; bar.left = d.right; (void)FillRect(dc, &bar, black);
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = g_fw;
    bi.bmiHeader.biHeight = -g_fh;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    if (g_opt_crt && d.bottom - d.top <= (LONG)sizeof g_crt_lvl && d.right - d.left <= 8192 && crt_surface(dc, d.right - d.left, d.bottom - d.top)) {
        int w = d.right - d.left, h = d.bottom - d.top, y, lines = 2 * g_fh, ph = crt_period_rows(h);
        if (g_crt_lines != lines || g_crt_ph != ph) {
            crt_levels(g_crt_lvl, h, lines, ph);
            g_crt_lines = lines;
            g_crt_ph = ph;
        }
        (void)GdiFlush();
        if (g_opt_smooth) {                            /* GDI's smoothing, then every pixel through the filter */
            (void)SetStretchBltMode(g_crt_dc, HALFTONE);
            (void)SetBrushOrgEx(g_crt_dc, 0, 0, NULL);
            (void)StretchDIBits(g_crt_dc, 0, 0, w, h, 0, 0, g_fw, g_fh, g_frame, &bi, DIB_RGB_COLORS, SRCCOPY);
            (void)GdiFlush();
            for (y = 0; y < h; y++) crt_row(g_crt_bits + (size_t)y * (size_t)w, w, g_crt_lvl[y]);
        } else {
            /* sharp scaling: a source row is filtered once per weight (fw pixels) and widened by nearest
             * neighbour; a row equal to the one above (same source row and weight) is a copy */
            static uint32_t tmp[640];
            static int xmap[8192];
            int x, prev_sy = -1, prev_l = -1;
            for (x = 0; x < w; x++) xmap[x] = (int)(((int64_t)x * g_fw) / w);
            for (y = 0; y < h; y++) {
                int sy = (int)(((int64_t)y * g_fh) / h), l = g_crt_lvl[y];
                uint32_t *row = g_crt_bits + (size_t)y * (size_t)w;
                if (sy == prev_sy && l == prev_l) {
                    memcpy(row, row - w, (size_t)w * 4u);
                    continue;
                }
                memcpy(tmp, g_frame + (size_t)sy * (size_t)g_fw, (size_t)g_fw * 4u);
                crt_row(tmp, g_fw, l);
                for (x = 0; x < w; x++) row[x] = tmp[xmap[x]];
                prev_sy = sy;
                prev_l = l;
            }
        }
        (void)BitBlt(dc, d.left, d.top, w, h, g_crt_dc, 0, 0, SRCCOPY);
        return;
    }
    (void)SetStretchBltMode(dc, g_opt_smooth ? HALFTONE : COLORONCOLOR);
    if (g_opt_smooth) (void)SetBrushOrgEx(dc, 0, 0, NULL);
    (void)StretchDIBits(dc, d.left, d.top, d.right - d.left, d.bottom - d.top, 0, 0, g_fw, g_fh, g_frame, &bi,
                        DIB_RGB_COLORS, SRCCOPY);
}

void host_present(const HostFrame *f)
{
    HDC dc;
    g_fw = f->width;
    g_fh = f->height;
    g_fa_w = f->aspect_w > 0 ? f->aspect_w : 4;
    g_fa_h = f->aspect_h > 0 ? f->aspect_h : 3;
    memcpy(g_frame, f->pixels, (size_t)f->width * (size_t)f->height * 4u);
    if (g_hwnd == NULL || IsIconic(g_hwnd)) return;
    dc = GetDC(g_hwnd);
    paint(dc);
    (void)ReleaseDC(g_hwnd, dc);
}

/* ---- time ---- */
uint64_t host_time_us(void)
{
    LARGE_INTEGER c;
    (void)QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * 1000000.0 / (double)g_qpf.QuadPart);
}
void host_sleep_us(uint64_t us)
{
    if (us > 200000u) us = 200000u;
    if (g_timer != NULL) {
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)(us * 10u);
        if (SetWaitableTimerEx(g_timer, &due, 0, NULL, NULL, NULL, 0)) {
            (void)WaitForSingleObject(g_timer, INFINITE);
            return;
        }
    }
    Sleep((DWORD)(us / 1000u));
}

void host_audio(const int16_t *s, int n)
{
    audio_push(s, n);
}

int host_quit_requested(void)
{
    return g_quit;
}

void hal_fatal(const char *msg)
{
    wchar_t w[600];
    (void)MultiByteToWideChar(CP_UTF8, 0, msg, -1, w, 600);
    MessageBoxW(g_hwnd, w, APP_NAMEW L" - internal error", MB_OK | MB_ICONERROR);
    ExitProcess(3);
}

/* ---- full screen ---- */
static void set_full(int on)
{
    DWORD st = (DWORD)GetWindowLongW(g_hwnd, GWL_STYLE);
    if (on) {
        MONITORINFO mi = {sizeof mi};
        if (!g_opt_full || (st & WS_OVERLAPPEDWINDOW)) (void)GetWindowPlacement(g_hwnd, &g_wp);
        (void)GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
        (void)SetWindowLongW(g_hwnd, GWL_STYLE, (LONG)(st & ~(DWORD)WS_OVERLAPPEDWINDOW) | WS_POPUP);
        (void)SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                           mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else if (!(st & WS_OVERLAPPEDWINDOW)) {
        (void)SetWindowLongW(g_hwnd, GWL_STYLE, (LONG)((st & ~(DWORD)WS_POPUP) | WS_OVERLAPPEDWINDOW));
        (void)SetWindowPlacement(g_hwnd, &g_wp);
        (void)SetWindowPos(g_hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    g_opt_full = on;
    save_options();
}

/* ---- the system menu ---- */
#define SC_P2_FULL 0x1010u
#define SC_P2_ASPECT 0x1020u
#define SC_P2_INTEGER 0x1030u
#define SC_P2_SMOOTH 0x1040u
#define SC_P2_MUTE 0x1050u
#define SC_P2_LOCATE 0x1060u
#define SC_P2_RESTART 0x1070u
#define SC_P2_ABOUT 0x1080u
#define SC_P2_PAUSE 0x1090u
#define SC_P2_CRT 0x10A0u
#define SC_P2_SAVE 0x10B0u
#define SC_P2_LOAD 0x10C0u
#define SC_P2_KEYS 0x10D0u
#define SC_P2_SHOT 0x10E0u
#define SC_P2_WIDE 0x1100u      /* + 0x10 * (0 off, 1 16:10, 2 16:9, 3 21:9) */
#define SC_P2_SIZE 0x1200u      /* + 0x10 * (0..4): 2x .. 6x */
#define SC_P2_VOL 0x1300u       /* + 0x10 * (0..3): 100, 75, 50, 25 % */
#define SC_P2_SLOT 0x1400u      /* + 0x10 * (0..3) */

static const int g_wide_modes[4] = {WIDE_OFF, WIDE_16_10, WIDE_16_9, WIDE_21_9};
static const int g_volumes[4] = {100, 75, 50, 25};

static void update_menu(void)
{
    HMENU m = GetSystemMenu(g_hwnd, FALSE);
    if (m == NULL) return;
    (void)CheckMenuItem(m, SC_P2_FULL, MF_BYCOMMAND | (g_opt_full ? MF_CHECKED : MF_UNCHECKED));
    (void)CheckMenuItem(m, SC_P2_ASPECT, MF_BYCOMMAND | (g_opt_aspect ? MF_CHECKED : MF_UNCHECKED));
    (void)CheckMenuItem(m, SC_P2_INTEGER, MF_BYCOMMAND | (g_opt_integer ? MF_CHECKED : MF_UNCHECKED));
    (void)CheckMenuItem(m, SC_P2_SMOOTH, MF_BYCOMMAND | (g_opt_smooth ? MF_CHECKED : MF_UNCHECKED));
    (void)CheckMenuItem(m, SC_P2_CRT, MF_BYCOMMAND | (g_opt_crt ? MF_CHECKED : MF_UNCHECKED));
    (void)CheckMenuItem(m, SC_P2_MUTE, MF_BYCOMMAND | (g_opt_mute ? MF_CHECKED : MF_UNCHECKED));
    (void)CheckMenuItem(m, SC_P2_PAUSE, MF_BYCOMMAND | (g_opt_pause_inactive ? MF_CHECKED : MF_UNCHECKED));
    {
        int i, uw, uh;
        RECT wa;
        UINT dpi = GetDpiForWindow(g_hwnd);
        MONITORINFO mi;
        mi.cbSize = sizeof mi;
        (void)GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        wa = mi.rcWork;
        canvas_units(&uw, &uh);
        for (i = 0; i < 4; i++) {
            (void)CheckMenuItem(m, SC_P2_WIDE + 0x10u * (UINT)i, MF_BYCOMMAND | ((wide_on() ? g_opt_wide : WIDE_OFF) == g_wide_modes[i] ? MF_CHECKED : MF_UNCHECKED));
            if (i > 0) (void)EnableMenuItem(m, SC_P2_WIDE + 0x10u * (UINT)i, MF_BYCOMMAND | (qol_wide_supported() ? MF_ENABLED : MF_GRAYED));
            (void)CheckMenuItem(m, SC_P2_VOL + 0x10u * (UINT)i, MF_BYCOMMAND | (g_opt_volume == g_volumes[i] ? MF_CHECKED : MF_UNCHECKED));
            (void)CheckMenuItem(m, SC_P2_SLOT + 0x10u * (UINT)i, MF_BYCOMMAND | (g_opt_slot == i + 1 ? MF_CHECKED : MF_UNCHECKED));
        }
        for (i = 0; i < 5; i++) {
            RECT r = {0, 0, (i + 2) * uw, (i + 2) * uh};
            (void)AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
            (void)EnableMenuItem(m, SC_P2_SIZE + 0x10u * (UINT)i, MF_BYCOMMAND |
                                 ((!g_opt_full && r.right - r.left <= wa.right - wa.left && r.bottom - r.top <= wa.bottom - wa.top) ? MF_ENABLED : MF_GRAYED));
        }
    }
}

static void add_menu(void)
{
    HMENU m, disp, sub;
    int n, i;
    wchar_t t[32];
    (void)GetSystemMenu(g_hwnd, TRUE);
    m = GetSystemMenu(g_hwnd, FALSE);
    if (m == NULL) return;
    (void)DeleteMenu(m, SC_CLOSE, MF_BYCOMMAND);
    n = GetMenuItemCount(m);
    if (n > 0 && (GetMenuState(m, (UINT)(n - 1), MF_BYPOSITION) & MF_SEPARATOR) != 0u)
        (void)DeleteMenu(m, (UINT)(n - 1), MF_BYPOSITION);
    (void)AppendMenuW(m, MF_STRING, SC_P2_FULL, L"&Full screen\tAlt+Enter");
    disp = CreatePopupMenu();
    (void)AppendMenuW(disp, MF_STRING, SC_P2_ASPECT, L"&4:3 aspect ratio");
    (void)AppendMenuW(disp, MF_STRING, SC_P2_INTEGER, L"&Integer scaling");
    (void)AppendMenuW(disp, MF_STRING, SC_P2_SMOOTH, L"&Smooth scaling");
    (void)AppendMenuW(disp, MF_SEPARATOR, 0u, NULL);
    (void)AppendMenuW(disp, MF_STRING, SC_P2_CRT, L"&CRT scanlines");
    (void)AppendMenuW(disp, MF_SEPARATOR, 0u, NULL);
    sub = CreatePopupMenu();
    (void)AppendMenuW(sub, MF_STRING, SC_P2_WIDE, L"&Off (original 4:3)");
    (void)AppendMenuW(sub, MF_STRING, SC_P2_WIDE + 0x10u, L"16:1&0");
    (void)AppendMenuW(sub, MF_STRING, SC_P2_WIDE + 0x20u, L"16:&9");
    (void)AppendMenuW(sub, MF_STRING, SC_P2_WIDE + 0x30u, L"&21:9");
    (void)AppendMenuW(disp, MF_POPUP, (UINT_PTR)sub, L"&Widescreen");
    sub = CreatePopupMenu();
    for (i = 0; i < 5; i++) {
        (void)swprintf(t, 32, L"&%d\u00D7", i + 2);
        (void)AppendMenuW(sub, MF_STRING, SC_P2_SIZE + 0x10u * (UINT)i, t);
    }
    (void)AppendMenuW(disp, MF_POPUP, (UINT_PTR)sub, L"Window si&ze");
    (void)AppendMenuW(m, MF_POPUP, (UINT_PTR)disp, L"&Display");
    (void)AppendMenuW(m, MF_STRING, SC_P2_MUTE, L"&Mute sound");
    sub = CreatePopupMenu();
    for (i = 0; i < 4; i++) {
        (void)swprintf(t, 32, L"%d %%", g_volumes[i]);
        (void)AppendMenuW(sub, MF_STRING, SC_P2_VOL + 0x10u * (UINT)i, t);
    }
    (void)AppendMenuW(m, MF_POPUP, (UINT_PTR)sub, L"&Volume");
    (void)AppendMenuW(m, MF_STRING, SC_P2_PAUSE, L"&Pause when inactive");
    (void)AppendMenuW(m, MF_STRING, SC_P2_KEYS, L"&Keys\u2026");
    (void)AppendMenuW(m, MF_SEPARATOR, 0u, NULL);
    (void)AppendMenuW(m, MF_STRING, SC_P2_SAVE, L"&Save state\tF5");
    (void)AppendMenuW(m, MF_STRING, SC_P2_LOAD, L"L&oad state\tF7");
    sub = CreatePopupMenu();
    for (i = 0; i < 4; i++) {
        (void)swprintf(t, 32, L"Slot &%d", i + 1);
        (void)AppendMenuW(sub, MF_STRING, SC_P2_SLOT + 0x10u * (UINT)i, t);
    }
    (void)AppendMenuW(m, MF_POPUP, (UINT_PTR)sub, L"S&tate slot");
    (void)AppendMenuW(m, MF_STRING, SC_P2_SHOT, L"Scree&nshot\tF12");
    (void)AppendMenuW(m, MF_SEPARATOR, 0u, NULL);
    (void)AppendMenuW(m, MF_STRING, SC_P2_RESTART, L"&Restart game");
    if (!hal_have_pack()) (void)AppendMenuW(m, MF_STRING, SC_P2_LOCATE, L"&Locate game files…");
    (void)AppendMenuW(m, MF_SEPARATOR, 0u, NULL);
    (void)AppendMenuW(m, MF_STRING, SC_P2_ABOUT, L"&About…");
    (void)AppendMenuW(m, MF_SEPARATOR, 0u, NULL);
    (void)AppendMenuW(m, MF_STRING, SC_CLOSE, L"&Close\tAlt+F4");
    (void)SetMenuDefaultItem(m, SC_CLOSE, FALSE);
    update_menu();
}

/* ---- keyboard ----
 * Key bindings: every action of the game has the key the original program reads (a set-1 scancode,
 * +256 when extended) and the key of this keyboard that stands for it (the same by default).  A key
 * bound to an action sends that action's original key; the original key of an action that was moved
 * elsewhere sends nothing; every other key goes to the game as it is. */
typedef struct Action {
    const wchar_t *name, *ini;
    int game;
} Action;
static const Action g_actions[] = {
    {L"Left", L"Left", 0x14B}, {L"Right", L"Right", 0x14D}, {L"Up / jump", L"Up", 0x148},
    {L"Down / crouch", L"Down", 0x150}, {L"Strike", L"Strike", 0x39}, {L"Scroll the view", L"Scroll", 0x4C},
    {L"Pause (the game's)", L"Pause", 0x19}, {L"Restart at the green light", L"Restart", 0x3B},
    {L"Back to the menu", L"Menu", 0x3C}, {L"Sound on/off", L"Sound", 0x3D},
};
#define N_ACTIONS (sizeof g_actions / sizeof g_actions[0])
static int g_bind[N_ACTIONS];
static uint32_t g_pad_prev;         /* the gamepad's buttons at the last poll (pad_poll) */

static void load_binds(void)
{
    size_t a;
    for (a = 0; a < N_ACTIONS; a++) {
        int k = (int)GetPrivateProfileIntW(L"Keys", g_actions[a].ini, g_actions[a].game, g_ini);
        g_bind[a] = (k > 0 && k < 512 && (k & 0x80) == 0) ? k : g_actions[a].game;
    }
}
static void save_binds(void)
{
    size_t a;
    for (a = 0; a < N_ACTIONS; a++) {
        wchar_t b[16];
        (void)swprintf(b, 16, L"%d", g_bind[a]);
        (void)WritePrivateProfileStringW(L"Keys", g_actions[a].ini, b, g_ini);
    }
}
/* the game's key for a key of the keyboard, or -1 when it sends nothing */
static int map_key(int k)
{
    size_t a;
    for (a = 0; a < N_ACTIONS; a++)
        if (g_bind[a] == k) return g_actions[a].game;
    for (a = 0; a < N_ACTIONS; a++)
        if (g_actions[a].game == k) return -1;
    return k;
}

static void release_keys(void)
{
    int i;
    for (i = 0; i < 512; i++) {
        if (g_keys_down[i]) {
            g_keys_down[i] = 0;
            if (g_running) hal_key((uint8_t)(i & 0x7F), 0, i >= 256);
        }
    }
}

static void key_event(WPARAM vk, LPARAM l, int down)
{
    int sc = (int)((l >> 16) & 0x7F);
    int ext = (l & (1 << 24)) != 0;
    int k;
    if (sc == 0) sc = (int)MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
    k = map_key(sc | (ext ? 256 : 0));
    if (k < 0) return;
    g_keys_down[k] = (uint8_t)down;
    if (g_running) hal_key((uint8_t)(k & 0x7F), down, k >= 256);
}

/* ---- pause, fast forward, messages ---- */
static void update_title(void)
{
    (void)SetWindowTextW(g_hwnd, (g_user_pause || (g_inactive && g_opt_pause_inactive)) ? APP_NAMEW L" (paused)" : APP_NAMEW);
}
static void apply_mute(void)
{
    audio_set_muted(g_opt_mute || g_turbo || g_user_pause || (g_inactive && g_opt_pause_inactive));
}
static void set_turbo(int on)
{
    if (g_turbo == on) return;
    g_turbo = on;
    hal_set_turbo(on);
    apply_mute();
}
static void note(const wchar_t *text)
{
    (void)wcsncpy(g_note, text, 95);
    g_note[95] = 0;
    g_note_until = host_time_us() + 1500000u;
    (void)InvalidateRect(g_hwnd, NULL, FALSE);
}

/* ---- save states: %APPDATA%\PreH2\stateN.p2s ---- */
static void state_path(wchar_t *out, int slot)
{
    wchar_t *sl;
    (void)wcscpy(out, g_ini);
    sl = wcsrchr(out, L'\\');
    (void)swprintf(sl != NULL ? sl + 1 : out, 32, L"state%d.p2s", slot);
}

static void save_state(void)
{
    uint8_t *buf = NULL;
    size_t len = 0;
    wchar_t path[MAX_PATH + 32], msg[96];
    HANDLE f;
    DWORD wr = 0;
    int ok = 0;
    if (!hal_state_save(&buf, &len)) { note(L"The state could not be saved"); return; }
    state_path(path, g_opt_slot);
    f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        ok = WriteFile(f, buf, (DWORD)len, &wr, NULL) && wr == (DWORD)len;
        (void)CloseHandle(f);
        if (!ok) (void)DeleteFileW(path);
    }
    free(buf);
    (void)swprintf(msg, 96, ok ? L"State saved (slot %d)" : L"The state could not be saved (slot %d)", g_opt_slot);
    note(msg);
}

static void load_state(void)
{
    wchar_t path[MAX_PATH + 32], msg[96];
    HANDLE f;
    LARGE_INTEGER size;
    uint8_t *buf = NULL;
    DWORD rd = 0;
    int ok = 0, i;
    state_path(path, g_opt_slot);
    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (64 << 20) && (buf = (uint8_t *)malloc((size_t)size.QuadPart)) != NULL &&
            ReadFile(f, buf, (DWORD)size.QuadPart, &rd, NULL) && rd == (DWORD)size.QuadPart)
            ok = hal_state_load(buf, (size_t)rd);
        (void)CloseHandle(f);
        free(buf);
        if (ok) {                           /* the keys held now are pressed again in the loaded machine */
            for (i = 0; i < 512; i++)
                if (g_keys_down[i]) hal_key((uint8_t)(i & 0x7F), 1, i >= 256);
            g_pad_prev = 0;
        }
        (void)swprintf(msg, 96, ok ? L"State loaded (slot %d)" : L"The state in slot %d is not usable", g_opt_slot);
    } else {
        (void)swprintf(msg, 96, L"No state in slot %d", g_opt_slot);
    }
    note(msg);
}

/* ---- screenshot: Pictures\Prehistorik 2\Prehistorik2-<date>-<time>.png, in the picture's shape on
 * a 4:3 screen (5x6 screen pixels for each pixel of a 200-line mode), encoded by GDI+ ---- */
static void screenshot(void)
{
    typedef struct GpStartup { UINT32 version; void *callback; BOOL no_thread, no_codecs; } GpStartup;
    typedef int(WINAPI * StartFn)(ULONG_PTR *, const GpStartup *, void *);
    typedef int(WINAPI * CreateFn)(INT, INT, INT, INT, BYTE *, void **);
    typedef int(WINAPI * SaveFn)(void *, const WCHAR *, const CLSID *, const void *);
    typedef int(WINAPI * DisposeFn)(void *);
    static const CLSID png = {0x557CF406, 0x1A04, 0x11D3, {0x9A, 0x73, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E}};
    static HMODULE gp;
    static ULONG_PTR token;
    static CreateFn create;
    static SaveFn save;
    static DisposeFn dispose;
    wchar_t dir[MAX_PATH], path[MAX_PATH + 80];
    SYSTEMTIME st;
    int kx = g_fh == 200 ? 5 : 2, ky = g_fh == 200 ? 6 : 2, w = g_fw * kx, h = g_fh * ky, x, y, ok = 0;
    uint32_t *px;
    void *bmp = NULL;
    if (gp == NULL) {
        GpStartup in = {1, NULL, FALSE, FALSE};
        StartFn start;
        gp = LoadLibraryExW(L"gdiplus.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        start = gp != NULL ? (StartFn)(void *)GetProcAddress(gp, "GdiplusStartup") : NULL;
        if (start != NULL && start(&token, &in, NULL) == 0) {
            create = (CreateFn)(void *)GetProcAddress(gp, "GdipCreateBitmapFromScan0");
            save = (SaveFn)(void *)GetProcAddress(gp, "GdipSaveImageToFile");
            dispose = (DisposeFn)(void *)GetProcAddress(gp, "GdipDisposeImage");
        }
    }
    if (create == NULL || save == NULL || dispose == NULL || SHGetFolderPathW(NULL, CSIDL_MYPICTURES, NULL, 0, dir) != S_OK ||
        (px = (uint32_t *)malloc((size_t)w * (size_t)h * 4u)) == NULL) {
        note(L"The screenshot could not be saved");
        return;
    }
    for (y = 0; y < h; y++) {
        const uint32_t *src = g_frame + (size_t)(y / ky) * (size_t)g_fw;
        uint32_t *row = px + (size_t)y * (size_t)w;
        for (x = 0; x < w; x++) row[x] = src[x / kx];
    }
    (void)wcscat(dir, L"\\Prehistorik 2");
    (void)CreateDirectoryW(dir, NULL);
    GetLocalTime(&st);
    (void)swprintf(path, MAX_PATH + 80, L"%ls\\Prehistorik2-%04d%02d%02d-%02d%02d%02d-%03d.png", dir, st.wYear, st.wMonth, st.wDay,
                   st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    if (create(w, h, w * 4, 0x00022009 /* 32 bits, RGB */, (BYTE *)px, &bmp) == 0 && bmp != NULL) {
        ok = save(bmp, path, &png, NULL) == 0;
        (void)dispose(bmp);
    }
    free(px);
    note(ok ? L"Screenshot saved in Pictures\\Prehistorik 2" : L"The screenshot could not be saved");
}

/* ---- window size and place ---- */
static void set_client_size(int cw, int ch)
{
    RECT r = {0, 0, cw, ch}, wr;
    MONITORINFO mi;
    (void)AdjustWindowRectExForDpi(&r, (DWORD)GetWindowLongW(g_hwnd, GWL_STYLE), FALSE, 0, GetDpiForWindow(g_hwnd));
    (void)SetWindowPos(g_hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    mi.cbSize = sizeof mi;                 /* keep it on the screen */
    if (GetWindowRect(g_hwnd, &wr) && GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
        int dx = 0, dy = 0;
        if (wr.right > mi.rcWork.right) dx = mi.rcWork.right - wr.right;
        if (wr.left + dx < mi.rcWork.left) dx = mi.rcWork.left - wr.left;
        if (wr.bottom > mi.rcWork.bottom) dy = mi.rcWork.bottom - wr.bottom;
        if (wr.top + dy < mi.rcWork.top) dy = mi.rcWork.top - wr.top;
        if (dx != 0 || dy != 0) (void)SetWindowPos(g_hwnd, NULL, wr.left + dx, wr.top + dy, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

static void set_window_scale(int k)
{
    int uw, uh;
    if (g_opt_full) return;
    if (IsZoomed(g_hwnd)) (void)ShowWindow(g_hwnd, SW_RESTORE);
    canvas_units(&uw, &uh);
    set_client_size(k * uw, k * uh);
}

/* Display > Widescreen: the window keeps its height and takes the new shape */
static void set_wide(int width)
{
    int uw0, uw, uh;
    canvas_units(&uw0, &uh);
    g_opt_wide = width;
    save_options();
    qol_set_wide(wide_on() ? g_opt_wide : WIDE_OFF);
    canvas_units(&uw, &uh);
    if (uw != uw0 && !g_opt_full && !IsZoomed(g_hwnd) && !IsIconic(g_hwnd)) {
        RECT cr;
        (void)GetClientRect(g_hwnd, &cr);
        set_client_size(MulDiv(cr.bottom, uw, uh), cr.bottom);
    }
    (void)InvalidateRect(g_hwnd, NULL, FALSE);
    update_menu();
}

static void save_placement(void)
{
    RECT r;
    if (g_opt_full || IsZoomed(g_hwnd) || IsIconic(g_hwnd) || !GetWindowRect(g_hwnd, &r)) return;
    ini_set_int(L"WinX", r.left);
    ini_set_int(L"WinY", r.top);
    ini_set_int(L"WinW", r.right - r.left);
    ini_set_int(L"WinH", r.bottom - r.top);
}
static void restore_placement(void)
{
    RECT r;
    int w = ini_int(L"WinW", 0), h = ini_int(L"WinH", 0);
    if (w < 200 || h < 150) return;
    r.left = ini_int(L"WinX", 0);
    r.top = ini_int(L"WinY", 0);
    r.right = r.left + w;
    r.bottom = r.top + h;
    if (MonitorFromRect(&r, MONITOR_DEFAULTTONULL) == NULL) return;
    (void)SetWindowPos(g_hwnd, NULL, r.left, r.top, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

/* ---- the Keys box ---- */
#define IDC_KEYS_LIST 301
#define IDC_KEYS_CHANGE 302
#define IDC_KEYS_RESET 303
#define IDC_KEYS_HINT 304
static const wchar_t g_keys_hint[] = L"Choose an action, press Change (or double-click it), then press its new key.";

static void key_name(int k, wchar_t *out, int n)
{
    LONG l = (LONG)(((k & 0x7F) << 16) | ((k & 0x100) ? (1 << 24) : 0));
    if (GetKeyNameTextW(l, out, n) == 0) (void)swprintf(out, (size_t)n, L"key %02X%ls", k & 0x7F, (k & 0x100) ? L" (extended)" : L"");
}

static void keys_fill(HWND dlg)
{
    HWND lb = GetDlgItem(dlg, IDC_KEYS_LIST);
    int sel = (int)SendMessageW(lb, LB_GETCURSEL, 0u, 0);
    size_t a;
    (void)SendMessageW(lb, LB_RESETCONTENT, 0u, 0);
    for (a = 0; a < N_ACTIONS; a++) {
        wchar_t line[160], kn[64];
        key_name(g_bind[a], kn, 64);
        (void)swprintf(line, 160, L"%ls\t%ls", g_actions[a].name, kn);
        (void)SendMessageW(lb, LB_ADDSTRING, 0u, (LPARAM)line);
    }
    (void)SendMessageW(lb, LB_SETCURSEL, (WPARAM)(sel < 0 ? 0 : sel), 0);
}

static void keys_theme(HWND dlg)
{
    HWND lb = GetDlgItem(dlg, IDC_KEYS_LIST);
    g_about_dark = theme_wanted();
    theme_window(dlg, g_about_dark);
    if (lb != NULL) {
        if (g_ux_window != NULL) (void)g_ux_window(lb, g_about_dark ? TRUE : FALSE);
        (void)SetWindowTheme(lb, g_about_dark ? L"DarkMode_Explorer" : NULL, NULL);
        (void)SendMessageW(lb, WM_THEMECHANGED, 0u, 0);
    }
    (void)RedrawWindow(dlg, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}

/* a row of the list (owner-drawn, so that the chosen row reads well in both themes): the action
 * on the left, its key from the middle on */
static void keys_row(const DRAWITEMSTRUCT *di)
{
    wchar_t kn[64];
    RECT r = di->rcItem;
    int sel = (di->itemState & ODS_SELECTED) != 0;
    COLORREF bg = g_about_dark ? (sel ? RGB(0, 95, 184) : THEME_DARK_FIELD) : GetSysColor(sel ? COLOR_HIGHLIGHT : COLOR_WINDOW);
    COLORREF fg = g_about_dark ? THEME_DARK_TEXT : GetSysColor(sel ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT);
    HBRUSH b = CreateSolidBrush(bg);
    (void)FillRect(di->hDC, &r, b);
    (void)DeleteObject(b);
    if (di->itemID >= N_ACTIONS) return;
    (void)SetBkMode(di->hDC, TRANSPARENT);
    (void)SetTextColor(di->hDC, fg);
    r.left += 6;
    (void)DrawTextW(di->hDC, g_actions[di->itemID].name, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    r.left = di->rcItem.left + (di->rcItem.right - di->rcItem.left) * 55 / 100;
    key_name(g_bind[di->itemID], kn, 64);
    (void)DrawTextW(di->hDC, kn, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
}

/* waits for a key: its index (scancode, +256 extended), or -1 when cancelled or not allowed */
static int keys_capture(HWND dlg)
{
    MSG m;
    int k = -1;
    (void)SetDlgItemTextW(dlg, IDC_KEYS_HINT, L"Press the new key (Esc: keep the present one)…");
    (void)SetFocus(dlg);
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (m.message == WM_KEYDOWN || m.message == WM_SYSKEYDOWN) {
            int sc = (int)((m.lParam >> 16) & 0x7F), ext = (m.lParam & (1 << 24)) != 0;
            WPARAM vk = m.wParam;
            if (vk != VK_ESCAPE && vk != VK_F5 && vk != VK_F7 && vk != VK_F11 && vk != VK_F12 && vk != VK_TAB && vk != VK_PAUSE &&
                vk != VK_RETURN && vk != VK_LWIN && vk != VK_RWIN && sc != 0)
                k = sc | (ext ? 256 : 0);
            break;
        }
        if (m.message == WM_LBUTTONDOWN || m.message == WM_RBUTTONDOWN || m.message == WM_NCLBUTTONDOWN) {
            (void)PostMessageW(m.hwnd, m.message, m.wParam, m.lParam);   /* the click does what it would have done */
            break;
        }
        (void)DispatchMessageW(&m);
        if (!IsWindow(dlg)) return -1;
    }
    (void)SetDlgItemTextW(dlg, IDC_KEYS_HINT, g_keys_hint);
    (void)SetFocus(GetDlgItem(dlg, IDC_KEYS_LIST));
    return k;
}

static INT_PTR CALLBACK keys_proc(HWND dlg, UINT msg, WPARAM w, LPARAM l)
{
    switch (msg) {
    case WM_INITDIALOG: {
        RECT du = {0, 0, 4, 11};               /* a row: 11 dialog units high */
        (void)MapDialogRect(dlg, &du);
        (void)SendDlgItemMessageW(dlg, IDC_KEYS_LIST, LB_SETITEMHEIGHT, 0u, (LPARAM)du.bottom);
        g_about_bg = CreateSolidBrush(THEME_DARK_BG);
        g_about_field = CreateSolidBrush(THEME_DARK_FIELD);
        keys_fill(dlg);
        keys_theme(dlg);
        (void)SetFocus(GetDlgItem(dlg, IDC_KEYS_LIST));
        return FALSE;
    }
    case WM_DESTROY:
        if (g_about_bg != NULL) { (void)DeleteObject(g_about_bg); g_about_bg = NULL; }
        if (g_about_field != NULL) { (void)DeleteObject(g_about_field); g_about_field = NULL; }
        break;
    case WM_SETTINGCHANGE:
        if (theme_setting(l)) keys_theme(dlg);
        break;
    case WM_DRAWITEM:
        if (w == IDOK || w == IDC_KEYS_CHANGE || w == IDC_KEYS_RESET) { box_button((const DRAWITEMSTRUCT *)l, g_about_dark, g_about_bg); return TRUE; }
        if (w == IDC_KEYS_LIST) { keys_row((const DRAWITEMSTRUCT *)l); return TRUE; }
        break;
    case WM_CTLCOLORDLG:
        if (g_about_dark && g_about_bg != NULL) return (INT_PTR)g_about_bg;
        break;
    case WM_CTLCOLORSTATIC:
        if (g_about_dark && g_about_bg != NULL) {
            (void)SetTextColor((HDC)w, THEME_DARK_TEXT);
            (void)SetBkColor((HDC)w, THEME_DARK_BG);
            return (INT_PTR)g_about_bg;
        }
        break;
    case WM_CTLCOLORLISTBOX:
        if (g_about_dark && g_about_field != NULL) {
            (void)SetTextColor((HDC)w, THEME_DARK_TEXT);
            (void)SetBkColor((HDC)w, THEME_DARK_FIELD);
            return (INT_PTR)g_about_field;
        }
        break;
    case WM_COMMAND:
        if (LOWORD(w) == IDC_KEYS_CHANGE || (LOWORD(w) == IDC_KEYS_LIST && HIWORD(w) == LBN_DBLCLK)) {
            int a = (int)SendDlgItemMessageW(dlg, IDC_KEYS_LIST, LB_GETCURSEL, 0u, 0), k;
            size_t o;
            if (a < 0 || a >= (int)N_ACTIONS) return TRUE;
            k = keys_capture(dlg);
            if (k < 0 || !IsWindow(dlg)) return TRUE;
            for (o = 0; o < N_ACTIONS; o++)
                if (g_bind[o] == k) g_bind[o] = g_bind[a];        /* the key's former action takes this one's key */
            g_bind[a] = k;
            keys_fill(dlg);
            return TRUE;
        }
        if (LOWORD(w) == IDC_KEYS_RESET) {
            size_t a;
            for (a = 0; a < N_ACTIONS; a++) g_bind[a] = g_actions[a].game;
            keys_fill(dlg);
            return TRUE;
        }
        if (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL) { save_binds(); EndDialog(dlg, 1); return TRUE; }
        break;
    default:
        break;
    }
    return FALSE;
}

static void show_keys(void)
{
    Tmpl t;
    DWORD style = DS_MODALFRAME | DS_SETFONT | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    memset(&t, 0, sizeof t);
    t_word(&t, LOWORD(style)); t_word(&t, HIWORD(style));
    t_word(&t, 0); t_word(&t, 0);
    t_word(&t, 5);
    t_word(&t, 0); t_word(&t, 0); t_word(&t, 240); t_word(&t, 176);
    t_word(&t, 0); t_word(&t, 0);
    t_str(&t, APP_NAMEW L" - keys");
    t_word(&t, 9);
    t_str(&t, L"Segoe UI");
    t_item(&t, WS_CHILD | WS_VISIBLE | SS_LEFT, 10, 8, 220, 18, IDC_KEYS_HINT, 0x0082, g_keys_hint);
    t_item(&t, WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
           10, 30, 220, 116, IDC_KEYS_LIST, 0x0083, L"");
    t_item(&t, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 10, 154, 60, 15, IDC_KEYS_CHANGE, 0x0080, L"&Change…");
    t_item(&t, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 76, 154, 60, 15, IDC_KEYS_RESET, 0x0080, L"&Defaults");
    t_item(&t, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 180, 154, 50, 15, IDOK, 0x0080, L"OK");
    release_keys();
    g_menu_open++;
    (void)DialogBoxIndirectParamW(GetModuleHandleW(NULL), (LPCDLGTEMPLATEW)t.buf, g_hwnd, keys_proc, 0);
    g_menu_open--;
}

static int start_game(void)
{
    char err[512];
    release_keys();
    g_running = 0;
    if (!hal_init(err, sizeof err)) {
        wchar_t w[600];
        (void)MultiByteToWideChar(CP_UTF8, 0, err, -1, w, 600);
        MessageBoxW(g_hwnd, w, APP_NAMEW, MB_OK | MB_ICONERROR);
        return 0;
    }
    qol_set_wide(wide_on() ? g_opt_wide : WIDE_OFF);
    g_running = 1;
    return 1;
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        paint(dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        (void)InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)l;
        mm->ptMinTrackSize.x = 340;
        mm->ptMinTrackSize.y = 280;
        return 0;
    }
    case WM_DPICHANGED: {
        const RECT *r = (const RECT *)l;
        if (!g_opt_full)
            (void)SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_SETTINGCHANGE:
        if (theme_setting(l)) theme_apply(1);
        break;
    case WM_ACTIVATEAPP:
        if (!w) release_keys();
        g_inactive = !w;
        update_title();
        apply_mute();
        break;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT && g_opt_full) {
            (void)SetCursor(NULL);
            return TRUE;
        }
        break;
    case WM_KILLFOCUS:
        release_keys();
        set_turbo(0);
        break;
    case WM_ENTERMENULOOP: case WM_ENTERSIZEMOVE:
        g_menu_open++;
        release_keys();
        break;
    case WM_EXITMENULOOP: case WM_EXITSIZEMOVE:
        if (g_menu_open > 0) g_menu_open--;
        break;
    case WM_INITMENUPOPUP:
        update_menu();
        break;
    case WM_SYSCOMMAND:
        switch (w & 0xFFF0u) {
        case SC_P2_FULL: set_full(!g_opt_full); update_menu(); return 0;
        case SC_P2_ASPECT: g_opt_aspect = !g_opt_aspect; save_options(); update_menu(); (void)InvalidateRect(h, NULL, FALSE); return 0;
        case SC_P2_INTEGER: g_opt_integer = !g_opt_integer; save_options(); update_menu(); (void)InvalidateRect(h, NULL, FALSE); return 0;
        case SC_P2_SMOOTH: g_opt_smooth = !g_opt_smooth; save_options(); update_menu(); (void)InvalidateRect(h, NULL, FALSE); return 0;
        case SC_P2_CRT: g_opt_crt = !g_opt_crt; save_options(); update_menu(); (void)InvalidateRect(h, NULL, FALSE); return 0;
        case SC_P2_MUTE: g_opt_mute = !g_opt_mute; apply_mute(); save_options(); update_menu(); return 0;
        case SC_P2_SAVE: g_pending = 1; return 0;
        case SC_P2_LOAD: g_pending = 2; return 0;
        case SC_P2_SHOT: g_pending = 3; return 0;
        case SC_P2_KEYS: show_keys(); return 0;
        case SC_P2_ABOUT: show_about(); return 0;
        case SC_P2_PAUSE: g_opt_pause_inactive = !g_opt_pause_inactive; save_options(); update_menu(); return 0;
        case SC_P2_RESTART:
            if (MessageBoxW(h, L"Restart the game from the beginning?", APP_NAMEW, MB_OKCANCEL | MB_ICONQUESTION) == IDOK)
                (void)start_game();
            return 0;
        case SC_P2_LOCATE: {
            wchar_t dir[MAX_PATH * 2];
            g_menu_open++;
            if (pick_game_dir(h, dir)) {
                (void)WritePrivateProfileStringW(L"PreH2", L"GameDir", dir, g_ini);
                set_game_dir(dir);
                (void)start_game();
            }
            g_menu_open--;
            return 0;
        }
        case SC_KEYMENU:
            if (l == 0 || l == VK_RETURN) return 0;   /* Alt alone, F10: the game's */
            break;
        default: {
            UINT id = (UINT)(w & 0xFFF0u);
            if (id >= SC_P2_WIDE && id <= SC_P2_WIDE + 0x30u) { set_wide(g_wide_modes[(id - SC_P2_WIDE) >> 4]); return 0; }
            if (id >= SC_P2_SIZE && id <= SC_P2_SIZE + 0x40u) { set_window_scale((int)((id - SC_P2_SIZE) >> 4) + 2); return 0; }
            if (id >= SC_P2_VOL && id <= SC_P2_VOL + 0x30u) {
                g_opt_volume = g_volumes[(id - SC_P2_VOL) >> 4];
                audio_set_volume(g_opt_volume);
                save_options();
                return 0;
            }
            if (id >= SC_P2_SLOT && id <= SC_P2_SLOT + 0x30u) { g_opt_slot = (int)((id - SC_P2_SLOT) >> 4) + 1; save_options(); return 0; }
            break;
        }
        }
        break;
    case WM_SYSKEYDOWN:
        if (w == VK_RETURN && (l & (1 << 29))) {
            if (!(l & (1 << 30))) { set_full(!g_opt_full); update_menu(); }
            return 0;
        }
        if (w == VK_F4 && (l & (1 << 29))) break;
        if (w == VK_SPACE && (l & (1 << 29))) break;
        key_event(w, l, 1);
        return 0;
    case WM_SYSKEYUP:
        key_event(w, l, 0);
        return 0;
    case WM_KEYDOWN:
        if (w == VK_F11) {
            if (!(l & (1 << 30))) { set_full(!g_opt_full); update_menu(); }
            return 0;
        }
        if (w == VK_F5 || w == VK_F7 || w == VK_F12) {
            if (!(l & (1 << 30))) g_pending = w == VK_F5 ? 1 : w == VK_F7 ? 2 : 3;
            return 0;
        }
        if (w == VK_TAB) { set_turbo(1); return 0; }
        if (w == VK_PAUSE) {
            if (!(l & (1 << 30))) { g_user_pause = !g_user_pause; update_title(); apply_mute(); }
            return 0;
        }
        key_event(w, l, 1);
        return 0;
    case WM_KEYUP:
        if (w == VK_F11 || w == VK_F5 || w == VK_F7 || w == VK_F12 || w == VK_PAUSE) return 0;
        if (w == VK_TAB) { set_turbo(0); return 0; }
        key_event(w, l, 0);
        return 0;
    case WM_CLOSE:
        save_placement();
        g_quit = 1;
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(h, msg, w, l);
}

/* ---- gamepad (XInput, loaded at run time): pressed as the keys of the original controls ----
 * D-pad / left stick: the arrows; A: up (jump); X and B: Space (strike); Y: keypad 5 (look ahead);
 * Start: Enter; Back: Esc; LB: F1 (restart at the green light); RB: P (pause). */
typedef struct XPad {
    DWORD packet;
    WORD buttons;
    BYTE lt, rt;
    SHORT lx, ly, rx, ry;
} XPad;
typedef DWORD(WINAPI *XGetStateFn)(DWORD, XPad *);
static XGetStateFn g_xget;
static int g_xtried;
static uint32_t g_pad_prev;

static const struct { uint32_t bit; uint8_t sc; int ext; } g_padmap[] = {
    {1u << 0, 0x48, 1}, {1u << 1, 0x50, 1}, {1u << 2, 0x4B, 1}, {1u << 3, 0x4D, 1},
    {1u << 4, 0x1C, 0}, {1u << 5, 0x01, 0}, {1u << 6, 0x3B, 0}, {1u << 7, 0x19, 0},
    {1u << 8, 0x48, 1}, {1u << 9, 0x39, 0}, {1u << 10, 0x4C, 0},
};

static void pad_poll(void)
{
    XPad p;
    uint32_t now = 0, ch;
    size_t k;
    if (!g_xtried) {
        HMODULE x = LoadLibraryExW(L"xinput1_4.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (x == NULL) x = LoadLibraryExW(L"xinput9_1_0.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        g_xget = x != NULL ? (XGetStateFn)(void *)GetProcAddress(x, "XInputGetState") : NULL;
        g_xtried = 1;
    }
    if (g_xget == NULL || !g_running || GetForegroundWindow() != g_hwnd) {
        now = 0;
    } else if (g_xget(0, &p) == ERROR_SUCCESS) {
        WORD b = p.buttons;
        if ((b & 0x0001u) || p.ly > 12000) now |= 1u << 0;            /* up */
        if ((b & 0x0002u) || p.ly < -12000) now |= 1u << 1;           /* down */
        if ((b & 0x0004u) || p.lx < -12000) now |= 1u << 2;           /* left */
        if ((b & 0x0008u) || p.lx > 12000) now |= 1u << 3;            /* right */
        if (b & 0x0010u) now |= 1u << 4;                                /* Start */
        if (b & 0x0020u) now |= 1u << 5;                                /* Back */
        if (b & 0x0100u) now |= 1u << 6;                                /* LB */
        if (b & 0x0200u) now |= 1u << 7;                                /* RB */
        if (b & 0x1000u) now |= 1u << 8;                                /* A */
        if (b & (0x2000u | 0x4000u)) now |= 1u << 9;                   /* B, X */
        if (b & 0x8000u) now |= 1u << 10;                               /* Y */
    }
    ch = now ^ g_pad_prev;
    if (ch == 0) return;
    {   /* the up arrow has two sources (d-pad and A): one key event for their union */
        const uint32_t up = (1u << 0) | (1u << 8);
        int was = (g_pad_prev & up) != 0, is = (now & up) != 0;
        if (was != is && g_running) hal_key(0x48, is, 1);
        ch &= ~up;
    }
    for (k = 0; k < sizeof g_padmap / sizeof g_padmap[0]; k++) {
        if (!(ch & g_padmap[k].bit)) continue;
        if (g_running) hal_key(g_padmap[k].sc, (now & g_padmap[k].bit) != 0, g_padmap[k].ext);
    }
    g_pad_prev = now;
}

void host_pump(void)
{
    MSG m;
    pad_poll();
    while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
        if (m.message == WM_QUIT) { g_quit = 1; break; }
        (void)TranslateMessage(&m);
        (void)DispatchMessageW(&m);
    }
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    WNDCLASSEXW wc;
    RECT r;
    DWORD style = WS_OVERLAPPEDWINDOW;
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    UINT dpi;
    (void)prev;
    (void)cmd;
    (void)CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    (void)QueryPerformanceFrequency(&g_qpf);
    (void)timeBeginPeriod(1);
    g_timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    if (g_timer == NULL) g_timer = CreateWaitableTimerW(NULL, TRUE, NULL);
    ini_path();
    g_opt_aspect = ini_int(L"Aspect", 1);
    g_opt_integer = ini_int(L"Integer", 0);
    g_opt_smooth = ini_int(L"Smooth", 0);
    g_opt_crt = ini_int(L"Crt", 0);
    g_opt_mute = ini_int(L"Mute", 0);
    g_opt_pause_inactive = ini_int(L"PauseInactive", 1);
    g_opt_wide = ini_int(L"Wide", WIDE_OFF);
    if (g_opt_wide != WIDE_16_10 && g_opt_wide != WIDE_16_9 && g_opt_wide != WIDE_21_9) g_opt_wide = WIDE_OFF;
    g_opt_volume = ini_int(L"Volume", 100);
    g_opt_slot = ini_int(L"Slot", 1);
    if (g_opt_slot < 1 || g_opt_slot > 4) g_opt_slot = 1;
    load_binds();
    if (!load_embedded_pack() && !find_game_dir(argc > 1 ? argv[1] : NULL)) return 1;
    theme_apply(0);
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = L"PreH2Window";
    (void)RegisterClassExW(&wc);
    dpi = GetDpiForSystem();
    r.left = 0; r.top = 0;
    r.right = MulDiv(g_opt_wide > WIDE_OFF ? 3 * g_opt_wide : 960, (int)dpi, 96);
    r.bottom = MulDiv(720, (int)dpi, 96);
    {
        RECT wa;
        (void)SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        while (r.bottom > (wa.bottom - wa.top) * 9 / 10 && r.bottom > 400) { r.right = r.right * 3 / 4; r.bottom = r.bottom * 3 / 4; }
    }
    (void)AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, APP_NAMEW, style, CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
    if (g_hwnd == NULL) return 1;
    theme_apply(0);
    add_menu();
    restore_placement();
    (void)ShowWindow(g_hwnd, show);
    (void)UpdateWindow(g_hwnd);
    if (ini_int(L"FullScreen", 0)) { set_full(1); update_menu(); }
    (void)audio_open();
    audio_set_muted(g_opt_mute);
    audio_set_volume(g_opt_volume);
    if (!start_game()) return 1;
    while (!g_quit) {
        if (g_note[0] != 0 && host_time_us() >= g_note_until) {
            g_note[0] = 0;
            (void)InvalidateRect(g_hwnd, NULL, FALSE);
        }
        if (g_pending && g_running && !g_menu_open) {
            int what = g_pending;
            g_pending = 0;
            if (what == 1) save_state();
            else if (what == 2) load_state();
            else screenshot();
        }
        if (!g_running || g_menu_open || IsIconic(g_hwnd) || g_user_pause || (g_inactive && g_opt_pause_inactive)) {
            host_pump();
            host_sleep_us(15000);
            continue;
        }
        if (!hal_frame()) {
            if (hal_exited() && !g_quit) {
                if (g_hal_exit_msg[0] != 0 && strstr(g_hal_exit_msg, "FATAL") != NULL) {
                    wchar_t wm[300];
                    (void)MultiByteToWideChar(CP_ACP, 0, g_hal_exit_msg, -1, wm, 300);
                    MessageBoxW(g_hwnd, wm, APP_NAMEW, MB_OK | MB_ICONWARNING);
                }
                g_quit = 1;
            }
        }
    }
    audio_close();
    DestroyWindow(g_hwnd);
    LocalFree(argv);
    return 0;
}
