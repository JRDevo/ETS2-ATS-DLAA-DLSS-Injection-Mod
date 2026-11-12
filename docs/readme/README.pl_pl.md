<!-- Short README in Polish. The full guide (English): ../../README.md -->
> [!CAUTION]
> **Pobieraj ten mod tylko z https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases.** Nie ma strony internetowej, Discorda,
> Nexusa ani Steam Workshop. Każda inna strona, mirror lub „instalator”, który go oferuje, nie jest mój: nie uruchamiaj go.

![ETS2 & ATS DLAA & DLSS Mod](../images/banner.jpg)

# ETS2 & ATS: DLAA & DLSS Injection Mod

NVIDIA **DLAA** i **DLSS** dla **Euro Truck Simulator 2** i **American Truck Simulator**, na zwykłym ekranie i w VR.
Mod zastępuje własne wygładzanie krawędzi gry przez DLAA: płoty, przewody, oznaczenia na drodze i cienie przestają migotać.
DLSS może też skalować obraz w górę, by uzyskać więcej klatek na sekundę.

To jeden mały plik, `dinput8.dll`, w folderze gry. Bez launchera, bez konta.

- **Stabilny, ostry obraz:** bez migotania płotów, przewodów i cieni. Lusterka też są wygładzane.
- **Zwykły ekran i VR:** jeden plik do obu. Działają potrójne ekrany i Windows HDR.
- **Przyspieszenia dla VR:** foveated rendering (także z eye trackingiem), DLAA tylko w środku każdego oka, limity liczby klatek.
- **Menu w grze w Twoim języku:** naciśnij `Delete`. Każde ustawienie, jego klawisz i koszt, z krótkim opisem.
- **Zgłoszenie błędu jednym naciśnięciem:** naciśnij `Insert` dwa razy. Mod zapisuje obrazy, informacje o systemie i logi do plików zip.

## Czego potrzebujesz

- Euro Truck Simulator 2 lub American Truck Simulator (64-bitowy, DirectX 11).
- Karta graficzna **NVIDIA RTX** z aktualnym sterownikiem. DLAA i DLSS działają tylko na kartach RTX.
- Plik `nvngx_dlss.dll` (środowisko DLSS od NVIDIA). Mod go nie zawiera: zobacz krok 6.
- Dla VR: środowisko OpenXR (na przykład Virtual Desktop) i gra uruchomiona z opcją `-openxr`.

## Instalacja

1. **Zrób kopię zapasową** folderu gry `bin\win_x64\`.
2. [Pobierz najnowsze wydanie](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases/latest): plik `.zip` w sekcji **Assets**. Rozpakuj go.
3. Otwórz folder gry, na przykład `...\steamapps\common\Euro Truck Simulator 2\bin\win_x64\`
   (lub `...\American Truck Simulator\bin\win_x64\`).
4. **Najpierw usuń inne mody proxy:** skasuj w tym folderze każdy inny `dinput8.dll` lub `dxgi.dll` pochodzący z innego moda.
5. Skopiuj **`dinput8.dll`** do `bin\win_x64\`. Możesz tam też skopiować **`dlaa.ini`**, by zmieniać ustawienia i klawisze (nie jest wymagany).
6. Wstaw **`nvngx_dlss.dll`** do tego samego folderu. Weź go z NVIDIA DLSS SDK (<https://github.com/NVIDIA/DLSS>,
   plik `lib/Windows_x86_64/rel/nvngx_dlss.dll`) lub skopiuj z dowolnej gry, która go ma.
7. Uruchom grę. DLAA jest włączone.

Aby odinstalować: usuń `dinput8.dll` (i `dlaa.ini`) z `bin\win_x64\`.

**Linux / Proton:** zainstaluj tak samo. W Steam ustaw opcje uruchamiania gry na `WINEDLLOVERRIDES="dinput8=n,b" %command%`.

## Najważniejsze klawisze

| Klawisz | Co robi |
|---|---|
| `Delete` | Otwiera i zamyka menu. `Up` / `Down` wybierają wiersz, `Left` / `Right` zmieniają go. |
| `End` | Przełącza tryb: wył., DLAA, DLSS. |
| `Insert` dwa razy | Zgłoszenie błędu: zapisuje obrazy, informacje o systemie i logi do `dlaa_selftest\`. |

## Więcej

- **Pełny przewodnik (po angielsku):** [README.md](../../README.md): wszystkie ustawienia, klawisze, VR, wydajność i rozwiązywanie problemów.
- **Problemy:** [otwórz zgłoszenie (issue)](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new/choose). Napisz je, proszę, po angielsku.
- **Tłumaczenia** są wykonane przez AI i mogą zawierać błędy.
  [Zgłoś problem z tłumaczeniem](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new?template=translation.yml).
- To nieoficjalny projekt fanowski. Nie jest powiązany z SCS Software ani NVIDIA.
