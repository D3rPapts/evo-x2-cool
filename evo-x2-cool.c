// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Fabian Storck
//
// evo-x2-cool: learning fan and clock controller for the GMKtec EVO-X2 (Sixunited AXB35, Ryzen AI MAX+ 395, EC IT5570 1.10).
// Once per second it
//  - measures Tctl (SMN), per-core and package power (RAPL MSRs), highest effective clock (APERF), EC temperature and
//    fan speeds (EC through PawnIO), GPU power/load/temperature (ADL), CPU load per core and the SSDs;
//  - learns in a table (heat power x fan duty) how warm it really gets, compared to a physical prior, predicts rising
//    power ahead and picks the quietest duty whose predicted end temperature stays below the target;
//  - steers the fans through the curve INSIDE the EC (0x60-0x6B, rising above today's temperature), so if this program
//    dies the EC keeps regulating with a rising curve;
//  - caps the CPU clock (power plan PROCFREQMAX) per load type, drops to a safe step for 30 s after a hot spot and learns
//    per load type which step is safe;
//  - many cores busy and the GPU idle: EC power mode "leise" (54 W).
// On stop: model saved, BIOS fan values (from the UEFI Setup variable) written back, clock cap left at the safe value,
// EC power mode restored (if it was readable). Data: %ProgramData%\evo-x2-cool (einstellungen.ini, modell.txt, regler-<date>.csv, ereignisse.txt).
// Settings and the on/off switches live in einstellungen.ini (re-read within 5 s), texts in sprache-<code>.ini next to the exe.
//
//   evo-x2-cool                      run in this console (Ctrl+C stops and restores)
//   evo-x2-cool --nur-lesen          only measure and show, write nothing to the hardware
//   evo-x2-cool --ohne-ec            measure without any EC access (implies --nur-lesen)
//   evo-x2-cool --installieren [--nur-lesen]   register as Windows service (auto start, restart on failure) and start it
//   evo-x2-cool --entfernen          stop and remove the service (restores everything on the way out)
//   evo-x2-cool --stopp              end a run started outside the service, also with the restore
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <winioctl.h>
#include <powrprof.h>
#include <sddl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>
#include <math.h>
#include <time.h>
#include <winevt.h>
#include <evntrace.h>
#include <evntcons.h>
#include "evo.h"
#pragma comment(lib, "powrprof.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "wevtapi.lib")

#define PIO_LOAD 0xA1B22084u   // (41394 << 16) | (0x821 << 2)
#define PIO_EXEC 0xA1B22104u   // (41394 << 16) | (0x841 << 2)
#define MAXCPU 64
#define NDRIVES 8
#define SVC L"evo-x2-cool"

// ---------- settings (einstellungen.ini, section [Regler]) ----------
// One table for defaults, the default file and the limits. Limits that depend on each other follow in load_settings.
static float ziel; static int minP, minLeer, maxP, spitze, achtung, alarm, sicherMhz, taktPlus, nurLesen, ohneEc;
static int minDauer, dauerW;   // fan floor under lasting high load (0 = off), and from how many watts
static int sprungW, sprungP;   // power jump within 2 s that sends the fan straight to sprungP % (0 = off)
static const struct { const wchar_t *key; int *var, def, lo, hi, imFile; } settings[] = {
    { L"MinProzent", &minP, 30, 0, 100, 1 },
    { L"MinLeerlauf", &minLeer, 15, 0, 100, 1 },   // lower floor while idle and far below the target
    { L"MaxProzent", &maxP, 70, 0, 100, 1 },
    { L"Spitze", &spitze, 88, 70, 95, 1 },         // clock brake: hot spot temperature
    { L"Achtung", &achtung, 90, 70, 100, 1 },      // yellow: fan climbs steadily
    { L"Alarm", &alarm, 95, 80, 97, 1 },           // red: fan 100 % (the freezes came at ~100 °C)
    { L"SicherMHz", &sicherMhz, 3600, INT_MIN, INT_MAX, 1 },   // clock cap left behind on stop (not limited)
    { L"MinDauerlast", &minDauer, 45, 0, 100, 1 },
    { L"DauerlastW", &dauerW, 70, 30, 150, 1 },
    { L"SprungW", &sprungW, 50, 0, 150, 1 },
    { L"SprungProzent", &sprungP, 50, 30, 100, 1 },
    { L"TaktPlus", &taktPlus, 0, -600, 600, 0 },   // test switch: MHz added to every clock cap
};
#define ZIEL_DEF 80
static unsigned fn = F_ALLE;   // functions switched on
static const int stufen[] = { 3000, 3300, 3600, 3900, 4200, 4500 };   // clock cap steps, MHz
#define NSTUFEN 6
// lowest step per load type, also the cap for 30 s after a hot spot: 3300 MHz, 3000 with GPU and many cores busy (there
// the cores take the GPU's power and Tctl jumped from 80 to 91-97 °C within 5-10 s). Lower steps cost much clock for
// little less heat.
static const int unten[EVO_NLAGE] = { 1, 1, 1, 0 };
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

static void event2(int wichtig, const char *fmt, ...);
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// the settings the viewer shows
static void publish_settings(void)
{
    if (!sh) return;
    sh->nurLesen = nurLesen; sh->funktionen = nurLesen ? 0 : fn;
    sh->ziel = ziel; sh->minP = minP; sh->maxP = maxP; sh->spitze = spitze; sh->achtung = achtung; sh->alarm = alarm; sh->taktPlus = taktPlus;
}

// writes the default file if there is none (interactive users may change it, so the switches in the viewer work without
// admin rights)
static void write_default_settings(void)
{
    char def[1024]; int n = snprintf(def, sizeof def, "; evo-x2-cool settings, re-read within 5 s after a change\r\n[Regler]\r\nZiel=%d\r\n", ZIEL_DEF);
    for (int i = 0; i < (int)(sizeof settings / sizeof *settings); i++)
        if (settings[i].imFile) n += snprintf(def + n, sizeof def - n, "%ls=%d\r\n", settings[i].key, settings[i].def);
    n += snprintf(def + n, sizeof def - n,
                  "Sprache=de\r\n\r\n"
                  "; 1 = on, 0 = off. The emergency (fan 100 %% when overheating) always stays on.\r\n"
                  "[Funktionen]\r\nLuefter=1\r\nTaktbremse=1\r\nEcLeise=1\r\nLernen=1\r\nVorhersage=1\r\n\r\n"
                  "; window: Thema = system / hell / dunkel; UpdatePruefen = 1: ask GitHub once a day for a newer version (only shown, nothing installed)\r\n"
                  "[Anzeige]\r\nThema=system\r\nUpdatePruefen=1\r\n");
    SECURITY_ATTRIBUTES sa = { sizeof sa }; DWORD w;
    ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;FA;;;SY)(A;;FA;;;BA)(A;;FRFW;;;IU)", SDDL_REVISION_1, &sa.lpSecurityDescriptor, 0);
    HANDLE f = CreateFileW(iniFile, GENERIC_WRITE, 0, &sa, CREATE_NEW, 0, 0);
    if (f != INVALID_HANDLE_VALUE) { WriteFile(f, def, (DWORD)n, &w, 0); CloseHandle(f); }
    LocalFree(sa.lpSecurityDescriptor);
}

