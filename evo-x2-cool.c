// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Fabian Storck
//
// evo-x2-cool: learning fan and clock controller for the GMKtec EVO-X2 (Sixunited AXB35, Ryzen AI MAX+ 395, EC IT5570 1.10).
// Grew out of a PowerShell prototype (evo-luefter-lernend.ps1). Once per second it
//  - measures Tctl (SMN), per-core and package power (RAPL MSRs), highest effective clock (APERF), EC temperature and
//    fan speeds (PawnIO), GPU power/load/temperature (ADL), CPU load per core, and keeps a smoothed temperature;
//  - learns in a table (heat power x fan duty) how warm it really gets, compared to a physical prior, and predicts
//    rising power ahead, then picks the quietest duty whose predicted end temperature stays below the target;
//  - steers the fan through the curve INSIDE the EC (0x60-0x6B, rising above today's temperature), so if this program
//    dies the EC keeps regulating with a rising curve;
//  - caps the CPU clock (power plan PROCFREQMAX) per situation (few cores / many cores / GPU load and its after-run),
//    drops to the lowest step for 30 s after a hot spot and learns per situation which step is safe;
//  - many cores busy and the GPU idle: EC power mode "leise" (54 W).
// On stop: model saved, BIOS fan values (from the UEFI Setup variable) written back, cap left at the safe value,
// EC power mode restored. Data: %ProgramData%\evo-x2-cool (einstellungen.ini, modell.txt, regler-<date>.csv, ereignisse.txt).
// Settings and the on/off switches of the single functions live in einstellungen.ini (re-read within 5 s when it changes),
// texts in sprache-<code>.ini next to the exe.
//
//   evo-x2-cool                      run in this console (Ctrl+C stops and restores)
//   evo-x2-cool --nur-lesen          only measure and show, write nothing to the hardware
//   evo-x2-cool --installieren [--nur-lesen]   register as Windows service (auto start, restart on failure) and start it
//   evo-x2-cool --entfernen          stop and remove the service (restores everything on the way out)
//   evo-x2-cool --stopp              end a run started outside the service, also with the restore
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <powrprof.h>
#include <sddl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "evo.h"
#pragma comment(lib, "powrprof.lib")
#pragma comment(lib, "advapi32.lib")

#define PIO_LOAD 0xA1B22084u   // (41394 << 16) | (0x821 << 2)
#define PIO_EXEC 0xA1B22104u   // (41394 << 16) | (0x841 << 2)
#define MAXCPU 64
#define SVC L"evo-x2-cool"

// ---------- settings (einstellungen.ini) ----------
static float ziel = 80; static int minP = 30, maxP = 70, spitze = 88, sicherMhz = 3600, nurLesen;
static unsigned fn = F_ALLE;   // functions switched on
static const int stufen[] = { 3300, 3600, 3900, 4200, 4500 };   // clock cap steps, MHz; the first one is taken after a hot spot
#define NSTUFEN 5
static const float tau = 40;   // thermal time constant of the heatsink, s

static HANDLE hCpu, hEc, mPci, mEc, stopEv;
static EvoShared *sh;
static wchar_t dataDir[MAX_PATH], exeDir[MAX_PATH], iniFile[MAX_PATH], langFile[MAX_PATH];

// ---------- language file: section [Regler] of sprache-<code>.ini (UTF-16), built-in German as fallback ----------
static struct { const char *key; char val[256]; } lang[64];
static int nLang;

static const char *L_(const char *key, const char *def)
{
    for (int i = 0; i < nLang; i++) if (lang[i].key == key) return lang[i].val;
    if (nLang == 64) return def;
    wchar_t wkey[64], wdef[256], buf[256]; char a[32], b[32];
    MultiByteToWideChar(CP_UTF8, 0, key, -1, wkey, 64);
    MultiByteToWideChar(CP_UTF8, 0, def, -1, wdef, 256);
    lang[nLang].key = key;
    if (GetPrivateProfileStringW(L"Regler", wkey, L"", buf, 256, langFile) > 0
        && (evo_conv_sig(buf, a, 32), evo_conv_sig(wdef, b, 32), !strcmp(a, b)))
        WideCharToMultiByte(CP_UTF8, 0, buf, -1, lang[nLang].val, 256, 0, 0);
    else strncpy(lang[nLang].val, def, 255);
    return lang[nLang++].val;
}

// reads einstellungen.ini (creates it with the defaults first; interactive users may change it, so the switches in the
// viewer work without admin rights). Returns the previous function bits.
static unsigned load_settings(void)
{
    if (GetFileAttributesW(iniFile) == INVALID_FILE_ATTRIBUTES) {
        static const char def[] =
            "; evo-x2-cool settings, re-read within 5 s after a change\r\n"
            "[Regler]\r\nZiel=80\r\nMinProzent=30\r\nMaxProzent=70\r\nSpitze=88\r\nSicherMHz=3600\r\nSprache=de\r\n\r\n"
            "; 1 = on, 0 = off. The emergency (fan 100 % when overheating) always stays on.\r\n"
            "[Funktionen]\r\nLuefter=1\r\nTaktbremse=1\r\nEcLeise=1\r\nLernen=1\r\nVorhersage=1\r\n\r\n"
            "; window: Thema = system / hell / dunkel; UpdatePruefen = 1: ask GitHub once a day for a newer version (only shown, nothing installed)\r\n"
            "[Anzeige]\r\nThema=system\r\nUpdatePruefen=1\r\n";
        SECURITY_ATTRIBUTES sa = { sizeof sa }; DWORD w;
        ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;FA;;;SY)(A;;FA;;;BA)(A;;FRFW;;;IU)", SDDL_REVISION_1, &sa.lpSecurityDescriptor, 0);
        HANDLE f = CreateFileW(iniFile, GENERIC_WRITE, 0, &sa, CREATE_NEW, 0, 0);
        if (f != INVALID_HANDLE_VALUE) { WriteFile(f, def, sizeof def - 1, &w, 0); CloseHandle(f); }
        LocalFree(sa.lpSecurityDescriptor);
    }
    wchar_t buf[16], code[16];
    GetPrivateProfileStringW(L"Regler", L"Ziel", L"80", buf, 16, iniFile); ziel = (float)_wtof(buf);
    minP = GetPrivateProfileIntW(L"Regler", L"MinProzent", 30, iniFile);
    maxP = GetPrivateProfileIntW(L"Regler", L"MaxProzent", 70, iniFile);
    spitze = GetPrivateProfileIntW(L"Regler", L"Spitze", 88, iniFile);
    sicherMhz = GetPrivateProfileIntW(L"Regler", L"SicherMHz", 3600, iniFile);
    // sane limits, whatever is in the file
    ziel = ziel < 50 ? 50 : ziel > 92 ? 92 : ziel; minP = minP < 0 ? 0 : minP > 100 ? 100 : minP;
    maxP = maxP < minP ? minP : maxP > 100 ? 100 : maxP; spitze = spitze < 70 ? 70 : spitze > 95 ? 95 : spitze;
    GetPrivateProfileStringW(L"Regler", L"Sprache", L"de", code, 16, iniFile);
    wchar_t lf[MAX_PATH]; swprintf(lf, MAX_PATH, L"%s\\sprache-%s.ini", exeDir, code);
    if (wcscmp(lf, langFile)) { wcscpy(langFile, lf); nLang = 0; }
    unsigned old = fn; fn = 0;
    for (int i = 0; i < EVO_NFUNK; i++) if (GetPrivateProfileIntW(L"Funktionen", evoFunkKey[i], 1, iniFile)) fn |= 1u << i;
    if (sh) { sh->ziel = ziel; sh->minP = minP; sh->maxP = maxP; sh->spitze = spitze; sh->funktionen = nurLesen ? 0 : fn; }
    return old;
}

