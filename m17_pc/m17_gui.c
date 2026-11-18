/* m17_pc: a window for the M17 station of m17_engine.c.
 * Left: reflector (hosts\M17Hosts.txt, filtered as you type), host and port, module, callsign, destination,
 * microphone, connect, the PTT (F2 toggles it like the button), meters. Right: the call being heard, last heard
 * and the log. Settings are kept in m17_pc.ini next to the program.
 * Mode "無線機": instead of a reflector, an FM radio on the sound card (4FSK made here, radio_win.c); its devices and
 * PTT are set in a small dialog and kept in the [radio] section of m17_pc.ini.
 * Mode "中継": the radio and the reflector at the same time; calls heard on the radio go to the reflector, and (only
 * if ticked in the dialog) calls from the reflector are transmitted by the radio. */
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "license_check.h"
#include "m17_engine.h"
#include "m17_parrot.h"
#include "mic_win.h"
#include "radio_win.h"

enum {
    ID_FILTER = 100, ID_REFL, ID_HOST, ID_PORT, ID_MODULE, ID_CALL, ID_DEST, ID_MIC, ID_SAVE, ID_CONNECT, ID_PTT,
    ID_STATUS, ID_RXINFO, ID_MICBAR, ID_RXBAR, ID_HEARD, ID_LOG, ID_PARROT, ID_ADD, ID_DEL, ID_MODE, ID_RADIOSET, ID_TIMER = 1, WM_LOGLINE = WM_APP + 1
};

static HWND s_wnd, s_ctl[64];
static HFONT s_font, s_big;
static int s_dpi = 96;
static wchar_t s_ini[MAX_PATH], s_dir[MAX_PATH];
static bool s_connected, s_warned;
static int s_heard_shown = -1;
static char s_heard_top[40];

static int S(int v)
{
    return MulDiv(v, s_dpi, 96);
}

static HWND ctl(int id)
{
    return s_ctl[id - 100];
}

static HWND add(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExW(!wcscmp(cls, L"EDIT") || !wcscmp(cls, WC_LISTVIEWW) ? WS_EX_CLIENTEDGE : 0, cls, text,
                             WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), s_wnd,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)s_font, TRUE);
    if (id >= 100 && id < 164) {
        s_ctl[id - 100] = c;
    }
    return c;
}

static void label(const wchar_t *t, int x, int y, int w)
{
    add(L"STATIC", t, SS_LEFT, x, y + 3, w, 20, 0);
}

/* ---------------------------------------------------------------- reflector list
 * hosts\M17Hosts.txt (UTF-8): one reflector per line, "NAME HOST [PORT]  # comment"; the comment is shown in the
 * list. The add / delete buttons write the file (add: appended at the end; delete: that line removed). */
typedef struct {
    char name[64], host[128], desc[128];
    unsigned port;
} refl_t;
static refl_t *s_refl;
static int s_refl_n, s_refl_cap;
static int s_shown[4000];                   /* combo row -> s_refl index */
static int s_shown_n;

static int refl_cmp(const void *a, const void *b)
{
    return _stricmp(((const refl_t *)a)->name, ((const refl_t *)b)->name);
}

static void hosts_path(wchar_t *path)
{
    swprintf(path, MAX_PATH, L"%ls\\hosts\\M17Hosts.txt", s_dir);
}

/* splits "NAME HOST [PORT] # comment"; false for blank and comment-only lines */
static bool parse_host_line(const char *line, refl_t *r)
{
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", line);
    buf[strcspn(buf, "\r\n")] = 0;
    char *hash = strchr(buf, '#');
    r->desc[0] = 0;
    if (hash) {
        *hash = 0;
        const char *d = hash + 1;
        while (*d == ' ' || *d == '\t') {
            ++d;
        }
        snprintf(r->desc, sizeof(r->desc), "%s", d);
    }
    unsigned port = 0;
    if (sscanf(buf, "%63s %127s %u", r->name, r->host, &port) < 2) {
        return false;
    }
    r->port = port ? port : M17_PORT;
    return true;
}

static void load_reflectors(void)
{
    s_refl_n = 0;
    wchar_t path[MAX_PATH];
    hosts_path(path);
    FILE *f = _wfopen(path, L"r");
    if (!f) {
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        refl_t r;
        if (!parse_host_line(line, &r)) {
            continue;
        }
        if (s_refl_n == s_refl_cap) {
            s_refl_cap = s_refl_cap ? s_refl_cap * 2 : 256;
            s_refl = realloc(s_refl, (size_t)s_refl_cap * sizeof(refl_t));
        }
        s_refl[s_refl_n++] = r;
    }
    fclose(f);
    qsort(s_refl, (size_t)s_refl_n, sizeof(refl_t), refl_cmp);
}

static bool contains_ci(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    if (!n) {
        return true;
    }
    for (; *hay; ++hay) {
        if (!_strnicmp(hay, needle, n)) {
            return true;
        }
    }
    return false;
}

static void fill_reflectors(void)
{
    wchar_t wf[64];
    char flt[192];
    GetWindowTextW(ctl(ID_FILTER), wf, 64);
    WideCharToMultiByte(CP_UTF8, 0, wf, -1, flt, sizeof(flt), NULL, NULL);
    HWND cb = ctl(ID_REFL);
    SendMessageW(cb, WM_SETREDRAW, FALSE, 0);
    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
    s_shown_n = 0;
    for (int i = 0; i < s_refl_n && s_shown_n < (int)(sizeof(s_shown) / sizeof(s_shown[0])); ++i) {
        const refl_t *r = &s_refl[i];
        if (!contains_ci(r->name, flt) && !contains_ci(r->host, flt) && !contains_ci(r->desc, flt)) {
            continue;
        }
        char a[400];
        wchar_t w[400];
        snprintf(a, sizeof(a), "%s  %s  (%s:%u)", r->name, r->desc, r->host, r->port);
        MultiByteToWideChar(CP_UTF8, 0, a, -1, w, 400);
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)w);
        s_shown[s_shown_n++] = i;
    }
    SendMessageW(cb, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(cb, NULL, TRUE);
}

static void pick_reflector(void)
{
    int row = (int)SendMessageW(ctl(ID_REFL), CB_GETCURSEL, 0, 0);
    if (row < 0 || row >= s_shown_n) {
        return;
    }
    const refl_t *r = &s_refl[s_shown[row]];
    SetWindowTextA(ctl(ID_HOST), r->host);
    char port[12];
    snprintf(port, sizeof(port), "%u", r->port);
    SetWindowTextA(ctl(ID_PORT), port);
}

static void select_reflector(const char *name)    /* show the list unfiltered and select NAME */
{
    SetWindowTextW(ctl(ID_FILTER), L"");
    fill_reflectors();
    for (int i = 0; i < s_shown_n; ++i) {
        if (!_stricmp(s_refl[s_shown[i]].name, name)) {
            SendMessageW(ctl(ID_REFL), CB_SETCURSEL, (WPARAM)i, 0);
            return;
        }
    }
}

/* rewrites hosts\M17Hosts.txt without the lines whose NAME is `name` (comments and order kept); appends `add` */
static bool rewrite_hosts(const char *name, const char *add)
{
    wchar_t path[MAX_PATH], tmp[MAX_PATH];
    hosts_path(path);
    swprintf(tmp, MAX_PATH, L"%ls.new", path);
    wchar_t dir[MAX_PATH];
    swprintf(dir, MAX_PATH, L"%ls\\hosts", s_dir);
    CreateDirectoryW(dir, NULL);
    FILE *in = _wfopen(path, L"rb");
    FILE *out = _wfopen(tmp, L"wb");
    if (!out) {
        if (in) {
            fclose(in);
        }
        return false;
    }
    char line[512];
    bool ends_nl = true;
    while (in && fgets(line, sizeof(line), in)) {
        refl_t r;
        if (name && parse_host_line(line, &r) && !_stricmp(r.name, name)) {
            continue;
        }
        fputs(line, out);
        size_t n = strlen(line);
        ends_nl = n && line[n - 1] == '\n';
    }
    if (in) {
        fclose(in);
    }
    if (add) {
        if (!ends_nl) {
            fputs("\n", out);
        }
        fputs(add, out);
        fputs("\n", out);
    }
    bool ok = fclose(out) == 0;
    return ok && MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING);
}

