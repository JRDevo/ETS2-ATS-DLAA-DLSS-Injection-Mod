<!-- Short README in Spanish (Spain). The full guide (English): ../../README.md -->
> [!CAUTION]
> **Descarga este mod solo desde https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases.** No hay página web, ni Discord,
> ni Nexus, ni Steam Workshop. Cualquier otro sitio, espejo o «instalador» que lo ofrezca no es mío: no lo ejecutes.

![ETS2 & ATS DLAA & DLSS Mod](../images/banner.jpg)

# ETS2 & ATS: DLAA & DLSS Injection Mod

**DLAA** y **DLSS** de NVIDIA para **Euro Truck Simulator 2** y **American Truck Simulator**, en pantalla plana y en VR.
El mod sustituye el antialiasing del juego por DLAA: las vallas, los cables, las marcas viales y las sombras ya no parpadean.
El DLSS también puede escalar la imagen para conseguir más fotogramas por segundo.

Es un solo archivo pequeño, `dinput8.dll`, en la carpeta del juego. Sin lanzador, sin cuenta.

- **Una imagen estable y nítida:** sin parpadeo en vallas, cables y sombras. Los retrovisores también se suavizan.
- **Pantalla plana y VR:** un solo archivo para ambas. Funcionan las pantallas triples y el HDR de Windows.
- **Aceleraciones VR:** Foveated rendering (también con seguimiento ocular), DLAA solo en el centro de cada ojo, límites de fotogramas.
- **Un menú en el juego en tu idioma:** pulsa `Delete`. Cada ajuste, su tecla y su coste, con una descripción corta.
- **Un informe de error con una sola pulsación:** pulsa `Insert` dos veces. El mod guarda imágenes, información del sistema y registros en archivos zip.

## Qué necesitas

- Euro Truck Simulator 2 o American Truck Simulator (64 bits, DirectX 11).
- Una tarjeta gráfica **NVIDIA RTX** con un controlador actual. DLAA y DLSS solo funcionan en tarjetas RTX.
- El archivo `nvngx_dlss.dll` (el runtime NVIDIA DLSS). El mod no lo incluye: mira el paso 6.
- Para VR: un runtime OpenXR (por ejemplo Virtual Desktop) y el juego iniciado con la opción `-openxr`.

## Instalación

1. **Haz una copia de seguridad** de la carpeta `bin\win_x64\` del juego.
2. [Descarga la última versión](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases/latest): el `.zip` en **Assets**. Descomprímelo.
3. Abre la carpeta del juego, por ejemplo `...\steamapps\common\Euro Truck Simulator 2\bin\win_x64\`
   (o `...\American Truck Simulator\bin\win_x64\`).
4. **Quita antes otros mods proxy:** borra cualquier otro `dinput8.dll` o `dxgi.dll` de esa carpeta.
5. Copia **`dinput8.dll`** en `bin\win_x64\`. También puedes copiar ahí **`dlaa.ini`** para cambiar ajustes y teclas (no es obligatorio).
6. Pon **`nvngx_dlss.dll`** en la misma carpeta. Consíguelo en el SDK NVIDIA DLSS (<https://github.com/NVIDIA/DLSS>,
   archivo `lib/Windows_x86_64/rel/nvngx_dlss.dll`) o cópialo de cualquier juego que lo tenga.
7. Inicia el juego. El DLAA está activado.

Para desinstalar: borra `dinput8.dll` (y `dlaa.ini`) de `bin\win_x64\`.

**Linux / Proton:** instala de la misma forma. En Steam, pon en las opciones de lanzamiento del juego `WINEDLLOVERRIDES="dinput8=n,b" %command%`.

## Las teclas más importantes

| Tecla | Qué hace |
|---|---|
| `Delete` | Abre y cierra el menú. `Up` / `Down` eligen una fila, `Left` / `Right` la cambian. |
| `End` | Cambia el modo: desactivado, DLAA, DLSS. |
| `Insert` dos veces | Informe de error: guarda imágenes, información del sistema y registros en `dlaa_selftest\`. |

## Más información

- **La guía completa (en inglés):** [README.md](../../README.md): todos los ajustes, teclas, VR, rendimiento y solución de problemas.
- **Problemas:** [abre una issue](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new/choose). Por favor, escríbela en inglés.
- **Las traducciones** las hace una IA y pueden contener errores.
  [Informar de un problema de traducción](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new?template=translation.yml).
- Este es un proyecto de aficionados no oficial. No está afiliado a SCS Software ni a NVIDIA.
