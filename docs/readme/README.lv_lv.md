<!-- Short README in Latvian. The full guide (English): ../../README.md -->
> [!CAUTION]
> **Lejupielādējiet šo modifikāciju tikai no https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases.** Nav ne mājaslapas, ne Discord,
> ne Nexus, ne Steam Workshop. Jebkura cita vietne, spogulis vai „instalētājs”, kas to piedāvā, nav mans: nepalaidiet to.

![ETS2 & ATS DLAA & DLSS Mod](../images/banner.jpg)

# ETS2 & ATS: DLAA & DLSS Injection modifikācija

NVIDIA **DLAA** un **DLSS** spēlēm **Euro Truck Simulator 2** un **American Truck Simulator**, uz plakana ekrāna un VR.
Modifikācija aizstāj spēles paša izlīdzināšanu ar DLAA: žogi, vadi, ceļa marķējums un ēnas vairs nemirgo.
DLSS var arī palielināt attēlu augstākam kadru ātrumam.

Tas ir viens neliels fails, `dinput8.dll`, spēles mapē. Bez palaidēja, bez konta.

- **Mierīgs un ass attēls:** bez mirgošanas uz žogiem, vadiem un ēnām. Spoguļi arī tiek izlīdzināti.
- **Plakanais režīms un VR:** viens fails abiem. Darbojas trīs ekrāni un Windows HDR.
- **VR paātrinājumi:** foveated renderēšana (arī ar acu izsekošanu), DLAA tikai katras acs vidū, kadru ātruma robežas.
- **Spēles izvēlne jūsu valodā:** nospiediet `Delete`. Katrs iestatījums, tā taustiņš un cena, ar īsu aprakstu.
- **Kļūdas ziņojums ar vienu taustiņu:** nospiediet `Insert` divreiz. Modifikācija saglabā attēlus, sistēmas info un žurnālus zip failos.

## Kas jums vajadzīgs

- Euro Truck Simulator 2 vai American Truck Simulator (64 bitu, DirectX 11).
- **NVIDIA RTX** videokarte ar aktuālu draiveri. DLAA un DLSS darbojas tikai uz RTX kartēm.
- Fails `nvngx_dlss.dll` (NVIDIA DLSS izpildlaiks). Modifikācija to neietver: skatiet 6. soli.
- VR gadījumam: OpenXR izpildlaiks (piemēram, Virtual Desktop) un spēle, kas palaista ar opciju `-openxr`.

## Instalēšana

1. **Izveidojiet rezerves kopiju** spēles mapei `bin\win_x64\`.
2. [Lejupielādējiet jaunāko laidienu](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases/latest): `.zip` failu sadaļā **Assets**. Izpakojiet to.
3. Atveriet spēles mapi, piemēram, `...\steamapps\common\Euro Truck Simulator 2\bin\win_x64\`
   (vai `...\American Truck Simulator\bin\win_x64\`).
4. **Vispirms noņemiet citas starpniekmodifikācijas:** dzēsiet citas modifikācijas `dinput8.dll` vai `dxgi.dll` šajā mapē.
5. Nokopējiet **`dinput8.dll`** mapē `bin\win_x64\`. Tur varat nokopēt arī **`dlaa.ini`**, lai mainītu iestatījumus un taustiņus (nav obligāti).
6. Ielieciet **`nvngx_dlss.dll`** tajā pašā mapē. Ņemiet to no NVIDIA DLSS SDK (<https://github.com/NVIDIA/DLSS>,
   fails `lib/Windows_x86_64/rel/nvngx_dlss.dll`) vai nokopējiet no jebkuras spēles, kurai tas ir.
7. Palaidiet spēli. DLAA ir ieslēgts.

Lai noņemtu: dzēsiet `dinput8.dll` (un `dlaa.ini`) no `bin\win_x64\`.

**Linux / Proton:** instalējiet tāpat. Steam iestatiet spēles palaišanas opcijas uz `WINEDLLOVERRIDES="dinput8=n,b" %command%`.

## Svarīgākie taustiņi

| Taustiņš | Ko dara |
|---|---|
| `Delete` | Atver un aizver izvēlni. `Up` / `Down` izvēlas rindu, `Left` / `Right` to maina. |
| `End` | Pārslēdz režīmu: izslēgts, DLAA, DLSS. |
| `Insert` divreiz | Kļūdas ziņojums: saglabā attēlus, sistēmas info un žurnālus mapē `dlaa_selftest\`. |

## Vairāk

- **Pilnā rokasgrāmata (angliski):** [README.md](../../README.md): visi iestatījumi, taustiņi, VR, veiktspēja un problēmu risināšana.
- **Problēmas:** [izveidojiet issue](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new/choose). Lūdzu, rakstiet angliski.
- **Tulkojumus** veidojis MI, un tajos var būt kļūdas.
  [Ziņot par tulkojuma problēmu](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new?template=translation.yml).
- Šis ir neoficiāls fanu projekts. Tas nav saistīts ar SCS Software vai NVIDIA.
