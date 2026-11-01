<!-- Short README in Russian. The full guide (English): ../../README.md -->
> [!CAUTION]
> **Скачивайте этот мод только с https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases.** Сайта, Discord,
> Nexus и Steam Workshop у мода нет. Любой другой сайт, зеркало или «установщик», предлагающие его, не мои: не запускайте их.

![ETS2 & ATS DLAA & DLSS Mod](../images/banner.jpg)

# ETS2 & ATS: мод DLAA & DLSS Injection

NVIDIA **DLAA** и **DLSS** для **Euro Truck Simulator 2** и **American Truck Simulator**, на обычном экране и в VR.
Мод заменяет собственное сглаживание игры на DLAA: заборы, провода, дорожная разметка и тени больше не мерцают.
DLSS также может увеличивать картинку для более высокой частоты кадров.

Это один небольшой файл, `dinput8.dll`, в папке игры. Без лаунчера, без аккаунта.

- **Ровная и чёткая картинка:** без мерцания на заборах, проводах и тенях. Зеркала тоже получают сглаживание.
- **Плоский режим и VR:** один файл для обоих. Работают тройной экран и Windows HDR.
- **Ускорение VR:** foveated-рендеринг (в том числе по взгляду), DLAA только в центре каждого глаза, лимиты частоты кадров.
- **Игровое меню на вашем языке:** нажмите `Delete`. Каждая настройка, её клавиша и цена, с коротким описанием.
- **Отчёт об ошибке одной клавишей:** нажмите `Insert` дважды. Мод сохранит картинки, информацию о системе и логи в zip-файлы.

## Что нужно

- Euro Truck Simulator 2 или American Truck Simulator (64-бит, DirectX 11).
- Видеокарта **NVIDIA RTX** с актуальным драйвером. DLAA и DLSS работают только на картах RTX.
- Файл `nvngx_dlss.dll` (среда выполнения NVIDIA DLSS). В мод он не входит: см. шаг 6.
- Для VR: среда выполнения OpenXR (например, Virtual Desktop) и игра, запущенная с параметром `-openxr`.

## Установка

1. **Сделайте резервную копию** папки игры `bin\win_x64\`.
2. [Скачайте последний релиз](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases/latest): файл `.zip` в разделе **Assets**. Распакуйте его.
3. Откройте папку игры, например `...\steamapps\common\Euro Truck Simulator 2\bin\win_x64\`
   (или `...\American Truck Simulator\bin\win_x64\`).
4. **Сначала удалите другие прокси-моды:** удалите в этой папке чужие `dinput8.dll` или `dxgi.dll`.
5. Скопируйте **`dinput8.dll`** в `bin\win_x64\`. Туда же можно скопировать **`dlaa.ini`**, чтобы менять настройки и клавиши (необязательно).
6. Положите **`nvngx_dlss.dll`** в ту же папку. Возьмите его из NVIDIA DLSS SDK (<https://github.com/NVIDIA/DLSS>,
   файл `lib/Windows_x86_64/rel/nvngx_dlss.dll`) или скопируйте из любой игры, где он есть.
7. Запустите игру. DLAA включён.

Чтобы удалить мод: удалите `dinput8.dll` (и `dlaa.ini`) из `bin\win_x64\`.

**Linux / Proton:** устанавливайте так же. В Steam задайте в параметрах запуска игры `WINEDLLOVERRIDES="dinput8=n,b" %command%`.

## Самые важные клавиши

| Клавиша | Что делает |
|---|---|
| `Delete` | Открывает и закрывает меню. `Up` / `Down` выбирают строку, `Left` / `Right` меняют её. |
| `End` | Переключает режим: выкл, DLAA, DLSS. |
| `Insert` дважды | Отчёт об ошибке: сохраняет картинки, информацию о системе и логи в `dlaa_selftest\`. |

## Ещё

- **Полное руководство (на английском):** [README.md](../../README.md): все настройки, клавиши, VR, производительность и решение проблем.
- **Проблемы:** [создайте issue](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new/choose). Пожалуйста, пишите по-английски.
- **Переводы** сделаны ИИ и могут содержать ошибки.
  [Сообщить об ошибке перевода](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new?template=translation.yml).
- Это неофициальный фанатский проект. Он не связан с SCS Software или NVIDIA.
