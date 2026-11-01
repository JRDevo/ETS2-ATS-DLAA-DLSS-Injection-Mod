<!-- Short README in Hungarian. The full guide (English): ../../README.md -->
> [!CAUTION]
> **Ezt a modot csak innen töltsd le: https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases.** Nincs weboldal, Discord,
> Nexus és Steam Workshop sem. Bármely más oldal, tükör vagy "telepítő", amely kínálja, nem az enyém: ne futtasd.

![ETS2 & ATS DLAA & DLSS Mod](../images/banner.jpg)

# ETS2 & ATS: DLAA & DLSS Injection Mod

Az NVIDIA **DLAA** és **DLSS** technológiája az **Euro Truck Simulator 2** és az **American Truck Simulator** játékhoz, sík képernyőn és VR-ben.
A mod a játék saját élsimítását DLAA-ra cseréli: a kerítések, vezetékek, útburkolati jelek és árnyékok nem csillognak többé.
A DLSS a képet fel is skálázhatja a magasabb képkockasebességért.

Egyetlen kis fájl, a `dinput8.dll`, a játék mappájában. Nincs launcher, nincs fiók.

- **Stabil, éles kép:** nincs csillogás a kerítéseken, vezetékeken és árnyékokon. A tükrök is kapnak élsimítást.
- **Sík és VR:** egy fájl mindkettőhöz. A hármas képernyő és a Windows HDR működik.
- **VR-gyorsítások:** foveated rendering (szemkövetéssel is), DLAA csak mindkét szem közepén, képkockasebesség-korlátok.
- **Játékon belüli menü a saját nyelveden:** nyomd meg a `Delete` gombot. Minden beállítás, a billentyűje és a költsége, rövid leírással.
- **Hibajelentés egy nyomással:** nyomd meg kétszer az `Insert` gombot. A mod képeket, rendszerinfót és naplókat ment zipekbe.

## Mire van szükség

- Euro Truck Simulator 2 vagy American Truck Simulator (64 bites, DirectX 11).
- **NVIDIA RTX** videokártya friss illesztőprogrammal. A DLAA és a DLSS csak RTX kártyán fut.
- Az `nvngx_dlss.dll` fájl (az NVIDIA DLSS futtatókörnyezete). A mod nem tartalmazza: lásd a 6. lépést.
- VR-hez: egy OpenXR futtatókörnyezet (például a Virtual Desktop) és a `-openxr` kapcsolóval indított játék.

## Telepítés

1. **Készíts biztonsági másolatot** a játék `bin\win_x64\` mappájáról.
2. [Töltsd le a legújabb kiadást](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases/latest): az **Assets** alatti `.zip` fájlt. Csomagold ki.
3. Nyisd meg a játék mappáját, például `...\steamapps\common\Euro Truck Simulator 2\bin\win_x64\`
   (vagy `...\American Truck Simulator\bin\win_x64\`).
4. **Előbb távolítsd el a többi proxy modot:** töröld a más modok `dinput8.dll` vagy `dxgi.dll` fájlját abból a mappából.
5. Másold a **`dinput8.dll`** fájlt a `bin\win_x64\` mappába. A **`dlaa.ini`** fájlt is odamásolhatod a beállítások és gombok módosításához (nem kötelező).
6. Tedd az **`nvngx_dlss.dll`** fájlt ugyanabba a mappába. Az NVIDIA DLSS SDK-ból szerezheted meg (<https://github.com/NVIDIA/DLSS>,
   a `lib/Windows_x86_64/rel/nvngx_dlss.dll` fájl), vagy másold át bármelyik játékból, amelyiknek megvan.
7. Indítsd el a játékot. A DLAA be van kapcsolva.

Eltávolításhoz: töröld a `dinput8.dll` (és a `dlaa.ini`) fájlt a `bin\win_x64\` mappából.

**Linux / Proton:** ugyanígy telepítsd. A Steamben a játék indítási beállításai közé írd: `WINEDLLOVERRIDES="dinput8=n,b" %command%`.

## A legfontosabb gombok

| Gomb | Mit csinál |
|---|---|
| `Delete` | Megnyitja és bezárja a menüt. `Up` / `Down` sort választ, `Left` / `Right` módosítja. |
| `End` | Váltja a módot: ki, DLAA, DLSS. |
| `Insert` kétszer | Hibajelentés: képeket, rendszerinfót és naplókat ment a `dlaa_selftest\` mappába. |

## Továbbiak

- **A teljes útmutató (angolul):** [README.md](../../README.md): minden beállítás, gomb, VR, teljesítmény és hibaelhárítás.
- **Problémák:** [nyiss egy issue-t](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new/choose). Kérlek, angolul írd meg.
- **A fordításokat** mesterséges intelligencia készíti, és hibákat tartalmazhatnak.
  [Fordítási hiba jelentése](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new?template=translation.yml).
- Ez egy nem hivatalos rajongói projekt. Nem áll kapcsolatban az SCS Software-rel vagy az NVIDIA-val.
