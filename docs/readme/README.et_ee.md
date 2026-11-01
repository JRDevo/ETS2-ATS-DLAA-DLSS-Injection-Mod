<!-- Short README in Estonian. The full guide (English): ../../README.md -->
> [!CAUTION]
> **Laadi see mod alla ainult aadressilt https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases.** Veebilehte, Discordi,
> Nexust ega Steam Workshopi ei ole. Iga muu leht, peeglikoopia või "installer", mis seda pakub, ei ole minu oma: ära käivita seda.

![ETS2 & ATS DLAA & DLSS Mod](../images/banner.jpg)

# ETS2 & ATS: DLAA & DLSS Injection Mod

NVIDIA **DLAA** ja **DLSS** mängudele **Euro Truck Simulator 2** ja **American Truck Simulator**, tavalisel ekraanil ja VR-is.
Mod asendab mängu enda kantide silumise DLAA-ga: aiad, juhtmed, teemarkeering ja varjud ei vilgu enam.
DLSS saab pilti ka üles skaleerida, et kaadrisagedus oleks kõrgem.

See on üks väike fail, `dinput8.dll`, mängu kaustas. Ei mingit käivitajat, ei mingit kontot.

- **Rahulik, terav pilt:** aedadel, juhtmetel ja varjudel pole vilkumist. Ka peeglid saavad kantide silumise.
- **Lame ja VR:** üks fail mõlemale. Kolm ekraani ja Windows HDR töötavad.
- **VR kiirendid:** foveated rendering (ka silmaseirega), DLAA ainult iga silma keskel, kaadrisageduse piirid.
- **Mängusisene menüü sinu keeles:** vajuta `Delete`. Iga seade, selle klahv ja hind, lühikese kirjeldusega.
- **Veaaruanne ühe vajutusega:** vajuta kaks korda `Insert`. Mod salvestab pildid, süsteemiinfo ja logid zip-failidesse.

## Mida sa vajad

- Euro Truck Simulator 2 või American Truck Simulator (64-bitine, DirectX 11).
- **NVIDIA RTX** graafikakaart ja värske draiver. DLAA ja DLSS töötavad ainult RTX kaartidel.
- Fail `nvngx_dlss.dll` (NVIDIA DLSS käitusaegne teek). Mod seda ei sisalda: vaata sammu 6.
- VR-i jaoks: OpenXR käitusaegne teek (näiteks Virtual Desktop) ja mäng käivitatud valikuga `-openxr`.

## Paigaldus

1. **Tee varukoopia** mängu kaustast `bin\win_x64\`.
2. [Laadi alla uusim väljalase](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases/latest): `.zip` jaotise **Assets** all. Paki see lahti.
3. Ava mängu kaust, näiteks `...\steamapps\common\Euro Truck Simulator 2\bin\win_x64\`
   (või `...\American Truck Simulator\bin\win_x64\`).
4. **Eemalda kõigepealt teised proxy-modid:** kustuta selles kaustas iga teise modi `dinput8.dll` või `dxgi.dll`.
5. Kopeeri **`dinput8.dll`** kausta `bin\win_x64\`. Seadete ja klahvide muutmiseks võid sinna kopeerida ka **`dlaa.ini`** (pole kohustuslik).
6. Pane **`nvngx_dlss.dll`** samasse kausta. Võta see NVIDIA DLSS SDK-st (<https://github.com/NVIDIA/DLSS>,
   fail `lib/Windows_x86_64/rel/nvngx_dlss.dll`) või kopeeri see mis tahes mängust, millel see on.
7. Käivita mäng. DLAA on sees.

Eemaldamiseks: kustuta `dinput8.dll` (ja `dlaa.ini`) kaustast `bin\win_x64\`.

**Linux / Proton:** paigalda samamoodi. Pane Steamis mängu käivitusvalikuteks `WINEDLLOVERRIDES="dinput8=n,b" %command%`.

## Kõige olulisemad klahvid

| Klahv | Mida see teeb |
|---|---|
| `Delete` | Avab ja sulgeb menüü. `Up` / `Down` valivad rea, `Left` / `Right` muudavad seda. |
| `End` | Vahetab režiimi: väljas, DLAA, DLSS. |
| `Insert` kaks korda | Veaaruanne: salvestab pildid, süsteemiinfo ja logid kausta `dlaa_selftest\`. |

## Veel

- **Täielik juhend (inglise keeles):** [README.md](../../README.md): kõik seaded, klahvid, VR, jõudlus ja tõrkeotsing.
- **Probleemid:** [ava issue](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new/choose). Palun kirjuta see inglise keeles.
- **Tõlked** teeb tehisintellekt ja neis võib olla vigu.
  [Teata tõlkeprobleemist](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new?template=translation.yml).
- See on mitteametlik fännide projekt. See ei ole seotud SCS Software'i ega NVIDIAga.