static FILETIME ini_time(void)
{
    WIN32_FILE_ATTRIBUTE_DATA a = { 0 };
    GetFileAttributesExW(iniFile, GetFileExInfoStandard, &a);
    return a.ftLastWriteTime;
}

// ---------- PawnIO ----------
static HANDLE pio_load(const wchar_t *dir, const wchar_t *name)
{
    wchar_t p[MAX_PATH];
    swprintf(p, MAX_PATH, L"%s\\pawnio\\%s.bin", dir, name);
    HANDLE f = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
    if (f == INVALID_HANDLE_VALUE) return 0;
    DWORD n = GetFileSize(f, 0), got = 0;
    void *bin = HeapAlloc(GetProcessHeap(), 0, n);
    BOOL ok = ReadFile(f, bin, n, &got, 0) && got == n;
    CloseHandle(f);
    HANDLE h = ok ? CreateFileW(L"\\\\?\\GLOBALROOT\\Device\\PawnIO", GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0) : INVALID_HANDLE_VALUE;
    if (h != INVALID_HANDLE_VALUE && !DeviceIoControl(h, PIO_LOAD, bin, n, 0, 0, &got, 0)) { CloseHandle(h); h = INVALID_HANDLE_VALUE; }
    HeapFree(GetProcessHeap(), 0, bin);
    return h == INVALID_HANDLE_VALUE ? 0 : h;
}

static int pio_exec(HANDLE h, const char *func, const int64_t *in, int nin, int64_t *out, int nout)
{
    uint8_t buf[32 + 8 * 2] = { 0 };
    DWORD got = 0;
    memcpy(buf, func, strlen(func));
    if (nin) memcpy(buf + 32, in, 8 * nin);
    return DeviceIoControl(h, PIO_EXEC, buf, 32 + 8 * nin, out, 8 * nout, &got, 0) && got >= 8u * nout;
}

static int msr(uint32_t idx, uint64_t *v) { int64_t a = idx; return pio_exec(hCpu, "ioctl_read_msr", &a, 1, (int64_t *)v, 1); }

// ---------- EC (ACPI protocol on ports 0x62/0x66) ----------
static uint8_t pin(uint8_t port) { int64_t a = port, r = 0; pio_exec(hEc, "ioctl_pio_read", &a, 1, &r, 1); return (uint8_t)r; }
static void pout(uint8_t port, uint8_t v) { int64_t a[2] = { port, v }; pio_exec(hEc, "ioctl_pio_write", a, 2, 0, 0); }
// Must spin: sleeping between the handshake steps made every read fail in a test (likely Windows' own ACPI EC
// driver cutting in; it does not take our mutex). Each byte costs ~1 ms of spinning, so few bytes are read.
static int ec_wait(uint8_t mask, uint8_t want) { for (int i = 0; i < 20000; i++) if ((pin(0x66) & mask) == want) return 1; return 0; }

static int ec_read_raw(uint8_t reg)
{
    if (pin(0x66) & 1) pin(0x62);   // drop a stale byte
    if (!ec_wait(2, 0)) return -1; pout(0x66, 0x80);
    if (!ec_wait(2, 0)) return -1; pout(0x62, reg);
    if (!ec_wait(1, 1)) return -1; return pin(0x62);
}

static int ec_lock(void) { return WaitForSingleObject(mEc, 500) <= WAIT_ABANDONED; }

static int ec_read(uint8_t reg)
{
    for (int t = 0; t < 3; t++) {
        if (ec_lock()) { int v = ec_read_raw(reg); ReleaseMutex(mEc); if (v >= 0) return v; }
        Sleep(20);
    }
    return -1;
}

// write with read-back; never called in read-only mode
static int ec_write(uint8_t reg, uint8_t val)
{
    if (nurLesen) return 0;
    for (int t = 0; t < 5; t++) {
        if (ec_lock()) {
            int ok = ec_wait(2, 0); if (ok) pout(0x66, 0x81);
            ok = ok && ec_wait(2, 0); if (ok) pout(0x62, reg);
            ok = ok && ec_wait(2, 0); if (ok) pout(0x62, val);
            ok = ok && ec_wait(2, 0) && ec_read_raw(reg) == val;
            ReleaseMutex(mEc);
            if (ok) return 1;
        }
        Sleep(20);
    }
    return 0;
}

// ---------- GPU via ADL ----------
typedef void *(__stdcall *ADL_ALLOC)(int);
typedef struct { int supported, value; } AdlSensor;
typedef struct { int size; AdlSensor s[256]; } AdlPmLog;
typedef int (*ADL2_CREATE)(ADL_ALLOC, int, void **);
typedef int (*ADL2_COUNT)(void *, int *);
typedef int (*ADL2_PMLOG)(void *, int, AdlPmLog *);
static void *__stdcall adl_alloc(int n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n); }
static void *adlCtx; static ADL2_PMLOG adlPm; static int adlIdx = -1;
enum { PM_GFX_ACTIVITY = 19, PM_ASIC_POWER = 23, PM_TEMP_GFX = 28, PM_GFX_POWER = 30 };

static void adl_init(void)
{
    HMODULE m = LoadLibraryW(L"atiadlxx.dll");
    if (!m) return;
    ADL2_CREATE create = (ADL2_CREATE)GetProcAddress(m, "ADL2_Main_Control_Create");
    ADL2_COUNT count = (ADL2_COUNT)GetProcAddress(m, "ADL2_Adapter_NumberOfAdapters_Get");
    adlPm = (ADL2_PMLOG)GetProcAddress(m, "ADL2_New_QueryPMLogData_Get");
    int n = 0;
    if (!create || !count || !adlPm || create(adl_alloc, 1, &adlCtx) != 0 || count(adlCtx, &n) != 0) return;
    static AdlPmLog log;
    for (int i = 0; i < n && adlIdx < 0; i++)
        if (adlPm(adlCtx, i, &log) == 0 && log.s[PM_GFX_POWER].supported) adlIdx = i;
}

// ---------- CPU topology and load ----------
static int nThreads, coreOf[MAXCPU], firstOfCore[MAXCPU], nCores, threadsOfCore[MAXCPU];

static void topology(void)
{
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, 0, &len);
    uint8_t *buf = HeapAlloc(GetProcessHeap(), 0, len), *p = buf;
    GetLogicalProcessorInformationEx(RelationProcessorCore, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf, &len);
    while (p < buf + len) {
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX e = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)p;
        KAFFINITY mask = e->Processor.GroupMask[0].Mask;
        int first = 1;
        for (int b = 0; b < MAXCPU; b++) if (mask >> b & 1) {
            coreOf[b] = nCores; firstOfCore[b] = first; first = 0; threadsOfCore[nCores]++;
            if (b + 1 > nThreads) nThreads = b + 1;
        }
        nCores++; p += e->Size;
    }
    HeapFree(GetProcessHeap(), 0, buf);
}

typedef struct { LARGE_INTEGER idle, kernel, user, r1, r2; ULONG ints; } ProcPerf;
typedef LONG (WINAPI *NTQSI)(int, void *, ULONG, ULONG *);

