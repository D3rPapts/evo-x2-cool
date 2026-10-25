// SPDX-License-Identifier: GPL-3.0-or-later
// Shared memory between the controller (evo-x2-cool.exe, runs as a service) and the viewer (evo-x2-cool-front.exe).
// The controller appends one sample per second and an event whenever it decides something; the viewer only reads.
#pragma once
#include <stdint.h>
#include <wchar.h>

#define EVO_SHM    L"Global\\evo-x2-cool"
#define EVO_STOPP  L"Global\\evo-x2-cool-stopp"   // set it and the controller restores everything and ends (service too)
#define EVO_MAGIC  0x45564F32u   // "EVO2"
#define EVO_N      7200          // samples kept: 2 hours at 1 s
#define EVO_EV     256           // events kept

enum { LAGE_WENIGE, LAGE_VIELE, LAGE_GRAFIK };
enum { GRUND_MODELL, GRUND_VORHERSAGE, GRUND_NACHREGELN, GRUND_NOTFALL, GRUND_NUR_LESEN };

// functions that can be switched off in einstellungen.ini, section [Funktionen] (1 = on). The emergency (fan 100 %
// when overheating) cannot be switched off: it takes over even while the fan control is off.
enum { F_LUEFTER = 1, F_TAKT = 2, F_LEISE = 4, F_LERNEN = 8, F_VORHERSAGE = 16, F_ALLE = 31 };
#define EVO_NFUNK 5
static const wchar_t *const evoFunkKey[EVO_NFUNK] = { L"Luefter", L"Taktbremse", L"EcLeise", L"Lernen", L"Vorhersage" };
#define EVO_INI L"%ProgramData%\\evo-x2-cool\\einstellungen.ini"

// sequence of printf conversions in a format string ("%.0f °C %d" -> "fd"). A translated format from a language file is
// used only when its sequence matches the built-in one, so a broken translation cannot crash anything.
static void evo_conv_sig(const wchar_t *s, char *out, int n)
{
    int k = 0;
    for (; *s && k < n - 1; s++) {
        if (*s != L'%') continue;
        if (*++s == L'%') continue;
        while (*s && !wcschr(L"diouxXcsfFeEgGaA", *s)) s++;
        if (!*s) break;
        out[k++] = (char)*s;
    }
    out[k] = 0;
}

typedef struct {
    int64_t t;                          // unix time, seconds
    float tctl, tglatt, vorhersage;     // °C; vorhersage = predicted end temperature at the current fan duty
    float pkgW, coreW, maxCoreW, gpuW, apuW, heatW, pVorher;   // W; heatW = 1.1 * cores + 0.6 * GPU, pVorher = predicted heatW
    float mhz, load;                    // highest effective clock, CPU load %
    int16_t ecT, busy, gpuLoad, gpuT, fan, cap, rpm[3];   // fan = duty % the controller asked for
    uint8_t lage, modus, grund, leise;  // modus = EC power mode register 0x31
} EvoSample;

typedef struct { int64_t t; uint8_t wichtig; char text[119]; } EvoEvent;   // wichtig = hot spot / emergency, shown red; text UTF-8

typedef struct {
    uint32_t magic, size;
    uint32_t nurLesen;                  // 1 = read-only mode, nothing is controlled
    uint32_t funktionen;                // F_* bits currently active
    float ziel; int32_t minP, maxP, spitze;
    float versatz; int32_t felder;      // learned model: overall offset, number of learned cells
    int32_t deckel[3];                  // learned clock cap per situation (MHz)
    volatile uint32_t nSample, nEvent;  // totals written; slot = n % EVO_N / EVO_EV
    EvoSample s[EVO_N];
    EvoEvent e[EVO_EV];
} EvoShared;
