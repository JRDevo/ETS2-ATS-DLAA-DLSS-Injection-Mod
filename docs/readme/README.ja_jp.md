<!-- Short README in Japanese. The full guide (English): ../../README.md -->
> [!CAUTION]
> **このMODは https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases からのみダウンロードしてください。** ウェブサイト、Discord、
> Nexus、Steam Workshopはありません。これを配布するほかのサイト、ミラー、「インストーラー」は私のものではありません。実行しないでください。

![ETS2 & ATS DLAA & DLSS Mod](../images/banner.jpg)

# ETS2 & ATS: DLAA & DLSS Injection Mod

**Euro Truck Simulator 2** と **American Truck Simulator** 用のNVIDIA **DLAA** と **DLSS**。通常の画面でもVRでも使えます。
このMODはゲーム標準のアンチエイリアスをDLAAに置き換えます。フェンス、電線、道路のペイント、影がちらつかなくなります。
DLSSで画像をアップスケールして、フレームレートを上げることもできます。

ゲームフォルダに置く小さなファイル `dinput8.dll` 1つだけです。ランチャーもアカウントも不要です。

- **安定してくっきりした画面:** フェンス、電線、影のちらつきがなくなります。ミラーにもアンチエイリアスがかかります。
- **非VRもVRも:** 1つのファイルで両方に対応。トリプルスクリーンとWindows HDRも動きます。
- **VRの高速化:** Foveated レンダリング (アイトラッキング対応)、各目の中央だけDLAA、フレームレート上限。
- **自分の言語のゲーム内メニュー:** `Delete` を押します。すべての設定、そのキー、負荷を、短い説明つきで表示。
- **ワンタッチのバグレポート:** `Insert` を2回押します。MODが画像、システム情報、ログをzipに保存します。

## 必要なもの

- Euro Truck Simulator 2 または American Truck Simulator (64ビット、DirectX 11)。
- 最新のドライバーを入れた **NVIDIA RTX** グラフィックスカード。DLAAとDLSSはRTXカードでのみ動きます。
- `nvngx_dlss.dll` ファイル (NVIDIA DLSSランタイム)。MODには含まれていません: 手順6を見てください。
- VRの場合: OpenXRランタイム (例: Virtual Desktop) と、`-openxr` オプションで起動したゲーム。

## インストール

1. ゲームの `bin\win_x64\` フォルダの**バックアップを作ります**。
2. [最新リリースをダウンロード](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases/latest)します: **Assets** の下の `.zip` です。解凍してください。
3. ゲームフォルダを開きます。例: `...\steamapps\common\Euro Truck Simulator 2\bin\win_x64\`
   (または `...\American Truck Simulator\bin\win_x64\`)。
4. **先にほかのプロキシMODを削除します:** そのフォルダにある、ほかのMODの `dinput8.dll` や `dxgi.dll` を削除してください。
5. **`dinput8.dll`** を `bin\win_x64\` にコピーします。設定やキーを変えたいときは **`dlaa.ini`** もコピーします (必須ではありません)。
6. **`nvngx_dlss.dll`** を同じフォルダに置きます。NVIDIA DLSS SDK (<https://github.com/NVIDIA/DLSS>、
   ファイル `lib/Windows_x86_64/rel/nvngx_dlss.dll`) から入手するか、これを持つ任意のゲームからコピーしてください。
7. ゲームを起動します。DLAAがオンになっています。

アンインストールするには、`bin\win_x64\` から `dinput8.dll` (と `dlaa.ini`) を削除します。

**Linux / Proton:** 同じ方法でインストールします。Steamで、ゲームの起動オプションに `WINEDLLOVERRIDES="dinput8=n,b" %command%` を設定してください。

## いちばん大事なキー

| キー | 動作 |
|---|---|
| `Delete` | メニューを開閉します。`Up` / `Down` で行を選び、`Left` / `Right` で変更します。 |
| `End` | モードを切り替えます: オフ、DLAA、DLSS。 |
| `Insert` 2回 | バグレポート: 画像、システム情報、ログを `dlaa_selftest\` に保存します。 |

## その他

- **完全ガイド (英語):** [README.md](../../README.md): すべての設定、キー、VR、パフォーマンス、トラブルシューティング。
- **問題があるとき:** [issueを作成](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new/choose)してください。英語で書いてください。
- **翻訳** はAIが作成しており、間違いを含む場合があります。
  [翻訳の問題を報告](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new?template=translation.yml)。
- これは非公式のファンプロジェクトです。SCS SoftwareやNVIDIAとは関係ありません。