// ---------- clock cap (power plan) ----------
static const GUID freqMax = { 0x75b0ae3f, 0xbce0, 0x45a7, { 0x8c, 0x89, 0xc9, 0x61, 0x1c, 0x25, 0xe1, 0x00 } };
static int read_cap(void)
{
    GUID *scheme = 0; DWORD v = 0;
    if (PowerGetActiveScheme(0, &scheme) == 0) { PowerReadACValueIndex(0, scheme, &GUID_PROCESSOR_SETTINGS_SUBGROUP, &freqMax, &v); LocalFree(scheme); }
    return (int)v;
}
static int write_cap(int mhz)
{
    GUID *scheme = 0; int ok = 0;
    if (nurLesen) return 0;
    if (PowerGetActiveScheme(0, &scheme) == 0) {
        ok = PowerWriteACValueIndex(0, scheme, &GUID_PROCESSOR_SETTINGS_SUBGROUP, &freqMax, mhz) == 0 && PowerSetActiveScheme(0, scheme) == 0;
        LocalFree(scheme);
    }
    return ok;
}

// ---------- files, events ----------
static HANDLE csv; static int csvDay = -1;

static void append(const wchar_t *name, const char *text)
{
    wchar_t p[MAX_PATH]; DWORD w;
    swprintf(p, MAX_PATH, L"%s\\%s", dataDir, name);
    HANDLE f = CreateFileW(p, FILE_APPEND_DATA, FILE_SHARE_READ, 0, OPEN_ALWAYS, 0, 0);
    if (f != INVALID_HANDLE_VALUE) { WriteFile(f, text, (DWORD)strlen(text), &w, 0); CloseHandle(f); }
}

#define event(...) event2(0, __VA_ARGS__)
static void event2(int wichtig, const char *fmt, ...)
{
    char text[119], line[160]; va_list a;
    va_start(a, fmt); vsnprintf(text, sizeof text, fmt, a); va_end(a);
    SYSTEMTIME st; GetLocalTime(&st);
    snprintf(line, sizeof line, "%04d-%02d-%02d %02d:%02d:%02d  %s\r\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, text);
    append(L"ereignisse.txt", line);
    if (sh) { EvoEvent *e = &sh->e[sh->nEvent % EVO_EV]; e->t = time(0); e->wichtig = (uint8_t)wichtig; memcpy(e->text, text, sizeof text); sh->nEvent++; }
    if (GetConsoleWindow()) printf("\n%s", line);
}

static void csv_line(const char *line)
{
    SYSTEMTIME st; GetLocalTime(&st); DWORD w;
    if (st.wDay != csvDay) {
        if (csv) CloseHandle(csv);
        wchar_t p[MAX_PATH];
        swprintf(p, MAX_PATH, L"%s\\regler-%04d-%02d-%02d.csv", dataDir, st.wYear, st.wMonth, st.wDay);
        // write-through: the last seconds before a freeze must reach the disk
        csv = CreateFileW(p, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_DELETE, 0, OPEN_ALWAYS, FILE_FLAG_WRITE_THROUGH, 0);
        if (csv == INVALID_HANDLE_VALUE) { csv = 0; return; }
        csvDay = st.wDay;
        if (GetFileSize(csv, 0) == 0) {
            static const char h[] = "Zeit;Tctl;Tctl_glatt;EC_C;Paket_W;Kerne_W;Kern_max_W;GPU_W;APU_W;Waerme_W;P_vorher;Takt_max_MHz;CPU_Last;"
                                    "Kerne_aktiv;GPU_Last;GPU_C;Prozent;Grund;L1;L2;Sys;Vorhersage_C;Versatz;Felder;Deckel_MHz;Lage;Modus\r\n";
            WriteFile(csv, h, sizeof h - 1, &w, 0);
        }
    }
    if (csv) WriteFile(csv, line, (DWORD)strlen(line), &w, 0);
}

// ---------- learned model ----------
// A table over heat power (10-W steps) x fan duty (10-% steps); each cell keeps how much warmer or cooler it really got
// than the prior. Prior, fitted on the logs of two EVO-X2 (64 and 128 GB):
//   end temperature = 34 + P * (0.58 - 0.30 * duty / 100) + 6 * M
// P = power of the whole chip (ADL ASIC power, else the package counter), M = power of the hottest core: a hot spot inside
// the chip that adds about 6 °C per watt whatever the fan does. (Up to 0.2.0 P was 1.1 * cores + 0.6 * GPU; under GPU
// inference that saw ~20-30 W of the real ~85 W, because memory and fabric did not count, issue #3.)
// Cells without enough experience borrow the nearest learned duty at the same power, then one overall offset.
#define SKALA 2   // model scale in modell.txt; cells learned on another scale are dropped
#define NP 40
#define ND 11
static struct { float r; int n; } cell[NP][ND];
static float versatz;
static const char *lageName[3] = { "wenige", "viele", "grafik" };   // keys in modell.txt
static const char *funk_text(int i)
{
    switch (i) {
    case 0: return L_("f0", "Lüftersteuerung");
    case 1: return L_("f1", "Taktbremse");
    case 2: return L_("f2", "EC-Leise-Modus");
    case 3: return L_("f3", "Lernen");
    default: return L_("f4", "Vorhersage");
    }
}
static const char *lage_text(int i) { return i == LAGE_WENIGE ? L_("lage0", "wenige Kerne aktiv") : i == LAGE_VIELE ? L_("lage1", "viele Kerne aktiv") : L_("lage2", "Grafik arbeitet (oder kühlt danach ab)"); }
// per situation: current cap step, calm seconds earned at that cap, highest temperature since, and how many hot spots
// each step has caused. A step that caused hot spots is tried again only after a longer calm run: 10 min, 1 h, 1 day.
static struct { int stufe, ruhig, spitze, fehl[NSTUFEN]; } lage[3];
static int wartezeit(int fehl) { return fehl <= 0 ? 600 : fehl == 1 ? 3600 : 86400; }

static int pk(float p) { int k = (int)lroundf(p / 10); return k < 0 ? 0 : k >= NP ? NP - 1 : k; }
static int dk(float d) { int k = (int)lroundf(d / 10); return k < 0 ? 0 : k >= ND ? ND - 1 : k; }
static float prior(float p, float d, float m) { return 34 + p * (0.58f - 0.30f * d / 100) + 6 * m; }

static float predict(float p, float d, float m)
{
    int a = pk(p), b = dk(d);
    if (cell[a][b].n >= 20) return prior(p, d, m) + cell[a][b].r;
    int best = -1; float bd = 1e9f;
    for (int j = 0; j < ND; j++) if (cell[a][j].n >= 20 && fabsf(j * 10 - d) < bd) { bd = fabsf(j * 10 - d); best = j; }
    return prior(p, d, m) + (best >= 0 ? cell[a][best].r : versatz);
}

static void learn(float p, float d, float m, float y)
{
    float res = y - prior(p, d, m);
    versatz += (res - versatz) / 60;
    int a = pk(p), b = dk(d);
    if (cell[a][b].n == 0) cell[a][b].r = versatz;
    cell[a][b].n++;
    cell[a][b].r += (res - cell[a][b].r) / (cell[a][b].n < 60 ? cell[a][b].n : 60);
}

static int felder(void) { int n = 0; for (int a = 0; a < NP; a++) for (int b = 0; b < ND; b++) n += cell[a][b].n > 0; return n; }

static int stufe_von(int mhz) { int i = 0; while (i + 1 < NSTUFEN && stufen[i + 1] <= mhz) i++; return i; }

static void load_model(void)
{
    wchar_t p[MAX_PATH]; char line[128], name[16]; int a, b, n, m; float r;
    lage[LAGE_WENIGE].stufe = stufe_von(4200); lage[LAGE_VIELE].stufe = stufe_von(3600); lage[LAGE_GRAFIK].stufe = stufe_von(3600);
    swprintf(p, MAX_PATH, L"%s\\modell.txt", dataDir);
    FILE *f = _wfopen(p, L"r");
    if (!f) return;
    int skala = 1;   // files without the line come from 0.1/0.2 (old heat estimate)
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "skala %d", &m) == 1) skala = m;
        else if (sscanf(line, "versatz %f", &r) == 1) { if (skala == SKALA) versatz = r; }
        else if (sscanf(line, "feld %d %d %f %d", &a, &b, &r, &n) == 4 && a >= 0 && a < NP && b >= 0 && b < ND) { if (skala == SKALA) { cell[a][b].r = r; cell[a][b].n = n; } }
        else if (sscanf(line, "deckel %15s %d", name, &m) == 2) { for (int i = 0; i < 3; i++) if (!strcmp(name, lageName[i])) lage[i].stufe = stufe_von(m); }
        else if (sscanf(line, "fehl %15s %d %d", name, &m, &n) == 3) { for (int i = 0; i < 3; i++) if (!strcmp(name, lageName[i])) lage[i].fehl[stufe_von(m)] = n; }
    }
    fclose(f);
    if (skala != SKALA) event(L_("skala", "Neues Wärmemodell (ganze Chipleistung): Lüfter-Erfahrung wird neu gelernt, Taktgrenzen bleiben"));
}

