<!-- Short README in Croatian. The full guide (English): ../../README.md -->
> [!CAUTION]
> **Ovaj mod preuzimajte samo s https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases.** Ne postoji web-stranica, Discord,
> Nexus ni Steam Workshop. Svaka druga stranica, zrcalo ili „instalater“ koji ga nudi nije moj: ne pokrećite ga.

![ETS2 & ATS DLAA & DLSS Mod](../images/banner.jpg)

# ETS2 & ATS: DLAA & DLSS Injection Mod

NVIDIA **DLAA** i **DLSS** za **Euro Truck Simulator 2** i **American Truck Simulator**, na običnom zaslonu i u VR-u.
Mod zamjenjuje igrino vlastito zaglađivanje rubova s DLAA: ograde, žice, oznake na cesti i sjene prestaju treperiti.
DLSS može i povećati sliku za veći broj sličica u sekundi.

To je jedna mala datoteka, `dinput8.dll`, u mapi igre. Bez pokretača, bez računa.

- **Mirna, oštra slika:** bez treperenja ograda, žica i sjena. Retrovizori se također zaglađuju.
- **Ravni način i VR:** jedna datoteka za oboje. Rade trostruki zasloni i Windows HDR.
- **VR ubrzanja:** foveated rendering (i s praćenjem očiju), DLAA samo u sredini svakog oka, ograničenja broja sličica.
- **Izbornik u igri na vašem jeziku:** pritisnite `Delete`. Svaka postavka, njezina tipka i cijena, s kratkim opisom.
- **Prijava greške jednim pritiskom:** dvaput pritisnite `Insert`. Mod sprema slike, podatke o sustavu i zapise u zip datoteke.

## Što trebate

- Euro Truck Simulator 2 ili American Truck Simulator (64-bitni, DirectX 11).
- Grafičku karticu **NVIDIA RTX** s aktualnim upravljačkim programom. DLAA i DLSS rade samo na RTX karticama.
- Datoteku `nvngx_dlss.dll` (NVIDIA DLSS runtime). Mod je ne sadrži: vidi korak 6.
- Za VR: OpenXR runtime (na primjer Virtual Desktop) i igru pokrenutu s opcijom `-openxr`.

## Instalacija

1. **Napravite sigurnosnu kopiju** mape igre `bin\win_x64\`.
2. [Preuzmite najnovije izdanje](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases/latest): datoteku `.zip` pod **Assets**. Raspakirajte je.
3. Otvorite mapu igre, na primjer `...\steamapps\common\Euro Truck Simulator 2\bin\win_x64\`
   (ili `...\American Truck Simulator\bin\win_x64\`).
4. **Prvo uklonite druge proxy modove:** u toj mapi obrišite svaki drugi `dinput8.dll` ili `dxgi.dll` iz drugog moda.
5. Kopirajte **`dinput8.dll`** u `bin\win_x64\`. Tamo možete kopirati i **`dlaa.ini`** za promjenu postavki i tipki (nije obavezan).
6. Stavite **`nvngx_dlss.dll`** u istu mapu. Dobit ćete ga iz NVIDIA DLSS SDK-a (<https://github.com/NVIDIA/DLSS>,
   datoteka `lib/Windows_x86_64/rel/nvngx_dlss.dll`) ili ga kopirajte iz bilo koje igre koja ga ima.
7. Pokrenite igru. DLAA je uključen.

Deinstalacija: obrišite `dinput8.dll` (i `dlaa.ini`) iz `bin\win_x64\`.

**Linux / Proton:** instalirajte na isti način. U Steamu postavite opcije pokretanja igre na `WINEDLLOVERRIDES="dinput8=n,b" %command%`.

## Najvažnije tipke

| Tipka | Što radi |
|---|---|
| `Delete` | Otvara i zatvara izbornik. `Up` / `Down` biraju redak, `Left` / `Right` ga mijenjaju. |
| `End` | Prebacuje način: isklj., DLAA, DLSS. |
| `Insert` dvaput | Prijava greške: sprema slike, podatke o sustavu i zapise u `dlaa_selftest\`. |

## Više

- **Cijeli vodič (na engleskom):** [README.md](../../README.md): sve postavke, tipke, VR, performanse i rješavanje problema.
- **Problemi:** [otvorite prijavu (issue)](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new/choose). Napišite je, molim, na engleskom.
- **Prijevode** radi AI i mogu sadržavati greške.
  [Prijavite problem s prijevodom](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new?template=translation.yml).
- Ovo je neslužbeni projekt obožavatelja. Nije povezan sa SCS Software ni NVIDIA.
