<!-- Short README in Norwegian (Bokmål). The full guide (English): ../../README.md -->
> [!CAUTION]
> **Last ned denne modden bare fra https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases.** Det finnes ingen nettside, ingen Discord,
> ingen Nexus og ingen Steam Workshop. Enhver annen side, mirror eller "installer" som tilbyr den, er ikke min: ikke kjør den.

![ETS2 & ATS DLAA & DLSS Mod](../images/banner.jpg)

# ETS2 & ATS: DLAA & DLSS Injection Mod

NVIDIA **DLAA** og **DLSS** for **Euro Truck Simulator 2** og **American Truck Simulator**, på en vanlig skjerm og i VR.
Modden erstatter spillets egen anti-aliasing med DLAA: gjerder, ledninger, veimerking og skygger flimrer ikke lenger.
DLSS kan også oppskalere bildet, for høyere bildefrekvens.

Det er én liten fil, `dinput8.dll`, i spillmappen. Ingen launcher, ingen konto.

- **Et rolig, skarpt bilde:** ingen flimmer på gjerder, ledninger og skygger. Speil får også anti-aliasing.
- **Flat og VR:** én fil for begge. Tre skjermer og Windows HDR fungerer.
- **VR-fartsøkere:** foveated rendering (også med eyetracking), DLAA bare i midten av hvert øye, grenser for bildefrekvens.
- **En meny i spillet på ditt språk:** trykk `Delete`. Hver innstilling, tasten og prisen, med en kort beskrivelse.
- **En feilrapport med ett trykk:** trykk `Insert` to ganger. Modden lagrer bilder, systeminfo og logger i zip-filer.

## Hva du trenger

- Euro Truck Simulator 2 eller American Truck Simulator (64-bit, DirectX 11).
- Et **NVIDIA RTX**-grafikkort med en oppdatert driver. DLAA og DLSS kjører bare på RTX-kort.
- Filen `nvngx_dlss.dll` (NVIDIA DLSS-runtime). Modden inneholder den ikke: se trinn 6.
- For VR: en OpenXR-runtime (for eksempel Virtual Desktop) og spillet startet med alternativet `-openxr`.

## Installasjon

1. **Ta en sikkerhetskopi** av spillets mappe `bin\win_x64\`.
2. [Last ned den nyeste utgaven](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases/latest): `.zip`-filen under **Assets**. Pakk den ut.
3. Åpne spillmappen, for eksempel `...\steamapps\common\Euro Truck Simulator 2\bin\win_x64\`
   (eller `...\American Truck Simulator\bin\win_x64\`).
4. **Fjern andre proxy-modder først:** slett en annen mods `dinput8.dll` eller `dxgi.dll` i den mappen.
5. Kopier **`dinput8.dll`** til `bin\win_x64\`. Du kan også kopiere **`dlaa.ini`** dit for å endre innstillinger og taster (ikke påkrevd).
6. Legg **`nvngx_dlss.dll`** i samme mappe. Hent den fra NVIDIA DLSS SDK (<https://github.com/NVIDIA/DLSS>,
   filen `lib/Windows_x86_64/rel/nvngx_dlss.dll`) eller kopier den fra et spill som har den.
7. Start spillet. DLAA er på.

For å avinstallere: slett `dinput8.dll` (og `dlaa.ini`) fra `bin\win_x64\`.

**Linux / Proton:** installer på samme måte. Sett spillets startalternativer i Steam til `WINEDLLOVERRIDES="dinput8=n,b" %command%`.

## De viktigste tastene

| Tast | Hva den gjør |
|---|---|
| `Delete` | Åpner og lukker menyen. `Up` / `Down` velger en rad, `Left` / `Right` endrer den. |
| `End` | Bytter modus: av, DLAA, DLSS. |
| `Insert` to ganger | Feilrapport: lagrer bilder, systeminfo og logger i `dlaa_selftest\`. |

## Mer

- **Den fullstendige veiledningen (engelsk):** [README.md](../../README.md): alle innstillinger, taster, VR, ytelse og feilsøking.
- **Problemer:** [opprett et issue](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new/choose). Skriv det gjerne på engelsk.
- **Oversettelser** lages av KI og kan inneholde feil.
  [Meld fra om et oversettelsesproblem](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new?template=translation.yml).
- Dette er et uoffisielt fanprosjekt. Det har ingen tilknytning til SCS Software eller NVIDIA.