static void save_model(void)
{
    wchar_t p[MAX_PATH], tmp[MAX_PATH];
    swprintf(p, MAX_PATH, L"%s\\modell.txt", dataDir); swprintf(tmp, MAX_PATH, L"%s\\modell.tmp", dataDir);
    FILE *f = _wfopen(tmp, L"w");
    if (!f) return;
    fprintf(f, "skala %d\nversatz %.2f\n", SKALA, versatz);
    for (int i = 0; i < 3; i++) fprintf(f, "deckel %s %d\n", lageName[i], stufen[lage[i].stufe]);
    for (int i = 0; i < 3; i++) for (int s = 0; s < NSTUFEN; s++) if (lage[i].fehl[s]) fprintf(f, "fehl %s %d %d\n", lageName[i], stufen[s], lage[i].fehl[s]);
    for (int a = 0; a < NP; a++) for (int b = 0; b < ND; b++) if (cell[a][b].n) fprintf(f, "feld %d %d %.2f %d\n", a, b, cell[a][b].r, cell[a][b].n);
    fclose(f);
    MoveFileExW(tmp, p, MOVEFILE_REPLACE_EXISTING);
}

// ---------- BIOS fan values from the UEFI variable "Setup" (offsets 0xD1-0xDC = EC 0x60-0x6B) ----------
// Read from firmware, so a curve left in the EC by a killed run can never be mistaken for the original.
static int read_bios_fan(uint8_t orig[12])
{
    HANDLE tok; TOKEN_PRIVILEGES tp = { 1 };
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &tok)) {
        LookupPrivilegeValueW(0, L"SeSystemEnvironmentPrivilege", &tp.Privileges[0].Luid);
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(tok, FALSE, &tp, 0, 0, 0); CloseHandle(tok);
    }
    static uint8_t buf[4096];
    DWORD n = GetFirmwareEnvironmentVariableW(L"Setup", L"{EC87D643-EBA4-4BB5-A1E5-3F3E36B20DA9}", buf, sizeof buf);
    if (n < 0xDD) return 0;
    memcpy(orig, buf + 0xD1, 12);
    return 1;
}

// fan curve anchored 3 °C below the smoothed EC temperature: today's temperature lies on the rising part,
// duty there = d, +2 % per °C above. Both CPU fan blocks (0x60, 0x66), mode 1 = curve active.
// Each EC write costs a write plus a read-back (~2 ms of spinning), so only registers that change are written (usually
// start/full/pwm). check = 1 reads every register first instead of trusting the cache (done once a minute, in case the
// EC was reset, e.g. after sleep).
static int curveD = -1; static float curveAnchor = -1;
static int curveReg[12] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
static void write_curve(int d, float teSmooth, int check)
{
    int start = (int)floorf(teSmooth) - 3; start = start < 25 ? 25 : start > 92 ? 92 : start;
    int pwm = d - 6 < 0 ? 0 : d - 6;
    int full = start + (100 - pwm + 1) / 2 + 1; if (full > 105) full = 105;
    static const int order[6] = { 1, 2, 3, 4, 5, 0 };   // the mode byte (curve on) last
    int val[6] = { 1, 20, start, full, pwm, 2 };
    for (int b = 0; b <= 6; b += 6)
        for (int j = 0; j < 6; j++) {
            int i = b + order[j];
            if ((check ? ec_read((uint8_t)(0x60 + i)) : curveReg[i]) != val[order[j]])
                curveReg[i] = ec_write((uint8_t)(0x60 + i), (uint8_t)val[order[j]]) ? val[order[j]] : -1;
            else curveReg[i] = val[order[j]];
        }
    curveD = d; curveAnchor = teSmooth;
}

static int restore_bios(const uint8_t orig[12])
{
    int ok = 1;
    for (int i = 11; i >= 0; i--) { ok &= ec_write((uint8_t)(0x60 + i), orig[i]); curveReg[i] = -1; }
    curveD = -1; curveAnchor = -1;
    return ok;
}

static int cmp_float(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return (x > y) - (x < y); }

