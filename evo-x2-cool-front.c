// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Fabian Storck
//
// evo-x2-cool-front: small window showing what evo-x2-cool does. Reads the controller's shared memory (no network, no files);
// costs nothing while closed. Tiles with the current values, four curves (temperature, power, fan, clock), a strip for the
// situation (GPU / many cores / emergency), the event list, and a tooltip with the values under the mouse.
// Keys 1-4 or the buttons change the time span. The switches on the right turn single functions of the controller on and off
// (written to einstellungen.ini, which the controller re-reads within 5 s). Texts come from sprache-<code>.ini next to the exe.
// An icon next to the clock shows the current values as tooltip and turns red while it is too hot; closing the window only
// hides it there (then nothing is drawn). --leiste starts hidden. A second start brings the running window to the front.
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "evo.h"
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "winhttp.lib")
#define WM_TRAY   (WM_APP + 1)   // mouse on the tray icon
#define WM_ZEIGEN (WM_APP + 2)   // sent by a second start: show the window
#define WM_UPDATE (WM_APP + 3)   // the update check found a newer version
#define RELEASES L"https://github.com/D3rPapts/evo-x2-cool/releases"

static const EvoShared *sh;
static HANDLE map;
static int spanIdx = 1, mouseX = -1, mouseY = -1, dpi = 96;
static const int spans[4] = { 300, 900, 3600, 7200 };
static const wchar_t *spanText[4] = { L"5 min", L"15 min", L"1 h", L"2 h" };
static RECT btn[4], btnThema, btnSprache, btnUpdate, btnZoom;
static int zoom;   // 1 = every chart zooms on the values of the newest quarter of the shown time ([Anzeige] Zoom)
static wchar_t updVer[16];   // newer version on GitHub, empty = none (or check switched off)
static void open_releases(void);
static HFONT fValue, fText, fSmall, fBold;
static wchar_t langs[8][16], langCode[16]; static int nLangs;   // available sprache-<code>.ini next to the exe
static int thema, dark;   // thema: 0 system, 1 light, 2 dark
static COLORREF C_CARD, C_TEXT, C_OK;
static int S(int v);
static void fill(HDC dc, RECT r, COLORREF c, int round);
static int text(HDC dc, HFONT f, int x, int y, COLORREF c, int align, const wchar_t *fmt, ...);
static RECT sw[EVO_NFUNK];                  // switch rectangles
static unsigned swWant; static ULONGLONG swUntil, swErrUntil;   // clicked state shown until the controller has taken it over
static RECT stMinus[3], stPlus[3]; static float stWant[3]; static ULONGLONG stUntil[3];   // target / attention / alarm - / +
static float cur_stufe(int j)   // clicked value until the controller has taken it over
{
    if (GetTickCount64() < stUntil[j]) return stWant[j];
    return !sh ? (j == 0 ? 80.0f : j == 1 ? 90.0f : 95.0f) : j == 0 ? sh->ziel : (float)(j == 1 ? sh->achtung : sh->alarm);
}

// ---------- texts: section [Anzeige] of sprache-<code>.ini (UTF-16), built-in German as fallback ----------
static wchar_t iniFile[MAX_PATH], langFile[MAX_PATH];
static struct { const wchar_t *key; wchar_t val[200]; } lang[128];
static int nLang;

static const wchar_t *T(const wchar_t *key, const wchar_t *def)
{
    for (int i = 0; i < nLang; i++) if (lang[i].key == key) return lang[i].val;
    if (nLang == 128) return def;
    char a[32], b[32];
    lang[nLang].key = key;
    if (!GetPrivateProfileStringW(L"Anzeige", key, L"", lang[nLang].val, 200, langFile)
        || (evo_conv_sig(lang[nLang].val, a, 32), evo_conv_sig(def, b, 32), strcmp(a, b)))
        wcsncpy(lang[nLang].val, def, 199);
    return lang[nLang++].val;
}

static void load_lang(void)
{
    wchar_t exe[MAX_PATH], lf[MAX_PATH];
    GetPrivateProfileStringW(L"Regler", L"Sprache", L"de", langCode, 16, iniFile);
    GetModuleFileNameW(0, exe, MAX_PATH); *wcsrchr(exe, L'\\') = 0;
    swprintf(lf, MAX_PATH, L"%s\\sprache-%s.ini", exe, langCode);
    if (wcscmp(lf, langFile)) { wcscpy(langFile, lf); nLang = 0; }
    if (!nLangs) {   // which languages are there
        WIN32_FIND_DATAW fd; swprintf(lf, MAX_PATH, L"%s\\sprache-*.ini", exe);
        HANDLE h = FindFirstFileW(lf, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do { if (nLangs < 8 && swscanf(fd.cFileName, L"sprache-%15[^.].ini", langs[nLangs]) == 1) nLangs++; } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    wchar_t t[16]; GetPrivateProfileStringW(L"Anzeige", L"Thema", L"system", t, 16, iniFile);
    thema = !wcscmp(t, L"hell") ? 1 : !wcscmp(t, L"dunkel") ? 2 : 0;
    zoom = GetPrivateProfileIntW(L"Anzeige", L"Zoom", 0, iniFile) != 0;
}

// theme and language buttons at the right end of a row; returns their left edge
static int top_buttons(HDC dc, int right, int y)
{
    const wchar_t *tl = thema == 1 ? T(L"thema1", L"☀ Hell") : thema == 2 ? T(L"thema2", L"☾ Dunkel") : T(L"thema0", L"◐ System");
    wchar_t sl[24]; swprintf(sl, 24, L"🌐 %s", langCode); CharUpperW(sl + 3);
    const wchar_t *lb[2] = { sl, tl }; RECT *rb[2] = { &btnSprache, &btnThema };
    for (int i = 0; i < 2; i++) {
        SelectObject(dc, fText); SIZE sz; GetTextExtentPoint32W(dc, lb[i], (int)wcslen(lb[i]), &sz);
        *rb[i] = (RECT){ right - sz.cx - S(20), y, right, y + S(22) };
        fill(dc, *rb[i], C_CARD, S(8));
        text(dc, fText, (rb[i]->left + rb[i]->right) / 2, y + S(3), C_TEXT, 2, L"%s", lb[i]);
        right = rb[i]->left - S(6);
    }
    if (updVer[0]) {   // newer version on GitHub: one more button, opens the release page
        wchar_t ul[48]; swprintf(ul, 48, T(L"upd_knopf", L"⬆ Version %s"), updVer);
        SelectObject(dc, fText); SIZE sz; GetTextExtentPoint32W(dc, ul, (int)wcslen(ul), &sz);
        btnUpdate = (RECT){ right - sz.cx - S(20), y, right, y + S(22) };
        fill(dc, btnUpdate, C_OK, S(8));
        text(dc, fText, (btnUpdate.left + btnUpdate.right) / 2, y + S(3), RGB(255, 255, 255), 2, L"%s", ul);
        right = btnUpdate.left - S(6);
    } else SetRectEmpty(&btnUpdate);
    return right;
}

// ---------- colours: dark or light, chosen in einstellungen.ini [Anzeige] Thema=system|hell|dunkel ----------
static COLORREF C_BG, C_GRID, C_DIM, C_HOT, C_WARM, C_GRAFIK, C_VIELE, C_TIP, C_OFF, C_RAW, C_PKG, C_RPM, C_FAN, C_MHZ, C_CAP;
static const wchar_t *themaKey[3] = { L"system", L"hell", L"dunkel" };

static void apply_theme(HWND w)
{
    DWORD light = 0, n = sizeof light;   // Windows app setting, used for "system"
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD, 0, &light, &n)) light = 0;
    dark = thema == 2 || (thema == 0 && !light);
    if (dark) {
        C_BG = RGB(18, 20, 23); C_CARD = RGB(27, 30, 35); C_GRID = RGB(40, 44, 51); C_TEXT = RGB(230, 232, 235); C_DIM = RGB(139, 146, 156);
        C_HOT = RGB(255, 107, 91); C_WARM = RGB(240, 166, 64); C_OK = RGB(76, 195, 138); C_GRAFIK = RGB(120, 96, 200); C_VIELE = RGB(200, 140, 60);
        C_TIP = RGB(44, 48, 56); C_OFF = RGB(70, 75, 84); C_RAW = RGB(140, 62, 56); C_PKG = RGB(95, 102, 114); C_RPM = RGB(30, 110, 110);
        C_FAN = RGB(46, 196, 196); C_MHZ = RGB(170, 176, 186); C_CAP = RGB(242, 201, 76);
    } else {
        C_BG = RGB(240, 242, 245); C_CARD = RGB(255, 255, 255); C_GRID = RGB(226, 229, 234); C_TEXT = RGB(28, 30, 34); C_DIM = RGB(96, 103, 114);
        C_HOT = RGB(214, 58, 46); C_WARM = RGB(204, 122, 16); C_OK = RGB(28, 150, 88); C_GRAFIK = RGB(140, 112, 220); C_VIELE = RGB(226, 160, 70);
        C_TIP = RGB(250, 250, 252); C_OFF = RGB(196, 201, 208); C_RAW = RGB(232, 160, 152); C_PKG = RGB(160, 166, 176); C_RPM = RGB(120, 190, 190);
        C_FAN = RGB(0, 140, 150); C_MHZ = RGB(150, 156, 166); C_CAP = RGB(196, 140, 0);
    }
    BOOL d = dark;   // title bar to match (Windows 11)
    static HMODULE dwm; if (!dwm) dwm = LoadLibraryW(L"dwmapi.dll");
    typedef HRESULT (WINAPI *DWMSET)(HWND, DWORD, const void *, DWORD);
    if (dwm && w) { DWMSET set = (DWMSET)(void (*)(void))GetProcAddress(dwm, "DwmSetWindowAttribute"); if (set) set(w, 20, &d, sizeof d); }
}
static const wchar_t *lageText(int i) { return i == LAGE_VIELE ? T(L"lage1", L"viele Kerne aktiv") : i == LAGE_GRAFIK ? T(L"lage2", L"Grafik (+ Abkühlen)") : T(L"lage0", L"wenige Kerne aktiv"); }
static const wchar_t *grundText(int i)
{
    switch (i) {
    case GRUND_VORHERSAGE: return T(L"grund1", L"vorausschauend");
    case GRUND_NACHREGELN: return T(L"grund2", L"nachgeregelt");
    case GRUND_NOTFALL: return T(L"grund3", L"NOTFALL");
    case GRUND_NUR_LESEN: return T(L"grund4", L"nur lesen");
    case GRUND_ACHTUNG: return T(L"grund5", L"Achtung, steigt");
    default: return T(L"grund0", L"nach Modell");
    }
}
static const wchar_t *funkText(int i)
{
    switch (i) {
    case 0: return T(L"f0", L"Lüftersteuerung");
    case 1: return T(L"f1", L"Taktbremse");
    case 2: return T(L"f2", L"EC-Leise-Modus");
    case 3: return T(L"f3", L"Lernen");
    default: return T(L"f4", L"Vorhersage");
    }
}

