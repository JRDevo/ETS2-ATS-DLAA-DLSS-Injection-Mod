<!-- Short README in Swedish. The full guide (English): ../../README.md -->
> [!CAUTION]
> **Ladda bara ner den här modden från https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases.** Det finns ingen webbplats, ingen Discord,
> ingen Nexus och ingen Steam Workshop. Alla andra sidor, speglingar eller "installers" som erbjuder den är inte mina: kör den inte.

![ETS2 & ATS DLAA & DLSS Mod](../images/banner.jpg)

# ETS2 & ATS: DLAA & DLSS Injection Mod

NVIDIA **DLAA** och **DLSS** för **Euro Truck Simulator 2** och **American Truck Simulator**, på en vanlig skärm och i VR.
Modden ersätter spelets egen kantutjämning med DLAA: stängsel, ledningar, vägmarkeringar och skuggor flimrar inte längre.
DLSS kan också skala upp bilden, för högre bildfrekvens.

Det är en enda liten fil, `dinput8.dll`, i spelmappen. Ingen launcher, inget konto.

- **En stadig, skarp bild:** inget flimmer på stängsel, ledningar och skuggor. Speglar får också kantutjämning.
- **Platt och VR:** en fil för båda. Tre skärmar och Windows HDR fungerar.
- **VR-snabbare:** foveated rendering (även med eyetracking), DLAA bara i mitten av varje öga, gränser för bildfrekvens.
- **En meny i spelet på ditt språk:** tryck på `Delete`. Varje inställning, dess tangent och dess kostnad, med en kort beskrivning.
- **En felrapport med ett tryck:** tryck två gånger på `Insert`. Modden sparar bilder, systeminfo och loggar i zip-filer.

## Vad du behöver

- Euro Truck Simulator 2 eller American Truck Simulator (64-bit, DirectX 11).
- Ett **NVIDIA RTX**-grafikkort med en aktuell drivrutin. DLAA och DLSS fungerar bara på RTX-kort.
- Filen `nvngx_dlss.dll` (NVIDIA DLSS-runtime). Modden innehåller den inte: se steg 6.
- För VR: en OpenXR-runtime (till exempel Virtual Desktop) och spelet startat med alternativet `-openxr`.

## Installation

1. **Gör en säkerhetskopia** av spelets mapp `bin\win_x64\`.
2. [Ladda ner den senaste utgåvan](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases/latest): `.zip`-filen under **Assets**. Packa upp den.
3. Öppna spelmappen, till exempel `...\steamapps\common\Euro Truck Simulator 2\bin\win_x64\`
   (eller `...\American Truck Simulator\bin\win_x64\`).
4. **Ta bort andra proxy-modder först:** radera en annan mods `dinput8.dll` eller `dxgi.dll` i den mappen.
5. Kopiera **`dinput8.dll`** till `bin\win_x64\`. Du kan också kopiera **`dlaa.ini`** dit för att ändra inställningar och tangenter (krävs inte).
6. Lägg **`nvngx_dlss.dll`** i samma mapp. Hämta den från NVIDIA DLSS SDK (<https://github.com/NVIDIA/DLSS>,
   filen `lib/Windows_x86_64/rel/nvngx_dlss.dll`) eller kopiera den från ett spel som har den.
7. Starta spelet. DLAA är på.

För att avinstallera: radera `dinput8.dll` (och `dlaa.ini`) från `bin\win_x64\`.

**Linux / Proton:** installera på samma sätt. Sätt spelets startalternativ i Steam till `WINEDLLOVERRIDES="dinput8=n,b" %command%`.

## De viktigaste tangenterna

| Tangent | Vad den gör |
|---|---|
| `Delete` | Öppnar och stänger menyn. `Up` / `Down` väljer en rad, `Left` / `Right` ändrar den. |
| `End` | Byter läge: av, DLAA, DLSS. |
| `Insert` två gånger | Felrapport: sparar bilder, systeminfo och loggar i `dlaa_selftest\`. |

## Mer

- **Den fullständiga guiden (engelska):** [README.md](../../README.md): alla inställningar, tangenter, VR, prestanda och felsökning.
- **Problem:** [öppna ett issue](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new/choose). Skriv det gärna på engelska.
- **Översättningar** görs av AI och kan innehålla fel.
  [Rapportera ett översättningsproblem](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new?template=translation.yml).
- Detta är ett inofficiellt fanprojekt. Det har ingen koppling till SCS Software eller NVIDIA.
