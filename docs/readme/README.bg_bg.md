<!-- Short README in Bulgarian. The full guide (English): ../../README.md -->
> [!CAUTION]
> **Изтегляйте този мод само от https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases.** Няма уебсайт, Discord,
> Nexus и Steam Workshop. Всеки друг сайт, огледало или „инсталатор“, който го предлага, не е мой: не го стартирайте.

![ETS2 & ATS DLAA & DLSS Mod](../images/banner.jpg)

# ETS2 & ATS: мод DLAA & DLSS Injection

NVIDIA **DLAA** и **DLSS** за **Euro Truck Simulator 2** и **American Truck Simulator**, на обикновен екран и във VR.
Модът заменя собственото изглаждане на играта с DLAA: оградите, жиците, пътната маркировка и сенките вече не блещукат.
DLSS може и да мащабира картината за по-висока кадрова честота.

Това е един малък файл, `dinput8.dll`, в папката на играта. Без лаунчър, без акаунт.

- **Стабилна и ясна картина:** без блещукане по огради, жици и сенки. Огледалата също получават изглаждане.
- **Плосък режим и VR:** един файл за двата. Работят тройни екрани и Windows HDR.
- **Ускорения за VR:** foveated рендиране (и по поглед), DLAA само в средата на всяко око, лимити на кадровата честота.
- **Меню в играта на вашия език:** натиснете `Delete`. Всяка настройка, нейният клавиш и цена, с кратко описание.
- **Доклад за грешка с един клавиш:** натиснете `Insert` два пъти. Модът записва картини, системна информация и журнали в zip файлове.

## Какво ви трябва

- Euro Truck Simulator 2 или American Truck Simulator (64-битов, DirectX 11).
- Видеокарта **NVIDIA RTX** с актуален драйвер. DLAA и DLSS работят само на RTX карти.
- Файлът `nvngx_dlss.dll` (средата за изпълнение на NVIDIA DLSS). Модът не го включва: вижте стъпка 6.
- За VR: OpenXR среда за изпълнение (например Virtual Desktop) и играта, стартирана с опцията `-openxr`.

## Инсталиране

1. **Направете резервно копие** на папката `bin\win_x64\` на играта.
2. [Изтеглете последната версия](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases/latest): `.zip` файла под **Assets**. Разархивирайте го.
3. Отворете папката на играта, например `...\steamapps\common\Euro Truck Simulator 2\bin\win_x64\`
   (или `...\American Truck Simulator\bin\win_x64\`).
4. **Първо махнете други прокси модове:** изтрийте `dinput8.dll` или `dxgi.dll` на друг мод в тази папка.
5. Копирайте **`dinput8.dll`** в `bin\win_x64\`. Можете да копирате там и **`dlaa.ini`**, за да променяте настройки и клавиши (не е задължително).
6. Сложете **`nvngx_dlss.dll`** в същата папка. Вземете го от NVIDIA DLSS SDK (<https://github.com/NVIDIA/DLSS>,
   файл `lib/Windows_x86_64/rel/nvngx_dlss.dll`) или го копирайте от всяка игра, която го има.
7. Стартирайте играта. DLAA е включен.

За премахване: изтрийте `dinput8.dll` (и `dlaa.ini`) от `bin\win_x64\`.

**Linux / Proton:** инсталирайте по същия начин. В Steam задайте в опциите за стартиране на играта `WINEDLLOVERRIDES="dinput8=n,b" %command%`.

## Най-важните клавиши

| Клавиш | Какво прави |
|---|---|
| `Delete` | Отваря и затваря менюто. `Up` / `Down` избират ред, `Left` / `Right` го променят. |
| `End` | Превключва режима: изкл., DLAA, DLSS. |
| `Insert` два пъти | Доклад за грешка: записва картини, системна информация и журнали в `dlaa_selftest\`. |

## Още

- **Пълното ръководство (на английски):** [README.md](../../README.md): всички настройки, клавиши, VR, производителност и отстраняване на проблеми.
- **Проблеми:** [отворете issue](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new/choose). Моля, пишете на английски.
- **Преводите** са направени от ИИ и може да съдържат грешки.
  [Докладвайте проблем с превода](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new?template=translation.yml).
- Това е неофициален фен проект. Не е свързан със SCS Software или NVIDIA.