// ---------- the controller ----------
static int run(void)
{
    GetModuleFileNameW(0, exeDir, MAX_PATH);
    *wcsrchr(exeDir, L'\\') = 0;
    ExpandEnvironmentStringsW(L"%ProgramData%\\evo-x2-cool", dataDir, MAX_PATH);
    ExpandEnvironmentStringsW(EVO_INI, iniFile, MAX_PATH);
    CreateDirectoryW(dataDir, 0);
    load_settings();
    FILETIME iniSeen = ini_time();

    // shared objects: everyone may use the EC/PCI locks (LibreHardwareMonitor, FanControl), users may read our data
    SECURITY_ATTRIBUTES saAll = { sizeof saAll }, saRead = { sizeof saRead };
    ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;WD)", SDDL_REVISION_1, &saAll.lpSecurityDescriptor, 0);
    ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;AU)", SDDL_REVISION_1, &saRead.lpSecurityDescriptor, 0);
    HANDLE single = CreateMutexW(0, TRUE, L"Global\\evo-x2-cool-laeuft");
    if (!single || GetLastError() == ERROR_ALREADY_EXISTS) { event(L_("laeuft", "evo-x2-cool läuft schon, zweiter Start beendet")); return 1; }
    mPci = CreateMutexW(&saAll, FALSE, L"Global\\Access_PCI");
    mEc = CreateMutexW(&saAll, FALSE, L"Global\\Access_EC");
    hCpu = pio_load(exeDir, L"AMDFamily17");
    hEc = pio_load(exeDir, L"LpcACPIEC");
    if (!hCpu || !hEc) { event(L_("pawnio", "PawnIO nicht erreichbar (Adminrechte? pawnio\\*.bin neben der exe?)")); return 1; }
    HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, &saRead, PAGE_READWRITE, 0, sizeof(EvoShared), EVO_SHM);
    sh = map ? MapViewOfFile(map, FILE_MAP_WRITE, 0, 0, 0) : 0;
    if (sh) { memset(sh, 0, sizeof *sh); sh->magic = EVO_MAGIC; sh->size = sizeof *sh; }

    // safety: only control the EC we know (firmware 1.10 = bytes 01 0A at 0x00)
    int v0 = ec_read(0x00), v1 = ec_read(0x01);
    if (!nurLesen && (v0 != 1 || v1 != 10)) { event(L_("ec_version", "EC-Version %d.%d unbekannt (erwartet 1.10): nur lesen"), v0, v1); nurLesen = 1; }
    uint8_t orig[12] = { 0 };
    if (!nurLesen && !read_bios_fan(orig)) { event(L_("bios_fehlt", "BIOS-Lüftereinstellungen (UEFI Setup) nicht lesbar: nur lesen")); nurLesen = 1; }

    topology();
    adl_init();
    if (adlIdx < 0) event(L_("adl", "GPU-Werte vom Treiber nicht verfügbar: GPU-Leistung wird geschätzt"));
    NTQSI ntqsi = (NTQSI)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation");
    load_model();
    int modusOrig = ec_read(0x31), modusJetzt = modusOrig;
    if (modusOrig == 2 || modusOrig < 0) modusOrig = 0;   // "leise" at start is most likely left over from a killed run
    if (sh) {
        sh->nurLesen = nurLesen; sh->funktionen = nurLesen ? 0 : fn; sh->ziel = ziel; sh->minP = minP; sh->maxP = maxP; sh->spitze = spitze;
        sh->versatz = versatz; sh->felder = felder();
        for (int i = 0; i < 3; i++) sh->deckel[i] = stufen[lage[i].stufe];
    }
    if (nurLesen) event(L_("start_nurlesen", "Start: nur lesen, es wird nichts gesteuert"));
    else {
        event(L_("start", "Start: Ziel %.0f °C, Lüfter %d–%d %%, Spitze ab %d °C, gelernt: %d Bereiche, %+.1f °C"),
              ziel, minP, maxP, spitze, felder(), versatz);
        for (int i = 0; i < EVO_NFUNK; i++) if (!(fn >> i & 1)) event(L_("f_aus", "%s ist ausgeschaltet"), funk_text(i));
        event(L_("bios", "BIOS-Lüfterkurve gesichert (%d/%d/%d °C), kommt beim Beenden zurück"),
              orig[1], orig[2], orig[3]);
    }

    if (!nurLesen && !(fn & F_LUEFTER)) restore_bios(orig);   // a killed run may have left its curve in the EC
    if (!nurLesen && !(fn & F_TAKT) && read_cap() != 0) write_cap(0);
    uint64_t unit = 0; msr(0xC0010299, &unit);
    double joule = 1.0 / (double)(1ull << ((unit >> 8) & 0x1F));   // energy status unit
    uint32_t eCore[MAXCPU] = { 0 }, ePkg = 0;
    uint64_t aperf[MAXCPU] = { 0 };
    double coreW[MAXCPU] = { 0 };
    LARGE_INTEGER tSeen[MAXCPU] = { 0 }; unsigned seen[MAXCPU] = { 0 };
    static ProcPerf pp[MAXCPU], pp0[MAXCPU];
    static AdlPmLog gl;
    LARGE_INTEGER f, tPkg = { 0 }; QueryPerformanceFrequency(&f);
    double pkgW = 0;

    // controller state
    float hist[10]; int nHist = 0;
    float emaT = 0, slope = 0, pFast = 0, pSlow = 0, mFast = 0, mSlow = 0, emaTe = 0, d = (float)(minP > 40 ? minP : 40), boost = 0;
    int capNow = read_cap(), sicherBis = 0, grafikSeit = -1, nachlauf = 0, lageNow = -1, hot = 0, lowSince = -1, warNotfall = 0;
    int ec[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
    static const uint8_t regs[8] = { 0x70, 0x35, 0x36, 0x31, 0x37, 0x38, 0x28, 0x29 };
    int console = GetConsoleWindow() != 0;
    ULONGLONG next = GetTickCount64();

    for (int k = 0; ; k++) {
        // --- measure ---
        double load = 0, coreLoad[MAXCPU] = { 0 }, coreBusy[MAXCPU] = { 0 }, thrBusy[MAXCPU] = { 0 }; int busy = 0;
        if (ntqsi && ntqsi(8, pp, sizeof pp, 0) >= 0) {
            double sb = 0, st = 0;
            for (int c = 0; c < nThreads; c++) {
                double idle = (double)(pp[c].idle.QuadPart - pp0[c].idle.QuadPart);
                double tot = (double)(pp[c].kernel.QuadPart - pp0[c].kernel.QuadPart + pp[c].user.QuadPart - pp0[c].user.QuadPart);
                if (tot > 0) { thrBusy[c] = 1 - idle / tot; sb += tot - idle; st += tot; }
                coreLoad[coreOf[c]] += thrBusy[c] / threadsOfCore[coreOf[c]];
                if (thrBusy[c] > coreBusy[coreOf[c]]) coreBusy[coreOf[c]] = thrBusy[c];
            }
            if (st > 0) load = 100 * sb / st;
            for (int i = 0; i < nCores; i++) busy += coreLoad[i] > 0.5;
            memcpy(pp0, pp, sizeof pp);
        }
        // APERF and core energy need the thread pinned to that CPU. Moving there costs more than the read, so only the first
        // thread of each core is visited (the clock shown is that thread's), and idle cores are skipped; their counters are
        // simply read over a longer span later (each core at least every 10 s), which keeps the averages exact.
        double maxMhz = 0;
        for (int c = 0; c < nThreads; c++) {
            if (!firstOfCore[c] || (k && coreBusy[coreOf[c]] <= 0.03 && k - seen[c] < 10)) continue;
            SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << c);
            LARGE_INTEGER now; QueryPerformanceCounter(&now);
            double span = (double)(now.QuadPart - tSeen[c].QuadPart) / f.QuadPart;
            uint64_t a = 0, e = 0;
            if (msr(0xC00000E8, &a)) { if (k && a > aperf[c]) { double m = (a - aperf[c]) / span / 1e6; if (m > maxMhz) maxMhz = m; } aperf[c] = a; }
            if (msr(0xC001029A, &e)) { if (k) coreW[coreOf[c]] = (uint32_t)((uint32_t)e - eCore[c]) * joule / span; eCore[c] = (uint32_t)e; }
            tSeen[c] = now; seen[c] = k;
        }
        // package energy always on CPU 0 and timed right at the read (issue #2). Read on whatever CPU the thread was on and
        // timed before the core loop, it jumped between 40 and 130 W at a steady 85 W, and a counter standing a little
        // behind the previous read wrapped to ~65000 W. A step backwards or an implausible value keeps the last reading.
        SetThreadAffinityMask(GetCurrentThread(), 1);
        uint64_t e = 0; LARGE_INTEGER tp; QueryPerformanceCounter(&tp);
        if (msr(0xC001029B, &e)) {
            uint32_t de = (uint32_t)e - ePkg; double sp = (double)(tp.QuadPart - tPkg.QuadPart) / f.QuadPart;
            if (k && de < 0x80000000u && sp > 0.2 && de * joule / sp < 400) pkgW = de * joule / sp;
            ePkg = (uint32_t)e; tPkg = tp;
        }
        SetThreadAffinityMask(GetCurrentThread(), ((DWORD_PTR)1 << nThreads) - 1);
        double sumW = 0, maxW = 0;
        for (int i = 0; i < nCores; i++) { sumW += coreW[i]; if (coreW[i] > maxW) maxW = coreW[i]; }

        float T = -1;
        if (WaitForSingleObject(mPci, 50) <= WAIT_ABANDONED) {
            int64_t a = 0x59800, r = 0;
            if (pio_exec(hCpu, "ioctl_read_smn", &a, 1, &r, 1)) {
                uint32_t v = (uint32_t)r;
                T = (v >> 21) * 0.125f;
                if ((v & 0x80000) || (v & 0x30000) == 0x30000) T -= 49;
            }
            ReleaseMutex(mPci);
        }
        // EC temperature and fan 1 every 2 s (the EC temperature is only the second emergency path and the slow curve
        // anchor; Tctl is read every second), the rest every 10 s
        int nEc = k % 10 == 0 ? 8 : k % 2 == 0 ? 3 : 0;
        for (int i = 0; i < nEc; i++) { int v = ec_read(regs[i]); if (v >= 0) ec[i] = v; }
        int Te = ec[0];
        // GPU every second while it works, every 2 s while it rests (a start is noticed at most 1 s later)
        static int gW = -1, gL = -1, gT = -1, aW = -1;
        if (adlIdx >= 0 && (k % 2 == 0 || gW > 5) && adlPm(adlCtx, adlIdx, &gl) == 0) {
            gW = gl.s[PM_GFX_POWER].supported ? gl.s[PM_GFX_POWER].value : -1;
            gL = gl.s[PM_GFX_ACTIVITY].supported ? gl.s[PM_GFX_ACTIVITY].value : -1;
            gT = gl.s[PM_TEMP_GFX].supported ? gl.s[PM_TEMP_GFX].value : -1;
            aW = gl.s[PM_ASIC_POWER].supported ? gl.s[PM_ASIC_POWER].value : -1;
        }
        float G = gW >= 0 ? (float)gW : (float)fmax(0, pkgW - sumW - 15);   // fallback: package minus cores minus ~15 W SoC base
        if (nurLesen && k % 10 == 0) capNow = read_cap();
        if (k == 0) { next += 1000; Sleep(1000); continue; }   // first round only primes the counters
        if (T < 0) T = emaT;

        // --- smoothing: Tctl with a 10-s median against boost spikes, then EMA; heat power fast (3 s) and slow (20 s) ---
        // heat power: the whole chip as the GPU driver reports it (ADL ASIC power), else the package counter, else the old
        // estimate from cores and GPU plus a SoC base; the hottest core counts separately in the prior (M)
        float heat = aW > 0 ? (float)aW : pkgW > 0 ? (float)pkgW : (float)(1.1 * sumW + 0.6 * G + 15);
        if (nHist < 10) hist[nHist++] = T; else { memmove(hist, hist + 1, 9 * sizeof *hist); hist[9] = T; }
        float sorted[10]; memcpy(sorted, hist, nHist * sizeof *hist); qsort(sorted, nHist, sizeof *sorted, cmp_float);
        float med = sorted[nHist / 2];
        if (k == 1) { emaT = T; pFast = pSlow = heat; mFast = mSlow = (float)maxW; emaTe = (float)Te; }   // start from real values, a gap would look like a rise
        float prevT = emaT;
        emaT += (med - emaT) / 8;
        slope += ((emaT - prevT) - slope) / 10;
        pFast += (heat - pFast) / 3; pSlow += (heat - pSlow) / 20;
        mFast += ((float)maxW - mFast) / 3; mSlow += ((float)maxW - mSlow) / 20;
        if (Te >= 0) emaTe += (Te - emaTe) / 10;   // the EC temperature jumps by 15 °C within seconds; the anchor follows slowly

        // --- settings file changed? (checked every 5 s, a cheap time stamp compare) ---
        if (k % 5 == 0) {
            FILETIME t = ini_time();
            if (CompareFileTime(&t, &iniSeen)) {
                iniSeen = t;
                unsigned old = load_settings();
                for (int i = 0; i < EVO_NFUNK; i++)
                    if ((old ^ fn) >> i & 1) event(fn >> i & 1 ? L_("f_an", "%s eingeschaltet") : L_("f_aus", "%s ist ausgeschaltet"), funk_text(i));
                if (old == fn) event(L_("ini", "Einstellungen neu gelesen: Ziel %.0f °C, Lüfter %d–%d %%, Spitze ab %d °C"), ziel, minP, maxP, spitze);
                if (!nurLesen && (old & F_TAKT) && !(fn & F_TAKT) && write_cap(0)) capNow = 0;   // off = no clock limit at all
            }
        }

        // --- situation and clock cap ---
        if (G > 20) { if (grafikSeit < 0) grafikSeit = k; }
        else if (grafikSeit >= 0) { if (k - grafikSeit >= 30) nachlauf = 1; grafikSeit = -1; }
        if (nachlauf && emaT < 65) nachlauf = 0;   // the heat of the GPU phase is gone
        int lg = G > 20 || nachlauf ? LAGE_GRAFIK : busy > 4 ? LAGE_VIELE : LAGE_WENIGE;
        if (lg != lageNow) { lage[lg].spitze = (int)T; if (lageNow >= 0 && !nurLesen) event(L_("lastart", "Lastart jetzt: %s"), lage_text(lg)); lageNow = lg; }
        else if (T > lage[lg].spitze) lage[lg].spitze = (int)T;
        if (!nurLesen && (fn & F_TAKT)) {
            if (T >= spitze) {
                // hot spot: lowest step at once; this situation loses one step (learned from danger), and the step that
                // caused it is tried again only after a longer calm run each time
                if (k > sicherBis && lage[lg].stufe > 0 && capNow > stufen[0]) {
                    int s = lage[lg].stufe;
                    if (lage[lg].fehl[s] < 9) lage[lg].fehl[s]++;
                    lage[lg].stufe--;
                    event2(1, L_("spitze_runter", "Spitze %.0f °C bei „%s“: 30 s Taktbremse %d MHz, für diese Lastart künftig %d MHz"), T, lage_text(lg), stufen[0], stufen[lage[lg].stufe]);
                    event(L_("spitze_warte", "%d MHz erst wieder nach %d min Last ohne Spitze"), stufen[s], wartezeit(lage[lg].fehl[s]) / 60);
                } else if (k > sicherBis)
                    event2(1, L_("spitze_unten", "Spitze %.0f °C bei „%s“, Taktbremse stand schon auf %d MHz"), T, lage_text(lg), capNow);
                sicherBis = k + 30; lage[lg].ruhig = 0;
            }
            // calm long enough at this step: may try the next one. Only seconds in which a core really runs at the cap count
            // (idle time says nothing about whether more clock is safe), and a step with hot spots behind it waits longer.
            if (k > sicherBis && lage[lg].stufe < NSTUFEN - 1) {
                if (lage[lg].spitze >= 80) { lage[lg].ruhig = 0; lage[lg].spitze = (int)T; }
                else if (capNow > 0 && maxMhz >= capNow - 250) lage[lg].ruhig++;
                int s = lage[lg].stufe + 1, w = wartezeit(lage[lg].fehl[s]);
                if (lage[lg].ruhig >= w) {
                    lage[lg].stufe = s; lage[lg].ruhig = 0;
                    event(L_("ruhig", "%d min am Limit ohne Spitze bei „%s“: probiert %d MHz"), w / 60, lage_text(lg), stufen[s]);
                }
            }
            int want = k <= sicherBis ? stufen[0] : stufen[lage[lg].stufe];
            if (want != capNow && write_cap(want)) { event(L_("takt", "Taktbremse %d → %d MHz%s"), capNow, want, k <= sicherBis ? L_("takt_spitze", " (nach Hitzespitze)") : ""); capNow = want; }
        }
        if (!nurLesen) {
            int modus = (fn & F_LEISE) && lg == LAGE_VIELE && G < 5 ? 2 : modusOrig;
            if (modus != modusJetzt && ec_write(0x31, (uint8_t)modus)) { event(L_("modus", "EC-Leistungsmodus → %s"), modus == 2 ? L_("modus2", "leise (54 W)") : modus == 1 ? L_("modus1", "Leistung") : L_("modus0", "ausgewogen")); modusJetzt = modus; }
        }

        // --- learn: only when calm (power steady, temperature hardly moving) and not during boost hot spots (>= 90 °C:
        //     those come from one core inside the chip, more fan does not change them). End temperature = T + tau * slope ---
        if (!nurLesen && (fn & F_LERNEN) && k > 30 && T < 90 && emaT < 90 && fabsf(pFast - pSlow) < 5 && fabsf(slope) < 0.08f && curveD >= 0)
            learn(pSlow, (float)curveD, mSlow, emaT + tau * slope);

        // --- predict: rising power counts ahead (fast EMA plus its lead over the slow one); switched off: current power only ---
        float pv = fmaxf(pFast, pSlow) + (fn & F_VORHERSAGE ? fmaxf(0, pFast - pSlow) : 0);
        int need = maxP;
        for (int x = minP; x <= maxP; x += 5) if (predict(pv, (float)x, mFast) <= ziel - 1) { need = x; break; }
        // feedback: warmer than the target anyway -> add more; let it go again slowly (not in the first seconds: median not full)
        if (k >= 15) { if (emaT > ziel + 1) boost = fminf(40, boost + 1); else if (emaT < ziel - 3) boost = fmaxf(0, boost - 1); }
        float wantD = fminf((float)maxP, need + boost);
        int grund = boost >= 1 ? GRUND_NACHREGELN : (fn & F_VORHERSAGE) && pv > pSlow + 5 ? GRUND_VORHERSAGE : GRUND_MODELL;
        // emergency above the noise limit
        if (T >= 96) hot++; else if (hot > 0) hot--;
        if (hot >= 5 || Te >= 90) { wantD = 100; grund = GRUND_NOTFALL; }
        if (grund == GRUND_NOTFALL && !warNotfall && !nurLesen) event2(1, L_("notfall", "NOTFALL: CPU %.0f °C, Gehäuse %d °C → Lüfter 100 %%"), T, Te);
        // up: a prediction raises by at most 4 %/s (jumping 30 -> 70 % is loud, the fan needs ~10 s per step anyway),
        // at once only when really hot or in an emergency; down after 10 s by 3 %/s; after an emergency straight to the wanted value
        if (wantD >= d) { d = grund == GRUND_NOTFALL || emaT >= ziel + 5 ? wantD : fminf(wantD, d + 4); lowSince = -1; }
        else if (warNotfall && T < 90) { d = wantD; lowSince = -1; }
        else { if (lowSince < 0) lowSince = k; if (k - lowSince >= 10) d = fmaxf(wantD, d - 3); }
        warNotfall = grund == GRUND_NOTFALL;
        int di = (int)lroundf(d);
        int fanOn = !nurLesen && ((fn & F_LUEFTER) || grund == GRUND_NOTFALL);   // the emergency takes over even when switched off
        if (fanOn && (abs(di - curveD) >= 2 || fabsf(emaTe - curveAnchor) > 3)) write_curve(di, emaTe, 0);
        else if (fanOn && k % 60 == 30 && curveD >= 0) write_curve(curveD, curveAnchor, 1);
        else if (!fanOn && curveD >= 0) restore_bios(orig);
        if (nurLesen) grund = GRUND_NUR_LESEN;

        // --- record ---
        int r1 = ec[1] < 0 || ec[2] < 0 ? -1 : ec[1] << 8 | ec[2], r2 = ec[4] < 0 || ec[5] < 0 ? -1 : ec[4] << 8 | ec[5], r3 = ec[6] < 0 || ec[7] < 0 ? -1 : ec[6] << 8 | ec[7];
        float vorh = predict(pv, (float)(curveD >= 0 ? curveD : di), mFast);
        static const char *grundName[] = { "modell", "vorhersage", "nachregeln", "NOTFALL", "nur-lesen" };
        if (sh) {
            EvoSample *s = &sh->s[sh->nSample % EVO_N];
            s->t = time(0); s->tctl = T; s->tglatt = emaT; s->vorhersage = vorh; s->pkgW = (float)pkgW; s->coreW = (float)sumW; s->maxCoreW = (float)maxW;
            s->gpuW = G; s->apuW = (float)aW; s->heatW = heat; s->pVorher = pv; s->mhz = (float)maxMhz; s->load = (float)load;
            s->ecT = (int16_t)Te; s->busy = (int16_t)busy; s->gpuLoad = (int16_t)gL; s->gpuT = (int16_t)gT; s->fan = (int16_t)(nurLesen ? -1 : curveD);
            s->cap = (int16_t)capNow; s->rpm[0] = (int16_t)r1; s->rpm[1] = (int16_t)r2; s->rpm[2] = (int16_t)r3;
            s->lage = (uint8_t)lg; s->modus = (uint8_t)ec[3]; s->grund = (uint8_t)grund; s->leise = modusJetzt == 2;
            sh->nSample++;
            if (k % 60 == 0) { sh->versatz = versatz; sh->felder = felder(); for (int i = 0; i < 3; i++) sh->deckel[i] = stufen[lage[i].stufe]; }
        }
        SYSTEMTIME st; GetLocalTime(&st);
        char line[400];
        snprintf(line, sizeof line, "%02d:%02d:%02d;%.1f;%.1f;%d;%.1f;%.1f;%.1f;%.0f;%d;%.1f;%.1f;%.0f;%.0f;%d;%d;%d;%d;%s;%d;%d;%d;%.1f;%.2f;%d;%d;%s;%d\r\n",
                 st.wHour, st.wMinute, st.wSecond, T, emaT, Te, pkgW, sumW, maxW, G, aW, heat, pv, maxMhz, load, busy, gL, gT,
                 nurLesen ? -1 : curveD, grundName[grund], r1, r2, r3, vorh, versatz, felder(), capNow, lageName[lg], ec[3]);
        csv_line(line);
        if (console) printf("\r%02d:%02d:%02d  Tctl %5.1f (glatt %5.1f)  EC %3d  Kerne %5.1f W  GPU %5.1f W  -> Luefter %3d %% [%s] %d U/min  Takt %d MHz [%s]   ",
                            st.wHour, st.wMinute, st.wSecond, T, emaT, Te, sumW, G, nurLesen ? -1 : curveD, grundName[grund], r1, capNow, lageName[lg]);
        if (!nurLesen && k % 60 == 0) save_model();

        next += 1000;
        ULONGLONG now = GetTickCount64();
        if (WaitForSingleObject(stopEv, next > now ? (DWORD)(next - now) : 0) == WAIT_OBJECT_0) break;
        if (next < now) next = now;
    }

    // --- stop: save, restore BIOS fan values, safe clock, original power mode ---
    if (!nurLesen) {
        save_model();
        int ok = restore_bios(orig);
        if (fn & F_TAKT) write_cap(sicherMhz);
        if (modusJetzt != modusOrig) ec_write(0x31, (uint8_t)modusOrig);
        event(L_("stopp", "Stopp: BIOS-Lüfterkurve %s, Taktbremse bleibt %d MHz, Modell gespeichert"),
              ok ? L_("stopp_ok", "zurück") : L_("stopp_fehler", "NICHT vollständig zurück"), fn & F_TAKT ? sicherMhz : capNow);
    } else event(L_("stopp_nurlesen", "Stopp (nur lesen)"));
    if (csv) CloseHandle(csv);
    CloseHandle(hCpu); CloseHandle(hEc);
    return 0;
}

