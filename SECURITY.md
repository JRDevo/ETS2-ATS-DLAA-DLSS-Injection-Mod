# Security

## What this mod is

The mod is a single `dinput8.dll` that the game loads from its `bin\win_x64\` folder. It hooks the game's DirectX 11 and
OpenXR calls. It does not go online, has no account and collects nothing. It only writes files next to the game:
`dlaa.ini`, `dlaa_inject.log`, and test captures when you press a capture key.

## Get it from one place only

Download the mod only from the [Releases page](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases).
A `dinput8.dll` from anywhere else can be anything.

The DLL is not code-signed. Some antivirus programs flag DLLs that hook a game. If yours does, check that the file came
from the Releases page. You can also build it yourself from this source (see
[Build from source](README.md#build-from-source)).

The mod never ships NVIDIA's `nvngx_dlss.dll`. Get that file from a trusted source (for example NVIDIA or your own
games).

## Supported versions

Only the newest release and the newest pre-release get fixes.

## Report a security problem

Do **not** put the details of a security problem in a public issue.

Open an [issue](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new/choose) with the title
**Security contact** and no details. I answer there and we move to a private channel.

For normal bugs, crashes and performance problems, open a normal
[issue](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new/choose).
