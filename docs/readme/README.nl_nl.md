<!-- Short README in Dutch. The full guide (English): ../../README.md -->
> [!CAUTION]
> **Download deze mod alleen van https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases.** Er is geen website, geen Discord,
> geen Nexus en geen Steam Workshop. Elke andere site, mirror of "installer" die hem aanbiedt is niet van mij: voer hem niet uit.

![ETS2 & ATS DLAA & DLSS Mod](../images/banner.jpg)

# ETS2 & ATS: DLAA & DLSS Injection Mod

NVIDIA **DLAA** en **DLSS** voor **Euro Truck Simulator 2** en **American Truck Simulator**, op een gewoon scherm en in VR.
De mod vervangt de eigen anti-aliasing van het spel door DLAA: hekken, draden, wegmarkeringen en schaduwen flikkeren niet meer.
DLSS kan het beeld ook opschalen, voor een hogere framerate.

Het is één klein bestand, `dinput8.dll`, in de spelmap. Geen launcher, geen account.

- **Een rustig, scherp beeld:** geen geflikker op hekken, draden en schaduwen. Spiegels krijgen ook anti-aliasing.
- **Plat en VR:** één bestand voor beide. Drie schermen en Windows HDR werken.
- **VR-versnellers:** foveated rendering (ook met eyetracking), DLAA alleen in het midden van elk oog, framerate-limieten.
- **Een menu in het spel in je eigen taal:** druk op `Delete`. Elke instelling, de toets en de kosten, met een korte beschrijving.
- **Een bugrapport met één druk:** druk twee keer op `Insert`. De mod bewaart beelden, systeeminfo en logs in zips.

## Wat je nodig hebt

- Euro Truck Simulator 2 of American Truck Simulator (64-bit, DirectX 11).
- Een **NVIDIA RTX**-videokaart met een actueel stuurprogramma. DLAA en DLSS werken alleen op RTX-kaarten.
- Het bestand `nvngx_dlss.dll` (de NVIDIA DLSS-runtime). De mod bevat het niet: zie stap 6.
- Voor VR: een OpenXR-runtime (bijvoorbeeld Virtual Desktop) en het spel gestart met de optie `-openxr`.

## Installeren

1. **Maak een back-up** van de map `bin\win_x64\` van het spel.
2. [Download de nieuwste release](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases/latest): de `.zip` onder **Assets**. Pak hem uit.
3. Open de spelmap, bijvoorbeeld `...\steamapps\common\Euro Truck Simulator 2\bin\win_x64\`
   (of `...\American Truck Simulator\bin\win_x64\`).
4. **Verwijder eerst andere proxy-mods:** verwijder de `dinput8.dll` of `dxgi.dll` van een andere mod in die map.
5. Kopieer **`dinput8.dll`** naar `bin\win_x64\`. Je kunt ook **`dlaa.ini`** daar neerzetten om instellingen en toetsen te wijzigen (niet verplicht).
6. Zet **`nvngx_dlss.dll`** in dezelfde map. Haal hem uit de NVIDIA DLSS SDK (<https://github.com/NVIDIA/DLSS>,
   bestand `lib/Windows_x86_64/rel/nvngx_dlss.dll`) of kopieer hem uit een spel dat hem heeft.
7. Start het spel. DLAA staat aan.

Verwijderen: verwijder `dinput8.dll` (en `dlaa.ini`) uit `bin\win_x64\`.

**Linux / Proton:** installeer op dezelfde manier. Zet in Steam de startopties van het spel op `WINEDLLOVERRIDES="dinput8=n,b" %command%`.

## De belangrijkste toetsen

| Toets | Wat hij doet |
|---|---|
| `Delete` | Opent en sluit het menu. `Up` / `Down` kiezen een rij, `Left` / `Right` wijzigen hem. |
| `End` | Wisselt de modus: uit, DLAA, DLSS. |
| `Insert` twee keer | Bugrapport: bewaart beelden, systeeminfo en logs in `dlaa_selftest\`. |

## Meer

- **De volledige gids (Engels):** [README.md](../../README.md): alle instellingen, toetsen, VR, prestaties en probleemoplossing.
- **Problemen:** [open een issue](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new/choose). Schrijf het alsjeblieft in het Engels.
- **Vertalingen** worden door AI gemaakt en kunnen fouten bevatten.
  [Meld een vertaalprobleem](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new?template=translation.yml).
- Dit is een onofficieel fanproject. Het is niet verbonden met SCS Software of NVIDIA.