// ---------- console / service plumbing ----------
static BOOL WINAPI on_ctrl(DWORD t) { (void)t; SetEvent(stopEv); Sleep(10000); return TRUE; }   // keep the process alive while run() restores

static SERVICE_STATUS_HANDLE ssh;
static SERVICE_STATUS ss = { SERVICE_WIN32_OWN_PROCESS, SERVICE_START_PENDING };
static DWORD WINAPI svc_ctrl(DWORD c, DWORD type, void *data, void *ctx)
{
    (void)type; (void)data; (void)ctx;
    if (c == SERVICE_CONTROL_STOP || c == SERVICE_CONTROL_SHUTDOWN || c == SERVICE_CONTROL_PRESHUTDOWN) {
        ss.dwCurrentState = SERVICE_STOP_PENDING; ss.dwWaitHint = 10000; SetServiceStatus(ssh, &ss);
        SetEvent(stopEv); return NO_ERROR;
    }
    return c == SERVICE_CONTROL_INTERROGATE ? NO_ERROR : ERROR_CALL_NOT_IMPLEMENTED;
}
static void WINAPI svc_main(DWORD argc, wchar_t **argv)
{
    (void)argc; (void)argv;
    ssh = RegisterServiceCtrlHandlerExW(SVC, svc_ctrl, 0);
    ss.dwCurrentState = SERVICE_RUNNING; ss.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_PRESHUTDOWN;
    SetServiceStatus(ssh, &ss);
    ss.dwWin32ExitCode = run() ? ERROR_SERVICE_SPECIFIC_ERROR : 0; ss.dwServiceSpecificExitCode = ss.dwWin32ExitCode ? 1 : 0;
    ss.dwCurrentState = SERVICE_STOPPED; SetServiceStatus(ssh, &ss);
}

