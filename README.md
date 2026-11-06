<p align="center"><img src="logo-256.png" width="96" alt="EVO-X2-COOL logo"></p>

# EVO-X2-COOL

Learning fan and clock controller for the **GMKtec EVO-X2** mini PC (AMD Ryzen AI MAX+ 395 "Strix Halo", Sixunited AXB35 board) under Windows.

> **Unofficial community tool.** Not affiliated with, endorsed by or supported by GMKtec, Sixunited or AMD.
> It writes to the embedded controller (EC) and changes the CPU clock limit. Use it at your own risk.
>
> Written with AI assistance (Claude Code), tested on real hardware.

> **Note (06.11.2026):** the code on `main` is the withdrawn 0.7.1. Its fan control through the BIOS interface reacts too fast (the CPU fans pump under bursty load). Please use the [0.6.8 release](https://github.com/D3rPapts/evo-x2-cool/releases/tag/v0.6.8) until 0.7 is back after a full night of testing. / **Hinweis:** `main` enthält das zurückgezogene 0.7.1, bitte 0.6.8 verwenden.

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

- **controls the fans** (since 0.7.0 the two CPU fans through the BIOS' own interface, see below): as quiet as possible while the predicted end temperature stays below the target (default 80 °C). Rising power is counted ahead, but only while it still rises and never beyond the measured power; far below the target the fan ramps up slowly, so a short burst stays quiet. Under **lasting high load** (default: 2 minutes above 70 W) the fan keeps at least 45 %, so a burst on the cores on top of a heat-soaked cooler does not reach the emergency; on a **power jump** (default 50 W within 2 s) the fan goes to 50 % at once and holds for 20 s, then comes down slowly (1.5 %/s);
- **cools the SSDs** with the 120 mm system fan on the back (SSDs, WLAN): it watches the SSD temperature including the hotter controller sensor (a Samsung 990 PRO reported 96 °C on the controller while its main value said 70 °C) and the amount of data moved. It switches on ahead of the heat after 5 s of more than 100 MB/s, runs at least 30 % and 10 % above the CPU fans, and is off while idle with cool SSDs (on at 68 °C controller / 55 °C main value; at least 5 min on, 2 min off, so it does not flap);
- **brakes the clock** depending on the load type (few cores / many cores / GPU busy). After a hot spot it drops to a safe clock for 30 s and remembers a lower limit for that load type. It tries the next higher step again only after a calm run with a core really at the limit: 10 minutes, then 1 hour, then 1 day after every further hot spot on that step;
- **learns** how warm the machine really gets at a given chip power and fan duty, and corrects its own model. The starting model (end temperature = 34 + P × (0.58 − 0.30 × fan) + 6 × hottest core W, P = whole chip power) was fitted on the logs of two EVO-X2;
- **switches the EC to the quiet power mode** (54 W) while many cores are busy and the GPU is idle;
- has two warning stages: **yellow** (default 90 °C: the fan climbs steadily, also beyond MaxProzent) and **red** (default 95 °C: fan to full at once). Red cannot be switched off.

On a normal stop the fans go back to the BIOS curve. Since 0.7.0 the CPU fans hold a fixed duty set by the program, so if it crashes Windows restarts the service after 5 s; if the BIOS interface stops answering, the program hands the fans back to the EC and continues the old direct way.

**Since 0.7.0 it no longer talks to the EC directly for the CPU fans.** The BIOS of the EVO-X2 has an ACPI-WMI interface (`root\wmi`, class `PowerSwitchInterface`) that sets the duty of both CPU fans, reads their speeds, the EC temperature and the power mode, and reaches the EC through Windows' own ACPI driver, so nothing can collide with it. EVO-X2-COOL now applies its fan curve itself every second through that interface. Only the 120 mm system fan is not reachable this way: its curve is still written into the EC directly, as a flat step (0/30/45/60/80/100 %, safety ramp to full above 80 °C) that is rewritten only when the step changes, a few times an hour, with 6 s of quiet afterwards. Without the WMI interface (other BIOS) everything works the direct way of 0.6.x (about 1,500 accesses per hour, no bursts, 6 s quiet after every write).

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
| `evo-x2-cool.exe` | Controller. Runs in a console or as the Windows service `evo-x2-cool`. | ~62 KB, ~2 MB RAM, < 1 % of one core, never goes online |
| `evo-x2-cool-front.exe` | Window with tiles (CPU, GPU, fans, clock brake, chip power, SSD with controller temperature and system fan), curves, events and on/off switches, plus a tray icon (tooltip with current values, red when hot). Reads the controller's shared memory. Once a day it asks GitHub whether a newer version exists (see below). | ~78 KB, ~2 MB RAM |

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
MinLeerlauf=15   ; lower floor while idle and far below the target (15 since 0.6.9: steadier than 10, same temperature)
MaxProzent=70
Spitze=88        ; hot spot: brake the clock from here
Achtung=90       ; yellow: fan climbs by 2 %/s, also beyond MaxProzent
Alarm=95         ; red: fan 100 % at once (at most 97)
SicherMHz=3600   ; clock cap left behind on stop
MinDauerlast=45  ; fan at least this % after 2 min above DauerlastW (0 = off)
DauerlastW=70    ; lasting load from this many watts (smoothed); off again 10 W below
SprungW=50       ; power jump: this many watts more within 2 s sets the fan to SprungProzent at once (0 = off)
SprungProzent=50 ; fan % on a power jump, held for 20 s
TaktPlus=0       ; test switch: MHz added to every clock cap (-600..+600), also +/- in the window
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

Data in the same folder: `modell.txt` (learned model), `regler-<date>.csv` (one line per second, including SSD temperatures and data rate), `ereignisse.txt` (decisions).

## Known issues

- **ACPI event 13 ("EC did not respond within the timeout")** in the Windows system log, up to 40 a day with 0.6.x. **This was caused by EVO-X2-COOL's direct EC access**: on our second machine (BIOS 1.12 / EC 1.10 since February, system log back one year) the first one ever came 10 minutes after EVO-X2-COOL was first started, never before. The EC signals an event about every 10 s, and Windows then queries it on the same ports 0x62/0x66; Windows' EC driver shares no lock with other programs, so a direct access of ours could land in the middle of that query (the same was measured on the Bosgame M5 with the same board family, see [crmaris/FanControl.AXB35 #1](https://github.com/crmaris/FanControl.AXB35/issues/1)). 0.7.0 uses the BIOS' WMI interface for the CPU fans instead and only rarely writes the system fan's curve directly; please tell us if you still see ACPI 13 with 0.7.0.
- **Samsung 990 PRO without heatsink:** under heavy data load its controller reached 96 °C even with the system fan at full speed, and one drive dropped out under such load. A thin heatsink or a thermal pad to the case helps more than any fan curve. Check the space above the M.2 slot first, and do not remove the label.

## Share your logs

We tune EVO-X2-COOL with the logs of two machines. Logs from other EVO-X2 (other BIOS versions, SSDs, cases, workloads) help a lot. If you want to help:

1. Since 0.6.7: right-click the icon next to the clock, "Save logs (ZIP on the desktop)". With an older version, open PowerShell and run this. It packs the last three days into `evo-x2-cool-logs.zip` on your desktop, together with the times of the ACPI 13 events:

   ```powershell
   $d = "$env:ProgramData\evo-x2-cool"; $t = "$env:TEMP\evo-x2-cool-logs"
   New-Item -ItemType Directory -Force $t | Out-Null; Remove-Item "$t\*" -ErrorAction SilentlyContinue
   Get-WinEvent -FilterHashtable @{LogName='System'; ProviderName='ACPI'; Id=13} -MaxEvents 500 -ErrorAction SilentlyContinue |
     Select-Object TimeCreated | Export-Csv "$t\acpi13.csv" -NoTypeInformation
   # copied while the controller keeps writing today's file
   @("$d\ereignisse.txt", "$d\modell.txt", "$d\einstellungen.ini") + (Get-ChildItem "$d\regler-*.csv" | Sort-Object Name | Select-Object -Last 3).FullName |
     Where-Object { Test-Path $_ } | ForEach-Object {
       $in = [IO.File]::Open($_, 'Open', 'Read', 'ReadWrite'); $out = [IO.File]::Create("$t\" + (Split-Path $_ -Leaf))
       $in.CopyTo($out); $out.Close(); $in.Close() }
   Compress-Archive -Path "$t\*" -DestinationPath (Join-Path ([Environment]::GetFolderPath('Desktop')) 'evo-x2-cool-logs.zip') -Force
   ```

2. Open a [new issue with the "Share logs" form](../../issues/new?template=logs.yml) and drag the ZIP file into it.

**What is inside:** temperatures, power, fan speeds, clock limits, SSD temperatures and data rates, one line per second; the controller's decisions; your settings; the times of the ACPI events. No user names, file names, programs or network data. The files are plain text, so you can open them before you send them. Note that an issue is public.

## Update check

Once a day, starting a minute after logon, the window asks the GitHub API for the newest release of this repository. If it is newer, a button and a tray menu entry appear, plus one notification per version. Nothing is downloaded or installed automatically: the setup is unsigned and brings a driver, so you decide. This request only tells GitHub your IP address and the program version. The controller service never goes online. Switch it off with `UpdatePruefen=0`.

## Build

Visual Studio Build Tools (C, x64). `build.cmd` builds both programs with the hybrid CRT (no redistributable needed). `logo.ps1` redraws the icons.

## Thanks

- [cmetz/ec-su_axb35-linux](https://github.com/cmetz/ec-su_axb35-linux): EC register layout of the AXB35 board
- [pajtony/FanControl.AXB35](https://github.com/pajtony/FanControl.AXB35): the same EC through PawnIO in FanControl
- [crmaris/FanControl.AXB35](https://github.com/crmaris/FanControl.AXB35): measured the ACPI 13 collisions on the Bosgame M5 and also moved to the BIOS WMI interface
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

- Regelt die Lüfter so leise wie möglich unter einer Zieltemperatur (Standard 80 °C) und sagt den Temperaturverlauf voraus. Unter Dauerlast (2 Minuten über 70 W) läuft der Lüfter mit mindestens 45 %. Bei einem Leistungssprung (50 W in 2 s) geht er sofort auf 50 % und danach langsam wieder runter.
- Kühlt die SSDs mit dem Systemlüfter: achtet auch auf den heißeren Controller-Sensor und schaltet bei viel Datenverkehr schon vorher ein.
- Seit 0.7.0 steuert es die CPU-Lüfter über die BIOS-eigene Schnittstelle (WMI) statt direkt über den EC. Nur die Kurve des Systemlüfters wird noch direkt geschrieben, und nur wenn sich seine Stufe ändert. Stürzt das Programm ab, startet Windows den Dienst nach 5 s neu.
- Bremst den Takt je nach Lastart, nach einer Hitzespitze 30 s stark, und merkt sich eine sichere Grenze.
- Lernt aus Erfahrung. Den Notfall (Lüfter auf voll) kann man nicht abschalten.
- Fenster und Meldungen auf Deutsch oder Englisch, hell oder dunkel, Funktionen einzeln schaltbar.
- Braucht EC-Firmware 1.10 und den Treiber PawnIO. Bedienung und Einstellungen wie oben.
- ACPI-Ereignis 13 im Windows-Protokoll (bis 0.6.x bis zu 40 am Tag) kam von unserem direkten EC-Zugriff: Auf unserem zweiten Rechner (BIOS 1.12/EC 1.10 seit Februar) kam das erste nach einem Jahr ohne zehn Minuten nach dem ersten Start von EVO-X2-COOL. 0.7.0 nutzt dafür die BIOS-Schnittstelle. Bitte melden, falls es mit 0.7.0 noch auftritt. Eine Samsung 990 PRO ohne Kühlkörper wird unter Datenlast trotz vollem Systemlüfter sehr heiß – Kühlkörper oder Wärmeleitpad zum Gehäuse helfen.
- **Logs teilen:** Mit den Logs anderer EVO-X2 können wir weiter verbessern. Ab 0.6.7: Rechtsklick auf das Symbol neben der Uhr → „Logs speichern (ZIP auf dem Desktop)“; bei älteren Versionen packt der PowerShell-Befehl oben unter „Share your logs“ die letzten drei Tage als ZIP auf den Desktop. Die ZIP-Datei bitte über das Formular „Logs teilen“ als Issue hochladen. Enthalten sind nur Messwerte (Temperaturen, Leistung, Lüfter, Takt, SSD), keine Namen, Dateien oder Programme. Issues sind öffentlich.