// reads einstellungen.ini; returns the previous function bits
static unsigned load_settings(void)
{
    if (GetFileAttributesW(iniFile) == INVALID_FILE_ATTRIBUTES) write_default_settings();
    wchar_t buf[16], code[16], def[8];
    swprintf(def, 8, L"%d", ZIEL_DEF);
    GetPrivateProfileStringW(L"Regler", L"Ziel", def, buf, 16, iniFile); ziel = (float)_wtof(buf);
    for (int i = 0; i < (int)(sizeof settings / sizeof *settings); i++)
        *settings[i].var = clampi(GetPrivateProfileIntW(L"Regler", settings[i].key, settings[i].def, iniFile), settings[i].lo, settings[i].hi);
    // limits that depend on each other: idle floor <= floor <= max; green < yellow < red, at least 2 °C apart;
    // a power jump below 20 W would fire on noise
    minLeer = minLeer > minP ? minP : minLeer;
    maxP = maxP < minP ? minP : maxP;
    achtung = achtung > alarm - 2 ? alarm - 2 : achtung;
    ziel = ziel < 50 ? 50 : ziel > 92 ? 92 : ziel;
    if (ziel > achtung - 2) ziel = (float)(achtung - 2);
    if (sprungW > 0 && sprungW < 20) sprungW = 20;
    GetPrivateProfileStringW(L"Regler", L"Sprache", L"de", code, 16, iniFile);
    wchar_t lf[MAX_PATH]; swprintf(lf, MAX_PATH, L"%s\\sprache-%s.ini", exeDir, code);
    if (wcscmp(lf, langFile)) { wcscpy(langFile, lf); nLang = 0; }
    unsigned old = fn; fn = 0;
    for (int i = 0; i < EVO_NFUNK; i++) if (GetPrivateProfileIntW(L"Funktionen", evoFunkKey[i], 1, iniFile)) fn |= 1u << i;
    publish_settings();
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
#define MSR_RAPL_UNIT 0xC0010299
#define MSR_CORE_ENERGY 0xC001029A
#define MSR_PKG_ENERGY 0xC001029B
#define MSR_APERF 0xC00000E8
#define SMN_TCTL 0x59800

// ---------- EC (ACPI protocol on ports 0x62/0x66) ----------
#define EC_DATA 0x62
#define EC_SC 0x66        // status (read) / command (write)
#define EC_OBF 0x01       // output buffer full: a byte waits to be read
#define EC_IBF 0x02       // input buffer full: the EC has not taken the last byte yet
#define EC_SCI 0x20       // SCI_EVT: the EC has an event pending that Windows is about to query
#define EC_RD 0x80
#define EC_WR 0x81
// After a write the controller leaves the EC alone for EC_RUHE_MS (no reads, no writes; emergency and yellow excepted):
// ACPI 13 came 5 s after mode switches and curve writes.
#define EC_RUHE_MS 6000
// registers (EC firmware 1.10)
#define REG_VERSION 0x00  // 0x00/0x01 = major/minor
#define REG_MODUS 0x31    // power mode: 0 ausgewogen, 1 Leistung, 2 leise (54 W)
#define MODUS_LEISE 2
#define MODUS_HALT_S 120  // back from "leise" only after this many seconds of not wanting it
#define REG_TEMP 0x70     // EC temperature (case sensor), °C
#define REG_L1 0x35       // fan speeds, rpm high byte then low byte: CPU fan 1 (0x35/0x36), CPU fan 2 (0x37/0x38),
#define REG_L2 0x37       //   system fan (0x28/0x29)
#define REG_SYS 0x28
#define REG_KURVE 0x60    // fan curves: 0x60-0x65 CPU fans (both blowers), 0x66-0x6B system fan (120 mm, back of the board)

static uint8_t pin(uint8_t port) { int64_t a = port, r = 0; pio_exec(hEc, "ioctl_pio_read", &a, 1, &r, 1); return (uint8_t)r; }
static void pout(uint8_t port, uint8_t v) { int64_t a[2] = { port, v }; pio_exec(hEc, "ioctl_pio_write", a, 2, 0, 0); }
// Must spin: sleeping between the handshake steps made every read fail (Windows' own EC driver cuts in; it does not take
// our mutex). Each byte costs ~1 ms of spinning, so few bytes are read.
static int ec_wait(uint8_t mask, uint8_t want) { for (int i = 0; i < 20000; i++) if ((pin(EC_SC) & mask) == want) return 1; return 0; }

// Windows' ACPI driver uses the same two ports: the EC raises SCI_EVT about every 10 s and Windows then sends its query.
// A transaction of ours in that moment, or taking a byte meant for Windows, makes Windows time out (ACPI event 13). So a
// transaction starts only after the status showed no byte waiting, no busy input and no pending event 16 times in a row
// (~0.1 ms); otherwise it gives up and the caller retries 20 ms later. Windows fetches its bytes and serves the event
// within milliseconds (the event flag stands ~1 ms every 10 s, a retry 20 ms later sees it clear; any read with the flag
// clear starts the count again). A flag that every read showed for more than EC_HAENGT_MS, in at least 3 separate calls
// and with no gap above EC_LUECKE_MS between those reads (longer than a quiet time plus the 5-s read interval), is taken
// as stuck: a waiting byte (e.g. the late answer to a read of ours that timed out) is dropped, an event flag that Windows
// no longer serves is ignored as in 0.6.9. So the EC is locked out for at most one read interval; an emergency (writes
// every second) and the stop (rounds half a second apart) get through after about a second.
#define EC_HAENGT_MS 1000
#define EC_LUECKE_MS (EC_RUHE_MS + 6000)
typedef struct { ULONGLONG seit, zuletzt; int aufrufe, diesmal; } Flagge;
// one read of a flag: set -> counts on (a new call counts once), clear -> starts again
static void flagge(Flagge *f, int an, ULONGLONG now)
{
    if (!an) { f->seit = 0; f->aufrufe = 0; return; }
    if (!f->seit || now - f->zuletzt > EC_LUECKE_MS) { f->seit = now; f->aufrufe = 0; }
    if (!f->diesmal) { f->diesmal = 1; f->aufrufe++; }
    f->zuletzt = now;
}
static int haengt(const Flagge *f, ULONGLONG now) { return f->seit && f->aufrufe >= 3 && now - f->seit > EC_HAENGT_MS; }

static uint8_t ecStatus;   // last status ec_idle read, for the silence report; 0xFF = the EC lock was not available
static int ec_idle(void)
{
    static Flagge obf, sci;
    ULONGLONG now = 0;
    int frei = 0;
    uint8_t s = 0;
    obf.diesmal = sci.diesmal = 0;
    for (int i = 0; i < 2000; i++) {
        s = ecStatus = pin(EC_SC); now = GetTickCount64();
        flagge(&obf, s & EC_OBF, now);
        flagge(&sci, s & EC_SCI, now);
        if ((s & EC_SCI) && !haengt(&sci, now)) return 0;
        if (s & (EC_OBF | EC_IBF)) frei = 0; else if (++frei >= 16) return 1;
    }
    if ((s & EC_OBF) && haengt(&obf, now)) { pin(EC_DATA); flagge(&obf, 0, now); }
    return 0;
}

// EC reads and writes failing for more than EC_STUMM_MS without a success (input buffer never clears, the EC does not
// answer, or another tool holds the EC mutex): reported once with the time since the first failure, an emergency could
// not write then either; reported again after a success in between. A gap above EC_LUECKE_MS between failures (sleep:
// the tick count keeps running) starts the count again.
#define EC_STUMM_MS 30000
static void ec_ergebnis(int ok)
{
    static ULONGLONG fehlSeit, fehlZuletzt; static int gemeldet;
    if (ok) { fehlSeit = 0; gemeldet = 0; return; }
    ULONGLONG now = GetTickCount64();
    if (!fehlSeit || now - fehlZuletzt > EC_LUECKE_MS) fehlSeit = now;
    fehlZuletzt = now;
    if (!gemeldet && !nurLesen && now - fehlSeit > EC_STUMM_MS) {
        gemeldet = 1;
        event2(1, L_("ec_stumm", "EC antwortet seit %d s nicht (Status 0x%02X): Lüfter und Leistungsmodus folgen nicht"), (int)((now - fehlSeit) / 1000), ecStatus);
    }
}

static int ec_read_raw(uint8_t reg)
{
    if (!ec_idle()) return -1; pout(EC_SC, EC_RD);
    if (!ec_wait(EC_IBF, 0)) return -1; pout(EC_DATA, reg);
    if (!ec_wait(EC_OBF, EC_OBF)) return -1; return pin(EC_DATA);
}

// ---------- Windows' own EC query (ETW Microsoft-Windows-Kernel-Acpi, event 7) ----------
// The EC raises SCI_EVT about every 10.07 s (now and then one extra half way) and Windows queries it at once, for ~1 ms
// (measured 08.11.2026). A transaction of ours overlapping that query makes Windows time out (ACPI event 13). Windows
// reports every query as ETW event 7 with an exact time stamp, so the next one is predictable: a thread listens the
// whole time (it sleeps until the kernel hands over an event, a few small events every 10 s), and before each transaction
// acpi_fenster() waits while a query is due, from ACPI_VOR_MS before to ACPI_NACH_MS after (also at half the period).
// ETW delivers late (buffers, flushed every second), but the time stamps are exact. Without a query seen for ACPI_ALT_MS
// (session could not start, other Windows) nothing waits, as in 0.6.11.
static const GUID ACPI_PROV = { 0xC514638F, 0x7723, 0x485B, { 0xBC, 0xFC, 0x96, 0x56, 0x5D, 0x73, 0x5D, 0x4A } };
static wchar_t etwName[] = L"EVO-X2-COOL-ACPI";
#define ACPI_TAKT_MS 10066
#define ACPI_VOR_MS 40
#define ACPI_NACH_MS 25
#define ACPI_ALT_MS 60000
static volatile LONG64 acpiQpc, acpiTakt;   // QPC time stamp of the last query; learned period in QPC ticks
static volatile LONG acpiZahl;
static LARGE_INTEGER qpcHz;
static TRACEHANDLE etwSitzung, etwSpur = INVALID_PROCESSTRACE_HANDLE;

// Diagnosis (does a remaining ACPI 13 come from a query off the beat, or from an access of ours too close to one on the
// beat?): ec_lock() notes the start of each of our transactions, the ETW thread notes for each query its kind and the
// nearest access of ours, the main loop writes the remarkable ones and a summary every 10 minutes into the event list.
#define ACPI_RASTER_MS 30      // a query within this of the beat (full or half period) counts as on the beat
#define ACPI_NAH_MS 10         // an access of ours this close to a query is noted
#define ACPI_NEU_MS 25000      // no query on the beat for this long: the next one becomes the new reference
enum { ABFRAGE_TAKT, ABFRAGE_HALB, ABFRAGE_AUSSER, ABFRAGE_NEU };
static volatile LONG64 zugriffe[64]; static volatile LONG zugriffNr;
typedef struct { LONG64 t; int art; float abwMs, zugriffMs; } Abfrage;
static Abfrage abfragen[32]; static volatile LONG abfrageNr; static LONG abfrageGelesen;

static void WINAPI etw_ereignis(PEVENT_RECORD e)
{
    if (e->EventHeader.EventDescriptor.Id != 7) return;   // 4 events per query (keyword 0x200000000001)
    static LONG64 letzte;
    LONG64 t = e->EventHeader.TimeStamp.QuadPart, f = qpcHz.QuadPart, alt = acpiQpc;
    if (letzte && t - letzte < f / 10) return;   // one query brings several events within a millisecond
    letzte = t;
    // the reference moves only with queries on the beat: one off the beat (the EC's own events) would otherwise shift the
    // predicted windows away from the next regular query for up to 10 s
    int art = ABFRAGE_NEU; float abw = 0;
    if (alt && t - alt <= f * ACPI_NEU_MS / 1000) {
        LONG64 halb = acpiTakt / 2, d = t - alt, n = (d + halb / 2) / halb, rest = d - n * halb;
        abw = (float)rest * 1000 / f;
        if (n > 0 && rest * 1000 < f * ACPI_RASTER_MS && -rest * 1000 < f * ACPI_RASTER_MS) {
            art = n % 2 ? ABFRAGE_HALB : ABFRAGE_TAKT;
            if (n % 2 == 0) { LONG64 takt = acpiTakt; InterlockedExchange64(&acpiTakt, takt + (d * 2 / n - takt) / 4); }
            InterlockedExchange64(&acpiQpc, t);
        } else art = ABFRAGE_AUSSER;
    } else InterlockedExchange64(&acpiQpc, t);
    InterlockedIncrement(&acpiZahl);
    LONG64 best = 0; int gefunden = 0;
    for (int i = 0; i < 64; i++) { LONG64 z = zugriffe[i]; if (z && (!gefunden || _abs64(z - t) < _abs64(best - t))) { best = z; gefunden = 1; } }
    Abfrage *a = &abfragen[abfrageNr % 32];
    a->t = t; a->art = art; a->abwMs = abw; a->zugriffMs = gefunden ? (float)(best - t) * 1000 / f : 1e9f;
    InterlockedIncrement(&abfrageNr);
}

static EVENT_TRACE_PROPERTIES *etw_eigenschaften(void)
{
    static union { EVENT_TRACE_PROPERTIES p; char raum[sizeof(EVENT_TRACE_PROPERTIES) + 256]; } u;
    memset(&u, 0, sizeof u);
    u.p.Wnode.BufferSize = sizeof u; u.p.LoggerNameOffset = sizeof u.p;
    return &u.p;
}

static DWORD WINAPI etw_faden(void *x)
{
    (void)x;
    ControlTraceW(0, etwName, etw_eigenschaften(), EVENT_TRACE_CONTROL_STOP);   // left over from a killed run
    EVENT_TRACE_PROPERTIES *p = etw_eigenschaften();
    p->Wnode.Flags = WNODE_FLAG_TRACED_GUID; p->Wnode.ClientContext = 1;   // time stamps in QPC ticks
    p->LogFileMode = EVENT_TRACE_REAL_TIME_MODE; p->FlushTimer = 1; p->BufferSize = 4; p->MinimumBuffers = 2; p->MaximumBuffers = 4;
    if (StartTraceW(&etwSitzung, etwName, p) != ERROR_SUCCESS) { etwSitzung = 0; return 0; }
    // only event 7 into the small buffers (a burst of other ACPI events could fill them and push out the ones we need);
    // the keyword mask stays open, a narrower one delivered nothing in the test
    EVENT_FILTER_EVENT_ID ids = { TRUE, 0, 1, { 7 } };
    EVENT_FILTER_DESCRIPTOR fd = { (ULONGLONG)(ULONG_PTR)&ids, sizeof ids, EVENT_FILTER_TYPE_EVENT_ID };
    ENABLE_TRACE_PARAMETERS ep = { 0 };
    ep.Version = ENABLE_TRACE_PARAMETERS_VERSION_2; ep.EnableFilterDesc = &fd; ep.FilterDescCount = 1;
    if (EnableTraceEx2(etwSitzung, &ACPI_PROV, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_VERBOSE, ~0ULL, 0, 0, &ep) != ERROR_SUCCESS
        && EnableTraceEx2(etwSitzung, &ACPI_PROV, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_VERBOSE, ~0ULL, 0, 0, 0) != ERROR_SUCCESS) return 0;
    EVENT_TRACE_LOGFILEW lf = { 0 };
    lf.LoggerName = etwName;
    lf.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;   // QPC, not FILETIME
    lf.EventRecordCallback = etw_ereignis;
    etwSpur = OpenTraceW(&lf);
    if (etwSpur != INVALID_PROCESSTRACE_HANDLE) ProcessTrace(&etwSpur, 1, 0, 0);   // returns when the session stops
    return 0;
}

static void etw_start(void)
{
    QueryPerformanceFrequency(&qpcHz);
    acpiTakt = qpcHz.QuadPart * ACPI_TAKT_MS / 1000;
    HANDLE h = CreateThread(0, 0, etw_faden, 0, 0, 0);
    if (h) CloseHandle(h);
}

static void etw_stopp(void)
{
    if (etwSitzung) ControlTraceW(etwSitzung, 0, etw_eigenschaften(), EVENT_TRACE_CONTROL_STOP);
    if (etwSpur != INVALID_PROCESSTRACE_HANDLE) CloseTrace(etwSpur);
}

// waits while Windows' next query is due (at most ACPI_VOR_MS + ACPI_NACH_MS plus a timer tick); checks again after
// each sleep, which can end a tick early or late
static void acpi_fenster(void)
{
    for (int runde = 0; runde < 3; runde++) {
        LONG64 t0 = acpiQpc, halb = acpiTakt / 2, f = qpcHz.QuadPart;
        if (!t0 || halb <= 0) return;
        LARGE_INTEGER jetzt; QueryPerformanceCounter(&jetzt);
        LONG64 seit = jetzt.QuadPart - t0;
        if (seit < 0 || seit > f * ACPI_ALT_MS / 1000) return;
        LONG64 phase = seit % halb, vor = f * ACPI_VOR_MS / 1000, nach = f * ACPI_NACH_MS / 1000, warten = 0;
        if (phase > halb - vor) warten = halb - phase + nach; else if (phase < nach) warten = nach - phase;
        if (warten <= 0) return;
        Sleep((DWORD)(warten * 1000 / f) + 1);
    }
}

// ---------- reading through the BIOS WMI interface ----------
// The BIOS offers root\WMI PowerSwitchInterface (DSDT \AMW0.WMAA); its methods reach the EC fields through Windows' own
// EC driver, which serializes them with Windows' queries, so a read there can never collide (ACPI 13). Only READ
// methods are used: GetHwTemp(1) = EC temperature (0x70), GetFanControl(1) = speeds of both CPU fans (0x35-0x38) in
// one 32-bit value (L1 low word, L2 high word). Measured 13.11.2026: same values as the direct reads, 7-15 ms a call.
// Control stays as it is (curve tables in the EC, written directly; the EC keeps regulating if this service stops);
// the system fan speed (0x28/0x29) is not offered and stays a direct read, once a minute. WMI failing 3 times in a
// row: direct reads as before.
#define COBJMACROS
#include <wbemidl.h>
#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
static IWbemServices *wSvc;
static IWbemClassObject *wCls;
static BSTR wPath;   // the instance, e.g. PowerSwitchInterface.InstanceName="ACPI\PNP0C14\IP3POWERSWITCH_0"
static int wmiOk, wmiFehl;

static int wmi_init(void)
{
    IWbemLocator *loc = 0; IEnumWbemClassObject *en = 0; IWbemClassObject *o = 0; ULONG n = 0; VARIANT v;
    HRESULT hr = CoInitializeEx(0, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return 0;
    CoInitializeSecurity(0, -1, 0, 0, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, 0, EOAC_NONE, 0);
    if (FAILED(CoCreateInstance(&CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER, &IID_IWbemLocator, (void **)&loc))) return 0;
    BSTR ns = SysAllocString(L"ROOT\\WMI"), cls = SysAllocString(L"PowerSwitchInterface");
    hr = IWbemLocator_ConnectServer(loc, ns, 0, 0, 0, 0, 0, 0, &wSvc);
    IWbemLocator_Release(loc);
    if (SUCCEEDED(hr)) CoSetProxyBlanket((IUnknown *)wSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, 0, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, 0, EOAC_NONE);
    if (SUCCEEDED(hr)) hr = IWbemServices_GetObject(wSvc, cls, 0, 0, &wCls, 0);
    if (SUCCEEDED(hr)) hr = IWbemServices_CreateInstanceEnum(wSvc, cls, WBEM_FLAG_FORWARD_ONLY, 0, &en);
    if (SUCCEEDED(hr) && IEnumWbemClassObject_Next(en, 3000, 1, &o, &n) == WBEM_S_NO_ERROR && n == 1) {
        if (SUCCEEDED(IWbemClassObject_Get(o, L"__RELPATH", 0, &v, 0, 0)) && v.vt == VT_BSTR) wPath = v.bstrVal;
        IWbemClassObject_Release(o);
    }
    if (en) IEnumWbemClassObject_Release(en);
    SysFreeString(ns); SysFreeString(cls);
    return wPath != 0;
}

// one read method with one UInt8 input; returns the named output (or -1)
static long long wmi_lesen(const wchar_t *method, const wchar_t *in1, int v1, const wchar_t *out)
{
    IWbemClassObject *def = 0, *in = 0, *res = 0; long long r = -1; VARIANT v;
    BSTR m = SysAllocString(method);
    if (SUCCEEDED(IWbemClassObject_GetMethod(wCls, method, 0, &def, 0)) && def) {
        IWbemClassObject_SpawnInstance(def, 0, &in);
        VariantInit(&v); v.vt = VT_UI1; v.bVal = (BYTE)v1;
        if (in) IWbemClassObject_Put(in, in1, 0, &v, 0);
    }
    if (SUCCEEDED(IWbemServices_ExecMethod(wSvc, wPath, m, 0, 0, in, &res, 0)) && res) {
        VariantInit(&v);
        if (SUCCEEDED(IWbemClassObject_Get(res, out, 0, &v, 0, 0)))
            r = v.vt == VT_I4 ? (long long)(uint32_t)v.lVal : v.vt == VT_UI1 ? v.bVal : v.vt == VT_I2 ? v.iVal : -1;
        VariantClear(&v);
        IWbemClassObject_Release(res);
    }
    if (in) IWbemClassObject_Release(in);
    if (def) IWbemClassObject_Release(def);
    SysFreeString(m);
    return r;
}

// a failed call counts; 3 in a row: back to direct reads for the rest of the run
static void wmi_ergebnis(int ok)
{
    if (ok) { wmiFehl = 0; return; }
    if (++wmiFehl >= 3 && wmiOk) {
        wmiOk = 0;
        event2(1, L_("wmi_weg", "BIOS-Schnittstelle (WMI) antwortet nicht mehr: EC-Temperatur und Drehzahlen wieder direkt"));
    }
}
// EC temperature; 0x7FFFFFFF (the BIOS' answer for an unknown sensor), 0 or above 110 °C count as no reading
// GetHwTemp(1) packs four EC bytes (DSDT): CPUT 0x70 (byte 0, the one used), GPUT 0x71, CPAD 0x72, GPAD 0x73 (all 0 on
// the EVO-X2, 13.11.2026); only byte 0 counts, so a BIOS that fills the others changes nothing
static int wmi_ec_temp(void)
{
    long long v = wmi_lesen(L"GetHwTemp", L"HwTempType", 1, L"Temp");
    int t = v < 0 || v == 0x7FFFFFFF ? -1 : (int)(v & 0xFF), ok = t >= 10 && t <= 110;
    wmi_ergebnis(ok);
    return ok ? t : -1;
}
// speeds of both CPU fans: r[0] = L1, r[1] = L2
static int wmi_rpm(int r[2])
{
    long long v = wmi_lesen(L"GetFanControl", L"FanNumber", 1, L"FanDuty");
    int ok = v >= 0 && v != 0x7FFFFFFF && (v & 0xFFFF) < 10000 && (v >> 16 & 0xFFFF) < 10000;
    wmi_ergebnis(ok);
    if (!ok) return 0;
    r[0] = (int)(v & 0xFFFF); r[1] = (int)(v >> 16 & 0xFFFF);
    return 1;
}

// the window is checked while the mutex is held: another EC program (LibreHardwareMonitor ...) holding it up to 500 ms
// would otherwise move our transaction into the query; it waits at most the window length, as during its own accesses
static int ec_lock(void)
{
    if (WaitForSingleObject(mEc, 500) > WAIT_ABANDONED) return 0;
    acpi_fenster();
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    zugriffe[InterlockedIncrement(&zugriffNr) % 64] = t.QuadPart;
    return 1;
}

static int ec_read(uint8_t reg)
{
    if (ohneEc) return -1;
    for (int t = 0; t < 3; t++) {
        if (ec_lock()) { int v = ec_read_raw(reg); ReleaseMutex(mEc); if (v >= 0) { ec_ergebnis(1); return v; } }
        else ecStatus = 0xFF;
        Sleep(20);
    }
    ec_ergebnis(0);
    return -1;
}

// A changed value counts only when a second read gives the same (a stray byte rarely comes twice); unchanged costs one read.
static int ec_read_sicher(uint8_t reg, int alt)
{
    int v = ec_read(reg);
    if (v < 0 || v == alt) return v;
    return ec_read(reg) == v ? v : -1;
}

// at start: a busy EC gets up to GEDULD_MS (also the time a stuck flag needs to be recognized), tried every
// GEDULD_PAUSE_MS; a stop ends the wait
#define GEDULD_MS 3000
#define GEDULD_PAUSE_MS 200
static int ec_read_geduldig(uint8_t reg)
{
    ULONGLONG ende = GetTickCount64() + GEDULD_MS;
    int v;
    do v = ec_read_sicher(reg, -1);
    while (v < 0 && !ohneEc && GetTickCount64() < ende && WaitForSingleObject(stopEv, GEDULD_PAUSE_MS) != WAIT_OBJECT_0);
    return v;
}

// write with read-back; never in read-only mode
static int ec_write(uint8_t reg, uint8_t val)
{
    if (nurLesen) return 0;
    int gesendet = 0;   // a previous try got as far as sending the value
    for (int t = 0; t < 5; t++) {
        if (ec_lock()) {
            // a retry may only have lost the read-back (an event right after the write): no second write then
            if (gesendet && ec_read_raw(reg) == val) { ReleaseMutex(mEc); ec_ergebnis(1); return 1; }
            int ok = ec_idle(); if (ok) pout(EC_SC, EC_WR);
            ok = ok && ec_wait(EC_IBF, 0); if (ok) pout(EC_DATA, reg);
            ok = ok && ec_wait(EC_IBF, 0); if (ok) { pout(EC_DATA, val); gesendet = 1; }
            ok = ok && ec_wait(EC_IBF, 0) && ec_read_raw(reg) == val;
            ReleaseMutex(mEc);
            if (ok) { ec_ergebnis(1); return 1; }
        } else ecStatus = 0xFF;
        Sleep(20);
    }
    ec_ergebnis(0);
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
#define SystemProcessorPerformanceInformation 8

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

// Windows' "EC did not respond within the timeout" (ACPI event 13) goes into the event list with its own time, so logs
// show it next to the controller's EC accesses. Looked up every 30 s in the system log (last 2 min), each event once.
static void acpi13_pruefen(void)
{
    static ULONGLONG letzte;
    static __declspec(align(8)) BYTE puffer[8192];
    EVT_HANDLE q = EvtQuery(0, L"System", L"*[System[Provider[@Name='ACPI'] and EventID=13 and TimeCreated[timediff(@SystemTime) <= 120000]]]",
                            EvtQueryChannelPath | EvtQueryForwardDirection);
    if (!q) return;
    EVT_HANDLE ctx = EvtCreateRenderContext(0, 0, EvtRenderContextSystem), ev[8];
    DWORD n = 0;
    while (ctx && EvtNext(q, 8, ev, 0, 0, &n)) {
        for (DWORD i = 0; i < n; i++) {
            DWORD used = 0, cnt = 0;
            PEVT_VARIANT v = (PEVT_VARIANT)puffer;
            if (EvtRender(ctx, ev[i], EvtRenderEventValues, sizeof puffer, puffer, &used, &cnt) && v[EvtSystemEventRecordId].UInt64Val > letzte) {
                letzte = v[EvtSystemEventRecordId].UInt64Val;
                ULONGLONG t = v[EvtSystemTimeCreated].FileTimeVal;
                FILETIME ft = { (DWORD)t, (DWORD)(t >> 32) }, lt; SYSTEMTIME st;
                FileTimeToLocalFileTime(&ft, &lt); FileTimeToSystemTime(&lt, &st);
                event(L_("acpi13", "Windows: EC hat nicht geantwortet (ACPI-Ereignis 13) um %02d:%02d:%02d"), st.wHour, st.wMinute, st.wSecond);
            }
            EvtClose(ev[i]);
        }
    }
    if (ctx) EvtClose(ctx);
    EvtClose(q);
}

// the queries noted by the ETW thread: off the beat, or an access of ours closer than ACPI_NAH_MS, one line each (at most
// 20 in 10 minutes, with the time to the millisecond to put them next to an ACPI 13); every 10 minutes a summary
static void abfragen_melden(int zusammenfassen)
{
    static int zahl[4], nah, zeilen; static float minZugriff = 1e9f, maxAbw;
    LARGE_INTEGER jetzt; QueryPerformanceCounter(&jetzt);
    FILETIME ftJetzt; GetSystemTimeAsFileTime(&ftJetzt);
    ULONGLONG uJetzt = ((ULONGLONG)ftJetzt.dwHighDateTime << 32) | ftJetzt.dwLowDateTime;
    LONG bis = abfrageNr;
    if (bis - abfrageGelesen > 32) abfrageGelesen = bis - 32;
    for (; abfrageGelesen < bis; abfrageGelesen++) {
        Abfrage a = abfragen[abfrageGelesen % 32];
        zahl[a.art]++;
        if (a.art <= ABFRAGE_HALB && fabsf(a.abwMs) > maxAbw) maxAbw = fabsf(a.abwMs);
        if (fabsf(a.zugriffMs) < fabsf(minZugriff)) minZugriff = a.zugriffMs;
        int istNah = fabsf(a.zugriffMs) < ACPI_NAH_MS;
        nah += istNah;
        if ((a.art != ABFRAGE_AUSSER && !istNah) || zeilen >= 20) continue;
        zeilen++;
        ULONGLONG u = uJetzt - (ULONGLONG)((jetzt.QuadPart - a.t) * 10000000 / qpcHz.QuadPart);
        FILETIME ft = { (DWORD)u, (DWORD)(u >> 32) }, lt; SYSTEMTIME st;
        FileTimeToLocalFileTime(&ft, &lt); FileTimeToSystemTime(&lt, &st);
        if (a.art == ABFRAGE_AUSSER)
            event(L_("etw_ausser", "EC-Abfrage %02d:%02d:%02d.%03d außer Takt (%+.0f ms), unser nächster Zugriff %+.1f ms"),
                  st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, a.abwMs, a.zugriffMs);
        else
            event(L_("etw_nah", "EC-Abfrage %02d:%02d:%02d.%03d im Takt (%+.1f ms), unser Zugriff nur %+.1f ms daneben"),
                  st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, a.abwMs, a.zugriffMs);
    }
    if (!zusammenfassen) return;
    event(L_("etw_10min", "EC-Abfragen 10 min: %d im Takt, %d halb, %d außer Takt, %d neu; Zugriff nah %d, min %+.1f ms; Takt ±%.1f ms"),
          zahl[ABFRAGE_TAKT], zahl[ABFRAGE_HALB], zahl[ABFRAGE_AUSSER], zahl[ABFRAGE_NEU], nah, minZugriff > 1e8f ? 0.0f : minZugriff, maxAbw);
    memset(zahl, 0, sizeof zahl); nah = zeilen = 0; minZugriff = 1e9f; maxAbw = 0;
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
                                    "Kerne_aktiv;GPU_Last;GPU_C;Prozent;Grund;L1;L2;Sys;Vorhersage_C;Versatz;Felder;Deckel_MHz;Lage;Modus;Regler_Prozent;SSD_C;SSD_Sensor_C;SSD_einzeln;SSD_MBs\r\n";
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
// the chip that adds about 6 °C per watt whatever the fan does.
// Cells without enough experience borrow the nearest learned duty at the same power, then one overall offset.
#define SKALA 2   // model scale in modell.txt; cells learned on another scale (0.1/0.2: other heat estimate) are dropped
#define NP 40
#define ND 11
static struct { float r; int n; } cell[NP][ND];
static float versatz;
static const char *lageName[EVO_NLAGE] = { "wenige", "viele", "grafik", "beide" };   // keys in modell.txt
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
static const char *lage_text(int i)
{
    return i == LAGE_WENIGE ? L_("lage0", "wenige Kerne aktiv") : i == LAGE_VIELE ? L_("lage1", "viele Kerne aktiv")
         : i == LAGE_BEIDE ? L_("lage3", "Grafik + viele Kerne") : L_("lage2", "Grafik arbeitet (oder kühlt danach ab)");
}
// per load type: current cap step, calm seconds earned at that cap, highest Tctl since, and how many hot spots each step
// has caused. A step that caused hot spots is tried again only after a longer calm run: 10 min, 1 h, 1 day.
static struct { int stufe, ruhig, maxT, fehl[NSTUFEN]; } lage[EVO_NLAGE];
static int wartezeit(int fehl) { return fehl <= 0 ? 600 : fehl == 1 ? 3600 : 86400; }

static int pk(float p) { int k = (int)lroundf(p / 10); return k < 0 ? 0 : k >= NP ? NP - 1 : k; }
static int dk(float d) { int k = (int)lroundf(d / 10); return k < 0 ? 0 : k >= ND ? ND - 1 : k; }
static float prior(float p, float d, float m) { return 34 + p * (0.58f - 0.30f * d / 100) + 6 * m; }

// 0.6.11: limits, so one broken reading can never poison the model (.56: field 8 7 = -33257 from 06.11., fan 70 % for nothing)
#define REST_MAX 25.0f          // a learned field is at most this far from the prior
#define T_MIN 20.0f             // plausible end temperatures / predictions
#define T_MAX 110.0f
static float grenze(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static float predict(float p, float d, float m)
{
    int a = pk(p), b = dk(d);
    float v;
    if (cell[a][b].n >= 20) v = prior(p, d, m) + cell[a][b].r;
    else {
        int best = -1; float bd = 1e9f;
        for (int j = 0; j < ND; j++) if (cell[a][j].n >= 20 && fabsf(j * 10 - d) < bd) { bd = fabsf(j * 10 - d); best = j; }
        v = prior(p, d, m) + (best >= 0 ? cell[a][best].r : versatz);
    }
    return isfinite(v) ? grenze(v, T_MIN, T_MAX) : prior(p, d, m);
}

static void learn(float p, float d, float m, float y)
{
    if (!isfinite(y) || y < T_MIN || y > T_MAX || !isfinite(p) || !isfinite(d)) return;   // implausible reading: not learned
    float res = grenze(y - prior(p, d, m), -REST_MAX, REST_MAX);
    versatz += (res - versatz) / 60;
    int a = pk(p), b = dk(d);
    if (cell[a][b].n == 0) cell[a][b].r = versatz;
    cell[a][b].n++;
    cell[a][b].r += (res - cell[a][b].r) / (cell[a][b].n < 60 ? cell[a][b].n : 60);
}

static int felder(void) { int n = 0; for (int a = 0; a < NP; a++) for (int b = 0; b < ND; b++) n += cell[a][b].n > 0; return n; }

static int stufe_von(int mhz) { int i = 0; while (i + 1 < NSTUFEN && stufen[i + 1] <= mhz) i++; return i; }

// the learned values the viewer shows
static void publish_model(void)
{
    if (!sh) return;
    sh->versatz = versatz; sh->felder = felder();
    for (int i = 0; i < EVO_NLAGE; i++) sh->deckel[i] = stufen[lage[i].stufe];
}

static void load_model(void)
{
    wchar_t p[MAX_PATH]; char line[128], name[16]; int a, b, n, m; float r;
    lage[LAGE_WENIGE].stufe = stufe_von(4200); lage[LAGE_VIELE].stufe = stufe_von(3600); lage[LAGE_GRAFIK].stufe = stufe_von(3600);
    lage[LAGE_BEIDE].stufe = stufe_von(3300);
    swprintf(p, MAX_PATH, L"%s\\modell.txt", dataDir);
    FILE *f = _wfopen(p, L"r");
    if (!f) return;
    int skala = 1, verworfen = 0;   // files without the line come from 0.1/0.2
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "skala %d", &m) == 1) skala = m;
        else if (sscanf(line, "versatz %f", &r) == 1) { if (skala == SKALA) { if (isfinite(r) && fabsf(r) <= REST_MAX) versatz = r; else { versatz = 0; verworfen++; } } }
        else if (sscanf(line, "feld %d %d %f %d", &a, &b, &r, &n) == 4 && a >= 0 && a < NP && b >= 0 && b < ND) {
            // a field outside the limits (broken reading in an older version) is dropped and learned anew
            if (skala == SKALA && isfinite(r) && fabsf(r) <= REST_MAX && n > 0) { cell[a][b].r = r; cell[a][b].n = n; }
            else if (skala == SKALA) verworfen++;
        }
        else if (sscanf(line, "deckel %15s %d", name, &m) == 2) { for (int i = 0; i < EVO_NLAGE; i++) if (!strcmp(name, lageName[i])) lage[i].stufe = stufe_von(m); }
        else if (sscanf(line, "fehl %15s %d %d", name, &m, &n) == 3) { for (int i = 0; i < EVO_NLAGE; i++) if (!strcmp(name, lageName[i])) lage[i].fehl[stufe_von(m)] = n; }
    }
    fclose(f);
    if (skala != SKALA) event(L_("skala", "Neues Wärmemodell (ganze Chipleistung): Lüfter-Erfahrung wird neu gelernt, Taktgrenzen bleiben"));
    if (verworfen) event(L_("feld_verworfen", "Wärmemodell: %d unplausible Bereiche verworfen, sie werden neu gelernt"), verworfen);
}

static void save_model(void)
{
    wchar_t p[MAX_PATH], tmp[MAX_PATH];
    swprintf(p, MAX_PATH, L"%s\\modell.txt", dataDir); swprintf(tmp, MAX_PATH, L"%s\\modell.tmp", dataDir);
    FILE *f = _wfopen(tmp, L"w");
    if (!f) return;
    fprintf(f, "skala %d\nversatz %.2f\n", SKALA, versatz);
    for (int i = 0; i < EVO_NLAGE; i++) fprintf(f, "deckel %s %d\n", lageName[i], stufen[lage[i].stufe]);
    for (int i = 0; i < EVO_NLAGE; i++) for (int s = 0; s < NSTUFEN; s++) if (lage[i].fehl[s]) fprintf(f, "fehl %s %d %d\n", lageName[i], stufen[s], lage[i].fehl[s]);
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

// ---------- SSDs ----------
// The drives are opened once (access 0: only queries) and shared by the temperature and the data rate; a drive added
// while the controller runs is picked up at its next start.
static HANDLE drive[NDRIVES];
static LONGLONG ioAlt[NDRIVES];   // bytes moved per drive at the last call, -1 = none yet
static void drives_open(void)
{
    for (int i = 0; i < NDRIVES; i++) {
        ioAlt[i] = -1;
        wchar_t p[32]; swprintf(p, 32, L"\\\\.\\PhysicalDrive%d", i);
        drive[i] = CreateFileW(p, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
    }
}

// Hottest composite temperature of all drives (°C, -1 = unknown; the value the drive's own limits refer to); *sensor gets
// the hottest single sensor. A Samsung 990 PRO reports its controller as a second sensor, up to 25 °C above the composite.
// Each drive goes to ssdEinzeln as "disk:composite/sensor" for the log.
static char ssdEinzeln[96];
static int ssd_temp(int *sensor)
{
    int best = -1, len = 0; *sensor = -1; ssdEinzeln[0] = 0;
    for (int i = 0; i < NDRIVES; i++) {
        if (drive[i] == INVALID_HANDLE_VALUE) continue;
        STORAGE_PROPERTY_QUERY q = { StorageDeviceTemperatureProperty, PropertyStandardQuery };
        union { STORAGE_TEMPERATURE_DATA_DESCRIPTOR d; BYTE b[256]; } o; DWORD n;
        if (DeviceIoControl(drive[i], IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, &o, sizeof o, &n, 0) && n >= sizeof o.d && o.d.InfoCount > 0) {
            int c = o.d.TemperatureInfo[0].Temperature, s = -1;
            if (c > best && c < 120) best = c;
            int nInfo = (n - offsetof(STORAGE_TEMPERATURE_DATA_DESCRIPTOR, TemperatureInfo)) / sizeof(STORAGE_TEMPERATURE_INFO);
            for (int j = 0; j < o.d.InfoCount && j < nInfo; j++) {
                int t = o.d.TemperatureInfo[j].Temperature;
                if (t > s && t < 150) s = t;
            }
            if (s > *sensor) *sensor = s;
            if (len < (int)sizeof ssdEinzeln - 16)
                len += snprintf(ssdEinzeln + len, sizeof ssdEinzeln - len, "%s%d:%d/%d", len ? " " : "", i, c, s);
        }
    }
    return best;
}

// Data moved by all drives together in MB/s since the last call (every second). Known at once, so the system fan can
// start before a drive is warm (its temperature comes too late).
static double ssd_io(void)
{
    static ULONGLONG tAlt;
    ULONGLONG t = GetTickCount64(); double sek = tAlt ? (t - tAlt) / 1000.0 : 0; tAlt = t;
    double mb = 0;
    for (int i = 0; i < NDRIVES; i++) {
        DISK_PERFORMANCE dp; DWORD n;
        if (drive[i] == INVALID_HANDLE_VALUE || !DeviceIoControl(drive[i], IOCTL_DISK_PERFORMANCE, 0, 0, &dp, sizeof dp, &n, 0)) continue;
        LONGLONG b = dp.BytesRead.QuadPart + dp.BytesWritten.QuadPart;
        if (ioAlt[i] >= 0 && b >= ioAlt[i] && sek > 0) mb += (b - ioAlt[i]) / 1e6 / sek;
        ioAlt[i] = b;
    }
    return mb;
}

// ---------- fan curves inside the EC ----------
// Each block of 6 registers: [0] mode (1 = this curve active, written last), [1] 20 and [5] 2 (constants as the BIOS
// writes them), [2] start °C, [3] full °C, [4] duty % at start; linear from start to 100 % at full.
// The CPU curve is anchored 3 °C below the smoothed EC temperature with duty d - 6 at the start, so today's temperature
// lies on the rising part (+2 % per °C) and gets duty d. The system fan gets sysWant; 0 = off, written as a curve that
// starts only at 85 °C, so the EC still turns it on if the controller dies.
enum { K_MODUS, K_KONST20, K_START, K_VOLL, K_PWM, K_KONST2 };
// Curves go out 2 registers a second (emergency, yellow, and the BIOS values at start and stop: all at once), checks 1
// register a second; the quiet time (EC_RUHE_MS) follows every completed write, also the BIOS values going back.
static ULONGLONG ecRuheBis;
static int ec_ruhig(void) { return GetTickCount64() < ecRuheBis; }
static void ec_ruhe(void) { ecRuheBis = GetTickCount64() + EC_RUHE_MS; }

static int curveD = -1, sysWant = -1, curveSys = -1; static float curveAnchor = -1;   // curveD -1 = BIOS curve
static int curveReg[12] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };   // what the EC holds (-1 = unknown)
static int curveZiel[12], curveOffen, pruefNr = -1;                             // what it should hold, registers pending
static ULONGLONG schreibBis;   // start/stop: no new curve write after this time (0 = no limit)

// writes up to max pending registers in a fixed order per block: the mode byte last; start and full in the same call
// (the budget takes both or neither), rising full first, falling start first, so full never lies below start for longer
// than the pair takes. Once a register of a block has to wait (budget used up, the start/stop deadline passed, or its
// write failed), the rest of that block waits as well, so the order holds and the mode byte is never set over half a
// curve. Quiet time once everything is written.
static void curve_schritt(int max)
{
    static const int steigend[6] = { K_KONST20, K_VOLL, K_START, K_PWM, K_KONST2, K_MODUS };
    static const int fallend[6] = { K_KONST20, K_START, K_VOLL, K_PWM, K_KONST2, K_MODUS };
    static const struct { int von, n; } gruppe[5] = { { 0, 1 }, { 1, 2 }, { 3, 1 }, { 4, 1 }, { 5, 1 } };   // start + full together
    int geschrieben = 0;
    curveOffen = 0;
    for (int b = 0; b <= 6; b += 6) {
        const int *order = curveReg[b + K_START] >= 0 && curveZiel[b + K_START] > curveReg[b + K_START] ? steigend : fallend;
        int warten = 0;
        for (int g = 0; g < 5; g++) {
            int offen = 0;
            for (int t = 0; t < gruppe[g].n; t++) { int i = b + order[gruppe[g].von + t]; offen += curveReg[i] != curveZiel[i]; }
            if (!offen) continue;
            if (warten || geschrieben + offen > max) { curveOffen += offen; warten = 1; continue; }
            for (int t = 0; t < gruppe[g].n; t++) {
                int i = b + order[gruppe[g].von + t];
                if (curveReg[i] == curveZiel[i]) continue;
                if (schreibBis && GetTickCount64() > schreibBis) warten = 1;
                if (warten) { curveOffen++; continue; }
                geschrieben++;
                if (ec_write((uint8_t)(REG_KURVE + i), (uint8_t)curveZiel[i])) curveReg[i] = curveZiel[i];
                else { curveReg[i] = -1; warten = 1; curveOffen++; }
            }
        }
    }
    if (geschrieben && !curveOffen) ec_ruhe();
}
// check against an EC reset (after sleep): one register a second, read twice when it differs. A difference means the
// whole block is not ours any more: it is written again completely, in order, mode byte last.
static void curve_pruefen(void)
{
    int v = ec_read_sicher((uint8_t)(REG_KURVE + pruefNr), curveReg[pruefNr]);
    if (v >= 0 && v != curveReg[pruefNr]) {
        int b = pruefNr < 6 ? 0 : 6;
        for (int j = 0; j < 6; j++) curveReg[b + j] = -1;
        curveOffen++;
    }
    if (++pruefNr >= 12) pruefNr = -1;
}
static void write_curve(int d, float teSmooth, int max)
{
    int start = (int)floorf(teSmooth) - 3; start = start < 25 ? 25 : start > 92 ? 92 : start;
    int pwm = d - 6 < 0 ? 0 : d - 6;
    int full = start + (100 - pwm + 1) / 2 + 1; if (full > 105) full = 105;
    int s = sysWant < 0 ? d : sysWant, spwm = s - 6 < 0 ? 0 : s - 6;
    int sfull = start + (100 - spwm + 1) / 2 + 1; if (sfull > 105) sfull = 105;
    int val[12] = { 1, 20, start, full, pwm, 2, 1, 20, s ? start : 85, s ? sfull : 105, s ? spwm : 0, 2 };
    memcpy(curveZiel, val, sizeof val);
    curveD = d; curveSys = s; curveAnchor = teSmooth;
    curve_schritt(max);
}

// duty % the written curve gives at EC temperature t: pwm up to start, then linear to 100 % at full; -1 = BIOS curve.
// b = 0: CPU fans, b = 6: system fan
static int curve_duty(int t, int b)
{
    int s = curveReg[b + K_START], f = curveReg[b + K_VOLL], p = curveReg[b + K_PWM];
    if (curveD < 0 || s < 0 || f <= s || p < 0) return -1;
    return t <= s ? p : t >= f ? 100 : p + (100 - p) * (t - s) / (f - s);
}

// the BIOS values become the target; curve_schritt writes them (with retries) like any curve
static void bios_als_ziel(const uint8_t orig[12])
{
    for (int i = 0; i < 12; i++) curveZiel[i] = orig[i];
    curveD = curveSys = -1; curveAnchor = -1; pruefNr = -1;
    curveOffen = 12;   // all pending until curve_schritt has compared them
}
// at start and stop: write the BIOS values back now, every register whatever the cache says (the EC may have been reset
// or changed since the last check); every pending register per round (a failed one holds back the rest of its block),
// up to BIOS_RUNDEN rounds half a stuck time apart, so a flag stuck just before the stop is recognized in time; no new
// write after BIOS_FRIST_MS (one in flight takes up to 2.6 s when another tool holds the EC lock), so with the mode
// write after it the stop stays within the 15 s Windows waits for the service at shutdown (SERVICE_PRESHUTDOWN_INFO)
#define BIOS_RUNDEN 5
#define BIOS_FRIST_MS 6000
static int bios_zurueck_jetzt(const uint8_t orig[12])
{
    schreibBis = GetTickCount64() + BIOS_FRIST_MS;
    bios_als_ziel(orig);
    for (int i = 0; i < 12; i++) curveReg[i] = -1;
    for (int t = 0; t < BIOS_RUNDEN && curveOffen && GetTickCount64() < schreibBis; t++) { if (t) Sleep(EC_HAENGT_MS / 2); curve_schritt(12); }
    schreibBis = 0;
    return !curveOffen;
}

static int cmp_float(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return (x > y) - (x < y); }

// ---------- the controller ----------
#define PRUEF_SEK 16
#define MODUS_SEK 47
// Everything the loop keeps from one second to the next.
// Schedule within the loop (k = seconds): EC temperature k % 5 == 0; fan speeds k % 30 == 1-4, 6-9, 11; SSD temperature
// k % 5 == 1; ACPI 13 lookup k % 30 == 25; settings check k % 5 == 0 (file time only); curve check
// k % 300 == PRUEF_SEK, then one register a second (17-28); power mode k % 300 == MODUS_SEK (or the first second after a
// quiet time); model saved and shown k % 60 == 0.
// So at most one EC read a second besides the temperature, as long as no quiet time pushes the check or the mode read
// onto other reads (a changed value is read twice).
typedef struct {
    int k;                                  // seconds since start
    // measuring
    NTQSI ntqsi; double joule; LARGE_INTEGER freq, tPkg; double pkgW;
    uint32_t eCore[MAXCPU], ePkg; uint64_t aperf[MAXCPU]; double coreW[MAXCPU];
    LARGE_INTEGER tSeen[MAXCPU]; unsigned seen[MAXCPU]; ProcPerf pp[MAXCPU], pp0[MAXCPU];
    int ec[7];                              // EC values: temperature, then speed hi/lo of L1, L2 and the system fan
    int gW, gL, gT, aW;                     // GPU power, load, temperature, whole-chip power (ADL, -1 = unknown)
    // smoothing
    float hist[10]; int nHist;
    float emaT, slope, pFast, pSlow, mFast, mSlow, emaTe;
    // load type and clock brake
    int capNow, sicherBis, sicherCap, grafikSeit, grafikBis, nachlauf, lageNow, locker;
    // EC power mode (-1 = not controlled: unknown at start); modusWill = last mode we tried to write; modusFaellig = the
    // 5-min re-read is due (waits for the end of a quiet time)
    int modusOrig, modusJetzt, modusWill, modusLocker, modusFaellig;
    // CPU fans
    float d, boost, h1, h2; int leer, hot, lowSince, warNotfall, nachNotfall, dauerSek, dauer, sprungHalt, gelb, warGelb;
    // system fan
    int ssdT, ssdS, sysAus, sysLeer, sysSeit, ioSek; float ioHalt; double io;
    int console;
} Regler;

// one second's measurements
typedef struct {
    double load, sumW, maxW, maxMhz; int busy;
    float T, G, heat, pv;   // Tctl, GPU power, heat power, predicted heat power
    float pAlt;             // fast heat EMA before this second's update
    int Te;                 // EC temperature
} Messung;

static void messen(Regler *r, Messung *m)
{
    static AdlPmLog gl;
    int k = r->k;
    double coreLoad[MAXCPU] = { 0 }, coreBusy[MAXCPU] = { 0 }, thrBusy[MAXCPU] = { 0 };
    m->load = 0; m->busy = 0;
    if (r->ntqsi && r->ntqsi(SystemProcessorPerformanceInformation, r->pp, sizeof r->pp, 0) >= 0) {
        double sb = 0, st = 0;
        for (int c = 0; c < nThreads; c++) {
            double idle = (double)(r->pp[c].idle.QuadPart - r->pp0[c].idle.QuadPart);
            double tot = (double)(r->pp[c].kernel.QuadPart - r->pp0[c].kernel.QuadPart + r->pp[c].user.QuadPart - r->pp0[c].user.QuadPart);
            if (tot > 0) { thrBusy[c] = 1 - idle / tot; sb += tot - idle; st += tot; }
            coreLoad[coreOf[c]] += thrBusy[c] / threadsOfCore[coreOf[c]];
            if (thrBusy[c] > coreBusy[coreOf[c]]) coreBusy[coreOf[c]] = thrBusy[c];
        }
        if (st > 0) m->load = 100 * sb / st;
        for (int i = 0; i < nCores; i++) m->busy += coreLoad[i] > 0.5;
        memcpy(r->pp0, r->pp, sizeof r->pp);
    }
    // APERF and core energy need the thread on that CPU; only the first thread of each core is visited and idle cores are
    // skipped (each at least every 10 s), their counters then cover a longer span, which keeps the averages exact
    m->maxMhz = 0;
    for (int c = 0; c < nThreads; c++) {
        if (!firstOfCore[c] || (k && coreBusy[coreOf[c]] <= 0.03 && k - r->seen[c] < 10)) continue;
        SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << c);
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        double span = (double)(now.QuadPart - r->tSeen[c].QuadPart) / r->freq.QuadPart;
        uint64_t a = 0, e = 0;
        if (msr(MSR_APERF, &a)) { if (k && a > r->aperf[c]) { double mhz = (a - r->aperf[c]) / span / 1e6; if (mhz > m->maxMhz) m->maxMhz = mhz; } r->aperf[c] = a; }
        if (msr(MSR_CORE_ENERGY, &e)) { if (k) r->coreW[coreOf[c]] = (uint32_t)((uint32_t)e - r->eCore[c]) * r->joule / span; r->eCore[c] = (uint32_t)e; }
        r->tSeen[c] = now; r->seen[c] = k;
    }
    // package energy always on CPU 0 and timed right at the read; a step backwards or an implausible value keeps the last
    // reading (on another CPU it jumped between 40 and 130 W at a steady 85 W, a wrap gave ~65000 W)
    SetThreadAffinityMask(GetCurrentThread(), 1);
    uint64_t e = 0; LARGE_INTEGER tp; QueryPerformanceCounter(&tp);
    if (msr(MSR_PKG_ENERGY, &e)) {
        uint32_t de = (uint32_t)e - r->ePkg; double sp = (double)(tp.QuadPart - r->tPkg.QuadPart) / r->freq.QuadPart;
        if (k && de < 0x80000000u && sp > 0.2 && de * r->joule / sp < 400) r->pkgW = de * r->joule / sp;
        r->ePkg = (uint32_t)e; r->tPkg = tp;
    }
    SetThreadAffinityMask(GetCurrentThread(), ((DWORD_PTR)1 << nThreads) - 1);
    m->sumW = 0; m->maxW = 0;
    for (int i = 0; i < nCores; i++) { m->sumW += r->coreW[i]; if (r->coreW[i] > m->maxW) m->maxW = r->coreW[i]; }

    m->T = -1;
    if (WaitForSingleObject(mPci, 50) <= WAIT_ABANDONED) {
        int64_t a = SMN_TCTL, v = 0;
        if (pio_exec(hCpu, "ioctl_read_smn", &a, 1, &v, 1)) {
            uint32_t x = (uint32_t)v;
            m->T = (x >> 21) * 0.125f;
            if ((x & 0x80000) || (x & 0x30000) == 0x30000) m->T -= 49;
        }
        ReleaseMutex(mPci);
    }
    // as few EC reads as possible: EC temperature every 5 s (second emergency path and the slow curve anchor; Tctl comes
    // every second), fan speeds every 30 s for the display, one register a second, never during the quiet time; a value
    // that is not confirmed or impossible (case above 110 °C) keeps the last reading. A speed is two bytes read a second
    // apart: high, low, high again (seconds 1-4, 6-9, 11); the pair counts only if both high bytes match, else a speed
    // crossing a multiple of 256 in between gives a value off by up to 255 rpm.
    // With WMI (see wmi_init) the EC temperature and both CPU fans come from there (fans in second 1 of 30), only the
    // system fan is read directly, once a minute.
    if (k % 5 == 0 && !ec_ruhig()) {
        int v = wmiOk ? wmi_ec_temp() : -1;
        if (v < 0 && !wmiOk) v = ec_read_sicher(REG_TEMP, r->ec[0]);
        if (v >= 0 && v <= 110) r->ec[0] = v;
    }
    if (wmiOk && k % 30 == 1 && !ec_ruhig()) {
        int rpm[2];
        if (wmi_rpm(rpm)) { r->ec[1] = rpm[0] >> 8; r->ec[2] = rpm[0] & 0xFF; r->ec[3] = rpm[1] >> 8; r->ec[4] = rpm[1] & 0xFF; }
    }
    static const uint8_t sek[9] = { 1, 2, 3, 4, 6, 7, 8, 9, 11 }, regs[3] = { REG_L1, REG_L2, REG_SYS };
    static int hoch[3] = { -1, -1, -1 }, tief[3] = { -1, -1, -1 };
    for (int i = 0; i < 9; i++) {
        if (k % 30 != sek[i] || ec_ruhig()) continue;
        if (wmiOk && (i < 6 || k % 60 >= 30)) continue;   // CPU fans through WMI; system fan every other cycle
        int l = i / 3, schritt = i % 3;
        if (schritt == 0) { hoch[l] = ec_read(regs[l]); tief[l] = -1; }   // a step skipped in the quiet time leaves -1:
        else if (schritt == 1) tief[l] = hoch[l] < 0 ? -1 : ec_read_sicher((uint8_t)(regs[l] + 1), r->ec[2 + 2 * l]);  // no mixing with bytes
        else {                                                                 // from 30 s before; low byte confirmed
            int v = hoch[l] < 0 || tief[l] < 0 ? -1 : ec_read(regs[l]);
            if (v >= 0 && v == hoch[l]) { r->ec[1 + 2 * l] = v; r->ec[2 + 2 * l] = tief[l]; }
            hoch[l] = tief[l] = -1;
        }
    }
    m->Te = r->ec[0];
    // GPU every second while it works, every 2 s while it rests
    if (adlIdx >= 0 && (k % 2 == 0 || r->gW > 5) && adlPm(adlCtx, adlIdx, &gl) == 0) {
        r->gW = gl.s[PM_GFX_POWER].supported ? gl.s[PM_GFX_POWER].value : -1;
        r->gL = gl.s[PM_GFX_ACTIVITY].supported ? gl.s[PM_GFX_ACTIVITY].value : -1;
        r->gT = gl.s[PM_TEMP_GFX].supported ? gl.s[PM_TEMP_GFX].value : -1;
        r->aW = gl.s[PM_ASIC_POWER].supported ? gl.s[PM_ASIC_POWER].value : -1;
    }
    m->G = r->gW >= 0 ? (float)r->gW : (float)fmax(0, r->pkgW - m->sumW - 15);   // fallback: package minus cores minus ~15 W SoC base
    // heat power: the whole chip as the GPU driver reports it, else the package counter, else an estimate
    m->heat = r->aW > 0 ? (float)r->aW : r->pkgW > 0 ? (float)r->pkgW : (float)(1.1 * m->sumW + 0.6 * m->G + 15);
}

// Tctl: 10-s median against boost spikes, then EMA; heat power and hottest core fast (3 s) and slow (20 s)
static void glaetten(Regler *r, Messung *m)
{
    if (m->T < 0) m->T = r->emaT;
    if (r->nHist < 10) r->hist[r->nHist++] = m->T; else { memmove(r->hist, r->hist + 1, 9 * sizeof *r->hist); r->hist[9] = m->T; }
    float sorted[10]; memcpy(sorted, r->hist, r->nHist * sizeof *r->hist); qsort(sorted, r->nHist, sizeof *sorted, cmp_float);
    float med = sorted[r->nHist / 2];
    if (r->k == 1) { r->emaT = m->T; r->pFast = r->pSlow = m->heat; r->mFast = r->mSlow = (float)m->maxW; r->emaTe = (float)m->Te; }   // start from real values
    float prevT = r->emaT;
    r->emaT += (med - r->emaT) / 8;
    r->slope += ((r->emaT - prevT) - r->slope) / 10;
    m->pAlt = r->pFast;
    r->pFast += (m->heat - r->pFast) / 3; r->pSlow += (m->heat - r->pSlow) / 20;
    r->mFast += ((float)m->maxW - r->mFast) / 3; r->mSlow += ((float)m->maxW - r->mSlow) / 20;
    if (m->Te >= 0) r->emaTe += (m->Te - r->emaTe) / 10;   // the EC temperature jumps by 15 °C within seconds; the anchor follows slowly
}

static void einstellungen_pruefen(Regler *r, FILETIME *iniSeen)
{
    FILETIME t = ini_time();
    if (!CompareFileTime(&t, iniSeen)) return;
    *iniSeen = t;
    unsigned old = load_settings();
    for (int i = 0; i < EVO_NFUNK; i++)
        if ((old ^ fn) >> i & 1) event(fn >> i & 1 ? L_("f_an", "%s eingeschaltet") : L_("f_aus", "%s ist ausgeschaltet"), funk_text(i));
    if (old == fn) event(L_("ini", "Einstellungen neu gelesen: Ziel %.0f °C, Achtung %d °C, Alarm %d °C, Lüfter %d–%d %%, Taktbremse ab %d °C"), ziel, achtung, alarm, minP, maxP, spitze);
    if (!nurLesen && (old & F_TAKT) && !(fn & F_TAKT) && write_cap(0)) r->capNow = 0;   // off = no clock limit at all
}

// load type: GPU (also its pause under a minute and the after-run while still warm), GPU + many cores, many cores, few.
// A type with a lower cap counts at once, a higher or equal cap only after 60 s / 10 s, so loads that come and go do not
// flip the cap and the EC mode every few seconds.
static int lastart(Regler *r, const Messung *m)
{
    int k = r->k;
    if (m->G > 20) { if (r->grafikSeit < 0) r->grafikSeit = k; r->grafikBis = k + 60; }
    else if (r->grafikSeit >= 0) { if (k - r->grafikSeit >= 30) r->nachlauf = 1; r->grafikSeit = -1; }
    if (r->nachlauf && r->emaT < 65) r->nachlauf = 0;
    float kernW = r->lageNow == LAGE_BEIDE ? 10.0f : 15.0f;   // cores with real power: in above 15 W, out below 10 W
    int lg = m->G > 20 || r->nachlauf || k < r->grafikBis ? (m->busy > 4 && (m->G > 20 || m->sumW > kernW) ? LAGE_BEIDE : LAGE_GRAFIK)
           : m->busy > 4 ? LAGE_VIELE : LAGE_WENIGE;
    if (r->lageNow >= 0 && lg != r->lageNow && lage[lg].stufe >= lage[r->lageNow].stufe
        && ++r->locker < (lage[lg].stufe > lage[r->lageNow].stufe ? 60 : 10)) lg = r->lageNow;
    else r->locker = 0;
    if (lg != r->lageNow) { lage[lg].maxT = (int)m->T; if (r->lageNow >= 0 && !nurLesen) event(L_("lastart", "Lastart jetzt: %s"), lage_text(lg)); r->lageNow = lg; }
    else if (m->T > lage[lg].maxT) lage[lg].maxT = (int)m->T;
    return lg;
}

// clock brake: a hot spot drops to the lowest step at once for 30 s (also across load type changes) and costs this load
// type one step; a step is tried again after a calm run at the cap that gets longer with each hot spot it caused
static void taktbremse(Regler *r, const Messung *m, int lg)
{
    if (nurLesen || !(fn & F_TAKT)) return;
    int k = r->k;
    if (m->T >= spitze) {
        if (k > r->sicherBis && lage[lg].stufe > unten[lg] && r->capNow - taktPlus > stufen[unten[lg]]) {
            int s = lage[lg].stufe;
            if (lage[lg].fehl[s] < 9) lage[lg].fehl[s]++;
            lage[lg].stufe--;
            event2(1, L_("spitze_runter", "Spitze %.0f °C bei „%s“: 30 s Taktbremse %d MHz, für diese Lastart künftig %d MHz"), m->T, lage_text(lg), stufen[unten[lg]], stufen[lage[lg].stufe]);
            event(L_("spitze_warte", "%d MHz erst wieder nach %d min Last ohne Spitze"), stufen[s], wartezeit(lage[lg].fehl[s]) / 60);
        } else if (k > r->sicherBis)
            event2(1, L_("spitze_unten", "Spitze %.0f °C bei „%s“, Taktbremse stand schon auf %d MHz"), m->T, lage_text(lg), r->capNow);
        if (k > r->sicherBis || stufen[unten[lg]] < r->sicherCap) r->sicherCap = stufen[unten[lg]];
        r->sicherBis = k + 30; lage[lg].ruhig = 0;
    }
    // only seconds in which a core really runs at the cap count as calm (idle says nothing about whether more is safe)
    if (k > r->sicherBis && lage[lg].stufe < NSTUFEN - 1) {
        if (lage[lg].maxT >= 80) { lage[lg].ruhig = 0; lage[lg].maxT = (int)m->T; }
        else if (r->capNow > 0 && m->maxMhz >= r->capNow - 250) lage[lg].ruhig++;
        int s = lage[lg].stufe + 1, w = wartezeit(lage[lg].fehl[s]);
        if (lage[lg].ruhig >= w) {
            lage[lg].stufe = s; lage[lg].ruhig = 0;
            event(L_("ruhig", "%d min am Limit ohne Spitze bei „%s“: probiert %d MHz"), w / 60, lage_text(lg), stufen[s]);
        }
    }
    int want = (k <= r->sicherBis ? (r->sicherCap < stufen[unten[lg]] ? r->sicherCap : stufen[unten[lg]]) : stufen[lage[lg].stufe]) + taktPlus;
    if (want != r->capNow && write_cap(want)) { event(L_("takt", "Taktbremse %d → %d MHz%s"), r->capNow, want, k <= r->sicherBis ? L_("takt_spitze", " (nach Hitzespitze)") : ""); r->capNow = want; }
}

// EC power mode "leise" (54 W) while many cores work and the GPU rests; at once, back only after 2 min (short GPU blips
// must not flip the power limit)
static void ec_modus(Regler *r, const Messung *m, int lg)
{
    if (nurLesen || r->modusOrig < 0) return;
    int modus = (fn & F_LEISE) && lg == LAGE_VIELE && m->G < 5 ? MODUS_LEISE : r->modusOrig;
    if (modus == r->modusJetzt || modus == MODUS_LEISE) r->modusLocker = 0;
    else if (r->modusJetzt == MODUS_LEISE && ++r->modusLocker < MODUS_HALT_S) modus = MODUS_LEISE;
    if (modus == r->modusJetzt || ec_ruhig()) return;
    r->modusWill = modus;
    if (ec_write(REG_MODUS, (uint8_t)modus)) {
        ec_ruhe();
        event(L_("modus", "EC-Leistungsmodus → %s"), modus == MODUS_LEISE ? L_("modus2", "leise (54 W)") : modus == 1 ? L_("modus1", "Leistung") : L_("modus0", "ausgewogen"));
        r->modusJetzt = modus;
    }
}

// CPU fan duty: the quietest duty whose predicted end temperature stays below the target, plus feedback, floors and the
// warning stages; returns the reason (GRUND_*) and moves r->d towards it
static int cpu_luefter(Regler *r, Messung *m, int lg)
{
    int k = r->k;
    // prediction: rising power counts ahead (lead of the fast over the slow EMA), only while it still rises and never
    // beyond the measured power; switched off: current power only
    float lead = (fn & F_VORHERSAGE) && r->pFast - m->pAlt > 0.5f ? fmaxf(0, r->pFast - r->pSlow) : 0;
    m->pv = fmaxf(r->pFast, r->pSlow) + fminf(lead, fmaxf(0, m->heat - fmaxf(r->pFast, r->pSlow)));
    // idle and far below the target: the lower idle floor, with a gap between on and off
    if (r->emaT < ziel - 25 && r->pSlow < 30) r->leer = 1; else if (r->emaT > ziel - 20 || r->pSlow > 40) r->leer = 0;
    int lo = r->leer ? minLeer : minP, need = maxP;
    for (int x = lo; x <= maxP; x += 5) if (predict(m->pv, (float)x, r->mFast) <= ziel - 1) { need = x; break; }
    // feedback: warmer than the target anyway -> add more, let it go again slowly (not before the median is full)
    if (k >= 15) { if (r->emaT > ziel + 1) r->boost = fminf(40, r->boost + 1); else if (r->emaT < ziel - 3) r->boost = fmaxf(0, r->boost - 1); }
    float wantD = fminf((float)maxP, need + r->boost);
    // GPU + many cores with real core power: the spike comes within 5 s and the fan needs ~10 s, so 50 % at once
    int vorbeugen = (lg == LAGE_BEIDE || lg == LAGE_GRAFIK) && m->busy > 4 && m->sumW > 15 && wantD < 50;
    if (vorbeugen) wantD = 50;
    // lasting high load: a floor keeps a reserve for bursts on a heat-soaked cooler; on after 2 min above DauerlastW, off
    // after a minute 10 W below, and not while the power is rising again
    if (r->pSlow >= dauerW) r->dauerSek = r->dauerSek < 120 ? r->dauerSek + 1 : 120;
    // (never below 0: up to 0.6.9 it ran on into the negative while idle, and after hours of idle the floor took as many
    // hours of load to come on; on machine 2 it never came on from 01.11. to 06.11.)
    else if (r->pSlow < dauerW - 10 && r->pFast <= r->pSlow + 5) r->dauerSek = r->dauerSek > 60 ? 60 : r->dauerSek > 0 ? r->dauerSek - 1 : 0;
    int dauerNeu = minDauer > 0 && (r->dauer ? r->dauerSek > 0 : r->dauerSek >= 120);
    if (dauerNeu != r->dauer && !nurLesen && (fn & F_LUEFTER))
        event(dauerNeu ? L_("dauer_an", "Dauerlast über %d W: Lüfter mindestens %d %%") : L_("dauer_aus", "Dauerlast vorbei (unter %d W), Untergrenze %d %% aufgehoben"), dauerNeu ? dauerW : dauerW - 10, minDauer);
    r->dauer = dauerNeu;
    if (r->dauer && wantD < minDauer) wantD = (float)minDauer;
    // power jump: SprungW more within 2 s (against the higher of 2 s ago and the 20-s average, so dips under steady load do
    // not count) up to at least DauerlastW, or the prediction at the current duty far above the hot spot, sends the fan to
    // SprungProzent and holds 20 s; every further jump renews the hold
    int sprung = sprungW > 0 && k > 3 && m->heat >= dauerW && m->heat - fmaxf(r->h2, r->pSlow) >= sprungW;
    if (sprungW > 0 && k > 30 && (fn & F_VORHERSAGE) && predict(m->pv, r->d, r->mFast) >= spitze + 5) sprung = 1;
    r->h2 = r->h1; r->h1 = m->heat;
    if (sprung && r->sprungHalt == 0 && !nurLesen && (fn & F_LUEFTER)) event(L_("sprung", "Leistungssprung auf %.0f W: Lüfter sofort %d %%"), m->heat, sprungP);
    if (sprung) r->sprungHalt = 20; else if (r->sprungHalt > 0) r->sprungHalt--;
    if (r->sprungHalt > 0 && wantD < sprungP) wantD = (float)sprungP;
    int grund = r->boost >= 1 ? GRUND_NACHREGELN : (fn & F_VORHERSAGE) && m->pv > r->pSlow + 5 ? GRUND_VORHERSAGE : GRUND_MODELL;
    if (vorbeugen && grund == GRUND_MODELL) grund = GRUND_VORBEUGEN;   // 0.6.11: the 50 % for GPU + many cores is named as such
    // yellow (smoothed CPU or case sensor >= Achtung, off 2 °C below): +2 %/s, also beyond MaxProzent;
    // red (CPU >= Alarm for 3 s or case sensor >= Alarm): 100 % at once
    if (r->emaT >= achtung || m->Te >= achtung) r->gelb = 1; else if (r->emaT < achtung - 2 && m->Te < achtung - 2) r->gelb = 0;
    if (r->gelb && !r->warGelb && !nurLesen && (fn & F_LUEFTER)) event(L_("achtung", "Achtung: CPU %.0f °C, Gehäuse %d °C → Lüfter steigt"), m->T, m->Te);
    r->warGelb = r->gelb;
    if (r->gelb) { wantD = fmaxf(wantD, fminf(100, r->d + 2)); grund = GRUND_ACHTUNG; }
    if (m->T >= alarm) r->hot++; else if (r->hot > 0) r->hot--;
    if (r->hot >= 3 || m->Te >= alarm) { wantD = 100; grund = GRUND_NOTFALL; }
    if (grund == GRUND_NOTFALL && !r->warNotfall && !nurLesen) event2(1, L_("notfall", "NOTFALL: CPU %.0f °C, Gehäuse %d °C → Lüfter 100 %%"), m->T, m->Te);
    // up: at most 4 %/s (1 %/s far below the target), at once when really hot, after a jump or in an emergency; down after
    // 10 s by 1.5 %/s; after an emergency down at once but only 1 %/s (dropping straight back made it pump)
    float auf = m->T < ziel - 8 && r->emaT < ziel - 8 ? 1.0f : 4.0f;
    if (wantD >= r->d) {
        r->d = grund == GRUND_NOTFALL || grund == GRUND_ACHTUNG || vorbeugen || r->sprungHalt > 0 || r->emaT >= ziel + 5 ? wantD : fminf(wantD, r->d + auf);
        r->lowSince = -1; r->nachNotfall = 0;
    }
    else if (r->warNotfall || r->nachNotfall) { r->d = fmaxf(wantD, r->d - 1); r->nachNotfall = r->d > wantD; r->lowSince = -1; }
    else { if (r->lowSince < 0) r->lowSince = k; if (k - r->lowSince >= 10) r->d = fmaxf(wantD, r->d - 1.5f); }
    r->warNotfall = grund == GRUND_NOTFALL;
    return grund;
}

// system fan (cools SSDs, WLAN card and the board, not the chip): off while idle with cool SSDs; otherwise at least 30 %
// and 10 % above the CPU fans; warm SSDs +6 % per °C above 50 (composite) and +4 % per °C above 70 (hottest sensor);
// data moving (5 s in a row above 100 MB/s) ahead of the temperature, 50 % from 100 MB/s, held and let down by 1 %/s.
// Own idle check with a wide gap (off below target - 30, on above target - 17), on at least 5 min, off at least 2 min.
static int system_luefter(Regler *r, int di, int notfall)
{
    int k = r->k;
    if (k % 5 == 1) r->ssdT = ssd_temp(&r->ssdS);
    r->io = ssd_io();
    r->ioSek = r->io >= 100 ? r->ioSek + 1 : 0;
    float ioWant = r->ioSek >= 5 ? (float)fmin(100, 50 + (r->io - 100) / 20) : 0;
    r->ioHalt = fmaxf(ioWant, r->ioHalt - 1);
    int ssdWarm = notfall || r->ioHalt > 0 || r->ssdT < 0 || r->ssdT >= (r->sysAus ? 50 : 47) || r->ssdS >= (r->sysAus ? 75 : 70);
    if (r->emaT < ziel - 30 && r->pSlow < 30) r->sysLeer = 1; else if (r->emaT > ziel - 17 || r->pSlow > 40) r->sysLeer = 0;
    int aus = r->sysLeer && !ssdWarm;
    if (aus != r->sysAus && !ssdWarm && k - r->sysSeit < (r->sysAus ? 120 : 300)) aus = r->sysAus;
    if (aus != r->sysAus) r->sysSeit = k;
    if (aus != r->sysAus && !nurLesen && (fn & F_LUEFTER)) event(aus ? L_("sys_aus", "Systemlüfter aus (Leerlauf, SSD %d °C)") : L_("sys_an", "Systemlüfter wieder an (SSD %d °C)"), r->ssdT);
    r->sysAus = aus;
    int sw = aus ? 0 : di + 10 > 30 ? di + 10 : 30;
    if (r->ssdT >= 50) { int x = 30 + 6 * (r->ssdT - 50); sw = sw > x ? sw : x; }
    if (r->ssdS >= 70) { int x = 30 + 4 * (r->ssdS - 70); sw = sw > x ? sw : x; }
    if (r->ioHalt > sw) sw = (int)ceilf(r->ioHalt);
    return sw > 100 ? 100 : sw;
}

// the curves go into the EC only on a change of 5 % or an anchor 5 °C away (an emergency on any change; emergency and
// yellow write pending registers at once, all of them, also in a quiet time); the registers are checked against an EC
// reset every 5 min, one a second. Fan control off: the BIOS values go back the same way (2 registers a second, retried
// until written).
static void ec_kurven(Regler *r, int di, int grund, int fanOn, const uint8_t orig[12])
{
    int k = r->k;
    int neu = curveD < 0 || abs(di - curveD) >= 5 || abs(sysWant - curveSys) >= 5 || fabsf(r->emaTe - curveAnchor) > 5
              || (grund == GRUND_NOTFALL && di != curveD);
    int dringend = grund == GRUND_NOTFALL || grund == GRUND_ACHTUNG;
    if (fanOn && neu) { if (!ec_ruhig() || dringend) write_curve(di, r->emaTe, dringend ? 12 : 2); }
    else if (!fanOn && curveD >= 0) { bios_als_ziel(orig); if (!ec_ruhig()) curve_schritt(2); }
    else if (curveOffen) { if (!ec_ruhig() || dringend) curve_schritt(dringend ? 12 : 2); }
    else if (fanOn && pruefNr >= 0) { if (!ec_ruhig()) curve_pruefen(); }
    else if (fanOn && k % 300 == PRUEF_SEK && curveD >= 0) pruefNr = 0;
}

// the power mode read again every 5 min: after an EC reset (sleep) the controller knows the real mode again; a change
// outside the quiet mode (by the user or the firmware) becomes the mode to keep; unreadable at start: taken over now
// ("leise" then counts as a leftover of a killed run, as at start). The mode we last tried to write, found in the EC,
// means our unconfirmed write landed. Due every 5 min, done at the first second without a quiet time.
static void modus_pruefen(Regler *r)
{
    if (nurLesen) return;
    if (r->k % 300 == MODUS_SEK) r->modusFaellig = 1;
    if (!r->modusFaellig || ec_ruhig()) return;
    r->modusFaellig = 0;
    int v = ec_read_sicher(REG_MODUS, r->modusJetzt);
    if (v < 0) return;
    if (v == r->modusJetzt) { r->modusWill = v; return; }   // a failed write of ours did not land
    if (v == r->modusWill) { r->modusJetzt = v; r->modusLocker = MODUS_HALT_S; return; }   // landed earlier: no further hold
    if (r->modusOrig < 0) r->modusOrig = v == MODUS_LEISE ? 0 : v;
    else if (r->modusJetzt != MODUS_LEISE) r->modusOrig = v;
    r->modusJetzt = r->modusWill = v;
}

static void aufzeichnen(Regler *r, const Messung *m, int lg, int grund, int di)
{
    static const char *grundName[] = { "modell", "vorhersage", "nachregeln", "NOTFALL", "nur-lesen", "achtung", "vorbeugen" };
    // Prozent = duty the controller asked for (written into the EC curve as soon as the EC is free), -1 = BIOS curve;
    // Regler_Prozent = what the controller wants, also when switched off
    int *ec = r->ec;
    int r1 = ec[1] < 0 || ec[2] < 0 ? -1 : ec[1] << 8 | ec[2], r2 = ec[3] < 0 || ec[4] < 0 ? -1 : ec[3] << 8 | ec[4], r3 = ec[5] < 0 || ec[6] < 0 ? -1 : ec[5] << 8 | ec[6];
    float vorh = predict(m->pv, (float)(curveD >= 0 ? curveD : di), r->mFast);
    if (sh) {
        EvoSample *s = &sh->s[sh->nSample % EVO_N];
        s->t = time(0); s->tctl = m->T; s->tglatt = r->emaT; s->vorhersage = vorh; s->pkgW = (float)r->pkgW; s->coreW = (float)m->sumW; s->maxCoreW = (float)m->maxW;
        s->gpuW = m->G; s->apuW = (float)r->aW; s->heatW = m->heat; s->pVorher = m->pv; s->mhz = (float)m->maxMhz; s->load = (float)m->load;
        s->ecT = (int16_t)m->Te; s->busy = (int16_t)m->busy; s->gpuLoad = (int16_t)r->gL; s->gpuT = (int16_t)r->gT; s->fan = (int16_t)(nurLesen ? -1 : curveD);
        s->cap = (int16_t)r->capNow; s->rpm[0] = (int16_t)r1; s->rpm[1] = (int16_t)r2; s->rpm[2] = (int16_t)r3;
        s->lage = (uint8_t)lg; s->modus = (uint8_t)r->modusJetzt; s->grund = (uint8_t)grund; s->leise = r->modusJetzt == MODUS_LEISE;
        s->ecFan = (int16_t)(nurLesen || m->Te < 0 ? -1 : curve_duty(m->Te, 0));
        s->sysEc = (int16_t)(nurLesen || m->Te < 0 ? -1 : curve_duty(m->Te, 6)); s->sysFan = (int16_t)(nurLesen ? -1 : curveSys);
        s->ssdT = (int16_t)r->ssdT; s->ssdS = (int16_t)r->ssdS;
        sh->nSample++;
        if (r->k % 60 == 0) publish_model();
    }
    SYSTEMTIME st; GetLocalTime(&st);
    char line[400];
    snprintf(line, sizeof line, "%02d:%02d:%02d;%.1f;%.1f;%d;%.1f;%.1f;%.1f;%.0f;%d;%.1f;%.1f;%.0f;%.0f;%d;%d;%d;%d;%s;%d;%d;%d;%.1f;%.2f;%d;%d;%s;%d;%d;%d;%d;%s;%.0f\r\n",
             st.wHour, st.wMinute, st.wSecond, m->T, r->emaT, m->Te, r->pkgW, m->sumW, m->maxW, m->G, r->aW, m->heat, m->pv, m->maxMhz, m->load, m->busy, r->gL, r->gT,
             nurLesen ? -1 : curveD, grundName[grund], r1, r2, r3, vorh, versatz, felder(), r->capNow, lageName[lg], r->modusJetzt, nurLesen ? -1 : di, r->ssdT, r->ssdS, ssdEinzeln, r->io);
    csv_line(line);
    if (r->console) printf("\r%02d:%02d:%02d  Tctl %5.1f (glatt %5.1f)  EC %3d  Kerne %5.1f W  GPU %5.1f W  -> Luefter %3d %% [%s] %d U/min  Takt %d MHz [%s]   ",
                           st.wHour, st.wMinute, st.wSecond, m->T, r->emaT, m->Te, m->sumW, m->G, nurLesen ? -1 : curveD, grundName[grund], r1, r->capNow, lageName[lg]);
}

// shared memory for the viewer; an old viewer may still hold a smaller mapping of an older layout: then run without it
static EvoShared *shared_open(void)
{
    SECURITY_ATTRIBUTES saRead = { sizeof saRead };   // users may read our data
    ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;AU)", SDDL_REVISION_1, &saRead.lpSecurityDescriptor, 0);
    HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, &saRead, PAGE_READWRITE, 0, sizeof(EvoShared), EVO_SHM);
    int alt = map && GetLastError() == ERROR_ALREADY_EXISTS;
    EvoShared *s = map ? MapViewOfFile(map, FILE_MAP_WRITE, 0, 0, 0) : 0;
    MEMORY_BASIC_INFORMATION mi;
    if (s && alt && (!VirtualQuery(s, &mi, sizeof mi) || mi.RegionSize < sizeof(EvoShared))) {
        UnmapViewOfFile(s); s = 0;
        event(L_("shm_alt", "Eine alte Anzeige läuft noch: Anzeige erst nach ihrem Neustart"));
    }
    if (!s && map) CloseHandle(map);
    if (s) { memset(s, 0, sizeof *s); s->magic = EVO_MAGIC; s->size = sizeof *s; }
    return s;
}

