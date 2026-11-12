<!-- Short README in Simplified Chinese. The full guide (English): ../../README.md -->
> [!CAUTION]
> **请只从 https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases 下载这个模组。** 没有网站、没有 Discord、
> 没有 Nexus，也没有 Steam 创意工坊。任何其他提供它的网站、镜像或“安装程序”都不是我的: 请不要运行。

![ETS2 & ATS DLAA & DLSS Mod](../images/banner.jpg)

# ETS2 & ATS: DLAA & DLSS Injection Mod

为 **Euro Truck Simulator 2** 和 **American Truck Simulator** 提供 NVIDIA **DLAA** 和 **DLSS**，非VR和VR都能用。
模组用 DLAA 取代游戏自带的抗锯齿: 围栏、电线、道路标线和阴影不再闪烁。
DLSS 还可以放大画面，换取更高的帧率。

它只是游戏文件夹里的一个小文件 `dinput8.dll`。不需要启动器，也不需要账号。

- **稳定、清晰的画面:** 围栏、电线和阴影不再闪烁。后视镜也有抗锯齿。
- **非VR和VR:** 一个文件两边通用。三联屏和 Windows HDR 也能用。
- **VR 加速:** 注视点渲染 (支持眼动追踪)、只在每只眼的中央用 DLAA、帧率上限。
- **你的语言的游戏内菜单:** 按 `Delete`。每个设置、它的按键和它的开销，都附有简短说明。
- **一键错误报告:** 按两次 `Insert`。模组会把画面、系统信息和日志存进 zip。

## 你需要什么

- Euro Truck Simulator 2 或 American Truck Simulator (64 位，DirectX 11)。
- 装有最新驱动的 **NVIDIA RTX** 显卡。DLAA 和 DLSS 只能在 RTX 显卡上运行。
- 文件 `nvngx_dlss.dll` (NVIDIA DLSS 运行库)。模组不包含它: 见第 6 步。
- VR: 需要 OpenXR 运行时 (例如 Virtual Desktop)，并且游戏要用 `-openxr` 选项启动。

## 安装

1. **备份**游戏的 `bin\win_x64\` 文件夹。
2. [下载最新版本](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases/latest): 在 **Assets** 下面的 `.zip`。解压它。
3. 打开游戏文件夹，例如 `...\steamapps\common\Euro Truck Simulator 2\bin\win_x64\`
   (或 `...\American Truck Simulator\bin\win_x64\`)。
4. **先移除其他代理模组:** 删除该文件夹里其他模组的 `dinput8.dll` 或 `dxgi.dll`。
5. 把 **`dinput8.dll`** 复制到 `bin\win_x64\`。也可以把 **`dlaa.ini`** 复制进去，用来更改设置和按键 (不是必须的)。
6. 把 **`nvngx_dlss.dll`** 放进同一个文件夹。可从 NVIDIA DLSS SDK (<https://github.com/NVIDIA/DLSS>，
   文件 `lib/Windows_x86_64/rel/nvngx_dlss.dll`) 获取，或从任何带有它的游戏里复制。
7. 启动游戏。DLAA 已开启。

卸载: 从 `bin\win_x64\` 删除 `dinput8.dll` (和 `dlaa.ini`)。

**Linux / Proton:** 用同样的方式安装。在 Steam 里，把游戏的启动选项设为 `WINEDLLOVERRIDES="dinput8=n,b" %command%`。

## 最重要的按键

| 按键 | 作用 |
|---|---|
| `Delete` | 打开和关闭菜单。`Up` / `Down` 选择一行，`Left` / `Right` 更改它。 |
| `End` | 切换模式: 关、DLAA、DLSS。 |
| 按两次 `Insert` | 错误报告: 把画面、系统信息和日志存进 `dlaa_selftest\`。 |

## 更多

- **完整指南 (英文):** [README.md](../../README.md): 所有设置、按键、VR、性能和故障排除。
- **遇到问题:** [提交 issue](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new/choose)。请用英文书写。
- **翻译**由 AI 完成，可能有错误。
  [报告翻译问题](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new?template=translation.yml)。
- 这是一个非官方的粉丝项目，与 SCS Software 和 NVIDIA 没有关联。
