<!-- Short README in Lithuanian. The full guide (English): ../../README.md -->
> [!CAUTION]
> **Atsisiųskite šią modifikaciją tik iš https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases.** Nėra jokios svetainės, Discord,
> Nexus ar Steam Workshop. Bet kuri kita svetainė, veidrodis ar „diegimo programa“, siūlanti ją, nėra mano: nepaleiskite jos.

![ETS2 & ATS DLAA & DLSS Mod](../images/banner.jpg)

# ETS2 & ATS: DLAA & DLSS Injection Mod

NVIDIA **DLAA** ir **DLSS** žaidimams **Euro Truck Simulator 2** ir **American Truck Simulator**, įprastame ekrane ir VR.
Modifikacija pakeičia paties žaidimo kraštų glotninimą DLAA: tvoros, laidai, kelio žymėjimai ir šešėliai nustoja mirgėti.
DLSS taip pat gali padidinti vaizdą, kad kadrų dažnis būtų didesnis.

Tai vienas mažas failas, `dinput8.dll`, žaidimo aplanke. Be paleidimo programos, be paskyros.

- **Stabilus, ryškus vaizdas:** tvoros, laidai ir šešėliai nemirga. Veidrodžiai taip pat glotninami.
- **Plokščias režimas ir VR:** vienas failas abiem. Veikia trigubi ekranai ir Windows HDR.
- **VR pagreitinimai:** foveated rendering (taip pat su akių sekimu), DLAA tik kiekvienos akies viduryje, kadrų dažnio ribos.
- **Meniu žaidime jūsų kalba:** paspauskite `Delete`. Kiekvienas nustatymas, jo klavišas ir kaina, su trumpu aprašu.
- **Pranešimas apie klaidą vienu paspaudimu:** du kartus paspauskite `Insert`. Modifikacija įrašo vaizdus, sistemos informaciją ir žurnalus į zip failus.

## Ko jums reikia

- Euro Truck Simulator 2 arba American Truck Simulator (64 bitų, DirectX 11).
- **NVIDIA RTX** vaizdo plokštė su naujausia tvarkykle. DLAA ir DLSS veikia tik su RTX plokštėmis.
- Failas `nvngx_dlss.dll` (NVIDIA DLSS vykdymo biblioteka). Modifikacija jo neturi: žr. 6 žingsnį.
- VR: OpenXR vykdymo aplinka (pavyzdžiui, Virtual Desktop) ir žaidimas, paleistas su parinktimi `-openxr`.

## Diegimas

1. **Pasidarykite atsarginę kopiją** žaidimo aplanko `bin\win_x64\`.
2. [Atsisiųskite naujausią leidimą](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases/latest): `.zip` failą skiltyje **Assets**. Išarchyvuokite jį.
3. Atidarykite žaidimo aplanką, pavyzdžiui, `...\steamapps\common\Euro Truck Simulator 2\bin\win_x64\`
   (arba `...\American Truck Simulator\bin\win_x64\`).
4. **Pirmiausia pašalinkite kitas proxy modifikacijas:** tame aplanke ištrinkite bet kurį kitos modifikacijos `dinput8.dll` ar `dxgi.dll`.
5. Nukopijuokite **`dinput8.dll`** į `bin\win_x64\`. Ten galite nukopijuoti ir **`dlaa.ini`**, kad keistumėte nustatymus ir klavišus (nebūtina).
6. Įdėkite **`nvngx_dlss.dll`** į tą patį aplanką. Gaukite jį iš NVIDIA DLSS SDK (<https://github.com/NVIDIA/DLSS>,
   failas `lib/Windows_x86_64/rel/nvngx_dlss.dll`) arba nukopijuokite iš bet kurio žaidimo, kuris jį turi.
7. Paleiskite žaidimą. DLAA įjungtas.

Norėdami pašalinti: ištrinkite `dinput8.dll` (ir `dlaa.ini`) iš `bin\win_x64\`.

**Linux / Proton:** įdiekite taip pat. Steam nustatykite žaidimo paleidimo parinktis į `WINEDLLOVERRIDES="dinput8=n,b" %command%`.

## Svarbiausi klavišai

| Klavišas | Ką daro |
|---|---|
| `Delete` | Atidaro ir uždaro meniu. `Up` / `Down` pasirenka eilutę, `Left` / `Right` ją keičia. |
| `End` | Perjungia režimą: išjungta, DLAA, DLSS. |
| `Insert` du kartus | Pranešimas apie klaidą: įrašo vaizdus, sistemos informaciją ir žurnalus į `dlaa_selftest\`. |

## Daugiau

- **Pilnas vadovas (angliškai):** [README.md](../../README.md): visi nustatymai, klavišai, VR, našumas ir problemų sprendimas.
- **Problemos:** [atidarykite pranešimą (issue)](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new/choose). Rašykite jį, prašau, angliškai.
- **Vertimus** atlieka dirbtinis intelektas, todėl juose gali būti klaidų.
  [Pranešti apie vertimo problemą](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new?template=translation.yml).
- Tai neoficialus gerbėjų projektas. Jis nesusijęs su SCS Software ar NVIDIA.
