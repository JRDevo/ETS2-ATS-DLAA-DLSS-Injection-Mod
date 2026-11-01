<!-- Short README in Danish. The full guide (English): ../../README.md -->
> [!CAUTION]
> **Hent kun denne mod fra https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases.** Der er ingen hjemmeside, ingen Discord,
> ingen Nexus og intet Steam Workshop. Enhver anden side, mirror eller "installer", der tilbyder den, er ikke min: kør den ikke.

![ETS2 & ATS DLAA & DLSS Mod](../images/banner.jpg)

# ETS2 & ATS: DLAA & DLSS Injection Mod

NVIDIA **DLAA** og **DLSS** til **Euro Truck Simulator 2** og **American Truck Simulator**, på en almindelig skærm og i VR.
Modden erstatter spillets egen anti-aliasing med DLAA: hegn, ledninger, vejstriber og skygger flimrer ikke mere.
DLSS kan også opskalere billedet for en højere billedhastighed.

Det er én lille fil, `dinput8.dll`, i spilmappen. Ingen launcher, ingen konto.

- **Et roligt, skarpt billede:** ingen flimmer på hegn, ledninger og skygger. Spejle får også anti-aliasing.
- **Flad og VR:** én fil til begge. Tre skærme og Windows HDR virker.
- **VR-fartforøgere:** foveated rendering (også med eyetracking), DLAA kun i midten af hvert øje, grænser for billedhastighed.
- **En menu i spillet på dit sprog:** tryk på `Delete`. Hver indstilling, dens tast og dens pris, med en kort beskrivelse.
- **En fejlrapport med et enkelt tryk:** tryk to gange på `Insert`. Modden gemmer billeder, systeminfo og logfiler i zip-filer.

## Hvad du skal bruge

- Euro Truck Simulator 2 eller American Truck Simulator (64-bit, DirectX 11).
- Et **NVIDIA RTX**-grafikkort med en aktuel driver. DLAA og DLSS kører kun på RTX-kort.
- Filen `nvngx_dlss.dll` (NVIDIA DLSS-runtime). Modden indeholder den ikke: se trin 6.
- Til VR: en OpenXR-runtime (for eksempel Virtual Desktop) og spillet startet med indstillingen `-openxr`.

## Installation

1. **Lav en sikkerhedskopi** af spillets mappe `bin\win_x64\`.
2. [Hent den nyeste udgave](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases/latest): `.zip`-filen under **Assets**. Pak den ud.
3. Åbn spilmappen, for eksempel `...\steamapps\common\Euro Truck Simulator 2\bin\win_x64\`
   (eller `...\American Truck Simulator\bin\win_x64\`).
4. **Fjern andre proxy-mods først:** slet en anden mods `dinput8.dll` eller `dxgi.dll` i den mappe.
5. Kopier **`dinput8.dll`** til `bin\win_x64\`. Du kan også kopiere **`dlaa.ini`** dertil for at ændre indstillinger og taster (ikke påkrævet).
6. Læg **`nvngx_dlss.dll`** i samme mappe. Hent den fra NVIDIA DLSS SDK (<https://github.com/NVIDIA/DLSS>,
   filen `lib/Windows_x86_64/rel/nvngx_dlss.dll`) eller kopier den fra et spil, der har den.
7. Start spillet. DLAA er slået til.

Sådan afinstallerer du: slet `dinput8.dll` (og `dlaa.ini`) fra `bin\win_x64\`.

**Linux / Proton:** installer på samme måde. Sæt spillets startindstillinger i Steam til `WINEDLLOVERRIDES="dinput8=n,b" %command%`.

## De vigtigste taster

| Tast | Hvad den gør |
|---|---|
| `Delete` | Åbner og lukker menuen. `Up` / `Down` vælger en række, `Left` / `Right` ændrer den. |
| `End` | Skifter tilstand: fra, DLAA, DLSS. |
| `Insert` to gange | Fejlrapport: gemmer billeder, systeminfo og logfiler i `dlaa_selftest\`. |

## Mere

- **Den fulde vejledning (engelsk):** [README.md](../../README.md): alle indstillinger, taster, VR, ydelse og fejlfinding.
- **Problemer:** [opret et issue](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new/choose). Skriv det gerne på engelsk.
- **Oversættelser** er lavet af AI og kan indeholde fejl.
  [Meld et oversættelsesproblem](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new?template=translation.yml).
- Dette er et uofficielt fanprojekt. Det er ikke tilknyttet SCS Software eller NVIDIA.
