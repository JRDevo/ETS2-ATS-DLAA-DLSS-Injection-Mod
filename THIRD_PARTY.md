# Third-party components

This project's own source is under the PolyForm Noncommercial License 1.0.0
(see `LICENSE.md`) — free for noncommercial use only. The binary
`dinput8.dll` it produces, and the build, link against the two third-party
components below, and the sharpening pass follows a published AMD algorithm (last
section). Their own license terms govern those parts.

Neither component is committed to this repository: MinHook is fetched by CMake at
configure time, and the NVIDIA DLSS SDK must be cloned into `external/DLSS`
yourself (both are `.gitignore`d).

---

## MinHook

- **What it is / how used:** minimalistic x64 API-hooking library. Four of its C
  sources are compiled into a small static library and linked into `dinput8.dll`
  (it installs the trampoline hook on DXGI `Present` and the device-context calls).
- **Source:** <https://github.com/TsudaKageyu/minhook> (tag `v1.3.4`), fetched by
  CMake `FetchContent` — see `CMakeLists.txt`. The full text ships in the fetched
  tree at `build/_deps/minhook-src/LICENSE.txt`.
- **License:** BSD 2-Clause. It also contains the Hacker Disassembler Engine
  (HDE), Copyright (c) 2008-2009 Vyacheslav Patkov, under the same BSD 2-Clause
  terms.

```
MinHook - The Minimalistic API Hooking Library for x64/x86
Copyright (C) 2009-2017 Tsuda Kageyu.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

 1. Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
 2. Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER
OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

Portions of this software are Copyright (c) 2008-2009, Vyacheslav Patkov.
```

Because the BSD 2-Clause license requires binary redistributions to reproduce the
copyright notice "in the documentation and/or other materials provided with the
distribution", this file is included in the release zip and satisfies that term.

---

## NVIDIA DLSS / NGX SDK

- **What it is / how used:** the DLAA / DLSS super-resolution runtime. The SDK's
  headers are compiled against, and its **static** import library
  (`nvsdk_ngx_d.lib`, from `external/DLSS/lib/Windows_x86_64/x64`) is linked into
  `dinput8.dll`. At runtime the DLL calls into NVIDIA's `nvngx_dlss.dll`.
- **Source:** <https://github.com/NVIDIA/DLSS> — clone into `external/DLSS`
  yourself (see the README build section). Full terms: `external/DLSS/LICENSE.txt`
  ("NVIDIA RTX SDKs LICENSE", v. March 14, 2024) and
  `external/DLSS/"DLSS Donut and Sample Source_Pre-Release License (2July2019).pdf"`.
- **License:** proprietary NVIDIA RTX SDK license — **not** open source, **not**
  redistributable as a stand-alone product. Read `external/DLSS/LICENSE.txt` before
  distributing a build. The requirements that bear on this project:

  - Attribution notice (clause 2.b): *"The following notice shall be included in
    modifications and derivative works of source code distributed: 'This software
    contains source code provided by NVIDIA Corporation.'"* — so that notice is
    reproduced below.
  - Material additional functionality (clause 2.a): the application must add
    material functionality beyond the included SDK portions (this injector does).
  - At-least-as-protective terms (clause 2.c): the SDK may only be distributed under
    terms at least as protective as NVIDIA's license.
  - No stand-alone distribution (clause 4.b): you may not distribute the SDK by
    itself; only incorporated in object-code form into an application.
  - Commercial-release notification (supplement clause 4): NVIDIA must be notified
    prior to the commercial release of an application incorporating the SDK, at
    <https://developer.nvidia.com/sw-notification>.

> This software contains source code provided by NVIDIA Corporation.

### What this project does NOT redistribute

This project ships **no** NVIDIA binaries. `nvngx_dlss.dll` (and any Streamline
`sl.*.dll`) are **not** included in the repository or in any release zip — the
`.gitignore` excludes them and `scripts/package.ps1` fails the build if one would
end up in the zip. The user supplies `nvngx_dlss.dll` themselves (from the NVIDIA
DLSS SDK, or any game that already ships it) and places it next to `dinput8.dll`.
The only NVIDIA code that leaves this project is the statically linked
`nvsdk_ngx_d` import stub compiled into `dinput8.dll`.

---

## AMD FidelityFX Super Resolution 1.0 — RCAS

- **What it is / how used:** the optional sharpening pass after DLAA / DLSS
  (`kRcasShader` in `src/scene_dlaa.cpp`) is this project's own compute-shader
  implementation of the RCAS (robust contrast-adaptive sharpening) method that AMD
  published with FidelityFX Super Resolution 1.0. No AMD file is included; the
  shader was written for this project and changed (variable tap radius, an
  anti-ringing clamp, an area border blend).
- **Source of the method:** <https://github.com/GPUOpen-Effects/FidelityFX-FSR>
  (`ffx_fsr1.h`).
- **License:** MIT. The notice is reproduced here because the shader follows AMD's
  method closely.

```
Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```
