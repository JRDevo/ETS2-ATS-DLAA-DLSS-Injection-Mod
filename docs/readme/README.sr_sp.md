<!-- Short README in Serbian (Latin). The full guide (English): ../../README.md -->
> [!CAUTION]
> **Preuzimajte ovaj mod samo sa https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases.** Nema veb-sajta, Discord-a,
> Nexus-a ni Steam Workshop-a. Svaki drugi sajt, ogledalo ili „instaler“ koji ga nudi nije moj: ne pokrećite ga.

![ETS2 & ATS DLAA & DLSS Mod](../images/banner.jpg)

# ETS2 & ATS: mod DLAA & DLSS Injection

NVIDIA **DLAA** i **DLSS** za **Euro Truck Simulator 2** i **American Truck Simulator**, na ravnom ekranu i u VR-u.
Mod zamenjuje sopstveno glađenje igre sa DLAA: ograde, žice, oznake na putu i senke više ne trepere.
DLSS može i da uveća sliku za veći broj kadrova.

To je jedna mala datoteka, `dinput8.dll`, u fascikli igre. Bez launčera, bez naloga.

- **Mirna i oštra slika:** bez treperenja na ogradama, žicama i senkama. I ogledala dobijaju glađenje.
- **Ravan režim i VR:** jedna datoteka za oba. Rade trostruki ekrani i Windows HDR.
- **VR ubrzanja:** foveated renderovanje (i uz praćenje očiju), DLAA samo u sredini svakog oka, limiti broja kadrova.
- **Meni u igri na vašem jeziku:** pritisnite `Delete`. Svako podešavanje, njegov taster i cena, sa kratkim opisom.
- **Izveštaj o grešci jednim tasterom:** pritisnite `Insert` dvaput. Mod čuva slike, podatke o sistemu i dnevnike u zip datoteke.

## Šta vam treba

- Euro Truck Simulator 2 ili American Truck Simulator (64-bitni, DirectX 11).
- Grafička kartica **NVIDIA RTX** sa aktuelnim drajverom. DLAA i DLSS rade samo na RTX karticama.
- Datoteka `nvngx_dlss.dll` (NVIDIA DLSS runtime). Mod je ne sadrži: vidite korak 6.
- Za VR: OpenXR runtime (na primer Virtual Desktop) i igra pokrenuta sa opcijom `-openxr`.

## Instalacija

1. **Napravite rezervnu kopiju** fascikle `bin\win_x64\` igre.
2. [Preuzmite poslednje izdanje](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases/latest): `.zip` datoteku pod **Assets**. Raspakujte je.
3. Otvorite fasciklu igre, na primer `...\steamapps\common\Euro Truck Simulator 2\bin\win_x64\`
   (ili `...\American Truck Simulator\bin\win_x64\`).
4. **Prvo uklonite druge proksi modove:** obrišite `dinput8.dll` ili `dxgi.dll` drugog moda u toj fascikli.
5. Kopirajte **`dinput8.dll`** u `bin\win_x64\`. Tamo možete kopirati i **`dlaa.ini`** za promenu podešavanja i tastera (nije obavezno).
6. Stavite **`nvngx_dlss.dll`** u istu fasciklu. Uzmite je iz NVIDIA DLSS SDK (<https://github.com/NVIDIA/DLSS>,
   datoteka `lib/Windows_x86_64/rel/nvngx_dlss.dll`) ili je kopirajte iz bilo koje igre koja je ima.
7. Pokrenite igru. DLAA je uključen.

Za deinstalaciju: obrišite `dinput8.dll` (i `dlaa.ini`) iz `bin\win_x64\`.

**Linux / Proton:** instalirajte na isti način. U Steam-u postavite opcije pokretanja igre na `WINEDLLOVERRIDES="dinput8=n,b" %command%`.

## Najvažniji tasteri

| Taster | Šta radi |
|---|---|
| `Delete` | Otvara i zatvara meni. `Up` / `Down` biraju red, `Left` / `Right` ga menjaju. |
| `End` | Menja režim: isklj., DLAA, DLSS. |
| `Insert` dvaput | Izveštaj o grešci: čuva slike, podatke o sistemu i dnevnike u `dlaa_selftest\`. |

## Još

- **Potpuni vodič (na engleskom):** [README.md](../../README.md): sva podešavanja, tasteri, VR, performanse i rešavanje problema.
- **Problemi:** [otvorite issue](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new/choose). Molimo pišite na engleskom.
- **Prevodi** su napravljeni pomoću veštačke inteligencije i mogu sadržati greške.
  [Prijavite problem s prevodom](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new?template=translation.yml).
- Ovo je nezvaničan projekat fanova. Nije povezan sa SCS Software ili NVIDIA.