static int S(int v) { return MulDiv(v, dpi, 96); }

static void open_shm(void)
{
    if (sh) { if (sh->magic == EVO_MAGIC) return; UnmapViewOfFile(sh); CloseHandle(map); sh = 0; }
    map = OpenFileMappingW(FILE_MAP_READ, FALSE, EVO_SHM);
    if (map) { sh = MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0); if (!sh) { CloseHandle(map); map = 0; } }
}

static void fill(HDC dc, RECT r, COLORREF c, int round)
{
    HBRUSH b = CreateSolidBrush(c), ob = SelectObject(dc, b); HPEN op = SelectObject(dc, GetStockObject(NULL_PEN));
    if (round) RoundRect(dc, r.left, r.top, r.right + 1, r.bottom + 1, round, round); else FillRect(dc, &r, b);
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(b);
}

// text at x,y (align: 0 left, 1 right edge at x, 2 centred); returns its width
static int text(HDC dc, HFONT f, int x, int y, COLORREF c, int align, const wchar_t *fmt, ...)
{
    wchar_t b[300]; va_list a;
    va_start(a, fmt); int n = _vsnwprintf(b, 299, fmt, a); va_end(a);
    if (n < 0) n = 299;
    SelectObject(dc, f); SetTextColor(dc, c);
    SIZE sz; GetTextExtentPoint32W(dc, b, n, &sz);
    TextOutW(dc, align == 1 ? x - sz.cx : align == 2 ? x - sz.cx / 2 : x, y, b, n);
    return sz.cx;
}

static void line(HDC dc, int x0, int y0, int x1, int y1, COLORREF c, int w, int style)
{
    HPEN p = CreatePen(style, w, c), o = SelectObject(dc, p);
    MoveToEx(dc, x0, y0, 0); LineTo(dc, x1, y1);
    DeleteObject(SelectObject(dc, o));
}

// ---------- series ----------
typedef float (*Getter)(const EvoSample *);
static float gTctl(const EvoSample *s) { return s->tctl; }
static float gGlatt(const EvoSample *s) { return s->tglatt; }
static float gEc(const EvoSample *s) { return (float)s->ecT; }
static float gVorh(const EvoSample *s) { return s->grund == GRUND_NUR_LESEN ? -1 : s->vorhersage; }
static float gCore(const EvoSample *s) { return s->coreW; }
static float gGpu(const EvoSample *s) { return s->gpuW; }
static float gPkg(const EvoSample *s) { return s->pkgW; }
static float gFan(const EvoSample *s) { return (float)s->fan; }
// fan speed as duty %, so the measured line is directly comparable with the target. Calibration on a 64 GB EVO-X2 at
// 0/10/20/30/50/70/100 % (median rpm at a steady duty from the logs; 10 % on CPU fan 1 confirmed on the 128 GB one).
// The system fan stalls below ~15 % and sometimes reads 8000 rpm when it stops: that is no value.
static float rpmPct(int r, const float rpm[7])
{
    static const float pct[7] = { 0, 10, 20, 30, 50, 70, 100 };
    if (r < 0 || r > 6000) return -1;
    for (int i = 1; i < 7; i++) if (r <= rpm[i]) return pct[i - 1] + (r - rpm[i - 1]) / (rpm[i] - rpm[i - 1]) * (pct[i] - pct[i - 1]);
    return 100;
}
static float gRpm1(const EvoSample *s) { static const float r[7] = { 0, 1500, 1850, 2230, 2890, 3450, 4210 }; return rpmPct(s->rpm[0], r); }
static float gRpm2(const EvoSample *s) { static const float r[7] = { 0, 1200, 1670, 2000, 2770, 3160, 4015 }; return rpmPct(s->rpm[1], r); }
static float gRpm3(const EvoSample *s) { static const float r[7] = { 0, 200, 414, 630, 1090, 1281, 1850 }; return rpmPct(s->rpm[2], r); }
static float gEcFan(const EvoSample *s) { return s->fan < 0 || s->ecFan < 0 ? -1 : (float)s->ecFan; }
static float gSysFan(const EvoSample *s) { return (float)s->sysFan; }
static float gSysEc(const EvoSample *s) { return s->sysFan < 0 || s->sysEc < 0 ? -1 : (float)s->sysEc; }
static float gCap(const EvoSample *s) { return s->cap > 0 ? (float)s->cap : -1; }
static float gMhz(const EvoSample *s) { return s->mhz; }