/* ---------------------------------------------------------------- settings */
static void ini_get(const wchar_t *key, const wchar_t *def, int id)
{
    wchar_t v[256];
    GetPrivateProfileStringW(L"m17_pc", key, def, v, 256, s_ini);
    SetWindowTextW(ctl(id), v);
}

static void ini_put(const wchar_t *key, int id)
{
    wchar_t v[256];
    GetWindowTextW(ctl(id), v, 256);
    WritePrivateProfileStringW(L"m17_pc", key, v, s_ini);
}

static int mode_sel(void)                         /* 0 reflector, 1 radio, 2 relay */
{
    return (int)SendMessageW(ctl(ID_MODE), CB_GETCURSEL, 0, 0);
}

static bool radio_mode(void)                       /* the sound card is used (radio or relay) */
{
    return mode_sel() >= 1;
}

static void load_settings(void)
{
    ini_get(L"host", L"", ID_HOST);
    ini_get(L"port", L"17000", ID_PORT);
    ini_get(L"callsign", L"", ID_CALL);
    ini_get(L"dest", L"@ALL", ID_DEST);
    ini_get(L"filter", L"", ID_FILTER);
    int mod = GetPrivateProfileIntW(L"m17_pc", L"module", 0, s_ini);
    SendMessageW(ctl(ID_MODULE), CB_SETCURSEL, (WPARAM)(mod >= 0 && mod < 26 ? mod : 0), 0);
    int mic = GetPrivateProfileIntW(L"m17_pc", L"mic", -1, s_ini);
    SendMessageW(ctl(ID_MIC), CB_SETCURSEL, (WPARAM)(mic + 1), 0);
    SendMessageW(ctl(ID_SAVE), BM_SETCHECK, GetPrivateProfileIntW(L"m17_pc", L"save_rx", 0, s_ini) ? BST_CHECKED : 0, 0);
    int md = GetPrivateProfileIntW(L"m17_pc", L"mode", 0, s_ini);
    SendMessageW(ctl(ID_MODE), CB_SETCURSEL, (WPARAM)(md >= 0 && md <= 2 ? md : 0), 0);
}

static void save_settings(void)
{
    ini_put(L"host", ID_HOST);
    ini_put(L"port", ID_PORT);
    ini_put(L"callsign", ID_CALL);
    ini_put(L"dest", ID_DEST);
    ini_put(L"filter", ID_FILTER);
    wchar_t v[16];
    swprintf(v, 16, L"%d", (int)SendMessageW(ctl(ID_MODULE), CB_GETCURSEL, 0, 0));
    WritePrivateProfileStringW(L"m17_pc", L"module", v, s_ini);
    swprintf(v, 16, L"%d", (int)SendMessageW(ctl(ID_MIC), CB_GETCURSEL, 0, 0) - 1);
    WritePrivateProfileStringW(L"m17_pc", L"mic", v, s_ini);
    WritePrivateProfileStringW(L"m17_pc", L"save_rx",
                               SendMessageW(ctl(ID_SAVE), BM_GETCHECK, 0, 0) == BST_CHECKED ? L"1" : L"0", s_ini);
    wchar_t ms[4];
    swprintf(ms, 4, L"%d", mode_sel());
    WritePrivateProfileStringW(L"m17_pc", L"mode", ms, s_ini);
}

static void get_a(int id, char *out, int size)
{
    wchar_t w[256];
    GetWindowTextW(ctl(id), w, 256);
    WideCharToMultiByte(CP_ACP, 0, w, -1, out, size, NULL, NULL);
    char *s = out;                               /* trim */
    while (*s == ' ') {
        ++s;
    }
    memmove(out, s, strlen(s) + 1);
    size_t n = strlen(out);
    while (n && out[n - 1] == ' ') {
        out[--n] = 0;
    }
}

/* ---------------------------------------------------------------- log */
/* every log line also goes to m17_pc_log_YYYYMMDD.txt next to the program (UTF-8): the window keeps only the last
 * part, and on 2026-11-19 a two-radio QSO could not be analysed afterwards because its log was only on screen */
static void log_file(const wchar_t *w)
{
    static FILE *f;
    static int day;
    SYSTEMTIME st;
    GetLocalTime(&st);
    if (!f || day != st.wDay) {
        if (f) {
            fclose(f);
        }
        wchar_t p[MAX_PATH];
        swprintf(p, MAX_PATH, L"%ls\\m17_pc_log_%04u%02u%02u.txt", s_dir, st.wYear, st.wMonth, st.wDay);
        f = _wfopen(p, L"ab");
        day = st.wDay;
        if (!f) {
            return;
        }
    }
    char u[1400];
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, u, sizeof(u), NULL, NULL) > 0) {
        fputs(u, f);
        fflush(f);
    }
}

static void append_log(const char *line)
{
    HWND e = ctl(ID_LOG);
    wchar_t w[600];
    int n = MultiByteToWideChar(CP_ACP, 0, line, -1, w, 590);
    if (n <= 0) {
        return;
    }
    wcscat(w, L"\r\n");
    log_file(w);
    int len = GetWindowTextLengthW(e);
    if (len > 60000) {
        SendMessageW(e, EM_SETSEL, 0, 20000);
        SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)L"");
        len = GetWindowTextLengthW(e);
    }
    SendMessageW(e, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)w);
}

static void append_logw(const wchar_t *text)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t w[700];
    swprintf(w, 700, L"%02u:%02u:%02u  %ls\r\n", st.wHour % 24u, st.wMinute % 60u, st.wSecond % 60u, text);
    log_file(w);
    HWND e = ctl(ID_LOG);
    int len = GetWindowTextLengthW(e);
    SendMessageW(e, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)w);
}

/* ---------------------------------------------------------------- licence check (once per callsign per run)
 * Opens the public search of 総務省 電波利用ポータル for the callsign in the user's own browser; the user sees
 * whether the station is listed. m17_pc keeps working either way. Off with license_check=0 in m17_pc.ini. */
static char s_checked[16][16];
static int s_checked_n;

static void license_check_open(const char *call)
{
    if (!GetPrivateProfileIntW(L"m17_pc", L"license_check", 1, s_ini)) {
        return;
    }
    char base[16];
    license_base_call(call, base);
    for (int i = 0; i < s_checked_n; ++i) {
        if (!strcmp(s_checked[i], base)) {
            return;                                  /* already shown during this run */
        }
    }
    if (s_checked_n < 16) {
        snprintf(s_checked[s_checked_n++], 16, "%s", base);
    }
    wchar_t w[600];
    if (!license_is_japanese(base)) {
        swprintf(w, 600, L"免許の照合: %hs は日本のアマチュア局の形式ではないので照合しません", base);
        append_logw(w);
        return;
    }
    char url[400];
    license_search_url(base, url, sizeof(url));
    if ((INT_PTR)ShellExecuteA(s_wnd, "open", url, NULL, NULL, SW_SHOWNORMAL) <= 32) {
        append_logw(L"免許の照合: ブラウザを開けませんでした。このまま使えます");
        return;
    }
    swprintf(w, 600, L"免許の照合: 総務省 電波利用ポータルの無線局等情報検索で %hs をブラウザに表示しました", base);
    append_logw(w);
    swprintf(w, 600,
             L"総務省 電波利用ポータルの「無線局等情報検索」で、%hs の検索結果をブラウザに表示しました。\n\n"
             L"一覧に「（%hs）」がない場合、無線局の免許を受けずに送信すると、電波法により罰せられる可能性があります。\n\n"
             L"このまま使い続けることはできます。",
             base, base);
    MessageBoxW(s_wnd, w, L"m17_pc  免許の照合", MB_OK | MB_ICONINFORMATION);
}

