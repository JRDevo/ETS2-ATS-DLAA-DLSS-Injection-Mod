<!-- Short README in Portuguese (Portugal). The full guide (English): ../../README.md -->
> [!CAUTION]
> **Descarregue este mod apenas em https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases.** Não existe site, Discord,
> Nexus nem Steam Workshop. Qualquer outro site, espelho ou "instalador" que o ofereça não é meu: não o execute.

![ETS2 & ATS DLAA & DLSS Mod](../images/banner.jpg)

# ETS2 & ATS: DLAA & DLSS Injection Mod

**DLAA** e **DLSS** da NVIDIA para o **Euro Truck Simulator 2** e o **American Truck Simulator**, em ecrã plano e em VR.
O mod substitui o anti-aliasing do próprio jogo pelo DLAA: vedações, fios, marcas da estrada e sombras deixam de cintilar.
O DLSS também pode fazer upscale da imagem para mais imagens por segundo.

É um ficheiro pequeno, `dinput8.dll`, na pasta do jogo. Sem launcher, sem conta.

- **Uma imagem estável e nítida:** sem cintilação em vedações, fios e sombras. Os retrovisores também recebem anti-aliasing.
- **Ecrã plano e VR:** um ficheiro para ambos. Ecrãs triplos e Windows HDR funcionam.
- **Aceleradores de VR:** foveated rendering (também com eye tracking), DLAA só no centro de cada olho, limites de taxa de imagens.
- **Um menu no jogo na sua língua:** prima `Delete`. Cada definição, a sua tecla e o seu custo, com uma descrição curta.
- **Um relatório de erros com um toque:** prima `Insert` duas vezes. O mod guarda imagens, informações do sistema e registos em zips.

## O que precisa

- Euro Truck Simulator 2 ou American Truck Simulator (64 bits, DirectX 11).
- Uma placa gráfica **NVIDIA RTX** com um controlador atual. DLAA e DLSS só funcionam em placas RTX.
- O ficheiro `nvngx_dlss.dll` (o runtime DLSS da NVIDIA). O mod não o inclui: veja o passo 6.
- Para VR: um runtime OpenXR (por exemplo o Virtual Desktop) e o jogo iniciado com a opção `-openxr`.

## Instalação

1. **Faça uma cópia de segurança** da pasta `bin\win_x64\` do jogo.
2. [Descarregue a versão mais recente](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases/latest): o `.zip` em **Assets**. Descompacte-o.
3. Abra a pasta do jogo, por exemplo `...\steamapps\common\Euro Truck Simulator 2\bin\win_x64\`
   (ou `...\American Truck Simulator\bin\win_x64\`).
4. **Remova antes outros mods proxy:** apague qualquer outro `dinput8.dll` ou `dxgi.dll` de mod nessa pasta.
5. Copie o **`dinput8.dll`** para `bin\win_x64\`. Também pode copiar o **`dlaa.ini`** para lá, para mudar definições e teclas (não é obrigatório).
6. Coloque o **`nvngx_dlss.dll`** na mesma pasta. Obtenha-o no NVIDIA DLSS SDK (<https://github.com/NVIDIA/DLSS>,
   ficheiro `lib/Windows_x86_64/rel/nvngx_dlss.dll`) ou copie-o de qualquer jogo que o tenha.
7. Inicie o jogo. O DLAA está ligado.

Para desinstalar: apague o `dinput8.dll` (e o `dlaa.ini`) de `bin\win_x64\`.

**Linux / Proton:** instale da mesma forma. No Steam, defina nas opções de arranque do jogo `WINEDLLOVERRIDES="dinput8=n,b" %command%`.

## As teclas mais importantes

| Tecla | O que faz |
|---|---|
| `Delete` | Abre e fecha o menu. `Up` / `Down` escolhem uma linha, `Left` / `Right` mudam-na. |
| `End` | Muda o modo: desligado, DLAA, DLSS. |
| `Insert` duas vezes | Relatório de erros: guarda imagens, informações do sistema e registos em `dlaa_selftest\`. |

## Mais

- **O guia completo (inglês):** [README.md](../../README.md): todas as definições, teclas, VR, desempenho e resolução de problemas.
- **Problemas:** [abra uma issue](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new/choose). Escreva-a em inglês, por favor.
- **As traduções** são feitas por IA e podem conter erros.
  [Comunique um problema de tradução](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new?template=translation.yml).
- Este é um projeto de fã não oficial. Não é afiliado à SCS Software nem à NVIDIA.
