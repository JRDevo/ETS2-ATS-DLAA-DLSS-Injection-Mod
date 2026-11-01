<!-- Short README in Slovenian. The full guide (English): ../../README.md -->
> [!CAUTION]
> **Ta mod prenesite samo z https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases.** Ni spletne strani, Discorda,
> Nexusa ali Steam Workshopa. Vsaka druga stran, zrcalo ali „namestitveni program“, ki ga ponuja, ni moja: ne zaženite je.

![ETS2 & ATS DLAA & DLSS Mod](../images/banner.jpg)

# ETS2 & ATS: DLAA & DLSS Injection Mod

NVIDIA **DLAA** in **DLSS** za **Euro Truck Simulator 2** in **American Truck Simulator**, na navadnem zaslonu in v VR.
Mod nadomesti igrino lastno glajenje robov z DLAA: ograje, žice, talne označbe in sence prenehajo utripati.
DLSS lahko sliko tudi poveča za višjo hitrost sličic.

To je ena majhna datoteka, `dinput8.dll`, v mapi igre. Brez zaganjalnika, brez računa.

- **Mirna, ostra slika:** brez utripanja ograj, žic in senc. Tudi ogledala so zglajena.
- **Ravni način in VR:** ena datoteka za oba. Delujejo trojni zasloni in Windows HDR.
- **Pohitritve za VR:** foveated rendering (tudi s sledenjem očem), DLAA samo na sredini vsakega očesa, omejitve hitrosti sličic.
- **Meni v igri v vašem jeziku:** pritisnite `Delete`. Vsaka nastavitev, njena tipka in cena, s kratkim opisom.
- **Poročilo o napaki z enim pritiskom:** dvakrat pritisnite `Insert`. Mod shrani slike, podatke o sistemu in dnevnike v zip datoteke.

## Kaj potrebujete

- Euro Truck Simulator 2 ali American Truck Simulator (64-bitni, DirectX 11).
- Grafično kartico **NVIDIA RTX** z aktualnim gonilnikom. DLAA in DLSS delujeta samo na karticah RTX.
- Datoteko `nvngx_dlss.dll` (izvajalno okolje NVIDIA DLSS). Mod je ne vsebuje: glejte korak 6.
- Za VR: izvajalno okolje OpenXR (na primer Virtual Desktop) in igro, zagnano z možnostjo `-openxr`.

## Namestitev

1. **Naredite varnostno kopijo** mape igre `bin\win_x64\`.
2. [Prenesite najnovejšo izdajo](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases/latest): datoteko `.zip` pod **Assets**. Razpakirajte jo.
3. Odprite mapo igre, na primer `...\steamapps\common\Euro Truck Simulator 2\bin\win_x64\`
   (ali `...\American Truck Simulator\bin\win_x64\`).
4. **Najprej odstranite druge proxy mode:** v tej mapi izbrišite vsak drug `dinput8.dll` ali `dxgi.dll` iz drugega moda.
5. Kopirajte **`dinput8.dll`** v `bin\win_x64\`. Tja lahko kopirate tudi **`dlaa.ini`**, da spremenite nastavitve in tipke (ni obvezen).
6. Dajte **`nvngx_dlss.dll`** v isto mapo. Dobite ga v NVIDIA DLSS SDK (<https://github.com/NVIDIA/DLSS>,
   datoteka `lib/Windows_x86_64/rel/nvngx_dlss.dll`) ali ga kopirajte iz katere koli igre, ki ga ima.
7. Zaženite igro. DLAA je vklopljen.

Odstranitev: izbrišite `dinput8.dll` (in `dlaa.ini`) iz `bin\win_x64\`.

**Linux / Proton:** namestite enako. V Steamu nastavite zagonske možnosti igre na `WINEDLLOVERRIDES="dinput8=n,b" %command%`.

## Najpomembnejše tipke

| Tipka | Kaj naredi |
|---|---|
| `Delete` | Odpre in zapre meni. `Up` / `Down` izbirata vrstico, `Left` / `Right` jo spreminjata. |
| `End` | Preklaplja način: izk., DLAA, DLSS. |
| `Insert` dvakrat | Poročilo o napaki: shrani slike, podatke o sistemu in dnevnike v `dlaa_selftest\`. |

## Več

- **Celoten vodnik (v angleščini):** [README.md](../../README.md): vse nastavitve, tipke, VR, zmogljivost in odpravljanje težav.
- **Težave:** [odprite prijavo (issue)](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new/choose). Napišite jo, prosim, v angleščini.
- **Prevode** naredi umetna inteligenca in lahko vsebujejo napake.
  [Prijavite težavo s prevodom](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new?template=translation.yml).
- To je neuradni projekt oboževalcev. Ni povezan s SCS Software ali NVIDIA.
