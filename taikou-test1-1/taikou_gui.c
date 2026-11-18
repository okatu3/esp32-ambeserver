/* taikou-gui: a window for taikou-test1-1.exe (two IC-705s into dummy loads).
 * It does not measure anything itself: every button runs taikou-test1-1.exe (same folder) with the right options
 * and shows its output. Two slots run at the same time, so one PC can drive both radios:
 *   left  = receiver side (B): ifcheck, tones-rx, m17-rx, selftest, analyze
 *   right = transmitter side (A): civ, tones-tx, m17-tx
 * "止める" sets a named event (TAIKOU_STOP_EVENT) that taikou-test1-1 treats like Ctrl+C: PTT off, recording ends
 * and is analysed. The B3 values that tones-rx prints are copied into the M17 fields. Everything shown is also
 * written to taikou_log_<date>.txt next to the exe. Settings: taikou-gui.ini next to the exe. */
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <mmsystem.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define WM_APP_LINE (WM_APP + 1)   /* wParam slot, lParam malloc'ed wchar_t* */
#define WM_APP_DONE (WM_APP + 2)   /* wParam slot, lParam exit code */
#define TIMER_KILL 100              /* + slot */

enum { SL_RX, SL_TX, NSLOT };

enum {
    ID_IN = 101, ID_OUT, ID_COM, ID_REFRESH, ID_CALL, ID_SEC, ID_TAG, ID_RXAF,
    ID_IFCHECK, ID_TONESRX, ID_M17RX, ID_STOPRX, ID_SELFTEST, ID_ANALYZE,
    ID_CIV, ID_TONESTX, ID_M17TX, ID_STOPTX,
    ID_HP, ID_ORDER, ID_PRE, ID_FLOOR, ID_GAIN, ID_LEVEL, ID_TLEVEL, ID_TRAINEQ, ID_USEEQ, ID_TXEQ, ID_REFINE,
    ID_OUTRX, ID_OUTTX, ID_LBLRX, ID_LBLTX, ID_HINT
};

typedef struct {
    HANDLE proc, rd, stop_ev;
    bool running;
} slot_t;

static HWND g_wnd, g_out[NSLOT];
static slot_t g_slot[NSLOT];
static HFONT g_font, g_bold, g_mono;
static int g_dpi = 96;
static wchar_t g_dir[MAX_PATH], g_exe[MAX_PATH], g_ini[MAX_PATH], g_log[MAX_PATH];
static FILE *g_logf;

static int S(int v) { return MulDiv(v, g_dpi, 96); }
static HWND C(int id) { return GetDlgItem(g_wnd, id); }

/* ---------------------------------------------------------------- output */
static void log_line(int slot, const wchar_t *s)
{
    if (!g_logf) {
        g_logf = _wfopen(g_log, L"ab");
        if (!g_logf) {
            return;
        }
    }
    char u[4096];
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, u, sizeof(u), NULL, NULL);
    if (n > 0) {
        fprintf(g_logf, "%s %s\r\n", slot == SL_RX ? "[RX]" : "[TX]", u);
        fflush(g_logf);
    }
}

static void append(int slot, const wchar_t *s)
{
    HWND e = g_out[slot];
    int len = GetWindowTextLengthW(e);
    SendMessageW(e, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)s);
    SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
    SendMessageW(e, EM_SCROLLCARET, 0, 0);
    log_line(slot, s);
}