static void engine_log(const char *line)          /* engine thread -> window thread */
{
    char *copy = _strdup(line);
    if (!copy || !PostMessageW(s_wnd, WM_LOGLINE, 0, (LPARAM)copy)) {
        free(copy);
    }
}

/* ---------------------------------------------------------------- a one-line text prompt (modal) */
static struct {
    HWND wnd, edit;
    bool done, ok;
    wchar_t *io;
    int size;
} P;

static LRESULT CALLBACK prompt_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) {
            P.ok = LOWORD(wp) == IDOK;
            if (P.ok) {
                GetWindowTextW(P.edit, P.io, P.size);
            }
            P.done = true;
            return 0;
        }
        break;
    case WM_CLOSE:
        P.done = true;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static bool prompt(const wchar_t *title, const wchar_t *text, wchar_t *io, int size)
{
    static bool registered;
    if (!registered) {
        WNDCLASSW wc = { 0 };
        wc.lpfnWndProc = prompt_proc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"m17_pc_prompt";
        RegisterClassW(&wc);
        registered = true;
    }
    RECT pr, r = { 0, 0, S(420), S(124) };
    GetWindowRect(s_wnd, &pr);
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRect(&r, style, FALSE);
    int w = r.right - r.left, hgt = r.bottom - r.top;
    P.wnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"m17_pc_prompt", title, style, pr.left + ((pr.right - pr.left) - w) / 2,
                            pr.top + S(120), w, hgt, s_wnd, NULL, GetModuleHandleW(NULL), NULL);
    HWND lb = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT, S(12), S(10), S(396), S(40), P.wnd,
                              NULL, NULL, NULL);
    P.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", io, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                             S(12), S(52), S(396), S(24), P.wnd, NULL, NULL, NULL);
    HWND ok = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                              S(232), S(86), S(84), S(28), P.wnd, (HMENU)IDOK, NULL, NULL);
    HWND cancel = CreateWindowExW(0, L"BUTTON", L"やめる", WS_CHILD | WS_VISIBLE | WS_TABSTOP, S(324), S(86), S(84),
                                  S(28), P.wnd, (HMENU)IDCANCEL, NULL, NULL);
    HWND all[4] = { lb, P.edit, ok, cancel };
    for (int i = 0; i < 4; ++i) {
        SendMessageW(all[i], WM_SETFONT, (WPARAM)s_font, TRUE);
    }
    P.io = io;
    P.size = size;
    P.done = P.ok = false;
    EnableWindow(s_wnd, FALSE);
    ShowWindow(P.wnd, SW_SHOW);
    SetFocus(P.edit);
    SendMessageW(P.edit, EM_SETSEL, 0, -1);
    MSG m;
    while (!P.done && GetMessageW(&m, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(P.wnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    EnableWindow(s_wnd, TRUE);
    DestroyWindow(P.wnd);
    SetForegroundWindow(s_wnd);
    return P.ok;
}

/* ---------------------------------------------------------------- radio settings ([radio] in m17_pc.ini) */
static int rini(const wchar_t *key, int def)
{
    return (int)GetPrivateProfileIntW(L"radio", key, def, s_ini);
}

static void radio_cfg_from_ini(radio_cfg_t *c)
{
    radio_default_cfg(c);
    c->in_device = rini(L"in", RADIO_DEV_DEFAULT);
    c->out_device = rini(L"out", RADIO_DEV_DEFAULT);
    c->ptt = (radio_ptt_t)rini(L"ptt", RP_NONE);
    if (c->ptt > RP_CIV) {
        c->ptt = RP_NONE;
    }
    wchar_t w[32];
    GetPrivateProfileStringW(L"radio", L"port", L"", w, 32, s_ini);
    WideCharToMultiByte(CP_ACP, 0, w, -1, c->ptt_port, sizeof(c->ptt_port), NULL, NULL);
    GetPrivateProfileStringW(L"radio", L"civ_addr", L"A4", w, 32, s_ini);
    c->civ_addr = (unsigned)wcstoul(w, NULL, 16) & 0xFF;
    c->civ_baud = (unsigned)rini(L"civ_baud", 115200);
    c->tx_dbfs = (float)rini(L"tx_level_db", -10);
    c->tx_delay_ms = (unsigned)rini(L"tx_delay_ms", 150);
    c->tx_tail_ms = (unsigned)rini(L"tx_tail_ms", 50);
    c->tx_invert = rini(L"tx_invert", 0) != 0;
    /* a radio without a data port over USB (IC-705): IF receive and TX precompensation */
    c->rx_if = c->tx_pre = rini(L"usb_radio", 0) != 0;
    c->pre_floor_hz = (float)rini(L"pre_floor_hz", 50);
    c->pre_gain_db = (float)rini(L"pre_gain_db", 30);
    /* defaults: the IC-705 FM-D path measured with taikou-test1-1 on 2026-11-18 */
    c->pre_radio.hp_hz = rini(L"radio_hp_hz", 110);
    c->pre_radio.hp_order = rini(L"radio_hp_order", 2);
    c->pre_radio.preemph_us = rini(L"radio_preemph_us", 5000);
    /* equalizers learned with taikou-test1-1 (eq_A_tx.txt / eq_A.txt): only with the USB radio */
    wchar_t p[260];
    GetPrivateProfileStringW(L"radio", L"tx_eq", L"", p, 260, s_ini);
    WideCharToMultiByte(CP_ACP, 0, p, -1, c->tx_eq, sizeof(c->tx_eq), NULL, NULL);
    GetPrivateProfileStringW(L"radio", L"rx_eq", L"", p, 260, s_ini);
    WideCharToMultiByte(CP_ACP, 0, p, -1, c->rx_eq, sizeof(c->rx_eq), NULL, NULL);
    if (!c->rx_if) {
        c->tx_eq[0] = c->rx_eq[0] = 0;
    }
}

static void wput_int(const wchar_t *key, int v)
{
    wchar_t w[16];
    swprintf(w, 16, L"%d", v);
    WritePrivateProfileStringW(L"radio", key, w, s_ini);
}

enum { RD_IN = 300, RD_OUT, RD_PTT, RD_PORT, RD_ADDR, RD_BAUD, RD_LEVEL, RD_DELAY, RD_INV, RD_N2R, RD_KEEP, RD_CMD,
       RD_USB, RD_FLOOR, RD_PGAIN, RD_TXEQ, RD_RXEQ };
static struct {
    HWND wnd;
    bool done, ok;
} RDLG;

static LRESULT CALLBACK radio_dlg_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) {
            RDLG.ok = LOWORD(wp) == IDOK;
            RDLG.done = true;
            return 0;
        }
        break;
    case WM_CLOSE:
        RDLG.done = true;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static HWND rd_add(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExW(!wcscmp(cls, L"EDIT") ? WS_EX_CLIENTEDGE : 0, cls, text, WS_CHILD | WS_VISIBLE | style,
                             S(x), S(y), S(w), S(h), RDLG.wnd, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)s_font, TRUE);
    return c;
}

static void rd_devices(HWND cb, bool input, int cur)
{
    SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"Windows の既定");
    SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"内部で折り返す（試験・音は出ない）");
    int n = input ? radio_in_count() : radio_out_count();
    for (int i = 0; i < n; ++i) {
        char a[96];
        wchar_t w[96];
        if (input ? radio_in_name(i, a, sizeof(a)) : radio_out_name(i, a, sizeof(a))) {
            MultiByteToWideChar(CP_ACP, 0, a, -1, w, 96);
            SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)w);
        }
    }
    SendMessageW(cb, CB_SETCURSEL, (WPARAM)(cur == RADIO_DEV_LOOP ? 1 : cur + 1), 0);
}

static int rd_device(HWND cb)
{
    int i = (int)SendMessageW(cb, CB_GETCURSEL, 0, 0);
    return i <= 0 ? RADIO_DEV_DEFAULT : (i == 1 ? RADIO_DEV_LOOP : i - 2);
}