typedef struct { Getter g; COLORREF c; int w; const wchar_t *name; } Series;
typedef struct { const wchar_t *title; float lo, hi, step; Series s[4]; float minW, za, zb; int z; } Chart;   // za..zb = zoomed band

static uint32_t first, count, span;
static int xOf(RECT r, uint32_t i) { return r.left + (int)((int64_t)(r.right - r.left) * (int)(span - (count - 1 - i)) / (int)span); }
static const EvoSample *sample(uint32_t i) { return &sh->s[(first + i) % EVO_N]; }

// height 0..1 of value v in a chart. Zoom: the band za..zb gets the middle 70 % of the height, values below or above it
// are squeezed logarithmically into the 15 % at either edge, so they stay visible without taking the room
static float yfrac(const Chart *c, float v)
{
    float f;
    if (!c->z) f = (v - c->lo) / (c->hi - c->lo);
    else if (v < c->za) f = 0.15f * logf(1 + fmaxf(0, v - c->lo)) / logf(1 + fmaxf(c->za - c->lo, 0.001f));
    else if (v > c->zb) f = 0.85f + 0.15f * logf(1 + (v - c->zb)) / logf(1 + fmaxf(c->hi - c->zb, 0.001f));
    else f = 0.15f + 0.7f * (v - c->za) / (c->zb - c->za);
    return f < 0 ? 0 : f > 1 ? 1 : f;
}

// zoomed band: range of all lines in the newest quarter of the shown samples, at least minW wide, plus a fifth margin
static void zoom_setup(Chart *c)
{
    c->z = 0;
    if (!zoom || count < 8) return;
    float a = 1e9f, b = -1e9f;
    for (uint32_t i = count - count / 4; i < count; i++)
        for (int k = 0; k < 4; k++) {
            if (!c->s[k].g) continue;
            float v = c->s[k].g(sample(i));
            if (v >= 0) { if (v < a) a = v; if (v > b) b = v; }
        }
    if (a > b) return;
    float w = fmaxf(b - a, c->minW) * 1.4f, m = (a + b) / 2;
    c->za = fmaxf(c->lo, m - w / 2); c->zb = fminf(c->hi, m + w / 2);
    c->z = c->zb - c->za < (c->hi - c->lo) * 0.9f;   // nearly the whole range anyway: stay linear
}

static void chart(HDC dc, RECT r, const Chart *c)
{
    // title row with legend on the right
    int x = r.right;
    for (int k = 3; k >= 0; k--) {
        if (!c->s[k].g) continue;
        x -= text(dc, fSmall, x, r.top, C_DIM, 1, L"%s", c->s[k].name) + S(6);
        line(dc, x - S(14), r.top + S(8), x, r.top + S(8), c->s[k].c, S(2), PS_SOLID);
        x -= S(28);
    }
    text(dc, fBold, r.left, r.top - S(1), C_TEXT, 0, L"%s", c->title);
    RECT p = { r.left + S(34), r.top + S(20), r.right, r.bottom };
    fill(dc, p, C_CARD, 0);
    float st = c->step, v0 = c->lo, v1 = c->hi;
    if (c->z) {   // zoomed: round steps inside the band, plus the chart's ends if their labels have room
        float raw = (c->zb - c->za) / 4, p10 = powf(10, floorf(log10f(raw))), q = raw / p10;
        st = (q > 5 ? 10 : q > 2 ? 5 : q > 1 ? 2 : 1) * p10; v0 = ceilf(c->za / st) * st; v1 = c->zb;
    }
    int yLo = 99999, yHi = -99999;
    for (float v = v0; v <= v1 + 0.01f; v += st) {
        int y = p.bottom - (int)(yfrac(c, v) * (p.bottom - p.top - 1)) - 1;
        line(dc, p.left, y, p.right, y, C_GRID, 1, PS_SOLID);
        text(dc, fSmall, p.left - S(6), y - S(7), C_DIM, 1, L"%g", v);
        if (y < yLo) yLo = y; if (y > yHi) yHi = y;
    }
    if (c->z) {
        int yb = p.bottom - 1, yt = p.top;
        line(dc, p.left, yb, p.right, yb, C_GRID, 1, PS_SOLID); line(dc, p.left, yt, p.right, yt, C_GRID, 1, PS_SOLID);
        if (yb - yHi > S(12)) text(dc, fSmall, p.left - S(6), yb - S(7), C_DIM, 1, L"%g", c->lo);
        if (yLo - yt > S(12)) text(dc, fSmall, p.left - S(6), yt - S(7), C_DIM, 1, L"%g", c->hi);
    }
    for (int k = 0; k < 4; k++) {
        if (!c->s[k].g) continue;
        HPEN pen = CreatePen(PS_SOLID, c->s[k].w, c->s[k].c), o = SelectObject(dc, pen);
        int down = 0, lastX = -99999;
        for (uint32_t i = 0; i < count; i++) {
            float v = c->s[k].g(sample(i));
            if (v < 0) { down = 0; continue; }   // -1 = no value
            int px = xOf(p, i);
            if (down && px == lastX && i + 1 < count) continue;   // more samples than pixels: one point per column is enough
            float f = yfrac(c, v);
            int py = p.bottom - 1 - (int)(f * (p.bottom - p.top - 2));
            if (down) LineTo(dc, px, py); else MoveToEx(dc, px, py, 0);
            down = 1; lastX = px;
        }
        DeleteObject(SelectObject(dc, o));
    }
}

