<!-- Short README in Serbian (Cyrillic). The full guide (English): ../../README.md -->
> [!CAUTION]
> **Преузимајте овај мод само са https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases.** Нема веб-сајта, Discord-а,
> Nexus-а ни Steam Workshop-а. Сваки други сајт, огледало или „инсталер“ који га нуди није мој: не покрећите га.

![ETS2 & ATS DLAA & DLSS Mod](../images/banner.jpg)

# ETS2 & ATS: мод DLAA & DLSS Injection

NVIDIA **DLAA** и **DLSS** за **Euro Truck Simulator 2** и **American Truck Simulator**, на равном екрану и у VR-у.
Мод замењује сопствено глађење игре са DLAA: ограде, жице, ознаке на путу и сенке више не трепере.
DLSS може и да увећа слику за већи број кадрова.

То је једна мала датотека, `dinput8.dll`, у фасцикли игре. Без лаунчера, без налога.

- **Мирна и оштра слика:** без треперења на оградама, жицама и сенкама. И огледала добијају глађење.
- **Раван режим и VR:** једна датотека за оба. Раде троструки екрани и Windows HDR.
- **VR убрзања:** foveated рендеровање (и уз праћење очију), DLAA само у средини сваког ока, лимити броја кадрова.
- **Мени у игри на вашем језику:** притисните `Delete`. Свако подешавање, његов тастер и цена, са кратким описом.
- **Извештај о грешци једним тастером:** притисните `Insert` двапут. Мод чува слике, податке о систему и дневнике у zip датотеке.

## Шта вам треба

- Euro Truck Simulator 2 или American Truck Simulator (64-битни, DirectX 11).
- Графичка картица **NVIDIA RTX** са актуелним драјвером. DLAA и DLSS раде само на RTX картицама.
- Датотека `nvngx_dlss.dll` (NVIDIA DLSS runtime). Мод је не садржи: видите корак 6.
- За VR: OpenXR runtime (на пример Virtual Desktop) и игра покренута са опцијом `-openxr`.

## Инсталација

1. **Направите резервну копију** фасцикле `bin\win_x64\` игре.
2. [Преузмите последње издање](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases/latest): `.zip` датотеку под **Assets**. Распакујте је.
3. Отворите фасциклу игре, на пример `...\steamapps\common\Euro Truck Simulator 2\bin\win_x64\`
   (или `...\American Truck Simulator\bin\win_x64\`).
4. **Прво уклоните друге прокси модове:** обришите `dinput8.dll` или `dxgi.dll` другог мода у тој фасцикли.
5. Копирајте **`dinput8.dll`** у `bin\win_x64\`. Тамо можете копирати и **`dlaa.ini`** за промену подешавања и тастера (није обавезно).
6. Ставите **`nvngx_dlss.dll`** у исту фасциклу. Узмите је из NVIDIA DLSS SDK (<https://github.com/NVIDIA/DLSS>,
   датотека `lib/Windows_x86_64/rel/nvngx_dlss.dll`) или је копирајте из било које игре која је има.
7. Покрените игру. DLAA је укључен.

За деинсталацију: обришите `dinput8.dll` (и `dlaa.ini`) из `bin\win_x64\`.

**Linux / Proton:** инсталирајте на исти начин. У Steam-у поставите опције покретања игре на `WINEDLLOVERRIDES="dinput8=n,b" %command%`.

## Најважнији тастери

| Тастер | Шта ради |
|---|---|
| `Delete` | Отвара и затвара мени. `Up` / `Down` бирају ред, `Left` / `Right` га мењају. |
| `End` | Мења режим: искљ., DLAA, DLSS. |
| `Insert` двапут | Извештај о грешци: чува слике, податке о систему и дневнике у `dlaa_selftest\`. |

## Још

- **Потпуни водич (на енглеском):** [README.md](../../README.md): сва подешавања, тастери, VR, перформансе и решавање проблема.
- **Проблеми:** [отворите issue](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new/choose). Молимо пишите на енглеском.
- **Преводи** су направљени помоћу вештачке интелигенције и могу садржати грешке.
  [Пријавите проблем с преводом](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new?template=translation.yml).
- Ово је незваничан пројекат фанова. Није повезан са SCS Software или NVIDIA.
