<!-- Short README in Korean. The full guide (English): ../../README.md -->
> [!CAUTION]
> **이 모드는 https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases 에서만 내려받으세요.** 웹사이트, Discord,
> Nexus, Steam 창작마당은 없습니다. 이 모드를 제공한다는 다른 사이트, 미러, "설치 프로그램"은 제가 만든 것이 아닙니다: 실행하지 마세요.

![ETS2 & ATS DLAA & DLSS Mod](../images/banner.jpg)

# ETS2 & ATS: DLAA & DLSS Injection Mod

**Euro Truck Simulator 2**와 **American Truck Simulator**용 NVIDIA **DLAA**와 **DLSS**. 일반 화면과 VR 모두 지원합니다.
이 모드는 게임 자체의 안티앨리어싱을 DLAA로 바꿉니다: 울타리, 전선, 도로 표시, 그림자가 더 이상 일렁이지 않습니다.
DLSS로 화면을 업스케일해서 프레임 속도를 높일 수도 있습니다.

게임 폴더에 넣는 작은 파일 하나, `dinput8.dll`이 전부입니다. 런처도 계정도 필요 없습니다.

- **안정적이고 또렷한 화면:** 울타리, 전선, 그림자의 일렁임이 없습니다. 거울에도 안티앨리어싱이 적용됩니다.
- **일반 화면과 VR:** 파일 하나로 둘 다 됩니다. 트리플 모니터와 Windows HDR도 작동합니다.
- **VR 가속:** Foveated 렌더링 (시선 추적 지원), 각 눈의 가운데에만 DLAA, 프레임 속도 제한.
- **내 언어로 된 게임 내 메뉴:** `Delete`를 누르세요. 모든 설정, 단축키, 부하를 짧은 설명과 함께 볼 수 있습니다.
- **한 번에 만드는 버그 리포트:** `Insert`를 두 번 누르세요. 모드가 화면, 시스템 정보, 로그를 zip으로 저장합니다.

## 필요한 것

- Euro Truck Simulator 2 또는 American Truck Simulator (64비트, DirectX 11).
- 최신 드라이버가 설치된 **NVIDIA RTX** 그래픽 카드. DLAA와 DLSS는 RTX 카드에서만 작동합니다.
- `nvngx_dlss.dll` 파일 (NVIDIA DLSS 런타임). 모드에는 들어 있지 않습니다: 6단계를 보세요.
- VR의 경우: OpenXR 런타임 (예: Virtual Desktop)과 `-openxr` 옵션으로 시작한 게임.

## 설치

1. 게임의 `bin\win_x64\` 폴더를 **백업하세요**.
2. [최신 릴리스를 내려받으세요](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases/latest): **Assets** 아래의 `.zip`입니다. 압축을 푸세요.
3. 게임 폴더를 엽니다. 예: `...\steamapps\common\Euro Truck Simulator 2\bin\win_x64\`
   (또는 `...\American Truck Simulator\bin\win_x64\`).
4. **다른 프록시 모드를 먼저 지우세요:** 그 폴더에 있는 다른 모드의 `dinput8.dll`이나 `dxgi.dll`을 삭제합니다.
5. **`dinput8.dll`**을 `bin\win_x64\`에 복사하세요. 설정과 키를 바꾸려면 **`dlaa.ini`**도 복사할 수 있습니다 (필수는 아닙니다).
6. **`nvngx_dlss.dll`**을 같은 폴더에 넣으세요. NVIDIA DLSS SDK (<https://github.com/NVIDIA/DLSS>,
   파일 `lib/Windows_x86_64/rel/nvngx_dlss.dll`)에서 받거나, 이 파일이 있는 아무 게임에서 복사하세요.
7. 게임을 시작하세요. DLAA가 켜져 있습니다.

제거하려면 `bin\win_x64\`에서 `dinput8.dll` (과 `dlaa.ini`)을 삭제하세요.

**Linux / Proton:** 같은 방법으로 설치하세요. Steam에서 게임의 실행 옵션을 `WINEDLLOVERRIDES="dinput8=n,b" %command%`로 설정합니다.

## 가장 중요한 키

| 키 | 동작 |
|---|---|
| `Delete` | 메뉴를 열고 닫습니다. `Up` / `Down`으로 줄을 고르고 `Left` / `Right`로 바꿉니다. |
| `End` | 모드를 바꿉니다: 끔, DLAA, DLSS. |
| `Insert` 두 번 | 버그 리포트: 화면, 시스템 정보, 로그를 `dlaa_selftest\`에 저장합니다. |

## 더 보기

- **전체 가이드 (영어):** [README.md](../../README.md): 모든 설정, 키, VR, 성능, 문제 해결.
- **문제가 있으면:** [이슈를 만들어](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new/choose) 주세요. 영어로 써 주세요.
- **번역**은 AI가 만들었으며 실수가 있을 수 있습니다.
  [번역 문제 신고](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new?template=translation.yml).
- 이것은 비공식 팬 프로젝트입니다. SCS Software나 NVIDIA와 관련이 없습니다.
