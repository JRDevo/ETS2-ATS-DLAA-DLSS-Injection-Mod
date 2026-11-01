<!-- Short README in German. The full guide (English): ../../README.md -->
> [!CAUTION]
> **Lade diesen Mod nur von https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases herunter.** Es gibt keine Website, keinen Discord,
> kein Nexus und keinen Steam Workshop. Jede andere Seite, jeder Mirror oder "Installer", der ihn anbietet, ist nicht von mir: führe ihn nicht aus.

![ETS2 & ATS DLAA & DLSS Mod](../images/banner.jpg)

# ETS2 & ATS: DLAA & DLSS Injection Mod

NVIDIA **DLAA** und **DLSS** für den **Euro Truck Simulator 2** und den **American Truck Simulator**, auf einem normalen Bildschirm und in VR.
Der Mod ersetzt die eigene Kantenglättung des Spiels durch DLAA: Zäune, Drähte, Fahrbahnmarkierungen und Schatten flimmern nicht mehr.
DLSS kann das Bild außerdem hochskalieren, für eine höhere Bildrate.

Es ist eine einzige kleine Datei, `dinput8.dll`, im Spielordner. Kein Launcher, kein Konto.

- **Ein ruhiges, scharfes Bild:** kein Flimmern an Zäunen, Drähten und Schatten. Auch Spiegel bekommen Kantenglättung.
- **Flach und VR:** eine Datei für beides. Dreifach-Bildschirme und Windows-HDR funktionieren.
- **VR-Beschleuniger:** Foveated Rendering (auch mit Eye-Tracking), DLAA nur in der Mitte jedes Auges, Bildraten-Limits.
- **Ein Menü im Spiel in deiner Sprache:** drücke `Delete`. Jede Einstellung, ihre Taste und ihre Kosten, mit einer kurzen Beschreibung.
- **Ein Fehlerbericht mit einem Tastendruck:** drücke zweimal `Insert`. Der Mod speichert Bilder, Systeminfo und Logs in Zip-Dateien.

## Was du brauchst

- Euro Truck Simulator 2 oder American Truck Simulator (64 Bit, DirectX 11).
- Eine **NVIDIA-RTX**-Grafikkarte mit aktuellem Treiber. DLAA und DLSS laufen nur auf RTX-Karten.
- Die Datei `nvngx_dlss.dll` (die NVIDIA-DLSS-Laufzeit). Der Mod enthält sie nicht: siehe Schritt 6.
- Für VR: eine OpenXR-Laufzeit (zum Beispiel Virtual Desktop) und das Spiel, gestartet mit der Option `-openxr`.

## Installation

1. **Mache eine Sicherung** des Ordners `bin\win_x64\` des Spiels.
2. [Lade die neueste Version herunter](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases/latest): die `.zip` unter **Assets**. Entpacke sie.
3. Öffne den Spielordner, zum Beispiel `...\steamapps\common\Euro Truck Simulator 2\bin\win_x64\`
   (oder `...\American Truck Simulator\bin\win_x64\`).
4. **Entferne zuerst andere Proxy-Mods:** lösche jede `dinput8.dll` oder `dxgi.dll` eines anderen Mods in diesem Ordner.
5. Kopiere **`dinput8.dll`** nach `bin\win_x64\`. Du kannst auch **`dlaa.ini`** dorthin kopieren, um Einstellungen und Tasten zu ändern (nicht erforderlich).
6. Lege **`nvngx_dlss.dll`** in denselben Ordner. Du bekommst sie aus dem NVIDIA DLSS SDK (<https://github.com/NVIDIA/DLSS>,
   Datei `lib/Windows_x86_64/rel/nvngx_dlss.dll`) oder kopierst sie aus einem beliebigen Spiel, das sie hat.
7. Starte das Spiel. DLAA ist an.

Zum Deinstallieren: lösche `dinput8.dll` (und `dlaa.ini`) aus `bin\win_x64\`.

**Linux / Proton:** installiere auf die gleiche Weise. Stelle in Steam die Startoptionen des Spiels auf `WINEDLLOVERRIDES="dinput8=n,b" %command%`.

## Die wichtigsten Tasten

| Taste | Was sie tut |
|---|---|
| `Delete` | Öffnet und schließt das Menü. `Up` / `Down` wählen eine Zeile, `Left` / `Right` ändern sie. |
| `End` | Wechselt den Modus: aus, DLAA, DLSS. |
| `Insert` zweimal | Fehlerbericht: speichert Bilder, Systeminfo und Logs in `dlaa_selftest\`. |

## Mehr

- **Die vollständige Anleitung (Englisch):** [README.md](../../README.md): alle Einstellungen, Tasten, VR, Leistung und Fehlerbehebung.
- **Probleme:** [öffne ein Issue](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new/choose). Bitte schreibe es auf Englisch.
- **Übersetzungen** werden von einer KI gemacht und können Fehler enthalten.
  [Melde ein Übersetzungsproblem](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new?template=translation.yml).
- Dies ist ein inoffizielles Fanprojekt. Es steht in keiner Verbindung zu SCS Software oder NVIDIA.
