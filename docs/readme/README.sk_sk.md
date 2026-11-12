<!-- Short README in Slovak. The full guide (English): ../../README.md -->
> [!CAUTION]
> **Sťahujte tento mod len z https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases.** Neexistuje žiadna webstránka, Discord,
> Nexus ani Steam Workshop. Každá iná stránka, zrkadlo alebo „inštalátor“, ktorý ho ponúka, nie je moja: nespúšťajte ho.

![ETS2 & ATS DLAA & DLSS Mod](../images/banner.jpg)

# ETS2 & ATS: DLAA & DLSS Injection Mod

NVIDIA **DLAA** a **DLSS** pre **Euro Truck Simulator 2** a **American Truck Simulator**, na bežnej obrazovke aj vo VR.
Mod nahrádza vlastné vyhladzovanie hrán hry technikou DLAA: ploty, drôty, čiary na ceste a tiene prestanú blikať.
DLSS vie obraz aj zväčšiť pre vyššiu snímkovú frekvenciu.

Je to jeden malý súbor, `dinput8.dll`, v priečinku hry. Žiadny launcher, žiadny účet.

- **Stály, ostrý obraz:** žiadne blikanie plotov, drôtov a tieňov. Zrkadlá sa vyhladzujú tiež.
- **Plochý režim aj VR:** jeden súbor pre oboje. Funguje trojitá obrazovka aj Windows HDR.
- **Zrýchlenia pre VR:** foveated rendering (aj so sledovaním očí), DLAA len v strede každého oka, limity snímkovej frekvencie.
- **Menu v hre vo vašom jazyku:** stlačte `Delete`. Každé nastavenie, jeho kláves a cena, s krátkym popisom.
- **Hlásenie chyby jedným stlačením:** stlačte `Insert` dvakrát. Mod uloží obrázky, informácie o systéme a logy do zipov.

## Čo potrebujete

- Euro Truck Simulator 2 alebo American Truck Simulator (64-bitový, DirectX 11).
- Grafickú kartu **NVIDIA RTX** s aktuálnym ovládačom. DLAA a DLSS bežia len na kartách RTX.
- Súbor `nvngx_dlss.dll` (bežová knižnica NVIDIA DLSS). Mod ho neobsahuje: pozri krok 6.
- Pre VR: bežové prostredie OpenXR (napríklad Virtual Desktop) a hru spustenú s voľbou `-openxr`.

## Inštalácia

1. **Zálohujte** priečinok hry `bin\win_x64\`.
2. [Stiahnite najnovšie vydanie](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases/latest): súbor `.zip` v časti **Assets**. Rozbaľte ho.
3. Otvorte priečinok hry, napríklad `...\steamapps\common\Euro Truck Simulator 2\bin\win_x64\`
   (alebo `...\American Truck Simulator\bin\win_x64\`).
4. **Najprv odstráňte iné proxy mody:** zmažte v tom priečinku každý iný `dinput8.dll` alebo `dxgi.dll` z iného modu.
5. Skopírujte **`dinput8.dll`** do `bin\win_x64\`. Môžete tam skopírovať aj **`dlaa.ini`**, aby ste mohli meniť nastavenia a klávesy (nie je povinný).
6. Dajte **`nvngx_dlss.dll`** do rovnakého priečinka. Získate ho z NVIDIA DLSS SDK (<https://github.com/NVIDIA/DLSS>,
   súbor `lib/Windows_x86_64/rel/nvngx_dlss.dll`) alebo ho skopírujte z akejkoľvek hry, ktorá ho má.
7. Spustite hru. DLAA je zapnuté.

Odinštalovanie: zmažte `dinput8.dll` (a `dlaa.ini`) z `bin\win_x64\`.

**Linux / Proton:** nainštalujte rovnako. V službe Steam nastavte spúšťacie voľby hry na `WINEDLLOVERRIDES="dinput8=n,b" %command%`.

## Najdôležitejšie klávesy

| Kláves | Čo robí |
|---|---|
| `Delete` | Otvorí a zatvorí menu. `Up` / `Down` vyberajú riadok, `Left` / `Right` ho menia. |
| `End` | Prepína režim: vyp., DLAA, DLSS. |
| `Insert` dvakrát | Hlásenie chyby: uloží obrázky, informácie o systéme a logy do `dlaa_selftest\`. |

## Viac

- **Úplný návod (po anglicky):** [README.md](../../README.md): všetky nastavenia, klávesy, VR, výkon a riešenie problémov.
- **Problémy:** [otvorte hlásenie (issue)](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new/choose). Napíšte ho, prosím, po anglicky.
- **Preklady** robí AI a môžu obsahovať chyby.
  [Nahláste problém s prekladom](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new?template=translation.yml).
- Toto je neoficiálny fanúšikovský projekt. Nemá žiadnu väzbu na SCS Software ani NVIDIA.
