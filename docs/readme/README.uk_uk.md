<!-- Short README in Ukrainian. The full guide (English): ../../README.md -->
> [!CAUTION]
> **Завантажуйте цей мод лише з https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases.** Сайту, Discord,
> Nexus і Steam Workshop у мода немає. Будь-який інший сайт, дзеркало чи «інсталятор», що пропонує його, не мій: не запускайте їх.

![ETS2 & ATS DLAA & DLSS Mod](../images/banner.jpg)

# ETS2 & ATS: мод DLAA & DLSS Injection

NVIDIA **DLAA** і **DLSS** для **Euro Truck Simulator 2** та **American Truck Simulator**, на звичайному екрані та у VR.
Мод замінює власне згладжування гри на DLAA: паркани, дроти, дорожня розмітка й тіні більше не мерехтять.
DLSS також може збільшувати картинку для вищої частоти кадрів.

Це один невеликий файл, `dinput8.dll`, у теці гри. Без лаунчера, без акаунта.

- **Рівна й чітка картинка:** без мерехтіння на парканах, дротах і тінях. Дзеркала теж отримують згладжування.
- **Плоский режим і VR:** один файл для обох. Працюють потрійний екран і Windows HDR.
- **Прискорення VR:** foveated-рендеринг (зокрема за поглядом), DLAA лише в центрі кожного ока, ліміти частоти кадрів.
- **Ігрове меню вашою мовою:** натисніть `Delete`. Кожне налаштування, його клавіша й ціна, з коротким описом.
- **Звіт про помилку однією клавішею:** натисніть `Insert` двічі. Мод збереже картинки, інформацію про систему та журнали в zip-файли.

## Що потрібно

- Euro Truck Simulator 2 або American Truck Simulator (64-біт, DirectX 11).
- Відеокарта **NVIDIA RTX** з актуальним драйвером. DLAA і DLSS працюють лише на картах RTX.
- Файл `nvngx_dlss.dll` (середовище виконання NVIDIA DLSS). У мод він не входить: див. крок 6.
- Для VR: середовище виконання OpenXR (наприклад, Virtual Desktop) і гра, запущена з параметром `-openxr`.

## Встановлення

1. **Зробіть резервну копію** теки гри `bin\win_x64\`.
2. [Завантажте останній реліз](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases/latest): файл `.zip` у розділі **Assets**. Розпакуйте його.
3. Відкрийте теку гри, наприклад `...\steamapps\common\Euro Truck Simulator 2\bin\win_x64\`
   (або `...\American Truck Simulator\bin\win_x64\`).
4. **Спочатку видаліть інші проксі-моди:** видаліть у цій теці чужі `dinput8.dll` або `dxgi.dll`.
5. Скопіюйте **`dinput8.dll`** в `bin\win_x64\`. Туди ж можна скопіювати **`dlaa.ini`**, щоб змінювати налаштування й клавіші (необов'язково).
6. Покладіть **`nvngx_dlss.dll`** в ту саму теку. Візьміть його з NVIDIA DLSS SDK (<https://github.com/NVIDIA/DLSS>,
   файл `lib/Windows_x86_64/rel/nvngx_dlss.dll`) або скопіюйте з будь-якої гри, де він є.
7. Запустіть гру. DLAA увімкнено.

Щоб видалити мод: видаліть `dinput8.dll` (і `dlaa.ini`) з `bin\win_x64\`.

**Linux / Proton:** встановлюйте так само. У Steam задайте в параметрах запуску гри `WINEDLLOVERRIDES="dinput8=n,b" %command%`.

## Найважливіші клавіші

| Клавіша | Що робить |
|---|---|
| `Delete` | Відкриває й закриває меню. `Up` / `Down` вибирають рядок, `Left` / `Right` змінюють його. |
| `End` | Перемикає режим: вимк., DLAA, DLSS. |
| `Insert` двічі | Звіт про помилку: зберігає картинки, інформацію про систему та журнали в `dlaa_selftest\`. |

## Ще

- **Повний посібник (англійською):** [README.md](../../README.md): усі налаштування, клавіші, VR, продуктивність і розв'язання проблем.
- **Проблеми:** [створіть issue](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new/choose). Будь ласка, пишіть англійською.
- **Переклади** зроблені ШІ й можуть містити помилки.
  [Повідомити про помилку перекладу](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new?template=translation.yml).
- Це неофіційний фанатський проєкт. Він не пов'язаний із SCS Software чи NVIDIA.
