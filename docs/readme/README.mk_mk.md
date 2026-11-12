<!-- Short README in Macedonian. The full guide (English): ../../README.md -->
> [!CAUTION]
> **Преземајте го овој мод само од https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases.** Нема веб-страница, Discord,
> Nexus ниту Steam Workshop. Секоја друга страница, огледало или „инсталер“ што го нуди не е мој: не го стартувајте.

![ETS2 & ATS DLAA & DLSS Mod](../images/banner.jpg)

# ETS2 & ATS: мод DLAA & DLSS Injection

NVIDIA **DLAA** и **DLSS** за **Euro Truck Simulator 2** и **American Truck Simulator**, на рамен екран и во VR.
Модот го заменува сопственото измазнување на играта со DLAA: оградите, жиците, ознаките на патот и сенките повеќе не трепкаат.
DLSS може и да ја зголеми сликата за повисока брзина на кадри.

Тоа е една мала датотека, `dinput8.dll`, во папката на играта. Без лаунчер, без сметка.

- **Мирна и јасна слика:** без трепкање на огради, жици и сенки. И огледалата добиваат измазнување.
- **Рамен режим и VR:** една датотека за двата. Работат тројни екрани и Windows HDR.
- **VR забрзувања:** foveated рендерирање (и со следење на очи), DLAA само во средината на секое око, лимити на брзината на кадри.
- **Мени во играта на вашиот јазик:** притиснете `Delete`. Секоја поставка, нејзиното копче и цена, со кус опис.
- **Извештај за грешка со едно копче:** притиснете `Insert` двапати. Модот зачувува слики, информации за системот и дневници во zip датотеки.

## Што ви треба

- Euro Truck Simulator 2 или American Truck Simulator (64-битна, DirectX 11).
- Видеокартичка **NVIDIA RTX** со актуелен драјвер. DLAA и DLSS работат само на RTX картички.
- Датотеката `nvngx_dlss.dll` (NVIDIA DLSS runtime). Модот не ја содржи: видете го чекорот 6.
- За VR: OpenXR runtime (на пример Virtual Desktop) и играта стартувана со опцијата `-openxr`.

## Инсталација

1. **Направете резервна копија** на папката `bin\win_x64\` на играта.
2. [Преземете го последното издание](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases/latest): `.zip` датотеката под **Assets**. Распакувајте ја.
3. Отворете ја папката на играта, на пример `...\steamapps\common\Euro Truck Simulator 2\bin\win_x64\`
   (или `...\American Truck Simulator\bin\win_x64\`).
4. **Прво отстранете други прокси модови:** избришете `dinput8.dll` или `dxgi.dll` од друг мод во таа папка.
5. Копирајте ја **`dinput8.dll`** во `bin\win_x64\`. Таму можете да ја копирате и **`dlaa.ini`** за менување на поставки и копчиња (не е задолжително).
6. Ставете ја **`nvngx_dlss.dll`** во истата папка. Земете ја од NVIDIA DLSS SDK (<https://github.com/NVIDIA/DLSS>,
   датотека `lib/Windows_x86_64/rel/nvngx_dlss.dll`) или копирајте ја од која било игра што ја има.
7. Стартувајте ја играта. DLAA е вклучен.

За деинсталација: избришете ја `dinput8.dll` (и `dlaa.ini`) од `bin\win_x64\`.

**Linux / Proton:** инсталирајте на ист начин. Во Steam поставете ги опциите за стартување на играта на `WINEDLLOVERRIDES="dinput8=n,b" %command%`.

## Најважните копчиња

| Копче | Што прави |
|---|---|
| `Delete` | Го отвора и затвора менито. `Up` / `Down` избираат ред, `Left` / `Right` го менуваат. |
| `End` | Го менува режимот: искл., DLAA, DLSS. |
| `Insert` двапати | Извештај за грешка: зачувува слики, информации за системот и дневници во `dlaa_selftest\`. |

## Повеќе

- **Целосниот водич (на англиски):** [README.md](../../README.md): сите поставки, копчиња, VR, перформанси и решавање проблеми.
- **Проблеми:** [отворете issue](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new/choose). Ве молиме пишувајте на англиски.
- **Преводите** се направени со вештачка интелигенција и може да содржат грешки.
  [Пријавете проблем со преводот](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new?template=translation.yml).
- Ова е неофицијален фан проект. Не е поврзан со SCS Software или NVIDIA.