static void tile(HDC dc, RECT r, const wchar_t *label, COLORREF vc, const wchar_t *value, const wchar_t *sub)
{
    fill(dc, r, C_CARD, S(10));
    text(dc, fSmall, r.left + S(12), r.top + S(8), C_DIM, 0, L"%s", label);
    text(dc, fValue, r.left + S(12), r.top + S(22), vc, 0, L"%s", value);
    RECT s = { r.left + S(12), r.bottom - S(20), r.right - S(8), r.bottom };   // cut with … if the tile is narrow
    SelectObject(dc, fSmall); SetTextColor(dc, C_DIM);
    DrawTextW(dc, sub, -1, &s, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}

static void paint(HWND w, HDC dc)
{
    RECT rc; GetClientRect(w, &rc);
    fill(dc, rc, C_BG, 0);
    SetBkMode(dc, TRANSPARENT);
    open_shm();
    int pad = S(16);
    if (!sh || sh->nSample == 0) {
        top_buttons(dc, rc.right - pad, pad);
        text(dc, fValue, rc.right / 2, rc.bottom / 2 - S(20), C_TEXT, 2, L"%s", T(L"kein_regler", L"Regler läuft nicht"));
        text(dc, fText, rc.right / 2, rc.bottom / 2 + S(16), C_DIM, 2, L"%s", T(L"kein_regler2", L"evo-x2-cool starten oder den Dienst einrichten"));
        return;
    }
    uint32_t n = sh->nSample;
    span = spans[spanIdx]; count = n < span ? n : span; if (count > EVO_N) count = EVO_N; first = n - count;
    const EvoSample *cur = &sh->s[(n - 1) % EVO_N];
    int stale = (int)(time(0) - cur->t) > 5;

    // ---- layout: tiles on top, charts left, events right ----
    int evW = S(300), tileH = S(80), gap = S(12);
    RECT charts = { pad, pad + tileH + gap + S(30), rc.right - pad - evW - gap, rc.bottom - pad - S(30) };
    RECT plot = { charts.left + S(34), 0, charts.right, 0 };   // x extent of all plots

    // sample under the mouse
    const EvoSample *at = cur; int hover = -1;
    if (mouseX >= plot.left && mouseX < plot.right && mouseY >= charts.top && mouseY < charts.bottom + S(14)) {
        int back = (int)span - 1 - (int)((int64_t)(mouseX - plot.left) * span / (plot.right - plot.left));
        if (back >= 0 && back < (int)count) { at = &sh->s[(n - 1 - back) % EVO_N]; hover = (int)count - 1 - back; }
    }

    // ---- tiles ----
    wchar_t v[6][32], sub[6][80];
    COLORREF tc = at->tctl >= sh->spitze ? C_HOT : at->tglatt > sh->ziel ? C_WARM : C_TEXT;
    // the same order for CPU and GPU: temperature large, power below
    swprintf(v[0], 32, L"%.1f °C", at->tctl);         swprintf(sub[0], 80, T(L"sub_cpu", L"geglättet %.1f (Ziel %.0f) · %.1f W"), at->tglatt, sh->ziel, at->coreW);
    if (at->gpuT >= 0) swprintf(v[1], 32, L"%d °C", at->gpuT); else wcscpy(v[1], L"–");
    swprintf(sub[1], 80, T(L"sub_gpu", L"%.0f W · Last %d %%"), at->gpuW, at->gpuLoad);
    if (at->fan >= 0) swprintf(v[2], 32, L"%d %%", at->fan); else wcscpy(v[2], T(L"bios", L"BIOS"));
    swprintf(sub[2], 80, T(L"sub_fan", L"%d U/min · %s"), at->rpm[0], grundText(at->grund));
    if (at->cap > 0) swprintf(v[3], 32, L"%d MHz", at->cap); else wcscpy(v[3], T(L"cap_aus", L"aus"));
    swprintf(sub[3], 80, T(L"sub_cap", L"Lastart: %s%s"), lageText(at->lage), at->leise ? T(L"leise", L" + EC leise") : L"");
    swprintf(v[4], 32, L"%.0f W", at->pkgW);          swprintf(sub[4], 80, T(L"sub_chip", L"stärkster Kern %.1f W · %d Kerne voll"), at->maxCoreW, at->busy);
    swprintf(v[5], 32, T(L"v_learn", L"%d Bereiche"), sh->felder); swprintf(sub[5], 80, T(L"sub_learn", L"real %+.1f °C wärmer als Formel"), sh->versatz);
    const wchar_t *labels[6] = { T(L"t_cpu", L"CPU"), T(L"t_gpu", L"Grafik"), T(L"t_fan", L"Lüfter"), T(L"t_cap", L"Taktbremse"), T(L"t_chip", L"Chip gesamt"), T(L"t_learn", L"Gelernte Last-Bereiche") };
    int tw = (rc.right - 2 * pad - 5 * gap) / 6;
    for (int i = 0; i < 6; i++) {
        RECT t = { pad + i * (tw + gap), pad, pad + i * (tw + gap) + tw, pad + tileH };
        tile(dc, t, labels[i], i == 0 ? tc : at->grund == GRUND_NOTFALL && i == 2 ? C_HOT : at->grund == GRUND_ACHTUNG && i == 2 ? C_WARM : C_TEXT, v[i], sub[i]);
    }

    // ---- span buttons and status ----
    int by = pad + tileH + gap, bx = pad;
    for (int i = 0; i < 4; i++) {
        SelectObject(dc, fText); SIZE sz; GetTextExtentPoint32W(dc, spanText[i], (int)wcslen(spanText[i]), &sz);
        btn[i] = (RECT){ bx, by, bx + sz.cx + S(20), by + S(22) };
        fill(dc, btn[i], i == spanIdx ? RGB(58, 92, 160) : C_CARD, S(8));
        text(dc, fText, (btn[i].left + btn[i].right) / 2, by + S(3), i == spanIdx ? C_TEXT : C_DIM, 2, L"%s", spanText[i]);
        bx = btn[i].right + S(6);
    }
    const wchar_t *status = stale ? T(L"stale", L"Keine neuen Werte – läuft der Regler?") : sh->nurLesen ? T(L"nurlesen", L"Nur lesen – es wird nichts gesteuert") : 0;
    int sx = top_buttons(dc, charts.right, by) - S(10);
    if (status) text(dc, fSmall, sx, by + S(4), C_HOT, 1, L"%s", status);
    else {
        struct tm lt; time_t tt = (time_t)at->t; localtime_s(&lt, &tt);
        text(dc, fSmall, sx, by + S(4), C_DIM, 1, T(L"status", L"%s%02d:%02d:%02d · Taktbremse gelernt – wenige %d · viele %d · Grafik %d MHz"),
             hover >= 0 ? T(L"maus", L"Mauszeiger ") : L"", lt.tm_hour, lt.tm_min, lt.tm_sec, sh->deckel[0], sh->deckel[1], sh->deckel[2]);
    }

    // ---- charts ----
    float pmax = 40;
    for (uint32_t i = 0; i < count; i++) if (sample(i)->pkgW > pmax && sample(i)->pkgW < 400) pmax = sample(i)->pkgW;   // < 400: old 65000-W misreads (issue #2)
    pmax = (float)((int)(pmax / 40) + 1) * 40;
    // temperature axis grows when a value goes beyond it (one pass over the samples, nothing measurable)
    float tmin = 30, tmax = 100;
    for (uint32_t i = 0; i < count; i++) { const EvoSample *s = sample(i); if (s->tctl > tmax) tmax = s->tctl; if (s->tctl > 0 && s->tctl < tmin) tmin = s->tctl; }
    tmax = (float)((int)((tmax + 9.99f) / 10) * 10); tmin = (float)((int)(tmin / 10) * 10);
    const wchar_t *lIst = T(L"l_ist", L"Ist (aus Drehzahl)"), *lEc = T(L"l_ecfan", L"EC-Kurve (mit Aufschlag)"), *lSoll = T(L"l_soll", L"Soll");
    Chart cs[6] = {
        { T(L"c_temp", L"Temperatur °C"), tmin, tmax, 10, { { gVorh, C_DIM, 1, T(L"l_erwartet", L"erwartet") }, { gEc, C_WARM, 1, T(L"l_ec", L"Gehäuse-Fühler (EC)") },
                                           { gTctl, C_RAW, 1, T(L"l_roh", L"CPU roh") }, { gGlatt, C_HOT, S(2), T(L"l_glatt", L"CPU geglättet (Regelgröße)") } } },
        { T(L"c_power", L"Leistung W"), 0, pmax, pmax / 4, { { gPkg, C_PKG, 1, T(L"l_chip", L"ganzer Chip") }, { gGpu, RGB(165, 124, 255), S(2), T(L"l_gpu", L"Grafik") },
                                              { gCore, RGB(78, 161, 255), S(2), T(L"l_kerne", L"Kerne") } } },
        { T(L"c_clock", L"CPU-Takt MHz"), 0, 6000, 1500, { { gMhz, C_MHZ, 1, T(L"l_kern", L"schnellster Kern (Mittel je Sekunde)") }, { gCap, C_CAP, S(2), T(L"l_bremse", L"Taktbremse") } } },
        // one chart per fan at the bottom, smaller: CPU fans 1 and 2 share the curve (block 0x60), the system fan has its own (0x66)
        { T(L"c_fan1", L"CPU-Lüfter 1 %"), 0, 100, 50, { { gRpm1, C_RPM, 1, lIst }, { gEcFan, C_WARM, 1, lEc }, { gFan, C_FAN, S(2), lSoll } } },
        { T(L"c_fan2", L"CPU-Lüfter 2 %"), 0, 100, 50, { { gRpm2, C_RPM, 1, lIst }, { gEcFan, C_WARM, 1, lEc }, { gFan, C_FAN, S(2), lSoll } } },
        { T(L"c_fan3", L"Systemlüfter (SSD, WLAN) %"), 0, 100, 50, { { gRpm3, C_RPM, 1, lIst }, { gSysEc, C_WARM, 1, lEc }, { gSysFan, C_FAN, S(2), lSoll } } },
    };
    const float minW[6] = { 8, pmax / 6, 800, 15, 15, 15 };   // narrowest zoomed band: °C, W, MHz, %
    for (int i = 0; i < 6; i++) { cs[i].minW = minW[i]; zoom_setup(&cs[i]); }
    static const int weight[6] = { 3, 3, 3, 2, 2, 2 };   // the three fan charts get 2/3 of the height of the others
    int unit = (charts.bottom - charts.top - 5 * gap) / 15, cy = charts.top;
    for (int i = 0; i < 6; i++) {
        RECT r = { charts.left, cy, charts.right, i == 5 ? charts.bottom : cy + weight[i] * unit };
        cy = r.bottom + gap;
        chart(dc, r, &cs[i]);
        if (i == 0) {   // dashed lines: target, clock brake, attention, alarm; labels side by side so they do not overlap
            RECT p = { plot.left, r.top + S(20), plot.right, r.bottom };
            const float lv[4] = { sh->ziel, (float)sh->spitze, (float)sh->achtung, (float)sh->alarm };
            const COLORREF lcol[4] = { C_OK, C_DIM, C_CAP, C_HOT };
            const wchar_t *lname[4] = { T(L"ziel", L"Ziel"), T(L"spitze", L"Taktbremse"), T(L"l_achtung", L"Achtung"), T(L"l_alarm", L"Alarm") };
            int lx = p.left + S(4);
            for (int j = 0; j < 4; j++) {
                int yy = p.bottom - 1 - (int)(yfrac(&cs[0], lv[j]) * (p.bottom - p.top - 2));
                line(dc, p.left, yy, p.right, yy, lcol[j], 1, PS_DASH);
                lx += text(dc, fSmall, lx, yy - S(15), lcol[j], 0, L"%s", lname[j]) + S(10);
            }
        }
    }

    // ---- situation strip and time axis ----
    RECT strip = { plot.left, charts.bottom + S(6), plot.right, charts.bottom + S(12) };
    fill(dc, strip, C_CARD, 0);
    for (uint32_t i = 0; i < count; i++) {
        const EvoSample *s = sample(i);
        COLORREF c = s->grund == GRUND_NOTFALL ? C_HOT : s->grund == GRUND_ACHTUNG ? C_CAP : s->lage == LAGE_GRAFIK ? C_GRAFIK : s->lage == LAGE_VIELE ? C_VIELE : C_CARD;
        if (c == C_CARD) continue;
        int x0 = i ? xOf(strip, i - 1) : xOf(strip, 0) - 1, x1 = xOf(strip, i);
        RECT t = { x0, strip.top, x1 > x0 ? x1 : x0 + 1, strip.bottom };
        fill(dc, t, c, 0);
    }
    static const int tick[4] = { 1, 5, 15, 30 };   // minutes between time labels
    for (int m = (int)span / 60; m >= 0; m -= tick[spanIdx]) {
        int x = plot.right - (int)((int64_t)(plot.right - plot.left) * m * 60 / span);
        text(dc, fSmall, x, strip.bottom + S(3), C_DIM, m * 60 == (int)span ? 0 : m == 0 ? 1 : 2, m == 0 ? T(L"jetzt", L"jetzt") : T(L"minus", L"-%d min"), m);
    }
    int lx = charts.left;
    text(dc, fSmall, lx, strip.top - S(5), C_DIM, 0, L"%s", T(L"lastart", L"Lastart"));

    // ---- hover line and tooltip ----
    if (hover >= 0) {
        int x = xOf(plot, (uint32_t)hover);
        line(dc, x, charts.top + S(20), x, strip.bottom, C_DIM, 1, PS_DOT);
        wchar_t tip[4][80];
        swprintf(tip[0], 80, T(L"tip0", L"CPU %.1f °C (geglättet %.1f) · Gehäuse %d °C"), at->tctl, at->tglatt, at->ecT);
        swprintf(tip[1], 80, T(L"tip1", L"Kerne %.1f W · Grafik %.0f W · Chip %.0f W"), at->coreW, at->gpuW, at->pkgW);
        if (at->fan >= 0) swprintf(tip[2], 80, T(L"tip2", L"Lüfter %d %% %s · %d / %d / %d U/min"), at->fan, grundText(at->grund), at->rpm[0], at->rpm[1], at->rpm[2]);
        else swprintf(tip[2], 80, T(L"tip2b", L"Lüfter BIOS · %d / %d / %d U/min"), at->rpm[0], at->rpm[1], at->rpm[2]);
        swprintf(tip[3], 80, T(L"tip3", L"Bremse %d MHz · Kern %.0f MHz · %s"), at->cap, at->mhz, lageText(at->lage));
        int tw2 = 0; SelectObject(dc, fSmall);
        for (int i = 0; i < 4; i++) { SIZE sz; GetTextExtentPoint32W(dc, tip[i], (int)wcslen(tip[i]), &sz); if (sz.cx > tw2) tw2 = sz.cx; }
        RECT box = { x + S(12), mouseY + S(12), x + S(12) + tw2 + S(20), mouseY + S(12) + 4 * S(16) + S(14) };
        if (box.right > charts.right) OffsetRect(&box, -(box.right - box.left) - S(24), 0);
        if (box.bottom > rc.bottom - pad) OffsetRect(&box, 0, -(box.bottom - box.top) - S(24));
        fill(dc, box, C_TIP, S(8));
        for (int i = 0; i < 4; i++) text(dc, fSmall, box.left + S(10), box.top + S(7) + i * S(16), C_TEXT, 0, L"%s", tip[i]);
    }

    // ---- switches: single functions on/off (a click writes einstellungen.ini) ----
    RECT sc = { rc.right - pad - evW, pad + tileH + gap, rc.right - pad, pad + tileH + gap + S(134) + EVO_NFUNK * S(28) + S(28) };
    fill(dc, sc, C_CARD, S(10));
    text(dc, fBold, sc.left + S(14), sc.top + S(12), C_TEXT, 0, L"%s", T(L"funktionen", L"Funktionen"));
    ULONGLONG now = GetTickCount64();
    unsigned on = now < swUntil ? swWant : sh->funktionen;
    // three temperature steps, 1 °C per click (written to [Regler], the controller takes them over within 5 s):
    // green = target (as quiet as possible below it), yellow = attention (fan climbs), red = alarm (fan full)
    {
        static const wchar_t *stKey[3] = { L"z_ziel", L"z_achtung", L"z_alarm" }, *stDef[3] = { L"Ziel", L"Achtung (Lüfter steigt)", L"Alarm (Lüfter voll)" };
        const COLORREF stC[3] = { C_OK, C_CAP, C_HOT }; COLORREF bc = sh->nurLesen ? C_GRID : C_OFF;
        for (int j = 0; j < 3; j++) {
            int zy = sc.top + S(40) + (2 - j) * S(30);   // red on top, like the temperature scale
            RECT dot = { sc.left + S(14), zy + S(7), sc.left + S(24), zy + S(17) }; fill(dc, dot, stC[j], S(10));
            text(dc, fText, sc.left + S(32), zy + S(2), C_TEXT, 0, L"%s", T(stKey[j], stDef[j]));
            stPlus[j] = (RECT){ sc.right - S(38), zy, sc.right - S(14), zy + S(24) };
            stMinus[j] = (RECT){ stPlus[j].left - S(94), zy, stPlus[j].left - S(70), zy + S(24) };
            fill(dc, stMinus[j], bc, S(8)); fill(dc, stPlus[j], bc, S(8));
            text(dc, fBold, (stMinus[j].left + stMinus[j].right) / 2, zy + S(2), C_TEXT, 2, L"−");
            text(dc, fBold, (stPlus[j].left + stPlus[j].right) / 2, zy + S(2), C_TEXT, 2, L"+");
            text(dc, fBold, (stMinus[j].right + stPlus[j].left) / 2, zy + S(2), now < stUntil[j] ? C_WARM : C_TEXT, 2, L"%.0f °C", cur_stufe(j));
        }
    }
    for (int i = 0; i < EVO_NFUNK; i++) {
        int y = sc.top + S(134) + i * S(28), isOn = on >> i & 1;
        sw[i] = (RECT){ sc.left + S(14), y, sc.left + S(50), y + S(20) };
        fill(dc, sw[i], sh->nurLesen ? C_GRID : isOn ? C_OK : C_OFF, S(20));
        RECT knob = { isOn ? sw[i].right - S(18) : sw[i].left + S(2), y + S(2), isOn ? sw[i].right - S(2) : sw[i].left + S(18), y + S(18) };
        fill(dc, knob, dark ? C_TEXT : C_CARD, S(16));
        text(dc, fText, sw[i].right + S(10), y + S(1), isOn && !sh->nurLesen ? C_TEXT : C_DIM, 0, L"%s", funkText(i));
        sw[i].right = sc.right - S(14);   // the whole row is clickable
    }
    text(dc, fSmall, sc.left + S(14), sc.bottom - S(24), now < swErrUntil ? C_HOT : C_DIM, 0, L"%s",
         now < swErrUntil ? T(L"f_err", L"Keine Schreibrechte für einstellungen.ini")
         : sh->nurLesen ? T(L"f_ro", L"Nur-Lese-Modus: Schalter ohne Wirkung") : T(L"f_hint", L"Notfall (Lüfter auf voll) bleibt immer an"));

    // ---- events, newest first ----
    RECT ev = { rc.right - pad - evW, sc.bottom + gap, rc.right - pad, rc.bottom - pad };
    fill(dc, ev, C_CARD, S(10));
    text(dc, fBold, ev.left + S(14), ev.top + S(12), C_TEXT, 0, L"%s", T(L"ereignisse", L"Ereignisse"));
    int ey = ev.top + S(40);
    for (uint32_t i = 0; i < sh->nEvent && i < EVO_EV && ey < ev.bottom - S(120); i++) {
        const EvoEvent *e = &sh->e[(sh->nEvent - 1 - i) % EVO_EV];
        struct tm et; time_t t2 = (time_t)e->t; localtime_s(&et, &t2);
        char t8[120]; memcpy(t8, e->text, 119); t8[119] = 0;
        wchar_t wt[120]; MultiByteToWideChar(CP_UTF8, 0, t8, -1, wt, 120);
        int bad = e->wichtig;
        text(dc, fSmall, ev.left + S(14), ey, bad ? C_HOT : C_DIM, 0, L"%02d.%02d. %02d:%02d:%02d", et.tm_mday, et.tm_mon + 1, et.tm_hour, et.tm_min, et.tm_sec);
        RECT tr = { ev.left + S(14), ey + S(16), ev.right - S(14), ev.bottom };
        SelectObject(dc, fText); SetTextColor(dc, bad ? C_HOT : C_TEXT);
        int h = DrawTextW(dc, wt, -1, &tr, DT_WORDBREAK | DT_NOPREFIX);
        ey += S(16) + h + S(10);
    }
    // what the colours of the strip under the curves mean
    int lx2 = ev.left + S(14), ly = ev.bottom - S(44);   // two rows: load type, then the warning stages
    text(dc, fSmall, lx2, ly - S(18), C_DIM, 0, L"%s", T(L"streifen", L"Farbstreifen unten = Lastart:"));
    const COLORREF lc[4] = { C_GRAFIK, C_VIELE, C_CAP, C_HOT };
    const wchar_t *ln[4] = { T(L"s_gpu", L"Grafik arbeitet"), T(L"s_viele", L"viele Kerne"), T(L"s_achtung", L"Achtung"), T(L"s_notfall", L"Notfall") };
    for (int i = 0; i < 4; i++) {
        if (i == 2) { lx2 = ev.left + S(14); ly += S(16); }
        RECT q = { lx2, ly + S(3), lx2 + S(10), ly + S(13) }; fill(dc, q, lc[i], 0);
        lx2 += S(16) + text(dc, fSmall, lx2 + S(14), ly, C_DIM, 0, L"%s", ln[i]) + S(10);
    }
    // zoom switch, bottom right
    {
        const wchar_t *zl = T(L"zoom", L"🔍 Zoom");
        SelectObject(dc, fSmall); SIZE sz; GetTextExtentPoint32W(dc, zl, (int)wcslen(zl), &sz);
        btnZoom = (RECT){ ev.right - S(14) - sz.cx - S(16), ly - S(2), ev.right - S(14), ly + S(16) };
        fill(dc, btnZoom, zoom ? RGB(58, 92, 160) : C_GRID, S(8));
        text(dc, fSmall, (btnZoom.left + btnZoom.right) / 2, ly, zoom ? C_TEXT : C_DIM, 2, L"%s", zl);
    }
}

// ---------- switches: shared by the window and the tray menu ----------
static unsigned cur_funk(void) { return GetTickCount64() < swUntil ? swWant : sh ? sh->funktionen : 0; }

static void toggle_funk(HWND w, int i)
{
    if (!sh || sh->nurLesen) return;
    ULONGLONG now = GetTickCount64();
    unsigned nw = cur_funk() ^ (1u << i);
    if (WritePrivateProfileStringW(L"Funktionen", evoFunkKey[i], nw >> i & 1 ? L"1" : L"0", iniFile)) { swWant = nw; swUntil = now + 8000; }
    else swErrUntil = now + 5000;
    InvalidateRect(w, 0, FALSE);
}

// the three steps, each at least 2 °C apart: target 60 °C up to 3 °C below the hot-spot limit (above that the clock
// brake would fight the fan), attention from 70 °C, alarm 80-97 °C (the freezes came at ~100 °C)
static void set_stufe(HWND w, int j, float z)
{
    if (!sh || sh->nurLesen) return;
    ULONGLONG now = GetTickCount64();
    static const wchar_t *key[3] = { L"Ziel", L"Achtung", L"Alarm" };
    float lo = j == 0 ? 60 : j == 1 ? fmaxf(70, cur_stufe(0) + 2) : fmaxf(80, cur_stufe(1) + 2);
    float hi = j == 0 ? fminf((float)sh->spitze - 3, cur_stufe(1) - 2) : j == 1 ? cur_stufe(2) - 2 : 97;
    z = z < lo ? lo : z > hi ? hi : z;
    wchar_t v[8]; swprintf(v, 8, L"%.0f", z);
    if (WritePrivateProfileStringW(L"Regler", key[j], v, iniFile)) { stWant[j] = z; stUntil[j] = now + 8000; }
    else swErrUntil = now + 5000;
    InvalidateRect(w, 0, FALSE);
}
static void set_ziel(HWND w, float z) { set_stufe(w, 0, z); }
static float cur_ziel(void) { return cur_stufe(0); }

// ---------- tray icon ----------
static NOTIFYICONDATAW tray;
static HICON icoNormal, icoHeiss;
static UINT msgTaskbar;   // explorer restarted: add the icon again

static void tray_add(HWND w)
{
    tray.cbSize = sizeof tray; tray.hWnd = w; tray.uID = 1;
    tray.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE; tray.uCallbackMessage = WM_TRAY;
    if (!tray.hIcon) { tray.hIcon = icoNormal; wcscpy(tray.szTip, L"EVO-X2-COOL"); }
    Shell_NotifyIconW(NIM_ADD, &tray);
}

static void tray_update(void)   // once per second; talks to explorer only when something changed
{
    open_shm();
    const EvoSample *c = sh && sh->magic == EVO_MAGIC && sh->nSample ? &sh->s[(sh->nSample - 1) % EVO_N] : 0;
    wchar_t tip[128], l1[48], cap[48]; HICON ico = icoNormal;
    int n = swprintf(tip, 128, L"EVO-X2-COOL\n");
    if (!c || time(0) - c->t > 5) swprintf(tip + n, 128 - n, L"%s", T(L"kein_regler", L"Regler läuft nicht"));
    else {
        if (c->gpuT > 0) swprintf(l1, 48, T(L"t_tip1", L"CPU %.0f °C · Grafik %d °C"), c->tctl, c->gpuT);
        else swprintf(l1, 48, T(L"t_tip1b", L"CPU %.0f °C"), c->tctl);
        if (c->cap) swprintf(cap, 48, T(L"tr_cap", L"Taktbremse %d MHz"), c->cap); else wcscpy(cap, T(L"t_capaus", L"Taktbremse aus"));
        n += swprintf(tip + n, 128 - n, L"%s\n", l1);
        swprintf(tip + n, 128 - n, T(L"t_tip2", L"Lüfter %d %% · %s"), c->fan, cap);
        if (c->grund == GRUND_NOTFALL || c->tctl >= sh->spitze) ico = icoHeiss;
    }
    if (ico != tray.hIcon || wcscmp(tip, tray.szTip)) { tray.hIcon = ico; wcscpy(tray.szTip, tip); Shell_NotifyIconW(NIM_MODIFY, &tray); }
}

static void show_window(HWND w, int show)
{
    if (show) { ShowWindow(w, IsIconic(w) ? SW_RESTORE : SW_SHOW); SetForegroundWindow(w); InvalidateRect(w, 0, FALSE); }
    else { ShowWindow(w, SW_HIDE); SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1); }   // give the drawing memory back
}