static int install(const wchar_t *opts)
{
    wchar_t exe[MAX_PATH], cmd[MAX_PATH + 300];
    GetModuleFileNameW(0, exe, MAX_PATH);
    swprintf(cmd, sizeof cmd / sizeof *cmd, L"\"%s\" --dienst%s", exe, opts);
    SC_HANDLE scm = OpenSCManagerW(0, 0, SC_MANAGER_CREATE_SERVICE);
    if (!scm) { printf("Dienstverwaltung nicht erreichbar (Admin-Shell?), Fehler %lu\n", GetLastError()); return 1; }
    SC_HANDLE s = CreateServiceW(scm, SVC, L"EVO-X2-COOL", SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                                 SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, cmd, 0, 0, 0, 0, 0);
    if (!s) { printf("Dienst nicht angelegt, Fehler %lu (gibt es ihn schon? erst --entfernen)\n", GetLastError()); return 1; }
    SERVICE_DESCRIPTIONW desc = { L"Lernende Luefter- und Taktsteuerung fuer die GMKtec EVO-X2 (evo-x2-cool). Anzeige: evo-x2-cool-front.exe" };
    ChangeServiceConfig2W(s, SERVICE_CONFIG_DESCRIPTION, &desc);
    SC_ACTION act[3] = { { SC_ACTION_RESTART, 60000 }, { SC_ACTION_RESTART, 60000 }, { SC_ACTION_RESTART, 60000 } };
    SERVICE_FAILURE_ACTIONSW fa = { 86400, 0, 0, 3, act };
    ChangeServiceConfig2W(s, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);
    SERVICE_PRESHUTDOWN_INFO pre = { 15000 };
    ChangeServiceConfig2W(s, SERVICE_CONFIG_PRESHUTDOWN_INFO, &pre);
    BOOL ok = StartServiceW(s, 0, 0);
    printf("Dienst %ls angelegt (startet mit Windows)%s: %ls\n", SVC, ok ? " und gestartet" : ", Start fehlgeschlagen", cmd);
    CloseServiceHandle(s); CloseServiceHandle(scm);
    return !ok;
}