static void appendf(int slot, const wchar_t *fmt, ...)
{
    wchar_t b[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(b, 1023, fmt, ap);
    b[1023] = 0;
    va_end(ap);
    append(slot, b);
}

/* ---------------------------------------------------------------- devices, settings */
static void fill_devices(void)
{
    HWND in = C(ID_IN), out = C(ID_OUT), com = C(ID_COM);
    int si = (int)SendMessageW(in, CB_GETCURSEL, 0, 0), so = (int)SendMessageW(out, CB_GETCURSEL, 0, 0);
    int want_in = si >= 0 ? (int)SendMessageW(in, CB_GETITEMDATA, si, 0) : (int)GetPrivateProfileIntW(L"taikou", L"in", -1, g_ini);
    int want_out = so >= 0 ? (int)SendMessageW(out, CB_GETITEMDATA, so, 0) : (int)GetPrivateProfileIntW(L"taikou", L"out", -1, g_ini);
    wchar_t want_com[32];
    if (!GetWindowTextW(com, want_com, 32)) {
        GetPrivateProfileStringW(L"taikou", L"com", L"", want_com, 32, g_ini);
    }
    SendMessageW(in, CB_RESETCONTENT, 0, 0);
    SendMessageW(out, CB_RESETCONTENT, 0, 0);
    SendMessageW(com, CB_RESETCONTENT, 0, 0);
    wchar_t t[128];
    for (UINT i = 0; i < waveInGetNumDevs(); ++i) {
        WAVEINCAPSW c;
        if (waveInGetDevCapsW(i, &c, sizeof(c)) == MMSYSERR_NOERROR) {
            swprintf(t, 128, L"%u: %ls", i, c.szPname);
            int k = (int)SendMessageW(in, CB_ADDSTRING, 0, (LPARAM)t);
            SendMessageW(in, CB_SETITEMDATA, k, i);
            if ((int)i == want_in) SendMessageW(in, CB_SETCURSEL, k, 0);
        }
    }
    for (UINT i = 0; i < waveOutGetNumDevs(); ++i) {
        WAVEOUTCAPSW c;
        if (waveOutGetDevCapsW(i, &c, sizeof(c)) == MMSYSERR_NOERROR) {
            swprintf(t, 128, L"%u: %ls", i, c.szPname);
            int k = (int)SendMessageW(out, CB_ADDSTRING, 0, (LPARAM)t);
            SendMessageW(out, CB_SETITEMDATA, k, i);
            if ((int)i == want_out) SendMessageW(out, CB_SETCURSEL, k, 0);
        }
    }
    static wchar_t names[32768];
    DWORD n = QueryDosDeviceW(NULL, names, 32768);
    int ports[256], np = 0;
    for (wchar_t *p = names; n && *p && np < 256; p += wcslen(p) + 1) {
        if (!wcsncmp(p, L"COM", 3) && p[3] >= L'0' && p[3] <= L'9') ports[np++] = _wtoi(p + 3);
    }
    for (int i = 0; i < np; ++i) {          /* sort by number */
        for (int j = i + 1; j < np; ++j) {
            if (ports[j] < ports[i]) { int x = ports[i]; ports[i] = ports[j]; ports[j] = x; }
        }
    }
    for (int i = 0; i < np; ++i) {
        swprintf(t, 128, L"COM%d", ports[i]);
        int k = (int)SendMessageW(com, CB_ADDSTRING, 0, (LPARAM)t);
        if (!wcscmp(t, want_com)) SendMessageW(com, CB_SETCURSEL, k, 0);
    }
}

static const struct { int id; const wchar_t *key, *def; } EDITS[] = {
    { ID_CALL, L"call", L"" }, { ID_SEC, L"sec", L"60" }, { ID_TAG, L"tag", L"A" },
    { ID_HP, L"radio_hp", L"" }, { ID_ORDER, L"radio_hp_order", L"" }, { ID_PRE, L"radio_preemph", L"" },
    { ID_FLOOR, L"pre_floor", L"50" }, { ID_GAIN, L"pre_gain", L"-36" }, { ID_LEVEL, L"level", L"" },
    { ID_TLEVEL, L"tones_level", L"" },
};

static void load_settings(void)
{
    wchar_t v[64];
    for (size_t i = 0; i < sizeof(EDITS) / sizeof(EDITS[0]); ++i) {
        GetPrivateProfileStringW(L"taikou", EDITS[i].key, EDITS[i].def, v, 64, g_ini);
        SetWindowTextW(C(EDITS[i].id), v);
    }
    SendMessageW(C(ID_RXAF), BM_SETCHECK, GetPrivateProfileIntW(L"taikou", L"rx_af", 0, g_ini) ? BST_CHECKED : BST_UNCHECKED, 0);
    static const struct { int id; const wchar_t *key; } CB[] = { { ID_TRAINEQ, L"train_eq" }, { ID_USEEQ, L"use_eq" }, { ID_TXEQ, L"tx_eq" },
                                                                 { ID_REFINE, L"refine_eq" } };
    for (size_t i = 0; i < 4; ++i)
        SendMessageW(C(CB[i].id), BM_SETCHECK, GetPrivateProfileIntW(L"taikou", CB[i].key, 0, g_ini) ? BST_CHECKED : BST_UNCHECKED, 0);
}

static int sel_dev(int id)
{
    int k = (int)SendMessageW(C(id), CB_GETCURSEL, 0, 0);
    return k < 0 ? -1 : (int)SendMessageW(C(id), CB_GETITEMDATA, k, 0);
}

static bool checked(int id)
{
    return SendMessageW(C(id), BM_GETCHECK, 0, 0) == BST_CHECKED;
}

/* eq_<記録名>.txt next to the exe: learned by m17-rx (B3), used by m17-rx / analyze, and by m17-tx for B4 */
static void eq_path(wchar_t *out, int n)
{
    wchar_t tag[32];
    GetWindowTextW(C(ID_TAG), tag, 32);
    swprintf(out, n, L"%ls\\eq_%ls.txt", g_dir, tag[0] ? tag : L"A");
}

/* the transmit equalizer for B4: eq_<記録名>_tx.txt (refined from a received B4) if there is one, else eq_<記録名>.txt */
static void tx_eq_path(wchar_t *out, int n, bool *refined)
{
    wchar_t tag[32];
    GetWindowTextW(C(ID_TAG), tag, 32);
    swprintf(out, n, L"%ls\\eq_%ls_tx.txt", g_dir, tag[0] ? tag : L"A");
    *refined = GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES;
    if (!*refined) eq_path(out, n);
}

/* receiver side: learn (needs the transmitter's callsign: it is part of the burst) or use the equalizer */
static void rx_eq_opts(wchar_t *a)
{
    wchar_t e[MAX_PATH], call[32];
    size_t l = wcslen(a);
    eq_path(e, MAX_PATH);
    GetWindowTextW(C(ID_CALL), call, 32);
    if (checked(ID_TRAINEQ)) {
        swprintf(a + l, 1024 - l, L" --train-eq \"%ls\" --call %ls", e, call[0] ? call : L"N0CALL");
    } else if (checked(ID_USEEQ)) {
        swprintf(a + l, 1024 - l, L" --eq \"%ls\"", e);
    }
    if (checked(ID_REFINE)) {                /* learn what is left in the received B4 and fold it into the TX equalizer */
        wchar_t base[MAX_PATH], out[MAX_PATH], tag[32];
        bool refined;
        tx_eq_path(base, MAX_PATH, &refined);
        GetWindowTextW(C(ID_TAG), tag, 32);
        swprintf(out, MAX_PATH, L"%ls\\eq_%ls_tx.txt", g_dir, tag[0] ? tag : L"A");
        l = wcslen(a);
        swprintf(a + l, 1024 - l, L" --refine-eq \"%ls\" --refine-out \"%ls\" --call %ls", base, out, call[0] ? call : L"N0CALL");
    }
}

static void save_settings(void)
{
    wchar_t v[64];
    for (size_t i = 0; i < sizeof(EDITS) / sizeof(EDITS[0]); ++i) {
        GetWindowTextW(C(EDITS[i].id), v, 64);
        WritePrivateProfileStringW(L"taikou", EDITS[i].key, v, g_ini);
    }
    swprintf(v, 64, L"%d", sel_dev(ID_IN));
    WritePrivateProfileStringW(L"taikou", L"in", v, g_ini);
    swprintf(v, 64, L"%d", sel_dev(ID_OUT));
    WritePrivateProfileStringW(L"taikou", L"out", v, g_ini);
    GetWindowTextW(C(ID_COM), v, 64);
    WritePrivateProfileStringW(L"taikou", L"com", v, g_ini);
    WritePrivateProfileStringW(L"taikou", L"rx_af", SendMessageW(C(ID_RXAF), BM_GETCHECK, 0, 0) == BST_CHECKED ? L"1" : L"0", g_ini);
    WritePrivateProfileStringW(L"taikou", L"train_eq", checked(ID_TRAINEQ) ? L"1" : L"0", g_ini);
    WritePrivateProfileStringW(L"taikou", L"use_eq", checked(ID_USEEQ) ? L"1" : L"0", g_ini);
    WritePrivateProfileStringW(L"taikou", L"tx_eq", checked(ID_TXEQ) ? L"1" : L"0", g_ini);
    WritePrivateProfileStringW(L"taikou", L"refine_eq", checked(ID_REFINE) ? L"1" : L"0", g_ini);
}

/* ---------------------------------------------------------------- running taikou-test1-1 */
static void set_buttons(void)
{
    static const int RX[] = { ID_IFCHECK, ID_TONESRX, ID_M17RX, ID_SELFTEST, ID_ANALYZE };
    static const int TX[] = { ID_CIV, ID_TONESTX, ID_M17TX };
    for (size_t i = 0; i < sizeof(RX) / sizeof(RX[0]); ++i) EnableWindow(C(RX[i]), !g_slot[SL_RX].running);
    for (size_t i = 0; i < sizeof(TX) / sizeof(TX[0]); ++i) EnableWindow(C(TX[i]), !g_slot[SL_TX].running);
    EnableWindow(C(ID_STOPRX), g_slot[SL_RX].running);
    EnableWindow(C(ID_STOPTX), g_slot[SL_TX].running);
    SetWindowTextW(C(ID_LBLRX), g_slot[SL_RX].running ? L"受信側（B）の表示 ― 実行中" : L"受信側（B）の表示");
    SetWindowTextW(C(ID_LBLTX), g_slot[SL_TX].running ? L"送信側（A）の表示 ― 実行中（止める＝PTT を切る）" : L"送信側（A）の表示");
}

typedef struct { int slot; HANDLE rd, proc; } reader_t;

static DWORD WINAPI reader(LPVOID arg)
{
    reader_t r = *(reader_t *)arg;
    free(arg);
    char line[4096];
    int n = 0;
    char buf[1024];
    DWORD got;
    for (;;) {
        bool ok = ReadFile(r.rd, buf, sizeof(buf), &got, NULL) && got > 0;
        for (DWORD i = 0; ok && i <= got; ++i) {
            bool end = i == got;
            if (end) break;
            char c = buf[i];
            if (c == '\r') continue;
            if (c != '\n' && n < (int)sizeof(line) - 1) { line[n++] = c; continue; }
            if (c != '\n') continue;
            line[n] = 0;
            int wl = MultiByteToWideChar(CP_ACP, 0, line, -1, NULL, 0);
            wchar_t *w = malloc((size_t)wl * sizeof(wchar_t));
            MultiByteToWideChar(CP_ACP, 0, line, -1, w, wl);
            PostMessageW(g_wnd, WM_APP_LINE, (WPARAM)r.slot, (LPARAM)w);
            n = 0;
        }
        if (!ok) break;
    }
    if (n > 0) {
        line[n] = 0;
        int wl = MultiByteToWideChar(CP_ACP, 0, line, -1, NULL, 0);
        wchar_t *w = malloc((size_t)wl * sizeof(wchar_t));
        MultiByteToWideChar(CP_ACP, 0, line, -1, w, wl);
        PostMessageW(g_wnd, WM_APP_LINE, (WPARAM)r.slot, (LPARAM)w);
    }
    WaitForSingleObject(r.proc, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(r.proc, &code);
    PostMessageW(g_wnd, WM_APP_DONE, (WPARAM)r.slot, (LPARAM)code);
    return 0;
}

static bool run(int slot, const wchar_t *args, const wchar_t *what)
{
    slot_t *s = &g_slot[slot];
    if (s->running) return false;
    if (GetFileAttributesW(g_exe) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(g_wnd, L"taikou-test1-1.exe が、この画面の exe と同じフォルダーにありません。", L"taikou", MB_ICONERROR);
        return false;
    }
    save_settings();
    static wchar_t cmd[4096];
    swprintf(cmd, 4096, L"\"%ls\" %ls", g_exe, args);
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    wchar_t ev[96];
    static unsigned seq;
    swprintf(ev, 96, L"Local\\taikou-stop-%lu-%d-%u", GetCurrentProcessId(), slot, ++seq);
    s->stop_ev = CreateEventW(NULL, TRUE, FALSE, ev);
    char eva[96];
    WideCharToMultiByte(CP_ACP, 0, ev, -1, eva, sizeof(eva), NULL, NULL);
    SetEnvironmentVariableA("TAIKOU_STOP_EVENT", eva);
    STARTUPINFOW si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = wr;
    si.hStdInput = NULL;
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, g_dir, &si, &pi);
    SetEnvironmentVariableA("TAIKOU_STOP_EVENT", NULL);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        CloseHandle(s->stop_ev);
        appendf(slot, L"起動できません（%lu）", GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    s->proc = pi.hProcess;
    s->rd = rd;
    s->running = true;
    SYSTEMTIME t;
    GetLocalTime(&t);
    append(slot, L"");
    appendf(slot, L"=== %02d:%02d:%02d  %ls", t.wHour, t.wMinute, t.wSecond, what);
    appendf(slot, L"> taikou-test1-1 %ls", args);
    reader_t *r = malloc(sizeof(*r));
    r->slot = slot;
    r->rd = rd;
    r->proc = pi.hProcess;
    CloseHandle(CreateThread(NULL, 0, reader, r, 0, NULL));
    set_buttons();
    return true;
}

static void stop(int slot)
{
    slot_t *s = &g_slot[slot];
    if (!s->running) return;
    SetEvent(s->stop_ev);
    append(slot, slot == SL_TX ? L"--- 止めます（PTT を切ります）" : L"--- 止めます（ここまでの録音を解析します）");
    SetTimer(g_wnd, TIMER_KILL + slot, slot == SL_TX ? 5000 : 30000, NULL);
}

/* ---------------------------------------------------------------- building the options */
static bool get_text(int id, wchar_t *v, int n)
{
    GetWindowTextW(C(id), v, n);
    wchar_t *p = v;
    while (*p == L' ' || *p == 0x3000) ++p;
    memmove(v, p, (wcslen(p) + 1) * sizeof(wchar_t));
    int l = (int)wcslen(v);
    while (l > 0 && (v[l - 1] == L' ' || v[l - 1] == 0x3000)) v[--l] = 0;
    return v[0] != 0;
}

static void wav_name(const wchar_t *kind, wchar_t *out, int n)
{
    wchar_t tag[32];
    if (!get_text(ID_TAG, tag, 32)) wcscpy(tag, L"A");
    for (int k = 1; k < 1000; ++k) {
        if (k == 1) swprintf(out, n, L"%ls_%ls.wav", kind, tag);
        else swprintf(out, n, L"%ls_%ls_%d.wav", kind, tag, k);
        wchar_t full[MAX_PATH];
        swprintf(full, MAX_PATH, L"%ls\\%ls", g_dir, out);
        if (GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES) return;
    }
}

/* "N: マイク (4- USB Audio CODEC )" -> 4; a USB Audio CODEC without a number -> 0; anything else -> -1 */
static int radio_no(int id, wchar_t *name, int n)
{
    name[0] = 0;
    int k = (int)SendMessageW(C(id), CB_GETCURSEL, 0, 0);
    if (k < 0) return -1;
    SendMessageW(C(id), CB_GETLBTEXT, k, (LPARAM)name);
    if (!wcsstr(name, L"USB Audio CODEC")) return -1;
    const wchar_t *p = wcschr(name, L'(');
    if (p && p[1] >= L'0' && p[1] <= L'9') {
        int v = _wtoi(p + 1);
        while (*++p >= L'0' && *p <= L'9') {}
        if (*p == L'-') return v;
    }
    return 0;
}

static bool ask(const wchar_t *msg)
{
    return MessageBoxW(g_wnd, msg, L"確認", MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) == IDYES;
}

/* the checks that would have caught the 2026-11-18 setup: a PC microphone as the receiver, both sides on one radio */
static bool check_devices(bool rx_side)
{
    wchar_t ni[160], no[160], m[512];
    int ri = radio_no(ID_IN, ni, 160), ro = radio_no(ID_OUT, no, 160);
    if (rx_side && ri < 0) {
        swprintf(m, 512, L"受信側（B）の入力「%ls」は、IC-705 の USB Audio CODEC ではありません。\n"
                         L"PC のマイクなどでは、無線機の受信は録音できません。\n\nこのまま続けますか？", ni);
        if (!ask(m)) return false;
    }
    if (!rx_side && ro < 0) {
        swprintf(m, 512, L"送信側（A）の出力「%ls」は、IC-705 の USB Audio CODEC ではありません。\n"
                         L"PC のスピーカーなどでは、無線機に音が入りません。\n\nこのまま続けますか？", no);
        if (!ask(m)) return false;
    }
    if (ri > 0 && ro > 0 && ri == ro) {
        swprintf(m, 512, L"受信側（B）の入力と送信側（A）の出力が、同じ無線機（%d- USB Audio CODEC）です。\n"
                         L"受信側には、もう 1 台の番号（「%d-」以外）を選んでください。\n\nこのまま続けますか？", ri, ri);
        if (!ask(m)) return false;
    }
    return true;
}

static bool need_in(int *in)
{
    *in = sel_dev(ID_IN);
    if (*in < 0) {
        MessageBoxW(g_wnd, L"受信側（B）の「入力」を選んでください。", L"taikou", MB_ICONINFORMATION);
        return false;
    }
    return check_devices(true);
}

static bool need_tx(int *out, wchar_t *com)
{
    *out = sel_dev(ID_OUT);
    GetWindowTextW(C(ID_COM), com, 32);
    if (*out < 0 || !com[0]) {
        MessageBoxW(g_wnd, L"送信側（A）の「出力」と「COM」を選んでください。", L"taikou", MB_ICONINFORMATION);
        return false;
    }
    return check_devices(false);
}

static bool confirm_tx(void)
{
    return MessageBoxW(g_wnd,
                       L"送信側（A）の IC-705 から送信します。\n\n"
                       L"・アンテナ端子はダミーロードですか？\n"
                       L"・RF 出力は最小ですか？\n"
                       L"・周波数とモード（FM-D）は免許の範囲ですか？\n"
                       L"・受信側（B）は先に動かしましたか？\n\n"
                       L"送信は 45 秒までで、止めるボタンや終了で必ず PTT を切ります。",
                       L"送信の確認", MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) == IDYES;
}

/* the receiver must know the level tones-tx used (the 試験音 field; -20 when empty), or the deviation per full scale
 * and the M17 levels come out wrong by the difference (2026-11-18: 26.8 dB) */
static void tones_level(wchar_t *a)
{
    wchar_t v[32];
    size_t l = wcslen(a);
    GetWindowTextW(C(ID_TLEVEL), v, 32);
    swprintf(a + l, 1024 - l, L" --level %ls", v[0] ? v : L"-20");
}

static void rx_af(wchar_t *a)
{
    if (SendMessageW(C(ID_RXAF), BM_GETCHECK, 0, 0) == BST_CHECKED) wcscat(a, L" --rx-af");
}

static void on_button(int id)
{
    wchar_t a[1024], v[64], com[32], w[MAX_PATH];
    int in, out;
    a[0] = 0;
    switch (id) {
    case ID_REFRESH:
        fill_devices();
        break;
    case ID_SELFTEST:
        run(SL_RX, L"selftest", L"自己試験（無線機なし、30 秒ほど）");
        break;
    case ID_IFCHECK:
        if (!need_in(&in)) return;
        swprintf(a, 1024, L"ifcheck --in %d --sec 10", in);
        rx_af(a);
        if (run(SL_RX, a, L"IF の確認（10 秒）"))
            append(SL_RX, L"この間に、送信側（A）の PTT を手で数秒押してください（無変調）。");
        break;
    case ID_TONESRX:
    case ID_M17RX:
        if (!need_in(&in)) return;
        if (SendMessageW(C(ID_RXAF), BM_GETCHECK, 0, 0) == BST_CHECKED &&
            !ask(L"「B の USB 出力が音声（AF）のとき」にチェックが入っています。\n"
                 L"受信側の出力を IF にしているなら、チェックを外してください（AF のままだと解析できません）。\n\n"
                 L"このまま（AF として）続けますか？"))
            return;
        get_text(ID_SEC, v, 64);
        wav_name(id == ID_TONESRX ? L"tones" : L"m17", w, MAX_PATH);
        swprintf(a, 1024, L"%ls --in %d --sec %ls --wav \"%ls\"", id == ID_TONESRX ? L"tones-rx" : L"m17-rx", in,
                 v[0] ? v : L"60", w);
        if (id == ID_TONESRX) tones_level(a);
        else rx_eq_opts(a);
        rx_af(a);
        if (run(SL_RX, a, id == ID_TONESRX ? L"試験音を受信（送信側の音声処理を測る）" : L"M17 を受信（B1・B2・B3）"))
            appendf(SL_RX, L"録音は %ls に保存します。送信側（A）の送信が終わったら「止める」で早めに解析できます。", w);
        break;
    case ID_ANALYZE: {
        wchar_t file[MAX_PATH] = L"";
        OPENFILENAMEW of;
        memset(&of, 0, sizeof(of));
        of.lStructSize = sizeof(of);
        of.hwndOwner = g_wnd;
        of.lpstrFilter = L"WAV\0*.wav\0";
        of.lpstrFile = file;
        of.nMaxFile = MAX_PATH;
        of.lpstrInitialDir = g_dir;
        of.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetOpenFileNameW(&of)) return;
        const wchar_t *base = wcsrchr(file, L'\\') ? wcsrchr(file, L'\\') + 1 : file;
        bool tones;
        if (!_wcsnicmp(base, L"tones", 5)) tones = true;
        else if (!_wcsnicmp(base, L"m17", 3)) tones = false;
        else tones = MessageBoxW(g_wnd, L"試験音の録音ですか？\n（はい＝試験音、いいえ＝M17）", L"解析", MB_YESNO) == IDYES;
        swprintf(a, 1024, L"analyze %ls \"%ls\"", tones ? L"tones" : L"m17", file);
        if (tones) tones_level(a);
        else rx_eq_opts(a);
        rx_af(a);
        run(SL_RX, a, L"WAV を解析");
        break;
    }
    case ID_CIV:
        GetWindowTextW(C(ID_COM), com, 32);
        if (!com[0]) { MessageBoxW(g_wnd, L"送信側（A）の「COM」を選んでください。", L"taikou", MB_ICONINFORMATION); return; }
        swprintf(a, 1024, L"civ %ls", com);
        run(SL_TX, a, L"無線機を確認（読むだけ。無線機は何も変えない）");
        break;
    case ID_TONESTX:
        if (!need_tx(&out, com) || !confirm_tx()) return;
        swprintf(a, 1024, L"tones-tx --out %d --ptt %ls", out, com);
        if (get_text(ID_TLEVEL, v, 64)) {
            size_t l = wcslen(a);
            swprintf(a + l, 1024 - l, L" --level %ls", v);
        }
        run(SL_TX, a, L"試験音を送信（約 40 秒）");
        break;
    case ID_M17TX: {
        if (!need_tx(&out, com)) return;
        wchar_t call[32];
        if (!get_text(ID_CALL, call, 32)) {
            MessageBoxW(g_wnd, L"「コールサイン」を入れてください（試験送信の送信元になります）。", L"taikou", MB_ICONINFORMATION);
            return;
        }
        if (!confirm_tx()) return;
        swprintf(a, 1024, L"m17-tx --out %d --ptt %ls --call %ls", out, com, call);
        static const struct { int id; const wchar_t *opt; } P[] = {
            { ID_HP, L"--radio-hp" }, { ID_ORDER, L"--radio-hp-order" }, { ID_PRE, L"--radio-preemph" },
            { ID_FLOOR, L"--pre-floor" }, { ID_GAIN, L"--pre-gain" }, { ID_LEVEL, L"--level" },
        };
        for (size_t i = 0; i < sizeof(P) / sizeof(P[0]); ++i) {
            if (get_text(P[i].id, v, 64)) {
                size_t l = wcslen(a);
                swprintf(a + l, 1024 - l, L" %ls %ls", P[i].opt, v);
            }
        }
        if (checked(ID_TXEQ)) {
            wchar_t e[MAX_PATH];
            bool refined;
            tx_eq_path(e, MAX_PATH, &refined);
            if (GetFileAttributesW(e) == INVALID_FILE_ATTRIBUTES) {
                MessageBoxW(g_wnd, L"イコライザーのファイル（eq_<記録名>.txt）がまだありません。\n"
                                   L"受信側で「B3 から EQ を作る」にして ④ を受信すると作られます（2 台の PC なら、送信側の PC にコピー）。",
                            L"taikou", MB_ICONINFORMATION);
                return;
            }
            size_t l = wcslen(a);
            swprintf(a + l, 1024 - l, L" --tx-eq \"%ls\"", e);
        }
        run(SL_TX, a, checked(ID_TXEQ) ? L"M17 を送信（B1・B2・B3、B4 = B3 + イコライザー）"
                                       : L"M17 を送信（B1 補正なし、B2 推定値、B3 下の欄の値）");
        break;
    }
    case ID_STOPRX:
        stop(SL_RX);
        break;
    case ID_STOPTX:
        stop(SL_TX);
        break;
    }
}

/* the B3 line of tones-rx / analyze tones / selftest goes into the M17 fields */
static void scan_line(int slot, const wchar_t *s)
{
    static const struct { const wchar_t *key, *hint; } H[] = {
        { L"looks like the 12 kHz IF", L"→ OK：受信側の出力は 12 kHz の IF です。" },
        { L"looks like AUDIO", L"→ 受信側の出力が音声（AF）です。受信側 IC-705 の MENU > SET > Connectors > USB AF/IF Output > "
                               L"Output Select を IF に。入力の番号が受信側の無線機かも確かめてください。" },
        { L"=> unclear", L"→ 判断できません。送信側の PTT を押している間に、もう一度確認してください。" },
        { L"no 1 kHz reference found", L"→ 試験音が届いていません。入力が受信側の無線機か、2 台とも FM-D で同じ周波数か、"
                                       L"受信側の出力が IF か、を確かめてください。" },
        { L"  nothing decoded", L"→ M17 が 1 フレームも受かっていません。まず「② IF の確認」と「③ 試験音」が通るかを確かめてください。" },
        { L"not FM-D", L"→ FM を押してから DATA を押して、FM-D にしてください（この試験は FM-D だけ）。" },
        { L"is not in FM-D", L"→ 送信しませんでした。送信側を FM → DATA で FM-D にしてから、もう一度。" },
        { L"no answer from CI-V", L"→ この COM は返事がありません。別の COM を選ぶか、CI-V の設定（115200 bps、アドレス A4、"
                                  L"USB Echo Back OFF）を確かめてください。" },
        { L"no CI-V answer", L"→ 送信しませんでした。COM が送信側の無線機か、CI-V の設定を確かめてください。" },
        { L"transmit equalizer saved:", L"→ 送信用のイコライザー（eq_<記録名>_tx.txt）を直しました。次の ④ の B4 から使われます。" },
        { L"[!] REFINE", L"→ 送信用のイコライザーを直せませんでした。B4 が録音に入っているか、コールサインを確かめてください。" },
        { L"equalizer saved:", L"→ イコライザーを保存しました。次からは「EQ で受信」で受けられます。送信側で「B4」を付けると、送信にも掛けます。" },
        { L"[!] EQ:", L"→ B3 が録音の中に見つからず、イコライザーを作れませんでした。送信側のコールサインと、④ の送信が録音に入っているかを確かめてください。" },
        { L"[!] LIMITER", L"→ 送信の音が大きすぎて、偏移のリミッターで頭打ちです。上の周波数特性は正しくありません。"
                          L"下の値が「試験音」と M17 の欄に入るので、③ をもう一度。" },
    };
    for (size_t i = 0; i < sizeof(H) / sizeof(H[0]); ++i) {
        if (wcsstr(s, H[i].key)) {
            append(slot, H[i].hint);
            break;
        }
    }
    if (wcsstr(s, L"looks like the 12 kHz IF") && SendMessageW(C(ID_RXAF), BM_GETCHECK, 0, 0) == BST_CHECKED) {
        SendMessageW(C(ID_RXAF), BM_SETCHECK, BST_UNCHECKED, 0);
        append(slot, L"→ 「B の USB 出力が音声（AF）のとき」のチェックを外しました。");
        save_settings();
    }
    double lv, pg;
    const wchar_t *q;
    if ((q = wcsstr(s, L"for tones-tx: --level ")) && swscanf(q, L"for tones-tx: --level %lf", &lv) == 1) {
        wchar_t v[32];
        swprintf(v, 32, L"%.1f", lv);
        SetWindowTextW(C(ID_TLEVEL), v);
        appendf(slot, L"→ 送信側の「試験音」を %.1f dBFS にしました。", lv);
        save_settings();
    }
    if ((q = wcsstr(s, L"for m17-tx (levels): --level ")) &&
        swscanf(q, L"for m17-tx (levels): --level %lf --pre-gain %lf", &lv, &pg) == 2) {
        wchar_t v[32];
        swprintf(v, 32, L"%.1f", lv);
        SetWindowTextW(C(ID_LEVEL), v);
        swprintf(v, 32, L"%g", pg);
        SetWindowTextW(C(ID_GAIN), v);
        appendf(slot, L"→ M17 の「+3 記号」を %.1f dBFS、「補正後」を %g dB にしました。", lv, pg);
        save_settings();
    }
    const wchar_t *p = wcsstr(s, L"for m17-tx (burst B3):");
    if (!p || !(p = wcsstr(p, L"--radio-hp "))) return;
    double hp, pre;
    int ord;
    if (swscanf(p, L"--radio-hp %lf --radio-hp-order %d --radio-preemph %lf", &hp, &ord, &pre) != 3) return;
    wchar_t v[32];
    swprintf(v, 32, L"%g", hp);
    SetWindowTextW(C(ID_HP), v);
    swprintf(v, 32, L"%d", ord);
    SetWindowTextW(C(ID_ORDER), v);
    swprintf(v, 32, L"%g", pre);
    SetWindowTextW(C(ID_PRE), v);
    appendf(slot, L"→ 測った値（低域カット %g Hz・%d 次・プリエンファシス %g µs）を、M17 送信の B3 の欄に入れました。", hp, ord, pre);
    save_settings();
}

/* ---------------------------------------------------------------- window */
static HWND mk(const wchar_t *cls, const wchar_t *txt, DWORD style, int x, int y, int w, int h, int id, HFONT f)
{
    DWORD ex = !wcscmp(cls, L"EDIT") ? WS_EX_CLIENTEDGE : 0;
    HWND c = CreateWindowExW(ex, cls, txt, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), g_wnd,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)(f ? f : g_font), TRUE);
    return c;
}

