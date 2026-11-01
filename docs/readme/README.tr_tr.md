<!-- Short README in Turkish. The full guide (English): ../../README.md -->
> [!CAUTION]
> **Bu modu yalnızca https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases adresinden indir.** Web sitesi, Discord,
> Nexus ve Steam Workshop yok. Onu sunan başka herhangi bir site, yansı veya "yükleyici" benim değildir: çalıştırma.

![ETS2 & ATS DLAA & DLSS Mod](../images/banner.jpg)

# ETS2 & ATS: DLAA & DLSS Injection Mod

**Euro Truck Simulator 2** ve **American Truck Simulator** için NVIDIA **DLAA** ve **DLSS**, düz ekranda ve VR'da.
Mod, oyunun kendi kenar yumuşatmasını DLAA ile değiştirir: çitler, teller, yol çizgileri ve gölgeler artık titremez.
DLSS ayrıca daha yüksek kare hızı için görüntüyü ölçekleyebilir.

Oyun klasöründe tek bir küçük dosyadır: `dinput8.dll`. Başlatıcı yok, hesap yok.

- **Kararlı, keskin bir görüntü:** çitlerde, tellerde ve gölgelerde titreme yok. Aynalar da kenar yumuşatma alır.
- **Düz ekran ve VR:** ikisi için tek dosya. Üçlü ekran ve Windows HDR çalışır.
- **VR hızlandırmaları:** foveated rendering (göz takibiyle de), DLAA yalnızca her gözün ortasında, kare hızı sınırları.
- **Kendi dilinde oyun içi menü:** `Delete` tuşuna bas. Her ayar, tuşu ve maliyeti, kısa bir açıklamayla.
- **Tek basışla hata raporu:** `Insert` tuşuna iki kez bas. Mod görüntüleri, sistem bilgisini ve günlükleri zip dosyalarına kaydeder.

## Neye ihtiyacın var

- Euro Truck Simulator 2 veya American Truck Simulator (64 bit, DirectX 11).
- Güncel sürücülü bir **NVIDIA RTX** ekran kartı. DLAA ve DLSS yalnızca RTX kartlarda çalışır.
- `nvngx_dlss.dll` dosyası (NVIDIA DLSS çalışma zamanı). Mod onu içermez: 6. adıma bak.
- VR için: bir OpenXR çalışma zamanı (örneğin Virtual Desktop) ve `-openxr` seçeneğiyle başlatılmış oyun.

## Kurulum

1. Oyunun `bin\win_x64\` klasörünün **yedeğini al**.
2. [Son sürümü indir](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases/latest): **Assets** altındaki `.zip` dosyası. Arşivden çıkar.
3. Oyun klasörünü aç, örneğin `...\steamapps\common\Euro Truck Simulator 2\bin\win_x64\`
   (veya `...\American Truck Simulator\bin\win_x64\`).
4. **Önce diğer proxy modları kaldır:** o klasördeki başka bir modun `dinput8.dll` veya `dxgi.dll` dosyasını sil.
5. **`dinput8.dll`** dosyasını `bin\win_x64\` içine kopyala. Ayarları ve tuşları değiştirmek için **`dlaa.ini`** dosyasını da oraya kopyalayabilirsin (zorunlu değil).
6. **`nvngx_dlss.dll`** dosyasını aynı klasöre koy. NVIDIA DLSS SDK'dan al (<https://github.com/NVIDIA/DLSS>,
   `lib/Windows_x86_64/rel/nvngx_dlss.dll` dosyası) veya onu içeren herhangi bir oyundan kopyala.
7. Oyunu başlat. DLAA açıktır.

Kaldırmak için: `bin\win_x64\` içinden `dinput8.dll` (ve `dlaa.ini`) dosyasını sil.

**Linux / Proton:** aynı şekilde kur. Steam'de oyunun başlatma seçeneklerine `WINEDLLOVERRIDES="dinput8=n,b" %command%` yaz.

## En önemli tuşlar

| Tuş | Ne yapar |
|---|---|
| `Delete` | Menüyü açar ve kapatır. `Up` / `Down` bir satır seçer, `Left` / `Right` değiştirir. |
| `End` | Modu değiştirir: kapalı, DLAA, DLSS. |
| `Insert` iki kez | Hata raporu: görüntüleri, sistem bilgisini ve günlükleri `dlaa_selftest\` içine kaydeder. |

## Daha fazlası

- **Tam kılavuz (İngilizce):** [README.md](../../README.md): tüm ayarlar, tuşlar, VR, performans ve sorun giderme.
- **Sorunlar:** [bir issue aç](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new/choose). Lütfen İngilizce yaz.
- **Çeviriler** yapay zekâ tarafından yapılır ve hata içerebilir.
  [Bir çeviri sorununu bildir](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new?template=translation.yml).
- Bu resmî olmayan bir hayran projesidir. SCS Software veya NVIDIA ile bağlantılı değildir.