static int uninstall(void)
{
    SC_HANDLE scm = OpenSCManagerW(0, 0, SC_MANAGER_CONNECT), s = scm ? OpenServiceW(scm, SVC, SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE) : 0;
    if (!s) { printf("Dienst nicht gefunden, Fehler %lu\n", GetLastError()); return 1; }
    SERVICE_STATUS st;
    if (ControlService(s, SERVICE_CONTROL_STOP, &st))
        for (int i = 0; i < 30 && QueryServiceStatus(s, &st) && st.dwCurrentState != SERVICE_STOPPED; i++) Sleep(500);
    BOOL ok = DeleteService(s);
    printf(ok ? "Dienst gestoppt (Werte zurueckgeschrieben) und entfernt.\n" : "Entfernen fehlgeschlagen, Fehler %lu\n", GetLastError());
    CloseServiceHandle(s); CloseServiceHandle(scm);
    return !ok;
}

int wmain(int argc, wchar_t **argv)
{
    int dienst = 0, inst = 0;
    for (int i = 1; i < argc; i++) {
        const wchar_t *a = argv[i];
        if (!wcscmp(a, L"--nur-lesen")) nurLesen = 1;
        else if (!wcscmp(a, L"--dienst")) dienst = 1;
        else if (!wcscmp(a, L"--installieren")) inst = 1;
        else if (!wcscmp(a, L"--entfernen")) return uninstall();
        else if (!wcscmp(a, L"--stopp")) {   // ends a run started outside the service, with the normal restore
            HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, EVO_STOPP);
            if (!ev) { printf("Kein laufender Regler gefunden.\n"); return 1; }
            SetEvent(ev); CloseHandle(ev);
            HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, L"Global\\evo-x2-cool-laeuft");   // wait until it has restored and exited
            DWORD r = m ? WaitForSingleObject(m, 20000) : WAIT_FAILED;
            if (m) CloseHandle(m);
            printf(r == WAIT_TIMEOUT ? "Stopp gesendet, der Regler beendet sich noch nicht.\n" : "Regler beendet, Werte zurueckgestellt.\n");
            return r == WAIT_TIMEOUT;
        }
        else { printf("Unbekannte Option %ls (Einstellungen stehen in %%ProgramData%%\\evo-x2-cool\\einstellungen.ini)\n", a); return 1; }
    }
    if (inst) return install(nurLesen ? L" --nur-lesen" : L"");
    SECURITY_ATTRIBUTES sa = { sizeof sa };   // admins and SYSTEM may send the stop, the logged-on user too ("Alles beenden" in the window):
    // stopping only gives the fans back to the BIOS curve and leaves the safe clock cap, so that is harmless
    ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x100002;;;IU)", SDDL_REVISION_1, &sa.lpSecurityDescriptor, 0);
    stopEv = CreateEventW(&sa, TRUE, FALSE, EVO_STOPP);
    ResetEvent(stopEv);
    if (dienst) {
        SERVICE_TABLE_ENTRYW t[] = { { SVC, svc_main }, { 0, 0 } };
        return !StartServiceCtrlDispatcherW(t);
    }
    SetConsoleCtrlHandler(on_ctrl, TRUE);
    return run();
}