#define LBL(t, x, y, w) mk(L"STATIC", t, SS_LEFT, x, (y) + 4, w, 20, -1, NULL)
#define BTN(t, x, y, w, id) mk(L"BUTTON", t, BS_PUSHBUTTON | WS_TABSTOP, x, y, w, 28, id, NULL)
#define EDT(x, y, w, id) mk(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, x, y, w, 26, id, NULL)
#define CMB(x, y, w, id) mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, x, y, w, 300, id, NULL)

static void create_controls(void)
{
    /* left: receiver B */
    mk(L"STATIC", L"受信側（B）　USB 出力を IF にした IC-705", SS_LEFT, 12, 12, 560, 22, -1, g_bold);
    LBL(L"入力", 12, 42, 80);
    CMB(92, 42, 380, ID_IN);
    BTN(L"一覧を更新", 480, 41, 100, ID_REFRESH);
    LBL(L"録音", 12, 76, 40);
    EDT(52, 76, 46, ID_SEC);
    LBL(L"秒", 102, 76, 20);
    LBL(L"記録名", 132, 76, 50);
    EDT(184, 76, 50, ID_TAG);
    mk(L"BUTTON", L"B の USB 出力が音声（AF）のとき", BS_AUTOCHECKBOX | WS_TABSTOP, 250, 78, 300, 22, ID_RXAF, NULL);
    BTN(L"② IF の確認", 12, 110, 110, ID_IFCHECK);
    BTN(L"③ 試験音を受信", 128, 110, 130, ID_TONESRX);
    BTN(L"④ M17 を受信", 264, 110, 120, ID_M17RX);
    BTN(L"止める", 390, 110, 90, ID_STOPRX);
    BTN(L"① 自己試験", 12, 144, 110, ID_SELFTEST);
    BTN(L"WAV を解析…", 128, 144, 130, ID_ANALYZE);
    mk(L"BUTTON", L"B3 で EQ を作る", BS_AUTOCHECKBOX | WS_TABSTOP, 266, 147, 128, 22, ID_TRAINEQ, NULL);
    mk(L"BUTTON", L"EQ で受信", BS_AUTOCHECKBOX | WS_TABSTOP, 396, 147, 92, 22, ID_USEEQ, NULL);
    mk(L"BUTTON", L"B4 で直す", BS_AUTOCHECKBOX | WS_TABSTOP, 490, 147, 96, 22, ID_REFINE, NULL);

    /* right: transmitter A */
    mk(L"STATIC", L"送信側（A）　ダミーロード・最小出力", SS_LEFT, 600, 12, 560, 22, -1, g_bold);
    LBL(L"出力", 600, 42, 80);
    CMB(680, 42, 380, ID_OUT);
    LBL(L"CI-V の COM", 600, 76, 80);
    CMB(680, 76, 110, ID_COM);
    BTN(L"無線機を確認", 798, 75, 110, ID_CIV);
    LBL(L"コールサイン", 920, 76, 80);
    EDT(1002, 76, 100, ID_CALL);
    BTN(L"③ 試験音を送信", 600, 110, 130, ID_TONESTX);
    BTN(L"④ M17 を送信", 736, 110, 120, ID_M17TX);
    BTN(L"止める（PTT 切）", 862, 110, 130, ID_STOPTX);
    mk(L"BUTTON", L"B4 も送る（B3 に EQ を掛けて送信）", BS_AUTOCHECKBOX | WS_TABSTOP, 600, 147, 300, 22, ID_TXEQ, NULL);
    LBL(L"試験音", 1000, 110, 48);
    EDT(1048, 110, 50, ID_TLEVEL);
    LBL(L"dBFS", 1102, 110, 40);

    /* M17 B3 precompensation */
    LBL(L"M17 送信 B3 の補正：", 12, 184, 150);
    LBL(L"低域カット", 162, 184, 70);
    EDT(232, 184, 52, ID_HP);
    LBL(L"Hz", 288, 184, 24);
    LBL(L"次数", 318, 184, 34);
    EDT(352, 184, 36, ID_ORDER);
    LBL(L"プリエンファシス", 398, 184, 106);
    EDT(504, 184, 46, ID_PRE);
    LBL(L"µs", 554, 184, 22);
    LBL(L"戻す下限", 584, 184, 60);
    EDT(644, 184, 46, ID_FLOOR);
    LBL(L"Hz", 694, 184, 24);
    LBL(L"補正後", 724, 184, 46);
    EDT(770, 184, 46, ID_GAIN);
    LBL(L"dB", 820, 184, 24);
    LBL(L"+3 記号", 850, 184, 56);
    EDT(906, 184, 46, ID_LEVEL);
    LBL(L"dBFS", 956, 184, 40);
    mk(L"STATIC",
       L"③ の受信が終わると、測った値が自動で入ります。空欄は既定値（補正後 -36 dB、+3 記号 -10 dBFS）。"
       L"　手順：①→②→③（B を先に）→④→役を入れ替えて③④",
       SS_LEFT, 12, 214, 1160, 20, ID_HINT, NULL);

    mk(L"STATIC", L"", SS_LEFT, 12, 242, 560, 20, ID_LBLRX, g_bold);
    mk(L"STATIC", L"", SS_LEFT, 600, 242, 560, 20, ID_LBLTX, g_bold);
    for (int s = 0; s < NSLOT; ++s) {
        g_out[s] = mk(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_HSCROLL,
                      s ? 600 : 12, 266, 560, 400, s ? ID_OUTTX : ID_OUTRX, g_mono);
        SendMessageW(g_out[s], EM_SETLIMITTEXT, 0, 0);
    }
}

