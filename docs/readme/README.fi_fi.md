<!-- Short README in Finnish. The full guide (English): ../../README.md -->
> [!CAUTION]
> **Lataa tämä modi vain osoitteesta https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases.** Verkkosivua, Discordia,
> Nexusta tai Steam Workshopia ei ole. Mikä tahansa muu sivusto, peilipalvelin tai "asennusohjelma", joka tarjoaa sitä, ei ole minun: älä suorita sitä.

![ETS2 & ATS DLAA & DLSS Mod](../images/banner.jpg)

# ETS2 & ATS: DLAA & DLSS Injection Mod

NVIDIA **DLAA** ja **DLSS** peleihin **Euro Truck Simulator 2** ja **American Truck Simulator**, tavallisella näytöllä ja VR:ssä.
Modi korvaa pelin oman reunanpehmennyksen DLAA:lla: aidat, johdot, tienmerkinnät ja varjot eivät enää välky.
DLSS voi myös skaalata kuvan ylös, jolloin kuvataajuus on korkeampi.

Se on yksi pieni tiedosto, `dinput8.dll`, pelin kansiossa. Ei käynnistintä, ei tiliä.

- **Tasainen, terävä kuva:** ei välkettä aidoissa, johdoissa eikä varjoissa. Myös peilit saavat reunanpehmennyksen.
- **Litteä ja VR:** yksi tiedosto kumpaankin. Kolme näyttöä ja Windows HDR toimivat.
- **VR-nopeutukset:** foveated rendering (myös silmänseurannalla), DLAA vain kunkin silmän keskellä, kuvataajuuden rajat.
- **Pelin sisäinen valikko omalla kielelläsi:** paina `Delete`. Jokainen asetus, sen näppäin ja hinta, lyhyen kuvauksen kera.
- **Virheraportti yhdellä painalluksella:** paina `Insert` kahdesti. Modi tallentaa kuvat, järjestelmätiedot ja lokit zip-tiedostoihin.

## Mitä tarvitset

- Euro Truck Simulator 2 tai American Truck Simulator (64-bittinen, DirectX 11).
- **NVIDIA RTX** -näytönohjain ja ajan tasalla oleva ajuri. DLAA ja DLSS toimivat vain RTX-korteilla.
- Tiedosto `nvngx_dlss.dll` (NVIDIA DLSS -ajonaikainen kirjasto). Modi ei sisällä sitä: katso vaihe 6.
- VR:lle: OpenXR-ajonaikainen kirjasto (esimerkiksi Virtual Desktop) ja peli käynnistettynä valinnalla `-openxr`.

## Asennus

1. **Tee varmuuskopio** pelin kansiosta `bin\win_x64\`.
2. [Lataa uusin julkaisu](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases/latest): `.zip` kohdasta **Assets**. Pura se.
3. Avaa pelin kansio, esimerkiksi `...\steamapps\common\Euro Truck Simulator 2\bin\win_x64\`
   (tai `...\American Truck Simulator\bin\win_x64\`).
4. **Poista ensin muut proxy-modit:** poista kaikki muiden modien `dinput8.dll`- tai `dxgi.dll`-tiedostot siitä kansiosta.
5. Kopioi **`dinput8.dll`** kansioon `bin\win_x64\`. Voit myös kopioida sinne **`dlaa.ini`**-tiedoston asetusten ja näppäinten muuttamista varten (ei pakollinen).
6. Laita **`nvngx_dlss.dll`** samaan kansioon. Hae se NVIDIA DLSS SDK:sta (<https://github.com/NVIDIA/DLSS>,
   tiedosto `lib/Windows_x86_64/rel/nvngx_dlss.dll`) tai kopioi se mistä tahansa pelistä, jossa se on.
7. Käynnistä peli. DLAA on päällä.

Asennuksen poisto: poista `dinput8.dll` (ja `dlaa.ini`) kansiosta `bin\win_x64\`.

**Linux / Proton:** asenna samalla tavalla. Aseta Steamissä pelin käynnistysasetuksiksi `WINEDLLOVERRIDES="dinput8=n,b" %command%`.

## Tärkeimmät näppäimet

| Näppäin | Mitä se tekee |
|---|---|
| `Delete` | Avaa ja sulkee valikon. `Up` / `Down` valitsevat rivin, `Left` / `Right` muuttavat sitä. |
| `End` | Vaihtaa tilaa: pois, DLAA, DLSS. |
| `Insert` kahdesti | Virheraportti: tallentaa kuvat, järjestelmätiedot ja lokit kansioon `dlaa_selftest\`. |

## Lisää

- **Täysi opas (englanniksi):** [README.md](../../README.md): kaikki asetukset, näppäimet, VR, suorituskyky ja vianetsintä.
- **Ongelmat:** [avaa issue](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new/choose). Kirjoita se englanniksi.
- **Käännökset** on tehnyt tekoäly, ja niissä voi olla virheitä.
  [Ilmoita käännösongelmasta](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new?template=translation.yml).
- Tämä on epävirallinen fanien projekti. Se ei liity SCS Softwareen tai NVIDIAan.
