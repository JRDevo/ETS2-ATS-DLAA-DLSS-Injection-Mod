<!-- Short README in Traditional Chinese. The full guide (English): ../../README.md -->
> [!CAUTION]
> **請只從 https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases 下載這個模組。** 沒有網站、沒有 Discord、
> 沒有 Nexus，也沒有 Steam 工作坊。任何其他提供它的網站、鏡像或「安裝程式」都不是我的: 請不要執行。

![ETS2 & ATS DLAA & DLSS Mod](../images/banner.jpg)

# ETS2 & ATS: DLAA & DLSS Injection Mod

為 **Euro Truck Simulator 2** 和 **American Truck Simulator** 提供 NVIDIA **DLAA** 和 **DLSS**，非VR和VR都能用。
模組用 DLAA 取代遊戲內建的抗鋸齒: 圍欄、電線、道路標線和陰影不再閃爍。
DLSS 還可以放大畫面，換取更高的幀率。

它只是遊戲資料夾裡的一個小檔案 `dinput8.dll`。不需要啟動器，也不需要帳號。

- **穩定、清晰的畫面:** 圍欄、電線和陰影不再閃爍。後照鏡也有抗鋸齒。
- **非VR和VR:** 一個檔案兩邊通用。三螢幕和 Windows HDR 也能用。
- **VR 加速:** 注視點算繪 (支援眼球追蹤)、只在每隻眼的中央用 DLAA、幀率上限。
- **你的語言的遊戲內選單:** 按 `Delete`。每個設定、它的按鍵和它的負擔，都附有簡短說明。
- **一鍵錯誤回報:** 按兩次 `Insert`。模組會把畫面、系統資訊和記錄存進 zip。

## 你需要什麼

- Euro Truck Simulator 2 或 American Truck Simulator (64 位元，DirectX 11)。
- 裝有最新驅動程式的 **NVIDIA RTX** 顯示卡。DLAA 和 DLSS 只能在 RTX 顯示卡上執行。
- 檔案 `nvngx_dlss.dll` (NVIDIA DLSS 執行階段)。模組不包含它: 見第 6 步。
- VR: 需要 OpenXR 執行階段 (例如 Virtual Desktop)，而且遊戲要用 `-openxr` 選項啟動。

## 安裝

1. **備份**遊戲的 `bin\win_x64\` 資料夾。
2. [下載最新版本](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases/latest): 在 **Assets** 下面的 `.zip`。解壓縮它。
3. 開啟遊戲資料夾，例如 `...\steamapps\common\Euro Truck Simulator 2\bin\win_x64\`
   (或 `...\American Truck Simulator\bin\win_x64\`)。
4. **先移除其他代理模組:** 刪除該資料夾裡其他模組的 `dinput8.dll` 或 `dxgi.dll`。
5. 把 **`dinput8.dll`** 複製到 `bin\win_x64\`。也可以把 **`dlaa.ini`** 複製進去，用來變更設定和按鍵 (不是必須的)。
6. 把 **`nvngx_dlss.dll`** 放進同一個資料夾。可從 NVIDIA DLSS SDK (<https://github.com/NVIDIA/DLSS>，
   檔案 `lib/Windows_x86_64/rel/nvngx_dlss.dll`) 取得，或從任何帶有它的遊戲裡複製。
7. 啟動遊戲。DLAA 已開啟。

解除安裝: 從 `bin\win_x64\` 刪除 `dinput8.dll` (和 `dlaa.ini`)。

**Linux / Proton:** 用同樣的方式安裝。在 Steam 裡，把遊戲的啟動選項設為 `WINEDLLOVERRIDES="dinput8=n,b" %command%`。

## 最重要的按鍵

| 按鍵 | 作用 |
|---|---|
| `Delete` | 開啟和關閉選單。`Up` / `Down` 選擇一列，`Left` / `Right` 變更它。 |
| `End` | 切換模式: 關、DLAA、DLSS。 |
| 按兩次 `Insert` | 錯誤回報: 把畫面、系統資訊和記錄存進 `dlaa_selftest\`。 |

## 更多

- **完整指南 (英文):** [README.md](../../README.md): 所有設定、按鍵、VR、效能和疑難排解。
- **遇到問題:** [提交 issue](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new/choose)。請用英文撰寫。
- **翻譯**由 AI 完成，可能有錯誤。
  [回報翻譯問題](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new?template=translation.yml)。
- 這是一個非官方的粉絲專案，與 SCS Software 和 NVIDIA 沒有關聯。