// ends the controller as well (service or not): it puts the BIOS fan curve back, keeps the safe clock cap, then exits
static void stop_all(HWND w)
{
    if (MessageBoxW(w, T(L"m_alles_frage", L"Regler beenden? Die Lüfter laufen dann wieder nach der BIOS-Kurve, die Taktbremse bleibt auf dem sicheren Wert."),
                    L"EVO-X2-COOL", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
    HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, EVO_STOPP);
    if (ev) { SetEvent(ev); CloseHandle(ev); }
    else if (GetLastError() == ERROR_ACCESS_DENIED) {   // controller from before this version: only an admin may stop it
        MessageBoxW(w, T(L"m_alles_err", L"Keine Rechte, den Regler zu beenden. In einer Admin-Shell: evo-x2-cool --stopp"), L"EVO-X2-COOL", MB_ICONWARNING);
        return;
    }
    DestroyWindow(w);
}

static void tray_menu(HWND w)
{
    int visible = IsWindowVisible(w) && !IsIconic(w);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"EVO-X2-COOL " EVO_VERSION_W);
    if (updVer[0]) { wchar_t ul[64]; swprintf(ul, 64, T(L"upd_menu", L"⬆ Version %s herunterladen …"), updVer); AppendMenuW(m, MF_STRING, 103, ul); }
    AppendMenuW(m, MF_SEPARATOR, 0, 0);
    AppendMenuW(m, MF_STRING, 100, visible ? T(L"m_verstecken", L"Fenster verstecken") : T(L"m_oeffnen", L"Fenster öffnen"));
    AppendMenuW(m, MF_SEPARATOR, 0, 0);
    unsigned on = cur_funk(), off = !sh || sh->nurLesen ? MF_GRAYED : 0;
    for (int i = 0; i < EVO_NFUNK; i++) AppendMenuW(m, MF_STRING | off | (on >> i & 1 ? MF_CHECKED : 0), 200 + i, funkText(i));
    HMENU zm = CreatePopupMenu();   // target temperature presets
    static const int zv[5] = { 75, 78, 80, 82, 85 };
    for (int i = 0; i < 5; i++) { wchar_t zl[16]; swprintf(zl, 16, L"%d °C", zv[i]); AppendMenuW(zm, MF_STRING | ((int)(cur_ziel() + 0.5f) == zv[i] ? MF_CHECKED : 0), 300 + zv[i], zl); }
    wchar_t zt[48]; swprintf(zt, 48, T(L"z_menu", L"Zieltemperatur (%.0f °C)"), cur_ziel());
    AppendMenuW(m, MF_POPUP | off, (UINT_PTR)zm, zt);
    AppendMenuW(m, MF_SEPARATOR, 0, 0);
    AppendMenuW(m, MF_STRING, 101, T(L"m_ende", L"Beenden (Regler läuft weiter)"));
    AppendMenuW(m, MF_STRING | (sh ? 0 : MF_GRAYED), 102, T(L"m_alles", L"Alles beenden (auch den Regler)"));
    POINT p; GetCursorPos(&p);
    SetForegroundWindow(w);   // otherwise the menu does not close when clicking elsewhere
    int id = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, p.x, p.y, 0, w, 0);
    PostMessageW(w, WM_NULL, 0, 0);
    DestroyMenu(m);
    if (id == 100) show_window(w, !visible);
    else if (id == 101) DestroyWindow(w);
    else if (id == 102) stop_all(w);
    else if (id == 103) open_releases();
    else if (id >= 300) set_ziel(w, (float)(id - 300));
    else if (id >= 200) toggle_funk(w, id - 200);
}

