<p align="center"><img src="logo-256.png" width="96" alt="EVO-X2-COOL logo"></p>

# EVO-X2-COOL

Learning fan and clock controller for the **GMKtec EVO-X2** mini PC (AMD Ryzen AI MAX+ 395 "Strix Halo", Sixunited AXB35 board) under Windows.

> **Unofficial community tool.** Not affiliated with, endorsed by or supported by GMKtec, Sixunited or AMD.
> It writes to the embedded controller (EC) and changes the CPU clock limit. Use it at your own risk.
>
> Written with AI assistance (Claude Code), tested on real hardware.

*Deutsch: siehe [unten](#deutsch).*

## Why

Under single-core boost the EVO-X2 can jump from 40 °C to almost 100 °C in a few seconds. The stock fan curve reacts too late, which led to thermal hard freezes on our machine. EVO-X2-COOL watches the chip every second, predicts where the temperature is heading, and:

- **controls the fans** through the fan curve inside the EC: as quiet as possible while the predicted end temperature stays below the target (default 80 °C);
- **brakes the clock** depending on the load type (few cores / many cores / GPU busy). After a hot spot it drops to a safe clock for 30 s and remembers a lower limit for that load type;
- **learns** how warm the machine really gets at a given heat power and fan duty, and corrects its own model;
- **switches the EC to the quiet power mode** (54 W) while many cores are busy and the GPU is idle;
- has an **emergency mode** (fan to full) that cannot be switched off.

If the program crashes, the EC keeps regulating with the last (rising) curve. On a normal stop the BIOS fan curve is written back.

## Programs

| File | What it does | Size |
|---|---|---|
| `evo-x2-cool.exe` | Controller. Runs in a console or as the Windows service `evo-x2-cool`. | ~57 KB, ~2 MB RAM, < 1 % of one core |
| `evo-x2-cool-front.exe` | Window with tiles, curves, events and on/off switches, plus a tray icon (tooltip with current values, red when hot). Reads the controller's shared memory, no network. | ~65 KB, ~2 MB RAM |

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

Start `evo-x2-cool-front.exe` as a normal user. `--leiste` starts it hidden in the tray. Closing the window only hides it. The tray menu has **Exit** (window only) and **Exit all** (also stops the controller and gives the fans back to the BIOS curve).

## Settings

`%ProgramData%\evo-x2-cool\einstellungen.ini` is re-read within 5 s after a change:

```ini
[Regler]
Ziel=80          ; target temperature °C
MinProzent=30    ; fan duty range
MaxProzent=70
Spitze=88        ; hot spot: brake the clock from here
SicherMHz=3600   ; clock cap left behind on stop
Sprache=de       ; de / en, any sprache-<code>.ini next to the exe works

[Funktionen]     ; 1 = on, also switchable in the window
Luefter=1
Taktbremse=1
EcLeise=1
Lernen=1
Vorhersage=1

[Anzeige]
Thema=system     ; system / hell / dunkel
```

Data in the same folder: `modell.txt` (learned model), `regler-<date>.csv` (one line per second), `ereignisse.txt` (decisions).

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

- Regelt die Lüfter so leise wie möglich unter einer Zieltemperatur (Standard 80 °C) und sagt den Temperaturverlauf voraus.
- Bremst den Takt je nach Lastart, nach einer Hitzespitze 30 s stark, und merkt sich eine sichere Grenze.
- Lernt aus Erfahrung. Den Notfall (Lüfter auf voll) kann man nicht abschalten.
- Fenster und Meldungen auf Deutsch oder Englisch, hell oder dunkel, Funktionen einzeln schaltbar.
- Braucht EC-Firmware 1.10 und den Treiber PawnIO. Bedienung und Einstellungen wie oben.