static void radio_settings(void)
{
    static bool registered;
    if (!registered) {
        WNDCLASSW wc = { 0 };
        wc.lpfnWndProc = radio_dlg_proc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"m17_pc_radio";
        RegisterClassW(&wc);
        registered = true;
    }
    radio_cfg_t c;
    radio_cfg_from_ini(&c);
    RECT pr, r = { 0, 0, S(520), S(662) };
    GetWindowRect(s_wnd, &pr);
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRect(&r, style, FALSE);
    int w = r.right - r.left, hgt = r.bottom - r.top;
    RDLG.wnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"m17_pc_radio", L"無線機の設定", style,
                               pr.left + ((pr.right - pr.left) - w) / 2, pr.top + S(60), w, hgt, s_wnd, NULL,
                               GetModuleHandleW(NULL), NULL);
    int y = 12;
    rd_add(L"STATIC", L"DATA 端子（9600bps 用）を PC の音声入出力に、または IC-705 を USB でつなぎます。",
           SS_LEFT, 12, y, 496, 20, 0);
    y += 30;
    rd_add(L"STATIC", L"入力（無線機から）", SS_LEFT, 12, y + 3, 130, 20, 0);
    HWND in = rd_add(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 146, y, 362, 300, RD_IN);
    rd_devices(in, true, c.in_device);
    y += 32;
    rd_add(L"STATIC", L"出力（無線機へ）", SS_LEFT, 12, y + 3, 130, 20, 0);
    HWND out = rd_add(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 146, y, 362, 300, RD_OUT);
    rd_devices(out, false, c.out_device);
    y += 40;
    rd_add(L"STATIC", L"PTT", SS_LEFT, 12, y + 3, 130, 20, 0);
    HWND ptt = rd_add(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, 146, y, 200, 200, RD_PTT);
    static const wchar_t *pn[4] = { L"なし（VOX・手動）", L"RTS", L"DTR", L"CI-V（Icom）" };
    for (int i = 0; i < 4; ++i) {
        SendMessageW(ptt, CB_ADDSTRING, 0, (LPARAM)pn[i]);
    }
    SendMessageW(ptt, CB_SETCURSEL, (WPARAM)c.ptt, 0);
    rd_add(L"STATIC", L"ポート", SS_LEFT, 360, y + 3, 50, 20, 0);
    wchar_t t[32];
    swprintf(t, 32, L"%hs", c.ptt_port);
    HWND port = rd_add(L"EDIT", t, ES_UPPERCASE | ES_AUTOHSCROLL | WS_TABSTOP, 412, y, 96, 24, RD_PORT);
    y += 32;
    rd_add(L"STATIC", L"CI-V アドレス（16進）", SS_LEFT, 12, y + 3, 150, 20, 0);
    swprintf(t, 32, L"%02X", c.civ_addr);
    HWND addr = rd_add(L"EDIT", t, ES_UPPERCASE | WS_TABSTOP, 166, y, 50, 24, RD_ADDR);
    rd_add(L"STATIC", L"速度", SS_LEFT, 232, y + 3, 40, 20, 0);
    swprintf(t, 32, L"%u", c.civ_baud);
    HWND baud = rd_add(L"EDIT", t, ES_NUMBER | WS_TABSTOP, 274, y, 80, 24, RD_BAUD);
    rd_add(L"STATIC", L"IC-705 は A4", SS_LEFT, 366, y + 3, 140, 20, 0);
    y += 40;
    rd_add(L"STATIC", L"送信レベル（dBFS）", SS_LEFT, 12, y + 3, 150, 20, 0);
    swprintf(t, 32, L"%d", (int)c.tx_dbfs);
    HWND level = rd_add(L"EDIT", t, WS_TABSTOP, 166, y, 50, 24, RD_LEVEL);
    rd_add(L"STATIC", L"送信遅延（ms）", SS_LEFT, 232, y + 3, 110, 20, 0);
    swprintf(t, 32, L"%u", c.tx_delay_ms);
    HWND delay = rd_add(L"EDIT", t, ES_NUMBER | WS_TABSTOP, 346, y, 60, 24, RD_DELAY);
    y += 32;
    HWND inv = rd_add(L"BUTTON", L"送信の変調を反転する（相手のログに inverted と出るとき）", BS_AUTOCHECKBOX | WS_TABSTOP, 12, y,
                      496, 24, RD_INV);
    SendMessageW(inv, BM_SETCHECK, c.tx_invert ? BST_CHECKED : 0, 0);
    y += 30;
    HWND usb = rd_add(L"BUTTON", L"DATA 端子のない無線機を USB でつなぐ（IC-705）",
                      BS_AUTOCHECKBOX | WS_TABSTOP, 12, y, 496, 24, RD_USB);
    SendMessageW(usb, BM_SETCHECK, c.rx_if ? BST_CHECKED : 0, 0);
    y += 28;
    rd_add(L"STATIC", L"低域をどこまで戻すか（Hz）", SS_LEFT, 30, y + 3, 170, 20, 0);
    swprintf(t, 32, L"%d", (int)c.pre_floor_hz);
    HWND floor_ = rd_add(L"EDIT", t, ES_NUMBER | WS_TABSTOP, 204, y, 50, 24, RD_FLOOR);
    rd_add(L"STATIC", L"補正後のレベル（dB）", SS_LEFT, 268, y + 3, 150, 20, 0);
    swprintf(t, 32, L"%d", (int)c.pre_gain_db);
    HWND pgain = rd_add(L"EDIT", t, WS_TABSTOP, 420, y, 50, 24, RD_PGAIN);
    y += 28;
    rd_add(L"STATIC", L"送信イコライザー", SS_LEFT, 30, y + 3, 120, 20, 0);
    wchar_t ep[260];
    MultiByteToWideChar(CP_ACP, 0, c.tx_eq, -1, ep, 260);
    HWND txeq = rd_add(L"EDIT", ep, ES_AUTOHSCROLL | WS_TABSTOP, 154, y, 354, 24, RD_TXEQ);
    y += 28;
    rd_add(L"STATIC", L"受信イコライザー", SS_LEFT, 30, y + 3, 120, 20, 0);
    MultiByteToWideChar(CP_ACP, 0, c.rx_eq, -1, ep, 260);
    HWND rxeq = rd_add(L"EDIT", ep, ES_AUTOHSCROLL | WS_TABSTOP, 154, y, 354, 24, RD_RXEQ);
    y += 28;
    rd_add(L"STATIC", L"受信は 12 kHz の IF、送信は補正。無線機は FM-D、DATA MOD＝USB、USB AF/IF 出力＝IF。"
           L"IC-705 の実測値：送信レベル -47、戻す下限 50、補正後 30。イコライザーは taikou-test1-1 で作るファイル"
           L"（送信 eq_A_tx.txt、受信 eq_A.txt。空欄なら使わない）。", SS_LEFT, 30, y, 478, 56, 0);
    y += 62;
    rd_add(L"STATIC", L"中継モード：無線機で受けた通話は、いつもリフレクターへ送ります。", SS_LEFT, 12, y, 496, 20, 0);
    y += 24;
    HWND n2r = rd_add(L"BUTTON", L"リフレクターの通話を、無線機から送信する（自動で電波が出ます）", BS_AUTOCHECKBOX | WS_TABSTOP,
                      12, y, 496, 24, RD_N2R);
    SendMessageW(n2r, BM_SETCHECK, rini(L"relay_to_radio", 0) ? BST_CHECKED : 0, 0);
    y += 26;
    HWND keep = rd_add(L"BUTTON", L"そのとき送信元を相手のコールサインのままにする（既定は自局のコールサイン）",
                       BS_AUTOCHECKBOX | WS_TABSTOP, 30, y, 478, 24, RD_KEEP);
    SendMessageW(keep, BM_SETCHECK, rini(L"relay_keep_src", 0) ? BST_CHECKED : 0, 0);
    y += 26;
    HWND cmd = rd_add(L"BUTTON", L"無線機からの操作を受け付け、返事を送信する（宛先 M17-XXX A・UNLINK など）",
                      BS_AUTOCHECKBOX | WS_TABSTOP, 12, y, 496, 24, RD_CMD);
    SendMessageW(cmd, BM_SETCHECK, rini(L"relay_commands", 1) ? BST_CHECKED : 0, 0);
    y += 34;
    rd_add(L"STATIC",
           L"マイク端子・スピーカー端子では通りません（300 Hz〜3 kHz の帯域では復調できないことを試験で確認）。"
           L"USB の IC-705 は 2026-11-18 に 2 台の対向試験（ダミーロード）で、送信イコライザーを使って受信を確認しました。"
           L"送信の前に、免許の範囲か確かめてください。調整はダミーロードで。",
           SS_LEFT, 12, y, 496, 60, 0);
    y += 70;
    rd_add(L"BUTTON", L"OK", BS_DEFPUSHBUTTON | WS_TABSTOP, 332, y, 84, 28, IDOK);
    rd_add(L"BUTTON", L"やめる", WS_TABSTOP, 424, y, 84, 28, IDCANCEL);

    RDLG.done = RDLG.ok = false;
    EnableWindow(s_wnd, FALSE);
    ShowWindow(RDLG.wnd, SW_SHOW);
    MSG m;
    while (!RDLG.done && GetMessageW(&m, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(RDLG.wnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    if (RDLG.ok) {
        wput_int(L"in", rd_device(in));
        wput_int(L"out", rd_device(out));
        wput_int(L"ptt", (int)SendMessageW(ptt, CB_GETCURSEL, 0, 0));
        GetWindowTextW(port, t, 32);
        WritePrivateProfileStringW(L"radio", L"port", t, s_ini);
        GetWindowTextW(addr, t, 32);
        WritePrivateProfileStringW(L"radio", L"civ_addr", t, s_ini);
        GetWindowTextW(baud, t, 32);
        WritePrivateProfileStringW(L"radio", L"civ_baud", t, s_ini);
        GetWindowTextW(level, t, 32);
        wput_int(L"tx_level_db", _wtoi(t) > 0 ? 0 : _wtoi(t));
        GetWindowTextW(delay, t, 32);
        WritePrivateProfileStringW(L"radio", L"tx_delay_ms", t, s_ini);
        wput_int(L"tx_invert", SendMessageW(inv, BM_GETCHECK, 0, 0) == BST_CHECKED);
        wput_int(L"usb_radio", SendMessageW(usb, BM_GETCHECK, 0, 0) == BST_CHECKED);
        GetWindowTextW(floor_, t, 32);
        wput_int(L"pre_floor_hz", _wtoi(t) < 5 ? 5 : _wtoi(t));
        GetWindowTextW(pgain, t, 32);
        wput_int(L"pre_gain_db", _wtoi(t) > 40 ? 40 : _wtoi(t));   /* +30 for the IC-705 (its pre-emphasis) */
        wchar_t ep2[260];
        GetWindowTextW(txeq, ep2, 260);
        WritePrivateProfileStringW(L"radio", L"tx_eq", ep2, s_ini);
        GetWindowTextW(rxeq, ep2, 260);
        WritePrivateProfileStringW(L"radio", L"rx_eq", ep2, s_ini);
        wput_int(L"relay_to_radio", SendMessageW(n2r, BM_GETCHECK, 0, 0) == BST_CHECKED);
        wput_int(L"relay_keep_src", SendMessageW(keep, BM_GETCHECK, 0, 0) == BST_CHECKED);
        wput_int(L"relay_commands", SendMessageW(cmd, BM_GETCHECK, 0, 0) == BST_CHECKED);
        append_logw(L"無線機の設定を保存しました");
    }
    EnableWindow(s_wnd, TRUE);
    DestroyWindow(RDLG.wnd);
    SetForegroundWindow(s_wnd);
}

/* ---------------------------------------------------------------- add to / delete from the reflector list */
static void add_reflector(void)
{
    char host[128], port_s[16];
    get_a(ID_HOST, host, sizeof(host));
    get_a(ID_PORT, port_s, sizeof(port_s));
    unsigned port = (unsigned)atoi(port_s);
    if (!host[0] || strchr(host, ' ') || strchr(host, '#')) {
        MessageBoxW(s_wnd, L"先に「ホスト」と「ポート」に、追加したいリフレクターを入れてください。", L"m17_pc  一覧に追加",
                    MB_ICONINFORMATION);
        return;
    }
    if (!port) {
        port = M17_PORT;
    }
    wchar_t name[64] = L"";
    me_status_t st;
    me_get_status(&st);
    if (s_connected && st.reflector[0]) {
        MultiByteToWideChar(CP_ACP, 0, st.reflector, -1, name, 64);     /* the name the reflector gives in PING */
    } else {
        for (int i = 0; i < s_refl_n; ++i) {
            if (!_stricmp(s_refl[i].host, host) && s_refl[i].port == port) {
                MultiByteToWideChar(CP_UTF8, 0, s_refl[i].name, -1, name, 64);
                break;
            }
        }
    }
    wchar_t text[300];
    swprintf(text, 300, L"%hs:%u を一覧に追加します。名前を入れてください（例 M17-XXX、空白なし）。", host, port);
    if (!prompt(L"m17_pc  一覧に追加", text, name, 64)) {
        return;
    }
    char n8[64];
    int k = 0;
    for (const wchar_t *c = name; *c && k < 32; ++c) {               /* letters, digits and - _ / . only */
        wchar_t ch = *c >= L'a' && *c <= L'z' ? (wchar_t)(*c - 32) : *c;
        if ((ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9') || ch == L'-' || ch == L'_' || ch == L'/' || ch == L'.') {
            n8[k++] = (char)ch;
        } else if (ch == L' ' && k) {
            n8[k++] = '-';
        }
    }
    n8[k] = 0;
    if (!k) {
        MessageBoxW(s_wnd, L"名前には英字・数字・ - _ / . を使ってください。", L"m17_pc  一覧に追加", MB_ICONINFORMATION);
        return;
    }
    wchar_t desc[128] = L"";
    if (!prompt(L"m17_pc  一覧に追加", L"説明（なくてもよい。一覧の名前の横に出ます）", desc, 128)) {
        return;
    }
    for (wchar_t *c = desc; *c; ++c) {
        if (*c == L'#' || *c == L'\r' || *c == L'\n') {
            *c = L' ';
        }
    }
    bool exists = false;
    for (int i = 0; i < s_refl_n; ++i) {
        exists |= !_stricmp(s_refl[i].name, n8);
    }
    if (exists) {
        wchar_t q[200];
        swprintf(q, 200, L"%hs はもう一覧にあります。新しいホストとポートで置き換えますか？", n8);
        if (MessageBoxW(s_wnd, q, L"m17_pc  一覧に追加", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) {
            return;
        }
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    char d8[400], line[700];
    WideCharToMultiByte(CP_UTF8, 0, desc, -1, d8, sizeof(d8), NULL, NULL);
    snprintf(line, sizeof(line), "%-8s %-28s %-5u  # %s%s(追加 %04u-%02u-%02u)", n8, host, port, d8, d8[0] ? " " : "",
             t.wYear, t.wMonth, t.wDay);
    if (!rewrite_hosts(exists ? n8 : NULL, line)) {
        MessageBoxW(s_wnd, L"hosts\\M17Hosts.txt に書き込めませんでした。", L"m17_pc  一覧に追加", MB_ICONWARNING);
        return;
    }
    load_reflectors();
    select_reflector(n8);
    wchar_t w[300];
    swprintf(w, 300, L"一覧に追加しました: %hs  %hs:%u", n8, host, port);
    append_logw(w);
}

static void delete_reflector(void)
{
    int row = (int)SendMessageW(ctl(ID_REFL), CB_GETCURSEL, 0, 0);
    if (row < 0 || row >= s_shown_n) {
        MessageBoxW(s_wnd, L"消したいリフレクターを「リフレクター」の一覧で選んでください。", L"m17_pc  一覧から削除",
                    MB_ICONINFORMATION);
        return;
    }
    refl_t r = s_refl[s_shown[row]];
    wchar_t q[300];
    swprintf(q, 300, L"%hs（%hs:%u）を一覧から消しますか？\n（hosts\\M17Hosts.txt から、その行がなくなります）", r.name,
             r.host, r.port);
    if (MessageBoxW(s_wnd, q, L"m17_pc  一覧から削除", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) {
        return;
    }
    if (!rewrite_hosts(r.name, NULL)) {
        MessageBoxW(s_wnd, L"hosts\\M17Hosts.txt に書き込めませんでした。", L"m17_pc  一覧から削除", MB_ICONWARNING);
        return;
    }
    load_reflectors();
    fill_reflectors();
    wchar_t w[200];
    swprintf(w, 200, L"一覧から削除しました: %hs", r.name);
    append_logw(w);
}

/* ---------------------------------------------------------------- connect / PTT */
static void set_enabled_fields(bool on)
{
    static const int ids[] = { ID_FILTER, ID_REFL, ID_HOST, ID_PORT, ID_MODULE, ID_CALL, ID_DEST, ID_MIC, ID_SAVE, ID_DEL,
                               ID_MODE, ID_RADIOSET };
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i) {
        EnableWindow(ctl(ids[i]), on);
    }
}

static void do_connect(void)
{
    if (s_connected) {
        me_stop();
        s_connected = false;
        SetWindowTextW(ctl(ID_CONNECT), L"接続");
        set_enabled_fields(true);
        return;
    }
    me_cfg_t c;
    me_default_cfg(&c);
    get_a(ID_HOST, c.host, sizeof(c.host));
    char tmp[32];
    get_a(ID_PORT, tmp, sizeof(tmp));
    c.port = (uint16_t)(atoi(tmp) ? atoi(tmp) : M17_PORT);
    c.module = (char)('A' + (int)SendMessageW(ctl(ID_MODULE), CB_GETCURSEL, 0, 0));
    get_a(ID_CALL, c.callsign, sizeof(c.callsign));
    CharUpperA(c.callsign);
    get_a(ID_DEST, c.dest, sizeof(c.dest));
    CharUpperA(c.dest);
    c.mic_device = (int)SendMessageW(ctl(ID_MIC), CB_GETCURSEL, 0, 0) - 1;
    c.spk_device = (int)GetPrivateProfileIntW(L"m17_pc", L"spk", -1, s_ini);   /* speaker: only in the ini */
    c.tx_gain_db = (float)GetPrivateProfileIntW(L"m17_pc", L"tx_gain_db", 0, s_ini);   /* gains: only in the ini */
    c.rx_gain_db = (float)GetPrivateProfileIntW(L"m17_pc", L"rx_gain_db", 0, s_ini);
    if (SendMessageW(ctl(ID_SAVE), BM_GETCHECK, 0, 0) == BST_CHECKED) {
        wchar_t d[MAX_PATH];
        swprintf(d, MAX_PATH, L"%ls\\recordings", s_dir);
        CreateDirectoryW(d, NULL);
        WideCharToMultiByte(CP_ACP, 0, d, -1, c.wav_dir, sizeof(c.wav_dir), NULL, NULL);
    }
    if (radio_mode()) {
        c.radio = true;
        radio_cfg_from_ini(&c.rc);
        c.gateway = mode_sel() == 2;
        c.gw_net_to_rf = rini(L"relay_to_radio", 0) != 0;
        c.gw_keep_src = rini(L"relay_keep_src", 0) != 0;
        c.gw_cushion_ms = (unsigned)rini(L"relay_cushion_ms", 200);
        c.gw_commands = c.gw_replies = rini(L"relay_commands", 1) != 0;
        {
            wchar_t hp[MAX_PATH];
            hosts_path(hp);
            WideCharToMultiByte(CP_ACP, 0, hp, -1, c.hosts_file, sizeof(c.hosts_file), NULL, NULL);
        }
        if (!c.gateway) {
            snprintf(c.host, sizeof(c.host), "radio");
        }
        if (c.gateway && (c.gw_net_to_rf || c.gw_replies) && c.rc.in_device != RADIO_DEV_LOOP &&
            c.rc.out_device != RADIO_DEV_LOOP) {
            const wchar_t *msg =
                c.gw_net_to_rf
                    ? L"中継モードで「リフレクターの通話を無線機から送信する」が入っています。\n"
                      L"リフレクターに誰かが話すたびに、あなたの無線機から自動で電波が出ます。\n\n"
                      L"・ほかの人の通話を自局の電波で送ることが、あなたの免許・運用の条件で認められているか確かめてください。\n"
                      L"・無人で動かさず、そばで監視してください。\n"
                      L"・調整のうちはダミーロードで。\n\n"
                      L"この設定で接続しますか？"
                    : L"中継モードで「無線機からの操作」が入っています。\n"
                      L"無線機から操作（宛先 M17-XXX A・UNLINK・ECHO・INFO）を受けると、返事として自動で短く電波が出ます。\n\n"
                      L"・無人で動かさず、そばで監視してください。\n"
                      L"・調整のうちはダミーロードで。\n\n"
                      L"この設定で接続しますか？";
            int a = MessageBoxW(s_wnd, msg, L"m17_pc  中継の確認", MB_OKCANCEL | MB_ICONWARNING);
            if (a != IDOK) {
                return;
            }
        }
        if (c.rc.ptt != RP_NONE && !c.rc.ptt_port[0]) {
            MessageBoxW(s_wnd, L"「無線機の設定」で PTT のポート（COM5 など）を入れてください。", L"m17_pc",
                        MB_ICONINFORMATION);
            return;
        }
    }
    if ((!c.radio && !c.host[0]) || !c.callsign[0]) {
        MessageBoxW(s_wnd, L"リフレクター（ホスト）とコールサインを入れてください。", L"m17_pc", MB_ICONINFORMATION);
        return;
    }
    save_settings();
    if (!me_start(&c, engine_log)) {
        append_log("[E] cannot start (codec or audio)");
        return;
    }
    s_connected = true;
    SetWindowTextW(ctl(ID_CONNECT), L"切断");
    set_enabled_fields(false);
    license_check_open(c.callsign);
}

static unsigned parrot_port(void)
{
    unsigned p = (unsigned)GetPrivateProfileIntW(L"m17_pc", L"parrot_port", PARROT_PORT, s_ini);
    return p ? p : PARROT_PORT;
}

/* The built-in test reflector: start it and point host/port at it, or stop it. */
static void toggle_parrot(void)
{
    if (parrot_running()) {
        parrot_stop();
        SetWindowTextW(ctl(ID_PARROT), L"パロットで試す");
        return;
    }
    if (!parrot_start((uint16_t)parrot_port(), engine_log)) {
        MessageBoxW(s_wnd, L"試験用リフレクターを起動できませんでした（ポートが使われています）。\n"
                           L"m17_pc.ini の parrot_port で別の番号にできます。", L"m17_pc", MB_ICONWARNING);
        return;
    }
    SetWindowTextW(ctl(ID_PARROT), L"パロットを止める");
    if (!s_connected) {
        SetWindowTextW(ctl(ID_HOST), L"127.0.0.1");
        wchar_t p[12];
        swprintf(p, 12, L"%u", parrot_port());
        SetWindowTextW(ctl(ID_PORT), p);
        append_log("host and port now point at the parrot: connect, then talk with F2 (you hear yourself 1 s later)");
    }
}

static bool to_parrot(void)                       /* connected to our own test reflector? */
{
    char host[64], port[16];
    get_a(ID_HOST, host, sizeof(host));
    get_a(ID_PORT, port, sizeof(port));
    return parrot_running() && (!strcmp(host, "127.0.0.1") || !_stricmp(host, "localhost")) &&
           (unsigned)atoi(port) == parrot_port();
}

static void toggle_ptt(void)
{
    me_status_t st;
    me_get_status(&st);
    if (!s_connected || st.link != ML_UP) {
        return;
    }
    if (!st.tx && !s_warned && radio_mode()) {
        radio_cfg_t rc;
        radio_cfg_from_ini(&rc);
        if (rc.in_device != RADIO_DEV_LOOP && rc.out_device != RADIO_DEV_LOOP) {
            int a = MessageBoxW(s_wnd,
                                L"無線機から M17 の電波が出ます。\n"
                                L"M17（4値FSK）での送信が、あなたの無線局の免許の範囲に入っているか確かめてください。\n"
                                L"調整のうちは、アンテナの代わりにダミーロードをつないでください。\n\n"
                                L"送信しますか？（この確認は、このアプリを閉じるまで一度だけ出ます）",
                                L"m17_pc  送信の確認", MB_OKCANCEL | MB_ICONWARNING);
            if (a != IDOK) {
                return;
            }
        }
        s_warned = true;
    }
    if (!st.tx && !s_warned && !to_parrot()) {
        int a = MessageBoxW(s_wnd,
                            L"送信した声は、リフレクターにつながっている世界中の局に届きます。\n"
                            L"アマチュア無線の免許を持つ本人が、そのコールサインで送信してください。\n\n"
                            L"送信しますか？（この確認は、このアプリを閉じるまで一度だけ出ます）",
                            L"m17_pc  送信の確認", MB_OKCANCEL | MB_ICONWARNING);
        if (a != IDOK) {
            return;
        }
        s_warned = true;
    }
    me_set_ptt(!st.tx);
}

/* ---------------------------------------------------------------- status */
static int bar_pos(float dbfs)                   /* -60..0 dBFS -> 0..60 */
{
    int v = (int)(dbfs + 60.0f);
    return v < 0 ? 0 : (v > 60 ? 60 : v);
}

static void set_text_if(HWND c, const wchar_t *t)
{
    wchar_t cur[300];
    GetWindowTextW(c, cur, 300);
    if (wcscmp(cur, t)) {
        SetWindowTextW(c, t);
    }
}

static void update_status(void)
{
    me_status_t st;
    me_get_status(&st);
    if (s_connected && !me_running()) {          /* the engine stopped by itself (socket, codec) */
        s_connected = false;
        SetWindowTextW(ctl(ID_CONNECT), L"接続");
        set_enabled_fields(true);
    }
    wchar_t t[300];
    if (!s_connected) {
        wcscpy(t, L"未接続");
    } else if (st.tx) {
        swprintf(t, 300, L"送信中  %.1f 秒", st.tx_seconds);
    } else if (st.link == ML_UP) {
        if (st.rx) {
            wcscpy(t, L"受信中");
        } else if (!strcmp(st.reflector, "RADIO")) {
            wcscpy(t, L"無線機  待受");
        } else if (st.radio_up) {
            swprintf(t, 300, L"中継中  %hs", st.reflector[0] ? st.reflector : "");
        } else if (st.reflector[0]) {
            swprintf(t, 300, L"接続中  %hs", st.reflector);
        } else {
            wcscpy(t, L"接続中（待受）");
        }
    } else if (st.link == ML_OFF && st.radio_up) {
        wcscpy(t, L"中継  リフレクター未接続");
    } else if (st.link == ML_REFUSED) {
        wcscpy(t, L"接続を断られました");
    } else {
        wcscpy(t, L"接続しています…");
    }
    set_text_if(ctl(ID_STATUS), t);
    if (st.rx) {
        swprintf(t, 300, L"%hs  →  %hs   Codec 2 %hs   欠落 %u %%", st.rx_call, st.rx_dest, st.rx_type, st.rx_lost_pct);
    } else {
        t[0] = 0;
    }
    set_text_if(ctl(ID_RXINFO), t);
    set_text_if(ctl(ID_PTT), st.tx ? L"送信をやめる (F2)" : L"送信 PTT (F2)");
    EnableWindow(ctl(ID_PTT), s_connected && st.link == ML_UP);
    SendMessageW(ctl(ID_MICBAR), PBM_SETPOS, (WPARAM)(st.tx ? bar_pos(st.mic_dbfs) : 0), 0);
    SendMessageW(ctl(ID_RXBAR), PBM_SETPOS, (WPARAM)(st.rx ? bar_pos(st.rx_dbfs) : 0), 0);

    char top[40] = "";
    if (st.heard_n) {
        snprintf(top, sizeof(top), "%s%s", st.heard[0].time, st.heard[0].call);
    }
    if (st.heard_n != s_heard_shown || strcmp(top, s_heard_top)) {
        HWND lv = ctl(ID_HEARD);
        ListView_DeleteAllItems(lv);
        for (int i = 0; i < st.heard_n; ++i) {
            const me_heard_t *h = &st.heard[i];
            wchar_t c0[16], c1[16], c2[16], c3[8], c4[16];
            swprintf(c0, 16, L"%hs", h->time);
            swprintf(c1, 16, L"%hs", h->call);
            swprintf(c2, 16, L"%hs", h->dest);
            swprintf(c3, 8, L"%hs", h->type);
            swprintf(c4, 16, L"%.1f 秒", h->seconds);
            LVITEMW it = { 0 };
            it.mask = LVIF_TEXT;
            it.iItem = i;
            it.pszText = c0;
            ListView_InsertItem(lv, &it);
            ListView_SetItemText(lv, i, 1, c1);
            ListView_SetItemText(lv, i, 2, c2);
            ListView_SetItemText(lv, i, 3, c3);
            ListView_SetItemText(lv, i, 4, c4);
        }
        s_heard_shown = st.heard_n;
        snprintf(s_heard_top, sizeof(s_heard_top), "%s", top);
    }
}

/* ---------------------------------------------------------------- window */
static void create_controls(void)
{
    int x = 12, y = 12;
    label(L"接続先", x, y, 110);
    add(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, x + 116, y, 236, 200, ID_MODE);
    SendMessageW(ctl(ID_MODE), CB_ADDSTRING, 0, (LPARAM)L"リフレクター（インターネット）");
    SendMessageW(ctl(ID_MODE), CB_ADDSTRING, 0, (LPARAM)L"無線機（サウンドカード）");
    SendMessageW(ctl(ID_MODE), CB_ADDSTRING, 0, (LPARAM)L"中継（無線機 ⇔ リフレクター）");
    SendMessageW(ctl(ID_MODE), CB_SETCURSEL, 0, 0);
    add(L"BUTTON", L"無線機の設定…", BS_PUSHBUTTON | WS_TABSTOP, x + 358, y, 102, 25, ID_RADIOSET);
    y += 32;
    label(L"一覧の絞り込み", x, y, 110);
    add(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, x + 116, y, 140, 24, ID_FILTER);
    add(L"STATIC", L"（名前・ホストの一部）", SS_LEFT, x + 264, y + 3, 200, 20, 0);
    y += 32;
    label(L"リフレクター", x, y, 110);
    add(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, x + 116, y, 236, 400, ID_REFL);
    SendMessageW(ctl(ID_REFL), CB_SETDROPPEDWIDTH, (WPARAM)S(560), 0);
    add(L"BUTTON", L"追加", BS_PUSHBUTTON | WS_TABSTOP, x + 358, y, 49, 25, ID_ADD);
    add(L"BUTTON", L"削除", BS_PUSHBUTTON | WS_TABSTOP, x + 411, y, 49, 25, ID_DEL);
    y += 32;
    label(L"ホスト", x, y, 110);
    add(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, x + 116, y, 220, 24, ID_HOST);
    label(L"ポート", x + 346, y, 40);
    add(L"EDIT", L"17000", ES_NUMBER | WS_TABSTOP, x + 390, y, 70, 24, ID_PORT);
    y += 32;
    label(L"モジュール", x, y, 110);
    add(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, x + 116, y, 60, 400, ID_MODULE);
    for (int i = 0; i < 26; ++i) {
        wchar_t m[2] = { (wchar_t)(L'A' + i), 0 };
        SendMessageW(ctl(ID_MODULE), CB_ADDSTRING, 0, (LPARAM)m);
    }
    label(L"宛先", x + 196, y, 40);
    add(L"EDIT", L"@ALL", ES_UPPERCASE | ES_AUTOHSCROLL | WS_TABSTOP, x + 240, y, 110, 24, ID_DEST);
    SendMessageW(ctl(ID_DEST), EM_LIMITTEXT, 9, 0);
    y += 32;
    label(L"コールサイン", x, y, 110);
    add(L"EDIT", L"", ES_UPPERCASE | ES_AUTOHSCROLL | WS_TABSTOP, x + 116, y, 110, 24, ID_CALL);
    SendMessageW(ctl(ID_CALL), EM_LIMITTEXT, 9, 0);
    add(L"STATIC", L"（9 文字まで）", SS_LEFT, x + 236, y + 3, 200, 20, 0);
    y += 32;
    label(L"マイク", x, y, 110);
    add(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, x + 116, y, 344, 300, ID_MIC);
    SendMessageW(ctl(ID_MIC), CB_ADDSTRING, 0, (LPARAM)L"Windows の既定");
    for (int i = 0; i < mic_device_count(); ++i) {
        char name[128];
        wchar_t w[128];
        if (mic_device_name(i, name, sizeof(name))) {
            MultiByteToWideChar(CP_ACP, 0, name, -1, w, 128);
            SendMessageW(ctl(ID_MIC), CB_ADDSTRING, 0, (LPARAM)w);
        }
    }
    y += 32;
    add(L"BUTTON", L"受信した通話を recordings\\ に保存", BS_AUTOCHECKBOX | WS_TABSTOP, x + 116, y, 344, 24, ID_SAVE);
    y += 40;
    add(L"BUTTON", L"接続", BS_PUSHBUTTON | WS_TABSTOP, x, y, 120, 36, ID_CONNECT);
    add(L"BUTTON", L"パロットで試す", BS_PUSHBUTTON | WS_TABSTOP, x + 132, y, 150, 36, ID_PARROT);
    add(L"STATIC", L"（自分の声が 1 秒後に返る）", SS_LEFT, x + 290, y + 9, 180, 20, 0);
    y += 48;
    HWND ptt = add(L"BUTTON", L"送信 PTT (F2)", BS_PUSHBUTTON | WS_TABSTOP, x, y, 200, 80, ID_PTT);
    SendMessageW(ptt, WM_SETFONT, (WPARAM)s_big, TRUE);
    EnableWindow(ptt, FALSE);
    add(L"STATIC", L"未接続", SS_LEFT | SS_NOPREFIX, x + 214, y, 250, 22, ID_STATUS);
    SendMessageW(ctl(ID_STATUS), WM_SETFONT, (WPARAM)s_big, TRUE);
    add(L"STATIC", L"マイク", SS_LEFT, x + 214, y + 32, 50, 20, 0);
    add(PROGRESS_CLASSW, L"", PBS_SMOOTH, x + 268, y + 34, 192, 14, ID_MICBAR);
    add(L"STATIC", L"受信", SS_LEFT, x + 214, y + 56, 50, 20, 0);
    add(PROGRESS_CLASSW, L"", PBS_SMOOTH, x + 268, y + 58, 192, 14, ID_RXBAR);
    SendMessageW(ctl(ID_MICBAR), PBM_SETRANGE, 0, MAKELPARAM(0, 60));
    SendMessageW(ctl(ID_RXBAR), PBM_SETRANGE, 0, MAKELPARAM(0, 60));
    y += 92;
    add(L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, x, y, 460, 22, ID_RXINFO);
    y += 28;
    add(L"STATIC", L"送信は Codec 2 3200 です。受信は 3200 と 1600（音声＋データ）を再生します。",
        SS_LEFT, x, y, 460, 40, 0);

    int rx = 488;
    add(L"STATIC", L"受信履歴", SS_LEFT, rx, 12, 200, 20, 0);
    HWND lv = add(WC_LISTVIEWW, L"", LVS_REPORT | LVS_NOSORTHEADER | LVS_SINGLESEL, rx, 34, 460, 200, ID_HEARD);
    ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    static const wchar_t *cols[5] = { L"時刻", L"コールサイン", L"宛先", L"方式", L"長さ" };
    static const int widths[5] = { 70, 110, 110, 50, 80 };
    for (int i = 0; i < 5; ++i) {
        LVCOLUMNW c = { 0 };
        c.mask = LVCF_TEXT | LVCF_WIDTH;
        c.pszText = (wchar_t *)cols[i];
        c.cx = S(widths[i]);
        ListView_InsertColumn(lv, i, &c);
    }
    add(L"STATIC", L"ログ", SS_LEFT, rx, 244, 200, 20, 0);
    add(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, rx, 266, 460, 258, ID_LOG);
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_FILTER:
            if (HIWORD(wp) == EN_CHANGE) {
                fill_reflectors();
            }
            return 0;
        case ID_REFL:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                pick_reflector();
            }
            return 0;
        case ID_CONNECT:
            do_connect();
            return 0;
        case ID_PTT:
            toggle_ptt();
            return 0;
        case ID_PARROT:
            toggle_parrot();
            return 0;
        case ID_ADD:
            add_reflector();
            return 0;
        case ID_DEL:
            delete_reflector();
            return 0;
        case ID_RADIOSET:
            radio_settings();
            return 0;
        }
        break;
    case WM_TIMER:
        update_status();
        return 0;
    case WM_LOGLINE:
        append_log((const char *)lp);
        free((void *)lp);
        return 0;
    case WM_CTLCOLORSTATIC:
        if ((HWND)lp == ctl(ID_STATUS)) {
            me_status_t st;
            me_get_status(&st);
            SetTextColor((HDC)wp, st.tx ? RGB(0xC0, 0x10, 0x10) : (st.rx ? RGB(0x10, 0x70, 0x10) : RGB(0x30, 0x30, 0x30)));
            SetBkMode((HDC)wp, TRANSPARENT);
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        break;
    case WM_CLOSE:
        if (s_connected) {
            me_stop();
        }
        parrot_stop();
        save_settings();
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmd, int show)
{
    (void)prev;
    (void)cmd;
    SetProcessDPIAware();
    HDC dc = GetDC(NULL);
    s_dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(NULL, dc);
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&icc);

    GetModuleFileNameW(NULL, s_dir, MAX_PATH);
    wchar_t *slash = wcsrchr(s_dir, L'\\');
    if (slash) {
        *slash = 0;
    }
    swprintf(s_ini, MAX_PATH, L"%ls\\m17_pc.ini", s_dir);

    NONCLIENTMETRICSW ncm;
    memset(&ncm, 0, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    ncm.lfMessageFont.lfHeight = -S(13);
    s_font = CreateFontIndirectW(&ncm.lfMessageFont);
    ncm.lfMessageFont.lfHeight = -S(17);
    ncm.lfMessageFont.lfWeight = FW_BOLD;
    s_big = CreateFontIndirectW(&ncm.lfMessageFont);

    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"m17_pc";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    RegisterClassW(&wc);
    RECT r = { 0, 0, S(960), S(536) };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX), FALSE);
    s_wnd = CreateWindowW(L"m17_pc", L"m17_pc  -  M17（Codec 2）  リフレクター／無線機",
                          WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX),
                          CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
    create_controls();
    load_reflectors();
    load_settings();
    fill_reflectors();
    if (!s_refl_n) {
        append_log("[W] no reflectors in hosts\\M17Hosts.txt: type the host yourself");
    } else {
        char line[96];
        snprintf(line, sizeof(line), "%d reflectors in hosts\\M17Hosts.txt", s_refl_n);
        append_log(line);
    }
    SetTimer(s_wnd, ID_TIMER, 100, NULL);
    ShowWindow(s_wnd, show);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (m.message == WM_KEYDOWN && m.wParam == VK_F2 && !(m.lParam & 0x40000000)) {
            toggle_ptt();
            continue;
        }
        if (!IsDialogMessageW(s_wnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    return 0;
}
