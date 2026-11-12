<!-- Short README in Romanian. The full guide (English): ../../README.md -->
> [!CAUTION]
> **Descarcă acest mod doar de la https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases.** Nu există site web, Discord,
> Nexus și nici Steam Workshop. Orice alt site, mirror sau „installer” care îl oferă nu este al meu: nu-l rula.

![ETS2 & ATS DLAA & DLSS Mod](../images/banner.jpg)

# ETS2 & ATS: DLAA & DLSS Injection Mod

**DLAA** și **DLSS** de la NVIDIA pentru **Euro Truck Simulator 2** și **American Truck Simulator**, pe ecran plat și în VR.
Modul înlocuiește anti-aliasing-ul propriu al jocului cu DLAA: gardurile, firele, marcajele rutiere și umbrele nu mai sclipesc.
DLSS poate, de asemenea, să mărească imaginea pentru mai multe cadre pe secundă.

Este un singur fișier mic, `dinput8.dll`, în folderul jocului. Fără launcher, fără cont.

- **O imagine stabilă și clară:** fără sclipiri pe garduri, fire și umbre. Și oglinzile primesc anti-aliasing.
- **Ecran plat și VR:** un singur fișier pentru ambele. Ecranele triple și Windows HDR funcționează.
- **Accelerări VR:** foveated rendering (și cu eye tracking), DLAA doar în centrul fiecărui ochi, limite de rată de cadre.
- **Un meniu în joc în limba ta:** apasă `Delete`. Fiecare setare, tasta ei și costul ei, cu o scurtă descriere.
- **Un raport de erori dintr-o apăsare:** apasă `Insert` de două ori. Modul salvează imagini, informații despre sistem și jurnale în arhive zip.

## Ce ai nevoie

- Euro Truck Simulator 2 sau American Truck Simulator (pe 64 de biți, DirectX 11).
- O placă video **NVIDIA RTX** cu un driver actual. DLAA și DLSS rulează doar pe plăci RTX.
- Fișierul `nvngx_dlss.dll` (runtime-ul NVIDIA DLSS). Modul nu îl include: vezi pasul 6.
- Pentru VR: un runtime OpenXR (de exemplu Virtual Desktop) și jocul pornit cu opțiunea `-openxr`.

## Instalare

1. **Fă o copie de siguranță** a folderului `bin\win_x64\` al jocului.
2. [Descarcă ultima versiune](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases/latest): fișierul `.zip` de sub **Assets**. Dezarhivează-l.
3. Deschide folderul jocului, de exemplu `...\steamapps\common\Euro Truck Simulator 2\bin\win_x64\`
   (sau `...\American Truck Simulator\bin\win_x64\`).
4. **Elimină mai întâi alte moduri proxy:** șterge orice alt `dinput8.dll` sau `dxgi.dll` al unui mod din acel folder.
5. Copiază **`dinput8.dll`** în `bin\win_x64\`. Poți copia acolo și **`dlaa.ini`**, pentru a schimba setările și tastele (nu este obligatoriu).
6. Pune **`nvngx_dlss.dll`** în același folder. Ia-l din NVIDIA DLSS SDK (<https://github.com/NVIDIA/DLSS>,
   fișierul `lib/Windows_x86_64/rel/nvngx_dlss.dll`) sau copiază-l din orice joc care îl are.
7. Pornește jocul. DLAA este pornit.

Pentru dezinstalare: șterge `dinput8.dll` (și `dlaa.ini`) din `bin\win_x64\`.

**Linux / Proton:** instalează la fel. În Steam, setează opțiunile de lansare ale jocului la `WINEDLLOVERRIDES="dinput8=n,b" %command%`.

## Cele mai importante taste

| Tasta | Ce face |
|---|---|
| `Delete` | Deschide și închide meniul. `Up` / `Down` aleg un rând, `Left` / `Right` îl schimbă. |
| `End` | Schimbă modul: oprit, DLAA, DLSS. |
| `Insert` de două ori | Raport de erori: salvează imagini, informații despre sistem și jurnale în `dlaa_selftest\`. |

## Mai multe

- **Ghidul complet (în engleză):** [README.md](../../README.md): toate setările, tastele, VR, performanța și rezolvarea problemelor.
- **Probleme:** [deschide o problemă (issue)](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new/choose). Te rog scrie-o în engleză.
- **Traducerile** sunt făcute de AI și pot conține greșeli.
  [Raportează o problemă de traducere](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new?template=translation.yml).
- Acesta este un proiect neoficial de fan. Nu este afiliat cu SCS Software sau NVIDIA.
