<!-- Short README in Czech. The full guide (English): ../../README.md -->
> [!CAUTION]
> **Stahujte tento mod jen z https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases.** Neexistuje žádný web, Discord,
> Nexus ani Steam Workshop. Každý jiný web, zrcadlo nebo „instalátor“, který ho nabízí, není můj: nespouštějte ho.

![ETS2 & ATS DLAA & DLSS Mod](../images/banner.jpg)

# ETS2 & ATS: DLAA & DLSS Injection Mod

NVIDIA **DLAA** a **DLSS** pro **Euro Truck Simulator 2** a **American Truck Simulator**, na běžné obrazovce i ve VR.
Mod nahrazuje vlastní vyhlazování hran hry technikou DLAA: ploty, dráty, čáry na silnici a stíny přestanou blikat.
DLSS umí obraz také zvětšit pro vyšší snímkovou frekvenci.

Je to jeden malý soubor, `dinput8.dll`, ve složce hry. Žádný launcher, žádný účet.

- **Stálý, ostrý obraz:** žádné blikání plotů, drátů a stínů. Zrcátka se vyhlazují také.
- **Plochý režim i VR:** jeden soubor pro oboje. Funguje trojitá obrazovka i Windows HDR.
- **Zrychlení pro VR:** foveated rendering (také se sledováním očí), DLAA jen uprostřed každého oka, limity snímkové frekvence.
- **Menu ve hře ve vašem jazyce:** stiskněte `Delete`. Každé nastavení, jeho klávesa a cena, s krátkým popisem.
- **Hlášení chyby jedním stiskem:** stiskněte `Insert` dvakrát. Mod uloží obrázky, informace o systému a logy do zipů.

## Co potřebujete

- Euro Truck Simulator 2 nebo American Truck Simulator (64bitový, DirectX 11).
- Grafickou kartu **NVIDIA RTX** s aktuálním ovladačem. DLAA a DLSS běží jen na kartách RTX.
- Soubor `nvngx_dlss.dll` (běhová knihovna NVIDIA DLSS). Mod ho neobsahuje: viz krok 6.
- Pro VR: běhové prostředí OpenXR (například Virtual Desktop) a hru spuštěnou s volbou `-openxr`.

## Instalace

1. **Zálohujte** složku hry `bin\win_x64\`.
2. [Stáhněte nejnovější verzi](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases/latest): soubor `.zip` v části **Assets**. Rozbalte ho.
3. Otevřete složku hry, například `...\steamapps\common\Euro Truck Simulator 2\bin\win_x64\`
   (nebo `...\American Truck Simulator\bin\win_x64\`).
4. **Nejdřív odstraňte jiné proxy mody:** smažte v té složce každý jiný `dinput8.dll` nebo `dxgi.dll` z jiného modu.
5. Zkopírujte **`dinput8.dll`** do `bin\win_x64\`. Můžete tam zkopírovat i **`dlaa.ini`**, abyste mohli měnit nastavení a klávesy (není povinný).
6. Dejte **`nvngx_dlss.dll`** do stejné složky. Získáte ho z NVIDIA DLSS SDK (<https://github.com/NVIDIA/DLSS>,
   soubor `lib/Windows_x86_64/rel/nvngx_dlss.dll`) nebo ho zkopírujte z jakékoli hry, která ho má.
7. Spusťte hru. DLAA je zapnuté.

Odinstalování: smažte `dinput8.dll` (a `dlaa.ini`) z `bin\win_x64\`.

**Linux / Proton:** nainstalujte stejně. Ve Steamu nastavte spouštěcí volby hry na `WINEDLLOVERRIDES="dinput8=n,b" %command%`.

## Nejdůležitější klávesy

| Klávesa | Co dělá |
|---|---|
| `Delete` | Otevře a zavře menu. `Up` / `Down` vybírají řádek, `Left` / `Right` ho mění. |
| `End` | Přepíná režim: vyp., DLAA, DLSS. |
| `Insert` dvakrát | Hlášení chyby: uloží obrázky, informace o systému a logy do `dlaa_selftest\`. |

## Další informace

- **Úplný návod (anglicky):** [README.md](../../README.md): všechna nastavení, klávesy, VR, výkon a řešení problémů.
- **Problémy:** [otevřete hlášení (issue)](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new/choose). Napište ho prosím anglicky.
- **Překlady** dělá AI a mohou obsahovat chyby.
  [Nahlaste problém s překladem](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new?template=translation.yml).
- Toto je neoficiální fanouškovský projekt. Nemá žádnou vazbu na SCS Software ani NVIDIA.