static int run(void)
{
    GetModuleFileNameW(0, exeDir, MAX_PATH);
    *wcsrchr(exeDir, L'\\') = 0;
    ExpandEnvironmentStringsW(L"%ProgramData%\\evo-x2-cool", dataDir, MAX_PATH);
    ExpandEnvironmentStringsW(EVO_INI, iniFile, MAX_PATH);
    CreateDirectoryW(dataDir, 0);
    load_settings();
    FILETIME iniSeen = ini_time();

    // the EC/PCI locks are shared with other tools (LibreHardwareMonitor, FanControl)
    SECURITY_ATTRIBUTES saAll = { sizeof saAll };
    ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;WD)", SDDL_REVISION_1, &saAll.lpSecurityDescriptor, 0);
    HANDLE single = CreateMutexW(0, TRUE, L"Global\\evo-x2-cool-laeuft");
    if (!single || GetLastError() == ERROR_ALREADY_EXISTS) { event(L_("laeuft", "evo-x2-cool läuft schon, zweiter Start beendet")); return 1; }
    mPci = CreateMutexW(&saAll, FALSE, L"Global\\Access_PCI");
    mEc = CreateMutexW(&saAll, FALSE, L"Global\\Access_EC");
    hCpu = pio_load(exeDir, L"AMDFamily17");
    hEc = pio_load(exeDir, L"LpcACPIEC");
    if (!hCpu || !hEc) { event(L_("pawnio", "PawnIO nicht erreichbar (Adminrechte? pawnio\\*.bin neben der exe?)")); return 1; }
    sh = shared_open();
    etw_start();
    wmiOk = !ohneEc && wmi_init();
    event(wmiOk ? L_("wmi_lesen", "EC-Temperatur und CPU-Lüfterdrehzahl über die BIOS-Schnittstelle (WMI) gelesen, Steuerung weiter direkt")
                : L_("wmi_lesen_aus", "BIOS-Schnittstelle (WMI) nicht verfügbar: alles direkt über den EC gelesen"));

    // safety: only control the EC we know (firmware 1.10 = bytes 01 0A at 0x00); a busy EC gets 3 s (ec_read_geduldig),
    // still unreadable or another version: read only. A stop during these reads ends the run before anything is done.
    // power mode: "leise" at start is most likely left over from a killed run; unreadable: not controlled until the 5-min
    // check can read it
    int v0 = ec_read_geduldig(REG_VERSION), v1 = ec_read_geduldig(REG_VERSION + 1), modus0 = ec_read_geduldig(REG_MODUS);
    if (WaitForSingleObject(stopEv, 0) == WAIT_OBJECT_0) {
        event(L_("stopp_start", "Stopp während des Starts: nichts verändert"));
        etw_stopp(); CloseHandle(hCpu); CloseHandle(hEc); return 0;
    }
    if (!nurLesen && (v0 != 1 || v1 != 10)) { event(L_("ec_version", "EC-Version %d.%d unbekannt (erwartet 1.10): nur lesen"), v0, v1); nurLesen = 1; }
    uint8_t orig[12] = { 0 };
    if (!nurLesen && !read_bios_fan(orig)) { event(L_("bios_fehlt", "BIOS-Lüftereinstellungen (UEFI Setup) nicht lesbar: nur lesen")); nurLesen = 1; }

    topology();
    adl_init();
    if (adlIdx < 0) event(L_("adl", "GPU-Werte vom Treiber nicht verfügbar: GPU-Leistung wird geschätzt"));
    drives_open();
    load_model();
    static Regler reg;   // static: ~8 KB of per-CPU counters
    Regler *r = &reg;
    for (int i = 0; i < 7; i++) r->ec[i] = -1;
    r->gW = r->gL = r->gT = r->aW = -1;
    r->ntqsi = (NTQSI)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation");
    r->modusJetzt = modus0;
    r->modusOrig = r->modusJetzt == MODUS_LEISE ? 0 : r->modusJetzt;
    r->modusWill = r->modusJetzt;
    publish_settings(); publish_model();
    if (nurLesen) event(L_("start_nurlesen", "Start: nur lesen, es wird nichts gesteuert"));
    else {
        event(L_("start", "Start %s: Ziel %.0f °C, Achtung %d °C, Alarm %d °C, Lüfter %d–%d %%, Taktbremse ab %d °C, gelernt: %d Bereiche, %+.1f °C"),
              EVO_VERSION, ziel, achtung, alarm, minP, maxP, spitze, felder(), versatz);
        for (int i = 0; i < EVO_NFUNK; i++) if (!(fn >> i & 1)) event(L_("f_aus", "%s ist ausgeschaltet"), funk_text(i));
        event(L_("bios", "BIOS-Lüfterkurve gesichert (%d/%d/%d °C), kommt beim Beenden zurück"), orig[1], orig[2], orig[3]);
        if (r->modusOrig < 0) event(L_("modus_unbekannt", "EC-Leistungsmodus nicht lesbar: Leise-Modus erst, wenn er lesbar ist"));
    }
    if (!nurLesen && !(fn & F_LUEFTER)) { bios_zurueck_jetzt(orig); ecRuheBis = 0; }   // a killed run may have left its curve in the EC; the loop reads the EC from the first second
    if (!nurLesen && !(fn & F_TAKT) && read_cap() != 0) write_cap(0);
    uint64_t unit = 0; msr(MSR_RAPL_UNIT, &unit);
    r->joule = 1.0 / (double)(1ull << ((unit >> 8) & 0x1F));   // energy status unit
    QueryPerformanceFrequency(&r->freq);
    r->d = (float)(minP > 40 ? minP : 40);
    r->capNow = read_cap(); r->grafikSeit = -1; r->lageNow = -1; r->lowSince = -1; r->sysSeit = -1000; r->ssdT = r->ssdS = -1;
    r->console = GetConsoleWindow() != 0;
    ULONGLONG next = GetTickCount64();

    for (r->k = 0; ; r->k++) {
        int k = r->k;
        Messung m;
        messen(r, &m);
        if (nurLesen && k % 10 == 0) r->capNow = read_cap();
        if (k == 0) { next += 1000; Sleep(1000); continue; }   // first round only primes the counters
        glaetten(r, &m);
        if (k % 30 == 25) acpi13_pruefen();
        if (k % 30 == 26) abfragen_melden(k % 600 == 26);
        if (k == 120) {
            if (acpiZahl > 1) event(L_("etw_an", "Windows-EC-Abfragen sichtbar (Takt %.2f s): EC-Zugriffe weichen ihnen aus"), (double)acpiTakt / qpcHz.QuadPart);
            else event(L_("etw_aus", "Windows-EC-Abfragen nicht sichtbar: EC-Zugriffe wie bisher"));
        }
        if (k % 5 == 0) einstellungen_pruefen(r, &iniSeen);
        int lg = lastart(r, &m);
        if (WaitForSingleObject(stopEv, 0) == WAIT_OBJECT_0) break;   // stop requested: no more writes this second
        taktbremse(r, &m, lg);
        ec_modus(r, &m, lg);
        // learn only when calm (power steady, temperature hardly moving) and not at boost hot spots (>= 90 °C: one core inside
        // the chip, more fan does not change them); end temperature = T + tau * slope
        if (!nurLesen && (fn & F_LERNEN) && k > 30 && m.T < 90 && r->emaT < 90 && fabsf(r->pFast - r->pSlow) < 5 && fabsf(r->slope) < 0.08f && curveD >= 0)
            learn(r->pSlow, (float)curveD, r->mSlow, r->emaT + tau * r->slope);
        int grund = cpu_luefter(r, &m, lg);
        int di = (int)lroundf(r->d);
        sysWant = system_luefter(r, di, grund == GRUND_NOTFALL);
        // switched off, the emergency still takes over, and the controller keeps the fan until it is down at its wanted
        // value (yellow does not: switched off, the BIOS curve is at full from 85 °C anyway)
        int fanOn = !nurLesen && ((fn & F_LUEFTER) || grund == GRUND_NOTFALL || r->nachNotfall);
        ec_kurven(r, di, grund, fanOn, orig);
        modus_pruefen(r);
        if (nurLesen) grund = GRUND_NUR_LESEN;
        aufzeichnen(r, &m, lg, grund, di);
        if (!nurLesen && k % 60 == 0) save_model();

        next += 1000;
        ULONGLONG now = GetTickCount64();
        if (WaitForSingleObject(stopEv, next > now ? (DWORD)(next - now) : 0) == WAIT_OBJECT_0) break;
        if (next < now) next = now;
    }

    // --- stop: save, BIOS fan values back, safe clock, original power mode ---
    if (!nurLesen) {
        save_model();
        int ok = bios_zurueck_jetzt(orig);
        if (fn & F_TAKT) write_cap(sicherMhz);
        if (r->modusOrig >= 0 && (r->modusJetzt != r->modusOrig || r->modusWill != r->modusOrig)) ec_write(REG_MODUS, (uint8_t)r->modusOrig);
        event(L_("stopp", "Stopp: BIOS-Lüfterkurve %s, Taktbremse bleibt %d MHz, Modell gespeichert"),
              ok ? L_("stopp_ok", "zurück") : L_("stopp_fehler", "NICHT vollständig zurück"), fn & F_TAKT ? sicherMhz : r->capNow);
    } else event(L_("stopp_nurlesen", "Stopp (nur lesen)"));
    if (csv) CloseHandle(csv);
    etw_stopp();
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
    if (c == SERVICE_CONTROL_STOP || c == SERVICE_CONTROL_PRESHUTDOWN) {
        ss.dwCurrentState = SERVICE_STOP_PENDING; ss.dwWaitHint = 15000; SetServiceStatus(ssh, &ss);   // see BIOS_FRIST_MS
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
        else if (!wcscmp(a, L"--ohne-ec")) nurLesen = ohneEc = 1;
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
    SECURITY_ATTRIBUTES sa = { sizeof sa };   // admins and SYSTEM may send the stop, the logged-on user too ("Alles beenden"
    // in the window): stopping only gives the fans back to the BIOS curve and leaves the safe clock cap, so that is harmless
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
