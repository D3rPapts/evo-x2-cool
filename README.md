<p align="center"><img src="logo-256.png" width="96" alt="EVO-X2-COOL logo"></p>

# EVO-X2-COOL

Learning fan and clock controller for the **GMKtec EVO-X2** mini PC (AMD Ryzen AI MAX+ 395 "Strix Halo", Sixunited AXB35 board) under Windows.

> **Unofficial community tool.** Not affiliated with, endorsed by or supported by GMKtec, Sixunited or AMD.
> It writes to the embedded controller (EC) and changes the CPU clock limit. Use it at your own risk.
>
> Written with AI assistance (Claude Code), tested on real hardware.

*Deutsch: siehe [unten](#deutsch).*

## The problem

We run two EVO-X2 side by side: one with 64 GB, one with 128 GB, both with the same BIOS (1.12) and EC firmware (1.10). The 128 GB machine runs fine. The 64 GB machine **froze completely three times under heavy load**. No blue screen, no log entry, only the power button helped.

What we found:

- **Single-core boost is the killer, not full load.** One core boosting to ~4.5 GHz (e.g. the Windows search indexer) pushes the chip from 40 °C to almost **100 °C within 3–4 seconds**. The highest value we logged before a freeze was 99.8 °C. All cores at 100 % produce less of a hot spot than one core at full boost.
- **The stock fan control is too slow for that.** The EC ramps the fans up by only about 60 rpm per second, so the fans are still slow when the spike has already happened.
- **The fan curve set in the BIOS is ignored** unless the fan mode is "Manual PWM". In automatic mode the EC uses its own built-in curve.
- Cleaning the fans did not make the spikes go away.

There was no way to control the fans of this machine under Windows, so we built one.

## What it does

EVO-X2-COOL watches the chip every second, predicts where the temperature is heading, and:

- **controls the fans** through the fan curve inside the EC: as quiet as possible while the predicted end temperature stays below the target (default 80 °C). Rising power is counted ahead, but only while it still rises and never beyond the measured power; far below the target the fan ramps up slowly, so a short burst stays quiet. The 120 mm system fan on the back (SSDs, WLAN) is off while idle and the SSDs are cool, and runs at least 30 % from 55 °C SSD temperature;
- **brakes the clock** depending on the load type (few cores / many cores / GPU busy). After a hot spot it drops to a safe clock for 30 s and remembers a lower limit for that load type. It tries the next higher step again only after a calm run with a core really at the limit: 10 minutes, then 1 hour, then 1 day after every further hot spot on that step;
- **learns** how warm the machine really gets at a given chip power and fan duty, and corrects its own model. The starting model (end temperature = 34 + P × (0.58 − 0.30 × fan) + 6 × hottest core W, P = whole chip power) was fitted on the logs of two EVO-X2;
- **switches the EC to the quiet power mode** (54 W) while many cores are busy and the GPU is idle;
- has two warning stages: **yellow** (default 90 °C: the fan climbs steadily, also beyond MaxProzent) and **red** (default 95 °C: fan to full at once). Red cannot be switched off.

If the program crashes, the EC keeps regulating with the last (rising) curve. On a normal stop the BIOS fan curve is written back.

**Measured on the 64 GB machine**, one core at 100 % for 3 minutes:

| | Clock | Temperature | Fan |
|---|---|---|---|
| Learned limit too high (4.5 GHz) | 4450 MHz | 44 → **88 °C in 4 s** | – |
| Controller brakes for 30 s | 3300 MHz | ~56 °C | 30 % |
| Then the newly learned limit | 4150 MHz | **78–80 °C steady** | 30 % (quiet) |

So the single-core performance drops by about 7 %, the fans stay quiet, and the hot spot is gone.

## Programs

| File | What it does | Size |
|---|---|---|
| `evo-x2-cool.exe` | Controller. Runs in a console or as the Windows service `evo-x2-cool`. | ~58 KB, ~2 MB RAM, < 1 % of one core, never goes online |
| `evo-x2-cool-front.exe` | Window with tiles, curves, events and on/off switches, plus a tray icon (tooltip with current values, red when hot). Reads the controller's shared memory. Once a day it asks GitHub whether a newer version exists (see below). | ~70 KB, ~2 MB RAM |

## Requirements

- GMKtec EVO-X2, **EC firmware 1.10** (with any other EC version the controller only measures and changes nothing)
- Windows 10/11 x64, administrator rights for the controller
- The **[PawnIO](https://pawnio.eu)** driver (it also comes with FanControl and LibreHardwareMonitor). It works with memory integrity (HVCI) turned on.
- PawnIO modules `AMDFamily17.bin` and `LpcACPIEC.bin` from [PawnIO.Modules](https://github.com/namazso/PawnIO.Modules/releases) in a `pawnio\` folder next to the exe

## Install

`evo-x2-cool-setup-<version>.exe` installs both programs to `C:\Program Files\EVO-X2-COOL`, the PawnIO driver if it is missing, the controller as a service, and optionally the window in the tray at logon. Uninstall stops everything, gives the fans back to the BIOS curve and asks whether to keep the learned data. To build the setup: `installer\bauen.cmd` (needs Inno Setup 6; fetches the official PawnIO files and checks their hashes).

## Use

Without the installer, in an admin shell:

```
evo-x2-cool.exe                     run in this console (Ctrl+C stops and restores)
evo-x2-cool.exe --nur-lesen         only measure and show, write nothing
evo-x2-cool.exe --installieren      register and start the Windows service
evo-x2-cool.exe --entfernen         stop and remove the service
evo-x2-cool.exe --stopp             stop a console run, with restore
```

Start `evo-x2-cool-front.exe` as a normal user. The three temperatures target (green), attention (yellow) and alarm (red) can be changed with − / + in the window, the target also in the tray menu; each is at least 2 °C above the one before, alarm at most 97 °C. A higher target means less fan. `--leiste` starts it hidden in the tray. Closing the window only hides it. The tray menu has **Exit** (window only) and **Exit all** (also stops the controller and gives the fans back to the BIOS curve).

## Settings

`%ProgramData%\evo-x2-cool\einstellungen.ini` is re-read within 5 s after a change:

```ini
[Regler]
Ziel=80          ; target temperature °C
MinProzent=30    ; fan duty range
MinLeerlauf=10   ; lower floor while idle and far below the target
MaxProzent=70
Spitze=88        ; hot spot: brake the clock from here
Achtung=90       ; yellow: fan climbs by 2 %/s, also beyond MaxProzent
Alarm=95         ; red: fan 100 % at once (at most 97)
SicherMHz=3600   ; clock cap left behind on stop
MinDauerlast=45  ; fan at least this % after 2 min above DauerlastW (0 = off)
DauerlastW=70    ; lasting load from this many watts (smoothed); off again 10 W below
Sprache=de       ; de / en, any sprache-<code>.ini next to the exe works

[Funktionen]     ; 1 = on, also switchable in the window
Luefter=1
Taktbremse=1
EcLeise=1
Lernen=1
Vorhersage=1

[Anzeige]
Thema=system     ; system / hell / dunkel
UpdatePruefen=1  ; 0 = never ask GitHub for a newer version
```

Data in the same folder: `modell.txt` (learned model), `regler-<date>.csv` (one line per second), `ereignisse.txt` (decisions).

## Update check

Once a day, starting a minute after logon, the window asks the GitHub API for the newest release of this repository. If it is newer, a button and a tray menu entry appear, plus one notification per version. Nothing is downloaded or installed automatically: the setup is unsigned and brings a driver, so you decide. This request only tells GitHub your IP address and the program version. The controller service never goes online. Switch it off with `UpdatePruefen=0`.

## Build

Visual Studio Build Tools (C, x64). `build.cmd` builds both programs with the hybrid CRT (no redistributable needed). `logo.ps1` redraws the icons.

## Thanks

- [cmetz/ec-su_axb35-linux](https://github.com/cmetz/ec-su_axb35-linux): EC register layout of the AXB35 board
- [pajtony/FanControl.AXB35](https://github.com/pajtony/FanControl.AXB35): the same EC through PawnIO in FanControl
- [PawnIO](https://github.com/namazso/PawnIO) and [PawnIO.Modules](https://github.com/namazso/PawnIO.Modules) by namazso
- [LibreHardwareMonitor](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor)

No code was copied from these projects. The PawnIO modules are not part of this repository; they are under the LGPL-2.1.

## License

GPL-3.0-or-later, see [LICENSE](LICENSE).

---

## Deutsch

Lernende Lüfter- und Taktsteuerung für die GMKtec EVO-X2 unter Windows. Inoffizielles Community-Werkzeug, nicht von GMKtec. Benutzung auf eigene Gefahr. Mit KI-Unterstützung (Claude Code) geschrieben und auf echter Hardware getestet.

**Warum es das gibt:** Wir haben zwei EVO-X2 nebeneinander, 64 GB und 128 GB, mit gleichem BIOS (1.12) und gleicher EC-Firmware (1.10). Die 64-GB-Maschine ist unter schwerer Last dreimal komplett eingefroren, ohne Bluescreen und ohne Logeintrag. Ursache: Boostet ein einzelner Kern, springt der Chip in 3–4 Sekunden von 40 auf fast 100 °C (gemessen bis 99,8 °C). Die Lüfter drehen nur etwa 60 U/min pro Sekunde hoch und kommen zu spät. Die Lüfterkurve aus dem BIOS wirkt nur im Modus „Manual PWM“. Unter Windows gab es keine Möglichkeit, die Lüfter dieser Maschine zu steuern, also haben wir eine gebaut.

**Ergebnis** bei einem Kern auf 100 %: Vorher 88 °C nach 4 Sekunden. Mit der gelernten Taktgrenze (etwa 7 % weniger Ein-Kern-Takt) bleibt die CPU stabil bei 78–80 °C, und die Lüfter laufen leise mit 30 %.

- Regelt die Lüfter so leise wie möglich unter einer Zieltemperatur (Standard 80 °C) und sagt den Temperaturverlauf voraus.
- Bremst den Takt je nach Lastart, nach einer Hitzespitze 30 s stark, und merkt sich eine sichere Grenze.
- Lernt aus Erfahrung. Den Notfall (Lüfter auf voll) kann man nicht abschalten.
- Fenster und Meldungen auf Deutsch oder Englisch, hell oder dunkel, Funktionen einzeln schaltbar.
- Braucht EC-Firmware 1.10 und den Treiber PawnIO. Bedienung und Einstellungen wie oben.
