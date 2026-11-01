<!-- Short README in Basque. The full guide (English): ../../README.md -->
> [!CAUTION]
> **Deskargatu mod hau https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases helbidetik soilik.** Ez dago webgunerik, Discord-ik,
> Nexus-ik ez Steam Workshop-ik. Hura eskaintzen duen beste edozein webgune, ispilu edo "instalatzaile" ez da nirea: ez exekutatu.

![ETS2 & ATS DLAA & DLSS Mod](../images/banner.jpg)

# ETS2 & ATS: DLAA & DLSS Injection Mod

NVIDIAren **DLAA** eta **DLSS** **Euro Truck Simulator 2** eta **American Truck Simulator** jokoentzat, pantaila lauan eta VRn.
Modak jokoaren anti-aliasing propioa DLAA-rekin ordezkatzen du: hesiek, alanbreek, errepide-marrek eta itzalek ez dute dir-dir egiten.
DLSS-k irudia eskala dezake ere, fotograma gehiago segundoko lortzeko.

Fitxategi txiki bat da, `dinput8.dll`, jokoaren karpetan. Launcher-ik gabe, konturik gabe.

- **Irudi egonkor eta zorrotza:** dir-dirik gabe hesietan, alanbreetan eta itzaletan. Ispiluek ere anti-aliasing-a jasotzen dute.
- **Pantaila laua eta VR:** fitxategi bat biretarako. Hiru pantailek eta Windows HDR-k lan egiten dute.
- **VR bizkortzeak:** foveated rendering (begi-jarraipenarekin ere), DLAA begi bakoitzaren erdian soilik, fotograma-tasaren mugak.
- **Jokoko menu bat zure hizkuntzan:** sakatu `Delete`. Doikuntza bakoitza, bere tekla eta bere kostua, azalpen labur batekin.
- **Akats-txosten bat sakatze batean:** sakatu `Insert` bi aldiz. Modak irudiak, sistemaren informazioa eta erregistroak zip-etan gordetzen ditu.

## Zer behar duzun

- Euro Truck Simulator 2 edo American Truck Simulator (64 biteko, DirectX 11).
- **NVIDIA RTX** txartel grafikoa, kontrolatzaile eguneratuarekin. DLAA eta DLSS RTX txarteletan soilik dabiltza.
- `nvngx_dlss.dll` fitxategia (NVIDIAren DLSS runtime-a). Modak ez du barne hartzen: ikusi 6. urratsa.
- VRrentzat: OpenXR runtime bat (adibidez Virtual Desktop) eta jokoa `-openxr` aukerarekin abiarazita.

## Instalazioa

1. **Egin babeskopia bat** jokoaren `bin\win_x64\` karpetarena.
2. [Deskargatu azken bertsioa](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases/latest): **Assets** azpiko `.zip` fitxategia. Deskonprimitu.
3. Ireki jokoaren karpeta, adibidez `...\steamapps\common\Euro Truck Simulator 2\bin\win_x64\`
   (edo `...\American Truck Simulator\bin\win_x64\`).
4. **Kendu lehenago beste proxy mod batzuk:** ezabatu karpeta horretako beste edozein modren `dinput8.dll` edo `dxgi.dll`.
5. Kopiatu **`dinput8.dll`** `bin\win_x64\` karpetara. **`dlaa.ini`** ere kopia dezakezu hara, doikuntzak eta teklak aldatzeko (ez da beharrezkoa).
6. Jarri **`nvngx_dlss.dll`** karpeta berean. Lortu NVIDIA DLSS SDK-tik (<https://github.com/NVIDIA/DLSS>,
   `lib/Windows_x86_64/rel/nvngx_dlss.dll` fitxategia) edo kopiatu daukan edozein jokotatik.
7. Abiarazi jokoa. DLAA gaituta dago.

Desinstalatzeko: ezabatu `dinput8.dll` (eta `dlaa.ini`) `bin\win_x64\` karpetatik.

**Linux / Proton:** instalatu modu berean. Steam-en, jarri jokoaren abiarazte-aukeretan `WINEDLLOVERRIDES="dinput8=n,b" %command%`.

## Tekla garrantzitsuenak

| Tekla | Zer egiten duen |
|---|---|
| `Delete` | Menua ireki eta ixten du. `Up` / `Down` lerro bat aukeratzen dute, `Left` / `Right` aldatzen dute. |
| `End` | Modua aldatzen du: ezgaituta, DLAA, DLSS. |
| `Insert` bi aldiz | Akats-txostena: irudiak, sistemaren informazioa eta erregistroak gordetzen ditu `dlaa_selftest\` karpetan. |

## Gehiago

- **Gida osoa (ingelesez):** [README.md](../../README.md): doikuntza guztiak, teklak, VR, errendimendua eta arazoen konponketa.
- **Arazoak:** [ireki issue bat](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new/choose). Mesedez, idatzi ingelesez.
- **Itzulpenak** AIak egiten ditu eta akatsak izan ditzakete.
  [Jakinarazi itzulpen-arazo bat](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new?template=translation.yml).
- Zale-proiektu ez-ofiziala da hau. Ez dago SCS Software edo NVIDIArekin lotuta.