static void layout(int cw, int ch)
{
    int x0 = S(12), gap = S(16), top = S(266);
    int w = (cw - 2 * x0 - gap) / 2;
    int h = ch - top - S(12);
    if (h < S(100)) h = S(100);
    MoveWindow(g_out[SL_RX], x0, top, w, h, TRUE);
    MoveWindow(g_out[SL_TX], x0 + w + gap, top, w, h, TRUE);
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_CREATE:
        g_wnd = h;
        create_controls();
        load_settings();
        fill_devices();
        set_buttons();
        append(SL_RX, L"taikou-test1-1 の画面です。受信側（B）の表示。");
        append(SL_RX, L"まず「① 自己試験」で、この PC で動くかを確かめられます（無線機なし）。");
        append(SL_TX, L"送信側（A）の表示。送信の前に「無線機を確認」で、FM-D・データモード ON・低い出力を確かめてください。");
        appendf(SL_TX, L"表示はすべて %ls にも保存します。", wcsrchr(g_log, L'\\') + 1);
        return 0;
    case WM_SIZE:
        layout(LOWORD(lp), HIWORD(lp));
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        mm->ptMinTrackSize.x = S(1200);
        mm->ptMinTrackSize.y = S(560);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        if ((HWND)lp == g_out[0] || (HWND)lp == g_out[1]) {
            SetBkColor((HDC)wp, GetSysColor(COLOR_WINDOW));
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        }
        if (GetDlgCtrlID((HWND)lp) == ID_HINT) {
            SetTextColor((HDC)wp, RGB(90, 100, 117));
            SetBkMode((HDC)wp, TRANSPARENT);
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        break;
    case WM_COMMAND:
        if (HIWORD(wp) == BN_CLICKED) on_button(LOWORD(wp));
        return 0;
    case WM_APP_LINE: {
        wchar_t *s = (wchar_t *)lp;
        append((int)wp, s);
        scan_line((int)wp, s);
        free(s);
        return 0;
    }
    case WM_APP_DONE: {
        int sl = (int)wp;
        slot_t *s = &g_slot[sl];
        KillTimer(h, TIMER_KILL + sl);
        CloseHandle(s->proc);
        CloseHandle(s->rd);
        CloseHandle(s->stop_ev);
        s->running = false;
        appendf(sl, L"=== 終わり（終了コード %d）", (int)lp);
        set_buttons();
        return 0;
    }
    case WM_TIMER:
        if (wp >= TIMER_KILL && wp < TIMER_KILL + NSLOT) {
            int sl = (int)(wp - TIMER_KILL);
            KillTimer(h, wp);
            if (g_slot[sl].running) {
                TerminateProcess(g_slot[sl].proc, 99);
                append(sl, sl == SL_TX ? L"[!] 応答がないので強制終了しました。無線機が送信のままなら、手で PTT を切ってください。"
                                       : L"[!] 応答がないので強制終了しました。");
            }
        }
        return 0;
    case WM_CLOSE:
        if (g_slot[SL_RX].running || g_slot[SL_TX].running) {
            if (MessageBoxW(h, L"実行中です。止めて終了しますか？（送信中なら PTT を切ります）", L"taikou",
                            MB_YESNO | MB_ICONQUESTION) != IDYES)
                return 0;
            for (int s = 0; s < NSLOT; ++s) {
                if (g_slot[s].running) SetEvent(g_slot[s].stop_ev);
            }
            for (int s = 0; s < NSLOT; ++s) {
                if (g_slot[s].running && WaitForSingleObject(g_slot[s].proc, s == SL_TX ? 5000 : 3000) == WAIT_TIMEOUT)
                    TerminateProcess(g_slot[s].proc, 99);
            }
        }
        save_settings();
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        if (g_logf) fclose(g_logf);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE prev, LPSTR cl, int show)
{
    (void)prev;
    (void)cl;
    SetProcessDPIAware();
    HDC dc = GetDC(NULL);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(NULL, dc);

    GetModuleFileNameW(NULL, g_dir, MAX_PATH);
    *wcsrchr(g_dir, L'\\') = 0;
    swprintf(g_exe, MAX_PATH, L"%ls\\taikou-test1-1.exe", g_dir);
    swprintf(g_ini, MAX_PATH, L"%ls\\taikou-gui.ini", g_dir);
    SYSTEMTIME t;
    GetLocalTime(&t);
    swprintf(g_log, MAX_PATH, L"%ls\\taikou_log_%04d%02d%02d.txt", g_dir, t.wYear, t.wMonth, t.wDay);

    g_font = CreateFontW(-S(14), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Yu Gothic UI");
    g_bold = CreateFontW(-S(15), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Yu Gothic UI");
    g_mono = CreateFontW(-S(13), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");

    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wndproc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"taikou_gui";
    RegisterClassW(&wc);
    RECT r = { 0, 0, S(1184), S(800) };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND w = CreateWindowW(L"taikou_gui", L"taikou-test1-1　IC-705 2 台の対向試験（ダミーロード）", WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, hi, NULL);
    ShowWindow(w, show);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        if (!IsDialogMessageW(w, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return 0;
}
