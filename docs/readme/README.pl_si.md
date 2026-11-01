<!-- Short README in Silesian. The full guide (English): ../../README.md -->
> [!CAUTION]
> **Ściōngej tyn mod yno z https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases.** Niy ma strōny internetowyj, Discorda,
> Nexusa ani Steam Workshop. Kożdo inkszo strōna, mirror abo „instalator”, kery go dŏwŏ, niy je mojy: niy ôdpalej go.

![ETS2 & ATS DLAA & DLSS Mod](../images/banner.jpg)

# ETS2 & ATS: DLAA & DLSS Injection Mod

NVIDIA **DLAA** i **DLSS** dlŏ **Euro Truck Simulator 2** i **American Truck Simulator**, na płŏskim ekranie i we VR.
Mod zastympuje gry ôsobne gładzynie krawynżni przez DLAA: płoty, drŏty, znaki na drodze i cienie przestŏwajõ migotać.
DLSS mŏże tyż powiynkszyć ôbrŏz, coby mieć wiyncyj klatek na sekunda.

To je jedyn mały plik, `dinput8.dll`, w folderze gry. Bez launchera, bez kōnta.

- **Stały, ôstry ôbrŏz:** bez migotaniŏ płotōw, drŏtōw i cieni. Szpigle tyż sōm gładzōne.
- **Płŏski ekran i VR:** jedyn plik do ôboju. Robiõ trojne ekrany i Windows HDR.
- **Przyspieszynia dlŏ VR:** foveated rendering (tyż z eye trackingiym), DLAA yno na srodku kożdego ôka, limity ilości klatek.
- **Menu w grze w twojij godce:** naciś `Delete`. Kożdy sztelunek, jego klawisz i koszt, z krōtkim ôpisym.
- **Zgłoszynie felera jednym naciśniyńciym:** naciś `Insert` dwa razy. Mod zapisuje ôbrazy, informacyje ô systymie i logi do zipōw.

## Co trza mieć

- Euro Truck Simulator 2 abo American Truck Simulator (64-bitowy, DirectX 11).
- Karta grafiki **NVIDIA RTX** z aktualnym sterownikiym. DLAA i DLSS robiõ yno na kartach RTX.
- Plik `nvngx_dlss.dll` (runtime DLSS ôd NVIDIA). Mod go niy mŏ w sobie: glej krok 6.
- Dlŏ VR: runtime OpenXR (na przikłŏd Virtual Desktop) i gra ôdpolonŏ z ôpcyjõ `-openxr`.

## Instalacyjŏ

1. **Zrōb kopijõ zapasowõ** folderu gry `bin\win_x64\`.
2. [Ściōngnij nojnowsze wydanie](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases/latest): plik `.zip` pod **Assets**. Rozpakuj go.
3. Ôtwōrz folder gry, na przikłŏd `...\steamapps\common\Euro Truck Simulator 2\bin\win_x64\`
   (abo `...\American Truck Simulator\bin\win_x64\`).
4. **Piyrwyj wyciep inksze mody proxy:** skasuj w tym folderze kożdy inkszy `dinput8.dll` abo `dxgi.dll` z inkszego moda.
5. Skopiuj **`dinput8.dll`** do `bin\win_x64\`. Mōżesz tam tyż skopiować **`dlaa.ini`**, coby zmiyniać sztelunki i klawisze (niy je potrzebne).
6. Wrzuć **`nvngx_dlss.dll`** do tego samego folderu. Weź go z NVIDIA DLSS SDK (<https://github.com/NVIDIA/DLSS>,
   plik `lib/Windows_x86_64/rel/nvngx_dlss.dll`) abo skopiuj z jakijkolwiek gry, kerŏ go mŏ.
7. Ôdpal grã. DLAA je włōnczōne.

Coby ôdinstalować: skasuj `dinput8.dll` (i `dlaa.ini`) z `bin\win_x64\`.

**Linux / Proton:** zainstaluj tak samo. We Steam ustaw ôpcyje ôdpalaniŏ gry na `WINEDLLOVERRIDES="dinput8=n,b" %command%`.

## Nojważniejsze klawisze

| Klawisz | Co robi |
|---|---|
| `Delete` | Ôtwiyra i zawiyrŏ menu. `Up` / `Down` wybiyrajõ rzōnd, `Left` / `Right` go zmiyniajõ. |
| `End` | Przełōnczŏ tryb: wył., DLAA, DLSS. |
| `Insert` dwa razy | Zgłoszynie felera: zapisuje ôbrazy, informacyje ô systymie i logi do `dlaa_selftest\`. |

## Wiyncyj

- **Cołki przewodnik (po angielsku):** [README.md](../../README.md): wszystke sztelunki, klawisze, VR, wydajność i rozwiōnzywanie problymōw.
- **Problymy:** [ôtwōrz zgłoszynie (issue)](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new/choose). Napisz je, proszã, po angielsku.
- **Przekłady** robiõ AI i mogõ mieć felery.
  [Zgłoś problym z przekładym](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new?template=translation.yml).
- To je niyôficjalny projekt fanowski. Niy je powiōnzany z SCS Software ani NVIDIA.