static void set_title(HWND w)
{
    wchar_t cur[100], t[100]; swprintf(t, 100, L"%s %s", T(L"titel", L"EVO-X2-COOL"), EVO_VERSION_W);
    GetWindowTextW(w, cur, 100);
    if (wcscmp(cur, t)) SetWindowTextW(w, t);
}

// ---------- update check: once a day the window asks GitHub for the newest release (prereleases too) and only SHOWS
// a newer version (button, tray menu, one balloon per version). Nothing is downloaded or installed: the setup is
// unsigned and brings a driver, so a person decides. [Anzeige] UpdatePruefen=0 switches it off. The controller service
// itself never goes online. ----------
static int ver_num(const char *s) { int a = 0, b = 0, c = 0; sscanf(s, "%d.%d.%d", &a, &b, &c); return a * 10000 + b * 100 + c; }

static int fetch_latest(char tag[16])
{
    int ok = 0;
    HINTERNET s = WinHttpOpen(L"evo-x2-cool/" EVO_VERSION_W, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0), c = 0, r = 0;
    if (s) { WinHttpSetTimeouts(s, 10000, 10000, 10000, 10000); c = WinHttpConnect(s, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0); }
    if (c) r = WinHttpOpenRequest(c, L"GET", L"/repos/D3rPapts/evo-x2-cool/releases?per_page=1", 0, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (r && WinHttpSendRequest(r, L"Accept: application/vnd.github+json\r\n", (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, 0)) {
        static char buf[8192]; DWORD got = 0, total = 0;   // tag_name comes early in the answer
        while (total < sizeof buf - 1 && WinHttpReadData(r, buf + total, sizeof buf - 1 - total, &got) && got) total += got;
        buf[total] = 0;
        char *p = strstr(buf, "\"tag_name\"");
        if (p && (p = strchr(p + 10, '"')) && sscanf(p + 1, "v%15[0-9.]", tag) == 1) ok = 1;
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    return ok;
}

static char updTag[16];
static DWORD WINAPI update_thread(void *w)
{
    Sleep(60000);   // not during logon
    for (;;) {
        char tag[16];
        if (GetPrivateProfileIntW(L"Anzeige", L"UpdatePruefen", 1, iniFile) && fetch_latest(tag) && ver_num(tag) > ver_num(EVO_VERSION)) {
            memcpy(updTag, tag, sizeof tag); PostMessageW(w, WM_UPDATE, 0, 0);
        }
        Sleep(24 * 3600 * 1000);
    }
}

static void open_releases(void) { ShellExecuteW(0, L"open", RELEASES, 0, 0, SW_SHOWNORMAL); }

static LRESULT CALLBACK wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(w, &ps);
        RECT rc; GetClientRect(w, &rc);
        HDC mem = CreateCompatibleDC(dc); HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom), ob = SelectObject(mem, bmp);
        paint(w, mem);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, ob); DeleteObject(bmp); DeleteDC(mem);
        EndPaint(w, &ps); return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_TIMER: {
        static int ticks;
        if (++ticks % 10 == 0) { load_lang(); apply_theme(w); set_title(w); }   // language or theme changed (also the Windows theme for "system")?
        tray_update();
        if (IsWindowVisible(w) && !IsIconic(w)) InvalidateRect(w, 0, FALSE);
        return 0;
    }
    case WM_MOUSEMOVE: {
        mouseX = (short)LOWORD(lp); mouseY = (short)HIWORD(lp);
        TRACKMOUSEEVENT t = { sizeof t, TME_LEAVE, w }; TrackMouseEvent(&t);
        InvalidateRect(w, 0, FALSE); return 0;
    }
    case WM_MOUSELEAVE: mouseX = mouseY = -1; InvalidateRect(w, 0, FALSE); return 0;
    case WM_LBUTTONDOWN: {
        POINT p = { (short)LOWORD(lp), (short)HIWORD(lp) };
        for (int i = 0; i < 4; i++) if (PtInRect(&btn[i], p)) { spanIdx = i; InvalidateRect(w, 0, FALSE); }
        if (PtInRect(&btnUpdate, p)) open_releases();
        if (PtInRect(&btnZoom, p)) { zoom = !zoom; WritePrivateProfileStringW(L"Anzeige", L"Zoom", zoom ? L"1" : L"0", iniFile); InvalidateRect(w, 0, FALSE); }
        if (PtInRect(&btnThema, p) || PtInRect(&btnSprache, p)) {
            int ok;
            if (PtInRect(&btnThema, p)) { thema = (thema + 1) % 3; ok = WritePrivateProfileStringW(L"Anzeige", L"Thema", themaKey[thema], iniFile); apply_theme(w); }
            else {   // next language; the controller picks it up for its events within 5 s
                int cur = 0; for (int i = 0; i < nLangs; i++) if (!_wcsicmp(langs[i], langCode)) cur = i;
                ok = nLangs < 2 || WritePrivateProfileStringW(L"Regler", L"Sprache", langs[(cur + 1) % nLangs], iniFile);
            }
            if (!ok) swErrUntil = GetTickCount64() + 5000;
            load_lang(); set_title(w); InvalidateRect(w, 0, FALSE);
        }
        for (int i = 0; i < EVO_NFUNK; i++) if (PtInRect(&sw[i], p)) toggle_funk(w, i);
        for (int j = 0; j < 3; j++) {
            if (PtInRect(&stMinus[j], p)) set_stufe(w, j, cur_stufe(j) - 1);
            if (PtInRect(&stPlus[j], p)) set_stufe(w, j, cur_stufe(j) + 1);
        }
        return 0;
    }
    case WM_KEYDOWN: if (wp >= '1' && wp <= '4') { spanIdx = (int)(wp - '1'); InvalidateRect(w, 0, FALSE); } return 0;
    case WM_DPICHANGED: {   // moved to a screen with another scale: new fonts, size suggested by Windows
        RECT *r = (RECT *)lp; dpi = HIWORD(wp);
        SendMessageW(w, WM_APP, 0, 0);
        SetWindowPos(w, 0, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_APP: {
        HFONT *fs[4] = { &fValue, &fText, &fSmall, &fBold };
        static const int size[4] = { 24, 13, 12, 13 }, weight[4] = { FW_SEMIBOLD, FW_NORMAL, FW_NORMAL, FW_SEMIBOLD };
        for (int i = 0; i < 4; i++) {
            if (*fs[i]) DeleteObject(*fs[i]);
            *fs[i] = CreateFontW(-S(size[i]), 0, 0, 0, weight[i], 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        }
        return 0;
    }
    case WM_TRAY:
        if (lp == WM_LBUTTONUP) show_window(w, !IsWindowVisible(w) || IsIconic(w));
        else if (lp == WM_RBUTTONUP) tray_menu(w);
        return 0;
    case WM_ZEIGEN: show_window(w, 1); return 0;
    case WM_UPDATE: {   // newer version: button and menu entry; a balloon only once per version
        wchar_t was[16];
        MultiByteToWideChar(CP_UTF8, 0, updTag, -1, updVer, 16);
        GetPrivateProfileStringW(L"Anzeige", L"UpdateGemeldet", L"", was, 16, iniFile);
        if (wcscmp(was, updVer)) {
            NOTIFYICONDATAW n = tray; n.uFlags = NIF_INFO; n.dwInfoFlags = NIIF_INFO;
            wcscpy(n.szInfoTitle, L"EVO-X2-COOL");
            swprintf(n.szInfo, 256, T(L"upd_info", L"Version %s ist verfügbar. Rechtsklick auf das Symbol → herunterladen."), updVer);
            Shell_NotifyIconW(NIM_MODIFY, &n);
            WritePrivateProfileStringW(L"Anzeige", L"UpdateGemeldet", updVer, iniFile);
        }
        InvalidateRect(w, 0, FALSE); return 0;
    }
    case WM_CLOSE: show_window(w, 0); return 0;   // only hide; "Beenden" in the tray menu really ends
    case WM_DESTROY: Shell_NotifyIconW(NIM_DELETE, &tray); PostQuitMessage(0); return 0;
    }
    if (m == msgTaskbar && m) { tray_add(w); return 0; }
    return DefWindowProcW(w, m, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    (void)prev;
    HWND old = FindWindowW(L"evo-x2-cool-front", 0);   // already running: bring it to the front instead of a second copy
    if (old) { DWORD pid; GetWindowThreadProcessId(old, &pid); AllowSetForegroundWindow(pid); PostMessageW(old, WM_ZEIGEN, 0, 0); return 0; }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    dpi = (int)GetDpiForSystem();
    ExpandEnvironmentStringsW(EVO_INI, iniFile, MAX_PATH);
    load_lang();
    WNDCLASSW wc = { 0, wndproc, 0, 0, inst, LoadIconW(inst, MAKEINTRESOURCEW(1)), LoadCursorW(0, IDC_ARROW), 0, 0, L"evo-x2-cool-front" };
    RegisterClassW(&wc);
    HWND w = CreateWindowW(L"evo-x2-cool-front", T(L"titel", L"EVO-X2-COOL"), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, S(1280), S(860), 0, 0, inst, 0);
    dpi = (int)GetDpiForWindow(w);
    set_title(w);
    SendMessageW(w, WM_APP, 0, 0);
    apply_theme(w);
    int cx = GetSystemMetrics(SM_CXSMICON), cy = GetSystemMetrics(SM_CYSMICON);
    icoNormal = LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, cx, cy, 0);
    icoHeiss = LoadImageW(inst, MAKEINTRESOURCEW(2), IMAGE_ICON, cx, cy, 0);
    msgTaskbar = RegisterWindowMessageW(L"TaskbarCreated");
    UINT allow[3] = { msgTaskbar, WM_TRAY, WM_ZEIGEN };   // also when started as admin, explorer and a second start may talk to us
    for (int i = 0; i < 3; i++) ChangeWindowMessageFilterEx(w, allow[i], MSGFLT_ALLOW, 0);
    tray_add(w);
    if (!(cmd && wcsstr(cmd, L"--leiste"))) ShowWindow(w, show);
    tray_update();
    CloseHandle(CreateThread(0, 0, update_thread, w, 0, 0));
    SetTimer(w, 1, 1000, 0);
    MSG msg;
    while (GetMessageW(&msg, 0, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}
