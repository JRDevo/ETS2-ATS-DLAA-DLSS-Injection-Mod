<!-- Short README in Brazilian Portuguese. The full guide (English): ../../README.md -->
> [!CAUTION]
> **Baixe este mod somente em https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases.** Não existe site, Discord,
> Nexus nem Steam Workshop. Qualquer outro site, espelho ou "instalador" que o ofereça não é meu: não execute.

![ETS2 & ATS DLAA & DLSS Mod](../images/banner.jpg)

# ETS2 & ATS: DLAA & DLSS Injection Mod

**DLAA** e **DLSS** da NVIDIA para o **Euro Truck Simulator 2** e o **American Truck Simulator**, em tela plana e em VR.
O mod troca o anti-aliasing do próprio jogo pelo DLAA: cercas, fios, faixas de pista e sombras deixam de cintilar.
O DLSS também pode fazer upscale da imagem para mais quadros por segundo.

É um arquivo pequeno, `dinput8.dll`, na pasta do jogo. Sem launcher, sem conta.

- **Uma imagem estável e nítida:** sem cintilação em cercas, fios e sombras. Os retrovisores também recebem anti-aliasing.
- **Tela plana e VR:** um arquivo para os dois. Telas triplas e Windows HDR funcionam.
- **Aceleradores de VR:** foveated rendering (também com eye tracking), DLAA só no centro de cada olho, limites de taxa de quadros.
- **Um menu no jogo no seu idioma:** aperte `Delete`. Cada ajuste, a tecla dele e o custo, com uma descrição curta.
- **Um relatório de bug com um toque:** aperte `Insert` duas vezes. O mod salva imagens, informações do sistema e logs em zips.

## O que você precisa

- Euro Truck Simulator 2 ou American Truck Simulator (64 bits, DirectX 11).
- Uma placa de vídeo **NVIDIA RTX** com um driver atual. DLAA e DLSS só rodam em placas RTX.
- O arquivo `nvngx_dlss.dll` (o runtime DLSS da NVIDIA). O mod não o inclui: veja o passo 6.
- Para VR: um runtime OpenXR (por exemplo o Virtual Desktop) e o jogo iniciado com a opção `-openxr`.

## Instalação

1. **Faça um backup** da pasta `bin\win_x64\` do jogo.
2. [Baixe a versão mais recente](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases/latest): o `.zip` em **Assets**. Descompacte.
3. Abra a pasta do jogo, por exemplo `...\steamapps\common\Euro Truck Simulator 2\bin\win_x64\`
   (ou `...\American Truck Simulator\bin\win_x64\`).
4. **Remova antes outros mods proxy:** apague qualquer outro `dinput8.dll` ou `dxgi.dll` de mod nessa pasta.
5. Copie o **`dinput8.dll`** para `bin\win_x64\`. Você também pode copiar o **`dlaa.ini`** para lá, para mudar ajustes e teclas (não é obrigatório).
6. Coloque o **`nvngx_dlss.dll`** na mesma pasta. Pegue-o no NVIDIA DLSS SDK (<https://github.com/NVIDIA/DLSS>,
   arquivo `lib/Windows_x86_64/rel/nvngx_dlss.dll`) ou copie de qualquer jogo que o tenha.
7. Inicie o jogo. O DLAA está ligado.

Para desinstalar: apague o `dinput8.dll` (e o `dlaa.ini`) de `bin\win_x64\`.

**Linux / Proton:** instale do mesmo jeito. No Steam, coloque nas opções de inicialização do jogo `WINEDLLOVERRIDES="dinput8=n,b" %command%`.

## As teclas mais importantes

| Tecla | O que faz |
|---|---|
| `Delete` | Abre e fecha o menu. `Up` / `Down` escolhem uma linha, `Left` / `Right` mudam o valor. |
| `End` | Troca o modo: desligado, DLAA, DLSS. |
| `Insert` duas vezes | Relatório de bug: salva imagens, informações do sistema e logs em `dlaa_selftest\`. |

## Mais

- **O guia completo (inglês):** [README.md](../../README.md): todos os ajustes, teclas, VR, desempenho e solução de problemas.
- **Problemas:** [abra uma issue](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new/choose). Por favor, escreva em inglês.
- **As traduções** são feitas por IA e podem conter erros.
  [Avise sobre um problema de tradução](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new?template=translation.yml).
- Este é um projeto de fã não oficial. Não é afiliado à SCS Software nem à NVIDIA.
