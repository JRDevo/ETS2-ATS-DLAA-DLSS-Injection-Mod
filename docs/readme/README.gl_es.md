<!-- Short README in Galician. The full guide (English): ../../README.md -->
> [!CAUTION]
> **Descarga este mod só desde https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases.** Non hai páxina web, nin Discord,
> nin Nexus, nin Steam Workshop. Calquera outro sitio, espello ou «instalador» que o ofreza non é meu: non o executes.

![ETS2 & ATS DLAA & DLSS Mod](../images/banner.jpg)

# ETS2 & ATS: DLAA & DLSS Injection Mod

**DLAA** e **DLSS** de NVIDIA para **Euro Truck Simulator 2** e **American Truck Simulator**, en pantalla plana e en VR.
O mod substitúe o antialiasing do xogo por DLAA: os valos, os cables, as marcas viais e as sombras xa non cintilan.
O DLSS tamén pode escalar a imaxe para conseguir máis fotogramas por segundo.

É un só ficheiro pequeno, `dinput8.dll`, no cartafol do xogo. Sen lanzador, sen conta.

- **Unha imaxe estable e nítida:** sen cintilación en valos, cables e sombras. Os retrovisores tamén se suavizan.
- **Pantalla plana e VR:** un só ficheiro para as dúas. Funcionan as pantallas triples e o HDR de Windows.
- **Aceleracións VR:** Foveated rendering (tamén con seguimento ocular), DLAA só no centro de cada ollo, límites de fotogramas.
- **Un menú no xogo no teu idioma:** preme `Delete`. Cada axuste, a súa tecla e o seu custo, cunha descrición curta.
- **Un informe de erro cunha soa pulsación:** preme `Insert` dúas veces. O mod garda imaxes, información do sistema e rexistros en ficheiros zip.

## Que necesitas

- Euro Truck Simulator 2 ou American Truck Simulator (64 bits, DirectX 11).
- Unha tarxeta gráfica **NVIDIA RTX** cun controlador actual. DLAA e DLSS só funcionan en tarxetas RTX.
- O ficheiro `nvngx_dlss.dll` (o runtime NVIDIA DLSS). O mod non o inclúe: mira o paso 6.
- Para VR: un runtime OpenXR (por exemplo Virtual Desktop) e o xogo iniciado coa opción `-openxr`.

## Instalación

1. **Fai unha copia de seguranza** do cartafol `bin\win_x64\` do xogo.
2. [Descarga a última versión](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases/latest): o `.zip` en **Assets**. Descomprímeo.
3. Abre o cartafol do xogo, por exemplo `...\steamapps\common\Euro Truck Simulator 2\bin\win_x64\`
   (ou `...\American Truck Simulator\bin\win_x64\`).
4. **Quita antes outros mods proxy:** borra calquera outro `dinput8.dll` ou `dxgi.dll` deste cartafol.
5. Copia **`dinput8.dll`** en `bin\win_x64\`. Tamén podes copiar alí **`dlaa.ini`** para cambiar axustes e teclas (non é obrigatorio).
6. Pon **`nvngx_dlss.dll`** no mesmo cartafol. Consíguelo no SDK NVIDIA DLSS (<https://github.com/NVIDIA/DLSS>,
   ficheiro `lib/Windows_x86_64/rel/nvngx_dlss.dll`) ou cópiao de calquera xogo que o teña.
7. Inicia o xogo. O DLAA está activado.

Para desinstalar: borra `dinput8.dll` (e `dlaa.ini`) de `bin\win_x64\`.

**Linux / Proton:** instala do mesmo xeito. En Steam, pon nas opcións de lanzamento do xogo `WINEDLLOVERRIDES="dinput8=n,b" %command%`.

## As teclas máis importantes

| Tecla | Que fai |
|---|---|
| `Delete` | Abre e pecha o menú. `Up` / `Down` escollen unha fila, `Left` / `Right` cambiana. |
| `End` | Cambia o modo: desactivado, DLAA, DLSS. |
| `Insert` dúas veces | Informe de erro: garda imaxes, información do sistema e rexistros en `dlaa_selftest\`. |

## Máis información

- **A guía completa (en inglés):** [README.md](../../README.md): todos os axustes, teclas, VR, rendemento e solución de problemas.
- **Problemas:** [abre unha issue](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new/choose). Por favor, escríbea en inglés.
- **As traducións** fainas unha IA e poden conter erros.
  [Informar dun problema de tradución](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new?template=translation.yml).
- Este é un proxecto de afeccionados non oficial. Non está afiliado a SCS Software nin a NVIDIA.
