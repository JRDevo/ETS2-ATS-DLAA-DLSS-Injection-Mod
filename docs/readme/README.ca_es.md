<!-- Short README in Catalan. The full guide (English): ../../README.md -->
> [!CAUTION]
> **Descarrega aquest mod només des de https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases.** No hi ha pàgina web, ni Discord,
> ni Nexus, ni Steam Workshop. Qualsevol altre lloc, mirall o «instal·lador» que l'ofereixi no és meu: no l'executis.

![ETS2 & ATS DLAA & DLSS Mod](../images/banner.jpg)

# ETS2 & ATS: DLAA & DLSS Injection Mod

**DLAA** i **DLSS** de NVIDIA per a **Euro Truck Simulator 2** i **American Truck Simulator**, en pantalla plana i en VR.
El mod substitueix l'antialiàsing del joc per DLAA: les tanques, els cables, les marques viàries i les ombres ja no parpellegen.
El DLSS també pot escalar la imatge per aconseguir més fotogrames per segon.

És un sol fitxer petit, `dinput8.dll`, a la carpeta del joc. Sense llançador, sense compte.

- **Una imatge estable i nítida:** sense parpelleig a tanques, cables i ombres. Els retrovisors també se suavitzen.
- **Pantalla plana i VR:** un sol fitxer per a totes dues. Funcionen les pantalles triples i l'HDR de Windows.
- **Acceleracions VR:** Foveated rendering (també amb seguiment ocular), DLAA només al centre de cada ull, límits de fotogrames.
- **Un menú al joc en la teva llengua:** prem `Delete`. Cada ajust, la seva tecla i el seu cost, amb una descripció curta.
- **Un informe d'error amb una sola pulsació:** prem `Insert` dues vegades. El mod desa imatges, informació del sistema i registres en fitxers zip.

## Què necessites

- Euro Truck Simulator 2 o American Truck Simulator (64 bits, DirectX 11).
- Una targeta gràfica **NVIDIA RTX** amb un controlador actual. DLAA i DLSS només funcionen en targetes RTX.
- El fitxer `nvngx_dlss.dll` (el runtime NVIDIA DLSS). El mod no l'inclou: mira el pas 6.
- Per a VR: un runtime OpenXR (per exemple Virtual Desktop) i el joc iniciat amb l'opció `-openxr`.

## Instal·lació

1. **Fes una còpia de seguretat** de la carpeta `bin\win_x64\` del joc.
2. [Descarrega l'última versió](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases/latest): el `.zip` a **Assets**. Descomprimeix-lo.
3. Obre la carpeta del joc, per exemple `...\steamapps\common\Euro Truck Simulator 2\bin\win_x64\`
   (o `...\American Truck Simulator\bin\win_x64\`).
4. **Treu abans altres mods proxy:** esborra qualsevol altre `dinput8.dll` o `dxgi.dll` d'aquesta carpeta.
5. Copia **`dinput8.dll`** a `bin\win_x64\`. També hi pots copiar **`dlaa.ini`** per canviar ajustos i tecles (no és obligatori).
6. Posa **`nvngx_dlss.dll`** a la mateixa carpeta. Aconsegueix-lo de l'SDK NVIDIA DLSS (<https://github.com/NVIDIA/DLSS>,
   fitxer `lib/Windows_x86_64/rel/nvngx_dlss.dll`) o copia'l de qualsevol joc que el tingui.
7. Inicia el joc. El DLAA està activat.

Per desinstal·lar: esborra `dinput8.dll` (i `dlaa.ini`) de `bin\win_x64\`.

**Linux / Proton:** instal·la de la mateixa manera. A Steam, posa a les opcions d'inici del joc `WINEDLLOVERRIDES="dinput8=n,b" %command%`.

## Les tecles més importants

| Tecla | Què fa |
|---|---|
| `Delete` | Obre i tanca el menú. `Up` / `Down` trien una fila, `Left` / `Right` la canvien. |
| `End` | Canvia el mode: desactivat, DLAA, DLSS. |
| `Insert` dues vegades | Informe d'error: desa imatges, informació del sistema i registres a `dlaa_selftest\`. |

## Més informació

- **La guia completa (en anglès):** [README.md](../../README.md): tots els ajustos, tecles, VR, rendiment i solució de problemes.
- **Problemes:** [obre una issue](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new/choose). Si us plau, escriu-la en anglès.
- **Les traduccions** les fa una IA i poden contenir errors.
  [Informa d'un problema de traducció](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new?template=translation.yml).
- Aquest és un projecte d'aficionats no oficial. No està afiliat a SCS Software ni a NVIDIA.
